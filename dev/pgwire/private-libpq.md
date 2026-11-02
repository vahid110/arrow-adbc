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

The Redshift mixed-result dependency is an opt-in package foundation. The driver
still links its existing stock libpq; this change does not enable mixed HLL in
installed drivers. A private macOS arm64 prototype passed all-symbol isolation,
stock/private static and shared coexistence, and the loopback vector protocol
proof. This repository recipe and its installed consumers require coordinator
validation before adoption. Windows, cross builds, seven C distributions and
five wheels remain mandatory later release work, not supported by this recipe.

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
prepared but unqualified until built on each intended runner. No release/wheel
workflow or driver CMake/Meson dependency has been switched by this foundation.

Windows x64/arm64 C packages and the Windows x64 wheel must be added before B6
can close: use PostgreSQL 17.11's Meson static frontend libpq/common/port variants,
including their private encoding aliases; disable PCH/LTO for the initial proof;
generate the namespace from that exact COFF trio and apply `/FI` during rebuild.
Extend the inventory reader for reviewed COFF symbol kinds/compiler globals,
preserve the existing static CRT triplets, and require the same exact-definition
and reference audits. Never reuse the Darwin map or mix Meson static frontend
objects with Make's shlib variants. Windows/cross builds currently fail explicitly.
There is no silent fallback to ordinary libpq.

Then validate all seven C distributions and five wheels: installed/relocated
shared and static consumers, both drivers plus stock libpq in both load orders,
whole-archive collision checks, repaired-wheel imports/exports and dependencies,
private source/license provenance, and missing/incorrect helper/ABI rejection.
Driver namespace linkage and per-column result dispatch are separate next tasks.

Offline tests use small fake archives for positive and negative CMake/Meson link
gates; they do not compile PostgreSQL:

```sh
python3 -m unittest discover -s ci/scripts -p 'test_pgwire_private_pq*.py' -v
```
