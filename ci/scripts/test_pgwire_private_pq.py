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

"""Offline negative gates; tiny fake libraries only, never PostgreSQL compilation."""

import argparse
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import pgwire_private_pq as package


def fixture(root):
    assets = root / "assets"
    shutil.copytree(package.ASSETS, assets)
    archive = root / "source.tar.bz2"
    archive.write_text("fake input; must never extract")
    ssl = root / "ssl"
    for name in (
        "include/openssl/ssl.h",
        "lib/pkgconfig/libssl.pc",
        "lib/pkgconfig/libcrypto.pc",
    ):
        path = ssl / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("fake external input")
    pins = json.loads((assets / "pins.json").read_text())
    pins["source_sha256"] = package.digest(archive)
    args = argparse.Namespace(
        source_archive=archive,
        work_dir=root / "work",
        prefix=root / "work/install",
        openssl_prefix=ssl,
        openssl_libdir=None,
        cc="cc",
        build_type="debug",
    )
    return assets, pins, args


class PreflightTest(unittest.TestCase):
    def test_optimized_mismatches_have_no_subprocess_or_output(self):
        for mode in ("-O", "environment"):
            for gate in ("source", "patch", "namespace", "license", "openssl"):
                with (
                    self.subTest(mode=mode, gate=gate),
                    tempfile.TemporaryDirectory() as tmp,
                ):
                    root = Path(tmp)
                    assets, pins, args = fixture(root)
                    paths = {
                        "source": args.source_archive,
                        "patch": assets / "libpq-result-formats.patch",
                        "namespace": assets / "namespace-macro-restore.patch",
                        "license": assets / "PostgreSQL-COPYRIGHT",
                        "openssl": args.openssl_prefix / "include/openssl/ssl.h",
                    }
                    if gate == "openssl":
                        paths[gate].unlink()
                    else:
                        paths[gate].write_text("corrupt input")
                    marker = root / "subprocess-called"
                    runner = root / "runner.py"
                    arg_values = {
                        k: str(v) if isinstance(v, Path) else v
                        for k, v in vars(args).items()
                    }
                    runner.write_text(
                        "import argparse, sys\nfrom pathlib import Path\n"
                        f"sys.path.insert(0, {str(Path(package.__file__).parent)!r})\n"
                        "import pgwire_private_pq as p\n"
                        "if sys.flags.optimize < 1:\n"
                        "    raise RuntimeError('not optimized')\n"
                        f"p.ASSETS = Path({str(assets)!r})\n"
                        f"values = {arg_values!r}\n"
                        "for k in ('source_archive','work_dir','prefix',"
                        "'openssl_prefix'):\n"
                        "    values[k] = Path(values[k])\n"
                        "def forbidden(*a, **kw):\n"
                        f"    Path({str(marker)!r}).write_text('called')\n"
                        "    raise SystemExit(99)\n"
                        "p.subprocess.run = forbidden\n"
                        f"p.build(argparse.Namespace(**values), {pins!r})\n"
                    )
                    env = dict(os.environ)
                    env.pop("PYTHONOPTIMIZE", None)
                    flags = ["-O"] if mode == "-O" else []
                    if not flags:
                        env["PYTHONOPTIMIZE"] = "1"
                    result = subprocess.run(
                        [sys.executable, *flags, str(runner)],
                        env=env,
                        capture_output=True,
                        text=True,
                        timeout=10,
                    )
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn(
                        "OpenSSL headers missing"
                        if gate == "openssl"
                        else "Pin validation failed",
                        result.stderr,
                    )
                    self.assertFalse(marker.exists())
                    self.assertFalse(args.work_dir.exists())

    def test_existing_directory_and_prefix_escape_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            assets, pins, args = fixture(root)
            with patch.object(package, "ASSETS", assets):
                args.prefix = args.work_dir / "../outside"
                with self.assertRaisesRegex(RuntimeError, "inside the new owned"):
                    package.preflight(args, pins)
                args.prefix = args.work_dir / "install"
                args.work_dir.mkdir()
                with self.assertRaisesRegex(RuntimeError, "must be new"):
                    package.preflight(args, pins)

    def test_header_generation_failure_stops_before_archives(self):
        with tempfile.TemporaryDirectory() as tmp:
            assets, pins, args = fixture(Path(tmp))
            commands = []

            def fake(command, **kwargs):
                commands.append(command)
                if command[-1] == "generated-headers":
                    kwargs["stdout"].write("compiler diagnostic")
                    return subprocess.CompletedProcess(command, 2)
                return subprocess.CompletedProcess(command, 0)

            with (
                patch.object(sys, "stderr", new_callable=io.StringIO) as diagnostic,
                patch.object(package, "ASSETS", assets),
                patch.object(package.subprocess, "run", side_effect=fake),
            ):
                with self.assertRaises(subprocess.CalledProcessError):
                    package.build(args, pins)
            self.assertIn("compiler diagnostic", diagnostic.getvalue())
            self.assertEqual(commands[-1][:3], ["make", "-j1", "-C"])
            self.assertEqual(commands[-1][-1], "generated-headers")
            self.assertFalse(any(c[-1].endswith(".a") for c in commands))
            self.assertFalse(args.prefix.exists())


def fake_installed_package(root, variant):
    """Real tiny archives for configure/link-negative checks; no libpq source used."""
    source = root / "fake-source"
    mapping = root / "map"
    mapping.mkdir()
    names = [
        "PQlibVersion",
        "ADBCPQprivatePackageAbi",
        "ADBCPQexecPreparedFormats",
        "ADBCPQsendQueryPreparedFormats",
    ]
    prefix = "adbc_rs_pq_17_11_v1_"
    (mapping / "public-namespace.h").write_text(
        "".join(f"#define {n} {prefix}{n}\n" for n in names)
    )
    (mapping / "namespace.h").write_text("/* fake full map */\n")
    (mapping / "symbols.json").write_text("{}\n")
    pq = source / "src/interfaces/libpq"
    pq.mkdir(parents=True)
    inc = source / "src/include"
    inc.mkdir(parents=True)
    (pq / "libpq-fe.h").write_text(
        '#include "postgres_ext.h"\n#include <stddef.h>\n'
        "typedef struct pg_conn PGconn;\ntypedef struct pg_result PGresult;\n"
        "int PQlibVersion(void);\n"
    )
    (inc / "postgres_ext.h").write_text('#include "pg_config_ext.h"\n')
    (inc / "pg_config_ext.h").write_text("/* empty */\n")
    cfile = root / "fake.c"
    body = "int " + prefix + "PQlibVersion(void) { return 170011; }\n"
    body += "int " + prefix + "ADBCPQprivatePackageAbi(void) { return 1; }\n"
    if variant != "missing-helper":
        body += "int " + prefix + "ADBCPQsendQueryPreparedFormats(void) { return 0; }\n"
    body += "void *" + prefix + "ADBCPQexecPreparedFormats(void) { return 0; }\n"
    cfile.write_text(body)
    obj = root / "fake.o"
    subprocess.run(
        ["cc", "-c", str(cfile), "-o", str(obj)], check=True, capture_output=True
    )
    for archive in package.ARCHIVES:
        target = source / archive
        target.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            ["ar", "rcs", str(target), str(obj)], check=True, capture_output=True
        )
    pins = json.loads((package.ASSETS / "pins.json").read_text())
    installed = root / "installed"
    package.install_package(installed, source, mapping, pins)
    if variant == "wrong-abi":
        header = installed / "include" / package.PACKAGE / "adbc_rs_pq.h"
        header.write_text(
            header.read_text().replace("PRIVATE_PQ_ABI 1", "PRIVATE_PQ_ABI 2")
        )
    if variant == "missing-archive":
        (installed / "lib" / ("lib" + package.LIBRARY + "_port.a")).unlink()
    for name in ("libssl", "libcrypto"):
        (installed / "lib/pkgconfig" / (name + ".pc")).write_text(
            f"Name: {name}\nDescription: fake external dependency\nVersion: 1\n"
        )
    return installed


@unittest.skipUnless(shutil.which("cc") and shutil.which("ar"), "needs cc and ar")
class RecipeInstallTest(unittest.TestCase):
    def test_vcpkg_manifest_survives_install_and_rebuild(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = (package.ASSETS / "vcpkg.json").read_bytes()
            assets = package.ASSETS
            for generation in ("from-checkout", "from-embedded"):
                work = root / generation
                work.mkdir()
                with patch.object(package, "ASSETS", assets):
                    built = fake_installed_package(work, "good")
                # vcpkg skips these three basenames at any depth during install.
                copied = work / "vcpkg-install"
                shutil.copytree(
                    built,
                    copied,
                    ignore=shutil.ignore_patterns(
                        "vcpkg.json", "CONTROL", "BUILD_INFO"
                    ),
                )
                recipe_path = Path("share") / package.PACKAGE / "recipe"
                assets = copied / recipe_path / "ci/vcpkg/ports" / package.PACKAGE
                self.assertEqual((assets / "vcpkg.json.in").read_bytes(), manifest)
                self.assertFalse(
                    (
                        built
                        / recipe_path
                        / "ci/vcpkg/ports"
                        / package.PACKAGE
                        / "vcpkg.json"
                    ).exists()
                )
                for file in (built / recipe_path).rglob("*"):
                    if file.is_file():
                        self.assertEqual(
                            file.read_bytes(),
                            (copied / file.relative_to(built)).read_bytes(),
                        )


@unittest.skipUnless(
    all(shutil.which(t) for t in ("cc", "ar", "cmake", "meson", "pkg-config")),
    "configure seam tests require local CMake, Meson and C tools",
)
class ConfigureTest(unittest.TestCase):
    def test_real_link_gates_with_tiny_offline_archives(self):
        for variant in ("good", "missing-helper", "wrong-abi", "missing-archive"):
            with self.subTest(variant=variant), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                installed = fake_installed_package(root, variant)
                project = root / "project"
                project.mkdir()
                # No real OpenSSL dependency is needed by this tiny fake library.
                (project / "FindOpenSSL.cmake").write_text(
                    "set(OpenSSL_FOUND TRUE)\n"
                    "add_library(OpenSSL::SSL INTERFACE IMPORTED)\n"
                    "add_library(OpenSSL::Crypto INTERFACE IMPORTED)\n"
                )
                (project / "CMakeLists.txt").write_text(
                    "cmake_minimum_required(VERSION 3.18)\nproject(probe C)\n"
                    "list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR})\n"
                    "set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n"
                    "find_package(AdbcRedshiftPrivatePQ CONFIG REQUIRED "
                    f'PATHS "{installed}" NO_DEFAULT_PATH)\n'
                )
                cmake = subprocess.run(
                    ["cmake", "-S", str(project), "-B", str(root / "cmake")],
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                self.assertEqual(
                    cmake.returncode == 0,
                    variant == "good",
                    cmake.stdout + cmake.stderr,
                )
                shutil.copyfile(package.ASSETS / "link_check.c", project / "check.c")
                (project / "meson.build").write_text(
                    "project('probe', 'c')\n"
                    "d = dependency('adbc-redshift-private-pq', static: true, "
                    "method: 'pkg-config', required: true)\n"
                    "if not meson.get_compiler('c').links("
                    "files('check.c'), dependencies: d)\n"
                    "  error('private ABI/helper link rejected')\nendif\n"
                )
                env = dict(
                    os.environ,
                    PKG_CONFIG_PATH="",
                    PKG_CONFIG_LIBDIR=str(installed / "lib/pkgconfig"),
                )
                meson = subprocess.run(
                    ["meson", "setup", str(root / "meson"), str(project)],
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                self.assertEqual(
                    meson.returncode == 0,
                    variant == "good",
                    meson.stdout + meson.stderr,
                )


if __name__ == "__main__":
    unittest.main()
