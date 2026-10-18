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

#include "copy/writer.h"
#include "redshift_interval_test_fixture.h"
#include "redshift_numeric_test_fixture.h"
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

// Borrow real consumer-exported C Data, run the production parameter writer, and
// return its field bytes through C Data. Target p/s == 0 means an OID-only SQL
// parameter; positive p/s describe a synthetic catalog fixture, never a live DB.
extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftDecimalWriteFixture(
    ArrowArray* input, ArrowSchema* input_schema, int precision, int scale,
    ArrowArray* array, ArrowSchema* schema) {
  if (!input || !input_schema || !array || !schema || precision < 0 || precision > 38 ||
      scale < 0 || scale > 37 || scale > precision)
    return EINVAL;
  ArrowError error{};
  nanoarrow::UniqueArrayView view;
  NANOARROW_RETURN_NOT_OK(ArrowArrayViewInitFromSchema(view.get(), input_schema, &error));
  if (view->storage_type != NANOARROW_TYPE_STRUCT || view->n_children != 1) return EINVAL;
  NANOARROW_RETURN_NOT_OK(ArrowArrayViewSetArray(view.get(), input, &error));
  const auto profile = adbc::driver::pgwire::BackendProfile::Redshift();
  std::shared_ptr<adbcpq::PostgresTypeResolver> resolver;
  NANOARROW_RETURN_NOT_OK(adbcpq::testing::ModifierResolver(profile, &resolver));
  adbcpq::PostgresType target;
  if (precision) {
    NANOARROW_RETURN_NOT_OK(
        resolver->Find(adbcpq::testing::kNumericOid, &target, &error));
    target = target.WithFieldDescriptor(4 + (precision << 16) + scale, profile);
  }
  std::unique_ptr<adbcpq::PostgresCopyFieldWriter> writer;
  NANOARROW_RETURN_NOT_OK(
      adbcpq::MakeCopyFieldWriter(input_schema->children[0], view->children[0], *resolver,
                                  precision ? &target : nullptr, &writer, false, &error));
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
  for (int64_t row = 0; row < input->length; row++) {
    if (ArrowArrayViewIsNull(view->children[0], row)) {
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(output->children[0], 1));
    } else {
      nanoarrow::UniqueBuffer bytes;
      NANOARROW_RETURN_NOT_OK(writer->Write(bytes.get(), row, &error));
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
