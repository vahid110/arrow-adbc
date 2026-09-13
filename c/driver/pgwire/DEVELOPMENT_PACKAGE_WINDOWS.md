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

The development driver supports the tested query, metadata, transaction, and
bounded parameterized-ingest MVP. Built-in IAM token generation, S3-staged
`COPY`/`UNLOAD`, and qualified `SUPER`/spatial mappings remain unavailable. See
the repository's `c/driver/postgresql/REDSHIFT.md` and
`docs/source/driver/pgwire-development.md` for current evidence and limits.
