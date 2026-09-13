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
#include <vector>

#include <nanoarrow/nanoarrow.h>

namespace adbc::driver::pgwire {

enum class RedshiftCsvWriteStatus {
  kSucceeded,
  kInvalidInput,
  kColumnMismatch,
  kMalformedArrow,
  kUnsupportedType,
  kNullValue,
  kInvalidText,
  kAmbiguousNullMarker,
  kPayloadTooLarge,
};

// AWS-free, Redshift-private preparation for COPY ... MANIFEST CSV. The Arrow
// struct fields must have exactly the names and order of the COPY column list.
// This deliberately supports only non-null INT32, INT64, and UTF-8 string
// values. On any error, serialized_csv is left unchanged. Empty batches are
// rejected because the staged COPY coordinator requires a nonempty payload.
RedshiftCsvWriteStatus WriteRedshiftCsv(
    const ArrowSchema* schema, const ArrowArray* array,
    const std::vector<std::string_view>& ordered_columns, std::string* serialized_csv);

}  // namespace adbc::driver::pgwire
