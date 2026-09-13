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

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <arrow-adbc/driver/redshift.h>
#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow.hpp>

#include "driver/framework/objects.h"
#include "validation/adbc_validation_util.h"

using adbc_validation::IsOkStatus;

namespace {

TEST(RedshiftDriverConstructionTest, RejectsPostgreSQLServer) {
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  if (uri == nullptr) {
    GTEST_SKIP() << "ADBC_POSTGRESQL_TEST_URI is not configured";
  }

  struct AdbcError error = {};
  struct AdbcDriver driver = {};
  struct AdbcDatabase database = {};
  ASSERT_EQ(AdbcDriverRedshiftInit(ADBC_VERSION_1_1_0, &driver, &error), ADBC_STATUS_OK);
  ASSERT_THAT(driver.DatabaseNew(&database, &error), IsOkStatus(&error));
  ASSERT_THAT(driver.DatabaseSetOption(&database, "uri", uri, &error),
              IsOkStatus(&error));
  EXPECT_EQ(driver.DatabaseInit(&database, &error), ADBC_STATUS_INVALID_ARGUMENT);
  ASSERT_NE(error.message, nullptr);
  EXPECT_NE(std::string_view(error.message).find("Expected Redshift"),
            std::string_view::npos);
  if (error.release != nullptr) {
    error.release(&error);
    error = {};
  }
  EXPECT_THAT(driver.DatabaseRelease(&database, &error), IsOkStatus(&error));
}

class RedshiftSmokeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* uri = std::getenv("ADBC_REDSHIFT_TEST_URI");
    if (uri == nullptr) {
      GTEST_SKIP() << "ADBC_REDSHIFT_TEST_URI is not configured";
    }

    ASSERT_EQ(AdbcDriverRedshiftInit(ADBC_VERSION_1_1_0, &driver_, &error_),
              ADBC_STATUS_OK);
    ASSERT_THAT(driver_.DatabaseNew(&database_, &error_), IsOkStatus(&error_));
    ASSERT_THAT(driver_.DatabaseSetOption(&database_, "uri", uri, &error_),
                IsOkStatus(&error_));
    ASSERT_THAT(driver_.DatabaseInit(&database_, &error_), IsOkStatus(&error_));
    ASSERT_THAT(driver_.ConnectionNew(&connection_, &error_), IsOkStatus(&error_));
    ASSERT_THAT(driver_.ConnectionInit(&connection_, &database_, &error_),
                IsOkStatus(&error_));
  }

  void TearDown() override {
    if (connection_.private_data != nullptr) {
      EXPECT_THAT(AdbcConnectionRelease(&connection_, &error_), IsOkStatus(&error_));
    }
    if (database_.private_data != nullptr) {
      EXPECT_THAT(AdbcDatabaseRelease(&database_, &error_), IsOkStatus(&error_));
    }
    if (error_.release != nullptr) error_.release(&error_);
  }

  struct AdbcError error_ = {};
  struct AdbcDriver driver_ = {};
  struct AdbcDatabase database_ = {};
  struct AdbcConnection connection_ = {};
};

void ExecuteSql(struct AdbcConnection* connection, std::string_view sql,
                struct AdbcError* error) {
  struct AdbcStatement statement = {};
  ASSERT_THAT(AdbcStatementNew(connection, &statement, error), IsOkStatus(error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, std::string(sql).c_str(), error),
              IsOkStatus(error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, error),
              IsOkStatus(error));
  ASSERT_THAT(AdbcStatementRelease(&statement, error), IsOkStatus(error));
}

TEST_F(RedshiftSmokeTest, DetectsVendorAndExecutesTextResultQuery) {
  const std::string version = adbc_validation::GetDriverVendorVersion(&connection_);
  EXPECT_FALSE(version.empty());
  EXPECT_NE(version, "0.0.0");

  struct AdbcStatement statement = {};
  ASSERT_THAT(AdbcStatementNew(&connection_, &statement, &error_), IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                       "SELECT CAST(42 AS BIGINT) AS answer, "
                                       "CAST('redshift' AS VARCHAR(16)) AS label",
                                       &error_),
              IsOkStatus(&error_));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error_),
              IsOkStatus(&error_));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_EQ(reader.fields.size(), 2U);
  EXPECT_EQ(reader.fields[0].type, NANOARROW_TYPE_INT64);
  EXPECT_EQ(reader.fields[1].type, NANOARROW_TYPE_STRING);

  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(reader.array->release, nullptr);
  ASSERT_EQ(reader.array->length, 1);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 42);
  const ArrowStringView label =
      ArrowArrayViewGetStringUnsafe(reader.array_view->children[1], 0);
  EXPECT_EQ(std::string_view(label.data, label.size_bytes), "redshift");

  EXPECT_THAT(AdbcStatementRelease(&statement, &error_), IsOkStatus(&error_));
}

TEST_F(RedshiftSmokeTest, MapsCoreScalarTypes) {
  struct AdbcStatement statement = {};
  ASSERT_THAT(AdbcStatementNew(&connection_, &statement, &error_), IsOkStatus(&error_));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &statement,
          "SELECT TRUE::BOOLEAN, (-7)::SMALLINT, 42::INTEGER, 9000000000::BIGINT, "
          "1.25::REAL, 2.5::DOUBLE PRECISION, '12.34'::DECIMAL(10, 2), "
          "'2026-09-12'::DATE, '12:34:56'::TIME, "
          "'2026-09-12 12:34:56'::TIMESTAMP, 'redshift'::VARCHAR(16), "
          "TO_VARBYTE('414243', 'hex')",
          &error_),
      IsOkStatus(&error_));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error_),
              IsOkStatus(&error_));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_EQ(reader.fields.size(), 12U);
  EXPECT_EQ(reader.fields[0].type, NANOARROW_TYPE_BOOL);
  EXPECT_EQ(reader.fields[1].type, NANOARROW_TYPE_INT16);
  EXPECT_EQ(reader.fields[2].type, NANOARROW_TYPE_INT32);
  EXPECT_EQ(reader.fields[3].type, NANOARROW_TYPE_INT64);
  EXPECT_EQ(reader.fields[4].type, NANOARROW_TYPE_FLOAT);
  EXPECT_EQ(reader.fields[5].type, NANOARROW_TYPE_DOUBLE);
  EXPECT_EQ(reader.fields[6].type, NANOARROW_TYPE_STRING);
  EXPECT_EQ(reader.fields[7].type, NANOARROW_TYPE_DATE32);
  EXPECT_EQ(reader.fields[8].type, NANOARROW_TYPE_TIME64);
  EXPECT_EQ(reader.fields[9].type, NANOARROW_TYPE_TIMESTAMP);
  EXPECT_EQ(reader.fields[10].type, NANOARROW_TYPE_STRING);
  EXPECT_EQ(reader.fields[11].type, NANOARROW_TYPE_BINARY);

  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(reader.array->release, nullptr);
  ASSERT_EQ(reader.array->length, 1);
  EXPECT_TRUE(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0));
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[1], 0), -7);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[2], 0), 42);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[3], 0), 9000000000);
  const ArrowBufferView bytes =
      ArrowArrayViewGetBytesUnsafe(reader.array_view->children[11], 0);
  EXPECT_EQ(
      std::string_view(reinterpret_cast<const char*>(bytes.data.data), bytes.size_bytes),
      "ABC");

  EXPECT_THAT(AdbcStatementRelease(&statement, &error_), IsOkStatus(&error_));
}

TEST_F(RedshiftSmokeTest, PreservesNullResults) {
  struct AdbcStatement statement = {};
  ASSERT_THAT(AdbcStatementNew(&connection_, &statement, &error_), IsOkStatus(&error_));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&statement,
                               "SELECT 1::INTEGER AS id, 'value'::VARCHAR(16) AS label "
                               "UNION ALL SELECT NULL::INTEGER, NULL::VARCHAR(16) "
                               "ORDER BY id NULLS LAST",
                               &error_),
      IsOkStatus(&error_));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error_),
              IsOkStatus(&error_));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_EQ(reader.fields.size(), 2U);
  EXPECT_EQ(reader.fields[0].type, NANOARROW_TYPE_INT32);
  EXPECT_EQ(reader.fields[1].type, NANOARROW_TYPE_STRING);
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(reader.array->release, nullptr);
  ASSERT_EQ(reader.array->length, 2);
  EXPECT_FALSE(ArrowArrayViewIsNull(reader.array_view->children[0], 0));
  EXPECT_FALSE(ArrowArrayViewIsNull(reader.array_view->children[1], 0));
  EXPECT_TRUE(ArrowArrayViewIsNull(reader.array_view->children[0], 1));
  EXPECT_TRUE(ArrowArrayViewIsNull(reader.array_view->children[1], 1));

  EXPECT_THAT(AdbcStatementRelease(&statement, &error_), IsOkStatus(&error_));
}

TEST_F(RedshiftSmokeTest, RecoversAfterQueryError) {
  struct AdbcStatement statement = {};
  ASSERT_THAT(AdbcStatementNew(&connection_, &statement, &error_), IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELEC 1", &error_),
              IsOkStatus(&error_));
  EXPECT_NE(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error_),
            ADBC_STATUS_OK);
  ASSERT_NE(error_.message, nullptr);
  if (error_.release != nullptr) {
    error_.release(&error_);
    error_ = {};
  }

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT 42::INTEGER", &error_),
              IsOkStatus(&error_));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error_),
              IsOkStatus(&error_));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->length, 1);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 42);
  EXPECT_THAT(AdbcStatementRelease(&statement, &error_), IsOkStatus(&error_));
}

TEST_F(RedshiftSmokeTest, ReturnsLargerResultCompletely) {
  struct AdbcStatement statement = {};
  ASSERT_THAT(AdbcStatementNew(&connection_, &statement, &error_), IsOkStatus(&error_));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&statement,
                               "WITH RECURSIVE seq(n) AS ("
                               "SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 1024) "
                               "SELECT n FROM seq ORDER BY n",
                               &error_),
      IsOkStatus(&error_));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error_),
              IsOkStatus(&error_));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_EQ(reader.fields.size(), 1U);
  EXPECT_EQ(reader.fields[0].type, NANOARROW_TYPE_INT32);
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(reader.array->release, nullptr);
  ASSERT_EQ(reader.array->length, 1024);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 1);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 1023), 1024);
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  EXPECT_EQ(reader.array->release, nullptr);

  EXPECT_THAT(AdbcStatementRelease(&statement, &error_), IsOkStatus(&error_));
}

TEST_F(RedshiftSmokeTest, ReturnsSchemaForEmptyResult) {
  struct AdbcStatement statement = {};
  ASSERT_THAT(AdbcStatementNew(&connection_, &statement, &error_), IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "SELECT 1::INTEGER AS id WHERE 1 = 0", &error_),
              IsOkStatus(&error_));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error_),
              IsOkStatus(&error_));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_EQ(reader.fields.size(), 1U);
  EXPECT_EQ(reader.fields[0].type, NANOARROW_TYPE_INT32);
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  EXPECT_EQ(reader.array->release, nullptr);

  EXPECT_THAT(AdbcStatementRelease(&statement, &error_), IsOkStatus(&error_));
}

TEST_F(RedshiftSmokeTest, MetadataAndTableSchema) {
  constexpr std::string_view kTableName = "adbc_redshift_mvp_metadata";
  ExecuteSql(&connection_, "DROP TABLE IF EXISTS adbc_redshift_mvp_metadata", &error_);
  ExecuteSql(&connection_,
             "CREATE TABLE adbc_redshift_mvp_metadata ("
             "id BIGINT, label VARCHAR(32), amount DECIMAL(10, 2), active BOOLEAN)",
             &error_);

  nanoarrow::UniqueSchema schema;
  ASSERT_THAT(AdbcConnectionGetTableSchema(&connection_, nullptr, "public",
                                           kTableName.data(), schema.get(), &error_),
              IsOkStatus(&error_));
  ASSERT_EQ(schema->n_children, 4);
  EXPECT_STREQ(schema->children[0]->name, "id");
  EXPECT_STREQ(schema->children[0]->format, "l");
  EXPECT_STREQ(schema->children[1]->name, "label");
  EXPECT_STREQ(schema->children[1]->format, "u");
  EXPECT_STREQ(schema->children[2]->name, "amount");
  EXPECT_STREQ(schema->children[2]->format, "u");
  EXPECT_STREQ(schema->children[3]->name, "active");
  EXPECT_STREQ(schema->children[3]->format, "b");

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcConnectionGetObjects(&connection_, ADBC_OBJECT_DEPTH_COLUMNS, nullptr,
                                       "public", kTableName.data(), nullptr, nullptr,
                                       &reader.stream.value, &error_),
              IsOkStatus(&error_));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  auto objects = adbc_validation::GetObjectsReader{&reader.array_view.value};
  ASSERT_NE(*objects, nullptr);
  auto* table = InternalAdbcGetObjectsDataGetTableByName(*objects, "dev", "public",
                                                         kTableName.data());
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->n_table_columns, 4);
  EXPECT_EQ(table->n_table_constraints, 0);

  ExecuteSql(&connection_, "DROP TABLE adbc_redshift_mvp_metadata", &error_);
}

TEST_F(RedshiftSmokeTest, ReportsMissingTableSchema) {
  nanoarrow::UniqueSchema schema;
  EXPECT_EQ(AdbcConnectionGetTableSchema(&connection_, nullptr, "public",
                                         "adbc_redshift_mvp_table_does_not_exist",
                                         schema.get(), &error_),
            ADBC_STATUS_NOT_FOUND);
  ASSERT_NE(error_.message, nullptr);
  if (error_.release != nullptr) {
    error_.release(&error_);
    error_ = {};
  }
}

TEST_F(RedshiftSmokeTest, ReportsOnlySupportedTableTypes) {
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcConnectionGetTableTypes(&connection_, &reader.stream.value, &error_),
              IsOkStatus(&error_));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(reader.array->release, nullptr);

  std::vector<std::string> table_types;
  for (int64_t i = 0; i < reader.array->length; i++) {
    const ArrowStringView value =
        ArrowArrayViewGetStringUnsafe(reader.array_view->children[0], i);
    table_types.emplace_back(value.data, value.size_bytes);
  }
  EXPECT_THAT(table_types, ::testing::UnorderedElementsAre("table", "view"));
}

TEST_F(RedshiftSmokeTest, CommitsAndRollsBackExplicitTransactions) {
  ASSERT_THAT(AdbcConnectionSetOption(&connection_, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                                      ADBC_OPTION_VALUE_DISABLED, &error_),
              IsOkStatus(&error_));
  ExecuteSql(&connection_, "SELECT 1", &error_);
  ASSERT_THAT(AdbcConnectionCommit(&connection_, &error_), IsOkStatus(&error_));
  ExecuteSql(&connection_, "SELECT 1", &error_);
  ASSERT_THAT(AdbcConnectionRollback(&connection_, &error_), IsOkStatus(&error_));
}

TEST_F(RedshiftSmokeTest, RejectsSessionIsolationOverrides) {
  EXPECT_EQ(AdbcConnectionSetOption(&connection_, ADBC_CONNECTION_OPTION_ISOLATION_LEVEL,
                                    ADBC_OPTION_ISOLATION_LEVEL_SERIALIZABLE, &error_),
            ADBC_STATUS_NOT_IMPLEMENTED);
  ASSERT_NE(error_.message, nullptr);
  EXPECT_NE(std::string_view(error_.message).find("database level"),
            std::string_view::npos);
  if (error_.release != nullptr) {
    error_.release(&error_);
    error_ = {};
  }

  EXPECT_THAT(
      AdbcConnectionSetOption(&connection_, ADBC_CONNECTION_OPTION_ISOLATION_LEVEL,
                              ADBC_OPTION_ISOLATION_LEVEL_DEFAULT, &error_),
      IsOkStatus(&error_));
}

TEST_F(RedshiftSmokeTest, ExecutesBoundParameterQuery) {
  nanoarrow::UniqueSchema bind_schema;
  ArrowSchemaInit(bind_schema.get());
  ASSERT_THAT(ArrowSchemaSetTypeStruct(bind_schema.get(), 1),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(bind_schema->children[0], NANOARROW_TYPE_INT32),
              adbc_validation::IsOkErrno());

  nanoarrow::UniqueArray bind;
  ASSERT_THAT(ArrowArrayInitFromSchema(bind.get(), bind_schema.get(), nullptr),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayStartAppending(bind.get()), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayAppendInt(bind->children[0], 41), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayFinishElement(bind.get()), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayFinishBuildingDefault(bind.get(), nullptr),
              adbc_validation::IsOkErrno());

  struct AdbcStatement statement = {};
  ASSERT_THAT(AdbcStatementNew(&connection_, &statement, &error_), IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT $1 + 1", &error_),
              IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementBind(&statement, bind.get(), bind_schema.get(), &error_),
              IsOkStatus(&error_));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error_),
              IsOkStatus(&error_));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->length, 1);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 42);

  EXPECT_THAT(AdbcStatementRelease(&statement, &error_), IsOkStatus(&error_));
}

TEST_F(RedshiftSmokeTest, BulkIngestUsesParameterizedInsert) {
  constexpr std::string_view kTableName = "adbc_redshift_mvp_ingest";
  ExecuteSql(&connection_, "DROP TABLE IF EXISTS adbc_redshift_mvp_ingest", &error_);

  nanoarrow::UniqueSchema bind_schema;
  ArrowSchemaInit(bind_schema.get());
  ASSERT_THAT(ArrowSchemaSetTypeStruct(bind_schema.get(), 2),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(bind_schema->children[0], NANOARROW_TYPE_INT32),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetName(bind_schema->children[0], "id"),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(bind_schema->children[1], NANOARROW_TYPE_STRING),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetName(bind_schema->children[1], "label"),
              adbc_validation::IsOkErrno());

  nanoarrow::UniqueArray bind;
  ASSERT_THAT((adbc_validation::MakeBatch<int32_t, std::string>(
                  bind_schema.get(), bind.get(), nullptr, {1, 2}, {"one", "two"})),
              adbc_validation::IsOkErrno());

  struct AdbcStatement ingest = {};
  ASSERT_THAT(AdbcStatementNew(&connection_, &ingest, &error_), IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementSetOption(&ingest, ADBC_INGEST_OPTION_TARGET_TABLE,
                                     kTableName.data(), &error_),
              IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementBind(&ingest, bind.get(), bind_schema.get(), &error_),
              IsOkStatus(&error_));
  int64_t rows_affected = -1;
  ASSERT_THAT(AdbcStatementExecuteQuery(&ingest, nullptr, &rows_affected, &error_),
              IsOkStatus(&error_));
  EXPECT_EQ(rows_affected, 2);
  EXPECT_THAT(AdbcStatementRelease(&ingest, &error_), IsOkStatus(&error_));

  struct AdbcStatement query = {};
  ASSERT_THAT(AdbcStatementNew(&connection_, &query, &error_), IsOkStatus(&error_));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &query, "SELECT id, label FROM adbc_redshift_mvp_ingest ORDER BY id", &error_),
      IsOkStatus(&error_));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&query, &reader.stream.value,
                                        &reader.rows_affected, &error_),
              IsOkStatus(&error_));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NO_FATAL_FAILURE(
      adbc_validation::CompareArray<int32_t>(reader.array_view->children[0], {1, 2}));
  ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareArray<std::string>(
      reader.array_view->children[1], {"one", "two"}));
  EXPECT_THAT(AdbcStatementRelease(&query, &error_), IsOkStatus(&error_));

  ExecuteSql(&connection_, "DROP TABLE adbc_redshift_mvp_ingest", &error_);
}

TEST_F(RedshiftSmokeTest, BulkIngestRollsBackWholeBatchOnError) {
  constexpr std::string_view kTableName = "adbc_redshift_mvp_ingest_atomic";
  ExecuteSql(&connection_, "DROP TABLE IF EXISTS adbc_redshift_mvp_ingest_atomic",
             &error_);
  ExecuteSql(&connection_,
             "CREATE TABLE adbc_redshift_mvp_ingest_atomic (id INTEGER NOT NULL)",
             &error_);

  nanoarrow::UniqueSchema bind_schema;
  ArrowSchemaInit(bind_schema.get());
  ASSERT_THAT(ArrowSchemaSetTypeStruct(bind_schema.get(), 1),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(bind_schema->children[0], NANOARROW_TYPE_INT32),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetName(bind_schema->children[0], "id"),
              adbc_validation::IsOkErrno());

  nanoarrow::UniqueArray bind;
  ASSERT_THAT((adbc_validation::MakeBatch<int32_t>(
                  bind_schema.get(), bind.get(), nullptr,
                  std::vector<std::optional<int32_t>>{1, std::nullopt})),
              adbc_validation::IsOkErrno());

  struct AdbcStatement ingest = {};
  ASSERT_THAT(AdbcStatementNew(&connection_, &ingest, &error_), IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementSetOption(&ingest, ADBC_INGEST_OPTION_TARGET_TABLE,
                                     kTableName.data(), &error_),
              IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementSetOption(&ingest, ADBC_INGEST_OPTION_MODE,
                                     ADBC_INGEST_OPTION_MODE_APPEND, &error_),
              IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementBind(&ingest, bind.get(), bind_schema.get(), &error_),
              IsOkStatus(&error_));

  int64_t rows_affected = -1;
  EXPECT_NE(AdbcStatementExecuteQuery(&ingest, nullptr, &rows_affected, &error_),
            ADBC_STATUS_OK);
  EXPECT_EQ(rows_affected, 0);
  if (error_.release != nullptr) {
    error_.release(&error_);
    error_ = {};
  }
  EXPECT_THAT(AdbcStatementRelease(&ingest, &error_), IsOkStatus(&error_));

  struct AdbcStatement query = {};
  ASSERT_THAT(AdbcStatementNew(&connection_, &query, &error_), IsOkStatus(&error_));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &query, "SELECT COUNT(*) FROM adbc_redshift_mvp_ingest_atomic", &error_),
      IsOkStatus(&error_));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&query, &reader.stream.value,
                                        &reader.rows_affected, &error_),
              IsOkStatus(&error_));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 0);
  EXPECT_THAT(AdbcStatementRelease(&query, &error_), IsOkStatus(&error_));

  ExecuteSql(&connection_, "DROP TABLE adbc_redshift_mvp_ingest_atomic", &error_);
}

class RedshiftBenchmarkTest : public RedshiftSmokeTest {
 protected:
  void SetUp() override {
    if (std::getenv("ADBC_REDSHIFT_BENCHMARK") == nullptr) {
      GTEST_SKIP() << "ADBC_REDSHIFT_BENCHMARK is not configured";
    }
    RedshiftSmokeTest::SetUp();
  }

  void TearDown() override {
    if (!table_name_.empty() && connection_.private_data != nullptr) {
      struct AdbcError cleanup_error = {};
      ExecuteSql(&connection_, "DROP TABLE IF EXISTS " + table_name_, &cleanup_error);
      if (cleanup_error.release != nullptr) cleanup_error.release(&cleanup_error);
    }
    RedshiftSmokeTest::TearDown();
  }

  std::string table_name_;
};

TEST_F(RedshiftBenchmarkTest, PreparedInsertThroughput) {
  constexpr int32_t kRows = 1000;
  table_name_ = "adbc_redshift_insert_benchmark_" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());

  nanoarrow::UniqueSchema schema;
  ArrowSchemaInit(schema.get());
  ASSERT_THAT(ArrowSchemaSetTypeStruct(schema.get(), 1), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "id"),
              adbc_validation::IsOkErrno());

  std::vector<std::optional<int32_t>> ids;
  ids.reserve(kRows);
  for (int32_t i = 0; i < kRows; ++i) ids.emplace_back(i);
  nanoarrow::UniqueArray array;
  ASSERT_THAT((adbc_validation::MakeBatch<int32_t>(schema.get(), array.get(), nullptr,
                                                   ids)),
              adbc_validation::IsOkErrno());

  struct AdbcStatement ingest = {};
  ASSERT_THAT(AdbcStatementNew(&connection_, &ingest, &error_), IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementSetOption(&ingest, ADBC_INGEST_OPTION_TARGET_TABLE,
                                     table_name_.c_str(), &error_),
              IsOkStatus(&error_));
  const auto start = std::chrono::steady_clock::now();
  ASSERT_THAT(AdbcStatementBind(&ingest, array.get(), schema.get(), &error_),
              IsOkStatus(&error_));
  int64_t rows_affected = -1;
  ASSERT_THAT(AdbcStatementExecuteQuery(&ingest, nullptr, &rows_affected, &error_),
              IsOkStatus(&error_));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(rows_affected, kRows);
  EXPECT_THAT(AdbcStatementRelease(&ingest, &error_), IsOkStatus(&error_));

  struct AdbcStatement verify = {};
  ASSERT_THAT(AdbcStatementNew(&connection_, &verify, &error_), IsOkStatus(&error_));
  const std::string count_query = "SELECT COUNT(*) FROM " + table_name_;
  ASSERT_THAT(AdbcStatementSetSqlQuery(&verify, count_query.c_str(), &error_),
              IsOkStatus(&error_));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&verify, &reader.stream.value,
                                        &reader.rows_affected, &error_),
              IsOkStatus(&error_));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->length, 1);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), kRows);
  EXPECT_THAT(AdbcStatementRelease(&verify, &error_), IsOkStatus(&error_));

  const double seconds = std::chrono::duration<double>(elapsed).count();
  ASSERT_GT(seconds, 0.0);
  const double rows_per_second = static_cast<double>(kRows) / seconds;
  RecordProperty("rows_per_second", rows_per_second);
  std::cout << "Redshift prepared-insert benchmark: " << kRows << " rows in "
            << seconds << " s (" << rows_per_second << " rows/s)\n";
}

}  // namespace
