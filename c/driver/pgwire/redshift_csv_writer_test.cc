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

#include "postgresql/redshift_csv_writer.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.hpp>

#include "postgresql/redshift_staged_copy_coordinator.h"

namespace adbc::driver::pgwire {
namespace {

class CsvBatch {
 public:
  explicit CsvBatch(const std::vector<ArrowType>& types,
                    const std::vector<std::string>& names) {
    EXPECT_EQ(types.size(), names.size());
    EXPECT_EQ(ArrowSchemaInitFromType(schema_.get(), NANOARROW_TYPE_STRUCT),
              NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetTypeStruct(schema_.get(), types.size()), NANOARROW_OK);
    for (std::size_t i = 0; i < types.size(); ++i) {
      EXPECT_EQ(ArrowSchemaSetType(schema_->children[i], types[i]), NANOARROW_OK);
      EXPECT_EQ(ArrowSchemaSetName(schema_->children[i], names[i].c_str()), NANOARROW_OK);
    }
    EXPECT_EQ(ArrowArrayInitFromSchema(array_.get(), schema_.get(), nullptr),
              NANOARROW_OK);
    EXPECT_EQ(ArrowArrayStartAppending(array_.get()), NANOARROW_OK);
  }

  void AppendRow(int32_t id, int64_t big, std::string_view text) {
    ASSERT_EQ(ArrowArrayAppendInt(array_->children[0], id), NANOARROW_OK);
    ASSERT_EQ(ArrowArrayAppendInt(array_->children[1], big), NANOARROW_OK);
    const ArrowStringView value = {text.data(), static_cast<int64_t>(text.size())};
    ASSERT_EQ(ArrowArrayAppendString(array_->children[2], value), NANOARROW_OK);
    ASSERT_EQ(ArrowArrayFinishElement(array_.get()), NANOARROW_OK);
  }

  void Finish() {
    ASSERT_EQ(ArrowArrayFinishBuildingDefault(array_.get(), nullptr), NANOARROW_OK);
  }

  ArrowSchema* schema() { return schema_.get(); }
  ArrowArray* array() { return array_.get(); }

 private:
  nanoarrow::UniqueSchema schema_;
  nanoarrow::UniqueArray array_;
};

CsvBatch MakeBatch() {
  return CsvBatch({NANOARROW_TYPE_INT32, NANOARROW_TYPE_INT64, NANOARROW_TYPE_STRING},
                  {"id", "big", "label"});
}

RedshiftCsvWriteStatus Write(CsvBatch* batch, std::string* out) {
  return WriteRedshiftCsv(batch->schema(), batch->array(), {"id", "big", "label"}, out);
}

TEST(RedshiftCsvWriterTest, SerializesExactFieldOrderAndQuotesText) {
  auto batch = MakeBatch();
  batch.AppendRow(-42, std::numeric_limits<int64_t>::min(), "a,b\n\"c\"\r");
  batch.AppendRow(std::numeric_limits<int32_t>::max(),
                  std::numeric_limits<int64_t>::max(), "");
  batch.AppendRow(0, 0, "Grüße");
  batch.Finish();

  std::string csv;
  EXPECT_EQ(Write(&batch, &csv), RedshiftCsvWriteStatus::kSucceeded);
  EXPECT_EQ(csv,
            "-42,-9223372036854775808,\"a,b\n\"\"c\"\"\r\"\n"
            "2147483647,9223372036854775807,\"\"\n"
            "0,0,\"Grüße\"\n");
}

TEST(RedshiftCsvWriterTest, RejectsWrongColumnOrderWithoutChangingOutput) {
  auto batch = MakeBatch();
  batch.AppendRow(1, 2, "three");
  batch.Finish();
  std::string csv = "original";
  EXPECT_EQ(WriteRedshiftCsv(batch.schema(), batch.array(), {"big", "id", "label"}, &csv),
            RedshiftCsvWriteStatus::kColumnMismatch);
  EXPECT_EQ(csv, "original");
}

TEST(RedshiftCsvWriterTest, HonorsParentAndChildArrayOffsets) {
  auto batch = MakeBatch();
  batch.AppendRow(1, 10, "first");
  batch.AppendRow(2, 20, "second");
  batch.AppendRow(3, 30, "third");
  batch.Finish();

  batch.array()->offset = 1;
  batch.array()->length = 1;
  std::string csv;
  EXPECT_EQ(Write(&batch, &csv), RedshiftCsvWriteStatus::kSucceeded);
  EXPECT_EQ(csv, "2,20,\"second\"\n");

  for (int64_t i = 0; i < batch.array()->n_children; ++i) {
    batch.array()->children[i]->offset = 1;
    batch.array()->children[i]->length = 2;
  }
  EXPECT_EQ(Write(&batch, &csv), RedshiftCsvWriteStatus::kSucceeded);
  EXPECT_EQ(csv, "3,30,\"third\"\n");
}

TEST(RedshiftCsvWriterTest, RejectsNullAndUnsupportedType) {
  auto batch = MakeBatch();
  ASSERT_EQ(ArrowArrayAppendInt(batch.array()->children[0], 1), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayAppendInt(batch.array()->children[1], 2), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayAppendNull(batch.array()->children[2], 1), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayFinishElement(batch.array()), NANOARROW_OK);
  batch.Finish();
  std::string csv = "original";
  EXPECT_EQ(Write(&batch, &csv), RedshiftCsvWriteStatus::kNullValue);
  EXPECT_EQ(csv, "original");

  CsvBatch unsupported({NANOARROW_TYPE_INT32, NANOARROW_TYPE_INT64, NANOARROW_TYPE_BOOL},
                       {"id", "big", "label"});
  ASSERT_EQ(ArrowArrayAppendInt(unsupported.array()->children[0], 1), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayAppendInt(unsupported.array()->children[1], 2), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayAppendInt(unsupported.array()->children[2], 1), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayFinishElement(unsupported.array()), NANOARROW_OK);
  unsupported.Finish();
  EXPECT_EQ(Write(&unsupported, &csv), RedshiftCsvWriteStatus::kUnsupportedType);
  EXPECT_EQ(csv, "original");
}

TEST(RedshiftCsvWriterTest, RejectsInvalidUtf8NulAndDefaultNullMarker) {
  for (const auto& [text, status] :
       std::vector<std::pair<std::string, RedshiftCsvWriteStatus>>{
           {std::string("a\0b", 3), RedshiftCsvWriteStatus::kInvalidText},
           {std::string("\xc0\xaf", 2), RedshiftCsvWriteStatus::kInvalidText},
           {std::string("\xed\xa0\x80", 3), RedshiftCsvWriteStatus::kInvalidText},
           {std::string("\xf4\x90\x80\x80", 4), RedshiftCsvWriteStatus::kInvalidText},
           {"\\N", RedshiftCsvWriteStatus::kAmbiguousNullMarker}}) {
    auto batch = MakeBatch();
    batch.AppendRow(1, 2, text);
    batch.Finish();
    std::string csv = "original";
    EXPECT_EQ(Write(&batch, &csv), status);
    EXPECT_EQ(csv, "original");
  }
}

TEST(RedshiftCsvWriterTest, RejectsMalformedOffsets) {
  auto batch = MakeBatch();
  batch.AppendRow(1, 2, "abc");
  batch.AppendRow(3, 4, "def");
  batch.Finish();
  auto* offsets =
      reinterpret_cast<int32_t*>(ArrowArrayBuffer(batch.array()->children[2], 1)->data);
  offsets[1] = 7;
  std::string csv = "original";
  EXPECT_EQ(Write(&batch, &csv), RedshiftCsvWriteStatus::kMalformedArrow);
  EXPECT_EQ(csv, "original");
}

TEST(RedshiftCsvWriterTest, RejectsPayloadLargerThanCoordinatorBound) {
  auto batch = MakeBatch();
  constexpr std::size_t kMaxRowBytes = 4'000'000;
  batch.AppendRow(1, 2, std::string(kMaxRowBytes - 7, 'x'));
  batch.AppendRow(1, 2, std::string(kMaxRowBytes - 7, 'x'));
  batch.AppendRow(
      1, 2, std::string(kRedshiftStagedCopyMaxPayloadBytes - 2 * kMaxRowBytes - 6, 'x'));
  batch.Finish();
  std::string csv = "original";
  EXPECT_EQ(Write(&batch, &csv), RedshiftCsvWriteStatus::kPayloadTooLarge);
  EXPECT_EQ(csv, "original");
}

TEST(RedshiftCsvWriterTest, AcceptsExactlyCoordinatorPayloadBound) {
  auto batch = MakeBatch();
  constexpr std::size_t kMaxRowBytes = 4'000'000;
  // 1,2,"..."\n adds seven bytes around each simple string.
  batch.AppendRow(1, 2, std::string(kMaxRowBytes - 7, 'x'));
  batch.AppendRow(1, 2, std::string(kMaxRowBytes - 7, 'x'));
  batch.AppendRow(
      1, 2, std::string(kRedshiftStagedCopyMaxPayloadBytes - 2 * kMaxRowBytes - 7, 'x'));
  batch.Finish();
  std::string csv;
  EXPECT_EQ(Write(&batch, &csv), RedshiftCsvWriteStatus::kSucceeded);
  EXPECT_EQ(csv.size(), kRedshiftStagedCopyMaxPayloadBytes);
}

TEST(RedshiftCsvWriterTest, RejectsSingleRowOverRedshiftCopyLimit) {
  auto batch = MakeBatch();
  // The complete CSV row is 4,000,001 bytes, below the 8 MiB object cap.
  batch.AppendRow(1, 2, std::string(4'000'000 - 6, 'x'));
  batch.Finish();
  std::string csv = "original";
  EXPECT_EQ(Write(&batch, &csv), RedshiftCsvWriteStatus::kRowTooLarge);
  EXPECT_EQ(csv, "original");
}

}  // namespace
}  // namespace adbc::driver::pgwire
