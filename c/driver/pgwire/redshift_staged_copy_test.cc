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

#include "postgresql/redshift_staged_copy.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adbc::driver::pgwire {
namespace {

constexpr std::string_view kDataUrl = "s3://pgwire-ci/staging/run-01/data.csv";
constexpr std::string_view kManifestUrl = "s3://pgwire-ci/staging/run-01/load.manifest";
constexpr std::string_view kRoleArn =
    "arn:aws:iam::149112076833:role/adbc-pgwire-ci-copy";

std::optional<RedshiftStagedCopyPlan> PreparePlan(std::string_view schema,
                                                  std::string_view table,
                                                  std::string_view data_s3_url,
                                                  std::string_view manifest_s3_url,
                                                  std::string_view iam_role_arn) {
  return PrepareRedshiftStagedCopy(schema, table, {"id"}, data_s3_url, manifest_s3_url,
                                   iam_role_arn);
}

TEST(RedshiftStagedCopyTest, GeneratesSingleMandatoryObjectAndManifestCopy) {
  auto plan = PreparePlan("public", "my_table", kDataUrl, kManifestUrl, kRoleArn);
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->manifest_json,
            "{\"entries\":[{\"url\":\"s3://pgwire-ci/staging/run-01/data.csv\","
            "\"mandatory\":true}]}");
  EXPECT_EQ(plan->copy_sql,
            "COPY \"public\".\"my_table\" (\"id\") FROM "
            "'s3://pgwire-ci/staging/run-01/load.manifest' IAM_ROLE "
            "'arn:aws:iam::149112076833:role/adbc-pgwire-ci-copy' MANIFEST CSV");
}

TEST(RedshiftStagedCopyTest, QuotesIdentifiersWithoutAcceptingSqlFragments) {
  auto plan =
      PreparePlan("a.b", "x\"; DROP TABLE victim;--", kDataUrl, kManifestUrl, kRoleArn);
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->copy_sql,
            "COPY \"a.b\".\"x\"\"; DROP TABLE victim;--\" (\"id\") FROM "
            "'s3://pgwire-ci/staging/run-01/load.manifest' IAM_ROLE "
            "'arn:aws:iam::149112076833:role/adbc-pgwire-ci-copy' MANIFEST CSV");
}

TEST(RedshiftStagedCopyTest, RejectsInvalidIdentifiers) {
  const std::string with_nul("a\0b", 3);
  for (std::string_view bad :
       {std::string_view(), std::string_view(with_nul), std::string_view("tab\nle"),
        std::string_view("tab\\le"), std::string_view("\xc3\xa9")}) {
    EXPECT_FALSE(PreparePlan("public", bad, kDataUrl, kManifestUrl, kRoleArn));
    EXPECT_FALSE(PreparePlan(bad, "table", kDataUrl, kManifestUrl, kRoleArn));
  }
  EXPECT_FALSE(
      PreparePlan("public", std::string(128, 'a'), kDataUrl, kManifestUrl, kRoleArn));
}

TEST(RedshiftStagedCopyTest, RejectsNonExactOrUnsafeS3Objects) {
  for (std::string_view bad :
       {std::string_view("s3://pgwire-ci"), std::string_view("s3://pgwire-ci/"),
        std::string_view("s3://pgwire-ci/staging/"),
        std::string_view("s3://pgwire-ci/staging//x.csv"),
        std::string_view("s3://pgwire-ci/staging/../x.csv"),
        std::string_view("s3://pgwire-ci/staging/*.csv"),
        std::string_view("s3://pgwire-ci/staging/x.csv?version=1"),
        std::string_view("s3://pgwire-ci/staging/x'bad.csv"),
        std::string_view("s3://PGWIRE-CI/staging/x.csv"),
        std::string_view("https://pgwire-ci/staging/x.csv")}) {
    EXPECT_FALSE(PreparePlan("public", "table", bad, kManifestUrl, kRoleArn)) << bad;
    EXPECT_FALSE(PreparePlan("public", "table", kDataUrl, bad, kRoleArn)) << bad;
  }
  EXPECT_FALSE(
      PreparePlan("public", "table", "s3://ab/staging/data.csv", kManifestUrl, kRoleArn));
  EXPECT_FALSE(PreparePlan("public", "table", kDataUrl, kDataUrl, kRoleArn));
  EXPECT_FALSE(PreparePlan("public", "table", kDataUrl,
                           "s3://another-bucket/staging/load.manifest", kRoleArn));
  EXPECT_FALSE(PreparePlan("public", "table", "s3://pgwire-ci/" + std::string(1025, 'a'),
                           kManifestUrl, kRoleArn));
}

TEST(RedshiftStagedCopyTest, RejectsUnsafeOrNonRoleArns) {
  for (std::string_view bad :
       {std::string_view("arn:aws:iam::149112076833:role/"),
        std::string_view("arn:aws:iam::14911207683:role/copy"),
        std::string_view("arn:aws:iam::149112076833:user/copy"),
        std::string_view("arn:aws:iam::149112076833:role/a//b"),
        std::string_view("arn:aws:iam::149112076833:role/../copy"),
        std::string_view("arn:aws:iam::149112076833:role/copy,other"),
        std::string_view("arn:aws:iam::149112076833:role/copy' SQL"),
        std::string_view("arn:aws:iam::149112076833:role/copy\n")}) {
    EXPECT_FALSE(PreparePlan("public", "table", kDataUrl, kManifestUrl, bad)) << bad;
  }
  EXPECT_FALSE(PreparePlan("public", "table", kDataUrl, kManifestUrl,
                           "arn:aws:iam::149112076833:role/" + std::string(513, 'a')));
}

TEST(RedshiftStagedCopyTest, UsesExactOrderedColumnsForReorderedAppend) {
  auto plan = PrepareRedshiftStagedCopy("public", "my_table", {"label", "id"}, kDataUrl,
                                        kManifestUrl, kRoleArn);
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->copy_sql,
            "COPY \"public\".\"my_table\" (\"label\", \"id\") FROM "
            "'s3://pgwire-ci/staging/run-01/load.manifest' IAM_ROLE "
            "'arn:aws:iam::149112076833:role/adbc-pgwire-ci-copy' MANIFEST CSV");
}

TEST(RedshiftStagedCopyTest, EscapesColumnSqlFragments) {
  auto plan = PrepareRedshiftStagedCopy("public", "my_table",
                                        {"id", "x\"); DROP TABLE victim;--"}, kDataUrl,
                                        kManifestUrl, kRoleArn);
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->copy_sql,
            "COPY \"public\".\"my_table\" (\"id\", "
            "\"x\"\"); DROP TABLE victim;--\") FROM "
            "'s3://pgwire-ci/staging/run-01/load.manifest' IAM_ROLE "
            "'arn:aws:iam::149112076833:role/adbc-pgwire-ci-copy' MANIFEST CSV");
}

TEST(RedshiftStagedCopyTest, RejectsMissingInvalidOrDuplicateColumns) {
  EXPECT_FALSE(PrepareRedshiftStagedCopy("public", "my_table", {}, kDataUrl, kManifestUrl,
                                         kRoleArn));

  const std::string with_nul("a\0b", 3);
  for (std::string_view bad :
       {std::string_view(), std::string_view(with_nul), std::string_view("tab\nle"),
        std::string_view("tab\\le"), std::string_view("\xc3\xa9")}) {
    EXPECT_FALSE(PrepareRedshiftStagedCopy("public", "my_table", {"id", bad}, kDataUrl,
                                           kManifestUrl, kRoleArn));
  }
  EXPECT_FALSE(PrepareRedshiftStagedCopy("public", "my_table",
                                         {"id", std::string(128, 'a')}, kDataUrl,
                                         kManifestUrl, kRoleArn));
  EXPECT_FALSE(PrepareRedshiftStagedCopy("public", "my_table", {"id", "id"}, kDataUrl,
                                         kManifestUrl, kRoleArn));
  EXPECT_FALSE(PrepareRedshiftStagedCopy("public", "my_table", {"Id", "id"}, kDataUrl,
                                         kManifestUrl, kRoleArn));
}

}  // namespace
}  // namespace adbc::driver::pgwire
