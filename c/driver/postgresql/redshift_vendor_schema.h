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

#include <string_view>
#include "redshift_bytes.h"

namespace adbcpq {
enum class RedshiftEncodedKind { kNone, kGeometry, kGeography, kHll };
// Closed version1 storage contracts. A source marker is an application assertion.
struct RedshiftEncodedSchema {
  RedshiftEncodedKind kind = RedshiftEncodedKind::kNone;
  bool enabled() const { return kind != RedshiftEncodedKind::kNone; }
  bool spatial() const {
    return kind == RedshiftEncodedKind::kGeometry ||
           kind == RedshiftEncodedKind::kGeography;
  }
  static const char* Family(RedshiftEncodedKind kind) {
    switch (kind) {
      case RedshiftEncodedKind::kGeometry:
        return "GEOMETRY";
      case RedshiftEncodedKind::kGeography:
        return "GEOGRAPHY";
      case RedshiftEncodedKind::kHll:
        return "HLLSKETCH";
      default:
        return "";
    }
  }
  static const char* Extension(RedshiftEncodedKind kind) {
    switch (kind) {
      case RedshiftEncodedKind::kGeometry:
        return "adbc.redshift.geometry.ewkb";
      case RedshiftEncodedKind::kGeography:
        return "adbc.redshift.geography.ewkb";
      case RedshiftEncodedKind::kHll:
        return "adbc.redshift.hllsketch";
      default:
        return "";
    }
  }
  static const char* Metadata(RedshiftEncodedKind kind) {
    switch (kind) {
      case RedshiftEncodedKind::kGeometry:
        return R"({"version":1,"encoding":"ewkb","family":"GEOMETRY","srid":"per-value","axes":"native"})";
      case RedshiftEncodedKind::kGeography:
        return R"({"version":1,"encoding":"ewkb","family":"GEOGRAPHY","srid":"per-value","axes":"native"})";
      case RedshiftEncodedKind::kHll:
        return R"({"version":1,"encoding":"redshift-serialized","logm":15})";
      default:
        return "";
    }
  }
  static ArrowErrorCode Invalid(ArrowError* error) {
    ArrowErrorSet(error, "[Redshift] Invalid spatial/HLL schema or storage contract");
    return EINVAL;
  }
  static ArrowErrorCode Parse(const ArrowSchema* schema, bool redshift,
                              RedshiftEncodedSchema* out, ArrowError* error) {
    *out = {};
    std::optional<std::string_view> family, extension, metadata;
    NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
        schema, "ADBC:redshift:sql_type", &family, error));
    NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
        schema, "ARROW:extension:name", &extension, error));
    NANOARROW_RETURN_NOT_OK(RedshiftDecimalSchema::Metadata(
        schema, "ARROW:extension:metadata", &metadata, error));
    bool candidate = family && (*family == "GEOMETRY" || *family == "GEOGRAPHY" ||
                                *family == "HLLSKETCH");
    candidate |= extension && (extension->starts_with("adbc.redshift.geometry") ||
                               extension->starts_with("adbc.redshift.geography") ||
                               extension->starts_with("adbc.redshift.hllsketch"));
    if (!candidate) return NANOARROW_OK;
    if (!redshift || !family || !extension || !metadata) return Invalid(error);
    for (auto kind : {RedshiftEncodedKind::kGeometry, RedshiftEncodedKind::kGeography,
                      RedshiftEncodedKind::kHll}) {
      if (*family == Family(kind) && *extension == Extension(kind) &&
          *metadata == Metadata(kind))
        out->kind = kind;
    }
    ArrowSchemaView view{};
    NANOARROW_RETURN_NOT_OK(ArrowSchemaViewInit(&view, schema, error));
    if (!out->enabled()) return Invalid(error);
    const std::string_view typname = out->kind == RedshiftEncodedKind::kHll ? "hllsketch"
                                     : out->kind == RedshiftEncodedKind::kGeometry
                                         ? "geometry"
                                         : "geography";
    for (const char* key : {"POSTGRESQL:type", "ADBC:postgresql:typname"}) {
      std::optional<std::string_view> asserted;
      NANOARROW_RETURN_NOT_OK(
          RedshiftDecimalSchema::Metadata(schema, key, &asserted, error));
      if (asserted && *asserted != typname) return Invalid(error);
    }
    if (view.type != (out->spatial() ? NANOARROW_TYPE_LARGE_BINARY
                                     : NANOARROW_TYPE_LARGE_STRING) ||
        schema->dictionary || schema->n_children ||
        (schema->flags & ~static_cast<int64_t>(ARROW_FLAG_NULLABLE)))
      return Invalid(error);
    ArrowMetadataReader reader;
    NANOARROW_RETURN_NOT_OK(ArrowMetadataReaderInit(&reader, schema->metadata));
    while (reader.remaining_keys) {
      ArrowStringView k{}, v{};
      NANOARROW_RETURN_NOT_OK(ArrowMetadataReaderRead(&reader, &k, &v));
      std::string_view key(k.data, k.size_bytes);
      if (key.starts_with("ADBC:redshift:") && key != "ADBC:redshift:sql_type")
        return Invalid(error);
    }
    return NANOARROW_OK;
  }
  static ArrowErrorCode SetSchema(ArrowSchema* schema, RedshiftEncodedKind kind) {
    NANOARROW_RETURN_NOT_OK(ArrowSchemaSetType(
        schema, kind == RedshiftEncodedKind::kHll ? NANOARROW_TYPE_LARGE_STRING
                                                  : NANOARROW_TYPE_LARGE_BINARY));
    nanoarrow::UniqueBuffer metadata;
    NANOARROW_RETURN_NOT_OK(ArrowMetadataBuilderInit(metadata.get(), nullptr));
    NANOARROW_RETURN_NOT_OK(
        ArrowMetadataBuilderAppend(metadata.get(), ArrowCharView("ARROW:extension:name"),
                                   ArrowCharView(Extension(kind))));
    NANOARROW_RETURN_NOT_OK(ArrowMetadataBuilderAppend(
        metadata.get(), ArrowCharView("ARROW:extension:metadata"),
        ArrowCharView(Metadata(kind))));
    return ArrowSchemaSetMetadata(schema, reinterpret_cast<char*>(metadata->data));
  }
};
}  // namespace adbcpq
