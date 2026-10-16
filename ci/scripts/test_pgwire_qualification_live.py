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

"""Fake-only admission, authenticated provenance and finite lifecycle regressions."""

import copy
import importlib.util
import json
import os
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
import test_pgwire_qualification as legacy
from pgwire_qualification_host import OWNER, Host
from pgwire_qualification_manifest import Rejected
from test_pgwire_qualification import NOW, ROOT, RUN, SOURCE, fixture


class NotFound(Exception):
    pass


def admission():
    return {
        "review": {
            "pr": 15,
            "head_sha": SOURCE,
            "tree_sha": "e" * 40,
            "comment_id": 7,
            "comment_sha256": "b" * 64,
            "checks_sha256": "b" * 64,
        },
        "version": 2,
        "nonce": "d" * 64,
        "source_sha": SOURCE,
        "run_id": RUN,
        "artifacts": {
            k: {
                "id": i + 20,
                "run_id": i + 40,
                "sha256": "b" * 64,
                "installed_tree_sha256": "b" * 64,
            }
            for i, k in enumerate(live.KINDS)
        },
        "resources": {
            "workgroup_id": "11111111-1111-1111-1111-111111111111",
            "namespace_id": "22222222-2222-2222-2222-222222222222",
            "security_group_id": "sg-11111111111111111",
            "vpc_id": live.VPC,
            "subnet_ids": live.SUBNETS[:],
            "usage_limit_id": "33333333-3333-3333-3333-333333333333",
        },
        "window": {
            "not_before": NOW - 1,
            "admit_before": NOW + 200,
            "query_deadline": NOW + 800,
            "teardown_deadline": NOW + 3500,
        },
        "cleanup": {"owner": OWNER, "observer": OWNER},
        "ca_sha256": "b" * 64,
        "runner_cidr": "8.8.8.8/32",
        "c_client_sha256": "b" * 64,
    }


class AWS:
    """Stateful AWS fake; all SDK operations record ordering and can lose replies."""

    def __init__(self, a):
        self.a = a
        self.events = []
        self.fail = None
        self.claims = set()
        self.ingress = []
        self.ns_exists = self.wg_exists = self.sg_exists = self.usage_exists = True
        self.policy = None
        self.inline_inventory = {"PolicyNames": ["adbc-qualification-initial"]}
        self.attached_inventory = {"AttachedPolicies": []}
        self.initial_policy = {
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
                    "Resource": f"arn:aws:ssm:{live.REGION}:{live.ACCOUNT}:parameter"
                    + live.PARAMETER,
                },
            ],
        }
        self.exceptions = types.SimpleNamespace(
            ResourceNotFoundException=NotFound, NoSuchEntityException=NotFound
        )

    def event(self, name):
        self.events.append(name)
        if self.fail == name:
            raise RuntimeError("SECRET SDK ERROR should never escape")

    def get_caller_identity(self):
        self.event("identity")
        return {
            "Account": live.ACCOUNT,
            "Arn": (
                f"arn:aws:sts::{live.ACCOUNT}:assumed-role/"
                "adbc-isolated-qualification-ci/run"
            ),
        }

    def get_workgroup(self, **kwargs):
        self.event("workgroup")
        if not self.wg_exists:
            raise NotFound()
        r = self.a["resources"]
        return {
            "workgroup": {
                "workgroupId": r["workgroup_id"],
                "workgroupArn": live.arn("workgroup", r["workgroup_id"]),
                "workgroupName": live.NAME,
                "namespaceName": live.NAME,
                "status": "AVAILABLE",
                "creationDate": datetime.fromtimestamp(NOW, timezone.utc),
                "baseCapacity": 4,
                "maxCapacity": 4,
                "extraComputeForAutomaticOptimization": False,
                "pricePerformanceTarget": {"status": "DISABLED"},
                "securityGroupIds": [r["security_group_id"]],
                "subnetIds": r["subnet_ids"],
                "publiclyAccessible": True,
                "endpoint": {"address": live.HOST, "port": 5439},
                "configParameters": [
                    {"parameterKey": "require_ssl", "parameterValue": "true"},
                    {
                        "parameterKey": "max_query_execution_time",
                        "parameterValue": "10",
                    },
                ],
            }
        }

    def get_namespace(self, **kwargs):
        self.event("namespace")
        if not self.ns_exists:
            raise NotFound()
        r = self.a["resources"]
        return {
            "namespace": {
                "namespaceId": r["namespace_id"],
                "namespaceArn": live.arn("namespace", r["namespace_id"]),
                "namespaceName": live.NAME,
                "dbName": "adbc_qualification",
                "status": "AVAILABLE",
                "creationDate": datetime.fromtimestamp(NOW, timezone.utc),
                "adminPasswordSecretArn": (
                    f"arn:aws:secretsmanager:{live.REGION}:{live.ACCOUNT}:"
                    "secret:managed-fake"
                ),
            }
        }

    def list_tags_for_resource(self, **kwargs):
        self.event("resource-tags")
        return {"tags": [{"key": live.TAG, "value": self.a["nonce"]}]}

    def get_usage_limit(self, **kwargs):
        self.event("usage")
        if not self.usage_exists:
            raise NotFound()
        r = self.a["resources"]
        return {
            "usageLimit": {
                "usageLimitId": r["usage_limit_id"],
                "resourceArn": live.arn("workgroup", r["workgroup_id"]),
                "usageType": "serverless-compute",
                "period": "daily",
                "amount": 4,
                "breachAction": "deactivate",
            }
        }

    def describe_security_groups(self, **kwargs):
        self.event("group")
        r = self.a["resources"]
        return {
            "SecurityGroups": [
                {
                    "OwnerId": live.ACCOUNT,
                    "GroupId": r["security_group_id"],
                    "GroupName": live.NAME,
                    "VpcId": live.VPC,
                    "Tags": [{"Key": live.TAG, "Value": self.a["nonce"]}],
                }
            ]
            if self.sg_exists
            else []
        }

    def describe_subnets(self, **kwargs):
        self.event("subnets")
        return {
            "Subnets": [
                {
                    "SubnetId": s,
                    "VpcId": live.VPC,
                    "OwnerId": live.ACCOUNT,
                    "AvailabilityZone": str(i),
                }
                for i, s in enumerate(live.SUBNETS)
            ]
        }

    def put_parameter(self, **kwargs):
        self.events.append(
            "claim" if kwargs["Name"].startswith(live.CLAIMS) else "admission"
        )
        assert (
            kwargs["Overwrite"] is False
            and kwargs["Type"] == "String"
            and kwargs["Tier"] == "Standard"
        )
        if kwargs["Name"] in self.claims:
            raise RuntimeError("duplicate SECRET")
        self.claims.add(kwargs["Name"])
        if self.fail == "claim-uncertain":
            raise RuntimeError("lost SECRET reply")
        return {"Version": 1}

    def describe_security_group_rules(self, **kwargs):
        self.event("rules")
        return {"SecurityGroupRules": copy.deepcopy(self.ingress)}

    def authorize_security_group_ingress(self, **kwargs):
        self.events.append("authorize")
        rule = {
            "GroupId": self.a["resources"]["security_group_id"],
            "GroupOwnerId": live.ACCOUNT,
            "SecurityGroupRuleId": "sgr-11111111111111111",
            "IsEgress": False,
            "IpProtocol": "tcp",
            "FromPort": 5439,
            "ToPort": 5439,
            "CidrIpv4": self.a["runner_cidr"],
            "Description": "adbc-q-" + self.a["nonce"],
            "Tags": [{"Key": live.TAG, "Value": self.a["nonce"]}],
        }
        self.ingress.append(rule)
        if self.fail == "authorize-uncertain":
            raise RuntimeError("lost SECRET reply")
        return (
            {"Return": True, "SecurityGroupRules": [rule]}
            if self.fail != "authorize-response"
            else {"Return": False}
        )

    def revoke_security_group_ingress(self, **kwargs):
        self.event("revoke")
        assert kwargs["SecurityGroupRuleIds"] == ["sgr-11111111111111111"]
        self.ingress.clear()

    def get_credentials(self, **kwargs):
        self.event("credentials")
        assert kwargs == {
            "workgroupName": live.NAME,
            "dbName": "adbc_qualification",
            "durationSeconds": 900,
        }
        return {
            "dbUser": "IAMR:adbc-isolated-qualification-ci",
            "dbPassword": "PRIVATE-db-password",
            "expiration": datetime.fromtimestamp(NOW + 900, timezone.utc),
        }

    def create_namespace(self, **kwargs):
        self.ns_exists = (
            True if self.fail == "create-namespace-uncertain" else self.ns_exists
        )
        if self.fail == "create-namespace-uncertain":
            raise RuntimeError("lost SECRET namespace reply")
        self.event("create-namespace")
        self.ns_exists = True
        return self.get_namespace()

    def create_security_group(self, **kwargs):
        self.event("create-group")
        self.sg_exists = True
        return {"GroupId": self.a["resources"]["security_group_id"]}

    def create_workgroup(self, **kwargs):
        self.event("create-workgroup")
        self.wg_exists = True
        return self.get_workgroup()

    def create_usage_limit(self, **kwargs):
        self.event("create-usage")
        return self.get_usage_limit()

    def list_usage_limits(self, **kwargs):
        return {
            "usageLimits": [self.get_usage_limit()["usageLimit"]]
            if self.usage_exists
            else []
        }

    def delete_usage_limit(self, **kwargs):
        self.event("delete-usage")
        self.usage_exists = False

    def delete_workgroup(self, **kwargs):
        self.event("delete-workgroup")
        self.wg_exists = False

    def delete_namespace(self, **kwargs):
        self.event("delete-namespace")
        self.ns_exists = False

    def delete_security_group(self, **kwargs):
        self.event("delete-group")
        self.sg_exists = False

    def get_role(self, **kwargs):
        return {
            "Role": {
                "Arn": live.ROLE,
                "Tags": [{"Key": live.TAG, "Value": self.a["nonce"]}],
                "AssumeRolePolicyDocument": {
                    "Version": "2012-10-17",
                    "Statement": [
                        {
                            "Effect": "Allow",
                            "Principal": {
                                "Federated": (
                                    f"arn:aws:iam::{live.ACCOUNT}:oidc-provider/"
                                    "token.actions.githubusercontent.com"
                                )
                            },
                            "Action": "sts:AssumeRoleWithWebIdentity",
                            "Condition": {
                                "StringEquals": {
                                    "token.actions.githubusercontent.com:aud": (
                                        "sts.amazonaws.com"
                                    ),
                                    "token.actions.githubusercontent.com:sub": "repo:"
                                    + live.REPOSITORY
                                    + ":ref:"
                                    + live.REF,
                                }
                            },
                        }
                    ],
                },
            }
        }

    def list_role_policies(self, **kwargs):
        return copy.deepcopy(self.inline_inventory)

    def list_attached_role_policies(self, **kwargs):
        return copy.deepcopy(self.attached_inventory)

    def put_role_policy(self, **kwargs):
        self.event("install-runtime-policy")
        self.policy = json.loads(kwargs["PolicyDocument"])

    def get_role_policy(self, **kwargs):
        if kwargs["PolicyName"] == "adbc-qualification-initial":
            self.event("read-initial-policy")
            return {"PolicyDocument": copy.deepcopy(self.initial_policy)}
        if self.policy is None:
            raise NotFound()
        return {"PolicyDocument": self.policy}

    def delete_role_policy(self, **kwargs):
        self.event("remove-runtime-policy")
        self.policy = None

    def execute_statement(self, **kwargs):
        self.event("host-sql")
        assert "SecretArn" in kwargs and not any(
            k in kwargs for k in ("Password", "DbUser")
        )
        return {"Id": "fake-statement-id"}

    def describe_statement(self, **kwargs):
        return {"Status": "FINISHED"}

    def get_statement_result(self, **kwargs):
        self.event("independent-observation")
        return {"Records": [[{"longValue": 0}, {"longValue": 0}]]}

    def cancel_statement(self, **kwargs):
        self.event("cancel-host-sql")


class LiveTest(unittest.TestCase):
    def setUp(self):
        self.network = patch.object(
            socket, "socket", side_effect=AssertionError("NETWORK FORBIDDEN")
        )
        self.network.start()
        self.addCleanup(self.network.stop)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        old = legacy.QualificationTest()
        old.manifest = fixture()
        self.paths = old.artifacts(self.temp.name)
        self.a = admission()
        self.a["ca_sha256"] = live.file_sha(self.paths["ca"])
        for kind in live.KINDS:
            self.a["artifacts"][kind].update(
                {
                    k: old.manifest["binding"][kind][k]
                    for k in ("sha256", "installed_tree_sha256")
                }
            )
        self.c_client = Path(self.temp.name) / "c-client"
        self.c_client.write_bytes(b"compiled same-head client fixture")
        self.a["c_client_sha256"] = live.file_sha(self.c_client)
        self.sdk = AWS(self.a)
        self.github = Mock()
        self.github.context.side_effect = lambda: (
            self.sdk.events.append("github-context") or (SOURCE, RUN)
        )
        self.github.verify.side_effect = lambda a: self.sdk.events.append(
            "github-authenticated"
        )
        spec = importlib.util.spec_from_file_location(
            "adbc_driver_redshift.iam",
            ROOT / "python/adbc_driver_redshift/adbc_driver_redshift/iam.py",
        )
        iam = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(iam)
        package = types.ModuleType("adbc_driver_redshift")
        package.iam = iam
        self.modules = patch.dict(
            sys.modules,
            {"adbc_driver_redshift": package, "adbc_driver_redshift.iam": iam},
        )
        self.modules.start()
        self.addCleanup(self.modules.stop)
        self.invoke = Mock(
            side_effect=lambda argv, uri, **kwargs: self.sdk.events.append(
                "private-payload"
            )
        )

        def probe(argv):
            root, kind = Path(argv[4]), argv[5]
            return json.dumps(
                {
                    "library": str(
                        (
                            root
                            / (
                                "adbc_driver_redshift/libadbc_driver_redshift.so"
                                if kind == "python_wheel"
                                else "lib/libadbc_driver_redshift.so"
                            )
                        ).resolve()
                    ),
                    "module": str(root / "adbc_driver_redshift/__init__.py")
                    if kind == "python_wheel"
                    else None,
                    "distribution_root": str(root) if kind == "python_wheel" else None,
                    "entrypoint": "AdbcDriverRedshiftInit",
                }
            ).encode()

        self.probes = patch.object(live, "private_process", side_effect=probe)
        self.probes.start()
        self.addCleanup(self.probes.stop)

    def execute(self, **kwargs):
        return live.execute(
            self.a,
            self.paths,
            "/fake/python",
            self.c_client,
            aws={k: self.sdk for k in ("sts", "ssm", "ec2", "redshift-serverless")},
            github=self.github,
            clock=kwargs.pop("clock", lambda: NOW),
            invoke=self.invoke,
            **kwargs,
        )

    def test_success_has_real_inventory_provenance_before_claim_and_credentials(self):
        result = self.execute()
        self.assertEqual(result["installed_payloads"], "passed")
        self.assertEqual(result["external_teardown"], "required")
        e = self.sdk.events
        self.assertLess(e.index("github-authenticated"), e.index("claim"))
        self.assertLess(e.index("usage"), e.index("claim"))
        self.assertLess(e.index("claim"), e.index("authorize"))
        self.assertLess(e.index("authorize"), e.index("credentials"))
        self.assertEqual(e.count("credentials"), 2)
        self.assertEqual(e.count("private-payload"), 2)
        self.assertEqual(e.count("revoke"), 1)
        self.assertEqual(self.sdk.ingress, [])
        self.assertNotIn("PRIVATE-db-password", json.dumps(result))
        for call in self.invoke.call_args_list:
            argv, uri = call.args
            self.assertNotIn("PRIVATE-db-password", json.dumps(argv))
            self.assertIn("sslmode=verify-full", uri)

    def test_forged_stale_wrong_source_resources_fail_before_cloud(self):
        mutations = [
            ("source_sha", "f" * 40),
            ("run_id", RUN + 1),
            ("runner_cidr", "8.8.8.8/24"),
            ("resources.security_group_id", "sg-06227a706295e3f05"),
            ("resources.workgroup_id", "d41cff82-7eb8-410d-be60-1d467c335756"),
            ("resources.vpc_id", "vpc-11111111111111111"),
            ("resources.subnet_ids", ["subnet-11111111111111111"] * 3),
            ("window.admit_before", NOW),
            ("window.query_deadline", NOW + 4000),
            ("cleanup.owner", live.ROLE),
        ]
        for field, value in mutations:
            with self.subTest(field=field):
                original = copy.deepcopy(self.a)
                current = self.a
                parts = field.split(".")
                for key in parts[:-1]:
                    current = current[key]
                current[parts[-1]] = value
                self.sdk.events.clear()
                with self.assertRaises((Rejected, ValueError)):
                    self.execute()
                self.assertFalse(
                    any(
                        e in self.sdk.events
                        for e in ("identity", "claim", "credentials", "authorize")
                    )
                )
                self.a.clear()
                self.a.update(original)

    def test_tamper_installed_artifact_or_client_never_claims(self):
        for path in (
            self.paths["c_archive"]["root"] / "lib/libadbc_driver_redshift.so",
            self.c_client,
        ):
            original = path.read_bytes()
            path.write_bytes(b"tamper")
            with self.assertRaises(Rejected):
                self.execute()
            self.assertNotIn("claim", self.sdk.events)
            path.write_bytes(original)

    def test_resource_control_and_association_mismatch_never_claims(self):
        changes = [
            ("get_workgroup", "workgroup", "maxCapacity", 8),
            (
                "get_workgroup",
                "workgroup",
                "securityGroupIds",
                ["sg-22222222222222222"],
            ),
            ("get_workgroup", "workgroup", "namespaceName", "wrong"),
            ("get_usage_limit", "usageLimit", "breachAction", "log"),
        ]
        for method, key, field, value in changes:
            original = getattr(self.sdk, method)

            def mutated(*args, **kwargs):
                result = original(*args, **kwargs)
                result[key][field] = value
                return result

            with patch.object(self.sdk, method, side_effect=mutated):
                with self.assertRaises(Rejected):
                    self.execute()
            self.assertNotIn("claim", self.sdk.events)

    def test_duplicate_or_uncertain_claim_cannot_start_paid_work(self):
        self.sdk.fail = "claim-uncertain"
        with self.assertRaises(RuntimeError):
            self.execute()
        self.assertNotIn("authorize", self.sdk.events)
        self.sdk.fail = None
        with self.assertRaises(RuntimeError):
            self.execute()
        self.assertNotIn("credentials", self.sdk.events)
        self.assertEqual(self.sdk.events.count("claim"), 2)

    def test_lost_or_invalid_ingress_reply_recovers_exact_rule(self):
        for failure in ("authorize-uncertain", "authorize-response"):
            self.sdk.fail = failure
            with self.assertRaisesRegex(Rejected, "host cleanup observation") as caught:
                self.execute()
            self.assertNotIn("SECRET", str(caught.exception))
            self.assertEqual(self.sdk.ingress, [])
            self.assertNotIn("credentials", self.sdk.events)
            self.sdk.claims.clear()

    def test_client_failure_or_timeout_reaps_and_revokes_with_no_second_credentials(
        self,
    ):
        self.invoke.side_effect = RuntimeError("PRIVATE-db-password SECRET")
        with self.assertRaises(Rejected) as caught:
            self.execute()
        self.assertNotIn("PRIVATE", str(caught.exception))
        self.assertEqual(self.sdk.events.count("credentials"), 1)
        self.assertEqual(self.sdk.ingress, [])

    def test_cleanup_failure_retains_rule_and_fails(self):
        self.sdk.fail = "revoke"
        with self.assertRaises(Rejected):
            self.execute()
        self.assertEqual(len(self.sdk.ingress), 1)

    def test_expiry_before_claim_or_before_second_client_stops_work(self):
        tick = iter([NOW, NOW + 201])
        with self.assertRaises(Rejected):
            self.execute(clock=lambda: next(tick))
        self.assertNotIn("claim", self.sdk.events)
        tick = iter([NOW, NOW, NOW, NOW, NOW, NOW + 800])
        with self.assertRaises(Rejected):
            self.execute(clock=lambda: next(tick))
        self.assertEqual(self.sdk.events.count("credentials"), 1)
        self.assertEqual(self.sdk.ingress, [])

    def test_endpoint_profile_overrides_rejected_before_session_creation(self):
        for name in (
            "AWS_ENDPOINT_URL",
            "AWS_ENDPOINT_URL_SSM",
            "AWS_PROFILE",
            "AWS_CA_BUNDLE",
        ):
            with (
                patch.dict(os.environ, {name: "forbidden"}, clear=True),
                patch("boto3.Session") as session,
            ):
                with self.assertRaises(Rejected):
                    live.clients()
                session.assert_not_called()

    def test_real_sdk_endpoint_signing_regions_with_dummy_credentials(self):
        import boto3
        from pgwire_qualification_host import host_clients

        session = boto3.Session(
            aws_access_key_id="DUMMY",
            aws_secret_access_key="DUMMY",
            aws_session_token="DUMMY",
            region_name=live.REGION,
        )
        with (
            patch.dict(os.environ, {}, clear=True),
            patch("boto3.Session", return_value=session),
        ):
            clients = host_clients()
        self.assertEqual(clients["iam"]._request_signer._region_name, "us-east-1")
        self.assertEqual(clients["iam"].meta.endpoint_url, "https://iam.amazonaws.com")
        for service in ("sts", "ssm", "redshift-serverless", "ec2", "redshift-data"):
            self.assertEqual(clients[service]._request_signer._region_name, live.REGION)
            self.assertEqual(
                clients[service].meta.config.retries["total_max_attempts"], 1
            )

    def test_missing_rule_tag_is_never_adopted_or_qualified(self):
        authorize = self.sdk.authorize_security_group_ingress

        def untagged(**kwargs):
            response = authorize(**kwargs)
            self.sdk.ingress[0]["Tags"] = []
            response["SecurityGroupRules"][0]["Tags"] = []
            return response

        with patch.object(
            self.sdk, "authorize_security_group_ingress", side_effect=untagged
        ):
            with self.assertRaises(Rejected):
                self.execute()
        self.assertNotIn("credentials", self.sdk.events)
        self.assertNotIn("revoke", self.sdk.events)
        self.assertEqual(len(self.sdk.ingress), 1)

    def test_sdk_uses_fixed_endpoints_no_retries_no_profiles(self):
        with (
            patch.dict(
                os.environ,
                {
                    "AWS_ACCESS_KEY_ID": "fake",
                    "AWS_SECRET_ACCESS_KEY": "fake",
                    "AWS_SESSION_TOKEN": "fake",
                },
                clear=True,
            ),
            patch("boto3.Session") as session,
        ):
            live.clients()
            self.assertNotIn("profile_name", session.call_args.kwargs)
            for call in session.return_value.client.call_args_list:
                self.assertEqual(
                    call.kwargs["endpoint_url"],
                    "https://" + call.args[0] + ".eu-north-1.amazonaws.com",
                )
                self.assertEqual(call.kwargs["config"].retries["total_max_attempts"], 1)
                self.assertEqual(call.kwargs["config"].proxies, {})
                self.assertTrue(call.kwargs["verify"])


class ReplaceCoverageTest(unittest.TestCase):
    def test_each_invalid_replace_rejects_before_drop_preserving_target_rows(self):
        from pgwire_qualification import python_replace_coverage

        class InvalidArgument(Exception):
            status_code = 5

        cursor = Mock()
        cursor.adbc_ingest.side_effect = InvalidArgument("SECRET must not leak")
        cursor.fetchall.return_value = [(1,), (1,), (2,), (2,)]
        python_replace_coverage(
            cursor, ("128-byte column", "value/VALUE"), "adbc_q_1231"
        )
        self.assertEqual(cursor.adbc_ingest.call_count, 2)
        self.assertEqual(cursor.fetchall.call_count, 2)
        self.assertEqual(
            cursor.execute.call_args.args, ("RESET enable_case_sensitive_identifier",)
        )
        # Simulate the original destructive regression: rejection follows DROP.
        cursor.fetchall.return_value = []
        with self.assertRaisesRegex(Rejected, "changed existing rows"):
            python_replace_coverage(
                cursor, ("128-byte column", "value/VALUE"), "adbc_q_1231"
            )
        cursor.adbc_ingest.side_effect = None
        with self.assertRaisesRegex(Rejected, "was accepted"):
            python_replace_coverage(
                cursor, ("128-byte column", "value/VALUE"), "adbc_q_1231"
            )


class ProvenanceTest(unittest.TestCase):
    def test_wrong_run_workflow_source_attempt_or_artifact_rejected(self):
        github = prepare.GitHub("fake-token")
        artifact = {
            "id": 20,
            "name": prepare.ARTIFACTS["c_archive"][0],
            "expired": False,
            "workflow_run": {
                "id": 40,
                "head_sha": SOURCE,
                "head_branch": "feature/pgwire-core-redshift",
            },
        }
        run = {
            "head_sha": SOURCE,
            "path": prepare.ARTIFACTS["c_archive"][1],
            "event": "push",
            "head_branch": "feature/pgwire-core-redshift",
            "status": "completed",
            "conclusion": "success",
            "run_attempt": 1,
            "repository": {"full_name": live.REPOSITORY},
        }
        with patch.object(github, "get", side_effect=[artifact, run]):
            self.assertEqual(github.artifact("c_archive", 20, SOURCE), artifact)
        for key, value in (
            ("head_sha", "e" * 40),
            ("path", "wrong.yml"),
            ("conclusion", "failure"),
            ("event", "pull_request"),
            ("run_attempt", 2),
        ):
            altered = {**run, key: value}
            with (
                patch.object(github, "get", side_effect=[artifact, altered]),
                self.assertRaises(Rejected),
            ):
                github.artifact("c_archive", 20, SOURCE)
        altered = {**artifact, "expired": True}
        with (
            patch.object(github, "get", return_value=altered),
            self.assertRaises(Rejected),
        ):
            github.artifact("c_archive", 20, SOURCE)

    def test_authenticated_review_binds_original_tree_approval_and_complete_checks(
        self,
    ):
        github = prepare.GitHub("fake-token")
        base = {
            "merge_commit_sha": SOURCE,
            "merged_at": "date",
            "number": 15,
            "base": {"ref": "feature/pgwire-core-redshift"},
        }
        pr = {**base, "merged": True, "head": {"sha": SOURCE}}
        comment = {
            "id": 7,
            "user": {"login": "vahid110"},
            "author_association": "OWNER",
            "body": "VERDICT: APPROVE\nHead: " + SOURCE,
        }
        check = {
            "id": 8,
            "name": "Safety",
            "head_sha": SOURCE,
            "status": "completed",
            "conclusion": "success",
            "app": {"slug": "github-actions"},
        }
        replies = [
            [base],
            pr,
            {"tree": {"sha": "e" * 40}},
            {"tree": {"sha": "e" * 40}},
            [comment],
            {"total_count": 1, "check_runs": [check]},
        ]
        with patch.object(github, "get", side_effect=copy.deepcopy(replies)):
            result = github.review(SOURCE)
            self.assertEqual(result["head_sha"], SOURCE)
            self.assertEqual(result["pr"], 15)
        for failure in ("tree", "approval", "check", "incomplete"):
            altered = copy.deepcopy(replies)
            if failure == "tree":
                altered[3]["tree"]["sha"] = "f" * 40
            if failure == "approval":
                altered[4][0]["user"]["login"] = "untrusted"
            if failure == "check":
                altered[5]["check_runs"][0]["conclusion"] = "failure"
            if failure == "incomplete":
                altered[5]["total_count"] = 2
            with (
                patch.object(github, "get", side_effect=altered),
                self.assertRaises(Rejected),
            ):
                github.review(SOURCE)

    def test_duplicate_json_and_nonfinite_rejected(self):
        for text in ('{"nonce":1,"nonce":2}', '{"nonce":NaN}'):
            with self.assertRaises(Rejected):
                prepare.parse_json(text)


class HostTest(unittest.TestCase):
    def test_broad_initial_or_incomplete_inventory_rejects_before_creation(self):
        for failure in (
            "all-actions",
            "admission-wildcard",
            "extra-grant",
            "inline-truncated",
            "managed-truncated",
            "inline-marker",
            "managed-marker",
        ):
            with (
                self.subTest(failure=failure),
                tempfile.TemporaryDirectory() as temp,
                patch.object(
                    socket, "socket", side_effect=AssertionError("NETWORK FORBIDDEN")
                ),
            ):
                a = admission()
                sdk = AWS(a)
                sdk.ns_exists = sdk.wg_exists = sdk.sg_exists = False
                sdk.get_caller_identity = lambda: {
                    "Account": live.ACCOUNT,
                    "Arn": (
                        f"arn:aws:sts::{live.ACCOUNT}:assumed-role/"
                        "adbc-provisioner/host"
                    ),
                }
                if failure == "all-actions":
                    sdk.initial_policy = {
                        "Version": "2012-10-17",
                        "Statement": [
                            {"Effect": "Allow", "Action": "*", "Resource": "*"}
                        ],
                    }
                elif failure == "admission-wildcard":
                    sdk.initial_policy["Statement"][1]["Resource"] = "*"
                elif failure == "extra-grant":
                    sdk.initial_policy["Statement"].append(
                        {
                            "Effect": "Allow",
                            "Action": "redshift-serverless:GetCredentials",
                            "Resource": "*",
                        }
                    )
                elif failure == "inline-truncated":
                    sdk.inline_inventory["IsTruncated"] = True
                elif failure == "managed-truncated":
                    sdk.attached_inventory["IsTruncated"] = True
                elif failure == "inline-marker":
                    sdk.inline_inventory["Marker"] = "hidden-page"
                else:
                    sdk.attached_inventory["Marker"] = "hidden-page"
                github = Mock()
                github.get.return_value = {
                    "status": "completed",
                    "conclusion": "success",
                }
                host = Host(
                    {
                        k: sdk
                        for k in (
                            "sts",
                            "ssm",
                            "redshift-serverless",
                            "redshift-data",
                            "ec2",
                            "iam",
                        )
                    },
                    github,
                    Path(temp) / "receipt.json",
                    clock=lambda: NOW,
                    pause=lambda _: None,
                )
                template = Path(temp) / "template.json"
                template.write_text(
                    json.dumps(
                        {
                            "Version": "2012-10-17",
                            "Statement": [
                                {
                                    "Effect": "Allow",
                                    "Action": "ssm:PutParameter",
                                    "Resource": a["nonce"] + "-EXACT_RUN_ID",
                                }
                            ],
                        }
                    )
                )
                preflight = {
                    k: a[k]
                    for k in (
                        "source_sha",
                        "run_id",
                        "artifacts",
                        "ca_sha256",
                        "runner_cidr",
                        "c_client_sha256",
                        "review",
                    )
                }
                with (
                    patch.object(host, "preflight", return_value=preflight),
                    self.assertRaises(Rejected),
                ):
                    host.run(
                        preflight_id=1,
                        run=RUN,
                        source=SOURCE,
                        nonce=a["nonce"],
                        policy_template=template,
                        policy_sha256=live.file_sha(template),
                    )
                self.assertFalse(
                    set(sdk.events)
                    & {
                        "create-namespace",
                        "create-group",
                        "create-workgroup",
                        "install-runtime-policy",
                        "host-sql",
                        "admission",
                        "credentials",
                    }
                )
                self.assertFalse(sdk.ns_exists or sdk.wg_exists or sdk.sg_exists)

    def test_actual_fake_host_lifecycle_bootstraps_observes_and_deletes(self):
        a = admission()
        sdk = AWS(a)
        sdk.ns_exists = sdk.wg_exists = sdk.sg_exists = False
        sdk.get_caller_identity = lambda: {
            "Account": live.ACCOUNT,
            "Arn": (f"arn:aws:sts::{live.ACCOUNT}:assumed-role/adbc-provisioner/host"),
        }
        github = Mock()
        github.get.return_value = {"status": "completed", "conclusion": "success"}
        preflight = {
            k: a[k]
            for k in (
                "source_sha",
                "run_id",
                "artifacts",
                "ca_sha256",
                "runner_cidr",
                "c_client_sha256",
                "review",
            )
        }
        with (
            tempfile.TemporaryDirectory() as temp,
            patch.object(
                socket, "socket", side_effect=AssertionError("NETWORK FORBIDDEN")
            ),
        ):
            host = Host(
                {
                    k: sdk
                    for k in (
                        "sts",
                        "ssm",
                        "redshift-serverless",
                        "redshift-data",
                        "ec2",
                        "iam",
                    )
                },
                github,
                Path(temp) / "receipt.json",
                clock=lambda: NOW,
                pause=lambda _: None,
            )
            template = Path(temp) / "policy.json"
            template.write_text(
                json.dumps(
                    {
                        "Version": "2012-10-17",
                        "Statement": [
                            {
                                "Effect": "Allow",
                                "Action": "ssm:PutParameter",
                                "Resource": a["nonce"] + "-EXACT_RUN_ID",
                                "Condition": {
                                    "DateLessThan": {"aws:CurrentTime": "EXACT_CUTOFF"}
                                },
                            }
                        ],
                    }
                )
            )
            with patch.object(host, "preflight", return_value=preflight):
                result = host.run(
                    preflight_id=1,
                    run=RUN,
                    source=SOURCE,
                    nonce=a["nonce"],
                    policy_template=template,
                    policy_sha256=live.file_sha(template),
                )
            self.assertEqual(result["external_teardown"], "verified")
            self.assertTrue(result["independent_observation"]["fixtures_absent"])
            self.assertEqual(sdk.events.count("host-sql"), 3)
            self.assertEqual(sdk.events.count("read-initial-policy"), 2)
            self.assertLess(
                sdk.events.index("read-initial-policy"),
                sdk.events.index("create-namespace"),
            )
            self.assertLess(
                sdk.events.index("independent-observation"),
                sdk.events.index("delete-workgroup"),
            )
            self.assertLess(
                sdk.events.index("delete-workgroup"),
                sdk.events.index("delete-namespace"),
            )
            self.assertFalse(sdk.ns_exists or sdk.wg_exists or sdk.sg_exists)
            receipt = json.loads((Path(temp) / "receipt.json").read_text())
            self.assertEqual(
                receipt["created"]["namespace"], a["resources"]["namespace_id"]
            )
            self.assertNotIn("PRIVATE", json.dumps(receipt))

    def test_partial_create_failure_still_deletes_owned_namespace_and_no_admission(
        self,
    ):
        a = admission()
        sdk = AWS(a)
        sdk.ns_exists = sdk.wg_exists = sdk.sg_exists = False
        sdk.fail = "create-group"
        sdk.get_caller_identity = lambda: {
            "Account": live.ACCOUNT,
            "Arn": (f"arn:aws:sts::{live.ACCOUNT}:assumed-role/adbc-provisioner/host"),
        }
        with tempfile.TemporaryDirectory() as temp:
            host = Host(
                {
                    k: sdk
                    for k in (
                        "sts",
                        "ssm",
                        "redshift-serverless",
                        "redshift-data",
                        "ec2",
                        "iam",
                    )
                },
                Mock(),
                Path(temp) / "receipt.json",
                clock=lambda: NOW,
                pause=lambda _: None,
            )
            with (
                patch.object(host, "preflight", return_value={}),
                self.assertRaises(Rejected),
            ):
                host.run(preflight_id=1, run=RUN, source=SOURCE, nonce=a["nonce"])
            self.assertFalse(sdk.ns_exists)
            self.assertNotIn("admission", sdk.events)
            self.assertTrue(host.path.exists())


if __name__ == "__main__":
    unittest.main()
