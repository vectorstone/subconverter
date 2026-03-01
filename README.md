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
  - [Advanced Usage](#advanced-usage)
  - [Auto Upload](#auto-upload)

## What's New

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

2. You can add `&remark=` to Telegram-liked HTTP/Socks 5 links to set a remark for this node. For example:

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
