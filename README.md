# subconverter

Utility to convert between various proxy subscription formats.

[![Build Status](https://github.com/tindy2013/subconverter/actions/workflows/build.yml/badge.svg)](https://github.com/tindy2013/subconverter/actions)
[![GitHub tag (latest SemVer)](https://img.shields.io/github/tag/tindy2013/subconverter.svg)](https://github.com/tindy2013/subconverter/tags)
[![GitHub release](https://img.shields.io/github/release/tindy2013/subconverter.svg)](https://github.com/tindy2013/subconverter/releases)
[![GitHub license](https://img.shields.io/github/license/tindy2013/subconverter.svg)](https://github.com/tindy2013/subconverter/blob/master/LICENSE)

[Docker README](https://github.com/tindy2013/subconverter/blob/master/README-docker.md)

[中文文档](https://github.com/tindy2013/subconverter/blob/master/README-cn.md)

- [subconverter](#subconverter)
  - [What's New](#whats-new)
  - [Supported Types](#supported-types)
  - [Quick Usage](#quick-usage)
    - [Access Interface](#access-interface)
    - [Description](#description)
    - [Dialer Node Parameter (SS Links)](#dialer-node-parameter-ss-links)
  - [Web UI Short Links (PostgreSQL)](#web-ui-short-links-postgresql)
  - [Advanced Usage](#advanced-usage)
  - [Auto Upload](#auto-upload)

## What's New

2026/04/12

- Added `tuic://` share-link parsing support for the scoped TUIC link family used by mihomo/OpenClash clients.
- Added TUIC export support for `clash` and `singbox`.
- Unsupported targets now skip TUIC nodes while continuing to export the remaining nodes.
- OpenClash compatibility for TUIC is documented against `clash_meta / mihomo` cores only.

2026/03/01

- Added VLESS and Reality parsing support for `vless://` links.
- Added VLESS export support for `clash`, `surge`, `quanx`, `singbox`, `vless` and `mixed` targets.
- Updated `mixed` target output to include VLESS links.
- Release commit: `f3e4e21`.
- Docker image published: `a823002162/subconvert:f3e4e21` and `a823002162/subconvert:latest`.

## Supported Types

| Type         | As Source  | As Target    | Target Name |
| ------------ | :--------: | :----------: | ----------- |
| Clash        |     ✓      |      ✓       | clash       |
| ClashR       |     ✓      |      ✓       | clashr      |
| Quantumult   |     ✓      |      ✓       | quan        |
| Quantumult X |     ✓      |      ✓       | quanx       |
| Loon         |     ✓      |      ✓       | loon        |
| SS (SIP002)  |     ✓      |      ✓       | ss          |
| SS Android   |     ✓      |      ✓       | sssub       |
| SSD          |     ✓      |      ✓       | ssd         |
| SSR          |     ✓      |      ✓       | ssr         |
| Surfboard    |     ✓      |      ✓       | surfboard   |
| Surge 2      |     ✓      |      ✓       | surge&ver=2 |
| Surge 3      |     ✓      |      ✓       | surge&ver=3 |
| Surge 4      |     ✓      |      ✓       | surge&ver=4 |
| V2Ray        |     ✓      |      ✓       | v2ray       |
| VLESS        |     ✓      |      ✓       | vless       |
| Telegram-liked HTTP/Socks 5 links |     ✓      |      ×       | Only as source |

Notice:

1. Shadowrocket users should use `ss`, `ssr`, `v2ray` or `vless` as target.
2. `tuic://` links are accepted as source nodes and can currently be exported to `clash` and `singbox`. There is no standalone `tuic` target.
3. OpenClash TUIC compatibility is scoped to `clash_meta / mihomo` cores only.
4. You can add `&remark=` to Telegram-liked HTTP/Socks 5 links to set a remark for this node. For example:

   - tg://http?server=1.2.3.4&port=233&user=user&pass=pass&remark=Example

   - https://t.me/http?server=1.2.3.4&port=233&user=user&pass=pass&remark=Example


---

## Quick Usage

> Using default groups and rulesets configuration directly, without changing any settings

### Access Interface

```txt
http://127.0.0.1:25500/sub?target=%TARGET%&url=%URL%&config=%CONFIG%
```

### Description

| Argument | Required | Example | Description |
| -------- | :------: | :------ | ----------- |
| target   | Yes      | clash   | Target subscription type. Acquire from Target Name in [Supported Types](#supported-types). |
| url      | Yes      | https%3A%2F%2Fwww.xxx.com | Subscription to convert. Supports URLs and file paths. Process with [URLEncode](https://www.urlencoder.org/) first. |
| config   | No       | https%3A%2F%2Fwww.xxx.com | External configuration file path. Supports URLs and file paths. Process with [URLEncode](https://www.urlencoder.org/) first. More examples can be found in [this](https://github.com/lzdnico/subconverteriniexample) repository. |

For `target=clash`, when `&config=` is omitted, subconverter now falls back to the built-in default chain config at runtime path `config/default_clash_chainproxy.ini` (source repo path `base/config/default_clash_chainproxy.ini`). If `default_external_config` is already set in the server preferences, that configured value still takes precedence over the built-in Clash fallback. If you want to tune the built-in default Clash groups/rules later, edit that file. An explicit `&config=` still overrides the built-in default.

The default Clash config references `base/rules/custom_proxy.list` and `base/rules/custom_direct.list` (runtime paths `rules/custom_proxy.list` and `rules/custom_direct.list`). Maintain frequently changed domain exceptions in those lists; rules are emitted in declaration order, so keep proxy exceptions before broad direct rules. Rules are cached by default, so restart the service or call `/refreshrules?token=YOUR_TOKEN` after editing; set `update_ruleset_on_request=true` to reload them on each request.

If you need to merge two or more subscription, you should join them with '|' before the URLEncode process.

Example:

```txt
You have 2 subscriptions and you want to merge them and generate a Clash subscription:
1. https://dler.cloud/subscribe/ABCDE?clash=vmess
2. https://rich.cloud/subscribe/ABCDE?clash=vmess

First use '|' to separate 2 subscriptions:
https://dler.cloud/subscribe/ABCDE?clash=vmess|https://rich.cloud/subscribe/ABCDE?clash=vmess

Then process it with URLEncode to get %URL%:
https%3A%2F%2Fdler.cloud%2Fsubscribe%2FABCDE%3Fclash%3Dvmess%7Chttps%3A%2F%2Frich.cloud%2Fsubscribe%2FABCDE%3Fclash%3Dvmess

Then fill %TARGET% and %URL% in Access Interface with actual values:
http://127.0.0.1:25500/sub?target=clash&url=https%3A%2F%2Fdler.cloud%2Fsubscribe%2FABCDE%3Fclash%3Dvmess%7Chttps%3A%2F%2Frich.cloud%2Fsubscribe%2FABCDE%3Fclash%3Dvmess

Finally subscribe this link in Clash and you are done!
```

### Dialer Node Parameter (SS Links)

To mark a Shadowsocks node as a dialer node, append `x-sc-underlying-proxy=dialer` in the query part (before `#`).

Examples:

```txt
ss://YWVzLTEyOC1nY206VGVzdFBhc3N3b3Jk@198.51.100.10:443/?x-sc-underlying-proxy=dialer#example-dialer

ss://YWVzLTEyOC1nY206VGVzdFBhc3N3b3Jk@198.51.100.10:443/?plugin=obfs-local%3Bobfs%3Dhttp%3Bobfs-host%3Dexample.com&x-sc-underlying-proxy=dialer#example-dialer
```

For backward compatibility, `underlying-proxy=dialer` is still supported.

The built-in default Clash chain config keeps `♻️ 自动选择` as a `url-test` group and exposes `ChainProxyEntry` as a manual `select` group so you can either choose a real node directly or switch to the auto-tested group. The external `nodnsleak.ini` template in the companion `clashConfig` repo now mirrors the same layout.

## Web UI Short Links (PostgreSQL)

With `SHORTLINK_ENABLED=true`, `DATABASE_URL`, and `SHORTLINK_ENCRYPTION_KEY` configured, the service provides a Cloudflare Access-protected Web UI and PostgreSQL-backed short-link API. `GET /s/<code>` remains public and returns Clash YAML directly; create, list, refresh, and revoke require Cloudflare Access or a user API key.

Short links use the Lite Clash conversion profile by default: `SHORTLINK_CLASH_CONFIG=config/default_clash_lite.ini` and `SHORTLINK_CLASH_EXPAND=false`. The service fixes the target to `clash`, disables inserts, and does not accept an arbitrary per-request `config` from Web UI/API users. This keeps stored snapshots small by emitting remote rule providers instead of embedding the complete rule contents. `SHORTLINK_LITE_MAX_OUTPUT_BYTES` defaults to 262144 and rejects an unexpectedly large Lite snapshot before it is stored.

The variables are also the configuration rollback switch. To temporarily restore the older expanded chain profile for newly created or refreshed links, set `SHORTLINK_CLASH_CONFIG=config/default_clash_chainproxy.ini` and `SHORTLINK_CLASH_EXPAND=true`, then restart the service. Restore the Lite values and restart to roll forward again. Existing snapshots are immutable on read: older snapshots continue to download unchanged, while an explicit `POST /api/short-links/<id>/refresh` recalculates that link from its encrypted original sources using the settings active at refresh time. Refreshing A never changes a dependent short link B.

For a local authenticated smoke check, run:

```bash
API_KEY='your-user-api-key' BASE_URL='http://127.0.0.1:25500' ASSERT_LITE_OUTPUT=1 \
  bash tests/shortlink_api_smoke.sh
```

`ASSERT_LITE_OUTPUT=1` additionally requires a bounded snapshot (default 256 KiB) containing `rule-providers` and `RULE-SET` references. Set `LITE_MAX_SNAPSHOT_BYTES` to adjust the test bound for a custom Lite template.

---

## Advanced Usage

Please refer to [中文文档](https://github.com/tindy2013/subconverter/blob/master/README-cn.md#%E8%BF%9B%E9%98%B6%E7%94%A8%E6%B3%95).

## Auto Upload

> Upload Gist automatically

Add a [Personal Access Token](https://github.com/settings/tokens/new) into [gistconf.ini](./gistconf.ini) in the root directory, then add `&upload=true` to the local subscription link, then when you access this link, the program will automatically update the content to Gist repository.

Example:

```ini
[common]
;uncomment the following line and enter your token to enable upload function
token = xxxxxxxxxxxxxxxxxxxxxxxx(Your Personal Access Token)
```
