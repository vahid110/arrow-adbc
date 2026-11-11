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

#include <adbc_rs_pq.h>
#include <string.h>

#if ADBC_REDSHIFT_PRIVATE_PQ_ABI != 1 || ADBC_REDSHIFT_PRIVATE_PQ_VERSION != 170011
#error "Wrong private libpq package ABI/version"
#endif

int main(void) {
  const int encoding = pg_char_to_encoding("UTF8");
  return encoding < 0 || !pg_valid_server_encoding_id(encoding) ||
         strcmp(pg_encoding_to_char(encoding), "UTF8") != 0 ||
         ADBCPQprivatePackageAbi() != 1 || PQlibVersion() != 170011 ||
         ADBCPQsendQueryPreparedFormats(NULL, "", 0, NULL, NULL, NULL, 0, NULL) != 0 ||
         ADBCPQexecPreparedFormats(NULL, "", 0, NULL, NULL, NULL, 0, NULL) != NULL;
}
