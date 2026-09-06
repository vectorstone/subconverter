#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:25500}"
API_KEY="${API_KEY:?API_KEY is required}"

payload='{"name":"smoke-test","target":"clash","expires_in":3600,"links":["ss://YWVzLTEyOC1nY206Zml4dHVyZQ==@198.51.100.10:443#smoke"]}'
response=$(curl -fsS --max-time 30 -X POST -H 'Content-Type: application/json' -H "X-API-Key: ${API_KEY}" --data-binary "${payload}" "${BASE_URL}/api/short-links")
id=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])' <<<"${response}")
url=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["short_url"])' <<<"${response}")

curl -fsS --max-time 20 "${url}" | grep -q 'smoke'
curl -fsS --max-time 10 -H "X-API-Key: ${API_KEY}" "${BASE_URL}/api/short-links" | grep -q "${id}"
curl -fsS --max-time 10 -X DELETE -H "X-API-Key: ${API_KEY}" "${BASE_URL}/api/short-links/${id}" | grep -q 'revoked'
if curl -sS --max-time 10 -o /dev/null -w '%{http_code}' "${url}" | grep -q '^410$'; then
    echo 'shortlink-smoke-ok'
else
    echo 'shortlink-smoke-failed' >&2
    exit 1
fi
