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

**V1 is this fork's first public Redshift ADBC release for installed Python and
C/C++ users, covering all 21 canonical SQL column types with accurate limits.**
A finite supported contract and checked rejection can establish readiness;
exhaustive mathematical-domain testing is not the goal. The human explicitly
required mixed HLL results and direct SUPER/spatial
parameters on 2026-10-28. Documenting rejection does not close acceptance.
The local candidate implements all three direct parameter families and B3 limits:
SUPER JSON input at 16MiB/depth1000, spatial reads/writes at 1,048,447 raw bytes,
and native JSON output bounded by received length rather than stored object size.
Independent source review, 35 serial local validation steps and Meson targets
passed at `aed6ced40fb058f56dc8b097937776c05cbe9496`. Selected installed
qualification passed 15 cases each in standalone C and Python, with independently
reviewed results and full teardown. Deliver this scoped PR, then pause. Mixed HLL
results remain separate mandatory B1 work;
selected B3 cases do not establish all maxima, HLL transitions or B5 metadata.

## Current baseline

Merged PR29 is `fca2eab2e05e1a63cfbd9adecc802c53334bdbd0`, reviewed tree
`0fa498a6e88c30eb1758195c614755a9942c51af`. Task010t's 234 installed edge cases,
independent review, applicable green CI and full resource closure are complete.
Its original C-stream cleanup timeout remains separate from successful residual
cleanup. Earlier L/M/Q/R/S qualifications retain their own source, platform and
operation scopes; [STATUS](STATUS.md) records the results.

The [21-type matrix](REDSHIFT_TYPES.md#current-evidence-by-operation) separates
native read, direct prepared bind, generated CREATE, APPEND and metadata.
[V1_ACCEPTANCE.md](V1_ACCEPTANCE.md) is the finite remaining-work checklist.
The merged baseline's generated vendor ingestion does not establish direct
parameter support in the candidate; all-HLL reads and scalar HLL cardinality
outputs do not satisfy mixed HLL result rows. Unknown interval public constraints
remain honestly unknown despite internal target qualification.

## Delivery order

| Phase | Work | Exit criterion |
|---|---|---|
| Contract and source | B1 mixed HLL; B2 direct SUPER/spatial binds; B3 finite supported values and material input limits | Mandatory mixed HLL and direct SUPER/spatial workflows implemented; finite value contracts reviewed; all 21 types retained |
| Tests and local validation | B4 installed SUPER test execution and operation gaps; B5 metadata parity | Finite fixture list and meaningful regressions; independent exact-source review; one serial rebuilt installed C/Python validation |
| Isolated qualification | Consolidated installed campaign using existing harnesses and immutable candidate artifacts | Required operation cells have exact outcomes; original failures preserved; full teardown independently verified |
| Candidate delivery | B6 docs/examples/install checks, exact-head review, scoped PRs and applicable CI, exact-candidate Native Daily and release dry run | Reviewed release-readiness report with source/artifact identities and explicit limitations |
| Task009 publication | Version/changelog/release preparation after all acceptance gates | Separate human approval before any release tag, GitHub Release or PyPI publication |

Workers remain local/offline and do not compete with the coordinator's serial
build. The coordinator owns integration and dedicated live work within existing
4-RPU, USD 5 target per attempt and 60-minute bounds, with full verified teardown.
No shared resources, new auth scope, AO/timers or automatic dispatch are introduced.
This roadmap grants no additional paid attempt or publication authority.

## Delivered foundations

The [private libpq dependency](private-libpq.md) supplies local B1 Redshift
linkage and mixed HLL result dispatch. Installed/native qualification remains
pending. Portable dependency/provenance wiring is local and unqualified; Windows
still fails closed. Windows/cross and all seven C/five wheel validations remain B6 gates.

Queries, core metadata, transactions, cancellation and prepared INSERT ingestion
are implemented. Packaging supports seven archive targets and five portable
wheels, with clean install/import and PostgreSQL-rejection checks. Those checks
are not all-platform live Redshift qualification. Task006's thin caller-owned
boto3 IAM helper and Task007 user guide are merged. The Linux installed C/wheel
Serverless temporary-IAM/verify-full TLS path passed
[run 39138655470](https://github.com/vahid110/arrow-adbc/actions/runs/39138655470);
provisioned-cluster helpers remain offline-qualified only.

This is an open-source independent Redshift ADBC delivery route under the
existing licenses and notices. Do not claim first-to-market or unbenchmarked
speed superiority. ODBCPP remains the separate ODBC product; no duplicate ADBC
adapter or new commercial-demand claim follows from this work.

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
