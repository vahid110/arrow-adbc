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

"""Exercise the workflow's actual verifier with tiny offline wheel fixtures."""

import os
import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest
import zipfile

WORKFLOW = pathlib.Path(__file__).resolve().parents[2] / (
    ".github/workflows/pgwire-python-wheels.yml"
)
VERIFIER = textwrap.dedent(
    WORKFLOW.read_text()
    .split("          python3 - <<'PY'\n", 1)[1]
    .split("          PY\n", 1)[0]
)
VERSION = "0.0.0.dev0+g95e9cee4d340"
PLATFORMS = [
    "manylinux_2_26_x86_64.manylinux_2_28_x86_64",
    "manylinux_2_26_aarch64.manylinux_2_28_aarch64",
    "macosx_10_15_x86_64",
    "macosx_11_0_arm64",
    "win_amd64",
]


class WheelVerificationTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = pathlib.Path(self.directory.name)

    def write_wheel(self, platform, **changes):
        version = changes.get("filename_version", VERSION)
        path = self.root / f"adbc_driver_redshift-{version}-py3-none-{platform}.whl"
        tags = changes.get("tags", [f"py3-none-{p}" for p in platform.split(".")])
        dist_info = f"adbc_driver_redshift-{version}.dist-info"
        contents = {
            f"{dist_info}/METADATA": f"Version: {changes.get('version', VERSION)}\n",
            f"{dist_info}/WHEEL": (
                f"Root-Is-Purelib: {changes.get('pure', 'false')}\n"
                + "".join(f"Tag: {tag}\n" for tag in tags)
            ),
            "adbc_driver_redshift/_static_version.py": (
                f"version = '{changes.get('static_version', VERSION)}'\n"
            ),
            "adbc_driver_redshift/libadbc_driver_redshift.so": "dummy library",
        }
        contents.update(changes.get("extra_contents", {}))
        contents.pop(changes.get("omit", ""), None)
        with zipfile.ZipFile(path, "w") as archive:
            for name, content in contents.items():
                archive.writestr(name, content)

    def populate(self, platforms=PLATFORMS, **first_changes):
        for index, platform in enumerate(platforms):
            self.write_wheel(platform, **(first_changes if index == 0 else {}))

    def verify(self, expected_error=None):
        result = subprocess.run(
            [sys.executable, "-c", VERIFIER],
            cwd=self.root,
            env={**os.environ, "PYTHON_VERSION_EXPECTED": VERSION},
            capture_output=True,
            text=True,
        )
        if expected_error is None:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.count("OK "), 5)
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(expected_error, result.stderr)

    def test_auditwheel_dual_tags(self):
        self.populate()
        self.verify()

    def test_single_tags(self):
        self.populate([p.split(".")[-1] for p in PLATFORMS])
        self.verify()

    def test_reversed_dual_tags(self):
        self.populate([".".join(reversed(p.split("."))) for p in PLATFORMS])
        self.verify()

    def test_invalid_platforms(self):
        invalid = [
            "manylinux_2_26_aarch64.manylinux_2_28_x86_64",
            "manylinux_2_28_ppc64le",
            "manylinux_2_26_x86_64",
            "manylinux_2_28_x86_64.manylinux_2_29_x86_64",
            "manylinux_2_28_x86_64.linux_x86_64",
            "manylinux_2_28_x86_64.manylinux_2_28_x86_64",
            "win_arm64",
        ]
        for platform in invalid:
            with self.subTest(platform=platform):
                for wheel in self.root.iterdir():
                    wheel.unlink()
                self.populate([platform, *PLATFORMS[1:]])
                self.verify("platform")

    def test_missing_wheel(self):
        self.populate(PLATFORMS[:-1])
        self.verify("expected exactly five wheels")

    def test_extra_wheel(self):
        self.populate([*PLATFORMS, "manylinux_2_28_x86_64"])
        self.verify("expected exactly five wheels")

    def test_duplicate_target_with_five_wheels(self):
        self.populate([*PLATFORMS[:-1], "manylinux_2_28_x86_64"])
        self.verify("duplicate wheel target")

    def test_filename_version(self):
        self.populate(filename_version="1.2.3")
        self.verify("unexpected wheel")

    def test_metadata_mismatches(self):
        mutations = [
            ({"version": "1.2.3"}, "METADATA version mismatch"),
            ({"static_version": "1.2.3"}, "static version mismatch"),
            ({"pure": "true"}, "expected a non-pure wheel"),
            ({"tags": ["py3-none-manylinux_2_28_x86_64"]}, "tag mismatch"),
            ({"tags": [f"py3-none-{PLATFORMS[0]}"]}, "tag mismatch"),
            ({"tags": ["py3-none-manylinux_2_28_aarch64"]}, "tag mismatch"),
            ({"tags": ["py3-none-manylinux_2_28_x86_64"] * 2}, "tag mismatch"),
            (
                {
                    "tags": [f"py3-none-{p}" for p in PLATFORMS[0].split(".")]
                    + ["py3-none-linux_x86_64"]
                },
                "tag mismatch",
            ),
            (
                {"omit": "adbc_driver_redshift/libadbc_driver_redshift.so"},
                "missing bundled library",
            ),
            (
                {"extra_contents": {"other.dist-info/WHEEL": "Tag: py3-none-any\n"}},
                "expected one",
            ),
        ]
        for changes, error in mutations:
            with self.subTest(changes=changes):
                self.populate(**changes)
                self.verify(error)


if __name__ == "__main__":
    unittest.main()
