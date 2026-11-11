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

#include <ctype.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define URI_LIMIT 4096

// Length-prefixed private pipe, followed by EOF. One process-wide alarm also
// bounds a writer that slowly drips bytes and all native calls including release.
static bool ReadExact(void* out, size_t size) {
  size_t used = 0;
  while (used < size) {
    struct pollfd fd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&fd, 1, 3000) <= 0) return false;
    ssize_t n = read(STDIN_FILENO, (char*)out + used, size - used);
    if (n <= 0) return false;
    used += (size_t)n;
  }
  return true;
}

static bool ReadUri(char* uri) {
  struct stat input;
  if (fstat(STDIN_FILENO, &input) != 0 || !S_ISFIFO(input.st_mode)) return false;
  unsigned char header[4];
  if (!ReadExact(header, sizeof(header))) return false;
  uint32_t size = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) |
                  ((uint32_t)header[2] << 8) | header[3];
  if (size < 16 || size > URI_LIMIT || !ReadExact(uri, size)) return false;
  uri[size] = '\0';
  for (uint32_t i = 0; i < size; i++) {
    if ((unsigned char)uri[i] <= 32 || (unsigned char)uri[i] >= 127) return false;
    if (uri[i] == '%' && (i + 2 >= size || !isxdigit((unsigned char)uri[i + 1]) ||
                          !isxdigit((unsigned char)uri[i + 2])))
      return false;
  }
  struct pollfd fd = {STDIN_FILENO, POLLIN, 0};
  char extra;
  bool end = poll(&fd, 1, 3000) > 0 && read(STDIN_FILENO, &extra, 1) == 0;
  close(STDIN_FILENO);
  return end && strncmp(uri, "postgresql://", 13) == 0;
}
