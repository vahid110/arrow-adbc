<!--
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

# PostgreSQL-Wire Driver Development Plan

This document tracks the implementation of a reusable C++ ADBC core for databases
that expose the PostgreSQL wire protocol. PostgreSQL remains the compatibility
baseline and Amazon Redshift is the first additional backend.

The design rule is: common code owns libpq/wire interaction and ADBC mechanics;
each backend owns its SQL semantics, catalog interpretation, type meaning,
transaction behavior, authentication preparation, and supported transfer paths.

## Upstream baseline

- Repository: `apache/arrow-adbc`
- Baseline branch: `main`
- Original baseline commit: `150528f0fb9f1117fecce1da6a27fb548ddc6d0f`
- Current rebased baseline commit: `4d50e2e30f9e96905a64644defe964447d54cbe7`
- Development branch: `feature/pgwire-core-redshift`
- Historical references: PR #2219 (experimental Redshift support) and PR #4365
  (removal of that support)

The implementation should remain a sequence of small commits that can be rebased
onto Apache `main`. If Apache accepts an equivalent refactor, the corresponding
local commit should be dropped rather than retained as a competing implementation.

## Architectural target

`PgWireDatabase`, `PgWireConnection`, and `PgWireStatement` will own ADBC state and
compose a backend profile selected once after connecting. The profile will contain
capability data and focused type, metadata, transaction, query, and bulk-transfer
policies. It will not be a dynamic plugin framework.

The core continues to use libpq; it does not implement protocol framing, TLS, or
authentication exchanges itself.

## Milestones

- [x] Pin and record the upstream baseline.
- [x] Record the current PostgreSQL build/test baseline.
- [x] Extract libpq connection, result, cancellation, and error lifetime helpers.
- [x] Separate text-result and binary-COPY query paths.
- [x] Separate type discovery, Arrow mapping, and value encoding.
- [x] Move PostgreSQL metadata SQL behind a normalized metadata provider.
- [x] Isolate PostgreSQL transaction policy.
- [x] Construct the existing PostgreSQL driver from the reusable core and
      PostgreSQL semantics, with no intended behavior change.
- [x] Add Redshift detection and a read/query-only profile.
- [x] Add the verified Redshift scalar type matrix.
- [x] Add Redshift `GetInfo`, `GetTableTypes`, `GetObjects`, and `GetTableSchema`.
- [x] Add Redshift transaction coverage.
- [x] Add correctness-first batched-INSERT ingestion.
- [x] Package distinct PostgreSQL and Redshift driver artifacts.
- [x] Keep CMake and Meson source, artifact, package, and focused-test coverage in
      parity.

## Quality gates

Every structural milestone must:

1. Build as both shared and static libraries through the existing build system.
2. Preserve the full PostgreSQL test suite and validation behavior.
3. Add focused unit tests for new seams and failure paths.
4. Avoid backend-name conditionals in core execution code.
5. Return explicit `ADBC_STATUS_NOT_IMPLEMENTED` for unsupported semantics instead
   of incomplete or misleading results.
6. Keep public PostgreSQL symbols, package names, option names, and defaults stable.

## Redshift MVP support matrix

| Area | MVP behavior |
| --- | --- |
| Driver artifact | Dedicated shared/static `adbc_driver_redshift` library and `AdbcDriverRedshiftInit`; rejects non-Redshift servers |
| Connection/authentication | Standard libpq URI options, TLS, user/password; no Redshift-specific IAM token generation yet |
| Vendor detection | `SELECT version()` once during database initialization; required by the Redshift artifact |
| Query results | Portable libpq text-result path with Arrow conversion; PostgreSQL alone retains binary query `COPY` |
| Parameters | Prepared statements with binary parameter encoding |
| Core types | Boolean, signed integers, float, numeric-as-string, date/time/timestamp, text/varchar, and `VARBYTE` |
| Metadata | `GetInfo`, `GetTableTypes`, `GetObjects`, and `GetTableSchema`; constraints and statistics are explicitly unsupported |
| Transactions | Autocommit, explicit commit/rollback; isolation is database-configured and session overrides are explicitly unsupported |
| Bulk ingest | Create and append through atomic prepared inserts; PostgreSQL alone retains binary ingest `COPY` |

Post-MVP work should be driven by concrete use cases: IAM/Identity Center credential
helpers, Redshift-native staged `COPY`/`UNLOAD`, additional Redshift types such as
`SUPER` and spatial values, richer external/materialized-view metadata, and
faster large-scale transfer strategies beyond the now-tested multi-row insert
path. These do not belong in the core until a second implementation or a
measured Redshift requirement demonstrates the extension point.

## Production-readiness roadmap

The MVP is not a production-readiness claim. Work in small, independently buildable
and testable checkpoints; record the result of each checkpoint below. A green build
is evidence, not a substitute for a clean-client test or documented limitations.

### 1. Installable and usable driver

- [x] Publish a Redshift build/install/connection guide and explicit MVP limits.
- [x] Exercise the installed shared library through the ADBC driver manager from
      a clean prefix, independent of the driver-linked unit test binary.
- [x] Add a CI smoke test for the installed artifact and its public header/package
      metadata on Linux.

### 2. Correctness and compatibility

- [x] Expand focused live Redshift tests for NULL values, query errors and
      recovery, larger results, parameter schemas, and quoted/empty/UTF-8
      text plus binary edge values.
- [x] Qualify quoted table and column identifiers through live Redshift
      `GetTableSchema` and column-filtered `GetObjects`, with fixture cleanup.
- [x] Qualify signed 32/64-bit integer limits and positive/negative
      `DECIMAL(38,0)` values against the live Redshift engine.
- [ ] Expand coverage for remaining type and metadata edges only where live
      behavior or a concrete client use case justifies it.
- [x] Qualify Redshift query cancellation with a bounded, cleanup-safe live
      query; do not reuse PostgreSQL's `pg_sleep()` fixture, which Redshift
      documents as unsupported.
- [x] Repeat the opt-in cancellation case before deciding whether to make it a
      push-triggered CI gate. Two bounded live successes justify retaining it
      as an opt-in gate while Redshift CI cost is being minimized.
- [ ] Keep PostgreSQL's complete integration suite and downstream client
      compatibility tests green; preserve observed public behavior and ordering.
- [x] Define a documented support matrix with explicit unsupported features and
      test evidence for each claim.

### 3. Redshift-native capabilities

- [ ] Add optional, short-lived IAM credential preparation without changing the
      common libpq authentication path.
- [x] Benchmark prepared-insert throughput before choosing an optimization.
- [x] Add an opt-in, one-shot 1,000-row parameterized-insert benchmark with an exact
      row-count check and guaranteed test-table cleanup.
- [x] Provision a private, short-retention S3 fixture and a namespace-attached,
      read-only Redshift IAM role for an opt-in staged `COPY` experiment.
- [ ] Resolve the scoped IAM role-assumption failure before using staged `COPY`.
- [x] Add and live-test a bounded multi-row parameterized INSERT path for
      Redshift, preserving whole-bind atomicity, as an intermediate improvement
      that requires no AWS uploader or broader IAM trust.
- [x] Qualify a small, explicit `JSON_SERIALIZE(SUPER)` query as Arrow text,
      while documenting that its `VARCHAR` limit makes it unsuitable as an
      automatic general `SUPER` mapping.
- [ ] If justified, add staged S3 `COPY` ingestion with least-privilege IAM,
      deterministic object cleanup, and a separate opt-in live test. Consider
      `UNLOAD` only after a measured read-path need.
- [ ] Extend `SUPER`, spatial, external-object, or materialized-view support only
      with verified Redshift behavior and tests.

### 4. Release and upstream alignment

- [ ] Split backend-neutral PostgreSQL-wire refactors into small Apache-ready
      contributions, independent of Redshift-specific behavior.
- [x] Prepare and publish the first isolated Apache-ready libpq RAII cleanup
      branch from current `upstream/main`, with no Redshift code or project
      roadmap in its diff; build and run both PostgreSQL C++ test suites locally.
- [x] Prepare and publish a second small Apache-ready cancellation-handle
      cleanup stacked on the first, again without Redshift code or roadmap
      changes and with both PostgreSQL C++ test suites passing locally.
- [x] Prepare and publish a third small Apache-ready COPY-stream result-handle
      cleanup stacked on the second, with only PostgreSQL statement code in
      its added diff and both PostgreSQL C++ suites passing locally.
- [x] Publish the fourth Apache-facing PostgreSQL-only parameter-schema status
      fix after the core branch's native Windows and Redshift checks pass.
- [ ] Publish open-source release artifacts, install instructions, checksums,
      dependency requirements, and a tested platform matrix.
- [x] Start with a short-retention Ubuntu x86-64 development archive containing
      licenses, install guide, and SHA-256 checksum; compile and load a client
      against the extracted archive before publishing it as a CI artifact.
- [x] Extend the same short-retention, extracted-client archive check to macOS
      Intel and Apple Silicon, with architecture-specific artifacts and runtime
      dependency instructions.
- [x] Connect an independent client built from each extracted macOS archive
      through its PostgreSQL driver to a temporary PostgreSQL 18 server, then
      stop that server in an unconditional cleanup step.
- [x] Publish separately checked Debian Bookworm x86-64 and Ubuntu 24.04 ARM64
      development archives; keep their qualification distinct from Ubuntu
      x86-64 and from production release support.
- [x] Connect an independent client built from each extracted Linux archive
      through its PostgreSQL driver to PostgreSQL 18 on the matching CI target;
      require the packaged Redshift driver to reject the same server.
- [x] Publish short-retention Windows x64/ARM64 development ZIPs only after
      collecting linked vcpkg dependency licenses, verifying SHA-256 after
      download, and loading the DLL from an extracted archive on each native
      Windows architecture.
- [x] Connect independent clients built from each extracted Windows ZIP to a
      temporary native PostgreSQL 18 server: the packaged PostgreSQL driver
      must connect, and the packaged Redshift driver must reject that server.
- [x] Download a checked, same-commit Ubuntu x86-64 Release development
      archive in the live Redshift job and connect an independent client from
      that extracted archive without treating it as a production release.
- [x] Rebase on a newer Apache baseline after checking upstream changes and
      rerunning PostgreSQL, Redshift, and downstream compatibility suites.
- [x] Rehearse the 67-commit driver series on the newer Apache main without
      patch drift and pass local PostgreSQL plus Redshift artifact tests.

### Platform and architecture qualification

The current evidence proves source builds and short-lived development archives,
not production prebuilt-binary support. A release claim requires an
installed-artifact smoke test on each target plus appropriate database-backed
behavior tests. Do not infer Debian support from Ubuntu alone.

| Target | Current evidence | Release qualification still needed |
| --- | --- | --- |
| Ubuntu 24.04 x86-64 | CMake/Meson builds and tests; PostgreSQL 18 and live Redshift; relocated Debug client query against Redshift; checked, short-retention Release development archive with extracted-client PostgreSQL 18 query, Redshift-driver PostgreSQL rejection, and earlier live Redshift connection | Production release artifact and compatibility guarantee |
| macOS Intel | CMake build, PostgreSQL 18 suite, clean-prefix manager/client smoke; checked x86-64 development archive with extracted-client PostgreSQL 18 query and Redshift-driver PostgreSQL rejection | Production release artifact and live Redshift installed-client test |
| macOS Apple Silicon | CMake build, PostgreSQL 18 suite, clean-prefix manager/client smoke; checked arm64 development archive with extracted-client PostgreSQL 18 query and Redshift-driver PostgreSQL rejection | Production release artifact and live Redshift installed-client test |
| Windows x86-64 | C++/vcpkg Release build; checked, short-retention x64 development ZIP; extracted-client PostgreSQL 18 query and Redshift-driver PostgreSQL rejection | Production release artifact and live Redshift installed-client test |
| Windows ARM64 | Native vcpkg Release build; checked, short-retention ARM64 development ZIP; extracted-client PostgreSQL 18 query and Redshift-driver PostgreSQL rejection | Production release artifact and live Redshift installed-client test |
| Debian Bookworm x86-64 | CMake build, PostgreSQL 18 suite, clean-prefix manager/client smoke; checked x86-64 development archive with extracted-client PostgreSQL 18 query and Redshift-driver PostgreSQL rejection | Production release artifact and live Redshift installed-client test |
| Ubuntu 24.04 ARM64 | Native CMake build, PostgreSQL 18 suite, clean-prefix manager/client smoke; checked arm64 development archive with extracted-client PostgreSQL 18 query and Redshift-driver PostgreSQL rejection | Production release artifact and live Redshift installed-client test |

- [x] Add a Linux x86-64 clean-prefix installation test first.
- [x] Add macOS Intel/Apple Silicon and Windows x86-64/ARM64 installation tests.
- [x] Add dedicated Debian x86-64 and Linux ARM64 CI jobs.
- [ ] Publish packages only for targets whose release qualification is green.

## Redshift test-cost discipline

- Use the `eu-central-1` Serverless workgroup `pgwire-ci`, capped at 4 RPUs.
- On 2026-09-13 the AWS dashboard showed $298.08 of $300.00 Redshift trial
  credit remaining, expiring 2026-12-11. The workgroup has a 4-RPU maximum
  capacity but no RPU-hour usage limit or alarm; do not choose an arbitrary
  query-disabling threshold without an agreed budget. AWS documents that idle
  Serverless workgroups are not billed for compute, while managed storage is
  still charged.
- A read-only dashboard check after the Release-archive and repeat-cancellation
  live runs on 2026-09-13 showed $297.51 of $300.00 trial credit remaining,
  with no snapshots or active alarms. This is $0.57 less than the previous
  recorded balance, but the dashboard balance is not a per-run cost breakdown.
  The compute-usage panel was not opened because the console warns that
  retrieving it may consume workgroup capacity.
- Keep test runs focused and batched; do not run Redshift for PostgreSQL-only changes.
- [ ] Harden temporary CI ingress cleanup for an ambiguous authorize failure:
  the current `always()` step runs only after a confirmed successful authorize.
  If AWS creates the rule but the CLI loses its response, the `/32` could remain.
  Do not blindly revoke by CIDR after failure: an identical pre-existing rule
  might belong to someone else. A safe recovery needs a run-unique rule
  description, the returned rule ID when available, and read-only
  [`ec2:DescribeSecurityGroupRules`](https://docs.aws.amazon.com/cli/latest/reference/ec2/describe-security-group-rules.html)
  to verify exact ownership before revoking an ambiguous rule. AWS lists this
  read-only action without a resource type, so its IAM grant would require
  `Resource: "*"` (with a regional condition where supported). This is a
  proposed IAM permission expansion, not yet made.
- Avoid keepalive connections, polling queries, and idle open transactions so that
  Serverless can return to its non-compute-billed idle state promptly.
- Check trial-credit and RPU usage before and after larger integration runs.
- Delete temporary schemas, tables, snapshots, and other billable test artifacts.
- Do not purchase reservations or increase capacity without explicit approval.
- When Redshift testing is no longer active, evaluate deleting the workgroup and
  namespace; recreate them when needed rather than carrying avoidable storage cost.

See [AWS Serverless billing and cost controls](https://docs.aws.amazon.com/redshift/latest/mgmt/serverless-billing-on-demand.html)
for the distinction between idle compute, storage, RPU ceilings, and usage limits.

### Staged `COPY` test fixture (not driver support)

In AWS account `149112076833`, region `eu-central-1`:

- Bucket `adbc-pgwire-ci-149112076833-euc1` is dedicated to the `pgwire-ci`
  workgroup. Public access is blocked, ACLs are disabled, and default encryption
  uses SSE-S3. The `staging/` prefix expires after one day and incomplete
  multipart uploads are aborted after one day. Tests must still delete their
  exact objects in a `finally`/cleanup path; lifecycle is a backstop.
- IAM role `adbc-pgwire-ci-copy` trusts the two Redshift service principals
  required by AWS documentation, constrained to account `149112076833` and
  the known `pgwire-ci` workgroup/namespace ARN variants, including the actual
  workgroup UUID ARN shown in the console. Its inline policy
  permits `s3:ListBucket` only for `staging/*` and `s3:GetObject` only for
  `staging/*`. It has no S3 write or broad managed policy.
- The role was attached to namespace `pgwire-ci` on 2026-09-12. A subsequent
  `get-namespace` returned `AVAILABLE` with the role `in-sync`.
- A two-row, temporary-table `COPY` smoke test failed three times with
  `UnauthorizedException: Not authorized to get credentials of role`, even
  after adding the documented second service principal and exact known
  workgroup/namespace ARN variants. A database privilege check returned
  `has_assumerole_privilege(..., 'copy') = true`, so database `ASSUMEROLE` is
  not the apparent cause. No `AssumeRole` event for this role was visible in
  CloudTrail event history. The failed batches rolled back their temporary
  tables; the exact staging object was deleted after the test.
- On 2026-09-13, temporarily removing only `aws:SourceArn` while retaining
  `aws:SourceAccount` and the narrow read-only S3 policy let the same two-row
  `COPY` succeed. The Data API batch finished all three statements and its
  count query returned 2. CloudTrail event history in `eu-central-1` and
  `us-east-1` did not expose an `AssumeRole` event for this role, so the actual
  source ARN remains unknown. The exact test object was deleted and the
  original three-ARN trust condition was restored and verified. Thus the
  current test role is intentionally not usable for `COPY`; choose and verify
  a least-privilege operational trust policy before enabling staged ingestion.
- On 2026-09-13, a second temporary probe replaced the failing `aws:SourceArn`
  condition with the exact `sts:ExternalId` for the `IAM:RootIdentity` Redshift
  database-user ARN, retaining `aws:SourceAccount`, both Redshift service
  principals, and the read-only S3 policy. Data API batch
  `7d8fb5c1-d7c6-430a-b369-396a6364f3da` created a temporary table, loaded
  the two-row SSE-S3 object, and returned a count of 2. The original
  three-ARN trust policy was restored (verified by semantic comparison), and
  the exact staging object was deleted (verified by a 404). This validates
  role assumption and `COPY` for this database identity only; it does not
  enable staged ingestion in the ADBC driver or establish a permanent CI trust
  policy. AWS documents [database-user-scoped `sts:ExternalId` trust](https://docs.aws.amazon.com/redshift/latest/mgmt/authorizing-redshift-service-database-users.html)
  as a way to restrict Redshift role access.
- AWS's [Redshift confused-deputy guidance](https://docs.aws.amazon.com/redshift/latest/mgmt/cross-service-confused-deputy-prevention.html)
  recommends both `aws:SourceAccount` and a Serverless workgroup
  `aws:SourceArn`. Its example uses a workgroup *name* in the ARN, while the
  [Serverless service authorization reference](https://docs.aws.amazon.com/service-authorization/latest/reference/list_redshift-serverless.html)
  defines the workgroup ARN by *ID*. The fixture already tried both forms,
  including its actual UUID, so this documentation difference alone does not
  explain the failed assumption. The observed success when only `SourceArn`
  was removed suggests the request's source ARN was absent or different, but
  that is an inference, not a verified value. The exact `sts:ExternalId` probe
  above is a verified alternative for `IAM:RootIdentity`; qualify a dedicated
  non-root test identity and its narrow trust before any permanent change.
- This fixture alone does not enable ADBC staged ingestion. An uploader needs
  separate, short-lived write permissions, and any opt-in driver path needs
  strict option validation, safe SQL construction, and deterministic cleanup.

The next gated AWS checkpoint is a *manual*, two-row non-root test, not an
automatic push job. Reuse the branch-scoped GitHub OIDC role `adbc-redshift-ci`
as the test identity rather than creating a long-lived key. Before changing the
COPY role's trust, grant only workgroup-scoped Serverless credential access to
that identity and verify its actual `IAMR:` database username. Then evaluate
an exact database-user `sts:ExternalId` alongside `aws:SourceAccount` and the
two Redshift service principals; keep the COPY role's S3 access read-only.
Restrict the uploader identity to `PutObject`/`DeleteObject` for
`staging/ci/*`. Use unique run-specific data and manifest keys, require
`mandatory: true`, verify exactly two rows, and delete both exact objects in
failure-safe cleanup. The one-day lifecycle remains a backstop, not the normal
cleanup path. An isolated non-canceling CI concurrency group is needed so a
push cannot interrupt cleanup. No permanent IAM change or new CI path has been
made for this checkpoint; qualify the policy and database privileges first.

The benchmark is available through manual dispatch of the `PgWire Drivers`
workflow with `benchmark=true`. Push-triggered CI does not run it. Run
`34744399423` inserted and verified 1,000 rows in 186.081 seconds, or 5.374
rows/second, on the 4-RPU `pgwire-ci` workgroup. After bounded 16-row batching,
run `34745478491` inserted and verified 1,000 rows in 11.7552 seconds, or
85.069 rows/second (about 15.8 times faster). These are single measurements,
not capacity estimates. High-volume ingest still justifies an opt-in staged
`COPY` investigation.

See the AWS documentation on [Serverless namespace IAM roles](https://docs.aws.amazon.com/redshift/latest/mgmt/serverless-security-other-services.html),
[minimum S3 permissions for `COPY`](https://docs.aws.amazon.com/redshift/latest/dg/copy-usage_notes-access-permissions.html),
and [source-scoped service trust](https://docs.aws.amazon.com/redshift/latest/mgmt/cross-service-confused-deputy-prevention.html).

## Current work

The planned Redshift MVP and reusable-core milestones are complete. PostgreSQL CI
remains the first mandatory gate. After it passes, the Redshift job builds before
requesting short-lived AWS credentials, opens TCP 5439 for only the current GitHub
runner `/32`, runs only the focused Redshift suite, and revokes that exact rule in
an `always()` cleanup step. The AWS role can modify ingress on only the dedicated
Redshift security group and its OIDC trust is pinned to this repository's immutable
owner/repository IDs plus the development branch. The bounded multi-row INSERT
path is live-tested, while two-row staged `COPY` probes proved the AWS mechanism
with reduced trust conditions, including an exact database-user External ID.
Operational trust for a dedicated non-root identity and an uploader remain
unfinished. A private, offline-tested preparation helper now builds a mandatory
exact-object manifest and validates `COPY` SQL, but is not selected by the
driver. Four PostgreSQL-only cleanup patches are published on separate
Apache-facing fork branches.
Current work is production hardening and platform qualification through
short-lived evaluation archives.

## Progress log

- 2026-09-13: Extended the extracted-archive client check on Linux and macOS
  to require the packaged Redshift driver to reject PostgreSQL, matching the
  existing Windows check. Workflow YAML, shell syntax, and diff checks passed;
  package CI run `34767036919` passed all seven platform jobs plus the final
  archive-and-checksum gate. Separately documented an ambiguous
  ingress-authorize cleanup gap and its ownership-safe remediation; no AWS IAM
  or security-group change was made.
- 2026-09-13: Strengthened the independent installed-client smoke test to
  execute `SELECT CAST(42 AS BIGINT)` and check one non-null Arrow int64 value,
  stream exhaustion, and resource cleanup. A local PostgreSQL 17 connection
  passed; the Redshift driver's PostgreSQL rejection and load-only paths still
  passed. Fixed a Windows `min`/`max` macro collision in the numeric-boundary
  test and a codespell false positive in the staged-`COPY` test fixture.
  Local builds, focused tests, pinned formatting, and codespell passed. CI run
  `34764557170` passed all five PostgreSQL 18 targets and the focused live
  Redshift suite, including the relocated-client query and temporary ingress
  cleanup. Package run `34764557189` verified extracted-client queries on all
  seven Linux, macOS, and Windows targets. Dev run `34764557186` passed, and
  the native Windows C++ job passed the previously failing driver build.
- 2026-09-13: CI run `34762177471` passed all five PostgreSQL 18 targets and
  the focused live Redshift job; its temporary ingress cleanup succeeded.
  Development-package run `34762177517` passed all seven Linux, macOS, and
  Windows archive targets. An earlier Linux test build exposed that the new
  private helper was not exported from the shared driver; the test now compiles
  the helper separately without expanding the driver's public ABI.
- 2026-09-13: Added a Redshift-private staged-`COPY` preparation helper that
  validates generated-object S3 URLs, a single IAM role ARN, and separate
  schema/table identifiers. It produces a one-object `mandatory: true`
  manifest and quoted `COPY ... MANIFEST CSV` SQL, with no uploader, execution,
  public option, or change to the selected batched-INSERT path. Focused local
  tests passed; live staged-ingest support remains blocked on a dedicated
  non-root trust policy and deterministic upload/object cleanup.
- 2026-09-13: Verified a database-user-scoped `sts:ExternalId` trust candidate
  for the two-row Redshift Serverless `COPY` fixture. The Data API batch
  returned count 2. Restored and semantically verified the original
  `aws:SourceArn`-scoped trust, then deleted the exact S3 object and confirmed
  its absence. This is an AWS-side feasibility result, not driver support or a
  permanent trust-policy change.
- 2026-09-13: Confirmed the AWS Serverless dashboard's free-trial balance
  after the live qualification runs: $297.51/$300.00 remained, versus $298.08
  at the previous recorded check. No snapshots or alarms were present; no
  capacity, IAM, or billing settings were changed.
- 2026-09-13: Rechecked AWS's current Redshift confused-deputy example and
  Serverless ARN reference against the unsuccessful scoped `COPY` attempts.
  The example's name-based workgroup ARN differs from the ID-based ARN
  reference, but both were already tried in this fixture. Documented the
  unresolved source-context question without changing IAM policy or
  incurring another Redshift test window.
- 2026-09-13: Manually dispatched focused run `34758596998` passed all five
  PostgreSQL 18 platform jobs, 19 standard live Redshift cases, and a second
  bounded cancellation case in 5.816 seconds. The connection remained usable
  afterward and exact temporary ingress cleanup passed. Cancellation remains
  opt-in rather than push-triggered to avoid additional paid Redshift windows
  on routine commits; this is two successes, not a broad reliability claim.
- 2026-09-13: Corrected package run `34758297459` passed all seven native
  development archive jobs and its final download/checksum gate. Both Windows
  x64 and ARM64 jobs built an independent client from their extracted ZIPs,
  connected through the packaged PostgreSQL driver to native PostgreSQL 18,
  verified the packaged Redshift driver rejected that server, and stopped it.
  This is database-backed archive qualification on Windows, not a live
  Windows-to-Redshift or production release claim.
- 2026-09-13: Initial Windows archive run `34758013851` passed the new native
  ARM64 extracted-client PostgreSQL connection/rejection gate. Its x64 job
  built and loaded the ZIP but failed before database startup because the
  UCRT64 PostgreSQL package's executables were not on the existing MINGW64
  compiler PATH. The workflow now resolves `initdb` and `pg_ctl` through each
  architecture's explicit MSYS2 package bin directory; the successful
  requalification is recorded above.
- 2026-09-13: Added a native PostgreSQL 18 package dependency to both Windows
  archive jobs and a post-extraction database-backed smoke gate. Each job
  builds the independent CMake client against its extracted ZIP, then should
  connect through the packaged PostgreSQL DLL and verify that the packaged
  Redshift DLL rejects PostgreSQL. The temporary server is stopped in a
  `finally` block. `actionlint` passed with only the two known newer GitHub
  runner labels suppressed; native CI evidence is recorded above.
- 2026-09-13: Package run `34757277976` passed all seven native archive jobs
  and its final downloadable archive/checksum gate at commit `9c20745f69`.
  Manually dispatched focused run `34757555359` at that exact commit passed
  all five PostgreSQL 18 platform jobs and all 19 live Redshift smoke tests.
  Its independent client compiled against the downloaded, SHA-256-checked
  Ubuntu x86-64 Release development archive connected successfully to live
  Redshift; the exact temporary runner ingress was revoked. This qualifies
  that development archive connection, not a production release or the other
  platforms' live Redshift connectivity.
- 2026-09-13: Added an opt-in manual live qualification path for the actual
  Ubuntu x86-64 Release development archive. It requires a successful package
  run at the exact current commit, verifies the archive SHA-256, extracts it,
  compiles an independent client against that tree before AWS access, then
  connects to Redshift during the existing short-lived ingress window. The
  workflow token has `actions: read` only for this download. `actionlint` and
  artifact-name/run-metadata preflight checks passed; the end-to-end live run
  is recorded above.
- 2026-09-13: Focused run `34756822553` passed all five PostgreSQL 18
  platform jobs and 19 live Redshift smoke tests. The Ubuntu job tarred and
  relocated its Debug CI installation, compiled the independent client
  against that extracted tree, connected to live Redshift, and revoked exact
  temporary runner ingress. This proves relocation in the live test path,
  but does not yet connect the separate Release development archive to
  Redshift or qualify a production package.
- 2026-09-13: Changed the Ubuntu live Redshift job to tar and relocate its
  existing installed CMake build before compiling and connecting the
  independent driver-manager client. This is a relocated Debug CI build, not
  the separate Release development archive, but it tests path-independent
  packaged-driver loading against live Redshift without a second build or
  additional database query. `actionlint` passed; focused CI later passed as
  recorded above.
- 2026-09-13: Isolated package run `34756455835` completed successfully on
  all seven targets and the final archive/checksum download gate. Both macOS
  jobs logged an independent client loading the extracted Redshift driver,
  connecting through the extracted PostgreSQL driver to local PostgreSQL 18,
  and stopping that temporary server. This qualifies database-backed archive
  use on macOS Intel/Apple Silicon, not packaged-client Redshift connections
  or production release compatibility.
- 2026-09-13: Extended macOS Intel and Apple Silicon development-archive jobs
  to start temporary PostgreSQL 18 servers, connect an independent client
  through each extracted PostgreSQL driver, and stop the servers in an
  unconditional cleanup step. A separately extracted archive connected to
  local PostgreSQL 18 on Apple Silicon, and `actionlint` passed. Isolated
  seven-platform package run `34756455835` was pending at this checkpoint and
  later passed, as recorded above.
- 2026-09-13: Isolated package run `34756083906` completed successfully on
  all seven targets, including its final archive/checksum download gate. All
  three Linux jobs logged an independent client loading the extracted
  Redshift driver and then connecting to PostgreSQL 18 through the extracted
  PostgreSQL driver. This establishes database-backed archive use on Ubuntu
  x86-64/ARM64 and Debian x86-64, but not packaged-client Redshift use or a
  production release guarantee.
- 2026-09-13: Extended the Linux development-archive workflow to start a
  PostgreSQL 18 service and compile a driver-manager client against the
  extracted archive that connects through its PostgreSQL driver. This closes
  a gap left by load-only archive smoke tests, without involving Redshift or
  AWS. The same independent client compiled and connected through a fresh
  local CMake installation against PostgreSQL 18; `actionlint` passed.
  Isolated package workflow `34756083906` was pending at this checkpoint and
  later passed, as recorded above. The commit used `[skip ci]` and manually
  dispatched only package CI to avoid an unnecessary live Redshift rerun for
  client-test code.
- 2026-09-13: Focused run `34755613857` passed all five PostgreSQL 18
  platform jobs and 19 live Redshift smoke tests. The new
  `ReadsExplicitlySerializedSuperAsText` case passed for a small JSON array;
  the independently installed client connected, and the exact temporary
  ingress was revoked. This does not qualify native or large-value `SUPER`
  mapping.
- 2026-09-13: Added a read-only live test for explicit
  `JSON_SERIALIZE(JSON_PARSE(...))` as Arrow text and documented why this
  cannot be an automatic, arbitrary-size `SUPER` mapping: Redshift's
  serialized `VARCHAR` limit is smaller than its `SUPER` value limit. The
  pinned formatter, local C++ build, and PostgreSQL suites pass; the live
  qualification was pending at this checkpoint and later passed as recorded
  above. Native `SUPER` mapping remains unsupported.
- 2026-09-13: Manual focused run `34755153065` passed all five PostgreSQL 18
  platform jobs, the 18 existing live Redshift smoke tests, and the separate
  opt-in `RedshiftCancelTest.CancelsBoundedAnalyticQuery` (5.52 seconds
  including connection setup and the deliberate two-second wait). The
  assertion required cancellation before the 20-second server backstop and
  verified the connection remained usable. The independently installed
  client connected and the exact temporary runner ingress was revoked.
  Cancellation has one successful live qualification, but remains opt-in
  until repeatability is established.
- 2026-09-13: Prepared an opt-in Redshift cancellation experiment using a
  1,024-row recursive CTE crossed three ways, a per-session 20-second
  `statement_timeout` backstop, a separate cancel thread, a 15-second
  completion assertion to distinguish explicit cancellation from timeout,
  and a post-cancel connection-health check. It is excluded from push-triggered
  Redshift tests and requires manual dispatch with `cancel_test=true`.
  `actionlint`, the pinned formatter, the local C++ build, and both PostgreSQL
  suites pass. Live behavior was unqualified at this checkpoint; the manual
  result is recorded above.
- 2026-09-13: Focused run `34754680798` passed all five PostgreSQL 18
  platform jobs and all 18 live Redshift tests. The new
  `PreservesNumericBoundaries` case passed for integer limits and exact
  38-digit decimal-as-string values; the independently installed client
  connected, and the exact temporary ingress was revoked successfully.
- 2026-09-13: Added a live value-and-schema test in `ceea6f746` for signed
  32/64-bit integer limits and positive/negative 38-digit `DECIMAL(38,0)`
  values (Arrow strings under the current lossless numeric policy). The pinned
  formatter, local C++ build, and both PostgreSQL suites pass; focused run
  `34754680798` was pending at this checkpoint and later passed, as recorded
  above.
- 2026-09-13: Focused run `34754290928` passed all five PostgreSQL 18
  platform jobs and all 17 live Redshift tests. The new
  `MetadataQuotesTableAndColumnNames` case passed, the independently installed
  driver-manager client connected, and exact temporary runner ingress cleanup
  succeeded. This qualifies the quoted-identifier metadata edge, not broader
  numeric or cancellation behavior.
- 2026-09-13: Added a cleanup-safe live metadata case for a quoted table name,
  a spaced column name, and a reserved-word column in `b72d7f696`. It checks
  both `GetTableSchema` and column-filtered `GetObjects`. The pinned formatter,
  local C++ build, and both PostgreSQL suites against local PostgreSQL pass;
  focused run `34754290928` was pending at this checkpoint and later passed,
  as recorded above. The narrowed path filter correctly avoided another
  seven-platform package rebuild for this test-only change.
- 2026-09-13: Delayed focused run `34752544871` completed successfully: all
  five PostgreSQL 18 platform jobs passed, followed by 16 live Redshift tests.
  `PreservesTextAndBinaryEdges` and `DiscoversParameterSchema` both passed;
  the independently installed client connected, and the exact temporary
  runner ingress was revoked successfully. The fourth PostgreSQL-only
  Apache-facing branch also passed Dev, Integration, Rust, Native Unix,
  Native Windows, and native vcpkg fork workflows. Cancellation and further
  metadata/type edges remain separate, unqualified work.
- 2026-09-13: Focused run `34752544871` passed its Ubuntu x86-64, Ubuntu
  ARM64, and Debian PostgreSQL 18 jobs. Both macOS jobs remained queued, so
  the dependency-gated live Redshift job had not started. GitHub's
  [status page](https://www.githubstatus.com/) reported degraded Actions
  performance beginning 09:25 UTC; this may explain the runner delay but is
  not evidence that the new live cases passed. At that point, the prior
  14-test Redshift result was the last completed live qualification; the
  successful completion is recorded above.
- 2026-09-13: Added live Redshift test cases for parameter-schema discovery
  and quoted/empty/UTF-8 text plus zero/high-byte binary values in
  `3690c591c`. The pinned formatter, local C++ build, both PostgreSQL suites
  against PostgreSQL 18, and Redshift test binary's non-live checks pass.
  Focused multi-platform and live Redshift CI run `34752544871` was pending
  at this checkpoint and later passed, as recorded above. The package
  workflow now excludes test-only and Markdown changes so the seven archives
  are not rebuilt for coverage-only edits; canceled its redundant queued run
  `34752544856` without affecting the focused database test.
- 2026-09-13: With the renewed AWS console session, confirmed the `pgwire-ci`
  workgroup is available, its actual workgroup ARN is still covered by the
  staged-COPY role trust condition, and the trust policy still includes both
  service principals, `aws:SourceAccount`, and the scoped `aws:SourceArn`
  variants. The Serverless dashboard still displayed $298.08 of $300 trial
  credit; no compute-usage chart or query was opened for this check. The
  apparent source-context mismatch remains unresolved, so no IAM scope was
  broadened.
- 2026-09-13: Published `feature/pgwire-parameter-schema-upstream` at
  `8c4797e84` on the fork after the core branch's native Windows C/C++ jobs,
  14 live Redshift tests, and installed-client live test passed. The fourth
  Apache-facing diff is limited to `statement.cc` and one PostgreSQL test;
  both PostgreSQL C++ suites and the pinned formatter passed locally. Fork CI
  for this new branch was pending at publication and later passed, as recorded
  above; no Apache PR has been opened.
- 2026-09-13: Isolated package workflow `34751985546` passed on all seven
  targets, including native Windows x64 and ARM64 ZIP builds, extracted-client
  DLL load checks, and the final archive/checksum download gate. Separately
  downloaded both Windows ZIPs and verified their unmodified LF-only SHA-256
  files with macOS `shasum -c`; inspected the x64 and ARM64 PE DLLs and linked
  vcpkg copyright texts. These are short-retention development archives, not
  production releases or live Windows-to-Redshift qualification.
- 2026-09-13: The corrected Windows x64 ZIP job in workflow `34751714432`
  passed its native build, vcpkg copyright collection, extracted CMake-client
  build, and DLL load. A separate download audit confirmed an x86-64 PE DLL
  and libpq/OpenSSL/zlib/lz4 license texts, but found its PowerShell-written
  `.sha256` used CRLF; macOS `shasum -c` treats the trailing carriage return
  as part of the archive filename. The archive bytes match after ignoring
  CRLF, but that artifact pair was not cross-platform portable. Windows
  checksum output is now LF-only with an explicit no-CR assertion; the
  replacement workflow passed, as recorded above.
- 2026-09-13: Prepared local Apache-facing branch
  `feature/pgwire-parameter-schema-upstream` with commit `8c4797e84`, stacked
  on the third cleanup. Its added diff changes only PostgreSQL statement code
  and a focused parameter-schema test. Both PostgreSQL C++ suites and the
  pinned formatter pass locally; subsequent publication is recorded above.
- 2026-09-13: Initial Windows ZIP workflow `34751390419` built all five
  existing Linux/macOS archive targets but both native Windows Release jobs
  exposed a previously inherited `GetParameterSchema` bug: a nanoarrow
  `ArrowErrorCode` could be returned as an ADBC status, which MSVC rejects
  under warnings-as-errors. Changed that path to report a proper ADBC internal
  error and use a scoped temporary schema, and added a PostgreSQL parameter
  schema regression test. The pinned formatter, local macOS Apple Silicon
  build, both PostgreSQL suites against local PostgreSQL, and the Redshift
  artifact suite pass. Windows requalification is pending. Package CI now
  triggers for changes to driver, manager, and core CMake sources too, so
  downloadable artifacts cannot silently lag behind code changes.
- 2026-09-13: The third Apache-facing COPY-stream result-handle branch passed
  Dev, Integration, Rust, Native Windows, native vcpkg, and Native Unix fork
  workflows; its added diff remains limited to two PostgreSQL statement
  files. It is ready for upstream review preparation, but no Apache PR has
  been opened. Native Windows x64/ARM64 development ZIP qualification is now
  running in isolated package workflow `34751390419`; release claims remain
  pending its result and third-party-license inventory.
- 2026-09-13: The rebased core branch's Native Unix workflow `34749280271`
  completed successfully, including C/C++ CMake/Meson, Go, Python, Ruby,
  documentation, and clang-tidy jobs. With the earlier focused PostgreSQL and
  Redshift, Java, C#, Rust, Integration, Native Windows, vcpkg, and Dev passes,
  the Apache-main rebase checkpoint is complete. This qualifies the current
  development baseline, not production binaries or untested Redshift features.
- 2026-09-13: After qualifying the five-platform development-package workflow,
  removed the duplicate Ubuntu archive/upload steps from the PostgreSQL and
  live-Redshift workflow. That workflow retains its installed-client checks;
  the isolated package workflow alone now owns downloadable artifacts. The
  workflow edit passes local Actions syntax validation and does not change
  driver code; the previous full driver run remains the live-behavior evidence.
- 2026-09-13: Corrected package workflow `34750754477` passed all five build,
  extracted-client, and upload jobs plus a final job that downloaded every
  archive/checksum pair and verified each SHA-256. Separately downloaded the
  Debian x86-64 and Ubuntu ARM64 replacements, verified their checksums, and
  confirmed their ELF architectures. Removed only the four older Linux
  artifacts known to omit checksums (three from `34750508662` and one from the
  earlier driver workflow); the corrected replacements remain available.
  This still does not qualify packaged binaries against live Redshift or make
  a production release claim.
- 2026-09-13: First Linux-matrix package run `34750508662` passed its build,
  checksum, extracted-client, and upload steps on Ubuntu x86-64/ARM64 and
  Debian x86-64, and both macOS jobs also passed. A separate download audit
  found the containerized Linux artifacts omitted the adjacent `.sha256` file:
  `runner.temp` resolved to a host path while the shell wrote under the
  container's `$RUNNER_TEMP`. These Linux downloads are incomplete and must
  not be treated as qualified. The workflow is being corrected to upload from
  a workspace-relative directory and to download/verify all five artifact
  pairs in a final CI job.
- 2026-09-13: Published the third Apache-facing branch,
  `feature/pgwire-libpq-stream-upstream`, stacked on the cancel-handle branch.
  Its added commit `c45517428` changes only PostgreSQL statement COPY-stream
  result ownership to RAII; both PostgreSQL C++ suites and the pinned
  formatting check passed locally. The parent cancel-handle branch passed
  Dev, Native Unix/Windows/vcpkg, Integration, and Rust fork workflows. The
  third branch's own CI remains pending; no Apache PR has been opened.
- 2026-09-13: Package workflow `34749872614` passed for Ubuntu 24.04 x86-64,
  macOS Intel, and macOS Apple Silicon. Each job built the driver and manager,
  verified a SHA-256 checksum, extracted the archive to a new prefix, compiled
  an independent client against that prefix, loaded the driver, and uploaded
  a seven-day artifact. Both macOS artifacts were downloaded separately;
  their checksums passed and their Redshift libraries were confirmed as
  x86-64 and arm64 Mach-O binaries respectively. No packaged macOS binary has
  yet been connected to live Redshift, and no production release is claimed.
- 2026-09-13: Focused driver workflow `34749295187` passed after a retry of
  an initial Actions startup error: PostgreSQL 18 checks passed on
  Ubuntu x86-64/ARM64, Debian x86-64, and macOS Intel/Apple Silicon. The
  Ubuntu job also packaged and reloaded its extracted development archive.
  All 14 live Redshift tests and the independently installed client connection
  passed; the exact temporary runner-ingress cleanup step completed
  successfully. The separate, smaller Release-built Ubuntu archive remains
  the preferred evaluation artifact. The analogous macOS Intel and Apple
  Silicon archive jobs were qualified in isolated package workflow
  `34749872614`.
- 2026-09-13: Isolated Ubuntu package workflow `34749412874` passed. It built
  the Redshift driver and manager, verified the archive checksum, extracted to
  a second prefix, checked package contents and pkg-config relocation, compiled
  an independent client against the extracted headers/library, and loaded the
  Redshift driver. The seven-day CI artifact was downloaded separately and its
  checksum and key contents verified again. The same packaging and extracted
  client sequence passed locally on macOS Apple Silicon, using that platform's
  `.dylib` output. This is a development archive, not a production release or
  live Redshift qualification for the packaged binary. The earlier manual
  workflow run `34748813873` remained queued with zero planned jobs; a later
  focused push run encountered an Actions startup error before retry.
- 2026-09-13: Rebased-core vcpkg workflow `34747963136` passed x64 Debug,
  x64 Release, and native ARM64 Release, including installed-driver checks.
  The same rebased commit passed the repository-wide Integration, C#, Rust,
  Dev/pre-commit, Native Windows, Java, and focused PostgreSQL/Redshift workflows.
  Native Unix completed later in workflow `34749280271`.
  The AWS dashboard showed $298.08 of $300.00 Redshift trial
  credit remaining through 2026-12-11; no RPU-hour limit or alarms are
  configured, and no AWS setting was changed during this inspection.
- 2026-09-13: Published stacked `feature/pgwire-libpq-cancel-upstream` from the
  Apache-ready RAII branch. Its only additional patch, `dccef94bc`, changes
  `PGcancel` ownership in the PostgreSQL connection to the same RAII helper;
  it does not include Redshift behavior or the project roadmap. The pinned
  clang-format check, local build, and both PostgreSQL C++ suites passed.
  The rebased core branch's workflow `34747963110` also passed all five
  PostgreSQL 18 platform jobs and all 14 live Redshift tests, including the
  stronger persisted-data commit/rollback assertion; AWS runner ingress was
  revoked. Its Dev/pre-commit workflow `34747963180` passed. Windows and
  broader downstream jobs from that push are still running.
- 2026-09-13: All three Windows vcpkg jobs for workflow `34747059182`
  completed successfully: x64 Debug, x64 Release, and native ARM64 Release.
  Their installed Redshift DLL/client checks passed; this is still not a
  published binary release or a live Windows-to-Redshift test. Rebased the
  67-commit development series onto Apache main `4d50e2e30`; `git range-diff`
  reports every project patch unchanged. The four intervening Apache commits
  only update Go, Java, and C# dependencies. A clean local build and all three
  PostgreSQL/Redshift artifact C++ suites passed on the rebased checkout.
  Strengthened the live Redshift transaction test to check the persisted row
  count after commit and rollback; live CI qualification is pending.
- 2026-09-13: Added a live `GetObjects` column-filter assertion to the Redshift
  metadata test. Workflow `34747059209` passed all five PostgreSQL 18 platform
  jobs, the installed-client checks, and all 14 focused Redshift tests. Its
  temporary runner ingress rule was revoked. The same commit passed local
  macOS Apple Silicon PostgreSQL and Redshift unit suites with ASan/UBSan.
  Applied the repository's pinned clang-format version and the missing
  `<utility>` include; the Apache-ready RAII branch's Dev/pre-commit workflow
  `34746962492` passed. The core branch's repository-wide Dev workflow
  `34747059190` failed only in C# `dotnet format` dependency restore, outside
  the C++ driver checks; investigate it separately when rebasing or preparing
  a broader PR.
- 2026-09-13: Built both PostgreSQL and Redshift C++ drivers locally with
  AddressSanitizer and UndefinedBehaviorSanitizer enabled on macOS Apple Silicon.
  The PostgreSQL driver and binary-COPY suites passed against local PostgreSQL,
  and the Redshift artifact/unit suite passed without a live AWS connection.
  This is an additional local memory/undefined-behavior check, not a substitute
  for sanitizer-qualified live Redshift or all-platform release testing.
- 2026-09-13: Created `feature/pgwire-libpq-raii-upstream` directly from
  `upstream/main` with only the libpq RAII/result-helper cleanup (commit
  `d8dc6a7c9`), leaving the project roadmap and all Redshift code out of the
  Apache-facing diff. A clean local macOS Apple Silicon CMake build and both
  PostgreSQL driver C++ test suites passed against local PostgreSQL. Pushed the
  branch to the `vahid110/arrow-adbc` fork; no upstream PR has been opened.
- 2026-09-13: Corrected batch-size code passed full PostgreSQL 18 and live
  Redshift workflow run `34745845306`, including 14 focused live cases and
  the two-Arrow-batch ingest test. Native Windows vcpkg Release checks on x64
  and ARM64 plus x64 Debug passed in run `34745845370`. A read-only Redshift
  catalog probe found
  `GEOMETRY` OID 3000, `GEOGRAPHY` OID 3001, and `SUPER` OID 4000. Direct
  libpq/Arrow value behavior is still unqualified, so no mapping claim is
  added yet.
- 2026-09-13: The native Windows vcpkg Release build exposed Windows' `min`
  macro expanding the new batch-size cap. Parenthesized the `std::min` call;
  local build passes and Windows requalification is pending. The concurrently
  running push checks may be superseded by this correction.
- 2026-09-13: Implemented a bounded 16-row parameterized INSERT path selected
  by Redshift's backend capability; PostgreSQL retains its binary `COPY`
  path. The driver caps each SQL statement at 32,767 parameters and preserves
  one transaction across the entire Arrow bind. The focused live tests now
  span a full and partial batch and put an error in the second batch to check
  rollback. Live CI run `34745172344` passed all platform and Redshift gates;
  manual benchmark run `34745478491` passed and measured 85.069 rows/second
  versus the 5.374 rows/second row-at-a-time baseline. A follow-up local test
  now splits 18 rows across two Arrow batches, crossing both stream and SQL
  batch boundaries;
  its live verification is pending.
- 2026-09-13: Manual benchmark CI run `34744399423` passed all PostgreSQL and
  live Redshift gates. Its one-shot prepared-insert measurement was 1,000 rows
  in 186.081 seconds (5.374 rows/second), with the exact database row count
  checked. A two-row S3 `COPY` succeeded only after removing the exact
  `aws:SourceArn` trust condition; its count query returned 2. The object was
  deleted and the original trust condition restored. This proves feasibility
  but does not yet establish a safe permanent role trust or an ADBC uploader.
- 2026-09-13: Push-triggered CI run `34744044155` passed PostgreSQL 18 and
  installed-client checks on Ubuntu x86-64/ARM64, Debian x86-64, and macOS
  Intel/Apple Silicon, followed by the focused live Redshift suite. Added a
  local regression test for empty Redshift query results retaining their Arrow
  schema; local build passes and live verification is pending. Refreshed Apache
  `main`: it is four commits ahead of the pinned baseline, limited to Go, Java,
  and C# dependency updates; defer a history rewrite until the active CI run
  completes.
- 2026-09-13: Added a manually dispatched, one-shot Redshift prepared-insert
  benchmark. It prepares 1,000 Arrow integer rows, times bind plus ingest,
  checks both reported and queried row counts, and drops its uniquely named
  table in fixture teardown. Local build and database-independent focused tests
  pass; the measured live result is pending. The full local PostgreSQL suite
  could not run without `ADBC_POSTGRESQL_TEST_URI` and remains a CI gate.
- 2026-09-09: Created the local fork and development branch from Apache `main`.
- 2026-09-09: Added this tracked implementation plan before changing driver code.
- 2026-09-09: Built both PostgreSQL test executables against libpq 18.6. The 22
  database-independent COPY reader tests pass. Database-backed tests require an
  `ADBC_POSTGRESQL_TEST_URI` and are pending a local test service.
- 2026-09-09: Began extracting backend-neutral libpq RAII handles and added
  database-independent ownership tests.
- 2026-09-09: Migrated `PqResultHelper` to the reusable result handle. Both
  PostgreSQL test binaries compile; the focused error, ownership, and type suite
  passes 9/9 tests.
- 2026-09-09: Added immutable PostgreSQL and conservative Redshift capability
  profiles. PostgreSQL vendor identity now comes from the composed profile without
  changing its public value.
- 2026-09-09: Published the development branch to `vahid110/arrow-adbc`, retained
  Apache as the `upstream` remote, enabled GitHub Actions, and added a focused C++
  integration workflow backed by PostgreSQL 18.
- 2026-09-09: GitHub Actions run `34384221476` built both PostgreSQL test binaries
  and passed the complete `driver-postgresql` CTest label against PostgreSQL 18.
- 2026-09-12: Created the `pgwire-ci` Redshift Serverless workgroup and namespace
  in Frankfurt with base and maximum capacity fixed at 4 RPUs. Recorded the
  cost-control rules above before enabling Redshift integration tests.
- 2026-09-12: Migrated PostgreSQL connection cancellation ownership to the common
  pgwire RAII handle, removing the remaining manual `PGcancel` cleanup path.
- 2026-09-12: Migrated streaming query result ownership to the common pgwire RAII
  handle and kept response draining explicit, eliminating another manual cleanup
  path without changing COPY behavior.
- 2026-09-12: Added an explicit query-result transport selector. PostgreSQL keeps
  binary COPY when enabled, while capability-limited backends such as Redshift
  select the portable text-result path.
- 2026-09-12: Added one-time Redshift detection from `SELECT version()` and parse
  the vendor version using the selected backend profile. Unknown PostgreSQL-wire
  servers retain the PostgreSQL-compatible default.
- 2026-09-12: GitHub Actions run `34686322856` passed the complete PostgreSQL 18
  integration suite after the result-transport and backend-detection refactors.
- 2026-09-12: Made type-catalog discovery capability-aware for Redshift's missing
  `pg_type.typarray`, used the cached Redshift vendor version for `GetInfo`, and
  disabled unsupported constraint discovery without affecting PostgreSQL SQL.
- 2026-09-12: Added an opt-in Redshift smoke test, gated by
  `ADBC_REDSHIFT_TEST_URI`, covering database initialization, vendor version
  reporting, text-result execution, schema mapping, and scalar values.
- 2026-09-12: Added capability-driven bulk-ingest selection and explicit
  `NOT_IMPLEMENTED` results for Redshift ingestion and statistics, preventing
  accidental execution of unsupported PostgreSQL-specific paths.
- 2026-09-12: GitHub Actions runs `34686600361`, `34686724734`, and `34686810469`
  passed the complete PostgreSQL 18 suite for catalog adaptation, Redshift smoke
  test integration, and unsupported-path guards respectively.
- 2026-09-12: Generated a dedicated Redshift admin credential and stored the
  connection URI only as the encrypted GitHub Actions secret
  `ADBC_REDSHIFT_TEST_URI`; no credential value is stored in the repository.
- 2026-09-12: Restricted the Redshift security group to TCP 5439 from the current
  development address as a `/32`. Public endpoint activation is limited by that
  rule; broad public ingress is not permitted.
- 2026-09-12: Enabled the public endpoint behind the restricted security group and
  authenticated over required SSL. The live server reported Redshift
  `1.0.436211`.
- 2026-09-12: Expanded the opt-in live suite to five tests covering twelve core
  scalar mappings, value decoding, `GetInfo`, Redshift-specific table types,
  `GetObjects`, `GetTableSchema`, and explicit commit/rollback. All five tests
  passed; their temporary table was dropped and all test connections were closed.
- 2026-09-12: Moved table-type names and `pg_class.relkind` mappings into the
  backend profile. PostgreSQL retains its six existing table types while Redshift
  reports only the verified `table` and `view` types.
- 2026-09-12: Verified a bound `INT32` parameter and binary result round trip on
  Redshift. Marked binary parameters supported while keeping binary `COPY` query
  transport disabled; these are separate protocol capabilities.
- 2026-09-12: Added the first backend-owned type discovery alias, mapping
  Redshift's `varbyte_recv` identity to the core binary representation. A live
  `VARBYTE` query returned the expected Arrow binary schema and bytes.
- 2026-09-12: Added capability-selected, correctness-first Redshift ingestion via
  prepared binary parameters. PostgreSQL retains binary `COPY`; the Redshift path
  creates or appends through the same ADBC ingest mechanics and counts affected
  rows without adding vendor conditionals to statement execution.
- 2026-09-12: Wrapped each autocommit Redshift Arrow batch in one transaction so a
  later row failure cannot leave earlier rows committed. Live tests verified both
  a successful two-row create ingest and full rollback when the second appended
  row violated a `NOT NULL` constraint. The complete eight-test Redshift suite
  passed, and all temporary tables and connections were cleaned up.
- 2026-09-12: Completed libpq lifetime extraction across the PostgreSQL-wire
  implementation. Connection, cancellation, prepared/query results, transaction
  commands, table DDL, and COPY setup/completion now use the shared scoped handles;
  no manual `PQfinish`, `PQfreeCancel`, or `PQclear` calls remain in the driver.
- 2026-09-12: Added backend-owned transaction semantics. PostgreSQL retains
  session-configurable ADBC isolation levels and transactional DDL; Redshift is
  marked as database-configured and non-transactional for DDL. Because AWS marks
  `SET SESSION CHARACTERISTICS` as deprecated for Redshift, non-default ADBC
  isolation overrides now fail explicitly instead of reporting a misleading
  success; default isolation remains accepted. Live commit, rollback, and
  isolation-policy tests passed.
- 2026-09-12: Refactored driver initialization around one common ADBC function-table
  builder and distinct PostgreSQL and Redshift database factories. The public
  `AdbcDriverPostgresqlInit` behavior remains compatible and the new
  `AdbcDriverRedshiftInit` pins Redshift semantics, rejecting a mismatched server.
  The live Redshift connection test now runs through the named Redshift entry point.
- 2026-09-12: Added independently installable shared and static Redshift artifacts,
  `libadbc_driver_redshift`, whose standard `AdbcDriverInit` selects the Redshift
  factory. Added a public Redshift entry-point header, CMake package metadata,
  pkg-config metadata, an artifact-level test, and CI coverage. A temporary-prefix
  install verified every library, header, and package metadata output.
- 2026-09-12: Extracted type-catalog discovery from database lifecycle code into a
  focused component with capability-driven PostgreSQL and Redshift query plans.
  Catalog parsing feeds the existing type resolver, Arrow schema mapping remains
  in the type model, and wire value decoding/encoding remains in result and bind
  paths. Focused query-plan tests and a live Redshift scalar-mapping test passed.
- 2026-09-12: Moved catalog, schema, table, column, constraint, table-schema, and
  table-type SQL construction behind a normalized metadata query provider. The
  provider consumes only the backend profile, so core `GetObjects` iteration no
  longer owns backend SQL or table-kind mapping. Focused provider tests and live
  Redshift `GetObjects`, `GetTableSchema`, and `GetTableTypes` tests passed.
- 2026-09-12: Completed the final regression pass. All nine live Redshift tests
  passed through the named Redshift driver, including query, types, metadata,
  transactions, parameter binding, successful ingest, and ingest rollback. GitHub
  Actions run `34690357323` built both driver artifacts, passed the complete
  PostgreSQL 18 integration suite, verified that the Redshift driver rejects a
  PostgreSQL server, and passed the Redshift artifact-entry-point test.
- 2026-09-12: Registered GitHub's OIDC endpoint in AWS and created the
  `adbc-redshift-ci` role. Its trust policy requires the immutable
  `vahid110@5770674/arrow-adbc@1363017696` subject on
  `feature/pgwire-core-redshift`; its only resource permissions are authorizing
  and revoking ingress on `sg-00551344e2efb9c68`. Added a post-PostgreSQL live CI
  job that keeps the database port open only for the focused test interval and
  always removes the runner rule afterward.
- 2026-09-12: GitHub Actions run `34691303802` passed the complete PostgreSQL 18
  gate and all nine live Redshift tests using OIDC credentials. The temporary
  runner ingress rule was revoked successfully; an independent AWS console check
  confirmed that only the pre-existing default-group and development `/32` rules
  remained.
- 2026-09-12: Restored CMake/Meson parity after the core extraction. Meson now
  compiles the extracted metadata and type-discovery components into both drivers,
  produces and installs distinct PostgreSQL and Redshift shared libraries, public
  headers, and pkg-config files, and runs the common seam tests plus the Redshift
  artifact-entry-point test. A fresh Meson build completed all 84 targets; the 14
  database-independent core tests and the Redshift artifact test passed.
- 2026-09-12: Removed the remaining backend-name branches from common version and
  `GetInfo` execution. Each backend profile now owns its version prefix, version
  source policy, and public driver name; Redshift consequently identifies the
  driver as `ADBC Redshift Driver` instead of inheriting PostgreSQL's name.
- 2026-09-12: The broad GLib/Ruby integration gate exposed a PostgreSQL
  compatibility detail not asserted by the C++ suite: `GetTableTypes` ordering.
  Restored Apache's historical order in the PostgreSQL profile and added an exact
  provider test while leaving Redshift's verified `table`, `view` order unchanged.
- 2026-09-12: Completed the first production-readiness checkpoint. A standalone C
  client compiled only against a clean installed prefix and loaded the Redshift
  shared library through the ADBC driver manager. GitHub Actions run
  `34709216855` verified the installed public header, pkg-config/CMake metadata,
  and rejection of a PostgreSQL server; the full PostgreSQL 18 and nine-test live
  Redshift suites passed, and the temporary AWS ingress rule was revoked.
- 2026-09-12: Started correctness hardening with live tests for nullable result
  columns and statement/connection recovery after a SQL error. GitHub Actions
  run `34709484319` passed PostgreSQL 18, the installed-client smoke check, and
  all eleven focused live Redshift tests. The temporary AWS ingress rule was
  revoked. Cancellation, larger results, and further type/metadata edge cases
  remain open.
- 2026-09-12: Extended the PostgreSQL 18 and installed-client gates to Debian
  Bookworm x86-64 and native Ubuntu 24.04 ARM64. GitHub Actions run `34709694015`
  passed all three Linux platform jobs, the eleven-test live Redshift suite, and
  the AWS ingress cleanup. These are source/install test results, not published
  binary-package or cross-platform live Redshift guarantees.
- 2026-09-12: GitHub Actions run `34709927476` passed the eleven-test live
  Redshift suite and then opened a real Redshift connection through the ADBC
  driver manager and independently installed Redshift shared library on Ubuntu
  x86-64. The same run passed Ubuntu ARM64 and Debian x86-64 PostgreSQL/install
  gates, and revoked the temporary AWS ingress rule.
- 2026-09-12: Added macOS Intel and Apple Silicon PostgreSQL 18 plus clean-prefix
  Redshift library-loading jobs. The initial Apple Silicon run found a CI-only
  libpq discovery gap; using Homebrew's separate `libpq` package corrected it.
  GitHub Actions run `34710421526` passed both macOS jobs, all three Linux
  platform jobs, the eleven-test live Redshift suite, and AWS ingress cleanup.
  Windows installation and macOS live Redshift-client qualification remain open.
- 2026-09-12: Added a bounded 1,024-row recursive-query test for complete Arrow
  result delivery and end-of-stream behavior. GitHub Actions run `34710970303`
  passed all five Linux/macOS platform jobs, twelve focused live Redshift tests,
  the installed-client live connection, and temporary AWS ingress cleanup.
  Cancellation and remaining type/metadata edge cases are still open.
- 2026-09-12: Added a missing-table `GetTableSchema` check to the live Redshift
  suite. GitHub Actions run `34712196347` passed all five Linux/macOS platform
  jobs, thirteen focused live Redshift tests, the installed-client connection,
  and temporary AWS ingress cleanup.
- 2026-09-12: Added a standalone driver-manager load-only mode and a small
  installed-package CMake client. GitHub Actions run `34712196368` built that
  client against the installed public CMake package and loaded the Redshift DLL
  on Windows x64 Debug, x64 Release, and ARM64 Release; all three vcpkg jobs
  passed. This proves installed artifact loading, not a distributable package or
  live Windows-to-Redshift behavior.
- 2026-09-12: Locally installed the CMake build to a different prefix and built
  the standalone client with `find_package(AdbcDriverManager)` from that
  relocated prefix. It loaded the installed Redshift library successfully;
  cross-platform relocation remains a release-packaging test item.
