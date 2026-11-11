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

"""Offline counterexamples for the installed mixed-HLL recipe (no database)."""

import io
import os
import unittest
from unittest.mock import patch

import pgwire_vendor_public_test as vendor
import pyarrow as pa


class MixedOracleTest(unittest.TestCase):
    def setUp(self):
        self.case = vendor.VendorPublicTest("test_mixed_hll_native_projection")
        self.case.connection_options = {
            "adbc.redshift.temporal_representation": "arrow"
        }
        names = [n for n, _, _ in vendor.MIXED_FIELDS]
        columns = {name: pa.array([1, None]) for name in names}
        columns.update(
            i64=[9007199254740993, None],
            vb=[bytes.fromhex("0080ff5c"), None],
            sv=['{"n":1}', None],
            gm=[vendor.GEOMETRY_POINT, None],
            gg=[vendor.GEOGRAPHY_POINT, None],
            ts=pa.array([1, None], type=pa.timestamp("us")),
        )
        self.ordinary = pa.table({"id": [1, 2], **columns})
        field = vendor.encoded_field("HLLSKETCH", name="h")
        values = pa.array(["a stored sketch", None], type=field.type)
        self.hll = pa.Table.from_arrays([values], schema=pa.schema([field]))
        self.mixed = self.ordinary.add_column(0, field.with_name("first_h"), values)
        self.mixed = self.mixed.append_column(field.with_name("last_h"), values)

    def test_mixed_projection_detects_values_schema_and_alias_order(self):
        self.case.check_projection(self.mixed, self.ordinary, self.hll)
        h = self.mixed.schema.field("first_h")
        for bad in (
            self.mixed.set_column(0, h.remove_metadata(), self.mixed.column(0)),
            self.mixed.set_column(0, h, pa.array([None, "swapped"], type=h.type)),
            self.mixed.rename_columns(list(reversed(self.mixed.column_names))),
            self.mixed.set_column(
                self.mixed.schema.get_field_index("i64"),
                "i64",
                pa.array([9007199254740992, None]),
            ),
        ):
            with self.assertRaises(AssertionError):
                self.case.check_projection(bad, self.ordinary, self.hll)

    def test_same_cardinality_wrong_member_fails_rebound_union(self):
        # Both projection paths deliver the same substituted singleton. The
        # ordinary comparison succeeds, but its union with the stored one is two.
        self.case.check_projection(self.mixed, self.ordinary, self.hll)
        result = pa.table({"rebound": [1], "combined": [2]})
        reader = result.to_reader()
        self.case.connection = object()
        with patch.object(vendor.adm, "AdbcStatement") as factory:
            stmt = factory.return_value.__enter__.return_value
            handle = vendor.adm.ArrowArrayStreamHandle()
            reader._export_to_c(handle.address)
            stmt.execute_query.return_value = (handle, 1)
            with self.assertRaises(AssertionError):
                self.case.check_mixed_sketch_member('"fixture"', self.mixed)
            batch = stmt.bind.call_args.args[0]
            self.assertEqual(batch.column(0).to_pylist(), ["a stored sketch"])
            self.assertEqual(
                batch.schema.field(0).metadata,
                vendor.encoded_field("HLLSKETCH").metadata,
            )
            sql = stmt.set_sql_query.call_args.args[0]
            self.assertIn("HLL_COMBINE(value)", sql)
            self.assertIn('FROM "fixture" WHERE id = 1', sql)
            self.assertIn("UNION ALL SELECT $1::HLLSKETCH", sql)

    def test_history_observation_never_replays_volatile_statement(self):
        result = pa.table({"sample": [0.3], "h": ["sketch"]})
        with patch.object(self.case, "query_table", return_value=result) as execute:
            with patch.object(self.case, "query", side_effect=[[{"n": 0}], [{"n": 1}]]):
                with patch.object(vendor.time, "sleep"):
                    self.case.check_tagged_execution('"fixture"')
            execute.assert_called_once()
        with patch.object(self.case, "query_table", return_value=result) as execute:
            with patch.object(self.case, "query", return_value=[{"n": 2}]):
                with self.assertRaises(AssertionError):
                    self.case.check_tagged_execution('"fixture"')
            execute.assert_called_once()

    def test_private_pipe_and_redacted_failure(self):
        read, write = os.pipe()
        with os.fdopen(write, "wb") as channel:
            channel.write(b"synthetic-private-value\n")
        with os.fdopen(read, "rb") as channel:
            self.assertEqual(
                vendor.read_uri(channel.fileno()), "synthetic-private-value"
            )

        class Failure(unittest.TestCase):
            def runTest(self):
                raise RuntimeError("synthetic-private-value")

        stream = io.StringIO()
        result = unittest.TextTestRunner(
            stream=stream, resultclass=vendor.SafeResult
        ).run(Failure())
        self.assertFalse(result.wasSuccessful())
        self.assertIn("RuntimeError", stream.getvalue())
        self.assertNotIn("synthetic-private-value", stream.getvalue())


if __name__ == "__main__":
    unittest.main()
