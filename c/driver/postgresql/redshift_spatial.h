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
#include "redshift_vendor_schema.h"
namespace adbcpq {
struct RedshiftSpatial {
  static constexpr int64_t kMaxReadBytes = 1048447;
  static constexpr int64_t kMaxWriteBytes = 500000;
  class Parser {
   public:
    Parser(const uint8_t* data, size_t size, bool geography)
        : data_(data), size_(size), geography_(geography) {}
    bool Run() { return Geometry(0, 0, 0, 0, true) && pos_ == size_; }

   private:
    const uint8_t* data_;
    size_t size_, pos_ = 0;
    bool geography_;
    bool Integer(int bytes, bool little, uint64_t* out) {
      if (size_ - pos_ < static_cast<size_t>(bytes)) return false;
      *out = 0;
      for (int i = 0; i < bytes; ++i)
        *out |= static_cast<uint64_t>(data_[pos_++])
                << (8 * (little ? i : bytes - 1 - i));
      return true;
    }
    bool Coordinates(uint32_t n, int axes, bool little, bool point) {
      if (n > (size_ - pos_) / (8 * axes)) return false;
      for (uint32_t i = 0; i < n; ++i) {
        int nan = 0;
        for (int j = 0; j < axes; ++j) {
          uint64_t bits;
          if (!Integer(8, little, &bits)) return false;
          if ((bits & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000)) {
            if (!(bits & UINT64_C(0x000fffffffffffff))) return false;
            ++nan;
          }
        }
        if (nan && (!point || nan != axes)) return false;
      }
      return true;
    }
    bool Geometry(int depth, uint32_t expected, uint32_t inherited, uint32_t axes_flags,
                  bool root) {
      if (depth >= 128 || size_ - pos_ < 5) return false;
      uint8_t endian = data_[pos_++];
      if (endian > 1) return false;
      uint64_t raw;
      if (!Integer(4, endian, &raw)) return false;
      const uint32_t type = raw & UINT32_C(0x1fffffff),
                     flags = raw & UINT32_C(0xc0000000);
      if (type < 1 || type > 7 || (expected && type != expected) ||
          (!root && flags != axes_flags))
        return false;
      uint32_t srid = inherited;
      if (raw & UINT32_C(0x20000000)) {
        uint64_t value;
        if (!Integer(4, endian, &value)) return false;
        srid = static_cast<uint32_t>(value);
        if (!root && srid != inherited) return false;
      } else if (root && geography_)
        return false;
      const int axes = 2 + ((flags >> 31) & 1) + ((flags >> 30) & 1);
      if (type == 1) return Coordinates(1, axes, endian, true);
      uint64_t count;
      if (!Integer(4, endian, &count)) return false;
      if (type == 2)
        return Coordinates(static_cast<uint32_t>(count), axes, endian, false);
      if (type == 3) {
        if (count > (size_ - pos_) / 4) return false;
        for (uint64_t i = 0; i < count; ++i) {
          uint64_t points;
          if (!Integer(4, endian, &points) ||
              !Coordinates(static_cast<uint32_t>(points), axes, endian, false))
            return false;
        }
        return true;
      }
      if (count > (size_ - pos_) / 5) return false;
      for (uint64_t i = 0; i < count; ++i)
        if (!Geometry(depth + 1, type == 7 ? 0 : type - 3, srid, flags, false))
          return false;
      return true;
    }
  };
  static ArrowErrorCode Validate(ArrowBufferView bytes, bool geography, bool writing,
                                 ArrowError* error) {
    if (bytes.size_bytes <= 0 ||
        bytes.size_bytes > (writing ? kMaxWriteBytes : kMaxReadBytes) ||
        !bytes.data.data ||
        !Parser(bytes.data.as_uint8, static_cast<size_t>(bytes.size_bytes), geography)
             .Run()) {
      ArrowErrorSet(error, "[Redshift] Invalid or unsupported bounded EWKB payload");
      return EINVAL;
    }
    return NANOARROW_OK;
  }
};
}  // namespace adbcpq
