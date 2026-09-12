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

#include "metadata.h"

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace adbcpq {
namespace {

TEST(MetadataQuerySetTest, TableTypesAreBackendOwned) {
  MetadataQuerySet postgresql(
      adbc::driver::pgwire::BackendProfile::PostgreSQL());
  MetadataQuerySet redshift(adbc::driver::pgwire::BackendProfile::Redshift());

  EXPECT_EQ(postgresql.TableTypeNames().size(), 6);
  EXPECT_EQ(postgresql.TableTypesArrayLiteral({}),
            R"({"r", "v", "m", "t", "f", "p"})");
  EXPECT_EQ(redshift.TableTypeNames(),
            (std::vector<std::string>{"table", "view"}));
  EXPECT_EQ(redshift.TableTypesArrayLiteral({}), R"({"r", "v"})");
  EXPECT_EQ(redshift.TableTypesArrayLiteral({"view", "foreign_table"}),
            R"({"v"})");
}

TEST(MetadataQuerySetTest, CapabilitiesGuardOptionalQueries) {
  MetadataQuerySet postgresql(
      adbc::driver::pgwire::BackendProfile::PostgreSQL());
  MetadataQuerySet redshift(adbc::driver::pgwire::BackendProfile::Redshift());

  EXPECT_TRUE(postgresql.LoadsConstraints());
  EXPECT_FALSE(redshift.LoadsConstraints());
  EXPECT_NE(postgresql.Constraints(false).find("pg_catalog.pg_constraint"),
            std::string::npos);
  EXPECT_NE(redshift.TableSchema().find("pg_catalog.pg_attribute"),
            std::string::npos);
}

}  // namespace
}  // namespace adbcpq
