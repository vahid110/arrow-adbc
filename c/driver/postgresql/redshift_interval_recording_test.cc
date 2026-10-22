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
#include "redshift_super_allocation_test.h"
#include "redshift_super_test_fixture.h"
#include "redshift_timetz_test_fixture.h"

namespace {
#include "redshift_super_native_corpus_test.inc"
struct RecordedParameter {
  Oid oid;
  bool null;
  std::string bytes;
  int format = 1;
  int physical_length = 0;
};
struct Recording {
  std::vector<std::string> queries;
  std::vector<Oid> prepared;
  std::string prepared_query;
  std::vector<std::vector<RecordedParameter>> executions;
  PGTransactionStatusType transaction = PQTRANS_IDLE;
  bool exists = true;
  bool super_target = false;
  Oid encoded_target = 0;
  int vendor_result_calls = 0, vendor_result_drift = 0;
  Oid geometry_result_oid = 72113;
  bool geometry_result_invalid = false;
  bool vendor_result_error = false, hll_param_mismatch = false;
  std::string super_setting = "off";
  int super_setting_shape = 0, super_reads = 0, super_drift_after_reads = 0;
  std::string encoding = "UTF8";
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
PGresult* TimetzResult(bool empty = false, ExecStatusType status = PGRES_TUPLES_OK) {
  auto* out = PQmakeEmptyPGresult(nullptr, status);
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
constexpr const char* kSparseHll =
    R"({"version":1,"logm":15,"sparse":{"indices":[],"values":[]}})";
PGresult* VendorResult(std::string_view query, int format,
                       ExecStatusType status = PGRES_TUPLES_OK) {
  const bool describe = status == PGRES_COMMAND_OK;
  if (!describe && recording.vendor_result_error)
    return PQmakeEmptyPGresult(nullptr, PGRES_FATAL_ERROR);
  const bool spatial = query == "SELECT owned_geometry";
  const bool super = query.starts_with("SELECT owned_super");
  const bool mixed = query == "SELECT owned_hll_mixed";
  const int columns = mixed || query == "SELECT owned_hll_pair" ||
                              query.find("super_mixed") != std::string_view::npos
                          ? 2
                          : 1;
  auto* out = PQmakeEmptyPGresult(nullptr, status);
  std::vector<PGresAttDesc> attrs(columns);
  for (int i = 0; i < columns; ++i) {
    attrs[i].name = const_cast<char*>(i ? "other" : "amount");
    attrs[i].typid = super        ? (i ? 67009 : 72111)
                     : spatial    ? recording.geometry_result_oid
                     : mixed && i ? 67009
                                  : 72115;
    attrs[i].format = format;
    attrs[i].atttypmod = super && i == 0 ? 16384000 : -1;
  }
  if (!describe && recording.vendor_result_drift) {
    if (recording.vendor_result_drift == 1) attrs[0].name = const_cast<char*>("changed");
    if (recording.vendor_result_drift == 2) attrs[0].typid = super ? 72112 : 72111;
    if (recording.vendor_result_drift == 3) attrs[0].format = 1 - format;
    if (recording.vendor_result_drift == 4) attrs[0].atttypmod = 9;
    if (recording.vendor_result_drift == 5) attrs[0].tableid = 123;
    if (recording.vendor_result_drift == 6) attrs[0].columnid = 2;
  }
  if (!PQsetResultAttrs(out, columns, attrs.data())) std::abort();
  if (super) {
    const int rows = describe || query == "SELECT owned_super_empty" ? 0
                     : query == "SELECT owned_super_null"            ? 2
                                                                     : 16;
    for (int row = 0; row < rows; ++row) {
      const char* value = query == "SELECT owned_super_null" ? nullptr
                          : query == "SELECT owned_super_invalid"
                              ? "{"
                              : kNativeSuperCells[row];
      if (!PQsetvalue(out, row, 0, const_cast<char*>(value),
                      value ? static_cast<int>(std::strlen(value)) : -1))
        std::abort();
      if (columns == 2) {
        // OID67009 in the recording catalog is int8, not int4.
        char id[8]{};
        id[7] = static_cast<char>(row + 1);
        if (!PQsetvalue(out, row, 1, id, 8)) std::abort();
      }
    }
    return out;
  }
  if (!describe && query != "SELECT owned_hll_empty") {
    const std::string value =
        spatial && recording.geometry_result_invalid ? std::string("not EWKB")
        : spatial
            ? adbcpq::testing::NumericHex("0101000000000000000000f03f000000000000f03f")
            : std::string(kSparseHll);
    for (int i = 0; i < columns; ++i) {
      if (!PQsetvalue(out, 0, i, const_cast<char*>(value.data()), value.size()) ||
          !PQsetvalue(out, 1, i, nullptr, -1))
        std::abort();
    }
  }
  return out;
}
PGresult* Query(const char* query) {
  std::string q(query);
  recording.queries.push_back(q);
  if (q == "SELECT owned_geometry" || q.starts_with("SELECT owned_super")) {
    ++recording.vendor_result_calls;
    return VendorResult(q, 1);
  }
  if (q.starts_with("SHOW parse_super_null_on_error") ||
      q.starts_with("SHOW json_parse_dedup_attributes") ||
      q.starts_with("SHOW json_parse_truncate_strings")) {
    ++recording.super_reads;
    if (recording.super_setting_shape == 1) return Rows(1, {{nullptr}});
    if (recording.super_setting_shape == 2) return Rows(1, {});
    if (recording.super_setting_shape == 3) return Rows(2, {{"off", "off"}});
    if (recording.super_setting_shape == 4)
      return PQmakeEmptyPGresult(nullptr, PGRES_FATAL_ERROR);
    if (recording.super_drift_after_reads &&
        recording.super_reads > recording.super_drift_after_reads)
      return Rows(1, {{"on"}});
    return Rows(1, {{recording.super_setting.c_str()}});
  }
  if (q.starts_with("SELECT owned_timetz")) return TimetzResult(q.ends_with("empty"));
  if (q == "SELECT version();")
    return Rows(1, {{"Redshift 1.0.00000 unqualified recording fixture"}});
  if (q.find("attrelid,") != std::string::npos) return Rows(3, {});
  if (q.find("SELECT oid, typname") != std::string::npos)
    return Rows(5, {{"64001", "intervaly2m", "unqualified_fixture_receive", "0", "0"},
                    {"65017", "intervald2s", "unqualified_fixture_receive", "0", "0"},
                    {"67009", "int8", "int8recv", "0", "0"},
                    {"65113", "timetz", "timetz_recv", "0", "0"},
                    {"75001", "bpchar", "bpcharrecv", "0", "0"},
                    {"75002", "text", "textrecv", "0", "0"},
                    {"72111", "super", "fixture_only", "0", "0"},
                    {"72112", "varbyte", "varbyte_recv", "0", "0"},
                    {"72113", "geometry", "fixture_only", "0", "0"},
                    {"72114", "geography", "fixture_only", "0", "0"},
                    {"72115", "hllsketch", "fixture_only", "0", "0"}});
  if (q == "SELECT CURRENT_SCHEMA()") return Rows(1, {{"public"}});
  if (q.find("SHOW enable_case_sensitive_identifier") != std::string::npos)
    return Rows(1, {{"on"}});
  if (q.find("SELECT cls.oid FROM") != std::string::npos)
    return Rows(1, recording.exists ? std::vector<std::vector<const char*>>{{"12345"}}
                                    : std::vector<std::vector<const char*>>{});
  if (q.find("SELECT attr.attname, attr.atttypid") != std::string::npos ||
      q.find("SELECT attname, atttypid, atttypmod") != std::string::npos) {
    if (!recording.exists) return PQmakeEmptyPGresult(nullptr, PGRES_FATAL_ERROR);
    if (recording.super_target) return Rows(3, {{"amount", "72111", "-1"}});
    if (recording.encoded_target) {
      auto oid = std::to_string(recording.encoded_target);
      return Rows(3, {{"amount", oid.c_str(), "-1"}});
    }
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
    EXPECT_EQ(formats[col], types[col] == 72115 ? 0 : 1);
    row.push_back({types[col], values[col] == nullptr,
                   values[col] ? std::string(values[col], formats[col] == 0
                                                              ? std::strlen(values[col])
                                                              : lengths[col])
                               : std::string{},
                   formats[col], lengths[col]});
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
const char* PQparameterStatus(const PGconn*, const char*) {
  return recording.encoding.c_str();
}
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
  if (std::string_view(query).starts_with("INSERT ")) {
    recording.queries.emplace_back(query);
    return Parameters(n, types, values, lengths, formats);
  }
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
                         const int* lengths, const int* formats, int result_format) {
  EXPECT_EQ(recording.prepared.size(), static_cast<size_t>(n));
  if (recording.prepared_query.starts_with("SELECT owned_hll") ||
      recording.prepared_query.starts_with("SELECT owned_super")) {
    ++recording.vendor_result_calls;
    if (n) {
      auto* captured = Parameters(n, recording.prepared.data(), values, lengths, formats);
      PQclear(captured);
    }
    return VendorResult(recording.prepared_query, result_format);
  }
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
  if (recording.prepared_query.starts_with("SELECT owned_hll") ||
      recording.prepared_query == "SELECT owned_geometry")
    return VendorResult(recording.prepared_query, 0, PGRES_COMMAND_OK);
  if (recording.prepared_query.starts_with("SELECT owned_super"))
    return VendorResult(recording.prepared_query, 0, PGRES_COMMAND_OK);
  return recording.prepared_query.starts_with("SELECT owned_timetz")
             ? TimetzResult(true, PGRES_COMMAND_OK)
             : Command();
}
int PQnparams(const PGresult*) {
  if (recording.prepared_query.starts_with("SELECT owned_hll") ||
      std::find(recording.prepared.begin(), recording.prepared.end(), 72115) !=
          recording.prepared.end())
    return recording.prepared.size();
  return recording.prepared_query == "SELECT owned_timetz_param" ? 1 : 0;
}
Oid PQparamtype(const PGresult*, int index) {
  if (recording.prepared_query.starts_with("SELECT owned_hll") ||
      std::find(recording.prepared.begin(), recording.prepared.end(), 72115) !=
          recording.prepared.end())
    return recording.hll_param_mismatch ? 72111 : recording.prepared[index];
  return 65113;
}
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
    if (statement.private_data) {
      EXPECT_EQ(driver.StatementRelease(&statement, &error), ADBC_STATUS_OK);
    }
    if (connection.private_data) {
      EXPECT_EQ(driver.ConnectionRelease(&connection, &error), ADBC_STATUS_OK);
    }
    if (database.private_data) {
      EXPECT_EQ(driver.DatabaseRelease(&database, &error), ADBC_STATUS_OK);
    }
    if (driver.release) {
      EXPECT_EQ(driver.release(&driver, &error), ADBC_STATUS_OK);
    }
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
    if (i % 3 != 1) {
      EXPECT_EQ(parameter.bytes, testing::NumericHex(i % 3 == 0 ? "00000000000003e8"
                                                                : "fffffffffffff830"));
    }
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
      if (exists || std::string_view(mode) != ADBC_INGEST_OPTION_MODE_APPEND) {
        EXPECT_NE(std::string_view(Message()).find("unknown"), std::string::npos)
            << Message();
      }
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
      if (mutation == 1) {
        ASSERT_EQ(testing::IntervalSetMetadata(field, "ARROW:extension:name",
                                               "adbc.redshift.timetz_us_offset.v2"),
                  0);
      }
      if (mutation == 2) {
        ASSERT_EQ(testing::IntervalSetMetadata(field, "ARROW:extension:metadata", "{}"),
                  0);
      }
      if (mutation == 3) {
        ASSERT_EQ(
            testing::IntervalSetMetadata(field, "ADBC:redshift:sql_type", "HLLSKETCH"),
            0);
      }
      if (mutation == 4) field->children[0]->flags |= ARROW_FLAG_NULLABLE;
      if (mutation == 5) {
        ASSERT_EQ(ArrowSchemaSetName(field->children[0], "offset_seconds"), 0);
      }
      if (mutation == 6) {
        ASSERT_EQ(testing::IntervalSetMetadata(field->children[0],
                                               "ARROW:extension:metadata", "{}"),
                  0);
      }
      if (mutation == 7) {
        ASSERT_EQ(
            testing::DecimalMetadata(field, "ADBC:redshift:fractional_precision", "6"),
            0);
      }
      if (mutation == 8) {
        ASSERT_EQ(
            testing::IntervalSetMetadata(field, "ADBC:redshift:type_modifier", "999"), 0);
      }
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
  // CREATE's unused declaration maps NA through discovered TEXT. This recorder
  // must supply that ordinary catalog prerequisite; actual NULL parameters below
  // must still select the known TIMETZ target OID rather than the fallback.
  auto* owner =
      static_cast<std::shared_ptr<PostgresConnection>*>(connection.private_data);
  nanoarrow::UniqueSchema null_schema;
  ASSERT_EQ(ArrowSchemaInitFromType(null_schema.get(), NANOARROW_TYPE_NA), 0);
  PostgresType fallback;
  ASSERT_EQ(PostgresType::FromSchema(*(*owner)->type_resolver(), null_schema.get(),
                                     &fallback, nullptr),
            0);
  EXPECT_EQ(fallback.oid(), 75002U);
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
          if (!raw || *raw != 3) {
            EXPECT_TRUE(recording.executions.empty());
          }
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
      if (!owned) {
        ASSERT_EQ(
            driver.ConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                                       ADBC_OPTION_VALUE_DISABLED, &error),
            ADBC_STATUS_OK);
      }
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

namespace adbcpq {
namespace {
TEST_F(IntervalPublicRecordingTest,
       SuperGeneratedIngestUsesConstructorDynamicVarbyteAndNullPointer) {
  recording.super_target = true;
  for (bool stream : {false, true}) {
    nanoarrow::UniqueSchema schema;
    nanoarrow::UniqueArray array;
    ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
    ASSERT_EQ(testing::SuperBatch(schema.get(), array.get(),
                                  {std::nullopt, "null", R"({"x":1.20})"}),
              0);
    ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                        "owned", &error),
              ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                        ADBC_INGEST_OPTION_MODE_APPEND, &error),
              ADBC_STATUS_OK);
    if (stream) {
      nanoarrow::UniqueArrayStream input;
      adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
      ASSERT_EQ(driver.StatementBindStream(&statement, input.get(), &error),
                ADBC_STATUS_OK)
          << Message();
    } else {
      ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                ADBC_STATUS_OK)
          << Message();
    }
    recording.queries.clear();
    recording.executions.clear();
    ASSERT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ADBC_STATUS_OK)
        << Message();
    ASSERT_EQ(recording.executions.size(), 1);
    const auto& values = recording.executions[0];
    ASSERT_EQ(values.size(), 3);
    EXPECT_TRUE(values[0].null);
    EXPECT_FALSE(values[1].null);
    EXPECT_EQ(values[1].bytes, "null");
    EXPECT_EQ(values[2].bytes, R"({"x":1.20})");
    for (const auto& value : values) EXPECT_EQ(value.oid, 72112);
    EXPECT_NE(std::find(recording.queries.begin(), recording.queries.end(),
                        "INSERT INTO \"public\" . \"owned\" (\"amount\") VALUES "
                        "(JSON_PARSE($1)), (JSON_PARSE($2)), (JSON_PARSE($3))"),
              recording.queries.end())
        << ::testing::PrintToString(recording.queries);
  }
}
TEST_F(IntervalPublicRecordingTest,
       SuperPreownershipAndPostBindModeMutationNeverDropOrExecute) {
  for (bool stream : {false, true}) {
    nanoarrow::UniqueSchema schema;
    nanoarrow::UniqueArray array;
    ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
    ASSERT_EQ(testing::SuperBatch(schema.get(), array.get(), {std::nullopt}), 0);
    ASSERT_EQ(driver.StatementSetSqlQuery(&statement, "SELECT $1::super", &error),
              ADBC_STATUS_OK);
    if (stream) {
      nanoarrow::UniqueArrayStream input;
      adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
      EXPECT_NE(driver.StatementBindStream(&statement, input.get(), &error),
                ADBC_STATUS_OK);
      EXPECT_NE(input->release, nullptr);
    } else {
      EXPECT_NE(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                ADBC_STATUS_OK);
      EXPECT_NE(array->release, nullptr);
      EXPECT_NE(schema->release, nullptr);
    }
  }
  ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                      "owned", &error),
            ADBC_STATUS_OK);
  ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                      ADBC_INGEST_OPTION_MODE_APPEND, &error),
            ADBC_STATUS_OK);
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
  ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
  ASSERT_EQ(testing::SuperBatch(schema.get(), array.get(), {"null"}), 0);
  ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
            ADBC_STATUS_OK);
  ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                      ADBC_INGEST_OPTION_MODE_REPLACE, &error),
            ADBC_STATUS_OK);
  recording.queries.clear();
  EXPECT_NE(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
            ADBC_STATUS_OK);
  EXPECT_TRUE(recording.queries.empty());
  EXPECT_TRUE(recording.exists);
}
TEST_F(IntervalPublicRecordingTest, SuperContextAndTargetFailuresArePreEffect) {
  recording.super_target = true;
  ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                      "owned", &error),
            ADBC_STATUS_OK);
  ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                      ADBC_INGEST_OPTION_MODE_CREATE, &error),
            ADBC_STATUS_OK);
  for (auto setting : {"on", "true", "unknown", "off "}) {
    recording.super_setting = setting;
    nanoarrow::UniqueSchema schema;
    nanoarrow::UniqueArray array;
    ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
    ASSERT_EQ(testing::SuperBatch(schema.get(), array.get(), {}), 0);
    ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
              ADBC_STATUS_OK);
    recording.queries.clear();
    EXPECT_NE(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ADBC_STATUS_OK);
    for (const auto& q : recording.queries)
      EXPECT_FALSE(q.starts_with("CREATE ") || q.starts_with("DROP ") ||
                   q.starts_with("INSERT "));
  }
}
TEST_F(IntervalPublicRecordingTest, SuperContextShapesAndPerBatchDriftCannotDispatch) {
  recording.super_target = true;
  ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                      "owned", &error),
            ADBC_STATUS_OK);
  ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                      ADBC_INGEST_OPTION_MODE_APPEND, &error),
            ADBC_STATUS_OK);
  for (int shape = 1; shape <= 5; ++shape) {
    recording.super_setting_shape = shape < 5 ? shape : 0;
    recording.super_reads = 0;
    recording.super_drift_after_reads = shape == 5 ? 3 : 0;
    nanoarrow::UniqueSchema schema;
    nanoarrow::UniqueArray array;
    ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
    ASSERT_EQ(testing::SuperBatch(schema.get(), array.get(), {std::nullopt}), 0);
    ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
              ADBC_STATUS_OK);
    recording.executions.clear();
    EXPECT_NE(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ADBC_STATUS_OK);
    EXPECT_TRUE(recording.executions.empty());
    if (shape == 5) {
      EXPECT_EQ(recording.super_reads, 4);
    }
  }
}
TEST_F(IntervalPublicRecordingTest, SuperExistingAndMissingCreateAppendAndWrongTarget) {
  recording.super_target = true;
  for (bool exists : {false, true}) {
    recording.exists = exists;
    recording.queries.clear();
    ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                        "owned", &error),
              ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                        ADBC_INGEST_OPTION_MODE_CREATE_APPEND, &error),
              ADBC_STATUS_OK);
    nanoarrow::UniqueSchema schema;
    nanoarrow::UniqueArray array;
    ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
    ASSERT_EQ(testing::SuperBatch(schema.get(), array.get(), {}), 0);
    ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
              ADBC_STATUS_OK);
    EXPECT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ADBC_STATUS_OK)
        << Message();
    const bool created =
        std::any_of(recording.queries.begin(), recording.queries.end(),
                    [](const std::string& q) { return q.starts_with("CREATE "); });
    EXPECT_EQ(created, !exists);
    if (created) {
      EXPECT_NE(std::find_if(recording.queries.begin(), recording.queries.end(),
                             [](const std::string& q) {
                               return q.find("amount\" SUPER") != std::string::npos;
                             }),
                recording.queries.end());
    }
  }
  recording.super_target = false;
  recording.executions.clear();
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
  ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
  ASSERT_EQ(testing::SuperBatch(schema.get(), array.get(), {std::nullopt}), 0);
  ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                      ADBC_INGEST_OPTION_MODE_APPEND, &error),
            ADBC_STATUS_OK);
  ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
            ADBC_STATUS_OK);
  EXPECT_NE(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
            ADBC_STATUS_OK);
  EXPECT_TRUE(recording.executions.empty());
}
// Fault only the production scanner, after source-view/setup allocations. This
// wrapper is test-only; ExecutePreparedRows retains the real transaction owner.
class SuperScannerFaultWriter : public PostgresCopyFieldWriter {
 public:
  explicit SuperScannerFaultWriter(std::unique_ptr<PostgresCopyFieldWriter> actual)
      : actual_(std::move(actual)) {}
  ArrowErrorCode Write(ArrowBuffer* buffer, int64_t row, ArrowError* error) override {
    testing::super_allocation_fail_after = 0;
    struct Reset {
      ~Reset() { testing::super_allocation_fail_after = -1; }
    } reset;
    return actual_->Write(buffer, row, error);
  }

 private:
  std::unique_ptr<PostgresCopyFieldWriter> actual_;
};
struct SuperFaultSource {
  ArrowArrayStream input{};
  BindStream* bind;
  bool armed = false;
};
void SuperFaultStream(ArrowArrayStream* input, BindStream* bind, ArrowArrayStream* out) {
  auto* state = new SuperFaultSource{};
  state->bind = bind;
  ArrowArrayStreamMove(input, &state->input);
  out->private_data = state;
  out->get_schema = [](ArrowArrayStream* stream, ArrowSchema* schema) {
    auto* state = static_cast<SuperFaultSource*>(stream->private_data);
    return state->input.get_schema(&state->input, schema);
  };
  out->get_next = [](ArrowArrayStream* stream, ArrowArray* array) {
    auto* state = static_cast<SuperFaultSource*>(stream->private_data);
    const int result = state->input.get_next(&state->input, array);
    if (!result && array->release && !state->armed) {
      state->bind->bind_field_writers[0] = std::make_unique<SuperScannerFaultWriter>(
          std::move(state->bind->bind_field_writers[0]));
      state->armed = true;
    }
    return result;
  };
  out->get_last_error = [](ArrowArrayStream*) -> const char* { return nullptr; };
  out->release = [](ArrowArrayStream* stream) {
    auto* state = static_cast<SuperFaultSource*>(stream->private_data);
    state->input.release(&state->input);
    delete state;
    stream->release = nullptr;
  };
}
TEST_F(IntervalPublicRecordingTest,
       SuperScannerAllocationFailureKeepsOwnerAndPrimaryError) {
  for (bool owned : {false, true}) {
    for (bool fail_rollback : {false, true}) {
      recording.transaction = owned ? PQTRANS_IDLE : PQTRANS_INTRANS;
      recording.committed = 7;
      recording.pending = 0;
      recording.fail_rollback = fail_rollback;
      recording.executions.clear();
      recording.queries.clear();
      nanoarrow::UniqueSchema schema;
      nanoarrow::UniqueArray array;
      ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
      ASSERT_EQ(testing::SuperBatch(schema.get(), array.get(),
                                    {R"({"long_key_requiring_heap_allocation":1})"}),
                0);
      nanoarrow::UniqueArrayStream input, fault;
      adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
      BindStream bind;
      bind.generated_super_ingest = true;
      SuperFaultStream(input.get(), &bind, fault.get());
      bind.SetBind(fault.get());
      ASSERT_TRUE(bind.Begin([] { return Status::Ok(); }).ok());
      auto* impl =
          static_cast<std::shared_ptr<PostgresConnection>*>(connection.private_data);
      PostgresType target, targets(PostgresTypeId::kRecord);
      ASSERT_EQ((*impl)->type_resolver()->Find(72111, &target, nullptr), 0);
      targets.AppendChild("amount", target);
      int64_t rows = -1;
      Status status;
      EXPECT_NO_THROW(status = bind.ExecutePreparedRows(
                          (*impl)->conn(), "owned", "amount", *(*impl)->type_resolver(),
                          owned, 16, &rows, &targets));
      EXPECT_FALSE(status.ok());
      AdbcError failure = ADBC_ERROR_INIT;
      EXPECT_EQ(status.ToAdbc(&failure), ADBC_STATUS_INTERNAL);
      ASSERT_NE(failure.message, nullptr);
      EXPECT_NE(std::string_view(failure.message).find("allocation failed"),
                std::string_view::npos);
      if (failure.release) failure.release(&failure);
      EXPECT_TRUE(recording.executions.empty());
      EXPECT_EQ(recording.committed, 7);
      EXPECT_EQ(bind.cleanup_failed, owned && fail_rollback);
      if (!owned) {
        EXPECT_EQ(recording.transaction, PQTRANS_INTRANS);
        EXPECT_EQ(
            std::count(recording.queries.begin(), recording.queries.end(), "ROLLBACK"),
            0);
      } else if (!fail_rollback) {
        EXPECT_EQ(recording.transaction, PQTRANS_IDLE);
      }
    }
  }
}
void EncodedSchema(ArrowSchema* schema, const std::vector<RedshiftEncodedKind>& kinds) {
  ArrowSchemaInit(schema);
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema, kinds.size()), 0);
  for (size_t i = 0; i < kinds.size(); ++i) {
    if (kinds[i] == RedshiftEncodedKind::kNone) {
      ASSERT_EQ(ArrowSchemaSetType(schema->children[i], NANOARROW_TYPE_INT64), 0);
    } else {
      ASSERT_EQ(RedshiftEncodedSchema::SetSchema(schema->children[i], kinds[i]), 0);
      nanoarrow::UniqueBuffer m;
      ASSERT_EQ(ArrowMetadataBuilderInit(m.get(), schema->children[i]->metadata), 0);
      ASSERT_EQ(ArrowMetadataBuilderAppend(
                    m.get(), ArrowCharView("ADBC:redshift:sql_type"),
                    ArrowCharView(RedshiftEncodedSchema::Family(kinds[i]))),
                0);
      ASSERT_EQ(
          ArrowSchemaSetMetadata(schema->children[i], reinterpret_cast<char*>(m->data)),
          0);
    }
    ASSERT_EQ(ArrowSchemaSetName(schema->children[i], i == 0 ? "amount" : "other"), 0);
  }
}
TEST_F(IntervalPublicRecordingTest,
       HllAdjacentMixedParametersPreservePhysicalNulStrideAndNulls) {
  for (bool stream : {false, true})
    for (int null_at : {-1, 0, 1, 2}) {
      nanoarrow::UniqueSchema schema;
      EncodedSchema(schema.get(), {RedshiftEncodedKind::kHll, RedshiftEncodedKind::kHll,
                                   RedshiftEncodedKind::kNone});
      nanoarrow::UniqueArray array;
      ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
      ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
      for (int i = 0; i < 3; ++i) {
        if (i == null_at) {
          ASSERT_EQ(ArrowArrayAppendNull(array->children[i], 1), 0);
        } else if (i == 2) {
          ASSERT_EQ(ArrowArrayAppendInt(array->children[i], 42), 0);
        } else
          ASSERT_EQ(ArrowArrayAppendString(array->children[i], ArrowCharView(kSparseHll)),
                    0);
      }
      ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
      ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
      ASSERT_EQ(driver.StatementSetSqlQuery(&statement, "SELECT owned_hll_param", &error),
                ADBC_STATUS_OK);
      if (stream) {
        nanoarrow::UniqueArrayStream input;
        adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
        ASSERT_EQ(driver.StatementBindStream(&statement, input.get(), &error),
                  ADBC_STATUS_OK)
            << Message();
      } else
        ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                  ADBC_STATUS_OK)
            << Message();
      recording.executions.clear();
      nanoarrow::UniqueArrayStream output;
      ASSERT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
                ADBC_STATUS_OK)
          << Message();
      ASSERT_EQ(recording.executions.size(), 1);
      const auto& params = recording.executions[0];
      ASSERT_EQ(params.size(), 3);
      for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(params[i].null, i == null_at);
        if (i < 2) {
          EXPECT_EQ(params[i].oid, 72115);
          EXPECT_EQ(params[i].format, 0);
          EXPECT_EQ(params[i].physical_length,
                    i == null_at ? 0 : std::strlen(kSparseHll) + 1);
          if (i != null_at) {
            EXPECT_EQ(params[i].bytes, kSparseHll);
          }
        } else if (i != null_at) {
          EXPECT_EQ(params[i].bytes, testing::NumericHex("000000000000002a"));
        }
      }
      nanoarrow::UniqueSchema actual;
      ASSERT_EQ(output->get_schema(output.get(), actual.get()), 0);
      EXPECT_STREQ(actual->children[0]->format, "U");
      nanoarrow::UniqueArray rows;
      ASSERT_EQ(output->get_next(output.get(), rows.get()), 0);
      EXPECT_EQ(rows->length, 2);
    }
}
TEST_F(IntervalPublicRecordingTest,
       AllHllMultipleEmptyAndSpatialResultsUseSingleExactFormat) {
  for (const char* query :
       {"SELECT owned_hll_pair", "SELECT owned_hll_empty", "SELECT owned_geometry"}) {
    ASSERT_EQ(driver.StatementSetSqlQuery(&statement, query, &error), ADBC_STATUS_OK);
    recording.vendor_result_calls = 0;
    nanoarrow::UniqueArrayStream output;
    ASSERT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
              ADBC_STATUS_OK)
        << Message();
    EXPECT_EQ(recording.vendor_result_calls, 1);
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(output->get_schema(output.get(), schema.get()), 0);
    EXPECT_STREQ(schema->children[0]->format,
                 std::string_view(query) == "SELECT owned_geometry" ? "Z" : "U");
    nanoarrow::UniqueArray rows;
    ASSERT_EQ(output->get_next(output.get(), rows.get()), 0);
    EXPECT_EQ(rows->release != nullptr,
              std::string_view(query) != "SELECT owned_hll_empty");
  }
}
TEST_F(IntervalPublicRecordingTest, HllMixedDescriptorDriftAndServerFailureNeverRetry) {
  ASSERT_EQ(driver.StatementSetSqlQuery(&statement, "SELECT owned_hll_mixed", &error),
            ADBC_STATUS_OK);
  nanoarrow::UniqueArrayStream out;
  EXPECT_NE(driver.StatementExecuteQuery(&statement, out.get(), nullptr, &error),
            ADBC_STATUS_OK);
  EXPECT_EQ(recording.vendor_result_calls, 0);
  for (int fault = 1; fault <= 6; ++fault) {
    recording.vendor_result_drift = fault < 6 ? fault : 0;
    recording.vendor_result_error = fault == 6;
    recording.vendor_result_calls = 0;
    ASSERT_EQ(driver.StatementSetSqlQuery(&statement, "SELECT owned_hll_pair", &error),
              ADBC_STATUS_OK);
    EXPECT_NE(driver.StatementExecuteQuery(&statement, out.get(), nullptr, &error),
              ADBC_STATUS_OK);
    EXPECT_EQ(recording.vendor_result_calls, 1);
  }
}
TEST_F(IntervalPublicRecordingTest,
       GeometryResultAliasPreservesTypedSchemaAndValidation) {
  // The real Redshift catalog and protocol identities differ. A dynamic catalog
  // OID here also ensures the output alias cannot become a parameter/catalog OID.
  recording.geometry_result_oid = 3999;
  for (bool invalid : {false, true}) {
    recording.geometry_result_invalid = invalid;
    recording.vendor_result_calls = 0;
    ASSERT_EQ(driver.StatementSetSqlQuery(&statement, "SELECT owned_geometry", &error),
              ADBC_STATUS_OK);
    nanoarrow::UniqueArrayStream output;
    ASSERT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
              ADBC_STATUS_OK)
        << Message();
    EXPECT_EQ(recording.vendor_result_calls, 1);
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(output->get_schema(output.get(), schema.get()), 0);
    EXPECT_STREQ(schema->children[0]->format, "Z");
    ArrowError parse_error{};
    RedshiftEncodedSchema parsed;
    EXPECT_EQ(
        RedshiftEncodedSchema::Parse(schema->children[0], true, &parsed, &parse_error),
        0);
    EXPECT_EQ(parsed.kind, RedshiftEncodedKind::kGeometry);
    nanoarrow::UniqueArray rows;
    if (invalid) {
      EXPECT_NE(output->get_next(output.get(), rows.get()), 0);
      continue;
    }
    ASSERT_EQ(output->get_next(output.get(), rows.get()), 0);
    ASSERT_EQ(rows->length, 2);
    nanoarrow::UniqueArrayView view;
    ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), &parse_error), 0);
    ASSERT_EQ(ArrowArrayViewSetArray(view.get(), rows.get(), &parse_error), 0);
    auto actual = ArrowArrayViewGetBytesUnsafe(view->children[0], 0);
    EXPECT_EQ(std::string_view(actual.data.as_char, actual.size_bytes),
              testing::NumericHex("0101000000000000000000f03f000000000000f03f"));
    EXPECT_TRUE(ArrowArrayViewIsNull(view->children[0], 1));
  }
}
TEST_F(IntervalPublicRecordingTest,
       SpatialGeneratedConstructorAndHllTypeUseActualTarget) {
  for (auto kind : {RedshiftEncodedKind::kGeometry, RedshiftEncodedKind::kGeography,
                    RedshiftEncodedKind::kHll})
    for (bool stream : {false, true}) {
      recording.encoded_target = kind == RedshiftEncodedKind::kGeometry    ? 72113
                                 : kind == RedshiftEncodedKind::kGeography ? 72114
                                                                           : 72115;
      nanoarrow::UniqueSchema schema;
      EncodedSchema(schema.get(), {kind});
      nanoarrow::UniqueArray array;
      ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
      ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
      const auto value =
          kind == RedshiftEncodedKind::kHll
              ? std::string(kSparseHll)
              : testing::NumericHex("0101000020e6100000000000000000f03f000000000000f03f");
      ArrowBufferView bytes{};
      bytes.data.as_char = value.data();
      bytes.size_bytes = value.size();
      ASSERT_EQ(ArrowArrayAppendBytes(array->children[0], bytes), 0);
      ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
      ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
      ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                          "owned", &error),
                ADBC_STATUS_OK);
      ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                          ADBC_INGEST_OPTION_MODE_APPEND, &error),
                ADBC_STATUS_OK);
      if (stream) {
        nanoarrow::UniqueArrayStream in;
        adbc::driver::MakeArrayStream(schema.get(), array.get(), in.get());
        ASSERT_EQ(driver.StatementBindStream(&statement, in.get(), &error),
                  ADBC_STATUS_OK)
            << Message();
      } else
        ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                  ADBC_STATUS_OK)
            << Message();
      recording.executions.clear();
      recording.queries.clear();
      ASSERT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
                ADBC_STATUS_OK)
          << Message();
      ASSERT_EQ(recording.executions.size(), 1);
      const auto& p = recording.executions[0][0];
      EXPECT_EQ(p.oid, kind == RedshiftEncodedKind::kHll ? 72115 : 72112);
      EXPECT_EQ(p.format, kind == RedshiftEncodedKind::kHll ? 0 : 1);
      if (kind == RedshiftEncodedKind::kHll) {
        EXPECT_EQ(p.bytes, value);
      } else {
        EXPECT_EQ(p.bytes, value);
        EXPECT_EQ(p.physical_length, value.size());
        EXPECT_NE(p.bytes.size(), value.size() * 2);
        const auto it =
            std::find_if(recording.queries.begin(), recording.queries.end(),
                         [](const auto& q) { return q.starts_with("INSERT "); });
        ASSERT_NE(it, recording.queries.end());
        EXPECT_NE(it->find("ST_GeomFromEWKB($1)"), std::string::npos);
        EXPECT_EQ(it->find("::GEOGRAPHY") != std::string::npos,
                  kind == RedshiftEncodedKind::kGeography);
      }
    }
}
TEST_F(IntervalPublicRecordingTest, HllVarbyteBothOrdersGrowthAndNullPositions) {
  for (bool reverse : {false, true})
    for (int null_at : {-1, 0, 1, 2}) {
      nanoarrow::UniqueSchema schema;
      EncodedSchema(schema.get(), {RedshiftEncodedKind::kHll, RedshiftEncodedKind::kHll,
                                   RedshiftEncodedKind::kHll});
      const int binary = reverse ? 0 : 1;
      auto* field = schema->children[binary];
      ASSERT_EQ(ArrowSchemaSetType(field, NANOARROW_TYPE_LARGE_BINARY), 0);
      ASSERT_EQ(ArrowSchemaSetMetadata(field, nullptr), 0);
      ASSERT_EQ(testing::DecimalMetadata(field, "ADBC:redshift:sql_type", "VARBYTE"), 0);
      nanoarrow::UniqueArray array;
      ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
      ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
      const std::string large = "\"" + std::string(10000, 'A') + "\"";
      const auto literal = testing::NumericHex("0080ff");
      for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
          if (col == null_at) {
            ASSERT_EQ(ArrowArrayAppendNull(array->children[col], 1), 0);
          } else {
            const auto& value = col == binary ? literal : large;
            ArrowBufferView bytes{};
            bytes.data.as_char = value.data();
            bytes.size_bytes = value.size();
            ASSERT_EQ(ArrowArrayAppendBytes(array->children[col], bytes), 0);
          }
        }
        ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
      }
      ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
      ASSERT_EQ(driver.StatementSetSqlQuery(&statement, "SELECT owned_hll_param", &error),
                ADBC_STATUS_OK);
      ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                ADBC_STATUS_OK)
          << Message();
      recording.executions.clear();
      nanoarrow::UniqueArrayStream out;
      ASSERT_EQ(driver.StatementExecuteQuery(&statement, out.get(), nullptr, &error),
                ADBC_STATUS_OK)
          << Message();
      while (true) {
        nanoarrow::UniqueArray rows;
        ASSERT_EQ(out->get_next(out.get(), rows.get()), 0)
            << out->get_last_error(out.get());
        if (!rows->release) break;
      }
      ASSERT_EQ(recording.executions.size(), 2);
      for (const auto& params : recording.executions)
        for (int col = 0; col < 3; ++col) {
          EXPECT_EQ(params[col].null, col == null_at);
          EXPECT_EQ(params[col].oid, col == binary ? 72112 : 72115);
          EXPECT_EQ(params[col].format, col == binary ? 1 : 0);
          if (col != null_at) {
            EXPECT_EQ(params[col].bytes, col == binary ? literal : large);
          }
        }
    }
}
TEST_F(IntervalPublicRecordingTest,
       HllParameterMismatchAndVendorMutationRejectBeforeDispatch) {
  for (bool stream : {false, true}) {
    nanoarrow::UniqueSchema schema;
    EncodedSchema(schema.get(), {RedshiftEncodedKind::kHll});
    nanoarrow::UniqueArray array;
    ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
    ASSERT_EQ(ArrowArrayAppendNull(array->children[0], 1), 0);
    ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
    ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
    ASSERT_EQ(driver.StatementSetSqlQuery(&statement, "SELECT owned_hll_param", &error),
              ADBC_STATUS_OK);
    if (stream) {
      nanoarrow::UniqueArrayStream in;
      adbc::driver::MakeArrayStream(schema.get(), array.get(), in.get());
      ASSERT_EQ(driver.StatementBindStream(&statement, in.get(), &error), ADBC_STATUS_OK);
    } else
      ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                ADBC_STATUS_OK);
    recording.hll_param_mismatch = true;
    recording.executions.clear();
    nanoarrow::UniqueArrayStream out;
    EXPECT_NE(driver.StatementExecuteQuery(&statement, out.get(), nullptr, &error),
              ADBC_STATUS_OK);
    EXPECT_TRUE(recording.executions.empty());
    recording.hll_param_mismatch = false;
  }
  nanoarrow::UniqueSchema schema;
  EncodedSchema(schema.get(), {RedshiftEncodedKind::kHll});
  nanoarrow::UniqueArray array;
  ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
  ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
  ASSERT_EQ(ArrowArrayAppendNull(array->children[0], 1), 0);
  ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
  ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
            ADBC_STATUS_OK);
  ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                      "owned", &error),
            ADBC_STATUS_OK);
  ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                      ADBC_INGEST_OPTION_MODE_REPLACE, &error),
            ADBC_STATUS_OK);
  recording.queries.clear();
  EXPECT_NE(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
            ADBC_STATUS_OK);
  EXPECT_TRUE(recording.queries.empty());
}

TEST_F(IntervalPublicRecordingTest,
       VendorMalformedValuesHavePublicInvalidArgumentAndPreserveOwner) {
  for (int family = 0; family < 4; ++family)
    for (bool stream : {false, true})
      for (bool owned : {false, true}) {
        recording.super_target = family == 0;
        recording.encoded_target = family == 1 ? 72113 : family == 2 ? 72114 : 72115;
        recording.transaction = owned ? PQTRANS_IDLE : PQTRANS_INTRANS;
        recording.committed = 7;
        recording.pending = owned ? 0 : 2;
        recording.executions.clear();
        recording.queries.clear();
        ASSERT_EQ(
            driver.ConnectionSetOption(
                &connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                owned ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED, &error),
            ADBC_STATUS_OK);
        nanoarrow::UniqueSchema schema;
        nanoarrow::UniqueArray array;
        if (family == 0) {
          ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
          ASSERT_EQ(testing::SuperBatch(schema.get(), array.get(),
                                        {"null", R"({"a":1,"a":2})"}),
                    0);
        } else {
          const auto kind = family == 1   ? RedshiftEncodedKind::kGeometry
                            : family == 2 ? RedshiftEncodedKind::kGeography
                                          : RedshiftEncodedKind::kHll;
          EncodedSchema(schema.get(), {kind});
          ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
          ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
          ArrowBufferView malformed{};
          malformed.data.as_char = "bad";
          malformed.size_bytes = 3;
          ASSERT_EQ(ArrowArrayAppendBytes(array->children[0], malformed), 0);
          ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
          ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
        }
        ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                            "owned", &error),
                  ADBC_STATUS_OK);
        ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                            ADBC_INGEST_OPTION_MODE_APPEND, &error),
                  ADBC_STATUS_OK);
        if (stream) {
          nanoarrow::UniqueArrayStream input;
          adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
          ASSERT_EQ(driver.StatementBindStream(&statement, input.get(), &error),
                    ADBC_STATUS_OK);
          EXPECT_FALSE(input->release);
        } else {
          ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                    ADBC_STATUS_OK);
          EXPECT_FALSE(array->release);
          EXPECT_FALSE(schema->release);
        }
        EXPECT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
                  ADBC_STATUS_INVALID_ARGUMENT)
            << family << ": " << Message();
        EXPECT_EQ(std::string(error.sqlstate, sizeof(error.sqlstate)), std::string(5, 0));
        EXPECT_TRUE(recording.executions.empty());
        EXPECT_EQ(recording.committed, 7);
        EXPECT_EQ(recording.pending, owned ? 0 : 2);
        EXPECT_EQ(recording.transaction, owned ? PQTRANS_IDLE : PQTRANS_INTRANS);
        EXPECT_EQ(
            std::count(recording.queries.begin(), recording.queries.end(), "ROLLBACK"),
            owned ? 1 : 0);
        EXPECT_EQ(
            std::count(recording.queries.begin(), recording.queries.end(), "COMMIT"), 0);
        recording.transaction = PQTRANS_IDLE;
        recording.pending = 0;
      }
}
TEST_F(IntervalPublicRecordingTest,
       HllMalformedDirectArrayAndStreamUseCurrentRowInvalidArgument) {
  for (bool stream : {false, true}) {
    nanoarrow::UniqueSchema schema;
    EncodedSchema(schema.get(), {RedshiftEncodedKind::kHll});
    nanoarrow::UniqueArray array;
    ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
    ASSERT_EQ(ArrowArrayAppendString(array->children[0], ArrowCharView("bad")), 0);
    ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
    ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
    ASSERT_EQ(driver.StatementSetSqlQuery(&statement, "SELECT owned_hll_param", &error),
              ADBC_STATUS_OK);
    if (stream) {
      nanoarrow::UniqueArrayStream input;
      adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
      ASSERT_EQ(driver.StatementBindStream(&statement, input.get(), &error),
                ADBC_STATUS_OK);
    } else {
      ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                ADBC_STATUS_OK);
    }
    recording.executions.clear();
    nanoarrow::UniqueArrayStream output;
    EXPECT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
              ADBC_STATUS_INVALID_ARGUMENT)
        << Message();
    EXPECT_TRUE(recording.executions.empty());
    EXPECT_FALSE(output->release);
  }
}

TEST_F(IntervalPublicRecordingTest,
       SuperDuplicateLateErrorRetainsPrimaryAndOwnedRollbackForBothBatchPaths) {
  for (size_t max_rows : {1, 16})
    for (bool owned : {false, true})
      for (bool fail_rollback : {false, true}) {
        recording.transaction = owned ? PQTRANS_IDLE : PQTRANS_INTRANS;
        recording.committed = 7;
        recording.pending = owned ? 0 : 2;
        recording.fail_rollback = fail_rollback;
        recording.executions.clear();
        recording.queries.clear();
        nanoarrow::UniqueSchema schema;
        ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
        nanoarrow::UniqueArray first, late;
        ASSERT_EQ(
            testing::SuperBatch(schema.get(), first.get(),
                                std::vector<std::optional<std::string>>(16, "null")),
            0);
        ASSERT_EQ(testing::SuperBatch(schema.get(), late.get(), {R"({"a":1,"a":2})"}), 0);
        nanoarrow::UniqueArrayStream input;
        ASSERT_EQ(ArrowBasicArrayStreamInit(input.get(), schema.get(), 2), 0);
        ArrowBasicArrayStreamSetArray(input.get(), 0, first.get());
        ArrowBasicArrayStreamSetArray(input.get(), 1, late.get());
        BindStream bind;
        bind.generated_super_ingest = true;
        bind.SetBind(input.get());
        ASSERT_TRUE(bind.Begin([] { return Status::Ok(); }).ok());
        auto* impl =
            static_cast<std::shared_ptr<PostgresConnection>*>(connection.private_data);
        PostgresType target, targets(PostgresTypeId::kRecord);
        ASSERT_EQ((*impl)->type_resolver()->Find(72111, &target, nullptr), 0);
        targets.AppendChild("amount", target);
        int64_t rows = -1;
        auto status = bind.ExecutePreparedRows((*impl)->conn(), "owned", "amount",
                                               *(*impl)->type_resolver(), owned, max_rows,
                                               &rows, &targets);
        AdbcError failure = ADBC_ERROR_INIT;
        EXPECT_EQ(status.ToAdbc(&failure), ADBC_STATUS_INVALID_ARGUMENT);
        ASSERT_NE(failure.message, nullptr);
        EXPECT_NE(std::string_view(failure.message).find("duplicate key"),
                  std::string_view::npos);
        if (failure.release) failure.release(&failure);
        EXPECT_EQ(recording.executions.size(), max_rows == 1 ? 16 : 1);
        EXPECT_EQ(recording.committed, 7);
        EXPECT_EQ(rows, owned ? 0 : 16);
        EXPECT_EQ(bind.cleanup_failed, owned && fail_rollback);
        EXPECT_EQ(recording.pending,
                  owned ? (fail_rollback ? recording.executions.size() : 0)
                        : 2 + recording.executions.size());
        EXPECT_EQ(
            std::count(recording.queries.begin(), recording.queries.end(), "ROLLBACK"),
            owned ? 1 : 0);
      }
}

TEST_F(IntervalPublicRecordingTest,
       SpatialLargeRawArrayAndStreamKeepNullPointersAndVarbyteIdentity) {
  for (auto kind : {RedshiftEncodedKind::kGeometry, RedshiftEncodedKind::kGeography})
    for (bool stream : {false, true}) {
      recording.encoded_target = kind == RedshiftEncodedKind::kGeometry ? 72113 : 72114;
      nanoarrow::UniqueSchema schema;
      EncodedSchema(schema.get(), {kind});
      nanoarrow::UniqueArray array;
      ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
      ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
      std::string value = testing::NumericHex("0102000020e610000088130000");
      const auto zero = testing::NumericHex("00000000000000000000000000000000");
      const auto one = testing::NumericHex("000000000000f03f000000000000f03f");
      for (int i = 0; i < 5000; ++i) value += i % 2 ? one : zero;
      ASSERT_GT(value.size(), 65536);
      ASSERT_LT(value.size(), 500000);
      for (int row = 0; row < 3; ++row) {
        if (row != 1) {
          ASSERT_EQ(ArrowArrayAppendNull(array->children[0], 1), 0);
        } else {
          ArrowBufferView bytes{};
          bytes.data.as_char = value.data();
          bytes.size_bytes = value.size();
          ASSERT_EQ(ArrowArrayAppendBytes(array->children[0], bytes), 0);
        }
        ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
      }
      ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
      ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                          "owned", &error),
                ADBC_STATUS_OK);
      ASSERT_EQ(driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                          ADBC_INGEST_OPTION_MODE_APPEND, &error),
                ADBC_STATUS_OK);
      if (stream) {
        nanoarrow::UniqueArrayStream input;
        adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
        ASSERT_EQ(driver.StatementBindStream(&statement, input.get(), &error),
                  ADBC_STATUS_OK);
        EXPECT_FALSE(input->release);
      } else {
        ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                  ADBC_STATUS_OK);
        EXPECT_FALSE(array->release);
        EXPECT_FALSE(schema->release);
      }
      recording.executions.clear();
      ASSERT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
                ADBC_STATUS_OK)
          << Message();
      ASSERT_EQ(recording.executions.size(), 1);
      const auto& params = recording.executions[0];
      ASSERT_EQ(params.size(), 3);
      for (size_t row = 0; row < 3; ++row) {
        EXPECT_EQ(params[row].oid, 72112);
        EXPECT_EQ(params[row].format, 1);
        EXPECT_EQ(params[row].null, row != 1);
        EXPECT_EQ(params[row].physical_length, row == 1 ? value.size() : 0);
        EXPECT_EQ(params[row].bytes, row == 1 ? value : std::string{});
      }
    }
}

class ErrnoFieldWriter : public PostgresCopyFieldWriter {
 public:
  explicit ErrnoFieldWriter(int code) : code_(code) {}
  ArrowErrorCode Write(ArrowBuffer*, int64_t, ArrowError* error) override {
    ArrowErrorSet(error, "fixed writer failure");
    return code_;
  }

 private:
  int code_;
};
TEST(IntervalParameterStatusTest,
     VendorInvalidArgumentDoesNotRemapOtherFailuresOrFields) {
  for (int code : {0, EINVAL, ENOMEM, EIO, EOVERFLOW})
    for (int family = 0; family < 5; ++family) {
      BindStream bind;
      ArrowBufferInit(&bind.param_buffer.value);
      bind.json_parse_fields = {family == 1, true};
      bind.bytes_fields = {false, false};
      bind.encoded_fields = {family == 2   ? RedshiftEncodedKind::kGeometry
                             : family == 3 ? RedshiftEncodedKind::kGeography
                             : family == 4 ? RedshiftEncodedKind::kHll
                                           : RedshiftEncodedKind::kNone,
                             RedshiftEncodedKind::kHll};
      bind.bind_field_writers.push_back(std::make_unique<ErrnoFieldWriter>(code));
      const auto status = bind.WriteParameter(0, 0);
      AdbcError failure = ADBC_ERROR_INIT;
      EXPECT_EQ(status.ToAdbc(&failure), code == 0 ? ADBC_STATUS_OK
                                         : code == EINVAL && family != 0
                                             ? ADBC_STATUS_INVALID_ARGUMENT
                                             : ADBC_STATUS_INTERNAL);
      if (failure.release) failure.release(&failure);
    }
}

TEST_F(IntervalPublicRecordingTest,
       NativeSuperSchemaMixedRowsAndSingleOriginalExecution) {
  for (const char* query : {"SELECT owned_super", "SELECT owned_super_mixed",
                            "SELECT owned_super_empty", "SELECT owned_super_null"}) {
    ASSERT_EQ(driver.StatementSetSqlQuery(&statement, query, &error), ADBC_STATUS_OK);
    recording.vendor_result_calls = 0;
    nanoarrow::UniqueSchema described;
    ASSERT_EQ(driver.StatementExecuteSchema(&statement, described.get(), &error),
              ADBC_STATUS_OK)
        << Message();
    EXPECT_EQ(recording.vendor_result_calls, 0);
    recording.queries.clear();
    nanoarrow::UniqueArrayStream output;
    ASSERT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
              ADBC_STATUS_OK)
        << Message();
    EXPECT_EQ(recording.prepared_query, query);
    EXPECT_EQ(recording.vendor_result_calls, 1);
    EXPECT_EQ(std::count(recording.queries.begin(), recording.queries.end(), query), 2);
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(output->get_schema(output.get(), schema.get()), 0);
    ArrowError parse{};
    EXPECT_EQ(RedshiftSuperResultSchema::Parse(schema->children[0], &parse), 0)
        << parse.message;
    EXPECT_STREQ(schema->children[0]->format, "U");
    EXPECT_STREQ(described->children[0]->format, "U");
    std::optional<std::string_view> actual_metadata, described_metadata;
    ASSERT_EQ(
        RedshiftDecimalSchema::Metadata(schema->children[0], "ARROW:extension:metadata",
                                        &actual_metadata, &parse),
        0);
    ASSERT_EQ(RedshiftDecimalSchema::Metadata(described->children[0],
                                              "ARROW:extension:metadata",
                                              &described_metadata, &parse),
              0);
    EXPECT_EQ(actual_metadata, described_metadata);
    nanoarrow::UniqueArray rows;
    ASSERT_EQ(output->get_next(output.get(), rows.get()), 0)
        << output->get_last_error(output.get());
    if (std::string_view(query).ends_with("empty")) {
      EXPECT_EQ(rows->release, nullptr);
      continue;
    }
    const bool null = std::string_view(query).ends_with("null");
    EXPECT_EQ(rows->length, null ? 2 : 16);
    nanoarrow::UniqueArrayView view;
    ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), &parse), 0);
    ASSERT_EQ(ArrowArrayViewSetArray(view.get(), rows.get(), &parse), 0);
    for (int64_t row = 0; row < rows->length; ++row) {
      EXPECT_EQ(ArrowArrayViewIsNull(view->children[0], row), null || row < 2);
      if (!null && row >= 2) {
        const auto value = ArrowArrayViewGetStringUnsafe(view->children[0], row);
        EXPECT_EQ(std::string_view(value.data, value.size_bytes), kNativeSuperCells[row]);
      }
      if (schema->n_children == 2) {
        EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view->children[1], row), row + 1);
      }
    }
    nanoarrow::UniqueArray end;
    ASSERT_EQ(output->get_next(output.get(), end.get()), 0);
    EXPECT_EQ(end->release, nullptr);
    EXPECT_EQ(recording.vendor_result_calls, 1);
  }
}
TEST_F(IntervalPublicRecordingTest, NativeSuperDescriptorDriftNeverFallsBackOrRetries) {
  for (int fault = 1; fault <= 6; ++fault) {
    recording.vendor_result_drift = fault;
    recording.vendor_result_calls = 0;
    ASSERT_EQ(driver.StatementSetSqlQuery(&statement, "SELECT owned_super", &error),
              ADBC_STATUS_OK);
    nanoarrow::UniqueArrayStream output;
    EXPECT_NE(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
              ADBC_STATUS_OK);
    EXPECT_EQ(output->release, nullptr);
    EXPECT_EQ(recording.vendor_result_calls, 1);
  }
  recording.vendor_result_drift = 0;
  ASSERT_EQ(driver.StatementSetSqlQuery(&statement, "SELECT owned_super_invalid", &error),
            ADBC_STATUS_OK);
  nanoarrow::UniqueArrayStream output;
  ASSERT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
            ADBC_STATUS_OK)
      << Message();
  nanoarrow::UniqueArray rows;
  EXPECT_NE(output->get_next(output.get(), rows.get()), 0);
  EXPECT_EQ(rows->release, nullptr);
}

TEST_F(IntervalPublicRecordingTest,
       NativeSuperBoundArrayAndStreamStopAfterDecodeFailure) {
  for (bool stream : {false, true})
    for (bool invalid : {false, true}) {
      nanoarrow::UniqueSchema source;
      ArrowSchemaInit(source.get());
      ASSERT_EQ(ArrowSchemaSetTypeStruct(source.get(), 1), 0);
      ASSERT_EQ(ArrowSchemaSetType(source->children[0], NANOARROW_TYPE_INT64), 0);
      ASSERT_EQ(ArrowSchemaSetName(source->children[0], "id"), 0);
      nanoarrow::UniqueArray input;
      ASSERT_EQ(ArrowArrayInitFromSchema(input.get(), source.get(), nullptr), 0);
      ASSERT_EQ(ArrowArrayStartAppending(input.get()), 0);
      for (int value : {7, 8}) {
        ASSERT_EQ(ArrowArrayAppendInt(input->children[0], value), 0);
        ASSERT_EQ(ArrowArrayFinishElement(input.get()), 0);
      }
      ASSERT_EQ(ArrowArrayFinishBuildingDefault(input.get(), nullptr), 0);
      const char* query =
          invalid ? "SELECT owned_super_invalid" : "SELECT owned_super_mixed_param";
      ASSERT_EQ(driver.StatementSetSqlQuery(&statement, query, &error), ADBC_STATUS_OK);
      if (stream) {
        nanoarrow::UniqueArrayStream in;
        adbc::driver::MakeArrayStream(source.get(), input.get(), in.get());
        ASSERT_EQ(driver.StatementBindStream(&statement, in.get(), &error),
                  ADBC_STATUS_OK)
            << Message();
      } else {
        ASSERT_EQ(driver.StatementBind(&statement, input.get(), source.get(), &error),
                  ADBC_STATUS_OK)
            << Message();
      }
      recording.vendor_result_calls = 0;
      recording.executions.clear();
      nanoarrow::UniqueArrayStream output;
      ASSERT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
                ADBC_STATUS_OK)
          << Message();
      EXPECT_EQ(recording.prepared_query, query);
      nanoarrow::UniqueArray first;
      const int status = output->get_next(output.get(), first.get());
      if (invalid) {
        EXPECT_NE(status, 0);
        EXPECT_EQ(first->release, nullptr);
      } else {
        ASSERT_EQ(status, 0) << output->get_last_error(output.get());
        EXPECT_EQ(first->length, 16);
        nanoarrow::UniqueArray second;
        ASSERT_EQ(output->get_next(output.get(), second.get()), 0);
        EXPECT_EQ(second->length, 16);
      }
      nanoarrow::UniqueArray end;
      ASSERT_EQ(output->get_next(output.get(), end.get()), 0);
      EXPECT_EQ(end->release, nullptr);
      EXPECT_EQ(recording.vendor_result_calls, invalid ? 1 : 2);
      ASSERT_EQ(recording.executions.size(), invalid ? 1 : 2);
      int row = 0;
      for (const auto& parameters : recording.executions) {
        ASSERT_EQ(parameters.size(), 1);
        EXPECT_EQ(parameters[0].oid, 67009U);
        EXPECT_EQ(parameters[0].format, 1);
        EXPECT_EQ(parameters[0].bytes.size(), 8);
        std::string expected(8, '\0');
        expected[7] = static_cast<char>(7 + row++);
        EXPECT_EQ(parameters[0].bytes, expected);
      }
    }
}

}  // namespace
}  // namespace adbcpq
