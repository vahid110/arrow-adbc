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

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow.hpp>

#include "driver/framework/objects.h"
#include "validation/adbc_validation_util.h"

using adbc_validation::IsOkStatus;

namespace {

class RedshiftSmokeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* uri = std::getenv("ADBC_REDSHIFT_TEST_URI");
    if (uri == nullptr) {
      GTEST_SKIP() << "ADBC_REDSHIFT_TEST_URI is not configured";
    }

    ASSERT_THAT(AdbcDatabaseNew(&database_, &error_), IsOkStatus(&error_));
    ASSERT_THAT(AdbcDatabaseSetOption(&database_, "uri", uri, &error_),
                IsOkStatus(&error_));
    ASSERT_THAT(AdbcDatabaseInit(&database_, &error_), IsOkStatus(&error_));
    ASSERT_THAT(AdbcConnectionNew(&connection_, &error_), IsOkStatus(&error_));
    ASSERT_THAT(AdbcConnectionInit(&connection_, &database_, &error_),
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
  ASSERT_THAT(AdbcStatementNew(&connection_, &statement, &error_),
              IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement,
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
  ASSERT_THAT(AdbcStatementNew(&connection_, &statement, &error_),
              IsOkStatus(&error_));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &statement,
          "SELECT TRUE::BOOLEAN, (-7)::SMALLINT, 42::INTEGER, 9000000000::BIGINT, "
          "1.25::REAL, 2.5::DOUBLE PRECISION, '12.34'::DECIMAL(10, 2), "
          "'2026-09-12'::DATE, '12:34:56'::TIME, "
          "'2026-09-12 12:34:56'::TIMESTAMP, 'redshift'::VARCHAR(16)",
          &error_),
      IsOkStatus(&error_));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error_),
              IsOkStatus(&error_));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_EQ(reader.fields.size(), 11U);
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

  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(reader.array->release, nullptr);
  ASSERT_EQ(reader.array->length, 1);
  EXPECT_TRUE(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0));
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[1], 0), -7);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[2], 0), 42);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[3], 0), 9000000000);

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
  ASSERT_THAT(AdbcConnectionGetObjects(
                  &connection_, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, "public",
                  kTableName.data(), nullptr, nullptr, &reader.stream.value, &error_),
              IsOkStatus(&error_));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  auto objects = adbc_validation::GetObjectsReader{&reader.array_view.value};
  ASSERT_NE(*objects, nullptr);
  auto* table = InternalAdbcGetObjectsDataGetTableByName(
      *objects, "dev", "public", kTableName.data());
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->n_table_columns, 4);
  EXPECT_EQ(table->n_table_constraints, 0);

  ExecuteSql(&connection_, "DROP TABLE adbc_redshift_mvp_metadata", &error_);
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

}  // namespace
