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

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include "redshift_decimal.h"
#include "redshift_temporal.h"

namespace adbcpq {

enum class RedshiftScalarKind {
  kNone,
  kSmallint,
  kInteger,
  kBigint,
  kReal,
  kDouble,
  kBoolean
};

// The explicit canonical marker opts into Redshift's closed scalar contract on
// either backend. Unmarked PostgreSQL schemas retain their existing behavior.
struct RedshiftScalarSchema {
  RedshiftScalarKind kind = RedshiftScalarKind::kNone;
  bool enabled() const { return kind != RedshiftScalarKind::kNone; }

  static ArrowErrorCode Invalid(ArrowError* error, const char* reason) {
    ArrowErrorSet(error, "[Redshift] Invalid scalar write: %s", reason);
    return EINVAL;
  }

  static RedshiftScalarKind Canonical(std::string_view name) {
    if (name == "SMALLINT") return RedshiftScalarKind::kSmallint;
    if (name == "INTEGER") return RedshiftScalarKind::kInteger;
    if (name == "BIGINT") return RedshiftScalarKind::kBigint;
    if (name == "REAL") return RedshiftScalarKind::kReal;
    if (name == "DOUBLE PRECISION") return RedshiftScalarKind::kDouble;
    if (name == "BOOLEAN") return RedshiftScalarKind::kBoolean;
    return RedshiftScalarKind::kNone;
  }

  static const char* SqlName(RedshiftScalarKind kind) {
    switch (kind) {
      case RedshiftScalarKind::kSmallint:
        return "SMALLINT";
      case RedshiftScalarKind::kInteger:
        return "INTEGER";
      case RedshiftScalarKind::kBigint:
        return "BIGINT";
      case RedshiftScalarKind::kReal:
        return "REAL";
      case RedshiftScalarKind::kDouble:
        return "DOUBLE PRECISION";
      case RedshiftScalarKind::kBoolean:
        return "BOOLEAN";
      default:
        return "";
    }
  }

  static bool Integral(RedshiftScalarKind kind) {
    return kind == RedshiftScalarKind::kSmallint ||
           kind == RedshiftScalarKind::kInteger || kind == RedshiftScalarKind::kBigint;
  }
  static bool Floating(RedshiftScalarKind kind) {
    return kind == RedshiftScalarKind::kReal || kind == RedshiftScalarKind::kDouble;
  }
  static bool Unsigned(ArrowType type) {
    return type == NANOARROW_TYPE_UINT8 || type == NANOARROW_TYPE_UINT16 ||
           type == NANOARROW_TYPE_UINT32 || type == NANOARROW_TYPE_UINT64;
  }
  static RedshiftScalarKind Storage(ArrowType type) {
    switch (type) {
      case NANOARROW_TYPE_INT8:
      case NANOARROW_TYPE_UINT8:
      case NANOARROW_TYPE_INT16:
        return RedshiftScalarKind::kSmallint;
      case NANOARROW_TYPE_UINT16:
      case NANOARROW_TYPE_INT32:
        return RedshiftScalarKind::kInteger;
      case NANOARROW_TYPE_UINT32:
      case NANOARROW_TYPE_UINT64:
      case NANOARROW_TYPE_INT64:
        return RedshiftScalarKind::kBigint;
      case NANOARROW_TYPE_HALF_FLOAT:
      case NANOARROW_TYPE_FLOAT:
        return RedshiftScalarKind::kReal;
      case NANOARROW_TYPE_DOUBLE:
        return RedshiftScalarKind::kDouble;
      case NANOARROW_TYPE_BOOL:
        return RedshiftScalarKind::kBoolean;
      default:
        return RedshiftScalarKind::kNone;
    }
  }
  static bool Compatible(RedshiftScalarKind source, RedshiftScalarKind target) {
    return (Integral(source) && Integral(target)) ||
           (Floating(source) && Floating(target)) ||
           (source == RedshiftScalarKind::kBoolean && target == source);
  }
  static ArrowErrorCode Parse(const ArrowSchema* schema, bool redshift,
                              RedshiftScalarSchema* out, ArrowError* error) {
    *out = {};
    ArrowSchemaView view{};
    NANOARROW_RETURN_NOT_OK(ArrowSchemaViewInit(&view, schema, error));
    std::optional<std::string_view> family;
    NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
        schema, "ADBC:redshift:sql_type", &family, error));
    if (view.extension_name.data &&
        RedshiftTemporalSchema::TemporalExtension(
            std::string_view(view.extension_name.data, view.extension_name.size_bytes)))
      return NANOARROW_OK;
    const auto storage = Storage(view.type);
    const auto marked = family ? Canonical(*family) : RedshiftScalarKind::kNone;
    if (!redshift && marked == RedshiftScalarKind::kNone) return NANOARROW_OK;
    if (storage == RedshiftScalarKind::kNone && marked == RedshiftScalarKind::kNone)
      return NANOARROW_OK;
    if (family && marked == RedshiftScalarKind::kNone)
      return Invalid(error, "scalar storage contradicts SQL family");
    out->kind = marked == RedshiftScalarKind::kNone ? storage : marked;
    if (!Compatible(storage, out->kind))
      return Invalid(error, "unsupported scalar storage/family conversion");
    if (view.extension_name.data) return Invalid(error, "unsupported scalar extension");
    return NANOARROW_OK;
  }
};

// Check signed/unsigned endpoints before any narrowing. An unsigned high bit
// never passes through a signed getter or out-of-range cast.
inline ArrowErrorCode RedshiftScalarInteger(const ArrowArrayView* view, int64_t row,
                                            RedshiftScalarKind target, int64_t* out,
                                            ArrowError* error) {
  const int64_t minimum = target == RedshiftScalarKind::kSmallint  ? INT16_MIN
                          : target == RedshiftScalarKind::kInteger ? INT32_MIN
                                                                   : INT64_MIN;
  const int64_t maximum = target == RedshiftScalarKind::kSmallint  ? INT16_MAX
                          : target == RedshiftScalarKind::kInteger ? INT32_MAX
                                                                   : INT64_MAX;
  if (RedshiftScalarSchema::Unsigned(view->storage_type)) {
    const uint64_t value = ArrowArrayViewGetUIntUnsafe(view, row);
    if (value > static_cast<uint64_t>(maximum))
      return RedshiftScalarSchema::Invalid(error, "unsigned integer exceeds target");
    *out = static_cast<int64_t>(value);
  } else {
    const int64_t value = ArrowArrayViewGetIntUnsafe(view, row);
    if (value < minimum || value > maximum)
      return RedshiftScalarSchema::Invalid(error, "signed integer exceeds target");
    *out = value;
  }
  return NANOARROW_OK;
}

inline ArrowErrorCode RedshiftScalarFloat32(double value, float* out, ArrowError* error) {
  static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
  static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
  if (std::isfinite(value)) {
    const double limit = (std::numeric_limits<float>::max)();
    if (value < -limit || value > limit)
      return RedshiftScalarSchema::Invalid(error, "finite float32 overflow");
    const float narrowed = static_cast<float>(value);
    if (static_cast<double>(narrowed) != value ||
        (value == 0 && std::signbit(narrowed) != std::signbit(value)))
      return RedshiftScalarSchema::Invalid(error, "inexact float32 narrowing");
    *out = narrowed;
  } else {
    // NaN/Infinity are deliberate numeric semantics, not payload-bit promises.
    *out = static_cast<float>(value);
  }
  return NANOARROW_OK;
}
}  // namespace adbcpq
