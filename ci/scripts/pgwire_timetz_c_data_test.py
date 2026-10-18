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

"""Installed PyArrow consumer of TIMETZ reference codec C Data, never native proof."""

import argparse
import ctypes
import importlib.util
import struct
import unittest
from pathlib import Path

import pyarrow as pa

_types_spec = importlib.util.spec_from_file_location(
    "_timetz_c_data_types",
    Path(__file__).resolve().with_name("pgwire_interval_c_data_test.py"),
)
if _types_spec is None or _types_spec.loader is None:
    raise RuntimeError("Cannot load the exact sibling C Data test definitions")
_types = importlib.util.module_from_spec(_types_spec)
_types_spec.loader.exec_module(_types)
ArrowArray, ArrowSchema = _types.ArrowArray, _types.ArrowSchema
PREFIX = b"ADBC:redshift:"
NAME = b"adbc.redshift.timetz_us_offset.v1"
METADATA = (
    b'{"version":1,"family":"TIMETZ","time_unit":"us","origin":"start_of_day",'
    b'"offset_unit":"s","offset_sign":"east_of_utc"}'
)
VALUES = [
    {"time_us": 0, "offset_seconds": 0},
    {"time_us": 19800000000, "offset_seconds": 19800},
    {"time_us": 1001, "offset_seconds": -30},
    {"time_us": 86400000000, "offset_seconds": 20700},
    {"time_us": 1, "offset_seconds": 2147483647},
    {"time_us": 1, "offset_seconds": -2147483647},
    None,
]


def release(*values):
    for value in values:
        if value.release:
            ctypes.CFUNCTYPE(None, ctypes.POINTER(type(value)))(value.release)(
                ctypes.byref(value)
            )


class TimetzCDataTest(unittest.TestCase):
    def batch(self, number=0):
        array, schema = ArrowArray(), ArrowSchema()
        self.assertEqual(
            self.produce(number, ctypes.byref(array), ctypes.byref(schema)), 0
        )
        try:
            return pa.RecordBatch._import_from_c(
                ctypes.addressof(array), ctypes.addressof(schema)
            )
        finally:
            release(array, schema)

    def encode(self, batch, target=-2):
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
                target,
                ctypes.byref(output),
                ctypes.byref(output_schema),
            )
            if code:
                return code, None
            return 0, pa.RecordBatch._import_from_c(
                ctypes.addressof(output), ctypes.addressof(output_schema)
            ).column(0).to_pylist()
        finally:
            release(array, schema, output, output_schema)

    def project(self, batch, column=0, metadata=None):
        field = batch.schema.field(column).with_name("amount")
        if metadata is not None:
            field = field.with_metadata(metadata)
        return pa.RecordBatch.from_arrays(
            [batch.column(column)], schema=pa.schema([field])
        )

    def test_codec_produced_pair_metadata_nulls_modes_ipc_and_slices(self):
        for number in (0, 3):
            batch = self.batch(number)
            self.assertEqual(batch.column(0).to_pylist(), VALUES)
            field = batch.schema.field(0)
            self.assertEqual(field.metadata[b"ARROW:extension:name"], NAME)
            self.assertEqual(field.metadata[b"ARROW:extension:metadata"], METADATA)
            self.assertEqual(field.metadata[PREFIX + b"sql_type"], b"TIMETZ")
            self.assertEqual(field.metadata[PREFIX + b"fractional_precision"], b"6")
            self.assertEqual(
                field.type[0], pa.field("time_us", pa.int64(), nullable=False)
            )
            self.assertEqual(
                field.type[1], pa.field("offset_seconds", pa.int32(), nullable=False)
            )
            self.assertNotIn(
                PREFIX + b"fractional_precision", batch.schema.field(1).metadata
            )
            self.assertEqual(
                batch.schema.field(1).metadata[PREFIX + b"type_modifier"], b"999"
            )
            self.assertEqual(
                batch.schema.field(2).metadata[PREFIX + b"fractional_precision"], b"3"
            )
            sink = pa.BufferOutputStream()
            with pa.ipc.new_stream(sink, batch.schema) as writer:
                writer.write_batch(batch)
            restored = pa.ipc.open_stream(sink.getvalue()).read_next_batch()
            self.assertTrue(batch.equals(restored))
            self.assertTrue(batch.schema.equals(restored.schema, check_metadata=True))
            expected = [
                None
                if value is None
                else struct.pack(">iqi", 12, value["time_us"], -value["offset_seconds"])
                for value in VALUES
            ]
            projected = self.project(restored)
            self.assertEqual(self.encode(projected), (0, expected))
            self.assertEqual(self.encode(projected.slice(1, 4)), (0, expected[1:5]))
            # Parent SQL NULL has canonical child placeholders, not a valid zero pair.
            self.assertEqual(batch.column(0).field(0)[6].as_py(), 0)
            self.assertEqual(batch.column(0).field(1)[6].as_py(), 0)

    def test_unknown_read_precision_is_not_an_implicit_write_assertion(self):
        batch = self.batch()
        unknown = self.project(batch, 1)
        self.assertNotEqual(self.encode(unknown)[0], 0)
        metadata = dict(unknown.schema.field(0).metadata)
        metadata[PREFIX + b"fractional_precision"] = b"6"
        # Unsupported raw999 remains diagnostic; adding p6 cannot erase it.
        self.assertNotEqual(self.encode(self.project(unknown, metadata=metadata))[0], 0)
        del metadata[PREFIX + b"type_modifier"]
        asserted = self.project(unknown, metadata=metadata)
        self.assertEqual(self.encode(asserted)[0], 0)

    def test_known_source_and_target_precisions_are_independent(self):
        batch = self.project(self.batch(), 2)
        expected = [struct.pack(">iqi", 12, 1000, 30)] * 6 + [None]
        for target in range(7):
            code, wire = self.encode(batch, target)
            self.assertEqual(code == 0, target >= 3)
            if code == 0:
                self.assertEqual(wire, expected)
        metadata = dict(batch.schema.field(0).metadata)
        metadata[PREFIX + b"fractional_precision"] = b"2"
        del metadata[PREFIX + b"type_modifier"]
        self.assertNotEqual(
            self.encode(self.project(batch, metadata=metadata), 6)[0], 0
        )

    def test_null_empty_and_poisoned_children_require_known_actual_target(self):
        for number in (1, 2):
            batch = self.project(self.batch(number))
            self.assertEqual(self.encode(batch, 3)[0], 0)
            for target in (-1, 999):
                self.assertNotEqual(self.encode(batch, target)[0], 0)
        source = self.project(self.batch(2))
        storage = pa.StructArray.from_arrays(
            [
                pa.array([None], type=pa.int64()),
                pa.array([-2147483648], type=pa.int32()),
            ],
            fields=list(source.schema.field(0).type),
            mask=pa.array([True]),
        )
        batch = pa.RecordBatch.from_arrays([storage], schema=source.schema)
        self.assertEqual(self.encode(batch, 3), (0, [None]))
        # Identical child data under a valid parent must reject, not become zero.
        storage = pa.StructArray.from_arrays(
            list(storage.flatten()), fields=list(source.schema.field(0).type)
        )
        batch = pa.RecordBatch.from_arrays([storage], schema=source.schema)
        self.assertNotEqual(self.encode(batch, 3)[0], 0)
        nulls = pa.RecordBatch.from_arrays([pa.nulls(2)], names=["amount"])
        self.assertEqual(self.encode(nulls, 3), (0, [None, None]))
        for target in (-1, 999):
            self.assertNotEqual(self.encode(nulls, target)[0], 0)

    def test_wrong_schema_precision_versions_and_runtime_domain_reject(self):
        source = self.project(self.batch())
        original = source.schema.field(0)
        for key, value in [
            (b"ARROW:extension:name", b"adbc.redshift.timetz_us_offset.v2"),
            (b"ARROW:extension:metadata", b"{}"),
            (PREFIX + b"sql_type", b"TIME"),
            (PREFIX + b"fractional_precision", b"06"),
            (PREFIX + b"fractional_precision", b"7"),
        ]:
            metadata = dict(original.metadata)
            metadata[key] = value
            self.assertNotEqual(
                self.encode(self.project(source, metadata=metadata))[0], 0
            )
        for civil, east in [(-1, 0), (86400000001, 0), (0, -2147483648)]:
            array = pa.StructArray.from_arrays(
                [pa.array([civil], type=pa.int64()), pa.array([east], type=pa.int32())],
                fields=list(original.type),
            )
            batch = pa.RecordBatch.from_arrays([array], schema=source.schema)
            self.assertNotEqual(self.encode(batch)[0], 0)
        nullable = pa.struct(
            [
                pa.field("time_us", pa.int64()),
                pa.field("offset_seconds", pa.int32(), nullable=False),
            ]
        )
        invalid = pa.array([{"time_us": 1, "offset_seconds": 0}], type=nullable)
        batch = pa.RecordBatch.from_arrays(
            [invalid],
            schema=pa.schema(
                [pa.field("amount", nullable, metadata=original.metadata)]
            ),
        )
        self.assertNotEqual(self.encode(batch)[0], 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", required=True)
    args = parser.parse_args()
    library = ctypes.CDLL(args.fixture)
    TimetzCDataTest.produce = staticmethod(library.AdbcRedshiftTimetzFixture)
    TimetzCDataTest.produce.argtypes = [
        ctypes.c_int,
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
    ]
    TimetzCDataTest.produce.restype = ctypes.c_int
    TimetzCDataTest.write = staticmethod(library.AdbcRedshiftTimetzWriteFixture)
    TimetzCDataTest.write.argtypes = [
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
        ctypes.c_int,
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
    ]
    TimetzCDataTest.write.restype = ctypes.c_int
    unittest.main(argv=[__file__], verbosity=2)
