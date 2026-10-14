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

# ADBC Redshift development archive (macOS)

This is a short-lived CI build for evaluation, **not a production release** or
an OS/ABI compatibility guarantee. Choose the archive for your machine's
architecture (`arm64` for Apple Silicon or `x86_64` for Intel). The archive
contains the shared Redshift and PostgreSQL drivers, the ADBC driver manager,
public headers, CMake and pkg-config metadata, and license notices. Only the
shared-library client path is qualified; static archives may need dependencies
not included here.

Install Homebrew's `libpq` to provide the runtime library. Before extraction,
verify the adjacent checksum file:

```sh
LC_ALL=C shasum -a 256 -c adbc-redshift-dev-macos-<arch>-<commit>.tar.gz.sha256
mkdir adbc-redshift-dev
LC_ALL=C tar -xzf adbc-redshift-dev-macos-<arch>-<commit>.tar.gz \
  -C adbc-redshift-dev
```

Set your library search path to the extracted `lib/` directory, or install the
contents into a normal library path. The bundled `.pc` files retain the build's
default `/usr/local` prefix; when using the extracted tree in place, pass
`--define-variable=prefix=/absolute/path/to/adbc-redshift-dev` to `pkg-config`.
For an ADBC client using the driver manager, set the database's `driver`
option to the absolute path of `lib/libadbc_driver_redshift.dylib` and the `uri`
option to a libpq-style Redshift connection URI. Do not store passwords in
scripts, logs, or committed configuration.

## Load by name

Release archives (`adbc-driver-redshift-<version>-<platform>`) also contain
`redshift.toml`, an ADBC driver manifest. Its `Driver.shared` path starts with
the placeholder `@PREFIX@`, which must become the absolute path of the
extracted directory before the driver manager can use it. Run this inside the
extracted directory to install the manifest for the current user, then set the
database's `driver` option to `redshift` instead of a library path:

```sh
mkdir -p ~/Library/Application\ Support/ADBC/Drivers
sed "s|'@PREFIX@/|'$PWD/|" redshift.toml \
  > ~/Library/Application\ Support/ADBC/Drivers/redshift.toml
```

Any directory listed in `ADBC_DRIVER_PATH` also works (CI loads the driver by
name that way). The extracted path must not contain `'` or `|`.

CI compiles an independent client against the extracted archive, loads the
Redshift driver, and connects through the extracted PostgreSQL driver to a
temporary PostgreSQL 18 server on both macOS architectures. It does not
connect the packaged Redshift binary to Redshift. The driver
supports the tested query, metadata, transaction, and bounded
parameterized-ingest MVP; built-in IAM token generation, S3-staged
`COPY`/`UNLOAD`, and qualified `SUPER`/spatial mappings remain unavailable. See
the repository's `c/driver/postgresql/REDSHIFT.md` and
`docs/source/driver/pgwire-development.md` for current evidence and limits.
