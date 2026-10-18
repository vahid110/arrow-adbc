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

# Local evidence linking (Task010g0)

`ci/scripts/pgwire_local_evidence.py` is a separate standard-library, read-only
linker for historical local proof. It does not change the CI qualification
verifiers. The API is `verify_local_evidence(record_bytes, *, selected, roots)`.
`Selected`, `Blob`, `Step`, `Limits` and `Roots` describe separately root-selected
immutable expectations and explicit canonical evidence/artifact directories.

The strict version 1 `adbc-local-evidence-link` record binds repository,
commit/tree/local integration ref and a local ID. Seven anchored evidence roles
cover the source freeze, review receipt/body/checks, root validation, validator
source and case plan. Three artifact roles cover the C snapshot and both wheels.
No GitHub identities or CI/native-success fields are accepted. Root accepts the
independent review and selects hashes outside this API; a caller's name or a
self-consistent success sidecar establishes no trust.

The linker matches historical source maps and exact review identity/approval;
binds referenced body/check/validation/source bytes; requires the selected
ordered commands and terminal zero exits; and hashes every actual step log and
package file. It never executes stored commands, parses review prose as approval
or treats a plan as execution. Historical pending fields in the source freeze
remain unchanged: the separate final review and validation receipts supply their
own anchored facts. A newly selected historical case plan is not retrospective
before-execution attestation.

Canonical record JSON uses sorted keys, compact separators, ASCII escapes and
UTF8 bytes; original input and canonical payload digests remain separate. Unknown
record/reference fields, duplicate JSON keys, floats/nonfinite values and bool
counts reject. Reads reject path aliases, escape, symlink traversal, nonregular
files, size excess and observed identity changes. Limits only tighten: 1 MiB
record/semantic JSON, 64 MiB body/checks/log/source, 512 MiB compressed artifact,
128 steps, 256 arguments per step, 16,384 characters per string and 10,000 source
map entries. No tar/zip extraction, package import, dynamic load or scan occurs.

Success returns a frozen `LinkedEvidence` with `status=local-evidence-linked`,
input/payload/evidence/log/artifact digests. This proves consistency with selected
historical evidence only. It does not prove fresh checkout/ref cleanliness,
archive/install inventories, platform compatibility, imports/runtime closure,
live readiness or authority. The current Mac artifacts remain host-dependent;
the separate runtime observation's incomplete closure remains incomplete. The
later collector/consumer must observe and recheck those facts. Existing derived
installation bytecode is also future inventory work, not accepted by this linker.

Run the tiny, clearly synthetic offline fixtures with
`python -I -B ci/scripts/test_pgwire_local_evidence.py -v`. Root separately links
actual accepted evidence. No CLI/provider/connection/execution/admission API is
provided. Later installed/native all21 acceptance, the finite own-resource live
handoff and teardown, final cross-platform CI and release authority remain open.
