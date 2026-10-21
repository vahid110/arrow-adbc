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
#include <type_traits>
#include <vector>

#include "redshift_decimal_test_fixture.h"
#include "redshift_numeric_test_fixture.h"
#include "type_modifier_test_fixture.h"

namespace adbcpq::testing {
inline constexpr uint32_t kScalarOids[] = {74001, 74002, 74003, 74004, 74005, 74006};
inline constexpr const char* kScalarNames[] = {"int2",   "int4",   "int8",
                                               "float4", "float8", "bool"};
inline constexpr const char* kScalarSqlNames[] = {
    "SMALLINT", "INTEGER", "BIGINT", "REAL", "DOUBLE PRECISION", "BOOLEAN"};
inline constexpr RedshiftScalarKind kScalarKinds[] = {
    RedshiftScalarKind::kSmallint, RedshiftScalarKind::kInteger,
    RedshiftScalarKind::kBigint,   RedshiftScalarKind::kReal,
    RedshiftScalarKind::kDouble,   RedshiftScalarKind::kBoolean};

inline ArrowErrorCode ScalarResolver(const adbc::driver::pgwire::BackendProfile& profile,
                                     std::shared_ptr<PostgresTypeResolver>* out) {
  *out = std::make_shared<PostgresTypeResolver>(profile);
  const char* receive[] = {"int2recv",   "int4recv",   "int8recv",
                           "float4recv", "float8recv", "boolrecv"};
  for (int i = 0; i < 6; i++) {
    NANOARROW_RETURN_NOT_OK(
        (*out)->Insert({kScalarOids[i], kScalarNames[i], receive[i], 0, 0, 0}, nullptr));
  }
  return (*out)->Insert({74007, "text", "textrecv", 0, 0, 0}, nullptr);
}

inline ArrowErrorCode ScalarSchema(ArrowSchema* schema, ArrowType storage,
                                   const char* marker = nullptr) {
  ArrowSchemaInit(schema);
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(schema, 1));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetType(schema->children[0], storage));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(schema->children[0], "amount"));
  if (marker)
    return DecimalMetadata(schema->children[0], "ADBC:redshift:sql_type", marker);
  return NANOARROW_OK;
}

template <typename T>
inline ArrowErrorCode ScalarBatch(ArrowSchema* schema, ArrowArray* array,
                                  const std::vector<std::optional<T>>& values,
                                  ArrowError* error) {
  NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromSchema(array, schema, error));
  NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(array));
  for (const auto& value : values) {
    if (!value) {
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(array->children[0], 1));
    } else if constexpr (std::is_floating_point_v<T>) {
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendDouble(array->children[0], *value));
    } else if constexpr (std::is_unsigned_v<T>) {
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendUInt(array->children[0], *value));
    } else {
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendInt(array->children[0], *value));
    }
    NANOARROW_RETURN_NOT_OK(ArrowArrayFinishElement(array));
  }
  return ArrowArrayFinishBuildingDefault(array, error);
}
// Independent standard PostgreSQL binary scalar reference fields, never captured
// Redshift traffic. Codec-produced Arrow arrays are imported by installed PyArrow.
inline ArrowErrorCode ScalarFixture(int batch, ArrowArray* array, ArrowSchema* schema,
                                    ArrowError* error) {
  if (batch < 0 || batch > 7) return EINVAL;
  const char* fields[6][6] = {
      {"8000", "80000000", "8000000000000000", "ff7fffff", "ffefffffffffffff", "00"},
      {"7fff", "7fffffff", "7fffffffffffffff", "7f7fffff", "7fefffffffffffff", "01"},
      {"0000", "00000000", "0000000000000000", "80000000", "8000000000000000", "00"},
      {"0001", "00000001", "0000000000000001", "00000001", "0000000000000001", "01"},
      {"ffff", "ffffffff", "ffffffffffffffff", "7f800000", "fff0000000000000", "00"},
      {"0002", "00000002", "0000000000000002", "7fc00000", "7ff8000000000000", "01"}};
  std::shared_ptr<PostgresTypeResolver> resolver;
  NANOARROW_RETURN_NOT_OK(
      ScalarResolver(adbc::driver::pgwire::BackendProfile::Redshift(), &resolver));
  PqResultHelper query(nullptr, "");
  PGresult* result = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  if (!result) return ENOMEM;
  query.SetResult(result);
  PGresAttDesc attrs[6] = {};
  for (int col = 0; col < 6; col++) {
    attrs[col].name = const_cast<char*>(kScalarNames[col]);
    attrs[col].typid = kScalarOids[col];
    attrs[col].atttypmod = -1;
    attrs[col].format = 1;
  }
  if (!PQsetResultAttrs(result, 6, attrs)) return ENOMEM;
  const int rows = batch == 2 ? 0 : (batch >= 3 ? 1 : (batch == 1 ? 2 : 7));
  for (int row = 0; row < rows; row++) {
    const bool null = batch == 3 || row == 6;
    const int reference = batch == 1 ? (row == 0 ? 5 : 3) : row;
    for (int col = 0; col < 6; col++) {
      const char* hex = null ? "" : fields[reference][col];
      if (batch == 4 && col == 5) hex = "02";
      if (batch == 5 && col == 5) hex = "0100";
      if (batch == 6 && col == 0) hex = "80";
      if (batch == 7 && col == 0) hex = "";
      const auto bytes = NumericHex(hex);
      if (!PQsetvalue(result, row, col, null ? nullptr : const_cast<char*>(bytes.data()),
                      null ? -1 : static_cast<int>(bytes.size())))
        return ENOMEM;
    }
  }
  return FixtureFromQuery(query, resolver, array, schema, error);
}
}  // namespace adbcpq::testing
