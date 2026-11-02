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

// Included by the public recorder and the bounded offline batch seam.
TEST(RedshiftVendorLimitsRecordingTest, ByteFlushRevisitsRowsAcrossInputBoundaries) {
  for (bool wide_row : {false, true})
    for (bool split : {false, true})
      for (bool invalid_tail : {false, true}) {
        recording.executions.clear();
        nanoarrow::UniqueSchema schema;
        ArrowSchemaInit(schema.get());
        ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 2), 0);
        for (int col = 0; col < 2; ++col)
          ASSERT_EQ(ArrowSchemaSetType(schema->children[col], NANOARROW_TYPE_LARGE_STRING),
                    0);
        std::string large = "\"" + std::string(9 * 1024 * 1024 - 2, 'a') + "\"";
        nanoarrow::UniqueArrayStream stream;
        ASSERT_EQ(ArrowBasicArrayStreamInit(stream.get(), schema.get(), split ? 3 : 1),
                  0);
        // ArrowBasicArrayStreamInit moves the schema; recover an owned copy.
        ASSERT_EQ(ArrowArrayStreamGetSchema(stream.get(), schema.get(), nullptr), 0);
        for (int batch = 0; batch < (split ? 3 : 1); ++batch) {
          nanoarrow::UniqueArray array;
          ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
          ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
          const int begin = !split || batch == 0 ? 0 : 1;
          const int end = !split ? (invalid_tail ? 4 : 3)
                         : batch == 0 ? 1
                         : batch == 1 ? 1
                                      : (invalid_tail ? 4 : 3);
          for (int row = begin; row < end; ++row) {
            if (wide_row && row == 0) {
              ASSERT_EQ(ArrowArrayAppendString(array->children[0],
                                               {large.data(),
                                                static_cast<int64_t>(large.size())}),
                        0);
            } else {
              ASSERT_EQ(ArrowArrayAppendNull(array->children[0], 1), 0);
            }
            large[1] = static_cast<char>('a' + row);
            const std::string_view value = row == 3 ? std::string_view(R"({"a":1,"\u0061":2})")
                                                   : std::string_view(large);
            ASSERT_EQ(ArrowArrayAppendString(
                          array->children[1],
                          {value.data(), static_cast<int64_t>(value.size())}),
                      0);
            ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
          }
          ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
          ArrowBasicArrayStreamSetArray(stream.get(), batch, array.get());
        }
        BindStream bind;
        bind.SetBind(stream.get());
        EXPECT_FALSE(stream->release);
        ASSERT_TRUE(bind.Begin([] { return Status::Ok(); }).ok());
        // Exercise batch mechanics with the real strict writers, while bypassing
        // catalog/session setup already covered by the public API recorder.
        bind.validate_redshift_bytes = true;
        bind.generated_super_ingest = true;
        bind.param_types.assign(2, 72112);
        bind.param_formats.assign(2, 1);
        bind.param_lengths.resize(2);
        bind.param_values.resize(2);
        bind.timetz_fields.assign(2, false);
        bind.bytes_fields.assign(2, false);
        bind.interval_fields.assign(2, false);
        bind.json_parse_fields.assign(2, true);
        bind.encoded_fields.assign(2, RedshiftEncodedKind::kNone);
        for (int col = 0; col < 2; ++col)
          bind.bind_field_writers.push_back(
              PostgresCopyFieldWriter::Create<RedshiftSuperFieldWriter>(
                  bind.array_view->children[col]));
        int64_t total = 0;
        bool failed = false;
        for (int call = 0; call < 5; ++call) {
          int64_t rows = -1;
          auto status = bind.BindAndExecuteNextBatch(nullptr, "owned", "a,b", 16, &rows);
          if (!status.ok()) {
            AdbcError error = ADBC_ERROR_INIT;
            EXPECT_EQ(status.ToAdbc(&error), ADBC_STATUS_INVALID_ARGUMENT);
            ASSERT_NE(error.message, nullptr);
            EXPECT_NE(std::string_view(error.message).find("duplicate key"),
                      std::string_view::npos);
            if (error.release) error.release(&error);
            failed = true;
            break;
          }
          total += rows;
          EXPECT_LE(bind.param_buffer->size_bytes, 18 * 1024 * 1024 + 8);
          if (rows == 0) break;
          EXPECT_EQ(rows, 1);
        }
        EXPECT_EQ(failed, invalid_tail);
        EXPECT_EQ(total, invalid_tail ? 2 : 3);
        ASSERT_EQ(recording.executions.size(), invalid_tail ? 2 : 3);
        for (size_t row = 0; row < recording.executions.size(); ++row) {
          const auto& params = recording.executions[row];
          ASSERT_EQ(params.size(), 2);
          EXPECT_EQ(params[0].null, !(wide_row && row == 0));
          EXPECT_FALSE(params[1].null);
          EXPECT_EQ(params[1].bytes.size(), 9 * 1024 * 1024);
          EXPECT_EQ(params[1].bytes[1], 'a' + row);
          EXPECT_EQ(params[1].oid, 72112U);
          EXPECT_EQ(params[1].format, 1);
        }
      }
}
TEST(RedshiftVendorLimitsRecordingTest, CheckedMessageFramingDoesNotLimitRowToBatchTarget) {
  EXPECT_TRUE(BindStream::ParameterMessageFits(32 * 1024 * 1024, 2));
  EXPECT_TRUE(BindStream::ParameterMessageFits(static_cast<int64_t>(INT_MAX) - 20, 2));
  EXPECT_FALSE(BindStream::ParameterMessageFits(static_cast<int64_t>(INT_MAX) - 19, 2));
  EXPECT_FALSE(BindStream::ParameterMessageFits(INT64_MAX, 2));
  EXPECT_FALSE(BindStream::ParameterMessageFits(0, INT64_MAX));
  EXPECT_FALSE(BindStream::ParameterMessageFits(-1, 0));
}
