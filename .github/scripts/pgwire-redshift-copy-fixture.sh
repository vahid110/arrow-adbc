#!/usr/bin/env bash
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

set -Eeuo pipefail
umask 077

: "${RUNNER_TEMP:?}"
: "${GITHUB_RUN_ID:?}"
: "${GITHUB_RUN_ATTEMPT:?}"
: "${REDSHIFT_SECURITY_GROUP_ID:?}"
: "${REDSHIFT_COPY_BUCKET:?}"
: "${REDSHIFT_COPY_ROLE_ARN:?}"
: "${REDSHIFT_WORKGROUP:?}"
: "${REDSHIFT_HOST:?}"
: "${REDSHIFT_DBNAME:?}"
[[ "$GITHUB_RUN_ID" =~ ^[0-9]+$ && "$GITHUB_RUN_ATTEMPT" =~ ^[0-9]+$ ]]

fixture_prefix="staging/ci/${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}"
data_key="${fixture_prefix}/data.csv"
manifest_key="${fixture_prefix}/manifest.json"
rule_description="adbc-pgwire-copy-${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}"
state_dir="${RUNNER_TEMP}/pgwire-redshift-copy-${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}"
mkdir -p "$state_dir"

cleanup() {
  local failed=0 ingress_failed=0 cidr rules ids rule_id result attempt verify_rules
  local object_label object_key object_etag

  # A failed or response-lost conditional PutObject may have created an object,
  # or may have collided with someone else's object. Without GetObject, neither
  # case can be distinguished safely. Delete only after a successful PUT gave
  # us its ETag, and condition the DELETE on that same ETag. An unresolved key
  # requires an independent exact-prefix audit, not a blind DELETE. An ETag
  # detects changed content, not a same-content replacement; unique run keys
  # and independent post-run audit remain necessary.
  for object_label in manifest data; do
    if [[ ! -f "$state_dir/${object_label}-upload-attempted" ||
          -f "$state_dir/${object_label}-deleted" ]]; then
      continue
    fi
    if [[ "$object_label" == manifest ]]; then
      object_key="$manifest_key"
    else
      object_key="$data_key"
    fi
    if [[ ! -s "$state_dir/${object_label}-owned-etag" ]]; then
      echo "::error::COPY $object_label upload ownership is unconfirmed; do not delete s3://${REDSHIFT_COPY_BUCKET}/${object_key}. Independently audit this exact key."
      failed=1
      continue
    fi
    object_etag="$(< "$state_dir/${object_label}-owned-etag")"
    if aws s3api delete-object --bucket "$REDSHIFT_COPY_BUCKET" \
        --key "$object_key" --if-match "$object_etag" > /dev/null; then
      : > "$state_dir/${object_label}-deleted"
    else
      echo "::error::Could not confirm ETag-conditional deletion of s3://${REDSHIFT_COPY_BUCKET}/${object_key}; independently audit this exact key."
      failed=1
    fi
  done

  if [[ -f "$state_dir/authorize-attempted" && ! -f "$state_dir/ingress-revoked" ]]; then
    cidr="$(< "$state_dir/cidr")"
    ids=''
    # The authorize response can be lost after EC2 creates the rule. Recover
    # only this run's uniquely described rule; never revoke by CIDR alone.
    for attempt in {1..12}; do
      if rules="$(aws ec2 describe-security-group-rules \
          --filters "Name=group-id,Values=$REDSHIFT_SECURITY_GROUP_ID" \
          --output json)"; then
        if ids="$(jq -r --arg group "$REDSHIFT_SECURITY_GROUP_ID" \
            --arg cidr "$cidr" --arg description "$rule_description" \
            '.SecurityGroupRules[]? | select(.GroupId == $group and
              .IsEgress == false and .IpProtocol == "tcp" and
              .FromPort == 5439 and .ToPort == 5439 and
              .CidrIpv4 == $cidr and .Description == $description) |
              .SecurityGroupRuleId' <<< "$rules")"; then
          [[ -n "$ids" ]] && break
        fi
      fi
      sleep 2
    done
    # A successful authorize response gives an ownership-safe fallback ID if
    # Describe remains eventually inconsistent. An ambiguous response has no
    # fallback and must remain a visible manual-audit failure.
    if [[ -z "$ids" && -s "$state_dir/authorized-rule-id" ]]; then
      ids="$(< "$state_dir/authorized-rule-id")"
    fi
    if [[ -z "$ids" ]]; then
      echo "::error::Cannot confirm whether this run's ingress rule exists; inspect description $rule_description."
      failed=1
      ingress_failed=1
    else
      while IFS= read -r rule_id; do
        if ! result="$(aws ec2 revoke-security-group-ingress \
            --group-id "$REDSHIFT_SECURITY_GROUP_ID" \
            --security-group-rule-ids "$rule_id" \
            --query Return --output text)" || [[ "$result" != 'True' ]]; then
          # EC2 may remove the rule and lose the response. Only treat that as
          # cleaned up if a fresh Describe confirms the exact owned ID absent.
          if ! verify_rules="$(aws ec2 describe-security-group-rules \
              --filters "Name=group-id,Values=$REDSHIFT_SECURITY_GROUP_ID" \
              --output json)" ||
              ! jq -e '.SecurityGroupRules | type == "array"' \
                <<< "$verify_rules" > /dev/null ||
              jq -e --arg id "$rule_id" --arg group "$REDSHIFT_SECURITY_GROUP_ID" \
                --arg cidr "$cidr" --arg description "$rule_description" \
                '.SecurityGroupRules[] | select(.SecurityGroupRuleId == $id and
                  .GroupId == $group and .IsEgress == false and
                  .IpProtocol == "tcp" and .FromPort == 5439 and
                  .ToPort == 5439 and .CidrIpv4 == $cidr and
                  .Description == $description)' <<< "$verify_rules" > /dev/null; then
            echo "::error::Could not confirm removal of this run's ingress rule ID $rule_id."
            failed=1
            ingress_failed=1
          fi
        fi
      done <<< "$ids"
      if (( ingress_failed == 0 )); then
        : > "$state_dir/ingress-revoked"
      fi
    fi
  fi
  return "$failed"
}

on_exit() {
  local status="$1"
  trap - EXIT
  if ! cleanup; then status=1; fi
  unset PGPASSWORD
  exit "$status"
}

run_fixture() {
  local runner_ip cidr rules existing credentials db_user password
  local permissions response identity can_copy row_count data_uri manifest_uri

  # ETag-matched DeleteObject requires BOTH s3:DeleteObject and s3:GetObject
  # (unlike If-Match:*). The CI role currently lacks GetObject. Keep every
  # live run gated until that exact-prefix grant is separately reviewed and
  # this assertion is deliberately enabled in the workflow. This is an
  # operator approval flag, not a substitute for IAM verification.
  if [[ "${PGWIRE_COPY_GETOBJECT_VERIFIED:-}" != true ]]; then
    echo '::error::Staged COPY is gated: ETag-matched cleanup needs reviewed s3:GetObject on staging/ci/*; set PGWIRE_COPY_GETOBJECT_VERIFIED=true only after verifying that grant.'
    return 1
  fi

  # This is also the exact tag for the required post-run security-group audit.
  # Do not log the runner IP or temporary database password.
  printf 'COPY ingress audit description: %s\n' "$rule_description"

  # This read-only preflight must pass before opening ingress or staging S3.
  runner_ip="$(curl --fail --silent --show-error --retry 3 https://checkip.amazonaws.com | tr -d '[:space:]')"
  python3 -c 'import ipaddress,sys; assert ipaddress.ip_address(sys.argv[1]).version == 4' "$runner_ip"
  echo "::add-mask::$runner_ip"
  cidr="${runner_ip}/32"
  printf '%s\n' "$cidr" > "$state_dir/cidr"
  rules="$(aws ec2 describe-security-group-rules \
    --filters "Name=group-id,Values=$REDSHIFT_SECURITY_GROUP_ID" --output json)"
  jq -e '.SecurityGroupRules | type == "array"' <<< "$rules" > /dev/null
  existing="$(jq -r --arg cidr "$cidr" \
    '.SecurityGroupRules[] | select(.IsEgress == false and
      .IpProtocol == "tcp" and .FromPort == 5439 and .ToPort == 5439 and
      .CidrIpv4 == $cidr) | .SecurityGroupRuleId' <<< "$rules")"
  if [[ -n "$existing" ]]; then
    echo '::error::An exact ingress rule already exists for this runner; leaving it untouched.'
    return 1
  fi

  credentials="$(aws redshift-serverless get-credentials \
    --workgroup-name "$REDSHIFT_WORKGROUP" \
    --duration-seconds 900 --output json)"
  db_user="$(jq -er '.dbUser | select(. == "IAMR:adbc-redshift-ci")' <<< "$credentials")"
  password="$(jq -er '.dbPassword | select(type == "string" and length > 0)' <<< "$credentials")"
  echo "::add-mask::$password"
  export PGHOST="$REDSHIFT_HOST" PGPORT=5439 PGDATABASE="$REDSHIFT_DBNAME"
  export PGUSER="$db_user" PGPASSWORD="$password" PGSSLMODE=verify-full
  export PGSSLROOTCERT=system
  export PGCONNECT_TIMEOUT=15
  unset credentials password

  permissions="$(jq -nc --arg cidr "$cidr" --arg description "$rule_description" \
    '[{IpProtocol:"tcp",FromPort:5439,ToPort:5439,
       IpRanges:[{CidrIp:$cidr,Description:$description}]}]')"
  : > "$state_dir/authorize-attempted"
  if ! aws ec2 authorize-security-group-ingress \
      --group-id "$REDSHIFT_SECURITY_GROUP_ID" \
      --ip-permissions "$permissions" --output json > "$state_dir/authorize.json"; then
    echo '::error::Ingress authorization failed or its response was lost; reconciling only this run description.'
    return 1
  fi
  jq -er '.SecurityGroupRules[0].SecurityGroupRuleId' \
    "$state_dir/authorize.json" > "$state_dir/authorized-rule-id"

  identity="$(psql -X -A -t -q -v ON_ERROR_STOP=1 -c 'SELECT current_user')"
  if [[ "$identity" != "$db_user" ]]; then
    echo "::error::Current database user $identity does not match the temporary IAM identity."
    return 1
  fi
  can_copy="$(psql -X -A -t -q -v ON_ERROR_STOP=1 \
    -v role_arn="$REDSHIFT_COPY_ROLE_ARN" <<'SQL'
SELECT has_assumerole_privilege(current_user, :'role_arn', 'copy');
SQL
  )"
  if [[ "$can_copy" != 't' ]]; then
    echo '::error::The IAMR database user lacks ASSUMEROLE for COPY; grant it explicitly outside this workflow.'
    return 1
  fi

  printf '1,alpha\n2,beta\n' > "$state_dir/data.csv"
  data_uri="s3://${REDSHIFT_COPY_BUCKET}/${data_key}"
  manifest_uri="s3://${REDSHIFT_COPY_BUCKET}/${manifest_key}"
  jq -n --arg url "$data_uri" \
    '{entries:[{url:$url,mandatory:true}]}' > "$state_dir/manifest.json"
  : > "$state_dir/data-upload-attempted"
  if ! aws s3api put-object --bucket "$REDSHIFT_COPY_BUCKET" \
    --key "$data_key" --body "$state_dir/data.csv" \
    --server-side-encryption AES256 --if-none-match '*' \
    --output json > "$state_dir/data-put.json"; then
    echo '::error::Conditional COPY data upload failed or its response was lost; object ownership is unconfirmed.'
    return 1
  fi
  if ! jq -er '.ETag | select(type == "string" and length > 0)' \
      "$state_dir/data-put.json" > "$state_dir/data-owned-etag"; then
    echo '::error::COPY data upload returned no usable ETag; object ownership is unconfirmed.'
    return 1
  fi
  : > "$state_dir/manifest-upload-attempted"
  if ! aws s3api put-object --bucket "$REDSHIFT_COPY_BUCKET" \
    --key "$manifest_key" --body "$state_dir/manifest.json" \
    --server-side-encryption AES256 --if-none-match '*' \
    --output json > "$state_dir/manifest-put.json"; then
    echo '::error::Conditional COPY manifest upload failed or its response was lost; object ownership is unconfirmed.'
    return 1
  fi
  if ! jq -er '.ETag | select(type == "string" and length > 0)' \
      "$state_dir/manifest-put.json" > "$state_dir/manifest-owned-etag"; then
    echo '::error::COPY manifest upload returned no usable ETag; object ownership is unconfirmed.'
    return 1
  fi

  row_count="$(psql -X -A -t -q -v ON_ERROR_STOP=1 \
    -v manifest_uri="$manifest_uri" \
    -v role_arn="$REDSHIFT_COPY_ROLE_ARN" <<'SQL'
BEGIN;
CREATE TEMP TABLE pgwire_copy_ci_fixture (id INTEGER, name VARCHAR(16));
COPY pgwire_copy_ci_fixture FROM :'manifest_uri' IAM_ROLE :'role_arn' MANIFEST CSV;
SELECT COUNT(*),
       SUM(CASE WHEN id = 1 AND name = 'alpha' THEN 1 ELSE 0 END),
       SUM(CASE WHEN id = 2 AND name = 'beta' THEN 1 ELSE 0 END)
  FROM pgwire_copy_ci_fixture;
ROLLBACK;
SQL
  )"
  if [[ "$row_count" != '2|1|1' ]]; then
    echo "::error::Expected exactly the two fixture rows; got aggregate $row_count."
    return 1
  fi
  echo 'Two-row staged COPY fixture passed.'
}

case "${1:-}" in
  run)
    trap 'on_exit $?' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    run_fixture
    ;;
  cleanup)
    cleanup
    ;;
  *)
    echo 'Usage: pgwire-redshift-copy-fixture.sh run|cleanup' >&2
    exit 2
    ;;
esac
