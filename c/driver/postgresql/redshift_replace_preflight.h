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

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "driver/framework/status.h"

namespace adbc::driver::pgwire {

struct RedshiftReplacePreflightResult {
  Status status;
  bool needs_case_setting = false;
};

// Validate logical identifier bytes, not escaped SQL text. An empty schema
// means that the caller has not supplied one. This performs no SQL or session
// changes and does not replace the established identifier quoting path.
// A case-only collision requires the current session's identifier-case setting.
RedshiftReplacePreflightResult CheckRedshiftReplaceIdentifiers(
    std::string_view table, std::string_view schema,
    const std::vector<std::string_view>& columns);

// Call only when CheckRedshiftReplaceIdentifiers reports needs_case_setting.
// Identifiers are always quoted by the caller: on/true preserves distinct case,
// while off/false folds ASCII letters and makes the pending names collide.
Status ValidateRedshiftReplaceCaseSetting(std::string_view setting);

// Reads the current session's identifier-case setting. Never changes or caches it.
inline constexpr std::string_view kRedshiftCaseSettingQuery =
    "SHOW enable_case_sensitive_identifier";

// Executes a single-value query on the caller's connection.
using RedshiftSettingReader =
    std::function<Status(std::string_view query, std::string* value)>;

// Full preflight run before replace ingest issues DROP. read_setting is called
// only when a case-only column collision is possible.
Status PreflightRedshiftReplace(std::string_view table, std::string_view schema,
                                const std::vector<std::string_view>& columns,
                                const RedshiftSettingReader& read_setting);

}  // namespace adbc::driver::pgwire
