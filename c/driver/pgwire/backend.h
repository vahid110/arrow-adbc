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

enum class BulkIngestMode {
  kBinaryCopy,
  kUnsupported,
};

struct BackendCapabilities {
  bool prepared_statements = true;
  bool binary_parameters = true;
  bool binary_query_copy = false;
  bool binary_ingest_copy = false;
  bool type_catalog_has_typarray = false;
  bool metadata_constraints = false;
  bool metadata_statistics = false;
  bool transactional_ddl = false;
};

struct TableTypeMapping {
  std::string_view name;
  std::string_view relkind;
};

inline constexpr TableTypeMapping kPostgreSQLTableTypes[] = {
    {"table", "r"},             {"view", "v"},
    {"materialized_view", "m"}, {"toast_table", "t"},
    {"foreign_table", "f"},     {"partitioned_table", "p"},
};

inline constexpr TableTypeMapping kRedshiftTableTypes[] = {
    {"table", "r"},
    {"view", "v"},
};

struct BackendProfile {
  BackendKind kind;
  std::string_view name;
  BackendCapabilities capabilities;
  const TableTypeMapping* table_types;
  std::size_t table_type_count;

  constexpr const TableTypeMapping* FindTableType(std::string_view type_name) const {
    for (std::size_t i = 0; i < table_type_count; i++) {
      if (table_types[i].name == type_name) return &table_types[i];
    }
    return nullptr;
  }

  static constexpr BackendProfile PostgreSQL() {
    return {
        BackendKind::kPostgreSQL,
        "PostgreSQL",
        {/*prepared_statements=*/true,
         /*binary_parameters=*/true,
         /*binary_query_copy=*/true,
         /*binary_ingest_copy=*/true,
         /*type_catalog_has_typarray=*/true,
         /*metadata_constraints=*/true,
         /*metadata_statistics=*/true,
         /*transactional_ddl=*/true},
        kPostgreSQLTableTypes,
        sizeof(kPostgreSQLTableTypes) / sizeof(kPostgreSQLTableTypes[0]),
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
         /*type_catalog_has_typarray=*/false,
         /*metadata_constraints=*/false,
         /*metadata_statistics=*/false,
         /*transactional_ddl=*/false},
        kRedshiftTableTypes,
        sizeof(kRedshiftTableTypes) / sizeof(kRedshiftTableTypes[0]),
    };
  }
};

inline BackendProfile DetectBackendProfile(std::string_view version_string) {
  if (version_string.find("Redshift") != std::string_view::npos) {
    return BackendProfile::Redshift();
  }
  return BackendProfile::PostgreSQL();
}

constexpr QueryResultMode SelectQueryResultMode(const BackendProfile& profile,
                                                bool copy_enabled,
                                                bool output_requested) {
  if (output_requested && copy_enabled && profile.capabilities.binary_query_copy) {
    return QueryResultMode::kBinaryCopy;
  }
  return QueryResultMode::kText;
}

constexpr BulkIngestMode SelectBulkIngestMode(const BackendProfile& profile) {
  return profile.capabilities.binary_ingest_copy ? BulkIngestMode::kBinaryCopy
                                                 : BulkIngestMode::kUnsupported;
}

}  // namespace adbc::driver::pgwire
