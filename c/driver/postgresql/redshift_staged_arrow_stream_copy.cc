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

#include "redshift_staged_arrow_stream_copy.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include <nanoarrow/nanoarrow.hpp>

namespace adbc::driver::pgwire {
namespace {

// Empty batches produce no bytes, so the payload cap alone cannot bound a
// broken stream that yields them indefinitely. This private first step accepts
// at most 128 batches, including empty ones.
constexpr std::size_t kMaxStagedCopyStreamBatches = 128;

RedshiftCsvWriteStatus ValidateEmptyBatch(const ArrowSchema* schema,
                                          const ArrowArray* array) {
  if (!schema->format || !schema->children || schema->n_children < 0 || !array->buffers ||
      !array->children || array->n_children != schema->n_children) {
    return RedshiftCsvWriteStatus::kMalformedArrow;
  }
  for (int64_t i = 0; i < array->n_children; ++i) {
    if (!schema->children[i] || !schema->children[i]->format || !array->children[i] ||
        !array->children[i]->buffers) {
      return RedshiftCsvWriteStatus::kMalformedArrow;
    }
  }

  nanoarrow::UniqueArrayView view;
  ArrowError error;
  ArrowErrorInit(&error);
  if (ArrowArrayViewInitFromSchema(view.get(), schema, &error) != NANOARROW_OK ||
      ArrowArrayViewSetArray(view.get(), array, &error) != NANOARROW_OK ||
      ArrowArrayViewValidate(view.get(), NANOARROW_VALIDATION_LEVEL_FULL, &error) !=
          NANOARROW_OK) {
    return RedshiftCsvWriteStatus::kMalformedArrow;
  }
  return RedshiftCsvWriteStatus::kSucceeded;
}

}  // namespace

RedshiftStagedArrowStreamCopyResult RunRedshiftStagedArrowStreamCopy(
    const RedshiftStagedArrowStreamCopyRequest& request, RedshiftStagedObjectStore& store,
    const std::function<RedshiftCopyExecutionResult(std::string_view)>& execute_copy) {
  if (!request.arrow_stream || !request.arrow_stream->release ||
      !request.arrow_stream->get_schema || !request.arrow_stream->get_next ||
      !request.arrow_stream->get_last_error) {
    return {RedshiftStagedArrowStreamPreflightStatus::kInvalidInput, std::nullopt,
            std::nullopt};
  }

  nanoarrow::UniqueArrayStream owned_stream;
  owned_stream.reset(request.arrow_stream);
  nanoarrow::UniqueSchema schema;
  ArrowError error;
  ArrowErrorInit(&error);
  if (ArrowArrayStreamGetSchema(owned_stream.get(), schema.get(), &error) !=
          NANOARROW_OK ||
      !schema->release) {
    return {RedshiftStagedArrowStreamPreflightStatus::kStreamError, std::nullopt,
            std::nullopt};
  }

  std::string csv;
  std::size_t batch_count = 0;
  for (;;) {
    nanoarrow::UniqueArray array;
    ArrowErrorInit(&error);
    if (ArrowArrayStreamGetNext(owned_stream.get(), array.get(), &error) !=
        NANOARROW_OK) {
      return {RedshiftStagedArrowStreamPreflightStatus::kStreamError, std::nullopt,
              std::nullopt};
    }
    if (!array->release) break;
    if (++batch_count > kMaxStagedCopyStreamBatches) {
      return {RedshiftStagedArrowStreamPreflightStatus::kTooManyBatches, std::nullopt,
              std::nullopt};
    }
    if (array->length == 0) {
      const auto csv_status = ValidateEmptyBatch(schema.get(), array.get());
      if (csv_status != RedshiftCsvWriteStatus::kSucceeded) {
        return {RedshiftStagedArrowStreamPreflightStatus::kCsvRejected, csv_status,
                std::nullopt};
      }
      continue;
    }

    std::string batch_csv;
    const auto csv_status =
        WriteRedshiftCsv(schema.get(), array.get(), request.columns, &batch_csv);
    if (csv_status != RedshiftCsvWriteStatus::kSucceeded) {
      return {RedshiftStagedArrowStreamPreflightStatus::kCsvRejected, csv_status,
              std::nullopt};
    }
    if (batch_csv.size() > kRedshiftStagedCopyMaxPayloadBytes - csv.size()) {
      return {RedshiftStagedArrowStreamPreflightStatus::kPayloadTooLarge, std::nullopt,
              std::nullopt};
    }
    csv += batch_csv;
  }
  if (csv.empty()) {
    return {RedshiftStagedArrowStreamPreflightStatus::kEmptyStream, std::nullopt,
            std::nullopt};
  }

  // The stream is fully consumed; release any producer resources before the
  // synchronous store and COPY callbacks begin.
  schema.reset();
  owned_stream.reset();

  const RedshiftStagedCopyRequest staged_request = {
      request.database_schema, request.table,
      request.columns,         request.data_s3_url,
      request.manifest_s3_url, request.iam_role_arn,
      request.ownership_token, csv};
  return {RedshiftStagedArrowStreamPreflightStatus::kSucceeded, std::nullopt,
          RunRedshiftStagedCopy(staged_request, store, execute_copy)};
}

}  // namespace adbc::driver::pgwire
