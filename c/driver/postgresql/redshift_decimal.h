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

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nanoarrow/nanoarrow.hpp>

#include "driver/pgwire/backend.h"

namespace adbcpq {

// The explicit DECIMAL marker opts into this closed semantic contract on either
// backend. Unmarked PostgreSQL schemas keep their existing mapping and writers.
struct RedshiftDecimalSchema {
  bool enabled = false;
  bool text = false;
  std::optional<int32_t> precision;
  std::optional<int32_t> scale;

  static ArrowErrorCode Invalid(ArrowError* error, const char* reason) {
    ArrowErrorSet(error, "[Redshift] Invalid decimal write: %s", reason);
    return EINVAL;
  }

  static ArrowErrorCode Metadata(const ArrowSchema* schema, const char* name,
                                 std::optional<std::string_view>* out,
                                 ArrowError* error) {
    ArrowMetadataReader reader;
    NANOARROW_RETURN_NOT_OK(ArrowMetadataReaderInit(&reader, schema->metadata));
    ArrowStringView key{}, value{};
    *out = std::nullopt;
    if (reader.remaining_keys < 0) return Invalid(error, "malformed metadata count");
    while (reader.remaining_keys > 0) {
      NANOARROW_RETURN_NOT_OK(ArrowMetadataReaderRead(&reader, &key, &value));
      if (std::string_view(key.data, key.size_bytes) == name) {
        if (*out) return Invalid(error, "duplicate decimal metadata");
        *out = std::string_view(value.data, value.size_bytes);
      }
    }
    return NANOARROW_OK;
  }

  static ArrowErrorCode Integer(std::string_view value, int32_t* out, ArrowError* error) {
    if (value.empty()) return Invalid(error, "empty decimal metadata integer");
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), *out);
    if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size()) {
      return Invalid(error, "malformed decimal metadata integer");
    }
    return NANOARROW_OK;
  }

  static ArrowErrorCode Parse(const ArrowSchema* schema, bool redshift,
                              RedshiftDecimalSchema* out, ArrowError* error) {
    *out = {};
    ArrowSchemaView view{};
    NANOARROW_RETURN_NOT_OK(ArrowSchemaViewInit(&view, schema, error));
    std::optional<std::string_view> family, p, s, raw;
    NANOARROW_RETURN_NOT_OK(Metadata(schema, "ADBC:redshift:sql_type", &family, error));
    NANOARROW_RETURN_NOT_OK(Metadata(schema, "ADBC:redshift:precision", &p, error));
    NANOARROW_RETURN_NOT_OK(Metadata(schema, "ADBC:redshift:scale", &s, error));
    const bool decimal =
        view.type == NANOARROW_TYPE_DECIMAL128 || view.type == NANOARROW_TYPE_DECIMAL256;
    if (decimal && family && *family != "DECIMAL") {
      return Invalid(error, "decimal storage contradicts SQL family");
    }
    out->enabled = (redshift && decimal) || (family && *family == "DECIMAL");
    if (!out->enabled && !p && !s) return NANOARROW_OK;
    if (!out->enabled || (family && *family != "DECIMAL")) {
      return Invalid(error, "contradictory decimal identity");
    }
    out->text = view.type == NANOARROW_TYPE_STRING ||
                view.type == NANOARROW_TYPE_LARGE_STRING ||
                view.type == NANOARROW_TYPE_STRING_VIEW;
    if (!decimal && !out->text) return Invalid(error, "unsupported decimal storage");
    if (view.extension_name.data) return Invalid(error, "unsupported decimal extension");
    if (p.has_value() != s.has_value())
      return Invalid(error, "incomplete precision/scale");
    if (decimal) {
      out->precision = view.decimal_precision;
      out->scale = view.decimal_scale;
    }
    if (p) {
      int32_t precision, scale;
      NANOARROW_RETURN_NOT_OK(Integer(*p, &precision, error));
      NANOARROW_RETURN_NOT_OK(Integer(*s, &scale, error));
      if (decimal && (precision != out->precision || scale != out->scale)) {
        return Invalid(error, "metadata disagrees with Arrow decimal");
      }
      out->precision = precision;
      out->scale = scale;
    }
    if (out->precision && (*out->precision < 1 || *out->precision > 38 ||
                           *out->scale < 0 || *out->scale > *out->precision)) {
      return Invalid(error, "unsupported source precision/scale");
    }
    NANOARROW_RETURN_NOT_OK(Metadata(schema, "ADBC:redshift:type_modifier", &raw, error));
    if (raw) {
      int32_t modifier;
      NANOARROW_RETURN_NOT_OK(Integer(*raw, &modifier, error));
      if (modifier != -1) {
        const auto info =
            adbc::driver::pgwire::BackendProfile::Redshift().DescribeTypeModifier(
                "numeric", modifier);
        if (!info.precision || info.precision != out->precision ||
            info.scale != out->scale) {
          return Invalid(error, "modifier disagrees with precision/scale");
        }
      }
    }
    return NANOARROW_OK;
  }

  ArrowErrorCode Declaration(std::string* out, ArrowError* error) const {
    if (!precision || !scale || *precision < 1 || *precision > 38 || *scale < 0 ||
        *scale > *precision || *scale > 37) {
      return Invalid(error, "CREATE requires explicit legal stored precision/scale");
    }
    *out = "DECIMAL(" + std::to_string(*precision) + "," + std::to_string(*scale) + ")";
    return NANOARROW_OK;
  }
};

// Portable bounded coefficient arithmetic. No Arrow fixed-word conversion or
// server coercion is used as a precision/range/rescaling check.
struct RedshiftDecimalValue {
  std::string digits = "0";
  bool negative = false;
  int32_t scale = 0;

  void Normalize() {
    const auto first = digits.find_first_not_of('0');
    if (first == std::string::npos) {
      digits = "0";
      negative = false;
    } else {
      digits.erase(0, first);
    }
  }

  static ArrowErrorCode ParseText(std::string_view text, RedshiftDecimalValue* out,
                                  ArrowError* error) {
    *out = {};
    if (text.empty() || text.size() > 80) {
      return RedshiftDecimalSchema::Invalid(error, "decimal text size outside contract");
    }
    size_t i = 0;
    out->negative = text[0] == '-';
    if (text[0] == '-' || text[0] == '+') ++i;
    out->digits.clear();
    size_t integral = 0;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i, ++integral) {
      out->digits.push_back(text[i]);
    }
    if (!integral) return RedshiftDecimalSchema::Invalid(error, "decimal text grammar");
    if (i < text.size() && text[i] == '.') {
      const size_t dot = i++;
      for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
        out->digits.push_back(text[i]);
        ++out->scale;
      }
      if (i == dot + 1)
        return RedshiftDecimalSchema::Invalid(error, "decimal text grammar");
    }
    if (i != text.size() || out->scale > 38) {
      return RedshiftDecimalSchema::Invalid(error, "decimal text grammar/scale");
    }
    out->Normalize();
    return NANOARROW_OK;
  }

  ArrowErrorCode Rescale(int32_t target, ArrowError* error) {
    if (target < 0 || target > 38)
      return RedshiftDecimalSchema::Invalid(error, "target scale");
    const int32_t shift = target - scale;
    if (digits != "0" && shift < 0) {
      const size_t remove = static_cast<size_t>(-shift);
      if (remove >= digits.size() ||
          digits.find_last_not_of('0') >= digits.size() - remove) {
        return RedshiftDecimalSchema::Invalid(error, "nonzero fractional loss");
      }
      digits.resize(digits.size() - remove);
    } else if (digits != "0" && shift > 0) {
      if (digits.size() + shift > 38) {
        return RedshiftDecimalSchema::Invalid(error, "rescaled coefficient overflow");
      }
      digits.append(shift, '0');
    }
    scale = target;
    return NANOARROW_OK;
  }

  ArrowErrorCode CheckPrecision(int32_t precision, bool stored, ArrowError* error) const {
    if (precision < 1 || precision > 38 || digits.empty() ||
        digits.size() > static_cast<size_t>(precision)) {
      return RedshiftDecimalSchema::Invalid(error, "coefficient exceeds precision");
    }
    if (stored && precision == 19) {
      const std::string_view limit =
          negative ? "9223372036854775808" : "9223372036854775807";
      if (digits.size() == limit.size() && digits.compare(limit) > 0) {
        return RedshiftDecimalSchema::Invalid(error, "stored precision19 signed64 limit");
      }
    }
    return NANOARROW_OK;
  }
};
}  // namespace adbcpq
