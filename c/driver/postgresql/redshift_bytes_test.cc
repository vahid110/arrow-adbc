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

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bind_stream.h"
#include "copy/reader.h"
#include "copy/writer.h"
#include "redshift_bytes_test_fixture.h"

namespace adbcpq {
namespace {
using adbc::driver::pgwire::BackendProfile;
class BytesWriteCase {
 public:
  ArrowErrorCode Init(ArrowType storage, const char* family, const char* width,
                      const std::vector<std::optional<std::string>>& values,
                      int target = -1, int32_t target_width = -1, bool redshift = true) {
    NANOARROW_RETURN_NOT_OK(testing::BytesSchema(schema.get(), storage, family, width));
    NANOARROW_RETURN_NOT_OK(
        testing::BytesBatch(schema.get(), array.get(), values, &error));
    NANOARROW_RETURN_NOT_OK(
        ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error));
    NANOARROW_RETURN_NOT_OK(ArrowArrayViewSetArray(view.get(), array.get(), &error));
    const auto profile =
        redshift ? BackendProfile::Redshift() : BackendProfile::PostgreSQL();
    std::shared_ptr<PostgresTypeResolver> resolver;
    NANOARROW_RETURN_NOT_OK(testing::BytesResolver(profile, &resolver));
    PostgresType destination;
    if (target >= 0) {
      NANOARROW_RETURN_NOT_OK(
          resolver->Find(testing::kBytesOids[target], &destination, &error));
      destination = destination.WithFieldDescriptor(
          target_width < 0 ? -1 : target_width + 4, profile);
    }
    return MakeCopyFieldWriter(schema->children[0], view->children[0], *resolver,
                               target < 0 ? nullptr : &destination, &writer, false,
                               &error);
  }
  ArrowErrorCode Write(int row = 0) {
    auto before = bytes->size_bytes;
    auto code = writer->Write(bytes.get(), row, &error);
    if (code) {
      EXPECT_EQ(before, bytes->size_bytes);
    }
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
TEST(RedshiftBytesTest, LiteralWirePreservesUtf8SpacesQuotesBackslashesAndRawBinary) {
  BytesWriteCase text;
  ASSERT_EQ(text.Init(NANOARROW_TYPE_STRING, "VARCHAR", "20",
                      {testing::NumericHex("f09f9880275c2020")}),
            0);
  ASSERT_EQ(text.Write(), 0);
  EXPECT_EQ(text.Wire(), testing::NumericHex("00000008f09f9880275c2020"));
  BytesWriteCase raw;
  ASSERT_EQ(raw.Init(NANOARROW_TYPE_BINARY, "VARBYTE", "8",
                     {testing::NumericHex("0080ff5c78")}),
            0);
  ASSERT_EQ(raw.Write(), 0);
  EXPECT_EQ(raw.Wire(), testing::NumericHex("000000050080ff5c78"));
  BytesWriteCase spaces;
  ASSERT_EQ(spaces.Init(NANOARROW_TYPE_STRING, "CHAR", "2", {"a  "}), 0);
  EXPECT_NE(spaces.Write(), 0);  // Never allow server blank truncation.
}
TEST(RedshiftBytesTest, Utf8GrammarAndSingleByteCharRejectBeforeOutput) {
  for (const auto* hex : {"00", "80", "c080", "c2", "e08080", "eda080", "f0808080",
                          "f4908080", "f5808080", "e228a1"}) {
    BytesWriteCase test;
    ASSERT_EQ(
        test.Init(NANOARROW_TYPE_STRING, "VARCHAR", "20", {testing::NumericHex(hex)}), 0);
    EXPECT_NE(test.Write(), 0) << hex;
  }
  for (const auto* hex : {"7f", "0120275c", "c280", "f48fbfbf"}) {
    BytesWriteCase test;
    ASSERT_EQ(
        test.Init(NANOARROW_TYPE_STRING, "VARCHAR", "20", {testing::NumericHex(hex)}), 0);
    EXPECT_EQ(test.Write(), 0) << hex;
  }
  BytesWriteCase fixed;
  ASSERT_EQ(fixed.Init(NANOARROW_TYPE_STRING, "CHAR", "4", {testing::NumericHex("c2a0")}),
            0);
  EXPECT_NE(fixed.Write(), 0);
}
TEST(RedshiftBytesTest,
     DestinationAndSourceWidthAreBothCheckedWithClosedBinaryCoercions) {
  BytesWriteCase narrow;
  ASSERT_EQ(narrow.Init(NANOARROW_TYPE_LARGE_STRING, "VARCHAR", "20", {"abcd"}, 0, 3), 0);
  EXPECT_NE(narrow.Write(), 0);
  BytesWriteCase source;
  ASSERT_EQ(source.Init(NANOARROW_TYPE_STRING_VIEW, "VARCHAR", "3", {"abcd"}, 1, 20), 0);
  EXPECT_NE(source.Write(), 0);
  BytesWriteCase crossed;
  EXPECT_NE(crossed.Init(NANOARROW_TYPE_BINARY, "VARBYTE", "20", {"abcd"}, 1, 20), 0);
  BytesWriteCase unknown;
  EXPECT_NE(unknown.Init(NANOARROW_TYPE_STRING, nullptr, nullptr, {"abcd"}, 1), 0);
}
TEST(RedshiftBytesTest,
     FullReadWidthsStoredDeclarationsAndProvisionalInsertEnvelopeDiffer) {
  for (auto storage :
       {NANOARROW_TYPE_STRING, NANOARROW_TYPE_LARGE_STRING, NANOARROW_TYPE_STRING_VIEW}) {
    BytesWriteCase test;
    ASSERT_EQ(test.Init(storage, "VARCHAR", "70000", {std::string(70000, 'a')}), 0);
    EXPECT_EQ(test.Write(), 0);
    RedshiftBytesSchema parsed;
    ASSERT_EQ(
        RedshiftBytesSchema::Parse(test.schema->children[0], true, &parsed, &test.error),
        0);
    std::string declaration;
    EXPECT_NE(parsed.Declaration(&declaration, &test.error), 0);
  }
  for (int n : {1000000, 1000001, 1048575, 1048576, 1048577}) {
    BytesWriteCase test;
    ASSERT_EQ(test.Init(NANOARROW_TYPE_LARGE_BINARY, "VARBYTE", "16777216",
                        {std::string(n, '\0')}),
              0);
    EXPECT_EQ(test.Write() == 0, n <= RedshiftBytesSchema::kInsertBinaryLimit);
  }
  ArrowError error{};
  std::string declaration;
  EXPECT_EQ((RedshiftBytesSchema{RedshiftBytesKind::kVarbyte, std::nullopt}.Declaration(
                &declaration, &error)),
            0);
  EXPECT_EQ(declaration, "VARBYTE(16777216)");
  EXPECT_EQ((RedshiftBytesSchema{RedshiftBytesKind::kVarchar, std::nullopt}.Declaration(
                &declaration, &error)),
            0);
  EXPECT_EQ(declaration, "VARCHAR(65535)");
  EXPECT_NE((RedshiftBytesSchema{RedshiftBytesKind::kChar, std::nullopt}.Declaration(
                &declaration, &error)),
            0);
}
TEST(RedshiftBytesTest, MalformedOffsetsAndViewReferencesFailBeforeUnsafeGetter) {
  for (auto storage : {NANOARROW_TYPE_STRING, NANOARROW_TYPE_LARGE_STRING}) {
    BytesWriteCase test;
    ASSERT_EQ(test.Init(storage, "VARCHAR", "20", {"abc"}), 0);
    if (storage == NANOARROW_TYPE_STRING) {
      auto* offsets =
          const_cast<int32_t*>(test.view->children[0]->buffer_views[1].data.as_int32);
      offsets[1] = -1;
    } else {
      auto* offsets =
          const_cast<int64_t*>(test.view->children[0]->buffer_views[1].data.as_int64);
      offsets[1] = INT64_MAX;
    }
    EXPECT_NE(test.Write(), 0);
  }
  BytesWriteCase ref;
  ASSERT_EQ(ref.Init(NANOARROW_TYPE_BINARY_VIEW, "VARBYTE", "20", {std::string(20, 'a')}),
            0);
  auto* values = const_cast<ArrowBinaryView*>(
      ref.view->children[0]->buffer_views[1].data.as_binary_view);
  values[0].ref.buffer_index = INT32_MAX;
  EXPECT_NE(ref.Write(), 0);
}
TEST(RedshiftBytesTest, RawCharPaddingEmptyNullAndStrictReadBoundsPreserveBytes) {
  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_STRING), 0);
  nanoarrow::UniqueArray array;
  ArrowError error{};
  ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), &error), 0);
  ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
  PostgresCopyBinaryFieldReader reader({RedshiftBytesKind::kChar, 4});
  ASSERT_EQ(reader.InitSchema(schema.get()), 0);
  ASSERT_EQ(reader.InitArray(array.get()), 0);
  const std::string padded = "a   ";
  ArrowBufferView data{};
  data.data.data = padded.data();
  data.size_bytes = 4;
  ASSERT_EQ(reader.Read(&data, 4, array.get(), &error), 0);
  data.size_bytes = 0;
  ASSERT_EQ(reader.Read(&data, 0, array.get(), &error), 0);
  ASSERT_EQ(reader.Read(&data, -1, array.get(), &error), 0);
  EXPECT_NE(reader.Read(&data, -2, array.get(), &error), 0);
  EXPECT_EQ(array->length, 3);
  ASSERT_EQ(reader.FinishArray(array.get(), &error), 0);
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), &error), 0);
  nanoarrow::UniqueArrayView view;
  ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error), 0);
  ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), &error), 0);
  const auto got = ArrowArrayViewGetStringUnsafe(view.get(), 0);
  EXPECT_EQ(std::string(got.data, got.size_bytes), padded);
  EXPECT_EQ(ArrowArrayViewGetStringUnsafe(view.get(), 1).size_bytes, 0);
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get(), 2));
}
TEST(RedshiftBytesTest, DynamicVarbyteOidCannotFallBackToByteaAndMarkersAreValidated) {
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::BytesResolver(BackendProfile::Redshift(), &resolver), 0);
  BytesWriteCase test;
  ASSERT_EQ(test.Init(NANOARROW_TYPE_BINARY, "VARBYTE", "20", {"a"}), 0);
  PostgresType type;
  ASSERT_EQ(
      PostgresType::FromSchema(*resolver, test.schema->children[0], &type, &test.error),
      0);
  EXPECT_EQ(type.oid(), testing::kBytesOids[2]);
  for (const char* n : {"0", "-1", "16777217", "1junk"}) {
    BytesWriteCase invalid;
    EXPECT_NE(invalid.Init(NANOARROW_TYPE_BINARY, "VARBYTE", n, {"a"}), 0);
  }
  ASSERT_EQ(testing::DecimalMetadata(test.schema->children[0],
                                     "ADBC:redshift:type_modifier", "99"),
            0);
  EXPECT_NE(
      PostgresType::ValidateBytesWriteSchema(*resolver, test.schema.get(), &test.error),
      0);
}
}  // namespace
}  // namespace adbcpq
