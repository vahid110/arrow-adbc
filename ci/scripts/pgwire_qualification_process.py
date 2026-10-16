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

"""Bounded private-process primitives, NOT live admission or ingress adapters."""

import json
import os
import selectors
import signal
import struct
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from pgwire_qualification import file_sha
from pgwire_qualification_manifest import (
    Rejected,
    admit_live,
    require,
    validate_structure,
)

MAX_URI = 4096
MAX_OUTPUT = 2048


def frame(uri):
    data = uri.encode("ascii")
    require(
        16 <= len(data) <= MAX_URI and all(32 < b < 127 for b in data),
        "invalid private input",
    )
    return struct.pack("!I", len(data)) + data


def validate_uri(uri, manifest, ca):
    """No arbitrary libpq options, alternative host, database, service or TLS mode."""
    parts = urlsplit(uri)
    require(
        parts.scheme == "postgresql"
        and parts.hostname == manifest["tls"]["hostname"]
        and parts.port == 5439
        and parts.path == "/adbc_qualification"
        and parts.username
        and parts.password
        and not parts.fragment,
        "credential target mismatch",
    )
    query = parse_qs(parts.query, strict_parsing=True)
    require(
        query
        == {
            "sslmode": ["verify-full"],
            "sslrootcert": [str(ca)],
            "connect_timeout": ["10"],
        },
        "unsafe connection options",
    )
    frame(uri)


def _serverless_uri_payload(client, iam, manifest, ca, *, source_sha, run_id, now=None):
    """Unchanged task006 helper with a caller-owned bounded SDK client.

    Internal payload primitive, not exported by run_live. Integration MUST verify
    admission first. Tests use fake clients; this module never creates an SDK client.
    """
    now = datetime.now(timezone.utc).timestamp() if now is None else now
    validate_structure(manifest, source_sha=source_sha, run_id=run_id, now=int(now))
    require(file_sha(ca) == manifest["tls"]["trusted_ca_sha256"], "untrusted CA")
    try:
        response = iam.fetch_serverless_credentials(
            client,
            workgroup_name=manifest["workgroup"]["name"],
            db_name=manifest["namespace"]["database"],
            duration_seconds=900,
        )
        expiry = response["expiration"]
        require(
            isinstance(expiry, datetime) and expiry.tzinfo is not None,
            "invalid credential expiry",
        )
        require(
            manifest["window"]["query_deadline"] < expiry.timestamp() <= now + 900,
            "credential lifetime mismatch",
        )
        uri = iam.build_uri(
            manifest["tls"]["hostname"],
            manifest["namespace"]["database"],
            response["dbUser"],
            response["dbPassword"],
            port=5439,
            sslmode="verify-full",
            extra_params={"sslrootcert": str(ca), "connect_timeout": "10"},
        )
        validate_uri(uri, manifest, ca)
        return uri
    except BaseException:
        raise Rejected("Temporary credential preparation failed") from None


def _stop(child):
    # Always reap, including cancellation and blocked stdin/output paths. A new
    # session keeps these signals scoped to this child's process group only.
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(child.pid, sig)
        except ProcessLookupError:
            pass
        try:
            child.wait(timeout=0.3 if sig == signal.SIGTERM else 3)
        except subprocess.TimeoutExpired:
            continue
        # Also kill remaining descendants after the leader exits.
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        return
    raise Rejected("Qualification process could not be reaped")


def private_process(argv, secret=b"", *, seconds=60):
    """Absolute executable, minimal nonsecret environment, bounded pipes and time.

    Native/SDK diagnostics are discarded, never logged or persisted. Output is
    accepted only after parsing a caller-specific fixed whitelist below.
    """
    require(Path(argv[0]).is_absolute() and 0 < seconds <= 60, "invalid process bound")
    child = None
    output = bytearray()
    try:
        child = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            close_fds=True,
            start_new_session=True,
            env={"LANG": "C.UTF-8"},
        )
        with selectors.DefaultSelector() as selector:
            os.set_blocking(child.stdin.fileno(), False)
            os.set_blocking(child.stdout.fileno(), False)
            if secret:
                selector.register(child.stdin, selectors.EVENT_WRITE)
            else:
                child.stdin.close()
            selector.register(child.stdout, selectors.EVENT_READ)
            remaining = memoryview(secret)
            deadline = time.monotonic() + seconds
            while selector.get_map():
                require(time.monotonic() < deadline, "qualification process timeout")
                for key, event in selector.select(
                    min(0.1, deadline - time.monotonic())
                ):
                    if event & selectors.EVENT_WRITE:
                        n = os.write(key.fd, remaining)
                        remaining = remaining[n:]
                        if not remaining:
                            selector.unregister(key.fileobj)
                            key.fileobj.close()
                    else:
                        data = os.read(key.fd, MAX_OUTPUT + 1)
                        output.extend(data)
                        require(
                            len(output) <= MAX_OUTPUT, "qualification output rejected"
                        )
                        if not data:
                            selector.unregister(key.fileobj)
            require(
                child.wait(timeout=max(0.001, deadline - time.monotonic())) == 0,
                "qualification child failed",
            )
        return bytes(output)
    except BaseException:
        raise Rejected("Qualification process failed; cleanup unknown") from None
    finally:
        if child is not None:
            _stop(child)
            for pipe in (child.stdin, child.stdout):
                if pipe is not None:
                    pipe.close()


def client(argv, uri, *, plumbing=False, seconds=60):
    try:
        report = json.loads(private_process(argv, frame(uri), seconds=seconds))
        require(
            report
            == {
                "coverage": True,
                "cleanup": "removed",
                "mode": "postgresql-plumbing" if plumbing else "redshift",
            },
            "unknown coverage or cleanup",
        )
        return report
    except BaseException:
        raise Rejected("Installed qualification failed; cleanup unknown") from None


def serverless_uri(client, iam, manifest, ca, *, source_sha, run_id):
    # No SDK call is possible from the supported entry point while trust/admission
    # is unavailable. No boolean, key, callback or environment override exists.
    admit_live(manifest, source_sha=source_sha, run_id=run_id)
    return _serverless_uri_payload(
        client, iam, manifest, ca, source_sha=source_sha, run_id=run_id
    )
