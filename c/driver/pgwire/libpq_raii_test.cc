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

#include "driver/pgwire/libpq_raii.h"

#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

namespace adbc::driver::pgwire {
namespace {

static_assert(!std::is_copy_constructible_v<UniqueConnection>);
static_assert(std::is_nothrow_move_constructible_v<UniqueConnection>);
static_assert(!std::is_copy_constructible_v<UniqueResult>);
static_assert(std::is_nothrow_move_constructible_v<UniqueResult>);
static_assert(!std::is_copy_constructible_v<UniqueCancel>);
static_assert(std::is_nothrow_move_constructible_v<UniqueCancel>);

TEST(LibpqRaiiTest, NullHandlesAreSafeAndMovable) {
  UniqueConnection connection;
  UniqueResult result;
  UniqueCancel cancel;

  UniqueConnection moved_connection(std::move(connection));
  UniqueResult moved_result(std::move(result));
  UniqueCancel moved_cancel(std::move(cancel));

  EXPECT_EQ(moved_connection.get(), nullptr);
  EXPECT_EQ(moved_result.get(), nullptr);
  EXPECT_EQ(moved_cancel.get(), nullptr);
}

TEST(LibpqRaiiTest, OwnsFailedConnectionObject) {
  UniqueConnection connection(PQconnectStart("postgresql://"));
  ASSERT_NE(connection.get(), nullptr);
  EXPECT_NE(PQstatus(connection.get()), CONNECTION_OK);
}

}  // namespace
}  // namespace adbc::driver::pgwire
