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

// Offline generated protocol fixtures, not captured Redshift/libpq traffic.
// Reference: AWS Python connector 248ffd5896c7a46bba115452dc10b8389d3ede54,
// type_utils.py intervaly2m_recv_integer / intervald2s_recv_integer.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#include <libpq-fe.h>
#include <nanoarrow/nanoarrow.hpp>

#include "postgres_type.h"
#include "result_helper.h"
#include "type_discovery.h"

namespace adbcpq::testing {

// Arbitrary fixture catalog OIDs intentionally unrelated to vendor constants.
inline constexpr uint32_t kYearMonthOid = 64001;
inline constexpr uint32_t kDaySecondOid = 65017;
inline constexpr uint32_t kIntegerOid = 67009;

inline ArrowErrorCode FixtureResolver(const adbc::driver::pgwire::BackendProfile& profile,
                                      std::shared_ptr<PostgresTypeResolver>* out) {
  PqResultHelper catalog(nullptr, "");
  PGresult* result = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  if (!result) return ENOMEM;
  catalog.SetResult(result);
  PGresAttDesc attrs[5] = {};
  const char* names[] = {"oid", "typname", "typreceive", "typbasetype", "typrelid"};
  for (int i = 0; i < 5; i++) attrs[i].name = const_cast<char*>(names[i]);
  if (!PQsetResultAttrs(result, 5, attrs)) return ENOMEM;
  // Receive labels are deliberately synthetic: no actual libpq catalog capture exists.
  const char* rows[3][5] = {
      {"64001", "intervaly2m", "unqualified_fixture_receive", "0", "0"},
      {"65017", "intervald2s", "unqualified_fixture_receive", "0", "0"},
      {"67009", "int8", "int8recv", "0", "0"},
  };
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 5; col++) {
      const char* value = rows[row][col];
      if (!PQsetvalue(result, row, col, const_cast<char*>(value),
                      static_cast<int>(std::strlen(value))))
        return ENOMEM;
    }
  }
  auto resolver = std::make_shared<PostgresTypeResolver>(profile);
  if (!InsertPgTypeResult(catalog, resolver, profile).ok()) return EINVAL;
  *out = std::move(resolver);
  return NANOARROW_OK;
}

inline ArrowErrorCode FixtureFromQuery(
    PqResultHelper& query, const std::shared_ptr<PostgresTypeResolver>& resolver,
    ArrowArray* out, ArrowSchema* schema, ArrowError* error,
    adbc::driver::pgwire::TemporalRepresentation temporal =
        adbc::driver::pgwire::TemporalRepresentation::kArrow) {
  PostgresType root;
  if (!query.ResolveOutputTypes(*resolver, &root, temporal).ok()) return EINVAL;
  nanoarrow::UniqueSchema tmp_schema;
  ArrowSchemaInit(tmp_schema.get());
  NANOARROW_RETURN_NOT_OK(root.SetSchema(tmp_schema.get(), "Redshift"));
  nanoarrow::UniqueArray array;
  NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromSchema(array.get(), tmp_schema.get(), error));
  NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(array.get()));
  for (int col = 0; col < query.NumColumns(); col++) {
    std::unique_ptr<PostgresCopyFieldReader> reader;
    NANOARROW_RETURN_NOT_OK(
        MakeCopyFieldReader(root.child(col), tmp_schema->children[col], &reader, error));
    reader->Init(root.child(col));
    NANOARROW_RETURN_NOT_OK(reader->InitSchema(tmp_schema->children[col]));
    NANOARROW_RETURN_NOT_OK(reader->InitArray(array->children[col]));
    for (int row = 0; row < query.NumRows(); row++) {
      const auto item = query.Row(row)[col];
      ArrowBufferView data{};
      data.data.data = item.data;
      data.size_bytes = item.is_null ? -1 : item.len;
      NANOARROW_RETURN_NOT_OK(reader->Read(&data, static_cast<int32_t>(data.size_bytes),
                                           array->children[col], error));
    }
    NANOARROW_RETURN_NOT_OK(reader->FinishArray(array->children[col], error));
  }
  array->length = query.NumRows();
  array->null_count = 0;
  NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(array.get(), error));
  array.move(out);
  tmp_schema.move(schema);
  return NANOARROW_OK;
}

// Both C++ assertions and installed PyArrow import consume arrays made by the
// real resolver, schema builder and field readers. There is no expected-array
// construction here. Literal network-order bytes are independent test inputs.
inline ArrowErrorCode IntervalFixture(int empty, ArrowArray* out, ArrowSchema* schema,
                                      ArrowError* error) {
  auto profile = adbc::driver::pgwire::BackendProfile::Redshift();
  std::shared_ptr<PostgresTypeResolver> resolver;
  NANOARROW_RETURN_NOT_OK(FixtureResolver(profile, &resolver));
  PqResultHelper query(nullptr, "");
  PGresult* result = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  if (!result) return ENOMEM;
  query.SetResult(result);
  PGresAttDesc attrs[3] = {};
  const char* names[] = {"months", "micros", "ordinary"};
  const Oid oids[] = {kYearMonthOid, kDaySecondOid, kIntegerOid};
  for (int i = 0; i < 3; i++) {
    attrs[i].name = const_cast<char*>(names[i]);
    attrs[i].typid = oids[i];
    attrs[i].format = 1;
    attrs[i].atttypmod = -1;
  }
  if (!PQsetResultAttrs(result, 3, attrs)) return ENOMEM;
  const uint8_t months[5][4] = {{0, 0, 0, 0},
                                {0, 0, 0, 14},
                                {255, 255, 255, 242},
                                {128, 0, 0, 0},
                                {127, 255, 255, 255}};
  const uint8_t micros[5][8] = {{0, 0, 0, 0, 0, 0, 0, 0},
                                {0, 0, 0, 0, 0, 15, 66, 65},
                                {255, 255, 255, 255, 255, 240, 189, 191},
                                {128, 0, 0, 0, 0, 0, 0, 0},
                                {127, 255, 255, 255, 255, 255, 255, 255}};
  const uint8_t ordinary[8] = {0, 0, 0, 0, 0, 0, 0, 42};
  if (!empty) {
    for (int row = 0; row < 6; row++) {
      const char* m = row == 5 ? nullptr : reinterpret_cast<const char*>(months[row]);
      const char* d = row == 5 ? nullptr : reinterpret_cast<const char*>(micros[row]);
      if (!PQsetvalue(result, row, 0, const_cast<char*>(m), row == 5 ? -1 : 4) ||
          !PQsetvalue(result, row, 1, const_cast<char*>(d), row == 5 ? -1 : 8) ||
          !PQsetvalue(result, row, 2,
                      const_cast<char*>(reinterpret_cast<const char*>(ordinary)), 8)) {
        return ENOMEM;
      }
    }
  }
  return FixtureFromQuery(query, resolver, out, schema, error);
}

}  // namespace adbcpq::testing
