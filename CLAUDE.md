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

# ADBC Redshift fork: rules for agents

This is a fork of Apache `arrow-adbc` (`origin` = `vahid110/arrow-adbc`).
The goal is **v1: a released Redshift ADBC driver people can use**. Scope and
order of work live in [`dev/pgwire/V1_PLAN.md`](dev/pgwire/V1_PLAN.md); current
state lives in [`dev/pgwire/STATUS.md`](dev/pgwire/STATUS.md).

## Architecture rule

Common code owns wire protocol and ADBC mechanics; each database owns its
semantics. Shared code is under `c/driver/pgwire/` and the shared parts of
`c/driver/postgresql/`; Redshift differences go through the small backend
capability/profile interfaces (`c/driver/pgwire/backend.h`). Do not add a plugin
framework, and do not add an extension point until two backends need it.
Keep the fork rebaseable onto Apache `main`: avoid gratuitous renames/moves of
upstream code.

## Roles

- **Coordinator** (the long-running session): owns the queue, writes task briefs,
  merges PRs, runs live Redshift batches, reports to the human. Does not write
  product code.
- **Worker**: implements exactly one task brief on `task/<nnn>-<slug>`, branched
  from `feature/pgwire-core-redshift`. Writes the tests with the code. Opens a PR
  into `feature/pgwire-core-redshift`, makes CI green, answers review.
- **Reviewer**: independent; sees only the brief, the PR diff and this file.
  Posts one PR comment starting with `VERDICT: APPROVE` or
  `VERDICT: CHANGES REQUESTED`, listing concrete findings with file:line.
  Reviews correctness, test adequacy, architecture rule, scope creep.

## Hard rules

- Never push to `main`, never push to `upstream`, never open PRs against
  `apache/arrow-adbc`. Push only `task/*` branches to `origin`.
  Only the coordinator merges into `feature/pgwire-core-redshift` (squash).
- Merge requires: required CI green on the PR head, reviewer `APPROVE`, and
  `STATUS.md`/`V1_PLAN.md` updated if the task changes scope or state.
- No heavy local builds (the host has 8 GB RAM and other jobs). GitHub Actions is
  the build and test machine. Local work is limited to editing, `git`, `gh`, and
  cheap checks (e.g. `clang-format` on changed files, `pre-commit` on changed
  files if it is installed).
- AWS: never run the `aws` CLI or create/modify cloud resources. Live Redshift
  tests run only through the `PgWire PostgreSQL` workflow's `workflow_dispatch`,
  started by the coordinator under the budget below.
- Keep docs short. A task's PR description is its record; do not append long
  narrative to the docs. `STATUS.md` holds current state, not history.

## CI gates

Required on every PR (triggered by the PR or by the `task/*` push):
`PgWire PostgreSQL`, `PgWire Development Package` (when driver sources change),
`PgWire AWS Safety Mocks` (when its scripts change), `Native Libraries (Unix)`,
`Native Libraries (Windows)`, `Native Libraries (vcpkg)`, `Dev` (pre-commit).
`Integration` is known red on the base branch; it is not a gate until fixed.
Inspect failures with `gh run view <id> --log-failed`.

## Live Redshift budget (shared with the ODBCPP project, same AWS account)

- Redshift Serverless, one workgroup, 4 RPU. USD 150 cumulative allowance shared
  with ODBCPP; AWS budget alerts at USD 100/125/150 and a 267 RPU-hour monthly
  deactivation limit.
- At most one live batch per 12 hours, only when Redshift-affecting code merged
  since the last batch, only on a merged head with green CI. No automatic retry.
- Every live run is logged in `STATUS.md` with run id and estimated cost
  (4 RPU x billed minutes x USD 0.374/RPU-hour, minimum 60 s).
- Anything else (new resources, capacity, IAM changes, a projected total above
  USD 120) requires human approval.

## Escalate to the human (otherwise proceed)

Spending beyond the budget rule, scope/architecture decisions not settled by
`V1_PLAN.md`, the same task failing CI or review three times, secrets or
credential changes, publishing a release.
