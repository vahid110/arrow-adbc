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

"""Installed public API vendor boundaries; explicit PostgreSQL or native Redshift.

PostgreSQL mode proves rejection and existing binary plumbing only. Redshift mode
exercises generated JSON-subset/spatial/HLL ingest and bounded typed vendor reads;
SUPER reads preserve observable native JSON bytes and wire NULLs, not every
server-side distinction. The mixed-HLL suite checks native per-column result formats.
Run the installed mixed-HLL suite inside an admitted Redshift test window:
  python -I -B ci/scripts/pgwire_vendor_public_test.py --backend redshift \
      --driver /path/to/libadbc_driver_redshift.dylib --suite mixed-hll --uri-fd 3
FD 3 must be an inherited pipe (or an owner-only file) containing one UTF-8 URI
terminated by EOF. Alternatively omit --uri-fd and set ADBC_REDSHIFT_TEST_URI.
The command runs both temporal modes and returns nonzero on failure.
"""

import argparse
import hashlib
import os
import pathlib
import re
import stat
import time
import unittest
import uuid

import pyarrow as pa

import adbc_driver_manager as adm

SUPER_METADATA = {
    b"ARROW:extension:name": b"adbc.redshift.super.json",
    b"ARROW:extension:metadata": (
        b'{"version":1,"encoding":"utf8-json","scope":"json-subset-write"}'
    ),
    b"ADBC:redshift:sql_type": b"SUPER",
}


def super_batch(values):
    return pa.RecordBatch.from_arrays(
        [
            pa.array(list(range(len(values))), type=pa.int32()),
            pa.array(values, type=pa.large_string()),
        ],
        schema=pa.schema(
            [
                pa.field("row_id", pa.int32()),
                pa.field("amount", pa.large_string(), metadata=SUPER_METADATA),
            ]
        ),
    )


HLL_EMPTY = '{"version":1,"logm":15,"sparse":{"indices":[],"values":[]}}'
GEOMETRY_POINT = bytes.fromhex("0101000000000000000000f03f000000000000f03f")
GEOGRAPHY_POINT = bytes.fromhex("0101000020e6100000000000000000f03f000000000000f03f")


# The HLL column is added separately; these are the 20 released non-HLL
# families. Store declared types (especially decimal/interval) before comparing
# native schemas. The NULL row leaves every non-key column unspecified.
MIXED_FIELDS = (
    ("s16", "SMALLINT", "-123"),
    ("i32", "INTEGER", "-1234567"),
    ("i64", "BIGINT", "9007199254740993"),
    ("num", "DECIMAL(20,4)", "1234567890123456.7890"),
    ("f32", "REAL", "1.25"),
    ("f64", "DOUBLE PRECISION", "-2.5"),
    ("ch", "CHAR(4)", "'x'"),
    ("vc", "VARCHAR(32)", "'é🙂'"),
    ("d", "DATE", "DATE '2000-01-02'"),
    ("tm", "TIME", "TIME '01:02:03.000004'"),
    ("tz", "TIMETZ", "TIMETZ '01:02:03.000004+02'"),
    ("ts", "TIMESTAMP", "TIMESTAMP '2000-01-01 00:00:00.000001'"),
    ("tstz", "TIMESTAMPTZ", "TIMESTAMPTZ '2000-01-01 00:00:00.000001+00'"),
    ("ym", "INTERVAL YEAR TO MONTH", "INTERVAL '13' MONTH"),
    ("ds", "INTERVAL DAY TO SECOND", "INTERVAL '1.000001' SECOND"),
    ("flag", "BOOLEAN", "TRUE"),
    ("sv", "SUPER", """JSON_PARSE('{"n":1}')"""),
    ("vb", "VARBYTE(4)", "FROM_HEX('0080ff5c')"),
    ("gm", "GEOMETRY", f"ST_GeomFromEWKB(FROM_HEX('{GEOMETRY_POINT.hex()}'))"),
    (
        "gg",
        "GEOGRAPHY",
        f"ST_GeomFromEWKB(FROM_HEX('{GEOGRAPHY_POINT.hex()}'))::GEOGRAPHY",
    ),
)


def encoded_field(family, name="amount"):
    if family == "HLLSKETCH":
        extension = "adbc.redshift.hllsketch"
        metadata = '{"version":1,"encoding":"redshift-serialized","logm":15}'
        storage = pa.large_string()
    else:
        extension = f"adbc.redshift.{family.lower()}.ewkb"
        metadata = (
            '{"version":1,"encoding":"ewkb","family":"'
            + family
            + '","srid":"per-value","axes":"native"}'
        )
        storage = pa.large_binary()
    return pa.field(
        name,
        storage,
        metadata={
            b"ARROW:extension:name": extension.encode(),
            b"ARROW:extension:metadata": metadata.encode(),
            b"ADBC:redshift:sql_type": family.encode(),
        },
    )


def encoded_batch(family, values, with_id=True):
    values = list(values)
    field = encoded_field(family)
    columns = [pa.array(values, type=field.type)]
    fields = [field]
    if with_id:
        columns.insert(0, pa.array(range(len(values)), type=pa.int32()))
        fields.insert(0, pa.field("row_id", pa.int32()))
    return pa.RecordBatch.from_arrays(columns, schema=pa.schema(fields))


class VendorPublicTest(unittest.TestCase):
    def setUp(self):
        entrypoint = (
            "AdbcDriverRedshiftInit"
            if self.backend == "redshift"
            else "AdbcDriverPostgresqlInit"
        )
        self.database = adm.AdbcDatabase(
            driver=self.driver, entrypoint=entrypoint, uri=self.uri
        )
        self.addCleanup(self.database.close)
        options = getattr(self, "connection_options", {})
        self.connection = adm.AdbcConnection(self.database, **options)
        self.addCleanup(self.connection.close)
        self.connection.set_autocommit(True)
        self.table = "super_" + uuid.uuid4().hex

    def shortDescription(self):
        if hasattr(self, "connection_options"):
            return (
                "mixed HLL: "
                + self.connection_options["adbc.redshift.temporal_representation"]
            )
        return super().shortDescription()

    def execute(self, sql):
        with adm.AdbcStatement(self.connection) as statement:
            statement.set_sql_query(sql)
            statement.execute_update()

    def query(self, sql):
        with adm.AdbcStatement(self.connection) as statement:
            statement.set_sql_query(sql)
            stream, _ = statement.execute_query()
            with pa.RecordBatchReader._import_from_c(stream.address) as reader:
                return reader.read_all().to_pylist()

    def ingest(self, mode, values, stream=False):
        with adm.AdbcStatement(self.connection) as statement:
            statement.set_options(
                **{
                    "adbc.ingest.target_table": self.table,
                    "adbc.ingest.mode": "adbc.ingest.mode." + mode,
                    "adbc.ingest.temporary": "true",
                }
            )
            batch = super_batch(values)
            if stream:
                statement.bind_stream(
                    pa.RecordBatchReader.from_batches(batch.schema, [batch])
                )
            else:
                statement.bind(batch)
            statement.execute_update()

    def ingest_encoded(self, family, values, mode="create", stream=False):
        with adm.AdbcStatement(self.connection) as statement:
            statement.set_options(
                **{
                    "adbc.ingest.target_table": self.table,
                    "adbc.ingest.mode": "adbc.ingest.mode." + mode,
                    "adbc.ingest.temporary": "true",
                }
            )
            batch = encoded_batch(family, values)
            if stream:
                statement.bind_stream(
                    pa.RecordBatchReader.from_batches(batch.schema, [batch])
                )
            else:
                statement.bind(batch)
            statement.execute_update()

    def query_table(self, sql):
        with adm.AdbcStatement(self.connection) as statement:
            statement.set_sql_query(sql)
            stream, _ = statement.execute_query()
            with pa.RecordBatchReader._import_from_c(stream.address) as reader:
                return reader.read_all()

    def describe(self, sql):
        with adm.AdbcStatement(self.connection) as stmt:
            stmt.set_sql_query(sql)
            handle = stmt.execute_schema()
            return pa.Schema._import_from_c(handle.address)

    def check_empty_projection(self, empty, described, ordinary, hll, populated):
        for table in (empty, ordinary, hll):
            self.assertEqual(table.num_rows, 0)
        # Use positional fields so a missing/reordered/renamed field is a
        # reportable schema mismatch rather than an error looking up an alias.
        controls = pa.schema(
            [hll.schema.field(0), *ordinary.schema, hll.schema.field(1)],
            metadata=ordinary.schema.metadata,
        )
        self.assertEqual(hll.schema.metadata, ordinary.schema.metadata)
        comparisons = (
            ("populated-vs-empty", populated, empty.schema),
            ("empty-describe", described, empty.schema),
            ("empty-controls", controls, empty.schema),
        )
        failed = []
        for label, expected, actual in comparisons:
            if not actual.equals(expected, check_metadata=True):
                mode = self.connection_options["adbc.redshift.temporal_representation"]
                print(mode, label, safe_schema_diff(expected, actual), flush=True)
                if label != "populated-vs-empty":
                    failed.append(label)
        self.assertFalse(failed)

    def test_encoded_replace_preserves_sentinel_on_both_bind_apis(self):
        self.execute(f'CREATE TEMP TABLE "{self.table}" (marker INTEGER)')
        self.execute(f'INSERT INTO "{self.table}" VALUES (71)')
        for family in ("GEOMETRY", "GEOGRAPHY", "HLLSKETCH"):
            for stream in (False, True):
                with self.assertRaises(adm.Error):
                    self.ingest_encoded(family, [None], "replace", stream)
                self.assertEqual(
                    self.query(f'SELECT marker FROM "{self.table}"'), [{"marker": 71}]
                )

    def test_encoded_wrong_backend_and_direct_spatial_are_preownership_errors(self):
        families = (
            ("GEOMETRY", "GEOGRAPHY", "HLLSKETCH")
            if self.backend == "postgresql"
            else ("GEOMETRY", "GEOGRAPHY")
        )
        for family in families:
            for stream in (False, True):
                with adm.AdbcStatement(self.connection) as statement:
                    statement.set_sql_query("SELECT $1")
                    batch = encoded_batch(family, [None], with_id=False)
                    with self.assertRaises(adm.Error):
                        if stream:
                            statement.bind_stream(
                                pa.RecordBatchReader.from_batches(batch.schema, [batch])
                            )
                        else:
                            statement.bind(batch)

    def test_spatial_generated_write_native_read_and_ipc(self):
        if self.backend != "redshift":
            for family in ("GEOMETRY", "GEOGRAPHY"):
                with self.assertRaises(adm.Error):
                    self.ingest_encoded(family, [None])
            return
        for family, value in (
            ("GEOMETRY", GEOMETRY_POINT),
            ("GEOGRAPHY", GEOGRAPHY_POINT),
        ):
            self.table = "spatial_" + uuid.uuid4().hex
            self.ingest_encoded(family, [value, None])
            self.ingest_encoded(family, [value], "append", True)
            table = self.query_table(
                f'SELECT amount FROM "{self.table}" ORDER BY row_id'
            )
            self.assertEqual(
                sorted(x.hex() for x in table.column(0).to_pylist() if x is not None),
                [value.hex(), value.hex()],
            )
            self.assertEqual(table.column(0).null_count, 1)
            self.assertEqual(
                table.schema.field(0).metadata[b"ARROW:extension:name"],
                encoded_field(family).metadata[b"ARROW:extension:name"],
            )
            sink = pa.BufferOutputStream()
            with pa.ipc.new_stream(sink, table.schema) as writer:
                writer.write_table(table)
            restored = pa.ipc.open_stream(sink.getvalue()).read_all()
            self.assertEqual(restored, table)

    def test_hll_typed_direct_bind_all_hll_multiple_empty_and_mixed_boundary(self):
        if self.backend != "redshift":
            with self.assertRaises(adm.Error):
                self.ingest_encoded("HLLSKETCH", [None])
            return
        self.ingest_encoded("HLLSKETCH", [HLL_EMPTY, None])
        self.ingest_encoded("HLLSKETCH", [HLL_EMPTY], "append", True)
        table = self.query_table(
            f'SELECT amount, amount AS second FROM "{self.table}" ORDER BY row_id'
        )
        self.assertEqual(table.num_columns, 2)
        self.assertEqual(table.column(0).to_pylist(), table.column(1).to_pylist())
        self.assertEqual(table.column(0).null_count, 1)
        self.assertEqual(table.schema.field(0).type, pa.large_string())
        empty = self.query_table(f'SELECT amount FROM "{self.table}" WHERE false')
        self.assertEqual(empty.num_rows, 0)
        self.assertEqual(
            empty.schema.field(0).metadata[b"ARROW:extension:name"],
            b"adbc.redshift.hllsketch",
        )
        mixed = self.query_table(
            f'SELECT row_id, amount FROM "{self.table}" ORDER BY row_id'
        )
        self.assertEqual(mixed.column("row_id").to_pylist(), [0, 0, 1])
        self.assertEqual(
            mixed.column("amount").to_pylist(), table.column(0).to_pylist()
        )
        self.assertEqual(
            mixed.schema.field("amount").metadata, table.schema.field(0).metadata
        )
        with adm.AdbcStatement(self.connection) as statement:
            statement.set_sql_query("SELECT $1")
            statement.bind(encoded_batch("HLLSKETCH", [HLL_EMPTY, None], with_id=False))
            stream, _ = statement.execute_query()
            with pa.RecordBatchReader._import_from_c(stream.address) as reader:
                result = reader.read_all()
            self.assertEqual(result.column(0).null_count, 1)
            self.assertEqual(
                result.schema.field(0).metadata[b"ARROW:extension:name"],
                b"adbc.redshift.hllsketch",
            )
        self.assertEqual(
            [
                x["n"]
                for x in self.query(
                    "SELECT HLL_CARDINALITY(amount) AS n "
                    f'FROM "{self.table}" WHERE amount IS NOT NULL'
                )
            ],
            [0, 0],
        )

    def test_mixed_hll_native_projection(self):
        if self.backend != "redshift":
            self.skipTest("native Redshift only")
        name = f'"{self.table}"'
        columns = ", ".join(f"{alias} {typ}" for alias, typ, _ in MIXED_FIELDS)
        values = ", ".join(expr for _, _, expr in MIXED_FIELDS)
        names = [alias for alias, _, _ in MIXED_FIELDS]
        self.execute(f"CREATE TEMP TABLE {name} (id INTEGER, h HLLSKETCH, {columns})")
        # One effectful INSERT, not a query to replay on an execution error. Its
        # observable effect is checked independently of mixed-format decoding.
        self.execute(f"INSERT INTO {name} SELECT 1, HLL_CREATE_SKETCH(42), {values}")
        self.assertEqual(self.query(f"SELECT COUNT(*) AS n FROM {name}"), [{"n": 1}])
        self.execute(f"INSERT INTO {name} (id) VALUES (2)")
        tail = f" FROM {name} ORDER BY id"
        hll = self.query_table("SELECT h" + tail)
        self.assertEqual(
            self.query("SELECT id, HLL_CARDINALITY(h) AS n" + tail),
            [{"id": 1, "n": 1}, {"id": 2, "n": None}],
        )
        self.assertIsNotNone(hll.column(0)[0].as_py())
        self.assertIsNone(hll.column(0)[1].as_py())
        self.assertEqual(hll.schema.field(0).type, pa.large_string())
        for key, value in encoded_field("HLLSKETCH").metadata.items():
            self.assertEqual(hll.schema.field(0).metadata[key], value)

        # The same stored values through the ordinary binary-only projection
        # provide an independent type/metadata/value oracle for all neighbors.
        ordinary = self.query_table("SELECT id, " + ", ".join(names) + tail)
        mixed_columns = "h AS first_h, id, " + ", ".join(names) + ", h AS last_h"
        mixed = self.query_table("SELECT " + mixed_columns + tail)
        self.check_projection(mixed, ordinary, hll)
        self.check_mixed_sketch_member(name, mixed)
        described = self.describe(f"SELECT {mixed_columns} FROM {name}")
        self.assertTrue(described.equals(mixed.schema, check_metadata=True))
        empty_from = f" FROM {name} WHERE 1 = 0"
        empty_sql = "SELECT " + mixed_columns + empty_from
        self.check_empty_projection(
            self.query_table(empty_sql),
            self.describe(empty_sql),
            self.query_table("SELECT id, " + ", ".join(names) + empty_from),
            self.query_table("SELECT h AS first_h, h AS last_h" + empty_from),
            mixed.schema,
        )

        # Bind and BindStream each execute one fixed SELECT per input row.
        # Two nonempty input batches surround an empty batch; no duplicating or
        # reordering rows across result batches is permitted.
        for stream_bind in (False, True):
            with (
                self.subTest(stream_bind=stream_bind),
                adm.AdbcStatement(self.connection) as stmt,
            ):
                stmt.set_sql_query(f"SELECT id, h FROM {name} WHERE id = $1")
                stmt.prepare()
                batch = pa.record_batch(
                    [pa.array([2, 1, 2], type=pa.int32())], names=["id"]
                )
                if stream_bind:
                    stmt.bind_stream(
                        pa.RecordBatchReader.from_batches(
                            batch.schema,
                            [batch.slice(0, 1), batch.slice(1, 0), batch.slice(1, 2)],
                        )
                    )
                else:
                    stmt.bind(batch)
                handle, _ = stmt.execute_query()
                with pa.RecordBatchReader._import_from_c(handle.address) as reader:
                    batches = list(reader)
                    schema = reader.schema
                self.assertGreaterEqual(sum(b.num_rows > 0 for b in batches), 2)
                result = pa.Table.from_batches(batches, schema=schema)
                self.assertEqual(result.column_names, ["id", "h"])
                self.assertEqual(result.column("id").to_pylist(), [2, 1, 2])
                self.assertEqual(
                    result.column("h").to_pylist(),
                    [None, hll.column(0)[0].as_py(), None],
                )
                self.assertEqual(
                    result.schema.field("h").metadata, hll.schema.field(0).metadata
                )

        if self.connection_options["adbc.redshift.temporal_representation"] == "arrow":
            self.check_tagged_execution(name)

    def check_projection(self, mixed, ordinary, hll):
        names = [alias for alias, _, _ in MIXED_FIELDS]
        self.assertEqual(ordinary.column_names, ["id", *names])
        self.assertEqual(ordinary.column("id").to_pylist(), [1, 2])
        for col in names:
            self.assertEqual(ordinary.column(col).is_null().to_pylist(), [False, True])
        self.assertEqual(mixed.column_names, ["first_h", "id", *names, "last_h"])
        self.assertTrue(
            mixed.select(ordinary.column_names).equals(ordinary, check_metadata=True)
        )
        for col in ("first_h", "last_h"):
            self.assertEqual(mixed.column(col), hll.column(0))
            self.assertEqual(
                mixed.schema.field(col).metadata, hll.schema.field(0).metadata
            )
        self.assertEqual(mixed.column("i64").to_pylist(), [9007199254740993, None])
        self.assertEqual(
            mixed.column("vb").to_pylist(), [bytes.fromhex("0080ff5c"), None]
        )
        self.assertEqual(mixed.column("sv").to_pylist(), ['{"n":1}', None])
        self.assertEqual(mixed.column("gm").to_pylist(), [GEOMETRY_POINT, None])
        self.assertEqual(mixed.column("gg").to_pylist(), [GEOGRAPHY_POINT, None])
        if (
            self.connection_options["adbc.redshift.temporal_representation"]
            == "lossless"
        ):
            self.assertEqual(mixed.column("ts").to_pylist(), [1, None])
            self.assertEqual(
                mixed.schema.field("ts").metadata[b"ARROW:extension:name"],
                b"adbc.redshift.timestamp_us_2000.v1",
            )
        else:
            self.assertEqual(mixed.schema.field("ts").type, pa.timestamp("us"))

    def check_mixed_sketch_member(self, name, mixed):
        # Rebind the mixed result itself. Two different one-member sketches
        # both have cardinality one, but their union can reveal substitution.
        value = mixed.column("first_h")[0].as_py()
        batch = encoded_batch("HLLSKETCH", [value], with_id=False)
        with adm.AdbcStatement(self.connection) as stmt:
            stmt.set_sql_query(
                "SELECT HLL_CARDINALITY($1::HLLSKETCH) AS rebound, "
                "(SELECT HLL_CARDINALITY(HLL_COMBINE(value)) FROM "
                f"(SELECT h AS value FROM {name} WHERE id = 1 "
                "UNION ALL SELECT $1::HLLSKETCH AS value) AS sketches) AS combined"
            )
            stmt.bind(batch)
            handle, _ = stmt.execute_query()
            with pa.RecordBatchReader._import_from_c(handle.address) as reader:
                self.assertEqual(
                    reader.read_all().to_pylist(), [{"rebound": 1, "combined": 1}]
                )

    def check_tagged_execution(self, name):
        # Query-history visibility is limited to the current user's own query.
        # Poll only the observation; never replay the volatile SELECT.
        tag = "adbc_hll_" + uuid.uuid4().hex
        prefix = f"SELECT /* {tag} */"
        single = self.query_table(
            f"{prefix} RANDOM() AS sample, h FROM {name} WHERE id = 1"
        )
        self.assertEqual(single.num_rows, 1)
        self.assertIsNotNone(single.column("h")[0].as_py())
        self.assertGreaterEqual(single.column("sample")[0].as_py(), 0.0)
        self.assertLessEqual(single.column("sample")[0].as_py(), 1.0)
        for attempt in range(5):
            try:
                count = self.query(
                    f"SELECT COUNT(*) AS n FROM sys_query_history "
                    f"WHERE LEFT(LTRIM(query_text), {len(prefix)}) = '{prefix}'"
                )[0]["n"]
            except adm.Error as exc:
                if exc.sqlstate not in ("42501", b"42501"):
                    raise
                print("Tagged SELECT count unavailable (query history access denied).")
                return
            if count:
                self.assertEqual(count, 1)
                return
            if attempt < 4:
                time.sleep(0.5)
        self.fail("tagged execution not visible in own query history")

    def test_explicit_super_replace_preserves_sentinel_on_both_bind_apis(self):
        self.execute(f'CREATE TEMP TABLE "{self.table}" (marker INTEGER)')
        self.execute(f'INSERT INTO "{self.table}" VALUES (71)')
        for stream in (False, True):
            with self.assertRaises(adm.Error):
                self.ingest("replace", [None, "null"], stream)
            self.assertEqual(
                self.query(f'SELECT marker FROM "{self.table}"'), [{"marker": 71}]
            )

    def test_arbitrary_super_bind_rejects_and_postbind_mutation_is_checked(self):
        batch = super_batch([None])
        with adm.AdbcStatement(self.connection) as statement:
            statement.set_sql_query("SELECT $1, $2")
            with self.assertRaises(adm.Error):
                statement.bind(batch)
        if self.backend == "redshift":
            with adm.AdbcStatement(self.connection) as statement:
                statement.set_options(**{"adbc.ingest.target_table": self.table})
                statement.bind(super_batch(["null"]))
                statement.set_sql_query("SELECT $1, $2")
                with self.assertRaises(adm.Error):
                    statement.execute_update()

    def test_existing_binary_varbyte_path_preserves_literal_bytes(self):
        with adm.AdbcStatement(self.connection) as statement:
            statement.set_sql_query("SELECT $1")
            batch = pa.record_batch(
                [pa.array([b"\x00\x80\xff", b"", None], type=pa.binary())],
                names=["value"],
            )
            statement.bind(batch)
            stream, _ = statement.execute_query()
            with pa.RecordBatchReader._import_from_c(stream.address) as reader:
                self.assertEqual(
                    reader.read_all().column(0).to_pylist(),
                    [b"\x00\x80\xff", b"", None],
                )

    def test_json_subset_ingest_native_read_and_ipc(self):
        if self.backend != "redshift":
            with self.assertRaises(adm.Error):
                self.ingest("create", ["null"])
            return
        # JSON_PARSE fixtures from native_protocol_plan_data.inc and their
        # retained native bytes in redshift_super_native_corpus_test.inc.
        values = [
            None,
            "null",
            '""',
            "true",
            "[]",
            '{"other":1}',
            "9007199254740993",
            "1e0",
            '"é🙂\\u0001"',
            '"null"',
        ]
        self.ingest("create", values)
        rows = self.query(
            "SELECT row_id, JSON_TYPEOF(amount) AS kind, "
            f'JSON_SERIALIZE(amount) AS value FROM "{self.table}" ORDER BY row_id'
        )
        self.assertEqual([x["value"] for x in rows[:3]], ["null", "null", '""'])
        self.assertEqual(
            [x["kind"] for x in rows[2:6]], ["string", "boolean", "array", "object"]
        )
        table = self.query_table(
            f'SELECT row_id, amount FROM "{self.table}" ORDER BY row_id'
        )
        self.assertEqual(table.column("row_id").to_pylist(), list(range(len(values))))
        field = table.schema.field("amount")
        self.assertEqual(field.type, pa.large_string())
        self.assertEqual(
            field.metadata[b"ARROW:extension:name"], b"adbc.redshift.super.native_json"
        )
        self.assertEqual(
            field.metadata[b"ARROW:extension:metadata"],
            b'{"version":1,"encoding":"redshift-json-wire",'
            b'"scope":"native-result","numbers":"lexemes",'
            b'"nulls":"wire-null"}',
        )
        self.assertEqual(field.metadata[b"ADBC:redshift:sql_type"], b"SUPER")
        self.assertNotEqual(field.metadata, SUPER_METADATA)
        # SQL NULL and JSON null both arrived as wire NULL in retained evidence.
        # JSON_PARSE also normalized 1e0 to 1: assert received bytes, not inputs.
        expected = [
            None,
            None,
            b'""',
            b"true",
            b"[]",
            b'{"other":1}',
            b"9007199254740993",
            b"1",
            '"é🙂\\u0001"'.encode(),
            b'"null"',
        ]
        self.assertEqual(
            table.column("amount").cast(pa.large_binary()).to_pylist(), expected
        )
        self.assertEqual(table.column("amount").null_count, 2)
        sink = pa.BufferOutputStream()
        with pa.ipc.new_stream(sink, table.schema) as writer:
            writer.write_table(table)
        restored = pa.ipc.open_stream(sink.getvalue()).read_all()
        self.assertTrue(restored.equals(table, check_metadata=True))
        before = self.query(f'SELECT count(*) AS n FROM "{self.table}"')
        for stream in (False, True):
            with self.assertRaises(adm.Error) as failure:
                self.ingest("append", ['{"a":1,"\\u0061":2}'], stream)
            self.assertEqual(
                failure.exception.status_code, adm.AdbcStatusCode.INVALID_ARGUMENT
            )
            self.assertEqual(
                self.query(f'SELECT count(*) AS n FROM "{self.table}"'), before
            )


def safe_schema_diff(expected, actual):
    """Report fixture fields and numeric modifiers; hash arbitrary metadata."""

    def digest(value):
        return hashlib.sha256(value).hexdigest()[:12]

    def metadata_diff(left, right):
        left, right = left or {}, right or {}
        changes = []
        numeric = {
            b"ADBC:redshift:" + key
            for key in (
                b"type_modifier",
                b"byte_width",
                b"precision",
                b"scale",
                b"fractional_precision",
                b"oid",
            )
        }
        known = numeric | {
            b"POSTGRESQL:type",
            b"ADBC:redshift:sql_type",
            b"ARROW:extension:name",
            b"ARROW:extension:metadata",
        }
        for key in sorted(left.keys() | right.keys()):
            if left.get(key) == right.get(key):
                continue

            def token(value):
                if value is None:
                    return "absent"
                if key in numeric and len(value) <= 11 and value.lstrip(b"-").isdigit():
                    return value.decode("ascii")
                return digest(value)

            label = key.decode("ascii") if key in known else digest(key)
            changes.append((label, token(left.get(key)), token(right.get(key))))
        return changes

    def type_token(field):
        arrow_type = field.type
        if pa.types.is_decimal(arrow_type):
            return (arrow_type.id, arrow_type.precision, arrow_type.scale)
        return (arrow_type.id, digest(str(arrow_type).encode()))

    changes = [("fields", len(expected), len(actual))]
    top = metadata_diff(expected.metadata, actual.metadata)
    if top:
        changes.append(("schema-metadata", top))
    names = {"id", "first_h", "last_h"} | {alias for alias, _, _ in MIXED_FIELDS}
    for i, (left, right) in enumerate(zip(expected, actual)):
        if left.equals(right, check_metadata=True):
            continue
        changes.append(
            (
                i,
                left.name if left.name in names else digest(left.name.encode()),
                right.name if right.name in names else digest(right.name.encode()),
                type_token(left),
                type_token(right),
                left.nullable,
                right.nullable,
                metadata_diff(left.metadata, right.metadata),
            )
        )
    text = repr(changes)
    if len(text) > 255:
        return text[:235] + "..." + digest(text.encode())
    return text


class SafeResult(unittest.TextTestResult):
    def _exc_info_to_string(self, err, test):
        # libpq/manager exceptions can quote complete connection arguments.
        # Report a useful failing frame but never stringify the exception.
        kind, exc, trace = err
        selected = trace
        while trace is not None:
            if trace.tb_frame.f_code.co_filename == __file__:
                selected = trace
            trace = trace.tb_next
        frame = selected.tb_frame.f_code.co_name
        detail = ""
        if isinstance(exc, adm.Error):
            state = exc.sqlstate
            if isinstance(state, bytes):
                state = state.decode("ascii", "replace")
            if isinstance(state, str) and re.fullmatch("[0-9A-Z]{5}", state):
                detail += f" SQLSTATE={state}"
            if exc.status_code in tuple(adm.AdbcStatusCode):
                detail += f" ADBC={int(exc.status_code)}"
        return f"{kind.__name__} in {frame}:{selected.tb_lineno}{detail}"


def read_uri(fd):
    if fd is None:
        value = os.environ.get("ADBC_REDSHIFT_TEST_URI", "")
    else:
        info = os.fstat(fd)
        if not stat.S_ISFIFO(info.st_mode) and not (
            stat.S_ISREG(info.st_mode)
            and hasattr(os, "getuid")
            and info.st_uid == os.getuid()
            and not (info.st_mode & 0o077)
        ):
            raise ValueError("private URI descriptor required")
        with os.fdopen(os.dup(fd), "rb") as source:
            raw = source.read(16385)
        if len(raw) > 16384:
            raise ValueError("URI input exceeds limit")
        value = raw.decode("utf-8").removesuffix("\n")
    if not value or len(value) > 16384 or any(c in value for c in "\r\n\x00"):
        raise ValueError("invalid private URI input")
    return value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=pathlib.Path, required=True)
    auth = parser.add_mutually_exclusive_group()
    auth.add_argument("--uri-fd", type=int)
    auth.add_argument("--uri", help="legacy PostgreSQL fixture only")
    parser.add_argument("--backend", choices=("postgresql", "redshift"), required=True)
    parser.add_argument("--suite", choices=("all", "mixed-hll"), default="all")
    args = parser.parse_args()
    if args.backend == "redshift" and args.uri is not None:
        parser.error("Redshift requires private URI input")
    if args.suite == "mixed-hll" and args.backend != "redshift":
        parser.error("mixed-hll requires Redshift")
    try:
        uri = args.uri if args.uri is not None else read_uri(args.uri_fd)
    except (ValueError, OSError):
        parser.error("invalid private URI input")
    VendorPublicTest.driver = str(args.driver.resolve(strict=True))
    VendorPublicTest.uri = uri
    VendorPublicTest.backend = args.backend
    suite = unittest.TestSuite()
    for name in unittest.defaultTestLoader.getTestCaseNames(VendorPublicTest):
        if name == "test_mixed_hll_native_projection":
            if args.backend == "redshift":
                for mode in ("arrow", "lossless"):
                    case = VendorPublicTest(name)
                    case.connection_options = {
                        "adbc.redshift.temporal_representation": mode
                    }
                    suite.addTest(case)
        elif args.suite == "all":
            suite.addTest(VendorPublicTest(name))
    return (
        0
        if unittest.TextTestRunner(verbosity=2, resultclass=SafeResult)
        .run(suite)
        .wasSuccessful()
        else 1
    )


if __name__ == "__main__":
    raise SystemExit(main())
