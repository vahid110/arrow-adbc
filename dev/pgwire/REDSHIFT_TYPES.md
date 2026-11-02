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

Reconciled 2026-10-28 against merged PR29 source
`fca2eab2e05e1a63cfbd9adecc802c53334bdbd0` (tree
`0fa498a6e88c30eb1758195c614755a9942c51af`). **All 21 canonical SQL column
types remain first-release scope.** This inventory separates implementation from
retained installed evidence; it does not declare release readiness. The finite
remaining gates are in [V1_ACCEPTANCE.md](V1_ACCEPTANCE.md).

Aliases resolve to canonical types; recovering original CREATE spelling is not
required. The [AWS list](https://docs.aws.amazon.com/redshift/latest/dg/c_Supported_data_types.html)
is the inventory boundary. PostgreSQL UUID/JSON/JSONB/arrays/domains/ranges/money,
quoted `"char"`, and nested SUPER values are not additional SQL column types.

## Current evidence, by operation

`C` = implemented, but the selected retained evidence does not qualify this
operation for this family. It is an **evidence gap**, not a finding that code fails.
`L/M/Q/R/S/T` = bounded installed live evidence at the exact source listed below;
none means every value, API, platform or final candidate was tested. `P` limits
that evidence to Python through the installed C driver; standalone C is separate.
`U` = explicitly unsupported operation. Mixed HLL rows and direct SUPER/spatial
binds are mandatory v1 work under the human decision of 2026-10-28.
A direct bind is application-supplied prepared SQL, distinct from the driver's
generated CREATE/APPEND SQL. Bind/BindStream ingest proof never fills a direct-bind
gap. Metadata evidence below covers the indicated query/schema observations;
full ExecuteSchema/parameter/GetTableSchema/GetObjects parity remains gate B5.

| Canonical SQL type and aliases (21) | Native read | Direct bind | Create | Append | Metadata | Arrow representation |
|---|---|---|---|---|---|---|
| SMALLINT / INT2 | C | C | C | C | C identity | int16 |
| INTEGER / INT / INT4 | C | C | C | C | C identity | int32 |
| BIGINT / INT8 | L | L | L | L | L bounded schema | int64 |
| DECIMAL / NUMERIC | Q | C | Q-P | Q-P | Q query p/s; unknown expressions exact text | decimal128 or exact text |
| REAL / FLOAT4 | C | C | C | C | C identity | float32 |
| DOUBLE PRECISION / FLOAT8 / FLOAT | C | C | C | C | C identity | float64 |
| CHAR / CHARACTER / NCHAR / BPCHAR | R | C | R-P | R-P | R widths/identity | utf8, padding retained |
| VARCHAR / CHARACTER VARYING / NVARCHAR / TEXT | Q,R | C | R-P | R-P | R widths/identity | utf8/large_utf8 |
| DATE | Q | C | Q-P | Q-P | Q query identity; C remaining APIs | date32 |
| TIME / TIME WITHOUT TIME ZONE | Q | C | Q-P | Q-P | Q query; unknown precision retained | time64(us) or native-count extension |
| TIMETZ / TIME WITH TIME ZONE | Q | C | Q-P | Q-P | Q query family/offset struct | fixed-offset struct extension |
| TIMESTAMP / TIMESTAMP WITHOUT TIME ZONE | Q | C | Q-P | Q-P | Q query; unknown precision retained | timestamp(us) or native-count extension |
| TIMESTAMPTZ / TIMESTAMP WITH TIME ZONE | Q | C | Q-P | Q-P | Q query; unknown precision retained | timestamp(us, UTC) or native-count extension |
| INTERVAL YEAR TO MONTH | Q,S,T | C | S,T | S,T | S,T public unknowns; internal target tuples | month_day_nano_interval(months,0,0) |
| INTERVAL DAY TO SECOND | Q,S,T | C | S,T | S,T | S,T public unknowns; internal target tuples | duration(us) plus family |
| BOOLEAN / BOOL | C | C | C | C | C identity | bool |
| HLLSKETCH | M-P,Q-P all-HLL; U mixed rows | M-P,Q-P text subset | M-P,Q-P | C | M-P,Q-P query extension | large_utf8 serialized sketch |
| SUPER | Q; corrected C Arrow rerun open | C; installed direct qualification pending | M-P,Q-P JSON subset | M-P,Q-P JSON subset | Q native output schema | large_utf8 native JSON wire |
| VARBYTE / VARBINARY / BINARY VARYING | M,Q,R | R | R | R | R widths/identity | binary/large_binary |
| GEOMETRY | M-P,Q-P bounded EWKB | C; installed direct qualification pending | M-P,Q-P | C | M-P,Q-P query extension | large_binary EWKB/per-value SRID |
| GEOGRAPHY | M-P,Q-P bounded EWKB | C; installed direct qualification pending | M-P,Q-P | C | M-P,Q-P query extension | large_binary EWKB/per-value SRID |

Evidence keys (precise retained paths/operations are indexed in
[V1_ACCEPTANCE.md](V1_ACCEPTANCE.md#retained-evidence-index)):

- **L:** Linux installed C archive/Python wheel, source `3460c5419`,
  [run 39138655470](https://github.com/vahid110/arrow-adbc/actions/runs/39138655470):
  temporary IAM/TLS, value 42, int64 create/append/schema and rejection retention.
- **M:** macOS arm64 source `a239f66cc`, operation `726f4b3594e048e29f4571b2770549c3`:
  15 positive vendor cases and three expected rejections through the Python
  client/public C API, not a standalone C client. Spatial/HLL CREATE and
  readback are proven; APPEND is not. The then-expected SUPER read rejection was
  superseded by Q, while its original receipt remains unchanged.
- **Q:** macOS arm64 source `26fb72348`: Python Arrow 76/76, lossless 11/11,
  writes 60/60 (48 decimal/temporal/TIMETZ positive, 12 interval-target rejections);
  standalone C Arrow 22/23, interval reads 37/37, lossless 24/24. The original C
  SUPER fixture failure remains; the corrected fixture passed only in lossless mode.
- **R:** macOS arm64 reads at `d5b39f4`: Python 61, C 16 small cases and two
  maximum reads per language. Corrected `3be87d3` passed 18 VARBYTE cases in each
  Python/C generated/direct route. Python character writes do not establish the
  standalone C character-write cross-product. See [R details](STATUS.md#task010r-characterbinary-qualification).
- **S:** `218b59859`, four macOS arm64 Python/C array/stream ingestion legs,
  68 cases each: 37 qualifier/precision create+append pairs, 31 target-quantum
  rejections/retention. See [S details](STATUS.md#task010s-interval-ingestion-qualification).
- **T:** `77da4599c`, four macOS arm64 Python/C array/stream legs, 57/60/57/60
  cases: selected extrema/defaults/negative and late-failure behavior. All 234
  cases passed. Full resource closure is verified separately from the original C
  stream cleanup timeout. See [T details](STATUS.md#task010t-interval-edge-qualification).

Public interval constraints remaining unknown are an intentional truthful
representation, not missing proof of a guessed qualifier. S/T target decoding is
internal ingestion evidence only. Opaque bytes never qualify semantic reads.
All receipts retain their original failures, source and platform scope; actual
billing is unreconciled. PR29 is merged with applicable CI green; candidate
validation after new changes remains a separate gate.

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
  p/s and exact UTF-8 for unknown descriptors. Ordinary PostgreSQL TIMETZ keeps its opaque binary fallback; Task010e3 adds the
  explicit Redshift fixed-offset struct below. Generic PostgreSQL interval to month/day/nanosecond. Task010b adds distinct backend-classified Redshift
  interval IDs and strict family readers; neither uses the PostgreSQL interval codec. Unknown bytes can be useful diagnostics without proving a native codec.
- Modifiers: Task010c1 retains `PQfmod` per field in
  [ResolveOutputTypes](../../c/driver/postgresql/result_helper.cc), including the
  actual non-COPY reader, empty and schema-only descriptions. The global OID cache
  stays unmodified. GetTableSchema parses name/OID/atttypmod; GetObjects parses
  ordered catalog columns through the same backend interpretation. OID-only
  parameters retain canonical family but no inferred precision/scale/width.
- Binding: [BindStream::SetParamTypes](../../c/driver/postgresql/bind_stream.h#L144)
  derives parameter types from Arrow or explicit canonical semantic metadata;
  Arrow NA can use inferred expected types. Known decimal/scalar/character/binary/temporal ingest targets
  select matching binary writers and parameter OIDs together. Existing PostgreSQL `arrow.json` mapping is not SUPER.
  Opaque readback is not safely round-trippable. Task010e1 adds checked temporal
  units/epochs, exact DATE64 days and closed temporal native-count extensions;
  unmarked PostgreSQL keeps its previous conversion policy.
- Ingest: [CREATE mapping](../../c/driver/postgresql/statement.cc#L743) calls
  validated semantic declarations rather than relying on unqualified names. Task010c3 adds explicit decimal(p,s)
  and actual target descriptors to both COPY and prepared INSERT decimal writers;
  Task010c4 extends this to the six primitive numeric/boolean families; Task010d
  adds CHAR/VARCHAR/VARBYTE identity, explicit byte widths and aligned destination
  OIDs. Task010e1 extends aligned targets to four temporal families, checking
  source and known target precision independently. Other prepared parameters keep
  their Arrow-derived types. Native VARBYTE
  receive/modifier/parameter evidence must be read per operation in the matrix above.
  The staged CSV helper is not selected by the supported ingest path.
- Remaining scalar hazards: Task010c4 guards Redshift integer/float narrowing;
  unmarked PostgreSQL retains its previous conversion policy. Task010e1 rejects
  temporal submicrosecond remainder and checked unit/epoch overflow, including
  source-unit epoch cancellation. Q provides bounded native temporal evidence;
  remaining finite cases are B4 in the acceptance checklist; generic interval
  nanosecond conversion is separate successor work.
- Tests: [RedshiftSmokeTest](../../c/driver/postgresql/redshift_test.cc#L387) declares
  scalar/null/edge/metadata cases, including `DECIMAL(38,0)`, but SUPER coverage is
  explicitly `JSON_SERIALIZE`, not native SELECT. [reader tests](../../c/driver/postgresql/copy/postgres_copy_reader_test.cc#L536),
  [writer tests](../../c/driver/postgresql/copy/postgres_copy_writer_test.cc#L451)
  and [schema tests](../../c/driver/postgresql/postgres_type_test.cc#L127) prove
  PostgreSQL fixtures, not all Redshift types. Task010b adds
  [interval catalog/schema/codec tests](../../c/driver/postgresql/redshift_interval_test.cc),
  [literal offline receive fixtures](../../c/driver/postgresql/redshift_interval_test_fixture.h)
  and [PyArrow C Data import](../../ci/scripts/pgwire_interval_c_data_test.py).
  Full native TIMETZ/SUPER/HLL/spatial qualification remains open; bounded
  installed vendor evidence is recorded separately above.

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

## Interval constraint contract (Task010e2)

Both reference codecs retain the physical representations above and the exact
`ADBC:redshift:interval_family` values `intervaly2m` / `intervald2s`. Additive field
keys under `ADBC:redshift:` are `interval_contract_version=1`,
`interval_constraint_state`, `interval_qualifier` and `fractional_precision`.
`sql_type` is the canonical `INTERVAL YEAR TO MONTH` / `INTERVAL DAY TO SECOND`.
States are closed: `known` requires an exact qualifier and SECOND precision;
`unknown` is decoded diagnostic information and cannot be rebound; `unspecified`
is a deliberate application assertion with no qualifier/precision/raw facts.
Removing metadata and asserting a new state is an application semantic decision,
not authenticated provenance. Legacy family-only readback does not authorize writes.

The 13 qualifier identities are YEAR, MONTH, YEAR TO MONTH, DAY, HOUR, MINUTE,
SECOND, DAY TO HOUR, DAY TO MINUTE, DAY TO SECOND, HOUR TO MINUTE, HOUR TO SECOND
and MINUTE TO SECOND. SECOND-ending forms require precision 0..6; others forbid
fractional precision. Known source and actual-target constraints independently
require exact integral month/hour/minute/day or fractional-second quanta. No
rounding, timezone conversion, 24-hour duration cap or float intermediary is used.
Month-family day/nanosecond components must be zero. Duration s/ms multiplication
is checked and ns division must be exact. Signed extrema are reference storage
codes, not proof of native SQL domain endpoints or sentinels.

CREATE renders the exact known qualifier and SECOND precision. Explicitly
unspecified sources default to `INTERVAL YEAR TO MONTH` / `INTERVAL DAY TO SECOND(6)`.
Existing CREATE_APPEND skips unused declarations and resolves aligned actual
columns; missing targets use plain CREATE with the requested shape. Source schema
errors precede ownership/DDL; runtime conversion retains the existing single
whole-stream transaction owner and caller-owned transaction boundary. Nested,
dictionary, duplicate/conflicting metadata, unknown versions and wrong storage
reject. A null-only source must be genuinely unmarked NA (unrelated user metadata
is allowed), and even empty/NULL ingest requires a known actual target first.
Ordinary unmarked PostgreSQL retains its 16-byte interval mechanics; Redshift
interval semantic writes on PostgreSQL remain unsupported.

Only aligned interval targets trigger additional exact relation/column catalog
observations. Relation OID, schema/table, name, ordinal, type OID and raw modifier
are checked around bound exact SVV_COLUMNS reads. Missing/duplicate/inconsistent
observations and SQL errors fail; raw `interval_type`, `interval_precision` and
`datetime_precision` remain `interval_raw_type`, `interval_raw_precision` and
`interval_raw_datetime_precision` diagnostics. Task010s promotes 37 observed exact
family/raw-modifier/`format_type` tuples, with all three SVV fields NULL, to known
constraints inside actual ingestion-target resolution. This covers the 13
qualifiers and precision 0..6 on SECOND-ending forms. Task010t adds four exact
observed omitted-precision tuples (SECOND, DAY TO SECOND, HOUR TO SECOND and
MINUTE TO SECOND), each decoded as precision 6; native stored +1/-1 microsecond
readback independently supports that mapping. Other tuples, conflicting
facts and unsupported targets remain unknown and fail closed; plausible SVV text,
a PostgreSQL mask/default or driver-issued DDL alone is not sufficient proof.

Exact source and target quanta are checked independently during CREATE/APPEND.
Installed Python/C Bind and BindStream evidence covers these declarations and
retained rows after first-invalid target rejection.
Ordinary query and GetTableSchema metadata still report unknown constraints and
raw diagnostics: internal target decoding does not authorize rebinding readback
as a known source. Explicit source metadata remains an application assertion.
Task010s cases do not establish omitted SQL defaults or native extrema. Task010t
adds installed Python/C array/stream CREATE/APPEND/readback of selected extrema,
conversion thresholds and omitted defaults.
Exact binary parameter echoes alone do not prove stored values. The installed
stored samples do not establish contiguous finite domains, sentinel meaning or
arithmetic behavior. Native text overflow samples returned altered values rather
than errors: observed month wrapping and whole-second narrowing are recorded in
[STATUS](STATUS.md#task010t-interval-edge-qualification). Those text/literal
observations do not change the driver's checked binary conversion contract.
Empty-result descriptors can differ from nonempty descriptors and retain unknown
constraint metadata, without promotion to a known source contract.
Read observations detect observed changes, not every concurrent external DDL
race or a later typmod change.

Test-only public C libpq recording exercises real ownership, source wire/OID,
DDL and unknown-target rejection without a server or identity change. Internal
known-target fixtures exercise the production writer and stream transaction
owner separately. Codec-produced installed PyArrow/C Data/IPC tests cover all
qualifiers, precision, NULL/empty/sliced arrays and exact bytes. These fixtures
are excluded from production artifacts and do not qualify native protocol,
catalog population, SQL grammar/domain or installed Redshift target behavior.

## TIMETZ fixed-offset reference contract (Task010e3)

Redshift TIMETZ uses `adbc.redshift.timetz_us_offset.v1` in both existing
Arrow/lossless connection modes. Storage is a nullable struct with exactly two
nonnullable primitive children, in order: `time_us:int64` and
`offset_seconds:int32`. Exact extension metadata is
`{"version":1,"family":"TIMETZ","time_unit":"us","origin":"start_of_day","offset_unit":"s","offset_sign":"east_of_utc"}`.
`time_us` is civil time since start of day; positive offsets are east of UTC.
Pairs remain pairs, including end-of-day and second-resolution offsets: no
UTC conversion, modulo-day normalization or named zone reconstruction occurs.
Parent SQL NULL ignores child values after structural validation, and decoded
NULLs receive 0/0 child placeholders. Valid parents require both actual children
nonnull. Parent/relation/child offsets are each applied exactly once.

Every semantic write requires the exact extension and `sql_type=TIMETZ`, plus
explicit single-digit `fractional_precision=0..6`. Dictionaries, nesting,
ambiguous child storage, nullable child schemas, conflicting/duplicate recognized
metadata and unknown versions reject before Bind ownership. Unrelated application
metadata is preserved. Missing/unsupported read precision stays diagnostic and
read-only; adding precision cannot erase a conflicting raw modifier. The pinned
[JDBC reference](https://github.com/aws/amazon-redshift-jdbc-driver/blob/773fa16aabbd589e9e2c93684c7ca2543e3ce31a/src/main/java/com/amazon/redshift/jdbc/TypeInfoCache.java#L800)
recognizes raw -1 as 6 or raw 0..6; absent modifiers do not imply 6. Known read
precision is checked, and known source and actual-target precision independently
require exact civil-microsecond divisibility. Actual ingest targets must have
known TIMETZ OID/family/precision even for NULL/empty sources; only genuine
unmarked Arrow NA may use the known-target NULL route.

The **unqualified fixed reference** is exactly 12 network-order bytes: signed
int64 civil microseconds, then signed int32 seconds west of UTC. Widened sign
inversion rejects the non-reversible INT32_MIN code. The reference civil envelope
is inclusive 0..86,400,000,000 us and reversible offsets -2,147,483,647 through
2,147,483,647 seconds. These are codec bounds, **not verified native SQL domains,
infinities or endpoint facts**. An 8-byte payload is rejected rather than guessed
or assigned a zero offset. Reader failures preserve the field cursor and prior
logical output. TIMETZ-only staged fresh buffer allocations preserve old bytes
and ownership on failure; all reservations precede valid-prefix bitmap backfill
and the non-failing logical commit. No imported input is grown.

Bare TIMETZ CREATE/REPLACE accepts explicit source p6. Source p0..5 rejects
before DDL until native precision syntax is qualified. Existing-target
CREATE_APPEND uses the bounded catalog/identifier route and actual target
constraints, skipping unused mixed-projection declarations; missing-target
CREATE_APPEND retains the p0..5 gate. Stable target definitions are required;
observations reject detected changes but do not prove exclusion of every
external DDL race. The existing single stream transaction owner is unchanged;
caller-owned transactions are never silently rolled back. Marked TIMETZ writes
on PostgreSQL remain unsupported, and ordinary unmarked PostgreSQL is unchanged.

[Literal codecs/allocator tests](../../c/driver/postgresql/redshift_timetz_test.cc),
[test-linked public API recorder](../../c/driver/postgresql/redshift_interval_recording_test.cc)
and [codec-produced installed PyArrow C Data/IPC](../../ci/scripts/pgwire_timetz_c_data_test.py)
are offline reference evidence. Q subsequently qualified selected stored TIMETZ
offsets and installed reads/Python writes. Remaining direct/C-write, metadata and
finite offset/precision cases are B4/B5; a passed subset is not a full-domain claim. No network producer or public backend
identity override is introduced by these test fixtures.

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
keep raw diagnostics without fabricated defaults. Additional family-specific
reference contracts are documented separately; unqualified native modifier
meanings remain qualification gates. PostgreSQL's
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
values remain null. CREATE/append validation follows the separate semantic writer
contracts below.

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
Redshift traffic**. Native precision 19 coefficient limits, scale37/38 boundaries
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
precision/range checks; stored precision 19 additionally checks signed 64-bit coefficient
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
and unsigned integer endpoints are checked before casting, including unsigned 64-bit's
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

## Temporal core contract (Task010e1)

`adbc.redshift.temporal_representation` accepts `arrow` (default) or `lossless`
before connection Init. Any later change, including the same value, rejects.
Explicit use on a PostgreSQL connection rejects at Init; ordinary unconfigured
PostgreSQL remains unchanged. Per-field contexts copy the choice without changing
the database-shared OID resolver. Reference decoder contexts over one resolver
are offline evidence; actual two-Redshift-connection isolation remains a live gate.

DATE uses date32 with a checked native2000-to-Unix1970 day shift. Default TIME is
time64(us) on `[0,86400000000)`; default TIMESTAMP is civil timestamp(us), and
TIMESTAMPTZ is timestamp(us, UTC), both with checked epoch shifts. Lossless mode
uses these exact primitive int64 extension contracts (exact JSON bytes):

| Extension name | Serialized extension metadata |
|---|---|
| `adbc.redshift.time_us.v1` | `{"version":1,"family":"TIME","unit":"us","origin":"start_of_day","semantics":"civil"}` |
| `adbc.redshift.timestamp_us_2000.v1` | `{"version":1,"family":"TIMESTAMP","unit":"us","epoch":"2000-01-01T00:00:00","semantics":"civil"}` |
| `adbc.redshift.timestamptz_us_2000.v1` | `{"version":1,"family":"TIMESTAMPTZ","unit":"us","epoch":"2000-01-01T00:00:00Z","semantics":"utc_instant"}` |

`ADBC:redshift:sql_type` retains canonical identity in either mode. Unknown
versions, conflicting family/unit/epoch/metadata, children and semantic dictionaries
reject before binding ownership/DDL; stripped int64 metadata never implies a
temporal family. Explicit matching semantic metadata may opt PostgreSQL input
into the shared contract for honest local testing; it changes no backend identity
or output mode. A nonempty Arrow timestamp timezone label denotes already-UTC
instant counts; labels are not reinterpreted as local clock offsets. Original
server-discarded timezone names/offsets are not recoverable.

The provisional **reference storage** policy accepts TIME us `[0,86400000000]`,
with 24:00 representable only in the lossless extension. DATE int32 and timestamp
int64 signed extrema are rejected as reserved/unqualified reference codes, not
claimed Redshift infinities. The remaining integer space is **not** thereby SQL
valid. Native negotiated wire, exact finite endpoints, sentinel codes, BC/no-year-
zero calendar semantics and session-zone behavior require qualification. Lossless
storage avoids an additional Arrow epoch overflow but does not validate SQL ranges.
Use integer storage rather than Python datetime convenience conversion for BC,
extreme counts or 24:00; C Data import and IPC retain the extension metadata:

```python
# batch is imported codec-produced C Data; unknown extensions retain metadata.
exact_counts = batch.column("timestamp").cast(pa.int64()).to_pylist()
exact_days = batch.column("date").cast(pa.int32()).to_pylist()
# Read exact extension metadata from batch.schema.field("timestamp").metadata.
```

Inputs accept checked Arrow date32/date64, all legal time units, timestamp units
and these closed extensions. DATE64 requires exact whole days; nanosecond input
requires zero submicrosecond remainder. Known source and actual aligned target
fractional precision independently require exact divisibility, before writing
parameter bytes. Cross-family targets reject. Arbitrary SQL parameter casts have
OID-only descriptions, so no target precision is invented or SQL rewritten.

`ADBC:redshift:fractional_precision` records only understood raw modifiers:
reference raw -1 means6, raw0..6 means itself. Missing modifiers remain unknown;
other **read** raw modifiers remain diagnostic `ADBC:redshift:type_modifier` only,
without invented precision. Known read precision is checked before cursor/output
mutation. Unsupported known **source/target write** modifiers reject. These raw
interpretations are pinned reference evidence, not qualified native PQfmod proof.
Canonical bare DDL is implemented; explicitly requested source p0..5 CREATE,
REPLACE or creating CREATE_APPEND rejects preDDL until native precision syntax is
qualified, rather than silently discarding precision. This interim restriction is
an open first-release gate. Existing-target APPEND and CREATE_APPEND honor exact known p. For an explicit
source p0..5 CREATE_APPEND, a parameterized catalog-only probe distinguishes an
existing exact namespace/name from absent targets, then re-resolves the actual
target and skips all unused creation declarations. Own-TEMP discovery uses the
implicit namespace in [CURRENT_SCHEMAS(true)](https://docs.aws.amazon.com/redshift/latest/dg/r_CURRENT_SCHEMAS.html),
matching the pinned JDBC `nspname=ANY(current_schemas(true))` reference; uppercase
Redshift lookup identifiers use a fresh recognized case-setting read and ASCII-only
normalization. No missing-regclass exception, SQL parsing or retry is used; races
surface through actual target resolution. Native own-TEMP/case behavior remains
a qualification gate.
The existing whole-stream transaction owner, caller-owned transaction semantics,
rollback uncertainty retirement and result-owned UTF8 lifetime proof are retained.

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

Redshift direct prepared parameters and generated ingestion accept VARBYTE up to
16,777,216 bytes, with source/target widths still enforced. Larger values reject
locally. Marked PostgreSQL COPY retains a separate 1,000,000-byte guard. R supplies
18 corrected cases in each of four installed Python/C generated/direct routes;
the native 16 MiB+1 probe stopped on unsafe XX000 and is not a classified size
rejection. No staging/file upload, S3 or automatic route switch is added.

Fresh client-encoding checks require UTF8 for Redshift and explicitly marked PG
character writes. A changed/missing encoding rejects rather than repairing or
retrying the session. Successful result-producing actions are checked before text
interpretation/export; arbitrary SQL can already have effects when a postcheck
fails. Existing stream transaction ownership and cleanup remain in charge; no
claim that a failed encoding postcheck undoes arbitrary SQL is made. Ordinary
unmarked PostgreSQL remains unchanged. Codec/public/installed local evidence is
separate from native Redshift and final platform CI.

## Finite release acceptance

[V1_ACCEPTANCE.md](V1_ACCEPTANCE.md) replaces the historical implementation
sequence and open-ended "full envelope" gate. Every canonical family needs a
reviewed representation, required installed C/Python workflows, a finite boundary
set and checked rejection outside the supported contract. Native SELECT must work
without user casts or opaque fallback. Ordinary mixed rows, direct vendor binds,
metadata truthfulness and vendor size restrictions are explicit gates;
an unsupported important workflow is not accepted merely by documenting it.

The detailed contracts above and below describe current code. The evidence
matrix identifies what was actually exercised. Existing limits, including native
SUPER wire-NULL collapse, remain visible; no exhaustive-domain proof or additional
paid operation is authorized by this document.

### Task010i1 bounded SUPER input

The application may assert `adbc.redshift.super.json` on `large_utf8`, with exact
extension metadata `{"version":1,"encoding":"utf8-json","scope":"json-subset-write"}`
and `ADBC:redshift:sql_type=SUPER`. Task010i1 initially supported generated
ingestion only. Current reviewed local code also supports direct prepared
parameters through verified `pg_catalog.json_parse(VARBYTE)` conversion, with
logical descriptors preserved and conservative SQL token checks. Installed direct
qualification remains pending. This marker cannot turn a native opaque result
into lossless JSON.
Arrow validity supplies SQL NULL; nonnull `null` supplies a JSON null document.
The driver preserves validated UTF8 document bytes and numeric tokens up to the
local 16,777,216-byte/1000-container-depth JSON subset. Validation is iterative,
strict UTF8 and decoded-key duplicate aware; no VARCHAR width cap is applied.
This is a transport budget, not a promise that every document fits the stored
SUPER object or per-string limits. Numeric storage interpretation is
JSON_PARSE's server contract, not a host float conversion or full SUPER subtype,
precision/scale or lexical roundtrip guarantee.

Generated INSERT uses `JSON_PARSE($n)` with actual catalog VARBYTE OID/binary raw
JSON bytes. Parse-null-on-error, attribute deduplication and string truncation must
all be OFF, checked read-only before DDL and each actual batch; the driver never
changes these settings. Semantic schemas/actual target identities are strict even
for all-NULL/empty streams. The direct prepared path rewrites only verified SUPER
parameter tokens; unsupported lexical contexts reject. SUPER REPLACE is rejected
before ownership/DROP. CREATE or missing CREATE_APPEND may leave a newly
created empty table following a later payload failure. APPEND keeps the existing
data transaction owner. The fixed installed JSON-subset cases passed; native output has the separate
Task010q contract below. Lossless reconstruction of input subtypes is not promised. HLL's documented
24,580-byte number is a raw sketch-object bound, not the serialized UTF8 budget (retained dense examples exceed it).

### Task010i2 bounded spatial and HLL subsets

Spatial output uses `large_binary` with exact `adbc.redshift.geometry.ewkb` or
`adbc.redshift.geography.ewkb`, canonical SQL family and version1 metadata
`{"version":1,"encoding":"ewkb","family":"GEOMETRY","srid":"per-value","axes":"native"}`
(GEOGRAPHY changes the family). Bytes, axes and SRID are preserved. The complete
PostGIS EWKB structural subset includes seven shapes/Z/M/both endian orders;
ISO offset variants and conflicting nested SRIDs reject. A missing top-level
GEOMETRY SRID means0; GEOGRAPHY requires an embedded SRID. Empty POINT requires
all ordinates NaN, other coordinates finite. Structure validation is not topology
or reprojection. Reads and writes share the 1,048,447-byte raw EWKB cap.
Collection frames are bounded by input bytes, without a separate depth128 cap.
Generated writes pass owned raw EWKB as VARBYTE through `ST_GeomFromEWKB`, and
explicitly cast for GEOGRAPHY. The local direct path uses catalog-typed uppercase
EWKB text; hex expansion does not halve the raw input contract. Installed POINT controls accepted raw binary
VARBYTE and rejected ASCIIhex VARBYTE. Subsequent installed evidence preserved
spatial bytes and witnesses but exposed a GEOMETRY result-identity metadata gap.
Task010k resolves documented Redshift output 3999 through unique catalog GEOMETRY,
without changing parameter OIDs or treating opaque bytes as qualified EWKB.
The fixed corrected installed cases passed; full native boundary qualification
remains pending. Vendor payload EINVAL maps to INVALID_ARGUMENT at both bind
paths; allocation/other failures remain INTERNAL.

HLL uses `large_utf8`, `adbc.redshift.hllsketch` and exact
`{"version":1,"encoding":"redshift-serialized","logm":15}` plus HLLSKETCH identity.
A bounded sparse JSON/quoted canonical Base64 lexical subset preserves bytes up
to1,000,000 encoded bytes, with actual server sketch semantics still authoritative.
Typed direct HLL binds require backend cache identity and actual typed-Prepare /
Describe consistency; this is not independent target or native provenance.
Format0 values own their NUL terminator (physical framed length n+1, logical
Arrow length n), while NULL keeps the existing zero-placeholder/separate-nullptr
framing. Generated ingest separately verifies actual relation targets.

Result-returning queries describe first. All-HLL nonempty projections execute
once in global text format and validate actual descriptor/format before reading.
Mixed HLL/non-HLL projections reject before execution; normal scalar expressions
remain normal scalars. No SQL rewrite, retry or unrelated text fallback occurs.
The fixed installed subsets passed on the selected macOS arm64 artifacts; this
does not close full native boundaries, lossless SUPER reads or all 21 acceptance.

### Native SUPER observable output (Task010q)

Scalar SUPER results use output-only `adbc.redshift.super.native_json` large_utf8
with version1 metadata `encoding=redshift-json-wire`, `scope=native-result`,
`numbers=lexemes`, `nulls=wire-null`. Received bytes and numeric spellings are
unchanged; ordinary mixed fields keep binary codecs. No casts, rewrites or
reexecution are added. Native JSON may expand beyond the stored16MB object
limit. Validation uses the actual received signed32-bit field length and1000
container levels; checked64-bit Arrow append preserves bytes without a stored-size
cap on serialization. SQL wire NULL
maps to Arrow NULL; retained SQL NULL and parsed JSON null inputs produced the
same wire NULL, so the driver cannot reconstruct their original distinction.
Received nonnull JSON `null` remains nonnull. Original input spelling, native
numeric subtype/precision and lossless SUPER roundtrip are not promised. The
native output marker cannot silently become the separate JSON-subset write input.
Bounded installed macOS arm64 live qualification verified native SUPER through
Python in Arrow temporal mode and standalone C in lossless temporal mode. The
original C Arrow-mode fixture failure remains recorded; its corrected case was
not rerun. See [current qualification results](STATUS.md) for scope and limits.

### Local vendor value limits (V1 B3)

The limits above have local boundary/adjacent, deep nesting, malformed input,
allocation-failure and byte-preservation tests. Native SUPER escaped output above
16MiB is covered locally. Generated batches target16MiB of framed values and
flush between rows; a single wider row is permitted subject to signed32-bit Bind
message framing and allocation success. Deferred rows are retried once in order,
including across Arrow batch boundaries. Generic PostgreSQL binding keeps its
existing policy. This is code evidence, not large-value live qualification.

AWS documents [SUPER stored/depth/string limits](https://docs.aws.amazon.com/redshift/latest/dg/limitations-super.html)
and the [spatial object limit](https://docs.aws.amazon.com/redshift/latest/dg/spatial-limitations.html).
Stored object size, JSON serialization, raw EWKB and text parameter expansion are
different budgets. The retained nine native feasibility probes establish only
small direct transport controls. Installed large SUPER/spatial fixtures and the
finite sparse-to-dense HLL transition remain pending; HLL's known-format1,000,000
encoded-byte contract is unchanged.
