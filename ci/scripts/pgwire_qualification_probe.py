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

"""Isolated installed-artifact probe and private-pipe DBAPI client.

Load-only never connects or requests credentials. Connection operations are private
primitives, not live admission. CI exercises only fixed local PostgreSQL plumbing.
"""

import ctypes
import importlib.machinery
import importlib.metadata
import json
import sys
import types
from pathlib import Path


def probe(root, kind):
    root = Path(root).resolve(strict=True)
    if kind == "python_wheel":
        # Recheck before importing: -B only prevents writes, not cache reads.
        if any(
            p.name == "__pycache__" or p.suffix in (".pyc", ".pyo")
            for p in (root / "adbc_driver_redshift").rglob("*")
        ):
            raise ValueError("cached bytecode is not allowed")
        # Locate on the interpreter's real import path BEFORE executing package
        # code. Never add a caller prefix/source directory to sys.path.
        spec = importlib.machinery.PathFinder.find_spec("adbc_driver_redshift")
        if (
            spec is None
            or spec.origin is None
            or Path(spec.origin).resolve(strict=True)
            != root / "adbc_driver_redshift/__init__.py"
            or any(
                name == "adbc_driver_redshift"
                or name.startswith("adbc_driver_redshift.")
                for name in sys.modules
            )
        ):
            raise ValueError("wrong installed package before import")
        for path in (root / "adbc_driver_redshift").rglob("*"):
            if path.is_symlink():
                raise ValueError("package symlink is not allowed")
        import adbc_driver_redshift as package

        distribution = importlib.metadata.distribution("adbc-driver-redshift")
        module = Path(package.__file__).resolve(strict=True)
        distribution_root = Path(distribution.locate_file("")).resolve(strict=True)
        library = Path(package._driver_path()).resolve(strict=True)
        if (
            module != root / "adbc_driver_redshift/__init__.py"
            or distribution_root != root
            or library != root / "adbc_driver_redshift/libadbc_driver_redshift.so"
        ):
            raise ValueError("wrong installed package")
    elif kind == "c_archive":
        library = (root / "lib/libadbc_driver_redshift.so").resolve(strict=True)
        module = distribution_root = None
        if root not in library.parents:
            raise ValueError("wrong installed library")
    else:
        raise ValueError("unknown artifact kind")
    getattr(ctypes.CDLL(str(library)), "AdbcDriverRedshiftInit")
    return {
        "library": str(library),
        "module": str(module) if module else None,
        "distribution_root": str(distribution_root) if distribution_root else None,
        "entrypoint": "AdbcDriverRedshiftInit",
    }


def read_connection_uri(operation):
    """Private-input path shared by the executable and offline parser regressions."""
    import os
    import signal
    import stat
    import struct
    from urllib.parse import parse_qs, urlsplit

    from pgwire_qualification_manifest import schema
    from pgwire_qualification_process import validate_plumbing_uri, validate_uri

    signal.alarm(60)
    if not stat.S_ISFIFO(os.fstat(0).st_mode):
        raise ValueError("private pipe required")

    def exact(n):
        value = bytearray()
        while len(value) < n:
            chunk = os.read(0, n - len(value))
            if not chunk:
                raise ValueError("truncated input")
            value.extend(chunk)
        return bytes(value)

    size = struct.unpack("!I", exact(4))[0]
    if not 16 <= size <= 4096:
        raise ValueError("invalid input size")
    uri = exact(size).decode("ascii")
    if any(not 32 < ord(c) < 127 for c in uri) or os.read(0, 1):
        raise ValueError("invalid or extra input")
    os.close(0)
    if operation == "--postgresql-plumbing":
        validate_plumbing_uri(uri)
    elif operation == "--redshift":
        options = parse_qs(
            urlsplit(uri).query, keep_blank_values=True, strict_parsing=True
        )
        ca = options["sslrootcert"][0]
        validate_uri(uri, schema(), ca)
    else:
        raise ValueError("invalid operation")
    return uri


def main():
    try:
        if (
            not sys.flags.isolated
            or not sys.flags.dont_write_bytecode
            or sys.pycache_prefix is not None
            or len(sys.argv) != 8
        ):
            raise ValueError("isolated cache-free interpreter required")
        root, kind, artifact, sha, tree, operation, fixture = sys.argv[1:]
        # Only this reviewed helper directory, not an artifact/source package root.
        helper_root = Path(__file__).resolve().parent
        for name in (
            "pgwire_qualification_manifest",
            "pgwire_qualification",
            "pgwire_qualification_process",
        ):
            path = helper_root / (name + ".py")
            module = types.ModuleType(name)
            module.__file__ = str(path)
            sys.modules[name] = module
            exec(compile(path.read_bytes(), str(path), "exec"), module.__dict__)
        from pgwire_qualification import python_coverage, verify_installed

        verify_installed(
            artifact,
            root,
            {"sha256": sha, "installed_tree_sha256": tree},
            wheel=kind == "python_wheel",
        )
        report = probe(root, kind)
        if operation == "load-only":
            print(json.dumps(report))
            return 0
        if kind != "python_wheel" or operation not in (
            "--postgresql-plumbing",
            "--redshift",
        ):
            raise ValueError("invalid operation")
        # Validation and private-input reading use the same executable path
        # covered by parser regressions; imports below cannot run on rejection.
        uri = read_connection_uri(operation)
        plumbing = operation == "--postgresql-plumbing"
        import pyarrow

        import adbc_driver_manager
        import adbc_driver_manager.dbapi
        import adbc_driver_redshift.dbapi as dbapi

        connection = None
        try:
            if plumbing:
                database = adbc_driver_manager.AdbcDatabase(
                    driver=report["library"],
                    entrypoint="AdbcDriverPostgresqlInit",
                    uri=uri,
                )
                try:
                    low = adbc_driver_manager.AdbcConnection(database)
                    connection = adbc_driver_manager.dbapi.Connection(
                        database, low, autocommit=True
                    )
                except BaseException:
                    database.close()
                    raise
            else:
                connection = dbapi.connect(uri, autocommit=True)
            table = pyarrow.table(
                {"value": pyarrow.array([1, 2], type=pyarrow.int64())}
            )
            result = python_coverage(
                connection,
                table,
                fixture,
                plumbing=plumbing,
                replace_tables=()
                if plumbing
                else (
                    table.rename_columns(["c" * 128]),
                    table.append_column("VALUE", table.column(0)),
                ),
            )
        finally:
            uri = None
            if connection is not None:
                connection.close()
        print(json.dumps(result))
        return 0
    except BaseException:
        print("Installed artifact operation failed; cleanup unknown.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
