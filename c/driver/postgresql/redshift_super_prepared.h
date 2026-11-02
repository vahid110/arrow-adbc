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

#include <charconv>
#include <string>
#include <utility>
#include <vector>

#include "redshift_super_parameter.h"
#include "redshift_vendor.h"

namespace adbcpq {

// Redshift's explicit JSON-subset input uses a logical SUPER description and a
// physical VARBYTE/JSON_PARSE expression. No live statement is cached. The caller
// finishes all callbacks and serialization before Prepare(), then executes once.
class RedshiftSuperPrepared {
 public:
  explicit RedshiftSuperPrepared(std::string sql) : original_(std::move(sql)) {}

  static constexpr const char* kCatalog =
      "SELECT n.nspname,p.proname,p.oid,p.pronargs,p.proargtypes,p.prorettype,"
      "p.proowner,p.prolang FROM pg_catalog.pg_proc p "
      "JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace "
      "WHERE n.nspname='pg_catalog' AND p.proname='json_parse' ORDER BY p.oid";

  Status Prepare(PGconn* conn, PostgresTypeResolver& resolver,
                 const std::vector<Oid>& logical, const std::vector<Oid>& physical,
                 const std::vector<bool>& selected,
                 adbc::driver::pgwire::TemporalRepresentation temporal) {
    if (!resolver.is_redshift() || logical.size() != selected.size() ||
        physical.size() != selected.size())
      return Status::InvalidArgument("[Redshift] Invalid SUPER conversion schema");
    const Oid super =
        resolver.GetVendorOID(adbc::driver::pgwire::CatalogVendorFamily::kSuper);
    const Oid varbyte = resolver.GetBytesOID(RedshiftBytesKind::kVarbyte);
    if (!super || !varbyte)
      return Status::InvalidState(
          "[Redshift] SUPER/VARBYTE catalog identity unavailable");
    bool any = false;
    for (size_t i = 0; i < selected.size(); ++i) {
      any |= selected[i];
      if (!logical[i] || !physical[i] ||
          (selected[i] ? logical[i] != super || physical[i] != varbyte
                       : logical[i] != physical[i]))
        return Status::InvalidArgument("[Redshift] Inconsistent SUPER parameter plan");
    }
    if (!any) return Status::InvalidArgument("[Redshift] No explicit SUPER parameters");
    UNWRAP_STATUS(CheckUtf8Encoding(conn));
    UNWRAP_STATUS(CheckSuperParseContext(conn));
    UNWRAP_STATUS(CheckContext(conn, super, varbyte));
    RedshiftSuperParameterPlan plan;
    std::string error;
    if (!RedshiftSuperParameterPlan::ParseUnambiguous(original_, selected.size(), &plan,
                                                      &error) ||
        !plan.Convert(selected, "pg_catalog", &generated_, &error))
      return Status::NotImplemented("[Redshift] SUPER parameter SQL: ", error);

    PqResultHelper logical_helper(conn, original_, true);
    UNWRAP_STATUS(logical_helper.Prepare(logical));
    UNWRAP_STATUS(logical_helper.DescribePrepared());
    UNWRAP_STATUS(logical_helper.CheckParameters(logical));
    if (logical_) UNWRAP_STATUS(logical_helper.CheckDescriptor(logical_.get(), 0));
    adbc::driver::pgwire::UniqueResult described(logical_helper.CopyDescriptor());
    if (!described)
      return Status::Internal("[Redshift] Could not snapshot logical SUPER description");
    PostgresType output;
    UNWRAP_STATUS(logical_helper.ResolveOutputTypes(resolver, &output, temporal));
    UNWRAP_STATUS(SelectVendorResultFormat(output, &result_format_));

    PqResultHelper physical_helper(conn, generated_.sql, true);
    auto status = physical_helper.Prepare(physical);
    if (!status.ok()) return Decorate(std::move(status));
    status = physical_helper.DescribePrepared();
    if (!status.ok()) return Decorate(std::move(status));
    UNWRAP_STATUS(physical_helper.CheckParameters(physical));
    adbc::driver::pgwire::UniqueResult actual(physical_helper.CopyDescriptor());
    UNWRAP_STATUS(Compatible(described.get(), actual.get()));
    logical_ = std::move(described);
    physical_ = std::move(actual);
    output_ = std::move(output);
    return Status::Ok();
  }

  int result_format() const { return result_format_; }
  const PostgresType& output() const { return output_; }
  PGresult* CopyPhysicalDescriptor() const {
    return PQcopyResult(physical_.get(), PG_COPYRES_ATTRS);
  }
  Status CheckResult(PqResultHelper& helper) const {
    return helper.CheckDescriptor(physical_.get(), result_format_);
  }
  Status Decorate(Status status) const {
    // Preserve every original libpq detail. The additional position has a
    // distinct name, so internal/JSON positions and original provenance survive.
    for (int i = 0; i < status.CDetailCount(); ++i) {
      const auto detail = status.CDetail(i);
      if (std::string_view(detail.key) != "PG_DIAG_STATEMENT_POSITION") continue;
      const auto* data = reinterpret_cast<const char*>(detail.value);
      size_t position = 0, mapped;
      auto parsed = std::from_chars(data, data + detail.value_length, position);
      if (parsed.ec == std::errc() && parsed.ptr == data + detail.value_length &&
          generated_.OriginalPosition(original_, position, &mapped))
        status.AddDetail("ADBC:redshift:original_statement_position",
                         std::to_string(mapped));
      break;
    }
    return status;
  }

 private:
  std::string original_;
  RedshiftSuperParameterPlan::Generated generated_;
  adbc::driver::pgwire::UniqueResult logical_, physical_;
  PostgresType output_;
  int result_format_ = 1;

  static Status Compatible(const PGresult* logical, const PGresult* physical) {
    if (!logical || !physical || PQnfields(logical) != PQnfields(physical))
      return Status::InvalidState("[Redshift] SUPER result count changed");
    for (int i = 0; i < PQnfields(logical); ++i)
      if (PQftype(logical, i) != PQftype(physical, i) ||
          PQfmod(logical, i) != PQfmod(physical, i) ||
          PQftable(logical, i) != PQftable(physical, i) ||
          PQftablecol(logical, i) != PQftablecol(physical, i) ||
          PQfformat(logical, i) != 0 || PQfformat(physical, i) != 0)
        return Status::InvalidState("[Redshift] SUPER result identity changed");
    return Status::Ok();
  }
  static Status CheckContext(PGconn* conn, Oid super, Oid varbyte) {
    PqResultHelper catalog(conn, kCatalog, true);
    UNWRAP_STATUS(catalog.Execute());
    if (catalog.NumColumns() != 8 || catalog.NumRows() < 1 || catalog.NumRows() > 8)
      return Status::InvalidState("[Redshift] JSON_PARSE catalog is unqualified");
    int matches = 0;
    for (auto row : catalog) {
      for (int i = 0; i < 8; ++i)
        if (row[i].is_null)
          return Status::InvalidState("[Redshift] NULL JSON_PARSE catalog identity");
      if (row[0].value() != "pg_catalog" || row[1].value() != "json_parse")
        return Status::InvalidState("[Redshift] JSON_PARSE namespace changed");
      if (row[3].value() != "1" || row[4].value() != std::to_string(varbyte)) continue;
      if (row[5].value() != std::to_string(super))
        return Status::InvalidState("[Redshift] JSON_PARSE result identity changed");
      for (int col : {2, 6, 7}) {
        int64_t value;
        UNWRAP_RESULT(value, row[col].ParseInteger());
        if (value <= 0 || value > UINT32_MAX)
          return Status::InvalidState("[Redshift] Invalid JSON_PARSE catalog identity");
      }
      ++matches;
    }
    if (matches != 1)
      return Status::InvalidState(
          "[Redshift] Qualified JSON_PARSE(VARBYTE) is unavailable");
    return Status::Ok();
  }
};

}  // namespace adbcpq
