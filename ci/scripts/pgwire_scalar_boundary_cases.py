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

"""Finite integer/float qualification on a supplied, bounded owned session.

run(session, stream) uses exactly 38 operations: three positive routes with full
and empty reads, nine first-invalid APPEND controls with identity and retained-row
checks. The owner supplies connection, schema, database, username, execute, query,
and charge; no credentials, connections or cleanup SQL are created here. Stop all
SQL after UnsafeBoundary. NaNs are numeric values, not payload-bit promises.
"""

from __future__ import annotations

import math
import re
import struct
from dataclasses import dataclass

import pyarrow as pa

import adbc_driver_manager as adm

NAMES = ("row_id", "i16", "i32", "i64", "u64", "f32", "f64", "narrow")
SQL_TYPES = (
    "INTEGER",
    "SMALLINT",
    "INTEGER",
    "BIGINT",
    "BIGINT",
    "REAL",
    "DOUBLE PRECISION",
    "REAL",
)
F32 = (
    0x7F7FFFFF,
    0xFF7FFFFF,
    1,
    0x80000001,
    0,
    0x80000000,
    0x7FC00000,
    0x7F800000,
    0xFF800000,
    0x00800000,
    0x007FFFFF,
)
F64 = (
    0x7FEFFFFFFFFFFFFF,
    0xFFEFFFFFFFFFFFFF,
    1,
    0x8000000000000001,
    0,
    0x8000000000000000,
    0x7FF8000000000000,
    0x7FF0000000000000,
    0xFFF0000000000000,
    0x0010000000000000,
    0x000FFFFFFFFFFFFF,
)
NARROW = (
    0x47EFFFFFE0000000,
    0xC7EFFFFFE0000000,
    0x36A0000000000000,
    0xB6A0000000000000,
    0,
    0x8000000000000000,
    0x7FF8000000000000,
    0x7FF0000000000000,
    0xFFF0000000000000,
    0x3FF4000000000000,
    0xBFF4000000000000,
)
NARROW_OUTPUT = F32[:9] + (0x3FA00000, 0xBFA00000)


def _float(bits, width):
    return struct.unpack(
        ">f" if width == 32 else ">d", bits.to_bytes(width // 8, "big")
    )[0]


def _integers(width):
    return [
        None,
        -(2 ** (width - 1)),
        2 ** (width - 1) - 1,
        0,
        -1,
        1,
        42,
        127,
        -128,
        256,
        7,
        -7,
    ]


def _values(offset=0, output=False):
    return [
        list(range(offset, offset + 12)),
        _integers(16),
        _integers(32),
        _integers(64),
        [None, 0, 2**63 - 1, 1, 42, 2, None, 0, 42, 17, 9, None],
        [None] + [_float(x, 32) for x in F32],
        [None] + [_float(x, 64) for x in F64],
        [None]
        + [
            _float(x, 32 if output else 64)
            for x in (NARROW_OUTPUT if output else NARROW)
        ],
    ]


def source_batch(route):
    if route not in ("direct", "create", "append"):
        raise ValueError("closed scalar route")
    types = (
        [pa.int32()]
        + [pa.int64()] * 3
        + [pa.uint64(), pa.float32(), pa.float64(), pa.float64()]
    )
    markers = list(SQL_TYPES)
    if route == "direct":
        markers[-1] = "DOUBLE PRECISION"
    schema = pa.schema(
        [
            pa.field(n, t, metadata={b"ADBC:redshift:sql_type": m.encode()})
            for n, t, m in zip(NAMES, types, markers)
        ]
    )
    return pa.RecordBatch.from_arrays(
        [
            pa.array(v, type=t)
            for v, t in zip(_values(12 if route == "append" else 0), types)
        ],
        schema=schema,
    )


def expected_table(append=False, empty=False):
    # Independent stored schema and float32 narrowing bits; never cast source Arrow.
    types = (
        pa.int32(),
        pa.int16(),
        pa.int32(),
        pa.int64(),
        pa.int64(),
        pa.float32(),
        pa.float64(),
        pa.float32(),
    )
    pg = ("int4", "int2", "int4", "int8", "int8", "float4", "float8", "float4")
    schema = pa.schema(
        [
            pa.field(
                n,
                t,
                metadata={
                    b"POSTGRESQL:type": p.encode(),
                    b"ADBC:redshift:sql_type": m.encode(),
                    b"ADBC:redshift:type_modifier": b"-1",
                },
            )
            for n, t, p, m in zip(NAMES, types, pg, SQL_TYPES)
        ]
    )
    values = _values(output=True)
    if append:
        values = [a + b for a, b in zip(values, _values(12, output=True))]
    return pa.Table.from_arrays(
        [pa.array([] if empty else v, type=t) for v, t in zip(values, types)],
        schema=schema,
    )


def compare(actual, expected):
    if not actual.schema.equals(expected.schema, check_metadata=True):
        raise ValueError("scalar schema mismatch")
    if actual.num_rows != expected.num_rows:
        raise ValueError("scalar row count mismatch")
    for column, field in enumerate(expected.schema):
        for got, want in zip(
            actual.column(column).to_pylist(), expected.column(column).to_pylist()
        ):
            if want is None or got is None:
                valid = want is None and got is None
            elif pa.types.is_floating(field.type):
                code = ">f" if pa.types.is_float32(field.type) else ">d"
                valid = (
                    math.isnan(got)
                    if math.isnan(want)
                    else (struct.pack(code, got) == struct.pack(code, want))
                )
            else:
                valid = got == want
            if not valid:
                raise ValueError("scalar value mismatch")


def negative_batch(index):
    if type(index) is not int or index not in range(9):
        raise ValueError("closed rejection control")
    names = ("i16", "i16", "i32", "i32", "u64", "u64", "narrow", "narrow", "narrow")
    values = (
        -32769,
        32768,
        -2147483649,
        2147483648,
        2**63,
        2**64 - 1,
        1.1,
        _float(0x7FEFFFFFFFFFFFFF, 64),
        _float(1, 64),
    )
    dtype = pa.int64() if index < 4 else pa.uint64() if index < 6 else pa.float64()
    # Unmarked source: validate conversion to the actual APPEND catalog target.
    return pa.record_batch([pa.array([values[index]], dtype)], names=[names[index]])


@dataclass(frozen=True)
class Facts:
    phase: int
    status: int | None
    sqlstate: str | None


def _facts(error, phase=0):
    status = getattr(error, "status_code", None)
    state = getattr(error, "sqlstate", None)
    if not isinstance(status, (int, adm.AdbcStatusCode)) or not 0 <= int(status) <= 14:
        status = None
    if not isinstance(state, str) or re.fullmatch(r"[0-9A-Z]{5}", state) is None:
        state = None
    return Facts(phase, None if status is None else int(status), state)


class UnsafeBoundary(RuntimeError):
    def __init__(self, operation, phase, original=None, cleanup=None):
        self.operation, self.original, self.cleanup = operation, original, cleanup
        self.facts = _facts(original, phase)
        self.cleanup_facts = None if cleanup is None else _facts(cleanup, 59)
        super().__init__("unresolved scalar boundary")


def _expected_rejection(error, index):
    reason = (
        "signed integer exceeds target"
        if index < 4
        else "unsigned integer exceeds target"
        if index < 6
        else "finite float32 overflow"
        if index == 7
        else "inexact float32 narrowing"
    )
    return (
        isinstance(error, adm.Error)
        and _facts(error, 56) == Facts(56, 5, None)
        and getattr(error, "sqlstate", None) is None
        and getattr(error, "vendor_code", None) is None
        and not getattr(error, "details", ())
        and str(error) == "INVALID_ARGUMENT: [Redshift] Invalid scalar write: " + reason
    )


def _write(session, stream, route, table, batch, rejection=None):
    statement = None
    phase, original, cleanup = 51, None, None
    session.charge()
    try:
        statement = adm.AdbcStatement(session.connection)
        phase = 52
        if route == "direct":
            columns = ",".join(f'"{name}"' for name in NAMES)
            params = ",".join(f"${i}" for i in range(1, 9))
            statement.set_sql_query(
                f'INSERT INTO "{session.schema}"."{table}" '
                f"({columns}) VALUES ({params})"
            )
            phase = 53
            statement.prepare()
        else:
            statement.set_options(
                **{
                    "adbc.ingest.target_table": table,
                    "adbc.ingest.target_db_schema": session.schema,
                    "adbc.ingest.mode": (
                        adm.INGEST_OPTION_MODE_CREATE
                        if route == "create"
                        else adm.INGEST_OPTION_MODE_APPEND
                    ),
                }
            )
        phase = 55
        if stream:
            batches = [batch.slice(0, 0), batch.slice(0, 1)]
            if batch.num_rows > 1:
                batches.append(batch.slice(1))
            statement.bind_stream(
                pa.RecordBatchReader.from_batches(batch.schema, batches)
            )
        else:
            statement.bind(batch)
        phase = 56
        statement.execute_update()
    except Exception as error:
        original = error
    finally:
        if statement is not None:
            try:
                statement.close()
            except Exception as error:
                cleanup = error
    accepted = (
        rejection is not None
        and phase == 56
        and original is not None
        and _expected_rejection(original, rejection)
    )
    if (
        cleanup is not None
        or (rejection is None and original is not None)
        or (rejection is not None and not accepted)
    ):
        raise UnsafeBoundary(route, phase, original, cleanup) from original


def _identity(session):
    table = session.query(
        "SELECT current_database() AS db,current_user AS usr,current_schema() AS sch"
    )
    if (
        table.num_rows != 1
        or table.num_columns != 3
        or table.to_pylist()
        != [{"db": session.database, "usr": session.username, "sch": session.schema}]
    ):
        raise ValueError("scalar identity mismatch")


def run(session, stream):
    if (
        type(stream) is not bool
        or type(session.schema) is not str
        or re.fullmatch(r"adbc_types_[0-9a-f]{32}", session.schema) is None
    ):
        raise ValueError("exact owned scalar schema")
    direct, ingest = f"scalar_direct_{int(stream)}", f"scalar_ingest_{int(stream)}"
    phase = 60
    try:
        _identity(session)
        phase = 61
        columns = ",".join(f'"{n}" {t}' for n, t in zip(NAMES, SQL_TYPES))
        session.execute(f'CREATE TABLE "{session.schema}"."{direct}" ({columns})')
        for route in ("direct", "create", "append"):
            table = direct if route == "direct" else ingest
            _write(session, stream, route, table, source_batch(route))
            select = f'SELECT * FROM "{session.schema}"."{table}"'
            phase = 62
            compare(
                session.query(select + " ORDER BY row_id"),
                expected_table(append=route == "append"),
            )
            phase = 63
            compare(session.query(select + " WHERE false"), expected_table(empty=True))
        for index in range(9):
            _write(session, stream, "append", ingest, negative_batch(index), index)
            phase = 60
            _identity(session)
            phase = 62
            compare(
                session.query(
                    f'SELECT * FROM "{session.schema}"."{ingest}" ORDER BY row_id'
                ),
                expected_table(append=True),
            )
    except UnsafeBoundary:
        raise
    except Exception as error:
        raise UnsafeBoundary("observe", phase, error) from error
    session.scalar_receipt = {"positive_routes": 3, "rejections": 9}
    return True
