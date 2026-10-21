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
exercises generated JSON-subset ingest; it does not qualify lossless SUPER reads.
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
