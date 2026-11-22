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
#include <string>
#include <string_view>
#include <vector>

#include "redshift_replace_preflight.h"

namespace adbc::driver::pgwire {

// Catalog-only: CURRENT_SCHEMAS is leader-only on Redshift. A visible permanent
// relation (or another session's temporary relation) must never be a fallback.
inline constexpr std::string_view kRedshiftTemporaryTargetQuery =
    "SELECT ns.nspname FROM pg_catalog.pg_class AS cls "
    "INNER JOIN pg_catalog.pg_namespace AS ns ON ns.oid=cls.relnamespace "
    "WHERE cls.relname=$1 AND substring(ns.nspname,1,8)='pg_temp_' "
    "AND ns.nspname=ANY(current_schemas(true))";

using RedshiftTemporaryTargetReader =
    std::function<Status(std::string_view query, const std::string& target,
                         std::vector<std::string>* schemas)>;

// An empty result means absent. Errors and ambiguous results are not absence.
// Return the real namespace and normalized catalog table name, not the
// PostgreSQL-only pg_temp alias or the original mixed-case spelling. Catalog
// regclass lookup must not reinterpret a name differently from SQL identifier
// folding. The caller quotes BOTH returned names for metadata/INSERT/COPY/DROP.
inline Status ResolveRedshiftTemporaryTarget(
    std::string target, const RedshiftSettingReader& read_setting,
    const RedshiftTemporaryTargetReader& read_target, std::string* schema,
    std::string* catalog_target) {
  schema->clear();
  catalog_target->clear();
  if (target.empty() || target.size() > 127 || target.find('\0') != std::string::npos)
    return Status::InvalidArgument("Invalid Redshift temporary table identifier");
  std::string unused_schema;
  UNWRAP_STATUS(
      NormalizeRedshiftLookupIdentifiers(&target, &unused_schema, read_setting));
  std::vector<std::string> schemas;
  UNWRAP_STATUS(read_target(kRedshiftTemporaryTargetQuery, target, &schemas));
  if (schemas.empty()) return Status::Ok();
  if (schemas.size() != 1 || schemas[0].size() <= 8 ||
      schemas[0].compare(0, 8, "pg_temp_") != 0)
    return Status(ADBC_STATUS_INVALID_DATA, "Invalid Redshift temporary target catalog");
  *schema = schemas[0];
  *catalog_target = target;
  return Status::Ok();
}

}  // namespace adbc::driver::pgwire
