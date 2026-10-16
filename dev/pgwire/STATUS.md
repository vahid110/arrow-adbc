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
  The successor repair separates load-only installed evidence from the mandatory
  admitted execution window, with real prepare-to-consumer regressions.
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

This failure occurred before preflight publication/OIDC. No dedicated Redshift
resources, admission, claim or paid batch were started; the paid batch remains
unspent. The original failed run is retained, not rerun or relabeled as success.

| Date | Run | Head | Result | Est. incremental cost (USD) |
|---|---|---|---|---|
| 2026-10-16 | [38908550615](https://github.com/vahid110/arrow-adbc/actions/runs/38908550615) | `596aec4a23fd3c6fd134f5e569b902cd3134d28f` | failed before OIDC: load-only projection required an absent execution window; offline reproduction and successor repair | 0.00 |
