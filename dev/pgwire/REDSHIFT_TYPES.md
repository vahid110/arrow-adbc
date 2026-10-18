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
(baseline PostgreSQL evidence; Task010b changes are separately identified below).
`T`: opt-in Redshift test declaration, not a qualification receipt.
`L`: narrowly live proven by [run 39138655470](https://github.com/vahid110/arrow-adbc/actions/runs/39138655470).
`F`: Task010b offline protocol/C Data checks passed in PR22; Task010c1 adds
synthetic descriptor/parser and C Data metadata checks. Neither is captured
Redshift traffic or a live libpq qualification.
`NQ`: Redshift behavior not qualified; `N`: no semantic implementation.
`opaque` means unsupported external bytes, **not full type support**.
C/O-PG/T without L remains NQ; no row is a claim of full-range support.
Create and append are separate in the fourth column. Metadata is current schema/
canonical name evidence, not proof of precision, scale, length or qualifier.

| SQL column type and aliases (21 entries) | Native read | Bind | Create / append | Metadata | Recommended representation |
|---|---|---|---|---|---|
| SMALLINT / INT2 | C,O-PG,T,NQ | C-checked,NQ | C-canonical / C-target-checked,NQ | C-canonical name,NQ | int16 |
| INTEGER / INT / INT4 | C,O-PG,T,NQ | C-checked,NQ | C-canonical / C-target-checked,NQ | C-canonical name,NQ | int32 |
| BIGINT / INT8 | C,O-PG,T,L | C-checked,NQ; prior L | C-canonical / C-target-checked,NQ; prior L | C-canonical name,NQ; prior L | int64 |
| DECIMAL / NUMERIC | C-decimal128/exact-text,F,NQ; PG O-PG; T | C-exact128/256/marked-text,NQ | C-explicit-p/s / C-target-exact,NQ | C,F numeric modifiers; NQ | decimal128(p,s); exact text for unresolved expression modifiers |
| REAL / FLOAT4 | C,O-PG,T,NQ | C-checked,NQ | C-canonical / C-target-checked,NQ | C-canonical name,NQ | float32 |
| DOUBLE PRECISION / FLOAT8 / FLOAT | C,O-PG,T,NQ | C-checked,NQ | C-canonical / C-target-checked,NQ | C-canonical name,NQ | float64 |
| CHAR / CHARACTER / NCHAR / BPCHAR | C-checked raw padding,F,NQ; O-PG | C-UTF8 single-byte/width,NQ | C-explicit width / C-target checked,NQ | C,F byte width/identity,NQ | utf8 + CHAR byte width/identity |
| VARCHAR / CHARACTER VARYING / NVARCHAR / TEXT | C-checked UTF8,F,NQ; O-PG,T | C-checked UTF8/width,NQ | C-explicit/fixed default / C-target checked,NQ | C,F,T byte width/identity,NQ | utf8/large_utf8 + byte limit |
| DATE | C,O-PG,T | C-date32,O-PG | C / C | C-name | date32 |
| TIME / TIME WITHOUT TIME ZONE | C,O-PG,T | C-time64(us),O-PG | C / C | C-name; precision N | time64(us); endpoint decision below |
| TIMETZ / TIME WITH TIME ZONE | C-opaque,NQ | N semantic mapping | N / N | C-name only | explicit offset-time representation, below |
| TIMESTAMP / TIMESTAMP WITHOUT TIME ZONE | C,O-PG,T | C,O-PG | C / C | C-name; precision N | timestamp(us), no timezone |
| TIMESTAMPTZ / TIMESTAMP WITH TIME ZONE | C,O-PG | C,O-PG | C / C | C-name; precision N | timestamp(us, UTC), server-retained instant |
| INTERVAL YEAR TO MONTH | C,F,NQ | explicitly rejected | rejected / rejected | C,F family; qualifier N | month_day_nano_interval(months,0,0) |
| INTERVAL DAY TO SECOND | C,F,NQ | explicitly rejected | rejected / rejected | C,F family; precision N | duration(us) + interval family |
| BOOLEAN / BOOL | C-strict,NQ; O-PG,T | C-checked,NQ | C-canonical / C-target-checked,NQ | C-canonical name,NQ | bool |
| HLLSKETCH | C-opaque,NQ | N | N / N | C-name only | proposed versioned sketch serialization |
| SUPER | C-opaque,NQ; serialized VARCHAR test only | N | N / N | C-name only | JSON extension only after losslessness proof |
| VARBYTE / VARBINARY / BINARY VARYING | C-raw reference,F,NQ | C-raw/provisional size cap,NQ | C-explicit width / C-target checked,NQ | C,F byte width/identity,NQ | binary/large_binary |
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
  to string with `arrow.opaque` for PostgreSQL; Redshift uses decimal128 for known
  p/s and exact UTF-8 for unknown descriptors. TIMETZ maps to opaque binary and
  generic PostgreSQL interval to month/day/nanosecond. Task010b adds distinct backend-classified Redshift
  interval IDs and strict family readers; neither uses the PostgreSQL interval codec. Unknown bytes can be useful diagnostics without proving a native codec.
- Modifiers: Task010c1 retains `PQfmod` per field in
  [ResolveOutputTypes](../../c/driver/postgresql/result_helper.cc), including the
  actual non-COPY reader, empty and schema-only descriptions. The global OID cache
  stays unmodified. GetTableSchema parses name/OID/atttypmod; GetObjects parses
  ordered catalog columns through the same backend interpretation. OID-only
  parameters retain canonical family but no inferred precision/scale/width.
- Binding: [BindStream::SetParamTypes](../../c/driver/postgresql/bind_stream.h#L144)
  derives parameter types from Arrow or explicit canonical semantic metadata;
  Arrow NA can use inferred expected types. Known decimal/scalar/character/binary ingest targets
  select matching binary writers and parameter OIDs together. Existing PostgreSQL `arrow.json` mapping is not SUPER.
  Opaque readback is not safely round-trippable. TIME32/DATE64 map to candidate DB
  types but lack matching writer cases; TIME64 accepts only microseconds.
- Ingest: [CREATE mapping](../../c/driver/postgresql/statement.cc#L743) calls
  validated semantic declarations rather than relying on unqualified names. Task010c3 adds explicit decimal(p,s)
  and actual target descriptors to both COPY and prepared INSERT decimal writers;
  Task010c4 extends this to the six primitive numeric/boolean families; Task010d
  adds CHAR/VARCHAR/VARBYTE identity, explicit byte widths and aligned destination
  OIDs. Other prepared parameters keep their Arrow-derived types. Native VARBYTE
  receive/modifier/parameter qualification remains pending.
  The staged CSV helper is not selected by the supported ingest path.
- Remaining scalar hazards: Task010c4 guards Redshift integer/float narrowing;
  unmarked PostgreSQL retains its previous conversion policy. Timestamp
  nanoseconds divide by 1000, as do generic interval nanoseconds. Reject overflow/nonrepresentable fractions, not silent wrapping or
  truncation. Timestamp reader epoch overflow already errors; full Redshift range,
  TIME 24:00 and timezone semantics still need explicit acceptance.
- Tests: [RedshiftSmokeTest](../../c/driver/postgresql/redshift_test.cc#L387) declares
  scalar/null/edge/metadata cases, including `DECIMAL(38,0)`, but SUPER coverage is
  explicitly `JSON_SERIALIZE`, not native SELECT. [reader tests](../../c/driver/postgresql/copy/postgres_copy_reader_test.cc#L536),
  [writer tests](../../c/driver/postgresql/copy/postgres_copy_writer_test.cc#L451)
  and [schema tests](../../c/driver/postgresql/postgres_type_test.cc#L127) prove
  PostgreSQL fixtures, not all Redshift types. Task010b adds
  [interval catalog/schema/codec tests](../../c/driver/postgresql/redshift_interval_test.cc),
  [literal offline receive fixtures](../../c/driver/postgresql/redshift_interval_test_fixture.h)
  and [PyArrow C Data import](../../ci/scripts/pgwire_interval_c_data_test.py).
  TIMETZ, native SUPER, HLLSKETCH and spatial family coverage remains pending.

The [official Python connector source](https://github.com/aws/amazon-redshift-python-driver/blob/248ffd5896c7a46bba115452dc10b8389d3ede54/redshift_connector/utils/type_utils.py)
(at pinned commit `248ffd5896c7a46bba115452dc10b8389d3ede54`) distinguishes
`intervaly2m` signed 4-byte months and `intervald2s` signed 8-byte microseconds; the
generic PostgreSQL reader expects 16 bytes; Task010b keeps it separate. Its numeric binary codec takes scaled 8/16
bytes, while this driver parses PostgreSQL numeric digits. **Protocol evidence
requirement, not proof of a current decimal bug**: the connector
[negotiates client_protocol_version](https://github.com/aws/amazon-redshift-python-driver/blob/248ffd5896c7a46bba115452dc10b8389d3ede54/redshift_connector/core.py#L811).
Each libpq path needs its own qualified descriptor/format/fixture evidence. The generated literal fixture bytes follow only the pinned reference contract, use
arbitrary fixture catalog OIDs, and are not captured libpq evidence. No upstream
implementation is copied.

## Current Redshift descriptor metadata (Task010c1)

Additive UTF-8 field metadata, alongside existing `POSTGRESQL:type`, opaque and
interval-family metadata:

| Key (`ADBC:redshift:` prefix) | Meaning |
|---|---|
| `sql_type` | Known canonical family `DECIMAL`, `CHAR` (bpchar), or `VARCHAR`; includes OID-only parameters |
| `type_modifier` | Exact observed signed int32 descriptor in decimal text, including unknown `-1`; absent if not supplied |
| `precision`, `scale` | Both emitted only for a supported numeric encoding: precision 1..38, scale 0..precision through 38 |
| `byte_width` | Supported bpchar/varchar descriptor byte width, 1..16,000,000 |

The [pinned AWS JDBC arithmetic](https://github.com/aws/amazon-redshift-jdbc-driver/blob/773fa16aabbd589e9e2c93684c7ca2543e3ce31a/src/main/java/com/amazon/redshift/jdbc/TypeInfoCache.java#L665)
is reference evidence, not a captured native descriptor. Unsupported encodings
keep raw diagnostics without fabricated defaults. Interval/temporal precision,
qualifiers, VARBYTE and other modifier meanings remain unknown. PostgreSQL's
public schema metadata policy stays unchanged. Task010c2 uses these descriptors for decimal128 reads; unknown Redshift numeric
modifiers keep exact UTF-8 text with DECIMAL identity and no opaque label.

Catalogs describe views/derived expressions too, so both schema/catalog readers
retain the full read envelope above. This does not authorize wider stored-column
DDL or imply every engine produces every boundary. [AWS numeric computations](https://docs.aws.amazon.com/redshift/latest/dg/r_numeric_computations201.html)
conflicts internally: its computed-result section allows scale 38 while overflow
conditions cap intermediate/final results at 37. Native boundary qualification
remains open. [Character results](https://docs.aws.amazon.com/redshift/latest/dg/r_Character_types.html)
may reach 16M bytes despite narrower stored-column limits. GetObjects emits known
numeric precision/scale/radix or character column-size/octet-length; unknown
values remain null. CREATE/append validation is a separate future writer task.

## Decimal reference reads (Task010c2)

Known Redshift numeric descriptors produce decimal128(p,s); missing/unsupported
modifiers produce exact UTF-8 decimal text with DECIMAL identity. Schemas do not
sample values or change between batches. Finite sign/header/digit/length checks,
field isolation, precision checks and exact rescaling reject malformed values,
nonfinite signs, hidden nonzero fractions and overflow before appending a value.
Extra trailing zeroes may rescale exactly. SQL NULL remains distinct from zero.
Unknown text retains display-scale trailing zeroes and normalizes signed zero.

The tested wire reference is [PostgreSQL 18 numeric_send](https://github.com/postgres/postgres/blob/3d6a828938a5fa0444275d3d2f67b64ec3199eb7/src/backend/utils/adt/numeric.c),
base-10000 numeric binary, matching this
driver's existing libpq binary path; it does not select formats by payload length
or substitute the AWS Python connector's negotiated scaled-integer format. The
reference's int16 count/weight and 14-bit display scale bound per-field work and
text allocation. PostgreSQL's existing public schema/reader stays unchanged.
Literal reference vectors, actual normal/bound/empty PostgreSQL plumbing and
codec-produced C Data/PyArrow decimals are offline evidence, **not captured
Redshift traffic**. Native precision19 coefficient limits, scale37/38 boundaries
and actual libpq protocol qualification remain open. Task010c3's write contract
below is independently bounded; offline PostgreSQL execution is not Redshift proof.

## Decimal write contract (Task010c3)

Arrow decimal128/256 with precision1..38 and scale0..precision through38 uses
bounded exact coefficient validation and PostgreSQL base-10000 serialization.
Raw coefficients must fit the declared precision, including signed extrema;
a small value does not make a decimal256 schema with precision76 supported.
An explicit `ADBC:redshift:sql_type=DECIMAL` opts into the same contract on either
backend; ordinary unmarked PostgreSQL schemas retain their existing behavior.
Marked UTF-8 uses ASCII `[+-]?[0-9]+(\.[0-9]+)?`, at most80 bytes and38 fractional
digits, with no exponent, whitespace or nonfinite values. Unknown-p/s values
must fit38 coefficient digits (redundant fractional zeroes may be removed to fit).
Supplied precision/scale must be paired, legal and consistent with physical Arrow
storage and any supported raw modifier. Unknown p/s never supplies CREATE defaults.
Dictionary-contained DECIMAL identity/storage is unsupported and rejects before
Bind ownership, type mapping or writer selection; it cannot use a raw-byte writer.
Ordinary unmarked PostgreSQL string dictionaries retain their text behavior.

CREATE/REPLACE/CREATE_APPEND validates and renders `DECIMAL(p,s)` before destructive
SQL: p1..38, s0..min(p,37). APPEND aligns actual name/OID/modifier catalog fields,
including reordered/subset fields, and permits only exact target rescaling with
precision/range checks; stored precision19 additionally checks signed64 coefficient
bounds. Decimal128/256, DECIMAL-marked text and Arrow null are accepted numeric
ingest sources; unmarked text, integer and float inputs reject on Redshift numeric
targets. OID-only SQL parameters preserve the offered exact value; application
CASTs may intentionally round. Incompatible/unknown numeric targets reject.

Schema rejection precedes replacement DDL. Runtime value failures may follow DDL;
no Redshift table-replacement atomicity is promised. The existing prepared stream
transaction owner rolls back owned INSERT batches on late failure. Caller-owned
transactions retain earlier work for the caller to resolve. A failed owned rollback
now signals cleanup uncertainty and retires the connection while retaining the
original error; issuing ROLLBACK alone is not evidence of cleanup. Production public
Bind/stream/catalog/DDL/COPY tests, a prepared INSERT seam and consumer-exported
C Data writer checks provide local evidence only. Native libpq/Redshift boundaries,
independent review and final platform CI remain qualification gates.

## Primitive scalar contract (Task010c4)

SMALLINT/INTEGER/BIGINT, REAL/DOUBLE PRECISION and BOOLEAN carry canonical
`ADBC:redshift:sql_type` identity across result, schema, catalog and OID-only
parameter descriptions. GetObjects uses the same canonical names for these six
Redshift families; original CREATE alias spelling and unknown qualifiers are not
invented. Ordinary PostgreSQL metadata remains unchanged.

Redshift Arrow primitive inputs use checked source and actual-target widths;
explicit canonical metadata opts into the same contract on PostgreSQL. Signed
and unsigned integer endpoints are checked before casting, including unsigned64's
high bit. Within-family integral narrowing accepts only fitting values. Float32
narrowing checks finite bounds before casting and rejects inexact values, including
underflow; widening is exact. Signed zero, NaN and signed infinities have numeric
semantics, without a NaN payload promise. Integer/float/boolean cross-family
coercions and semantic dictionaries reject; unmarked PG string dictionaries retain
their previous behavior. Arrow null is accepted for a known supported target.

Validated schema identity renders canonical DDL before replacement; known append
fields select matching destination OID and binary layout together. OID-only SQL
parameters preserve the offered value; an application-written CAST may round.
Redshift boolean binary reads accept only byte0/1 and reject malformed/truncated
fields before appending. Integer/float reads retain their native fixed widths.
The existing whole-stream INSERT transaction owner and cleanup uncertainty signal
remain shared with C3; caller-owned work stays caller-owned. Runtime value errors
may follow DDL, so no stronger table-replacement atomicity is promised. Native
Redshift range, subnormal retention and alias/protocol qualification remain open;
local PostgreSQL/codec/installed-consumer tests are separate evidence.

## Character/binary contract (Task010d)

Canonical `CHAR`, `VARCHAR` and `VARBYTE` use `ADBC:redshift:sql_type` and known
`ADBC:redshift:byte_width`; unknown raw modifiers remain diagnostic, not inferred
widths. The pinned AWS JDBC reference applies typmod minus four to VARBYTE as to
character types; actual native PQfmod/catalog confirmation is still required.
Only the catalog receive mapping enables binary VARBYTE; its dynamic OID is
selected independently of BYTEA. Reference raw bytes are not a display-hex codec
or native wire qualification. CHAR reads retain received padding and empty/NULL
remain distinct. VARCHAR expression read descriptors may reach 16,000,000 bytes;
VARBYTE reads/declarations may reach 16,777,216 bytes. No width is sampled from rows.

Semantic CHAR CREATE requires explicit 1..4096 byte width. VARCHAR stored widths
are 1..65535; unmarked Redshift UTF8 uses fixed `VARCHAR(65535)`. Unmarked binary
uses `VARBYTE(16777216)`; validated explicit widths override that default.
Declarations fail before replacement DDL. Runtime value failures may follow
CREATE, so table replacement has no stronger atomicity promise. Known append
fields select aligned destination OID, width and writer together, including
subsets/reordering. Text and binary do not implicitly coerce; source and target
byte limits both apply, including overflow made entirely of spaces. Dictionary
semantic shapes reject before either Bind API takes ownership; ordinary unmarked
PostgreSQL dictionaries and type policy remain unchanged.

UTF8 validation rejects malformed/overlong/surrogate/out-of-range scalars and
embedded NUL. Under UTF8, CHAR's single-byte domain is non-NUL ASCII; no high-bit
single-byte encoding is inferred. There is no trimming, transcoding or hex decode.
Binary zero/high bits pass through. Array offsets, lengths and binary-view
references are checked before getters; normal/large/view and fixed binary storage
share bounded checks. Canonical markers opt PostgreSQL into these source/target
checks without pretending its connection is Redshift. Native charset, padding,
large values and wire boundaries remain qualification gates.

A **provisional** plain parameterized/COPY semantic VARBYTE guard accepts at most
1,000,000 bytes. It deliberately does not guess whether AWS's documented “1 MB”
means 1,048,576 bytes. This narrower checkpoint guard does **not** complete the
all-type release contract: native independent value/length probes must resolve
1,000,000/+1 and 1,048,575/576/577. No staging/file upload, S3 or automatic route
switch is added. Larger declarations and reads are separate from this write guard.

Fresh client-encoding checks require UTF8 for Redshift and explicitly marked PG
character writes. A changed/missing encoding rejects rather than repairing or
retrying the session. Successful result-producing actions are checked before text
interpretation/export; arbitrary SQL can already have effects when a postcheck
fails. Existing stream transaction ownership and cleanup remain in charge; no
claim that a failed encoding postcheck undoes arbitrary SQL is made. Ordinary
unmarked PostgreSQL remains unchanged. Codec/public/installed local evidence is
separate from native Redshift and final platform CI.

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

- **010b implemented, offline qualification:** backend-owned catalog-name
  classification and runtime-discovered OIDs for `intervaly2m` / `intervald2s`;
  strict 4-byte signed months -> month_day_nano_interval(months,0,0) and 8-byte
  signed microseconds -> duration(us). C++ and installed PyArrow checks consume
  actual codec-produced arrays, including null/empty/mixed results, signs, extrema,
  unaligned input and malformed widths. Shared `SetSchema` carries
  `ADBC:redshift:interval_family` plus canonical type name across query/metadata
  schema paths. Qualifiers/fractional precision remain unknown because current
  descriptors do not expose them. No extension or qualifier metadata is invented.
  Both bind APIs reject family-marked schemas before ownership transfer/SQL/DDL;
  Redshift also rejects interval/duration storage with stripped metadata. Unknown
  family-marker values reject. Unmarked PostgreSQL interval behavior is preserved.
  Bind/create/append remain unsupported. The test-only C Data producer is excluded
  from driver artifacts and normal installs. Native libpq-path qualification is
  still pending a separately admitted installed-artifact batch, with its own finite
  authority; this slice does not complete Task010.
- **010c1 merged (PR23), offline CI passed:** per-field descriptor plumbing and
  bounded Redshift schema/GetObjects metadata, including normal/bound/empty query
  readers and OID-only parameter identity. Synthetic descriptor/C Data and local
  PostgreSQL regressions cover the actual parser/consumer seams.
- **010c2 merged (PR24), offline CI passed:** known numeric p/s selects decimal128;
  unknown modifiers keep exact decimal text. Strict finite base-10000 reference
  fields pass through the production reader and C Data import tests. Native
  Redshift/libpq format/boundary qualification remains pending.
- **010c3 locally integrated d04f2d2, review/native/installed validation passed:** explicit decimal DDL,
  bounded exact decimal/marked-text binding, actual target-aware append and owned
  rollback uncertainty handling. Final platform CI is deferred until development ends.
- **010c4 locally integrated b90664a, review/native/installed validation passed:** checked primitive
  scalar conversions, canonical identity/DDL, exact known-target append and strict
  boolean wire reads. Final platform CI follows development.
- **010d implemented, final local validation/review pending:** CHAR/VARCHAR/VARBYTE
  read, binding, explicit DDL, target-aware append and metadata checks, with native
  format/charset/size qualification (including the provisional binary write cap)
  still open.
- **Prepared successors, not dispatched here:** temporal value/write gaps and
  interval writes/qualifiers; native SUPER and lossless extension/protocol decisions;
  HLLSKETCH and spatial native codecs/semantic contracts. Coordinator briefs bound
  these phases; no arbitrary future phase number implies completion or authority.
- **Final all-type qualification:** complete the 21-row read/bind/create/append/
  metadata evidence on installed C and wheel artifacts, all aliases and bounded
  unsupported operations. The coordinator controls the separately admitted finite
  own-resource live window/teardown and final push/applicable green platform CI.
  No live run, AWS action, staged upload, publication or automatic retry is authorized
  by this document. Only then can Task009 release readiness be assessed.
