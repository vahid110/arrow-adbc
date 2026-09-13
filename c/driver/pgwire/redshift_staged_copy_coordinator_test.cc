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

#include "postgresql/redshift_staged_copy_coordinator.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace adbc::driver::pgwire {
namespace {

constexpr std::string_view kDataUrl = "s3://pgwire-ci/staging/run-01/data.csv";
constexpr std::string_view kManifestUrl = "s3://pgwire-ci/staging/run-01/load.manifest";
constexpr std::string_view kRoleArn =
    "arn:aws:iam::149112076833:role/adbc-pgwire-ci-copy";

RedshiftStagedCopyRequest MakeRequest() {
  return {.schema = "public",
          .table = "my_table",
          .columns = {"label", "id"},
          .data_s3_url = kDataUrl,
          .manifest_s3_url = kManifestUrl,
          .iam_role_arn = kRoleArn,
          .ownership_token = "run-01",
          .serialized_csv = "hello,1\n"};
}

class FakeObjectStore : public RedshiftStagedObjectStore {
 public:
  struct Object {
    std::string bytes;
    std::string token;
  };

  RedshiftStagedPutResult PutIfAbsent(std::string_view uri, std::string_view bytes,
                                      std::string_view token) noexcept override {
    const std::string key(uri);
    events.push_back(key == kDataUrl ? "put:data" : "put:manifest");
    const auto configured = put_results.find(key);
    if (configured != put_results.end()) {
      switch (configured->second) {
        case RedshiftStagedPutResult::kCreated:
          break;
        case RedshiftStagedPutResult::kCollision:
          objects.emplace(key, Object{"foreign", "someone-else"});
          return RedshiftStagedPutResult::kCollision;
        case RedshiftStagedPutResult::kFailed:
          return RedshiftStagedPutResult::kFailed;
        case RedshiftStagedPutResult::kUnknown:
          if (unknown_creates.count(key) != 0) {
            objects.emplace(key, Object{std::string(bytes), std::string(token)});
          }
          return RedshiftStagedPutResult::kUnknown;
      }
    }
    if (objects.count(key) != 0) return RedshiftStagedPutResult::kCollision;
    objects.emplace(key, Object{std::string(bytes), std::string(token)});
    return RedshiftStagedPutResult::kCreated;
  }

  RedshiftStagedOwnership CheckOwnership(std::string_view uri,
                                         std::string_view token) noexcept override {
    const std::string key(uri);
    events.push_back(key == kDataUrl ? "check:data" : "check:manifest");
    if (unresolved_ownership.count(key) != 0) return RedshiftStagedOwnership::kUnknown;
    const auto found = objects.find(key);
    if (found != objects.end() && found->second.token == token) {
      return RedshiftStagedOwnership::kOwned;
    }
    return RedshiftStagedOwnership::kOtherOrAbsent;
  }

  bool DeleteIfOwned(std::string_view uri, std::string_view token) noexcept override {
    const std::string key(uri);
    events.push_back(key == kDataUrl ? "delete:data" : "delete:manifest");
    if (failed_deletes.count(key) != 0) return false;
    const auto found = objects.find(key);
    if (found == objects.end()) return true;
    if (found->second.token != token) return false;
    objects.erase(found);
    return true;
  }

  std::unordered_map<std::string, Object> objects;
  std::unordered_map<std::string, RedshiftStagedPutResult> put_results;
  std::unordered_set<std::string> unknown_creates;
  std::unordered_set<std::string> unresolved_ownership;
  std::unordered_set<std::string> failed_deletes;
  std::vector<std::string> events;
};

RedshiftStagedCopyRunResult RunWithCopyResult(
    const RedshiftStagedCopyRequest& request, FakeObjectStore& store,
    RedshiftCopyExecutionResult copy_result = RedshiftCopyExecutionResult::kSucceeded) {
  return RunRedshiftStagedCopy(request, store, [&](std::string_view) {
    store.events.push_back("copy");
    return copy_result;
  });
}

TEST(RedshiftStagedCopyCoordinatorTest, ValidatesBeforeAnyExternalAction) {
  FakeObjectStore store;
  auto request = MakeRequest();
  request.ownership_token = "wrong";
  EXPECT_EQ(RunWithCopyResult(request, store).status,
            RedshiftStagedCopyRunStatus::kInvalidInput);
  request = MakeRequest();
  request.manifest_s3_url = "s3://pgwire-ci/staging/other/load.manifest";
  EXPECT_EQ(RunWithCopyResult(request, store).status,
            RedshiftStagedCopyRunStatus::kInvalidInput);
  request = MakeRequest();
  request.columns = {"id", "Id"};
  EXPECT_EQ(RunWithCopyResult(request, store).status,
            RedshiftStagedCopyRunStatus::kInvalidInput);
  request = MakeRequest();
  request.serialized_csv = "";
  EXPECT_EQ(RunWithCopyResult(request, store).status,
            RedshiftStagedCopyRunStatus::kInvalidInput);
  const std::string oversized_csv(kRedshiftStagedCopyMaxPayloadBytes + 1, 'x');
  request = MakeRequest();
  request.serialized_csv = oversized_csv;
  EXPECT_EQ(RunWithCopyResult(request, store).status,
            RedshiftStagedCopyRunStatus::kInvalidInput);
  request = MakeRequest();
  EXPECT_EQ(RunRedshiftStagedCopy(request, store, {}).status,
            RedshiftStagedCopyRunStatus::kInvalidInput);
  EXPECT_TRUE(store.events.empty());
}

TEST(RedshiftStagedCopyCoordinatorTest, RunsManifestCopyAndExactReverseCleanup) {
  FakeObjectStore store;
  std::string copy_sql;
  const auto result =
      RunRedshiftStagedCopy(MakeRequest(), store, [&](std::string_view sql) {
        store.events.push_back("copy");
        copy_sql = sql;
        const auto manifest = store.objects.find(std::string(kManifestUrl));
        EXPECT_NE(manifest, store.objects.end());
        if (manifest != store.objects.end()) {
          EXPECT_EQ(manifest->second.bytes,
                    "{\"entries\":[{\"url\":\"s3://pgwire-ci/staging/run-01/data.csv\","
                    "\"mandatory\":true}]}");
        }
        return RedshiftCopyExecutionResult::kSucceeded;
      });
  EXPECT_EQ(result.status, RedshiftStagedCopyRunStatus::kSucceeded);
  EXPECT_TRUE(result.copy_succeeded);
  EXPECT_TRUE(result.cleanup_complete);
  EXPECT_TRUE(store.objects.empty());
  EXPECT_EQ(store.events, (std::vector<std::string>{"put:data", "put:manifest", "copy",
                                                    "delete:manifest", "delete:data"}));
  EXPECT_EQ(copy_sql,
            "COPY \"public\".\"my_table\" (\"label\", \"id\") FROM "
            "'s3://pgwire-ci/staging/run-01/load.manifest' IAM_ROLE "
            "'arn:aws:iam::149112076833:role/adbc-pgwire-ci-copy' MANIFEST CSV");
}

TEST(RedshiftStagedCopyCoordinatorTest, DataCollisionNeverDeletesForeignObject) {
  FakeObjectStore store;
  store.put_results[std::string(kDataUrl)] = RedshiftStagedPutResult::kCollision;
  const auto result = RunWithCopyResult(MakeRequest(), store);
  EXPECT_EQ(result.status, RedshiftStagedCopyRunStatus::kUploadCollision);
  EXPECT_TRUE(result.cleanup_complete);
  EXPECT_EQ(store.events, (std::vector<std::string>{"put:data"}));
  EXPECT_EQ(store.objects.at(std::string(kDataUrl)).token, "someone-else");
}

TEST(RedshiftStagedCopyCoordinatorTest, ManifestCollisionDeletesOnlyPriorData) {
  FakeObjectStore store;
  store.put_results[std::string(kManifestUrl)] = RedshiftStagedPutResult::kCollision;
  const auto result = RunWithCopyResult(MakeRequest(), store);
  EXPECT_EQ(result.status, RedshiftStagedCopyRunStatus::kUploadCollision);
  EXPECT_TRUE(result.cleanup_complete);
  EXPECT_EQ(store.events,
            (std::vector<std::string>{"put:data", "put:manifest", "delete:data"}));
  EXPECT_EQ(store.objects.size(), 1);
  EXPECT_EQ(store.objects.at(std::string(kManifestUrl)).token, "someone-else");
}

TEST(RedshiftStagedCopyCoordinatorTest, CollisionStatusSurvivesPriorDataCleanupFailure) {
  FakeObjectStore store;
  store.put_results[std::string(kManifestUrl)] = RedshiftStagedPutResult::kCollision;
  store.failed_deletes.insert(std::string(kDataUrl));
  const auto result = RunWithCopyResult(MakeRequest(), store);
  EXPECT_EQ(result.status, RedshiftStagedCopyRunStatus::kUploadCollision);
  EXPECT_FALSE(result.cleanup_complete);
  EXPECT_EQ(store.events,
            (std::vector<std::string>{"put:data", "put:manifest", "delete:data"}));
}

TEST(RedshiftStagedCopyCoordinatorTest, DefinitiveUploadFailureDoesNotDeleteTarget) {
  FakeObjectStore store;
  store.put_results[std::string(kDataUrl)] = RedshiftStagedPutResult::kFailed;
  const auto result = RunWithCopyResult(MakeRequest(), store);
  EXPECT_EQ(result.status, RedshiftStagedCopyRunStatus::kUploadFailed);
  EXPECT_EQ(store.events, (std::vector<std::string>{"put:data"}));
}

TEST(RedshiftStagedCopyCoordinatorTest, ManifestUploadFailureCleansPriorData) {
  FakeObjectStore store;
  store.put_results[std::string(kManifestUrl)] = RedshiftStagedPutResult::kFailed;
  const auto result = RunWithCopyResult(MakeRequest(), store);
  EXPECT_EQ(result.status, RedshiftStagedCopyRunStatus::kUploadFailed);
  EXPECT_TRUE(result.cleanup_complete);
  EXPECT_TRUE(store.objects.empty());
  EXPECT_EQ(store.events,
            (std::vector<std::string>{"put:data", "put:manifest", "delete:data"}));
}

TEST(RedshiftStagedCopyCoordinatorTest, LostDataResponseDeletesOnlyConfirmedOwnedObject) {
  FakeObjectStore store;
  store.put_results[std::string(kDataUrl)] = RedshiftStagedPutResult::kUnknown;
  store.unknown_creates.insert(std::string(kDataUrl));
  const auto result = RunWithCopyResult(MakeRequest(), store);
  EXPECT_EQ(result.status, RedshiftStagedCopyRunStatus::kUploadUnknown);
  EXPECT_TRUE(result.cleanup_complete);
  EXPECT_TRUE(store.objects.empty());
  EXPECT_EQ(store.events,
            (std::vector<std::string>{"put:data", "check:data", "delete:data"}));
}

TEST(RedshiftStagedCopyCoordinatorTest, LostResponseDoesNotDeleteForeignObject) {
  FakeObjectStore store;
  store.put_results[std::string(kDataUrl)] = RedshiftStagedPutResult::kUnknown;
  store.objects.emplace(std::string(kDataUrl),
                        FakeObjectStore::Object{"foreign", "someone-else"});
  const auto result = RunWithCopyResult(MakeRequest(), store);
  EXPECT_EQ(result.status, RedshiftStagedCopyRunStatus::kUploadUnknown);
  EXPECT_FALSE(result.cleanup_complete);
  EXPECT_EQ(store.events, (std::vector<std::string>{"put:data", "check:data"}));
  EXPECT_EQ(store.objects.at(std::string(kDataUrl)).bytes, "foreign");
}

TEST(RedshiftStagedCopyCoordinatorTest, UnknownOwnershipLeavesCleanupIncomplete) {
  FakeObjectStore store;
  store.put_results[std::string(kDataUrl)] = RedshiftStagedPutResult::kUnknown;
  store.unresolved_ownership.insert(std::string(kDataUrl));
  const auto result = RunWithCopyResult(MakeRequest(), store);
  EXPECT_EQ(result.status, RedshiftStagedCopyRunStatus::kUploadUnknown);
  EXPECT_FALSE(result.cleanup_complete);
  EXPECT_EQ(store.events, (std::vector<std::string>{"put:data", "check:data"}));
}

TEST(RedshiftStagedCopyCoordinatorTest, LostManifestResponseCleansBothOwnedObjects) {
  FakeObjectStore store;
  store.put_results[std::string(kManifestUrl)] = RedshiftStagedPutResult::kUnknown;
  store.unknown_creates.insert(std::string(kManifestUrl));
  const auto result = RunWithCopyResult(MakeRequest(), store);
  EXPECT_EQ(result.status, RedshiftStagedCopyRunStatus::kUploadUnknown);
  EXPECT_TRUE(result.cleanup_complete);
  EXPECT_TRUE(store.objects.empty());
  EXPECT_EQ(store.events,
            (std::vector<std::string>{"put:data", "put:manifest", "check:manifest",
                                      "delete:manifest", "delete:data"}));
}

TEST(RedshiftStagedCopyCoordinatorTest, SettledCopyFailureAndUnknownAreNotRetried) {
  for (auto copy_result :
       {RedshiftCopyExecutionResult::kFailed, RedshiftCopyExecutionResult::kUnknown}) {
    FakeObjectStore store;
    const auto result = RunWithCopyResult(MakeRequest(), store, copy_result);
    EXPECT_EQ(result.status, copy_result == RedshiftCopyExecutionResult::kFailed
                                 ? RedshiftStagedCopyRunStatus::kCopyFailed
                                 : RedshiftStagedCopyRunStatus::kCopyUnknown);
    EXPECT_FALSE(result.copy_succeeded);
    EXPECT_TRUE(result.cleanup_complete);
    EXPECT_TRUE(store.objects.empty());
    EXPECT_EQ(store.events, (std::vector<std::string>{"put:data", "put:manifest", "copy",
                                                      "delete:manifest", "delete:data"}));
  }
}

TEST(RedshiftStagedCopyCoordinatorTest, UnsettledCallbackExceptionDefersCleanup) {
  FakeObjectStore store;
  const auto result = RunRedshiftStagedCopy(
      MakeRequest(), store, [&](std::string_view) -> RedshiftCopyExecutionResult {
        throw std::runtime_error("COPY callback failure");
      });
  EXPECT_EQ(result.status, RedshiftStagedCopyRunStatus::kCopyUnknown);
  EXPECT_FALSE(result.cleanup_complete);
  EXPECT_EQ(store.objects.size(), 2);
  EXPECT_EQ(store.events, (std::vector<std::string>{"put:data", "put:manifest"}));
}

TEST(RedshiftStagedCopyCoordinatorTest, CleanupFailureAfterCopyDoesNotImplyRetry) {
  FakeObjectStore store;
  store.failed_deletes.insert(std::string(kManifestUrl));
  const auto result = RunWithCopyResult(MakeRequest(), store);
  EXPECT_EQ(result.status, RedshiftStagedCopyRunStatus::kCleanupFailed);
  EXPECT_TRUE(result.copy_succeeded);
  EXPECT_FALSE(result.cleanup_complete);
  EXPECT_EQ(store.events, (std::vector<std::string>{"put:data", "put:manifest", "copy",
                                                    "delete:manifest", "delete:data"}));
  EXPECT_EQ(store.objects.size(), 1);
  EXPECT_NE(store.objects.find(std::string(kManifestUrl)), store.objects.end());
}

TEST(RedshiftStagedCopyCoordinatorTest, CleanupNeverDeletesObjectNowOwnedByAnother) {
  FakeObjectStore store;
  const auto result = RunRedshiftStagedCopy(MakeRequest(), store, [&](std::string_view) {
    store.objects.at(std::string(kDataUrl)).token = "someone-else";
    return RedshiftCopyExecutionResult::kSucceeded;
  });
  EXPECT_EQ(result.status, RedshiftStagedCopyRunStatus::kCleanupFailed);
  EXPECT_TRUE(result.copy_succeeded);
  EXPECT_FALSE(result.cleanup_complete);
  EXPECT_EQ(store.objects.size(), 1);
  EXPECT_EQ(store.objects.at(std::string(kDataUrl)).token, "someone-else");
}

TEST(RedshiftStagedCopyCoordinatorTest, CleanupFailurePreservesEarlierCopyFailure) {
  FakeObjectStore store;
  store.failed_deletes.insert(std::string(kDataUrl));
  const auto result =
      RunWithCopyResult(MakeRequest(), store, RedshiftCopyExecutionResult::kFailed);
  EXPECT_EQ(result.status, RedshiftStagedCopyRunStatus::kCopyFailed);
  EXPECT_FALSE(result.copy_succeeded);
  EXPECT_FALSE(result.cleanup_complete);
  EXPECT_EQ(store.events, (std::vector<std::string>{"put:data", "put:manifest", "copy",
                                                    "delete:manifest", "delete:data"}));
  EXPECT_EQ(store.objects.size(), 1);
  EXPECT_NE(store.objects.find(std::string(kDataUrl)), store.objects.end());
}

}  // namespace
}  // namespace adbc::driver::pgwire
