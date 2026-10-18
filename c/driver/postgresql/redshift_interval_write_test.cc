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
    if (result) EXPECT_EQ(before, bytes->size_bytes);
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
          exact.Init(Family::kDaySecond, {quanta[strictest]}, "known", "DAY TO SECOND",
                     p.c_str(), NANOARROW_TIME_UNIT_MICRO, &target),
          0);
      ASSERT_EQ(exact.Write(), 0);
      EXPECT_EQ(exact.Wire(), testing::NumericHex(wires[strictest]));
      if (quanta[strictest] > 1) {
        IntervalWriteCase invalid;
        ASSERT_EQ(
            invalid.Init(Family::kDaySecond, {quanta[strictest] - 1}, "known",
                         "DAY TO SECOND", p.c_str(), NANOARROW_TIME_UNIT_MICRO, &target),
            0);
        EXPECT_NE(invalid.Write(), 0);
      }
    }
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
        if (known) EXPECT_EQ(bind.param_types[0], type.oid());
      }
}
}  // namespace
}  // namespace adbcpq
