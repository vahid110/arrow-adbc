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

// Test-only bounded libpq recorder. Production translation units use renamed
// network calls in this target; PGresult memory/parsers are the real libpq.
// There is no socket, server, configurable backend or native producer claim.
#include <arrow-adbc/driver/redshift.h>
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "bind_stream.h"
#include "connection.h"
#include "driver/framework/utility.h"
#include "metadata.h"
#include "redshift_interval_write_test_fixture.h"
#include "redshift_numeric_test_fixture.h"
#include "redshift_timetz_test_fixture.h"

namespace {
struct RecordedParameter {
  Oid oid;
  bool null;
  std::string bytes;
};
struct Recording {
  std::vector<std::string> queries;
  std::vector<Oid> prepared;
  std::string prepared_query;
  std::vector<std::vector<RecordedParameter>> executions;
  PGTransactionStatusType transaction = PQTRANS_IDLE;
  bool exists = true;
  bool fail_rollback = false;
  bool timetz_target = false, mixed_target = false;
  std::optional<int32_t> timetz_raw = 3;
  int committed = 0, pending = 0;
} recording;
PGresult* Rows(int columns, const std::vector<std::vector<const char*>>& rows) {
  auto* out = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  std::vector<PGresAttDesc> attrs(columns);
  for (auto& attr : attrs) attr.name = const_cast<char*>("fixture");
  if (!PQsetResultAttrs(out, columns, attrs.data())) std::abort();
  for (size_t row = 0; row < rows.size(); ++row)
    for (int col = 0; col < columns; ++col) {
      auto* value = rows[row][col];
      if (!PQsetvalue(out, static_cast<int>(row), col, const_cast<char*>(value),
                      value ? static_cast<int>(std::strlen(value)) : -1))
        std::abort();
    }
  return out;
}
PGresult* Command() { return PQmakeEmptyPGresult(nullptr, PGRES_COMMAND_OK); }
PGresult* TimetzResult(bool empty = false) {
  auto* out = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  PGresAttDesc attr{};
  attr.name = const_cast<char*>("amount");
  attr.typid = 65113;
  attr.format = 1;
  attr.atttypmod = 3;
  if (!PQsetResultAttrs(out, 1, &attr)) std::abort();
  if (!empty) {
    const auto wire = adbcpq::testing::NumericHex("00000000000003e80000001e");
    if (!PQsetvalue(out, 0, 0, const_cast<char*>(wire.data()), 12) ||
        !PQsetvalue(out, 1, 0, nullptr, -1))
      std::abort();
  }
  return out;
}
PGresult* Query(const char* query) {
  std::string q(query);
  recording.queries.push_back(q);
  if (q.starts_with("SELECT owned_timetz")) return TimetzResult(q.ends_with("empty"));
  if (q == "SELECT version();")
    return Rows(1, {{"Redshift 1.0.00000 unqualified recording fixture"}});
  if (q.find("attrelid,") != std::string::npos) return Rows(3, {});
  if (q.find("SELECT oid, typname") != std::string::npos)
    return Rows(5, {{"64001", "intervaly2m", "unqualified_fixture_receive", "0", "0"},
                    {"65017", "intervald2s", "unqualified_fixture_receive", "0", "0"},
                    {"67009", "int8", "int8recv", "0", "0"},
                    {"65113", "timetz", "timetz_recv", "0", "0"},
                    {"75001", "bpchar", "bpcharrecv", "0", "0"}});
  if (q == "SELECT CURRENT_SCHEMA()") return Rows(1, {{"public"}});
  if (q.find("SHOW enable_case_sensitive_identifier") != std::string::npos)
    return Rows(1, {{"on"}});
  if (q.find("SELECT cls.oid FROM") != std::string::npos)
    return Rows(1, recording.exists ? std::vector<std::vector<const char*>>{{"12345"}}
                                    : std::vector<std::vector<const char*>>{});
  if (q.find("SELECT attr.attname, attr.atttypid") != std::string::npos ||
      q.find("SELECT attname, atttypid, atttypmod") != std::string::npos) {
    if (!recording.exists) return PQmakeEmptyPGresult(nullptr, PGRES_FATAL_ERROR);
    if (recording.timetz_target) {
      const auto raw =
          recording.timetz_raw ? std::to_string(*recording.timetz_raw) : std::string{};
      const char* code = recording.timetz_raw ? raw.c_str() : nullptr;
      return recording.mixed_target
                 ? Rows(3, {{"amount", "65113", code}, {"label", "75001", "6"}})
                 : Rows(3, {{"amount", "65113", code}});
    }
    return Rows(3, {{"amount", "65017", "-1"}});
  }
  if (q.find("SELECT current_database(), ns.nspname") != std::string::npos)
    return Rows(
        8, {{"fixturedb", "public", "owned", "12345", "1", "amount", "65017", "-1"}});
  if (q.find("FROM svv_columns") != std::string::npos)
    return Rows(5, {{"1", "amount", "DAY TO SECOND", "6", "3"}});
  if (q.starts_with("CREATE ")) {
    recording.exists = true;
    return Command();
  }
  if (q.starts_with("DROP ")) {
    recording.exists = false;
    return Command();
  }
  if (q == "BEGIN" || q == "BEGIN TRANSACTION") {
    recording.transaction = PQTRANS_INTRANS;
    recording.pending = 0;
    return Command();
  }
  if (q == "COMMIT") {
    recording.committed += recording.pending;
    recording.pending = 0;
    recording.transaction = PQTRANS_IDLE;
    return Command();
  }
  if (q == "ROLLBACK") {
    if (recording.fail_rollback) return PQmakeEmptyPGresult(nullptr, PGRES_FATAL_ERROR);
    recording.pending = 0;
    recording.transaction = PQTRANS_IDLE;
    return Command();
  }
  ADD_FAILURE() << "Unexpected recording query: " << q;
  return PQmakeEmptyPGresult(nullptr, PGRES_FATAL_ERROR);
}
PGresult* Parameters(int n, const Oid* types, const char* const* values,
                     const int* lengths, const int* formats) {
  std::vector<RecordedParameter> row;
  for (int col = 0; col < n; ++col) {
    EXPECT_EQ(formats[col], 1);
    row.push_back({types[col], values[col] == nullptr,
                   values[col] ? std::string(values[col], lengths[col]) : std::string{}});
  }
  recording.executions.push_back(std::move(row));
  if (recording.transaction == PQTRANS_INTRANS)
    ++recording.pending;
  else
    ++recording.committed;
  return Command();
}
}  // namespace

// These names are rewritten by this test target's definitions, never production.
extern "C" {
PGconn* PQconnectdb(const char*) { return reinterpret_cast<PGconn*>(new int(1)); }
ConnStatusType PQstatus(const PGconn*) { return CONNECTION_OK; }
void PQfinish(PGconn* conn) { delete reinterpret_cast<int*>(conn); }
const char* PQparameterStatus(const PGconn*, const char*) { return "UTF8"; }
PGcancel* PQgetCancel(PGconn*) { return reinterpret_cast<PGcancel*>(new int(1)); }
void PQfreeCancel(PGcancel* cancel) { delete reinterpret_cast<int*>(cancel); }
PQnoticeProcessor PQsetNoticeProcessor(PGconn*, PQnoticeProcessor, void*) {
  return nullptr;
}
PGTransactionStatusType PQtransactionStatus(const PGconn*) {
  return recording.transaction;
}
char* PQerrorMessage(const PGconn*) {
  static char message[] = "unqualified recorder failure";
  return message;
}
char* PQescapeIdentifier(PGconn*, const char* value, size_t size) {
  std::string quoted = "\"";
  for (size_t i = 0; i < size; ++i) {
    quoted += value[i];
    if (value[i] == '"') quoted += '"';
  }
  quoted += '"';
  auto* out = static_cast<char*>(std::malloc(quoted.size() + 1));
  std::memcpy(out, quoted.c_str(), quoted.size() + 1);
  return out;
}
PGresult* PQexec(PGconn*, const char* query) { return Query(query); }
PGresult* PQexecParams(PGconn*, const char* query, int n, const Oid* types,
                       const char* const* values, const int* lengths, const int* formats,
                       int) {
  if (std::string_view(query).starts_with("INSERT "))
    return Parameters(n, types, values, lengths, formats);
  return Query(query);
}
PGresult* PQprepare(PGconn*, const char*, const char* query, int n, const Oid* types) {
  recording.queries.emplace_back(query);
  recording.prepared_query = query;
  recording.prepared.clear();
  if (n) recording.prepared.assign(types, types + n);
  return Command();
}
PGresult* PQexecPrepared(PGconn*, const char*, int n, const char* const* values,
                         const int* lengths, const int* formats, int) {
  EXPECT_EQ(recording.prepared.size(), static_cast<size_t>(n));
  if (recording.prepared_query.starts_with("SELECT owned_timetz")) {
    if (n) {
      auto* captured = Parameters(n, recording.prepared.data(), values, lengths, formats);
      PQclear(captured);
    }
    return TimetzResult(recording.prepared_query.ends_with("empty"));
  }
  return Parameters(n, recording.prepared.data(), values, lengths, formats);
}
PGresult* PQdescribePrepared(PGconn*, const char*) {
  return recording.prepared_query.starts_with("SELECT owned_timetz") ? TimetzResult(true)
                                                                     : Command();
}
int PQnparams(const PGresult*) {
  return recording.prepared_query == "SELECT owned_timetz_param" ? 1 : 0;
}
Oid PQparamtype(const PGresult*, int) { return 65113; }
}

namespace adbcpq {
namespace {
class IntervalPublicRecordingTest : public ::testing::Test {
 public:
  void SetUp() override {
    recording = {};
    ASSERT_EQ(AdbcDriverRedshiftInit(ADBC_VERSION_1_1_0, &driver, &error),
              ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseNew(&database, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&database, "uri",
                                       "recording-fixture-never-a-network-uri", &error),
              ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&database, &error), ADBC_STATUS_OK) << Message();
    ASSERT_EQ(driver.ConnectionNew(&connection, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&connection, &database, &error), ADBC_STATUS_OK)
        << Message();
    ASSERT_EQ(driver.StatementNew(&connection, &statement, &error), ADBC_STATUS_OK);
    recording.queries.clear();
  }
  void TearDown() override {
    if (statement.private_data)
      EXPECT_EQ(driver.StatementRelease(&statement, &error), ADBC_STATUS_OK);
    if (connection.private_data)
      EXPECT_EQ(driver.ConnectionRelease(&connection, &error), ADBC_STATUS_OK);
    if (database.private_data)
      EXPECT_EQ(driver.DatabaseRelease(&database, &error), ADBC_STATUS_OK);
    if (driver.release) EXPECT_EQ(driver.release(&driver, &error), ADBC_STATUS_OK);
    if (error.release) error.release(&error);
  }
  const char* Message() { return error.message ? error.message : ""; }
  void Input(nanoarrow::UniqueSchema* schema, nanoarrow::UniqueArray* array,
             const std::vector<std::optional<int64_t>>& values,
             const char* qualifier = "SECOND", const char* precision = "3") {
    ASSERT_EQ(
        testing::IntervalWriteSchema(schema->get(), RedshiftIntervalFamily::kDaySecond,
                                     "known", qualifier, precision),
        0);
    ASSERT_EQ(testing::IntervalWriteBatch(schema->get(), array->get(), values, nullptr),
              0);
  }
  AdbcDriver driver{};
  AdbcDatabase database{};
  AdbcConnection connection{};
  AdbcStatement statement{};
  AdbcError error = ADBC_ERROR_INIT;
};
TEST_F(IntervalPublicRecordingTest, PublicArrayAndStreamUseExactSourceOidAndWire) {
  for (bool stream : {false, true}) {
    nanoarrow::UniqueSchema schema;
    nanoarrow::UniqueArray array;
    Input(&schema, &array, {1000, std::nullopt, -2000});
    ASSERT_EQ(
        driver.StatementSetSqlQuery(&statement, "INSERT INTO owned VALUES ($1)", &error),
        ADBC_STATUS_OK);
    if (stream) {
      nanoarrow::UniqueArrayStream input;
      adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
      ASSERT_EQ(driver.StatementBindStream(&statement, input.get(), &error),
                ADBC_STATUS_OK)
          << Message();
      EXPECT_FALSE(input->release);
    } else {
      ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                ADBC_STATUS_OK)
          << Message();
      EXPECT_FALSE(array->release);
      EXPECT_FALSE(schema->release);
    }
    ASSERT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ADBC_STATUS_OK)
        << Message();
  }
  ASSERT_EQ(recording.executions.size(), 6);
  for (size_t i = 0; i < 6; ++i) {
    const auto& parameter = recording.executions[i][0];
    EXPECT_EQ(parameter.oid, 65017U);
    EXPECT_EQ(parameter.null, i % 3 == 1);
    if (i % 3 != 1)
      EXPECT_EQ(parameter.bytes, testing::NumericHex(i % 3 == 0 ? "00000000000003e8"
                                                                : "fffffffffffff830"));
  }
}
TEST_F(IntervalPublicRecordingTest,
       SchemaInvalidNullNeverTransfersArrayOrStreamOwnership) {
  for (bool stream : {false, true})
    for (auto marker : {std::pair{"ARROW:extension:name", "foreign"},
                        {"ARROW:extension:metadata", "{}"},
                        {"ADBC:redshift:sql_type", "HLLSKETCH"},
                        {"ADBC:redshift:type_modifier", "-1"}}) {
      nanoarrow::UniqueSchema schema;
      ArrowSchemaInit(schema.get());
      ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
      ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_NA), 0);
      nanoarrow::UniqueArray array;
      ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
      ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
      ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
      ASSERT_EQ(
          testing::IntervalSetMetadata(schema->children[0], marker.first, marker.second),
          0);
      auto schema_releases = std::make_shared<int>(0);
      auto array_releases = std::make_shared<int>(0);
      testing::DecimalCountReleases(schema.get(), schema_releases);
      testing::DecimalCountReleases(array.get(), array_releases);
      ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                          "owned", &error),
                ADBC_STATUS_OK);
      ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                          ADBC_INGEST_OPTION_MODE_REPLACE, &error),
                ADBC_STATUS_OK);
      recording.committed = 7;
      if (stream) {
        nanoarrow::UniqueArrayStream input;
        ASSERT_EQ(ArrowBasicArrayStreamInit(input.get(), schema.get(), 1), 0);
        ArrowBasicArrayStreamSetArray(input.get(), 0, array.get());
        EXPECT_EQ(driver.StatementBindStream(&statement, input.get(), &error),
                  ADBC_STATUS_INVALID_ARGUMENT);
        EXPECT_TRUE(input->release);
        EXPECT_EQ(*schema_releases, 0);
        EXPECT_EQ(*array_releases, 0);
        input->release(input.get());
      } else {
        EXPECT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                  ADBC_STATUS_INVALID_ARGUMENT);
        EXPECT_TRUE(array->release);
        EXPECT_TRUE(schema->release);
        EXPECT_EQ(*schema_releases, 0);
        EXPECT_EQ(*array_releases, 0);
        array->release(array.get());
        schema->release(schema.get());
      }
      EXPECT_EQ(*schema_releases, 1);
      EXPECT_EQ(*array_releases, 1);
      EXPECT_EQ(recording.committed, 7);
      EXPECT_TRUE(recording.queries.empty());
      EXPECT_TRUE(recording.executions.empty());
    }
}
TEST_F(IntervalPublicRecordingTest,
       ExactDdlAndExistingCreateAppendDoNotAuthorizeUnknownTargets) {
  for (const char* mode :
       {ADBC_INGEST_OPTION_MODE_CREATE, ADBC_INGEST_OPTION_MODE_REPLACE,
        ADBC_INGEST_OPTION_MODE_APPEND, ADBC_INGEST_OPTION_MODE_CREATE_APPEND}) {
    for (bool exists : {false, true}) {
      recording.queries.clear();
      recording.exists = exists;
      ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                          "owned", &error),
                ADBC_STATUS_OK);
      ASSERT_EQ(
          driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE, mode, &error),
          ADBC_STATUS_OK);
      nanoarrow::UniqueSchema schema;
      nanoarrow::UniqueArray array;
      Input(&schema, &array, {1000});
      ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                ADBC_STATUS_OK);
      EXPECT_NE(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
                ADBC_STATUS_OK);
      bool create = false, drop = false;
      for (const auto& query : recording.queries) {
        if (query.starts_with("CREATE ")) {
          create = true;
          EXPECT_NE(query.find("INTERVAL SECOND(3)"), std::string::npos);
          EXPECT_EQ(query.find("IF NOT EXISTS"), std::string::npos);
        }
        drop |= query.starts_with("DROP ");
      }
      EXPECT_EQ(create,
                std::string_view(mode) != ADBC_INGEST_OPTION_MODE_APPEND &&
                    !(std::string_view(mode) == ADBC_INGEST_OPTION_MODE_CREATE_APPEND &&
                      exists));
      EXPECT_EQ(drop, std::string_view(mode) == ADBC_INGEST_OPTION_MODE_REPLACE);
      EXPECT_TRUE(recording.executions.empty());
      if (exists || std::string_view(mode) != ADBC_INGEST_OPTION_MODE_APPEND)
        EXPECT_NE(std::string_view(Message()).find("unknown"), std::string::npos)
            << Message();
    }
  }
}
TEST_F(IntervalPublicRecordingTest,
       UnknownActualTargetsRejectEmptyAndNullSourcesWithoutPayload) {
  for (bool stream : {false, true})
    for (bool unmarked : {false, true})
      for (int count : {0, 2}) {
        recording.queries.clear();
        recording.executions.clear();
        ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                            "owned", &error),
                  ADBC_STATUS_OK);
        ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                            ADBC_INGEST_OPTION_MODE_APPEND, &error),
                  ADBC_STATUS_OK);
        nanoarrow::UniqueSchema schema;
        nanoarrow::UniqueArray array;
        if (unmarked) {
          ArrowSchemaInit(schema.get());
          ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
          ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_NA), 0);
          ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "amount"), 0);
          ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
          ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
          for (int row = 0; row < count; ++row) {
            ASSERT_EQ(ArrowArrayAppendNull(array->children[0], 1), 0);
            ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
          }
          ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
        } else
          Input(&schema, &array, std::vector<std::optional<int64_t>>(count));
        if (stream) {
          nanoarrow::UniqueArrayStream input;
          ASSERT_EQ(ArrowBasicArrayStreamInit(input.get(), schema.get(), 1), 0);
          ArrowBasicArrayStreamSetArray(input.get(), 0, array.get());
          ASSERT_EQ(driver.StatementBindStream(&statement, input.get(), &error),
                    ADBC_STATUS_OK)
              << Message();
        } else {
          ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                    ADBC_STATUS_OK)
              << Message();
        }
        EXPECT_NE(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
                  ADBC_STATUS_OK);
        EXPECT_TRUE(recording.executions.empty());
        EXPECT_EQ(recording.committed, 0);
      }
}
TEST_F(IntervalPublicRecordingTest,
       KnownReferenceTargetUsesWholeStreamOwnerAndPreservesPrimaryError) {
  // Internal labeled constraints exercise the same ownership/writer/transaction
  // implementation; they do not qualify the native catalog producer above.
  for (bool owned : {false, true})
    for (bool fail_rollback : {false, true}) {
      recording.queries.clear();
      recording.executions.clear();
      recording.committed = 7;
      recording.pending = 0;
      recording.transaction = owned ? PQTRANS_IDLE : PQTRANS_INTRANS;
      recording.fail_rollback = fail_rollback;
      nanoarrow::UniqueSchema schema;
      nanoarrow::UniqueArray array;
      Input(&schema, &array, {1000});
      nanoarrow::UniqueArray late;
      ASSERT_EQ(testing::IntervalWriteBatch(schema.get(), late.get(), {1234}, nullptr),
                0);
      nanoarrow::UniqueArrayStream input;
      ASSERT_EQ(ArrowBasicArrayStreamInit(input.get(), schema.get(), 2), 0);
      ArrowBasicArrayStreamSetArray(input.get(), 0, array.get());
      ArrowBasicArrayStreamSetArray(input.get(), 1, late.get());
      BindStream bind;
      bind.SetBind(input.get());
      ASSERT_TRUE(bind.Begin([] { return Status::Ok(); }).ok());
      auto* impl =
          static_cast<std::shared_ptr<PostgresConnection>*>(connection.private_data);
      PostgresType target;
      ASSERT_EQ((*impl)->type_resolver()->Find(65017, &target, nullptr), 0);
      target = target.WithIntervalConstraint(testing::IntervalReferenceConstraint(
          RedshiftIntervalFamily::kDaySecond, RedshiftIntervalQualifier::kSecond, 3));
      PostgresType targets(PostgresTypeId::kRecord);
      targets.AppendChild("amount", target);
      int64_t rows = -1;
      auto status =
          bind.ExecutePreparedRows((*impl)->conn(), "owned", "amount",
                                   *(*impl)->type_resolver(), owned, 1, &rows, &targets);
      EXPECT_FALSE(status.ok());
      AdbcError primary = ADBC_ERROR_INIT;
      status.ToAdbc(&primary);
      ASSERT_NE(primary.message, nullptr);
      EXPECT_NE(std::string_view(primary.message).find("inexact qualifier"),
                std::string::npos);
      if (primary.release) primary.release(&primary);
      EXPECT_EQ(recording.executions.size(), 1);
      EXPECT_EQ(recording.committed, 7);
      EXPECT_EQ(bind.cleanup_failed, owned && fail_rollback);
      EXPECT_EQ(rows, owned ? 0 : 1);
      EXPECT_EQ(recording.pending, owned && !fail_rollback ? 0 : 1);
    }
}

TEST_F(IntervalPublicRecordingTest,
       TimetzPublicArrayStreamAndRelationOffsetsUseExactWire) {
  for (bool stream : {false, true}) {
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(testing::TimetzWriteSchema(schema.get()), 0);
    std::vector<testing::TimetzValue> rows;
    for (int row = 0; row < 12; ++row)
      rows.emplace_back(std::pair<int64_t, int32_t>{row * 1000, 100 + row});
    nanoarrow::UniqueArray array;
    ASSERT_EQ(testing::TimetzWriteBatch(schema.get(), array.get(), rows, nullptr), 0);
    // Independent root, TIMETZ parent and primitive child offsets. Logical
    // rows0/1 become civil5000/6000 and east106/107, not duplicated offsets.
    array->offset = 2;
    array->length = 2;
    auto* field = array->children[0];
    field->offset = 1;
    field->length = 7;
    field->children[0]->offset = 2;
    field->children[0]->length = 10;
    field->children[1]->offset = 3;
    field->children[1]->length = 9;
    ASSERT_EQ(
        driver.StatementSetSqlQuery(&statement, "INSERT INTO owned VALUES ($1)", &error),
        ADBC_STATUS_OK);
    if (stream) {
      nanoarrow::UniqueArrayStream input;
      ASSERT_EQ(ArrowBasicArrayStreamInit(input.get(), schema.get(), 1), 0);
      ArrowBasicArrayStreamSetArray(input.get(), 0, array.get());
      ASSERT_EQ(driver.StatementBindStream(&statement, input.get(), &error),
                ADBC_STATUS_OK)
          << Message();
    } else
      ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                ADBC_STATUS_OK)
          << Message();
    ASSERT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ADBC_STATUS_OK)
        << Message();
  }
  ASSERT_EQ(recording.executions.size(), 4);
  for (size_t row = 0; row < 4; ++row) {
    ASSERT_EQ(recording.executions[row].size(), 1);
    EXPECT_EQ(recording.executions[row][0].oid, testing::kTimetzOid);
    EXPECT_EQ(recording.executions[row][0].bytes,
              testing::NumericHex(row % 2 == 0 ? "0000000000001388ffffff96"
                                               : "0000000000001770ffffff95"));
  }
}
TEST_F(IntervalPublicRecordingTest,
       TimetzSchemaFailuresPreserveOwnershipAndReplaceSentinel) {
  for (bool stream : {false, true})
    for (int mutation = 0; mutation < 10; ++mutation) {
      nanoarrow::UniqueSchema schema;
      ASSERT_EQ(testing::TimetzWriteSchema(schema.get(), mutation == 0 ? nullptr : "6"),
                0);
      nanoarrow::UniqueArray array;
      ASSERT_EQ(
          testing::TimetzWriteBatch(schema.get(), array.get(), {std::nullopt}, nullptr),
          0);
      auto* field = schema->children[0];
      if (mutation == 1)
        ASSERT_EQ(testing::IntervalSetMetadata(field, "ARROW:extension:name",
                                               "adbc.redshift.timetz_us_offset.v2"),
                  0);
      if (mutation == 2)
        ASSERT_EQ(testing::IntervalSetMetadata(field, "ARROW:extension:metadata", "{}"),
                  0);
      if (mutation == 3)
        ASSERT_EQ(
            testing::IntervalSetMetadata(field, "ADBC:redshift:sql_type", "HLLSKETCH"),
            0);
      if (mutation == 4) field->children[0]->flags |= ARROW_FLAG_NULLABLE;
      if (mutation == 5)
        ASSERT_EQ(ArrowSchemaSetName(field->children[0], "offset_seconds"), 0);
      if (mutation == 6)
        ASSERT_EQ(testing::IntervalSetMetadata(field->children[0],
                                               "ARROW:extension:metadata", "{}"),
                  0);
      if (mutation == 7)
        ASSERT_EQ(
            testing::DecimalMetadata(field, "ADBC:redshift:fractional_precision", "6"),
            0);
      if (mutation == 8)
        ASSERT_EQ(
            testing::IntervalSetMetadata(field, "ADBC:redshift:type_modifier", "999"), 0);
      if (mutation == 9) {
        ASSERT_EQ(ArrowSchemaAllocateDictionary(field), 0);
        ArrowSchemaInitFromType(field->dictionary, NANOARROW_TYPE_INT32);
      }
      auto schema_count = std::make_shared<int>(0),
           array_count = std::make_shared<int>(0);
      testing::DecimalCountReleases(schema.get(), schema_count);
      testing::DecimalCountReleases(array.get(), array_count);
      ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                          "owned", &error),
                ADBC_STATUS_OK);
      ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                          ADBC_INGEST_OPTION_MODE_REPLACE, &error),
                ADBC_STATUS_OK);
      recording.committed = 7;
      if (stream) {
        nanoarrow::UniqueArrayStream input;
        ASSERT_EQ(ArrowBasicArrayStreamInit(input.get(), schema.get(), 1), 0);
        ArrowBasicArrayStreamSetArray(input.get(), 0, array.get());
        EXPECT_NE(driver.StatementBindStream(&statement, input.get(), &error),
                  ADBC_STATUS_OK);
        EXPECT_TRUE(input->release);
        EXPECT_EQ(*schema_count, 0);
        EXPECT_EQ(*array_count, 0);
        input->release(input.get());
      } else {
        EXPECT_NE(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                  ADBC_STATUS_OK);
        EXPECT_TRUE(array->release);
        EXPECT_TRUE(schema->release);
        EXPECT_EQ(*schema_count, 0);
        EXPECT_EQ(*array_count, 0);
        array->release(array.get());
        schema->release(schema.get());
      }
      EXPECT_EQ(*schema_count, 1);
      EXPECT_EQ(*array_count, 1);
      EXPECT_EQ(recording.committed, 7);
      EXPECT_TRUE(recording.queries.empty());
      EXPECT_TRUE(recording.executions.empty());
    }
}
TEST_F(IntervalPublicRecordingTest, TimetzKnownTargetNullEmptyAndUnknownPrecisionGate) {
  recording.timetz_target = true;
  for (bool stream : {false, true})
    for (bool unmarked : {false, true})
      for (int count : {0, 2})
        for (auto raw : {std::optional<int32_t>{}, std::optional<int32_t>{999},
                         std::optional<int32_t>{3}}) {
          recording.executions.clear();
          recording.timetz_raw = raw;
          ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                              "owned", &error),
                    ADBC_STATUS_OK);
          ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                              ADBC_INGEST_OPTION_MODE_APPEND, &error),
                    ADBC_STATUS_OK);
          nanoarrow::UniqueSchema schema;
          nanoarrow::UniqueArray array;
          if (unmarked) {
            ArrowSchemaInit(schema.get());
            ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
            ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "amount"), 0);
            ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_NA), 0);
            ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
            ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
            for (int row = 0; row < count; ++row) {
              ASSERT_EQ(ArrowArrayAppendNull(array->children[0], 1), 0);
              ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
            }
            ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
          } else {
            ASSERT_EQ(testing::TimetzWriteSchema(schema.get(), "3"), 0);
            ASSERT_EQ(testing::TimetzWriteBatch(schema.get(), array.get(),
                                                std::vector<testing::TimetzValue>(count),
                                                nullptr),
                      0);
          }
          if (stream) {
            nanoarrow::UniqueArrayStream input;
            ASSERT_EQ(ArrowBasicArrayStreamInit(input.get(), schema.get(), 1), 0);
            ArrowBasicArrayStreamSetArray(input.get(), 0, array.get());
            ASSERT_EQ(driver.StatementBindStream(&statement, input.get(), &error),
                      ADBC_STATUS_OK);
          } else
            ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                      ADBC_STATUS_OK);
          const auto status =
              driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error);
          EXPECT_EQ(status == ADBC_STATUS_OK, raw == std::optional<int32_t>{3})
              << Message();
          if (!raw || *raw != 3) EXPECT_TRUE(recording.executions.empty());
          for (const auto& execution : recording.executions)
            for (const auto& parameter : execution) {
              EXPECT_EQ(parameter.oid, testing::kTimetzOid);
              EXPECT_TRUE(parameter.null);
            }
        }
}
TEST_F(IntervalPublicRecordingTest,
       TimetzCreateAppendExistingSkipsUnusedDeclarationsMissingRejects) {
  recording.timetz_target = true;
  recording.mixed_target = true;
  for (bool exists : {false, true}) {
    recording.exists = exists;
    recording.queries.clear();
    recording.executions.clear();
    nanoarrow::UniqueSchema simple;
    ASSERT_EQ(testing::TimetzWriteSchema(simple.get(), "3"), 0);
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 2), 0);
    ArrowSchemaMove(simple->children[0], schema->children[0]);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[1], "label"), 0);
    ASSERT_EQ(testing::IntervalSetMetadata(schema->children[1], "ADBC:redshift:sql_type",
                                           "CHAR"),
              0);
    nanoarrow::UniqueArray array;
    ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
    ASSERT_EQ(ArrowArrayAppendInt(array->children[0]->children[0], 1000), 0);
    ASSERT_EQ(ArrowArrayAppendInt(array->children[0]->children[1], 30), 0);
    ASSERT_EQ(ArrowArrayFinishElement(array->children[0]), 0);
    ASSERT_EQ(ArrowArrayAppendString(array->children[1], ArrowCharView("a")), 0);
    ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
    ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
    ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                        "owned", &error),
              ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                        ADBC_INGEST_OPTION_MODE_CREATE_APPEND, &error),
              ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
              ADBC_STATUS_OK)
        << Message();
    EXPECT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error) ==
                  ADBC_STATUS_OK,
              exists)
        << Message();
    for (const auto& query : recording.queries) {
      EXPECT_FALSE(query.starts_with("CREATE "));
      EXPECT_FALSE(query.starts_with("DROP "));
    }
    EXPECT_EQ(recording.executions.size(), exists ? 1 : 0);
    if (exists) {
      ASSERT_EQ(recording.executions[0].size(), 2);
      EXPECT_EQ(recording.executions[0][0].oid, testing::kTimetzOid);
      EXPECT_EQ(recording.executions[0][1].bytes, "a");
    }
  }
}
TEST_F(IntervalPublicRecordingTest,
       TimetzCreateReplaceBareP6AndExplicitLowerPrecisionPreDdlGate) {
  recording.timetz_target = true;
  for (const char* mode :
       {ADBC_INGEST_OPTION_MODE_CREATE, ADBC_INGEST_OPTION_MODE_REPLACE,
        ADBC_INGEST_OPTION_MODE_CREATE_APPEND})
    for (int p : {3, 6}) {
      recording.exists = false;
      recording.queries.clear();
      recording.executions.clear();
      const auto precision = std::to_string(p);
      nanoarrow::UniqueSchema schema;
      ASSERT_EQ(testing::TimetzWriteSchema(schema.get(), precision.c_str()), 0);
      nanoarrow::UniqueArray array;
      ASSERT_EQ(
          testing::TimetzWriteBatch(schema.get(), array.get(), {{{1000, 30}}}, nullptr),
          0);
      ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                          "owned", &error),
                ADBC_STATUS_OK);
      ASSERT_EQ(
          driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE, mode, &error),
          ADBC_STATUS_OK);
      ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                ADBC_STATUS_OK);
      EXPECT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error) ==
                    ADBC_STATUS_OK,
                p == 6)
          << Message();
      bool create = false, drop = false;
      for (const auto& query : recording.queries) {
        if (query.starts_with("CREATE ")) {
          create = true;
          EXPECT_NE(query.find(" TIMETZ"), std::string::npos);
          EXPECT_EQ(query.find("TIMETZ("), std::string::npos);
        }
        drop |= query.starts_with("DROP ");
      }
      EXPECT_EQ(create, p == 6);
      EXPECT_EQ(drop,
                p == 6 && std::string_view(mode) == ADBC_INGEST_OPTION_MODE_REPLACE);
    }
}
TEST_F(IntervalPublicRecordingTest, TimetzLateRuntimeErrorUsesExistingWholeStreamOwner) {
  recording.timetz_target = true;
  for (bool owned : {false, true})
    for (bool fail_rollback : {false, true}) {
      recording.executions.clear();
      recording.queries.clear();
      recording.committed = 7;
      recording.pending = owned ? 0 : 2;
      recording.transaction = owned ? PQTRANS_IDLE : PQTRANS_INTRANS;
      recording.fail_rollback = fail_rollback;
      if (!owned)
        ASSERT_EQ(
            driver.ConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                                       ADBC_OPTION_VALUE_DISABLED, &error),
            ADBC_STATUS_OK);
      ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                          "owned", &error),
                ADBC_STATUS_OK);
      ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                          ADBC_INGEST_OPTION_MODE_APPEND, &error),
                ADBC_STATUS_OK);
      nanoarrow::UniqueSchema schema;
      ASSERT_EQ(testing::TimetzWriteSchema(schema.get(), "3"), 0);
      nanoarrow::UniqueArray first, late;
      ASSERT_EQ(testing::TimetzWriteBatch(schema.get(), first.get(),
                                          std::vector<testing::TimetzValue>(
                                              16, std::pair<int64_t, int32_t>{1000, 30}),
                                          nullptr),
                0);
      ASSERT_EQ(
          testing::TimetzWriteBatch(schema.get(), late.get(), {{{1234, 30}}}, nullptr),
          0);
      nanoarrow::UniqueArrayStream input;
      ASSERT_EQ(ArrowBasicArrayStreamInit(input.get(), schema.get(), 2), 0);
      ArrowBasicArrayStreamSetArray(input.get(), 0, first.get());
      ArrowBasicArrayStreamSetArray(input.get(), 1, late.get());
      ASSERT_EQ(driver.StatementBindStream(&statement, input.get(), &error),
                ADBC_STATUS_OK);
      int64_t rows = -1;
      EXPECT_NE(driver.StatementExecuteQuery(&statement, nullptr, &rows, &error),
                ADBC_STATUS_OK);
      EXPECT_NE(std::string_view(Message()).find("inexact civil precision"),
                std::string::npos);
      EXPECT_EQ(recording.executions.size(), 1);
      EXPECT_EQ(recording.committed, 7);
      auto* impl =
          static_cast<std::shared_ptr<PostgresConnection>*>(connection.private_data);
      EXPECT_EQ((*impl)->unusable(), owned && fail_rollback);
      EXPECT_EQ(rows, owned ? 0 : 16);
      EXPECT_EQ(recording.pending, owned ? (fail_rollback ? 1 : 0) : 3);
      // Each uncertain-owned failure retires its connection; don't reuse it for
      // another scenario. A separate test instance covers the final owned case.
      if (owned && fail_rollback) break;
      if (!owned) {
        recording.transaction = PQTRANS_IDLE;
        recording.pending = 0;
        ASSERT_EQ(
            driver.ConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                                       ADBC_OPTION_VALUE_ENABLED, &error),
            ADBC_STATUS_OK);
      }
    }
}
TEST_F(IntervalPublicRecordingTest, TimetzPublicResultSchemaBoundAndEmptyUseSameAdapter) {
  for (int path = 0; path < 3; ++path) {
    ASSERT_EQ(
        driver.StatementSetSqlQuery(
            &statement, path == 2 ? "SELECT owned_timetz_empty" : "SELECT owned_timetz",
            &error),
        ADBC_STATUS_OK);
    if (path == 1) {
      nanoarrow::UniqueSchema schema;
      ASSERT_EQ(testing::TimetzWriteSchema(schema.get()), 0);
      nanoarrow::UniqueArray array;
      ASSERT_EQ(
          testing::TimetzWriteBatch(schema.get(), array.get(), {{{1000, 30}}}, nullptr),
          0);
      ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                ADBC_STATUS_OK);
    }
    nanoarrow::UniqueSchema described;
    ASSERT_EQ(driver.StatementExecuteSchema(&statement, described.get(), &error),
              ADBC_STATUS_OK)
        << Message();
    RedshiftTimetzSchema parsed;
    ASSERT_EQ(
        RedshiftTimetzSchema::Parse(described->children[0], true, &parsed, nullptr, true),
        0);
    EXPECT_EQ(parsed.precision, 3);
    nanoarrow::UniqueArrayStream output;
    ASSERT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
              ADBC_STATUS_OK)
        << Message();
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(ArrowArrayStreamGetSchema(output.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(
        RedshiftTimetzSchema::Parse(schema->children[0], true, &parsed, nullptr, true),
        0);
    EXPECT_EQ(parsed.precision, 3);
    int rows = 0;
    while (true) {
      nanoarrow::UniqueArray batch;
      ASSERT_EQ(ArrowArrayStreamGetNext(output.get(), batch.get(), nullptr), 0);
      if (!batch->release) break;
      nanoarrow::UniqueArrayView view;
      ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), nullptr), 0);
      ASSERT_EQ(ArrowArrayViewSetArray(view.get(), batch.get(), nullptr), 0);
      for (int64_t i = 0; i < batch->length; ++i, ++rows) {
        EXPECT_EQ(ArrowArrayViewIsNull(view->children[0], i), rows % 2 == 1);
        if (rows % 2 == 0) {
          EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view->children[0]->children[0], i), 1000);
          EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view->children[0]->children[1], i), -30);
        }
      }
    }
    EXPECT_EQ(rows, path == 2 ? 0 : 2);
  }
}
TEST_F(IntervalPublicRecordingTest,
       TimetzOidOnlyParameterAndTableDescriptorsKeepUnknowns) {
  ASSERT_EQ(driver.StatementSetSqlQuery(&statement, "SELECT owned_timetz_param", &error),
            ADBC_STATUS_OK);
  nanoarrow::UniqueSchema parameters;
  ASSERT_EQ(driver.StatementGetParameterSchema(&statement, parameters.get(), &error),
            ADBC_STATUS_OK)
      << Message();
  ASSERT_EQ(parameters->n_children, 1);
  RedshiftTimetzSchema parsed;
  ASSERT_EQ(
      RedshiftTimetzSchema::Parse(parameters->children[0], true, &parsed, nullptr, true),
      0);
  EXPECT_TRUE(parsed.enabled);
  EXPECT_FALSE(parsed.precision);
  EXPECT_NE(RedshiftTimetzSchema::Parse(parameters->children[0], true, &parsed, nullptr),
            0);
  recording.timetz_target = true;
  for (auto raw : {std::optional<int32_t>{}, std::optional<int32_t>{999},
                   std::optional<int32_t>{3}}) {
    recording.timetz_raw = raw;
    nanoarrow::UniqueSchema table;
    ASSERT_EQ(driver.ConnectionGetTableSchema(&connection, nullptr, "public", "owned",
                                              table.get(), &error),
              ADBC_STATUS_OK)
        << Message();
    ASSERT_EQ(
        RedshiftTimetzSchema::Parse(table->children[0], true, &parsed, nullptr, true), 0);
    EXPECT_EQ(parsed.precision, RedshiftTimetzSchema::ReferencePrecision(raw));
  }
}
TEST_F(IntervalPublicRecordingTest, TimetzMaterializedResultSurvivesOwnerRelease) {
  ASSERT_EQ(driver.StatementSetSqlQuery(&statement, "SELECT owned_timetz", &error),
            ADBC_STATUS_OK);
  nanoarrow::UniqueArrayStream output;
  ASSERT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
            ADBC_STATUS_OK)
      << Message();
  ASSERT_EQ(driver.StatementRelease(&statement, &error), ADBC_STATUS_OK);
  ASSERT_EQ(driver.ConnectionRelease(&connection, &error), ADBC_STATUS_OK);
  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(ArrowArrayStreamGetSchema(output.get(), schema.get(), nullptr), 0);
  nanoarrow::UniqueArray batch;
  ASSERT_EQ(ArrowArrayStreamGetNext(output.get(), batch.get(), nullptr), 0);
  ASSERT_TRUE(batch->release);
  EXPECT_EQ(batch->length, 2);
  batch.reset();
  ASSERT_EQ(ArrowArrayStreamGetNext(output.get(), batch.get(), nullptr), 0);
  EXPECT_FALSE(batch->release);
}
TEST_F(IntervalPublicRecordingTest, TimetzGetObjectsParserEmitsOnlyKnownPrecision) {
  PqResultHelper columns(nullptr, "");
  columns.SetResult(Rows(5, {{"a", "1", nullptr, "timetz", nullptr},
                             {"b", "2", nullptr, "timetz", "999"},
                             {"c", "3", nullptr, "timetz", "3"},
                             {"d", "4", nullptr, "timetz", "-1"}}));
  for (int row = 0; row < 4; ++row) {
    auto parsed = ResolveColumnMetadata(columns.Row(row),
                                        adbc::driver::pgwire::BackendProfile::Redshift());
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed.value().xdbc);
    EXPECT_EQ(parsed.value().xdbc->xdbc_type_name, "TIMETZ");
    EXPECT_EQ(parsed.value().xdbc->xdbc_decimal_digits,
              row < 2 ? std::optional<int16_t>{}
                      : std::optional<int16_t>{static_cast<int16_t>(row == 2 ? 3 : 6)});
    EXPECT_FALSE(parsed.value().xdbc->xdbc_column_size);
  }
}
}  // namespace
}  // namespace adbcpq
