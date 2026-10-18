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

"""Import real codec-produced C Data. Offline protocol fixtures, never live evidence."""

import ctypes
import sys
import unittest
from decimal import Decimal

import pyarrow as pa


class ArrowSchema(ctypes.Structure):
    _fields_ = [
        ("format", ctypes.c_char_p),
        ("name", ctypes.c_char_p),
        ("metadata", ctypes.c_void_p),
        ("flags", ctypes.c_int64),
        ("n_children", ctypes.c_int64),
        ("children", ctypes.c_void_p),
        ("dictionary", ctypes.c_void_p),
        ("release", ctypes.c_void_p),
        ("private_data", ctypes.c_void_p),
    ]


class ArrowArray(ctypes.Structure):
    _fields_ = [
        ("length", ctypes.c_int64),
        ("null_count", ctypes.c_int64),
        ("offset", ctypes.c_int64),
        ("n_buffers", ctypes.c_int64),
        ("n_children", ctypes.c_int64),
        ("buffers", ctypes.c_void_p),
        ("children", ctypes.c_void_p),
        ("dictionary", ctypes.c_void_p),
        ("release", ctypes.c_void_p),
        ("private_data", ctypes.c_void_p),
    ]


class IntervalCDataTest(unittest.TestCase):
    def batch(self, empty=False, modifier=False):
        schema = ArrowSchema()
        array = ArrowArray()
        produce = self.modifier_produce if modifier else self.produce
        code = produce(int(empty), ctypes.byref(array), ctypes.byref(schema))
        self.assertEqual(code, 0)
        try:
            return pa.RecordBatch._import_from_c(
                ctypes.addressof(array), ctypes.addressof(schema)
            )
        finally:
            for value in (array, schema):
                if value.release:
                    release = ctypes.CFUNCTYPE(None, ctypes.POINTER(type(value)))(
                        value.release
                    )
                    release(ctypes.byref(value))

    def test_signed_extremes_nulls_and_mixed_rows(self):
        batch = self.batch()
        self.assertEqual(batch.num_rows, 6)
        self.assertEqual(batch.column(0).type, pa.month_day_nano_interval())
        self.assertEqual(batch.column(1).type, pa.duration("us"))
        # These are expected scalar values, never independently constructed arrays.
        months = batch.column(0).to_pylist()
        self.assertEqual(
            [None if x is None else tuple(x) for x in months],
            [
                (0, 0, 0),
                (14, 0, 0),
                (-14, 0, 0),
                (-(2**31), 0, 0),
                (2**31 - 1, 0, 0),
                None,
            ],
        )
        # Do not convert int64 extrema into datetime.timedelta or nanoseconds.
        self.assertEqual(
            batch.column(1).cast(pa.int64()).to_pylist(),
            [0, 1000001, -1000001, -(2**63), 2**63 - 1, None],
        )
        self.assertEqual(batch.column(2).to_pylist(), [42] * 6)
        for column, family in [(0, b"intervaly2m"), (1, b"intervald2s")]:
            self.assertEqual(
                batch.schema.field(column).metadata[b"ADBC:redshift:interval_family"],
                family,
            )

    def test_empty_batch_retains_schema(self):
        batch = self.batch(empty=True)
        self.assertEqual(batch.num_rows, 0)
        self.assertEqual(batch.schema.field(0).type, pa.month_day_nano_interval())
        self.assertEqual(batch.schema.field(1).type, pa.duration("us"))
        self.assertEqual(
            batch.schema.field(1).metadata[b"ADBC:redshift:interval_family"],
            b"intervald2s",
        )

    def test_descriptor_metadata_survives_c_data_import_and_null_rows(self):
        for empty in (False, True):
            batch = self.batch(empty=empty, modifier=True)
            self.assertEqual(batch.num_rows, 0 if empty else 1)
            self.assertEqual(
                batch.schema.names,
                ["n10", "n38", "v20", "vwide", "fixed", "months", "unknown"],
            )

            def metadata(column, key):
                return batch.schema.field(column).metadata.get(key)

            self.assertEqual(metadata(0, b"ADBC:redshift:sql_type"), b"DECIMAL")
            self.assertEqual(metadata(0, b"ADBC:redshift:precision"), b"10")
            self.assertEqual(metadata(0, b"ADBC:redshift:scale"), b"2")
            self.assertEqual(metadata(1, b"ADBC:redshift:precision"), b"38")
            self.assertEqual(metadata(1, b"ADBC:redshift:scale"), b"37")
            self.assertEqual(metadata(2, b"ADBC:redshift:byte_width"), b"20")
            self.assertEqual(metadata(3, b"ADBC:redshift:byte_width"), b"70000")
            self.assertEqual(metadata(4, b"ADBC:redshift:sql_type"), b"CHAR")
            self.assertEqual(metadata(4, b"ADBC:redshift:byte_width"), b"10")
            self.assertEqual(
                metadata(5, b"ADBC:redshift:interval_family"), b"intervaly2m"
            )
            self.assertEqual(metadata(5, b"ADBC:redshift:type_modifier"), b"12345")
            self.assertIsNone(metadata(5, b"ADBC:redshift:scale"))
            self.assertIsNone(metadata(6, b"ADBC:redshift:byte_width"))
            for column in batch.columns:
                self.assertEqual(column.null_count, 0 if empty else 1)

    def test_decimal_reference_values_from_production_reader_across_batches(self):
        # Independent exact strings; no float or context-rounded scaleb operation.
        expected = [
            ["9", "-9", "0"],
            ["999999999999999999", "-999999999999999999", "1"],
            ["9223372036854775807", "-9223372036854775808", "9999999999999999999"],
            ["99999999999999999999", "0", "1"],
            [
                "99999999999999999999999999999999999999",
                "-99999999999999999999999999999999999999",
                "0",
            ],
            ["1.25", "-1.23", "0.00"],
            [
                "9.9999999999999999999999999999999999999",
                "-0.0000000000000000000000000000000000001",
                "0.0000000000000000000000000000000000000",
            ],
            [
                "0.99999999999999999999999999999999999999",
                "0.00000000000000000000000000000000000001",
                "0.00000000000000000000000000000000000000",
            ],
        ]
        unknown = ["-12345678901234567890.0012300", "0.12345678", "0.0000"]
        schema = None
        for batch_id in range(4):
            array = ArrowArray()
            c_schema = ArrowSchema()
            self.assertEqual(
                self.decimal_produce(
                    batch_id, ctypes.byref(array), ctypes.byref(c_schema)
                ),
                0,
            )
            batch = pa.RecordBatch._import_from_c(
                ctypes.addressof(array), ctypes.addressof(c_schema)
            )
            if schema is None:
                schema = batch.schema
            else:
                self.assertTrue(schema.equals(batch.schema, check_metadata=True))
            rows = (
                [0, 1, 2, None]
                if batch_id == 0
                else [2, 1]
                if batch_id == 1
                else []
                if batch_id == 2
                else [None]
            )
            for col, (precision, scale) in enumerate(
                [
                    (1, 0),
                    (18, 0),
                    (19, 0),
                    (20, 0),
                    (38, 0),
                    (10, 2),
                    (38, 37),
                    (38, 38),
                ]
            ):
                self.assertEqual(
                    batch.schema.field(col).type, pa.decimal128(precision, scale)
                )
                self.assertEqual(
                    batch.column(col).to_pylist(),
                    [
                        None if row is None else Decimal(expected[col][row])
                        for row in rows
                    ],
                )
                self.assertEqual(
                    batch.schema.field(col).metadata[b"ADBC:redshift:sql_type"],
                    b"DECIMAL",
                )
            self.assertEqual(
                batch.column(8).to_pylist(),
                [None if row is None else unknown[row] for row in rows],
            )
            self.assertEqual(batch.schema.field(8).type, pa.string())
            self.assertEqual(
                batch.schema.field(8).metadata[b"ADBC:redshift:sql_type"], b"DECIMAL"
            )
            self.assertNotIn(b"ARROW:extension:name", batch.schema.field(8).metadata)
            self.assertNotIn(b"ADBC:redshift:precision", batch.schema.field(8).metadata)
            self.assertEqual(
                batch.column(9).to_pylist(),
                [None if row is None else 42 for row in rows],
            )

    def encode_decimal(self, arrow_type, values, target=(0, 0), metadata=None):
        field = pa.field("amount", arrow_type, metadata=metadata)
        batch = pa.RecordBatch.from_arrays(
            [pa.array(values, type=arrow_type)], schema=pa.schema([field])
        )
        input_array, input_schema = ArrowArray(), ArrowSchema()
        output_array, output_schema = ArrowArray(), ArrowSchema()
        batch._export_to_c(
            ctypes.addressof(input_array), ctypes.addressof(input_schema)
        )
        try:
            code = self.decimal_write(
                ctypes.byref(input_array),
                ctypes.byref(input_schema),
                *target,
                ctypes.byref(output_array),
                ctypes.byref(output_schema),
            )
            if code:
                # A late invalid value must not export a partial result batch.
                self.assertFalse(output_array.release)
                self.assertFalse(output_schema.release)
                return code, None
            return code, pa.RecordBatch._import_from_c(
                ctypes.addressof(output_array), ctypes.addressof(output_schema)
            ).column(0).to_pylist()
        finally:
            for value in (input_array, input_schema, output_array, output_schema):
                if value.release:
                    ctypes.CFUNCTYPE(None, ctypes.POINTER(type(value)))(value.release)(
                        ctypes.byref(value)
                    )

    def test_consumer_exported_decimal_arrays_reach_production_writer(self):
        # Wire includes the int32 parameter length. These independent literals
        # are not generated by a matching reader or by expected Arrow arrays.
        for factory in (pa.decimal128, pa.decimal256):
            for precision, scale, value, wire in [
                (1, 0, "9", "0000000a00010000000000000009"),
                (
                    19,
                    0,
                    "-9223372036854775808",
                    "000000120005000440000000039a0d2c0170156516b0",
                ),
                (
                    38,
                    0,
                    "99999999999999999999999999999999999999",
                    "0000001c000a0009000000000063270f270f270f270f270f270f270f270f270f",
                ),
                (10, 2, "-12.34", "0000000c0002000040000002000c0d48"),
                (
                    38,
                    37,
                    "0.0000000000000000000000000000000000001",
                    "0000000a0001fff60000002503e8",
                ),
            ]:
                for values in ([Decimal(value), None], [], [None]):
                    code, actual = self.encode_decimal(
                        factory(precision, scale), values
                    )
                    self.assertEqual(code, 0)
                    self.assertEqual(
                        actual,
                        [None if x is None else bytes.fromhex(wire) for x in values],
                    )
        # Known append target rescales only exact values. Repeated C Data exports
        # use independent batches, including NULL/empty batches.
        for values in ([Decimal("1.2500"), None], [], [None]):
            code, actual = self.encode_decimal(pa.decimal256(18, 4), values, (10, 2))
            self.assertEqual(code, 0)
            self.assertEqual(
                actual,
                [
                    None
                    if x is None
                    else bytes.fromhex("0000000c0002000000000002000109c4")
                    for x in values
                ],
            )

    def test_decimal_text_identity_and_failed_late_values_through_c_data(self):
        code, actual = self.encode_decimal(pa.null(), [None], (10, 2))
        self.assertEqual(code, 0)
        self.assertEqual(actual, [None])
        identity = {b"ADBC:redshift:sql_type": b"DECIMAL"}
        code, actual = self.encode_decimal(
            pa.string(), ["+001.2500", None], (10, 2), identity
        )
        self.assertEqual(code, 0)
        self.assertEqual(
            actual, [bytes.fromhex("0000000c0002000000000002000109c4"), None]
        )
        for arrow_type, values, metadata in [
            (pa.decimal128(18, 4), [Decimal("1.2500"), Decimal("1.2345")], None),
            (pa.decimal256(19, 0), [Decimal("9223372036854775808")], None),
            (pa.decimal256(76, 0), [Decimal("1")], None),
            (pa.string(), ["1.25"], None),
            (pa.string(), ["1.25", "1e2"], identity),
            (pa.float64(), [1.25], None),
            (pa.int64(), [1], None),
        ]:
            target = (19, 0) if arrow_type == pa.decimal256(19, 0) else (10, 2)
            code, actual = self.encode_decimal(arrow_type, values, target, metadata)
            self.assertNotEqual(code, 0)
            self.assertIsNone(actual)


if __name__ == "__main__":
    library = ctypes.CDLL(sys.argv.pop(1))
    IntervalCDataTest.produce = staticmethod(library.AdbcRedshiftIntervalFixture)
    IntervalCDataTest.produce.argtypes = [
        ctypes.c_int,
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
    ]
    IntervalCDataTest.produce.restype = ctypes.c_int
    IntervalCDataTest.modifier_produce = staticmethod(
        library.AdbcRedshiftModifierFixture
    )
    IntervalCDataTest.modifier_produce.argtypes = IntervalCDataTest.produce.argtypes
    IntervalCDataTest.modifier_produce.restype = ctypes.c_int
    IntervalCDataTest.decimal_produce = staticmethod(library.AdbcRedshiftDecimalFixture)
    IntervalCDataTest.decimal_produce.argtypes = IntervalCDataTest.produce.argtypes
    IntervalCDataTest.decimal_produce.restype = ctypes.c_int
    IntervalCDataTest.decimal_write = staticmethod(
        library.AdbcRedshiftDecimalWriteFixture
    )
    IntervalCDataTest.decimal_write.argtypes = [
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
    ]
    IntervalCDataTest.decimal_write.restype = ctypes.c_int
    unittest.main(verbosity=2)
