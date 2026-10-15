.. Licensed to the Apache Software Foundation (ASF) under one
.. or more contributor license agreements.  See the NOTICE file
.. distributed with this work for additional information
.. regarding copyright ownership.  The ASF licenses this file
.. to you under the Apache License, Version 2.0 (the
.. "License"); you may not use this file except in compliance
.. with the License.  You may obtain a copy of the License at
..
..   http://www.apache.org/licenses/LICENSE-2.0
..
.. Unless required by applicable law or agreed to in writing,
.. software distributed under the License is distributed on an
.. "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
.. KIND, either express or implied.  See the License for the
.. specific language governing permissions and limitations
.. under the License.

:orphan:

======================
Amazon Redshift Driver
======================

This fork's dedicated ``adbc_driver_redshift`` library uses libpq to query
Amazon Redshift and return Arrow data. It rejects non-Redshift servers.
The implementation is fully open source: inspect, build, modify and redistribute
it under the existing applicable licenses and third-party notices. This fork
is the independent ADBC delivery route, separate from the Foundry Redshift driver.
It is a development driver.
**No public release or PyPI publication is claimed here.** Installed-artifact
live qualification for v1 is still pending.

Installation
============

Download artifacts from a successful run for the commit you want to evaluate:

* `C/C++ development archives <https://github.com/vahid110/arrow-adbc/actions/workflows/pgwire-package.yml>`__
* `Portable Python wheels <https://github.com/vahid110/arrow-adbc/actions/workflows/pgwire-python-wheels.yml>`__
* `Release dry-run artifacts <https://github.com/vahid110/arrow-adbc/actions/workflows/pgwire-release.yml>`__

CI artifacts have seven-day retention and may have expired. A draft GitHub
Release, when prepared, is a candidate for review, not a published release.
Use a matching OS/CPU artifact; keep its version and checksum with your deployment.

C/C++ archives
--------------

Development downloads contain an archive and adjacent ``.sha256`` file.
Verify before extracting (Linux example; substitute the actual filename):

.. code-block:: sh

   archive='adbc-redshift-dev-ubuntu24-x86_64-<commit>.tar.gz'
   sha256sum -c "$archive.sha256"
   mkdir adbc-redshift
   tar -xzf "$archive" -C adbc-redshift

The extracted tree contains headers, driver manager, Redshift and PostgreSQL
libraries, and CMake/pkg-config metadata. Follow its embedded ``README.md``
for macOS ``shasum`` or Windows PowerShell checksum/extraction commands and
library search paths. Linux needs system ``libpq5``; macOS needs Homebrew
``libpq``. Windows links libpq statically but may need the matching Visual
C++ Redistributable. Only shared-library clients are qualified; static
consumers must supply transitive dependencies.

Release-format archives use ``SHA256SUMS`` and an enclosing versioned directory;
follow their README. They also contain ``redshift.toml``: replace ``@PREFIX@``
with the extracted absolute path using the README's **Load by name** instructions
before using ``driver=redshift``. Development archives can always be loaded by
absolute library path.

Python wheels
-------------

Use Python 3.10 or newer. Unpack the downloaded CI artifact, then install the
actual wheel file in your environment (replace the placeholder filename):

.. code-block:: sh

   python -m pip install '/path/to/adbc_driver_redshift-<version>-<tags>.whl[dbapi]'
   # For the IAM example below, include the optional SDK extra:
   python -m pip install '/path/to/adbc_driver_redshift-<version>-<tags>.whl[dbapi,iam]'

The ``dbapi`` extra installs PyArrow (>=14.0.1) and pandas; the package also
requires ``adbc-driver-manager``. The ``iam`` extra adds boto3 (>=1.28).
Portable wheels bundle the driver and its libpq dependencies. The separate
``PgWire Python`` fast Linux wheel uses system libpq and is not the portable
manylinux wheel. These commands install a downloaded wheel; a bare
``pip install adbc-driver-redshift`` is not a publication promise.

Connect
=======

Use a libpq ``postgresql://`` URI with your Redshift endpoint and port
(usually 5439). The URI scheme does not choose the driver.
Use TLS: ``sslmode=verify-full`` validates the certificate and hostname with a
trusted CA bundle; ``sslmode=require`` requires encryption but does not provide
hostname verification. See `libpq TLS settings <https://www.postgresql.org/docs/current/libpq-ssl.html>`__.

Password (Python)
-----------------

Supply the following environment values privately: ``REDSHIFT_HOST``,
``REDSHIFT_DATABASE``, ``REDSHIFT_USER``, ``REDSHIFT_PASSWORD``, and
``REDSHIFT_CA_FILE`` (path to a trusted CA bundle). The URI builder escapes
user names, passwords and database names; it does not need boto3.

.. code-block:: python

   import os
   from adbc_driver_redshift import dbapi
   from adbc_driver_redshift.iam import build_uri

   uri = build_uri(
       host=os.environ["REDSHIFT_HOST"],
       database=os.environ["REDSHIFT_DATABASE"],
       db_user=os.environ["REDSHIFT_USER"],
       db_password=os.environ["REDSHIFT_PASSWORD"],
       sslmode="verify-full",
       extra_params={"sslrootcert": os.environ["REDSHIFT_CA_FILE"]},
   )
   with dbapi.connect(uri) as conn:
       with conn.cursor() as cur:
           cur.execute("SELECT 1")
           print(cur.fetch_arrow_table())

DBAPI defaults to ``autocommit=False``; call ``conn.commit()`` to persist
writes or pass ``autocommit=True``. Treat the whole URI as a secret: never
print it, put it in command-line arguments, commit it, or include it in logs.
Use a secret manager or private process environment; avoid shell history.

C/C++ driver loading
--------------------

Link your application to the archive's ADBC driver manager. Before
``AdbcDatabaseInit``, set these database options with
``AdbcDatabaseSetOption`` (check every returned status):

.. list-table::
   :header-rows: 1

   * - Option
     - Value
   * - ``driver``
     - Absolute path to ``lib/libadbc_driver_redshift.so`` (Linux),
       ``lib/libadbc_driver_redshift.dylib`` (macOS), or
       ``bin/adbc_driver_redshift.dll`` (Windows)
   * - ``entrypoint``
     - ``AdbcDriverRedshiftInit``
   * - ``uri``
     - Your private libpq URI (password or temporary IAM credentials)

Then initialize an ``AdbcConnection`` from that database. Release statements,
connections, databases and errors when finished. The repository's
`standalone C client <https://github.com/vahid110/arrow-adbc/blob/feature/pgwire-core-redshift/c/driver/pgwire/redshift_install_smoke.c>`__
shows the complete lifecycle. Loading the PostgreSQL library does not select
this dedicated Redshift driver. Python's ``dbapi.connect`` selects the bundled
Redshift library and entrypoint automatically.

IAM temporary credentials
-------------------------

The driver does not obtain AWS credentials or refresh database passwords.
V1's optional boto3 helper is a thin, caller-owned, replaceable adapter;
it adds no native auth framework, shared credential cache or background refresh.
With an existing authorized boto3 session, replace the password example's
``uri`` construction with this Serverless example, then use the same
``dbapi.connect(uri)`` block:

.. code-block:: python

   import boto3
   from adbc_driver_redshift.iam import fetch_serverless_credentials

   client = boto3.client("redshift-serverless", region_name="eu-north-1")
   creds = fetch_serverless_credentials(
       client, workgroup_name="my-workgroup", db_name="dev", duration_seconds=900
   )
   uri = build_uri(
       host=os.environ["REDSHIFT_HOST"], database="dev",
       db_user=creds["dbUser"], db_password=creds["dbPassword"],
       sslmode="verify-full",
       extra_params={"sslrootcert": os.environ["REDSHIFT_CA_FILE"]},
   )

Use your actual region, workgroup, endpoint and database. The existing
`IAM recipe <https://github.com/vahid110/arrow-adbc/blob/feature/pgwire-core-redshift/c/driver/postgresql/REDSHIFT.md#iam-temporary-credentials-recipe>`__
covers permissions and provisioned clusters (``fetch_cluster_credentials`` /
``fetch_cluster_credentials_with_iam``). C/C++ callers can obtain the same
credentials with their AWS SDK and pass an escaped libpq URI.

Honor the response's ``expiration`` / ``Expiration``; Serverless credentials
last 900 seconds by default, up to 3600 seconds (`GetCredentials API <https://docs.aws.amazon.com/redshift-serverless/latest/APIReference/API_GetCredentials.html>`__).
Fetch fresh credentials and construct a new URI before opening or reconnecting
a database. Expiry limits new logins; it does not itself close an established
session. The helper does not renew credentials or reconnect for you. Its tests
are offline; a live IAM connection check remains deferred.

Type mapping (query results)
============================

.. list-table::
   :header-rows: 1

   * - Redshift type
     - Arrow type
   * - ``BOOLEAN``
     - ``bool``
   * - ``SMALLINT``, ``INTEGER``, ``BIGINT``
     - ``int16``, ``int32``, ``int64``
   * - ``REAL``, ``DOUBLE PRECISION``
     - ``float32``, ``float64``
   * - ``DECIMAL`` / ``NUMERIC``
     - UTF-8 string (preserves precision, not Arrow decimal)
   * - ``CHAR``, ``VARCHAR`` / text
     - UTF-8 string
   * - ``VARBYTE``
     - Binary
   * - ``DATE``, ``TIME``, ``TIMESTAMP``, ``TIMESTAMPTZ``
     - ``date32``, ``time64[us]``, ``timestamp[us]``, ``timestamp[us, UTC]``

Timestamps outside Arrow's signed 64-bit microsecond range return an error.
``SUPER``, ``GEOMETRY`` and ``GEOGRAPHY`` have no native mapping. Explicit
``JSON_SERIALIZE(super_column)`` can return text for values that fit Redshift's
``VARCHAR`` limit; it is not a lossless strategy for arbitrary ``SUPER`` values.

Platform evidence and limits
============================

These are tested build targets, not a production support or ABI guarantee.
Archive checks use an extracted client, a PostgreSQL query through the
PostgreSQL driver, and rejection of PostgreSQL by the Redshift driver.
Portable wheel checks cover installation/import and PostgreSQL rejection.

.. list-table::
   :header-rows: 1

   * - Target
     - C/C++ archive
     - Portable wheel
   * - Ubuntu 24.04 x86-64
     - Yes; historical live Redshift archive check
     - manylinux_2_28 x86-64
   * - Ubuntu 24.04 ARM64
     - Yes; no live archive check
     - manylinux_2_28 aarch64
   * - Debian 12 x86-64
     - Yes; no live archive check
     - manylinux_2_28 x86-64
   * - macOS Intel / Apple Silicon
     - Yes; no live archive check
     - x86-64 / arm64 (deployment targets 10.15 / 11.0)
   * - Windows x64 / ARM64
     - Yes; no live archive check
     - x64 only

Linux wheels require glibc >=2.28; musl/Alpine is not qualified. The macOS
minimums are build targets, not tests on those oldest OS versions. No installed
wheel has passed the planned live v1 qualification batch. Other archive
OS/CPU combinations are not qualified.

* Queries, prepared/bound parameters, core metadata (``GetInfo``,
  ``GetTableTypes``, ``GetObjects``, ``GetTableSchema``), autocommit,
  commit/rollback and cancellation are implemented. Cancellation has bounded
  historical live evidence, not broad reliability qualification.
* Ingest create/append/replace uses prepared multi-row ``INSERT`` (16-row
  batches, at most 32,767 parameters per statement), not S3 ``COPY``.
  **Replace is non-atomic across DROP, CREATE and data transfer. Failure can
  leave the old table lost or the replacement incomplete, even inside an
  explicit transaction.** Validate inputs and protect important data before
  replacing. Identifier-collision preflight is offline-tested, not live-qualified.
* No staged S3 ``COPY``/``UNLOAD``, native IAM/Identity Center credential
  generation, session isolation overrides, constraint/statistics discovery,
  or special reporting of external tables/materialized views. Do not assume
  PostgreSQL feature compatibility.
* Close an unusable connection after failed/unconfirmed transaction or cleanup
  operations. The driver never retries; a lost commit reply can leave the
  durable outcome unknown. Concurrent connection use and remaining timezone
  cleanup failure edges are not qualified.

Current qualification blockers (including CI OIDC role assumption), deferred
live checks and release status are tracked in
`STATUS.md <https://github.com/vahid110/arrow-adbc/blob/feature/pgwire-core-redshift/dev/pgwire/STATUS.md>`__.
