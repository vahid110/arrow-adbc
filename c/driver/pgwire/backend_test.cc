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

#include "driver/pgwire/backend.h"

#include <gtest/gtest.h>

namespace adbc::driver::pgwire {
namespace {

TEST(BackendProfileTest, PostgreSQLCapabilitiesPreserveCurrentFastPaths) {
  constexpr auto profile = BackendProfile::PostgreSQL();
  EXPECT_EQ(profile.kind, BackendKind::kPostgreSQL);
  EXPECT_EQ(profile.name, "PostgreSQL");
  EXPECT_TRUE(profile.capabilities.binary_parameters);
  EXPECT_TRUE(profile.capabilities.binary_query_copy);
  EXPECT_TRUE(profile.capabilities.binary_ingest_copy);
  EXPECT_TRUE(profile.capabilities.metadata_constraints);
  EXPECT_TRUE(profile.capabilities.metadata_statistics);
}

TEST(BackendProfileTest, RedshiftStartsFromVerifiedConservativeCapabilities) {
  constexpr auto profile = BackendProfile::Redshift();
  EXPECT_EQ(profile.kind, BackendKind::kRedshift);
  EXPECT_EQ(profile.name, "Redshift");
  EXPECT_TRUE(profile.capabilities.prepared_statements);
  EXPECT_FALSE(profile.capabilities.binary_parameters);
  EXPECT_FALSE(profile.capabilities.binary_query_copy);
  EXPECT_FALSE(profile.capabilities.binary_ingest_copy);
  EXPECT_FALSE(profile.capabilities.metadata_constraints);
  EXPECT_FALSE(profile.capabilities.metadata_statistics);
}

}  // namespace
}  // namespace adbc::driver::pgwire
