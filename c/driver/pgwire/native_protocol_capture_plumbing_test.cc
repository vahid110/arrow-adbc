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

// Root-only ordinary PostgreSQL test. Never installed or registered in CTest.
// No native vendor type or backend identity override is involved.
#include "libpq_raii.h"
#include "native_protocol_capture.h"

#include <sys/stat.h>
#include <unistd.h>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

extern char** environ;
namespace {
using namespace adbc::qualification;  // NOLINT(build/namespaces)
using adbc::driver::pgwire::UniqueConnection;
using adbc::driver::pgwire::UniqueResult;
bool Identifier(std::string_view s) {
  if (s.empty() || s.size() > 63) return false;
  for (char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '_'))
      return false;
  return true;
}
struct Fixture {
  PGconn* conn = nullptr;
  std::string schema;
  bool created = false, removed = false, cleanup_attempted = false;
  bool Cleanup() noexcept {
    if (!created || removed) return true;
    if (cleanup_attempted) return false;
    cleanup_attempted = true;
    try {
      const auto sql = "DROP SCHEMA \"" + schema + "\" CASCADE";
      UniqueResult result(
          PQexecParams(conn, sql.c_str(), 0, nullptr, nullptr, nullptr, nullptr, 0));
      removed = result && PQresultStatus(result.get()) == PGRES_COMMAND_OK;
      return removed;
    } catch (...) {
      return false;
    }
  }
  ~Fixture() {
    if (!Cleanup()) std::cerr << "ordinary_postgresql_plumbing cleanup_failed\n";
  }
};
bool Observe(PGconn* conn, ProbeId id, const std::string& schema) {
  auto plan = BuildProbe(id, schema);
  if (!plan) return false;
  UniqueResult result;
  if (plan->operation == Operation::kDescribe) {
    UniqueResult prepared(PQprepare(conn, "", plan->sql.c_str(), 0, nullptr));
    if (!prepared || PQresultStatus(prepared.get()) != PGRES_COMMAND_OK) return false;
    result.reset(PQdescribePrepared(conn, ""));
  } else
    result.reset(PQexecParams(conn, plan->sql.c_str(), 0, nullptr, nullptr, nullptr,
                              nullptr, *plan->requested_format));
  Observation out;
  const auto status = CaptureResult(id, result.get(), &out);
  if (id == ProbeId::kPlumbingError) {
    if (status != CaptureStatus::kResultStatus || !out.present || out.complete ||
        out.sqlstate != "22012" || !out.rows.empty())
      return false;
    if (out.json.find("division by zero") != std::string::npos ||
        out.json.find("SELECT") != std::string::npos)
      return false;
  } else {
    if (status != CaptureStatus::kComplete || !out.complete || !out.present) return false;
    if (out.fields.size() != 3 || out.fields[0].oid != 20 || out.fields[1].oid != 700 ||
        out.fields[2].oid != 701)
      return false;
    for (size_t i = 0; i < out.fields.size(); ++i)
      if (!out.fields[i].table_oid ||
          out.fields[i].table_column != static_cast<int>(i) + 1 ||
          out.fields[i].modifier != -1)
        return false;
    if (id == ProbeId::kFloatBitsParameterDescribe &&
        (!out.parameters || *out.parameters != std::vector<uint32_t>{20}))
      return false;
    if (id == ProbeId::kFloatBitsParameterDescribe) {
      CaptureLimits zero_parameters;
      zero_parameters.parameters = 0;
      Observation limited = out;
      if (CaptureResult(id, result.get(), &limited, zero_parameters) !=
              CaptureStatus::kParameterLimit ||
          limited.complete || !limited.rows.empty())
        return false;
      if (CaptureResult(ProbeId::kFloatBitsValuesDescribe, result.get(), &limited) !=
              CaptureStatus::kParameterCount ||
          limited.complete)
        return false;
    }
    if (id == ProbeId::kFloatBitsNullsBinary &&
        (out.rows.size() != 1 || !out.rows[0][1].is_null || !out.rows[0][2].is_null))
      return false;
    if (id == ProbeId::kFloatBitsEmptyBinary && out.row_count != 0) return false;
  }
  std::cout << "F0_PLUMBING_OBSERVATION=" << out.json << "\n";
  return true;
}
}  // namespace
int main(int argc, char** argv) {
  const char* phase = "input";
  try {
    alarm(30);
    if (argc != 11) return 2;
    const char* keys[] = {"--socket-dir", "--port", "--database", "--user", "--schema"};
    for (int i = 0; i < 5; ++i)
      if (std::strcmp(argv[i * 2 + 1], keys[i])) return 2;
    for (char** var = environ; *var; ++var)
      if ((*var)[0] == 'P' && (*var)[1] == 'G') return 2;
    std::string socket_dir = argv[2], port = argv[4], database = argv[6], user = argv[8],
                schema = argv[10];
    if (socket_dir.empty() || socket_dir[0] != '/' || socket_dir.size() > 1024 ||
        !Identifier(database) || !Identifier(user) || !ValidOwnedSchemaSyntax(schema))
      return 2;
    int numeric_port = 0;
    auto number = std::from_chars(port.data(), port.data() + port.size(), numeric_port);
    if (number.ec != std::errc() || number.ptr != port.data() + port.size() ||
        numeric_port <= 0 || numeric_port > 65535 || numeric_port == 5432 ||
        std::to_string(numeric_port) != port)
      return 2;
    char* resolved = realpath(socket_dir.c_str(), nullptr);
    if (!resolved) return 2;
    const bool canonical = socket_dir == resolved;
    std::free(resolved);
    if (!canonical) return 2;
    struct stat directory {
    }, socket{};
    const auto socket_file = socket_dir + "/.s.PGSQL." + port;
    if (lstat(socket_dir.c_str(), &directory) || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != geteuid() || (directory.st_mode & 0077) ||
        lstat(socket_file.c_str(), &socket) || !S_ISSOCK(socket.st_mode) ||
        socket.st_uid != geteuid())
      return 2;
    const char* parameters[] = {"host",
                                "port",
                                "dbname",
                                "user",
                                "password",
                                "passfile",
                                "sslmode",
                                "gssencmode",
                                "channel_binding",
                                "connect_timeout",
                                "options",
                                "application_name",
                                nullptr};
    const char* values[] = {socket_dir.c_str(),
                            port.c_str(),
                            database.c_str(),
                            user.c_str(),
                            "",
                            "/dev/null",
                            "disable",
                            "disable",
                            "disable",
                            "3",
                            "-c statement_timeout=3000 -c lock_timeout=1000",
                            "adbc_protocol_observation_test",
                            nullptr};
    phase = "connect";
    UniqueConnection conn(PQconnectdbParams(parameters, values, 0));
    if (!conn || PQstatus(conn.get()) != CONNECTION_OK) throw 1;
    PQsetNoticeProcessor(conn.get(), [](void*, const char*) {}, nullptr);
    std::cout << "ordinary_postgresql_plumbing libpq=" << PQlibVersion()
              << " server=" << PQserverVersion(conn.get()) << "\n";
    Fixture fixture{conn.get(), schema};
    phase = "fixture_create";
    const auto create = "CREATE SCHEMA \"" + schema + "\"";
    UniqueResult result(PQexecParams(conn.get(), create.c_str(), 0, nullptr, nullptr,
                                     nullptr, nullptr, 0));
    if (!result || PQresultStatus(result.get()) != PGRES_COMMAND_OK) throw 1;
    fixture.created = true;
    const auto table = "\"" + schema + "\".\"float_bits\"";
    const auto ddl = "CREATE TABLE " + table +
                     "(case_id BIGINT,real_value REAL,double_value DOUBLE PRECISION)";
    result.reset(
        PQexecParams(conn.get(), ddl.c_str(), 0, nullptr, nullptr, nullptr, nullptr, 0));
    if (!result || PQresultStatus(result.get()) != PGRES_COMMAND_OK) throw 1;
    const auto insert =
        "INSERT INTO " + table + " VALUES(1,1.25,2.5),(2,-1.25,-2.5),(3,NULL,NULL)";
    result.reset(PQexecParams(conn.get(), insert.c_str(), 0, nullptr, nullptr, nullptr,
                              nullptr, 0));
    if (!result || PQresultStatus(result.get()) != PGRES_COMMAND_OK) throw 1;
    phase = "fixed_observation";
    for (auto id : {ProbeId::kFloatBitsValuesText, ProbeId::kFloatBitsValuesBinary,
                    ProbeId::kFloatBitsNullsBinary, ProbeId::kFloatBitsEmptyBinary,
                    ProbeId::kFloatBitsValuesDescribe,
                    ProbeId::kFloatBitsParameterDescribe, ProbeId::kPlumbingError})
      if (!Observe(conn.get(), id, schema)) throw 1;
    phase = "cleanup";
    if (!fixture.Cleanup()) throw 1;
    std::cout << "ordinary_postgresql_plumbing complete cleanup_acknowledged\n";
    return 0;
  } catch (...) {
    std::cerr << "ordinary_postgresql_plumbing failure_phase=" << phase << "\n";
    return 1;
  }
}
