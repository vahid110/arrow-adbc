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
if(WIN32)
  set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
endif()
find_package(AdbcRedshiftPrivatePQ CONFIG REQUIRED)
if(WIN32)
  find_package(PostgreSQL REQUIRED)
  set(stock_pq PostgreSQL::PostgreSQL)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(STOCKPQ REQUIRED IMPORTED_TARGET libpq)
  set(stock_pq PkgConfig::STOCKPQ)
endif()
enable_testing()
foreach(kind STATIC SHARED)
  string(TOLOWER "${kind}" kind_lower)
  add_library(private_${kind_lower} ${kind} consumer.c)
  target_link_libraries(private_${kind_lower} PRIVATE AdbcRedshiftPrivatePQ::pq)
  if(kind STREQUAL "SHARED")
    target_compile_definitions(private_${kind_lower} PRIVATE ADBC_PRIVATE_CONSUMER_SHARED)
  endif()
  add_executable(consume_${kind_lower} main.c)
  target_link_libraries(consume_${kind_lower} PRIVATE private_${kind_lower} ${stock_pq})
  add_test(NAME consume_${kind_lower} COMMAND consume_${kind_lower})
endforeach()

if(WIN32)
  add_executable(consume_whole main.c)
  target_link_libraries(consume_whole PRIVATE private_static ${stock_pq})
  foreach(component pq common port)
    target_link_options(consume_whole PRIVATE
                        "/WHOLEARCHIVE:$<TARGET_FILE:AdbcRedshiftPrivatePQ::${component}>"
    )
  endforeach()
  target_link_options(consume_whole PRIVATE
                      "/WHOLEARCHIVE:$<TARGET_FILE:PostgreSQL::PostgreSQL>")
  add_test(NAME consume_whole COMMAND consume_whole)
endif()
