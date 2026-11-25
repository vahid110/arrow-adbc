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

"""Offline fault tests execute the public bounded helper with real Arrow data."""

import math
import struct
import unittest
from unittest.mock import patch

import pgwire_scalar_boundary_cases as cases
import pyarrow as pa

import adbc_driver_manager as adm


def rejection(index):
    reason = (
        "signed integer exceeds target"
        if index < 4
        else "unsigned integer exceeds target"
        if index < 6
        else "finite float32 overflow"
        if index == 7
        else "inexact float32 narrowing"
    )
    return adm.ProgrammingError(
        "INVALID_ARGUMENT: [Redshift] Invalid scalar write: " + reason,
        status_code=adm.AdbcStatusCode.INVALID_ARGUMENT,
    )


class Session:
    schema = "adbc_types_" + "1" * 32
    database, username = "owned", "runtime"

    def __init__(self):
        self.connection = self
        self.operations = []
        self.writes = []
        self.closed = 0
        self.events = []
        self.fault_index = None
        self.fault_phase = 56
        self.fault = None
        self.close_fault = None
        self.query_fault = None
        self.mutate = None
        self.rejection_index = 0
        self.appended = False

    def charge(self):
        self.operations.append("write")

    def execute(self, sql):
        self.operations.append("ddl")
        self.events.append("ddl")
        assert sql.startswith('CREATE TABLE "adbc_types_')

    def query(self, sql):
        self.operations.append("query")
        self.events.append("query")
        if self.query_fault == len(self.operations):
            raise RuntimeError("query failed")
        if sql.startswith("SELECT current_database"):
            return pa.table(
                {"db": [self.database], "usr": [self.username], "sch": [self.schema]}
            )
        expected = cases.expected_table(
            append=self.appended and "scalar_ingest" in sql,
            empty=sql.endswith("WHERE false"),
        )
        return self.mutate(expected) if self.mutate else expected


class Statement:
    def __init__(self, connection):
        self.session = connection
        self.route = "direct"
        self.index = len(connection.writes)
        self._fault(51)

    def _fault(self, phase):
        s = self.session
        if s.fault_index == self.index and s.fault_phase == phase:
            raise s.fault

    def set_sql_query(self, sql):
        self._fault(52)
        assert "VALUES ($1,$2,$3,$4,$5,$6,$7,$8)" in sql

    def set_options(self, **options):
        self._fault(52)
        assert options["adbc.ingest.target_db_schema"] == self.session.schema
        # Independently enforce the public ADBC option protocol. Raw convenience
        # names belong to dbapi.adbc_ingest, not AdbcStatement.set_options.
        modes = {
            "adbc.ingest.mode.create": "create",
            "adbc.ingest.mode.append": "append",
        }
        mode = options["adbc.ingest.mode"]
        if mode not in modes:
            raise adm.ProgrammingError(
                "INVALID_ARGUMENT: invalid ingest mode",
                status_code=adm.AdbcStatusCode.INVALID_ARGUMENT,
            )
        self.route = modes[mode]

    def prepare(self):
        self._fault(53)

    def bind(self, batch):
        self._fault(55)
        self.batches = [batch]

    def bind_stream(self, reader):
        self._fault(55)
        self.batches = list(reader)

    def execute_update(self):
        s = self.session
        s.events.append("execute")
        s.writes.append((self.route, self.batches))
        self._fault(56)
        if self.index >= 3:
            index = s.rejection_index
            s.rejection_index += 1
            if s.fault_index == self.index and s.fault_phase == 0:
                return 1  # Unexpected success must stop, never inspect/retry SQL.
            raise rejection(index)
        if self.route == "append":
            s.appended = True
        return 12

    def close(self):
        self.session.closed += 1
        self.session.events.append("close")
        if self.session.close_fault == self.index:
            raise RuntimeError("close failed")


class ScalarBoundaryTest(unittest.TestCase):
    def run_case(self, session, stream=False):
        with patch.object(cases.adm, "AdbcStatement", Statement):
            return cases.run(session, stream)

    def test_array_stream_all_routes_exact_budget_and_retention(self):
        for stream in (False, True):
            session = Session()
            self.assertTrue(self.run_case(session, stream))
            self.assertEqual(len(session.operations), 38)
            self.assertEqual(session.closed, 12)
            self.assertEqual(
                session.scalar_receipt, {"positive_routes": 3, "rejections": 9}
            )
            self.assertEqual(
                [x[0] for x in session.writes], ["direct", "create"] + ["append"] * 10
            )
            for index, (_, batches) in enumerate(session.writes):
                self.assertEqual(
                    [b.num_rows for b in batches],
                    ([0, 1, 11] if index < 3 else [0, 1])
                    if stream
                    else [12 if index < 3 else 1],
                )
                if index >= 3:
                    self.assertIsNone(batches[-1].schema.field(0).metadata)
            self.assertEqual(session.events[-1], "query")

    def test_raw_ingest_modes_reproduce_set_option_failure_before_any_bind(self):
        class RawModeStatement(Statement):
            def set_options(self, **options):
                options["adbc.ingest.mode"] = options["adbc.ingest.mode"].rsplit(
                    ".", 1
                )[-1]
                return super().set_options(**options)

        # Execute the actual helper; replace only the option value at its API
        # boundary. Both CREATE and APPEND counterexamples fail before bind/SQL.
        for route in ("create", "append"):
            for stream in (False, True):
                session = Session()
                with patch.object(cases.adm, "AdbcStatement", RawModeStatement):
                    with self.assertRaises(cases.UnsafeBoundary) as caught:
                        cases._write(
                            session,
                            stream,
                            route,
                            "scalar_ingest_0",
                            cases.source_batch(route),
                        )
                self.assertEqual(caught.exception.facts, cases.Facts(52, 5, None))
                self.assertEqual(caught.exception.operation, route)
                self.assertEqual(session.closed, 1)
                self.assertEqual(session.writes, [])
                self.assertEqual(session.operations, ["write"])
                self.assertEqual(session.events, ["close"])

        # The full public group must stop at its first CREATE option failure,
        # retain the already completed direct route and issue no follow-up SQL.
        session = Session()
        with patch.object(cases.adm, "AdbcStatement", RawModeStatement):
            with self.assertRaises(cases.UnsafeBoundary) as caught:
                cases.run(session, False)
        self.assertEqual(caught.exception.operation, "create")
        self.assertEqual(caught.exception.facts, cases.Facts(52, 5, None))
        self.assertEqual(len(session.writes), 1)
        self.assertEqual(len(session.operations), 6)
        self.assertEqual(session.events[-1], "close")
        self.assertFalse(hasattr(session, "scalar_receipt"))

    def test_source_literal_bits_null_and_unsigned_not_wrapped(self):
        for route in ("direct", "create", "append"):
            batch = cases.source_batch(route)
            self.assertEqual(batch.num_rows, 12)
            self.assertEqual(batch.num_columns, 8)
            self.assertEqual(batch.column(4).to_pylist()[2], 2**63 - 1)
            self.assertEqual(batch.schema.field(4).type, pa.uint64())
            self.assertEqual(
                batch.schema.field(7).metadata[b"ADBC:redshift:sql_type"],
                b"DOUBLE PRECISION" if route == "direct" else b"REAL",
            )
            self.assertEqual(
                batch.column(0).to_pylist()[0], 12 if route == "append" else 0
            )
            for column, width, bits in (
                (5, 32, cases.F32),
                (6, 64, cases.F64),
                (7, 64, cases.NARROW),
            ):
                values = batch.column(column).to_pylist()
                self.assertIsNone(values[0])
                for value, bit in zip(values[1:], bits):
                    if math.isnan(value):
                        continue
                    self.assertEqual(
                        struct.pack(">f" if width == 32 else ">d", value),
                        bit.to_bytes(width // 8, "big"),
                    )
        self.assertEqual(cases.negative_batch(4).column(0).to_pylist(), [2**63])
        self.assertEqual(cases.negative_batch(5).column(0).to_pylist(), [2**64 - 1])
        self.assertEqual(
            cases.negative_batch(8).column(0).to_pylist(),
            [float.fromhex("0x0.0000000000001p-1022")],
        )

    def test_oracle_nan_numeric_but_signedzero_and_subnormal_exact(self):
        expected = cases.expected_table()
        cases.compare(expected, expected)
        for column, row, replacement in (
            (5, 6, 0.0),
            (6, 3, 0.0),
            (4, 2, 2**63 - 2),
            (7, 10, 1.2500001192092896),
        ):
            arrays = list(expected.columns)
            values = arrays[column].to_pylist()
            values[row] = replacement
            arrays[column] = pa.array(values, type=expected.schema.field(column).type)
            wrong = pa.Table.from_arrays(arrays, schema=expected.schema)
            with self.assertRaisesRegex(ValueError, "value mismatch"):
                cases.compare(wrong, expected)
        arrays = list(expected.columns)
        values = arrays[5].to_pylist()
        values[7] = struct.unpack(">f", bytes.fromhex("7fc12345"))[0]
        arrays[5] = pa.array(values, pa.float32())
        cases.compare(pa.Table.from_arrays(arrays, schema=expected.schema), expected)
        with self.assertRaisesRegex(ValueError, "schema mismatch"):
            cases.compare(
                expected.replace_schema_metadata({b"wrong": b"yes"}), expected
            )
        with self.assertRaisesRegex(ValueError, "row count"):
            cases.compare(expected.slice(0, 11), expected)

    def test_all_wrong_rejection_categories_stop_before_identity_or_read(self):
        errors = [
            adm.OperationalError("IO", status_code=adm.AdbcStatusCode.IO),
            adm.InternalError("internal", status_code=adm.AdbcStatusCode.INTERNAL),
            adm.ProgrammingError(str(rejection(0)), status_code=5, sqlstate="22003"),
            adm.ProgrammingError("INVALID_ARGUMENT: wrong category", status_code=5),
            RuntimeError("unknown"),
        ]
        for stream in (False, True):
            for error in errors:
                session = Session()
                session.fault_index, session.fault = 3, error
                with self.assertRaises(cases.UnsafeBoundary) as caught:
                    self.run_case(session, stream)
                self.assertIs(caught.exception.original, error)
                self.assertEqual(session.events[-2:], ["execute", "close"])
                self.assertEqual(len(session.operations), 12)
                self.assertEqual(len(session.writes), 4)
                self.assertFalse(hasattr(session, "scalar_receipt"))

    def test_unexpected_success_and_wrong_phase_are_unsafe(self):
        for phase in (0, 51, 52, 55):
            session = Session()
            session.fault_index, session.fault_phase = 3, phase
            session.fault = rejection(0)
            with self.assertRaises(cases.UnsafeBoundary):
                self.run_case(session)
            self.assertEqual(len(session.operations), 12)
            self.assertFalse(hasattr(session, "scalar_receipt"))

    def test_cleanup_failure_keeps_original_and_stops_later_sql(self):
        for index in (0, 3):
            session = Session()
            session.close_fault = index
            with self.assertRaises(cases.UnsafeBoundary) as caught:
                self.run_case(session)
            self.assertIsInstance(caught.exception.cleanup, RuntimeError)
            self.assertEqual(caught.exception.cleanup_facts.phase, 59)
            if index == 3:
                self.assertEqual(str(caught.exception.original), str(rejection(0)))
            self.assertEqual(session.events[-1], "close")
            self.assertFalse(hasattr(session, "scalar_receipt"))

    def test_read_identity_and_schema_failures_stop_immediately(self):
        for fault in (1, 4, 5, 13, 14):
            session = Session()
            session.query_fault = fault
            with self.assertRaises(cases.UnsafeBoundary):
                self.run_case(session)
            self.assertEqual(len(session.operations), fault)
        session = Session()
        session.mutate = lambda table: table.replace_schema_metadata({b"bad": b"yes"})
        with self.assertRaises(cases.UnsafeBoundary):
            self.run_case(session)
        self.assertEqual(len(session.writes), 1)

    def test_owned_schema_and_selection_are_closed_before_sql(self):
        for schema in ("public", "adbc_types_" + "x" * 32, 'bad";DROP SCHEMA public'):
            session = Session()
            session.schema = schema
            with self.assertRaises(ValueError):
                self.run_case(session)
            self.assertEqual(session.operations, [])
        with self.assertRaises(ValueError):
            cases.negative_batch(-1)
        with self.assertRaises(ValueError):
            cases.source_batch("replace")


if __name__ == "__main__":
    unittest.main()
