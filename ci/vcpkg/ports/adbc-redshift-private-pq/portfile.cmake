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

# This unique port never overrides vcpkg's ordinary libpq dependency.
if(VCPKG_CROSSCOMPILING)
  message(FATAL_ERROR "Private libpq cross-build route is not admitted")
endif()
if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_CRT_LINKAGE STREQUAL "dynamic")
  message(FATAL_ERROR "Private Windows libpq requires the existing /MD runtime")
endif()
if(NOT VCPKG_BUILD_TYPE STREQUAL "release")
  message(FATAL_ERROR "Use an existing static-release triplet for the initial private port"
  )
endif()
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
vcpkg_find_acquire_program(PYTHON3)
file(READ "${CMAKE_CURRENT_LIST_DIR}/pins.json" private_pq_pins)
string(JSON
       private_pq_url
       GET
       "${private_pq_pins}"
       source_url)
string(JSON
       private_pq_sha512
       GET
       "${private_pq_pins}"
       source_sha512)
# Script digests are part of the port's inputs, so its binary-cache key changes too.
foreach(private_pq_script pgwire_private_pq pgwire_private_pq_symbols
                          pgwire_private_pq_windows)
  if(private_pq_script STREQUAL "pgwire_private_pq")
    set(private_pq_pin build_script_sha256)
  elseif(private_pq_script STREQUAL "pgwire_private_pq_windows")
    set(private_pq_pin windows_build_script_sha256)
  else()
    set(private_pq_pin symbol_script_sha256)
  endif()
  string(JSON
         private_pq_expected
         GET
         "${private_pq_pins}"
         ${private_pq_pin})
  file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../../scripts/${private_pq_script}.py"
       private_pq_actual)
  if(NOT private_pq_actual STREQUAL private_pq_expected)
    message(FATAL_ERROR "Private libpq recipe digest mismatch")
  endif()
endforeach()
vcpkg_download_distfile(private_pq_archive
                        URLS
                        "${private_pq_url}"
                        FILENAME
                        "postgresql-17.11.tar.bz2"
                        SHA512
                        "${private_pq_sha512}")

# This directory is owned by this port, not a user installation prefix.
set(private_pq_work "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-private-pq")
file(REMOVE_RECURSE "${private_pq_work}")
set(private_pq_script pgwire_private_pq)
set(private_pq_platform_args --build-type release)
if(VCPKG_TARGET_IS_WINDOWS)
  foreach(private_pq_tool
          MESON
          NINJA
          CLANG
          GIT
          BISON
          FLEX
          PERL)
    vcpkg_find_acquire_program(${private_pq_tool})
  endforeach()
  get_filename_component(private_pq_llvm_dir "${CLANG}" DIRECTORY)
  foreach(private_pq_tool nm objcopy readobj)
    find_program(private_pq_${private_pq_tool}
                 NAMES llvm-${private_pq_tool}
                 PATHS "${private_pq_llvm_dir}"
                 NO_DEFAULT_PATH REQUIRED)
  endforeach()
  find_program(private_pq_cc NAMES cl REQUIRED)
  find_program(private_pq_lib NAMES lib REQUIRED)
  find_program(private_pq_pkgconf
               NAMES pkgconf pkg-config
               PATHS "${CURRENT_HOST_INSTALLED_DIR}/tools/pkgconf"
               NO_DEFAULT_PATH REQUIRED)
  set(private_pq_script pgwire_private_pq_windows)
  set(private_pq_platform_args
      --architecture
      "${VCPKG_TARGET_ARCHITECTURE}"
      --crt
      md
      --cc
      "${private_pq_cc}"
      --lib
      "${private_pq_lib}"
      --git
      "${GIT}"
      --ninja
      "${NINJA}"
      --nm
      "${private_pq_nm}"
      --objcopy
      "${private_pq_objcopy}"
      --readobj
      "${private_pq_readobj}"
      --pkgconf
      "${private_pq_pkgconf}"
      --bison
      "${BISON}"
      --flex
      "${FLEX}"
      --perl
      "${PERL}")
  # MESON is an argv list (Python, optional -I, meson.py), not one executable.
  foreach(private_pq_element IN LISTS MESON)
    list(APPEND private_pq_platform_args "--meson-command-element=${private_pq_element}")
  endforeach()
endif()
vcpkg_execute_required_process(COMMAND
                               "${PYTHON3}"
                               "${CMAKE_CURRENT_LIST_DIR}/../../../scripts/${private_pq_script}.py"
                               --source-archive
                               "${private_pq_archive}"
                               --work-dir
                               "${private_pq_work}"
                               --prefix
                               "${private_pq_work}/install"
                               --openssl-prefix
                               "${CURRENT_INSTALLED_DIR}"
                               ${private_pq_platform_args}
                               WORKING_DIRECTORY
                               "${CURRENT_BUILDTREES_DIR}"
                               LOGNAME
                               "private-pq-${TARGET_TRIPLET}")
file(INSTALL "${private_pq_work}/install/" DESTINATION "${CURRENT_PACKAGES_DIR}")
vcpkg_install_copyright(FILE_LIST "${CMAKE_CURRENT_LIST_DIR}/PostgreSQL-COPYRIGHT")
