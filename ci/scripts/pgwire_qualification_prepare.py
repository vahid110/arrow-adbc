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

"""Authenticate exact merged-head GitHub artifacts before OIDC or database IAM."""

import argparse
import hashlib
import io
import ipaddress
import json
import os
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path
from urllib.parse import urlsplit

from pgwire_qualification import _inside, _member, file_sha
from pgwire_qualification_ci import claims
from pgwire_qualification_live import KINDS, REF, REPOSITORY, WORKFLOW, verify_local
from pgwire_qualification_manifest import COMMIT, digest, require

CA_BUNDLE = "/etc/ssl/certs/ca-certificates.crt"

ARTIFACTS = {
    "c_archive": (
        "adbc-redshift-dev-ubuntu24-x86_64",
        ".github/workflows/pgwire-package.yml",
    ),
    "python_wheel": (
        "adbc-redshift-python-wheel-ubuntu24-x86_64",
        ".github/workflows/pgwire-python.yml",
    ),
}


def parse_json(value):
    def unique(pairs):
        result = {}
        for key, item in pairs:
            require(key not in result, "duplicate JSON field")
            result[key] = item
        return result

    return json.loads(
        value,
        object_pairs_hook=unique,
        parse_constant=lambda _: require(False, "invalid JSON constant"),
    )


class DownloadRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        parts = urlsplit(newurl)
        require(
            parts.scheme == "https"
            and (
                parts.hostname == "api.github.com"
                or parts.hostname.endswith(".blob.core.windows.net")
                or parts.hostname.endswith(".actions.githubusercontent.com")
            ),
            "untrusted artifact redirect",
        )
        request = super().redirect_request(req, fp, code, msg, headers, newurl)
        request.remove_header("Authorization")
        return request


class GitHub:
    def __init__(self, token):
        self.token = token

    def get(self, path, *, binary=False, method="GET"):
        require(
            path.startswith("/")
            and ".." not in path
            and ("?" not in path or path.endswith("?per_page=100")),
            "invalid GitHub path",
        )
        request = urllib.request.Request(
            "https://api.github.com/repos/" + REPOSITORY + path,
            method=method,
            headers={
                "Authorization": "Bearer " + self.token,
                "Accept": "application/vnd.github+json",
                "X-GitHub-Api-Version": "2022-11-28",
            },
        )
        opener = urllib.request.build_opener(
            DownloadRedirect(), urllib.request.ProxyHandler({})
        )
        with opener.open(request, timeout=30) as response:
            data = response.read(300 * 1024 * 1024 + 1)
        require(len(data) <= 300 * 1024 * 1024, "oversized GitHub response")
        return data if binary else parse_json(data)

    def context(self):
        require(
            os.environ["GITHUB_REPOSITORY"] == REPOSITORY
            and os.environ["GITHUB_REF"] == REF
            and os.environ["GITHUB_EVENT_NAME"] == "workflow_dispatch"
            and os.environ["GITHUB_RUN_ATTEMPT"] == "1"
            and os.environ["GITHUB_WORKFLOW_REF"]
            == REPOSITORY + "/" + WORKFLOW + "@" + REF,
            "unsupported dispatch context",
        )
        source, run = os.environ["GITHUB_SHA"], int(os.environ["GITHUB_RUN_ID"])
        require(COMMIT.fullmatch(source), "invalid source")
        actual = self.get("/actions/runs/" + str(run))
        require(
            actual["head_sha"] == source
            and actual["head_branch"] == REF.removeprefix("refs/heads/")
            and actual["event"] == "workflow_dispatch"
            and actual["run_attempt"] == 1
            and actual["path"] == WORKFLOW
            and actual["repository"]["full_name"] == REPOSITORY,
            "unauthenticated live run",
        )
        require(
            self.get("/git/ref/heads/" + REF.removeprefix("refs/heads/"))["object"][
                "sha"
            ]
            == source,
            "integration head changed",
        )
        checkout = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True
        ).strip()
        require(checkout == source, "checkout mismatch")
        return source, run

    def review(self, source):
        associated = self.get("/commits/" + source + "/pulls?per_page=100")
        merged = [
            p
            for p in associated
            if p.get("merged_at")
            and p["merge_commit_sha"] == source
            and p["base"]["ref"] == REF.removeprefix("refs/heads/")
        ]
        require(len(merged) == 1, "exact merged PR unavailable")
        pr = self.get("/pulls/" + str(merged[0]["number"]))
        head = pr["head"]["sha"]
        require(
            pr["merged"] is True and pr["merge_commit_sha"] == source,
            "source is not merged reviewed PR",
        )
        tree = self.get("/git/commits/" + source)["tree"]["sha"]
        require(
            self.get("/git/commits/" + head)["tree"]["sha"] == tree,
            "squash changed reviewed tree",
        )
        comments = self.get("/issues/" + str(pr["number"]) + "/comments?per_page=100")
        require(len(comments) < 100, "review comment pagination requires inspection")
        approved = [
            c
            for c in comments
            if c["user"]["login"] == "vahid110"
            and c["author_association"] == "OWNER"
            and c["body"].startswith("VERDICT: APPROVE")
            and head in c["body"]
        ]
        require(approved, "owner-published exact-head independent approval missing")
        comment = max(approved, key=lambda c: c["id"])
        response = self.get("/commits/" + head + "/check-runs?per_page=100")
        require(
            response["total_count"] == len(response["check_runs"]),
            "original checks incomplete",
        )
        checks = sorted(
            (
                {k: c[k] for k in ("id", "name", "head_sha", "status", "conclusion")}
                for c in response["check_runs"]
                if c["app"]["slug"] == "github-actions"
            ),
            key=lambda c: c["id"],
        )
        require(
            checks
            and all(
                c["head_sha"] == head
                and c["status"] == "completed"
                and c["conclusion"] in ("success", "skipped", "neutral")
                for c in checks
            )
            and any(c["conclusion"] == "success" for c in checks),
            "original applicable CI not green",
        )
        return {
            "pr": pr["number"],
            "head_sha": head,
            "tree_sha": tree,
            "comment_id": comment["id"],
            "comment_sha256": hashlib.sha256(comment["body"].encode()).hexdigest(),
            "checks_sha256": digest(checks),
        }

    def artifact(self, kind, artifact_id, source):
        item = self.get("/actions/artifacts/" + str(artifact_id))
        name, workflow = ARTIFACTS[kind]
        require(
            item["id"] == artifact_id
            and item["name"] == name
            and item["expired"] is False
            and item["workflow_run"]["head_sha"] == source
            and item["workflow_run"]["head_branch"] == REF.removeprefix("refs/heads/"),
            "artifact source mismatch",
        )
        run = self.get("/actions/runs/" + str(item["workflow_run"]["id"]))
        require(
            run["head_sha"] == source
            and run["path"] == workflow
            and run["event"] in ("push", "workflow_dispatch")
            and run["head_branch"] == REF.removeprefix("refs/heads/")
            and run["status"] == "completed"
            and run["conclusion"] == "success"
            and run["run_attempt"] == 1
            and run["repository"]["full_name"] == REPOSITORY,
            "artifact run not successful merged source",
        )
        return item

    def verify(self, admission):
        source, run = self.context()
        require(
            source == admission["source_sha"] and run == admission["run_id"],
            "admission source mismatch",
        )
        require(
            self.review(source) == admission["review"],
            "authenticated source review mismatch",
        )
        for kind in KINDS:
            binding = admission["artifacts"][kind]
            item = self.artifact(kind, binding["id"], source)
            require(
                item["workflow_run"]["id"] == binding["run_id"], "artifact run mismatch"
            )

    def download(self, kind, artifact_id, source, target):
        item = self.artifact(kind, artifact_id, source)
        data = self.get("/actions/artifacts/" + str(artifact_id) + "/zip", binary=True)
        require(
            item["digest"] == "sha256:" + hashlib.sha256(data).hexdigest(),
            "GitHub artifact archive digest mismatch",
        )
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            require(
                sum(i.file_size for i in archive.infolist()) <= 600 * 1024 * 1024,
                "oversized archive",
            )
            names = set()
            for member in archive.infolist():
                name = _member(member.filename)
                require(
                    name not in names
                    and (member.external_attr >> 16) & 0o170000 != 0o120000,
                    "unsafe artifact member",
                )
                names.add(name)
                path = _inside(target, name)
                if member.is_dir():
                    path.mkdir(parents=True, exist_ok=True)
                else:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(archive.read(member))
        return item


def prepare(github, directory, interpreter, c_id, wheel_id, *, nonce):
    from pgwire_qualification_policy import admission_name

    admission_name(nonce)
    source, run = github.context()
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=False)
    paths = {"ca": CA_BUNDLE}
    with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(
        "https://checkip.amazonaws.com", timeout=10
    ) as response:
        address = ipaddress.ip_address(response.read(64).decode().strip())
    require(
        address.version == 4 and address.is_global, "runner public IPv4 unavailable"
    )
    admission = {
        "nonce": nonce,
        "review": github.review(source),
        "runner_cidr": str(address) + "/32",
        "source_sha": source,
        "run_id": run,
        "artifacts": {},
        "ca_sha256": file_sha(paths["ca"]),
    }
    print("Preparation stage: source/review and runner inputs verified.", flush=True)
    for kind, artifact_id in zip(KINDS, (c_id, wheel_id)):
        download = directory / (kind + "-download")
        download.mkdir()
        item = github.download(kind, artifact_id, source, download)
        pattern = (
            "*.tar.gz"
            if kind == "c_archive"
            else "adbc_driver_redshift-*-py3-none-linux_x86_64.whl"
        )
        candidates = list(download.glob(pattern))
        require(len(candidates) == 1, "missing/ambiguous installed artifact")
        artifact = candidates[0]
        if kind == "c_archive":
            root = directory / "c-prefix"
            root.mkdir()
            with tarfile.open(artifact) as archive:
                # Python 3.12 data filter rejects links escaping the prefix.
                archive.extractall(root, filter="data")
        else:
            subprocess.run(
                [
                    interpreter,
                    "-m",
                    "pip",
                    "install",
                    "--no-deps",
                    "--no-compile",
                    str(artifact),
                ],
                check=True,
            )
            root = Path(
                subprocess.check_output(
                    [
                        interpreter,
                        "-I",
                        "-c",
                        "import sysconfig; print(sysconfig.get_path('purelib'))",
                    ],
                    text=True,
                ).strip()
            )
        paths[kind] = {"artifact": str(artifact), "root": str(root)}
        admission["artifacts"][kind] = {
            "id": artifact_id,
            "run_id": item["workflow_run"]["id"],
            **claims(artifact, root, wheel=kind == "python_wheel"),
        }
        print(
            "Preparation stage: " + kind + " installed inventory verified.", flush=True
        )
    c_client = directory / "redshift-qualification"
    prefix = paths["c_archive"]["root"]
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            prefix + "/include",
            "c/driver/pgwire/redshift_qualification.c",
            "-L",
            prefix + "/lib",
            "-Wl,-rpath," + prefix + "/lib",
            "-ladbc_driver_manager",
            "-o",
            str(c_client),
        ],
        check=True,
    )
    paths["c_client"], paths["c_client_sha256"] = str(c_client), file_sha(c_client)
    admission["c_client_sha256"] = paths["c_client_sha256"]
    print(
        "Preparation stage: C client built; load-only verification starting.",
        flush=True,
    )
    verify_local(admission, paths, interpreter)
    print("Preparation stage: installed entrypoints verified.", flush=True)
    (directory / "paths.json").write_text(json.dumps(paths, sort_keys=True) + "\n")
    # This is NONSECRET host input. It never grants admission or substitutes for
    # the consumer's fresh authenticated GitHub calls and installed-byte checks.
    (directory / "preflight.json").write_text(
        json.dumps(admission, sort_keys=True) + "\n"
    )
    return admission


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--c-artifact", required=True, type=int)
    parser.add_argument("--python-artifact", required=True, type=int)
    parser.add_argument("--nonce", required=True)
    args = parser.parse_args()
    try:
        prepare(
            GitHub(os.environ["GH_TOKEN"]),
            args.directory,
            sys.executable,
            args.c_artifact,
            args.python_artifact,
            nonce=args.nonce,
        )
        print(
            "Exact merged-head artifacts installed and load-verified; "
            "no AWS credentials requested."
        )
        return 0
    except BaseException:
        print(
            "Artifact preparation rejected; no AWS or live admission.", file=sys.stderr
        )
        return 1


if __name__ == "__main__":
    sys.exit(main())
