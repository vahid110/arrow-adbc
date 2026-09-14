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

#include "postgresql/redshift_staged_arrow_copy.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.hpp>

namespace adbc::driver::pgwire {
namespace {

constexpr std::string_view kDataUrl = "s3://pgwire-ci/staging/run-01/data.csv";
constexpr std::string_view kManifestUrl = "s3://pgwire-ci/staging/run-01/load.manifest";
constexpr std::string_view kRoleArn =
    "arn:aws:iam::149112076833:role/adbc-pgwire-ci-copy";

class ArrowBatch {
 public:
  ArrowBatch() {
    EXPECT_EQ(ArrowSchemaInitFromType(schema_.get(), NANOARROW_TYPE_STRUCT),
              NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetTypeStruct(schema_.get(), 2), NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetType(schema_->children[0], NANOARROW_TYPE_INT32),
              NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetName(schema_->children[0], "id"), NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetType(schema_->children[1], NANOARROW_TYPE_STRING),
              NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetName(schema_->children[1], "label"), NANOARROW_OK);
    EXPECT_EQ(ArrowArrayInitFromSchema(array_.get(), schema_.get(), nullptr),
              NANOARROW_OK);
    EXPECT_EQ(ArrowArrayStartAppending(array_.get()), NANOARROW_OK);
  }

  void AppendRow(int32_t id, std::string_view label) {
    ASSERT_EQ(ArrowArrayAppendInt(array_->children[0], id), NANOARROW_OK);
    const ArrowStringView text = {label.data(), static_cast<int64_t>(label.size())};
    ASSERT_EQ(ArrowArrayAppendString(array_->children[1], text), NANOARROW_OK);
    ASSERT_EQ(ArrowArrayFinishElement(array_.get()), NANOARROW_OK);
  }

  void AppendNullRow(int32_t id) {
    ASSERT_EQ(ArrowArrayAppendInt(array_->children[0], id), NANOARROW_OK);
    ASSERT_EQ(ArrowArrayAppendNull(array_->children[1], 1), NANOARROW_OK);
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

RedshiftStagedArrowCopyRequest MakeRequest(ArrowBatch* batch) {
  return {batch->schema(), batch->array(), "public", "my_table", {"id", "label"},
          kDataUrl,        kManifestUrl,   kRoleArn, "run-01"};
}

class FakeObjectStore : public RedshiftStagedObjectStore {
 public:
  struct Object {
    std::string bytes;
    std::string owner;
  };

  RedshiftStagedPutResult PutIfAbsent(std::string_view uri, std::string_view bytes,
                                      std::string_view token) noexcept override {
    const std::string key(uri);
    events.push_back(key == kDataUrl ? "put:data" : "put:manifest");
    if (objects.count(key) != 0) return RedshiftStagedPutResult::kCollision;
    objects.emplace(key, Object{std::string(bytes), std::string(token)});
    return RedshiftStagedPutResult::kCreated;
  }

  RedshiftStagedOwnership CheckOwnership(std::string_view uri,
                                         std::string_view token) noexcept override {
    const auto found = objects.find(std::string(uri));
    return found != objects.end() && found->second.owner == token
               ? RedshiftStagedOwnership::kOwned
               : RedshiftStagedOwnership::kOtherOrAbsent;
  }

  bool DeleteIfOwned(std::string_view uri, std::string_view token) noexcept override {
    const std::string key(uri);
    events.push_back(key == kDataUrl ? "delete:data" : "delete:manifest");
    const auto found = objects.find(key);
    if (found == objects.end()) return true;
    if (found->second.owner != token) return false;
    objects.erase(found);
    return true;
  }

  std::unordered_map<std::string, Object> objects;
  std::vector<std::string> events;
};

TEST(RedshiftStagedArrowCopyTest, WritesExactBytesThenCopiesAndCleansOwnedObjects) {
  ArrowBatch batch;
  batch.AppendRow(7, "a,b\n\"x\"");
  batch.AppendRow(-8, "");
  batch.Finish();
  FakeObjectStore store;
  std::string copy_sql;
  const auto result =
      RunRedshiftStagedArrowCopy(MakeRequest(&batch), store, [&](std::string_view sql) {
        store.events.push_back("copy");
        copy_sql = sql;
        EXPECT_EQ(store.objects.at(std::string(kDataUrl)).bytes,
                  "7,\"a,b\n\"\"x\"\"\"\n-8,\"\"\n");
        EXPECT_EQ(store.objects.at(std::string(kManifestUrl)).bytes,
                  "{\"entries\":[{\"url\":\"s3://pgwire-ci/staging/run-01/data.csv\","
                  "\"mandatory\":true}]}");
        return RedshiftCopyExecutionResult::kSucceeded;
      });
  EXPECT_EQ(result.csv_status, RedshiftCsvWriteStatus::kSucceeded);
  ASSERT_TRUE(result.staged_copy.has_value());
  EXPECT_EQ(result.staged_copy->status, RedshiftStagedCopyRunStatus::kSucceeded);
  EXPECT_TRUE(result.staged_copy->copy_succeeded);
  EXPECT_TRUE(result.staged_copy->cleanup_complete);
  EXPECT_TRUE(store.objects.empty());
  EXPECT_EQ(store.events, (std::vector<std::string>{"put:data", "put:manifest", "copy",
                                                    "delete:manifest", "delete:data"}));
  EXPECT_EQ(copy_sql,
            "COPY \"public\".\"my_table\" (\"id\", \"label\") FROM "
            "'s3://pgwire-ci/staging/run-01/load.manifest' IAM_ROLE "
            "'arn:aws:iam::149112076833:role/adbc-pgwire-ci-copy' MANIFEST CSV");
}

TEST(RedshiftStagedArrowCopyTest, CsvFailuresHaveNoExternalSideEffects) {
  ArrowBatch batch;
  batch.AppendNullRow(1);
  batch.Finish();
  FakeObjectStore store;
  int copy_calls = 0;
  const auto execute_copy = [&](std::string_view) {
    ++copy_calls;
    return RedshiftCopyExecutionResult::kSucceeded;
  };

  const auto null_result =
      RunRedshiftStagedArrowCopy(MakeRequest(&batch), store, execute_copy);
  EXPECT_EQ(null_result.csv_status, RedshiftCsvWriteStatus::kNullValue);
  EXPECT_FALSE(null_result.staged_copy.has_value());

  auto mismatch_request = MakeRequest(&batch);
  mismatch_request.columns = {"label", "id"};
  const auto mismatch_result =
      RunRedshiftStagedArrowCopy(mismatch_request, store, execute_copy);
  EXPECT_EQ(mismatch_result.csv_status, RedshiftCsvWriteStatus::kColumnMismatch);
  EXPECT_FALSE(mismatch_result.staged_copy.has_value());

  auto missing_array = MakeRequest(&batch);
  missing_array.arrow_array = nullptr;
  const auto invalid_result =
      RunRedshiftStagedArrowCopy(missing_array, store, execute_copy);
  EXPECT_EQ(invalid_result.csv_status, RedshiftCsvWriteStatus::kInvalidInput);
  EXPECT_FALSE(invalid_result.staged_copy.has_value());

  ArrowBatch ambiguous;
  ambiguous.AppendRow(1, "\\N");
  ambiguous.Finish();
  const auto ambiguous_result =
      RunRedshiftStagedArrowCopy(MakeRequest(&ambiguous), store, execute_copy);
  EXPECT_EQ(ambiguous_result.csv_status, RedshiftCsvWriteStatus::kAmbiguousNullMarker);
  EXPECT_FALSE(ambiguous_result.staged_copy.has_value());

  ArrowBatch oversized;
  oversized.AppendRow(1, std::string(4'000'000 - 5, 'x'));
  oversized.AppendRow(2, std::string(4'000'000 - 5, 'x'));
  oversized.AppendRow(
      3, std::string(kRedshiftStagedCopyMaxPayloadBytes - 2 * 4'000'000 - 4, 'x'));
  oversized.Finish();
  const auto oversized_result =
      RunRedshiftStagedArrowCopy(MakeRequest(&oversized), store, execute_copy);
  EXPECT_EQ(oversized_result.csv_status, RedshiftCsvWriteStatus::kPayloadTooLarge);
  EXPECT_FALSE(oversized_result.staged_copy.has_value());

  ArrowBatch oversized_row;
  // A 4,000,001-byte row must fail before any S3 or COPY action.
  oversized_row.AppendRow(1, std::string(4'000'000 - 4, 'x'));
  oversized_row.Finish();
  const auto oversized_row_result =
      RunRedshiftStagedArrowCopy(MakeRequest(&oversized_row), store, execute_copy);
  EXPECT_EQ(oversized_row_result.csv_status, RedshiftCsvWriteStatus::kRowTooLarge);
  EXPECT_FALSE(oversized_row_result.staged_copy.has_value());
  EXPECT_TRUE(store.events.empty());
  EXPECT_EQ(copy_calls, 0);
}

TEST(RedshiftStagedArrowCopyTest, SlicedNullBitmapHasNoExternalSideEffects) {
  ArrowBatch batch;
  batch.AppendRow(1, "first");
  batch.AppendRow(2, "second");
  batch.AppendNullRow(3);
  batch.Finish();

  // Select the third logical row through both a parent and child slice. The
  // child's validity bit is at the sum of the two offsets, not either alone.
  batch.array()->offset = 1;
  batch.array()->length = 1;
  for (int64_t i = 0; i < batch.array()->n_children; ++i) {
    batch.array()->children[i]->offset = 1;
    batch.array()->children[i]->length = 2;
  }

  FakeObjectStore store;
  int copy_calls = 0;
  const auto result =
      RunRedshiftStagedArrowCopy(MakeRequest(&batch), store, [&](std::string_view) {
        ++copy_calls;
        return RedshiftCopyExecutionResult::kSucceeded;
      });
  EXPECT_EQ(result.csv_status, RedshiftCsvWriteStatus::kNullValue);
  EXPECT_FALSE(result.staged_copy.has_value());
  EXPECT_TRUE(store.events.empty());
  EXPECT_TRUE(store.objects.empty());
  EXPECT_EQ(copy_calls, 0);
}

TEST(RedshiftStagedArrowCopyTest, InvalidPlanIsRejectedBeforeUpload) {
  ArrowBatch batch;
  batch.AppendRow(1, "one");
  batch.Finish();
  FakeObjectStore store;
  auto request = MakeRequest(&batch);
  request.ownership_token = "wrong-token";
  int copy_calls = 0;
  const auto result = RunRedshiftStagedArrowCopy(request, store, [&](std::string_view) {
    ++copy_calls;
    return RedshiftCopyExecutionResult::kSucceeded;
  });
  EXPECT_EQ(result.csv_status, RedshiftCsvWriteStatus::kSucceeded);
  ASSERT_TRUE(result.staged_copy.has_value());
  EXPECT_EQ(result.staged_copy->status, RedshiftStagedCopyRunStatus::kInvalidInput);
  EXPECT_TRUE(store.events.empty());
  EXPECT_EQ(copy_calls, 0);
}

TEST(RedshiftStagedArrowCopyTest, CollisionLeavesForeignObjectUntouched) {
  ArrowBatch batch;
  batch.AppendRow(1, "one");
  batch.Finish();
  FakeObjectStore store;
  store.objects.emplace(std::string(kDataUrl),
                        FakeObjectStore::Object{"foreign", "other-owner"});
  int copy_calls = 0;
  const auto result =
      RunRedshiftStagedArrowCopy(MakeRequest(&batch), store, [&](std::string_view) {
        ++copy_calls;
        return RedshiftCopyExecutionResult::kSucceeded;
      });
  EXPECT_EQ(result.csv_status, RedshiftCsvWriteStatus::kSucceeded);
  ASSERT_TRUE(result.staged_copy.has_value());
  EXPECT_EQ(result.staged_copy->status, RedshiftStagedCopyRunStatus::kUploadCollision);
  EXPECT_EQ(store.objects.at(std::string(kDataUrl)).bytes, "foreign");
  EXPECT_EQ(store.events, (std::vector<std::string>{"put:data"}));
  EXPECT_EQ(copy_calls, 0);
}

TEST(RedshiftStagedArrowCopyTest, AmbiguousSettledCopyIsNotRetried) {
  ArrowBatch batch;
  batch.AppendRow(1, "one");
  batch.Finish();
  FakeObjectStore store;
  int copy_calls = 0;
  const auto result =
      RunRedshiftStagedArrowCopy(MakeRequest(&batch), store, [&](std::string_view) {
        store.events.push_back("copy");
        ++copy_calls;
        return RedshiftCopyExecutionResult::kUnknown;
      });
  EXPECT_EQ(result.csv_status, RedshiftCsvWriteStatus::kSucceeded);
  ASSERT_TRUE(result.staged_copy.has_value());
  EXPECT_EQ(result.staged_copy->status, RedshiftStagedCopyRunStatus::kCopyUnknown);
  EXPECT_FALSE(result.staged_copy->copy_succeeded);
  EXPECT_TRUE(result.staged_copy->cleanup_complete);
  EXPECT_EQ(copy_calls, 1);
  EXPECT_TRUE(store.objects.empty());
}

}  // namespace
}  // namespace adbc::driver::pgwire
