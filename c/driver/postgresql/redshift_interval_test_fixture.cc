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

// Test-only C Data producer, never linked into or installed with a driver artifact.
#include <cstdint>
#include <memory>

#include "copy/reader.h"
#include "copy/writer.h"
#include "redshift_bytes_test_fixture.h"
#include "redshift_interval_test_fixture.h"
#include "redshift_interval_write_test_fixture.h"
#include "redshift_numeric_test_fixture.h"
#include "redshift_scalar_test_fixture.h"
#include "redshift_super_test_fixture.h"
#include "redshift_temporal_test_fixture.h"
#include "redshift_timetz_test_fixture.h"
#include "type_modifier_test_fixture.h"

#if defined(_WIN32)
#define INTERVAL_FIXTURE_EXPORT __declspec(dllexport)
#else
#define INTERVAL_FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftIntervalFixture(int empty,
                                                                   ArrowArray* array,
                                                                   ArrowSchema* schema) {
  ArrowError error{};
  return adbcpq::testing::IntervalFixture(empty, array, schema, &error);
}

extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftModifierFixture(int empty,
                                                                   ArrowArray* array,
                                                                   ArrowSchema* schema) {
  ArrowError error{};
  return adbcpq::testing::ModifierFixture(empty, array, schema, &error);
}

extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftDecimalFixture(int batch,
                                                                  ArrowArray* array,
                                                                  ArrowSchema* schema) {
  ArrowError error{};
  return adbcpq::testing::DecimalFixture(batch, array, schema, &error);
}

namespace {
int ExportParameterWire(ArrowArray* input, ArrowSchema* input_schema,
                        const adbcpq::PostgresTypeResolver& resolver,
                        const adbcpq::PostgresType* target, ArrowArray* array,
                        ArrowSchema* schema) {
  if (!input || !input_schema || !array || !schema) return EINVAL;
  ArrowError error{};
  nanoarrow::UniqueArrayView view;
  NANOARROW_RETURN_NOT_OK(ArrowArrayViewInitFromSchema(view.get(), input_schema, &error));
  if (view->storage_type != NANOARROW_TYPE_STRUCT || view->n_children != 1) return EINVAL;
  NANOARROW_RETURN_NOT_OK(ArrowArrayViewSetArray(view.get(), input, &error));
  std::unique_ptr<adbcpq::PostgresCopyFieldWriter> writer;
  NANOARROW_RETURN_NOT_OK(adbcpq::MakeCopyFieldWriter(input_schema->children[0],
                                                      view->children[0], resolver, target,
                                                      &writer, false, &error));
  nanoarrow::UniqueSchema output_schema;
  ArrowSchemaInit(output_schema.get());
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(output_schema.get(), 1));
  NANOARROW_RETURN_NOT_OK(
      ArrowSchemaSetType(output_schema->children[0], NANOARROW_TYPE_BINARY));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(output_schema->children[0], "wire"));
  nanoarrow::UniqueArray output;
  NANOARROW_RETURN_NOT_OK(
      ArrowArrayInitFromSchema(output.get(), output_schema.get(), &error));
  NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(output.get()));
  adbcpq::RedshiftTimetzSchema timetz;
  NANOARROW_RETURN_NOT_OK(adbcpq::RedshiftTimetzSchema::Parse(input_schema->children[0],
                                                              true, &timetz, &error));
  for (int64_t row = 0; row < input->length; row++) {
    if (timetz.enabled && (view->offset < 0 || view->offset > INT64_MAX - row))
      return EINVAL;
    const int64_t field_row = timetz.enabled ? view->offset + row : row;
    if (ArrowArrayViewIsNull(view->children[0], field_row)) {
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(output->children[0], 1));
    } else {
      nanoarrow::UniqueBuffer bytes;
      NANOARROW_RETURN_NOT_OK(writer->Write(bytes.get(), field_row, &error));
      ArrowBufferView value{};
      value.data.data = bytes->data;
      value.size_bytes = bytes->size_bytes;
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendBytes(output->children[0], value));
    }
    NANOARROW_RETURN_NOT_OK(ArrowArrayFinishElement(output.get()));
  }
  NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(output.get(), &error));
  ArrowArrayMove(output.get(), array);
  ArrowSchemaMove(output_schema.get(), schema);
  return NANOARROW_OK;
}
}  // namespace

// Borrow consumer-exported C Data, execute the production parameter writer,
// and export actual field bytes. These are synthetic target/catalog fixtures.
extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftDecimalWriteFixture(
    ArrowArray* input, ArrowSchema* input_schema, int precision, int scale,
    ArrowArray* array, ArrowSchema* schema) {
  if (precision < 0 || precision > 38 || scale < 0 || scale > 37 || scale > precision)
    return EINVAL;
  ArrowError error{};
  const auto profile = adbc::driver::pgwire::BackendProfile::Redshift();
  std::shared_ptr<adbcpq::PostgresTypeResolver> resolver;
  NANOARROW_RETURN_NOT_OK(adbcpq::testing::ModifierResolver(profile, &resolver));
  adbcpq::PostgresType target;
  if (precision) {
    NANOARROW_RETURN_NOT_OK(
        resolver->Find(adbcpq::testing::kNumericOid, &target, &error));
    target = target.WithFieldDescriptor(4 + (precision << 16) + scale, profile);
  }
  return ExportParameterWire(input, input_schema, *resolver,
                             precision ? &target : nullptr, array, schema);
}

extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftScalarWriteFixture(
    ArrowArray* input, ArrowSchema* input_schema, int target_kind, ArrowArray* array,
    ArrowSchema* schema) {
  if (target_kind < 0 || target_kind > 6) return EINVAL;
  ArrowError error{};
  const auto profile = adbc::driver::pgwire::BackendProfile::Redshift();
  std::shared_ptr<adbcpq::PostgresTypeResolver> resolver;
  NANOARROW_RETURN_NOT_OK(adbcpq::testing::ScalarResolver(profile, &resolver));
  adbcpq::PostgresType target;
  if (target_kind) {
    NANOARROW_RETURN_NOT_OK(
        resolver->Find(adbcpq::testing::kScalarOids[target_kind - 1], &target, &error));
    target = target.WithFieldDescriptor(-1, profile);
  }
  return ExportParameterWire(input, input_schema, *resolver,
                             target_kind ? &target : nullptr, array, schema);
}

extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftScalarFixture(int batch,
                                                                 ArrowArray* array,
                                                                 ArrowSchema* schema) {
  ArrowError error{};
  return adbcpq::testing::ScalarFixture(batch, array, schema, &error);
}

extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftBytesFixture(int batch,
                                                                ArrowArray* array,
                                                                ArrowSchema* schema) {
  ArrowError error{};
  return adbcpq::testing::BytesFixture(batch, array, schema, &error);
}

extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftBytesWriteFixture(
    ArrowArray* input, ArrowSchema* input_schema, int target_kind, int target_width,
    ArrowArray* array, ArrowSchema* schema) {
  if (target_kind < 0 || target_kind > 3 || target_width < -1 || target_width > 16777216)
    return EINVAL;
  ArrowError error{};
  const auto profile = adbc::driver::pgwire::BackendProfile::Redshift();
  std::shared_ptr<adbcpq::PostgresTypeResolver> resolver;
  NANOARROW_RETURN_NOT_OK(adbcpq::testing::BytesResolver(profile, &resolver));
  adbcpq::PostgresType target;
  if (target_kind) {
    NANOARROW_RETURN_NOT_OK(
        resolver->Find(adbcpq::testing::kBytesOids[target_kind - 1], &target, &error));
    target =
        target.WithFieldDescriptor(target_width < 0 ? -1 : target_width + 4, profile);
  }
  return ExportParameterWire(input, input_schema, *resolver,
                             target_kind ? &target : nullptr, array, schema);
}

extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftTemporalFixture(int batch,
                                                                   ArrowArray* array,
                                                                   ArrowSchema* schema) {
  ArrowError error{};
  return adbcpq::testing::TemporalFixture(batch, array, schema, &error);
}
extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftTemporalWriteFixture(
    ArrowArray* input, ArrowSchema* input_schema, int target_kind, int precision,
    ArrowArray* array, ArrowSchema* schema) {
  if (target_kind < 0 || target_kind > 4 || precision < -1 || precision > 6)
    return EINVAL;
  ArrowError error{};
  const auto profile = adbc::driver::pgwire::BackendProfile::Redshift();
  std::shared_ptr<adbcpq::PostgresTypeResolver> resolver;
  NANOARROW_RETURN_NOT_OK(adbcpq::testing::TemporalResolver(profile, &resolver));
  adbcpq::PostgresType target;
  if (target_kind) {
    NANOARROW_RETURN_NOT_OK(
        resolver->Find(adbcpq::testing::kTemporalOids[target_kind - 1], &target, &error));
    target = target.WithFieldDescriptor(precision, profile);
  }
  return ExportParameterWire(input, input_schema, *resolver,
                             target_kind ? &target : nullptr, array, schema);
}

// Explicit internal reference producer: selector is not a driver option and
// cannot qualify native catalog facts. Both exports are excluded from drivers.
extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftIntervalQualifiedFixture(
    int qualifier, int precision, int batch, ArrowArray* array, ArrowSchema* schema) {
  if (qualifier < 0 || qualifier >= 13 || precision < 0 || precision > 6 || batch < 0 ||
      batch > 2)
    return EINVAL;
  using adbcpq::MakeCopyFieldReader;
  using adbcpq::PostgresCopyFieldReader;
  using adbcpq::PostgresType;
  using adbcpq::PostgresTypeId;
  using adbcpq::PostgresTypeResolver;
  using adbcpq::RedshiftIntervalConstraint;
  using adbcpq::RedshiftIntervalFamily;
  using adbcpq::RedshiftIntervalQualifier;
  const bool months = qualifier < 3;
  const auto q = static_cast<RedshiftIntervalQualifier>(qualifier + 1);
  auto constraint = adbcpq::testing::IntervalReferenceConstraint(
      months ? RedshiftIntervalFamily::kYearMonth : RedshiftIntervalFamily::kDaySecond, q,
      RedshiftIntervalConstraint::HasSecond(q) ? std::optional<int32_t>{precision}
                                               : std::nullopt);
  auto type = PostgresType(months ? PostgresTypeId::kRedshiftIntervalYearMonth
                                  : PostgresTypeId::kRedshiftIntervalDaySecond)
                  .WithIntervalConstraint(constraint);
  nanoarrow::UniqueSchema output;
  ArrowSchemaInit(output.get());
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(output.get(), 1));
  NANOARROW_RETURN_NOT_OK(type.SetSchema(output->children[0], "Redshift"));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(output->children[0], "amount"));
  nanoarrow::UniqueArray values;
  NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromSchema(values.get(), output.get(), nullptr));
  NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(values.get()));
  std::unique_ptr<PostgresCopyFieldReader> reader;
  NANOARROW_RETURN_NOT_OK(
      MakeCopyFieldReader(type, output->children[0], &reader, nullptr));
  reader->Init(type);
  NANOARROW_RETURN_NOT_OK(reader->InitSchema(output->children[0]));
  NANOARROW_RETURN_NOT_OK(reader->InitArray(values->children[0]));
  // Independent fixture quantum table, not the production qualifier function.
  const int64_t quanta[] = {12,
                            1,
                            1,
                            86400000000LL,
                            3600000000LL,
                            60000000LL,
                            1,
                            3600000000LL,
                            60000000LL,
                            1,
                            60000000LL,
                            1,
                            1};
  int64_t quantum = quanta[qualifier];
  if (RedshiftIntervalConstraint::HasSecond(q))
    for (int p = precision; p < 6; ++p) quantum *= 10;
  const int rows = batch == 0 ? 4 : batch == 1 ? 0 : 1;
  for (int row = 0; row < rows; ++row) {
    const bool null = batch == 2 || row == 3;
    const int64_t value = row == 0 ? 0 : row == 1 ? quantum : -quantum;
    const uint64_t bits = static_cast<uint64_t>(value);
    uint8_t wire[8]{};
    const int width = months ? 4 : 8;
    for (int byte = 0; byte < width; ++byte)
      wire[width - 1 - byte] = static_cast<uint8_t>(bits >> (byte * 8));
    ArrowBufferView data{};
    data.data.as_uint8 = wire;
    data.size_bytes = null ? -1 : width;
    NANOARROW_RETURN_NOT_OK(
        reader->Read(&data, null ? -1 : width, values->children[0], nullptr));
    NANOARROW_RETURN_NOT_OK(ArrowArrayFinishElement(values.get()));
  }
  NANOARROW_RETURN_NOT_OK(reader->FinishArray(values->children[0], nullptr));
  NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(values.get(), nullptr));
  ArrowArrayMove(values.get(), array);
  ArrowSchemaMove(output.get(), schema);
  return NANOARROW_OK;
}
extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftIntervalWriteFixture(
    ArrowArray* input, ArrowSchema* input_schema, int qualifier, int precision,
    ArrowArray* array, ArrowSchema* schema) {
  if (qualifier < -1 || qualifier >= 13 || precision < 0 || precision > 6) return EINVAL;
  using adbcpq::MakeCopyFieldReader;
  using adbcpq::PostgresCopyFieldReader;
  using adbcpq::PostgresType;
  using adbcpq::PostgresTypeId;
  using adbcpq::PostgresTypeResolver;
  using adbcpq::RedshiftIntervalConstraint;
  using adbcpq::RedshiftIntervalFamily;
  using adbcpq::RedshiftIntervalQualifier;
  std::shared_ptr<PostgresTypeResolver> resolver;
  NANOARROW_RETURN_NOT_OK(adbcpq::testing::FixtureResolver(
      adbc::driver::pgwire::BackendProfile::Redshift(), &resolver));
  PostgresType target;
  if (qualifier >= 0) {
    const bool months = qualifier < 3;
    NANOARROW_RETURN_NOT_OK(resolver->Find(
        months ? adbcpq::testing::kYearMonthOid : adbcpq::testing::kDaySecondOid, &target,
        nullptr));
    const auto q = static_cast<RedshiftIntervalQualifier>(qualifier + 1);
    target = target.WithIntervalConstraint(adbcpq::testing::IntervalReferenceConstraint(
        months ? RedshiftIntervalFamily::kYearMonth : RedshiftIntervalFamily::kDaySecond,
        q,
        RedshiftIntervalConstraint::HasSecond(q) ? std::optional<int32_t>{precision}
                                                 : std::nullopt));
  }
  return ExportParameterWire(input, input_schema, *resolver,
                             qualifier >= 0 ? &target : nullptr, array, schema);
}

extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftTimetzFixture(int batch,
                                                                 ArrowArray* array,
                                                                 ArrowSchema* schema) {
  ArrowError error{};
  return adbcpq::testing::TimetzFixture(batch, array, schema, &error);
}
// Internal labeled target selector: -2 source-only, -1 missing target modifier,
// 0..6 known reference target,999 unsupported target. Not an ADBC driver option.
extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftTimetzWriteFixture(
    ArrowArray* input, ArrowSchema* input_schema, int target_raw, ArrowArray* array,
    ArrowSchema* schema) {
  if (target_raw < -2 || (target_raw > 6 && target_raw != 999)) return EINVAL;
  std::shared_ptr<adbcpq::PostgresTypeResolver> resolver;
  NANOARROW_RETURN_NOT_OK(adbcpq::testing::TimetzResolver(
      adbc::driver::pgwire::BackendProfile::Redshift(), &resolver));
  adbcpq::PostgresType target;
  if (target_raw != -2) {
    NANOARROW_RETURN_NOT_OK(
        resolver->Find(adbcpq::testing::kTimetzOid, &target, nullptr));
    target = target.WithFieldDescriptor(
        target_raw == -1 ? std::nullopt : std::optional<int32_t>{target_raw},
        adbc::driver::pgwire::BackendProfile::Redshift());
  }
  return ExportParameterWire(input, input_schema, *resolver,
                             target_raw != -2 ? &target : nullptr, array, schema);
}

// Consumer-owned C Data -> production generated-ingest writer. No SQL or backend
// override.
extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftSuperWriteFixture(
    ArrowArray* input, ArrowSchema* input_schema, ArrowArray* array,
    ArrowSchema* schema) {
  if (!input || !input_schema || !array || !schema) return EINVAL;
  ArrowError error{};
  adbcpq::PostgresTypeResolver resolver(adbc::driver::pgwire::BackendProfile::Redshift());
  NANOARROW_RETURN_NOT_OK(
      resolver.Insert({72111, "super", "fixture_only", 0, 0, 0}, &error));
  adbcpq::PostgresType target;
  NANOARROW_RETURN_NOT_OK(resolver.Find(72111, &target, &error));
  nanoarrow::UniqueArrayView view;
  NANOARROW_RETURN_NOT_OK(ArrowArrayViewInitFromSchema(view.get(), input_schema, &error));
  if (view->storage_type != NANOARROW_TYPE_STRUCT || view->n_children != 1) return EINVAL;
  NANOARROW_RETURN_NOT_OK(ArrowArrayViewSetArray(view.get(), input, &error));
  std::unique_ptr<adbcpq::PostgresCopyFieldWriter> writer;
  NANOARROW_RETURN_NOT_OK(
      adbcpq::MakeCopyFieldWriter(input_schema->children[0], view->children[0], resolver,
                                  &target, &writer, false, &error, true));
  nanoarrow::UniqueSchema output_schema;
  ArrowSchemaInit(output_schema.get());
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(output_schema.get(), 1));
  NANOARROW_RETURN_NOT_OK(
      ArrowSchemaSetType(output_schema->children[0], NANOARROW_TYPE_BINARY));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(output_schema->children[0], "wire"));
  nanoarrow::UniqueArray output;
  NANOARROW_RETURN_NOT_OK(
      ArrowArrayInitFromSchema(output.get(), output_schema.get(), &error));
  NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(output.get()));
  for (int64_t row = 0; row < input->length; ++row) {
    if (view->offset < 0 || row > INT64_MAX - view->offset) return EINVAL;
    if (ArrowArrayViewIsNull(view.get(), row)) return EINVAL;
    const int64_t child_row = row + view->offset;
    if (child_row >= view->children[0]->length) return EINVAL;
    if (ArrowArrayViewIsNull(view->children[0], child_row)) {
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(output->children[0], 1));
    } else {
      nanoarrow::UniqueBuffer bytes;
      NANOARROW_RETURN_NOT_OK(writer->Write(bytes.get(), child_row, &error));
      ArrowBufferView value{};
      value.data.data = bytes->data;
      value.size_bytes = bytes->size_bytes;
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendBytes(output->children[0], value));
    }
    NANOARROW_RETURN_NOT_OK(ArrowArrayFinishElement(output.get()));
  }
  NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(output.get(), &error));
  ArrowArrayMove(output.get(), array);
  ArrowSchemaMove(output_schema.get(), schema);
  return NANOARROW_OK;
}

// Test-only constructed catalog and native payload path. No server roundtrip or
// native-origin assertion: consumer input exercises actual writer and reader.
extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftEncodedFixture(
    ArrowArray* input, ArrowSchema* input_schema, ArrowArray* array,
    ArrowSchema* schema) {
  if (!input || !input_schema || !array || !schema) return EINVAL;
  ArrowError error{};
  adbcpq::RedshiftEncodedSchema encoded;
  if (input_schema->n_children != 1) return EINVAL;
  NANOARROW_RETURN_NOT_OK(adbcpq::RedshiftEncodedSchema::Parse(input_schema->children[0],
                                                               true, &encoded, &error));
  if (!encoded.enabled()) return EINVAL;
  const bool hll = encoded.kind == adbcpq::RedshiftEncodedKind::kHll;
  const char* name = hll ? "hllsketch"
                     : encoded.kind == adbcpq::RedshiftEncodedKind::kGeometry
                         ? "geometry"
                         : "geography";
  adbcpq::PostgresTypeResolver resolver(adbc::driver::pgwire::BackendProfile::Redshift());
  NANOARROW_RETURN_NOT_OK(
      resolver.Insert({72123, name, "fixture_only", 0, 0, 0}, &error));
  adbcpq::PostgresType target;
  NANOARROW_RETURN_NOT_OK(resolver.Find(72123, &target, &error));
  target = target.WithFieldDescriptor(-1, resolver.backend_profile());
  nanoarrow::UniqueArrayView view;
  NANOARROW_RETURN_NOT_OK(ArrowArrayViewInitFromSchema(view.get(), input_schema, &error));
  NANOARROW_RETURN_NOT_OK(ArrowArrayViewSetArray(view.get(), input, &error));
  std::unique_ptr<adbcpq::PostgresCopyFieldWriter> writer;
  NANOARROW_RETURN_NOT_OK(
      adbcpq::MakeCopyFieldWriter(input_schema->children[0], view->children[0], resolver,
                                  &target, &writer, false, &error, true));
  // A constructed RowDescription exercises the same output resolver as public
  // reads. GEOMETRY's3999 output identity differs from its cached catalog OID;
  // the writer above must continue using that unchanged catalog identity.
  adbcpq::PqResultHelper result(nullptr, "");
  auto* raw = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  PGresAttDesc attr{};
  attr.name = const_cast<char*>("value");
  attr.typid = encoded.kind == adbcpq::RedshiftEncodedKind::kGeometry ? 3999 : 72123;
  attr.format = hll ? 0 : 1;
  attr.atttypmod = -1;
  result.SetResult(raw);
  if (!PQsetResultAttrs(raw, 1, &attr)) return ENOMEM;
  adbcpq::PostgresType result_type;
  if (!result.ResolveOutputTypes(resolver, &result_type).ok()) return EINVAL;
  const auto& read_type = result_type.child(0);
  nanoarrow::UniqueSchema output_schema;
  ArrowSchemaInit(output_schema.get());
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(output_schema.get(), 2));
  NANOARROW_RETURN_NOT_OK(
      ArrowSchemaSetType(output_schema->children[0], NANOARROW_TYPE_BINARY));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(output_schema->children[0], "wire"));
  NANOARROW_RETURN_NOT_OK(read_type.SetSchema(output_schema->children[1], "Redshift"));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(output_schema->children[1], "value"));
  std::unique_ptr<adbcpq::PostgresCopyFieldReader> reader;
  NANOARROW_RETURN_NOT_OK(adbcpq::MakeCopyFieldReader(
      read_type, output_schema->children[1], &reader, &error, hll ? 0 : 1));
  NANOARROW_RETURN_NOT_OK(reader->InitSchema(output_schema->children[1]));
  nanoarrow::UniqueArray output;
  NANOARROW_RETURN_NOT_OK(
      ArrowArrayInitFromSchema(output.get(), output_schema.get(), &error));
  NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(output.get()));
  NANOARROW_RETURN_NOT_OK(reader->InitArray(output->children[1]));
  for (int64_t row = 0; row < input->length; ++row) {
    if (ArrowArrayViewIsNull(view.get(), row) || view->offset < 0 ||
        row > INT64_MAX - view->offset)
      return EINVAL;
    const int64_t child = row + view->offset;
    if (child >= view->children[0]->length) return EINVAL;
    if (ArrowArrayViewIsNull(view->children[0], child)) {
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(output->children[0], 1));
      ArrowBufferView empty{};
      NANOARROW_RETURN_NOT_OK(reader->Read(&empty, -1, output->children[1], &error));
    } else {
      nanoarrow::UniqueBuffer wire;
      NANOARROW_RETURN_NOT_OK(writer->Write(wire.get(), child, &error));
      ArrowBufferView bytes{};
      bytes.data.data = wire->data;
      bytes.size_bytes = wire->size_bytes;
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendBytes(output->children[0], bytes));
      NANOARROW_RETURN_NOT_OK(adbcpq::RedshiftBytesSchema::GetBytes(
          view->children[0], child, &bytes, &error));
      NANOARROW_RETURN_NOT_OK(
          reader->Read(&bytes, bytes.size_bytes, output->children[1], &error));
    }
    NANOARROW_RETURN_NOT_OK(ArrowArrayFinishElement(output.get()));
  }
  NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(output.get(), &error));
  ArrowArrayMove(output.get(), array);
  ArrowSchemaMove(output_schema.get(), schema);
  return NANOARROW_OK;
}
