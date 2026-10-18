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

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "metadata.h"
#include "type_modifier_test_fixture.h"

namespace adbcpq {
namespace {
using adbc::driver::pgwire::BackendProfile;

std::optional<std::string> FieldMetadata(const ArrowSchema* schema, const char* key) {
  ArrowStringView value{};
  EXPECT_EQ(ArrowMetadataGetValue(schema->metadata, ArrowCharView(key), &value), 0);
  if (!value.data) return std::nullopt;
  return std::string(value.data, value.size_bytes);
}

void TextCatalog(PqResultHelper* query, int columns,
                 const std::vector<std::vector<std::optional<std::string>>>& rows) {
  PGresult* result = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  ASSERT_NE(result, nullptr);
  query->SetResult(result);
  std::vector<PGresAttDesc> attrs(columns);
  for (auto& attr : attrs) attr.name = const_cast<char*>("fixture");
  ASSERT_EQ(PQsetResultAttrs(result, columns, attrs.data()), 1);
  for (size_t i = 0; i < rows.size(); i++) {
    ASSERT_EQ(rows[i].size(), static_cast<size_t>(columns));
    for (int col = 0; col < columns; col++) {
      const auto& value = rows[i][col];
      ASSERT_EQ(PQsetvalue(result, static_cast<int>(i), col,
                           value ? const_cast<char*>(value->c_str()) : nullptr,
                           value ? static_cast<int>(value->size()) : -1),
                1);
    }
  }
}

TEST(TypeModifierTest, RealDescriptionKeepsSameOidFieldsIndependentAndCacheUnchanged) {
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::ModifierResolver(BackendProfile::Redshift(), &resolver), 0);
  PqResultHelper query(nullptr, "");
  ASSERT_EQ(testing::ModifierDescription(&query, true), 0);
  PostgresType root;
  ASSERT_TRUE(query.ResolveOutputTypes(*resolver, &root).ok());
  EXPECT_EQ(root.child(0).oid(), root.child(1).oid());
  EXPECT_EQ(root.child(0).type_modifier(), 655366);
  EXPECT_EQ(root.child(1).type_modifier(), 2490409);
  auto renamed = root.child(0).WithFieldName("renamed");
  EXPECT_EQ(renamed.field_name(), "renamed");
  EXPECT_EQ(renamed.type_modifier(), 655366);
  EXPECT_EQ(root.child(0).field_name(), "n10");
  PostgresType cached;
  ASSERT_EQ(resolver->Find(testing::kNumericOid, &cached, nullptr), 0);
  EXPECT_FALSE(cached.type_modifier().has_value());
  nanoarrow::UniqueSchema schema;
  ArrowSchemaInit(schema.get());
  ASSERT_EQ(root.SetSchema(schema.get(), "Redshift"), 0);
  EXPECT_STREQ(schema->children[0]->format, "u");  // Numeric codec is unchanged.
  EXPECT_EQ(FieldMetadata(schema->children[0], "ADBC:redshift:sql_type"), "DECIMAL");
  EXPECT_EQ(FieldMetadata(schema->children[0], "ADBC:redshift:precision"), "10");
  EXPECT_EQ(FieldMetadata(schema->children[0], "ADBC:redshift:scale"), "2");
  EXPECT_EQ(FieldMetadata(schema->children[1], "ADBC:redshift:precision"), "38");
  EXPECT_EQ(FieldMetadata(schema->children[1], "ADBC:redshift:scale"), "37");
  EXPECT_EQ(FieldMetadata(schema->children[2], "ADBC:redshift:byte_width"), "20");
  EXPECT_EQ(FieldMetadata(schema->children[3], "ADBC:redshift:byte_width"), "70000");
  EXPECT_EQ(FieldMetadata(schema->children[4], "ADBC:redshift:sql_type"), "CHAR");
  EXPECT_EQ(FieldMetadata(schema->children[5], "ADBC:redshift:interval_family"),
            "intervaly2m");
  EXPECT_EQ(FieldMetadata(schema->children[5], "ADBC:redshift:type_modifier"), "12345");
  EXPECT_FALSE(FieldMetadata(schema->children[5], "ADBC:redshift:scale"));
  EXPECT_FALSE(FieldMetadata(schema->children[6], "ADBC:redshift:byte_width"));
  EXPECT_EQ(FieldMetadata(schema->children[6], "ADBC:redshift:type_modifier"), "99");
  // Nested copies retain field association, not a shared mutable OID modifier.
  PostgresType nested(PostgresTypeId::kRecord);
  nested.AppendChild("nested", root);
  nanoarrow::UniqueSchema nested_schema;
  ArrowSchemaInit(nested_schema.get());
  ASSERT_EQ(nested.SetSchema(nested_schema.get(), "Redshift"), 0);
  EXPECT_EQ(FieldMetadata(nested_schema->children[0]->children[1], "ADBC:redshift:scale"),
            "37");
}

TEST(TypeModifierTest, ReorderedRenamedZeroRowDescriptionUsesDescriptorPosition) {
  PqResultHelper query(nullptr, "");
  PGresult* result = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  ASSERT_NE(result, nullptr);
  query.SetResult(result);
  PGresAttDesc attrs[2] = {};
  attrs[0].name = const_cast<char*>("second_first");
  attrs[1].name = const_cast<char*>("first_second");
  for (auto& attr : attrs) attr.typid = testing::kNumericOid;
  attrs[0].atttypmod =
      2490410;  // Derived DECIMAL(38,38) reference; native boundary unqualified.
  attrs[1].atttypmod = 65540;  // DECIMAL(1,0).
  ASSERT_EQ(PQsetResultAttrs(result, 2, attrs), 1);
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::ModifierResolver(BackendProfile::Redshift(), &resolver), 0);
  PostgresType root;
  ASSERT_TRUE(query.ResolveOutputTypes(*resolver, &root).ok());
  EXPECT_EQ(query.NumRows(), 0);
  nanoarrow::UniqueSchema schema;
  ArrowSchemaInit(schema.get());
  ASSERT_EQ(root.SetSchema(schema.get(), "Redshift"), 0);
  EXPECT_STREQ(schema->children[0]->name, "second_first");
  EXPECT_EQ(FieldMetadata(schema->children[0], "ADBC:redshift:scale"), "38");
  EXPECT_EQ(FieldMetadata(schema->children[1], "ADBC:redshift:precision"), "1");
  EXPECT_EQ(FieldMetadata(schema->children[1], "ADBC:redshift:scale"), "0");
}

TEST(TypeModifierTest, CatalogTableParserPreservesDerivedDescriptorsAndUnknowns) {
  PqResultHelper table(nullptr, "");
  TextCatalog(&table, 3,
              {{"n", "73001", "2490410"},
               {"v", "73002", "70004"},
               {"c", "73003", "4101"},
               {"unknown", "73001", std::nullopt}});
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::ModifierResolver(BackendProfile::Redshift(), &resolver), 0);
  PostgresType root;
  ASSERT_TRUE(table.ResolveTableSchema(*resolver, &root).ok());
  nanoarrow::UniqueSchema schema;
  ArrowSchemaInit(schema.get());
  ASSERT_EQ(root.SetSchema(schema.get(), "Redshift"), 0);
  EXPECT_EQ(FieldMetadata(schema->children[0], "ADBC:redshift:precision"), "38");
  EXPECT_EQ(FieldMetadata(schema->children[0], "ADBC:redshift:scale"), "38");
  EXPECT_EQ(FieldMetadata(schema->children[0], "ADBC:redshift:type_modifier"), "2490410");
  EXPECT_EQ(FieldMetadata(schema->children[1], "ADBC:redshift:byte_width"), "70000");
  EXPECT_EQ(FieldMetadata(schema->children[2], "ADBC:redshift:byte_width"), "4097");
  EXPECT_FALSE(FieldMetadata(schema->children[3], "ADBC:redshift:type_modifier"));
  EXPECT_FALSE(FieldMetadata(schema->children[3], "ADBC:redshift:precision"));
}

TEST(TypeModifierTest, AllSupportedDeclaredNumericContextsReachSchemaMetadata) {
  std::vector<std::vector<std::optional<std::string>>> rows;
  for (int precision = 1; precision <= 38; precision++) {
    for (int scale = 0; scale <= precision && scale <= 37; scale++) {
      rows.push_back({"n", "73001", std::to_string((precision << 16) + scale + 4)});
    }
  }
  PqResultHelper table(nullptr, "");
  TextCatalog(&table, 3, rows);
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::ModifierResolver(BackendProfile::Redshift(), &resolver), 0);
  PostgresType root;
  ASSERT_TRUE(table.ResolveTableSchema(*resolver, &root).ok());
  nanoarrow::UniqueSchema schema;
  ArrowSchemaInit(schema.get());
  ASSERT_EQ(root.SetSchema(schema.get(), "Redshift"), 0);
  int index = 0;
  for (int precision = 1; precision <= 38; precision++) {
    for (int scale = 0; scale <= precision && scale <= 37; scale++, index++) {
      EXPECT_EQ(FieldMetadata(schema->children[index], "ADBC:redshift:precision"),
                std::to_string(precision));
      EXPECT_EQ(FieldMetadata(schema->children[index], "ADBC:redshift:scale"),
                std::to_string(scale));
    }
  }
}

TEST(TypeModifierTest, LiteralTypeSpecificBoundsNeverProduceDefaultsOrWrap) {
  const auto profile = BackendProfile::Redshift();
  struct Case {
    const char* name;
    int32_t raw;
    std::optional<int32_t> precision;
    std::optional<int32_t> scale;
    std::optional<int32_t> width;
  };
  const std::vector<Case> cases = {
      {"numeric", 65540, 1, 0, {}},      {"numeric", 2490372, 38, 0, {}},
      {"numeric", 2490409, 38, 37, {}},  {"numeric", 2490410, 38, 38, {}},
      {"numeric", 2555908, {}, {}, {}},  {"numeric", 2490411, {}, {}, {}},
      {"numeric", 65542, {}, {}, {}},    {"bpchar", 4, {}, {}, {}},
      {"bpchar", 5, {}, {}, 1},          {"bpchar", 4100, {}, {}, 4096},
      {"bpchar", 4101, {}, {}, 4097},    {"varchar", 65539, {}, {}, 65535},
      {"varchar", 65540, {}, {}, 65536}, {"varchar", 16000004, {}, {}, 16000000},
      {"varchar", 16000005, {}, {}, {}}, {"varbyte", 100, {}, {}, {}},
      {"intervald2s", 6, {}, {}, {}},
  };
  for (const auto& item : cases) {
    const auto info = profile.DescribeTypeModifier(item.name, item.raw);
    EXPECT_EQ(info.precision, item.precision) << item.name << " " << item.raw;
    EXPECT_EQ(info.scale, item.scale);
    EXPECT_EQ(info.byte_width, item.width);
  }
  for (int32_t raw : {-1, 0, 3, 4, (std::numeric_limits<int32_t>::min)(),
                      (std::numeric_limits<int32_t>::max)()}) {
    const auto info = profile.DescribeTypeModifier("numeric", raw);
    EXPECT_FALSE(info.precision);
    EXPECT_FALSE(info.scale);
  }
}

TEST(TypeModifierTest, CheckedCatalogParsingRejectsMalformedAndOverflowModifiers) {
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::ModifierResolver(BackendProfile::Redshift(), &resolver), 0);
  for (const char* bad :
       {"", " 4", "4x", "1.0", "2147483648", "-2147483649", "9223372036854775808"}) {
    PqResultHelper table(nullptr, "");
    TextCatalog(&table, 3, {{"n", "73001", bad}});
    PostgresType root;
    EXPECT_FALSE(table.ResolveTableSchema(*resolver, &root).ok()) << bad;
  }
  for (const char* bad_oid : {"-1", "4294967296", "x"}) {
    PqResultHelper table(nullptr, "");
    TextCatalog(&table, 3, {{"n", bad_oid, "-1"}});
    PostgresType root;
    EXPECT_FALSE(table.ResolveTableSchema(*resolver, &root).ok());
  }
  for (const char* raw : {"-2147483648", "2147483647", "-1"}) {
    PqResultHelper table(nullptr, "");
    TextCatalog(&table, 3, {{"n", "73001", raw}});
    PostgresType root;
    ASSERT_TRUE(table.ResolveTableSchema(*resolver, &root).ok());
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(root.SetSchema(schema.get(), "Redshift"), 0);
    EXPECT_EQ(FieldMetadata(schema->children[0], "ADBC:redshift:type_modifier"), raw);
    EXPECT_FALSE(FieldMetadata(schema->children[0], "ADBC:redshift:precision"));
  }
}

TEST(TypeModifierTest, GetObjectsPreservesDerivedDescriptorsAndKeepsUnknownFieldsNull) {
  PqResultHelper catalog(nullptr, "");
  TextCatalog(&catalog, 5,
              {{"n", "1", std::nullopt, "numeric", "2490409"},
               {"v", "3", "remark", "varchar", "24"},
               {"u", "4", std::nullopt, "numeric", "-1"},
               {"derived_scale38", "5", std::nullopt, "numeric", "2490410"},
               {"derived_wide", "6", std::nullopt, "varchar", "70004"}});
  for (int row = 0; row < 5; row++) {
    auto parsed = ResolveColumnMetadata(catalog.Row(row), BackendProfile::Redshift());
    ASSERT_TRUE(parsed.has_value());
    const auto& column = parsed.value();
    ASSERT_TRUE(column.xdbc);
    if (row == 0) {
      EXPECT_EQ(column.xdbc->xdbc_column_size, 38);
      EXPECT_EQ(column.xdbc->xdbc_decimal_digits, 37);
      EXPECT_EQ(column.xdbc->xdbc_num_prec_radix, 10);
    } else if (row == 1) {
      EXPECT_EQ(column.ordinal_position, 3);
      EXPECT_EQ(column.remarks, "remark");
      EXPECT_EQ(column.xdbc->xdbc_column_size, 20);
      EXPECT_EQ(column.xdbc->xdbc_char_octet_length, 20);
    } else if (row == 3) {
      EXPECT_EQ(column.xdbc->xdbc_column_size, 38);
      EXPECT_EQ(column.xdbc->xdbc_decimal_digits, 38);
    } else if (row == 4) {
      EXPECT_EQ(column.xdbc->xdbc_column_size, 70000);
      EXPECT_EQ(column.xdbc->xdbc_char_octet_length, 70000);
    } else {
      EXPECT_FALSE(column.xdbc->xdbc_column_size);
      EXPECT_FALSE(column.xdbc->xdbc_decimal_digits);
    }
    auto pg = ResolveColumnMetadata(catalog.Row(row), BackendProfile::PostgreSQL());
    ASSERT_TRUE(pg.has_value());
    EXPECT_FALSE(pg.value().xdbc->xdbc_column_size);
  }
  for (const char* bad : {"0", "-1", "2147483648", "one"}) {
    TextCatalog(&catalog, 5, {{"n", bad, std::nullopt, "numeric", "655366"}});
    EXPECT_FALSE(
        ResolveColumnMetadata(catalog.Row(0), BackendProfile::Redshift()).has_value());
  }
  TextCatalog(&catalog, 5, {{"n", "1", std::nullopt, "numeric", "2147483648"}});
  EXPECT_FALSE(
      ResolveColumnMetadata(catalog.Row(0), BackendProfile::Redshift()).has_value());
}

TEST(TypeModifierTest, EmptyCatalogAndPostgreSQLPublicMetadataStayUnchanged) {
  PqResultHelper table(nullptr, "");
  TextCatalog(&table, 3, {});
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::ModifierResolver(BackendProfile::Redshift(), &resolver), 0);
  PostgresType root;
  ASSERT_TRUE(table.ResolveTableSchema(*resolver, &root).ok());
  EXPECT_EQ(root.n_children(), 0);
  EXPECT_FALSE(
      ResolveColumnMetadata(table.Row(0), BackendProfile::Redshift()).has_value());
  ASSERT_EQ(testing::ModifierResolver(BackendProfile::PostgreSQL(), &resolver), 0);
  PqResultHelper query(nullptr, "");
  ASSERT_EQ(testing::ModifierDescription(&query, true), 0);
  ASSERT_TRUE(query.ResolveOutputTypes(*resolver, &root).ok());
  EXPECT_EQ(root.child(0).type_modifier(), 655366);
  nanoarrow::UniqueSchema schema;
  ArrowSchemaInit(schema.get());
  ASSERT_EQ(root.SetSchema(schema.get()), 0);
  EXPECT_FALSE(FieldMetadata(schema->children[0], "ADBC:redshift:type_modifier"));
  EXPECT_FALSE(FieldMetadata(schema->children[0], "ADBC:redshift:precision"));
  EXPECT_EQ(FieldMetadata(schema->children[0], "POSTGRESQL:type"), "numeric");
}

}  // namespace
}  // namespace adbcpq
