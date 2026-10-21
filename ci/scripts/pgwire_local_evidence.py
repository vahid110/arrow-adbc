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

"""Link anchored historical local evidence; never authorize or execute anything."""

import hashlib
import json
import os
import re
import stat
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path, PurePosixPath

REPOSITORY = "vahid110/arrow-adbc"
INTEGRATION_REF = "refs/heads/local/redshift-types-20261018"
EVIDENCE_ROLES = (
    "source_freeze",
    "review_receipt",
    "review_body",
    "review_checks",
    "validation_receipt",
    "validator_source",
    "case_plan",
)
ARTIFACT_ROLES = ("c_archive", "manager_wheel", "redshift_wheel")
_SHA = re.compile(r"[0-9a-f]{64}")
_COMMIT = re.compile(r"[0-9a-f]{40}")
_LOCAL_ID = re.compile(r"[0-9a-f]{32}")


class Rejected(ValueError):
    """Static category only; never include an input value or underlying error."""


def _require(condition, category):
    if not condition:
        raise Rejected(category)


@dataclass(frozen=True)
class Blob:
    path: str
    sha256: str
    bytes: int


@dataclass(frozen=True)
class Step:
    kind: str
    command: tuple[str, ...]


@dataclass(frozen=True)
class Limits:
    record: int = 1048576
    evidence_json: int = 1048576
    body: int = 67108864
    artifact: int = 536870912
    steps: int = 128
    argv: int = 256
    string: int = 16384
    source_files: int = 10000


@dataclass(frozen=True)
class Selected:
    repository: str
    head: str
    tree: str
    integration_ref: str
    local_id: str
    evidence: tuple[tuple[str, Blob], ...]
    artifacts: tuple[tuple[str, Blob], ...]
    steps: tuple[Step, ...]
    limits: Limits = Limits()


@dataclass(frozen=True)
class Roots:
    evidence: Path
    artifacts: Path


@dataclass(frozen=True)
class LinkedEvidence:
    status: str
    record_sha256: str
    payload_sha256: str
    evidence: tuple[tuple[str, Blob], ...]
    artifacts: tuple[tuple[str, Blob], ...]
    logs: tuple[Blob, ...]


def _hash(value, pattern=_SHA):
    _require(type(value) is str and pattern.fullmatch(value), "invalid-identity")


def _keys(value, keys):
    _require(type(value) is dict and value.keys() == set(keys), "invalid-fields")


def _path(value):
    _require(
        type(value) is str
        and 0 < len(value) <= 16384
        and "\\" not in value
        and "\0" not in value,
        "invalid-path",
    )
    path = PurePosixPath(value)
    _require(
        not path.is_absolute()
        and value == str(path)
        and value != "."
        and ".." not in path.parts
        and len(path.parts) <= 128,
        "invalid-path",
    )
    return path


def _blob(value, cap):
    _require(type(value) is Blob, "invalid-anchor")
    _path(value.path)
    _hash(value.sha256)
    _require(type(value.bytes) is int and 0 <= value.bytes <= cap, "size-limit")
    return value


def _role_map(value, roles, limits):
    _require(type(value) is tuple and len(value) == len(roles), "invalid-anchor")
    result = {}
    for pair in value:
        _require(type(pair) is tuple and len(pair) == 2, "invalid-anchor")
        role, blob = pair
        _require(
            type(role) is str and role in roles and role not in result, "invalid-anchor"
        )
        cap = (
            limits.artifact
            if roles == ARTIFACT_ROLES
            else (
                limits.body
                if role in ("review_body", "review_checks", "validator_source")
                else limits.evidence_json
            )
        )
        result[role] = _blob(blob, cap)
    _require(len({b.path for b in result.values()}) == len(roles), "duplicate-path")
    return result


def _step(kind, command, limits):
    _require(type(kind) is str and 0 < len(kind) <= limits.string, "invalid-step")
    _require(
        type(command) in (list, tuple) and 0 < len(command) <= limits.argv,
        "invalid-step",
    )
    _require(
        all(
            type(arg) is str and len(arg) <= limits.string and "\0" not in arg
            for arg in command
        ),
        "invalid-step",
    )
    return Step(kind, tuple(command))


def _selection(selected):
    _require(
        type(selected) is Selected and type(selected.limits) is Limits,
        "invalid-selection",
    )
    limits = selected.limits
    for key, hard in vars(Limits()).items():
        value = getattr(limits, key)
        _require(type(value) is int and 0 <= value <= hard, "invalid-limits")
    _require(
        selected.repository == REPOSITORY
        and selected.integration_ref == INTEGRATION_REF,
        "invalid-selection",
    )
    _hash(selected.head, _COMMIT)
    _hash(selected.tree, _COMMIT)
    _hash(selected.local_id, _LOCAL_ID)
    evidence = _role_map(selected.evidence, EVIDENCE_ROLES, limits)
    artifacts = _role_map(selected.artifacts, ARTIFACT_ROLES, limits)
    _require(
        type(selected.steps) is tuple and 0 < len(selected.steps) <= limits.steps,
        "invalid-step",
    )
    for step in selected.steps:
        _require(type(step) is Step and type(step.command) is tuple, "invalid-step")
        _step(step.kind, step.command, limits)
    _require(len(set(selected.steps)) == len(selected.steps), "duplicate-step")
    return evidence, artifacts


def _json(data, limits):
    def pairs(items):
        value = {}
        for key, item in items:
            _require(key not in value, "duplicate-json-key")
            value[key] = item
        return value

    def invalid(_):
        raise Rejected("invalid-json-number")

    try:
        value = json.loads(
            data, object_pairs_hook=pairs, parse_float=invalid, parse_constant=invalid
        )
        pending = [(value, 0)]
        while pending:
            item, depth = pending.pop()
            _require(depth <= 64, "json-limit")
            if type(item) is dict:
                _require(len(item) <= 10000, "json-limit")
                pending.extend((key, depth + 1) for key in item)
                pending.extend((child, depth + 1) for child in item.values())
            elif type(item) is list:
                _require(len(item) <= 10000, "json-limit")
                pending.extend((child, depth + 1) for child in item)
            elif type(item) is str:
                _require(
                    len(item) <= limits.string
                    and not any(0xD800 <= ord(c) <= 0xDFFF for c in item),
                    "json-limit",
                )
            elif type(item) is int:
                _require(-(2**63) <= item < 2**63, "json-limit")
        return value
    except (ValueError, UnicodeError, RecursionError) as error:
        if isinstance(error, Rejected):
            raise
        raise Rejected("invalid-json") from None


def _signature(value):
    return (
        value.st_dev,
        value.st_ino,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
    )


class _Files:
    def __init__(self, root):
        _require(isinstance(root, Path) and root.is_absolute(), "invalid-root")
        _require(str(root.resolve(strict=True)) == str(root), "invalid-root")
        self.root = root
        self.fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)

    def close(self):
        os.close(self.fd)

    def relative(self, absolute):
        _require(type(absolute) is str, "invalid-path")
        prefix = str(self.root) + "/"
        _require(absolute.startswith(prefix), "invalid-path")
        value = absolute[len(prefix) :]
        _path(value)
        return value

    def pointer(self, path, digest, blob):
        _require(
            self.relative(path) == blob.path and digest == blob.sha256,
            "pointer-mismatch",
        )

    def read(self, path, digest, cap, *, size=None, collect=False):
        parts = _path(path).parts
        _hash(digest)
        directory = os.dup(self.fd)
        directories = [directory]
        links = []
        fd = None
        try:
            for component in parts[:-1]:
                child = os.open(
                    component,
                    os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                    dir_fd=directory,
                )
                links.append((directory, component, child))
                directories.append(child)
                directory = child
            fd = os.open(
                parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory
            )
            before = os.fstat(fd)
            _require(stat.S_ISREG(before.st_mode), "nonregular-file")
            _require(
                0 <= before.st_size <= cap and (size is None or size == before.st_size),
                "size-limit",
            )
            actual = hashlib.sha256()
            data = bytearray() if collect else None
            total = 0
            while True:
                chunk = os.read(fd, min(1048576, cap - total + 1))
                if not chunk:
                    break
                total += len(chunk)
                _require(total <= cap, "size-limit")
                actual.update(chunk)
                if collect:
                    data.extend(chunk)
            after = os.fstat(fd)
            root_named = os.stat(self.root, follow_symlinks=False)
            root_opened = os.fstat(self.fd)
            _require(
                (root_named.st_dev, root_named.st_ino, root_named.st_mode)
                == (root_opened.st_dev, root_opened.st_ino, root_opened.st_mode),
                "changed-file",
            )
            for parent, component, child in links:
                opened = os.fstat(child)
                named_dir = os.stat(component, dir_fd=parent, follow_symlinks=False)
                _require(
                    (opened.st_dev, opened.st_ino, opened.st_mode)
                    == (named_dir.st_dev, named_dir.st_ino, named_dir.st_mode),
                    "changed-file",
                )
            named = os.stat(parts[-1], dir_fd=directory, follow_symlinks=False)
            _require(
                _signature(before) == _signature(after) == _signature(named)
                and total == before.st_size,
                "changed-file",
            )
            _require(actual.hexdigest() == digest, "digest-mismatch")
            return (bytes(data) if collect else None), Blob(path, digest, total)
        finally:
            if fd is not None:
                os.close(fd)
            for directory in reversed(directories):
                os.close(directory)


def _source_map(value, limits):
    _require(
        type(value) is dict and 0 < len(value) <= limits.source_files,
        "invalid-source-map",
    )
    for path, digest in value.items():
        _path(path)
        _hash(digest)
    return value


def _identity(value, selected, *, source=False):
    _require(type(value) is dict, "invalid-fields")
    head, tree = ("source_head", "source_tree") if source else ("head", "tree")
    _require(
        value.get(head) == selected.head and value.get(tree) == selected.tree,
        "source-mismatch",
    )


def _verify(record_bytes, selected, evidence, artifacts, files, packages):
    limits = selected.limits
    _require(
        type(record_bytes) is bytes and len(record_bytes) <= limits.record,
        "record-limit",
    )
    record = _json(record_bytes, limits)
    _keys(
        record,
        (
            "version",
            "kind",
            "local_id",
            "repository",
            "head",
            "tree",
            "integration_ref",
            "evidence",
            "artifacts",
        ),
    )
    _require(
        type(record["version"]) is int
        and record["version"] == 1
        and record["kind"] == "adbc-local-evidence-link",
        "invalid-profile",
    )
    for key in ("repository", "head", "tree", "integration_ref", "local_id"):
        _require(record[key] == getattr(selected, key), "source-mismatch")
    for key, refs, roles in (
        ("evidence", evidence, EVIDENCE_ROLES),
        ("artifacts", artifacts, ARTIFACT_ROLES),
    ):
        _keys(record[key], roles)
        for role, blob in refs.items():
            _keys(record[key][role], ("path", "sha256", "bytes"))
            _require(
                type(record[key][role]["bytes"]) is int
                and record[key][role] == vars(blob),
                "anchor-mismatch",
            )
    documents = {}
    for role, blob in evidence.items():
        json_role = role in (
            "source_freeze",
            "review_receipt",
            "validation_receipt",
            "case_plan",
        )
        data, _ = files.read(
            blob.path,
            blob.sha256,
            limits.evidence_json if json_role else limits.body,
            size=blob.bytes,
            collect=json_role,
        )
        if json_role:
            documents[role] = _json(data, limits)
    source, review, validation, plan = (
        documents[k]
        for k in ("source_freeze", "review_receipt", "validation_receipt", "case_plan")
    )
    _identity(source, selected, source=True)
    _identity(review, selected, source=True)
    _require(
        _source_map(source.get("verified_files"), limits)
        == _source_map(review.get("source_files"), limits),
        "source-map-mismatch",
    )
    _require(
        review.get("verdict") == "APPROVE"
        and type(review.get("findings")) is list
        and review["findings"] == [],
        "unapproved-review",
    )
    for path_key, hash_key, role in (
        ("review_body", "review_body_sha256", "review_body"),
        ("checks", "checks_sha256", "review_checks"),
        ("root_full_validation", "root_full_validation_sha256", "validation_receipt"),
    ):
        files.pointer(review.get(path_key), review.get(hash_key), evidence[role])
    _require(
        type(validation) is dict
        and validation.get("source") == selected.head
        and validation.get("tree") == selected.tree
        and validation.get("status") == "success"
        and validation.get("packages") is True
        and validation.get("remote_or_aws_operations") is False,
        "invalid-validation",
    )
    finished = validation.get("finished_at")
    _require(type(finished) is str, "nonterminal-validation")
    try:
        _require(
            datetime.fromisoformat(finished).utcoffset() is not None,
            "nonterminal-validation",
        )
    except ValueError:
        raise Rejected("nonterminal-validation") from None
    validator = validation.get("validator_source")
    _keys(validator, ("path", "sha256"))
    files.pointer(validator["path"], validator["sha256"], evidence["validator_source"])
    _keys(plan, ("version", "kind", "repository", "head", "tree", "steps"))
    _require(
        type(plan["version"]) is int
        and plan["version"] == 1
        and plan["kind"] == "adbc-local-evidence-case-plan"
        and plan["repository"] == selected.repository,
        "invalid-case-plan",
    )
    _identity(plan, selected)
    _require(
        type(plan["steps"]) is list and len(plan["steps"]) == len(selected.steps),
        "invalid-step",
    )
    steps = validation.get("steps")
    _require(type(steps) is list and len(steps) == len(selected.steps), "invalid-step")
    logs = []
    for expected, planned, observed in zip(selected.steps, plan["steps"], steps):
        _keys(planned, ("kind", "command"))
        _require(type(observed) is dict, "invalid-step")
        for step in (planned, observed):
            _require(
                type(step.get("command")) is list
                and _step(step.get("kind"), step["command"], limits) == expected,
                "step-mismatch",
            )
        _require(
            type(observed.get("exit")) is int and observed["exit"] == 0, "failed-step"
        )
        path = files.relative(observed.get("log"))
        _, blob = files.read(path, observed.get("log_sha256"), limits.body)
        logs.append(blob)
    _require(len({log.path for log in logs}) == len(logs), "duplicate-log")
    wheels = validation.get("wheels")
    _require(type(wheels) is list and len(wheels) == 2, "invalid-packages")
    expected_wheels = {
        artifacts[role].path: artifacts[role]
        for role in ("manager_wheel", "redshift_wheel")
    }
    seen = set()
    for wheel in wheels:
        _keys(wheel, ("path", "sha256"))
        path = packages.relative(wheel["path"])
        _require(path in expected_wheels and path not in seen, "package-mismatch")
        packages.pointer(wheel["path"], wheel["sha256"], expected_wheels[path])
        seen.add(path)
    archive = validation.get("installed_c_snapshot")
    _require(type(archive) is dict, "invalid-packages")
    packages.pointer(archive.get("path"), archive.get("sha256"), artifacts["c_archive"])
    for blob in artifacts.values():
        packages.read(blob.path, blob.sha256, limits.artifact, size=blob.bytes)
    canonical = json.dumps(
        record,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
        allow_nan=False,
    ).encode("utf-8")
    return LinkedEvidence(
        "local-evidence-linked",
        hashlib.sha256(record_bytes).hexdigest(),
        hashlib.sha256(canonical).hexdigest(),
        tuple((role, evidence[role]) for role in EVIDENCE_ROLES),
        tuple((role, artifacts[role]) for role in ARTIFACT_ROLES),
        tuple(logs),
    )


def verify_local_evidence(record_bytes, *, selected, roots):
    """Link selected historical proof bytes only, with no execution/authority."""
    opened = []
    try:
        evidence, artifacts = _selection(selected)
        _require(type(roots) is Roots, "invalid-root")
        files = _Files(roots.evidence)
        opened.append(files)
        packages = _Files(roots.artifacts)
        opened.append(packages)
        return _verify(record_bytes, selected, evidence, artifacts, files, packages)
    except Rejected:
        raise
    except (OSError, ValueError, RuntimeError):
        raise Rejected("file-unavailable") from None
    except MemoryError:
        raise Rejected("allocation-failed") from None
    finally:
        for root in opened:
            root.close()
