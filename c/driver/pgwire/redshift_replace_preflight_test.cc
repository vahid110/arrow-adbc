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

#include "postgresql/redshift_replace_preflight.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace adbc::driver::pgwire {
namespace {

void ExpectRejected(RedshiftReplacePreflightResult result) {
  EXPECT_EQ(result.status.ToAdbc(nullptr), ADBC_STATUS_INVALID_ARGUMENT);
  EXPECT_FALSE(result.needs_case_setting);
}

TEST(RedshiftReplacePreflightTest, AllowsNamesAtAsciiByteLimit) {
  const std::string limit(127, 'a');
  auto result = CheckRedshiftReplaceIdentifiers(limit, limit, {limit});
  EXPECT_TRUE(result.status.ok());
  EXPECT_FALSE(result.needs_case_setting);
}

TEST(RedshiftReplacePreflightTest, RejectsOverlongTableSchemaAndColumnNames) {
  const std::string overlong(128, 'a');
  ExpectRejected(CheckRedshiftReplaceIdentifiers(overlong, "public", {"id"}));
  ExpectRejected(CheckRedshiftReplaceIdentifiers("table", overlong, {"id"}));
  ExpectRejected(CheckRedshiftReplaceIdentifiers("table", "public", {overlong}));
}

TEST(RedshiftReplacePreflightTest, MeasuresUtf8BytesNotCharacters) {
  std::string limit(125, 'a');
  limit += "\xc3\xa9";
  ASSERT_EQ(limit.size(), 127U);
  auto result = CheckRedshiftReplaceIdentifiers(limit, limit, {limit});
  EXPECT_TRUE(result.status.ok());
  EXPECT_FALSE(result.needs_case_setting);

  std::string overlong;
  for (int i = 0; i < 64; ++i) overlong += "\xc3\xa9";
  ASSERT_EQ(overlong.size(), 128U);
  ExpectRejected(CheckRedshiftReplaceIdentifiers(overlong, "public", {"id"}));
  ExpectRejected(CheckRedshiftReplaceIdentifiers("table", overlong, {"id"}));
  ExpectRejected(CheckRedshiftReplaceIdentifiers("table", "public", {overlong}));
}

TEST(RedshiftReplacePreflightTest, RejectsSharedTruncationPrefixesWithoutClipping) {
  const std::string prefix(127, 'a');
  const std::string first = prefix + "x";
  const std::string second = prefix + "y";
  ExpectRejected(CheckRedshiftReplaceIdentifiers("table", "public", {first, second}));
}

TEST(RedshiftReplacePreflightTest, ChecksByteLimitsBeforeCaseCollisions) {
  const std::string overlong(128, 'a');
  ExpectRejected(
      CheckRedshiftReplaceIdentifiers("table", "public", {"id", "ID", overlong}));
}

TEST(RedshiftReplacePreflightTest, RejectsEmptyAndExactDuplicateNames) {
  ExpectRejected(CheckRedshiftReplaceIdentifiers("", "public", {"id"}));
  ExpectRejected(CheckRedshiftReplaceIdentifiers("table", "public", {}));
  ExpectRejected(CheckRedshiftReplaceIdentifiers("table", "public", {""}));
  ExpectRejected(CheckRedshiftReplaceIdentifiers("table", "public", {"id", "id"}));
  ExpectRejected(CheckRedshiftReplaceIdentifiers("table", "public", {"id", "ID", "id"}));
}

TEST(RedshiftReplacePreflightTest, AllowsOmittedSchema) {
  auto result = CheckRedshiftReplaceIdentifiers("table", "", {"id"});
  EXPECT_TRUE(result.status.ok());
  EXPECT_FALSE(result.needs_case_setting);
}

TEST(RedshiftReplacePreflightTest, RequestsSettingOnlyForAsciiFoldedCollisions) {
  for (const std::vector<std::string_view>& columns :
       {std::vector<std::string_view>{"id", "ID"},
        std::vector<std::string_view>{"Id", "iD"},
        std::vector<std::string_view>{"This \"IS IT\"", "this \"is it\""}}) {
    auto result = CheckRedshiftReplaceIdentifiers("table", "public", columns);
    EXPECT_TRUE(result.status.ok());
    EXPECT_TRUE(result.needs_case_setting);
  }
  auto distinct = CheckRedshiftReplaceIdentifiers("table", "public", {"Id", "other"});
  EXPECT_TRUE(distinct.status.ok());
  EXPECT_FALSE(distinct.needs_case_setting);
}

TEST(RedshiftReplacePreflightTest, AllowsQuotesPunctuationAndUtf8Names) {
  auto result = CheckRedshiftReplaceIdentifiers(
      "table\"; punctuation", "schema.name",
      {"quoted\"name", "with space", "city/id", "%", "\xc3\xa9"});
  EXPECT_TRUE(result.status.ok());
  EXPECT_FALSE(result.needs_case_setting);
}

TEST(RedshiftReplacePreflightTest, MeasuresLogicalBytesBeforeQuoteEscaping) {
  // The SQL quoting path doubles each quote, but the logical name is 127 bytes.
  const std::string limit(127, '"');
  auto result = CheckRedshiftReplaceIdentifiers(limit, limit, {limit});
  EXPECT_TRUE(result.status.ok());
  EXPECT_FALSE(result.needs_case_setting);
}

TEST(RedshiftReplacePreflightTest, DoesNotFoldNonAsciiCase) {
  auto result =
      CheckRedshiftReplaceIdentifiers("table", "public", {"\xc3\x84", "\xc3\xa4"});
  EXPECT_TRUE(result.status.ok());
  EXPECT_FALSE(result.needs_case_setting);

  auto ascii_collision =
      CheckRedshiftReplaceIdentifiers("table", "public", {"\xc3\x84ID", "\xc3\x84id"});
  EXPECT_TRUE(ascii_collision.status.ok());
  EXPECT_TRUE(ascii_collision.needs_case_setting);
}

TEST(RedshiftReplacePreflightTest, AllowsRecognizedCasePreservingSettings) {
  for (std::string_view setting : {"on", "true"}) {
    EXPECT_TRUE(ValidateRedshiftReplaceCaseSetting(setting).ok());
  }
}

TEST(RedshiftReplacePreflightTest, RejectsRecognizedCaseFoldingSettings) {
  for (std::string_view setting : {"off", "false"}) {
    EXPECT_EQ(ValidateRedshiftReplaceCaseSetting(setting).ToAdbc(nullptr),
              ADBC_STATUS_INVALID_ARGUMENT);
  }
}

TEST(RedshiftReplacePreflightTest, FailsClosedOnUnknownCaseSettings) {
  for (std::string_view setting : {"", "unknown", "ON", "TRUE", " off", "1", "0"}) {
    EXPECT_EQ(ValidateRedshiftReplaceCaseSetting(setting).ToAdbc(nullptr),
              ADBC_STATUS_INVALID_DATA);
  }
}

struct FakeSettingReader {
  explicit FakeSettingReader(std::string setting, AdbcStatusCode code = ADBC_STATUS_OK)
      : value(std::move(setting)), fail_code(code) {}

  std::string value;
  AdbcStatusCode fail_code;
  int calls = 0;
  std::string last_query;

  RedshiftSettingReader Reader() {
    return [this](std::string_view query, std::string* out) -> Status {
      ++calls;
      last_query = std::string(query);
      if (fail_code != ADBC_STATUS_OK) {
        return Status(fail_code, "injected setting read failure");
      }
      *out = value;
      return Status::Ok();
    };
  }
};

TEST(RedshiftReplacePreflightTest, SkipsSettingReadWithoutCaseCollision) {
  FakeSettingReader fake{"off"};
  EXPECT_TRUE(
      PreflightRedshiftReplace("table", "public", {"id", "other"}, fake.Reader()).ok());
  EXPECT_EQ(fake.calls, 0);
}

TEST(RedshiftReplacePreflightTest, RejectsInvalidIdentifiersBeforeReadingSetting) {
  FakeSettingReader fake{"on"};
  const std::string overlong(128, 'a');
  EXPECT_EQ(PreflightRedshiftReplace(overlong, "public", {"id", "ID"}, fake.Reader())
                .ToAdbc(nullptr),
            ADBC_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(PreflightRedshiftReplace("table", "public", {"id", "id"}, fake.Reader())
                .ToAdbc(nullptr),
            ADBC_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(fake.calls, 0);
}

TEST(RedshiftReplacePreflightTest, ReadsCurrentSettingForCaseCollision) {
  for (std::string_view setting : {"on", "true"}) {
    FakeSettingReader fake{std::string(setting)};
    EXPECT_TRUE(
        PreflightRedshiftReplace("table", "public", {"id", "ID"}, fake.Reader()).ok());
    EXPECT_EQ(fake.calls, 1);
    EXPECT_EQ(fake.last_query, kRedshiftCaseSettingQuery);
  }
  for (std::string_view setting : {"off", "false"}) {
    FakeSettingReader fake{std::string(setting)};
    EXPECT_EQ(PreflightRedshiftReplace("table", "public", {"id", "ID"}, fake.Reader())
                  .ToAdbc(nullptr),
              ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(fake.calls, 1);
  }
  FakeSettingReader unknown{"maybe"};
  EXPECT_EQ(PreflightRedshiftReplace("table", "public", {"id", "ID"}, unknown.Reader())
                .ToAdbc(nullptr),
            ADBC_STATUS_INVALID_DATA);
}

TEST(RedshiftReplacePreflightTest, PropagatesSettingReadFailure) {
  FakeSettingReader fake{"", ADBC_STATUS_IO};
  EXPECT_EQ(PreflightRedshiftReplace("table", "public", {"id", "ID"}, fake.Reader())
                .ToAdbc(nullptr),
            ADBC_STATUS_IO);
  EXPECT_EQ(fake.calls, 1);
}

}  // namespace
TEST(RedshiftReplacePreflightTest, LookupNamesUseFreshCaseSettingWithoutUnicodeFolding) {
  for (const std::string setting : {"on", "true", "off", "false"}) {
    int calls = 0;
    std::string table = "TÄble", schema = "MiXeD";
    auto read = [&](std::string_view query, std::string* value) {
      EXPECT_EQ(query, kRedshiftCaseSettingQuery);
      ++calls;
      *value = setting;
      return Status::Ok();
    };
    ASSERT_TRUE(NormalizeRedshiftLookupIdentifiers(&table, &schema, read).ok());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(table, setting == "off" || setting == "false" ? "tÄble" : "TÄble");
    EXPECT_EQ(schema, setting == "off" || setting == "false" ? "mixed" : "MiXeD");
  }
  std::string table = "lower_Ä", schema = "schema";
  auto unused = [](std::string_view, std::string*) {
    ADD_FAILURE() << "Lowercase ASCII names must not query the case setting";
    return Status::Ok();
  };
  ASSERT_TRUE(NormalizeRedshiftLookupIdentifiers(&table, &schema, unused).ok());
  EXPECT_EQ(table, "lower_Ä");
}
TEST(RedshiftReplacePreflightTest, LookupUnknownOrFailedCaseSettingPreservesNames) {
  for (bool fail : {false, true}) {
    std::string table = "Table", schema = "Schema";
    auto read = [&](std::string_view, std::string* value) {
      *value = "unknown";
      return fail ? Status::InvalidState("read failed") : Status::Ok();
    };
    EXPECT_FALSE(NormalizeRedshiftLookupIdentifiers(&table, &schema, read).ok());
    EXPECT_EQ(table, "Table");
    EXPECT_EQ(schema, "Schema");
  }
}

}  // namespace adbc::driver::pgwire
