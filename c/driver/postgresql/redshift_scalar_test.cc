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

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bind_stream.h"
#include "copy/reader.h"
#include "copy/writer.h"
#include "driver/framework/utility.h"
#include "redshift_numeric_test_fixture.h"
#include "redshift_scalar_test_fixture.h"

namespace adbcpq {
namespace {
using adbc::driver::pgwire::BackendProfile;

class ScalarWriteCase {
 public:
  template <typename T>
  ArrowErrorCode Init(ArrowType storage, const char* marker,
                      const std::vector<std::optional<T>>& values,
                      RedshiftScalarKind target = RedshiftScalarKind::kNone,
                      bool redshift = true) {
    NANOARROW_RETURN_NOT_OK(testing::ScalarSchema(schema.get(), storage, marker));
    NANOARROW_RETURN_NOT_OK(
        testing::ScalarBatch(schema.get(), array.get(), values, &error));
    NANOARROW_RETURN_NOT_OK(
        ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error));
    NANOARROW_RETURN_NOT_OK(ArrowArrayViewSetArray(view.get(), array.get(), &error));
    const auto profile =
        redshift ? BackendProfile::Redshift() : BackendProfile::PostgreSQL();
    std::shared_ptr<PostgresTypeResolver> resolver;
    NANOARROW_RETURN_NOT_OK(testing::ScalarResolver(profile, &resolver));
    PostgresType destination;
    if (target != RedshiftScalarKind::kNone) {
      for (int i = 0; i < 6; i++) {
        if (testing::kScalarKinds[i] == target) {
          NANOARROW_RETURN_NOT_OK(
              resolver->Find(testing::kScalarOids[i], &destination, &error));
          destination = destination.WithFieldDescriptor(-1, profile);
        }
      }
    }
    return MakeCopyFieldWriter(
        schema->children[0], view->children[0], *resolver,
        target == RedshiftScalarKind::kNone ? nullptr : &destination, &writer, false,
        &error);
  }
  ArrowErrorCode Write(int64_t row = 0) {
    const auto before = bytes->size_bytes;
    const auto result = writer->Write(bytes.get(), row, &error);
    if (result) {
      EXPECT_EQ(bytes->size_bytes, before);
    }
    return result;
  }
  std::string Wire() const {
    return std::string(reinterpret_cast<char*>(bytes->data), bytes->size_bytes);
  }
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
  nanoarrow::UniqueArrayView view;
  nanoarrow::UniqueBuffer bytes;
  std::unique_ptr<PostgresCopyFieldWriter> writer;
  ArrowError error{};
};

TEST(RedshiftScalarTest, SignedEndpointsAndTargetWidthsUseLiteralWire) {
  struct Case {
    int64_t value;
    RedshiftScalarKind target;
    const char* wire;
  };
  const Case cases[] = {
      {INT16_MIN, RedshiftScalarKind::kSmallint, "000000028000"},
      {INT16_MAX, RedshiftScalarKind::kSmallint, "000000027fff"},
      {INT32_MIN, RedshiftScalarKind::kInteger, "0000000480000000"},
      {INT32_MAX, RedshiftScalarKind::kInteger, "000000047fffffff"},
      {INT64_MIN, RedshiftScalarKind::kBigint, "000000088000000000000000"},
      {INT64_MAX, RedshiftScalarKind::kBigint, "000000087fffffffffffffff"}};
  for (const auto& item : cases) {
    ScalarWriteCase test;
    ASSERT_EQ(
        test.Init<int64_t>(NANOARROW_TYPE_INT64, nullptr, {item.value}, item.target), 0);
    ASSERT_EQ(test.Write(), 0) << test.error.message;
    EXPECT_EQ(test.Wire(), testing::NumericHex(item.wire));
  }
  ScalarWriteCase narrow;
  ASSERT_EQ(narrow.Init<uint64_t>(NANOARROW_TYPE_UINT64, nullptr, {42},
                                  RedshiftScalarKind::kSmallint),
            0);
  ASSERT_EQ(narrow.Write(), 0);
  EXPECT_EQ(narrow.Wire(), testing::NumericHex("00000002002a"));
}

TEST(RedshiftScalarTest, OverflowAndSourceIdentityRejectBeforeEmitting) {
  for (int64_t value : {int64_t{INT16_MIN} - 1, int64_t{INT16_MAX} + 1}) {
    ScalarWriteCase test;
    ASSERT_EQ(test.Init<int64_t>(NANOARROW_TYPE_INT64, nullptr, {value},
                                 RedshiftScalarKind::kSmallint),
              0);
    EXPECT_NE(test.Write(), 0);
  }
  for (int64_t value : {int64_t{INT32_MIN} - 1, int64_t{INT32_MAX} + 1}) {
    ScalarWriteCase test;
    ASSERT_EQ(test.Init<int64_t>(NANOARROW_TYPE_INT64, nullptr, {value},
                                 RedshiftScalarKind::kInteger),
              0);
    EXPECT_NE(test.Write(), 0);
  }
  for (uint64_t value : {uint64_t{1} << 63, UINT64_MAX}) {
    ScalarWriteCase test;
    ASSERT_EQ(test.Init<uint64_t>(NANOARROW_TYPE_UINT64, nullptr, {value}), 0);
    EXPECT_NE(test.Write(), 0);
  }
  ScalarWriteCase source;
  ASSERT_EQ(source.Init<int64_t>(NANOARROW_TYPE_INT64, "SMALLINT", {32768},
                                 RedshiftScalarKind::kBigint, false),
            0);
  EXPECT_NE(source.Write(), 0);
  ScalarWriteCase cross;
  EXPECT_NE(cross.Init<double>(NANOARROW_TYPE_DOUBLE, nullptr, {1.0},
                               RedshiftScalarKind::kBigint),
            0);
  ScalarWriteCase boolean;
  EXPECT_NE(boolean.Init<int64_t>(NANOARROW_TYPE_INT64, "BOOLEAN", {1}), 0);
}

TEST(RedshiftScalarTest, FloatWidthsExtremaSubnormalsAndSignedZeroUseLiteralWire) {
  struct Case {
    double value;
    const char* wire;
  };
  const Case cases[] = {
      {0.0, "0000000400000000"},
      {-0.0, "0000000480000000"},
      {1.25, "000000043fa00000"},
      {static_cast<double>((std::numeric_limits<float>::max)()), "000000047f7fffff"},
      {static_cast<double>(std::numeric_limits<float>::denorm_min()), "0000000400000001"},
      {std::numeric_limits<double>::infinity(), "000000047f800000"},
      {-std::numeric_limits<double>::infinity(), "00000004ff800000"}};
  for (const auto& item : cases) {
    ScalarWriteCase test;
    ASSERT_EQ(test.Init<double>(NANOARROW_TYPE_DOUBLE, nullptr, {item.value},
                                RedshiftScalarKind::kReal),
              0);
    ASSERT_EQ(test.Write(), 0) << test.error.message;
    EXPECT_EQ(test.Wire(), testing::NumericHex(item.wire));
  }
  for (double value : {1.1, (std::numeric_limits<double>::max)(),
                       std::numeric_limits<double>::denorm_min()}) {
    ScalarWriteCase test;
    ASSERT_EQ(test.Init<double>(NANOARROW_TYPE_DOUBLE, nullptr, {value},
                                RedshiftScalarKind::kReal),
              0);
    EXPECT_NE(test.Write(), 0);
  }
  ScalarWriteCase identity;
  ASSERT_EQ(identity.Init<double>(NANOARROW_TYPE_DOUBLE, "REAL", {1.1},
                                  RedshiftScalarKind::kDouble, false),
            0);
  EXPECT_NE(identity.Write(), 0);
  ScalarWriteCase wide;
  ASSERT_EQ(wide.Init<float>(NANOARROW_TYPE_FLOAT, nullptr, {-0.0f},
                             RedshiftScalarKind::kDouble),
            0);
  ASSERT_EQ(wide.Write(), 0);
  EXPECT_EQ(wide.Wire(), testing::NumericHex("000000088000000000000000"));
  ScalarWriteCase nan;
  ASSERT_EQ(nan.Init<double>(NANOARROW_TYPE_DOUBLE, nullptr,
                             {std::numeric_limits<double>::quiet_NaN()},
                             RedshiftScalarKind::kReal),
            0);
  ASSERT_EQ(nan.Write(), 0);
  EXPECT_EQ(nan.bytes->size_bytes, 8);
  const auto* raw = nan.bytes->data + 4;
  const uint32_t bits = (uint32_t{raw[0]} << 24) | (uint32_t{raw[1]} << 16) |
                        (uint32_t{raw[2]} << 8) | uint32_t{raw[3]};
  EXPECT_EQ(bits & 0x7f800000U, 0x7f800000U);
  EXPECT_NE(bits & 0x007fffffU, 0U);
}

TEST(RedshiftScalarTest, NullIngestParametersUseActualTargetOidInsteadOfTextFallback) {
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::ScalarResolver(BackendProfile::Redshift(), &resolver), 0);
  for (int col = 0; col < 6; col++) {
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(testing::ScalarSchema(schema.get(), NANOARROW_TYPE_NA), 0);
    nanoarrow::UniqueArray array;
    ASSERT_EQ(
        testing::ScalarBatch<int64_t>(schema.get(), array.get(), {std::nullopt}, nullptr),
        0);
    nanoarrow::UniqueArrayStream stream;
    adbc::driver::MakeArrayStream(schema.get(), array.get(), stream.get());
    BindStream bind;
    bind.SetBind(stream.get());
    ASSERT_TRUE(bind.Begin([] { return adbc::driver::Status::Ok(); }).ok());
    PostgresType field;
    ASSERT_EQ(resolver->Find(testing::kScalarOids[col], &field, nullptr), 0);
    PostgresType target(PostgresTypeId::kRecord);
    target.AppendChild("amount",
                       field.WithFieldDescriptor(-1, BackendProfile::Redshift()));
    // No SQL/network is needed for scalar parameter selection. The existing
    // production writer and actual destination OID are selected in one call.
    ASSERT_TRUE(bind.SetParamTypes(nullptr, *resolver, false, &target).ok());
    ASSERT_EQ(bind.param_types.size(), 1);
    EXPECT_EQ(bind.param_types[0], testing::kScalarOids[col]);
    EXPECT_EQ(bind.param_formats[0], kPgBinaryFormat);
  }
}

TEST(RedshiftScalarTest, CanonicalDescriptorIdentityLeavesUnknownQualifiersUnknown) {
  for (bool redshift : {false, true}) {
    const auto profile =
        redshift ? BackendProfile::Redshift() : BackendProfile::PostgreSQL();
    std::shared_ptr<PostgresTypeResolver> resolver;
    ASSERT_EQ(testing::ScalarResolver(profile, &resolver), 0);
    for (int i = 0; i < 6; i++) {
      PostgresType type;
      ASSERT_EQ(resolver->Find(testing::kScalarOids[i], &type, nullptr), 0);
      type = type.WithFieldDescriptor(std::nullopt, profile);
      nanoarrow::UniqueSchema schema;
      ArrowSchemaInit(schema.get());
      ASSERT_EQ(type.SetSchema(schema.get()), 0);
      ArrowStringView family{};
      ASSERT_EQ(ArrowMetadataGetValue(schema->metadata,
                                      ArrowCharView("ADBC:redshift:sql_type"), &family),
                0);
      if (redshift) {
        EXPECT_EQ(std::string_view(family.data, family.size_bytes),
                  testing::kScalarSqlNames[i]);
      } else
        EXPECT_EQ(family.data, nullptr);
      for (const char* key :
           {"ADBC:redshift:precision", "ADBC:redshift:scale", "ADBC:redshift:byte_width",
            "ADBC:redshift:type_modifier"}) {
        ArrowStringView value{};
        ASSERT_EQ(ArrowMetadataGetValue(schema->metadata, ArrowCharView(key), &value), 0);
        EXPECT_EQ(value.data, nullptr);
      }
    }
  }
}

TEST(RedshiftScalarTest, BooleanStrictWireRejectsBeforeArrayMutationAndPreservesPg) {
  for (bool redshift : {false, true}) {
    const auto profile =
        redshift ? BackendProfile::Redshift() : BackendProfile::PostgreSQL();
    std::shared_ptr<PostgresTypeResolver> resolver;
    ASSERT_EQ(testing::ScalarResolver(profile, &resolver), 0);
    PostgresType boolean;
    ASSERT_EQ(resolver->Find(testing::kScalarOids[5], &boolean, nullptr), 0);
    boolean = boolean.WithFieldDescriptor(-1, profile);
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(boolean.SetSchema(schema.get()), 0);
    nanoarrow::UniqueArray array;
    ArrowError error{};
    ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), &error), 0);
    ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
    std::unique_ptr<PostgresCopyFieldReader> reader;
    ASSERT_EQ(MakeCopyFieldReader(boolean, schema.get(), &reader, &error), 0);
    ASSERT_EQ(reader->InitSchema(schema.get()), 0);
    ASSERT_EQ(reader->InitArray(array.get()), 0);
    for (uint8_t value : {uint8_t{2}, uint8_t{255}}) {
      ArrowBufferView input{};
      input.data.as_uint8 = &value;
      input.size_bytes = 1;
      const auto length = array->length;
      const auto result = reader->Read(&input, 1, array.get(), &error);
      if (redshift) {
        EXPECT_NE(result, 0);
        EXPECT_EQ(array->length, length);
        EXPECT_EQ(input.size_bytes, 1);
      } else {
        EXPECT_EQ(result, 0);
      }
    }
    if (redshift) {
      ArrowBufferView short_input{};
      EXPECT_NE(reader->Read(&short_input, 1, array.get(), &error), 0);
      EXPECT_EQ(array->length, 0);
    }
  }
}
}  // namespace
}  // namespace adbcpq
