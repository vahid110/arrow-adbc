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

"""Installed character/binary checks with an explicit own-local PostgreSQL input.

Canonical metadata opts into the shared semantic contract; this is not native
Redshift wire/size/charset qualification. All database fixtures are TEMP.
"""

import argparse
import pathlib
import struct
import sys
import unittest
from urllib.parse import parse_qs, urlsplit

import pyarrow as pa

import adbc_driver_manager as adm


def field(name, arrow_type, family, width=None):
    metadata = {b"ADBC:redshift:sql_type": family.encode()} if family else {}
    if width is not None:
        metadata[b"ADBC:redshift:byte_width"] = str(width).encode()
    return pa.field(name, arrow_type, metadata=metadata or None)


def batch(arrow_type, values, family, width=None, name="amount"):
    return pa.RecordBatch.from_arrays(
        [pa.array(values, type=arrow_type)],
        schema=pa.schema([field(name, arrow_type, family, width)]),
    )


class BytesPublicPythonTest(unittest.TestCase):
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

    def test_bind_array_and_stream_preserve_literal_wire_and_parameter_identity(self):
        cases = [
            (
                pa.string(),
                "CHAR",
                8,
                "character",
                "bpcharsend",
                ["a  ", "", None],
                ["612020", "", None],
            ),
            (
                pa.large_string(),
                "VARCHAR",
                20,
                "character varying",
                "varcharsend",
                ["😀'\\  ", "", None],
                ["f09f9880275c2020", "", None],
            ),
            (
                pa.binary(),
                "VARBYTE",
                20,
                "bytea",
                "byteasend",
                [b"\x00\x80\xff\\x", b"", None],
                ["0080ff5c78", "", None],
            ),
            (
                pa.binary(3),
                "VARBYTE",
                3,
                "bytea",
                "byteasend",
                [b"\x00\x80\xff", None],
                ["0080ff", None],
            ),
        ]
        for arrow_type, family, width, kind, send, values, wire in cases:
            data = batch(arrow_type, values, family, width)
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
            self.assertEqual(
                data.schema.field(0).metadata[b"ADBC:redshift:byte_width"],
                str(width).encode(),
            )

    def test_explicit_create_padding_raw_bytes_reordered_subset_and_catalog(self):
        schema = pa.schema(
            [
                field("fixed", pa.string(), "CHAR", 6),
                field("variable", pa.string(), "VARCHAR", 12),
                field("raw", pa.binary(), "VARBYTE", 100),
            ]
        )
        data = pa.RecordBatch.from_arrays(
            [pa.array(["a  "]), pa.array(["😀'\\  "]), pa.array([b"\x00\x80\xff"])],
            schema=schema,
        )
        self.assertEqual(self.ingest("adbc_d_python_values", "create", data), 1)
        self.assertEqual(
            self.query(
                "SELECT attname,format_type(atttypid,atttypmod) AS kind FROM "
                "pg_attribute WHERE "
                "attrelid='adbc_d_python_values'::regclass AND attnum>0 AND "
                "NOT attisdropped ORDER BY attnum"
            ),
            [
                {"attname": "fixed", "kind": "character(6)"},
                {"attname": "variable", "kind": "character varying(12)"},
                {"attname": "raw", "kind": "bytea"},
            ],
        )
        stored = self.query(
            "SELECT encode(bpcharsend(fixed),'hex') AS "
            "fixed,encode(varcharsend(variable),'hex') AS "
            "variable,encode(byteasend(raw),'hex') AS "
            "raw,octet_length(fixed) AS n FROM adbc_d_python_values"
        )
        self.assertEqual(
            stored,
            [
                {
                    "fixed": "612020202020",
                    "variable": "f09f9880275c2020",
                    "raw": "0080ff",
                    "n": 6,
                }
            ],
        )
        # Select native CHAR directly: conversion to VARCHAR hides its padding.
        self.assertEqual(
            self.query("SELECT fixed FROM adbc_d_python_values"), [{"fixed": "a     "}]
        )
        reverse = pa.RecordBatch.from_arrays(
            [pa.array([b"\xff\x00"]), pa.array(["z"])],
            schema=pa.schema(
                [
                    field("raw", pa.binary(), "VARBYTE", 20),
                    field("variable", pa.string(), "VARCHAR", 30),
                ]
            ),
        )
        self.assertEqual(self.ingest("adbc_d_python_values", "append", reverse), 1)
        self.assertEqual(
            self.query(
                "SELECT fixed,variable,encode(raw,'hex') AS raw FROM "
                "adbc_d_python_values WHERE fixed IS NULL"
            ),
            [{"fixed": None, "variable": "z", "raw": "ff00"}],
        )
        self.assertEqual(
            self.ingest("adbc_d_python_values", "create_append", reverse), 1
        )
        self.assertEqual(
            self.query("SELECT count(*) AS n FROM adbc_d_python_values"), [{"n": 3}]
        )
        table_schema = self.connection.get_table_schema(
            None, None, "adbc_d_python_values"
        )
        imported = pa.Schema._import_from_c(table_schema.address)
        self.assertEqual(imported.names, ["fixed", "variable", "raw"])
        self.assertEqual(
            [f.type for f in imported], [pa.string(), pa.string(), pa.binary()]
        )
        with adm.AdbcStatement(self.connection) as stmt:
            stmt.set_sql_query(
                "SELECT fixed,variable,raw FROM adbc_d_python_values WHERE FALSE"
            )
            handle = stmt.execute_schema()
            self.assertEqual(
                pa.Schema._import_from_c(handle.address).names, imported.names
            )
            output, _ = stmt.execute_query()
            with pa.RecordBatchReader._import_from_c(output.address) as reader:
                self.assertEqual(reader.read_all().num_rows, 0)

    def test_late_value_failure_rolls_back_whole_owned_stream_without_trimming(self):
        self.execute(
            "CREATE TEMP TABLE adbc_d_python_late(amount VARCHAR(4)); "
            "INSERT INTO adbc_d_python_late VALUES('sent')"
        )
        first = batch(pa.string(), ["ok"], "VARCHAR", 20)
        for bad in ("abc  ", "😀x", "a\0b"):
            last = batch(pa.string(), [bad], "VARCHAR", 20)
            data = pa.RecordBatchReader.from_batches(first.schema, [first, last])
            with self.assertRaises(adm.Error):
                self.ingest("adbc_d_python_late", "append", data, True)
            self.assertEqual(
                self.query(
                    "SELECT encode(varcharsend(amount),'hex') AS value FROM "
                    "adbc_d_python_late"
                ),
                [{"value": "73656e74"}],
            )
        malformed = pa.Array.from_buffers(
            pa.string(),
            1,
            [
                None,
                pa.py_buffer(struct.pack("=ii", 0, 3)),
                pa.py_buffer(b"\xed\xa0\x80"),
            ],
        )
        data = pa.RecordBatch.from_arrays([malformed], schema=first.schema)
        for stream in (False, True):
            with self.assertRaises(adm.Error):
                self.ingest(
                    "adbc_d_python_late",
                    "append",
                    pa.RecordBatchReader.from_batches(data.schema, [first, data])
                    if stream
                    else data,
                    stream,
                )
        self.assertEqual(
            self.query("SELECT amount FROM adbc_d_python_late"), [{"amount": "sent"}]
        )

    def test_schema_rejection_before_replace_preserves_sentinel_and_pg_dictionary(self):
        self.execute(
            "CREATE TEMP TABLE adbc_d_python_guard(amount INTEGER); "
            "INSERT INTO adbc_d_python_guard VALUES(42)"
        )
        invalid = [
            batch(pa.binary(), [b"x"], "VARCHAR", 10),
            batch(pa.string(), ["x"], "CHAR", 0),
            batch(pa.string(), ["x"], "VARBYTE", 10),
            batch(pa.dictionary(pa.int8(), pa.string()), ["x"], "VARCHAR", 10),
        ]
        for data in invalid:
            for stream in (False, True):
                with adm.AdbcStatement(self.connection) as stmt:
                    stmt.set_options(
                        **{
                            "adbc.ingest.target_table": "adbc_d_python_guard",
                            "adbc.ingest.mode": "adbc.ingest.mode.replace",
                            "adbc.ingest.temporary": "true",
                        }
                    )
                    with self.assertRaises(adm.Error):
                        if stream:
                            stmt.bind_stream(
                                pa.RecordBatchReader.from_batches(data.schema, [data])
                            )
                        else:
                            stmt.bind(data)
                self.assertEqual(
                    self.query(
                        "SELECT amount,pg_typeof(amount)::text AS kind FROM "
                        "adbc_d_python_guard"
                    ),
                    [{"amount": 42, "kind": "integer"}],
                )
        # Unknown/oversize CREATE descriptor is detected before replacement DDL.
        for family, width in (("CHAR", None), ("VARCHAR", 70000), ("CHAR", 4097)):
            with self.assertRaises(adm.Error):
                self.ingest(
                    "adbc_d_python_guard",
                    "replace",
                    batch(pa.string(), ["x"], family, width),
                )
            self.assertEqual(
                self.query("SELECT amount FROM adbc_d_python_guard"), [{"amount": 42}]
            )
        ordinary = batch(
            pa.dictionary(pa.int8(), pa.string()), ["ordinary", None], None
        )
        with adm.AdbcStatement(self.connection) as stmt:
            stmt.set_sql_query("SELECT $1 AS value")
            stmt.bind(ordinary)
            handle, _ = stmt.execute_query()
            with pa.RecordBatchReader._import_from_c(handle.address) as reader:
                self.assertEqual(
                    reader.read_all().to_pylist(),
                    [{"value": "ordinary"}, {"value": None}],
                )

    def test_empty_null_and_provisional_binary_insert_boundary(self):
        self.assertEqual(
            self.ingest(
                "adbc_d_python_empty",
                "create",
                batch(pa.binary(), [], "VARBYTE", 16777216),
            ),
            0,
        )
        self.assertEqual(
            self.ingest(
                "adbc_d_python_empty",
                "append",
                batch(pa.binary(), [b"", None, b"\x00\xff"], "VARBYTE", 16777216),
            ),
            3,
        )
        self.assertEqual(
            self.query("SELECT encode(amount,'hex') AS value FROM adbc_d_python_empty"),
            [{"value": ""}, {"value": None}, {"value": "00ff"}],
        )
        for n in (1000000, 1000001):
            data = batch(pa.large_binary(), [b"\xff" * n], "VARBYTE", 16777216)
            if n == 1000000:
                self.assertEqual(self.ingest("adbc_d_python_empty", "append", data), 1)
                self.assertEqual(
                    self.query(
                        "SELECT octet_length(amount) AS "
                        "n,encode(substring(amount,1,1),'hex') AS "
                        "first,encode(substring(amount,octet_length(amount),1),'hex')"
                        " AS last FROM adbc_d_python_empty WHERE "
                        "octet_length(amount)=1000000"
                    ),
                    [{"n": n, "first": "ff", "last": "ff"}],
                )
            else:
                with self.assertRaises(adm.Error):
                    self.ingest("adbc_d_python_empty", "append", data)
        self.assertEqual(
            self.query("SELECT count(*) AS n FROM adbc_d_python_empty"), [{"n": 4}]
        )

    def test_marked_character_contract_rejects_non_utf8_session_without_repair(self):
        self.execute("SET client_encoding = LATIN1")
        try:
            # Explicit semantic text must reject before inserting/interpreting.
            with adm.AdbcStatement(self.connection) as stmt:
                stmt.set_sql_query("SELECT $1 AS value")
                stmt.bind(batch(pa.string(), ["é"], "VARCHAR", 20))
                with self.assertRaises(adm.Error):
                    stmt.execute_query()
            self.assertEqual(
                self.query(
                    "SELECT current_setting('client_encoding') AS client_encoding"
                ),
                [{"client_encoding": "LATIN1"}],
            )
        finally:
            self.execute("SET client_encoding = UTF8")
        with adm.AdbcStatement(self.connection) as stmt:
            stmt.set_sql_query("SELECT set_config('client_encoding','LATIN1',false),$1")
            stmt.bind(batch(pa.string(), ["x"], "VARCHAR", 20))
            with self.assertRaises(adm.Error):
                stmt.execute_query()
        # The SET happened; a failed postcheck does not undo arbitrary SQL effects.
        self.assertEqual(
            self.query("SELECT current_setting('client_encoding') AS client_encoding"),
            [{"client_encoding": "LATIN1"}],
        )
        self.execute("SET client_encoding = UTF8")


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
    BytesPublicPythonTest.driver = str(driver)
    BytesPublicPythonTest.uri = options.uri
    unittest.main(argv=[sys.argv[0]], verbosity=2)
