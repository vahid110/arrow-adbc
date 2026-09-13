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

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fixture_script="$script_dir/pgwire-redshift-copy-fixture.sh"
test_root="$(mktemp -d "${TMPDIR:-/tmp}/pgwire-copy-test.XXXXXX")"
finish() {
  [[ "$test_root" == */pgwire-copy-test.* && -d "$test_root" ]] || return
  rm -r -- "$test_root"
}
trap finish EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
contains() { [[ "$(< "$1")" == *"$2"* ]] || fail "Missing $2 in $1"; }
excludes() { [[ "$(< "$1")" != *"$2"* ]] || fail "Unexpected $2 in $1"; }

aws() {
  local service="${1:-}" operation="${2:-}" description body='' previous='' argument
  local object_file object_etag requested_etag expected_csv
  printf '%s\n' "$*" >> "$MOCK_DIR/aws-calls"
  case "$service/$operation" in
    ec2/describe-security-group-rules)
      if [[ "$MOCK_CASE" == describe_denied ]]; then return 253; fi
      if [[ -f "$MOCK_DIR/rule" ]]; then
        description="$(< "$MOCK_DIR/rule")"
        jq -n --arg description "$description" \
          --arg group "$REDSHIFT_SECURITY_GROUP_ID" \
          '{SecurityGroupRules:[{SecurityGroupRuleId:"sgr-owned-test",
            GroupId:$group,IsEgress:false,IpProtocol:"tcp",FromPort:5439,
            ToPort:5439,CidrIpv4:"203.0.113.7/32",Description:$description}]}'
      else
        printf '{"SecurityGroupRules":[]}\n'
      fi
      ;;
    ec2/authorize-security-group-ingress)
      printf 'adbc-pgwire-copy-%s-%s\n' "$GITHUB_RUN_ID" "$GITHUB_RUN_ATTEMPT" > "$MOCK_DIR/rule"
      if [[ "$MOCK_CASE" == authorize_response_lost ]]; then return 255; fi
      printf '{"Return":true,"SecurityGroupRules":[{"SecurityGroupRuleId":"sgr-owned-test"}]}\n'
      ;;
    ec2/revoke-security-group-ingress)
      [[ "$*" == *'--security-group-rule-ids sgr-owned-test'* ]] || return 40
      rm -- "$MOCK_DIR/rule"
      if [[ "$MOCK_CASE" == revoke_response_lost ]]; then return 255; fi
      printf 'True\n'
      ;;
    redshift-serverless/get-credentials)
      printf '{"dbUser":"IAMR:adbc-redshift-ci","dbPassword":"offline-test-only"}\n'
      ;;
    s3api/put-object)
      [[ "$*" == *'--if-none-match *'* ]] || return 50
      for argument in "$@"; do
        if [[ "$previous" == --body ]]; then body="$argument"; break; fi
        previous="$argument"
      done
      if [[ "$*" == *'/data.csv'* ]]; then
        if [[ "$MOCK_CASE" == csv_semantics_* ]]; then
          expected_csv=$'1,""\n2,\n3,\\N\n4,"\\N"\n5,"a,b"\n6,"a ""quote"""\n7,"line\nbreak"\n9223372036854775807,"max"\n-9223372036854775808,"min"'
        else
          expected_csv=$'1,alpha\n2,beta'
        fi
        [[ "$(< "$body")" == "$expected_csv" ]] || return 45
        object_file="$MOCK_DIR/data-object"
        object_etag='"etag-data"'
        [[ ! -f "$object_file" ]] || return 254
        printf '%s\n' "$object_etag" > "$object_file"
        if [[ "$MOCK_CASE" == partial_upload ]]; then return 255; fi
        if [[ "$MOCK_CASE" == data_put_no_etag ]]; then printf '{}\n'; return; fi
      elif [[ "$*" == *'/manifest.json'* ]]; then
        jq -e '.entries | length == 1 and .[0].mandatory == true and
          .[0].url == "s3://bucket-test/staging/ci/12345-2/data.csv"' \
          "$body" > /dev/null || return 46
        object_file="$MOCK_DIR/manifest-object"
        object_etag='"etag-manifest"'
        [[ ! -f "$object_file" ]] || return 254
        printf '%s\n' "$object_etag" > "$object_file"
        if [[ "$MOCK_CASE" == manifest_upload_response_lost ]]; then return 255; fi
        if [[ "$MOCK_CASE" == manifest_put_no_etag ]]; then printf '{}\n'; return; fi
      else
        return 41
      fi
      jq -n --arg etag "$object_etag" '{ETag:$etag}'
      ;;
    s3api/delete-object)
      for argument in "$@"; do
        if [[ "$previous" == --if-match ]]; then requested_etag="$argument"; break; fi
        previous="$argument"
      done
      [[ -n "$requested_etag" ]] || return 51
      if [[ "$*" == *'/data.csv'* ]]; then
        object_file="$MOCK_DIR/data-object"
      elif [[ "$*" == *'/manifest.json'* ]]; then
        object_file="$MOCK_DIR/manifest-object"
      else
        return 42
      fi
      [[ -f "$object_file" && "$(< "$object_file")" == "$requested_etag" ]] || return 254
      rm -- "$object_file"
      ;;
    *) return 43 ;;
  esac
}
curl() { printf '203.0.113.7\n'; }
psql() {
  local sql
  [[ "${PGSSLMODE:-}" == verify-full && "${PGSSLROOTCERT:-}" == system ]] || return 47
  if [[ "$*" == *'SELECT current_user'* ]]; then
    if [[ "$MOCK_CASE" == identity_mismatch ]]; then
      printf 'someone-else\n'
    else
      printf 'IAMR:adbc-redshift-ci\n'
    fi
    return
  fi
  sql="$(cat)"
  if [[ "$sql" == *'has_assumerole_privilege'* ]]; then
    if [[ "$MOCK_CASE" == assumerole_denied ]]; then
      printf 'f\n'
    else
      printf 't\n'
    fi
  elif [[ "$sql" == *'COPY pgwire_copy_csv_semantics_fixture'* ]]; then
    [[ "$sql" == *'SELECT COUNT(*)'* &&
       "$sql" == *"id = 1 AND name = ''"* &&
       "$sql" == *"id = 2 AND name = ''"* &&
       "$sql" == *"id = 3 AND name IS NULL"* &&
       "$sql" == *"id = 4 AND name = CHR(92) || 'N'"* &&
       "$sql" == *"id = 5 AND name = 'a,b'"* &&
       "$sql" == *"id = 6 AND name = 'a \"quote\"'"* &&
       "$sql" == *"id = 7 AND name = 'line' || CHR(10) || 'break'"* &&
       "$sql" == *"CAST('9223372036854775807' AS BIGINT)"* &&
       "$sql" == *"CAST('-9223372036854775808' AS BIGINT)"* &&
       "$sql" == *'ROLLBACK;'* ]] || return 52
    case "$MOCK_CASE" in
      csv_semantics_null) printf '9|1|1|1|1|1|1|1|1|1|0\n' ;;
      csv_semantics_literal) printf '9|1|1|1|1|1|1|1|1|0|1\n' ;;
      csv_semantics_other) printf '9|1|1|1|1|1|1|1|1|0|0\n' ;;
      csv_semantics_bad_escape) printf '9|1|1|1|1|0|1|1|1|0|1\n' ;;
      csv_semantics_bad_marker) printf '9|1|1|1|1|1|1|1|1|2|0\n' ;;
      *) return 53 ;;
    esac
  elif [[ "$sql" == *'COPY pgwire_copy_ci_fixture'* ]]; then
    [[ "$sql" == *"id = 1 AND name = 'alpha'"* &&
       "$sql" == *"id = 2 AND name = 'beta'"* ]] || return 48
    if [[ "$MOCK_CASE" == copy_query_failed ]]; then return 49; fi
    if [[ "$MOCK_CASE" == object_replaced ]]; then
      printf '"etag-foreign"\n' > "$MOCK_DIR/data-object"
    fi
    printf '2|1|1\n'
  else
    return 44
  fi
}
export -f aws curl psql

run_case() {
  local scenario="$1" expected="$2" case_dir="$test_root/$1" status=0
  local cleanup_status=0 fixture_mode=run
  mkdir -p "$case_dir"
  (
    export MOCK_CASE="$scenario" MOCK_DIR="$case_dir" RUNNER_TEMP="$case_dir"
    export GITHUB_RUN_ID=12345 GITHUB_RUN_ATTEMPT=2
    export REDSHIFT_SECURITY_GROUP_ID=sg-test
    export REDSHIFT_COPY_BUCKET=bucket-test
    export REDSHIFT_COPY_ROLE_ARN=arn:aws:iam::149112076833:role/adbc-pgwire-ci-copy
    export REDSHIFT_WORKGROUP=pgwire-ci
    export REDSHIFT_HOST=pgwire-ci.example.com REDSHIFT_DBNAME=dev
    export PGWIRE_COPY_GETOBJECT_VERIFIED=true
    : > "$MOCK_DIR/aws-calls"
    if [[ "$scenario" == missing_getobject_approval ||
          "$scenario" == missing_getobject_approval_csv ]]; then
      unset PGWIRE_COPY_GETOBJECT_VERIFIED
    fi
    if [[ "$scenario" == csv_semantics_* ||
          "$scenario" == missing_getobject_approval_csv ]]; then
      fixture_mode=csv-semantics
    fi
    if [[ "$scenario" == existing_other_owner ]]; then
      printf 'someone-else\n' > "$MOCK_DIR/rule"
    fi
    if [[ "$scenario" == data_collision ]]; then
      printf '"etag-foreign"\n' > "$MOCK_DIR/data-object"
    fi
    if [[ "$scenario" == manifest_collision ]]; then
      printf '"etag-foreign"\n' > "$MOCK_DIR/manifest-object"
    fi
    bash "$fixture_script" "$fixture_mode" > "$MOCK_DIR/output" 2>&1 || status=$?
    if [[ "$expected" == success && "$status" != 0 ]]; then
      fail "$scenario failed unexpectedly: $(< "$MOCK_DIR/output")"
    fi
    if [[ "$expected" == failure && "$status" == 0 ]]; then
      fail "$scenario passed unexpectedly"
    fi
    case "$scenario" in
      missing_getobject_approval|missing_getobject_approval_csv)
        contains "$MOCK_DIR/output" 'ETag-matched cleanup needs reviewed s3:GetObject'
        [[ ! -s "$MOCK_DIR/aws-calls" ]] || fail 'Unapproved COPY gate made an AWS call'
        ;;
      describe_denied)
        excludes "$MOCK_DIR/aws-calls" 'authorize-security-group-ingress'
        excludes "$MOCK_DIR/aws-calls" 'put-object'
        excludes "$MOCK_DIR/aws-calls" 'delete-object'
        excludes "$MOCK_DIR/aws-calls" 'revoke-security-group-ingress'
        ;;
      existing_other_owner)
        contains "$MOCK_DIR/output" 'leaving it untouched'
        excludes "$MOCK_DIR/aws-calls" 'authorize-security-group-ingress'
        excludes "$MOCK_DIR/aws-calls" 'revoke-security-group-ingress'
        excludes "$MOCK_DIR/aws-calls" 'put-object'
        ;;
      authorize_response_lost)
        contains "$MOCK_DIR/aws-calls" '--security-group-rule-ids sgr-owned-test'
        excludes "$MOCK_DIR/aws-calls" 'put-object'
        [[ ! -f "$MOCK_DIR/rule" ]] || fail 'Owned ingress rule remained after lost response'
        ;;
      identity_mismatch|assumerole_denied)
        contains "$MOCK_DIR/aws-calls" '--security-group-rule-ids sgr-owned-test'
        excludes "$MOCK_DIR/aws-calls" 'put-object'
        [[ ! -f "$MOCK_DIR/rule" ]] || fail "$scenario ingress rule remained"
        ;;
      revoke_response_lost)
        contains "$MOCK_DIR/aws-calls" '--security-group-rule-ids sgr-owned-test'
        [[ ! -f "$MOCK_DIR/rule" ]] || fail 'Owned ingress rule remained after lost revoke response'
        ;;
      partial_upload)
        contains "$MOCK_DIR/output" 'ownership is unconfirmed'
        excludes "$MOCK_DIR/aws-calls" 'delete-object'
        [[ -f "$MOCK_DIR/data-object" && ! -f "$MOCK_DIR/rule" ]] || fail 'Unconfirmed data object or ingress state changed'
        ;;
      data_collision|data_put_no_etag)
        contains "$MOCK_DIR/output" 'ownership is unconfirmed'
        excludes "$MOCK_DIR/aws-calls" 'delete-object'
        [[ -f "$MOCK_DIR/data-object" && ! -f "$MOCK_DIR/rule" ]] || fail "$scenario changed an unowned data object or ingress rule"
        ;;
      manifest_upload_response_lost|manifest_collision|manifest_put_no_etag)
        contains "$MOCK_DIR/aws-calls" 'delete-object --bucket bucket-test --key staging/ci/12345-2/data.csv'
        excludes "$MOCK_DIR/aws-calls" 'delete-object --bucket bucket-test --key staging/ci/12345-2/manifest.json'
        contains "$MOCK_DIR/aws-calls" '--security-group-rule-ids sgr-owned-test'
        [[ ! -f "$MOCK_DIR/data-object" && -f "$MOCK_DIR/manifest-object" && ! -f "$MOCK_DIR/rule" ]] || fail "$scenario changed an unowned manifest object or left owned resources"
        ;;
      object_replaced)
        contains "$MOCK_DIR/output" 'Could not confirm ETag-conditional deletion'
        contains "$MOCK_DIR/aws-calls" 'delete-object --bucket bucket-test --key staging/ci/12345-2/data.csv --if-match "etag-data"'
        [[ -f "$MOCK_DIR/data-object" && ! -f "$MOCK_DIR/manifest-object" && ! -f "$MOCK_DIR/rule" ]] || fail 'Replaced object was deleted or owned resources remained'
        ;;
      copy_query_failed)
        contains "$MOCK_DIR/aws-calls" 'delete-object --bucket bucket-test --key staging/ci/12345-2/data.csv --if-match "etag-data"'
        contains "$MOCK_DIR/aws-calls" 'delete-object --bucket bucket-test --key staging/ci/12345-2/manifest.json --if-match "etag-manifest"'
        [[ ! -f "$MOCK_DIR/data-object" && ! -f "$MOCK_DIR/manifest-object" && ! -f "$MOCK_DIR/rule" ]] || fail "$scenario resources remained"
        ;;
      csv_semantics_null|csv_semantics_literal|csv_semantics_other)
        contains "$MOCK_DIR/output" 'Nine-row CSV semantics fixture passed'
        case "$scenario" in
          csv_semantics_null) contains "$MOCK_DIR/output" 'classified as null' ;;
          csv_semantics_literal) contains "$MOCK_DIR/output" 'classified as literal' ;;
          csv_semantics_other) contains "$MOCK_DIR/output" 'classified as other' ;;
        esac
        [[ ! -f "$MOCK_DIR/data-object" && ! -f "$MOCK_DIR/manifest-object" && ! -f "$MOCK_DIR/rule" ]] || fail "$scenario resources remained"
        ;;
      csv_semantics_bad_escape)
        contains "$MOCK_DIR/output" 'CSV semantics fixture failed its nine-row aggregate'
        [[ ! -f "$MOCK_DIR/data-object" && ! -f "$MOCK_DIR/manifest-object" && ! -f "$MOCK_DIR/rule" ]] || fail "$scenario resources remained"
        ;;
      csv_semantics_bad_marker)
        contains "$MOCK_DIR/output" 'CSV quoted-null-marker classification was malformed'
        [[ ! -f "$MOCK_DIR/data-object" && ! -f "$MOCK_DIR/manifest-object" && ! -f "$MOCK_DIR/rule" ]] || fail "$scenario resources remained"
        ;;
      success)
        contains "$MOCK_DIR/output" 'Two-row staged COPY fixture passed.'
        contains "$MOCK_DIR/aws-calls" 'delete-object --bucket bucket-test --key staging/ci/12345-2/data.csv --if-match "etag-data"'
        contains "$MOCK_DIR/aws-calls" 'delete-object --bucket bucket-test --key staging/ci/12345-2/manifest.json --if-match "etag-manifest"'
        [[ ! -f "$MOCK_DIR/data-object" && ! -f "$MOCK_DIR/manifest-object" && ! -f "$MOCK_DIR/rule" ]] || fail 'Success resources remained'
        ;;
    esac
    # The workflow's always() step calls cleanup again. It must be idempotent.
    cp "$MOCK_DIR/aws-calls" "$MOCK_DIR/before-final-cleanup"
    bash "$fixture_script" cleanup >> "$MOCK_DIR/output" 2>&1 || cleanup_status=$?
    case "$scenario" in
      partial_upload|data_collision|data_put_no_etag|manifest_upload_response_lost|manifest_collision|manifest_put_no_etag|object_replaced)
        [[ "$cleanup_status" != 0 ]] || fail "$scenario concealed incomplete cleanup"
        ;;
      *)
        [[ "$cleanup_status" == 0 ]] || fail "$scenario final cleanup failed"
        ;;
    esac
    if [[ "$scenario" != object_replaced ]]; then
      cmp "$MOCK_DIR/aws-calls" "$MOCK_DIR/before-final-cleanup" > /dev/null || fail "$scenario repeated a mutation"
    fi
  )
  printf 'PASS: %s\n' "$scenario"
}

run_case missing_getobject_approval failure
run_case missing_getobject_approval_csv failure
run_case describe_denied failure
run_case existing_other_owner failure
run_case authorize_response_lost failure
run_case identity_mismatch failure
run_case assumerole_denied failure
run_case revoke_response_lost success
run_case partial_upload failure
run_case data_collision failure
run_case data_put_no_etag failure
run_case manifest_upload_response_lost failure
run_case manifest_collision failure
run_case manifest_put_no_etag failure
run_case object_replaced failure
run_case copy_query_failed failure
run_case success success
run_case csv_semantics_null success
run_case csv_semantics_literal success
run_case csv_semantics_other success
run_case csv_semantics_bad_escape failure
run_case csv_semantics_bad_marker failure
