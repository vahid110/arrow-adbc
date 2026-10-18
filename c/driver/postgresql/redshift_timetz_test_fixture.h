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
#include <utility>
#include <vector>
#include "redshift_interval_write_test_fixture.h"
#include "redshift_numeric_test_fixture.h"

namespace adbcpq::testing {
inline constexpr Oid kTimetzOid = 65113;
inline ArrowErrorCode TimetzResolver(const adbc::driver::pgwire::BackendProfile& profile,
                                     std::shared_ptr<PostgresTypeResolver>* out) {
  NANOARROW_RETURN_NOT_OK(FixtureResolver(profile, out));
  PostgresTypeResolver::Item item{};
  item.oid = kTimetzOid;
  item.typname = "timetz";
  item.typreceive = "timetz_recv";
  return (*out)->Insert(item, nullptr);
}
inline ArrowErrorCode TimetzWriteSchema(ArrowSchema* schema,
                                        const char* precision = "6") {
  ArrowSchemaInit(schema);
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(schema, 1));
  auto* field = schema->children[0];
  NANOARROW_RETURN_NOT_OK(RedshiftTimetzSchema::SetSchema(field));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(field, "amount"));
  NANOARROW_RETURN_NOT_OK(IntervalSetMetadata(field, "ADBC:redshift:sql_type", "TIMETZ"));
  if (precision)
    NANOARROW_RETURN_NOT_OK(
        IntervalSetMetadata(field, "ADBC:redshift:fractional_precision", precision));
  return NANOARROW_OK;
}
using TimetzValue = std::optional<std::pair<int64_t, int32_t>>;
inline ArrowErrorCode TimetzWriteBatch(const ArrowSchema* schema, ArrowArray* array,
                                       const std::vector<TimetzValue>& rows,
                                       ArrowError* error) {
  NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromSchema(array, schema, error));
  NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(array));
  for (const auto& row : rows) {
    auto* field = array->children[0];
    if (row) {
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendInt(field->children[0], row->first));
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendInt(field->children[1], row->second));
      NANOARROW_RETURN_NOT_OK(ArrowArrayFinishElement(field));
    } else
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(field, 1));
    NANOARROW_RETURN_NOT_OK(ArrowArrayFinishElement(array));
  }
  return ArrowArrayFinishBuildingDefault(array, error);
}

inline ArrowErrorCode TimetzFixture(int batch, ArrowArray* array, ArrowSchema* schema,
                                    ArrowError* error) {
  if (batch < 0 || batch > 3) return EINVAL;
  std::shared_ptr<PostgresTypeResolver> resolver;
  NANOARROW_RETURN_NOT_OK(
      TimetzResolver(adbc::driver::pgwire::BackendProfile::Redshift(), &resolver));
  PqResultHelper query(nullptr, "");
  auto* result = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  if (!result) return ENOMEM;
  query.SetResult(result);
  PGresAttDesc attrs[3]{};
  const char* names[] = {"fixed", "unknown", "milli"};
  for (int col = 0; col < 3; ++col) {
    attrs[col].name = const_cast<char*>(names[col]);
    attrs[col].typid = kTimetzOid;
    attrs[col].format = 1;
    attrs[col].atttypmod = col == 0 ? 6 : col == 1 ? 999 : 3;
  }
  if (!PQsetResultAttrs(result, 3, attrs)) return ENOMEM;
  const char* hex[] = {"000000000000000000000000", "000000049c2c0600ffffb2a8",
                       "00000000000003e90000001e", "000000141dd76000ffffaf24",
                       "000000000000000180000001", "00000000000000017fffffff"};
  const int rows = batch == 1 ? 0 : batch == 2 ? 1 : 7;
  for (int row = 0; row < rows; ++row)
    for (int col = 0; col < 3; ++col) {
      const bool null = batch == 2 || row == 6;
      std::string wire;
      if (!null) wire = NumericHex(col == 2 ? "00000000000003e80000001e" : hex[row]);
      if (!PQsetvalue(result, row, col, null ? nullptr : wire.data(), null ? -1 : 12))
        return ENOMEM;
    }
  return FixtureFromQuery(query, resolver, array, schema, error,
                          batch == 3
                              ? adbc::driver::pgwire::TemporalRepresentation::kLossless
                              : adbc::driver::pgwire::TemporalRepresentation::kArrow);
}
}  // namespace adbcpq::testing
