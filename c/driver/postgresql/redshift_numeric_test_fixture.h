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

// Literal PostgreSQL base-10000 binary reference fields for the current libpq
// path. Not captured Redshift traffic; not the negotiated AWS Python protocol.
// Reference: PostgreSQL 18 numeric_send, numeric.c at commit
// 3d6a828938a5fa0444275d3d2f67b64ec3199eb7 (no implementation copied).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "type_modifier_test_fixture.h"

namespace adbcpq::testing {

inline std::string NumericHex(const char* hex) {
  std::string out;
  for (size_t i = 0; hex[i]; i += 2) {
    auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    out.push_back(static_cast<char>(digit(hex[i]) * 16 + digit(hex[i + 1])));
  }
  return out;
}

// Each row is an independently specified literal header/digit vector. Expected
// values live separately in C++/Python tests; no production writer is used.
inline constexpr const char* kDecimalHex[3][9] = {
    {"00010000000000000009", "00050004000000000063270f270f270f270f",
     "0005000400000000039a0d2c0170156516af", "0005000400000000270f270f270f270f270f",
     "000a0009000000000063270f270f270f270f270f270f270f270f270f",
     "0002000000000004000109c4",
     "000b0000000000250009270f270f270f270f270f270f270f270f270f2328",
     "000affff00000026270f270f270f270f270f270f270f270f270f26ac",
     "000700044000000704d2162e23340d801ed2000c0bb8"},
    {"00010000400000000009", "00050004400000000063270f270f270f270f",
     "0005000440000000039a0d2c0170156516b0", "0000000000000000",
     "000a0009400000000063270f270f270f270f270f270f270f270f270f",
     "0002000040000004000108fc", "0001fff64000002503e8", "0001fff6000000260064",
     "0002ffff0000000804d2162e"},
    {"0000000000000000", "00010000000000000001", "000500040000000003e7270f270f270f270f",
     "00010000000000000001", "0000000000000000", "0000000000000002", "0000000000000025",
     "0000000000000026", "0000007b40000004"}};

inline ArrowErrorCode DecimalDescription(PqResultHelper* query, int batch) {
  PGresult* result = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  if (!result) return ENOMEM;
  query->SetResult(result);
  PGresAttDesc attrs[10] = {};
  const char* names[] = {"p1", "p18", "p19", "p20",     "p38",
                         "s2", "s37", "s38", "unknown", "ordinary"};
  const int32_t modifiers[] = {65540,  1179652, 1245188, 1310724, 2490372,
                               655366, 2490409, 2490410, -1,      -1};
  for (int i = 0; i < 10; i++) {
    attrs[i].name = const_cast<char*>(names[i]);
    attrs[i].typid = i == 9 ? kIntegerOid : kNumericOid;
    attrs[i].atttypmod = modifiers[i];
    attrs[i].format = 1;
  }
  if (!PQsetResultAttrs(result, 10, attrs)) return ENOMEM;
  const int rows = batch == 2 ? 0 : (batch == 3 ? 1 : (batch == 1 ? 2 : 4));
  for (int row = 0; row < rows; row++) {
    for (int col = 0; col < 10; col++) {
      const bool null = batch == 3 || row == 3;
      const auto value =
          NumericHex(null ? ""
                          : (col == 9 ? "000000000000002a"
                                      : kDecimalHex[batch == 1 ? 2 - row : row][col]));
      if (!PQsetvalue(result, row, col, null ? nullptr : const_cast<char*>(value.data()),
                      null ? -1 : static_cast<int>(value.size())))
        return ENOMEM;
    }
  }
  return NANOARROW_OK;
}

inline ArrowErrorCode DecimalFixture(int batch, ArrowArray* array, ArrowSchema* schema,
                                     ArrowError* error) {
  if (batch < 0 || batch > 3) return EINVAL;
  std::shared_ptr<PostgresTypeResolver> resolver;
  NANOARROW_RETURN_NOT_OK(
      ModifierResolver(adbc::driver::pgwire::BackendProfile::Redshift(), &resolver));
  PqResultHelper query(nullptr, "");
  NANOARROW_RETURN_NOT_OK(DecimalDescription(&query, batch));
  return FixtureFromQuery(query, resolver, array, schema, error);
}
}  // namespace adbcpq::testing
