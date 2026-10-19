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

#include "native_protocol_plan.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <climits>
#include <memory>
#include <set>
#include <string>
#include <vector>

extern thread_local int allocation_fail_after;
namespace adbc::qualification {
namespace {
constexpr char schema[] = "adbc_types_0123456789abcdef0123456789abcdef";
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
NativeStepId Id(std::string_view name) { return FindNativeStep(name).value(); }
NativeStepPlan Plan(std::string_view name) {
  return BuildNativeStep(Id(name), schema).value();
}
Result Make(std::vector<std::string> names, int count = 1, int format = 0,
            ExecStatusType status = PGRES_TUPLES_OK, Oid oid = 400000001U) {
  Result r(PQmakeEmptyPGresult(nullptr, status), PQclear);
  if (!r) return r;
  std::vector<PGresAttDesc> fields(names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    fields[i].name = names[i].data();
    fields[i].typid = oid;
    fields[i].typlen = -1;
    fields[i].atttypmod = -7;
    fields[i].format = format;
    fields[i].tableid = 123;
    fields[i].columnid = static_cast<int>(i + 1);
  }
  if (!names.empty() &&
      !PQsetResultAttrs(r.get(), static_cast<int>(fields.size()), fields.data()))
    return {nullptr, PQclear};
  for (int row = 0; row < count; ++row)
    for (size_t col = 0; col < fields.size(); ++col)
      if (!PQsetvalue(r.get(), row, static_cast<int>(col), const_cast<char*>(""), 0))
        return {nullptr, PQclear};
  return r;
}
void Value(PGresult* r, int row, int col, std::string_view value) {
  ASSERT_TRUE(PQsetvalue(r, row, col, const_cast<char*>(value.data()),
                         static_cast<int>(value.size())));
}
std::vector<std::string> SuperColumns(bool case_id) {
  std::vector<std::string> result;
  if (case_id) result.emplace_back("case_id");
  for (const char* name :
       {"value", "sql_null", "kind", "dec_kind", "float_kind", "p", "s"})
    result.emplace_back(name);
  for (const char* prefix : {"nested", "a0", "a1"})
    for (const char* suffix : {"_sql_null", "_kind", "_dec", "_float", "_p", "_s"})
      result.push_back(std::string(prefix) + suffix);
  result.emplace_back("missing_sql_null");
  result.emplace_back("missing_kind");
  return result;
}
Observation Baseline(std::string_view name) {
  const auto plan = Plan(name);
  const bool super = name.starts_with("super_cases.");
  const int rows = super ? 8 : 2;
  const int start = name.starts_with("super_cases.9.") ? 9 : 1;
  auto result = Make(plan.columns, rows, *plan.requested_format);
  for (int i = 0; i < rows; ++i) {
    std::string value;
    if (*plan.requested_format == 0)
      value = std::to_string(start + i);
    else {
      value.resize(8);
      value[7] = static_cast<char>(start + i);
    }
    Value(result.get(), i, 0, value);
    if (super && start + i == 1) EXPECT_TRUE(PQsetvalue(result.get(), i, 1, nullptr, 0));
  }
  Observation out;
  EXPECT_EQ(CaptureNativeResult(plan.id, result.get(), &out), CaptureStatus::kComplete);
  return out;
}

TEST(NativePlanTest, ExactClosedInventoryCountsPoliciesAndLegacyPlans) {
  std::array<int, 3> operations{}, policies{};
  int calls = 0, legacy = 0, parameters = 0;
  std::set<std::string> names;
  for (size_t i = 0; i < static_cast<size_t>(NativeStepId::kCount); ++i) {
    const auto plan = BuildNativeStep(static_cast<NativeStepId>(i), schema);
    ASSERT_TRUE(plan);
    EXPECT_EQ(FindNativeStep(plan->name), plan->id);
    EXPECT_TRUE(names.insert(plan->name).second);
    ++operations[static_cast<int>(plan->operation)];
    ++policies[static_cast<int>(plan->definitive_error)];
    EXPECT_EQ(plan->uncertain, NativeUncertainPolicy::kStopSpentNoRetry);
    calls += plan->protocol_calls;
    EXPECT_LE(plan->columns.size(), 64);
    EXPECT_LE(plan->max_rows, 63);
    EXPECT_GE(plan->output_group, 1);
    EXPECT_LE(plan->output_group, 8);
    for (auto id : plan->prerequisites) EXPECT_LT(static_cast<size_t>(id), i);
    if (plan->parameter) ++parameters;
    if (plan->legacy_probe) {
      ++legacy;
      const auto old = BuildProbe(*plan->legacy_probe, schema);
      ASSERT_TRUE(old);
      EXPECT_EQ(plan->name, old->name);
      EXPECT_EQ(plan->columns, old->columns);
      EXPECT_LE(plan->max_rows, old->max_rows);
      EXPECT_EQ(plan->requested_format, old->requested_format);
      EXPECT_EQ(plan->expected_parameters, old->expected_parameters);
    }
  }
  EXPECT_EQ(names.size(), 715);
  EXPECT_EQ(operations, (std::array<int, 3>{103, 494, 118}));
  EXPECT_EQ(policies, (std::array<int, 3>{40, 674, 1}));
  EXPECT_EQ(calls, 833);
  EXPECT_EQ(legacy, 35);
  EXPECT_EQ(parameters, 156);
}
TEST(NativePlanTest, LiteralSqlAliasesSpatialSuperAndSchemaCannotBeOverridden) {
  EXPECT_EQ(Plan("setup.schema").sql, std::string("CREATE SCHEMA \"") + schema + "\"");
  EXPECT_NE(Plan("setup.aliases.positive").sql.find("\xc3\xa9\xf0\x9f\x99\x82 "),
            std::string::npos);
  const auto aliases = Plan("setup.aliases").sql;
  for (const char* text : {"a05 NUMERIC(38,4)", "a11 BPCHAR", "a14 TEXT",
                           "a20 VARBINARY(16777216)", "a22 NATIONAL CHARACTER(4)"})
    EXPECT_NE(aliases.find(text), std::string::npos);
  const auto bare = Plan("setup.bare_aliases").sql;
  for (const char* text :
       {"a05 NUMERIC,", "a09 CHARACTER,", "a11 BPCHAR,", "a14 TEXT,", "a20 VARBINARY,"})
    EXPECT_NE(bare.find(text), std::string::npos);
  EXPECT_EQ(Plan("candidate.geometry.18").sql,
            std::string("UPDATE \"") + schema +
                "\".\"spatial_cases\" SET g=ST_GeomFromText('MULTIPOINT M((0 0 4),(1 1 "
                "4))',0) WHERE case_id=18");
  EXPECT_EQ(Plan("spatial.geography.36.text").sql,
            std::string("SELECT case_id,h AS value FROM \"") + schema +
                "\".\"spatial_cases\" WHERE case_id=36");
  const auto super = Plan("parameter.super.case09.format1");
  EXPECT_EQ(super.columns, SuperColumns(false));
  EXPECT_EQ(Plan("super_cases.9.semantics").columns, SuperColumns(true));
  EXPECT_NE(super.sql.find("FROM (SELECT $1::SUPER AS v) p"), std::string::npos);
  EXPECT_NE(super.sql.find("CASE WHEN IS_DECIMAL(v[1].n) THEN DECIMAL_SCALE(v[1].n) ELSE "
                           "NULL END AS a1_s"),
            std::string::npos);
  for (const char* invalid : {"", "public", "adbc_types_0123456789abcdef0123456789abcdeF",
                              "adbc_types_0123456789abcdef0123456789abcdef;DROP"})
    EXPECT_FALSE(BuildNativeStep(Id("setup.schema"), invalid));
  EXPECT_FALSE(BuildNativeStep(static_cast<NativeStepId>(65535), schema));
  EXPECT_FALSE(FindNativeStep("arbitrary.query"));
  EXPECT_FALSE(FindNativeStep(std::string(128, 'x')));
  auto edited = super;
  edited.columns = {"wrong"};
  edited.sql = "SELECT arbitrary";
  auto r = Make({"wrong"});
  Observation out;
  EXPECT_EQ(CaptureNativeResult(super.id, r.get(), &out), CaptureStatus::kShapeMismatch);
  EXPECT_FALSE(out.complete);
}
TEST(NativePlanTest, CommandShapeAndMissingAffectedCountFailClosed) {
  auto command = Make({}, 0, 0, PGRES_COMMAND_OK);
  Observation out;
  EXPECT_EQ(CaptureNativeResult(Id("setup.schema"), command.get(), &out),
            CaptureStatus::kComplete);
  EXPECT_TRUE(out.fields.empty());
  EXPECT_TRUE(out.rows.empty());
  EXPECT_FALSE(out.parameters);
  EXPECT_NE(out.json.find("\"operation\":\"command\""), std::string::npos);
  EXPECT_EQ(CaptureNativeResult(Id("candidate.geometry.01"), command.get(), &out),
            CaptureStatus::kAffectedRows);
  EXPECT_FALSE(out.complete);
  EXPECT_FALSE(out.affected_rows);
  EXPECT_EQ(Plan("candidate.geometry.01").affected_rows, 1);
  EXPECT_EQ(Plan("setup.spatial_cases.baseline").affected_rows, 36);
  EXPECT_EQ(Plan("setup.hll_input.values").affected_rows, 32768);
  for (auto status :
       {PGRES_TUPLES_OK, PGRES_EMPTY_QUERY, PGRES_FATAL_ERROR, PGRES_COPY_IN}) {
    auto wrong = Make({}, 0, 0, status);
    EXPECT_EQ(CaptureNativeResult(Id("setup.schema"), wrong.get(), &out),
              CaptureStatus::kResultStatus);
    EXPECT_FALSE(out.complete);
  }
  auto fields = Make({"extra"}, 0, 0, PGRES_COMMAND_OK);
  EXPECT_EQ(CaptureNativeResult(Id("setup.schema"), fields.get(), &out),
            CaptureStatus::kShapeMismatch);
}
TEST(NativePlanTest, HllAffectedCountMatchesLiteralFilteredCartesianSql) {
  const auto plan = Plan("setup.hll_input.values");
  const std::string digit_sql =
      "(SELECT 0 AS d UNION ALL SELECT 1 AS d UNION ALL SELECT 2 AS d UNION ALL SELECT 3 "
      "AS d UNION ALL SELECT 4 AS d UNION ALL SELECT 5 AS d UNION ALL SELECT 6 AS d "
      "UNION ALL SELECT 7 AS d UNION ALL SELECT 8 AS d UNION ALL SELECT 9 AS d)";
  const auto expected_sql =
      "INSERT INTO \"" + std::string(schema) +
      "\".\"hll_input\" SELECT a.d+10*b.d+100*c.d+1000*d.d+10000*e.d FROM " + digit_sql +
      " a CROSS JOIN " + digit_sql + " b CROSS JOIN " + digit_sql + " c CROSS JOIN " +
      digit_sql + " d CROSS JOIN " + digit_sql +
      " e WHERE a.d+10*b.d+100*c.d+1000*d.d+10000*e.d<32768";
  EXPECT_EQ(plan.sql, expected_sql);
  constexpr std::array<int, 10> digits{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  std::array<bool, 32768> seen{};
  int candidates = 0, retained = 0;
  for (int a : digits)
    for (int b : digits)
      for (int c : digits)
        for (int d : digits)
          for (int e : digits) {
            ++candidates;
            const int n = a + 10 * b + 100 * c + 1000 * d + 10000 * e;
            if (n < 32768) {
              ASSERT_GE(n, 0);
              ASSERT_FALSE(seen[n]);
              seen[n] = true;
              ++retained;
            }
          }
  EXPECT_EQ(candidates, 100000);
  EXPECT_EQ(retained, 32768);
  EXPECT_TRUE(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));
  EXPECT_EQ(plan.affected_rows, retained);
  EXPECT_EQ(plan.operation, NativeOperation::kCommand);
  EXPECT_EQ(plan.protocol_calls, 1);
  EXPECT_EQ(plan.definitive_error, NativeErrorPolicy::kStopRequired);
  // Pure parser/compiled-plan evidence, not a fabricated server command tag.
  // Genuine COMMAND_OK shape/missing-count negatives remain above. Root alone
  // obtains the actual INSERT command result from its own PostgreSQL instance.
  int64_t count = -1;
  ASSERT_TRUE(detail::ParseAffectedRows("32768", &count));
  EXPECT_EQ(plan.affected_rows, count);
  ASSERT_TRUE(detail::ParseAffectedRows("100000", &count));
  EXPECT_NE(plan.affected_rows, count);
}
TEST(NativePlanTest, AffectedCountParserHasIndependentLiteralBoundaries) {
  int64_t out = -1;
  for (const auto& pair :
       std::vector<std::pair<std::string, int64_t>>{{"0", 0},
                                                    {"1", 1},
                                                    {"36", 36},
                                                    {"100000", 100000},
                                                    {"9223372036854775807", INT64_MAX}}) {
    ASSERT_TRUE(detail::ParseAffectedRows(pair.first, &out));
    EXPECT_EQ(out, pair.second);
  }
  for (const auto& text : std::vector<std::string>{
           "", "-1", "+1", "01", "00", " 1", "1 ", "1\n", "1e0", "9223372036854775808",
           std::string("1\0x", 3), std::string(65536, '9')}) {
    out = 17;
    EXPECT_FALSE(detail::ParseAffectedRows(text, &out));
    EXPECT_EQ(out, 17);
  }
  EXPECT_FALSE(detail::ParseAffectedRows("1", nullptr));
}
TEST(NativePlanTest, LiteralNewRowDescribeBodiesAndRawMetadata) {
  for (auto name : {"spatial.geometry.01.text", "spatial.geometry.01.binary",
                    "spatial.geometry.01.describe"}) {
    auto p = Plan(name);
    auto r = Make({"case_id", "value"},
                  p.operation == NativeOperation::kPrepareDescribe ? 0 : 1,
                  p.requested_format.value_or(0),
                  p.operation == NativeOperation::kPrepareDescribe ? PGRES_COMMAND_OK
                                                                   : PGRES_TUPLES_OK);
    if (p.operation != NativeOperation::kPrepareDescribe) {
      Value(r.get(), 0, 0, "1");
      Value(r.get(), 0, 1, std::string("\0\x80\xff", 3));
    }
    Observation out;
    ASSERT_EQ(CaptureNativeResult(p.id, r.get(), &out), CaptureStatus::kComplete);
    EXPECT_EQ(out.fields[1].oid, 400000001U);
    EXPECT_EQ(out.fields[1].modifier, -7);
    EXPECT_EQ(out.fields[1].table_oid, 123);
    if (p.operation == NativeOperation::kPrepareDescribe) {
      ASSERT_TRUE(out.parameters);
      EXPECT_TRUE(out.parameters->empty());
      EXPECT_TRUE(out.rows.empty());
    } else
      EXPECT_EQ(out.rows[0][1].bytes, std::string("\0\x80\xff", 3));
  }
  auto r = Make({"case_id", "value"}, 1, 0, PGRES_TUPLES_OK, 0);
  Observation out;
  EXPECT_EQ(CaptureNativeResult(Id("spatial.geometry.01.text"), r.get(), &out),
            CaptureStatus::kInvalidResult);
}
TEST(NativePlanTest, IndependentSuperSpatialAliasShapesAndNullEmpty) {
  for (auto name : {"parameter.super.case09.format0", "parameter.super.case09.format1",
                    "super_cases.9.semantics"}) {
    auto p = Plan(name);
    auto columns = SuperColumns(std::string_view(name).starts_with("super_cases"));
    auto r = Make(columns);
    ASSERT_TRUE(PQsetvalue(r.get(), 0, 0, nullptr, 0));
    Observation out;
    ASSERT_EQ(CaptureNativeResult(p.id, r.get(), &out), CaptureStatus::kComplete);
    EXPECT_TRUE(out.rows[0][0].is_null);
    EXPECT_FALSE(out.rows[0][1].is_null);
    EXPECT_TRUE(out.rows[0][1].bytes.empty());
    columns.back() = "wrong";
    auto wrong = Make(columns);
    EXPECT_EQ(CaptureNativeResult(p.id, wrong.get(), &out),
              CaptureStatus::kShapeMismatch);
    EXPECT_TRUE(out.rows.empty());
  }
  const std::vector<std::string> spatial{"case_id", "srid", "dims", "is_empty", "ewkb"};
  EXPECT_EQ(Plan("spatial.geography.36.semantics").columns, spatial);
  auto result = Make(spatial);
  Observation out;
  EXPECT_EQ(CaptureNativeResult(Id("spatial.geography.36.semantics"), result.get(), &out),
            CaptureStatus::kComplete);
  std::vector<std::string> aliases{"case_id"};
  for (int i = 1; i <= 23; ++i)
    aliases.push_back("a" + std::string(i < 10 ? "0" : "") + std::to_string(i));
  EXPECT_EQ(Plan("bare_aliases.text").columns, aliases);
  EXPECT_EQ(Plan("aliases.describe").columns, aliases);
}
TEST(NativePlanTest, ShowIsOnlyFixedOneFieldNameObservationAndStrictActualValue) {
  for (const char* label : {"TimeZone", "timezone"}) {
    auto result = Make({label});
    Value(result.get(), 0, 0, "UTC");
    Observation out;
    EXPECT_FALSE(Plan("session.raw_utc.timezone").exact_column_names);
    ASSERT_EQ(CaptureNativeResult(Id("session.raw_utc.timezone"), result.get(), &out),
              CaptureStatus::kComplete);
    EXPECT_EQ(out.fields[0].name, label);
    Value(result.get(), 0, 0, "America/New_York");
    EXPECT_EQ(CaptureNativeResult(Id("session.raw_utc.timezone"), result.get(), &out),
              CaptureStatus::kContextMismatch);
    EXPECT_FALSE(out.complete);
    ASSERT_EQ(out.rows.size(), 1);
    EXPECT_EQ(out.rows[0][0].bytes, "America/New_York");
    RawParameter input;
    EXPECT_EQ(input.Prepare(out, 0, 0), ParameterStatus::kInvalidObservation);
  }
  for (const char* good : {"UTF8", "LATIN1"}) {
    auto result = Make({"client_encoding"});
    Value(result.get(), 0, 0, good);
    Observation out;
    EXPECT_EQ(
        CaptureNativeResult(Id("session.raw_utc.client_encoding"), result.get(), &out),
        std::string_view(good) == "UTF8" ? CaptureStatus::kComplete
                                         : CaptureStatus::kContextMismatch);
  }
  for (int count : {0, 2}) {
    auto r = Make({"timezone"}, count);
    Observation out;
    EXPECT_NE(CaptureNativeResult(Id("session.raw_utc.timezone"), r.get(), &out),
              CaptureStatus::kComplete);
  }
  auto extra = Make({"timezone", "extra"});
  Observation out;
  EXPECT_EQ(CaptureNativeResult(Id("session.raw_utc.timezone"), extra.get(), &out),
            CaptureStatus::kShapeMismatch);
  auto null = Make({"timezone"});
  ASSERT_TRUE(PQsetvalue(null.get(), 0, 0, nullptr, 0));
  EXPECT_EQ(CaptureNativeResult(Id("session.raw_utc.timezone"), null.get(), &out),
            CaptureStatus::kContextMismatch);
  ASSERT_EQ(out.rows.size(), 1);
  EXPECT_TRUE(out.rows[0][0].is_null);
  Value(null.get(), 0, 0, "");
  EXPECT_EQ(CaptureNativeResult(Id("session.raw_utc.timezone"), null.get(), &out),
            CaptureStatus::kContextMismatch);
  ASSERT_EQ(out.rows.size(), 1);
  EXPECT_FALSE(out.rows[0][0].is_null);
  EXPECT_TRUE(out.rows[0][0].bytes.empty());
  CaptureLimits limited;
  limited.cell_bytes = 2;
  Value(null.get(), 0, 0, "LATIN1");
  EXPECT_EQ(CaptureNativeResult(Id("session.raw_utc.client_encoding"), null.get(), &out,
                                limited),
            CaptureStatus::kCellLimit);
  EXPECT_TRUE(out.rows.empty());
}
TEST(NativePlanTest, FormatMismatchBodiesDiagnosticAndFailureOverridesWithoutStale) {
  auto r = Make({"case_id", "value"}, 1, 0);
  Value(r.get(), 0, 1, std::string("\0\x80\xff", 3));
  Observation out;
  EXPECT_EQ(CaptureNativeResult(Id("spatial.geometry.01.binary"), r.get(), &out),
            CaptureStatus::kFormatMismatch);
  ASSERT_EQ(out.rows.size(), 1);
  EXPECT_FALSE(out.complete);
  EXPECT_EQ(out.rows[0][1].bytes, std::string("\0\x80\xff", 3));
  CaptureLimits small;
  small.cell_bytes = 2;
  EXPECT_EQ(CaptureNativeResult(Id("spatial.geometry.01.binary"), r.get(), &out, small),
            CaptureStatus::kCellLimit);
  EXPECT_TRUE(out.rows.empty());
  small = CaptureLimits{};
  small.encoded_bytes = 1;
  EXPECT_EQ(CaptureNativeResult(Id("spatial.geometry.01.text"), r.get(), &out, small),
            CaptureStatus::kEncodingLimit);
  EXPECT_FALSE(out.present);
  EXPECT_TRUE(out.json.empty());
  EXPECT_EQ(CaptureNativeResult(static_cast<NativeStepId>(65535), r.get(), &out),
            CaptureStatus::kInvalidProbe);
  EXPECT_FALSE(out.present);
  EXPECT_EQ(CaptureNativeResult(Id("spatial.geometry.01.text"), nullptr, &out),
            CaptureStatus::kNullResult);
  EXPECT_FALSE(out.complete);
}
TEST(NativePlanTest, NativeAllocationFailureAndLimitsDoNotPublishPreviousSuccess) {
  auto r = Make({"case_id", "value"});
  Observation out;
  ASSERT_EQ(CaptureNativeResult(Id("spatial.geometry.01.text"), r.get(), &out),
            CaptureStatus::kComplete);
  const auto id = Id("spatial.geometry.01.text");
  allocation_fail_after = 0;
  auto status = CaptureNativeResult(id, r.get(), &out);
  allocation_fail_after = -1;
  EXPECT_EQ(status, CaptureStatus::kAllocationFailure);
  EXPECT_FALSE(out.present);
  CaptureLimits limits;
  limits.fields = 1;
  EXPECT_EQ(CaptureNativeResult(id, r.get(), &out, limits), CaptureStatus::kFieldLimit);
  limits = CaptureLimits{};
  limits.rows = 0;
  EXPECT_EQ(CaptureNativeResult(id, r.get(), &out, limits), CaptureStatus::kRowLimit);
}
TEST(NativePlanTest, NewCellBodyNameAndEncodedExactNeighbors) {
  const auto id = Id("spatial.geometry.01.text");
  auto r = Make({"case_id", "value"});
  Observation out;
  Value(r.get(), 0, 1, std::string(65536, 'x'));
  ASSERT_EQ(CaptureNativeResult(id, r.get(), &out), CaptureStatus::kComplete);
  CaptureLimits exact;
  exact.encoded_bytes = static_cast<int64_t>(out.json.size());
  EXPECT_EQ(CaptureNativeResult(id, r.get(), &out, exact), CaptureStatus::kComplete);
  --exact.encoded_bytes;
  EXPECT_EQ(CaptureNativeResult(id, r.get(), &out, exact), CaptureStatus::kEncodingLimit);
  EXPECT_FALSE(out.present);
  Value(r.get(), 0, 1, std::string(65537, 'x'));
  EXPECT_EQ(CaptureNativeResult(id, r.get(), &out), CaptureStatus::kCellLimit);
  EXPECT_TRUE(out.rows.empty());
  auto wide = Make(SuperColumns(false));
  for (int i = 0; i < 16; ++i) Value(wide.get(), 0, i, std::string(65536, 'z'));
  ASSERT_EQ(CaptureNativeResult(Id("parameter.super.case09.format0"), wide.get(), &out),
            CaptureStatus::kComplete);
  Value(wide.get(), 0, 16, "x");
  EXPECT_EQ(CaptureNativeResult(Id("parameter.super.case09.format0"), wide.get(), &out),
            CaptureStatus::kBodyLimit);
  EXPECT_TRUE(out.rows.empty());
  auto name = Make({std::string(127, 'a')});
  Value(name.get(), 0, 0, "UTC");
  EXPECT_EQ(CaptureNativeResult(Id("session.raw_utc.timezone"), name.get(), &out),
            CaptureStatus::kComplete);
  name = Make({std::string(128, 'a')});
  Value(name.get(), 0, 0, "UTC");
  EXPECT_EQ(CaptureNativeResult(Id("session.raw_utc.timezone"), name.get(), &out),
            CaptureStatus::kNameLimit);
  EXPECT_EQ(CaptureNativeResult(id, r.get(), nullptr), CaptureStatus::kInvalidOutput);
}
TEST(NativePlanTest, LegacyJsonStaysIdenticalWithoutNativeCommandFields) {
  for (size_t i = 0; i < static_cast<size_t>(NativeStepId::kCount); ++i) {
    auto p = BuildNativeStep(static_cast<NativeStepId>(i), schema);
    if (!p->legacy_probe) continue;
    const bool desc = p->operation == NativeOperation::kPrepareDescribe;
    const bool error = p->name == "plumbing.error";
    auto r = Make(p->columns, desc || error ? 0 : 1, p->requested_format.value_or(0),
                  error  ? PGRES_FATAL_ERROR
                  : desc ? PGRES_COMMAND_OK
                         : PGRES_TUPLES_OK);
    Observation native, old;
    EXPECT_EQ(CaptureNativeResult(p->id, r.get(), &native),
              CaptureResult(*p->legacy_probe, r.get(), &old));
    EXPECT_EQ(native.json, old.json);
    EXPECT_EQ(native.json.find("affected_rows"), std::string::npos);
  }
}
TEST(NativePlanTest, LegacyRowCapsRemainWhileNativeSequenceOnlyTightens) {
  const auto p = Plan("vendor_mixed.values.text");
  ASSERT_TRUE(p.legacy_probe);
  EXPECT_EQ(p.max_rows, 2);
  EXPECT_EQ(BuildProbe(*p.legacy_probe, schema)->max_rows, 8);
  auto result = Make(p.columns, 3);
  Observation out;
  EXPECT_EQ(CaptureResult(*p.legacy_probe, result.get(), &out), CaptureStatus::kComplete);
  EXPECT_EQ(CaptureNativeResult(p.id, result.get(), &out), CaptureStatus::kRowLimit);
  EXPECT_TRUE(out.rows.empty());
}
TEST(NativePlanTest, FixedInputMappingOwnsBytesRejectsWrongSourceCaseShapeFormatAndNull) {
  RawParameter input;
  for (auto name : {"parameter.smallint.0", "parameter.smallint.1",
                    "parameter.geometry.0", "parameter.hllsketch.1"}) {
    const auto p = Plan(name);
    ASSERT_TRUE(p.parameter);
    const auto selection = *p.parameter;
    auto source = Baseline(BuildNativeStep(selection.source, schema)->name);
    ASSERT_EQ(PrepareNativeInput(p.id, selection.source, source, &input),
              ParameterStatus::kReady);
    EXPECT_EQ(input.Get()->format, selection.format);
    EXPECT_EQ(input.Get()->origin, ParameterOrigin::kObserved);
    for (int change = 0; change < 7; ++change) {
      auto bad = source;
      if (change == 0) bad.complete = false;
      if (change == 1) bad.fields[1].name = "lookalike";
      if (change == 2) bad.fields[0].format = 1 - bad.fields[0].format;
      if (change == 3) bad.rows[0][0].bytes = "2";
      if (change == 4) bad.rows.pop_back();
      if (change == 5) bad.rows[0][selection.field].is_null = true;
      if (change == 6) bad.fields[selection.field].oid = 0;
      EXPECT_NE(PrepareNativeInput(p.id, selection.source, bad, &input),
                ParameterStatus::kReady);
      EXPECT_FALSE(input.Get());
      ASSERT_EQ(PrepareNativeInput(p.id, selection.source, source, &input),
                ParameterStatus::kReady);
    }
    EXPECT_EQ(PrepareNativeInput(p.id, Id("aliases.text"), source, &input),
              ParameterStatus::kInvalidSelection);
    EXPECT_FALSE(input.Get());
  }
}
TEST(NativePlanTest, SuperObservedSqlNullDiffersFromFixturesAndCase16KeepsSelection) {
  RawParameter input;
  for (auto name :
       {"parameter.super.case01.format0", "parameter.super.case01.format1",
        "parameter.super.case16.format0", "parameter.super.fixture.sqlnull.format1",
        "parameter.super.fixture.empty_nonnull.format1"}) {
    auto p = Plan(name);
    ASSERT_TRUE(p.parameter);
    auto source = Baseline(BuildNativeStep(p.parameter->source, schema)->name);
    ASSERT_EQ(PrepareNativeInput(p.id, p.parameter->source, source, &input),
              ParameterStatus::kReady);
    EXPECT_EQ(input.Get()->origin, p.parameter->origin);
    EXPECT_EQ(input.Get()->format, p.parameter->format);
    if (std::string_view(name).find("case16") != std::string_view::npos) {
      EXPECT_EQ(p.parameter->row, 7);
      EXPECT_EQ(p.parameter->field, 1);
    }
    const bool null = std::string_view(name).find("case01") != std::string_view::npos ||
                      std::string_view(name).find("sqlnull") != std::string_view::npos;
    EXPECT_EQ(input.Get()->value == nullptr, null);
  }
}
TEST(NativePlanTest, ClosedInputAllocationAndUnexpectedCommandMetadataExpirePriorView) {
  const auto p = Plan("parameter.smallint.0");
  auto source = Baseline("vendor_mixed.values.text");
  RawParameter input;
  ASSERT_EQ(PrepareNativeInput(p.id, p.parameter->source, source, &input),
            ParameterStatus::kReady);
  auto bad = source;
  bad.affected_rows = 1;
  EXPECT_EQ(PrepareNativeInput(p.id, p.parameter->source, bad, &input),
            ParameterStatus::kInvalidObservation);
  EXPECT_FALSE(input.Get());
  ASSERT_EQ(PrepareNativeInput(p.id, p.parameter->source, source, &input),
            ParameterStatus::kReady);
  allocation_fail_after = 0;
  const auto status = PrepareNativeInput(p.id, p.parameter->source, source, &input);
  allocation_fail_after = -1;
  EXPECT_EQ(status, ParameterStatus::kAllocationFailure);
  EXPECT_FALSE(input.Get());
  EXPECT_EQ(PrepareNativeInput(Id("setup.schema"), p.parameter->source, source, &input),
            ParameterStatus::kInvalidSelection);
  EXPECT_FALSE(input.Get());
}
TEST(NativePlanTest, CandidateFirstMiddleLastDependenciesAreIsolatedAndPoliciesClosed) {
  for (auto family : {"geometry", "geography"})
    for (int n : {1, 18, 36}) {
      const auto stem = std::string("candidate.") + family + "." + (n < 10 ? "0" : "") +
                        std::to_string(n);
      const auto p = Plan(stem);
      EXPECT_EQ(p.definitive_error, NativeErrorPolicy::kContinueDefinitive);
      EXPECT_EQ(p.affected.size(), n == 18 ? 4 : 5);
      for (auto id : p.affected)
        EXPECT_TRUE(BuildNativeStep(id, schema)
                        ->name.starts_with(std::string("spatial.") + family + "."));
      EXPECT_EQ(p.prerequisites,
                (std::vector<NativeStepId>{Id("setup.schema"), Id("setup.spatial_cases"),
                                           Id("setup.spatial_cases.baseline")}));
      EXPECT_EQ(p.uncertain, NativeUncertainPolicy::kStopSpentNoRetry);
    }
  EXPECT_EQ(Plan("session.raw_utc.timezone").definitive_error,
            NativeErrorPolicy::kStopRequired);
  EXPECT_EQ(Plan("cleanup.schema").definitive_error, NativeErrorPolicy::kCleanupUnknown);
}
}  // namespace
}  // namespace adbc::qualification
