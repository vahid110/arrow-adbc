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
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "redshift_decimal_test_fixture.h"
#include "redshift_numeric_test_fixture.h"

namespace adbcpq::testing {
inline constexpr uint32_t kTemporalOids[] = {76001, 76002, 76003, 76004};
inline constexpr const char* kTemporalNames[] = {"date", "time", "timestamp",
                                                 "timestamptz"};
inline constexpr RedshiftTemporalKind kTemporalKinds[] = {
    RedshiftTemporalKind::kDate, RedshiftTemporalKind::kTime,
    RedshiftTemporalKind::kTimestamp, RedshiftTemporalKind::kTimestamptz};
inline ArrowErrorCode TemporalResolver(
    const adbc::driver::pgwire::BackendProfile& profile,
    std::shared_ptr<PostgresTypeResolver>* out) {
  *out = std::make_shared<PostgresTypeResolver>(profile);
  const char* receive[] = {"date_recv", "time_recv", "timestamp_recv",
                           "timestamptz_recv"};
  for (int i = 0; i < 4; ++i)
    NANOARROW_RETURN_NOT_OK((*out)->Insert(
        {kTemporalOids[i], kTemporalNames[i], receive[i], 0, 0, 0}, nullptr));
  return (*out)->Insert({76005, "int8", "int8recv", 0, 0, 0}, nullptr);
}
inline ArrowErrorCode TemporalSchema(ArrowSchema* schema, ArrowType storage,
                                     RedshiftTemporalKind kind,
                                     ArrowTimeUnit unit = NANOARROW_TIME_UNIT_MICRO,
                                     bool extension = false,
                                     const char* precision = nullptr,
                                     bool marker = true) {
  ArrowSchemaInit(schema);
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(schema, 1));
  auto* child = schema->children[0];
  if (storage == NANOARROW_TYPE_TIME32 || storage == NANOARROW_TYPE_TIME64 ||
      storage == NANOARROW_TYPE_TIMESTAMP)
    NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeDateTime(
        child, storage, unit,
        storage == NANOARROW_TYPE_TIMESTAMP && kind == RedshiftTemporalKind::kTimestamptz
            ? "UTC"
            : nullptr));
  else
    NANOARROW_RETURN_NOT_OK(ArrowSchemaSetType(child, storage));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(child, "value"));
  if (marker)
    NANOARROW_RETURN_NOT_OK(DecimalMetadata(child, "ADBC:redshift:sql_type",
                                            RedshiftTemporalSchema::SqlName(kind)));
  if (extension) {
    const auto name = RedshiftTemporalSchema::Extension(kind),
               metadata = RedshiftTemporalSchema::ExtensionMetadata(kind);
    NANOARROW_RETURN_NOT_OK(
        DecimalMetadata(child, "ARROW:extension:name", std::string(name).c_str()));
    NANOARROW_RETURN_NOT_OK(DecimalMetadata(child, "ARROW:extension:metadata",
                                            std::string(metadata).c_str()));
  }
  if (precision)
    NANOARROW_RETURN_NOT_OK(
        DecimalMetadata(child, "ADBC:redshift:fractional_precision", precision));
  return NANOARROW_OK;
}
inline ArrowErrorCode TemporalBatch(ArrowSchema* schema, ArrowArray* array,
                                    const std::vector<std::optional<int64_t>>& values,
                                    ArrowError* error) {
  NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromSchema(array, schema, error));
  NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(array));
  for (const auto& value : values) {
    if (value)
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendInt(array->children[0], *value));
    else
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(array->children[0], 1));
    NANOARROW_RETURN_NOT_OK(ArrowArrayFinishElement(array));
  }
  return ArrowArrayFinishBuildingDefault(array, error);
}
// Standard PostgreSQL binary reference vectors, not captured Redshift traffic.
// Decode through the real PGresult descriptor/reader path and export its C Data.
inline ArrowErrorCode TemporalFixture(int batch, ArrowArray* array, ArrowSchema* schema,
                                      ArrowError* error) {
  if (batch < 0 || batch > 4) return EINVAL;
  const bool lossless = batch == 1 || batch == 2 || batch == 4;
  const auto mode = lossless ? adbc::driver::pgwire::TemporalRepresentation::kLossless
                             : adbc::driver::pgwire::TemporalRepresentation::kArrow;
  std::shared_ptr<PostgresTypeResolver> resolver;
  NANOARROW_RETURN_NOT_OK(
      TemporalResolver(adbc::driver::pgwire::BackendProfile::Redshift(), &resolver));
  PqResultHelper query(nullptr, "");
  PGresult* result = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  if (!result) return ENOMEM;
  query.SetResult(result);
  PGresAttDesc attrs[4] = {};
  for (int col = 0; col < 4; ++col) {
    attrs[col].name = const_cast<char*>(kTemporalNames[col]);
    attrs[col].typid = kTemporalOids[col];
    attrs[col].atttypmod = col == 0 ? -1 : 6;
    attrs[col].format = 1;
  }
  if (!PQsetResultAttrs(result, 4, attrs)) return ENOMEM;
  const char* fields[4][4] = {
      {"00000000", "0000000000000000", "0000000000000000", "0000000000000000"},
      {"ffffd533", "000000141dd75fff", "fffca2fec4c82000", "fffca2fec4c82000"},
      {"fff4da8b", "000000000016e360", "ffffffffffffffff", "ffffffffffffffff"},
      {"00000001", "00000000000004d2", "0000000000000001", "0000000000000001"}};
  const int rows = batch >= 3 ? 0 : 5;
  for (int row = 0; row < rows; ++row) {
    for (int col = 0; col < 4; ++col) {
      const bool null = row == 4;
      const char* hex = null ? "" : fields[row][col];
      if (batch == 2 && !null) {
        if (col == 1 && row == 0) hex = "000000141dd76000";
        if (col >= 2 && row == 0) hex = "7ffffffffffffffe";
        if (col >= 2 && row == 1) hex = "8000000000000001";
      }
      const auto value = NumericHex(hex);
      if (!PQsetvalue(result, row, col, null ? nullptr : const_cast<char*>(value.data()),
                      null ? -1 : static_cast<int>(value.size())))
        return ENOMEM;
    }
  }
  return FixtureFromQuery(query, resolver, array, schema, error, mode);
}
}  // namespace adbcpq::testing
