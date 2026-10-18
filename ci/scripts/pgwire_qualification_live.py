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

"""One finite, host-issued SSM admission; the old unsigned gates stay closed.

Production constructs only fixed AWS endpoints and authenticates GitHub directly.
Tests inject fake clients into this internal orchestration, never real credentials.
"""

import argparse
import ipaddress
import json
import os
import re
import sys
import time
from datetime import datetime
from pathlib import Path

from pgwire_qualification import file_sha, preflight, verify_probe
from pgwire_qualification_manifest import (
    ACCOUNT,
    COMMIT,
    IDENTITY,
    NAME,
    PROTECTED,
    REGION,
    ROLE,
    SHA,
    UUID,
    Rejected,
    digest,
    load,
    require,
)
from pgwire_qualification_policy import admission_name, claim_name
from pgwire_qualification_process import client, private_process, validate_uri

REPOSITORY = "vahid110/arrow-adbc"
REF = "refs/heads/feature/pgwire-core-redshift"
# Pinned to authenticated repository OIDC metadata, including immutable IDs.
OIDC_SUBJECT = (
    "repo:vahid110@5770674/arrow-adbc@1363017696:"
    "ref:refs/heads/feature/pgwire-core-redshift"
)
WORKFLOW = ".github/workflows/pgwire-qualification-live.yml"
HOST = f"{NAME}.{ACCOUNT}.{REGION}.redshift-serverless.amazonaws.com"
TAG = "adbc-qualification-nonce"
KINDS = ("c_archive", "python_wheel")
VPC = "vpc-043316eb7ec0f10f5"
SUBNETS = [
    "subnet-0c69fa76ab9443636",
    "subnet-050bbadb54934c3e4",
    "subnet-08c74d1fdfbb8d023",
]


def keys(value, names):
    require(
        type(value) is dict and set(value) == set(names.split()),
        "invalid admission fields",
    )


def matches(value, pattern):
    require(
        type(value) is str and pattern.fullmatch(value), "invalid admission identity"
    )


def validate(admission, *, source, run, now, nonce):
    admission_name(nonce)
    require(admission.get("nonce") == nonce, "operation nonce mismatch")
    # Never query a protected caller-supplied target, even in a malformed record.
    encoded = json.dumps(admission, allow_nan=False)
    require(len(encoded.encode()) <= 4096, "admission exceeds Standard String limit")
    require(not any(p in encoded.lower() for p in PROTECTED), "protected target")
    keys(
        admission,
        "version nonce source_sha run_id artifacts resources window cleanup "
        "ca_sha256 runner_cidr c_client_sha256 review",
    )
    require(admission["version"] == 2, "unsupported admission")
    matches(admission["nonce"], SHA)
    matches(admission["source_sha"], COMMIT)
    require(
        admission["source_sha"] == source
        and type(run) is int
        and type(admission["run_id"]) is int
        and admission["run_id"] == run,
        "source/run mismatch",
    )
    matches(admission["ca_sha256"], SHA)
    matches(admission["c_client_sha256"], SHA)
    network = ipaddress.ip_network(admission["runner_cidr"], strict=True)
    require(
        network.version == 4
        and network.prefixlen == 32
        and network.network_address.is_global,
        "invalid owned ingress",
    )
    keys(admission["artifacts"], " ".join(KINDS))
    for item in admission["artifacts"].values():
        keys(item, "id run_id sha256 installed_tree_sha256")
        require(
            type(item["id"]) is int
            and item["id"] > 0
            and type(item["run_id"]) is int
            and item["run_id"] > 0,
            "invalid artifact identity",
        )
        matches(item["sha256"], SHA)
        matches(item["installed_tree_sha256"], SHA)
    keys(
        admission["review"],
        "pr head_sha tree_sha comment_id comment_sha256 checks_sha256",
    )
    for field in ("head_sha", "tree_sha"):
        matches(admission["review"][field], COMMIT)
    for field in ("comment_sha256", "checks_sha256"):
        matches(admission["review"][field], SHA)
    require(
        all(
            type(admission["review"][f]) is int and admission["review"][f] > 0
            for f in ("pr", "comment_id")
        ),
        "invalid review identity",
    )
    r = admission["resources"]
    keys(
        r,
        "workgroup_id namespace_id security_group_id vpc_id subnet_ids usage_limit_id",
    )
    for field in ("workgroup_id", "namespace_id", "usage_limit_id"):
        matches(r[field], UUID)
    require(r["workgroup_id"] != r["namespace_id"], "invalid resource association")
    matches(r["security_group_id"], re.compile(r"sg-[0-9a-f]{17}"))
    require(
        r["vpc_id"] == VPC and sorted(r["subnet_ids"]) == sorted(SUBNETS),
        "unapproved VPC/subnets",
    )
    require(
        type(r["subnet_ids"]) is list
        and len(r["subnet_ids"]) == 3
        and len(set(r["subnet_ids"])) == 3,
        "invalid subnets",
    )
    for subnet in r["subnet_ids"]:
        matches(subnet, re.compile(r"subnet-[0-9a-f]{17}"))
    w = admission["window"]
    keys(w, "not_before admit_before query_deadline teardown_deadline")
    require(all(type(v) is int for v in w.values()), "invalid window")
    require(
        w["not_before"]
        <= now
        < w["admit_before"]
        <= w["query_deadline"]
        <= w["teardown_deadline"],
        "expired admission",
    )
    require(
        w["query_deadline"] - w["not_before"] <= 900
        and w["teardown_deadline"] - w["not_before"] <= 3600,
        "unbounded admission",
    )
    keys(admission["cleanup"], "owner observer")
    for identity in admission["cleanup"].values():
        matches(identity, IDENTITY)
        require(identity != ROLE, "external cleanup owner required")
    return admission


def installed_manifest(record):
    """Installed bytes and fixed TLS target only; this never grants admission.

    Preparation has no host-issued execution window. Admitted execution validates
    and reads its mandatory deadlines directly from the authenticated admission.
    """
    return {
        "binding": record["artifacts"],
        "tls": {"trusted_ca_sha256": record["ca_sha256"], "hostname": HOST},
        "namespace": {"database": "adbc_qualification"},
    }


def identity(sts, role=ROLE):
    observed = sts.get_caller_identity()
    expected = f"arn:aws:sts::{ACCOUNT}:assumed-role/{role.rsplit('/', 1)[1]}/"
    require(
        observed["Account"] == ACCOUNT
        and observed["Arn"].startswith(expected)
        and "/" not in observed["Arn"][len(expected) :],
        "wrong authenticated AWS identity",
    )
    return observed["Arn"]


def arn(kind, value):
    return f"arn:aws:redshift-serverless:{REGION}:{ACCOUNT}:{kind}/{value}"


def resources(admission, redshift, ec2, *, initializing=False):
    r = admission["resources"]
    wg = redshift.get_workgroup(workgroupName=NAME)["workgroup"]
    ns = redshift.get_namespace(namespaceName=NAME)["namespace"]
    require(
        wg["workgroupId"] == r["workgroup_id"]
        and wg["workgroupArn"] == arn("workgroup", r["workgroup_id"])
        and wg["workgroupName"] == NAME
        and wg["namespaceName"] == NAME,
        "workgroup ownership mismatch",
    )
    require(
        wg["status"] == "AVAILABLE" or (initializing and wg["status"] == "MODIFYING"),
        "workgroup not ready",
    )
    require(
        ns["namespaceId"] == r["namespace_id"]
        and ns["namespaceArn"] == arn("namespace", r["namespace_id"])
        and ns["namespaceName"] == NAME
        and ns["dbName"] == "adbc_qualification",
        "namespace ownership mismatch",
    )
    require(
        ns["status"] == "AVAILABLE" or (initializing and ns["status"] == "MODIFYING"),
        "namespace not ready",
    )
    require(
        wg["baseCapacity"] == wg["maxCapacity"] == 4
        and wg.get("extraComputeForAutomaticOptimization") is False
        and wg.get("pricePerformanceTarget", {}).get("status") == "DISABLED",
        "finite compute control mismatch",
    )
    require(
        wg["securityGroupIds"] == [r["security_group_id"]]
        and sorted(wg["subnetIds"]) == sorted(r["subnet_ids"])
        and wg["publiclyAccessible"] is True
        and wg["endpoint"]["address"] == HOST
        and wg["endpoint"]["port"] == 5439,
        "workgroup network mismatch",
    )
    config = {p["parameterKey"]: p["parameterValue"] for p in wg["configParameters"]}
    require(
        config.get("require_ssl") == "true"
        and config.get("max_query_execution_time") == "10",
        "workgroup SQL limits mismatch",
    )
    for resource in (wg["workgroupArn"], ns["namespaceArn"]):
        tags = {
            t["key"]: t["value"]
            for t in redshift.list_tags_for_resource(resourceArn=resource)["tags"]
        }
        require(tags.get(TAG) == admission["nonce"], "unowned Redshift resource")
    usage = redshift.get_usage_limit(usageLimitId=r["usage_limit_id"])["usageLimit"]
    require(
        usage["usageLimitId"] == r["usage_limit_id"]
        and usage["resourceArn"] == wg["workgroupArn"]
        and usage["usageType"] == "serverless-compute"
        and usage["period"] == "daily"
        and usage["amount"] == 4
        and usage["breachAction"] == "deactivate",
        "finite usage control mismatch",
    )
    groups = ec2.describe_security_groups(GroupIds=[r["security_group_id"]])[
        "SecurityGroups"
    ]
    require(len(groups) == 1, "security group missing")
    sg = groups[0]
    require(
        sg["OwnerId"] == ACCOUNT
        and sg["GroupId"] == r["security_group_id"]
        and sg["GroupName"] == NAME
        and sg["VpcId"] == r["vpc_id"]
        and {t["Key"]: t["Value"] for t in sg.get("Tags", [])}.get(TAG)
        == admission["nonce"],
        "unowned security group",
    )
    subnets = ec2.describe_subnets(SubnetIds=r["subnet_ids"])["Subnets"]
    require(
        sorted(s["SubnetId"] for s in subnets) == sorted(r["subnet_ids"])
        and all(s["VpcId"] == r["vpc_id"] and s["OwnerId"] == ACCOUNT for s in subnets)
        and len({s["AvailabilityZone"] for s in subnets}) == 3,
        "subnet association mismatch",
    )
    return wg, ns


def rules(ec2, group):
    response = ec2.describe_security_group_rules(
        Filters=[{"Name": "group-id", "Values": [group]}]
    )
    require(not response.get("NextToken"), "unexpected ingress pagination")
    return response["SecurityGroupRules"]


def owned_rule(rule, admission):
    return (
        rule.get("GroupId") == admission["resources"]["security_group_id"]
        and rule.get("GroupOwnerId") == ACCOUNT
        and rule.get("IsEgress") is False
        and rule.get("IpProtocol") == "tcp"
        and rule.get("FromPort") == rule.get("ToPort") == 5439
        and rule.get("CidrIpv4") == admission["runner_cidr"]
        and rule.get("Description") == "adbc-q-" + admission["nonce"]
        and {t["Key"]: t["Value"] for t in rule.get("Tags", [])}.get(TAG)
        == admission["nonce"]
    )


def probe_args(kind, paths, interpreter, admission):
    item = admission["artifacts"][kind]
    return [
        str(Path(interpreter).absolute()),
        "-I",
        "-B",
        str(Path(__file__).with_name("pgwire_qualification_probe.py").resolve()),
        str(Path(paths[kind]["root"]).resolve()),
        kind,
        str(Path(paths[kind]["artifact"]).resolve()),
        item["sha256"],
        item["installed_tree_sha256"],
    ]


def verify_local(admission, paths, interpreter):
    libraries = preflight(installed_manifest(admission), paths)
    for kind, library in libraries.items():
        print(
            "Qualification stage: " + kind + " installed entrypoint probe.", flush=True
        )
        report = json.loads(
            private_process(
                [*probe_args(kind, paths, interpreter, admission), "load-only", "-"]
            )
        )
        verify_probe(
            report,
            package_root=paths[kind]["root"],
            library=library,
            wheel=kind == "python_wheel",
        )
    return libraries


STAGES = (
    "START",
    "PREPARED",
    "SDK",
    "ADMISSION_WAIT",
    "ADMISSION_VALIDATE",
    "GITHUB_VERIFY",
    "INSTALLED_VERIFY",
    "IDENTITY",
    "RESOURCE_VERIFY",
    "CLAIM",
    "INGRESS",
    "C_CREDENTIALS",
    "C_PAYLOAD",
    "PYTHON_CREDENTIALS",
    "PYTHON_PAYLOAD",
    "INGRESS_CLEANUP",
    "COMPLETE",
)


class ConsumerReceipt:
    def __init__(self, path=None):
        require(
            path is None or not path.exists(),
            "existing consumer receipt requires recovery, never retry",
        )
        self.path = path
        self.data = {
            "version": 3,
            "status": "failed-or-interrupted",
            "stage": "START",
            "first_failure": None,
            "source_sha": None,
            "run_id": None,
            "operation_nonce": None,
            "admission_sha256": None,
            "claim": "NOT_ATTEMPTED",
            "owned_ingress": "NOT_ATTEMPTED",
            "installed_entrypoints_verified": False,
            "payloads": {k: "NOT_STARTED" for k in KINDS},
            "external_teardown": "required",
        }
        self.save()

    def save(self):
        if self.path is not None:
            temporary = self.path.with_suffix(".tmp")
            with temporary.open("w") as stream:
                json.dump(self.data, stream, sort_keys=True)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self.path)

    def mark(self, stage):
        require(stage in STAGES, "invalid consumer stage")
        self.data["stage"] = stage
        self.save()

    def fail(self, error):
        if self.data["first_failure"] is not None:
            return
        stage = self.data["stage"]
        response = getattr(error, "response", {})
        details = response.get("Error", {}) if type(response) is dict else {}
        code = details.get("Code") if type(details) is dict else None
        expired = isinstance(error, Rejected) and str(error) in (
            "admission deadline reached",
            "query deadline reached",
            "query deadline reached after credentials",
            "insufficient query window",
        )
        if stage == "CLAIM":
            reason = (
                "DEADLINE"
                if expired
                else "CLAIM_ACCESS_DENIED"
                if code in ("AccessDenied", "AccessDeniedException")
                else "CLAIM_ALREADY_SPENT"
                if code == "ParameterAlreadyExists"
                else "CLAIM_UNCERTAIN"
            )
            self.data["claim"] = (
                "DENIED_NO_RETRY"
                if reason == "CLAIM_ACCESS_DENIED"
                else "SPENT_OR_UNKNOWN"
            )
        elif expired:
            reason = "DEADLINE"
        elif stage.endswith("PAYLOAD"):
            reason = "PAYLOAD_FAILED_OR_INTERRUPTED"
        elif stage == "INGRESS_CLEANUP":
            reason = "INGRESS_CLEANUP_FAILED"
        elif isinstance(error, Rejected):
            reason = {
                "operation nonce mismatch": "OPERATION_MISMATCH",
                "source/run mismatch": "SOURCE_RUN_MISMATCH",
                "query deadline reached": "DEADLINE",
                "insufficient query window": "DEADLINE",
            }.get(str(error), "VERIFICATION_REJECTED")
        else:
            reason = "SDK_OR_INTERRUPTED"
        self.data["first_failure"] = {"stage": stage, "reason": reason}
        self.save()


def execute(
    admission,
    paths,
    interpreter,
    c_client,
    *,
    aws,
    github,
    nonce,
    clock=time.time,
    invoke=client,
    receipt=None,
):
    receipt = receipt if receipt is not None else ConsumerReceipt()
    try:
        result = _execute(
            admission,
            paths,
            interpreter,
            c_client,
            aws=aws,
            github=github,
            nonce=nonce,
            clock=clock,
            invoke=invoke,
            receipt=receipt,
        )
        receipt.data["status"] = "passed"
        receipt.mark("COMPLETE")
        return result
    except BaseException as error:
        receipt.fail(error)
        raise


def _execute(
    admission,
    paths,
    interpreter,
    c_client,
    *,
    aws,
    github,
    nonce,
    receipt,
    clock=time.time,
    invoke=client,
):
    """Internal injectable boundary. Production main supplies authenticated clients."""
    source, run = github.context()
    receipt.data["source_sha"] = (
        source if type(source) is str and COMMIT.fullmatch(source) else None
    )
    receipt.data["run_id"] = run if type(run) is int and run > 0 else None
    admission_name(nonce)
    receipt.data["operation_nonce"] = nonce
    receipt.data["admission_sha256"] = digest(admission)
    receipt.mark("ADMISSION_VALIDATE")
    validate(admission, source=source, run=run, now=int(clock()), nonce=nonce)
    receipt.mark("GITHUB_VERIFY")
    github.verify(admission)
    receipt.mark("INSTALLED_VERIFY")
    libraries = verify_local(admission, paths, interpreter)
    require(
        file_sha(c_client) == admission["c_client_sha256"],
        "C client changed after preflight",
    )
    receipt.data["installed_entrypoints_verified"] = True
    receipt.mark("IDENTITY")
    identity(aws["sts"])
    receipt.mark("RESOURCE_VERIFY")
    resources(admission, aws["redshift-serverless"], aws["ec2"])
    validate(admission, source=source, run=run, now=int(clock()), nonce=nonce)
    # Atomic create; a lost reply is consumed/unknown. There is NO retry/readback
    # path that authorizes paid work, even if a claim happens to match our bytes.
    receipt.data["claim"] = "ATTEMPTED_UNKNOWN"
    receipt.mark("CLAIM")
    require(clock() < admission["window"]["admit_before"], "admission deadline reached")
    aws["ssm"].put_parameter(
        Name=claim_name(nonce, run),
        Value=json.dumps(
            {"admission_sha256": digest(admission), "run_id": run}, sort_keys=True
        ),
        Type="String",
        Tier="Standard",
        Overwrite=False,
    )
    receipt.data["claim"] = "CREATED"
    receipt.mark("INGRESS")
    ec2, redshift = aws["ec2"], aws["redshift-serverless"]
    group = admission["resources"]["security_group_id"]
    attempted_ingress = False
    failed = False
    removed = False
    try:
        require(
            clock() < admission["window"]["query_deadline"] - 180,
            "insufficient query window",
        )
        # Reject all existing inbound rules; this is a NEW dedicated group.
        require(
            not any(not r["IsEgress"] for r in rules(ec2, group)),
            "pre-existing ingress",
        )
        attempted_ingress = True
        receipt.data["owned_ingress"] = "ATTEMPTED_UNKNOWN"
        receipt.save()
        require(
            clock() < admission["window"]["query_deadline"] - 180,
            "insufficient query window",
        )
        response = ec2.authorize_security_group_ingress(
            GroupId=group,
            IpPermissions=[
                {
                    "IpProtocol": "tcp",
                    "FromPort": 5439,
                    "ToPort": 5439,
                    "IpRanges": [
                        {
                            "CidrIp": admission["runner_cidr"],
                            "Description": "adbc-q-" + admission["nonce"],
                        }
                    ],
                }
            ],
            TagSpecifications=[
                {
                    "ResourceType": "security-group-rule",
                    "Tags": [{"Key": TAG, "Value": admission["nonce"]}],
                }
            ],
        )
        require(
            response.get("Return") is True
            and len(response.get("SecurityGroupRules", [])) == 1
            and owned_rule(response["SecurityGroupRules"][0], admission),
            "unknown ingress response",
        )
        observed = [r for r in rules(ec2, group) if not r["IsEgress"]]
        require(
            len(observed) == 1 and owned_rule(observed[0], admission),
            "ingress observation mismatch",
        )
        # Import only the already inventoried installed wheel, never source auth.
        import adbc_driver_redshift.iam as iam

        for index, kind in enumerate(KINDS):
            require(
                clock() < admission["window"]["query_deadline"] - 65,
                "query deadline reached",
            )
            receipt.mark("C_CREDENTIALS" if index == 0 else "PYTHON_CREDENTIALS")
            require(
                clock() < admission["window"]["query_deadline"] - 65,
                "query deadline reached",
            )
            response = iam.fetch_serverless_credentials(
                redshift,
                workgroup_name=NAME,
                db_name="adbc_qualification",
                duration_seconds=900,
            )
            expiry = response["expiration"]
            require(
                isinstance(expiry, datetime)
                and expiry.tzinfo is not None
                and admission["window"]["query_deadline"]
                < expiry.timestamp()
                <= clock() + 900,
                "credential expiry mismatch",
            )
            require(
                response["dbUser"] == "IAMR:adbc-isolated-qualification-ci",
                "unexpected database identity",
            )
            uri = iam.build_uri(
                HOST,
                "adbc_qualification",
                response["dbUser"],
                response["dbPassword"],
                port=5439,
                sslmode="verify-full",
                extra_params={"sslrootcert": str(paths["ca"]), "connect_timeout": "10"},
            )
            validate_uri(uri, installed_manifest(admission), paths["ca"])
            fixture = "adbc_q_" + str(run) + str(index)
            argv = (
                [
                    str(Path(c_client).absolute()),
                    str(libraries[kind]),
                    fixture,
                    "--redshift",
                ]
                if index == 0
                else [
                    *probe_args(kind, paths, interpreter, admission),
                    "--redshift",
                    fixture,
                ]
            )
            receipt.data["payloads"][kind] = "STARTED_UNKNOWN"
            receipt.mark("C_PAYLOAD" if index == 0 else "PYTHON_PAYLOAD")
            # Persistence may block. Budget the child only after the durable
            # attempted mark; a skipped invocation is known not to have started.
            remaining = min(60, admission["window"]["query_deadline"] - clock() - 1)
            if remaining <= 0:
                receipt.data["payloads"][kind] = "NOT_STARTED"
                receipt.save()
                raise Rejected("query deadline reached after credentials")
            invoke(argv, uri, seconds=remaining)
            receipt.data["payloads"][kind] = "PASSED"
            receipt.save()
            print(
                "Qualification stage: " + kind + " private payload passed.", flush=True
            )
            uri = response = None
    except BaseException as error:
        receipt.fail(error)
        failed = True
    finally:
        if attempted_ingress:
            receipt.mark("INGRESS_CLEANUP")
            # Lost authorize replies are recovered only by independent exact
            # ownership observation. Never revoke a CIDR, guessed ID or other rule.
            try:
                owned = [r for r in rules(ec2, group) if owned_rule(r, admission)]
                require(len(owned) <= 1, "ambiguous owned ingress")
                for rule in owned:
                    ec2.revoke_security_group_ingress(
                        GroupId=group,
                        SecurityGroupRuleIds=[rule["SecurityGroupRuleId"]],
                    )
                require(
                    not any(owned_rule(r, admission) for r in rules(ec2, group)),
                    "owned ingress remains",
                )
                removed = True
                receipt.data["owned_ingress"] = "removed"
                receipt.save()
            except BaseException as error:
                receipt.data["owned_ingress"] = "UNKNOWN"
                receipt.fail(error)
                receipt.save()
                failed = True
    require(
        not failed and removed and clock() < admission["window"]["teardown_deadline"],
        "Live qualification failed; host cleanup observation required",
    )
    return {
        "version": 2,
        "run_id": run,
        "source_sha": source,
        "admission_sha256": digest(admission),
        "installed_payloads": "passed",
        "owned_ingress": "removed",
        "external_teardown": "required",
        "cleanup": admission["cleanup"],
    }


def clients():
    """Action-issued STS session only, no ambient profiles/endpoints/retries."""
    for name in os.environ:
        require(
            not name.startswith("AWS_ENDPOINT_URL")
            and name
            not in {
                "AWS_PROFILE",
                "AWS_DEFAULT_PROFILE",
                "AWS_CONFIG_FILE",
                "AWS_SHARED_CREDENTIALS_FILE",
                "AWS_CA_BUNDLE",
                "BOTO_CONFIG",
            },
            "ambient AWS configuration rejected",
        )
    import boto3
    from botocore.config import Config

    session = boto3.Session(
        aws_access_key_id=os.environ["AWS_ACCESS_KEY_ID"],
        aws_secret_access_key=os.environ["AWS_SECRET_ACCESS_KEY"],
        aws_session_token=os.environ["AWS_SESSION_TOKEN"],
        region_name=REGION,
    )
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
            endpoint_url=f"https://{service}.{REGION}.amazonaws.com",
            config=config,
            verify=True,
        )
        for service in ("sts", "ssm", "redshift-serverless", "ec2")
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--nonce", required=True)
    args = parser.parse_args()
    try:
        receipt = ConsumerReceipt(args.output)
        admission_name(args.nonce)
        receipt.data["operation_nonce"] = args.nonce
        receipt.mark("PREPARED")
        from pgwire_qualification_prepare import GitHub

        github = GitHub(os.environ["GH_TOKEN"])
        source, run = github.context()
        require(
            type(source) is str
            and COMMIT.fullmatch(source)
            and type(run) is int
            and run > 0,
            "invalid consumer context",
        )
        receipt.data["source_sha"], receipt.data["run_id"] = source, run
        receipt.save()
        paths = load(args.paths)
        prepared = load(args.paths.with_name("preflight.json"))
        require(prepared["nonce"] == args.nonce, "operation nonce mismatch")
        parameter_name = admission_name(args.nonce)
        github.verify(prepared)
        verify_local(prepared, paths, sys.executable)
        require(
            file_sha(paths["c_client"]) == prepared["c_client_sha256"],
            "prepared C client changed",
        )
        receipt.data["installed_entrypoints_verified"] = True
        receipt.mark("SDK")
        aws = clients()
        identity(aws["sts"])
        receipt.mark("ADMISSION_WAIT")
        deadline = time.monotonic() + 1200
        while True:
            try:
                parameter = aws["ssm"].get_parameter(
                    Name=parameter_name, WithDecryption=False
                )["Parameter"]
                break
            except aws["ssm"].exceptions.ParameterNotFound:
                require(
                    time.monotonic() < deadline, "host admission publication timed out"
                )
                time.sleep(5)
        require(
            parameter["Name"] == parameter_name
            and parameter["Type"] == "String"
            and parameter["Version"] == 1,
            "admission is not immutable version one",
        )
        # Use the duplicate-field rejecting loader without persisting admission.
        from pgwire_qualification_prepare import parse_json

        admission = parse_json(parameter["Value"])
        receipt.mark("ADMISSION_VALIDATE")
        receipt.data["admission_sha256"] = digest(admission)
        execute(
            admission,
            paths,
            sys.executable,
            paths["c_client"],
            aws=aws,
            github=github,
            nonce=args.nonce,
            receipt=receipt,
            invoke=client,
            clock=time.time,
        )
        print(
            "Installed Redshift qualification passed; "
            "external cleanup observation and teardown REQUIRED."
        )
        return 0
    except BaseException as error:
        if "receipt" in locals():
            receipt.fail(error)
        print(
            "Qualification failed closed; claim may be spent; "
            "external host observation REQUIRED.",
            file=sys.stderr,
        )
        return 1


if __name__ == "__main__":
    sys.exit(main())
