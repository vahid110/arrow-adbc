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
// link to the Redshift driver, so loading the installed artifact is exercised.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <arrow-adbc/adbc.h>

static void ReleaseError(struct AdbcError* error) {
  if (error->release != NULL) error->release(error);
}

int main(int argc, char** argv) {
  if (argc != 3 && argc != 4) {
    fprintf(stderr, "usage: %s DRIVER_LIBRARY URI [--expect-postgresql-rejection]\n",
            argv[0]);
    return 2;
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
    fprintf(stderr, "installed Redshift driver could not connect (status %d): %s\n",
            (int)status, error.message == NULL ? "no error message" : error.message);
  }
  if (connection.private_data != NULL) AdbcConnectionRelease(&connection, &error);
  if (database.private_data != NULL) AdbcDatabaseRelease(&database, &error);
  ReleaseError(&error);
  if (!connected) return 1;
  puts("installed Redshift driver connected successfully");
  return 0;
}
