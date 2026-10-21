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

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "redshift_decimal.h"

namespace adbcpq {

enum class RedshiftBytesKind { kNone, kChar, kVarchar, kVarbyte };

// Canonical markers explicitly opt PostgreSQL into this contract. Unmarked
// PostgreSQL retains its own text/bytea semantics. Widths always count bytes.
struct RedshiftBytesSchema {
  RedshiftBytesKind kind = RedshiftBytesKind::kNone;
  std::optional<int32_t> width;
  bool enabled() const { return kind != RedshiftBytesKind::kNone; }
  static constexpr int32_t kInsertBinaryLimit = 1000000;
  static ArrowErrorCode Invalid(ArrowError* error, const char* reason) {
    ArrowErrorSet(error, "[Redshift] Invalid character/binary value: %s", reason);
    return EINVAL;
  }
  static RedshiftBytesKind Canonical(std::string_view name) {
    if (name == "CHAR") return RedshiftBytesKind::kChar;
    if (name == "VARCHAR") return RedshiftBytesKind::kVarchar;
    if (name == "VARBYTE") return RedshiftBytesKind::kVarbyte;
    return RedshiftBytesKind::kNone;
  }
  static const char* SqlName(RedshiftBytesKind kind) {
    if (kind == RedshiftBytesKind::kChar) return "CHAR";
    if (kind == RedshiftBytesKind::kVarchar) return "VARCHAR";
    if (kind == RedshiftBytesKind::kVarbyte) return "VARBYTE";
    return "";
  }
  static bool Text(ArrowType type) {
    return type == NANOARROW_TYPE_STRING || type == NANOARROW_TYPE_LARGE_STRING ||
           type == NANOARROW_TYPE_STRING_VIEW;
  }
  static bool Binary(ArrowType type) {
    return type == NANOARROW_TYPE_BINARY || type == NANOARROW_TYPE_LARGE_BINARY ||
           type == NANOARROW_TYPE_BINARY_VIEW || type == NANOARROW_TYPE_FIXED_SIZE_BINARY;
  }
  static int32_t ReadLimit(RedshiftBytesKind kind) {
    return kind == RedshiftBytesKind::kChar      ? 4096
           : kind == RedshiftBytesKind::kVarchar ? 16000000
                                                 : 16777216;
  }
  static int32_t StoredLimit(RedshiftBytesKind kind) {
    return kind == RedshiftBytesKind::kVarchar ? 65535 : ReadLimit(kind);
  }
  static ArrowErrorCode Parse(const ArrowSchema* schema, bool redshift,
                              RedshiftBytesSchema* out, ArrowError* error) {
    *out = {};
    ArrowSchemaView view{};
    NANOARROW_RETURN_NOT_OK(ArrowSchemaViewInit(&view, schema, error));
    std::optional<std::string_view> family, width, raw;
    NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
        schema, "ADBC:redshift:sql_type", &family, error));
    const auto marked = family ? Canonical(*family) : RedshiftBytesKind::kNone;
    // DECIMAL text and other explicit extensions retain their own contract.
    if (marked == RedshiftBytesKind::kNone && (family || view.extension_name.data))
      return NANOARROW_OK;
    if (!redshift && marked == RedshiftBytesKind::kNone) return NANOARROW_OK;
    out->kind = marked;
    if (!out->enabled()) {
      if (Text(view.type)) out->kind = RedshiftBytesKind::kVarchar;
      if (Binary(view.type)) out->kind = RedshiftBytesKind::kVarbyte;
    }
    if (!out->enabled()) return NANOARROW_OK;
    if (view.extension_name.data ||
        (out->kind == RedshiftBytesKind::kVarbyte ? !Binary(view.type)
                                                  : !Text(view.type)))
      return Invalid(error, "SQL identity contradicts Arrow storage");
    NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
        schema, "ADBC:redshift:byte_width", &width, error));
    if (width) {
      int32_t n;
      NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Integer(*width, &n, error));
      if (n < 1 || n > ReadLimit(out->kind)) return Invalid(error, "invalid byte width");
      out->width = n;
    }
    NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
        schema, "ADBC:redshift:type_modifier", &raw, error));
    if (raw) {
      int32_t n;
      NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Integer(*raw, &n, error));
      if (n != -1 && (n < 5 || !out->width || n - 4 != *out->width))
        return Invalid(error, "raw modifier disagrees with byte width");
    }
    return NANOARROW_OK;
  }
  ArrowErrorCode Declaration(std::string* out, ArrowError* error) const {
    if (kind == RedshiftBytesKind::kChar && !width)
      return Invalid(error, "CHAR CREATE requires explicit byte width");
    const int32_t n =
        width.value_or(kind == RedshiftBytesKind::kVarchar ? 65535 : 16777216);
    if (n < 1 || n > StoredLimit(kind))
      return Invalid(error, "invalid stored byte width");
    *out = std::string(SqlName(kind)) + "(" + std::to_string(n) + ")";
    return NANOARROW_OK;
  }
  // Validate indices before any unsafe Arrow view getter. In particular the
  // vendored nanoarrow full validator does not validate binary-view references.
  static ArrowErrorCode GetBytes(const ArrowArrayView* view, int64_t row,
                                 ArrowBufferView* out, ArrowError* error) {
    if (row < 0 || row >= view->length || view->offset < 0 ||
        row > INT64_MAX - view->offset)
      return Invalid(error, "invalid array index");
    const int64_t index = row + view->offset;
    if (view->storage_type == NANOARROW_TYPE_STRING_VIEW ||
        view->storage_type == NANOARROW_TYPE_BINARY_VIEW) {
      const auto& refs = view->buffer_views[1];
      if (!refs.data.data ||
          index >= refs.size_bytes / static_cast<int64_t>(sizeof(ArrowBinaryView)))
        return Invalid(error, "missing view record");
      const auto& value = refs.data.as_binary_view[index];
      if (value.inlined.size < 0) return Invalid(error, "negative view length");
      if (value.inlined.size > NANOARROW_BINARY_VIEW_INLINE_SIZE) {
        if (value.ref.buffer_index < 0 ||
            value.ref.buffer_index >= view->n_variadic_buffers || value.ref.offset < 0 ||
            !view->variadic_buffer_sizes || !view->variadic_buffers)
          return Invalid(error, "invalid view reference");
        const int64_t n = view->variadic_buffer_sizes[value.ref.buffer_index];
        if (!view->variadic_buffers[value.ref.buffer_index] || n < value.inlined.size ||
            value.ref.offset > n - value.inlined.size)
          return Invalid(error, "view reference exceeds data buffer");
      }
    } else if (view->storage_type == NANOARROW_TYPE_STRING ||
               view->storage_type == NANOARROW_TYPE_BINARY ||
               view->storage_type == NANOARROW_TYPE_LARGE_STRING ||
               view->storage_type == NANOARROW_TYPE_LARGE_BINARY) {
      const bool large = view->storage_type == NANOARROW_TYPE_LARGE_STRING ||
                         view->storage_type == NANOARROW_TYPE_LARGE_BINARY;
      const auto& offsets = view->buffer_views[1];
      const auto& data = view->buffer_views[2];
      const int64_t entries = offsets.size_bytes / (large ? 8 : 4);
      if (!offsets.data.data || index >= entries - 1)
        return Invalid(error, "missing byte offsets");
      const int64_t start =
          large ? offsets.data.as_int64[index] : offsets.data.as_int32[index];
      const int64_t end =
          large ? offsets.data.as_int64[index + 1] : offsets.data.as_int32[index + 1];
      if (start < 0 || end < start || end > data.size_bytes || (end && !data.data.data))
        return Invalid(error, "invalid byte offsets");
      // Avoid null-pointer arithmetic in the generic getter for empty arrays.
      if (end == 0) {
        *out = {};
        return NANOARROW_OK;
      }
    } else if (view->storage_type == NANOARROW_TYPE_FIXED_SIZE_BINARY) {
      const int64_t size = view->layout.element_size_bits[1] / 8;
      if (size < 0 || index == INT64_MAX ||
          (size && (index + 1 > view->buffer_views[1].size_bytes / size ||
                    !view->buffer_views[1].data.data)))
        return Invalid(error, "invalid fixed binary buffer");
      if (!size) {
        *out = {};
        return NANOARROW_OK;
      }
    } else
      return Invalid(error, "unsupported byte storage");
    *out = ArrowArrayViewGetBytesUnsafe(view, row);
    return NANOARROW_OK;
  }
  // Closed UTF-8, excluding NUL. Under the UTF-8 connection contract CHAR's
  // single-byte domain is 1..127: bytes >=128 require multiple-byte sequences.
  static ArrowErrorCode Validate(ArrowBufferView bytes, RedshiftBytesKind kind,
                                 std::optional<int32_t> width, bool writing,
                                 ArrowError* error) {
    if (bytes.size_bytes < 0 || bytes.size_bytes > ReadLimit(kind) ||
        (bytes.size_bytes && !bytes.data.data) || (width && bytes.size_bytes > *width))
      return Invalid(error, "byte length exceeds descriptor/format");
    if (kind == RedshiftBytesKind::kVarbyte) {
      if (writing && bytes.size_bytes > kInsertBinaryLimit)
        return Invalid(error, "plain INSERT binary envelope exceeded");
      return NANOARROW_OK;
    }
    return ValidateUtf8(bytes, kind == RedshiftBytesKind::kChar, error);
  }
  // Encoding only: each caller retains its own byte/domain bounds.
  static ArrowErrorCode ValidateUtf8(ArrowBufferView bytes, bool single_byte,
                                     ArrowError* error) {
    if (bytes.size_bytes < 0 || (bytes.size_bytes && !bytes.data.data))
      return Invalid(error, "invalid UTF-8 buffer");
    const uint8_t* p = bytes.data.as_uint8;
    int64_t i = 0;
    while (i < bytes.size_bytes) {
      const uint8_t first = p[i++];
      if (first == 0) return Invalid(error, "NUL text unsupported");
      if (first < 0x80) continue;
      if (single_byte) return Invalid(error, "multibyte CHAR");
      int continuation;
      uint32_t value, minimum;
      if (first >= 0xc2 && first <= 0xdf) {
        continuation = 1;
        value = first & 0x1f;
        minimum = 0x80;
      } else if (first >= 0xe0 && first <= 0xef) {
        continuation = 2;
        value = first & 0x0f;
        minimum = 0x800;
      } else if (first >= 0xf0 && first <= 0xf4) {
        continuation = 3;
        value = first & 0x07;
        minimum = 0x10000;
      } else
        return Invalid(error, "invalid UTF-8 lead");
      if (continuation > bytes.size_bytes - i) return Invalid(error, "short UTF-8");
      for (int j = 0; j < continuation; ++j) {
        const uint8_t next = p[i++];
        if ((next & 0xc0) != 0x80) return Invalid(error, "invalid UTF-8 continuation");
        value = (value << 6) | (next & 0x3f);
      }
      if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return Invalid(error, "invalid UTF-8 scalar");
    }
    return NANOARROW_OK;
  }
};

}  // namespace adbcpq
