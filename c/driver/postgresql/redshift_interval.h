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

#include <cerrno>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "driver/pgwire/backend.h"
#include "redshift_decimal.h"

namespace adbcpq {
using RedshiftIntervalFamily = adbc::driver::pgwire::CatalogIntervalFamily;
enum class RedshiftIntervalState { kUnspecified, kKnown, kUnknown };
enum class RedshiftIntervalQualifier {
  kNone,
  kYear,
  kMonth,
  kYearToMonth,
  kDay,
  kHour,
  kMinute,
  kSecond,
  kDayToHour,
  kDayToMinute,
  kDayToSecond,
  kHourToMinute,
  kHourToSecond,
  kMinuteToSecond
};

struct RedshiftIntervalConstraint {
  RedshiftIntervalFamily family = RedshiftIntervalFamily::kNone;
  RedshiftIntervalState state = RedshiftIntervalState::kUnknown;
  RedshiftIntervalQualifier qualifier = RedshiftIntervalQualifier::kNone;
  std::optional<int32_t> precision;
  std::optional<std::string> raw_type, raw_precision, raw_datetime_precision;

  static constexpr std::string_view QualifierName(RedshiftIntervalQualifier q) {
    constexpr std::string_view names[] = {"",
                                          "YEAR",
                                          "MONTH",
                                          "YEAR TO MONTH",
                                          "DAY",
                                          "HOUR",
                                          "MINUTE",
                                          "SECOND",
                                          "DAY TO HOUR",
                                          "DAY TO MINUTE",
                                          "DAY TO SECOND",
                                          "HOUR TO MINUTE",
                                          "HOUR TO SECOND",
                                          "MINUTE TO SECOND"};
    const auto index = static_cast<unsigned>(q);
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : std::string_view{};
  }
  static RedshiftIntervalQualifier Qualifier(std::string_view name) {
    for (int i = 1; i <= 13; ++i) {
      auto q = static_cast<RedshiftIntervalQualifier>(i);
      if (QualifierName(q) == name) return q;
    }
    return RedshiftIntervalQualifier::kNone;
  }
  static constexpr const char* FamilyName(RedshiftIntervalFamily family) {
    return family == RedshiftIntervalFamily::kYearMonth   ? "intervaly2m"
           : family == RedshiftIntervalFamily::kDaySecond ? "intervald2s"
                                                          : "";
  }
  static constexpr const char* SqlName(RedshiftIntervalFamily family) {
    return family == RedshiftIntervalFamily::kYearMonth   ? "INTERVAL YEAR TO MONTH"
           : family == RedshiftIntervalFamily::kDaySecond ? "INTERVAL DAY TO SECOND"
                                                          : "";
  }
  static constexpr const char* StateName(RedshiftIntervalState state) {
    return state == RedshiftIntervalState::kUnspecified ? "unspecified"
           : state == RedshiftIntervalState::kKnown     ? "known"
                                                        : "unknown";
  }
  static bool HasSecond(RedshiftIntervalQualifier q) {
    return q == RedshiftIntervalQualifier::kSecond ||
           q == RedshiftIntervalQualifier::kDayToSecond ||
           q == RedshiftIntervalQualifier::kHourToSecond ||
           q == RedshiftIntervalQualifier::kMinuteToSecond;
  }
  static ArrowErrorCode Invalid(ArrowError* error, const char* reason) {
    ArrowErrorSet(error, "[Redshift] Invalid interval contract: %s", reason);
    return EINVAL;
  }
  ArrowErrorCode Validate(ArrowError* error) const {
    if (family != RedshiftIntervalFamily::kYearMonth &&
        family != RedshiftIntervalFamily::kDaySecond)
      return Invalid(error, "unknown family");
    if (state == RedshiftIntervalState::kUnspecified) {
      if (qualifier != RedshiftIntervalQualifier::kNone || precision || raw_type ||
          raw_precision || raw_datetime_precision)
        return Invalid(error, "unspecified constraint has facts");
      return NANOARROW_OK;
    }
    if (state != RedshiftIntervalState::kKnown &&
        state != RedshiftIntervalState::kUnknown)
      return Invalid(error, "unknown state");
    if (qualifier == RedshiftIntervalQualifier::kNone) {
      if (precision || state == RedshiftIntervalState::kKnown)
        return Invalid(error, "missing qualifier");
      return NANOARROW_OK;
    }
    if (QualifierName(qualifier).empty()) return Invalid(error, "unknown qualifier");
    const bool year_month = qualifier == RedshiftIntervalQualifier::kYear ||
                            qualifier == RedshiftIntervalQualifier::kMonth ||
                            qualifier == RedshiftIntervalQualifier::kYearToMonth;
    if (year_month != (family == RedshiftIntervalFamily::kYearMonth))
      return Invalid(error, "qualifier and family conflict");
    if (precision && (!HasSecond(qualifier) || *precision < 0 || *precision > 6))
      return Invalid(error, "misplaced fractional precision");
    if (state == RedshiftIntervalState::kKnown && HasSecond(qualifier) && !precision)
      return Invalid(error, "missing fractional precision");
    return NANOARROW_OK;
  }
  ArrowErrorCode RequireKnown(ArrowError* error) const {
    NANOARROW_RETURN_NOT_OK(Validate(error));
    if (state != RedshiftIntervalState::kKnown) {
      ArrowErrorSet(error, "[Redshift] Actual interval target constraints are unknown");
      return ENOTSUP;
    }
    return NANOARROW_OK;
  }
  ArrowErrorCode CheckValue(int64_t value, ArrowError* error) const {
    NANOARROW_RETURN_NOT_OK(Validate(error));
    if (state != RedshiftIntervalState::kKnown) return NANOARROW_OK;
    int64_t quantum = 1;
    if (qualifier == RedshiftIntervalQualifier::kYear)
      quantum = 12;
    else if (qualifier == RedshiftIntervalQualifier::kDay)
      quantum = 86400000000;
    else if (qualifier == RedshiftIntervalQualifier::kHour ||
             qualifier == RedshiftIntervalQualifier::kDayToHour)
      quantum = 3600000000;
    else if (qualifier == RedshiftIntervalQualifier::kMinute ||
             qualifier == RedshiftIntervalQualifier::kDayToMinute ||
             qualifier == RedshiftIntervalQualifier::kHourToMinute)
      quantum = 60000000;
    else if (HasSecond(qualifier))
      for (int i = *precision; i < 6; ++i) quantum *= 10;
    if (value % quantum) return Invalid(error, "inexact qualifier value");
    return NANOARROW_OK;
  }
  ArrowErrorCode Declaration(std::string* out, ArrowError* error) const {
    NANOARROW_RETURN_NOT_OK(Validate(error));
    if (state == RedshiftIntervalState::kUnknown)
      return Invalid(error, "unknown source constraints");
    if (state == RedshiftIntervalState::kUnspecified) {
      *out = family == RedshiftIntervalFamily::kYearMonth ? "INTERVAL YEAR TO MONTH"
                                                          : "INTERVAL DAY TO SECOND(6)";
    } else {
      *out = "INTERVAL " + std::string(QualifierName(qualifier));
      if (HasSecond(qualifier)) *out += "(" + std::to_string(*precision) + ")";
    }
    return NANOARROW_OK;
  }
};

struct RedshiftIntervalSchema {
  RedshiftIntervalConstraint constraint;
  ArrowType storage = NANOARROW_TYPE_UNINITIALIZED;
  ArrowTimeUnit unit = NANOARROW_TIME_UNIT_MICRO;
  bool enabled() const { return constraint.family != RedshiftIntervalFamily::kNone; }

  static bool IntervalStorage(ArrowType type) {
    return type == NANOARROW_TYPE_DURATION || type == NANOARROW_TYPE_INTERVAL_MONTHS ||
           type == NANOARROW_TYPE_INTERVAL_DAY_TIME ||
           type == NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO;
  }
  static ArrowErrorCode Parse(const ArrowSchema* schema, bool redshift,
                              RedshiftIntervalSchema* out, ArrowError* error,
                              bool read = false) {
    *out = {};
    ArrowSchemaView view{};
    NANOARROW_RETURN_NOT_OK(ArrowSchemaViewInit(&view, schema, error));
    std::optional<std::string_view> family, sql, version, state, qualifier, precision,
        raw, name, extension_metadata;
    auto get = [&](const char* key, std::optional<std::string_view>* value) {
      return RedshiftDecimalSchema::Metadata(schema, key, value, error);
    };
    NANOARROW_RETURN_NOT_OK(get("ADBC:redshift:interval_family", &family));
    NANOARROW_RETURN_NOT_OK(get("ADBC:redshift:sql_type", &sql));
    NANOARROW_RETURN_NOT_OK(get("ADBC:redshift:interval_contract_version", &version));
    NANOARROW_RETURN_NOT_OK(get("ADBC:redshift:interval_constraint_state", &state));
    NANOARROW_RETURN_NOT_OK(get("ADBC:redshift:interval_qualifier", &qualifier));
    NANOARROW_RETURN_NOT_OK(get("ADBC:redshift:fractional_precision", &precision));
    const char* diagnostic_keys[] = {"ADBC:redshift:interval_raw_type",
                                     "ADBC:redshift:interval_raw_precision",
                                     "ADBC:redshift:interval_raw_datetime_precision"};
    std::optional<std::string_view> diagnostics[3];
    for (int i = 0; i < 3; ++i)
      NANOARROW_RETURN_NOT_OK(get(diagnostic_keys[i], &diagnostics[i]));
    const bool marked = family || version || state || qualifier || diagnostics[0] ||
                        diagnostics[1] || diagnostics[2] ||
                        (sql && sql->starts_with("INTERVAL"));
    if (!marked) {
      if (redshift && IntervalStorage(view.type)) {
        ArrowErrorSet(
            error, "[Redshift] Interval storage requires explicit family/version/state");
        return ENOTSUP;
      }
      return NANOARROW_OK;
    }
    if (!family || !version || *version != "1" || !state)
      return RedshiftIntervalConstraint::Invalid(
          error, "missing/unsupported family/version/state");
    auto& c = out->constraint;
    c.family = *family == "intervaly2m"   ? RedshiftIntervalFamily::kYearMonth
               : *family == "intervald2s" ? RedshiftIntervalFamily::kDaySecond
                                          : RedshiftIntervalFamily::kNone;
    if (c.family == RedshiftIntervalFamily::kNone ||
        (sql && *sql != RedshiftIntervalConstraint::SqlName(c.family)))
      return RedshiftIntervalConstraint::Invalid(error, "family/SQL identity conflict");
    if (*state == "unspecified")
      c.state = RedshiftIntervalState::kUnspecified;
    else if (*state == "known")
      c.state = RedshiftIntervalState::kKnown;
    else if (*state == "unknown")
      c.state = RedshiftIntervalState::kUnknown;
    else
      return RedshiftIntervalConstraint::Invalid(error, "unknown state");
    if (qualifier) {
      c.qualifier = RedshiftIntervalConstraint::Qualifier(*qualifier);
      if (c.qualifier == RedshiftIntervalQualifier::kNone)
        return RedshiftIntervalConstraint::Invalid(error, "unknown qualifier");
    }
    if (precision) {
      if (precision->size() != 1 || (*precision)[0] < '0' || (*precision)[0] > '6')
        return RedshiftIntervalConstraint::Invalid(error, "invalid precision digit");
      c.precision = (*precision)[0] - '0';
    }
    NANOARROW_RETURN_NOT_OK(get("ADBC:redshift:type_modifier", &raw));
    if (diagnostics[0]) c.raw_type = std::string(*diagnostics[0]);
    if (diagnostics[1]) c.raw_precision = std::string(*diagnostics[1]);
    if (diagnostics[2]) c.raw_datetime_precision = std::string(*diagnostics[2]);
    NANOARROW_RETURN_NOT_OK(c.Validate(error));
    if (c.state == RedshiftIntervalState::kUnspecified && raw)
      return RedshiftIntervalConstraint::Invalid(
          error, "unspecified source has raw diagnostics");
    NANOARROW_RETURN_NOT_OK(get("ARROW:extension:name", &name));
    NANOARROW_RETURN_NOT_OK(get("ARROW:extension:metadata", &extension_metadata));
    if (name || extension_metadata || schema->n_children || schema->dictionary ||
        (c.family == RedshiftIntervalFamily::kYearMonth &&
         view.type != NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO) ||
        (c.family == RedshiftIntervalFamily::kDaySecond &&
         view.type != NANOARROW_TYPE_DURATION))
      return RedshiftIntervalConstraint::Invalid(
          error, "unsupported interval storage/extension");
    out->storage = view.type;
    out->unit = view.time_unit;
    if (view.type == NANOARROW_TYPE_DURATION &&
        view.time_unit != NANOARROW_TIME_UNIT_SECOND &&
        view.time_unit != NANOARROW_TIME_UNIT_MILLI &&
        view.time_unit != NANOARROW_TIME_UNIT_MICRO &&
        view.time_unit != NANOARROW_TIME_UNIT_NANO)
      return RedshiftIntervalConstraint::Invalid(error, "unsupported duration unit");
    if (!read) {
      if (!redshift) {
        ArrowErrorSet(error,
                      "Redshift interval reference writes on PostgreSQL are unsupported");
        return ENOTSUP;
      }
      if (c.state == RedshiftIntervalState::kUnknown || raw || c.raw_type ||
          c.raw_precision || c.raw_datetime_precision) {
        ArrowErrorSet(error,
                      "[Redshift] Unknown/unqualified interval source constraints");
        return ENOTSUP;
      }
    }
    return NANOARROW_OK;
  }
  static ArrowErrorCode ValidateGenuineNullSource(const ArrowSchema* schema,
                                                  ArrowError* error) {
    ArrowSchemaView view{};
    NANOARROW_RETURN_NOT_OK(ArrowSchemaViewInit(&view, schema, error));
    if (view.type != NANOARROW_TYPE_NA || schema->n_children || schema->dictionary)
      return RedshiftIntervalConstraint::Invalid(
          error, "null source has non-null/dictionary storage");
    ArrowMetadataReader metadata;
    NANOARROW_RETURN_NOT_OK(ArrowMetadataReaderInit(&metadata, schema->metadata));
    if (metadata.remaining_keys < 0)
      return RedshiftIntervalConstraint::Invalid(error, "malformed null metadata");
    ArrowStringView key{}, value{};
    while (metadata.remaining_keys > 0) {
      NANOARROW_RETURN_NOT_OK(ArrowMetadataReaderRead(&metadata, &key, &value));
      const std::string_view name(key.data, key.size_bytes);
      if (name.starts_with("ADBC:redshift:") || name == "ARROW:extension:name" ||
          name == "ARROW:extension:metadata")
        return RedshiftIntervalConstraint::Invalid(
            error, "null source has semantic/extension metadata");
    }
    return NANOARROW_OK;
  }
  ArrowErrorCode ToNative(const ArrowArrayView* view, int64_t index, int64_t* out,
                          ArrowError* error) const {
    if (constraint.family == RedshiftIntervalFamily::kYearMonth) {
      ArrowInterval value{};
      ArrowIntervalInit(&value, NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO);
      ArrowArrayViewGetIntervalUnsafe(view, index, &value);
      if (value.days || value.ns)
        return RedshiftIntervalConstraint::Invalid(
            error, "nonzero month-family day/ns components");
      *out = value.months;
    } else {
      const int64_t value = ArrowArrayViewGetIntUnsafe(view, index);
      if (unit == NANOARROW_TIME_UNIT_NANO) {
        if (value % 1000)
          return RedshiftIntervalConstraint::Invalid(error, "submicrosecond loss");
        *out = value / 1000;
      } else {
        const int64_t factor = unit == NANOARROW_TIME_UNIT_SECOND  ? 1000000
                               : unit == NANOARROW_TIME_UNIT_MILLI ? 1000
                                                                   : 1;
        if (value > INT64_MAX / factor || value < INT64_MIN / factor)
          return RedshiftIntervalConstraint::Invalid(error, "duration unit overflow");
        *out = value * factor;
      }
    }
    return constraint.CheckValue(*out, error);
  }
};
}  // namespace adbcpq
