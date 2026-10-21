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

// Windows
#define NOMINMAX

// For #warning Please include winsock2.h before windows.h on RTools/msys2
#ifdef _WIN32
#include <winsock2.h>
#endif

#include "statement.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <libpq-fe.h>
#include <nanoarrow/nanoarrow.hpp>

#include "bind_stream.h"
#include "connection.h"
#include "driver/common/options.h"
#include "driver/common/utils.h"
#include "driver/framework/utility.h"
#include "driver/pgwire/libpq_raii.h"
#include "error.h"
#include "postgres_type.h"
#include "postgres_util.h"
#include "redshift_interval_catalog.h"
#include "redshift_replace_preflight.h"
#include "result_helper.h"
#include "result_reader.h"

namespace adbcpq {

namespace {

// Only autocommit replace ingest on a transactional-DDL backend owns this
// transaction. PostgreSQL binary COPY can keep it open through data transfer.
// Caller-owned PostgreSQL transactions use ScopedReplaceIngestSavepoint.
class ScopedReplaceIngestTransaction {
 public:
  ScopedReplaceIngestTransaction(PostgresConnection* connection, bool enabled)
      : connection_(connection), conn_(connection->conn()), enabled_(enabled) {}

  ScopedReplaceIngestTransaction(const ScopedReplaceIngestTransaction&) = delete;
  ScopedReplaceIngestTransaction& operator=(const ScopedReplaceIngestTransaction&) =
      delete;

  ~ScopedReplaceIngestTransaction() {
    // This is a last-resort cleanup for an unexpected exit. Explicit error
    // paths call Rollback() so their original SQL error can be returned.
    // Avoid allocating a diagnostic Status from this noexcept destructor.
    if (!open_) return;
    open_ = false;
    const PGTransactionStatusType txn_status = PQtransactionStatus(conn_);
    if (connection_->unusable() || PQstatus(conn_) != CONNECTION_OK ||
        (txn_status != PQTRANS_INTRANS && txn_status != PQTRANS_INERROR)) {
      connection_->MarkReplaceIngestFinalizationFailed();
      return;
    }
    adbc::driver::pgwire::UniqueResult result(PQexec(conn_, "ROLLBACK"));
    if (!result || PQresultStatus(result.get()) != PGRES_COMMAND_OK ||
        std::strcmp(PQcmdStatus(result.get()), "ROLLBACK") != 0 ||
        PQtransactionStatus(conn_) != PQTRANS_IDLE) {
      connection_->MarkReplaceIngestFinalizationFailed();
    }
  }

  Status Begin() {
    if (!enabled_) return Status::Ok();
    // An ADBC autocommit connection may still have a caller's raw-SQL BEGIN.
    // Never claim or roll back that transaction.
    const PGTransactionStatusType txn_status = PQtransactionStatus(conn_);
    if (txn_status == PQTRANS_UNKNOWN) {
      connection_->MarkUnknownTransactionState();
      return Status::IO(
          "[libpq] Cannot determine transaction state before replace ingest");
    }
    if (txn_status != PQTRANS_IDLE) {
      return Status::InvalidState(
          "[libpq] Cannot replace table while a transaction is already active");
    }

    adbc::driver::pgwire::UniqueResult result(PQexec(conn_, "BEGIN"));
    if (PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
      Status failure = MakeStatus(
          result.get(), "[libpq] Failed to begin replace-ingest transaction: {}",
          PQerrorMessage(conn_));
      if (PQtransactionStatus(conn_) != PQTRANS_IDLE) {
        connection_->MarkReplaceIngestFinalizationFailed();
      }
      return failure;
    }
    if (std::strcmp(PQcmdStatus(result.get()), "BEGIN") != 0 ||
        PQtransactionStatus(conn_) != PQTRANS_INTRANS) {
      connection_->MarkReplaceIngestFinalizationFailed();
      return Status::IO("[libpq] Replace-ingest transaction did not begin cleanly");
    }
    open_ = true;
    return Status::Ok();
  }

  Status Rollback() { return Finalize("ROLLBACK"); }
  Status Commit() { return Finalize("COMMIT"); }
  // A failed COPY termination or lost connection forbids further SQL. The
  // caller has already retired the connection; closing it releases the txn.
  void Abandon() { open_ = false; }

 private:
  Status Finalize(const char* command) {
    if (!open_) return Status::Ok();
    // Disarm before issuing SQL: a failed COMMIT can have an uncertain outcome,
    // and must not prompt an accidental second rollback from the destructor.
    open_ = false;
    const PGTransactionStatusType txn_status = PQtransactionStatus(conn_);
    if (connection_->unusable() || PQstatus(conn_) != CONNECTION_OK ||
        (txn_status != PQTRANS_INTRANS && txn_status != PQTRANS_INERROR)) {
      connection_->MarkReplaceIngestFinalizationFailed();
      return Status::IO(
          "[libpq] Cannot finalize replace ingest while protocol state is uncertain");
    }
    adbc::driver::pgwire::UniqueResult result(PQexec(conn_, command));
    if (PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
      connection_->MarkReplaceIngestFinalizationFailed();
      return MakeStatus(result.get(), "[libpq] Failed to finalize replace ingest: {}",
                        PQerrorMessage(conn_));
    }
    if (std::strcmp(PQcmdStatus(result.get()), command) != 0 ||
        PQtransactionStatus(conn_) != PQTRANS_IDLE) {
      connection_->MarkReplaceIngestFinalizationFailed();
      return Status::IO(
          "[libpq] Replace-ingest finalization had an unexpected "
          "command tag or transaction state");
    }
    return Status::Ok();
  }

  PostgresConnection* connection_;
  PGconn* conn_;
  bool enabled_;
  bool open_ = false;
};

// A recoverable replacement inside a caller-owned transaction. Never COMMIT
// or roll back the whole transaction. The backend profile explicitly opts into
// PostgreSQL's nested same-name savepoint semantics.
class ScopedReplaceIngestSavepoint {
 public:
  ScopedReplaceIngestSavepoint(PostgresConnection* connection, bool enabled)
      : connection_(connection), conn_(connection->conn()), enabled_(enabled) {}

  ScopedReplaceIngestSavepoint(const ScopedReplaceIngestSavepoint&) = delete;
  ScopedReplaceIngestSavepoint& operator=(const ScopedReplaceIngestSavepoint&) = delete;

  ~ScopedReplaceIngestSavepoint() {
    // Allocation-free last resort: recover only our confirmed newest point.
    if (!open_) return;
    open_ = false;
    if (!CanRecover()) {
      connection_->MarkReplaceIngestFinalizationFailed();
      return;
    }
    adbc::driver::pgwire::UniqueResult rollback(PQexec(conn_, kRollback));
    if (!Confirmed(rollback.get(), "ROLLBACK")) {
      connection_->MarkReplaceIngestFinalizationFailed();
      return;
    }
    adbc::driver::pgwire::UniqueResult release(PQexec(conn_, kRelease));
    if (!Confirmed(release.get(), "RELEASE")) {
      connection_->MarkReplaceIngestFinalizationFailed();
    }
  }

  Status Begin() {
    if (!enabled_) return Status::Ok();
    const PGTransactionStatusType txn_status = PQtransactionStatus(conn_);
    if (PQstatus(conn_) != CONNECTION_OK || txn_status == PQTRANS_ACTIVE ||
        txn_status == PQTRANS_UNKNOWN) {
      connection_->MarkReplaceIngestFinalizationFailed();
      return Status::IO("[libpq] Cannot begin replace savepoint in uncertain state");
    }
    if (connection_->unusable() || txn_status != PQTRANS_INTRANS) {
      return Status::InvalidState(
          "[libpq] Replace-ingest savepoint requires an active healthy transaction");
    }
    adbc::driver::pgwire::UniqueResult result(PQexec(conn_, kBegin));
    if (PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
      // No confirmed point means recovery must never target a caller's older
      // same-named point. A known SQL failure remains caller-recoverable.
      if (!CanRecover()) connection_->MarkReplaceIngestFinalizationFailed();
      return MakeStatus(result.get(), "[libpq] Failed to begin replace savepoint: {}",
                        PQerrorMessage(conn_));
    }
    if (!Confirmed(result.get(), "SAVEPOINT")) {
      connection_->MarkReplaceIngestFinalizationFailed();
      return Status::IO("[libpq] Replace-ingest savepoint did not begin cleanly");
    }
    open_ = true;
    return Status::Ok();
  }

  Status Rollback() { return Finalize(/*recover=*/true); }
  Status Release() { return Finalize(/*recover=*/false); }
  void Abandon() { open_ = false; }

 private:
  bool CanRecover() const {
    const PGTransactionStatusType txn_status = PQtransactionStatus(conn_);
    return !connection_->unusable() && PQstatus(conn_) == CONNECTION_OK &&
           (txn_status == PQTRANS_INTRANS || txn_status == PQTRANS_INERROR);
  }

  bool Confirmed(PGresult* result, const char* tag) const {
    return result && PQresultStatus(result) == PGRES_COMMAND_OK &&
           std::strcmp(PQcmdStatus(result), tag) == 0 &&
           PQstatus(conn_) == CONNECTION_OK &&
           PQtransactionStatus(conn_) == PQTRANS_INTRANS;
  }

  Status Finalize(bool recover) {
    if (!open_) return Status::Ok();
    // Never retry uncertain recovery or address an older caller savepoint.
    open_ = false;
    if (!CanRecover() || (!recover && PQtransactionStatus(conn_) != PQTRANS_INTRANS)) {
      connection_->MarkReplaceIngestFinalizationFailed();
      return Status::IO("[libpq] Cannot finalize replace savepoint in uncertain state");
    }
    if (recover) {
      adbc::driver::pgwire::UniqueResult rollback(PQexec(conn_, kRollback));
      if (!Confirmed(rollback.get(), "ROLLBACK")) {
        connection_->MarkReplaceIngestFinalizationFailed();
        return MakeStatus(rollback.get(),
                          "[libpq] Failed to recover replace savepoint: {}",
                          PQerrorMessage(conn_));
      }
    }
    adbc::driver::pgwire::UniqueResult release(PQexec(conn_, kRelease));
    if (!Confirmed(release.get(), "RELEASE")) {
      connection_->MarkReplaceIngestFinalizationFailed();
      return MakeStatus(release.get(), "[libpq] Failed to release replace savepoint: {}",
                        PQerrorMessage(conn_));
    }
    return Status::Ok();
  }

  static constexpr const char* kBegin = "SAVEPOINT \"adbc_pgwire_replace\"";
  static constexpr const char* kRollback =
      "ROLLBACK TO SAVEPOINT \"adbc_pgwire_replace\"";
  static constexpr const char* kRelease = "RELEASE SAVEPOINT \"adbc_pgwire_replace\"";
  PostgresConnection* connection_;
  PGconn* conn_;
  bool enabled_;
  bool open_ = false;
};

}  // namespace

int TupleReader::GetSchema(struct ArrowSchema* out) {
  assert(copy_reader_ != nullptr);
  ArrowErrorInit(&na_error_);

  int na_res = copy_reader_->GetSchema(out);
  if (out->release == nullptr) {
    InternalAdbcSetError(&error_, "[libpq] Result set was already consumed or freed");
    status_ = ADBC_STATUS_INVALID_STATE;
    return InternalAdbcStatusCodeToErrno(status_);
  } else if (na_res != NANOARROW_OK) {
    // e.g., Can't allocate memory
    InternalAdbcSetError(&error_, "[libpq] Error copying schema");
    status_ = ADBC_STATUS_INTERNAL;
  }

  return na_res;
}

int TupleReader::GetCopyData() {
  if (pgbuf_ != nullptr) {
    PQfreemem(pgbuf_);
    pgbuf_ = nullptr;
  }

  data_.size_bytes = 0;
  data_.data.as_char = nullptr;

  int get_copy_res = PQgetCopyData(conn_, &pgbuf_, /*async=*/0);
  if (get_copy_res == -2) {
    InternalAdbcSetError(&error_, "[libpq] PQgetCopyData() failed: %s",
                         PQerrorMessage(conn_));
    status_ = ADBC_STATUS_IO;
    return InternalAdbcStatusCodeToErrno(status_);
  }

  if (get_copy_res == -1) {
    // Check the server-side response
    result_.reset(PQgetResult(conn_));
    const ExecStatusType pq_status = PQresultStatus(result_.get());
    int errno_result = ENODATA;
    if (pq_status != PGRES_COMMAND_OK) {
      status_ = MakeStatus(result_.get(), "[libpq] Execution error [{}]: {}",
                           PQresStatus(pq_status), PQresultErrorMessage(result_.get()))
                    .ToAdbc(&error_);
      errno_result = InternalAdbcStatusCodeToErrno(status_);
    }

    // Drain remaining responses
    do {
      result_.reset(PQgetResult(conn_));
    } while (result_);
    if (pq_status == PGRES_COMMAND_OK) {
      const int validation_result = copy_reader_->ValidateEndOfStream(&na_error_);
      if (validation_result != NANOARROW_OK) {
        InternalAdbcSetError(&error_, "[libpq] Invalid binary COPY stream: %s",
                             na_error_.message);
        status_ = ADBC_STATUS_INVALID_DATA;
        return validation_result;
      }
    }
    return errno_result;
  }

  data_.size_bytes = get_copy_res;
  data_.data.as_char = pgbuf_;
  if (copy_reader_->trailer_seen()) {
    InternalAdbcSetError(&error_, "[libpq] Unexpected COPY data after trailer");
    status_ = ADBC_STATUS_INVALID_DATA;
    return EINVAL;
  }
  return NANOARROW_OK;
}

int TupleReader::AppendRowAndFetchNext() {
  // Parse the result (the header AND the first row are included in the first
  // call to PQgetCopyData())
  int na_res = copy_reader_->ReadRecord(&data_, &na_error_);
  if (na_res != NANOARROW_OK && na_res != ENODATA) {
    InternalAdbcSetError(&error_, "[libpq] ReadRecord failed at row %" PRId64 ": %s",
                         row_id_, na_error_.message);
    status_ = ADBC_STATUS_IO;
    return na_res;
  }

  row_id_++;

  NANOARROW_RETURN_NOT_OK(GetCopyData());
  if ((copy_reader_->array_size_approx_bytes() + data_.size_bytes) >=
      batch_size_hint_bytes_) {
    // Appending the next row will result in an array larger than requested.
    // Return EOVERFLOW to force GetNext() to build the current result and return.
    return EOVERFLOW;
  }

  return NANOARROW_OK;
}

int TupleReader::BuildOutput(struct ArrowArray* out) {
  if (copy_reader_->array_size_approx_bytes() == 0) {
    out->release = nullptr;
    return NANOARROW_OK;
  }

  int na_res = copy_reader_->GetArray(out, &na_error_);
  if (na_res != NANOARROW_OK) {
    InternalAdbcSetError(&error_, "[libpq] Failed to build result array: %s",
                         na_error_.message);
    status_ = ADBC_STATUS_INTERNAL;
    return na_res;
  }

  return NANOARROW_OK;
}

int TupleReader::GetNext(struct ArrowArray* out) {
  if (is_finished_) {
    out->release = nullptr;
    return 0;
  }

  int na_res;
  ArrowErrorInit(&na_error_);

  if (row_id_ == -1) {
    na_res = GetCopyData();
    if (na_res == ENODATA) {
      is_finished_ = true;
      out->release = nullptr;
      return 0;
    } else if (na_res != NANOARROW_OK) {
      return na_res;
    }

    na_res = copy_reader_->ReadHeader(&data_, &na_error_);
    if (na_res != NANOARROW_OK) {
      InternalAdbcSetError(&error_, "[libpq] ReadHeader() failed: %s", na_error_.message);
      return na_res;
    }

    row_id_++;
  }

  do {
    na_res = AppendRowAndFetchNext();
    if (na_res == EOVERFLOW) {
      // The result would be too big to return if we appended the row. When EOVERFLOW is
      // returned, the copy reader leaves the output in a valid state. The data is left in
      // pg_buf_/data_ and will attempt to be appended on the next call to GetNext()
      return BuildOutput(out);
    }
  } while (na_res == NANOARROW_OK);

  if (na_res != ENODATA) {
    return na_res;
  }

  is_finished_ = true;

  // Finish the result properly and return the last result. Note that BuildOutput() may
  // set tmp.release = nullptr if there were zero rows in the copy reader (can
  // occur in an overflow scenario).
  NANOARROW_RETURN_NOT_OK(BuildOutput(out));
  return NANOARROW_OK;
}

void TupleReader::Release() {
  if (error_.release) {
    error_.release(&error_);
  }
  error_ = ADBC_ERROR_INIT;
  status_ = ADBC_STATUS_OK;

  result_.reset();

  if (pgbuf_) {
    PQfreemem(pgbuf_);
    pgbuf_ = nullptr;
  }

  if (copy_reader_) {
    copy_reader_.reset();
  }

  is_finished_ = false;
  row_id_ = -1;
}

// Instead of directly exporting the TupleReader, which is tied to the
// lifetime of the Statement, we export a weak_ptr reference instead.  That
// way if the user accidentally closes the Statement before the
// ArrowArrayStream, we can avoid a crash.
// See https://github.com/apache/arrow-adbc/issues/2629
struct ExportedTupleReader {
  std::weak_ptr<TupleReader> self;
};

void TupleReader::ExportTo(struct ArrowArrayStream* stream) {
  stream->get_schema = &GetSchemaTrampoline;
  stream->get_next = &GetNextTrampoline;
  stream->get_last_error = &GetLastErrorTrampoline;
  stream->release = &ReleaseTrampoline;
  stream->private_data = new ExportedTupleReader{weak_from_this()};
}

const struct AdbcError* TupleReader::ErrorFromArrayStream(struct ArrowArrayStream* self,
                                                          AdbcStatusCode* status) {
  if (!self->private_data || self->release != &ReleaseTrampoline) {
    return nullptr;
  }

  auto* wrapper = static_cast<ExportedTupleReader*>(self->private_data);
  auto maybe_reader = wrapper->self.lock();
  if (maybe_reader) {
    if (status) {
      *status = maybe_reader->status_;
    }
    return &maybe_reader->error_;
  }
  return nullptr;
}

int TupleReader::GetSchemaTrampoline(struct ArrowArrayStream* self,
                                     struct ArrowSchema* out) {
  if (!self || !self->private_data) return EINVAL;

  auto* wrapper = static_cast<ExportedTupleReader*>(self->private_data);
  auto maybe_reader = wrapper->self.lock();
  if (maybe_reader) {
    return maybe_reader->GetSchema(out);
  }
  // statement was closed or reader was otherwise invalidated
  return EINVAL;
}

int TupleReader::GetNextTrampoline(struct ArrowArrayStream* self,
                                   struct ArrowArray* out) {
  if (!self || !self->private_data) return EINVAL;

  auto* wrapper = static_cast<ExportedTupleReader*>(self->private_data);
  auto maybe_reader = wrapper->self.lock();
  if (maybe_reader) {
    return maybe_reader->GetNext(out);
  }
  // statement was closed or reader was otherwise invalidated
  return EINVAL;
}

const char* TupleReader::GetLastErrorTrampoline(struct ArrowArrayStream* self) {
  if (!self || !self->private_data) return nullptr;
  constexpr std::string_view kReaderInvalidated =
      "[libpq] Reader invalidated (statement or reader was closed)";

  auto* wrapper = static_cast<ExportedTupleReader*>(self->private_data);
  auto maybe_reader = wrapper->self.lock();
  if (maybe_reader) {
    return maybe_reader->last_error();
  }
  // statement was closed or reader was otherwise invalidated
  return kReaderInvalidated.data();
}

void TupleReader::ReleaseTrampoline(struct ArrowArrayStream* self) {
  if (!self || !self->private_data) return;

  auto* wrapper = static_cast<ExportedTupleReader*>(self->private_data);
  auto maybe_reader = wrapper->self.lock();
  if (maybe_reader) {
    maybe_reader->Release();
  }
  delete wrapper;
  self->private_data = nullptr;
  self->release = nullptr;
}

AdbcStatusCode PostgresStatement::New(struct AdbcConnection* connection,
                                      struct AdbcError* error) {
  if (!connection || !connection->private_data) {
    InternalAdbcSetError(error, "%s",
                         "[libpq] Must provide an initialized AdbcConnection");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  connection_ =
      *reinterpret_cast<std::shared_ptr<PostgresConnection>*>(connection->private_data);
  type_resolver_ = connection_->type_resolver();
  use_copy_ = connection_->use_copy();
  ClearResult();
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresStatement::Bind(struct ArrowArray* values,
                                       struct ArrowSchema* schema,
                                       struct AdbcError* error) {
  if (!values || !values->release) {
    InternalAdbcSetError(error, "%s", "[libpq] Must provide non-NULL array");
    return ADBC_STATUS_INVALID_ARGUMENT;
  } else if (!schema || !schema->release) {
    InternalAdbcSetError(error, "%s", "[libpq] Must provide non-NULL schema");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }

  ArrowError vendor_error{};
  bool has_super = false;
  if (type_resolver_ && RedshiftSuperSchema::ValidateRoute(
                            schema, type_resolver_->is_redshift(),
                            !ingest_.target.empty(), ingest_.mode == IngestMode::kReplace,
                            &has_super, &vendor_error) != NANOARROW_OK) {
    InternalAdbcSetError(error, "%s", vendor_error.message);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  ArrowError interval_error{};
  if (type_resolver_ && PostgresType::ValidateTemporalWriteSchema(
                            *type_resolver_, schema, &interval_error) != NANOARROW_OK) {
    InternalAdbcSetError(error, "%s", interval_error.message);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  const int interval_status = type_resolver_
                                  ? PostgresType::ValidateIntervalWriteSchema(
                                        *type_resolver_, schema, &interval_error)
                                  : NANOARROW_OK;
  if (interval_status != NANOARROW_OK) {
    InternalAdbcSetError(error, "%s", interval_error.message);
    return interval_status == ENOTSUP ? ADBC_STATUS_NOT_IMPLEMENTED
                                      : ADBC_STATUS_INVALID_ARGUMENT;
  }

  if (type_resolver_ && PostgresType::ValidateDecimalWriteSchema(
                            *type_resolver_, schema, &interval_error) != NANOARROW_OK) {
    InternalAdbcSetError(error, "%s", interval_error.message);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  if (type_resolver_ && PostgresType::ValidateScalarWriteSchema(
                            *type_resolver_, schema, &interval_error) != NANOARROW_OK) {
    InternalAdbcSetError(error, "%s", interval_error.message);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }

  if (type_resolver_ && PostgresType::ValidateBytesWriteSchema(
                            *type_resolver_, schema, &interval_error) != NANOARROW_OK) {
    InternalAdbcSetError(error, "%s", interval_error.message);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }

  if (bind_.release) bind_.release(&bind_);
  // Make a one-value stream
  adbc::driver::MakeArrayStream(schema, values, &bind_);
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresStatement::Bind(struct ArrowArrayStream* stream,
                                       struct AdbcError* error) {
  if (!stream || !stream->release) {
    InternalAdbcSetError(error, "%s", "[libpq] Must provide non-NULL stream");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  // Inspect without taking ownership: reject read-only interval schemas before
  // execution, transactions, parameter inference, or replace-ingest DDL.
  nanoarrow::UniqueSchema schema;
  ArrowError interval_error{};
  CHECK_NA_DETAIL(INVALID_ARGUMENT,
                  ArrowArrayStreamGetSchema(stream, schema.get(), &interval_error),
                  &interval_error, error);
  bool has_super = false;
  if (type_resolver_ && RedshiftSuperSchema::ValidateRoute(
                            schema.get(), type_resolver_->is_redshift(),
                            !ingest_.target.empty(), ingest_.mode == IngestMode::kReplace,
                            &has_super, &interval_error) != NANOARROW_OK) {
    InternalAdbcSetError(error, "%s", interval_error.message);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  if (type_resolver_ &&
      PostgresType::ValidateTemporalWriteSchema(*type_resolver_, schema.get(),
                                                &interval_error) != NANOARROW_OK) {
    InternalAdbcSetError(error, "%s", interval_error.message);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  const int interval_status = type_resolver_
                                  ? PostgresType::ValidateIntervalWriteSchema(
                                        *type_resolver_, schema.get(), &interval_error)
                                  : NANOARROW_OK;
  if (interval_status != NANOARROW_OK) {
    InternalAdbcSetError(error, "%s", interval_error.message);
    return interval_status == ENOTSUP ? ADBC_STATUS_NOT_IMPLEMENTED
                                      : ADBC_STATUS_INVALID_ARGUMENT;
  }
  if (type_resolver_ &&
      PostgresType::ValidateDecimalWriteSchema(*type_resolver_, schema.get(),
                                               &interval_error) != NANOARROW_OK) {
    InternalAdbcSetError(error, "%s", interval_error.message);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  if (type_resolver_ &&
      PostgresType::ValidateScalarWriteSchema(*type_resolver_, schema.get(),
                                              &interval_error) != NANOARROW_OK) {
    InternalAdbcSetError(error, "%s", interval_error.message);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  if (type_resolver_ &&
      PostgresType::ValidateBytesWriteSchema(*type_resolver_, schema.get(),
                                             &interval_error) != NANOARROW_OK) {
    InternalAdbcSetError(error, "%s", interval_error.message);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  // Move stream
  if (bind_.release) bind_.release(&bind_);
  bind_ = *stream;
  std::memset(stream, 0, sizeof(*stream));
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresStatement::Cancel(struct AdbcError* error) {
  // Ultimately the same underlying PGconn
  return connection_->Cancel(error);
}

AdbcStatusCode PostgresStatement::CreateBulkTable(
    const std::string& current_schema, const struct ArrowSchema& source_schema,
    std::string* escaped_table, std::string* escaped_field_list,
    PostgresType* copy_target_types, bool* has_copy_target_types,
    const std::function<Status()>& before_replace_ddl, struct AdbcError* error) {
  bool has_super = false;
  ArrowError super_error{};
  CHECK_NA_DETAIL(INVALID_ARGUMENT,
                  RedshiftSuperSchema::ValidateRoute(
                      &source_schema, type_resolver_->is_redshift(), true,
                      ingest_.mode == IngestMode::kReplace, &has_super, &super_error),
                  &super_error, error);
  if (has_super) RAISE_STATUS(error, CheckSuperParseContext(connection_->conn()));

  PGconn* conn = connection_->conn();
  *has_copy_target_types = false;

  if (ingest_.mode == IngestMode::kReplace && source_schema.n_children == 0) {
    InternalAdbcSetError(error,
                         "[libpq] Cannot replace a table with zero ingest columns");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }

  // Validate the Arrow field names before replacement can drop an existing table.
  // An ArrowSchema child may legitimately have a null name, but SQL columns may not.
  // Only byte-identical names are checked here; other identifier equivalence is
  // backend-specific.
  std::unordered_set<std::string_view> seen_field_names;
  for (int64_t i = 0; i < source_schema.n_children; ++i) {
    if (source_schema.children == nullptr || source_schema.children[i] == nullptr ||
        source_schema.children[i]->name == nullptr ||
        source_schema.children[i]->name[0] == '\0') {
      InternalAdbcSetError(
          error, "[libpq] Bulk ingest column %" PRId64 " must have a nonempty name", i);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }

    const std::string_view field_name(source_schema.children[i]->name);
    if (!seen_field_names.insert(field_name).second) {
      InternalAdbcSetError(
          error, "[libpq] Bulk ingest column %" PRId64 " duplicates column name %s", i,
          source_schema.children[i]->name);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
  }

  if (!ingest_.db_schema.empty() && ingest_.temporary) {
    InternalAdbcSetError(error, "[libpq] Cannot set both %s and %s",
                         ADBC_INGEST_OPTION_TARGET_DB_SCHEMA,
                         ADBC_INGEST_OPTION_TEMPORARY);
    return ADBC_STATUS_INVALID_STATE;
  }

  if (ingest_.mode == IngestMode::kReplace &&
      connection_->backend_profile().replace_identifier_preflight ==
          adbc::driver::pgwire::ReplaceIdentifierPreflight::kRedshift) {
    std::vector<std::string_view> column_names;
    column_names.reserve(source_schema.n_children);
    for (int64_t i = 0; i < source_schema.n_children; ++i) {
      column_names.emplace_back(source_schema.children[i]->name);
    }
    // The backend module owns the rule and the setting query; this only runs it.
    auto read_setting = [conn](std::string_view query, std::string* value) -> Status {
      PqResultHelper helper{conn, std::string(query)};
      UNWRAP_STATUS(helper.Execute());
      auto it = helper.begin();
      if (it == helper.end() || (*it)[0].is_null) {
        return Status(ADBC_STATUS_INVALID_DATA,
                      "[libpq] No value returned for '" + std::string(query) + "'");
      }
      *value = std::string((*it)[0].value());
      return Status::Ok();
    };
    RAISE_STATUS(error,
                 adbc::driver::pgwire::PreflightRedshiftReplace(
                     ingest_.target, ingest_.db_schema, column_names, read_setting));
  }

  {
    if (!ingest_.db_schema.empty()) {
      char* escaped =
          PQescapeIdentifier(conn, ingest_.db_schema.c_str(), ingest_.db_schema.size());
      if (escaped == nullptr) {
        InternalAdbcSetError(
            error, "[libpq] Failed to escape target schema %s for ingestion: %s",
            ingest_.db_schema.c_str(), PQerrorMessage(conn));
        return ADBC_STATUS_INTERNAL;
      }
      *escaped_table += escaped;
      *escaped_table += " . ";
      PQfreemem(escaped);
    } else if (ingest_.temporary) {
      // OK to be redundant (CREATE TEMPORARY TABLE pg_temp.foo)
      *escaped_table += "pg_temp . ";
    } else {
      // Explicitly specify the current schema to avoid any temporary tables
      // shadowing this table
      char* escaped =
          PQescapeIdentifier(conn, current_schema.c_str(), current_schema.size());
      *escaped_table += escaped;
      *escaped_table += " . ";
      PQfreemem(escaped);
    }

    if (!ingest_.target.empty()) {
      char* escaped =
          PQescapeIdentifier(conn, ingest_.target.c_str(), ingest_.target.size());
      if (escaped == nullptr) {
        InternalAdbcSetError(error,
                             "[libpq] Failed to escape target table %s for ingestion: %s",
                             ingest_.target.c_str(), PQerrorMessage(conn));
        return ADBC_STATUS_INTERNAL;
      }
      *escaped_table += escaped;
      PQfreemem(escaped);
    }
  }

  // Interval constraints and temporal precision need an existing-vs-new
  // distinction: unused CREATE declarations must not erase append semantics.
  bool append_existing_semantic = false;
  bool interval_create_append = false;
  if (ingest_.mode == IngestMode::kCreateAppend) {
    bool requires_existing = false;
    for (int64_t i = 0; i < source_schema.n_children; ++i) {
      RedshiftTemporalSchema temporal;
      ArrowError na_error{};
      CHECK_NA_DETAIL(INVALID_ARGUMENT,
                      RedshiftTemporalSchema::Parse(source_schema.children[i],
                                                    type_resolver_->is_redshift(),
                                                    &temporal, &na_error),
                      &na_error, error);
      requires_existing |=
          temporal.enabled() && temporal.precision && *temporal.precision < 6;
      RedshiftTimetzSchema timetz;
      CHECK_NA_DETAIL(
          INVALID_ARGUMENT,
          RedshiftTimetzSchema::Parse(source_schema.children[i],
                                      type_resolver_->is_redshift(), &timetz, &na_error),
          &na_error, error);
      requires_existing |= timetz.enabled && timetz.precision && *timetz.precision < 6;
      RedshiftIntervalSchema interval;
      CHECK_NA_DETAIL(INVALID_ARGUMENT,
                      RedshiftIntervalSchema::Parse(source_schema.children[i],
                                                    type_resolver_->is_redshift(),
                                                    &interval, &na_error),
                      &na_error, error);
      interval_create_append |= interval.enabled();
      requires_existing |= interval.enabled();
    }
    requires_existing |= has_super;
    if (requires_existing) {
      std::string target = ingest_.target;
      std::string schema = ingest_.db_schema.empty() ? current_schema : ingest_.db_schema;
      if (type_resolver_->is_redshift()) {
        std::string lookup_schema = ingest_.temporary ? "" : schema;
        auto read_setting = [conn](std::string_view query, std::string* value) -> Status {
          PqResultHelper setting(conn, std::string(query), true);
          UNWRAP_STATUS(setting.Execute());
          if (setting.NumRows() != 1 || setting.NumColumns() != 1 ||
              setting.Row(0)[0].is_null)
            return Status(ADBC_STATUS_INVALID_DATA, "Missing identifier-case setting");
          *value = std::string(setting.Row(0)[0].value());
          return Status::Ok();
        };
        RAISE_STATUS(error, adbc::driver::pgwire::NormalizeRedshiftLookupIdentifiers(
                                &target, &lookup_schema, read_setting));
        if (!ingest_.temporary) schema = std::move(lookup_schema);
      }
      // Catalog-only query: CURRENT_SCHEMAS is leader-only on Redshift. The
      // current session's implicit TEMP namespace is the only matching member.
      // Do not cast a possibly missing relation to regclass or catch SQL errors:
      // that would abort a caller-owned transaction before the pre-DDL gate.
      PqResultHelper exists(
          conn,
          "SELECT cls.oid FROM pg_catalog.pg_class AS cls "
          "INNER JOIN pg_catalog.pg_namespace AS ns ON ns.oid=cls.relnamespace "
          "WHERE cls.relname=$1 AND " +
              std::string(ingest_.temporary ? "substring(ns.nspname,1,8)='pg_temp_' AND "
                                              "ns.nspname=ANY(current_schemas(true))"
                                            : "ns.nspname=$2"),
          type_resolver_->is_redshift());
      RAISE_STATUS(error, exists.Execute(ingest_.temporary
                                             ? std::vector<std::string>{target}
                                             : std::vector<std::string>{target, schema}));
      if (exists.NumColumns() != 1 || exists.NumRows() > 1) {
        InternalAdbcSetError(error, "[libpq] Ambiguous CREATE_APPEND target");
        return ADBC_STATUS_INVALID_DATA;
      }
      append_existing_semantic = exists.NumRows() == 1;
    }
  }

  std::string create;

  if (ingest_.temporary) {
    create = "CREATE TEMPORARY TABLE ";
  } else {
    create = "CREATE TABLE ";
  }

  switch (ingest_.mode) {
    case IngestMode::kCreate:
    case IngestMode::kAppend:
    case IngestMode::kReplace:
      // The replacement DROP is deferred until column mapping succeeds.
      break;
    case IngestMode::kCreateAppend:
      // An absent interval target uses plain CREATE; a concurrent appearance
      // errors instead of silently replacing the requested declaration.
      if (!interval_create_append) create += "IF NOT EXISTS ";
      break;
  }
  create += *escaped_table;
  create += " (";
  std::vector<std::string> source_field_names;
  source_field_names.reserve(source_schema.n_children);
  bool has_semantic_schema = false;

  for (int64_t i = 0; i < source_schema.n_children; i++) {
    if (i > 0) {
      create += ", ";
      *escaped_field_list += ", ";
    }

    const char* unescaped = source_schema.children[i]->name;
    char* escaped = PQescapeIdentifier(conn, unescaped, std::strlen(unescaped));
    if (escaped == nullptr) {
      InternalAdbcSetError(error, "[libpq] Failed to escape column %s for ingestion: %s",
                           unescaped, PQerrorMessage(conn));
      return ADBC_STATUS_INTERNAL;
    }
    create += escaped;
    *escaped_field_list += escaped;
    source_field_names.emplace_back(unescaped);
    PQfreemem(escaped);

    PostgresType pg_type;
    struct ArrowError na_error;
    CHECK_NA_DETAIL(INTERNAL,
                    PostgresType::FromSchema(*type_resolver_, source_schema.children[i],
                                             &pg_type, &na_error),
                    &na_error, error);
    RedshiftDecimalSchema decimal;
    CHECK_NA_DETAIL(
        INVALID_ARGUMENT,
        RedshiftDecimalSchema::Parse(source_schema.children[i],
                                     type_resolver_->is_redshift(), &decimal, &na_error),
        &na_error, error);
    RedshiftScalarSchema scalar;
    CHECK_NA_DETAIL(
        INVALID_ARGUMENT,
        RedshiftScalarSchema::Parse(source_schema.children[i],
                                    type_resolver_->is_redshift(), &scalar, &na_error),
        &na_error, error);
    RedshiftBytesSchema bytes;
    CHECK_NA_DETAIL(
        INVALID_ARGUMENT,
        RedshiftBytesSchema::Parse(source_schema.children[i],
                                   type_resolver_->is_redshift(), &bytes, &na_error),
        &na_error, error);
    if (bytes.enabled() && bytes.kind != RedshiftBytesKind::kVarbyte)
      RAISE_STATUS(error, CheckUtf8Encoding(conn));
    RedshiftTemporalSchema temporal;
    CHECK_NA_DETAIL(INVALID_ARGUMENT,
                    RedshiftTemporalSchema::Parse(source_schema.children[i],
                                                  type_resolver_->is_redshift(),
                                                  &temporal, &na_error),
                    &na_error, error);
    RedshiftIntervalSchema interval;
    CHECK_NA_DETAIL(INVALID_ARGUMENT,
                    RedshiftIntervalSchema::Parse(source_schema.children[i],
                                                  type_resolver_->is_redshift(),
                                                  &interval, &na_error),
                    &na_error, error);
    RedshiftTimetzSchema timetz;
    CHECK_NA_DETAIL(
        INVALID_ARGUMENT,
        RedshiftTimetzSchema::Parse(source_schema.children[i],
                                    type_resolver_->is_redshift(), &timetz, &na_error),
        &na_error, error);
    RedshiftSuperSchema super;
    CHECK_NA_DETAIL(
        INVALID_ARGUMENT,
        RedshiftSuperSchema::Parse(source_schema.children[i],
                                   type_resolver_->is_redshift(), &super, &na_error),
        &na_error, error);
    has_semantic_schema |= super.enabled || decimal.enabled || scalar.enabled() ||
                           bytes.enabled() || temporal.enabled() || interval.enabled() ||
                           timetz.enabled;
    std::string declaration = super.enabled ? "SUPER" : pg_type.sql_type_name();
    if (decimal.enabled && ingest_.mode != IngestMode::kAppend &&
        !append_existing_semantic) {
      CHECK_NA_DETAIL(INVALID_ARGUMENT, decimal.Declaration(&declaration, &na_error),
                      &na_error, error);
    }
    if (scalar.enabled()) declaration = RedshiftScalarSchema::SqlName(scalar.kind);
    if (temporal.enabled() && ingest_.mode != IngestMode::kAppend &&
        !append_existing_semantic)
      CHECK_NA_DETAIL(INVALID_ARGUMENT, temporal.Declaration(&declaration, &na_error),
                      &na_error, error);
    if (bytes.enabled() && ingest_.mode != IngestMode::kAppend &&
        !append_existing_semantic) {
      CHECK_NA_DETAIL(INVALID_ARGUMENT, bytes.Declaration(&declaration, &na_error),
                      &na_error, error);
      // PostgreSQL has BYTEA without a width declaration. Keep the explicit
      // semantic source envelope while exercising the shared path on PostgreSQL.
      if (!type_resolver_->is_redshift() && bytes.kind == RedshiftBytesKind::kVarbyte)
        declaration = "BYTEA";
    }
    if (interval.enabled() && ingest_.mode != IngestMode::kAppend &&
        !append_existing_semantic)
      CHECK_NA_DETAIL(INVALID_ARGUMENT,
                      interval.constraint.Declaration(&declaration, &na_error), &na_error,
                      error);
    if (timetz.enabled && ingest_.mode != IngestMode::kAppend &&
        !append_existing_semantic)
      CHECK_NA_DETAIL(INVALID_ARGUMENT, timetz.Declaration(&declaration, &na_error),
                      &na_error, error);
    create += " " + declaration;
  }

  if (ingest_.mode == IngestMode::kAppend || append_existing_semantic) {
    AdbcStatusCode status = ResolveCopyTargetTypes(*escaped_table, source_field_names,
                                                   copy_target_types, error);
    if (status != ADBC_STATUS_OK) {
      return status;
    }
    RAISE_STATUS(error, ValidateVendorTargets(&source_schema, *copy_target_types,
                                              type_resolver_->is_redshift()));
    *has_copy_target_types = true;
    return ADBC_STATUS_OK;
  }

  create += ")";
  const bool own_replace_ddl =
      ingest_.mode == IngestMode::kReplace && connection_->autocommit() &&
      connection_->backend_profile().transactions.transactional_ddl &&
      !before_replace_ddl;
  ScopedReplaceIngestTransaction ddl_transaction(connection_.get(), own_replace_ddl);
  if (ingest_.mode == IngestMode::kReplace && before_replace_ddl) {
    RAISE_STATUS(error, before_replace_ddl());
  }
  RAISE_STATUS(error, ddl_transaction.Begin());

  if (ingest_.mode == IngestMode::kReplace) {
    std::string drop = "DROP TABLE IF EXISTS " + *escaped_table;
    adbc::driver::pgwire::UniqueResult result(
        PQexecParams(conn, drop.c_str(), /*nParams=*/0,
                     /*paramTypes=*/nullptr, /*paramValues=*/nullptr,
                     /*paramLengths=*/nullptr, /*paramFormats=*/nullptr,
                     /*resultFormat=*/1 /*(binary)*/));
    if (PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
      Status failure =
          MakeStatus(result.get(), "[libpq] Failed to drop table: {}\nQuery was: {}",
                     PQerrorMessage(conn), drop);
      (void)ddl_transaction.Rollback();
      return failure.ToAdbc(error);
    }
  }

  adbc::driver::pgwire::UniqueResult result(
      PQexecParams(conn, create.c_str(), /*nParams=*/0,
                   /*paramTypes=*/nullptr, /*paramValues=*/nullptr,
                   /*paramLengths=*/nullptr, /*paramFormats=*/nullptr,
                   /*resultFormat=*/1 /*(binary)*/));
  if (PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
    Status failure =
        MakeStatus(result.get(), "[libpq] Failed to create table: {}\nQuery was: {}",
                   PQerrorMessage(conn), create);
    (void)ddl_transaction.Rollback();
    return failure.ToAdbc(error);
  }
  RAISE_STATUS(error, ddl_transaction.Commit());
  if (ingest_.mode == IngestMode::kCreateAppend || type_resolver_->is_redshift() ||
      has_semantic_schema) {
    AdbcStatusCode status = ResolveCopyTargetTypes(*escaped_table, source_field_names,
                                                   copy_target_types, error);
    if (status != ADBC_STATUS_OK) {
      return status;
    }
    RAISE_STATUS(error, ValidateVendorTargets(&source_schema, *copy_target_types,
                                              type_resolver_->is_redshift()));
    *has_copy_target_types = true;
  }
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresStatement::ResolveCopyTargetTypes(
    const std::string& escaped_table, const std::vector<std::string>& source_field_names,
    PostgresType* copy_target_types, struct AdbcError* error) {
  PqResultHelper result_helper{connection_->conn(),
                               "SELECT attr.attname, attr.atttypid, attr.atttypmod "
                               "FROM pg_catalog.pg_attribute AS attr "
                               "WHERE attr.attrelid = $1::regclass::oid "
                               "AND attr.attnum > 0 AND NOT attr.attisdropped "
                               "ORDER BY attr.attnum"};
  result_helper.set_require_utf8(type_resolver_->is_redshift());
  RAISE_STATUS(error, result_helper.Execute({escaped_table}));
  PostgresType table;
  RAISE_STATUS(error, result_helper.ResolveTableSchema(*type_resolver_, &table));
  *copy_target_types = PostgresType(PostgresTypeId::kRecord);
  for (const auto& field_name : source_field_names) {
    const PostgresType* target = nullptr;
    for (int64_t i = 0; i < table.n_children(); i++) {
      if (table.child(i).field_name() == field_name) {
        target = &table.child(i);
        break;
      }
    }
    if (!target) {
      InternalAdbcSetError(error,
                           "[libpq] Column \"%s\" does not exist in target table %s",
                           field_name.c_str(), escaped_table.c_str());
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    copy_target_types->AppendChild(field_name, *target);
  }
  if (type_resolver_->is_redshift())
    RAISE_STATUS(error, ObserveIntervalTargets(connection_->conn(), escaped_table, table,
                                               copy_target_types));
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresStatement::ExecuteBind(struct ArrowArrayStream* stream,
                                              int64_t* rows_affected,
                                              struct AdbcError* error) {
  auto lease = std::make_shared<int>(0);
  if (!connection_->TryClaimBoundStream(lease)) {
    InternalAdbcSetError(error,
                         "[libpq] Release the bound result stream before another query");
    return ADBC_STATUS_INVALID_STATE;
  }
  PqResultArrayReader reader(connection_->conn(), type_resolver_, query_,
                             connection_->temporal_representation());
  reader.SetAutocommit(connection_->autocommit());
  reader.SetConnection(connection_, std::move(lease));
  reader.SetBind(&bind_);
  reader.SetVendorName(connection_->VendorName());
  RAISE_STATUS(error, reader.ToArrayStream(rows_affected, stream));
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresStatement::ExecuteQuery(struct ArrowArrayStream* stream,
                                               int64_t* rows_affected,
                                               struct AdbcError* error) {
  if (!connection_->conn()) {
    InternalAdbcSetError(error, "[libpq] Connection was released");
    return ADBC_STATUS_INVALID_STATE;
  }
  ClearResult();
  if (bind_.release) {
    nanoarrow::UniqueSchema schema;
    ArrowError na_error{};
    CHECK_NA_DETAIL(INVALID_ARGUMENT,
                    ArrowArrayStreamGetSchema(&bind_, schema.get(), &na_error), &na_error,
                    error);
    bool has_super = false;
    CHECK_NA_DETAIL(
        INVALID_ARGUMENT,
        RedshiftSuperSchema::ValidateRoute(
            schema.get(), type_resolver_->is_redshift(), !ingest_.target.empty(),
            ingest_.mode == IngestMode::kReplace, &has_super, &na_error),
        &na_error, error);
  }

  // Use a dedicated path to handle bulk ingest
  if (!ingest_.target.empty()) {
    return ExecuteIngest(stream, rows_affected, error);
  }

  if (query_.empty()) {
    InternalAdbcSetError(error, "%s", "[libpq] Must SetSqlQuery before ExecuteQuery");
    return ADBC_STATUS_INVALID_STATE;
  }

  RAISE_ADBC(connection_->EnsureTransaction(error));

  // Use a dedicated path to handle parameter binding
  if (bind_.release != nullptr) {
    return ExecuteBind(stream, rows_affected, error);
  }

  const auto result_mode = adbc::driver::pgwire::SelectQueryResultMode(
      connection_->backend_profile(), use_copy(), stream != nullptr);

  // Text results are the portable PostgreSQL-wire path. Binary COPY is an
  // optional backend capability and remains PostgreSQL's default fast path.
  if (result_mode == adbc::driver::pgwire::QueryResultMode::kText) {
    PqResultArrayReader reader(connection_->conn(), type_resolver_, query_,
                               connection_->temporal_representation());
    reader.SetVendorName(connection_->VendorName());
    RAISE_STATUS(error, reader.ToArrayStream(rows_affected, stream));
    return ADBC_STATUS_OK;
  }

  PqResultHelper helper(connection_->conn(), query_, type_resolver_->is_redshift());
  RAISE_STATUS(error, helper.Prepare());
  RAISE_STATUS(error, helper.DescribePrepared());

  // Initialize the copy reader and infer the output schema (i.e., error for
  // unsupported types before issuing the COPY query). This could be lazier
  // (i.e., executed on the first call to GetSchema() or GetNext()).
  PostgresType root_type;
  RAISE_STATUS(error, helper.ResolveOutputTypes(*type_resolver_, &root_type,
                                                connection_->temporal_representation()));

  // If there will be no columns in the result, we can also avoid COPY
  if (root_type.n_children() == 0) {
    // Could/should move the helper into the reader instead of repreparing
    PqResultArrayReader reader(connection_->conn(), type_resolver_, query_,
                               connection_->temporal_representation());
    reader.SetVendorName(connection_->VendorName());
    RAISE_STATUS(error, reader.ToArrayStream(rows_affected, stream));
    return ADBC_STATUS_OK;
  }

  struct ArrowError na_error;
  reader_->copy_reader_ = std::make_unique<PostgresCopyStreamReader>();
  CHECK_NA(INTERNAL, reader_->copy_reader_->Init(root_type), error);
  CHECK_NA_DETAIL(INTERNAL,
                  reader_->copy_reader_->InferOutputSchema(
                      std::string(connection_->VendorName()), &na_error),
                  &na_error, error);

  CHECK_NA_DETAIL(INTERNAL, reader_->copy_reader_->InitFieldReaders(&na_error), &na_error,
                  error);

  // Execute the COPY query
  RAISE_STATUS(error, helper.ExecuteCopy());

  // We need the PQresult back for the reader
  reader_->result_.reset(helper.ReleaseResult());

  // Export to stream
  reader_->ExportTo(stream);
  if (rows_affected) *rows_affected = -1;
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresStatement::ExecuteSchema(struct ArrowSchema* schema,
                                                struct AdbcError* error) {
  RAISE_ADBC(connection_->CheckConnectionUsable(error));
  if (!connection_->conn() || connection_->HasActiveBoundStream()) {
    InternalAdbcSetError(
        error, "[libpq] Connection is closed or has an active bound result stream");
    return ADBC_STATUS_INVALID_STATE;
  }
  ClearResult();
  if (query_.empty()) {
    InternalAdbcSetError(error, "%s", "[libpq] Must SetSqlQuery before ExecuteQuery");
    return ADBC_STATUS_INVALID_STATE;
  }

  PqResultHelper helper(connection_->conn(), query_, type_resolver_->is_redshift());

  if (bind_.release) {
    nanoarrow::UniqueSchema param_schema;
    struct ArrowError na_error;
    ArrowErrorInit(&na_error);
    CHECK_NA_DETAIL(INTERNAL,
                    ArrowArrayStreamGetSchema(&bind_, param_schema.get(), &na_error),
                    &na_error, error);

    if (std::string(param_schema->format) != "+s") {
      InternalAdbcSetError(error, "%s", "[libpq] Bind parameters must have type STRUCT");
      return ADBC_STATUS_INVALID_STATE;
    }

    std::vector<Oid> param_oids(param_schema->n_children);
    for (int64_t i = 0; i < param_schema->n_children; i++) {
      PostgresType pg_type;
      CHECK_NA_DETAIL(INTERNAL,
                      PostgresType::FromSchema(*type_resolver_, param_schema->children[i],
                                               &pg_type, &na_error),
                      &na_error, error);
      param_oids[i] = pg_type.oid();
    }

    RAISE_STATUS(error, helper.Prepare(param_oids));
  } else {
    RAISE_STATUS(error, helper.Prepare());
  }

  RAISE_STATUS(error, helper.DescribePrepared());

  PostgresType output_type;
  RAISE_STATUS(error, helper.ResolveOutputTypes(*type_resolver_, &output_type,
                                                connection_->temporal_representation()));

  nanoarrow::UniqueSchema tmp;
  ArrowSchemaInit(tmp.get());
  CHECK_NA(INTERNAL,
           output_type.SetSchema(tmp.get(), std::string(connection_->VendorName())),
           error);

  tmp.move(schema);
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresStatement::ExecuteIngest(struct ArrowArrayStream* stream,
                                                int64_t* rows_affected,
                                                struct AdbcError* error) {
  const auto bulk_mode =
      adbc::driver::pgwire::SelectBulkIngestMode(connection_->backend_profile());
  if (bulk_mode == adbc::driver::pgwire::BulkIngestMode::kUnsupported) {
    InternalAdbcSetError(error, "[libpq] %s bulk ingestion is not implemented",
                         std::string(connection_->VendorName()).c_str());
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }

  RAISE_ADBC(connection_->EnsureTransaction(error));

  if (!bind_.release) {
    InternalAdbcSetError(error, "%s",
                         "[libpq] Must Bind() before Execute() for bulk ingestion");
    return ADBC_STATUS_INVALID_STATE;
  }

  if (stream != nullptr) {
    InternalAdbcSetError(error, "%s",
                         "[libpq] Bulk ingest with result set is not supported");
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }

  // Need the current schema to avoid being shadowed by temp tables
  // This is a little unfortunate; we need another DB roundtrip
  std::string current_schema;
  {
    PqResultHelper result_helper{connection_->conn(), "SELECT CURRENT_SCHEMA()",
                                 type_resolver_->is_redshift()};
    RAISE_STATUS(error, result_helper.Execute());
    auto it = result_helper.begin();
    if (it == result_helper.end()) {
      InternalAdbcSetError(
          error, "[libpq] PostgreSQL returned no rows for 'SELECT CURRENT_SCHEMA()'");
      return ADBC_STATUS_INTERNAL;
    }
    current_schema = (*it)[0].data;
  }

  BindStream bind_stream;
  bind_stream.generated_super_ingest = type_resolver_->is_redshift();
  bind_stream.SetBind(&bind_);
  std::memset(&bind_, 0, sizeof(bind_));
  std::string escaped_table;
  std::string escaped_field_list;
  PostgresType copy_target_types;
  bool has_copy_target_types = false;
  const bool own_replace_copy =
      ingest_.mode == IngestMode::kReplace && connection_->autocommit() &&
      connection_->backend_profile().transactions.transactional_ddl &&
      bulk_mode == adbc::driver::pgwire::BulkIngestMode::kBinaryCopy;
  const bool use_replace_savepoint =
      ingest_.mode == IngestMode::kReplace && !connection_->autocommit() &&
      connection_->backend_profile().transactions.transactional_ddl &&
      connection_->backend_profile().transactions.replace_savepoints &&
      bulk_mode == adbc::driver::pgwire::BulkIngestMode::kBinaryCopy;
  ScopedReplaceIngestTransaction replace_transaction(connection_.get(), own_replace_copy);
  ScopedReplaceIngestSavepoint replace_savepoint(connection_.get(),
                                                 use_replace_savepoint);
  std::function<Status()> before_replace_ddl;
  if (own_replace_copy || use_replace_savepoint) {
    if (rows_affected) *rows_affected = 0;
    before_replace_ddl = [&]() {
      UNWRAP_STATUS(replace_transaction.Begin());
      return replace_savepoint.Begin();
    };
  }

  bool copy_cleanup_failed = false;
  Status execution_status = [&]() -> Status {
    UNWRAP_STATUS(bind_stream.Begin([&]() -> Status {
      struct AdbcError tmp_error = ADBC_ERROR_INIT;
      AdbcStatusCode status_code =
          CreateBulkTable(current_schema, bind_stream.bind_schema.value, &escaped_table,
                          &escaped_field_list, &copy_target_types, &has_copy_target_types,
                          before_replace_ddl, &tmp_error);
      return Status::FromAdbc(status_code, tmp_error);
    }));

    if (bulk_mode == adbc::driver::pgwire::BulkIngestMode::kParameterizedInsert) {
      return bind_stream.ExecutePreparedRows(
          connection_->conn(), escaped_table, escaped_field_list,
          *connection_->type_resolver(), connection_->autocommit(),
          connection_->backend_profile().capabilities.parameterized_ingest_batch_rows,
          rows_affected, has_copy_target_types ? &copy_target_types : nullptr);
    }

    UNWRAP_STATUS(bind_stream.CheckTextEncoding(connection_->conn(), *type_resolver_));
    std::string query = "COPY ";
    query += escaped_table;
    query += " (";
    query += escaped_field_list;
    query += ") FROM STDIN WITH (FORMAT binary)";
    adbc::driver::pgwire::UniqueResult result(PQexec(connection_->conn(), query.c_str()));
    const ExecStatusType pg_status = PQresultStatus(result.get());
    if (pg_status != PGRES_COPY_IN) {
      // An unexpected COPY direction still owns an active wire protocol. Do
      // not let transaction finalization issue SQL on this connection.
      if (pg_status == PGRES_COPY_OUT || pg_status == PGRES_COPY_BOTH) {
        copy_cleanup_failed = true;
      }
      return MakeStatus(result.get(), "[libpq] COPY query failed: {}\nQuery was: {}",
                        PQerrorMessage(connection_->conn()), query);
    }
    return bind_stream.ExecuteCopy(connection_->conn(), *connection_->type_resolver(),
                                   has_copy_target_types ? &copy_target_types : nullptr,
                                   disable_decimal_fast_path_, rows_affected,
                                   &copy_cleanup_failed);
  }();

  if (copy_cleanup_failed || bind_stream.cleanup_failed) {
    connection_->MarkCopyIngestCleanupFailed();
    replace_transaction.Abandon();
    replace_savepoint.Abandon();
  } else if (!execution_status.ok()) {
    // COPY must already be terminal before this rollback. Keep its original
    // error if finalization fails and retires the connection too.
    (void)replace_transaction.Rollback();
    (void)replace_savepoint.Rollback();
  } else {
    execution_status = replace_transaction.Commit();
    if (execution_status.ok()) execution_status = replace_savepoint.Release();
  }
  if ((own_replace_copy || use_replace_savepoint) && !execution_status.ok() &&
      rows_affected) {
    *rows_affected = 0;
  }
  return execution_status.ToAdbc(error);
}

AdbcStatusCode PostgresStatement::GetOption(const char* key, char* value, size_t* length,
                                            struct AdbcError* error) {
  std::string result;
  if (std::strcmp(key, ADBC_INGEST_OPTION_TARGET_TABLE) == 0) {
    result = ingest_.target;
  } else if (std::strcmp(key, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA) == 0) {
    result = ingest_.db_schema;
  } else if (std::strcmp(key, ADBC_INGEST_OPTION_MODE) == 0) {
    switch (ingest_.mode) {
      case IngestMode::kCreate:
        result = ADBC_INGEST_OPTION_MODE_CREATE;
        break;
      case IngestMode::kAppend:
        result = ADBC_INGEST_OPTION_MODE_APPEND;
        break;
      case IngestMode::kReplace:
        result = ADBC_INGEST_OPTION_MODE_REPLACE;
        break;
      case IngestMode::kCreateAppend:
        result = ADBC_INGEST_OPTION_MODE_CREATE_APPEND;
        break;
    }
  } else if (std::strcmp(key, ADBC_POSTGRESQL_OPTION_BATCH_SIZE_HINT_BYTES) == 0) {
    result = std::to_string(reader_->batch_size_hint_bytes_);
  } else if (std::strcmp(key, ADBC_POSTGRESQL_OPTION_USE_COPY) == 0) {
    if (use_copy()) {
      result = "true";
    } else {
      result = "false";
    }
  } else if (std::strcmp(key, ADBC_POSTGRESQL_OPTION_DISABLE_DECIMAL_FAST_PATH) == 0) {
    result = disable_decimal_fast_path_ ? "true" : "false";
  } else {
    InternalAdbcSetError(error, "[libpq] Unknown statement option '%s'", key);
    return ADBC_STATUS_NOT_FOUND;
  }

  if (result.size() + 1 <= *length) {
    std::memcpy(value, result.data(), result.size() + 1);
  }
  *length = static_cast<int64_t>(result.size() + 1);
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresStatement::GetOptionBytes(const char* key, uint8_t* value,
                                                 size_t* length,
                                                 struct AdbcError* error) {
  InternalAdbcSetError(error, "[libpq] Unknown statement option '%s'", key);
  return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode PostgresStatement::GetOptionDouble(const char* key, double* value,
                                                  struct AdbcError* error) {
  InternalAdbcSetError(error, "[libpq] Unknown statement option '%s'", key);
  return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode PostgresStatement::GetOptionInt(const char* key, int64_t* value,
                                               struct AdbcError* error) {
  std::string result;
  if (std::strcmp(key, ADBC_POSTGRESQL_OPTION_BATCH_SIZE_HINT_BYTES) == 0) {
    *value = reader_->batch_size_hint_bytes_;
    return ADBC_STATUS_OK;
  }
  InternalAdbcSetError(error, "[libpq] Unknown statement option '%s'", key);
  return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode PostgresStatement::GetParameterSchema(struct ArrowSchema* schema,
                                                     struct AdbcError* error) {
  RAISE_ADBC(connection_->CheckConnectionUsable(error));
  if (!connection_->conn() || connection_->HasActiveBoundStream()) {
    InternalAdbcSetError(
        error, "[libpq] Connection is closed or has an active bound result stream");
    return ADBC_STATUS_INVALID_STATE;
  }
  if (query_.empty()) {
    InternalAdbcSetError(error, "[libpq] Must SetSqlQuery before GetParameterSchema");
    return ADBC_STATUS_INVALID_STATE;
  }

  PqResultHelper helper(connection_->conn(), query_, type_resolver_->is_redshift());
  RAISE_STATUS(error, helper.Prepare());
  RAISE_STATUS(error, helper.DescribePrepared());
  PostgresType param_types;
  RAISE_STATUS(error,
               helper.ResolveParamTypes(*connection_->type_resolver(), &param_types,
                                        connection_->temporal_representation()));

  nanoarrow::UniqueSchema tmp;
  ArrowSchemaInit(tmp.get());
  CHECK_NA(INTERNAL,
           param_types.SetSchema(tmp.get(), std::string(connection_->VendorName())),
           error);

  tmp.move(schema);
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresStatement::Prepare(struct AdbcError* error) {
  if (query_.empty()) {
    InternalAdbcSetError(error, "%s", "[libpq] Must SetSqlQuery() before Prepare()");
    return ADBC_STATUS_INVALID_STATE;
  }

  // Don't actually prepare until execution time, so we know the
  // parameter types
  prepared_ = true;
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresStatement::Release(struct AdbcError* error) {
  ClearResult();
  if (bind_.release) {
    bind_.release(&bind_);
  }
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresStatement::SetSqlQuery(const char* query,
                                              struct AdbcError* error) {
  ingest_.target.clear();
  ingest_.db_schema.clear();
  query_ = query;
  prepared_ = false;
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresStatement::SetOption(const char* key, const char* value,
                                            struct AdbcError* error) {
  if (std::strcmp(key, ADBC_INGEST_OPTION_TARGET_TABLE) == 0) {
    query_.clear();
    ingest_.target = value;
    prepared_ = false;
  } else if (std::strcmp(key, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA) == 0) {
    query_.clear();
    if (value == nullptr) {
      ingest_.db_schema.clear();
    } else {
      ingest_.db_schema = value;
    }
    prepared_ = false;
  } else if (std::strcmp(key, ADBC_INGEST_OPTION_MODE) == 0) {
    if (std::strcmp(value, ADBC_INGEST_OPTION_MODE_CREATE) == 0) {
      ingest_.mode = IngestMode::kCreate;
    } else if (std::strcmp(value, ADBC_INGEST_OPTION_MODE_APPEND) == 0) {
      ingest_.mode = IngestMode::kAppend;
    } else if (std::strcmp(value, ADBC_INGEST_OPTION_MODE_REPLACE) == 0) {
      ingest_.mode = IngestMode::kReplace;
    } else if (std::strcmp(value, ADBC_INGEST_OPTION_MODE_CREATE_APPEND) == 0) {
      ingest_.mode = IngestMode::kCreateAppend;
    } else {
      InternalAdbcSetError(error, "[libpq] Invalid value '%s' for option '%s'", value,
                           key);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    prepared_ = false;
  } else if (std::strcmp(key, ADBC_INGEST_OPTION_TEMPORARY) == 0) {
    if (std::strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0) {
      // https://github.com/apache/arrow-adbc/issues/1109: only clear the
      // schema if enabling since Python always sets the flag explicitly
      ingest_.temporary = true;
      ingest_.db_schema.clear();
    } else if (std::strcmp(value, ADBC_OPTION_VALUE_DISABLED) == 0) {
      ingest_.temporary = false;
    } else {
      InternalAdbcSetError(error, "[libpq] Invalid value '%s' for option '%s'", value,
                           key);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    prepared_ = false;
  } else if (std::strcmp(key, ADBC_POSTGRESQL_OPTION_BATCH_SIZE_HINT_BYTES) == 0) {
    int64_t int_value = std::atol(value);
    if (int_value <= 0) {
      InternalAdbcSetError(error, "[libpq] Invalid value '%s' for option '%s'", value,
                           key);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }

    this->batch_size_hint_bytes_ = this->reader_->batch_size_hint_bytes_ = int_value;
  } else if (std::strcmp(key, ADBC_POSTGRESQL_OPTION_USE_COPY) == 0) {
    if (std::strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0) {
      use_copy_ = true;
    } else if (std::strcmp(value, ADBC_OPTION_VALUE_DISABLED) == 0) {
      use_copy_ = false;
    } else {
      InternalAdbcSetError(error, "[libpq] Invalid value '%s' for option '%s'", value,
                           key);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
  } else if (std::strcmp(key, ADBC_POSTGRESQL_OPTION_DISABLE_DECIMAL_FAST_PATH) == 0) {
    if (std::strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0) {
      disable_decimal_fast_path_ = true;
    } else if (std::strcmp(value, ADBC_OPTION_VALUE_DISABLED) == 0) {
      disable_decimal_fast_path_ = false;
    } else {
      InternalAdbcSetError(error, "[libpq] Invalid value '%s' for option '%s'", value,
                           key);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
  } else {
    InternalAdbcSetError(error, "[libpq] Unknown statement option '%s'", key);
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresStatement::SetOptionBytes(const char* key, const uint8_t* value,
                                                 size_t length, struct AdbcError* error) {
  InternalAdbcSetError(error, "%s%s", "[libpq] Unknown statement option ", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode PostgresStatement::SetOptionDouble(const char* key, double value,
                                                  struct AdbcError* error) {
  InternalAdbcSetError(error, "%s%s", "[libpq] Unknown statement option ", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode PostgresStatement::SetOptionInt(const char* key, int64_t value,
                                               struct AdbcError* error) {
  if (std::strcmp(key, ADBC_POSTGRESQL_OPTION_BATCH_SIZE_HINT_BYTES) == 0) {
    if (value <= 0) {
      InternalAdbcSetError(error, "[libpq] Invalid value '%" PRIi64 "' for option '%s'",
                           value, key);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }

    this->batch_size_hint_bytes_ = this->reader_->batch_size_hint_bytes_ = value;
    return ADBC_STATUS_OK;
  }
  InternalAdbcSetError(error, "[libpq] Unknown statement option '%s'", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

void PostgresStatement::ClearResult() {
  // TODO: we may want to synchronize here for safety
  if (reader_) reader_->Release();
  reader_ = std::make_shared<TupleReader>(connection_->conn());
  reader_->batch_size_hint_bytes_ = batch_size_hint_bytes_;
}

}  // namespace adbcpq
