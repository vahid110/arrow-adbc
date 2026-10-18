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

#include <algorithm>
#include <charconv>
#include <cinttypes>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nanoarrow/nanoarrow.hpp>

#include "../connection.h"
#include "../postgres_util.h"
#include "copy_common.h"

namespace adbcpq {

// The maximum value in seconds that can be converted into microseconds
// without overflow
constexpr int64_t kMaxSafeSecondsToMicros = 9223372036854L;

// The minimum value in seconds that can be converted into microseconds
// without overflow
constexpr int64_t kMinSafeSecondsToMicros = -9223372036854L;

// The maximum value in milliseconds that can be converted into microseconds
// without overflow
constexpr int64_t kMaxSafeMillisToMicros = 9223372036854775L;

// The minimum value in milliseconds that can be converted into microseconds
// without overflow
constexpr int64_t kMinSafeMillisToMicros = -9223372036854775L;

// 2000-01-01 00:00:00.000000 in microseconds
constexpr int64_t kPostgresTimestampEpoch = 946684800000000L;

// Write a value to a buffer without checking the buffer size. Advances
// the cursor of buffer and reduces it by sizeof(T)
template <typename T>
inline void WriteUnsafe(ArrowBuffer* buffer, T in) {
  const T value = SwapNetworkToHost(in);
  ArrowBufferAppendUnsafe(buffer, &value, sizeof(T));
}

template <>
inline void WriteUnsafe(ArrowBuffer* buffer, int8_t in) {
  ArrowBufferAppendUnsafe(buffer, &in, sizeof(int8_t));
}

template <>
inline void WriteUnsafe(ArrowBuffer* buffer, int16_t in) {
  WriteUnsafe<uint16_t>(buffer, in);
}

template <>
inline void WriteUnsafe(ArrowBuffer* buffer, int32_t in) {
  WriteUnsafe<uint32_t>(buffer, in);
}

template <>
inline void WriteUnsafe(ArrowBuffer* buffer, int64_t in) {
  WriteUnsafe<uint64_t>(buffer, in);
}

template <typename T>
ArrowErrorCode WriteChecked(ArrowBuffer* buffer, T in, ArrowError* error) {
  NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buffer, sizeof(T)));
  WriteUnsafe<T>(buffer, in);
  return NANOARROW_OK;
}

class PostgresCopyFieldWriter {
 public:
  virtual ~PostgresCopyFieldWriter() {}

  template <class T, typename... Params>
  static std::unique_ptr<T> Create(struct ArrowArrayView* array_view, Params&&... args) {
    auto writer = std::make_unique<T>(std::forward<Params>(args)...);
    writer->Init(array_view);
    return writer;
  }

  virtual ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) {
    return ENOTSUP;
  }

 protected:
  virtual void Init(struct ArrowArrayView* array_view) { array_view_ = array_view; };

  struct ArrowArrayView* array_view_;
  std::vector<std::unique_ptr<PostgresCopyFieldWriter>> children_;
};

class PostgresCopyNullFieldWriter : public PostgresCopyFieldWriter {
 public:
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    (void)buffer;
    ArrowErrorSet(error,
                  "[libpq] Unexpected non-null value for Arrow null type at row %" PRId64,
                  index);
    return EINVAL;
  }
};

class PostgresCopyFieldTupleWriter : public PostgresCopyFieldWriter {
 public:
  void AppendChild(std::unique_ptr<PostgresCopyFieldWriter> child) {
    children_.push_back(std::move(child));
  }

  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    if (index >= array_view_->length) {
      return ENODATA;
    }

    const int16_t n_fields = children_.size();
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, n_fields, error));

    for (int16_t i = 0; i < n_fields; i++) {
      const int8_t is_null = ArrowArrayViewIsNull(array_view_->children[i], index);
      if (is_null) {
        constexpr int32_t field_size_bytes = -1;
        NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));
      } else {
        NANOARROW_RETURN_NOT_OK(children_[i]->Write(buffer, index, error));
      }
    }

    return NANOARROW_OK;
  }

 private:
  std::vector<std::unique_ptr<PostgresCopyFieldWriter>> children_;
};

class PostgresCopyBooleanFieldWriter : public PostgresCopyFieldWriter {
 public:
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    constexpr int32_t field_size_bytes = 1;
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));
    const int8_t value =
        static_cast<int8_t>(ArrowArrayViewGetIntUnsafe(array_view_, index));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int8_t>(buffer, value, error));

    return ADBC_STATUS_OK;
  }
};

template <typename T, T kOffset = 0>
class PostgresCopyNetworkEndianFieldWriter : public PostgresCopyFieldWriter {
 public:
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    constexpr int32_t field_size_bytes = sizeof(T);
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));
    const T value =
        static_cast<T>(ArrowArrayViewGetIntUnsafe(array_view_, index)) - kOffset;
    NANOARROW_RETURN_NOT_OK(WriteChecked<T>(buffer, value, error));

    return ADBC_STATUS_OK;
  }
};

class PostgresCopyIntegerNumericFieldWriter : public PostgresCopyFieldWriter {
 public:
  explicit PostgresCopyIntegerNumericFieldWriter(bool is_signed)
      : is_signed_(is_signed) {}

  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    uint64_t value;
    int16_t sign = kNumericPos;
    if (is_signed_) {
      const int64_t raw = ArrowArrayViewGetIntUnsafe(array_view_, index);
      if (raw < 0) {
        sign = kNumericNeg;
        value = static_cast<uint64_t>(-(raw + 1)) + 1;
      } else {
        value = static_cast<uint64_t>(raw);
      }
    } else {
      value = ArrowArrayViewGetUIntUnsafe(array_view_, index);
    }

    if (value == 0) {
      return WriteZero(buffer, error);
    }

    constexpr int kMaxUInt64PgDigits = 5;
    int16_t reversed_digits[kMaxUInt64PgDigits];
    int n_digits = 0;
    while (value != 0) {
      reversed_digits[n_digits++] = static_cast<int16_t>(value % 10000);
      value /= 10000;
    }

    int first_digit = 0;
    while (first_digit < n_digits && reversed_digits[first_digit] == 0) {
      first_digit++;
    }

    const int16_t ndigits = static_cast<int16_t>(n_digits - first_digit);
    const int16_t weight = static_cast<int16_t>(n_digits - 1);
    constexpr int16_t dscale = 0;
    const int32_t field_size_bytes = sizeof(ndigits) + sizeof(weight) + sizeof(sign) +
                                     sizeof(dscale) + ndigits * sizeof(int16_t);

    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, ndigits, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, weight, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, sign, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, dscale, error));

    const size_t pg_digit_bytes = sizeof(int16_t) * ndigits;
    NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buffer, pg_digit_bytes));
    for (int i = n_digits - 1; i >= first_digit; i--) {
      WriteUnsafe<int16_t>(buffer, reversed_digits[i]);
    }

    return ADBC_STATUS_OK;
  }

 private:
  static ArrowErrorCode WriteZero(ArrowBuffer* buffer, ArrowError* error) {
    constexpr int16_t ndigits = 0;
    constexpr int16_t weight = 0;
    constexpr int16_t sign = kNumericPos;
    constexpr int16_t dscale = 0;
    constexpr int32_t field_size_bytes =
        sizeof(ndigits) + sizeof(weight) + sizeof(sign) + sizeof(dscale);

    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, ndigits, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, weight, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, sign, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, dscale, error));
    return ADBC_STATUS_OK;
  }

  static constexpr int16_t kNumericPos = 0x0000;
  static constexpr int16_t kNumericNeg = 0x4000;
  const bool is_signed_;
};

class PostgresCopyFloatFieldWriter : public PostgresCopyFieldWriter {
 public:
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    constexpr int32_t field_size_bytes = sizeof(uint32_t);
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));

    uint32_t value;
    float raw_value = ArrowArrayViewGetDoubleUnsafe(array_view_, index);
    std::memcpy(&value, &raw_value, sizeof(uint32_t));
    NANOARROW_RETURN_NOT_OK(WriteChecked<uint32_t>(buffer, value, error));

    return ADBC_STATUS_OK;
  }
};

class PostgresCopyDoubleFieldWriter : public PostgresCopyFieldWriter {
 public:
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    constexpr int32_t field_size_bytes = sizeof(uint64_t);
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));

    uint64_t value;
    double raw_value = ArrowArrayViewGetDoubleUnsafe(array_view_, index);
    std::memcpy(&value, &raw_value, sizeof(uint64_t));
    NANOARROW_RETURN_NOT_OK(WriteChecked<uint64_t>(buffer, value, error));

    return ADBC_STATUS_OK;
  }
};

// Validated scalar contract: calculate before reserving/emitting bytes, and
// preserve source semantics only within the same integer/float/boolean family.
class RedshiftScalarFieldWriter : public PostgresCopyFieldWriter {
 public:
  RedshiftScalarFieldWriter(RedshiftScalarKind source, RedshiftScalarKind target)
      : source_(source), target_(target) {}
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    if (RedshiftScalarSchema::Integral(target_)) {
      int64_t value;
      NANOARROW_RETURN_NOT_OK(
          RedshiftScalarInteger(array_view_, index, source_, &value, error));
      NANOARROW_RETURN_NOT_OK(
          RedshiftScalarInteger(array_view_, index, target_, &value, error));
      const int32_t width = target_ == RedshiftScalarKind::kSmallint  ? 2
                            : target_ == RedshiftScalarKind::kInteger ? 4
                                                                      : 8;
      NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buffer, width + 4));
      WriteUnsafe<int32_t>(buffer, width);
      if (width == 2)
        WriteUnsafe<int16_t>(buffer, static_cast<int16_t>(value));
      else if (width == 4)
        WriteUnsafe<int32_t>(buffer, static_cast<int32_t>(value));
      else
        WriteUnsafe<int64_t>(buffer, value);
    } else if (target_ == RedshiftScalarKind::kBoolean) {
      const int64_t value = ArrowArrayViewGetIntUnsafe(array_view_, index);
      if (value != 0 && value != 1)
        return RedshiftScalarSchema::Invalid(error, "invalid boolean value");
      NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buffer, 5));
      WriteUnsafe<int32_t>(buffer, 1);
      WriteUnsafe<int8_t>(buffer, static_cast<int8_t>(value));
    } else if (target_ == RedshiftScalarKind::kReal) {
      float value;
      NANOARROW_RETURN_NOT_OK(RedshiftScalarFloat32(
          ArrowArrayViewGetDoubleUnsafe(array_view_, index), &value, error));
      uint32_t bits;
      std::memcpy(&bits, &value, sizeof(bits));
      NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buffer, 8));
      WriteUnsafe<int32_t>(buffer, 4);
      WriteUnsafe<uint32_t>(buffer, bits);
    } else if (target_ == RedshiftScalarKind::kDouble) {
      const double value = ArrowArrayViewGetDoubleUnsafe(array_view_, index);
      if (source_ == RedshiftScalarKind::kReal) {
        float source;
        NANOARROW_RETURN_NOT_OK(RedshiftScalarFloat32(value, &source, error));
      }
      uint64_t bits;
      std::memcpy(&bits, &value, sizeof(bits));
      NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buffer, 12));
      WriteUnsafe<int32_t>(buffer, 8);
      WriteUnsafe<uint64_t>(buffer, bits);
    } else {
      return RedshiftScalarSchema::Invalid(error, "unsupported scalar target");
    }
    return NANOARROW_OK;
  }

 private:
  RedshiftScalarKind source_;
  RedshiftScalarKind target_;
};

class PostgresCopyIntervalFieldWriter : public PostgresCopyFieldWriter {
 public:
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    constexpr int32_t field_size_bytes = 16;
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));

    struct ArrowInterval interval;
    ArrowIntervalInit(&interval, NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO);
    ArrowArrayViewGetIntervalUnsafe(array_view_, index, &interval);
    const int64_t ms = interval.ns / 1000;
    NANOARROW_RETURN_NOT_OK(WriteChecked<int64_t>(buffer, ms, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, interval.days, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, interval.months, error));

    return ADBC_STATUS_OK;
  }
};

// Exact decimal contract used by Redshift and explicitly DECIMAL-marked fields.
// The wire representation remains PostgreSQL base-10000 numeric reference data.
class RedshiftDecimalFieldWriter : public PostgresCopyFieldWriter {
 public:
  RedshiftDecimalFieldWriter(RedshiftDecimalSchema source,
                             std::optional<RedshiftDecimalSchema> target,
                             ArrowType storage)
      : source_(std::move(source)), target_(std::move(target)), storage_(storage) {}

  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    RedshiftDecimalValue value;
    if (source_.text) {
      const auto text = ArrowArrayViewGetStringUnsafe(array_view_, index);
      NANOARROW_RETURN_NOT_OK(RedshiftDecimalValue::ParseText(
          std::string_view(text.data, text.size_bytes), &value, error));
      if (source_.scale) {
        NANOARROW_RETURN_NOT_OK(value.Rescale(*source_.scale, error));
      } else {
        // Only remove mathematically redundant zeroes when necessary to fit the
        // closed38-digit value contract; never infer a schema from these rows.
        while (value.digits.size() > 38 && value.scale > 0 &&
               value.digits.back() == '0') {
          value.digits.pop_back();
          --value.scale;
        }
      }
    } else {
      ArrowDecimal decimal;
      ArrowDecimalInit(&decimal, storage_ == NANOARROW_TYPE_DECIMAL128 ? 128 : 256,
                       *source_.precision, *source_.scale);
      ArrowArrayViewGetDecimalUnsafe(array_view_, index, &decimal);
      nanoarrow::UniqueBuffer digits;
      NANOARROW_RETURN_NOT_OK(ArrowDecimalAppendDigitsToBuffer(&decimal, digits.get()));
      value.digits.assign(reinterpret_cast<char*>(digits->data), digits->size_bytes);
      value.negative = !value.digits.empty() && value.digits[0] == '-';
      if (value.negative) value.digits.erase(0, 1);
      value.scale = *source_.scale;
      value.Normalize();
    }
    NANOARROW_RETURN_NOT_OK(
        value.CheckPrecision(source_.precision.value_or(38), false, error));
    if (target_) {
      NANOARROW_RETURN_NOT_OK(value.Rescale(*target_->scale, error));
      NANOARROW_RETURN_NOT_OK(value.CheckPrecision(*target_->precision, true, error));
    }

    // Work is bounded by38 coefficient/scale digits. Build groups before touching
    // the output buffer; a rejected value cannot emit a partial parameter.
    std::string full = value.digits;
    if (full.size() <= static_cast<size_t>(value.scale)) {
      full.insert(0, static_cast<size_t>(value.scale) + 1 - full.size(), '0');
    }
    const size_t integral = full.size() - value.scale;
    const size_t left = (4 - integral % 4) % 4;
    full.insert(0, left, '0');
    full.append((4 - value.scale % 4) % 4, '0');
    std::vector<uint16_t> groups;
    for (size_t i = 0; i < full.size(); i += 4) {
      uint16_t digit = 0;
      for (size_t j = 0; j < 4; j++) digit = digit * 10 + full[i + j] - '0';
      groups.push_back(digit);
    }
    int16_t weight = static_cast<int16_t>((integral + left) / 4 - 1);
    size_t first = 0;
    size_t last = groups.size();
    while (first < last && groups[first] == 0) {
      ++first;
      --weight;
    }
    while (last > first && groups[last - 1] == 0) --last;
    if (first == last) weight = 0;
    const int16_t count = static_cast<int16_t>(last - first);
    const int32_t field_size = 8 + count * 2;
    NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buffer, field_size + 4));
    WriteUnsafe<int32_t>(buffer, field_size);
    WriteUnsafe<int16_t>(buffer, count);
    WriteUnsafe<int16_t>(buffer, weight);
    WriteUnsafe<uint16_t>(buffer, value.negative ? 0x4000 : 0);
    WriteUnsafe<uint16_t>(buffer, static_cast<uint16_t>(value.scale));
    for (size_t i = first; i < last; i++) WriteUnsafe<uint16_t>(buffer, groups[i]);
    return NANOARROW_OK;
  }

 private:
  RedshiftDecimalSchema source_;
  std::optional<RedshiftDecimalSchema> target_;
  ArrowType storage_;
};

// Inspiration for this taken from get_str_from_var in the pg source
// src/backend/utils/adt/numeric.c
template <enum ArrowType T>
class PostgresCopyNumericFieldWriter : public PostgresCopyFieldWriter {
 public:
  PostgresCopyNumericFieldWriter(int32_t precision, int32_t scale,
                                 bool disable_fast_path = false)
      : precision_{precision}, scale_{scale}, disable_fast_path_{disable_fast_path} {}

  // PostgreSQL NUMERIC Binary Format:
  // ===================================
  // PostgreSQL stores NUMERIC values in a variable-length binary format:
  //   - ndigits (int16): Number of base-10000 digits stored
  //   - weight (int16): Position of the first digit group relative to decimal point
  //                     (weight can be negative for small fractional numbers)
  //   - sign (int16): kNumericPos (0x0000) or kNumericNeg (0x4000)
  //   - dscale (int16): Number of decimal digits after the decimal point (display scale)
  //   - digits[]: Array of int16 values, each 0-9999 (base-10000 representation)
  //
  // Value calculation: sum(digits[i] * 10000^(weight - i)) * 10^(-dscale)
  //
  // Example 1: 12300 (from Arrow Decimal value=123, scale=-2)
  //   - Logical representation: "12300"
  //   - Grouped in base-10000: [1][2300]
  //   - ndigits=2, weight=1, sign=0x0000, dscale=0, digits=[1, 2300]
  //   - Calculation: 1*10000^1 + 2300*10000^0 = 10000 + 2300 = 12300
  //
  // Example 2: 123.45 (from Arrow Decimal value=12345, scale=2)
  //   - Logical representation: "123.45"
  //   - Integer part "123", fractional part "45"
  //   - Grouped in base-10000: [123][4500] (fractional part right-padded)
  //   - ndigits=2, weight=0, sign=0x0000, dscale=2, digits=[123, 4500]
  //   - Calculation: 123*10000^0 + 4500*10000^(-1) = 123 + 0.45 = 123.45
  //
  // Example 3: 0.00123 (from Arrow Decimal value=123, scale=5)
  //   - Logical representation: "0.00123"
  //   - Integer part "0", fractional part "00123"
  //   - Grouped in base-10000: [123] (leading zeros skipped via negative weight)
  //   - ndigits=1, weight=-1, sign=0x0000, dscale=5, digits=[123]
  //   - Calculation: 123*10000^(-1) * 10^0 = 0.0123, but dscale=5 means display as
  //   0.00123

  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    struct ArrowDecimal decimal;
    ArrowDecimalInit(&decimal, bitwidth_, precision_, scale_);
    ArrowArrayViewGetDecimalUnsafe(array_view_, index, &decimal);

#if defined(__SIZEOF_INT128__)
    if constexpr (T == NANOARROW_TYPE_DECIMAL128) {
      if (!disable_fast_path_) {
        const ArrowErrorCode fast_res = WriteDecimal128Fast(buffer, &decimal, error);
        if (fast_res != ENOTSUP) {
          return fast_res;
        }
      }
    }
#endif

    const int16_t sign = ArrowDecimalSign(&decimal) > 0 ? kNumericPos : kNumericNeg;

    // Convert decimal to string and split into integer/fractional parts
    // Example transformation for Arrow Decimal(value=12345, scale=2) representing 123.45:
    //   Input: decimal.value = 12345, scale_ = 2
    //   After DecimalToString: raw_decimal_string = "12345", original_digits = 5
    //   After SplitDecimalParts: parts.integer_part = "123"
    //                           parts.fractional_part = "45"
    //                           parts.effective_scale = 2
    char raw_decimal_string[max_decimal_digits_ + 1];
    int original_digits = DecimalToString<bitwidth_>(&decimal, raw_decimal_string);
    DecimalParts parts = SplitDecimalParts(raw_decimal_string, original_digits, scale_);

    // Group into PostgreSQL base-10000 representation
    //   After GroupIntegerDigits: int_digits = [123], weight = 0
    //     (groups "123" right-to-left: "123" → 123, only 1 group so weight = 0)
    auto [int_digits, weight] = GroupIntegerDigits(parts.integer_part);

    //   After GroupFractionalDigits: frac_digits = [4500], final_weight = 0
    //     (groups "45" left-to-right with right-padding: "45" → "4500" → 4500)
    auto [frac_digits, final_weight] =
        GroupFractionalDigits(parts.fractional_part, weight, !parts.integer_part.empty());

    // Combine digit arrays
    //   After combining: all_digits = [123, 4500]
    std::vector<int16_t> all_digits = int_digits;
    all_digits.insert(all_digits.end(), frac_digits.begin(), frac_digits.end());

    // Calculate display scale by counting trailing zeros in the DECIMAL STRING
    //   For our example: frac_part="45" has 0 trailing zeros, effective_scale=2
    //   So dscale = 2 - 0 = 2 (2 fractional digits to display)
    int trailing_zeros = 0;
    for (int j = parts.fractional_part.length() - 1;
         j >= 0 && parts.fractional_part[j] == '0'; j--) {
      trailing_zeros++;
    }
    int16_t dscale =
        static_cast<int16_t>((std::max)(0, parts.effective_scale - trailing_zeros));

    // Optimize: remove trailing zero digit groups from fractional part
    int n_int_digit_groups = int_digits.size();
    while (static_cast<int>(all_digits.size()) > n_int_digit_groups &&
           all_digits.back() == 0) {
      all_digits.pop_back();
    }

    // Handle zero special case
    if (all_digits.empty()) {
      final_weight = 0;
      dscale = 0;
    } else if (static_cast<int>(all_digits.size()) <= n_int_digit_groups) {
      // All fractional digits were removed
      dscale = 0;
    }

    if (dscale < 0) dscale = 0;

    // Write PostgreSQL NUMERIC binary format to buffer
    //   Final values for our example: ndigits = 2
    //                                final_weight = 0
    //                                sign = 0x0000
    //                                dscale = 2
    //                                digits = [123, 4500]
    //   Binary output represents: 123 * 10000^0 + 4500 * 10000^(-1) = 123 + 0.45 = 123.45
    int16_t ndigits = all_digits.size();
    int32_t field_size_bytes = sizeof(ndigits) + sizeof(final_weight) + sizeof(sign) +
                               sizeof(dscale) + ndigits * sizeof(int16_t);

    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, ndigits, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, final_weight, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, sign, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, dscale, error));

    const size_t pg_digit_bytes = sizeof(int16_t) * all_digits.size();
    NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buffer, pg_digit_bytes));
    for (auto pg_digit : all_digits) {
      WriteUnsafe<int16_t>(buffer, pg_digit);
    }

    return ADBC_STATUS_OK;
  }

 private:
#if defined(__SIZEOF_INT128__)
  __extension__ using UInt128 = unsigned __int128;

  ArrowErrorCode WriteDecimal128Fast(ArrowBuffer* buffer, struct ArrowDecimal* decimal,
                                     ArrowError* error) const {
    constexpr int kDecDigits = 4;
    constexpr int kMaxPgDigits = 48;

    UInt128 value =
        (static_cast<UInt128>(decimal->words[decimal->high_word_index]) << 64) |
        static_cast<UInt128>(decimal->words[decimal->low_word_index]);

    const int16_t sign = ArrowDecimalSign(decimal) > 0 ? kNumericPos : kNumericNeg;
    if (sign == kNumericNeg) {
      value = ~value + 1;
    }

    if (value == 0) {
      constexpr int16_t ndigits = 0;
      constexpr int16_t weight = 0;
      constexpr int16_t dscale = 0;
      constexpr int32_t field_size_bytes =
          sizeof(ndigits) + sizeof(weight) + sizeof(sign) + sizeof(dscale);

      NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));
      NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, ndigits, error));
      NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, weight, error));
      NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, sign, error));
      NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, dscale, error));
      return ADBC_STATUS_OK;
    }

    int effective_scale = scale_;
    if (effective_scale > 0) {
      while (effective_scale > 0 && value % 10 == 0) {
        value /= 10;
        effective_scale--;
      }
    } else if (effective_scale < 0) {
      if (!MultiplyPow10(&value, -effective_scale)) {
        return ENOTSUP;
      }
      effective_scale = 0;
    }

    if (effective_scale > (std::numeric_limits<int16_t>::max)()) {
      return ENOTSUP;
    }

    const int frac_groups = (effective_scale + kDecDigits - 1) / kDecDigits;
    const int alignment_zeros = frac_groups * kDecDigits - effective_scale;
    if (!MultiplyPow10(&value, alignment_zeros)) {
      return ENOTSUP;
    }

    int16_t reversed_digits[kMaxPgDigits];
    int n_digits = 0;
    while (value != 0) {
      if (n_digits >= kMaxPgDigits) {
        return ENOTSUP;
      }
      reversed_digits[n_digits++] = static_cast<int16_t>(value % 10000);
      value /= 10000;
    }

    const int weight_int = n_digits - frac_groups - 1;
    if (weight_int < (std::numeric_limits<int16_t>::min)() ||
        weight_int > (std::numeric_limits<int16_t>::max)()) {
      return ENOTSUP;
    }

    int first_digit = 0;
    while (first_digit < n_digits && reversed_digits[first_digit] == 0) {
      first_digit++;
    }

    const int canonical_digits = n_digits - first_digit;
    if (canonical_digits == 0) {
      constexpr int16_t ndigits = 0;
      constexpr int16_t weight = 0;
      constexpr int16_t dscale = 0;
      constexpr int32_t field_size_bytes =
          sizeof(ndigits) + sizeof(weight) + sizeof(sign) + sizeof(dscale);

      NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));
      NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, ndigits, error));
      NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, weight, error));
      NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, sign, error));
      NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, dscale, error));
      return ADBC_STATUS_OK;
    }

    const int16_t ndigits = static_cast<int16_t>(canonical_digits);
    const int16_t weight = static_cast<int16_t>(weight_int);
    const int16_t dscale = static_cast<int16_t>(effective_scale);
    const int32_t field_size_bytes = sizeof(ndigits) + sizeof(weight) + sizeof(sign) +
                                     sizeof(dscale) + ndigits * sizeof(int16_t);

    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, ndigits, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, weight, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, sign, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int16_t>(buffer, dscale, error));

    const size_t pg_digit_bytes = sizeof(int16_t) * canonical_digits;
    NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buffer, pg_digit_bytes));
    for (int i = n_digits - 1; i >= first_digit; i--) {
      WriteUnsafe<int16_t>(buffer, reversed_digits[i]);
    }

    return ADBC_STATUS_OK;
  }

  static bool MultiplyPow10(UInt128* value, int zeros) {
    constexpr UInt128 max_value = ~static_cast<UInt128>(0);
    for (int i = 0; i < zeros; i++) {
      if (*value > max_value / 10) {
        return false;
      }
      *value *= 10;
    }
    return true;
  }
#endif

  // Helper struct for organizing data flow between functions
  struct DecimalParts {
    std::string integer_part;     // e.g., "12300" or "123"
    std::string fractional_part;  // e.g., "45" or "00123"
    int effective_scale;          // Scale after handling negative values
  };

  // Helper function implementations for decimal-to-PostgreSQL NUMERIC conversion

  // Convert decimal to string (absolute value, no sign)
  // Returns the length of the string
  template <int32_t DEC_WIDTH>
  int DecimalToString(struct ArrowDecimal* decimal, char* out) const {
    constexpr size_t nwords = (DEC_WIDTH == 128) ? 2 : 4;
    uint8_t tmp[DEC_WIDTH / 8];
    ArrowDecimalGetBytes(decimal, tmp);
    uint64_t buf[DEC_WIDTH / 64];
    std::memcpy(buf, tmp, sizeof(buf));
    const int16_t sign = ArrowDecimalSign(decimal) > 0 ? kNumericPos : kNumericNeg;
    const bool is_negative = sign == kNumericNeg ? true : false;
    if (is_negative) {
      buf[0] = ~buf[0] + 1;
      for (size_t i = 1; i < nwords; i++) {
        buf[i] = ~buf[i];
      }
    }

    // Basic approach adopted from https://stackoverflow.com/a/8023862/621736
    char s[max_decimal_digits_ + 1];
    std::memset(s, '0', sizeof(s) - 1);
    s[sizeof(s) - 1] = '\0';

    for (size_t i = 0; i < DEC_WIDTH; i++) {
      int carry;

      carry = (buf[nwords - 1] > 0x7FFFFFFFFFFFFFFF);
      for (size_t j = nwords - 1; j > 0; j--) {
        buf[j] = ((buf[j] << 1) & 0xFFFFFFFFFFFFFFFF) + (buf[j - 1] > 0x7FFFFFFFFFFFFFFF);
      }
      buf[0] = ((buf[0] << 1) & 0xFFFFFFFFFFFFFFFF);

      for (int j = sizeof(s) - 2; j >= 0; j--) {
        s[j] += s[j] - '0' + carry;
        carry = (s[j] > '9');
        if (carry) {
          s[j] -= 10;
        }
      }
    }

    char* p = s;
    while ((p[0] == '0') && (p < &s[sizeof(s) - 2])) {
      p++;
    }

    const size_t ndigits = sizeof(s) - 1 - (p - s);
    std::memcpy(out, p, ndigits);
    out[ndigits] = '\0';

    return ndigits;
  }

  DecimalParts SplitDecimalParts(const char* decimal_digits, int digit_count,
                                 int scale) const {
    // Virtual zeros represent the logical zeros appended for negative scale
    // Example: value=123, scale=-2 → "123" with 2 virtual zeros = "12300"
    const int virtual_zeros = (scale < 0) ? -scale : 0;
    const int effective_scale = (scale < 0) ? 0 : scale;
    const int total_logical_digits = digit_count + virtual_zeros;

    // Calculate split point
    const int n_int_digits = total_logical_digits > effective_scale
                                 ? total_logical_digits - effective_scale
                                 : 0;
    const int n_frac_digits = total_logical_digits - n_int_digits;

    DecimalParts parts;
    parts.effective_scale = effective_scale;

    // Extract integer part
    if (n_int_digits > 0) {
      if (n_int_digits <= digit_count) {
        // Integer part is within the original digits
        parts.integer_part.assign(decimal_digits, n_int_digits);
      } else {
        // Integer part includes all original digits + virtual zeros
        parts.integer_part.assign(decimal_digits, digit_count);
        parts.integer_part.append(virtual_zeros, '0');
      }
    }

    // Extract fractional part (only exists if scale > 0)
    if (n_int_digits == 0 && total_logical_digits < effective_scale) {
      // Small fractional: 0.00123 needs leading zeros
      parts.fractional_part.assign(effective_scale - total_logical_digits, '0');
      parts.fractional_part.append(decimal_digits, digit_count);
    } else if (n_frac_digits > 0 && n_int_digits < digit_count) {
      // Fractional part from remaining digits (virtual zeros don't appear in fractional
      // part)
      parts.fractional_part.assign(decimal_digits + n_int_digits,
                                   digit_count - n_int_digits);
    }

    return parts;
  }

  std::pair<std::vector<int16_t>, int16_t> GroupIntegerDigits(
      const std::string& int_part) const {
    constexpr int kDecDigits = 4;
    std::vector<int16_t> digits;

    if (int_part.empty()) {
      return {digits, -1};  // weight = -1 for pure fractional numbers
    }

    // Calculate weight: ceil(length / 4) - 1
    int16_t weight = (int_part.length() + kDecDigits - 1) / kDecDigits - 1;

    // Group right-to-left in chunks of 4
    int i = int_part.length();
    while (i > 0) {
      int chunk_size = (std::min)(i, kDecDigits);
      std::string_view chunk =
          std::string_view(int_part).substr(i - chunk_size, chunk_size);

      int16_t val{};
      std::from_chars(chunk.data(), chunk.data() + chunk.size(), val);

      digits.insert(digits.begin(), val);
      i -= chunk_size;
    }

    return {digits, weight};
  }

  std::pair<std::vector<int16_t>, int16_t> GroupFractionalDigits(
      const std::string& frac_part, int16_t initial_weight, bool has_integer_part) const {
    constexpr int kDecDigits = 4;
    std::vector<int16_t> digits;
    int16_t weight = initial_weight;

    if (frac_part.empty()) {
      return {digits, weight};
    }

    bool skip_leading_zeros = !has_integer_part;

    // Group left-to-right in chunks of 4, right-padding last chunk
    for (size_t i = 0; i < frac_part.length(); i += kDecDigits) {
      int chunk_size = (std::min)(kDecDigits, static_cast<int>(frac_part.length() - i));
      std::string chunk_str = frac_part.substr(i, chunk_size);

      // Right-pad to 4 digits (e.g., "45" → "4500")
      chunk_str.resize(kDecDigits, '0');

      int16_t val{};
      std::from_chars(chunk_str.data(), chunk_str.data() + chunk_str.size(), val);

      if (skip_leading_zeros && val == 0) {
        // Skip leading zero groups in fractional part (e.g., 0.0012 → skip "0012")
        weight--;
      } else {
        digits.push_back(val);
        skip_leading_zeros = false;
      }
    }

    return {digits, weight};
  }

  static constexpr uint16_t kNumericPos = 0x0000;
  static constexpr uint16_t kNumericNeg = 0x4000;
  static constexpr int32_t bitwidth_ = (T == NANOARROW_TYPE_DECIMAL128) ? 128 : 256;
  static constexpr size_t max_decimal_digits_ =
      (T == NANOARROW_TYPE_DECIMAL128) ? 39 : 78;
  const int32_t precision_;
  const int32_t scale_;
  const bool disable_fast_path_;
};

template <enum ArrowTimeUnit TU>
class PostgresCopyDurationFieldWriter : public PostgresCopyFieldWriter {
 public:
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    constexpr int32_t field_size_bytes = 16;
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));

    int64_t raw_value = ArrowArrayViewGetIntUnsafe(array_view_, index);
    int64_t value = 0;

    bool overflow_safe = true;
    switch (TU) {
      case NANOARROW_TIME_UNIT_SECOND:
        overflow_safe =
            raw_value <= kMaxSafeSecondsToMicros && raw_value >= kMinSafeSecondsToMicros;
        if (overflow_safe) {
          value = raw_value * 1000000;
        }
        break;
      case NANOARROW_TIME_UNIT_MILLI:
        overflow_safe =
            raw_value <= kMaxSafeMillisToMicros && raw_value >= kMinSafeMillisToMicros;
        if (overflow_safe) {
          value = raw_value * 1000;
        }
        break;
      case NANOARROW_TIME_UNIT_MICRO:
        value = raw_value;
        break;
      case NANOARROW_TIME_UNIT_NANO:
        value = raw_value / 1000;
        break;
    }

    if (!overflow_safe) {
      ArrowErrorSet(
          error, "Row %" PRId64 " duration value %" PRId64 " with unit %d would overflow",
          index, raw_value, TU);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }

    // 2000-01-01 00:00:00.000000 in microseconds
    constexpr uint32_t days = 0;
    constexpr uint32_t months = 0;
    NANOARROW_RETURN_NOT_OK(WriteChecked<int64_t>(buffer, value, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, days, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, months, error));

    return ADBC_STATUS_OK;
  }
};

class RedshiftTemporalFieldWriter : public PostgresCopyFieldWriter {
 public:
  RedshiftTemporalFieldWriter(RedshiftTemporalSchema source,
                              std::optional<int32_t> target_precision)
      : source_(source), target_precision_(target_precision) {}
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    int64_t native;
    NANOARROW_RETURN_NOT_OK(
        source_.ToNative(ArrowArrayViewGetIntUnsafe(array_view_, index), &native, error));
    NANOARROW_RETURN_NOT_OK(
        RedshiftTemporalSchema::CheckPrecision(native, target_precision_, error));
    const int32_t width = source_.kind == RedshiftTemporalKind::kDate ? 4 : 8;
    NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buffer, width + 4));
    WriteUnsafe<int32_t>(buffer, width);
    if (width == 4)
      WriteUnsafe<int32_t>(buffer, static_cast<int32_t>(native));
    else
      WriteUnsafe<int64_t>(buffer, native);
    return NANOARROW_OK;
  }

 private:
  RedshiftTemporalSchema source_;
  std::optional<int32_t> target_precision_;
};

class RedshiftBytesFieldWriter : public PostgresCopyFieldWriter {
 public:
  RedshiftBytesFieldWriter(RedshiftBytesSchema source, RedshiftBytesSchema target)
      : source_(source), target_(target) {}
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    ArrowBufferView bytes{};
    NANOARROW_RETURN_NOT_OK(
        RedshiftBytesSchema::GetBytes(array_view_, index, &bytes, error));
    NANOARROW_RETURN_NOT_OK(
        RedshiftBytesSchema::Validate(bytes, source_.kind, source_.width, true, error));
    NANOARROW_RETURN_NOT_OK(
        RedshiftBytesSchema::Validate(bytes, target_.kind, target_.width, true, error));
    // All validation precedes buffer mutation; never trim overflow blanks.
    NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buffer, bytes.size_bytes + 4));
    WriteUnsafe<int32_t>(buffer, static_cast<int32_t>(bytes.size_bytes));
    ArrowBufferAppendUnsafe(buffer, bytes.data.data, bytes.size_bytes);
    return NANOARROW_OK;
  }

 private:
  RedshiftBytesSchema source_, target_;
};

class PostgresCopyBinaryFieldWriter : public PostgresCopyFieldWriter {
 public:
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    struct ArrowBufferView buffer_view = ArrowArrayViewGetBytesUnsafe(array_view_, index);
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, buffer_view.size_bytes, error));
    NANOARROW_RETURN_NOT_OK(
        ArrowBufferAppend(buffer, buffer_view.data.as_uint8, buffer_view.size_bytes));

    return ADBC_STATUS_OK;
  }
};

class PostgresCopyJsonbFieldWriter : public PostgresCopyFieldWriter {
 public:
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    struct ArrowBufferView buffer_view = ArrowArrayViewGetBytesUnsafe(array_view_, index);
    if (buffer_view.size_bytes >= (std::numeric_limits<int32_t>::max)()) {
      ArrowErrorSet(error, "[libpq] JSON value at row %" PRId64 " is too large", index);
      return EOVERFLOW;
    }

    const int32_t field_size_bytes = static_cast<int32_t>(buffer_view.size_bytes) + 1;
    constexpr int8_t kJsonbVersion = 1;
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int8_t>(buffer, kJsonbVersion, error));
    NANOARROW_RETURN_NOT_OK(
        ArrowBufferAppend(buffer, buffer_view.data.as_uint8, buffer_view.size_bytes));

    return ADBC_STATUS_OK;
  }
};

class PostgresCopyBinaryDictFieldWriter : public PostgresCopyFieldWriter {
 public:
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    int64_t dict_index = ArrowArrayViewGetIntUnsafe(array_view_, index);
    if (ArrowArrayViewIsNull(array_view_->dictionary, dict_index)) {
      constexpr int32_t field_size_bytes = -1;
      NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));
    } else {
      struct ArrowBufferView buffer_view =
          ArrowArrayViewGetBytesUnsafe(array_view_->dictionary, dict_index);
      NANOARROW_RETURN_NOT_OK(
          WriteChecked<int32_t>(buffer, buffer_view.size_bytes, error));
      NANOARROW_RETURN_NOT_OK(
          ArrowBufferAppend(buffer, buffer_view.data.as_uint8, buffer_view.size_bytes));
    }

    return ADBC_STATUS_OK;
  }
};

template <bool IsFixedSize>
class PostgresCopyListFieldWriter : public PostgresCopyFieldWriter {
 public:
  explicit PostgresCopyListFieldWriter(uint32_t child_oid,
                                       std::unique_ptr<PostgresCopyFieldWriter> child)
      : child_oid_{child_oid}, child_{std::move(child)} {}

  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    if (index >= array_view_->length) {
      return ENODATA;
    }

    constexpr int32_t ndim = 1;
    constexpr int32_t has_null_flags = 0;

    // TODO: the LARGE_LIST should use 64 bit indexes
    int32_t start, end;
    const int64_t logical = array_view_->offset + index;
    if constexpr (IsFixedSize) {
      start = logical * array_view_->layout.child_size_elements;
      end = start + array_view_->layout.child_size_elements;
    } else {
      start = ArrowArrayViewListChildOffset(array_view_, logical);
      end = ArrowArrayViewListChildOffset(array_view_, logical + 1);
    }

    const int32_t dim = end - start;
    constexpr int32_t lb = 1;

    // for children of a fixed size T we could avoid the use of a temporary buffer
    /// and theoretically just write
    //
    // const int32_t field_size_bytes =
    //    sizeof(ndim) + sizeof(has_null_flags) + sizeof(child_oid_) + sizeof(dim) * ndim
    //    + sizeof(lb) * ndim
    //    + sizeof(int32_t) * dim + T * dim;
    //
    // directly to our buffer
    nanoarrow::UniqueBuffer tmp;
    ArrowBufferInit(tmp.get());
    for (auto i = start; i < end; ++i) {
      NANOARROW_RETURN_NOT_OK(child_->Write(tmp.get(), i, error));
    }
    const int32_t field_size_bytes = sizeof(ndim) + sizeof(has_null_flags) +
                                     sizeof(child_oid_) + sizeof(dim) * ndim +
                                     sizeof(lb) * ndim + tmp->size_bytes;

    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, ndim, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, has_null_flags, error));
    NANOARROW_RETURN_NOT_OK(WriteChecked<uint32_t>(buffer, child_oid_, error));
    for (int32_t i = 0; i < ndim; ++i) {
      NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, dim, error));
      NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, lb, error));
    }

    NANOARROW_RETURN_NOT_OK(ArrowBufferAppend(buffer, tmp->data, tmp->size_bytes));

    return NANOARROW_OK;
  }

 private:
  const uint32_t child_oid_;
  std::unique_ptr<PostgresCopyFieldWriter> child_;
};

template <enum ArrowTimeUnit TU>
class PostgresCopyTimestampFieldWriter : public PostgresCopyFieldWriter {
 public:
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    constexpr int32_t field_size_bytes = sizeof(int64_t);
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, field_size_bytes, error));

    int64_t raw_value = ArrowArrayViewGetIntUnsafe(array_view_, index);
    int64_t value = 0;

    bool overflow_safe = true;
    switch (TU) {
      case NANOARROW_TIME_UNIT_SECOND:
        overflow_safe =
            raw_value <= kMaxSafeSecondsToMicros && raw_value >= kMinSafeSecondsToMicros;
        if (overflow_safe) {
          value = raw_value * 1000000;
        }
        break;
      case NANOARROW_TIME_UNIT_MILLI:
        overflow_safe =
            raw_value <= kMaxSafeMillisToMicros && raw_value >= kMinSafeMillisToMicros;
        if (overflow_safe) {
          value = raw_value * 1000;
        }
        break;
      case NANOARROW_TIME_UNIT_MICRO:
        value = raw_value;
        break;
      case NANOARROW_TIME_UNIT_NANO:
        value = raw_value / 1000;
        break;
    }

    if (!overflow_safe) {
      ArrowErrorSet(error,
                    "[libpq] Row %" PRId64 " timestamp value %" PRId64
                    " with unit %d would overflow",
                    index, raw_value, TU);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }

    if (value < (std::numeric_limits<int64_t>::min)() + kPostgresTimestampEpoch) {
      ArrowErrorSet(error,
                    "[libpq] Row %" PRId64 " timestamp value %" PRId64
                    " with unit %d would underflow",
                    index, raw_value, TU);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }

    const int64_t scaled = value - kPostgresTimestampEpoch;
    NANOARROW_RETURN_NOT_OK(WriteChecked<int64_t>(buffer, scaled, error));

    return ADBC_STATUS_OK;
  }
};

// Redshift's reference interval parameters have no generic PostgreSQL body.
class RedshiftIntervalFieldWriter : public PostgresCopyFieldWriter {
 public:
  RedshiftIntervalFieldWriter(RedshiftIntervalSchema source,
                              std::optional<RedshiftIntervalConstraint> target)
      : source_(std::move(source)), target_(std::move(target)) {}
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t index, ArrowError* error) override {
    int64_t native;
    NANOARROW_RETURN_NOT_OK(source_.ToNative(array_view_, index, &native, error));
    if (target_) NANOARROW_RETURN_NOT_OK(target_->CheckValue(native, error));
    const bool months = source_.constraint.family == RedshiftIntervalFamily::kYearMonth;
    // Reserve before emitting length/body: all value checks precede mutation.
    NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buffer, months ? 8 : 12));
    NANOARROW_RETURN_NOT_OK(WriteChecked<int32_t>(buffer, months ? 4 : 8, error));
    if (months) return WriteChecked<int32_t>(buffer, static_cast<int32_t>(native), error);
    return WriteChecked<int64_t>(buffer, native, error);
  }

 private:
  RedshiftIntervalSchema source_;
  std::optional<RedshiftIntervalConstraint> target_;
};

static inline ArrowErrorCode MakeCopyFieldWriter(
    struct ArrowSchema* schema, struct ArrowArrayView* array_view,
    const PostgresTypeResolver& type_resolver, const PostgresType* target_type,
    std::unique_ptr<PostgresCopyFieldWriter>* out, bool disable_decimal_fast_path,
    ArrowError* error) {
  NANOARROW_RETURN_NOT_OK(
      PostgresType::ValidateDecimalWriteSchema(type_resolver, schema, error));
  NANOARROW_RETURN_NOT_OK(
      PostgresType::ValidateScalarWriteSchema(type_resolver, schema, error));
  NANOARROW_RETURN_NOT_OK(
      PostgresType::ValidateBytesWriteSchema(type_resolver, schema, error));
  struct ArrowSchemaView schema_view;
  NANOARROW_RETURN_NOT_OK(ArrowSchemaViewInit(&schema_view, schema, error));

  NANOARROW_RETURN_NOT_OK(
      PostgresType::ValidateTemporalWriteSchema(type_resolver, schema, error));
  NANOARROW_RETURN_NOT_OK(
      PostgresType::ValidateIntervalWriteSchema(type_resolver, schema, error));
  RedshiftIntervalSchema interval;
  NANOARROW_RETURN_NOT_OK(RedshiftIntervalSchema::Parse(
      schema, type_resolver.is_redshift(), &interval, error));
  const auto interval_target =
      target_type ? target_type->interval_family() : RedshiftIntervalFamily::kNone;
  if (interval.enabled() ||
      (type_resolver.is_redshift() && interval_target != RedshiftIntervalFamily::kNone)) {
    std::optional<RedshiftIntervalConstraint> target;
    if (target_type) {
      if (interval_target == RedshiftIntervalFamily::kNone)
        return RedshiftIntervalConstraint::Invalid(error, "cross-family actual target");
      target = target_type->interval_constraint();
      if (target->family != interval_target)
        return RedshiftIntervalConstraint::Invalid(
            error, "target context and OID family conflict");
      NANOARROW_RETURN_NOT_OK(target->RequireKnown(error));
    }
    // Genuine unmarked NA is only safe after validating this actual target.
    if (schema_view.type == NANOARROW_TYPE_NA && target && !interval.enabled()) {
      *out = PostgresCopyFieldWriter::Create<PostgresCopyNullFieldWriter>(array_view);
      return NANOARROW_OK;
    }
    if (!interval.enabled() || (target && interval.constraint.family != target->family))
      return RedshiftIntervalConstraint::Invalid(error, "cross-family actual target");
    *out = PostgresCopyFieldWriter::Create<RedshiftIntervalFieldWriter>(array_view,
                                                                        interval, target);
    return NANOARROW_OK;
  }
  RedshiftTemporalSchema temporal;
  NANOARROW_RETURN_NOT_OK(RedshiftTemporalSchema::Parse(
      schema, type_resolver.is_redshift(), &temporal, error));
  const auto temporal_target =
      target_type ? target_type->temporal_kind() : RedshiftTemporalKind::kNone;
  if (temporal.enabled() ||
      (type_resolver.is_redshift() && temporal_target != RedshiftTemporalKind::kNone)) {
    if (schema_view.type == NANOARROW_TYPE_NA &&
        temporal_target != RedshiftTemporalKind::kNone) {
      *out = PostgresCopyFieldWriter::Create<PostgresCopyNullFieldWriter>(array_view);
      return NANOARROW_OK;
    }
    if (!temporal.enabled() || (target_type && temporal_target != temporal.kind))
      return RedshiftTemporalSchema::Invalid(error, "cross-family temporal target");
    std::optional<int32_t> target_precision;
    if (target_type && temporal.kind != RedshiftTemporalKind::kDate) {
      NANOARROW_RETURN_NOT_OK(RedshiftTemporalSchema::Precision(
          target_type->type_modifier(), &target_precision, error));
      if (!target_precision)
        return RedshiftTemporalSchema::Invalid(error,
                                               "unknown target fractional precision");
    }
    *out = PostgresCopyFieldWriter::Create<RedshiftTemporalFieldWriter>(
        array_view, temporal, target_precision);
    return NANOARROW_OK;
  }

  RedshiftDecimalSchema source_decimal;
  NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Parse(
      schema, type_resolver.is_redshift(), &source_decimal, error));
  const bool strict_target = target_type &&
                             target_type->type_id() == PostgresTypeId::kNumeric &&
                             type_resolver.is_redshift();
  if (source_decimal.enabled || strict_target) {
    std::optional<RedshiftDecimalSchema> target_decimal;
    if (target_type) {
      if (target_type->type_id() != PostgresTypeId::kNumeric) {
        return RedshiftDecimalSchema::Invalid(error,
                                              "decimal source requires numeric target");
      }
      const auto info =
          adbc::driver::pgwire::BackendProfile::Redshift().DescribeTypeModifier(
              "numeric", target_type->type_modifier());
      if (!info.precision || !info.scale || *info.scale > 37) {
        return RedshiftDecimalSchema::Invalid(
            error, "unknown/unsupported stored target descriptor");
      }
      target_decimal = RedshiftDecimalSchema{true, false, info.precision, info.scale};
    }
    if (schema_view.type == NANOARROW_TYPE_NA) {
      *out = PostgresCopyFieldWriter::Create<PostgresCopyNullFieldWriter>(array_view);
      return NANOARROW_OK;
    }
    if (!source_decimal.enabled) {
      return RedshiftDecimalSchema::Invalid(
          error, "numeric ingest requires decimal, marked text or null");
    }
    *out = PostgresCopyFieldWriter::Create<RedshiftDecimalFieldWriter>(
        array_view, source_decimal, target_decimal, schema_view.type);
    return NANOARROW_OK;
  }

  RedshiftScalarSchema source_scalar;
  NANOARROW_RETURN_NOT_OK(RedshiftScalarSchema::Parse(schema, type_resolver.is_redshift(),
                                                      &source_scalar, error));
  const auto scalar_target =
      target_type ? target_type->scalar_kind() : RedshiftScalarKind::kNone;
  if (source_scalar.enabled() ||
      (type_resolver.is_redshift() && scalar_target != RedshiftScalarKind::kNone)) {
    if (target_type && scalar_target == RedshiftScalarKind::kNone)
      return RedshiftScalarSchema::Invalid(error,
                                           "scalar source requires compatible target");
    if (schema_view.type == NANOARROW_TYPE_NA) {
      *out = PostgresCopyFieldWriter::Create<PostgresCopyNullFieldWriter>(array_view);
      return NANOARROW_OK;
    }
    const auto destination = target_type ? scalar_target : source_scalar.kind;
    if (!source_scalar.enabled() ||
        !RedshiftScalarSchema::Compatible(source_scalar.kind, destination))
      return RedshiftScalarSchema::Invalid(error, "unsupported scalar target conversion");
    *out = PostgresCopyFieldWriter::Create<RedshiftScalarFieldWriter>(
        array_view, source_scalar.kind, destination);
    return NANOARROW_OK;
  }

  RedshiftBytesSchema source_bytes;
  NANOARROW_RETURN_NOT_OK(RedshiftBytesSchema::Parse(schema, type_resolver.is_redshift(),
                                                     &source_bytes, error));
  const auto bytes_target =
      target_type ? target_type->bytes_kind() : RedshiftBytesKind::kNone;
  if (source_bytes.enabled() ||
      (type_resolver.is_redshift() && bytes_target != RedshiftBytesKind::kNone)) {
    if (target_type && bytes_target == RedshiftBytesKind::kNone)
      return RedshiftBytesSchema::Invalid(
          error, "character/binary source requires compatible target");
    if (schema_view.type == NANOARROW_TYPE_NA) {
      *out = PostgresCopyFieldWriter::Create<PostgresCopyNullFieldWriter>(array_view);
      return NANOARROW_OK;
    }
    RedshiftBytesSchema destination = source_bytes;
    if (target_type) {
      destination.kind = bytes_target;
      // Width comes from the actual aligned destination, not source rows/schema.
      const char* name = bytes_target == RedshiftBytesKind::kChar      ? "bpchar"
                         : bytes_target == RedshiftBytesKind::kVarchar ? "varchar"
                                                                       : "varbyte";
      destination.width = adbc::driver::pgwire::BackendProfile::Redshift()
                              .DescribeTypeModifier(name, target_type->type_modifier())
                              .byte_width;
      // Ordinary PostgreSQL TEXT/BYTEA have no width descriptor; explicit semantic
      // sources still retain their own limits while these destinations are unbounded.
      const bool pg_unbounded = !type_resolver.is_redshift() &&
                                (target_type->type_id() == PostgresTypeId::kText ||
                                 target_type->type_id() == PostgresTypeId::kBytea);
      if (!destination.width && !pg_unbounded)
        return RedshiftBytesSchema::Invalid(error, "unknown destination byte width");
    }
    if (!source_bytes.enabled() || ((source_bytes.kind == RedshiftBytesKind::kVarbyte) !=
                                    (destination.kind == RedshiftBytesKind::kVarbyte)))
      return RedshiftBytesSchema::Invalid(error, "unsupported text/binary coercion");
    *out = PostgresCopyFieldWriter::Create<RedshiftBytesFieldWriter>(
        array_view, source_bytes, destination);
    return NANOARROW_OK;
  }

  const bool is_arrow_json =
      schema_view.extension_name.data != nullptr &&
      std::string_view(schema_view.extension_name.data,
                       schema_view.extension_name.size_bytes) == "arrow.json";
  if (target_type != nullptr && target_type->type_id() == PostgresTypeId::kJsonb &&
      is_arrow_json) {
    switch (schema_view.type) {
      case NANOARROW_TYPE_STRING:
      case NANOARROW_TYPE_LARGE_STRING:
      case NANOARROW_TYPE_STRING_VIEW: {
        using T = PostgresCopyJsonbFieldWriter;
        *out = T::Create<T>(array_view);
        return NANOARROW_OK;
      }
      default:
        break;
    }
  }

  if (target_type != nullptr && target_type->type_id() == PostgresTypeId::kNumeric) {
    switch (schema_view.type) {
      case NANOARROW_TYPE_INT8:
      case NANOARROW_TYPE_INT16:
      case NANOARROW_TYPE_INT32:
      case NANOARROW_TYPE_INT64: {
        using T = PostgresCopyIntegerNumericFieldWriter;
        *out = T::Create<T>(array_view, /*is_signed=*/true);
        return NANOARROW_OK;
      }
      case NANOARROW_TYPE_UINT8:
      case NANOARROW_TYPE_UINT16:
      case NANOARROW_TYPE_UINT32:
      case NANOARROW_TYPE_UINT64: {
        using T = PostgresCopyIntegerNumericFieldWriter;
        *out = T::Create<T>(array_view, /*is_signed=*/false);
        return NANOARROW_OK;
      }
      case NANOARROW_TYPE_NA:
      case NANOARROW_TYPE_DECIMAL128:
      case NANOARROW_TYPE_DECIMAL256:
        break;
      default:
        ArrowErrorSet(error,
                      "COPY Writer from Arrow type '%s' to PostgreSQL numeric is not "
                      "implemented",
                      ArrowTypeString(schema_view.type));
        return ENOTSUP;
    }
  }

  switch (schema_view.type) {
    case NANOARROW_TYPE_NA:
      *out = PostgresCopyFieldWriter::Create<PostgresCopyNullFieldWriter>(array_view);
      return NANOARROW_OK;
    case NANOARROW_TYPE_BOOL:
      using T = PostgresCopyBooleanFieldWriter;
      *out = T::Create<T>(array_view);
      return NANOARROW_OK;
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_UINT8: {
      using T = PostgresCopyNetworkEndianFieldWriter<int16_t>;
      *out = T::Create<T>(array_view);
      return NANOARROW_OK;
    }
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_UINT16: {
      using T = PostgresCopyNetworkEndianFieldWriter<int32_t>;
      *out = T::Create<T>(array_view);
      return NANOARROW_OK;
    }
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_INT64:
    case NANOARROW_TYPE_UINT64: {
      using T = PostgresCopyNetworkEndianFieldWriter<int64_t>;
      *out = T::Create<T>(array_view);
      return NANOARROW_OK;
    }
    case NANOARROW_TYPE_DATE32: {
      constexpr int32_t kPostgresDateEpoch = 10957;
      using T = PostgresCopyNetworkEndianFieldWriter<int32_t, kPostgresDateEpoch>;
      *out = T::Create<T>(array_view);
      return NANOARROW_OK;
    }
    case NANOARROW_TYPE_TIME64: {
      switch (schema_view.time_unit) {
        case NANOARROW_TIME_UNIT_MICRO:
          using T = PostgresCopyNetworkEndianFieldWriter<int64_t>;
          *out = T::Create<T>(array_view);
          return NANOARROW_OK;
        default:
          return ADBC_STATUS_NOT_IMPLEMENTED;
      }
    }
    case NANOARROW_TYPE_HALF_FLOAT:
    case NANOARROW_TYPE_FLOAT: {
      using T = PostgresCopyFloatFieldWriter;
      *out = T::Create<T>(array_view);
      return NANOARROW_OK;
    }
    case NANOARROW_TYPE_DOUBLE: {
      using T = PostgresCopyDoubleFieldWriter;
      *out = T::Create<T>(array_view);
      return NANOARROW_OK;
    }
    case NANOARROW_TYPE_DECIMAL128: {
      using T = PostgresCopyNumericFieldWriter<NANOARROW_TYPE_DECIMAL128>;
      const auto precision = schema_view.decimal_precision;
      const auto scale = schema_view.decimal_scale;
      *out = T::Create<T>(array_view, precision, scale, disable_decimal_fast_path);
      return NANOARROW_OK;
    }
    case NANOARROW_TYPE_DECIMAL256: {
      using T = PostgresCopyNumericFieldWriter<NANOARROW_TYPE_DECIMAL256>;
      const auto precision = schema_view.decimal_precision;
      const auto scale = schema_view.decimal_scale;
      *out = T::Create<T>(array_view, precision, scale, disable_decimal_fast_path);
      return NANOARROW_OK;
    }
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_BINARY:
    case NANOARROW_TYPE_FIXED_SIZE_BINARY:
    case NANOARROW_TYPE_BINARY_VIEW:
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_STRING_VIEW: {
      using T = PostgresCopyBinaryFieldWriter;
      *out = T::Create<T>(array_view);
      return NANOARROW_OK;
    }
    case NANOARROW_TYPE_TIMESTAMP: {
      switch (schema_view.time_unit) {
        case NANOARROW_TIME_UNIT_NANO: {
          using T = PostgresCopyTimestampFieldWriter<NANOARROW_TIME_UNIT_NANO>;
          *out = T::Create<T>(array_view);
          break;
        }
        case NANOARROW_TIME_UNIT_MILLI: {
          using T = PostgresCopyTimestampFieldWriter<NANOARROW_TIME_UNIT_MILLI>;
          *out = T::Create<T>(array_view);
          break;
        }
        case NANOARROW_TIME_UNIT_MICRO: {
          using T = PostgresCopyTimestampFieldWriter<NANOARROW_TIME_UNIT_MICRO>;
          *out = T::Create<T>(array_view);
          break;
        }
        case NANOARROW_TIME_UNIT_SECOND: {
          using T = PostgresCopyTimestampFieldWriter<NANOARROW_TIME_UNIT_SECOND>;
          *out = T::Create<T>(array_view);
          break;
        }
      }
      return NANOARROW_OK;
    }
    case NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO: {
      using T = PostgresCopyIntervalFieldWriter;
      *out = T::Create<T>(array_view);
      return NANOARROW_OK;
    }
    case NANOARROW_TYPE_DURATION: {
      switch (schema_view.time_unit) {
        case NANOARROW_TIME_UNIT_SECOND: {
          using T = PostgresCopyDurationFieldWriter<NANOARROW_TIME_UNIT_SECOND>;
          *out = T::Create<T>(array_view);
          break;
        }
        case NANOARROW_TIME_UNIT_MILLI: {
          using T = PostgresCopyDurationFieldWriter<NANOARROW_TIME_UNIT_MILLI>;
          *out = T::Create<T>(array_view);
          break;
        }
        case NANOARROW_TIME_UNIT_MICRO: {
          using T = PostgresCopyDurationFieldWriter<NANOARROW_TIME_UNIT_MICRO>;
          *out = T::Create<T>(array_view);
          break;
        }
        case NANOARROW_TIME_UNIT_NANO: {
          using T = PostgresCopyDurationFieldWriter<NANOARROW_TIME_UNIT_NANO>;
          *out = T::Create<T>(array_view);
          break;
        }
      }
      return NANOARROW_OK;
    }
    case NANOARROW_TYPE_DICTIONARY: {
      struct ArrowSchemaView value_view;
      NANOARROW_RETURN_NOT_OK(
          ArrowSchemaViewInit(&value_view, schema->dictionary, error));
      switch (value_view.type) {
        case NANOARROW_TYPE_BINARY:
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_BINARY:
        case NANOARROW_TYPE_LARGE_STRING: {
          using T = PostgresCopyBinaryDictFieldWriter;
          *out = T::Create<T>(array_view);
          return NANOARROW_OK;
        }
        default:
          break;
      }
      break;
    }
    case NANOARROW_TYPE_LIST:
    case NANOARROW_TYPE_LARGE_LIST:
    case NANOARROW_TYPE_FIXED_SIZE_LIST: {
      // For now our implementation only supports primitive children types
      // See PostgresCopyListFieldWriter::Write for limitations
      struct ArrowSchemaView child_schema_view;
      NANOARROW_RETURN_NOT_OK(
          ArrowSchemaViewInit(&child_schema_view, schema->children[0], error));
      PostgresType child_type;
      const PostgresType* target_child_type = nullptr;
      if (target_type != nullptr && target_type->type_id() == PostgresTypeId::kArray &&
          target_type->n_children() == 1) {
        target_child_type = &target_type->child(0);
        child_type = *target_child_type;
      } else {
        NANOARROW_RETURN_NOT_OK(PostgresType::FromSchema(
            type_resolver, schema->children[0], &child_type, error));
      }

      std::unique_ptr<PostgresCopyFieldWriter> child_writer;
      NANOARROW_RETURN_NOT_OK(MakeCopyFieldWriter(
          schema->children[0], array_view->children[0], type_resolver, target_child_type,
          &child_writer, disable_decimal_fast_path, error));

      if (schema_view.type == NANOARROW_TYPE_FIXED_SIZE_LIST) {
        using T = PostgresCopyListFieldWriter<true>;
        *out = T::Create<T>(array_view, child_type.oid(), std::move(child_writer));
      } else {
        using T = PostgresCopyListFieldWriter<false>;
        *out = T::Create<T>(array_view, child_type.oid(), std::move(child_writer));
      }
      return NANOARROW_OK;
    }
    default:
      break;
  }

  ArrowErrorSet(error, "COPY Writer not implemented for type %d", schema_view.type);
  return EINVAL;
}

class PostgresCopyStreamWriter {
 public:
  ArrowErrorCode Init(struct ArrowSchema* schema) {
    schema_ = schema;
    NANOARROW_RETURN_NOT_OK(
        ArrowArrayViewInitFromSchema(&array_view_.value, schema, nullptr));
    root_writer_ = PostgresCopyFieldTupleWriter::Create<PostgresCopyFieldTupleWriter>(
        &array_view_.value);
    ArrowBufferInit(&buffer_.value);
    return NANOARROW_OK;
  }

  ArrowErrorCode SetArray(struct ArrowArray* array) {
    NANOARROW_RETURN_NOT_OK(ArrowArrayViewSetArray(&array_view_.value, array, nullptr));
    return NANOARROW_OK;
  }

  ArrowErrorCode WriteHeader(ArrowError* error) {
    NANOARROW_RETURN_NOT_OK(ArrowBufferAppend(&buffer_.value, kPgCopyBinarySignature,
                                              sizeof(kPgCopyBinarySignature)));

    const uint32_t flag_fields = 0;
    NANOARROW_RETURN_NOT_OK(
        ArrowBufferAppend(&buffer_.value, &flag_fields, sizeof(flag_fields)));

    const uint32_t extension_bytes = 0;
    NANOARROW_RETURN_NOT_OK(
        ArrowBufferAppend(&buffer_.value, &extension_bytes, sizeof(extension_bytes)));

    return NANOARROW_OK;
  }

  ArrowErrorCode WriteRecord(ArrowError* error) {
    NANOARROW_RETURN_NOT_OK(root_writer_->Write(&buffer_.value, records_written_, error));
    records_written_++;
    return NANOARROW_OK;
  }

  ArrowErrorCode InitFieldWriters(const PostgresTypeResolver& type_resolver,
                                  const PostgresType* target_types,
                                  bool disable_decimal_fast_path, ArrowError* error) {
    if (schema_->release == nullptr) {
      return EINVAL;
    }

    for (int64_t i = 0; i < schema_->n_children; i++) {
      std::unique_ptr<PostgresCopyFieldWriter> child_writer;
      const PostgresType* target_type = nullptr;
      if (target_types != nullptr && i < target_types->n_children()) {
        target_type = &target_types->child(i);
      }
      NANOARROW_RETURN_NOT_OK(MakeCopyFieldWriter(
          schema_->children[i], array_view_->children[i], type_resolver, target_type,
          &child_writer, disable_decimal_fast_path, error));
      root_writer_->AppendChild(std::move(child_writer));
    }

    return NANOARROW_OK;
  }

  const struct ArrowBuffer& WriteBuffer() const { return buffer_.value; }

  void Rewind() {
    records_written_ = 0;
    buffer_->size_bytes = 0;
  }

 private:
  std::unique_ptr<PostgresCopyFieldTupleWriter> root_writer_;
  struct ArrowSchema* schema_;
  Handle<struct ArrowArrayView> array_view_;
  Handle<struct ArrowBuffer> buffer_;
  int64_t records_written_ = 0;
};

}  // namespace adbcpq
