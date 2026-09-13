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

# ADBC Redshift development archive (Linux)

This is a short-lived CI build for evaluation, **not a production release** or
an ABI/platform compatibility guarantee. Choose the archive matching your
distribution and CPU: Ubuntu 24.04 x86-64 or ARM64, or Debian Bookworm x86-64.
The archive contains the installed ADBC Redshift and PostgreSQL driver
libraries, the ADBC driver manager, public
headers, CMake and pkg-config metadata, and license notices. The CI job builds
a client against the archive after extracting it to a different path, loads
the Redshift driver, and connects through the extracted PostgreSQL driver to
PostgreSQL 18. It does not connect the packaged Redshift driver to Redshift.
Only the shared-library client path is qualified; bundled static archives may
require dependencies that are not included here.

Before extraction, verify the adjacent checksum file:

```sh
sha256sum -c adbc-redshift-dev-<platform>-<commit>.tar.gz.sha256
mkdir adbc-redshift-dev
tar -xzf adbc-redshift-dev-<platform>-<commit>.tar.gz \
  -C adbc-redshift-dev
```

The shared libraries require a compatible system `libpq` at runtime (the
`libpq5` package on Ubuntu and Debian). Set your library search path to the
extracted
`lib/` directory, or install the archive contents into a normal library path.
The bundled `.pc` files retain the build's default `/usr/local` prefix; when
using the extracted tree in place, pass
`--define-variable=prefix=/absolute/path/to/adbc-redshift-dev` to `pkg-config`.
For an ADBC client using the driver manager, set the database's `driver`
option to the absolute path of `lib/libadbc_driver_redshift.so` and the `uri`
option to a libpq-style Redshift connection URI. Do not store passwords in
scripts, logs, or committed configuration.

The development driver supports the tested query, metadata, transaction, and
bounded parameterized-ingest MVP. It does not yet include built-in IAM token
generation, S3-staged `COPY`/`UNLOAD`, or qualified `SUPER`/spatial mappings.
See the repository's `c/driver/postgresql/REDSHIFT.md` and
`docs/source/driver/pgwire-development.md` for details and current test evidence.
