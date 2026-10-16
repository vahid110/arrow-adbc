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

"""No-AWS installed artifact exercise and UNTRUSTED provenance sidecar producer.

Build workflows call this against their actual artifact bytes and clean prefix.
GitHub run/artifact API observations remain an EXTERNAL verifier responsibility.
"""

import argparse
import json
import os
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

from pgwire_qualification import (
    ENTRYPOINT,
    _inside,
    _member,
    file_sha,
    verify_installed,
    verify_probe,
)
from pgwire_qualification_manifest import digest, require
from pgwire_qualification_process import client, private_process


def claims(artifact, root, *, wheel):
    tree = {}
    root = Path(root)
    if wheel:
        with zipfile.ZipFile(artifact) as archive:
            names = [
                item.filename
                for item in archive.infolist()
                if not item.is_dir() and not item.filename.endswith(".dist-info/RECORD")
            ]
    else:
        with tarfile.open(artifact) as archive:
            names = [item.name for item in archive if not item.isdir()]
    for name in names:
        name = _member(name)
        path = _inside(root, name)
        tree[name] = (
            "symlink:" + path.readlink().as_posix()
            if path.is_symlink()
            else file_sha(path)
        )
    result = {"sha256": file_sha(artifact), "installed_tree_sha256": digest(tree)}
    # The generated inventory is a claim, never trust. Compare actual archive
    # members and bytes independently, including traversal, duplicate/link checks.
    verify_installed(artifact, root, result, wheel=wheel)
    return result


def exercise(artifact, root, kind, interpreter, *, c_client=None):
    wheel = kind == "python_wheel"
    inventory = claims(artifact, root, wheel=wheel)
    library = verify_installed(artifact, root, inventory, wheel=wheel)
    probe = [
        str(Path(interpreter).absolute()),
        "-I",
        "-B",
        str(Path(__file__).with_name("pgwire_qualification_probe.py").resolve()),
        str(Path(root).resolve()),
        kind,
        str(Path(artifact).resolve()),
        inventory["sha256"],
        inventory["installed_tree_sha256"],
    ]
    report = json.loads(private_process([*probe, "load-only", "-"]))
    verify_probe(report, package_root=root, library=library, wheel=wheel)
    # Fixed local PostgreSQL service only, no AWS credentials, no host environment.
    uri = "postgresql://postgres:password@postgres:5432/postgres?connect_timeout=10"
    argv = (
        [*probe, "--postgresql-plumbing", "adbc_q_80082"]
        if wheel
        else [
            str(Path(c_client).resolve()),
            str(library),
            "adbc_q_80081",
            "--postgresql-plumbing",
        ]
    )
    coverage = client(argv, uri, plumbing=True)
    return {
        **inventory,
        "entrypoint": ENTRYPOINT,
        "probe": report,
        "coverage": coverage,
        "qualification": "local-postgresql-plumbing-only",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("c_archive", "python_wheel"))
    parser.add_argument("artifact", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("--interpreter", default=sys.executable)
    parser.add_argument("--c-client")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        # Compare the actual checkout and tree; do not derive provenance solely
        # from env/files supplied beside the artifact. Remote origin is verified
        # later using GitHub's authenticated run/artifact endpoints.
        source = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True
        ).strip()
        tree = subprocess.check_output(
            ["git", "rev-parse", "HEAD^{tree}"], text=True
        ).strip()
        require(source == os.environ["GITHUB_SHA"], "checkout/source mismatch")
        require(
            os.environ["GITHUB_REPOSITORY"] == "vahid110/arrow-adbc",
            "wrong CI repository",
        )
        result = exercise(
            args.artifact,
            args.root,
            args.kind,
            args.interpreter,
            c_client=args.c_client,
        )
        args.output.write_text(
            json.dumps(
                {
                    "version": 1,
                    "trust": "unverified-ci-claims",
                    "source_sha": source,
                    "source_tree": tree,
                    "workflow_ref": os.environ["GITHUB_WORKFLOW_REF"],
                    "run_id": int(os.environ["GITHUB_RUN_ID"]),
                    "run_attempt": int(os.environ["GITHUB_RUN_ATTEMPT"]),
                    "artifact_file": args.artifact.name,
                    "kind": args.kind,
                    **result,
                },
                indent=2,
            )
            + "\n"
        )
        return 0
    except BaseException:
        print("Installed CI qualification failed; no live proof.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
