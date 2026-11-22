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

"""Finite HLL crossing qualification using a supplied, bounded owned session.

Session supplies connection/schema/database/username, charge(), execute(sql) and
query(sql)->Table. execute/query charge their own call. No connection, credentials
or cleanup SQL is created here. discover costs at most23 calls; each qualify costs
six. The owner stops SQL after UnsafeTransition and owns teardown. The crossing is
one observed adjacent pair in this fixture, not a universal or first threshold.
Native HLL serializations are observed before and after rebinding; this is an
observable roundtrip invariant, not an independent raw-wire decoding oracle.
HLL_CARDINALITY and the separately constructed prefix/union give server semantic
controls. HLL values are projected directly: Redshift does not support the former
HLL-to-VARCHAR cast used by this fixture.
"""

from __future__ import annotations

import base64
import json
import re
from dataclasses import dataclass

import pyarrow as pa

import adbc_driver_manager as adm

MAX_MIDPOINTS = 12
MAX_SERIALIZED_BYTES = 65535
HLL_METADATA = {
    b"ARROW:extension:name": b"adbc.redshift.hllsketch",
    b"ARROW:extension:metadata": (
        b'{"version":1,"encoding":"redshift-serialized","logm":15}'
    ),
    b"ADBC:redshift:sql_type": b"HLLSKETCH",
}


class Mismatch(RuntimeError):
    """Only fixed diagnostic labels; never serialize SQL, values or server text."""


class UnsafeTransition(RuntimeError):
    def __init__(self, phase, original, cleanup=None):
        self.phase, self.original, self.cleanup = phase, original, cleanup
        self.status, self.sqlstate = _facts(original)
        self.cleanup_status, self.cleanup_sqlstate = _facts(cleanup)
        super().__init__("unresolved HLL transition qualification")


def _facts(error):
    status = getattr(error, "status_code", None)
    state = getattr(error, "sqlstate", None)
    if not isinstance(status, (int, adm.AdbcStatusCode)) or not 0 <= int(status) <= 14:
        status = None
    if type(state) is not str or re.fullmatch(r"[0-9A-Z]{5}", state) is None:
        state = None
    return None if status is None else int(status), state


def _observe(phase, function):
    try:
        return function()
    except UnsafeTransition:
        raise
    except BaseException as error:
        raise UnsafeTransition(phase, error) from None


def relation(schema, name):
    if (
        type(schema) is not str
        or re.fullmatch(r"adbc_types_[0-9a-f]{32}", schema) is None
    ):
        raise ValueError("exact owned schema required")
    if name not in ("hll_keys", "hll_crossing", "hll_rebound_0", "hll_rebound_1"):
        raise ValueError("closed HLL fixture table")
    return f'"{schema}"."{name}"'


def _identity(session):
    relation(session.schema, "hll_keys")
    if not all(type(x) is str and x for x in (session.database, session.username)):
        raise ValueError("explicit expected identity required")
    actual = session.query("SELECT current_database(),current_user")
    if (
        not isinstance(actual, pa.Table)
        or actual.num_rows != 1
        or [c.to_pylist() for c in actual.columns]
        != [[session.database], [session.username]]
    ):
        raise Mismatch("identity")


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise Mismatch("unknown-representation")
        result[key] = value
    return result


def representation(value):
    """Classify the documented envelope; retain the original opaque string.

    No dense headers, ranks, threshold or register semantics are inferred. The
    finite fixture cap is smaller than the driver's general serialization limit.
    """
    if (
        type(value) is not str
        or not 0 < len(value.encode("utf-8")) <= MAX_SERIALIZED_BYTES
    ):
        raise Mismatch("serialization-bound")
    try:
        decoded = json.loads(value, object_pairs_hook=_unique_object)
        if type(decoded) is str:
            # Classification only: never replace the original with re-encoded text.
            if not re.fullmatch(r'"[A-Za-z0-9+/]+={0,2}"', value):
                raise ValueError("dense envelope")
            raw = base64.b64decode(decoded, validate=True)
            if not raw or base64.b64encode(raw).decode("ascii") != decoded:
                raise ValueError("dense padding")
            return "dense"
        if type(decoded) is not dict or set(decoded) != {"version", "logm", "sparse"}:
            raise ValueError("sparse envelope")
        if type(decoded["version"]) is not int or decoded["version"] != 1:
            raise ValueError("version")
        if type(decoded["logm"]) is not int or decoded["logm"] != 15:
            raise ValueError("logm")
        sparse = decoded["sparse"]
        if type(sparse) is not dict or set(sparse) != {"indices", "values"}:
            raise ValueError("sparse members")
        arrays = [sparse[k] for k in ("indices", "values")]
        if any(
            type(a) is not list or any(type(x) is not int or x < 0 for x in a)
            for a in arrays
        ):
            raise ValueError("sparse arrays")
        if len(arrays[0]) != len(arrays[1]):
            raise ValueError("sparse lengths")
        return "sparse"
    except (ValueError, TypeError, RecursionError):
        raise Mismatch("unknown-representation") from None


@dataclass(frozen=True)
class Observation:
    count: int
    serialized: str
    cardinality: int
    form: str


@dataclass(frozen=True)
class Fixture:
    schema: str
    database: str
    username: str
    crossing: int
    observations: tuple[Observation, ...]
    # Row order: prefix n-1, prefix n, prefix n+1, singleton n-1, union, NULL.
    serialized: tuple[str | None, ...]
    cardinalities: tuple[int | None, ...]


def adjacent_crossing(observe):
    """Retain an adjacent sparse/dense bracket without global monotonicity claims."""
    history = []

    def read(count):
        item = observe(count)
        if (
            not isinstance(item, Observation)
            or item.count != count
            or item.form != representation(item.serialized)
        ):
            raise Mismatch("probe-identity")
        if type(item.cardinality) is not int or item.cardinality < 0:
            raise Mismatch("cardinality")
        history.append(item)
        return item

    low, high = read(4), read(4096)
    if low.form != "sparse" or high.form != "dense":
        raise Mismatch("endpoint-bracket")
    for _ in range(MAX_MIDPOINTS):
        if high.count - low.count == 1:
            break
        mid = read((low.count + high.count) // 2)
        if mid.form == "sparse":
            low = mid
        else:
            high = mid
    if high.count - low.count != 1:
        raise Mismatch("midpoint-bound")
    return high.count, tuple(history)


def _scalar_rows(table, counts):
    """Read native HLL plus ordinary identity/cardinality scalar columns.

    The legacy helper name does not imply a separate scalar serialization path.
    Require the native HLL contract; plain UTF-8 is not evidence of HLL identity.
    """
    rows = len(counts)
    if (
        not isinstance(table, pa.Table)
        or table.column_names != ["row_id", "input_count", "serialized", "cardinality"]
        or table.num_rows != rows
    ):
        raise Mismatch("scalar-shape")
    if [f.type for f in table.schema] != [
        pa.int32(),
        pa.int32(),
        pa.large_string(),
        pa.int64(),
    ]:
        raise Mismatch("scalar-types")
    expected_hll = pa.field(
        "serialized",
        pa.large_string(),
        metadata={**HLL_METADATA, b"POSTGRESQL:type": b"hllsketch"},
    )
    if not table.schema.field("serialized").equals(expected_hll, check_metadata=True):
        raise Mismatch("scalar-types")
    values = table.to_pydict()
    if values["row_id"] != list(range(rows)) or values["input_count"] != list(counts):
        raise Mismatch("scalar-identity")
    for index, (value, cardinality) in enumerate(
        zip(values["serialized"], values["cardinality"])
    ):
        if rows == 6 and index == 5:
            if value is not None or cardinality is not None:
                raise Mismatch("null")
        elif type(cardinality) is not int or cardinality < 0:
            raise Mismatch("cardinality")
        else:
            representation(value)
    return tuple(values["serialized"]), tuple(values["cardinality"])


def input_counts(crossing):
    if type(crossing) is not int or not 5 <= crossing <= 4096:
        raise ValueError("finite crossing")
    return (crossing - 1, crossing, crossing + 1, 1, crossing, None)


def scalar_sql(table):
    return (
        "SELECT row_id,input_count,value AS serialized,"
        f"HLL_CARDINALITY(value) AS cardinality FROM {table} ORDER BY row_id"
    )


def native_schema():
    return pa.schema(
        [
            pa.field(
                "row_id",
                pa.int32(),
                metadata={
                    b"POSTGRESQL:type": b"int4",
                    b"ADBC:redshift:sql_type": b"INTEGER",
                    b"ADBC:redshift:type_modifier": b"-1",
                },
            ),
            pa.field(
                "input_count",
                pa.int32(),
                metadata={
                    b"POSTGRESQL:type": b"int4",
                    b"ADBC:redshift:sql_type": b"INTEGER",
                    b"ADBC:redshift:type_modifier": b"-1",
                },
            ),
            pa.field(
                "value",
                pa.large_string(),
                metadata={**HLL_METADATA, b"POSTGRESQL:type": b"hllsketch"},
            ),
        ]
    )


def _native_read(session, table, serialized, counts, empty=False):
    sql = f"SELECT row_id,input_count,value FROM {table}" + (
        " WHERE 1=0" if empty else " ORDER BY row_id"
    )
    actual = session.query(sql)
    expected = pa.Table.from_arrays(
        [
            pa.array([] if empty else range(6), pa.int32()),
            pa.array([] if empty else counts, pa.int32()),
            pa.array([] if empty else serialized, pa.large_string()),
        ],
        schema=native_schema(),
    )
    if not isinstance(actual, pa.Table) or not actual.equals(
        expected, check_metadata=True
    ):
        raise Mismatch("native-bytes-or-schema")


def discover(session):
    """At most23 charged operations; never retry or enlarge the producer."""
    _observe("identity", lambda: _identity(session))
    keys = relation(session.schema, "hll_keys")
    table = relation(session.schema, "hll_crossing")
    _observe("keys-create", lambda: session.execute(f"CREATE TABLE {keys} (n INTEGER)"))
    digits = "(" + " UNION ALL ".join(f"SELECT {n} AS d" for n in range(10)) + ")"
    # Exactly the retained prefix keys, with four decimal digits rather than
    # five because this fixture needs only0..4096 (10,000 candidates maximum).
    number = "a.d+10*b.d+100*c.d+1000*d.d"
    _observe(
        "keys-insert",
        lambda: session.execute(
            f"INSERT INTO {keys} SELECT {number} FROM {digits} a "
            f"CROSS JOIN {digits} b CROSS JOIN {digits} c "
            f"CROSS JOIN {digits} d WHERE {number}<4097"
        ),
    )

    def probe(count):
        sql = (
            f"SELECT 0::INTEGER AS row_id,{count}::INTEGER AS input_count,"
            "value AS serialized,"
            "HLL_CARDINALITY(value) AS cardinality "
            f"FROM (SELECT HLL_CREATE_SKETCH(n) AS value FROM {keys} "
            f"WHERE n<{count}) AS sketch"
        )
        values, cards = _scalar_rows(session.query(sql), [count])
        return Observation(count, values[0], cards[0], representation(values[0]))

    crossing, history = _observe("bracket", lambda: adjacent_crossing(probe))
    _observe(
        "witness-create",
        lambda: session.execute(
            f"CREATE TABLE {table} (row_id INTEGER,input_count INTEGER,value HLLSKETCH)"
        ),
    )
    branches = [
        f"SELECT {i},{n},HLL_CREATE_SKETCH(n) FROM {keys} WHERE n<{n}"
        for i, n in enumerate((crossing - 1, crossing, crossing + 1))
    ]
    branches.append(
        f"SELECT 3,1,HLL_CREATE_SKETCH(n) FROM {keys} WHERE n={crossing - 1}"
    )
    _observe(
        "witness-insert",
        lambda: session.execute(f"INSERT INTO {table} " + " UNION ALL ".join(branches)),
    )
    _observe(
        "combine-insert",
        lambda: session.execute(
            f"INSERT INTO {table} SELECT 4,{crossing},HLL_COMBINE(value) "
            f"FROM {table} WHERE row_id IN (0,3) "
            "UNION ALL SELECT 5,NULL::INTEGER,NULL::HLLSKETCH"
        ),
    )
    counts = input_counts(crossing)
    values, cards = _observe(
        "scalar-witness", lambda: _scalar_rows(session.query(scalar_sql(table)), counts)
    )

    def check():
        if (
            representation(values[0]) != "sparse"
            or representation(values[1]) != "dense"
        ):
            raise Mismatch("witness-bracket")
        observed = {item.count: item for item in history}
        # Independent re-execution must retain representation and cardinality;
        # serialization canonicality is not presumed even for the same inputs.
        if cards[:2] != (
            observed[crossing - 1].cardinality,
            observed[crossing].cardinality,
        ):
            raise Mismatch("witness-cardinality")
        if cards[3] != 1 or cards[4] != cards[1]:
            raise Mismatch("native-union")

    _observe("witness-check", check)
    _observe("native-read", lambda: _native_read(session, table, values, counts))
    _observe("empty-read", lambda: _native_read(session, table, values, counts, True))
    return Fixture(
        session.schema,
        session.database,
        session.username,
        crossing,
        history,
        values,
        cards,
    )


def _bind(session, table, batch, stream):
    statement = original = cleanup = None
    phase = "charge"
    try:
        session.charge()
        phase = "statement-new"
        statement = adm.AdbcStatement(session.connection)
        phase = "set-sql"
        statement.set_sql_query(
            f"INSERT INTO {table} (row_id,input_count,value) VALUES ($1,$2,$3)"
        )
        phase = "prepare"
        statement.prepare()
        phase = "bind-stream" if stream else "bind-array"
        if stream:
            statement.bind_stream(
                pa.RecordBatchReader.from_batches(
                    batch.schema, [batch.slice(0, 0), batch.slice(0, 1), batch.slice(1)]
                )
            )
        else:
            statement.bind(batch)
        phase = "execute"
        statement.execute_update()
    except BaseException as error:
        original = error
    finally:
        if statement is not None:
            try:
                statement.close()
            except BaseException as error:
                cleanup = error
    if original is not None or cleanup is not None:
        raise UnsafeTransition(phase, original, cleanup) from None


def qualify(session, fixture, stream):
    """Six charged operations; observed native HLL bytes are rebound unchanged."""
    if (
        type(stream) is not bool
        or not isinstance(fixture, Fixture)
        or (fixture.schema, fixture.database, fixture.username)
        != (session.schema, session.database, session.username)
    ):
        raise ValueError("fixture ownership")
    counts = input_counts(fixture.crossing)
    if len(fixture.serialized) != 6 or len(fixture.cardinalities) != 6:
        raise ValueError("finite fixture rows")
    table = relation(session.schema, f"hll_rebound_{int(stream)}")
    _observe("identity", lambda: _identity(session))
    _observe(
        "rebound-create",
        lambda: session.execute(
            f"CREATE TABLE {table} (row_id INTEGER,input_count INTEGER,value HLLSKETCH)"
        ),
    )
    batch = pa.RecordBatch.from_arrays(
        [
            pa.array(range(6), pa.int32()),
            pa.array(counts, pa.int32()),
            pa.array(fixture.serialized, pa.large_string()),
        ],
        schema=pa.schema(
            [
                pa.field("row_id", pa.int32()),
                pa.field("input_count", pa.int32()),
                pa.field("value", pa.large_string(), metadata=HLL_METADATA),
            ]
        ),
    )
    _bind(session, table, batch, stream)

    def compare_scalar():
        if _scalar_rows(session.query(scalar_sql(table)), counts) != (
            fixture.serialized,
            fixture.cardinalities,
        ):
            raise Mismatch("rebound-scalar")

    _observe("rebound-scalar", compare_scalar)
    _observe(
        "native-read", lambda: _native_read(session, table, fixture.serialized, counts)
    )
    _observe(
        "empty-read",
        lambda: _native_read(session, table, fixture.serialized, counts, True),
    )
