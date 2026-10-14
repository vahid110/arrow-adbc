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

#include "redshift_replace_preflight.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>

namespace adbc::driver::pgwire {

RedshiftReplacePreflightResult CheckRedshiftReplaceIdentifiers(
    std::string_view table, std::string_view schema,
    const std::vector<std::string_view>& columns) {
  // Redshift truncates table and column names beyond 127 bytes. Reject instead
  // of approximating its UTF-8 clipping or allowing DROP to address an alias.
  constexpr std::size_t kMaxIdentifierBytes = 127;
  if (table.size() > kMaxIdentifierBytes) {
    return {Status::InvalidArgument(
        "Redshift replacement table name exceeds 127 logical bytes")};
  }
  if (schema.size() > kMaxIdentifierBytes) {
    return {Status::InvalidArgument(
        "Redshift replacement schema name exceeds 127 logical bytes")};
  }
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].size() > kMaxIdentifierBytes) {
      return {Status::InvalidArgument("Redshift replacement column ", i,
                                       " name exceeds 127 logical bytes")};
    }
  }

  if (table.empty()) {
    return {Status::InvalidArgument("Redshift replacement requires a table name")};
  }
  if (columns.empty()) {
    return {Status::InvalidArgument("Redshift replacement requires ingest columns")};
  }

  std::unordered_set<std::string_view> exact_names;
  std::unordered_set<std::string> folded_names;
  bool needs_case_setting = false;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    const std::string_view name = columns[i];
    if (name.empty()) {
      return {Status::InvalidArgument("Redshift replacement column ", i,
                                       " must have a nonempty name")};
    }
    if (!exact_names.insert(name).second) {
      return {Status::InvalidArgument("Redshift replacement column ", i,
                                       " duplicates another column name")};
    }

    // AWS specifies ASCII letter folding. Leave all non-ASCII bytes untouched;
    // locale-dependent or Unicode case conversion would invent other aliases.
    std::string folded(name);
    for (char& c : folded) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (!folded_names.insert(std::move(folded)).second) needs_case_setting = true;
  }

  return {Status::Ok(), needs_case_setting};
}

Status ValidateRedshiftReplaceCaseSetting(std::string_view setting) {
  if (setting == "on" || setting == "true") return Status::Ok();
  if (setting == "off" || setting == "false") {
    return Status::InvalidArgument(
        "Redshift replacement column names collide while "
        "enable_case_sensitive_identifier is disabled");
  }
  return Status(ADBC_STATUS_INVALID_DATA,
                "Cannot safely preflight Redshift replacement: unrecognized "
                "enable_case_sensitive_identifier setting");
}

}  // namespace adbc::driver::pgwire
