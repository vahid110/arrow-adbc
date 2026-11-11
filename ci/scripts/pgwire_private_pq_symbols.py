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

"""Fail-closed Mach-O/ELF/COFF inventory and private archive audit."""

import argparse
import json
import re
import subprocess
from pathlib import Path

PREFIX = "adbc_rs_pq_17_11_v1_"


def coff_literal(name):
    """MSVC content-addressed numeric constants; string pooling is disabled."""
    if name.startswith(PREFIX):
        name = name[len(PREFIX) :]
    return bool(re.fullmatch(r"__(real|xmm|ymm)@[0-9A-Fa-f]+", name)) and (
        (name.startswith("__real@") and len(name.split("@")[1]) in (8, 16))
        or (name.startswith("__xmm@") and len(name.split("@")[1]) == 32)
        or (name.startswith("__ymm@") and len(name.split("@")[1]) == 64)
    )


def coff_scoped_static(name):
    """MSVC decorated storage for a static local in an extern C function."""
    return re.fullmatch(
        r"\?([A-Za-z_][A-Za-z_0-9]*)@\?(?:[0-9]|[A-P]+@)\?\?"
        r"([A-Za-z_][A-Za-z_0-9]*)@@9@9",
        name.removeprefix(PREFIX),
    )


def inventory(archives, macho, *, coff=False, nm="nm", metadata=None, common=None):
    defined, undefined = set(), set()
    unsupported_coff = set()
    for archive in archives:
        output = subprocess.check_output([nm, "-g", "-P", str(archive)], text=True)
        for line in output.splitlines():
            line = line.strip()
            if not line or line.endswith(":"):
                continue
            fields = line.split()
            if len(fields) < 2 or len(fields[1]) != 1:
                raise ValueError(f"unparsed nm line: {line}")
            name, kind = fields[:2]
            if macho:
                if not name.startswith("_"):
                    raise ValueError(f"unexpected Mach-O global: {name}")
                name = name[1:]
            if coff and kind == "A" and name in ("@feat.00", "@comp.id"):
                if len(fields) < 3 or not re.fullmatch(r"[0-9A-Fa-f]+", fields[2]):
                    raise ValueError("Unparsed COFF compiler metadata")
                if metadata is not None:
                    metadata.add((name, int(fields[2], 16)))
                continue
            if coff and kind == "C":
                # COFF common symbols encode their allocation size in the value
                # column of llvm-nm -P; the final size column may be zero.
                if len(fields) < 3 or not re.fullmatch(r"[0-9A-Fa-f]+", fields[2]):
                    raise ValueError("Unparsed COFF common allocation")
                if common is not None:
                    common.add((name, int(fields[2], 16)))
            if kind in {"U", "w", "v"}:
                undefined.add(name)
            elif kind in {"T", "D", "B", "S", "R", "C", "G", "V", "W"}:
                if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name) and not (
                    coff
                    and (
                        (kind == "R" and coff_literal(name))
                        or (
                            kind in {"B", "C", "D", "R", "V"}
                            and coff_scoped_static(name)
                        )
                    )
                ):
                    if coff:
                        unsupported_coff.add((name, kind))
                        continue
                    raise ValueError(f"defined global is not a C identifier: {name}")
                defined.add(name)
            else:
                if coff:
                    unsupported_coff.add((name, kind))
                else:
                    raise ValueError(f"unreviewed nm global kind: {line}")
    if unsupported_coff:
        raise ValueError(f"Unreviewed COFF definitions: {sorted(unsupported_coff)}")
    if not defined:
        raise ValueError("no definitions found")
    return defined, undefined


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["generate", "audit"])
    formats = parser.add_mutually_exclusive_group()
    formats.add_argument("--macho", action="store_true")
    formats.add_argument("--coff", action="store_true")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--exports", type=Path)
    parser.add_argument("archives", type=Path, nargs="+")
    args = parser.parse_args()
    metadata, common = set(), set()
    defined, undefined = inventory(
        args.archives,
        args.macho,
        coff=args.coff,
        nm=args.nm,
        metadata=metadata,
        common=common,
    )
    if args.mode == "generate":
        if args.exports is None:
            parser.error("generate requires --exports")
        public = {
            line.split()[0]
            for line in args.exports.read_text().splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }
        public |= {
            "ADBCPQexecPreparedFormats",
            "ADBCPQsendQueryPreparedFormats",
            "ADBCPQprivatePackageAbi",
        }
        if not public <= defined:
            raise ValueError(f"public definitions absent: {sorted(public - defined)}")
        args.directory.mkdir(parents=True, exist_ok=True)
        object_names = defined if args.coff else set()
        for filename, names in [
            ("namespace.h", defined - object_names),
            ("public-namespace.h", public),
        ]:
            content = (
                "/* Generated from ALL defined globals; private build input only. */\n"
                "#pragma once\n"
            )
            content += "".join(
                f"#define {name} {PREFIX}{name}\n" for name in sorted(names)
            )
            (args.directory / filename).write_text(content)
        manifest = {
            "prefix": PREFIX,
            "defined": sorted(defined),
            "public": sorted(public),
        }

        if args.coff:
            manifest["coff_external_references"] = sorted(undefined - defined)
            manifest["coff_metadata"] = sorted(metadata)
            manifest["coff_common"] = sorted(common)
            manifest["coff_object_symbols"] = sorted(object_names)
            (args.directory / "coff-redefine.syms").write_text(
                "".join(f"{name} {PREFIX}{name}\n" for name in sorted(object_names))
            )
        (args.directory / "symbols.json").write_text(
            json.dumps(manifest, indent=2) + "\n"
        )
        print(
            json.dumps(
                {
                    "defined_count": len(defined),
                    "public_count": len(public),
                    "non_public_count": len(defined - public),
                }
            )
        )
    else:
        manifest = json.loads((args.directory / "symbols.json").read_text())
        original = set(manifest["defined"])
        expected = {PREFIX + name for name in original}
        problems = {
            "missing_definitions": sorted(expected - defined),
            "unexpected_definitions": sorted(defined - expected),
            "unprefixed_internal_references": sorted(undefined & original),
            "unresolved_private_references": sorted(
                name for name in undefined - defined if name.startswith(PREFIX)
            ),
        }
        if args.coff:
            external = set(manifest["coff_external_references"])
            problems["missing_external_references"] = sorted(
                external - (undefined - defined)
            )
            problems["unexpected_external_references"] = sorted(
                (undefined - defined) - external
            )
            expected_common = {
                (PREFIX + name, size) for name, size in manifest["coff_common"]
            }
            problems["missing_common_allocations"] = sorted(expected_common - common)
            problems["unexpected_common_allocations"] = sorted(common - expected_common)
            # Static private symbols cannot resolve via the public DLL import table.
            problems["imported_internal_references"] = sorted(
                name
                for name in undefined
                if name.startswith("__imp_")
                and (name[6:] in original or name[6:].startswith(PREFIX))
            )
        if args.coff and manifest.get("coff_metadata") != [
            list(m) for m in sorted(metadata)
        ]:
            problems["changed_coff_compiler_metadata"] = sorted(metadata)
            # A change to an empty set must fail too.
            if not metadata:
                problems["changed_coff_compiler_metadata"] = ["metadata removed"]
        if any(problems.values()):
            raise ValueError(json.dumps(problems, indent=2))
        print(
            json.dumps(
                {
                    "status": "all-definitions-prefixed-no-original-references",
                    "defined_count": len(defined),
                }
            )
        )


if __name__ == "__main__":
    main()
