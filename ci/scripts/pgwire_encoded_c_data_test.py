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

"""Actual consumer C Data/IPC with internal constructed catalog, no server proof."""

import argparse
import ctypes
import importlib.util
import pathlib
import struct
import unittest

import pyarrow as pa

_path = pathlib.Path(__file__).with_name("pgwire_interval_c_data_test.py")
_spec = importlib.util.spec_from_file_location("_encoded_definitions", _path)
_defs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_defs)
ArrowArray, ArrowSchema = _defs.ArrowArray, _defs.ArrowSchema
SPARSE = '{"version":1,"logm":15,"sparse":{"indices":[],"values":[]}}'
POINT = bytes.fromhex("0101000000000000000000f03f000000000000f03f")
GEOGRAPHY = bytes.fromhex("0101000020e6100000000000000000f03f000000000000f03f")


def encoded_field(family, name="amount"):
    if family == "HLLSKETCH":
        ext = "adbc.redshift.hllsketch"
        metadata = '{"version":1,"encoding":"redshift-serialized","logm":15}'
        storage = pa.large_string()
    else:
        ext = f"adbc.redshift.{family.lower()}.ewkb"
        metadata = (
            '{"version":1,"encoding":"ewkb","family":"'
            + family
            + '","srid":"per-value","axes":"native"}'
        )
        storage = pa.large_binary()
    return pa.field(
        name,
        storage,
        metadata={
            b"ARROW:extension:name": ext.encode(),
            b"ARROW:extension:metadata": metadata.encode(),
            b"ADBC:redshift:sql_type": family.encode(),
        },
    )


class EncodedCDataTest(unittest.TestCase):
    def apply(self, family, values, slice_=False, field=None):
        field = field or encoded_field(family)
        batch = pa.RecordBatch.from_arrays(
            [pa.array(values, type=field.type)], schema=pa.schema([field])
        )
        if slice_:
            batch = batch.slice(1)
        array, schema, out, outschema = (
            ArrowArray(),
            ArrowSchema(),
            ArrowArray(),
            ArrowSchema(),
        )
        batch._export_to_c(ctypes.addressof(array), ctypes.addressof(schema))
        try:
            code = self.fixture(
                ctypes.byref(array),
                ctypes.byref(schema),
                ctypes.byref(out),
                ctypes.byref(outschema),
            )
            if code:
                return code, None
            return 0, pa.RecordBatch._import_from_c(
                ctypes.addressof(out), ctypes.addressof(outschema)
            )
        finally:
            for item in (array, schema, out, outschema):
                if item.release:
                    ctypes.CFUNCTYPE(None, ctypes.POINTER(type(item)))(item.release)(
                        ctypes.byref(item)
                    )

    def test_actual_writer_reader_null_empty_and_slice(self):
        for family, value in (
            ("GEOMETRY", POINT),
            ("GEOGRAPHY", GEOGRAPHY),
            ("HLLSKETCH", SPARSE),
        ):
            code, batch = self.apply(family, [value, value, None], True)
            self.assertEqual(code, 0)
            self.assertEqual(batch.column(1).to_pylist(), [value, None])
            wire = batch.column(0)[0].as_py()
            payload = value.encode() + b"\0" if family == "HLLSKETCH" else value
            self.assertEqual(struct.unpack("!i", wire[:4])[0], len(payload))
            self.assertEqual(wire[4:], payload)
            if family != "HLLSKETCH":
                self.assertNotEqual(wire[4:], value.hex().encode())
            self.assertEqual(
                batch.schema.field(1).metadata[b"ARROW:extension:name"],
                encoded_field(family).metadata[b"ARROW:extension:name"],
            )
            sink = pa.BufferOutputStream()
            with pa.ipc.new_stream(sink, batch.schema) as writer:
                writer.write_batch(batch)
            restored = pa.ipc.open_stream(sink.getvalue()).read_next_batch()
            self.assertEqual(restored.schema, batch.schema)
            self.assertEqual(restored.to_pylist(), batch.to_pylist())

    def test_geometry_protocol_alias_survives_c_data_and_ipc(self):
        # Fixture catalog72123 and result3999 exercise the actual shared output
        # resolver, rather than constructing the output schema from input metadata.
        for values in ([], [None], [POINT, None]):
            code, batch = self.apply("GEOMETRY", values)
            self.assertEqual(code, 0)
            field = batch.schema.field(1)
            self.assertEqual(field.type, pa.large_binary())
            for key, value in encoded_field("GEOMETRY").metadata.items():
                self.assertEqual(field.metadata[key], value)
            self.assertEqual(batch.column(1).to_pylist(), values)
            sink = pa.BufferOutputStream()
            with pa.ipc.new_stream(sink, batch.schema) as writer:
                writer.write_batch(batch)
            restored = pa.ipc.open_stream(sink.getvalue()).read_next_batch()
            self.assertEqual(restored.schema, batch.schema)
            self.assertEqual(restored.column(1).to_pylist(), values)

    def test_large_spatial_raw_payload_and_null_keep_exact_c_data_bytes(self):
        for family in ("GEOMETRY", "GEOGRAPHY"):
            # Finite owned LINESTRING with an embedded SRID, larger than64KiB.
            value = struct.pack("<BIII", 1, 0x20000002, 4326, 5000) + b"".join(
                struct.pack("<dd", float(i % 2), float(i % 2)) for i in range(5000)
            )
            code, batch = self.apply(family, [None, value, None])
            self.assertEqual(code, 0)
            self.assertEqual(batch.column(1).to_pylist(), [None, value, None])
            self.assertEqual(batch.column(0).null_count, 2)
            wire = batch.column(0)[1].as_py()
            self.assertGreater(len(value), 65536)
            self.assertLess(len(value), 500000)
            self.assertEqual(struct.unpack("!i", wire[:4])[0], len(value))
            self.assertEqual(wire[4:], value)
            self.assertNotEqual(wire[4:], value.hex().encode())
            # ASCIIhex is not an EWKB application input either.
            self.assertNotEqual(self.apply(family, [value.hex().encode()])[0], 0)

    def test_spatial_srid_nan_endian_and_structural_rejections(self):
        empty = bytes.fromhex("0101000000000000000000f87f000000000000f87f")
        big = bytes.fromhex("00000000013ff00000000000003ff0000000000000")
        self.assertEqual(self.apply("GEOMETRY", [empty, big])[0], 0)
        for family, value in (
            ("GEOGRAPHY", POINT),
            ("GEOMETRY", POINT + b"x"),
            ("GEOMETRY", POINT[:-1]),
        ):
            self.assertNotEqual(self.apply(family, [value])[0], 0)

    def test_hll_encoded_null_and_malformed_are_distinct(self):
        self.assertEqual(self.apply("HLLSKETCH", [None, SPARSE, '"AAAA"'])[0], 0)
        for value in (
            "",
            '"AB=="',
            SPARSE + "\0",
            SPARSE.replace('"version":1', '"version":2'),
        ):
            self.assertNotEqual(self.apply("HLLSKETCH", [value])[0], 0)

    def test_schema_version_family_storage_and_all_null_reject(self):
        for family in ("GEOMETRY", "GEOGRAPHY", "HLLSKETCH"):
            field = encoded_field(family)
            for kind in ("version", "family", "storage"):
                meta = dict(field.metadata)
                if kind == "version":
                    meta[b"ARROW:extension:metadata"] = meta[
                        b"ARROW:extension:metadata"
                    ].replace(b'"version":1', b'"version":2')
                if kind == "family":
                    meta[b"ADBC:redshift:sql_type"] = b"SUPER"
                bad = pa.field(
                    "amount",
                    pa.binary() if kind == "storage" else field.type,
                    metadata=meta,
                )
                self.assertNotEqual(self.apply(family, [None], field=bad)[0], 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", required=True, type=pathlib.Path)
    args = parser.parse_args()
    library = ctypes.CDLL(str(args.fixture.resolve(strict=True)))
    EncodedCDataTest.fixture = staticmethod(library.AdbcRedshiftEncodedFixture)
    EncodedCDataTest.fixture.argtypes = [
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
        ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ArrowSchema),
    ]
    EncodedCDataTest.fixture.restype = ctypes.c_int
    unittest.main(argv=[__file__], verbosity=2)
