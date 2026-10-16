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

// Ubuntu-only installed-client primitive. Not an admission interface. The parent
// must verify external admission before sending credentials. CI uses only the
// explicit PostgreSQL plumbing mode; that result is never Redshift evidence.
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <arrow-adbc/adbc.h>

#define URI_LIMIT 4096

static void ReleaseError(struct AdbcError* error) {
  if (error->release) error->release(error);
}

// Length-prefixed private pipe, followed by EOF. One process-wide alarm also
// bounds a writer that slowly drips bytes and all native calls including release.
static bool ReadExact(void* out, size_t size) {
  size_t used = 0;
  while (used < size) {
    struct pollfd fd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&fd, 1, 3000) <= 0) return false;
    ssize_t n = read(STDIN_FILENO, (char*)out + used, size - used);
    if (n <= 0) return false;
    used += (size_t)n;
  }
  return true;
}

static bool ReadUri(char* uri) {
  struct stat input;
  if (fstat(STDIN_FILENO, &input) != 0 || !S_ISFIFO(input.st_mode)) return false;
  unsigned char header[4];
  if (!ReadExact(header, sizeof(header))) return false;
  uint32_t size = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) |
                  ((uint32_t)header[2] << 8) | header[3];
  if (size < 16 || size > URI_LIMIT || !ReadExact(uri, size)) return false;
  uri[size] = '\0';
  for (uint32_t i = 0; i < size; i++) {
    if ((unsigned char)uri[i] <= 32 || (unsigned char)uri[i] >= 127) return false;
    if (uri[i] == '%' && (i + 2 >= size || !isxdigit((unsigned char)uri[i + 1]) ||
                          !isxdigit((unsigned char)uri[i + 2])))
      return false;
  }
  struct pollfd fd = {STDIN_FILENO, POLLIN, 0};
  char extra;
  bool end = poll(&fd, 1, 3000) > 0 && read(STDIN_FILENO, &extra, 1) == 0;
  close(STDIN_FILENO);
  return end && strncmp(uri, "postgresql://", 13) == 0;
}

static bool TargetUri(const char* uri, bool plumbing) {
  const char* host = strchr(uri, '@');
  if (!host || strchr(host + 1, '@') || strchr(uri, '#')) return false;
  const char* password = strchr(uri + 13, ':');
  if (!password || password == uri + 13 || password >= host - 1) return false;
  if (plumbing) {
    return strcmp(host, "@postgres:5432/postgres?connect_timeout=10") == 0 ||
           strcmp(host, "@127.0.0.1:5432/postgres?connect_timeout=10") == 0;
  }
  const char* expected =
      "@adbc-qualification-20261016.385207570137.eu-north-1."
      "redshift-serverless.amazonaws.com:5439/adbc_qualification?"
      "sslmode=verify-full&sslrootcert=";
  if (strncmp(host, expected, strlen(expected))) return false;
  const char* ca = host + strlen(expected);
  const char* end = strchr(ca, '&');
  if (!end || ca == end || strcmp(end, "&connect_timeout=10")) return false;
  // The URI builder percent-encodes the absolute trusted CA path.
  return strncmp(ca, "%2F", 3) == 0 && !memchr(ca, '?', (size_t)(end - ca));
}

static void ArrayRelease(struct ArrowArray* array) { array->release = NULL; }
static void SchemaRelease(struct ArrowSchema* schema) { schema->release = NULL; }

// Arrow storage remains alive until the synchronous execution and release finish.
static bool Execute(struct AdbcConnection* conn, const char* sql, bool scalar,
                    int64_t* values, int64_t count, const char* schema_name,
                    const char* mode, struct AdbcError* error) {
  struct AdbcStatement stmt = {0};
  struct ArrowArrayStream stream = {0};
  struct ArrowSchema schema = {0}, field = {0}, result_schema = {0};
  struct ArrowArray array = {0}, child = {0}, batch = {0}, end = {0};
  struct ArrowSchema* fields[] = {&field};
  struct ArrowArray* children[] = {&child};
  const void* buffers[] = {NULL, values};
  const void* parent_buffers[] = {NULL};
  bool ok = false;
  if (AdbcStatementNew(conn, &stmt, error)) goto cleanup;
  if (mode) {
    if (AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "fixture",
                               error) ||
        AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA, schema_name,
                               error) ||
        AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_MODE, mode, error))
      goto cleanup;
  } else {
    if (AdbcStatementSetSqlQuery(&stmt, sql, error)) goto cleanup;
    if (values && AdbcStatementPrepare(&stmt, error)) goto cleanup;
  }
  if (values) {
    field.format = "l";
    field.name = "value";
    field.release = SchemaRelease;
    schema.format = "+s";
    schema.n_children = 1;
    schema.children = fields;
    schema.release = SchemaRelease;
    child.length = count;
    child.n_buffers = 2;
    child.buffers = buffers;
    child.release = ArrayRelease;
    array.length = count;
    array.n_buffers = 1;
    array.buffers = parent_buffers;
    array.n_children = 1;
    array.children = children;
    array.release = ArrayRelease;
    if (AdbcStatementBind(&stmt, &array, &schema, error)) goto cleanup;
  }
  if (AdbcStatementExecuteQuery(&stmt, scalar ? &stream : NULL, NULL, error))
    goto cleanup;
  if (scalar) {
    if (!stream.release || !stream.get_schema || !stream.get_next ||
        stream.get_schema(&stream, &result_schema) || result_schema.n_children != 1 ||
        !result_schema.children || !result_schema.children[0] ||
        !result_schema.children[0]->format ||
        strcmp(result_schema.children[0]->format, "l") ||
        stream.get_next(&stream, &batch) || !batch.release || batch.length != 1 ||
        batch.offset != 0 || batch.n_children != 1 || !batch.children ||
        !batch.children[0])
      goto cleanup;
    const struct ArrowArray* result = batch.children[0];
    if (result->length != 1 || result->null_count != 0 || result->offset < 0 ||
        result->n_buffers != 2 || !result->buffers || !result->buffers[1])
      goto cleanup;
    int64_t value;
    memcpy(&value, (const int64_t*)result->buffers[1] + result->offset, sizeof(value));
    if (value != 42 || stream.get_next(&stream, &end) || end.release) goto cleanup;
  }
  ok = true;
cleanup:
  if (end.release) end.release(&end);
  if (batch.release) batch.release(&batch);
  if (result_schema.release) result_schema.release(&result_schema);
  if (stream.release) stream.release(&stream);
  if (stmt.private_data && AdbcStatementRelease(&stmt, error)) ok = false;
  if (array.release) array.release(&array);
  if (schema.release) schema.release(&schema);
  ReleaseError(error);
  return ok;
}

int main(int argc, char** argv) {
  // No URI, credential or environment fallback is accepted in argv.
  if (argc != 4 || argv[1][0] != '/' || strncmp(argv[2], "adbc_q_", 7) ||
      strlen(argv[2]) < 8 || strlen(argv[2]) > 40)
    return 2;
  for (const char* p = argv[2] + 7; *p; p++) {
    if (!isdigit((unsigned char)*p)) return 2;
  }
  bool plumbing = strcmp(argv[3], "--postgresql-plumbing") == 0;
  if (!plumbing && strcmp(argv[3], "--redshift")) return 2;
  alarm(60);
  char uri[URI_LIMIT + 1] = {0};
  if (!ReadUri(uri) || !TargetUri(uri, plumbing)) {
    fputs("Private qualification input rejected.\n", stderr);
    return 2;
  }
  struct AdbcError error = ADBC_ERROR_INIT;
  struct AdbcDatabase db = {0};
  struct AdbcConnection conn = {0};
  struct ArrowSchema table_schema = {0};
  bool ok = false, owned = false, cleanup_known = true, attempted_create = false;
  char sql[512];
  if (AdbcDatabaseNew(&db, &error) ||
      AdbcDatabaseSetOption(&db, "driver", argv[1], &error) ||
      AdbcDatabaseSetOption(
          &db, "entrypoint",
          plumbing ? "AdbcDriverPostgresqlInit" : "AdbcDriverRedshiftInit", &error) ||
      AdbcDatabaseSetOption(&db, "uri", uri, &error) || AdbcDatabaseInit(&db, &error) ||
      AdbcConnectionNew(&conn, &error) || AdbcConnectionInit(&conn, &db, &error))
    goto cleanup;
  memset(uri, 0, sizeof(uri));
  if (!Execute(&conn, "SET statement_timeout = 10000", false, NULL, 0, NULL, NULL,
               &error) ||
      !Execute(&conn, "SELECT CAST(42 AS BIGINT)", true, NULL, 0, NULL, NULL, &error))
    goto cleanup;
  int64_t bound[] = {41};
  if (!Execute(&conn, "SELECT CAST($1 AS BIGINT) + CAST(1 AS BIGINT)", true, bound, 1,
               NULL, NULL, &error))
    goto cleanup;
  snprintf(sql, sizeof(sql), "CREATE SCHEMA \"%s\"", argv[2]);
  attempted_create = true;
  if (!Execute(&conn, sql, false, NULL, 0, NULL, NULL, &error)) {
    cleanup_known = false;  // Failed/lost CREATE reply must not drop a prior schema.
    goto cleanup;
  }
  owned = true;
  int64_t initial[] = {1, 2}, append[] = {3};
  if (!Execute(&conn, NULL, false, initial, 2, argv[2], ADBC_INGEST_OPTION_MODE_CREATE,
               &error) ||
      !Execute(&conn, NULL, false, append, 1, argv[2], ADBC_INGEST_OPTION_MODE_APPEND,
               &error))
    goto cleanup;
  if (AdbcConnectionGetTableSchema(&conn, NULL, argv[2], "fixture", &table_schema,
                                   &error) ||
      table_schema.n_children != 1 || !table_schema.children ||
      !table_schema.children[0] || !table_schema.children[0]->format ||
      strcmp(table_schema.children[0]->format, "l") || !table_schema.children[0]->name ||
      strcmp(table_schema.children[0]->name, "value"))
    goto cleanup;
  snprintf(sql, sizeof(sql),
           "SELECT CAST(CASE WHEN COUNT(*) = 3 AND MIN(value) = 1 AND MAX(value) = 3 "
           "AND SUM(value) = 6 THEN 42 ELSE 0 END AS BIGINT) FROM \"%s\".fixture",
           argv[2]);
  ok = Execute(&conn, sql, true, NULL, 0, NULL, NULL, &error);
cleanup:
  memset(uri, 0, sizeof(uri));
  ReleaseError(&error);
  if (table_schema.release) table_schema.release(&table_schema);
  if (owned) {
    snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS \"%s\".fixture", argv[2]);
    if (!Execute(&conn, sql, false, NULL, 0, NULL, NULL, &error)) cleanup_known = false;
    snprintf(sql, sizeof(sql), "DROP SCHEMA \"%s\"", argv[2]);
    if (!Execute(&conn, sql, false, NULL, 0, NULL, NULL, &error)) cleanup_known = false;
  }
  if (conn.private_data && AdbcConnectionRelease(&conn, &error)) cleanup_known = false;
  ReleaseError(&error);
  if (db.private_data && AdbcDatabaseRelease(&db, &error)) cleanup_known = false;
  ReleaseError(&error);
  // Fixed whitelist only. Never expose SDK, driver or Arrow stream diagnostics.
  printf("{\"coverage\":%s,\"cleanup\":\"%s\",\"mode\":\"%s\"}\n", ok ? "true" : "false",
         cleanup_known ? (attempted_create ? "removed" : "not-created") : "unknown",
         plumbing ? "postgresql-plumbing" : "redshift");
  if (!ok || !cleanup_known) fputs("Installed qualification failed.\n", stderr);
  return ok && cleanup_known ? 0 : 1;
}
