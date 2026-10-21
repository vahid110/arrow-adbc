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
#include <memory>
#include <string>
#include <vector>
#include "bind_stream.h"
#include "copy/reader.h"
#include "copy/writer.h"
#include "metadata.h"
#include "redshift_numeric_test_fixture.h"
#include "redshift_timetz_test_fixture.h"

namespace adbcpq {
namespace {
using adbc::driver::pgwire::BackendProfile;
using adbc::driver::pgwire::TemporalRepresentation;
class TimetzCodec {
 public:
  ArrowErrorCode Init(std::optional<int32_t> raw = 6,
                      ArrowBufferAllocator* allocation = nullptr) {
    std::shared_ptr<PostgresTypeResolver> resolver;
    NANOARROW_RETURN_NOT_OK(
        testing::TimetzResolver(BackendProfile::Redshift(), &resolver));
    NANOARROW_RETURN_NOT_OK(resolver->Find(testing::kTimetzOid, &type, &error));
    type = type.WithFieldDescriptor(raw, BackendProfile::Redshift());
    ArrowSchemaInit(schema.get());
    NANOARROW_RETURN_NOT_OK(type.SetSchema(schema.get(), "Redshift"));
    NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromSchema(array.get(), schema.get(), &error));
    NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(array.get()));
    NANOARROW_RETURN_NOT_OK(MakeCopyFieldReader(type, schema.get(), &reader, &error));
    if (allocation)
      reader = std::make_unique<RedshiftTimetzFieldReader>(
          RedshiftTimetzSchema::ReferencePrecision(raw), *allocation);
    reader->Init(type);
    NANOARROW_RETURN_NOT_OK(reader->InitSchema(schema.get()));
    return reader->InitArray(array.get());
  }
  int Read(const std::string& wire, int size = 12) {
    ArrowBufferView input{};
    input.data.as_char = wire.data();
    input.size_bytes = wire.size();
    const auto* before = input.data.data;
    const auto before_length = array->length;
    const int code = reader->Read(&input, size, array.get(), &error);
    if (code) {
      EXPECT_EQ(input.data.data, before);
      EXPECT_EQ(input.size_bytes, static_cast<int64_t>(wire.size()));
      EXPECT_EQ(array->length, before_length);
    }
    return code;
  }
  PostgresType type;
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
  std::unique_ptr<PostgresCopyFieldReader> reader;
  ArrowError error{};
};
TEST(RedshiftTimetzTest, LiteralReferenceWireKeepsCivilOffsetPairsAndNullPlaceholders) {
  struct Case {
    int64_t civil;
    int32_t east;
    const char* hex;
  };
  const Case cases[] = {{0, 0, "000000000000000000000000"},
                        {19800000000LL, 19800, "000000049c2c0600ffffb2a8"},
                        {1001, -30, "00000000000003e90000001e"},
                        {86400000000LL, 20700, "000000141dd76000ffffaf24"},
                        {1, INT32_MAX, "000000000000000180000001"},
                        {1, -INT32_MAX, "00000000000000017fffffff"}};
  for (const auto& value : cases) {
    TimetzCodec codec;
    ASSERT_EQ(codec.Init(), 0);
    auto wire = testing::NumericHex(value.hex);
    ASSERT_EQ(codec.Read(wire), 0) << codec.error.message;
    EXPECT_EQ(codec.array->children[0]->length, 1);
    EXPECT_EQ(codec.array->children[1]->length, 1);
    EXPECT_EQ(ArrowArrayBuffer(codec.array->children[0], 1)->size_bytes, 8);
    EXPECT_EQ(ArrowArrayBuffer(codec.array->children[1], 1)->size_bytes, 4);
    nanoarrow::UniqueArrayView view;
    ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), codec.schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayFinishBuildingDefault(codec.array.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayViewSetArray(view.get(), codec.array.get(), nullptr), 0);
    EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view->children[0], 0), value.civil);
    EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view->children[1], 0), value.east);
    std::shared_ptr<PostgresTypeResolver> resolver;
    ASSERT_EQ(testing::TimetzResolver(BackendProfile::Redshift(), &resolver), 0);
    std::unique_ptr<PostgresCopyFieldWriter> writer;
    ASSERT_EQ(MakeCopyFieldWriter(codec.schema.get(), view.get(), *resolver, &codec.type,
                                  &writer, false, nullptr),
              0);
    nanoarrow::UniqueBuffer bytes;
    ASSERT_EQ(writer->Write(bytes.get(), 0, nullptr), 0);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes->data), bytes->size_bytes),
              testing::NumericHex("0000000c") + wire);
    ASSERT_EQ(codec.Read("", -1), 0);
    EXPECT_EQ(codec.array->null_count, 1);
    EXPECT_EQ(ArrowArrayBuffer(codec.array->children[0], 1)->data[8], 0);
    EXPECT_EQ(ArrowArrayBuffer(codec.array->children[1], 1)->data[4], 0);
  }
}
TEST(RedshiftTimetzTest, ClosedSchemaAndReadUnknownPrecisionNeverBecomeWriteDefaults) {
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::TimetzResolver(BackendProfile::Redshift(), &resolver), 0);
  for (const char* precision : std::vector<const char*>{nullptr, "", "06", "7", "-1"}) {
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(testing::TimetzWriteSchema(schema.get(), precision), 0);
    EXPECT_NE(PostgresType::ValidateTemporalWriteSchema(*resolver, schema.get(), nullptr),
              0);
  }
  for (auto mode : {TemporalRepresentation::kArrow, TemporalRepresentation::kLossless})
    for (auto raw : {std::optional<int32_t>{}, std::optional<int32_t>{999},
                     std::optional<int32_t>{3}, std::optional<int32_t>{-1}}) {
      PostgresType cached;
      ASSERT_EQ(resolver->Find(testing::kTimetzOid, &cached, nullptr), 0);
      const auto field =
          cached.WithFieldDescriptor(raw, BackendProfile::Redshift(), mode);
      nanoarrow::UniqueSchema schema;
      ArrowSchemaInit(schema.get());
      ASSERT_EQ(field.SetSchema(schema.get(), "Redshift"), 0);
      RedshiftTimetzSchema parsed;
      ASSERT_EQ(RedshiftTimetzSchema::Parse(schema.get(), true, &parsed, nullptr, true),
                0);
      EXPECT_EQ(parsed.precision, RedshiftTimetzSchema::ReferencePrecision(raw));
      EXPECT_EQ(RedshiftTimetzSchema::Parse(schema.get(), true, &parsed, nullptr) == 0,
                RedshiftTimetzSchema::ReferencePrecision(raw).has_value());
      EXPECT_FALSE(cached.type_modifier());
      EXPECT_FALSE(cached.is_redshift_timetz());
    }
  for (int change = 0; change < 13; ++change) {
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(testing::TimetzWriteSchema(schema.get()), 0);
    auto* field = schema->children[0];
    if (change == 0) {
      ASSERT_EQ(testing::IntervalSetMetadata(field, "ARROW:extension:name",
                                             "adbc.redshift.timetz_us_offset.v2"),
                0);
    }
    if (change == 1) {
      ASSERT_EQ(testing::IntervalSetMetadata(field, "ARROW:extension:metadata", "{}"), 0);
    }
    if (change == 2) {
      ASSERT_EQ(testing::IntervalSetMetadata(field, "ADBC:redshift:sql_type", "TIME"), 0);
    }
    if (change == 3) field->children[0]->flags |= ARROW_FLAG_NULLABLE;
    if (change == 4) {
      ASSERT_EQ(ArrowSchemaSetName(field->children[0], "offset_seconds"), 0);
    }
    if (change == 5) {
      ASSERT_EQ(ArrowSchemaSetType(field->children[1], NANOARROW_TYPE_INT64), 0);
    }
    if (change == 6) {
      ASSERT_EQ(testing::IntervalSetMetadata(field->children[1], "ADBC:redshift:sql_type",
                                             "INTEGER"),
                0);
    }
    if (change == 7) {
      ASSERT_EQ(testing::IntervalSetMetadata(field->children[1],
                                             "ARROW:extension:metadata", "{}"),
                0);
    }
    if (change == 8) {
      ASSERT_EQ(
          testing::DecimalMetadata(field, "ADBC:redshift:fractional_precision", "6"), 0);
    }
    if (change == 9) {
      ASSERT_EQ(testing::IntervalSetMetadata(field, "ADBC:redshift:type_modifier", "999"),
                0);
    }
    if (change == 10) {
      ASSERT_EQ(ArrowSchemaAllocateDictionary(field), 0);
      ArrowSchemaInitFromType(field->dictionary, NANOARROW_TYPE_INT32);
    }
    if (change == 11) field->flags |= ARROW_FLAG_DICTIONARY_ORDERED;
    if (change == 12) field->children[0]->flags |= ARROW_FLAG_MAP_KEYS_SORTED;
    EXPECT_NE(PostgresType::ValidateTemporalWriteSchema(*resolver, schema.get(), nullptr),
              0)
        << change;
  }
  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(testing::TimetzWriteSchema(schema.get()), 0);
  RedshiftTimetzSchema parsed;
  EXPECT_EQ(RedshiftTimetzSchema::Parse(schema->children[0], false, &parsed, nullptr),
            ENOTSUP);
  for (const char* name :
       {"adbc.redshift.time_us.v2", "adbc.redshift.timestamp_us_2000.v2",
        "adbc.redshift.timetz.v999"}) {
    ASSERT_EQ(
        testing::IntervalSetMetadata(schema->children[0], "ARROW:extension:name", name),
        0);
    EXPECT_NE(PostgresType::ValidateTemporalWriteSchema(*resolver, schema.get(), nullptr),
              0);
  }
}
TEST(RedshiftTimetzTest, StrictMalformedFieldsPrecisionAndSignPreserveReaderState) {
  const auto good = testing::NumericHex("00000000000003e90000001e");
  for (int size : {0, 8, 11, 13, -2}) {
    TimetzCodec codec;
    ASSERT_EQ(codec.Init(), 0);
    EXPECT_NE(codec.Read(good, size), 0);
  }
  for (const char* hex : {"ffffffffffffffff00000000", "000000141dd7600100000000",
                          "000000000000000080000000"}) {
    TimetzCodec codec;
    ASSERT_EQ(codec.Init(), 0);
    EXPECT_NE(codec.Read(testing::NumericHex(hex)), 0);
  }
  TimetzCodec short_field;
  ASSERT_EQ(short_field.Init(), 0);
  EXPECT_NE(short_field.Read(good.substr(0, 11)), 0);
  for (int p = 0; p <= 6; ++p) {
    TimetzCodec codec;
    ASSERT_EQ(codec.Init(p), 0);
    EXPECT_EQ(codec.Read(good) == 0, p == 6);
    // A separate exact one-second vector is valid at every declared p.
    ASSERT_EQ(codec.Read(testing::NumericHex("00000000000f42400000001e")), 0);
  }
}
struct AllocationFailure {
  ArrowBufferAllocator delegate = ArrowBufferAllocatorDefault();
  int calls = 0, fail_at = 1, allocations = 0, frees = 0;
  bool fail = true;
  ArrowBufferAllocator allocator() {
    return {[](ArrowBufferAllocator* self, uint8_t* data, int64_t old_size,
               int64_t size) -> uint8_t* {
              auto* state = static_cast<AllocationFailure*>(self->private_data);
              EXPECT_EQ(data, nullptr);
              EXPECT_EQ(old_size, 0);
              EXPECT_GT(size, 0);
              if (++state->calls == state->fail_at && state->fail) return nullptr;
              auto* value =
                  state->delegate.reallocate(&state->delegate, data, old_size, size);
              if (value) ++state->allocations;
              return value;
            },
            [](ArrowBufferAllocator* self, uint8_t* data, int64_t size) {
              auto* state = static_cast<AllocationFailure*>(self->private_data);
              if (data) ++state->frees;
              state->delegate.free(&state->delegate, data, size);
            },
            this};
  }
};
TEST(RedshiftTimetzTest, FirstNullReservesAreAtomicAndBackfillValidPrefix) {
  for (int fail_at = 1; fail_at <= 3; ++fail_at)
    for (bool retry_null : {false, true}) {
      AllocationFailure failure;
      failure.fail_at = fail_at;
      {
        auto allocator = failure.allocator();
        TimetzCodec codec;
        ASSERT_EQ(codec.Init(6, &allocator), 0);
        // Preexisting generic valid appends leave parent bitmap absent.
        for (int row = 0; row < 2; ++row) {
          ASSERT_EQ(ArrowArrayAppendInt(codec.array->children[0], row * 1000), 0);
          ASSERT_EQ(ArrowArrayAppendInt(codec.array->children[1], row * 19800), 0);
          ASSERT_EQ(ArrowArrayFinishElement(codec.array.get()), 0);
        }
        auto* bitmap = ArrowArrayValidityBitmap(codec.array.get());
        ASSERT_EQ(bitmap->buffer.data, nullptr);
        std::vector<std::string> old;
        for (int child = 0; child < 2; ++child) {
          auto* buffer = ArrowArrayBuffer(codec.array->children[child], 1);
          old.emplace_back(reinterpret_cast<const char*>(buffer->data),
                           buffer->size_bytes);
          buffer->capacity_bytes = buffer->size_bytes;
        }
        EXPECT_EQ(codec.Read("", -1), ENOMEM);
        EXPECT_EQ(codec.array->length, 2);
        EXPECT_EQ(codec.array->null_count, 0);
        EXPECT_EQ(bitmap->buffer.data, nullptr);
        EXPECT_EQ(bitmap->size_bits, 0);
        for (int child = 0; child < 2; ++child) {
          EXPECT_EQ(codec.array->children[child]->length, 2);
          const auto* buffer = ArrowArrayBuffer(codec.array->children[child], 1);
          EXPECT_EQ(std::string(reinterpret_cast<const char*>(buffer->data),
                                buffer->size_bytes),
                    old[child]);
        }
        failure.fail = false;
        ASSERT_EQ(
            codec.Read(retry_null ? "" : testing::NumericHex("00000000000003e90000001e"),
                       retry_null ? -1 : 12),
            0);
        EXPECT_EQ(bitmap->size_bits, 3);
        EXPECT_TRUE(ArrowBitGet(bitmap->buffer.data, 0));
        EXPECT_TRUE(ArrowBitGet(bitmap->buffer.data, 1));
        EXPECT_EQ(ArrowBitGet(bitmap->buffer.data, 2), !retry_null);
        EXPECT_EQ(codec.array->length, 3);
        EXPECT_EQ(codec.array->null_count, retry_null ? 1 : 0);
        EXPECT_EQ(ArrowArrayBuffer(codec.array->children[0], 1)->size_bytes, 24);
        EXPECT_EQ(ArrowArrayBuffer(codec.array->children[1], 1)->size_bytes, 12);
      }
      EXPECT_EQ(failure.allocations, failure.frees);
    }
}
TEST(RedshiftTimetzTest, StagedWireReserveNeverReallocatesOldOwnerOrLosesPrefix) {
  struct OldOwner {
    int frees = 0, reallocations = 0;
  } owner;
  AllocationFailure failure;
  {
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(testing::TimetzWriteSchema(schema.get()), 0);
    nanoarrow::UniqueArray array;
    ASSERT_EQ(
        testing::TimetzWriteBatch(schema.get(), array.get(), {{{1001, -30}}}, nullptr),
        0);
    nanoarrow::UniqueArrayView view;
    ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), nullptr), 0);
    RedshiftTimetzSchema source;
    ASSERT_EQ(RedshiftTimetzSchema::Parse(schema->children[0], true, &source, nullptr),
              0);
    auto writer = PostgresCopyFieldWriter::Create<RedshiftTimetzFieldWriter>(
        view->children[0], source, std::optional<int32_t>{6}, failure.allocator());
    nanoarrow::UniqueBuffer wire;
    ASSERT_EQ(ArrowBufferAppend(wire.get(), "prefix", 6), 0);
    wire->capacity_bytes = wire->size_bytes;
    wire->allocator = {
        [](ArrowBufferAllocator* allocator, uint8_t* data, int64_t, int64_t) -> uint8_t* {
          auto* owner = static_cast<OldOwner*>(allocator->private_data);
          ++owner->reallocations;
          std::free(data);
          return nullptr;
        },
        [](ArrowBufferAllocator* allocator, uint8_t* data, int64_t) {
          auto* owner = static_cast<OldOwner*>(allocator->private_data);
          if (data) ++owner->frees;
          std::free(data);
        },
        &owner};
    const auto* before = wire->data;
    const auto capacity = wire->capacity_bytes;
    EXPECT_EQ(writer->Write(wire.get(), 0, nullptr), ENOMEM);
    EXPECT_EQ(wire->data, before);
    EXPECT_EQ(wire->size_bytes, 6);
    EXPECT_EQ(wire->capacity_bytes, capacity);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(wire->data), 6), "prefix");
    EXPECT_EQ(owner.frees, 0);
    EXPECT_EQ(owner.reallocations, 0);
    failure.fail = false;
    ASSERT_EQ(writer->Write(wire.get(), 0, nullptr), 0);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(wire->data), wire->size_bytes),
              "prefix" + testing::NumericHex("0000000c00000000000003e90000001e"));
    EXPECT_EQ(owner.frees, 1);
    EXPECT_EQ(owner.reallocations, 0);
  }
  EXPECT_EQ(failure.allocations, failure.frees);
}
TEST(RedshiftTimetzTest, IndependentSourceTargetPrecisionAndKnownTargetNullValidation) {
  std::shared_ptr<PostgresTypeResolver> resolver;
  ASSERT_EQ(testing::TimetzResolver(BackendProfile::Redshift(), &resolver), 0);
  for (int source = 0; source <= 6; ++source)
    for (int target = 0; target <= 6; ++target) {
      nanoarrow::UniqueSchema schema;
      const auto p = std::to_string(source);
      ASSERT_EQ(testing::TimetzWriteSchema(schema.get(), p.c_str()), 0);
      nanoarrow::UniqueArray array;
      ASSERT_EQ(testing::TimetzWriteBatch(schema.get(), array.get(), {{{1234000, 30}}},
                                          nullptr),
                0);
      nanoarrow::UniqueArrayView view;
      ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), nullptr), 0);
      ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), nullptr), 0);
      PostgresType actual;
      ASSERT_EQ(resolver->Find(testing::kTimetzOid, &actual, nullptr), 0);
      actual = actual.WithFieldDescriptor(target, BackendProfile::Redshift());
      std::unique_ptr<PostgresCopyFieldWriter> writer;
      ASSERT_EQ(MakeCopyFieldWriter(schema->children[0], view->children[0], *resolver,
                                    &actual, &writer, false, nullptr),
                0);
      nanoarrow::UniqueBuffer bytes;
      EXPECT_EQ(writer->Write(bytes.get(), 0, nullptr) == 0, source >= 3 && target >= 3);
      EXPECT_EQ(bytes->size_bytes, source >= 3 && target >= 3 ? 16 : 0);
      nanoarrow::UniqueArray exact;
      ASSERT_EQ(testing::TimetzWriteBatch(schema.get(), exact.get(), {{{1000000, 30}}},
                                          nullptr),
                0);
      ASSERT_EQ(ArrowArrayViewSetArray(view.get(), exact.get(), nullptr), 0);
      bytes->size_bytes = 0;
      ASSERT_EQ(writer->Write(bytes.get(), 0, nullptr), 0);
      EXPECT_EQ(bytes->size_bytes, 16);
    }
  for (auto raw : {std::optional<int32_t>{}, std::optional<int32_t>{999},
                   std::optional<int32_t>{3}}) {
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_NA);
    nanoarrow::UniqueArrayView view;
    ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), nullptr), 0);
    PostgresType target;
    ASSERT_EQ(resolver->Find(testing::kTimetzOid, &target, nullptr), 0);
    target = target.WithFieldDescriptor(raw, BackendProfile::Redshift());
    std::unique_ptr<PostgresCopyFieldWriter> writer;
    EXPECT_EQ(MakeCopyFieldWriter(schema.get(), view.get(), *resolver, &target, &writer,
                                  false, nullptr) == 0,
              raw == std::optional<int32_t>{3});
  }
}
TEST(RedshiftTimetzTest, IndependentOffsetsNullChildrenAndStructuralBounds) {
  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(testing::TimetzWriteSchema(schema.get()), 0);
  std::vector<testing::TimetzValue> rows;
  for (int i = 0; i < 12; ++i) rows.push_back({{i * 1000, 100 + i}});
  nanoarrow::UniqueArray array;
  ASSERT_EQ(testing::TimetzWriteBatch(schema.get(), array.get(), rows, nullptr), 0);
  nanoarrow::UniqueArrayView view;
  ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), nullptr), 0);
  ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), nullptr), 0);
  auto* parent = view->children[0];
  parent->offset = 1;
  parent->length = 7;
  parent->children[0]->offset = 2;
  parent->children[0]->length = 10;
  parent->children[1]->offset = 3;
  parent->children[1]->length = 9;
  RedshiftTimetzSchema source;
  ASSERT_EQ(RedshiftTimetzSchema::Parse(schema->children[0], true, &source, nullptr), 0);
  int64_t civil;
  int32_t east;
  ASSERT_EQ(source.Value(parent, 2, &civil, &east, nullptr), 0);
  EXPECT_EQ(civil, 5000);
  EXPECT_EQ(east, 106);
  auto* child = parent->children[0];
  uint8_t validity[2] = {0xff, 0xff};
  ArrowBitClear(validity, 5);
  child->buffer_views[0].data.as_uint8 = validity;
  child->buffer_views[0].size_bytes = 2;
  EXPECT_NE(source.Value(parent, 2, &civil, &east, nullptr), 0);
  child->buffer_views[0].data.data = nullptr;
  child->buffer_views[0].size_bytes = 0;
  const auto bytes = child->buffer_views[1].size_bytes;
  child->buffer_views[1].size_bytes = 5 * 8;
  EXPECT_NE(source.Value(parent, 2, &civil, &east, nullptr), 0);
  child->buffer_views[1].size_bytes = bytes;
  child->offset = INT64_MAX;
  EXPECT_NE(source.Value(parent, 2, &civil, &east, nullptr), 0);
  child->offset = 2;
  parent->offset = INT64_MAX;
  EXPECT_NE(source.Value(parent, 2, &civil, &east, nullptr), 0);
  parent->offset = 1;
  parent->buffer_views[0].data.as_uint8 = validity;
  parent->buffer_views[0].size_bytes = 0;
  EXPECT_NE(source.Value(parent, 2, &civil, &east, nullptr), 0);
}
TEST(RedshiftTimetzTest, UnalignedWireAndMalformedOutputDoNotMutateLogicalState) {
  auto wire = std::string("x") + testing::NumericHex("00000000000003e90000001e");
  TimetzCodec codec;
  ASSERT_EQ(codec.Init(), 0);
  ArrowBufferView input{};
  input.data.as_char = wire.data() + 1;
  input.size_bytes = 12;
  ASSERT_EQ(codec.reader->Read(&input, 12, codec.array.get(), nullptr), 0);
  EXPECT_EQ(input.size_bytes, 0);
  EXPECT_EQ(codec.array->length, 1);
  auto* bitmap = ArrowArrayValidityBitmap(codec.array.get());
  const auto size_bits = bitmap->size_bits;
  bitmap->size_bits = 2;
  input.data.as_char = wire.data() + 1;
  input.size_bytes = 12;
  EXPECT_NE(codec.reader->Read(&input, 12, codec.array.get(), nullptr), 0);
  EXPECT_EQ(input.data.as_char, wire.data() + 1);
  EXPECT_EQ(input.size_bytes, 12);
  EXPECT_EQ(codec.array->length, 1);
  EXPECT_EQ(codec.array->children[0]->length, 1);
  bitmap->size_bits = size_bits;
  input.data.data = nullptr;
  input.size_bytes = 12;
  EXPECT_NE(codec.reader->Read(&input, 12, codec.array.get(), nullptr), 0);
  EXPECT_EQ(codec.array->length, 1);
  nanoarrow::UniqueBuffer buffer;
  EXPECT_NE(RedshiftTimetzSchema::Reserve(buffer.get(), -1, nullptr), 0);
  buffer->size_bytes = INT64_MAX;
  buffer->capacity_bytes = INT64_MAX;
  EXPECT_NE(RedshiftTimetzSchema::Reserve(buffer.get(), 1, nullptr), 0);
  buffer->size_bytes = 0;
  buffer->capacity_bytes = 0;
}
TEST(RedshiftTimetzTest,
     ExistingBitmapGrowthFailurePreservesPrefixAndInitializesNewTail) {
  for (int fail_at = 1; fail_at <= 3; ++fail_at) {
    AllocationFailure failure;
    failure.fail = false;
    {
      auto allocation = failure.allocator();
      TimetzCodec codec;
      ASSERT_EQ(codec.Init(6, &allocation), 0);
      for (int row = 0; row < 7; ++row)
        ASSERT_EQ(codec.Read(testing::NumericHex("00000000000003e90000001e")), 0);
      ASSERT_EQ(codec.Read("", -1), 0);
      auto* bitmap = ArrowArrayValidityBitmap(codec.array.get());
      ASSERT_EQ(bitmap->size_bits, 8);
      ASSERT_EQ(bitmap->buffer.size_bytes, 1);
      EXPECT_EQ(bitmap->buffer.data[0], 0x7f);
      bitmap->buffer.capacity_bytes = bitmap->buffer.size_bytes;
      for (int child = 0; child < 2; ++child) {
        auto* buffer = ArrowArrayBuffer(codec.array->children[child], 1);
        buffer->capacity_bytes = buffer->size_bytes;
      }
      failure.calls = 0;
      failure.fail_at = fail_at;
      failure.fail = true;
      EXPECT_EQ(codec.Read("", -1), ENOMEM);
      EXPECT_EQ(codec.array->length, 8);
      EXPECT_EQ(codec.array->null_count, 1);
      EXPECT_EQ(bitmap->size_bits, 8);
      EXPECT_EQ(bitmap->buffer.size_bytes, 1);
      EXPECT_EQ(bitmap->buffer.data[0], 0x7f);
      EXPECT_EQ(codec.array->children[0]->length, 8);
      EXPECT_EQ(codec.array->children[1]->length, 8);
      failure.fail = false;
      ASSERT_EQ(codec.Read(testing::NumericHex("00000000000003e90000001e")), 0);
      EXPECT_EQ(bitmap->size_bits, 9);
      EXPECT_EQ(bitmap->buffer.size_bytes, 2);
      EXPECT_EQ(bitmap->buffer.data[0], 0x7f);
      EXPECT_EQ(bitmap->buffer.data[1], 1);
      EXPECT_EQ(codec.array->length, 9);
      EXPECT_EQ(codec.array->null_count, 1);
    }
    EXPECT_EQ(failure.allocations, failure.frees);
  }
}
}  // namespace
}  // namespace adbcpq
