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
  validation steps. Native interval target constraint producers remain unqualified
  and actual-target writes fail closed. Task010e3 adds an explicit fixed-offset
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
  Task010h1 adds the noninstalled closed 715-step async runner and eight bounded
  output descriptors; exact review and root serial validation are pending.
  Broader runtime closure and the local execution/live handoff remain
  separate prerequisites. Synthetic/local PostgreSQL
  evidence does not qualify Redshift; platform CI follows the local-first workflow.
  Native temporal wire/domain/precision and character/binary boundaries, TIMETZ native layout/domain/offset/precision,
  interval native wire/domain/catalog/qualifier qualification, SUPER,
  HLL and spatial work remain
  required before Task009. No paid operation is performed or authorized here.
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
