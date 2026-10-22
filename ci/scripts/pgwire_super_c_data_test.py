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

"""Consumer C Data/IPC and production SUPER-input writer, synthetic target only."""

import argparse
import ctypes
import importlib.util
import pathlib
import struct
import unittest

import pyarrow as pa

_definition = pathlib.Path(__file__).with_name("pgwire_interval_c_data_test.py")
_spec = importlib.util.spec_from_file_location("_super_c_definitions", _definition)
_definitions = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_definitions)
ArrowArray, ArrowSchema = _definitions.ArrowArray, _definitions.ArrowSchema

METADATA = {
    b"ARROW:extension:name": b"adbc.redshift.super.json",
    b"ARROW:extension:metadata": (
        b'{"version":1,"encoding":"utf8-json","scope":"json-subset-write"}'
    ),
    b"ADBC:redshift:sql_type": b"SUPER",
}


def super_field(name="amount"):
    return pa.field(name, pa.large_string(), metadata=METADATA)


class SuperCDataTest(unittest.TestCase):
    def test_native_result_marker_never_becomes_write_input_even_null(self):
        metadata = {
            b"ARROW:extension:name": b"adbc.redshift.super.native_json",
            b"ARROW:extension:metadata": (
                b'{"version":1,"encoding":"redshift-json-wire",'
                b'"scope":"native-result","numbers":"lexemes",'
                b'"nulls":"wire-null"}'
            ),
            b"ADBC:redshift:sql_type": b"SUPER",
        }
        field = pa.field("amount", pa.large_string(), metadata=metadata)
        self.assertNotEqual(self.encode([None], field=field)[0], 0)

    def encode(self, values, field=None, sliced=False):
        field = field or super_field()
        batch = pa.RecordBatch.from_arrays(
            [pa.array(values, type=field.type)], schema=pa.schema([field])
        )
        if sliced:
            batch = batch.slice(1, len(values) - 1)
        array, schema, output, output_schema = (
            ArrowArray(),
            ArrowSchema(),
            ArrowArray(),
            ArrowSchema(),
        )
        batch._export_to_c(ctypes.addressof(array), ctypes.addressof(schema))
        try:
            code = self.write(
                ctypes.byref(array),
                ctypes.byref(schema),
                ctypes.byref(output),
                ctypes.byref(output_schema),
            )
            if code:
                return code, None
            result = pa.RecordBatch._import_from_c(
                ctypes.addressof(output), ctypes.addressof(output_schema)
            )
            return 0, result.column(0).to_pylist()
        finally:
            for value in (array, schema, output, output_schema):
                if value.release:
                    ctypes.CFUNCTYPE(None, ctypes.POINTER(type(value)))(value.release)(
                        ctypes.byref(value)
                    )

    def test_wire_preserves_json_null_sql_null_and_numeric_tokens(self):
        values = [
            None,
            "null",
            '""',
            '{"é":123456789012345678901234567890.1200}',
            "1e999999999",
        ]
        code, wire = self.encode(values)
        self.assertEqual(code, 0)
        self.assertEqual(
            wire,
            [None]
            + [struct.pack("!i", len(v.encode())) + v.encode() for v in values[1:]],
        )

    def test_parent_slice_and_empty_batch(self):
        self.assertEqual(
            self.encode(["true", "null", None], sliced=True),
            (0, [b"\x00\x00\x00\x04null", None]),
        )
        self.assertEqual(self.encode([]), (0, []))

    def test_duplicate_unicode_and_bad_document_reject(self):
        for text in [
            '{"a":1,"\\u0061":2}',
            '{"😀":1,"\\ud83d\\ude00":2}',
            "",
            "[1,]",
            '"\\ud800"',
        ]:
            self.assertNotEqual(self.encode([text])[0], 0)

    def test_wrong_storage_version_or_family_rejects_even_null(self):
        for key, value in [
            (b"ARROW:extension:name", b"adbc.redshift.super.json.v2"),
            (b"ADBC:redshift:sql_type", b"VARCHAR"),
            (b"ARROW:extension:metadata", b'{"version":2}'),
        ]:
            metadata = dict(METADATA)
            metadata[key] = value
            self.assertNotEqual(
                self.encode(
                    [None], pa.field("amount", pa.large_string(), metadata=metadata)
                )[0],
                0,
            )
        self.assertNotEqual(
            self.encode([None], pa.field("amount", pa.string(), metadata=METADATA))[0],
            0,
        )

    def test_metadata_survives_ipc_without_native_origin_claim(self):
        batch = pa.RecordBatch.from_arrays(
            [pa.array(["null", None], type=pa.large_string())],
            schema=pa.schema([super_field()]),
        )
        sink = pa.BufferOutputStream()
        with pa.ipc.new_stream(sink, batch.schema) as writer:
            writer.write_batch(batch)
        restored = pa.ipc.open_stream(sink.getvalue()).read_next_batch()
        self.assertEqual(restored.schema, batch.schema)
        self.assertEqual(restored.to_pylist(), batch.to_pylist())


class SuperNativeCDataTest(unittest.TestCase):
    def decode(self, mode):
        array, schema = ArrowArray(), ArrowSchema()
        code = self.read(mode, ctypes.byref(array), ctypes.byref(schema))
        if code:
            self.assertFalse(array.release)
            self.assertFalse(schema.release)
            return code, None
        return code, pa.RecordBatch._import_from_c(
            ctypes.addressof(array), ctypes.addressof(schema)
        )

    def test_mixed_native_lexemes_wire_nulls_and_ipc(self):
        code, batch = self.decode(0)
        self.assertEqual(code, 0)
        self.assertEqual(batch.column(0).to_pylist(), list(range(1, 20)))
        expected = [
            None,
            None,
            "true",
            '""',
            '"é🙂\\u0001"',
            "[]",
            "{}",
            "9007199254740993",
            "1.2200",
            "1.22",
            '{"n":1.2200}',
            '{"n":1.22}',
            '{"other":1}',
            '"null"',
            "1",
            '[{"n":1.2200},{"n":1.22}]',
            "null",
            '"null"',
            '""',
        ]
        self.assertEqual(batch.column(1).to_pylist(), expected)
        field = batch.schema.field(1)
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
        self.assertEqual(field.metadata[b"ADBC:redshift:type_modifier"], b"16384000")
        sink = pa.BufferOutputStream()
        with pa.ipc.new_stream(sink, batch.schema) as writer:
            writer.write_batch(batch)
        restored = pa.ipc.open_stream(sink.getvalue()).read_next_batch()
        self.assertEqual(restored.schema, batch.schema)
        self.assertEqual(restored.to_pylist(), batch.to_pylist())

    def test_empty_and_all_null_schema_stable(self):
        _, batch = self.decode(0)
        for mode in (1, 2):
            code, other = self.decode(mode)
            self.assertEqual(code, 0)
            self.assertEqual(other.schema, batch.schema)
            self.assertEqual(other.column(1).to_pylist(), [] if mode == 1 else [None])

    def test_malformed_native_output_has_no_partial_export(self):
        code, batch = self.decode(3)
        self.assertNotEqual(code, 0)
        self.assertIsNone(batch)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", required=True, type=pathlib.Path)
    args = parser.parse_args()
    library = ctypes.CDLL(str(args.fixture.resolve(strict=True)))
    SuperCDataTest.write = staticmethod(library.AdbcRedshiftSuperWriteFixture)
    SuperCDataTest.write.argtypes = [
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
    ]
    SuperCDataTest.write.restype = ctypes.c_int
    SuperNativeCDataTest.read = staticmethod(library.AdbcRedshiftSuperReadFixture)
    SuperNativeCDataTest.read.argtypes = [
        ctypes.c_int,
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
    ]
    SuperNativeCDataTest.read.restype = ctypes.c_int
    unittest.main(argv=[__file__], verbosity=2)
