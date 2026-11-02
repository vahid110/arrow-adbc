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
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "bind_stream.h"
#include "driver/framework/utility.h"
#include "redshift_bytes_test_fixture.h"

namespace adbcpq {
namespace {
constexpr const char* kPoint = "0101000020E6100000000000000000F03F0000000000000040";
constexpr const char* kEmpty = "0102000020E610000000000000";
std::string SpatialRaw(std::string hex) {
  for (char& c : hex)
    if (c >= 'A' && c <= 'F') c += 'a' - 'A';
  return testing::NumericHex(hex.c_str());
}
ArrowBufferView SpatialBytes(const std::string& value) {
  ArrowBufferView bytes{};
  bytes.data.as_char = value.data();
  bytes.size_bytes = value.size();
  return bytes;
}
void SpatialField(ArrowSchema* field, RedshiftEncodedKind kind) {
  ASSERT_EQ(RedshiftEncodedSchema::SetSchema(field, kind), 0);
  nanoarrow::UniqueBuffer metadata;
  ASSERT_EQ(ArrowMetadataBuilderInit(metadata.get(), field->metadata), 0);
  ASSERT_EQ(
      ArrowMetadataBuilderAppend(metadata.get(), ArrowCharView("ADBC:redshift:sql_type"),
                                 ArrowCharView(RedshiftEncodedSchema::Family(kind))),
      0);
  ASSERT_EQ(ArrowSchemaSetMetadata(field, reinterpret_cast<char*>(metadata->data)), 0);
}
void SpatialCatalog(PostgresTypeResolver* resolver) {
  ASSERT_EQ(resolver->Insert({81101, "geometry", "fixture", 0, 0, 0}, nullptr), 0);
  ASSERT_EQ(resolver->Insert({81102, "geography", "fixture", 0, 0, 0}, nullptr), 0);
  ASSERT_EQ(resolver->Insert({81103, "varbyte", "varbyte_recv", 0, 0, 0}, nullptr), 0);
  ASSERT_EQ(resolver->Insert({23, "int4", "int4recv", 0, 0, 0}, nullptr), 0);
}
TEST(RedshiftSpatialParameterTest, ExactTextOwnedTerminatorAndRawIngest) {
  for (auto kind : {RedshiftEncodedKind::kGeometry, RedshiftEncodedKind::kGeography}) {
    PostgresTypeResolver resolver(adbc::driver::pgwire::BackendProfile::Redshift());
    SpatialCatalog(&resolver);
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    SpatialField(schema.get(), kind);
    PostgresType target;
    ASSERT_EQ(PostgresType::FromSchema(resolver, schema.get(), &target, nullptr), 0);
    EXPECT_EQ(target.oid(), kind == RedshiftEncodedKind::kGeometry ? 81101 : 81102);
    for (const char* hex : {kPoint, kEmpty}) {
      const auto value = SpatialRaw(hex);
      nanoarrow::UniqueArray array;
      ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
      ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
      ASSERT_EQ(ArrowArrayAppendBytes(array.get(), SpatialBytes(value)), 0);
      ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
      nanoarrow::UniqueArrayView view;
      ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), nullptr), 0);
      ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), nullptr), 0);
      for (bool ingest : {false, true}) {
        std::unique_ptr<PostgresCopyFieldWriter> writer;
        ASSERT_EQ(MakeCopyFieldWriter(schema.get(), view.get(), resolver, &target,
                                      &writer, false, nullptr, ingest, true),
                  0);
        nanoarrow::UniqueBuffer output;
        ASSERT_EQ(writer->Write(output.get(), 0, nullptr), 0);
        const auto expected = ingest ? value : std::string(hex) + '\0';
        EXPECT_EQ(output->size_bytes, 4 + expected.size());
        EXPECT_EQ(std::string(reinterpret_cast<char*>(output->data + 4),
                              output->size_bytes - 4),
                  expected);
        // Serialization owns its bytes independently of the Arrow input.
        std::memset(ArrowArrayBuffer(array.get(), 2)->data, 0, value.size());
        EXPECT_EQ(std::string(reinterpret_cast<char*>(output->data + 4),
                              output->size_bytes - 4),
                  expected);
        std::memcpy(ArrowArrayBuffer(array.get(), 2)->data, value.data(), value.size());
      }
      std::unique_ptr<PostgresCopyFieldWriter> writer;
      EXPECT_NE(MakeCopyFieldWriter(schema.get(), view.get(), resolver, &target, &writer,
                                    false, nullptr),
                0);  // COPY remains unsupported.
      for (Oid wrong :
           {81103U, kind == RedshiftEncodedKind::kGeometry ? 81102U : 81101U}) {
        PostgresType other;
        ASSERT_EQ(resolver.Find(wrong, &other, nullptr), 0);
        EXPECT_NE(MakeCopyFieldWriter(schema.get(), view.get(), resolver, &other, &writer,
                                      false, nullptr, false, true),
                  0);
      }
      PostgresType alias;
      ASSERT_EQ(resolver.FindOutput(3999, &alias, nullptr), 0);
      EXPECT_NE(MakeCopyFieldWriter(schema.get(), view.get(), resolver, &alias, &writer,
                                    false, nullptr, false, true),
                0);
      PostgresTypeResolver postgres;
      EXPECT_NE(MakeCopyFieldWriter(schema.get(), view.get(), postgres, &target, &writer,
                                    false, nullptr, false, true),
                0);
    }
  }
}
TEST(RedshiftSpatialParameterTest, InvalidPayloadAndAllocationLeaveBufferUntouched) {
  for (bool allocation_failure : {false, true}) {
    auto allocation = ArrowBufferAllocatorDefault();
    if (allocation_failure)
      allocation.reallocate = [](ArrowBufferAllocator*, uint8_t*, int64_t,
                                 int64_t) -> uint8_t* { return nullptr; };
    for (const auto& value : std::vector<std::string>{
             "", "bad", SpatialRaw(kPoint), SpatialRaw(kPoint) + "x",
             SpatialRaw("0101000000000000000000F03F0000000000000040")}) {
      nanoarrow::UniqueSchema schema;
      ASSERT_EQ(ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_LARGE_BINARY), 0);
      nanoarrow::UniqueArray array;
      ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
      ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
      ASSERT_EQ(ArrowArrayAppendBytes(array.get(), SpatialBytes(value)), 0);
      ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
      nanoarrow::UniqueArrayView view;
      ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), nullptr), 0);
      ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), nullptr), 0);
      auto writer = PostgresCopyFieldWriter::Create<RedshiftSpatialParameterFieldWriter>(
          view.get(), true, allocation);
      nanoarrow::UniqueBuffer output;
      ASSERT_EQ(ArrowBufferAppend(output.get(), "old", 3), 0);
      output->capacity_bytes = 3;
      const auto* pointer = output->data;
      const bool valid = value == SpatialRaw(kPoint);
      const int code = writer->Write(output.get(), 0, nullptr);
      if (valid && !allocation_failure) {
        EXPECT_EQ(code, 0);
      } else {
        EXPECT_EQ(code, valid ? ENOMEM : EINVAL);
        EXPECT_EQ(output->data, pointer);
        EXPECT_EQ(output->size_bytes, 3);
        EXPECT_EQ(std::string(reinterpret_cast<char*>(output->data), 3), "old");
      }
    }
  }
}
TEST(RedshiftSpatialParameterTest, DirectCDataMixedNeighborsNullsAndPhysicalStride) {
  PostgresTypeResolver resolver(adbc::driver::pgwire::BackendProfile::Redshift());
  SpatialCatalog(&resolver);
  for (int null_at : {-1, 0, 1, 2, 3}) {
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 4), 0);
    SpatialField(schema->children[0], RedshiftEncodedKind::kGeometry);
    SpatialField(schema->children[1], RedshiftEncodedKind::kGeography);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[2], NANOARROW_TYPE_BINARY), 0);
    ASSERT_EQ(testing::DecimalMetadata(schema->children[2], "ADBC:redshift:sql_type",
                                       "VARBYTE"),
              0);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[3], NANOARROW_TYPE_INT32), 0);
    nanoarrow::UniqueArray array;
    ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
    for (int col = 0; col < 4; ++col) {
      if (col == null_at) {
        ASSERT_EQ(ArrowArrayAppendNull(array->children[col], 1), 0);
      } else if (col == 3) {
        ASSERT_EQ(ArrowArrayAppendInt(array->children[col], -71), 0);
      } else {
        const auto bytes = SpatialRaw(col == 0 ? kPoint : col == 1 ? kEmpty : "0080ff");
        ASSERT_EQ(ArrowArrayAppendBytes(array->children[col], SpatialBytes(bytes)), 0);
      }
    }
    ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
    ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
    nanoarrow::UniqueArrayStream input;
    adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
    BindStream bind;
    bind.SetBind(input.get());
    ASSERT_TRUE(bind.Begin([] { return Status::Ok(); }).ok());
    auto status = bind.SetParamTypes(nullptr, resolver, true);
    AdbcError error = ADBC_ERROR_INIT;
    const auto code = status.ToAdbc(&error);
    const std::string message = error.message ? error.message : "";
    if (error.release) error.release(&error);
    ASSERT_EQ(code, ADBC_STATUS_OK) << message;
    ASSERT_TRUE(bind.EnsureNextRow().ok());
    ASSERT_TRUE(bind.BindCurrentRow().ok());
    EXPECT_EQ(bind.param_types, (std::vector<uint32_t>{81101, 81102, 81103, 23}));
    EXPECT_EQ(bind.logical_param_types, bind.param_types);
    EXPECT_EQ(bind.param_formats, (std::vector<int>{0, 0, 1, 1}));
    for (int col = 0; col < 4; ++col) {
      if (col == null_at) {
        EXPECT_EQ(bind.param_values[col], nullptr);
        EXPECT_EQ(bind.param_lengths[col], 0);
      } else {
        const auto expected = col == 0   ? std::string(kPoint) + '\0'
                              : col == 1 ? std::string(kEmpty) + '\0'
                                         : SpatialRaw(col == 2 ? "0080ff" : "ffffffb9");
        EXPECT_EQ(std::string(bind.param_values[col], bind.param_lengths[col]), expected);
        EXPECT_GE(reinterpret_cast<uint8_t*>(bind.param_values[col]),
                  bind.param_buffer->data);
        EXPECT_LE(
            reinterpret_cast<uint8_t*>(bind.param_values[col]) + bind.param_lengths[col],
            bind.param_buffer->data + bind.param_buffer->size_bytes);
      }
    }
  }
}
}  // namespace
}  // namespace adbcpq
