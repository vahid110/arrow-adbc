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

"""Offline distribution provenance and pinned-download failure tests."""

import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import pgwire_private_pq_distribution as distribution


def fixture(root):
    prefix = root / "dependency"
    names = [
        "lib/libadbc_rs_pq_17_11_v1" + suffix + ".a"
        for suffix in ("", "_common", "_port")
    ]
    share = prefix / "share" / distribution.PACKAGE
    for name in ("pins.json", "PostgreSQL-COPYRIGHT", "symbols.json", "namespace.h"):
        names.append("share/" + distribution.PACKAGE + "/" + name)
    names.append("include/adbc-redshift-private-pq/adbc_rs_pq.h")
    for name in names:
        target = prefix / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("fake dependency input: " + name)
    (share / "pins.json").write_text(json.dumps(distribution.pins()))
    receipt = {
        "status": "built-audited-installed-socket-free-probe-passed",
        "pins": distribution.pins(),
        "installed_files": {name: distribution.digest(prefix / name) for name in names},
    }
    (share / "provenance.json").write_text(json.dumps(receipt))
    return prefix, share, receipt


class DistributionTest(unittest.TestCase):
    def test_stage_preserves_metadata_and_replaces_only_owned_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix, share, _ = fixture(root)
            destination = root / "_private_pq"
            destination.mkdir()
            (destination / "stale").write_text("old")
            distribution.stage(prefix, destination)
            self.assertFalse((destination / "stale").exists())
            self.assertEqual(
                (destination / "provenance.json").read_bytes(),
                (share / "provenance.json").read_bytes(),
            )
            self.assertFalse((destination / "lib").exists())
            with self.assertRaisesRegex(ValueError, "outside the dependency"):
                distribution.stage(prefix, share / "_private_pq")
            with self.assertRaisesRegex(ValueError, "owned _private_pq"):
                distribution.stage(prefix, root / "other")

    def test_archive_tamper_rejects_before_replacing_prior_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix, _, _ = fixture(root)
            (prefix / "lib/libadbc_rs_pq_17_11_v1.a").write_text("wrong archive")
            destination = root / "_private_pq"
            destination.mkdir()
            sentinel = destination / "keep"
            sentinel.write_text("unchanged")
            with self.assertRaisesRegex(ValueError, "installed file differs"):
                distribution.stage(prefix, destination)
            self.assertEqual(sentinel.read_text(), "unchanged")

    def test_stale_recipe_missing_files_escaping_paths_and_failure_rejected(self):
        for failure in ("stale", "missing", "escape", "failed"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                prefix, share, receipt = fixture(root)
                if failure == "stale":
                    receipt["pins"]["abi"] = 99
                elif failure == "missing":
                    receipt["installed_files"].pop("lib/libadbc_rs_pq_17_11_v1.a")
                elif failure == "escape":
                    receipt["installed_files"]["../outside"] = "bad"
                else:
                    receipt["status"] = "started"
                (share / "provenance.json").write_text(json.dumps(receipt))
                with self.assertRaises(ValueError):
                    distribution.stage(prefix, root / "_private_pq")
                self.assertFalse((root / "_private_pq").exists())

    def test_package_manager_metadata_is_not_trusted_or_copied(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix, share, _ = fixture(root)
            (share / "vcpkg.spdx.json").write_text("not part of original recipe")
            destination = root / "_private_pq"
            distribution.stage(prefix, destination)
            self.assertFalse((destination / "vcpkg.spdx.json").exists())
            self.assertTrue((destination / "PostgreSQL-COPYRIGHT").exists())

    def test_bundle_relocates_complete_package_and_preserves_other_driver(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix, _, _ = fixture(root)
            destination = root / "installed-driver"
            destination.mkdir()
            stock = destination / "stock-libpq"
            stock.write_text("untouched")
            distribution.bundle(prefix, destination)
            distribution.verified_metadata(destination)
            self.assertEqual(stock.read_text(), "untouched")
            with self.assertRaisesRegex(ValueError, "Refusing to overwrite"):
                distribution.bundle(prefix, destination)

    def test_bundle_conflict_does_not_partially_copy(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix, _, _ = fixture(root)
            destination = root / "installed-driver"
            conflict = destination / "share" / distribution.PACKAGE / "namespace.h"
            conflict.parent.mkdir(parents=True)
            conflict.write_text("preserve")
            with self.assertRaisesRegex(ValueError, "Refusing to overwrite"):
                distribution.bundle(prefix, destination)
            self.assertFalse((destination / "lib").exists())
            self.assertEqual(conflict.read_text(), "preserve")

    def test_download_requires_both_pins_before_publishing(self):
        for failure in (None, "sha256", "sha512"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                source = root / "fake-source"
                source.write_bytes(b"pinned source bytes")
                expected = distribution.pins()
                for algorithm in ("sha256", "sha512"):
                    expected["source_" + algorithm] = distribution.digest(
                        source, algorithm
                    )
                if failure:
                    expected["source_" + failure] = "wrong"
                destination = root / "download"
                with (
                    patch.object(distribution, "pins", return_value=expected),
                    patch.object(
                        distribution.urllib.request,
                        "urlopen",
                        return_value=io.BytesIO(source.read_bytes()),
                    ),
                ):
                    if failure:
                        with self.assertRaisesRegex(ValueError, "digest mismatch"):
                            distribution.fetch(destination)
                        self.assertFalse(destination.exists())
                    else:
                        distribution.fetch(destination)
                        self.assertEqual(destination.read_bytes(), source.read_bytes())


if __name__ == "__main__":
    unittest.main()
