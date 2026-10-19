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

#include <libpq-fe.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Noninstalled qualification primitive. No connection, execution or admission.
namespace adbc::qualification {
enum class ProbeId {
  kVendorMixedValuesText,
  kVendorMixedValuesBinary,
  kVendorMixedValuesDescribe,
  kVendorMixedNullsText,
  kVendorMixedNullsBinary,
  kVendorMixedNullsDescribe,
  kVendorMixedEmptyText,
  kVendorMixedEmptyBinary,
  kVendorMixedEmptyDescribe,
  kVendorMixedParameterDescribe,
  kInterval37ValuesText,
  kInterval37ValuesBinary,
  kInterval37ValuesDescribe,
  kInterval37NullsText,
  kInterval37NullsBinary,
  kInterval37NullsDescribe,
  kInterval37EmptyText,
  kInterval37EmptyBinary,
  kInterval37EmptyDescribe,
  kInterval37ParameterDescribe,
  kFloatBitsValuesText,
  kFloatBitsValuesBinary,
  kFloatBitsValuesDescribe,
  kFloatBitsNullsText,
  kFloatBitsNullsBinary,
  kFloatBitsNullsDescribe,
  kFloatBitsEmptyText,
  kFloatBitsEmptyBinary,
  kFloatBitsEmptyDescribe,
  kFloatBitsParameterDescribe,
  kCatalogBefore,
  kCatalogAfter,
  kSvvBefore,
  kSvvAfter,
  kPlumbingError,
  kCount
};
enum class Operation { kRows, kDescribe };
enum class CaptureStatus {
  kComplete,
  kInvalidOutput,
  kInvalidProbe,
  kInvalidLimits,
  kNullResult,
  kResultStatus,
  kFieldLimit,
  kRowLimit,
  kParameterLimit,
  kParameterCount,
  kNameLimit,
  kShapeMismatch,
  kFormatMismatch,
  kCellLimit,
  kBodyLimit,
  kEncodingLimit,
  kAllocationFailure,
  kInvalidResult
};
struct CaptureLimits {
  int64_t fields = 64, parameters = 64, rows = 64, name_bytes = 127, cell_bytes = 65536;
  int64_t body_bytes = 1048576, encoded_bytes = 3145728;
};
struct ProbePlan {
  std::string name, sql;
  Operation operation;
  std::optional<int> requested_format;
  std::optional<int> expected_parameters;
  std::vector<std::string> columns;
  int64_t max_rows;
};
// Returned plan is explanatory data; capture re-derives all fixed expectations
// from ProbeId, never trusts a caller-mutated ProbePlan.
std::optional<ProbePlan> BuildProbe(ProbeId id, std::string_view schema) noexcept;
bool ValidOwnedSchemaSyntax(std::string_view schema) noexcept;
const char* CaptureStatusName(CaptureStatus status) noexcept;
struct CapturedField {
  std::string name;
  uint32_t oid = 0, table_oid = 0;
  int32_t modifier = 0, format = 0, table_column = 0;
};
struct CapturedCell {
  bool is_null = false;
  int64_t length = 0;
  std::string bytes;
};
struct Observation {
  bool present = false, complete = false;
  std::optional<int> result_status;
  std::optional<int64_t> row_count, field_count;
  std::optional<int64_t> failure_row, failure_field, failure_length;
  std::optional<std::string> sqlstate;
  std::vector<CapturedField> fields;
  std::optional<std::vector<uint32_t>> parameters;
  std::vector<std::vector<CapturedCell>> rows;
  std::string json;
};
// Clear out first. If no diagnostic fits the encoded bound, leave no observation
// or JSON; the separate nonallocating return status remains authoritative.
CaptureStatus CaptureResult(ProbeId id, const PGresult* result, Observation* out,
                            CaptureLimits limits = {}) noexcept;

enum class ParameterOrigin { kObserved, kFixtureNull, kFixtureEmpty };
enum class ParameterStatus {
  kReady,
  kInvalidObservation,
  kInvalidSelection,
  kInvalidOrigin,
  kInvalidFormat,
  kInvalidText,
  kAllocationFailure
};
struct RawParameterView {
  Oid type;
  int format, length;
  const char* value;
  ParameterOrigin origin;
};
// Single noninstalled replay input, not a codec, executor or admission proof.
// Every Prepare attempt expires previous views, even on failure. A valid SQL
// NULL is an engaged view with nullptr; failed/unprepared Get returns nullopt.
// The source may be destroyed after Prepare. This owner cannot move/copy; views
// remain stable only until the next Prepare attempt or owner destruction.
class RawParameter {
 public:
  RawParameter() = default;
  RawParameter(const RawParameter&) = delete;
  RawParameter& operator=(const RawParameter&) = delete;
  RawParameter(RawParameter&&) = delete;
  RawParameter& operator=(RawParameter&&) = delete;
  ParameterStatus Prepare(const Observation& source, size_t row, size_t field) noexcept;
  // Controlled NULL/empty trial only: never reinterpret an observed positive body.
  ParameterStatus PrepareFixture(const Observation& source, size_t row, size_t field,
                                 ParameterOrigin origin, int requested_format) noexcept;
  std::optional<RawParameterView> Get() const noexcept;

 private:
  ParameterStatus PrepareImpl(const Observation& source, size_t row, size_t field,
                              ParameterOrigin origin,
                              std::optional<int> fixture_format) noexcept;
  std::string bytes_;
  Oid type_ = 0;
  int format_ = 0, length_ = 0;
  bool ready_ = false, null_ = true;
  ParameterOrigin origin_ = ParameterOrigin::kObserved;
};
namespace detail {
bool CheckedAdd(uint64_t a, uint64_t b, uint64_t ceiling, uint64_t* out) noexcept;
bool CheckedMultiply(uint64_t a, uint64_t b, uint64_t ceiling, uint64_t* out) noexcept;
}  // namespace detail
}  // namespace adbc::qualification
