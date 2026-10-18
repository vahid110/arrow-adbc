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
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "redshift_decimal.h"

namespace adbcpq {
// Fixed PostgreSQL-style reference only. Native libpq layout/domain/precision
// qualification is still required; no payload-length codec selection exists.
struct RedshiftTimetzSchema {
  bool enabled = false;
  std::optional<int32_t> precision;
  static constexpr int64_t kDayUs = 86400000000;
  static constexpr std::string_view kName = "adbc.redshift.timetz_us_offset.v1";
  static constexpr std::string_view kMetadata =
      R"({"version":1,"family":"TIMETZ","time_unit":"us","origin":"start_of_day","offset_unit":"s","offset_sign":"east_of_utc"})";
  static ArrowErrorCode Invalid(ArrowError* error, const char* reason) {
    ArrowErrorSet(error, "[Redshift] Invalid TIMETZ contract/value: %s", reason);
    return EINVAL;
  }
  // Destination buffers are driver-owned builders. Never realloc their old
  // pointer: an ArrowBuffer custom deallocator may consume it even on failure.
  // The fresh allocation uses default mutable ownership in production; only
  // internal test constructors supply a well-behaved failure-injection allocator.
  static ArrowErrorCode Reserve(
      ArrowBuffer* buffer, int64_t additional, ArrowError* error,
      ArrowBufferAllocator allocation = ArrowBufferAllocatorDefault()) {
    if (additional < 0 || buffer->size_bytes < 0 ||
        buffer->capacity_bytes < buffer->size_bytes ||
        (buffer->capacity_bytes > 0 && !buffer->data) ||
        buffer->size_bytes > INT64_MAX - additional)
      return Invalid(error, "buffer reserve size overflow/ownership shape");
    const int64_t required = buffer->size_bytes + additional;
    if (required <= buffer->capacity_bytes) return NANOARROW_OK;
    int64_t capacity =
        buffer->capacity_bytes > 0 && buffer->capacity_bytes <= INT64_MAX / 2
            ? buffer->capacity_bytes * 2
            : required;
    if (capacity < required) capacity = required;
    if (static_cast<uint64_t>(capacity) > static_cast<uint64_t>(SIZE_MAX))
      return Invalid(error, "allocation exceeds size_t");
    auto* replacement = allocation.reallocate(&allocation, nullptr, 0, capacity);
    if (!replacement) return ENOMEM;
    if (buffer->size_bytes) std::memcpy(replacement, buffer->data, buffer->size_bytes);
    auto old_allocation = buffer->allocator;
    old_allocation.free(&old_allocation, buffer->data, buffer->capacity_bytes);
    buffer->data = replacement;
    buffer->capacity_bytes = capacity;
    buffer->allocator = allocation;
    return NANOARROW_OK;
  }
  static bool Prefix(std::string_view name) {
    return name.starts_with("adbc.redshift.timetz");
  }
  static std::optional<int32_t> ReferencePrecision(std::optional<int32_t> raw) {
    // AWS JDBC773fa16 getScale800-807 reference, not native PQfmod proof.
    if (raw && *raw == -1) return 6;
    if (raw && *raw >= 0 && *raw <= 6) return raw;
    return std::nullopt;
  }
  static ArrowErrorCode Parse(const ArrowSchema* schema, bool redshift,
                              RedshiftTimetzSchema* out, ArrowError* error,
                              bool read = false) {
    *out = {};
    std::optional<std::string_view> sql, name, metadata, p, raw;
    auto get = [&](const char* key, std::optional<std::string_view>* value) {
      return RedshiftDecimalSchema::Metadata(schema, key, value, error);
    };
    NANOARROW_RETURN_NOT_OK(get("ADBC:redshift:sql_type", &sql));
    NANOARROW_RETURN_NOT_OK(get("ARROW:extension:name", &name));
    if (!(sql && *sql == "TIMETZ") && !(name && Prefix(*name))) return NANOARROW_OK;
    NANOARROW_RETURN_NOT_OK(get("ARROW:extension:metadata", &metadata));
    NANOARROW_RETURN_NOT_OK(get("ADBC:redshift:fractional_precision", &p));
    NANOARROW_RETURN_NOT_OK(get("ADBC:redshift:type_modifier", &raw));
    ArrowSchemaView view{};
    NANOARROW_RETURN_NOT_OK(ArrowSchemaViewInit(&view, schema, error));
    if (!sql || *sql != "TIMETZ" || !name || *name != kName || !metadata ||
        *metadata != kMetadata || view.type != NANOARROW_TYPE_STRUCT ||
        schema->n_children != 2 || schema->dictionary)
      return Invalid(error, "extension/family/version/struct conflict");
    const char* child_names[] = {"time_us", "offset_seconds"};
    for (int child = 0; child < 2; ++child) {
      const auto* value = schema->children[child];
      ArrowSchemaView child_view{};
      NANOARROW_RETURN_NOT_OK(ArrowSchemaViewInit(&child_view, value, error));
      if (!value->name || std::string_view(value->name) != child_names[child] ||
          child_view.type != (child == 0 ? NANOARROW_TYPE_INT64 : NANOARROW_TYPE_INT32) ||
          (value->flags & ARROW_FLAG_NULLABLE) || value->n_children || value->dictionary)
        return Invalid(error, "primitive child name/order/type/nullability conflict");
      NANOARROW_RETURN_NOT_OK(ValidateKeys(value, true, error));
    }
    NANOARROW_RETURN_NOT_OK(ValidateKeys(schema, false, error));
    if (p) {
      if (p->size() != 1 || (*p)[0] < '0' || (*p)[0] > '6')
        return Invalid(error, "precision must be one digit0..6");
      out->precision = (*p)[0] - '0';
    }
    if (raw) {
      int32_t code;
      NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Integer(*raw, &code, error));
      const auto expected = ReferencePrecision(code);
      if ((!read && !expected) || (out->precision && out->precision != expected))
        return Invalid(error, "raw modifier/precision unsupported or inconsistent");
    }
    out->enabled = true;
    if (!read) {
      if (!redshift) {
        ArrowErrorSet(error,
                      "Redshift TIMETZ reference writes on PostgreSQL are unsupported");
        return ENOTSUP;
      }
      if (!out->precision)
        return Invalid(error, "write requires explicit source precision");
    }
    return NANOARROW_OK;
  }
  static ArrowErrorCode ValidateKeys(const ArrowSchema* schema, bool child,
                                     ArrowError* error) {
    ArrowMetadataReader metadata;
    NANOARROW_RETURN_NOT_OK(ArrowMetadataReaderInit(&metadata, schema->metadata));
    if (metadata.remaining_keys < 0) return Invalid(error, "negative metadata count");
    ArrowStringView key{}, value{};
    while (metadata.remaining_keys > 0) {
      NANOARROW_RETURN_NOT_OK(ArrowMetadataReaderRead(&metadata, &key, &value));
      const std::string_view name(key.data, key.size_bytes);
      if (child && (name.starts_with("ADBC:redshift:") ||
                    name == "ARROW:extension:name" || name == "ARROW:extension:metadata"))
        return Invalid(error, "child semantic/extension metadata");
      if (!child && name.starts_with("ADBC:redshift:") &&
          name != "ADBC:redshift:sql_type" &&
          name != "ADBC:redshift:fractional_precision" &&
          name != "ADBC:redshift:type_modifier")
        return Invalid(error, "conflicting reserved metadata");
    }
    return NANOARROW_OK;
  }
  static ArrowErrorCode SetSchema(ArrowSchema* schema) {
    NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(schema, 2));
    for (int child = 0; child < 2; ++child) {
      NANOARROW_RETURN_NOT_OK(
          ArrowSchemaSetType(schema->children[child],
                             child == 0 ? NANOARROW_TYPE_INT64 : NANOARROW_TYPE_INT32));
      NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(
          schema->children[child], child == 0 ? "time_us" : "offset_seconds"));
      schema->children[child]->flags &= ~ARROW_FLAG_NULLABLE;
    }
    nanoarrow::UniqueBuffer metadata;
    NANOARROW_RETURN_NOT_OK(ArrowMetadataBuilderInit(metadata.get(), nullptr));
    NANOARROW_RETURN_NOT_OK(ArrowMetadataBuilderAppend(
        metadata.get(), ArrowCharView("ARROW:extension:name"),
        ArrowStringView{kName.data(), static_cast<int64_t>(kName.size())}));
    NANOARROW_RETURN_NOT_OK(ArrowMetadataBuilderAppend(
        metadata.get(), ArrowCharView("ARROW:extension:metadata"),
        ArrowStringView{kMetadata.data(), static_cast<int64_t>(kMetadata.size())}));
    return ArrowSchemaSetMetadata(schema, reinterpret_cast<const char*>(metadata->data));
  }
  static ArrowErrorCode CheckValue(int64_t civil, int64_t east, std::optional<int32_t> p,
                                   ArrowError* error) {
    if (civil < 0 || civil > kDayUs || east < -INT32_MAX || east > INT32_MAX)
      return Invalid(error, "reference clock/sign code outside reversible envelope");
    if (p) {
      if (*p < 0 || *p > 6) return Invalid(error, "unsupported precision");
      int64_t quantum = 1;
      for (int i = *p; i < 6; ++i) quantum *= 10;
      if (civil % quantum) return Invalid(error, "inexact civil precision");
    }
    return NANOARROW_OK;
  }
  ArrowErrorCode Declaration(std::string* out, ArrowError* error) const {
    if (!precision || *precision != 6)
      return Invalid(error, "CREATE precision syntax remains unqualified");
    *out = "TIMETZ";
    return NANOARROW_OK;
  }
  static ArrowErrorCode CheckedNull(const ArrowArrayView* view, int64_t index, bool* null,
                                    ArrowError* error) {
    if (index < 0 || index >= view->length || view->offset < 0 ||
        view->offset > INT64_MAX - index)
      return Invalid(error, "validity index outside view");
    const int64_t physical = view->offset + index;
    if (view->buffer_views[0].data.data &&
        view->buffer_views[0].size_bytes < physical / 8 + 1)
      return Invalid(error, "validity bitmap outside declared coverage");
    *null = ArrowArrayViewIsNull(view, index);
    return NANOARROW_OK;
  }
  ArrowErrorCode Value(const ArrowArrayView* view, int64_t index, int64_t* civil,
                       int32_t* east, ArrowError* error) const {
    if (index < 0 || index >= view->length || view->offset < 0 ||
        view->offset > INT64_MAX - index || view->n_children != 2)
      return Invalid(error, "parent index/offset outside view");
    const int64_t child_index = view->offset + index;
    bool parent_null;
    NANOARROW_RETURN_NOT_OK(CheckedNull(view, index, &parent_null, error));
    if (parent_null) return Invalid(error, "unexpected writer invocation on NULL parent");
    for (int child = 0; child < 2; ++child) {
      const auto* value = view->children[child];
      if (child_index >= value->length || value->offset < 0 ||
          value->offset > INT64_MAX - child_index || !value->buffer_views[1].data.data)
        return Invalid(error, "valid parent has missing/NULL child");
      bool child_null;
      NANOARROW_RETURN_NOT_OK(CheckedNull(value, child_index, &child_null, error));
      if (child_null) return Invalid(error, "valid parent has NULL child");
      const int64_t physical = value->offset + child_index;
      const int64_t width = child == 0 ? 8 : 4;
      if (physical > INT64_MAX / width - 1 ||
          value->buffer_views[1].size_bytes < (physical + 1) * width)
        return Invalid(error, "child data buffer outside declared coverage");
    }
    *civil = ArrowArrayViewGetIntUnsafe(view->children[0], child_index);
    *east =
        static_cast<int32_t>(ArrowArrayViewGetIntUnsafe(view->children[1], child_index));
    return CheckValue(*civil, *east, precision, error);
  }
};
}  // namespace adbcpq
