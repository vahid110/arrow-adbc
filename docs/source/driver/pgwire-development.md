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
- [ ] Address remaining replace-ingest failures after destructive DDL, including
      zero fields, duplicate names, SQL `CREATE` errors, and later transfer
      failures. Name and Arrow type preflight are narrow completed steps.
- [ ] Complete timezone-aware bound-query cleanup for output-schema failures,
      cleanup SQL failures, and concurrent connection use. Post-export decoder
      errors now terminalize the bound stream and clean up the timezone state.
      Timezone-setup, typed-prepare, and bound-row SQL errors have scoped
      autocommit rollback; synchronous bind/writer errors in healthy explicit
      transactions restore the prior timezone without committing. Early bound
      stream release has connection-lifetime-guarded, one-shot cleanup and a
      per-connection active-stream lease that rejects competing ADBC SQL and
      transaction changes. Failed rollback, timezone restore, or COMMIT now
      marks the connection unusable; callers must close and recreate it rather
      than risk using uncertain transaction/session state. PostgreSQL backend-
      loss tests cover failed rollback and end-of-stream cleanup. Output-schema
      failure injection, healthy-connection cleanup-SQL failure, and concurrent
      connection use still need qualification before a general production-safe
      claim.
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
- [x] Resolve the scoped IAM role-assumption failure for the dedicated non-root
      test identity; the two-row fixture succeeded with exact IAMR External ID
      trust. This does not enable staged ingestion in the driver.
- [x] Add a Redshift-private, AWS-free exact-object staging coordinator with
      failure-path tests, without selecting it from the active ingest path.
- [x] Add a conservative one-batch Arrow-to-CSV preparation and compose it with
      the staging coordinator behind an AWS-free, Redshift-private seam; leave
      active ingestion on the existing prepared-INSERT path.
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
- [x] Publish a fifth standalone Apache-facing PostgreSQL `GetObjects` fix:
      column filtering retains table constraints, with a focused regression
      test and no Redshift-specific code. This is a fork branch, not an Apache PR.
- [x] Publish a sixth isolated PostgreSQL timestamp range fix from local
      `upstream/main`: use checked epoch arithmetic and a non-retryable range
      error, with an offline boundary regression. This is a fork branch, not an
      Apache PR.
- [x] Publish a seventh isolated PostgreSQL array-shape guard from local
      `upstream/main`: report unsupported multidimensional arrays instead of
      silently flattening them. This is a fork branch, not an Apache PR.
- [x] Publish an eighth isolated PostgreSQL JSONB binary-validation fix from
      local `upstream/main`: reject unknown versions and zero-length non-null
      fields without consuming another field. This is a fork branch, not an
      Apache PR.
- [x] Publish a ninth isolated PostgreSQL binary-COPY field-bounds fix from
      local `upstream/main`: reject truncated tuple, nested-record, and array
      element payloads before primitive decoding. This is a fork branch, not
      an Apache PR.
- [x] Publish a tenth isolated PostgreSQL binary-COPY header fix from local
      `upstream/main`: reject unsupported critical format flags while accepting
      advisory flags and bounded extensions. This is a fork branch, not an
      Apache PR.
- [x] Publish an eleventh PostgreSQL binary-COPY field-framing fix stacked on
      the isolated field-bounds branch: constrain tuple, record, and array
      children to their declared byte ranges, including exact consumption.
      This is a fork branch, not an Apache PR.
- [x] Publish a twelfth standalone PostgreSQL binary-COPY trailer fix from
      local `upstream/main`: reject trailing data and missing or truncated
      trailers while retaining server-error precedence. This is a fork
      branch, not an Apache PR.
- [x] Publish a thirteenth standalone PostgreSQL binary-COPY output-schema
      fix from local `upstream/main`: validate the supplied Arrow schema and
      retain caller ownership on rejection. This is a fork branch, not an
      Apache PR.
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
| macOS Apple Silicon | CMake and Meson builds, including a Meson ASan/UBSan Redshift artifact suite; PostgreSQL 18 suite and clean-prefix manager/client smoke; checked arm64 development archive with extracted-client PostgreSQL 18 query and Redshift-driver PostgreSQL rejection | Production release artifact and live Redshift installed-client test |
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
- [x] Harden the opt-in COPY fixture's temporary CI ingress cleanup for an
  ambiguous authorize or revoke response. It marks an attempt before calling
  AWS and reconciles only a rule matching this run's unique description,
  group, port, protocol, and CIDR; an exact returned rule ID is a fallback.
  Never revoke a pre-existing rule by CIDR. The branch-scoped CI role's
  separate `RedshiftCiIngressInspect` policy grants read-only
  [`ec2:DescribeSecurityGroupRules`](https://docs.aws.amazon.com/cli/latest/reference/ec2/describe-security-group-rules.html)
  with `Resource: "*"` and `aws:RequestedRegion=eu-central-1`. IAM simulation
  allowed the Frankfurt request and denied `us-east-1`. Twenty-three COPY
  fixture mocks and eleven ordinary-ingress mocks passed; an earlier live
  two-row run cleaned its rule before these latest changes. An ambiguous
  revoke response now stays a visible audit failure, even when an eventually
  consistent `DescribeSecurityGroupRules` reports an empty list. A lost runner
  can still bypass all in-job cleanup, so manually audit each opt-in run;
  automatic push-triggered COPY remains gated on independent reconciliation.
- [x] Make S3 fixture uploads conditional (`If-None-Match: *`) and cleanup
  conditional on the ETag from a confirmed successful upload. Collision,
  missing ETag, or a lost response leaves an unconfirmed exact key untouched
  for independent audit; the one-day lifecycle is only a backstop. Sixteen
  offline safety mocks passed. An ETag cannot distinguish a same-content
  replacement, so unique per-run keys and independent audit remain necessary.
  AWS requires `s3:GetObject` in addition to `s3:DeleteObject` for
  ETag-matched deletion, so a fail-before-AWS runtime
  gate now blocks live COPY until a narrowly scoped grant is reviewed and the
  workflow deliberately sets `PGWIRE_COPY_GETOBJECT_VERIFIED=true` after
  verification. This does not grant new IAM permission.
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
  exact objects in a `finally`/cleanup path; lifecycle is a backstop. A
  read-only `GetBucketVersioning` check on 2026-09-13 returned no versioning
  configuration. If versioning is enabled later, the future adapter must
  capture/delete exact object versions rather than treating a delete marker
  as byte cleanup.
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

The AWS checkpoint is a *manual*, two-row non-root test, not an automatic
push job. A manual, credential-only OIDC run `34767827101` verified
that the branch-scoped GitHub role `adbc-redshift-ci` receives the database
username `IAMR:adbc-redshift-ci`, without opening ingress or querying Redshift.
Its separate inline policy `RedshiftCiCopyFixture` grants only
`redshift-serverless:GetCredentials` on the actual `pgwire-ci` workgroup ARN
and `s3:PutObject`/`s3:DeleteObject` under `staging/ci/*`. An IAM simulation
allowed those exact resources and denied a sibling staging prefix. The COPY
role's trust now uses the exact Serverless database-user `sts:ExternalId`
alongside `aws:SourceAccount` and both Redshift service principals; its S3
policy remains read-only. Manual run
[`34770466003`](https://github.com/vahid110/arrow-adbc/actions/runs/34770466003)
verified that exact non-root database identity and successfully loaded the two
expected rows with staged `COPY`. This qualifies the fixture and trust, not a
driver ingest path.

The manual COPY qualification must first check the actual connected user and
database `ASSUMEROLE` privilege. Use unique run-specific data and manifest
keys, require `mandatory: true`, verify exactly two rows, and delete only
confirmed-owned exact objects in failure-safe cleanup. The one-day lifecycle
remains a backstop, not the normal cleanup path. Isolate the non-canceling CI
run from push-driven jobs so a push cannot interrupt cleanup. Ownership-safe
recovery uses the regional read-only `ec2:DescribeSecurityGroupRules` grant
described above. After each manual run, independently inspect the security
group for its unique `adbc-pgwire-copy-<run-id>-<attempt>` rule description;
a force-canceled job or lost runner can bypass in-job cleanup, and no ingress
TTL exists. Remove only a verified rule ID belonging to that run if one remains.
A future push-triggered COPY gate would need automatic independent
reconciliation.

The optional driver ingest path remains unimplemented. Its first internal
coordinator accepts caller-supplied serialized data, an exact mandatory manifest,
and a tiny conditional object-store interface. It never deletes a collided or
unconfirmed object. A lost response or unsettled COPY can require independent
owned-object reconciliation; `cleanup_complete` is reported separately from
whether COPY succeeded. The coordinator is not selected by `ExecuteIngest`.
An AWS-free Redshift-private Arrow-to-CSV writer now serializes only non-null
INT32, INT64, and validated UTF-8 strings, with an exact ordered field match
and an 8 MiB payload cap plus a conservative 4,000,000-byte serialized row cap
below Redshift's documented 4 MB COPY row limit. It rejects literal `\\N`
pending a live check of quoted null-marker behavior. A separate, private
ArrowArrayStream preflight combines at most 128 batches into one bounded CSV
payload, validates even empty batches, and releases the stream before calling
the existing coordinator once. Neither adapter is selected by `ExecuteIngest`.
Before enabling staged ingestion, qualify null versus empty string and the
remaining Arrow type mappings, an optional S3 adapter with short-lived
credentials, an explicit transaction boundary, and exact cleanup. AWS requires
`s3:GetObject` for
[`HeadObject`](https://docs.aws.amazon.com/AmazonS3/latest/API/API_HeadObject.html)
and in addition to `s3:DeleteObject` for an
[ETag-matched conditional `DeleteObject`](https://docs.aws.amazon.com/AmazonS3/latest/userguide/conditional-deletes.html).
Without `s3:ListBucket`, `HeadObject` cannot distinguish a nonexistent key
from an inaccessible one by status alone (403 instead of 404), so ambiguous
uploads still need independent reconciliation. The branch-scoped uploader
role does not have `s3:GetObject`; granting it would also permit reading the
exact objects, not merely checking or deleting them. No such IAM expansion
or SDK dependency has been added. Do not dispatch the ETag-conditional manual
fixture until a narrow
`s3:GetObject` grant for `staging/ci/*` is explicitly approved and verified;
keep automatic COPY disabled until a lost runner can also be reconciled
independently.

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
The dedicated IAMR role trust and uploader permissions are narrowly configured
and live-qualified by a non-root two-row `COPY` fixture, with independent
cleanup checks. A private, offline-tested preparation helper builds a mandatory
exact-object manifest and validates `COPY` SQL with an explicit, ordered ingest
column list. An AWS-free staging coordinator tests conditional creation and
owned-object cleanup. A conservative CSV writer covers three non-null Arrow
types, and a one-batch adapter composes it with the coordinator while owning
the CSV buffer. A bounded multi-batch stream preflight now rejects malformed
or oversized input before any object-store call; none of these private seams
is selected by the active driver ingest path. Thirteen
PostgreSQL-only fixes are published on separate Apache-facing fork branches.
Current work is production hardening and platform qualification through
short-lived evaluation archives.

Ordinary live Redshift CI now marks each temporary runner `/32` ingress rule
with a run-specific `adbc-pgwire-live-<run-id>-<attempt>` description. It refuses
to alter a pre-existing exact-CIDR rule, saves the authorization attempt before
the AWS call, and revokes only a verified owned rule ID even if the authorize
response is lost. Cleanup runs after any successful AWS credential step and
reports unresolved ambiguity as a failure, including a lost revoke response
followed by a stale empty read. A lost runner or force-canceled job
can still bypass in-job cleanup, so independent post-run rule inspection remains
necessary; this is not a claim of automatic orphan reconciliation.

## Progress log

- 2026-09-14: Bound-result finalization is now one-shot. A rollback, timezone
  restore, or COMMIT failure marks the ADBC connection unusable while preserving
  the primary bind/decode error; subsequent SQL, transaction, schema, metadata,
  and option operations refuse to reuse it, and connection release remains
  allowed. A failed timezone-setup rollback is tracked even before the bind is
  marked timezone-aware. Two PostgreSQL 17 backend-loss tests cover failed
  rollback after an intermediate bound row and failed restore/COMMIT at stream
  exhaustion. Both full PostgreSQL C++ suites, the AWS-free Redshift suite,
  shared/static PostgreSQL and Redshift builds, and three focused ASan/UBSan
  tests passed locally. No AWS resources were started. The disposable local
  PostgreSQL cluster was stopped and removed. Cross-platform CI is pending.
- 2026-09-14: Post-export Arrow decoding errors now finalize a timezone-aware
  bound stream immediately: discard remaining bound rows, clear the current
  result, and roll back only a driver-owned autocommit transaction; a healthy
  explicit transaction keeps its work and has its prior timezone restored.
  A PostgreSQL 17 test executes one `INSERT ... RETURNING` row whose interval
  overflows Arrow decoding, confirms the original decoder error, a terminal
  stream, zero committed rows in autocommit, and caller-controlled rollback
  in explicit mode. Both full PostgreSQL C++ suites, three focused ASan/UBSan
  tests, shared/static PostgreSQL and Redshift builds, and the AWS-free
  Redshift artifact suite passed locally. The
  [five-platform PostgreSQL 18 matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34810617157)
  passed with all AWS-backed jobs skipped; output-schema and cleanup-SQL
  failures remain separate gaps. The disposable PostgreSQL cluster was
  stopped and removed.
- 2026-09-14: An unfinished bound result stream now holds a weak reference to
  its ADBC connection and a per-connection active-stream lease. On ordinary
  early release it rolls back a driver-owned autocommit transaction, or
  restores the prior timezone without ending a healthy caller-owned
  transaction. The lease prevents another ADBC query, schema/parameter
  discovery, commit, rollback, or option change from replacing that
  transaction while the stream is unfinished. After explicit connection
  close, `get_next` and a retained statement's execute report an error and
  stream release skips the stale libpq pointer. PostgreSQL 17 tests cover
  release before and after reading the first batch, explicit transaction
  ownership and competing-operation rejection, and connection close before
  stream release. Both full PostgreSQL C++ suites, three focused ASan/UBSan
  tests, shared/static PostgreSQL and Redshift builds, and the AWS-free
  Redshift artifact suite passed locally. The
  [five-platform PostgreSQL 18 matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34810102696)
  passed with all AWS-backed jobs skipped. Cleanup SQL failures and concurrent
  connection use still need separate qualification; post-export decoder errors
  are addressed in the subsequent checkpoint above. The disposable
  PostgreSQL cluster was stopped and removed. The same-head
  [seven-platform development-package run](https://github.com/vahid110/arrow-adbc/actions/runs/34810116274)
  passed all extracted-client archives and the final downloaded-checksum gate;
  this is not a production release.
- 2026-09-14: A synchronous Arrow/binary-writer bind error in a healthy
  explicit transaction now restores the caller's timezone, leaves that
  transaction open, and terminalizes the failed bind. A PostgreSQL 17 test
  binds an overflowing UTC timestamp in both no-output and output modes:
  the original overflow error is retained, the transaction remains healthy,
  its uncommitted marker row is still visible, and only the caller's later
  rollback discards that row. Both full PostgreSQL C++ suites, three focused
  ASan/UBSan tests, shared/static PostgreSQL and Redshift builds, and the
  AWS-free Redshift artifact suite passed locally. The
  [five-platform PostgreSQL 18 matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34808790422)
  passed with all AWS-backed jobs skipped. Post-export decoding remained open;
  ordinary early release is addressed in the subsequent checkpoint above.
- 2026-09-14: If timezone lookup or UTC setup fails after the driver opens an
  autocommit transaction, the setup path now rolls it back while preserving the
  original error. A PostgreSQL 17 regression shadows `current_setting(text)`
  to inject failure after `BEGIN`; both no-output and output execution return
  the marker error, leave the connection idle, preserve `Europe/Berlin`, and
  allow a subsequent query. The full PostgreSQL driver and binary-COPY C++
  suites passed, including 107 statement cases and six expected skips; three
  focused ASan/UBSan tests passed. Shared/static PostgreSQL and Redshift
  artifacts built and the AWS-free Redshift artifact suite passed. Cross-
  platform CI for this narrow fix
  [passed on PostgreSQL 18 across five platforms](https://github.com/vahid110/arrow-adbc/actions/runs/34808166092)
  with AWS jobs skipped; the remaining cleanup paths are tracked above. The
  disposable local PostgreSQL server was stopped and its temporary cluster
  removed. The same-head
  [seven-platform development-package run](https://github.com/vahid110/arrow-adbc/actions/runs/34808191495)
  passed all extracted-client archives and the final downloaded-checksum
  gate; these are development archives, not production release artifacts.
- 2026-09-14: A bound-row error after successful timezone setup now rolls back
  only the driver's autocommit-owned transaction, clears its affected-row count,
  and makes the result stream terminal so a retry cannot execute remaining
  rows outside that transaction. PostgreSQL 17 tests insert one valid row then
  fail on division by zero in the second: no-output and lazy output modes
  restore timezone, leave the connection idle/reusable, and persist zero rows;
  an explicit-transaction case remains caller-owned. The local statement suite
  passed 106 cases with six expected skips, two focused ASan/UBSan tests passed,
  CMake shared/static driver artifacts built, and the AWS-free Redshift artifact
  suite passed. Other cleanup paths remain in the roadmap above. The
  [five-platform PostgreSQL 18 matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34807560141)
  passed with all AWS-backed jobs skipped.
- 2026-09-14: On a failed typed-query `Prepare`, the bound-query reader now
  rolls back only the timezone transaction it opened for autocommit and
  preserves the original SQL error. PostgreSQL 17 tests cover both output
  modes, idle/reusable connection and restored `Europe/Berlin` timezone, plus
  an explicit-transaction case that remains caller-owned. Both focused cases
  passed under Meson ASan/UBSan; local CMake shared/static PostgreSQL and
  Redshift artifacts built and the AWS-free Redshift artifact suite passed.
  Failures inside timezone setup, after successful preparation, and early
  result-stream release remain the separate cleanup item above. The
  [five-platform PostgreSQL 18 and Ubuntu Meson matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34806953242)
  passed with all AWS-backed jobs skipped.
- 2026-09-14: Deferred timezone-sensitive parameter setup until all Arrow
  fields have valid PostgreSQL mappings and binary writers. A PostgreSQL
  regression reproduced the old leak (open transaction and UTC session after
  a later field failed), then passed with the ordering fix. On a disposable
  local PostgreSQL 17 server, 102 statement tests passed with six expected
  skips; the new case and existing timezone-ingest case also passed under
  Meson ASan/UBSan. Both CMake shared/static driver artifacts built, and the
  AWS-free Redshift artifact suite passed. Preparation, execution, and early
  result-stream release cleanup remain the separate roadmap item above;
  the [five-platform PostgreSQL 18 and Ubuntu Meson matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34806383397)
  passed with all AWS-backed jobs skipped.
- 2026-09-14: Requalified the current `34a3bfa6c` production-code head through
  the [seven-platform development archive matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34805781805).
  All seven Linux, macOS, and Windows archive jobs passed, as did the final
  downloaded-archive/checksum verification. This is AWS-free development-
  archive evidence, not a production release or live Redshift test.
- 2026-09-14: Moved shared Arrow column type resolution ahead of replace-ingest
  `DROP TABLE`. A PostgreSQL-backed regression binds a named unsupported nested
  type and verifies the original temporary table's sentinel row remains and
  the statement is reusable. A disposable local PostgreSQL 17 run passed 268
  tests with 27 expected skips; CMake shared and static PostgreSQL/Redshift
  artifacts built, and the AWS-free Redshift artifact suite passed. This does
  not make SQL creation or data transfer after the drop atomic. The
  [five-platform PostgreSQL 18 and Ubuntu Meson matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34803561706)
  passed with all AWS-backed jobs skipped.
- 2026-09-14: Guarded the active shared PostgreSQL/Redshift ingest path against
  unnamed or empty Arrow field names before replace mode can drop an existing
  table. A PostgreSQL-backed ADBC regression verifies an unnamed field returns
  `INVALID_ARGUMENT`, preserves a temporary target's sentinel row, and leaves
  the connection reusable. A temporary local PostgreSQL 17 run passed 267
  tests with 27 expected skips; local CMake PostgreSQL/Redshift test targets
  built and the AWS-free Redshift artifact suite passed. Other malformed
  replace schemas remain a separate preflight checkpoint.
- 2026-09-14: Added a focused Ubuntu x86-64 Meson build/test step to the
  PostgreSQL 18 GitHub Actions job. It reuses the existing database service and
  skips Redshift Serverless. The [five-platform PostgreSQL 18 and Redshift-
  artifact run](https://github.com/vahid110/arrow-adbc/actions/runs/34803185570)
  passed, including the new Linux Meson step, with every AWS-backed job skipped.
- 2026-09-14: Added a second AWS-free regression for the active parameterized
  ingest helper: an Arrow stream read error after one successful 16-row SQL
  batch must roll back the entire bind. The callback confirms 17 rows were
  visible inside the transaction (one seed plus 16 inserted); afterward only
  the seed remains, affected rows reset to zero, and the connection is idle
  and reusable. Both focused cases passed against temporary local PostgreSQL
  18, which was stopped and removed after testing. The direct helper test
  exposed hidden `PqResultHelper` symbols in the Linux test link, so CMake and
  Meson now compile that helper into the PostgreSQL test executable. The
  [first AWS-free five-platform run](https://github.com/vahid110/arrow-adbc/actions/runs/34800116037)
  failed to link on Linux; the [corrected five-platform PostgreSQL 18 and
  Redshift-artifact matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34800679139)
  passed with every AWS-backed job skipped. No Redshift compute was used.
- 2026-09-14: Added an AWS-free, PostgreSQL-backed regression for the shared
  parameterized ingest helper. It inserts 18 rows across two Arrow batches,
  then verifies that a `23502` error in a second SQL batch after 16 valid rows
  rolls back the entire later bind, reports zero affected rows, and leaves the connection
  reusable. The focused test passed against a temporary local PostgreSQL
  17.11 server; the server and its files were removed afterward. This does not
  replace the live Redshift ingest qualification.
- 2026-09-14: Extended the first-upload staged-`COPY` lifetime assertion to
  track the Arrow schema release as well as the stream and yielded arrays.
  The full AWS-free Redshift Meson test executable passed 49/49 cases under
  AddressSanitizer and UndefinedBehaviorSanitizer on macOS ARM64. No AWS role,
  S3 object, or active ingest path was changed.
- 2026-09-14: Rebuilt the macOS ARM64 Meson target with AddressSanitizer and
  UndefinedBehaviorSanitizer in a separate temporary tree. The Redshift Meson
  test executable passed all 49 GoogleTest cases; the build also compiled the
  PostgreSQL driver and both COPY test targets. This is an AWS-free local
  sanitizer check, not live Redshift or a cross-platform Meson guarantee.
- 2026-09-14: Qualified the new private stream source/test lists through an
  isolated Meson 1.11.2/Ninja 1.13.2 build on macOS ARM64 with Homebrew libpq
  18.6. PostgreSQL and Redshift targets compiled, including the stream helper
  and test; `meson test adbc-driver-redshift` passed (1/1 executable). This
  closes the local Meson gap for this target only, not Linux/Windows Meson
  qualification or any AWS-backed behavior. No repository dependency or
  production package was changed.
- 2026-09-14: Strengthened the private stream-preflight success test to assert
  that the owned Arrow stream and both yielded arrays are released before the
  first object-store upload, not merely before the later COPY callback. All
  49 Redshift artifact tests pass under ASan/UBSan after this test-only change.
  The [five-platform PostgreSQL 18 and Redshift-artifact gate](https://github.com/vahid110/arrow-adbc/actions/runs/34797397979)
  also passed with AWS jobs skipped. The helper already releases its schema
  before the same callback; no active ingest path, AWS policy, or S3 object
  was changed.
- 2026-09-14: The bounded stream-preflight head passed the
  [five-platform PostgreSQL 18 and Redshift-artifact matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34795497470)
  with AWS-backed jobs explicitly skipped. The first run caught a GCC-only
  ambiguous empty-list constructor in the test fixture; the explicit
  `std::vector<Batch>` correction passed the replacement Debian job and all
  four other platforms. The preceding code commit passed the
  [seven-platform development archive/checksum matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34795275226);
  the later correction changes only a test initializer, not packaged code.
  Neither run is a live staged-COPY or production-release qualification.
- 2026-09-14: Added a Redshift-private, AWS-free multi-batch stream preflight.
  It moves and releases the input stream, validates empty and nonempty batches,
  caps the aggregate CSV at 8 MiB and the batch count at 128, then calls the
  existing exact-object coordinator only after the entire stream succeeds.
  Seven new tests cover two-batch exact output and once-only COPY, later NULL,
  malformed empty batch, stream-read failure, aggregate overflow, empty or
  excessive batch streams, and invalid callback handles; failed preflights
  make zero store/COPY calls. CMake shared/static builds and all 49 Redshift
  artifact tests pass under ASan/UBSan. Meson source/test lists were kept in
  parity and later qualified locally as recorded above; the AWS-free CI gate
  passed. No S3 adapter, IAM change, active ingest
  selection, or live Redshift support claim follows.
- 2026-09-14: The all-empty-string CSV regression head passed the
  [five-platform PostgreSQL 18 and Redshift-artifact matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34794342952)
  and [seven-platform development archive plus checksum verification](https://github.com/vahid110/arrow-adbc/actions/runs/34794351261).
  The Redshift Serverless and manual COPY jobs were explicitly skipped; these
  results do not qualify staged `COPY` on AWS or a production package.
- 2026-09-14: Added an AWS-free regression for an all-empty Arrow string
  column with a valid absent data buffer. The private Redshift CSV writer
  emits the exact quoted empty fields without pointer arithmetic on a null
  buffer; all 41 focused staging/CSV tests pass under ASan/UBSan. This does
  not select staged `COPY` in the active ingest path.
- 2026-09-14: Apache `main` advanced from this branch's `4d50e2e30` base to
  `86667c4d7` by a driver-manager profile interpolation fix and widened
  ingest-value validation comparisons. Neither touches the PostgreSQL or
  PgWire source paths, so no design change is indicated. A future rebase
  should rerun PostgreSQL/Redshift ingest validation and installed-client
  package smoke checks; no history rewrite was done for this read-only audit.
- 2026-09-14: The output-schema development head passed the
  [five-platform PostgreSQL 18 and Redshift-artifact matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34792226698)
  and [seven-platform checked development archive matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34792236582),
  including Windows x64/ARM64. AWS jobs were skipped. The isolated
  [output-schema branch](https://github.com/vahid110/arrow-adbc/tree/feature/upstream-pg-copy-output-schema)
  passed all six broad fork workflows (Dev, Native Unix, Native Windows,
  vcpkg, Integration, and Rust). The remaining Native Unix run for the
  isolated trailer branch also passed all 24 jobs. These are development
  checks, not live Redshift staged-COPY or production-release qualification.
- 2026-09-14: A read-only review of official S3 documentation confirmed the
  staged-COPY uploader's fail-before-AWS `s3:GetObject` gate for `HeadObject`
  and ETag-conditional deletion. The same docs show that a missing key can
  appear as 403 without `s3:ListBucket`; this cannot safely replace
  independent reconciliation of ambiguous uploads. No IAM permission, trust
  policy, AWS resource, or active ingest path changed.
- 2026-09-14: Corrected `PostgresCopyStreamReader::SetOutputSchema` to validate
  the supplied Arrow schema instead of the reader's previous/uninitialized
  schema, including null, uninitialized, non-struct, and wrong-column-count
  rejection without taking ownership of invalid input. Four offline
  regressions bring the binary-COPY reader suite to 49/49 under ASan/UBSan.
  This is a latent API-correctness fix; the active query path currently
  infers its own output schema. Published the one-commit generic fix on
  [`feature/upstream-pg-copy-output-schema`](https://github.com/vahid110/arrow-adbc/tree/feature/upstream-pg-copy-output-schema)
  from `upstream/main` at `4d50e2e30`; its isolated source build and 26/26
  offline reader tests pass. Broader fork CI is green as recorded above. No AWS
  run or Apache PR was used.
- 2026-09-14: Added a Redshift-private staging regression with both parent
  and child Arrow offsets nonzero and a NULL at the combined physical row.
  CSV preparation rejects it with `kNullValue` before any object-store event
  or COPY call. All 40 focused staging tests pass under ASan/UBSan. No
  production ingest path, uploader, IAM policy, or AWS resource was changed;
  staged `COPY` remains inactive.
- 2026-09-14: The trailer-validation development head passed the
  [five-platform PostgreSQL 18 and Redshift-artifact matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34790185614)
  and [seven-platform development archive/checksum matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34790193554),
  with AWS-backed jobs skipped. The isolated COPY-bounds and COPY-header
  branches now pass all six broad fork workflows (Dev, Native Unix, Native
  Windows, vcpkg, Integration, and Rust). The isolated field-framing branch
  also completed its six-workflow matrix; the trailer branch's Native Unix
  check subsequently passed. No release claim follows from these development
  checks.
- 2026-09-14: Enforced the binary-COPY trailer at parser and libpq physical
  end-of-stream boundaries. A trailer with bytes after it, additional COPY
  data after a trailer, a truncated trailer, and a successful physical EOF
  without a trailer are now rejected; a server-reported error retains
  precedence. Four new offline regressions bring the development reader
  suite to 45/45 under ASan/UBSan. Published the standalone PostgreSQL-only
  [`feature/upstream-pg-copy-trailer`](https://github.com/vahid110/arrow-adbc/tree/feature/upstream-pg-copy-trailer)
  from `upstream/main` at `4d50e2e30`, with its source build and 26 offline
  reader tests passing. The libpq EOF branch is covered through a testable
  decoder validation method, not a synthetic libpq integration test; the
  broader fork CI is pending. No AWS run or Apache PR was used.
- 2026-09-14: The header-validation development head passed the
  [five-platform PostgreSQL 18 matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34789014071)
  and [seven-platform development package/checksum matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34789021228)
  without AWS. The isolated bounds branch's sole Integration failure came
  from AlloyDB Omni closing all connections during validation; its failed-job
  [retry passed](https://github.com/vahid110/arrow-adbc/actions/runs/34788686718).
  The other isolated platform checks may still be in progress.
- 2026-09-14: Bounded each binary-COPY tuple, nested-record, and array child
  decoder to its own declared field view and reject successful decodes that
  leave bytes unconsumed. Five new regressions cover cross-field array,
  record, and numeric reads, surplus numeric bytes, and cursor preservation
  on `EOVERFLOW`. The development reader suite passes 41/41 both normally and
  under ASan/UBSan. Published the generic fix on
  [`feature/upstream-pg-copy-framing`](https://github.com/vahid110/arrow-adbc/tree/feature/upstream-pg-copy-framing),
  stacked on the isolated bounds branch; its source build and all 34 offline
  reader tests pass. Fatal malformed-field errors may still leave a partial
  Arrow builder, and this does not validate the COPY trailer or redesign
  whole-row overflow retry. The broader fork CI remains pending; no AWS run
  or Apache PR was used.
- 2026-09-14: The prior COPY-bounds head passed the
  [five-platform PostgreSQL 18 matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34786999583)
  and [seven-platform development archive and checksum matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34787001908)
  with AWS jobs skipped. The isolated branch's Dev check caught three
  formatting-only hunks; the pinned-format correction passed
  [replacement Dev CI](https://github.com/vahid110/arrow-adbc/actions/runs/34788686676)
  and its 29 offline reader tests pass again. The broader replacement CI is
  still running.
- 2026-09-14: Reject high-order critical binary-COPY header flags, including
  the OID-row variant unsupported by our tuple reader, as required by the
  [PostgreSQL binary format](https://www.postgresql.org/docs/current/sql-copy.html).
  Continue to tolerate low-order advisory flags and skip a bounded header
  extension. Three new offline checks bring the development reader suite to
  36/36 under ASan/UBSan. Published the one-commit generic fix on isolated
  [`feature/upstream-pg-copy-header`](https://github.com/vahid110/arrow-adbc/tree/feature/upstream-pg-copy-header)
  from `upstream/main` at `4d50e2e30`; its source build and 25 offline reader
  tests pass. Trailer framing and exact parent-field consumption are separate
  outstanding hardening work. No Redshift/AWS run was used for this fix and
  no Apache PR has been opened.
- 2026-09-14: Hardened shared PostgreSQL binary-COPY decoding against malformed
  field lengths before invoking primitive readers. Tuple, nested-record, and
  array-element readers now reject lengths below `-1` or beyond the remaining
  buffer. Fixed-width scalars and arrays treat only `-1` as NULL; arrays reject
  negative dimensions and require the exact 12-byte payload for zero dimensions.
  Seven new malformed-input tests bring the AWS-free reader suite to 33/33 under
  ASan/UBSan. This is not complete field framing: nested readers are not yet
  confined to their declared parent subview, so exact consumption and
  cross-field isolation remain a separate hardening step. No AWS run was used.
- 2026-09-14: Published the generic bounds fix on isolated fork branch
  [`feature/upstream-pg-copy-bounds`](https://github.com/vahid110/arrow-adbc/tree/feature/upstream-pg-copy-bounds)
  from `upstream/main` at `4d50e2e30`. Its one-commit diff contains only the
  PostgreSQL COPY reader and test. After resolving test-only context from
  earlier standalone fixes, an isolated source build and all 29 offline reader
  cases pass. No Apache PR has been opened.
- 2026-09-13: Corrected two shared PostgreSQL binary-COPY JSONB failure paths:
  an unknown binary version previously set an error but returned success, and
  a zero-length non-null field could read the following field's first byte.
  Both now return `EINVAL` without appending a value; the zero-length case
  leaves the input cursor unchanged. Two new regressions and all 26 offline
  reader cases pass locally, including under ASan/UBSan. The pushed head passed
  the [five-platform PostgreSQL 18 and Redshift artifact matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34784112936)
  and [seven-platform development archive/checksum run](https://github.com/vahid110/arrow-adbc/actions/runs/34784115801)
  with AWS-backed jobs skipped. No AWS call was needed.
- 2026-09-13: Published the generic JSONB validation fix on isolated fork
  branch [`feature/upstream-pg-jsonb-validation`](https://github.com/vahid110/arrow-adbc/tree/feature/upstream-pg-jsonb-validation)
  from `upstream/main` at `4d50e2e30`. Its one-commit diff changes only the
  PostgreSQL COPY reader and test; an isolated source build and all 24 offline
  reader cases pass. The fork's
  [repository-wide pre-commit check](https://github.com/vahid110/arrow-adbc/actions/runs/34784028374)
  and [Unix](https://github.com/vahid110/arrow-adbc/actions/runs/34784028344),
  [Windows](https://github.com/vahid110/arrow-adbc/actions/runs/34784028474),
  [vcpkg](https://github.com/vahid110/arrow-adbc/actions/runs/34784028373),
  [integration](https://github.com/vahid110/arrow-adbc/actions/runs/34784028350),
  and [Rust](https://github.com/vahid110/arrow-adbc/actions/runs/34784028348)
  workflows all passed on the isolated branch. No Apache PR has been opened.
- 2026-09-13: Hardened the private Redshift CSV writer against a malformed
  Arrow child array with no buffer list. It now returns `kMalformedArrow`
  without touching the caller's output instead of reaching a nanoarrow null
  dereference. The new regression and all 40 AWS-free Redshift artifact tests
  pass locally. The
  [five-platform PostgreSQL 18 and Redshift artifact matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34781875099)
  and [seven-platform development archive/checksum run](https://github.com/vahid110/arrow-adbc/actions/runs/34781873040)
  passed on the pushed head with all AWS-backed jobs skipped; staged `COPY`
  remains disabled.
- 2026-09-13: Prevented the shared PostgreSQL binary-COPY reader from silently
  flattening a multidimensional PostgreSQL array into a one-dimensional Arrow
  list. It now reports an explicit unsupported error until nested-list mapping
  exists. A valid 2-by-2 `int4[]` binary fixture proves the error; the existing
  one-dimensional array case and all 24 offline COPY reader tests pass.
- 2026-09-13: Published that generic array-shape guard on isolated fork branch
  [`feature/upstream-pg-array-shape`](https://github.com/vahid110/arrow-adbc/tree/feature/upstream-pg-array-shape)
  from `upstream/main` at `4d50e2e30`. Its one-commit diff changes only the
  PostgreSQL COPY reader and test; an isolated source build and all 23 offline
  reader cases pass. No Apache PR has been opened.
- 2026-09-13: The fork's general `Dev` pre-commit job exposed three repository
  hygiene failures after the new safety workflow push: four Redshift fixture
  scripts had shebangs without executable modes, the CSV writer lacked an
  explicit `<vector>` include, and eight existing PgWire files differed from
  the repository-pinned clang-format 18.1.7. Fixed the first two separately
  from a formatting-only commit. Local PostgreSQL reader tests (23/23),
  Redshift artifact tests (39/39), and both AWS-free safety mock suites
  (34/34) pass after the changes. A docs-only push then passed the
  [repository-wide pre-commit gate](https://github.com/vahid110/arrow-adbc/actions/runs/34779120566);
  the later unity-fix head passed it again in
  [run 34779475982](https://github.com/vahid110/arrow-adbc/actions/runs/34779475982).
  No live Redshift test was dispatched for formatting.
- 2026-09-13: The broader native Unix CI exposed a CMake unity-build collision
  between three independent Redshift fixture tests sharing anonymous-namespace
  names. Only `adbc-driver-redshift-test` now opts out of unity compilation;
  production libraries remain unity-enabled. With `CMAKE_UNITY_BUILD=ON`, the
  full local CMake build and the 39-case Redshift suite pass. The fresh
  [native Unix CI run 34779475942](https://github.com/vahid110/arrow-adbc/actions/runs/34779475942)
  passed all 24 jobs, including CMake on Ubuntu/macOS Intel/Apple Silicon,
  Meson on Ubuntu, clang-tidy, and downstream client checks. No AWS test was
  required.
- 2026-09-13: Corrected a shared PostgreSQL-wire result conversion edge: the
  binary-encoded `TIMESTAMP`/`TIMESTAMPTZ` epoch adjustment now checks for
  overflow and reports a non-retryable range error instead of wrapping a valid
  server value outside Arrow's signed 64-bit microsecond range. A new boundary and
  first-overflow reader test passes with 23 offline reader tests and 39 Redshift
  artifact tests locally. Redshift documents timestamp values through
  [year 294276](https://docs.aws.amazon.com/redshift/latest/dg/r_Datetime_types.html),
  beyond Arrow's 1970-epoch microsecond range. No AWS test was needed; the
  corrected head passed the
  [five-target PostgreSQL 18 and Redshift artifact matrix](https://github.com/vahid110/arrow-adbc/actions/runs/34778289135)
  and [seven-platform development archive/checksum run](https://github.com/vahid110/arrow-adbc/actions/runs/34778281078),
  with all AWS-backed jobs skipped.
- 2026-09-13: Published the generic range fix separately on fork branch
  [`feature/upstream-pg-timestamp-range`](https://github.com/vahid110/arrow-adbc/tree/feature/upstream-pg-timestamp-range)
  from `upstream/main` at `4d50e2e30`. Its two-commit diff contains only the
  PostgreSQL COPY reader and its test. An isolated source build passed 21/21
  offline reader cases. No Apache PR has been opened.
- 2026-09-13: Fixed a false-success cleanup edge in both ordinary Redshift CI
  ingress and the opt-in COPY fixture: a failed or response-lost EC2 revoke
  cannot be certified by one stale empty Describe result. It leaves the rule
  pending independent audit and fails the job. The new stale-read/denied-revoke
  regressions bring the local mock suites to 11 ordinary-ingress and 23 COPY
  cases. The [AWS-free cleanup selftest](https://github.com/vahid110/arrow-adbc/actions/runs/34777883620)
  passed; no IAM grant, live COPY, or Redshift compute was used. A genuinely
  lost runner still needs independent post-run rule inspection.
- 2026-09-13: Added a separate, push-triggered
  [AWS-free cleanup workflow](https://github.com/vahid110/arrow-adbc/actions/runs/34778632907)
  for edits to either ingress/COPY script or its mock tests. Its first run
  passed Bash syntax plus all 34 safety scenarios with only read access to
  repository contents; it has no AWS credential or live Redshift job. The
  existing paid live workflow's script path filter stays unchanged.
- 2026-09-13: Hardened ordinary live Redshift CI ingress ownership and failure
  recovery. Ten AWS-free mocks cover pre-existing foreign ingress, ambiguous or
  lost authorize/revoke responses, denied reads/revocation, invalid runner CIDR,
  and idempotent cleanup; all ten and the 22 existing COPY fixture mocks passed
  locally. A read-only regional security-group audit found no run-scoped CI
  ingress; an existing narrow developer rule was left untouched. The live
  Redshift workflow was not dispatched for this safety-only change, and the
  hard-lost-runner limitation above remains. The updated head passed the
  [AWS-free cleanup selftest](https://github.com/vahid110/arrow-adbc/actions/runs/34775735021),
  [five PostgreSQL/artifact targets](https://github.com/vahid110/arrow-adbc/actions/runs/34775730590),
  and [seven-platform development archive and checksum gates](https://github.com/vahid110/arrow-adbc/actions/runs/34775734174).
- 2026-09-13: Prevented an offline-preparable Redshift `COPY` failure: the
  private CSV writer now rejects any serialized row exceeding 4,000,000 bytes,
  conservatively below [Redshift's 4 MB input-row limit](https://docs.aws.amazon.com/redshift/latest/dg/r_COPY.html),
  while retaining the separate 8 MiB object cap. New writer/adapter tests
  prove an oversized row causes no S3 or COPY call and that exact row/object
  boundaries still pass; both local CMake and Meson Redshift suites passed
  39/39. No AWS call or active ingest-path change was made.
- 2026-09-13: Prepared a separate, opt-in nine-row CSV semantics probe reusing
  the exact-key staging fixture and transaction rollback. It checks quoted and
  bare empty text, bare `\\N` NULL, comma/quote/newline escaping, and BIGINT
  bounds, while classifying quoted `\\N` as NULL, literal, or other without
  assuming the answer. The original two-row probe remains separate. Twenty-two
  offline mocks pass, including the missing-permission gate on both modes;
  no live fixture execution is allowed until the reviewed grant and deliberate
  workflow flag are added. The workflow now isolates AWS-free selftests from the
  PostgreSQL-only matrix after one selftest superseded an unrelated matrix.
  Concurrent AWS-free qualification runs
  [`34773423309`](https://github.com/vahid110/arrow-adbc/actions/runs/34773423309)
  and [`34773423480`](https://github.com/vahid110/arrow-adbc/actions/runs/34773423480)
  both passed independently: all 22 cleanup/CSV mocks passed on Ubuntu with
  AWS and database jobs skipped, while the PostgreSQL/Redshift artifact matrix
  passed Ubuntu x86-64/ARM64, Debian x86-64, and macOS Intel/Apple Silicon
  with AWS jobs skipped. Neither canceled the other. No AWS call or IAM change
  was made for this checkpoint.
- 2026-09-13: Hardened the manual staged-`COPY` fixture's exact S3 keys:
  conditional create and ETag-matched delete now prevent a collision or
  ambiguous upload from authorizing a blind delete. Sixteen offline mocks
  cover missing approval, collisions, lost responses, changed ETag, exact
  ingress cleanup, and success. AWS-free selftest run
  [`34772988617`](https://github.com/vahid110/arrow-adbc/actions/runs/34772988617)
  passed on Ubuntu with every database and AWS job skipped. AWS
  [requires both `s3:DeleteObject` and `s3:GetObject` for an
  ETag-conditional delete](https://docs.aws.amazon.com/AmazonS3/latest/userguide/conditional-deletes.html),
  so live fixture dispatch is deliberately gated until the CI uploader role's
  exact `staging/ci/*` read permission is separately approved and verified.
  No IAM change or live AWS call was made for this checkpoint.
- 2026-09-13: Composed one Arrow record batch, the conservative CSV writer,
  and the exact-object staging coordinator in a Redshift-private adapter. Five
  new fake-store tests cover exact uploaded CSV/manifest/COPY SQL, rejection
  before any external side effect, collision preservation, and a settled
  ambiguous COPY result without retry. All 38 local Redshift tests passed in
  CMake with warnings as errors; a fresh Meson C++17 build also passed 38/38.
  AWS-free PostgreSQL/Redshift matrix run
  [`34772908486`](https://github.com/vahid110/arrow-adbc/actions/runs/34772908486)
  was superseded by the later AWS-free cleanup selftest under the workflow's
  shared concurrency group; replacement matrix run
  [`34773086915`](https://github.com/vahid110/arrow-adbc/actions/runs/34773086915)
  passed Ubuntu x86-64/ARM64, Debian x86-64, and macOS Intel/Apple Silicon,
  with AWS-backed jobs skipped. The seven-platform development-archive run
  [`34772902589`](https://github.com/vahid110/arrow-adbc/actions/runs/34772902589)
  also passed. The package run verified extracted archives and independent
  clients on Ubuntu x86-64/ARM64, Debian x86-64,
  macOS Intel/Apple Silicon, and Windows x64/ARM64, plus all seven checksums.
  These are development archives, not production releases. This adapter
  neither supplies an S3 implementation nor selects `ExecuteIngest`; no AWS
  call was made for it.
- 2026-09-13: Added a Redshift-private, AWS-free CSV writer for non-null INT32,
  INT64, and UTF-8 string Arrow fields. It requires the COPY column list to
  exactly match field names and order, quotes strings, escapes embedded quotes,
  validates UTF-8 and Arrow buffers, respects slices, and caps output at the
  coordinator's 8 MiB bound. Nulls, unsupported types, malformed text, and
  the ambiguous literal `\\N` fail without changing the output. Eight new
  focused tests and all 33 local Redshift tests passed with warnings as errors;
  CMake/Meson lists were updated. AWS-free five-platform CI run
  [`34772508677`](https://github.com/vahid110/arrow-adbc/actions/runs/34772508677)
  passed Ubuntu x86-64/ARM64, Debian x86-64, and macOS Intel/Apple Silicon;
  every AWS-backed job was skipped. A fresh local Meson Redshift test build
  passed all 33 tests after explicitly linking nanoarrow in that test target.
  Live CSV semantics, S3 integration, and driver selection remain unverified.
  No AWS call was made for this checkpoint.
- 2026-09-13: Added a Redshift-only, AWS-free coordinator for the already
  validated one-object `COPY` plan. It accepts bounded pre-serialized data and
  a store that conditionally creates and deletes only proven-owned exact keys;
  16 fake-store tests cover collisions, ambiguous uploads, SQL outcomes,
  exception deferral, and cleanup errors. CMake built the Redshift library and
  PostgreSQL targets, and the Redshift suite passed locally. CMake/Meson source
  lists are aligned; AWS-free run
  [`34771998414`](https://github.com/vahid110/arrow-adbc/actions/runs/34771998414)
  passed Ubuntu x86-64/ARM64, Debian x86-64, and macOS Intel/Apple Silicon,
  with every AWS-backed job skipped. A later fresh local Meson Redshift test
  build passed all 33 tests including this seam. At that checkpoint there was
  no S3 SDK, IAM change, CSV serializer, or ADBC ingest selection; no AWS call
  was made for it.
- 2026-09-13: Published standalone fork branch
  [`feature/upstream-pg-getobjects-column-constraints`](https://github.com/vahid110/arrow-adbc/tree/feature/upstream-pg-getobjects-column-constraints)
  from the current Apache `main` at `4d50e2e30`. Its diff is only PostgreSQL
  connection logic and a regression test; 17 focused upstream PostgreSQL
  GetObjects tests passed against a disposable PostgreSQL 17.11 instance.
  No PR against Apache was opened.
- 2026-09-13: Fixed a PostgreSQL `GetObjects` semantics bug: a column-name
  filter now affects columns, not table constraints. A new regression checks
  that the primary key on an excluded column remains in the returned table
  metadata; ten focused integration/unit tests passed against an isolated
  local PostgreSQL server, which was stopped and removed afterward. Separately,
  the Redshift-private staged-`COPY` plan now requires an exact ordered column
  list, rejecting invalid or duplicate names so a future reordered append
  cannot silently load the wrong table fields. The focused plan tests and
  combined C++ build passed locally. Added `postgresql_only=true` manual CI
  dispatch to qualify both patches across the PostgreSQL matrix without AWS.
  Run [`34771025810`](https://github.com/vahid110/arrow-adbc/actions/runs/34771025810)
  passed Ubuntu x86-64/ARM64, Debian x86-64, and macOS Intel/Apple Silicon;
  both AWS-backed jobs were skipped. Neither change selects staged ingestion
  in the driver.
- 2026-09-13: The approved, separate `RedshiftCiIngressInspect` IAM policy now
  grants `ec2:DescribeSecurityGroupRules` only in `eu-central-1` (the action
  requires `Resource: "*"`); IAM simulation allowed Frankfurt and denied
  `us-east-1`. AWS-free Ubuntu run
  [`34770428140`](https://github.com/vahid110/arrow-adbc/actions/runs/34770428140)
  passed all ten COPY cleanup mocks with all AWS-backed jobs skipped. Then
  manual run [`34770466003`](https://github.com/vahid110/arrow-adbc/actions/runs/34770466003)
  connected as `IAMR:adbc-redshift-ci` and passed the exact two-row staged
  `COPY` fixture. An independent post-run AWS inspection found no rule with
  `adbc-pgwire-copy-34770466003-1` and no objects under
  `staging/ci/34770466003-1/`. This is fixture qualification, not driver support.
- 2026-09-13: Added an AWS-free manual dispatch path for the staged-`COPY`
  cleanup mocks; `copy_fixture_selftest=true` is configured to skip the
  PostgreSQL and AWS-backed jobs, even if another manual probe input is also
  selected. Expanded the offline fixture from six to ten scenarios, covering
  an unexpected database identity, denied `ASSUMEROLE`, a lost manifest-upload
  response, and a failed `COPY` query after ingress and staging. All ten mocks,
  shell syntax, workflow YAML parsing, and diff checks passed locally on macOS.
  At this commit, Ubuntu CI qualification was pending; no live AWS call or
  `COPY` had yet been made.
- 2026-09-13: Published a manual, opt-in two-row `COPY` fixture, separate from
  the ADBC driver path. It requires read-only ingress discovery before any
  network/S3 mutation, uses an exact run-owned security-group rule ID and
  two run-owned S3 keys, verifies TLS hostname, database identity, role-use
  privilege, row count and both values, and cleans up on ordinary failures.
  Six offline mocks pass, including ambiguous authorize/revoke responses and
  partial upload. Updated credential-only run `34768476642` passed while all
  database jobs were skipped; no `COPY` was attempted. The missing ingress-read
  permission and required independent post-run audit still gate a live test.
- 2026-09-13: Manual OIDC credential run `34767827101` succeeded and returned
  `IAMR:adbc-redshift-ci`; PostgreSQL and live Redshift jobs were skipped. Added
  an isolated CI inline policy for workgroup-specific temporary credentials and
  write/delete only under `staging/ci/*`, verifying allowed and denied IAM
  simulations. Replaced the COPY role's failing source-ARN trust condition with
  the exact observed IAMR database-user External ID, retaining SourceAccount,
  both Redshift principals, and read-only S3 access. No non-root connection,
  S3 upload, or COPY has been run with this new trust; the ownership-safe
  ingress-read grant and two-row cleanup gate remain pending.
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
