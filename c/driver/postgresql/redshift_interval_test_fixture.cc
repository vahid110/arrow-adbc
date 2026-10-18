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

// Test-only C Data producer, never linked into or installed with a driver artifact.
#include "redshift_interval_test_fixture.h"

#if defined(_WIN32)
#define INTERVAL_FIXTURE_EXPORT __declspec(dllexport)
#else
#define INTERVAL_FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" INTERVAL_FIXTURE_EXPORT int AdbcRedshiftIntervalFixture(int empty,
                                                                   ArrowArray* array,
                                                                   ArrowSchema* schema) {
  ArrowError error{};
  return adbcpq::testing::IntervalFixture(empty, array, schema, &error);
}
