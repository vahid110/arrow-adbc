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

"""Fetch pinned source or stage verified private-pq provenance for distributions."""

import argparse
import hashlib
import json
import shutil
import tempfile
import urllib.request
from pathlib import Path

ASSETS = Path(__file__).resolve().parents[1] / "vcpkg/ports/adbc-redshift-private-pq"
PACKAGE = "adbc-redshift-private-pq"


def digest(path, algorithm="sha256"):
    hasher = hashlib.new(algorithm)
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def pins():
    return json.loads((ASSETS / "pins.json").read_text())


def fetch(destination):
    """Publish a download only after both pinned digests match."""
    if destination.exists():
        raise ValueError("Source destination must be new")
    expected = pins()
    with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
        archive = Path(temporary) / "source.tar.bz2"
        with urllib.request.urlopen(expected["source_url"], timeout=120) as source:
            with archive.open("wb") as output:
                shutil.copyfileobj(source, output)
        for algorithm in ("sha256", "sha512"):
            if digest(archive, algorithm) != expected["source_" + algorithm]:
                raise ValueError("Downloaded private-pq source digest mismatch")
        archive.rename(destination)


def verified_metadata(prefix):
    share = prefix / "share" / PACKAGE
    receipt = json.loads((share / "provenance.json").read_text())
    expected = pins()
    if receipt.get("status") != "built-audited-installed-socket-free-probe-passed":
        raise ValueError("Private-pq package has no successful build receipt")
    if (
        receipt.get("pins") != expected
        or json.loads((share / "pins.json").read_text()) != expected
    ):
        raise ValueError("Private-pq package does not match this source recipe")
    recorded = receipt["installed_files"]
    required = [
        "lib/libadbc_rs_pq_17_11_v1" + suffix + ".a"
        for suffix in ("", "_common", "_port")
    ]
    required += [
        "share/" + PACKAGE + "/" + name
        for name in ("pins.json", "PostgreSQL-COPYRIGHT", "symbols.json", "namespace.h")
    ]
    if not all(name in recorded for name in required):
        raise ValueError("Private-pq build receipt omits required package files")
    # Validate the full installed package before staging metadata. A stale cached
    # package or modified archive must not acquire a fresh distribution receipt.
    for name, expected_hash in recorded.items():
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("Invalid private-pq receipt path")
        path = prefix / relative
        if (
            not path.is_file()
            or path.is_symlink()
            or not path.resolve().is_relative_to(prefix.resolve())
            or digest(path) != expected_hash
        ):
            raise ValueError("Private-pq installed file differs: " + name)
    # vcpkg may append its own copyright/cache metadata. Those unrecorded files
    # are never trusted or copied into our distributions.
    return share


def stage(prefix, destination):
    share = verified_metadata(prefix)
    if destination.resolve().is_relative_to(prefix.resolve()):
        raise ValueError("Metadata destination must be outside the dependency prefix")
    # Only this explicitly named generated wheel directory may be replaced.
    if destination.name != "_private_pq" or destination.is_symlink():
        raise ValueError("Metadata destination must be an owned _private_pq directory")
    with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
        staged = Path(temporary) / "_private_pq"
        staged.mkdir()
        receipt = json.loads((share / "provenance.json").read_text())
        metadata_prefix = "share/" + PACKAGE + "/"
        names = [
            name[len(metadata_prefix) :]
            for name in receipt["installed_files"]
            if name.startswith(metadata_prefix)
        ] + ["provenance.json"]
        for name in names:
            target = staged / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(share / name, target)
        if destination.exists():
            shutil.rmtree(destination)
        staged.rename(destination)


def bundle(prefix, destination):
    """Copy only the verified private package alongside the driver installation."""
    share = verified_metadata(prefix)
    receipt = json.loads((share / "provenance.json").read_text())
    names = list(receipt["installed_files"]) + ["share/" + PACKAGE + "/provenance.json"]
    for name in names:
        if (destination / name).exists():
            raise ValueError("Refusing to overwrite distribution file: " + name)
    for name in names:
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(prefix / name, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    download = commands.add_parser("fetch")
    download.add_argument("destination", type=Path)
    for action in ("stage", "bundle"):
        metadata = commands.add_parser(action)
        metadata.add_argument("prefix", type=Path)
        metadata.add_argument("destination", type=Path)
    verify = commands.add_parser("verify")
    verify.add_argument("prefix", type=Path)
    args = parser.parse_args()
    if args.command == "fetch":
        fetch(args.destination)
    elif args.command == "verify":
        verified_metadata(args.prefix)
    elif args.command == "bundle":
        bundle(args.prefix, args.destination)
    else:
        stage(args.prefix, args.destination)


if __name__ == "__main__":
    main()
