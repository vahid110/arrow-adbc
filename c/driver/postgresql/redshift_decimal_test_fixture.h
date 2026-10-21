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

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nanoarrow/nanoarrow.hpp>

namespace adbcpq::testing {
inline ArrowErrorCode DecimalMetadata(ArrowSchema* field, const char* key,
                                      const char* value) {
  nanoarrow::UniqueBuffer buffer;
  NANOARROW_RETURN_NOT_OK(ArrowMetadataBuilderInit(buffer.get(), field->metadata));
  NANOARROW_RETURN_NOT_OK(
      ArrowMetadataBuilderAppend(buffer.get(), ArrowCharView(key), ArrowCharView(value)));
  return ArrowSchemaSetMetadata(field, reinterpret_cast<char*>(buffer->data));
}

inline ArrowErrorCode DecimalWriteSchema(ArrowSchema* schema, ArrowType storage,
                                         int precision, int scale, bool marked = true,
                                         bool text_modifiers = false) {
  ArrowSchemaInit(schema);
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(schema, 1));
  ArrowSchema* field = schema->children[0];
  if (storage == NANOARROW_TYPE_DECIMAL128 || storage == NANOARROW_TYPE_DECIMAL256) {
    NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeDecimal(field, storage, precision, scale));
  } else {
    NANOARROW_RETURN_NOT_OK(ArrowSchemaSetType(field, storage));
  }
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(field, "amount"));
  if (marked) {
    NANOARROW_RETURN_NOT_OK(DecimalMetadata(field, "ADBC:redshift:sql_type", "DECIMAL"));
  }
  if (text_modifiers) {
    NANOARROW_RETURN_NOT_OK(DecimalMetadata(field, "ADBC:redshift:precision",
                                            std::to_string(precision).c_str()));
    NANOARROW_RETURN_NOT_OK(
        DecimalMetadata(field, "ADBC:redshift:scale", std::to_string(scale).c_str()));
  }
  return NANOARROW_OK;
}

// Decimal inputs are literal integer coefficients; text inputs are decimal text.
// ArrowDecimalSetDigits deliberately does not validate declared precision, so
// callers can test raw out-of-schema coefficients through public binding.
inline ArrowErrorCode DecimalWriteBatch(
    ArrowSchema* schema, ArrowArray* array,
    const std::vector<std::optional<std::string>>& values, ArrowError* error) {
  ArrowSchemaView view;
  NANOARROW_RETURN_NOT_OK(ArrowSchemaViewInit(&view, schema->children[0], error));
  NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromSchema(array, schema, error));
  NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(array));
  for (const auto& input : values) {
    if (!input || view.type == NANOARROW_TYPE_NA) {
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(array->children[0], 1));
    } else if (view.type == NANOARROW_TYPE_STRING ||
               view.type == NANOARROW_TYPE_LARGE_STRING) {
      NANOARROW_RETURN_NOT_OK(
          ArrowArrayAppendString(array->children[0], ArrowCharView(input->c_str())));
    } else {
      ArrowDecimal value;
      ArrowDecimalInit(&value, view.type == NANOARROW_TYPE_DECIMAL128 ? 128 : 256,
                       view.decimal_precision, view.decimal_scale);
      NANOARROW_RETURN_NOT_OK(
          ArrowDecimalSetDigits(&value, ArrowCharView(input->c_str())));
      NANOARROW_RETURN_NOT_OK(ArrowArrayAppendDecimal(array->children[0], &value));
    }
    NANOARROW_RETURN_NOT_OK(ArrowArrayFinishElement(array));
  }
  return ArrowArrayFinishBuildingDefault(array, error);
}
}  // namespace adbcpq::testing

namespace adbcpq::testing {

// The dictionary value, not the index field, carries the semantic marker.
inline ArrowErrorCode DecimalDictionarySchema(ArrowSchema* schema,
                                              ArrowType storage = NANOARROW_TYPE_STRING,
                                              bool marked = true) {
  ArrowSchemaInit(schema);
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(schema, 1));
  ArrowSchema* field = schema->children[0];
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetType(field, NANOARROW_TYPE_INT32));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(field, "amount"));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaAllocateDictionary(field));
  ArrowSchemaInit(field->dictionary);
  if (storage == NANOARROW_TYPE_DECIMAL128 || storage == NANOARROW_TYPE_DECIMAL256) {
    NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeDecimal(field->dictionary, storage, 10, 2));
  } else {
    NANOARROW_RETURN_NOT_OK(ArrowSchemaSetType(field->dictionary, storage));
  }
  if (marked) {
    NANOARROW_RETURN_NOT_OK(
        DecimalMetadata(field->dictionary, "ADBC:redshift:sql_type", "DECIMAL"));
  }
  return NANOARROW_OK;
}

inline ArrowErrorCode DecimalDictionaryBatch(ArrowSchema* schema, ArrowArray* array,
                                             ArrowStringView value, ArrowError* error) {
  NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromSchema(array, schema, error));
  NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(array));
  NANOARROW_RETURN_NOT_OK(ArrowArrayAppendString(array->children[0]->dictionary, value));
  NANOARROW_RETURN_NOT_OK(ArrowArrayAppendInt(array->children[0], 0));
  NANOARROW_RETURN_NOT_OK(ArrowArrayFinishElement(array));
  return ArrowArrayFinishBuildingDefault(array, error);
}

// Keep the original producer's private_data and release callback intact while
// recording exact ownership. Safe even when a regression unexpectedly accepts a bind.
template <typename T>
inline void DecimalCountReleases(T* value, std::shared_ptr<int> count) {
  struct Probe {
    T original;
    std::shared_ptr<int> count;
  };
  auto* probe = new Probe{*value, count};
  value->private_data = probe;
  value->release = [](T* value) {
    std::unique_ptr<Probe> probe(static_cast<Probe*>(value->private_data));
    probe->original.release(&probe->original);
    ++*probe->count;
    value->release = nullptr;
  };
}
}  // namespace adbcpq::testing
