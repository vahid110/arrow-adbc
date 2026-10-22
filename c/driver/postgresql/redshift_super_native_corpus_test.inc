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

// Actual cd9f native SUPER cells, cases1..16; SQL NULL and JSON null both
// arrived as wire NULL. Numeric tokens are received spellings, not input lexemes.
static constexpr const char* kNativeSuperCells[] = {
    nullptr,
    nullptr,
    "true",
    "\"\"",
    "\"é🙂\\u0001\"",
    "[]",
    "{}",
    "9007199254740993",
    "1.2200",
    "1.22",
    "{\"n\":1.2200}",
    "{\"n\":1.22}",
    "{\"other\":1}",
    "\"null\"",
    "1",
    "[{\"n\":1.2200},{\"n\":1.22}]",
};
