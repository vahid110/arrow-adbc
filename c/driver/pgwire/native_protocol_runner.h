// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>
#include "native_protocol_plan.h"

namespace adbc::qualification {
// Noninstalled POSIX library, NOT admission. Root must authenticate exact
// source/runtime/target/schema/one-use operation before calling. No connect/CLI.
struct NativeRunInput {
  std::array<PGconn*, 2> sessions;
  std::array<int, 8> outputs;
  std::string_view schema, authenticated_cleanup_schema;
  std::chrono::steady_clock::time_point discovery_deadline, cleanup_deadline;
  // Sessions are borrowed, distinct, nonblocking, quiescent/autocommit and
  // exclusively used by this call. Root must install sanitized/nonlogging libpq
  // notice handling before SQL; default notices can print raw server text. This
  // library never changes/restores an unknown caller callback or opaque argument.
  // Descriptors are borrowed, private and empty.
};
enum class NativeRunState {
  kUnattempted,
  kCaptured,
  kDefinitiveError,
  kDiagnostic,
  kSkipped,
  kUncertain,
  kInvalid
};
enum class NativeRunPhase {
  kPreflight,
  kSend,
  kPrepare,
  kDescribe,
  kFlush,
  kRead,
  kCapture,
  kParameter,
  kCache,
  kOutput,
  kCleanup
};
enum class NativeRunReason {
  kInvalidInput,
  kSession,
  kEncoding,
  kDeadline,
  kTransport,
  kSqlError,
  kUnexpectedResult,
  kCapture,
  kParameter,
  kCache,
  kOutput,
  kAllocation,
  kOwnership,
  kStopped,
  kDependency
};
struct NativeRunFailure {
  size_t step;
  NativeRunPhase phase;
  NativeRunReason reason;
  std::array<char, 6> sqlstate{};
};
struct NativeRunResult {
  std::array<NativeRunState, static_cast<size_t>(NativeStepId::kCount)> states{};
  std::optional<NativeRunFailure> first_failure;
  size_t attempted_steps = 0, operation_calls = 0, records = 0;
  uint64_t cache_peak_bytes = 0, cache_remaining_bytes = 0;
  std::array<uint64_t, 8> output_bytes{};
  bool spent = true, discovery_finished = false, output_complete = false;
  bool schema_created = false, cleanup_attempted = false, cleanup_captured = false;
  // On uncertainty retain these stable owners until ROOT closes/discards the
  // corresponding borrowed sessions. They do not mean completion/cancellation.
  std::array<std::unique_ptr<RawParameter>, 2> pending_parameters;
};
// Uses only the closed715 sequence. Output is protocol/accounting evidence, not
// native compatibility. Root still needs independent absence, process/RSS/receive
// and wall-clock containment: PGresult/cell caps do NOT bound receive memory or
// a kernel-blocked file operation. No retry/cancel/reconnect/borrowed-handle close.
NativeRunResult RunNativeDiscovery(const NativeRunInput& input) noexcept;
}  // namespace adbc::qualification
