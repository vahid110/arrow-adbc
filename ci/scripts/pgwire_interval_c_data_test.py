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
import math
import struct
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

    def test_character_binary_reader_preserves_padding_raw_bytes_and_wide_descriptors(
        self,
    ):
        def produce(which):
            schema, array = ArrowSchema(), ArrowArray()
            code = self.bytes_produce(which, ctypes.byref(array), ctypes.byref(schema))
            if code:
                self.assertFalse(array.release)
                self.assertFalse(schema.release)
                return code, None
            result = pa.RecordBatch._import_from_c(
                ctypes.addressof(array), ctypes.addressof(schema)
            )
            return code, result

        _, result = produce(0)
        self.assertEqual(result.column(0).to_pylist(), ["a   ", "", "    ", None])
        self.assertEqual(result.column(1).to_pylist(), ["😀'\\  ", "", "  ", None])
        self.assertEqual(
            result.column(2).to_pylist(), [b"\x00\x80\xff\\x", b"", b"ab", None]
        )
        for i, family, width in (
            (0, b"CHAR", b"4"),
            (1, b"VARCHAR", b"20"),
            (2, b"VARBYTE", b"20"),
        ):
            self.assertEqual(
                result.schema.field(i).metadata[b"ADBC:redshift:sql_type"], family
            )
            self.assertEqual(
                result.schema.field(i).metadata[b"ADBC:redshift:byte_width"], width
            )
        _, empty = produce(1)
        self.assertEqual(empty.num_rows, 0)
        self.assertEqual(empty.schema, result.schema)
        _, nulls = produce(2)
        self.assertEqual(
            nulls.to_pylist(), [{"bpchar": None, "varchar": None, "varbyte": None}]
        )
        _, wide = produce(3)
        self.assertEqual(wide.column(1)[0].as_py(), "q" * 70000)
        self.assertEqual(
            wide.schema.field(1).metadata[b"ADBC:redshift:byte_width"], b"70000"
        )
        for invalid in (4, 5, 6):
            self.assertNotEqual(produce(invalid)[0], 0)
        _, maximum = produce(7)
        raw = maximum.column(2)[0].as_buffer()
        self.assertEqual(len(raw), 16777216)
        self.assertEqual(raw[0], 0)
        self.assertEqual(raw[-1], 255)
        self.assertEqual(
            maximum.schema.field(2).metadata[b"ADBC:redshift:byte_width"], b"16777216"
        )

    def test_consumer_character_binary_writers_have_closed_utf8_and_byte_widths(self):
        def encode(kind, values, family, width, target=(0, 0)):
            metadata = {
                b"ADBC:redshift:sql_type": family.encode(),
                b"ADBC:redshift:byte_width": str(width).encode(),
            }
            return self.encode_parameter(
                kind, values, self.bytes_write, target, metadata
            )

        for arrow_type in (pa.string(), pa.large_string(), pa.string_view()):
            code, wire = encode(arrow_type, ["😀'\\  ", "", None], "VARCHAR", 20)
            self.assertEqual(code, 0)
            self.assertEqual(
                wire,
                [
                    bytes.fromhex("00000008f09f9880275c2020"),
                    bytes.fromhex("00000000"),
                    None,
                ],
            )
        for arrow_type in (
            pa.binary(),
            pa.large_binary(),
            pa.binary_view(),
            pa.binary(3),
        ):
            code, wire = encode(
                arrow_type, [b"\x00\x80\xff", None], "VARBYTE", 20, (3, 20)
            )
            self.assertEqual(code, 0)
            self.assertEqual(wire, [bytes.fromhex("000000030080ff"), None])
        for arrow_type, values, family, width, target in (
            (pa.string(), ["a  "], "CHAR", 2, (0, 0)),
            (pa.string(), ["😀x"], "VARCHAR", 20, (2, 4)),
            (pa.string(), ["é"], "CHAR", 4, (0, 0)),
            (pa.string(), ["a\0b"], "VARCHAR", 20, (0, 0)),
            (pa.binary(), [b"abc"], "VARBYTE", 2, (3, 20)),
            (pa.binary(), [b"a"], "VARBYTE", 20, (2, 20)),
            (pa.string(), ["a"], "VARCHAR", 20, (3, 20)),
            (pa.string(), ["a"], "VARCHAR", 20, (2, -1)),
        ):
            code, output = encode(arrow_type, values, family, width, target)
            self.assertNotEqual(code, 0)
            self.assertIsNone(output)
        code, wire = encode(pa.string(), [], "VARCHAR", 20)
        self.assertEqual((code, wire), (0, []))

    def encode_decimal(self, arrow_type, values, target=(0, 0), metadata=None):
        return self.encode_parameter(
            arrow_type, values, self.decimal_write, target, metadata
        )

    def temporal_batch(self, variant):
        array, schema = ArrowArray(), ArrowSchema()
        self.assertEqual(
            self.temporal_produce(variant, ctypes.byref(array), ctypes.byref(schema)),
            0,
        )
        try:
            return pa.RecordBatch._import_from_c(
                ctypes.addressof(array), ctypes.addressof(schema)
            )
        finally:
            for value in (array, schema):
                if value.release:
                    ctypes.CFUNCTYPE(None, ctypes.POINTER(type(value)))(value.release)(
                        ctypes.byref(value)
                    )

    def test_temporal_codec_contexts_c_data_and_ipc_keep_exact_physical_values(self):
        # Both schemas/arrays come from production descriptor and field readers.
        # Compare integer storage, avoiding datetime's narrower civil calendar.
        # Independent proleptic-Gregorian integer oracle (astronomical year0).
        # This states Arrow day-count expectations, not a native SQL BC claim.
        def before_year(year):
            previous = year - 1
            return 365 * previous + previous // 4 - previous // 100 + previous // 400

        self.assertEqual(before_year(2000) - before_year(1970), 10957)
        self.assertEqual(before_year(0) - before_year(1970), -719528)
        for variant in (0, 1, 0, 1, 3, 4):
            data = self.temporal_batch(variant)
            lossless = variant in (1, 4)
            self.assertEqual(
                data.schema.names, ["date", "time", "timestamp", "timestamptz"]
            )
            self.assertEqual(data.column(0).type, pa.date32())
            self.assertEqual(
                data.column(1).type, pa.int64() if lossless else pa.time64("us")
            )
            self.assertEqual(
                data.column(2).type, pa.int64() if lossless else pa.timestamp("us")
            )
            self.assertEqual(
                data.column(3).type,
                pa.int64() if lossless else pa.timestamp("us", "UTC"),
            )
            expected = [
                [10957, 0, -719528, 10958, None],
                [0, 86399999999, 1500000, 1234, None],
                [0, -946684800000000, -1, 1, None],
                [0, -946684800000000, -1, 1, None],
            ]
            for col in range(4):
                values = (
                    data.column(col)
                    .cast(pa.int32() if col == 0 else pa.int64())
                    .to_pylist()
                )
                want = [] if variant >= 3 else expected[col]
                if not lossless and col >= 2:
                    want = [None if x is None else x + 946684800000000 for x in want]
                self.assertEqual(values, want)
                field = data.schema.field(col)
                self.assertEqual(
                    field.metadata[b"ADBC:redshift:sql_type"],
                    [b"DATE", b"TIME", b"TIMESTAMP", b"TIMESTAMPTZ"][col],
                )
                if col:
                    self.assertEqual(
                        field.metadata[b"ADBC:redshift:fractional_precision"], b"6"
                    )
                    if lossless:
                        self.assertIn(b"ARROW:extension:name", field.metadata)
                    else:
                        self.assertNotIn(b"ARROW:extension:name", field.metadata)
            sink = pa.BufferOutputStream()
            with pa.ipc.new_stream(sink, data.schema) as writer:
                writer.write_batch(data)
            with pa.ipc.open_stream(sink.getvalue()) as reader:
                restored = reader.read_next_batch()
            self.assertTrue(restored.equals(data))
            self.assertTrue(restored.schema.equals(data.schema, check_metadata=True))

    def test_temporal_lossless_extrema_and_end_of_day_survive_ipc_without_datetime(
        self,
    ):
        data = self.temporal_batch(2)
        self.assertEqual(data.column(1)[0].as_py(), 86400000000)
        for col in (2, 3):
            self.assertEqual(
                data.column(col).to_pylist(), [2**63 - 2, -(2**63) + 1, -1, 1, None]
            )
        sink = pa.BufferOutputStream()
        with pa.ipc.new_stream(sink, data.schema) as writer:
            writer.write_batch(data)
        restored = pa.ipc.open_stream(sink.getvalue()).read_next_batch()
        self.assertTrue(restored.equals(data))
        # Export the imported lossless C Data field back to the actual writer.
        # No physical int64 inference is involved: metadata supplies closed identity.
        for col in (1, 2, 3):
            field = restored.schema.field(col)
            code, actual = self.encode_parameter(
                field.type,
                restored.column(col).to_pylist(),
                self.temporal_write,
                (col + 1, 6),
                field.metadata,
            )
            self.assertEqual(code, 0)
            first = [
                "00000008000000141dd76000",
                "000000087ffffffffffffffe",
                "000000087ffffffffffffffe",
            ][col - 1]
            self.assertEqual(actual[0].hex(), first)
            self.assertIsNone(actual[-1])

    def test_temporal_consumer_units_precision_and_closed_metadata_reach_writer(self):
        for arrow_type, family, value, target, expected in [
            (pa.date32(), b"DATE", 10957, 1, "0000000400000000"),
            (pa.date64(), b"DATE", -86400000, 1, "00000004ffffd532"),
            (pa.time32("s"), b"TIME", 1, 2, "0000000800000000000f4240"),
            (pa.time32("ms"), b"TIME", 1, 2, "0000000800000000000003e8"),
            (pa.time64("ns"), b"TIME", 1000, 2, "000000080000000000000001"),
            (
                pa.timestamp("ms"),
                b"TIMESTAMP",
                9224318015999999,
                3,
                "000000087fffff5bb3b29c18",
            ),
            (
                pa.timestamp("s", "Asia/Kathmandu"),
                b"TIMESTAMPTZ",
                9224318015999,
                4,
                "000000087fffff5bb3a35dc0",
            ),
        ]:
            code, actual = self.encode_parameter(
                arrow_type,
                [value, None],
                self.temporal_write,
                (target, 6),
                {b"ADBC:redshift:sql_type": family},
            )
            self.assertEqual(code, 0)
            self.assertEqual(actual, [bytes.fromhex(expected), None])
        for arrow_type, family, value, precision in [
            (pa.date64(), b"DATE", 1, None),
            (pa.time64("ns"), b"TIME", 1001, None),
            (pa.time64("us"), b"TIME", 86400000000, None),
            (pa.timestamp("us"), b"TIMESTAMP", 946684800001234, b"3"),
        ]:
            metadata = {b"ADBC:redshift:sql_type": family}
            if precision:
                metadata[b"ADBC:redshift:fractional_precision"] = precision
            code, actual = self.encode_parameter(
                arrow_type, [value], self.temporal_write, (0, 6), metadata
            )
            self.assertNotEqual(code, 0)
            self.assertIsNone(actual)
        field = self.temporal_batch(1).schema.field(1)
        for key, value in [
            (b"ARROW:extension:name", b"adbc.redshift.time_us.v2"),
            (b"ARROW:extension:metadata", b"{}"),
            (b"ADBC:redshift:sql_type", b"TIMESTAMP"),
        ]:
            metadata = dict(field.metadata)
            metadata[key] = value
            code, actual = self.encode_parameter(
                pa.int64(), [1], self.temporal_write, (0, 6), metadata
            )
            self.assertNotEqual(code, 0)
            self.assertIsNone(actual)

    def encode_scalar(self, arrow_type, values, target=0, metadata=None):
        return self.encode_parameter(
            arrow_type, values, self.scalar_write, (target,), metadata
        )

    def encode_parameter(self, arrow_type, values, produce, target, metadata):
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
            code = produce(
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

    def test_scalar_reader_c_data_extrema_subnormals_nulls_and_metadata(self):
        expected_ints = [
            [-32768, 32767, 0, 1, -1, 2],
            [-2147483648, 2147483647, 0, 1, -1, 2],
            [-(2**63), 2**63 - 1, 0, 1, -1, 2],
        ]
        expected_float = [
            [
                -float.fromhex("0x1.fffffep+127"),
                float.fromhex("0x1.fffffep+127"),
                -0.0,
                float.fromhex("0x1p-149"),
                math.inf,
                math.nan,
            ],
            [
                -float.fromhex("0x1.fffffffffffffp+1023"),
                float.fromhex("0x1.fffffffffffffp+1023"),
                -0.0,
                float.fromhex("0x1p-1074"),
                -math.inf,
                math.nan,
            ],
        ]
        families = [
            b"SMALLINT",
            b"INTEGER",
            b"BIGINT",
            b"REAL",
            b"DOUBLE PRECISION",
            b"BOOLEAN",
        ]
        schema = None
        for batch_id, rows in [
            (0, [0, 1, 2, 3, 4, 5, None]),
            (1, [5, 3]),
            (2, []),
            (3, [None]),
        ]:
            c_array, c_schema = ArrowArray(), ArrowSchema()
            self.assertEqual(
                self.scalar_produce(
                    batch_id, ctypes.byref(c_array), ctypes.byref(c_schema)
                ),
                0,
            )
            batch = pa.RecordBatch._import_from_c(
                ctypes.addressof(c_array), ctypes.addressof(c_schema)
            )
            if schema is None:
                schema = batch.schema
            else:
                self.assertTrue(schema.equals(batch.schema, check_metadata=True))
            self.assertEqual(
                batch.schema.types,
                [
                    pa.int16(),
                    pa.int32(),
                    pa.int64(),
                    pa.float32(),
                    pa.float64(),
                    pa.bool_(),
                ],
            )
            for col in range(3):
                self.assertEqual(
                    batch.column(col).to_pylist(),
                    [None if row is None else expected_ints[col][row] for row in rows],
                )
            for col in (3, 4):
                actual = batch.column(col).to_pylist()
                for value, row in zip(actual, rows):
                    if row is None:
                        self.assertIsNone(value)
                    elif math.isnan(expected_float[col - 3][row]):
                        self.assertTrue(math.isnan(value))
                    else:
                        self.assertEqual(value, expected_float[col - 3][row])
                        if row == 2:
                            self.assertEqual(
                                struct.pack(">d", value).hex(), "8000000000000000"
                            )
            self.assertEqual(
                batch.column(5).to_pylist(),
                [None if row is None else bool(row % 2) for row in rows],
            )
            for col, family in enumerate(families):
                metadata = batch.schema.field(col).metadata
                self.assertEqual(metadata[b"ADBC:redshift:sql_type"], family)
                self.assertEqual(metadata[b"ADBC:redshift:type_modifier"], b"-1")
                for key in (b"precision", b"scale", b"byte_width"):
                    self.assertNotIn(b"ADBC:redshift:" + key, metadata)
        for malformed in (4, 5, 6, 7):
            c_array, c_schema = ArrowArray(), ArrowSchema()
            self.assertNotEqual(
                self.scalar_produce(
                    malformed, ctypes.byref(c_array), ctypes.byref(c_schema)
                ),
                0,
            )
            self.assertFalse(c_array.release)
            self.assertFalse(c_schema.release)

    def test_consumer_scalar_writers_match_literal_bytes_and_reject_loss(self):
        cases = [
            (pa.int64(), -32768, 1, "000000028000"),
            (pa.int64(), 32767, 1, "000000027fff"),
            (pa.int64(), -2147483648, 2, "0000000480000000"),
            (pa.int64(), 2147483647, 2, "000000047fffffff"),
            (pa.uint64(), 2**63 - 1, 3, "000000087fffffffffffffff"),
            (pa.int64(), -(2**63), 3, "000000088000000000000000"),
            (pa.float64(), 1.25, 4, "000000043fa00000"),
            (pa.float64(), -0.0, 4, "0000000480000000"),
            (pa.float64(), float.fromhex("0x1p-149"), 4, "0000000400000001"),
            (pa.float64(), math.inf, 4, "000000047f800000"),
            (pa.float64(), -math.inf, 4, "00000004ff800000"),
            (pa.float32(), -0.0, 5, "000000088000000000000000"),
            (pa.float64(), float.fromhex("0x1p-1074"), 5, "000000080000000000000001"),
            (pa.bool_(), True, 6, "0000000101"),
            (pa.bool_(), False, 6, "0000000100"),
        ]
        for arrow_type, value, target, wire in cases:
            for values in ([value, None], [], [None]):
                code, actual = self.encode_scalar(arrow_type, values, target)
                self.assertEqual(code, 0)
                self.assertEqual(
                    actual, [None if x is None else bytes.fromhex(wire) for x in values]
                )
        code, actual = self.encode_scalar(pa.null(), [None], 6)
        self.assertEqual(code, 0)
        self.assertEqual(actual, [None])
        for arrow_type, values, target, metadata in [
            (pa.uint64(), [42, 2**63], 0, None),
            (pa.uint64(), [2**64 - 1], 0, None),
            (pa.int64(), [12, 32768], 1, None),
            (pa.int64(), [-32769], 1, None),
            (pa.float64(), [1.25, 1.1], 4, None),
            (pa.float64(), [float.fromhex("0x1.fffffffffffffp+1023")], 4, None),
            (pa.float64(), [float.fromhex("0x1p-1074")], 4, None),
            (pa.float64(), [1.0], 3, None),
            (pa.int64(), [1], 4, None),
            (pa.bool_(), [True], 3, None),
            (pa.float64(), [1.1], 5, {b"ADBC:redshift:sql_type": b"REAL"}),
            (pa.int64(), [32768], 3, {b"ADBC:redshift:sql_type": b"SMALLINT"}),
        ]:
            code, actual = self.encode_scalar(arrow_type, values, target, metadata)
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
    IntervalCDataTest.scalar_produce = staticmethod(library.AdbcRedshiftScalarFixture)
    IntervalCDataTest.scalar_produce.argtypes = IntervalCDataTest.produce.argtypes
    IntervalCDataTest.scalar_produce.restype = ctypes.c_int
    IntervalCDataTest.scalar_write = staticmethod(
        library.AdbcRedshiftScalarWriteFixture
    )
    IntervalCDataTest.scalar_write.argtypes = [
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
        ctypes.c_int,
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
    ]
    IntervalCDataTest.scalar_write.restype = ctypes.c_int
    IntervalCDataTest.bytes_produce = staticmethod(library.AdbcRedshiftBytesFixture)
    IntervalCDataTest.bytes_produce.argtypes = IntervalCDataTest.produce.argtypes
    IntervalCDataTest.bytes_produce.restype = ctypes.c_int
    IntervalCDataTest.bytes_write = staticmethod(library.AdbcRedshiftBytesWriteFixture)
    IntervalCDataTest.bytes_write.argtypes = IntervalCDataTest.decimal_write.argtypes
    IntervalCDataTest.bytes_write.restype = ctypes.c_int
    IntervalCDataTest.temporal_produce = staticmethod(
        library.AdbcRedshiftTemporalFixture
    )
    IntervalCDataTest.temporal_produce.argtypes = IntervalCDataTest.produce.argtypes
    IntervalCDataTest.temporal_produce.restype = ctypes.c_int
    IntervalCDataTest.temporal_write = staticmethod(
        library.AdbcRedshiftTemporalWriteFixture
    )
    IntervalCDataTest.temporal_write.argtypes = IntervalCDataTest.decimal_write.argtypes
    IntervalCDataTest.temporal_write.restype = ctypes.c_int
    unittest.main(verbosity=2)
