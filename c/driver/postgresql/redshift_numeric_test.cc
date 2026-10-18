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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "redshift_numeric_test_fixture.h"

namespace adbcpq {
namespace {
using adbc::driver::pgwire::BackendProfile;

class NumericReaderFixture {
 public:
  ArrowErrorCode Init(int32_t modifier, bool redshift = true) {
    auto profile = redshift ? BackendProfile::Redshift() : BackendProfile::PostgreSQL();
    type_ = PostgresType(PostgresTypeId::kNumeric)
                .WithPgTypeInfo(testing::kNumericOid, "numeric")
                .WithFieldDescriptor(modifier, profile);
    ArrowSchemaInit(schema_.get());
    NANOARROW_RETURN_NOT_OK(
        type_.SetSchema(schema_.get(), redshift ? "Redshift" : "PostgreSQL"));
    NANOARROW_RETURN_NOT_OK(MakeCopyFieldReader(type_, schema_.get(), &reader_, &error_));
    reader_->Init(type_);
    NANOARROW_RETURN_NOT_OK(reader_->InitSchema(schema_.get()));
    NANOARROW_RETURN_NOT_OK(
        ArrowArrayInitFromSchema(array_.get(), schema_.get(), &error_));
    NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(array_.get()));
    return reader_->InitArray(array_.get());
  }

  ArrowErrorCode Read(const std::string& bytes, int32_t length = -3) {
    ArrowBufferView data{};
    data.data.data = bytes.data();
    data.size_bytes = bytes.size();
    const auto* start = data.data.as_char;
    const int64_t size = data.size_bytes;
    const auto code = reader_->Read(
        &data, length == -3 ? static_cast<int32_t>(size) : length, array_.get(), &error_);
    if (code) {
      EXPECT_EQ(data.data.as_char, start);
      EXPECT_EQ(data.size_bytes, size);
    } else if (length != -1) {
      EXPECT_EQ(data.size_bytes, size - (length == -3 ? size : length));
    }
    return code;
  }

  std::string Text() {
    EXPECT_EQ(ArrowArrayFinishBuildingDefault(array_.get(), &error_), 0);
    nanoarrow::UniqueArrayView view;
    EXPECT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema_.get(), &error_), 0);
    EXPECT_EQ(ArrowArrayViewSetArray(view.get(), array_.get(), &error_), 0);
    const auto value = ArrowArrayViewGetStringUnsafe(view.get(), array_->length - 1);
    return std::string(value.data, value.size_bytes);
  }

  PostgresType type_;
  nanoarrow::UniqueSchema schema_;
  nanoarrow::UniqueArray array_;
  std::unique_ptr<PostgresCopyFieldReader> reader_;
  ArrowError error_{};
};

TEST(RedshiftNumericTest, LiteralFieldsReachCDataWithStableDescriptorsAcrossBatches) {
  const char* coefficients[3][8] = {
      {"9", "999999999999999999", "9223372036854775807", "99999999999999999999",
       "99999999999999999999999999999999999999", "125",
       "99999999999999999999999999999999999999",
       "99999999999999999999999999999999999999"},
      {"-9", "-999999999999999999", "-9223372036854775808", "0",
       "-99999999999999999999999999999999999999", "-123", "-1", "1"},
      {"0", "1", "9999999999999999999", "1", "0", "0", "0", "0"}};
  const int precision[] = {1, 18, 19, 20, 38, 10, 38, 38};
  const int scale[] = {0, 0, 0, 0, 0, 2, 37, 38};
  for (int batch : {0, 1, 2, 3}) {
    nanoarrow::UniqueSchema schema;
    nanoarrow::UniqueArray array;
    ArrowError error{};
    ASSERT_EQ(testing::DecimalFixture(batch, array.get(), schema.get(), &error), 0)
        << error.message;
    nanoarrow::UniqueArrayView view;
    ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error), 0);
    ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), &error), 0);
    EXPECT_EQ(array->length, batch == 2 ? 0 : (batch == 3 ? 1 : (batch == 1 ? 2 : 4)));
    for (int col = 0; col < 8; col++) {
      ArrowSchemaView field;
      ASSERT_EQ(ArrowSchemaViewInit(&field, schema->children[col], &error), 0);
      EXPECT_EQ(field.type, NANOARROW_TYPE_DECIMAL128);
      EXPECT_EQ(field.decimal_precision, precision[col]);
      EXPECT_EQ(field.decimal_scale, scale[col]);
      for (int64_t row = 0; row < array->length; row++) {
        const bool null = batch == 3 || row == 3;
        EXPECT_EQ(ArrowArrayViewIsNull(view->children[col], row), null);
        if (null) continue;
        ArrowDecimal value;
        ArrowDecimalInit(&value, 128, precision[col], scale[col]);
        ArrowArrayViewGetDecimalUnsafe(view->children[col], row, &value);
        nanoarrow::UniqueBuffer digits;
        ASSERT_EQ(ArrowDecimalAppendDigitsToBuffer(&value, digits.get()), 0);
        EXPECT_EQ(std::string(reinterpret_cast<char*>(digits->data), digits->size_bytes),
                  coefficients[batch == 1 ? 2 - row : row][col]);
      }
    }
    EXPECT_STREQ(schema->children[8]->format, "u");
    ArrowStringView extension{};
    ASSERT_EQ(ArrowMetadataGetValue(schema->children[8]->metadata,
                                    ArrowCharView("ARROW:extension:name"), &extension),
              0);
    EXPECT_EQ(extension.data, nullptr);
    if (batch == 0) {
      const char* expected[] = {"-12345678901234567890.0012300", "0.12345678", "0.0000"};
      for (int row = 0; row < 3; row++) {
        const auto text = ArrowArrayViewGetStringUnsafe(view->children[8], row);
        EXPECT_EQ(std::string(text.data, text.size_bytes), expected[row]);
        EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view->children[9], row), 42);
      }
    }
  }
}

TEST(RedshiftNumericTest, MalformedFieldsRejectBeforeAnyAppendOrCursorChange) {
  const char* invalid[] = {"",
                           "00000000000000",    // Short headers.
                           "ffff000000000000",  // Negative count.
                           "7fff000000000000",  // Claimed count exceeds field.
                           "0001000000000000",  // Missing digit.
                           "0000000000010000",  // Unknown sign.
                           "00000000c0000000",  // Nonfinite unsupported.
                           "00000000d0000000",
                           "00000000f0000000",
                           "0000000000004000",          // Invalid display scale.
                           "00010000000000002710",      // Digit 10000.
                           "0001000000000000ffff",      // Negative digit.
                           "000000000000000000",        // Trailing byte.
                           "000200000000000200010929",  // 1.2345 hidden by dscale2.
                           "00017fff000000000001",      // Extreme integral weight.
                           "00018000000000000001"};     // Fraction hidden by dscale0.
  for (int32_t modifier : {655366, -1}) {
    for (const char* hex : invalid) {
      SCOPED_TRACE(hex);
      NumericReaderFixture fixture;
      ASSERT_EQ(fixture.Init(modifier), 0);
      // Unknown text permits the bounded large integral value (131069 digits),
      // while known precision rejects it. Exercise it separately below.
      if (modifier == -1 && std::string(hex) == "00017fff000000000001") continue;
      EXPECT_NE(fixture.Read(testing::NumericHex(hex)), 0);
      EXPECT_EQ(fixture.array_->length, 0);
    }
  }
  NumericReaderFixture fixture;
  ASSERT_EQ(fixture.Init(655366), 0);
  const auto bytes = testing::NumericHex("00010000000000000001");
  EXPECT_NE(fixture.Read(bytes, 8), 0);  // Following bytes cannot rescue short field.
  EXPECT_NE(fixture.Read(bytes, -2), 0);
  EXPECT_NE(fixture.Read(bytes.substr(0, 9), 10), 0);
  EXPECT_EQ(fixture.array_->length, 0);
}

TEST(RedshiftNumericTest, ExactRescaleAndPrecisionRejectWithoutSilentRounding) {
  NumericReaderFixture fixture;
  ASSERT_EQ(fixture.Init(655366), 0);  // numeric(10,2)
  // dscale4 stores exact extra zeroes, so 1.2500 becomes coefficient125.
  EXPECT_EQ(fixture.Read(testing::NumericHex("0002000000000004000109c4")), 0);
  EXPECT_EQ(fixture.array_->length, 1);
  // Actual 1.2345 cannot fit scale2, even with sufficient display scale.
  EXPECT_NE(fixture.Read(testing::NumericHex("000200000000000400010929")), 0);
  // 100000000.00 needs11 coefficient digits, beyond declared precision10.
  EXPECT_NE(fixture.Read(testing::NumericHex("00010002000000000001")), 0);
  EXPECT_EQ(fixture.array_->length, 1);

  NumericReaderFixture single;
  ASSERT_EQ(single.Init(65540), 0);
  EXPECT_NE(single.Read(testing::NumericHex("0001000000000000000a")), 0);
  EXPECT_EQ(single.Read(testing::NumericHex("00010000000000000009")), 0);

  NumericReaderFixture wide;
  ASSERT_EQ(wide.Init(2490372), 0);  // numeric(38,0)
  // 10^38 and its negative have39 coefficient digits. Neither can be accepted
  // merely because ArrowDecimalSetDigits can write two words without checking p.
  EXPECT_NE(wide.Read(testing::NumericHex("00010009000000000064")), 0);
  EXPECT_NE(wide.Read(testing::NumericHex("00010009400000000064")), 0);
  // 2^127 is beyond signed128, also rejected before any fixed-word conversion.
  EXPECT_NE(wide.Read(testing::NumericHex(
                "000a00090000000000aa0583209a01d5090d0c601c871bf620da1660")),
            0);
  EXPECT_EQ(wide.array_->length, 0);
}

TEST(RedshiftNumericTest, UnknownExactTextRemainsStableUnalignedAndBounded) {
  NumericReaderFixture fixture;
  ASSERT_EQ(fixture.Init(-1), 0);
  EXPECT_EQ(fixture.Read(testing::NumericHex("00017fff000000000001")), 0);
  const auto big = fixture.Text();
  EXPECT_EQ(big.size(), 131069);
  EXPECT_EQ(big[0], '1');
  EXPECT_EQ(big.find_first_not_of('0', 1), std::string::npos);
  NumericReaderFixture zero;
  ASSERT_EQ(zero.Init(-1), 0);
  EXPECT_EQ(zero.Read(testing::NumericHex("0000800040003fff")), 0);
  EXPECT_EQ(zero.Text().size(), 16385);
  // Explicit unaligned input passed through the production reader.
  NumericReaderFixture unaligned;
  ASSERT_EQ(unaligned.Init(-1), 0);
  std::string bytes = "x" + testing::NumericHex("0002000000000004000109c4");
  ArrowBufferView data{};
  data.data.as_char = bytes.data() + 1;
  data.size_bytes = bytes.size() - 1;
  ASSERT_EQ(unaligned.reader_->Read(&data, static_cast<int32_t>(data.size_bytes),
                                    unaligned.array_.get(), &unaligned.error_),
            0);
  EXPECT_EQ(unaligned.Text(), "1.2500");
}

TEST(RedshiftNumericTest, RejectedLaterRowExportsNoPartialCDataBatch) {
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::ModifierResolver(BackendProfile::Redshift(), &resolver), 0);
  PqResultHelper query(nullptr, "");
  ASSERT_EQ(testing::DecimalDescription(&query, 0), 0);
  PGresult* result = query.ReleaseResult();
  const auto invalid = testing::NumericHex("0001000000000000000a");
  ASSERT_EQ(PQsetvalue(result, 1, 0, const_cast<char*>(invalid.data()),
                       static_cast<int>(invalid.size())),
            1);
  query.SetResult(result);
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
  ArrowError error{};
  EXPECT_NE(testing::FixtureFromQuery(query, resolver, array.get(), schema.get(), &error),
            0);
  EXPECT_EQ(array->release, nullptr);
  EXPECT_EQ(schema->release, nullptr);
}

TEST(RedshiftNumericTest, PostgreSQLSchemaAndGenericNumericReaderRemainUnchanged) {
  NumericReaderFixture postgres;
  ASSERT_EQ(postgres.Init(655366, false), 0);
  EXPECT_STREQ(postgres.schema_->format, "u");
  EXPECT_EQ(postgres.Read(testing::NumericHex("0002000000000004000109c4")), 0);
  EXPECT_EQ(postgres.Text(), "1.2500");
  NumericReaderFixture unknown;
  ASSERT_EQ(unknown.Init(65539), 0);  // Unsupported modifier must not invent p/s.
  EXPECT_STREQ(unknown.schema_->format, "u");
  EXPECT_EQ(unknown.Read(testing::NumericHex("0002000000000004000109c4")), 0);
  EXPECT_EQ(unknown.Text(), "1.2500");
}
}  // namespace
}  // namespace adbcpq
