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

#include <string>
#include <vector>

#include "redshift_super_parameter.h"

namespace adbcpq {
namespace {
using Plan = RedshiftSuperParameterPlan;

TEST(RedshiftSuperParameterTest, OnlyActualParameterTokensChange) {
  const std::string sql =
      R"(SELECT $1, foo$1, "$1", '$1', E'\'$1', $$ $1 $$, $tag$ $1 $tag$, )"
      R"(U, B'$1', X'$1', $10, $1::SUPER -- $1)"
      "\n/* $1 /* nested $1 */ $1 */;";
  Plan plan;
  std::string error;
  ASSERT_TRUE(Plan::Parse(sql, 10, true, &plan, &error)) << error;
  ASSERT_EQ(plan.references().size(), 3);
  EXPECT_EQ(plan.references()[0].index, 0);
  EXPECT_EQ(plan.references()[1].index, 9);
  EXPECT_EQ(plan.references()[2].index, 0);
  std::vector<bool> selected(10, false);
  selected[0] = true;
  Plan::Generated generated;
  ASSERT_TRUE(plan.Convert(selected, "verified_namespace", &generated, &error));
  ASSERT_EQ(generated.replacements.size(), 2);
  EXPECT_NE(generated.sql.find("foo$1"), std::string::npos);
  EXPECT_NE(generated.sql.find("$10"), std::string::npos);
  EXPECT_NE(generated.sql.find(R"(("verified_namespace"."json_parse"($1))::SUPER)"),
            std::string::npos);
  // Reassemble from unchanged gaps and original tokens: no opaque token changed.
  std::string restored;
  size_t cursor = 0;
  for (const auto& replacement : generated.replacements) {
    restored.append(generated.sql, cursor, replacement.generated_begin - cursor);
    restored.append(sql, replacement.source_begin,
                    replacement.source_end - replacement.source_begin);
    cursor = replacement.generated_end;
  }
  restored.append(generated.sql, cursor, std::string::npos);
  EXPECT_EQ(restored, sql);
  EXPECT_EQ(plan.original(), sql);
}

TEST(RedshiftSuperParameterTest, CtesDmlNavigationRepeatedIndicesAndGaps) {
  for (const std::string sql : {
           "SELECT $3.n, $3[0], ($3)::SUPER, $1 FROM t",
           "WITH q AS (SELECT $3 AS n) SELECT n FROM q WHERE n = $1",
           "INSERT INTO t(v) VALUES ($3),($1)",
           "UPDATE t SET v = CASE WHEN id = 1 THEN $3 ELSE $1 END",
           "DELETE FROM t WHERE v = $3 OR v = $1",
           "MERGE INTO t USING s ON t.id = s.id WHEN MATCHED THEN UPDATE SET v = $3 "
           "WHEN NOT MATCHED THEN INSERT VALUES ($1)",
           "VALUES ($3),($1)",
       }) {
    Plan plan;
    std::string error;
    ASSERT_TRUE(Plan::Parse(sql, 3, true, &plan, &error)) << sql << ": " << error;
    ASSERT_GE(plan.references().size(), 2);
    Plan::Generated generated;
    ASSERT_TRUE(plan.Convert({true, false, true}, "verified", &generated, &error));
    EXPECT_EQ(generated.replacements.size(), plan.references().size());
  }
}

TEST(RedshiftSuperParameterTest, ContextControlsOrdinaryButNotEscapeStrings) {
  Plan plan;
  std::string error;
  // With backslash escapes, the first quote is escaped and $1 remains inert.
  const std::string backslash = R"(SELECT '\' $1', $2)";
  ASSERT_TRUE(Plan::Parse(backslash, 2, false, &plan, &error)) << error;
  ASSERT_EQ(plan.references().size(), 1);
  EXPECT_EQ(plan.references()[0].index, 1);
  EXPECT_FALSE(Plan::Parse(backslash, 2, true, &plan, &error));
  for (bool standard : {true, false}) {
    ASSERT_TRUE(Plan::Parse(R"(SELECT E'\' $1', $2)", 2, standard, &plan, &error));
    ASSERT_EQ(plan.references().size(), 1);
    EXPECT_EQ(plan.references()[0].index, 1);
  }
  ASSERT_TRUE(Plan::Parse(R"(SELECT '\', $1)", 1, true, &plan, &error));
  EXPECT_FALSE(Plan::Parse(R"(SELECT '\', $1)", 1, false, &plan, &error));
}

TEST(RedshiftSuperParameterTest, DoubledQuotesCommentsAndDollarTags) {
  for (const std::string token : {
           R"('$1''$2')",
           R"("$1""$2")",
           R"($$'$1";$2$$)",
           R"($foo_1$ $1 $FOO_1$ $2 $foo_1$)",
           R"($é$ $1 $2 $é$)",
           "/* $1 /* $2 */ */",
           "-- $1\r",
           "-- $2\n",
       }) {
    Plan plan;
    std::string error;
    const std::string sql = "SELECT " + token + " $3";
    ASSERT_TRUE(Plan::Parse(sql, 3, true, &plan, &error)) << sql << ": " << error;
    ASSERT_EQ(plan.references().size(), 1) << sql;
    EXPECT_EQ(plan.references()[0].index, 2);
  }
}

TEST(RedshiftSuperParameterTest, RejectsUnqualifiedScopesAndMalformedTokens) {
  for (const std::string sql : {
           "",
           "/* only comment */",
           "SELECT $0",
           "SELECT $4",
           "SELECT $99999999999999",
           "SELECT $1suffix",
           "SELECT $1$tag$",
           "SELECT $",
           "SELECT $tag$never closed",
           "SELECT 'never closed",
           "SELECT \"never closed",
           "SELECT /* never closed",
           "SELECT 1; SELECT $1",
           "SELECT 1;;",
           ";SELECT $1",
           "CALL f($1)",
           "PREPARE s AS SELECT $1",
           "EXECUTE s($1)",
           "DO $$SELECT $1$$",
           "CREATE FUNCTION f() AS $$SELECT $1$$",
           "COPY t FROM $1",
           "SHOW $1",
           R"(SELECT U&"$1")",
           R"(SELECT U&'$1')",
           R"(SELECT \ $1)",
       }) {
    Plan plan;
    std::string error;
    EXPECT_FALSE(Plan::Parse(sql, 3, true, &plan, &error)) << sql;
    EXPECT_FALSE(error.empty()) << sql;
  }
  Plan plan;
  std::string error;
  ASSERT_TRUE(Plan::Parse("SELECT $1; /* terminal */ -- end", 1, true, &plan, &error));
}

TEST(RedshiftSuperParameterTest, ContinuedEscapeStringsRejectBeforeConversion) {
  for (bool standard : {true, false}) {
    for (const std::string sql : {
             "SELECT E'a'\n'\\' $1 -- '\n, $2",
             "SELECT E'a' /* newline\n comment */ '\\' $1 -- '\n, $2",
             "SELECT 'a'\n'$1', $2",
             "SELECT E'a' E'$1', $2",
         }) {
      Plan plan;
      std::string error;
      EXPECT_FALSE(Plan::Parse(sql, 2, standard, &plan, &error));
      EXPECT_EQ(error, "Adjacent string segments are not qualified");
    }
  }
}
TEST(RedshiftSuperParameterTest, UtilityWordsAsColumnLabelsHaveNoParameterScope) {
  for (const std::string word : {"prepare", "call", "execute", "copy"}) {
    Plan plan;
    std::string error;
    ASSERT_TRUE(Plan::Parse("SELECT $1 AS " + word, 1, true, &plan, &error)) << error;
    ASSERT_EQ(plan.references().size(), 1);
  }
}

TEST(RedshiftSuperParameterTest, InvalidUtf8AndNulRejectBeforePlanning) {
  for (const std::string bad :
       {std::string("SELECT $1\0", 10), std::string("SELECT '\xc0\x80'"),
        std::string("SELECT '\xed\xa0\x80'"), std::string("SELECT '\xf4\x90\x80\x80'"),
        std::string("SELECT '\xe2\x82'")}) {
    Plan plan;
    std::string error;
    EXPECT_FALSE(Plan::Parse(bad, 1, true, &plan, &error));
    EXPECT_EQ(error, "SQL must be non-NUL valid UTF8");
  }
  Plan plan;
  std::string error;
  ASSERT_TRUE(Plan::Parse("SELECT é$1, \"🙂$1\", $1", 1, true, &plan, &error));
  ASSERT_EQ(plan.references().size(), 1);
}

TEST(RedshiftSuperParameterTest, FailurePreservesExistingPlanAndGeneratedOutput) {
  Plan plan;
  std::string error;
  ASSERT_TRUE(Plan::Parse("SELECT $1", 1, true, &plan, &error));
  EXPECT_FALSE(Plan::Parse("CALL f($1)", 1, true, &plan, &error));
  EXPECT_EQ(plan.original(), "SELECT $1");
  Plan::Generated generated;
  ASSERT_TRUE(plan.Convert({true}, "verified", &generated, &error));
  const auto sql = generated.sql;
  EXPECT_FALSE(plan.Convert({true, false}, "verified", &generated, &error));
  EXPECT_EQ(generated.sql, sql);
  for (const std::string name : {"", "user.name", "x\";SELECT 1", "1invalid"}) {
    EXPECT_FALSE(plan.Convert({true}, name, &generated, &error));
    EXPECT_EQ(generated.sql, sql);
  }
}

TEST(RedshiftSuperParameterTest, UnicodeStatementPositionMapAndInsertedSyntax) {
  const std::string sql = "SELECT 'é🙂', $1, '終', $2;";
  Plan plan;
  std::string error;
  ASSERT_TRUE(Plan::Parse(sql, 2, true, &plan, &error));
  Plan::Generated generated;
  ASSERT_TRUE(plan.Convert({true, true}, "verified", &generated, &error));
  auto character = [](const std::string& text, size_t end) {
    size_t count = 1;
    for (size_t i = 0; i < end; ++i)
      if ((static_cast<unsigned char>(text[i]) & 0xc0) != 0x80) ++count;
    return count;
  };
  size_t mapped = 0;
  EXPECT_FALSE(generated.OriginalPosition(sql, 0, &mapped));
  for (const auto& replacement : generated.replacements) {
    for (size_t byte = replacement.generated_begin; byte < replacement.generated_end;
         ++byte) {
      ASSERT_TRUE(
          generated.OriginalPosition(sql, character(generated.sql, byte), &mapped));
      EXPECT_EQ(mapped, character(sql, replacement.source_begin));
    }
  }
  ASSERT_TRUE(generated.OriginalPosition(
      sql, character(generated.sql, generated.sql.size()), &mapped));
  EXPECT_EQ(mapped, character(sql, sql.size()));
  ASSERT_TRUE(generated.OriginalPosition(
      sql, character(generated.sql, generated.sql.find("'終'")), &mapped));
  EXPECT_EQ(mapped, character(sql, sql.find("'終'")));
  EXPECT_FALSE(generated.OriginalPosition(
      sql, character(generated.sql, generated.sql.size()) + 1, &mapped));
}

TEST(RedshiftSuperParameterTest, OpaquePayloadPropertyAcrossReferenceIndices) {
  // Finite cross-product: hidden placeholders must stay hidden for every admitted
  // quoting/comment form, regardless of selected parameter position or repeats.
  for (int index = 1; index <= 32; ++index) {
    const auto ref = "$" + std::to_string(index);
    const std::vector<std::string> hidden = {
        "'" + ref + "'",           "\"" + ref + "\"",
        "$body$" + ref + "$body$", "/* outer /* " + ref + " */ */",
        "-- " + ref + "\n",        "name" + ref};
    for (const auto& token : hidden) {
      const std::string sql = "SELECT " + token + ", " + ref + ", " + ref;
      Plan plan;
      std::string error;
      ASSERT_TRUE(Plan::Parse(sql, 32, true, &plan, &error)) << sql;
      ASSERT_EQ(plan.references().size(), 2) << sql;
      for (const auto& reference : plan.references())
        EXPECT_EQ(reference.index, static_cast<size_t>(index - 1));
      std::vector<bool> selected(32, false);
      selected[index - 1] = true;
      Plan::Generated generated;
      ASSERT_TRUE(plan.Convert(selected, "verified", &generated, &error));
      EXPECT_EQ(generated.sql.substr(0, 7 + token.size()), "SELECT " + token);
      EXPECT_EQ(generated.replacements.size(), 2);
    }
  }
}

TEST(RedshiftSuperParameterTest, DualModeRequiresEveryReferenceSpanToAgree) {
  const std::string sql = "SELECT '\\' || $1 -- ' || $1 -- '\n, $2";
  Plan standard, escaped, stable;
  std::string error;
  ASSERT_TRUE(Plan::Parse(sql, 2, true, &standard, &error)) << error;
  ASSERT_TRUE(Plan::Parse(sql, 2, false, &escaped, &error)) << error;
  ASSERT_EQ(standard.references().size(), 2U);
  ASSERT_EQ(escaped.references().size(), 2U);
  EXPECT_EQ(standard.references()[0].index, escaped.references()[0].index);
  EXPECT_NE(standard.references()[0].begin, escaped.references()[0].begin);
  EXPECT_FALSE(Plan::ParseUnambiguous(sql, 2, &stable, &error));

  // Only $2 is SUPER: rewriting selected references alone would look identical.
  Plan::Generated left, right;
  ASSERT_TRUE(standard.Convert({false, true}, "verified", &left, &error));
  ASSERT_TRUE(escaped.Convert({false, true}, "verified", &right, &error));
  EXPECT_EQ(left.sql, right.sql);
  EXPECT_FALSE(Plan::ParseUnambiguous(sql, 2, &stable, &error));
}

TEST(RedshiftSuperParameterTest, DualModeAcceptsEqualBoundariesNotEqualStringValues) {
  for (const std::string sql : {
           R"(SELECT 'a\b', $1, E'\'$2', $2)",
           "SELECT '$1', /* $2 */ $1, $2",
           R"(SELECT $$ \ $1 $$, $2)",
           R"(SELECT foo$1, "$2", $1, $2)",
       }) {
    Plan stable, standard, escaped;
    std::string error;
    ASSERT_TRUE(Plan::ParseUnambiguous(sql, 2, &stable, &error)) << sql << ": " << error;
    ASSERT_TRUE(Plan::Parse(sql, 2, true, &standard, &error));
    ASSERT_TRUE(Plan::Parse(sql, 2, false, &escaped, &error));
    Plan::Generated generated, left, right;
    ASSERT_TRUE(stable.Convert({true, true}, "verified", &generated, &error));
    ASSERT_TRUE(standard.Convert({true, true}, "verified", &left, &error));
    ASSERT_TRUE(escaped.Convert({true, true}, "verified", &right, &error));
    EXPECT_EQ(generated.sql, left.sql);
    EXPECT_EQ(generated.sql, right.sql);
    ASSERT_EQ(left.replacements.size(), right.replacements.size());
    for (size_t i = 0; i < left.replacements.size(); ++i) {
      EXPECT_EQ(left.replacements[i].source_begin, right.replacements[i].source_begin);
      EXPECT_EQ(left.replacements[i].source_end, right.replacements[i].source_end);
      EXPECT_EQ(left.replacements[i].generated_begin,
                right.replacements[i].generated_begin);
      EXPECT_EQ(left.replacements[i].generated_end, right.replacements[i].generated_end);
    }
  }
  Plan plan;
  std::string error;
  EXPECT_FALSE(
      Plan::ParseUnambiguous("SELECT E'a'\n'\\' $1 -- '\n, $2", 2, &plan, &error));
  EXPECT_FALSE(Plan::ParseUnambiguous(R"(SELECT '\', $1)", 1, &plan, &error));
}

}  // namespace
}  // namespace adbcpq
