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
