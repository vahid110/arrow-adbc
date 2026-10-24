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
#include <utility>
#include <vector>

#include "postgres_type.h"
#include "result_helper.h"

namespace adbcpq {
// Only the closed, natively qualified catalog combinations below can produce
// known target constraints. Query descriptors and OID-only caches stay unknown.
struct RedshiftIntervalCatalogColumn {
  std::string catalog, schema, table, name;
  int64_t relation = 0, ordinal = 0, oid = 0;
  std::optional<int32_t> raw;
  std::optional<std::string> formatted;
  bool operator==(const RedshiftIntervalCatalogColumn&) const = default;
};

inline Status ParseIntervalCatalogIdentity(
    const PqResultHelper& result, const PostgresType& table,
    std::vector<RedshiftIntervalCatalogColumn>* out) {
  if (result.NumColumns() != 9 || result.NumRows() != table.n_children())
    return Status(ADBC_STATUS_INVALID_DATA, "Interval catalog identity shape changed");
  std::vector<RedshiftIntervalCatalogColumn> columns;
  for (int row = 0; row < result.NumRows(); ++row) {
    const auto item = result.Row(row);
    for (int col = 0; col < 7; ++col)
      if (item[col].is_null)
        return Status(ADBC_STATUS_INVALID_DATA, "Missing interval catalog identity");
    RedshiftIntervalCatalogColumn value;
    value.catalog = item[0].value();
    value.schema = item[1].value();
    value.table = item[2].value();
    value.name = item[5].value();
    UNWRAP_RESULT(value.relation, item[3].ParseInteger());
    UNWRAP_RESULT(value.ordinal, item[4].ParseInteger());
    UNWRAP_RESULT(value.oid, item[6].ParseInteger());
    UNWRAP_RESULT(value.raw, item[7].ParseTypeModifier());
    if (!item[8].is_null) value.formatted = item[8].value();
    if (value.catalog.empty() || value.schema.empty() || value.table.empty() ||
        value.relation <= 0 || value.relation > UINT32_MAX || value.ordinal <= 0 ||
        value.ordinal > INT16_MAX || value.oid != table.child(row).oid() ||
        value.name != table.child(row).field_name() ||
        value.raw != table.child(row).type_modifier())
      return Status(ADBC_STATUS_INVALID_DATA,
                    "Interval catalog identity disagrees with target");
    if (!columns.empty() &&
        (value.catalog != columns[0].catalog || value.schema != columns[0].schema ||
         value.table != columns[0].table || value.relation != columns[0].relation ||
         value.ordinal <= columns.back().ordinal))
      return Status(ADBC_STATUS_INVALID_DATA,
                    "Interval catalog relation/ordinal changed");
    columns.push_back(std::move(value));
  }
  *out = std::move(columns);
  return Status::Ok();
}

// Captured on native Redshift operation cd9f2ad028224383be5e7fb164691305:
// pg_attribute/format_type observations 104 and 708 agree for these 37 explicit
// declarations. SVV observations 105 and 709 are NULL in all three fields.
// This is a closed lookup, not PostgreSQL typmod arithmetic or a default rule.
inline void QualifyIntervalCatalogTarget(const RedshiftIntervalCatalogColumn& identity,
                                         RedshiftIntervalConstraint* constraint) {
  using Family = RedshiftIntervalFamily;
  using Qualifier = RedshiftIntervalQualifier;
  struct Observation {
    int32_t raw;
    Family family;
    Qualifier qualifier;
    std::optional<int32_t> precision;
    std::string_view formatted;
  };
  static const Observation qualified[] = {
      {327683, Family::kYearMonth, Qualifier::kYear, std::nullopt, "interval year"},
      {196611, Family::kYearMonth, Qualifier::kMonth, std::nullopt, "interval month"},
      {458755, Family::kYearMonth, Qualifier::kYearToMonth, std::nullopt,
       "interval year to month"},
      {589827, Family::kDaySecond, Qualifier::kDay, std::nullopt, "interval day"},
      {67174403, Family::kDaySecond, Qualifier::kHour, std::nullopt, "interval hour"},
      {134283267, Family::kDaySecond, Qualifier::kMinute, std::nullopt,
       "interval minute"},
      {268435460, Family::kDaySecond, Qualifier::kSecond, 0, "interval second (0)"},
      {268435461, Family::kDaySecond, Qualifier::kSecond, 1, "interval second (1)"},
      {268435462, Family::kDaySecond, Qualifier::kSecond, 2, "interval second (2)"},
      {268435463, Family::kDaySecond, Qualifier::kSecond, 3, "interval second (3)"},
      {268435464, Family::kDaySecond, Qualifier::kSecond, 4, "interval second (4)"},
      {268435465, Family::kDaySecond, Qualifier::kSecond, 5, "interval second (5)"},
      {268435466, Family::kDaySecond, Qualifier::kSecond, 6, "interval second (6)"},
      {67698691, Family::kDaySecond, Qualifier::kDayToHour, std::nullopt,
       "interval day to hour"},
      {201916419, Family::kDaySecond, Qualifier::kDayToMinute, std::nullopt,
       "interval day to minute"},
      {470286340, Family::kDaySecond, Qualifier::kDayToSecond, 0,
       "interval day to second (0)"},
      {470286341, Family::kDaySecond, Qualifier::kDayToSecond, 1,
       "interval day to second (1)"},
      {470286342, Family::kDaySecond, Qualifier::kDayToSecond, 2,
       "interval day to second (2)"},
      {470286343, Family::kDaySecond, Qualifier::kDayToSecond, 3,
       "interval day to second (3)"},
      {470286344, Family::kDaySecond, Qualifier::kDayToSecond, 4,
       "interval day to second (4)"},
      {470286345, Family::kDaySecond, Qualifier::kDayToSecond, 5,
       "interval day to second (5)"},
      {470286346, Family::kDaySecond, Qualifier::kDayToSecond, 6,
       "interval day to second (6)"},
      {201392131, Family::kDaySecond, Qualifier::kHourToMinute, std::nullopt,
       "interval hour to minute"},
      {469762052, Family::kDaySecond, Qualifier::kHourToSecond, 0,
       "interval hour to second (0)"},
      {469762053, Family::kDaySecond, Qualifier::kHourToSecond, 1,
       "interval hour to second (1)"},
      {469762054, Family::kDaySecond, Qualifier::kHourToSecond, 2,
       "interval hour to second (2)"},
      {469762055, Family::kDaySecond, Qualifier::kHourToSecond, 3,
       "interval hour to second (3)"},
      {469762056, Family::kDaySecond, Qualifier::kHourToSecond, 4,
       "interval hour to second (4)"},
      {469762057, Family::kDaySecond, Qualifier::kHourToSecond, 5,
       "interval hour to second (5)"},
      {469762058, Family::kDaySecond, Qualifier::kHourToSecond, 6,
       "interval hour to second (6)"},
      {402653188, Family::kDaySecond, Qualifier::kMinuteToSecond, 0,
       "interval minute to second (0)"},
      {402653189, Family::kDaySecond, Qualifier::kMinuteToSecond, 1,
       "interval minute to second (1)"},
      {402653190, Family::kDaySecond, Qualifier::kMinuteToSecond, 2,
       "interval minute to second (2)"},
      {402653191, Family::kDaySecond, Qualifier::kMinuteToSecond, 3,
       "interval minute to second (3)"},
      {402653192, Family::kDaySecond, Qualifier::kMinuteToSecond, 4,
       "interval minute to second (4)"},
      {402653193, Family::kDaySecond, Qualifier::kMinuteToSecond, 5,
       "interval minute to second (5)"},
      {402653194, Family::kDaySecond, Qualifier::kMinuteToSecond, 6,
       "interval minute to second (6)"},
  };
  if (!identity.raw || !identity.formatted || constraint->raw_type ||
      constraint->raw_precision || constraint->raw_datetime_precision)
    return;
  for (const auto& item : qualified) {
    if (*identity.raw == item.raw && constraint->family == item.family &&
        *identity.formatted == item.formatted) {
      constraint->state = RedshiftIntervalState::kKnown;
      constraint->qualifier = item.qualifier;
      constraint->precision = item.precision;
      return;
    }
  }
}

inline Status ApplyIntervalCatalogObservation(
    const PqResultHelper& result,
    const std::vector<RedshiftIntervalCatalogColumn>& identity, PostgresType* selected) {
  if (result.NumColumns() != 5)
    return Status(ADBC_STATUS_INVALID_DATA, "Interval SVV observation shape changed");
  PostgresType output(PostgresTypeId::kRecord);
  for (int64_t col = 0; col < selected->n_children(); ++col) {
    auto type = selected->child(col);
    if (type.interval_family() != RedshiftIntervalFamily::kNone) {
      const RedshiftIntervalCatalogColumn* expected = nullptr;
      for (const auto& value : identity)
        if (value.name == type.field_name()) expected = &value;
      if (!expected || expected->oid != type.oid() ||
          expected->raw != type.type_modifier())
        return Status(ADBC_STATUS_INVALID_DATA, "Interval target identity missing");
      bool found = false;
      auto constraint = type.interval_constraint();
      constraint.state = RedshiftIntervalState::kUnknown;
      constraint.qualifier = RedshiftIntervalQualifier::kNone;
      constraint.precision.reset();
      constraint.raw_type.reset();
      constraint.raw_precision.reset();
      constraint.raw_datetime_precision.reset();
      for (int row = 0; row < result.NumRows(); ++row) {
        const auto item = result.Row(row);
        if (item[0].is_null || item[1].is_null)
          return Status(ADBC_STATUS_INVALID_DATA, "Missing interval SVV identity");
        int64_t ordinal;
        UNWRAP_RESULT(ordinal, item[0].ParseInteger());
        if (item[1].value() != expected->name) continue;
        if (found || ordinal != expected->ordinal)
          return Status(ADBC_STATUS_INVALID_DATA,
                        "Duplicate/changed interval SVV column");
        found = true;
        if (!item[2].is_null) constraint.raw_type = item[2].value();
        if (!item[3].is_null) constraint.raw_precision = item[3].value();
        if (!item[4].is_null) constraint.raw_datetime_precision = item[4].value();
      }
      if (!found)
        return Status(ADBC_STATUS_INVALID_DATA, "Interval SVV column not visible");
      // Promote only actual target catalog facts, never a source declaration.
      QualifyIntervalCatalogTarget(*expected, &constraint);
      type = type.WithIntervalConstraint(std::move(constraint));
    }
    output.AppendChild(type.field_name(), type);
  }
  *selected = std::move(output);
  return Status::Ok();
}

inline Status ObserveIntervalTargets(PGconn* conn, const std::string& escaped_table,
                                     const PostgresType& table, PostgresType* selected) {
  bool interval = false;
  for (int64_t col = 0; col < selected->n_children(); ++col)
    interval |= selected->child(col).interval_family() != RedshiftIntervalFamily::kNone;
  if (!interval) return Status::Ok();
  const std::string query =
      "SELECT current_database(), ns.nspname, cls.relname, cls.oid, attr.attnum, "
      "attr.attname, attr.atttypid, attr.atttypmod, "
      "pg_catalog.format_type(attr.atttypid, attr.atttypmod) "
      "FROM pg_catalog.pg_attribute attr "
      "JOIN pg_catalog.pg_class cls ON cls.oid=attr.attrelid "
      "JOIN pg_catalog.pg_namespace ns ON ns.oid=cls.relnamespace "
      "WHERE cls.oid=$1::regclass::oid AND attr.attnum>0 AND NOT attr.attisdropped "
      "ORDER BY attr.attnum";
  PqResultHelper before(conn, query, true);
  UNWRAP_STATUS(before.Execute({escaped_table}));
  std::vector<RedshiftIntervalCatalogColumn> identity;
  UNWRAP_STATUS(ParseIntervalCatalogIdentity(before, table, &identity));
  if (identity.empty())
    return Status(ADBC_STATUS_INVALID_DATA, "Interval relation missing");
  // Bound exact observed names; never join leader-only identity functions into
  // a query of SVV_COLUMNS or infer temporary namespace from a caller name.
  PqResultHelper detail(
      conn,
      "SELECT ordinal_position, column_name, interval_type, interval_precision, "
      "datetime_precision FROM svv_columns WHERE table_catalog=$1 AND table_schema=$2 "
      "AND table_name=$3 ORDER BY ordinal_position",
      true);
  UNWRAP_STATUS(
      detail.Execute({identity[0].catalog, identity[0].schema, identity[0].table}));
  PostgresType observed = *selected;
  UNWRAP_STATUS(ApplyIntervalCatalogObservation(detail, identity, &observed));
  PqResultHelper after(conn, query, true);
  UNWRAP_STATUS(after.Execute({escaped_table}));
  std::vector<RedshiftIntervalCatalogColumn> fresh;
  UNWRAP_STATUS(ParseIntervalCatalogIdentity(after, table, &fresh));
  if (identity != fresh)
    return Status(ADBC_STATUS_INVALID_STATE,
                  "Interval relation changed during observation");
  // These reads detect observed changes, not all external concurrent DDL. The
  // existing transaction owner is unchanged; qualification uses stable targets.
  *selected = std::move(observed);
  return Status::Ok();
}
}  // namespace adbcpq
