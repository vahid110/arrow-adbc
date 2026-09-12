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

// For #warning Please include winsock2.h before windows.h on RTools/msys2
#ifdef _WIN32
#include <winsock2.h>
#endif

#include "database.h"

#include <array>
#include <charconv>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <libpq-fe.h>
#include <nanoarrow/nanoarrow.h>

#include "driver/common/utils.h"
#include "driver/pgwire/libpq_raii.h"
#include "postgres_util.h"
#include "result_helper.h"
#include "type_discovery.h"

namespace adbcpq {

PostgresDatabase::PostgresDatabase() : open_connections_(0) {
  type_resolver_ = std::make_shared<PostgresTypeResolver>();
}

PostgresDatabase::PostgresDatabase(
    adbc::driver::pgwire::BackendProfile required_profile)
    : open_connections_(0),
      backend_profile_(required_profile),
      required_backend_(required_profile.kind) {
  type_resolver_ = std::make_shared<PostgresTypeResolver>();
}
PostgresDatabase::~PostgresDatabase() = default;

AdbcStatusCode PostgresDatabase::GetOption(const char* option, char* value,
                                           size_t* length, struct AdbcError* error) {
  std::string output;
  if (std::strcmp(option, ADBC_POSTGRESQL_OPTION_USE_COPY) == 0) {
    output = use_copy_ ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED;
  } else {
    InternalAdbcSetError(error, "[libpq] unknown database option '%s'", option);
    return ADBC_STATUS_NOT_FOUND;
  }
  if (output.size() + 1 <= *length) {
    std::memcpy(value, output.c_str(), output.size() + 1);
  }
  *length = output.size() + 1;
  return ADBC_STATUS_OK;
}
AdbcStatusCode PostgresDatabase::GetOptionBytes(const char* option, uint8_t* value,
                                                size_t* length, struct AdbcError* error) {
  return ADBC_STATUS_NOT_FOUND;
}
AdbcStatusCode PostgresDatabase::GetOptionInt(const char* option, int64_t* value,
                                              struct AdbcError* error) {
  return ADBC_STATUS_NOT_FOUND;
}
AdbcStatusCode PostgresDatabase::GetOptionDouble(const char* option, double* value,
                                                 struct AdbcError* error) {
  return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode PostgresDatabase::Init(struct AdbcError* error) {
  // Connect to initialize the version information and build the type table
  PGconn* conn = nullptr;
  RAISE_ADBC(Connect(&conn, error));

  Status status = InitVersions(conn);
  if (!status.ok()) {
    RAISE_ADBC(Disconnect(&conn, nullptr));
    return status.ToAdbc(error);
  }

  status = RebuildTypeResolver(conn);
  RAISE_ADBC(Disconnect(&conn, nullptr));
  return status.ToAdbc(error);
}

AdbcStatusCode PostgresDatabase::Release(struct AdbcError* error) {
  if (open_connections_ != 0) {
    InternalAdbcSetError(error, "%s%" PRId32 "%s", "[libpq] Database released with ",
                         open_connections_, " open connections");
    return ADBC_STATUS_INVALID_STATE;
  }
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresDatabase::SetOption(const char* key, const char* value,
                                           struct AdbcError* error) {
  if (std::strcmp(key, "uri") == 0) {
    uri_ = value;
  } else if (strcmp(key, ADBC_POSTGRESQL_OPTION_USE_COPY) == 0) {
    if (strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0) {
      use_copy_ = true;
    } else if (strcmp(value, ADBC_OPTION_VALUE_DISABLED) == 0) {
      use_copy_ = false;
    } else {
      InternalAdbcSetError(error, "[libpq] Invalid value for option %s=%s", key, value);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
  } else {
    InternalAdbcSetError(error, "%s%s", "[libpq] Unknown database option ", key);
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresDatabase::SetOptionBytes(const char* key, const uint8_t* value,
                                                size_t length, struct AdbcError* error) {
  InternalAdbcSetError(error, "%s%s", "[libpq] Unknown option ", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode PostgresDatabase::SetOptionDouble(const char* key, double value,
                                                 struct AdbcError* error) {
  InternalAdbcSetError(error, "%s%s", "[libpq] Unknown option ", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode PostgresDatabase::SetOptionInt(const char* key, int64_t value,
                                              struct AdbcError* error) {
  InternalAdbcSetError(error, "%s%s", "[libpq] Unknown option ", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode PostgresDatabase::Connect(PGconn** conn, struct AdbcError* error) {
  if (uri_.empty()) {
    InternalAdbcSetError(
        error, "%s",
        "[libpq] Must set database option 'uri' before creating a connection");
    return ADBC_STATUS_INVALID_STATE;
  }
  adbc::driver::pgwire::UniqueConnection connection(PQconnectdb(uri_.c_str()));
  if (PQstatus(connection.get()) != CONNECTION_OK) {
    InternalAdbcSetError(error, "%s%s",
                         "[libpq] Failed to connect: ",
                         PQerrorMessage(connection.get()));
    return ADBC_STATUS_IO;
  }
  *conn = connection.release();
  open_connections_++;
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresDatabase::Disconnect(PGconn** conn, struct AdbcError* error) {
  adbc::driver::pgwire::UniqueConnection connection(*conn);
  *conn = nullptr;
  if (--open_connections_ < 0) {
    InternalAdbcSetError(error, "%s", "[libpq] Open connection count underflowed");
    return ADBC_STATUS_INTERNAL;
  }
  return ADBC_STATUS_OK;
}

namespace {

// Parse an individual version in the form of "xxx.xxx.xxx".
// If the version components aren't numeric, they will be zero.
std::array<int, 3> ParseVersion(std::string_view version) {
  std::array<int, 3> out{};
  size_t component = 0;
  size_t component_begin = 0;
  size_t component_end = 0;

  // While there are remaining version components and we haven't reached the end of the
  // string
  while (component_begin < version.size() && component < out.size()) {
    // Find the next character that marks a version component separation or the end of the
    // string
    component_end = version.find_first_of(".-", component_begin);
    if (component_end == version.npos) {
      component_end = version.size();
    }

    // Try to parse the component as an integer (assigning zero if this fails)
    int value = 0;
    std::from_chars(version.data() + component_begin, version.data() + component_end,
                    value);
    out[component] = value;

    // Move on to the next component
    component_begin = component_end + 1;
    component_end = component_begin;
    component++;
  }

  return out;
}

// Parse the PostgreSQL version() string that looks like:
// PostgreSQL 8.0.2 on i686-pc-linux-gnu, compiled by GCC gcc (GCC) 3.4.2 20041017 (Red
// Hat 3.4.2-6.fc3), Redshift 1.0.77467
std::array<int, 3> ParsePrefixedVersion(std::string_view version_info,
                                        std::string_view prefix) {
  size_t pos = version_info.find(prefix);
  if (pos == version_info.npos) {
    return {0, 0, 0};
  }

  // Skip the prefix and any leading whitespace
  pos = version_info.find_first_not_of(' ', pos + prefix.size());
  if (pos == version_info.npos) {
    return {0, 0, 0};
  }

  return ParseVersion(version_info.substr(pos));
}

}  // namespace

Status PostgresDatabase::InitVersions(PGconn* conn) {
  PqResultHelper helper(conn, "SELECT version();");
  UNWRAP_STATUS(helper.Execute());
  if (helper.NumRows() != 1 || helper.NumColumns() != 1) {
    return Status::Internal("Expected 1 row and 1 column for SELECT version(); but got ",
                            helper.NumRows(), "/", helper.NumColumns());
  }

  std::string_view version_info = helper.Row(0)[0].value();
  const auto detected_profile =
      adbc::driver::pgwire::DetectBackendProfile(version_info);
  if (required_backend_.has_value() && detected_profile.kind != *required_backend_) {
    return Status::InvalidArgument("[libpq] Expected ", backend_profile_.name,
                                   " but connected to ", detected_profile.name);
  }
  backend_profile_ = detected_profile;
  const std::string_view version_prefix =
      backend_profile_.kind == adbc::driver::pgwire::BackendKind::kRedshift
          ? "Redshift"
          : "PostgreSQL";
  postgres_server_version_ = ParsePrefixedVersion(version_info, version_prefix);

  return Status::Ok();
}

Status PostgresDatabase::RebuildTypeResolver(PGconn* conn) {
  return DiscoverPostgresTypes(conn, backend_profile_, &type_resolver_);
}

}  // namespace adbcpq
