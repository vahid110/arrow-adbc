<!--
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

# PostgreSQL-Wire Driver Developer Guide

This fork builds a reusable C++ ADBC core for databases that speak the
PostgreSQL wire protocol. PostgreSQL is the compatibility baseline; Amazon
Redshift is the first additional backend. User-facing build and connection
instructions are in [`c/driver/postgresql/REDSHIFT.md`](../../../c/driver/postgresql/REDSHIFT.md).
The roadmap is [`dev/pgwire/V1_PLAN.md`](../../../dev/pgwire/V1_PLAN.md) and the
current state is [`dev/pgwire/STATUS.md`](../../../dev/pgwire/STATUS.md). History
is in git and in PR descriptions.

## Architecture rule

Common code owns the wire protocol and ADBC mechanics. Each database owns its
semantics: SQL, catalog interpretation, type meaning, transaction behavior,
authentication preparation and supported transfer paths.

- The core uses libpq. It does not implement protocol framing, TLS or
  authentication exchanges itself.
- One backend profile is selected once, after connecting. It holds capability
  data and a few focused policies. It is not a plugin framework; add an
  extension point only when two backends need it.
- No backend-name conditionals in common execution code. Branch on a
  capability or policy field instead.
- Unsupported semantics return `ADBC_STATUS_NOT_IMPLEMENTED` (or a clear error),
  never incomplete or misleading results.
- PostgreSQL public symbols, package names, option names, defaults and observed
  behavior (including result ordering) stay stable.
- Keep the fork rebaseable onto Apache `main`: small commits, no gratuitous
  renames or moves of upstream code. If Apache accepts an equivalent refactor,
  drop the local commit. The fork was created from Apache `150528f0f` and later
  rebased onto `4d50e2e30`. Earlier Apache history: PR #2219 added experimental
  Redshift support and PR #4365 removed it.

## Code layout

| Path | Contents |
| --- | --- |
| `c/driver/pgwire/backend.h` | `BackendProfile`, `BackendCapabilities`, `TransactionSemantics`, table-type and type-alias tables, `DetectBackendProfile`, `SelectQueryResultMode`, `SelectBulkIngestMode` |
| `c/driver/pgwire/libpq_raii.h` | Scoped owners for `PGconn`, `PGresult` and `PGcancel`; the driver has no manual `PQfinish`, `PQclear` or `PQfreeCancel` |
| `c/driver/pgwire/*_test.cc` | Unit tests for the seams above and for the Redshift-private helpers |
| `c/driver/pgwire/redshift_install_smoke.c`, `install_smoke/` | Independent driver-manager clients used by CI against installed or extracted artifacts (C, and a CMake client with `--load-only` for Windows) |
| `c/driver/pgwire/DEVELOPMENT_PACKAGE*.md` | READMEs embedded in the development archives |
| `c/driver/postgresql/` | Shared driver sources (`PGWIRE_DRIVER_SOURCES` in CMake): `database.cc`, `connection.cc`, `statement.cc`, `metadata.cc`, `type_discovery.cc`, `result_reader.cc`, `result_helper.cc`, `error.cc`, `copy/` |
| `c/driver/postgresql/postgresql.cc` | Common ADBC function-table builder and the two entry points, `AdbcDriverPostgresqlInit` and `AdbcDriverRedshiftInit` |
| `c/driver/postgresql/redshift_replace_preflight.*` | Redshift identifier checks before replace-ingest `DROP` (linked into both libraries) |
| `c/driver/postgresql/redshift_csv_writer.*`, `redshift_staged_*` | Redshift-private staged `COPY` helpers, linked only into the Redshift library and not selected by any ingest path |
| `c/driver/postgresql/redshift_test.cc` | Live Redshift tests, compiled into `adbc-driver-postgresql-test` |
| `c/include/arrow-adbc/driver/redshift.h` | Public Redshift entry-point header |
| `c/driver/postgresql/*Redshift*.in`, `adbc-driver-redshift.pc.in` | CMake package and pkg-config metadata for the Redshift library |

Two libraries are built from the same sources. `adbc_driver_postgresql`
detects the server and falls back to the PostgreSQL profile for unknown
PostgreSQL-wire servers. `adbc_driver_redshift` is compiled with
`ADBC_PGWIRE_DEFAULT_REDSHIFT`, so its standard `AdbcDriverInit` selects the
Redshift factory and rejects any server that does not identify as Redshift.

## Extension points

Where each `BackendProfile` field is consumed:

| Field | Meaning | Consumer |
| --- | --- | --- |
| `name`, `driver_name` | Backend and public driver name (`ADBC Redshift Driver`) | `GetInfo` |
| `version_prefix`, `vendor_version_source` | PostgreSQL reads the server parameter; Redshift parses `SELECT version()` | `database.cc` |
| `capabilities.binary_query_copy` | Binary `COPY` result transport; otherwise the text-result path | `SelectQueryResultMode`, `statement.cc` |
| `capabilities.binary_ingest_copy`, `prepared_statements`, `binary_parameters` | Ingest via binary `COPY`, else parameterized `INSERT`, else unsupported | `SelectBulkIngestMode`, `statement.cc` |
| `capabilities.parameterized_ingest_batch_rows` | Rows per `INSERT` statement (Redshift: 16) | `statement.cc` |
| `capabilities.type_catalog_has_typarray` | Type-catalog query plan (Redshift has no `pg_type.typarray`) | `type_discovery.h` |
| `capabilities.metadata_constraints`, `metadata_statistics` | Constraint discovery and `GetStatistics`; off means not implemented | `metadata.h`, `connection.cc` |
| `transactions.isolation_levels` | Session-configurable, or database-configured (non-default overrides fail) | `connection.cc` |
| `transactions.transactional_ddl` | Driver-owned transaction around replace `DROP`/`CREATE` | `statement.cc` |
| `transactions.replace_savepoints` | Private-savepoint recovery for replace inside explicit transactions | `statement.cc` |
| `table_types` | ADBC table-type names and `pg_class.relkind` values | `metadata.cc` |
| `type_receive_aliases` | Vendor receive function mapped to a core one (Redshift `varbyte_recv` to `bytearecv`) | `type_discovery.cc` |
| `replace_identifier_preflight` | Backend identifier checks before replace `DROP` | `statement.cc` |

Metadata SQL (catalogs, schemas, tables, columns, constraints, table schema,
table types) is built by the metadata query provider in `metadata.cc` from the
profile only, so `GetObjects` iteration owns no backend SQL. Type discovery
(`type_discovery.cc`), the Arrow type mapping (`postgres_type.h`) and value
encoding/decoding (`copy/`, `result_reader.cc`, `bind_stream.h`) are separate
layers.

### Adding a backend

1. Add a `BackendKind` value and a `static constexpr BackendProfile` factory in
   `backend.h`. Start every capability at its most conservative value.
2. Extend `DetectBackendProfile` to recognize the server's `version()` string.
3. Only if the new backend needs behavior no existing field expresses, add one
   narrow field, set it for every existing profile, and branch on it in the
   consumer. Do not test the backend kind in common code.
4. Add unit tests in `c/driver/pgwire/backend_test.cc` and the provider/type
   tests, and keep the PostgreSQL suite green.
5. For a separate artifact, follow the Redshift pattern in
   `c/driver/postgresql/CMakeLists.txt` and `meson.build`: a second
   `add_arrow_lib` and Meson `library()` with a default-backend define, a public
   header, CMake/pkg-config templates and an artifact test. Keep CMake and Meson
   source lists, artifacts and tests in parity.
6. Qualify each claimed behavior against the real database and document what
   is unsupported.

## Build and test

GitHub Actions is the build machine for this fork; avoid heavy local builds
(see [`CLAUDE.md`](../../../CLAUDE.md)). The commands below are what CI runs.

### CMake

```sh
cmake -S c -B build/pgwire-ci -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DADBC_BUILD_TESTS=ON -DADBC_DRIVER_POSTGRESQL=ON -DADBC_DRIVER_MANAGER=ON \
  -DADBC_DRIVER_SQLITE=OFF -DADBC_DRIVER_FLIGHTSQL=OFF
cmake --build build/pgwire-ci --target adbc-driver-postgresql-test \
  adbc-driver-postgresql-copy-test adbc-driver-redshift-test
ctest --test-dir build/pgwire-ci --output-on-failure -L driver-postgresql
ctest --test-dir build/pgwire-ci --output-on-failure -L driver-redshift
```

`-DADBC_DRIVER_POSTGRESQL=ON` builds both drivers. Add `-DADBC_USE_ASAN=ON
-DADBC_USE_UBSAN=ON` for sanitizer builds. libpq is found with pkg-config; on
macOS, Homebrew's keg-only `libpq` needs `PKG_CONFIG_PATH` set to its
`lib/pkgconfig`.

| Test binary | Label | Needs |
| --- | --- | --- |
| `adbc-driver-postgresql-test` | `driver-postgresql` | `ADBC_POSTGRESQL_TEST_URI` for database-backed cases; also contains the unit tests for `backend.h`, libpq RAII, errors, metadata, type discovery, and the live Redshift tests |
| `adbc-driver-postgresql-copy-test` | `driver-postgresql` | Nothing; offline binary `COPY` reader/writer tests |
| `adbc-driver-redshift-test` | `driver-redshift` | Nothing; Redshift artifact entry point and Redshift-private helpers, linked against the Redshift library |

Build notes: `adbc-driver-redshift-test` has `UNITY_BUILD OFF` because its
fixture files reuse anonymous-namespace names. `adbc-driver-postgresql-test`
compiles `result_helper.cc`, `metadata.cc` and `error.cc` directly because
their symbols are hidden in the shared library. Private Redshift helpers are
compiled into the Redshift test rather than exported from the driver ABI.
Under MSVC, parenthesize `(std::min)`/`(std::max)` because of Windows macros.

### Meson

```sh
meson setup build/pgwire-meson c -Dtests=enabled -Dpostgresql=enabled \
  -Dflightsql=disabled -Dsqlite=disabled -Ddriver_manager=disabled \
  -Dbenchmarks=disabled
meson compile -C build/pgwire-meson adbc-driver-postgresql-test \
  adbc-driver-postgresql-copy-test adbc-driver-redshift-test
meson test -C build/pgwire-meson --no-rebuild --num-processes 1 \
  --print-errorlogs adbc-driver-postgresql adbc-driver-postgresql-copy \
  adbc-driver-redshift
```

The full `adbc-driver-postgresql` test has a 120-second timeout because
sanitizer runs exceed Meson's 30-second default; the others keep 30 seconds.
Use `-Db_sanitize=address,undefined` for sanitizer builds.

### Test configuration

| Variable | Effect |
| --- | --- |
| `ADBC_POSTGRESQL_TEST_URI` | PostgreSQL server for the integration suite, e.g. `postgresql://localhost:5432/postgres?user=postgres&password=password`. CI uses a `postgres:18` service. Without it, database-backed cases skip. |
| `ADBC_REDSHIFT_TEST_URI` | Enables `RedshiftSmokeTest.*` (libpq URI, TLS, port 5439). Never commit or log it; CI reads it from a repository secret. |
| `ADBC_REDSHIFT_CANCEL_TEST` | With the URI, enables `RedshiftCancelTest.*` |
| `ADBC_REDSHIFT_BENCHMARK` | With the URI, enables `RedshiftBenchmarkTest.*` (1,000-row insert benchmark) |

Run live Redshift tests with
`adbc-driver-postgresql-test --gtest_filter='RedshiftSmokeTest.*'`. Live tests
must clean up their tables in teardown, keep queries bounded, and must not use
`pg_sleep()` (Redshift does not support it; the cancellation test uses a
recursive CTE with a 20-second session `statement_timeout` backstop).

### CI workflows

| Workflow | Triggers | Covers |
| --- | --- | --- |
| `PgWire Drivers` (`pgwire-postgresql.yml`) | PR/push touching `c/**` (not `*.md`) | PostgreSQL 18 suite and Redshift artifact tests on Ubuntu x86-64/ARM64, Debian x86-64, macOS Intel/Apple Silicon; install plus driver-manager client that requires the Redshift driver to reject PostgreSQL; Meson parity on Ubuntu x86-64; offline AWS cleanup mocks |
| `PgWire Drivers`, `workflow_dispatch` | Coordinator only | Adds the live job `C++ / Redshift Serverless`: `RedshiftSmokeTest.*` plus a relocated installed-client connection. Inputs: `cancel_test`, `benchmark`, `package_run_id` (connect a same-commit Ubuntu Release archive), `postgresql_only`, `credential_probe`, `copy_probe`, `copy_csv_semantics_probe`, `copy_fixture_selftest` |
| `PgWire Development Package` (`pgwire-package.yml`) | Driver, manager and CMake sources (not tests or `*.md`) | Seven-day archives and SHA-256 files for seven targets, an independent client built from each extracted archive and connected to PostgreSQL 18, and a final job that downloads and verifies every archive/checksum pair |
| `PgWire AWS Safety Mocks` (`pgwire-aws-safety.yml`) | The ingress and COPY fixture scripts in `.github/scripts/` | Offline mocks of ingress and staged-COPY cleanup; no AWS |
| `PgWire Redshift COPY Fixture` (`pgwire-redshift-copy.yml`) | Manual or called from `PgWire Drivers` | Staged S3 `COPY` fixture (not driver support) |
| `Native Libraries (Unix)`, `Native Libraries (Windows)` | Push to `c/**`, `ci/**`, `docs/**`, ... | Broad Apache C/C++ builds; Unix includes clang-tidy, downstream GLib/Ruby/Python and the docs build; Windows includes the vcpkg x64/ARM64 driver builds |
| `Dev` | Every PR and push | pre-commit (clang-format 18.1.7, cpplint, cmake-format, codespell, whitespace) and the PR title check |

Unrelated Apache workflows are disabled in this fork. Inspect failures with
`gh run view <id> --log-failed`.

## Redshift support

| Area | Behavior |
| --- | --- |
| Driver artifact | Shared and static `adbc_driver_redshift`, `AdbcDriverRedshiftInit`; rejects non-Redshift servers |
| Connection/authentication | Standard libpq URI options, TLS, user/password; no IAM or Identity Center token generation |
| Vendor detection | `SELECT version()` once at database initialization |
| Query results | Portable libpq text-result path with Arrow conversion; only PostgreSQL uses binary query `COPY` |
| Parameters | Prepared statements with binary parameter encoding; parameter-schema discovery |
| Core types | Boolean, signed integers, float, numeric as string (lossless, tested to `DECIMAL(38,0)`), date/time/timestamp, text/varchar, `VARBYTE` |
| Metadata | `GetInfo`, `GetTableTypes` (`table`, `view`), `GetObjects` (including column filters and quoted identifiers), `GetTableSchema`; constraints and statistics are not implemented |
| Transactions | Autocommit, explicit commit/rollback; isolation is database-configured |
| Bulk ingest | Create, append and replace through prepared multi-row `INSERT`; only PostgreSQL uses binary ingest `COPY` |
| Cancellation | Qualified by two opt-in live runs; not in default CI |

### Known limitations

Redshift:

- Non-default ADBC isolation levels fail explicitly, because AWS deprecates
  `SET SESSION CHARACTERISTICS` on Redshift; the default level is accepted.
- DDL is not transactional, so replace ingest is not atomic across `DROP`,
  `CREATE` and the data transfer.
- Replace ingest rejects, before `DROP`: table, schema or column names over 127
  bytes (Redshift truncates them), and ASCII case-only column collisions unless
  the session's `enable_case_sensitive_identifier` is on. The setting is read,
  not changed or cached; unrecognized values fail closed. This is unit-tested
  but not yet live-qualified.
- Ingest uses 16-row `INSERT` statements, capped at 32,767 parameters per
  statement. In autocommit, one transaction spans the whole Arrow bind, so any
  failure rolls back every row of that bind and reports zero affected rows. One
  measurement on 4 RPU: 85 rows/s batched versus 5.4 rows/s row-at-a-time. High
  volume needs staged `COPY`, which is not implemented.
- `SUPER`, `GEOMETRY` and `GEOGRAPHY` (OIDs 4000, 3000, 3001) are not mapped.
  `JSON_SERIALIZE(super_column)` reads as Arrow text, but is limited by
  Redshift's `VARCHAR` size, so it must not become an implicit conversion.
- Table types are limited to `table` and `view`; external objects and
  materialized views are not reported specially.
- No `UNLOAD` or staged S3 `COPY`. The Redshift-private helpers
  (`redshift_staged_copy*`, `redshift_csv_writer`, `redshift_staged_arrow_*`)
  build a mandatory one-object manifest and quoted `COPY ... MANIFEST CSV` SQL
  with an exact, ordered column list. The CSV writer accepts only non-null
  INT32, INT64 and valid UTF-8 strings, caps the payload at 8 MiB, each row at
  4,000,000 bytes (below Redshift's 4 MB row limit) and a stream at 128
  batches, and rejects the literal `\N`. The coordinator only deletes objects it
  proved it created and reports cleanup separately from `COPY` success. Before
  enabling it: qualify NULL versus empty string and other types, add an S3
  adapter with short-lived credentials, define the transaction boundary, and
  get a reviewed least-privilege IAM grant.

Shared core (affects both backends):

- PostgreSQL replace ingest in autocommit runs `DROP`, `CREATE` and binary
  `COPY` in one driver-owned transaction; inside an explicit ADBC transaction it
  uses a private savepoint and never ends the caller's transaction. A raw-SQL
  `BEGIN` on an autocommit connection is refused before `DROP`. Shared
  preflight rejects zero fields, byte-identical duplicate names (no case
  folding, because backends differ), empty names and unsupported Arrow types.
- A failed or unconfirmed `COMMIT`, rollback, timezone restore or `COPY`
  termination marks the connection unusable; the caller must close and
  recreate it. A lost `COMMIT` reply can still leave the durable outcome
  unknown. The driver never retries.
- Binary ingest `COPY` sends `CopyFail` on client errors, keeps the server's
  SQLSTATE, and reports zero affected rows unless the whole transfer succeeds.
- Timezone-aware bound queries: an unfinished result stream holds a
  per-connection lease that rejects other ADBC SQL, transaction and option
  changes until it is released. Not yet qualified: output-schema failure
  injection, cleanup-SQL failure on a healthy connection, concurrent connection
  use, and deterministic lost-reply injection.
- Timestamps outside Arrow's signed 64-bit microsecond range return a range
  error (Redshift allows years up to 294276). Multidimensional arrays return an
  unsupported error. After a fatal malformed `COPY` field the Arrow builder may
  be partially filled.
- PostgreSQL `GetTableTypes` keeps Apache's historical order, which downstream
  GLib/Ruby tests check.

### Platform qualification

Only development archives exist; they are not production releases. A release
claim needs an installed-artifact test on each target plus database-backed
behavior tests. Do not infer Debian support from Ubuntu. The installed shared
library needs a compatible libpq at runtime; static consumers must link the
transitive dependencies from the package metadata.

| Target | Evidence | Still needed |
| --- | --- | --- |
| Ubuntu 24.04 x86-64 | CMake and Meson tests, PostgreSQL 18, live Redshift; relocated Debug client and checked Release archive connected to Redshift | Production release artifact |
| Ubuntu 24.04 ARM64 | Native CMake build, PostgreSQL 18, checked archive with extracted-client query and Redshift-driver rejection | Release artifact, live Redshift client test |
| Debian Bookworm x86-64 | Same as Ubuntu ARM64 | Release artifact, live Redshift client test |
| macOS Intel | Same as Ubuntu ARM64 | Release artifact, live Redshift client test |
| macOS Apple Silicon | Same, plus Meson ASan/UBSan local runs | Release artifact, live Redshift client test |
| Windows x64, ARM64 | Native vcpkg Release build, checked ZIP with vcpkg dependency licenses, extracted-client PostgreSQL 18 query and Redshift-driver rejection | Release artifact, live Redshift client test |

## Live Redshift testing

The budget, frequency limit and cost formula are in
[`CLAUDE.md`](../../../CLAUDE.md); every live run is logged in `STATUS.md`.

- Live tests run only through `workflow_dispatch` of `PgWire Drivers`, started
  by the coordinator, on a merged head with green CI. Never run Redshift for
  PostgreSQL-only changes, and do not retry automatically.
- Target: Redshift Serverless workgroup `pgwire-ci` in `eu-central-1`, fixed at
  4 RPU. Idle workgroups bill only storage, so avoid keepalive connections,
  polling and idle open transactions.
- The job builds before requesting short-lived OIDC credentials for the role
  `adbc-redshift-ci`, opens TCP 5439 for the runner's `/32` only, runs the
  focused tests, and revokes the rule in an `always()` step. The role's trust is
  pinned to this repository and branch; it may change ingress only on the
  dedicated security group. Live runs use a non-canceling concurrency group so
  a push cannot interrupt cleanup.
- Each ingress rule is tagged `adbc-pgwire-live-<run-id>-<attempt>` (COPY
  fixture: `adbc-pgwire-copy-...`). Cleanup revokes only a verified rule it
  owns, never a pre-existing rule by CIDR, and fails visibly on ambiguity. A
  lost runner or force-canceled job bypasses cleanup, so audit the security
  group after each run.
- Delete temporary schemas, tables and objects. Do not add capacity, buy
  reservations or change IAM without human approval.
- The staged-`COPY` fixture uses bucket `adbc-pgwire-ci-149112076833-euc1`
  (prefix `staging/` expires after one day, which is only a backstop) and the
  read-only role `adbc-pgwire-ci-copy`, trusted through the exact database-user
  `sts:ExternalId` for `IAMR:adbc-redshift-ci`. Uploads are conditional
  (`If-None-Match: *`) and deletes match the confirmed ETag. ETag deletion also
  needs `s3:GetObject`, which the uploader does not have, so the fixture stays
  blocked until that grant is approved and the workflow sets
  `PGWIRE_COPY_GETOBJECT_VERIFIED=true`.

## Upstream alignment

Backend-neutral PostgreSQL fixes are kept as small branches on the fork, based
on Apache `main`, with no Redshift code: `feature/pgwire-libpq-raii-upstream`,
`feature/pgwire-libpq-cancel-upstream`, `feature/pgwire-libpq-stream-upstream`,
`feature/pgwire-parameter-schema-upstream`,
`feature/upstream-pg-getobjects-column-constraints`,
`feature/upstream-pg-timestamp-range`, `feature/upstream-pg-array-shape`,
`feature/upstream-pg-jsonb-validation`, `feature/upstream-pg-copy-bounds`,
`feature/upstream-pg-copy-framing` (stacked on bounds),
`feature/upstream-pg-copy-header`, `feature/upstream-pg-copy-trailer` and
`feature/upstream-pg-copy-output-schema`. No Apache PR has been opened. After a
rebase, rerun the PostgreSQL, Redshift artifact and installed-client checks.
