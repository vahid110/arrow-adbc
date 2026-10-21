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
it does not qualify lossless SUPER reads or mixed HLL projections.
The native CLI is a manual primitive, not a credential/admission mechanism. Root
live orchestration imports these fixed recipes in-process rather than putting
credentials in argv.
"""

import argparse
import pathlib
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
        self.connection = adm.AdbcConnection(self.database)
        self.connection.set_autocommit(True)
        self.table = "super_" + uuid.uuid4().hex

    def tearDown(self):
        self.connection.close()
        self.database.close()

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
        with self.assertRaises(adm.Error):
            self.query_table(f'SELECT row_id, amount FROM "{self.table}"')
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

    def test_json_subset_ingest_semantic_witness_and_unsupported_raw_read(self):
        if self.backend != "redshift":
            with self.assertRaises(adm.Error):
                self.ingest("create", ["null"])
            return
        self.ingest("create", [None, "null", '""', "true", "[]", '{"x":1.20}'])
        rows = self.query(
            "SELECT row_id, JSON_TYPEOF(amount) AS kind, "
            f'JSON_SERIALIZE(amount) AS value FROM "{self.table}" ORDER BY row_id'
        )
        self.assertEqual([x["value"] for x in rows[:3]], ["null", "null", '""'])
        self.assertEqual(
            [x["kind"] for x in rows[2:]], ["string", "boolean", "array", "object"]
        )
        with self.assertRaises(adm.Error):
            self.query(f'SELECT amount FROM "{self.table}"')
        before = self.query(f'SELECT count(*) AS n FROM "{self.table}"')
        with self.assertRaises(adm.Error):
            self.ingest("append", ['{"a":1,"\\u0061":2}'], True)
        self.assertEqual(
            self.query(f'SELECT count(*) AS n FROM "{self.table}"'), before
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=pathlib.Path, required=True)
    parser.add_argument("--uri", required=True)
    parser.add_argument("--backend", choices=("postgresql", "redshift"), required=True)
    args = parser.parse_args()
    VendorPublicTest.driver = str(args.driver.resolve(strict=True))
    VendorPublicTest.uri = args.uri
    VendorPublicTest.backend = args.backend
    unittest.main(argv=[__file__], verbosity=2)
