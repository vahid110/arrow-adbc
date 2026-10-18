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
    def batch(self, empty=False):
        schema = ArrowSchema()
        array = ArrowArray()
        code = self.produce(int(empty), ctypes.byref(array), ctypes.byref(schema))
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


if __name__ == "__main__":
    library = ctypes.CDLL(sys.argv.pop(1))
    IntervalCDataTest.produce = staticmethod(library.AdbcRedshiftIntervalFixture)
    IntervalCDataTest.produce.argtypes = [
        ctypes.c_int,
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
    ]
    IntervalCDataTest.produce.restype = ctypes.c_int
    unittest.main(verbosity=2)
