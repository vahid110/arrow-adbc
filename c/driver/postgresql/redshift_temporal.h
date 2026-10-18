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

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "redshift_decimal.h"

namespace adbcpq {
enum class RedshiftTemporalKind { kNone, kDate, kTime, kTimestamp, kTimestamptz };

// Reference wire-code policy, not a claim that the remaining integer code space
// is valid Redshift SQL. Actual finite/sentinel/calendar qualification is open.
struct RedshiftTemporalSchema {
  RedshiftTemporalKind kind = RedshiftTemporalKind::kNone;
  bool native_epoch = false;
  ArrowType storage = NANOARROW_TYPE_UNINITIALIZED;
  ArrowTimeUnit unit = NANOARROW_TIME_UNIT_MICRO;
  std::optional<int32_t> precision;
  bool enabled() const { return kind != RedshiftTemporalKind::kNone; }
  static constexpr int64_t kDayUs = 86400000000;
  static constexpr int64_t kEpochUs = 946684800000000;
  static constexpr int64_t kEpochDays = 10957;

  static ArrowErrorCode Invalid(ArrowError* error, const char* reason) {
    ArrowErrorSet(error, "[Redshift] Invalid temporal value: %s", reason);
    return EINVAL;
  }
  static RedshiftTemporalKind Canonical(std::string_view family) {
    if (family == "DATE") return RedshiftTemporalKind::kDate;
    if (family == "TIME") return RedshiftTemporalKind::kTime;
    if (family == "TIMESTAMP") return RedshiftTemporalKind::kTimestamp;
    if (family == "TIMESTAMPTZ") return RedshiftTemporalKind::kTimestamptz;
    return RedshiftTemporalKind::kNone;
  }
  static const char* SqlName(RedshiftTemporalKind kind) {
    if (kind == RedshiftTemporalKind::kDate) return "DATE";
    if (kind == RedshiftTemporalKind::kTime) return "TIME";
    if (kind == RedshiftTemporalKind::kTimestamp) return "TIMESTAMP";
    if (kind == RedshiftTemporalKind::kTimestamptz) return "TIMESTAMPTZ";
    return "";
  }
  static std::string_view Extension(RedshiftTemporalKind kind) {
    if (kind == RedshiftTemporalKind::kTime) return "adbc.redshift.time_us.v1";
    if (kind == RedshiftTemporalKind::kTimestamp)
      return "adbc.redshift.timestamp_us_2000.v1";
    if (kind == RedshiftTemporalKind::kTimestamptz)
      return "adbc.redshift.timestamptz_us_2000.v1";
    return {};
  }
  static std::string_view ExtensionMetadata(RedshiftTemporalKind kind) {
    if (kind == RedshiftTemporalKind::kTime)
      return R"({"version":1,"family":"TIME","unit":"us","origin":"start_of_day","semantics":"civil"})";
    if (kind == RedshiftTemporalKind::kTimestamp)
      return R"({"version":1,"family":"TIMESTAMP","unit":"us","epoch":"2000-01-01T00:00:00","semantics":"civil"})";
    if (kind == RedshiftTemporalKind::kTimestamptz)
      return R"({"version":1,"family":"TIMESTAMPTZ","unit":"us","epoch":"2000-01-01T00:00:00Z","semantics":"utc_instant"})";
    return {};
  }
  static bool TemporalExtension(std::string_view name) {
    return name.starts_with("adbc.redshift.time");
  }
  static ArrowErrorCode Precision(std::optional<int32_t> raw, std::optional<int32_t>* out,
                                  ArrowError* error) {
    *out = {};
    if (!raw) return NANOARROW_OK;
    if (*raw == -1) {
      *out = 6;
      return NANOARROW_OK;
    }
    if (*raw < 0 || *raw > 6)
      return Invalid(error, "unsupported reference temporal modifier");
    *out = *raw;
    return NANOARROW_OK;
  }
  static ArrowErrorCode Parse(const ArrowSchema* schema, bool redshift,
                              RedshiftTemporalSchema* out, ArrowError* error) {
    *out = {};
    ArrowSchemaView view{};
    NANOARROW_RETURN_NOT_OK(ArrowSchemaViewInit(&view, schema, error));
    std::optional<std::string_view> family, name, metadata, precision, raw;
    NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
        schema, "ADBC:redshift:sql_type", &family, error));
    NANOARROW_RETURN_NOT_OK(
        RedshiftDecimalSchema::Metadata(schema, "ARROW:extension:name", &name, error));
    NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
        schema, "ADBC:redshift:fractional_precision", &precision, error));
    // Interval qualifiers reuse fractional_precision under their own closed contract.
    std::optional<std::string_view> interval_family;
    NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
        schema, "ADBC:redshift:interval_family", &interval_family, error));
    if (interval_family || (family && family->starts_with("INTERVAL")))
      return NANOARROW_OK;
    const auto marked = family ? Canonical(*family) : RedshiftTemporalKind::kNone;
    const bool temporal_extension = name && TemporalExtension(*name);
    RedshiftTemporalKind storage = RedshiftTemporalKind::kNone;
    if (view.type == NANOARROW_TYPE_DATE32 || view.type == NANOARROW_TYPE_DATE64)
      storage = RedshiftTemporalKind::kDate;
    else if (view.type == NANOARROW_TYPE_TIME32 || view.type == NANOARROW_TYPE_TIME64)
      storage = RedshiftTemporalKind::kTime;
    else if (view.type == NANOARROW_TYPE_TIMESTAMP)
      storage = view.timezone && *view.timezone ? RedshiftTemporalKind::kTimestamptz
                                                : RedshiftTemporalKind::kTimestamp;
    if (!redshift && marked == RedshiftTemporalKind::kNone && !temporal_extension)
      return precision ? Invalid(error, "misplaced fractional precision") : NANOARROW_OK;
    if (storage == RedshiftTemporalKind::kNone && marked == RedshiftTemporalKind::kNone &&
        !temporal_extension)
      return precision ? Invalid(error, "misplaced fractional precision") : NANOARROW_OK;
    out->kind = marked == RedshiftTemporalKind::kNone ? storage : marked;
    out->storage = view.type;
    out->unit = view.time_unit;
    if (temporal_extension) {
      if (family && marked == RedshiftTemporalKind::kNone)
        return Invalid(error, "extension and SQL identity conflict");
      for (auto kind : {RedshiftTemporalKind::kTime, RedshiftTemporalKind::kTimestamp,
                        RedshiftTemporalKind::kTimestamptz}) {
        if (*name == Extension(kind)) {
          if (marked != RedshiftTemporalKind::kNone && marked != kind)
            return Invalid(error, "extension and SQL identity conflict");
          out->kind = kind;
        }
      }
      NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
          schema, "ARROW:extension:metadata", &metadata, error));
      if (!out->enabled() || *name != Extension(out->kind) || !metadata ||
          *metadata != ExtensionMetadata(out->kind) ||
          view.type != NANOARROW_TYPE_INT64 || schema->n_children || schema->dictionary)
        return Invalid(error, "unsupported temporal extension version/storage/metadata");
      out->native_epoch = true;
    } else if (name || storage != out->kind ||
               (family && marked == RedshiftTemporalKind::kNone))
      return Invalid(error, "temporal storage and identity conflict");
    if (schema->n_children || schema->dictionary)
      return Invalid(error, "nested temporal storage unsupported");
    if (precision) {
      int32_t p;
      NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Integer(*precision, &p, error));
      if (out->kind == RedshiftTemporalKind::kDate || p < 0 || p > 6)
        return Invalid(error, "invalid source fractional precision");
      out->precision = p;
    }
    NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
        schema, "ADBC:redshift:type_modifier", &raw, error));
    if (raw && out->kind != RedshiftTemporalKind::kDate) {
      int32_t value;
      NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Integer(*raw, &value, error));
      std::optional<int32_t> p;
      NANOARROW_RETURN_NOT_OK(Precision(value, &p, error));
      if (out->precision && out->precision != p)
        return Invalid(error, "modifier and fractional precision conflict");
      out->precision = p;
    }
    return NANOARROW_OK;
  }
  ArrowErrorCode Declaration(std::string* out, ArrowError* error) const {
    if (precision && *precision != 6)
      return Invalid(error, "CREATE precision syntax remains unqualified");
    *out = SqlName(kind);
    return NANOARROW_OK;
  }
  static ArrowErrorCode ScaleToUs(int64_t value, ArrowTimeUnit unit, int64_t* out,
                                  ArrowError* error) {
    if (unit == NANOARROW_TIME_UNIT_NANO) {
      if (value % 1000) return Invalid(error, "submicrosecond loss");
      *out = value / 1000;
      return NANOARROW_OK;
    }
    const int64_t factor = unit == NANOARROW_TIME_UNIT_SECOND  ? 1000000
                           : unit == NANOARROW_TIME_UNIT_MILLI ? 1000
                                                               : 1;
    if (value > INT64_MAX / factor || value < INT64_MIN / factor)
      return Invalid(error, "unit conversion overflow");
    *out = value * factor;
    return NANOARROW_OK;
  }
  static ArrowErrorCode CheckPrecision(int64_t value, std::optional<int32_t> p,
                                       ArrowError* error) {
    if (!p) return NANOARROW_OK;
    int64_t factor = 1;
    for (int i = *p; i < 6; ++i) factor *= 10;
    return value % factor ? Invalid(error, "fractional precision loss") : NANOARROW_OK;
  }
  static ArrowErrorCode CheckNative(RedshiftTemporalKind kind, int64_t value,
                                    ArrowError* error) {
    if (kind == RedshiftTemporalKind::kDate) {
      if (value <= INT32_MIN || value >= INT32_MAX)
        return Invalid(error, "reserved/unqualified reference DATE code");
    } else if (kind == RedshiftTemporalKind::kTime) {
      if (value < 0 || value > kDayUs)
        return Invalid(error, "reference TIME outside closed day");
    } else if (value == INT64_MIN || value == INT64_MAX)
      return Invalid(error, "reserved/unqualified reference timestamp code");
    return NANOARROW_OK;
  }
  ArrowErrorCode ToNative(int64_t raw, int64_t* out, ArrowError* error) const {
    int64_t value = raw;
    if (!native_epoch) {
      if (kind == RedshiftTemporalKind::kDate) {
        if (storage == NANOARROW_TYPE_DATE64) {
          if (raw % 86400000) return Invalid(error, "DATE64 requires whole days");
          value = raw / 86400000;
        }
        if (value < INT64_MIN + kEpochDays) return Invalid(error, "DATE epoch overflow");
        value -= kEpochDays;
      } else if (kind == RedshiftTemporalKind::kTime) {
        NANOARROW_RETURN_NOT_OK(ScaleToUs(raw, unit, &value, error));
        if (value < 0 || value >= kDayUs)
          return Invalid(error, "standard Arrow TIME excludes endofday");
      } else if (unit == NANOARROW_TIME_UNIT_SECOND ||
                 unit == NANOARROW_TIME_UNIT_MILLI) {
        // Epoch subtraction cancels part of the source count. Subtract in its
        // exact original unit first so a valid final native count isn't rejected
        // merely because an unnecessary Unix-microsecond intermediate overflows.
        const int64_t factor = unit == NANOARROW_TIME_UNIT_SECOND ? 1000000 : 1000;
        const int64_t offset = kEpochUs / factor;
        if (raw < INT64_MIN + offset) return Invalid(error, "timestamp epoch overflow");
        NANOARROW_RETURN_NOT_OK(ScaleToUs(raw - offset, unit, &value, error));
      } else {
        NANOARROW_RETURN_NOT_OK(ScaleToUs(raw, unit, &value, error));
        if (value < INT64_MIN + kEpochUs)
          return Invalid(error, "timestamp epoch overflow");
        value -= kEpochUs;
      }
    }
    NANOARROW_RETURN_NOT_OK(CheckNative(kind, value, error));
    NANOARROW_RETURN_NOT_OK(CheckPrecision(value, precision, error));
    *out = value;
    return NANOARROW_OK;
  }
};
}  // namespace adbcpq
