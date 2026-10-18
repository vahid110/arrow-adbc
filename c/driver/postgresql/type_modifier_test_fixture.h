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

// Synthetic descriptors following the pinned AWS JDBC reference arithmetic.
// These are not captured Redshift/libpq descriptors or value-format evidence.
#pragma once

#include <cstdint>
#include <memory>

#include "redshift_interval_test_fixture.h"

namespace adbcpq::testing {
inline constexpr uint32_t kNumericOid = 73001;
inline constexpr uint32_t kVarcharOid = 73002;
inline constexpr uint32_t kBpcharOid = 73003;
inline constexpr uint32_t kUnknownOid = 73004;

inline ArrowErrorCode ModifierResolver(
    const adbc::driver::pgwire::BackendProfile& profile,
    std::shared_ptr<PostgresTypeResolver>* out) {
  NANOARROW_RETURN_NOT_OK(FixtureResolver(profile, out));
  NANOARROW_RETURN_NOT_OK(
      (*out)->Insert({kNumericOid, "numeric", "numeric_recv", 0, 0, 0}, nullptr));
  NANOARROW_RETURN_NOT_OK(
      (*out)->Insert({kVarcharOid, "varchar", "varcharrecv", 0, 0, 0}, nullptr));
  NANOARROW_RETURN_NOT_OK(
      (*out)->Insert({kBpcharOid, "bpchar", "bpcharrecv", 0, 0, 0}, nullptr));
  return (*out)->Insert({kUnknownOid, "varbyte", "unqualified_fixture_receive", 0, 0, 0},
                        nullptr);
}

inline ArrowErrorCode ModifierDescription(PqResultHelper* query, bool empty) {
  PGresult* result = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  if (!result) return ENOMEM;
  query->SetResult(result);
  PGresAttDesc attrs[7] = {};
  const char* names[] = {"n10", "n38", "v20", "vwide", "fixed", "months", "unknown"};
  const Oid oids[] = {kNumericOid, kNumericOid,   kVarcharOid, kVarcharOid,
                      kBpcharOid,  kYearMonthOid, kUnknownOid};
  // Independent literal descriptor examples: numeric(10,2), numeric(38,37),
  // varchar(20), result varchar(70000), char(10), uninterpreted interval/varbyte.
  const int32_t modifiers[] = {655366, 2490409, 24, 70004, 14, 12345, 99};
  for (int i = 0; i < 7; i++) {
    attrs[i].name = const_cast<char*>(names[i]);
    attrs[i].typid = oids[i];
    attrs[i].atttypmod = modifiers[i];
    attrs[i].format = 1;
  }
  if (!PQsetResultAttrs(result, 7, attrs)) return ENOMEM;
  // Null data intentionally avoids introducing a vendor numeric codec claim.
  // Arrays still flow through each production field reader and C Data export.
  if (!empty) {
    for (int i = 0; i < 7; i++) {
      if (!PQsetvalue(result, 0, i, nullptr, -1)) return ENOMEM;
    }
  }
  return NANOARROW_OK;
}

inline ArrowErrorCode ModifierFixture(int empty, ArrowArray* array, ArrowSchema* schema,
                                      ArrowError* error) {
  std::shared_ptr<PostgresTypeResolver> resolver;
  NANOARROW_RETURN_NOT_OK(
      ModifierResolver(adbc::driver::pgwire::BackendProfile::Redshift(), &resolver));
  PqResultHelper query(nullptr, "");
  NANOARROW_RETURN_NOT_OK(ModifierDescription(&query, empty != 0));
  return FixtureFromQuery(query, resolver, array, schema, error);
}
}  // namespace adbcpq::testing
