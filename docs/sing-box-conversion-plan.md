# sing-box 转换能力迭代设计方案（v1 · 待评审）

> 状态：**方案评审中，未开始编码**。
> 基线：仓库 `27cc484`（fork master），sing-box 目标版本 **1.14.0**（本机实测二进制 `sing-box 1.14.0`）。
> 配套材料：`docs/sing-box-notes-digest.md`（本机 Obsidian sing-box 专题 11+ 篇的结构化摘要，已脱敏）。

---

## 0. 结论摘要与已确认决策

| # | 决策点 | 结论 |
|---|---|---|
| D1 | OpenWrt/ImmortalWrt 产物形态 | **只出完整配置**（含 tun / DNS / route / rule_set / experimental），可与 Open-Box 内核共存的独立配置 |
| D2 | 平台参数 | `target=singbox&singbox_platform=<os>`，并接受 `platform` 别名；不带参数时用 pref 默认平台 |
| D3 | 规则集策略 | 桌面/移动用 `rule_set` **remote `.srs` + `cache_file`**；OpenWrt 用 **local `.srs` 绝对路径** |
| D4 | 链式代理来源 | 自动识别 `dialer-proxy` / `underlying-proxy` / `x-sc-underlying-proxy` → `detour`，**生成期做依赖图校验** |
| D5 | Web 短链门户 | 页面加「目标 / 平台」下拉，短链持久化 `target`+`platform`，新增 DB migration |
| D6 | 部署节奏 | 先在 test 环境验证，通过后再推 prod |
| D7 | 移动端客户端 | 官方 sing-box for Android + sing-box for Apple（SFI/SFM），按 1.14 字段生成 |
| D8 | 默认平台 | **`macos`**（`singbox_default_platform = "macos"`） |
| D9 | OpenWrt 参数取值 | **直接复用 Open-Box 现网值**：tun `172.19.0.1/30`、`clash_api` `127.0.0.1:9095`、`cache_file` `/opt/open-box/data/cache.db`、`rule_set` `/opt/open-box/data/rulesets/*.srs` |
| D10 | OpenWrt 落地方式 | **路径 B：临时替换 `/opt/open-box/etc/config.json`**（面板下次部署会覆盖，属预期） |
| D11 | `.srs` 同步 | 部署脚本把缺失的 `.srs` 推到 `/opt/open-box/data/rulesets` |
| D12 | 链式降级 | 默认 **丢弃 + WARN**（`singbox_chain_strict=1` 时才 400） |
| D13 | 移动端 `experimental` | Android/iOS **不输出** `clash_api` / `cache_file` |

一句话方案：**把 sing-box 从「模板里的一段 pre-1.12 静态 JSON」重写为「按平台参数化的 1.14 生成器」**，补齐链式代理、DNS/路由/规则集/experimental 的代码化生成，同步修掉 5 个确定性缺陷，并把平台维度打通到短链门户与部署链路。

---

## 1. 目标与范围

PRD 四条 → 拆解为可验收项：

| PRD | 验收标准 |
|---|---|
| ① 按最新 sing-box 代码/docs 迭代 sing-box 转换 | 生成物在 **1.14.0 `sing-box check` 与 `run` 双通过**；不再输出任何 1.10~1.14 已删除字段 |
| ② 支持 sing-box，且区分 openwrt/macos/linux/windows/android/ios | `singbox_platform` 6 值；每平台产物在对应内核/客户端上可用；短链页面可选 |
| ③ 支持链式代理 | 从订阅链路数据自动生成 `detour`；生成期检出并处理悬空引用与成环 |
| ④ 部署到 test 与 prod | test 环境端到端验证通过；prod 灰度并保留回滚路径 |

**不在本轮范围**：sing-box 入站服务端（`services`）、`v2ray_api`、TUN 之外的内核级透明代理（redirect/tproxy）、Hiddify/Karing 等第三方客户端兼容矩阵。

---

## 2. 现状基线（实测，非推断）

### 2.1 当前输出实测

用本仓库当前源码构建后实测：`/sub?target=singbox` 对一份 6 节点的测试订阅产出 **183 KB** 单文件配置：

- 顶层：`log / dns / ntp / inbounds / outbounds / route / experimental`
- `outbounds` 22 个：`direct`、**`block`**、**`dns`**、2×shadowsocks、vmess、vless、trojan、13×selector、1×urltest
- 分组映射正确（`select`→`selector`，`url-test`→`urltest`），但因测试订阅里 `LAND-SS` 写了 `dialer-proxy`，输出中 **`detour` 出现 0 次** → 链式信息被静默丢弃
- `inbounds`：`mixed` + `tun`，tun 使用 **`inet4_address`**（1.10 已改名）、入站 **`sniff`**（1.13 已移除）
- `dns`：`servers[].address` 旧格式（含 `tls://`/`h3://`/`rcode://success`）+ `address_resolver` + 顶层 `fakeip` + `independent_cache` —— **1.12 弃用、1.14 全部移除**
- `route.rules`：`geosite`/`geoip` 内联键 + `outbound: "dns-out"` —— **geosite/geoip 1.12 已移除、dns 出站 1.13 已移除**

### 2.2 在当前内核上直接失败（实测）

```
$ sing-box check -c out-singbox.json            # 本机 1.14.0
FATAL decode config: dns: legacy DNS fakeip options are deprecated in sing-box
      1.12.0 and removed in sing-box 1.14.0

$ /opt/open-box/bin/sing-box check -c /tmp/sc-legacy.json   # 网关内核 1.14.0-openbox-tcp1
FATAL decode config at /tmp/sc-legacy.json: dns: legacy DNS fakeip options are
      deprecated in sing-box 1.12.0 and removed in sing-box 1.14.0
```

即：**当前 sing-box 输出在 1.14 上无法启动**。C++ 生成器本身停留在 ≤1.11；`base/base/all_base.tpl:300-424` 停留在 ~1.10 世代。

### 2.3 确定性缺陷（与 PRD 无关也必须修）

| # | 缺陷 | 位置 | 后果 |
|---|---|---|---|
| B1 | `shadowsocksr` 出站类型 1.6.0 已移除 | `subexport.cpp:2708-2718` | 现代内核 `unknown outbound type` |
| B2 | AnyTLS 出站写成入站 schema：`users:[{username:"sekai",...}]`，且缺 required `password` | `subexport.cpp:2867-2877` | `unknown field "users"` + 缺必填字段 |
| B3 | Hysteria2 的 `up_mbps/down_mbps` 永不输出（判据用 `x.Up`，但 hysteria2 解析只写 `UpSpeed`） | `subexport.cpp:2833-2836` vs `subparser.cpp:286-287` | 带宽参数静默丢失 |
| B4 | WireGuard 走 `outbounds[].type="wireguard"` | `subexport.cpp:2766-2801` | 1.13 起移除，必须迁到顶层 `endpoints[]` |
| B5 | `urltest` 的 `"interval": ""` / `"url": ""` 无 0 值守卫；`route.final` 可能为空串 | `subexport.cpp:2979-2985`、`ruleconvert.cpp:599-605` | 空 duration / 空 tag，语义错误 |
| B6 | 规则值统一 `toLower()`，破坏 `process_name`/`process_path`/`package_name`/`domain_regex`；第三列 option（`no-resolve`）静默丢弃 | `ruleconvert.cpp:479/513/480-481/506-507` | 规则语义丢失 |
| B7 | `dns` 出站 + `route.rules[].outbound:"dns-out"`（`block` 出站一并替换） | `subexport.cpp:2673/2675`、`ruleconvert.cpp:535` | `dns` 出站 1.13 起硬失败；`block` 出站 1.14 实测仍能加载但**无语义**，故一并改为 `action:"reject"` |

### 2.4 平台 / 链式 / DNS / rule_set 全部为零

- 全仓库无任何 sing-box 平台参数或模板分支（`target=singbox` 只有 4 处别名定义，无 query 参数）
- `detour` / `multiplex` / `rule_set` / `endpoints` / `default_domain_resolver` 在 `src/` 内 **0 命中**
- 链式数据 `Proxy::UnderlyingProxy`（`proxy.h:111`）已在解析阶段就绪，Clash 侧实现完整，**只有 sing-box 侧没用**

---

## 3. 版本与兼容基线

### 3.1 唯一目标：1.14.0 typed 语法

- 官方最新稳定版 **1.14.0**（2026-08-31）；上一稳定线 1.13.21。
- 本机 `/opt/homebrew/bin/sing-box` = **1.14.0**，网关 `/opt/open-box/bin/sing-box` = **1.14.0-openbox-tcp1**（官方 1.14.0 + 两个补丁的兼容构建，配置兼容性与官方一致）。
- 移动端官方客户端内核同样按 1.14 生成。
- **不输出兼容旧版的分支**：不提供 `singbox_version` 参数；旧内核用户请升级（这是 fork 自有部署，没有历史包袱）。

### 3.2 生成器禁止输出字段（黑名单）

| 禁止输出 | 替代 |
|---|---|
| `dns.servers[].address` / 字符串数组形式 | `{type, server}` 对象形式 |
| `dns.fakeip`（顶层对象） | `{type:"fakeip", inet4_range, inet6_range}` |
| `dns.independent_cache` | 直接删除（1.14 弃用，计划 1.16 移除） |
| `inbounds[].sniff` / `domain_strategy` / `sniff_timeout` | `route.rules[0]={"action":"sniff"}` |
| `inbounds[].inet4_address` / `inet6_address` / `*_route_address` | `address` / `route_address` |
| `outbounds[].type:"dns"` | `route.rules[].action:"hijack-dns"` |
| `outbounds[].type:"block"` | `route.rules[].action:"reject"` |
| `outbounds[].type:"shadowsocksr"` | 跳过该节点并记日志 |
| `outbounds[].type:"wireguard"` | 顶层 `endpoints[]` |
| `route.rules[].geosite` / `geoip` / 顶层 `geosite`/`geoip` | `rule_set` |
| `rule_set[].download_detour` | `http_client` |
| `dns.servers[].address_resolver` | `domain_resolver` |
| `outbound` 上的 `override_address`/`override_port` | `action:"route-options"` |
| `experimental.cache_file.store_rdrc` / `store_mode` / `store_selected` | `store_dns`；`store_*` 仅在 cache_file 下 |
| `clash_api.cache_file` 等（1.14 unknown field） | `experimental.cache_file` |

### 3.3 生成期必须自建的三类校验（`check` 查不出）

实测（本机 1.14.0）：

| 用例 | `check` | `run` |
|---|---|---|
| `land.detour = "DOES-NOT-EXIST"` | **通过** | FATAL `dependency[...] not found` |
| 成环（`FRONT` 组含 `LAND`，`LAND.detour=FRONT`） | **通过** | FATAL `circular outbound dependency` |
| `selector` 上写 `detour` | 失败 `unknown field "detour"` | — |
| `route.final` 指向不存在的 tag | 通过 | FATAL `default outbound not found` |
| route rule 的 `outbound` 指向不存在的 tag | 通过 | 仅命中该连接时报错 |
| `dns.servers` ≥2 且缺 `route.default_domain_resolver` | 通过 | **FATAL**（需环境变量逃生舱） |
| `detour` 指向「只有 type+tag 的空 `direct`」 | 通过 | 首次拨号 FATAL `detour to an empty direct outbound makes no sense` |

→ 生成器必须实现：**tag 引用完整性 + detour 依赖图无环 + `dns.servers`≥2 ⇒ 必写 `route.default_domain_resolver` + 禁止 detour 指向空 direct**。

---

## 4. 输出契约设计

### 4.1 参数

```
/sub?target=singbox&singbox_platform=<platform>     # 主参数
/sub?target=singbox&platform=<platform>             # 别名
```

| platform | 别名 |
|---|---|
| `openwrt` | `immortalwrt`、`router` |
| `macos` | `mac`、`darwin` |
| `windows` | `win` |
| `linux` | — |
| `android` | — |
| `ios` | `apple`、`iphone` |

- 非法值 → HTTP 400 + 明确错误文本（列出合法值）。
- 不带参数 → `singbox_default_platform`（新增 pref 键，**建议默认 `openwrt`**，见 §12）。
- 既有 `list=true` / `nodelist` 语义保持不变（只出 `{"outbounds":[...]}`）。
- 新增 `singbox_chain_strict=1`：链式校验失败时返回 400 而不是降级丢弃。

### 4.2 平台差异矩阵（生成器决策表）

| 维度 | macOS | Windows | Linux 桌面 | Android | iOS | OpenWrt |
|---|---|---|---|---|---|---|
| `inbounds` | tun + mixed(127.0.0.1:2080) | tun + mixed | tun + mixed | **仅 tun** | **仅 tun** | tun（+ 可选 dns-in） |
| `route.auto_detect_interface` | ✅ | ✅ | ✅ | ❌ 不支持 | ❌ 不支持 | ✅（必开） |
| `strict_route` | ✅ | ✅ | ✅ | ❌ 未实现 | ❌ 未实现 | ✅ |
| `auto_redirect` | ❌ 写了直接 check 失败 | ❌ | 仅可选（root+nft） | ❌ | ❌ | ✅（默认开） |
| `dns_mode`/`dns_address`（tun 级） | 可选 | 可选 | 可选 | ✅ hijack + tun 对端地址（`172.19.0.1/30` 的对端） | ✅ 同 Android | **不写**（dnsmasq 接管；劫持规则限定 `inbound:["dns-in"]`） |
| `tun.platform.http_proxy` | 可选 | — | — | — | 可选（SFI/SFM） | — |
| `route.override_android_vpn` | — | — | — | ✅ | — | — |
| 进程/用户规则 | ✅ | ✅ | ✅ | ❌（用 `package_name`，GUI 可覆盖） | ❌ | ✅ |
| `clash_api` | ✅ 127.0.0.1:9095 | ✅ | ✅ | 由 App 管理 → 不输出 | 由 App 管理 → 不输出 | ✅ 127.0.0.1:9095 |
| `experimental.cache_file` | ✅ 相对路径 | ✅ | ✅ | 不输出 | 不输出 | ✅ `/opt/open-box/data/cache.db` |
| `rule_set` | remote `.srs` + `update_interval` | remote | remote | remote | remote | **local `.srs` 绝对路径** |
| `log` | `{level:"warn",timestamp:true}` | 同 | 同 | `{level:"warn"}` | 同 | **`{level:"warn"}`，无 output（走 syslog）** |
| `mtu` | 9000 | 9000 | 9000 | 8500 | 8500 | 9000 |
| `stack` | `mixed` | `mixed` | `mixed` | `mixed` | `system`（移动端 gvisor 支持不确定，取保守值） | `mixed` |

> `stack` 在 1.14 完全可用（1.15 起才废弃、计划 1.17 移除），因此现在照常输出。

### 4.3 通用骨架（平台无关部分）

```jsonc
{
  "$schema": "https://sing-box.sagernet.org/schema.json",
  "log": { "level": "warn", "timestamp": true },
  "dns": {
    "servers": [
      { "type": "udp",   "tag": "dns-direct", "server": "223.5.5.5" },
      { "type": "https", "tag": "dns-proxy",  "server": "1.1.1.1",
        "detour": "PROXY", "domain_resolver": "dns-direct" }
    ],
    "rules": [
      { "rule_set": "geosite-cn", "action": "route", "server": "dns-direct" },
      { "action": "route", "server": "dns-proxy" }
    ],
    "final": "dns-proxy",
    "strategy": "prefer_ipv4"
  },
  "outbounds": [ /* 节点 + 组 + direct，见 §4.9 / §4.6 */ ],
  "route": {
    "auto_detect_interface": true,
    "default_domain_resolver": "dns-direct",
    "rule_set": [ /* §4.8 */ ],
    "rules": [
      { "action": "sniff" },
      { "protocol": "dns", "action": "hijack-dns" },
      { "ip_is_private": true, "action": "route", "outbound": "direct" },
      { "clash_mode": "Direct", "action": "route", "outbound": "direct" },
      { "rule_set": ["geosite-cn"], "action": "route", "outbound": "direct" },
      { "clash_mode": "Global", "action": "route", "outbound": "PROXY" }
    ],
    "final": "PROXY"
  },
  "experimental": {
    "cache_file": { "enabled": true, "path": "cache.db" },
    "clash_api": { "external_controller": "127.0.0.1:9095", "secret": "<生成或占位>", "default_mode": "Rule" }
  }
}
```

要点：
- DNS 双通道：直连 `udp`（纯 IP）+ 代理 `https`（纯 IP + `detour:PROXY` + `domain_resolver`）。
- `dns.rules` 用 `action:"route"` 显式写法；`dns` 段永远保留一条**无匹配条件的兜底规则** + `final`。
- route 顺序：`sniff` → `hijack-dns` → 回环 reject（仅 OpenWrt/auto_redirect）→ `ip_is_private` → 广告 reject → 自定义 → clash_mode → 国内直连 → 兜底。
- 域名规则排在 IP 规则之前；`reject` 排在 `ip_is_private` 之后。
- `route.rules[].outbound` 在 1.14 仍可用，但统一输出 `action:"route"` + `outbound` 的显式形式（两种形式实测均通过 check）。

### 4.4 逐平台 delta（相对 §4.3）

**macOS**
```jsonc
"inbounds": [
  { "type":"tun","tag":"tun-in","address":["172.19.0.1/30","fdfe:dcba:9876::1/126"],
    "mtu":9000,"stack":"mixed","auto_route":true,"strict_route":true },
  { "type":"mixed","tag":"mixed-in","listen":"127.0.0.1","listen_port":2080 }
]
```
（**不得出现 `auto_redirect` / `exclude_interface` / `interface_name`**。）

**Windows**：同 macOS 骨架；`strict_route` 语义为防多宿主 DNS 泄漏；可选 `set_system_proxy`。

**Linux 桌面**：同 macOS；`auto_redirect` 默认关闭，可通过模板参数打开（仅在 root + nftables 环境有效）。

**Android**
```jsonc
"inbounds": [
  { "type":"tun","tag":"tun-in","address":["172.19.0.1/30","fdfe:dcba:9876::1/126"],
    "mtu":8500,"stack":"mixed","auto_route":true,
    "dns_mode":"hijack","dns_address":["<TUN_PEER_IP>"] }   // tun 网段 +1
],
"route": { "default_domain_resolver":"dns-direct", "override_android_vpn": true, "final":"PROXY" }
```
（**无 `auto_detect_interface`、无 `strict_route`、无进程/包规则、无 `route_address_set`、无 `experimental`**。）

**iOS**
```jsonc
"inbounds": [
  { "type":"tun","tag":"tun-in","address":["172.19.0.1/30","fdfe:dcba:9876::1/126"],
    "mtu":8500,"stack":"system","auto_route":true,
    "dns_mode":"hijack","dns_address":["<TUN_PEER_IP>"] }   // tun 网段 +1
]
```
（可选 `platform.http_proxy`；其余同 Android 的「不输出」清单。）

**OpenWrt（ImmortalWrt / 替换 Open-Box 配置）**

现网实测前提（已在网关确认）：网关当前是 **dnsmasq 接管模式** ——
`dhcp.@dnsmasq[0].noresolv='1'` + `server='127.0.0.1#7853'`。因此 OpenWrt 产物必须与现网结构一致，**不能换成全局 DNS 劫持**（会形成 dns-in 自环、整片 LAN 解析超时）。

```jsonc
"log": { "level": "warn" },
"inbounds": [
  { "type":"tun","tag":"tun-in","address":["172.19.0.1/30"],
    "auto_route":true,"strict_route":true,"stack":"mixed","auto_redirect":true },
  { "type":"direct","tag":"dns-in","listen":"127.0.0.1","listen_port":7853 }
],
"dns": { "strategy": "ipv4_only" },
"route": {
  "rules": [
    { "action":"sniff" },
    { "inbound":["dns-in"],"action":"hijack-dns" },   // dnsmasq 模式：必须限定 inbound，禁止全局 protocol:dns
    { "ip_cidr":["172.19.0.1/30"],"action":"reject" },// 防 TCP DNS 自环，必须在 ip_is_private 之前
    { "ip_is_private":true,"action":"route","outbound":"direct" }
    /* …其余同 §4.3… */
  ]
},
"experimental": {
  "clash_api": { "external_controller": "127.0.0.1:9095", "secret": "<singbox_clash_api_secret，默认空>" },
  "cache_file": { "enabled": true, "path": "/opt/open-box/data/cache.db" }
}
```

OpenWrt 专属硬约束（来自网关现网实测 + Open-Box 引擎源码）：
1. **不发 redirect/tproxy 入站**：Open-Box 全代码无此二者；只发 tproxy 而无配套 nft/ip rule 等于没有流量进来。auto_redirect 与 tproxy 互斥。
2. **DNS 模式跟随现网 dnsmasq 接管**：路由规则用 `{"inbound":["dns-in"],"action":"hijack-dns"}`，**不要**用全局 `{"protocol":"dns","action":"hijack-dns"}`（tun 里转发到 dns-in 的查询会被劫持回 dns-in 形成自环）。`dns-in` 必须保留 `127.0.0.1:7853`（dnsmasq 的上游指向它）。
3. tun 网段 → `action:"reject"` 必须排在 `ip_is_private` **之前**（真机踩过 TCP DNS 自环，几十秒吃光句柄）。
4. `experimental.clash_api.external_controller` 绑回环 9095（与现网一致）；`secret` 由 pref/env 提供，**转换器不得内置固定 secret**；不配 secret 时只监听回环。
5. `cache_file.path` 用现网的 `/opt/open-box/data/cache.db`（可写、持久、在 flash；写 `/tmp` 会让每次重启丢掉 selector 选择）。
6. `log` 不写 `output`（procd 收 stdout 进 syslog），避免刷坏闪存。
7. `rule_set` 只引用设备上**确实存在**的 `.srs`（`check` 会真的打开文件，缺失即 FATAL）；缺失的由部署脚本先补齐再 `check`。
8. 纯 tun 模式（关 auto_redirect）还需防火墙放行 tun 的 forward **与 input** 链，否则 TCP 被秒回 RST —— 作为部署文档条目，不在配置内表达。

### 4.5 OpenWrt 完整配置的落地方式（D10）

产物直接替换 **`/opt/open-box/etc/config.json`**：

1. 备份现网 `config.json`（用真实时间戳命名，保留最近 3 份）；
2. 先补齐缺失的 `.srs` 到 `/opt/open-box/data/rulesets/`（D11），**再**跑 `check`（`check` 会真的打开文件）；
3. `check` 通过后 `procd` 重启 `openbox` 服务；
4. 端到端验证：`logread -e sing-box` 出现 `sing-box started`、LAN 客户端解析正常、出口 IP 变化；
5. 回滚：把备份文件拷回并重启服务。

已知代价（用户已知悉并接受）：**面板下次部署/升级会重写 `config.json`**，替换生效期只到下次面板操作为止。

### 4.6 链式代理（detour）设计

**语义**：`A.detour = B` ⇒ A 到自己服务器的连接由 B 承载 ⇒ 链路 `本机 → B → A的服务器 → 目标`。因此**落地节点写 `detour: 前置`**。

**数据来源**（全部已存在，无需改解析器）：
- `Proxy::UnderlyingProxy`（`proxy.h:111`）
- Clash YAML：`underlying-proxy` → 回落 `dialer-proxy`（`subparser.cpp:1236-1240`）
- SS 分享链接：`x-sc-underlying-proxy` / `underlying-proxy` / `underlying_proxy`（`subparser.cpp:596-600`）

**tag 解析（三级匹配）**：
1. 与最终节点 tag（经 emoji/重命名后）精确匹配
2. 与最终分组 tag 精确匹配（如 `ChainProxyEntry`、`♻️ 自动选择`）
3. 与节点**原始 remark** 匹配后映射到最终 tag
4. 都不中 → `missing-via`，丢弃该链路并记日志

**生成期校验（新增 `SingBoxChainResolver`）**：

| 拒绝原因 | 判据 |
|---|---|
| `missing-landing` | 落地不在最终 tag 集合 |
| `landing-is-group` | 落地是 selector/urltest（组不能写 detour） |
| `missing-via` | 前置 tag 不存在 |
| `empty-direct` | 前置是「只有 type+tag 的 `direct`」（运行期 FATAL） |
| `cycle` | 依赖图（组 → 成员 + 节点 → detour）上 `via` 可达 `landing` |
| `self-loop` | `landing == via` |

默认行为：**丢弃 + WARN 日志**；`singbox_chain_strict=1` 时返回 400，响应体列出每条被拒链路与原因。

**与分组的关系**：`PROXY` 组通常包含全部节点，若落地 `detour` 指向 `PROXY`（或指向包含自己的组）即构成环 —— 依赖图把「组 → 成员」也作为边，因此能被 `cycle` 检出。这与 Open-Box `chain.mjs` 的判定集合一致。

### 4.7 DNS 设计

- 双通道：`dns-direct`（`udp`，纯 IP，无 detour）+ `dns-proxy`（`https`，纯 IP，`detour:"PROXY"`，`domain_resolver:"dns-direct"`）。
- `dns.rules`：国内规则集 → `dns-direct`；其余显式兜底 → `dns-proxy`；`dns.final = dns-proxy`。
- `route.default_domain_resolver = "dns-direct"`（**必写**，见 §3.3）。
- `strategy`：桌面/移动 `prefer_ipv4`；OpenWrt `ipv4_only`（与现网一致，防 IPv6 泄漏）。
- **不使用 fakeip**（多设备透明代理场景收益低、副作用大）。
- 不使用 `type:"tcp"` 作为代理侧解析器（网关内核的 TCP DNS 回归只在自编译 `openbox-tcpN` 上被补丁修过；`https` 无此问题）。

### 4.8 规则与 rule_set

- `GEOSITE,<name>` → `rule_set:["geosite-<name>"]`；`GEOIP,<name>` → `rule_set:["geoip-<name>"]`；同时为被引用的 tag 生成 `route.rule_set` 条目。
- 桌面/移动：`{"type":"remote","tag":...,"format":"binary","url":<模板>,"update_interval":"1d"}` + `experimental.cache_file.enabled=true`。
  - URL 模板（pref 可配）：`https://raw.githubusercontent.com/SagerNet/sing-geosite/rule-set/geosite-<n>.srs`、`.../sing-geoip/rule-set/geoip-<n>.srs`
- OpenWrt：`{"type":"local","tag":...,"format":"binary","path":"<singbox_ruleset_dir>/<tag>.srs"}`，默认 `<singbox_ruleset_dir> = /opt/open-box/data/rulesets`（改 pref 可覆盖）。
- 其余内联规则（`DOMAIN-SUFFIX`/`IP-CIDR`/`PROCESS-NAME`…）继续内联，但**保留原始大小写**，且 `action` 显式化。
- `[]FINAL` → `route.final`；`no-resolve` 在 sing-box 无对应语义 → 记 DEBUG 日志后忽略。
- 规则顺序严格保持订阅中的相对顺序（DNS 兜底规则除外）。

### 4.9 出站字段规范（含缺陷修复）

| 协议 | 输出字段 | 本轮修复 |
|---|---|---|
| shadowsocks | type/tag/server/server_port/method/password/plugin/plugin_opts | — |
| shadowsocksr | **跳过 + WARN** | B1 |
| vmess | uuid/alter_id/security/transport/tls + `packet_encoding`（可选） | — |
| vless | uuid/flow/packet_encoding/transport/tls(+utls+reality) | 非 reality 也输出 `utls.fingerprint` |
| trojan / tuic / hysteria2 / anytls | 各自字段 + tls | **AnyTLS 改为 `password`（字符串）** B2；**hysteria2 `up_mbps/down_mbps` 修复取值** B3 |
| hysteria2 | 不再同时输出 `server_port` 与 `server_ports` | B5 类 |
| http / socks | 各自字段 | `http` **不输出 `network`** |
| wireguard | **迁到顶层 `endpoints[]`**（`address`/`private_key`/`peers[].address`/`peers[].port`） | B4 |
| selector | tag/outbounds/default/interrupt_exist_connections | — |
| urltest | tag/outbounds/url/interval/tolerance（**空值守卫**） | B5 |
| 通用 | `tcp_fast_open`、`domain_resolver`、`detour` | — |

Clash `fallback`/`load-balance` 组：sing-box 无对应类型，统一降级为 `urltest` 并记 WARN。

**字段名映射速查**（实现时最容易写错的几个；左边是订阅侧常见名字，右边是 v1.14.0 的真实路径）：

| 订阅侧 | sing-box 真实路径 |
|---|---|
| `sni` / `servername` | `tls.server_name`（**`sni` 这个键不存在**） |
| `skip-cert-verify` / `allowInsecure` | `tls.insecure` |
| `alpn` | `tls.alpn` |
| `client-fingerprint` / `fp` | `tls.utls.fingerprint`（**reality 必带 utls**） |
| `pbk` / `sid` | `tls.reality.public_key` / `tls.reality.short_id` |
| `ws-opts.path` / `ws-opts.headers` | `transport.type:"ws"` + `transport.path` + `transport.headers` |
| `grpc-opts.grpc-service-name` | `transport.service_name` |
| Clash 风格 `network: ws` | **错**：sing-box 的 `transport` 才是传输层，`network` 只表示 `tcp`/`udp` |
| `mux` | `multiplex{}` |
| `underlying-proxy` / `dialer-proxy` | `detour`（指向**上游**出站） |
| WireGuard 节点 | 顶层 `endpoints[]` |

---

## 5. 代码落地方案

> **实现进度（本轮已落地并验证）**
> - M1 ~ M4 已完成：`src/generator/config/singbox.{h,cpp}` 新增平台 profile、1.14 骨架生成、rule_set 生成、链式依赖图校验与结构自检；`subexport.cpp` 的 sing-box 分支重写（含 B1~B7 修复）；`ruleconvert.cpp` 规则改写（geosite/geoip → rule_set、保留大小写、action 语法、FINAL 处理）。
> - 验证：`tests/singbox_golden.sh` 全绿 —— macos/windows/linux 在本机 `sing-box 1.14.0` 通过 `check`；openwrt 产物在网关内核 `1.14.0-openbox-tcp1` 通过 `check`；android/ios 因平台专属字段无法在本机解码，改用结构断言校验。
> - 新增 URL 参数：`singbox_platform`（别名 `platform`）、`singbox_ipv6`、`singbox_dns_direct`、`singbox_dns_proxy`、`singbox_dns_ruleset`、`singbox_dns_split`、`singbox_ruleset_source=local|remote`、`singbox_ruleset_dir`、`singbox_cache_path`、`singbox_clash_api`、`singbox_clash_api_secret`、`singbox_chain_strict`。
> - 新增 pref 键：`singbox_default_platform`（默认 `macos`）、`singbox_chain_strict`（默认 `false`）。
> - 模板路径保留：仅当外部配置显式提供 `singbox_rule_base` 时才回退到 `render_template`，否则一律走 C++ 生成（`ext.singbox_generated`）。



### 5.1 新增模块（推荐把 sing-box 生成从 `subexport.cpp` 里拆出来）

```
src/generator/config/singbox/
    singbox_platform.h/.cpp     # 平台枚举、别名解析、SingBoxPlatformProfile
    singbox_builder.h/.cpp      # 顶层骨架：log/dns/inbounds/route/experimental/$schema
    singbox_outbound.h/.cpp     # 单节点 → outbound / endpoint（从 proxyToSingBox 抽出）
    singbox_chain.h/.cpp        # UnderlyingProxy → detour + 依赖图校验
    singbox_ruleset.h/.cpp      # rule_set 条目与 remote/local 策略
    singbox_validate.h/.cpp     # tag 引用完整性、dns.servers≥2 断言、空值断言
```

理由：现有 `proxyToSingBox` 已 340+ 行且模板/代码职责混杂；平台矩阵 + 依赖图校验 + rule_set 动态构造三者都需要独立可测单元。

### 5.2 逐文件改动点

| 文件 | 改动 |
|---|---|
| `src/handler/interfaces.cpp:391-428` | 解析 `singbox_platform`/`platform`/`singbox_chain_strict`；非法值 400 |
| `src/handler/interfaces.cpp:446,555` | `lSingBoxBase` 逻辑改为「平台骨架是否走模板覆盖」 |
| `src/handler/interfaces.cpp:1061-1076` | `singbox` 分支改调新生成器（保留 `ext.nodelist` 与 `config=` 模板路径） |
| `src/handler/settings.h:50,58,76-101` | 新增 `singBoxDefaultPlatform`、`singBoxRulesetDir`、`singBoxRulesetSource`、`singBoxChainStrict`、`singBoxClashApiPort` 等 |
| `src/handler/settings.cpp:334/381/619/869/903/1111/1186/1288` | 上述键的 TOML/YAML/INI 读写 + external config |
| `src/generator/config/subexport.h:18-59,74` | `extra_settings` 增平台字段；`proxyToSingBox` 签名扩展 |
| `src/generator/config/subexport.cpp:2560-3034` | 抽出/替换 sing-box 生成实现；修 B1~B5 |
| `src/generator/config/ruleconvert.cpp:18/473-606` | 规则重写（geosite/geoip→rule_set、action 语法、大小写、final 空值） |
| `src/generator/config/ruleconvert.h:36` | `rulesetToSingBox` 签名扩展（接收平台与 rule_set 收集器） |
| `base/base/all_base.tpl:300-424` | legacy sing-box 段改为「模板覆盖模式」的 1.14 骨架，或标注 deprecated |
| `base/pref.toml` / `pref.example.{toml,ini,yml}` | 新增键与默认值 |
| `src/handler/shortlink_api.cpp:355,424` | target 白名单 `clash|singbox`；新增 platform 字段；快照与刷新保持 |
| `src/storage/postgres_store.cpp:119-135` + `db/migrations/003_*.sql` | `ALTER TABLE short_links ADD COLUMN IF NOT EXISTS platform TEXT NOT NULL DEFAULT ''`（幂等） |
| `base/web/index.html` / `app.js` | 目标/平台下拉；下载扩展名随 target 变化 |
| `tests/` | 新增 golden + check 脚本（见 §9） |

### 5.3 兼容与回退

- **Clash 输出零改动**：所有改动限定在 `singbox` 分支与独立模块；`tests/` 加入 Clash golden 对比防回归。
- **模板覆盖保留**：显式设置 `singbox_rule_base` + `singbox_config_mode="template"` 时仍走 `render_template` 老路径（默认 `generated`）。
- 旧 `singbox_rule_base` 默认值保留但不再被默认使用，避免破坏已有配置文件解析。

---

## 6. Web 短链门户改造

> **实现进度（本轮已落地）**
> - `short_links` 新增 `platform` 列：`ensure_schema()` 里用 `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` 幂等升级，另有 `db/migrations/003_shortlink_platform.sql` 留档。
> - `POST /api/short-links` 接受 `target` ∈ {`clash`, `singbox`} 与 `platform`（sing-box 时必填，缺省取 `SHORTLINK_SINGBOX_PLATFORM`）；非法平台返回 400。
> - 短链快照按存储的 `target`/`platform` 生成，刷新（refresh）沿用存储值；sing-box 目标 `Content-Type: application/json`。
> - 页面新增「目标格式」「sing-box 平台」下拉，列表展示 `sing-box/<platform>`，下载按钮文案与扩展名随目标切换。
> - 新增环境变量 `SHORTLINK_SINGBOX_PLATFORM`（默认 `macos`），已写入 `deploy/shortlink.env.example` 与 `docker-compose.shortlink.yml`。
> - 待验证：短链 API 冒烟需要 PostgreSQL，计划在 M6（test 环境）执行。



1. `base/web/index.html`：新增「目标」（Clash / sing-box）与「平台」（6 值，仅 sing-box 时可用）两个下拉；下载按钮文案/扩展名随目标切换（`.yaml` / `.json`）。
2. `base/web/app.js`：提交时带 `target` + `platform`；列表项展示目标与平台；预览按 `Content-Type` 处理。
3. `src/handler/shortlink_api.cpp`：
   - `parse_shortlink_request` 放开 target 白名单为 `{clash, singbox}`，非法值 400 `unsupported target`；
   - 新增 platform 校验（仅 `target=singbox` 时必填/可默认），写库；
   - `build_source_payload` 带上 platform；刷新（refresh）沿用存储值重新转换（保持「同一短链不同时间取到最新节点」的既有语义）；
   - 下载响应 `Content-Type: application/json; charset=utf-8`，文件名 `.json`。
4. DB：`short_links` 新增 `platform TEXT NOT NULL DEFAULT ''`；`ensure_schema()` 用 `ADD COLUMN IF NOT EXISTS` 幂等升级；`db/migrations/003_shortlink_platform.sql` 留档。
5. 环境变量新增 `SHORTLINK_SINGBOX_CONFIG`（对应现有 `SHORTLINK_CLASH_CONFIG`）、`SHORTLINK_SINGBOX_PLATFORM_DEFAULT`。

---

## 7. 缺陷修复清单（与 PRD 的对应）

见 §2.3 的 B1~B7。验收方式：每项都有「修复前 `check`/`run` 失败 → 修复后通过」的证据，写进 PR 描述。

---

## 8. 部署方案

> **实现进度（M6 已完成，2026-09-15）**
> - 在 test 宿主上用 `scripts/Dockerfile` 构建镜像，`docker-compose.shortlink.yml`（独立 project、`127.0.0.1:15053`）拉起 subconverter + PostgreSQL；
>   启动日志确认 `platform` 列的幂等迁移已执行（`ALTER TABLE ... ADD COLUMN IF NOT EXISTS`）。
> - `tests/shortlink_api_smoke.sh` 全绿：Clash 短链创建/预览/下载/刷新/撤销 + **sing-box/openwrt 短链**（结构断言、`Content-Type: application/json`、文件名 `custom-singbox-<date>[-n]-openwrt.json`）+ 非法平台 400 + 非法 target 400。
> - 部署实例生成物校验：macos/windows/linux 本机 `sing-box 1.14.0 check` 通过；openwrt 产物在网关内核 `1.14.0-openbox-tcp1` 通过。
> - **部署验证发现并修复的两个真实缺陷**：
>   1. `short_links` INSERT 增加了 `platform` 列后调用点只传了 11 个参数（SQL 有 12 个占位符）→ 创建短链 429；已修复。
>   2. `exec_params` 失败时静默返回，掩盖了上一条错误 → 现在会把 PostgreSQL 错误写入日志。
> - `tests/shortlink_api_smoke.sh` 另修正两处用例缺陷：httplib 对**无 Content-Length 的 POST** 返回 400（刷新请求需带空 body）；断言原先用命令行传整份配置导致 `Argument list too long`（改为临时文件）。
> - 回归证据：以 `27cc484`（改动前）单独构建的二进制与当前二进制，对同一订阅输出 `clash`/`clashr`/`surge` **逐字节相同**（使用仓库默认 `base/pref.toml`）。
> - **M7 前置发现（已处理）**：prod 现网运行的是 `codex/shortlink-usage` 分支构建的镜像（含 `SHORTLINK_USAGE_*` 用量采集、`/api/usage`、`services/sui-usage-adapter`）。该分支比 master 多 2 个提交（+5815 行），**不在 master 上**；直接部署会回退线上用量功能。
>   本轮已把 `codex/shortlink-usage` 合入工作分支 `codex/singbox-conversion`（合并提交，无功能取舍）：
>   `base/web/index.html`、`base/web/app.js` 以用量面板为基线重新叠加目标/平台选择器；`tests/shortlink_api_smoke.sh` 保留空 body POST 修复与 API key / 管理员 token 双认证。
>   合并后本地重新构建通过，`tests/singbox_golden.sh` 全绿，`scripts/check-sensitive.sh --all` 无发现。
> - M7 待办：用合并后的源码构建镜像 → 备份 prod（`.env`、数据卷、当前镜像 digest）→ 灰度替换 → 验证 `/version`、短链 smoke、用量接口未回退 → 保留回滚路径。



### 8.1 拓扑（沿用现状）

- 构建：`scripts/Dockerfile` → 镜像；`docker-compose.shortlink.yml`（subconverter + postgres）+ 宿主 nginx（`deploy/nginx/*.conf`）+ `deploy/shortlink.env.example`。
- test：`<TEST_HOST>`（x86_64，Docker 28.5）——先在宿主构建镜像并 `docker compose up -d`，跑 §9 验证。
- prod：`<PROD_HOST>`（x86_64，Docker 29.4 + Compose v5.1）——test 通过后同样流程 + nginx reload；**先备份 `postgres_data` 与 `.env`**。
- 部署脚本与 runbook 中一律使用占位符；真实主机、域名、证书路径只保留在私有运维记录中（遵循 `AGENTS.md` 的脱敏要求，`scripts/check-sensitive.sh` 门禁必过）。

### 8.2 步骤

1. 本地：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`；跑 §9 的 golden。
2. 构建 test 镜像（tag 带短 SHA），推/传到 test 宿主；`docker compose -f docker-compose.shortlink.yml up -d`。
3. test 验证：`/version`、Clash 短链回归、sing-box 6 平台各生成一份并过 `check`；网关与 macOS 各做一次真实 `run`。
4. 通过后按相同镜像 tag 部署 prod；观察 `/version`、短链创建/刷新、错误率；保留上一镜像 tag 以便一键回滚。
5. DB：prod 上执行 `003`（幂等），确认 `short_links.platform` 存在。

### 8.3 回滚

- 镜像回滚：切回上一 tag + `docker compose up -d`。
- 配置回滚：短链快照存于 DB，历史版本表 `short_link_versions` 可回溯。
- 网关侧：**只做 `check`，不做自动 `run`**；如需实跑，先备份 `/opt/open-box/etc/config.json` 与 `config.meta.json`。

---

## 9. 验证计划

### 9.1 结构验证（每次构建）

新增 `tests/singbox_golden.sh`：
1. 起本地 subconverter（独立端口）与本地订阅服务；
2. 对 6 个平台各请求一次，落盘到 `tests/singbox/out/<platform>.json`；
3. 用 `sing-box check -c` 逐个校验（本机 1.14.0）；
4. 对 OpenWrt 产物，额外在**网关内核**上 `check`（路径存在性只能在设备上验证）；
5. 断言关键字：不含黑名单字段（`inet4_address`、`"type":"dns"`、`"type":"block"`、`geosite`、`address_resolver`…），含 `default_domain_resolver`、`action:"sniff"`。

### 9.2 行为验证

- 链式：构造 6 组用例（正常两跳、悬空 via、成环、落地是组、自指、detour→空 direct），断言「前 1 组输出 `detour`，后 5 组按预期丢弃或 400」。
- 规则：geosite/geoip → `rule_set` 且 `route.rule_set` 数量与引用一致；大小写敏感字段保真；`[]FINAL` → `route.final`。
- 回归：Clash 输出 golden 不变。

### 9.3 端到端

- 本机 macOS：`sing-box run -c proto-macos.json` → `curl -x socks5h://127.0.0.1:2080 https://ipinfo.io/ip` 出口 IP 变化；`clash_api /connections` 看到 `chains`。
- 网关：`check` 通过 + 维护窗口 `sing-box run`（独立 tun 网段/端口），确认 `sing-box started` 与 `logread` 无 FATAL；随后恢复。
- 短链：`tests/shortlink_api_smoke.sh` 扩展 sing-box 用例（创建/预览/刷新/下载/非法平台 400）。

### 9.4 已完成的可行性预验证（本次评审阶段）

- 本机 1.14.0：新骨架原型 `check` 通过；
- 网关 1.14.0-openbox-tcp1：含 `auto_redirect` + `dns_mode/dns_address` + `detour` 链式 + 本地 `.srs` + 防回环 reject 的 OpenWrt 原型 `check` 通过；
- 反向用例：悬空 `detour`、成环 `detour` 均能通过 `check` → 证实生成期校验的必要性。

---

## 10. 里程碑

| 里程碑 | 内容 | 出口标准 |
|---|---|---|
| M1 | 生成器骨架 + 6 平台变体（§4.3/4.4），含参数解析 | 6 份产物本机 `check` 通过 |
| M2 | 链式代理（§4.6） | 6 组链路用例断言通过 |
| M3 | DNS / route / rule_set / experimental 代码化（§4.7/4.8） | 产物含 `default_domain_resolver`、remote/local rule_set 且 check 通过 |
| M4 | 缺陷 B1~B7 | 每项有前后对比证据 |
| M5 | Web/短链 + DB migration（§6） | 页面可选平台；API smoke 通过 |
| M6 | test 部署与验证（§8/9） | test 全绿 |
| M7 | prod 灰度 | 线上短链正常、可回滚 |

---

## 11. 风险与回滚

| 风险 | 影响 | 缓解 |
|---|---|---|
| OpenWrt 完整配置被 Open-Box 覆盖 | 配置失效 | §4.5 路径 A（独立服务/独立配置路径）；文档写明 |
| 与 Open-Box 抢 `auto_redirect` / tun / 9095 | 网关断网 | 错开 tun 网段、clash_api 端口、cache_file；同一时刻只有一个进程接管 |
| 生成的 remote `.srs` 首次下载失败（GitHub 不可达） | sing-box 起不来 | 桌面端保留 `cache_file`；OpenWrt 用本地 `.srs`；文档给出离线获取方式 |
| 规则语义变化（1.14 规则集合并语义修正） | 分流结果与旧版不同 | M3 后做一次分流回放（按 `route.rules` 顺序离线匹配域名对比） |
| 移动端字段被 App 覆盖 | 用户以为配置生效 | 文档标注「哪些字段由 App 管理」 |
| prod 数据库变更 | 短链不可用 | migration 幂等 + 先备份 + 回滚用上一镜像 |
| 链式代理真实延迟误导（urltest 测的是单跳） | 选路不佳 | 文档说明；不把链式节点自动放进 urltest 组（或加选项） |

---

## 12. 已确认决策（2026-09-15 评审）

§0 的 D1~D13 全部确认，评审遗留问题全部关闭：

1. 默认平台 = **macos**（D8）
2. OpenWrt 参数直接复用 Open-Box 现网值（D9）
3. OpenWrt 落地 = **路径 B：临时替换 `/opt/open-box/etc/config.json`**（D10）
4. 缺失 `.srs` 由部署脚本推送到 `/opt/open-box/data/rulesets`（D11）
5. 链式代理默认 **丢弃 + WARN**（D12）
6. 移动端 **不输出** `clash_api` / `cache_file`（D13）

实现期仍需现场确认（不阻塞编码）：
- OpenWrt 产物的 `clash_api.secret`：默认空（只监听回环）；若要面板继续可用，需与 Open-Box 存储的 secret 对齐。
- 部署时 `.srs` 的获取源（网关现网已有 7 个；缺失的从 `sing-geosite` / `sing-geoip` 拉取后推送）。
