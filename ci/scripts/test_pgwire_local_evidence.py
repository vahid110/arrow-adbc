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

"""Synthetic offline fixtures only; not real provenance or execution proof."""

import ast
import copy
import hashlib
import importlib.util
import json
import os
import sys
import tempfile
import unittest
from dataclasses import FrozenInstanceError, replace
from pathlib import Path
from unittest.mock import patch

# Exact sibling import works under Python -I without adding a source search path.
MODULE = Path(__file__).with_name("pgwire_local_evidence.py")
SPEC = importlib.util.spec_from_file_location("pgwire_local_evidence", MODULE)
link = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = link
SPEC.loader.exec_module(link)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


class SyntheticBundle:
    """Genuine-shaped tiny files; root selection here is deliberately synthetic."""

    def __init__(self, path):
        self.evidence = path / "evidence"
        self.artifacts = path / "artifacts"
        self.evidence.mkdir()
        self.artifacts.mkdir()
        self.roots = link.Roots(self.evidence, self.artifacts)
        self.head = "1" * 40
        self.tree = "2" * 40
        self.steps = (
            link.Step("compile", ("NOT_EXECUTED", "literal argument")),
            link.Step("synthetic-tests", ("NEVER_RUN",)),
        )
        self.package = {}
        for role, name in (
            ("c_archive", "prefix.tar.gz"),
            ("manager_wheel", "manager-macos.whl"),
            ("redshift_wheel", "redshift-macos.whl"),
        ):
            # G0 hashes bytes only. These are not valid packages/native proof.
            self.package[role] = self.put(self.artifacts, name, role.encode())
        self.validator = self.put(
            self.evidence, "validator.py", b"NEVER EXECUTE THIS SOURCE"
        )
        self.body = self.put(self.evidence, "review.md", b"SYNTHETIC REVIEW BODY")
        self.checks = self.put(self.evidence, "checks.json", b'{"synthetic":true}')
        self.logs = [
            self.put(self.evidence, "logs/step%d.log" % i, b"SYNTHETIC LOG")
            for i in range(2)
        ]
        self.source = {
            "source_head": self.head,
            "source_tree": self.tree,
            "verified_files": {"c/fixture.cc": "a" * 64},
            "root_validation": "pending30steps",
            "independent_review": "pendingexactFULL",
        }
        self.review = {
            "source_head": self.head,
            "source_tree": self.tree,
            "verdict": "APPROVE",
            "findings": [],
            "source_files": copy.deepcopy(self.source["verified_files"]),
            "review_body": str(self.evidence / self.body.path),
            "review_body_sha256": self.body.sha256,
            "checks": str(self.evidence / self.checks.path),
            "checks_sha256": self.checks.sha256,
            "scope": "SYNTHETIC, no real independent approval",
        }
        self.validation = {
            "source": self.head,
            "tree": self.tree,
            "status": "success",
            "packages": True,
            "remote_or_aws_operations": False,
            "finished_at": "2026-10-19T10:00:00+00:00",
            "steps": [
                {
                    "kind": step.kind,
                    "command": list(step.command),
                    "exit": 0,
                    "log": str(self.evidence / log.path),
                    "log_sha256": log.sha256,
                }
                for step, log in zip(self.steps, self.logs)
            ],
            "validator_source": {
                "path": str(self.evidence / self.validator.path),
                "sha256": self.validator.sha256,
            },
            "wheels": [
                {
                    "path": str(self.artifacts / self.package[role].path),
                    "sha256": self.package[role].sha256,
                }
                for role in ("manager_wheel", "redshift_wheel")
            ],
            "installed_c_snapshot": {
                "path": str(self.artifacts / self.package["c_archive"].path),
                "sha256": self.package["c_archive"].sha256,
                "note": "Synthetic bytes; no archive interpretation",
            },
        }
        self.plan = {
            "version": 1,
            "kind": "adbc-local-evidence-case-plan",
            "repository": link.REPOSITORY,
            "head": self.head,
            "tree": self.tree,
            "steps": [
                {"kind": step.kind, "command": list(step.command)}
                for step in self.steps
            ],
        }
        self.refresh()

    @staticmethod
    def put(root, name, data):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return link.Blob(name, sha(data), len(data))

    def refresh(self):
        validation = self.put(
            self.evidence, "validation.json", encoded(self.validation)
        )
        self.review["root_full_validation"] = str(self.evidence / validation.path)
        self.review["root_full_validation_sha256"] = validation.sha256
        evidence = {
            "source_freeze": self.put(
                self.evidence, "freeze.json", encoded(self.source)
            ),
            "review_receipt": self.put(
                self.evidence, "review.json", encoded(self.review)
            ),
            "validation_receipt": validation,
            "review_body": self.body,
            "review_checks": self.checks,
            "validator_source": self.validator,
            "case_plan": self.put(self.evidence, "case-plan.json", encoded(self.plan)),
        }
        self.selected = link.Selected(
            link.REPOSITORY,
            self.head,
            self.tree,
            link.INTEGRATION_REF,
            "3" * 32,
            tuple(evidence.items()),
            tuple(self.package.items()),
            self.steps,
        )
        self.record = {
            "version": 1,
            "kind": "adbc-local-evidence-link",
            "local_id": "3" * 32,
            "repository": link.REPOSITORY,
            "head": self.head,
            "tree": self.tree,
            "integration_ref": link.INTEGRATION_REF,
            "evidence": {k: vars(v) for k, v in evidence.items()},
            "artifacts": {k: vars(v) for k, v in self.package.items()},
        }
        self.data = encoded(self.record)

    def reanchor_blob(self, role, data):
        refs = dict(self.selected.evidence)
        refs[role] = self.put(self.evidence, refs[role].path, data)
        self.selected = replace(self.selected, evidence=tuple(refs.items()))
        self.record["evidence"][role] = vars(refs[role])
        self.data = encoded(self.record)

    def verify(self, data=None, selected=None):
        return link.verify_local_evidence(
            self.data if data is None else data,
            selected=self.selected if selected is None else selected,
            roots=self.roots,
        )


class LocalEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.bundle = SyntheticBundle(Path(self.temp.name).resolve())

    def tearDown(self):
        self.temp.cleanup()

    def rejected(self, data=None, selected=None):
        with self.assertRaises(link.Rejected) as caught:
            self.bundle.verify(data, selected)
        self.assertRegex(str(caught.exception), r"^[a-z-]+$")

    def test_complete_synthetic_bundle_is_frozen_and_only_links_bytes(self):
        result = self.bundle.verify()
        self.assertEqual(result.status, "local-evidence-linked")
        self.assertEqual(result.record_sha256, sha(self.bundle.data))
        self.assertEqual(result.payload_sha256, sha(encoded(self.bundle.record)))
        self.assertEqual(len(result.logs), 2)
        self.assertEqual(dict(result.artifacts), self.bundle.package)
        with self.assertRaises(FrozenInstanceError):
            result.status = "authorized"
        pretty = json.dumps(self.bundle.record, indent=2).encode()
        other = self.bundle.verify(pretty)
        self.assertNotEqual(result.record_sha256, other.record_sha256)
        self.assertEqual(result.payload_sha256, other.payload_sha256)
        # Historical pending labels are not rewritten or treated as terminal facts.
        self.assertEqual(self.bundle.source["root_validation"], "pending30steps")
        self.assertFalse(hasattr(result, "authorized"))

    def test_strict_record_json_and_identity(self):
        b = self.bundle
        for raw in (
            b"{}",
            b"null",
            b"[]",
            b"\xff",
            b'{"version":1,"version":1}',
            b'{"version":NaN}',
            b'{"version":1.0}',
            b"[" * 1000,
        ):
            with self.subTest(raw=raw[:32]):
                self.rejected(raw)
        for key, bad in (
            ("version", True),
            ("version", 1.0),
            ("version", 2),
            ("head", "f" * 40),
            ("tree", "e" * 40),
            ("repository", "other/repo"),
            ("integration_ref", "refs/heads/main"),
            ("local_id", "4" * 32),
            ("kind", "ci-evidence"),
        ):
            value = copy.deepcopy(b.record)
            value[key] = bad
            with self.subTest(key=key, bad=bad):
                self.rejected(encoded(value))
        value = copy.deepcopy(b.record)
        value["fake_run_id"] = 123
        self.rejected(encoded(value))
        value = copy.deepcopy(b.record)
        value["evidence"]["source_freeze"]["bytes"] = True
        self.rejected(encoded(value))

    def test_source_and_review_must_match_even_if_selected(self):
        b = self.bundle
        mutations = [
            (b.source, "source_head", "f" * 40),
            (b.source, "source_tree", "f" * 40),
            (b.source, "verified_files", {}),
            (b.source, "verified_files", {"../escape": "a" * 64}),
            (b.review, "source_files", {"different.cc": "a" * 64}),
            (b.review, "verdict", "CHANGES REQUESTED"),
            (b.review, "findings", ["unresolved"]),
            (b.review, "findings", {}),
            (b.review, "review_body_sha256", "f" * 64),
            (b.review, "checks_sha256", "f" * 64),
            (b.review, "review_body", str(b.evidence / "other.md")),
        ]
        for obj, key, bad in mutations:
            old = obj[key]
            obj[key] = bad
            b.refresh()
            with self.subTest(key=key):
                self.rejected()
            obj[key] = old
        b.refresh()
        b.verify()

    def test_referenced_anchors_are_not_merely_self_consistent(self):
        b = self.bundle
        b.review["root_full_validation_sha256"] = "f" * 64
        b.reanchor_blob("review_receipt", encoded(b.review))
        self.rejected()
        b.refresh()
        b.review["root_full_validation"] = str(b.evidence) + "/./validation.json"
        b.reanchor_blob("review_receipt", encoded(b.review))
        self.rejected()
        b.refresh()
        b.review["checks"] = str(b.evidence.parent / "outside-private-checks.json")
        b.reanchor_blob("review_receipt", encoded(b.review))
        self.rejected()
        for invalid in (
            b'{"source_head":"x","source_head":"y"}',
            b'{"number":1.5}',
            b'{"number":Infinity}',
            b"null",
        ):
            b.refresh()
            b.reanchor_blob("source_freeze", invalid)
            self.rejected()

    def test_validation_status_terminal_flags_and_validator_pointer(self):
        b = self.bundle
        for key, bad in (
            ("source", "f" * 40),
            ("tree", "f" * 40),
            ("status", "running"),
            ("packages", 1),
            ("remote_or_aws_operations", 0),
            ("finished_at", None),
            ("finished_at", "2026-02-30T00:00:00+00:00"),
            ("finished_at", "2026-10-19T00:00:00"),
            (
                "validator_source",
                {"path": str(b.evidence / b.validator.path), "sha256": "f" * 64},
            ),
        ):
            old = b.validation[key]
            b.validation[key] = bad
            b.refresh()
            with self.subTest(key=key, bad=bad):
                self.rejected()
            b.validation[key] = old
        b.refresh()

    def test_steps_must_be_exact_order_complete_unique_successful_and_logged(self):
        b = self.bundle
        original = copy.deepcopy(b.validation["steps"])
        for altered in (
            [],
            original[:1],
            original + original[:1],
            list(reversed(original)),
            [original[0], original[0]],
        ):
            b.validation["steps"] = copy.deepcopy(altered)
            b.refresh()
            self.rejected()
        for key, bad in (
            ("exit", True),
            ("exit", 1),
            ("exit", None),
            ("kind", "unexpected"),
            ("command", ["different"]),
            ("command", "NOT_EXECUTED"),
            ("log_sha256", "f" * 64),
            ("log", str(b.evidence / "missing.log")),
        ):
            b.validation["steps"] = copy.deepcopy(original)
            b.validation["steps"][0][key] = bad
            b.refresh()
            with self.subTest(key=key):
                self.rejected()
        b.validation["steps"] = original
        b.refresh()

    def test_case_plan_is_anchored_and_not_a_success_substitute(self):
        b = self.bundle
        for key, bad in (
            ("head", "f" * 40),
            ("tree", "e" * 40),
            ("version", True),
            ("kind", "native-all21"),
            ("steps", []),
            ("steps", list(reversed(b.plan["steps"]))),
        ):
            old = b.plan[key]
            b.plan[key] = bad
            b.refresh()
            self.rejected()
            b.plan[key] = old
        b.refresh()
        selected = b.selected
        b.source["historical_note"] = "self-consistent but unanchored"
        b.refresh()
        self.rejected(selected=selected)

    def test_every_actual_dependency_byte_is_hashed(self):
        b = self.bundle
        paths = [b.evidence / ref.path for _, ref in b.selected.evidence]
        paths += [b.evidence / ref.path for ref in b.logs]
        paths += [b.artifacts / ref.path for ref in b.package.values()]
        for path in paths:
            original = path.read_bytes()
            path.write_bytes(bytes([original[0] ^ 1]) + original[1:])
            with self.subTest(path=path.name):
                self.rejected()
            path.write_bytes(original)
        b.verify()

    def test_package_receipt_roles_and_paths(self):
        b = self.bundle
        original = copy.deepcopy(b.validation)
        for bad in ([], b.validation["wheels"][:1], [b.validation["wheels"][0]] * 2):
            b.validation["wheels"] = bad
            b.refresh()
            self.rejected()
        b.validation = copy.deepcopy(original)
        b.validation["wheels"][0]["sha256"] = "f" * 64
        b.refresh()
        self.rejected()
        b.validation = copy.deepcopy(original)
        b.validation["installed_c_snapshot"]["path"] = str(
            b.artifacts / b.package["manager_wheel"].path
        )
        b.refresh()
        self.rejected()

    def test_path_escape_alias_symlink_and_nonregular_rejection(self):
        b = self.bundle
        for bad in (
            "../escape",
            "/absolute",
            "./freeze.json",
            "a//b",
            "a/../b",
            "a/",
            "a\\b",
            "a\0b",
        ):
            refs = dict(b.selected.evidence)
            refs["source_freeze"] = replace(refs["source_freeze"], path=bad)
            with self.subTest(path=bad):
                self.rejected(
                    selected=replace(b.selected, evidence=tuple(refs.items()))
                )
        target = b.evidence / b.logs[0].path
        saved = target.read_bytes()
        target.unlink()
        target.symlink_to(b.evidence / b.logs[1].path)
        self.rejected()
        target.unlink()
        target.mkdir()
        self.rejected()
        target.rmdir()
        os.mkfifo(target)
        self.rejected()
        target.unlink()
        target.write_bytes(saved)
        logs_dir = b.evidence / "logs"
        real = b.evidence / "other-logs"
        logs_dir.rename(real)
        logs_dir.symlink_to(real, target_is_directory=True)
        self.rejected()
        logs_dir.unlink()
        real.rename(logs_dir)
        b.validation["steps"][0]["log"] = str(b.evidence) + "/logs/../logs/step0.log"
        b.refresh()
        self.rejected()

    def test_exact_limits_and_immediate_neighbors(self):
        b = self.bundle
        limits = link.Limits(
            record=len(b.data),
            evidence_json=max(
                ref.bytes
                for role, ref in b.selected.evidence
                if role
                in (
                    "source_freeze",
                    "review_receipt",
                    "validation_receipt",
                    "case_plan",
                )
            ),
            body=max(ref.bytes for ref in [b.body, b.checks, b.validator, *b.logs]),
            artifact=max(ref.bytes for ref in b.package.values()),
            steps=2,
            argv=2,
            source_files=1,
        )
        b.verify(selected=replace(b.selected, limits=limits))
        for name in (
            "record",
            "evidence_json",
            "body",
            "artifact",
            "steps",
            "argv",
            "source_files",
        ):
            with self.subTest(limit=name):
                self.rejected(
                    selected=replace(
                        b.selected,
                        limits=replace(limits, **{name: getattr(limits, name) - 1}),
                    )
                )
        self.rejected(selected=replace(b.selected, limits=link.Limits(record=1048577)))
        self.rejected(selected=replace(b.selected, limits=link.Limits(steps=True)))
        self.rejected(selected=replace(b.selected, steps=(b.steps[0], b.steps[0])))

    def test_selected_context_cannot_contain_mutable_or_malformed_anchors(self):
        b = self.bundle
        for selected in (
            replace(b.selected, evidence=list(b.selected.evidence)),
            replace(b.selected, steps=list(b.selected.steps)),
            replace(b.selected, head="G" * 40),
            replace(b.selected, local_id="A" * 32),
            replace(b.selected, evidence=b.selected.evidence[:-1]),
            replace(b.selected, steps=(link.Step("bad", (True,)),)),
        ):
            self.rejected(selected=selected)
        with self.assertRaises(link.Rejected):
            link.verify_local_evidence(
                b.data,
                selected=b.selected,
                roots=link.Roots(Path("relative"), b.artifacts),
            )

    def test_changed_file_during_read_is_rejected_even_with_same_bytes(self):
        b = self.bundle
        path = b.evidence / b.logs[0].path
        identity = path.stat().st_ino
        read = os.read
        changed = False

        def change_after_read(fd, count):
            nonlocal changed
            chunk = read(fd, count)
            if not changed and os.fstat(fd).st_ino == identity:
                changed = True
                current = path.stat()
                os.utime(path, ns=(current.st_atime_ns, current.st_mtime_ns + 1000000))
            return chunk

        with patch.object(link.os, "read", side_effect=change_after_read):
            self.rejected()
        self.assertTrue(changed)

    def test_module_has_no_execution_provider_or_archive_facility(self):
        tree = ast.parse(MODULE.read_text())
        imports = {
            node.names[0].name.split(".")[0]
            for node in ast.walk(tree)
            if isinstance(node, ast.Import)
        }
        imports |= {
            node.module.split(".")[0]
            for node in ast.walk(tree)
            if isinstance(node, ast.ImportFrom)
        }
        self.assertEqual(
            imports,
            {
                "hashlib",
                "json",
                "os",
                "re",
                "stat",
                "dataclasses",
                "datetime",
                "pathlib",
            },
        )
        calls = {
            node.func.id
            for node in ast.walk(tree)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
        }
        self.assertTrue(calls.isdisjoint({"exec", "eval", "__import__", "compile"}))
        self.assertFalse(hasattr(link, "main"))
        self.assertFalse(hasattr(link, "admit"))


if __name__ == "__main__":
    unittest.main()
