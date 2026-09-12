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

#include "type_discovery.h"

#include <string>

#include <gtest/gtest.h>

namespace adbcpq {
namespace {

TEST(TypeDiscoveryTest, CatalogQueryUsesBackendCapabilities) {
  const std::string postgresql =
      BuildTypeCatalogQuery(adbc::driver::pgwire::BackendProfile::PostgreSQL());
  const std::string redshift =
      BuildTypeCatalogQuery(adbc::driver::pgwire::BackendProfile::Redshift());

  EXPECT_NE(postgresql.find(", typarray"), std::string::npos);
  EXPECT_NE(postgresql.find("array_recv"), std::string::npos);
  EXPECT_EQ(redshift.find("typarray"), std::string::npos);
  EXPECT_EQ(redshift.find("array_recv"), std::string::npos);
  EXPECT_NE(redshift.find("pg_catalog.pg_type"), std::string::npos);
}

}  // namespace
}  // namespace adbcpq
