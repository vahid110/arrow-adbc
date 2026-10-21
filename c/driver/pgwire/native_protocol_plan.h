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

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "native_protocol_capture.h"

namespace adbc::qualification {
// Closed compiled sequence, not an executor or an arbitrary query/shape API.
enum class NativeStepId : uint16_t {
#include "native_protocol_step_ids.inc"
  kCount
};
enum class NativeOperation { kCommand, kRows, kPrepareDescribe };
enum class NativeErrorPolicy { kStopRequired, kContinueDefinitive, kCleanupUnknown };
enum class NativeUncertainPolicy { kStopSpentNoRetry };
enum class NativeConnection { kUtc, kAlternateZone };
struct NativeParameterInput {
  NativeStepId source;
  size_t row, field;
  ParameterOrigin origin;
  int format;
  std::optional<NativeStepId> witness;
};
struct NativeStepPlan {
  NativeStepId id;
  std::string name, sql;
  NativeOperation operation;
  NativeConnection connection;
  NativeErrorPolicy definitive_error;
  NativeUncertainPolicy uncertain = NativeUncertainPolicy::kStopSpentNoRetry;
  // Uncertain transport/timeout/cancel/unknown ALWAYS stops, spends operation,
  // and has no retry. Cleanup is separately evidenced, never implied by a flag.
  std::vector<NativeStepId> prerequisites, affected;
  std::vector<std::string> columns;
  bool exact_column_names;
  int64_t max_rows;
  int protocol_calls, output_group, query_parameters;
  std::optional<int> requested_format, expected_parameters;
  std::optional<int64_t> affected_rows;
  std::optional<NativeParameterInput> parameter;
  std::optional<ProbeId> legacy_probe;
  // Empty means observed SHOW fact, not an inferred GUC default. A successful
  // SET alone cannot qualify a context. Fixed frontend must establish/verify
  // UTF8 through connection parameters/local state BEFORE ANY fixture SQL.
  std::string required_context_value;
};
std::optional<NativeStepId> FindNativeStep(std::string_view name) noexcept;
std::optional<NativeStepPlan> BuildNativeStep(NativeStepId id,
                                              std::string_view schema) noexcept;
// Capture re-derives all expectations from id. Returned plan edits never count.
CaptureStatus CaptureNativeResult(NativeStepId id, const PGresult* result,
                                  Observation* out, CaptureLimits limits = {}) noexcept;
// Closed positional/literal join only, not authentication of either argument.
// The future frontend must bind the actual capture call/session/source/resource.
// Every attempt expires previous parameter access, including a rejected join.
ParameterStatus PrepareNativeInput(NativeStepId parameter_step, NativeStepId source_step,
                                   const Observation& source, RawParameter* out) noexcept;
namespace detail {
// Bounded canonical nonnegative PQcmdTuples count; no server result mutation.
bool ParseAffectedRows(std::string_view text, int64_t* out) noexcept;
}  // namespace detail
}  // namespace adbc::qualification
