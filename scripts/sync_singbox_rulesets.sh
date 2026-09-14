#!/usr/bin/env bash
#
# Push the binary rule sets (.srs) referenced by a generated sing-box
# configuration onto a router.
#
# OpenWrt targets reference rule sets as local files
# (e.g. /opt/open-box/data/rulesets/geosite-cn.srs). `sing-box check` opens
# those files, so a missing or zero byte file makes the whole configuration
# fail to load. This helper downloads whatever is missing from the SagerNet
# rule-set releases and copies it over.
#
# Usage:
#   scripts/sync_singbox_rulesets.sh <config.json> [user@host] [remote-dir]
#
# Environment:
#   RULESET_CACHE     local download cache (default: ${TMPDIR:-/tmp}/singbox-rulesets)
#   RULESET_BASE_URL  override for private mirrors (default: SagerNet raw URLs)
#   DRY_RUN=1         report what would be transferred without touching the host
#
set -euo pipefail

CONFIG="${1:?usage: sync_singbox_rulesets.sh <config.json> [user@host] [remote-dir]}"
TARGET="${2:-}"
REMOTE_DIR="${3:-/opt/open-box/data/rulesets}"
CACHE="${RULESET_CACHE:-${TMPDIR:-/tmp}/singbox-rulesets}"
GEOSITE_BASE="https://raw.githubusercontent.com/SagerNet/sing-geosite/rule-set"
GEOIP_BASE="https://raw.githubusercontent.com/SagerNet/sing-geoip/rule-set"

[[ -f "$CONFIG" ]] || { echo "config not found: $CONFIG" >&2; exit 1; }
mkdir -p "$CACHE"

mapfile -t ENTRIES < <(python3 - "$CONFIG" <<'PY'
import json
import sys

doc = json.load(open(sys.argv[1]))
for entry in doc.get("route", {}).get("rule_set", []):
    if entry.get("type") != "local":
        continue
    path = entry.get("path", "")
    tag = entry.get("tag", "")
    if tag and path:
        print(f"{tag}\t{path}")
PY
)

if [[ ${#ENTRIES[@]} -eq 0 ]]; then
    echo "no local rule sets referenced by $CONFIG"
    exit 0
fi

remote_size() {
    [[ -z "$TARGET" ]] && { echo "-1"; return; }
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$TARGET" "wc -c < '$1' 2>/dev/null || echo -1" | tr -d '[:space:]'
}

for entry in "${ENTRIES[@]}"; do
    tag="${entry%%$'\t'*}"
    path="${entry#*$'\t'}"
    size=$(remote_size "$path")
    if [[ "$size" != "-1" && "$size" -gt 0 ]]; then
        echo "ok      $tag ($size bytes on device)"
        continue
    fi

    case "$tag" in
        geoip-*) url="$GEOIP_BASE/$tag.srs" ;;
        *)       url="$GEOSITE_BASE/$tag.srs" ;;
    esac
    if [[ -n "${RULESET_BASE_URL:-}" ]]; then
        url="${RULESET_BASE_URL%/}/$tag.srs"
    fi

    local_file="$CACHE/$tag.srs"
    if [[ ! -s "$local_file" ]]; then
        echo "fetch   $tag <- $url"
        if ! curl -fsS --max-time 120 -o "$local_file.part" "$url"; then
            rm -f "$local_file.part"
            echo "error   unable to download $url" >&2
            exit 1
        fi
        # Some mirrors answer 200 with an empty body; a zero byte .srs breaks
        # the kernel at startup, so treat it as a failure.
        if [[ ! -s "$local_file.part" ]]; then
            rm -f "$local_file.part"
            echo "error   downloaded $tag is empty" >&2
            exit 1
        fi
        mv "$local_file.part" "$local_file"
    fi

    if [[ "${DRY_RUN:-0}" == "1" ]]; then
        echo "would push $local_file -> ${TARGET:-<local>}:$path"
        continue
    fi
    if [[ -z "$TARGET" ]]; then
        echo "cached  $local_file (no target given)"
        continue
    fi

    echo "push    $tag -> $TARGET:$path"
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$TARGET" "mkdir -p '$(dirname "$path")'"
    # The router images usually ship without sftp-server, so stream the file.
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$TARGET" "cat > '$path'" < "$local_file"

    size=$(remote_size "$path")
    if [[ "$size" == "-1" || "$size" -le 0 ]]; then
        echo "error   $path is missing or empty after transfer" >&2
        exit 1
    fi
    echo "ok      $tag ($size bytes on device)"
done

echo "rule sets are in place"
