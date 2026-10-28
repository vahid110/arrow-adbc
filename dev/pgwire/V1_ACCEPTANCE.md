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

# V1 acceptance at merged PR29

Audit baseline: `fca2eab2e05e1a63cfbd9adecc802c53334bdbd0`, tree
`0fa498a6e88c30eb1758195c614755a9942c51af`, 2026-10-28.
The [21-type operation matrix](REDSHIFT_TYPES.md#current-evidence-by-operation)
is the coverage ledger. This is a finite remaining-work list, not release approval.
Earlier successful suites stay qualified only for their recorded source and scope.
On 2026-10-28 the human explicitly required the missing operations before v1:
ordinary mixed HLL results and direct SUPER/spatial parameters are mandatory.
There is no ship-with-these-operations-missing alternative in this plan.

## Release blockers and ownership

| Gate / priority / class | User impact and existing evidence | Smallest remaining work and completion criterion | Owner / phase |
|---|---|---|---|
| B1 / P0 / functional limitation | Selecting HLL alongside an ID or any other type rejects before execution. M/Q explicitly prove that rejection; all-HLL reads alone do not satisfy ordinary rows. | Implement and qualify mixed HLL rows without user casts, silent coercion, duplicate execution or arbitrary SQL rewriting. Exercise HLL+scalar+vendor, NULL, empty/schema-only and multiple batches through installed C/Python. A partial HLL+integer-only mode is insufficient: mixed consumption must cover the released type contract, both temporal modes and execute effectful SQL once. This mandatory gate cannot close through documentation. | Vendor implementation, then qualification |
| B2 / P0 / mandatory functional gap | Direct semantic SUPER/GEOMETRY/GEOGRAPHY parameters reject. Generated constructors work within bounded input contracts, but application prepared SQL is a different workflow. | Establish actual server parameter transport, implement a bounded validated direct path and test array/stream, NULL/empty and mismatched prepared types; retain rejection outside it. All three direct families are mandatory before v1. Never remove guards based only on generated-ingest success. | Vendor investigation/implementation, then qualification |
| B3 / P1 / finite contract and material limit decisions | Current SUPER input is JSON subset <=1,000,000 bytes/depth 128; native output <=16 MiB/depth 1000 loses server-discarded subtype/NULL distinctions. Spatial writes cap EWKB at 500,000 bytes while reads cap at 1,048,447; HLL has a 1,000,000-byte serialized lexical cap. These restrict ordinary server types despite successful M/Q samples. | Review and publish explicit supported values and rejection behavior for each family. Resolve whether input size/subtype restrictions meet the all-type goal; larger ordinary workflows need implementation or human agreement. Test sparse/dense HLL including transition; seven spatial shapes, dimensions/SRIDs/endian/empty values; SUPER mixed/nested/large values and wire NULL. Test limits/adjacent rejection in existing local seams and selected live native boundary fixtures, without demanding every mathematical value. | Contract/source review before qualification |
| B4 / P1 / installed evidence gaps | Matrix C cells have code but no matching installed proof; M/Q/R prove only selected operations. C Arrow SUPER remains 22/23. The stale public SUPER rejection assertion was repaired locally in coordinator commit `58fb0528aab53e154673101ca55fdd846715cf32`, with offline validation and independent review; native installed execution is still pending. | Execute the repaired public SUPER test against installed Redshift artifacts and run the corrected C Arrow fixture. Fill required C/Python direct-bind/create/append gaps, especially primitive scalars, C decimal/temporal/character writes, HLL/spatial APPEND and direct interval binds. Cover the finite family cases below and check safe failure/retained rows. Every required cell must point to exact passing installed output within the reviewed finite B3 contract. | Test implementation, serial installed build, consolidated live campaign |
| B5 / P1 / metadata evidence gap | Query schema evidence does not establish ExecuteSchema, parameter schema, GetTableSchema and GetObjects for every family/alias. Interval public qualifier/precision remains unknown even when ingestion target decoding knows an observed tuple. | One fixed all-type catalog fixture: compare identity/storage and available p/s/width/qualifier across these APIs, bound/unbound query, empty results and computed expressions. Assert unknowns stay unknown and OID-only parameters never invent modifiers. Add/fix only observed contract discrepancies; do not expose target guesses as public truth. | Metadata tests/source fixes, then installed qualification |
| B6 / P1 / release candidate delivery | Older merges/CI and macOS live cases do not validate new candidate bytes, packaging or every supported platform. | Independent review of exact final head/tree; one serial rebuilt installed validation; green applicable PR gates, exact-candidate Native Daily, release dry run, seven archive/five wheel checksums/install/import/manifest checks and documented examples. Merge scoped PRs and issue a reviewed readiness report. Publication needs a separate human decision. | Coordinator integration/release readiness |

B1/B2 are existing functional gaps, not newly reproduced regressions. B4 retains
the native execution gate after local repair of the SUPER verification defect.
No type may be dropped from the 21-row ledger.
B1/B2 remain mandatory. A rejected operation alone is not evidence that an
important workflow is acceptable; B3 cannot silently narrow these requirements.

## Finite qualification cases

Use the existing C/public-API and Python installed harnesses; do not build another
controller framework. Reuse retained fixtures where source/behavior still matches.
Each family needs NULL, empty-result/all-null, a normal value, mixed rows and stable
schema. Each supported write route needs array and stream input, CREATE then
APPEND/readback, a first-invalid conversion with retained data, and ownership-aware
failure checks. This does not promise general streaming atomicity or atomic REPLACE.

| Family | Additional bounded cases to map to retained or new evidence |
|---|---|
| Integers/boolean | Min/max of each integer width, unsigned overflow; true/false/NULL; exact destination-family checks |
| Decimal | p1/18/19/20/38, s0/2/37; precision 19 signed 64-bit coefficient boundary, negative/zero, exact rescale/inexact reject, decimal256/marked text and unknown expression descriptor |
| Float | +/-0, finite extrema/subnormal where retained, NaN/+/-Infinity; numeric semantics without NaN-payload promises |
| Character/VARBYTE | Alias/default identity, padding/UTF8/quotes/backslashes/NUL/empty, stored widths and first invalid; retained 16,000,000-byte VARCHAR/16MiB VARBYTE reads and four-route VARBYTE writes reused |
| DATE/time/timestamps/TIMETZ | BC/native epoch, microseconds, 24:00 policy, checked Arrow/lossless overflow, timezone offset/session restoration, known/unknown precision, explicit p0..5 CREATE restriction/native syntax, own-TEMP/case behavior and explicit infinity/sentinel disposition |
| Intervals | Reuse S/T 13 qualifiers, precision 0..6, omitted-default tuples, selected signed extrema/inexact rejection and late stream failure. Add direct public prepared binding proof; do not infer a general text parser or contiguous native domain from samples |
| SUPER/HLL/spatial | B1–B3 routes and limits; exact representation/SQL NULL semantics, IPC/import, ordinary mixed rows and source-specific shape/serialization witnesses |

A finite contract with exact boundary tests and conservative rejection can close
acceptance; exhaustive mathematical-domain testing is not required. Unsupported
server values that are material to the promised workflows still require B3's
explicit resolution. Do not relabel unsafe native XX000 as expected size rejection.

## Executable release checklist

- [ ] Freeze B1–B5 implementations and finite case IDs; record any human scope
  decisions alongside the matrix. Verify no required operation remains C/U without
  an open gate; mandatory gates must close before release.
- [ ] Workers run relevant small regressions and
  `SKIP=dotnet-format ~/.venvs/adbc-lint/bin/pre-commit run --files <changed-files>`;
  independent reviewer records exact head/tree approval before local integration.
- [ ] Coordinator builds one immutable source serially, installs clean C and Python
  artifacts, records source/artifact hashes and executes native/public API/C Data
  regressions plus the corrected vendor test. No worker full build.
- [ ] Bind the finite installed case list to those artifacts; coordinator runs the
  authorized isolated campaign within the dedicated 4-RPU, USD 5 target/attempt and
  60-minute bounds, with full verified teardown. Retain original failures and any
  separate cleanup receipts; do not renew spent receipts or use shared resources.
- [ ] Update all 21 operation cells from actual outputs, including metadata APIs,
  failures/rejections, platform and source. Independently review final code/docs
  and resulting readiness evidence; no unavailable evidence becomes a pass.
- [ ] Coordinator opens/merges scoped fork PRs after applicable green gates in
  [CLAUDE.md](../../CLAUDE.md). Then require `pgwire-native-daily.yml` at the exact
  candidate head and `pgwire-release.yml` with `dry_run=true`; verify resulting
  seven archives, five wheels, checksums/manifests/versions, clean install/import
  and examples. Native Daily includes the Sphinx documentation build.
- [ ] Deliver a reviewed readiness report with supported operations, limitations,
  exact source/artifact identities and any remaining blockers. Keep release tags,
  GitHub Release publication and PyPI publication awaiting separate human approval.

## Retained evidence index

Local evidence root (not distributed with this repository):
`~/.local/state/adbc-redshift/workflow/type-coverage-20261018/`.
Paths below are relative to that root. A receipt's bound result/journal is the
primary outcome; source readiness reviews alone never prove a live pass.

| Key | Exact retained entry point |
|---|---|
| L | [run 39138655470](https://github.com/vahid110/arrow-adbc/actions/runs/39138655470), source 3460c541914560a91872e4417c4c3b4ccbb81df0 |
| M | `live-type-fixes-20261021/installed726f-recorded-independent-v1/receipt.json`, operation 726f4b3594e048e29f4571b2770549c3, source a239f66cc29565e5625b6660ae2aed93612f17af |
| Q Python Arrow | Final row of `native-operation-claims/48a425e2e92046dda6a76a509fa464bc-run/parent.jsonl`: 76 passed, source 26fb72348a6dee794d86d47b70d66d487f9894a0, complete teardown |
| Q Python lossless/writes | `live-type-fixes-20261021/installed-python-lossless-recorded-independent-v1/receipt.json` (6f23207ac3664464b79a8eeaa8b2f5e8,11); `installed-write60-recorded-independent-v1/receipt.json` in the same directory (6e7e1df7dd7c40f485e997d9a3620346,60) |
| Q standalone C | `live-type-fixes-20261021/installed-c-{arrow_core,arrow_intervals,lossless}-recorded-independent-v1/receipt.json`: operations 5647ee25f9a94467b0948b93ad4c76d8,649c382e334c4a1884efbf33f694ee7f,be12a89bad4349b6ad03e96063d90f77; 22/23, 37/37, 24/24 |
| R reads | `task010r-character-binary-20261022/{python-small-2b2f,c-small-0808,python-large-8c0c,c-large-fb7a}-final-independent-v1/receipt.json` |
| R corrected writes | `task010r-character-binary-20261022/python-generated-66b794-independent-v1/receipt.json` and `{c-ingest-517d,python-direct-e771,c-prepared-cdee}-final-independent-v1/receipt.json` in the same directory |
| S | `task010s-interval-20261024/{python-array-43d,python-stream-3a40,c-array-187d,c-stream-7e89}-final-independent-v1/receipt.json` |
| T | `task010t-interval-edges-20261026/goal-completion-v1/receipt.json` links all four installed audits, native oracle/overflow dispositions, original C-stream timeout and separate exact-group cleanup; 234 cases, PR29 merged and latest applicable CI green |

Receipts preserve historical source/platform differences and original failures.
PR29 review head 9ac6f35148dd473da09ad37729b10b20081e0014 has the identical merged
tree, with applicable CI recorded in T's `runs.json`; this closes that task's
review/merge gates, not the future release candidate's B6 gates. Actual billing
remains unreconciled and is not represented as a verified USD amount.
