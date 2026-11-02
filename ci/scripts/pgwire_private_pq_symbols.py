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

"""Small macOS/ELF inventory generator and fail-closed archive audit."""

import argparse
import json
import re
import subprocess
from pathlib import Path

PREFIX = "adbc_rs_pq_17_11_v1_"


def inventory(archives, macho):
    defined, undefined = set(), set()
    for archive in archives:
        output = subprocess.check_output(["nm", "-g", "-P", str(archive)], text=True)
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
            if kind in {"U", "w", "v"}:
                undefined.add(name)
            elif kind in {"T", "D", "B", "S", "R", "C", "G", "V", "W"}:
                if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name):
                    raise ValueError(f"defined global is not a C identifier: {name}")
                defined.add(name)
            else:
                raise ValueError(f"unreviewed nm global kind: {line}")
    if not defined:
        raise ValueError("no definitions found")
    return defined, undefined


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["generate", "audit"])
    parser.add_argument("--macho", action="store_true")
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--exports", type=Path)
    parser.add_argument("archives", type=Path, nargs="+")
    args = parser.parse_args()
    defined, undefined = inventory(args.archives, args.macho)
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
        for filename, names in [
            ("namespace.h", defined),
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
