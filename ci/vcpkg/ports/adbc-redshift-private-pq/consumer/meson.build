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

project('private-pq-consumer', 'c')
private_pq = dependency(
    'adbc-redshift-private-pq',
    version: '=17.11.1',
    method: 'pkg-config',
    static: true,
    required: true,
)
if private_pq.get_variable(pkgconfig: 'abi') != '1'
    error('Wrong private libpq package ABI')
endif
if not meson.get_compiler('c').links(
    files('../link_check.c'),
    dependencies: private_pq,
    name: 'private libpq ABI and both format helpers',
)
    error('Private Redshift libpq ABI/helper link check failed')
endif
stock_pq = dependency('libpq', method: 'pkg-config', required: true)
private_static = static_library(
    'private_static',
    'consumer.c',
    dependencies: private_pq,
)
private_shared = shared_library(
    'private_shared',
    'consumer.c',
    dependencies: private_pq,
)
consume_static = executable(
    'consume_static',
    'main.c',
    link_with: private_static,
    dependencies: [stock_pq, private_pq],
)
consume_shared = executable(
    'consume_shared',
    'main.c',
    link_with: private_shared,
    dependencies: stock_pq,
)
test('consume_static', consume_static)
test('consume_shared', consume_shared)
