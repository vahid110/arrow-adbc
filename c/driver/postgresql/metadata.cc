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

#include <limits>

#include "redshift_scalar.h"
#include "result_helper.h"

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
    "pg_catalog.col_description(cls.oid, attr.attnum), typ.typname, attr.atttypmod "
    "FROM pg_catalog.pg_attribute AS attr "
    "INNER JOIN pg_catalog.pg_class AS cls ON attr.attrelid = cls.oid "
    "INNER JOIN pg_catalog.pg_namespace AS nsp ON nsp.oid = cls.relnamespace "
    "INNER JOIN pg_catalog.pg_type AS typ ON attr.atttypid = typ.oid "
    "WHERE attr.attnum > 0 AND NOT attr.attisdropped "
    "AND nsp.nspname = $1 AND cls.relname = $2";

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

adbc::driver::Result<adbc::driver::GetObjectsHelper::Column> ResolveColumnMetadata(
    const PqResultRow& row, const adbc::driver::pgwire::BackendProfile& profile) {
  using adbc::driver::Status;
  using Column = adbc::driver::GetObjectsHelper::Column;
  if (!row.IsValid() || row.NumColumns() != 5 || row[0].is_null || row[1].is_null) {
    return Status(ADBC_STATUS_INVALID_DATA,
                  "Expected name/ordinal/remark/type/modifier catalog column");
  }
  Column column;
  column.column_name = std::string_view(row[0].data, row[0].len);
  int64_t ordinal;
  UNWRAP_RESULT(ordinal, row[1].ParseInteger());
  if (ordinal <= 0 || ordinal > (std::numeric_limits<int32_t>::max)()) {
    return Status(ADBC_STATUS_INVALID_DATA,
                  "Catalog column ordinal is outside positive int32 range");
  }
  column.ordinal_position = static_cast<int32_t>(ordinal);
  if (!row[2].is_null) column.remarks = std::string_view(row[2].data, row[2].len);
  std::optional<int32_t> raw;
  UNWRAP_RESULT(raw, row[4].ParseTypeModifier());
  if (!row[3].is_null) {
    column.xdbc = adbc::driver::GetObjectsHelper::ColumnXdbc();
    auto& xdbc = *column.xdbc;
    xdbc.xdbc_type_name = std::string_view(row[3].data, row[3].len);
    const auto metadata = profile.DescribeTypeModifier(*xdbc.xdbc_type_name, raw);
    if (RedshiftScalarSchema::Canonical(metadata.sql_type) != RedshiftScalarKind::kNone ||
        RedshiftBytesSchema::Canonical(metadata.sql_type) != RedshiftBytesKind::kNone ||
        RedshiftTemporalSchema::Canonical(metadata.sql_type) !=
            RedshiftTemporalKind::kNone ||
        profile.IntervalFamily(*xdbc.xdbc_type_name) !=
            adbc::driver::pgwire::CatalogIntervalFamily::kNone) {
      xdbc.xdbc_type_name = metadata.sql_type;
    }
    if (metadata.fractional_precision)
      xdbc.xdbc_decimal_digits = static_cast<int16_t>(*metadata.fractional_precision);
    if (metadata.precision && metadata.scale) {
      xdbc.xdbc_column_size = metadata.precision;
      xdbc.xdbc_decimal_digits = static_cast<int16_t>(*metadata.scale);
      xdbc.xdbc_num_prec_radix = 10;
    } else if (metadata.byte_width) {
      xdbc.xdbc_column_size = metadata.byte_width;
      xdbc.xdbc_char_octet_length = metadata.byte_width;
    }
  }
  return column;
}

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
  return std::string(kColumnsQueryAll) + (filtered ? " AND attr.attname LIKE $3" : "") +
         " ORDER BY attr.attnum";
}

std::string MetadataQuerySet::Constraints() const {
  return std::string(kConstraintsQueryAll);
}

std::string MetadataQuerySet::TableSchema() const {
  return "SELECT attname, atttypid, atttypmod FROM pg_catalog.pg_class AS cls "
         "INNER JOIN pg_catalog.pg_attribute AS attr ON cls.oid = attr.attrelid "
         "INNER JOIN pg_catalog.pg_type AS typ ON attr.atttypid = typ.oid "
         "WHERE attr.attnum > 0 AND NOT attr.attisdropped AND cls.oid = "
         "$1::regclass::oid "
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
