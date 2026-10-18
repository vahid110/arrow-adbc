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
from pathlib import Path

from pgwire_qualification import file_sha
from pgwire_qualification_live import (
    ACCOUNT,
    KINDS,
    NAME,
    OIDC_SUBJECT,
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
from pgwire_qualification_manifest import PROTECTED, SHA, Rejected, digest, require
from pgwire_qualification_policy import (
    admission_name,
    checked_template,
    host_attempt_name,
    initial_policy,
    operation_prefix,
    runtime_policy,
)
from pgwire_qualification_prepare import GitHub, parse_json

OWNER = f"arn:aws:iam::{ACCOUNT}:role/adbc-provisioner"
INITIAL_POLICY = "adbc-qualification-initial"
RUN_POLICY = "adbc-qualification-20261016-runtime"
ROLE_NAME = "adbc-isolated-qualification-ci"
FAILURE_STAGES = {
    "preflight-verification": "PREFLIGHT",
    "operation-inventory": "OPERATION_INVENTORY",
    "host-reservation-attempted": "HOST_RESERVATION",
    "initial-role-verification": "INITIAL_ROLE",
    "initial-role-verified": "INITIAL_ROLE",
    "create-namespace-attempted": "CREATE_NAMESPACE",
    "create-security_group-attempted": "CREATE_SECURITY_GROUP",
    "create-workgroup-attempted": "CREATE_WORKGROUP",
    "create-usage-limit-attempted": "CREATE_USAGE_LIMIT",
    "initial-resource-verification": "INITIAL_RESOURCES",
    "runtime-policy-write-attempted": "RUNTIME_POLICY",
    "host-sql-attempted": "HOST_SQL",
    "host-sql-validation": "HOST_SQL",
    "observer-sql": "OBSERVER",
    "admission-write-attempted": "ADMISSION",
    "admission-run-verification": "ADMISSION_RUN",
    "admission-issued": "CI_COMPLETION",
}
FAILURE_REASONS = {
    "operation inventory is existing or incomplete": "OPERATION_SPENT_OR_UNKNOWN",
    "operation nonce mismatch": "OPERATION_NONCE",
    "runtime template is not the versioned contract": "POLICY_TEMPLATE",
    "workgroup not ready": "WORKGROUP_NOT_READY",
    "workgroup ownership mismatch": "WORKGROUP_OWNERSHIP",
    "namespace ownership mismatch": "NAMESPACE_OWNERSHIP",
    "namespace not ready": "NAMESPACE_NOT_READY",
    "finite compute control mismatch": "COMPUTE_CONTROLS",
    "workgroup network mismatch": "NETWORK_CONTROLS",
    "workgroup SQL limits mismatch": "SQL_CONTROLS",
    "unowned Redshift resource": "RESOURCE_TAGS",
    "finite usage control mismatch": "USAGE_CONTROLS",
    "unowned security group": "SECURITY_GROUP_OWNERSHIP",
    "subnet association mismatch": "SUBNET_ASSOCIATION",
    "host stage deadline reached": "DEADLINE",
    "host SQL failed": "SQL_FAILED",
    "observer identity or counts invalid": "OBSERVER_PROOF_INVALID",
    "observer found remaining fixtures or sessions": "OBSERVER_NONZERO",
    "host dispatch origin mismatch": "DISPATCH_PROVENANCE",
    "host dispatch no longer running": "DISPATCH_STOPPED",
}
TERMINAL = ("FINISHED", "FAILED", "ABORTED")


def failure_reason(error):
    # Compare only our static rejection messages; never serialize SDK/SQL text.
    return (
        FAILURE_REASONS.get(str(error), "REJECTED")
        if isinstance(error, Rejected)
        else "SDK_OR_UNEXPECTED_FAILURE"
    )


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
            self.check_deadline(deadline)
            value = operation()
            self.check_deadline(deadline)
            if predicate(value):
                return value
            self.pause(5)

    def check_deadline(self, deadline=None):
        require(
            self.clock()
            < min(self.deadline, self.deadline if deadline is None else deadline),
            "host stage deadline reached",
        )

    def absent(self, operation, exception):
        try:
            operation()
        except exception:
            return True
        return False

    def statement(self, sql, *, records=False):
        self.check_deadline()
        self.phase("host-sql-validation")
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
        self.check_deadline()
        response = data.execute_statement(
            WorkgroupName=NAME, Database="adbc_qualification", SecretArn=secret, Sql=sql
        )
        statement_id = response["Id"]
        self.state.setdefault("statement_ids", []).append(statement_id)
        evidence = {"terminal": "UNKNOWN", "cancel": "NOT_ATTEMPTED"}
        self.state.setdefault("statements", []).append(evidence)
        self.save()

        def describe():
            result = data.describe_statement(Id=statement_id)
            if result["Status"] in TERMINAL:
                # A late response is still proof of termination. Preserve this
                # fact before until() rejects time, so finally never cancels it.
                evidence["terminal"] = result["Status"]
                evidence["cancel"] = "NOT_NEEDED"
                self.save()
            return result

        try:
            result = self.until(
                describe,
                lambda r: r["Status"] in TERMINAL,
                seconds=30,
            )
            require(result["Status"] == "FINISHED", "host SQL failed")
            rows = (
                data.get_statement_result(Id=statement_id)["Records"]
                if records
                else None
            )
            self.check_deadline()
            return rows
        finally:
            if evidence["terminal"] == "UNKNOWN":
                # Never mask the original failure. A completed-statement race
                # needs a fresh terminal readback, not a blind cancel retry.
                cancelled = False
                race = False
                try:
                    cancelled = (
                        data.cancel_statement(Id=statement_id).get("Status") is True
                    )
                except data.exceptions.ValidationException:
                    race = True
                except BaseException:
                    pass
                evidence["cancel"] = "UNKNOWN"
                try:
                    observed = data.describe_statement(Id=statement_id)["Status"]
                    if observed in TERMINAL:
                        evidence["terminal"] = observed
                        if race:
                            evidence["cancel"] = "TERMINAL_RACE"
                        elif cancelled:
                            evidence["cancel"] = "REQUESTED_AND_TERMINAL"
                except BaseException:
                    pass
                self.save()

    def preflight(self, artifact_id, run, source):
        item = self.github.get("/actions/artifacts/" + str(artifact_id))
        require(
            item["name"] == "qualification-preflight-" + str(run)
            and item["expired"] is False
            and item["workflow_run"]["id"] == run
            and item["workflow_run"]["head_sha"] == source,
            "host preflight origin mismatch",
        )
        self.verify_run(run, source)
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

    def verify_run(self, run, source, *, active=False):
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
        require(
            not active or actual["status"] == "in_progress",
            "host dispatch no longer running",
        )

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
            observed == initial_policy(self.state["nonce"]),
            "initial CI policy exceeds exact admission read",
        )
        self.state["initial_policy_sha256"] = digest(observed)
        self.phase("initial-role-verified")

    def install_policy(self, template, expected_sha):
        self.check_deadline()
        # Recheck after resource creation; an expected policy NAME is not proof
        # of the permissions that remain alongside the finite runtime policy.
        self.verify_initial_role()
        self.check_deadline()
        iam = self.aws["iam"]
        require(
            file_sha(template) == expected_sha, "unreviewed runtime policy template"
        )
        checked_template(template, expected_sha)
        policy = runtime_policy(self.state["admission"])
        self.state["runtime_policy"] = policy
        self.state["runtime_policy_template_sha256"] = expected_sha
        self.state["attempted"].append("runtime_policy")
        self.phase("runtime-policy-write-attempted")
        self.check_deadline()
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
        self.check_deadline()

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
        started = self.clock()
        self.deadline = started + 3600
        identity(self.aws["sts"], OWNER)
        require(SHA.fullmatch(nonce), "invalid host ownership nonce")
        require(
            not self.path.exists(),
            "existing host receipt requires recovery, never retry",
        )
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.state = {
            "version": 1,
            "nonce": nonce,
            "source_sha": source,
            "run_id": run,
            "started_at": int(started),
            "deadline": int(self.deadline),
            "host_attempt": "NOT_ATTEMPTED",
            "attempted": [],
            "created": {},
            "external_teardown": "required",
        }
        with self.path.open("x") as stream:
            json.dump(self.state, stream)
        failed = False
        try:
            self.phase("preflight-verification")
            self.check_deadline()
            preflight = self.preflight(preflight_id, run, source)
            self.check_deadline()
            require(preflight["nonce"] == nonce, "operation nonce mismatch")
            checked_template(policy_template, policy_sha256)
            self.phase("operation-inventory")
            self.check_deadline()
            inventory = self.aws["ssm"].describe_parameters(
                ParameterFilters=[
                    {
                        "Key": "Name",
                        "Option": "BeginsWith",
                        "Values": [operation_prefix(nonce)],
                    }
                ],
                MaxResults=50,
            )
            require(
                type(inventory) is dict
                and "NextToken" not in inventory
                and type(inventory.get("Parameters")) is list
                and inventory["Parameters"] == [],
                "operation inventory is existing or incomplete",
            )
            self.check_deadline()
            # Reject broad initial authority before any resource can be created.
            self.phase("initial-role-verification")
            self.verify_initial_role()
            self.check_deadline()
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
            self.check_deadline()
            # This immutable reservation wins before any provisioning. A lost
            # reply is spent, never read back or retried as authorization.
            reservation = {
                "version": 1,
                "source_sha": source,
                "run_id": run,
                "nonce": nonce,
                "policy_sha256": policy_sha256,
                "deadline": self.state["deadline"],
            }
            self.state["host_attempt"] = "ATTEMPTED_UNKNOWN"
            self.phase("host-reservation-attempted")
            self.check_deadline()
            reservation_reply = self.aws["ssm"].put_parameter(
                Name=host_attempt_name(nonce),
                Type="String",
                Tier="Standard",
                Overwrite=False,
                Value=json.dumps(reservation, sort_keys=True),
            )
            self.check_deadline()
            require(
                reservation_reply.get("Version") == 1,
                "host reservation is not immutable version one",
            )
            self.state["host_attempt"] = "CREATED"
            self.save()
            self.check_deadline()
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
                self.check_deadline()
                self.state["attempted"].append(kind)
                self.phase("create-" + kind + "-attempted")
                self.check_deadline()
                value = create()
                self.state["created"][kind] = (
                    value["namespaceId"] if kind == "namespace" else value["GroupId"]
                )
                self.save()
                self.check_deadline()
            self.until(
                lambda: rs.get_namespace(namespaceName=NAME)["namespace"],
                lambda n: n["status"] == "AVAILABLE",
            )
            group = self.state["created"]["security_group"]
            self.state["attempted"].append("workgroup")
            self.phase("create-workgroup-attempted")
            self.check_deadline()
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
            self.check_deadline()
            usage = rs.create_usage_limit(
                amount=4,
                breachAction="deactivate",
                period="daily",
                resourceArn=wg["workgroupArn"],
                usageType="serverless-compute",
            )["usageLimit"]
            self.state["created"]["usage_limit"] = usage["usageLimitId"]
            self.save()
            self.check_deadline()
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
            validate(admission, source=source, run=run, now=now, nonce=nonce)
            self.state["admission"] = admission
            self.save()
            self.phase("initial-resource-verification")
            self.until(
                lambda: resources(admission, rs, ec2, initializing=True),
                lambda pair: all(v["status"] == "AVAILABLE" for v in pair),
                seconds=120,
            )
            resources(admission, rs, ec2)
            self.install_policy(policy_template, policy_sha256)
            self.statement(
                'CREATE USER "IAMR:adbc-isolated-qualification-ci" PASSWORD DISABLE'
            )
            self.statement(
                "GRANT CREATE ON DATABASE adbc_qualification "
                'TO "IAMR:adbc-isolated-qualification-ci"'
            )
            observed = self.observe()
            self.state["pre_admission_observation"] = observed
            self.save()
            require(
                observed["fixtures_absent"] and observed["sessions_absent"],
                "observer found remaining fixtures or sessions",
            )
            self.check_deadline(admission["window"]["admit_before"])
            self.phase("admission-run-verification")
            self.verify_run(run, source, active=True)
            self.check_deadline(admission["window"]["admit_before"])
            require(
                self.clock() < admission["window"]["admit_before"],
                "bootstrap exhausted admission window",
            )
            self.phase("admission-write-attempted")
            self.check_deadline(admission["window"]["admit_before"])
            self.aws["ssm"].put_parameter(
                Name=admission_name(nonce),
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
        except BaseException as error:
            failed = True
            self.state["first_failure"] = {
                "stage": FAILURE_STAGES.get(
                    self.state.get("phase"), "OTHER_HOST_STAGE"
                ),
                "reason": failure_reason(error),
            }
            self.state["failure"] = "host-lifecycle-failed; no paid retry"
            self.save()
        finally:
            try:
                if (
                    not self.state["attempted"]
                    and self.state["host_attempt"] != "CREATED"
                ):
                    # A duplicate/uncertain reservation owns no resources or
                    # shared workflow cancellation. Preserve it without effects.
                    self.state["external_teardown"] = "not-started"
                    self.save()
                else:
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

    def observe(self):
        self.phase("observer-sql")
        identity(self.aws["sts"], OWNER)
        try:
            run = self.state["run_id"]
            rows = self.statement(
                "SELECT (SELECT COUNT(*) FROM svv_user_info "
                "WHERE user_name=current_user "
                "AND user_name='adbc_qualification_admin' AND superuser), "
                "(SELECT COUNT(*) FROM svv_user_info "
                "WHERE user_name='IAMR:adbc-isolated-qualification-ci'), "
                "(SELECT COUNT(*) FROM pg_namespace "
                "WHERE nspname IN ('adbc_q_"
                + str(run)
                + "0','adbc_q_"
                + str(run)
                + "1')), (SELECT COUNT(*) FROM sys_session_history "
                "WHERE status='active' AND user_id IN "
                "(SELECT CAST(user_id AS VARCHAR) FROM svv_user_info "
                "WHERE user_name='IAMR:adbc-isolated-qualification-ci'))",
                records=True,
            )
            require(
                type(rows) is list
                and len(rows) == 1
                and type(rows[0]) is list
                and len(rows[0]) == 4
                and all(
                    type(v) is dict
                    and set(v) == {"longValue"}
                    and type(v["longValue"]) is int
                    and v["longValue"] >= 0
                    for v in rows[0]
                ),
                "observer identity or counts invalid",
            )
            admin, users, fixtures, sessions = [v["longValue"] for v in rows[0]]
            require(
                admin == 1 and users == 1 and fixtures <= 2,
                "observer identity or counts invalid",
            )
            return {
                "authenticated_role": OWNER,
                "observed_at": int(self.clock()),
                "admin_visibility_verified": True,
                "ci_user": "PRESENT",
                "fixtures_absent": fixtures == 0,
                "sessions_absent": sessions == 0,
            }
        finally:
            self.phase("observer-sql")

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
                self.state["independent_observation"] = self.observe()
            except BaseException as error:
                self.state["independent_observation"] = {
                    "state": "unknown; dedicated resource teardown still required",
                    "failure_reason": failure_reason(error),
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
