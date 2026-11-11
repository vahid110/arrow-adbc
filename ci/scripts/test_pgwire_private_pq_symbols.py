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

import contextlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import pgwire_private_pq_symbols as symbols


class SymbolAuditTest(unittest.TestCase):
    def test_all_global_kinds_and_external_darwin_alias(self):
        output = (
            "part.o:\n_PQlibVersion T 0 0\n_table S 0 0\n_common C 0 0\n"
            "_weak W 0 0\n_realpath$DARWIN_EXTSN U 0 0\n"
        )
        with patch("subprocess.check_output", return_value=output):
            defined, undefined = symbols.inventory([Path("library.a")], True)
        self.assertEqual(defined, {"PQlibVersion", "table", "common", "weak"})
        self.assertEqual(undefined, {"realpath$DARWIN_EXTSN"})

    def test_unknown_definition_not_silently_ignored(self):
        with patch("subprocess.check_output", return_value="_compiler.special T 0 0\n"):
            with self.assertRaisesRegex(ValueError, "not a C identifier"):
                symbols.inventory([Path("library.a")], True)

    def audit(self, output):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            (directory / "symbols.json").write_text(
                json.dumps({"defined": ["PQf", "data"]})
            )
            argv = ["symbols.py", "audit", "--macho", "--directory", temp, "library.a"]
            with (
                patch("sys.argv", argv),
                patch("subprocess.check_output", return_value=output),
            ):
                with contextlib.redirect_stdout(io.StringIO()):
                    symbols.main()

    def test_exact_prefixed_set_passes(self):
        p = symbols.PREFIX
        self.audit(f"_{p}PQf T 0 0\n_{p}data S 0 0\n_{p}data U 0 0\n_malloc U 0 0\n")

    def test_unprefixed_definition_fails(self):
        p = symbols.PREFIX
        with self.assertRaisesRegex(ValueError, "unexpected_definitions"):
            self.audit(f"_{p}PQf T 0 0\n_data S 0 0\n")

    def test_unprefixed_reference_fails(self):
        p = symbols.PREFIX
        with self.assertRaisesRegex(
            ValueError, '"unprefixed_internal_references": \\['
        ):
            self.audit(f"_{p}PQf T 0 0\n_{p}data S 0 0\n_data U 0 0\n")

    def test_missing_private_dependency_fails(self):
        p = symbols.PREFIX
        with self.assertRaisesRegex(ValueError, '"unresolved_private_references": \\['):
            self.audit(f"_{p}PQf T 0 0\n_{p}data S 0 0\n_{p}missing U 0 0\n")


class CoffSymbolAuditTest(unittest.TestCase):
    literal = "__real@3ff0000000000000"

    def test_readonly_numeric_constants_and_compiler_metadata(self):
        output = (
            "@feat.00 A 800 0\nPQlibVersion T 0 0\n"
            + self.literal
            + " R 0 0\n__imp_malloc U 0 0\n"
        )
        metadata = set()
        with patch("subprocess.check_output", return_value=output) as command:
            defined, undefined = symbols.inventory(
                [Path("libpq.lib")], False, coff=True, nm="llvm-nm", metadata=metadata
            )
        self.assertEqual(defined, {"PQlibVersion", self.literal})
        self.assertEqual(undefined, {"__imp_malloc"})
        self.assertEqual(metadata, {("@feat.00", 0x800)})
        self.assertEqual(command.call_args.args[0][0], "llvm-nm")

    def test_unreviewed_special_symbols_and_writable_constants_fail(self):
        for line in (
            "??_C@string R 0 0",
            "@unknown A 0 0",
            self.literal + " D 0 0",
            "__real@bad R 0 0",
        ):
            with (
                self.subTest(line=line),
                patch("subprocess.check_output", return_value=line),
            ):
                with self.assertRaises(ValueError):
                    symbols.inventory([Path("libpq.lib")], False, coff=True)

    def test_scoped_static_and_its_c_tokens_are_renamed_at_object_level(self):
        owner = "__local_stdio_scanf_options"
        name = f"?_OptionsStorage@?1??{owner}@@9@9"
        names = {name, owner, "_OptionsStorage", self.literal, "PQlibVersion"}
        self.assertEqual(symbols.coff_object_names(names), names - {"PQlibVersion"})
        output = f"{name} B 0 0\n" + f"{symbols.PREFIX}{name} D 0 0\n"
        with patch("subprocess.check_output", return_value=output):
            self.assertEqual(
                symbols.inventory([Path("a")], False, coff=True)[0],
                {name, symbols.PREFIX + name},
            )
        with patch(
            "subprocess.check_output",
            return_value=f"{name} T 0 0\n??bad D 0 0\n?unknown R 0 0\n",
        ):
            with self.assertRaises(ValueError) as rejected:
                symbols.inventory([Path("a")], False, coff=True)
            for symbol in (name, "??bad", "?unknown"):
                self.assertIn(symbol, str(rejected.exception))

    @unittest.skipUnless(
        all(
            shutil.which(t)
            for t in ("clang", "llvm-lib", "llvm-nm", "llvm-objcopy", "lld-link")
        ),
        "requires LLVM COFF tools",
    )
    def test_real_coff_archive_indexes_and_imports(self):
        # No SDK, CRT or Windows execution is required for this link test.
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "owner.c").write_text(
                "extern int dependency(void);\n"
                "volatile long long scoped "
                '__asm__("?_OptionsStorage@?1??owner@@9@9");\n'
                "int owner(void) {return scoped;}\n"
                "int PQlibVersion(void) {return owner()+dependency();}\n"
                "int ADBCPQprivatePackageAbi(void) {return 1;}\n"
                "int ADBCPQexecPreparedFormats(void) {return 1;}\n"
                "int ADBCPQsendQueryPreparedFormats(void) {return 1;}\n"
            )
            (base / "dependency.c").write_text(
                "__declspec(dllimport) int external_system_fn(void);\n"
                "int dependency(void) {return external_system_fn();}\n"
            )
            (base / "consumer.c").write_text(
                f"int PQlibVersion(void), {symbols.PREFIX}PQlibVersion(void);\n"
                "int stub(void) {return 1;}\n"
                "int (*__imp_external_system_fn)(void) = stub;\n"
                "int entry(void) {return PQlibVersion() + "
                f"{symbols.PREFIX}PQlibVersion();}}\n"
            )
            (base / "exports.txt").write_text("PQlibVersion 1\n")
            for arch in ("x86_64", "aarch64"):
                folder = base / arch
                folder.mkdir()

                def run(*args):
                    subprocess.run(args, check=True, cwd=folder, capture_output=True)

                cc = ["clang", f"--target={arch}-pc-windows-msvc", "-fcommon", "-c"]
                for phase in ("raw", "private"):
                    include = [] if phase == "raw" else ["-include", "map/namespace.h"]
                    for src in ("owner", "dependency"):
                        run(*cc, *include, f"../{src}.c", "-o", f"{src}.obj")
                    run("llvm-lib", f"/out:{phase}.lib", "owner.obj", "dependency.obj")
                    tool = [sys.executable, symbols.__file__]
                    common = ["--coff", "--nm", "llvm-nm", "--directory", "map"]
                    if phase == "raw":
                        run(
                            *tool,
                            "generate",
                            *common,
                            "--exports",
                            "../exports.txt",
                            "raw.lib",
                        )
                    else:
                        run(
                            "llvm-objcopy",
                            "--redefine-syms=map/coff-redefine.syms",
                            "private.lib",
                            "renamed.lib",
                        )
                        run(*tool, "audit", *common, "renamed.lib")
                run(*cc, "../consumer.c", "-o", "consumer.obj")
                run(
                    "lld-link",
                    "/entry:entry",
                    "/subsystem:console",
                    "/nodefaultlib",
                    "/out:consumer.exe",
                    "consumer.obj",
                    "raw.lib",
                    "renamed.lib",
                )
                self.assertTrue((folder / "consumer.exe").is_file())
                manifest_path = folder / "map/symbols.json"
                manifest = json.loads(manifest_path.read_text())
                self.assertEqual(
                    manifest["coff_common"], [["?_OptionsStorage@?1??owner@@9@9", 8]]
                )
                manifest["coff_common"][0][1] = 4
                manifest_path.write_text(json.dumps(manifest))
                with self.assertRaises(subprocess.CalledProcessError) as changed:
                    run(*tool, "audit", *common, "renamed.lib")
                self.assertIn(b"missing_common_allocations", changed.exception.stderr)

    def test_macro_and_object_maps_partition_all_definitions(self):
        public = [
            "PQlibVersion",
            "ADBCPQprivatePackageAbi",
            "ADBCPQexecPreparedFormats",
            "ADBCPQsendQueryPreparedFormats",
        ]
        output = (
            "@feat.00 A 800 0\n"
            + "".join(name + " T 0 0\n" for name in public)
            + self.literal
            + " R 0 0\n"
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            exports = root / "exports.txt"
            exports.write_text("PQlibVersion 1\n")
            argv = [
                "symbols",
                "generate",
                "--coff",
                "--exports",
                str(exports),
                "--directory",
                str(root),
                "libpq.lib",
            ]
            with (
                patch("sys.argv", argv),
                patch("subprocess.check_output", return_value=output),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                symbols.main()
            self.assertNotIn(self.literal, (root / "namespace.h").read_text())
            self.assertEqual(
                (root / "coff-redefine.syms").read_text(),
                self.literal + " " + symbols.PREFIX + self.literal + "\n",
            )
            manifest = json.loads((root / "symbols.json").read_text())
            self.assertEqual(set(manifest["defined"]), set(public) | {self.literal})
            private = (
                "@feat.00 A 800 0\n"
                + "".join(symbols.PREFIX + name + " T 0 0\n" for name in public)
                + symbols.PREFIX
                + self.literal
                + " R 0 0\n"
            )
            argv = ["symbols", "audit", "--coff", "--directory", str(root), "libpq.lib"]
            for mutation in (
                None,
                "constant",
                "metadata",
                "reference",
                "import",
                "private-import",
            ):
                changed = private
                if mutation == "constant":
                    changed = changed.replace(
                        symbols.PREFIX + self.literal, self.literal
                    )
                elif mutation == "metadata":
                    changed = changed.replace("A 800", "A 1")
                elif mutation == "reference":
                    changed += "PQlibVersion U 0 0\n"
                elif mutation == "import":
                    changed += "__imp_PQlibVersion U 0 0\n"
                elif mutation == "private-import":
                    changed += "__imp_" + symbols.PREFIX + "PQlibVersion U 0 0\n"
                with (
                    self.subTest(mutation=mutation),
                    patch("sys.argv", argv),
                    patch("subprocess.check_output", return_value=changed),
                    contextlib.redirect_stdout(io.StringIO()),
                ):
                    if mutation:
                        with self.assertRaises(ValueError):
                            symbols.main()
                    else:
                        symbols.main()


if __name__ == "__main__":
    unittest.main()
