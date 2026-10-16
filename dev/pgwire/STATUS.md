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
- Task 006 merged:
  helper `adbc_driver_redshift.iam` with `build_uri` and
  `fetch_{serverless,cluster}_credentials` (injectable clients, offline
  tests), `iam` extra (`boto3>=1.28`), recipe in `c/driver/postgresql/REDSHIFT.md`
  and `python/adbc_driver_redshift/README.md`; offline tests only, live
  IAM check deferred to next authorized batch.
- Task008b (PR #14) is merged: installed C/Python private-input clients,
  bounded process cleanup and no-AWS plumbing checks; the original synthetic/
  unsigned admission gates remain closed.
- Task008c (PR #15) merged the explicit-dispatch SSM admission/one-shot claim path,
  authenticated merged-head artifact provenance, actual installed IAM payloads,
  exact owned ingress cleanup, and a finite host bootstrap/observer/teardown
  helper. See [the host handoff](qualification-live.md). This is offline-tested
  implementation, **not a completed Redshift live qualification**. Dedicated
  resources, exact-head review/green CI, host issuance, one successful batch and
  independent cleanup/secret/IAM teardown observations remain required.
  PR #16 repaired load-only installed evidence separately from the mandatory
  admitted execution window, with real prepare-to-consumer regressions. The next
  dispatch passed installed preflight but failed OIDC: the retained role and host
  validator pinned a legacy subject while this repository issues immutable IDs.
  PR #17 pinned the exact immutable subject; the next run passed preflight/STS
  but `CreateWorkgroup` rejected conflicting scaling/pricing request fields.
  PR #18 repaired that request. Run `38920727258` then created dedicated compute
  but stopped at the post-usage resource gate (exact earlier status unrecorded);
  cleanup SQL failed on unsupported STV access, then cancellation masked that
  failure. The successor adds bounded readiness observation, static failure
  receipts, terminal-aware cancellation and a supported cross-user SYS observer
  before admission and at cleanup. Original receipts remain spent; further host
  authority requires coordinator/human resolution after review.
  PR #19's first head `f10b26670` required independent deadline/fresh-run repairs;
  its [cancelled Dev run](https://github.com/vahid110/arrow-adbc/actions/runs/38923832691)
  remains evidence. The batched successor requires fresh exact-head review/CI;
  no external resources or CI role remain and no live authority is renewed.
  AO stays paused; Task009 publication stays paused.
- External host provisioning access is verified through 2027-12-31T23:00:00Z;
  earlier scoped denials remain historical evidence. Workers/reviewers stay
  offline; only the coordinator operates the authorized dedicated host path.
- Native matrices: `PgWire Native Daily` is dispatched every 24 hours on
  `feature/pgwire-core-redshift`.

## Known issues

- Historical shared-target live attempt could not assume `adbc-redshift-ci`
  (run 38563931739). This failure is retained as evidence, not a current role
  repair instruction.
  This does not authorize shared-resource reuse. The isolated installed-wheel batch now exercises the equivalent
  128-byte/case-collision replace rejection and unchanged schema/row assertions
  inside its owned fixture; the original named/global gtest is not dispatched.
  This behavior remains unqualified until the admitted live batch passes.
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
