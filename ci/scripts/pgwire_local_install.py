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

"""Same-host trusted in-place snapshots and one fixed isolated load observation."""

import base64
import csv
import gzip
import hashlib
import importlib.util
import io
import json
import os
import posixpath
import selectors
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import warnings
import zipfile
from dataclasses import dataclass
from email.parser import Parser
from pathlib import Path


def _sibling(name):
    path = Path(__file__).with_name(name + ".py").resolve()
    if name in sys.modules:
        module = sys.modules[name]
        if Path(module.__file__).resolve() != path:
            raise ValueError("wrong-local-helper")
        return module
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


g0 = _sibling("pgwire_local_evidence")
probe = _sibling("pgwire_local_load_probe")
Rejected = g0.Rejected
require = g0._require


@dataclass(frozen=True)
class Bounds:
    compressed: int = 33554432
    expanded: int = 67108864
    member: int = 16777216
    c_members: int = 256
    wheel_members: int = 128
    metadata: int = 1048576
    images: int = 1024
    runtime_files: int = 128
    runtime_bytes: int = 67108864
    seconds: int = 30


@dataclass(frozen=True)
class FileIdentity:
    path: str
    resolved: str
    sha256: str
    bytes: int
    links: tuple[tuple[str, str], ...] = ()


@dataclass(frozen=True)
class Wheel:
    role: str
    package: str
    dist_info: str
    tag: str


@dataclass(frozen=True)
class Selected:
    evidence: g0.Selected
    collector_head: str
    collector_tree: str
    collector: FileIdentity
    child: FileIdentity
    interpreter: FileIdentity
    client: FileIdentity
    c_library: str
    manager_native: str
    c_links: tuple[tuple[str, str], ...]
    wheels: tuple[Wheel, ...]
    runtime: tuple[FileIdentity, ...]
    os_version: str
    os_build: str
    python_abi: str
    bounds: Bounds = Bounds()


@dataclass(frozen=True)
class Roots:
    evidence: g0.Roots
    prefix: Path
    site: Path
    metadata: Path


@dataclass(frozen=True)
class Entry:
    path: str
    kind: str
    sha256: str = ""
    bytes: int = 0
    target: str = ""


@dataclass(frozen=True)
class Observed:
    status: str
    scope: str
    artifact_head: str
    artifact_tree: str
    validation_sha256: str
    evidence_record_sha256: str
    collector_head: str
    collector_tree: str
    input_identities: tuple[FileIdentity, ...]
    inventories: tuple[tuple[str, str], ...]
    metadata: g0.Blob
    used_runtime: tuple[str, ...]
    unused_runtime: tuple[str, ...]


def _digest(value):
    from dataclasses import asdict

    if type(value) is tuple and all(type(item) is Entry for item in value):
        value = [asdict(item) for item in value]
    return hashlib.sha256(
        json.dumps(
            value, sort_keys=True, separators=(",", ":"), allow_nan=False
        ).encode()
    ).hexdigest()


def _absolute(value):
    require(
        type(value) is str
        and 0 < len(value) <= 16384
        and "\0" not in value
        and "\\" not in value,
        "invalid-path",
    )
    path = Path(value)
    require(
        path.is_absolute() and str(path) == value and ".." not in path.parts,
        "invalid-path",
    )
    return path


def _resolve(path):
    """Observe a finite literal symlink chain; no loader/default-path search."""
    path = _absolute(path)
    pending = list(path.parts[1:])
    current = Path("/")
    links = []
    while pending:
        current = current / pending.pop(0)
        info = current.lstat()
        if stat.S_ISLNK(info.st_mode):
            require(len(links) < 32, "link-limit")
            target = os.readlink(current)
            links.append((str(current), target))
            replacement = (
                target if target.startswith("/") else str(current.parent / target)
            )
            replacement = posixpath.normpath(replacement)
            pending = list(Path(replacement).parts[1:]) + pending
            current = Path("/")
    return str(current), tuple(links)


def _file(value, cap, *, collect=False):
    require(
        type(value) is FileIdentity and type(value.links) is tuple,
        "invalid-file-identity",
    )
    _absolute(value.path)
    _absolute(value.resolved)
    require(type(value.bytes) is int and 0 <= value.bytes <= cap, "size-limit")
    resolved, links = _resolve(value.path)
    require((resolved, links) == (value.resolved, value.links), "file-link-mismatch")
    path = Path(resolved)
    files = g0._Files(path.parent)
    try:
        data, _ = files.read(
            path.name, value.sha256, cap, size=value.bytes, collect=collect
        )
    finally:
        files.close()
    require(_resolve(value.path) == (resolved, links), "changed-file")
    return data


def _read(root, name, cap):
    parts = g0._path(name).parts
    files = g0._Files(root)
    directories = [os.dup(files.fd)]
    links = []
    fd = None
    try:
        for part in parts[:-1]:
            parent = directories[-1]
            child = os.open(
                part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent
            )
            links.append((parent, part, child))
            directories.append(child)
        parent = directories[-1]
        fd = os.open(
            parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent
        )
        before = os.fstat(fd)
        require(
            stat.S_ISREG(before.st_mode) and 0 <= before.st_size <= cap,
            "invalid-member",
        )
        data = bytearray()
        while True:
            chunk = os.read(fd, min(1048576, cap - len(data) + 1))
            if not chunk:
                break
            data.extend(chunk)
            require(len(data) <= cap, "size-limit")
        for parent_fd, part, child in links:
            opened, named = (
                os.fstat(child),
                os.stat(part, dir_fd=parent_fd, follow_symlinks=False),
            )
            require(
                (opened.st_dev, opened.st_ino, opened.st_mode)
                == (named.st_dev, named.st_ino, named.st_mode),
                "changed-file",
            )
        require(
            g0._signature(before)
            == g0._signature(os.fstat(fd))
            == g0._signature(os.stat(parts[-1], dir_fd=parent, follow_symlinks=False))
            and len(data) == before.st_size,
            "changed-file",
        )
        root_before, root_after = os.fstat(files.fd), root.lstat()
        require(
            (root_before.st_dev, root_before.st_ino, root_before.st_mode)
            == (root_after.st_dev, root_after.st_ino, root_after.st_mode),
            "changed-file",
        )
        return bytes(data)
    finally:
        if fd is not None:
            os.close(fd)
        for directory in reversed(directories):
            os.close(directory)
        files.close()


def _entries(root, cap, maximum):
    files = g0._Files(root)
    out = []

    def walk(fd, prefix):
        with os.scandir(fd) as entries:
            for item in entries:
                require(len(out) < maximum, "member-limit")
                name = prefix + item.name
                g0._path(name)
                info = item.stat(follow_symlinks=False)
                if stat.S_ISLNK(info.st_mode):
                    out.append(
                        Entry(name, "link", target=os.readlink(item.name, dir_fd=fd))
                    )
                elif stat.S_ISDIR(info.st_mode):
                    out.append(Entry(name, "dir"))
                    child = os.open(
                        item.name,
                        os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                        dir_fd=fd,
                    )
                    try:
                        walk(child, name + "/")
                        after = os.stat(item.name, dir_fd=fd, follow_symlinks=False)
                        require(
                            (info.st_dev, info.st_ino, info.st_mode)
                            == (after.st_dev, after.st_ino, after.st_mode),
                            "changed-file",
                        )
                    finally:
                        os.close(child)
                else:
                    require(stat.S_ISREG(info.st_mode), "invalid-member")
                    data = _read(root, name, cap)
                    out.append(
                        Entry(name, "file", hashlib.sha256(data).hexdigest(), len(data))
                    )

    try:
        walk(files.fd, "")
        before, after = os.fstat(files.fd), root.lstat()
        require(
            (before.st_dev, before.st_ino, before.st_mode)
            == (after.st_dev, after.st_ino, after.st_mode),
            "changed-file",
        )
        return tuple(sorted(out, key=lambda item: item.path))
    finally:
        files.close()


def _member(name, directory=False):
    if directory and name.endswith("/"):
        name = name[:-1]
    g0._path(name)
    return name


def _body(stream, declared, bounds):
    require(type(declared) is int and 0 <= declared <= bounds.member, "size-limit")
    data = stream.read(declared + 1)
    require(len(data) == declared, "member-size")
    return data


def _internal(name, links):
    seen = set()
    while name in links:
        require(name not in seen, "link-cycle")
        seen.add(name)
        target = links[name]
        g0._path(target)
        name = str(Path(name).parent / target)
        g0._path(name)
    return name


class _TarBytes:
    # Bound even hidden tar metadata before the standard parser consumes it.
    def __init__(self, data, cap):
        self.stream = gzip.GzipFile(fileobj=io.BytesIO(data))
        self.cap = cap
        self.total = 0

    def read(self, count):
        data = self.stream.read(min(count, self.cap - self.total + 1))
        self.total += len(data)
        require(self.total <= self.cap, "expanded-limit")
        return data


def _c_inventory(artifact, prefix, client, selected):
    bounds = selected.bounds
    expected = []
    seen = set()
    expanded = 0
    found_client = False
    links = dict(selected.c_links)
    require(len(links) == len(selected.c_links) == 6, "invalid-c-links")
    stream = _TarBytes(
        artifact, bounds.expanded + bounds.c_members * (16384 + 2048) + 10240
    )
    with tarfile.open(fileobj=stream, mode="r|") as archive:
        for index, member in enumerate(archive):
            require(index < bounds.c_members, "member-limit")
            name = _member(member.name, member.isdir())
            require(name not in seen and not member.sparse, "duplicate-member")
            seen.add(name)
            if name == "test-prefix":
                require(member.isdir(), "invalid-member")
                continue
            if name == "test-client/adbc_redshift_install_smoke":
                require(member.isfile() and not found_client, "invalid-member")
                data = _body(archive.extractfile(member), member.size, bounds)
                require(
                    len(data) == client.bytes
                    and hashlib.sha256(data).hexdigest() == client.sha256,
                    "client-mismatch",
                )
                expanded += len(data)
                require(expanded <= bounds.expanded, "expanded-limit")
                found_client = True
                continue
            require(name.startswith("test-prefix/"), "unexpected-member")
            relative = name[len("test-prefix/") :]
            if member.isdir():
                expected.append(Entry(relative, "dir"))
            elif member.issym():
                require(
                    relative in links and member.linkname == links[relative],
                    "link-mismatch",
                )
                expected.append(Entry(relative, "link", target=member.linkname))
            else:
                require(member.isfile() and not member.islnk(), "invalid-member")
                data = _body(archive.extractfile(member), member.size, bounds)
                expanded += len(data)
                expected.append(
                    Entry(relative, "file", hashlib.sha256(data).hexdigest(), len(data))
                )
            require(expanded <= bounds.expanded, "expanded-limit")
    require(found_client, "missing-client")
    expected = tuple(sorted(expected, key=lambda item: item.path))
    require(
        {entry.path: entry.target for entry in expected if entry.kind == "link"}
        == links,
        "link-mismatch",
    )
    regular = {entry.path for entry in expected if entry.kind == "file"}
    for name in links:
        require(_internal(name, links) in regular, "invalid-link-target")
    require(
        _entries(prefix, bounds.member, bounds.c_members) == expected, "prefix-mismatch"
    )
    require(_internal(selected.c_library, links) in regular, "wrong-c-library")
    return expected


def _record(data, expected, record_name):
    require(len(data) <= 1048576, "metadata-limit")
    rows = list(csv.reader(io.StringIO(data.decode("utf-8"), newline=""), strict=True))
    seen = set()
    for row in rows:
        require(len(row) == 3, "invalid-record")
        name, digest, size = row
        g0._path(name)
        require(name in expected and name not in seen, "invalid-record")
        seen.add(name)
        if name == record_name:
            require(digest == size == "", "invalid-record")
        else:
            entry = expected[name]
            correct = (
                base64.urlsafe_b64encode(bytes.fromhex(entry.sha256))
                .rstrip(b"=")
                .decode()
            )
            require(
                digest == "sha256=" + correct and size == str(entry.bytes),
                "invalid-record",
            )
    require(seen == set(expected), "invalid-record")


def _wheel_inventory(artifact, wheel_path, wheel_sha, site, spec, bounds):
    require(
        type(spec) is Wheel
        and spec.package in ("adbc_driver_manager", "adbc_driver_redshift"),
        "invalid-wheel",
    )
    g0._path(spec.dist_info)
    require(
        "/" not in spec.dist_info
        and spec.dist_info.startswith(spec.package + "-")
        and spec.dist_info.endswith(".dist-info"),
        "invalid-wheel",
    )
    files, bodies, seen, explicit_dirs = {}, {}, set(), set()
    expanded = 0
    with zipfile.ZipFile(io.BytesIO(artifact)) as archive:
        for index, member in enumerate(archive.infolist()):
            require(index < bounds.wheel_members, "member-limit")
            name = _member(member.filename, member.is_dir())
            require(name not in seen and member.flag_bits & 1 == 0, "duplicate-member")
            seen.add(name)
            require(
                name.split("/")[0] in (spec.package, spec.dist_info),
                "unexpected-member",
            )
            kind = (member.external_attr >> 16) & 0o170000
            require(
                (member.is_dir() and kind in (0, stat.S_IFDIR))
                or (not member.is_dir() and kind in (0, stat.S_IFREG)),
                "invalid-member",
            )
            require(
                "__pycache__" not in Path(name).parts
                and Path(name).suffix not in (".pyc", ".pyo"),
                "bytecode-rejected",
            )
            if member.is_dir():
                explicit_dirs.add(name)
                continue
            with archive.open(member) as stream:
                data = _body(stream, member.file_size, bounds)
            expanded += len(data)
            require(expanded <= bounds.expanded, "expanded-limit")
            files[name] = Entry(
                name, "file", hashlib.sha256(data).hexdigest(), len(data)
            )
            if name.startswith(spec.dist_info + "/"):
                require(len(data) <= bounds.metadata, "metadata-limit")
                bodies[name] = data
    record_name = spec.dist_info + "/RECORD"
    require(
        record_name in bodies and spec.package + "/__init__.py" in files,
        "incomplete-wheel",
    )
    _record(bodies[record_name], files, record_name)
    for filename in ("WHEEL", "METADATA"):
        require(spec.dist_info + "/" + filename in bodies, "incomplete-wheel")
    wheel_headers = Parser().parsestr(bodies[spec.dist_info + "/WHEEL"].decode("utf-8"))
    metadata = Parser().parsestr(bodies[spec.dist_info + "/METADATA"].decode("utf-8"))
    require(
        wheel_headers.get_all("Tag") == [spec.tag]
        and wheel_headers.get_all("Wheel-Version") == ["1.0"]
        and wheel_headers.get_all("Root-Is-Purelib") == ["false"],
        "wrong-wheel-tag",
    )
    names, versions = metadata.get_all("Name", []), metadata.get_all("Version", [])
    require(
        len(names) == len(versions) == 1
        and names[0].replace("-", "_") == spec.package
        and spec.dist_info == spec.package + "-" + versions[0] + ".dist-info",
        "wrong-wheel-metadata",
    )
    # Only these literal pip additions are permitted; installed RECORD is verified.
    expected = dict(files)
    for filename, required in (("INSTALLER", b"pip\n"), ("REQUESTED", b"")):
        name = spec.dist_info + "/" + filename
        data = _read(site, name, bounds.metadata)
        require(data == required, "wrong-pip-metadata")
        expected[name] = Entry(
            name, "file", hashlib.sha256(data).hexdigest(), len(data)
        )
    name = spec.dist_info + "/direct_url.json"
    data = _read(site, name, bounds.metadata)
    require(
        probe.strict_json(data)
        == {
            "archive_info": {
                "hash": "sha256=" + wheel_sha,
                "hashes": {"sha256": wheel_sha},
            },
            "url": wheel_path.as_uri(),
        },
        "wrong-pip-origin",
    )
    expected[name] = Entry(name, "file", hashlib.sha256(data).hexdigest(), len(data))
    installed_record = _read(site, record_name, bounds.metadata)
    expected[record_name] = Entry(
        record_name,
        "file",
        hashlib.sha256(installed_record).hexdigest(),
        len(installed_record),
    )
    _record(installed_record, expected, record_name)
    ancestors = set()
    for name in expected:
        ancestors.update(
            str(parent) for parent in Path(name).parents if str(parent) != "."
        )
    require(explicit_dirs <= ancestors, "unexpected-member")
    actual = []
    for name in (spec.package, spec.dist_info):
        root = site / name
        require(not root.is_symlink(), "package-link")
        actual.append(Entry(name, "dir"))
        for entry in _entries(root, bounds.member, bounds.wheel_members + 16):
            actual.append(
                Entry(
                    name + "/" + entry.path,
                    entry.kind,
                    entry.sha256,
                    entry.bytes,
                    entry.target,
                )
            )
    wanted = list(expected.values()) + [Entry(name, "dir") for name in ancestors]
    require(
        tuple(sorted(actual, key=lambda item: item.path))
        == tuple(sorted(wanted, key=lambda item: item.path)),
        "installed-wheel-mismatch",
    )
    return tuple(sorted(wanted, key=lambda item: item.path))


def _version(value):
    require(type(value) is str and len(value) <= 128, "invalid-os-version")
    pieces = value.split(".")
    require(
        1 <= len(pieces) <= 3
        and all(
            p.isascii() and p.isdecimal() and str(int(p)) == p and int(p) <= 65535
            for p in pieces
        ),
        "invalid-os-version",
    )
    parts = [int(p) for p in pieces] + [0] * (3 - len(pieces))
    require(parts[1] <= 255 and parts[2] <= 255, "invalid-os-version")
    return (parts[0] << 16) | (parts[1] << 8) | parts[2]


def _selection(selected, roots):
    require(
        type(selected) is Selected
        and type(roots) is Roots
        and type(selected.bounds) is Bounds,
        "invalid-selection",
    )
    bounds = selected.bounds
    for key, hard in vars(Bounds()).items():
        value = getattr(bounds, key)
        require(type(value) is int and 0 < value <= hard, "invalid-bounds")
    require(
        all(
            type(value) is FileIdentity
            for value in (
                selected.collector,
                selected.child,
                selected.interpreter,
                selected.client,
            )
        ),
        "invalid-file-identity",
    )
    g0._hash(selected.collector_head, g0._COMMIT)
    g0._hash(selected.collector_tree, g0._COMMIT)
    require(
        selected.collector.path == str(Path(__file__).resolve())
        and selected.child.path
        == str(Path(__file__).with_name("pgwire_local_load_probe.py").resolve()),
        "wrong-local-code",
    )
    for root in (roots.prefix, roots.site, roots.metadata):
        require(
            isinstance(root, Path)
            and root.is_absolute()
            and root.resolve(strict=True) == root,
            "invalid-root",
        )
    directory = roots.metadata.stat()
    require(
        stat.S_ISDIR(directory.st_mode)
        and directory.st_uid == os.geteuid()
        and stat.S_IMODE(directory.st_mode) == 0o700,
        "invalid-metadata-directory",
    )
    require(
        type(selected.runtime) is tuple
        and 0 < len(selected.runtime) <= bounds.runtime_files
        and all(type(entry) is FileIdentity for entry in selected.runtime)
        and len({entry.path for entry in selected.runtime}) == len(selected.runtime),
        "invalid-runtime-set",
    )
    require(
        all(
            type(entry.path) is str
            and not entry.path.startswith(("/usr/lib/", "/System/Library/"))
            for entry in selected.runtime
        ),
        "os-image-is-not-file-selection",
    )
    require(
        type(selected.c_links) is tuple
        and len(selected.c_links) == 6
        and all(type(pair) is tuple and len(pair) == 2 for pair in selected.c_links),
        "invalid-c-links",
    )
    for name, target in selected.c_links:
        g0._path(name)
        g0._path(target)
    for name in (selected.c_library, selected.manager_native):
        g0._path(name)
    require(
        selected.c_library.startswith("lib/libadbc_driver_redshift")
        and selected.manager_native.startswith("adbc_driver_manager/_lib."),
        "wrong-load-origin",
    )
    require(
        type(selected.wheels) is tuple
        and len(selected.wheels) == 2
        and all(
            type(w) is Wheel and type(w.tag) is str and 0 < len(w.tag) <= 16384
            for w in selected.wheels
        )
        and {w.role for w in selected.wheels} == {"manager_wheel", "redshift_wheel"},
        "invalid-wheels",
    )
    require(
        {w.role: w.package for w in selected.wheels}
        == {
            "manager_wheel": "adbc_driver_manager",
            "redshift_wheel": "adbc_driver_redshift",
        },
        "invalid-wheels",
    )
    _version(selected.os_version)
    require(
        type(selected.os_build) is str
        and 0 < len(selected.os_build) <= 128
        and selected.os_build.isascii()
        and selected.os_build.isalnum(),
        "invalid-os-build",
    )
    require(
        type(selected.python_abi) is str and 0 < len(selected.python_abi) <= 128,
        "invalid-abi",
    )


def _inventory(linked, selected, roots):
    bounds = selected.bounds
    artifacts = dict(linked.artifacts)
    files = g0._Files(roots.evidence.artifacts)
    inventories = []
    try:
        artifact = artifacts["c_archive"]
        data, _ = files.read(
            artifact.path,
            artifact.sha256,
            bounds.compressed,
            size=artifact.bytes,
            collect=True,
        )
        _file(selected.client, bounds.member)
        inventories.append(
            ("c_prefix", _c_inventory(data, roots.prefix, selected.client, selected))
        )
        for spec in selected.wheels:
            artifact = artifacts[spec.role]
            data, _ = files.read(
                artifact.path,
                artifact.sha256,
                bounds.compressed,
                size=artifact.bytes,
                collect=True,
            )
            path = roots.evidence.artifacts / artifact.path
            inventories.append(
                (
                    spec.role,
                    _wheel_inventory(
                        data, path, artifact.sha256, roots.site, spec, bounds
                    ),
                )
            )
    finally:
        files.close()
    return tuple(inventories)


def _stop(child):
    for sig, delay in ((signal.SIGTERM, 0.2), (signal.SIGKILL, 3)):
        try:
            os.killpg(child.pid, sig)
        except ProcessLookupError:
            pass
        try:
            child.wait(timeout=delay)
        except subprocess.TimeoutExpired:
            continue
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        return
    raise Rejected("load-reap-failed")


def _launch(selected, request, fd):
    """Only this fixed sibling protocol, never a caller-supplied argv runner."""
    argv = [
        selected.interpreter.path,
        "-I",
        "-B",
        selected.child.path,
        str(fd),
        json.dumps(request, separators=(",", ":"), allow_nan=False),
    ]
    child = None
    primary = None
    output = bytearray()
    deadline = time.monotonic() + selected.bounds.seconds
    try:
        child = subprocess.Popen(
            argv,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            close_fds=True,
            pass_fds=(fd,),
            start_new_session=True,
            env={"LANG": "C.UTF-8"},
        )
        require(time.monotonic() < deadline, "load-timeout")
        with selectors.DefaultSelector() as selector:
            os.set_blocking(child.stdout.fileno(), False)
            selector.register(child.stdout, selectors.EVENT_READ)
            while selector.get_map():
                require(time.monotonic() < deadline, "load-timeout")
                for key, _ in selector.select(
                    min(0.1, max(0, deadline - time.monotonic()))
                ):
                    data = os.read(key.fd, 2049)
                    output.extend(data)
                    require(len(output) <= 2048, "load-status-limit")
                    if not data:
                        selector.unregister(key.fileobj)
        require(time.monotonic() < deadline, "load-timeout")
        require(
            child.wait(timeout=max(0.001, deadline - time.monotonic())) == 0,
            "load-child-failed",
        )
        require(time.monotonic() < deadline, "load-timeout")
    except BaseException as error:
        primary = (
            error if isinstance(error, Rejected) else Rejected("load-process-failed")
        )
    finally:
        if child is not None:
            try:
                _stop(child)
            except BaseException:
                if primary is None:
                    primary = Rejected("load-reap-failed")
                primary.cleanup_unknown = True
            child.stdout.close()
    if primary is not None:
        raise primary from None
    status = probe.strict_json(output)
    g0._keys(status, ("version", "status", "bytes", "sha256"))
    require(
        type(status["version"]) is int
        and status["version"] == 1
        and status["status"] == "load-only-observed"
        and type(status["bytes"]) is int
        and 0 < status["bytes"] <= selected.bounds.metadata,
        "invalid-load-status",
    )
    g0._hash(status["sha256"])
    return status


def _metadata(fd, path, created, status, bounds):
    info = os.fstat(fd)
    require(
        (
            info.st_dev,
            info.st_ino,
            info.st_uid,
            stat.S_IMODE(info.st_mode),
            info.st_nlink,
        )
        == (created.st_dev, created.st_ino, os.geteuid(), 0o600, 1)
        and info.st_size == status["bytes"] <= bounds.metadata,
        "invalid-metadata-file",
    )
    require(g0._signature(info) == g0._signature(path.lstat()), "changed-metadata-file")
    os.lseek(fd, 0, os.SEEK_SET)
    data = bytearray()
    while True:
        chunk = os.read(fd, min(1048576, bounds.metadata - len(data) + 1))
        if not chunk:
            break
        data.extend(chunk)
        require(len(data) <= bounds.metadata, "metadata-limit")
    require(
        len(data) == info.st_size
        and g0._signature(info)
        == g0._signature(os.fstat(fd))
        == g0._signature(path.lstat())
        and hashlib.sha256(data).hexdigest() == status["sha256"],
        "metadata-mismatch",
    )
    return probe.strict_json(data)


def _fact(value, os_image=False):
    fields = ("cpu", "subtype", "filetype", "flags", "uuid", "deployment")
    g0._keys(
        value, fields if os_image else fields + ("dylibs", "rpaths", "other_commands")
    )
    for key in ("cpu", "subtype", "filetype", "flags"):
        require(
            type(value[key]) is int and 0 <= value[key] <= 4294967295, "invalid-image"
        )
    require(value["cpu"] == probe.ARM64, "wrong-image-cpu")
    g0._hash(value["uuid"], g0._LOCAL_ID)
    require(value["uuid"] != "0" * 32, "missing-image-uuid")
    deployment = value["deployment"]
    if deployment is not None:
        g0._keys(deployment, ("platform", "minos_packed", "sdk_packed"))
        require(
            all(type(n) is int and 0 <= n <= 4294967295 for n in deployment.values()),
            "invalid-image",
        )
    if os_image:
        return
    require(
        type(value["dylibs"]) is list
        and type(value["rpaths"]) is list
        and type(value["other_commands"]) is list
        and len(value["dylibs"]) + len(value["rpaths"]) + len(value["other_commands"])
        <= 4096,
        "image-command-limit",
    )
    for edge in value["dylibs"]:
        g0._keys(
            edge,
            ("command", "name", "timestamp", "current", "compatible", "resolution"),
        )
        require(
            type(edge["command"]) is int
            and edge["command"] in (0xD, 0xC, 0x80000018, 0x8000001F, 0x80000023)
            and edge["resolution"] == "unresolved",
            "invalid-image-edge",
        )
        require(
            type(edge["name"]) is str
            and 0 < len(edge["name"]) <= 16384
            and "\0" not in edge["name"],
            "invalid-image-edge",
        )
        require(
            all(
                type(edge[key]) is int and 0 <= edge[key] <= 4294967295
                for key in ("timestamp", "current", "compatible")
            ),
            "invalid-image-edge",
        )
    require(
        all(
            type(path) is str and 0 < len(path) <= 16384 and "\0" not in path
            for path in value["rpaths"]
        ),
        "invalid-image-rpath",
    )
    for command in value["other_commands"]:
        g0._keys(command, ("command", "bytes"))
        require(
            type(command["command"]) is int
            and 0 <= command["command"] <= 4294967295
            and type(command["bytes"]) is int
            and 8 <= command["bytes"] <= 1048576,
            "invalid-image-command",
        )


def _runtime(selected):
    out = {}
    for entry in selected.runtime:
        data = _file(entry, selected.bounds.runtime_bytes, collect=True)
        facts = probe.parse_macho(data)
        require(
            facts["filetype"] in (2, 6, 8)
            and facts["deployment"] is not None
            and facts["deployment"]["platform"] == 1
            and facts["deployment"]["minos_packed"] <= _version(selected.os_version),
            "unsupported-image-deployment",
        )
        out[entry.path] = facts
    return out


def _check_metadata(value, request, selected, before, inventories):
    g0._keys(
        value,
        (
            "version",
            "kind",
            "scope",
            "system",
            "machine",
            "os_version",
            "os_build",
            "python",
            "abi",
            "executable",
            "origins",
            "images",
            "connection_created",
            "broader_closure",
        ),
    )
    require(
        type(value["version"]) is int
        and value["version"] == 1
        and value["kind"] == "adbc-same-host-load-metadata"
        and value["scope"] == "named-load-only"
        and value["system"] == "Darwin"
        and value["machine"] == "arm64"
        and value["os_version"] == selected.os_version
        and value["os_build"] == selected.os_build
        and value["abi"] == selected.python_abi
        and value["executable"] == selected.interpreter.path
        and type(value["python"]) is str
        and 0 < len(value["python"]) <= 16384
        and value["connection_created"] is False
        and value["broader_closure"] == "unknown",
        "wrong-load-scope",
    )
    expected_paths = {
        "manager": str(Path(request["site"]) / "adbc_driver_manager/__init__.py"),
        "redshift": str(Path(request["site"]) / "adbc_driver_redshift/__init__.py"),
        "manager_native": request["manager_native"],
        "c_library": request["c_library"],
        "wheel_library": request["wheel_library"],
    }
    g0._keys(value["origins"], expected_paths)
    entries = {
        str(Path(request["site"]) / entry.path): entry
        for role, inventory in inventories
        if role != "c_prefix"
        for entry in inventory
        if entry.kind == "file"
    }
    for key, path in expected_paths.items():
        origin = value["origins"][key]
        g0._keys(origin, ("path", "sha256", "bytes"))
        require(
            origin["path"] == path and type(origin["bytes"]) is int, "wrong-load-origin"
        )
        if key == "c_library":
            require(path in before, "missing-c-image-selection")
            expected = next(entry for entry in selected.runtime if entry.path == path)
        else:
            require(path in entries, "wrong-load-origin")
            expected = entries[path]
        require(
            (origin["sha256"], origin["bytes"]) == (expected.sha256, expected.bytes),
            "wrong-load-origin",
        )
    images = value["images"]
    require(
        type(images) is list and 0 < len(images) <= selected.bounds.images,
        "image-count-limit",
    )
    used = set()
    for index, image in enumerate(images):
        require(
            type(image) is dict
            and type(image.get("index")) is int
            and image["index"] == index,
            "invalid-image-index",
        )
        path = image.get("path")
        _absolute(path)
        facts = {
            key: item for key, item in image.items() if key not in ("index", "path")
        }
        os_image = path.startswith(("/usr/lib/", "/System/Library/"))
        _fact(facts, os_image)
        if os_image:
            # Classify OS prefixes before disk existence; invent no cache hash.
            continue
        require(path in before and facts == before[path], "unknown-or-changed-image")
        used.add(path)
    require(
        request["c_library"] in used
        and request["wheel_library"] in used
        and request["manager_native"] in used,
        "missing-loaded-image",
    )
    # The interpreter launch binary can differ from dyld's actual main executable.
    return tuple(sorted(used)), tuple(sorted(set(before) - used))


def _observe_install(record_bytes, *, selected, roots):
    """Named same-host load facts only. Root owns actual invocation and selection."""
    fd = None
    try:
        _selection(selected, roots)
        linked = g0.verify_local_evidence(
            record_bytes, selected=selected.evidence, roots=roots.evidence
        )
        identities = (
            selected.collector,
            selected.child,
            selected.interpreter,
            selected.client,
            *selected.runtime,
        )
        for value in identities:
            _file(value, selected.bounds.runtime_bytes)
        inventories = _inventory(linked, selected, roots)
        before = _runtime(selected)
        c_library = roots.prefix / _internal(selected.c_library, dict(selected.c_links))
        request = {
            "version": 1,
            "site": str(roots.site),
            "c_library": str(c_library),
            "wheel_library": str(
                roots.site / "adbc_driver_redshift/libadbc_driver_redshift.so"
            ),
            "manager_native": str(roots.site / selected.manager_native),
        }
        fd, name = tempfile.mkstemp(
            prefix="local-load-", suffix=".json", dir=roots.metadata
        )
        path = Path(name)
        created = os.fstat(fd)
        for code in (selected.collector, selected.child, selected.interpreter):
            _file(code, selected.bounds.runtime_bytes)
        status = _launch(selected, request, fd)
        value = _metadata(fd, path, created, status, selected.bounds)
        used, unused = _check_metadata(value, request, selected, before, inventories)
        require(
            g0.verify_local_evidence(
                record_bytes, selected=selected.evidence, roots=roots.evidence
            )
            == linked,
            "changed-evidence",
        )
        require(
            _inventory(linked, selected, roots) == inventories, "changed-installation"
        )
        require(_runtime(selected) == before, "changed-runtime")
        for item in identities:
            _file(item, selected.bounds.runtime_bytes)
        return Observed(
            "same-host-load-observed",
            "one-named-load-only-process; broader-closure-unknown",
            selected.evidence.head,
            selected.evidence.tree,
            dict(linked.evidence)["validation_receipt"].sha256,
            linked.record_sha256,
            selected.collector_head,
            selected.collector_tree,
            identities,
            tuple((role, _digest(inventory)) for role, inventory in inventories),
            g0.Blob(path.name, status["sha256"], status["bytes"]),
            used,
            unused,
        )
    except Rejected:
        raise
    except (
        OSError,
        ValueError,
        RuntimeError,
        KeyError,
        TypeError,
        csv.Error,
        tarfile.TarError,
        zipfile.BadZipFile,
        probe.Invalid,
        Warning,
    ):
        raise Rejected("same-host-observation-failed") from None
    finally:
        if fd is not None:
            os.close(fd)


def observe_install(record_bytes, *, selected, roots):
    """One fixed same-host load; never an admission or arbitrary execution API."""
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        return _observe_install(record_bytes, selected=selected, roots=roots)
