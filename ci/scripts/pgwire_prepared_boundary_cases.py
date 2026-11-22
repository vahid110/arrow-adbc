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

"""Finite installed vendor parameter boundaries on a supplied owned session.

No connections, credentials, environment discovery or cleanup SQL are created here.
``run(session, stream)`` requires connection/schema/database/username and bounded
execute(sql), query(sql)->pyarrow.Table, charge() methods. execute/query must charge
one operation themselves; this helper charges one per prepared operation. A group
performs exactly 31 operations on success. The owner must stop the connection on
UnsafeBoundary and retain its diagnostics. Server incompatibility candidates are
not descriptor-drift tests: a legitimate successful cast remains unresolved.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

import pyarrow as pa

import adbc_driver_manager as adm

FAMILIES = ("SUPER", "GEOMETRY", "GEOGRAPHY")
POINT = bytes.fromhex("0101000020e6100000000000000000f03f000000000000f03f")
LOCAL_MESSAGES = (
    "[Redshift] Invalid SUPER JSON input: "
    "unsupported or contradictory vendor schema/version",
    "[Redshift] Invalid spatial/HLL schema or storage contract",
    "[Redshift] Invalid spatial/HLL schema or storage contract",
)
NATIVE_PAIRS = frozenset(((5, "42804"), (5, "42846"), (3, "42883")))


@dataclass(frozen=True)
class ErrorFacts:
    """Closed diagnostics only: never native message, SQL, URI or input values."""

    phase: int
    status: int | None
    sqlstate: str | None


def error_facts(phase, error):
    if error is None:
        return ErrorFacts(phase, 0, None)
    status = getattr(error, "status_code", None)
    if not isinstance(status, (int, adm.AdbcStatusCode)) or not 0 <= int(status) <= 14:
        status = None
    state = getattr(error, "sqlstate", None)
    if not isinstance(state, str) or re.fullmatch(r"[0-9A-Z]{5}", state) is None:
        state = None
    return ErrorFacts(phase, None if status is None else int(status), state)


class PreparedFailure:
    """Keep operation and cleanup errors separately, including original objects."""

    def __init__(self, phase, original, cleanup):
        self.original = original
        self.cleanup = cleanup
        self.facts = error_facts(phase, original)
        self.cleanup_facts = None if cleanup is None else error_facts(59, cleanup)


class UnsafeBoundary(RuntimeError):
    """Stop SQL on this connection, including cleanup SQL.

    Family -1 is the initial identity check. Observation phases60/61/62/63 mean
    identity/DDL/full read/empty read; prepared phases51/52/53/55/56/59 mean
    new/set SQL/prepare/bind/execute/close. No message text enters ErrorFacts.
    """

    def __init__(self, family, control, reason, failure):
        self.family = family
        self.control = control
        self.reason = reason
        self.failure = failure
        self.facts = failure.facts
        self.cleanup_facts = failure.cleanup_facts
        super().__init__("unresolved vendor parameter boundary")


class BoundaryMismatch(RuntimeError):
    def __init__(self, reason):
        if reason not in ("identity", "rows", "schema", "values"):
            raise ValueError("closed boundary comparison")
        self.reason = reason
        super().__init__("vendor boundary comparison failed")


def _selection(family, control):
    if type(family) is not int or family not in range(3):
        raise ValueError("closed vendor family")
    if type(control) is not int or control not in range(3):
        raise ValueError("closed boundary control")


def table_name(schema, stream, family):
    _selection(family, 0)
    if (
        type(schema) is not str
        or re.fullmatch(r"adbc_types_[0-9a-f]{32}", schema) is None
        or type(stream) is not bool
    ):
        raise ValueError("exact owned vendor target")
    return f'"{schema}"."vendor_boundary_{int(stream)}_{family}"'


def source_batch(family, control):
    _selection(family, control)
    # Source declarations are independent from the stored-result schema below.
    source_metadata = (
        {
            b"ADBC:redshift:sql_type": b"SUPER",
            b"ARROW:extension:name": b"adbc.redshift.super.json",
            b"ARROW:extension:metadata": (
                b'{"version":1,"encoding":"utf8-json","scope":"json-subset-write"}'
            ),
        },
        {
            b"ADBC:redshift:sql_type": b"GEOMETRY",
            b"ARROW:extension:name": b"adbc.redshift.geometry.ewkb",
            b"ARROW:extension:metadata": (
                b'{"version":1,"encoding":"ewkb","family":"GEOMETRY",'
                b'"srid":"per-value","axes":"native"}'
            ),
        },
        {
            b"ADBC:redshift:sql_type": b"GEOGRAPHY",
            b"ARROW:extension:name": b"adbc.redshift.geography.ewkb",
            b"ARROW:extension:metadata": (
                b'{"version":1,"encoding":"ewkb","family":"GEOGRAPHY",'
                b'"srid":"per-value","axes":"native"}'
            ),
        },
    )
    metadata = source_metadata[family]
    if control == 1:
        if family == 0:
            metadata[b"ARROW:extension:metadata"] = (
                b'{"version":2,"encoding":"utf8-json","scope":"json-subset-write"}'
            )
        else:
            metadata[b"ADBC:redshift:sql_type"] = (
                b"GEOGRAPHY" if family == 1 else b"GEOMETRY"
            )
    value_type = pa.large_binary() if family else pa.large_string()
    schema = pa.schema(
        [
            pa.field("row_id", pa.int32()),
            pa.field("value", value_type, metadata=metadata),
        ]
    )
    return pa.RecordBatch.from_arrays(
        [
            pa.array([100, 101] if control else [0, 1], pa.int32()),
            pa.array([None, POINT if family else "true"], value_type),
        ],
        schema=schema,
    )


def _integer(name):
    return pa.field(
        name,
        pa.int32(),
        metadata={
            b"POSTGRESQL:type": b"int4",
            b"ADBC:redshift:sql_type": b"INTEGER",
            b"ADBC:redshift:type_modifier": b"-1",
        },
    )


def _stored_vendor(name, family, empty):
    # Literal read contract, not derived from source metadata or input Arrow.
    metadata = (
        {
            b"POSTGRESQL:type": b"super",
            b"ADBC:redshift:sql_type": b"SUPER",
            b"ADBC:redshift:type_modifier": b"-1" if empty else b"16384000",
            b"ARROW:extension:name": b"adbc.redshift.super.native_json",
            b"ARROW:extension:metadata": (
                b'{"version":1,"encoding":"redshift-json-wire",'
                b'"scope":"native-result","numbers":"lexemes",'
                b'"nulls":"wire-null"}'
            ),
        },
        {
            b"POSTGRESQL:type": b"geometry",
            b"ADBC:redshift:sql_type": b"GEOMETRY",
            b"ARROW:extension:name": b"adbc.redshift.geometry.ewkb",
            b"ARROW:extension:metadata": (
                b'{"version":1,"encoding":"ewkb","family":"GEOMETRY",'
                b'"srid":"per-value","axes":"native"}'
            ),
        },
        {
            b"POSTGRESQL:type": b"geography",
            b"ADBC:redshift:sql_type": b"GEOGRAPHY",
            b"ARROW:extension:name": b"adbc.redshift.geography.ewkb",
            b"ARROW:extension:metadata": (
                b'{"version":1,"encoding":"ewkb","family":"GEOGRAPHY",'
                b'"srid":"per-value","axes":"native"}'
            ),
        },
    )
    return pa.field(
        name,
        pa.large_binary() if family else pa.large_string(),
        metadata=metadata[family],
    )


def expected_table(family, empty=False):
    _selection(family, 0)
    if type(empty) is not bool:
        raise ValueError("closed read shape")
    schema = pa.schema(
        [
            _integer("row_id"),
            _stored_vendor("value", family, empty),
            _integer("incompatible")
            if family
            else _stored_vendor("incompatible", 1, empty),
        ]
    )
    point = bytes.fromhex("0101000020e6100000000000000000f03f000000000000f03f")
    columns = (
        ([], [], [])
        if empty
        else ([0, 1], [None, point if family else "true"], [None, None])
    )
    return pa.Table.from_arrays(
        [pa.array(values, field.type) for values, field in zip(columns, schema)],
        schema=schema,
    )


def compare(table, family, empty=False):
    expected = expected_table(family, empty)
    if not isinstance(table, pa.Table) or table.num_rows != expected.num_rows:
        raise BoundaryMismatch("rows")
    if not table.schema.equals(expected.schema, check_metadata=True):
        raise BoundaryMismatch("schema")
    if not table.equals(expected, check_metadata=True):
        raise BoundaryMismatch("values")


def prepared(session, stream, family, control):
    """Exactly one charged operation, with explicit error-preserving close."""
    table = table_name(session.schema, stream, family)
    data = source_batch(family, control)
    target = "incompatible" if control == 2 else "value"
    cast = ("::INTEGER" if family else "::GEOMETRY") if control == 2 else ""
    sql = f'INSERT INTO {table} ("row_id","{target}") VALUES ($1,$2{cast})'
    statement = None
    original = cleanup = None
    phase = 50
    try:
        session.charge()
        phase = 51
        statement = adm.AdbcStatement(session.connection)
        phase = 52
        statement.set_sql_query(sql)
        phase = 53
        statement.prepare()
        phase = 55
        if stream:
            statement.bind_stream(
                pa.RecordBatchReader.from_batches(
                    data.schema, [data.slice(0, 0), data.slice(0, 1), data.slice(1)]
                )
            )
        else:
            statement.bind(data)
        # If metadata validation unexpectedly succeeds, never execute that input.
        if control != 1:
            phase = 56
            statement.execute_update()
    except BaseException as error:
        original = error
    finally:
        if statement is not None:
            try:
                statement.close()
            except BaseException as error:
                cleanup = error
    failure = PreparedFailure(phase, original, cleanup)
    if original is None and cleanup is None and control == 0:
        return None
    if expected_rejection(failure, family, control):
        return failure.facts
    reason = (
        "unexpected-success"
        if original is None and cleanup is None
        else "unmatched-error"
    )
    raise UnsafeBoundary(family, control, reason, failure) from None


def expected_rejection(failure, family, control):
    _selection(family, control)
    error = failure.original
    if failure.cleanup is not None or not isinstance(error, adm.Error):
        return False
    if control == 1:
        return (
            failure.facts.phase == 55
            and type(error) is adm.ProgrammingError
            and error.status_code == adm.AdbcStatusCode.INVALID_ARGUMENT
            and error.sqlstate is None
            and error.vendor_code is None
            and not error.details
            and str(error) == "INVALID_ARGUMENT: " + LOCAL_MESSAGES[family]
        )
    return (
        control == 2
        and failure.facts.phase == 56
        and (failure.facts.status, error.sqlstate) in NATIVE_PAIRS
    )


def run(session, stream):
    """Three vendor families; 31 charged operations if every boundary qualifies."""
    # Validate before the first external operation; every table uses these values.
    table_name(session.schema, stream, 0)
    if not all(type(x) is str and x for x in (session.database, session.username)):
        raise ValueError("explicit expected identity")

    def observe(family, control, phase, call):
        try:
            return call()
        except BaseException as error:
            raise UnsafeBoundary(
                family,
                control,
                "observation-failed",
                PreparedFailure(phase, error, None),
            ) from None

    def identity(family, control):
        def check():
            result = session.query("SELECT current_database(),current_user")
            if (
                not isinstance(result, pa.Table)
                or result.num_rows != 1
                or [column.to_pylist() for column in result.columns]
                != [[session.database], [session.username]]
            ):
                raise BoundaryMismatch("identity")

        observe(family, control, 60, check)

    def read(select, family, control, empty=False):
        observe(
            family,
            control,
            63 if empty else 62,
            lambda: compare(session.query(select), family, empty),
        )

    receipts = []
    identity(-1, 0)
    for family in range(3):
        table = table_name(session.schema, stream, family)
        negative_type = "INTEGER" if family else "GEOMETRY"
        observe(
            family,
            0,
            61,
            lambda: session.execute(
                f'CREATE TABLE {table} ("row_id" INTEGER,"value" {FAMILIES[family]},'
                f'"incompatible" {negative_type})'
            ),
        )
        prepared(session, stream, family, 0)
        select = f'SELECT "row_id","value","incompatible" FROM {table}'
        read(select + ' ORDER BY "row_id"', family, 0)
        for control in (1, 2):
            facts = prepared(session, stream, family, control)
            receipts.append((family, control, facts))
            identity(family, control)
            read(select + ' ORDER BY "row_id"', family, control)
        read(select + " WHERE false", family, 0, True)
    return tuple(receipts)
