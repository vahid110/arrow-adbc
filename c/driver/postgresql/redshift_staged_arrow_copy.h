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

// A single Arrow record batch only. This does not consume an ArrowArrayStream,
// generate object names, upload to S3, or select an ADBC ingest strategy.
struct RedshiftStagedArrowCopyRequest {
  const ArrowSchema* arrow_schema;
  const ArrowArray* arrow_array;
  std::string_view database_schema;
  std::string_view table;
  std::vector<std::string_view> columns;
  std::string_view data_s3_url;
  std::string_view manifest_s3_url;
  std::string_view iam_role_arn;
  std::string_view ownership_token;
};

struct RedshiftStagedArrowCopyResult {
  RedshiftCsvWriteStatus csv_status;
  // Empty only when CSV preflight failed; no store or COPY callback was called.
  std::optional<RedshiftStagedCopyRunResult> staged_copy;
};

// Owns serialized bytes until the synchronous coordinator returns. The
// execute_copy callback must obey RunRedshiftStagedCopy's settled-result
// contract: it may return only after the server has finished or canceled COPY.
RedshiftStagedArrowCopyResult RunRedshiftStagedArrowCopy(
    const RedshiftStagedArrowCopyRequest& request, RedshiftStagedObjectStore& store,
    const std::function<RedshiftCopyExecutionResult(std::string_view)>& execute_copy);

}  // namespace adbc::driver::pgwire
