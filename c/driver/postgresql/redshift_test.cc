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

#include <arrow-adbc/adbc.h>
#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>

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

}  // namespace
