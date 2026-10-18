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
// Independent raw standard PostgreSQL field bytes; not captured VARBYTE or
// native Redshift CHAR traffic. No production writer is the read-test oracle.
inline ArrowErrorCode BytesFixture(int batch, ArrowArray* array, ArrowSchema* schema,
                                   ArrowError* error) {
  if (batch < 0 || batch > 7) return EINVAL;
  std::shared_ptr<PostgresTypeResolver> resolver;
  NANOARROW_RETURN_NOT_OK(
      BytesResolver(adbc::driver::pgwire::BackendProfile::Redshift(), &resolver));
  PqResultHelper query(nullptr, "");
  PGresult* result = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  if (!result) return ENOMEM;
  query.SetResult(result);
  PGresAttDesc attrs[3] = {};
  for (int col = 0; col < 3; ++col) {
    attrs[col].name = const_cast<char*>(kBytesNames[col]);
    attrs[col].typid = kBytesOids[col];
    attrs[col].format = 1;
    attrs[col].atttypmod = col == 0   ? 8
                           : col == 1 ? (batch == 3 ? 70004 : 24)
                                      : (batch == 7 ? 16777220 : 24);
  }
  if (!PQsetResultAttrs(result, 3, attrs)) return ENOMEM;
  const int rows = batch == 1 ? 0 : (batch >= 2 ? 1 : 4);
  for (int row = 0; row < rows; ++row) {
    for (int col = 0; col < 3; ++col) {
      const bool null = batch == 2 || row == 3;
      std::string bytes;
      if (row == 0)
        bytes = NumericHex(col == 0   ? "61202020"
                           : col == 1 ? "f09f9880275c2020"
                                      : "0080ff5c78");
      if (row == 2) bytes = col == 0 ? "    " : col == 1 ? "  " : NumericHex("6162");
      if (batch == 3 && col == 1) bytes.assign(70000, 'q');
      if (batch == 4 && col == 1) bytes = NumericHex("eda080");
      if (batch == 5 && col == 2) bytes.assign(21, 'a');
      if (batch == 6 && col == 0) bytes = NumericHex("c280");
      if (batch == 7 && col == 2) {
        bytes.assign(16777216, static_cast<char>(0xff));
        bytes[0] = 0;
      }
      if (!PQsetvalue(result, row, col, null ? nullptr : bytes.data(),
                      null ? -1 : static_cast<int>(bytes.size())))
        return ENOMEM;
    }
  }
  return FixtureFromQuery(query, resolver, array, schema, error);
}
}  // namespace adbcpq::testing
