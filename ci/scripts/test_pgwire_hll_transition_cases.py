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

"""Offline real-helper regressions; no server, credentials or connection setup."""

import base64
import dataclasses
import json
import re
import unittest
from unittest.mock import patch

import pgwire_hll_transition_cases as hll
import pyarrow as pa

import adbc_driver_manager as adm

SCHEMA = "adbc_types_" + "a" * 32
THRESHOLD = 1234


def sketch(count, combined=False):
    if count < THRESHOLD:
        return json.dumps(
            {"version": 1, "logm": 15, "sparse": {"indices": [count], "values": [1]}}
        )
    return (
        '"'
        + base64.b64encode(
            f"{'union' if combined else 'prefix'}:{count}".encode()
        ).decode()
        + '"'
    )


def estimate(count):
    # Deliberately not exact count; callers must compare independent native
    # witnesses, not assume approximate cardinality equals number of source rows.
    return count if count < 4 else count - 1


def result_schema():
    integer = {
        b"POSTGRESQL:type": b"int4",
        b"ADBC:redshift:sql_type": b"INTEGER",
        b"ADBC:redshift:type_modifier": b"-1",
    }
    vendor = {
        b"POSTGRESQL:type": b"hllsketch",
        b"ADBC:redshift:sql_type": b"HLLSKETCH",
        b"ARROW:extension:name": b"adbc.redshift.hllsketch",
        b"ARROW:extension:metadata": (
            b'{"version":1,"encoding":"redshift-serialized","logm":15}'
        ),
    }
    return pa.schema(
        [
            pa.field("row_id", pa.int32(), metadata=integer),
            pa.field("input_count", pa.int32(), metadata=integer),
            pa.field("value", pa.large_string(), metadata=vendor),
        ]
    )


def scalar_table(rows):
    return pa.Table.from_arrays(
        [
            pa.array([r[0] for r in rows], pa.int32()),
            pa.array([r[1] for r in rows], pa.int32()),
            pa.array([r[2] for r in rows], pa.large_string()),
            pa.array([r[3] for r in rows], pa.int64()),
        ],
        schema=pa.schema(
            [
                pa.field("row_id", pa.int32()),
                pa.field("input_count", pa.int32()),
                result_schema().field("value").with_name("serialized"),
                pa.field("cardinality", pa.int64()),
            ]
        ),
    )


class Session:
    schema, database, username = SCHEMA, "owned_database", "owned_user"

    def __init__(self, fail_at=None, corrupt=None):
        self.connection = self
        self.operations = []
        self.tables = {}
        self.fail_at, self.corrupt = fail_at, corrupt
        self.statements, self.stream_shapes = [], []
        self.poisoned = False
        self.failure = adm.OperationalError(
            "private diagnostic", status_code=10, sqlstate="08006"
        )

    def charge(self):
        if self.poisoned:
            raise AssertionError("operation after unsafe failure")
        self.operations.append("prepared")
        if len(self.operations) == self.fail_at:
            self.poisoned = True
            raise self.failure

    def call(self, sql):
        self.charge()
        self.operations[-1] = sql

    def execute(self, sql):
        self.call(sql)
        table = re.search(r'"hll_[a-z0-9_]+"', sql).group()[1:-1]
        if sql.startswith("CREATE TABLE"):
            if table in self.tables:
                raise AssertionError("duplicate table")
            self.tables[table] = []
        elif table == "hll_keys":
            self.assert_keys(sql)
        elif "HLL_COMBINE" in sql:
            n = self.tables[table][1][1]
            self.tables[table] += [
                (4, n, sketch(n, combined=True), estimate(n)),
                (5, None, None, None),
            ]
        else:
            matches = re.findall(
                r"SELECT ([0-3]),(\d+),HLL_CREATE_SKETCH\(n\).*?WHERE n([<=])(\d+)", sql
            )
            if len(matches) != 4:
                raise AssertionError("fixed witness branches")
            for index, count, relation, limit in matches:
                count = int(count)
                if relation == "=" and (
                    index != "3" or count != 1 or int(limit) != int(matches[1][1]) - 1
                ):
                    raise AssertionError("singleton identity")
                self.tables[table].append(
                    (int(index), count, sketch(count), estimate(count))
                )

    @staticmethod
    def assert_keys(sql):
        if sql.count(" CROSS JOIN ") != 3 or not sql.endswith(
            "a.d+10*b.d+100*c.d+1000*d.d<4097"
        ):
            raise AssertionError("finite exact producer")
        for digit in range(10):
            if sql.count(f"SELECT {digit} AS d") != 4:
                raise AssertionError("retained decimal digits")

    def query(self, sql):
        self.call(sql)
        # Recorded live refusal: native HLL -> VARCHAR CAST is unsupported.
        # Running the actual helper against this fake catches accidental reuse.
        if "CAST(value AS VARCHAR" in sql:
            raise adm.ProgrammingError(
                "fixed unsupported HLL cast", status_code=5, sqlstate="42846"
            )
        if sql == "SELECT current_database(),current_user":
            return pa.table(
                {
                    "database": [
                        self.database if self.corrupt != "identity" else "wrong"
                    ],
                    "user": [self.username],
                }
            )
        if "FROM (SELECT HLL_CREATE_SKETCH" in sql:
            n = int(re.search(r"WHERE n<(\d+)", sql)[1])
            value = "unknown" if self.corrupt == "probe" else sketch(n)
            table = scalar_table([(0, n, value, estimate(n))])
            if self.corrupt == "witness-metadata":
                table = table.set_column(2, "serialized", table.column(2))
            return table
        table = re.search(r'"hll_[a-z0-9_]+"', sql).group()[1:-1]
        rows = self.tables[table]
        if "AS serialized" in sql:
            rows = list(rows)
            if self.corrupt == "union":
                r = rows[4]
                rows[4] = (*r[:3], r[3] + 1)
            if self.corrupt == "count":
                r = rows[1]
                rows[1] = (r[0], r[1] + 1, *r[2:])
            return scalar_table(rows)
        empty = "WHERE 1=0" in sql
        schema = result_schema()
        columns = [[], [], []] if empty else [[r[i] for r in rows] for i in range(3)]
        if self.corrupt == "native-bytes" and not empty:
            columns[2][0] = sketch(1)
        if self.corrupt == "native-schema" or self.corrupt == "empty-schema" and empty:
            schema = schema.remove_metadata().set(
                2, pa.field("value", pa.large_string())
            )
        return pa.Table.from_arrays(
            [pa.array(c, f.type) for c, f in zip(columns, schema)], schema=schema
        )


class Statement:
    fail_stage = None
    fail_close = False

    def __init__(self, session):
        self.session = session
        session.statements.append(self)
        self.closed = False
        self.stage("new")

    def stage(self, stage):
        if self.fail_stage == stage:
            raise self.session.failure

    def set_sql_query(self, sql):
        self.stage("set")
        self.table = re.search(r'"hll_rebound_[01]"', sql).group()[1:-1]
        if not sql.endswith("(row_id,input_count,value) VALUES ($1,$2,$3)"):
            raise AssertionError("fixed prepared route")

    def prepare(self):
        self.stage("prepare")

    def bind(self, batch):
        self.stage("bind")
        self.batch = batch
        self.session.stream_shapes.append([batch.num_rows])

    def bind_stream(self, reader):
        self.stage("bind")
        batches = list(reader)
        self.session.stream_shapes.append([b.num_rows for b in batches])
        self.batch = pa.Table.from_batches(batches).combine_chunks().to_batches()[0]

    def execute_update(self):
        self.stage("execute")
        if self.batch.schema.field("value").metadata != {
            b"ADBC:redshift:sql_type": b"HLLSKETCH",
            b"ARROW:extension:name": b"adbc.redshift.hllsketch",
            b"ARROW:extension:metadata": (
                b'{"version":1,"encoding":"redshift-serialized","logm":15}'
            ),
        }:
            raise AssertionError("independent write schema")
        rows = self.batch.to_pydict()
        for i, n, value in zip(rows["row_id"], rows["input_count"], rows["value"]):
            card = None if value is None else estimate(n)
            self.session.tables[self.table].append((i, n, value, card))
        return 6

    def close(self):
        self.closed = True
        if self.fail_close:
            raise adm.OperationalError(
                "private close diagnostic", status_code=10, sqlstate="08003"
            )


class HllTransitionTest(unittest.TestCase):
    def tearDown(self):
        Statement.fail_stage = None
        Statement.fail_close = False

    def complete(self, session):
        with patch.object(hll.adm, "AdbcStatement", Statement):
            fixture = hll.discover(session)
            hll.qualify(session, fixture, False)
            hll.qualify(session, fixture, True)
        return fixture

    def test_actual_helper_full_sequence_bound_and_bytes(self):
        session = Session()
        fixture = self.complete(session)
        self.assertEqual(fixture.crossing, THRESHOLD)
        self.assertLessEqual(len(fixture.observations), 14)
        self.assertLessEqual(len(session.operations), 35)
        self.assertEqual(session.stream_shapes, [[6], [0, 1, 5]])
        self.assertNotEqual(fixture.serialized[1], fixture.serialized[4])
        self.assertEqual(fixture.cardinalities[1], fixture.cardinalities[4])
        self.assertNotEqual(fixture.cardinalities[1], fixture.crossing)
        self.assertTrue(all(s.closed for s in session.statements))
        self.assertEqual(
            session.tables["hll_crossing"], session.tables["hll_rebound_0"]
        )
        self.assertEqual(
            session.tables["hll_crossing"], session.tables["hll_rebound_1"]
        )

    def test_every_operation_failure_stops_immediately(self):
        baseline = Session()
        self.complete(baseline)
        for failure_at in range(1, len(baseline.operations) + 1):
            with self.subTest(failure_at=failure_at):
                session = Session(fail_at=failure_at)
                with self.assertRaises(hll.UnsafeTransition) as caught:
                    self.complete(session)
                self.assertIs(caught.exception.original, session.failure)
                self.assertEqual(len(session.operations), failure_at)
                self.assertNotIn("private", str(caught.exception))

    def test_mutated_observations_stop_without_later_sql(self):
        for mutation, phase in [
            ("identity", "identity"),
            ("probe", "bracket"),
            ("witness-metadata", "bracket"),
            ("count", "scalar-witness"),
            ("union", "witness-check"),
            ("native-bytes", "native-read"),
            ("native-schema", "native-read"),
            ("empty-schema", "empty-read"),
        ]:
            with self.subTest(mutation=mutation):
                session = Session(corrupt=mutation)
                with self.assertRaises(hll.UnsafeTransition) as caught:
                    self.complete(session)
                self.assertEqual(caught.exception.phase, phase)
                self.assertNotIn("hll_rebound_0", session.tables)

    def test_prepared_errors_and_close_error_preserve_original(self):
        for stage in ("new", "set", "prepare", "bind", "execute"):
            for cleanup in (False, True):
                with self.subTest(stage=stage, cleanup=cleanup):
                    session = Session()
                    fixture = hll.discover(session)
                    count = len(session.operations)
                    Statement.fail_stage, Statement.fail_close = stage, cleanup
                    with (
                        patch.object(hll.adm, "AdbcStatement", Statement),
                        self.assertRaises(hll.UnsafeTransition) as caught,
                    ):
                        hll.qualify(session, fixture, True)
                    self.assertIs(caught.exception.original, session.failure)
                    self.assertEqual(caught.exception.sqlstate, "08006")
                    self.assertEqual(
                        caught.exception.cleanup_sqlstate,
                        "08003" if cleanup and stage != "new" else None,
                    )
                    self.assertEqual(len(session.operations), count + 3)
        Statement.fail_stage = None
        Statement.fail_close = True
        session = Session()
        fixture = hll.discover(session)
        with (
            patch.object(hll.adm, "AdbcStatement", Statement),
            self.assertRaises(hll.UnsafeTransition) as caught,
        ):
            hll.qualify(session, fixture, False)
        self.assertIsNone(caught.exception.original)
        self.assertEqual(caught.exception.cleanup_sqlstate, "08003")

    def test_search_all_positions_finite_without_monotonicity_claim(self):
        for threshold in (5, 6, 1234, 2048, 4095, 4096):
            calls = []

            def probe(n):
                calls.append(n)
                value = sketch(1) if n < threshold else '"AQ=="'
                return hll.Observation(n, value, n - 1, hll.representation(value))

            actual, history = hll.adjacent_crossing(probe)
            self.assertEqual(actual, threshold)
            self.assertLessEqual(len(calls), 14)
            self.assertEqual(len(set(calls)), len(calls))
            self.assertIn(actual - 1, calls)
            self.assertIn(actual, calls)
            self.assertEqual(len(history), len(calls))

        # A representation island does not invalidate the local pair proof and
        # the helper makes no claim that its selected crossing is the first.
        def island(n):
            dense = n == 4096 or 100 <= n <= 2000 or n >= 3000
            value = '"AQ=="' if dense else sketch(1)
            return hll.Observation(n, value, n, hll.representation(value))

        crossing, history = hll.adjacent_crossing(island)
        forms = {o.count: o.form for o in history}
        self.assertEqual((forms[crossing - 1], forms[crossing]), ("sparse", "dense"))

    def test_bad_endpoint_identity_unknown_and_cap_fail_closed(self):
        for mode in ("endpoint", "identity", "unknown", "cardinality"):
            calls = []

            def bad(n):
                calls.append(n)
                value = "bad" if mode == "unknown" else sketch(1)
                return hll.Observation(
                    n + 1 if mode == "identity" else n,
                    value,
                    -1 if mode == "cardinality" else n,
                    "sparse",
                )

            with self.assertRaises(hll.Mismatch):
                hll.adjacent_crossing(bad)
            self.assertLessEqual(len(calls), 2)
        with patch.object(hll, "MAX_MIDPOINTS", 0), self.assertRaises(hll.Mismatch):
            hll.adjacent_crossing(
                lambda n: hll.Observation(
                    n, sketch(n), n, hll.representation(sketch(n))
                )
            )

    def test_representation_rejects_unknown_preserves_opaque_bytes(self):
        sparse = ' {"version":1,"logm":15,"sparse":{"values":[1],"indices":[99]}} '
        self.assertEqual(hll.representation(sparse), "sparse")
        for bad in (
            "null",
            "[]",
            "{}",
            '"AB=="',
            '""',
            '"\\u0041Q=="',
            sparse.replace('1,"logm"', 'true,"logm"'),
            sparse.replace("[99]", "[99.0]"),
            sparse.replace("[99]", "[]"),
            sparse.replace('"version":1', '"version":1,"version":1'),
            "x" * 65536,
        ):
            with self.subTest(bad=bad[:30]), self.assertRaises(hll.Mismatch):
                hll.representation(bad)

    def test_native_hll_witness_requires_exact_type_and_identity(self):
        witness = scalar_table([(0, 4, sketch(4), estimate(4))])
        self.assertEqual(hll._scalar_rows(witness, [4]), ((sketch(4),), (estimate(4),)))
        variants = [
            pa.field("serialized", pa.string()),  # Refused legacy VARCHAR path.
            pa.field("serialized", pa.large_string()),  # Storage alone is not HLL.
            pa.field(
                "serialized", pa.string(), metadata=witness.schema.field(2).metadata
            ),
        ]
        for key in witness.schema.field(2).metadata:
            missing = dict(witness.schema.field(2).metadata)
            del missing[key]
            variants.append(pa.field("serialized", pa.large_string(), metadata=missing))
            wrong = dict(witness.schema.field(2).metadata)
            wrong[key] = b"wrong"
            variants.append(pa.field("serialized", pa.large_string(), metadata=wrong))
        extra = dict(witness.schema.field(2).metadata)
        extra[b"ADBC:redshift:type_modifier"] = b"-1"
        variants.append(pa.field("serialized", pa.large_string(), metadata=extra))
        variants.append(witness.schema.field(2).with_nullable(False))
        for field in variants:
            with self.subTest(field=field), self.assertRaises(hll.Mismatch):
                hll._scalar_rows(
                    witness.set_column(2, field, witness.column(2).cast(field.type)),
                    [4],
                )

    def test_observed_unsupported_cast_is_never_reintroduced(self):
        session = Session()
        fixture = self.complete(session)
        self.assertEqual(fixture.crossing, THRESHOLD)
        self.assertFalse(
            any("CAST(value AS VARCHAR" in sql for sql in session.operations)
        )
        self.assertEqual(
            sum("value AS serialized" in sql for sql in session.operations), 17
        )
        self.assertTrue(
            any("HLL_CARDINALITY(value)" in sql for sql in session.operations)
        )

    def test_owned_fixture_and_bounds_before_sql(self):
        for n in (True, 4, 4097, -1):
            with self.assertRaises(ValueError):
                hll.input_counts(n)
        session = Session()
        fixture = hll.discover(session)
        count = len(session.operations)
        with self.assertRaises(ValueError):
            hll.qualify(session, dataclasses.replace(fixture, schema="public"), False)
        self.assertEqual(len(session.operations), count)


if __name__ == "__main__":
    unittest.main()
