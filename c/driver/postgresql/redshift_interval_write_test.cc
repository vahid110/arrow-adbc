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

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "bind_stream.h"
#include "copy/reader.h"
#include "copy/writer.h"
#include "driver/framework/utility.h"
#include "redshift_interval_catalog.h"
#include "redshift_interval_write_test_fixture.h"
#include "redshift_numeric_test_fixture.h"

namespace adbcpq {
namespace {
using Family = RedshiftIntervalFamily;
using State = RedshiftIntervalState;
using Qualifier = RedshiftIntervalQualifier;
using adbc::driver::pgwire::BackendProfile;
class IntervalWriteCase {
 public:
  ArrowErrorCode Init(Family family, const std::vector<std::optional<int64_t>>& values,
                      const char* state = "unspecified", const char* qualifier = nullptr,
                      const char* precision = nullptr,
                      ArrowTimeUnit unit = NANOARROW_TIME_UNIT_MICRO,
                      const RedshiftIntervalConstraint* target = nullptr,
                      int32_t days = 0, int64_t nanos = 0) {
    NANOARROW_RETURN_NOT_OK(testing::IntervalWriteSchema(schema.get(), family, state,
                                                         qualifier, precision, unit));
    NANOARROW_RETURN_NOT_OK(testing::IntervalWriteBatch(schema.get(), array.get(), values,
                                                        &error, days, nanos));
    NANOARROW_RETURN_NOT_OK(
        ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error));
    NANOARROW_RETURN_NOT_OK(ArrowArrayViewSetArray(view.get(), array.get(), &error));
    std::shared_ptr<PostgresTypeResolver> resolver;
    NANOARROW_RETURN_NOT_OK(
        testing::FixtureResolver(BackendProfile::Redshift(), &resolver));
    PostgresType type;
    if (target) {
      NANOARROW_RETURN_NOT_OK(resolver->Find(target->family == Family::kYearMonth
                                                 ? testing::kYearMonthOid
                                                 : testing::kDaySecondOid,
                                             &type, &error));
      type = type.WithIntervalConstraint(*target);
    }
    return MakeCopyFieldWriter(schema->children[0], view->children[0], *resolver,
                               target ? &type : nullptr, &writer, false, &error);
  }
  ArrowErrorCode Write(int64_t row = 0) {
    const auto before = bytes->size_bytes;
    auto result = writer->Write(bytes.get(), row, &error);
    if (result) {
      EXPECT_EQ(before, bytes->size_bytes);
    }
    return result;
  }
  std::string Wire() {
    return {reinterpret_cast<char*>(bytes->data), static_cast<size_t>(bytes->size_bytes)};
  }
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
  nanoarrow::UniqueArrayView view;
  nanoarrow::UniqueBuffer bytes;
  std::unique_ptr<PostgresCopyFieldWriter> writer;
  ArrowError error{};
};
TEST(RedshiftIntervalWriteTest, AllThirteenQualifierIdentitiesRenderAndCheckExactWire) {
  struct Case {
    const char* name;
    Family family;
    int64_t quantum;
    const char* hex;
  };
  const Case cases[] = {
      {"YEAR", Family::kYearMonth, 12, "000000040000000c"},
      {"MONTH", Family::kYearMonth, 1, "0000000400000001"},
      {"YEAR TO MONTH", Family::kYearMonth, 1, "0000000400000001"},
      {"DAY", Family::kDaySecond, 86400000000, "00000008000000141dd76000"},
      {"HOUR", Family::kDaySecond, 3600000000, "0000000800000000d693a400"},
      {"MINUTE", Family::kDaySecond, 60000000, "000000080000000003938700"},
      {"SECOND", Family::kDaySecond, 1000, "0000000800000000000003e8"},
      {"DAY TO HOUR", Family::kDaySecond, 3600000000, "0000000800000000d693a400"},
      {"DAY TO MINUTE", Family::kDaySecond, 60000000, "000000080000000003938700"},
      {"DAY TO SECOND", Family::kDaySecond, 1000, "0000000800000000000003e8"},
      {"HOUR TO MINUTE", Family::kDaySecond, 60000000, "000000080000000003938700"},
      {"HOUR TO SECOND", Family::kDaySecond, 1000, "0000000800000000000003e8"},
      {"MINUTE TO SECOND", Family::kDaySecond, 1000, "0000000800000000000003e8"}};
  for (const auto& item : cases) {
    SCOPED_TRACE(item.name);
    const bool second = std::string_view(item.name).ends_with("SECOND");
    IntervalWriteCase write;
    ASSERT_EQ(write.Init(item.family, {item.quantum, -item.quantum}, "known", item.name,
                         second ? "3" : nullptr),
              0)
        << write.error.message;
    ASSERT_EQ(write.Write(), 0) << write.error.message;
    EXPECT_EQ(write.Wire(), testing::NumericHex(item.hex));
    ArrowBufferReset(write.bytes.get());
    ASSERT_EQ(write.Write(1), 0);
    const char* negative =
        item.family == Family::kYearMonth
            ? (item.quantum == 12 ? "00000004fffffff4" : "00000004ffffffff")
        : item.quantum == 86400000000 ? "00000008ffffffebe228a000"
        : item.quantum == 3600000000  ? "00000008ffffffff296c5c00"
        : item.quantum == 60000000    ? "00000008fffffffffc6c7900"
                                      : "00000008fffffffffffffc18";
    EXPECT_EQ(write.Wire(), testing::NumericHex(negative));
    RedshiftIntervalSchema parsed;
    ASSERT_EQ(RedshiftIntervalSchema::Parse(write.schema->children[0], true, &parsed,
                                            &write.error),
              0);
    std::string ddl;
    ASSERT_EQ(parsed.constraint.Declaration(&ddl, &write.error), 0);
    EXPECT_EQ(ddl, "INTERVAL " + std::string(item.name) + (second ? "(3)" : ""));
    if (item.quantum > 1) {
      IntervalWriteCase invalid;
      ASSERT_EQ(invalid.Init(item.family, {item.quantum - 1, -item.quantum + 1}, "known",
                             item.name, second ? "3" : nullptr),
                0);
      EXPECT_NE(invalid.Write(), 0);
      EXPECT_NE(invalid.Write(1), 0);
    }
  }
}
TEST(RedshiftIntervalWriteTest,
     AllSecondPrecisionsAndIndependentSourceTargetConstraints) {
  const int64_t quanta[] = {1000000, 100000, 10000, 1000, 100, 10, 1};
  const char* wires[] = {"0000000800000000000f4240", "0000000800000000000186a0",
                         "000000080000000000002710", "0000000800000000000003e8",
                         "000000080000000000000064", "00000008000000000000000a",
                         "000000080000000000000001"};
  for (int source = 0; source <= 6; ++source)
    for (int destination = 0; destination <= 6; ++destination) {
      auto target = testing::IntervalReferenceConstraint(Family::kDaySecond,
                                                         Qualifier::kSecond, destination);
      const auto p = std::to_string(source);
      const int strictest = source < destination ? source : destination;
      IntervalWriteCase exact;
      ASSERT_EQ(
          exact.Init(Family::kDaySecond, {quanta[strictest], -quanta[strictest]}, "known",
                     "DAY TO SECOND", p.c_str(), NANOARROW_TIME_UNIT_MICRO, &target),
          0);
      ASSERT_EQ(exact.Write(), 0);
      EXPECT_EQ(exact.Wire(), testing::NumericHex(wires[strictest]));
      ArrowBufferReset(exact.bytes.get());
      ASSERT_EQ(exact.Write(1), 0);
      const char* negative_wires[] = {
          "00000008fffffffffff0bdc0", "00000008fffffffffffe7960",
          "00000008ffffffffffffd8f0", "00000008fffffffffffffc18",
          "00000008ffffffffffffff9c", "00000008fffffffffffffff6",
          "00000008ffffffffffffffff"};
      EXPECT_EQ(exact.Wire(), testing::NumericHex(negative_wires[strictest]));
      if (quanta[strictest] > 1) {
        IntervalWriteCase invalid;
        ASSERT_EQ(
            invalid.Init(Family::kDaySecond,
                         {quanta[strictest] - 1, -quanta[strictest] + 1}, "known",
                         "DAY TO SECOND", p.c_str(), NANOARROW_TIME_UNIT_MICRO, &target),
            0);
        EXPECT_EQ(invalid.Write(), EINVAL);
        EXPECT_EQ(invalid.Write(1), EINVAL);
        EXPECT_EQ(invalid.bytes->size_bytes, 0);
      }
    }
}
TEST(RedshiftIntervalWriteTest, SignedAlignedStorageEdgesForEveryDeclaration) {
  struct Case {
    Family family;
    const char* qualifier;
    const char* precision;
    int64_t low;
    int64_t high;
    const char* wire;
  };
  // Storage bounds rounded inward by integer mathematics, not native SQL limits.
  const Case cases[] = {
      {Family::kYearMonth, "YEAR", nullptr, -2147483640LL, 2147483640LL,
       "0000000480000008000000047ffffff8"},
      {Family::kYearMonth, "MONTH", nullptr, -2147483648LL, 2147483647LL,
       "0000000480000000000000047fffffff"},
      {Family::kYearMonth, "YEAR TO MONTH", nullptr, -2147483648LL, 2147483647LL,
       "0000000480000000000000047fffffff"},
      {Family::kDaySecond, "DAY", nullptr, -9223372022400000000LL, 9223372022400000000LL,
       "00000008800000035d926000000000087ffffffca26da000"},
      {Family::kDaySecond, "HOUR", nullptr, -9223372036800000000LL, 9223372036800000000LL,
       "00000008800000000343d000000000087ffffffffcbc3000"},
      {Family::kDaySecond, "MINUTE", nullptr, -9223372036800000000LL,
       9223372036800000000LL, "00000008800000000343d000000000087ffffffffcbc3000"},
      {Family::kDaySecond, "DAY TO HOUR", nullptr, -9223372036800000000LL,
       9223372036800000000LL, "00000008800000000343d000000000087ffffffffcbc3000"},
      {Family::kDaySecond, "DAY TO MINUTE", nullptr, -9223372036800000000LL,
       9223372036800000000LL, "00000008800000000343d000000000087ffffffffcbc3000"},
      {Family::kDaySecond, "HOUR TO MINUTE", nullptr, -9223372036800000000LL,
       9223372036800000000LL, "00000008800000000343d000000000087ffffffffcbc3000"},
      {Family::kDaySecond, "SECOND", "0", -9223372036854000000LL, 9223372036854000000LL,
       "0000000880000000000bd680000000087ffffffffff42980"},
      {Family::kDaySecond, "SECOND", "1", -9223372036854700000LL, 9223372036854700000LL,
       "000000088000000000012820000000087ffffffffffed7e0"},
      {Family::kDaySecond, "SECOND", "2", -9223372036854770000LL, 9223372036854770000LL,
       "0000000880000000000016b0000000087fffffffffffe950"},
      {Family::kDaySecond, "SECOND", "3", -9223372036854775000LL, 9223372036854775000LL,
       "000000088000000000000328000000087ffffffffffffcd8"},
      {Family::kDaySecond, "SECOND", "4", -9223372036854775800LL, 9223372036854775800LL,
       "000000088000000000000008000000087ffffffffffffff8"},
      {Family::kDaySecond, "SECOND", "5", -9223372036854775800LL, 9223372036854775800LL,
       "000000088000000000000008000000087ffffffffffffff8"},
      {Family::kDaySecond, "SECOND", "6", INT64_MIN, 9223372036854775807LL,
       "000000088000000000000000000000087fffffffffffffff"},
      {Family::kDaySecond, "DAY TO SECOND", "0", -9223372036854000000LL,
       9223372036854000000LL, "0000000880000000000bd680000000087ffffffffff42980"},
      {Family::kDaySecond, "DAY TO SECOND", "1", -9223372036854700000LL,
       9223372036854700000LL, "000000088000000000012820000000087ffffffffffed7e0"},
      {Family::kDaySecond, "DAY TO SECOND", "2", -9223372036854770000LL,
       9223372036854770000LL, "0000000880000000000016b0000000087fffffffffffe950"},
      {Family::kDaySecond, "DAY TO SECOND", "3", -9223372036854775000LL,
       9223372036854775000LL, "000000088000000000000328000000087ffffffffffffcd8"},
      {Family::kDaySecond, "DAY TO SECOND", "4", -9223372036854775800LL,
       9223372036854775800LL, "000000088000000000000008000000087ffffffffffffff8"},
      {Family::kDaySecond, "DAY TO SECOND", "5", -9223372036854775800LL,
       9223372036854775800LL, "000000088000000000000008000000087ffffffffffffff8"},
      {Family::kDaySecond, "DAY TO SECOND", "6", INT64_MIN, 9223372036854775807LL,
       "000000088000000000000000000000087fffffffffffffff"},
      {Family::kDaySecond, "HOUR TO SECOND", "0", -9223372036854000000LL,
       9223372036854000000LL, "0000000880000000000bd680000000087ffffffffff42980"},
      {Family::kDaySecond, "HOUR TO SECOND", "1", -9223372036854700000LL,
       9223372036854700000LL, "000000088000000000012820000000087ffffffffffed7e0"},
      {Family::kDaySecond, "HOUR TO SECOND", "2", -9223372036854770000LL,
       9223372036854770000LL, "0000000880000000000016b0000000087fffffffffffe950"},
      {Family::kDaySecond, "HOUR TO SECOND", "3", -9223372036854775000LL,
       9223372036854775000LL, "000000088000000000000328000000087ffffffffffffcd8"},
      {Family::kDaySecond, "HOUR TO SECOND", "4", -9223372036854775800LL,
       9223372036854775800LL, "000000088000000000000008000000087ffffffffffffff8"},
      {Family::kDaySecond, "HOUR TO SECOND", "5", -9223372036854775800LL,
       9223372036854775800LL, "000000088000000000000008000000087ffffffffffffff8"},
      {Family::kDaySecond, "HOUR TO SECOND", "6", INT64_MIN, 9223372036854775807LL,
       "000000088000000000000000000000087fffffffffffffff"},
      {Family::kDaySecond, "MINUTE TO SECOND", "0", -9223372036854000000LL,
       9223372036854000000LL, "0000000880000000000bd680000000087ffffffffff42980"},
      {Family::kDaySecond, "MINUTE TO SECOND", "1", -9223372036854700000LL,
       9223372036854700000LL, "000000088000000000012820000000087ffffffffffed7e0"},
      {Family::kDaySecond, "MINUTE TO SECOND", "2", -9223372036854770000LL,
       9223372036854770000LL, "0000000880000000000016b0000000087fffffffffffe950"},
      {Family::kDaySecond, "MINUTE TO SECOND", "3", -9223372036854775000LL,
       9223372036854775000LL, "000000088000000000000328000000087ffffffffffffcd8"},
      {Family::kDaySecond, "MINUTE TO SECOND", "4", -9223372036854775800LL,
       9223372036854775800LL, "000000088000000000000008000000087ffffffffffffff8"},
      {Family::kDaySecond, "MINUTE TO SECOND", "5", -9223372036854775800LL,
       9223372036854775800LL, "000000088000000000000008000000087ffffffffffffff8"},
      {Family::kDaySecond, "MINUTE TO SECOND", "6", INT64_MIN, 9223372036854775807LL,
       "000000088000000000000000000000087fffffffffffffff"},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(item.qualifier);
    SCOPED_TRACE(item.precision ? item.precision : "no precision");
    IntervalWriteCase write;
    ASSERT_EQ(write.Init(item.family, {item.low, item.high}, "known", item.qualifier,
                         item.precision),
              0);
    ASSERT_EQ(write.Write(0), 0) << write.error.message;
    ASSERT_EQ(write.Write(1), 0) << write.error.message;
    EXPECT_EQ(write.Wire(), testing::NumericHex(item.wire));
  }
}

TEST(RedshiftIntervalWriteTest, SignedConversionThresholdsPreserveExistingBytes) {
  struct Case {
    ArrowTimeUnit unit;
    int64_t exact;
    int64_t invalid;
    const char* wire;
  };
  // Literal boundaries and wire bytes, independent of the production conversion.
  const Case cases[] = {
      {NANOARROW_TIME_UNIT_SECOND, 9223372036854LL, 9223372036855LL,
       "000000087ffffffffff42980"},
      {NANOARROW_TIME_UNIT_SECOND, -9223372036854LL, -9223372036855LL,
       "0000000880000000000bd680"},
      {NANOARROW_TIME_UNIT_MILLI, 9223372036854775LL, 9223372036854776LL,
       "000000087ffffffffffffcd8"},
      {NANOARROW_TIME_UNIT_MILLI, -9223372036854775LL, -9223372036854776LL,
       "000000088000000000000328"},
      {NANOARROW_TIME_UNIT_NANO, 9223372036854775000LL, 9223372036854775001LL,
       "000000080020c49ba5e353f7"},
      {NANOARROW_TIME_UNIT_NANO, -9223372036854775000LL, -9223372036854775001LL,
       "00000008ffdf3b645a1cac09"},
      {NANOARROW_TIME_UNIT_NANO, 1000LL, 1001LL, "000000080000000000000001"},
      {NANOARROW_TIME_UNIT_NANO, -1000LL, -1001LL, "00000008ffffffffffffffff"},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(item.exact);
    IntervalWriteCase write;
    ASSERT_EQ(write.Init(Family::kDaySecond, {item.exact, item.invalid}, "unspecified",
                         nullptr, nullptr, item.unit),
              0);
    ASSERT_EQ(write.Write(), 0) << write.error.message;
    EXPECT_EQ(write.Wire(), testing::NumericHex(item.wire));
    const auto before = write.Wire();
    EXPECT_EQ(write.Write(1), EINVAL);
    EXPECT_EQ(write.Wire(), before);
    EXPECT_NE(std::string_view(write.error.message)
                  .find(item.unit == NANOARROW_TIME_UNIT_NANO ? "submicrosecond loss"
                                                              : "duration unit overflow"),
              std::string_view::npos);
  }
  IntervalWriteCase submicro;
  ASSERT_EQ(submicro.Init(Family::kDaySecond, {-1, -999}, "unspecified", nullptr, nullptr,
                          NANOARROW_TIME_UNIT_NANO),
            0);
  EXPECT_EQ(submicro.Write(0), EINVAL);
  EXPECT_EQ(submicro.Write(1), EINVAL);
  EXPECT_EQ(submicro.bytes->size_bytes, 0);
}

TEST(RedshiftIntervalWriteTest,
     ReferenceExtremaExactUnitsAndRuntimeMixturesNeverMutateBuffer) {
  for (auto family : {Family::kYearMonth, Family::kDaySecond}) {
    IntervalWriteCase write;
    ASSERT_EQ(write.Init(family, {family == Family::kYearMonth ? INT32_MIN : INT64_MIN,
                                  family == Family::kYearMonth ? INT32_MAX : INT64_MAX}),
              0);
    ASSERT_EQ(write.Write(), 0);
    ASSERT_EQ(write.Write(1), 0);
    EXPECT_EQ(
        write.Wire(),
        testing::NumericHex(family == Family::kYearMonth
                                ? "0000000480000000000000047fffffff"
                                : "000000088000000000000000000000087fffffffffffffff"));
  }
  for (auto unit : {NANOARROW_TIME_UNIT_SECOND, NANOARROW_TIME_UNIT_MILLI,
                    NANOARROW_TIME_UNIT_MICRO, NANOARROW_TIME_UNIT_NANO}) {
    IntervalWriteCase exact;
    const int64_t raw = unit == NANOARROW_TIME_UNIT_SECOND  ? 1
                        : unit == NANOARROW_TIME_UNIT_MILLI ? 1000
                        : unit == NANOARROW_TIME_UNIT_MICRO ? 1000000
                                                            : 1000000000;
    ASSERT_EQ(
        exact.Init(Family::kDaySecond, {raw}, "unspecified", nullptr, nullptr, unit), 0);
    ASSERT_EQ(exact.Write(), 0);
    EXPECT_EQ(exact.Wire(), testing::NumericHex("0000000800000000000f4240"));
  }
  for (auto unit : {NANOARROW_TIME_UNIT_SECOND, NANOARROW_TIME_UNIT_MILLI,
                    NANOARROW_TIME_UNIT_NANO}) {
    IntervalWriteCase invalid;
    ASSERT_EQ(invalid.Init(Family::kDaySecond, {INT64_MAX, INT64_MIN}, "unspecified",
                           nullptr, nullptr, unit),
              0);
    EXPECT_NE(invalid.Write(), 0);
    EXPECT_NE(invalid.Write(1), 0);
  }
  for (auto parts : {std::pair<int32_t, int64_t>{1, 0}, {0, 1}, {-1, -1}}) {
    IntervalWriteCase invalid;
    ASSERT_EQ(invalid.Init(Family::kYearMonth, {14}, "unspecified", nullptr, nullptr,
                           NANOARROW_TIME_UNIT_MICRO, nullptr, parts.first, parts.second),
              0);
    EXPECT_NE(invalid.Write(), 0);
  }
}
TEST(RedshiftIntervalWriteTest,
     UnspecifiedAndUnknownRemainDistinctAndDefaultsNeverEraseKnown) {
  for (auto family : {Family::kYearMonth, Family::kDaySecond}) {
    IntervalWriteCase source;
    ASSERT_EQ(source.Init(family, {1}), 0);
    RedshiftIntervalSchema parsed;
    ASSERT_EQ(RedshiftIntervalSchema::Parse(source.schema->children[0], true, &parsed,
                                            &source.error),
              0);
    std::string ddl;
    ASSERT_EQ(parsed.constraint.Declaration(&ddl, &source.error), 0);
    EXPECT_EQ(ddl, family == Family::kYearMonth ? "INTERVAL YEAR TO MONTH"
                                                : "INTERVAL DAY TO SECOND(6)");
    ASSERT_EQ(testing::IntervalSetMetadata(source.schema->children[0],
                                           "ADBC:redshift:interval_constraint_state",
                                           "unknown"),
              0);
    EXPECT_EQ(RedshiftIntervalSchema::Parse(source.schema->children[0], true, &parsed,
                                            &source.error),
              ENOTSUP);
    ASSERT_EQ(testing::IntervalSetMetadata(source.schema->children[0],
                                           "ADBC:redshift:interval_constraint_state",
                                           "unspecified"),
              0);
    ASSERT_EQ(testing::DecimalMetadata(source.schema->children[0],
                                       "ADBC:redshift:type_modifier", "-1"),
              0);
    EXPECT_NE(RedshiftIntervalSchema::Parse(source.schema->children[0], true, &parsed,
                                            &source.error),
              0);
  }
}
TEST(RedshiftIntervalWriteTest,
     ClosedMetadataAndRecursiveStorageCannotBypassSemanticWriter) {
  const std::pair<const char*, const char*> invalid[] = {
      {"ADBC:redshift:interval_family", "future"},
      {"ADBC:redshift:interval_contract_version", "2"},
      {"ADBC:redshift:interval_constraint_state", "future"},
      {"ADBC:redshift:interval_qualifier", "YEAR"},
      {"ADBC:redshift:fractional_precision", "03"},
      {"ADBC:redshift:fractional_precision", "7"},
      {"ADBC:redshift:sql_type", "INTERVAL YEAR TO MONTH"},
      {"ARROW:extension:name", "foreign"},
      {"ARROW:extension:metadata", "orphan"}};
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::FixtureResolver(BackendProfile::Redshift(), &resolver), 0);
  for (auto item : invalid) {
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(testing::IntervalWriteSchema(schema.get(), Family::kDaySecond, "known",
                                           "SECOND", "3"),
              0);
    ASSERT_EQ(testing::IntervalSetMetadata(schema->children[0], item.first, item.second),
              0);
    ArrowError error{};
    EXPECT_NE(PostgresType::ValidateIntervalWriteSchema(*resolver, schema.get(), &error),
              0)
        << item.first;
  }
  for (const char* key :
       {"ADBC:redshift:interval_family", "ADBC:redshift:sql_type",
        "ADBC:redshift:interval_contract_version",
        "ADBC:redshift:interval_constraint_state", "ADBC:redshift:interval_qualifier",
        "ADBC:redshift:fractional_precision"}) {
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(testing::IntervalWriteSchema(schema.get(), Family::kDaySecond, "known",
                                           "SECOND", "3"),
              0);
    ASSERT_EQ(testing::DecimalMetadata(schema->children[0], key, "duplicate"), 0);
    ArrowError error{};
    EXPECT_NE(PostgresType::ValidateIntervalWriteSchema(*resolver, schema.get(), &error),
              0);
    EXPECT_NE(std::string(error.message).find("duplicate"), std::string::npos);
  }
  for (bool dictionary : {false, true}) {
    nanoarrow::UniqueSchema source;
    ASSERT_EQ(testing::IntervalWriteSchema(source.get(), Family::kYearMonth), 0);
    nanoarrow::UniqueSchema outer;
    ArrowSchemaInit(outer.get());
    if (dictionary) {
      ASSERT_EQ(ArrowSchemaSetType(outer.get(), NANOARROW_TYPE_INT32), 0);
      ASSERT_EQ(ArrowSchemaAllocateDictionary(outer.get()), 0);
      ArrowSchemaMove(source->children[0], outer->dictionary);
    } else {
      ASSERT_EQ(ArrowSchemaSetType(outer.get(), NANOARROW_TYPE_LIST), 0);
      if (outer->children[0]->release) outer->children[0]->release(outer->children[0]);
      ArrowSchemaMove(source->children[0], outer->children[0]);
    }
    ArrowError error{};
    EXPECT_NE(PostgresType::ValidateIntervalWriteSchema(*resolver, outer.get(), &error),
              0);
  }
  nanoarrow::UniqueSchema marked_null;
  ASSERT_EQ(testing::IntervalWriteSchema(marked_null.get(), Family::kDaySecond), 0);
  ASSERT_EQ(ArrowSchemaSetType(marked_null->children[0], NANOARROW_TYPE_NA), 0);
  EXPECT_NE(
      PostgresType::ValidateIntervalWriteSchema(*resolver, marked_null.get(), nullptr),
      0);
  nanoarrow::UniqueSchema ordinary;
  ArrowSchemaInitFromType(ordinary.get(), NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO);
  PostgresTypeResolver pg(BackendProfile::PostgreSQL());
  ASSERT_EQ(pg.Insert({1186, "interval", "interval_recv", 0, 0, 0}, nullptr), 0);
  PostgresType type;
  EXPECT_EQ(PostgresType::FromSchema(pg, ordinary.get(), &type, nullptr), 0);
  EXPECT_EQ(type.type_id(), PostgresTypeId::kInterval);
  EXPECT_NE(PostgresType::ValidateIntervalWriteSchema(*resolver, ordinary.get(), nullptr),
            0);
}
TEST(RedshiftIntervalWriteTest,
     KnownReadsValidateBeforeCursorAndUnknownDiagnosticsCannotRebind) {
  for (auto family : {Family::kYearMonth, Family::kDaySecond}) {
    const auto id = family == Family::kYearMonth
                        ? PostgresTypeId::kRedshiftIntervalYearMonth
                        : PostgresTypeId::kRedshiftIntervalDaySecond;
    auto c = testing::IntervalReferenceConstraint(
        family, family == Family::kYearMonth ? Qualifier::kYear : Qualifier::kSecond,
        family == Family::kYearMonth ? std::nullopt : std::optional<int32_t>{3});
    auto type = PostgresType(id).WithIntervalConstraint(c);
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(type.SetSchema(schema.get(), "Redshift"), 0);
    nanoarrow::UniqueArray array;
    ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
    std::unique_ptr<PostgresCopyFieldReader> reader;
    ASSERT_EQ(MakeCopyFieldReader(type, schema.get(), &reader, nullptr), 0);
    reader->Init(type);
    ASSERT_EQ(reader->InitSchema(schema.get()), 0);
    ASSERT_EQ(reader->InitArray(array.get()), 0);
    const auto bad = testing::NumericHex(
        family == Family::kYearMonth ? "0000000e" : "00000000000004d2");
    ArrowBufferView data{};
    data.data.data = bad.data();
    data.size_bytes = bad.size();
    auto original = data.data.data;
    EXPECT_NE(reader->Read(&data, static_cast<int32_t>(bad.size()), array.get(), nullptr),
              0);
    EXPECT_EQ(data.data.data, original);
    EXPECT_EQ(data.size_bytes, static_cast<int64_t>(bad.size()));
    EXPECT_EQ(array->length, 0);
    const auto good = testing::NumericHex(
        family == Family::kYearMonth ? "0000000c" : "00000000000003e8");
    data.data.data = good.data();
    data.size_bytes = good.size();
    ASSERT_EQ(
        reader->Read(&data, static_cast<int32_t>(good.size()), array.get(), nullptr), 0);
    EXPECT_EQ(data.size_bytes, 0);
    EXPECT_EQ(array->length, 1);
    auto unknown =
        PostgresType(id).WithFieldDescriptor(12345, BackendProfile::Redshift());
    nanoarrow::UniqueSchema raw;
    ArrowSchemaInit(raw.get());
    ASSERT_EQ(unknown.SetSchema(raw.get(), "Redshift"), 0);
    RedshiftIntervalSchema parsed;
    ASSERT_EQ(RedshiftIntervalSchema::Parse(raw.get(), true, &parsed, nullptr, true), 0);
    EXPECT_EQ(parsed.constraint.state, State::kUnknown);
    EXPECT_FALSE(parsed.constraint.precision);
    EXPECT_EQ(RedshiftIntervalSchema::Parse(raw.get(), true, &parsed, nullptr), ENOTSUP);
    ASSERT_EQ(testing::IntervalSetMetadata(schema.get(),
                                           "ADBC:redshift:fractional_precision", "7"),
              0);
    EXPECT_NE(MakeCopyFieldReader(type, schema.get(), &reader, nullptr), 0);
  }
}
TEST(RedshiftIntervalWriteTest, NullShortcutRejectsForeignSemanticsAndDictionary) {
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::FixtureResolver(BackendProfile::Redshift(), &resolver), 0);
  PostgresType target;
  ASSERT_EQ(resolver->Find(testing::kDaySecondOid, &target, nullptr), 0);
  target = target.WithIntervalConstraint(
      testing::IntervalReferenceConstraint(Family::kDaySecond, Qualifier::kDay));
  for (const auto& marker : {std::pair{"ARROW:extension:name", "foreign"},
                             {"ARROW:extension:metadata", "{}"},
                             {"ADBC:redshift:sql_type", "HLLSKETCH"},
                             {"ADBC:redshift:byte_width", "10"},
                             {"ADBC:redshift:type_modifier", "-1"}}) {
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_NA);
    ASSERT_EQ(testing::DecimalMetadata(schema.get(), marker.first, marker.second), 0);
    nanoarrow::UniqueArrayView view;
    ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), nullptr), 0);
    std::unique_ptr<PostgresCopyFieldWriter> writer;
    EXPECT_NE(PostgresType::ValidateIntervalWriteSchema(*resolver, schema.get(), nullptr),
              0);
    EXPECT_NE(MakeCopyFieldWriter(schema.get(), view.get(), *resolver, &target, &writer,
                                  false, nullptr),
              0);
    EXPECT_FALSE(writer);
  }
  nanoarrow::UniqueSchema dictionary;
  ArrowSchemaInitFromType(dictionary.get(), NANOARROW_TYPE_INT8);
  ASSERT_EQ(ArrowSchemaAllocateDictionary(dictionary.get()), 0);
  ArrowSchemaInitFromType(dictionary->dictionary, NANOARROW_TYPE_NA);
  nanoarrow::UniqueArrayView view;
  ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), dictionary.get(), nullptr), 0);
  std::unique_ptr<PostgresCopyFieldWriter> writer;
  EXPECT_NE(
      PostgresType::ValidateIntervalWriteSchema(*resolver, dictionary.get(), nullptr), 0);
  EXPECT_NE(MakeCopyFieldWriter(dictionary.get(), view.get(), *resolver, &target, &writer,
                                false, nullptr),
            0);
  EXPECT_FALSE(writer);
}
TEST(RedshiftIntervalWriteTest,
     ActualTargetValidationPrecedesGenuineNullWriterAndOidSelection) {
  for (auto family : {Family::kYearMonth, Family::kDaySecond})
    for (bool known : {false, true})
      for (int count : {0, 2}) {
        auto target = testing::IntervalReferenceConstraint(
            family, family == Family::kYearMonth ? Qualifier::kYear : Qualifier::kDay);
        if (!known) target.state = State::kUnknown;
        std::shared_ptr<PostgresTypeResolver> resolver;
        ASSERT_EQ(testing::FixtureResolver(BackendProfile::Redshift(), &resolver), 0);
        PostgresType type;
        ASSERT_EQ(resolver->Find(family == Family::kYearMonth ? testing::kYearMonthOid
                                                              : testing::kDaySecondOid,
                                 &type, nullptr),
                  0);
        type = type.WithIntervalConstraint(target);
        PostgresType fields(PostgresTypeId::kRecord);
        fields.AppendChild("amount", type);
        nanoarrow::UniqueSchema schema;
        ArrowSchemaInit(schema.get());
        ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
        ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_NA), 0);
        ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "amount"), 0);
        ASSERT_EQ(testing::DecimalMetadata(schema->children[0], "example:note", "client"),
                  0);
        nanoarrow::UniqueArray array;
        ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
        ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
        for (int i = 0; i < count; ++i) {
          ASSERT_EQ(ArrowArrayAppendNull(array->children[0], 1), 0);
          ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
        }
        ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
        nanoarrow::UniqueArrayStream input;
        adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
        BindStream bind;
        bind.SetBind(input.get());
        ASSERT_TRUE(bind.Begin([] { return Status::Ok(); }).ok());
        auto result = bind.SetParamTypes(nullptr, *resolver, true, &fields);
        EXPECT_EQ(result.ok(), known);
        if (known) {
          EXPECT_EQ(bind.param_types[0], type.oid());
        }
      }
}
}  // namespace
}  // namespace adbcpq

namespace adbcpq {
namespace {
void SetObservation(PqResultHelper* out,
                    const std::vector<std::vector<const char*>>& rows, int columns) {
  auto* result = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  out->SetResult(result);
  std::vector<PGresAttDesc> attrs(columns);
  for (auto& attr : attrs) attr.name = const_cast<char*>("fixture");
  ASSERT_EQ(PQsetResultAttrs(result, columns, attrs.data()), 1);
  for (size_t row = 0; row < rows.size(); ++row)
    for (int col = 0; col < columns; ++col) {
      const auto* value = rows[row][col];
      ASSERT_EQ(PQsetvalue(result, static_cast<int>(row), col, const_cast<char*>(value),
                           value ? static_cast<int>(std::strlen(value)) : -1),
                1);
    }
}
TEST(RedshiftIntervalCatalogTest, ExactNativeProducerMatrixAndForeignFacts) {
  // Native operation cd9f2ad028224383be5e7fb164691305 observations 104/708.
  struct Captured {
    const char* raw;
    const char* formatted;
    const char* qualifier;
    int precision;
    bool months;
  };
  const Captured captured[] = {
      {"327683", "interval year", "YEAR", -1, true},
      {"196611", "interval month", "MONTH", -1, true},
      {"458755", "interval year to month", "YEAR TO MONTH", -1, true},
      {"589827", "interval day", "DAY", -1, false},
      {"67174403", "interval hour", "HOUR", -1, false},
      {"134283267", "interval minute", "MINUTE", -1, false},
      {"268435460", "interval second (0)", "SECOND", 0, false},
      {"268435461", "interval second (1)", "SECOND", 1, false},
      {"268435462", "interval second (2)", "SECOND", 2, false},
      {"268435463", "interval second (3)", "SECOND", 3, false},
      {"268435464", "interval second (4)", "SECOND", 4, false},
      {"268435465", "interval second (5)", "SECOND", 5, false},
      {"268435466", "interval second (6)", "SECOND", 6, false},
      {"67698691", "interval day to hour", "DAY TO HOUR", -1, false},
      {"201916419", "interval day to minute", "DAY TO MINUTE", -1, false},
      {"470286340", "interval day to second (0)", "DAY TO SECOND", 0, false},
      {"470286341", "interval day to second (1)", "DAY TO SECOND", 1, false},
      {"470286342", "interval day to second (2)", "DAY TO SECOND", 2, false},
      {"470286343", "interval day to second (3)", "DAY TO SECOND", 3, false},
      {"470286344", "interval day to second (4)", "DAY TO SECOND", 4, false},
      {"470286345", "interval day to second (5)", "DAY TO SECOND", 5, false},
      {"470286346", "interval day to second (6)", "DAY TO SECOND", 6, false},
      {"201392131", "interval hour to minute", "HOUR TO MINUTE", -1, false},
      {"469762052", "interval hour to second (0)", "HOUR TO SECOND", 0, false},
      {"469762053", "interval hour to second (1)", "HOUR TO SECOND", 1, false},
      {"469762054", "interval hour to second (2)", "HOUR TO SECOND", 2, false},
      {"469762055", "interval hour to second (3)", "HOUR TO SECOND", 3, false},
      {"469762056", "interval hour to second (4)", "HOUR TO SECOND", 4, false},
      {"469762057", "interval hour to second (5)", "HOUR TO SECOND", 5, false},
      {"469762058", "interval hour to second (6)", "HOUR TO SECOND", 6, false},
      {"402653188", "interval minute to second (0)", "MINUTE TO SECOND", 0, false},
      {"402653189", "interval minute to second (1)", "MINUTE TO SECOND", 1, false},
      {"402653190", "interval minute to second (2)", "MINUTE TO SECOND", 2, false},
      {"402653191", "interval minute to second (3)", "MINUTE TO SECOND", 3, false},
      {"402653192", "interval minute to second (4)", "MINUTE TO SECOND", 4, false},
      {"402653193", "interval minute to second (5)", "MINUTE TO SECOND", 5, false},
      {"402653194", "interval minute to second (6)", "MINUTE TO SECOND", 6, false},
  };
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::FixtureResolver(BackendProfile::Redshift(), &resolver), 0);
  for (const auto& item : captured) {
    SCOPED_TRACE(item.formatted);
    for (int variant = 0; variant < 10; ++variant) {
      // Matched, family mismatch, absent/-1/foreign raw, NULL/text drift,
      // and each previously unqualified non-NULL SVV diagnostic.
      const bool months = item.months != (variant == 1);
      const char* oid = months ? "64001" : "65017";
      const char* raw = variant == 2   ? nullptr
                        : variant == 3 ? "-1"
                        : variant == 4 ? "12345"
                                       : item.raw;
      const char* formatted = variant == 5   ? nullptr
                              : variant == 6 ? "interval"
                                             : item.formatted;
      PostgresType type;
      ASSERT_EQ(resolver->Find(months ? testing::kYearMonthOid : testing::kDaySecondOid,
                               &type, nullptr),
                0);
      type = type.WithFieldDescriptor(
          raw ? std::optional<int32_t>{std::stoi(raw)} : std::nullopt,
          BackendProfile::Redshift());
      PostgresType table(PostgresTypeId::kRecord);
      table.AppendChild("amount", type);
      PqResultHelper identity(nullptr, "");
      SetObservation(
          &identity,
          {{"db", "pg_temp_17", "owned", "12345", "3", "amount", oid, raw, formatted}},
          9);
      std::vector<RedshiftIntervalCatalogColumn> columns;
      ASSERT_TRUE(ParseIntervalCatalogIdentity(identity, table, &columns).ok());
      PqResultHelper detail(nullptr, "");
      SetObservation(&detail,
                     {{"3", "amount", variant == 7 ? "SECOND" : nullptr,
                       variant == 8 ? "6" : nullptr, variant == 9 ? "3" : nullptr}},
                     5);
      auto selected = table;
      ASSERT_TRUE(ApplyIntervalCatalogObservation(detail, columns, &selected).ok());
      const auto c = selected.child(0).interval_constraint();
      if (variant == 0) {
        EXPECT_EQ(c.state, State::kKnown);
        EXPECT_EQ(c.qualifier, RedshiftIntervalConstraint::Qualifier(item.qualifier));
        EXPECT_EQ(c.precision, item.precision < 0
                                   ? std::nullopt
                                   : std::optional<int32_t>{item.precision});
        EXPECT_EQ(c.RequireKnown(nullptr), 0);
      } else {
        EXPECT_EQ(c.state, State::kUnknown);
        EXPECT_NE(c.RequireKnown(nullptr), 0);
      }
      // Target qualification is local to this observation, never cached in OIDs.
      PostgresType cached;
      ASSERT_EQ(resolver->Find(type.oid(), &cached, nullptr), 0);
      EXPECT_EQ(cached.interval_constraint().state, State::kUnknown);
    }
  }
}

TEST(RedshiftIntervalCatalogTest, TargetMismatchAndMalformedIdentityDoNotPublish) {
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::FixtureResolver(BackendProfile::Redshift(), &resolver), 0);
  PostgresType type;
  ASSERT_EQ(resolver->Find(testing::kDaySecondOid, &type, nullptr), 0);
  PostgresType table(PostgresTypeId::kRecord);
  table.AppendChild("amount",
                    type.WithFieldDescriptor(268435463, BackendProfile::Redshift()));
  const std::vector<std::vector<const char*>> row = {{"db", "public", "owned", "12345",
                                                      "1", "amount", "65017", "268435463",
                                                      "interval second (3)"}};
  for (int column : {3, 4, 6, 7}) {
    for (const char* malformed : {"", "1garbage", "9223372036854775808"}) {
      auto changed = row;
      changed[0][column] = malformed;
      PqResultHelper identity(nullptr, "");
      SetObservation(&identity, changed, 9);
      std::vector<RedshiftIntervalCatalogColumn> columns;
      EXPECT_FALSE(ParseIntervalCatalogIdentity(identity, table, &columns).ok());
      EXPECT_TRUE(columns.empty());
    }
  }
  PqResultHelper identity(nullptr, "");
  SetObservation(&identity, row, 9);
  std::vector<RedshiftIntervalCatalogColumn> columns;
  ASSERT_TRUE(ParseIntervalCatalogIdentity(identity, table, &columns).ok());
  PqResultHelper detail(nullptr, "");
  SetObservation(&detail, {{"1", "amount", nullptr, nullptr, nullptr}}, 5);
  PostgresType selected(PostgresTypeId::kRecord);
  selected.AppendChild("amount",
                       type.WithFieldDescriptor(268435464, BackendProfile::Redshift()));
  EXPECT_FALSE(ApplyIntervalCatalogObservation(detail, columns, &selected).ok());
  EXPECT_EQ(selected.child(0).interval_constraint().state, State::kUnknown);
}

TEST(RedshiftIntervalCatalogTest, ObservationNeverPromotesPlausibleCatalogToKnown) {
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::FixtureResolver(BackendProfile::Redshift(), &resolver), 0);
  PostgresType target;
  ASSERT_EQ(resolver->Find(testing::kDaySecondOid, &target, nullptr), 0);
  target = target.WithFieldDescriptor(-1, BackendProfile::Redshift());
  PostgresType table(PostgresTypeId::kRecord);
  table.AppendChild("amount", target);
  PqResultHelper before(nullptr, "");
  const std::vector<std::vector<const char*>> identity = {
      {"fixturedb", "pg_temp_17", "owned", "12345", "3", "amount", "65017", "-1",
       "interval day to second"}};
  SetObservation(&before, identity, 9);
  std::vector<RedshiftIntervalCatalogColumn> columns;
  ASSERT_TRUE(ParseIntervalCatalogIdentity(before, table, &columns).ok());
  for (const char* qualifier : {"DAY TO SECOND", "SECOND", "unrecognized", ""}) {
    PqResultHelper detail(nullptr, "");
    SetObservation(&detail, {{"3", "amount", qualifier, "6", "3"}}, 5);
    auto selected = table;
    ASSERT_TRUE(ApplyIntervalCatalogObservation(detail, columns, &selected).ok());
    auto c = selected.child(0).interval_constraint();
    EXPECT_EQ(c.state, RedshiftIntervalState::kUnknown);
    EXPECT_EQ(c.qualifier, RedshiftIntervalQualifier::kNone);
    EXPECT_FALSE(c.precision);
    EXPECT_EQ(c.raw_type, qualifier);
    EXPECT_EQ(c.raw_precision, "6");
    EXPECT_EQ(c.raw_datetime_precision, "3");
    EXPECT_NE(c.RequireKnown(nullptr), 0);
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(selected.child(0).SetSchema(schema.get(), "Redshift"), 0);
    RedshiftIntervalSchema parsed;
    EXPECT_EQ(RedshiftIntervalSchema::Parse(schema.get(), true, &parsed, nullptr, true),
              0);
    EXPECT_EQ(parsed.constraint.state, RedshiftIntervalState::kUnknown);
    EXPECT_NE(RedshiftIntervalSchema::Parse(schema.get(), true, &parsed, nullptr), 0);
  }
  for (auto rows : {std::vector<std::vector<const char*>>{},
                    {{"3", "amount", nullptr, nullptr, nullptr},
                     {"3", "amount", nullptr, nullptr, nullptr}},
                    {{"2", "amount", nullptr, nullptr, nullptr}},
                    {{"3", "other", nullptr, nullptr, nullptr}},
                    {{nullptr, "amount", nullptr, nullptr, nullptr}}}) {
    PqResultHelper detail(nullptr, "");
    SetObservation(&detail, rows, 5);
    auto selected = table;
    EXPECT_FALSE(ApplyIntervalCatalogObservation(detail, columns, &selected).ok());
    EXPECT_EQ(selected.child(0).interval_constraint().raw_type, std::nullopt);
  }
  for (int col : {3, 4, 5, 6, 7}) {
    auto changed = identity;
    changed[0][col] = col == 5 ? "other" : col == 7 ? "999" : "0";
    PqResultHelper result(nullptr, "");
    SetObservation(&result, changed, 9);
    EXPECT_FALSE(ParseIntervalCatalogIdentity(result, table, &columns).ok());
  }
}
}  // namespace
}  // namespace adbcpq
