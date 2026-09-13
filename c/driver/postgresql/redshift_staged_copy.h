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

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adbc::driver::pgwire {

struct RedshiftStagedCopyPlan {
  std::string manifest_json;
  std::string copy_sql;
};

// Prepare only; this does not upload objects, execute COPY, or select an ingest path.
// Columns are required in the exact order of the staged data. The plan names
// them explicitly so an append cannot silently load into a different table order.
// S3 URLs deliberately accept a small, generated-object subset, not every legal S3 key.
// The manifest lists exactly one mandatory object so COPY cannot interpret a data URL
// as a prefix and accidentally load neighboring objects.
std::optional<RedshiftStagedCopyPlan> PrepareRedshiftStagedCopy(
    std::string_view schema, std::string_view table,
    const std::vector<std::string_view>& columns, std::string_view data_s3_url,
    std::string_view manifest_s3_url, std::string_view iam_role_arn);

}  // namespace adbc::driver::pgwire
