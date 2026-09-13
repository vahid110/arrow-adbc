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

#include "redshift_staged_copy_coordinator.h"

#include <optional>
#include <string_view>

#include "redshift_staged_copy.h"

namespace adbc::driver::pgwire {
namespace {

bool IsOwnershipToken(std::string_view token) {
  if (token.empty() || token.size() > 64) return false;
  for (char c : token) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '-' || c == '_')) {
      return false;
    }
  }
  return true;
}

bool IsSameOwnedDirectory(std::string_view data_uri, std::string_view manifest_uri,
                          std::string_view token) {
  const std::size_t data_end = data_uri.rfind('/');
  const std::size_t manifest_end = manifest_uri.rfind('/');
  if (data_end == std::string_view::npos || manifest_end == std::string_view::npos ||
      data_uri.substr(0, data_end) != manifest_uri.substr(0, manifest_end)) {
    return false;
  }
  const std::string_view directory = data_uri.substr(0, data_end);
  return directory.size() > token.size() &&
         directory[directory.size() - token.size() - 1] == '/' &&
         directory.substr(directory.size() - token.size()) == token;
}

}  // namespace

RedshiftStagedCopyRunResult RunRedshiftStagedCopy(
    const RedshiftStagedCopyRequest& request, RedshiftStagedObjectStore& store,
    const std::function<RedshiftCopyExecutionResult(std::string_view)>& execute_copy) {
  const std::optional<RedshiftStagedCopyPlan> plan = PrepareRedshiftStagedCopy(
      request.schema, request.table, request.columns, request.data_s3_url,
      request.manifest_s3_url, request.iam_role_arn);
  if (!plan || !execute_copy || !IsOwnershipToken(request.ownership_token) ||
      !IsSameOwnedDirectory(request.data_s3_url, request.manifest_s3_url,
                            request.ownership_token) ||
      request.serialized_csv.empty() ||
      request.serialized_csv.size() > kRedshiftStagedCopyMaxPayloadBytes) {
    return {RedshiftStagedCopyRunStatus::kInvalidInput, true, false};
  }

  bool data_owned = false;
  bool manifest_owned = false;
  bool cleanup_complete = true;
  auto confirm_unknown = [&](std::string_view uri, bool* owned) {
    switch (store.CheckOwnership(uri, request.ownership_token)) {
      case RedshiftStagedOwnership::kOwned:
        *owned = true;
        break;
      case RedshiftStagedOwnership::kOtherOrAbsent:
        // A timed-out conditional PUT might still finish after this probe.
        // Never claim deterministic cleanup without positive ownership proof.
        cleanup_complete = false;
        break;
      case RedshiftStagedOwnership::kUnknown:
        cleanup_complete = false;
        break;
    }
  };
  auto cleanup = [&]() {
    // Cleanup the manifest first, then the data, even if the first delete fails.
    if (manifest_owned &&
        !store.DeleteIfOwned(request.manifest_s3_url, request.ownership_token)) {
      cleanup_complete = false;
    }
    if (data_owned &&
        !store.DeleteIfOwned(request.data_s3_url, request.ownership_token)) {
      cleanup_complete = false;
    }
  };

  const RedshiftStagedPutResult data_result = store.PutIfAbsent(
      request.data_s3_url, request.serialized_csv, request.ownership_token);
  if (data_result != RedshiftStagedPutResult::kCreated) {
    if (data_result == RedshiftStagedPutResult::kUnknown) {
      confirm_unknown(request.data_s3_url, &data_owned);
    }
    cleanup();
    return {data_result == RedshiftStagedPutResult::kCollision
                ? RedshiftStagedCopyRunStatus::kUploadCollision
            : data_result == RedshiftStagedPutResult::kFailed
                ? RedshiftStagedCopyRunStatus::kUploadFailed
                : RedshiftStagedCopyRunStatus::kUploadUnknown,
            cleanup_complete, false};
  }
  data_owned = true;

  const RedshiftStagedPutResult manifest_result = store.PutIfAbsent(
      request.manifest_s3_url, plan->manifest_json, request.ownership_token);
  if (manifest_result != RedshiftStagedPutResult::kCreated) {
    if (manifest_result == RedshiftStagedPutResult::kUnknown) {
      confirm_unknown(request.manifest_s3_url, &manifest_owned);
    }
    cleanup();
    return {manifest_result == RedshiftStagedPutResult::kCollision
                ? RedshiftStagedCopyRunStatus::kUploadCollision
            : manifest_result == RedshiftStagedPutResult::kFailed
                ? RedshiftStagedCopyRunStatus::kUploadFailed
                : RedshiftStagedCopyRunStatus::kUploadUnknown,
            cleanup_complete, false};
  }
  manifest_owned = true;

  RedshiftCopyExecutionResult copy_result;
  try {
    copy_result = execute_copy(plan->copy_sql);
  } catch (...) {
    // The server may still be reading the objects. Leave them for an owned-
    // object reconciliation rather than racing an in-flight COPY with DELETE.
    return {RedshiftStagedCopyRunStatus::kCopyUnknown, false, false};
  }
  cleanup();
  if (copy_result == RedshiftCopyExecutionResult::kSucceeded) {
    return {cleanup_complete ? RedshiftStagedCopyRunStatus::kSucceeded
                             : RedshiftStagedCopyRunStatus::kCleanupFailed,
            cleanup_complete, true};
  }
  return {copy_result == RedshiftCopyExecutionResult::kUnknown
              ? RedshiftStagedCopyRunStatus::kCopyUnknown
              : RedshiftStagedCopyRunStatus::kCopyFailed,
          cleanup_complete, false};
}

}  // namespace adbc::driver::pgwire
