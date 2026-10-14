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

import typing

import pytest

import adbc_driver_manager.dbapi
from adbc_driver_redshift import dbapi

_EXCEPTIONS = (
    "Warning",
    "Error",
    "InterfaceError",
    "DatabaseError",
    "DataError",
    "OperationalError",
    "IntegrityError",
    "InternalError",
    "ProgrammingError",
    "NotSupportedError",
)


def test_dbapi_attributes() -> None:
    assert dbapi.apilevel == "2.0"
    assert dbapi.threadsafety == adbc_driver_manager.dbapi.threadsafety
    assert dbapi.paramstyle == "pyformat"
    assert dbapi.Connection is adbc_driver_manager.dbapi.Connection
    assert dbapi.Cursor is adbc_driver_manager.dbapi.Cursor
    assert callable(dbapi.connect)
    for name in dbapi.__all__:
        assert hasattr(dbapi, name), name
    for name in _EXCEPTIONS:
        assert getattr(dbapi, name) is getattr(adbc_driver_manager.dbapi, name)
    assert issubclass(dbapi.ProgrammingError, dbapi.DatabaseError)
    assert issubclass(dbapi.DatabaseError, dbapi.Error)


def test_dbapi_rejects_postgresql(postgres_uri: str) -> None:
    with pytest.raises(
        dbapi.ProgrammingError, match="Expected Redshift but connected to PostgreSQL"
    ):
        dbapi.connect(postgres_uri)


@pytest.fixture
def redshift(redshift_uri: str) -> typing.Generator[dbapi.Connection, None, None]:
    with dbapi.connect(redshift_uri) as conn:
        yield conn


def test_redshift_query(redshift: dbapi.Connection) -> None:
    with redshift.cursor() as cur:
        cur.execute("SELECT 1")
        assert cur.fetchone() == (1,)

        cur.execute(
            "SELECT $1::INTEGER + 1 AS answer, $2::VARCHAR AS label", (41, "hi")
        )
        assert cur.fetchall() == [(42, "hi")]

        cur.execute("SELECT 1 AS a UNION ALL SELECT 2 AS a ORDER BY a")
        table = cur.fetch_arrow_table()
        assert table.column_names == ["a"]
        assert table.column("a").to_pylist() == [1, 2]
