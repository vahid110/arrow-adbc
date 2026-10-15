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

import datetime
import urllib.parse

import pytest

from adbc_driver_redshift.iam import (
    build_uri,
    fetch_cluster_credentials,
    fetch_cluster_credentials_with_iam,
    fetch_serverless_credentials,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


class FakeServerlessClient:
    def __init__(self, response):
        self.response = response
        self.calls = []

    def get_credentials(self, **kwargs):
        self.calls.append(kwargs)
        return self.response


class FakeClusterClient:
    def __init__(self, response, with_iam_response=None):
        self.response = response
        self.with_iam_response = (
            with_iam_response if with_iam_response is not None else response
        )
        self.calls = []
        self.iam_calls = []

    def get_cluster_credentials(self, **kwargs):
        self.calls.append(kwargs)
        return self.response

    def get_cluster_credentials_with_iam(self, **kwargs):
        self.iam_calls.append(kwargs)
        return self.with_iam_response


# ---------------------------------------------------------------------------
# build_uri
# ---------------------------------------------------------------------------


def test_build_uri_basic():
    uri = build_uri("host.example.com", "dev", "alice", "s3cret")
    assert uri == "postgresql://alice:s3cret@host.example.com:5439/dev?sslmode=require"


def test_build_uri_escapes_special_chars():
    uri = build_uri("host.example.com", "dev", "user:name", "p@ss/w:ord#1")
    # user and password must be percent-encoded
    assert "user%3Aname" in uri
    assert "p%40ss%2Fw%3Aord%231" in uri
    # round-trip check
    parsed = urllib.parse.urlparse(uri)
    assert urllib.parse.unquote(parsed.username) == "user:name"
    assert urllib.parse.unquote(parsed.password) == "p@ss/w:ord#1"


def test_build_uri_iam_prefix_preserved():
    uri = build_uri("host.example.com", "dev", "IAMR:my-workgroup", "pass")
    assert "IAMR%3Amy-workgroup" in uri
    parsed = urllib.parse.urlparse(uri)
    assert urllib.parse.unquote(parsed.username) == "IAMR:my-workgroup"


def test_build_uri_custom_port_and_sslmode():
    uri = build_uri("h", "d", "u", "p", port=5432, sslmode="verify-full")
    assert ":5432/" in uri
    assert "sslmode=verify-full" in uri


def test_build_uri_extra_params():
    uri = build_uri("h", "d", "u", "p", extra_params={"connect_timeout": "10"})
    assert "connect_timeout=10" in uri
    assert "sslmode=require" in uri


@pytest.mark.parametrize(
    "key,value,encoded_value",
    [
        ("sslrootcert", "/tmp/Redshift CA.pem", "%2Ftmp%2FRedshift%20CA.pem"),
        ("sslrootcert", "/tmp/Redshift+CA.pem", "%2Ftmp%2FRedshift%2BCA.pem"),
        ("application_name", "Redshift client+IAM", "Redshift%20client%2BIAM"),
    ],
)
def test_build_uri_query_percent_encoding(key, value, encoded_value):
    uri = build_uri("h", "d", "u", "p", extra_params={key: value})
    query = urllib.parse.urlsplit(uri).query
    assert f"{key}={encoded_value}" in query
    # libpq percent-decodes query values without treating '+' as a space.
    params = dict(
        (urllib.parse.unquote(k), urllib.parse.unquote(v))
        for k, v in (param.split("=", 1) for param in query.split("&"))
    )
    assert params[key] == value


def test_build_uri_extra_params_empty_dict():
    uri = build_uri("h", "d", "u", "p", extra_params={})
    assert uri.count("sslmode") == 1


def test_build_uri_rejects_empty_host():
    with pytest.raises(ValueError, match="host"):
        build_uri("", "d", "u", "p")
    with pytest.raises(ValueError, match="host"):
        build_uri("   ", "d", "u", "p")


def test_build_uri_rejects_empty_database():
    with pytest.raises(ValueError, match="database"):
        build_uri("h", "", "u", "p")


def test_build_uri_rejects_empty_user():
    with pytest.raises(ValueError, match="db_user"):
        build_uri("h", "d", "", "p")


def test_build_uri_rejects_empty_password():
    with pytest.raises(ValueError, match="db_password"):
        build_uri("h", "d", "u", "")


def test_build_uri_rejects_invalid_port():
    with pytest.raises(ValueError, match="port"):
        build_uri("h", "d", "u", "p", port=0)
    with pytest.raises(ValueError, match="port"):
        build_uri("h", "d", "u", "p", port=70000)


def test_build_uri_rejects_newline_in_fields():
    with pytest.raises(ValueError, match="newline"):
        build_uri("h\n", "d", "u", "p")
    with pytest.raises(ValueError, match="newline"):
        build_uri("h", "d", "u", "p\nword")


def test_build_uri_rejects_host_with_slash():
    with pytest.raises(ValueError):
        build_uri("host/with/slash", "d", "u", "p")


def test_build_uri_rejects_host_with_at():
    with pytest.raises(ValueError):
        build_uri("host@evil", "d", "u", "p")


def test_build_uri_rejects_empty_sslmode():
    with pytest.raises(ValueError, match="sslmode"):
        build_uri("h", "d", "u", "p", sslmode="")


def test_build_uri_rejects_extra_params_newline():
    with pytest.raises(ValueError, match="newlines"):
        build_uri("h", "d", "u", "p", extra_params={"k\n": "v"})
    with pytest.raises(ValueError, match="newlines"):
        build_uri("h", "d", "u", "p", extra_params={"k": "v\n"})


def test_build_uri_rejects_extra_params_empty_key():
    with pytest.raises(ValueError):
        build_uri("h", "d", "u", "p", extra_params={"": "v"})


# ---------------------------------------------------------------------------
# fetch_serverless_credentials
# ---------------------------------------------------------------------------


def test_fetch_serverless_basic():
    fake = FakeServerlessClient(
        {
            "dbUser": "IAMR:alice",
            "dbPassword": "secret",
            "expiration": datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc),
            "nextRefreshTime": datetime.datetime(
                2026, 1, 1, tzinfo=datetime.timezone.utc
            ),
        }
    )
    resp = fetch_serverless_credentials(fake, workgroup_name="my-wg", db_name="dev")
    assert resp["dbUser"] == "IAMR:alice"
    assert fake.calls[0]["workgroupName"] == "my-wg"
    assert fake.calls[0]["dbName"] == "dev"


def test_fetch_serverless_with_all_params():
    fake = FakeServerlessClient({"dbUser": "u", "dbPassword": "p"})
    fetch_serverless_credentials(
        fake,
        workgroup_name="wg",
        db_name="dev",
        duration_seconds=900,
        custom_domain_name="example.com",
    )
    assert fake.calls[0] == {
        "workgroupName": "wg",
        "dbName": "dev",
        "durationSeconds": 900,
        "customDomainName": "example.com",
    }


def test_fetch_serverless_custom_domain_only():
    fake = FakeServerlessClient({"dbUser": "u", "dbPassword": "p"})
    fetch_serverless_credentials(fake, custom_domain_name="example.com")
    assert fake.calls[0] == {"customDomainName": "example.com"}


def test_fetch_serverless_requires_workgroup_or_domain():
    fake = FakeServerlessClient({"dbUser": "u", "dbPassword": "p"})
    with pytest.raises(ValueError, match="either workgroup_name or custom_domain_name"):
        fetch_serverless_credentials(fake)


def test_fetch_serverless_rejects_empty_workgroup():
    fake = FakeServerlessClient({"dbUser": "u", "dbPassword": "p"})
    with pytest.raises(ValueError, match="workgroup_name"):
        fetch_serverless_credentials(fake, workgroup_name="  ")


def test_fetch_serverless_rejects_bad_duration():
    fake = FakeServerlessClient({"dbUser": "u", "dbPassword": "p"})
    with pytest.raises(ValueError, match="duration_seconds"):
        fetch_serverless_credentials(fake, workgroup_name="wg", duration_seconds=100)
    with pytest.raises(ValueError, match="duration_seconds"):
        fetch_serverless_credentials(fake, workgroup_name="wg", duration_seconds=9999)


def test_fetch_serverless_duration_boundaries():
    fake = FakeServerlessClient({"dbUser": "u", "dbPassword": "p"})
    fetch_serverless_credentials(fake, workgroup_name="wg", duration_seconds=900)
    fetch_serverless_credentials(fake, workgroup_name="wg", duration_seconds=3600)


# ---------------------------------------------------------------------------
# fetch_cluster_credentials
# ---------------------------------------------------------------------------


def test_fetch_cluster_basic():
    fake = FakeClusterClient({"DbUser": "IAM:my_user", "DbPassword": "secret"})
    resp = fetch_cluster_credentials(
        fake, db_user="my_user", cluster_identifier="my-cluster"
    )
    assert resp["DbUser"] == "IAM:my_user"
    assert fake.calls[0]["DbUser"] == "my_user"
    assert fake.calls[0]["ClusterIdentifier"] == "my-cluster"


def test_fetch_cluster_with_all_params():
    fake = FakeClusterClient({"DbUser": "u", "DbPassword": "p"})
    fetch_cluster_credentials(
        fake,
        db_user="my_user",
        cluster_identifier="c1",
        db_name="dev",
        duration_seconds=900,
        auto_create=True,
        db_groups=["g1", "g2"],
        custom_domain_name="example.com",
    )
    assert fake.calls[0] == {
        "DbUser": "my_user",
        "ClusterIdentifier": "c1",
        "DbName": "dev",
        "DurationSeconds": 900,
        "AutoCreate": True,
        "DbGroups": ["g1", "g2"],
        "CustomDomainName": "example.com",
    }


def test_fetch_cluster_rejects_empty_db_user():
    fake = FakeClusterClient({})
    with pytest.raises(ValueError, match="db_user"):
        fetch_cluster_credentials(fake, db_user="")


def test_fetch_cluster_rejects_bad_duration():
    fake = FakeClusterClient({})
    with pytest.raises(ValueError, match="duration_seconds"):
        fetch_cluster_credentials(fake, db_user="u", duration_seconds=100)


def test_fetch_cluster_rejects_bad_db_groups_type():
    fake = FakeClusterClient({})
    with pytest.raises(ValueError, match="db_groups"):
        fetch_cluster_credentials(fake, db_user="u", db_groups="not-a-list")  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# fetch_cluster_credentials_with_iam
# ---------------------------------------------------------------------------


def test_fetch_cluster_with_iam_basic():
    fake = FakeClusterClient(
        {}, with_iam_response={"DbUser": "IAMR:alice", "DbPassword": "s"}
    )
    resp = fetch_cluster_credentials_with_iam(
        fake, cluster_identifier="c1", db_name="dev"
    )
    assert resp["DbUser"] == "IAMR:alice"
    assert fake.iam_calls[0]["ClusterIdentifier"] == "c1"
    assert fake.iam_calls[0]["DbName"] == "dev"


def test_fetch_cluster_with_iam_all_params():
    fake = FakeClusterClient({}, with_iam_response={"DbUser": "u", "DbPassword": "p"})
    fetch_cluster_credentials_with_iam(
        fake,
        cluster_identifier="c1",
        db_name="dev",
        duration_seconds=900,
        custom_domain_name="ex.com",
    )
    assert fake.iam_calls[0] == {
        "ClusterIdentifier": "c1",
        "DbName": "dev",
        "DurationSeconds": 900,
        "CustomDomainName": "ex.com",
    }


def test_fetch_cluster_with_iam_rejects_bad_duration():
    fake = FakeClusterClient({})
    with pytest.raises(ValueError, match="duration_seconds"):
        fetch_cluster_credentials_with_iam(fake, duration_seconds=100)


def test_fetch_cluster_with_iam_no_required_params():
    fake = FakeClusterClient({}, with_iam_response={"DbUser": "u", "DbPassword": "p"})
    # All params optional for this API
    resp = fetch_cluster_credentials_with_iam(fake)
    assert resp["DbUser"] == "u"
    assert fake.iam_calls[0] == {}


# ---------------------------------------------------------------------------
# Integration: credentials -> URI
# ---------------------------------------------------------------------------


def test_serverless_credentials_to_uri_roundtrip():
    fake = FakeServerlessClient(
        {
            "dbUser": "IAMR:alice",
            "dbPassword": "p@ss:w0rd/123",
            "expiration": datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc),
            "nextRefreshTime": datetime.datetime(
                2026, 1, 1, tzinfo=datetime.timezone.utc
            ),
        }
    )
    creds = fetch_serverless_credentials(fake, workgroup_name="wg", db_name="dev")
    uri = build_uri(
        "my-workgroup.123.eu-north-1.redshift-serverless.amazonaws.com",
        "dev",
        creds["dbUser"],
        creds["dbPassword"],
    )
    parsed = urllib.parse.urlparse(uri)
    assert urllib.parse.unquote(parsed.username) == "IAMR:alice"
    assert urllib.parse.unquote(parsed.password) == "p@ss:w0rd/123"
    assert (
        parsed.hostname
        == "my-workgroup.123.eu-north-1.redshift-serverless.amazonaws.com"
    )
    assert parsed.port == 5439


def test_cluster_credentials_to_uri_roundtrip():
    fake = FakeClusterClient({"DbUser": "IAM:my_user", "DbPassword": "s3cr#t"})
    creds = fetch_cluster_credentials(fake, db_user="my_user", cluster_identifier="c1")
    uri = build_uri(
        "my-cluster.xxx.redshift.amazonaws.com",
        "dev",
        creds["DbUser"],
        creds["DbPassword"],
    )
    parsed = urllib.parse.urlparse(uri)
    assert urllib.parse.unquote(parsed.username) == "IAM:my_user"
    assert urllib.parse.unquote(parsed.password) == "s3cr#t"
