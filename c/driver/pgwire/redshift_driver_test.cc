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

#include <arrow-adbc/adbc.h>
#include <arrow-adbc/driver/redshift.h>
#include <gtest/gtest.h>

extern "C" AdbcStatusCode AdbcDriverInit(int version, void* raw_driver,
                                          struct AdbcError* error);
extern "C" AdbcStatusCode AdbcDriverPostgresqlInit(int version, void* raw_driver,
                                                    struct AdbcError* error);

TEST(RedshiftArtifactTest, CommonEntrypointSelectsRedshiftFactory) {
  struct AdbcDriver common = {};
  struct AdbcDriver redshift = {};
  struct AdbcDriver postgresql = {};
  struct AdbcError error = {};

  ASSERT_EQ(AdbcDriverInit(ADBC_VERSION_1_1_0, &common, &error), ADBC_STATUS_OK);
  ASSERT_EQ(AdbcDriverRedshiftInit(ADBC_VERSION_1_1_0, &redshift, &error),
            ADBC_STATUS_OK);
  ASSERT_EQ(AdbcDriverPostgresqlInit(ADBC_VERSION_1_1_0, &postgresql, &error),
            ADBC_STATUS_OK);

  EXPECT_EQ(common.DatabaseNew, redshift.DatabaseNew);
  EXPECT_NE(common.DatabaseNew, postgresql.DatabaseNew);
}
