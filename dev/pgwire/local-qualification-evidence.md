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

## Same-host installed load observations (Task010g1)

`pgwire_local_install.observe_install(record_bytes, *, selected, roots)` adds a
separate trusted same-iMac snapshot comparison and one fixed load-only child.
It rechecks G0 anchors, then compares the selected C prefix plus separate smoke
client with the trusted tar, and manager/Redshift package and dist-info contents
with their wheels in place. It extracts and installs nothing. All wheel payload
metadata is compared; the only permitted pip additions are `INSTALLER` containing `pip`
and its newline, empty `REQUESTED`, the exact selected file URL/hash in
`direct_url.json`, and a fully validated installed `RECORD`. Only RECORD itself
can lack a hash/size. Cache files, stale cache rows and extra/shadow sources reject.
The collector never removes caches; root owns any separately recorded preparation.

`pgwire_local_load_probe.py` runs only under the selected interpreter with `-I -B`,
minimal environment, closed stdin and a maximum 30-second process window. It
checks installed origins before import, then records actual manager/native/
Redshift origins and resolves the selected C and wheel driver initializer
symbols without calling them. It creates no database or connection. A new owned
0600 metadata fd carries at most 1 MiB; compact status stays within 2 KiB. Parent
reaps the process group, validates exact metadata bytes and rechecks all inputs.
Partial/missing/nonzero/unknown/oversize output cannot yield success. Old CI status
protocols and admission paths stay unchanged.

Runtime facts cover only this named load-only process. Thin64le ARM64 file-backed
images must match selected before/after identities and on-disk UUID/CPU/deployment
and bounded load-command facts. OS-prefix images bind actual OS build and raw
in-memory UUID/CPU/deployment, including distinct system deployment codes; no disk
hash for a shared-cache image is invented. Raw dylib/RPATH edges stay unresolved;
future dynamic loads and broader runtime closure remain unknown. The interpreter
launch file and dyld main executable are separate facts. OS/architecture/ABI and
actual Mach-O bytes matter; a wheel `.so` suffix does not imply Linux compatibility.

Frozen `Selected`/`Roots` values bind artifact source/head/tree/validation digest,
collector source/head/tree/code, child/interpreter/client bytes, explicit prefix/
site/metadata roots, six C links, two wheel specifications and finite runtime
identities. Their source identities may differ when checking approved prior
artifacts with new verifier code. `Observed.status=same-host-load-observed` records
those facts and digests; it grants no readiness or authority. Fresh Git/ref/clean
observations remain root's responsibility outside this API. Defaults only tighten:
32 MiB compressed, 64 MiB expanded/artifact, 16 MiB/member, 256 C and 128 wheel
members, 1 MiB metadata, 1024 images, 128 file-backed nodes, 64 MiB/runtime file,
1 MiB/4096 Mach-O commands and 16,384-character paths.

Tiny synthetic inventory/parser/metadata/process tests use
`python -I -B ci/scripts/test_pgwire_local_install.py -v`. Worker/reviewer tests
never import/load actual installed packages. Actual root same-host validation and
independent review are required; no native database result follows. The separate
local execution/observation result channel, all21 native semantics and installed
acceptance, finite admitted own-resource teardown, final CI and release remain open.
