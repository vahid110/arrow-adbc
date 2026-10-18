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

#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "bind_stream.h"
#include "driver/framework/utility.h"
#include "redshift_interval_test_fixture.h"

namespace adbcpq {
namespace {
using adbc::driver::pgwire::BackendProfile;

TEST(RedshiftIntervalTest, CatalogDiscoveryIsBackendSpecificAndUsesReturnedOids) {
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::FixtureResolver(BackendProfile::Redshift(), &resolver), 0);
  PostgresType type;
  ASSERT_EQ(resolver->Find(testing::kYearMonthOid, &type, nullptr), 0);
  EXPECT_EQ(type.type_id(), PostgresTypeId::kRedshiftIntervalYearMonth);
  EXPECT_EQ(type.oid(), testing::kYearMonthOid);
  ASSERT_EQ(resolver->Find(testing::kDaySecondOid, &type, nullptr), 0);
  EXPECT_EQ(type.type_id(), PostgresTypeId::kRedshiftIntervalDaySecond);
  EXPECT_EQ(type.oid(), testing::kDaySecondOid);
  ASSERT_EQ(resolver->FindWithDefault(79001, &type), 0);
  EXPECT_EQ(type.type_id(), PostgresTypeId::kUnnamedArrowOpaque);

  ASSERT_EQ(testing::FixtureResolver(BackendProfile::PostgreSQL(), &resolver), 0);
  ASSERT_EQ(resolver->Find(testing::kYearMonthOid, &type, nullptr), 0);
  EXPECT_EQ(type.type_id(), PostgresTypeId::kUserDefined);
  ASSERT_EQ(resolver->Find(testing::kDaySecondOid, &type, nullptr), 0);
  EXPECT_EQ(type.type_id(), PostgresTypeId::kUserDefined);
  ASSERT_EQ(resolver->Insert({17, "bytea", "bytearecv", 0, 0, 0}, nullptr), 0);
  nanoarrow::UniqueSchema unknown_schema;
  ArrowSchemaInit(unknown_schema.get());
  ASSERT_EQ(type.SetSchema(unknown_schema.get()), 0);
  PostgresType mapped;
  ASSERT_EQ(PostgresType::FromSchema(*resolver, unknown_schema.get(), &mapped, nullptr),
            0);
  EXPECT_EQ(mapped.type_id(), PostgresTypeId::kBytea);
}

TEST(RedshiftIntervalTest, MixedQueryAndTableColumnSchemasAgreeWithoutInventedModifiers) {
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
  ArrowError error{};
  ASSERT_EQ(testing::IntervalFixture(0, array.get(), schema.get(), &error), 0)
      << error.message;
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::FixtureResolver(BackendProfile::Redshift(), &resolver), 0);
  for (const auto& item :
       {std::pair{testing::kYearMonthOid, 0}, std::pair{testing::kDaySecondOid, 1}}) {
    PostgresType type;
    ASSERT_EQ(resolver->Find(item.first, &type, &error), 0);
    nanoarrow::UniqueSchema table_column;
    ArrowSchemaInit(table_column.get());
    // This is GetTableSchema's actual OID/name -> SetSchema operation; the
    // query fixture above uses PqResultHelper::ResolveOutputTypes first.
    ASSERT_EQ(type.WithFieldName(schema->children[item.second]->name)
                  .SetSchema(table_column.get(), "Redshift"),
              0);
    EXPECT_STREQ(table_column->format, schema->children[item.second]->format);
    ArrowStringView family{};
    ASSERT_EQ(
        ArrowMetadataGetValue(table_column->metadata,
                              ArrowCharView("ADBC:redshift:interval_family"), &family),
        0);
    EXPECT_EQ(std::string(family.data, family.size_bytes),
              item.second == 0 ? "intervaly2m" : "intervald2s");
    ArrowMetadataReader metadata;
    ASSERT_EQ(ArrowMetadataReaderInit(&metadata, table_column->metadata), 0);
    // Only semantic family + existing type-name metadata, no invented typmod,
    // qualifier, fractional precision, or canonical extension claim.
    EXPECT_EQ(metadata.remaining_keys, 2);
  }
  EXPECT_STREQ(schema->children[0]->format, "tin");
  EXPECT_STREQ(schema->children[1]->format, "tDu");
}

TEST(RedshiftIntervalTest, CodecProducesExactSignedValuesNullsAndMixedRows) {
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
  nanoarrow::UniqueArrayView view;
  ArrowError error{};
  ASSERT_EQ(testing::IntervalFixture(0, array.get(), schema.get(), &error), 0)
      << error.message;
  ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error), 0);
  ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), &error), 0);
  ASSERT_EQ(array->length, 6);
  const std::array<int32_t, 5> months = {0, 14, -14, std::numeric_limits<int32_t>::min(),
                                         std::numeric_limits<int32_t>::max()};
  const std::array<int64_t, 5> micros = {0, 1000001, -1000001,
                                         std::numeric_limits<int64_t>::min(),
                                         std::numeric_limits<int64_t>::max()};
  for (int row = 0; row < 5; row++) {
    ArrowInterval value;
    ArrowIntervalInit(&value, NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO);
    ArrowArrayViewGetIntervalUnsafe(view->children[0], row, &value);
    EXPECT_EQ(value.months, months[row]);
    EXPECT_EQ(value.days, 0);
    EXPECT_EQ(value.ns, 0);
    EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view->children[1], row), micros[row]);
  }
  EXPECT_TRUE(ArrowArrayViewIsNull(view->children[0], 5));
  EXPECT_TRUE(ArrowArrayViewIsNull(view->children[1], 5));
  for (int row = 0; row < 6; row++) {
    EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view->children[2], row), 42);
  }
}

TEST(RedshiftIntervalTest, EmptyResultStillPreservesFamilySchemas) {
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
  ArrowError error{};
  ASSERT_EQ(testing::IntervalFixture(1, array.get(), schema.get(), &error), 0)
      << error.message;
  EXPECT_EQ(array->length, 0);
  EXPECT_EQ(array->children[0]->length, 0);
  EXPECT_STREQ(schema->children[0]->format, "tin");
  EXPECT_STREQ(schema->children[1]->format, "tDu");
}

TEST(RedshiftIntervalTest, StrictWidthsAndUnalignedNetworkBuffers) {
  for (const auto type_id : {PostgresTypeId::kRedshiftIntervalYearMonth,
                             PostgresTypeId::kRedshiftIntervalDaySecond}) {
    PostgresType type(type_id);
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(type.SetSchema(schema.get(), "Redshift"), 0);
    nanoarrow::UniqueArray array;
    ArrowError error{};
    ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), &error), 0);
    ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
    std::unique_ptr<PostgresCopyFieldReader> reader;
    ASSERT_EQ(MakeCopyFieldReader(type, schema.get(), &reader, &error), 0);
    reader->Init(type);
    ASSERT_EQ(reader->InitSchema(schema.get()), 0);
    ASSERT_EQ(reader->InitArray(array.get()), 0);
    const int width = type_id == PostgresTypeId::kRedshiftIntervalYearMonth ? 4 : 8;
    // Explicit unaligned pointer, -2 in signed network order at both widths.
    alignas(8) const uint8_t bytes[17] = {0, 255, 255, 255, 254, 255, 255, 255, 254,
                                          0, 0,   0,   0,   0,   0,   0,   0};
    alignas(8) std::array<uint8_t, 17> unaligned{};
    unaligned.fill(255);
    unaligned[width] = 254;
    ArrowBufferView data{};
    data.data.data = unaligned.data() + 1;
    data.size_bytes = width;
    ASSERT_EQ(reader->Read(&data, width, array.get(), &error), 0);
    EXPECT_EQ(data.size_bytes, 0);
    for (int invalid : {0, 1, 3, 4, 5, 7, 8, 9, 16}) {
      if (invalid == width) continue;
      data.data.data = bytes + 1;
      data.size_bytes = invalid;
      EXPECT_EQ(reader->Read(&data, invalid, array.get(), &error), EINVAL);
      EXPECT_EQ(array->length, 1);
    }
    ASSERT_EQ(reader->FinishArray(array.get(), &error), 0);
    ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), &error), 0);
    if (width == 4) {
      int32_t value;
      std::memcpy(&value, array->buffers[1], sizeof(value));
      EXPECT_EQ(value, -2);
    } else {
      int64_t value;
      std::memcpy(&value, array->buffers[1], sizeof(value));
      EXPECT_EQ(value, -2);
    }
  }
}

TEST(RedshiftIntervalTest, RejectsIncompatibleReaderStorageUnitsAndFamilies) {
  PostgresType day_second(PostgresTypeId::kRedshiftIntervalDaySecond);
  nanoarrow::UniqueSchema schema;
  ArrowSchemaInit(schema.get());
  ArrowError error{};
  std::unique_ptr<PostgresCopyFieldReader> reader;
  ASSERT_EQ(ArrowSchemaSetTypeDateTime(schema.get(), NANOARROW_TYPE_DURATION,
                                       NANOARROW_TIME_UNIT_NANO, nullptr),
            0);
  EXPECT_NE(MakeCopyFieldReader(day_second, schema.get(), &reader, &error), 0);
  ASSERT_EQ(ArrowSchemaSetType(schema.get(), NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO), 0);
  EXPECT_NE(MakeCopyFieldReader(day_second, schema.get(), &reader, &error), 0);
  PostgresType generic(PostgresTypeId::kInterval);
  ASSERT_EQ(ArrowSchemaSetTypeDateTime(schema.get(), NANOARROW_TYPE_DURATION,
                                       NANOARROW_TIME_UNIT_MICRO, nullptr),
            0);
  EXPECT_NE(MakeCopyFieldReader(generic, schema.get(), &reader, &error), 0);
}

TEST(RedshiftIntervalTest, ReadbackFamilyCannotFallThroughToGenericWriter) {
  for (const auto profile : {BackendProfile::Redshift(), BackendProfile::PostgreSQL()}) {
    PostgresTypeResolver resolver(profile);
    ASSERT_EQ(resolver.Insert({1186, "interval", "interval_recv", 0, 0, 0}, nullptr), 0);
    for (const auto id : {PostgresTypeId::kRedshiftIntervalYearMonth,
                          PostgresTypeId::kRedshiftIntervalDaySecond}) {
      PostgresType read_type(id);
      nanoarrow::UniqueSchema schema;
      ArrowSchemaInit(schema.get());
      ASSERT_EQ(read_type.SetSchema(schema.get(), "Redshift"), 0);
      ArrowError error{};
      PostgresType write_type;
      EXPECT_EQ(PostgresType::FromSchema(resolver, schema.get(), &write_type, &error),
                ENOTSUP);
      // Once all metadata is removed only the Redshift backend rejects storage
      // fallback; ordinary PostgreSQL interval input remains supported.
      ASSERT_EQ(ArrowSchemaSetMetadata(schema.get(), nullptr), 0);
      const int expected = resolver.is_redshift() ? ENOTSUP : NANOARROW_OK;
      EXPECT_EQ(PostgresType::FromSchema(resolver, schema.get(), &write_type, &error),
                expected);
      if (!resolver.is_redshift())
        EXPECT_EQ(write_type.type_id(), PostgresTypeId::kInterval);
    }
  }
}

TEST(RedshiftIntervalTest,
     UnknownFamilyMarkerIsReadOnlyAndOrdinaryRedshiftIntStillBinds) {
  PostgresTypeResolver resolver(BackendProfile::Redshift());
  ASSERT_EQ(resolver.Insert({29, "int8", "int8recv", 0, 0, 0}, nullptr), 0);
  nanoarrow::UniqueSchema schema;
  ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_INT64);
  ArrowError error{};
  PostgresType type;
  ASSERT_EQ(PostgresType::FromSchema(resolver, schema.get(), &type, &error), 0);
  EXPECT_EQ(type.type_id(), PostgresTypeId::kInt8);
  nanoarrow::UniqueBuffer metadata;
  ASSERT_EQ(ArrowMetadataBuilderInit(metadata.get(), nullptr), 0);
  ASSERT_EQ(ArrowMetadataBuilderAppend(metadata.get(),
                                       ArrowCharView("ADBC:redshift:interval_family"),
                                       ArrowCharView("future_unknown")),
            0);
  ASSERT_EQ(
      ArrowSchemaSetMetadata(schema.get(), reinterpret_cast<const char*>(metadata->data)),
      0);
  EXPECT_EQ(PostgresType::FromSchema(resolver, schema.get(), &type, &error), ENOTSUP);
}

TEST(RedshiftIntervalTest, NestedAndDictionaryIntervalsAreReadOnly) {
  PostgresTypeResolver resolver(BackendProfile::Redshift());
  for (bool dictionary : {false, true}) {
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInitFromType(schema.get(),
                            dictionary ? NANOARROW_TYPE_INT32 : NANOARROW_TYPE_LIST);
    ArrowSchema* child;
    if (dictionary) {
      ASSERT_EQ(ArrowSchemaAllocateDictionary(schema.get()), 0);
      child = schema->dictionary;
      ArrowSchemaInit(child);
    } else {
      child = schema->children[0];
    }
    ASSERT_EQ(ArrowSchemaSetTypeDateTime(child, NANOARROW_TYPE_DURATION,
                                         NANOARROW_TIME_UNIT_MICRO, nullptr),
              0);
    ArrowError error{};
    EXPECT_EQ(PostgresType::ValidateIntervalWriteSchema(resolver, schema.get(), &error),
              ENOTSUP);
  }
}

TEST(RedshiftIntervalTest, BindPreflightRejectsBeforeAnySqlWithNullConnection) {
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
  ArrowError error{};
  ASSERT_EQ(testing::IntervalFixture(0, array.get(), schema.get(), &error), 0);
  nanoarrow::UniqueArrayStream stream;
  adbc::driver::MakeArrayStream(schema.get(), array.get(), stream.get());
  BindStream bind;
  bind.SetBind(stream.get());
  ASSERT_TRUE(bind.Begin([] { return Status::Ok(); }).ok());
  PostgresTypeResolver resolver(BackendProfile::Redshift());
  auto status = bind.SetParamTypes(nullptr, resolver, true);
  EXPECT_EQ(status.ToAdbc(nullptr), ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_FALSE(bind.has_tz_field);
}

TEST(RedshiftIntervalTest, OrdinaryRedshiftIntegerBindStillBuildsItsWriterWithoutSql) {
  PostgresTypeResolver resolver(BackendProfile::Redshift());
  ASSERT_EQ(resolver.Insert({87, "int8", "int8recv", 0, 0, 0}, nullptr), 0);
  PostgresType root(PostgresTypeId::kRecord);
  root.AppendChild("value", PostgresType(PostgresTypeId::kInt8));
  nanoarrow::UniqueSchema schema;
  ArrowSchemaInit(schema.get());
  ASSERT_EQ(root.SetSchema(schema.get(), "Redshift"), 0);
  nanoarrow::UniqueArrayStream stream;
  nanoarrow::EmptyArrayStream(schema.get()).ToArrayStream(stream.get());
  BindStream bind;
  bind.SetBind(stream.get());
  ASSERT_TRUE(bind.Begin([] { return Status::Ok(); }).ok());
  EXPECT_TRUE(bind.SetParamTypes(nullptr, resolver, true).ok());
  ASSERT_EQ(bind.param_types.size(), 1U);
  EXPECT_EQ(bind.param_types[0], 87U);
}

}  // namespace
}  // namespace adbcpq
