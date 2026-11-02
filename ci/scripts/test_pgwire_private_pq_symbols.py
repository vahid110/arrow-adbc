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


if __name__ == "__main__":
    unittest.main()
