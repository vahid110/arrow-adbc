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

// Compile this client against only an installed ADBC driver manager. It does not
// link to a database driver, so loading the installed artifact is exercised.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <arrow-adbc/adbc.h>
#include <arrow-adbc/adbc_driver_manager.h>
#include <arrow-adbc/driver/redshift.h>

static void ReleaseError(struct AdbcError* error) {
  if (error->release != NULL) error->release(error);
}

static const char* StreamError(struct ArrowArrayStream* stream) {
  if (stream->get_last_error == NULL) return "no error message";
  const char* message = stream->get_last_error(stream);
  return message == NULL ? "no error message" : message;
}

static bool VerifyScalarQuery(struct AdbcConnection* connection,
                              struct AdbcError* error) {
  struct AdbcStatement statement = {0};
  struct ArrowArrayStream stream = {0};
  struct ArrowSchema schema = {0};
  struct ArrowArray batch = {0};
  struct ArrowArray end = {0};
  bool verified = false;
  AdbcStatusCode status = AdbcStatementNew(connection, &statement, error);
  if (status != ADBC_STATUS_OK) goto adbc_error;
  status = AdbcStatementSetSqlQuery(&statement, "SELECT CAST(42 AS BIGINT)", error);
  if (status != ADBC_STATUS_OK) goto adbc_error;
  status = AdbcStatementExecuteQuery(&statement, &stream, NULL, error);
  if (status != ADBC_STATUS_OK) goto adbc_error;
  if (stream.get_schema == NULL || stream.get_next == NULL || stream.release == NULL) {
    fputs("installed ADBC driver returned an invalid query stream\n", stderr);
    goto cleanup;
  }

  if (stream.get_schema(&stream, &schema) != 0) {
    fprintf(stderr, "installed ADBC driver could not describe query: %s\n",
            StreamError(&stream));
    goto cleanup;
  }
  if (schema.n_children != 1 || schema.children == NULL || schema.children[0] == NULL ||
      schema.children[0]->format == NULL ||
      strcmp(schema.children[0]->format, "l") != 0) {
    fputs("installed ADBC driver returned an unexpected query schema\n", stderr);
    goto cleanup;
  }

  if (stream.get_next(&stream, &batch) != 0) {
    fprintf(stderr, "installed ADBC driver could not read query: %s\n",
            StreamError(&stream));
    goto cleanup;
  }
  if (batch.release == NULL || batch.length != 1 || batch.offset != 0 ||
      batch.n_children != 1 || batch.children == NULL || batch.children[0] == NULL ||
      batch.children[0]->length != 1 || batch.children[0]->null_count != 0 ||
      batch.children[0]->offset < 0 || batch.children[0]->n_buffers < 2 ||
      batch.children[0]->buffers == NULL || batch.children[0]->buffers[1] == NULL) {
    fputs("installed ADBC driver returned an unexpected query batch\n", stderr);
    goto cleanup;
  }
  int64_t value = 0;
  const struct ArrowArray* scalar = batch.children[0];
  memcpy(&value, (const uint8_t*)scalar->buffers[1] + scalar->offset * sizeof(value),
         sizeof(value));
  if (value != 42) {
    fprintf(stderr, "installed ADBC driver returned unexpected query value: %lld\n",
            (long long)value);
    goto cleanup;
  }

  if (stream.get_next(&stream, &end) != 0) {
    fprintf(stderr, "installed ADBC driver could not finish query: %s\n",
            StreamError(&stream));
    goto cleanup;
  }
  if (end.release != NULL) {
    fputs("installed ADBC driver returned more than one query batch\n", stderr);
    goto cleanup;
  }
  verified = true;
  goto cleanup;

adbc_error:
  fprintf(stderr, "installed ADBC driver query failed (status %d): %s\n", (int)status,
          error->message == NULL ? "no error message" : error->message);
cleanup:
  if (end.release != NULL) end.release(&end);
  if (batch.release != NULL) batch.release(&batch);
  if (schema.release != NULL) schema.release(&schema);
  if (stream.release != NULL) stream.release(&stream);
  if (statement.private_data != NULL) {
    status = AdbcStatementRelease(&statement, error);
    if (status != ADBC_STATUS_OK) {
      fprintf(stderr, "installed ADBC driver could not release query (status %d): %s\n",
              (int)status, error->message == NULL ? "no error message" : error->message);
      verified = false;
    }
  }
  return verified;
}

int main(int argc, char** argv) {
  if (argc != 3 && argc != 4) {
    fprintf(stderr,
            "usage: %s DRIVER_LIBRARY URI [--expect-postgresql-rejection]\n"
            "       %s DRIVER_LIBRARY --load-only\n",
            argv[0], argv[0]);
    return 2;
  }

  if (argc == 3 && strcmp(argv[2], "--load-only") == 0) {
    struct AdbcError error = ADBC_ERROR_INIT;
    struct AdbcDriver driver = {0};
    AdbcStatusCode status =
        AdbcLoadDriver(argv[1], "AdbcDriverInit", ADBC_VERSION_1_1_0, &driver, &error);
    if (status != ADBC_STATUS_OK) {
      fprintf(stderr, "installed ADBC driver could not load (status %d): %s\n",
              (int)status, error.message == NULL ? "no error message" : error.message);
    }
    if (driver.release != NULL) {
      AdbcStatusCode release_status = driver.release(&driver, &error);
      if (status == ADBC_STATUS_OK && release_status != ADBC_STATUS_OK) {
        fprintf(stderr, "installed ADBC driver could not release (status %d): %s\n",
                (int)release_status,
                error.message == NULL ? "no error message" : error.message);
        status = release_status;
      }
    }
    ReleaseError(&error);
    if (status != ADBC_STATUS_OK) return 1;
    puts("installed ADBC driver loaded successfully");
    return 0;
  }

  const bool expect_rejection =
      argc == 4 && strcmp(argv[3], "--expect-postgresql-rejection") == 0;
  if (argc == 4 && !expect_rejection) return 2;

  struct AdbcError error = ADBC_ERROR_INIT;
  struct AdbcDatabase database = {0};
  struct AdbcConnection connection = {0};
  AdbcStatusCode status = AdbcDatabaseNew(&database, &error);
  if (status == ADBC_STATUS_OK) {
    status = AdbcDatabaseSetOption(&database, "driver", argv[1], &error);
  }
  if (status == ADBC_STATUS_OK) {
    status = AdbcDatabaseSetOption(&database, "uri", argv[2], &error);
  }
  if (status == ADBC_STATUS_OK) {
    status = AdbcDatabaseInit(&database, &error);
  }

  if (expect_rejection) {
    const bool rejected = status == ADBC_STATUS_INVALID_ARGUMENT &&
                          error.message != NULL &&
                          strstr(error.message, "Expected Redshift") != NULL;
    if (!rejected) {
      fprintf(stderr, "unexpected Redshift driver result (status %d): %s\n", (int)status,
              error.message == NULL ? "no error message" : error.message);
    }
    ReleaseError(&error);
    if (database.private_data != NULL) AdbcDatabaseRelease(&database, &error);
    ReleaseError(&error);
    if (!rejected) return 1;
    puts("installed Redshift driver loaded and rejected PostgreSQL as expected");
    return 0;
  }

  if (status == ADBC_STATUS_OK) status = AdbcConnectionNew(&connection, &error);
  if (status == ADBC_STATUS_OK) {
    status = AdbcConnectionInit(&connection, &database, &error);
  }
  const bool connected = status == ADBC_STATUS_OK;
  if (!connected) {
    fprintf(stderr, "installed ADBC driver could not connect (status %d): %s\n",
            (int)status, error.message == NULL ? "no error message" : error.message);
  }
  const bool queried = connected && VerifyScalarQuery(&connection, &error);
  if (connection.private_data != NULL) AdbcConnectionRelease(&connection, &error);
  if (database.private_data != NULL) AdbcDatabaseRelease(&database, &error);
  ReleaseError(&error);
  if (!queried) return 1;
  puts("installed ADBC driver connected and queried successfully");
  return 0;
}
