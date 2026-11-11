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

// Installed public C API qualification. All SQL and diagnostic labels are fixed.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>

#include <arrow-adbc/adbc.h>

#include "nanoarrow/nanoarrow.h"
#include "redshift_qualification_input.h"

#define FIELDS "si,iv,bi,num,rv,dv,ch,vc,dt,tm,tz,ts,tstz,ym,ds,b,sup,vb,geom,geog"
#define MIXED "id,h," FIELDS ",h AS sketch_again"
#define TABLE_NAME "adbc_hll_fixture"
#define DROP_HANDLE(x)                  \
  do {                                  \
    if ((x).release) (x).release(&(x)); \
  } while (0)

struct Client {
  struct AdbcConnection conn;
  struct AdbcError error;
  bool lossless;
};

// Only fixed call names, numeric statuses and a validated five-character SQLSTATE
// leave the process. Vendor messages can contain connection details or SQL.
static bool StatusOk(const char* call, int rc, const struct AdbcError* error) {
  if (!rc) return true;
  char state[6] = "-----";
  if (error) {
    bool valid = true;
    for (int i = 0; i < 5; i++) {
      char ch = error->sqlstate[i];
      if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z'))) valid = false;
    }
    if (valid) memcpy(state, error->sqlstate, 5);
  }
  fprintf(stderr, "call=%s status=%d sqlstate=%s\n", call, rc, state);
  return false;
}

static bool StreamOk(const char* call, int rc, struct ArrowArrayStream* stream) {
  if (!rc) return true;
  AdbcStatusCode status = ADBC_STATUS_OK;
  const struct AdbcError* e = AdbcErrorFromArrayStream(stream, &status);
  fprintf(stderr, "arrow_errno=%d ", rc);
  return StatusOk(call, e && status ? (int)status : -1, e);
}

static bool FieldCheck(const char* why, int field) {
  fprintf(stderr, "check=%s field=%d\n", why, field);
  return false;
}

struct Control {
  struct ArrowSchema schema;
  struct ArrowArray array;
  struct ArrowArrayView view;
};

static void ResetControl(struct Control* c) {
  ArrowArrayViewReset(&c->view);
  DROP_HANDLE(c->array);
  DROP_HANDLE(c->schema);
}

static bool EqualText(struct ArrowStringView a, struct ArrowStringView b) {
  return a.size_bytes == b.size_bytes &&
         (!a.size_bytes || !memcmp(a.data, b.data, (size_t)a.size_bytes));
}

static bool Metadata(const struct ArrowSchema* s, const char* key, const char* value) {
  struct ArrowStringView actual = {0};
  return !ArrowMetadataGetValue(s->metadata, ArrowCharView(key), &actual) &&
         EqualText(actual, ArrowCharView(value));
}

// Field aliases may differ. Compare logical metadata independent of key ordering.
static bool SameType(const struct ArrowSchema* a, const struct ArrowSchema* b) {
  if (strcmp(a->format, b->format) || a->flags != b->flags ||
      a->n_children != b->n_children || a->dictionary || b->dictionary)
    return false;
  struct ArrowMetadataReader r, other;
  if (ArrowMetadataReaderInit(&r, a->metadata) ||
      ArrowMetadataReaderInit(&other, b->metadata) ||
      r.remaining_keys != other.remaining_keys)
    return false;
  while (r.remaining_keys) {
    struct ArrowStringView key, value, actual = {0};
    if (ArrowMetadataReaderRead(&r, &key, &value) ||
        !ArrowMetadataHasKey(b->metadata, key) ||
        ArrowMetadataGetValue(b->metadata, key, &actual) || !EqualText(actual, value))
      return false;
  }
  for (int64_t i = 0; i < a->n_children; ++i) {
    if (strcmp(a->children[i]->name, b->children[i]->name) ||
        !SameType(a->children[i], b->children[i]))
      return false;
  }
  return true;
}

static bool SameCell(const struct ArrowArrayView* a, int64_t i,
                     const struct ArrowArrayView* b, int64_t j) {
  if (a->storage_type != b->storage_type || i < 0 || i >= a->length || j < 0 ||
      j >= b->length)
    return false;
  if (ArrowArrayViewIsNull(a, i) || ArrowArrayViewIsNull(b, j))
    return ArrowArrayViewIsNull(a, i) == ArrowArrayViewIsNull(b, j);
  switch (a->storage_type) {
    case NANOARROW_TYPE_STRUCT:
      if (a->n_children != b->n_children) return false;
      for (int64_t k = 0; k < a->n_children; ++k) {
        if (!SameCell(a->children[k], a->offset + i, b->children[k], b->offset + j))
          return false;
      }
      return true;
    case NANOARROW_TYPE_BOOL:
      return ArrowArrayViewGetIntUnsafe(a, i) == ArrowArrayViewGetIntUnsafe(b, j);
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_LARGE_BINARY: {
      struct ArrowBufferView x = ArrowArrayViewGetBytesUnsafe(a, i);
      struct ArrowBufferView y = ArrowArrayViewGetBytesUnsafe(b, j);
      return x.size_bytes == y.size_bytes &&
             (!x.size_bytes || !memcmp(x.data.data, y.data.data, (size_t)x.size_bytes));
    }
    default: {
      int64_t bits = a->layout.element_size_bits[1];
      if (bits <= 0 || bits % 8 || bits != b->layout.element_size_bits[1]) return false;
      size_t width = (size_t)(bits / 8);
      return !memcmp(a->buffer_views[1].data.as_uint8 + (a->offset + i) * width,
                     b->buffer_views[1].data.as_uint8 + (b->offset + j) * width, width);
    }
  }
}

static bool CloseStatement(struct Client* c, struct AdbcStatement* stmt) {
  DROP_HANDLE(c->error);
  if (!stmt->private_data) return true;
  bool ok =
      StatusOk("statement-release", AdbcStatementRelease(stmt, &c->error), &c->error);
  DROP_HANDLE(c->error);
  return ok;
}

static bool Parameters(struct Client* c, struct AdbcStatement* stmt, bool stream,
                       const int64_t* ids, int count) {
  struct ArrowSchema schema = {0};
  struct ArrowArrayStream input = {0};
  struct ArrowArray batch = {0};
  bool ok = false;
  ArrowSchemaInit(&schema);
  if (ArrowSchemaSetTypeStruct(&schema, 1) ||
      ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64))
    goto done;
  for (int i = 0; i < count; i++) {
    if (ArrowArrayInitFromSchema(&batch, &schema, NULL)) goto done;
    if (ArrowArrayStartAppending(&batch) ||
        ArrowArrayAppendInt(batch.children[0], ids[i]) ||
        ArrowArrayFinishElement(&batch) || ArrowArrayFinishBuildingDefault(&batch, NULL))
      goto done;
    if (!stream) {
      ok = count == 1 &&
           StatusOk("bind", AdbcStatementBind(stmt, &batch, &schema, &c->error),
                    &c->error);
      goto done;
    }
    if (!i) {
      struct ArrowSchema moved = {0};
      if (ArrowSchemaDeepCopy(&schema, &moved)) goto done;
      if (ArrowBasicArrayStreamInit(&input, &moved, count)) {
        DROP_HANDLE(moved);
        goto done;
      }
    }
    ArrowBasicArrayStreamSetArray(&input, i, &batch);
  }
  ok = StatusOk("bind-stream", AdbcStatementBindStream(stmt, &input, &c->error),
                &c->error);
done:
  DROP_HANDLE(batch);
  DROP_HANDLE(input);
  DROP_HANDLE(schema);
  return ok;
}

static bool Start(struct Client* c, struct AdbcStatement* stmt, const char* sql,
                  const int64_t* ids, int n, bool bind_stream,
                  struct ArrowArrayStream* out) {
  return StatusOk("statement-new", AdbcStatementNew(&c->conn, stmt, &c->error),
                  &c->error) &&
         StatusOk("set-sql", AdbcStatementSetSqlQuery(stmt, sql, &c->error), &c->error) &&
         (!ids ||
          (StatusOk("prepare", AdbcStatementPrepare(stmt, &c->error), &c->error) &&
           Parameters(c, stmt, bind_stream, ids, n))) &&
         StatusOk("execute-query", AdbcStatementExecuteQuery(stmt, out, NULL, &c->error),
                  &c->error);
}

static bool Command(struct Client* c, const char* sql, const int64_t* ids, int n) {
  struct AdbcStatement stmt = {0};
  bool ok = Start(c, &stmt, sql, ids, n, n > 1, NULL);
  return CloseStatement(c, &stmt) && ok;
}

static bool ReadControl(struct Client* c, const char* sql, struct Control* result,
                        bool empty) {
  struct AdbcStatement stmt = {0};
  struct ArrowArrayStream stream = {0};
  struct ArrowArray end = {0};
  bool ok = false;
  if (!Start(c, &stmt, sql, NULL, 0, false, &stream) ||
      !StreamOk("control-schema", stream.get_schema(&stream, &result->schema), &stream))
    goto done;
  // A zero-row result may be represented by zero-length batches followed by EOF.
  for (int n = 0; n < 16; n++) {
    if (!StreamOk("control-next", stream.get_next(&stream, &result->array), &stream))
      goto done;
    if (!result->array.release) {
      ok = empty;
      break;
    }
    if (empty && result->array.length == 0) {
      DROP_HANDLE(result->array);
      continue;
    }
    ok = !empty && result->array.length == 1 &&
         StreamOk("control-eof", stream.get_next(&stream, &end), &stream) &&
         !end.release &&
         !ArrowArrayViewInitFromSchema(&result->view, &result->schema, NULL) &&
         !ArrowArrayViewSetArray(&result->view, &result->array, NULL);
    break;
  }
done:
  DROP_HANDLE(end);
  DROP_HANDLE(stream);
  return CloseStatement(c, &stmt) && ok;
}

static bool ControlsValid(struct Client* c, const struct Control* h,
                          const struct Control* f) {
  const char* formats[] = {"s",
                           "i",
                           "l",
                           "d:38,4",
                           "f",
                           "g",
                           "u",
                           "u",
                           "tdD",
                           c->lossless ? "l" : "ttu",
                           "+s",
                           c->lossless ? "l" : "tsu:",
                           c->lossless ? "l" : "tsu:UTC",
                           "tin",
                           "tDu",
                           "b",
                           "U",
                           "z",
                           "Z",
                           "Z"};
  if (c->lossless && f->schema.n_children == 21 &&
      (!Metadata(f->schema.children[9], "ARROW:extension:name",
                 "adbc.redshift.time_us.v1") ||
       !Metadata(f->schema.children[11], "ARROW:extension:name",
                 "adbc.redshift.timestamp_us_2000.v1") ||
       !Metadata(f->schema.children[12], "ARROW:extension:name",
                 "adbc.redshift.timestamptz_us_2000.v1")))
    return false;
  if (h->schema.n_children != 1 || f->schema.n_children != 21 ||
      strcmp(h->schema.children[0]->format, "U") ||
      !Metadata(h->schema.children[0], "ARROW:extension:name",
                "adbc.redshift.hllsketch") ||
      !Metadata(h->schema.children[0], "ARROW:extension:metadata",
                "{\"version\":1,\"encoding\":\"redshift-serialized\",\"logm\":15}") ||
      ArrowArrayViewIsNull(h->view.children[0], 0) ||
      ArrowArrayViewGetBytesUnsafe(h->view.children[0], 0).size_bytes <= 0)
    return false;
  if (strcmp(f->schema.children[20]->format, "l")) return false;
  for (int i = 0; i < 20; i++) {
    if (strcmp(f->schema.children[i]->format, formats[i]) ||
        ArrowArrayViewIsNull(f->view.children[i], 0))
      return false;
  }
  struct ArrowBufferView bytes = ArrowArrayViewGetBytesUnsafe(f->view.children[17], 0);
  int64_t stamp;
  memcpy(
      &stamp,
      f->view.children[11]->buffer_views[1].data.as_int64 + f->view.children[11]->offset,
      sizeof(stamp));
  return ArrowArrayViewGetIntUnsafe(f->view.children[2], 0) ==
             INT64_C(9007199254740993) &&
         stamp == (c->lossless ? 1 : INT64_C(946684800000001)) && bytes.size_bytes == 3 &&
         !memcmp(bytes.data.data, "\x00\x80\xff", 3) &&
         Metadata(f->schema.children[16], "ARROW:extension:name",
                  "adbc.redshift.super.native_json") &&
         Metadata(f->schema.children[18], "ARROW:extension:name",
                  "adbc.redshift.geometry.ewkb") &&
         Metadata(f->schema.children[19], "ARROW:extension:name",
                  "adbc.redshift.geography.ewkb");
}

// Unknown metadata keys and values are never printed. The same-predicate controls
// still compare every key and value, including those outside the diagnostic list.
static void DescriptorDifference(const char* kind, int field,
                                 const struct ArrowSchema* actual,
                                 const struct ArrowSchema* control) {
  struct ArrowSchemaView a, b;
  int at = ArrowSchemaViewInit(&a, actual, NULL) ? -1 : (int)a.type;
  int bt = ArrowSchemaViewInit(&b, control, NULL) ? -1 : (int)b.type;
  fprintf(stderr, "schema=%s field=%d arrow_types=%d/%d", kind, field, at, bt);
  const char* keys[] = {"ADBC:redshift:type_modifier",
                        "ADBC:redshift:byte_width",
                        "ADBC:redshift:precision",
                        "ADBC:redshift:scale",
                        "ADBC:redshift:fractional_precision",
                        "ADBC:redshift:interval_raw_precision",
                        "ADBC:redshift:interval_raw_datetime_precision",
                        "ADBC:redshift:interval_raw_type",
                        "ADBC:redshift:interval_constraint_state",
                        "ADBC:redshift:interval_qualifier",
                        "ADBC:redshift:sql_type",
                        "ARROW:extension:name",
                        "ARROW:extension:metadata"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    struct ArrowStringView x = {0}, y = {0}, k = ArrowCharView(keys[i]);
    ArrowMetadataGetValue(actual->metadata, k, &x);
    ArrowMetadataGetValue(control->metadata, k, &y);
    if ((x.data != NULL) != (y.data != NULL) || !EqualText(x, y)) {
      fprintf(stderr, " changed=%s", keys[i]);
      struct ArrowStringView vals[] = {x, y};
      for (int v = 0; v < 2; v++) {
        bool numeric = vals[v].size_bytes > 0 && vals[v].size_bytes <= 11;
        for (int n = 0; numeric && n < vals[v].size_bytes; n++) {
          char ch = vals[v].data[n];
          numeric =
              (ch >= '0' && ch <= '9') || (!n && ch == '-' && vals[v].size_bytes > 1);
        }
        if (numeric)
          fprintf(stderr, "[%.*s]", (int)vals[v].size_bytes, vals[v].data);
        else
          fputs(vals[v].data ? "[present]" : "[absent]", stderr);
      }
    }
  }
  fputc('\n', stderr);
}

static bool MixedSchema(const struct ArrowSchema* schema, const struct Control* h,
                        const struct Control* f) {
  if (strcmp(schema->format, "+s") || schema->n_children != 23 ||
      h->schema.n_children != 1 || f->schema.n_children != 21)
    return FieldCheck("schema-shape", -1);
  for (int i = 0; i < 23; i++) {
    const struct ArrowSchema* ref = i == 1 || i == 22
                                        ? h->schema.children[0]
                                        : f->schema.children[i == 0 ? 20 : i - 2];
    const char* name = i == 22 ? "sketch_again" : ref->name;
    if (!name || !schema->children[i]->name || strcmp(schema->children[i]->name, name))
      return FieldCheck("schema-name", i);
    if (!SameType(schema->children[i], ref)) {
      DescriptorDifference("control-mismatch", i, schema->children[i], ref);
      return false;
    }
  }
  return true;
}

static bool MixedRows(struct Client* c, const char* sql, const int64_t* bind, int binds,
                      const int64_t* expected, int count, int min_batches,
                      const struct Control* h, const struct Control* f,
                      bool schema_only) {
  struct AdbcStatement stmt = {0};
  struct ArrowArrayStream stream = {0};
  struct ArrowSchema schema = {0};
  struct ArrowArray batch = {0};
  struct ArrowArrayView view = {0};
  int rows = 0, batches = 0, nonempty = 0;
  bool ok = false;
  if (schema_only) {
    ok =
        StatusOk("statement-new", AdbcStatementNew(&c->conn, &stmt, &c->error),
                 &c->error) &&
        StatusOk("set-sql", AdbcStatementSetSqlQuery(&stmt, sql, &c->error), &c->error) &&
        StatusOk("execute-schema", AdbcStatementExecuteSchema(&stmt, &schema, &c->error),
                 &c->error) &&
        MixedSchema(&schema, h, f);
    goto done;
  }
  if (!Start(c, &stmt, sql, bind, binds, binds > 1, &stream) ||
      !StreamOk("stream-schema", stream.get_schema(&stream, &schema), &stream) ||
      !MixedSchema(&schema, h, f) ||
      !StatusOk("array-view-schema", ArrowArrayViewInitFromSchema(&view, &schema, NULL),
                NULL))
    goto done;
  for (;;) {
    if (!StreamOk("stream-next", stream.get_next(&stream, &batch), &stream)) goto done;
    if (!batch.release) break;
    if (!StatusOk("array-view-batch", ArrowArrayViewSetArray(&view, &batch, NULL), NULL))
      goto done;
    if (++batches > 16) {
      FieldCheck("batch-cap", -1);
      goto done;
    }
    if (batch.length) nonempty++;
    for (int64_t i = 0; i < batch.length; i++, rows++) {
      int64_t j = view.offset + i;
      if (rows >= count || ArrowArrayViewIsNull(&view, i) ||
          ArrowArrayViewIsNull(view.children[0], j) ||
          ArrowArrayViewGetIntUnsafe(view.children[0], j) != expected[rows]) {
        FieldCheck("row-id-or-count", 0);
        goto done;
      }
      for (int k = 1; k < 23; k++) {
        const struct ArrowArrayView* ref =
            (k == 1 || k == 22) ? h->view.children[0] : f->view.children[k - 2];
        if (expected[rows] == 2) {
          if (!ArrowArrayViewIsNull(view.children[k], j)) {
            FieldCheck("expected-null", k);
            goto done;
          }
        } else if (!SameCell(view.children[k], j, ref, 0)) {
          FieldCheck("cell", k);
          goto done;
        }
      }
    }
    DROP_HANDLE(batch);
  }
  ok = rows == count && nonempty >= min_batches;
  if (!ok)
    fprintf(stderr, "rows=%d/%d batches=%d/min%d\n", rows, count, nonempty, min_batches);
done:
  ArrowArrayViewReset(&view);
  DROP_HANDLE(batch);
  DROP_HANDLE(schema);
  DROP_HANDLE(stream);
  return CloseStatement(c, &stmt) && ok;
}

static bool Run(struct Client* c) {
  struct Control h = {0}, f = {0}, empty_h = {0}, empty_f = {0};
  const int64_t one[] = {1}, both[] = {1, 2}, ordered[] = {1, 2, 1},
                effects[] = {1, 1, 2};
  const char* stage = "setup";
  bool ok = false;
  if (!Command(c, "SET TIMEZONE TO 'UTC'", NULL, 0) ||
      !Command(
          c,
          "CREATE TEMP TABLE " TABLE_NAME
          " (id BIGINT,h HLLSKETCH,si SMALLINT,iv INTEGER,bi BIGINT,num DECIMAL(38,4),"
          "rv REAL,dv DOUBLE PRECISION,ch CHAR(4),vc VARCHAR(16),dt DATE,tm TIME,tz "
          "TIMETZ,"
          "ts TIMESTAMP,tstz TIMESTAMPTZ,ym INTERVAL YEAR TO MONTH,ds INTERVAL DAY TO "
          "SECOND(6),"
          "b BOOLEAN,sup SUPER,vb VARBYTE(16),geom GEOMETRY,geog GEOGRAPHY)",
          NULL, 0) ||
      !Command(
          c,
          "INSERT INTO " TABLE_NAME " SELECT 1,HLL_CREATE_SKETCH(n),-7,12,"
          "9007199254740993,123456789012345678901234.1200,1.25,-2.75,'A','\303\251 ',"
          "DATE '2000-01-01',TIME '23:59:59.123456',TIMETZ '12:34:56.123456+00:00',"
          "TIMESTAMP '2000-01-01 00:00:00.000001',TIMESTAMPTZ '2000-01-01 "
          "00:00:00+00:00',"
          "INTERVAL '14' MONTH,INTERVAL '-1 02:03:04.123456' DAY TO SECOND,true,"
          "OBJECT('n',1,'a',ARRAY(true)),FROM_HEX('0080ff'),ST_GeomFromText('POINT(1 "
          "2)',4326),"
          "ST_GeogFromText('SRID=4326;POINT(1 2)') FROM (SELECT 7 AS n) AS input_values",
          NULL, 0) ||
      !Command(c, "INSERT INTO " TABLE_NAME " (id) VALUES (2)", NULL, 0))
    goto done;
  stage = "native-controls";
  if (!ReadControl(c, "SELECT h FROM " TABLE_NAME " WHERE id=1", &h, false) ||
      !ReadControl(c, "SELECT " FIELDS ",id FROM " TABLE_NAME " WHERE id=1", &f, false) ||
      !ControlsValid(c, &h, &f))
    goto done;
#define QUERY "SELECT " MIXED " FROM " TABLE_NAME
  stage = "ordinary";
  if (!MixedRows(c, QUERY " ORDER BY id", NULL, 0, both, 2, 1, &h, &f, false)) goto done;
    // Redshift can describe an empty unconstrained projection differently (e.g.
    // interval raw modifiers). Use independent equivalent empty controls, and
    // retain any populated/empty contrast for diagnosis rather than normalizing it.
#define EMPTY " WHERE 1=0"
  stage = "empty-controls";
  if (!ReadControl(c, "SELECT h FROM " TABLE_NAME EMPTY, &empty_h, true) ||
      !ReadControl(c, "SELECT " FIELDS ",id FROM " TABLE_NAME EMPTY, &empty_f, true) ||
      empty_h.schema.n_children != 1 || empty_f.schema.n_children != 21 ||
      strcmp(empty_h.schema.children[0]->format, "U") ||
      !Metadata(empty_h.schema.children[0], "ARROW:extension:name",
                "adbc.redshift.hllsketch") ||
      !Metadata(empty_h.schema.children[0], "ARROW:extension:metadata",
                "{\"version\":1,\"encoding\":\"redshift-serialized\",\"logm\":15}"))
    goto done;
  for (int k = 0; k < 21; k++) {
    if (!SameType(empty_f.schema.children[k], f.schema.children[k]))
      DescriptorDifference("empty-vs-populated", k == 20 ? 0 : k + 2,
                           empty_f.schema.children[k], f.schema.children[k]);
  }
  if (!SameType(empty_h.schema.children[0], h.schema.children[0]))
    DescriptorDifference("empty-vs-populated", 1, empty_h.schema.children[0],
                         h.schema.children[0]);
  stage = "empty";
  if (!MixedRows(c, QUERY EMPTY, NULL, 0, NULL, 0, 0, &empty_h, &empty_f, false))
    goto done;
  stage = "empty-execute-schema";
  if (!MixedRows(c, QUERY EMPTY, NULL, 0, NULL, 0, 0, &empty_h, &empty_f, true))
    goto done;
  stage = "schema-only";
  if (!MixedRows(c, QUERY, NULL, 0, NULL, 0, 0, &h, &f, true)) goto done;
  stage = "prepared-bind";
  if (!MixedRows(c, QUERY " WHERE id=$1", one, 1, one, 1, 1, &h, &f, false)) goto done;
  stage = "prepared-bind-stream";
  if (!MixedRows(c, QUERY " WHERE id=$1", ordered, 3, ordered, 3, 3, &h, &f, false))
    goto done;
  stage = "effects";
  if (!Command(c, "CREATE TEMP TABLE adbc_hll_effect (LIKE " TABLE_NAME ")", NULL, 0) ||
      !Command(c, "INSERT INTO adbc_hll_effect SELECT * FROM " TABLE_NAME " WHERE id=$1",
               ordered, 3) ||
      !MixedRows(c, "SELECT " MIXED " FROM adbc_hll_effect ORDER BY id", NULL, 0, effects,
                 3, 1, &h, &f, false))
    goto done;
  ok = true;
done:
  if (!ok) fprintf(stderr, "mixed HLL case failed (%s)\n", stage);
  ResetControl(&empty_h);
  ResetControl(&empty_f);
  ResetControl(&h);
  ResetControl(&f);
  return ok;
}

// A changed empty interval modifier is permitted only when the independent
// empty control has the same modifier. Schema-only and stream routes share this check.
static bool EmptySchemaSelfTest(void) {
  struct ArrowSchema mix = {0};
  struct Control h = {0}, f = {0};
  struct ArrowBuffer metadata;
  ArrowBufferInit(&metadata);
  ArrowSchemaInit(&mix);
  ArrowSchemaInit(&h.schema);
  ArrowSchemaInit(&f.schema);
  bool ok = false;
  if (ArrowSchemaSetTypeStruct(&mix, 23) || ArrowSchemaSetTypeStruct(&h.schema, 1) ||
      ArrowSchemaSetTypeStruct(&f.schema, 21))
    goto done;
  for (int i = 0; i < 23; i++) {
    struct ArrowSchema* ref =
        i == 1 || i == 22 ? h.schema.children[0] : f.schema.children[i == 0 ? 20 : i - 2];
    const char* name = i == 1 || i == 22 ? "h" : "col";
    if (ArrowSchemaSetType(ref, NANOARROW_TYPE_INT64) || ArrowSchemaSetName(ref, name) ||
        ArrowSchemaSetType(mix.children[i], NANOARROW_TYPE_INT64) ||
        ArrowSchemaSetName(mix.children[i], i == 22 ? "sketch_again" : name))
      goto done;
  }
  if (!MixedSchema(&mix, &h, &f) || ArrowMetadataBuilderInit(&metadata, NULL) ||
      ArrowMetadataBuilderAppend(&metadata, ArrowCharView("ADBC:redshift:type_modifier"),
                                 ArrowCharView("-1")) ||
      ArrowSchemaSetMetadata(mix.children[15], (const char*)metadata.data) ||
      SameType(mix.children[15], f.schema.children[13]) ||
      ArrowSchemaSetMetadata(f.schema.children[13], (const char*)metadata.data))
    goto done;
  ok = MixedSchema(&mix, &h, &f);
done:
  ArrowBufferReset(&metadata);
  DROP_HANDLE(mix);
  ResetControl(&h);
  ResetControl(&f);
  return ok;
}

// Exercise the same nested, variable-width and null comparison used on live streams.
static bool SelfTest(void) {
  struct Control data = {0};
  bool ok = false;
  ArrowSchemaInit(&data.schema);
  if (ArrowSchemaSetTypeStruct(&data.schema, 2) ||
      ArrowSchemaSetType(data.schema.children[0], NANOARROW_TYPE_LARGE_STRING) ||
      ArrowSchemaSetType(data.schema.children[1], NANOARROW_TYPE_INT64) ||
      ArrowArrayInitFromSchema(&data.array, &data.schema, NULL) ||
      ArrowArrayStartAppending(&data.array))
    goto done;
  const char* texts[] = {"a", "b", "a", "a"};
  for (int i = 0; i < 4; i++) {
    if (ArrowArrayAppendString(data.array.children[0], ArrowCharView(texts[i])) ||
        ArrowArrayAppendInt(data.array.children[1], i == 2 ? 2 : 1) ||
        ArrowArrayFinishElement(&data.array))
      goto done;
  }
  if (ArrowArrayAppendNull(&data.array, 1) ||
      ArrowArrayFinishBuildingDefault(&data.array, NULL) ||
      ArrowArrayViewInitFromSchema(&data.view, &data.schema, NULL) ||
      ArrowArrayViewSetArray(&data.view, &data.array, NULL))
    goto done;
  ok = SameCell(&data.view, 0, &data.view, 3) &&
       !SameCell(&data.view, 0, &data.view, 1) &&
       !SameCell(&data.view, 0, &data.view, 2) &&
       !SameCell(&data.view, 0, &data.view, 4) &&
       SameCell(&data.view, 4, &data.view, 4) && !SameCell(&data.view, 0, &data.view, 5);
  // An Arrow struct slice uses the parent's offset when indexing its children.
  data.view.offset = 2;
  data.view.length = 3;
  ok = ok && !SameCell(&data.view, 0, &data.view, 1) &&
       SameCell(&data.view, 2, &data.view, 2);
done:
  ResetControl(&data);
  return ok;
}

int main(int argc, char** argv) {
  if (argc == 2 && !strcmp(argv[1], "--self-test"))
    return SelfTest() && EmptySchemaSelfTest() ? 0 : 1;
  if (argc != 3 || argv[1][0] != '/' ||
      (strcmp(argv[2], "arrow") && strcmp(argv[2], "lossless"))) {
    fprintf(stderr,
            "usage: adbc_redshift_mixed_hll /absolute/driver-library arrow|lossless\n");
    return 2;
  }
  alarm(180);
  struct Client client = {0};
  client.lossless = !strcmp(argv[2], "lossless");
  struct AdbcDatabase db = {0};
  char uri[URI_LIMIT + 1] = {0};
  bool ok =
      ReadUri(uri) && !AdbcDatabaseNew(&db, &client.error) &&
      !AdbcDatabaseSetOption(&db, "driver", argv[1], &client.error) &&
      !AdbcDatabaseSetOption(&db, "entrypoint", "AdbcDriverRedshiftInit",
                             &client.error) &&
      !AdbcDatabaseSetOption(&db, "uri", uri, &client.error) &&
      !AdbcDatabaseInit(&db, &client.error) &&
      !AdbcConnectionNew(&client.conn, &client.error) &&
      !AdbcConnectionSetOption(&client.conn, "adbc.redshift.temporal_representation",
                               argv[2], &client.error) &&
      !AdbcConnectionInit(&client.conn, &db, &client.error) && Run(&client);
  DROP_HANDLE(client.error);
  if (client.conn.private_data && AdbcConnectionRelease(&client.conn, &client.error))
    ok = false;
  DROP_HANDLE(client.error);
  if (db.private_data && AdbcDatabaseRelease(&db, &client.error)) ok = false;
  DROP_HANDLE(client.error);
  if (!ok) {
    fprintf(stderr, "installed mixed HLL qualification failed\n");
    return 1;
  }
  printf("installed mixed HLL qualification passed (%s)\n", argv[2]);
  return 0;
}
