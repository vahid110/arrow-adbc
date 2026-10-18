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

#include "native_protocol_capture.h"

#include <gtest/gtest.h>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <vector>

// This separate test executable fails only C++ owned-output allocation. No
// libpq state/accessor is replaced or fabricated; PGresult constructors are real.
thread_local int allocation_fail_after = -1;
void* operator new(size_t size) {
  if (allocation_fail_after == 0) throw std::bad_alloc();
  if (allocation_fail_after > 0) --allocation_fail_after;
  if (void* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }

namespace adbc::qualification {
namespace {
constexpr const char* schema = "adbc_types_0123456789abcdef0123456789abcdef";
struct FreeResult {
  void operator()(PGresult* r) const {
    if (r) PQclear(r);
  }
};
using Result = std::unique_ptr<PGresult, FreeResult>;
Result Make(ProbeId id, int rows = 0, int format = 0,
            ExecStatusType status = PGRES_TUPLES_OK, std::vector<std::string> names = {},
            std::vector<int> formats = {}) {
  auto plan = BuildProbe(id, schema);
  if (!plan) return {};
  if (names.empty()) names = plan->columns;
  Result out(PQmakeEmptyPGresult(nullptr, status));
  if (!out) return {};
  std::vector<PGresAttDesc> attrs(names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    attrs[i].name = names[i].data();
    attrs[i].typid = i == 0 ? UINT32_MAX : 0;
    attrs[i].atttypmod = i == 0 ? -7 : -1;
    attrs[i].format = formats.empty() ? format : formats.at(i);
    attrs[i].tableid = i == 0 ? 4242 : 0;
    attrs[i].columnid = i == 0 ? 2 : 0;
  }
  if (!PQsetResultAttrs(out.get(), static_cast<int>(attrs.size()), attrs.data()))
    return {};
  for (int row = 0; row < rows; ++row)
    for (size_t col = 0; col < names.size(); ++col)
      if (!PQsetvalue(out.get(), row, static_cast<int>(col), const_cast<char*>(""), 0))
        return {};
  return out;
}
void Set(PGresult* result, int row, int col, const std::string& value) {
  ASSERT_TRUE(PQsetvalue(result, row, col, const_cast<char*>(value.data()),
                         static_cast<int>(value.size())));
}
constexpr ProbeId rows_id = ProbeId::kFloatBitsValuesBinary;
TEST(NativeProtocolCaptureTest, PlansAreClosedCompleteAndOnlySubstituteValidatedSchema) {
  std::set<std::string> names;
  for (int raw = 0; raw < static_cast<int>(ProbeId::kCount); ++raw) {
    auto plan = BuildProbe(static_cast<ProbeId>(raw), schema);
    ASSERT_TRUE(plan);
    EXPECT_TRUE(names.insert(plan->name).second);
    EXPECT_LE(plan->columns.size(), 64U);
    EXPECT_LE(plan->max_rows, 63);
    EXPECT_EQ(plan->operation == Operation::kDescribe,
              !plan->requested_format.has_value());
    if (plan->operation == Operation::kDescribe) {
      EXPECT_EQ(plan->max_rows, 0);
      EXPECT_EQ(plan->expected_parameters,
                plan->name.find("parameter_describe") != std::string::npos ? 1 : 0);
    } else
      EXPECT_FALSE(plan->expected_parameters);
    EXPECT_EQ(plan->sql.find("DROP "), std::string::npos);
    EXPECT_EQ(plan->sql.find("CREATE "), std::string::npos);
    EXPECT_EQ(plan->sql.find("INSERT "), std::string::npos);
    if (plan->name.find("parameter_describe") != std::string::npos) {
      EXPECT_NE(plan->sql.find("WHERE case_id=$1 ORDER BY case_id"), std::string::npos);
      EXPECT_EQ(plan->operation, Operation::kDescribe);
    }
    if (plan->name.starts_with("catalog.") || plan->name.starts_with("svv.")) {
      EXPECT_EQ(plan->requested_format, 0);
      EXPECT_EQ(plan->max_rows, 63);
      EXPECT_NE(plan->sql.find("'vendor_mixed','interval37','float_bits'"),
                std::string::npos);
      EXPECT_TRUE(plan->sql.ends_with("LIMIT 64"));
    }
  }
  EXPECT_EQ(BuildProbe(ProbeId::kVendorMixedValuesText, schema)->columns.size(), 22U);
  EXPECT_EQ(BuildProbe(ProbeId::kInterval37ValuesText, schema)->columns.size(), 38U);
  EXPECT_EQ(BuildProbe(ProbeId::kFloatBitsValuesText, schema)->columns.size(), 3U);
  for (auto bad : {"", "adbc_types_0123456789abcdef0123456789abcdeF",
                   "adbc_types_0123456789abcdef0123456789abcdefx", "public",
                   "adbc_types_0123456789abcdef0123456789abcde'"})
    EXPECT_FALSE(BuildProbe(rows_id, bad));
  EXPECT_FALSE(BuildProbe(static_cast<ProbeId>(-1), schema));
  auto mutated = BuildProbe(rows_id, schema);
  mutated->columns = {"caller_override"};
  auto result = Make(rows_id, 0, 1, PGRES_TUPLES_OK, mutated->columns);
  ASSERT_TRUE(result);
  Observation out;
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out), CaptureStatus::kShapeMismatch);
}
TEST(NativeProtocolCaptureTest, ExactRawMetadataBytesNullAndOwnedLifetime) {
  auto result = Make(rows_id, 2, 1);
  ASSERT_TRUE(result);
  ASSERT_TRUE(PQsetvalue(result.get(), 0, 0, nullptr, -1));
  Set(result.get(), 0, 1, "");
  Set(result.get(), 0, 2, std::string("\0\x80\xff", 3));
  Set(result.get(), 1, 0, "123");
  Observation out;
  ASSERT_EQ(CaptureResult(rows_id, result.get(), &out), CaptureStatus::kComplete);
  ASSERT_TRUE(out.present);
  ASSERT_TRUE(out.complete);
  ASSERT_FALSE(out.parameters);
  ASSERT_EQ(out.fields.size(), 3U);
  EXPECT_EQ(out.fields[0].oid, UINT32_MAX);
  EXPECT_EQ(out.fields[0].modifier, -7);
  EXPECT_EQ(out.fields[0].table_oid, 4242U);
  EXPECT_EQ(out.fields[0].table_column, 2);
  EXPECT_EQ(out.fields[1].oid, 0U);
  EXPECT_EQ(out.fields[1].modifier, -1);
  EXPECT_EQ(out.fields[1].table_oid, 0U);
  EXPECT_TRUE(out.rows[0][0].is_null);
  EXPECT_EQ(out.rows[0][0].length, 0);
  EXPECT_FALSE(out.rows[0][1].is_null);
  EXPECT_EQ(out.rows[0][1].bytes, "");
  EXPECT_EQ(out.rows[0][2].bytes, std::string("\0\x80\xff", 3));
  EXPECT_NE(out.json.find("\"bytes_hex\":\"0080ff\""), std::string::npos);
  EXPECT_NE(out.json.find("\"null\":true,\"length\":0,\"bytes_hex\":null"),
            std::string::npos);
  const auto saved = out.json;
  result.reset();
  EXPECT_EQ(out.fields[0].name, "case_id");
  EXPECT_EQ(out.rows[1][0].bytes, "123");
  EXPECT_EQ(out.json, saved);
  // Independent stdlib JSON decoding is performed from this actual emitted
  // observation in worker/root evidence, never from a hand-built expected array.
  std::cout << "F0_JSON=" << out.json << "\n";
}
TEST(NativeProtocolCaptureTest, DescribeHasNoExecutedFormatAndRequiresActualDescriptors) {
  Result result(PQmakeEmptyPGresult(nullptr, PGRES_COMMAND_OK));
  ASSERT_TRUE(result);
  PGresAttDesc mixed[3]{};
  const char* names[] = {"case_id", "real_value", "double_value"};
  for (int i = 0; i < 3; ++i) {
    mixed[i].name = const_cast<char*>(names[i]);
    mixed[i].typid = 23 + i;
    mixed[i].format = i == 1 ? 1 : 0;
    mixed[i].atttypmod = INT32_MIN;
  }
  ASSERT_TRUE(PQsetResultAttrs(result.get(), 3, mixed));
  Observation out;
  ASSERT_EQ(CaptureResult(ProbeId::kFloatBitsValuesDescribe, result.get(), &out),
            CaptureStatus::kComplete);
  ASSERT_TRUE(out.parameters);
  EXPECT_TRUE(out.parameters->empty());
  EXPECT_NE(out.json.find("\"requested_format\":null"), std::string::npos);
  EXPECT_EQ(out.fields[1].format, 1);
  EXPECT_EQ(out.fields[0].modifier, INT32_MIN);
  EXPECT_EQ(CaptureResult(ProbeId::kFloatBitsParameterDescribe, result.get(), &out),
            CaptureStatus::kParameterCount);
  EXPECT_FALSE(out.complete);
  ASSERT_TRUE(out.parameters);
  EXPECT_TRUE(out.parameters->empty());
  Result bare(PQmakeEmptyPGresult(nullptr, PGRES_COMMAND_OK));
  ASSERT_TRUE(bare);
  EXPECT_EQ(CaptureResult(ProbeId::kFloatBitsValuesDescribe, bare.get(), &out),
            CaptureStatus::kShapeMismatch);
  auto tuples = Make(rows_id, 0, 1);
  ASSERT_TRUE(tuples);
  EXPECT_EQ(CaptureResult(ProbeId::kFloatBitsValuesDescribe, tuples.get(), &out),
            CaptureStatus::kResultStatus);
}
TEST(NativeProtocolCaptureTest, ShapeFormatAndAllOtherStatusesNeverBecomeComplete) {
  Observation out;
  for (auto status :
       {PGRES_EMPTY_QUERY, PGRES_COMMAND_OK, PGRES_COPY_IN, PGRES_COPY_OUT,
        PGRES_COPY_BOTH, PGRES_BAD_RESPONSE, PGRES_NONFATAL_ERROR, PGRES_FATAL_ERROR,
        PGRES_SINGLE_TUPLE, PGRES_PIPELINE_SYNC, PGRES_PIPELINE_ABORTED}) {
    Result result(PQmakeEmptyPGresult(nullptr, status));
    ASSERT_TRUE(result);
    EXPECT_EQ(CaptureResult(rows_id, result.get(), &out), CaptureStatus::kResultStatus);
    EXPECT_FALSE(out.complete);
    EXPECT_FALSE(out.sqlstate);
    EXPECT_EQ(out.result_status, status);
  }
  auto text = Make(rows_id, 1, 0);
  ASSERT_TRUE(text);
  EXPECT_EQ(CaptureResult(rows_id, text.get(), &out), CaptureStatus::kFormatMismatch);
  EXPECT_EQ(out.fields[0].format, 0);
  EXPECT_EQ(out.rows.size(), 1U);
  auto duplicate =
      Make(rows_id, 0, 1, PGRES_TUPLES_OK, {"case_id", "case_id", "double_value"});
  ASSERT_TRUE(duplicate);
  EXPECT_EQ(CaptureResult(rows_id, duplicate.get(), &out), CaptureStatus::kShapeMismatch);
  EXPECT_EQ(out.fields[0].name, out.fields[1].name);
  auto raw = Make(rows_id, 0, 1, PGRES_TUPLES_OK,
                  {std::string("\xff", 1), "real_value", "double_value"});
  ASSERT_TRUE(raw);
  EXPECT_EQ(CaptureResult(rows_id, raw.get(), &out), CaptureStatus::kShapeMismatch);
  EXPECT_EQ(out.fields[0].name, std::string("\xff", 1));
  EXPECT_NE(out.json.find("\"name_hex\":\"ff\""), std::string::npos);
}
TEST(NativeProtocolCaptureTest, FieldsRowsNamesAndTightenedBoundsHaveExactNeighbors) {
  Observation out;
  CaptureLimits limits;
  auto result = Make(rows_id, 8, 1);
  ASSERT_TRUE(result);
  ASSERT_EQ(CaptureResult(rows_id, result.get(), &out), CaptureStatus::kComplete);
  limits.rows = 7;
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out, limits), CaptureStatus::kRowLimit);
  result = Make(rows_id, 9, 1);
  ASSERT_TRUE(result);
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out), CaptureStatus::kRowLimit);
  result = Make(rows_id, 0, 1);
  ASSERT_TRUE(result);
  limits = {};
  limits.fields = 3;
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out, limits), CaptureStatus::kComplete);
  limits.fields = 2;
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out, limits),
            CaptureStatus::kFieldLimit);
  limits = {};
  limits.name_bytes = 12;
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out, limits), CaptureStatus::kComplete);
  limits.name_bytes = 11;
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out, limits),
            CaptureStatus::kNameLimit);
  for (int count : {127, 128}) {
    result = Make(rows_id, 0, 1, PGRES_TUPLES_OK,
                  {std::string(count, 'x'), "real_value", "double_value"});
    ASSERT_TRUE(result);
    EXPECT_EQ(CaptureResult(rows_id, result.get(), &out),
              count == 127 ? CaptureStatus::kShapeMismatch : CaptureStatus::kNameLimit);
  }
  result = Make(rows_id, 0, 1, PGRES_TUPLES_OK, std::vector<std::string>(65, "x"));
  ASSERT_TRUE(result);
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out), CaptureStatus::kFieldLimit);
  for (int rows : {63, 64}) {
    result = Make(ProbeId::kCatalogBefore, rows, 0);
    ASSERT_TRUE(result);
    EXPECT_EQ(CaptureResult(ProbeId::kCatalogBefore, result.get(), &out),
              rows == 63 ? CaptureStatus::kComplete : CaptureStatus::kRowLimit);
  }
}
TEST(NativeProtocolCaptureTest, CellBodyAndEncodedLimitsNeverClipOrKeepStaleSuccess) {
  Observation out;
  CaptureLimits limits;
  for (int size : {65536, 65537}) {
    auto result = Make(rows_id, 1, 1);
    ASSERT_TRUE(result);
    Set(result.get(), 0, 0, std::string(size, 'x'));
    EXPECT_EQ(CaptureResult(rows_id, result.get(), &out),
              size == 65536 ? CaptureStatus::kComplete : CaptureStatus::kCellLimit);
    if (size == 65537) {
      EXPECT_TRUE(out.rows.empty());
      EXPECT_EQ(out.failure_row, 0);
      EXPECT_EQ(out.failure_field, 0);
      EXPECT_EQ(out.failure_length, 65537);
    }
  }
  auto result = Make(rows_id, 8, 1);
  ASSERT_TRUE(result);
  for (int i = 0; i < 16; ++i) Set(result.get(), i / 3, i % 3, std::string(65536, 'x'));
  ASSERT_EQ(CaptureResult(rows_id, result.get(), &out), CaptureStatus::kComplete);
  EXPECT_LE(out.json.size(), 3145728U);
  Set(result.get(), 5, 1, "x");
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out), CaptureStatus::kBodyLimit);
  result = Make(rows_id, 1, 1);
  ASSERT_TRUE(result);
  ASSERT_EQ(CaptureResult(rows_id, result.get(), &out), CaptureStatus::kComplete);
  const auto exact = static_cast<int64_t>(out.json.size());
  limits.encoded_bytes = exact;
  ASSERT_EQ(CaptureResult(rows_id, result.get(), &out, limits), CaptureStatus::kComplete);
  limits.encoded_bytes = exact - 1;
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out, limits),
            CaptureStatus::kEncodingLimit);
  EXPECT_FALSE(out.present);
  EXPECT_TRUE(out.json.empty());
  for (int cap : {0, 1, 8}) {
    limits.encoded_bytes = cap;
    EXPECT_EQ(CaptureResult(rows_id, nullptr, &out, limits),
              CaptureStatus::kEncodingLimit);
    EXPECT_FALSE(out.present);
    EXPECT_TRUE(out.json.empty());
    EXPECT_TRUE(out.fields.empty());
    EXPECT_TRUE(out.rows.empty());
  }
  limits = {};
  limits.cell_bytes = 0;
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out, limits), CaptureStatus::kComplete);
  Set(result.get(), 0, 0, "x");
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out, limits),
            CaptureStatus::kCellLimit);
  limits = {};
  limits.fields = 65;
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out, limits),
            CaptureStatus::kInvalidLimits);
  EXPECT_FALSE(out.present);
  limits = {};
  limits.encoded_bytes = 3145729;
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out, limits),
            CaptureStatus::kInvalidLimits);
  limits = {};
  limits.rows = -1;
  EXPECT_EQ(CaptureResult(rows_id, result.get(), &out, limits),
            CaptureStatus::kInvalidLimits);
  EXPECT_EQ(CaptureResult(static_cast<ProbeId>(-1), result.get(), &out),
            CaptureStatus::kInvalidProbe);
  EXPECT_FALSE(out.present);
  EXPECT_EQ(CaptureResult(rows_id, result.get(), nullptr), CaptureStatus::kInvalidOutput);
  EXPECT_EQ(CaptureResult(rows_id, nullptr, &out), CaptureStatus::kNullResult);
  EXPECT_TRUE(out.present);
  EXPECT_FALSE(out.complete);
  EXPECT_FALSE(out.result_status);
  EXPECT_FALSE(out.row_count);
  EXPECT_FALSE(out.field_count);
}
TEST(NativeProtocolCaptureTest, CheckedSizeArithmeticDoesNotNeedHugeObjects) {
  uint64_t out = 9;
  EXPECT_FALSE(detail::CheckedAdd(UINT64_MAX, 1, UINT64_MAX, &out));
  EXPECT_EQ(out, 9U);
  EXPECT_FALSE(detail::CheckedMultiply(UINT64_MAX, 2, UINT64_MAX, &out));
  EXPECT_EQ(out, 9U);
  EXPECT_TRUE(detail::CheckedAdd(UINT64_MAX - 1, 1, UINT64_MAX, &out));
  EXPECT_EQ(out, UINT64_MAX);
  EXPECT_TRUE(detail::CheckedMultiply(0, UINT64_MAX, 0, &out));
  EXPECT_EQ(out, 0U);
  EXPECT_FALSE(detail::CheckedMultiply(2, 3, 5, &out));
  EXPECT_EQ(out, 0U);
  EXPECT_TRUE(detail::CheckedMultiply(2, 3, 6, &out));
  EXPECT_EQ(out, 6U);
}
TEST(NativeProtocolCaptureTest, FormatMismatchKeepsAllBoundedBodiesOnlyAsDiagnostics) {
  for (auto id : {ProbeId::kFloatBitsValuesText, ProbeId::kFloatBitsValuesBinary}) {
    auto result = Make(id, 1, 0, PGRES_TUPLES_OK, {}, {0, 1, 0});
    ASSERT_TRUE(result);
    ASSERT_TRUE(PQsetvalue(result.get(), 0, 0, nullptr, -1));
    Set(result.get(), 0, 1, "");
    Set(result.get(), 0, 2, std::string("\0\x80\xff", 3));
    Observation out;
    ASSERT_EQ(CaptureResult(id, result.get(), &out), CaptureStatus::kFormatMismatch);
    ASSERT_TRUE(out.present);
    EXPECT_FALSE(out.complete);
    ASSERT_EQ(out.rows.size(), 1U);
    EXPECT_TRUE(out.rows[0][0].is_null);
    EXPECT_FALSE(out.rows[0][1].is_null);
    EXPECT_EQ(out.rows[0][1].bytes, "");
    EXPECT_EQ(out.rows[0][2].bytes, std::string("\0\x80\xff", 3));
    EXPECT_EQ(out.fields[1].format, 1);
    std::cout << "F0_MISMATCH_JSON=" << out.json << "\n";
    const auto size = out.json.size();
    CaptureLimits limits;
    limits.cell_bytes = 2;
    EXPECT_EQ(CaptureResult(id, result.get(), &out, limits), CaptureStatus::kCellLimit);
    EXPECT_TRUE(out.rows.empty());
    EXPECT_FALSE(out.complete);
    limits = {};
    limits.body_bytes = 2;
    EXPECT_EQ(CaptureResult(id, result.get(), &out, limits), CaptureStatus::kBodyLimit);
    EXPECT_TRUE(out.rows.empty());
    limits = {};
    limits.encoded_bytes = static_cast<int64_t>(size) - 1;
    EXPECT_EQ(CaptureResult(id, result.get(), &out, limits),
              CaptureStatus::kEncodingLimit);
    EXPECT_FALSE(out.present);
    EXPECT_TRUE(out.rows.empty());
    EXPECT_TRUE(out.json.empty());
  }
}
TEST(NativeProtocolCaptureTest, OwnedOutputAllocationFailureClearsStaleSuccess) {
  auto result = Make(rows_id, 1, 1);
  ASSERT_TRUE(result);
  Set(result.get(), 0, 0, std::string(64, 'x'));
  Observation successful;
  ASSERT_EQ(CaptureResult(rows_id, result.get(), &successful), CaptureStatus::kComplete);
  int failures = 0;
  for (int fail = 0; fail < 16; ++fail) {
    Observation out = successful;
    allocation_fail_after = fail;
    const auto status = CaptureResult(rows_id, result.get(), &out);
    allocation_fail_after = -1;
    if (status == CaptureStatus::kAllocationFailure) {
      ++failures;
      EXPECT_FALSE(out.present);
      EXPECT_TRUE(out.json.empty());
      EXPECT_TRUE(out.rows.empty());
      EXPECT_TRUE(out.fields.empty());
    } else {
      EXPECT_EQ(status, CaptureStatus::kComplete);
      EXPECT_TRUE(out.complete);
    }
  }
  EXPECT_GT(failures, 0);
}
}  // namespace
}  // namespace adbc::qualification
