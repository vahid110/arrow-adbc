# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

cmake_minimum_required(VERSION 3.18)
project(private_pq_consumer LANGUAGES C)
find_package(AdbcRedshiftPrivatePQ CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(STOCKPQ REQUIRED IMPORTED_TARGET libpq)
enable_testing()
foreach(kind STATIC SHARED)
  string(TOLOWER "${kind}" kind_lower)
  add_library(private_${kind_lower} ${kind} consumer.c)
  target_link_libraries(private_${kind_lower} PRIVATE AdbcRedshiftPrivatePQ::pq)
  add_executable(consume_${kind_lower} main.c)
  target_link_libraries(consume_${kind_lower} PRIVATE private_${kind_lower}
                                                      PkgConfig::STOCKPQ)
  add_test(NAME consume_${kind_lower} COMMAND consume_${kind_lower})
endforeach()
