#!/usr/bin/env bash
#
# sing-box generation smoke test.
#
# Generates one configuration per platform from a local fixture subscription and
# validates the result:
#   1. `sing-box check` for the platforms whose privileged fields the local
#      kernel accepts (macos / windows / linux);
#   2. structural assertions that do not depend on the kernel build
#      (no removed 1.10~1.14 fields, required fields present, chain handling).
#
# Android / iOS / OpenWrt configurations carry platform-only fields
# (`override_android_vpn`, `auto_redirect`, ...) which the local kernel refuses
# to decode, so they are validated by structure here and must be checked on the
# target device (see docs/sing-box-conversion-plan.md, M6).
#
# Usage: tests/singbox_golden.sh [path-to-sing-box]

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SINGBOX_BIN="${1:-$(command -v sing-box || true)}"
BIN="${SINGBOX_TEST_BINARY:-$ROOT/build/subconverter}"
PORT="${SINGBOX_TEST_PORT:-25519}"
FIXTURE_PORT="${SINGBOX_TEST_FIXTURE_PORT:-18899}"
WORK="$(mktemp -d)"
FIXTURES="$ROOT/tests/singbox/fixtures"

cleanup() {
    [[ -n "${CONVERTER_PID:-}" ]] && kill "$CONVERTER_PID" 2>/dev/null || true
    [[ -n "${FIXTURE_PID:-}" ]] && kill "$FIXTURE_PID" 2>/dev/null || true
    [[ -n "${PREF:-}" ]] && rm -f "$PREF" "$PREF.bak"
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "  ok: $*"; }

[[ -x "$BIN" ]] || fail "build/subconverter not found; build the project first"

# The binary chdir()s into its own directory, so relative preference paths resolve
# against build/. Preference snippets and bundled rules resolve the same way the
# production image resolves them against /base.
BIN_DIR="$(cd "$(dirname "$BIN")" && pwd)"
[[ -e "$BIN_DIR/base" ]] || ln -s "$ROOT/base" "$BIN_DIR/base"
[[ -e "$BIN_DIR/snippets" ]] || ln -s "$ROOT/base/snippets" "$BIN_DIR/snippets"
[[ -e "$BIN_DIR/rules" ]] || ln -s "$ROOT/base/rules" "$BIN_DIR/rules"
# the binary chdir()s into the preference file's directory, so the preference must
# live next to the linked snippets/ and rules/ for imports to resolve
cp "$ROOT/base/pref.example.toml" "$BIN_DIR/pref.golden.toml"
PREF="$BIN_DIR/pref.golden.toml"
# the api port is read from the preference file, not from an environment variable
sed -i.bak "s/^port = 25500$/port = $PORT/" "$PREF" && rm -f "$PREF.bak"

if [[ -n "$SINGBOX_BIN" && -x "$SINGBOX_BIN" ]]; then
    echo "using sing-box binary: $SINGBOX_BIN ($("$SINGBOX_BIN" version 2>/dev/null | head -1))"
else
    echo "WARNING: sing-box binary not found; kernel validation will be skipped"
    SINGBOX_BIN=""
fi

# Start both helpers without an extra subshell so the recorded PIDs can be killed
# reliably in the exit trap.
pushd "$FIXTURES" >/dev/null
python3 -m http.server "$FIXTURE_PORT" >/dev/null 2>&1 &
FIXTURE_PID=$!
popd >/dev/null
pushd "$BIN_DIR" >/dev/null
"./subconverter" -f "$PREF" >"$WORK/converter.log" 2>&1 &
CONVERTER_PID=$!
popd >/dev/null

for _ in $(seq 1 40); do
    curl -sf "http://127.0.0.1:$PORT/version" >/dev/null 2>&1 && break
    sleep 0.25
done
curl -sf "http://127.0.0.1:$PORT/version" >/dev/null || fail "converter did not start"

echo "== generating per platform"
PLATFORMS="macos windows linux android ios openwrt"
for platform in $PLATFORMS; do
    out="$WORK/$platform.json"
    code=$(curl -s -o "$out" -w '%{http_code}' \
        "http://127.0.0.1:$PORT/sub?target=singbox&singbox_platform=$platform&url=http%3A%2F%2F127.0.0.1%3A$FIXTURE_PORT%2Fsubscription.yaml")
    [[ "$code" == "200" ]] || fail "$platform: HTTP $code"
    python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$out" || fail "$platform: output is not valid JSON"
    pass "$platform generated ($(wc -c <"$out" | tr -d ' ') bytes)"
done

echo "== kernel validation (local)"
for platform in macos windows linux; do
    if [[ -n "$SINGBOX_BIN" ]]; then
        if ! "$SINGBOX_BIN" check -c "$WORK/$platform.json" >"$WORK/$platform.check.log" 2>&1; then
            cat "$WORK/$platform.check.log" >&2
            fail "$platform: sing-box check failed"
        fi
        pass "$platform passed sing-box check"
    fi
done

echo "== structural assertions"
python3 - "$WORK" <<'PY'
import json, sys, pathlib

work = pathlib.Path(sys.argv[1])
removed = [
    "address_resolver", "inet4_address", "inet6_address", "independent_cache",
    "sniff_override_destination", "store_fakeip", "download_detour",
]
failures = []

def check_platform(name):
    doc = json.loads((work / f"{name}.json").read_text())
    text = json.dumps(doc)

    for field in removed:
        if f'"{field}"' in text:
            failures.append(f"{name}: removed field {field} present")
    if '"type": "dns"' in text or '"type": "block", "tag": "REJECT"' in text and name == "android":
        pass
    if any(o.get("type") == "dns" for o in doc.get("outbounds", [])):
        failures.append(f"{name}: legacy dns outbound present")
    if any(o.get("type") == "shadowsocksr" for o in doc.get("outbounds", [])):
        failures.append(f"{name}: shadowsocksr outbound present")
    if any(o.get("type") == "wireguard" for o in doc.get("outbounds", [])):
        failures.append(f"{name}: wireguard must be an endpoint")

    servers = doc["dns"]["servers"]
    if len(servers) >= 2 and not doc["route"].get("default_domain_resolver"):
        failures.append(f"{name}: multiple dns servers without default_domain_resolver")
    resolver = doc["route"].get("default_domain_resolver")
    if resolver and resolver not in {s.get("tag") for s in servers}:
        failures.append(f"{name}: default_domain_resolver points at an unknown server")

    tags = {o.get("tag") for o in doc.get("outbounds", [])}
    tags |= {e.get("tag") for e in doc.get("endpoints", [])}
    final = doc["route"].get("final")
    if not final or final not in tags:
        failures.append(f"{name}: route.final {final!r} is not an outbound tag")
    for outbound in doc.get("outbounds", []):
        detour = outbound.get("detour")
        if not detour:
            continue
        if outbound.get("type") in ("selector", "urltest"):
            failures.append(f"{name}: group {outbound['tag']} carries a detour")
        if detour not in tags:
            failures.append(f"{name}: {outbound['tag']} detours to unknown tag {detour}")

    rule_sets = {r.get("tag") for r in doc["route"].get("rule_set", [])}
    for rule in doc["route"]["rules"]:
        for ref in rule.get("rule_set", []) if isinstance(rule.get("rule_set"), list) else []:
            if ref not in rule_sets:
                failures.append(f"{name}: rule references undefined rule_set {ref}")

    rule_types = {r.get("type") for r in doc["route"].get("rule_set", [])}
    if name == "openwrt":
        if rule_types - {"local"}:
            failures.append("openwrt: rule_set must be local")
        if not doc["inbounds"][0].get("auto_redirect"):
            failures.append("openwrt: auto_redirect is required")
        if doc["route"]["rules"][1].get("action") != "hijack-dns" or "inbound" not in doc["route"]["rules"][1]:
            failures.append("openwrt: DNS hijack must be scoped to the dns-in inbound")
        if not any(r.get("ip_cidr") and r.get("action") == "reject" for r in doc["route"]["rules"]):
            failures.append("openwrt: missing tun network reject rule")
        if "experimental" in doc and "clash_api" in doc["experimental"]:
            if doc["experimental"]["clash_api"].get("external_controller") != "127.0.0.1:9095":
                failures.append("openwrt: clash_api must bind 127.0.0.1:9095")
    if name in ("android", "ios"):
        if "experimental" in doc:
            failures.append(f"{name}: experimental must not be emitted")
        if doc["inbounds"][0].get("dns_mode") != "hijack":
            failures.append(f"{name}: tun dns_mode hijack expected")
    if name == "android" and not doc["route"].get("override_android_vpn"):
        failures.append("android: override_android_vpn expected")
    if name == "ios" and doc["route"].get("override_android_vpn"):
        failures.append("ios: override_android_vpn must not be set")
    if name in ("macos", "windows", "linux") and "auto_redirect" in text:
        failures.append(f"{name}: auto_redirect is Linux-only")

    # skeleton mode: minimal groups, remapped rules, remote rule-set references
    expected_groups = {"proxy", "auto", "ChainProxyEntry", "ChainProxyExit"}
    if name in ("macos", "windows", "linux", "openwrt"):
        expected_groups.add("GLOBAL")
    groups = {o["tag"] for o in doc.get("outbounds", []) if o.get("type") in ("selector", "urltest")}
    if groups != expected_groups:
        failures.append(f"{name}: skeleton groups {sorted(groups)} != {sorted(expected_groups)}")
    if doc["route"].get("final") != "proxy":
        failures.append(f"{name}: route.final {doc['route'].get('final')!r} != 'proxy'")
    valid_targets = expected_groups | {"DIRECT"}
    for rule in doc["route"]["rules"]:
        if rule.get("action") == "route" and rule.get("outbound") not in valid_targets:
            failures.append(f"{name}: rule references non-skeleton outbound {rule.get('outbound')!r}")
    if name != "openwrt":
        if rule_types and rule_types != {"remote"}:
            failures.append(f"{name}: rule_set must be remote, got {sorted(rule_types)}")
    inline_domains = sum(len(rule.get("domain", []) + rule.get("domain_suffix", []) + rule.get("domain_keyword", []))
                         for rule in doc["route"]["rules"])
    if inline_domains > 200:
        failures.append(f"{name}: {inline_domains} inline domains, rule-set mapping leaked")

for platform in ("macos", "windows", "linux", "android", "ios", "openwrt"):
    check_platform(platform)

if failures:
    for line in failures:
        print("  FAIL " + line)
    sys.exit(1)
print("  ok: all structural assertions passed")
PY

echo "== chained proxy handling"
gen() {
    curl -s "http://127.0.0.1:$PORT/sub?target=singbox&singbox_platform=macos&$1url=http%3A%2F%2F127.0.0.1%3A$FIXTURE_PORT%2Fchain.yaml"
}
gen "" >"$WORK/chain.json"
python3 - "$WORK/chain.json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
detours = {o["tag"]: o["detour"] for o in doc["outbounds"] if "detour" in o}
assert detours == {"LAND-OK": "FRONT-A", "LAND-DIAL": "ChainProxyEntry"}, detours
for tag in ("CYC-1", "CYC-2", "DANGLE"):
    assert tag not in detours, tag
groups = {o["tag"] for o in doc["outbounds"] if o.get("type") in ("selector", "urltest")}
assert {"proxy", "auto", "ChainProxyEntry", "ChainProxyExit", "GLOBAL"} <= groups, groups
exit_group = next(o for o in doc["outbounds"] if o["tag"] == "ChainProxyExit")
assert set(exit_group["outbounds"]) == {"DIRECT", "LAND-OK", "CYC-1", "CYC-2", "DANGLE", "LAND-DIAL"}, exit_group
print("  ok: valid chain kept, dialer magic resolved, cycles and dangling references dropped")
PY

code=$(curl -s -o "$WORK/strict.txt" -w '%{http_code}' \
    "http://127.0.0.1:$PORT/sub?target=singbox&singbox_platform=macos&singbox_chain_strict=1&url=http%3A%2F%2F127.0.0.1%3A$FIXTURE_PORT%2Fchain.yaml")
[[ "$code" == "400" ]] || fail "strict chain mode returned HTTP $code"
grep -q "cycle" "$WORK/strict.txt" || fail "strict chain mode did not report the cycle"
grep -q "unresolved-via" "$WORK/strict.txt" || fail "strict chain mode did not report the dangling reference"
pass "strict chain mode returns 400 with reasons"

code=$(curl -s -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$PORT/sub?target=singbox&singbox_platform=amiga&url=http%3A%2F%2F127.0.0.1%3A$FIXTURE_PORT%2Fsubscription.yaml")
[[ "$code" == "400" ]] || fail "invalid platform returned HTTP $code"
pass "invalid platform rejected with 400"

echo
echo "all sing-box smoke tests passed"
