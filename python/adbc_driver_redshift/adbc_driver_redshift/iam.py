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

"""Optional IAM helper for building Redshift URIs from temporary credentials.

This module has no hard dependency on ``boto3``.  Install the ``iam`` extra
(``pip install adbc-driver-redshift[iam]``) to use the credential-fetching
helpers, or pass an already-constructed boto3 client explicitly.

Credential handling
-------------------
Temporary credentials expire (typically 900-3600 seconds).  Retrieve fresh
credentials before each new ADBC database connection; do not cache and reuse
a password after ``expiration``.  Existing connections remain valid until
closed, but new connections require a new ``get_credentials`` call.

Security
--------
Never log the returned URI or password.  The helpers never log credentials.
Callers should treat the URI as a secret.

References
----------
- ``redshift-serverless:GetCredentials`` (`boto3` service ``redshift-serverless``)
- ``redshift:GetClusterCredentials`` and ``GetClusterCredentialsWithIAM``
  (`boto3` service ``redshift``)
"""

from __future__ import annotations

import typing
import urllib.parse
from datetime import datetime, timezone

__all__ = [
    "build_uri",
    "fetch_cluster_credentials",
    "fetch_cluster_credentials_with_iam",
    "fetch_serverless_credentials",
]


def build_uri(
    host: str,
    database: str,
    db_user: str,
    db_password: str,
    port: int = 5439,
    sslmode: str = "require",
    extra_params: typing.Optional[dict[str, str]] = None,
) -> str:
    """Build a libpq connection URI with proper escaping.

    Parameters
    ----------
    host:
        Redshift endpoint hostname (no scheme, no port).
    database:
        Database name.
    db_user:
        Database user name returned by the credentials API.
    db_password:
        Temporary password returned by the credentials API.
    port:
        TCP port.  Redshift default is 5439.
    sslmode:
        libpq ``sslmode`` value.  ``require`` is recommended; use
        ``verify-full`` with a trusted CA bundle when available.
    extra_params:
        Additional libpq URI query parameters (e.g. ``{"connect_timeout": "10"}``).

    Returns
    -------
    str
        A ``postgresql://`` URI suitable for :func:`adbc_driver_redshift.connect`.

    Raises
    ------
    ValueError
        If required fields are empty or contain characters that would break
        URI structure after encoding (newline, carriage return).
    """
    if not host or not host.strip():
        raise ValueError("host must be a non-empty string")
    if not database or not database.strip():
        raise ValueError("database must be a non-empty string")
    if not db_user:
        raise ValueError("db_user must be a non-empty string")
    if not db_password:
        raise ValueError("db_password must be a non-empty string")
    if not _is_valid_port(port):
        raise ValueError(f"port must be 1..65535, got {port!r}")
    if not sslmode or not sslmode.strip():
        raise ValueError("sslmode must be a non-empty string")

    for label, value in (
        ("host", host),
        ("database", database),
        ("db_user", db_user),
        ("db_password", db_password),
        ("sslmode", sslmode),
    ):
        if "\n" in value or "\r" in value:
            raise ValueError(f"{label} must not contain newline characters")

    encoded_user = urllib.parse.quote(db_user, safe="")
    encoded_password = urllib.parse.quote(db_password, safe="")
    encoded_host = host.strip()
    encoded_db = urllib.parse.quote(database, safe="")

    # Validate host does not contain characters that break URI authority.
    if "/" in encoded_host or "?" in encoded_host or "#" in encoded_host:
        raise ValueError("host must not contain '/', '?' or '#'")
    if "@" in host:
        raise ValueError("host must not contain '@'")

    params: dict[str, str] = {"sslmode": sslmode}
    if extra_params:
        for key, value in extra_params.items():
            if not key or not key.strip():
                raise ValueError("extra_params keys must be non-empty strings")
            if "\n" in key or "\r" in key or "\n" in value or "\r" in value:
                raise ValueError("extra_params must not contain newlines")
            params[key] = value

    query = urllib.parse.urlencode(params)
    return (
        f"postgresql://{encoded_user}:{encoded_password}"
        f"@{encoded_host}:{port}/{encoded_db}?{query}"
    )


def fetch_serverless_credentials(
    client: typing.Any,
    workgroup_name: typing.Optional[str] = None,
    db_name: typing.Optional[str] = None,
    duration_seconds: typing.Optional[int] = None,
    custom_domain_name: typing.Optional[str] = None,
) -> dict:
    """Call ``redshift-serverless:GetCredentials``.

    Parameters
    ----------
    client:
        A ``boto3`` client for ``redshift-serverless`` (``boto3.client(
        "redshift-serverless", region_name=...)``).  The caller creates and
        owns the client so tests can inject a mock.
    workgroup_name:
        Workgroup name.  Either this or ``custom_domain_name`` is required
        by the AWS API.
    db_name:
        Database name for the scoped credential.  When omitted the credential
        is valid for any database in the workgroup.
    duration_seconds:
        Lifetime in seconds (900..3600).  Defaults to the service default
        (900) when ``None``.
    custom_domain_name:
        Custom domain name associated with the workgroup.

    Returns
    -------
    dict
        Raw response with ``dbUser``, ``dbPassword``, ``expiration``,
        ``nextRefreshTime``.

    Notes
    -----
    The returned ``dbPassword`` expires at ``expiration``.  Callers must
    obtain new credentials before that time for new connections.
    """
    if workgroup_name is not None and not workgroup_name.strip():
        raise ValueError("workgroup_name must be non-empty when provided")
    if custom_domain_name is not None and not custom_domain_name.strip():
        raise ValueError("custom_domain_name must be non-empty when provided")
    if workgroup_name is None and custom_domain_name is None:
        raise ValueError("either workgroup_name or custom_domain_name is required")
    if duration_seconds is not None and not 900 <= duration_seconds <= 3600:
        raise ValueError("duration_seconds must be between 900 and 3600")

    kwargs: dict[str, typing.Any] = {}
    if workgroup_name is not None:
        kwargs["workgroupName"] = workgroup_name
    if db_name is not None:
        kwargs["dbName"] = db_name
    if duration_seconds is not None:
        kwargs["durationSeconds"] = duration_seconds
    if custom_domain_name is not None:
        kwargs["customDomainName"] = custom_domain_name

    return client.get_credentials(**kwargs)


def fetch_cluster_credentials(
    client: typing.Any,
    db_user: str,
    cluster_identifier: typing.Optional[str] = None,
    db_name: typing.Optional[str] = None,
    duration_seconds: typing.Optional[int] = None,
    auto_create: typing.Optional[bool] = None,
    db_groups: typing.Optional[typing.Sequence[str]] = None,
    custom_domain_name: typing.Optional[str] = None,
) -> dict:
    """Call ``redshift:GetClusterCredentials``.

    Parameters
    ----------
    client:
        A ``boto3`` client for ``redshift``
        (``boto3.client("redshift", region_name=...)``).
    db_user:
        Database user name (required).  If the user does not exist and
        ``auto_create`` is false the call succeeds but the later connection
        will fail.
    cluster_identifier:
        Provisioned cluster identifier.
    db_name:
        Database name for the scoped credential.
    duration_seconds:
        Lifetime in seconds (900..3600).
    auto_create:
        Create the database user if it does not exist.
    db_groups:
        Existing database groups the user joins for this session.
    custom_domain_name:
        Custom domain name for the cluster.

    Returns
    -------
    dict
        Raw response with ``DbUser``, ``DbPassword``, ``Expiration``.
    """
    if not db_user or not db_user.strip():
        raise ValueError("db_user must be a non-empty string")
    if duration_seconds is not None and not 900 <= duration_seconds <= 3600:
        raise ValueError("duration_seconds must be between 900 and 3600")
    if db_groups is not None and not isinstance(db_groups, (list, tuple)):
        raise ValueError("db_groups must be a list or tuple of strings")

    kwargs: dict[str, typing.Any] = {"DbUser": db_user}
    if cluster_identifier is not None:
        kwargs["ClusterIdentifier"] = cluster_identifier
    if db_name is not None:
        kwargs["DbName"] = db_name
    if duration_seconds is not None:
        kwargs["DurationSeconds"] = duration_seconds
    if auto_create is not None:
        kwargs["AutoCreate"] = auto_create
    if db_groups is not None:
        kwargs["DbGroups"] = list(db_groups)
    if custom_domain_name is not None:
        kwargs["CustomDomainName"] = custom_domain_name

    return client.get_cluster_credentials(**kwargs)


def fetch_cluster_credentials_with_iam(
    client: typing.Any,
    cluster_identifier: typing.Optional[str] = None,
    db_name: typing.Optional[str] = None,
    duration_seconds: typing.Optional[int] = None,
    custom_domain_name: typing.Optional[str] = None,
) -> dict:
    """Call ``redshift:GetClusterCredentialsWithIAM``.

    The database user is mapped 1:1 to the caller's IAM identity.

    Parameters
    ----------
    client:
        A ``boto3`` client for ``redshift``.
    cluster_identifier:
        Provisioned cluster identifier.
    db_name:
        Database name for the scoped credential.
    duration_seconds:
        Lifetime in seconds (900..3600).
    custom_domain_name:
        Custom domain name for the cluster.

    Returns
    -------
    dict
        Raw response with ``DbUser``, ``DbPassword``, ``Expiration``,
        ``NextRefreshTime``.
    """
    if duration_seconds is not None and not 900 <= duration_seconds <= 3600:
        raise ValueError("duration_seconds must be between 900 and 3600")

    kwargs: dict[str, typing.Any] = {}
    if cluster_identifier is not None:
        kwargs["ClusterIdentifier"] = cluster_identifier
    if db_name is not None:
        kwargs["DbName"] = db_name
    if duration_seconds is not None:
        kwargs["DurationSeconds"] = duration_seconds
    if custom_domain_name is not None:
        kwargs["CustomDomainName"] = custom_domain_name

    return client.get_cluster_credentials_with_iam(**kwargs)


def _is_valid_port(port: int) -> bool:
    return isinstance(port, int) and 1 <= port <= 65535


def _coerce_expiration(value: typing.Any) -> datetime | None:
    """Best-effort coercion of an expiration value to datetime."""
    if value is None:
        return None
    if isinstance(value, datetime):
        if value.tzinfo is None:
            return value.replace(tzinfo=timezone.utc)
        return value
    return None
