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

"""Synthetic byte/process fixtures only; never load actual installed packages."""

import base64
import copy
import csv
import hashlib
import importlib.util
import io
import json
import os
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from dataclasses import FrozenInstanceError, replace
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


def sibling(name):
    path = Path(__file__).with_name(name + ".py")
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


base = sibling("test_pgwire_local_evidence")
install = sibling("pgwire_local_install")
probe = install.probe


def build_version(platform, minimum, sdk, tools=()):
    return struct.pack(
        "<6I", 0x32, 24 + len(tools) * 8, platform, minimum, sdk, len(tools)
    ) + b"".join(struct.pack("<2I", *tool) for tool in tools)


def macho(
    uuid="1" * 32, minimum=0xF0000, platform=1, cpu=probe.ARM64, extra=b"", builds=None
):
    commands = struct.pack("<II", 0x1B, 24) + bytes.fromhex(uuid)
    builds = (build_version(platform, minimum, 0xF0400),) if builds is None else builds
    commands += b"".join(builds)
    count = 1 + len(builds)
    if extra:
        commands += extra
        count += 1
    return (
        struct.pack("<8I", 0xFEEDFACF, cpu, 0, 6, count, len(commands), 0, 0) + commands
    )


def record(files, name):
    out = io.StringIO(newline="")
    writer = csv.writer(out, lineterminator="\n")
    for path, data in sorted(files.items()):
        if path == name:
            writer.writerow([path, "", ""])
        else:
            digest = (
                base64.urlsafe_b64encode(hashlib.sha256(data).digest())
                .rstrip(b"=")
                .decode()
            )
            writer.writerow([path, "sha256=" + digest, str(len(data))])
    return out.getvalue().encode()


def identity(path):
    resolved, links = install._resolve(str(path))
    data = Path(resolved).read_bytes()
    return install.FileIdentity(str(path), resolved, base.sha(data), len(data), links)


class SyntheticInstall:
    """All packages/runtime bytes and load metadata are synthetic, never imported."""

    def __init__(self, root):
        self.root = root
        self.bundle = base.SyntheticBundle(root)
        self.prefix, self.site, self.metadata = [
            root / name for name in ("prefix", "site", "metadata")
        ]
        for path in (self.prefix, self.site, self.metadata):
            path.mkdir(mode=0o700)
        (self.prefix / "lib").mkdir()
        (self.prefix / "include").mkdir()
        (self.prefix / "include/adbc.h").write_bytes(b"SYNTHETIC HEADER")
        links = []
        for family in ("manager", "postgresql", "redshift"):
            stem = "libadbc_driver_" + family
            version = stem + ".113.0.0.dylib"
            data = macho("a" * 32 if family == "redshift" else "b" * 32)
            (self.prefix / "lib" / version).write_bytes(data)
            for name, target in (
                (stem + ".dylib", stem + ".113.dylib"),
                (stem + ".113.dylib", version),
            ):
                (self.prefix / "lib" / name).symlink_to(target)
                links.append(("lib/" + name, target))
        self.links = tuple(links)
        self.client = root / "client"
        self.client.write_bytes(b"SYNTHETIC CLIENT NEVER EXECUTED")
        self.repack_c()
        self.wheels = []
        self.payloads = {}
        for role, package, tag in (
            ("manager_wheel", "adbc_driver_manager", "cp314-cp314-macosx_15_0_arm64"),
            ("redshift_wheel", "adbc_driver_redshift", "py3-none-macosx_15_0_arm64"),
        ):
            spec = install.Wheel(role, package, package + "-1.0.dist-info", tag)
            payload = {
                package + "/__init__.py": b"# SYNTHETIC NEVER IMPORTED\n",
                spec.dist_info + "/METADATA": (
                    "Name: " + package.replace("_", "-") + "\nVersion: 1.0\n"
                ).encode(),
                spec.dist_info + "/WHEEL": (
                    "Wheel-Version: 1.0\nRoot-Is-Purelib: false\nTag: " + tag + "\n"
                ).encode(),
            }
            if role == "manager_wheel":
                payload[package + "/_lib.cpython-314-darwin.so"] = macho("c" * 32)
            else:
                payload[package + "/libadbc_driver_redshift.so"] = macho("a" * 32)
            record_name = spec.dist_info + "/RECORD"
            payload[record_name] = b""
            payload[record_name] = record(payload, record_name)
            raw = io.BytesIO()
            with zipfile.ZipFile(raw, "w", zipfile.ZIP_DEFLATED) as archive:
                for name, data in payload.items():
                    archive.writestr(name, data)
            name = self.bundle.package[role].path
            blob = self.bundle.put(self.bundle.artifacts, name, raw.getvalue())
            self.bundle.package[role] = blob
            installed = dict(payload)
            installed[spec.dist_info + "/INSTALLER"] = b"pip\n"
            installed[spec.dist_info + "/REQUESTED"] = b""
            installed[spec.dist_info + "/direct_url.json"] = base.encoded(
                {
                    "archive_info": {
                        "hash": "sha256=" + blob.sha256,
                        "hashes": {"sha256": blob.sha256},
                    },
                    "url": (self.bundle.artifacts / name).as_uri(),
                }
            )
            installed[record_name] = record(installed, record_name)
            for path, data in installed.items():
                self.bundle.put(self.site, path, data)
            self.payloads[role] = payload
            self.wheels.append(spec)
        self.bundle.validation["wheels"] = [
            {
                "path": str(self.bundle.artifacts / self.bundle.package[role].path),
                "sha256": self.bundle.package[role].sha256,
            }
            for role in ("manager_wheel", "redshift_wheel")
        ]
        self.bundle.refresh()
        self.main = root / "synthetic-main"
        self.main.write_bytes(macho("d" * 32))
        paths = [
            self.prefix / "lib/libadbc_driver_redshift.113.0.0.dylib",
            self.site / "adbc_driver_redshift/libadbc_driver_redshift.so",
            self.site / "adbc_driver_manager/_lib.cpython-314-darwin.so",
            self.main,
        ]
        self.selected = install.Selected(
            self.bundle.selected,
            "8" * 40,
            "9" * 40,
            identity(Path(install.__file__).resolve()),
            identity(Path(probe.__file__).resolve()),
            identity(Path(sys.executable)),
            identity(self.client),
            "lib/libadbc_driver_redshift.dylib",
            "adbc_driver_manager/_lib.cpython-314-darwin.so",
            self.links,
            tuple(self.wheels),
            tuple(identity(path) for path in paths),
            "15.3.2",
            "24D81",
            "synthetic-abi",
        )
        self.roots = install.Roots(
            self.bundle.roots, self.prefix, self.site, self.metadata
        )
        self.transform = lambda value: value

    def repack_c(self):
        raw = io.BytesIO()
        with tarfile.open(fileobj=raw, mode="w:gz") as archive:
            archive.add(self.prefix, arcname="test-prefix", recursive=True)
            archive.add(self.client, arcname="test-client/adbc_redshift_install_smoke")
        old = self.bundle.package["c_archive"]
        self.bundle.package["c_archive"] = self.bundle.put(
            self.bundle.artifacts, old.path, raw.getvalue()
        )
        self.bundle.validation["installed_c_snapshot"]["sha256"] = self.bundle.package[
            "c_archive"
        ].sha256

    def replace_artifact(self, role, data):
        old = self.bundle.package[role]
        blob = self.bundle.put(self.bundle.artifacts, old.path, data)
        self.bundle.package[role] = blob
        if role == "c_archive":
            self.bundle.validation["installed_c_snapshot"]["sha256"] = blob.sha256
        else:
            for wheel in self.bundle.validation["wheels"]:
                if wheel["path"] == str(self.bundle.artifacts / old.path):
                    wheel["sha256"] = blob.sha256
        self.rebind_artifacts()

    def rebind_artifacts(self):
        self.bundle.refresh()
        self.selected = replace(self.selected, evidence=self.bundle.selected)

    def metadata_value(self, request):
        origins = {
            "manager": self.site / "adbc_driver_manager/__init__.py",
            "redshift": self.site / "adbc_driver_redshift/__init__.py",
            "manager_native": Path(request["manager_native"]),
            "c_library": Path(request["c_library"]),
            "wheel_library": Path(request["wheel_library"]),
        }
        images = [
            {
                "index": index,
                "path": entry.path,
                **probe.parse_macho(Path(entry.resolved).read_bytes()),
            }
            for index, entry in enumerate(self.selected.runtime)
        ]
        os_facts = probe.parse_macho(macho("e" * 32, 0x120300, 6))
        images.append(
            {
                "index": len(images),
                "path": "/usr/lib/SyntheticOSImage.dylib",
                **{
                    key: os_facts[key]
                    for key in (
                        "cpu",
                        "subtype",
                        "filetype",
                        "flags",
                        "uuid",
                        "deployment",
                    )
                },
            }
        )
        return {
            "version": 1,
            "kind": "adbc-same-host-load-metadata",
            "scope": "named-load-only",
            "system": "Darwin",
            "machine": "arm64",
            "os_version": "15.3.2",
            "os_build": "24D81",
            "python": "SYNTHETIC TEST METADATA NOT ACTUAL RUNTIME",
            "abi": "synthetic-abi",
            "executable": self.selected.interpreter.path,
            "origins": {
                key: {
                    "path": str(path),
                    "sha256": base.sha(path.read_bytes()),
                    "bytes": path.stat().st_size,
                }
                for key, path in origins.items()
            },
            "images": images,
            "connection_created": False,
            "broader_closure": "unknown",
        }

    def fake_launch(self, selected, request, fd):
        value = self.transform(self.metadata_value(request))
        data = base.encoded(value)
        os.write(fd, data)
        os.fsync(fd)
        return {
            "version": 1,
            "status": "load-only-observed",
            "bytes": len(data),
            "sha256": base.sha(data),
        }

    def observe(self):
        with patch.object(install, "_launch", side_effect=self.fake_launch):
            return install.observe_install(
                self.bundle.data, selected=self.selected, roots=self.roots
            )


class LocalInstallTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.fixture = SyntheticInstall(Path(self.temp.name).resolve())

    def tearDown(self):
        self.temp.cleanup()

    def rejected(self):
        with self.assertRaises(install.Rejected) as caught:
            self.fixture.observe()
        self.assertRegex(str(caught.exception), r"^[a-z-]+$")

    def test_synthetic_public_api_binds_distinct_sources_inventories_and_runtime(self):
        f = self.fixture
        result = f.observe()
        self.assertEqual(result.status, "same-host-load-observed")
        self.assertEqual(result.artifact_head, "1" * 40)
        self.assertEqual(result.collector_head, "8" * 40)
        self.assertEqual(len(result.inventories), 3)
        self.assertEqual(len(result.used_runtime), 4)
        self.assertNotIn(f.selected.interpreter.path, result.used_runtime)
        self.assertFalse(hasattr(result, "authorized"))
        with self.assertRaises(FrozenInstanceError):
            result.status = "ready"
        # OS platform6/min18.3 stays raw; non-OS Mac15 rules do not apply.
        metadata = (f.metadata / result.metadata.path).read_bytes()
        self.assertEqual(base.sha(metadata), result.metadata.sha256)
        self.assertIn(b"SYNTHETIC TEST METADATA", metadata)

    def test_prefix_client_links_extra_missing_and_changed_bytes(self):
        f = self.fixture
        for path in (f.prefix / "include/adbc.h", f.client):
            saved = path.read_bytes()
            path.write_bytes(saved + b"changed")
            self.rejected()
            path.write_bytes(saved)
        extra = f.prefix / "extra"
        extra.write_bytes(b"extra")
        self.rejected()
        extra.unlink()
        link = f.prefix / f.links[0][0]
        target = link.readlink()
        link.unlink()
        link.symlink_to("missing")
        self.rejected()
        link.unlink()
        link.symlink_to(target)
        f.observe()

    def test_package_bytes_extra_cache_and_literal_pip_metadata(self):
        f = self.fixture
        for spec in f.wheels:
            for relative in (
                spec.package + "/__init__.py",
                spec.dist_info + "/WHEEL",
                spec.dist_info + "/METADATA",
                spec.dist_info + "/INSTALLER",
                spec.dist_info + "/REQUESTED",
                spec.dist_info + "/direct_url.json",
                spec.dist_info + "/RECORD",
            ):
                path = f.site / relative
                saved = path.read_bytes()
                path.write_bytes(saved + b"changed")
                with self.subTest(path=relative):
                    self.rejected()
                path.write_bytes(saved)
        for name in (
            "adbc_driver_manager/unexpected.py",
            "adbc_driver_redshift/__pycache__/x.pyc",
            "adbc_driver_redshift/unknown-source.py",
            f.wheels[0].dist_info + "/UNKNOWN",
        ):
            path = f.site / name
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(b"SYNTHETIC")
            self.rejected()
            path.unlink()
            if path.parent.name == "__pycache__":
                path.parent.rmdir()
        f.observe()

    def test_record_duplicate_unknown_unhashed_bad_size_and_stale_cache_rows(self):
        f = self.fixture
        path = f.site / f.wheels[0].dist_info / "RECORD"
        original = path.read_bytes()
        altered_rows = list(csv.reader(io.StringIO(original.decode())))
        altered_rows[0][2] = "999999"
        invalid_size = io.StringIO(newline="")
        csv.writer(invalid_size).writerows(altered_rows)
        for bad in (
            original + original.splitlines(keepends=True)[0],
            original + b"adbc_driver_manager/__pycache__/old.pyc,,\n",
            original.replace(b"sha256=", b"sha512=", 1),
            invalid_size.getvalue().encode(),
            b"unknown,,\n",
        ):
            self.assertNotEqual(bad, original)
            path.write_bytes(bad)
            self.rejected()
        path.write_bytes(original)
        f.observe()

    def test_client_member_requires_exact_named_regular_path(self):
        f = self.fixture
        path = f.bundle.artifacts / f.bundle.package["c_archive"].path
        original_bytes = path.read_bytes()
        with tarfile.open(fileobj=io.BytesIO(original_bytes), mode="r:gz") as archive:
            entries = [
                (
                    copy.copy(member),
                    archive.extractfile(member).read() if member.isfile() else None,
                )
                for member in archive
            ]
        client_name = "test-client/adbc_redshift_install_smoke"
        self.assertEqual(
            [
                member.name
                for member, _ in entries
                if member.isfile() and member.name.startswith("test-client")
            ],
            [client_name],
        )
        f.observe()
        client, data = next(
            (member, data) for member, data in entries if member.name == client_name
        )
        unrelated = [
            (member, data)
            for member, data in entries
            if not member.name.startswith("test-client")
        ]
        for wrong in (
            "test-client",
            "test-client/wrong-name",
            "other-client/adbc_redshift_install_smoke",
        ):
            changed = copy.copy(client)
            changed.name = wrong
            trial = unrelated + [(changed, data)]
            raw = io.BytesIO()
            with tarfile.open(fileobj=raw, mode="w:gz") as archive:
                for member, body in trial:
                    archive.addfile(
                        member, io.BytesIO(body) if body is not None else None
                    )
            f.replace_artifact("c_archive", raw.getvalue())
            with self.subTest(wrong=wrong):
                self.rejected()
        f.replace_artifact("c_archive", original_bytes)
        f.observe()

    def test_tar_and_zip_never_extract_or_accept_aliases_specials_duplicates(self):
        f = self.fixture
        tar_path = f.bundle.artifacts / f.bundle.package["c_archive"].path
        original_tar = tar_path.read_bytes()
        with tarfile.open(fileobj=io.BytesIO(original_tar), mode="r:gz") as archive:
            original = [
                (
                    copy.copy(member),
                    archive.extractfile(member).read() if member.isfile() else None,
                )
                for member in archive
            ]
        duplicate = next(
            (member, data)
            for member, data in original
            if member.name.endswith("/adbc.h")
        )
        extra = tarfile.TarInfo("test-prefix/../escape")
        extra.size = 1
        hardlink = tarfile.TarInfo("test-prefix/hardlink")
        hardlink.type, hardlink.linkname = tarfile.LNKTYPE, "test-client"
        cases = [
            original + [duplicate],
            original + [(extra, b"x")],
            original + [(hardlink, None)],
            [
                (member, data)
                for member, data in original
                if member.name != "test-client/adbc_redshift_install_smoke"
            ],
        ]
        for entries in cases:
            raw = io.BytesIO()
            with tarfile.open(fileobj=raw, mode="w:gz") as archive:
                for member, data in entries:
                    archive.addfile(
                        member, io.BytesIO(data) if data is not None else None
                    )
            f.replace_artifact("c_archive", raw.getvalue())
            self.rejected()
        f.replace_artifact("c_archive", original_tar)
        role = "manager_wheel"
        zip_path = f.bundle.artifacts / f.bundle.package[role].path
        original_zip = zip_path.read_bytes()
        with zipfile.ZipFile(io.BytesIO(original_zip)) as archive:
            contents = [
                (copy.copy(info), archive.read(info)) for info in archive.infolist()
            ]
        alias = zipfile.ZipInfo("adbc_driver_manager/../escape")
        link = zipfile.ZipInfo("adbc_driver_manager/link.py")
        link.external_attr = 0o120777 << 16
        for entry in [(alias, b"x"), (link, b"outside"), contents[0]]:
            raw = io.BytesIO()
            with (
                self.assertWarns(UserWarning)
                if entry is contents[0]
                else __import__("contextlib").nullcontext()
            ):
                with zipfile.ZipFile(raw, "w") as archive:
                    for info, data in contents + [entry]:
                        archive.writestr(info, data)
            f.replace_artifact(role, raw.getvalue())
            self.rejected()
        f.replace_artifact(role, original_zip)
        # direct_url intentionally still names original bytes after negatives.
        f.observe()

    def test_macho_literal_boundaries_and_no_suffix_guess(self):
        good = macho()
        self.assertEqual(probe.parse_macho(good)["uuid"], "1" * 32)
        for bad in (
            b"\x7fELF" + good[4:],
            macho(cpu=7),
            good[:31],
            good[:-1],
            good[:20] + struct.pack("<I", 1048577) + good[24:],
            good[:16] + struct.pack("<I", 4097) + good[20:],
            good[:32] + struct.pack("<II", 0x1B, 0) + good[40:],
            macho("0" * 32),
        ):
            with self.subTest(length=len(bad)):
                with self.assertRaises(probe.Invalid):
                    probe.parse_macho(bad)
        name = b"@rpath/example.dylib\0"
        length = (24 + len(name) + 7) // 8 * 8
        command = struct.pack("<6I", 0xC, length, 24, 0, 1, 1) + name
        command += b"\0" * (length - len(command))
        facts = probe.parse_macho(macho(extra=command))
        self.assertEqual(facts["dylibs"][0]["name"], "@rpath/example.dylib")
        self.assertEqual(facts["dylibs"][0]["resolution"], "unresolved")

    def test_os_multiple_deployment_commands_preserve_all_facts(self):
        commands = (
            build_version(1, 983808, 983808, ((3, 111),)),
            build_version(6, 1180416, 1180416, ((3, 222),)),
        )
        facts = probe.parse_macho(macho(builds=commands))
        expected = [
            {
                "platform": 1,
                "minos_packed": 983808,
                "sdk_packed": 983808,
                "tools": [{"tool": 3, "version": 111}],
            },
            {
                "platform": 6,
                "minos_packed": 1180416,
                "sdk_packed": 1180416,
                "tools": [{"tool": 3, "version": 222}],
            },
        ]
        self.assertEqual(facts["deployment"], expected)
        f = self.fixture
        f.transform = lambda value: (
            value["images"][-1].update(deployment=expected) or value
        )
        result = f.observe()
        metadata = json.loads((f.metadata / result.metadata.path).read_bytes())
        self.assertEqual(metadata["images"][-1]["deployment"], expected)
        self.assertNotIn("sha256", metadata["images"][-1])
        for bad in (
            expected + [expected[0]],
            [dict(expected[0], tools=[{"tool": True, "version": 1}])],
            [dict(expected[0], extra=1)],
            {"platform": 1},
            None,
        ):
            with self.subTest(bad=bad):
                f.transform = lambda value, bad=bad: (
                    value["images"][-1].update(deployment=bad) or value
                )
                self.rejected()
        f.transform = lambda value: value
        f.main.write_bytes(macho("d" * 32, builds=commands))
        f.selected = replace(
            f.selected,
            runtime=tuple(identity(Path(e.path)) for e in f.selected.runtime),
        )
        self.rejected()

    def test_deployment_command_malformed_and_duplicate_platform_rejected(self):
        good = build_version(1, 983808, 983808, ((3, 111),))
        for builds in (
            (good, good),
            (good, build_version(1, 1180416, 1180416)),
            (good[:-8],),
            (good[:20] + struct.pack("<I", 2) + good[24:],),
            (build_version(1, 983808, 983808, ((3, 1), (3, 2))),),
        ):
            with self.subTest(builds=builds):
                with self.assertRaises(probe.Invalid):
                    probe.parse_macho(macho(builds=builds))

    def test_runtime_unknown_uuid_deployment_origins_and_membership(self):
        f = self.fixture
        mutations = [
            lambda v: v.update(os_build="other"),
            lambda v: v.update(abi="wrong"),
            lambda v: v.update(connection_created=True),
            lambda v: v.update(broader_closure="complete"),
            lambda v: v["origins"]["manager"].update(path="/source/shadow.py"),
            lambda v: v["images"][0].update(uuid="f" * 32),
            lambda v: v["images"][0].update(path="/unknown/dependency"),
            lambda v: v["images"][0].update(cpu=True),
            lambda v: v["images"].pop(1),
        ]
        for mutate in mutations:

            def transform(value, mutate=mutate):
                mutate(value)
                return value

            f.transform = transform
            self.rejected()
        f.transform = lambda value: value
        f.main.write_bytes(macho("d" * 32, platform=6))
        f.selected = replace(
            f.selected,
            runtime=tuple(identity(Path(e.path)) for e in f.selected.runtime),
        )
        self.rejected()

    def test_preimport_rejects_preloaded_cache_and_wrong_origin_without_import(self):
        f = self.fixture
        with patch.dict(sys.modules, {"adbc_driver_manager": SimpleNamespace()}):
            with self.assertRaises(probe.Invalid):
                probe.preimport(f.site)
        with patch(
            "importlib.machinery.PathFinder.find_spec",
            return_value=SimpleNamespace(
                origin="/wrong/__init__.py", submodule_search_locations=["/wrong"]
            ),
        ):
            with self.assertRaises(probe.Invalid):
                probe.preimport(f.site)

    def test_child_rejects_readonly_or_stale_metadata_fd_before_any_import(self):
        self.assertTrue(sys.flags.isolated and sys.flags.dont_write_bytecode)
        f = self.fixture
        path = f.metadata / "readonly.json"
        path.write_bytes(b"")
        path.chmod(0o600)
        fd = os.open(path, os.O_RDONLY)
        try:
            with (
                patch.object(sys, "argv", ["probe", str(fd), "{}"]),
                patch.object(
                    probe, "observe", side_effect=AssertionError("must not import")
                ) as observe,
                patch.object(sys, "stderr", io.StringIO()),
            ):
                self.assertEqual(probe.main(), 1)
                observe.assert_not_called()
        finally:
            os.close(fd)
        path.write_bytes(b"stale")
        fd = os.open(path, os.O_RDWR)
        try:
            with (
                patch.object(sys, "argv", ["probe", str(fd), "{}"]),
                patch.object(
                    probe, "observe", side_effect=AssertionError("must not import")
                ) as observe,
                patch.object(sys, "stderr", io.StringIO()),
            ):
                self.assertEqual(probe.main(), 1)
                observe.assert_not_called()
        finally:
            os.close(fd)

    def test_recheck_after_child_detects_input_swap_and_metadata_status(self):
        f = self.fixture
        launch = f.fake_launch

        def mutate_after(selected, request, fd):
            status = launch(selected, request, fd)
            (f.site / "adbc_driver_redshift/__init__.py").write_bytes(
                b"CHANGED AFTER LOAD"
            )
            return status

        with patch.object(install, "_launch", side_effect=mutate_after):
            with self.assertRaises(install.Rejected):
                install.observe_install(
                    f.bundle.data, selected=f.selected, roots=f.roots
                )

    def test_runtime_file_change_symlink_and_bad_deployment_reject(self):
        f = self.fixture
        original = f.main.read_bytes()
        for data in (b"\x7fELF" + original[4:], macho("d" * 32, minimum=0x100000)):
            f.main.write_bytes(data)
            # Root-selected bytes updated solely to reach the supported-profile guard.
            f.selected = replace(
                f.selected,
                runtime=tuple(identity(Path(e.path)) for e in f.selected.runtime),
            )
            self.rejected()
        f.main.write_bytes(original)
        f.selected = replace(
            f.selected,
            runtime=tuple(identity(Path(e.path)) for e in f.selected.runtime),
        )
        f.main.write_bytes(original + b"changed")
        self.rejected()
        f.main.write_bytes(original)
        target = f.root / "replacement"
        target.write_bytes(original)
        f.main.unlink()
        f.main.symlink_to(target)
        self.rejected()

    def test_metadata_channel_invalid_incomplete_and_changed_identity(self):
        f = self.fixture
        valid = f.fake_launch
        for mode in (
            "empty",
            "truncated",
            "duplicate",
            "oversize",
            "wrong-sha",
            "replaced",
        ):

            def broken(selected, request, fd, mode=mode):
                if mode in ("wrong-sha", "replaced"):
                    status = valid(selected, request, fd)
                    if mode == "wrong-sha":
                        status["sha256"] = "f" * 64
                    else:
                        path = next(
                            path
                            for path in f.metadata.iterdir()
                            if path.stat().st_ino == os.fstat(fd).st_ino
                        )
                        path.unlink()
                        path.write_bytes(b"replacement")
                    return status
                data = {
                    "empty": b"",
                    "truncated": b"{",
                    "duplicate": b'{"version":1,"version":1}',
                    "oversize": b"x" * 1048577,
                }[mode]
                os.write(fd, data)
                return {
                    "version": 1,
                    "status": "load-only-observed",
                    "bytes": len(data),
                    "sha256": base.sha(data),
                }

            with (
                self.subTest(mode=mode),
                patch.object(install, "_launch", side_effect=broken),
            ):
                with self.assertRaises(install.Rejected):
                    install.observe_install(
                        f.bundle.data, selected=f.selected, roots=f.roots
                    )

    def transport(self, body, seconds=1):
        f = self.fixture
        script = f.root / "SYNTHETIC-transport-child.py"
        script.write_text(
            "import os,sys,json,hashlib,time,subprocess,signal\nfd=int(sys.argv[1])\n"
            + body
        )
        print(
            "SYNTHETIC_CHILD_BEFORE_EXECUTION=" + base.sha(script.read_bytes()),
            flush=True,
        )
        real_popen = subprocess.Popen
        metadata = f.metadata / "transport.json"
        fd = os.open(metadata, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)

        def only_synthetic(argv, **kwargs):
            self.assertEqual(
                argv[:4],
                [f.selected.interpreter.path, "-I", "-B", f.selected.child.path],
            )
            self.assertEqual(kwargs["env"], {"LANG": "C.UTF-8"})
            self.assertEqual(kwargs["pass_fds"], (fd,))
            return real_popen([argv[0], "-I", "-B", str(script), *argv[4:]], **kwargs)

        try:
            with patch.object(install.subprocess, "Popen", side_effect=only_synthetic):
                return install._launch(
                    replace(
                        f.selected, bounds=replace(f.selected.bounds, seconds=seconds)
                    ),
                    {},
                    fd,
                )
        finally:
            os.close(fd)
            metadata.unlink()

    def test_fixed_transport_accepts_only_compact_terminal_protocol(self):
        status = self.transport(
            "data=b'{}'\nos.write(fd,data)\nos.fsync(fd)\n"
            "value={'version':1,'status':'load-only-observed','bytes':2,\n"
            "'sha256':hashlib.sha256(data).hexdigest()}\n"
            "print(json.dumps(value))\n"
        )
        self.assertEqual(status["bytes"], 2)
        boolean_status = json.dumps(
            {
                "version": True,
                "status": "load-only-observed",
                "bytes": 1,
                "sha256": "f" * 64,
            }
        )
        for body in (
            "raise SystemExit(3)\n",
            "print('{}')\n",
            "print('x'*2049)\n",
            "pass\n",
            "print(" + repr(boolean_status) + ")\n",
        ):
            with self.subTest(body=body[:30]), self.assertRaises(ValueError):
                self.transport(body)

    def test_transport_timeout_terminates_and_reaps_descendant_group(self):
        f = self.fixture
        marker = f.root / "descendant-signalled"
        descendant = (
            "import signal,time,pathlib; "
            "signal.signal(signal.SIGTERM,lambda *a:(pathlib.Path("
            + repr(str(marker))
            + ").write_text('TERM'),exit(0))); "
            "time.sleep(10)"
        )
        body = (
            "signal.signal(signal.SIGTERM,lambda *a:(time.sleep(0.1),sys.exit(0)))\n"
            "child=subprocess.Popen([sys.executable,'-I','-B','-c',"
            + repr(descendant)
            + "])\ntime.sleep(10)\n"
        )
        started = __import__("time").monotonic()
        with self.assertRaises(install.Rejected):
            self.transport(body)
        self.assertLess(__import__("time").monotonic() - started, 5)
        self.assertEqual(marker.read_text(), "TERM")

    def test_limits_tighten_and_metadata_file_is_exclusive_private(self):
        f = self.fixture
        result = f.observe()
        path = f.metadata / result.metadata.path
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        second = f.observe()
        self.assertNotEqual(result.metadata.path, second.metadata.path)
        for name in (
            "compressed",
            "expanded",
            "member",
            "metadata",
            "c_members",
            "wheel_members",
            "images",
        ):
            original = f.selected
            f.selected = replace(original, bounds=replace(original.bounds, **{name: 1}))
            self.rejected()
            f.selected = original
        f.selected = replace(f.selected, bounds=install.Bounds(seconds=True))
        self.rejected()


if __name__ == "__main__":
    unittest.main()
