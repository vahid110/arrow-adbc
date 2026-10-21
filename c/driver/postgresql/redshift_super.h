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
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "redshift_bytes.h"

namespace adbcpq {

// Application-asserted JSON-subset input, never a native SUPER read assertion.
struct RedshiftSuperSchema {
  bool enabled = false;
  static constexpr std::string_view kExtension = "adbc.redshift.super.json";
  static constexpr std::string_view kMetadata =
      R"({"version":1,"encoding":"utf8-json","scope":"json-subset-write"})";
  static constexpr int64_t kMaxBytes = 1000000;
  static constexpr int kMaxDepth = 128;

  static ArrowErrorCode Invalid(ArrowError* error, const char* reason) {
    ArrowErrorSet(error, "[Redshift] Invalid SUPER JSON input: %s", reason);
    return EINVAL;
  }

  static ArrowErrorCode Parse(const ArrowSchema* schema, bool redshift,
                              RedshiftSuperSchema* out, ArrowError* error) {
    *out = {};
    ArrowSchemaView view{};
    NANOARROW_RETURN_NOT_OK(ArrowSchemaViewInit(&view, schema, error));
    std::optional<std::string_view> family, extension, metadata;
    NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
        schema, "ADBC:redshift:sql_type", &family, error));
    NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
        schema, "ARROW:extension:name", &extension, error));
    NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
        schema, "ARROW:extension:metadata", &metadata, error));
    const bool super_extension =
        extension && extension->starts_with("adbc.redshift.super");
    const bool vendor_family =
        family && (*family == "SUPER" || *family == "GEOMETRY" ||
                   *family == "GEOGRAPHY" || *family == "HLLSKETCH");
    const bool vendor_extension =
        extension &&
        (super_extension || extension->starts_with("adbc.redshift.geometry") ||
         extension->starts_with("adbc.redshift.geography") ||
         extension->starts_with("adbc.redshift.hllsketch"));
    if (redshift && metadata && !extension)
      return Invalid(error, "orphan extension metadata");
    if (!super_extension && !vendor_family && !vendor_extension) return NANOARROW_OK;
    if (!redshift) return Invalid(error, "Redshift-only semantic schema");
    if (!family || *family != "SUPER" || !extension || *extension != kExtension ||
        !metadata || *metadata != kMetadata)
      return Invalid(error, "unsupported or contradictory vendor schema/version");
    if (view.type != NANOARROW_TYPE_LARGE_STRING || schema->dictionary ||
        schema->n_children != 0 ||
        (schema->flags & ~static_cast<int64_t>(ARROW_FLAG_NULLABLE)))
      return Invalid(error, "requires plain large_utf8 without dictionary or children");
    ArrowMetadataReader reader;
    NANOARROW_RETURN_NOT_OK(ArrowMetadataReaderInit(&reader, schema->metadata));
    while (reader.remaining_keys > 0) {
      ArrowStringView key{}, value{};
      NANOARROW_RETURN_NOT_OK(ArrowMetadataReaderRead(&reader, &key, &value));
      const std::string_view name(key.data, key.size_bytes);
      if (name.starts_with("ADBC:redshift:") && name != "ADBC:redshift:sql_type")
        return Invalid(error, "conflicting semantic metadata");
    }
    out->enabled = true;
    return NANOARROW_OK;
  }

  static ArrowErrorCode ValidateRoute(const ArrowSchema* schema, bool redshift,
                                      bool generated_ingest, bool replace,
                                      bool* has_super, ArrowError* error) {
    *has_super = false;
    auto visit = [&](auto&& self, const ArrowSchema* node, bool direct,
                     bool dictionary) -> ArrowErrorCode {
      if (!node) return NANOARROW_OK;
      RedshiftSuperSchema field;
      NANOARROW_RETURN_NOT_OK(Parse(node, redshift, &field, error));
      if (field.enabled) {
        if (!direct || dictionary) return Invalid(error, "nested semantic field");
        if (!generated_ingest || replace)
          return Invalid(error,
                         "requires generated ingest; SUPER REPLACE is unsupported");
        *has_super = true;
      }
      for (int64_t i = 0; i < node->n_children; ++i)
        NANOARROW_RETURN_NOT_OK(
            self(self, node->children[i],
                 node == schema && std::string_view(node->format) == "+s", dictionary));
      return self(self, node->dictionary, false, true);
    };
    return visit(visit, schema, true, false);
  }

  // Grammar validation preserves original numeric tokens and UTF8 bytes. Numeric
  // storage interpretation remains the server's JSON_PARSE contract.
  class Scanner {
   public:
    explicit Scanner(std::string_view text) : text_(text) {}
    bool Run() { return Value(0) && (Space(), pos_ == text_.size()); }

   private:
    std::string_view text_;
    size_t pos_ = 0;
    void Space() {
      while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                     text_[pos_] == '\r' || text_[pos_] == '\n'))
        ++pos_;
    }
    bool Take(char c) {
      if (pos_ == text_.size() || text_[pos_] != c) return false;
      ++pos_;
      return true;
    }
    bool Literal(std::string_view value) {
      if (text_.substr(pos_, value.size()) != value) return false;
      pos_ += value.size();
      return true;
    }
    static bool Digit(char c) { return c >= '0' && c <= '9'; }
    bool Number() {
      Take('-');
      if (!Take('0')) {
        if (pos_ == text_.size() || text_[pos_] < '1' || text_[pos_] > '9') return false;
        while (pos_ < text_.size() && Digit(text_[pos_])) ++pos_;
      }
      if (Take('.')) {
        const size_t start = pos_;
        while (pos_ < text_.size() && Digit(text_[pos_])) ++pos_;
        if (start == pos_) return false;
      }
      if (Take('e') || Take('E')) {
        if (!Take('+')) Take('-');
        const size_t start = pos_;
        while (pos_ < text_.size() && Digit(text_[pos_])) ++pos_;
        if (start == pos_) return false;
      }
      return true;
    }
    bool Hex4(uint32_t* out) {
      *out = 0;
      for (int i = 0; i < 4; ++i) {
        if (pos_ == text_.size()) return false;
        const char c = text_[pos_++];
        int n = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                       : -1;
        if (n < 0) return false;
        *out = (*out << 4) | n;
      }
      return true;
    }
    static void Utf8(uint32_t c, std::string* out) {
      if (!out) return;
      if (c < 0x80)
        out->push_back(static_cast<char>(c));
      else if (c < 0x800) {
        out->push_back(static_cast<char>(0xc0 | (c >> 6)));
        out->push_back(static_cast<char>(0x80 | (c & 63)));
      } else if (c < 0x10000) {
        out->push_back(static_cast<char>(0xe0 | (c >> 12)));
        out->push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
        out->push_back(static_cast<char>(0x80 | (c & 63)));
      } else {
        out->push_back(static_cast<char>(0xf0 | (c >> 18)));
        out->push_back(static_cast<char>(0x80 | ((c >> 12) & 63)));
        out->push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
        out->push_back(static_cast<char>(0x80 | (c & 63)));
      }
    }
    bool String(std::string* decoded = nullptr) {
      if (!Take('"')) return false;
      while (pos_ < text_.size()) {
        const unsigned char c = text_[pos_++];
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c != '\\') {
          if (decoded) decoded->push_back(static_cast<char>(c));
          continue;
        }
        if (pos_ == text_.size()) return false;
        const char e = text_[pos_++];
        if (e == 'u') {
          uint32_t value;
          if (!Hex4(&value)) return false;
          if (value >= 0xd800 && value <= 0xdbff) {
            if (!Take('\\') || !Take('u')) return false;
            uint32_t low;
            if (!Hex4(&low) || low < 0xdc00 || low > 0xdfff) return false;
            value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
          } else if (value >= 0xdc00 && value <= 0xdfff)
            return false;
          Utf8(value, decoded);
        } else {
          const std::string_view escapes = "\"\\/bfnrt";
          const std::string_view values("\"\\/\b\f\n\r\t", 8);
          const auto i = escapes.find(e);
          if (i == std::string_view::npos) return false;
          if (decoded) decoded->push_back(values[i]);
        }
      }
      return false;
    }
    bool Value(int depth) {
      Space();
      if (pos_ == text_.size()) return false;
      if (text_[pos_] == '{' || text_[pos_] == '[') {
        if (depth >= kMaxDepth) return false;
        const bool object = Take('{');
        if (!object) ++pos_;
        const char end = object ? '}' : ']';
        Space();
        if (Take(end)) return true;
        std::unordered_set<std::string> keys;
        do {
          Space();
          if (object) {
            std::string key;
            if (!String(&key) || !keys.insert(std::move(key)).second) return false;
            Space();
            if (!Take(':')) return false;
          }
          if (!Value(depth + 1)) return false;
          Space();
          if (Take(end)) return true;
        } while (Take(','));
        return false;
      }
      if (text_[pos_] == '"') return String();
      if (text_[pos_] == 't') return Literal("true");
      if (text_[pos_] == 'f') return Literal("false");
      if (text_[pos_] == 'n') return Literal("null");
      return Number();
    }
  };

  static ArrowErrorCode Validate(ArrowBufferView bytes, ArrowError* error) {
    if (bytes.size_bytes <= 0 || bytes.size_bytes > kMaxBytes || !bytes.data.data)
      return Invalid(error, "document byte limit");
    NANOARROW_RETURN_NOT_OK(RedshiftBytesSchema::Validate(
        bytes, RedshiftBytesKind::kVarchar, std::nullopt, false, error));
    if (!Scanner(std::string_view(bytes.data.as_char, bytes.size_bytes)).Run())
      return Invalid(error, "JSON grammar, duplicate key or depth");
    return NANOARROW_OK;
  }
};

}  // namespace adbcpq
