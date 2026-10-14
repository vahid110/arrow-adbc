#!/usr/bin/env bash
#
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

# Fork: test one portable adbc_driver_redshift wheel in a clean virtualenv on
# Linux, macOS or Windows (Git Bash). The driver manager and test dependencies
# come from PyPI, as for a user. ADBC_POSTGRESQL_TEST_URI must point at a
# PostgreSQL server: the driver must load and reject it as not Redshift.
#
# Usage: pgwire_python_wheel_test.sh <python> <wheel> <expected-version> <adbc-src-dir>

set -euo pipefail

if [ "$#" -ne 4 ]; then
    echo "Usage: $0 <python> <wheel> <expected-version> <adbc-src-dir>"
    exit 1
fi

python=${1}
wheel=${2}
expected_version=${3}
source_dir=${4}
: "${ADBC_POSTGRESQL_TEST_URI:?must be set to a PostgreSQL URI}"
# The installed package must report its own version, not a build override.
unset SETUPTOOLS_SCM_PRETEND_VERSION

work=$(mktemp -d)
if command -v cygpath > /dev/null; then
    work=$(cygpath -m "${work}")
    wheel=$(cygpath -m "${wheel}")
    source_dir=$(cygpath -m "${source_dir}")
fi

echo "=== Installing ${wheel} into a clean virtualenv ==="
"${python}" -m venv "${work}/venv"
if [ -x "${work}/venv/bin/python" ]; then
    venv_python="${work}/venv/bin/python"
else
    venv_python="${work}/venv/Scripts/python.exe"
fi
"${venv_python}" -m pip install --upgrade pip
"${venv_python}" -m pip install "${wheel}" pandas 'pyarrow>=14.0.1' pytest

echo "=== Importing and loading the bundled driver ==="
cd "${work}"
library=$("${venv_python}" - "${expected_version}" <<'EOF'
import importlib.metadata
import pathlib
import sys

import adbc_driver_manager

import adbc_driver_redshift
import adbc_driver_redshift.dbapi  # noqa: F401

expected = sys.argv[1]
version = adbc_driver_redshift.__version__
metadata = importlib.metadata.version("adbc-driver-redshift")
assert version == expected, (version, expected)
assert metadata == expected, (metadata, expected)

path = pathlib.Path(adbc_driver_redshift._driver_path()).resolve()
assert path.is_file(), path
assert path.name == "libadbc_driver_redshift.so", path
assert pathlib.Path(sys.prefix).resolve() in path.parents, (path, sys.prefix)

# The driver manager loads the bundled library and its entrypoint; the driver
# (with its static libpq) then reports the bad URI.
try:
    adbc_driver_redshift.connect("invalid")
except adbc_driver_manager.OperationalError as e:
    assert "libpq" in str(e), e
else:
    raise AssertionError("connect('invalid') did not fail")
print(path)
EOF
)
echo "Bundled driver: ${library}"

# libpq is linked statically: the library must not need a system libpq.
case "$(uname -s)" in
    Linux)
        ldd "${library}"
        if ldd "${library}" | grep -E 'libpq|not found'; then
            echo "Unexpected dynamic dependency in ${library}"
            exit 1
        fi
        ;;
    Darwin)
        otool -L "${library}"
        if otool -L "${library}" | grep -E 'libpq|/opt/homebrew/|/usr/local/'; then
            echo "Unexpected dynamic dependency in ${library}"
            exit 1
        fi
        ;;
esac

echo "=== Running the package tests against PostgreSQL ==="
# --import-mode append: test the installed wheel, not the source tree
"${venv_python}" -m pytest -vv -rs -p no:cacheprovider --import-mode append \
    "${source_dir}/python/adbc_driver_redshift/tests" | tee "${work}/pytest.log"
# Only the live Redshift test may skip (ADBC_REDSHIFT_TEST_URI unset).
grep -Eq '[0-9]+ passed, 1 skipped' "${work}/pytest.log"
grep -q 'test_rejects_postgresql PASSED' "${work}/pytest.log"
echo "=== ${wheel} OK ==="
