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

"""Future load-only subprocess probe: python -I -B SCRIPT ROOT KIND.

Only load after external admission and archive preflight. This probe does not
connect, obtain credentials or accept a URI. Task008a tests use fake reports.
"""

import ctypes
import importlib.metadata
import json
import sys
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


if __name__ == "__main__":
    try:
        if (
            not sys.flags.isolated
            or not sys.flags.dont_write_bytecode
            or sys.pycache_prefix is not None
            or len(sys.argv) != 3
        ):
            raise ValueError("isolated cache-free interpreter required")
        print(json.dumps(probe(sys.argv[1], sys.argv[2])))
    except Exception:
        sys.exit("Installed artifact load-only probe failed.")
