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

#pragma once

#include <cstddef>
#include <functional>
#include <string_view>
#include <vector>

namespace adbc::driver::pgwire {

// This remains Redshift-private and AWS-free. The caller supplies serialized
// CSV, two unique object URLs beneath a directory named by ownership_token,
// and a store whose methods never throw. The store must use conditional create,
// classify lost responses as kUnknown, and delete only an object it can prove
// belongs to ownership_token. No prefix-wide deletion is permitted.
enum class RedshiftStagedPutResult {
  kCreated,
  kCollision,
  kFailed,   // Definitively not created.
  kUnknown,  // The request might have created the object.
};

enum class RedshiftStagedOwnership { kOwned, kOtherOrAbsent, kUnknown };

class RedshiftStagedObjectStore {
 public:
  virtual ~RedshiftStagedObjectStore() = default;

  virtual RedshiftStagedPutResult PutIfAbsent(std::string_view object_uri,
                                             std::string_view bytes,
                                             std::string_view ownership_token) noexcept = 0;
  virtual RedshiftStagedOwnership CheckOwnership(
      std::string_view object_uri, std::string_view ownership_token) noexcept = 0;
  // Returns true if an owned object was deleted or was already absent. Must
  // return false without deletion if ownership cannot be confirmed.
  virtual bool DeleteIfOwned(std::string_view object_uri,
                             std::string_view ownership_token) noexcept = 0;
};

// The callback must not return until the server has finished or canceled COPY
// and can no longer read the staged objects. kUnknown means its *result* is
// ambiguous after settling, not that COPY may still be running. In particular,
// callers must not retry a kUnknown COPY automatically: rows may have loaded.
// If the callback throws before confirming the server has stopped, the
// coordinator leaves both exact objects for later owned-object reconciliation.
enum class RedshiftCopyExecutionResult { kSucceeded, kFailed, kUnknown };

struct RedshiftStagedCopyRequest {
  std::string_view schema;
  std::string_view table;
  std::vector<std::string_view> columns;
  std::string_view data_s3_url;
  std::string_view manifest_s3_url;
  std::string_view iam_role_arn;
  std::string_view ownership_token;
  std::string_view serialized_csv;
};

enum class RedshiftStagedCopyRunStatus {
  kSucceeded,
  kInvalidInput,
  kUploadCollision,
  kUploadFailed,
  kUploadUnknown,
  kCopyFailed,
  kCopyUnknown,
  kCleanupFailed,  // COPY succeeded, but exact-object cleanup is not confirmed.
};

struct RedshiftStagedCopyRunResult {
  RedshiftStagedCopyRunStatus status;
  bool cleanup_complete;
  bool copy_succeeded;
};

// A deliberately small first bound; large streams/multipart upload are future
// work. This does not perform CSV conversion or select the ADBC ingest path.
inline constexpr std::size_t kRedshiftStagedCopyMaxPayloadBytes = 8 * 1024 * 1024;

RedshiftStagedCopyRunResult RunRedshiftStagedCopy(
    const RedshiftStagedCopyRequest& request, RedshiftStagedObjectStore& store,
    const std::function<RedshiftCopyExecutionResult(std::string_view)>& execute_copy);

}  // namespace adbc::driver::pgwire
