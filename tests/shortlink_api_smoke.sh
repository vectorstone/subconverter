#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:25500}"
API_KEY="${API_KEY:?API_KEY is required}"
ASSERT_LITE_OUTPUT="${ASSERT_LITE_OUTPUT:-0}"
LITE_MAX_SNAPSHOT_BYTES="${LITE_MAX_SNAPSHOT_BYTES:-262144}"

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
response=$(curl -fsS --max-time 30 -X POST -H 'Content-Type: application/json' -H "X-API-Key: ${API_KEY}" --data-binary "${payload}" "${BASE_URL}/api/short-links")
id=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])' <<<"${response}")
url=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["short_url"])' <<<"${response}")

snapshot=$(curl -fsS --max-time 20 "${url}")
grep -q 'smoke' <<<"${snapshot}"
if [[ "${ASSERT_LITE_OUTPUT}" == "1" ]]; then
    assert_lite_snapshot "${snapshot}"
fi
list_response=$(curl -fsS --max-time 10 -H "X-API-Key: ${API_KEY}" "${BASE_URL}/api/short-links")
download_url=$(python3 -c 'import json,sys; data=json.load(sys.stdin); print(next(item["download_url"] for item in data["items"] if item["id"] == sys.argv[1]))' "${id}" <<<"${list_response}")
download_headers=$(curl -fsS --max-time 20 -D - -o /dev/null "${download_url}")
grep -Eiq 'Content-Disposition: attachment; filename="custom-clash-[0-9]{6}(-[0-9]+)?\.yaml"' <<<"${download_headers}"
refresh_response=$(curl -fsS --max-time 30 -X POST -H "X-API-Key: ${API_KEY}" "${BASE_URL}/api/short-links/${id}/refresh")
grep -q 'refreshed' <<<"${refresh_response}"
refreshed_snapshot=$(curl -fsS --max-time 20 "${url}")
grep -q 'smoke' <<<"${refreshed_snapshot}"
if [[ "${ASSERT_LITE_OUTPUT}" == "1" ]]; then
    assert_lite_snapshot "${refreshed_snapshot}"
fi
revoke_response=$(curl -fsS --max-time 10 -X DELETE -H "X-API-Key: ${API_KEY}" "${BASE_URL}/api/short-links/${id}")
grep -q 'revoked' <<<"${revoke_response}"
status_code=$(curl -sS --max-time 10 -o /dev/null -w '%{http_code}' "${url}")
if [[ "${status_code}" == "410" ]]; then
    echo 'shortlink-smoke-ok'
else
    echo 'shortlink-smoke-failed' >&2
    exit 1
fi
