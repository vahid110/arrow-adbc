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

# Open-source ADBC Redshift driver (development build)

This fork builds a distinct `libadbc_driver_redshift` from the shared
PostgreSQL-wire C++ core and Redshift-specific semantics. It is an MVP, **not a
production-ready or published binary package**. The existing Apache PostgreSQL
driver remains a separate artifact and compatibility baseline. See the
[user guide](../../../docs/source/driver/redshift.rst) for artifact installation,
connection examples, types and limits, and the
[developer guide](../../../docs/source/driver/pgwire-development.md) for test
evidence, gaps, and platform qualification.

## Build and install

Install a C/C++ compiler, CMake, Ninja, `pkg-config`, and libpq development
files. On Debian or Ubuntu, `libpq-dev` provides the last dependency. On macOS,
Homebrew's `libpq` is keg-only; set `PKG_CONFIG_PATH` to its `lib/pkgconfig`
directory if CMake cannot find it. These commands build the driver and the ADBC
driver manager from source on Linux or macOS:

```sh
cmake -S c -B build/pgwire-install -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DADBC_BUILD_TESTS=OFF \
  -DADBC_DRIVER_MANAGER=ON \
  -DADBC_DRIVER_POSTGRESQL=ON \
  -DADBC_DRIVER_SQLITE=OFF \
  -DADBC_DRIVER_FLIGHTSQL=OFF
cmake --build build/pgwire-install --parallel 4
cmake --install build/pgwire-install --prefix "$PWD/build/pgwire-prefix"
```

The install contains `libadbc_driver_redshift` and `libadbc_driver_manager`,
the public `arrow-adbc/driver/redshift.h` header, and CMake/pkg-config metadata.
The installed shared Redshift library still needs a compatible libpq at runtime.
Static-library consumers should inspect the generated package metadata and
link transitive dependencies; the clean-client check below uses shared libraries.
There are no release packages or platform-support guarantees yet.

For Ubuntu 24.04 x86-64/ARM64, Debian Bookworm x86-64, or macOS Intel/Apple
Silicon evaluation, the
[development-package workflow](https://github.com/vahid110/arrow-adbc/actions/workflows/pgwire-package.yml)
publishes architecture-specific seven-day archives and SHA-256 files. Each
embedded `README.md` explains extraction and runtime requirements. CI checks
each extracted archive with an independent client and separately downloads
every artifact to verify its archive/checksum pair. The Ubuntu 24.04 x86-64
Release development archive also passed an opt-in live Redshift connection
through an independent client built from the downloaded, checked artifact.
The other platform archives have not passed that live check, and none are
production releases.

## Connect from an ADBC client

Load the installed Redshift shared-library **path** using the ADBC driver
manager's `driver` database option. Set `uri` to a libpq-style Redshift endpoint,
typically `postgresql://HOST:5439/DATABASE?...`. Use TLS and your organization's
certificate-validation policy. Do not commit the URI if it contains a password;
use a secret store or an environment variable in private testing.
For temporary IAM credentials see [IAM recipe](#iam-temporary-credentials-recipe)
below.

The [standalone install smoke client](../pgwire/redshift_install_smoke.c) is a
minimal C example. It compiles only against the installed public ADBC header and
driver manager, then loads the Redshift driver dynamically:

```sh
prefix="$PWD/build/pgwire-prefix"
cc -std=c11 -I "$prefix/include" \
  c/driver/pgwire/redshift_install_smoke.c \
  -L "$prefix/lib" -Wl,-rpath,"$prefix/lib" \
  -ladbc_driver_manager -o build/redshift-install-smoke
build/redshift-install-smoke "$prefix/lib/libadbc_driver_redshift.so" \
  "$ADBC_REDSHIFT_TEST_URI"
```

On macOS use `libadbc_driver_redshift.dylib` instead of `.so`. The live smoke
connection is opt-in; do not set the URI in shared logs. CI runs this client
against PostgreSQL on Linux and macOS and requires Redshift's driver to reject
that server. It also connects through the independently installed client during
the existing Ubuntu x86-64 live Redshift gate, after relocating its installed
Debug CI build through a tarball. A separate opt-in run also connected the
checked Ubuntu x86-64 Release development archive to live Redshift.

On Windows, the [standalone CMake client](../pgwire/install_smoke/CMakeLists.txt)
builds from the installed ADBC driver-manager package. Its `--load-only` mode
checks that the installed Redshift DLL initializes through the driver manager
without opening a database connection. The Windows x64 and ARM64 development
archive jobs also build an independent client from each extracted ZIP, connect
through its PostgreSQL driver to PostgreSQL 18, and check that the Redshift
driver rejects that server. Live Windows-to-Redshift connectivity is still
unqualified.

## Current behavior and limits

| Area | MVP behavior |
| --- | --- |
| Authentication | Standard libpq URI, TLS, username/password; no built-in IAM or Identity Center credential generation |
| Queries | Text-result Arrow conversion; prepared statements and binary-bound parameters |
| Cancellation | A bounded analytic query was canceled successfully in two opt-in live runs, with a server-side timeout as a backstop and a post-cancel connection check; it remains outside default CI for cost control, and broader reliability qualification remains pending |
| Types | Tested scalar integers, floats, booleans, strings, decimal-as-string, date/time/timestamp, and `VARBYTE`; `SUPER` and spatial types are not qualified |
| Metadata | `GetInfo`, `GetTableTypes`, `GetObjects`, `GetTableSchema`; constraint/statistics discovery is unsupported |
| Transactions | Autocommit and explicit commit/rollback; per-session isolation overrides are unsupported |
| Ingest | Create/append/replace via parameterized inserts in bounded 16-row batches; **replace is non-atomic across DROP, CREATE and data transfer** and failure can lose the old table; no Redshift S3 `COPY` or `UNLOAD` path |
| Platforms | Ubuntu x86-64 has source-build, PostgreSQL 18, and checked Release development archive client connections to live Redshift; Ubuntu ARM64, Debian x86-64, and macOS Intel/Apple Silicon have source-build, PostgreSQL 18, and clean-prefix rejection checks; Windows x64/ARM64 checked development ZIPs pass extracted-client PostgreSQL 18 connection and Redshift-driver rejection checks; checked short-retention development archives are available for all seven CI targets, but production release artifacts and other platforms' live Redshift installed-client qualification remain pending |

`SUPER` is not mapped natively to Arrow. A live test verified that a small
`SUPER` array explicitly passed through `JSON_SERIALIZE(...)` is read as an
Arrow string. For values that fit Redshift's `VARCHAR` limit, a query can
select `JSON_SERIALIZE(super_column)` to request JSON text. This is not a
general, lossless `SUPER` strategy:
[AWS documents](https://docs.aws.amazon.com/redshift/latest/dg/JSON_SERIALIZE.html)
that serialization errors when the JSON exceeds the system's `VARCHAR` limit,
which is smaller than the maximum `SUPER` value. Do not use this expression as
an implicit driver conversion for arbitrary-sized values.

PostgreSQL 18 integration tests and focused live Redshift tests are CI gates on
the development branch. The developer guide records what each gate proves and
what it does not. Report unsupported behavior as an issue rather than assuming
a PostgreSQL-compatible operation is safe on Redshift.

## IAM temporary-credentials recipe

The C driver does not call AWS APIs itself.  Obtain a short-lived Redshift
password from IAM, then connect with a normal libpq URI.  The Python package
ships an optional helper (install the downloaded wheel with its `iam` extra)
that wraps this recipe; the steps below work from any language with any AWS SDK.

### Serverless (`redshift-serverless:GetCredentials`)

```python
import boto3
from adbc_driver_redshift.iam import build_uri, fetch_serverless_credentials

client = boto3.client("redshift-serverless", region_name="eu-north-1")
creds = fetch_serverless_credentials(
    client, workgroup_name="my-workgroup", db_name="dev", duration_seconds=900
)
uri = build_uri(
    host="my-workgroup.123456789012.eu-north-1.redshift-serverless.amazonaws.com",
    database="dev",
    db_user=creds["dbUser"],
    db_password=creds["dbPassword"],
    sslmode="require",
)
```

AWS API: [`GetCredentials`](https://docs.aws.amazon.com/redshift-serverless/latest/APIReference/API_GetCredentials.html)
(boto3 `redshift-serverless` client `get_credentials`).
Required IAM action `redshift-serverless:GetCredentials` on the workgroup;
when `dbName` is set the policy must also allow `dbname` for that database.
Parameters `workgroupName` (or `customDomainName`), `dbName`, `durationSeconds`
(900-3600, default 900) and `customDomainName`.  Response fields `dbUser`,
`dbPassword`, `expiration`, `nextRefreshTime`.

Equivalent AWS CLI:

```
aws redshift-serverless get-credentials \
  --workgroup-name my-workgroup --db-name dev --duration-seconds 900
```

### Provisioned (`redshift:GetClusterCredentials` / `GetClusterCredentialsWithIAM`)

```python
import boto3
from adbc_driver_redshift.iam import (
    build_uri, fetch_cluster_credentials, fetch_cluster_credentials_with_iam,
)

client = boto3.client("redshift", region_name="eu-north-1")

# Named database user (optionally auto-created)
creds = fetch_cluster_credentials(
    client, db_user="my_user", cluster_identifier="my-cluster",
    db_name="dev", duration_seconds=900, auto_create=False,
)
uri = build_uri(
    host="my-cluster.xxx.eu-north-1.redshift.amazonaws.com",
    database="dev", db_user=creds["DbUser"], db_password=creds["DbPassword"],
    sslmode="require",
)

# IAM identity mapped 1:1 to the database user
creds2 = fetch_cluster_credentials_with_iam(
    client, cluster_identifier="my-cluster", db_name="dev",
)
uri2 = build_uri(host="my-cluster.xxx.eu-north-1.redshift.amazonaws.com",
                 database="dev",
                 db_user=creds2["DbUser"], db_password=creds2["DbPassword"],
                 sslmode="require")
```

AWS APIs: [`GetClusterCredentials`](https://docs.aws.amazon.com/redshift/latest/APIReference/API_GetClusterCredentials.html)
and [`GetClusterCredentialsWithIAM`](https://docs.aws.amazon.com/redshift/latest/APIReference/API_GetClusterCredentialsWithIAM.html)
(boto3 `redshift` client `get_cluster_credentials` /
`get_cluster_credentials_with_iam`).  Required IAM actions
`redshift:GetClusterCredentials` (plus `redshift:CreateClusterUser` when
`AutoCreate` is true and `redshift:JoinGroup` when `DbGroups` is set) or
`redshift:GetClusterCredentialsWithIAM` for the IAM-mapped variant.  Common
parameters `DbUser`/`DbName`/`ClusterIdentifier`/`DurationSeconds` (900-3600)/
`CustomDomainName`/`DbGroups`/`AutoCreate`.  Response fields `DbUser`,
`DbPassword`, `Expiration` (and `NextRefreshTime` for the `WithIAM` variant).

### Credential lifetime and reconnect

Temporary passwords expire at `expiration` / `Expiration` (typically 900-3600 s).
Fetch fresh credentials before each new ADBC database connection; existing
connections remain valid until closed.  Do not cache a password past its
expiry and do not log the URI or password.  The helper never logs credentials.

### TLS

Always use `sslmode=require` (or stricter, e.g. `verify-full` with a trusted
CA bundle).  The driver delegates TLS to libpq.

### References

- [Using IAM authentication to generate database user credentials](https://docs.aws.amazon.com/redshift/latest/mgmt/generating-user-credentials.html)
- [GetCredentials API](https://docs.aws.amazon.com/redshift-serverless/latest/APIReference/API_GetCredentials.html)
- [GetClusterCredentials API](https://docs.aws.amazon.com/redshift/latest/APIReference/API_GetClusterCredentials.html)

## Isolated qualification preparation (Task008a)

Task007 is complete (PR #12). Task008a prepares offline controls only. External
host provisioner access is verified through 2027-12-31T23:00:00Z. Actual Task008
still requires dedicated resources, verified manifest/usage/cleanup admission and
a reviewed exact-head live runner; none is established by IAM availability.
Task009 publication is paused. This worker/coordinator remain offline and must
not use the provisioner profile. No dedicated resources or live tests exist yet.

Earlier scoped denials for dedicated Serverless/SG creation and the isolated CI
role are historical evidence, superseded as the current access blocker by the
external provisioner validation. The old `adbc-redshift-ci` role/boundary exception
never authorized fallback. The protected `odbcpp-redshift-provisioned-pilot`
cluster and its subnet/parameter groups also remain forbidden fallbacks.
Task008a performs no AWS recheck, provisioning or live qualification.

The legacy `cpp-redshift` and credential/COPY handoff in `PgWire Drivers` are
removed. Default dispatch runs PostgreSQL/artifact regression; retired live flags
fail without OIDC credentials or cloud actions. PR/push runs keep the original
PostgreSQL, installed-artifact and cleanup-mock gates. Do not dispatch live work.

### Manifest and external evidence

`ci/scripts/pgwire_qualification_manifest.py` defines a closed, versioned schema;
unknown/missing fields, duplicate JSON keys and non-finite values fail. No real
manifest is supplied: provisioned UUIDs, SG, endpoint observations and usage-limit
IDs do not exist yet. Synthetic test claims are explicitly `offline-synthetic`.
They never grant admission. Intended names/account/region are fixed to the human
plan: `adbc-qualification-20261016`, `adbc_qualification`,
`adbc-isolated-qualification-ci`, `385207570137`, `eu-north-1`.

Claims include exact workgroup/namespace ARN/UUID association, dedicated SG,
endpoint/port/database, OIDC role/provider/audience/merged-ref trust, policy/trust
hashes and the exact authorization resource list. Wildcards, shared-role fallback
and protected ODBCPP identifiers anywhere in the manifest fail. A structurally
correct ARN or attester identity is still only a claim.

`manifest_digest` is SHA256 of canonical sorted compact JSON excluding only the
`evidence` object. Four mandatory evidence references (provisioning, usage,
cleanup, source/artifacts) bind that digest, observation bytes SHA256, independent
review receipt SHA256, identified external attester, observation time and expiry.
Observations must be at most 15 minutes old and remain valid through teardown.
A future trusted external verifier must independently retrieve the authoritative
observations and review receipts, authenticate the attester through a separately
reviewed trust decision, and compare every identity/control to that exact digest.
This is deliberately **not implemented** by accepting a boolean, JSON receipt,
environment variable, arbitrary verifier callback or general signing service.
Both `admit_live` and `run_live` always reject while that boundary is unavailable.

The external observation bundle must establish: actual workgroup/namespace/SG
association and TLS hostname; effective dedicated IAM policy/trust and absence of
shared authorization; usage-limit identity/scope/action; capacity controls;
authorized provisioner and cleanup ownership; exact merged source, independent
review/current-head CI receipts and successful artifact workflow run provenance.
Structural validation and local checksums alone cannot prove those facts.

### Installed artifacts and coverage

`pgwire_qualification.py` compares installed C archive and wheel bytes/links to
checksum-bound artifacts and reviewed installed inventories before any credential
or ingress step. Both artifacts must have the same merged source SHA, identified
build runs/workflows and qualification-run binding. Wheel package/module/library
origins must match its installation prefix, with no source/fallback library or
extra package files. Install with `pip --no-compile` into a cache-free prefix;
preflight rejects `__pycache__`, legacy `.pyc`/`.pyo`, and wheel-contained bytecode.
The future `python -I -B` probe rechecks for caches before importing and forbids
an external bytecode-cache prefix. `-B` prevents writes but does not suppress
reads, so it is not sufficient by itself. Keep the verified installation
unchanged between preflight and probe. `pgwire_qualification_probe.py` is a future
isolated interpreter load-only probe of `AdbcDriverRedshiftInit`; rehearsal uses fake
reports and does not launch it. Trusted CA bytes are pinned; future connections
must use `verify-full`, that CA and the exact attested hostname, without fallback.

| Surface | Prepared coverage | Remaining before live qualification |
|---|---|---|
| Installed C archive | Byte/link inventory, load-only probe contract; existing smoke client query/version path | Wire actual isolated probe and bounded client execution; move secret URI out of argv and sanitize diagnostics |
| Installed Python wheel | Inventory and origin/entrypoint checks; fake-tested DBAPI query, bound parameter, GetInfo and two-row fixture-ingest payload | Invoke through the verified installed interpreter with real Arrow fixture and finite cancellation/close |
| Current IAM helper | Existing Serverless helper with fake GetCredentials client, 900-second credentials and verify-full URI tested offline | Call only after trusted admission; no new auth method, client/profile fallback or refresh framework |
| C vs Python | C scalar query; Python query/params/metadata/ingest payload | C parameters/ingest are **not covered** by this thin client; explicitly qualify or record this gap in the live batch |
| Cleanup | Rehearsal failure/cancellation/unknown-receipt paths; existing exact ingress lost-reply mocks retained | Independent authoritative cleanup confirmation and external teardown receipt |

The small follow-up is trusted evidence admission plus real installed-process/IAM/
ingress adapters with bounded termination, private credential transport and the
coverage gaps above. This preparation deliberately supplies no live workflow or
adapter. It cannot be enabled by a dispatch input. No broad C client expansion,
provisioning/IAM implementation or driver authentication refactor is included.

### Finite execution and cleanup handoff

Admission requires base/max 4 RPUs, optimization/growth disabled, a dedicated daily
4 RPU-hour usage limit with `deactivate` already present, USD5 incremental ceiling,
one 15-minute query window, at most 60 minutes through compute/teardown, and no
automatic paid retry. These controls are necessary, not an absolute total-bill cap
(storage, secrets/network and enforcement delay can add cost).

Before admission an identified authorized external provisioner and cleanup owner
must accept the teardown deadline. On any failure/cancellation, close owned
sessions, remove only run-owned fixture objects, and revoke only exact owned IPv4
/32 rule IDs (including lost-reply recovery using the existing ingress helper).
Independently confirm those receipts bound to the manifest digest and run ID.
Then hand teardown of only the new workgroup/namespace/ancillaries to the authorized
external provisioner. No resource teardown is implemented here. Unknown cleanup
is a blocking failed result; the rehearsal reports external teardown pending and
never reports live success or permits another target. Retain sanitized stage,
exit/cancellation and ownership evidence; never log credentials/URIs or put them
in argv/process diagnostics.

Offline checks (stdlib only, sockets/processes disabled in qualification tests):

```sh
python3 -m unittest discover -s ci/scripts -p test_pgwire_qualification.py -v
bash .github/scripts/test-pgwire-redshift-ingress.sh
bash .github/scripts/test-pgwire-redshift-copy-fixture.sh
```
