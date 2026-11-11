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

"""Windows adapter tests with a real Git checkout and mocked build tools."""

import argparse
import io
import json
import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest.mock import patch

import pgwire_private_pq as package
import pgwire_private_pq_windows as windows
from test_pgwire_private_pq import fixture as unix_fixture


def fixture(root):
    assets, pins, args = unix_fixture(root)
    values = vars(args)
    values.pop("build_type")
    for name in (
        "cc",
        "lib",
        "git",
        "ninja",
        "nm",
        "objcopy",
        "readobj",
        "pkgconf",
        "bison",
        "flex",
        "perl",
    ):
        tool = root / "tools" / (name + ".exe")
        tool.parent.mkdir(exist_ok=True)
        tool.write_text("fake tool; never execute")
        values[name] = tool
    values.update(
        architecture="x64", crt="md", meson_command_element=[str(values["cc"])]
    )
    for name in ("libssl.lib", "libcrypto.lib"):
        (args.openssl_prefix / "lib" / name).write_text("fake OpenSSL")
    return assets, pins, argparse.Namespace(**values)


def fake_source(archive, directory):
    source = directory / "postgresql-17.11"
    pq = source / "src/interfaces/libpq"
    inc = source / "src/include"
    pq.mkdir(parents=True)
    inc.mkdir(parents=True)
    (pq / "libpq-fe.h").write_text('#include "postgres_ext.h"\n')
    (inc / "postgres_ext.h").write_text('#include "pg_config_ext.h"\n')
    (pq / "exports.txt").write_text("PQlibVersion 1\n")
    return source


class WindowsRecipeTest(unittest.TestCase):
    def test_autocrlf_checkout_preserves_pinned_recipe_bytes(self):
        port = package.ASSETS.relative_to(package.REPOSITORY).as_posix()
        paths = {
            "ci/scripts/pgwire_private_pq.py": "build_script_sha256",
            "ci/scripts/pgwire_private_pq_windows.py": "windows_build_script_sha256",
            "ci/scripts/pgwire_private_pq_symbols.py": "symbol_script_sha256",
            f"{port}/libpq-result-formats.patch": "patch_sha256",
            f"{port}/namespace-macro-restore.patch": "namespace_patch_sha256",
            f"{port}/PostgreSQL-COPYRIGHT": "license_sha256",
            "LICENSE.txt": "adbc_license_sha256",
            "NOTICE.txt": "adbc_notice_sha256",
        }
        pins = json.loads((package.ASSETS / "pins.json").read_text())
        with tempfile.TemporaryDirectory() as tmp:
            # Container checkouts may be source archives without a Git index.
            root = Path(tmp).resolve()
            source = root / "source"
            destination = root / "checkout"
            subprocess.run(["git", "init", "-q", str(source)], check=True)
            for path in (".gitattributes", *paths):
                target = source / path
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(package.REPOSITORY / path, target)
            subprocess.run(
                ["git", "add", "--", ".gitattributes", *paths], cwd=source, check=True
            )
            subprocess.run(
                [
                    "git",
                    "-c",
                    "core.autocrlf=true",
                    "checkout-index",
                    "--prefix=" + destination.as_posix() + "/",
                    "--",
                    *paths,
                ],
                cwd=source,
                check=True,
            )
            for path, key in paths.items():
                with self.subTest(path=path):
                    self.assertEqual(package.digest(destination / path), pins[key])

    def test_native_target_and_dynamic_runtime_are_required_before_subprocess(self):
        for failure in ("architecture", "crt", "source", "openssl", "tool"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                assets, pins, args = fixture(Path(tmp))
                if failure == "architecture":
                    args.architecture = "arm64"
                elif failure == "crt":
                    args.crt = "mt"
                elif failure == "source":
                    args.source_archive.write_text("tampered")
                elif failure == "openssl":
                    (args.openssl_prefix / "lib/libssl.lib").unlink()
                else:
                    args.readobj.unlink()
                with (
                    patch.object(package, "ASSETS", assets),
                    patch.object(windows.platform, "system", return_value="Windows"),
                    patch.object(windows.platform, "machine", return_value="AMD64"),
                    patch.dict(os.environ, {"PROCESSOR_ARCHITEW6432": "AMD64"}),
                    patch.object(windows.subprocess, "run") as process,
                ):
                    with self.assertRaises(RuntimeError):
                        windows.build(args, pins)
                    process.assert_not_called()
                self.assertFalse(args.work_dir.exists())

    def test_exact_frontend_target_selection_rejects_shlib_ambiguity_and_escape(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            rows = [
                {
                    "name": name,
                    "type": "static library",
                    "filename": [str(root / (name + ".lib"))],
                }
                for name in windows.TARGETS
            ]
            rows.append(
                {
                    "name": "libpq",
                    "type": "shared library",
                    "filename": [str(root / "libpq.dll")],
                }
            )
            self.assertEqual(
                [p.name for p in windows.static_targets(rows, root)],
                [name + ".lib" for name in windows.TARGETS],
            )
            with self.assertRaisesRegex(RuntimeError, "ambiguous"):
                windows.static_targets(rows + [rows[0]], root)
            with self.assertRaisesRegex(RuntimeError, "ambiguous"):
                windows.static_targets(rows[1:], root)
            rows[0]["filename"] = [str(root / "../outside.lib")]
            with self.assertRaisesRegex(RuntimeError, "escapes"):
                windows.static_targets(rows, root)

    def test_coff_machine_and_runtime_are_verified(self):
        for arch, machine in windows.ARCHITECTURES.items():
            valid = (
                "Machine: "
                + machine
                + " (0x0)\nDirective(s): /DEFAULTLIB:MSVCRT /DEFAULTLIB:OLDNAMES"
            )
            windows.verify_coff(valid, arch)
            for bad in (
                "",
                valid.replace(machine, "IMAGE_FILE_MACHINE_I386"),
                valid.replace("MSVCRT", "LIBCMT"),
                valid + " /DEFAULTLIB:MSVCRTD",
            ):
                with self.subTest(arch=arch, bad=bad), self.assertRaises(RuntimeError):
                    windows.verify_coff(bad, arch)

    def test_meson_flags_disable_pch_lto_and_preserve_md(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, _, args = fixture(Path(tmp))
            args.openssl_libdir = args.openssl_prefix / "lib"
            flags = windows.meson_options(args, Path(tmp) / "namespace.h")
            self.assertIn("-Db_vscrt=md", flags)
            self.assertIn("-Db_pch=false", flags)
            self.assertIn("-Db_lto=false", flags)
            self.assertIn("--wrap-mode=nodownload", flags)
            cflags = json.loads(
                next(f.split("=", 1)[1] for f in flags if f.startswith("-Dc_args="))
            )
            self.assertEqual(cflags[0], "/GF-")
            self.assertTrue(cflags[1].startswith("/FI"))

    def test_source_member_escape_rejected_before_extract(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = root / "source.tar.bz2"
            with tarfile.open(archive, "w:bz2") as tar:
                entry = tarfile.TarInfo("../outside")
                entry.size = 1
                tar.addfile(entry, io.BytesIO(b"x"))
            with self.assertRaisesRegex(RuntimeError, "Unsafe"):
                windows.extract_source(archive, root / "out")
            self.assertFalse((root / "out").exists())

    def test_mock_two_pass_flow_and_failed_audit_never_publish_success(self):
        for failure in (None, "audit", "probe"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                assets, pins, args = fixture(root)
                calls = []

                def process(command, **kwargs):
                    calls.append(command)
                    if "introspect" in command:
                        build = Path(command[-1])
                        build.mkdir(parents=True)
                        header = build / "src/include/pg_config_ext.h"
                        header.parent.mkdir(parents=True)
                        header.write_text("/* fake config */\n")
                        rows = []
                        for name in windows.TARGETS:
                            archive = build / (name + ".lib")
                            archive.write_text("fake COFF " + name)
                            rows.append(
                                {
                                    "name": name,
                                    "type": "static library",
                                    "filename": [str(archive)],
                                }
                            )
                        kwargs["stdout"].write(json.dumps(rows))
                    if "--file-headers" in command:
                        kwargs["stdout"].write(
                            "Machine: IMAGE_FILE_MACHINE_AMD64 (0x8664)\n"
                            "Directive(s): /DEFAULTLIB:MSVCRT"
                        )
                    outputs = ("/Fo", "/Fe", "/OUT:")  # codespell:ignore fo
                    for arg in command:
                        if arg.startswith(outputs):
                            name = arg[5:] if arg.startswith("/OUT:") else arg[3:]
                            Path(name).write_text("fake compiler output")
                    if "generate" in command:
                        mapping = Path(command[command.index("--directory") + 1])
                        mapping.mkdir()
                        for name in ("namespace.h", "public-namespace.h"):
                            (mapping / name).write_text("/* fake map */\n")
                        (mapping / "symbols.json").write_text("{}\n")
                        (mapping / "coff-redefine.syms").write_text(
                            "__real@3ff0000000000000 private_constant\n"
                        )
                    if any(arg.startswith("--redefine-syms=") for arg in command):
                        Path(command[-1]).write_bytes(Path(command[-2]).read_bytes())
                    if failure == "audit" and "audit" in command:
                        kwargs["stdout"].write("earlier output\n" + "detail\n" * 60)
                        kwargs["stdout"].write("COFF symbol audit failed\n")
                        raise subprocess.CalledProcessError(2, command)
                    if failure == "probe" and command[0].endswith(
                        "package-link-check.exe"
                    ):
                        raise subprocess.CalledProcessError(3, command)
                    return subprocess.CompletedProcess(command, 0)

                with (
                    patch.object(package, "ASSETS", assets),
                    patch.object(windows.platform, "system", return_value="Windows"),
                    patch.object(windows.platform, "machine", return_value="AMD64"),
                    patch.dict(os.environ, {"PROCESSOR_ARCHITEW6432": "AMD64"}),
                    patch.object(windows, "extract_source", side_effect=fake_source),
                    patch.object(windows.subprocess, "run", side_effect=process),
                ):
                    if failure:
                        with (
                            self.assertRaises(subprocess.CalledProcessError),
                            redirect_stderr(io.StringIO()) as diagnostic,
                        ):
                            windows.build(args, pins)
                        if failure == "audit":
                            self.assertIn(
                                "COFF symbol audit failed", diagnostic.getvalue()
                            )
                            self.assertNotIn("earlier output", diagnostic.getvalue())
                            self.assertEqual(
                                diagnostic.getvalue().count("detail\n"), 59
                            )
                    else:
                        windows.build(args, pins)
                receipt = args.prefix / "share/adbc-redshift-private-pq/provenance.json"
                if failure:
                    self.assertFalse(receipt.exists())
                else:
                    recorded = json.loads(receipt.read_text())
                    self.assertEqual(recorded["crt"], "md")
                    self.assertEqual(recorded["system"], "Windows")
                    self.assertEqual(len(list((args.prefix / "lib").glob("*.lib"))), 3)
                    audit = next(i for i, c in enumerate(calls) if "audit" in c)
                    rewrite = max(
                        i
                        for i, c in enumerate(calls)
                        if any(a.startswith("--redefine-syms=") for a in c)
                    )
                    self.assertLess(rewrite, audit)
                    self.assertFalse(any("postgres.exe" in str(c) for c in calls))


if __name__ == "__main__":
    unittest.main()
