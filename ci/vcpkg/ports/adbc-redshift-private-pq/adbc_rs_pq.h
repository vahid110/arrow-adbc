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

#ifndef ADBC_REDSHIFT_PRIVATE_PQ_H
#define ADBC_REDSHIFT_PRIVATE_PQ_H

/* Private implementation header: do not combine with stock libpq in one TU. */
#define ADBC_REDSHIFT_PRIVATE_PQ_ABI 1
#define ADBC_REDSHIFT_PRIVATE_PQ_VERSION 170011
#include "adbc_rs_pq_namespace.h"
#if !defined(PQlibVersion) || !defined(ADBCPQprivatePackageAbi)
#error "Private namespace must be present before libpq declarations"
#endif
#include "adbc_rs_libpq_fe.h"

#ifdef __cplusplus
extern "C" {
#endif
int ADBCPQprivatePackageAbi(void);
int ADBCPQsendQueryPreparedFormats(PGconn*, const char*, int, const char* const*,
                                   const int*, const int*, int, const int*);
PGresult* ADBCPQexecPreparedFormats(PGconn*, const char*, int, const char* const*,
                                    const int*, const int*, int, const int*);
#ifdef __cplusplus
}
#endif
#endif  // ADBC_REDSHIFT_PRIVATE_PQ_H
