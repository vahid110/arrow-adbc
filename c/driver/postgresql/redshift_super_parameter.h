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

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace adbcpq {

// Pure lexical plan only. It does not authorize a converter, assert that the SQL
// is valid, or change the direct-bind route. The eventual caller must validate
// session lexical rules and both typed server descriptions before execution.
class RedshiftSuperParameterPlan {
 public:
  struct Reference {
    size_t begin;
    size_t end;
    size_t index;  // Zero-based parameter index; repeated references stay repeated.
  };
  struct Replacement {
    size_t source_begin;
    size_t source_end;
    size_t generated_begin;
    size_t generated_end;
  };
  struct Generated {
    std::string sql;
    std::vector<Replacement> replacements;

    // PostgreSQL statement positions are one-based Unicode character offsets.
    // Only PG_DIAG_STATEMENT_POSITION may use this map, never internal positions
    // or JSON-parser offsets embedded in arbitrary diagnostic strings.
    bool OriginalPosition(std::string_view original, size_t position, size_t* out) const {
      size_t generated_byte;
      if (!ByteOffset(sql, position, &generated_byte)) return false;
      size_t previous_generated = 0, previous_source = 0;
      for (const auto& replacement : replacements) {
        if (generated_byte < replacement.generated_begin) break;
        if (generated_byte < replacement.generated_end) {
          if (replacement.source_begin > original.size()) return false;
          *out = CharacterOffset(original, replacement.source_begin);
          return true;
        }
        previous_generated = replacement.generated_end;
        previous_source = replacement.source_end;
      }
      const size_t source_byte = previous_source + generated_byte - previous_generated;
      if (source_byte > original.size()) return false;
      *out = CharacterOffset(original, source_byte);
      return true;
    }
  };

  // The caller supplies a known standard_conforming_strings value. Unknown
  // context must be rejected before calling; no default or session mutation.
  static bool Parse(std::string_view sql, size_t parameter_count,
                    bool standard_conforming_strings, RedshiftSuperParameterPlan* out,
                    std::string* error) {
    RedshiftSuperParameterPlan plan;
    plan.original_ = std::string(sql);
    plan.parameter_count_ = parameter_count;
    if (!ValidUtf8(sql)) return Fail(error, "SQL must be non-NUL valid UTF8");
    size_t pos = 0;
    bool first = true, terminal = false, previous_string = false;
    while (pos < sql.size()) {
      const auto c = static_cast<unsigned char>(sql[pos]);
      if (Space(c)) {
        ++pos;
        continue;
      }
      if (sql.substr(pos, 2) == "--") {
        pos += 2;
        while (pos < sql.size() && sql[pos] != '\n' && sql[pos] != '\r') ++pos;
        continue;
      }
      if (sql.substr(pos, 2) == "/*") {
        size_t depth = 1;
        pos += 2;
        while (pos < sql.size() && depth) {
          if (sql.substr(pos, 2) == "/*") {
            ++depth;
            pos += 2;
          } else if (sql.substr(pos, 2) == "*/") {
            --depth;
            pos += 2;
          } else {
            ++pos;
          }
        }
        if (depth) return Fail(error, "Unterminated block comment");
        continue;
      }
      if (terminal) return Fail(error, "Only one prepared statement is supported");
      if (c == ';') {
        terminal = true;
        ++pos;
        continue;
      }
      if (IdentifierStart(c)) {
        const size_t begin = pos++;
        while (pos < sql.size() && IdentifierPart(sql[pos])) ++pos;
        const std::string word = Upper(sql.substr(begin, pos - begin));
        if (first) {
          if (word != "SELECT" && word != "VALUES" && word != "INSERT" &&
              word != "UPDATE" && word != "DELETE" && word != "MERGE" && word != "WITH")
            return Fail(error, "SUPER parameters require a SELECT, VALUES or DML scope");
          first = false;
        }
        if (word == "U" && sql.substr(pos, 2) == "&\"")
          return Fail(error, "Unicode escape identifiers are not qualified");
        if (word == "U" && sql.substr(pos, 2) == "&'")
          return Fail(error, "Unicode escape strings are not qualified");
        if (word == "E" && pos < sql.size() && sql[pos] == '\'') {
          if (previous_string)
            return Fail(error, "Adjacent string segments are not qualified");
          if (!Quoted(sql, &pos, '\'', true))
            return Fail(error, "Unterminated escape string");
          previous_string = true;
        } else {
          previous_string = false;
        }
        continue;
      }
      if (first) return Fail(error, "Unqualified prepared statement scope");
      if (c == '\'' || c == '"') {
        if (c == '\'' && previous_string)
          return Fail(error, "Adjacent string segments are not qualified");
        if (!Quoted(sql, &pos, c, c == '\'' && !standard_conforming_strings))
          return Fail(error, "Unterminated quoted token");
        previous_string = c == '\'';
        continue;
      }
      previous_string = false;
      if (c == '$') {
        const size_t begin = pos;
        if (pos + 1 < sql.size() && Digit(sql[pos + 1])) {
          size_t index = 0;
          ++pos;
          while (pos < sql.size() && Digit(sql[pos])) {
            const size_t digit = sql[pos++] - '0';
            if (index > parameter_count / 10 ||
                (index == parameter_count / 10 && digit > parameter_count % 10))
              return Fail(error, "Parameter index exceeds supplied schema");
            index = index * 10 + digit;
          }
          if (!index) return Fail(error, "Parameter indices start at one");
          // A trailing identifier token is ambiguous across server lexers.
          if (pos < sql.size() && IdentifierPart(sql[pos]))
            return Fail(error, "Parameter token has an identifier suffix");
          plan.references_.push_back({begin, pos, index - 1});
          continue;
        }
        size_t end = pos + 1;
        if (end < sql.size() && IdentifierStart(sql[end])) {
          ++end;
          while (end < sql.size() && (IdentifierStart(sql[end]) || Digit(sql[end])))
            ++end;
        }
        if (end < sql.size() && sql[end] == '$') {
          const auto delimiter = sql.substr(pos, end - pos + 1);
          const size_t closing = sql.find(delimiter, end + 1);
          if (closing == std::string_view::npos)
            return Fail(error, "Unterminated dollar-quoted string");
          pos = closing + delimiter.size();
          continue;
        }
        return Fail(error, "Unqualified dollar token");
      }
      // Backslash outside a recognized quoted token has no admitted meaning.
      if (c == '\\') return Fail(error, "Unqualified backslash token");
      ++pos;
    }
    if (first) return Fail(error, "Empty prepared statement");
    *out = std::move(plan);
    return true;
  }

  // No server string-mode assumption: both lexical interpretations must find
  // the same complete parameter namespace, including unselected references.
  static bool ParseUnambiguous(std::string_view sql, size_t parameter_count,
                               RedshiftSuperParameterPlan* out, std::string* error) {
    RedshiftSuperParameterPlan standard, escaped;
    if (!Parse(sql, parameter_count, true, &standard, error) ||
        !Parse(sql, parameter_count, false, &escaped, error))
      return false;
    if (standard.references_.size() != escaped.references_.size())
      return Fail(error, "SQL parameter tokens depend on string escape mode");
    for (size_t i = 0; i < standard.references_.size(); ++i) {
      const auto& left = standard.references_[i];
      const auto& right = escaped.references_[i];
      if (left.begin != right.begin || left.end != right.end || left.index != right.index)
        return Fail(error, "SQL parameter tokens depend on string escape mode");
    }
    *out = std::move(standard);
    return true;
  }

  const std::string& original() const { return original_; }
  const std::vector<Reference>& references() const { return references_; }

  // Namespace/function identity must already have been verified by the caller.
  // Identifier validation prevents this internal argument becoming SQL syntax.
  // No default namespace is assumed while the built-in identity is unqualified.
  bool Convert(const std::vector<bool>& selected, std::string_view builtin_namespace,
               Generated* out, std::string* error) const {
    if (selected.size() != parameter_count_)
      return Fail(error, "Conversion schema differs from parsed parameter schema");
    if (!SimpleIdentifier(builtin_namespace))
      return Fail(error, "Converter identity must contain simple identifiers");
    Generated generated;
    size_t copied = 0;
    for (const auto& reference : references_) {
      if (!selected[reference.index]) continue;
      generated.sql.append(original_, copied, reference.begin - copied);
      const size_t begin = generated.sql.size();
      generated.sql += "(\"";
      generated.sql += builtin_namespace;
      generated.sql += "\".\"";
      generated.sql += "json_parse";
      generated.sql += "\"(";
      generated.sql.append(original_, reference.begin, reference.end - reference.begin);
      generated.sql += "))";
      generated.replacements.push_back(
          {reference.begin, reference.end, begin, generated.sql.size()});
      copied = reference.end;
    }
    generated.sql.append(original_, copied, std::string::npos);
    *out = std::move(generated);
    return true;
  }

 private:
  std::string original_;
  size_t parameter_count_ = 0;
  std::vector<Reference> references_;

  static bool Fail(std::string* error, const char* message) {
    *error = message;
    return false;
  }
  static bool Digit(unsigned char c) { return c >= '0' && c <= '9'; }
  static bool IdentifierStart(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 128;
  }
  static bool IdentifierPart(unsigned char c) {
    return IdentifierStart(c) || Digit(c) || c == '$';
  }
  static bool SimpleIdentifier(std::string_view word) {
    if (word.empty()) return false;
    for (unsigned char c : word)
      if (!((c >= 'a' && c <= 'z') || c == '_' || Digit(c))) return false;
    return !Digit(word[0]);
  }
  static bool Space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
  }
  static std::string Upper(std::string_view word) {
    std::string out(word);
    for (char& c : out)
      if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    return out;
  }
  static bool Quoted(std::string_view sql, size_t* pos, char delimiter, bool escapes) {
    ++*pos;
    while (*pos < sql.size()) {
      const char c = sql[(*pos)++];
      if (c == delimiter) {
        if (*pos < sql.size() && sql[*pos] == delimiter) {
          ++*pos;
        } else {
          return true;
        }
      } else if (c == '\\' && escapes) {
        if (*pos == sql.size()) return false;
        ++*pos;
      }
    }
    return false;
  }
  static bool ValidUtf8(std::string_view sql) {
    size_t pos = 0;
    while (pos < sql.size()) {
      const auto first = static_cast<unsigned char>(sql[pos++]);
      if (!first) return false;
      if (first < 128) continue;
      uint32_t value;
      int n;
      uint32_t minimum;
      if (first >= 0xc2 && first <= 0xdf) {
        value = first & 31;
        n = 1;
        minimum = 0x80;
      } else if (first >= 0xe0 && first <= 0xef) {
        value = first & 15;
        n = 2;
        minimum = 0x800;
      } else if (first >= 0xf0 && first <= 0xf4) {
        value = first & 7;
        n = 3;
        minimum = 0x10000;
      } else {
        return false;
      }
      while (n--) {
        if (pos == sql.size()) return false;
        const auto next = static_cast<unsigned char>(sql[pos++]);
        if ((next & 0xc0) != 0x80) return false;
        value = (value << 6) | (next & 63);
      }
      if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return false;
    }
    return true;
  }
  static size_t CharacterOffset(std::string_view sql, size_t byte) {
    size_t character = 1;
    for (size_t i = 0; i < byte; ++i)
      if ((static_cast<unsigned char>(sql[i]) & 0xc0) != 0x80) ++character;
    return character;
  }
  static bool ByteOffset(std::string_view sql, size_t character, size_t* byte) {
    if (!character) return false;
    size_t current = 1;
    for (size_t i = 0; i < sql.size(); ++i) {
      if ((static_cast<unsigned char>(sql[i]) & 0xc0) != 0x80) {
        if (current++ == character) {
          *byte = i;
          return true;
        }
      }
    }
    if (current != character) return false;
    *byte = sql.size();
    return true;
  }
};

}  // namespace adbcpq
