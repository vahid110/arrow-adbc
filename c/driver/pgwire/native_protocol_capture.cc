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
#include "native_protocol_plan.h"

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
bool ParseAffectedRows(std::string_view text, int64_t* out) noexcept {
  if (!out || text.empty() || text.size() > 19 || (text.size() > 1 && text[0] == '0'))
    return false;
  for (char c : text)
    if (c < '0' || c > '9') return false;
  int64_t value;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
  *out = value;
  return true;
}
}  // namespace detail
namespace {
struct Definition {
  const char* name;
  int group;
  const char* form;
  int format;
  bool command = false;
};
struct NativeDefinition {
  uint16_t name, sql, columns;
  int operation, connection, policy, format, rows, calls, group;
  uint16_t dep_start, dep_count, affected_start, affected_count;
  int64_t affected_rows;
  int source, source_row, source_field, origin, input_format, witness, legacy;
  bool show;
  uint16_t context;
};
#include "native_protocol_plan_data.inc"
const NativeDefinition* DefineNative(NativeStepId id) {
  const auto index = static_cast<size_t>(id);
  return index < std::size(native_definitions) ? &native_definitions[index] : nullptr;
}
std::vector<std::string> NativeColumns(const NativeDefinition& def) {
  std::string_view text(native_strings[def.columns]);
  std::vector<std::string> out;
  while (!text.empty()) {
    const auto end = text.find('|');
    out.emplace_back(text.substr(0, end));
    if (end == std::string_view::npos) break;
    text.remove_prefix(end + 1);
  }
  return out;
}
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
      !text("\",\"operation\":\"") ||
      !text(def.command      ? "command"
            : def.format < 0 ? "describe"
                             : "rows") ||
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
  if (def.command &&
      (!text(",\"affected_rows\":") || !e->MaybeNumber(value.affected_rows)))
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
  if (!value.complete && status != CaptureStatus::kFormatMismatch &&
      status != CaptureStatus::kContextMismatch)
    value.rows.clear();
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
std::optional<NativeStepId> FindNativeStep(std::string_view name) noexcept {
  if (name.size() > 127) return std::nullopt;
  for (size_t i = 0; i < std::size(native_definitions); ++i)
    if (name == native_strings[native_definitions[i].name])
      return static_cast<NativeStepId>(i);
  return std::nullopt;
}
std::optional<NativeStepPlan> BuildNativeStep(NativeStepId id,
                                              std::string_view schema) noexcept {
  const auto* d = DefineNative(id);
  if (!d || !ValidOwnedSchemaSyntax(schema)) return std::nullopt;
  try {
    NativeStepPlan p{};
    p.id = id;
    p.name = native_strings[d->name];
    std::string_view sql(native_strings[d->sql]);
    while (!sql.empty()) {
      const auto pos = sql.find("@SCHEMA@");
      p.sql.append(sql.substr(0, pos));
      if (pos == std::string_view::npos) break;
      p.sql.append(schema);
      sql.remove_prefix(pos + 8);
    }
    p.operation = static_cast<NativeOperation>(d->operation);
    p.connection = static_cast<NativeConnection>(d->connection);
    p.definitive_error = static_cast<NativeErrorPolicy>(d->policy);
    p.columns = NativeColumns(*d);
    p.exact_column_names = !d->show;
    p.max_rows = d->rows;
    p.protocol_calls = d->calls;
    p.output_group = d->group;
    if (d->format >= 0) p.requested_format = d->format;
    if (d->operation == 2) p.expected_parameters = 0;
    if (d->legacy >= 0) {
      p.legacy_probe = static_cast<ProbeId>(d->legacy);
      const auto old = BuildProbe(*p.legacy_probe, schema);
      if (!old) return std::nullopt;
      p.columns = old->columns;
      p.expected_parameters = old->expected_parameters;
      p.query_parameters = old->expected_parameters.value_or(0);
    }
    if (d->affected_rows >= 0) p.affected_rows = d->affected_rows;
    for (int i = 0; i < d->dep_count; ++i)
      p.prerequisites.push_back(
          static_cast<NativeStepId>(native_edges[d->dep_start + i]));
    for (int i = 0; i < d->affected_count; ++i)
      p.affected.push_back(
          static_cast<NativeStepId>(native_edges[d->affected_start + i]));
    if (d->source >= 0) {
      p.query_parameters = 1;
      NativeParameterInput input{static_cast<NativeStepId>(d->source),
                                 static_cast<size_t>(d->source_row),
                                 static_cast<size_t>(d->source_field),
                                 static_cast<ParameterOrigin>(d->origin),
                                 d->input_format,
                                 std::nullopt};
      if (d->witness >= 0) input.witness = static_cast<NativeStepId>(d->witness);
      p.parameter = input;
    }
    p.required_context_value = native_strings[d->context];
    return p;
  } catch (...) {
    return std::nullopt;
  }
}
ParameterStatus RawParameter::Prepare(const Observation& source, size_t row,
                                      size_t field) noexcept {
  return PrepareImpl(source, row, field, ParameterOrigin::kObserved, std::nullopt);
}
ParameterStatus RawParameter::PrepareFixture(const Observation& source, size_t row,
                                             size_t field, ParameterOrigin origin,
                                             int requested_format) noexcept {
  return PrepareImpl(source, row, field, origin, requested_format);
}
ParameterStatus RawParameter::PrepareImpl(const Observation& source, size_t row,
                                          size_t field, ParameterOrigin origin,
                                          std::optional<int> fixture_format) noexcept {
  // Invalidate access before even validation/allocation. Old views have expired.
  ready_ = false;
  bytes_.clear();
  type_ = 0;
  format_ = length_ = 0;
  null_ = true;
  origin_ = ParameterOrigin::kObserved;
  if (fixture_format) {
    if (origin != ParameterOrigin::kFixtureNull &&
        origin != ParameterOrigin::kFixtureEmpty)
      return ParameterStatus::kInvalidOrigin;
    if (*fixture_format != 0 && *fixture_format != 1)
      return ParameterStatus::kInvalidFormat;
  } else if (origin != ParameterOrigin::kObserved) {
    return ParameterStatus::kInvalidOrigin;
  }
  const CaptureLimits hard;
  if (!source.present || !source.complete || source.result_status != PGRES_TUPLES_OK ||
      !source.row_count || !source.field_count || *source.row_count < 0 ||
      *source.row_count > 63 || *source.field_count <= 0 ||
      *source.field_count > hard.fields ||
      source.rows.size() != static_cast<size_t>(*source.row_count) ||
      source.fields.size() != static_cast<size_t>(*source.field_count) ||
      source.parameters || source.affected_rows || source.sqlstate ||
      source.failure_row || source.failure_field || source.failure_length ||
      source.json.empty() ||
      source.json.size() > static_cast<uint64_t>(hard.encoded_bytes))
    return ParameterStatus::kInvalidObservation;
  for (const auto& descriptor : source.fields) {
    if (!descriptor.oid || (descriptor.format != 0 && descriptor.format != 1) ||
        descriptor.name.empty() ||
        descriptor.name.size() > static_cast<uint64_t>(hard.name_bytes))
      return ParameterStatus::kInvalidObservation;
  }
  uint64_t body = 0;
  for (const auto& cells : source.rows) {
    if (cells.size() != source.fields.size()) return ParameterStatus::kInvalidObservation;
    for (size_t i = 0; i < cells.size(); ++i) {
      const auto& cell = cells[i];
      if (cell.length < 0 || cell.length > hard.cell_bytes ||
          cell.bytes.size() != static_cast<uint64_t>(cell.length) ||
          (cell.is_null && (cell.length != 0 || !cell.bytes.empty())) ||
          !detail::CheckedAdd(body, cell.bytes.size(), hard.body_bytes, &body))
        return ParameterStatus::kInvalidObservation;
      if (source.fields[i].format == 0 && cell.bytes.find('\0') != std::string::npos)
        return ParameterStatus::kInvalidText;
    }
  }
  if (row >= source.rows.size() || field >= source.fields.size())
    return ParameterStatus::kInvalidSelection;
  const auto& cell = source.rows[row][field];
  try {
    // Stage ownership before publishing metadata; std::string owns its final NUL.
    std::string staged;
    if (origin == ParameterOrigin::kObserved && !cell.is_null) staged = cell.bytes;
    bytes_.swap(staged);
    type_ = source.fields[field].oid;
    format_ = fixture_format.value_or(source.fields[field].format);
    null_ = origin == ParameterOrigin::kFixtureNull ||
            (origin == ParameterOrigin::kObserved && cell.is_null);
    length_ = static_cast<int>(bytes_.size());  // Validated at most 65536 above.
    origin_ = origin;
    ready_ = true;
    return ParameterStatus::kReady;
  } catch (...) {
    return ParameterStatus::kAllocationFailure;
  }
}
std::optional<RawParameterView> RawParameter::Get() const noexcept {
  if (!ready_) return std::nullopt;
  return RawParameterView{type_, format_, length_, null_ ? nullptr : bytes_.c_str(),
                          origin_};
}
ParameterStatus PrepareNativeInput(NativeStepId parameter_step, NativeStepId source_step,
                                   const Observation& source,
                                   RawParameter* out) noexcept {
  if (!out) return ParameterStatus::kInvalidSelection;
  // Expire old access even if an ID/shape fails before G4 preparation.
  out->Prepare(Observation{}, 0, 0);
  const auto* parameter = DefineNative(parameter_step);
  const auto* baseline = DefineNative(source_step);
  if (!parameter || !baseline || parameter->source != static_cast<int>(source_step))
    return ParameterStatus::kInvalidSelection;
  try {
    const auto columns = NativeColumns(*baseline);
    const int expected_rows =
        std::string_view(native_strings[baseline->name]).starts_with("super_cases.") ? 8
                                                                                     : 2;
    if (!source.present || !source.complete || source.result_status != PGRES_TUPLES_OK ||
        source.row_count != expected_rows ||
        source.rows.size() != static_cast<size_t>(expected_rows) ||
        source.field_count != static_cast<int64_t>(columns.size()) ||
        source.fields.size() != columns.size() || parameter->source_row < 0 ||
        parameter->source_row >= expected_rows || parameter->source_field < 0 ||
        static_cast<size_t>(parameter->source_field) >= columns.size())
      return ParameterStatus::kInvalidObservation;
    for (size_t i = 0; i < columns.size(); ++i)
      if (source.fields[i].name != columns[i] ||
          source.fields[i].format != baseline->format || !source.fields[i].oid)
        return ParameterStatus::kInvalidObservation;
    for (const auto& row : source.rows)
      if (row.size() != columns.size()) return ParameterStatus::kInvalidObservation;
    const auto row = static_cast<size_t>(parameter->source_row);
    const auto field = static_cast<size_t>(parameter->source_field);
    const auto& case_id = source.rows[row][0];
    const bool super =
        std::string_view(native_strings[baseline->name]).starts_with("super_cases.");
    const int first_case = super && std::string_view(native_strings[baseline->name])
                                        .starts_with("super_cases.9.")
                               ? 9
                               : 1;
    const int expected_case = first_case + parameter->source_row;
    char literal[8]{};
    std::string_view wanted;
    if (baseline->format == 0) {
      const auto result =
          std::to_chars(literal, literal + sizeof(literal), expected_case);
      wanted = std::string_view(literal, result.ptr - literal);
    } else {
      literal[7] = static_cast<char>(expected_case);
      wanted = std::string_view(literal, sizeof(literal));
    }
    // Closed reference BIGINT case literals, not a vendor value decoder or OID guess.
    if (case_id.is_null || case_id.length != static_cast<int64_t>(wanted.size()) ||
        case_id.bytes != wanted)
      return ParameterStatus::kInvalidObservation;
    if (parameter->origin == 0) {
      const bool expected_null = super && expected_case == 1;
      if (source.rows[row][field].is_null != expected_null ||
          baseline->format != parameter->input_format)
        return ParameterStatus::kInvalidObservation;
      return out->Prepare(source, row, field);
    }
    return out->PrepareFixture(source, row, field,
                               static_cast<ParameterOrigin>(parameter->origin),
                               parameter->input_format);
  } catch (...) {
    return ParameterStatus::kAllocationFailure;
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
    case CaptureStatus::kAffectedRows:
      return "affected_rows";
    case CaptureStatus::kContextMismatch:
      return "context_mismatch";
  }
  return "invalid_status";
}
namespace {
CaptureStatus CaptureExpected(const Definition* def, const PGresult* result,
                              Observation* out, CaptureLimits limits,
                              const std::vector<std::string>& expected, int64_t row_bound,
                              int parameter_count,
                              const NativeDefinition* native = nullptr) noexcept {
  if (!out) return CaptureStatus::kInvalidOutput;
  *out = {};
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
    if (*value.result_status !=
        (def->command || def->format < 0 ? PGRES_COMMAND_OK : PGRES_TUPLES_OK))
      return finish(CaptureStatus::kResultStatus);
    if (*value.field_count < 0 || *value.row_count < 0)
      return finish(CaptureStatus::kInvalidResult);
    if (*value.field_count > limits.fields) return finish(CaptureStatus::kFieldLimit);
    const int64_t max_rows = std::min<int64_t>(row_bound, limits.rows);
    if (*value.row_count > max_rows) return finish(CaptureStatus::kRowLimit);
    if (def->command) {
      if (*value.field_count != 0 || *value.row_count != 0 || PQnparams(result) != 0)
        return finish(CaptureStatus::kShapeMismatch);
      const char* text = PQcmdTuples(const_cast<PGresult*>(result));
      if (!text) return finish(CaptureStatus::kInvalidResult);
      // No unbounded scan of a claimed count, nor raw tag/error text in output.
      size_t length = 0;
      while (length < 20 && text[length]) ++length;
      if (length == 20) return finish(CaptureStatus::kAffectedRows);
      if (native && native->affected_rows >= 0) {
        int64_t count;
        if (!detail::ParseAffectedRows(std::string_view(text, length), &count) ||
            count != native->affected_rows)
          return finish(CaptureStatus::kAffectedRows);
        value.affected_rows = count;
      } else if (length) {
        return finish(CaptureStatus::kAffectedRows);
      }
      return finish(CaptureStatus::kComplete);
    }
    if (def->format < 0) {
      const int n = PQnparams(result);
      if (n < 0) return finish(CaptureStatus::kInvalidResult);
      if (n > limits.parameters) return finish(CaptureStatus::kParameterLimit);
      value.parameters.emplace();
      value.parameters->reserve(n);
      for (int i = 0; i < n; ++i) value.parameters->push_back(PQparamtype(result, i));
      if (n != parameter_count) return finish(CaptureStatus::kParameterCount);
    }
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
      if (native && native->show && length == 0)
        return finish(CaptureStatus::kInvalidResult);
      CapturedField field;
      field.name.assign(name, static_cast<size_t>(length));
      field.oid = PQftype(result, col);
      field.modifier = PQfmod(result, col);
      field.format = PQfformat(result, col);
      field.table_oid = PQftable(result, col);
      field.table_column = PQftablecol(result, col);
      shape_bad |= static_cast<size_t>(col) >= expected.size() ||
                   (!(native && native->show) && field.name != expected[col]);
      if (native && native->legacy < 0 && field.oid == 0)
        return finish(CaptureStatus::kInvalidResult);
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
    if (native && native->show) {
      if (*value.row_count != 1 || value.rows[0][0].is_null)
        return finish(CaptureStatus::kContextMismatch);
      const std::string_view wanted(native_strings[native->context]);
      if (!wanted.empty() && value.rows[0][0].bytes != wanted)
        return finish(CaptureStatus::kContextMismatch);
    }
    return finish(format_bad ? CaptureStatus::kFormatMismatch : CaptureStatus::kComplete);
  } catch (...) {
    *out = {};
    return CaptureStatus::kAllocationFailure;
  }
}
}  // namespace
CaptureStatus CaptureResult(ProbeId id, const PGresult* result, Observation* out,
                            CaptureLimits limits) noexcept {
  if (!out) return CaptureStatus::kInvalidOutput;
  *out = {};
  const auto* def = Define(id);
  if (!def) return CaptureStatus::kInvalidProbe;
  if (!ValidLimits(limits)) return CaptureStatus::kInvalidLimits;
  try {
    const auto expected = ExpectedColumns(id, *def);
    const int64_t rows = def->format < 0                                   ? 0
                         : def->group < 0 && id != ProbeId::kPlumbingError ? 63
                                                                           : 8;
    return CaptureExpected(def, result, out, limits, expected, rows,
                           std::string_view(def->form) == "parameter_describe" ? 1 : 0);
  } catch (...) {
    *out = {};
    return CaptureStatus::kAllocationFailure;
  }
}
CaptureStatus CaptureNativeResult(NativeStepId id, const PGresult* result,
                                  Observation* out, CaptureLimits limits) noexcept {
  if (!out) return CaptureStatus::kInvalidOutput;
  *out = {};
  const auto* native = DefineNative(id);
  if (!native) return CaptureStatus::kInvalidProbe;
  if (!ValidLimits(limits)) return CaptureStatus::kInvalidLimits;
  if (native->legacy >= 0) {
    // The old API retains its caps; this selected sequence can only tighten them.
    limits.rows = std::min<int64_t>(limits.rows, native->rows);
    return CaptureResult(static_cast<ProbeId>(native->legacy), result, out, limits);
  }
  try {
    Definition def{native_strings[native->name], -1, "", native->format,
                   native->operation == 0};
    return CaptureExpected(&def, result, out, limits, NativeColumns(*native),
                           native->rows, 0, native);
  } catch (...) {
    *out = {};
    return CaptureStatus::kAllocationFailure;
  }
}
}  // namespace adbc::qualification
