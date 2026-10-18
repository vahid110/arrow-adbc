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
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "copy/reader.h"
#include "copy/writer.h"
#include "redshift_temporal_test_fixture.h"

namespace adbcpq {
namespace {
using adbc::driver::pgwire::BackendProfile;
using adbc::driver::pgwire::TemporalRepresentation;
class TemporalWriteCase {
 public:
  ArrowErrorCode Init(ArrowType storage, RedshiftTemporalKind kind,
                      const std::vector<std::optional<int64_t>>& values,
                      ArrowTimeUnit unit = NANOARROW_TIME_UNIT_MICRO,
                      bool extension = false, const char* p = nullptr, int target = -1,
                      std::optional<int32_t> target_raw = -1, bool redshift = true) {
    NANOARROW_RETURN_NOT_OK(
        testing::TemporalSchema(schema.get(), storage, kind, unit, extension, p));
    NANOARROW_RETURN_NOT_OK(
        testing::TemporalBatch(schema.get(), array.get(), values, &error));
    NANOARROW_RETURN_NOT_OK(
        ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error));
    NANOARROW_RETURN_NOT_OK(ArrowArrayViewSetArray(view.get(), array.get(), &error));
    const auto profile =
        redshift ? BackendProfile::Redshift() : BackendProfile::PostgreSQL();
    std::shared_ptr<PostgresTypeResolver> resolver;
    NANOARROW_RETURN_NOT_OK(testing::TemporalResolver(profile, &resolver));
    PostgresType destination;
    if (target >= 0) {
      NANOARROW_RETURN_NOT_OK(
          resolver->Find(testing::kTemporalOids[target], &destination, &error));
      destination = destination.WithFieldDescriptor(target_raw, profile);
    }
    return MakeCopyFieldWriter(schema->children[0], view->children[0], *resolver,
                               target < 0 ? nullptr : &destination, &writer, false,
                               &error);
  }
  ArrowErrorCode Write(int row = 0) {
    auto size = bytes->size_bytes;
    auto code = writer->Write(bytes.get(), row, &error);
    if (code) EXPECT_EQ(size, bytes->size_bytes);
    return code;
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
class TemporalReadCase {
 public:
  ArrowErrorCode Init(const PostgresType& type) {
    ArrowSchemaInit(schema.get());
    NANOARROW_RETURN_NOT_OK(type.SetSchema(schema.get(), "Redshift"));
    NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromSchema(array.get(), schema.get(), &error));
    NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(array.get()));
    NANOARROW_RETURN_NOT_OK(MakeCopyFieldReader(type, schema.get(), &reader, &error));
    reader->Init(type);
    NANOARROW_RETURN_NOT_OK(reader->InitSchema(schema.get()));
    return reader->InitArray(array.get());
  }
  ArrowErrorCode Read(const char* hex, int width = -99) {
    auto bytes = testing::NumericHex(hex);
    ArrowBufferView input{};
    input.data.data = bytes.data();
    input.size_bytes = bytes.size();
    auto size = input.size_bytes;
    auto length = array->length;
    auto code =
        reader->Read(&input, width == -99 ? static_cast<int32_t>(bytes.size()) : width,
                     array.get(), &error);
    if (code) {
      EXPECT_EQ(input.size_bytes, size);
      EXPECT_EQ(array->length, length);
    }
    return code;
  }
  int64_t Value() {
    EXPECT_EQ(reader->FinishArray(array.get(), &error), 0);
    EXPECT_EQ(ArrowArrayFinishBuildingDefault(array.get(), &error), 0);
    nanoarrow::UniqueArrayView view;
    EXPECT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error), 0);
    EXPECT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), &error), 0);
    return ArrowArrayViewGetIntUnsafe(view.get(), 0);
  }
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
  std::unique_ptr<PostgresCopyFieldReader> reader;
  ArrowError error{};
};
TEST(RedshiftTemporalTest, TwoFieldContextsDecodeOnOneUnchangedResolverInBothOrders) {
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::TemporalResolver(BackendProfile::Redshift(), &resolver), 0);
  for (auto mode : {TemporalRepresentation::kLossless, TemporalRepresentation::kArrow,
                    TemporalRepresentation::kLossless}) {
    PostgresType cached;
    ASSERT_EQ(resolver->Find(testing::kTemporalOids[2], &cached, nullptr), 0);
    EXPECT_FALSE(cached.is_redshift_descriptor());
    auto field = cached.WithFieldDescriptor(-1, BackendProfile::Redshift(), mode);
    TemporalReadCase read;
    ASSERT_EQ(read.Init(field), 0);
    ASSERT_EQ(read.Read("0000000000000000"), 0);
    EXPECT_EQ(read.Value(),
              mode == TemporalRepresentation::kLossless ? 0 : 946684800000000);
    ArrowSchemaView view{};
    ASSERT_EQ(ArrowSchemaViewInit(&view, read.schema.get(), nullptr), 0);
    EXPECT_EQ(view.type, mode == TemporalRepresentation::kLossless
                             ? NANOARROW_TYPE_INT64
                             : NANOARROW_TYPE_TIMESTAMP);
    PostgresType again;
    ASSERT_EQ(resolver->Find(testing::kTemporalOids[2], &again, nullptr), 0);
    EXPECT_FALSE(again.is_redshift_descriptor());
    EXPECT_FALSE(again.type_modifier());
  }
}
TEST(RedshiftTemporalTest, LiteralDateEpochAndWholeDayDate64Writer) {
  TemporalWriteCase date;
  ASSERT_EQ(date.Init(NANOARROW_TYPE_DATE32, RedshiftTemporalKind::kDate, {10957}), 0);
  ASSERT_EQ(date.Write(), 0);
  EXPECT_EQ(date.Wire(), testing::NumericHex("0000000400000000"));
  for (int64_t value : {int64_t{-86400000}, int64_t{86400000}}) {
    TemporalWriteCase day;
    ASSERT_EQ(day.Init(NANOARROW_TYPE_DATE64, RedshiftTemporalKind::kDate, {value}), 0);
    ASSERT_EQ(day.Write(), 0);
    EXPECT_EQ(day.Wire(),
              testing::NumericHex(value < 0 ? "00000004ffffd532" : "00000004ffffd534"));
  }
  TemporalWriteCase fraction;
  ASSERT_EQ(fraction.Init(NANOARROW_TYPE_DATE64, RedshiftTemporalKind::kDate, {-1}), 0);
  EXPECT_NE(fraction.Write(), 0);
  PostgresType date_type(PostgresTypeId::kDate);
  date_type = date_type.WithPgTypeInfo(76001, "date")
                  .WithFieldDescriptor(-1, BackendProfile::Redshift());
  TemporalReadCase read;
  ASSERT_EQ(read.Init(date_type), 0);
  ASSERT_EQ(read.Read("ffffd533"), 0);
  EXPECT_EQ(read.Value(), 0);
}
TEST(RedshiftTemporalTest, TimeAllUnitsAndLosslessEndofdayHaveDistinctDomains) {
  struct Case {
    ArrowType type;
    ArrowTimeUnit unit;
    int64_t value;
    int64_t end;
  };
  const Case cases[] = {
      {NANOARROW_TYPE_TIME32, NANOARROW_TIME_UNIT_SECOND, 86399, 86400},
      {NANOARROW_TYPE_TIME32, NANOARROW_TIME_UNIT_MILLI, 86399999, 86400000},
      {NANOARROW_TYPE_TIME64, NANOARROW_TIME_UNIT_MICRO, 86399999999, 86400000000},
      {NANOARROW_TYPE_TIME64, NANOARROW_TIME_UNIT_NANO, 86399999999000, 86400000000000}};
  for (const auto& item : cases) {
    TemporalWriteCase valid;
    ASSERT_EQ(valid.Init(item.type, RedshiftTemporalKind::kTime, {item.value}, item.unit),
              0);
    EXPECT_EQ(valid.Write(), 0);
    TemporalWriteCase end;
    ASSERT_EQ(end.Init(item.type, RedshiftTemporalKind::kTime, {item.end}, item.unit), 0);
    EXPECT_NE(end.Write(), 0);
  }
  TemporalWriteCase lossless;
  ASSERT_EQ(lossless.Init(NANOARROW_TYPE_INT64, RedshiftTemporalKind::kTime,
                          {86400000000}, NANOARROW_TIME_UNIT_MICRO, true),
            0);
  ASSERT_EQ(lossless.Write(), 0);
  EXPECT_EQ(lossless.Wire(), testing::NumericHex("00000008000000141dd76000"));
  TemporalWriteCase fraction;
  ASSERT_EQ(fraction.Init(NANOARROW_TYPE_TIME64, RedshiftTemporalKind::kTime, {1001},
                          NANOARROW_TIME_UNIT_NANO),
            0);
  EXPECT_NE(fraction.Write(), 0);
}
TEST(RedshiftTemporalTest,
     ReferenceCountsCheckEpochOverflowAndReservedCodesWithoutSqlDomainClaim) {
  for (auto kind :
       {RedshiftTemporalKind::kTimestamp, RedshiftTemporalKind::kTimestamptz}) {
    for (int64_t value : {INT64_MIN + 1, INT64_MAX - 1}) {
      TemporalWriteCase exact;
      ASSERT_EQ(exact.Init(NANOARROW_TYPE_INT64, kind, {value}, NANOARROW_TIME_UNIT_MICRO,
                           true),
                0);
      EXPECT_EQ(exact.Write(), 0);  // Reference-storage evidence, not SQL validity.
    }
    for (int64_t value : {INT64_MIN, INT64_MAX}) {
      TemporalWriteCase reserved;
      ASSERT_EQ(reserved.Init(NANOARROW_TYPE_INT64, kind, {value},
                              NANOARROW_TIME_UNIT_MICRO, true),
                0);
      EXPECT_NE(reserved.Write(), 0);
    }
    PostgresType type(kind == RedshiftTemporalKind::kTimestamp
                          ? PostgresTypeId::kTimestamp
                          : PostgresTypeId::kTimestamptz);
    type = type.WithPgTypeInfo(
        1, kind == RedshiftTemporalKind::kTimestamp ? "timestamp" : "timestamptz");
    TemporalReadCase bounded;
    ASSERT_EQ(bounded.Init(type.WithFieldDescriptor(-1, BackendProfile::Redshift())), 0);
    EXPECT_NE(bounded.Read("7ffffffffffffffe"), 0);
    TemporalReadCase lossless;
    ASSERT_EQ(lossless.Init(type.WithFieldDescriptor(-1, BackendProfile::Redshift(),
                                                     TemporalRepresentation::kLossless)),
              0);
    ASSERT_EQ(lossless.Read("7ffffffffffffffe"), 0);
    EXPECT_EQ(lossless.Value(), INT64_MAX - 1);
    TemporalReadCase reserved;
    ASSERT_EQ(reserved.Init(type.WithFieldDescriptor(-1, BackendProfile::Redshift(),
                                                     TemporalRepresentation::kLossless)),
              0);
    EXPECT_NE(reserved.Read("8000000000000000"), 0);
    EXPECT_NE(reserved.Read("7fffffffffffffff"), 0);
    EXPECT_NE(reserved.Read("0000"), 0);
    EXPECT_NE(reserved.Read("0000000000000000", -2), 0);
  }
}
TEST(RedshiftTemporalTest,
     TimestampEpochCancellationChecksFinalNativeBoundsBeforeMultiplication) {
  struct Case {
    ArrowTimeUnit unit;
    int64_t source;
    const char* wire;
  };
  // Independent Python-integer n=source*factor-946684800000000, encoded as
  // literal signed network bytes. Reference counts, never native SQL endpoints.
  const Case cases[] = {
      {NANOARROW_TIME_UNIT_SECOND, -9222425352054LL, "0000000880000000000bd680"},
      {NANOARROW_TIME_UNIT_SECOND, 9224318721654LL, "000000087ffffffffff42980"},
      {NANOARROW_TIME_UNIT_SECOND, 9224318015999LL, "000000087fffff5bb3a35dc0"},
      {NANOARROW_TIME_UNIT_MILLI, -9222425352054775LL, "000000088000000000000328"},
      {NANOARROW_TIME_UNIT_MILLI, 9224318721654775LL, "000000087ffffffffffffcd8"},
      {NANOARROW_TIME_UNIT_MILLI, 9224318015999999LL, "000000087fffff5bb3b29c18"}};
  for (auto kind :
       {RedshiftTemporalKind::kTimestamp, RedshiftTemporalKind::kTimestamptz}) {
    for (const auto& item : cases) {
      TemporalWriteCase test;
      ASSERT_EQ(test.Init(NANOARROW_TYPE_TIMESTAMP, kind, {item.source}, item.unit), 0);
      ASSERT_EQ(test.Write(), 0);
      EXPECT_EQ(test.Wire(), testing::NumericHex(item.wire));
    }
    for (const auto& item : std::vector<std::pair<ArrowTimeUnit, int64_t>>{
             {NANOARROW_TIME_UNIT_SECOND, -9222425352055LL},
             {NANOARROW_TIME_UNIT_SECOND, 9224318721655LL},
             {NANOARROW_TIME_UNIT_MILLI, -9222425352054776LL},
             {NANOARROW_TIME_UNIT_MILLI, 9224318721654776LL}}) {
      TemporalWriteCase test;
      ASSERT_EQ(test.Init(NANOARROW_TYPE_TIMESTAMP, kind, {item.second}, item.first), 0);
      EXPECT_NE(test.Write(), 0);
    }
  }
}

TEST(RedshiftTemporalTest,
     KnownReadPrecisionRejectsBeforeCursorAndUnknownModifierStaysUnknown) {
  for (int col : {1, 2, 3}) {
    const auto id = col == 1   ? PostgresTypeId::kTime
                    : col == 2 ? PostgresTypeId::kTimestamp
                               : PostgresTypeId::kTimestamptz;
    auto type = PostgresType(id).WithPgTypeInfo(testing::kTemporalOids[col],
                                                testing::kTemporalNames[col]);
    for (int p : {0, 3}) {
      TemporalReadCase read;
      ASSERT_EQ(read.Init(type.WithFieldDescriptor(p, BackendProfile::Redshift())), 0);
      EXPECT_NE(read.Read(p == 0 ? "000000000016e360" : "00000000000004d2"), 0);
    }
    for (std::optional<int32_t> raw :
         {std::optional<int32_t>{}, std::optional<int32_t>{999}}) {
      TemporalReadCase read;
      ASSERT_EQ(read.Init(type.WithFieldDescriptor(raw, BackendProfile::Redshift())), 0);
      ASSERT_EQ(read.Read("00000000000004d2"), 0);
      EXPECT_EQ(read.Value(), col == 1 ? 1234 : 946684800001234);
      std::optional<std::string_view> p;
      ASSERT_EQ(
          RedshiftDecimalSchema::Metadata(
              read.schema.get(), "ADBC:redshift:fractional_precision", &p, &read.error),
          0);
      EXPECT_FALSE(p);
    }
  }
}

TEST(RedshiftTemporalTest, SourceAndActualTargetPrecisionAreIndependentlyExact) {
  TemporalWriteCase source;
  ASSERT_EQ(source.Init(NANOARROW_TYPE_TIMESTAMP, RedshiftTemporalKind::kTimestamp,
                        {1001}, NANOARROW_TIME_UNIT_MICRO, false, "3", 2, 6),
            0);
  EXPECT_NE(source.Write(), 0);
  TemporalWriteCase target;
  ASSERT_EQ(target.Init(NANOARROW_TYPE_TIMESTAMP, RedshiftTemporalKind::kTimestamp,
                        {1001}, NANOARROW_TIME_UNIT_MICRO, false, "6", 2, 3),
            0);
  EXPECT_NE(target.Write(), 0);
  TemporalWriteCase exact;
  ASSERT_EQ(exact.Init(NANOARROW_TYPE_TIMESTAMP, RedshiftTemporalKind::kTimestamp, {1000},
                       NANOARROW_TIME_UNIT_MICRO, false, "3", 2, 3),
            0);
  EXPECT_EQ(exact.Write(), 0);
  std::string declaration;
  RedshiftTemporalSchema parsed;
  ASSERT_EQ(RedshiftTemporalSchema::Parse(exact.schema->children[0], true, &parsed,
                                          &exact.error),
            0);
  EXPECT_NE(parsed.Declaration(&declaration, &exact.error), 0);
  TemporalWriteCase unknown;
  EXPECT_NE(unknown.Init(NANOARROW_TYPE_TIMESTAMP, RedshiftTemporalKind::kTimestamp, {0},
                         NANOARROW_TIME_UNIT_MICRO, false, nullptr, 2, std::nullopt),
            0);
  TemporalWriteCase unsupported;
  EXPECT_NE(unsupported.Init(NANOARROW_TYPE_TIMESTAMP, RedshiftTemporalKind::kTimestamp,
                             {0}, NANOARROW_TIME_UNIT_MICRO, false, nullptr, 2, 7),
            0);
  TemporalWriteCase crossed;
  EXPECT_NE(crossed.Init(NANOARROW_TYPE_TIMESTAMP, RedshiftTemporalKind::kTimestamp, {0},
                         NANOARROW_TIME_UNIT_MICRO, false, nullptr, 3, 6),
            0);
}
TEST(RedshiftTemporalTest,
     ClosedExtensionsAndMissingMarkersCannotBecomeTemporalBigintFallback) {
  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(testing::TemporalSchema(schema.get(), NANOARROW_TYPE_INT64,
                                    RedshiftTemporalKind::kTime,
                                    NANOARROW_TIME_UNIT_MICRO, true),
            0);
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::TemporalResolver(BackendProfile::PostgreSQL(), &resolver), 0);
  ArrowError error{};
  PostgresType type;
  ASSERT_EQ(PostgresType::FromSchema(*resolver, schema->children[0], &type, &error), 0);
  EXPECT_EQ(type.oid(), testing::kTemporalOids[1]);
  ASSERT_EQ(testing::DecimalMetadata(schema->children[0], "ARROW:extension:name",
                                     "adbc.redshift.time_us.v2"),
            0);
  EXPECT_NE(PostgresType::FromSchema(*resolver, schema->children[0], &type, &error), 0);
  nanoarrow::UniqueSchema stripped;
  ASSERT_EQ(testing::TemporalSchema(stripped.get(), NANOARROW_TYPE_INT64,
                                    RedshiftTemporalKind::kTime,
                                    NANOARROW_TIME_UNIT_MICRO, false, nullptr, false),
            0);
  ASSERT_EQ(PostgresType::FromSchema(*resolver, stripped->children[0], &type, &error), 0);
  EXPECT_EQ(type.type_id(), PostgresTypeId::kInt8);
  ASSERT_EQ(
      testing::DecimalMetadata(stripped->children[0], "ADBC:redshift:sql_type", "TIME"),
      0);
  EXPECT_NE(PostgresType::FromSchema(*resolver, stripped->children[0], &type, &error), 0);
}
}  // namespace
}  // namespace adbcpq
