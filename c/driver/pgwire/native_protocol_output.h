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

#include "native_protocol_capture.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace adbc::qualification {
// Noninstalled POSIX-only plumbing output. Borrow fd; never close/reopen/truncate.
// Selected source already produced canonical Observation.json via CaptureResult.
class ObservationOutput {
 public:
  static constexpr uint64_t kMaximumBytes = 4 * 1024 * 1024;
  static constexpr std::array<ProbeId, 7> kOrder = {ProbeId::kFloatBitsValuesText,
                                                    ProbeId::kFloatBitsValuesBinary,
                                                    ProbeId::kFloatBitsNullsBinary,
                                                    ProbeId::kFloatBitsEmptyBinary,
                                                    ProbeId::kFloatBitsValuesDescribe,
                                                    ProbeId::kFloatBitsParameterDescribe,
                                                    ProbeId::kPlumbingError};
  // Only the standalone unit test supplies fault-injection callbacks.
  struct Calls {
    ssize_t (*write)(int, const void*, size_t);
    int (*sync)(int);
  };
  explicit ObservationOutput(int fd, uint64_t maximum = kMaximumBytes);
  ObservationOutput(int fd, uint64_t maximum, Calls calls);
  ObservationOutput(const ObservationOutput&) = delete;
  ObservationOutput& operator=(const ObservationOutput&) = delete;
  static bool ParseDescriptor(std::string_view literal, int* out) noexcept;
  bool Initialize() noexcept;
  bool Record(ProbeId id, std::string_view json) noexcept;
  std::optional<std::string> Complete(bool cleanup_acknowledged, int libpq_version,
                                      int server_version) noexcept;

 private:
  bool Check() noexcept;
  bool Write(std::string_view bytes) noexcept;
  bool Fail() noexcept;
  int fd_;
  uint64_t maximum_, bytes_ = 0;
  size_t records_ = 0;
  Calls calls_;
  struct stat identity_ {};
  bool initialized_ = false, failed_ = false, completed_ = false;
};
}  // namespace adbc::qualification
