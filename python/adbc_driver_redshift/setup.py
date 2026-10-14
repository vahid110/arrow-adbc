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

import os
import shutil
from pathlib import Path

from setuptools import setup
from setuptools.command.build_py import build_py

source_root = Path(__file__).parent
repo_root = source_root.joinpath("../../")

# ------------------------------------------------------------
# Resolve Shared Library

library = os.environ.get("ADBC_REDSHIFT_LIBRARY")
target = source_root.joinpath(
    "./adbc_driver_redshift/libadbc_driver_redshift.so"
).resolve()
if not library:
    if os.environ.get("_ADBC_IS_SDIST", "").strip().lower() in ("1", "true"):
        print("Building sdist, not requiring ADBC_REDSHIFT_LIBRARY")
    elif os.environ.get("_ADBC_IS_CONDA", "").strip().lower() in ("1", "true"):
        print("Building Conda package, not requiring ADBC_REDSHIFT_LIBRARY")
    elif target.is_file():
        print("Driver already exists (but may be stale?), continuing")
    else:
        raise ValueError("Must provide ADBC_REDSHIFT_LIBRARY")
else:
    shutil.copy(library, target)

# ------------------------------------------------------------
# Resolve Version (miniver)


def get_version(pkg_path):
    """
    Load version.py module without importing the whole package.

    Template code from miniver.
    """
    from importlib.util import module_from_spec, spec_from_file_location

    spec = spec_from_file_location("version", os.path.join(pkg_path, "_version.py"))
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.__version__


version = get_version("adbc_driver_redshift")


class build_py_with_version(build_py):
    """Record a SETUPTOOLS_SCM_PRETEND_VERSION build in the built package.

    Without this, the installed ``__version__`` would come from the source
    ``_static_version.py`` instead of the wheel's version.
    """

    def run(self):
        super().run()
        if os.environ.get("SETUPTOOLS_SCM_PRETEND_VERSION"):
            target = Path(self.build_lib, "adbc_driver_redshift", "_static_version.py")
            target.write_text(
                "# Written by setup.py from SETUPTOOLS_SCM_PRETEND_VERSION.\n"
                f"version = {version!r}\n",
                encoding="utf-8",
            )


# ------------------------------------------------------------
# Setup

setup(
    cmdclass={"build_py": build_py_with_version},
    version=version,
)
