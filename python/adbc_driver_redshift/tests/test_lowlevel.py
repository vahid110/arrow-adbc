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

import pytest

import adbc_driver_manager
import adbc_driver_redshift


def test_import() -> None:
    expected = {"ConnectionOptions", "StatementOptions", "connect", "__version__"}
    assert expected <= set(adbc_driver_redshift.__all__)


def test_version() -> None:
    assert adbc_driver_redshift.__version__  # type:ignore


def test_driver_path() -> None:
    assert "adbc_driver_redshift" in adbc_driver_redshift._driver_path()


def test_failed_connection() -> None:
    with pytest.raises(
        adbc_driver_manager.OperationalError, match=".*libpq.*Failed to connect.*"
    ):
        adbc_driver_redshift.connect("invalid")


def test_rejects_postgresql(postgres_uri: str) -> None:
    with pytest.raises(
        adbc_driver_manager.ProgrammingError,
        match="Expected Redshift but connected to PostgreSQL",
    ) as excinfo:
        adbc_driver_redshift.connect(postgres_uri)
    status = excinfo.value.status_code
    assert status == adbc_driver_manager.AdbcStatusCode.INVALID_ARGUMENT
