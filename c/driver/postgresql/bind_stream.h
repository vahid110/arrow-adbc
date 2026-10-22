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
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <arrow-adbc/adbc.h>

#include "copy/writer.h"
#include "driver/pgwire/libpq_raii.h"
#include "error.h"
#include "postgres_type.h"
#include "postgres_util.h"
#include "redshift_vendor.h"
#include "result_helper.h"

namespace adbcpq {

/// The flag indicating to PostgreSQL that we want binary-format values.
constexpr int kPgBinaryFormat = 1;

// table and fields must be SQL identifiers escaped by CreateBulkTable().
inline std::string BuildParameterizedInsertQuery(
    const std::string& table, const std::string& fields, size_t columns, size_t rows,
    const std::vector<bool>& json_parse = {},
    const std::vector<RedshiftEncodedKind>& encoded = {}) {
  std::string query = "INSERT INTO " + table + " (" + fields + ") VALUES ";
  for (size_t row = 0; row < rows; row++) {
    if (row > 0) query += ", ";
    query += "(";
    for (size_t col = 0; col < columns; col++) {
      if (col > 0) query += ", ";
      const bool super = col < json_parse.size() && json_parse[col];
      const auto kind = col < encoded.size() ? encoded[col] : RedshiftEncodedKind::kNone;
      const bool spatial = kind == RedshiftEncodedKind::kGeometry ||
                           kind == RedshiftEncodedKind::kGeography;
      if (super) query += "JSON_PARSE(";
      if (spatial) query += "ST_GeomFromEWKB(";
      query += "$" + std::to_string(row * columns + col + 1);
      if (super || spatial) query += ")";
      if (kind == RedshiftEncodedKind::kGeography) query += "::GEOGRAPHY";
    }
    query += ")";
  }
  return query;
}

/// Helper to manage bind parameters with a prepared statement
struct BindStream {
  Handle<struct ArrowArrayStream> bind;
  Handle<struct ArrowArrayView> array_view;
  Handle<struct ArrowArray> current;
  Handle<struct ArrowSchema> bind_schema;
  int64_t current_row = -1;

  std::vector<struct ArrowSchemaView> bind_schema_fields;
  std::vector<std::unique_ptr<PostgresCopyFieldWriter>> bind_field_writers;

  // OIDs for parameter types
  std::vector<uint32_t> param_types;
  std::vector<char*> param_values;
  std::vector<int> param_formats;
  std::vector<int> param_lengths;
  Handle<struct ArrowBuffer> param_buffer;

  bool has_tz_field = false;
  bool autocommit = false;
  // Timezone setup can fail before has_tz_field becomes true. Preserve a
  // failed rollback so the owning connection is not reused afterward.
  bool cleanup_failed = false;
  std::string tz_setting;

  // Expected types from PostgreSQL (after DESCRIBE); used to resolve NA params
  PostgresType expected_param_types;
  std::vector<bool> timetz_fields;
  bool has_expected_types = false;
  bool generated_super_ingest = false;
  std::vector<bool> json_parse_fields;
  std::vector<bool> bytes_fields;
  bool has_super_fields = false;
  bool has_encoded_fields = false;
  std::vector<RedshiftEncodedKind> encoded_fields;

  struct ArrowError na_error;

  BindStream() {
    this->bind->release = nullptr;
    std::memset(&na_error, 0, sizeof(na_error));
  }

  void SetBind(struct ArrowArrayStream* stream) {
    this->bind.reset();
    ArrowArrayStreamMove(stream, &bind.value);
  }

  Status ReconcileWithExpectedTypes(const PostgresType& expected_types) {
    if (bind_schema->release == nullptr) {
      return Status::InvalidState("[libpq] Bind stream schema not initialized");
    }
    if (expected_types.n_children() != bind_schema->n_children) {
      return Status::InvalidState("[libpq] Expected ", expected_types.n_children(),
                                  " parameters but bind stream has ",
                                  bind_schema->n_children);
    }
    expected_param_types = expected_types;
    has_expected_types = true;
    return Status::Ok();
  }

  template <typename Callback>
  Status Begin(Callback&& callback) {
    UNWRAP_NANOARROW(
        na_error, Internal,
        ArrowArrayStreamGetSchema(&bind.value, &bind_schema.value, &na_error));

    struct ArrowSchemaView bind_schema_view;
    UNWRAP_NANOARROW(
        na_error, Internal,
        ArrowSchemaViewInit(&bind_schema_view, &bind_schema.value, &na_error));
    if (bind_schema_view.type != ArrowType::NANOARROW_TYPE_STRUCT) {
      return Status::InvalidState("[libpq] Bind parameters must have type STRUCT");
    }

    bind_schema_fields.resize(bind_schema->n_children);
    for (size_t i = 0; i < bind_schema_fields.size(); i++) {
      UNWRAP_ERRNO(Internal,
                   ArrowSchemaViewInit(&bind_schema_fields[i], bind_schema->children[i],
                                       /*error*/ nullptr));
    }

    UNWRAP_NANOARROW(
        na_error, Internal,
        ArrowArrayViewInitFromSchema(&array_view.value, &bind_schema.value, &na_error));

    ArrowBufferInit(&param_buffer.value);

    return std::move(callback)();
  }

  bool validate_redshift_bytes = false;
  bool require_utf8 = false;

  Status CheckTextEncoding(PGconn* conn, const PostgresTypeResolver& resolver) {
    require_utf8 = resolver.is_redshift();
    validate_redshift_bytes = resolver.is_redshift();
    for (int64_t i = 0; i < bind_schema->n_children; ++i) {
      RedshiftBytesSchema bytes;
      UNWRAP_NANOARROW(
          na_error, InvalidArgument,
          RedshiftBytesSchema::Parse(bind_schema->children[i], resolver.is_redshift(),
                                     &bytes, &na_error));
      require_utf8 |= bytes.enabled() && bytes.kind != RedshiftBytesKind::kVarbyte;
    }
    return require_utf8 ? CheckUtf8Encoding(conn) : Status::Ok();
  }

  Status SetParamTypes(PGconn* pg_conn, const PostgresTypeResolver& type_resolver,
                       const bool autocommit,
                       const PostgresType* ingest_targets = nullptr) {
    validate_redshift_bytes = type_resolver.is_redshift();
    if (PostgresType::ValidateTemporalWriteSchema(type_resolver, &bind_schema.value,
                                                  &na_error) != NANOARROW_OK)
      return Status::InvalidArgument(na_error.message);
    require_utf8 = type_resolver.is_redshift();
    if (PostgresType::ValidateIntervalWriteSchema(type_resolver, &bind_schema.value,
                                                  &na_error) != NANOARROW_OK) {
      return Status::NotImplemented(na_error.message);
    }
    if (PostgresType::ValidateDecimalWriteSchema(type_resolver, &bind_schema.value,
                                                 &na_error) != NANOARROW_OK) {
      return Status::InvalidArgument(na_error.message);
    }
    if (PostgresType::ValidateScalarWriteSchema(type_resolver, &bind_schema.value,
                                                &na_error) != NANOARROW_OK) {
      return Status::InvalidArgument(na_error.message);
    }
    if (PostgresType::ValidateBytesWriteSchema(type_resolver, &bind_schema.value,
                                               &na_error) != NANOARROW_OK)
      return Status::InvalidArgument(na_error.message);
    if (ingest_targets && ingest_targets->n_children() != bind_schema->n_children) {
      return Status::InvalidArgument("Ingest target/source column count mismatch");
    }
    param_types.resize(bind_schema->n_children);
    param_values.resize(bind_schema->n_children);
    param_lengths.resize(bind_schema->n_children);
    param_formats.resize(bind_schema->n_children, kPgBinaryFormat);
    bind_field_writers.resize(bind_schema->n_children);
    timetz_fields.resize(bind_schema->n_children, false);
    json_parse_fields.assign(bind_schema->n_children, false);
    bytes_fields.assign(bind_schema->n_children, false);
    has_super_fields = false;
    has_encoded_fields = false;
    encoded_fields.assign(bind_schema->n_children, RedshiftEncodedKind::kNone);
    bool needs_utc_timezone = false;

    for (size_t i = 0; i < bind_field_writers.size(); i++) {
      PostgresType type;

      // Genuine NA ingest uses the actual interval OID; the writer factory must
      // still validate its known constraint before selecting the null writer.
      if (ingest_targets && type_resolver.is_redshift() &&
          bind_schema_fields[i].type == NANOARROW_TYPE_NA &&
          (ingest_targets->child(i).interval_family() != RedshiftIntervalFamily::kNone ||
           ingest_targets->child(i).type_id() == PostgresTypeId::kTimetz)) {
        type = ingest_targets->child(i);
      } else if (has_expected_types && bind_schema_fields[i].type == NANOARROW_TYPE_NA &&
                 i < static_cast<size_t>(expected_param_types.n_children())) {
        const auto& expected_type = expected_param_types.child(i);
        // If PostgreSQL couldn't infer a concrete type (e.g., SELECT $1), don't
        // force an "expected" type; fall back to Arrow-derived mapping.
        if (expected_type.oid() != 0 &&
            expected_type.type_id() != PostgresTypeId::kUnknown) {
          type = expected_type;
        } else {
          UNWRAP_NANOARROW(
              na_error, Internal,
              PostgresType::FromSchema(type_resolver, bind_schema->children[i], &type,
                                       &na_error));
        }
      } else {
        // Normal case: derive type from Arrow schema
        UNWRAP_NANOARROW(na_error, Internal,
                         PostgresType::FromSchema(type_resolver, bind_schema->children[i],
                                                  &type, &na_error));
      }

      if (!generated_super_ingest &&
          type.vendor_family() != adbc::driver::pgwire::CatalogVendorFamily::kNone &&
          type.encoded_kind() != RedshiftEncodedKind::kHll)
        return Status::NotImplemented(
            "[Redshift] Direct vendor parameter binding is unsupported");
      // Delay the session change until all parameter types and writers are valid.
      // Otherwise a later unsupported field can leave an open transaction and UTC.
      needs_utc_timezone |= type.type_id() == PostgresTypeId::kTimestamptz;

      RedshiftSuperSchema super;
      UNWRAP_NANOARROW(
          na_error, InvalidArgument,
          RedshiftSuperSchema::Parse(bind_schema->children[i],
                                     type_resolver.is_redshift(), &super, &na_error));
      RedshiftEncodedSchema encoded;
      UNWRAP_NANOARROW(
          na_error, InvalidArgument,
          RedshiftEncodedSchema::Parse(bind_schema->children[i],
                                       type_resolver.is_redshift(), &encoded, &na_error));
      encoded_fields[i] = encoded.kind;
      has_encoded_fields |= encoded.enabled();
      if (type.encoded_kind() != RedshiftEncodedKind::kNone &&
          type.encoded_kind() != encoded.kind)
        return Status::InvalidArgument(
            "[Redshift] Known vendor parameter requires exact marked source, including "
            "NULL");
      json_parse_fields[i] = super.enabled;
      has_super_fields |= super.enabled;

      // Exact semantic writers select destination OID and wire layout together.
      // Other parameters retain their existing source-derived wire type.
      RedshiftScalarSchema scalar;
      UNWRAP_NANOARROW(
          na_error, InvalidArgument,
          RedshiftScalarSchema::Parse(bind_schema->children[i],
                                      type_resolver.is_redshift(), &scalar, &na_error));
      RedshiftBytesSchema bytes;
      UNWRAP_NANOARROW(
          na_error, InvalidArgument,
          RedshiftBytesSchema::Parse(bind_schema->children[i],
                                     type_resolver.is_redshift(), &bytes, &na_error));
      require_utf8 |= bytes.enabled() && bytes.kind != RedshiftBytesKind::kVarbyte;
      RedshiftTemporalSchema temporal;
      UNWRAP_NANOARROW(na_error, InvalidArgument,
                       RedshiftTemporalSchema::Parse(bind_schema->children[i],
                                                     type_resolver.is_redshift(),
                                                     &temporal, &na_error));
      RedshiftIntervalSchema interval;
      UNWRAP_NANOARROW(na_error, InvalidArgument,
                       RedshiftIntervalSchema::Parse(bind_schema->children[i],
                                                     type_resolver.is_redshift(),
                                                     &interval, &na_error));
      RedshiftTimetzSchema timetz;
      UNWRAP_NANOARROW(
          na_error, InvalidArgument,
          RedshiftTimetzSchema::Parse(bind_schema->children[i],
                                      type_resolver.is_redshift(), &timetz, &na_error));
      timetz_fields[i] = timetz.enabled;
      const PostgresType* writer_target = nullptr;
      if (ingest_targets &&
          (encoded.enabled() || super.enabled ||
           ingest_targets->child(i).vendor_family() !=
               adbc::driver::pgwire::CatalogVendorFamily::kNone ||
           ((timetz.enabled || type_resolver.is_redshift()) &&
            (type.type_id() == PostgresTypeId::kTimetz ||
             ingest_targets->child(i).type_id() == PostgresTypeId::kTimetz)) ||
           ((interval.enabled() || type_resolver.is_redshift()) &&
            (type.interval_family() != RedshiftIntervalFamily::kNone ||
             ingest_targets->child(i).interval_family() !=
                 RedshiftIntervalFamily::kNone)) ||
           type.type_id() == PostgresTypeId::kNumeric ||
           ingest_targets->child(i).type_id() == PostgresTypeId::kNumeric ||
           ((temporal.enabled() || type_resolver.is_redshift()) &&
            (type.temporal_kind() != RedshiftTemporalKind::kNone ||
             ingest_targets->child(i).temporal_kind() != RedshiftTemporalKind::kNone)) ||
           ((bytes.enabled() || type_resolver.is_redshift()) &&
            (type.bytes_kind() != RedshiftBytesKind::kNone ||
             ingest_targets->child(i).bytes_kind() != RedshiftBytesKind::kNone)) ||
           ((scalar.enabled() || type_resolver.is_redshift()) &&
            (type.scalar_kind() != RedshiftScalarKind::kNone ||
             ingest_targets->child(i).scalar_kind() != RedshiftScalarKind::kNone)))) {
        writer_target = &ingest_targets->child(i);
      }
      if (!ingest_targets && encoded.kind == RedshiftEncodedKind::kHll)
        writer_target = &type;
      bytes_fields[i] =
          bytes.enabled() || (type_resolver.is_redshift() && writer_target &&
                              writer_target->bytes_kind() != RedshiftBytesKind::kNone);
      std::unique_ptr<PostgresCopyFieldWriter> writer;
      const int writer_code = MakeCopyFieldWriter(
          bind_schema->children[i], array_view->children[i], type_resolver, writer_target,
          &writer, /*disable_decimal_fast_path=*/false, &na_error,
          generated_super_ingest);
      // Byte-family compatibility is checked before a writer can be constructed.
      if (writer_code == EINVAL && bytes_fields[i])
        return Status::InvalidArgument(na_error.message);
      UNWRAP_NANOARROW(na_error, Internal, writer_code);

      param_types[i] =
          writer_target &&
                  (writer_target->type_id() == PostgresTypeId::kTimetz ||
                   writer_target->interval_family() != RedshiftIntervalFamily::kNone ||
                   writer_target->type_id() == PostgresTypeId::kNumeric ||
                   writer_target->scalar_kind() != RedshiftScalarKind::kNone ||
                   writer_target->bytes_kind() != RedshiftBytesKind::kNone ||
                   writer_target->temporal_kind() != RedshiftTemporalKind::kNone)
              ? writer_target->oid()
              : type.oid();
      if (super.enabled || encoded.spatial()) {
        param_types[i] = type_resolver.GetBytesOID(RedshiftBytesKind::kVarbyte);
        if (!param_types[i])
          return Status::InvalidState("[Redshift] VARBYTE OID unavailable");
      }
      param_formats[i] = encoded.kind == RedshiftEncodedKind::kHll ? 0 : kPgBinaryFormat;
      bind_field_writers[i] = std::move(writer);
    }

    if (needs_utc_timezone && !has_tz_field) {
      UNWRAP_STATUS(SetDatabaseTimezoneUTC(pg_conn, autocommit));
      has_tz_field = true;
      this->autocommit = autocommit;
    }

    return Status::Ok();
  }

  bool HasHllParameters() const {
    return std::find(encoded_fields.begin(), encoded_fields.end(),
                     RedshiftEncodedKind::kHll) != encoded_fields.end();
  }
  Status ValidateHllParameters(const PostgresType& described) const {
    if (described.n_children() != static_cast<int>(param_types.size()))
      return Status::InvalidArgument("[Redshift] Parameter descriptor count changed");
    for (size_t i = 0; i < encoded_fields.size(); ++i)
      if (encoded_fields[i] == RedshiftEncodedKind::kHll &&
          (described.child(i).encoded_kind() != RedshiftEncodedKind::kHll ||
           !described.child(i).oid() || described.child(i).oid() != param_types[i]))
        return Status::InvalidArgument(
            "[Redshift] HLL parameter Describe consistency mismatch");
    return Status::Ok();
  }

  Status SetDatabaseTimezoneUTC(PGconn* pg_conn, const bool autocommit) {
    if (autocommit) {
      PqResultHelper helper(pg_conn, "BEGIN");
      UNWRAP_STATUS(helper.Execute());
    }

    // Once BEGIN succeeds, failures during timezone setup must not leave the
    // driver's autocommit-owned transaction open. Keep the original error if
    // rollback also fails; explicit transactions still belong to the caller.
    auto fail_setup = [&](Status status) {
      if (autocommit) {
        PqResultHelper rollback(pg_conn, "ROLLBACK");
        if (!rollback.Execute().ok()) cleanup_failed = true;
      }
      return status;
    };

    PqResultHelper get_tz(pg_conn, "SELECT current_setting('TIMEZONE')");
    Status get_tz_status = get_tz.Execute();
    if (!get_tz_status.ok()) return fail_setup(std::move(get_tz_status));
    for (auto row : get_tz) {
      tz_setting = row[0].value();
    }

    PqResultHelper set_utc(pg_conn, "SET TIME ZONE 'UTC'");
    Status set_utc_status = set_utc.Execute();
    if (!set_utc_status.ok()) return fail_setup(std::move(set_utc_status));

    return Status::Ok();
  }

  Status Prepare(PGconn* pg_conn, const std::string& query) {
    PqResultHelper helper(pg_conn, query);
    UNWRAP_STATUS(helper.Prepare(param_types));
    return CheckHllPrepared(pg_conn, param_types);
  }

  Status CheckHllPrepared(PGconn* conn, const std::vector<Oid>& types) const {
    if (!HasHllParameters()) return Status::Ok();
    if (require_utf8) UNWRAP_STATUS(CheckUtf8Encoding(conn));
    adbc::driver::pgwire::UniqueResult result(PQdescribePrepared(conn, ""));
    if (PQresultStatus(result.get()) != PGRES_COMMAND_OK)
      return MakeStatus(result.get(), "[Redshift] HLL parameter Describe failed");
    if (PQnparams(result.get()) != static_cast<int>(types.size()))
      return Status::InvalidArgument("[Redshift] HLL parameter Describe count mismatch");
    for (size_t i = 0; i < types.size(); ++i)
      if (encoded_fields[i % encoded_fields.size()] == RedshiftEncodedKind::kHll &&
          PQparamtype(result.get(), i) != types[i])
        return Status::InvalidArgument("[Redshift] HLL parameter Describe OID mismatch");
    return require_utf8 ? CheckUtf8Encoding(conn) : Status::Ok();
  }

  Status PullNextArray() {
    if (current->release != nullptr) ArrowArrayRelease(&current.value);

    UNWRAP_NANOARROW(na_error, IO,
                     ArrowArrayStreamGetNext(&bind.value, &current.value, &na_error));

    if (current->release != nullptr) {
      UNWRAP_NANOARROW(
          na_error, Internal,
          ArrowArrayViewSetArray(&array_view.value, &current.value, &na_error));
      for (int64_t i = 0; i < bind_schema->n_children; ++i) {
        RedshiftBytesSchema bytes;
        UNWRAP_NANOARROW(
            na_error, InvalidArgument,
            RedshiftBytesSchema::Parse(bind_schema->children[i], validate_redshift_bytes,
                                       &bytes, &na_error));
        RedshiftSuperSchema super;
        UNWRAP_NANOARROW(
            na_error, InvalidArgument,
            RedshiftSuperSchema::Parse(bind_schema->children[i], validate_redshift_bytes,
                                       &super, &na_error));
        RedshiftEncodedSchema encoded;
        UNWRAP_NANOARROW(
            na_error, InvalidArgument,
            RedshiftEncodedSchema::Parse(bind_schema->children[i],
                                         validate_redshift_bytes, &encoded, &na_error));
        if (bytes.enabled() || super.enabled || encoded.enabled())
          UNWRAP_NANOARROW(
              na_error, InvalidArgument,
              ArrowArrayViewValidate(array_view->children[i],
                                     NANOARROW_VALIDATION_LEVEL_FULL, &na_error));
      }
    }

    return Status::Ok();
  }

  Status EnsureNextRow() {
    if (current->release != nullptr) {
      current_row++;
      if (current_row < current->length) {
        return Status::Ok();
      }
    }

    // Pull until we have an array with at least one row or the stream is finished
    do {
      UNWRAP_STATUS(PullNextArray());
      if (current->release == nullptr) {
        current_row = -1;
        return Status::Ok();
      }
    } while (current->length == 0);

    current_row = 0;
    return Status::Ok();
  }

  Status ParameterRow(size_t column, int64_t row, int64_t* field_row) {
    *field_row = row;
    if ((has_super_fields || has_encoded_fields) &&
        ArrowArrayViewIsNull(&array_view.value, row))
      return Status::InvalidArgument(
          "[Redshift] SUPER ingest requires nonnull relation rows");
    if (timetz_fields[column] || has_super_fields || has_encoded_fields) {
      // Struct child accessors add only their own offsets. Carry the containing
      // relation offset once here; field accessors carry their own offset.
      if (row < 0 || array_view->offset < 0 || array_view->offset > INT64_MAX - row)
        return Status::InvalidArgument("TIMETZ relation offset overflow");
      *field_row = array_view->offset + row;
      if (*field_row >= array_view->children[column]->length)
        return Status::InvalidArgument("TIMETZ relation/field coverage mismatch");
    }
    return Status::Ok();
  }

  Status WriteParameter(size_t column, int64_t field_row) {
    const int code =
        bind_field_writers[column]->Write(&param_buffer.value, field_row, &na_error);
    // These schema-selected writers validate application payloads before writing.
    // Keep allocator/other failures and non-validating writers on the Internal path.
    if (code == EINVAL && (bytes_fields[column] || json_parse_fields[column] ||
                           encoded_fields[column] != RedshiftEncodedKind::kNone)) {
      return Status::InvalidArgument(na_error.message);
    }
    UNWRAP_NANOARROW(na_error, Internal, code);
    return Status::Ok();
  }

  Status BindAndExecuteCurrentRow(PGconn* pg_conn,
                                  adbc::driver::pgwire::UniqueResult* result_out,
                                  int result_format) {
    if (require_utf8) UNWRAP_STATUS(CheckUtf8Encoding(pg_conn));
    param_buffer->size_bytes = 0;
    int64_t last_offset = 0;
    std::vector<bool> is_null_param(array_view->n_children);

    for (int64_t col = 0; col < array_view->n_children; col++) {
      int64_t field_row;
      UNWRAP_STATUS(ParameterRow(col, current_row, &field_row));
      is_null_param[col] = ArrowArrayViewIsNull(array_view->children[col], field_row);

      // Safety check: NA type arrays should only contain nulls
      if (bind_schema_fields[col].type == NANOARROW_TYPE_NA && !is_null_param[col]) {
        return Status::InvalidArgument(
            "Parameter $", col + 1,
            " has null type but contains a non-null value at row ", current_row);
      }

      if (!is_null_param[col]) {
        // Note that this Write() call currently writes the (int32_t) byte size of the
        // field in addition to the serialized value.
        UNWRAP_STATUS(WriteParameter(col, field_row));
      } else {
        UNWRAP_ERRNO(Internal, ArrowBufferAppendInt32(&param_buffer.value, 0));
      }

      int64_t param_length = param_buffer->size_bytes - last_offset - sizeof(int32_t);
      if (param_length > (std::numeric_limits<int>::max)()) {
        return Status::Internal("Parameter ", col, "serialized to >2GB of binary");
      }

      param_lengths[col] = static_cast<int>(param_length);
      last_offset = param_buffer->size_bytes;
    }

    last_offset = 0;
    for (int64_t col = 0; col < array_view->n_children; col++) {
      last_offset += sizeof(int32_t);
      if (is_null_param[col]) {
        param_values[col] = nullptr;
      } else {
        param_values[col] = reinterpret_cast<char*>(param_buffer->data) + last_offset;
      }
      last_offset += param_lengths[col];
    }

    if (require_utf8) UNWRAP_STATUS(CheckUtf8Encoding(pg_conn));
    if (has_super_fields) UNWRAP_STATUS(CheckSuperParseContext(pg_conn));
    adbc::driver::pgwire::UniqueResult result(
        PQexecPrepared(pg_conn, /*stmtName=*/"",
                       /*nParams=*/bind_schema->n_children, param_values.data(),
                       param_lengths.data(), param_formats.data(), result_format));

    ExecStatusType pg_status = PQresultStatus(result.get());
    if (pg_status != PGRES_COMMAND_OK && pg_status != PGRES_TUPLES_OK) {
      return MakeStatus(result.get(),
                        "[libpq] Failed to execute prepared statement: {} {}",
                        PQresStatus(pg_status), PQerrorMessage(pg_conn));
    }

    if (require_utf8) UNWRAP_STATUS(CheckUtf8Encoding(pg_conn));
    *result_out = std::move(result);
    return Status::Ok();
  }

  Status Cleanup(PGconn* pg_conn) {
    if (has_tz_field) {
      PqResultHelper reset(pg_conn, "SET TIME ZONE '" + tz_setting + "'");
      UNWRAP_STATUS(reset.Execute());

      if (autocommit) {
        // SetDatabaseTimezoneUTC will start a transaction if autocommit is
        // enabled (so the timezone setting will roll back if we error), so we
        // need to commit here.  Otherwise we should not commit and let the
        // user commit.
        PqResultHelper commit(pg_conn, "COMMIT");
        UNWRAP_STATUS(commit.Execute());
      }
    }

    return Status::Ok();
  }

  // A failed query preparation may leave PostgreSQL's transaction aborted, so
  // Cleanup() cannot reset the timezone with another SET. Only roll back a
  // transaction that this bind stream opened on behalf of autocommit mode.
  Status RollbackTimezoneTransactionIfOwned(PGconn* pg_conn) {
    if (!has_tz_field || !autocommit) return Status::Ok();
    PqResultHelper rollback(pg_conn, "ROLLBACK");
    return rollback.Execute();
  }

  Status ExecuteCopy(PGconn* pg_conn, const PostgresTypeResolver& type_resolver,
                     const PostgresType* target_types, bool disable_decimal_fast_path,
                     int64_t* rows_affected, bool* copy_cleanup_failed) {
    if (rows_affected) *rows_affected = 0;
    *copy_cleanup_failed = false;
    int64_t copied_rows = 0;

    // This driver uses blocking libpq connections. COPY_IN is already active.
    // Every client-side failure below must end the
    // protocol before another SQL command (including ROLLBACK) can be issued.
    // Keep the primary stream/writer error if COPY abort itself also fails.
    Status execution_status = [&]() -> Status {
      UNWRAP_STATUS(CheckTextEncoding(pg_conn, type_resolver));
      PostgresCopyStreamWriter writer;
      UNWRAP_ERRNO(Internal, writer.Init(&bind_schema.value));
      UNWRAP_NANOARROW(na_error, Internal,
                       writer.InitFieldWriters(type_resolver, target_types,
                                               disable_decimal_fast_path, &na_error));

      UNWRAP_NANOARROW(na_error, Internal, writer.WriteHeader(&na_error));

      while (true) {
        UNWRAP_STATUS(PullNextArray());
        if (!current->release) break;

        if (require_utf8) UNWRAP_STATUS(CheckUtf8Encoding(pg_conn));
        UNWRAP_ERRNO(Internal, writer.SetArray(&current.value));

        // build writer buffer
        int write_result;
        do {
          write_result = writer.WriteRecord(&na_error);
        } while (write_result == NANOARROW_OK);

        // check if not ENODATA at exit
        if (write_result != ENODATA) {
          return Status::IO("Error occurred writing COPY data: ", na_error.message);
        }

        UNWRAP_STATUS(FlushCopyWriterToConn(pg_conn, writer));

        copied_rows += current->length;
        writer.Rewind();
      }

      // If there were no arrays in the stream, we haven't flushed yet
      return FlushCopyWriterToConn(pg_conn, writer);
    }();

    const char* copy_error = execution_status.ok() ? nullptr : "ADBC COPY input failed";
    if (PQputCopyEnd(pg_conn, copy_error) <= 0) {
      // Do not attempt another SQL command while COPY termination is uncertain.
      *copy_cleanup_failed = true;
      if (!execution_status.ok()) return execution_status;
      return Status::IO("Error message returned by PQputCopyEnd: ",
                        PQerrorMessage(pg_conn));
    }

    // Even a failed COPY must be drained to NULL. A fatal result is the expected
    // response to CopyFail; it does not by itself make the connection unusable.
    bool saw_final_result = false;
    while (true) {
      adbc::driver::pgwire::UniqueResult result(PQgetResult(pg_conn));
      if (!result) break;
      saw_final_result = true;
      ExecStatusType pg_status = PQresultStatus(result.get());
      if (pg_status != PGRES_COMMAND_OK && execution_status.ok()) {
        execution_status =
            MakeStatus(result.get(), "[libpq] Failed to execute COPY statement: {} {}",
                       PQresStatus(pg_status), PQerrorMessage(pg_conn));
      }
      if (pg_status != PGRES_COMMAND_OK && pg_status != PGRES_FATAL_ERROR) {
        // An unexpected protocol state may require its own data transfer; do
        // not block trying to fetch another result and never reuse this handle.
        *copy_cleanup_failed = true;
        break;
      }
    }

    const PGTransactionStatusType transaction_status = PQtransactionStatus(pg_conn);
    if (!saw_final_result || PQstatus(pg_conn) != CONNECTION_OK ||
        (transaction_status != PQTRANS_IDLE && transaction_status != PQTRANS_INTRANS &&
         transaction_status != PQTRANS_INERROR)) {
      *copy_cleanup_failed = true;
    }
    if (*copy_cleanup_failed && execution_status.ok()) {
      return Status::IO("[libpq] COPY protocol cleanup could not be confirmed: ",
                        PQerrorMessage(pg_conn));
    }
    if (execution_status.ok() && require_utf8)
      execution_status = CheckUtf8Encoding(pg_conn);
    if (execution_status.ok() && rows_affected) *rows_affected = copied_rows;
    return execution_status;
  }

  Status BindAndExecuteNextBatch(PGconn* pg_conn, const std::string& table,
                                 const std::string& fields, size_t max_rows,
                                 int64_t* executed_rows) {
    if (require_utf8) UNWRAP_STATUS(CheckUtf8Encoding(pg_conn));
    *executed_rows = 0;
    param_buffer->size_bytes = 0;
    std::vector<Oid> batch_types;
    std::vector<const char*> batch_values;
    std::vector<int> batch_lengths;
    std::vector<int> batch_formats;
    std::vector<int64_t> batch_offsets;
    std::vector<bool> batch_nulls;
    const size_t columns = static_cast<size_t>(bind_schema->n_children);
    const size_t capacity = max_rows * columns;
    batch_types.reserve(capacity);
    batch_lengths.reserve(capacity);
    batch_formats.reserve(capacity);
    batch_offsets.reserve(capacity);
    batch_nulls.reserve(capacity);

    for (size_t row = 0; row < max_rows; row++) {
      UNWRAP_STATUS(EnsureNextRow());
      if (current->release == nullptr) break;

      for (size_t col = 0; col < columns; col++) {
        int64_t field_row;
        UNWRAP_STATUS(ParameterRow(col, current_row, &field_row));
        const bool is_null = ArrowArrayViewIsNull(array_view->children[col], field_row);
        if (bind_schema_fields[col].type == NANOARROW_TYPE_NA && !is_null) {
          return Status::InvalidArgument("Parameter $", row * columns + col + 1,
                                         " has null type but contains a non-null value");
        }

        const int64_t start = param_buffer->size_bytes;
        if (is_null) {
          UNWRAP_ERRNO(Internal, ArrowBufferAppendInt32(&param_buffer.value, 0));
        } else {
          UNWRAP_STATUS(WriteParameter(col, field_row));
        }
        const int64_t length = param_buffer->size_bytes - start - sizeof(int32_t);
        if (length > (std::numeric_limits<int>::max)()) {
          return Status::Internal("Parameter ", row * columns + col + 1,
                                  " serialized to >2GB of binary");
        }
        batch_types.push_back(param_types[col]);
        batch_lengths.push_back(static_cast<int>(length));
        batch_formats.push_back(param_formats[col]);
        batch_offsets.push_back(start + sizeof(int32_t));
        batch_nulls.push_back(is_null);
      }
      ++*executed_rows;
    }

    if (*executed_rows == 0) return Status::Ok();
    batch_values.reserve(batch_offsets.size());
    for (size_t i = 0; i < batch_offsets.size(); i++) {
      batch_values.push_back(batch_nulls[i]
                                 ? nullptr
                                 : reinterpret_cast<const char*>(param_buffer->data) +
                                       batch_offsets[i]);
    }

    const std::string query = BuildParameterizedInsertQuery(
        table, fields, columns, static_cast<size_t>(*executed_rows), json_parse_fields,
        encoded_fields);
    if (require_utf8) UNWRAP_STATUS(CheckUtf8Encoding(pg_conn));
    if (HasHllParameters()) {
      PqResultHelper prepared(pg_conn, query, require_utf8);
      UNWRAP_STATUS(prepared.Prepare(batch_types));
      UNWRAP_STATUS(CheckHllPrepared(pg_conn, batch_types));
    }
    if (has_super_fields) UNWRAP_STATUS(CheckSuperParseContext(pg_conn));
    adbc::driver::pgwire::UniqueResult result(
        PQexecParams(pg_conn, query.c_str(), static_cast<int>(batch_values.size()),
                     batch_types.data(), batch_values.data(), batch_lengths.data(),
                     batch_formats.data(), kPgBinaryFormat));
    const ExecStatusType pg_status = PQresultStatus(result.get());
    if (pg_status != PGRES_COMMAND_OK) {
      return MakeStatus(result.get(),
                        "[libpq] Failed to execute parameterized ingest batch: {} {}",
                        PQresStatus(pg_status), PQerrorMessage(pg_conn));
    }
    if (require_utf8) UNWRAP_STATUS(CheckUtf8Encoding(pg_conn));
    return Status::Ok();
  }

  Status ExecutePreparedRows(PGconn* pg_conn, const std::string& table,
                             const std::string& fields,
                             const PostgresTypeResolver& type_resolver,
                             bool connection_autocommit, size_t max_batch_rows,
                             int64_t* rows_affected,
                             const PostgresType* ingest_targets = nullptr) {
    if (rows_affected) *rows_affected = 0;
    const size_t columns = static_cast<size_t>(bind_schema->n_children);
    constexpr size_t kMaxParameters = 32767;
    if (columns == 0 || columns > kMaxParameters || max_batch_rows == 0) {
      return Status::InvalidArgument("Invalid parameterized ingest dimensions");
    }
    max_batch_rows = (std::min)(max_batch_rows, kMaxParameters / columns);

    // Both the single-row prepared path and the multi-row parameterized path
    // must preserve statement-level atomicity across the whole Arrow stream.
    // In autocommit mode we own this transaction; otherwise the caller does.
    if (connection_autocommit) {
      PqResultHelper begin(pg_conn, "BEGIN");
      UNWRAP_STATUS(begin.Execute());
    }

    auto rollback_if_owned = [&]() {
      if (!connection_autocommit) return;
      PqResultHelper rollback(pg_conn, "ROLLBACK");
      // Preserve the ingestion error and report uncertainty if rollback fails.
      if (!rollback.Execute().ok()) cleanup_failed = true;
      if (rows_affected) *rows_affected = 0;
    };

    // The transaction is already open in autocommit mode, so timezone cleanup
    // must not independently commit it.
    Status execution_status =
        SetParamTypes(pg_conn, type_resolver, /*autocommit=*/false, ingest_targets);
    if (execution_status.ok() && max_batch_rows == 1) {
      execution_status = Prepare(
          pg_conn, BuildParameterizedInsertQuery(table, fields, columns, 1,
                                                 json_parse_fields, encoded_fields));
    }
    while (true) {
      if (!execution_status.ok()) break;
      if (max_batch_rows == 1) {
        execution_status = EnsureNextRow();
        if (!execution_status.ok() || current->release == nullptr) break;

        adbc::driver::pgwire::UniqueResult result;
        execution_status = BindAndExecuteCurrentRow(pg_conn, &result, kPgBinaryFormat);
        if (!execution_status.ok()) break;
        if (rows_affected) (*rows_affected)++;
      } else {
        int64_t executed_rows = 0;
        execution_status = BindAndExecuteNextBatch(pg_conn, table, fields, max_batch_rows,
                                                   &executed_rows);
        if (!execution_status.ok() || executed_rows == 0) break;
        if (rows_affected) *rows_affected += executed_rows;
      }
    }

    Status cleanup_status = Cleanup(pg_conn);
    if (!execution_status.ok()) {
      rollback_if_owned();
      return execution_status;
    }
    if (!cleanup_status.ok()) {
      rollback_if_owned();
      return cleanup_status;
    }

    if (connection_autocommit) {
      PqResultHelper commit(pg_conn, "COMMIT");
      Status commit_status = commit.Execute();
      if (!commit_status.ok()) {
        rollback_if_owned();
        return commit_status;
      }
    }
    return Status::Ok();
  }

  Status FlushCopyWriterToConn(PGconn* pg_conn, const PostgresCopyStreamWriter& writer) {
    // https://github.com/apache/arrow-adbc/issues/1921: PostgreSQL has a max
    // size for a single message that we need to respect (1 GiB - 1).  Since
    // the buffer can be chunked up as much as we want, go for 16 MiB as our
    // limit.
    // https://github.com/postgres/postgres/blob/23c5a0e7d43bc925c6001538f04a458933a11fc1/src/common/stringinfo.c#L28
    constexpr int64_t kMaxCopyBufferSize = 0x1000000;
    ArrowBuffer buffer = writer.WriteBuffer();

    auto* data = reinterpret_cast<char*>(buffer.data);
    int64_t remaining = buffer.size_bytes;
    while (remaining > 0) {
      int64_t to_write = std::min<int64_t>(remaining, kMaxCopyBufferSize);
      if (PQputCopyData(pg_conn, data, to_write) <= 0) {
        return Status::IO("Error writing tuple field data: ", PQerrorMessage(pg_conn));
      }
      remaining -= to_write;
      data += to_write;
    }

    return Status::Ok();
  }
};
}  // namespace adbcpq
