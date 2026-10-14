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

#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <arrow-adbc/adbc.h>
#include "driver/framework/base_driver.h"
#include "driver/framework/connection.h"
#include "driver/framework/database.h"
#include "driver/framework/statement.h"

// Self-contained version of the Handle
static inline void clean_up(AdbcDriver* ptr) { ptr->release(ptr, nullptr); }

static inline void clean_up(AdbcDatabase* ptr) {
  ptr->private_driver->DatabaseRelease(ptr, nullptr);
}

static inline void clean_up(AdbcConnection* ptr) {
  ptr->private_driver->ConnectionRelease(ptr, nullptr);
}

static inline void clean_up(AdbcStatement* ptr) {
  ptr->private_driver->StatementRelease(ptr, nullptr);
}

template <typename T>
class Handle {
 public:
  explicit Handle(T* value) : value_(value) {}

  ~Handle() { clean_up(value_); }

 private:
  T* value_;
};

namespace {

class BaseVoidDatabase : public adbc::driver::BaseDatabase<BaseVoidDatabase> {
 public:
  [[maybe_unused]] constexpr static std::string_view kErrorPrefix = "[void]";
};

class BaseVoidConnection : public adbc::driver::BaseConnection<BaseVoidConnection> {
 public:
  [[maybe_unused]] constexpr static std::string_view kErrorPrefix = "[void]";
};

class BaseVoidStatement : public adbc::driver::BaseStatement<BaseVoidStatement> {
 public:
  [[maybe_unused]] constexpr static std::string_view kErrorPrefix = "[void]";
};

using BaseVoidDriver =
    adbc::driver::Driver<BaseVoidDatabase, BaseVoidConnection, BaseVoidStatement>;
}  // namespace

AdbcStatusCode BaseVoidDriverInitFunc(int version, void* raw_driver, AdbcError* error) {
  return BaseVoidDriver::Init(version, raw_driver, error);
}

TEST(TestDriverBase, StatusFromAdbcPreservesSqlStateAndFrameworkDetails) {
  adbc::driver::Status source(ADBC_STATUS_INTEGRITY, "original constraint failure");
  source.SetSqlState("23514");
  source.AddDetail("constraint", "positive_id");
  source.AddDetail("binary", std::string("a\0b", 3));
  AdbcError error = ADBC_ERROR_INIT;
  ASSERT_EQ(source.ToAdbc(&error), ADBC_STATUS_INTEGRITY);
  auto converted = adbc::driver::Status::FromAdbc(ADBC_STATUS_INTEGRITY, error);
  EXPECT_EQ(error.release, nullptr);
  EXPECT_EQ(error.message, nullptr);
  ASSERT_EQ(converted.CDetailCount(), 2);
  const auto constraint = converted.CDetail(0);
  EXPECT_STREQ(constraint.key, "constraint");
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(constraint.value),
                        constraint.value_length),
            "positive_id");
  const auto binary = converted.CDetail(1);
  EXPECT_STREQ(binary.key, "binary");
  ASSERT_EQ(binary.value_length, 3U);
  EXPECT_EQ(std::memcmp(binary.value, "a\0b", 3), 0);

  AdbcError roundtrip = ADBC_ERROR_INIT;
  ASSERT_EQ(converted.ToAdbc(&roundtrip), ADBC_STATUS_INTEGRITY);
  EXPECT_STREQ(roundtrip.message, "original constraint failure");
  EXPECT_EQ(std::string(roundtrip.sqlstate, 5), "23514");
  ASSERT_NE(roundtrip.release, nullptr);
  roundtrip.release(&roundtrip);
}

TEST(TestDriverBase, StatusFromAdbcDoesNotCastForeignPrivateData) {
  bool released = false;
  AdbcError error = ADBC_ERROR_INIT;
  error.message = const_cast<char*>("foreign failure");
  std::memcpy(error.sqlstate, "22008", 5);
  // The ADBC private-data marker alone does not identify a Status payload.
  error.private_data = &released;
  error.release = [](AdbcError* error) {
    *static_cast<bool*>(error->private_data) = true;
    std::memset(error, 0, sizeof(*error));
  };
  auto converted = adbc::driver::Status::FromAdbc(ADBC_STATUS_INVALID_DATA, error);
  EXPECT_TRUE(released);
  EXPECT_EQ(error.release, nullptr);
  EXPECT_EQ(converted.CDetailCount(), 0);
  AdbcError roundtrip = ADBC_ERROR_INIT;
  ASSERT_EQ(converted.ToAdbc(&roundtrip), ADBC_STATUS_INVALID_DATA);
  EXPECT_STREQ(roundtrip.message, "foreign failure");
  EXPECT_EQ(std::string(roundtrip.sqlstate, 5), "22008");
  ASSERT_NE(roundtrip.release, nullptr);
  roundtrip.release(&roundtrip);
}

TEST(TestDriverBase, StatusFromAdbcAcceptsLegacyErrorPrefix) {
  // A 1.0 caller can allocate only the public legacy prefix. ASan catches
  // accidental reads of newer private fields beyond this allocation.
  // GCC 14+ flags the intentionally undersized allocation (-Walloc-size).
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Walloc-size"
#endif
  std::unique_ptr<AdbcError, decltype(&std::free)> legacy(
      static_cast<AdbcError*>(std::malloc(ADBC_ERROR_1_0_0_SIZE)), &std::free);
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#pragma GCC diagnostic pop
#endif
  ASSERT_NE(legacy, nullptr);
  std::memset(legacy.get(), 0, ADBC_ERROR_1_0_0_SIZE);
  legacy->message = const_cast<char*>("legacy failure");
  legacy->vendor_code = 123;
  std::memcpy(legacy->sqlstate, "42701", 5);
  legacy->release = [](AdbcError* error) {
    error->message = nullptr;
    error->vendor_code = 456;
    error->release = nullptr;
  };
  auto converted = adbc::driver::Status::FromAdbc(ADBC_STATUS_ALREADY_EXISTS, *legacy);
  EXPECT_EQ(legacy->vendor_code, 456);
  EXPECT_EQ(legacy->release, nullptr);
  EXPECT_EQ(converted.CDetailCount(), 0);
  AdbcError roundtrip = ADBC_ERROR_INIT;
  ASSERT_EQ(converted.ToAdbc(&roundtrip), ADBC_STATUS_ALREADY_EXISTS);
  EXPECT_STREQ(roundtrip.message, "legacy failure");
  EXPECT_EQ(std::string(roundtrip.sqlstate, 5), "42701");
  ASSERT_NE(roundtrip.release, nullptr);
  roundtrip.release(&roundtrip);
}

TEST(TestDriverBase, TestBaseVoidDriverMethods) {
  // Checks that wires are plugged in for a framework-based driver based only on what is
  // available in base_driver.h

  struct AdbcDriver driver;
  memset(&driver, 0, sizeof(driver));
  ASSERT_EQ(BaseVoidDriverInitFunc(ADBC_VERSION_1_1_0, &driver, nullptr), ADBC_STATUS_OK);
  Handle<AdbcDriver> driver_handle(&driver);

  // Database methods are only option related
  struct AdbcDatabase database;
  memset(&database, 0, sizeof(database));
  ASSERT_EQ(driver.DatabaseNew(&database, nullptr), ADBC_STATUS_OK);
  database.private_driver = &driver;
  Handle<AdbcDatabase> database_handle(&database);
  ASSERT_EQ(driver.DatabaseInit(&database, nullptr), ADBC_STATUS_OK);

  // Test connection methods
  struct AdbcConnection connection;
  memset(&connection, 0, sizeof(connection));
  ASSERT_EQ(driver.ConnectionNew(&connection, nullptr), ADBC_STATUS_OK);
  connection.private_driver = &driver;
  Handle<AdbcConnection> connection_handle(&connection);
  ASSERT_EQ(driver.ConnectionInit(&connection, &database, nullptr), ADBC_STATUS_OK);

  EXPECT_EQ(driver.ConnectionCommit(&connection, nullptr), ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.ConnectionGetInfo(&connection, nullptr, 0, nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.ConnectionGetObjects(&connection, 0, nullptr, nullptr, 0, nullptr,
                                        nullptr, nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.ConnectionGetTableSchema(&connection, nullptr, nullptr, nullptr,
                                            nullptr, nullptr),
            ADBC_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(driver.ConnectionGetTableTypes(&connection, nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.ConnectionReadPartition(&connection, nullptr, 0, nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.ConnectionRollback(&connection, nullptr), ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.ConnectionCancel(&connection, nullptr), ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.ConnectionGetStatistics(&connection, nullptr, nullptr, nullptr, 0,
                                           nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.ConnectionGetStatisticNames(&connection, nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);

  // Test statement methods
  struct AdbcStatement statement;
  memset(&statement, 0, sizeof(statement));
  ASSERT_EQ(driver.StatementNew(&connection, &statement, nullptr), ADBC_STATUS_OK);
  statement.private_driver = &driver;
  Handle<AdbcStatement> statement_handle(&statement);

  EXPECT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.StatementExecuteSchema(&statement, nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.StatementPrepare(&statement, nullptr), ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.StatementSetSqlQuery(&statement, "", nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.StatementSetSubstraitPlan(&statement, nullptr, 0, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.StatementBind(&statement, nullptr, nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.StatementBindStream(&statement, nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.StatementCancel(&statement, nullptr), ADBC_STATUS_NOT_IMPLEMENTED);
}

namespace {

class VoidDatabase : public adbc::driver::Database<VoidDatabase> {
 public:
  [[maybe_unused]] constexpr static std::string_view kErrorPrefix = "[void]";
};

class VoidConnection : public adbc::driver::Connection<VoidConnection> {
 public:
  [[maybe_unused]] constexpr static std::string_view kErrorPrefix = "[void]";
};

class VoidStatement : public adbc::driver::Statement<VoidStatement> {
 public:
  [[maybe_unused]] constexpr static std::string_view kErrorPrefix = "[void]";
};

using VoidDriver = adbc::driver::Driver<VoidDatabase, VoidConnection, VoidStatement>;
}  // namespace

AdbcStatusCode VoidDriverInitFunc(int version, void* raw_driver, AdbcError* error) {
  return VoidDriver::Init(version, raw_driver, error);
}

TEST(TestDriverBase, TestVoidDriverMethods) {
  // Checks that wires are plugged in for a framework-based driver based on
  // the more-batteries-included Database, Connection, and Statement

  struct AdbcDriver driver;
  memset(&driver, 0, sizeof(driver));
  ASSERT_EQ(VoidDriverInitFunc(ADBC_VERSION_1_1_0, &driver, nullptr), ADBC_STATUS_OK);
  Handle<AdbcDriver> driver_handle(&driver);

  // Database methods are only option related
  struct AdbcDatabase database;
  memset(&database, 0, sizeof(database));
  ASSERT_EQ(driver.DatabaseNew(&database, nullptr), ADBC_STATUS_OK);
  database.private_driver = &driver;
  Handle<AdbcDatabase> database_handle(&database);
  ASSERT_EQ(driver.DatabaseInit(&database, nullptr), ADBC_STATUS_OK);

  // Test connection methods
  struct AdbcConnection connection;
  memset(&connection, 0, sizeof(connection));
  ASSERT_EQ(driver.ConnectionNew(&connection, nullptr), ADBC_STATUS_OK);
  connection.private_driver = &driver;
  Handle<AdbcConnection> connection_handle(&connection);
  ASSERT_EQ(driver.ConnectionInit(&connection, &database, nullptr), ADBC_STATUS_OK);

  EXPECT_EQ(driver.ConnectionCommit(&connection, nullptr), ADBC_STATUS_INVALID_STATE);
  EXPECT_EQ(driver.ConnectionGetInfo(&connection, nullptr, 0, nullptr, nullptr),
            ADBC_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(driver.ConnectionGetObjects(&connection, 0, nullptr, nullptr, 0, nullptr,
                                        nullptr, nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.ConnectionGetTableSchema(&connection, nullptr, nullptr, nullptr,
                                            nullptr, nullptr),
            ADBC_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(driver.ConnectionGetTableTypes(&connection, nullptr, nullptr),
            ADBC_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(driver.ConnectionReadPartition(&connection, nullptr, 0, nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.ConnectionRollback(&connection, nullptr), ADBC_STATUS_INVALID_STATE);
  EXPECT_EQ(driver.ConnectionCancel(&connection, nullptr), ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.ConnectionGetStatistics(&connection, nullptr, nullptr, nullptr, 0,
                                           nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.ConnectionGetStatisticNames(&connection, nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);

  // Test statement methods
  struct AdbcStatement statement;
  memset(&statement, 0, sizeof(statement));
  ASSERT_EQ(driver.StatementNew(&connection, &statement, nullptr), ADBC_STATUS_OK);
  statement.private_driver = &driver;
  Handle<AdbcStatement> statement_handle(&statement);

  EXPECT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, nullptr),
            ADBC_STATUS_INVALID_STATE);
  EXPECT_EQ(driver.StatementExecuteSchema(&statement, nullptr, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.StatementPrepare(&statement, nullptr), ADBC_STATUS_INVALID_STATE);
  EXPECT_EQ(driver.StatementSetSqlQuery(&statement, "", nullptr), ADBC_STATUS_OK);
  EXPECT_EQ(driver.StatementSetSubstraitPlan(&statement, nullptr, 0, nullptr),
            ADBC_STATUS_NOT_IMPLEMENTED);
  EXPECT_EQ(driver.StatementBind(&statement, nullptr, nullptr, nullptr),
            ADBC_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(driver.StatementBindStream(&statement, nullptr, nullptr),
            ADBC_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(driver.StatementCancel(&statement, nullptr), ADBC_STATUS_NOT_IMPLEMENTED);
}
