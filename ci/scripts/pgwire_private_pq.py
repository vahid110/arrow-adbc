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

"""Build the pinned, namespaced libpq package into a new explicitly owned prefix.

Offline input only; native Linux/macOS; two serial dependency builds. The coordinator
runs this script. No system installation or driver build is performed.
"""

import argparse
import hashlib
import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

PACKAGE = "adbc-redshift-private-pq"
LIBRARY = "adbc_rs_pq_17_11_v1"
ASSETS = Path(__file__).resolve().parents[1] / "vcpkg/ports" / PACKAGE
REPOSITORY = Path(__file__).resolve().parents[2]
ARCHIVES = (
    "src/interfaces/libpq/libpq.a",
    "src/common/libpgcommon_shlib.a",
    "src/port/libpgport_shlib.a",
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def check_pin(path, expected):
    require(path.is_file(), f"Required file missing: {path}")
    require(digest(path) == expected, f"Pin validation failed: {path}")


def preflight(args, pins, *, windows=False):
    if windows:
        require(platform.system() == "Windows", "Windows adapter requires Windows")
        require(
            platform.machine().lower() in ("amd64", "arm64", "aarch64", "x86_64"),
            "Unsupported CPU",
        )
    else:
        require(
            platform.system() in ("Darwin", "Linux"),
            "Only native Unix builds supported",
        )
        require(platform.machine() in ("arm64", "aarch64", "x86_64"), "Unsupported CPU")
    if args.openssl_libdir is None:
        args.openssl_libdir = args.openssl_prefix / "lib"
    for path in (
        args.work_dir,
        args.prefix,
        args.openssl_prefix,
        args.openssl_libdir,
        args.source_archive,
    ):
        require(path.is_absolute(), f"An absolute path is required: {path}")
        require(
            not any(c.isspace() for c in str(path)), "Build paths cannot contain spaces"
        )
    require(not os.path.lexists(args.work_dir), "Work directory must be new")
    for name in (
        "work_dir",
        "prefix",
        "openssl_prefix",
        "openssl_libdir",
        "source_archive",
    ):
        setattr(args, name, getattr(args, name).resolve())
    require(args.work_dir.parent.is_dir(), "Work directory parent must exist")
    require(
        args.prefix != args.work_dir and args.work_dir in args.prefix.parents,
        "Install prefix must be inside the new owned work directory",
    )
    require(not os.path.lexists(args.prefix), "Install prefix must be new")
    check_pin(args.source_archive, pins["source_sha256"])
    for name, key in (
        ("libpq-result-formats.patch", "patch_sha256"),
        ("namespace-macro-restore.patch", "namespace_patch_sha256"),
        ("PostgreSQL-COPYRIGHT", "license_sha256"),
    ):
        check_pin(ASSETS / name, pins[key])
    check_pin(Path(__file__), pins["build_script_sha256"])
    check_pin(
        Path(__file__).with_name("pgwire_private_pq_symbols.py"),
        pins["symbol_script_sha256"],
    )
    check_pin(
        Path(__file__).with_name("pgwire_private_pq_windows.py"),
        pins["windows_build_script_sha256"],
    )
    check_pin(REPOSITORY / "LICENSE.txt", pins["adbc_license_sha256"])
    check_pin(REPOSITORY / "NOTICE.txt", pins["adbc_notice_sha256"])
    require(
        (args.openssl_prefix / "include/openssl/ssl.h").is_file(),
        "Explicit OpenSSL headers missing",
    )
    require(
        (args.openssl_libdir / "pkgconfig/libssl.pc").is_file(),
        "Explicit OpenSSL libssl.pc missing",
    )
    require(
        (args.openssl_libdir / "pkgconfig/libcrypto.pc").is_file(),
        "Explicit OpenSSL libcrypto.pc missing",
    )


def install_package(
    prefix, source, mapping, pins, *, archives=None, config_header=None, windows=False
):
    """Only install uniquely named files; never upstream's generic libpq package."""
    include = prefix / "include" / PACKAGE
    share = prefix / "share" / PACKAGE
    for directory in (
        include,
        share,
        prefix / "lib/pkgconfig",
        prefix / "lib/cmake/AdbcRedshiftPrivatePQ",
    ):
        directory.mkdir(parents=True)
    names = (LIBRARY, LIBRARY + "_common", LIBRARY + "_port")
    archive_paths = archives if archives is not None else [source / a for a in ARCHIVES]
    require(len(archive_paths) == 3, "Exactly three private archives are required")
    for original, name in zip(archive_paths, names):
        filename = f"{name}.lib" if windows else f"lib{name}.a"
        shutil.copyfile(original, prefix / "lib" / filename)
    headers = {
        "src/interfaces/libpq/libpq-fe.h": (
            "adbc_rs_libpq_fe.h",
            {'"postgres_ext.h"': '"adbc_rs_postgres_ext.h"'},
        ),
        "src/include/postgres_ext.h": (
            "adbc_rs_postgres_ext.h",
            {'"pg_config_ext.h"': '"adbc_rs_pg_config_ext.h"'},
        ),
        "src/include/pg_config_ext.h": ("adbc_rs_pg_config_ext.h", {}),
    }
    for original, (name, replacements) in headers.items():
        header = source / original
        if original == "src/include/pg_config_ext.h" and config_header is not None:
            header = config_header
        contents = header.read_text()
        for old, new in replacements.items():
            require(contents.count(old) == 1, f"Unexpected include in {original}")
            contents = contents.replace(old, new)
        (include / name).write_text(contents)
    shutil.copyfile(mapping / "public-namespace.h", include / "adbc_rs_pq_namespace.h")
    shutil.copyfile(ASSETS / "adbc_rs_pq.h", include / "adbc_rs_pq.h")
    for name in (
        "PostgreSQL-COPYRIGHT",
        "libpq-result-formats.patch",
        "namespace-macro-restore.patch",
        "pins.json",
        "link_check.c",
    ):
        shutil.copyfile(ASSETS / name, share / name)
    shutil.copyfile(mapping / "symbols.json", share / "symbols.json")
    shutil.copyfile(mapping / "namespace.h", share / "namespace.h")
    if windows:
        shutil.copyfile(mapping / "coff-redefine.syms", share / "coff-redefine.syms")
    shutil.copyfile(
        ASSETS / "AdbcRedshiftPrivatePQConfig.cmake",
        prefix / "lib/cmake/AdbcRedshiftPrivatePQ/AdbcRedshiftPrivatePQConfig.cmake",
    )
    pc = (
        "prefix=${pcfiledir}/../..\nlibdir=${prefix}/lib\n"
        f"includedir=${{prefix}}/include/{PACKAGE}\nabi=1\n"
        f"Name: {PACKAGE}\nDescription: Isolated libpq with ADBC result formats\n"
        f"Version: {pins['package_version']}\n"
        "Cflags: -I${includedir}\n"
        "Libs: -L${libdir} " + " ".join("-l" + n for n in names) + "\n"
        "Requires.private: libssl libcrypto\nLibs.private: "
        + (
            "-lws2_32 -lsecur32 -ladvapi32 -lcrypt32 -lbcrypt -luser32\n"
            if windows
            else "-lm -pthread\n"
        )
    )
    (prefix / "lib/pkgconfig" / f"{PACKAGE}.pc").write_text(pc)
    recipe = share / "recipe"
    scripts = recipe / "ci/scripts"
    scripts.mkdir(parents=True)
    for name in (
        "pgwire_private_pq.py",
        "pgwire_private_pq_symbols.py",
        "pgwire_private_pq_windows.py",
    ):
        shutil.copyfile(Path(__file__).with_name(name), scripts / name)
    shutil.copytree(ASSETS, recipe / "ci/vcpkg/ports" / PACKAGE)
    for name in ("LICENSE.txt", "NOTICE.txt"):
        shutil.copyfile(REPOSITORY / name, share / name)
        shutil.copyfile(REPOSITORY / name, recipe / name)


def build(args, pins):
    preflight(
        args, pins
    )  # Must precede mkdir and every subprocess, even with python -O.
    work = args.work_dir
    work.mkdir()
    env = dict(os.environ, LC_ALL="C", LC_CTYPE="C", LANG="C", ZERO_AR_DATE="1")
    for name in (
        "CFLAGS",
        "CPPFLAGS",
        "LDFLAGS",
        "CXXFLAGS",
        "MAKEFLAGS",
        "MFLAGS",
        "CC",
        "CXX",
        "CPATH",
        "C_INCLUDE_PATH",
        "LIBRARY_PATH",
        "CONFIG_SITE",
    ):
        env.pop(name, None)
    env["CONFIG_SITE"] = "/dev/null"
    env["PKG_CONFIG_PATH"] = ""
    env["PKG_CONFIG_LIBDIR"] = str(args.openssl_libdir / "pkgconfig")
    commands = []

    def run(command, cwd=work):
        commands.append(command)
        number = len(commands)
        (work / f"{number:02d}-command.json").write_text(json.dumps(command) + "\n")
        with (work / f"{number:02d}.log").open("w") as output:
            subprocess.run(
                command,
                cwd=cwd,
                env=env,
                stdout=output,
                stderr=subprocess.STDOUT,
                check=True,
            )

    run([args.cc, "--version"])
    run(["make", "--version"])
    run(["pkg-config", "--modversion", "libssl", "libcrypto"])
    run(["pkg-config", "--libs", "--static", "libssl", "libcrypto"])
    ssl_link_flags = shlex.split((work / f"{len(commands):02d}.log").read_text())
    include_ssl = "-I" + str(args.openssl_prefix / "include")
    link_ssl = "-L" + str(args.openssl_libdir)
    optimization = "-O2" if args.build_type == "release" else "-O0"
    cflags = optimization + " -g0 -fPIC -fvisibility=hidden"
    mapping = work / "namespace"
    symbol_script = Path(__file__).with_name("pgwire_private_pq_symbols.py")
    macho = ["--macho"] if platform.system() == "Darwin" else []
    for phase in ("raw", "private"):
        phase_dir = work / phase
        phase_dir.mkdir()
        run(["tar", "-xjf", str(args.source_archive), "-C", str(phase_dir)])
        source = phase_dir / "postgresql-17.11"
        for patch in ("libpq-result-formats.patch", "namespace-macro-restore.patch"):
            # The restore patch encodes private names and belongs only in pass two.
            if phase == "private" or patch == "libpq-result-formats.patch":
                run(["patch", "--batch", "-p1", "-i", str(ASSETS / patch)], source)
        run(
            [
                str(source / "configure"),
                "--prefix=/adbc-redshift-private-pq",
                "--without-readline",
                "--without-zlib",
                "--without-icu",
                "--disable-nls",
                "--without-gssapi",
                "--without-ldap",
                "--with-ssl=openssl",
                "CC=" + args.cc,
                "CFLAGS=" + cflags,
                "CPPFLAGS=" + include_ssl,
                "LDFLAGS=" + link_ssl,
            ],
            source,
        )
        run(["make", "-j1", "-C", str(source / "src/backend"), "generated-headers"])
        cppflags = include_ssl
        if phase == "private":
            cppflags += " -include " + str(mapping / "namespace.h")
        for directory, target in (
            ("src/port", "libpgport_shlib.a"),
            ("src/common", "libpgcommon_shlib.a"),
            ("src/interfaces/libpq", "libpq.a"),
        ):
            run(
                [
                    "make",
                    "-j1",
                    "-C",
                    str(source / directory),
                    "CPPFLAGS=" + cppflags,
                    "CFLAGS=" + cflags,
                    target,
                ]
            )
        abi_object = phase_dir / "package-abi.o"
        abi_command = [args.cc, *cflags.split()]
        if phase == "private":
            abi_command += ["-include", str(mapping / "namespace.h")]
        run([*abi_command, "-c", str(ASSETS / "package-abi.c"), "-o", str(abi_object)])
        run(["ar", "r", str(source / ARCHIVES[0]), str(abi_object)])
        run(["ranlib", str(source / ARCHIVES[0])])
        action = [
            "generate",
            "--exports",
            str(source / "src/interfaces/libpq/exports.txt"),
        ]
        if phase == "private":
            action = ["audit"]
        run(
            [
                sys.executable,
                str(symbol_script),
                *action,
                *macho,
                "--directory",
                str(mapping),
                *[str(source / a) for a in ARCHIVES],
            ]
        )
    install_package(args.prefix, source, mapping, pins)
    # This static link and runtime probe uses only NULL connections and PGresults.
    private_libs = [
        str(args.prefix / "lib" / ("lib" + LIBRARY + suffix + ".a"))
        for suffix in ("", "_common", "_port")
    ]
    probe = work / "package-link-check"
    run(
        [
            args.cc,
            "-I" + str(args.prefix / "include" / PACKAGE),
            str(ASSETS / "link_check.c"),
            *private_libs,
            *ssl_link_flags,
            "-lm",
            "-pthread",
            "-o",
            str(probe),
        ]
    )
    run([str(probe)])
    share = args.prefix / "share" / PACKAGE
    inputs = {
        str(p.relative_to(ASSETS)): digest(p)
        for p in sorted(ASSETS.rglob("*"))
        if p.is_file()
    }
    inputs["build-script"] = digest(Path(__file__))
    inputs["symbol-script"] = digest(symbol_script)
    receipt = {
        "status": "built-audited-installed-socket-free-probe-passed",
        "pins": pins,
        "recipe_inputs": inputs,
        "system": platform.system(),
        "machine": platform.machine(),
        "compiler": args.cc,
        "cflags": cflags,
        "openssl_prefix": str(args.openssl_prefix),
        "openssl_libdir": str(args.openssl_libdir),
        "deployment_target": env.get("MACOSX_DEPLOYMENT_TARGET"),
        "commands": commands,
        "logs": {p.name: digest(p) for p in sorted(work.glob("*.log"))},
        "installed_files": {
            str(p.relative_to(args.prefix)): digest(p)
            for p in sorted(args.prefix.rglob("*"))
            if p.is_file()
        },
        "scope": (
            "private dependency only; driver linkage and release matrices unchanged"
        ),
    }
    (share / "provenance.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(share / "provenance.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--openssl-prefix", type=Path, required=True)
    parser.add_argument("--openssl-libdir", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--build-type", choices=("debug", "release"), default="debug")
    args = parser.parse_args()
    pins = json.loads((ASSETS / "pins.json").read_text())
    build(args, pins)


if __name__ == "__main__":
    main()
