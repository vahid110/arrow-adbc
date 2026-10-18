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

"""Exact nonsecret operation paths and the reviewed retained-action policy asset."""

import json
import re
from datetime import datetime, timezone
from pathlib import Path

from pgwire_qualification_manifest import ACCOUNT, COMMIT, REGION, SHA, UUID, require

LEGACY_PARAMETER = "/adbc/qualification/20261016/admission"
OPERATIONS = "/adbc/qualification/20261016/operations/"
TEMPLATE = Path(__file__).with_suffix(".json")


def operation_prefix(nonce):
    require(type(nonce) is str and SHA.fullmatch(nonce), "invalid operation nonce")
    return OPERATIONS + nonce + "/"


def admission_name(nonce):
    return operation_prefix(nonce) + "admission"


def host_attempt_name(nonce):
    return operation_prefix(nonce) + "host-attempt"


def claim_name(nonce, run):
    require(type(run) is int and run > 0, "invalid operation run")
    return operation_prefix(nonce) + "claim-" + str(run)


def initial_policy(nonce):
    return {
        "Version": "2012-10-17",
        "Statement": [
            {
                "Sid": "Identity",
                "Effect": "Allow",
                "Action": ["sts:GetCallerIdentity"],
                "Resource": "*",
            },
            {
                "Sid": "ReadSingleAdmission",
                "Effect": "Allow",
                "Action": ["ssm:GetParameter"],
                "Resource": f"arn:aws:ssm:{REGION}:{ACCOUNT}:parameter"
                + admission_name(nonce),
            },
        ],
    }


def checked_template(path, expected_sha):
    from pgwire_qualification import file_sha

    require(
        file_sha(path) == expected_sha and file_sha(TEMPLATE) == expected_sha,
        "runtime template is not the versioned contract",
    )
    return Path(path).read_text()


def runtime_policy(admission):
    require(
        type(admission["source_sha"]) is str
        and COMMIT.fullmatch(admission["source_sha"]),
        "invalid policy source",
    )
    nonce = admission["nonce"]
    admission_name(nonce)
    claim_name(nonce, admission["run_id"])
    for key in ("workgroup_id", "namespace_id"):
        value = admission["resources"][key]
        require(type(value) is str and UUID.fullmatch(value), "invalid policy resource")
    group = admission["resources"]["security_group_id"]
    require(
        type(group) is str and re.fullmatch(r"sg-[0-9a-f]{17}", group),
        "invalid policy resource",
    )
    encoded = TEMPLATE.read_text()
    for old, new in {
        "EXACT_OPERATION_NONCE": nonce,
        "EXACT_RUN_ID": str(admission["run_id"]),
        "EXACT_OWNED_WG": admission["resources"]["workgroup_id"],
        "EXACT_OWNED_NS": admission["resources"]["namespace_id"],
        "EXACT_OWNED_SG": admission["resources"]["security_group_id"],
        "EXACT_CUTOFF": datetime.fromtimestamp(
            admission["window"]["teardown_deadline"], timezone.utc
        ).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }.items():
        encoded = encoded.replace(old, new)
    require("EXACT_" not in encoded, "runtime policy binding incomplete")
    return json.loads(encoded)
