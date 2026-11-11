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

#if defined(_WIN32) && defined(ADBC_PRIVATE_CONSUMER_SHARED)
#define CONSUMER_EXPORT __declspec(dllexport)
#else
#define CONSUMER_EXPORT
#endif

typedef int (*VersionFunction)(void);

CONSUMER_EXPORT VersionFunction adbc_private_pq_version_function(void) {
  return PQlibVersion;
}

CONSUMER_EXPORT int adbc_private_pq_probe(void) {
  PGresult* result = PQmakeEmptyPGresult(NULL, PGRES_TUPLES_OK);
  if (!result) return 1;
  int ok = PQresultStatus(result) == PGRES_TUPLES_OK;
  PQclear(result);
  return !ok || ADBCPQprivatePackageAbi() != 1 || PQlibVersion() != 170011;
}
