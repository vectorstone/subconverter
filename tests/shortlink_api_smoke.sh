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

# ------------------------------------------------ large sing-box regression
# The Clash Lite size limit must not be applied to sing-box snapshots. A
# sing-box snapshot is generated from the local preference rulesets rather than
# from the Lite profile, so it legitimately exceeds the Lite limit once the
# subscription is realistically sized. This case used to fail with
# 413 "generated Lite configuration exceeds the configured size limit".
#
# The default of 100 links matches the service default SHORTLINK_MAX_LINKS; the
# node tags are deliberately long because node names are repeated in every
# proxy group, so realistic subscriptions cross the limit well below the cap.
LARGE_SINGBOX_NODES="${LARGE_SINGBOX_NODES:-100}"
large_payload_file=$(mktemp)
large_response_file=$(mktemp)
python3 - "${LARGE_SINGBOX_NODES}" >"${large_payload_file}" <<'PY_LARGE_LINKS'
import json
import sys

count = int(sys.argv[1])
links = []
for index in range(count):
    tag = "Large-Size-Probe-Node-%03d-with-a-deliberately-long-descriptive-tag-name" % index
    links.append(
        "vless://11111111-2222-3333-4444-555555555555@198.51.100.%d:443"
        "?encryption=none&flow=xtls-rprx-vision&security=reality&sni=www.example.org"
        "&fp=chrome&pbk=tTjmwAXdjAZYA0561YDYXBxotYbCxDSwezm1kBUbaQs&sid=abcd1234"
        "&type=tcp#%s" % ((index % 250) + 1, tag)
    )
print(json.dumps({
    "name": "smoke-singbox-large",
    "target": "singbox",
    "platform": "android",
    "expires_in": 3600,
    "links": links,
}))
PY_LARGE_LINKS

large_code=$(curl -sS --max-time 90 -o "${large_response_file}" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' "${AUTH_ARGS[@]}" \
    --data-binary "@${large_payload_file}" "${BASE_URL}/api/short-links")
if [[ "${large_code}" != "201" ]]; then
    echo "shortlink-smoke-failed: large sing-box link returned ${large_code}: $(cat "${large_response_file}")" >&2
    exit 1
fi
large_id=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])' <"${large_response_file}")
large_url=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["short_url"])' <"${large_response_file}")
large_snapshot_file=$(mktemp)
curl -fsS --max-time 30 -o "${large_snapshot_file}" "${large_url}"
python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "${large_snapshot_file}"
large_size=$(wc -c <"${large_snapshot_file}")
if (( large_size <= LITE_MAX_SNAPSHOT_BYTES )); then
    echo "shortlink-smoke-warning: ${LARGE_SINGBOX_NODES}-node sing-box snapshot is ${large_size} bytes, not above the Lite limit ${LITE_MAX_SNAPSHOT_BYTES}; this deployment cannot exercise the regression" >&2
else
    echo "  ok: ${LARGE_SINGBOX_NODES}-node sing-box snapshot accepted (${large_size} bytes > Lite limit ${LITE_MAX_SNAPSHOT_BYTES})"
fi
curl -fsS --max-time 30 -X POST --data-binary "" "${AUTH_ARGS[@]}" "${BASE_URL}/api/short-links/${large_id}/refresh" | grep -q refreshed
curl -fsS --max-time 10 -X DELETE "${AUTH_ARGS[@]}" "${BASE_URL}/api/short-links/${large_id}" | grep -q revoked
rm -f "${large_payload_file}" "${large_response_file}" "${large_snapshot_file}"

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
