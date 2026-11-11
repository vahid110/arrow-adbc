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

class SuperPreparedRecordingTest : public IntervalPublicRecordingTest {
 public:
  void SetUp() override {
    IntervalPublicRecordingTest::SetUp();
    recording.direct_super = true;
  }
  void BindSuper(const std::vector<std::optional<std::string>>& values,
                 bool stream = false) {
    nanoarrow::UniqueSchema schema;
    nanoarrow::UniqueArray array;
    ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
    ASSERT_EQ(testing::SuperBatch(schema.get(), array.get(), values), 0);
    if (stream) {
      nanoarrow::UniqueArrayStream input;
      adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
      ASSERT_EQ(driver.StatementBindStream(&statement, input.get(), &error),
                ADBC_STATUS_OK) << Message();
    } else {
      ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error),
                ADBC_STATUS_OK) << Message();
    }
  }
  void Sql(const char* sql) {
    ASSERT_EQ(driver.StatementSetSqlQuery(&statement, sql, &error), ADBC_STATUS_OK);
  }
  void ExpectDispatchOrder(size_t rows) {
    size_t count = 0;
    for (size_t i = 0; i < recording.events.size(); ++i) {
      if (recording.events[i] != "execute") continue;
      ASSERT_GE(i, 4U);
      EXPECT_EQ(recording.events[i - 4], "logical-prepare");
      EXPECT_EQ(recording.events[i - 3], "describe");
      EXPECT_EQ(recording.events[i - 2], "physical-prepare");
      EXPECT_EQ(recording.events[i - 1], "describe");
      ++count;
    }
    EXPECT_EQ(count, rows);
  }
};

TEST_F(SuperPreparedRecordingTest, ArrayAndStreamKeepLogicalLabelsAndPhysicalBytes) {
  for (bool input_stream : {false, true}) {
    recording.executions.clear();
    recording.events.clear();
    Sql("SELECT $1");
    const std::vector<std::optional<std::string>> values = {
        R"({"n":1.20})", R"([1,"é🙂"])", "true", "null", std::nullopt};
    BindSuper(values, input_stream);
    ASSERT_EQ(driver.StatementPrepare(&statement, &error), ADBC_STATUS_OK);
    nanoarrow::UniqueArrayStream output;
    ASSERT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
              ADBC_STATUS_OK) << Message();
    nanoarrow::UniqueSchema schema;
    ASSERT_EQ(output->get_schema(output.get(), schema.get()), 0);
    ASSERT_EQ(schema->n_children, 1);
    EXPECT_STREQ(schema->children[0]->name, "?column?");
    ArrowError parse{};
    EXPECT_EQ(RedshiftSuperResultSchema::Parse(schema->children[0], &parse), 0);
    for (size_t row = 0; row < values.size(); ++row) {
      nanoarrow::UniqueArray array;
      ASSERT_EQ(output->get_next(output.get(), array.get()), 0)
          << output->get_last_error(output.get());
      ASSERT_NE(array->release, nullptr);
      EXPECT_EQ(array->length, 1);
    }
    nanoarrow::UniqueArray eof;
    ASSERT_EQ(output->get_next(output.get(), eof.get()), 0);
    EXPECT_FALSE(eof->release);
    ASSERT_EQ(recording.executions.size(), values.size());
    for (size_t i = 0; i < values.size(); ++i) {
      const auto& value = recording.executions[i][0];
      EXPECT_EQ(value.oid, 72112U);
      EXPECT_EQ(value.format, 1);
      EXPECT_EQ(value.null, !values[i].has_value());
      if (values[i]) {
        EXPECT_EQ(value.bytes, *values[i]);
      }
    }
    ExpectDispatchOrder(values.size());
  }
}

TEST_F(SuperPreparedRecordingTest, ParameterAndOutputSchemasAreLogicalAndDoNotExecute) {
  Sql("SELECT $1 AS value");
  BindSuper({R"({"n":1})"});
  nanoarrow::UniqueSchema params, output;
  ASSERT_EQ(driver.StatementGetParameterSchema(&statement, params.get(), &error),
            ADBC_STATUS_OK) << Message();
  ArrowError parse{};
  RedshiftSuperSchema source;
  ASSERT_EQ(RedshiftSuperSchema::Parse(params->children[0], true, &source, &parse), 0);
  EXPECT_TRUE(source.enabled);
  ASSERT_EQ(driver.StatementExecuteSchema(&statement, output.get(), &error),
            ADBC_STATUS_OK) << Message();
  EXPECT_STREQ(output->children[0]->name, "value");
  EXPECT_TRUE(recording.executions.empty());
  ASSERT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
            ADBC_STATUS_OK) << Message();
  EXPECT_EQ(recording.executions.size(), 1U);
}

TEST_F(SuperPreparedRecordingTest, DmlRepeatedReferencesAndRebindUseOneExecutionPerRow) {
  for (const char* query : {
           "INSERT INTO t VALUES ($1,$1)",
           "UPDATE t SET v = $1 WHERE old = $1",
           "DELETE FROM t WHERE v = $1",
           "WITH q AS (SELECT $1 AS n) SELECT n FROM q",
       }) {
    recording.events.clear();
    recording.executions.clear();
    Sql(query);
    BindSuper({"true", std::nullopt}, true);
    ASSERT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ADBC_STATUS_OK) << Message();
    ASSERT_EQ(recording.executions.size(), 2U);
    for (const auto& row : recording.executions) EXPECT_EQ(row.size(), 1U);
    ExpectDispatchOrder(2);
    EXPECT_NE(recording.prepared_query.find(R"("pg_catalog"."json_parse"($1))"),
              std::string::npos);
  }
}

TEST_F(SuperPreparedRecordingTest, EmptyInputDescribesWithoutUserExecution) {
  Sql("SELECT $1");
  BindSuper({}, true);
  nanoarrow::UniqueArrayStream output;
  ASSERT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
            ADBC_STATUS_OK) << Message();
  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(output->get_schema(output.get(), schema.get()), 0);
  EXPECT_STREQ(schema->children[0]->name, "?column?");
  nanoarrow::UniqueArray array;
  ASSERT_EQ(output->get_next(output.get(), array.get()), 0);
  EXPECT_FALSE(array->release);
  EXPECT_TRUE(recording.executions.empty());
}

TEST_F(SuperPreparedRecordingTest, UnqualifiedConverterOrContextCannotExecute) {
  Sql("INSERT INTO t VALUES ($1)");
  for (int drift = 1; drift <= 6; ++drift) {
    recording.converter_drift = drift;
    recording.events.clear();
    BindSuper({"true"});
    EXPECT_NE(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ADBC_STATUS_OK);
    EXPECT_TRUE(recording.executions.empty());
    EXPECT_EQ(std::count(recording.events.begin(), recording.events.end(), "logical-prepare"), 0);
  }
  recording.converter_drift = 0;
  recording.super_setting = "on";
  BindSuper({"true"});
  EXPECT_NE(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
            ADBC_STATUS_OK);
  EXPECT_TRUE(recording.executions.empty());
}

TEST_F(SuperPreparedRecordingTest, DescriptorDriftStopsBeforeLaterRows) {
  Sql("SELECT $1");
  for (int drift = 1; drift <= 7; ++drift) {
    recording.direct_descriptor_drift = drift;
    recording.events.clear();
    recording.executions.clear();
    BindSuper({"true", "false"});
    EXPECT_NE(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ADBC_STATUS_OK);
    EXPECT_LE(recording.executions.size(), 1U);
    const bool execution_drift = drift == 1 || drift == 6;
    EXPECT_EQ(recording.executions.size(), execution_drift ? 1U : 0U);
  }
}

TEST_F(SuperPreparedRecordingTest, PrepareAndExecutionErrorsRetainOriginalDetails) {
  Sql("SELECT $1");
  for (const char* phase : {"logical-prepare", "physical-prepare", "execute"}) {
    if (error.release) error.release(&error);
    error = ADBC_ERROR_INIT;
    recording.direct_failure = phase;
    recording.events.clear();
    recording.executions.clear();
    BindSuper({"true", "false"});
    EXPECT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ADBC_STATUS_INTERNAL) << Message();
    EXPECT_EQ(std::string(error.sqlstate, 5), "XX000");
    EXPECT_NE(std::string(Message()).find("unqualified recorder failure"), std::string::npos);
    EXPECT_EQ(recording.executions.size(), std::string(phase) == "execute" ? 1U : 0U);
    bool original = false, internal = false, mapped = false;
    for (int i = 0; i < driver.ErrorGetDetailCount(&error); ++i) {
      const auto detail = driver.ErrorGetDetail(&error, i);
      const std::string value(reinterpret_cast<const char*>(detail.value),
                              detail.value_length);
      if (std::string_view(detail.key) == "PG_DIAG_STATEMENT_POSITION") {
        EXPECT_EQ(value, "15");
        original = true;
      }
      if (std::string_view(detail.key) == "ADBC:redshift:original_statement_position") {
        EXPECT_EQ(value, "8");
        mapped = true;
      }
      if (std::string_view(detail.key) == "PG_DIAG_INTERNAL_POSITION") {
        EXPECT_EQ(value, "9");
        internal = true;
      }
    }
    EXPECT_TRUE(original);
    EXPECT_TRUE(internal);
    EXPECT_EQ(mapped, std::string(phase) != "logical-prepare");
  }
}

TEST_F(SuperPreparedRecordingTest, MalformedBatchNeverExecutesItsEarlierValidRow) {
  Sql("INSERT INTO t VALUES ($1)");
  BindSuper({"true", R"({"a":1,"\u0061":2})"}, true);
  EXPECT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
            ADBC_STATUS_INVALID_ARGUMENT);
  EXPECT_TRUE(recording.executions.empty());
  EXPECT_EQ(std::count(recording.events.begin(), recording.events.end(), "logical-prepare"), 0);
}

TEST_F(SuperPreparedRecordingTest, LateBatchFailurePreservesOnlyEarlierExecutions) {
  Sql("INSERT INTO t VALUES ($1)");
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray first, late;
  ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
  ASSERT_EQ(testing::SuperBatch(schema.get(), first.get(), {"true"}), 0);
  ASSERT_EQ(testing::SuperBatch(schema.get(), late.get(), {"false", R"({"a":1,"a":2})"}), 0);
  nanoarrow::UniqueArrayStream input;
  ASSERT_EQ(ArrowBasicArrayStreamInit(input.get(), schema.get(), 2), 0);
  ArrowBasicArrayStreamSetArray(input.get(), 0, first.get());
  ArrowBasicArrayStreamSetArray(input.get(), 1, late.get());
  ASSERT_EQ(driver.StatementBindStream(&statement, input.get(), &error), ADBC_STATUS_OK);
  EXPECT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
            ADBC_STATUS_INVALID_ARGUMENT);
  ASSERT_EQ(recording.executions.size(), 1U);
  EXPECT_EQ(recording.executions[0][0].bytes, "true");
}

TEST_F(SuperPreparedRecordingTest, BinaryNeighborsKeepTheirOidFormatAndBytes) {
  Sql("INSERT INTO t VALUES ($1,$2,$3)");
  nanoarrow::UniqueSchema super, schema;
  ASSERT_EQ(testing::SuperSchema(super.get()), 0);
  ArrowSchemaInit(schema.get());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 3), 0);
  ASSERT_EQ(ArrowSchemaDeepCopy(super->children[0], schema->children[0]), 0);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_BINARY), 0);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[2], NANOARROW_TYPE_INT64), 0);
  nanoarrow::UniqueArray array;
  ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
  ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
  ASSERT_EQ(ArrowArrayAppendString(array->children[0], ArrowCharView("true")), 0);
  const std::string bytes("\0\x80\xff", 3);
  ArrowBufferView buffer{};
  buffer.data.as_char = bytes.data();
  buffer.size_bytes = bytes.size();
  ASSERT_EQ(ArrowArrayAppendBytes(array->children[1], buffer), 0);
  ASSERT_EQ(ArrowArrayAppendInt(array->children[2], -71), 0);
  ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
  ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error), ADBC_STATUS_OK);
  ASSERT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error), ADBC_STATUS_OK)
      << Message();
  ASSERT_EQ(recording.executions.size(), 1U);
  const auto& values = recording.executions[0];
  ASSERT_EQ(values.size(), 3U);
  EXPECT_EQ(values[1].oid, 72112U);
  EXPECT_EQ(values[1].bytes, bytes);
  EXPECT_EQ(values[2].oid, 67009U);
  EXPECT_EQ(values[2].bytes, testing::NumericHex("ffffffffffffffb9"));
  for (const auto& value : values) EXPECT_EQ(value.format, 1);
}


struct SuperCallbackSource {
  ArrowArrayStream input{};
  bool armed = false;
  std::function<void()> schema_callback, next_callback;
};
SuperCallbackSource* SuperCallbacks(ArrowArrayStream* input, ArrowArrayStream* out) {
  auto* state = new SuperCallbackSource;
  ArrowArrayStreamMove(input, &state->input);
  out->private_data = state;
  out->get_schema = [](ArrowArrayStream* stream, ArrowSchema* schema) {
    auto* state = static_cast<SuperCallbackSource*>(stream->private_data);
    const int result = state->input.get_schema(&state->input, schema);
    if (state->armed && state->schema_callback) state->schema_callback();
    return result;
  };
  out->get_next = [](ArrowArrayStream* stream, ArrowArray* array) {
    auto* state = static_cast<SuperCallbackSource*>(stream->private_data);
    const int result = state->input.get_next(&state->input, array);
    if (state->armed && state->next_callback) state->next_callback();
    return result;
  };
  out->get_last_error = [](ArrowArrayStream* stream) {
    auto* state = static_cast<SuperCallbackSource*>(stream->private_data);
    return state->input.get_last_error(&state->input);
  };
  out->release = [](ArrowArrayStream* stream) {
    auto* state = static_cast<SuperCallbackSource*>(stream->private_data);
    if (state->input.release) state->input.release(&state->input);
    delete state;
    stream->release = nullptr;
  };
  return state;
}

TEST_F(SuperPreparedRecordingTest, CallbackContextChangesAreCheckedAfterCallbacks) {
  for (int mode = 0; mode < 3; ++mode) {
    recording.super_setting = "off";
    recording.converter_drift = 0;
    recording.encoding = "UTF8";
    Sql("INSERT INTO t VALUES ($1)");
    nanoarrow::UniqueSchema schema;
    nanoarrow::UniqueArray array;
    ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
    ASSERT_EQ(testing::SuperBatch(schema.get(), array.get(), {"true"}), 0);
    nanoarrow::UniqueArrayStream input, wrapped;
    adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
    auto* callbacks = SuperCallbacks(input.get(), wrapped.get());
    callbacks->next_callback = [mode] {
      if (mode == 0) recording.super_setting = "on";
      if (mode == 1) recording.converter_drift = 2;
      if (mode == 2) recording.encoding = "LATIN1";
    };
    ASSERT_EQ(driver.StatementBindStream(&statement, wrapped.get(), &error),
              ADBC_STATUS_OK);
    callbacks->armed = true;
    EXPECT_NE(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ADBC_STATUS_OK);
    EXPECT_TRUE(recording.executions.empty());
    EXPECT_EQ(std::count(recording.events.begin(), recording.events.end(),
                         "physical-prepare"), 0);
  }
  recording.encoding = "UTF8";
}

TEST_F(SuperPreparedRecordingTest, SchemaAndNextCallbacksCannotExecuteReentrantSql) {
  AdbcStatement other{};
  ASSERT_EQ(driver.StatementNew(&connection, &other, &error), ADBC_STATUS_OK);
  ASSERT_EQ(driver.StatementSetSqlQuery(&other, "DELETE FROM protected", &error),
            ADBC_STATUS_OK);
  for (int mode = 0; mode < 3; ++mode) {
    Sql("SELECT $1");
    nanoarrow::UniqueSchema schema;
    nanoarrow::UniqueArray array;
    ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
    ASSERT_EQ(testing::SuperBatch(schema.get(), array.get(), {"true"}), 0);
    nanoarrow::UniqueArrayStream input, wrapped;
    adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
    auto* callbacks = SuperCallbacks(input.get(), wrapped.get());
    int calls = 0;
    auto reenter = [&] {
      AdbcError nested = ADBC_ERROR_INIT;
      EXPECT_EQ(driver.StatementExecuteQuery(&other, nullptr, nullptr, &nested),
                ADBC_STATUS_INVALID_STATE);
      if (nested.release) nested.release(&nested);
      ++calls;
    };
    callbacks->schema_callback = reenter;
    callbacks->next_callback = reenter;
    ASSERT_EQ(driver.StatementBindStream(&statement, wrapped.get(), &error), ADBC_STATUS_OK);
    callbacks->armed = true;
    if (mode == 0) {
      ASSERT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
                ADBC_STATUS_OK) << Message();
    } else {
      nanoarrow::UniqueSchema result;
      const auto code = mode == 1
                            ? driver.StatementGetParameterSchema(&statement, result.get(), &error)
                            : driver.StatementExecuteSchema(&statement, result.get(), &error);
      ASSERT_EQ(code, ADBC_STATUS_OK) << Message();
    }
    EXPECT_GT(calls, 0);
    EXPECT_EQ(std::count(recording.queries.begin(), recording.queries.end(),
                         "DELETE FROM protected"), 0);
  }
  ASSERT_EQ(driver.StatementRelease(&other, &error), ADBC_STATUS_OK);
}

TEST_F(SuperPreparedRecordingTest, SchemaCallbackChangeCannotReplaceCapturedSql) {
  Sql("SELECT $1");
  nanoarrow::UniqueSchema schema;
  nanoarrow::UniqueArray array;
  ASSERT_EQ(testing::SuperSchema(schema.get()), 0);
  ASSERT_EQ(testing::SuperBatch(schema.get(), array.get(), {"true"}), 0);
  nanoarrow::UniqueArrayStream input, wrapped;
  adbc::driver::MakeArrayStream(schema.get(), array.get(), input.get());
  auto* callbacks = SuperCallbacks(input.get(), wrapped.get());
  callbacks->schema_callback = [&] {
    AdbcError nested = ADBC_ERROR_INIT;
    EXPECT_EQ(driver.StatementSetSqlQuery(&statement, "DELETE FROM t WHERE v=$1", &nested),
              ADBC_STATUS_INVALID_STATE);
    if (nested.release) nested.release(&nested);
    nested = ADBC_ERROR_INIT;
    EXPECT_EQ(driver.StatementRelease(&statement, &nested), ADBC_STATUS_INVALID_STATE);
    EXPECT_NE(statement.private_data, nullptr);
    if (nested.release) nested.release(&nested);
    nested = ADBC_ERROR_INIT;
    nanoarrow::UniqueSchema offered;
    nanoarrow::UniqueArray values;
    ASSERT_EQ(testing::SuperSchema(offered.get()), 0);
    ASSERT_EQ(testing::SuperBatch(offered.get(), values.get(), {"false"}), 0);
    EXPECT_EQ(driver.StatementBind(&statement, values.get(), offered.get(), &nested),
              ADBC_STATUS_INVALID_STATE);
    EXPECT_NE(values->release, nullptr);
    EXPECT_NE(offered->release, nullptr);
    if (nested.release) nested.release(&nested);
  };
  ASSERT_EQ(driver.StatementBindStream(&statement, wrapped.get(), &error), ADBC_STATUS_OK);
  callbacks->armed = true;
  ASSERT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error), ADBC_STATUS_OK)
      << Message();
  EXPECT_TRUE(recording.prepared_query.starts_with("SELECT "));
  Sql("DELETE FROM t WHERE v=$1");
  BindSuper({"false"});
  ASSERT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error), ADBC_STATUS_OK)
      << Message();
  EXPECT_TRUE(recording.prepared_query.starts_with("DELETE "));
}

TEST_F(SuperPreparedRecordingTest, UnmarkedNullIsNotAnExplicitSuperInputContract) {
  Sql("SELECT $1::SUPER");
  nanoarrow::UniqueSchema schema;
  ArrowSchemaInit(schema.get());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_NA), 0);
  nanoarrow::UniqueArray array;
  ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), 0);
  ASSERT_EQ(ArrowArrayStartAppending(array.get()), 0);
  ASSERT_EQ(ArrowArrayAppendNull(array->children[0], 1), 0);
  ASSERT_EQ(ArrowArrayFinishElement(array.get()), 0);
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), 0);
  ASSERT_EQ(driver.StatementBind(&statement, array.get(), schema.get(), &error), ADBC_STATUS_OK);
  EXPECT_NE(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error), ADBC_STATUS_OK);
  EXPECT_TRUE(recording.executions.empty());
  EXPECT_EQ(std::count(recording.events.begin(), recording.events.end(), "physical-prepare"), 0);
}


TEST_F(SuperPreparedRecordingTest, RepeatedProjectionRetainsDuplicateLogicalLabels) {
  Sql("SELECT $1 AS value, $1 AS value");
  recording.direct_columns = 2;
  BindSuper({"true"});
  nanoarrow::UniqueArrayStream output;
  ASSERT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
            ADBC_STATUS_OK) << Message();
  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(output->get_schema(output.get(), schema.get()), 0);
  ASSERT_EQ(schema->n_children, 2);
  EXPECT_STREQ(schema->children[0]->name, "value");
  EXPECT_STREQ(schema->children[1]->name, "value");
  nanoarrow::UniqueArray array;
  ASSERT_EQ(output->get_next(output.get(), array.get()), 0);
  ASSERT_EQ(array->n_children, 2);
  EXPECT_EQ(recording.executions.size(), 1U);
  EXPECT_EQ(recording.executions[0].size(), 1U);
  const auto first = recording.prepared_query.find(R"("json_parse"($1))");
  ASSERT_NE(first, std::string::npos);
  EXPECT_NE(recording.prepared_query.find(R"("json_parse"($1))", first + 1), std::string::npos);
}

TEST_F(SuperPreparedRecordingTest, NavigationAndCastKeepLogicalScalarOutput) {
  Sql("SELECT $1.n::BIGINT AS value");
  recording.direct_integer_projection = true;
  BindSuper({R"({"n":1})"});
  nanoarrow::UniqueArrayStream output;
  ASSERT_EQ(driver.StatementExecuteQuery(&statement, output.get(), nullptr, &error),
            ADBC_STATUS_OK) << Message();
  nanoarrow::UniqueSchema schema;
  ASSERT_EQ(output->get_schema(output.get(), schema.get()), 0);
  ASSERT_EQ(schema->n_children, 1);
  EXPECT_STREQ(schema->children[0]->name, "value");
  EXPECT_STREQ(schema->children[0]->format, "l");
  nanoarrow::UniqueArray array;
  ASSERT_EQ(output->get_next(output.get(), array.get()), 0);
  ASSERT_EQ(array->length, 1);
  EXPECT_EQ(static_cast<const int64_t*>(array->children[0]->buffers[1])[0], 1);
}

TEST_F(SuperPreparedRecordingTest, AmbiguousStringModeCannotReachTypedUserPrepare) {
  Sql("SELECT '\\' || $1 -- ' || $1 -- '\n");
  BindSuper({"true"});
  EXPECT_EQ(driver.StatementExecuteQuery(&statement, nullptr, nullptr, &error),
            ADBC_STATUS_NOT_IMPLEMENTED) << Message();
  EXPECT_TRUE(recording.executions.empty());
  EXPECT_EQ(std::count(recording.events.begin(), recording.events.end(), "logical-prepare"), 0);
  EXPECT_EQ(std::count(recording.events.begin(), recording.events.end(), "physical-prepare"), 0);
}

TEST_F(SuperPreparedRecordingTest, MixedHllResultComposesWithPhysicalSuperPlan) {
  recording.mixed_projection = true;
  for (bool stream : {false, true}) {
    recording.executions.clear();
    recording.format_requests.clear();
    Sql("SELECT $1, owned_hll");
    BindSuper({"true", std::nullopt}, stream);
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
    EXPECT_NE(recording.prepared_query.find("json_parse"), std::string::npos);
  }
}
