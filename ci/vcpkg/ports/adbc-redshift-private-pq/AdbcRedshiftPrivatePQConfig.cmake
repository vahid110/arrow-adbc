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

include(CMakeFindDependencyMacro)
if(WIN32)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
find_dependency(OpenSSL
                REQUIRED
                COMPONENTS
                SSL
                Crypto)
find_dependency(Threads REQUIRED)

get_filename_component(_adbc_rs_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
foreach(_adbc_rs_component pq common port)
  if(NOT TARGET AdbcRedshiftPrivatePQ::${_adbc_rs_component})
    add_library(AdbcRedshiftPrivatePQ::${_adbc_rs_component} STATIC IMPORTED)
    if(_adbc_rs_component STREQUAL "pq")
      set(_adbc_rs_suffix "")
    else()
      set(_adbc_rs_suffix "_${_adbc_rs_component}")
    endif()
    if(WIN32)
      set(_adbc_rs_archive "adbc_rs_pq_17_11_v1${_adbc_rs_suffix}.lib")
    else()
      set(_adbc_rs_archive "libadbc_rs_pq_17_11_v1${_adbc_rs_suffix}.a")
    endif()
    set_target_properties(AdbcRedshiftPrivatePQ::${_adbc_rs_component}
                          PROPERTIES IMPORTED_LOCATION
                                     "${_adbc_rs_prefix}/lib/${_adbc_rs_archive}")
  endif()
endforeach()
if(WIN32)
  set(_adbc_rs_platform_libs
      ws2_32
      secur32
      advapi32
      crypt32
      bcrypt
      user32
      shell32)
else()
  set(_adbc_rs_platform_libs m)
endif()
set_target_properties(AdbcRedshiftPrivatePQ::pq
                      PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                 "${_adbc_rs_prefix}/include/adbc-redshift-private-pq"
                                 INTERFACE_LINK_LIBRARIES
                                 "AdbcRedshiftPrivatePQ::common;AdbcRedshiftPrivatePQ::port;OpenSSL::SSL;OpenSSL::Crypto;Threads::Threads;${_adbc_rs_platform_libs}"
)

# Use a function scope so cross-compilation settings remain unchanged for callers.
function(_adbc_rs_check_link)
  include(CheckCSourceCompiles)
  include(CMakePushCheckState)
  cmake_push_check_state(RESET)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
  if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
  endif()
  set(CMAKE_REQUIRED_LIBRARIES AdbcRedshiftPrivatePQ::pq)
  file(READ "${_adbc_rs_prefix}/share/adbc-redshift-private-pq/link_check.c"
       _adbc_rs_link_source)
  # Recheck on each configure; a cached success must not mask missing archives/helpers.
  unset(ADBC_REDSHIFT_PRIVATE_PQ_LINKS CACHE)
  check_c_source_compiles("${_adbc_rs_link_source}" ADBC_REDSHIFT_PRIVATE_PQ_LINKS)
  cmake_pop_check_state()
  if(NOT ADBC_REDSHIFT_PRIVATE_PQ_LINKS)
    message(FATAL_ERROR "Private Redshift libpq ABI/helper link check failed")
  endif()
endfunction()
_adbc_rs_check_link()
