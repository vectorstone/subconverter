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

    # The tun `stack` field is deprecated in 1.15.0 and removed in 1.17.0, and the
    # `gvisor`/`mixed` values hard-fail at runtime on any client built without
    # `with_gvisor` (all official Apple clients reject them, while a locally
    # installed CLI usually has the tag and checks clean). Assert on the structure
    # instead of relying on the local kernel build.
    for inbound in doc.get("inbounds", []):
        if inbound.get("type") == "tun" and "stack" in inbound:
            failures.append(f"{name}: tun stack {inbound['stack']!r} must not be emitted")

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

    # A larger tun mtu only pays off when the whole path carries jumbo frames,
    # which a mobile link never does, while it invites PMTU blackholes on the
    # segments that clamp lower. One conventional value for every platform.
    for inbound in doc.get("inbounds", []):
        if inbound.get("type") == "tun" and inbound.get("mtu") != 1500:
            failures.append(f"{name}: tun mtu {inbound.get('mtu')!r} != 1500")

    # `cache_file.path` decides where the remote rule-set cache lands. It must be
    # pinned (otherwise it follows the process working directory, so a client
    # started from another directory re-downloads every rule set), and pinned to
    # a *relative* name outside the deployment that owns its data directory: the
    # kernel expands neither `~` nor `$HOME` and refuses to start when the parent
    # directory is missing ("FATAL start service: initialize cache-file"), so an
    # absolute path the target cannot guarantee is worse than no path at all.
    cache = doc.get("experimental", {}).get("cache_file")
    if cache is not None:
        path = cache.get("path")
        if not path:
            failures.append(f"{name}: cache_file without path leaves the rule-set cache to the process cwd")
        elif name == "openwrt":
            if not path.startswith("/"):
                failures.append(f"{name}: cache_file path {path!r} must be the deployment absolute path")
        elif path.startswith("/") or path.startswith("~") or "$" in path:
            failures.append(f"{name}: cache_file path {path!r} must be relative; the kernel does not expand it")

    rule_sets = {r.get("tag") for r in doc["route"].get("rule_set", [])}
    for rule in doc["route"]["rules"]:
        for ref in rule.get("rule_set", []) if isinstance(rule.get("rule_set"), list) else []:
            if ref not in rule_sets:
                failures.append(f"{name}: rule references undefined rule_set {ref}")

    # Routing is first-match, so a rule that repeats an earlier one verbatim is
    # unreachable: it cannot change a decision, but it does make the match[N]
    # indices in the client log point at rules that never fire. A rule that only
    # overlaps an earlier one must survive — it still owns what that rule misses.
    seen_rules = set()
    for index, rule in enumerate(doc["route"]["rules"]):
        key = json.dumps(rule, sort_keys=True)
        if key in seen_rules:
            failures.append(f"{name}: route rule {index} duplicates an earlier rule")
        seen_rules.add(key)

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
    if name == "ios":
        if "experimental" in doc:
            failures.append("ios: experimental must not be emitted")
        if doc["inbounds"][0].get("dns_mode") != "hijack":
            failures.append("ios: tun dns_mode hijack expected")
    if name == "android":
        if not ("experimental" in doc and "cache_file" in doc["experimental"]):
            failures.append("android: experimental.cache_file expected")
        if doc["inbounds"][0].get("dns_mode") != "hijack":
            failures.append("android: tun dns_mode hijack expected")
    if name == "android" and not doc["route"].get("override_android_vpn"):
        failures.append("android: override_android_vpn expected")
    if name == "ios" and doc["route"].get("override_android_vpn"):
        failures.append("ios: override_android_vpn must not be set")
    # Android *requires* auto_detect_interface: it is the only path that reaches
    # platformInterface.AutoDetectInterfaceControl -> VpnService.protect(), and
    # without it every proxy socket is routed back into the tun, so the device
    # gets a working tunnel and no usable traffic. `constant.IsLinux` covers
    # Android, so the kernel accepts the field. Desktop/OpenWrt already emit it
    # to avoid tun route loops.
    if name in ("android", "macos", "windows", "linux", "openwrt"):
        if not doc["route"].get("auto_detect_interface"):
            failures.append(f"{name}: route.auto_detect_interface must be enabled")
    if name in ("macos", "windows", "linux") and "auto_redirect" in text:
        failures.append(f"{name}: auto_redirect is Linux-only")

    # Remote rule sets must name an HTTP client explicitly: sing-box 1.14.0
    # deprecated the implicit default (removed in 1.16.0) and it dialled through
    # the default outbound, i.e. whatever route.final currently selects.
    if rule_types == {"remote"}:
        clients = [c.get("tag") for c in doc.get("http_clients", [])]
        default_client = doc["route"].get("default_http_client")
        if not clients:
            failures.append(f"{name}: remote rule_set without http_clients")
        if not default_client:
            failures.append(f"{name}: remote rule_set without route.default_http_client")
        elif default_client not in clients:
            failures.append(f"{name}: default_http_client {default_client!r} is not declared in http_clients")
        for entry in doc["route"].get("rule_set", []):
            if entry.get("type") == "remote" and entry.get("http_client") != default_client:
                failures.append(f"{name}: rule_set {entry.get('tag')!r} must pin http_client")

    # sing-box ANDs a process condition with every other condition of the same
    # rule (process/user items are hard-AND; domain and ip_cidr share one OR
    # group). Since one list file becomes ONE rule object here, emitting a process
    # condition the platform cannot satisfy silently disables the file's other
    # conditions rather than merely adding noise. Verified: a rule with an
    # unmatchable process_name plus a matching domain_keyword never fires.
    #   - Apple clients: process_name/process_path/user only work in the macOS
    #     standalone and jailbroken-iOS builds (App Store builds throw
    #     "Not implemented"), so iOS must not receive them.
    #   - Android: connection owner lookups yield package names, never a process
    #     path, so process_name can never match there either.
    # macOS/Windows/Linux (CLI) and OpenWrt keep them: those do resolve a path —
    # but only as their own rule object, so a client that cannot resolve one (the
    # sandboxed Apple clients) merely fails to match that rule instead of taking
    # the same file's domain and rule_set conditions down with it.
    process_fields = {"process_name", "process_path", "process_path_regex", "user", "user_id"}
    package_fields = {"package_name", "package_name_regex"}
    supports_process = name in ("macos", "windows", "linux", "openwrt")
    carries_process = False
    for rule in doc["route"]["rules"]:
        present_process = process_fields & set(rule)
        present_package = package_fields & set(rule)
        if present_process and not supports_process:
            failures.append(f"{name}: {sorted(present_process)} can never match on this platform "
                            f"and would disable the whole rule")
        if present_package and name != "android":
            failures.append(f"{name}: {sorted(present_package)} is Android-only and would disable the whole rule")
        carries_process = carries_process or bool(present_process)
        # A process condition sharing a rule with any other condition family is
        # the dangerous case, on every platform: drop the process item and the
        # file's domains and rule sets stop matching with it.
        others = set(rule) - process_fields - package_fields - {"action", "outbound", "rule_set"}
        if present_process and others:
            failures.append(f"{name}: rule {rule} mixes {sorted(present_process)} with {sorted(others)}; "
                            f"a process condition is hard-ANDed with the rest of its rule")
    if supports_process and not carries_process:
        # Without this the "never mixed" assertion above would pass on an empty set.
        failures.append(f"{name}: the fixture carries no process condition at all; the split assertion is vacuous")

    # skeleton mode: minimal groups, remapped rules, remote rule-set references
    expected_groups = {"proxy", "auto", "ChainProxyEntry", "ChainProxyExit"}
    if name in ("macos", "windows", "linux", "openwrt"):
        expected_groups.add("GLOBAL")
    groups = {o["tag"] for o in doc.get("outbounds", []) if o.get("type") in ("selector", "urltest")}
    if groups != expected_groups:
        failures.append(f"{name}: skeleton groups {sorted(groups)} != {sorted(expected_groups)}")
    if doc["route"].get("final") != "proxy":
        failures.append(f"{name}: route.final {doc['route'].get('final')!r} != 'proxy'")

    # `proxy` is the traffic-entry selector (route.final), so it must expose the
    # chain landings (Clash 节点选择 parity): otherwise selecting a landing in the
    # client can never engage the chain. `auto` / `ChainProxyEntry` must keep
    # excluding them or landing.detour -> entry group -> landing is a cycle.
    by_tag = {o.get("tag"): o for o in doc.get("outbounds", [])}
    # A selector without `default` silently takes its first member, so the member
    # order would be the real decision. `proxy` keeps `auto` first for
    # compatibility, but the generator must not invent a landing on its own: that
    # is a deployment decision handed in as an argument (asserted below).
    if by_tag.get("proxy", {}).get("default") is not None:
        failures.append(f"{name}: proxy must not carry a default unless one was requested")
    # GLOBAL backs the clash_api "Global" switch and its first member is DIRECT,
    # so an implicit default would make Global mode route everything direct.
    global_group = by_tag.get("GLOBAL")
    if global_group is not None and global_group.get("default") != "proxy":
        failures.append(f"{name}: GLOBAL must default to 'proxy', got {global_group.get('default')!r}")
    chain_exit = by_tag.get("ChainProxyExit")
    if not chain_exit:
        failures.append(f"{name}: ChainProxyExit missing for a chained subscription")
    else:
        landings = [t for t in chain_exit.get("outbounds", []) if t != "DIRECT"]
        if not landings:
            failures.append(f"{name}: ChainProxyExit exposes no landing node")
        proxy_members = by_tag.get("proxy", {}).get("outbounds", [])
        if not proxy_members or proxy_members[0] != "auto":
            failures.append(f"{name}: proxy must keep 'auto' as its default (first) member")
        for landing in landings:
            if landing not in proxy_members:
                failures.append(f"{name}: proxy does not list chain landing {landing!r}")
            for group in ("auto", "ChainProxyEntry"):
                if landing in by_tag.get(group, {}).get("outbounds", []):
                    failures.append(f"{name}: landing {landing!r} in {group} would be a chain cycle")

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

echo "== dual stack tun"
# The tun may carry an IPv4 and an IPv6 address at once, and a client resolves
# over whichever one it was handed: every tun address needs its own hijacked-DNS
# listener, or the IPv6 address falls outside the hijacked range.
for platform in android ios; do
    curl -s -o "$WORK/$platform-ipv6.json" \
        "http://127.0.0.1:$PORT/sub?target=singbox&singbox_platform=$platform&singbox_ipv6=1&url=http%3A%2F%2F127.0.0.1%3A$FIXTURE_PORT%2Fsubscription.yaml"
done
python3 - "$WORK" <<'PY'
import json, sys, pathlib

work = pathlib.Path(sys.argv[1])
failures = []

def tun_of(path):
    doc = json.loads(path.read_text())
    return next(i for i in doc["inbounds"] if i.get("type") == "tun")

for platform in ("android", "ios"):
    single = tun_of(work / f"{platform}.json")
    if len(single.get("dns_address", [])) != 1:
        failures.append(f"{platform}: IPv4-only tun must hijack DNS on a single address")

    dual = tun_of(work / f"{platform}-ipv6.json")
    addresses = dual.get("address", [])
    listeners = dual.get("dns_address", [])
    if not any(":" in a for a in addresses):
        failures.append(f"{platform}: singbox_ipv6=1 did not add an IPv6 tun address")
    if len(listeners) != len(addresses):
        failures.append(f"{platform}: {len(addresses)} tun addresses but {len(listeners)} DNS listeners")
    for address, listener in zip(addresses, listeners):
        if (":" in address) != (":" in listener):
            failures.append(f"{platform}: DNS listener {listener!r} does not match the family of {address!r}")

if failures:
    for line in failures:
        print("  FAIL " + line)
    sys.exit(1)
print("  ok: one hijacked-DNS listener per tun address family")
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
proxy_group = next(o for o in doc["outbounds"] if o["tag"] == "proxy")
assert proxy_group["outbounds"][0] == "auto", proxy_group
assert set(proxy_group["outbounds"]) == {"auto", "LAND-OK", "CYC-1", "CYC-2", "DANGLE", "LAND-DIAL", "FRONT-A"}, proxy_group
entry_group = next(o for o in doc["outbounds"] if o["tag"] == "ChainProxyEntry")
auto_group = next(o for o in doc["outbounds"] if o["tag"] == "auto")
for landing in ("LAND-OK", "CYC-1", "CYC-2", "DANGLE", "LAND-DIAL"):
    assert landing not in entry_group["outbounds"], entry_group
    assert landing not in auto_group["outbounds"], auto_group
print("  ok: valid chain kept, dialer magic resolved, cycles and dangling references dropped")
print("  ok: landings selectable from proxy, excluded from auto/ChainProxyEntry")
PY

echo "== traffic selector default and urltest narrowing"
gen_tuned() {
    curl -s "http://127.0.0.1:$PORT/sub?target=singbox&singbox_platform=macos&url=http%3A%2F%2F127.0.0.1%3A$FIXTURE_PORT%2Fsubscription.yaml$1"
}
gen_tuned "&singbox_default=VMess&singbox_auto_include=Trojan" >"$WORK/tuned.json"
gen_tuned "&singbox_default=NOPE&singbox_auto_include=ZZZ" >"$WORK/untuned.json"
python3 - "$WORK" <<'PY'
import json, sys, pathlib

work = pathlib.Path(sys.argv[1])
failures = []
base = json.loads((work / "macos.json").read_text())
tuned = json.loads((work / "tuned.json").read_text())
untuned = json.loads((work / "untuned.json").read_text())

def pick(doc, tag):
    return next(o for o in doc["outbounds"] if o.get("tag") == tag)

base_proxy = pick(base, "proxy")
tuned_proxy = pick(tuned, "proxy")
# The requested remark resolves to the node it names (substring, case-insensitive)...
if tuned_proxy.get("default") != "VMess-WS":
    failures.append(f"tuned: proxy default {tuned_proxy.get('default')!r} != 'VMess-WS'")
# ...without reordering or dropping members: `auto` stays first, so a client or a
# reader that ignores `default` sees exactly the previous group.
if base_proxy["outbounds"] != tuned_proxy["outbounds"]:
    failures.append("tuned: proxy membership changed")
if base_proxy["outbounds"][0] != "auto":
    failures.append("tuned: proxy must keep 'auto' as its first member")
# `auto` is the only health-checked group, so narrowing its member set is what
# removes the startup probe storm (and the every-5-minutes rescan).
if pick(tuned, "auto")["outbounds"] != ["Trojan-01"]:
    failures.append(f"tuned: auto members {pick(tuned, 'auto')['outbounds']} != ['Trojan-01']")
if pick(base, "auto")["outbounds"] == pick(tuned, "auto")["outbounds"]:
    failures.append("tuned: auto narrowing changed nothing; the assertion is vacuous")
# Selectors are deliberately not narrowed: ChainProxyEntry decides which nodes can
# serve as a chain entry.
if pick(tuned, "ChainProxyEntry")["outbounds"] != pick(base, "ChainProxyEntry")["outbounds"]:
    failures.append("tuned: ChainProxyEntry must not follow the auto keyword filter")
# Unresolvable values degrade to the previous behaviour instead of shipping a
# dangling reference: the kernel accepts one in `sing-box check` and then refuses
# to start ("default outbound not found").
if pick(untuned, "proxy").get("default") is not None:
    failures.append("untuned: an unresolvable singbox_default must not be emitted")
if pick(untuned, "auto")["outbounds"] != pick(base, "auto")["outbounds"]:
    failures.append("untuned: an unmatched singbox_auto_include must keep every member")
if pick(untuned, "GLOBAL").get("default") != "proxy":
    failures.append("untuned: the GLOBAL default must not depend on the new options")

if failures:
    for line in failures:
        print("  FAIL " + line)
    sys.exit(1)
PY
grep -q "singbox_default 'NOPE' is not a member" "$WORK/converter.log" \
    || fail "an unresolvable singbox_default did not warn"
grep -q "singbox_auto_include matched no node" "$WORK/converter.log" \
    || fail "an unmatched singbox_auto_include did not warn"
pass "selector default resolves against members; urltest member set narrows by keyword"

echo "== intranet DNS resolver and direct CIDR knobs"
# Documentation-only values: RFC 5737 / example.com. The real deployment hands
# in its own intranet resolver and company suffixes as request arguments.
curl -s -o "$WORK/intranet.json" \
    "http://127.0.0.1:$PORT/sub?target=singbox&singbox_platform=macos&singbox_dns_internal=192.0.2.53&singbox_internal_domains=example.com%2Cexample.org&singbox_direct_cidr=198.51.100.0%2F24%2C203.0.113.0%2F24&url=http%3A%2F%2F127.0.0.1%3A$FIXTURE_PORT%2Fsubscription.yaml"
if [[ -n "$SINGBOX_BIN" ]]; then
    "$SINGBOX_BIN" check -c "$WORK/intranet.json" >"$WORK/intranet.check.log" 2>&1 \
        || { cat "$WORK/intranet.check.log" >&2; fail "intranet: sing-box check failed"; }
    pass "intranet passed sing-box check"
fi
python3 - "$WORK" <<'PY'
import json, sys, pathlib

work = pathlib.Path(sys.argv[1])
failures = []
base = json.loads((work / "macos.json").read_text())
doc = json.loads((work / "intranet.json").read_text())

server = next((s for s in doc["dns"]["servers"] if s.get("tag") == "dns-internal"), None)
if server is None:
    failures.append("intranet: dns.servers has no dns-internal entry")
elif server.get("server") != "192.0.2.53" or server.get("type") != "udp":
    failures.append(f"intranet: dns-internal misconfigured: {server}")
elif "detour" in server:
    # A new-format server without detour dials directly; routing the intranet
    # resolver through route rules or the proxy group would break it.
    failures.append("intranet: dns-internal must dial directly (no detour)")

# The internal resolver rule must sit before the geosite rule: dns.rules are
# first-match and the company suffixes are inside geosite-cn.
dns_rules = doc["dns"]["rules"]
internal_rule = next((r for r in dns_rules if r.get("server") == "dns-internal"), None)
if internal_rule is None:
    failures.append("intranet: dns.rules has no dns-internal route")
elif set(internal_rule.get("domain_suffix", [])) != {"example.com", "example.org"}:
    failures.append(f"intranet: dns-internal suffixes wrong: {internal_rule}")
else:
    def dns_index(pred):
        return next((i for i, r in enumerate(dns_rules) if pred(r)), len(dns_rules))
    if dns_index(lambda r: r.get("server") == "dns-internal") > dns_index(lambda r: "rule_set" in r):
        failures.append("intranet: the dns-internal rule must precede the geosite-cn rule")

# The direct rules must sit after ip_is_private but before the clash-mode rules:
# in Global mode a later clash_mode rule would take the intranet traffic.
route_rules = doc["route"]["rules"]
def route_index(pred):
    return next((i for i, r in enumerate(route_rules) if pred(r)), len(route_rules))
private_idx = route_index(lambda r: r.get("ip_is_private"))
domain_idx = route_index(lambda r: "example.com" in r.get("domain_suffix", []))
cidr_idx = route_index(lambda r: "198.51.100.0/24" in r.get("ip_cidr", []))
global_idx = route_index(lambda r: r.get("clash_mode") == "Global")
for name, idx in (("domain_suffix direct", domain_idx), ("ip_cidr direct", cidr_idx)):
    if idx >= len(route_rules):
        failures.append(f"intranet: the {name} rule is missing")
    elif not (private_idx < idx < global_idx):
        failures.append(f"intranet: the {name} rule (index {idx}) must follow ip_is_private "
                        f"({private_idx}) and precede clash_mode Global ({global_idx})")

# Without the arguments nothing of this may be emitted.
if any(s.get("tag") == "dns-internal" for s in base["dns"]["servers"]):
    failures.append("base: dns-internal must not be emitted without singbox_dns_internal")
if any("198.51.100.0/24" in r.get("ip_cidr", []) for r in base["route"]["rules"]):
    failures.append("base: the requested CIDR must not be emitted without singbox_direct_cidr")

if failures:
    for line in failures:
        print("  FAIL " + line)
    sys.exit(1)
PY
pass "intranet resolver routes before geosite-cn; direct rules sit inside the private..clash window"

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
