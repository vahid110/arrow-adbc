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

#include "native_protocol_output.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <charconv>
#include <string>
#include <system_error>

namespace adbc::qualification {
ObservationOutput::ObservationOutput(int fd, uint64_t maximum)
    : ObservationOutput(fd, maximum, {::write, ::fsync}) {}
ObservationOutput::ObservationOutput(int fd, uint64_t maximum, Calls calls)
    : fd_(fd), maximum_(maximum), calls_(calls) {}
bool ObservationOutput::ParseDescriptor(std::string_view literal, int* out) noexcept {
  if (!out || literal.empty() || literal.front() < '1' || literal.front() > '9')
    return false;
  int fd = 0;
  const auto parsed =
      std::from_chars(literal.data(), literal.data() + literal.size(), fd);
  if (parsed.ec != std::errc() || parsed.ptr != literal.data() + literal.size() ||
      fd <= 2)
    return false;
  *out = fd;
  return true;
}
bool ObservationOutput::Fail() noexcept {
  failed_ = true;
  return false;
}
bool ObservationOutput::Check() noexcept {
  struct stat info {};
  const int flags = fcntl(fd_, F_GETFL);
  if (flags < 0 || (flags & O_APPEND) || (flags & O_ACCMODE) == O_RDONLY ||
      fstat(fd_, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
      (info.st_mode & 07777) != 0600 || info.st_nlink != 1 || info.st_size < 0 ||
      static_cast<uint64_t>(info.st_size) != bytes_ ||
      lseek(fd_, 0, SEEK_CUR) != static_cast<off_t>(bytes_))
    return Fail();
  if (initialized_ &&
      (info.st_dev != identity_.st_dev || info.st_ino != identity_.st_ino))
    return Fail();
  identity_ = info;
  return true;
}
bool ObservationOutput::Initialize() noexcept {
  if (failed_ || initialized_ || completed_ || fd_ <= 2 || !maximum_ ||
      maximum_ > kMaximumBytes || !calls_.write || !calls_.sync)
    return Fail();
  if (!Check()) return false;
  initialized_ = true;
  return true;
}
bool ObservationOutput::Write(std::string_view bytes) noexcept {
  while (!bytes.empty()) {
    const ssize_t written = calls_.write(fd_, bytes.data(), bytes.size());
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0 || static_cast<size_t>(written) > bytes.size()) return Fail();
    bytes_ += static_cast<uint64_t>(written);
    bytes.remove_prefix(static_cast<size_t>(written));
  }
  return true;
}
bool ObservationOutput::Record(ProbeId id, std::string_view json) noexcept {
  if (!initialized_ || failed_ || completed_ || records_ >= kOrder.size() ||
      id != kOrder[records_] || json.size() < 2 || json.front() != '{' ||
      json.back() != '}' || json.find_first_of("\r\n") != std::string_view::npos ||
      json.find('\0') != std::string_view::npos || bytes_ >= maximum_ ||
      json.size() >= static_cast<uint64_t>(CaptureLimits{}.encoded_bytes) ||
      json.size() > maximum_ - bytes_ - 1)
    return Fail();
  if (!Check() || !Write(json) || !Write("\n") || !Check()) return false;
  ++records_;
  return true;
}
std::optional<std::string> ObservationOutput::Complete(bool cleanup_acknowledged,
                                                       int libpq_version,
                                                       int server_version) noexcept {
  if (!initialized_ || failed_ || completed_ || records_ != kOrder.size() ||
      !cleanup_acknowledged || libpq_version <= 0 || server_version <= 0) {
    Fail();
    return std::nullopt;
  }
  try {
    // Allocate all status bytes before fsync; no success if allocation fails.
    auto status =
        std::string("{\"version\":1,\"kind\":\"adbc-local-observation-complete\",") +
        "\"backend\":\"ordinary_postgresql\",\"records\":7,\"bytes\":" +
        std::to_string(bytes_) + ",\"libpq_version\":" + std::to_string(libpq_version) +
        ",\"server_version\":" + std::to_string(server_version) +
        ",\"cleanup_acknowledged\":true}";
    if (status.size() > 2048 || !Check() || calls_.sync(fd_) || !Check()) {
      Fail();
      return std::nullopt;
    }
    completed_ = true;
    return status;
  } catch (...) {
    Fail();
    return std::nullopt;
  }
}
}  // namespace adbc::qualification
