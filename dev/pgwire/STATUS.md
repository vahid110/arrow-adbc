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
  007 (user documentation, PR #12).
- Task 006 merged:
  helper `adbc_driver_redshift.iam` with `build_uri` and
  `fetch_{serverless,cluster}_credentials` (injectable clients, offline
  tests), `iam` extra (`boto3>=1.28`), recipe in `c/driver/postgresql/REDSHIFT.md`
  and `python/adbc_driver_redshift/README.md`; offline tests only, live
  IAM check deferred to next authorized batch.
- Task008a offline isolated-resource preparation is in progress
  ([PR #13](https://github.com/vahid110/arrow-adbc/pull/13)). The legacy
  shared-target dispatch is retired; structural admission and installed-artifact
  preflight/rehearsal are offline only. See the
  [qualification contract](../../c/driver/postgresql/REDSHIFT.md#isolated-qualification-preparation-task008a).
- Actual Task008 live execution is blocked by scoped provisioning authorization
  for the dedicated Serverless/SG resources and proposed isolated CI role, plus
  independently verified dedicated manifest/admission. Existing profiles
  authenticate; the old role exception is not permission to repurpose it.
  The human resolves an administrator-scoped grant/authorized provisioner externally.
  No dedicated resources or AWS/live qualification are established here.
  Task009 publication remains paused.
- Native matrices: `PgWire Native Daily` is dispatched every 24 hours on
  `feature/pgwire-core-redshift`.

## Known issues

- Historical shared-target live attempt could not assume `adbc-redshift-ci`
  (run 38563931739). This failure is retained as evidence, not a current role
  repair instruction.
  This does not authorize shared-resource reuse. A future isolated batch must run
  `RedshiftSmokeTest.ReplaceIngestRejectsCollidingIdentifiersBeforeDrop`; the
  Redshift replace preflight is not yet live-qualified.
- `Integration` and other unrelated Apache workflows are disabled
  in this fork.

## Historical live Redshift log

Workgroup `odbcpp-redshift-pilot` (eu-north-1, account 385207570137), 4 RPU,
shared with ODBCPP at the time; protected from current ADBC access.

| Date | Run | Head | Purpose | Result | Est. cost (USD) |
|---|---|---|---|---|---|
| 2026-10-14 | [38563931739](https://github.com/vahid110/arrow-adbc/actions/runs/38563931739) | `fc6f13029` | post-#2 live batch | failed: OIDC `AssumeRoleWithWebIdentity` denied before ingress; no live tests ran | 0.00 |
