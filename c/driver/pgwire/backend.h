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

#include <string_view>

namespace adbc::driver::pgwire {

enum class BackendKind {
  kPostgreSQL,
  kRedshift,
};

enum class QueryResultMode {
  kText,
  kBinaryCopy,
};

struct BackendCapabilities {
  bool prepared_statements = true;
  bool binary_parameters = true;
  bool binary_query_copy = false;
  bool binary_ingest_copy = false;
  bool metadata_constraints = false;
  bool metadata_statistics = false;
  bool transactional_ddl = false;
};

struct BackendProfile {
  BackendKind kind;
  std::string_view name;
  BackendCapabilities capabilities;

  static constexpr BackendProfile PostgreSQL() {
    return {
        BackendKind::kPostgreSQL,
        "PostgreSQL",
        {/*prepared_statements=*/true,
         /*binary_parameters=*/true,
         /*binary_query_copy=*/true,
         /*binary_ingest_copy=*/true,
         /*metadata_constraints=*/true,
         /*metadata_statistics=*/true,
         /*transactional_ddl=*/true},
    };
  }

  static constexpr BackendProfile Redshift() {
    return {
        BackendKind::kRedshift,
        "Redshift",
        {/*prepared_statements=*/true,
         /*binary_parameters=*/false,
         /*binary_query_copy=*/false,
         /*binary_ingest_copy=*/false,
         /*metadata_constraints=*/false,
         /*metadata_statistics=*/false,
         /*transactional_ddl=*/false},
    };
  }
};

constexpr QueryResultMode SelectQueryResultMode(const BackendProfile& profile,
                                                bool copy_enabled,
                                                bool output_requested) {
  if (output_requested && copy_enabled && profile.capabilities.binary_query_copy) {
    return QueryResultMode::kBinaryCopy;
  }
  return QueryResultMode::kText;
}

}  // namespace adbc::driver::pgwire
