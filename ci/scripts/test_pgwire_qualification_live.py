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
import hashlib
import importlib.util
import io
import json
import os
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import types
import unittest
import venv
import zipfile
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import Mock, patch

import pgwire_qualification_live as live
import pgwire_qualification_prepare as prepare
import test_pgwire_qualification as legacy
from pgwire_qualification_host import OWNER, Host
from pgwire_qualification_manifest import Rejected
from pgwire_qualification_policy import (
    TEMPLATE,
    admission_name,
    operation_prefix,
)
from test_pgwire_qualification import NOW, ROOT, RUN, SOURCE, fixture


class NotFound(Exception):
    pass


class ValidationException(Exception):
    pass


NONCE = "d" * 64


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
                    + admission_name(self.a["nonce"]),
                },
            ],
        }
        self.exceptions = types.SimpleNamespace(
            ResourceNotFoundException=NotFound,
            NoSuchEntityException=NotFound,
            ValidationException=ValidationException,
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

    def describe_parameters(self, **kwargs):
        self.event("operation-metadata")
        assert kwargs["MaxResults"] == 50
        prefix = kwargs["ParameterFilters"][0]["Values"][0]
        assert prefix == operation_prefix(self.a["nonce"])
        return {
            "Parameters": [
                {"Name": name} for name in self.claims if name.startswith(prefix)
            ]
        }

    def put_parameter(self, **kwargs):
        self.events.append(
            "host-reservation"
            if kwargs["Name"].endswith("/host-attempt")
            else "claim"
            if "/claim-" in kwargs["Name"]
            else "admission"
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
        # Captured service constraint: presence conflicts even for False/DISABLED.
        if (
            "pricePerformanceTarget" in kwargs
            and "extraComputeForAutomaticOptimization" in kwargs
        ):
            raise ValidationException("conflicting scaling and pricing controls")
        self.workgroup_request = copy.deepcopy(kwargs)
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
                                    "token.actions.githubusercontent.com:sub": (
                                        "repo:vahid110@5770674/arrow-adbc@1363017696:"
                                        "ref:refs/heads/feature/pgwire-core-redshift"
                                    ),
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
        self.statement_sql = kwargs["Sql"]
        assert "SecretArn" in kwargs and not any(
            k in kwargs for k in ("Password", "DbUser")
        )
        return {"Id": "fake-statement-id"}

    def describe_statement(self, **kwargs):
        if "stv_sessions" in getattr(self, "statement_sql", ""):
            return {"Status": "FAILED", "Error": "PRIVATE permission denied"}
        return {"Status": "FINISHED"}

    def get_statement_result(self, **kwargs):
        self.event("independent-observation")
        return {
            "Records": [
                [{"longValue": 1}, {"longValue": 1}, {"longValue": 0}, {"longValue": 0}]
            ]
        }

    def cancel_statement(self, **kwargs):
        self.event("cancel-host-sql")
        raise ValidationException("already FINISHED or FAILED")


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
            nonce=NONCE,
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

    def test_caller_cannot_enable_initializing_mode(self):
        self.a["initializing"] = True
        with self.assertRaises(Rejected):
            self.execute()
        self.assertNotIn("identity", self.sdk.events)
        self.assertNotIn("claim", self.sdk.events)

    def test_resource_control_and_association_mismatch_never_claims(self):
        changes = [
            ("get_workgroup", "workgroup", "status", "MODIFYING"),
            ("get_namespace", "namespace", "status", "MODIFYING"),
            ("get_workgroup", "workgroup", "baseCapacity", 8),
            ("get_workgroup", "workgroup", "maxCapacity", 8),
            (
                "get_workgroup",
                "workgroup",
                "extraComputeForAutomaticOptimization",
                True,
            ),
            (
                "get_workgroup",
                "workgroup",
                "extraComputeForAutomaticOptimization",
                None,
            ),
            ("get_workgroup", "workgroup", "pricePerformanceTarget", None),
            ("get_workgroup", "workgroup", "pricePerformanceTarget", {}),
            (
                "get_workgroup",
                "workgroup",
                "pricePerformanceTarget",
                {"status": "ENABLED"},
            ),
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
                if value is None:
                    result[key].pop(field)
                else:
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
        elapsed = [NOW]
        self.invoke.side_effect = lambda *args, **kwargs: elapsed.__setitem__(
            0, NOW + 800
        )
        with self.assertRaises(Rejected):
            self.execute(clock=lambda: elapsed[0])
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


@unittest.skipUnless(
    sys.version_info >= (3, 12), "preparer uses Python 3.12 data-filter extraction"
)
class PrepareFlowTest(unittest.TestCase):
    """Actual prepare/inventory/isolated-load flow, with no AWS or network.

    Only authenticated GitHub/IP inputs, pip installation and C-client compilation
    are fake boundaries. A tiny local exported-symbol library makes both real
    isolated load-only probes executable without building the driver.
    """

    def setUp(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("C compiler required for tiny load-only fixture")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.ca = self.root / "ca.pem"
        self.ca.write_bytes(b"nonsecret synthetic CA for load-only checks")
        source = self.root / "load-only.c"
        source.write_text("int AdbcDriverRedshiftInit(void) { return 0; }\n")
        library = self.root / "load-only.so"
        subprocess.run(
            [
                compiler,
                "-dynamiclib" if sys.platform == "darwin" else "-shared",
                "-fPIC",
                str(source),
                "-o",
                str(library),
            ],
            check=True,
            capture_output=True,
            timeout=20,
        )
        self.library = library.read_bytes()
        self.environment = self.root / "venv"
        venv.EnvBuilder(with_pip=False, symlinks=True).create(self.environment)
        self.interpreter = str(self.environment / "bin/python")
        self.package_root = Path(
            subprocess.check_output(
                [
                    self.interpreter,
                    "-I",
                    "-c",
                    "import sysconfig; print(sysconfig.get_path('purelib'))",
                ],
                text=True,
            ).strip()
        )
        self.directory = self.root / "prepared"
        self.github = Mock()
        self.github.context.return_value = (SOURCE, RUN)
        self.github.review.return_value = admission()["review"]
        self.github.download.side_effect = self.download
        self.network = patch.object(
            socket, "socket", side_effect=AssertionError("NETWORK FORBIDDEN")
        )
        self.network.start()
        self.addCleanup(self.network.stop)
        # The fixed production CA path stays unchanged; use its actual-file input
        # boundary on macOS as well as Linux, never an admission/TLS bypass.
        self.original_run = subprocess.run
        self.calls = []

    def download(self, kind, artifact_id, source, target):
        self.assertEqual(source, SOURCE)
        self.assertEqual(artifact_id, 20 if kind == "c_archive" else 21)
        if kind == "c_archive":
            with tarfile.open(target / "installed.tar.gz", "w:gz") as archive:
                for name, data in {
                    "lib/libadbc_driver_redshift.so": self.library,
                    "include/arrow-adbc/adbc.h": b"synthetic header",
                }.items():
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    archive.addfile(info, io.BytesIO(data))
        else:
            with zipfile.ZipFile(
                target / "adbc_driver_redshift-0-py3-none-linux_x86_64.whl", "w"
            ) as archive:
                files = {
                    "adbc_driver_redshift/__init__.py": (
                        b"from pathlib import Path\ndef _driver_path():\n"
                        b"    return str(Path(__file__).with_name("
                        b"'libadbc_driver_redshift.so'))\n"
                    ),
                    "adbc_driver_redshift/dbapi.py": b"# load-only fixture\n",
                    "adbc_driver_redshift/iam.py": (
                        ROOT / "python/adbc_driver_redshift/adbc_driver_redshift/iam.py"
                    ).read_bytes(),
                    "adbc_driver_redshift/libadbc_driver_redshift.so": self.library,
                    "adbc_driver_redshift-0.dist-info/METADATA": (
                        b"Name: adbc-driver-redshift\nVersion: 0\n"
                    ),
                }
                for name, data in files.items():
                    archive.writestr(name, data)
        return {"workflow_run": {"id": 40 if kind == "c_archive" else 41}}

    def installed_process(self, argv, **kwargs):
        if argv[:4] == [self.interpreter, "-m", "pip", "install"]:
            self.calls.append("install-wheel")
            self.assertIn("--no-deps", argv)
            self.assertIn("--no-compile", argv)
            with zipfile.ZipFile(argv[-1]) as archive:
                archive.extractall(self.package_root)
            return subprocess.CompletedProcess(argv, 0)
        if argv[0] == "cc":
            self.calls.append("compile-client")
            self.assertIn("c/driver/pgwire/redshift_qualification.c", argv)
            Path(argv[argv.index("-o") + 1]).write_bytes(
                b"synthetic client, never executed"
            )
            return subprocess.CompletedProcess(argv, 0)
        return self.original_run(argv, **kwargs)

    def prepare(self):
        response = Mock()
        response.__enter__ = Mock(return_value=io.BytesIO(b"8.8.8.8\n"))
        response.__exit__ = Mock(return_value=False)
        opener = Mock()
        opener.open.return_value = response
        with (
            patch.object(prepare.urllib.request, "build_opener", return_value=opener),
            patch.object(prepare.subprocess, "run", side_effect=self.installed_process),
            patch.object(prepare, "CA_BUNDLE", str(self.ca), create=True),
            patch.object(
                live, "clients", side_effect=AssertionError("SDK FORBIDDEN")
            ) as sdk,
            patch.object(live, "private_process", wraps=live.private_process) as probes,
        ):
            result = prepare.prepare(
                self.github, self.directory, self.interpreter, 20, 21, nonce=NONCE
            )
            self.assertEqual(probes.call_count, 2)
            self.assertTrue(
                all(c.args[0][-2:] == ["load-only", "-"] for c in probes.call_args_list)
            )
            sdk.assert_not_called()
        self.assertEqual(self.calls, ["install-wheel", "compile-client"])
        return result

    def test_prepare_emits_verified_preflight_without_authorization_window(self):
        record = self.prepare()
        self.assertNotIn("window", record)
        self.assertNotIn("resources", record)
        self.assertEqual(
            record, json.loads((self.directory / "preflight.json").read_text())
        )
        self.assertTrue((self.directory / "paths.json").is_file())
        with self.assertRaises(Rejected):
            live.validate(record, source=SOURCE, run=RUN, now=NOW, nonce=NONCE)

    def test_consumer_rechecks_emitted_record_before_sdk_and_rejects_tampering(self):
        record = self.prepare()
        paths = self.directory / "paths.json"
        output = self.directory / "result.json"
        with (
            patch.object(sys, "executable", self.interpreter),
            patch.dict(os.environ, {"GH_TOKEN": "synthetic-token"}),
            patch.object(
                sys,
                "argv",
                [
                    "qualification",
                    str(paths),
                    "--output",
                    str(output),
                    "--nonce",
                    NONCE,
                ],
            ),
            patch.object(prepare, "GitHub", return_value=self.github),
            patch.object(
                live, "clients", side_effect=RuntimeError("SECRET SDK BOUNDARY")
            ) as sdk,
            patch.object(live, "private_process", wraps=live.private_process) as probes,
            patch("sys.stderr", new_callable=io.StringIO) as stderr,
        ):
            self.assertEqual(live.main(), 1)
            self.assertEqual(probes.call_count, 2)
            sdk.assert_called_once_with()
            self.github.verify.assert_called_once_with(record)
            self.assertNotIn("SECRET", stderr.getvalue())
            sdk.reset_mock()
            library = self.directory / "c-prefix/lib/libadbc_driver_redshift.so"
            library.write_bytes(b"changed after prepare")
            sys.argv[3] = str(output.with_name("tamper-result.json"))
            self.assertEqual(live.main(), 1)
            sdk.assert_not_called()


class ConsumerPersistenceDeadlineTest(unittest.TestCase):
    def test_slow_persistence_blocks_each_expired_effect_and_preserves_cleanup(self):
        for stage in (
            "CLAIM",
            "INGRESS",
            "C_CREDENTIALS",
            "PYTHON_CREDENTIALS",
            "C_PAYLOAD",
            "PYTHON_PAYLOAD",
        ):
            with self.subTest(stage=stage):
                flow = LiveTest()
                flow.setUp()
                try:
                    tick = [NOW]
                    receipt = live.ConsumerReceipt(Path(flow.temp.name) / "result.json")
                    save = receipt.save
                    delayed = [False]

                    def persist():
                        save()
                        target = receipt.data["stage"] == stage
                        if stage == "INGRESS":
                            target &= (
                                receipt.data["owned_ingress"] == "ATTEMPTED_UNKNOWN"
                            )
                        if target and not delayed[0]:
                            delayed[0] = True
                            tick[0] = NOW + (201 if stage == "CLAIM" else 801)

                    receipt.save = persist
                    effects = []
                    for method, name in (
                        ("put_parameter", "claim"),
                        ("authorize_security_group_ingress", "ingress"),
                        ("get_credentials", "credentials"),
                    ):
                        original = getattr(flow.sdk, method)

                        def operation(*args, _operation=original, _name=name, **kwargs):
                            effects.append((_name, tick[0]))
                            return _operation(*args, **kwargs)

                        setattr(flow.sdk, method, operation)
                    flow.invoke.side_effect = lambda *args, **kwargs: effects.append(
                        ("payload", tick[0])
                    )
                    with (
                        self.assertRaises(Rejected),
                        patch("sys.stdout", new_callable=io.StringIO) as output,
                    ):
                        flow.execute(clock=lambda: tick[0], receipt=receipt)
                    self.assertTrue(delayed[0])
                    self.assertEqual(
                        receipt.data["first_failure"]["reason"], "DEADLINE"
                    )
                    self.assertFalse(
                        [
                            effect
                            for effect in effects
                            if effect[1] >= NOW + (200 if stage == "CLAIM" else 800)
                        ]
                    )
                    self.assertNotIn("private payload started", output.getvalue())
                    if stage == "CLAIM":
                        self.assertNotIn("claim", flow.sdk.events)
                        self.assertEqual(receipt.data["claim"], "SPENT_OR_UNKNOWN")
                    else:
                        self.assertEqual(flow.sdk.ingress, [])
                        if stage != "INGRESS":
                            self.assertIn("revoke", flow.sdk.events)
                    if stage.endswith("PAYLOAD"):
                        kind = "c_archive" if stage.startswith("C_") else "python_wheel"
                        self.assertEqual(receipt.data["payloads"][kind], "NOT_STARTED")
                        self.assertEqual(receipt.data["first_failure"]["stage"], stage)
                finally:
                    flow.doCleanups()

    def test_payload_timeout_is_recomputed_after_durable_mark(self):
        flow = LiveTest()
        flow.setUp()
        self.addCleanup(flow.doCleanups)
        tick = [NOW]
        receipt = live.ConsumerReceipt(Path(flow.temp.name) / "result.json")
        save = receipt.save

        def persist():
            save()
            if (
                receipt.data["stage"] == "C_PAYLOAD"
                and receipt.data["payloads"]["c_archive"] == "STARTED_UNKNOWN"
            ):
                tick[0] += 5

        receipt.save = persist
        fetch = flow.sdk.get_credentials

        def credentials(**kwargs):
            result = fetch(**kwargs)
            tick[0] = NOW + 790
            return result

        flow.sdk.get_credentials = credentials
        captured = []

        def invoke(*args, **kwargs):
            captured.append((tick[0], kwargs["seconds"]))
            raise RuntimeError("PRIVATE stop after observed invocation")

        flow.invoke.side_effect = invoke
        with self.assertRaises(Rejected):
            flow.execute(clock=lambda: tick[0], receipt=receipt)
        self.assertEqual(captured, [(NOW + 795, 4)])
        self.assertEqual(receipt.data["payloads"]["c_archive"], "STARTED_UNKNOWN")
        self.assertIn("revoke", flow.sdk.events)


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


class HostStatementTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.network = patch.object(
            socket, "socket", side_effect=AssertionError("NETWORK FORBIDDEN")
        )
        self.network.start()
        self.addCleanup(self.network.stop)
        self.a = admission()
        self.sdk = AWS(self.a)
        self.sdk.get_caller_identity = lambda: {
            "Account": live.ACCOUNT,
            "Arn": f"arn:aws:sts::{live.ACCOUNT}:assumed-role/adbc-provisioner/host",
        }
        self.tick = NOW
        self.host = Host(
            {
                k: self.sdk
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
            Path(self.temp.name) / "receipt.json",
            _attempt_directory=Path(self.temp.name).resolve() / "attempts",
            clock=lambda: self.tick,
            pause=self.pause,
        )
        self.host.github.get.return_value = {
            "status": "completed",
            "conclusion": "cancelled",
        }
        self.host.deadline = NOW + 120
        self.host.state = {
            "nonce": self.a["nonce"],
            "attempted": ["namespace", "workgroup", "security_group", "usage_limit"],
            "created": {},
            "admission": self.a,
            "run_id": RUN,
            "deadline": NOW + 120,
            "managed_secret_arn": self.sdk.get_namespace()["namespace"][
                "adminPasswordSecretArn"
            ],
        }

    def pause(self, seconds):
        self.tick += seconds

    def test_finished_records_survive_without_cancel(self):
        rows = self.host.statement("SELECT 0", records=True)
        self.assertEqual(rows[0][0], {"longValue": 1})
        self.assertNotIn("cancel-host-sql", self.sdk.events)

    def test_late_terminal_status_is_retained_before_deadline_rejection(self):
        for status in ("FINISHED", "FAILED", "ABORTED"):
            with self.subTest(status=status):
                self.tick = NOW

                def describe(**kwargs):
                    self.tick += 31
                    return {"Status": status, "Error": "PRIVATE SQL"}

                self.sdk.describe_statement = describe
                with self.assertRaisesRegex(Rejected, "host stage deadline reached"):
                    self.host.statement("SELECT 0", records=True)
                self.assertNotIn("cancel-host-sql", self.sdk.events)
                self.assertEqual(
                    self.host.state["statements"][-1],
                    {"terminal": status, "cancel": "NOT_NEEDED"},
                )
                self.assertNotIn("PRIVATE", self.host.path.read_text())

    def test_until_does_not_start_read_after_deadline(self):
        self.host.deadline = NOW
        operation = Mock(return_value=True)
        with self.assertRaisesRegex(Rejected, "host stage deadline reached"):
            self.host.until(operation, bool)
        operation.assert_not_called()

    def test_until_stops_before_next_read_when_pause_crosses_deadline(self):
        operation = Mock(return_value=False)
        with self.assertRaisesRegex(Rejected, "host stage deadline reached"):
            self.host.until(operation, bool, seconds=1)
        self.assertEqual(operation.call_count, 1)

    def test_cleanup_allowance_remains_after_original_deadline(self):
        self.tick = NOW + 121
        self.host.cleanup()
        self.assertTrue(self.host.state["independent_observation"]["sessions_absent"])
        self.assertEqual(self.host.state["external_teardown"], "verified")
        self.assertFalse(self.sdk.ns_exists or self.sdk.wg_exists or self.sdk.sg_exists)

    def test_failed_and_aborted_sql_are_not_masked_by_cancel(self):
        for status in ("FAILED", "ABORTED"):
            with self.subTest(status=status):
                self.sdk.describe_statement = lambda **kwargs: {
                    "Status": status,
                    "Error": "PRIVATE SQL ERROR",
                }
                with self.assertRaisesRegex(Rejected, "host SQL failed"):
                    self.host.statement("SELECT 0")
                self.assertNotIn("cancel-host-sql", self.sdk.events)
                self.assertEqual(
                    self.host.state["statements"][-1],
                    {"terminal": status, "cancel": "NOT_NEEDED"},
                )
                self.assertNotIn("PRIVATE", self.host.path.read_text())

    def test_finished_result_error_preserves_failure_without_cancel(self):
        self.sdk.get_statement_result = Mock(
            side_effect=RuntimeError("PRIVATE result error")
        )
        with self.assertRaises(RuntimeError):
            self.host.statement("SELECT 0", records=True)
        self.assertNotIn("cancel-host-sql", self.sdk.events)
        self.assertEqual(self.host.state["statements"][-1]["terminal"], "FINISHED")
        self.assertNotIn("PRIVATE", self.host.path.read_text())

    def test_unknown_describe_cancel_race_preserves_primary_failure(self):
        self.sdk.describe_statement = Mock(
            side_effect=[
                RuntimeError("PRIVATE original"),
                {"Status": "FAILED", "Error": "PRIVATE SQL"},
            ]
        )
        with self.assertRaisesRegex(RuntimeError, "PRIVATE original"):
            self.host.statement("SELECT 0")
        self.assertEqual(self.sdk.events.count("cancel-host-sql"), 1)
        self.assertEqual(
            self.host.state["statements"][-1],
            {"terminal": "FAILED", "cancel": "TERMINAL_RACE"},
        )
        self.assertNotIn("PRIVATE", self.host.path.read_text())

    def test_inflight_timeout_cancels_once_and_checks_terminal(self):
        cancelled = []
        self.sdk.describe_statement = lambda **kwargs: {
            "Status": "ABORTED" if cancelled else "STARTED"
        }

        def cancel(**kwargs):
            cancelled.append(kwargs["Id"])
            return {"Status": True}

        self.sdk.cancel_statement = cancel
        with self.assertRaisesRegex(Rejected, "host stage deadline reached"):
            self.host.statement("SELECT 0")
        self.assertEqual(len(cancelled), 1)
        self.assertEqual(self.tick - NOW, 30)
        self.assertEqual(
            self.host.state["statements"][-1],
            {"terminal": "ABORTED", "cancel": "REQUESTED_AND_TERMINAL"},
        )

    def test_uncertain_cancellation_and_failed_observation_remain_unknown(self):
        self.sdk.describe_statement = Mock(side_effect=RuntimeError("PRIVATE describe"))
        self.sdk.cancel_statement = Mock(
            side_effect=RuntimeError("PRIVATE cancellation")
        )
        self.host.cleanup()
        self.assertEqual(
            self.host.state["statements"][-1],
            {"terminal": "UNKNOWN", "cancel": "UNKNOWN"},
        )
        self.assertEqual(
            self.host.state["independent_observation"]["failure_reason"],
            "SDK_OR_UNEXPECTED_FAILURE",
        )
        self.assertIsNot(
            self.host.state["independent_observation"].get("sessions_absent"), True
        )
        self.assertEqual(self.host.state["external_teardown"], "verified")
        self.assertNotIn("PRIVATE", self.host.path.read_text())

    def test_restricted_or_malformed_observer_cannot_prove_zero(self):
        self.sdk.cancel_statement = lambda **kwargs: {"Status": True}
        for rows in (
            [[{"longValue": 0}, {"longValue": 0}]],
            [[{"longValue": 0}, {"longValue": 0}, {"longValue": 0}, {"longValue": 0}]],
            [[{"longValue": 1}, {"longValue": 0}]],
            [
                [
                    {"longValue": True},
                    {"longValue": 1},
                    {"longValue": 0},
                    {"longValue": 0},
                ]
            ],
            [[{"longValue": 1}, {"longValue": 2}, {"longValue": 0}, {"longValue": 0}]],
            [[{"longValue": 1}, {"longValue": 0}, {"longValue": 0}, {"longValue": 0}]],
            [[{"longValue": 1}, {"longValue": 1}, {"longValue": -1}, {"longValue": 0}]],
            [],
        ):
            with self.subTest(rows=rows):
                self.sdk.ns_exists = self.sdk.wg_exists = self.sdk.sg_exists = (
                    self.sdk.usage_exists
                ) = True
                self.sdk.get_statement_result = lambda **kwargs: {"Records": rows}
                self.host.cleanup()
                observation = self.host.state["independent_observation"]
                self.assertIsNot(observation.get("fixtures_absent"), True)
                self.assertIsNot(observation.get("sessions_absent"), True)

    def test_supported_observer_counts_sessions_without_database_filter(self):
        for fixtures, sessions in ((0, 0), (1, 0), (0, 1)):
            with self.subTest(fixtures=fixtures, sessions=sessions):
                self.sdk.ns_exists = self.sdk.wg_exists = self.sdk.sg_exists = (
                    self.sdk.usage_exists
                ) = True
                self.sdk.get_statement_result = lambda **kwargs: {
                    "Records": [
                        [
                            {"longValue": 1},
                            {"longValue": 1},
                            {"longValue": fixtures},
                            {"longValue": sessions},
                        ]
                    ]
                }
                execute = self.sdk.execute_statement
                with patch.object(
                    self.sdk, "execute_statement", wraps=execute
                ) as called:
                    self.host.cleanup()
                sql = called.call_args.kwargs["Sql"]
                self.assertIn("sys_session_history", sql)
                self.assertIn("svv_user_info", sql)
                self.assertIn("user_name=current_user", sql)
                self.assertIn("AND superuser", sql)
                self.assertIn("status='active'", sql)
                self.assertIn("IAMR:adbc-isolated-qualification-ci", sql)
                self.assertNotIn("database_name", sql)
                self.assertNotIn("stv_sessions", sql)
                observed = self.host.state["independent_observation"]
                self.assertTrue(observed["admin_visibility_verified"])
                self.assertEqual(observed["fixtures_absent"], fixtures == 0)
                self.assertEqual(observed["sessions_absent"], sessions == 0)

    def test_absent_ci_user_cannot_prove_cleanup_even_with_admin_visibility(self):
        self.sdk.get_statement_result = lambda **kwargs: {
            "Records": [
                [{"longValue": 1}, {"longValue": 0}, {"longValue": 0}, {"longValue": 0}]
            ]
        }
        self.host.cleanup()
        observed = self.host.state["independent_observation"]
        self.assertEqual(observed["failure_reason"], "OBSERVER_PROOF_INVALID")
        self.assertIsNot(observed.get("sessions_absent"), True)


class HostDeadlineTest(unittest.TestCase):
    def test_real_preflight_lifecycle_rejects_late_reads_and_stopped_runs(self):
        for mode in (
            "readiness-stage",
            "readiness-total",
            "preflight",
            "initial-role",
            "policy",
            "presql",
            "cancelled",
            "failure",
            "final-github",
            "final-github-total",
        ):
            with self.subTest(mode=mode):
                self.run_lifecycle(mode)

    def test_fresh_run_rejects_changed_provenance(self):
        for field in (
            "path",
            "head_sha",
            "head_branch",
            "event",
            "run_attempt",
            "repository",
        ):
            with self.subTest(field=field):
                self.run_lifecycle("wrong-" + field)

    def run_lifecycle(self, mode):
        a = admission()
        prepared = {
            k: a[k]
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
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            archive.writestr("preflight.json", json.dumps(prepared))
        zipped = buffer.getvalue()
        sdk = AWS(a)
        sdk.ns_exists = sdk.wg_exists = sdk.sg_exists = False
        sdk.get_caller_identity = lambda: {
            "Account": live.ACCOUNT,
            "Arn": f"arn:aws:sts::{live.ACCOUNT}:assumed-role/adbc-provisioner/host",
        }
        tick = [NOW]
        state = {
            "cleanup": False,
            "usage": False,
            "delayed": False,
            "policy_reads": 0,
            "runtime": False,
            "observed": False,
            "status": "in_progress",
        }
        events = []
        original_event = sdk.event

        def event(name):
            events.append((name, tick[0] - NOW, state["cleanup"]))
            original_event(name)

        sdk.event = event
        usage = sdk.create_usage_limit

        def create_usage(**kwargs):
            result = usage(**kwargs)
            state["usage"] = True
            return result

        sdk.create_usage_limit = create_usage
        workgroup = sdk.get_workgroup

        def get_workgroup(**kwargs):
            result = workgroup(**kwargs)
            if (
                mode.startswith("readiness-")
                and state["usage"]
                and not state["delayed"]
            ):
                state["delayed"] = True
                tick[0] += 121 if mode == "readiness-stage" else 3601
            return result

        sdk.get_workgroup = get_workgroup
        policy = sdk.get_role_policy

        def get_policy(**kwargs):
            if kwargs["PolicyName"] == "adbc-qualification-initial":
                state["policy_reads"] += 1
                if (mode == "policy" and state["policy_reads"] == 2) or (
                    mode == "initial-role" and state["policy_reads"] == 1
                ):
                    tick[0] += 3601
            return policy(**kwargs)

        sdk.get_role_policy = get_policy
        install = sdk.put_role_policy

        def put_policy(**kwargs):
            result = install(**kwargs)
            state["runtime"] = True
            return result

        sdk.put_role_policy = put_policy
        namespace = sdk.get_namespace

        def get_namespace(**kwargs):
            result = namespace(**kwargs)
            if mode == "presql" and state["runtime"] and not state["delayed"]:
                state["delayed"] = True
                tick[0] += 3601
            return result

        sdk.get_namespace = get_namespace
        statement_result = sdk.get_statement_result

        def get_result(**kwargs):
            result = statement_result(**kwargs)
            state["observed"] = True
            if mode in ("cancelled", "failure"):
                state["status"] = "completed"
            return result

        sdk.get_statement_result = get_result
        put = sdk.put_parameter

        def put_admission(**kwargs):
            result = put(**kwargs)
            if kwargs["Name"] == admission_name(NONCE):
                state["status"] = "completed"
            return result

        sdk.put_parameter = put_admission

        class GitHub:
            def get(self, route, **kwargs):
                if route.endswith("/zip"):
                    return zipped
                if route.startswith("/actions/artifacts/"):
                    return {
                        "name": f"qualification-preflight-{RUN}",
                        "expired": False,
                        "workflow_run": {"id": RUN, "head_sha": SOURCE},
                        "digest": "sha256:" + hashlib.sha256(zipped).hexdigest(),
                    }
                if route.endswith("/cancel"):
                    state["status"] = "completed"
                    return b""
                result = {
                    "path": live.WORKFLOW,
                    "head_sha": SOURCE,
                    "head_branch": live.REF.removeprefix("refs/heads/"),
                    "event": "workflow_dispatch",
                    "run_attempt": 1,
                    "repository": {"full_name": live.REPOSITORY},
                    "status": state["status"],
                    "conclusion": mode
                    if mode in ("cancelled", "failure")
                    else "success",
                }
                if state["observed"] and not state["cleanup"]:
                    if mode.startswith("wrong-"):
                        result[mode.removeprefix("wrong-")] = "wrong"
                    if mode.startswith("final-github"):
                        tick[0] += 301 if mode == "final-github" else 3601
                return result

            def review(self, source):
                return a["review"]

            def artifact(self, kind, artifact_id, source):
                return {"workflow_run": {"id": a["artifacts"][kind]["run_id"]}}

        class ObservedHost(Host):
            def cleanup(self):
                state["cleanup"] = True
                super().cleanup()

        with (
            tempfile.TemporaryDirectory() as temp,
            patch.object(
                socket, "socket", side_effect=AssertionError("NETWORK FORBIDDEN")
            ),
        ):
            host = ObservedHost(
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
                GitHub(),
                Path(temp) / "receipt.json",
                _attempt_directory=Path(temp).resolve() / "attempts",
                clock=lambda: tick[0],
                pause=lambda n: tick.__setitem__(0, tick[0] + n),
            )
            template = Path(temp) / "policy.json"
            template.write_bytes(TEMPLATE.read_bytes())
            if mode == "preflight":
                preflight = host.preflight

                def slow(*args):
                    result = preflight(*args)
                    tick[0] += 3601
                    return result

                host.preflight = slow
            with self.assertRaises(Rejected):
                host.run(
                    preflight_id=1,
                    run=RUN,
                    source=SOURCE,
                    nonce=a["nonce"],
                    policy_template=template,
                    policy_sha256=live.file_sha(template),
                )
            self.assertNotIn("admission", sdk.events)
            if mode not in ("cancelled", "failure") and not mode.startswith("wrong-"):
                self.assertEqual(host.state["first_failure"]["reason"], "DEADLINE")
            if mode in ("cancelled", "failure") or mode.startswith("wrong-"):
                self.assertEqual(host.state["first_failure"]["stage"], "ADMISSION_RUN")
            self.assertFalse(
                [
                    (name, elapsed)
                    for name, elapsed, cleanup in events
                    if not cleanup
                    and elapsed >= 3600
                    and name
                    in {
                        "create-namespace",
                        "create-group",
                        "create-workgroup",
                        "create-usage",
                        "install-runtime-policy",
                        "host-sql",
                        "admission",
                    }
                ]
            )
            if mode.startswith("readiness-"):
                self.assertNotIn("install-runtime-policy", sdk.events)
            self.assertFalse(sdk.ns_exists or sdk.wg_exists or sdk.sg_exists)
            self.assertEqual(
                host.state["external_teardown"],
                "not-started" if mode in ("preflight", "initial-role") else "verified",
            )
            if mode.startswith("wrong-") or mode in (
                "cancelled",
                "failure",
                "final-github",
                "final-github-total",
            ):
                self.assertTrue(state["observed"])
            self.assertNotIn("PRIVATE", host.path.read_text())


class HostTest(unittest.TestCase):
    def test_invalid_initial_role_rejects_before_creation(self):
        for failure in (
            "all-actions",
            "admission-wildcard",
            "extra-grant",
            "inline-truncated",
            "managed-truncated",
            "inline-marker",
            "managed-marker",
            "trust-legacy",
            "trust-owner-id",
            "trust-repository-id",
            "trust-ref",
            "trust-wildcard",
            "trust-stringlike",
            "trust-or-legacy",
            "trust-audience",
            "trust-principal",
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
                if failure.startswith("trust-"):
                    role = sdk.get_role()
                    statement = role["Role"]["AssumeRolePolicyDocument"]["Statement"][0]
                    condition = statement["Condition"]["StringEquals"]
                    subject_key = "token.actions.githubusercontent.com:sub"
                    subject = condition[subject_key]
                    legacy_subject = (
                        "repo:vahid110/arrow-adbc:ref:"
                        "refs/heads/feature/pgwire-core-redshift"
                    )
                    if failure == "trust-legacy":
                        condition[subject_key] = legacy_subject
                    elif failure == "trust-owner-id":
                        condition[subject_key] = subject.replace(
                            "@5770674/", "@5770675/"
                        )
                    elif failure == "trust-repository-id":
                        condition[subject_key] = subject.replace(
                            "@1363017696:", "@1363017697:"
                        )
                    elif failure == "trust-ref":
                        condition[subject_key] = subject.replace(
                            "feature/pgwire-core-redshift", "main"
                        )
                    elif failure == "trust-wildcard":
                        condition[subject_key] = (
                            "repo:vahid110@5770674/arrow-adbc@1363017696:*"
                        )
                    elif failure == "trust-stringlike":
                        statement["Condition"] = {"StringLike": condition}
                    elif failure == "trust-or-legacy":
                        condition[subject_key] = [subject, legacy_subject]
                    elif failure == "trust-audience":
                        condition["token.actions.githubusercontent.com:aud"] = (
                            "wrong-audience"
                        )
                    else:
                        statement["Principal"]["Federated"] = (
                            "arn:aws:iam::385207570137:oidc-provider/other.invalid"
                        )
                    sdk.get_role = lambda **kwargs: copy.deepcopy(role)
                elif failure == "all-actions":
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
                    _attempt_directory=Path(temp).resolve() / "attempts",
                    clock=lambda: NOW,
                    pause=lambda _: None,
                )
                template = Path(temp) / "template.json"
                template.write_bytes(TEMPLATE.read_bytes())
                preflight = {
                    k: a[k]
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
        self.run_host_lifecycle()

    def test_post_usage_modifying_waits_without_repeating_mutations(self):
        for transition in (True, "namespace"):
            with self.subTest(transition=transition):
                self.run_host_lifecycle(transition=transition)

    def test_never_ready_or_unsafe_initialization_fails_before_policy_and_sql(self):
        for failure in ("stuck", "ns-stuck", "compute", "owner", "tls"):
            with self.subTest(failure=failure):
                self.run_host_lifecycle(transition=True, failure=failure)

    def test_observer_fails_before_admission_for_hidden_user_nonzero_or_sql_error(self):
        for failure in ("identity", "user", "nonzero", "sql"):
            with self.subTest(failure=failure):
                self.run_host_lifecycle(observation_failure=failure)

    def run_host_lifecycle(
        self, *, transition=False, failure=None, observation_failure=None
    ):
        a = admission()
        sdk = AWS(a)
        sdk.ns_exists = sdk.wg_exists = sdk.sg_exists = False
        sdk.get_caller_identity = lambda: {
            "Account": live.ACCOUNT,
            "Arn": (f"arn:aws:sts::{live.ACCOUNT}:assumed-role/adbc-provisioner/host"),
        }
        tick = [NOW]
        readiness = ["MODIFYING", "AVAILABLE"]
        original_workgroup = sdk.get_workgroup
        original_namespace = sdk.get_namespace
        original_usage = sdk.create_usage_limit
        usage_created = [False]

        def create_usage(**kwargs):
            result = original_usage(**kwargs)
            usage_created[0] = True
            return result

        def workgroup(**kwargs):
            result = original_workgroup(**kwargs)
            if (
                transition
                and transition != "namespace"
                and failure != "ns-stuck"
                and usage_created[0]
            ):
                w = result["workgroup"]
                w["status"] = (
                    "MODIFYING"
                    if failure
                    else (readiness.pop(0) if readiness else "AVAILABLE")
                )
                if failure == "compute":
                    w["maxCapacity"] = 8
                elif failure == "owner":
                    w["workgroupId"] = "a" * 36
                elif failure == "tls":
                    w["configParameters"][0]["parameterValue"] = "false"
            return result

        def namespace(**kwargs):
            result = original_namespace(**kwargs)
            if (transition == "namespace" or failure == "ns-stuck") and usage_created[
                0
            ]:
                result["namespace"]["status"] = (
                    "MODIFYING"
                    if failure
                    else (readiness.pop(0) if readiness else "AVAILABLE")
                )
            return result

        sdk.get_workgroup = workgroup
        sdk.get_namespace = namespace
        sdk.create_usage_limit = create_usage
        if observation_failure:
            sdk.get_statement_result = lambda **kwargs: {
                "Records": [
                    [
                        {"longValue": 0 if observation_failure == "identity" else 1},
                        {"longValue": 0 if observation_failure == "user" else 1},
                        {"longValue": 0},
                        {"longValue": 1 if observation_failure == "nonzero" else 0},
                    ]
                ]
            }
            if observation_failure == "sql":
                describe = sdk.describe_statement
                sdk.describe_statement = lambda **kwargs: (
                    {"Status": "FAILED", "Error": "PRIVATE permission denied"}
                    if "sys_session_history" in getattr(sdk, "statement_sql", "")
                    else describe(**kwargs)
                )
        github = Mock()
        cancelled = [False]

        def github_get(route, **kwargs):
            if route.endswith("/cancel"):
                cancelled[0] = True
            return {
                "path": live.WORKFLOW,
                "head_sha": SOURCE,
                "head_branch": live.REF.removeprefix("refs/heads/"),
                "event": "workflow_dispatch",
                "run_attempt": 1,
                "repository": {"full_name": live.REPOSITORY},
                "status": "completed"
                if cancelled[0] or "admission" in sdk.events
                else "in_progress",
                "conclusion": "success",
            }

        github.get.side_effect = github_get
        preflight = {
            k: a[k]
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
                _attempt_directory=Path(temp).resolve() / "attempts",
                clock=lambda: tick[0],
                pause=lambda seconds: tick.__setitem__(0, tick[0] + seconds),
            )
            template = Path(temp) / "policy.json"
            template.write_bytes(TEMPLATE.read_bytes())
            if failure:
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
                self.assertEqual(
                    host.state["first_failure"]["stage"], "INITIAL_RESOURCES"
                )
                self.assertEqual(
                    host.state["first_failure"]["reason"],
                    {
                        "stuck": "DEADLINE",
                        "ns-stuck": "DEADLINE",
                        "compute": "COMPUTE_CONTROLS",
                        "owner": "WORKGROUP_OWNERSHIP",
                        "tls": "SQL_CONTROLS",
                    }[failure],
                )
                self.assertFalse(
                    set(sdk.events)
                    & {"install-runtime-policy", "host-sql", "admission", "credentials"}
                )
                self.assertLessEqual(tick[0] - NOW, 120)
                if failure == "owner":
                    self.assertTrue(sdk.wg_exists)
                    self.assertNotIn("delete-workgroup", sdk.events)
                    self.assertTrue(
                        host.state["external_teardown"].startswith("unknown")
                    )
                else:
                    self.assertFalse(sdk.ns_exists or sdk.wg_exists or sdk.sg_exists)
                    self.assertEqual(host.state["external_teardown"], "verified")
                return
            if observation_failure:
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
                self.assertEqual(
                    host.state["first_failure"],
                    {
                        "stage": "OBSERVER",
                        "reason": "SQL_FAILED"
                        if observation_failure == "sql"
                        else "OBSERVER_NONZERO"
                        if observation_failure == "nonzero"
                        else "OBSERVER_PROOF_INVALID",
                    },
                )
                self.assertFalse(
                    set(sdk.events)
                    & {"admission", "claim", "credentials", "cancel-host-sql"}
                )
                self.assertEqual(sdk.events.count("install-runtime-policy"), 1)
                self.assertIn("remove-runtime-policy", sdk.events)
                self.assertEqual(host.state["external_teardown"], "verified")
                self.assertFalse(sdk.ns_exists or sdk.wg_exists or sdk.sg_exists)
                self.assertNotIn("PRIVATE", host.path.read_text())
                if observation_failure == "sql":
                    self.assertEqual(host.state["statements"][-1]["terminal"], "FAILED")
                    self.assertEqual(
                        host.state["independent_observation"]["failure_reason"],
                        "SQL_FAILED",
                    )
                return
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
            self.assertEqual(sdk.events.count("create-workgroup"), 1)
            self.assertEqual(sdk.events.count("create-usage"), 1)
            self.assertEqual(tick[0], NOW + (5 if transition else 0))
            self.assertNotIn("cancel-host-sql", sdk.events)
            self.assertNotIn("pricePerformanceTarget", sdk.workgroup_request)
            self.assertIs(
                sdk.workgroup_request["extraComputeForAutomaticOptimization"], False
            )
            self.assertEqual(sdk.workgroup_request["baseCapacity"], 4)
            self.assertEqual(sdk.workgroup_request["maxCapacity"], 4)
            self.assertTrue(result["independent_observation"]["fixtures_absent"])
            self.assertEqual(sdk.events.count("host-sql"), 4)
            self.assertLess(
                sdk.events.index("independent-observation"),
                sdk.events.index("admission"),
            )
            self.assertTrue(
                result["pre_admission_observation"]["admin_visibility_verified"]
            )
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

    def test_conflicting_workgroup_controls_cleanup_without_admission_or_retry(self):
        a = admission()
        sdk = AWS(a)
        sdk.ns_exists = sdk.wg_exists = sdk.sg_exists = False
        sdk.get_caller_identity = lambda: {
            "Account": live.ACCOUNT,
            "Arn": f"arn:aws:sts::{live.ACCOUNT}:assumed-role/adbc-provisioner/host",
        }
        create = sdk.create_workgroup

        def conflicting(**kwargs):
            kwargs["pricePerformanceTarget"] = {"status": "DISABLED"}
            return create(**kwargs)

        sdk.create_workgroup = conflicting
        github = Mock()
        github.get.return_value = {"status": "completed", "conclusion": "cancelled"}
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
                _attempt_directory=Path(temp).resolve() / "attempts",
                clock=lambda: NOW,
                pause=lambda _: None,
            )
            with (
                patch.object(host, "preflight", return_value={"nonce": NONCE}),
                self.assertRaises(Rejected),
            ):
                host.run(
                    preflight_id=1,
                    run=RUN,
                    source=SOURCE,
                    nonce=a["nonce"],
                    policy_template=TEMPLATE,
                    policy_sha256=live.file_sha(TEMPLATE),
                )
            self.assertEqual(sdk.events.count("create-workgroup"), 1)
            self.assertFalse(
                set(sdk.events)
                & {
                    "create-usage",
                    "install-runtime-policy",
                    "admission",
                    "claim",
                    "credentials",
                    "host-sql",
                }
            )
            self.assertFalse(sdk.ns_exists or sdk.wg_exists or sdk.sg_exists)
            self.assertEqual(host.state["external_teardown"], "verified")
            self.assertNotIn("workgroup", host.state["created"])
            self.assertIn("delete-namespace", sdk.events)
            self.assertIn("delete-group", sdk.events)
            original_receipt = host.path.read_bytes()
            with self.assertRaisesRegex(Rejected, "existing host receipt"):
                host.run(
                    preflight_id=1,
                    run=RUN,
                    source=SOURCE,
                    nonce=a["nonce"],
                    policy_template=TEMPLATE,
                    policy_sha256=live.file_sha(TEMPLATE),
                )
            self.assertEqual(host.path.read_bytes(), original_receipt)
            self.assertEqual(sdk.events.count("create-workgroup"), 1)

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
                _attempt_directory=Path(temp).resolve() / "attempts",
                clock=lambda: NOW,
                pause=lambda _: None,
            )
            with (
                patch.object(host, "preflight", return_value={"nonce": NONCE}),
                self.assertRaises(Rejected),
            ):
                host.run(
                    preflight_id=1,
                    run=RUN,
                    source=SOURCE,
                    nonce=a["nonce"],
                    policy_template=TEMPLATE,
                    policy_sha256=live.file_sha(TEMPLATE),
                )
            self.assertFalse(sdk.ns_exists)
            self.assertNotIn("admission", sdk.events)
            self.assertTrue(host.path.exists())


if __name__ == "__main__":
    unittest.main()
