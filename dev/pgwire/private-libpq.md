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

# Private libpq package foundation

The Redshift artifact requires the pinned private dependency and uses its format
vector for mixed HLL/non-HLL results. PostgreSQL implementation translation units
continue to use stock libpq; the Redshift translation units uniformly include the
private namespace wrapper. No PGconn or PGresult crosses this boundary. Local
recorder evidence is not installed-driver/native qualification. Windows, cross
builds, seven C distributions and five wheels remain mandatory release gates.

Native package, relocation, stock/private coexistence and loopback vector proofs
passed on macOS arm64. The mixed driver still needs coordinator full/installed
validation and independent review before integration.

## Build and consume locally

Only the coordinator builds dependencies on the constrained development host.
Supply the pinned PostgreSQL 17.11 archive from
[the PostgreSQL source distribution](https://ftp.postgresql.org/pub/source/v17.11/postgresql-17.11.tar.bz2).
The recipe validates source/patch/license/script hashes before creating output or
running a subprocess, even under optimized Python. It performs no download,
database connection or system installation. Both output directories must be new;
the prefix must be inside the explicitly owned work directory. Failures are kept.

```sh
python3 ci/scripts/pgwire_private_pq.py \
  --source-archive "$private_pq_archive" \
  --work-dir "$private_pq_work" \
  --prefix "$private_pq_work/install" \
  --openssl-prefix "$private_pq_openssl"
```

All paths must be absolute and contain no whitespace. The default is native
O0/g0; `--build-type release` selects O2/g0. Use `--openssl-libdir` for Linux
multiarch/lib64 installations. No existing source, package or stock libpq is
modified. Two serial native builds use identical features, PIC, hidden visibility
and compiler flags: first inventory, then namespace rebuild. Each explicitly
generates PostgreSQL's required headers before the direct archive targets.

The complete defined global sets of libpq, common and port—including data,
platform fallbacks, encoding aliases and the package ABI marker—are prefixed.
The final audit requires exactly the renamed set and rejects original internal
references and unresolved private references. No PQ-only allowlist or PGconn
layout access is used. The small namespace patch preserves upstream macro aliases.

The package exports `AdbcRedshiftPrivatePQ::pq` through
`find_package(AdbcRedshiftPrivatePQ CONFIG REQUIRED)`. CMake links a required
marker/version/helper probe on every configure, including when a caller sets
`CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`. No target injects namespace macros
into application consumers. Private implementation sources explicitly include
`adbc_rs_pq.h`; stock libpq belongs in a separate translation unit.

Meson uses `dependency('adbc-redshift-private-pq', static: true, method: 'pkg-config')`,
checks its `abi` variable, and links the supplied `link_check.c`. The consumer
fixture demonstrates the required checks, plus actual shared/static consumers
coexisting with stock libpq without opening a connection:

```sh
private_pq_consumer=ci/vcpkg/ports/adbc-redshift-private-pq/consumer
cmake -S "$private_pq_consumer" -B "$private_pq_work/cmake-consumer" \
  -DCMAKE_PREFIX_PATH="$private_pq_work/install" \
  -DOPENSSL_ROOT_DIR="$private_pq_openssl"
cmake --build "$private_pq_work/cmake-consumer" --parallel 1
ctest --test-dir "$private_pq_work/cmake-consumer" --output-on-failure
meson setup "$private_pq_work/meson-consumer" "$private_pq_consumer"
meson compile -C "$private_pq_work/meson-consumer" -j1
meson test -C "$private_pq_work/meson-consumer" --print-errorlogs
```

Set `PKG_CONFIG_PATH` to the private prefix, explicit OpenSSL and stock libpq
pkg-config directories for these fixtures. Run them again after moving/copying
the prefix to test relocation. The dependency archives are statically absorbed
into a shared driver; static C consumers receive their ordered transitive link
dependencies. External OpenSSL remains a compatible shared dependency or a single
compatible static dependency; this is not isolation of conflicting OpenSSL ABIs.

`share/adbc-redshift-private-pq/provenance.json` records recipe/source hashes,
target/compiler/configuration, commands and log hashes, symbol inventory, and
installed file hashes. Exact recipe sources and PostgreSQL COPYRIGHT accompany
the package. This is reproducible input/provenance tracking, not a promise of
bit-identical archives across different compiler/SDK/install paths.

## Portable completion remains required

The uniquely named vcpkg overlay `adbc-redshift-private-pq` uses the same script
for native Linux/macOS static-release triplets. It never replaces the ordinary
`libpq` port, and script digests participate in its cache inputs. This adapter is
prepared but unqualified until built on each intended runner. The seven C package
and five wheel jobs now request the unique private dependency; Unix package and
fast driver/Python CI build the pinned source directly, while wheels and Windows
use the overlay. Ordinary PostgreSQL still uses its original libpq dependency.
Source downloads require both pinned hashes. Packaging verifies the full private
installation against its build receipt and carries licenses, patches, namespace
inventory and the exact build recipe; wheels carry these under `_private_pq`.
For a local wheel, first run `pgwire_private_pq_distribution.py stage <prefix>
python/adbc_driver_redshift/adbc_driver_redshift/_private_pq` against the matching
private dependency install; the wheel build rejects absent provenance.
PostgreSQL-only builds use stock libpq by default. To also build the dedicated
Redshift driver, use both `-DADBC_DRIVER_POSTGRESQL=ON -DADBC_DRIVER_REDSHIFT=ON`
in CMake or `-Dpostgresql=enabled -Dredshift=true` in Meson. That build
requires the private package and rejects a missing or incompatible ABI/helper.

The Windows adapter now selects PostgreSQL 17.11's Meson static frontend
libpq/common/port targets, including their private encoding aliases. It disables
PCH/LTO and string pooling, uses the existing static-library/dynamic-CRT (`/MD`)
triplets, and rejects cross builds. Tool paths, source pins, target machine and
runtime directives are checked explicitly. The first pass inventories COFF;
the second uses `/FI` for C identifiers and LLVM objcopy for only the exact
inventoried numeric constants which cannot be C macro names. Unknown special
symbols fail. The final audit requires every original definition to be prefixed,
no original internal references, and unchanged compiler metadata.

The Windows C jobs now include original/relocated static and shared consumers
alongside stock libpq, plus a whole-archive collision probe. These jobs and the
adapter have only source/mock validation locally: native x64/ARM64 compilation,
COFF rewrites, link/runtime behavior and Windows wheel repair remain unverified.
No Windows success or release readiness is claimed. Ordinary libpq remains a
separate dependency; there is no private-dependency fallback.

Then validate all seven C distributions and five wheels: installed/relocated
shared and static consumers, both drivers plus stock libpq in both load orders,
whole-archive collision checks, repaired-wheel imports/exports and dependencies,
private source/license provenance, and missing/incorrect helper/ABI rejection.
Driver namespace linkage and per-column dispatch are implemented locally;
portable and installed/native evidence remain required before release.

Offline tests use small fake archives for positive and negative CMake/Meson link
gates; they do not compile PostgreSQL:

```sh
python3 -m unittest discover -s ci/scripts -p 'test_pgwire_private_pq*.py' -v
```
