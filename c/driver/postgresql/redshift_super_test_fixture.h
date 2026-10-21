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

#include <optional>
#include <string>
#include <vector>

#include "postgres_type.h"

namespace adbcpq::testing {
// Internal synthetic schema/catalog construction, never a native origin claim.
inline ArrowErrorCode SuperSchema(ArrowSchema* schema) {
  ArrowSchemaInit(schema);
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(schema, 1));
  auto* field = schema->children[0];
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetType(field, NANOARROW_TYPE_LARGE_STRING));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(field, "amount"));
  nanoarrow::UniqueBuffer metadata;
  NANOARROW_RETURN_NOT_OK(ArrowMetadataBuilderInit(metadata.get(), nullptr));
  for (const auto& item : std::vector<std::pair<const char*, std::string_view>>{
           {"ARROW:extension:name", RedshiftSuperSchema::kExtension},
           {"ARROW:extension:metadata", RedshiftSuperSchema::kMetadata},
           {"ADBC:redshift:sql_type", "SUPER"}})
    NANOARROW_RETURN_NOT_OK(ArrowMetadataBuilderAppend(
        metadata.get(), ArrowCharView(item.first),
        ArrowStringView{item.second.data(), static_cast<int64_t>(item.second.size())}));
  return ArrowSchemaSetMetadata(field, reinterpret_cast<char*>(metadata->data));
}
inline ArrowErrorCode SuperBatch(ArrowSchema* schema, ArrowArray* array,
                                 const std::vector<std::optional<std::string>>& rows) {
  ArrowError error{};
  NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromSchema(array, schema, &error));
  NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(array));
  for (const auto& row : rows) {
    if (row) {
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendString(
          array->children[0],
          ArrowStringView{row->data(), static_cast<int64_t>(row->size())}));
    } else {
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(array->children[0], 1));
    }
    NANOARROW_RETURN_NOT_OK(ArrowArrayFinishElement(array));
  }
  return ArrowArrayFinishBuildingDefault(array, &error);
}
}  // namespace adbcpq::testing
