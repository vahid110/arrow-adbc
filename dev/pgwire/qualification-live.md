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

# Isolated installed-artifact qualification

Task008c adds a supported, explicit-dispatch path. A green offline suite or a
successful credential request is not live qualification. The historical unsigned
Task008a/b entry points still reject. The SSM path supersedes the unimplemented
three-signature proposal only for this one finite, dedicated batch.

## Trust and finite controls

The host authenticates as `adbc-provisioner` in account `385207570137`. It issues
one Standard String (at most 4 KB), version 1, without overwrite:
`/adbc/qualification/20261016/admission`. The reviewed host owns issuance; CI can
read only that parameter and create only its exact nonce/run claim. CI cannot
issue/delete/overwrite admissions or claims. Policy must explicitly deny
`ssm:PutParameter` when the **String** condition `ssm:Overwrite` equals `true`.
A failed or uncertain claim is spent/unknown and must never authorize a retry.
Retain admissions, claims, host receipts and original failures as evidence.

The compact v2 record binds the exact merged source, original PR/review/check
receipt, live run, immutable artifact IDs, payload/inventory/C-client/CA hashes,
public runner `/32`, dedicated resource IDs, host cleanup owner/observer and
finite timestamps. The consumer obtains authenticated GitHub REST, STS,
Serverless and EC2 observations itself. Caller-generated inventories are inputs
to comparison; they never grant authority. GitHub must show the exact merged PR,
an identical original-head/merged tree, owner-published `VERDICT: APPROVE` comment
containing the full reviewed head, and completed original GitHub Actions checks
(success, or legitimately skipped/neutral jobs). All applicable gates must be
green before the coordinator dispatches.

The fixed plan is namespace/workgroup `adbc-qualification-20261016`, database
`adbc_qualification`, eu-north-1, new tagged SG only, and the three read-only
subnets pinned in `pgwire_qualification_live.py`. Base/max capacity is 4 RPU,
automatic optimization and price/performance scaling are disabled, TLS is
required, each SQL statement has a 10-second service/driver limit, and a new daily
4 RPU-hour usage limit deactivates the workgroup. Query time is at most 15 minutes;
total host lifecycle at most 60 minutes, plus bounded teardown observation.
Incremental target is USD 5, one batch, paid retries zero.

`CreateWorkgroup` sends `extraComputeForAutomaticOptimization=false` and omits
`pricePerformanceTarget`: the service rejects both fields together, even when
their values are false/disabled. The [AWS example](https://docs.aws.amazon.com/redshift-serverless/latest/APIReference/API_CreateWorkgroup.html#API_CreateWorkgroup_Examples)
omits the pricing target and returns `DISABLED`. Actual readbacks must still
report `pricePerformanceTarget.status=DISABLED`, extra compute false and base/max
4; absent or enabled controls fail closed. No fallback or retry is authorized.

IAM isolates ingress to the exact new SG. EC2's request-tag condition alone does
not guarantee that all callers tag rules; the runner always tags, checks the exact
returned rule, independently describes it, revokes only its rule ID and verifies
removal. Missing tags, unexpected rules and uncertain replies fail closed. No
CIDR-based fallback, shared target, custom SDK endpoint/profile, database URI in
argv/environment/files/logs, admin credential delegation, or secret value read is
supported. Temporary database credentials go from SDK memory to private pipes.
Child errors are discarded; only fixed stage names and whitelisted receipts leave
the boundary. The installed wheel also rejects 128-byte/case-colliding replace schemas and
checks original fixture rows/schema survive; this is equivalent coverage, not a
claim that the original global-fixture gtest ran. The host separately observes
fixtures and role sessions through the
Data API and tears down dedicated resources even when CI fails.

## Coordinator sequence (host only)

Run only after the reviewed Task008c head is merged and its applicable CI is green.
The worker and reviewer must stay offline. Use only the already authorized host
profile; do not modify global profiles, the OIDC provider, VPC or subnet settings.
The host attempt/receipt for failed run `38916730017` remains spent evidence;
this offline API repair does not authorize another host invocation. The
coordinator must resolve further authority with the human before a new attempt.

1. Generate the IAM baseline with AWS Autopilot from the actual SDK sources
   `ci/scripts/pgwire_qualification_live.py` and the installed wheel's
   `python/adbc_driver_redshift/adbc_driver_redshift/iam.py`. Narrow to the exact
   API operations below; exclude unused provisioned-cluster/KMS/secret-value APIs.
   Prepare and independently review the runtime template with placeholders
   `EXACT_OWNED_WG`, `EXACT_OWNED_NS`, `EXACT_OWNED_SG`, `EXACT_RUN_ID`,
   `EXACT_CUTOFF`, and the exact ownership nonce. Every Allow expires at cutoff.
2. Before dispatch, the coordinator freshly authenticates repository metadata:
   `GET /repos/vahid110/arrow-adbc` must return owner ID `5770674` and repository
   ID `1363017696`; `GET /repos/vahid110/arrow-adbc/actions/oidc/customization/sub`
   must return `use_default=true`, `use_immutable_subject=true`, and
   `sub_claim_prefix=repo:vahid110@5770674/arrow-adbc@1363017696`.
   These are host checks, with no additional CI permissions. GitHub documents
   this [immutable subject format](https://docs.github.com/en/actions/reference/security/oidc#immutable-subject-claims).
   Confirm exact dedicated namespace/workgroup/SG names are absent. Create only
   the new CI role if absent, tagged `adbc-qualification-nonce=<64-hex nonce>`,
   with the account's exact GitHub OIDC provider principal, action
   `sts:AssumeRoleWithWebIdentity`, and `StringEquals` audience
   `sts.amazonaws.com` and subject
   `repo:vahid110@5770674/arrow-adbc@1363017696:ref:refs/heads/feature/pgwire-core-redshift`.
   `OIDC_SUBJECT` pins this exact value; legacy subjects, alternate IDs/refs,
   wildcard/OR subjects and `StringLike` are rejected.
   For the owned role retained after run `38912745155`, only the coordinator
   applies the independently reviewed exact trust via `UpdateAssumeRolePolicy`.
   Fresh `GetRole` must match the full trust, ARN and ownership tag before
   dispatch. Do not change repository OIDC settings or the global provider, and
   do not capture/log JWTs or introduce another authentication method.
   Its sole inline policy `adbc-qualification-initial` has two Allow statements:
   `sts:GetCallerIdentity` on `"*"` (Sid `Identity`) and `ssm:GetParameter`
   on the exact admission ARN (Sid `ReadSingleAdmission`), with no other actions
   or managed policies. The exact approved JSON document is pinned in
   `INITIAL_POLICY_DOCUMENT`; incomplete/paginated IAM inventories are rejected. No DB/ingress grant
   exists yet. Preserve role creation ownership and trust/policy receipts.
3. Dispatch `.github/workflows/pgwire-qualification-live.yml` explicitly at the
   exact merged integration SHA, supplying that SHA and the successful same-SHA
   C archive and Python wheel artifact IDs from the normal package/Python runs.
   No source driver rebuild is needed. The runner authenticates downloads,
   installs bytes, compiles its tiny C client and probes both Redshift entrypoints
   before OIDC. The uploaded `qualification-preflight-<run>` contains nonsecret
   hashes and its current `/32`; capture that artifact ID. It intentionally has no
   execution window or resource admission: installed load-only verification uses
   only the artifact/TLS projection; the host supplies the real finite window
   later. The consumer rechecks the same installed evidence before SDK access. CI waits at most twenty
   minutes for host admission. Do not provision while offline CI is unfinished.
4. Run the finite host helper in one process (CLI arguments are nonsecret):

   ```sh
   python3 -B ci/scripts/pgwire_qualification_host.py \
     --receipt /absolute/evidence/host-receipt.json \
     --preflight-artifact-id PRECISE_PREFLIGHT_ID --run-id LIVE_RUN_ID \
     --source-sha MERGED_SHA --nonce OWNERSHIP_NONCE \
     --policy-template /absolute/evidence/runtime-policy-template.json \
     --policy-template-sha256 REVIEWED_TEMPLATE_SHA256
   ```

   `GH_TOKEN` authenticates GitHub REST; the SDK uses only `adbc-provisioner`.
   The helper authenticates preflight/run/original-review/artifact provenance,
   journals calls before mutation, checks the exact initial IAM document before
   any resource creation and again before runtime-policy installation, rejects
   pre-existing names, creates tagged
   dedicated resources and the managed admin secret, verifies associations and
   finite controls, installs/readbacks only the exact run policy, and uses the
   namespace's **SecretArn** with `redshift-data:ExecuteStatement` to create
   `"IAMR:adbc-isolated-qualification-ci" PASSWORD DISABLE` and grant CREATE on
   this database. It never calls `GetSecretValue`. It then issues the immutable
   admission, waits boundedly for the exact CI run, and always enters cleanup.
   SIGINT/ordinary failures enter `finally`; a host crash requires exact-owned
   recovery from the durable receipt, never a second paid batch.
5. Inspect independent Data API counts for exactly `adbc_q_<run>0/1` and sessions
   for `IAMR:adbc-isolated-qualification-ci`. Unknown/nonzero observation is a
   failure even if resource deletion succeeds. The helper stops the exact CI run
   if needed, removes only the unchanged owned runtime policy, deletes/verifies
   the tagged workgroup with the usage control still active, then its usage
   limit, namespace and new SG. Unexpected/untagged ingress is preserved as
   unknown for host inspection; no guessed rule is revoked.
6. Independently verify the managed secret is removed or scheduled for deletion
   using **DescribeSecret only**, then remove the exact owned initial inline
   policy and CI role using `DeleteRolePolicy`, `DeleteRole`, and fresh
   `GetRole` absence. Record all resource/rule/usage/secret/role readbacks and
   SQL/session outcomes. Retain SSM admission/claim and local evidence. A helper
   success still requires these final secret/IAM readbacks before declaring
   complete teardown. No release/tag/PyPI action is part of this procedure.

## Exact SDK operations

CI: `sts:GetCallerIdentity`; `ssm:GetParameter`, `PutParameter` (exact claim,
no overwrite); Serverless `GetWorkgroup`, `GetNamespace`, `GetUsageLimit`,
`ListTagsForResource`, `GetCredentials` (900 seconds); EC2
`DescribeSecurityGroups`, `DescribeSubnets`, `DescribeSecurityGroupRules`,
`AuthorizeSecurityGroupIngress`, `RevokeSecurityGroupIngress`, with dependent
`CreateTags` only during rule creation. Metadata actions lacking resource-level
IAM support remain region constrained; DB/ingress grants use exact returned IDs.
CI gets no Data API, Secrets Manager, provisioning, deletion or role authority.

Host helper additionally: Serverless `CreateNamespace`, `CreateWorkgroup`,
`CreateUsageLimit`, `ListUsageLimits`, `DeleteUsageLimit`, `DeleteWorkgroup`,
`DeleteNamespace`; EC2 `CreateSecurityGroup`, dependent `CreateTags`,
`DeleteSecurityGroup`; Data API `ExecuteStatement`, `DescribeStatement`,
`GetStatementResult`, `CancelStatement`; IAM `GetRole`, `ListRolePolicies`,
`ListAttachedRolePolicies`, `GetRolePolicy`, `PutRolePolicy`, `DeleteRolePolicy`.
Root owns initial role creation/final deletion and managed-secret absence checks.
IAM uses its fixed global endpoint signed in us-east-1; all other SDK endpoints
are fixed in eu-north-1, with verified HTTPS, no proxy/config override and no SDK
retries. Offline tests use fake SDKs or dummy credentials for endpoint-model
inspection only, without network calls.
