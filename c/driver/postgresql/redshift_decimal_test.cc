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
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "copy/writer.h"
#include "redshift_decimal_test_fixture.h"
#include "redshift_numeric_test_fixture.h"

namespace adbcpq {
namespace {
using adbc::driver::pgwire::BackendProfile;

class DecimalWriteCase {
 public:
  ArrowErrorCode Init(ArrowType storage, int p, int s,
                      const std::vector<std::optional<std::string>>& values,
                      std::optional<int32_t> target = std::nullopt, bool marked = true,
                      bool redshift = true) {
    NANOARROW_RETURN_NOT_OK(
        testing::DecimalWriteSchema(schema.get(), storage, p, s, marked));
    NANOARROW_RETURN_NOT_OK(
        testing::DecimalWriteBatch(schema.get(), array.get(), values, &error));
    NANOARROW_RETURN_NOT_OK(
        ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error));
    NANOARROW_RETURN_NOT_OK(ArrowArrayViewSetArray(view.get(), array.get(), &error));
    const auto profile =
        redshift ? BackendProfile::Redshift() : BackendProfile::PostgreSQL();
    std::shared_ptr<PostgresTypeResolver> resolver;
    NANOARROW_RETURN_NOT_OK(testing::ModifierResolver(profile, &resolver));
    PostgresType destination;
    if (target) {
      NANOARROW_RETURN_NOT_OK(resolver->Find(testing::kNumericOid, &destination, &error));
      destination = destination.WithFieldDescriptor(*target, profile);
    }
    return MakeCopyFieldWriter(schema->children[0], view->children[0], *resolver,
                               target ? &destination : nullptr, &writer, false, &error);
  }

  ArrowErrorCode Write(int64_t row = 0) {
    const int64_t before = bytes->size_bytes;
    const auto result = writer->Write(bytes.get(), row, &error);
    if (result) EXPECT_EQ(bytes->size_bytes, before);
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

TEST(RedshiftDecimalWriteTest, IndependentLiteralWireAndPrecisionFamilies) {
  struct Case {
    int p;
    int s;
    const char* coefficient;
    const char* wire;
  };
  const Case cases[] = {
      {1, 0, "9", "0000000a00010000000000000009"},
      {18, 0, "999999999999999999", "0000001200050004000000000063270f270f270f270f"},
      {19, 0, "-9223372036854775808", "000000120005000440000000039a0d2c0170156516b0"},
      {20, 0, "99999999999999999999", "000000120005000400000000270f270f270f270f270f"},
      {38, 0, "99999999999999999999999999999999999999",
       "0000001c000a0009000000000063270f270f270f270f270f270f270f270f270f"},
      {10, 2, "1234", "0000000c0002000000000002000c0d48"},
      {10, 2, "-1234", "0000000c0002000040000002000c0d48"},
      {10, 2, "0", "000000080000000000000002"},
      {38, 37, "1", "0000000a0001fff60000002503e8"}};
  for (ArrowType storage : {NANOARROW_TYPE_DECIMAL128, NANOARROW_TYPE_DECIMAL256}) {
    for (const auto& item : cases) {
      DecimalWriteCase test;
      ASSERT_EQ(test.Init(storage, item.p, item.s, {item.coefficient}), 0)
          << test.error.message;
      ASSERT_EQ(test.Write(), 0) << test.error.message;
      EXPECT_EQ(test.Wire(), testing::NumericHex(item.wire));
    }
  }
}

TEST(RedshiftDecimalWriteTest, TargetRescaleIsExactAndStoredPrecision19IsSigned) {
  DecimalWriteCase exact;
  ASSERT_EQ(exact.Init(NANOARROW_TYPE_DECIMAL128, 18, 4, {"12500"}, 655366), 0);
  ASSERT_EQ(exact.Write(), 0);
  EXPECT_EQ(exact.Wire(), testing::NumericHex("0000000c0002000000000002000109c4"));
  DecimalWriteCase loss;
  ASSERT_EQ(loss.Init(NANOARROW_TYPE_DECIMAL128, 18, 4, {"12345"}, 655366), 0);
  EXPECT_NE(loss.Write(), 0);
  DecimalWriteCase overflow;
  ASSERT_EQ(overflow.Init(NANOARROW_TYPE_DECIMAL128, 20, 0, {"100000000"}, 655366), 0);
  EXPECT_NE(overflow.Write(), 0);
  for (const char* coefficient :
       {"9223372036854775808", "-9223372036854775809", "9999999999999999999"}) {
    DecimalWriteCase stored;
    ASSERT_EQ(stored.Init(NANOARROW_TYPE_DECIMAL128, 19, 0, {coefficient}, 1245188), 0);
    EXPECT_NE(stored.Write(), 0);
    // An OID-only parameter is not a stored DECIMAL(19,0) declaration. Preserve
    // the offered exact value for explicit application-written SQL semantics.
    DecimalWriteCase parameter;
    ASSERT_EQ(parameter.Init(NANOARROW_TYPE_DECIMAL128, 19, 0, {coefficient}), 0);
    EXPECT_EQ(parameter.Write(), 0);
  }
  for (const char* coefficient : {"9223372036854775807", "-9223372036854775808"}) {
    DecimalWriteCase stored;
    ASSERT_EQ(stored.Init(NANOARROW_TYPE_DECIMAL256, 19, 0, {coefficient}, 1245188), 0);
    EXPECT_EQ(stored.Write(), 0);
  }
  DecimalWriteCase scale38;
  ASSERT_EQ(scale38.Init(NANOARROW_TYPE_DECIMAL128, 38, 38, {"1"}, 2490409), 0);
  EXPECT_NE(scale38.Write(), 0);
}

TEST(RedshiftDecimalWriteTest, RawSignedExtremaNeverBypassSchemaPrecision) {
  for (ArrowType storage : {NANOARROW_TYPE_DECIMAL128, NANOARROW_TYPE_DECIMAL256}) {
    for (bool negative : {false, true}) {
      DecimalWriteCase test;
      ASSERT_EQ(test.Init(storage, 38, 0, {"0"}), 0);
      ArrowDecimal raw;
      ArrowDecimalInit(&raw, storage == NANOARROW_TYPE_DECIMAL128 ? 128 : 256, 38, 0);
      auto* words = reinterpret_cast<uint64_t*>(
          const_cast<void*>(test.array->children[0]->buffers[1]));
      for (int i = 0; i < raw.n_words; i++) words[i] = negative ? 0 : UINT64_MAX;
      words[raw.high_word_index] = negative ? (uint64_t{1} << 63) : (UINT64_MAX >> 1);
      EXPECT_NE(test.Write(), 0);
      EXPECT_EQ(test.bytes->size_bytes, 0);
    }
  }
  DecimalWriteCase invalid;
  ASSERT_EQ(invalid.Init(NANOARROW_TYPE_DECIMAL128, 1, 0, {"10"}), 0);
  EXPECT_NE(invalid.Write(), 0);
  DecimalWriteCase wide_schema;
  EXPECT_NE(wide_schema.Init(NANOARROW_TYPE_DECIMAL256, 76, 0, {"1"}), 0);
  RedshiftDecimalSchema ordinary;
  EXPECT_NE(RedshiftDecimalSchema::Parse(wide_schema.schema->children[0], false,
                                         &ordinary, &wide_schema.error),
            0);
  // The schema above is explicitly marked; ordinary PG is tested separately.
}

TEST(RedshiftDecimalWriteTest, TextIdentityHasClosedGrammarAndKnownTarget) {
  DecimalWriteCase valid;
  ASSERT_EQ(valid.Init(NANOARROW_TYPE_STRING, 0, 0, {"+001.2500"}, 655366), 0);
  ASSERT_EQ(valid.Write(), 0);
  EXPECT_EQ(valid.Wire(), testing::NumericHex("0000000c0002000000000002000109c4"));
  for (const char* text :
       {"", " ", " 1", "1 ", "1e2", ".1", "1.", "NaN", "inf", "--1", "1.2345"}) {
    DecimalWriteCase bad;
    ASSERT_EQ(bad.Init(NANOARROW_TYPE_STRING, 0, 0, {text}, 655366), 0);
    EXPECT_NE(bad.Write(), 0);
  }
  DecimalWriteCase plain;
  EXPECT_NE(plain.Init(NANOARROW_TYPE_STRING, 0, 0, {"1.25"}, 655366, false), 0);
  DecimalWriteCase unknown_target;
  EXPECT_NE(unknown_target.Init(NANOARROW_TYPE_STRING, 0, 0, {"1.25"}, -1), 0);
  DecimalWriteCase bad_stored_scale;
  EXPECT_NE(bad_stored_scale.Init(NANOARROW_TYPE_DECIMAL128, 38, 38, {"0"}, 2490410), 0);
}

TEST(RedshiftDecimalWriteTest, SchemaMetadataAndDeclarationRejectContradictions) {
  nanoarrow::UniqueSchema schema;
  ArrowError error{};
  ASSERT_EQ(testing::DecimalWriteSchema(schema.get(), NANOARROW_TYPE_DECIMAL128, 10, 2),
            0);
  ASSERT_EQ(
      testing::DecimalMetadata(schema->children[0], "ADBC:redshift:precision", "11"), 0);
  ASSERT_EQ(testing::DecimalMetadata(schema->children[0], "ADBC:redshift:scale", "2"), 0);
  RedshiftDecimalSchema parsed;
  EXPECT_NE(RedshiftDecimalSchema::Parse(schema->children[0], true, &parsed, &error), 0);
  schema.reset();
  ASSERT_EQ(testing::DecimalWriteSchema(schema.get(), NANOARROW_TYPE_STRING, 0, 0), 0);
  ASSERT_EQ(RedshiftDecimalSchema::Parse(schema->children[0], false, &parsed, &error), 0);
  std::string declaration;
  EXPECT_NE(parsed.Declaration(&declaration, &error), 0);
  ASSERT_EQ(
      testing::DecimalMetadata(schema->children[0], "ADBC:redshift:precision", "10;DROP"),
      0);
  ASSERT_EQ(testing::DecimalMetadata(schema->children[0], "ADBC:redshift:scale", "2"), 0);
  EXPECT_NE(RedshiftDecimalSchema::Parse(schema->children[0], true, &parsed, &error), 0);
  schema.reset();
  ASSERT_EQ(testing::DecimalWriteSchema(schema.get(), NANOARROW_TYPE_DECIMAL128, 38, 38),
            0);
  ASSERT_EQ(RedshiftDecimalSchema::Parse(schema->children[0], true, &parsed, &error), 0);
  EXPECT_NE(parsed.Declaration(&declaration, &error), 0);
  schema.reset();
  ASSERT_EQ(
      testing::DecimalWriteSchema(schema.get(), NANOARROW_TYPE_DECIMAL256, 76, 0, false),
      0);
  ASSERT_EQ(RedshiftDecimalSchema::Parse(schema->children[0], false, &parsed, &error), 0);
  EXPECT_FALSE(parsed.enabled);
}
}  // namespace
}  // namespace adbcpq
