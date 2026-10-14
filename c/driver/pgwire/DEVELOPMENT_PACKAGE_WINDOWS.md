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

# ADBC Redshift development archive (Windows)

This is a short-lived CI build for evaluation, **not a production release** or
an OS/ABI compatibility guarantee. Choose `windows-x64` or `windows-arm64` for
your machine. The archive contains the Redshift and PostgreSQL driver DLLs,
the ADBC driver manager, import libraries, public headers, CMake metadata,
Apache license notices, and `THIRD_PARTY_LICENSES/` for the linked vcpkg ports.
The package is built with a dynamic Microsoft C runtime, so the matching
Microsoft Visual C++ Redistributable may be required on another machine.

The workflow publishes a ZIP and adjacent SHA-256 file. Verify the ZIP before
extracting it; for example, in PowerShell:

```powershell
$archive = "adbc-redshift-dev-windows-<arch>-<commit>.zip"
$expected = (Get-Content "$archive.sha256").Split(' ')[0]
$actual = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $expected) { throw "Archive checksum mismatch" }
Expand-Archive $archive -DestinationPath adbc-redshift-dev
```

For an ADBC client using the driver manager, set the database's `driver`
option to the absolute path of `bin\adbc_driver_redshift.dll` and `uri` to a
libpq-style Redshift connection URI. Do not store passwords in scripts, logs,
or committed configuration. The workflow compiles a client against the
extracted CMake package, connects through the PostgreSQL DLL to a temporary
PostgreSQL 18 server, and checks that the Redshift DLL rejects that server.
It does not connect the packaged binary to a live Redshift server.

## Load by name

Release archives (`adbc-driver-redshift-<version>-<platform>`) also contain
`redshift.toml`, an ADBC driver manifest. Its `Driver.shared` path starts with
the placeholder `@PREFIX@`, which must become the absolute path of the
extracted directory before the driver manager can use it. Run this inside the
extracted directory to install the manifest for the current user, then set the
database's `driver` option to `redshift` instead of a library path:

```powershell
$drivers = Join-Path $env:LOCALAPPDATA "ADBC\Drivers"
New-Item -ItemType Directory -Force $drivers | Out-Null
$manifest = (Get-Content -Raw redshift.toml).Replace(
  "'@PREFIX@\", "'$((Get-Location).Path)\")
[System.IO.File]::WriteAllText((Join-Path $drivers "redshift.toml"), $manifest)
```

Any directory listed in `ADBC_DRIVER_PATH` also works (CI loads the driver by
name that way). The manifest keeps the backslash path in a TOML literal string
(`'...'`), so the extracted path must not contain `'`.

The development driver supports the tested query, metadata, transaction, and
bounded parameterized-ingest MVP. Built-in IAM token generation, S3-staged
`COPY`/`UNLOAD`, and qualified `SUPER`/spatial mappings remain unavailable. See
the repository's `c/driver/postgresql/REDSHIFT.md` and
`docs/source/driver/pgwire-development.md` for current evidence and limits.
