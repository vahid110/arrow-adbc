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
  return NANOARROW_OK;
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
}  // namespace adbcpq::testing
