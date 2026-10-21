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

## Current local-first workflow (human override, 2026-10-18)

Development stays local: no push, PR creation or GitHub Actions dispatch during
this phase. The coordinator maintains `local/redshift-types-20261018`; workers
branch from its exact verified commit, never a stale remote ref. One coordinator
serial O0/g0 build validates immutable checkpoints on this 8 GB host, including
meaningful native, public API, C Data and rebuilt installed-artifact tests.
Workers run small seams and pinned changed-file lint; they do not start competing
full builds. An independent exact-head/tree review is required before the
coordinator integrates locally.

After development, the coordinator owns the separately admitted finite live
window on dedicated resources and verified teardown, then the final push and all
applicable green CI gates. This ordering grants no paid attempt, credentials,
release or protected-resource authority. The prior Task008 batch is complete and
spent; its receipts remain immutable. AO/timer and automatic dispatch stay paused.
The PR/CI conventions below apply to the final remote phase.

## Roles

- **Coordinator** (the long-running session): owns the queue, writes task briefs,
  merges PRs, runs live Redshift batches, reports to the human. Does not write
  product code.
- **Worker**: implements exactly one task brief on `task/<nnn>-<slug>`, branched
  from the coordinator-verified local integration ref during development. Writes
  tests with the code, freezes immutable checkpoints and answers independent review.
  The coordinator controls integration and the final remote phase.
- **Reviewer**: independent; sees only the brief, the PR diff and this file.
  Records `VERDICT: APPROVE` or `VERDICT: CHANGES REQUESTED` for the exact local
  head/tree (a PR comment in the remote phase), with concrete file:line findings.
  Reviews correctness, test adequacy, architecture rule, scope creep.

## Hard rules

- Never push to `main`, never push to `upstream`, never open PRs against
  `apache/arrow-adbc`. Workers push only `task/*` branches to `origin`. The
  coordinator may also push and delete `redshift-v*` tags for release tasks.
  Only the coordinator merges into `feature/pgwire-core-redshift` (squash).
- Merge requires: every gate below that the PR triggered is green on the PR
  head, reviewer `APPROVE`, and `STATUS.md`/`V1_PLAN.md` updated in the same PR
  if the task changes scope or state.
- No worker full builds: the coordinator runs one serial immutable local build.
  Before each frozen checkpoint and final push, run pinned linters on every
  task-changed file: `SKIP=dotnet-format ~/.venvs/adbc-lint/bin/pre-commit run --files <files>`
  (clang-format 18.1.7, cpplint, cmake-format, whitespace). Batch fixes into one
  push; each push costs a CI round.
- AWS: workers/reviewers never run real AWS CLI/SDK calls or access host
  credentials; fake-only SDK tests are allowed. The coordinator alone operates
  the separately human-authorized Task008c dedicated host lifecycle and explicit
  `pgwire-qualification-live.yml` dispatch. The legacy shared-target live path
  remains closed; see `dev/pgwire/qualification-live.md` for the current finite
  admission, ownership and teardown boundary.
- Keep docs short. A task's PR description is its record; do not append long
  narrative to the docs. `STATUS.md` holds current state, not history.

## PR conventions

- Title: Conventional Commits, `type(component): subject`, where `component` is
  an existing path, e.g. `feat(c/driver/postgresql): ...`, `chore(dev): ...`,
  `docs(docs): ...`. The `Dev` workflow rejects other titles.
- Body: what / why / testing; no `@` mentions (`Dev` rejects them). End with an accurate
  tool-attribution line (Codex for Codex-authored work). Preserve historical
  attribution.

## CI gates

Each gate is required **when its path filter triggers it** (by the PR into
`feature/pgwire-core-redshift`); a gate that did not trigger is not required.

| Workflow (as shown in checks) | Triggers on |
|---|---|
| `PgWire Drivers` (`pgwire-postgresql.yml`) | `c/**` and its own file |
| `PgWire Development Package` | driver sources and build files |
| `PgWire AWS Safety Mocks` | its scripts |
| `PgWire Redshift Release` (dry run, includes the 5 wheels) | its own file, `pgwire-package.yml`, `c/driver/pgwire/install_smoke/**`, `c/driver/pgwire/redshift_manifest_smoke.c` |
| `PgWire Python` (`pgwire-python.yml`, fast Linux wheel) | `python/adbc_driver_redshift/**`, `python/adbc_driver_manager/**`, C driver and build sources, its own file |
| `PgWire Python Wheels` (`pgwire-python-wheels.yml`, 5 portable wheels) | its own file, `ci/scripts/python_util.sh`, `python_wheel_*` and `pgwire_python_wheel_test.sh`, `install_vcpkg.sh`, `ci/vcpkg/**`, the Redshift package's `setup.py`, `pyproject.toml`, `MANIFEST.in` |
| `Dev` (pre-commit, PR title/body) | every PR and push |

`Native Libraries (Unix)` and `(Windows)` (30-60 minutes, mostly macOS/Conda
queueing) are **not** PR gates. The coordinator dispatches `PgWire Native Daily`
on `feature/pgwire-core-redshift` once every 24 hours and opens a fix task for
any failure; a release candidate additionally requires a green daily run on its
exact head. The Sphinx docs build also lives only in Native Unix, so docs
breakage shows up in the daily run; for a docs-heavy PR the coordinator may
dispatch `PgWire Native Daily` on the PR branch before merging. A `Dev` failure while installing golangci-lint from
`proxy.golang.org`/`sum.golang.org` is a network flake: re-run the failed job.

Apache workflows unrelated to this fork (Rust, R, Java, C#, JavaScript,
Integration, vcpkg, verification, website, bots) are disabled with
`gh workflow disable`; GitHub once paused Actions on this fork for excessive
use. Inspect failures with `gh run view <id> --log-failed`.

## Live Redshift budget

Historical completed/spent Task008c authorization was one dedicated 4-RPU batch, USD 5 incremental
target, query at most 15 minutes, host compute at most 60 minutes, daily 4-RPU-hour
deactivation limit, zero paid retries and verified full teardown. Protected/shared
resources cannot be used. It grants no further attempt. The finite human-approved brief superseded the old
shared-target operating procedure below; historical amounts remain evidence.

Historical shared budget (no current shared-target access):

- Redshift Serverless, one workgroup, 4 RPU. USD 150 cumulative allowance shared
  with ODBCPP; AWS budget alerts at USD 100/125/150 and a 267 RPU-hour monthly
  deactivation limit.
- At most one live batch per 12 hours, only when Redshift-affecting code merged
  since the last batch, only on a merged head with green CI. No automatic retry.
- Every live run is logged with run id and estimated cost
  (4 RPU x billed minutes x USD 0.374/RPU-hour, minimum 60 s) in the
  `STATUS.md` of the next task PR; no separate PR just for the log.
- Anything else (new resources, capacity, IAM changes, a projected total above
  USD 120) requires human approval.

## Escalate to the human (otherwise proceed)

Spending beyond the budget rule, scope/architecture decisions not settled by
`V1_PLAN.md`, the same task failing CI or review three times, secrets or
credential changes, publishing a release.
