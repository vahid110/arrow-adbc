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

#include "native_protocol_capture.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace adbc::qualification {
namespace detail {
bool CheckedAdd(uint64_t a, uint64_t b, uint64_t ceiling, uint64_t* out) noexcept {
  if (!out || a > ceiling || b > ceiling - a) return false;
  *out = a + b;
  return true;
}
bool CheckedMultiply(uint64_t a, uint64_t b, uint64_t ceiling, uint64_t* out) noexcept {
  if (!out || (a && b > ceiling / a)) return false;
  *out = a * b;
  return true;
}
}  // namespace detail
namespace {
struct Definition {
  const char* name;
  int group;
  const char* form;
  int format;
};
constexpr Definition definitions[] = {
    {"vendor_mixed.values.text", 0, "values", 0},
    {"vendor_mixed.values.binary", 0, "values", 1},
    {"vendor_mixed.values.describe", 0, "values", -1},
    {"vendor_mixed.nulls.text", 0, "nulls", 0},
    {"vendor_mixed.nulls.binary", 0, "nulls", 1},
    {"vendor_mixed.nulls.describe", 0, "nulls", -1},
    {"vendor_mixed.empty.text", 0, "empty", 0},
    {"vendor_mixed.empty.binary", 0, "empty", 1},
    {"vendor_mixed.empty.describe", 0, "empty", -1},
    {"vendor_mixed.parameter_describe", 0, "parameter_describe", -1},
    {"interval37.values.text", 1, "values", 0},
    {"interval37.values.binary", 1, "values", 1},
    {"interval37.values.describe", 1, "values", -1},
    {"interval37.nulls.text", 1, "nulls", 0},
    {"interval37.nulls.binary", 1, "nulls", 1},
    {"interval37.nulls.describe", 1, "nulls", -1},
    {"interval37.empty.text", 1, "empty", 0},
    {"interval37.empty.binary", 1, "empty", 1},
    {"interval37.empty.describe", 1, "empty", -1},
    {"interval37.parameter_describe", 1, "parameter_describe", -1},
    {"float_bits.values.text", 2, "values", 0},
    {"float_bits.values.binary", 2, "values", 1},
    {"float_bits.values.describe", 2, "values", -1},
    {"float_bits.nulls.text", 2, "nulls", 0},
    {"float_bits.nulls.binary", 2, "nulls", 1},
    {"float_bits.nulls.describe", 2, "nulls", -1},
    {"float_bits.empty.text", 2, "empty", 0},
    {"float_bits.empty.binary", 2, "empty", 1},
    {"float_bits.empty.describe", 2, "empty", -1},
    {"float_bits.parameter_describe", 2, "parameter_describe", -1},
    {"catalog.before", -1, "", 0},
    {"catalog.after", -1, "", 0},
    {"svv.before", -1, "", 0},
    {"svv.after", -1, "", 0},
    {"plumbing.error", -1, "", 0},
};
static_assert(sizeof(definitions) / sizeof(definitions[0]) ==
              static_cast<size_t>(ProbeId::kCount));
const Definition* Define(ProbeId id) {
  const auto index = static_cast<size_t>(id);
  return index < static_cast<size_t>(ProbeId::kCount) ? &definitions[index] : nullptr;
}
std::vector<std::string> Columns(int group) {
  switch (group) {
    case 0:
      return {
          "case_id",         "smallint_value",    "integer_value",    "bigint_value",
          "decimal_value",   "real_value",        "double_value",     "char_value",
          "varchar_value",   "date_value",        "time_value",       "timetz_value",
          "timestamp_value", "timestamptz_value", "year_month_value", "day_second_value",
          "boolean_value",   "hllsketch_value",   "super_value",      "varbyte_value",
          "geometry_value",  "geography_value"};
    case 1:
      return {"case_id",
              "q01_year",
              "q02_month",
              "q03_year_to_month",
              "q04_day",
              "q05_hour",
              "q06_minute",
              "q07_second_p0",
              "q08_second_p1",
              "q09_second_p2",
              "q10_second_p3",
              "q11_second_p4",
              "q12_second_p5",
              "q13_second_p6",
              "q14_day_to_hour",
              "q15_day_to_minute",
              "q16_day_to_second_p0",
              "q17_day_to_second_p1",
              "q18_day_to_second_p2",
              "q19_day_to_second_p3",
              "q20_day_to_second_p4",
              "q21_day_to_second_p5",
              "q22_day_to_second_p6",
              "q23_hour_to_minute",
              "q24_hour_to_second_p0",
              "q25_hour_to_second_p1",
              "q26_hour_to_second_p2",
              "q27_hour_to_second_p3",
              "q28_hour_to_second_p4",
              "q29_hour_to_second_p5",
              "q30_hour_to_second_p6",
              "q31_minute_to_second_p0",
              "q32_minute_to_second_p1",
              "q33_minute_to_second_p2",
              "q34_minute_to_second_p3",
              "q35_minute_to_second_p4",
              "q36_minute_to_second_p5",
              "q37_minute_to_second_p6"};
    case 2:
      return {"case_id", "real_value", "double_value"};
    default:
      return {};
  }
}
constexpr const char* relations[] = {"vendor_mixed", "interval37", "float_bits"};
std::vector<std::string> ExpectedColumns(ProbeId id, const Definition& def) {
  if (def.group >= 0) return Columns(def.group);
  if (id == ProbeId::kCatalogBefore || id == ProbeId::kCatalogAfter)
    return {"schema_name",  "relation_name", "relation_oid", "ordinal",
            "column_name",  "type_oid",      "raw_modifier", "type_name",
            "receive_name", "formatted_type"};
  if (id == ProbeId::kSvvBefore || id == ProbeId::kSvvAfter)
    return {"schema_name",        "relation_name",     "ordinal",
            "column_name",        "data_type",         "interval_type",
            "interval_precision", "datetime_precision"};
  return {"expected_error"};
}
bool ValidLimits(const CaptureLimits& value) {
  const CaptureLimits hard;
  return value.fields >= 0 && value.fields <= hard.fields && value.parameters >= 0 &&
         value.parameters <= hard.parameters && value.rows >= 0 &&
         value.rows <= hard.rows && value.name_bytes >= 0 &&
         value.name_bytes <= hard.name_bytes && value.cell_bytes >= 0 &&
         value.cell_bytes <= hard.cell_bytes && value.body_bytes >= 0 &&
         value.body_bytes <= hard.body_bytes && value.encoded_bytes >= 0 &&
         value.encoded_bytes <= hard.encoded_bytes;
}
// Count exact JSON expansion first, reserve once, then append within that same
// bound. All variable byte strings are hex, never arbitrary JSON text/error prose.
class Encoder {
 public:
  explicit Encoder(uint64_t ceiling, std::string* out = nullptr)
      : ceiling_(ceiling), out_(out) {}
  bool Text(std::string_view value) {
    uint64_t next;
    if (!detail::CheckedAdd(size_, value.size(), ceiling_, &next)) return false;
    if (out_) out_->append(value);
    size_ = next;
    return true;
  }
  template <class T>
  bool Number(T value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return result.ec == std::errc() &&
           Text(std::string_view(buffer, result.ptr - buffer));
  }
  template <class T>
  bool MaybeNumber(const std::optional<T>& value) {
    return value ? Number(*value) : Text("null");
  }
  bool Hex(std::string_view value) {
    uint64_t expanded;
    if (!detail::CheckedMultiply(value.size(), 2, ceiling_, &expanded) ||
        expanded > ceiling_ - size_)
      return false;
    if (!Text("\"")) return false;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char byte : value) {
      char pair[] = {hex[byte >> 4], hex[byte & 15]};
      if (!Text(std::string_view(pair, 2))) return false;
    }
    return Text("\"");
  }
  uint64_t size() const { return size_; }

 private:
  uint64_t ceiling_, size_ = 0;
  std::string* out_;
};
bool Serialize(Encoder* e, const Definition& def, CaptureStatus status,
               const Observation& value) {
  auto text = [&](std::string_view s) { return e->Text(s); };
  if (!text("{\"version\":1,\"probe\":\"") || !text(def.name) ||
      !text("\",\"operation\":\"") || !text(def.format < 0 ? "describe" : "rows") ||
      !text("\",\"requested_format\":"))
    return false;
  if (def.format < 0) {
    if (!text("null")) return false;
  } else if (!e->Number(def.format))
    return false;
  if (!text(",\"result_status\":") || !e->MaybeNumber(value.result_status) ||
      !text(",\"outcome\":\"") || !text(CaptureStatusName(status)) ||
      !text("\",\"complete\":") || !text(value.complete ? "true" : "false") ||
      !text(",\"row_count\":") || !e->MaybeNumber(value.row_count) ||
      !text(",\"field_count\":") || !e->MaybeNumber(value.field_count) ||
      !text(",\"sqlstate\":"))
    return false;
  if (value.sqlstate) {
    if (!text("\"") || !text(*value.sqlstate) || !text("\"")) return false;
  } else if (!text("null"))
    return false;
  if (!text(",\"failure_row\":")) return false;
  if (value.failure_row) {
    if (!e->Number(*value.failure_row)) return false;
  } else if (!text("null"))
    return false;
  if (!text(",\"failure_field\":")) return false;
  if (value.failure_field) {
    if (!e->Number(*value.failure_field)) return false;
  } else if (!text("null"))
    return false;
  if (!text(",\"failure_length\":")) return false;
  if (value.failure_length) {
    if (!e->Number(*value.failure_length)) return false;
  } else if (!text("null"))
    return false;
  if (!text(",\"fields\":[")) return false;
  for (size_t i = 0; i < value.fields.size(); ++i) {
    const auto& f = value.fields[i];
    if ((i && !text(",")) || !text("{\"ordinal\":") || !e->Number(i + 1) ||
        !text(",\"name_hex\":") || !e->Hex(f.name) || !text(",\"oid\":") ||
        !e->Number(f.oid) || !text(",\"raw_modifier\":") || !e->Number(f.modifier) ||
        !text(",\"format\":") || !e->Number(f.format) || !text(",\"table_oid\":") ||
        !e->Number(f.table_oid) || !text(",\"table_column\":") ||
        !e->Number(f.table_column) || !text("}"))
      return false;
  }
  if (!text("],\"parameters\":")) return false;
  if (value.parameters) {
    if (!text("[")) return false;
    for (size_t i = 0; i < value.parameters->size(); ++i)
      if ((i && !text(",")) || !e->Number((*value.parameters)[i])) return false;
    if (!text("]")) return false;
  } else if (!text("null"))
    return false;
  if (!text(",\"rows\":[")) return false;
  for (size_t row = 0; row < value.rows.size(); ++row) {
    if ((row && !text(",")) || !text("[")) return false;
    for (size_t col = 0; col < value.rows[row].size(); ++col) {
      const auto& cell = value.rows[row][col];
      if ((col && !text(",")) || !text("{\"null\":") ||
          !text(cell.is_null ? "true" : "false") || !text(",\"length\":") ||
          !e->Number(cell.length) || !text(",\"bytes_hex\":"))
        return false;
      if (cell.is_null) {
        if (!text("null")) return false;
      } else if (!e->Hex(cell.bytes))
        return false;
      if (!text("}")) return false;
    }
    if (!text("]")) return false;
  }
  return text("]}");
}
CaptureStatus Finish(const Definition& def, CaptureStatus status, Observation&& value,
                     const CaptureLimits& limits, Observation* out) {
  value.complete = status == CaptureStatus::kComplete;
  if (!value.complete && status != CaptureStatus::kFormatMismatch) value.rows.clear();
  Encoder count(limits.encoded_bytes);
  if (!Serialize(&count, def, status, value)) return CaptureStatus::kEncodingLimit;
  value.json.reserve(static_cast<size_t>(count.size()));
  Encoder writer(limits.encoded_bytes, &value.json);
  if (!Serialize(&writer, def, status, value)) return CaptureStatus::kEncodingLimit;
  value.present = true;
  *out = std::move(value);
  return status;
}
}  // namespace
bool ValidOwnedSchemaSyntax(std::string_view schema) noexcept {
  if (schema.size() != 43 || !schema.starts_with("adbc_types_")) return false;
  for (char c : schema.substr(11))
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
std::optional<ProbePlan> BuildProbe(ProbeId id, std::string_view schema) noexcept {
  try {
    const auto* def = Define(id);
    if (!def || !ValidOwnedSchemaSyntax(schema)) return std::nullopt;
    ProbePlan out;
    out.name = def->name;
    out.columns = ExpectedColumns(id, *def);
    out.operation = def->format < 0 ? Operation::kDescribe : Operation::kRows;
    if (def->format >= 0) out.requested_format = def->format;
    out.max_rows = def->format < 0 ? 0 : def->group < 0 ? 63 : 8;
    if (def->format < 0)
      out.expected_parameters =
          std::string_view(def->form) == "parameter_describe" ? 1 : 0;
    if (id == ProbeId::kPlumbingError) {
      out.sql = "SELECT 1/0 AS expected_error";
      out.max_rows = 8;
      return out;
    }
    if (def->group >= 0) {
      out.sql = "SELECT ";
      for (size_t col = 0; col < out.columns.size(); ++col) {
        if (col) out.sql += ",";
        out.sql += '\"' + out.columns[col] + '\"';
      }
      out.sql +=
          " FROM \"" + std::string(schema) + "\".\"" + relations[def->group] + "\" ";
      const std::string_view form(def->form);
      if (form == "values")
        out.sql += "WHERE case_id IN (1,2) ORDER BY case_id";
      else if (form == "nulls")
        out.sql += "WHERE case_id=3 ORDER BY case_id";
      else if (form == "empty")
        out.sql += "WHERE 1=0";
      else
        out.sql += "WHERE case_id=$1 ORDER BY case_id";
    } else if (id == ProbeId::kCatalogBefore || id == ProbeId::kCatalogAfter) {
      out.sql =
          "SELECT n.nspname AS schema_name,c.relname AS relation_name,c.oid AS "
          "relation_oid,a.attnum AS ordinal,a.attname AS column_name,a.atttypid AS "
          "type_oid,a.atttypmod AS raw_modifier,t.typname AS "
          "type_name,t.typreceive::regproc::text AS "
          "receive_name,pg_catalog.format_type(a.atttypid,a.atttypmod) AS formatted_type "
          "FROM pg_catalog.pg_attribute a JOIN pg_catalog.pg_class c ON c.oid=a.attrelid "
          "JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace JOIN "
          "pg_catalog.pg_type t ON t.oid=a.atttypid WHERE n.nspname='" +
          std::string(schema) +
          "' AND c.relname IN ('vendor_mixed','interval37','float_bits') AND a.attnum>0 "
          "AND NOT a.attisdropped ORDER BY c.relname,a.attnum LIMIT 64";
    } else {
      out.sql =
          "SELECT table_schema AS schema_name,table_name AS "
          "relation_name,ordinal_position AS "
          "ordinal,column_name,data_type,interval_type,interval_precision,datetime_"
          "precision FROM svv_columns WHERE table_schema='" +
          std::string(schema) +
          "' AND table_name IN ('vendor_mixed','interval37','float_bits') ORDER BY "
          "table_name,ordinal_position LIMIT 64";
    }
    return out;
  } catch (...) {
    return std::nullopt;
  }
}
const char* CaptureStatusName(CaptureStatus value) noexcept {
  switch (value) {
    case CaptureStatus::kComplete:
      return "complete";
    case CaptureStatus::kInvalidOutput:
      return "invalid_output";
    case CaptureStatus::kInvalidProbe:
      return "invalid_probe";
    case CaptureStatus::kInvalidLimits:
      return "invalid_limits";
    case CaptureStatus::kNullResult:
      return "null_result";
    case CaptureStatus::kResultStatus:
      return "result_status";
    case CaptureStatus::kFieldLimit:
      return "field_limit";
    case CaptureStatus::kRowLimit:
      return "row_limit";
    case CaptureStatus::kParameterLimit:
      return "parameter_limit";
    case CaptureStatus::kParameterCount:
      return "parameter_count";
    case CaptureStatus::kNameLimit:
      return "name_limit";
    case CaptureStatus::kShapeMismatch:
      return "shape_mismatch";
    case CaptureStatus::kFormatMismatch:
      return "format_mismatch";
    case CaptureStatus::kCellLimit:
      return "cell_limit";
    case CaptureStatus::kBodyLimit:
      return "body_limit";
    case CaptureStatus::kEncodingLimit:
      return "encoding_limit";
    case CaptureStatus::kAllocationFailure:
      return "allocation_failure";
    case CaptureStatus::kInvalidResult:
      return "invalid_result";
  }
  return "invalid_status";
}
CaptureStatus CaptureResult(ProbeId id, const PGresult* result, Observation* out,
                            CaptureLimits limits) noexcept {
  if (!out) return CaptureStatus::kInvalidOutput;
  *out = {};
  const auto* def = Define(id);
  if (!def) return CaptureStatus::kInvalidProbe;
  if (!ValidLimits(limits)) return CaptureStatus::kInvalidLimits;
  try {
    Observation value;
    auto finish = [&](CaptureStatus status) {
      return Finish(*def, status, std::move(value), limits, out);
    };
    if (!result) return finish(CaptureStatus::kNullResult);
    value.result_status = PQresultStatus(result);
    value.row_count = PQntuples(result);
    value.field_count = PQnfields(result);
    const char* sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
    if (sqlstate) {
      bool valid = true;
      size_t length = 0;
      while (length < 6 && sqlstate[length]) {
        const auto c = sqlstate[length++];
        valid &= (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
      }
      if (valid && length == 5) value.sqlstate = std::string(sqlstate, 5);
    }
    if (*value.result_status != (def->format < 0 ? PGRES_COMMAND_OK : PGRES_TUPLES_OK))
      return finish(CaptureStatus::kResultStatus);
    if (*value.field_count < 0 || *value.row_count < 0)
      return finish(CaptureStatus::kInvalidResult);
    if (*value.field_count > limits.fields) return finish(CaptureStatus::kFieldLimit);
    const int64_t max_rows =
        def->format < 0
            ? 0
            : std::min<int64_t>(def->group < 0 && id != ProbeId::kPlumbingError ? 63 : 8,
                                limits.rows);
    if (*value.row_count > max_rows) return finish(CaptureStatus::kRowLimit);
    if (def->format < 0) {
      const int n = PQnparams(result);
      if (n < 0) return finish(CaptureStatus::kInvalidResult);
      if (n > limits.parameters) return finish(CaptureStatus::kParameterLimit);
      value.parameters.emplace();
      value.parameters->reserve(n);
      for (int i = 0; i < n; ++i) value.parameters->push_back(PQparamtype(result, i));
      const bool parameter = std::string_view(def->form) == "parameter_describe";
      if (n != (parameter ? 1 : 0)) return finish(CaptureStatus::kParameterCount);
    }
    const auto expected = ExpectedColumns(id, *def);
    bool shape_bad = static_cast<size_t>(*value.field_count) != expected.size(),
         format_bad = false;
    value.fields.reserve(*value.field_count);
    for (int col = 0; col < *value.field_count; ++col) {
      const char* name = PQfname(result, col);
      if (!name) return finish(CaptureStatus::kInvalidResult);
      int64_t length = 0;
      while (length <= limits.name_bytes && name[length]) ++length;
      if (length > limits.name_bytes) {
        value.failure_field = col;
        return finish(CaptureStatus::kNameLimit);
      }
      CapturedField field;
      field.name.assign(name, static_cast<size_t>(length));
      field.oid = PQftype(result, col);
      field.modifier = PQfmod(result, col);
      field.format = PQfformat(result, col);
      field.table_oid = PQftable(result, col);
      field.table_column = PQftablecol(result, col);
      shape_bad |=
          static_cast<size_t>(col) >= expected.size() || field.name != expected[col];
      format_bad |= (field.format != 0 && field.format != 1) ||
                    (def->format >= 0 && field.format != def->format);
      value.fields.push_back(std::move(field));
    }
    if (shape_bad) return finish(CaptureStatus::kShapeMismatch);
    uint64_t aggregate = 0;
    value.rows.resize(*value.row_count);
    for (int row = 0; row < *value.row_count; ++row) {
      value.rows[row].resize(*value.field_count);
      for (int col = 0; col < *value.field_count; ++col) {
        auto& cell = value.rows[row][col];
        cell.is_null = PQgetisnull(result, row, col) != 0;
        cell.length = PQgetlength(result, row, col);
        if (cell.length < 0 || (cell.is_null && cell.length != 0))
          return finish(CaptureStatus::kInvalidResult);
        value.failure_row = row;
        value.failure_field = col;
        value.failure_length = cell.length;
        if (cell.length > limits.cell_bytes) return finish(CaptureStatus::kCellLimit);
        if (!detail::CheckedAdd(aggregate, cell.length, limits.body_bytes, &aggregate))
          return finish(CaptureStatus::kBodyLimit);
      }
    }
    value.failure_row.reset();
    value.failure_field.reset();
    value.failure_length.reset();
    // Copy no bodies until every declared cell and aggregate has passed bounds.
    for (int row = 0; row < *value.row_count; ++row)
      for (int col = 0; col < *value.field_count; ++col) {
        auto& cell = value.rows[row][col];
        if (!cell.is_null)
          cell.bytes.assign(PQgetvalue(result, row, col),
                            static_cast<size_t>(cell.length));
      }
    return finish(format_bad ? CaptureStatus::kFormatMismatch : CaptureStatus::kComplete);
  } catch (...) {
    *out = {};
    return CaptureStatus::kAllocationFailure;
  }
}
}  // namespace adbc::qualification
