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

# ADBC Amazon Redshift Driver for Python

This package contains bindings for the libpq-based Amazon Redshift driver
(`libadbc_driver_redshift`, entrypoint `AdbcDriverRedshiftInit`), using the
[driver manager][driver-manager] to provide a [DBAPI 2.0/PEP
249-compatible][dbapi] interface on top.

[dbapi]: https://peps.python.org/pep-0249/
[driver-manager]: https://arrow.apache.org/adbc/current/python/driver_manager.html

## Example

The URI is a libpq connection URI. Redshift listens on port 5439 by
default, and `sslmode=require` is recommended. The driver rejects servers
that are not Redshift.

```python
import adbc_driver_redshift.dbapi

uri = "postgresql://user:password@host:5439/dev?sslmode=require"
with adbc_driver_redshift.dbapi.connect(uri) as conn:
    with conn.cursor() as cur:
        cur.execute("SELECT 1")
        print(cur.fetch_arrow_table())
```

## IAM temporary credentials

The driver does not generate AWS credentials itself.  Use temporary
credentials from IAM and build the libpq URI with the optional helper.

Install the extra:

```
pip install adbc-driver-redshift[iam]
# or: pip install boto3
```

The helper has no hard ``boto3`` dependency; it accepts an already-created
client so tests can inject a fake.

```python
import boto3
from adbc_driver_redshift.iam import (
    build_uri,
    fetch_cluster_credentials,
    fetch_serverless_credentials,
)

# Serverless (redshift-serverless:GetCredentials)
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

# Provisioned (redshift:GetClusterCredentials)
client2 = boto3.client("redshift", region_name="eu-north-1")
creds2 = fetch_cluster_credentials(
    client2, db_user="my_user", cluster_identifier="my-cluster", db_name="dev"
)
uri2 = build_uri(host="my-cluster.xxx.eu-north-1.redshift.amazonaws.com",
                 database="dev",
                 db_user=creds2["DbUser"],
                 db_password=creds2["DbPassword"],
                 sslmode="require")

# With IAM identity mapping (redshift:GetClusterCredentialsWithIAM)
from adbc_driver_redshift.iam import fetch_cluster_credentials_with_iam
creds3 = fetch_cluster_credentials_with_iam(client2, cluster_identifier="my-cluster")

import adbc_driver_redshift.dbapi
with adbc_driver_redshift.dbapi.connect(uri) as conn:
    with conn.cursor() as cur:
        cur.execute("SELECT 1")
```

Credentials expire (900-3600 s, typically 900 s).  Fetch fresh credentials
before each new connection; existing connections stay valid until closed but
new ones need a new password.  Do not log the URI or password.

See [Use IAM authentication to generate database user credentials][iam-creds]
and [GetCredentials][get-creds] / [GetClusterCredentials][get-ccreds] for IAM
policies.  A minimal recipe is in `c/driver/postgresql/REDSHIFT.md` and
`docs/source/driver/pgwire-development.md`.

[iam-creds]: https://docs.aws.amazon.com/redshift/latest/mgmt/generating-user-credentials.html
[get-creds]: https://docs.aws.amazon.com/redshift-serverless/latest/APIReference/API_GetCredentials.html
[get-ccreds]: https://docs.aws.amazon.com/redshift/latest/APIReference/API_GetClusterCredentials.html

## Building

Dependencies: a build of the Redshift driver (`-DADBC_DRIVER_POSTGRESQL=ON`
builds both the PostgreSQL and Redshift drivers), and the
`adbc-driver-manager` Python package. Optionally, install PyArrow to use the
DBAPI 2.0-compatible interface.

Set the environment variable `ADBC_REDSHIFT_LIBRARY` to the path to
`libadbc_driver_redshift.{dll,dylib,so}` before running `pip install`.

```
# If not already installed
pip install -e ../adbc_driver_manager

export ADBC_REDSHIFT_LIBRARY=/path/to/libadbc_driver_redshift.so
pip install --no-deps -e .
```

## Testing

Without Redshift, the tests check the package and that the driver rejects
a PostgreSQL server. Start PostgreSQL, for example with Docker:

```shell
$ docker run -it --rm -e POSTGRES_PASSWORD=password -p 5432:5432 postgres
$ export ADBC_POSTGRESQL_TEST_URI="postgresql://localhost:5432/postgres?user=postgres&password=password"
$ pytest -vvx
```

Live Redshift tests run only when `ADBC_REDSHIFT_TEST_URI` is set.
