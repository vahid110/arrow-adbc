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
[tracked roadmap](../../../docs/source/driver/pgwire-development.md) for test
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

## Connect from an ADBC client

Load the installed Redshift shared-library **path** using the ADBC driver
manager's `driver` database option. Set `uri` to a libpq-style Redshift endpoint,
typically `postgresql://HOST:5439/DATABASE?...`. Use TLS and your organization's
certificate-validation policy. Do not commit the URI if it contains a password;
use a secret store or an environment variable in private testing.

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
the existing Ubuntu x86-64 live Redshift gate.

On Windows, the [standalone CMake client](../pgwire/install_smoke/CMakeLists.txt)
builds from the installed ADBC driver-manager package. Its `--load-only` mode
checks that the installed Redshift DLL initializes through the driver manager
without opening a database connection. The Windows x64 and ARM64 vcpkg jobs
exercise this mode; live Windows-to-Redshift connectivity is still unqualified.

## Current behavior and limits

| Area | MVP behavior |
| --- | --- |
| Authentication | Standard libpq URI, TLS, username/password; no built-in IAM or Identity Center credential generation |
| Queries | Text-result Arrow conversion; prepared statements and binary-bound parameters |
| Types | Tested scalar integers, floats, booleans, strings, decimal-as-string, date/time/timestamp, and `VARBYTE`; `SUPER` and spatial types are not qualified |
| Metadata | `GetInfo`, `GetTableTypes`, `GetObjects`, `GetTableSchema`; constraint/statistics discovery is unsupported |
| Transactions | Autocommit and explicit commit/rollback; per-session isolation overrides are unsupported |
| Ingest | Atomic parameterized inserts in bounded 16-row SQL batches; no Redshift S3 `COPY` or `UNLOAD` path |
| Platforms | Ubuntu x86-64 has source-build, PostgreSQL 18, and clean-prefix client connection to live Redshift; Ubuntu ARM64, Debian x86-64, and macOS Intel/Apple Silicon have source-build, PostgreSQL 18, and clean-prefix rejection checks; Windows x64/ARM64 Release builds and installed DLL load checks pass through the public CMake package; release artifacts and other platforms' database-backed installed-client qualification remain pending |

PostgreSQL 18 integration tests and focused live Redshift tests are CI gates on
the development branch. The roadmap records what each gate proves and what it
does not. Report unsupported behavior as an issue rather than assuming a
PostgreSQL-compatible operation is safe on Redshift.
