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

#include <string_view>
#include "redshift_vendor_schema.h"
namespace adbcpq {
struct RedshiftHll {
  static constexpr int64_t kMaxBytes = 1000000;
  class Scanner {
   public:
    explicit Scanner(std::string_view s) : s_(s) {}
    bool Run() {
      if (!s_.empty() && s_[0] == '"') return Dense();
      return Object(false) && (Space(), p_ == s_.size());
    }

   private:
    std::string_view s_;
    size_t p_ = 0;
    size_t indices_ = 0, values_ = 0;
    void Space() {
      while (p_ < s_.size() &&
             (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\r' || s_[p_] == '\n'))
        ++p_;
    }
    bool Take(char c) {
      Space();
      if (p_ == s_.size() || s_[p_] != c) return false;
      ++p_;
      return true;
    }
    bool Key(std::string_view* key) {
      if (!Take('"')) return false;
      // Only known ASCII keys are representable: decode into fixed stack storage.
      char decoded[8];
      size_t n = 0;
      while (p_ < s_.size() && s_[p_] != '"') {
        unsigned char c = s_[p_++];
        if (c == '\\') {
          if (p_ == s_.size() || s_[p_++] != 'u' || s_.size() - p_ < 4) return false;
          uint32_t v = 0;
          for (int i = 0; i < 4; ++i) {
            char h = s_[p_++];
            int d = h >= '0' && h <= '9'   ? h - '0'
                    : h >= 'a' && h <= 'f' ? h - 'a' + 10
                    : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                           : -1;
            if (d < 0) return false;
            v = v * 16 + d;
          }
          if (v > 127) return false;
          c = static_cast<unsigned char>(v);
        }
        if (n == sizeof(decoded)) return false;
        decoded[n++] = static_cast<char>(c);
      }
      if (p_ == s_.size()) return false;
      ++p_;
      for (auto name : {std::string_view("version"), std::string_view("logm"),
                        std::string_view("sparse"), std::string_view("indices"),
                        std::string_view("values")})
        if (name == std::string_view(decoded, n)) {
          *key = name;
          return true;
        }
      return false;
    }
    bool Number(std::string_view* value) {
      Space();
      const size_t start = p_;
      if (p_ == s_.size() || s_[p_] < '0' || s_[p_] > '9') return false;
      if (s_[p_++] != '0')
        while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') ++p_;
      *value = s_.substr(start, p_ - start);
      return true;
    }
    bool Array(size_t* count) {
      if (!Take('[')) return false;
      *count = 0;
      if (Take(']')) return true;
      do {
        std::string_view n;
        if (!Number(&n)) return false;
        ++*count;
        if (Take(']')) return true;
      } while (Take(','));
      return false;
    }
    bool Object(bool sparse) {
      if (!Take('{')) return false;
      unsigned seen = 0;
      do {
        std::string_view key;
        if (!Key(&key) || !Take(':')) return false;
        unsigned bit = 0;
        bool ok = false;
        if (sparse) {
          if (key == "indices") {
            bit = 1;
            ok = Array(&indices_);
          } else if (key == "values") {
            bit = 2;
            ok = Array(&values_);
          }
        } else if (key == "sparse") {
          bit = 4;
          ok = Object(true);
        } else {
          std::string_view n;
          if (key == "version") {
            bit = 1;
            ok = Number(&n) && n == "1";
          } else if (key == "logm") {
            bit = 2;
            ok = Number(&n) && n == "15";
          }
        }
        if (!ok || (seen & bit)) return false;
        seen |= bit;
        if (Take('}'))
          return seen == (sparse ? 3u : 7u) && (!sparse || indices_ == values_);
      } while (Take(','));
      return false;
    }
    static int Base64(char c) {
      return c >= 'A' && c <= 'Z'   ? c - 'A'
             : c >= 'a' && c <= 'z' ? c - 'a' + 26
             : c >= '0' && c <= '9' ? c - '0' + 52
             : c == '+'             ? 62
             : c == '/'             ? 63
                                    : -1;
    }
    bool Dense() {
      if (s_.size() < 6 || s_.back() != '"') return false;
      const auto b = s_.substr(1, s_.size() - 2);
      if (b.size() % 4) return false;
      size_t padding = b.back() == '=' ? 1 : 0;
      if (padding && b[b.size() - 2] == '=') ++padding;
      for (size_t i = 0; i < b.size() - padding; ++i)
        if (Base64(b[i]) < 0) return false;
      for (size_t i = b.size() - padding; i < b.size(); ++i)
        if (b[i] != '=') return false;
      if (padding == 1 && (Base64(b[b.size() - 2]) & 3)) return false;
      if (padding == 2 && (Base64(b[b.size() - 3]) & 15)) return false;
      return true;
    }
  };
  static ArrowErrorCode Validate(ArrowBufferView bytes, ArrowError* error) {
    if (bytes.size_bytes <= 0 || bytes.size_bytes > kMaxBytes || !bytes.data.data) {
      ArrowErrorSet(error, "[Redshift] Invalid HLL serialization byte limit");
      return EINVAL;
    }
    NANOARROW_RETURN_NOT_OK(RedshiftBytesSchema::Validate(
        bytes, RedshiftBytesKind::kVarchar, std::nullopt, false, error));
    if (!Scanner(std::string_view(bytes.data.as_char, bytes.size_bytes)).Run()) {
      ArrowErrorSet(error, "[Redshift] Invalid HLL sparse/dense serialization subset");
      return EINVAL;
    }
    return NANOARROW_OK;
  }
};
}  // namespace adbcpq
