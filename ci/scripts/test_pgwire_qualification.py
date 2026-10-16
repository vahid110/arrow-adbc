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
import marshal
import socket
import struct
import subprocess
import sys
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
            "odbcpp-redshift-provisioned-pilot",
            "arn:aws:redshift:eu-north-1:385207570137:cluster:odbcpp-redshift-provisioned-pilot",
            "odbcpp-redshift-provisioned-pilot-subnet-group",
            "odbcpp-redshift-provisioned-pilot-parameter-group",
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

    def test_unverified_bytecode_rejected_before_probe_or_credentials(self):
        for module, flags in itertools.product(("__init__", "iam"), (0, 1)):
            with (
                self.subTest(module=module, flags=flags),
                tempfile.TemporaryDirectory() as temp,
            ):
                paths = self.artifacts(temp)
                package = paths["python_wheel"]["root"] / "adbc_driver_redshift"
                source = package / (module + ".py")
                original = source.read_bytes()
                # Actual timestamp/size and unchecked-hash pyc headers. Neither
                # kind may execute code outside the checksum-bound wheel bytes.
                header = importlib.util.MAGIC_NUMBER + struct.pack("<I", flags)
                header += (
                    struct.pack("<II", int(source.stat().st_mtime), len(original))
                    if flags == 0
                    else b"x" * 8
                )
                cached = (
                    package
                    / "__pycache__"
                    / f"{module}.{sys.implementation.cache_tag}.pyc"
                )
                cached.parent.mkdir()
                cached.write_bytes(
                    header
                    + marshal.dumps(
                        compile("value = 'unverified'", str(source), "exec")
                    )
                )
                with self.assertRaisesRegex(contract.Rejected, "bytecode"):
                    self.rehearse(paths)
                self.assertEqual(source.read_bytes(), original)
                self.assertEqual(self.adapter.events, [])
                # The load-only probe independently rejects the cache before
                # package import, metadata lookup or native-library load.
                import pgwire_qualification_probe as probe

                with (
                    patch.object(probe.importlib.metadata, "distribution") as metadata,
                    patch.object(probe.ctypes, "CDLL") as loader,
                ):
                    with self.assertRaisesRegex(ValueError, "bytecode"):
                        probe.probe(paths["python_wheel"]["root"], "python_wheel")
                    metadata.assert_not_called()
                    loader.assert_not_called()

    def test_wheel_bytecode_and_legacy_pyc_are_rejected(self):
        for cached in ("iam.pyc", "__pycache__/extra.pyc"):
            for in_wheel in (False, True):
                with (
                    self.subTest(cached=cached, in_wheel=in_wheel),
                    tempfile.TemporaryDirectory() as temp,
                ):
                    paths = self.artifacts(temp)
                    name = "adbc_driver_redshift/" + cached
                    installed = paths["python_wheel"]["root"] / name
                    installed.parent.mkdir(exist_ok=True)
                    installed.write_bytes(b"unverified bytecode")
                    if in_wheel:
                        with zipfile.ZipFile(
                            paths["python_wheel"]["artifact"], "a"
                        ) as archive:
                            archive.writestr(name, installed.read_bytes())
                        self.manifest["binding"]["python_wheel"]["sha256"] = (
                            runner.file_sha(paths["python_wheel"]["artifact"])
                        )
                        bind(self.manifest)
                    with self.assertRaisesRegex(contract.Rejected, "bytecode"):
                        self.rehearse(paths)
                    self.assertEqual(self.adapter.events, [])

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
        cursor.fetchall.side_effect = [[(42,)], [(42,)], [(1,), (1,), (2,), (2,)]]
        connection.adbc_get_info.return_value = {
            "vendor_name": "Redshift",
            "driver_name": "ADBC Redshift Driver",
            "driver_version": "test",
        }
        table = Mock()
        connection.adbc_get_table_schema.return_value = table.schema
        result = runner.python_coverage(connection, table, "adbc_q_123")
        self.assertEqual(result["cleanup"], "removed")
        self.assertEqual(cursor.adbc_ingest.call_count, 2)
        self.assertEqual(cursor.adbc_ingest.call_args.kwargs["mode"], "append")
        self.assertEqual(
            cursor.execute.call_args_list[-1].args[0], 'DROP SCHEMA "adbc_q_123"'
        )
        cursor.adbc_ingest.side_effect = RuntimeError("secret ingest failed")
        cursor.fetchall.side_effect = [[(42,)], [(42,)]]
        with self.assertRaisesRegex(contract.Rejected, "cleanup requires observation"):
            runner.python_coverage(connection, table, "adbc_q_123")
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
                original_import = __import__
                with (
                    patch(
                        "builtins.__import__",
                        side_effect=lambda name, *a, **kw: (
                            package
                            if name == "adbc_driver_redshift"
                            else original_import(name, *a, **kw)
                        ),
                    ),
                    patch.object(
                        probe.importlib.machinery.PathFinder,
                        "find_spec",
                        return_value=Mock(origin=package.__file__),
                    ),
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


class RuntimeQualificationTest(unittest.TestCase):
    """Real bounded subprocesses; no SDK, database, or network operations."""

    @classmethod
    def setUpClass(cls):
        import shutil

        compiler = shutil.which("cc")
        if compiler is None:
            raise RuntimeError("a C compiler is required for private-input regressions")
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / "private-input-test"
        source = ROOT / "c/driver/pgwire/redshift_qualification.c"
        # Compile the actual reader/target/main code with driver calls replaced
        # by fixed failures. The full driver and successful queries run in CI.
        import re

        names = set(re.findall(r"(Adbc\w+)\(", source.read_text()))
        harness = Path(cls.temp.name) / "reader.c"
        harness.write_text(
            "#define _POSIX_C_SOURCE 200809L\n"
            "#include <arrow-adbc/adbc.h>\n"
            + "\n".join(f"#define {name}(...) (1)" for name in sorted(names))
            + "\n#define main QualificationMain\n"
            + f'#include "{source}"\n'
            + "#undef main\n"
            + "int main(int argc, char** argv) {\n"
            + "  if (argc > 1) return QualificationMain(argc, argv);\n"
            + "  alarm(1); char uri[URI_LIMIT + 1];\n"
            + "  return ReadUri(uri) && TargetUri(uri, true) ? 0 : 2;\n}\n"
        )
        subprocess.run(
            [
                compiler,
                "-std=c11",
                "-I",
                str(ROOT / "c/include"),
                str(harness),
                "-o",
                str(cls.binary),
            ],
            check=True,
            capture_output=True,
            timeout=20,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_c_private_input_rejects_malformed_extra_and_targets(self):
        import pgwire_qualification_process as process

        uri = "postgresql://user:synthetic@127.0.0.1:5432/postgres?connect_timeout=10"
        valid = process.frame(uri)
        mutations = [
            b"",
            valid[:3],
            valid[:-1],
            valid + b"x",
            struct.pack("!I", 4097),
            struct.pack("!I", 4) + b"abcd",
        ]
        for replacement in (
            "user:@",
            "user:synthetic@evil",
            "user:bad%XX@",
            "user:bad\x00@",
            "user:bad\n@",
        ):
            bad = uri.replace("user:synthetic@", replacement).encode()
            mutations.append(struct.pack("!I", len(bad)) + bad)
        for payload in mutations:
            with self.subTest(size=len(payload)):
                result = subprocess.run(
                    [str(self.binary)], input=payload, capture_output=True, timeout=3
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")
        result = subprocess.run(
            [str(self.binary)], input=valid, capture_output=True, timeout=3
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        # The same bytes in a regular file are not the private pipe protocol.
        with tempfile.TemporaryFile() as stream:
            stream.write(valid)
            stream.seek(0)
            result = subprocess.run(
                [str(self.binary)], stdin=stream, capture_output=True, timeout=3
            )
        self.assertNotEqual(result.returncode, 0)
        # Main accepts only the dedicated modes and produces fixed diagnostics.
        result = subprocess.run(
            [
                str(self.binary),
                "/synthetic/driver",
                "adbc_q_42",
                "--postgresql-plumbing",
            ],
            input=valid,
            capture_output=True,
            timeout=3,
        )
        self.assertEqual(result.returncode, 1)
        self.assertEqual(
            json_load(result.stdout),
            {
                "coverage": False,
                "cleanup": "not-created",
                "mode": "postgresql-plumbing",
            },
        )
        self.assertNotIn(b"synthetic", result.stderr + result.stdout)

    def test_c_blocked_pipe_is_finite(self):
        import time

        start = time.monotonic()
        child = subprocess.Popen(
            [str(self.binary)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            self.assertNotEqual(child.wait(timeout=3), 0)
            self.assertLess(time.monotonic() - start, 3)
        finally:
            child.stdin.close()
            child.stdout.close()
            child.stderr.close()
            if child.poll() is None:
                child.kill()
            child.wait()

    def test_private_process_secret_transport_whitelist_and_reaping(self):
        import os

        import pgwire_qualification_process as process

        secret = "postgresql://user:never-print-this@localhost/db"
        script = (
            "import sys,json,struct; data=sys.stdin.buffer.read(); "
            "assert data[:4]==struct.pack('!I',len(data)-4); "
            "assert b'never-print-this' not in repr(sys.argv).encode(); "
            "assert b'never-print-this' not in "
            "repr(__import__('os').environ).encode(); "
            "print(json.dumps({'coverage':True,'cleanup':'removed','mode':'redshift'}))"
        )
        report = process.client([sys.executable, "-I", "-c", script], secret)
        self.assertEqual(report["cleanup"], "removed")
        children = []
        popen = subprocess.Popen

        def track(*args, **kwargs):
            child = popen(*args, **kwargs)
            children.append(child)
            return child

        for code in (
            "import time; time.sleep(30)",
            "import sys; sys.stdout.write('x'*10000); sys.stdout.flush()",
            "import sys; sys.stderr.write('never-print-this'); sys.exit(2)",
            'print(\'{"coverage":true,"cleanup":"unknown","mode":"redshift"}\')',
        ):
            with (
                self.subTest(code=code),
                patch.object(process.subprocess, "Popen", track),
            ):
                with self.assertRaises(contract.Rejected) as error:
                    process.client(
                        [sys.executable, "-I", "-c", code], secret, seconds=0.3
                    )
                self.assertNotIn("never-print-this", str(error.exception))
                self.assertIsNotNone(children[-1].returncode)
                with self.assertRaises(ProcessLookupError):
                    os.kill(children[-1].pid, 0)
        # Cancel while a real child is alive: the finally path must reap it.
        with (
            patch.object(process.subprocess, "Popen", track),
            patch.object(
                process.selectors.DefaultSelector,
                "select",
                side_effect=KeyboardInterrupt,
            ),
        ):
            with self.assertRaises(contract.Rejected):
                process.private_process(
                    [sys.executable, "-I", "-c", "import time; time.sleep(30)"]
                )
        self.assertIsNotNone(children[-1].returncode)

    def test_python_owned_cleanup_failures_and_collision_are_sanitized(self):
        def connection():
            conn = Mock()
            conn.cursor.return_value = MagicMock()
            cursor = conn.cursor.return_value.__enter__.return_value
            cursor.fetchall.side_effect = [[(42,)], [(42,)], [(1,), (1,), (2,), (2,)]]
            table = Mock()
            conn.adbc_get_table_schema.return_value = table.schema
            conn.adbc_get_info.return_value = {
                "vendor_name": "Redshift",
                "driver_name": "ADBC Redshift Driver",
                "driver_version": "test",
            }
            return conn, cursor, table

        for failure in ("CREATE SCHEMA", "DROP TABLE", "DROP SCHEMA", "SELECT value"):
            conn, cursor, table = connection()
            cursor.execute.side_effect = lambda sql, *args: (
                (_ for _ in ()).throw(RuntimeError("never-print-this"))
                if sql.startswith(failure)
                else None
            )
            with (
                self.subTest(failure=failure),
                self.assertRaises(contract.Rejected) as error,
            ):
                runner.python_coverage(conn, table, "adbc_q_42")
            self.assertNotIn("never-print-this", str(error.exception))
            commands = [call.args[0] for call in cursor.execute.call_args_list]
            if failure == "CREATE SCHEMA":
                self.assertFalse(any(sql.startswith("DROP") for sql in commands))
            else:
                self.assertIn('DROP SCHEMA "adbc_q_42"', commands)
        conn, cursor, table = connection()
        conn.adbc_get_info.return_value["driver_name"] = "Wrong driver"
        with self.assertRaises(contract.Rejected):
            runner.python_coverage(conn, table, "adbc_q_42")
        self.assertFalse(
            any(
                call.args[0].startswith("CREATE")
                for call in cursor.execute.call_args_list
            )
        )

    def test_serverless_payload_fake_expiry_and_live_boundary(self):
        from datetime import datetime, timezone

        import pgwire_qualification_process as process

        spec = importlib.util.spec_from_file_location(
            "offline_iam",
            ROOT / "python/adbc_driver_redshift/adbc_driver_redshift/iam.py",
        )
        iam = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(iam)
        manifest = fixture()
        client = Mock()
        with tempfile.TemporaryDirectory() as temp:
            ca = Path(temp) / "ca.pem"
            ca.write_text("synthetic pinned CA")
            manifest["tls"]["trusted_ca_sha256"] = runner.file_sha(ca)
            bind(manifest)
            client.get_credentials.return_value = {
                "dbUser": "fake-user",
                "dbPassword": "never-print-this",
                "expiration": datetime.fromtimestamp(NOW + 900, timezone.utc),
            }
            uri = process._serverless_uri_payload(
                client, iam, manifest, ca, source_sha=SOURCE, run_id=RUN, now=NOW
            )
            process.validate_uri(uri, manifest, ca)
            self.assertEqual(
                client.get_credentials.call_args.kwargs["durationSeconds"], 900
            )
            for expiration in (
                None,
                datetime.fromtimestamp(NOW + 901, timezone.utc),
                datetime.fromtimestamp(NOW + 100, timezone.utc),
            ):
                client.get_credentials.return_value["expiration"] = expiration
                with self.assertRaises(contract.Rejected):
                    process._serverless_uri_payload(
                        client,
                        iam,
                        manifest,
                        ca,
                        source_sha=SOURCE,
                        run_id=RUN,
                        now=NOW,
                    )
            client.reset_mock()
            with patch.object(
                process, "admit_live", side_effect=contract.Rejected("closed")
            ):
                with self.assertRaises(contract.Rejected):
                    process.serverless_uri(
                        client, iam, manifest, ca, source_sha=SOURCE, run_id=RUN
                    )
            client.get_credentials.assert_not_called()


class ExternalEvidenceTest(unittest.TestCase):
    """Synthetic external observations can compare, but can never admit a run."""

    def observations(self):
        import pgwire_qualification_evidence as evidence

        manifest = fixture()
        checks = [
            {
                "name": "original-required-check",
                "id": 42,
                "head_sha": "c" * 40,
                "conclusion": "success",
                "response_sha256": HASH,
            }
        ]
        manifest["binding"]["ci_receipt_sha256"] = contract.digest(checks)
        bind(manifest)
        source = {
            "repository": "vahid110/arrow-adbc",
            "ref": manifest["binding"]["ref"],
            "merged_sha": SOURCE,
            "merged_tree": "d" * 40,
            "reviewed_head": "c" * 40,
            "reviewed_tree": "d" * 40,
            "pr": 14,
            "original_checks": checks,
            "native_review": {
                "run": "synthetic-native-review",
                "head_sha": "c" * 40,
                "verdict": "approved",
                "receipt_sha256": HASH,
            },
            "host_comment": {
                "id": 43,
                "event": "COMMENT",
                "head_sha": "c" * 40,
                "native_receipt_sha256": HASH,
                "body_sha256": HASH,
                "response_sha256": HASH,
            },
        }
        artifacts = {}
        for kind in ("c_archive", "python_wheel"):
            binding = manifest["binding"][kind]
            root = "/synthetic/site-packages" if kind == "python_wheel" else None
            artifacts[kind] = {
                "artifact_id": 44,
                "archive_sha256": HASH,
                "payload_sha256": binding["sha256"],
                "installed_tree_sha256": binding["installed_tree_sha256"],
                "source_sha": SOURCE,
                "source_tree": "d" * 40,
                "workflow": binding["workflow"],
                "workflow_ref": "vahid110/arrow-adbc/"
                + binding["workflow"]
                + "@"
                + manifest["binding"]["ref"],
                "run_id": binding["run_id"],
                "run_attempt": 1,
                "run_conclusion": "success",
                "artifact_name": kind,
                "library": (
                    root + "/adbc_driver_redshift/libadbc_driver_redshift.so"
                    if root
                    else "/synthetic/prefix/lib/libadbc_driver_redshift.so"
                ),
                "module": root + "/adbc_driver_redshift/__init__.py" if root else None,
                "distribution_root": root,
            }
        cleanup = {
            "owner": manifest["cleanup"]["owner"],
            "external_provisioner": manifest["cleanup"]["external_provisioner"],
            "observer": manifest["evidence"]["cleanup"]["attester"],
            "deadline": manifest["window"]["teardown_deadline"],
            "manifest_sha256": contract.manifest_digest(manifest),
            "run_id": RUN,
            "state": "accepted-exact-owned-fixtures-rules-and-new-resources-only",
        }
        resources = {
            k: manifest[k]
            for k in (
                "account",
                "region",
                "namespace",
                "workgroup",
                "role",
                "controls",
                "tls",
            )
        }
        for kind, value in (
            ("provisioning", resources),
            ("usage", manifest["controls"]),
            ("cleanup", cleanup),
            ("source_artifacts", {"source": source, "artifacts": artifacts}),
        ):
            manifest["evidence"][kind]["observations_sha256"] = contract.digest(value)
        observation = {
            "version": 1,
            "consumer": evidence.CONSUMER,
            "manifest_sha256": contract.manifest_digest(manifest),
            "run_id": RUN,
            "observed_at": NOW,
            "expires_at": manifest["window"]["teardown_deadline"],
            "nonce": HASH,
            "source": source,
            "artifacts": artifacts,
            "resources": resources,
            "cleanup": manifest["cleanup"],
            "cleanup_acceptance": cleanup,
            "paid": {
                "manifest_sha256": contract.manifest_digest(manifest),
                "run_id": RUN,
                "nonce": HASH,
                "window": manifest["window"],
                "controls": manifest["controls"],
                "automatic_retries": 0,
            },
        }
        return manifest, observation

    def test_matching_claims_remain_untrusted_and_signed_unlock_is_absent(self):
        import pgwire_qualification_evidence as evidence

        manifest, observation = self.observations()
        self.assertEqual(
            evidence.compare_observations(
                manifest, observation, source_sha=SOURCE, run_id=RUN, now=NOW
            ),
            "untrusted observations match; NOT admission",
        )
        envelope = {
            "payload": observation,
            "signatures": {
                role: {"issuer": "caller-supplied", "signature": "caller-supplied"}
                for role in evidence.ROLES
            },
        }
        with self.assertRaisesRegex(contract.Rejected, "trust roots unavailable"):
            evidence.verify_external(
                manifest, envelope, source_sha=SOURCE, run_id=RUN, now=NOW
            )
        # Even hypothetical enrolled signatures do not implement durable one-shot
        # consumption: no successful verification token is exposed by this API.
        self.assertEqual(evidence.TRUST_ROOTS, {})

    def test_external_replay_expiry_source_review_artifact_and_cleanup_mismatch(self):
        import pgwire_qualification_evidence as evidence

        for key, value in (
            ("run_id", RUN + 1),
            ("observed_at", NOW - 901),
            ("expires_at", NOW),
            ("source.reviewed_tree", "e" * 40),
            ("source.native_review.verdict", "complete"),
            ("source.host_comment.event", "APPROVE"),
            ("artifacts.python_wheel.source_sha", "e" * 40),
            ("artifacts.c_archive.run_conclusion", "failure"),
            ("cleanup_acceptance.run_id", RUN + 1),
            ("paid.automatic_retries", 1),
        ):
            manifest, observation = self.observations()
            target = observation
            parts = key.split(".")
            for part in parts[:-1]:
                target = target[part]
            target[parts[-1]] = value
            with self.subTest(field=key), self.assertRaises(contract.Rejected):
                evidence.compare_observations(
                    manifest, observation, source_sha=SOURCE, run_id=RUN, now=NOW
                )
        manifest, observation = self.observations()
        with self.assertRaises(contract.Rejected):
            evidence.compare_observations(
                manifest, observation, source_sha=SOURCE, run_id=RUN, now=NOW + 3601
            )

    def test_machine_contract_tracks_manifest_and_is_not_admission(self):
        schema = json_load(
            (ROOT / "dev/pgwire/qualification-contract-v1.json").read_bytes()
        )
        self.assertEqual(
            schema["$defs"]["Manifest"]["required"], list(contract.schema())
        )
        self.assertEqual(
            schema["$defs"]["Manifest"]["properties"]["purpose"],
            {"enum": ["offline-synthetic", "dedicated-qualification"]},
        )
        self.assertEqual(schema["$defs"]["Manifest"]["additionalProperties"], False)
        self.assertIn("Codex", schema["x-producer-inputs"]["native"])
        self.assertNotIn("Authoritative AO", schema["x-producer-inputs"]["native"])


class UriParserRegressionTest(unittest.TestCase):
    """No connections: compare the guard to libpq's actual URI parser only."""

    def test_blank_duplicate_unknown_options_rejected_in_helpers_and_private_probe(
        self,
    ):
        from urllib.parse import urlencode

        import pgwire_qualification_process as process

        manifest = contract.schema()
        dedicated = (
            "postgresql://fake:synthetic@"
            + manifest["tls"]["hostname"]
            + ":5439/adbc_qualification?"
            + urlencode(
                {
                    "sslmode": "verify-full",
                    "sslrootcert": "/tmp/ca.pem",
                    "connect_timeout": "10",
                }
            )
        )
        local = "postgresql://fake:synthetic@127.0.0.1:5432/postgres?connect_timeout=10"
        harness = (
            "import sys; sys.path.insert(0," + repr(str(ROOT / "ci/scripts")) + "); "
            "from pgwire_qualification_probe import read_connection_uri; "
            "import socket; socket.socket=lambda *a,**k:sys.exit(99); "
            "read_connection_uri(sys.argv[1]); print('accepted')"
        )
        for uri, operation in (
            (dedicated, "--redshift"),
            (local, "--postgresql-plumbing"),
        ):

            def validate(value):
                if operation == "--redshift":
                    process.validate_uri(value, manifest, "/tmp/ca.pem")
                else:
                    process.validate_plumbing_uri(value)

            validate(uri)
            valid = subprocess.run(
                [sys.executable, "-I", "-B", "-c", harness, operation],
                input=process.frame(uri),
                capture_output=True,
                timeout=3,
            )
            self.assertEqual(valid.returncode, 0, valid.stderr)
            self.assertEqual(valid.stdout, b"accepted\n")
            for suffix in (
                "&host=",
                "&host=localhost",
                "&user=",
                "&password=",
                "&sslmode=",
                "&sslmode=disable",
                "&connect_timeout=",
                "&connect_timeout=10",
                "&sslrootcert=",
                "&service=",
                "&host",
                "&%68ost=",
                "&connect_timeout=%20",
            ):
                with self.subTest(operation=operation, suffix=suffix):
                    with self.assertRaises(ValueError):
                        validate(uri + suffix)
                    result = subprocess.run(
                        [sys.executable, "-I", "-B", "-c", harness, operation],
                        input=process.frame(uri + suffix),
                        capture_output=True,
                        timeout=3,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertNotEqual(result.returncode, 99)  # No socket operation.
                    self.assertNotIn(b"accepted", result.stdout)

    def test_effective_libpq_target_tls_timeout_are_the_validated_values(self):
        import ctypes
        import ctypes.util
        from urllib.parse import urlencode

        import pgwire_qualification_process as process

        class Option(ctypes.Structure):
            _fields_ = [
                (name, ctypes.c_char_p)
                for name in (
                    "keyword",
                    "envvar",
                    "compiled",
                    "val",
                    "label",
                    "dispchar",
                )
            ]
            _fields_ += [("dispsize", ctypes.c_int)]

        library = ctypes.util.find_library("pq")
        if (
            library is None
            and Path("/opt/homebrew/opt/libpq/lib/libpq.dylib").is_file()
        ):
            library = "/opt/homebrew/opt/libpq/lib/libpq.dylib"
        self.assertIsNotNone(
            library, "libpq parser is a required offline test dependency"
        )
        pq = ctypes.CDLL(library)
        pq.PQconninfoParse.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p)]
        pq.PQconninfoParse.restype = ctypes.POINTER(Option)
        pq.PQconninfoFree.argtypes = [ctypes.POINTER(Option)]
        pq.PQfreemem.argtypes = [ctypes.c_void_p]
        manifest = contract.schema()
        uri = (
            "postgresql://fake:synthetic@"
            + manifest["tls"]["hostname"]
            + ":5439/adbc_qualification?"
            + urlencode(
                {
                    "sslmode": "verify-full",
                    "sslrootcert": "/tmp/ca.pem",
                    "connect_timeout": "10",
                }
            )
        )
        for suffix, changed in (
            ("", None),
            ("&host=", "host"),
            ("&sslmode=", "sslmode"),
            ("&connect_timeout=", "connect_timeout"),
        ):
            error = ctypes.c_char_p()
            options = pq.PQconninfoParse((uri + suffix).encode(), ctypes.byref(error))
            self.assertTrue(options)
            try:
                actual = {}
                index = 0
                while options[index].keyword:
                    item = options[index]
                    actual[item.keyword.decode()] = (
                        item.val.decode() if item.val else ""
                    )
                    index += 1
                if changed is None:
                    process.validate_uri(uri, manifest, "/tmp/ca.pem")
                    self.assertEqual(actual["host"], manifest["tls"]["hostname"])
                    self.assertEqual(actual["sslmode"], "verify-full")
                    self.assertEqual(actual["sslrootcert"], "/tmp/ca.pem")
                    self.assertEqual(actual["connect_timeout"], "10")
                else:
                    self.assertEqual(actual[changed], "")
                    with self.assertRaises(ValueError):
                        process.validate_uri(uri + suffix, manifest, "/tmp/ca.pem")
            finally:
                pq.PQconninfoFree(options)
                if error:
                    pq.PQfreemem(error)


class BoundResultLifecycleTest(unittest.TestCase):
    """A fetched last row is not EOF: metadata requires the bound lease released."""

    @staticmethod
    def connection():
        connection = Mock()
        connection.cursor.return_value = MagicMock()
        cursor = connection.cursor.return_value.__enter__.return_value
        state = {"active_bound": False, "rows": []}
        table = Mock()
        connection.adbc_get_table_schema.return_value = table.schema

        def execute(sql, parameters=None):
            # DBAPI execute releases its own previous result. A bound stream
            # owns the connection until a subsequent fetch observes EOF/close.
            state["active_bound"] = parameters is not None
            state["rows"] = (
                [(1,), (1,), (2,), (2,)]
                if sql.startswith("SELECT value")
                else [(42,)]
                if sql.startswith("SELECT")
                else []
            )

        def fetchone():
            if state["rows"]:
                return state["rows"].pop(0)
            state["active_bound"] = False
            return None

        def fetchall():
            rows = []
            while (row := fetchone()) is not None:
                rows.append(row)
            return rows

        def metadata():
            if state["active_bound"]:
                raise RuntimeError("release bound result before metadata")
            return {
                "vendor_name": "Redshift",
                "driver_name": "ADBC Redshift Driver",
                "driver_version": "test",
            }

        cursor.execute.side_effect = execute
        cursor.fetchone.side_effect = fetchone
        cursor.fetchall.side_effect = fetchall
        connection.adbc_get_info.side_effect = metadata
        return connection, cursor, table

    def test_metadata_follows_bound_result_eof(self):
        connection, cursor, _ = self.connection()
        # Reproduce the former call ordering: one fetched result row still
        # holds its bound stream lease, even though no data rows remain.
        cursor.execute("SELECT CAST($1 AS BIGINT) + CAST(1 AS BIGINT)", (41,))
        self.assertEqual(cursor.fetchone(), (42,))
        with self.assertRaisesRegex(RuntimeError, "release bound result"):
            connection.adbc_get_info()
        self.assertIsNone(cursor.fetchone())
        self.assertEqual(connection.adbc_get_info()["vendor_name"], "Redshift")

        connection, cursor, table = self.connection()
        result = runner.python_coverage(connection, table, "adbc_q_42")
        self.assertEqual(
            result, {"coverage": True, "cleanup": "removed", "mode": "redshift"}
        )
        connection.adbc_get_info.assert_called_once()
        self.assertEqual(cursor.adbc_ingest.call_count, 2)
        self.assertEqual(
            cursor.execute.call_args_list[-1].args[0], 'DROP SCHEMA "adbc_q_42"'
        )

    def test_full_scalar_and_bound_results_reject_extra_or_wrong_rows(self):
        for scalar, bound in (
            ([(42,), (42,)], [(42,)]),
            ([(42,)], [(42,), (42,)]),
            ([(0,)], [(42,)]),
            ([(42,)], [(0,)]),
        ):
            connection, cursor, table = self.connection()
            cursor.fetchall.side_effect = [scalar, bound]
            with (
                self.subTest(scalar=scalar, bound=bound),
                self.assertRaises(contract.Rejected),
            ):
                runner.python_coverage(connection, table, "adbc_q_42")
            connection.adbc_get_info.assert_not_called()
            self.assertFalse(
                any(
                    call.args[0].startswith("CREATE")
                    for call in cursor.execute.call_args_list
                )
            )


def json_load(value):
    import json

    return json.loads(value)


if __name__ == "__main__":
    unittest.main()
