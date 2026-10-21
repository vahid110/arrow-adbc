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
#include <optional>
#include <string>
#include <vector>

#include "bind_stream.h"
#include "redshift_super_allocation_test.h"

namespace adbcpq {
namespace {
using adbc::driver::pgwire::BackendProfile;
using adbc::driver::pgwire::CatalogVendorFamily;

void SuperField(ArrowSchema* field) {
  ArrowSchemaInit(field);
  ASSERT_EQ(ArrowSchemaSetType(field, NANOARROW_TYPE_LARGE_STRING), 0);
  nanoarrow::UniqueBuffer metadata;
  ASSERT_EQ(ArrowMetadataBuilderInit(metadata.get(), nullptr), 0);
  for (const auto& item : std::vector<std::pair<const char*, std::string_view>>{
           {"ARROW:extension:name", RedshiftSuperSchema::kExtension},
           {"ARROW:extension:metadata", RedshiftSuperSchema::kMetadata},
           {"ADBC:redshift:sql_type", "SUPER"}})
    ASSERT_EQ(ArrowMetadataBuilderAppend(
                  metadata.get(), ArrowCharView(item.first),
                  ArrowStringView{item.second.data(),
                                  static_cast<int64_t>(item.second.size())}),
              0);
  ASSERT_EQ(ArrowSchemaSetMetadata(field, reinterpret_cast<char*>(metadata->data)), 0);
}
int Json(std::string_view value) {
  ArrowBufferView bytes{};
  bytes.data.as_char = value.data();
  bytes.size_bytes = value.size();
  ArrowError error{};
  return RedshiftSuperSchema::Validate(bytes, &error);
}

TEST(RedshiftSuperTest, ExactJsonGrammarAndDecodedDuplicateKeys) {
  for (std::string value :
       {"null", "true", "false", "0", "-0", "-12.34e+0000009999999999999999999999",
        " \t[1,2,{},[],null]\r\n", R"({"a":{"a":1},"b":2})", R"("\u0000")",
        R"({"\ud83d\ude00":1})", R"("\"\\/\b\f\n\r\t")"})
    EXPECT_EQ(Json(value), 0) << value;
  for (std::string value : {"",
                            " ",
                            "+1",
                            "01",
                            "-01",
                            "1.",
                            ".1",
                            "1e",
                            "1e+",
                            "NaN",
                            "Infinity",
                            "null null",
                            "[1,]",
                            "{,}",
                            R"({"a":1,"a":2})",
                            R"({"a":1,"\u0061":2})",
                            R"({"\u0000":1,"\u0000":2})",
                            R"({"😀":1,"\ud83d\ude00":2})",
                            R"("\ud800")",
                            R"("\udc00")",
                            R"("\ud800\u0000")",
                            R"("\x20")"})
    EXPECT_NE(Json(value), 0) << value;
  EXPECT_NE(Json(std::string("\"\xc0\x80\"", 4)), 0);
  EXPECT_NE(Json(std::string("\"\xed\xa0\x80\"", 5)), 0);
  EXPECT_NE(Json(std::string("null\0", 5)), 0);
}
TEST(RedshiftSuperTest, InclusiveDepthAndEncodedByteLimits) {
  EXPECT_EQ(Json(std::string(128, '[') + "0" + std::string(128, ']')), 0);
  EXPECT_NE(Json(std::string(129, '[') + "0" + std::string(129, ']')), 0);
  EXPECT_EQ(Json("\"" + std::string(999998, 'a') + "\""), 0);
  EXPECT_NE(Json("\"" + std::string(999999, 'a') + "\""), 0);
}
TEST(RedshiftSuperTest, ExactStorageBackendAndRoutePreflight) {
  nanoarrow::UniqueSchema schema;
  SuperField(schema.get());
  ArrowError error{};
  RedshiftSuperSchema parsed;
  ASSERT_EQ(RedshiftSuperSchema::Parse(schema.get(), true, &parsed, &error), 0);
  EXPECT_TRUE(parsed.enabled);
  EXPECT_NE(RedshiftSuperSchema::Parse(schema.get(), false, &parsed, &error), 0);
  bool has = false;
  EXPECT_EQ(
      RedshiftSuperSchema::ValidateRoute(schema.get(), true, true, false, &has, &error),
      0);
  EXPECT_TRUE(has);
  EXPECT_NE(
      RedshiftSuperSchema::ValidateRoute(schema.get(), true, false, false, &has, &error),
      0);
  EXPECT_NE(
      RedshiftSuperSchema::ValidateRoute(schema.get(), true, true, true, &has, &error),
      0);
  ASSERT_EQ(ArrowSchemaSetType(schema.get(), NANOARROW_TYPE_STRING), 0);
  EXPECT_NE(RedshiftSuperSchema::Parse(schema.get(), true, &parsed, &error), 0);
}
TEST(RedshiftSuperTest, CatalogFamilyIsBackendOwnedAndOidDynamic) {
  PostgresTypeResolver rs(BackendProfile::Redshift()), pg;
  ArrowError error{};
  for (auto* resolver : {&rs, &pg}) {
    ASSERT_EQ(resolver->Insert({72111, "super", "json_recv", 0, 0, 0}, &error), 0);
    ASSERT_EQ(resolver->Insert({72112, "varbyte", "varbyte_recv", 0, 0, 0}, &error), 0);
  }
  PostgresType type;
  ASSERT_EQ(rs.Find(72111, &type, &error), 0);
  EXPECT_EQ(type.vendor_family(), CatalogVendorFamily::kSuper);
  ASSERT_EQ(pg.Find(72111, &type, &error), 0);
  EXPECT_EQ(type.vendor_family(), CatalogVendorFamily::kNone);
  EXPECT_EQ(rs.GetVendorOID(CatalogVendorFamily::kSuper), 72111);
  EXPECT_EQ(rs.GetBytesOID(RedshiftBytesKind::kVarbyte), 72112);
}
TEST(RedshiftSuperTest, WriterNeedsExactGeneratedTargetAndPreservesRawNumbers) {
  PostgresTypeResolver resolver(BackendProfile::Redshift());
  ArrowError error{};
  ASSERT_EQ(resolver.Insert({72111, "super", "unqualified_receive", 0, 0, 0}, &error), 0);
  PostgresType target;
  ASSERT_EQ(resolver.Find(72111, &target, &error), 0);
  nanoarrow::UniqueSchema schema;
  SuperField(schema.get());
  nanoarrow::UniqueArray array;
  ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), &error), 0);
  ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
  const std::string raw = R"({"x":123456789012345678901234567890.1200,"y":1e9999999999})";
  ASSERT_EQ(ArrowArrayAppendString(array.get(), ArrowCharView(raw.c_str())), 0);
  ASSERT_EQ(ArrowArrayAppendString(array.get(), ArrowCharView("null")), 0);
  ASSERT_EQ(ArrowArrayAppendNull(array.get(), 1), 0);
  ASSERT_EQ(ArrowArrayAppendString(array.get(), ArrowCharView("{")), 0);
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), &error), 0);
  nanoarrow::UniqueArrayView view;
  ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error), 0);
  ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), &error), 0);
  std::unique_ptr<PostgresCopyFieldWriter> writer;
  EXPECT_NE(MakeCopyFieldWriter(schema.get(), view.get(), resolver, &target, &writer,
                                false, &error),
            0);
  ASSERT_EQ(MakeCopyFieldWriter(schema.get(), view.get(), resolver, &target, &writer,
                                false, &error, true),
            0);
  nanoarrow::UniqueBuffer bytes;
  ASSERT_EQ(writer->Write(bytes.get(), 0, &error), 0);
  EXPECT_EQ(
      std::string(reinterpret_cast<const char*>(bytes->data) + 4, bytes->size_bytes - 4),
      raw);
  const auto size = bytes->size_bytes;
  EXPECT_NE(writer->Write(bytes.get(), 3, &error), 0);
  EXPECT_EQ(bytes->size_bytes, size);
  EXPECT_EQ(ArrowArrayViewIsNull(view.get(), 1), 0);
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get(), 2));
}
TEST(RedshiftSuperTest, GeneratedExpressionsAndStrictContextValues) {
  EXPECT_EQ(BuildParameterizedInsertQuery("t", "a,b", 2, 2, {true, false}),
            "INSERT INTO t (a,b) VALUES (JSON_PARSE($1), $2), (JSON_PARSE($3), $4)");
  EXPECT_EQ(BuildParameterizedInsertQuery("t", "a", 1, 1),
            "INSERT INTO t (a) VALUES ($1)");
  for (auto v : {"OFF", "off", "false", "FaLsE"}) EXPECT_TRUE(StrictSuperParseSetting(v));
  for (auto v : {"on", "true", "", " off", "false ", "0", "falsejunk"})
    EXPECT_FALSE(StrictSuperParseSetting(v));
}
TEST(RedshiftSuperTest, ReservedMetadataDuplicatesOrphansAndUnknownVersionsReject) {
  for (const auto& item : std::vector<std::pair<const char*, const char*>>{
           {"ARROW:extension:name", "adbc.redshift.super.json"},
           {"ARROW:extension:metadata", "{}"},
           {"ADBC:redshift:sql_type", "SUPER"},
           {"ADBC:redshift:type_modifier", "-1"}}) {
    nanoarrow::UniqueSchema field;
    SuperField(field.get());
    nanoarrow::UniqueBuffer metadata;
    ASSERT_EQ(ArrowMetadataBuilderInit(metadata.get(), field->metadata), 0);
    ASSERT_EQ(ArrowMetadataBuilderAppend(metadata.get(), ArrowCharView(item.first),
                                         ArrowCharView(item.second)),
              0);
    ASSERT_EQ(
        ArrowSchemaSetMetadata(field.get(), reinterpret_cast<char*>(metadata->data)), 0);
    ArrowError error{};
    RedshiftSuperSchema parsed;
    EXPECT_NE(RedshiftSuperSchema::Parse(field.get(), true, &parsed, &error), 0);
  }
  for (const char* name : {"adbc.redshift.super.json.v2", "adbc.redshift.geometry.v1",
                           "adbc.redshift.hllsketch"}) {
    nanoarrow::UniqueSchema field;
    SuperField(field.get());
    nanoarrow::UniqueBuffer metadata;
    ASSERT_EQ(ArrowMetadataBuilderInit(metadata.get(), field->metadata), 0);
    ASSERT_EQ(
        ArrowMetadataBuilderSet(metadata.get(), ArrowCharView("ARROW:extension:name"),
                                ArrowCharView(name)),
        0);
    ASSERT_EQ(
        ArrowSchemaSetMetadata(field.get(), reinterpret_cast<char*>(metadata->data)), 0);
    ArrowError error{};
    RedshiftSuperSchema parsed;
    EXPECT_NE(RedshiftSuperSchema::Parse(field.get(), true, &parsed, &error), 0);
  }
}
TEST(RedshiftSuperTest, ActualTargetAndReadDescriptorCannotBecomeSourceAssertion) {
  PostgresTypeResolver resolver(BackendProfile::Redshift());
  ArrowError error{};
  ASSERT_EQ(resolver.Insert({72111, "super", "unqualified_receive", 0, 0, 0}, &error), 0);
  ASSERT_EQ(resolver.Insert({72113, "geometry", "unqualified_receive", 0, 0, 0}, &error),
            0);
  PostgresType target, spatial;
  ASSERT_EQ(resolver.Find(72111, &target, &error), 0);
  ASSERT_EQ(resolver.Find(72113, &spatial, &error), 0);
  nanoarrow::UniqueSchema descriptor;
  ASSERT_EQ(target.WithFieldDescriptor(-1, BackendProfile::Redshift())
                .SetSchema(descriptor.get(), "Redshift"),
            0);
  RedshiftSuperSchema parsed;
  EXPECT_NE(RedshiftSuperSchema::Parse(descriptor.get(), true, &parsed, &error), 0);
  PostgresType output(PostgresTypeId::kRecord);
  output.AppendChild("amount", target);
  EXPECT_FALSE(CheckVendorReadBoundary(output).ok());
  nanoarrow::UniqueSchema source;
  ArrowSchemaInit(source.get());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(source.get(), 1), 0);
  SuperField(source->children[0]);
  EXPECT_TRUE(ValidateVendorTargets(source.get(), output, true).ok());
  PostgresType wrong(PostgresTypeId::kRecord);
  wrong.AppendChild("amount", spatial);
  EXPECT_FALSE(ValidateVendorTargets(source.get(), wrong, true).ok());
  ASSERT_EQ(ArrowSchemaSetMetadata(source->children[0], nullptr), 0);
  EXPECT_FALSE(ValidateVendorTargets(source.get(), output, true).ok());
}
TEST(RedshiftSuperTest, FreshReserveFailurePreservesExistingWirePrefixAndAllocator) {
  nanoarrow::UniqueSchema schema;
  SuperField(schema.get());
  nanoarrow::UniqueArray array;
  ArrowError error{};
  ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), &error), 0);
  ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
  ASSERT_EQ(ArrowArrayAppendString(array.get(), ArrowCharView("true")), 0);
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), &error), 0);
  nanoarrow::UniqueArrayView view;
  ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error), 0);
  ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), &error), 0);
  auto allocation = ArrowBufferAllocatorDefault();
  allocation.reallocate = [](ArrowBufferAllocator*, uint8_t* old, int64_t,
                             int64_t) -> uint8_t* {
    EXPECT_EQ(old, nullptr);
    return nullptr;
  };
  auto writer =
      PostgresCopyFieldWriter::Create<RedshiftSuperFieldWriter>(view.get(), allocation);
  nanoarrow::UniqueBuffer bytes;
  ASSERT_EQ(ArrowBufferAppend(bytes.get(), "old", 3), 0);
  bytes->capacity_bytes = bytes->size_bytes;
  auto* old = bytes->data;
  EXPECT_EQ(writer->Write(bytes.get(), 0, &error), ENOMEM);
  EXPECT_EQ(bytes->data, old);
  EXPECT_EQ(bytes->size_bytes, 3);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(bytes->data), 3), "old");
}
TEST(RedshiftSuperTest, ScannerKeyNodeAndBucketFailuresReturnEnomem) {
  const std::string json = R"({"long_object_key_requiring_a_heap_buffer":1})";
  ArrowBufferView bytes{};
  bytes.data.as_char = json.data();
  bytes.size_bytes = json.size();
  int failed = 0;
  bool succeeded = false;
  for (int fail = 0; fail < 16; ++fail) {
    ArrowError error{};
    testing::super_allocation_fail_after = fail;
    const int result = RedshiftSuperSchema::Validate(bytes, &error);
    testing::super_allocation_fail_after = -1;
    if (result == NANOARROW_OK) {
      succeeded = true;
      break;
    }
    EXPECT_EQ(result, ENOMEM);
    EXPECT_NE(std::string_view(error.message).find("allocation failed"),
              std::string_view::npos);
    ++failed;
  }
  EXPECT_TRUE(succeeded);
  EXPECT_GE(failed, 3);  // Decoded key storage, node and bucket allocations.
}
}  // namespace
}  // namespace adbcpq
