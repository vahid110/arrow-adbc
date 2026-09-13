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

#include "redshift_staged_arrow_copy.h"

#include <string>

namespace adbc::driver::pgwire {

RedshiftStagedArrowCopyResult RunRedshiftStagedArrowCopy(
    const RedshiftStagedArrowCopyRequest& request, RedshiftStagedObjectStore& store,
    const std::function<RedshiftCopyExecutionResult(std::string_view)>& execute_copy) {
  std::string csv;
  const RedshiftCsvWriteStatus csv_status =
      WriteRedshiftCsv(request.arrow_schema, request.arrow_array, request.columns, &csv);
  if (csv_status != RedshiftCsvWriteStatus::kSucceeded) {
    return {csv_status, std::nullopt};
  }

  const RedshiftStagedCopyRequest staged_request = {
      request.database_schema, request.table,
      request.columns,         request.data_s3_url,
      request.manifest_s3_url, request.iam_role_arn,
      request.ownership_token, csv};
  return {csv_status, RunRedshiftStagedCopy(staged_request, store, execute_copy)};
}

}  // namespace adbc::driver::pgwire
