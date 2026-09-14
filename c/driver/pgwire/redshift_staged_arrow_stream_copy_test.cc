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

#include "postgresql/redshift_staged_arrow_stream_copy.h"

#include <cerrno>
#include <cstdint>
#include <functional>
#include <optional>
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

struct Row {
  int32_t id;
  std::optional<std::string> label;
};

using Batch = std::vector<Row>;

class TestStream {
 public:
  explicit TestStream(std::vector<Batch> batches, int fail_next_call = 0,
                      bool malformed_first_empty = false) {
    nanoarrow::UniqueSchema schema;
    EXPECT_EQ(ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_STRUCT), NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 2), NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32),
              NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetName(schema->children[0], "id"), NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_STRING),
              NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetName(schema->children[1], "label"), NANOARROW_OK);

    std::vector<nanoarrow::UniqueArray> arrays(batches.size());
    for (std::size_t i = 0; i < batches.size(); ++i) {
      EXPECT_EQ(ArrowArrayInitFromSchema(arrays[i].get(), schema.get(), nullptr),
                NANOARROW_OK);
      EXPECT_EQ(ArrowArrayStartAppending(arrays[i].get()), NANOARROW_OK);
      for (const Row& row : batches[i]) {
        EXPECT_EQ(ArrowArrayAppendInt(arrays[i]->children[0], row.id), NANOARROW_OK);
        if (row.label) {
          const ArrowStringView text = {row.label->data(),
                                        static_cast<int64_t>(row.label->size())};
          EXPECT_EQ(ArrowArrayAppendString(arrays[i]->children[1], text), NANOARROW_OK);
        } else {
          EXPECT_EQ(ArrowArrayAppendNull(arrays[i]->children[1], 1), NANOARROW_OK);
        }
        EXPECT_EQ(ArrowArrayFinishElement(arrays[i].get()), NANOARROW_OK);
      }
      EXPECT_EQ(ArrowArrayFinishBuildingDefault(arrays[i].get(), nullptr), NANOARROW_OK);
    }

    nanoarrow::UniqueArrayStream basic;
    EXPECT_EQ(ArrowBasicArrayStreamInit(basic.get(), schema.get(), batches.size()),
              NANOARROW_OK);
    for (std::size_t i = 0; i < batches.size(); ++i) {
      ArrowBasicArrayStreamSetArray(basic.get(), i, arrays[i].get());
    }
    auto* state = new State;
    state->basic.reset(basic.get());
    state->stream_releases = &stream_releases;
    state->schema_releases = &schema_releases;
    state->array_releases = &array_releases;
    state->fail_next_call = fail_next_call;
    state->malformed_first_empty = malformed_first_empty;
    stream_->get_schema = GetSchema;
    stream_->get_next = GetNext;
    stream_->get_last_error = GetLastError;
    stream_->release = Release;
    stream_->private_data = state;
  }

  ArrowArrayStream* get() { return stream_.get(); }

  int stream_releases = 0;
  int schema_releases = 0;
  int array_releases = 0;

 private:
  struct State {
    nanoarrow::UniqueArrayStream basic;
    int* stream_releases;
    int* schema_releases;
    int* array_releases;
    int fail_next_call;
    bool malformed_first_empty;
    int next_calls = 0;
  };

  struct ArrayReleaseState {
    void (*original_release)(ArrowArray*);
    void* original_private_data;
    int* array_releases;
    int64_t original_n_children;
  };

  struct SchemaReleaseState {
    void (*original_release)(ArrowSchema*);
    void* original_private_data;
    int* schema_releases;
  };

  static State* GetState(ArrowArrayStream* self) {
    return static_cast<State*>(self->private_data);
  }

  static int GetSchema(ArrowArrayStream* self, ArrowSchema* out) {
    State* state = GetState(self);
    const int status = state->basic->get_schema(state->basic.get(), out);
    if (status == NANOARROW_OK && out->release) {
      auto* release_state =
          new SchemaReleaseState{out->release, out->private_data, state->schema_releases};
      out->private_data = release_state;
      out->release = ReleaseSchema;
    }
    return status;
  }

  static int GetNext(ArrowArrayStream* self, ArrowArray* out) {
    State* state = GetState(self);
    if (++state->next_calls == state->fail_next_call) return EIO;
    const int status = state->basic->get_next(state->basic.get(), out);
    if (status == NANOARROW_OK && out->release) {
      auto* release_state = new ArrayReleaseState{out->release, out->private_data,
                                                  state->array_releases, out->n_children};
      out->private_data = release_state;
      out->release = ReleaseArray;
      if (state->malformed_first_empty && state->next_calls == 1 && out->length == 0) {
        out->n_children = 0;
      }
    }
    return status;
  }

  static const char* GetLastError(ArrowArrayStream*) { return "injected stream error"; }

  static void ReleaseSchema(ArrowSchema* schema) {
    auto* state = static_cast<SchemaReleaseState*>(schema->private_data);
    ++*state->schema_releases;
    schema->release = state->original_release;
    schema->private_data = state->original_private_data;
    delete state;
    schema->release(schema);
  }

  static void ReleaseArray(ArrowArray* array) {
    auto* state = static_cast<ArrayReleaseState*>(array->private_data);
    ++*state->array_releases;
    array->release = state->original_release;
    array->private_data = state->original_private_data;
    array->n_children = state->original_n_children;
    delete state;
    array->release(array);
  }

  static void Release(ArrowArrayStream* self) {
    State* state = GetState(self);
    ++*state->stream_releases;
    delete state;
    self->release = nullptr;
  }

  nanoarrow::UniqueArrayStream stream_;
};

RedshiftStagedArrowStreamCopyRequest MakeRequest(TestStream* stream) {
  return {stream->get(), "public",     "my_table", {"id", "label"},
          kDataUrl,      kManifestUrl, kRoleArn,   "run-01"};
}

class FakeObjectStore : public RedshiftStagedObjectStore {
 public:
  RedshiftStagedPutResult PutIfAbsent(std::string_view uri, std::string_view bytes,
                                      std::string_view token) noexcept override {
    if (events.empty() && before_first_put) before_first_put();
    events.push_back(uri == kDataUrl ? "put:data" : "put:manifest");
    objects.emplace(std::string(uri), std::string(bytes));
    owners.emplace(std::string(uri), std::string(token));
    return RedshiftStagedPutResult::kCreated;
  }

  RedshiftStagedOwnership CheckOwnership(std::string_view uri,
                                         std::string_view token) noexcept override {
    return owners.at(std::string(uri)) == token ? RedshiftStagedOwnership::kOwned
                                                : RedshiftStagedOwnership::kOtherOrAbsent;
  }

  bool DeleteIfOwned(std::string_view uri, std::string_view token) noexcept override {
    events.push_back(uri == kDataUrl ? "delete:data" : "delete:manifest");
    if (owners.at(std::string(uri)) != token) return false;
    owners.erase(std::string(uri));
    objects.erase(std::string(uri));
    return true;
  }

  std::unordered_map<std::string, std::string> objects;
  std::unordered_map<std::string, std::string> owners;
  std::vector<std::string> events;
  std::function<void()> before_first_put;
};

TEST(RedshiftStagedArrowStreamCopyTest, TwoBatchesStageOneExactObjectAndCopyOnce) {
  TestStream stream({{{7, "a,b"}}, {{-8, ""}}});
  FakeObjectStore store;
  store.before_first_put = [&] {
    EXPECT_EQ(stream.get()->release, nullptr);
    EXPECT_EQ(stream.stream_releases, 1);
    EXPECT_EQ(stream.schema_releases, 1);
    EXPECT_EQ(stream.array_releases, 2);
  };
  int copy_calls = 0;
  const auto result = RunRedshiftStagedArrowStreamCopy(
      MakeRequest(&stream), store, [&](std::string_view sql) {
        ++copy_calls;
        EXPECT_EQ(stream.stream_releases, 1);
        EXPECT_EQ(stream.array_releases, 2);
        EXPECT_EQ(store.objects.at(std::string(kDataUrl)), "7,\"a,b\"\n-8,\"\"\n");
        EXPECT_EQ(store.objects.at(std::string(kManifestUrl)),
                  "{\"entries\":[{\"url\":\"s3://pgwire-ci/staging/run-01/data.csv\","
                  "\"mandatory\":true}]}");
        EXPECT_EQ(sql,
                  "COPY \"public\".\"my_table\" (\"id\", \"label\") FROM "
                  "'s3://pgwire-ci/staging/run-01/load.manifest' IAM_ROLE "
                  "'arn:aws:iam::149112076833:role/adbc-pgwire-ci-copy' MANIFEST CSV");
        return RedshiftCopyExecutionResult::kSucceeded;
      });
  EXPECT_EQ(result.preflight_status,
            RedshiftStagedArrowStreamPreflightStatus::kSucceeded);
  ASSERT_TRUE(result.staged_copy);
  EXPECT_EQ(result.staged_copy->status, RedshiftStagedCopyRunStatus::kSucceeded);
  EXPECT_EQ(copy_calls, 1);
  EXPECT_EQ(stream.get()->release, nullptr);
  EXPECT_EQ(stream.stream_releases, 1);
  EXPECT_EQ(stream.schema_releases, 1);
  EXPECT_EQ(stream.array_releases, 2);
  EXPECT_TRUE(store.objects.empty());
  EXPECT_EQ(store.events, (std::vector<std::string>{"put:data", "put:manifest",
                                                    "delete:manifest", "delete:data"}));
}

TEST(RedshiftStagedArrowStreamCopyTest, LaterNullRejectsBeforeAnyExternalCall) {
  TestStream stream({{{1, "valid"}}, {{2, std::nullopt}}});
  FakeObjectStore store;
  int copy_calls = 0;
  const auto result = RunRedshiftStagedArrowStreamCopy(
      MakeRequest(&stream), store, [&](std::string_view) {
        ++copy_calls;
        return RedshiftCopyExecutionResult::kSucceeded;
      });
  EXPECT_EQ(result.preflight_status,
            RedshiftStagedArrowStreamPreflightStatus::kCsvRejected);
  ASSERT_TRUE(result.csv_status);
  EXPECT_EQ(*result.csv_status, RedshiftCsvWriteStatus::kNullValue);
  EXPECT_FALSE(result.staged_copy);
  EXPECT_TRUE(store.events.empty());
  EXPECT_EQ(copy_calls, 0);
  EXPECT_EQ(stream.stream_releases, 1);
  EXPECT_EQ(stream.array_releases, 2);
}

TEST(RedshiftStagedArrowStreamCopyTest, MalformedEmptyBatchRejectsBeforeUpload) {
  TestStream stream({{}, {{1, "valid"}}}, 0, true);
  FakeObjectStore store;
  int copy_calls = 0;
  const auto result = RunRedshiftStagedArrowStreamCopy(
      MakeRequest(&stream), store, [&](std::string_view) {
        ++copy_calls;
        return RedshiftCopyExecutionResult::kSucceeded;
      });
  EXPECT_EQ(result.preflight_status,
            RedshiftStagedArrowStreamPreflightStatus::kCsvRejected);
  ASSERT_TRUE(result.csv_status);
  EXPECT_EQ(*result.csv_status, RedshiftCsvWriteStatus::kMalformedArrow);
  EXPECT_FALSE(result.staged_copy);
  EXPECT_TRUE(store.events.empty());
  EXPECT_EQ(copy_calls, 0);
  EXPECT_EQ(stream.stream_releases, 1);
  EXPECT_EQ(stream.array_releases, 1);
}

TEST(RedshiftStagedArrowStreamCopyTest, LaterReadErrorRejectsAndReleasesStream) {
  TestStream stream({{{1, "valid"}}, {{2, "unread"}}}, 2);
  FakeObjectStore store;
  int copy_calls = 0;
  const auto result = RunRedshiftStagedArrowStreamCopy(
      MakeRequest(&stream), store, [&](std::string_view) {
        ++copy_calls;
        return RedshiftCopyExecutionResult::kSucceeded;
      });
  EXPECT_EQ(result.preflight_status,
            RedshiftStagedArrowStreamPreflightStatus::kStreamError);
  EXPECT_FALSE(result.staged_copy);
  EXPECT_TRUE(store.events.empty());
  EXPECT_EQ(copy_calls, 0);
  EXPECT_EQ(stream.stream_releases, 1);
  EXPECT_EQ(stream.array_releases, 1);
}

TEST(RedshiftStagedArrowStreamCopyTest, AggregateLimitRejectsBeforeUpload) {
  TestStream stream({{{1, std::string(3'999'995, 'a')}},
                     {{2, std::string(3'999'995, 'b')}},
                     {{3, std::string(388'604, 'c')}}});
  FakeObjectStore store;
  int copy_calls = 0;
  const auto result = RunRedshiftStagedArrowStreamCopy(
      MakeRequest(&stream), store, [&](std::string_view) {
        ++copy_calls;
        return RedshiftCopyExecutionResult::kSucceeded;
      });
  EXPECT_EQ(result.preflight_status,
            RedshiftStagedArrowStreamPreflightStatus::kPayloadTooLarge);
  EXPECT_FALSE(result.staged_copy);
  EXPECT_TRUE(store.events.empty());
  EXPECT_EQ(copy_calls, 0);
  EXPECT_EQ(stream.stream_releases, 1);
  EXPECT_EQ(stream.array_releases, 3);
}

TEST(RedshiftStagedArrowStreamCopyTest, EmptyAndUnboundedStreamsAreNotUploaded) {
  TestStream stream(std::vector<Batch>{{}, {}});
  FakeObjectStore store;
  const auto result = RunRedshiftStagedArrowStreamCopy(
      MakeRequest(&stream), store,
      [](std::string_view) { return RedshiftCopyExecutionResult::kSucceeded; });
  EXPECT_EQ(result.preflight_status,
            RedshiftStagedArrowStreamPreflightStatus::kEmptyStream);
  EXPECT_FALSE(result.staged_copy);
  EXPECT_TRUE(store.events.empty());
  EXPECT_EQ(stream.stream_releases, 1);
  EXPECT_EQ(stream.array_releases, 2);

  TestStream unbounded(std::vector<Batch>(129));
  const auto bounded_result = RunRedshiftStagedArrowStreamCopy(
      MakeRequest(&unbounded), store,
      [](std::string_view) { return RedshiftCopyExecutionResult::kSucceeded; });
  EXPECT_EQ(bounded_result.preflight_status,
            RedshiftStagedArrowStreamPreflightStatus::kTooManyBatches);
  EXPECT_FALSE(bounded_result.staged_copy);
  EXPECT_TRUE(store.events.empty());
  EXPECT_EQ(unbounded.stream_releases, 1);
  EXPECT_EQ(unbounded.array_releases, 129);
}

TEST(RedshiftStagedArrowStreamCopyTest, InvalidHandleDoesNotTransferOwnership) {
  TestStream stream({{{1, "unread"}}});
  FakeObjectStore store;
  ArrowArrayStream* handle = stream.get();
  auto* get_last_error = handle->get_last_error;
  handle->get_last_error = nullptr;
  const auto result = RunRedshiftStagedArrowStreamCopy(
      MakeRequest(&stream), store,
      [](std::string_view) { return RedshiftCopyExecutionResult::kSucceeded; });
  EXPECT_EQ(result.preflight_status,
            RedshiftStagedArrowStreamPreflightStatus::kInvalidInput);
  EXPECT_FALSE(result.staged_copy);
  EXPECT_TRUE(store.events.empty());
  EXPECT_NE(handle->release, nullptr);
  EXPECT_EQ(stream.stream_releases, 0);
  EXPECT_EQ(stream.array_releases, 0);
  handle->get_last_error = get_last_error;
}

}  // namespace
}  // namespace adbc::driver::pgwire
