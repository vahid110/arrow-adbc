# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

"""Host-only finite lifecycle. Never invoked by the live workflow or offline tests.

The coordinator creates the narrowly scoped OIDC role first. This helper creates
only the exact new dedicated database resources, issues one admission after
bootstrap, observes CI independently, and tears down in finally. Durable receipts
are nonsecret. No operation reads the managed admin secret's value.
"""

import argparse
import hashlib
import io
import json
import os
import sys
import time
import zipfile
from datetime import datetime, timezone
from pathlib import Path

from pgwire_qualification import file_sha
from pgwire_qualification_live import (
    ACCOUNT,
    KINDS,
    NAME,
    OIDC_SUBJECT,
    PARAMETER,
    REF,
    REGION,
    REPOSITORY,
    ROLE,
    SUBNETS,
    TAG,
    VPC,
    WORKFLOW,
    arn,
    identity,
    resources,
    rules,
    validate,
)
from pgwire_qualification_manifest import PROTECTED, SHA, digest, require
from pgwire_qualification_prepare import GitHub, parse_json

OWNER = f"arn:aws:iam::{ACCOUNT}:role/adbc-provisioner"
INITIAL_POLICY = "adbc-qualification-initial"
RUN_POLICY = "adbc-qualification-20261016-runtime"
ROLE_NAME = "adbc-isolated-qualification-ci"
INITIAL_POLICY_DOCUMENT = {
    "Version": "2012-10-17",
    "Statement": [
        {
            "Sid": "Identity",
            "Effect": "Allow",
            "Action": ["sts:GetCallerIdentity"],
            "Resource": "*",
        },
        {
            "Sid": "ReadSingleAdmission",
            "Effect": "Allow",
            "Action": ["ssm:GetParameter"],
            "Resource": f"arn:aws:ssm:{REGION}:{ACCOUNT}:parameter" + PARAMETER,
        },
    ],
}


class Host:
    def __init__(self, aws, github, receipt, *, clock=time.time, pause=time.sleep):
        self.aws, self.github, self.path = aws, github, Path(receipt)
        self.clock, self.pause = clock, pause
        self.state = None
        self.deadline = 0

    def save(self):
        # Replace atomically; preserve attempted calls even when replies are lost.
        temporary = self.path.with_suffix(".tmp")
        with temporary.open("w") as stream:
            json.dump(self.state, stream, sort_keys=True)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, self.path)

    def phase(self, phase):
        self.state["phase"] = phase
        self.save()

    def until(self, operation, predicate, *, seconds=300):
        deadline = min(self.deadline, self.clock() + seconds)
        while True:
            value = operation()
            if predicate(value):
                return value
            require(self.clock() < deadline, "host stage deadline reached")
            self.pause(5)

    def absent(self, operation, exception):
        try:
            operation()
        except exception:
            return True
        return False

    def statement(self, sql, *, records=False):
        rs, data = self.aws["redshift-serverless"], self.aws["redshift-data"]
        # Fresh resource and ownership reads precede EVERY host SQL operation.
        resources(self.state["admission"], rs, self.aws["ec2"])
        ns = rs.get_namespace(namespaceName=NAME)["namespace"]
        secret = ns["adminPasswordSecretArn"]
        require(
            secret == self.state["managed_secret_arn"],
            "managed secret association changed",
        )
        self.phase("host-sql-attempted")
        response = data.execute_statement(
            WorkgroupName=NAME, Database="adbc_qualification", SecretArn=secret, Sql=sql
        )
        statement_id = response["Id"]
        self.state.setdefault("statement_ids", []).append(statement_id)
        self.save()
        try:
            result = self.until(
                lambda: data.describe_statement(Id=statement_id),
                lambda r: r["Status"] in ("FINISHED", "FAILED", "ABORTED"),
                seconds=30,
            )
            require(result["Status"] == "FINISHED", "host SQL failed")
            return (
                data.get_statement_result(Id=statement_id)["Records"]
                if records
                else None
            )
        finally:
            # Bounded cancellation even on unknown describe/result replies.
            data.cancel_statement(Id=statement_id)

    def preflight(self, artifact_id, run, source):
        item = self.github.get("/actions/artifacts/" + str(artifact_id))
        require(
            item["name"] == "qualification-preflight-" + str(run)
            and item["expired"] is False
            and item["workflow_run"]["id"] == run
            and item["workflow_run"]["head_sha"] == source,
            "host preflight origin mismatch",
        )
        actual = self.github.get("/actions/runs/" + str(run))
        require(
            actual["path"] == WORKFLOW
            and actual["head_sha"] == source
            and actual["head_branch"] == REF.removeprefix("refs/heads/")
            and actual["event"] == "workflow_dispatch"
            and actual["run_attempt"] == 1
            and actual["repository"]["full_name"] == REPOSITORY,
            "host dispatch origin mismatch",
        )
        archive = self.github.get(
            "/actions/artifacts/" + str(artifact_id) + "/zip", binary=True
        )
        require(
            item["digest"] == "sha256:" + hashlib.sha256(archive).hexdigest(),
            "host preflight artifact digest mismatch",
        )
        with zipfile.ZipFile(io.BytesIO(archive)) as zipped:
            require(
                zipped.namelist() == ["preflight.json"],
                "unexpected host preflight files",
            )
            result = parse_json(zipped.read("preflight.json"))
        require(
            result["source_sha"] == source and result["run_id"] == run,
            "host preflight source mismatch",
        )
        require(
            result["review"] == self.github.review(source),
            "host original source review mismatch",
        )
        for kind in KINDS:
            observed = self.github.artifact(
                kind, result["artifacts"][kind]["id"], source
            )
            require(
                observed["workflow_run"]["id"] == result["artifacts"][kind]["run_id"],
                "host artifact run mismatch",
            )
        return result

    def verify_initial_role(self):
        iam = self.aws["iam"]
        role = iam.get_role(RoleName=ROLE_NAME)["Role"]
        require(
            role["Arn"] == ROLE
            and {t["Key"]: t["Value"] for t in role["Tags"]}.get(TAG)
            == self.state["nonce"],
            "unowned CI role",
        )
        trust = {
            "Version": "2012-10-17",
            "Statement": [
                {
                    "Effect": "Allow",
                    "Principal": {
                        "Federated": (
                            f"arn:aws:iam::{ACCOUNT}:oidc-provider/"
                            "token.actions.githubusercontent.com"
                        )
                    },
                    "Action": "sts:AssumeRoleWithWebIdentity",
                    "Condition": {
                        "StringEquals": {
                            "token.actions.githubusercontent.com:aud": (
                                "sts.amazonaws.com"
                            ),
                            "token.actions.githubusercontent.com:sub": OIDC_SUBJECT,
                        }
                    },
                }
            ],
        }
        require(role["AssumeRolePolicyDocument"] == trust, "CI OIDC trust mismatch")
        inline = iam.list_role_policies(RoleName=ROLE_NAME)
        attached = iam.list_attached_role_policies(RoleName=ROLE_NAME)
        require(
            inline.get("IsTruncated", False) is False
            and attached.get("IsTruncated", False) is False
            and "Marker" not in inline
            and "Marker" not in attached
            and inline["PolicyNames"] == [INITIAL_POLICY]
            and attached["AttachedPolicies"] == [],
            "pre-existing or incomplete CI policy inventory",
        )
        observed = iam.get_role_policy(RoleName=ROLE_NAME, PolicyName=INITIAL_POLICY)[
            "PolicyDocument"
        ]
        require(
            observed == INITIAL_POLICY_DOCUMENT,
            "initial CI policy exceeds exact admission read",
        )
        self.state["initial_policy_sha256"] = digest(observed)
        self.phase("initial-role-verified")

    def install_policy(self, template, expected_sha):
        # Recheck after resource creation; an expected policy NAME is not proof
        # of the permissions that remain alongside the finite runtime policy.
        self.verify_initial_role()
        iam = self.aws["iam"]
        require(
            file_sha(template) == expected_sha, "unreviewed runtime policy template"
        )
        cutoff = datetime.fromtimestamp(
            self.state["admission"]["window"]["teardown_deadline"], timezone.utc
        ).strftime("%Y-%m-%dT%H:%M:%SZ")
        encoded = Path(template).read_text()
        for name, value in {
            "EXACT_OWNED_WG": self.state["created"]["workgroup"],
            "EXACT_OWNED_NS": self.state["created"]["namespace"],
            "EXACT_OWNED_SG": self.state["created"]["security_group"],
            "EXACT_RUN_ID": str(self.state["run_id"]),
            "EXACT_CUTOFF": cutoff,
        }.items():
            encoded = encoded.replace(name, value)
        require(
            "EXACT_" not in encoded and self.state["nonce"] in encoded,
            "runtime policy binding incomplete",
        )
        policy = parse_json(encoded)
        cutoff = datetime.fromtimestamp(
            self.state["admission"]["window"]["teardown_deadline"], timezone.utc
        ).strftime("%Y-%m-%dT%H:%M:%SZ")
        for statement in policy["Statement"]:
            if statement["Effect"] == "Allow":
                statement.setdefault("Condition", {}).setdefault("DateLessThan", {})[
                    "aws:CurrentTime"
                ] = cutoff
        self.state["runtime_policy"] = policy
        self.state["runtime_policy_template_sha256"] = expected_sha
        self.state["attempted"].append("runtime_policy")
        self.phase("runtime-policy-write-attempted")
        iam.put_role_policy(
            RoleName=ROLE_NAME,
            PolicyName=RUN_POLICY,
            PolicyDocument=json.dumps(policy, sort_keys=True),
        )
        require(
            iam.get_role_policy(RoleName=ROLE_NAME, PolicyName=RUN_POLICY)[
                "PolicyDocument"
            ]
            == policy,
            "runtime policy readback mismatch",
        )

    def remove_policy(self):
        if "runtime_policy" not in self.state["attempted"]:
            return
        iam = self.aws["iam"]
        try:
            observed = iam.get_role_policy(RoleName=ROLE_NAME, PolicyName=RUN_POLICY)[
                "PolicyDocument"
            ]
        except iam.exceptions.NoSuchEntityException:
            return
        require(
            observed == self.state["runtime_policy"],
            "changed runtime policy requires inspection",
        )
        iam.delete_role_policy(RoleName=ROLE_NAME, PolicyName=RUN_POLICY)
        require(
            self.absent(
                lambda: iam.get_role_policy(RoleName=ROLE_NAME, PolicyName=RUN_POLICY),
                iam.exceptions.NoSuchEntityException,
            ),
            "runtime policy removal unverified",
        )

    def run(
        self,
        *,
        preflight_id,
        run,
        source,
        nonce,
        policy_template=None,
        policy_sha256=None,
    ):
        identity(self.aws["sts"], OWNER)
        require(SHA.fullmatch(nonce), "invalid host ownership nonce")
        require(
            not self.path.exists(),
            "existing host receipt requires recovery, never retry",
        )
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.deadline = self.clock() + 3600
        self.state = {
            "version": 1,
            "nonce": nonce,
            "source_sha": source,
            "run_id": run,
            "started_at": int(self.clock()),
            "deadline": int(self.deadline),
            "attempted": [],
            "created": {},
            "external_teardown": "required",
        }
        with self.path.open("x") as stream:
            json.dump(self.state, stream)
        failed = False
        try:
            preflight = self.preflight(preflight_id, run, source)
            # Reject broad initial authority before any resource can be created.
            self.verify_initial_role()
            rs, ec2 = self.aws["redshift-serverless"], self.aws["ec2"]
            require(
                self.absent(
                    lambda: rs.get_workgroup(workgroupName=NAME),
                    rs.exceptions.ResourceNotFoundException,
                )
                and self.absent(
                    lambda: rs.get_namespace(namespaceName=NAME),
                    rs.exceptions.ResourceNotFoundException,
                ),
                "dedicated name already exists",
            )
            require(
                not ec2.describe_security_groups(
                    Filters=[
                        {"Name": "group-name", "Values": [NAME]},
                        {"Name": "vpc-id", "Values": [VPC]},
                    ]
                )["SecurityGroups"],
                "dedicated security group already exists",
            )
            tags = [{"key": TAG, "value": nonce}]
            for kind, create in (
                (
                    "namespace",
                    lambda: rs.create_namespace(
                        namespaceName=NAME,
                        dbName="adbc_qualification",
                        adminUsername="adbc_qualification_admin",
                        manageAdminPassword=True,
                        iamRoles=[],
                        tags=tags,
                    )["namespace"],
                ),
                (
                    "security_group",
                    lambda: ec2.create_security_group(
                        GroupName=NAME,
                        Description="Finite ADBC isolated qualification",
                        VpcId=VPC,
                        TagSpecifications=[
                            {
                                "ResourceType": "security-group",
                                "Tags": [{"Key": TAG, "Value": nonce}],
                            }
                        ],
                    ),
                ),
            ):
                self.state["attempted"].append(kind)
                self.phase("create-" + kind + "-attempted")
                value = create()
                self.state["created"][kind] = (
                    value["namespaceId"] if kind == "namespace" else value["GroupId"]
                )
                self.save()
            self.until(
                lambda: rs.get_namespace(namespaceName=NAME)["namespace"],
                lambda n: n["status"] == "AVAILABLE",
            )
            group = self.state["created"]["security_group"]
            self.state["attempted"].append("workgroup")
            self.phase("create-workgroup-attempted")
            wg = rs.create_workgroup(
                workgroupName=NAME,
                namespaceName=NAME,
                baseCapacity=4,
                maxCapacity=4,
                enhancedVpcRouting=False,
                extraComputeForAutomaticOptimization=False,
                ipAddressType="ipv4",
                port=5439,
                publiclyAccessible=True,
                securityGroupIds=[group],
                subnetIds=SUBNETS,
                configParameters=[
                    {"parameterKey": "require_ssl", "parameterValue": "true"},
                    {
                        "parameterKey": "max_query_execution_time",
                        "parameterValue": "10",
                    },
                ],
                tags=tags,
            )["workgroup"]
            self.state["created"]["workgroup"] = wg["workgroupId"]
            self.save()
            wg = self.until(
                lambda: rs.get_workgroup(workgroupName=NAME)["workgroup"],
                lambda w: w["status"] == "AVAILABLE",
                seconds=600,
            )
            ns = rs.get_namespace(namespaceName=NAME)["namespace"]
            self.state["managed_secret_arn"] = ns["adminPasswordSecretArn"]
            self.state["attempted"].append("usage_limit")
            self.phase("create-usage-limit-attempted")
            usage = rs.create_usage_limit(
                amount=4,
                breachAction="deactivate",
                period="daily",
                resourceArn=wg["workgroupArn"],
                usageType="serverless-compute",
            )["usageLimit"]
            self.state["created"]["usage_limit"] = usage["usageLimitId"]
            self.save()
            now = int(self.clock())
            admission = {
                **preflight,
                "version": 2,
                "nonce": nonce,
                "resources": {
                    "workgroup_id": wg["workgroupId"],
                    "namespace_id": ns["namespaceId"],
                    "security_group_id": group,
                    "vpc_id": VPC,
                    "subnet_ids": SUBNETS,
                    "usage_limit_id": usage["usageLimitId"],
                },
                "window": {
                    "not_before": now,
                    "admit_before": now + 300,
                    "query_deadline": now + 900,
                    "teardown_deadline": int(self.deadline),
                },
                "cleanup": {"owner": OWNER, "observer": OWNER},
            }
            validate(admission, source=source, run=run, now=now)
            self.state["admission"] = admission
            self.save()
            resources(admission, rs, ec2)
            self.install_policy(policy_template, policy_sha256)
            self.statement(
                'CREATE USER "IAMR:adbc-isolated-qualification-ci" PASSWORD DISABLE'
            )
            self.statement(
                "GRANT CREATE ON DATABASE adbc_qualification "
                'TO "IAMR:adbc-isolated-qualification-ci"'
            )
            require(
                self.clock() < admission["window"]["admit_before"],
                "bootstrap exhausted admission window",
            )
            self.phase("admission-write-attempted")
            self.aws["ssm"].put_parameter(
                Name=PARAMETER,
                Value=json.dumps(admission, sort_keys=True, separators=(",", ":")),
                Type="String",
                Tier="Standard",
                Overwrite=False,
            )
            self.phase("admission-issued")
            result = self.until(
                lambda: self.github.get("/actions/runs/" + str(run)),
                lambda r: r["status"] == "completed",
                seconds=1000,
            )
            self.state["ci_conclusion"] = result["conclusion"]
            self.save()
            require(
                result["conclusion"] == "success", "CI qualification did not succeed"
            )
        except BaseException:
            failed = True
            self.state["failure"] = "host-lifecycle-failed; no paid retry"
            self.save()
        finally:
            try:
                self.cleanup()
            except BaseException:
                failed = True
                self.state["external_teardown"] = (
                    "unknown; manual exact-owned recovery required"
                )
                self.save()
        require(
            not failed
            and self.state["external_teardown"] == "verified"
            and self.state.get("independent_observation", {}).get("fixtures_absent")
            is True
            and self.state.get("independent_observation", {}).get("sessions_absent")
            is True
            and self.state.get("runtime_policy_cleanup")
            == "removed-or-never-installed",
            "host lifecycle failed; inspect durable nonsecret receipt",
        )
        return self.state

    def owned(self, kind):
        rs = self.aws["redshift-serverless"]
        getter = rs.get_workgroup if kind == "workgroup" else rs.get_namespace
        argument = {"workgroupName" if kind == "workgroup" else "namespaceName": NAME}
        if self.absent(
            lambda: getter(**argument), rs.exceptions.ResourceNotFoundException
        ):
            return None
        value = getter(**argument)[kind]
        resource_id = value[kind + "Id"]
        require(
            not any(
                p
                in " ".join(
                    str(value.get(k, ""))
                    for k in (kind + "Id", kind + "Arn", kind + "Name")
                ).lower()
                for p in PROTECTED
            ),
            "protected resource observation",
        )
        tags = {
            t["key"]: t["value"]
            for t in rs.list_tags_for_resource(resourceArn=value[kind + "Arn"])["tags"]
        }
        require(
            kind in self.state["attempted"]
            and tags.get(TAG) == self.state["nonce"]
            and value[kind + "Name"] == NAME
            and value[kind + "Arn"] == arn(kind, resource_id)
            and self.state["created"].get(kind, resource_id) == resource_id,
            "refuse unowned resource teardown",
        )
        self.state["created"][kind] = resource_id
        if kind == "namespace" and value.get("adminPasswordSecretArn"):
            self.state.setdefault("managed_secret_arn", value["adminPasswordSecretArn"])
        self.save()
        return value

    def cleanup(self):
        # Cleanup has a finite extra observation allowance, never paid SQL retry.
        self.deadline = min(self.state["deadline"] + 180, self.clock() + 180)
        identity(self.aws["sts"], OWNER)
        self.phase("external-observation")
        try:
            current = self.github.get("/actions/runs/" + str(self.state["run_id"]))
            if current["status"] != "completed":
                self.github.get(
                    "/actions/runs/" + str(self.state["run_id"]) + "/cancel",
                    binary=True,
                    method="POST",
                )
                self.until(
                    lambda: self.github.get(
                        "/actions/runs/" + str(self.state["run_id"])
                    ),
                    lambda r: r["status"] == "completed",
                    seconds=30,
                )
            self.state["ci_stopped"] = "verified"
        except BaseException:
            self.state["ci_stopped"] = "unknown; dedicated teardown still required"
        try:
            self.remove_policy()
            self.state["runtime_policy_cleanup"] = "removed-or-never-installed"
        except BaseException:
            self.state["runtime_policy_cleanup"] = "unknown; host IAM cleanup required"
        self.save()
        if "admission" in self.state:
            try:
                run = self.state["run_id"]
                rows = self.statement(
                    "SELECT (SELECT COUNT(*) FROM pg_namespace "
                    "WHERE nspname IN ('adbc_q_"
                    + str(run)
                    + "0','adbc_q_"
                    + str(run)
                    + "1')), (SELECT COUNT(*) FROM stv_sessions "
                    "WHERE user_name = 'IAMR:adbc-isolated-qualification-ci')",
                    records=True,
                )
                self.state["independent_observation"] = {
                    "authenticated_role": OWNER,
                    "observed_at": int(self.clock()),
                    "fixtures_absent": rows == [[{"longValue": 0}, {"longValue": 0}]],
                    "sessions_absent": rows == [[{"longValue": 0}, {"longValue": 0}]],
                }
            except BaseException:
                self.state["independent_observation"] = {
                    "state": "unknown; dedicated resource teardown still required"
                }
            self.save()
        rs, ec2 = self.aws["redshift-serverless"], self.aws["ec2"]
        issues = []
        group_id = None
        try:
            groups = ec2.describe_security_groups(
                Filters=[
                    {"Name": "group-name", "Values": [NAME]},
                    {"Name": "vpc-id", "Values": [VPC]},
                ]
            )["SecurityGroups"]
            require(len(groups) <= 1, "ambiguous dedicated group")
            for group in groups:
                require(
                    "security_group" in self.state["attempted"]
                    and group["OwnerId"] == ACCOUNT
                    and group["VpcId"] == VPC
                    and group["GroupName"] == NAME
                    and {t["Key"]: t["Value"] for t in group.get("Tags", [])}.get(TAG)
                    == self.state["nonce"]
                    and self.state["created"].get("security_group", group["GroupId"])
                    == group["GroupId"],
                    "refuse unowned group teardown",
                )
                group_id = group["GroupId"]
                for rule in rules(ec2, group_id):
                    if not rule["IsEgress"]:
                        a = self.state.get("admission")
                        from pgwire_qualification_live import owned_rule

                        if a is None or not owned_rule(rule, a):
                            issues.append("unexpected-ingress-preserved")
                            continue
                        ec2.revoke_security_group_ingress(
                            GroupId=group_id,
                            SecurityGroupRuleIds=[rule["SecurityGroupRuleId"]],
                        )
                require(
                    not any(not r["IsEgress"] for r in rules(ec2, group_id)),
                    "ingress removal unverified",
                )
        except BaseException:
            issues.append("ingress-unknown")
        # Independent attempts: a failed rule/usage API does not skip stopping
        # the authenticated dedicated workgroup or deleting its namespace.
        limits = []
        wg = None
        try:
            wg = self.owned("workgroup")
            if wg:
                try:
                    response = rs.list_usage_limits(resourceArn=wg["workgroupArn"])
                    require(not response.get("nextToken"), "usage pagination unknown")
                    for limit in response["usageLimits"]:
                        require(
                            "usage_limit" in self.state["attempted"]
                            and limit["resourceArn"] == wg["workgroupArn"]
                            and limit["amount"] == 4
                            and limit["period"] == "daily"
                            and limit["breachAction"] == "deactivate"
                            and self.state["created"].get(
                                "usage_limit", limit["usageLimitId"]
                            )
                            == limit["usageLimitId"],
                            "unowned usage limit",
                        )
                        limits.append(limit)
                except BaseException:
                    issues.append("usage-observation-unknown")
                self.phase("delete-workgroup-attempted")
                rs.delete_workgroup(workgroupName=NAME)
                self.until(
                    lambda: self.absent(
                        lambda: rs.get_workgroup(workgroupName=NAME),
                        rs.exceptions.ResourceNotFoundException,
                    ),
                    bool,
                    seconds=120,
                )
                for limit in limits:
                    try:
                        rs.delete_usage_limit(usageLimitId=limit["usageLimitId"])
                        require(
                            self.absent(
                                lambda: rs.get_usage_limit(
                                    usageLimitId=limit["usageLimitId"]
                                ),
                                rs.exceptions.ResourceNotFoundException,
                            ),
                            "usage deletion unverified",
                        )
                    except rs.exceptions.ResourceNotFoundException:
                        pass
                    except BaseException:
                        issues.append("usage-removal-unknown")
        except BaseException:
            issues.append("workgroup-removal-unknown")
        try:
            ns = self.owned("namespace")
            if ns:
                self.phase("delete-namespace-attempted")
                rs.delete_namespace(namespaceName=NAME)
                self.until(
                    lambda: self.absent(
                        lambda: rs.get_namespace(namespaceName=NAME),
                        rs.exceptions.ResourceNotFoundException,
                    ),
                    bool,
                    seconds=120,
                )
        except BaseException:
            issues.append("namespace-removal-unknown")
        try:
            # Preserve unexpected rules for inspection instead of deleting the
            # group around them. Workgroup deletion still stops its compute.
            if group_id and not any("ingress" in issue for issue in issues):
                ec2.delete_security_group(GroupId=group_id)
            require(
                not ec2.describe_security_groups(
                    Filters=[
                        {"Name": "group-name", "Values": [NAME]},
                        {"Name": "vpc-id", "Values": [VPC]},
                    ]
                )["SecurityGroups"],
                "group deletion unverified",
            )
        except BaseException:
            issues.append("group-removal-unknown")
        self.state["cleanup_issues"] = issues
        self.state["external_teardown"] = (
            "verified" if not issues else "unknown; exact-owned host recovery required"
        )
        self.phase("complete; remove exact CI IAM role/policy; retain SSM evidence")
        require(not issues, "host cleanup incomplete; inspect receipt")


def host_clients():
    import boto3
    from botocore.config import Config

    require(
        not any(
            k.startswith("AWS_ENDPOINT_URL")
            or k
            in (
                "AWS_PROFILE",
                "AWS_DEFAULT_PROFILE",
                "AWS_CONFIG_FILE",
                "AWS_SHARED_CREDENTIALS_FILE",
                "AWS_CA_BUNDLE",
                "BOTO_CONFIG",
            )
            for k in os.environ
        ),
        "ambient host AWS overrides rejected",
    )
    session = boto3.Session(profile_name="adbc-provisioner", region_name=REGION)
    config = Config(
        retries={"total_max_attempts": 1, "mode": "standard"},
        connect_timeout=5,
        read_timeout=10,
        proxies={},
        ignore_configured_endpoint_urls=True,
    )
    return {
        service: session.client(
            service,
            region_name="us-east-1" if service == "iam" else REGION,
            endpoint_url="https://iam.amazonaws.com"
            if service == "iam"
            else f"https://{service}.{REGION}.amazonaws.com",
            config=config,
            verify=True,
        )
        for service in (
            "sts",
            "ssm",
            "redshift-serverless",
            "redshift-data",
            "ec2",
            "iam",
        )
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--preflight-artifact-id", type=int, required=True)
    parser.add_argument("--run-id", type=int, required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--nonce", required=True)
    parser.add_argument("--policy-template", type=Path, required=True)
    parser.add_argument("--policy-template-sha256", required=True)
    args = parser.parse_args()
    try:
        Host(host_clients(), GitHub(os.environ["GH_TOKEN"]), args.receipt).run(
            preflight_id=args.preflight_artifact_id,
            run=args.run_id,
            source=args.source_sha,
            nonce=args.nonce,
            policy_template=args.policy_template,
            policy_sha256=args.policy_template_sha256,
        )
        print(
            "Dedicated database resources removed; inspect independent observation "
            "and finish exact IAM cleanup."
        )
        return 0
    except BaseException:
        print(
            "Host lifecycle failed; inspect exact owned cleanup and receipt. "
            "No paid retry.",
            file=sys.stderr,
        )
        return 1


if __name__ == "__main__":
    sys.exit(main())
