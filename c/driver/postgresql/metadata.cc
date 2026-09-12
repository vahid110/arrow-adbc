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

#include "metadata.h"

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace adbcpq {
namespace {

constexpr std::string_view kCatalogQueryAll =
    "SELECT datname FROM pg_catalog.pg_database";

constexpr std::string_view kSchemaQueryAll =
    "SELECT nspname FROM pg_catalog.pg_namespace WHERE "
    "nspname !~ '^pg_' AND nspname <> 'information_schema'";

constexpr std::string_view kTablesQueryAll =
    "SELECT c.relname, CASE c.relkind WHEN 'r' THEN 'table' WHEN 'v' THEN 'view' "
    "WHEN 'm' THEN 'materialized view' WHEN 't' THEN 'TOAST table' "
    "WHEN 'f' THEN 'foreign table' WHEN 'p' THEN 'partitioned table' END "
    "AS reltype FROM pg_catalog.pg_class c "
    "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
    "WHERE n.nspname = $1 AND c.relkind = ANY($2)";

constexpr std::string_view kColumnsQueryAll =
    "SELECT attr.attname, attr.attnum, "
    "pg_catalog.col_description(cls.oid, attr.attnum), typ.typname "
    "FROM pg_catalog.pg_attribute AS attr "
    "INNER JOIN pg_catalog.pg_class AS cls ON attr.attrelid = cls.oid "
    "INNER JOIN pg_catalog.pg_namespace AS nsp ON nsp.oid = cls.relnamespace "
    "INNER JOIN pg_catalog.pg_type AS typ ON attr.atttypid = typ.oid "
    "WHERE attr.attnum > 0 AND NOT attr.attisdropped "
    "AND nsp.nspname LIKE $1 AND cls.relname LIKE $2";

constexpr std::string_view kConstraintsQueryAll =
    "WITH fk_unnest AS ( "
    "SELECT con.conname, 'FOREIGN KEY' AS contype, conrelid, "
    "UNNEST(con.conkey) AS conkey, confrelid, UNNEST(con.confkey) AS confkey "
    "FROM pg_catalog.pg_constraint AS con "
    "INNER JOIN pg_catalog.pg_class AS cls ON cls.oid = conrelid "
    "INNER JOIN pg_catalog.pg_namespace AS nsp ON nsp.oid = cls.relnamespace "
    "WHERE con.contype = 'f' AND nsp.nspname = $1 AND cls.relname = $2), "
    "fk_names AS (SELECT fk_unnest.conname, fk_unnest.contype, "
    "fk_unnest.conkey, fk_unnest.confkey, attr.attname, "
    "fnsp.nspname AS fschema, fcls.relname AS ftable, fattr.attname AS fattname "
    "FROM fk_unnest "
    "INNER JOIN pg_catalog.pg_class AS cls ON cls.oid = fk_unnest.conrelid "
    "INNER JOIN pg_catalog.pg_class AS fcls ON fcls.oid = fk_unnest.confrelid "
    "INNER JOIN pg_catalog.pg_namespace AS fnsp ON fnsp.oid = fcls.relnamespace "
    "INNER JOIN pg_catalog.pg_attribute AS attr ON attr.attnum = fk_unnest.conkey "
    "AND attr.attrelid = fk_unnest.conrelid "
    "LEFT JOIN pg_catalog.pg_attribute AS fattr ON fattr.attnum = fk_unnest.confkey "
    "AND fattr.attrelid = fk_unnest.confrelid), "
    "fkeys AS (SELECT conname, contype, ARRAY_AGG(attname ORDER BY conkey) "
    "AS colnames, fschema, ftable, ARRAY_AGG(fattname ORDER BY confkey) "
    "AS fcolnames FROM fk_names GROUP BY conname, contype, fschema, ftable), "
    "other_constraints AS (SELECT con.conname, CASE con.contype "
    "WHEN 'c' THEN 'CHECK' WHEN 'u' THEN 'UNIQUE' WHEN 'p' THEN 'PRIMARY KEY' "
    "END AS contype, ARRAY_AGG(attr.attname) AS colnames "
    "FROM pg_catalog.pg_constraint AS con CROSS JOIN UNNEST(conkey) AS conkeys "
    "INNER JOIN pg_catalog.pg_class AS cls ON cls.oid = con.conrelid "
    "INNER JOIN pg_catalog.pg_namespace AS nsp ON nsp.oid = cls.relnamespace "
    "INNER JOIN pg_catalog.pg_attribute AS attr ON attr.attnum = conkeys "
    "AND cls.oid = attr.attrelid WHERE con.contype IN ('c', 'u', 'p') "
    "AND nsp.nspname = $1 AND cls.relname = $2 GROUP BY conname, contype) "
    "SELECT conname, contype, colnames, fschema, ftable, fcolnames FROM fkeys "
    "UNION ALL SELECT conname, contype, colnames, NULL, NULL, NULL "
    "FROM other_constraints";

}  // namespace

std::string MetadataQuerySet::Catalogs(bool filtered) const {
  return std::string(kCatalogQueryAll) + (filtered ? " WHERE datname = $1" : "");
}

std::string MetadataQuerySet::Schemas(bool filtered) const {
  return std::string(kSchemaQueryAll) + (filtered ? " AND nspname = $1" : "");
}

std::string MetadataQuerySet::Tables(bool filtered) const {
  return std::string(kTablesQueryAll) + (filtered ? " AND c.relname LIKE $3" : "");
}

std::string MetadataQuerySet::Columns(bool filtered) const {
  return std::string(kColumnsQueryAll) + (filtered ? " AND attr.attname LIKE $3" : "");
}

std::string MetadataQuerySet::Constraints(bool filtered) const {
  return std::string(kConstraintsQueryAll) + (filtered ? " WHERE conname LIKE $3" : "");
}

std::string MetadataQuerySet::TableSchema() const {
  return "SELECT attname, atttypid FROM pg_catalog.pg_class AS cls "
         "INNER JOIN pg_catalog.pg_attribute AS attr ON cls.oid = attr.attrelid "
         "INNER JOIN pg_catalog.pg_type AS typ ON attr.atttypid = typ.oid "
         "WHERE attr.attnum >= 0 AND cls.oid = $1::regclass::oid "
         "ORDER BY attr.attnum";
}

std::string MetadataQuerySet::TableTypesArrayLiteral(
    const std::vector<std::string_view>& table_types) const {
  std::ostringstream bind;
  bind << "{";
  int count = 0;
  auto append = [&](std::string_view relkind) {
    if (count++ > 0) bind << ", ";
    bind << "\"" << relkind << "\"";
  };

  if (table_types.empty()) {
    for (std::size_t i = 0; i < backend_profile_.table_type_count; i++) {
      append(backend_profile_.table_types[i].relkind);
    }
  } else {
    for (const auto type : table_types) {
      const auto* item = backend_profile_.FindTableType(type);
      if (item != nullptr) append(item->relkind);
    }
  }

  bind << "}";
  return bind.str();
}

std::vector<std::string> MetadataQuerySet::TableTypeNames() const {
  std::vector<std::string> names;
  names.reserve(backend_profile_.table_type_count);
  for (std::size_t i = 0; i < backend_profile_.table_type_count; i++) {
    names.emplace_back(backend_profile_.table_types[i].name);
  }
  return names;
}

}  // namespace adbcpq
