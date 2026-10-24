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
#include "driver/framework/utility.h"
#include "redshift_bytes_test_fixture.h"

namespace adbcpq {
namespace {
using adbc::driver::pgwire::BackendProfile;
class BytesWriteCase {
 public:
  ArrowErrorCode Init(ArrowType storage, const char* family, const char* width,
                      const std::vector<std::optional<std::string>>& values,
                      int target = -1, int32_t target_width = -1, bool redshift = true,
                      bool parameter_ingest = false) {
    NANOARROW_RETURN_NOT_OK(testing::BytesSchema(
        schema.get(),
        storage == NANOARROW_TYPE_FIXED_SIZE_BINARY ? NANOARROW_TYPE_BINARY : storage,
        family, width));
    if (storage == NANOARROW_TYPE_FIXED_SIZE_BINARY) {
      NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeFixedSize(
          schema->children[0], storage, static_cast<int32_t>(values.at(0)->size())));
    }
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
                               &error, false, parameter_ingest);
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
class FixedFailureWriter : public PostgresCopyFieldWriter {
 public:
  explicit FixedFailureWriter(int code) : code_(code) {}
  ArrowErrorCode Write(ArrowBuffer*, int64_t, ArrowError* error) override {
    ArrowErrorSet(error, "fixed writer failure");
    return code_;
  }

 private:
  int code_;
};

TEST(RedshiftBytesTest, ParameterValidationErrorsAreInvalidArguments) {
  struct Case {
    ArrowType storage;
    const char* family;
    const char* width;
    std::string value;
  };
  for (const auto& item :
       {Case{NANOARROW_TYPE_STRING, "CHAR", "4", testing::NumericHex("c2a0")},
        Case{NANOARROW_TYPE_STRING, "VARCHAR", "4", "abcde"},
        Case{NANOARROW_TYPE_BINARY, "VARBYTE", "4", "abcde"},
        Case{NANOARROW_TYPE_BINARY, "VARBYTE", "16777216", std::string(16777217, 'x')},
        Case{NANOARROW_TYPE_STRING, nullptr, nullptr, std::string("a\0b", 3)}}) {
    nanoarrow::UniqueSchema schema;
    nanoarrow::UniqueArray array;
    ArrowError error{};
    ASSERT_EQ(testing::BytesSchema(schema.get(), item.storage, item.family, item.width),
              0);
    ASSERT_EQ(testing::BytesBatch(schema.get(), array.get(), {item.value}, &error), 0);
    nanoarrow::UniqueArrayStream stream;
    adbc::driver::MakeArrayStream(schema.get(), array.get(), stream.get());
    BindStream bind;
    bind.SetBind(stream.get());
    ASSERT_TRUE(bind.Begin([] { return Status::Ok(); }).ok());
    std::shared_ptr<PostgresTypeResolver> resolver;
    ASSERT_EQ(testing::BytesResolver(BackendProfile::Redshift(), &resolver), 0);
    ASSERT_TRUE(bind.SetParamTypes(nullptr, *resolver, true).ok());
    ASSERT_TRUE(bind.PullNextArray().ok());
    EXPECT_EQ(bind.WriteParameter(0, 0).ToAdbc(nullptr), ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(bind.param_buffer->size_bytes, 0);
    for (int code : {ENOMEM, EIO}) {
      bind.bind_field_writers[0] = std::make_unique<FixedFailureWriter>(code);
      EXPECT_EQ(bind.WriteParameter(0, 0).ToAdbc(nullptr), ADBC_STATUS_INTERNAL);
    }
  }
}

TEST(RedshiftBytesTest, BinaryDomainExpansionRequiresRedshiftParameterRoute) {
  for (auto storage : {NANOARROW_TYPE_BINARY, NANOARROW_TYPE_LARGE_BINARY,
                       NANOARROW_TYPE_BINARY_VIEW, NANOARROW_TYPE_FIXED_SIZE_BINARY}) {
    for (int n : {1000000, 1000001, 1048575, 1048576, 1048577, 16777216, 16777217}) {
      std::string value(n, '\0');
      for (int i = 0; i < n; ++i) value[i] = static_cast<char>(i);
      BytesWriteCase qualified;
      ASSERT_EQ(qualified.Init(storage, "VARBYTE", "16777216", {value}, 2, 16777216, true,
                               true),
                0);
      const auto code = qualified.Write();
      EXPECT_EQ(code == 0, n <= 16777216);
      if (!code) {
        const auto wire = qualified.Wire();
        ASSERT_EQ(wire.size(), value.size() + 4);
        EXPECT_EQ(wire.substr(4), value);
      }
      for (bool redshift : {false, true}) {
        BytesWriteCase unqualified;
        ASSERT_EQ(unqualified.Init(storage, "VARBYTE", "16777216", {value},
                                   redshift ? 2 : -1, 16777216, redshift, !redshift),
                  0);
        EXPECT_EQ(unqualified.Write() == 0, n <= 1000000);
      }
    }
  }
  BytesWriteCase postgres;
  const std::string unmarked(1000001, 'x');
  ASSERT_EQ(postgres.Init(NANOARROW_TYPE_BINARY, nullptr, nullptr, {unmarked}, -1, -1,
                          false, true),
            0);
  ASSERT_EQ(postgres.Write(), 0);
  EXPECT_EQ(postgres.Wire().substr(4), unmarked);
  BytesWriteCase target;
  ASSERT_EQ(target.Init(NANOARROW_TYPE_BINARY, "VARBYTE", "16777216", {"123456789"}, 2, 8,
                        true, true),
            0);
  EXPECT_EQ(target.Write(), EINVAL);
  BytesWriteCase source;
  ASSERT_EQ(source.Init(NANOARROW_TYPE_BINARY, "VARBYTE", "8", {"123456789"}, 2, 16777216,
                        true, true),
            0);
  EXPECT_EQ(source.Write(), EINVAL);
}

TEST(RedshiftBytesTest, ParameterFamilyMismatchIsInvalidArgumentBeforeSql) {
  for (bool binary_source : {false, true}) {
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(
        testing::BytesSchema(
            schema.get(), binary_source ? NANOARROW_TYPE_BINARY : NANOARROW_TYPE_STRING,
            binary_source ? "VARBYTE" : "VARCHAR", "8"),
        0);
    nanoarrow::UniqueArrayStream stream;
    nanoarrow::EmptyArrayStream(schema.get()).ToArrayStream(stream.get());
    BindStream bind;
    bind.SetBind(stream.get());
    ASSERT_TRUE(bind.Begin([] { return Status::Ok(); }).ok());
    std::shared_ptr<PostgresTypeResolver> resolver;
    ASSERT_EQ(testing::BytesResolver(BackendProfile::Redshift(), &resolver), 0);
    PostgresType destination;
    ASSERT_EQ(resolver->Find(testing::kBytesOids[binary_source ? 1 : 2], &destination,
                             &bind.na_error),
              0);
    destination = destination.WithFieldDescriptor(12, BackendProfile::Redshift());
    PostgresType targets(PostgresTypeId::kRecord);
    targets.AppendChild("value", destination);
    EXPECT_EQ(bind.SetParamTypes(nullptr, *resolver, true, &targets).ToAdbc(nullptr),
              ADBC_STATUS_INVALID_ARGUMENT);
    ASSERT_EQ(bind.bind_field_writers.size(), 1U);
    EXPECT_EQ(bind.bind_field_writers[0], nullptr);
    EXPECT_EQ(bind.param_buffer->size_bytes, 0);
  }
}

TEST(RedshiftBytesTest, OrdinaryPostgresWriterErrorsRemainInternal) {
  BindStream bind;
  ArrowBufferInit(&bind.param_buffer.value);
  bind.bytes_fields = {false};
  bind.interval_fields = {false};
  bind.json_parse_fields = {false};
  bind.encoded_fields = {RedshiftEncodedKind::kNone};
  bind.bind_field_writers.push_back(std::make_unique<FixedFailureWriter>(EINVAL));
  EXPECT_EQ(bind.WriteParameter(0, 0).ToAdbc(nullptr), ADBC_STATUS_INTERNAL);
}

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
TEST(RedshiftBytesTest, ReaderMaximumAndAdjacentOversizePreservePrefixAndCursor) {
  struct Case {
    RedshiftBytesKind kind;
    ArrowType storage;
    int32_t maximum;
  };
  // Literal protocol boundaries are independent of ReadLimit(): changing the
  // production predicate must not silently change the expected test boundary.
  for (const auto test :
       {Case{RedshiftBytesKind::kVarchar, NANOARROW_TYPE_STRING, 16000000},
        Case{RedshiftBytesKind::kVarbyte, NANOARROW_TYPE_BINARY, 16777216}}) {
    SCOPED_TRACE(test.maximum);
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(ArrowSchemaInitFromType(schema.get(), test.storage), 0);
    nanoarrow::UniqueArray array;
    ArrowError error{};
    ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), &error), 0);
    ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
    // Unknown descriptor width isolates the family-wide read limit: a narrow
    // descriptor or the write guard must not be the reason for rejection.
    PostgresCopyBinaryFieldReader reader({test.kind, std::nullopt});
    ASSERT_EQ(reader.InitSchema(schema.get()), 0);
    ASSERT_EQ(reader.InitArray(array.get()), 0);
    const std::string prefix = "KEEP";
    ArrowBufferView data{};
    data.data.data = prefix.data();
    data.size_bytes = prefix.size();
    ASSERT_EQ(reader.Read(&data, 4, array.get(), &error), 0);

    std::string payload(test.maximum + 1, 'x');
    if (test.kind == RedshiftBytesKind::kVarbyte) {
      payload[0] = 0;
      payload[test.maximum - 1] = static_cast<char>(0xff);
    }
    data.data.data = payload.data();
    data.size_bytes = payload.size();
    const auto* cursor = data.data.data;
    const auto offsets_size = ArrowArrayBuffer(array.get(), 1)->size_bytes;
    const auto values_size = ArrowArrayBuffer(array.get(), 2)->size_bytes;
    ASSERT_EQ(reader.Read(&data, test.maximum + 1, array.get(), &error), EINVAL);
    EXPECT_NE(std::strstr(error.message, "byte length exceeds descriptor/format"),
              nullptr);
    EXPECT_EQ(data.data.data, cursor);
    EXPECT_EQ(data.size_bytes, test.maximum + 1);
    EXPECT_EQ(array->length, 1);
    EXPECT_EQ(array->null_count, 0);
    EXPECT_EQ(ArrowArrayBuffer(array.get(), 1)->size_bytes, offsets_size);
    EXPECT_EQ(ArrowArrayBuffer(array.get(), 2)->size_bytes, values_size);

    // The neighboring valid value must still decode after the failed field,
    // consuming exactly its own bytes and retaining the untouched suffix.
    ASSERT_EQ(reader.Read(&data, test.maximum, array.get(), &error), 0);
    EXPECT_EQ(data.size_bytes, 1);
    EXPECT_EQ(data.data.as_char, payload.data() + test.maximum);
    ASSERT_EQ(reader.FinishArray(array.get(), &error), 0);
    ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), &error), 0);
    nanoarrow::UniqueArrayView view;
    ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error), 0);
    ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), &error), 0);
    ASSERT_EQ(array->length, 2);
    const auto first = ArrowArrayViewGetBytesUnsafe(view.get(), 0);
    EXPECT_EQ(first.size_bytes, 4);
    EXPECT_EQ(std::memcmp(first.data.data, prefix.data(), 4), 0);
    const auto actual = ArrowArrayViewGetBytesUnsafe(view.get(), 1);
    ASSERT_EQ(actual.size_bytes, test.maximum);
    EXPECT_EQ(std::memcmp(actual.data.data, payload.data(), test.maximum), 0);
  }
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
