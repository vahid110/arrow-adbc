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

"""Installed-artifact preflight and offline-only Task008a rehearsal.

No cloud SDK, process launcher, database constructor or live adapter exists here.
The live entry point always rejects, before even an artifact is opened.
"""

import hashlib
import tarfile
import time
import zipfile
from pathlib import Path, PurePosixPath

from pgwire_qualification_manifest import (
    Rejected,
    admit_live,
    digest,
    manifest_digest,
    require,
    validate_structure,
)

ENTRYPOINT = "AdbcDriverRedshiftInit"


def file_sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _member(name):
    path = PurePosixPath(name)
    require(
        not path.is_absolute() and ".." not in path.parts and str(path) != ".",
        "unsafe archive member",
    )
    return str(path)


def _inside(root, name):
    path = root / name
    require(root.resolve() in path.resolve().parents, "installed path escapes prefix")
    return path


def verify_installed(artifact, root, claims, *, wheel):
    """Compare installed bytes/links with the reviewed archive, without loading it."""
    root = Path(root)
    require(file_sha(artifact) == claims["sha256"], "artifact checksum mismatch")
    tree = {}
    if wheel:
        with zipfile.ZipFile(artifact) as archive:
            for item in archive.infolist():
                if item.is_dir():
                    continue
                name = _member(item.filename)
                require(
                    "__pycache__" not in PurePosixPath(name).parts
                    and PurePosixPath(name).suffix not in (".pyc", ".pyo"),
                    "wheel bytecode is not allowed",
                )
                require(
                    name not in tree
                    and (item.external_attr >> 16) & 0o170000 != 0o120000,
                    "duplicate member or wheel symlink",
                )
                data = archive.read(item)
                # pip rewrites RECORD; the wheel checksum already binds its original.
                if name.endswith(".dist-info/RECORD"):
                    continue
                installed = _inside(root, name)
                require(
                    not installed.is_symlink() and installed.read_bytes() == data,
                    "installed wheel mismatch",
                )
                tree[name] = hashlib.sha256(data).hexdigest()
        package = "adbc_driver_redshift/"
        require(
            {
                package + name
                for name in (
                    "__init__.py",
                    "dbapi.py",
                    "iam.py",
                    "libadbc_driver_redshift.so",
                )
            }
            <= tree.keys(),
            "incomplete installed package",
        )
        # Install with --no-compile. -B prevents writing caches, but still reads
        # them, so reject all bytecode before the future load-only probe/import.
        package_paths = list((root / "adbc_driver_redshift").rglob("*"))
        require(
            not any(
                p.name == "__pycache__" or p.suffix in (".pyc", ".pyo")
                for p in package_paths
            ),
            "installed bytecode is not allowed",
        )
        # Prevent extra source modules from shadowing the attested wheel in its root.
        actual = {str(p.relative_to(root)) for p in package_paths if p.is_file()}
        require(
            actual == {n for n in tree if n.startswith(package)},
            "extra installed package files",
        )
        library = root / package / "libadbc_driver_redshift.so"
    else:
        with tarfile.open(artifact) as archive:
            for item in archive:
                if item.isdir():
                    continue
                name = _member(item.name)
                require(name not in tree, "duplicate archive member")
                installed = _inside(root, name)
                if item.issym():
                    target = _member(str(PurePosixPath(name).parent / item.linkname))
                    _inside(root, target)
                    require(
                        installed.is_symlink()
                        and installed.readlink().as_posix() == item.linkname,
                        "installed archive link mismatch",
                    )
                    tree[name] = "symlink:" + item.linkname
                else:
                    require(item.isfile(), "unsupported archive member")
                    data = archive.extractfile(item).read()
                    require(
                        not installed.is_symlink() and installed.read_bytes() == data,
                        "installed C archive mismatch",
                    )
                    tree[name] = hashlib.sha256(data).hexdigest()
        require(
            "lib/libadbc_driver_redshift.so" in tree
            and "include/arrow-adbc/adbc.h" in tree,
            "incomplete installed C archive",
        )
        library = root / "lib/libadbc_driver_redshift.so"
    require(
        digest(tree) == claims["installed_tree_sha256"], "installed inventory mismatch"
    )
    return library.resolve()


def verify_probe(report, *, package_root, library, wheel):
    """Check a future isolated-process load-only report; never trust PATH fallback."""
    require(
        set(report) == {"library", "module", "distribution_root", "entrypoint"},
        "invalid load-only report",
    )
    require(
        report["entrypoint"] == ENTRYPOINT
        and Path(report["library"]).resolve() == library,
        "wrong driver or entrypoint",
    )
    if wheel:
        root = Path(package_root).resolve()
        require(
            Path(report["module"]).resolve()
            == root / "adbc_driver_redshift/__init__.py"
            and Path(report["distribution_root"]).resolve() == root,
            "source import or wrong distribution",
        )
    else:
        require(
            report["module"] is None and report["distribution_root"] is None,
            "wrong C probe",
        )


def preflight(manifest, paths):
    """Paths are local inputs, never URIs. TLS CA is pinned before any credentials."""
    require(
        file_sha(paths["ca"]) == manifest["tls"]["trusted_ca_sha256"], "untrusted CA"
    )
    libraries = {}
    for kind in ("c_archive", "python_wheel"):
        libraries[kind] = verify_installed(
            paths[kind]["artifact"],
            paths[kind]["root"],
            manifest["binding"][kind],
            wheel=kind == "python_wheel",
        )
    return libraries


def python_coverage(connection, arrow_table, schema):
    """Small future installed-DBAPI payload, tested with fakes only in Task008a.

    Caller owns deadline/cancel/close. Schema is the validated run-owned fixture.
    No URI or package import is accepted here. No broad DROP CASCADE cleanup.
    """
    require(
        schema.startswith("adbc_q_") and schema[7:].isdigit(), "invalid fixture schema"
    )
    with connection.cursor() as cursor:
        cursor.execute("SELECT CAST(42 AS BIGINT)")
        require(cursor.fetchone() == (42,), "query result mismatch")
        cursor.execute("SELECT $1::INTEGER + 1", (41,))
        require(cursor.fetchone() == (42,), "parameter result mismatch")
        require(bool(connection.adbc_get_info()), "missing metadata")
        # Only the validated run-owned schema is used. Unknown cleanup fails.
        cursor.execute(f'CREATE SCHEMA "{schema}"')
        try:
            cursor.adbc_ingest(
                "fixture", arrow_table, mode="create", db_schema_name=schema
            )
            cursor.execute(f'SELECT COUNT(*) FROM "{schema}"."fixture"')
            require(cursor.fetchone() == (2,), "ingest result mismatch")
        finally:
            cursor.execute(f'DROP TABLE IF EXISTS "{schema}"."fixture"')
            cursor.execute(f'DROP SCHEMA "{schema}"')


class OfflineAdapter:
    """Pure fake boundary: no SDK, SQL, sockets or processes can be constructed."""

    def __init__(self):
        self.events = []

    def step(self, event):
        self.events.append(event)

    def probe(self, kind, root, library):
        self.step("probe:" + kind)
        return {
            "library": str(library),
            "entrypoint": ENTRYPOINT,
            "module": str(Path(root).resolve() / "adbc_driver_redshift/__init__.py")
            if kind == "python_wheel"
            else None,
            "distribution_root": str(Path(root).resolve())
            if kind == "python_wheel"
            else None,
        }

    def cleanup_receipt(self, manifest):
        self.step("independent-cleanup-observation")
        return {
            "run_id": manifest["binding"]["run_id"],
            "manifest_sha256": manifest_digest(manifest),
            "attester": manifest["evidence"]["cleanup"]["attester"],
            "sessions_closed": True,
            "fixture_removed": True,
            "owned_rule_ids_removed": True,
            "external_teardown": "pending",
            "owner": manifest["cleanup"]["owner"],
        }


def rehearse(manifest, paths, adapter, *, source_sha, run_id, clock=time.time):
    """Offline rehearsal only. Even a valid synthetic manifest cannot admit live."""
    require(type(adapter) is OfflineAdapter, "only built-in offline adapter allowed")
    validate_structure(manifest, source_sha=source_sha, run_id=run_id, now=int(clock()))
    require(
        manifest["purpose"] == "offline-synthetic",
        "rehearsal requires synthetic claims",
    )
    libraries = preflight(manifest, paths)
    # Probes are mocked reports, not processes. Credential/ingress names below
    # are events only; there is no function that performs those actions.
    for kind, library in libraries.items():
        verify_probe(
            adapter.probe(kind, paths[kind]["root"], library),
            package_root=paths[kind]["root"],
            library=library,
            wheel=kind == "python_wheel",
        )
    started = False
    failed = False
    try:
        for event in (
            "temporary-serverless-IAM-helper",
            "owned-ingress",
            "C:scalar-query",
            "Python:query-params-metadata-ingest",
        ):
            require(
                clock() < manifest["window"]["query_deadline"], "query deadline reached"
            )
            started = True  # Includes lost replies/partial setup failures.
            adapter.step(event)
    except BaseException:
        failed = True
    finally:
        if started:
            for event in (
                "cancel-owned-operations",
                "close-owned-sessions",
                "remove-owned-fixtures",
                "revoke-exact-owned-/32-rule-ids",
            ):
                try:
                    adapter.step(event)
                except BaseException:
                    failed = True
            try:
                receipt = adapter.cleanup_receipt(manifest)
                expected = {
                    "run_id": run_id,
                    "manifest_sha256": manifest_digest(manifest),
                    "attester": manifest["evidence"]["cleanup"]["attester"],
                    "sessions_closed": True,
                    "fixture_removed": True,
                    "owned_rule_ids_removed": True,
                    "external_teardown": "pending",
                    "owner": manifest["cleanup"]["owner"],
                }
                require(receipt == expected, "unknown cleanup")
                require(
                    clock() <= manifest["window"]["teardown_deadline"],
                    "cleanup deadline exceeded",
                )
                adapter.step("handoff-external-teardown-owner")
            except BaseException:
                failed = True
    if failed:
        raise Rejected(
            "offline rehearsal failed; cleanup or operation requires inspection"
        ) from None
    return "offline rehearsal only; external teardown pending; no live qualification"


def run_live(manifest, *, source_sha, run_id):
    # There is intentionally no adapter construction, credentials, ingress or
    # subprocess after this gate. Future orchestration is a separately reviewed slice.
    admit_live(manifest, source_sha=source_sha, run_id=run_id)
