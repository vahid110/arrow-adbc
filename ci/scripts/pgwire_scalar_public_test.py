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

"""Installed public scalar checks on an explicitly supplied local PostgreSQL driver.

Canonical semantic metadata opts into the Redshift contract without changing the
connection identity. TEMP fixtures only; native Redshift remains unqualified.
"""

import argparse
import pathlib
import struct
import sys
import unittest
from urllib.parse import parse_qs, urlsplit

import pyarrow as pa

import adbc_driver_manager as adm


def batch(arrow_type, values, family):
    metadata = {b"ADBC:redshift:sql_type": family.encode("ascii")} if family else None
    return pa.RecordBatch.from_arrays(
        [pa.array(values, type=arrow_type)],
        schema=pa.schema([pa.field("amount", arrow_type, metadata=metadata)]),
    )


class ScalarPublicPythonTest(unittest.TestCase):
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

    def test_all_six_families_bind_array_and_stream_with_literal_wire(self):
        cases = [
            (
                pa.int64(),
                "SMALLINT",
                "smallint",
                "int2send",
                [-32768, 32767, None],
                ["8000", "7fff", None],
            ),
            (
                pa.int64(),
                "INTEGER",
                "integer",
                "int4send",
                [-2147483648, 2147483647, None],
                ["80000000", "7fffffff", None],
            ),
            (
                pa.uint64(),
                "BIGINT",
                "bigint",
                "int8send",
                [0, 2**63 - 1, None],
                ["0000000000000000", "7fffffffffffffff", None],
            ),
            (
                pa.float64(),
                "REAL",
                "real",
                "float4send",
                [1.25, -0.0, None],
                ["3fa00000", "80000000", None],
            ),
            (
                pa.float64(),
                "DOUBLE PRECISION",
                "double precision",
                "float8send",
                [float.fromhex("0x1.fffffffffffffp+1023"), -0.0, None],
                ["7fefffffffffffff", "8000000000000000", None],
            ),
            (
                pa.bool_(),
                "BOOLEAN",
                "boolean",
                "boolsend",
                [True, False, None],
                ["01", "00", None],
            ),
        ]
        for arrow_type, family, kind, send, values, expected in cases:
            data = batch(arrow_type, values, family)
            for stream_bind in (False, True):
                with adm.AdbcStatement(self.connection) as stmt:
                    stmt.set_sql_query(
                        "SELECT pg_typeof($1)::text AS kind,"
                        f"encode({send}($1),'hex') AS wire"
                    )
                    if stream_bind:
                        stmt.bind_stream(
                            pa.RecordBatchReader.from_batches(
                                data.schema,
                                [data.slice(0, 0), data.slice(0, 1), data.slice(1, 2)],
                            )
                        )
                    else:
                        stmt.bind(data)
                    handle, _ = stmt.execute_query()
                    with pa.RecordBatchReader._import_from_c(handle.address) as reader:
                        self.assertEqual(
                            reader.read_all().to_pylist(),
                            [{"kind": kind, "wire": wire} for wire in expected],
                        )
                    self.assertEqual(
                        data.schema.field(0).metadata[b"ADBC:redshift:sql_type"],
                        family.encode(),
                    )

    def test_raw_sliced_unsigned_high_bit_never_wraps_to_a_negative_parameter(self):
        values = pa.Array.from_buffers(
            pa.uint64(),
            3,
            [None, pa.py_buffer(struct.pack("=QQQ", 42, 2**63, 2**64 - 1))],
        )
        schema = pa.schema(
            [
                pa.field(
                    "amount",
                    pa.uint64(),
                    metadata={b"ADBC:redshift:sql_type": b"BIGINT"},
                )
            ]
        )
        for offset in (1, 2):
            data = pa.RecordBatch.from_arrays([values.slice(offset, 1)], schema=schema)
            for stream_bind in (False, True):
                with adm.AdbcStatement(self.connection) as stmt:
                    stmt.set_sql_query("SELECT $1::text")
                    if stream_bind:
                        stmt.bind_stream(
                            pa.RecordBatchReader.from_batches(schema, [data])
                        )
                    else:
                        stmt.bind(data)
                    with self.assertRaises(adm.Error):
                        handle, _ = stmt.execute_query()
                        with pa.RecordBatchReader._import_from_c(
                            handle.address
                        ) as reader:
                            reader.read_all()
        self.assertEqual(self.query("SELECT 42 AS value"), [{"value": 42}])

    def test_create_all_families_and_append_exact_reordered_subsets(self):
        fields = [
            ("s", pa.int64(), "SMALLINT", [-32768, None]),
            ("i", pa.int64(), "INTEGER", [-2147483648, None]),
            ("b", pa.uint64(), "BIGINT", [2**63 - 1, None]),
            ("r", pa.float64(), "REAL", [1.25, None]),
            ("d", pa.float64(), "DOUBLE PRECISION", [-0.0, None]),
            ("t", pa.bool_(), "BOOLEAN", [True, None]),
        ]
        data = pa.RecordBatch.from_arrays(
            [pa.array(v, type=t) for _, t, _, v in fields],
            schema=pa.schema(
                [
                    pa.field(n, t, metadata={b"ADBC:redshift:sql_type": f.encode()})
                    for n, t, f, _ in fields
                ]
            ),
        )
        self.assertEqual(self.ingest("adbc_c4_python_create", "create", data), 2)
        self.assertEqual(
            self.query(
                "SELECT format_type(atttypid,atttypmod) AS type FROM pg_attribute "
                "WHERE attrelid='adbc_c4_python_create'::regclass "
                "AND attnum>0 AND NOT attisdropped ORDER BY attnum"
            ),
            [
                {"type": t}
                for t in [
                    "smallint",
                    "integer",
                    "bigint",
                    "real",
                    "double precision",
                    "boolean",
                ]
            ],
        )
        self.assertEqual(
            self.query(
                "SELECT encode(int2send(s),'hex') AS s,"
                "encode(float4send(r),'hex') AS r,"
                "encode(float8send(d),'hex') AS d,t "
                "FROM adbc_c4_python_create WHERE s IS NOT NULL"
            ),
            [{"s": "8000", "r": "3fa00000", "d": "8000000000000000", "t": True}],
        )
        handle = self.connection.get_table_schema(None, None, "adbc_c4_python_create")
        actual = pa.Schema._import_from_c(handle.address)
        self.assertEqual(
            actual.types,
            [
                pa.int16(),
                pa.int32(),
                pa.int64(),
                pa.float32(),
                pa.float64(),
                pa.bool_(),
            ],
        )
        subset = pa.RecordBatch.from_arrays(
            [pa.array([False]), pa.array([12], type=pa.int64())],
            schema=pa.schema(
                [
                    pa.field(
                        "t",
                        pa.bool_(),
                        metadata={b"ADBC:redshift:sql_type": b"BOOLEAN"},
                    ),
                    pa.field(
                        "s", pa.int64(), metadata={b"ADBC:redshift:sql_type": b"BIGINT"}
                    ),
                ]
            ),
        )
        self.assertEqual(self.ingest("adbc_c4_python_create", "append", subset), 1)
        self.assertEqual(
            self.query("SELECT s,i,t FROM adbc_c4_python_create WHERE s=12"),
            [{"s": 12, "i": None, "t": False}],
        )

    def test_late_known_target_overflow_and_float_loss_roll_back_copy_stream(self):
        self.execute("CREATE TEMP TABLE adbc_c4_python_guard(amount SMALLINT)")
        self.execute("INSERT INTO adbc_c4_python_guard VALUES(7)")
        for arrow_type, family, values in [
            (pa.int64(), "BIGINT", [12, 32768]),
            (pa.uint64(), "BIGINT", [12, 2**64 - 1]),
        ]:
            data = batch(arrow_type, values, family)
            source = pa.RecordBatchReader.from_batches(
                data.schema, [data.slice(0, 1), data.slice(1, 1)]
            )
            with self.assertRaises(adm.Error):
                self.ingest("adbc_c4_python_guard", "append", source, True)
            self.assertEqual(
                self.query("SELECT amount FROM adbc_c4_python_guard"), [{"amount": 7}]
            )
        self.execute("CREATE TEMP TABLE adbc_c4_python_real(amount REAL)")
        self.execute("INSERT INTO adbc_c4_python_real VALUES(2.5)")
        data = batch(pa.float64(), [1.25, 1.1], "DOUBLE PRECISION")
        source = pa.RecordBatchReader.from_batches(
            data.schema, [data.slice(0, 1), data.slice(1, 1)]
        )
        with self.assertRaises(adm.Error):
            self.ingest("adbc_c4_python_real", "append", source, True)
        self.assertEqual(
            self.query(
                "SELECT encode(float4send(amount),'hex') AS wire "
                "FROM adbc_c4_python_real"
            ),
            [{"wire": "40200000"}],
        )
        # Valid source dimensions do not make cross-family implicit coercion exact.
        for data in [
            batch(pa.int64(), [1], "BIGINT"),
            batch(pa.bool_(), [True], "BOOLEAN"),
        ]:
            with self.assertRaises(adm.Error):
                self.ingest("adbc_c4_python_real", "append", data)

    def test_invalid_identity_and_dictionary_reject_before_replace_and_keep_pg(self):
        self.execute("CREATE TEMP TABLE adbc_c4_python_replace(amount INTEGER)")
        self.execute("INSERT INTO adbc_c4_python_replace VALUES(42)")
        invalid = [
            batch(pa.int64(), [1], "BOOLEAN"),
            batch(pa.float64(), [1.0], "INTEGER"),
        ]
        dictionary = pa.array(["1"]).dictionary_encode()
        invalid.append(
            pa.RecordBatch.from_arrays(
                [dictionary],
                schema=pa.schema(
                    [
                        pa.field(
                            "amount",
                            dictionary.type,
                            metadata={b"ADBC:redshift:sql_type": b"INTEGER"},
                        )
                    ]
                ),
            )
        )
        for data in invalid:
            for stream_bind in (False, True):
                with self.assertRaises(adm.Error):
                    source = (
                        pa.RecordBatchReader.from_batches(data.schema, [data])
                        if stream_bind
                        else data
                    )
                    self.ingest(
                        "adbc_c4_python_replace", "replace", source, stream_bind
                    )
                self.assertEqual(
                    self.query(
                        "SELECT amount,pg_typeof(amount)::text AS kind "
                        "FROM adbc_c4_python_replace"
                    ),
                    [{"amount": 42, "kind": "integer"}],
                )
        with adm.AdbcStatement(self.connection) as stmt:
            stmt.set_sql_query("SELECT pg_typeof($1)::text AS kind,$1::text AS value")
            stmt.bind(pa.RecordBatch.from_arrays([dictionary], names=["amount"]))
            handle, _ = stmt.execute_query()
            with pa.RecordBatchReader._import_from_c(handle.address) as reader:
                self.assertEqual(
                    reader.read_all().to_pylist(), [{"kind": "text", "value": "1"}]
                )

    def test_empty_null_and_explicit_sql_cast_are_distinct_from_ingest_rules(self):
        data = batch(pa.bool_(), [], "BOOLEAN")
        self.assertEqual(self.ingest("adbc_c4_python_empty", "create", data), 0)
        self.assertEqual(
            self.ingest(
                "adbc_c4_python_empty", "append", batch(pa.null(), [None], None)
            ),
            1,
        )
        self.assertEqual(
            self.query("SELECT amount FROM adbc_c4_python_empty"), [{"amount": None}]
        )
        with adm.AdbcStatement(self.connection) as stmt:
            stmt.set_sql_query(
                "SELECT encode(float8send($1),'hex') AS offered,"
                "encode(float4send(CAST($1 AS REAL)),'hex') AS cast"
            )
            stmt.bind(batch(pa.float64(), [1.1], "DOUBLE PRECISION"))
            handle, _ = stmt.execute_query()
            with pa.RecordBatchReader._import_from_c(handle.address) as reader:
                self.assertEqual(
                    reader.read_all().to_pylist(),
                    [{"offered": "3ff199999999999a", "cast": "3f8ccccd"}],
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
    ScalarPublicPythonTest.driver = str(driver)
    ScalarPublicPythonTest.uri = options.uri
    unittest.main(argv=[sys.argv[0]], verbosity=2)
