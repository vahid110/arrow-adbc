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

- Task 001 (land parked work): merged (PR #2).
- Task 003 (concise PgWire developer guide): PR in review.
- The next live Redshift batch must run
  `RedshiftSmokeTest.ReplaceIngestRejectsCollidingIdentifiersBeforeDrop`
  (Redshift replace identifier preflight, not yet live-qualified).

## Known issues

- Live Redshift job cannot assume the `adbc-redshift-ci` OIDC role (run
  38563931739); the next live batch is blocked until the role trust is checked.
- `Integration` and other unrelated Apache workflows are disabled
  in this fork.

## Live Redshift log

Workgroup `pgwire-ci` (eu-central-1), 4 RPU, shared budget with ODBCPP.

| Date | Run | Head | Purpose | Result | Est. cost (USD) |
|---|---|---|---|---|---|
| 2026-10-14 | [38563931739](https://github.com/vahid110/arrow-adbc/actions/runs/38563931739) | `fc6f13029` | post-#2 live batch | failed: OIDC `AssumeRoleWithWebIdentity` denied before ingress; no live tests ran | 0.00 |
