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

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include <nanoarrow/nanoarrow.h>

#include "redshift_csv_writer.h"
#include "redshift_staged_copy_coordinator.h"

namespace adbc::driver::pgwire {

// This is an offline, Redshift-private seam. It does not select the active
// ingest path, create S3 objects itself, or establish Redshift CSV semantics.
struct RedshiftStagedArrowStreamCopyRequest {
  ArrowArrayStream* arrow_stream;
  std::string_view database_schema;
  std::string_view table;
  std::vector<std::string_view> columns;
  std::string_view data_s3_url;
  std::string_view manifest_s3_url;
  std::string_view iam_role_arn;
  std::string_view ownership_token;
};

enum class RedshiftStagedArrowStreamPreflightStatus {
  kSucceeded,
  kInvalidInput,
  kStreamError,
  kCsvRejected,
  kEmptyStream,
  kTooManyBatches,
  kPayloadTooLarge,
};

struct RedshiftStagedArrowStreamCopyResult {
  RedshiftStagedArrowStreamPreflightStatus preflight_status;
  // Populated only when the CSV writer rejected a batch.
  std::optional<RedshiftCsvWriteStatus> csv_status;
  // Empty on any preflight failure: neither store nor COPY was called.
  std::optional<RedshiftStagedCopyRunResult> staged_copy;
};

// Moves a valid stream from request.arrow_stream and releases it, its schema,
// and each yielded array on every path. Invalid stream handles are not moved.
// Preflights the entire finite stream into one bounded CSV object before any
// store or COPY callback. The callback must obey the coordinator's settled-
// result contract and cannot retain the supplied SQL string_view.
RedshiftStagedArrowStreamCopyResult RunRedshiftStagedArrowStreamCopy(
    const RedshiftStagedArrowStreamCopyRequest& request, RedshiftStagedObjectStore& store,
    const std::function<RedshiftCopyExecutionResult(std::string_view)>& execute_copy);

}  // namespace adbc::driver::pgwire
