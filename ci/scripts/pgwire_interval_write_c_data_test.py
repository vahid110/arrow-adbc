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

"""Installed Arrow consumer of production reference codecs, never native proof."""

import argparse
import ctypes
import importlib.util
import struct
import unittest
from pathlib import Path

import pyarrow as pa

# Python -I intentionally excludes sibling imports. Load only this exact test
# definition file without changing sys.path or shadowing installed packages.
_types_spec = importlib.util.spec_from_file_location(
    "_interval_c_data_types",
    Path(__file__).resolve().with_name("pgwire_interval_c_data_test.py"),
)
if _types_spec is None or _types_spec.loader is None:
    raise RuntimeError("Cannot load the exact sibling C Data test definitions")
_types = importlib.util.module_from_spec(_types_spec)
_types_spec.loader.exec_module(_types)
ArrowArray, ArrowSchema = _types.ArrowArray, _types.ArrowSchema

QUALIFIERS = [
    "YEAR",
    "MONTH",
    "YEAR TO MONTH",
    "DAY",
    "HOUR",
    "MINUTE",
    "SECOND",
    "DAY TO HOUR",
    "DAY TO MINUTE",
    "DAY TO SECOND",
    "HOUR TO MINUTE",
    "HOUR TO SECOND",
    "MINUTE TO SECOND",
]
QUANTA = [
    12,
    1,
    1,
    86400000000,
    3600000000,
    60000000,
    1,
    3600000000,
    60000000,
    1,
    60000000,
    1,
    1,
]
PREFIX = b"ADBC:redshift:"


def release(*values):
    for value in values:
        if value.release:
            ctypes.CFUNCTYPE(None, ctypes.POINTER(type(value)))(value.release)(
                ctypes.byref(value)
            )


class IntervalWriteCDataTest(unittest.TestCase):
    def batch(self, qualifier, precision=6, batch=0):
        array, schema = ArrowArray(), ArrowSchema()
        self.assertEqual(
            self.produce(
                qualifier, precision, batch, ctypes.byref(array), ctypes.byref(schema)
            ),
            0,
        )
        try:
            return pa.RecordBatch._import_from_c(
                ctypes.addressof(array), ctypes.addressof(schema)
            )
        finally:
            release(array, schema)

    def encode(self, batch, qualifier=-1, precision=6):
        array, schema, output, output_schema = (
            ArrowArray(),
            ArrowSchema(),
            ArrowArray(),
            ArrowSchema(),
        )
        batch._export_to_c(ctypes.addressof(array), ctypes.addressof(schema))
        try:
            result = self.write(
                ctypes.byref(array),
                ctypes.byref(schema),
                qualifier,
                precision,
                ctypes.byref(output),
                ctypes.byref(output_schema),
            )
            if result:
                return result, None
            return 0, pa.RecordBatch._import_from_c(
                ctypes.addressof(output), ctypes.addressof(output_schema)
            ).column(0).to_pylist()
        finally:
            release(array, schema, output, output_schema)

    def test_all_qualifiers_precisions_codec_data_ipc_and_exact_wire(self):
        for qualifier, name in enumerate(QUALIFIERS):
            for precision in range(7) if name.endswith("SECOND") else (6,):
                with self.subTest(qualifier=name, precision=precision):
                    batch = self.batch(qualifier, precision)
                    metadata = batch.schema.field(0).metadata
                    self.assertEqual(
                        metadata[PREFIX + b"interval_contract_version"], b"1"
                    )
                    self.assertEqual(
                        metadata[PREFIX + b"interval_constraint_state"], b"known"
                    )
                    self.assertEqual(
                        metadata[PREFIX + b"interval_qualifier"], name.encode()
                    )
                    self.assertEqual(
                        metadata.get(PREFIX + b"fractional_precision"),
                        str(precision).encode() if name.endswith("SECOND") else None,
                    )
                    quantum = QUANTA[qualifier]
                    if name.endswith("SECOND"):
                        quantum *= 10 ** (6 - precision)
                    if qualifier < 3:
                        self.assertEqual(
                            [
                                None if x is None else tuple(x)
                                for x in batch.column(0).to_pylist()
                            ],
                            [(0, 0, 0), (quantum, 0, 0), (-quantum, 0, 0), None],
                        )
                    else:
                        self.assertEqual(
                            batch.column(0).cast(pa.int64()).to_pylist(),
                            [0, quantum, -quantum, None],
                        )
                    sink = pa.BufferOutputStream()
                    with pa.ipc.new_stream(sink, batch.schema) as writer:
                        writer.write_batch(batch)
                    restored = pa.ipc.open_stream(sink.getvalue()).read_next_batch()
                    self.assertTrue(batch.equals(restored))
                    self.assertTrue(
                        batch.schema.equals(restored.schema, check_metadata=True)
                    )
                    width, fmt = (4, ">i") if qualifier < 3 else (8, ">q")
                    expected = [
                        struct.pack(">i", width) + struct.pack(fmt, value)
                        for value in (0, quantum, -quantum)
                    ] + [None]
                    self.assertEqual(
                        self.encode(restored, qualifier, precision), (0, expected)
                    )
                    self.assertEqual(
                        self.encode(restored.slice(1, 2), qualifier, precision),
                        (0, expected[1:3]),
                    )
                    for index in (1, 2):
                        empty_or_null = self.batch(qualifier, precision, index)
                        self.assertTrue(
                            batch.schema.equals(
                                empty_or_null.schema, check_metadata=True
                            )
                        )
                        self.assertEqual(
                            self.encode(empty_or_null, qualifier, precision),
                            (0, [] if index == 1 else [None]),
                        )

    def test_source_and_target_quantum_reject_independently(self):
        batch = self.batch(6, 6)
        # Native reference value +/-1us contradicts a known SECOND(3) target.
        self.assertNotEqual(self.encode(batch, 6, 3)[0], 0)
        metadata = dict(batch.schema.field(0).metadata)
        metadata[PREFIX + b"fractional_precision"] = b"3"
        field = batch.schema.field(0).with_metadata(metadata)
        offered = pa.RecordBatch.from_arrays(
            list(batch.columns), schema=pa.schema([field])
        )
        self.assertNotEqual(self.encode(offered, 6, 6)[0], 0)

    def test_unknown_is_not_unspecified_or_automatically_rebindable(self):
        batch = self.batch(9, 3)
        for state in (b"unknown", b"unspecified"):
            metadata = dict(batch.schema.field(0).metadata)
            metadata[PREFIX + b"interval_constraint_state"] = state
            # Unknown diagnostics cannot be erased implicitly by duration storage;
            # unspecified must deliberately omit qualifier and precision.
            field = batch.schema.field(0).with_metadata(metadata)
            changed = pa.RecordBatch.from_arrays(
                list(batch.columns), schema=pa.schema([field])
            )
            self.assertNotEqual(self.encode(changed)[0], 0)
        metadata = dict(batch.schema.field(0).metadata)
        metadata[PREFIX + b"interval_constraint_state"] = b"unspecified"
        del metadata[PREFIX + b"interval_qualifier"]
        del metadata[PREFIX + b"fractional_precision"]
        field = batch.schema.field(0).with_metadata(metadata)
        asserted = pa.RecordBatch.from_arrays(
            list(batch.columns), schema=pa.schema([field])
        )
        self.assertEqual(self.encode(asserted, 9, 3)[0], 0)

    def test_foreign_null_and_bad_versions_cannot_choose_null_writer(self):
        for key, value in [
            (b"ARROW:extension:metadata", b"{}"),
            (PREFIX + b"sql_type", b"HLLSKETCH"),
            (PREFIX + b"type_modifier", b"-1"),
        ]:
            field = pa.field("amount", pa.null(), metadata={key: value})
            batch = pa.RecordBatch.from_arrays([pa.nulls(2)], schema=pa.schema([field]))
            self.assertNotEqual(self.encode(batch, 9, 3)[0], 0)
        batch = self.batch(9, 3)
        for version in (b"", b"01", b"2"):
            metadata = dict(batch.schema.field(0).metadata)
            metadata[PREFIX + b"interval_contract_version"] = version
            field = batch.schema.field(0).with_metadata(metadata)
            changed = pa.RecordBatch.from_arrays(
                list(batch.columns), schema=pa.schema([field])
            )
            self.assertNotEqual(self.encode(changed)[0], 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", required=True)
    args = parser.parse_args()
    library = ctypes.CDLL(args.fixture)
    IntervalWriteCDataTest.produce = staticmethod(
        library.AdbcRedshiftIntervalQualifiedFixture
    )
    IntervalWriteCDataTest.produce.argtypes = [
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
    ]
    IntervalWriteCDataTest.produce.restype = ctypes.c_int
    IntervalWriteCDataTest.write = staticmethod(
        library.AdbcRedshiftIntervalWriteFixture
    )
    IntervalWriteCDataTest.write.argtypes = [
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
    ]
    IntervalWriteCDataTest.write.restype = ctypes.c_int
    unittest.main(argv=[__file__], verbosity=2)
