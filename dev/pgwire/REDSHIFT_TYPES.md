<!---
  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing,
  software distributed under the License is distributed on an
  "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
  KIND, either express or implied.  See the License for the
  specific language governing permissions and limitations
  under the License.
-->

# First-release Redshift type contract

Audit: 2026-10-18, source `3460c541914560a91872e4417c4c3b4ccbb81df0`.
This is a design and evidence inventory, not implemented all-type support.
**All 21 table types are first-release work before Task009.** Aliases resolve to
canonical server types; recovering the original CREATE spelling is not required.
The [AWS list](https://docs.aws.amazon.com/redshift/latest/dg/c_Supported_data_types.html)
is the inventory boundary: PostgreSQL UUID/JSON/JSONB/arrays/domains/ranges/money,
quoted `"char"`, and SUPER's nested arrays/objects are not additional Redshift
SQL column types. NCHAR/NVARCHAR also have NATIONAL CHARACTER / NATIONAL CHARACTER
VARYING spellings documented in [character types](https://docs.aws.amazon.com/redshift/latest/dg/r_Character_types.html).

## Current evidence, by operation

`C`: code present; `O-PG`: existing PostgreSQL codec/schema offline assertions
passed in [CI 38899143218](https://github.com/vahid110/arrow-adbc/actions/runs/38899143218)
(the audited codec/discovery/reader/binder sources are unchanged since that head).
`T`: opt-in Redshift test declaration, not a qualification receipt.
`L`: narrowly live proven by [run 39138655470](https://github.com/vahid110/arrow-adbc/actions/runs/39138655470).
`NQ`: Redshift behavior not qualified; `N`: no semantic implementation.
`opaque` means unsupported external bytes, **not full type support**.
C/O-PG/T without L remains NQ; no row is a claim of full-range support.
Create and append are separate in the fourth column. Metadata is current schema/
canonical name evidence, not proof of precision, scale, length or qualifier.

| SQL column type and aliases (21 entries) | Native read | Bind | Create / append | Metadata | Recommended representation |
|---|---|---|---|---|---|
| SMALLINT / INT2 | C,O-PG,T | C,O-PG | C / C | C-name | int16 |
| INTEGER / INT / INT4 | C,O-PG,T | C,O-PG,T | C / C,T | C-name,T | int32 |
| BIGINT / INT8 | C,O-PG,T,L | C,O-PG,L | C,L / C,L | C-name,L | int64 |
| DECIMAL / NUMERIC | C-string,O-PG,T | C-decimal,O-PG | C-defaults / C | C-name; modifiers N | decimal128(p,s); exact text for unresolved expression modifiers |
| REAL / FLOAT4 | C,O-PG,T | C,O-PG | C / C | C-name | float32 |
| DOUBLE PRECISION / FLOAT8 / FLOAT | C,O-PG,T | C,O-PG | C / C | C-name | float64 |
| CHAR / CHARACTER / NCHAR / BPCHAR | C,O-PG | N distinct CHAR; string coercion C | N CHAR / C-coercion | C-name; width N | utf8 + CHAR width/identity |
| VARCHAR / CHARACTER VARYING / NVARCHAR / TEXT | C,O-PG,T | C,O-PG | C-defaults / C,T | C-name,T; width N | utf8/large_utf8 + byte limit |
| DATE | C,O-PG,T | C-date32,O-PG | C / C | C-name | date32 |
| TIME / TIME WITHOUT TIME ZONE | C,O-PG,T | C-time64(us),O-PG | C / C | C-name; precision N | time64(us); endpoint decision below |
| TIMETZ / TIME WITH TIME ZONE | C-opaque,NQ | N semantic mapping | N / N | C-name only | explicit offset-time representation, below |
| TIMESTAMP / TIMESTAMP WITHOUT TIME ZONE | C,O-PG,T | C,O-PG | C / C | C-name; precision N | timestamp(us), no timezone |
| TIMESTAMPTZ / TIMESTAMP WITH TIME ZONE | C,O-PG | C,O-PG | C / C | C-name; precision N | timestamp(us, UTC), server-retained instant |
| INTERVAL YEAR TO MONTH | C-opaque,NQ | N family mapping | N / N | C-name only | month_day_nano_interval(months,0,0) |
| INTERVAL DAY TO SECOND | C-opaque,NQ | N family mapping | N / N | C-name only | duration(us) + interval family/qualifier |
| BOOLEAN / BOOL | C,O-PG,T | C,O-PG | C / C | C-name | bool |
| HLLSKETCH | C-opaque,NQ | N | N / N | C-name only | proposed versioned sketch serialization |
| SUPER | C-opaque,NQ; serialized VARCHAR test only | N | N / N | C-name only | JSON extension only after losslessness proof |
| VARBYTE / VARBINARY / BINARY VARYING | C-alias,O-PG,T | C-binary,NQ | C-lookup / C | C-name; length N | binary/large_binary |
| GEOMETRY | C-opaque,NQ | N | N / N | C-name only | typed WKB/SRID representation |
| GEOGRAPHY | C-opaque,NQ | N | N / N | C-name only | typed WKB/SRID + geodetic identity |

L covers installed C archive and Linux x86-64 wheel, temporary IAM/verify-full TLS,
query/parameter value 42, small int64 create+append, schema/roundtrip and owned
fixture cleanup. Python also checked invalid-replace preservation. Independent
physical/secret/IAM absence was verified separately after the helper's original
namespace-removal uncertainty; no receipt was relabeled. This is not all-types,
all-platform or every-API qualification; actual dollars remain unreconciled.

## Source audit and gaps

- Discovery: [type_discovery.cc](../../c/driver/postgresql/type_discovery.cc#L89)
  reads catalog OIDs/receive functions; [backend.h](../../c/driver/pgwire/backend.h#L96)
  aliases only `varbyte_recv`. [PostgresTypeResolver](../../c/driver/postgresql/postgres_type.h#L507)
  resolves by receive-function name, not SQL spelling. Unknown receive functions
  become user-defined; missing catalog entries become unnamed opaque OIDs. Do not
  hardcode observed OIDs or alias new vendor families to PostgreSQL layouts.
- Read/schema: [result_reader.cc](../../c/driver/postgresql/result_reader.cc#L203)
  requests **binary** extended-query output and parameters, then calls
  `MakeCopyFieldReader`. Backend `kText` means non-COPY execution, not text wire
  fields. [SetSchema](../../c/driver/postgresql/postgres_type.h#L216) maps numeric
  to string with `arrow.opaque`, TIMETZ to opaque binary, and generic PostgreSQL
  interval to month/day/nanosecond. This does not implement either Redshift interval
  family. Unknown bytes can be useful diagnostics without proving a native codec.
- Modifiers: [ResolveOutputTypes](../../c/driver/postgresql/result_helper.cc#L163),
  [GetTableSchema](../../c/driver/postgresql/connection.cc#L941) and
  [TableSchema SQL](../../c/driver/postgresql/metadata.cc#L110) use OID/name but no
  result typmod/column modifiers. [GetObjects](../../c/driver/postgresql/connection.cc#L190)
  fills `xdbc_type_name`, not numeric precision/scale/width. Schema-only, empty
  results, parameter schemas and populated reads must agree without sampling rows.
- Binding: [BindStream::SetParamTypes](../../c/driver/postgresql/bind_stream.h#L144)
  normally derives binary parameter OIDs/writers from Arrow; only Arrow NA uses
  inferred expected types. Existing PostgreSQL `arrow.json` mapping is not SUPER.
  Opaque readback is not safely round-trippable. TIME32/DATE64 map to candidate DB
  types but lack matching writer cases; TIME64 accepts only microseconds.
- Ingest: [CREATE mapping](../../c/driver/postgresql/statement.cc#L743) calls
  [sql_type_name](../../c/driver/postgresql/postgres_type.h#L200) without modifiers:
  decimal(p,s), text widths and CHAR identity are lost. Redshift
  [ExecutePreparedRows](../../c/driver/postgresql/bind_stream.h#L535) uses Arrow-derived
  parameter types, not the COPY target-type writers, even when append discovered
  a target. A read alias does not prove correct VARBYTE DDL/parameter identity.
  The staged CSV helper is not selected by the supported ingest path.
- Scalar hazards: [writer.h](../../c/driver/postgresql/copy/writer.h#L170) uses
  narrowing casts; timestamp nanoseconds divide by 1000, as do generic interval
  nanoseconds. Reject overflow/nonrepresentable fractions, not silent wrapping or
  truncation. Timestamp reader epoch overflow already errors; full Redshift range,
  TIME 24:00 and timezone semantics still need explicit acceptance.
- Tests: [RedshiftSmokeTest](../../c/driver/postgresql/redshift_test.cc#L387) declares
  scalar/null/edge/metadata cases, including `DECIMAL(38,0)`, but SUPER coverage is
  explicitly `JSON_SERIALIZE`, not native SELECT. [reader tests](../../c/driver/postgresql/copy/postgres_copy_reader_test.cc#L536),
  [writer tests](../../c/driver/postgresql/copy/postgres_copy_writer_test.cc#L451)
  and [schema tests](../../c/driver/postgresql/postgres_type_test.cc#L127) prove
  PostgreSQL fixtures, not all Redshift types. No dedicated family tests exist for
  the two Redshift intervals, TIMETZ, native SUPER, HLLSKETCH or spatial semantics.

The [official Python connector source](https://github.com/aws/amazon-redshift-python-driver/blob/248ffd5896c7a46bba115452dc10b8389d3ede54/redshift_connector/utils/type_utils.py)
(at pinned commit `248ffd5896c7a46bba115452dc10b8389d3ede54`) distinguishes
`intervaly2m` signed 4-byte months and `intervald2s` signed 8-byte microseconds; the
current generic reader expects 16 bytes. Its numeric binary codec takes scaled 8/16
bytes, while this driver parses PostgreSQL numeric digits. **Protocol evidence
requirement, not proof of a current decimal bug**: the connector
[negotiates client_protocol_version](https://github.com/aws/amazon-redshift-python-driver/blob/248ffd5896c7a46bba115452dc10b8389d3ede54/redshift_connector/core.py#L811).
Each libpq path needs its own qualified descriptor/format/fixture evidence. No wire
bytes or OIDs are invented here, and no upstream implementation is copied.

## Recommended lossless contract and acceptance

These are proposals for bounded implementation PRs. A representation is supported
only after native SELECT, typed binding, create, append and metadata each have
identified tests/evidence. No per-query user casts, silent coercion, undocumented
bytes or opaque fallback can satisfy the all-type milestone.

1. **Shared schema contract.** Keep Arrow validity for SQL NULL, including all-null,
   empty results and mixed-type projections. Proposed custom field keys
   under the `ADBC:redshift:` prefix (`typname`, `precision`, `scale`, `length`,
   `interval_qualifier`, `fractional_precision`) preserve canonical DB identity/modifiers when available;
   omit unknown modifiers rather than invent defaults. These are driver proposals,
   not standardized ADBC keys; preserve existing metadata compatibility. Bind and
   generated DDL must honor validated type identity, independent of storage type.
   Aliases must agree across query schema, ExecuteSchema, parameter schema,
   GetTableSchema and GetObjects. Native table columns and computed expressions
   require separate tests; a schema must stay stable across batches.
2. **Numbers/boolean.** [AWS numeric rules](https://docs.aws.amazon.com/redshift/latest/dg/r_Numeric_types201.html):
   signed minima/maxima and overflow on every integer width; reject oversized
   Arrow unsigned inputs. Check decimals at precision 1/18/19/20/38 and scale 0/2/37,
   negative/zero/trailing zeros and exact rescaling. Precision 19 has a signed64
   coefficient limit; preserve p/s in DDL. Prefer decimal128(p,s) when modifiers are
   reliable; expression results with unknown p/s need a separately specified exact
   decimal-text representation, never float or row-inferred schema. Arrow decimal256
   is accepted only when exactly representable in Redshift. Float tests cover +/-0,
   finite extremes, subnormals where retained by the server, NaN and +/-Infinity;
   promise numeric semantics, not NaN payload bits.
   [BOOLEAN](https://docs.aws.amazon.com/redshift/latest/dg/r_Boolean_type.html) tests require true,
   false and SQL NULL; accepted literal spellings are server conversions, not new types.
3. **Characters/binary.** [Stored CHAR/VARCHAR](https://docs.aws.amazon.com/redshift/latest/dg/r_Character_types.html)
   use byte widths: CHAR(4096)/single-byte, VARCHAR(65535)/UTF-8; TEXT and BPCHAR have
   width 256 defaults. Preserve CHAR padding, empty versus NULL, quotes/backslashes,
   trailing spaces, four-byte UTF-8 and invalid-input rejection. In-memory VARCHAR
   expressions/string functions now allow 16,000,000 bytes: do not impose a universal
   64 KB result cap. [VARBYTE](https://docs.aws.amazon.com/redshift/latest/dg/r_VARBYTE_type.html)
   reaches 16,777,216 bytes (default 64000); test zero bytes, high bits, empty/NULL,
   boundary sizes, and aggregate batch-offset overflow. Values over 1 MB have
   file-format ingest restrictions. Plain parameterized INSERT must reject an
   unsupported size before effects; a separately approved staged route would be
   required for those writes, not a hidden truncation or automatic S3 operation.
4. **Temporal.** [AWS datetime semantics](https://docs.aws.amazon.com/redshift/latest/dg/r_Datetime_types.html)
   advertise DATE/TIMESTAMP years 4713 BC..294276 AD and TIME 00:00..24:00.
   Require microsecond precision, BC dates/epoch boundaries, fractional
   second endpoints and signed timezone offsets. TIMESTAMP stays timezone-free;
   TIMESTAMPTZ preserves the UTC instant, not an original offset/name discarded
   by the server. TIMETZ needs an explicit offset-time extension/struct if an
   offset is server-retained; never silently drop it. [Arrow's type definitions](https://github.com/apache/arrow/blob/main/format/Schema.fbs)
   exclude24:00 from Time and bound timestamp(us) to int64. At these mismatches,
   explicit checked rejection is the current limit; a documented lossless
   full-range representation remains a release design gate, not modulo-day wrapping,
   infinity substitution or nanosecond truncation. Cover session timezone changes,
   DST instants and restoration after bind failure without promising server-lost information.
5. **Both intervals.** [AWS qualifiers](https://docs.aws.amazon.com/redshift/latest/dg/r_interval_data_types.html):
   YEAR, MONTH, YEAR TO MONTH; DAY, HOUR, MINUTE, SECOND, DAY TO HOUR,
   DAY TO MINUTE, DAY TO SECOND, HOUR TO MINUTE, HOUR TO SECOND, MINUTE TO SECOND. SECOND-bearing forms accept fractional
   precision 0..6. Prefer month_day_nano_interval with (months,0,0) and duration(us)
   for day/second signed elapsed microseconds, retaining SQL family/qualifier.
   [PyArrow](https://github.com/apache/arrow/blob/main/python/pyarrow/types.py#L47)
   does not support month-only/day-time interval types; the month/day/nanosecond
   layout avoids a C-only result. Require actual installed C and Python import
   tests; year/month uses zero days/nanoseconds, with no multiplication.
   Do not convert calendar months to days, use millisecond day_time (loses fractions),
   or multiply the whole int64 duration into nanoseconds. Check signs, zero,
   boundaries, null/empty/mixed projections, malformed lengths and all qualifiers;
   reflect server qualifier truncation without adding driver truncation. Generic
   PostgreSQL INTERVAL is not a third supported table family.
6. **SUPER.** [AWS SUPER](https://docs.aws.amazon.com/redshift/latest/dg/r_SUPER_type.html)
   can hold 16 MB and heterogeneous nested/scalar values; over 1 MB ingest has specific
   file-format restrictions. Prefer `arrow.json` on large_utf8 with Redshift identity
   only after native protocol evidence proves RFC8259, exact numeric lexemes and
   SQL NULL versus non-null JSON null. Test mixed numbers/strings/bools/null,
   empty/nested arrays/objects, case-sensitive keys, escapes and precision/size
   boundaries without decoding through host float. Full dynamic scalar subtype
   preservation/non-JSON values remain explicit design gates; a versioned
   `adbc.redshift.super` semantic extension may be needed. Do not label it complete
   by returning `arrow.opaque`. [JSON_SERIALIZE](https://docs.aws.amazon.com/redshift/latest/dg/JSON_SERIALIZE.html)
   returns VARCHAR, has a system-size limit and maps NULL to the JSON string null;
   it is not a lossless substitute for raw SELECT. The separate
   [VARBYTE serializer](https://docs.aws.amazon.com/redshift/latest/dg/JSON_SERIALIZE_TO_VARBYTE.html)
   is evidence for another path, not authority to rewrite arbitrary queries.
7. **HLLSKETCH.** Prefer a versioned `adbc.redshift.hllsketch` extension on large_utf8
   with documented Redshift sketch serialization and precision identity; do not
   imply ordinary text or JSON semantics. [Sparse JSON and dense Base64](https://docs.aws.amazon.com/redshift/latest/dg/r_HLLSKTECH_type.html)
   require distinct validation and roundtrip fixtures, including transitions,
   NULL, empty input and maximum size 24,580 bytes. Validate
   [logm=15 and engine restrictions](https://docs.aws.amazon.com/redshift/latest/dg/hyperloglog-functions-limitations.html).
   Preserve the sketch, not merely its estimated cardinality; native parameter/
   ingest acceptance remains unproved, and unsupported routes must reject clearly.
8. **GEOMETRY/GEOGRAPHY.** Prefer `geoarrow.wkb` only where its CRS/edge semantics fit.
   [GeoArrow](https://geoarrow.org/format.html#serialized-encodings) permits consumers
   to ignore embedded EWKB SRIDs; [column metadata](https://geoarrow.org/extension-types.html)
   cannot pretend a mixed-SRID result has one CRS. Otherwise propose separate
   `adbc.redshift.geometry` / `adbc.redshift.geography` semantic extensions over
   a documented WKB+per-value SRID representation, not undocumented opaque bytes.
   Preserve [planar versus geodetic identity](https://docs.aws.amazon.com/redshift/latest/dg/geospatial-overview.html),
   coordinate order, all seven shape families, XY/XYZ/XYM/XYZM, empty versus NULL,
   SRID 0/4326/other valid SRIDs and mixed-SRID rows; no implicit reprojection or
   invented spherical/ellipsoidal edge algorithm. Test shape equality and SRID/
   dimension preservation, malformed data and
   [maximum 1,048,447-byte objects](https://docs.aws.amazon.com/redshift/latest/dg/spatial-limitations.html).

[Arrow canonical extensions](https://arrow.apache.org/docs/format/CanonicalExtensions.html)
reserve `arrow.json` for valid UTF-8 JSON and describe `arrow.opaque` as unsupported
external data. Custom names above are proposed driver contracts, not new Arrow
canonical types. Their exact versioned storage/metadata and cross-language behavior
must be reviewed before implementation; an unresolved representation blocks release.

## Bounded sequence (coordinator briefs define each implementation task)

- **010b next:** the two interval families' discovery/native read/schema/metadata
  only. Use backend-owned family classification with dynamic catalog OIDs; separate
  proposed 4/8-byte fixture formats from the pinned AWS reference and Arrow
  month_day_nano_interval(months,0,0)/duration(us); actual installed C/Python
  import checks are required, libpq-path qualification remains pending and
  unsupported field widths must reject. Carry canonical
  family identity consistently in result/empty/ExecuteSchema/GetTableSchema;
  recover qualifier/precision only from validated metadata, otherwise report unknown.
  Fixtures must cite the supported protocol, not assume matching receive names.
  Add PostgreSQL non-regression, null/sign/boundary/malformed/mixed-column tests and
  installed C/Python consumption in CI. Bind/create/append stay explicitly unsupported
  for these families until their bounded successor; this does not complete Task010.
- **010c:** type-modifier plumbing and scalar/decimal/temporal write/read gaps,
  including native-format evidence, exact DDL, target-aware append and loss checks.
- **010d:** native SUPER plus supported-size binding/ingest, with the extension/
  protocol decisions above; a mixed projection must work without user query rewrites.
- **010e:** HLLSKETCH and spatial native codecs/semantic contracts, split if needed;
  type-specific write/size restrictions stay explicit.
- **010f:** complete 21-row read/bind/create/append/metadata evidence on installed C
  and wheel artifacts, including documented unsupported operations and all aliases,
  followed by a separately human-authorized finite live qualification and teardown.
  No live run, AWS action, staged upload, publication or automatic retry is authorized
  by this document. Only then can Task009 release readiness be assessed.
