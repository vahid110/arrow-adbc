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

"""Offline tests execute the public boundary helper with a recording ADBC API."""

import re
import unittest
from unittest.mock import patch

import pgwire_prepared_boundary_cases as boundary
import pyarrow as pa

import adbc_driver_manager as adm

SCHEMA = "adbc_types_" + "0" * 32
SECRET = "native diagnostic must not appear in public exceptions"
DEFAULT = object()


def local_error(family):
    return adm.ProgrammingError(
        "INVALID_ARGUMENT: " + boundary.LOCAL_MESSAGES[family],
        status_code=adm.AdbcStatusCode.INVALID_ARGUMENT,
    )


def native_error(status=5, state="42804"):
    return adm.ProgrammingError(SECRET, status_code=status, sqlstate=state)


class FakeSession:
    schema = SCHEMA
    database = "owned_database"
    username = "owned_user"

    def __init__(self):
        self.connection = self
        self.operations = 0
        self.events = []
        self.effect_rows = {i: [] for i in range(3)}
        self.override = {}
        self.cleanup_errors = {}
        self.batch_shapes = []
        self.failed_at = None
        self.last_family = 0
        self.mutate_read = None

    def charge(self):
        self.operations += 1
        if self.operations > 31:
            raise RuntimeError("operation cap")

    def execute(self, sql):
        self.charge()
        self.events.append(("ddl", sql))
        self.last_family = int(re.search(r"vendor_boundary_[01]_([012])", sql)[1])
        family = self.last_family
        expected_type = ("SUPER", "GEOMETRY", "GEOGRAPHY")[family]
        incompatible = "INTEGER" if family else "GEOMETRY"
        self_case = (
            f'("row_id" INTEGER,"value" {expected_type},"incompatible" {incompatible})'
        )
        assert sql.endswith(self_case)

    def query(self, sql):
        self.charge()
        self.events.append(("query", sql))
        if sql == "SELECT current_database(),current_user":
            return pa.table({"db": [self.database], "user": [self.username]})
        family = int(re.search(r"vendor_boundary_[01]_([012])", sql)[1])
        assert '"row_id","value","incompatible"' in sql
        assert self.effect_rows[family] == [0, 1]
        result = boundary.expected_table(family, sql.endswith("WHERE false"))
        return self.mutate_read(result) if self.mutate_read else result

    def fail(self, key, default=None):
        result = self.override.get(key, DEFAULT)
        error = default if result is DEFAULT else result
        if error is not None:
            self.failed_at = len(self.events)
            raise error


class RecordingStatement:
    def __init__(self, connection):
        self.session = connection
        self.family = connection.last_family
        self.control = 0
        self.sql = ""
        self.closed = False
        self.session.events.append(("new",))

    def set_sql_query(self, sql):
        self.sql = sql
        self.family = int(re.search(r"vendor_boundary_[01]_([012])", sql)[1])
        self.control = 2 if '"incompatible"' in sql else 0
        self.session.events.append(("set-sql", self.family))
        self.session.fail(("set-sql", self.family, self.control))

    def prepare(self):
        self.session.events.append(("prepare", self.family))
        self.session.fail(("prepare", self.family, self.control))

    def bind(self, data):
        self._bind(data, None)

    def bind_stream(self, reader):
        batches = list(reader)
        self._bind(
            pa.Table.from_batches(batches).combine_chunks().to_batches()[0],
            [len(batch) for batch in batches],
        )

    def _bind(self, data, shape):
        ids = data.column(0).to_pylist()
        if ids == [100, 101] and self.control == 0:
            self.control = 1
        assert ids == ([0, 1] if self.control == 0 else [100, 101])
        expected_values = [
            None,
            bytes.fromhex("0101000020e6100000000000000000f03f000000000000f03f")
            if self.family
            else "true",
        ]
        assert data.column(1).to_pylist() == expected_values
        assert data.schema.names == ["row_id", "value"]
        assert data.schema.field(0).type == pa.int32()
        assert data.schema.field(1).type == (
            pa.large_binary() if self.family else pa.large_string()
        )
        if shape is not None:
            assert shape == [0, 1, 1]
            self.session.batch_shapes.append(shape)
        self.session.events.append(("bind", self.family, self.control))
        self.session.fail(
            ("bind", self.family, self.control),
            local_error(self.family) if self.control == 1 else None,
        )

    def execute_update(self):
        self.session.events.append(("execute", self.family, self.control))
        suffix = (
            ("::INTEGER" if self.family else "::GEOMETRY") if self.control == 2 else ""
        )
        assert self.sql.endswith(f"VALUES ($1,$2{suffix})")
        self.session.fail(
            ("execute", self.family, self.control),
            native_error() if self.control == 2 else None,
        )
        self.session.effect_rows[self.family].extend(
            [100, 101] if self.control else [0, 1]
        )
        return 2

    def close(self):
        assert not self.closed
        self.closed = True
        self.session.events.append(("close", self.family, self.control))
        error = self.session.cleanup_errors.get((self.family, self.control))
        if error:
            raise error


class PreparedBoundaryTest(unittest.TestCase):
    def setUp(self):
        self.patch = patch.object(boundary.adm, "AdbcStatement", RecordingStatement)
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def assert_stopped(self, session):
        later = session.events[session.failed_at :]
        self.assertTrue(all(event[0] == "close" for event in later), later)

    def test_full_runs_have_exact_retention_sequence_and_budget(self):
        for stream in (False, True):
            session = FakeSession()
            receipts = boundary.run(session, stream)
            self.assertEqual(session.operations, 31)
            self.assertEqual(len(receipts), 6)
            self.assertEqual([facts.phase for _, _, facts in receipts], [55, 56] * 3)
            self.assertEqual(
                [facts.sqlstate for _, _, facts in receipts], [None, "42804"] * 3
            )
            self.assertEqual(session.effect_rows, {i: [0, 1] for i in range(3)})
            self.assertEqual(len(session.batch_shapes), 9 if stream else 0)
            for index, event in enumerate(session.events):
                if event[0] == "close" and event[2] in (1, 2):
                    following = session.events[index + 1 : index + 3]
                    self.assertEqual(
                        following[0],
                        ("query", "SELECT current_database(),current_user"),
                    )
                    self.assertEqual(following[1][0], "query")
                    self.assertIn('ORDER BY "row_id"', following[1][1])

    def test_source_and_output_contracts_are_separate(self):
        for family in range(3):
            source = boundary.source_batch(family, 0)
            malformed = boundary.source_batch(family, 1)
            expected = boundary.expected_table(family)
            self.assertEqual(source.column(1).null_count, 1)
            self.assertNotEqual(
                source.schema.field(1).metadata, expected.schema.field(1).metadata
            )
            self.assertNotEqual(
                source.schema.field(1).metadata, malformed.schema.field(1).metadata
            )
            self.assertEqual(expected.column(2).to_pylist(), [None, None])
            if family == 0:
                self.assertEqual(
                    expected.schema.field(1).metadata[b"ARROW:extension:name"],
                    b"adbc.redshift.super.native_json",
                )
                self.assertEqual(
                    boundary.expected_table(family, True)
                    .schema.field(1)
                    .metadata[b"ADBC:redshift:type_modifier"],
                    b"-1",
                )
            else:
                self.assertNotIn(
                    b"ADBC:redshift:type_modifier", expected.schema.field(1).metadata
                )

    def test_native_allowed_pairs_preserve_actual_state(self):
        for stream in (False, True):
            for status, state in ((5, "42804"), (5, "42846"), (3, "42883")):
                session = FakeSession()
                session.override[("execute", 0, 2)] = native_error(status, state)
                receipts = boundary.run(session, stream)
                self.assertEqual(
                    (receipts[1][2].status, receipts[1][2].sqlstate), (status, state)
                )

    def test_wrong_native_status_state_or_phase_stops(self):
        for stream in (False, True):
            for status, state in (
                (5, "42601"),
                (5, None),
                (3, "42804"),
                (5, "42883"),
                (10, "42804"),
                (5, "08006"),
                (5, "25P02"),
                (1, "XX000"),
            ):
                session = FakeSession()
                original = native_error(status, state)
                session.override[("execute", 0, 2)] = original
                with self.assertRaises(boundary.UnsafeBoundary) as caught:
                    boundary.run(session, stream)
                self.assertIs(caught.exception.failure.original, original)
                self.assertEqual(caught.exception.facts.phase, 56)
                self.assert_stopped(session)
            session = FakeSession()
            session.override[("prepare", 0, 2)] = native_error()
            with self.assertRaises(boundary.UnsafeBoundary) as caught:
                boundary.run(session, stream)
            self.assertEqual(caught.exception.facts.phase, 53)
            self.assert_stopped(session)

    def test_only_exact_local_message_type_and_details_match(self):
        bad = [
            adm.ProgrammingError("unrelated", status_code=5),
            adm.Error("INVALID_ARGUMENT: " + boundary.LOCAL_MESSAGES[0], status_code=5),
            adm.ProgrammingError(
                "INVALID_ARGUMENT: " + boundary.LOCAL_MESSAGES[0],
                status_code=5,
                sqlstate="22000",
            ),
            adm.ProgrammingError(
                "INVALID_ARGUMENT: " + boundary.LOCAL_MESSAGES[0], status_code=10
            ),
            adm.ProgrammingError(
                "INVALID_ARGUMENT: " + boundary.LOCAL_MESSAGES[0],
                status_code=5,
                vendor_code=7,
            ),
            adm.ProgrammingError(
                "INVALID_ARGUMENT: " + boundary.LOCAL_MESSAGES[0],
                status_code=5,
                details=[("other", b"detail")],
            ),
        ]
        for stream in (False, True):
            for original in bad:
                session = FakeSession()
                session.override[("bind", 0, 1)] = original
                with self.assertRaises(boundary.UnsafeBoundary) as caught:
                    boundary.run(session, stream)
                self.assertIs(caught.exception.failure.original, original)
                self.assert_stopped(session)

    def test_unexpected_success_is_unresolved_and_stops(self):
        for control in (1, 2):
            session = FakeSession()
            key = ("bind" if control == 1 else "execute", 0, control)
            session.override[key] = None
            with self.assertRaises(boundary.UnsafeBoundary) as caught:
                boundary.run(session, True)
            self.assertEqual(caught.exception.reason, "unexpected-success")
            self.assertIsNone(caught.exception.failure.original)
            close_index = session.events.index(("close", 0, control))
            self.assertEqual(close_index, len(session.events) - 1)
            if control == 1:
                self.assertNotIn(("execute", 0, 1), session.events)
            else:
                self.assertEqual(session.effect_rows[0], [0, 1, 100, 101])

    def test_cleanup_does_not_override_original_or_allow_retention_sql(self):
        for control in (0, 1, 2):
            session = FakeSession()
            close_error = native_error(10, "08006")
            session.cleanup_errors[(0, control)] = close_error
            original = None
            if control:
                original = local_error(0) if control == 1 else native_error()
                session.override[
                    ("bind" if control == 1 else "execute", 0, control)
                ] = original
            with self.assertRaises(boundary.UnsafeBoundary) as caught:
                boundary.run(session, True)
            failure = caught.exception.failure
            self.assertIs(failure.original, original)
            self.assertIs(failure.cleanup, close_error)
            self.assertEqual(
                failure.cleanup_facts, boundary.ErrorFacts(59, 10, "08006")
            )
            self.assertEqual(session.events[-1], ("close", 0, control))
            self.assertNotIn(SECRET, str(caught.exception))
            self.assertNotIn(SECRET, repr(caught.exception))

    def test_changed_retained_rows_or_schema_stop_before_next_control(self):
        mutations = (
            lambda t: t.slice(0, 1),
            lambda t: t.rename_columns(["wrong", "value", "incompatible"]),
            lambda t: t.set_column(
                0, t.schema.field(0), pa.array([0, 101], pa.int32())
            ),
            lambda t: t.set_column(
                2, t.schema.field(2), pa.array([None, b"changed"], pa.large_binary())
            ),
        )
        for mutate in mutations:
            session = FakeSession()
            session.mutate_read = mutate
            with self.assertRaises(boundary.UnsafeBoundary) as caught:
                boundary.run(session, False)
            self.assertEqual(caught.exception.facts.phase, 62)
            self.assertIsInstance(
                caught.exception.failure.original, boundary.BoundaryMismatch
            )
            self.assertEqual(session.operations, 4)
            self.assertNotIn(("bind", 0, 1), session.events)

    def test_retention_failure_after_accepted_control_stops_before_next_sql(self):
        session = FakeSession()
        reads = 0

        def mutate_second(table):
            nonlocal reads
            reads += 1
            if reads == 2:
                return table.set_column(
                    0, table.schema.field(0), pa.array([0, 100], pa.int32())
                )
            return table

        session.mutate_read = mutate_second
        with self.assertRaises(boundary.UnsafeBoundary) as caught:
            boundary.run(session, True)
        self.assertEqual(
            (caught.exception.control, caught.exception.facts.phase), (1, 62)
        )
        self.assertNotIn(("bind", 0, 2), session.events)
        self.assertEqual(session.events[-1][0], "query")

    def test_identity_failure_after_rejection_stops_before_retention_query(self):
        session = FakeSession()
        original_query = session.query
        identities = 0

        def query(sql):
            nonlocal identities
            result = original_query(sql)
            if sql == "SELECT current_database(),current_user":
                identities += 1
                if identities == 2:
                    return pa.table({"db": ["different"], "user": [session.username]})
            return result

        session.query = query
        with self.assertRaises(boundary.UnsafeBoundary) as caught:
            boundary.run(session, False)
        self.assertEqual(caught.exception.facts.phase, 60)
        self.assertEqual(caught.exception.control, 1)
        self.assertEqual(
            session.events[-1], ("query", "SELECT current_database(),current_user")
        )
        self.assertNotIn(("bind", 0, 2), session.events)

    def test_charge_and_statement_setup_failures_have_no_execution(self):
        session = FakeSession()
        failure = RuntimeError(SECRET)
        with patch.object(session, "charge", side_effect=failure):
            with self.assertRaises(boundary.UnsafeBoundary) as caught:
                boundary.prepared(session, False, 0, 0)
        self.assertEqual(caught.exception.facts.phase, 50)
        self.assertIs(caught.exception.failure.original, failure)
        self.assertEqual(session.events, [])
        self.assertNotIn(SECRET, str(caught.exception))
        session = FakeSession()
        session.override[("set-sql", 0, 0)] = native_error(10, "08006")
        with self.assertRaises(boundary.UnsafeBoundary) as caught:
            boundary.run(session, False)
        self.assertEqual(caught.exception.facts.phase, 52)
        self.assert_stopped(session)

    def test_invalid_ownership_or_route_never_touches_connection(self):
        for schema, stream in (("public", False), (SCHEMA + '"', False), (SCHEMA, 1)):
            session = FakeSession()
            session.schema = schema
            with self.assertRaises(ValueError):
                boundary.run(session, stream)
            self.assertEqual(session.events, [])
            self.assertEqual(session.operations, 0)


if __name__ == "__main__":
    unittest.main()
