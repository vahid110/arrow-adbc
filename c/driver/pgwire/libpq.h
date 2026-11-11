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

#pragma once

// All implementation translation units of the Redshift artifact use the same
// private namespace. Never pass a connection/result between this and stock libpq.
#ifdef ADBC_PGWIRE_PRIVATE_PQ
#include <adbc_rs_pq.h>
#if ADBC_REDSHIFT_PRIVATE_PQ_ABI != 1 || ADBC_REDSHIFT_PRIVATE_PQ_VERSION != 170011
#error "Unsupported private Redshift libpq ABI/version"
#endif
#else
#include <libpq-fe.h>
#endif

// Recorder-only seam: real PGresult storage, no network or installed symbols.
#ifdef ADBC_PGWIRE_RESULT_FORMATS_TEST
extern "C" PGresult* IntervalRecording_ExecPreparedFormats(PGconn*, const char*, int,
                                                           const char* const*, const int*,
                                                           const int*, int, const int*);
#endif
