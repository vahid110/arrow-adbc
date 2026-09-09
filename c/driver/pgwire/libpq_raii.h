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

#include <memory>

#include <libpq-fe.h>

namespace adbc::driver::pgwire {

struct ConnectionDeleter {
  void operator()(PGconn* connection) const noexcept {
    if (connection != nullptr) PQfinish(connection);
  }
};

struct ResultDeleter {
  void operator()(PGresult* result) const noexcept {
    if (result != nullptr) PQclear(result);
  }
};

struct CancelDeleter {
  void operator()(PGcancel* cancel) const noexcept {
    if (cancel != nullptr) PQfreeCancel(cancel);
  }
};

using UniqueConnection = std::unique_ptr<PGconn, ConnectionDeleter>;
using UniqueResult = std::unique_ptr<PGresult, ResultDeleter>;
using UniqueCancel = std::unique_ptr<PGcancel, CancelDeleter>;

}  // namespace adbc::driver::pgwire
