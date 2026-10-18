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
inline constexpr uint32_t kBytesOids[] = {75001, 75002, 75003};
inline constexpr const char* kBytesNames[] = {"bpchar", "varchar", "varbyte"};
inline constexpr const char* kBytesSqlNames[] = {"CHAR", "VARCHAR", "VARBYTE"};
inline constexpr RedshiftBytesKind kBytesKinds[] = {
    RedshiftBytesKind::kChar, RedshiftBytesKind::kVarchar, RedshiftBytesKind::kVarbyte};
inline ArrowErrorCode BytesResolver(const adbc::driver::pgwire::BackendProfile& profile,
                                    std::shared_ptr<PostgresTypeResolver>* out) {
  *out = std::make_shared<PostgresTypeResolver>(profile);
  const char* receive[] = {"bpcharrecv", "varcharrecv", "varbyte_recv"};
  // Deliberately insert BYTEA first: a vendor VARBYTE write must not reuse it.
  NANOARROW_RETURN_NOT_OK(
      (*out)->Insert({75004, "bytea", "bytearecv", 0, 0, 0}, nullptr));
  for (int i = 0; i < 3; ++i)
    NANOARROW_RETURN_NOT_OK(
        (*out)->Insert({kBytesOids[i], kBytesNames[i], receive[i], 0, 0, 0}, nullptr));
  return NANOARROW_OK;
}
inline ArrowErrorCode BytesSchema(ArrowSchema* schema, ArrowType storage,
                                  const char* family = nullptr,
                                  const char* width = nullptr) {
  ArrowSchemaInit(schema);
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(schema, 1));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetType(schema->children[0], storage));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(schema->children[0], "value"));
  if (family)
    NANOARROW_RETURN_NOT_OK(
        DecimalMetadata(schema->children[0], "ADBC:redshift:sql_type", family));
  if (width)
    NANOARROW_RETURN_NOT_OK(
        DecimalMetadata(schema->children[0], "ADBC:redshift:byte_width", width));
  return NANOARROW_OK;
}
inline ArrowErrorCode BytesBatch(ArrowSchema* schema, ArrowArray* array,
                                 const std::vector<std::optional<std::string>>& values,
                                 ArrowError* error) {
  NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromSchema(array, schema, error));
  NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(array));
  for (const auto& value : values) {
    if (value) {
      ArrowBufferView bytes{};
      bytes.data.data = value->data();
      bytes.size_bytes = value->size();
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendBytes(array->children[0], bytes));
    } else
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(array->children[0], 1));
    NANOARROW_RETURN_NOT_OK(ArrowArrayFinishElement(array));
  }
  return ArrowArrayFinishBuildingDefault(array, error);
}
}  // namespace adbcpq::testing
