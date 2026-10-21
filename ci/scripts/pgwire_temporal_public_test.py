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

"""Installed temporal semantic input on an explicit own-local PostgreSQL library.

These public API tests prove shared input/transaction plumbing, never native
Redshift wire, finite domains or output representation mode qualification.
One exact owned schema/table shadow fixture is removed in finally; others are TEMP.
"""

import argparse
import pathlib
import sys
import unittest
from urllib.parse import parse_qs, urlsplit

import pyarrow as pa

import adbc_driver_manager as adm

EXTENSIONS = {
    "TIME": (
        "adbc.redshift.time_us.v1",
        '{"version":1,"family":"TIME","unit":"us","origin":"start_of_day","semantics":"civil"}',
    ),
    "TIMESTAMP": (
        "adbc.redshift.timestamp_us_2000.v1",
        '{"version":1,"family":"TIMESTAMP","unit":"us","epoch":"2000-01-01T00:00:00","semantics":"civil"}',
    ),
    "TIMESTAMPTZ": (
        "adbc.redshift.timestamptz_us_2000.v1",
        '{"version":1,"family":"TIMESTAMPTZ","unit":"us","epoch":"2000-01-01T00:00:00Z","semantics":"utc_instant"}',
    ),
}


def field(name, arrow_type, family, precision=None, lossless=False):
    metadata = {b"ADBC:redshift:sql_type": family.encode()}
    if precision is not None:
        metadata[b"ADBC:redshift:fractional_precision"] = str(precision).encode()
    if lossless:
        extension, description = EXTENSIONS[family]
        metadata[b"ARROW:extension:name"] = extension.encode()
        metadata[b"ARROW:extension:metadata"] = description.encode()
    return pa.field(name, arrow_type, metadata=metadata)


def batch(arrow_type, values, family, precision=None, lossless=False, name="amount"):
    return pa.RecordBatch.from_arrays(
        [pa.array(values, type=arrow_type)],
        schema=pa.schema([field(name, arrow_type, family, precision, lossless)]),
    )


class TemporalPublicPythonTest(unittest.TestCase):
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

    def ingest(self, table, mode, data, stream=False, temporary=True, db_schema=None):
        with adm.AdbcStatement(self.connection) as stmt:
            options = {
                "adbc.ingest.target_table": table,
                "adbc.ingest.mode": "adbc.ingest.mode." + mode,
                "adbc.ingest.temporary": "true" if temporary else "false",
            }
            if db_schema is not None:
                options["adbc.ingest.target_db_schema"] = db_schema
            stmt.set_options(**options)
            if stream:
                stmt.bind_stream(data)
            else:
                stmt.bind(data)
            return stmt.execute_update()

    def test_bind_array_stream_units_epochs_naive_aware_and_literal_wire(self):
        cases = [
            (
                pa.date32(),
                "DATE",
                [10957, 0, -719528, None],
                "date",
                "date_send",
                ["00000000", "ffffd533", "fff4da8b", None],
                False,
            ),
            (
                pa.time32("s"),
                "TIME",
                [0, 86399, None],
                "time without time zone",
                "time_send",
                ["0000000000000000", "000000141dc81dc0", None],
                False,
            ),
            (
                pa.time64("ns"),
                "TIME",
                [1000, None],
                "time without time zone",
                "time_send",
                ["0000000000000001", None],
                False,
            ),
            (
                pa.timestamp("ms"),
                "TIMESTAMP",
                [946684800000, 0, -1, None],
                "timestamp without time zone",
                "timestamp_send",
                ["0000000000000000", "fffca2fec4c82000", "fffca2fec4c81c18", None],
                False,
            ),
            (
                pa.timestamp("us", "Asia/Kathmandu"),
                "TIMESTAMPTZ",
                [946684800000001, None],
                "timestamp with time zone",
                "timestamptz_send",
                ["0000000000000001", None],
                False,
            ),
            (
                pa.int64(),
                "TIME",
                [86400000000, None],
                "time without time zone",
                "time_send",
                ["000000141dd76000", None],
                True,
            ),
            (
                pa.int64(),
                "TIMESTAMP",
                [-1, 0, None],
                "timestamp without time zone",
                "timestamp_send",
                ["ffffffffffffffff", "0000000000000000", None],
                True,
            ),
            (
                pa.int64(),
                "TIMESTAMPTZ",
                [-1, 0, None],
                "timestamp with time zone",
                "timestamptz_send",
                ["ffffffffffffffff", "0000000000000000", None],
                True,
            ),
        ]
        for arrow_type, family, values, kind, send, wire, lossless in cases:
            data = batch(arrow_type, values, family, lossless=lossless)
            for stream in (False, True):
                with adm.AdbcStatement(self.connection) as stmt:
                    stmt.set_sql_query(
                        "SELECT pg_typeof($1)::text AS kind,"
                        f"encode({send}($1),'hex') AS wire"
                    )
                    if stream:
                        stmt.bind_stream(
                            pa.RecordBatchReader.from_batches(
                                data.schema,
                                [data.slice(0, 0), data.slice(0, 1), data.slice(1)],
                            )
                        )
                    else:
                        stmt.bind(data)
                    handle, _ = stmt.execute_query()
                    with pa.RecordBatchReader._import_from_c(handle.address) as reader:
                        self.assertEqual(
                            reader.read_all().to_pylist(),
                            [{"kind": kind, "wire": x} for x in wire],
                        )

    def test_explicit_create_reordered_subset_append_and_actual_catalog(self):
        schema = pa.schema(
            [
                field("d", pa.date32(), "DATE"),
                field("t", pa.time64("us"), "TIME"),
                field("ts", pa.timestamp("us"), "TIMESTAMP"),
                field("tz", pa.timestamp("us", "UTC"), "TIMESTAMPTZ"),
            ]
        )
        data = pa.RecordBatch.from_arrays(
            [
                pa.array([10957], pa.date32()),
                pa.array([1000], pa.time64("us")),
                pa.array([946684800000001], pa.timestamp("us")),
                pa.array([946684800000002], pa.timestamp("us", "UTC")),
            ],
            schema=schema,
        )
        self.assertEqual(self.ingest("adbc_e1_python_values", "create", data), 1)
        self.assertEqual(
            self.query(
                "SELECT attname,format_type(atttypid,atttypmod) AS kind FROM "
                "pg_attribute WHERE attrelid='adbc_e1_python_values'::regclass "
                "AND attnum>0 AND NOT attisdropped ORDER BY attnum"
            ),
            [
                {"attname": "d", "kind": "date"},
                {"attname": "t", "kind": "time without time zone"},
                {"attname": "ts", "kind": "timestamp without time zone"},
                {"attname": "tz", "kind": "timestamp with time zone"},
            ],
        )
        reverse_schema = pa.schema(
            [
                field("tz", pa.int64(), "TIMESTAMPTZ", lossless=True),
                field("d", pa.date32(), "DATE"),
            ]
        )
        reverse = pa.RecordBatch.from_arrays(
            [pa.array([3]), pa.array([0], pa.date32())], schema=reverse_schema
        )
        self.assertEqual(self.ingest("adbc_e1_python_values", "append", reverse), 1)
        self.assertEqual(
            self.ingest("adbc_e1_python_values", "create_append", reverse), 1
        )
        self.assertEqual(
            self.query(
                "SELECT encode(timestamptz_send(tz),'hex') AS wire FROM "
                "adbc_e1_python_values WHERE ts IS NULL"
            ),
            [{"wire": "0000000000000003"}] * 2,
        )
        schema_handle = self.connection.get_table_schema(
            None, None, "adbc_e1_python_values"
        )
        self.assertEqual(
            pa.Schema._import_from_c(schema_handle.address).names,
            ["d", "t", "ts", "tz"],
        )

    def test_preddl_precision_and_closed_identity_preserve_replace_sentinel(self):
        self.execute("CREATE TEMP TABLE adbc_e1_python_guard(amount INTEGER)")
        self.execute("INSERT INTO adbc_e1_python_guard VALUES(42)")
        invalid = [
            batch(pa.timestamp("us"), [946684800000000], "TIMESTAMP", 3),
            batch(pa.time64("us"), [1000], "TIME", 0),
        ]
        base = batch(pa.int64(), [1], "TIME", lossless=True)
        for key, value in [
            (b"ARROW:extension:name", b"adbc.redshift.time_us.v2"),
            (b"ARROW:extension:metadata", b"{}"),
            (b"ADBC:redshift:sql_type", b"TIMESTAMP"),
        ]:
            metadata = dict(base.schema.field(0).metadata)
            metadata[key] = value
            invalid.append(
                base.replace_schema_metadata(None).cast(
                    pa.schema([pa.field("amount", pa.int64(), metadata=metadata)])
                )
            )
        dictionary = pa.array([1]).dictionary_encode()
        invalid.append(
            pa.RecordBatch.from_arrays(
                [dictionary],
                schema=pa.schema(
                    [field("amount", dictionary.type, "TIME", lossless=True)]
                ),
            )
        )
        for data in invalid:
            for stream in (False, True):
                with self.assertRaises(adm.Error):
                    source = (
                        pa.RecordBatchReader.from_batches(data.schema, [data])
                        if stream
                        else data
                    )
                    self.ingest("adbc_e1_python_guard", "replace", source, stream)
                self.assertEqual(
                    self.query("SELECT amount FROM adbc_e1_python_guard"),
                    [{"amount": 42}],
                )
        # An actual existing target may honor source p3 exactly; the interim DDL
        # syntax gate must not silently erase that p3 on create_append's CREATE.
        with self.assertRaises(adm.Error):
            self.ingest("adbc_e1_python_new_precision", "create_append", invalid[0])
        self.assertEqual(
            self.query(
                "SELECT to_regclass('pg_temp.adbc_e1_python_new_precision') "
                "IS NULL AS absent"
            ),
            [{"absent": True}],
        )

    def test_known_target_precision_rejects_late_batch_and_rolls_back_owned_stream(
        self,
    ):
        self.execute("CREATE TEMP TABLE adbc_e1_python_precision(amount TIMESTAMP(3))")
        self.execute(
            "INSERT INTO adbc_e1_python_precision VALUES(TIMESTAMP '2000-01-01')"
        )
        data = batch(
            pa.timestamp("us"), [946684800001000, 946684800001001], "TIMESTAMP", 6
        )
        source = pa.RecordBatchReader.from_batches(
            data.schema, [data.slice(0, 0), data.slice(0, 1), data.slice(1)]
        )
        with self.assertRaises(adm.Error):
            self.ingest("adbc_e1_python_precision", "append", source, True)
        self.assertEqual(
            self.query(
                "SELECT encode(timestamp_send(amount),'hex') AS wire FROM "
                "adbc_e1_python_precision"
            ),
            [{"wire": "0000000000000000"}],
        )
        self.assertEqual(
            self.ingest(
                "adbc_e1_python_precision",
                "append",
                batch(pa.timestamp("us"), [946684800001000], "TIMESTAMP", 3),
            ),
            1,
        )
        with self.assertRaises(adm.Error):
            self.ingest(
                "adbc_e1_python_precision",
                "append",
                batch(pa.timestamp("us", "UTC"), [946684800001000], "TIMESTAMPTZ"),
            )

    def test_create_append_existing_precision_temp_permanent_shadow_and_missing(self):
        self.execute('CREATE TEMP TABLE "MixedTemporal"(amount TIMESTAMP(3))')
        self.execute("""INSERT INTO "MixedTemporal" VALUES(TIMESTAMP '2000-01-01')""")
        self.execute(
            "CREATE TEMP TABLE adbc_e1_python_mixed("
            "t TIMESTAMP(3),c CHAR(4),n NUMERIC(8,2))"
        )
        mixed = pa.RecordBatch.from_arrays(
            [
                pa.array([946684800001000], pa.timestamp("us")),
                pa.array(["x"]),
                pa.array(["1.25"]),
            ],
            schema=pa.schema(
                [
                    field("t", pa.timestamp("us"), "TIMESTAMP", 3),
                    field("c", pa.string(), "CHAR"),
                    field("n", pa.string(), "DECIMAL"),
                ]
            ),
        )
        self.assertEqual(self.ingest("adbc_e1_python_mixed", "create_append", mixed), 1)
        self.assertEqual(
            self.query(
                "SELECT encode(bpcharsend(c),'hex') AS c,n::text AS n FROM "
                "adbc_e1_python_mixed"
            ),
            [{"c": "78202020", "n": "1.25"}],
        )
        self.execute("CREATE SCHEMA adbc_e1_python_shadow")
        try:
            self.execute(
                'CREATE TABLE adbc_e1_python_shadow."MixedTemporal"'
                "(amount TIMESTAMP(2))"
            )
            exact = batch(pa.timestamp("us"), [946684800001000], "TIMESTAMP", 3)
            self.assertEqual(self.ingest("MixedTemporal", "create_append", exact), 1)
            for invalid in [
                batch(pa.timestamp("us"), [946684800001001], "TIMESTAMP", 3),
                batch(pa.timestamp("us", "UTC"), [946684800001000], "TIMESTAMPTZ", 3),
            ]:
                with self.assertRaises(adm.Error):
                    self.ingest("MixedTemporal", "create_append", invalid)
            self.assertEqual(
                self.query('SELECT count(*) AS n FROM "MixedTemporal"'), [{"n": 2}]
            )
            with self.assertRaises(adm.Error):
                self.ingest(
                    "MixedTemporal",
                    "create_append",
                    exact,
                    temporary=False,
                    db_schema="adbc_e1_python_shadow",
                )
            target_exact = batch(pa.timestamp("us"), [946684800010000], "TIMESTAMP", 3)
            self.assertEqual(
                self.ingest(
                    "MixedTemporal",
                    "create_append",
                    target_exact,
                    temporary=False,
                    db_schema="adbc_e1_python_shadow",
                ),
                1,
            )
            self.assertEqual(
                self.query(
                    "SELECT encode(timestamp_send(amount),'hex') AS wire FROM "
                    'adbc_e1_python_shadow."MixedTemporal"'
                ),
                [{"wire": "0000000000002710"}],
            )
            self.connection.set_autocommit(False)
            try:
                with self.assertRaises(adm.Error):
                    self.ingest(
                        "adbc_e1_python_absent_precision", "create_append", exact
                    )
                # Missing target rejection is a client pre-DDL gate, not a SQL
                # missing-regclass error that aborts a caller transaction.
                self.assertEqual(self.query("SELECT 42 AS alive"), [{"alive": 42}])
                self.assertEqual(
                    self.query(
                        "SELECT to_regclass('pg_temp.adbc_e1_python_absent_precision') "
                        "IS NULL AS absent"
                    ),
                    [{"absent": True}],
                )
                self.connection.rollback()
            finally:
                self.connection.set_autocommit(True)
        finally:
            self.execute('DROP TABLE IF EXISTS adbc_e1_python_shadow."MixedTemporal"')
            self.execute("DROP SCHEMA adbc_e1_python_shadow")

    def test_source_precision_submicrosecond_and_caller_owned_transaction(self):
        self.execute("CREATE TEMP TABLE adbc_e1_python_caller(amount TIME(6))")
        self.connection.set_autocommit(False)
        try:
            self.execute("INSERT INTO adbc_e1_python_caller VALUES(TIME '00:00:02')")
            data = batch(pa.time64("us"), [1000, 1001], "TIME", 3)
            source = pa.RecordBatchReader.from_batches(
                data.schema, [data.slice(0, 1), data.slice(1)]
            )
            # Prepared INSERT validates each offered value before its query.
            # A later client conversion failure leaves earlier rows and the
            # caller's own work in that caller-owned transaction.
            with adm.AdbcStatement(self.connection) as stmt:
                stmt.set_sql_query("INSERT INTO adbc_e1_python_caller VALUES($1)")
                stmt.bind_stream(source)
                with self.assertRaises(adm.Error):
                    stmt.execute_update()
            self.assertEqual(
                self.query(
                    "SELECT encode(time_send(amount),'hex') AS wire FROM "
                    "adbc_e1_python_caller ORDER BY amount"
                ),
                [{"wire": "00000000000003e8"}, {"wire": "00000000001e8480"}],
            )
            self.connection.rollback()
        finally:
            self.connection.set_autocommit(True)
        self.assertEqual(
            self.query("SELECT count(*) AS n FROM adbc_e1_python_caller"), [{"n": 0}]
        )
        # PostgreSQL COPY is a different transport: CopyFail aborts the caller
        # transaction, and the driver must not roll back that caller's work.
        self.connection.set_autocommit(False)
        try:
            self.execute("INSERT INTO adbc_e1_python_caller VALUES(TIME '00:00:02')")
            source = pa.RecordBatchReader.from_batches(
                data.schema, [data.slice(0, 1), data.slice(1)]
            )
            with self.assertRaises(adm.Error):
                self.ingest("adbc_e1_python_caller", "append", source, True)
            with self.assertRaises(adm.Error):
                self.query("SELECT 42 AS blocked_until_caller_rollback")
            self.connection.rollback()
        finally:
            self.connection.set_autocommit(True)
        self.assertEqual(
            self.query("SELECT count(*) AS n FROM adbc_e1_python_caller"), [{"n": 0}]
        )
        for arrow_type, family, value in [
            (pa.time64("ns"), "TIME", 1001),
            (pa.date64(), "DATE", -1),
        ]:
            with adm.AdbcStatement(self.connection) as stmt:
                stmt.set_sql_query("SELECT $1")
                stmt.bind(batch(arrow_type, [value], family))
                with self.assertRaises(adm.Error):
                    stmt.execute_query()

    def test_empty_null_and_unconfigured_postgres_output_remain_ordinary(self):
        for data in [
            batch(pa.timestamp("us"), [], "TIMESTAMP"),
            batch(pa.timestamp("us"), [None], "TIMESTAMP"),
        ]:
            with adm.AdbcStatement(self.connection) as stmt:
                stmt.set_sql_query("SELECT $1 AS amount")
                stmt.bind_stream(pa.RecordBatchReader.from_batches(data.schema, [data]))
                handle, _ = stmt.execute_query()
                with pa.RecordBatchReader._import_from_c(handle.address) as reader:
                    table = reader.read_all()
                    self.assertEqual(table.num_rows, data.num_rows)
                    self.assertEqual(table.schema.field(0).type, pa.timestamp("us"))
                    self.assertNotIn(
                        b"ADBC:redshift:sql_type", table.schema.field(0).metadata or {}
                    )
        with self.assertRaises(adm.Error):
            self.connection.set_options(
                **{"adbc.redshift.temporal_representation": "lossless"}
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
    TemporalPublicPythonTest.driver = str(driver)
    TemporalPublicPythonTest.uri = options.uri
    unittest.main(argv=[sys.argv[0]], verbosity=2)
