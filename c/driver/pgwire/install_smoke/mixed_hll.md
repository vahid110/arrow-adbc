<!---
  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing,
  software distributed under the License is distributed on an
  "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
  KIND, either express or implied.  See the License for the
  specific language governing permissions and limitations
  under the License.
-->

# Installed Redshift mixed HLL qualification

Build this standalone POSIX consumer against the installed ADBC driver manager.
The only source compiled alongside the consumer is the repository's nanoarrow
C Data helper. The driver is opened from the absolute library path passed at run time.

```sh
cmake -S c/driver/pgwire/install_smoke -B build/mixed-hll -DCMAKE_PREFIX_PATH="$ADBC_PREFIX"
cmake --build build/mixed-hll --target adbc_redshift_mixed_hll
build/mixed-hll/adbc_redshift_mixed_hll --self-test
```

Run the following separately for each temporal mode on the isolated qualification
database. Here `provide_private_frame` denotes the coordinator's secret provider.
It writes the URI as a four-byte big-endian byte length followed by up to 4096
ASCII bytes, then closes the pipe. Supply a verify-full PostgreSQL URI and a
trusted CA file that can be opened independently during database and connection
initialization. Keep the URI out of arguments, environment dumps and command logs.

```sh
provide_private_frame | build/mixed-hll/adbc_redshift_mixed_hll "$ADBC_DRIVER_LIBRARY" arrow
provide_private_frame | build/mixed-hll/adbc_redshift_mixed_hll "$ADBC_DRIVER_LIBRARY" lossless
```

Each invocation uses session-local temporary tables. It compares mixed native
fields to separate all-HLL and non-HLL controls, including the complete Arrow
field metadata and value buffers. It checks repeated HLL columns, NULL rows,
empty results, schema-only execution, prepared Bind, and three ordered
BindStream results. A parameterized INSERT copies mixed rows into a second
temporary table. The final read checks exact row multiplicity and values to
detect skipped or repeated writes. A nonzero exit indicates failure.

The zero-row query and its schema-only description are each checked against
independent all-HLL and non-HLL projections using the same empty predicate.
The client reports field ordinals and metadata differences when Redshift
describes the empty projection differently from the populated one.
