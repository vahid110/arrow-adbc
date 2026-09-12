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

#include <arrow-adbc/adbc.h>
#include <gtest/gtest.h>

extern "C" AdbcStatusCode AdbcDriverPostgresqlInit(int version, void* raw_driver,
                                                   struct AdbcError* error);
extern "C" AdbcStatusCode AdbcDriverRedshiftInit(int version, void* raw_driver,
                                                 struct AdbcError* error);

namespace adbc::driver::pgwire {
namespace {

TEST(BackendProfileTest, PostgreSQLCapabilitiesPreserveCurrentFastPaths) {
  constexpr auto profile = BackendProfile::PostgreSQL();
  EXPECT_EQ(profile.kind, BackendKind::kPostgreSQL);
  EXPECT_EQ(profile.name, "PostgreSQL");
  EXPECT_EQ(profile.driver_name, "ADBC PostgreSQL Driver");
  EXPECT_EQ(profile.version_prefix, "PostgreSQL");
  EXPECT_EQ(profile.vendor_version_source, VendorVersionSource::kServerParameter);
  EXPECT_TRUE(profile.capabilities.binary_parameters);
  EXPECT_TRUE(profile.capabilities.binary_query_copy);
  EXPECT_TRUE(profile.capabilities.binary_ingest_copy);
  EXPECT_TRUE(profile.capabilities.type_catalog_has_typarray);
  EXPECT_TRUE(profile.capabilities.metadata_constraints);
  EXPECT_TRUE(profile.capabilities.metadata_statistics);
  EXPECT_EQ(profile.transactions.isolation_levels,
            IsolationLevelPolicy::kSessionConfigurable);
  EXPECT_TRUE(profile.transactions.transactional_ddl);
}

TEST(BackendProfileTest, RedshiftStartsFromVerifiedConservativeCapabilities) {
  constexpr auto profile = BackendProfile::Redshift();
  EXPECT_EQ(profile.kind, BackendKind::kRedshift);
  EXPECT_EQ(profile.name, "Redshift");
  EXPECT_EQ(profile.driver_name, "ADBC Redshift Driver");
  EXPECT_EQ(profile.version_prefix, "Redshift");
  EXPECT_EQ(profile.vendor_version_source, VendorVersionSource::kParsedVersionString);
  EXPECT_TRUE(profile.capabilities.prepared_statements);
  EXPECT_TRUE(profile.capabilities.binary_parameters);
  EXPECT_FALSE(profile.capabilities.binary_query_copy);
  EXPECT_FALSE(profile.capabilities.binary_ingest_copy);
  EXPECT_FALSE(profile.capabilities.type_catalog_has_typarray);
  EXPECT_FALSE(profile.capabilities.metadata_constraints);
  EXPECT_FALSE(profile.capabilities.metadata_statistics);
  EXPECT_EQ(profile.transactions.isolation_levels,
            IsolationLevelPolicy::kDatabaseConfigured);
  EXPECT_FALSE(profile.transactions.transactional_ddl);
}

TEST(BackendProfileTest, TableTypesBelongToBackendSemantics) {
  constexpr auto postgres = BackendProfile::PostgreSQL();
  constexpr auto redshift = BackendProfile::Redshift();

  ASSERT_NE(postgres.FindTableType("partitioned_table"), nullptr);
  EXPECT_EQ(postgres.FindTableType("partitioned_table")->relkind, "p");
  EXPECT_EQ(redshift.table_type_count, 2U);
  ASSERT_NE(redshift.FindTableType("table"), nullptr);
  EXPECT_EQ(redshift.FindTableType("table")->relkind, "r");
  ASSERT_NE(redshift.FindTableType("view"), nullptr);
  EXPECT_EQ(redshift.FindTableType("view")->relkind, "v");
  EXPECT_EQ(redshift.FindTableType("partitioned_table"), nullptr);
}

TEST(BackendProfileTest, TypeReceiveAliasesBelongToBackendSemantics) {
  constexpr auto postgres = BackendProfile::PostgreSQL();
  constexpr auto redshift = BackendProfile::Redshift();

  EXPECT_EQ(postgres.CanonicalTypeReceive("varbyte_recv"), "varbyte_recv");
  EXPECT_EQ(redshift.CanonicalTypeReceive("varbyte_recv"), "bytearecv");
  EXPECT_EQ(redshift.CanonicalTypeReceive("int4recv"), "int4recv");
}

TEST(BackendProfileTest, QueryResultModeRequiresCapabilityOptionAndOutput) {
  constexpr auto postgres = BackendProfile::PostgreSQL();
  constexpr auto redshift = BackendProfile::Redshift();

  EXPECT_EQ(SelectQueryResultMode(postgres, true, true), QueryResultMode::kBinaryCopy);
  EXPECT_EQ(SelectQueryResultMode(postgres, false, true), QueryResultMode::kText);
  EXPECT_EQ(SelectQueryResultMode(postgres, true, false), QueryResultMode::kText);
  EXPECT_EQ(SelectQueryResultMode(redshift, true, true), QueryResultMode::kText);
}

TEST(BackendProfileTest, BulkIngestModeIsCapabilityDriven) {
  EXPECT_EQ(SelectBulkIngestMode(BackendProfile::PostgreSQL()),
            BulkIngestMode::kBinaryCopy);
  EXPECT_EQ(SelectBulkIngestMode(BackendProfile::Redshift()),
            BulkIngestMode::kParameterizedInsert);
}

TEST(BackendProfileTest, DetectsRedshiftFromServerVersion) {
  const auto redshift =
      DetectBackendProfile("PostgreSQL 8.0.2 on x86_64-pc-linux-gnu, Redshift 1.0.12345");
  EXPECT_EQ(redshift.kind, BackendKind::kRedshift);
  EXPECT_EQ(redshift.name, "Redshift");
}

TEST(BackendProfileTest, DefaultsUnknownPgWireServersToPostgreSQLCompatibility) {
  const auto postgres = DetectBackendProfile("PostgreSQL 18.1");
  const auto unknown = DetectBackendProfile("compatible pgwire server");
  EXPECT_EQ(postgres.kind, BackendKind::kPostgreSQL);
  EXPECT_EQ(unknown.kind, BackendKind::kPostgreSQL);
}

TEST(DriverConstructionTest, NamedDriversComposeDistinctDatabaseFactories) {
  struct AdbcDriver postgresql = {};
  struct AdbcDriver redshift = {};
  struct AdbcError error = {};

  ASSERT_EQ(AdbcDriverPostgresqlInit(ADBC_VERSION_1_1_0, &postgresql, &error),
            ADBC_STATUS_OK);
  ASSERT_EQ(AdbcDriverRedshiftInit(ADBC_VERSION_1_1_0, &redshift, &error),
            ADBC_STATUS_OK);
  ASSERT_NE(postgresql.DatabaseNew, nullptr);
  ASSERT_NE(redshift.DatabaseNew, nullptr);
  EXPECT_NE(postgresql.DatabaseNew, redshift.DatabaseNew);
}

}  // namespace
}  // namespace adbc::driver::pgwire
