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

# Status


Task010t interval edges passed all four installed Python/C array and stream legs
(234 cases), with resource closure verified. See [the bounded evidence](#task010t-interval-edge-qualification).

Task010s bounded interval ingestion qualification passed all four installed
Python/C Bind and BindStream legs on macOS arm64, with verified full teardown. See
[the source-specific results](#task010s-interval-ingestion-qualification).

Task010q/q1 implements native SUPER observable JSON-wire reads at
`26fb72348a6dee794d86d47b70d66d487f9894a0`, following independent source review
and rebuilt installed-artifact validation. Isolated live qualification on macOS
arm64 completed on 2026-10-22 against that source:

| Installed artifact route | Result | Scope |
|---|---|---|
| Python, Arrow temporal mode | 76/76 passed | Native SUPER, decimal precision, temporal/TIMETZ, interval qualifiers, large values and mixed results |
| Python, lossless temporal mode | 11/11 passed | Exact native temporal counts, TIMETZ and wide timestamp reads |
| Python writes | 60/60 passed | 48 positive decimal/temporal/TIMETZ create/stream/append cases; 12 unsupported interval-target rejection and retention cases |
| Standalone C, Arrow temporal mode | 22/23 passed | Original `super.native` fixture length mismatch retained; remaining cases passed |
| Standalone C, interval reads | 37/37 passed | Fixed qualifier/precision columns and exact Arrow schemas/buffers |
| Standalone C, lossless temporal mode | 24/24 passed | Corrected SUPER fixture, decimal/temporal/TIMETZ, large and mixed reads |

The C SUPER fixture now constructs JSON backslashes explicitly with `CHR(92)`;
its unchanged expected bytes passed through the same native SUPER decoder in the
lossless C leg. The corrected Arrow-mode C case was **not rerun**: its original
failure is not relabeled as a pass. The earlier Python lossless attempt stopped
before native cases at the strict installed-wheel cleanliness gate; the clean
successor passed all 11 cases. Every isolated attempt completed verified full
teardown, including all seven owned resource classes and independent database
cleanup checks. Actual billing is not reconciled.

Native SUPER preserves received numeric spellings and wire NULLs in a separate
output-only schema. The retained protocol collapses SQL NULL and parsed JSON
null; input-lexeme/native-subtype losslessness is not claimed. Those Task010q results do
not claim positive interval ingestion, complete domains, every metadata API or
cross-platform live qualification. Direct SUPER/spatial binds and mixed HLL
output remain unsupported. Vendor REPLACE remains unsupported. Task010s adds bounded actual interval-target
writes; unknown target constraints still fail closed. Final exact-head review, push and applicable
green CI remain before integration; no release-ready claim is made.

Current state only; history is in git and PR descriptions.

## Active

- Done: 000 (PR #1), 001 (PR #2), 003 (PR #3), 004 (PR #4),
  004b (release hardening, PR #8), 005 (Python package, PR #7),
  005b (portable wheels, PR #10), 006 (offline IAM recipe/helper, PR #11),
  007 (user documentation, PR #12), 008a (offline isolation contract, PR #13).
- Task006 IAM helper and Task008a/b/c/d (PRs #11, #13–#20) are merged.
  Installed C archive and Linux x86-64 wheel temporary-IAM qualification passed
  [run 39138655470](https://github.com/vahid110/arrow-adbc/actions/runs/39138655470)
  on `3460c541914560a91872e4417c4c3b4ccbb81df0`: query, parameters, int64
  metadata/create+append/roundtrip and Python invalid-replace preservation.
  Independent full physical/managed-secret/IAM teardown is verified. The helper's
  original namespace-removal uncertainty and all earlier failures remain separate
  immutable evidence; operation parameters/guard are retained. Actual billing is
  not reconciled. See [the finite host contract](qualification-live.md) and
  [merged PR20](https://github.com/vahid110/arrow-adbc/pull/20).
- Task010 is now the first-release priority: **all 21 Redshift table types**, not
  SUPER alone. [The source audit/type contract](REDSHIFT_TYPES.md) separates native
  read, bind, create, append and metadata evidence. The passed live batch proves
  its narrow int64 scope, not all-type support or release readiness. Task010a
  source audit/docs is merged (PR #21). Task010b adds distinct catalog-driven
  interval read/schema/family metadata, strict reference-format codecs, C/Python
  C Data tests and unsupported-write guards (PR #22 merged; offline CI passed).
  Task010c1 adds per-field raw modifiers and bounded numeric/character metadata
  across query/schema/catalog/parameter paths (PR23 merged; offline CI passed).
  Task010c2 adds descriptor-driven decimal128 reads or exact UTF-8 for unknown
  modifiers, with strict finite base-10000 reference decoding and codec-produced
  C Data/PyArrow tests (PR24 merged; offline CI passed). Task010c3 implements
  explicit decimal DDL, bounded exact binding and target-aware append, including
  DECIMAL-marked text and owned rollback uncertainty (locally integrated at d04f2d2;
  independent review and serial native/installed tests passed). Task010c4 adds
  checked integer/float/boolean binding, canonical DDL/metadata and target-aware
  append (locally integrated b90664a; independent review and serial native/installed
  validation passed). Task010d adds checked character/binary identity, widths, explicit DDL and
  target-aware append (locally integrated c07f811; independent review and all 20
  serial native/installed validation steps passed), retaining a provisional
  VARBYTE plain-route size gate. Task010e1 implements four temporal core families
  with immutable per-connection Arrow/lossless output contexts, exact binding and
  target-aware ingest (locally integrated 7fc46fa; independent FULL review and all 22
  validation steps passed). Task010e2 adds closed interval qualifier/precision
  schemas, exact reference reads/writes/DDL and bounded raw target observations;
  locally integrated 35584f5 after independent FULL approval and all 24 serial
  validation steps. Task010s supplies the bounded native target mapping and
  four-leg write evidence [below](#task010s-interval-ingestion-qualification).
  Task010e3 adds an explicit fixed-offset
  TIMETZ struct, checked reference reads/writes, source/target precision and
  bare declaration handling (locally integrated e2df066; independent FULL approval
  and all 25 serial validation steps passed). Task010f0 adds a separate noninstalled
  bounded protocol observer/fixed plans (locally integrated 65e7e9b; independent
  FULL approval and all 30 serial validation steps passed). Task010g0 adds a pure
  anchored historical local-evidence/package-byte linker (locally integrated
  1f35366 after independent FULL approval, actual historical receipt linkage and
  retained 30-step validation). Task010g1 adds trusted in-place install comparisons
  and a fixed same-host load-only observation (locally integrated 0285eba after
  exact independent approval and all 30 serial validation steps). Its named G0
  artifact load observed 328 images: 28 non-OS and 300 OS; collector source is G1.
  Task010g2 adds a bounded output fd for the existing seven-probe own-PG test
  (locally integrated cfe9feb after actual seven-record/stale-fd/cleanup checks,
  all 31 serial validation steps and exact independent approval). Task010g4 adds
  a noninstalled owned single replay parameter with distinct observed and
  controlled NULL/empty origins (locally integrated def7084; independent approval,
  all 31 serial validation steps and ten own-PG echoes/15 calls). Task010h0 adds
  closed compiled native discovery plans/capture shapes (locally integrated 4bdc23b;
  independent approval, all 31 serial stages and nine own-PG checks/15 calls).
  Task010h1's noninstalled closed 715-step async runner and eight bounded output
  descriptors were locally integrated at `3c956e3` after exact independent approval,
  all 32 serial stages and controlled own-PG checks. Later private native runs
  retain protocol observations and failures separately from installed-driver
  acceptance. Synthetic/local PostgreSQL evidence does not qualify Redshift;
  platform CI follows the local-first workflow. Full temporal/domain/precision,
  remaining character/binary boundaries and interval native-domain qualification remain
  open before Task009. The bounded native SUPER and interval read results above
  do not imply input-subtype losslessness or full interval-domain support. No paid operation is performed or authorized here.
  AO/timer and Task009 publication remain paused.
- External host provisioning access is verified through 2027-12-31T23:00:00Z;
  earlier scoped denials remain historical evidence. Workers/reviewers stay
  offline; only the coordinator operates the authorized dedicated host path.
- AO/timer and automatic daily dispatch remain paused. The coordinator may
  manually dispatch Native Daily; this update changes no workflow enablement.

## Known issues

- Historical shared-target live attempt could not assume `adbc-redshift-ci`
  (run 38563931739). This failure is retained as evidence, not a current role
  repair instruction.
  This does not authorize shared-resource reuse. The isolated installed-wheel batch now exercises the equivalent
  128-byte/case-collision replace rejection and unchanged schema/row assertions
  inside its owned fixture; the original named/global gtest is not dispatched.
  The isolated equivalent passed in run 39138655470; this does not claim the
  original named/global gtest ran.
- Original [PR19 cancelled CI evidence](https://github.com/vahid110/arrow-adbc/actions/runs/38923832691)
  and [PR20 review failures/repairs](https://github.com/vahid110/arrow-adbc/pull/20)
  remain retained alongside the failed live attempts below.
- `Integration` and other unrelated Apache workflows are disabled
  in this fork.

## Historical live Redshift log

Workgroup `odbcpp-redshift-pilot` (eu-north-1, account 385207570137), 4 RPU,
shared with ODBCPP at the time; protected from current ADBC access.

| Date | Run | Head | Purpose | Result | Est. cost (USD) |
|---|---|---|---|---|---|
| 2026-10-14 | [38563931739](https://github.com/vahid110/arrow-adbc/actions/runs/38563931739) | `fc6f13029` | post-#2 live batch | failed: OIDC `AssumeRoleWithWebIdentity` denied before ingress; no live tests ran | 0.00 |

## Isolated preflight log

The first two failures created no dedicated resources. Run `38916730017` created
only the owned namespace/SG before workgroup validation failed. Fresh independent
host cleanup verified workgroup, namespace, SG, managed secret, admission and
claim absent; CI is terminal cancelled. No usage-limit attempt, runtime policy,
credentials or SQL occurred. No workgroup compute started; ancillary AWS charges
are not reconciled here. All original runs and host receipts are retained.
Three failed revisions were already escalated to the human/coordinator; this
additional failure preserves the counters/budgets and grants no retry authority.
Run `38920727258` created workgroup/namespace/SG/usage but issued no admission,
claim or installed-client batch. Its original receipt preserves UNKNOWN SQL/
namespace cleanup; the actual cleanup statement FAILED with permission denied
for `stv_sessions`, not zero counts. Separate fresh host readbacks verified all
dedicated resources, usage limit, managed secret, admission and nonce claims
absent at 15:12:38Z, then exact CI-role/initial-policy removal at 15:13:57Z. CI is
completed/cancelled; runtime policy was never installed. Physical teardown is
resolved independently; the original SQL failure and primary-stage uncertainty
remain. Actual compute costs await reconciliation; no further attempt is authorized.

| Date | Run | Head | Result | Est. incremental cost (USD) |
|---|---|---|---|---|
| 2026-10-16 | [38908550615](https://github.com/vahid110/arrow-adbc/actions/runs/38908550615) | `596aec4a23fd3c6fd134f5e569b902cd3134d28f` | failed before OIDC: load-only projection required an absent execution window; offline reproduction and successor repair | 0.00 |
| 2026-10-16 | [38912745155](https://github.com/vahid110/arrow-adbc/actions/runs/38912745155) | `9f3bbaa770386b165f8e0cc9e1de4a4cdae9109c` | installed preflight passed (artifact `12051211152`); OIDC denied because dedicated trust used a legacy subject instead of authenticated immutable owner/repository IDs | 0.00 |
| 2026-10-16 | [38916730017](https://github.com/vahid110/arrow-adbc/actions/runs/38916730017) | `5e3afc8653e39eaf099c5cb91ea9be21b386adb6` | preflight/STS passed; `CreateWorkgroup` rejected simultaneous pricing target and scaling autonomics fields; owned partial resources independently removed, no admission/claim/SQL | 0.00 compute |
| 2026-10-16 | [38920727258](https://github.com/vahid110/arrow-adbc/actions/runs/38920727258) | `36b786d1c569c0f0c727c70dcc06995d0b31a173` | created dedicated resources; post-usage gate failed with original response status unrecorded; cleanup SQL FAILED on STV access and cancellation masked error; independent physical teardown later verified, no admission/claim/client batch | pending |
| 2026-10-16 | [38933278088](https://github.com/vahid110/arrow-adbc/actions/runs/38933278088) | `44bc188350125045e42a8b65e1fc1dc7820e3683` | installed preflight/OIDC/bootstrap/SYS proof passed; exact claim PutParameter denied before driver payload; claim absent, supported post-failure observer zero; independent full physical/secret/IAM teardown verified, original namespace uncertainty retained | pending |
| 2026-10-18 | [39138655470](https://github.com/vahid110/arrow-adbc/actions/runs/39138655470) | `3460c541914560a91872e4417c4c3b4ccbb81df0` | installed C/Linux-wheel temporary-IAM query/params/int64 metadata/create+append passed; independent full physical/secret/IAM teardown verified separately from original helper namespace uncertainty; all-types qualification remains pending | not reconciled |

## Task010r character/binary qualification

Installed macOS arm64 reads on `d5b39f4` passed Python small 61 cases, C small
16 cases and two maximum-read cases per language: 16,000,000-byte VARCHAR and
16,777,216-byte VARBYTE. All four legs have independently verified evidence and
full physical teardown. Python also exercised character/binary generated writes,
width/family rejection and retention; this is not a standalone C character-write
cross-product or a general streaming-atomicity guarantee.

Corrected source `3be87d3` passed 18 installed VARBYTE Bind/BindStream cases in
each of four routes: Python generated ingestion (`66b794`), C generated ingestion
(`517d`), Python direct prepared parameters (`e771`) and C direct prepared
parameters (`cdee`). Original controllers report completion and all seven owned
resource classes removed for each leg. Source review and serial rebuilt offline
validation cover the correction; the live read evidence remains on `d5b39f4`.
Test-only successor `50f9f8643` adds adjacent unsupported reader-boundary coverage.
Final exact-head review and applicable green remote CI remain gates.

The parameter guard is 16 MiB; marked PostgreSQL COPY retains its 1,000,000-byte
guard. Earlier installed rejections above 1,000,000 bytes preserve the superseded
policy evidence. Native plain INSERT accepted exactly 16 MiB, but the raw
16 MiB+1 attempt stopped on unsafe `XX000`: it is neither a classified server
size rejection nor a completed native-domain suite. Local oversize rejection is
separate. Direct prepared SQL does not discover arbitrary destination widths.
These bounded macOS arm64 results do not qualify every type, boundary or platform.

## Task010s interval ingestion qualification

Source `218b59859f7cc943ea7f2b0b6f43e8e32b78ee95` adds conservative internal
ingestion-target decoding for 37 observed family/raw-modifier/`format_type`
combinations: all 13 qualifiers, with precision 0..6 on SECOND-ending forms.
Ordinary query and GetTableSchema metadata remain `unknown`; unsupported or
inconsistent target observations still reject.

| Installed macOS arm64 leg | Result | Operation |
|---|---|---|
| Python Bind array | 68/68 passed; full teardown independently verified | `43d976c2ffa34a17969fbbbcb87290f8` |
| Standalone C Bind array | 68/68 passed; full teardown independently verified | `187df65251044bc89b17972c3e796d5b` |
| Python BindStream | 68/68 passed; full teardown independently verified | `3a4098e58c8a466995a14ce54ddbb1e3` |
| Standalone C BindStream | 68/68 passed; full teardown independently verified | `7e89616b451d40f9808489948034adeb` |

All four legs passed: 272 successful case executions in total. Each leg covers
37 exact CREATE/APPEND pairs and 31 first-invalid target-quantum rejections with
retained rows. Independent catalog/SVV/binary
observations bind literal expected values to runtime-discovered type OIDs and
receive functions. Original controller results and independent audits verify
child/handle closure, zero owned schemas and active runtime sessions, and absence
of all seven owned resource classes. Evidence is retained under
`task010s-interval-20261024/{python-array-43d,python-stream-3a40,c-array-187d,c-stream-7e89}-final-independent-v1`.

The original Python array attempt `c7eae61057e14971b86257ca54ee9f70` qualified no
case: the harness rejected a native-witness record after the first ingest and lost
that witness. The retained startup exposed a signed system-column descriptor;
the corrected harness accepts its diagnostic shape and preserves failed witness
records. The original schema-residue/observer failure remains recorded separately
from verified physical teardown. Earlier unsupported-target rejection receipts
and Task010r's unsafe native size-boundary failure remain historical evidence.

This is bounded macOS arm64 evidence, not native extrema/sentinel qualification,
omitted-default proof, arbitrary prepared-SQL target discovery or a general
streaming-atomicity guarantee. Final exact-head review and applicable green remote
CI remain gates; actual billing is not reconciled.

## Task010t interval edge qualification

Source `77da4599ca79f86a0afcae9cfe66a42da9b0f503` adds four observed
omitted-precision ingestion-target tuples (SECOND, DAY TO SECOND, HOUR TO SECOND
and MINUTE TO SECOND), each with precision 6. Unrecognized/conflicting targets
still reject; public query, empty-result and GetTableSchema constraints remain unknown.

| Installed macOS arm64 leg | Result | Operation |
|---|---|---|
| Python Bind array | 57/57 passed; full teardown independently verified | `1ab44b8fddf74a2dab3199a9e9f1b66b` |
| Python BindStream | 60/60 passed; full teardown independently verified | `886d0323c1f1488696f8710e72b807c9` |
| Standalone C array | 57/57 passed; full teardown independently verified | `af4f27a1ae734794b55d6b0dc3923875` |
| Standalone C stream | 60/60 passed; resource closure separately reconciled | `61bd8de8b627438a9458cd7ed5affc59` |

These 234 case executions cover selected signed extrema and conversion thresholds,
negative/inexact rejection, omitted defaults and retained rows. Stream cases also
check late conversion failure under driver-owned and caller-owned transactions;
the observed retained rows do not establish general streaming atomicity.
Native discovery separately retained 114 observations: 16 exact binary echoes
supported exploratory installed tests; four default columns preserved stored
+1/-1 microsecond. Four infinity text inputs rejected with SQLSTATE 22007. Outside-range text
parameters returned the following altered values without SQLSTATE:

| Text interval input | Observed binary result |
|---|---|
| -2147483649 months | 2147483647 months |
| 2147483648 months | -2147483648 months |
| -9223372036854.775809 seconds | -2077252342775809 microseconds |
| 9223372036854.775808 seconds | 2077252342775808 microseconds |

For these samples, month results match signed 32-bit wrap; second results match
signed 32-bit wrap of whole seconds with the six fractional digits preserved.
DAY TO SECOND text and SQL literals also altered the signed 64-bit microsecond
endpoint samples, whereas binary parameters and installed stored readbacks were
exact. These observations do not establish a general parser algorithm.
Successful installed
CREATE/APPEND/readback proves only the tested stored samples, not a contiguous
finite domain, sentinel interpretation or arithmetic behavior.

Evidence under `task010t-interval-edges-20261026`:
`native-complete-oracle-independent-v1`, `python-array-live-independent-v1`,
`python-stream-budget-live-independent-v1`, `c-array-accounting-live-independent-v1`
and `c-stream-accounting-live-independent-v1`; exact retained overflow comparisons
are in `native-overflow-dispositions-v1`. All four installed audits verify closed
handles and zero owned schemas/runtime sessions. Three controllers verified all
seven resource classes absent; the final C stream controller timed out waiting
for security-group cleanup after all 60 cases passed. Separate exact-group cleanup
and absence verification are audited in `c-stream-final-group-cleanup-independent-v1`;
its original teardown failure remains preserved.

Earlier C provisioning and Python bootstrap failures qualified no case. A C harness
accounting-cap failure completed only 39 prefix cases and did not qualify that leg;
physical teardown succeeded, with the pre-deletion schema residue recorded separately.
Original failures and cleanup evidence remain preserved. Final exact-head review
and applicable CI remain completion gates; actual billing is not reconciled.
