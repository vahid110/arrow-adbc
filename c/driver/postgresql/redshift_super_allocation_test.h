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

#include <cstdlib>
#include <new>

// Included by one test TU per executable, never production or installed fixtures.
// A single failed allocation lets subsequent status construction and rollback run.
namespace adbcpq::testing {
inline thread_local int super_allocation_fail_after = -1;
}  // namespace adbcpq::testing

void* operator new(size_t size) {
  auto& remaining = adbcpq::testing::super_allocation_fail_after;
  if (remaining == 0) {
    remaining = -1;
    throw std::bad_alloc();
  }
  if (remaining > 0) --remaining;
  if (void* out = std::malloc(size ? size : 1)) return out;
  throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
