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

#include <string>
#include <string_view>

#include "result_helper.h"

namespace adbcpq {

inline bool StrictSuperParseSetting(std::string_view value) {
  if (value.size() > 5) return false;
  std::string lower(value);
  for (auto& c : lower) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  }
  return lower == "off" || lower == "false";
}

// Read-only context checks; no cached GUC proof or hidden session mutation.
inline Status CheckSuperParseContext(PGconn* conn) {
  for (const char* query :
       {"SHOW parse_super_null_on_error", "SHOW json_parse_dedup_attributes",
        "SHOW json_parse_truncate_strings"}) {
    PqResultHelper helper(conn, query, true);
    UNWRAP_STATUS(helper.Execute());
    if (helper.NumColumns() != 1 || helper.NumRows() != 1)
      return Status::InvalidState("[Redshift] SUPER parse context shape is unqualified");
    const auto value = (*helper.begin())[0];
    if (value.is_null ||
        !StrictSuperParseSetting(std::string_view(value.data, value.len)))
      return Status::InvalidState("[Redshift] SUPER requires strict parse settings OFF");
  }
  return Status::Ok();
}

inline Status ValidateVendorTargets(const ArrowSchema* source, const PostgresType& target,
                                    bool redshift) {
  if (source->n_children != target.n_children())
    return Status::InvalidArgument(
        "[Redshift] Vendor source/target column count mismatch");
  for (int64_t i = 0; i < source->n_children; ++i) {
    RedshiftSuperSchema super;
    ArrowError error{};
    if (RedshiftSuperSchema::Parse(source->children[i], redshift, &super, &error))
      return Status::InvalidArgument(error.message);
    const auto family = target.child(i).vendor_family();
    if ((super.enabled || family != adbc::driver::pgwire::CatalogVendorFamily::kNone) &&
        (!super.enabled || family != adbc::driver::pgwire::CatalogVendorFamily::kSuper))
      return Status::InvalidArgument(
          "[Redshift] Vendor target needs exact SUPER JSON source");
  }
  return Status::Ok();
}

inline Status CheckVendorReadBoundary(const PostgresType& output) {
  for (int i = 0; i < output.n_children(); ++i) {
    if (output.child(i).vendor_family() !=
        adbc::driver::pgwire::CatalogVendorFamily::kNone)
      return Status::NotImplemented(
          "[Redshift] Native vendor result losslessness is not implemented; "
          "SUPER JSON input metadata is not a native read representation");
    UNWRAP_STATUS(CheckVendorReadBoundary(output.child(i)));
  }
  return Status::Ok();
}

}  // namespace adbcpq
