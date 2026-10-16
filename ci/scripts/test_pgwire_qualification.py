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

"""Synthetic offline claims only: no provisioned identities or admission proof."""

import contextlib
import copy
import hashlib
import importlib.util
import io
import itertools
import socket
import subprocess
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import MagicMock, Mock, patch

import pgwire_qualification as runner
import pgwire_qualification_manifest as contract

NOW = 1800000000
SOURCE = "a" * 40
RUN = 123
HASH = "b" * 64
ROOT = Path(__file__).resolve().parents[2]


def bind(manifest):
    for key, receipt in manifest["evidence"].items():
        if key != "kind":
            receipt["manifest_sha256"] = contract.manifest_digest(manifest)
    return manifest


def fixture():
    # Deliberately synthetic. These UUIDs/SG/hash claims are not AWS evidence.
    wg_id = "11111111-1111-1111-1111-111111111111"
    ns_id = "22222222-2222-2222-2222-222222222222"
    prefix = "arn:aws:redshift-serverless:eu-north-1:385207570137:"
    wg = prefix + "workgroup/" + wg_id
    ns = prefix + "namespace/" + ns_id
    host = (
        "adbc-qualification-20261016.385207570137.eu-north-1."
        "redshift-serverless.amazonaws.com"
    )
    owner = "arn:aws:iam::385207570137:role/offline-synthetic-owner"
    artifact = {
        "sha256": HASH,
        "run_id": 17,
        "source_sha": SOURCE,
        "installed_tree_sha256": HASH,
        "entrypoint": "AdbcDriverRedshiftInit",
    }
    receipt = {
        "attester": "arn:aws:iam::385207570137:role/offline-synthetic-attester",
        "observed_at": NOW - 1,
        "expires_at": NOW + 3600,
        "manifest_sha256": HASH,
        "observations_sha256": HASH,
        "review_receipt_sha256": HASH,
    }
    return bind(
        {
            "version": 1,
            "purpose": "offline-synthetic",
            "account": "385207570137",
            "region": "eu-north-1",
            "namespace": {
                "name": "adbc-qualification-20261016",
                "id": ns_id,
                "arn": ns,
                "database": "adbc_qualification",
            },
            "workgroup": {
                "name": "adbc-qualification-20261016",
                "id": wg_id,
                "arn": wg,
                "namespace_id": ns_id,
                "endpoint": host,
                "port": 5439,
                "security_group_name": "adbc-qualification-20261016",
                "security_group_id": "sg-11111111111111111",
            },
            "role": {
                "arn": contract.ROLE,
                "oidc_provider": (
                    "arn:aws:iam::385207570137:oidc-provider/"
                    "token.actions.githubusercontent.com"
                ),
                "audience": "sts.amazonaws.com",
                "subject": contract.SUBJECT,
                "policy_sha256": HASH,
                "trust_sha256": HASH,
                "authorization_resources": [
                    wg,
                    ns,
                    "arn:aws:ec2:eu-north-1:385207570137:security-group/sg-11111111111111111",
                ],
            },
            "controls": {
                "base_rpu": 4,
                "max_rpu": 4,
                "automatic_optimization": False,
                "capacity_growth": False,
                "usage_limit_id": "33333333-3333-3333-3333-333333333333",
                "usage_workgroup_arn": wg,
                "usage_period": "daily",
                "usage_rpu_hours": 4,
                "usage_action": "deactivate",
                "incremental_usd": "5.00",
                "query_seconds": 900,
                "compute_seconds": 3600,
                "paid_retries": 0,
            },
            "binding": {
                "repository": "vahid110/arrow-adbc",
                "ref": "refs/heads/feature/pgwire-core-redshift",
                "source_sha": SOURCE,
                "merged_sha": SOURCE,
                "ci_receipt_sha256": HASH,
                "independent_review_receipt_sha256": HASH,
                "run_id": RUN,
                "platform": "ubuntu-x86_64",
                "c_archive": {
                    **artifact,
                    "workflow": ".github/workflows/pgwire-package.yml",
                },
                "python_wheel": {
                    **artifact,
                    "workflow": ".github/workflows/pgwire-python-wheels.yml",
                },
            },
            "window": {
                "not_before": NOW - 10,
                "admit_before": NOW + 60,
                "query_deadline": NOW + 890,
                "compute_deadline": NOW + 3590,
                "teardown_deadline": NOW + 3590,
            },
            "cleanup": {
                "owner": owner,
                "external_provisioner": owner,
                "fixture_schema": "adbc_q_123",
                "ingress": "exact-owned-rule-id-and-ipv4-/32",
                "sessions": "close-owned",
                "objects": "owned-fixture-only",
                "verification": "independent-observation-required",
                "teardown": "external-new-resources-only",
            },
            "tls": {"mode": "verify-full", "hostname": host, "trusted_ca_sha256": HASH},
            "evidence": {
                "kind": "external-observations-requiring-independent-verification",
                **{
                    name: receipt.copy()
                    for name in ("provisioning", "usage", "cleanup", "source_artifacts")
                },
            },
        }
    )


class QualificationTest(unittest.TestCase):
    def setUp(self):
        self.manifest = fixture()
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        # Any accidental subprocess/network activity fails the entire suite.
        self.network = self.stack.enter_context(
            patch.object(
                socket, "socket", side_effect=AssertionError("network forbidden")
            )
        )
        self.process = self.stack.enter_context(
            patch.object(
                subprocess, "Popen", side_effect=AssertionError("process forbidden")
            )
        )
        self.adapter = runner.OfflineAdapter()

    def tearDown(self):
        self.network.assert_not_called()
        self.process.assert_not_called()

    def validate(self, manifest=None):
        return contract.validate_structure(
            manifest or self.manifest, source_sha=SOURCE, run_id=RUN, now=NOW
        )

    def rejected(self, manifest):
        with self.assertRaises(contract.Rejected):
            self.validate(manifest)
        with self.assertRaises(contract.Rejected):
            runner.rehearse(
                manifest,
                {},
                self.adapter,
                source_sha=SOURCE,
                run_id=RUN,
                clock=lambda: NOW,
            )
        self.assertEqual(self.adapter.events, [])
        self.network.assert_not_called()
        self.process.assert_not_called()

    def test_structural_claims_never_admit_live(self):
        self.validate()
        for purpose in ("offline-synthetic", "dedicated-qualification"):
            self.manifest["purpose"] = purpose
            bind(self.manifest)
            with self.assertRaisesRegex(contract.Rejected, "external verification"):
                contract.admit_live(
                    self.manifest, source_sha=SOURCE, run_id=RUN, now=NOW
                )
        with (
            patch.object(contract.time, "time", return_value=NOW),
            patch.object(runner, "preflight") as preflight,
        ):
            with self.assertRaises(contract.Rejected):
                runner.run_live(self.manifest, source_sha=SOURCE, run_id=RUN)
            preflight.assert_not_called()

    def test_missing_and_unknown_fields_every_object(self):
        def walk(value, path=()):
            if isinstance(value, dict):
                yield path, value
                for key, child in value.items():
                    yield from walk(child, (*path, key))

        for path, value in walk(self.manifest):
            for key in [*value, None]:
                with self.subTest(path=path, key=key):
                    changed = copy.deepcopy(self.manifest)
                    target = changed
                    for part in path:
                        target = target[part]
                    if key is None:
                        target["unknown"] = True
                    else:
                        del target[key]
                    self.rejected(changed)

    def test_protected_identities_anywhere(self):
        protected = [
            "odbcpp-redshift-pilot",
            "odbcpp_pilot",
            "sg-06227a706295e3f05",
            "arn:aws:redshift-serverless:eu-north-1:385207570137:workgroup/d41cff82-7eb8-410d-be60-1d467c335756",
            "odbcpp-redshift-pilot.385207570137.eu-north-1.redshift-serverless.amazonaws.com",
            "arn:aws:iam::385207570137:role/adbc-redshift-ci",
        ]
        for identifier in protected:
            for field in ("endpoint", "namespace_id", "security_group_id", "name"):
                changed = copy.deepcopy(self.manifest)
                changed["workgroup"][field] = identifier
                self.rejected(changed)
            changed = copy.deepcopy(self.manifest)
            changed["role"]["authorization_resources"].append(identifier)
            self.rejected(changed)

    def test_inconsistent_claims(self):
        changes = [
            ("account", "000000000000"),
            ("region", "us-east-1"),
            ("workgroup.port", True),
            ("namespace.id", "not-a-uuid"),
            ("workgroup.namespace_id", self.manifest["workgroup"]["id"]),
            ("controls.usage_workgroup_arn", self.manifest["namespace"]["arn"]),
            ("controls.usage_action", "log"),
            ("controls.paid_retries", 1),
            ("controls.automatic_optimization", True),
            ("controls.max_rpu", 8),
            ("role.authorization_resources", ["*"]),
            ("role.subject", "repo:other:ref:refs/heads/main"),
            ("binding.source_sha", "c" * 40),
            ("binding.merged_sha", "c" * 40),
            ("binding.python_wheel.source_sha", "c" * 40),
            ("binding.c_archive.workflow", "unreviewed.yml"),
            ("binding.run_id", 124),
            ("cleanup.owner", contract.ROLE),
            ("cleanup.fixture_schema", "public"),
            ("tls.mode", "require"),
            ("tls.hostname", "localhost"),
            ("window.admit_before", NOW),
            ("window.not_before", NOW + 1),
            ("window.query_deadline", NOW + 901),
            ("window.teardown_deadline", NOW + 4000),
            ("evidence.provisioning", True),
            ("evidence.usage.observed_at", NOW - 901),
            ("evidence.cleanup.attester", contract.ROLE),
            ("evidence.source_artifacts.expires_at", NOW),
        ]
        for path, value in changes:
            with self.subTest(path=path):
                changed = copy.deepcopy(self.manifest)
                target = changed
                parts = path.split(".")
                for part in parts[:-1]:
                    target = target[part]
                target[parts[-1]] = value
                self.rejected(changed)
        changed = copy.deepcopy(self.manifest)
        changed["binding"]["c_archive"]["sha256"] = "d" * 64
        self.rejected(changed)  # Stale evidence even when hash shape is valid.

    def test_json_duplicates_and_nonfinite(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "input.json"
            for text in ('{"account":1,"account":2}', '{"x":NaN}'):
                path.write_text(text)
                with self.assertRaises(contract.Rejected):
                    contract.load(path)

    def artifacts(self, directory):
        root = Path(directory)
        paths = {"ca": root / "ca.pem"}
        paths["ca"].write_bytes(b"synthetic CA, not trusted for live")
        self.manifest["tls"]["trusted_ca_sha256"] = runner.file_sha(paths["ca"])
        contents = {
            "c_archive": {
                "lib/libadbc_driver_redshift.so": b"fake C library",
                "include/arrow-adbc/adbc.h": b"fake header",
            },
            "python_wheel": {
                **{
                    f"adbc_driver_redshift/{name}": b"# synthetic"
                    for name in ("__init__.py", "dbapi.py", "iam.py")
                },
                "adbc_driver_redshift/libadbc_driver_redshift.so": b"fake library",
                "adbc_driver_redshift-0.dist-info/METADATA": (
                    b"Name: adbc-driver-redshift"
                ),
            },
        }
        for kind, files in contents.items():
            prefix = root / kind
            prefix.mkdir()
            artifact = root / (kind + (".whl" if kind == "python_wheel" else ".tar.gz"))
            for name, data in files.items():
                installed = prefix / name
                installed.parent.mkdir(parents=True, exist_ok=True)
                installed.write_bytes(data)
            if kind == "python_wheel":
                with zipfile.ZipFile(artifact, "w") as archive:
                    for name, data in files.items():
                        archive.writestr(name, data)
            else:
                with tarfile.open(artifact, "w:gz") as archive:
                    for name, data in files.items():
                        info = tarfile.TarInfo(name)
                        info.size = len(data)
                        archive.addfile(info, io.BytesIO(data))
            claims = self.manifest["binding"][kind]
            claims["sha256"] = runner.file_sha(artifact)
            claims["installed_tree_sha256"] = contract.digest(
                {n: hashlib.sha256(d).hexdigest() for n, d in files.items()}
            )
            paths[kind] = {"artifact": artifact, "root": prefix}
        bind(self.manifest)
        return paths

    def rehearse(self, paths, clock=lambda: NOW):
        return runner.rehearse(
            self.manifest,
            paths,
            self.adapter,
            source_sha=SOURCE,
            run_id=RUN,
            clock=clock,
        )

    def test_both_installed_artifact_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            paths = self.artifacts(temp)
            self.assertIn("offline rehearsal only", self.rehearse(paths))
        for event in (
            "probe:c_archive",
            "probe:python_wheel",
            "C:scalar-query",
            "Python:query-params-metadata-ingest",
            "handoff-external-teardown-owner",
        ):
            self.assertIn(event, self.adapter.events)
        self.assertLess(
            self.adapter.events.index("probe:python_wheel"),
            self.adapter.events.index("temporary-serverless-IAM-helper"),
        )

    def test_bad_artifacts_before_credentials(self):
        for kind in ("c_archive", "python_wheel"):
            for mutation in ("checksum", "installed", "extra", "ca"):
                with (
                    self.subTest(kind=kind, mutation=mutation),
                    tempfile.TemporaryDirectory() as temp,
                ):
                    paths = self.artifacts(temp)
                    if mutation == "checksum":
                        paths[kind]["artifact"].write_bytes(b"tampered")
                    elif mutation == "installed":
                        next(
                            p for p in paths[kind]["root"].rglob("*") if p.is_file()
                        ).write_bytes(b"tampered")
                    elif mutation == "extra":
                        (
                            paths["python_wheel"]["root"]
                            / "adbc_driver_redshift/extra.py"
                        ).write_text("shadow")
                    else:
                        paths["ca"].write_text("different CA")
                    with self.assertRaises(contract.Rejected):
                        self.rehearse(paths)
                    self.assertNotIn(
                        "temporary-serverless-IAM-helper", self.adapter.events
                    )

    def test_probe_rejects_source_import_fallback_entrypoint(self):
        with tempfile.TemporaryDirectory() as temp:
            paths = self.artifacts(temp)
            lib = runner.preflight(self.manifest, paths)["python_wheel"]
            report = self.adapter.probe(
                "python_wheel", paths["python_wheel"]["root"], lib
            )
            for key, value in (
                (
                    "module",
                    str(
                        ROOT
                        / "python/adbc_driver_redshift/adbc_driver_redshift/__init__.py"
                    ),
                ),
                ("library", "/usr/lib/libadbc_driver_redshift.so"),
                ("entrypoint", "AdbcDriverInit"),
                ("distribution_root", "/another/venv"),
            ):
                changed = {**report, key: value}
                with self.assertRaises(contract.Rejected):
                    runner.verify_probe(
                        changed,
                        package_root=paths["python_wheel"]["root"],
                        library=lib,
                        wheel=True,
                    )

    def test_deadline_cancellation_and_failed_cleanup(self):
        for failure in ("deadline", "cancel", "query", "cleanup", "receipt"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                paths = self.artifacts(temp)
                self.adapter.events.clear()
                original = self.adapter.step

                def step(event):
                    original(event)
                    if event == {
                        "cancel": "C:scalar-query",
                        "query": "Python:query-params-metadata-ingest",
                        "cleanup": "remove-owned-fixtures",
                    }.get(failure):
                        raise KeyboardInterrupt("secret URI must never escape")

                clock = (
                    iter([NOW, NOW, NOW, NOW + 900, NOW + 901])
                    if failure == "deadline"
                    else itertools.repeat(NOW)
                )
                with (
                    patch.object(self.adapter, "step", side_effect=step),
                    patch.object(
                        self.adapter,
                        "cleanup_receipt",
                        wraps=self.adapter.cleanup_receipt,
                    ) as receipt,
                ):
                    if failure == "receipt":
                        receipt.return_value = {"unknown": True}
                    with self.assertRaisesRegex(
                        contract.Rejected, "offline rehearsal failed"
                    ) as caught:
                        self.rehearse(paths, clock=lambda: next(clock))
                    self.assertNotIn("secret", str(caught.exception))
                self.assertIn("close-owned-sessions", self.adapter.events)
                self.assertIn("revoke-exact-owned-/32-rule-ids", self.adapter.events)
                self.assertEqual(
                    self.adapter.events.count("temporary-serverless-IAM-helper"), 1
                )

    def test_current_iam_helper_fake_only_verified_tls(self):
        path = ROOT / "python/adbc_driver_redshift/adbc_driver_redshift/iam.py"
        spec = importlib.util.spec_from_file_location("offline_iam", path)
        iam = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(iam)
        client = Mock()
        client.get_credentials.return_value = {
            "dbUser": "synthetic",
            "dbPassword": "synthetic-secret",
        }
        response = iam.fetch_serverless_credentials(
            client,
            workgroup_name=contract.NAME,
            db_name="adbc_qualification",
            duration_seconds=900,
        )
        client.get_credentials.assert_called_once_with(
            workgroupName=contract.NAME,
            dbName="adbc_qualification",
            durationSeconds=900,
        )
        uri = iam.build_uri(
            self.manifest["tls"]["hostname"],
            "adbc_qualification",
            response["dbUser"],
            response["dbPassword"],
            sslmode="verify-full",
            extra_params={"sslrootcert": "/offline/ca.pem", "connect_timeout": "10"},
        )
        self.assertIn("sslmode=verify-full", uri)
        self.assertIn("sslrootcert=", uri)

    def test_python_payload_and_owned_fixture_cleanup(self):
        connection = Mock()
        connection.cursor.return_value = MagicMock()
        cursor = connection.cursor.return_value.__enter__.return_value
        cursor.fetchone.side_effect = [(42,), (42,), (2,)]
        runner.python_coverage(connection, "fake arrow table", "adbc_q_123")
        connection.adbc_get_info.assert_called_once()
        cursor.adbc_ingest.assert_called_once_with(
            "fixture", "fake arrow table", mode="create", db_schema_name="adbc_q_123"
        )
        self.assertEqual(
            cursor.execute.call_args_list[-1].args[0], 'DROP SCHEMA "adbc_q_123"'
        )
        cursor.adbc_ingest.side_effect = RuntimeError("fixture ingest failed")
        cursor.fetchone.side_effect = [(42,), (42,)]
        with self.assertRaises(RuntimeError):
            runner.python_coverage(connection, "fake", "adbc_q_123")
        self.assertIn(
            'DROP TABLE IF EXISTS "adbc_q_123"."fixture"',
            [c.args[0] for c in cursor.execute.call_args_list],
        )

    def test_archive_path_and_symlink_safety(self):
        for member in ("../escape", "/absolute", "duplicate", "symlink-escape"):
            with self.subTest(member=member), tempfile.TemporaryDirectory() as temp:
                paths = self.artifacts(temp)
                artifact = paths["c_archive"]["artifact"]
                with tarfile.open(artifact, "w:gz") as archive:
                    name = (
                        "lib/libadbc_driver_redshift.so"
                        if member in ("duplicate", "symlink-escape")
                        else member
                    )
                    info = tarfile.TarInfo(name)
                    if member == "symlink-escape":
                        info.type = tarfile.SYMTYPE
                        info.linkname = "../../outside"
                        archive.addfile(info)
                    else:
                        info.size = 1
                        archive.addfile(info, io.BytesIO(b"x"))
                        if member == "duplicate":
                            archive.addfile(info, io.BytesIO(b"x"))
                self.manifest["binding"]["c_archive"]["sha256"] = runner.file_sha(
                    artifact
                )
                bind(self.manifest)
                with self.assertRaises(contract.Rejected):
                    self.rehearse(paths)
                self.assertNotIn("temporary-serverless-IAM-helper", self.adapter.events)

    def test_load_only_probe_checks_origins_and_symbol(self):
        import pgwire_qualification_probe as probe

        with tempfile.TemporaryDirectory() as temp:
            paths = self.artifacts(temp)
            for kind in ("c_archive", "python_wheel"):
                root = paths[kind]["root"].resolve()
                library = runner.preflight(self.manifest, paths)[kind]
                package = Mock(__file__=str(root / "adbc_driver_redshift/__init__.py"))
                package._driver_path.return_value = str(library)
                distribution = Mock()
                distribution.locate_file.return_value = root
                with (
                    patch.dict("sys.modules", {"adbc_driver_redshift": package}),
                    patch.object(
                        probe.importlib.metadata,
                        "distribution",
                        return_value=distribution,
                    ),
                    patch.object(probe.ctypes, "CDLL") as loader,
                ):
                    report = probe.probe(root, kind)
                    runner.verify_probe(
                        report,
                        package_root=root,
                        library=library,
                        wheel=kind == "python_wheel",
                    )
                    loader.assert_called_once_with(str(library))
                    loader.reset_mock()
                    if kind == "python_wheel":
                        package._driver_path.return_value = "/wrong/library.so"
                        with self.assertRaises((ValueError, OSError)):
                            probe.probe(root, kind)
                        loader.assert_not_called()

    def test_workflow_has_no_legacy_fallback(self):
        workflow = (ROOT / ".github/workflows/pgwire-postgresql.yml").read_text()
        for forbidden in (
            "cpp-redshift:",
            "redshift-credential-probe:",
            "configure-aws-credentials",
            "id-token:",
            "odbcpp",
            "pgwire-redshift-copy.yml",
            "aws redshift",
        ):
            self.assertNotIn(forbidden, workflow)
        self.assertIn("retired-live-request:", workflow)
        self.assertIn("cpp-postgresql:", workflow)
        self.assertIn("cpp-macos:", workflow)
        self.assertIn("postgres:18", workflow)
        self.assertIn(
            "ctest --test-dir build/pgwire-ci --output-on-failure -L driver-redshift",
            workflow,
        )
        safety = (ROOT / ".github/workflows/pgwire-aws-safety.yml").read_text()
        self.assertIn("test_pgwire_qualification.py", safety)
        self.assertNotIn("id-token:", safety)


if __name__ == "__main__":
    unittest.main()
