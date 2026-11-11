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

"""Pinned native Windows /MD static-frontend package; no database operations."""

import argparse
import json
import os
import platform
import re
import subprocess
import sys
import tarfile
from collections import deque
from pathlib import Path

import pgwire_private_pq as package

ARCHITECTURES = {"x64": "IMAGE_FILE_MACHINE_AMD64", "arm64": "IMAGE_FILE_MACHINE_ARM64"}
# These static _shlib variants provide the public frontend encoding ABI.
# Plain libpgcommon compiles those definitions with USE_PRIVATE_ENCODING_FUNCS.
TARGETS = ("libpq", "libpgcommon_shlib", "libpgport_shlib")


def native_architecture():
    machine = os.environ.get("PROCESSOR_ARCHITEW6432", platform.machine()).lower()
    return {"amd64": "x64", "x86_64": "x64", "arm64": "arm64", "aarch64": "arm64"}.get(
        machine
    )


def preflight(args, pins):
    package.preflight(args, pins, windows=True)
    package.require(
        args.architecture == native_architecture(), "Cross compilation is not admitted"
    )
    package.require(args.crt == "md", "Windows private package requires /MD")
    for tool in (
        args.cc,
        args.lib,
        args.git,
        args.ninja,
        args.nm,
        args.objcopy,
        args.readobj,
        args.pkgconf,
        args.bison,
        args.flex,
        args.perl,
    ):
        package.require(
            tool.is_absolute() and tool.is_file(), f"Explicit tool missing: {tool}"
        )
    package.require(args.lib.name.lower() == "lib.exe", "MSVC lib.exe is required")
    package.require(bool(args.meson_command_element), "Explicit Meson command required")
    for element in args.meson_command_element:
        if element == "-I":
            continue
        path = Path(element)
        package.require(
            path.is_absolute() and path.is_file(), "Explicit Meson command file missing"
        )
    for name in ("libssl.lib", "libcrypto.lib"):
        package.require(
            (args.openssl_libdir / name).is_file(),
            f"Explicit OpenSSL archive missing: {name}",
        )


def static_targets(targets, build_dir):
    """Select exact static frontend variants, never server/shared variants."""
    selected = []
    for name in TARGETS:
        matches = [
            t
            for t in targets
            if t.get("name") == name and t.get("type") == "static library"
        ]
        package.require(
            len(matches) == 1, f"Missing/ambiguous Meson static target: {name}"
        )
        filenames = matches[0].get("filename", [])
        package.require(len(filenames) == 1, "Unexpected Meson archive count")
        path = Path(filenames[0]).resolve()
        package.require(
            path.is_relative_to(build_dir.resolve()),
            "Meson output escapes build directory",
        )
        selected.append(path)
    return selected


def verify_coff(text, architecture):
    machines = re.findall(r"Machine:\s*(IMAGE_FILE_MACHINE_[A-Z0-9_]+)", text)
    package.require(bool(machines), "No COFF machine records")
    package.require(
        set(machines) == {ARCHITECTURES[architecture]}, "Wrong COFF target machine"
    )
    defaults = {
        m.upper()
        for m in re.findall(r"[/-]DEFAULTLIB:[\"]?([A-Za-z0-9_]+)", text, re.I)
    }
    package.require("MSVCRT" in defaults, "COFF archive does not record /MD runtime")
    package.require(
        not defaults.intersection({"LIBCMT", "LIBCMTD", "MSVCRTD"}),
        "Wrong COFF runtime",
    )


def extract_source(archive, directory):
    with tarfile.open(archive, "r:bz2") as source:
        for member in source.getmembers():
            name = Path(member.name)
            package.require(
                not name.is_absolute() and ".." not in name.parts,
                "Unsafe source member",
            )
            package.require(
                member.isfile() or member.isdir(), "Unsupported source archive member"
            )
            package.require(
                name.parts and name.parts[0] == "postgresql-17.11",
                "Unexpected source root",
            )
        source.extractall(directory)
    return directory / "postgresql-17.11"


def meson_options(args):
    return [
        "--backend=ninja",
        # PostgreSQL defaults to /usr/local/pgsql, a drive-relative path on Windows.
        "--prefix=" + args.prefix.as_posix(),
        "--buildtype=release",
        "--wrap-mode=nodownload",
        "-Dauto_features=disabled",
        "-Dssl=openssl",
        "-Db_pch=false",
        "-Db_lto=false",
        "-Db_vscrt=md",
        "-Ddebug=false",
        "-Doptimization=2",
        "-Dc_args=" + json.dumps(["/GF-"]),
        "-Dextra_include_dirs="
        + json.dumps([(args.openssl_prefix / "include").as_posix()]),
        "-Dextra_lib_dirs=" + json.dumps([args.openssl_libdir.as_posix()]),
        "-DBISON=" + json.dumps([args.bison.as_posix()]),
        "-DFLEX=" + json.dumps([args.flex.as_posix()]),
        "-DPERL=" + args.perl.as_posix(),
    ]


def build(args, pins):
    preflight(args, pins)  # No directory or subprocess before all input checks.
    work = args.work_dir
    work.mkdir()
    env = dict(os.environ)
    for key in (
        "CC",
        "CXX",
        "CFLAGS",
        "CXXFLAGS",
        "CPPFLAGS",
        "LDFLAGS",
        "CL",
        "_CL_",
        "LINK",
        "_LINK_",
        "CMAKE_PREFIX_PATH",
        "PKG_CONFIG_PATH",
        "PYTHONPATH",
        "GIT_DIR",
        "GIT_WORK_TREE",
        "GIT_INDEX_FILE",
    ):
        env.pop(key, None)
    env["PATH"] = (
        os.pathsep.join(
            str(p.parent)
            for p in (args.lib, args.ninja, args.bison, args.flex, args.perl)
        )
        + os.pathsep
        + env.get("PATH", "")
    )
    env["PKG_CONFIG_LIBDIR"] = str(args.openssl_libdir / "pkgconfig")
    env["GIT_CEILING_DIRECTORIES"] = str(work)
    env["GIT_CONFIG_NOSYSTEM"] = "1"
    env["GIT_CONFIG_GLOBAL"] = os.devnull
    commands = []

    def run(command, cwd=work):
        command = [str(c) for c in command]
        commands.append(command)
        number = len(commands)
        (work / f"{number:02d}-command.json").write_text(json.dumps(command) + "\n")
        log = work / f"{number:02d}.log"
        try:
            with log.open("w") as output:
                subprocess.run(
                    command,
                    cwd=cwd,
                    env=env,
                    stdout=output,
                    stderr=subprocess.STDOUT,
                    check=True,
                )
        except subprocess.CalledProcessError:
            with log.open(errors="replace") as failed_log:
                print(f"Last 60 lines of {log.name}:", file=sys.stderr)
                sys.stderr.writelines(deque(failed_log, maxlen=60))
            raise
        return log.read_text()

    native_file = work / "native.ini"
    native_file.write_text(
        # Meson 0.58 probes bare lib.exe with /? but an absolute path with
        # --version, misclassifying MSVC's librarian as Unix ar. The selected
        # librarian directory is first on PATH for both Meson and Ninja.
        "[binaries]\nar = 'lib.exe'\n"
        + "".join(
            name + " = " + repr(path.as_posix()) + "\n"
            for name, path in (
                ("c", args.cc),
                ("pkgconfig", args.pkgconf),
            )
        )
    )
    run([args.cc, "/?"])
    for tool in (args.nm, args.objcopy, args.readobj, args.ninja):
        run([tool, "--version"])
    run([*args.meson_command_element, "--version"])
    mapping = work / "namespace"
    symbols = Path(__file__).with_name("pgwire_private_pq_symbols.py")
    raw_dir = work / "raw"
    raw_dir.mkdir()
    source = extract_source(args.source_archive, raw_dir)
    run(
        [
            args.git,
            "apply",
            "--whitespace=nowarn",
            package.ASSETS / "libpq-result-formats.patch",
        ],
        source,
    )
    build_dir = raw_dir / "build"
    run(
        [
            *args.meson_command_element,
            "setup",
            build_dir,
            source,
            "--native-file",
            native_file,
            *meson_options(args),
        ]
    )
    targets = json.loads(
        run([*args.meson_command_element, "introspect", "--targets", build_dir])
    )
    archives = static_targets(targets, build_dir)
    run(
        [
            args.ninja,
            "-C",
            build_dir,
            "-j1",
            *[p.relative_to(build_dir).as_posix() for p in archives],
        ]
    )
    abi_object = raw_dir / "package-abi.obj"
    cflags = ["/nologo", "/MD", "/O2", "/GF-"]
    run(
        [
            args.cc,
            *cflags,
            "/c",
            package.ASSETS / "package-abi.c",
            "/Fo" + str(abi_object),  # codespell:ignore fo
        ]
    )
    merged = raw_dir / "libpq-with-abi.lib"
    run([args.lib, "/NOLOGO", "/OUT:" + str(merged), archives[0], abi_object])
    archives[0] = merged
    for archive in archives:
        verify_coff(
            run([args.readobj, "--file-headers", "--coff-directives", archive]),
            args.architecture,
        )
    raw_archives = list(archives)
    symbol_tool = [sys.executable, symbols]
    symbol_options = ["--coff", "--nm", args.nm, "--directory", mapping]
    run(
        [
            *symbol_tool,
            "generate",
            *symbol_options,
            "--exports",
            source / "src/interfaces/libpq/exports.txt",
            *raw_archives,
        ]
    )
    private_dir = work / "private"
    private_dir.mkdir()
    renames = mapping / "coff-redefine.syms"
    archives = []
    for archive in raw_archives:
        renamed = private_dir / archive.name
        run([args.objcopy, "--redefine-syms=" + str(renames), archive, renamed])
        verify_coff(
            run([args.readobj, "--file-headers", "--coff-directives", renamed]),
            args.architecture,
        )
        archives.append(renamed)
    run([*symbol_tool, "audit", *symbol_options, *archives])
    package.install_package(
        args.prefix,
        source,
        mapping,
        pins,
        archives=archives,
        config_header=build_dir / "src/include/pg_config_ext.h",
        windows=True,
    )
    libraries = [
        args.prefix / "lib" / (package.LIBRARY + suffix + ".lib")
        for suffix in ("", "_common", "_port")
    ]
    probe = work / "package-link-check.exe"
    run(
        [
            args.cc,
            "/nologo",
            "/MD",
            "/I" + str(args.prefix / "include" / package.PACKAGE),
            package.ASSETS / "link_check.c",
            *libraries,
            args.openssl_libdir / "libssl.lib",
            args.openssl_libdir / "libcrypto.lib",
            *[name + ".lib" for name in package.WINDOWS_SYSTEM_LIBS],
            "/Fe" + str(probe),
            "/Fo" + str(work / "probe.obj"),  # codespell:ignore fo
        ]
    )
    run([probe])  # NULL-only API calls, never a socket or a credential read.
    share = args.prefix / "share" / package.PACKAGE
    receipt = {
        "raw_archive_digests": {p.name: package.digest(p) for p in raw_archives},
        "status": "built-audited-installed-socket-free-probe-passed",
        "pins": pins,
        "system": "Windows",
        "machine": args.architecture,
        "crt": "md",
        "compiler": str(args.cc),
        "meson_command": args.meson_command_element,
        "commands": commands,
        "recipe_inputs": {
            str(p.relative_to(package.ASSETS)): package.digest(p)
            for p in sorted(package.ASSETS.rglob("*"))
            if p.is_file()
        },
        "logs": {p.name: package.digest(p) for p in sorted(work.glob("*.log"))},
        "installed_files": {
            p.relative_to(args.prefix).as_posix(): package.digest(p)
            for p in sorted(args.prefix.rglob("*"))
            if p.is_file()
        },
        "scope": (
            "private dependency only; native Windows platform/driver "
            "qualification remains separate"
        ),
    }
    (share / "provenance.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(share / "provenance.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in (
        "source-archive",
        "work-dir",
        "prefix",
        "openssl-prefix",
        "openssl-libdir",
    ):
        parser.add_argument("--" + name, type=Path, required=name != "openssl-libdir")
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
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--meson-command-element", action="append", required=True)
    parser.add_argument("--architecture", choices=ARCHITECTURES, required=True)
    parser.add_argument("--crt", choices=("md",), required=True)
    args = parser.parse_args()
    pins = json.loads((package.ASSETS / "pins.json").read_text())
    build(args, pins)


if __name__ == "__main__":
    main()
