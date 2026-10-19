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
#include <gtest/gtest.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

namespace adbc::qualification {
namespace {
struct File {
  char name[64] = "/tmp/adbc-observation-output-XXXXXX";
  int fd = mkstemp(name);
  File() {
    if (fd < 0) std::abort();
  }
  ~File() {
    close(fd);
    unlink(name);
  }
  uint64_t Size() {
    struct stat s {};
    return fstat(fd, &s) ? UINT64_MAX : s.st_size;
  }
  std::string Contents() {
    std::string result(Size(), '\0');
    if (pread(fd, result.data(), result.size(), 0) != static_cast<ssize_t>(result.size()))
      return "read-failed";
    return result;
  }
};
std::string Json(size_t size) { return "{\"x\":\"" + std::string(size - 8, 'x') + "\"}"; }
bool Seven(ObservationOutput* out) {
  for (auto id : ObservationOutput::kOrder)
    if (!out->Record(id, "{}")) return false;
  return true;
}
TEST(LocalObservationOutput, CanonicalDescriptorOnly) {
  for (const auto* s : {"", "0", "1", "2", "03", "-3", "+3", " 3", "3 ", "3x",
                        "2147483648", "999999999999999999999999"}) {
    int out = -1;
    EXPECT_FALSE(ObservationOutput::ParseDescriptor(s, &out));
    EXPECT_EQ(out, -1);
  }
  int out = -1;
  EXPECT_TRUE(ObservationOutput::ParseDescriptor("3", &out));
  EXPECT_EQ(out, 3);
  EXPECT_FALSE(ObservationOutput::ParseDescriptor("3", nullptr));
}
TEST(LocalObservationOutput, ValidBorrowedDescriptorAndSingleCompletion) {
  File f;
  {
    ObservationOutput output(f.fd);
    ASSERT_TRUE(output.Initialize());
    ASSERT_TRUE(Seven(&output));
    const auto status = output.Complete(true, 170011, 170011);
    ASSERT_TRUE(status);
    EXPECT_EQ(*status,
              "{\"version\":1,\"kind\":\"adbc-local-observation-complete\","
              "\"backend\":\"ordinary_postgresql\",\"records\":7,\"bytes\":21,"
              "\"libpq_version\":170011,\"server_version\":170011,\"cleanup_"
              "acknowledged\":true}");
    EXPECT_FALSE(output.Complete(true, 170011, 170011));
    EXPECT_FALSE(output.Record(ObservationOutput::kOrder[0], "{}"));
  }
  EXPECT_GE(fcntl(f.fd, F_GETFD), 0);
  EXPECT_EQ(f.Contents(), "{}\n{}\n{}\n{}\n{}\n{}\n{}\n");
}
TEST(LocalObservationOutput, InvalidMissingClosedReadonlyAppendSpecialDescriptors) {
  for (int fd : {-1, 0, 1, 2}) {
    ObservationOutput out(fd);
    EXPECT_FALSE(out.Initialize());
  }
  File f;
  int closed = dup(f.fd);
  ASSERT_GT(closed, 2);
  close(closed);
  ObservationOutput absent(closed);
  EXPECT_FALSE(absent.Initialize());
  for (int flags : {O_RDONLY, O_WRONLY | O_APPEND}) {
    const int fd = open(f.name, flags);
    ASSERT_GT(fd, 2);
    ObservationOutput out(fd);
    EXPECT_FALSE(out.Initialize());
    close(fd);
  }
  const int fd = open("/dev/null", O_WRONLY);
  ASSERT_GT(fd, 2);
  ObservationOutput special(fd);
  EXPECT_FALSE(special.Initialize());
  close(fd);
  int pipefd[2];
  ASSERT_EQ(pipe(pipefd), 0);
  ObservationOutput piped(pipefd[1]);
  EXPECT_FALSE(piped.Initialize());
  close(pipefd[0]);
  close(pipefd[1]);
}
TEST(LocalObservationOutput, StaleOffsetModeLinksAndDoubleInitializeReject) {
  File f;
  ASSERT_EQ(write(f.fd, "x", 1), 1);
  {
    ObservationOutput out(f.fd);
    EXPECT_FALSE(out.Initialize());
  }
  ASSERT_EQ(ftruncate(f.fd, 0), 0);
  {
    ObservationOutput out(f.fd);
    EXPECT_FALSE(out.Initialize());
  }
  ASSERT_EQ(lseek(f.fd, 0, SEEK_SET), 0);
  ASSERT_EQ(fchmod(f.fd, 0644), 0);
  {
    ObservationOutput out(f.fd);
    EXPECT_FALSE(out.Initialize());
  }
  ASSERT_EQ(fchmod(f.fd, 0600), 0);
  const auto linkname = std::string(f.name) + "-link";
  ASSERT_EQ(link(f.name, linkname.c_str()), 0);
  {
    ObservationOutput out(f.fd);
    EXPECT_FALSE(out.Initialize());
  }
  ASSERT_EQ(unlink(linkname.c_str()), 0);
  ObservationOutput out(f.fd);
  ASSERT_TRUE(out.Initialize());
  EXPECT_FALSE(out.Initialize());
  EXPECT_FALSE(Seven(&out));
}
TEST(LocalObservationOutput, WrongOrderDuplicateAndIncompleteNeverComplete) {
  for (bool duplicate : {false, true}) {
    File f;
    ObservationOutput out(f.fd);
    ASSERT_TRUE(out.Initialize());
    if (duplicate) ASSERT_TRUE(out.Record(ObservationOutput::kOrder[0], "{}"));
    EXPECT_FALSE(out.Record(ObservationOutput::kOrder[duplicate ? 0 : 1], "{}"));
    EXPECT_FALSE(out.Complete(true, 1, 1));
    EXPECT_EQ(f.Size(), duplicate ? 3 : 0);
  }
  File f;
  ObservationOutput out(f.fd);
  ASSERT_TRUE(out.Initialize());
  EXPECT_FALSE(out.Complete(true, 1, 1));
  EXPECT_FALSE(Seven(&out));
}
TEST(LocalObservationOutput, ExactAggregateBoundaryAndOverflowIncludesLF) {
  for (bool overflow : {false, true}) {
    File f;
    ObservationOutput out(f.fd);
    ASSERT_TRUE(out.Initialize());
    const size_t first = CaptureLimits{}.encoded_bytes - 1;
    ASSERT_TRUE(out.Record(ObservationOutput::kOrder[0], Json(first)));
    const size_t second = ObservationOutput::kMaximumBytes - (first + 1) - 15 - 1;
    ASSERT_TRUE(out.Record(ObservationOutput::kOrder[1], Json(second)));
    for (size_t i = 2; i < 6; ++i)
      ASSERT_TRUE(out.Record(ObservationOutput::kOrder[i], "{}"));
    EXPECT_EQ(out.Record(ObservationOutput::kOrder[6], overflow ? "{ }" : "{}"),
              !overflow);
    EXPECT_EQ(f.Size(), ObservationOutput::kMaximumBytes - (overflow ? 3 : 0));
    EXPECT_EQ(out.Complete(true, 1, 1).has_value(), !overflow);
  }
  for (uint64_t bound : {uint64_t{0}, ObservationOutput::kMaximumBytes + 1}) {
    File f;
    ObservationOutput out(f.fd, bound);
    EXPECT_FALSE(out.Initialize());
  }
}
TEST(LocalObservationOutput, PerObservationBoundaryAndTightenedBound) {
  for (size_t size : {size_t(CaptureLimits{}.encoded_bytes - 1),
                      size_t(CaptureLimits{}.encoded_bytes)}) {
    File f;
    ObservationOutput out(f.fd);
    ASSERT_TRUE(out.Initialize());
    EXPECT_EQ(out.Record(ObservationOutput::kOrder[0], Json(size)),
              size_t(CaptureLimits{}.encoded_bytes) != size);
    EXPECT_EQ(f.Size(), size_t(CaptureLimits{}.encoded_bytes) == size ? 0 : size + 1);
  }
  File f;
  ObservationOutput out(f.fd, 20);
  ASSERT_TRUE(out.Initialize());
  for (size_t i = 0; i < 6; ++i)
    ASSERT_TRUE(out.Record(ObservationOutput::kOrder[i], "{}"));
  EXPECT_FALSE(out.Record(ObservationOutput::kOrder[6], "{}"));
  EXPECT_EQ(f.Size(), 18);
}
int attempts = 0;
ssize_t ShortWrite(int fd, const void* bytes, size_t size) {
  if (++attempts == 1) {
    errno = EINTR;
    return -1;
  }
  return write(fd, bytes, size > 2 ? 2 : size);
}
ssize_t PartialError(int fd, const void* bytes, size_t size) {
  if (++attempts == 1) return write(fd, bytes, size > 2 ? 2 : size);
  errno = EIO;
  return -1;
}
ssize_t ZeroWrite(int, const void*, size_t) { return 0; }
ssize_t TooLargeWrite(int, const void*, size_t size) { return size + 1; }
int FailedSync(int) {
  errno = EIO;
  return -1;
}
int ChangedSync(int fd) { return fchmod(fd, 0644); }
TEST(LocalObservationOutput, ShortWritesAndInterruptedWritePreserveExactBytes) {
  File f;
  attempts = 0;
  ObservationOutput out(f.fd, 21, {ShortWrite, fsync});
  ASSERT_TRUE(out.Initialize());
  ASSERT_TRUE(Seven(&out));
  EXPECT_TRUE(out.Complete(true, 1, 1));
  EXPECT_EQ(f.Contents(), "{}\n{}\n{}\n{}\n{}\n{}\n{}\n");
}
TEST(LocalObservationOutput, PartialFailedZeroAndImpossibleWritesStayFailed) {
  for (auto callback : {PartialError, ZeroWrite, TooLargeWrite}) {
    File f;
    attempts = 0;
    ObservationOutput out(f.fd, 21, {callback, fsync});
    ASSERT_TRUE(out.Initialize());
    EXPECT_FALSE(out.Record(ObservationOutput::kOrder[0], "{}"));
    EXPECT_FALSE(out.Record(ObservationOutput::kOrder[0], "{}"));
    EXPECT_FALSE(out.Complete(true, 1, 1));
    EXPECT_EQ(f.Size(), callback == PartialError ? 2 : 0);
  }
}
TEST(LocalObservationOutput, FailedCleanupFsyncAndObservedChangeNeverComplete) {
  for (auto callback : {FailedSync, ChangedSync}) {
    File f;
    ObservationOutput out(f.fd, 21, {write, callback});
    ASSERT_TRUE(out.Initialize());
    ASSERT_TRUE(Seven(&out));
    EXPECT_FALSE(out.Complete(true, 1, 1));
    EXPECT_FALSE(out.Complete(true, 1, 1));
  }
  File f;
  ObservationOutput out(f.fd);
  ASSERT_TRUE(out.Initialize());
  ASSERT_TRUE(Seven(&out));
  EXPECT_FALSE(out.Complete(false, 1, 1));
  EXPECT_FALSE(out.Complete(true, 1, 1));
}
TEST(LocalObservationOutput, ObservedExternalSizeOffsetModeAndIdentityChangesFailClosed) {
  for (int change = 0; change < 4; ++change) {
    File f;
    ObservationOutput out(f.fd);
    ASSERT_TRUE(out.Initialize());
    if (change == 0) ASSERT_EQ(write(f.fd, "x", 1), 1);
    if (change == 1) ASSERT_EQ(lseek(f.fd, 1, SEEK_SET), 1);
    if (change == 2) ASSERT_EQ(fchmod(f.fd, 0644), 0);
    File replacement;
    if (change == 3) ASSERT_EQ(dup2(replacement.fd, f.fd), f.fd);
    EXPECT_FALSE(out.Record(ObservationOutput::kOrder[0], "{}"));
    EXPECT_FALSE(out.Complete(true, 1, 1));
  }
}
TEST(LocalObservationOutput, MalformedRecordCannotSplitLinesOrComplete) {
  for (const auto& json : {std::string{}, std::string("[]"), std::string("{\n}"),
                           std::string("{\r}"), std::string("{\0}", 3)}) {
    File f;
    ObservationOutput out(f.fd);
    ASSERT_TRUE(out.Initialize());
    EXPECT_FALSE(out.Record(ObservationOutput::kOrder[0], json));
    EXPECT_EQ(f.Size(), 0);
  }
}
}  // namespace
}  // namespace adbc::qualification
