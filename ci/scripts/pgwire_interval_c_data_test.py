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
    unittest.main(verbosity=2)
