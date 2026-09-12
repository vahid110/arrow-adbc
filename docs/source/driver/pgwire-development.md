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

# PostgreSQL-Wire Driver Development Plan

This document tracks the implementation of a reusable C++ ADBC core for databases
that expose the PostgreSQL wire protocol. PostgreSQL remains the compatibility
baseline and Amazon Redshift is the first additional backend.

The design rule is: common code owns libpq/wire interaction and ADBC mechanics;
each backend owns its SQL semantics, catalog interpretation, type meaning,
transaction behavior, authentication preparation, and supported transfer paths.

## Upstream baseline

- Repository: `apache/arrow-adbc`
- Baseline branch: `main`
- Baseline commit: `150528f0fb9f1117fecce1da6a27fb548ddc6d0f`
- Development branch: `feature/pgwire-core-redshift`
- Historical references: PR #2219 (experimental Redshift support) and PR #4365
  (removal of that support)

The implementation should remain a sequence of small commits that can be rebased
onto Apache `main`. If Apache accepts an equivalent refactor, the corresponding
local commit should be dropped rather than retained as a competing implementation.

## Architectural target

`PgWireDatabase`, `PgWireConnection`, and `PgWireStatement` will own ADBC state and
compose a backend profile selected once after connecting. The profile will contain
capability data and focused type, metadata, transaction, query, and bulk-transfer
policies. It will not be a dynamic plugin framework.

The core continues to use libpq; it does not implement protocol framing, TLS, or
authentication exchanges itself.

## Milestones

- [x] Pin and record the upstream baseline.
- [x] Record the current PostgreSQL build/test baseline.
- [x] Extract libpq connection, result, cancellation, and error lifetime helpers.
- [x] Separate text-result and binary-COPY query paths.
- [x] Separate type discovery, Arrow mapping, and value encoding.
- [x] Move PostgreSQL metadata SQL behind a normalized metadata provider.
- [x] Isolate PostgreSQL transaction policy.
- [x] Construct the existing PostgreSQL driver from the reusable core and
      PostgreSQL semantics, with no intended behavior change.
- [x] Add Redshift detection and a read/query-only profile.
- [x] Add the verified Redshift scalar type matrix.
- [x] Add Redshift `GetInfo`, `GetTableTypes`, `GetObjects`, and `GetTableSchema`.
- [x] Add Redshift transaction coverage.
- [x] Add correctness-first batched-INSERT ingestion.
- [x] Package distinct PostgreSQL and Redshift driver artifacts.
- [x] Keep CMake and Meson source, artifact, package, and focused-test coverage in
      parity.

## Quality gates

Every structural milestone must:

1. Build as both shared and static libraries through the existing build system.
2. Preserve the full PostgreSQL test suite and validation behavior.
3. Add focused unit tests for new seams and failure paths.
4. Avoid backend-name conditionals in core execution code.
5. Return explicit `ADBC_STATUS_NOT_IMPLEMENTED` for unsupported semantics instead
   of incomplete or misleading results.
6. Keep public PostgreSQL symbols, package names, option names, and defaults stable.

## Redshift MVP support matrix

| Area | MVP behavior |
| --- | --- |
| Driver artifact | Dedicated shared/static `adbc_driver_redshift` library and `AdbcDriverRedshiftInit`; rejects non-Redshift servers |
| Connection/authentication | Standard libpq URI options, TLS, user/password; no Redshift-specific IAM token generation yet |
| Vendor detection | `SELECT version()` once during database initialization; required by the Redshift artifact |
| Query results | Portable libpq text-result path with Arrow conversion; PostgreSQL alone retains binary query `COPY` |
| Parameters | Prepared statements with binary parameter encoding |
| Core types | Boolean, signed integers, float, numeric-as-string, date/time/timestamp, text/varchar, and `VARBYTE` |
| Metadata | `GetInfo`, `GetTableTypes`, `GetObjects`, and `GetTableSchema`; constraints and statistics are explicitly unsupported |
| Transactions | Autocommit, explicit commit/rollback; isolation is database-configured and session overrides are explicitly unsupported |
| Bulk ingest | Create and append through atomic prepared inserts; PostgreSQL alone retains binary ingest `COPY` |

Post-MVP work should be driven by concrete use cases: IAM/Identity Center credential
helpers, Redshift-native staged `COPY`/`UNLOAD`, additional Redshift types such as
`SUPER` and spatial values, richer external/materialized-view metadata, and a
multi-row or pipeline insert optimization that preserves the current atomic/error
semantics. These do not belong in the core until a second implementation or a
measured Redshift requirement demonstrates the extension point.

## Production-readiness roadmap

The MVP is not a production-readiness claim. Work in small, independently buildable
and testable checkpoints; record the result of each checkpoint below. A green build
is evidence, not a substitute for a clean-client test or documented limitations.

### 1. Installable and usable driver

- [ ] Publish a Redshift build/install/connection guide and explicit MVP limits.
- [ ] Exercise the installed shared library through the ADBC driver manager from
      a clean prefix, independent of the driver-linked unit test binary.
- [ ] Add a CI smoke test for the installed artifact and its public header/package
      metadata on Linux.

### 2. Correctness and compatibility

- [ ] Expand focused live Redshift tests for NULL values, errors, cancellation,
      larger results, and type/metadata edge cases.
- [ ] Keep PostgreSQL's complete integration suite and downstream client
      compatibility tests green; preserve observed public behavior and ordering.
- [ ] Define a documented support matrix with explicit unsupported features and
      test evidence for each claim.

### 3. Redshift-native capabilities

- [ ] Add optional, short-lived IAM credential preparation without changing the
      common libpq authentication path.
- [ ] Benchmark prepared-insert throughput before choosing an optimization.
- [ ] If justified, add staged S3 `COPY` ingestion with least-privilege IAM,
      deterministic object cleanup, and a separate opt-in live test. Consider
      `UNLOAD` only after a measured read-path need.
- [ ] Extend `SUPER`, spatial, external-object, or materialized-view support only
      with verified Redshift behavior and tests.

### 4. Release and upstream alignment

- [ ] Split backend-neutral PostgreSQL-wire refactors into small Apache-ready
      contributions, independent of Redshift-specific behavior.
- [ ] Publish open-source release artifacts, install instructions, checksums,
      dependency requirements, and a tested platform matrix.
- [ ] Rebase on a newer Apache baseline after checking upstream changes and
      rerunning PostgreSQL, Redshift, and downstream compatibility suites.

### Platform and architecture qualification

The current evidence proves source builds, not prebuilt binary support. A release
claim requires an installed-artifact smoke test on each target plus appropriate
database-backed behavior tests. Do not infer Debian support from Ubuntu alone.

| Target | Current evidence | Release qualification still needed |
| --- | --- | --- |
| Ubuntu 24.04 x86-64 | CMake/Meson builds and tests; PostgreSQL 18 and live Redshift | Clean install/client smoke and release artifact |
| macOS Intel | C++ build | Clean install/client smoke and live database test |
| macOS Apple Silicon | C++ build | Clean install/client smoke and live database test |
| Windows x86-64 | C++ build, including vcpkg | Clean install/client smoke and live database test |
| Windows ARM64 | vcpkg release build | Clean install/client smoke and live database test |
| Debian x86-64 | Not separately tested | Dedicated container build, install, client, and database tests |
| Linux ARM64 | Not tested | Native or cross-build, install, client, and database tests |

- [ ] Add a Linux x86-64 clean-prefix installation test first.
- [ ] Add macOS Intel/Apple Silicon and Windows x86-64/ARM64 installation tests.
- [ ] Add dedicated Debian x86-64 and Linux ARM64 CI jobs.
- [ ] Publish packages only for targets whose release qualification is green.

## Redshift test-cost discipline

- Use the `eu-central-1` Serverless workgroup `pgwire-ci`, capped at 4 RPUs.
- Keep test runs focused and batched; do not run Redshift for PostgreSQL-only changes.
- Avoid keepalive connections, polling queries, and idle open transactions so that
  Serverless can return to its non-compute-billed idle state promptly.
- Check trial-credit and RPU usage before and after larger integration runs.
- Delete temporary schemas, tables, snapshots, and other billable test artifacts.
- Do not purchase reservations or increase capacity without explicit approval.
- When Redshift testing is no longer active, evaluate deleting the workgroup and
  namespace; recreate them when needed rather than carrying avoidable storage cost.

## Current work

The planned Redshift MVP and reusable-core milestones are complete. PostgreSQL CI
remains the first mandatory gate. After it passes, the Redshift job builds before
requesting short-lived AWS credentials, opens TCP 5439 for only the current GitHub
runner `/32`, runs only the focused Redshift suite, and revokes that exact rule in
an `always()` cleanup step. The AWS role can modify ingress on only the dedicated
Redshift security group and its OIDC trust is pinned to this repository's immutable
owner/repository IDs plus the development branch.

## Progress log

- 2026-09-09: Created the local fork and development branch from Apache `main`.
- 2026-09-09: Added this tracked implementation plan before changing driver code.
- 2026-09-09: Built both PostgreSQL test executables against libpq 18.6. The 22
  database-independent COPY reader tests pass. Database-backed tests require an
  `ADBC_POSTGRESQL_TEST_URI` and are pending a local test service.
- 2026-09-09: Began extracting backend-neutral libpq RAII handles and added
  database-independent ownership tests.
- 2026-09-09: Migrated `PqResultHelper` to the reusable result handle. Both
  PostgreSQL test binaries compile; the focused error, ownership, and type suite
  passes 9/9 tests.
- 2026-09-09: Added immutable PostgreSQL and conservative Redshift capability
  profiles. PostgreSQL vendor identity now comes from the composed profile without
  changing its public value.
- 2026-09-09: Published the development branch to `vahid110/arrow-adbc`, retained
  Apache as the `upstream` remote, enabled GitHub Actions, and added a focused C++
  integration workflow backed by PostgreSQL 18.
- 2026-09-09: GitHub Actions run `34384221476` built both PostgreSQL test binaries
  and passed the complete `driver-postgresql` CTest label against PostgreSQL 18.
- 2026-09-12: Created the `pgwire-ci` Redshift Serverless workgroup and namespace
  in Frankfurt with base and maximum capacity fixed at 4 RPUs. Recorded the
  cost-control rules above before enabling Redshift integration tests.
- 2026-09-12: Migrated PostgreSQL connection cancellation ownership to the common
  pgwire RAII handle, removing the remaining manual `PGcancel` cleanup path.
- 2026-09-12: Migrated streaming query result ownership to the common pgwire RAII
  handle and kept response draining explicit, eliminating another manual cleanup
  path without changing COPY behavior.
- 2026-09-12: Added an explicit query-result transport selector. PostgreSQL keeps
  binary COPY when enabled, while capability-limited backends such as Redshift
  select the portable text-result path.
- 2026-09-12: Added one-time Redshift detection from `SELECT version()` and parse
  the vendor version using the selected backend profile. Unknown PostgreSQL-wire
  servers retain the PostgreSQL-compatible default.
- 2026-09-12: GitHub Actions run `34686322856` passed the complete PostgreSQL 18
  integration suite after the result-transport and backend-detection refactors.
- 2026-09-12: Made type-catalog discovery capability-aware for Redshift's missing
  `pg_type.typarray`, used the cached Redshift vendor version for `GetInfo`, and
  disabled unsupported constraint discovery without affecting PostgreSQL SQL.
- 2026-09-12: Added an opt-in Redshift smoke test, gated by
  `ADBC_REDSHIFT_TEST_URI`, covering database initialization, vendor version
  reporting, text-result execution, schema mapping, and scalar values.
- 2026-09-12: Added capability-driven bulk-ingest selection and explicit
  `NOT_IMPLEMENTED` results for Redshift ingestion and statistics, preventing
  accidental execution of unsupported PostgreSQL-specific paths.
- 2026-09-12: GitHub Actions runs `34686600361`, `34686724734`, and `34686810469`
  passed the complete PostgreSQL 18 suite for catalog adaptation, Redshift smoke
  test integration, and unsupported-path guards respectively.
- 2026-09-12: Generated a dedicated Redshift admin credential and stored the
  connection URI only as the encrypted GitHub Actions secret
  `ADBC_REDSHIFT_TEST_URI`; no credential value is stored in the repository.
- 2026-09-12: Restricted the Redshift security group to TCP 5439 from the current
  development address as a `/32`. Public endpoint activation is limited by that
  rule; broad public ingress is not permitted.
- 2026-09-12: Enabled the public endpoint behind the restricted security group and
  authenticated over required SSL. The live server reported Redshift
  `1.0.436211`.
- 2026-09-12: Expanded the opt-in live suite to five tests covering twelve core
  scalar mappings, value decoding, `GetInfo`, Redshift-specific table types,
  `GetObjects`, `GetTableSchema`, and explicit commit/rollback. All five tests
  passed; their temporary table was dropped and all test connections were closed.
- 2026-09-12: Moved table-type names and `pg_class.relkind` mappings into the
  backend profile. PostgreSQL retains its six existing table types while Redshift
  reports only the verified `table` and `view` types.
- 2026-09-12: Verified a bound `INT32` parameter and binary result round trip on
  Redshift. Marked binary parameters supported while keeping binary `COPY` query
  transport disabled; these are separate protocol capabilities.
- 2026-09-12: Added the first backend-owned type discovery alias, mapping
  Redshift's `varbyte_recv` identity to the core binary representation. A live
  `VARBYTE` query returned the expected Arrow binary schema and bytes.
- 2026-09-12: Added capability-selected, correctness-first Redshift ingestion via
  prepared binary parameters. PostgreSQL retains binary `COPY`; the Redshift path
  creates or appends through the same ADBC ingest mechanics and counts affected
  rows without adding vendor conditionals to statement execution.
- 2026-09-12: Wrapped each autocommit Redshift Arrow batch in one transaction so a
  later row failure cannot leave earlier rows committed. Live tests verified both
  a successful two-row create ingest and full rollback when the second appended
  row violated a `NOT NULL` constraint. The complete eight-test Redshift suite
  passed, and all temporary tables and connections were cleaned up.
- 2026-09-12: Completed libpq lifetime extraction across the PostgreSQL-wire
  implementation. Connection, cancellation, prepared/query results, transaction
  commands, table DDL, and COPY setup/completion now use the shared scoped handles;
  no manual `PQfinish`, `PQfreeCancel`, or `PQclear` calls remain in the driver.
- 2026-09-12: Added backend-owned transaction semantics. PostgreSQL retains
  session-configurable ADBC isolation levels and transactional DDL; Redshift is
  marked as database-configured and non-transactional for DDL. Because AWS marks
  `SET SESSION CHARACTERISTICS` as deprecated for Redshift, non-default ADBC
  isolation overrides now fail explicitly instead of reporting a misleading
  success; default isolation remains accepted. Live commit, rollback, and
  isolation-policy tests passed.
- 2026-09-12: Refactored driver initialization around one common ADBC function-table
  builder and distinct PostgreSQL and Redshift database factories. The public
  `AdbcDriverPostgresqlInit` behavior remains compatible and the new
  `AdbcDriverRedshiftInit` pins Redshift semantics, rejecting a mismatched server.
  The live Redshift connection test now runs through the named Redshift entry point.
- 2026-09-12: Added independently installable shared and static Redshift artifacts,
  `libadbc_driver_redshift`, whose standard `AdbcDriverInit` selects the Redshift
  factory. Added a public Redshift entry-point header, CMake package metadata,
  pkg-config metadata, an artifact-level test, and CI coverage. A temporary-prefix
  install verified every library, header, and package metadata output.
- 2026-09-12: Extracted type-catalog discovery from database lifecycle code into a
  focused component with capability-driven PostgreSQL and Redshift query plans.
  Catalog parsing feeds the existing type resolver, Arrow schema mapping remains
  in the type model, and wire value decoding/encoding remains in result and bind
  paths. Focused query-plan tests and a live Redshift scalar-mapping test passed.
- 2026-09-12: Moved catalog, schema, table, column, constraint, table-schema, and
  table-type SQL construction behind a normalized metadata query provider. The
  provider consumes only the backend profile, so core `GetObjects` iteration no
  longer owns backend SQL or table-kind mapping. Focused provider tests and live
  Redshift `GetObjects`, `GetTableSchema`, and `GetTableTypes` tests passed.
- 2026-09-12: Completed the final regression pass. All nine live Redshift tests
  passed through the named Redshift driver, including query, types, metadata,
  transactions, parameter binding, successful ingest, and ingest rollback. GitHub
  Actions run `34690357323` built both driver artifacts, passed the complete
  PostgreSQL 18 integration suite, verified that the Redshift driver rejects a
  PostgreSQL server, and passed the Redshift artifact-entry-point test.
- 2026-09-12: Registered GitHub's OIDC endpoint in AWS and created the
  `adbc-redshift-ci` role. Its trust policy requires the immutable
  `vahid110@5770674/arrow-adbc@1363017696` subject on
  `feature/pgwire-core-redshift`; its only resource permissions are authorizing
  and revoking ingress on `sg-00551344e2efb9c68`. Added a post-PostgreSQL live CI
  job that keeps the database port open only for the focused test interval and
  always removes the runner rule afterward.
- 2026-09-12: GitHub Actions run `34691303802` passed the complete PostgreSQL 18
  gate and all nine live Redshift tests using OIDC credentials. The temporary
  runner ingress rule was revoked successfully; an independent AWS console check
  confirmed that only the pre-existing default-group and development `/32` rules
  remained.
- 2026-09-12: Restored CMake/Meson parity after the core extraction. Meson now
  compiles the extracted metadata and type-discovery components into both drivers,
  produces and installs distinct PostgreSQL and Redshift shared libraries, public
  headers, and pkg-config files, and runs the common seam tests plus the Redshift
  artifact-entry-point test. A fresh Meson build completed all 84 targets; the 14
  database-independent core tests and the Redshift artifact test passed.
- 2026-09-12: Removed the remaining backend-name branches from common version and
  `GetInfo` execution. Each backend profile now owns its version prefix, version
  source policy, and public driver name; Redshift consequently identifies the
  driver as `ADBC Redshift Driver` instead of inheriting PostgreSQL's name.
- 2026-09-12: The broad GLib/Ruby integration gate exposed a PostgreSQL
  compatibility detail not asserted by the C++ suite: `GetTableTypes` ordering.
  Restored Apache's historical order in the PostgreSQL profile and added an exact
  provider test while leaving Redshift's verified `table`, `view` order unchanged.
