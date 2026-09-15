#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:25500}"
ASSERT_LITE_OUTPUT="${ASSERT_LITE_OUTPUT:-0}"
LITE_MAX_SNAPSHOT_BYTES="${LITE_MAX_SNAPSHOT_BYTES:-262144}"

# Authenticate either with a provisioned API key (X-API-Key) or with the
# service admin token (Authorization: Bearer).
AUTH_ARGS=()
if [[ -n "${API_KEY:-}" ]]; then
    AUTH_ARGS+=(-H "X-API-Key: ${API_KEY}")
fi
if [[ -n "${API_TOKEN:-}" ]]; then
    AUTH_ARGS+=(-H "Authorization: Bearer ${API_TOKEN}")
fi
if [[ ${#AUTH_ARGS[@]} -eq 0 ]]; then
    echo 'API_KEY or API_TOKEN is required' >&2
    exit 1
fi

assert_lite_snapshot()
{
    local body="$1"
    local body_size
    body_size=$(wc -c <<<"${body}")
    if (( body_size > LITE_MAX_SNAPSHOT_BYTES )); then
        echo "shortlink-lite-smoke-failed: snapshot is ${body_size} bytes (limit ${LITE_MAX_SNAPSHOT_BYTES})" >&2
        exit 1
    fi

    if ! grep -q '^rule-providers:' <<<"${body}" || ! grep -q 'RULE-SET,' <<<"${body}"; then
        echo 'shortlink-lite-smoke-failed: snapshot is not using rule providers' >&2
        exit 1
    fi
}

payload='{"name":"smoke-test","target":"clash","expires_in":3600,"links":["ss://YWVzLTEyOC1nY206Zml4dHVyZQ==@198.51.100.10:443#smoke"]}'
response=$(curl -fsS --max-time 30 -X POST -H 'Content-Type: application/json' "${AUTH_ARGS[@]}" --data-binary "${payload}" "${BASE_URL}/api/short-links")
id=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])' <<<"${response}")
url=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["short_url"])' <<<"${response}")

snapshot=$(curl -fsS --max-time 20 "${url}")
grep -q 'smoke' <<<"${snapshot}"
if [[ "${ASSERT_LITE_OUTPUT}" == "1" ]]; then
    assert_lite_snapshot "${snapshot}"
fi
list_response=$(curl -fsS --max-time 10 "${AUTH_ARGS[@]}" "${BASE_URL}/api/short-links")
download_url=$(python3 -c 'import json,sys; data=json.load(sys.stdin); print(next(item["download_url"] for item in data["items"] if item["id"] == sys.argv[1]))' "${id}" <<<"${list_response}")
download_headers=$(curl -fsS --max-time 20 -D - -o /dev/null "${download_url}")
grep -Eiq 'Content-Disposition: attachment; filename="custom-clash-[0-9]{6}(-[0-9]+)?\.yaml"' <<<"${download_headers}"
refresh_response=$(curl -fsS --max-time 30 -X POST --data-binary "" "${AUTH_ARGS[@]}" "${BASE_URL}/api/short-links/${id}/refresh")
grep -q 'refreshed' <<<"${refresh_response}"
refreshed_snapshot=$(curl -fsS --max-time 20 "${url}")
grep -q 'smoke' <<<"${refreshed_snapshot}"
if [[ "${ASSERT_LITE_OUTPUT}" == "1" ]]; then
    assert_lite_snapshot "${refreshed_snapshot}"
fi
revoke_response=$(curl -fsS --max-time 10 -X DELETE "${AUTH_ARGS[@]}" "${BASE_URL}/api/short-links/${id}")
grep -q 'revoked' <<<"${revoke_response}"
status_code=$(curl -sS --max-time 10 -o /dev/null -w '%{http_code}' "${url}")
if [[ "${status_code}" != "410" ]]; then
    echo 'shortlink-smoke-failed' >&2
    exit 1
fi

# --------------------------------------------------------------- sing-box
sb_payload='{"name":"smoke-singbox","target":"singbox","platform":"openwrt","expires_in":3600,"links":["ss://YWVzLTEyOC1nY206Zml4dHVyZQ==@198.51.100.10:443#smoke"]}'
sb_response=$(curl -fsS --max-time 30 -X POST -H 'Content-Type: application/json' "${AUTH_ARGS[@]}" --data-binary "${sb_payload}" "${BASE_URL}/api/short-links")
python3 -c 'import json,sys; data=json.load(sys.stdin); assert data["target"] == "singbox", data; assert data["platform"] == "openwrt", data' <<<"${sb_response}"
sb_id=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])' <<<"${sb_response}")
sb_url=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["short_url"])' <<<"${sb_response}")

sb_snapshot_file=$(mktemp)
curl -fsS --max-time 20 -o "${sb_snapshot_file}" "${sb_url}"
python3 - "${sb_snapshot_file}" <<'PY_SINGBOX'
import json
import sys

doc = json.load(open(sys.argv[1]))
assert doc["$schema"] == "https://sing-box.sagernet.org/schema.json"
assert all("address" not in server for server in doc["dns"]["servers"]), doc["dns"]["servers"]
assert all("type" in server for server in doc["dns"]["servers"])
assert doc["route"]["default_domain_resolver"] == "dns-direct"
assert doc["route"]["rules"][0] == {"action": "sniff"}
assert any(rule.get("inbound") == ["dns-in"] for rule in doc["route"]["rules"])
assert all(entry["type"] == "local" for entry in doc["route"]["rule_set"])
assert doc["experimental"]["clash_api"]["external_controller"] == "127.0.0.1:9095"
assert any(outbound.get("tag") == "smoke" for outbound in doc["outbounds"]), "node missing"
PY_SINGBOX
rm -f "${sb_snapshot_file}"

sb_list=$(curl -fsS --max-time 10 "${AUTH_ARGS[@]}" "${BASE_URL}/api/short-links")
sb_download_url=$(python3 -c 'import json,sys; data=json.load(sys.stdin); print(next(item["download_url"] for item in data["items"] if item["id"] == sys.argv[1]))' "${sb_id}" <<<"${sb_list}")
sb_headers=$(curl -fsS --max-time 20 -D - -o /dev/null "${sb_download_url}")
grep -Eiq 'Content-Disposition: attachment; filename="custom-singbox-[0-9]{6}(-[0-9]+)?-openwrt\.json"' <<<"${sb_headers}"
grep -Eiq 'Content-Type: application/json' <<<"${sb_headers}"
curl -fsS --max-time 30 -X POST --data-binary "" "${AUTH_ARGS[@]}" "${BASE_URL}/api/short-links/${sb_id}/refresh" | grep -q refreshed
curl -fsS --max-time 10 -X DELETE "${AUTH_ARGS[@]}" "${BASE_URL}/api/short-links/${sb_id}" | grep -q revoked

bad_platform=$(curl -sS --max-time 10 -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' "${AUTH_ARGS[@]}" \
    --data-binary '{"target":"singbox","platform":"amiga","links":["ss://YWVzLTEyOC1nY206Zml4dHVyZQ==@198.51.100.10:443#smoke"]}' "${BASE_URL}/api/short-links")
if [[ "${bad_platform}" != "400" ]]; then
    echo "shortlink-smoke-failed: invalid platform returned ${bad_platform}" >&2
    exit 1
fi

bad_target=$(curl -sS --max-time 10 -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' "${AUTH_ARGS[@]}" \
    --data-binary '{"target":"surge","links":["ss://YWVzLTEyOC1nY206Zml4dHVyZQ==@198.51.100.10:443#smoke"]}' "${BASE_URL}/api/short-links")
if [[ "${bad_target}" != "400" ]]; then
    echo "shortlink-smoke-failed: unsupported target returned ${bad_target}" >&2
    exit 1
fi

echo 'shortlink-smoke-ok'
