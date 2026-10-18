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

A later reviewed root adapter must bind exact source/build/libpq/session/target
provenance, private inputs/outputs, finite admission and execution counts. This
primitive grants none of those and changes no Task008 CI-only trust/claim gate.
The previous paid batch remains spent. Local-first development, full21 installed
acceptance, finite own-resource teardown, final applicable CI and separate release
authority remain required; no remote or live execution is introduced here.
