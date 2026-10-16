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

V1 keeps task006's boto3 IAM helper thin, caller-owned and replaceable: no native
C++ auth framework, extra auth methods, federation/Identity Center, shared
credential cache or background refresh. Task007 documentation is complete
(PR #12), as are Task008a/b/c (PRs #13/#14/#15). Task008c adds the reviewed finite
SSM admission and explicit-dispatch runner for installed C/Python IAM payloads,
with authenticated source/artifact/resource checks and mandatory independent host
cleanup. See [the executable host handoff](qualification-live.md). PR #16 repaired
pre-OIDC preparation. The next dispatch passed installed preflight but failed
OIDC; the successor pins authenticated immutable owner/repository IDs in the
host trust validator. Reviewed host-only role trust update/readback is still
required before the next dispatch; original failures and budgets are preserved.
Offline success is not live qualification: one exact merged-head batch and verified dedicated
resource/secret/IAM teardown remain required. The human publication gate remains;
this roadmap grants no additional spend, tag or publication authority.

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
| 006 | IAM authentication without native AWS SDK: a documented recipe for temporary credentials (`GetCredentials` / `GetClusterCredentials`) and an optional Python helper (boto3 extra) that builds the URI. | Offline implementation done (PR #11); live check deferred to a separately human-authorized qualification batch |
| 007 | User documentation: install (C, Python), connect (password, IAM recipe), type mapping, support matrix, known limitations, OSS positioning and approved shared-auth roadmap. | Done (PR #12) |
| 008a | Offline isolated-resource admission contract, installed-artifact preflight and fake-only rehearsal; retire shared-target dispatch. | Done (PR #13); live admission stays closed pending externally verified dedicated evidence |
| 008b | Installed C/Python private-input clients, no-AWS plumbing CI and machine-readable external admission handoff. | Done (PR #14), offline only; original unsigned gate stays closed |
| 008c | Executable finite SSM admission, authenticated installed-artifact runner and host lifecycle. | Merged (PRs #15/#16); immutable OIDC subject repair pending review and host trust update. Actual host-operated batch/teardown still required |
| 008 | Release qualification live batch: installed wheel + installed C archive on Ubuntu x64 against Redshift (query, params, metadata, ingest, IAM). | Pending: reviewed/green Task008c, dedicated host-issued admission, one green bounded batch and independently verified full teardown |
| 009 | Release candidate: version number, changelog, draft release. **Human approves publication** (GitHub Release; PyPI only if approved). | Human sign-off |

## Explicitly after v1

After v1 qualification, the shared-auth direction is the **same existing ODBCPP
AuthCore/optional provider implementation** consumed by ODBC and independently
built ADBC as a second-consumer proof, without an ODBCPP connectivity dependency.
Use private interfaces until that proof establishes the contract; public stable
ABI/bindings, independent packaging/stability promises, an auth service and full
federation are deferred. This does not resume parked ODBCPP AUTH or prove external
commercial demand. See the [shared-auth boundaries and proof requirements](../../docs/source/driver/pgwire-development.md#shared-auth-after-v1-qualification).
No implementation or automatic successor is authorized by this direction.

Staged S3 `COPY`/`UNLOAD`, `SUPER`/spatial mapping beyond text, atomic Redshift
replace ingest, remaining timezone bound-query cleanup edges (output-schema failure injection, cleanup-SQL failure,
concurrent use), deterministic lost-reply injection. Each needs a concrete user
need before it is scheduled.
