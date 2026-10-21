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
#include <string>
#include <utility>
#include <vector>
#include "bind_stream.h"
#include "copy/reader.h"
#include "driver/framework/utility.h"
namespace adbcpq {
namespace {
#include "redshift_vendor_corpus_test.inc"
std::string Unhex(std::string_view hex) {
  std::string out;
  auto digit = [](char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                  : c - 'A' + 10;
  };
  for (size_t i = 0; i < hex.size(); i += 2)
    out.push_back(static_cast<char>(digit(hex[i]) * 16 + digit(hex[i + 1])));
  return out;
}
ArrowBufferView Bytes(std::string_view s) {
  ArrowBufferView out{};
  out.data.as_char = s.data();
  out.size_bytes = s.size();
  return out;
}
void EncodedField(ArrowSchema* out, RedshiftEncodedKind kind) {
  ArrowSchemaInit(out);
  ASSERT_EQ(RedshiftEncodedSchema::SetSchema(out, kind), 0);
  nanoarrow::UniqueBuffer metadata;
  ASSERT_EQ(ArrowMetadataBuilderInit(metadata.get(), out->metadata), 0);
  ASSERT_EQ(
      ArrowMetadataBuilderAppend(metadata.get(), ArrowCharView("ADBC:redshift:sql_type"),
                                 ArrowCharView(RedshiftEncodedSchema::Family(kind))),
      0);
  ASSERT_EQ(ArrowSchemaSetMetadata(out, reinterpret_cast<char*>(metadata->data)), 0);
}
TEST(RedshiftEncodedTest, GeometryProtocolAliasIsOutputOnlyAndBackendScoped) {
  const auto redshift = adbc::driver::pgwire::BackendProfile::Redshift();
  for (uint32_t catalog_oid : {3000U, 72113U, 3999U}) {
    PostgresTypeResolver resolver(redshift);
    ArrowError error{};
    ASSERT_EQ(resolver.Insert({catalog_oid, "geometry", "fixture", 0, 0, 0}, &error), 0);
    PqResultHelper result(nullptr, "");
    auto* raw = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
    PGresAttDesc attr{};
    attr.name = const_cast<char*>("any_alias");
    attr.typid = 3999;
    attr.format = 1;
    attr.atttypmod = 1048447;
    attr.tableid = 12345;
    attr.columnid = 2;
    PGresAttDesc attrs[] = {attr, attr};
    attrs[1].name = const_cast<char*>("second_alias");
    attrs[1].atttypmod = -1;
    ASSERT_TRUE(PQsetResultAttrs(raw, 2, attrs));
    result.SetResult(raw);
    PostgresType output;
    ASSERT_TRUE(result.ResolveOutputTypes(resolver, &output).ok());
    ASSERT_EQ(output.n_children(), 2);
    const auto& field = output.child(0);
    EXPECT_EQ(field.oid(), 3999U);
    EXPECT_EQ(field.type_modifier(), 1048447);
    EXPECT_EQ(field.field_name(), "any_alias");
    EXPECT_EQ(field.encoded_kind(), RedshiftEncodedKind::kGeometry);
    EXPECT_EQ(output.child(1).oid(), 3999U);
    EXPECT_EQ(output.child(1).type_modifier(), -1);
    EXPECT_EQ(resolver.GetVendorOID(adbc::driver::pgwire::CatalogVendorFamily::kGeometry),
              catalog_oid);
    PostgresType catalog;
    ASSERT_EQ(resolver.Find(catalog_oid, &catalog, &error), 0);
    EXPECT_EQ(catalog.oid(), catalog_oid);
    EXPECT_FALSE(catalog.type_modifier().has_value());
    if (catalog_oid != 3999) EXPECT_NE(resolver.Find(3999, &catalog, &error), 0);
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(output.SetSchema(schema.get(), "Redshift"), 0);
    EXPECT_STREQ(schema->children[0]->format, "Z");
    RedshiftEncodedSchema parsed;
    ASSERT_EQ(RedshiftEncodedSchema::Parse(schema->children[0], true, &parsed, &error),
              0);
    EXPECT_EQ(parsed.kind, RedshiftEncodedKind::kGeometry);
  }

  PostgresTypeResolver postgres;
  ArrowError error{};
  ASSERT_EQ(postgres.Insert({3000, "geometry", "fixture", 0, 0, 0}, &error), 0);
  PostgresType output;
  ASSERT_EQ(postgres.FindOutput(3999, &output, &error), 0);
  EXPECT_EQ(output.encoded_kind(), RedshiftEncodedKind::kNone);
  EXPECT_EQ(output.typname(), "unnamed<oid:3999>");
  PostgresTypeResolver redshift_resolver(redshift);
  ASSERT_EQ(redshift_resolver.FindOutput(3998, &output, &error), 0);
  EXPECT_EQ(output.typname(), "unnamed<oid:3998>");
}
TEST(RedshiftEncodedTest, GeometryOutputAliasFailuresPropagateThroughResultResolution) {
  for (int fault = 0; fault < 5; ++fault) {
    PostgresTypeResolver resolver(adbc::driver::pgwire::BackendProfile::Redshift());
    ArrowError error{};
    if (fault != 0)
      ASSERT_EQ(resolver.Insert({3000, "geometry", "fixture", 0, 0, 0}, &error), 0);
    if (fault == 1)
      ASSERT_EQ(resolver.Insert({72113, "geometry", "fixture", 0, 0, 0}, &error), 0);
    if (fault >= 2)
      ASSERT_EQ(resolver.Insert({3999,
                                 fault == 2   ? "other"
                                 : fault == 3 ? "geography"
                                              : "geometry",
                                 "fixture", 0, 0, 0},
                                &error),
                0);
    PqResultHelper result(nullptr, "");
    auto* raw = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
    PGresAttDesc attr{};
    attr.name = const_cast<char*>("geometry");  // A plausible name proves nothing.
    attr.typid = 3999;
    attr.format = 1;
    ASSERT_TRUE(PQsetResultAttrs(raw, 1, &attr));
    result.SetResult(raw);
    PostgresType output;
    const auto status = result.ResolveOutputTypes(resolver, &output);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.ToAdbc(nullptr), ADBC_STATUS_INVALID_DATA);
  }
}
TEST(RedshiftEncodedTest, RetainedSpatialAndEveryTruncatedPrefix) {
  ArrowError error{};
  for (auto sample : kRetainedSpatial) {
    const auto bytes = Unhex(sample.hex);
    EXPECT_EQ(RedshiftSpatial::Validate(Bytes(bytes), sample.geography, false, &error), 0)
        << sample.hex;
    for (size_t i = 0; i < bytes.size(); ++i)
      EXPECT_NE(RedshiftSpatial::Validate(Bytes(std::string_view(bytes).substr(0, i)),
                                          sample.geography, false, &error),
                0)
          << i;
    EXPECT_NE(
        RedshiftSpatial::Validate(Bytes(bytes + "x"), sample.geography, false, &error),
        0);
  }
}
TEST(RedshiftEncodedTest, SpatialAxesEndianEmptySridAndUnknownFlags) {
  ArrowError error{};
  for (const char* hex : {"00000000013ff00000000000003ff0000000000000",
                          "0101000000000000000000f03f000000000000f03f",
                          "010100002000000000000000000000f03f000000000000f03f",
                          "0101000000000000000000f87f000000000000f87f"})
    EXPECT_EQ(RedshiftSpatial::Validate(Bytes(Unhex(hex)), false, true, &error), 0);
  EXPECT_NE(
      RedshiftSpatial::Validate(
          Bytes(Unhex("0101000000000000000000f03f000000000000f03f")), true, true, &error),
      0);
  for (const char* hex :
       {"0101000000000000000000f87f000000000000f03f",
        "0101000000000000000000f07f000000000000f03f",
        "01e9030000000000000000f03f000000000000f03f",
        "0101000010000000000000f03f000000000000f03f",
        "0201000000000000000000f03f000000000000f03f", "0102000000ffffffff"})
    EXPECT_NE(RedshiftSpatial::Validate(Bytes(Unhex(hex)), false, true, &error), 0);
  auto nested =
      Unhex("0107000020e6100000010000000101000000000000000000f03f000000000000f03f");
  EXPECT_EQ(RedshiftSpatial::Validate(Bytes(nested), true, true, &error), 0);
  auto conflict = Unhex(
      "0107000020e6100000010000000101000020110f0000000000000000f03f000000000000f03f");
  EXPECT_NE(RedshiftSpatial::Validate(Bytes(conflict), true, true, &error), 0);
  std::string chain;
  for (int i = 0; i < 127; ++i) chain += Unhex("010700000001000000");
  chain += Unhex("010700000000000000");
  EXPECT_EQ(RedshiftSpatial::Validate(Bytes(chain), false, false, &error), 0);
  chain = Unhex("010700000001000000") + chain;
  EXPECT_NE(RedshiftSpatial::Validate(Bytes(chain), false, false, &error), 0);
}
TEST(RedshiftEncodedTest, RetainedHllAndLexicalCounterexamples) {
  ArrowError error{};
  for (auto sample : kRetainedHll)
    EXPECT_EQ(RedshiftHll::Validate(Bytes(sample), &error), 0);
  for (
      auto value :
      {R"({"sparse":{"values":[1],"indices":[999999999999999999999999]},"logm":15,"\u0076ersion":1})",
       R"("AA==")", R"("AAA=")", R"("AAAA")"})
    EXPECT_EQ(RedshiftHll::Validate(Bytes(value), &error), 0) << value;
  for (auto value :
       {"", "null", "\"\"", R"("AB==")", R"("AAB=")", R"("A===")", R"("AA-_")",
        R"("AA==" )",
        R"({"version":1,"version":1,"logm":15,"sparse":{"indices":[],"values":[]}})",
        R"({"version":2,"logm":15,"sparse":{"indices":[],"values":[]}})",
        R"({"version":1,"logm":14,"sparse":{"indices":[],"values":[]}})",
        R"({"version":1,"logm":15,"sparse":{"indices":[0],"values":[]}})",
        R"({"version":1,"logm":15,"sparse":{"indices":[01],"values":[1]}})",
        R"({"version":1,"logm":15,"sparse":{"indices":[-1],"values":[1]}})",
        R"({"version":1,"logm":15,"sparse":{"indices":[1.0],"values":[1]}})"})
    EXPECT_NE(RedshiftHll::Validate(Bytes(value), &error), 0) << value;
  EXPECT_NE(RedshiftHll::Validate(Bytes(std::string("\"AAAA\"\0", 7)), &error), 0);
  EXPECT_EQ(RedshiftHll::Validate(Bytes("\"" + std::string(999996, 'A') + "\""), &error),
            0);
  EXPECT_NE(RedshiftHll::Validate(Bytes("\"" + std::string(1000000, 'A') + "\""), &error),
            0);
}
TEST(RedshiftEncodedTest, StorageRoutesBackendAndDynamicIdentity) {
  ArrowError error{};
  bool has = false;
  for (auto kind : {RedshiftEncodedKind::kGeometry, RedshiftEncodedKind::kGeography,
                    RedshiftEncodedKind::kHll}) {
    nanoarrow::UniqueSchema schema;
    EncodedField(schema.get(), kind);
    RedshiftEncodedSchema parsed;
    EXPECT_EQ(RedshiftEncodedSchema::Parse(schema.get(), true, &parsed, &error), 0);
    EXPECT_EQ(parsed.kind, kind);
    EXPECT_NE(RedshiftEncodedSchema::Parse(schema.get(), false, &parsed, &error), 0);
    EXPECT_EQ(
        RedshiftSuperSchema::ValidateRoute(schema.get(), true, true, false, &has, &error),
        0);
    EXPECT_NE(
        RedshiftSuperSchema::ValidateRoute(schema.get(), true, true, true, &has, &error),
        0);
    EXPECT_EQ(RedshiftSuperSchema::ValidateRoute(schema.get(), true, false, false, &has,
                                                 &error) == 0,
              kind == RedshiftEncodedKind::kHll);
    ASSERT_EQ(ArrowSchemaSetType(schema.get(), NANOARROW_TYPE_BINARY), 0);
    EXPECT_NE(RedshiftEncodedSchema::Parse(schema.get(), true, &parsed, &error), 0);
  }
}
TEST(RedshiftEncodedTest, ActualOwnedWriterPhysicalNulAndSpatialRawBytes) {
  ArrowError error{};
  std::vector<std::pair<RedshiftEncodedKind, std::string>> samples;
  for (auto sample : kRetainedSpatial)
    samples.emplace_back(sample.geography ? RedshiftEncodedKind::kGeography
                                          : RedshiftEncodedKind::kGeometry,
                         Unhex(sample.hex));
  for (auto sample : kRetainedHll)
    samples.emplace_back(RedshiftEncodedKind::kHll, sample);
  for (const auto& [kind, value] : samples) {
    nanoarrow::UniqueSchema schema;
    EncodedField(schema.get(), kind);
    nanoarrow::UniqueArray array;
    ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), &error), 0);
    ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
    auto value_bytes = Bytes(value);
    ASSERT_EQ(ArrowArrayAppendBytes(array.get(), value_bytes), 0);
    ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), &error), 0);
    nanoarrow::UniqueArrayView view;
    ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error), 0);
    ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), &error), 0);
    auto writer =
        PostgresCopyFieldWriter::Create<RedshiftEncodedFieldWriter>(view.get(), kind);
    nanoarrow::UniqueBuffer wire;
    ASSERT_EQ(writer->Write(wire.get(), 0, &error), 0);
    const int64_t size =
        kind == RedshiftEncodedKind::kHll ? value.size() + 1 : value.size();
    EXPECT_EQ(wire->size_bytes, 4 + size);
    auto bytes =
        Bytes(std::string_view(reinterpret_cast<char*>(wire->data), wire->size_bytes));
    EXPECT_EQ(ReadUnsafe<int32_t>(&bytes), size);
    if (kind == RedshiftEncodedKind::kHll) {
      EXPECT_EQ(std::string_view(bytes.data.as_char, value.size()), value);
      EXPECT_EQ(bytes.data.as_char[value.size()], 0);
    } else
      EXPECT_EQ(std::string_view(bytes.data.as_char, size), value);
  }
}
TEST(RedshiftEncodedTest, TypedReaderPreservesBytesAndValidPrefixAcrossNulls) {
  ArrowError error{};
  for (auto kind : {RedshiftEncodedKind::kGeometry, RedshiftEncodedKind::kGeography,
                    RedshiftEncodedKind::kHll}) {
    nanoarrow::UniqueSchema schema;
    EncodedField(schema.get(), kind);
    nanoarrow::UniqueArray array;
    ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), &error), 0);
    ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
    RedshiftEncodedFieldReader reader(kind);
    ASSERT_EQ(reader.InitSchema(schema.get()), 0);
    ASSERT_EQ(reader.InitArray(array.get()), 0);
    std::string value =
        kind == RedshiftEncodedKind::kHll
            ? std::string(kRetainedHll[1])
            : Unhex(kind == RedshiftEncodedKind::kGeometry ? kRetainedSpatial[0].hex
                                                           : kRetainedSpatial[36].hex);
    for (int i = 0; i < 9; ++i) {
      auto bytes = Bytes(value);
      ASSERT_EQ(reader.Read(&bytes, value.size(), array.get(), &error), 0);
      EXPECT_EQ(bytes.size_bytes, 0);
    }
    auto empty = Bytes("");
    ASSERT_EQ(reader.Read(&empty, -1, array.get(), &error), 0);
    ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), &error), 0);
    nanoarrow::UniqueArrayView view;
    ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error), 0);
    ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), &error), 0);
    for (int i = 0; i < 9; ++i) {
      EXPECT_FALSE(ArrowArrayViewIsNull(view.get(), i));
      auto actual = ArrowArrayViewGetBytesUnsafe(view.get(), i);
      EXPECT_EQ(std::string_view(actual.data.as_char, actual.size_bytes), value);
    }
    EXPECT_TRUE(ArrowArrayViewIsNull(view.get(), 9));
  }
}
TEST(RedshiftEncodedTest, ResultFormatSelectionIsAllHllOnly) {
  using F = adbc::driver::pgwire::CatalogVendorFamily;
  PostgresType root(PostgresTypeId::kRecord);
  int format = -1;
  EXPECT_TRUE(SelectVendorResultFormat(root, &format).ok());
  EXPECT_EQ(format, 1);
  auto hll = PostgresType(PostgresTypeId::kUserDefined).WithVendorFamily(F::kHll);
  root.AppendChild("s", hll);
  EXPECT_TRUE(SelectVendorResultFormat(root, &format).ok());
  EXPECT_EQ(format, 0);
  root.AppendChild("id", PostgresType(PostgresTypeId::kInt4));
  EXPECT_FALSE(SelectVendorResultFormat(root, &format).ok());
}
TEST(RedshiftEncodedTest, StagedWriterAndReaderFailuresPreservePrefixAndCursor) {
  ArrowError error{};
  nanoarrow::UniqueSchema schema;
  EncodedField(schema.get(), RedshiftEncodedKind::kHll);
  nanoarrow::UniqueArray input;
  ASSERT_EQ(ArrowArrayInitFromSchema(input.get(), schema.get(), &error), 0);
  ASSERT_EQ(ArrowArrayStartAppending(input.get()), 0);
  ASSERT_EQ(ArrowArrayAppendString(input.get(), ArrowCharView(kRetainedHll[0].data())),
            0);
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(input.get(), &error), 0);
  nanoarrow::UniqueArrayView view;
  ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error), 0);
  ASSERT_EQ(ArrowArrayViewSetArray(view.get(), input.get(), &error), 0);
  auto allocator = ArrowBufferAllocatorDefault();
  allocator.reallocate = [](ArrowBufferAllocator*, uint8_t* old, int64_t,
                            int64_t) -> uint8_t* {
    EXPECT_EQ(old, nullptr);
    return nullptr;
  };
  auto writer = PostgresCopyFieldWriter::Create<RedshiftEncodedFieldWriter>(
      view.get(), RedshiftEncodedKind::kHll, allocator);
  nanoarrow::UniqueBuffer wire;
  ASSERT_EQ(ArrowBufferAppend(wire.get(), "old", 3), 0);
  wire->capacity_bytes = 3;
  const auto* pointer = wire->data;
  EXPECT_EQ(writer->Write(wire.get(), 0, &error), ENOMEM);
  EXPECT_EQ(wire->data, pointer);
  EXPECT_EQ(wire->size_bytes, 3);
  EXPECT_EQ(std::string_view(reinterpret_cast<char*>(wire->data), 3), "old");
  for (int fail_at = 0; fail_at < 3; ++fail_at) {
    int remaining = fail_at;
    auto allocation = ArrowBufferAllocatorDefault();
    allocation.private_data = &remaining;
    allocation.reallocate = [](ArrowBufferAllocator* a, uint8_t* old, int64_t,
                               int64_t size) -> uint8_t* {
      EXPECT_EQ(old, nullptr);
      int& n = *static_cast<int*>(a->private_data);
      if (n-- == 0) return nullptr;
      return static_cast<uint8_t*>(std::malloc(size));
    };
    nanoarrow::UniqueArray out;
    ASSERT_EQ(ArrowArrayInitFromSchema(out.get(), schema.get(), &error), 0);
    ASSERT_EQ(ArrowArrayStartAppending(out.get()), 0);
    ArrowArrayBuffer(out.get(), 1)->capacity_bytes =
        ArrowArrayBuffer(out.get(), 1)->size_bytes;
    RedshiftEncodedFieldReader reader(RedshiftEncodedKind::kHll, allocation);
    ASSERT_EQ(reader.InitSchema(schema.get()), 0);
    ASSERT_EQ(reader.InitArray(out.get()), 0);
    auto data = Bytes(kRetainedHll[1]);
    const auto* cursor = data.data.as_char;
    EXPECT_EQ(reader.Read(&data, data.size_bytes, out.get(), &error), ENOMEM) << fail_at;
    EXPECT_EQ(data.data.as_char, cursor);
    EXPECT_EQ(out->length, 0);
    EXPECT_EQ(out->null_count, 0);
    EXPECT_EQ(ArrowArrayBuffer(out.get(), 1)->size_bytes, 8);
    EXPECT_EQ(ArrowArrayBuffer(out.get(), 2)->size_bytes, 0);
    EXPECT_EQ(ArrowArrayValidityBitmap(out.get())->buffer.data, nullptr);
    remaining = 10;
    ASSERT_EQ(reader.Read(&data, data.size_bytes, out.get(), &error), 0);
    auto null = Bytes("");
    ASSERT_EQ(reader.Read(&null, -1, out.get(), &error), 0);
    EXPECT_EQ(out->length, 2);
    EXPECT_EQ(out->null_count, 1);
    ASSERT_EQ(ArrowArrayFinishBuildingDefault(out.get(), &error), 0);
  }
}
TEST(RedshiftEncodedTest, SourceFamilyNeedsUnambiguousBackendCacheAndExactTarget) {
  PostgresTypeResolver resolver(adbc::driver::pgwire::BackendProfile::Redshift());
  ArrowError error{};
  nanoarrow::UniqueSchema schema;
  EncodedField(schema.get(), RedshiftEncodedKind::kHll);
  EXPECT_NE(PostgresType::ValidateVendorWriteSchema(resolver, schema.get(), &error), 0);
  ASSERT_EQ(resolver.Insert({72115, "hllsketch", "fixture", 0, 0, 0}, &error), 0);
  EXPECT_EQ(PostgresType::ValidateVendorWriteSchema(resolver, schema.get(), &error), 0);
  PostgresType hll;
  ASSERT_EQ(resolver.Find(72115, &hll, &error), 0);
  hll = hll.WithFieldDescriptor(-1, resolver.backend_profile());
  nanoarrow::UniqueSchema described;
  ArrowSchemaInit(described.get());
  ASSERT_EQ(hll.SetSchema(described.get(), "Redshift"), 0);
  RedshiftEncodedSchema parsed;
  EXPECT_EQ(RedshiftEncodedSchema::Parse(described.get(), true, &parsed, &error), 0);
  EXPECT_EQ(parsed.kind, RedshiftEncodedKind::kHll);
  nanoarrow::UniqueBuffer conflict;
  ASSERT_EQ(ArrowMetadataBuilderInit(conflict.get(), described->metadata), 0);
  ASSERT_EQ(ArrowMetadataBuilderSet(conflict.get(), ArrowCharView("POSTGRESQL:type"),
                                    ArrowCharView("geometry")),
            0);
  ASSERT_EQ(
      ArrowSchemaSetMetadata(described.get(), reinterpret_cast<char*>(conflict->data)),
      0);
  EXPECT_NE(RedshiftEncodedSchema::Parse(described.get(), true, &parsed, &error), 0);
  ASSERT_EQ(resolver.Insert({72116, "hllsketch", "fixture", 0, 0, 0}, &error), 0);
  EXPECT_NE(PostgresType::ValidateVendorWriteSchema(resolver, schema.get(), &error), 0);
}
std::string SizedSpatial(int64_t size) {
  // A collection of one finite LineString plus a few empty collections makes
  // exact synthetic byte boundaries without trailing padding or fake lengths.
  int empty = 0;
  while ((size - 18 - 9 * empty) % 16) ++empty;
  auto u32 = [](std::string* out, uint32_t n) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>(n >> (i * 8)));
  };
  std::string out(1, 1);
  u32(&out, 7);
  u32(&out, 1 + empty);
  out.push_back(1);
  u32(&out, 2);
  const int64_t points = (size - 18 - 9 * empty) / 16;
  u32(&out, points);
  out.append(points * 16, 0);
  for (int i = 0; i < empty; ++i) {
    out.push_back(1);
    u32(&out, 7);
    u32(&out, 0);
  }
  return out;
}
TEST(RedshiftEncodedTest, ExactLocalReadWriteAndLogicalHllByteBoundaries) {
  ArrowError error{};
  for (int64_t n : {500000, 500001, 1048447, 1048448}) {
    const auto value = SizedSpatial(n);
    ASSERT_EQ(value.size(), n);
    EXPECT_EQ(RedshiftSpatial::Validate(Bytes(value), false, true, &error) == 0,
              n <= 500000);
    EXPECT_EQ(RedshiftSpatial::Validate(Bytes(value), false, false, &error) == 0,
              n <= 1048447);
  }
  std::string hll(kRetainedHll[0]);
  hll.append(1000000 - hll.size(), ' ');
  EXPECT_EQ(RedshiftHll::Validate(Bytes(hll), &error), 0);
  hll.push_back(' ');
  EXPECT_NE(RedshiftHll::Validate(Bytes(hll), &error), 0);
}
TEST(RedshiftEncodedTest, ValidRowsFirstNullReserveFailureThenRetryKeepPrefix) {
  ArrowError error{};
  nanoarrow::UniqueSchema schema;
  EncodedField(schema.get(), RedshiftEncodedKind::kHll);
  nanoarrow::UniqueArray out;
  ASSERT_EQ(ArrowArrayInitFromSchema(out.get(), schema.get(), &error), 0);
  ASSERT_EQ(ArrowArrayStartAppending(out.get()), 0);
  int remaining = 100;
  auto allocation = ArrowBufferAllocatorDefault();
  allocation.private_data = &remaining;
  allocation.reallocate = [](ArrowBufferAllocator* a, uint8_t* old, int64_t,
                             int64_t size) -> uint8_t* {
    EXPECT_EQ(old, nullptr);
    int& n = *static_cast<int*>(a->private_data);
    if (n-- == 0) return nullptr;
    return static_cast<uint8_t*>(std::malloc(size));
  };
  RedshiftEncodedFieldReader reader(RedshiftEncodedKind::kHll, allocation);
  ASSERT_EQ(reader.InitSchema(schema.get()), 0);
  ASSERT_EQ(reader.InitArray(out.get()), 0);
  for (int i = 0; i < 8; ++i) {
    auto data = Bytes(kRetainedHll[0]);
    ASSERT_EQ(reader.Read(&data, data.size_bytes, out.get(), &error), 0);
  }
  auto* bitmap = ArrowArrayValidityBitmap(out.get());
  bitmap->buffer.capacity_bytes = bitmap->buffer.size_bytes;
  const auto values_size = ArrowArrayBuffer(out.get(), 2)->size_bytes;
  const std::string values(reinterpret_cast<char*>(ArrowArrayBuffer(out.get(), 2)->data),
                           values_size);
  remaining = 0;
  auto empty = Bytes("");
  EXPECT_EQ(reader.Read(&empty, -1, out.get(), &error), ENOMEM);
  EXPECT_EQ(out->length, 8);
  EXPECT_EQ(out->null_count, 0);
  EXPECT_EQ(bitmap->size_bits, 8);
  EXPECT_EQ(bitmap->buffer.data[0], 255);
  EXPECT_EQ(ArrowArrayBuffer(out.get(), 2)->size_bytes, values_size);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(ArrowArrayBuffer(out.get(), 2)->data),
                        values_size),
            values);
  remaining = 100;
  auto valid = Bytes(kRetainedHll[0]);
  ASSERT_EQ(reader.Read(&valid, valid.size_bytes, out.get(), &error), 0);
  ASSERT_EQ(reader.Read(&empty, -1, out.get(), &error), 0);
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(out.get(), &error), 0);
  EXPECT_EQ(out->length, 10);
  EXPECT_EQ(out->null_count, 1);
  EXPECT_EQ(bitmap->buffer.data[0], 255);
  EXPECT_EQ(bitmap->buffer.data[1] & 3, 1);
}
TEST(RedshiftEncodedTest, GenuineResultDescriptorsRejectEveryIdentityAndFormatDrift) {
  auto result = [](int fault) {
    auto* out = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
    PGresAttDesc attrs[2]{};
    for (int i = 0; i < 2; ++i) {
      attrs[i].name = const_cast<char*>(i ? "second" : "first");
      attrs[i].typid = 72115;
      attrs[i].atttypmod = -1;
      attrs[i].tableid = 91;
      attrs[i].columnid = i + 1;
      attrs[i].format = 0;
    }
    if (fault == 1) attrs[0].name = const_cast<char*>("second");
    if (fault == 2) attrs[0].typid = 72111;
    if (fault == 3) attrs[0].atttypmod = 0;
    if (fault == 4) attrs[0].tableid = 92;
    if (fault == 5) attrs[0].columnid = 2;
    if (fault == 6) attrs[0].format = 1;
    if (!PQsetResultAttrs(out, fault == 7 ? 1 : 2, attrs)) std::abort();
    return out;
  };
  PqResultHelper helper(nullptr, "");
  helper.SetResult(result(0));
  adbc::driver::pgwire::UniqueResult expected(helper.CopyDescriptor());
  ASSERT_TRUE(expected);
  EXPECT_TRUE(helper.CheckDescriptor(expected.get(), 0).ok());
  EXPECT_FALSE(helper.CheckDescriptor(expected.get(), 1).ok());
  for (int fault = 1; fault <= 7; ++fault) {
    helper.SetResult(result(fault));
    EXPECT_FALSE(helper.CheckDescriptor(expected.get(), 0).ok()) << fault;
  }
}
TEST(RedshiftEncodedTest, InferredHllDoesNotTurnUnmarkedNullIntoSemanticSource) {
  PostgresTypeResolver resolver(adbc::driver::pgwire::BackendProfile::Redshift());
  ArrowError error{};
  ASSERT_EQ(resolver.Insert({72115, "hllsketch", "fixture", 0, 0, 0}, &error), 0);
  nanoarrow::UniqueSchema schema;
  ArrowSchemaInit(schema.get());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_NA), 0);
  nanoarrow::UniqueArray array;
  ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), &error), 0);
  ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
  ASSERT_EQ(ArrowArrayAppendNull(array->children[0], 1), 0);
  ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), &error), 0);
  nanoarrow::UniqueArrayStream input;
  adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
  BindStream bind;
  bind.SetBind(input.get());
  ASSERT_TRUE(bind.Begin([] { return Status::Ok(); }).ok());
  PostgresType hll, expected(PostgresTypeId::kRecord);
  ASSERT_EQ(resolver.Find(72115, &hll, &error), 0);
  expected.AppendChild("amount", hll);
  ASSERT_TRUE(bind.ReconcileWithExpectedTypes(expected).ok());
  EXPECT_FALSE(bind.SetParamTypes(nullptr, resolver, false).ok());
}
}  // namespace
}  // namespace adbcpq
