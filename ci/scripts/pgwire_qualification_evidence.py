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

"""External host evidence integration. No trust roots are enrolled in Task008b.

Signature authentication is NOT a caller-supplied key/callback. Enrollment needs
an independently reviewed source change after host/human identity verification.
Matching synthetic observations can never grant live admission or paid authority.
"""

import base64
import json

from pgwire_qualification_manifest import (
    COMMIT,
    SHA,
    Rejected,
    digest,
    manifest_digest,
    require,
    validate_structure,
)

# Out-of-band pinned issuer keys, not manifest inputs, env, profiles or receipt
# hashes. Intentionally empty: external host has not supplied authenticated keys
# or observations. No runner CLI supports adding keys.
TRUST_ROOTS = {}
CONSUMER = "adbc-installed-qualification-v1"
ROLES = ("provisioner", "independent-observer", "paid-test-authority")


def _keys(value, fields):
    require(
        type(value) is dict and set(value) == set(fields.split()),
        "invalid external evidence fields",
    )


def _hash(value, pattern=SHA):
    require(type(value) is str and pattern.fullmatch(value), "invalid evidence digest")


def compare_observations(manifest, observation, *, source_sha, run_id, now):
    """Pure offline comparison, returns NO authorization. Inputs remain untrusted."""
    validate_structure(manifest, source_sha=source_sha, run_id=run_id, now=now)
    _keys(
        observation,
        "version consumer manifest_sha256 run_id observed_at expires_at "
        "nonce source artifacts resources cleanup cleanup_acceptance paid",
    )
    require(
        observation["version"] == 1
        and observation["consumer"] == CONSUMER
        and observation["manifest_sha256"] == manifest_digest(manifest)
        and observation["run_id"] == run_id,
        "external run/manifest mismatch",
    )
    _hash(observation["nonce"])
    require(
        type(observation["observed_at"]) is int
        and type(observation["expires_at"]) is int
        and now - 900 <= observation["observed_at"] <= now < observation["expires_at"]
        and observation["expires_at"] == manifest["window"]["teardown_deadline"],
        "stale external evidence",
    )
    resources = {
        k: manifest[k]
        for k in (
            "account",
            "region",
            "namespace",
            "workgroup",
            "role",
            "controls",
            "tls",
        )
    }
    require(observation["resources"] == resources, "external resource mismatch")
    require(observation["cleanup"] == manifest["cleanup"], "external cleanup mismatch")
    require(
        observation["cleanup_acceptance"]
        == {
            "owner": manifest["cleanup"]["owner"],
            "external_provisioner": manifest["cleanup"]["external_provisioner"],
            "observer": manifest["evidence"]["cleanup"]["attester"],
            "deadline": manifest["window"]["teardown_deadline"],
            "manifest_sha256": manifest_digest(manifest),
            "run_id": run_id,
            "state": "accepted-exact-owned-fixtures-rules-and-new-resources-only",
        },
        "cleanup owner/observer acceptance missing",
    )
    source = observation["source"]
    _keys(
        source,
        "repository ref merged_sha merged_tree reviewed_head reviewed_tree pr "
        "original_checks native_review host_comment",
    )
    require(
        source["repository"] == manifest["binding"]["repository"]
        and source["ref"] == manifest["binding"]["ref"]
        and source["merged_sha"] == source_sha
        and source["merged_tree"] == source["reviewed_tree"],
        "merged/reviewed source mismatch",
    )
    for key in ("merged_tree", "reviewed_head", "reviewed_tree"):
        _hash(source[key], COMMIT)
    require(type(source["pr"]) is int and source["pr"] > 0, "missing merged PR")
    checks = source["original_checks"]
    require(type(checks) is list and checks, "missing original checks")
    names = set()
    for check in checks:
        _keys(check, "name id head_sha conclusion response_sha256")
        require(
            type(check["id"]) is int
            and check["id"] > 0
            and type(check["name"]) is str
            and check["name"] not in names
            and check["head_sha"] == source["reviewed_head"]
            and check["conclusion"] == "success",
            "original CI mismatch",
        )
        names.add(check["name"])
        _hash(check["response_sha256"])
    require(
        digest(checks) == manifest["binding"]["ci_receipt_sha256"],
        "original CI receipt mismatch",
    )
    # Complete list of triggered original checks is an authenticated host
    # observation, not replaceable by rerun-only green checks or local sidecars.
    review = source["native_review"]
    _keys(review, "run head_sha verdict receipt_sha256")
    require(
        type(review["run"]) is str
        and review["run"]
        and review["head_sha"] == source["reviewed_head"]
        and review["verdict"] == "approved"
        and review["receipt_sha256"]
        == manifest["binding"]["independent_review_receipt_sha256"],
        "native verdict mismatch",
    )
    _hash(review["receipt_sha256"])
    comment = source["host_comment"]
    _keys(
        comment, "id event head_sha native_receipt_sha256 body_sha256 response_sha256"
    )
    require(
        type(comment["id"]) is int
        and comment["id"] > 0
        and comment["event"] == "COMMENT"
        and comment["head_sha"] == source["reviewed_head"]
        and comment["native_receipt_sha256"] == review["receipt_sha256"],
        "host publication missing or mismatched",
    )
    _hash(comment["body_sha256"])
    _hash(comment["response_sha256"])
    artifacts = observation["artifacts"]
    _keys(artifacts, "c_archive python_wheel")
    for kind, artifact in artifacts.items():
        _keys(
            artifact,
            "artifact_id archive_sha256 payload_sha256 installed_tree_sha256 "
            "source_sha source_tree workflow workflow_ref run_id run_attempt "
            "run_conclusion artifact_name library module distribution_root",
        )
        binding = manifest["binding"][kind]
        require(
            artifact["payload_sha256"] == binding["sha256"]
            and artifact["installed_tree_sha256"] == binding["installed_tree_sha256"]
            and artifact["source_sha"] == source_sha
            and artifact["source_tree"] == source["merged_tree"]
            and artifact["workflow"] == binding["workflow"]
            and artifact["workflow_ref"]
            == "vahid110/arrow-adbc/"
            + binding["workflow"]
            + "@"
            + manifest["binding"]["ref"]
            and artifact["run_id"] == binding["run_id"]
            and artifact["run_conclusion"] == "success",
            "external artifact provenance mismatch",
        )
        for key in ("artifact_id", "run_id", "run_attempt"):
            require(
                type(artifact[key]) is int and artifact[key] > 0,
                "missing artifact/run identity",
            )
        _hash(artifact["archive_sha256"])
        # Origins are authenticated observations, subsequently compared against
        # local bytes and isolated load-only report by the admission consumer.
        require(
            type(artifact["artifact_name"]) is str
            and artifact["artifact_name"]
            and type(artifact["library"]) is str
            and artifact["library"].startswith("/"),
            "missing artifact origin",
        )
        if kind == "python_wheel":
            root = artifact["distribution_root"]
            require(
                type(root) is str
                and root.startswith("/")
                and artifact["module"] == root + "/adbc_driver_redshift/__init__.py"
                and artifact["library"]
                == root + "/adbc_driver_redshift/libadbc_driver_redshift.so",
                "external installed origin mismatch",
            )
        else:
            require(
                artifact["module"] is None
                and artifact["distribution_root"] is None
                and artifact["library"].endswith("/lib/libadbc_driver_redshift.so"),
                "external C origin mismatch",
            )
    for kind, value in (
        ("provisioning", resources),
        ("usage", manifest["controls"]),
        ("cleanup", observation["cleanup_acceptance"]),
        ("source_artifacts", {"source": source, "artifacts": artifacts}),
    ):
        require(
            manifest["evidence"][kind]["observations_sha256"] == digest(value)
            and manifest["evidence"][kind]["review_receipt_sha256"]
            == review["receipt_sha256"],
            "external observation reference mismatch",
        )
    require(
        observation["paid"]
        == {
            "manifest_sha256": manifest_digest(manifest),
            "run_id": run_id,
            "nonce": observation["nonce"],
            "window": manifest["window"],
            "controls": manifest["controls"],
            "automatic_retries": 0,
        },
        "separate finite paid admission mismatch",
    )
    return "untrusted observations match; NOT admission"


def verify_external(manifest, envelope, *, source_sha, run_id, now):
    """Pinned Ed25519 signatures over the exact canonical payload from three roles.

    No external root exists today. This boundary rejects before any SDK, artifact
    load, subprocess or ingress. Even future verified evidence requires durable
    host nonce consumption BEFORE credentials; this module does not grant a token.
    """
    validate_structure(manifest, source_sha=source_sha, run_id=run_id, now=now)
    require(
        bool(TRUST_ROOTS), "external trust roots unavailable; live admission closed"
    )
    _keys(envelope, "payload signatures")
    payload = envelope["payload"]
    compare_observations(
        manifest, payload, source_sha=source_sha, run_id=run_id, now=now
    )
    _keys(envelope["signatures"], " ".join(ROLES))
    seen = set()
    # cryptography is a verifier dependency only when independently enrolled host
    # keys exist, not a new database authentication method or provider framework.
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

        message = (
            CONSUMER
            + "\n"
            + json.dumps(
                payload, sort_keys=True, separators=(",", ":"), allow_nan=False
            )
        ).encode()
        for role in ROLES:
            signature = envelope["signatures"][role]
            _keys(signature, "issuer signature")
            issuer = signature["issuer"]
            require(
                issuer not in seen and (role, issuer) in TRUST_ROOTS,
                "unknown or nonindependent external issuer",
            )
            seen.add(issuer)
            key = Ed25519PublicKey.from_public_bytes(TRUST_ROOTS[(role, issuer)])
            key.verify(base64.b64decode(signature["signature"], validate=True), message)
    except BaseException:
        raise Rejected("External evidence authentication failed") from None
    # Host must atomically consume nonce bound to manifest/run and return an
    # independently authenticated one-shot receipt; no such integration is wired.
    raise Rejected("durable one-shot paid admission unavailable; live admission closed")
