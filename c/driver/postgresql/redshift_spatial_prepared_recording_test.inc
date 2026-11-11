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

class SpatialPreparedRecordingTest : public IntervalPublicRecordingTest {
 protected:
  static constexpr const char* kPoint =
      "0101000020E6100000000000000000F03F0000000000000040";
  static constexpr const char* kEmpty = "0102000020E610000000000000";
  static std::string Raw(std::string hex) {
    for (char& c : hex)
      if (c >= 'A' && c <= 'F') c += 'a' - 'A';
    return testing::NumericHex(hex.c_str());
  }
  void SetUp() override {
    IntervalPublicRecordingTest::SetUp();
    recording.direct_spatial = true;
  }
  void Schema(ArrowSchema* schema, RedshiftEncodedKind kind) {
    EncodedSchema(schema, {kind, RedshiftEncodedKind::kNone, RedshiftEncodedKind::kNone});
    ASSERT_EQ(ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_BINARY), 0);
    ASSERT_EQ(testing::DecimalMetadata(schema->children[1], "ADBC:redshift:sql_type",
                                       "VARBYTE"),
              0);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[2], NANOARROW_TYPE_INT32), 0);
  }
  void Batch(ArrowSchema* schema, ArrowArray* array,
             const std::vector<std::optional<std::string>>& spatial, int null_at = -1) {
    ASSERT_EQ(ArrowArrayInitFromSchema(array, schema, nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(array), 0);
    for (const auto& value : spatial) {
      for (int col = 0; col < 3; ++col) {
        if (col == null_at || (col == 0 && !value)) {
          ASSERT_EQ(ArrowArrayAppendNull(array->children[col], 1), 0);
        } else if (col == 2) {
          ASSERT_EQ(ArrowArrayAppendInt(array->children[col], -71), 0);
        } else {
          const auto bytes = col == 0 ? *value : Raw("0080ff");
          ArrowBufferView view{};
          view.data.as_char = bytes.data();
          view.size_bytes = bytes.size();
          ASSERT_EQ(ArrowArrayAppendBytes(array->children[col], view), 0);
        }
      }
      ASSERT_EQ(ArrowArrayFinishElement(array), 0);
    }
    ASSERT_EQ(ArrowArrayFinishBuildingDefault(array, nullptr), 0);
  }
  void Bind(bool stream, RedshiftEncodedKind kind,
            const std::vector<std::optional<std::string>>& spatial, int null_at = -1,
            const char* query = "INSERT INTO owned_spatial VALUES ($1, $2, $3)") {
    nanoarrow::UniqueSchema schema;
    Schema(schema.get(), kind);
    nanoarrow::UniqueArray array;
    Batch(schema.get(), array.get(), spatial, null_at);
    ASSERT_EQ(driver.StatementSetSqlQuery(&statement, query, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementPrepare(&statement, &error), ADBC_STATUS_OK);
    if (stream) {
      nanoarrow::UniqueArrayStream input;
      adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
      ASSERT_EQ(driver.StatementBindStream(&statement, input.get(), &error),
                ADBC_STATUS_OK);
      EXPECT_FALSE(input->release);
    } else {
      ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                ADBC_STATUS_OK);
      EXPECT_FALSE(array->release);
      EXPECT_FALSE(schema->release);
    }
  }
};
TEST_F(SpatialPreparedRecordingTest, ArrayStreamPointNullEmptyMixedNeighborsNoRewrite) {
  for (auto kind : {RedshiftEncodedKind::kGeometry, RedshiftEncodedKind::kGeography})
    for (bool stream : {false, true})
      for (int null_at : {-1, 1, 2}) {
        Bind(stream, kind, {Raw(kPoint), std::nullopt, Raw(kEmpty)}, null_at);
        recording.executions.clear();
        recording.queries.clear();
        int64_t affected = -1;
        ASSERT_EQ(driver.StatementExecuteQuery(&statement, nullptr, &affected, &error),
                  ADBC_STATUS_OK)
            << Message();
        ASSERT_EQ(recording.executions.size(), 3);
        for (size_t row = 0; row < 3; ++row) {
          const auto& params = recording.executions[row];
          ASSERT_EQ(params.size(), 3);
          EXPECT_EQ(params[0].oid,
                    kind == RedshiftEncodedKind::kGeometry ? 72113 : 72114);
          EXPECT_EQ(params[0].format, 0);
          EXPECT_EQ(params[0].null, row == 1);
          EXPECT_EQ(params[0].physical_length,
                    row == 1 ? 0 : 1 + std::strlen(row == 0 ? kPoint : kEmpty));
          if (row != 1) {
            EXPECT_EQ(params[0].bytes, row == 0 ? kPoint : kEmpty);
          }
          for (int col = 1; col < 3; ++col) {
            EXPECT_EQ(params[col].oid, col == 1 ? 72112 : 23);
            EXPECT_EQ(params[col].format, 1);
            EXPECT_EQ(params[col].null, col == null_at);
            if (col != null_at) {
              EXPECT_EQ(params[col].bytes, Raw(col == 1 ? "0080ff" : "ffffffb9"));
            }
          }
        }
        for (const auto& query : recording.queries)
          EXPECT_EQ(query, "INSERT INTO owned_spatial VALUES ($1, $2, $3)");
      }
}
TEST_F(SpatialPreparedRecordingTest, ProjectionKeepsLogicalSchemaAcrossBoundRows) {
  recording.spatial_projection = true;
  for (bool stream : {false, true}) {
    Bind(stream, RedshiftEncodedKind::kGeography,
         {Raw(kPoint), std::nullopt, Raw(kEmpty)});
    recording.executions.clear();
    nanoarrow::UniqueArrayStream output;
    ASSERT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
              ADBC_STATUS_OK)
        << Message();
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(output->get_schema(output.get(), schema.get()), 0);
    ASSERT_EQ(schema->n_children, 1);
    EXPECT_STREQ(schema->children[0]->name, "marker");
    EXPECT_STREQ(schema->children[0]->format, "i");
    int64_t count = 0;
    while (true) {
      nanoarrow::UniqueArray array;
      ASSERT_EQ(output->get_next(output.get(), array.get()), 0)
          << output->get_last_error(output.get());
      if (!array->release) break;
      nanoarrow::UniqueArrayView view;
      ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), nullptr), 0);
      ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), nullptr), 0);
      for (int64_t i = 0; i < array->length; ++i)
        EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view->children[0], i), -71);
      count += array->length;
    }
    EXPECT_EQ(count, 3);
    EXPECT_EQ(recording.executions.size(), 3);
  }
}
TEST_F(SpatialPreparedRecordingTest, DescriptorMismatchRejectsNullBeforeExecution) {
  for (auto kind : {RedshiftEncodedKind::kGeometry, RedshiftEncodedKind::kGeography})
    for (bool stream : {false, true})
      for (int mismatch = 1; mismatch <= 4; ++mismatch) {
        Bind(stream, kind, {std::nullopt});
        recording.spatial_mismatch = mismatch;
        recording.executions.clear();
        EXPECT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
                  ADBC_STATUS_INVALID_STATE)
            << Message();
        EXPECT_TRUE(recording.executions.empty());
      }
}
TEST_F(SpatialPreparedRecordingTest, MalformedLaterRowRejectsWholeCurrentBatch) {
  for (bool stream : {false, true})
    for (const auto& bad :
         std::vector<std::string>{"", "bad", Raw(kPoint) + "x",
                                  Raw("0101000000000000000000F03F0000000000000040")}) {
      Bind(stream, RedshiftEncodedKind::kGeography, {Raw(kPoint), bad});
      recording.executions.clear();
      EXPECT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
                ADBC_STATUS_INVALID_ARGUMENT)
          << Message();
      EXPECT_TRUE(recording.executions.empty());
    }
}
TEST_F(SpatialPreparedRecordingTest, EmptyInputRetainsOutputSchemaWithoutExecution) {
  recording.spatial_projection = true;
  for (bool stream : {false, true}) {
    Bind(stream, RedshiftEncodedKind::kGeometry, {});
    nanoarrow::UniqueArrayStream output;
    ASSERT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
              ADBC_STATUS_OK)
        << Message();
    EXPECT_TRUE(recording.executions.empty());
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(output->get_schema(output.get(), schema.get()), 0);
    ASSERT_EQ(schema->n_children, 1);
    EXPECT_STREQ(schema->children[0]->name, "marker");
    EXPECT_STREQ(schema->children[0]->format, "i");
    nanoarrow::UniqueArray array;
    ASSERT_EQ(output->get_next(output.get(), array.get()), 0);
    EXPECT_FALSE(array->release);
  }
}
TEST_F(SpatialPreparedRecordingTest, OutputDriftAndServerErrorsNeverRetry) {
  for (bool output : {false, true}) {
    recording.spatial_projection = output;
    recording.spatial_output_drift = output;
    recording.spatial_execute_error = !output;
    recording.spatial_prepare_calls = 0;
    recording.executions.clear();
    Bind(true, RedshiftEncodedKind::kGeometry, {Raw(kPoint)});
    nanoarrow::UniqueArrayStream result;
    EXPECT_NE(driver.StatementExecuteQuery(&statement, result.get(), nullptr, &error),
              ADBC_STATUS_OK);
    EXPECT_EQ(recording.executions.size(), output ? 0 : 1);
    EXPECT_FALSE(result->release);
  }
}
struct SpatialStreamState {
  ArrowArrayStream input{};
  int calls = 0, fail_at = -1;
  int* releases;
};
void SpatialStream(ArrowArrayStream* input, ArrowArrayStream* output, int fail_at,
                   int* releases) {
  auto* state = new SpatialStreamState;
  state->releases = releases;
  state->fail_at = fail_at;
  ArrowArrayStreamMove(input, &state->input);
  output->private_data = state;
  output->get_schema = [](ArrowArrayStream* stream, ArrowSchema* schema) {
    auto* state = static_cast<SpatialStreamState*>(stream->private_data);
    return state->input.get_schema(&state->input, schema);
  };
  output->get_next = [](ArrowArrayStream* stream, ArrowArray* array) {
    auto* state = static_cast<SpatialStreamState*>(stream->private_data);
    // A callback can invalidate the unnamed statement; each row must prepare again.
    recording.prepared_valid = false;
    if (state->calls++ == state->fail_at) return EIO;
    return state->input.get_next(&state->input, array);
  };
  output->get_last_error = [](ArrowArrayStream*) { return "spatial input failure"; };
  output->release = [](ArrowArrayStream* stream) {
    auto* state = static_cast<SpatialStreamState*>(stream->private_data);
    if (state->input.release) state->input.release(&state->input);
    ++*state->releases;
    delete state;
    stream->release = nullptr;
  };
}
TEST_F(SpatialPreparedRecordingTest,
       MultipleBatchesCallbackInvalidationAndStreamFailure) {
  for (int fail_at : {-1, 1}) {
    int releases = 0;
    nanoarrow::UniqueSchema schema;
    Schema(schema.get(), RedshiftEncodedKind::kGeometry);
    nanoarrow::UniqueArray first, second;
    Batch(schema.get(), first.get(), {Raw(kPoint)});
    Batch(schema.get(), second.get(), {Raw(kEmpty), std::nullopt});
    nanoarrow::UniqueArrayStream input, wrapped;
    ASSERT_EQ(ArrowBasicArrayStreamInit(input.get(), schema.get(), 2), 0);
    ArrowBasicArrayStreamSetArray(input.get(), 0, first.get());
    ArrowBasicArrayStreamSetArray(input.get(), 1, second.get());
    SpatialStream(input.get(), wrapped.get(), fail_at, &releases);
    ASSERT_EQ(driver.StatementSetSqlQuery(
                  &statement, "INSERT INTO owned_spatial VALUES ($1, $2, $3)", &error),
              ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementBindStream(&statement, wrapped.get(), &error),
              ADBC_STATUS_OK);
    recording.executions.clear();
    const auto status =
        driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error);
    EXPECT_EQ(status, fail_at < 0 ? ADBC_STATUS_OK : ADBC_STATUS_IO) << Message();
    EXPECT_EQ(recording.executions.size(), fail_at < 0 ? 3 : 1);
    EXPECT_EQ(releases, 1);
    if (fail_at >= 0) {
      EXPECT_NE(std::string(Message()).find("spatial input failure"), std::string::npos);
    }
  }
}

TEST_F(SpatialPreparedRecordingTest, CombinedSuperAndHllKeepSpatialNativeTextParameters) {
  for (bool super : {false, true})
    for (bool stream : {false, true})
      for (int null_at : {-1, 1, 2, 3}) {
        recording.direct_super = super;
        recording.direct_spatial = !super;
        recording.executions.clear();
        nanoarrow::UniqueSchema schema;
        EncodedSchema(schema.get(),
                      {RedshiftEncodedKind::kHll, RedshiftEncodedKind::kGeometry,
                       RedshiftEncodedKind::kGeography, RedshiftEncodedKind::kHll,
                       RedshiftEncodedKind::kNone, RedshiftEncodedKind::kNone});
        if (super) {
          nanoarrow::UniqueSchema source;
          ASSERT_EQ(testing::SuperSchema(source.get()), 0);
          schema->children[0]->release(schema->children[0]);
          ASSERT_EQ(ArrowSchemaDeepCopy(source->children[0], schema->children[0]), 0);
        }
        ASSERT_EQ(ArrowSchemaSetType(schema->children[4], NANOARROW_TYPE_BINARY), 0);
        ASSERT_EQ(testing::DecimalMetadata(schema->children[4], "ADBC:redshift:sql_type",
                                           "VARBYTE"),
                  0);
        ASSERT_EQ(ArrowSchemaSetType(schema->children[5], NANOARROW_TYPE_INT32), 0);
        nanoarrow::UniqueArray array;
        ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
        ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
        for (int col = 0; col < 6; ++col) {
          if (col == null_at) {
            ASSERT_EQ(ArrowArrayAppendNull(array->children[col], 1), 0);
          } else if (col == 0 || col == 3) {
            ASSERT_EQ(ArrowArrayAppendString(
                          array->children[col],
                          ArrowCharView(col == 0 && super ? "true" : kSparseHll)),
                      0);
          } else if (col == 5) {
            ASSERT_EQ(ArrowArrayAppendInt(array->children[col], -71), 0);
          } else {
            const auto bytes = Raw(col == 1 ? kPoint : col == 2 ? kEmpty : "0080ff");
            ArrowBufferView value{};
            value.data.as_char = bytes.data();
            value.size_bytes = bytes.size();
            ASSERT_EQ(ArrowArrayAppendBytes(array->children[col], value), 0);
          }
        }
        ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
        ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
        ASSERT_EQ(driver.StatementSetSqlQuery(
                      &statement, "INSERT INTO mixed_vendor VALUES ($1,$2,$3,$4,$5,$6)",
                      &error),
                  ADBC_STATUS_OK);
        ASSERT_EQ(driver.StatementPrepare(&statement, &error), ADBC_STATUS_OK);
        if (stream) {
          nanoarrow::UniqueArrayStream input;
          adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
          ASSERT_EQ(driver.StatementBindStream(&statement, input.get(), &error),
                    ADBC_STATUS_OK);
        } else {
          ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                    ADBC_STATUS_OK);
        }
        ASSERT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
                  ADBC_STATUS_OK)
            << Message();
        ASSERT_EQ(recording.executions.size(), 1);
        const auto& values = recording.executions[0];
        ASSERT_EQ(values.size(), 6);
        const std::vector<Oid> expected = {
            super ? 72112U : 72115U, 72113, 72114, 72115, 72112, 23};
        const std::vector<int> formats = {super ? 1 : 0, 0, 0, 0, 1, 1};
        for (int col = 0; col < 6; ++col) {
          EXPECT_EQ(values[col].oid, expected[col]);
          EXPECT_EQ(values[col].format, formats[col]);
          EXPECT_EQ(values[col].null, col == null_at);
        }
        EXPECT_EQ(values[0].bytes, super ? "true" : kSparseHll);
        if (null_at != 1) {
          EXPECT_EQ(values[1].bytes, kPoint);
        }
        if (null_at != 2) {
          EXPECT_EQ(values[2].bytes, kEmpty);
        }
        if (null_at != 3) {
          EXPECT_EQ(values[3].bytes, kSparseHll);
        }
        EXPECT_EQ(values[4].bytes, Raw("0080ff"));
        EXPECT_EQ(values[5].bytes, Raw("ffffffb9"));
        EXPECT_EQ(recording.prepared_query,
                  super ? "INSERT INTO mixed_vendor VALUES "
                          "((\"pg_catalog\".\"json_parse\"($1)),$2,$3,$4,$5,$6)"
                        : "INSERT INTO mixed_vendor VALUES ($1,$2,$3,$4,$5,$6)");
      }
}

TEST_F(SpatialPreparedRecordingTest, MixedHllResultComposesWithSpatialPlan) {
  recording.mixed_projection = true;
  for (bool stream : {false, true}) {
    recording.executions.clear();
    recording.format_requests.clear();
    ASSERT_EQ(driver.StatementSetSqlQuery(&statement, "SELECT $1,$2,$3,owned_hll", &error), ADBC_STATUS_OK);
    Bind(stream, RedshiftEncodedKind::kGeometry, {Raw(kPoint), std::nullopt}, -1,
         "SELECT $1,$2,$3,owned_hll");
    nanoarrow::UniqueArrayStream out;
    ASSERT_EQ(driver.StatementExecuteQuery(&statement, out.get(), nullptr, &error), ADBC_STATUS_OK) << Message();
    for (int row = 0; row < 2; ++row) {
      nanoarrow::UniqueArray value;
      ASSERT_EQ(out->get_next(out.get(), value.get()), 0) << out->get_last_error(out.get());
      ASSERT_NE(value->release, nullptr);
      EXPECT_EQ(value->length, 2);
    }
    nanoarrow::UniqueArray eof;
    ASSERT_EQ(out->get_next(out.get(), eof.get()), 0);
    EXPECT_EQ(eof->release, nullptr);
    ASSERT_EQ(recording.executions.size(), 2);
    ASSERT_EQ(recording.format_requests.size(), 2);
    for (const auto& formats : recording.format_requests)
      EXPECT_EQ(formats, (std::vector<int>{0, 1}));
    EXPECT_EQ(recording.prepared_query, "SELECT $1,$2,$3,owned_hll");
  }
}
