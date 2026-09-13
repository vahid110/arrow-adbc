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
helper="$script_dir/pgwire-redshift-ingress.sh"
test_root="$(mktemp -d "${TMPDIR:-/tmp}/pgwire-ingress-test.XXXXXX")"
finish() {
  [[ "$test_root" == */pgwire-ingress-test.* && -d "$test_root" ]] || return
  rm -r -- "$test_root"
}
trap finish EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

aws() {
  local service="${1:-}" operation="${2:-}" previous='' argument permissions=''
  printf '%s\n' "$*" >> "$MOCK_DIR/aws-calls"
  case "$service/$operation" in
    ec2/describe-security-group-rules)
      if [[ "$MOCK_CASE" == describe_denied ||
            ( "$MOCK_CASE" == ambiguous_unresolved && -f "$MOCK_DIR/attempted" ) ||
            ( "$MOCK_CASE" == describe_after_auth_denied && -f "$MOCK_DIR/attempted" ) ]]; then
        return 253
      fi
      if [[ "$MOCK_CASE" == stale_describe_revoke_denied && -f "$MOCK_DIR/attempted" ]]; then
        printf '{"SecurityGroupRules":[]}\n'
        return 0
      fi
      if [[ -f "$MOCK_DIR/rule" ]]; then
        jq -n --arg description "$(< "$MOCK_DIR/rule")" \
          --arg group "$REDSHIFT_SECURITY_GROUP_ID" \
          '{SecurityGroupRules:[{SecurityGroupRuleId:"sgr-owned-test",
            GroupId:$group,IsEgress:false,IpProtocol:"tcp",FromPort:5439,
            ToPort:5439,CidrIpv4:"203.0.113.7/32",Description:$description}]}'
      else
        printf '{"SecurityGroupRules":[]}\n'
      fi
      ;;
    ec2/authorize-security-group-ingress)
      for argument in "$@"; do
        if [[ "$previous" == --ip-permissions ]]; then permissions="$argument"; break; fi
        previous="$argument"
      done
      jq -e --arg description "adbc-pgwire-live-${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}" \
        '.[0].IpProtocol == "tcp" and .[0].FromPort == 5439 and
         .[0].ToPort == 5439 and .[0].IpRanges[0].CidrIp == "203.0.113.7/32" and
         .[0].IpRanges[0].Description == $description' \
        <<< "$permissions" > /dev/null || return 40
      : > "$MOCK_DIR/attempted"
      printf 'adbc-pgwire-live-%s-%s\n' "$GITHUB_RUN_ID" "$GITHUB_RUN_ATTEMPT" > "$MOCK_DIR/rule"
      if [[ "$MOCK_CASE" == authorize_response_lost ||
            "$MOCK_CASE" == ambiguous_unresolved ]]; then return 255; fi
      if [[ "$MOCK_CASE" == authorize_no_verified_id ]]; then
        printf '{"SecurityGroupRules":[{"SecurityGroupRuleId":"sgr-owned-test"}]}\n'
      else
        jq -n --arg description "$(< "$MOCK_DIR/rule")" \
          --arg group "$REDSHIFT_SECURITY_GROUP_ID" \
          '{SecurityGroupRules:[{SecurityGroupRuleId:"sgr-owned-test",
            GroupId:$group,IsEgress:false,IpProtocol:"tcp",FromPort:5439,
            ToPort:5439,CidrIpv4:"203.0.113.7/32",Description:$description}]}'
      fi
      ;;
    ec2/revoke-security-group-ingress)
      [[ "$*" == *'--security-group-rule-ids sgr-owned-test'* ]] || return 41
      if [[ "$MOCK_CASE" == revoke_denied ||
            "$MOCK_CASE" == stale_describe_revoke_denied ]]; then return 253; fi
      rm -- "$MOCK_DIR/rule"
      if [[ "$MOCK_CASE" == revoke_response_lost ]]; then return 255; fi
      printf 'True\n'
      ;;
    *) return 42 ;;
  esac
}
sleep() { :; }
export -f aws sleep

run_case() {
  local scenario="$1" expected_open="$2" expected_cleanup="$3"
  local case_dir="$test_root/$scenario" status=0 cleanup_status=0
  mkdir -p "$case_dir"
  (
    export MOCK_CASE="$scenario" MOCK_DIR="$case_dir" RUNNER_TEMP="$case_dir"
    export GITHUB_RUN_ID=12345 GITHUB_RUN_ATTEMPT=2
    export REDSHIFT_SECURITY_GROUP_ID=sg-test RUNNER_CIDR=203.0.113.7/32
    : > "$MOCK_DIR/aws-calls"
    if [[ "$scenario" == existing_other_owner ]]; then
      printf 'someone-else\n' > "$MOCK_DIR/rule"
    fi
    if [[ "$scenario" == invalid_cidr ]]; then RUNNER_CIDR=203.0.113.0/24; fi
    bash "$helper" open > "$MOCK_DIR/open-output" 2>&1 || status=$?
    if [[ "$expected_open" == success ]]; then
      [[ "$status" == 0 ]] || fail "$scenario open failed unexpectedly: $(< "$MOCK_DIR/open-output")"
    else
      [[ "$status" != 0 ]] || fail "$scenario open passed unexpectedly"
    fi
    bash "$helper" cleanup > "$MOCK_DIR/cleanup-output" 2>&1 || cleanup_status=$?
    if [[ "$expected_cleanup" == success ]]; then
      [[ "$cleanup_status" == 0 ]] || fail "$scenario cleanup failed unexpectedly: $(< "$MOCK_DIR/cleanup-output")"
    else
      [[ "$cleanup_status" != 0 ]] || fail "$scenario cleanup passed unexpectedly"
    fi
    case "$scenario" in
      success|authorize_response_lost|authorize_no_verified_id|revoke_response_lost|describe_after_auth_denied)
        [[ ! -f "$MOCK_DIR/rule" ]] || fail "$scenario left owned ingress open"
        ;;
      existing_other_owner)
        [[ "$(< "$MOCK_DIR/rule")" == someone-else ]] || fail 'Foreign ingress was changed'
        ! grep -q 'revoke-security-group-ingress' "$MOCK_DIR/aws-calls" || fail 'Foreign rule was revoked'
        ;;
      describe_denied|invalid_cidr)
        ! grep -q 'authorize-security-group-ingress' "$MOCK_DIR/aws-calls" || fail "$scenario attempted authorization"
        ;;
      revoke_denied|stale_describe_revoke_denied|ambiguous_unresolved)
        [[ -f "$MOCK_DIR/rule" ]] || fail "$scenario removed an unresolved rule"
        if [[ "$scenario" == ambiguous_unresolved ]]; then
          ! grep -q 'revoke-security-group-ingress' "$MOCK_DIR/aws-calls" || fail 'Ambiguous ownership was revoked'
        fi
        ;;
    esac
    if [[ "$cleanup_status" != 0 ]]; then
      [[ ! -f "$RUNNER_TEMP/pgwire-redshift-ingress-${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}/ingress-revoked" ]] ||
        fail "$scenario recorded a failed cleanup as complete"
    fi
    if [[ "$cleanup_status" == 0 ]]; then
      cp "$MOCK_DIR/aws-calls" "$MOCK_DIR/before-repeat"
      bash "$helper" cleanup > "$MOCK_DIR/repeat-output" 2>&1 || fail "$scenario repeat cleanup failed"
      cmp "$MOCK_DIR/aws-calls" "$MOCK_DIR/before-repeat" > /dev/null || fail "$scenario repeated AWS calls"
    fi
  )
  printf 'PASS: %s\n' "$scenario"
}

run_case success success success
run_case existing_other_owner failure success
run_case describe_denied failure success
run_case invalid_cidr failure success
run_case authorize_response_lost failure success
run_case authorize_no_verified_id failure success
run_case revoke_response_lost success failure
run_case describe_after_auth_denied success success
run_case revoke_denied success failure
run_case stale_describe_revoke_denied success failure
run_case ambiguous_unresolved failure failure
