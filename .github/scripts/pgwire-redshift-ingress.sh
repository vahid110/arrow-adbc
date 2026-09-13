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
[[ "$GITHUB_RUN_ID" =~ ^[0-9]+$ && "$GITHUB_RUN_ATTEMPT" =~ ^[0-9]+$ ]]

rule_description="adbc-pgwire-live-${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}"
state_dir="${RUNNER_TEMP}/pgwire-redshift-ingress-${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}"
mkdir -p "$state_dir"

describe_rules() {
  local rules
  rules="$(aws ec2 describe-security-group-rules \
    --filters "Name=group-id,Values=$REDSHIFT_SECURITY_GROUP_ID" \
    --output json)" || return
  jq -e '.SecurityGroupRules | type == "array"' <<< "$rules" > /dev/null || return
  printf '%s\n' "$rules"
}

owned_ids() {
  local cidr="$1"
  jq -r --arg group "$REDSHIFT_SECURITY_GROUP_ID" \
    --arg cidr "$cidr" --arg description "$rule_description" \
    '.SecurityGroupRules[] | select(.GroupId == $group and
      .IsEgress == false and .IpProtocol == "tcp" and
      .FromPort == 5439 and .ToPort == 5439 and
      .CidrIpv4 == $cidr and .Description == $description and
      (.SecurityGroupRuleId | type == "string" and length > 0)) |
      .SecurityGroupRuleId'
}

cleanup() {
  local cidr rules ids='' rule_id result verify_rules attempt failed=0
  [[ -f "$state_dir/authorize-attempted" ]] || return 0
  [[ -f "$state_dir/ingress-revoked" ]] && return 0
  if [[ ! -s "$state_dir/cidr" ]]; then
    echo "::error::Missing saved CIDR for $rule_description; audit its ingress rule independently."
    return 1
  fi
  cidr="$(< "$state_dir/cidr")"

  # The authorize response may be lost after EC2 creates the rule. Describe
  # only the known group, and revoke only this run's exact description/CIDR.
  for attempt in {1..12}; do
    if rules="$(describe_rules)"; then
      ids="$(owned_ids "$cidr" <<< "$rules")"
      [[ -n "$ids" ]] && break
    fi
    sleep 2
  done
  # A successful authorize response supplies a verified ownership-safe ID
  # when Describe is eventually inconsistent. Never fall back to a CIDR.
  if [[ -z "$ids" && -s "$state_dir/authorized-rule-id" ]]; then
    ids="$(< "$state_dir/authorized-rule-id")"
  fi
  if [[ -z "$ids" ]]; then
    echo "::error::Could not confirm whether $rule_description exists; audit this exact rule independently."
    return 1
  fi

  while IFS= read -r rule_id; do
    if ! result="$(aws ec2 revoke-security-group-ingress \
        --group-id "$REDSHIFT_SECURITY_GROUP_ID" \
        --security-group-rule-ids "$rule_id" \
        --query Return --output text)" || [[ "$result" != True ]]; then
      # A lost revoke response is safe only if a fresh scoped read confirms
      # the exact ID absent. A failed read must remain a visible failure.
      if ! verify_rules="$(describe_rules)" ||
          jq -e --arg id "$rule_id" \
            '.SecurityGroupRules[] | select(.SecurityGroupRuleId == $id)' \
            <<< "$verify_rules" > /dev/null; then
        echo "::error::Could not confirm removal of ingress rule $rule_id ($rule_description)."
        failed=1
      fi
    fi
  done <<< "$ids"
  if (( failed == 0 )); then
    : > "$state_dir/ingress-revoked"
  fi
  return "$failed"
}

open_ingress() {
  local cidr rules existing permissions id
  : "${RUNNER_CIDR:?}"
  python3 -c 'import ipaddress,sys; n=ipaddress.ip_network(sys.argv[1],strict=True); assert n.version == 4 and n.prefixlen == 32' "$RUNNER_CIDR"
  if [[ -f "$state_dir/authorize-attempted" ]]; then
    echo "::error::Ingress authorization was already attempted for $rule_description."
    return 1
  fi
  cidr="$RUNNER_CIDR"
  printf '%s\n' "$cidr" > "$state_dir/cidr"
  rules="$(describe_rules)" || return
  existing="$(jq -r --arg group "$REDSHIFT_SECURITY_GROUP_ID" \
    --arg cidr "$cidr" \
    '.SecurityGroupRules[] | select(.GroupId == $group and
      .IsEgress == false and .IpProtocol == "tcp" and
      .FromPort == 5439 and .ToPort == 5439 and
      .CidrIpv4 == $cidr) | .SecurityGroupRuleId' <<< "$rules")"
  if [[ -n "$existing" ]]; then
    echo '::error::An ingress rule already exists for this runner /32; leaving it untouched.'
    return 1
  fi

  permissions="$(jq -nc --arg cidr "$cidr" --arg description "$rule_description" \
    '[{IpProtocol:"tcp",FromPort:5439,ToPort:5439,
       IpRanges:[{CidrIp:$cidr,Description:$description}]}]')"
  : > "$state_dir/authorize-attempted"
  if ! aws ec2 authorize-security-group-ingress \
      --group-id "$REDSHIFT_SECURITY_GROUP_ID" \
      --ip-permissions "$permissions" --output json > "$state_dir/authorize.json"; then
    echo "::error::Ingress authorization failed or response was lost for $rule_description."
    return 1
  fi
  # Only trust an ID if the returned rule itself matches every ownership field.
  if ! id="$(jq -er --arg group "$REDSHIFT_SECURITY_GROUP_ID" \
      --arg cidr "$cidr" --arg description "$rule_description" \
      '.SecurityGroupRules | map(select(.GroupId == $group and
        .IsEgress == false and .IpProtocol == "tcp" and
        .FromPort == 5439 and .ToPort == 5439 and
        .CidrIpv4 == $cidr and .Description == $description and
        (.SecurityGroupRuleId | type == "string" and length > 0))) |
        if length == 1 then .[0].SecurityGroupRuleId else empty end' \
      "$state_dir/authorize.json")"; then
    echo "::error::Ingress authorization returned no verified owned rule ID for $rule_description."
    return 1
  fi
  printf '%s\n' "$id" > "$state_dir/authorized-rule-id"
  printf 'Redshift ingress audit description: %s\n' "$rule_description"
}

case "${1:-}" in
  open)
    on_exit() {
      local status="$1"
      trap - EXIT
      if (( status != 0 )) && ! cleanup; then status=1; fi
      exit "$status"
    }
    trap 'on_exit $?' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    open_ingress
    ;;
  cleanup)
    cleanup
    ;;
  *)
    echo 'Usage: pgwire-redshift-ingress.sh open|cleanup' >&2
    exit 2
    ;;
esac
