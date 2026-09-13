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

#include "redshift_csv_writer.h"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nanoarrow/nanoarrow.hpp>

#include "redshift_staged_copy_coordinator.h"

namespace adbc::driver::pgwire {
namespace {

constexpr std::size_t kRedshiftStagedCopyMaxRowBytes = 4'000'000;

bool AppendBounded(std::string* output, std::string_view value) {
  if (value.size() > kRedshiftStagedCopyMaxPayloadBytes - output->size()) {
    return false;
  }
  output->append(value);
  return true;
}

// Reject NUL, overlong encodings, surrogates, and values above U+10FFFF.
bool IsValidUtf8(std::string_view value) {
  for (std::size_t i = 0; i < value.size();) {
    const auto first = static_cast<uint8_t>(value[i]);
    if (first == 0) return false;
    if (first < 0x80) {
      ++i;
      continue;
    }

    std::size_t width;
    uint32_t codepoint;
    if (first >= 0xc2 && first <= 0xdf) {
      width = 2;
      codepoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      width = 3;
      codepoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      width = 4;
      codepoint = first & 0x07;
    } else {
      return false;
    }
    if (width > value.size() - i) return false;
    for (std::size_t j = 1; j < width; ++j) {
      const auto next = static_cast<uint8_t>(value[i + j]);
      if ((next & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if ((width == 3 && codepoint < 0x800) || (width == 4 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
      return false;
    }
    i += width;
  }
  return true;
}

RedshiftCsvWriteStatus AppendText(std::string* output, std::string_view value) {
  if (!IsValidUtf8(value)) return RedshiftCsvWriteStatus::kInvalidText;
  // COPY's default NULL AS marker is \\N. Until its behavior for a quoted
  // literal is verified live, reject it instead of risking silent null loads.
  if (value == "\\N") return RedshiftCsvWriteStatus::kAmbiguousNullMarker;
  if (!AppendBounded(output, "\"")) return RedshiftCsvWriteStatus::kPayloadTooLarge;
  for (char c : value) {
    if (!AppendBounded(output, c == '"' ? "\"\"" : std::string_view(&c, 1))) {
      return RedshiftCsvWriteStatus::kPayloadTooLarge;
    }
  }
  if (!AppendBounded(output, "\"")) return RedshiftCsvWriteStatus::kPayloadTooLarge;
  return RedshiftCsvWriteStatus::kSucceeded;
}

}  // namespace

RedshiftCsvWriteStatus WriteRedshiftCsv(
    const ArrowSchema* schema, const ArrowArray* array,
    const std::vector<std::string_view>& ordered_columns, std::string* serialized_csv) {
  if (!schema || !array || !serialized_csv || ordered_columns.empty() ||
      !schema->format || !array->buffers || array->length <= 0 ||
      schema->n_children != static_cast<int64_t>(ordered_columns.size()) ||
      array->n_children != schema->n_children || !schema->children || !array->children) {
    return RedshiftCsvWriteStatus::kInvalidInput;
  }
  for (std::size_t i = 0; i < ordered_columns.size(); ++i) {
    if (!schema->children[i] || !schema->children[i]->format || !array->children[i]) {
      return RedshiftCsvWriteStatus::kMalformedArrow;
    }
  }

  nanoarrow::UniqueArrayView view;
  ArrowError error;
  ArrowErrorInit(&error);
  if (ArrowArrayViewInitFromSchema(view.get(), schema, &error) != NANOARROW_OK) {
    return RedshiftCsvWriteStatus::kMalformedArrow;
  }
  if (view->storage_type != NANOARROW_TYPE_STRUCT || schema->dictionary ||
      view->n_children != static_cast<int64_t>(ordered_columns.size())) {
    return RedshiftCsvWriteStatus::kUnsupportedType;
  }
  for (std::size_t i = 0; i < ordered_columns.size(); ++i) {
    if (!schema->children[i] || !schema->children[i]->name ||
        ordered_columns[i].empty() || ordered_columns[i] != schema->children[i]->name) {
      return RedshiftCsvWriteStatus::kColumnMismatch;
    }
    const ArrowArrayView* child = view->children[i];
    if (schema->children[i]->dictionary ||
        (child->storage_type != NANOARROW_TYPE_INT32 &&
         child->storage_type != NANOARROW_TYPE_INT64 &&
         child->storage_type != NANOARROW_TYPE_STRING)) {
      return RedshiftCsvWriteStatus::kUnsupportedType;
    }
  }
  if (ArrowArrayViewSetArray(view.get(), array, &error) != NANOARROW_OK ||
      ArrowArrayViewValidate(view.get(), NANOARROW_VALIDATION_LEVEL_FULL, &error) !=
          NANOARROW_OK) {
    return RedshiftCsvWriteStatus::kMalformedArrow;
  }

  std::string result;
  for (int64_t row = 0; row < array->length; ++row) {
    const std::size_t row_start = result.size();
    if (ArrowArrayViewIsNull(view.get(), row)) {
      return RedshiftCsvWriteStatus::kNullValue;
    }
    for (std::size_t col = 0; col < ordered_columns.size(); ++col) {
      if (col != 0 && !AppendBounded(&result, ",")) {
        return RedshiftCsvWriteStatus::kPayloadTooLarge;
      }
      const ArrowArrayView* child = view->children[col];
      const int64_t child_row = view->offset + row;
      if (ArrowArrayViewIsNull(child, child_row)) {
        return RedshiftCsvWriteStatus::kNullValue;
      }
      if (child->storage_type == NANOARROW_TYPE_STRING) {
        const char* data = child->buffer_views[2].data.as_char;
        std::string_view value;
        if (data) {
          const ArrowStringView text = ArrowArrayViewGetStringUnsafe(child, child_row);
          if (text.size_bytes < 0) return RedshiftCsvWriteStatus::kMalformedArrow;
          value = std::string_view(text.data, static_cast<std::size_t>(text.size_bytes));
        } else {
          // nanoarrow's unsafe accessor performs pointer arithmetic on data;
          // avoid invoking it when an all-empty string buffer is absent.
          const int64_t index = child->offset + child_row;
          const int32_t start = child->buffer_views[1].data.as_int32[index];
          const int32_t end = child->buffer_views[1].data.as_int32[index + 1];
          if (start != 0 || end != 0) {
            return RedshiftCsvWriteStatus::kMalformedArrow;
          }
        }
        const auto size = value.size();
        if (size > kRedshiftStagedCopyMaxPayloadBytes - result.size()) {
          return RedshiftCsvWriteStatus::kPayloadTooLarge;
        }
        const auto status = AppendText(&result, value);
        if (status != RedshiftCsvWriteStatus::kSucceeded) return status;
      } else {
        char digits[32];
        const int64_t value = ArrowArrayViewGetIntUnsafe(child, child_row);
        const auto [end, code] = std::to_chars(digits, digits + sizeof(digits), value);
        if (code != std::errc() ||
            !AppendBounded(&result, std::string_view(digits, end - digits))) {
          return RedshiftCsvWriteStatus::kPayloadTooLarge;
        }
      }
    }
    if (!AppendBounded(&result, "\n")) {
      return RedshiftCsvWriteStatus::kPayloadTooLarge;
    }
    if (result.size() - row_start > kRedshiftStagedCopyMaxRowBytes) {
      return RedshiftCsvWriteStatus::kRowTooLarge;
    }
  }
  *serialized_csv = std::move(result);
  return RedshiftCsvWriteStatus::kSucceeded;
}

}  // namespace adbc::driver::pgwire
