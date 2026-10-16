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

"""Task008a structural contract. This module cannot authorize a live run."""

import argparse
import hashlib
import json
import re
import time
from pathlib import Path

ACCOUNT = "385207570137"
REGION = "eu-north-1"
NAME = "adbc-qualification-20261016"
ROLE = f"arn:aws:iam::{ACCOUNT}:role/adbc-isolated-qualification-ci"
SUBJECT = "repo:vahid110/arrow-adbc:ref:refs/heads/feature/pgwire-core-redshift"
PROTECTED = (
    "odbcpp",  # Includes protected Serverless and provisioned cluster/groups.
    "sg-06227a706295e3f05",
    "d41cff82-7eb8-410d-be60-1d467c335756",
    "adbc-redshift-ci",
)
UUID = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}")
SHA = re.compile(r"[0-9a-f]{64}")
COMMIT = re.compile(r"[0-9a-f]{40}")
IDENTITY = re.compile(r"arn:aws:iam::385207570137:(?:role|user)/[A-Za-z0-9+=,.@_/-]+")


class Rejected(ValueError):
    """A safe, fixed diagnostic; never include input values or credentials."""


def require(condition, message):
    if not condition:
        raise Rejected(message)


def digest(value):
    return hashlib.sha256(
        json.dumps(
            value, sort_keys=True, separators=(",", ":"), allow_nan=False
        ).encode()
    ).hexdigest()


def manifest_digest(manifest):
    # Evidence observes this exact payload; evidence itself is not circularly hashed.
    return digest({k: v for k, v in manifest.items() if k != "evidence"})


def _shape(value, schema):
    if isinstance(schema, dict):
        require(
            type(value) is dict and value.keys() == schema.keys(),
            "unknown or missing fields",
        )
        for key, rule in schema.items():
            _shape(value[key], rule)
    elif isinstance(schema, re.Pattern):
        require(
            type(value) is str and schema.fullmatch(value) is not None,
            "malformed identity or hash",
        )
    elif schema is list:
        require(type(value) is list, "authorization resources must be a list")
    elif schema is int:
        require(type(value) is int and value > 0, "invalid timestamp or run")
    else:
        require(
            type(value) is type(schema) and value == schema, "unexpected contract value"
        )


def _artifact():
    return {
        "sha256": SHA,
        "run_id": int,
        "source_sha": COMMIT,
        "installed_tree_sha256": SHA,
        "entrypoint": "AdbcDriverRedshiftInit",
    }


def schema():
    resource = {
        "name": NAME,
        "id": UUID,
        "arn": re.compile(
            rf"arn:aws:redshift-serverless:{REGION}:{ACCOUNT}:(?:namespace|workgroup)/[0-9a-f-]+"
        ),
    }
    receipt = {
        "attester": IDENTITY,
        "observed_at": int,
        "expires_at": int,
        "manifest_sha256": SHA,
        "observations_sha256": SHA,
        "review_receipt_sha256": SHA,
    }
    return {
        "version": 1,
        "purpose": re.compile(r"offline-synthetic|dedicated-qualification"),
        "account": ACCOUNT,
        "region": REGION,
        "namespace": {**resource, "database": "adbc_qualification"},
        "workgroup": {
            **resource,
            "namespace_id": UUID,
            "endpoint": f"{NAME}.{ACCOUNT}.{REGION}.redshift-serverless.amazonaws.com",
            "port": 5439,
            "security_group_name": NAME,
            "security_group_id": re.compile(r"sg-[0-9a-f]{17}"),
        },
        "role": {
            "arn": ROLE,
            "oidc_provider": (
                f"arn:aws:iam::{ACCOUNT}:oidc-provider/"
                "token.actions.githubusercontent.com"
            ),
            "audience": "sts.amazonaws.com",
            "subject": SUBJECT,
            "policy_sha256": SHA,
            "trust_sha256": SHA,
            "authorization_resources": list,
        },
        "controls": {
            "base_rpu": 4,
            "max_rpu": 4,
            "automatic_optimization": False,
            "capacity_growth": False,
            "usage_limit_id": UUID,
            "usage_workgroup_arn": re.compile(r"arn:aws:redshift-serverless:.+"),
            "usage_period": "daily",
            "usage_rpu_hours": 4,
            "usage_action": "deactivate",
            "incremental_usd": "5.00",
            "query_seconds": 900,
            "compute_seconds": 3600,
            "paid_retries": 0,
        },
        "binding": {
            "repository": "vahid110/arrow-adbc",
            "ref": "refs/heads/feature/pgwire-core-redshift",
            "source_sha": COMMIT,
            "merged_sha": COMMIT,
            "ci_receipt_sha256": SHA,
            "independent_review_receipt_sha256": SHA,
            "run_id": int,
            "platform": "ubuntu-x86_64",
            "c_archive": {
                **_artifact(),
                "workflow": ".github/workflows/pgwire-package.yml",
            },
            "python_wheel": {
                **_artifact(),
                "workflow": re.compile(
                    r"\.github/workflows/pgwire-python(?:-wheels)?\.yml"
                ),
            },
        },
        "window": {
            "not_before": int,
            "admit_before": int,
            "query_deadline": int,
            "compute_deadline": int,
            "teardown_deadline": int,
        },
        "cleanup": {
            "owner": IDENTITY,
            "external_provisioner": IDENTITY,
            "fixture_schema": re.compile(r"adbc_q_[0-9]+"),
            "ingress": "exact-owned-rule-id-and-ipv4-/32",
            "sessions": "close-owned",
            "objects": "owned-fixture-only",
            "verification": "independent-observation-required",
            "teardown": "external-new-resources-only",
        },
        "tls": {
            "mode": "verify-full",
            "hostname": f"{NAME}.{ACCOUNT}.{REGION}.redshift-serverless.amazonaws.com",
            "trusted_ca_sha256": SHA,
        },
        "evidence": {
            "kind": "external-observations-requiring-independent-verification",
            "provisioning": receipt,
            "usage": receipt,
            "cleanup": receipt,
            "source_artifacts": receipt,
        },
    }


def validate_structure(manifest, *, source_sha, run_id, now=None):
    """Validate claims, not AWS truth. No clients/imports/network are constructed."""
    now = int(time.time()) if now is None else now
    # Scan all strings (including authorization resources and evidence) before shape.
    encoded = json.dumps(manifest, allow_nan=False).lower()
    require(
        not any(value in encoded for value in PROTECTED),
        "protected resource or fallback",
    )
    # Lists have one exact identity set, checked below (no wildcard resources).
    _shape(manifest, schema())
    wg, ns = manifest["workgroup"], manifest["namespace"]
    prefix = f"arn:aws:redshift-serverless:{REGION}:{ACCOUNT}:"
    require(wg["arn"] == prefix + "workgroup/" + wg["id"], "workgroup ARN mismatch")
    require(ns["arn"] == prefix + "namespace/" + ns["id"], "namespace ARN mismatch")
    require(
        wg["namespace_id"] == ns["id"] and wg["id"] != ns["id"],
        "namespace association mismatch",
    )
    sg = f"arn:aws:ec2:{REGION}:{ACCOUNT}:security-group/" + wg["security_group_id"]
    require(
        manifest["role"]["authorization_resources"] == [wg["arn"], ns["arn"], sg],
        "authorization resources mismatch",
    )
    require(
        manifest["controls"]["usage_workgroup_arn"] == wg["arn"], "usage scope mismatch"
    )
    binding = manifest["binding"]
    require(
        binding["source_sha"] == binding["merged_sha"] == source_sha
        and binding["run_id"] == run_id,
        "source or run mismatch",
    )
    for artifact in ("c_archive", "python_wheel"):
        require(
            binding[artifact]["source_sha"] == source_sha, "artifact source mismatch"
        )
    window = manifest["window"]
    require(
        window["not_before"]
        <= now
        < window["admit_before"]
        <= window["query_deadline"]
        <= window["compute_deadline"]
        <= window["teardown_deadline"],
        "expired or invalid admission window",
    )
    require(
        window["query_deadline"] - window["not_before"] <= 900
        and window["compute_deadline"] - window["not_before"] <= 3600
        and window["teardown_deadline"] - window["not_before"] <= 3600,
        "unbounded execution window",
    )
    require(
        manifest["cleanup"]["fixture_schema"] == f"adbc_q_{run_id}",
        "fixture ownership mismatch",
    )
    require(
        manifest["cleanup"]["external_provisioner"] != ROLE
        and manifest["cleanup"]["owner"] != ROLE,
        "external cleanup owner required",
    )
    require(
        manifest["evidence"]["cleanup"]["attester"]
        not in (
            manifest["cleanup"]["owner"],
            manifest["cleanup"]["external_provisioner"],
        ),
        "independent cleanup observer required",
    )
    for kind, receipt in manifest["evidence"].items():
        if kind == "kind":
            continue
        require(receipt["attester"] != ROLE, "independent attester required")
        require(
            receipt["manifest_sha256"] == manifest_digest(manifest),
            "evidence binding mismatch",
        )
        require(
            now - 900 <= receipt["observed_at"] <= now < receipt["expires_at"]
            and receipt["expires_at"] >= window["teardown_deadline"],
            "stale observation",
        )
    return manifest_digest(manifest)


def admit_live(manifest, **context):
    validate_structure(manifest, **context)
    # Deliberately no verifier callback/boolean/env override. A caller-supplied receipt
    # is not proof. Task008a has neither trusted external evidence nor live adapters.
    raise Rejected(
        "live admission unavailable: independent external verification required"
    )


def load(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON field")
            result[key] = value
        return result

    return json.loads(
        Path(path).read_text(),
        object_pairs_hook=unique,
        parse_constant=lambda _: require(False, "non-finite JSON"),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest")
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--run-id", type=int, required=True)
    parser.add_argument("--structural-only", action="store_true")
    args = parser.parse_args()
    try:
        manifest = load(args.manifest)
        context = {"source_sha": args.source_sha, "run_id": args.run_id}
        if args.structural_only:
            validate_structure(manifest, **context)
            print(
                "Structural claims valid; external attestation UNVERIFIED; "
                "live admission CLOSED"
            )
        else:
            admit_live(manifest, **context)
    except (ValueError, TypeError, KeyError, OSError):
        parser.exit(1, "Qualification rejected; no live admission granted.\n")


if __name__ == "__main__":
    main()
