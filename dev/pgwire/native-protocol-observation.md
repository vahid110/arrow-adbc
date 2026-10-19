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

# Bounded protocol observations (Task010f0)

`c/driver/pgwire/native_protocol_capture{.h,.cc}` is a noninstalled qualification
primitive. It accepts a borrowed `const PGresult*`, a closed `ProbeId` and optional
tighter limits. It copies owned facts/bytes and produces canonical version 1 JSON.
It has no connection, session, query execution, CLI, admission or driver codec.
Neither driver source list, installed headers nor wheels include this source.

`BuildProbe` returns fixed SQL and expected ordered columns for a validated schema
spelling `adbc_types_` plus 32 lowercase hex characters. Its returned plan is data;
capture independently re-derives expectations from the ID. Caller edits cannot
change capture shape/operation or authorize SQL. Schema syntax proves no ownership.
The closed groups are vendor_mixed (case_id plus 21 canonical type fields),
interval37 (case_id plus 37 interval declarations), and float_bits (case_id/REAL/
DOUBLE). Values select IDs1,2; NULL selects ID3; empty uses WHERE1=0. Ordering uses
only case_id. Text, binary and describe have distinct IDs. Parameter-describe is a
fixed case_id=$1 read query with exactly one observed parameter; ordinary describe
requires zero. Fixed text catalog-before/after queries cover only the three named
relations and at most 63 columns; LIMIT64 exposes overflow. PG10-column and SVV8-
column projections preserve raw diagnostics; unsupported queries fail without a
substitute or inferred interval producer mapping. A separate fixed division-error
ID exists only for ordinary-PG test plumbing.

Rows require TUPLES_OK; describe requires COMMAND_OK plus the required descriptors
and actual parameter count. Prepared descriptors do not prove binary execution.
Capture retains raw OIDs (including0), modifiers (including negatives), origin
codes, ordered names and actual field formats separately from requested format.
Names and payload bytes are hex; NULL has its own flag, distinct from empty bytes.
Only validated five-character SQLSTATE is read from an error; missing stays absent.
Raw error messages/detail/context/SQL and connection/auth/environment values are
never read by the observer.

Every successful output owns its bytes after PQclear. Default caps are64 fields,
64 parameters,64 rows overall (8 for value probes,63 for catalog rows),127 name
bytes,65,536 bytes per cell,1MiB aggregate body and3MiB complete JSON. Bounds can
only tighten. Two-pass checks cover counts, lengths, arithmetic and hex/JSON
expansion. Oversize, status and shape failures cannot become complete or retain
partial bodies. If only actual format mismatches the request, all otherwise-valid
bounded bodies remain diagnostic with actual codes, format_mismatch and
complete=false. Any body/allocation/encoding failure discards all bodies. If the
encoded cap cannot hold a complete record, no observation/JSON is returned; the
fixed return status survives, and stale prior output is cleared. A digest or
clipped prefix never substitutes for needed bytes.

A PGresult already contains received data: these caps bound extraction/output,
not query duration or receive memory. The later admitted harness must bound those.
These are small discovery budgets, not SQL type limits. In particular HLL's
24,580-byte raw object fact is not a serialized UTF8/Base64 cap; no HLL parser or
size policy is selected here. Native SUPER/HLL/spatial/interval/TIMETZ meanings,
large-value boundaries and whole-projection float fidelity remain unqualified.
Raw libpq observations do not prove installed ADBC read/bind/ingest/metadata.

The default `adbc-native-protocol-capture-test` is socket-free and uses genuine
PGresult constructors. Its independent byte/JSON, ownership, mismatch, limit and
allocation tests do not fabricate nonzero parameter/error descriptors. Separate
UNIX-only `adbc-native-protocol-capture-plumbing-test` is excluded from default
build, CTest and install. Only the coordinator may explicitly run it with validated
own Unix-socket/port/database/user/schema inputs, no URI/TCP/password or PG*
environment fallback. It uses fixed ordinary float_bits fixture SQL to observe
real parameters, origin/modifier fields and error SQLSTATE, then acknowledges
cleanup. Root separately verifies schema absence. Worker/reviewer do not connect.

The optional trailing `--observation-fd N` selects a separate borrowed output
descriptor; the old stdout mode is retained. Before connecting, the test requires
a canonical integer greater than 2 and a writable, non-append regular file owned
by the current UID, mode 0600, one link, empty size and zero offset. The root
creates it exclusively under its own 0700 directory. The child never opens a
result path, truncates data or closes this caller-owned descriptor.

Fd mode writes the seven actual `Observation.json` records in fixed probe order,
each followed by LF. Limits include LF: 3 MiB per record and 4 MiB total. Whole
record bounds precede writes; short writes and EINTR are handled, and any partial
failure stays failed. File identity, mode, length and offset are rechecked.
After successful fixture cleanup, fsync and final checks, stdout contains only
one compact version-1 `adbc-local-observation-complete` status with ordinary-PG
identity, seven records, actual bytes, libpq/server versions and cleanup
acknowledgement. The intentional error record remains `complete=false` with
SQLSTATE 22012; channel completion does not qualify that query or any type.
Failed/partial output never becomes completed evidence. The 30-second process
alarm can bypass destructors, so timeout leaves cleanup unknown until the root's
independent absence check. Observed-change checks do not promise detection of
every concurrent writer. This adds no parent executor or native access path.

`RawParameter` is a noninstalled single-input owner for a later fixed replay
frontend. It requires a present, complete TUPLES_OK row observation with consistent
bounded dimensions, nonzero observed OIDs, actual formats 0/1 and exact cell/null
storage. It rechecks the whole observation before copying the selected cell;
format-mismatch diagnostics cannot supply a baseline. `Prepare` preserves the
observed OID, format and body. Text rejects embedded NUL and owns a final NUL
outside the payload length; binary retains every byte. SQL NULL has a null pointer,
while empty non-null input has a non-null pointer in either format.

Separate `PrepareFixture` accepts only controlled NULL/empty trials using an
observed field identity and an explicit requested format 0/1. It never reinterprets
an observed positive body. The exposed origin distinguishes these trials from
observed replay; neither establishes receiver acceptance or native encoding.
The owner cannot copy/move. Views survive source mutation/destruction, but expire
on every preparation attempt or owner destruction. Failed/unprepared access is
`nullopt`, distinct from a valid SQL NULL view. Allocation failure leaves no
usable parameter. This adds no SQL, execution path, production writer or codec.

`native_protocol_plan.h` adds a closed compiled 715-step native discovery
specification: 103 commands, 494 row queries and 118 PREPARE/DESCRIBE pairs,
833 counted libpq operations. Fixed IDs select interned SQL, ordered columns,
formats, limits, source selections and dependency/affected sets. Only the
validated schema spelling is substituted. No runtime SQL/JSON loader or caller
shape is accepted. Existing 35 capture IDs and the seven-PG output remain intact. The old capture
API retains its caps; native sequence wrappers only tighten them to the selected
per-step row bounds. Commands require COMMAND_OK with no row/field/parameter payload, plus exact
bounded affected-row counts where required. Raw affected tag text is never logged.

Forty required setup/session steps stop on failure; 674 exploratory/read/replay
steps allow unrelated work only after a definitive completed SQL error, skipping
actual dependents. Uncertain outcomes always stop and spend the operation with
no retry. Cleanup failure stays unknown. These are explicit plan facts, not a
scheduler. SHOW alone has a fixed one-field/one-row shape retaining its bounded
actual field name because native label capitalization remains unqualified. Other
projected names remain exact. Actual UTF8/timezone values must match; a bounded
mismatch retains the actual row only as incomplete diagnostics. Size, shape or
allocation failures retain no bodies. Other SHOW facts are observed without guessed defaults. The future frontend must establish
and verify UTF8 before ANY fixture SQL; later SHOW cannot prove earlier state.

`PrepareNativeInput` joins only a fixed parameter/source ID and completed bounded
observation to G4 ownership. It checks ordered shape, actual format, fixed
BIGINT case literals and selected NULL semantics; vendor OIDs/encodings are not
guessed. Controlled BIGINT binary case literals are reference expectations,
not a native wire qualification. SUPER observed SQL NULL stays distinct from
fixture NULL/empty. Failed joins expire previous access without changing source
observations. This guard cannot authenticate a claimed source ID or observation;
the later frontend must bind the actual capture/session/source/resource identity.
No native connection, execution bridge, admission, semantic codec or production
Bind/BindStream implementation is supplied by these plans.

A later reviewed root adapter must bind exact source/build/libpq/session/target
provenance, private inputs/outputs, finite admission and execution counts. This
primitive grants none of those and changes no Task008 CI-only trust/claim gate.
The previous paid batch remains spent. Local-first development, full21 installed
acceptance, finite own-resource teardown, final applicable CI and separate release
authority remain required; no remote or live execution is introduced here.

## Closed runner (Task010h1)

`RunNativeDiscovery` is a noninstalled POSIX library entry with two borrowed,
already-connected, distinct nonblocking/autocommit sessions and eight borrowed
private output files. It executes only H0's 715 compiled steps, at most 833 send
operations. A prepare/describe pair shares one three-second deadline; only the
five selected HLL setup steps have ten seconds. Discovery is at most 720 seconds
with a separate 180-second cleanup reserve. No connect/authentication, CLI,
plan loader, alternate SQL/format, retry, cancellation or reconnect is provided.
The caller must authenticate the source/runtime/resource/one-use operation and
exact cleanup schema before calling. Borrowed handles or schema syntax are not
admission. The socket-free default test is `adbc-native-protocol-runner-test`.

Actual UTF8 is checked before SQL and interpretation. Selected SET plus exact
SHOW facts qualify each session only for subsequent work; earlier catalog/alias
records explicitly remain context-unqualified. Required setup/context failures
stop discovery. Definitive exploratory SQL errors skip actual descendants;
transport, timeout, missing/invalid SQLSTATE and unfamiliar outcomes stop and
spend the attempt. Cached fixed parameter baselines have a conservative 16 MiB
owned-storage cap (excluding allocator overhead and libpq receive storage) and
are released after last use. Positive replay uses actual format and stable G4
owned bytes; controlled NULL/empty trials retain distinct origins.

On uncertain in-flight parameter execution, the returned result retains its
owner. Root must close/discard the corresponding sessions **before destroying
that result**; return does not mean completed execution or cancellation. The
runner never closes borrowed sessions or descriptors. Its sole DROP is a finally
action only after positively completed, captured creation in this invocation and
matching root-authenticated cleanup schema. Duplicate or uncertain CREATE grants
no SQL cleanup ownership. Primary failure survives cleanup failure; independent
schema/resource absence remains a root duty.

Each JSONL record including LF/metadata is at most 3 MiB, each fixed group 4 MiB,
and all eight groups 32 MiB. Short writes/EINTR, sticky failures, fd identity,
mode/owner/offset/size and final fsync are checked. Step/error/skip records count
toward every cap; a final accounting record plus successful return/reap is needed
for transport completion. None is type compatibility. PGresult extraction caps
do not bound receive memory, allocator overhead or a blocked kernel operation:
root process/RSS/receive/deadline containment and exact admission are still
required before any paid execution. H1 introduces no native/all-type support,
installed consumer proof or renewed Task008 spending authority.
