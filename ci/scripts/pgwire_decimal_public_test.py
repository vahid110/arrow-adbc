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

"""Installed Python/public ADBC decimal checks against explicitly supplied local PG.

This uses the PostgreSQL driver with explicit DECIMAL semantic metadata. It never
changes backend identity and is not native Redshift qualification. Every table is
connection-local TEMP; there is no URI, driver, credential or port fallback.
"""

import argparse
import pathlib
import sys
import unittest
from decimal import Decimal
from urllib.parse import parse_qs, urlsplit

import pyarrow as pa

import adbc_driver_manager as adm

IDENTITY = {b"ADBC:redshift:sql_type": b"DECIMAL"}


def batch(arrow_type, values, metadata=IDENTITY):
    return pa.RecordBatch.from_arrays(
        [pa.array(values, type=arrow_type)],
        schema=pa.schema([pa.field("amount", arrow_type, metadata=metadata)]),
    )


class DecimalPublicPythonTest(unittest.TestCase):
    def setUp(self):
        self.database = adm.AdbcDatabase(
            driver=self.driver, entrypoint="AdbcDriverPostgresqlInit", uri=self.uri
        )
        self.connection = adm.AdbcConnection(self.database)
        self.connection.set_autocommit(True)

    def tearDown(self):
        self.connection.close()
        self.database.close()

    def query(self, sql):
        with adm.AdbcStatement(self.connection) as stmt:
            stmt.set_sql_query(sql)
            stream, _ = stmt.execute_query()
            with pa.RecordBatchReader._import_from_c(stream.address) as reader:
                return reader.read_all().to_pylist()

    def execute(self, sql):
        with adm.AdbcStatement(self.connection) as stmt:
            stmt.set_sql_query(sql)
            return stmt.execute_update()

    def ingest(self, table, mode, data, stream=False):
        with adm.AdbcStatement(self.connection) as stmt:
            stmt.set_options(
                **{
                    "adbc.ingest.target_table": table,
                    "adbc.ingest.mode": "adbc.ingest.mode." + mode,
                    "adbc.ingest.temporary": "true",
                }
            )
            if stream:
                stmt.bind_stream(data)
            else:
                stmt.bind(data)
            return stmt.execute_update()

    def test_bind_array_and_multibatch_stream_preserve_numeric_identity_and_value(self):
        for arrow_type in (pa.decimal128(38, 4), pa.decimal256(38, 4), pa.string()):
            values = ["12.3400", "-12.3400", None]
            data = batch(
                arrow_type,
                values
                if pa.types.is_string(arrow_type)
                else [None if x is None else Decimal(x) for x in values],
            )
            for stream_bind in (False, True):
                with adm.AdbcStatement(self.connection) as stmt:
                    stmt.set_sql_query(
                        "SELECT pg_typeof($1)::text AS kind,$1::text AS value"
                    )
                    if stream_bind:
                        source = pa.RecordBatchReader.from_batches(
                            data.schema, [data.slice(0, 1), data.slice(1, 2)]
                        )
                        self.assertEqual(source.schema.field(0).metadata, IDENTITY)
                        stmt.bind_stream(source)
                    else:
                        stmt.bind(data)
                    handle, _ = stmt.execute_query()
                    with pa.RecordBatchReader._import_from_c(handle.address) as reader:
                        self.assertEqual(
                            reader.read_all().to_pylist(),
                            [{"kind": "numeric", "value": x} for x in values],
                        )
                    self.assertEqual(data.schema.field(0).metadata, IDENTITY)
        # The SQL author may intentionally round. The driver preserves the
        # offered value and does not infer a target scale from the CAST text.
        with adm.AdbcStatement(self.connection) as stmt:
            stmt.set_sql_query(
                "SELECT $1::text AS offered,CAST($1 AS NUMERIC(10,2))::text AS cast"
            )
            stmt.bind(batch(pa.string(), ["1.2345"]))
            handle, _ = stmt.execute_query()
            with pa.RecordBatchReader._import_from_c(handle.address) as reader:
                self.assertEqual(
                    reader.read_all().to_pylist(),
                    [{"offered": "1.2345", "cast": "1.23"}],
                )

    def test_create_explicit_scale_and_late_append_failure_preserves_sentinel(self):
        data = batch(pa.decimal128(10, 2), [Decimal("9.99"), None])
        source = pa.RecordBatchReader.from_batches(
            data.schema, [data.slice(0, 0), data, data.slice(0, 0)]
        )
        self.assertEqual(
            self.ingest("adbc_c3_python_create", "create", source, True), 2
        )
        self.assertEqual(
            self.query(
                "SELECT format_type(atttypid,atttypmod) AS type FROM pg_attribute "
                "WHERE attrelid='adbc_c3_python_create'::regclass AND attname='amount'"
            ),
            [{"type": "numeric(10,2)"}],
        )
        values = batch(pa.decimal256(18, 4), [Decimal("1.2500"), Decimal("1.2345")])
        source = pa.RecordBatchReader.from_batches(
            values.schema, [values.slice(0, 1), values.slice(1, 1)]
        )
        with self.assertRaises(adm.Error):
            self.ingest("adbc_c3_python_create", "append", source, True)
        self.assertEqual(
            self.query(
                "SELECT amount::text AS amount FROM adbc_c3_python_create "
                "ORDER BY amount NULLS LAST"
            ),
            [{"amount": "9.99"}, {"amount": None}],
        )
        self.assertEqual(values.schema.field(0).metadata, IDENTITY)
        self.assertEqual(
            self.ingest("adbc_c3_python_create", "create_append", values.slice(0, 1)), 1
        )
        self.assertEqual(
            self.query(
                "SELECT amount::text AS amount FROM adbc_c3_python_create "
                "ORDER BY amount NULLS LAST"
            ),
            [{"amount": "1.25"}, {"amount": "9.99"}, {"amount": None}],
        )

    def test_rejected_replace_preserves_original_schema_and_rows(self):
        self.execute("CREATE TEMP TABLE adbc_c3_python_guard(amount INTEGER)")
        self.execute("INSERT INTO adbc_c3_python_guard VALUES(42)")
        for data in (
            batch(pa.decimal128(38, 38), [Decimal("0")]),
            batch(pa.string(), ["1.25"]),
        ):
            with self.assertRaises(adm.Error):
                self.ingest("adbc_c3_python_guard", "replace", data)
            self.assertEqual(
                self.query(
                    "SELECT amount::text AS amount,pg_typeof(amount)::text AS type "
                    "FROM adbc_c3_python_guard"
                ),
                [{"amount": "42", "type": "integer"}],
            )

    def test_decimal_dictionary_rejects_and_unmarked_pg_stays_text(
        self,
    ):
        values = pa.array(["1.25"]).dictionary_encode()
        for marked in (False, True):
            schema = pa.schema(
                [pa.field("amount", values.type, metadata=IDENTITY if marked else None)]
            )
            data = pa.RecordBatch.from_arrays([values], schema=schema)
            for stream_bind in (False, True):
                with adm.AdbcStatement(self.connection) as stmt:
                    stmt.set_sql_query(
                        "SELECT pg_typeof($1)::text AS kind,$1::text AS value"
                    )
                    source = pa.RecordBatchReader.from_batches(schema, [data])

                    def bind():
                        if stream_bind:
                            stmt.bind_stream(source)
                        else:
                            stmt.bind(data)

                    if marked:
                        with self.assertRaises(adm.Error):
                            bind()
                    else:
                        bind()
                        handle, _ = stmt.execute_query()
                        with pa.RecordBatchReader._import_from_c(
                            handle.address
                        ) as reader:
                            self.assertEqual(
                                reader.read_all().to_pylist(),
                                [{"kind": "text", "value": "1.25"}],
                            )

    def test_unknown_decimal_text_appends_but_never_defaults_create(self):
        metadata = {
            **IDENTITY,
            b"ADBC:redshift:precision": b"10",
            b"ADBC:redshift:scale": b"2",
        }
        data = batch(pa.string(), ["1.2500"], metadata)
        self.assertEqual(self.ingest("adbc_c3_python_text", "create", data), 1)
        unknown = batch(pa.string(), ["-2.5000", None])
        self.assertEqual(self.ingest("adbc_c3_python_text", "append", unknown), 2)
        self.assertEqual(
            self.query(
                "SELECT amount::text AS amount FROM adbc_c3_python_text "
                "ORDER BY amount NULLS LAST"
            ),
            [{"amount": "-2.50"}, {"amount": "1.25"}, {"amount": None}],
        )
        with self.assertRaises(adm.Error):
            self.ingest("adbc_c3_python_unknown", "create", unknown)
        contradictory = batch(
            pa.decimal128(10, 2),
            [Decimal("1.25")],
            {**metadata, b"ADBC:redshift:precision": b"11"},
        )
        with adm.AdbcStatement(self.connection) as stmt:
            stmt.set_sql_query("SELECT $1")
            with self.assertRaises(adm.Error):
                stmt.bind(contradictory)
        self.assertEqual(
            contradictory.schema.field(0).metadata[b"ADBC:redshift:precision"], b"11"
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", required=True, type=pathlib.Path)
    parser.add_argument("--uri", required=True)
    options = parser.parse_args()
    driver = options.driver
    if not driver.is_absolute() or not driver.is_file():
        parser.error(
            "--driver must name an existing absolute PostgreSQL driver library"
        )
    uri = urlsplit(options.uri)
    hosts = parse_qs(uri.query).get("host", [])
    socket = len(hosts) == 1 and hosts[0].startswith("/")
    loopback = (
        uri.hostname in ("localhost", "127.0.0.1", "::1", "postgres")
        and uri.port is not None
    )
    if uri.scheme not in ("postgres", "postgresql") or not (socket or loopback):
        parser.error(
            "--uri must select a local socket, loopback or CI postgres service"
        )
    if (
        pathlib.Path(sys.prefix).resolve()
        not in pathlib.Path(adm.__file__).resolve().parents
    ):
        parser.error("run with the installed driver-manager test environment")
    DecimalPublicPythonTest.driver = str(driver)
    DecimalPublicPythonTest.uri = options.uri
    unittest.main(argv=[sys.argv[0]], verbosity=2)
