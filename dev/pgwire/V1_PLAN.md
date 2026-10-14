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

**v1 = the first public release that a Python or C/C++ user can install and use
against Amazon Redshift, with honest documented limits.** It is not a claim that
every failure edge is qualified.

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
| 004b | Release hardening before the first real tag (PR #4 review): build binaries with the release version instead of `1.13.0-SNAPSHOT`; no vcpkg cache restore on tag runs; load-test the manifest on one macOS and one Windows runner; "Load by name" section in the package READMEs; run the release dry run when `pgwire-package.yml` changes; stricter semver regex; reproducible archives (fixed mtimes, `gzip -n`) or drop the claim. | Merged, dry run green |
| 005 | Python package `adbc_driver_redshift`, mirroring `python/adbc_driver_postgresql` (wheels for the same targets, DBAPI `connect()`), built in CI. | Wheels built and smoke-tested against PostgreSQL rejection + packaged import |
| 006 | IAM authentication without native AWS SDK: a documented recipe for temporary credentials (`GetCredentials` / `GetClusterCredentials`) and an optional Python helper (boto3 extra) that builds the URI. | Unit tests offline; one live check in the next batch |
| 007 | User documentation: install (C, Python), connect (password, IAM recipe), type mapping, support matrix, known limitations (non-atomic Redshift replace, unsupported features). | Reviewed docs page |
| 008 | Release qualification live batch: installed wheel + installed C archive on Ubuntu x64 against Redshift (query, params, metadata, ingest, IAM). | One green bounded batch |
| 009 | Release candidate: version number, changelog, draft release. **Human approves publication** (GitHub Release; PyPI only if approved). | Human sign-off |

## Explicitly after v1

Native C++ IAM/Identity Center, staged S3 `COPY`/`UNLOAD`, `SUPER`/spatial
mapping beyond text, atomic Redshift replace ingest, remaining timezone
bound-query cleanup edges (output-schema failure injection, cleanup-SQL failure,
concurrent use), deterministic lost-reply injection. Each needs a concrete user
need before it is scheduled.
