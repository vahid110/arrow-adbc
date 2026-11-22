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

# V1 acceptance: current evidence and remaining gates

Current merged baseline: `c383d6146facd0c9f74428233c5db2040c174d29`
(PR #40). Evidence reconciled 2026-11-22; historical sources remain identified.
The [21-type operation matrix](REDSHIFT_TYPES.md#current-evidence-by-operation)
is the coverage ledger. This is a finite remaining-work list, not release approval.
Earlier successful suites stay qualified only for their recorded source and scope.
PR #30 integrated `aed6ced40fb058f56dc8b097937776c05cbe9496`, with independent
source review, 35 passing serial local validation steps and passing Meson targets;
V adds 15 selected installed cases each in standalone C and Python, with
independently reviewed results and full teardown. This does not close every gate below.
On 2026-10-28 the human explicitly required the missing operations before v1:
ordinary mixed HLL results and direct SUPER/spatial parameters are mandatory.
There is no ship-with-these-operations-missing alternative in this plan.

## Release blockers and ownership

| Gate / priority / class | User impact and existing evidence | Smallest remaining work and completion criterion | Owner / phase |
|---|---|---|---|
| B1 / P0 / functional limitation | Earlier M/Q runs recorded a mixed-column rejection. The issue #31 candidate passed installed mixed reads in standalone C and Python in both temporal modes (case H). | Implement and qualify mixed HLL rows without user casts, silent coercion, duplicate execution or arbitrary SQL rewriting. Exercise HLL+scalar+vendor, NULL, empty/schema-only and multiple batches through installed C/Python. A partial HLL+integer-only mode is insufficient: mixed consumption must cover the released type contract, both temporal modes and execute effectful SQL once. This mandatory gate cannot close through documentation. | Complete: PR #32 merged after exact-head independent review and applicable green CI |
| B2 / P0 / mandatory operation qualification | PR #30 implements direct SUPER/GEOMETRY/GEOGRAPHY parameters. V passed selected C/Python array/stream, NULL/empty, repeated-placeholder, adjacent-parameter and exactly-once effects cases, including malformed-first rejection with retained rows. Historical PR29 rejected these direct families. | V retains positive direct-bind proof. VB adds installed metadata contradictions and incompatible SQL targets; offline recorder tests separately inject descriptor drift. Generated-ingest success alone does not qualify direct binding. | Qualified within V/VB scopes; final candidate delivery remains B6 |
| B3 / P1 / finite contract and material limit decisions | Local code now accepts SUPER JSON input <=16MiB/depth1000 with strict UTF8/duplicate validation; native output uses received field length and checked append, preserving only observable JSON/wire NULL. Spatial read/write raw EWKB limits both equal1,048,447 with byte-bounded iterative collection frames. HLL retains its1,000,000-byte known-format serialized cap. Stored object and serialized wire sizes are distinct. | Source review, serial local validation and Meson targets passed. V adds selected installed large/deep SUPER, spatial fixtures and native SUPER output beyond16MiB. Broader maximum-value coverage and finite sparse-to-dense HLL transition fixtures remain; the sparse/dense scalar cardinality controls do not prove the transition boundary. Selected cases do not prove every maximum. No mandatory operation or material value restriction is waived. | Installed qualification; HLL transitions separate |
| B4 / P1 / installed evidence gaps | SC closes selected installed read/direct/create/append/metadata gaps for five scalar families in both C and Python. Remaining matrix C cells lack matching installed proof; M/Q/R retain their selected scopes. VA separately passes the corrected C Arrow SUPER fixture; the original Q 22/23 remains preserved. VR passes the repaired public SUPER native-read/IPC method and an all-mode TEMP/permanent-shadow regression through fresh installed artifacts. | VA/VR close selected vendor APPEND and direct HLL routes plus public SUPER execution. TW fills selected direct decimal/temporal/CHAR/VARCHAR bindings in both languages and C CREATE/APPEND. DI fills selected direct interval binds with first-invalid conversion/retention controls. Fill remaining scalar boundary/rejection cases. Cover the finite family cases below and check safe failure/retained rows. Every required cell must point to exact passing installed output within the reviewed finite B3 contract. | Test implementation, serial installed build, consolidated live campaign |
| B5 / P1 / metadata evidence gap | SC covers selected APIs for five canonical scalar families; VA adds selected vendor metadata routes. It does not establish ExecuteSchema, parameter schema, GetTableSchema and GetObjects for every family/alias or attribute. Interval public qualifier/precision remains unknown even when ingestion target decoding knows an observed tuple. | One fixed all-type catalog fixture: compare identity/storage and available p/s/width/qualifier across these APIs, bound/unbound query, empty results and computed expressions. Assert unknowns stay unknown and OID-only parameters never invent modifiers. Add/fix only observed contract discrepancies; do not expose target guesses as public truth. | Metadata tests/source fixes, then installed qualification |
| B6 / P1 / release candidate delivery | Older merges/CI and macOS live cases do not validate new candidate bytes, packaging or every supported platform. | Independent review of exact final head/tree; one serial rebuilt installed validation; green applicable PR gates, exact-candidate Native Daily, release dry run, seven archive/five wheel checksums/install/import/manifest checks and documented examples. Merge scoped PRs and issue a reviewed readiness report. Publication needs a separate human decision. | Coordinator integration/release readiness |

B1 and B2 have separate installed C/Python evidence in cases H and V/VB.
The B1 run checked both the returned mixed-row sketch and its server-side union. VR closes B4's selected public SUPER execution gate; other operation gaps remain.
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
| Intervals | Reuse S/T 13 qualifiers, precision 0..6, omitted-default tuples, selected signed extrema/inexact rejection and late stream failure. DI adds direct public prepared binding proof for both families in C/Python array/stream; do not infer a general text parser or contiguous native domain from samples |
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

V evidence is under the separate local root
`~/.local/state/adbc-redshift/workflow/v1-readiness-20261028/`:
`prepared-vendor-c-parent-v2/controller-result.json`, operation
`80a19cb77c384fa7b80eb48185e55d97`, source
`aed6ced40fb058f56dc8b097937776c05cbe9496`, 15/15 cases and verified teardown.
Its bound output files are under
`~/.local/state/adbc-redshift/workflow/type-coverage-20261018/` at
`native-operation-claims/80a19cb77c384fa7b80eb48185e55d97-run/transport/`.
Python operation `f1b8ef97405848a3b86f757ae0207217` also passed 15/15 cases;
its result is retained at `prepared-vendor-python-parent-v2/controller-result.json`
under the v1-readiness root, with output under its operation directory in
`type-coverage-20261018/native-operation-claims/`. Both operations have verified
full teardown. Independent result reviews under the v1-readiness root are
`resume-20261102/prepared-vendor-live-evidence-review/c-review.json` and
`resume-20261102/prepared-vendor-live-evidence-review/python-review.json`.
The earlier C failure and separate residual cleanup remain at
`prepared-vendor-live-binding-v1/c/attempt-result-with-residual-closure.json`.

Receipts preserve historical source/platform differences and original failures.
PR29 review head 9ac6f35148dd473da09ad37729b10b20081e0014 has the identical merged
tree, with applicable CI recorded in T's `runs.json`; this closes that task's
review/merge gates, not the future release candidate's B6 gates. Actual billing
remains unreconciled and is not represented as a verified USD amount.

SC evidence is retained under `~/.local/state/adbc-redshift/issue33/`:
`attempt-134d349d3d3747db8d92a36553ec94c9/result.json` records four passing
recipe groups per language, both child exits zero and verified SQL session/schema
absence. The attempt's `original-binding.json` and `tested-source-snapshot.json`
preserve the exact tested fixtures, helpers, documentation and installed artifacts;
`independent-live-evidence-review.json` records the independent result review.
Full teardown verified all seven owned cloud resource classes absent; original
failed attempts and their closure records remain preserved.
The installed driver SHA256 is
`1ea57fbd518723404295bcb00b822b43d3e37adc5926d52ec0dbe100d911836d`,
source `38dc469cb534a92aa194b45e0e568c8946e04d2c`. Relevant driver sources match
merged baseline `dae9d97c2`; build-recipe changes and final candidate rebuilding
remain B6 work. Four earlier attempts retain their credential/runner/fixture
failures and individual teardown records. SC does not rerun the historical
corrected C Arrow SUPER fixture or close all B4/B5 cases.

VA evidence is retained under `~/.local/state/adbc-redshift/issue35/`:
`attempt-385cef31d10e4d45aa23680beb4ce3dd/result.json` records 11 passing C and
10 passing Python groups, zero child exits and verified SQL session/schema absence.
Full teardown verified all seven owned cloud resource classes absent, without
cleanup issues. The attempt's `original-binding.json`,
`tested-source-snapshot.json` and original independent reviews preserve all 49
exact tested inputs and provenance. The driver source/hash are the same retained
artifacts as SC; final candidate rebuilding remains B6.
The earlier `6041097aa2c046c9a6eb8d6055d10a49` attempt failed connection startup
before any native cases and has verified full teardown. Its exact original error
message was not captured. The successor's owned-endpoint TCP probe timed out twice
then succeeded; it demonstrates startup reachability delay, not the earlier
attempt's proven root cause. No permissions or network exposure were broadened.

VR evidence is retained under `~/.local/state/adbc-redshift/issue37/`:
`attempt-ab549442ad0641e68e6589e24959805e/result.json` records eight passing
C/Python groups with fresh driver source `7d6825dfcee1161e64b35ed58e1a223d6013b294`.
The original binding, source-object manifest and independent reviews preserve
106 exact tested inputs. SQL schemas/sessions and all seven owned cloud resource
classes were verified absent, with no cleanup issues. Original SQLSTATE 3F000/42P01 failures and the separate
startup timeout are retained. See VR in the operation matrix for bounded scope;
final-candidate packaging and cross-platform checks remain B6.

TW evidence is retained under `~/.local/state/adbc-redshift/issue39/`:
`attempt-35f90edc4ee7442d95ad940e5d89c7a9/result.json` records six passing C and
six passing Python groups, both child exits zero and SQL schema/session absence.
The original binding, source-object manifest and independent review preserve
93 exact tested inputs. Integration baseline `9ef47ac6` has the same driver
source as the reused VR artifacts; the private standalone C fixture is new.
Full teardown verified all seven owned cloud resource classes absent with no
cleanup issues. The first attempt
`3840d8aea83a438da6db1173e2216f0b` passed six Python groups and C decimal array,
then its C decimal stream fixture failed before later C groups ran. One SQL
schema remained until full namespace teardown; all seven cloud resource classes
were then verified absent. Its 89 original inputs and failure remain immutable.
See TW in the operation matrix for scope; this is not full B4/B5 or B6 completion.

DI evidence is retained under `~/.local/state/adbc-redshift/issue41/`:
`attempt-aaf0078cb043467c95dbfa6aa1053d45/result.json` records four passing
C/Python array/stream groups and the two expected rejection receipts in each.
Both child exits are zero; SQL schemas/sessions and all seven owned cloud
resource classes are verified absent with no cleanup issues. The attempt preserves 94 exact tested inputs and its
original binding, artifact provenance and independent source review. No driver
behavior changed from the reused reviewed VR artifacts. Offline review found and
fixed a Python control-stop gap before admission; its original finding/sources
remain preserved. DI does not replace S/T qualifier coverage or close all B4/B5/B6.

VB evidence (issue #42) is retained under `~/.local/state/adbc-redshift/issue42/`:
`attempt-727731fa3d5c4c358c4a9393cb308942/result.json` records four passing
C/Python array/stream groups at source `ae1a10f45f4d08c181075ae4086c1ac0bcf355af`.
The exact repository helper `ci/scripts/pgwire_prepared_boundary_cases.py` ran
against installed Python artifacts; its 12 offline regression tests run in CI.
Native recorder assertions separately retain injected descriptor/OID drift proof.
Across three vendor families, all 12 contradictory-metadata controls rejected
at bind phase55 with INVALID_ARGUMENT and no SQLSTATE. All 12 incompatible-cast
controls rejected at execute phase56 with INVALID_ARGUMENT/42846. Each rejection
was followed by identity and complete retained-row checks; final empty metadata
also passed. These server casts do not simulate prepared descriptor drift.
Both children exited zero. SQL schemas/sessions and all seven isolated resource
classes were verified absent with no cleanup issues. Independent review checked
104 immutable tested inputs and full closure. The installed driver bytes are
unchanged from VR; VB changes reusable tests and CI, not driver behavior.
