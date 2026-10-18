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

"""Policy-enforcing operation contract tests; no AWS or network calls."""

import copy
import fnmatch
import importlib.util
import io
import json
import socket
import sys
import tempfile
import types
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import Mock, patch

import pgwire_qualification_live as live
import pgwire_qualification_prepare as prepare
import test_pgwire_qualification_live as support
from botocore.exceptions import ClientError
from pgwire_qualification_host import Host
from pgwire_qualification_manifest import Rejected
from pgwire_qualification_policy import (
    LEGACY_PARAMETER,
    TEMPLATE,
    admission_name,
    claim_name,
    host_attempt_name,
    initial_policy,
    runtime_policy,
)
from test_pgwire_qualification_live import AWS, NONCE, NOW, RUN, SOURCE, admission


def decision(policy, action, resource, context):
    """Evaluate the asset's supported IAM operators, with deny precedence."""
    allowed = False
    for statement in policy["Statement"]:
        if not any(fnmatch.fnmatchcase(action, item) for item in statement["Action"]):
            continue
        targets = statement["Resource"]
        targets = [targets] if type(targets) is str else targets
        if not any(fnmatch.fnmatchcase(resource, target) for target in targets):
            continue
        matches = True
        for operator, conditions in statement.get("Condition", {}).items():
            for key, expected in conditions.items():
                actual = context.get(key)
                if operator == "StringEquals":
                    matches &= actual in (
                        expected if type(expected) is list else [expected]
                    )
                elif operator == "DateLessThan":
                    matches &= actual is not None and actual < expected
                else:
                    raise AssertionError("untested policy operator")
        if matches:
            if statement["Effect"] == "Deny":
                return "explicitDeny"
            allowed = True
    return "allowed" if allowed else "implicitDeny"


class PolicySSM(AWS):
    def __init__(self, a, *, overwrite=None, old=False, parameters=None):
        super().__init__(a)
        self.parameters = (
            parameters
            if parameters is not None
            else {LEGACY_PARAMETER: "retained immutable version1"}
        )
        self.policy = runtime_policy(a)
        self.overwrite = overwrite
        if old:
            next(
                s for s in self.policy["Statement"] if s["Sid"] == "ConsumeExactClaim"
            )["Condition"]["StringEquals"] = {"ssm:Overwrite": "false"}

    def authorize(self, action, name, overwrite=False):
        context = {
            "aws:CurrentTime": datetime.fromtimestamp(NOW, timezone.utc).strftime(
                "%Y-%m-%dT%H:%M:%SZ"
            )
        }
        if overwrite:
            context["ssm:Overwrite"] = "true"
        elif self.overwrite is not None:
            context["ssm:Overwrite"] = self.overwrite
        resource = f"arn:aws:ssm:{live.REGION}:{live.ACCOUNT}:parameter" + name
        if decision(self.policy, action, resource, context) != "allowed":
            raise ClientError(
                {"Error": {"Code": "AccessDenied", "Message": "PRIVATE policy denial"}},
                action,
            )

    def get_parameter(self, **kwargs):
        name = kwargs["Name"]
        assert kwargs["WithDecryption"] is False
        self.authorize("ssm:GetParameter", name)
        return {
            "Parameter": {
                "Name": name,
                "Type": "String",
                "Version": 1,
                "Value": self.parameters[name],
            }
        }

    def put_parameter(self, **kwargs):
        name = kwargs["Name"]
        self.events.append("claim-attempt")
        self.authorize("ssm:PutParameter", name, kwargs["Overwrite"])
        assert (
            kwargs["Overwrite"] is False
            and kwargs["Type"] == "String"
            and kwargs["Tier"] == "Standard"
        )
        if name in self.parameters:
            raise ClientError(
                {
                    "Error": {
                        "Code": "ParameterAlreadyExists",
                        "Message": "PRIVATE duplicate",
                    }
                },
                "PutParameter",
            )
        value = json.loads(kwargs["Value"])
        assert value == {
            "admission_sha256": live.digest(self.a),
            "run_id": self.a["run_id"],
        }
        self.parameters[name] = kwargs["Value"]
        self.claims.add(name)
        self.events.append("claim")
        if self.fail == "claim-uncertain":
            raise RuntimeError("PRIVATE lost reply")
        if self.fail == "claim-interrupted":
            raise KeyboardInterrupt("PRIVATE interruption")
        return {"Version": 1}


class PolicyTest(unittest.TestCase):
    def test_documented_pattern_missing_false_true_and_exact_scope(self):
        a = admission()
        sdk = PolicySSM(a)
        name = claim_name(NONCE, RUN)
        resource = f"arn:aws:ssm:{live.REGION}:{live.ACCOUNT}:parameter" + name
        for context, expected in (
            ({}, "allowed"),
            ({"ssm:Overwrite": "false"}, "allowed"),
            ({"ssm:Overwrite": "true"}, "explicitDeny"),
        ):
            context["aws:CurrentTime"] = "2026-01-01T00:00:00Z"
            self.assertEqual(
                decision(sdk.policy, "ssm:PutParameter", resource, context), expected
            )
        with self.assertRaises(ClientError):
            sdk.put_parameter(
                Name=name,
                Value="unused",
                Overwrite=True,
                Type="String",
                Tier="Standard",
            )
        self.assertNotIn(name, sdk.parameters)
        old = PolicySSM(a, old=True)
        self.assertEqual(
            decision(
                old.policy,
                "ssm:PutParameter",
                resource,
                {"aws:CurrentTime": "2026-01-01T00:00:00Z"},
            ),
            "implicitDeny",
        )
        for wrong in (
            LEGACY_PARAMETER,
            admission_name(NONCE),
            claim_name("e" * 64, RUN),
            claim_name(NONCE, RUN + 1),
            host_attempt_name(NONCE),
        ):
            with self.assertRaises(ClientError):
                sdk.authorize("ssm:PutParameter", wrong)
        with self.assertRaises(ClientError):
            sdk.authorize("ssm:GetParameter", LEGACY_PARAMETER)
        expired = copy.deepcopy(a)
        expired["window"]["teardown_deadline"] = NOW - 1
        sdk.policy = runtime_policy(expired)
        with self.assertRaises(ClientError):
            sdk.authorize("ssm:PutParameter", name)

    def test_exact_retained_actions_and_initial_read(self):
        policy = runtime_policy(admission())
        self.assertEqual(
            {action for s in policy["Statement"] for action in s["Action"]},
            {
                "sts:GetCallerIdentity",
                "ssm:GetParameter",
                "ssm:PutParameter",
                "ec2:DescribeSecurityGroups",
                "ec2:DescribeSecurityGroupRules",
                "ec2:DescribeSubnets",
                "ec2:AuthorizeSecurityGroupIngress",
                "ec2:RevokeSecurityGroupIngress",
                "ec2:CreateTags",
                "redshift-serverless:GetUsageLimit",
                "redshift-serverless:GetWorkgroup",
                "redshift-serverless:GetNamespace",
                "redshift-serverless:ListTagsForResource",
                "redshift-serverless:GetCredentials",
            },
        )
        self.assertEqual(
            initial_policy(NONCE)["Statement"][1]["Resource"],
            f"arn:aws:ssm:{live.REGION}:{live.ACCOUNT}:parameter"
            + admission_name(NONCE),
        )
        for invalid in ("", "D" * 64, "d" * 63, "../admission", NONCE + "/claim"):
            with self.assertRaises(Rejected):
                admission_name(invalid)


class HostReservationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.a = admission()
        self.sdk = AWS(self.a)
        self.sdk.ns_exists = self.sdk.wg_exists = self.sdk.sg_exists = False
        self.sdk.get_caller_identity = lambda: {
            "Account": live.ACCOUNT,
            "Arn": f"arn:aws:sts::{live.ACCOUNT}:assumed-role/adbc-provisioner/host",
        }
        self.tick = NOW
        self.github = Mock()
        self.github.get.side_effect = lambda *args, **kwargs: {
            "path": live.WORKFLOW,
            "head_sha": SOURCE,
            "head_branch": live.REF.removeprefix("refs/heads/"),
            "event": "workflow_dispatch",
            "run_attempt": 1,
            "repository": {"full_name": live.REPOSITORY},
            "status": "completed" if "admission" in self.sdk.events else "in_progress",
            "conclusion": "success",
        }
        self.network = patch.object(
            socket, "socket", side_effect=AssertionError("NETWORK FORBIDDEN")
        )
        self.network.start()
        self.addCleanup(self.network.stop)
        self.index = 0

    def host(self):
        self.index += 1
        host = Host(
            {
                k: self.sdk
                for k in (
                    "sts",
                    "ssm",
                    "ec2",
                    "redshift-serverless",
                    "redshift-data",
                    "iam",
                )
            },
            self.github,
            self.root / (str(self.index) + ".json"),
            clock=lambda: self.tick,
            pause=lambda seconds: setattr(self, "tick", self.tick + seconds),
        )
        # Authenticated artifact boundary is fake; the lifecycle is real.
        host.preflight = Mock(
            return_value={
                k: self.a[k]
                for k in (
                    "nonce",
                    "source_sha",
                    "run_id",
                    "artifacts",
                    "ca_sha256",
                    "runner_cidr",
                    "c_client_sha256",
                    "review",
                )
            }
        )
        return host

    def run_host(self, host, **kwargs):
        try:
            host.run(
                preflight_id=1,
                run=RUN,
                source=SOURCE,
                nonce=NONCE,
                policy_template=kwargs.get("template", TEMPLATE),
                policy_sha256=kwargs.get("sha", live.file_sha(TEMPLATE)),
            )
            return True
        except Rejected:
            return False

    def assert_no_provision(self):
        self.assertFalse(
            set(self.sdk.events)
            & {
                "create-namespace",
                "create-group",
                "create-workgroup",
                "create-usage",
                "install-runtime-policy",
                "host-sql",
                "admission",
            }
        )

    def test_success_retains_reservation_and_new_receipt_cannot_reenter(self):
        host = self.host()
        self.assertTrue(self.run_host(host))
        self.assertEqual(host.state["host_attempt"], "CREATED")
        self.assertIn(host_attempt_name(NONCE), self.sdk.claims)
        events = list(self.sdk.events)
        second = self.host()
        self.assertFalse(self.run_host(second))
        self.assertEqual(
            second.state["first_failure"],
            {"stage": "OPERATION_INVENTORY", "reason": "OPERATION_SPENT_OR_UNKNOWN"},
        )
        self.assertEqual(self.sdk.events[len(events) :], ["operation-metadata"])
        self.assertEqual(second.state["external_teardown"], "not-started")

    def test_lost_reply_and_prepublication_failure_are_spent_with_new_receipt(self):
        original = self.sdk.put_parameter
        for mode in ("lost", "prepublication"):
            with self.subTest(mode=mode):
                self.sdk.claims.clear()
                self.sdk.events.clear()

                def put(**kwargs):
                    result = original(**kwargs)
                    if kwargs["Name"] == host_attempt_name(NONCE) and mode == "lost":
                        raise RuntimeError("PRIVATE lost reply")
                    return result

                self.sdk.put_parameter = put
                self.sdk.fail = "create-namespace" if mode == "prepublication" else None
                first = self.host()
                self.assertFalse(self.run_host(first))
                primary = first.state["first_failure"]
                self.assertIn(host_attempt_name(NONCE), self.sdk.claims)
                self.assertNotIn("PRIVATE", first.path.read_text())
                self.sdk.fail = None
                self.sdk.events.clear()
                second = self.host()
                self.assertFalse(self.run_host(second))
                self.assert_no_provision()
                self.assertEqual(first.state["first_failure"], primary)
                self.assertEqual(self.sdk.events, ["operation-metadata"])

    def test_concurrent_empty_snapshots_only_one_reservation_wins(self):
        original = self.sdk.put_parameter
        loser = self.host()
        winner = self.host()
        empty = self.sdk.describe_parameters
        # Both invocations observed empty metadata before the atomic write.
        self.sdk.describe_parameters = lambda **kwargs: {"Parameters": []}

        def put(**kwargs):
            if (
                kwargs["Name"] == host_attempt_name(NONCE)
                and kwargs["Name"] not in self.sdk.claims
            ):
                result = original(**kwargs)
                self.assertFalse(self.run_host(loser))
                self.assertEqual(loser.state["host_attempt"], "ATTEMPTED_UNKNOWN")
                self.assertEqual(loser.state["external_teardown"], "not-started")
                self.assert_no_provision()
                return result
            return original(**kwargs)

        self.sdk.put_parameter = put
        self.assertTrue(self.run_host(winner))
        self.sdk.describe_parameters = empty
        self.assertEqual(self.sdk.events.count("create-namespace"), 1)
        self.assertEqual(self.sdk.events.count("admission"), 1)

    def test_metadata_incomplete_existing_or_malformed_never_reserves(self):
        for response in (
            {},
            {"Parameters": None},
            {"Parameters": [], "NextToken": ""},
            {"Parameters": [], "NextToken": "more"},
            {"Parameters": [{"Name": host_attempt_name(NONCE)}]},
            {"Parameters": [{"Name": admission_name(NONCE)}]},
            {"Parameters": [{"Name": claim_name(NONCE, RUN)}]},
        ):
            self.sdk.events.clear()
            self.sdk.describe_parameters = Mock(return_value=response)
            host = self.host()
            self.assertFalse(self.run_host(host))
            self.assert_no_provision()
            self.assertNotIn("host-reservation", self.sdk.events)
            self.assertEqual(
                host.state["first_failure"]["reason"], "OPERATION_SPENT_OR_UNKNOWN"
            )

    def test_template_nonce_or_initial_policy_rejects_before_reservation(self):
        changed = self.root / "changed.json"
        changed.write_text(TEMPLATE.read_text() + " ")
        for mode in ("template", "nonce", "initial"):
            self.sdk.events.clear()
            host = self.host()
            if mode == "nonce":
                host.preflight.return_value["nonce"] = "e" * 64
            if mode == "initial":
                self.sdk.get_role_policy = Mock(
                    return_value={"PolicyDocument": initial_policy("e" * 64)}
                )
            self.assertFalse(
                self.run_host(
                    host,
                    template=changed if mode == "template" else TEMPLATE,
                    sha=live.file_sha(changed)
                    if mode == "template"
                    else live.file_sha(TEMPLATE),
                )
            )
            self.assert_no_provision()
            self.assertNotIn("host-reservation", self.sdk.events)

    def test_late_metadata_and_reservation_reply_never_provision(self):
        original_read, original_write = (
            self.sdk.describe_parameters,
            self.sdk.put_parameter,
        )
        for boundary in ("metadata", "reservation"):
            self.tick = NOW
            self.sdk.events.clear()
            self.sdk.claims.clear()

            def read(**kwargs):
                result = original_read(**kwargs)
                if boundary == "metadata":
                    self.tick += 3601
                return result

            def write(**kwargs):
                result = original_write(**kwargs)
                if boundary == "reservation":
                    self.tick += 3601
                return result

            self.sdk.describe_parameters, self.sdk.put_parameter = read, write
            host = self.host()
            self.assertFalse(self.run_host(host))
            self.assert_no_provision()
            self.assertEqual(host.state["first_failure"]["reason"], "DEADLINE")
            self.assertEqual(
                host_attempt_name(NONCE) in self.sdk.claims, boundary == "reservation"
            )


@unittest.skipUnless(
    sys.version_info >= (3, 12), "requires actual data-filter preparation"
)
class PreparedClaimTest(unittest.TestCase):
    def setUp(self):
        self.flow = support.PrepareFlowTest()
        self.addCleanup(self.flow.doCleanups)
        self.flow.setUp()
        self.prepared = self.flow.prepare()
        paths = json.loads((self.flow.directory / "paths.json").read_text())
        self.paths_file = self.flow.directory / "paths.json"
        self.output = self.flow.directory / "result.json"
        self.a = {**admission(), **self.prepared}
        self.paths = paths
        spec = importlib.util.spec_from_file_location(
            "adbc_driver_redshift.iam",
            self.flow.package_root / "adbc_driver_redshift/iam.py",
        )
        iam = importlib.util.module_from_spec(spec)
        with patch.object(sys, "dont_write_bytecode", True):
            spec.loader.exec_module(iam)
        package = types.ModuleType("adbc_driver_redshift")
        package.iam = iam
        self.modules = patch.dict(
            sys.modules,
            {"adbc_driver_redshift": package, "adbc_driver_redshift.iam": iam},
        )
        self.modules.start()
        self.addCleanup(self.modules.stop)
        self.process_paths = {}

    def consume(self, sdk, *, payload=None, nonce=NONCE):
        if sdk not in self.process_paths:
            directory = self.flow.root / ("consumer-" + str(len(self.process_paths)))
            directory.mkdir()
            self.process_paths[sdk] = directory / "result.json"
        self.output = self.process_paths[sdk]
        sdk.parameters[admission_name(nonce)] = json.dumps(self.a)
        with (
            patch.object(sys, "executable", self.flow.interpreter),
            patch.object(
                sys,
                "argv",
                [
                    "qualification",
                    str(self.paths_file),
                    "--nonce",
                    nonce,
                    "--output",
                    str(self.output),
                ],
            ),
            patch.dict("os.environ", {"GH_TOKEN": "synthetic-token"}),
            patch.object(prepare, "GitHub", return_value=self.flow.github),
            patch.object(
                live,
                "clients",
                return_value={
                    k: sdk for k in ("sts", "ssm", "ec2", "redshift-serverless")
                },
            ),
            patch.object(live.time, "time", return_value=NOW),
            patch.object(
                live,
                "client",
                side_effect=payload
                or (lambda *args, **kwargs: sdk.events.append("private-payload")),
            ),
            patch("sys.stderr", new_callable=io.StringIO) as stderr,
        ):
            status = live.main()
        record = json.loads(self.output.read_text())
        self.assertNotIn("PRIVATE", self.output.read_text())
        self.assertNotIn("PRIVATE", stderr.getvalue())
        self.assertEqual(record["source_sha"], SOURCE)
        self.assertEqual(record["run_id"], RUN)
        self.assertEqual(
            sdk.parameters[LEGACY_PARAMETER], "retained immutable version1"
        )
        return status, record

    def test_old_condition_denies_before_ingress_and_payload(self):
        sdk = PolicySSM(self.a, old=True)
        status, record = self.consume(sdk)
        self.assertEqual(status, 1)
        self.assertEqual(
            record["first_failure"], {"stage": "CLAIM", "reason": "CLAIM_ACCESS_DENIED"}
        )
        self.assertEqual(record["admission_sha256"], live.digest(self.a))
        self.assertTrue(record["installed_entrypoints_verified"])
        self.assertEqual(record["payloads"], {k: "NOT_STARTED" for k in live.KINDS})
        self.assertFalse(
            set(sdk.events) & {"authorize", "credentials", "private-payload"}
        )

    def test_new_policy_missing_and_false_run_installed_payload_boundaries(self):
        for context in (None, "false"):
            with self.subTest(context=context):
                sdk = PolicySSM(self.a, overwrite=context)
                status, record = self.consume(sdk)
                self.assertEqual(status, 0)
                self.assertEqual(record["payloads"], {k: "PASSED" for k in live.KINDS})
                self.assertEqual(sdk.events.count("private-payload"), 2)
                self.assertEqual(record["claim"], "CREATED")
                self.assertIsNone(record["first_failure"])
                # A duplicate uses the same immutable claim and cannot run again.
                status, duplicate = self.consume(sdk)
                self.assertEqual(status, 1)
                self.assertEqual(duplicate, record)
                self.assertEqual(sdk.events.count("private-payload"), 2)
                second = PolicySSM(self.a, parameters=sdk.parameters)
                status, duplicate = self.consume(second)
                self.assertEqual(status, 1)
                self.assertEqual(
                    duplicate["first_failure"]["reason"], "CLAIM_ALREADY_SPENT"
                )
                self.assertNotIn("credentials", second.events)

    def test_lost_or_interrupted_claim_never_reauthorizes(self):
        for failure in ("claim-uncertain", "claim-interrupted"):
            sdk = PolicySSM(self.a)
            sdk.fail = failure
            status, record = self.consume(sdk)
            self.assertEqual(status, 1)
            self.assertEqual(record["first_failure"]["reason"], "CLAIM_UNCERTAIN")
            self.assertFalse(
                set(sdk.events) & {"authorize", "credentials", "private-payload"}
            )
            sdk.fail = None
            status, record = self.consume(sdk)
            self.assertEqual(status, 1)
            self.assertEqual(record["first_failure"]["reason"], "CLAIM_UNCERTAIN")
            second = PolicySSM(self.a, parameters=sdk.parameters)
            status, record = self.consume(second)
            self.assertEqual(status, 1)
            self.assertEqual(record["first_failure"]["reason"], "CLAIM_ALREADY_SPENT")

    def test_payload_primary_failure_survives_cleanup_failure(self):
        sdk = PolicySSM(self.a)
        sdk.fail = "revoke"
        status, record = self.consume(
            sdk, payload=Mock(side_effect=RuntimeError("PRIVATE payload"))
        )
        self.assertEqual(status, 1)
        self.assertEqual(
            record["first_failure"],
            {"stage": "C_PAYLOAD", "reason": "PAYLOAD_FAILED_OR_INTERRUPTED"},
        )
        self.assertEqual(record["payloads"]["c_archive"], "STARTED_UNKNOWN")
        self.assertEqual(record["payloads"]["python_wheel"], "NOT_STARTED")
        self.assertEqual(record["owned_ingress"], "UNKNOWN")

    def test_two_explicit_operations_preserve_each_other_and_legacy_singleton(self):
        first = PolicySSM(self.a)
        self.assertEqual(self.consume(first)[0], 0)
        retained = dict(first.parameters)
        original_receipt = self.output.read_bytes()
        self.a = {**self.a, "nonce": "e" * 64}
        prepared = {**self.prepared, "nonce": self.a["nonce"]}
        self.paths_file.with_name("preflight.json").write_text(json.dumps(prepared))
        second = PolicySSM(self.a, parameters=first.parameters)
        self.assertEqual(self.consume(second, nonce=self.a["nonce"])[0], 0)
        for name, value in retained.items():
            self.assertEqual(second.parameters[name], value)
        self.assertEqual(self.process_paths[first].read_bytes(), original_receipt)
        for sdk, other in ((first, "e" * 64), (second, NONCE)):
            for action, name in (
                ("ssm:GetParameter", admission_name(other)),
                ("ssm:PutParameter", claim_name(other, RUN)),
            ):
                with self.assertRaises(ClientError):
                    sdk.authorize(action, name)
        self.assertIn(claim_name(NONCE, RUN), first.parameters)
        self.assertIn(claim_name("e" * 64, RUN), first.parameters)

    def test_preclaim_failure_and_cleanup_only_failure_have_distinct_receipts(self):
        sdk = PolicySSM(self.a)
        sdk.fail = "subnets"
        status, record = self.consume(sdk)
        self.assertEqual(status, 1)
        self.assertEqual(record["first_failure"]["stage"], "RESOURCE_VERIFY")
        self.assertEqual(record["claim"], "NOT_ATTEMPTED")
        self.assertNotIn("claim-attempt", sdk.events)
        sdk = PolicySSM(self.a)
        sdk.fail = "revoke"
        status, record = self.consume(sdk)
        self.assertEqual(status, 1)
        self.assertEqual(
            record["first_failure"],
            {"stage": "INGRESS_CLEANUP", "reason": "INGRESS_CLEANUP_FAILED"},
        )
        self.assertEqual(record["payloads"], {k: "PASSED" for k in live.KINDS})
        self.assertEqual(record["owned_ingress"], "UNKNOWN")

    def test_wrong_policy_binding_and_expiry_cannot_read_or_consume(self):
        for mode in ("nonce", "run", "cutoff"):
            policy_admission = copy.deepcopy(self.a)
            if mode == "nonce":
                policy_admission["nonce"] = "e" * 64
            elif mode == "run":
                policy_admission["run_id"] += 1
            else:
                policy_admission["window"]["teardown_deadline"] = NOW - 1
            sdk = PolicySSM(self.a)
            sdk.policy = runtime_policy(policy_admission)
            status, record = self.consume(sdk)
            self.assertEqual(status, 1)
            self.assertFalse(
                set(sdk.events) & {"authorize", "credentials", "private-payload"}
            )
            self.assertEqual(
                record["first_failure"]["stage"],
                "CLAIM" if mode == "run" else "ADMISSION_WAIT",
            )

    def test_wrong_admission_source_run_and_nonce_stop_before_claim(self):
        for field, value in (
            ("source_sha", "f" * 40),
            ("run_id", RUN + 1),
            ("nonce", "e" * 64),
        ):
            original = self.a[field]
            self.a[field] = value
            sdk = PolicySSM(self.a)
            status, record = self.consume(sdk)
            self.assertEqual(status, 1)
            self.assertNotIn("claim-attempt", sdk.events)
            self.assertFalse(
                set(sdk.events) & {"authorize", "credentials", "private-payload"}
            )
            self.a[field] = original


if __name__ == "__main__":
    unittest.main()
