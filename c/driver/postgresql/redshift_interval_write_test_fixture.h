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
#include <string_view>
#include <vector>
#include "redshift_decimal_test_fixture.h"
#include "redshift_interval_test_fixture.h"

namespace adbcpq::testing {
inline ArrowErrorCode IntervalSetMetadata(ArrowSchema* field, const char* key,
                                          const char* value) {
  nanoarrow::UniqueBuffer buffer;
  NANOARROW_RETURN_NOT_OK(ArrowMetadataBuilderInit(buffer.get(), field->metadata));
  NANOARROW_RETURN_NOT_OK(
      ArrowMetadataBuilderSet(buffer.get(), ArrowCharView(key), ArrowCharView(value)));
  return ArrowSchemaSetMetadata(field, reinterpret_cast<char*>(buffer->data));
}
// Explicit internal reference constraints, never actual catalog observations.
inline RedshiftIntervalConstraint IntervalReferenceConstraint(
    RedshiftIntervalFamily family, RedshiftIntervalQualifier qualifier,
    std::optional<int32_t> precision = {}) {
  RedshiftIntervalConstraint out;
  out.family = family;
  out.state = RedshiftIntervalState::kKnown;
  out.qualifier = qualifier;
  out.precision = precision;
  return out;
}
inline ArrowErrorCode IntervalWriteSchema(
    ArrowSchema* schema, RedshiftIntervalFamily family, const char* state = "unspecified",
    const char* qualifier = nullptr, const char* precision = nullptr,
    ArrowTimeUnit unit = NANOARROW_TIME_UNIT_MICRO) {
  ArrowSchemaInit(schema);
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(schema, 1));
  auto* child = schema->children[0];
  if (family == RedshiftIntervalFamily::kYearMonth)
    NANOARROW_RETURN_NOT_OK(
        ArrowSchemaSetType(child, NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO));
  else
    NANOARROW_RETURN_NOT_OK(
        ArrowSchemaSetTypeDateTime(child, NANOARROW_TYPE_DURATION, unit, nullptr));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(child, "amount"));
  NANOARROW_RETURN_NOT_OK(
      DecimalMetadata(child, "ADBC:redshift:interval_family",
                      RedshiftIntervalConstraint::FamilyName(family)));
  NANOARROW_RETURN_NOT_OK(
      DecimalMetadata(child, "ADBC:redshift:interval_contract_version", "1"));
  NANOARROW_RETURN_NOT_OK(
      DecimalMetadata(child, "ADBC:redshift:interval_constraint_state", state));
  NANOARROW_RETURN_NOT_OK(DecimalMetadata(child, "ADBC:redshift:sql_type",
                                          RedshiftIntervalConstraint::SqlName(family)));
  if (qualifier)
    NANOARROW_RETURN_NOT_OK(
        DecimalMetadata(child, "ADBC:redshift:interval_qualifier", qualifier));
  if (precision)
    NANOARROW_RETURN_NOT_OK(
        DecimalMetadata(child, "ADBC:redshift:fractional_precision", precision));
  return NANOARROW_OK;
}
inline ArrowErrorCode IntervalWriteBatch(
    ArrowSchema* schema, ArrowArray* array,
    const std::vector<std::optional<int64_t>>& values, ArrowError* error,
    int32_t days = 0, int64_t nanos = 0) {
  NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromSchema(array, schema, error));
  NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(array));
  for (auto value : values) {
    if (!value)
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(array->children[0], 1));
    else if (std::string_view(schema->children[0]->format) == "tin") {
      if (*value < INT32_MIN || *value > INT32_MAX) return EINVAL;
      ArrowInterval interval{};
      ArrowIntervalInit(&interval, NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO);
      interval.months = static_cast<int32_t>(*value);
      interval.days = days;
      interval.ns = nanos;
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendInterval(array->children[0], &interval));
    } else
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendInt(array->children[0], *value));
    NANOARROW_RETURN_NOT_OK(ArrowArrayFinishElement(array));
  }
  return ArrowArrayFinishBuildingDefault(array, error);
}
}  // namespace adbcpq::testing
