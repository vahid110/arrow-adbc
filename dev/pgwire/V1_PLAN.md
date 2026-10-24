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

# Redshift driver v1 plan

**v1 = this fork's first public release that a Python or C/C++ user can install
and use against Amazon Redshift, with honest documented limits.** It is not a claim that
every failure edge is qualified.

## Product direction

This fork is the independent Redshift ADBC delivery route: fully open source,
inspectable, buildable, modifiable and redistributable under the existing
applicable licenses and notices. Do not claim first-to-market status or
unbenchmarked speed superiority. ODBCPP remains the ODBC product/connectivity
SDK; a duplicate ODBCPP ADBC adapter, Arrow integration and packaging are
not scheduled without a concrete unmet need and a new product decision.

V1 keeps Task006's caller-owned boto3 IAM helper thin; no native auth framework
or extra auth methods. Task007 docs and Task008a/b/c/d are merged. The installed
C archive/Linux x86-64 wheel temporary-IAM batch
[39138655470](https://github.com/vahid110/arrow-adbc/actions/runs/39138655470)
passed on `3460c541914560a91872e4417c4c3b4ccbb81df0`, with independently verified
full physical/managed-secret/IAM teardown. Its int64/query/parameter/metadata/
ingest scope is complete; original helper uncertainty and prior failure receipts
remain immutable, and actual billing is not reconciled.

The human reprioritized **all 21 Redshift SQL table types before first release**
on 2026-10-18. [Task010's evidence matrix and representation proposals](REDSHIFT_TYPES.md)
separate read, bind, create, append and metadata; the passed batch is not all-type
qualification. Task010a source audit/docs is merged (PR #21). Task010b implements
both interval families' native read/schema/family metadata with strict offline
reference fixtures, codec-produced C/Python C Data checks and unsupported-write
guards (PR #22 merged; offline CI passed). Task010c1 adds per-field modifiers and
bounded numeric/character schema/catalog metadata, with parameter identity and
unknowns retained (PR23 merged; offline CI passed). Task010c2 adds known-p/s
decimal128 reads and strict exact-text unknowns with reference codec/C Data tests
(PR24 merged; offline CI passed). Task010c3 adds explicit legal decimal DDL,
exact decimal128/256 and DECIMAL-text binding, target-aware append and owned
rollback uncertainty handling (locally integrated d04f2d2; independent review and
serial native/installed validation passed). Task010c4 adds checked integer/float/
boolean read/bind/target-aware ingest and canonical identity (locally integrated
b90664a; independent review and serial native/installed validation passed).
Task010d adds character/binary identity, checked widths and explicit target-aware
ingest (locally integrated c07f811; independent review and all 20 serial
native/installed validation steps passed). Its provisional plain VARBYTE limit
is a native/release gate. Task010e1 implements DATE/TIME/TIMESTAMP/TIMESTAMPTZ
with field-local Arrow/lossless modes, checked units/epochs and source/target
precision (locally integrated 7fc46fa; independent FULL review and all 22
validation steps passed). Task010e2 supplies versioned interval contracts for all
13 qualifiers and exact reference codecs/DDL (locally integrated 35584f5;
independent FULL approval and all 24 serial validation steps passed).
Task010e3 implements the fixed-offset TIMETZ struct/reference codec, explicit
source precision, actual-target precision checks and bounded bare declarations;
locally integrated e2df066 after independent FULL approval and all 25 serial
validation steps. Task010f0 adds a noninstalled bounded protocol observer and
fixed plans (locally integrated 65e7e9b; independent FULL approval and all 30
serial validation steps passed). Task010g0 links anchored historical local
source/review/validation/log/package bytes (locally integrated 1f35366 after
independent FULL approval, actual historical linkage and retained validation).
Task010g1 adds trusted in-place install comparisons and fixed same-host load-only
observations (locally integrated 0285eba after actual named G0 artifact load,
retained 30-step validation and exact independent approval). Collector G1 and
artifact G0 identities remain distinct. Task010g2 adds a bounded output fd for
the existing fixed seven-probe own-PG plumbing test (locally integrated cfe9feb;
actual channel/stale-fd/cleanup checks, all 31 validation steps and exact independent
approval). Task010g4 adds a noninstalled owned single raw replay parameter with
separate controlled NULL/empty trials (locally integrated def7084; independent
approval, all 31 validation steps and ten own-PG echoes/15 calls). Task010h0 adds
closed compiled discovery plans/capture shapes (locally integrated 4bdc23b;
independent approval, all 31 serial stages and nine own-PG checks/15 calls).
Task010h1's noninstalled closed 715-step async runner and eight bounded output
files are locally integrated at `3c956e3` after exact independent approval and all 32
serial validation stages. Later private admitted runs retain raw native protocol
observations separately from installed-driver acceptance; see
[the local evidence boundary](local-qualification-evidence.md). These components
grant no live authority. Task010s completed bounded interval target qualification
through four installed Python/C Bind/BindStream legs and independent teardown
audits. Final review and applicable green CI remain gates; see
[STATUS](STATUS.md#task010s-interval-ingestion-qualification).
The fixed temporal endpoints and precision cases listed in [STATUS](STATUS.md)
have installed live evidence; exhaustive domains and sentinels remain qualification
gates. Platform CI follows the local-first workflow.
Remaining libpq-path/boundary qualification, full interval-domain proof and
native character/binary boundaries, TIMETZ layout/domain/offset/precision, SUPER, HLL and spatial
full-envelope qualification remain open; the bounded installed vendor subset
below does not close the full 21-type milestone.
Task009 publication and shared-auth work follow that milestone. This roadmap
grants no new AWS/spending, live dispatch, tag or publication authority; AO/timer
remain paused. No release-ready claim is made.

## Already true (do not redo)

Queries with Arrow results, prepared/bound parameters, core types incl. `VARBYTE`,
`GetInfo`/`GetTableTypes`/`GetObjects`/`GetTableSchema`, autocommit and explicit
transactions, create/append bulk ingest via multi-row prepared INSERT, query
cancellation, dedicated `adbc_driver_redshift` library that rejects non-Redshift
servers, development archives with clean-client tests on 7 targets (Ubuntu
x64/arm64, Debian x64, macOS x64/arm64, Windows x64/arm64).

## v1 queue (in order)

| # | Task | Done when |
|---|---|---|
| 001 | Land parked work from `task/001-land-pending`: (a) SQLSTATE-preserving error import, (b) PostgreSQL explicit-transaction savepoint recovery for replace ingest, (c) Redshift identifier-collision preflight wired into replace. Split into reviewable commits/PRs if large. | Done (PR #2) |
| 002 | Dropped: `Integration` is disabled in this fork (Apache-wide integration is out of scope). | - |
| 003 | Cut `docs/source/driver/pgwire-development.md` (1,900 lines) to a concise developer guide: architecture, extension points, test commands, support matrix. History stays in git. | Done (PR #3) |
| 004 | Release workflow (`pgwire-release.yml`): tag `redshift-v*` builds 7 targets, archives + `SHA256SUMS` + `redshift.toml` manifest, draft GitHub Release. | Done (PR #4) |
| 004b | Release hardening before the first real tag (PR #4 review): build binaries with the release version instead of `1.13.0-SNAPSHOT`; no vcpkg cache restore on tag runs; load-test the manifest on one macOS and one Windows runner; "Load by name" section in the package READMEs; run the release dry run when `pgwire-package.yml` changes; stricter semver regex; reproducible archives (fixed mtimes, `gzip -n`) or drop the claim. Human: tag ruleset limiting who may create `redshift-v*` tags (repo setting). | Merged, dry run green |
| 005 | Python package `adbc_driver_redshift`, mirroring `python/adbc_driver_postgresql` (DBAPI `connect()`, bundled library, entrypoint `AdbcDriverRedshiftInit`): one Linux x86-64 wheel (`py3-none-linux_x86_64`) built and tested in CI (`PgWire Python`), linked against the system libpq. | Wheel built, installed and tested against PostgreSQL rejection + packaged import |
| 005b | Portable wheels mirroring upstream's `ci/scripts/python_wheel_*` scripts: Linux x64/arm64 (manylinux, static libpq via vcpkg plus auditwheel), macOS x64/arm64 (delocate), Windows x64; wheels attached to the draft release; wheel version mapped from the `redshift-v*` tag (`SETUPTOOLS_SCM_PRETEND_VERSION`). | Wheels built for all targets and import/rejection-tested on each |
| 006 | IAM authentication without native AWS SDK: a documented recipe for temporary credentials (`GetCredentials` / `GetClusterCredentials`) and an optional Python helper (boto3 extra) that builds the URI. | Merged (PR #11); Serverless temporary-IAM path qualified in run 39138655470; provisioned-cluster helper remains offline-only |
| 007 | User documentation: install (C, Python), connect (password, IAM recipe), type mapping, support matrix, known limitations, OSS positioning and approved shared-auth roadmap. | Done (PR #12) |
| 008a | Offline isolated-resource admission contract, installed-artifact preflight and fake-only rehearsal; retire shared-target dispatch. | Done (PR #13); live admission stays closed pending externally verified dedicated evidence |
| 008b | Installed C/Python private-input clients, no-AWS plumbing CI and machine-readable external admission handoff. | Done (PR #14), offline only; original unsigned gate stays closed |
| 008c | Executable finite SSM admission, authenticated installed-artifact runner and host lifecycle. | Merged (PRs #15–#19); installed client batch and independent teardown completed in run 39138655470 |
| 008d | Versioned exact-claim IAM contract, explicit nonce admission scopes, immutable host reservation and static consumer diagnostics. | Merged (PR #20); corrected operation qualified and fully torn down; retained one-use evidence, no retry authority |
| 008 | Release qualification live batch: installed wheel + installed C archive on Ubuntu x64 against Redshift (query, params, metadata, ingest, IAM). | Complete within run 39138655470 scope; int64/query/params/IAM and full teardown, not all types or APIs |
| 010a | Full21-type source audit, separate operation evidence and first-release representation/acceptance contract. | Merged (PR21); docs/audit only; [type contract](REDSHIFT_TYPES.md) |
| 010 | Complete all 21 Redshift table types before release: bounded interval, scalar/modifier, SUPER, HLL/spatial implementation and installed-artifact qualification phases. | Acceptance: every operation is proven or explicitly bounded/rejected, native reads require no user rewrites, unresolved representations closed; separate finite live authority required |
| 009 | Release candidate: version number, changelog, draft release. **Human approves publication** (GitHub Release; PyPI only if approved). | Human sign-off |

## Explicitly after v1

After the all-type first-release milestone, the shared-auth direction is the **same existing ODBCPP
AuthCore/optional provider implementation** consumed by ODBC and independently
built ADBC as a second-consumer proof, without an ODBCPP connectivity dependency.
Use private interfaces until that proof establishes the contract; public stable
ABI/bindings, independent packaging/stability promises, an auth service and full
federation are deferred. This does not resume parked ODBCPP AUTH or prove external
commercial demand. See the [shared-auth boundaries and proof requirements](../../docs/source/driver/pgwire-development.md#shared-auth-after-v1-qualification).
No implementation or automatic successor is authorized by this direction.

General staged S3 `COPY`/`UNLOAD`, atomic Redshift
replace ingest, remaining timezone bound-query cleanup edges (output-schema failure injection, cleanup-SQL failure,
concurrent use), deterministic lost-reply injection. Each needs a concrete user
need before it is scheduled. Type-specific large-value transport requirements
identified by Task010 are release decisions, not a blanket type deferral.


Task010i1/i2/j/k are locally integrated at `a239f66cc` with independent source
approval and rebuilt/installed offline validation. Operation `726f4b3594e048e29f4571b2770549c3` on 2026-10-21
passed the fixed 18-case macOS arm64 installed-driver subset: SUPER JSON-subset
CREATE/APPEND, spatial CREATE/readback, HLL CREATE/readback/text parameters,
VARBYTE binary readback and three explicit rejection boundaries. That batch tested `a239f66cc`; it does not verify Task010q.
All seven owned resource classes were verified absent after child reap.
Task010q/q1 at `26fb72348a6dee794d86d47b70d66d487f9894a0` adds native SUPER
observable JSON-wire reads with exact received lexemes, an output-only schema
and explicit wire-NULL/subtype limits. The 2026-10-22 installed macOS arm64
qualification passed Python Arrow-mode 76/76, Python lossless 11/11, C interval
reads 37/37 and C lossless 24/24. Python writes passed 48 positive
create/stream/append cases plus 12 unsupported interval-target rejection/retention
cases; positive interval ingestion is not claimed. All isolated attempts received
verified full teardown.

The original C Arrow-mode leg remains 22/23: its SUPER fixture failure is retained.
The corrected fixture passed with unchanged expected bytes through the same C
SUPER decoder in lossless mode; a corrected Arrow-mode rerun is not claimed.
See [STATUS](STATUS.md) for the bounded acceptance results and remaining limits.
Full 21-type/domain/boundary/platform acceptance remains open. Next, complete
final exact-head review and applicable green remote CI, then resolve the remaining
release gates before Task009 publication. Mixed HLL output and direct
SUPER/spatial binds remain unsupported.
No arbitrary SQL rewrite or fallback is selected. Existing size caps, NULL semantics and transaction ownership remain.

Task010r's bounded macOS arm64 character/binary read and four corrected VARBYTE
write-route legs are complete, with full teardown recorded by their controllers.
See [STATUS](STATUS.md#task010r-characterbinary-qualification) for source-specific
evidence, retained failures and scope. Final exact-head review, applicable green
remote CI and the remaining release gates are still required before publication.
