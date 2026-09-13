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

#include "redshift_staged_copy.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace adbc::driver::pgwire {
namespace {

constexpr std::string_view kS3Scheme = "s3://";
constexpr std::string_view kRoleArnPrefix = "arn:aws:iam::";
constexpr std::string_view kRoleResourcePrefix = ":role/";

bool IsAsciiLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

bool IsBucketChar(char c) {
  return (c >= 'a' && c <= 'z') || IsAsciiDigit(c) || c == '-';
}

bool IsObjectKeyChar(char c) {
  return IsAsciiLetter(c) || IsAsciiDigit(c) || c == '_' || c == '-' || c == '.';
}

bool IsRoleResourceChar(char c) {
  return IsAsciiLetter(c) || IsAsciiDigit(c) || c == '_' || c == '-' || c == '+' ||
         c == '=' || c == '.' || c == '@';
}

bool HasSafeSegments(std::string_view path, bool (*allowed)(char)) {
  if (path.empty()) return false;
  std::size_t segment_start = 0;
  while (segment_start < path.size()) {
    const std::size_t segment_end = path.find('/', segment_start);
    const std::size_t end =
        segment_end == std::string_view::npos ? path.size() : segment_end;
    const std::string_view segment = path.substr(segment_start, end - segment_start);
    if (segment.empty() || segment == "." || segment == "..") return false;
    for (char c : segment) {
      if (!allowed(c)) return false;
    }
    if (end == path.size()) return true;
    segment_start = end + 1;
  }
  return false;
}

struct S3Object {
  std::string_view bucket;
  std::string_view key;
};

std::optional<S3Object> ParseGeneratedS3Object(std::string_view url) {
  if (url.substr(0, kS3Scheme.size()) != kS3Scheme) return std::nullopt;
  const std::size_t bucket_start = kS3Scheme.size();
  const std::size_t slash = url.find('/', bucket_start);
  if (slash == std::string_view::npos) return std::nullopt;

  const std::string_view bucket = url.substr(bucket_start, slash - bucket_start);
  const std::string_view key = url.substr(slash + 1);
  // This is deliberately narrower than S3's full bucket/key grammar. The
  // future uploader will generate simple names under a controlled prefix.
  if (bucket.size() < 3 || bucket.size() > 63 || key.size() > 1024 ||
      !HasSafeSegments(key, IsObjectKeyChar)) {
    return std::nullopt;
  }
  if (!((bucket.front() >= 'a' && bucket.front() <= 'z') ||
        IsAsciiDigit(bucket.front())) ||
      !((bucket.back() >= 'a' && bucket.back() <= 'z') || IsAsciiDigit(bucket.back()))) {
    return std::nullopt;
  }
  for (char c : bucket) {
    if (!IsBucketChar(c)) return std::nullopt;
  }
  return S3Object{bucket, key};
}

bool IsValidRoleArn(std::string_view arn) {
  if (arn.substr(0, kRoleArnPrefix.size()) != kRoleArnPrefix) return false;
  const std::size_t account_start = kRoleArnPrefix.size();
  constexpr std::size_t kAccountIdLength = 12;
  if (arn.size() < account_start + kAccountIdLength + kRoleResourcePrefix.size() + 1) {
    return false;
  }
  for (char c : arn.substr(account_start, kAccountIdLength)) {
    if (!IsAsciiDigit(c)) return false;
  }
  if (arn.substr(account_start + kAccountIdLength, kRoleResourcePrefix.size()) !=
      kRoleResourcePrefix) {
    return false;
  }
  const std::string_view resource =
      arn.substr(account_start + kAccountIdLength + kRoleResourcePrefix.size());
  return resource.size() <= 512 && HasSafeSegments(resource, IsRoleResourceChar);
}

std::optional<std::string> QuoteIdentifier(std::string_view identifier) {
  // Redshift identifiers are at most 127 bytes. Limit this first internal
  // helper to printable ASCII; the established libpq path handles broader
  // database identifiers until staged ingestion is implemented.
  if (identifier.empty() || identifier.size() > 127) return std::nullopt;
  std::string quoted = "\"";
  for (unsigned char c : identifier) {
    if (c < 0x20 || c > 0x7e || c == '\\') return std::nullopt;
    if (c == '"') quoted += '"';
    quoted += static_cast<char>(c);
  }
  quoted += '"';
  return quoted;
}

}  // namespace

std::optional<RedshiftStagedCopyPlan> PrepareRedshiftStagedCopy(
    std::string_view schema, std::string_view table,
    const std::vector<std::string_view>& columns, std::string_view data_s3_url,
    std::string_view manifest_s3_url, std::string_view iam_role_arn) {
  const auto quoted_schema = QuoteIdentifier(schema);
  const auto quoted_table = QuoteIdentifier(table);
  const auto data = ParseGeneratedS3Object(data_s3_url);
  const auto manifest = ParseGeneratedS3Object(manifest_s3_url);
  if (!quoted_schema || !quoted_table || columns.empty() || !data || !manifest ||
      data->bucket != manifest->bucket || data->key == manifest->key ||
      !IsValidRoleArn(iam_role_arn)) {
    return std::nullopt;
  }

  std::string quoted_columns;
  std::unordered_set<std::string> seen_columns;
  for (const std::string_view column : columns) {
    const auto quoted_column = QuoteIdentifier(column);
    if (!quoted_column) return std::nullopt;

    // Case-only duplicates are ambiguous across server identifier-case
    // settings, so reject them regardless of the current configuration.
    std::string folded(column);
    for (char& c : folded) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (!seen_columns.insert(std::move(folded)).second) return std::nullopt;

    if (!quoted_columns.empty()) quoted_columns += ", ";
    quoted_columns += *quoted_column;
  }

  RedshiftStagedCopyPlan plan;
  // Validation above excludes JSON and SQL escape characters from all URLs.
  plan.manifest_json =
      "{\"entries\":[{\"url\":\"" + std::string(data_s3_url) + "\",\"mandatory\":true}]}";
  plan.copy_sql = "COPY " + *quoted_schema + "." + *quoted_table + " (" +
                  quoted_columns + ") FROM '" +
                  std::string(manifest_s3_url) + "' IAM_ROLE '" +
                  std::string(iam_role_arn) + "' MANIFEST CSV";
  return plan;
}

}  // namespace adbc::driver::pgwire
