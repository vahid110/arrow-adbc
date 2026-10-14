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

// Loads a driver by name through an ADBC driver manifest found in a directory
// listed in ADBC_DRIVER_PATH. Compile against only the installed driver manager.

#include <stdio.h>

#include <arrow-adbc/adbc.h>
#include <arrow-adbc/adbc_driver_manager.h>

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s DRIVER_NAME\n", argv[0]);
    return 2;
  }
  struct AdbcError error = ADBC_ERROR_INIT;
  struct AdbcDriver driver = {0};
  AdbcStatusCode status =
      AdbcFindLoadDriver(argv[1], NULL, ADBC_VERSION_1_1_0, ADBC_LOAD_FLAG_SEARCH_ENV,
                         NULL, &driver, &error);
  if (status != ADBC_STATUS_OK) {
    fprintf(stderr, "could not load %s by name (status %d): %s\n", argv[1], (int)status,
            error.message ? error.message : "no message");
    if (error.release) error.release(&error);
    return 1;
  }
  if (driver.release && driver.release(&driver, &error) != ADBC_STATUS_OK) {
    fprintf(stderr, "could not release %s: %s\n", argv[1],
            error.message ? error.message : "no message");
    if (error.release) error.release(&error);
    return 1;
  }
  printf("loaded the %s driver by name through its manifest\n", argv[1]);
  return 0;
}
