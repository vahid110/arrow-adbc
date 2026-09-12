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

#include <string>
#include <string_view>
#include <vector>

#include "driver/pgwire/backend.h"

namespace adbcpq {

class MetadataQuerySet {
 public:
  explicit MetadataQuerySet(
      const adbc::driver::pgwire::BackendProfile& backend_profile)
      : backend_profile_(backend_profile) {}

  std::string Catalogs(bool filtered) const;
  std::string Schemas(bool filtered) const;
  std::string Tables(bool filtered) const;
  std::string Columns(bool filtered) const;
  std::string Constraints(bool filtered) const;
  std::string TableSchema() const;

  bool LoadsConstraints() const {
    return backend_profile_.capabilities.metadata_constraints;
  }
  std::string TableTypesArrayLiteral(
      const std::vector<std::string_view>& table_types) const;
  std::vector<std::string> TableTypeNames() const;

 private:
  adbc::driver::pgwire::BackendProfile backend_profile_;
};

}  // namespace adbcpq
