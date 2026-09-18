# sing-box 配置生成器实现摘要（源自本地 Obsidian sing-box 学习专题）

> 阅读范围：`wiki/learning/sing-box/` 全 11 篇 + `_index.md`，以及 vault 内 `Open-Box`、`Open-Box TCP DNS 兼容内核`、`Open-Box 内核 TCP DNS 兼容补丁` 三篇。
> 目标：给 **subconverter** 的 C++ 开发者实现 `target=singbox` 生成器用。
> 所有真实凭据、真实 IP/域名已替换为占位符（`<UUID>` / `<PASSWORD>` / `example.com` / `203.0.113.10`）；绝对本地路径也一律脱敏。原笔记本身**未包含真实凭据**（模板里本来就是 `<uuid>` / `<pbk>` 形式），仅少量运维 IP 与路径，已在本文脱敏。

---

## 1. 版本基线

### 1.1 基准版本

- 全专题基准：**sing-box 1.14.0**（官方 stable，2026-08-31）。所有 JSON 示例经 1.14.0 二进制 `sing-box check` 实测（exit 0、无 warning）。
- 1.14 是"断代"版本：**1.12 起标记废弃的旧 DNS 写法在本版被彻底删除**，大量 2025 年前的教程配置在 1.14 上**直接启动失败**。

### 1.2 版本对照表（生成器必读）

| 来源 | 版本 | 关键影响 |
|---|---|---|
| 官方 stable | **1.14.0** | 基准；旧 DNS 写法彻底删除 |
| 1.13 线最新 | 1.13.21 | 旧 DNS 写法仍可用（需 `ENABLE_DEPRECATED_LEGACY_DNS_SERVERS=true`） |
| Homebrew（实测） | 1.13.7 | 比基准旧；用它验证 1.14 配置会得出错误结论 |
| Open-Box 生成基准内核 | 1.13.14 | 配置格式用 1.12+ typed DNS 新格式 |
| Open-Box 实机内核 | `1.14.0-openbox-tcp1` | 官方 1.14.0 + TCP DNS 短连接补丁，**配置兼容性与官方 1.14.0 一致** |
| ImmortalWrt 24.10 / 25.12.0 feed | **1.12.25** | 比 1.14 旧：`dns_mode` / `dns_address` 等 1.14 新字段**不能用** |
| OpenWrt 24.10 官方 feed | 1.12.22 | 同上 |
| `testing` 分支 | 1.15.x-alpha | `stack` 新增 `go` 并成为默认；`endpoint_independent_nat` 失效 |

**生成器结论**：默认输出 **1.14 typed 语法**；若目标端是 OpenWrt feed 包（1.12.x），需按最小功能子集渲染（见 §2 的"最小 delta"）。

### 1.3 版本特定 API 变更时间线（1.10 → 1.14）

| 版本 | 变更 | 在 1.14 上的行为 |
|---|---|---|
| 1.10 | `inet4_address`/`inet6_address` → `address`；`*_route_address` → `route_address` | **硬失败** |
| 1.10 | `rule_set_ipcidr_match_source` → `rule_set_ip_cidr_match_source` | **硬失败** |
| 1.10 | 新增 `rule_set` 的 `inline` 类型 | — |
| 1.11 | **引入 rule action**：入站 `sniff` / `domain_strategy` → `route.rules[].action` | **硬失败**（1.13 移除） |
| 1.11 | `{"type":"dns"}` 出站 + `outbound: "dns"` → `action: "hijack-dns"` | **硬失败** |
| 1.11 | `{"type":"block"}` → `action: "reject"` | 旧写法**仍能加载但已无语义**（见下） |
| 1.11 | `direct` 出站的 `override_address`/`override_port` → `action: "route-options"` | **硬失败** |
| 1.11 | WireGuard 出站 → `endpoints` | **硬失败** |
| 1.11 | TUN `gso` 移除 | **硬失败** |
| 1.12 | **DNS server 重构**：`address: "https://..."` → `type` + `server` | **硬失败（1.14 起）** |
| 1.12 | `address_resolver` → `domain_resolver`；新增 `route.default_domain_resolver` | 必须迁移 |
| 1.12 | GeoIP / Geosite 数据库删除 → `rule_set` | **硬失败** |
| 1.12 | `domain_strategy`（拨号字段）→ `domain_resolver` | 需环境变量（计划 1.14 删） |
| 1.12 | 新增 `tls.fragment` / `tls.record_fragment` | — |
| 1.13 | 清理 1.11 标记的全部 legacy；新增 NaiveProxy、ICMP 代理、`preferred_by`、`interface_address` | — |
| 1.14 | `independent_cache`、`store_rdrc`、`download_detour`、内联 `tls.acme`、DNS 规则的 `strategy` **弃用** | **仅 WARN**（计划 1.16 移除） |
| 1.14 | 新增 DNS `evaluate`/`respond` + `match_response`；**不带 `match_response` 的旧地址过滤字段（`ip_cidr`/`ip_is_private`）弃用** | WARN |
| 1.14 | `experimental.cache_file.store_mode` / `store_selected` **删除** | **硬失败（unknown field）** |
| 1.14 | 新增 `http_clients`、`network_namespaces`、`dns_mode`、`find_neighbor`、`package_name_regex`、`certificate_providers`、`services` | — |

**"WARN" vs "FATAL" 的判据**（笔记从 deprecated 机制推导）：计划删除版本距当前 ≤1 个 minor 才叫 imminent；imminent 且有环境变量逃生舱 → ERROR + FATAL；不 imminent → 仅 WARN。故在 1.14 上：**计划 1.16 删的 = WARN，计划 1.14 删的 = FATAL**。

### 1.4 明确"已废弃 / 已移除"清单（生成器禁止输出）

| 字段 | 状态 | 替代 |
|---|---|---|
| `dns.servers[].address`（含 `"https://…/dns-query"`、`"rcode://"`） | **1.14 已删除** | `type` + `server` |
| `inbounds[].sniff` | 1.11 弃用 / **1.13 移除** | `route.rules[].action: "sniff"` |
| `inbounds[].domain_strategy` | 同上 | `route.rules[].action: "resolve"` |
| `inbounds[].gso` | **1.11 移除** | 无 |
| `inbounds[].inet4_address` / `inet6_address` / `*_route_address` | **1.10 起硬失败** | `address` / `route_address` |
| `outbounds: {"type":"dns"}` | **1.13 起硬失败** | `action: "hijack-dns"` |
| `outbounds: {"type":"block"}` | 官方称 1.13 移除，**1.14 实测仍能过 check（空壳无语义）** | `action: "reject"` |
| `outbounds: {"type":"wireguard"}` | **1.13 起硬失败** | `endpoints` |
| `outbounds[].override_address` / `override_port` | **1.11 起硬失败** | `action: "route-options"` |
| `outbounds[].domain_strategy` | 1.12 弃用 | `domain_resolver` |
| 顶层 `geosite` / `geoip` 规则项 | **1.12 移除** | `rule_set` |
| `experimental.clash_api.cache_file` / `store_mode` / `store_selected` | **1.14 移除（unknown field）** | `experimental.cache_file` |
| `experimental.cache_file.store_rdrc` | 1.14 弃用，计划 1.16 移除 | `store_dns` |
| `dns.independent_cache` | 1.14 弃用，计划 1.16 移除 | 直接删（缓存恒按传输分离） |
| `route.rule_set[].download_detour` | 1.14 弃用，计划 1.16 移除 | `http_client` |
| `tls.acme`（内联） | 1.14 弃用 | `certificate_providers[]` |
| `dns.rules[].strategy` | 1.14 弃用 | — |
| `dns.fakeip`（顶层对象） | 已由 `type: "fakeip"` 的 server 取代 | `servers[]{type:"fakeip"}` |
| `rule_set_ipcidr_match_source` | 硬失败 | `rule_set_ip_cidr_match_source` |
| `address_resolver` | 硬失败 | `domain_resolver` |

> 工具链里已有 `default_domain_resolver`（1.12+）、`dns.servers[]` 的 `type` 对象形式、`endpoints`、`route.rules[].action`、rule_set `.srs`、`experimental.cache_file`、`clash_api` —— 这些正是 1.14 生成器必须输出的"现代形态"。

---

## 2. 跨平台差异

### 2.1 总结论

**一份 JSON 不能原样四端复用**；约 **80% 字段（策略层）可共享**，`inbounds` 与少量平台项必须按端改写。

| 层 | 能否共享 | 说明 |
|---|---|---|
| `outbounds`（节点 / selector / urltest / TLS / uTLS / REALITY / transport） | ✅ 完全共享 | 纯用户态代码 |
| `endpoints`（WireGuard / Tailscale…） | ✅ 共享 | 需要对应构建标签 |
| `dns`（servers / rules / final / strategy） | ✅ 共享 | 逻辑与平台无关 |
| `route.rules` / `route.rule_set` / `route.final` | ✅ 共享 | 含 `process_name` / `package_name` / `wifi_*` 等平台专属字段时要删 |
| `experimental` | ⚠️ 语义共享，**值要改** | `cache_file.path` 绝对路径必失效；移动端 `clash_api` 通常由 App 自管 |
| `inbounds` | ❌ **必须逐端重写** | 没有任何一个入站类型在四端都合法/可用 |
| `route.auto_detect_interface` | ✅ 必须为 true | Apple 端必须为 false | ✅ **Android 必须为 true**（见 2.1.1） | ✅ 必须为 true |

### 2.2 差异的三个根因

1. **进程有没有特权写内核网络栈**：Linux 路由器 root / `CAP_NET_ADMIN`；macOS CLI 需 root 才能建 `utun`；iOS/Android **完全无特权**，TUN 由 NetworkExtension / VpnService 提供。
2. **系统有没有可被接管的本地 DNS 解析器**：路由器有 dnsmasq/odhcpd；macOS 有 mDNSResponder（会代理缓存 DNS，故 `reverse_mapping` 不可靠）；iOS/Android DNS 由 VPN 配置下发。
3. **OS 是否提供官方 TUN 通道**：Linux/Windows/macOS 由 sing-box 自建（sing-tun）；iOS/Android 必须走 NE / VpnService，**字段可用性是框架能力的子集**。

### 2.2.1 Android 的 `auto_detect_interface` 是硬要求（2026-09 实测结论）

**结论：Android 必须写 `route.auto_detect_interface: true`；Apple 端（iOS/SFM/SFI）必须不写。**

早先本仓库的笔记误记为「Android 无此字段」，据此生成的 Android 骨架**不带该字段**，实测表现为「sing-box 启动成功但完全不能上网」。源码链路（`sing-box v1.14.1`）如下：

1. `constant.IsLinux = goos.IsLinux == 1 || goos.IsAndroid == 1`（`constant/os.go:23`）——Android 在 sing-box 里**算 Linux**，所以 `route.auto_detect_interface` 的守卫 `!(C.IsLinux || C.IsDarwin || C.IsWindows)`（`route/network.go:73`）**在 Android 上放行**；写了不会 `check` 失败。
2. 只有 `networkManager.AutoDetectInterface() == true` 时，`common/dialer/default.go` 才会走到 `networkManager.ProtectFunc()`（`default.go:123`）。
3. `ProtectFunc()` 在平台接口 `UsePlatformAutoDetectInterfaceControl()` 为 true 时，对每个新建 socket 调用 `platformInterface.AutoDetectInterfaceControl(fd)`（`route/network.go:396-403`）。
4. SFA 的实现就是 `override fun autoDetectInterfaceControl(fd: Int) { protect(fd) }`（`VpnService.kt:51`），也就是 Android `VpnService.protect()`——**把该 socket 排除在 VPN 路由之外**。
5. 反之，不写该字段时 `BindInterface`/`RoutingMark` 也都为空、`autoDetectBindFunc` 为 nil，**没有任何代码调用 `protect()`**：所有出站 TCP/UDP socket（含 `dns-direct` 的 53/UDP 与 `dns-proxy` 的 443）都被重新导入 TUN，形成自环。

**Apple 端为什么相反**：`ExtensionPlatformInterface.swift:225` 的 `usePlatformAutoDetectControl()` 返回 **false**，`autoDetectControl()` 是空实现；NE 由系统托管隧道与路由，不需要也无法用 `protect()`。且 Apple 端若设 `auto_detect_interface` 会走 `AutoDetectInterfaceFunc()` 分支去 bind 物理网卡，反而破坏 NE 语义。

**故障日志指纹**（`logs_*.txt`，1 秒内 1121 行）：

| 现象 | 证据 |
|---|---|
| DNS 包不断回到 TUN | `inbound/tun[tun-in]: inbound DNS packet from <TUN_PEER_IP>:<PORT>` 反复出现（本机 TUN 网段 `172.19.0.1/30`，源地址即隧道对端） |
| 源就是 App 自己的 socket | 紧邻的 `router: found package name: io.nekohasekai.sfa`（336 次）——只有「自己发给自己的包」才会被解析回自己的包名 |
| 自环而非正常查询 | 同一源端口 `:46186` 复用 203 次、几秒内上千行，且几乎**没有出站连接日志**（全程仅 10 条 `outbound connection`，且都是 urltest 探活） |

对照：同样的配置在 macOS 上（带 `auto_detect_interface`）能正常解析并出站（`outbound/vless[...]: outbound connection to raw.githubusercontent.com:443` → `sing-box started`）。

### 2.3 逐平台对比表

| 维度 | macOS (Apple Silicon) | iOS | Android | ImmortalWrt (arm64) |
|---|---|---|---|---|
| 运行形态 | CLI（brew）**需 root**；或官方 GUI | 官方 App（NetworkExtension） | 官方 App（VpnService） | procd 服务；官方包默认跑 `sing-box` 用户，**TUN 需 root** |
| 推荐入站 | `tun`；调试用 `mixed` | **只能 `tun`**（App 实现） | **只能 `tun`** | `tun` + `auto_redirect`（首选）；或 `redirect`+`tproxy`+`dns-in` |
| `auto_redirect` | ❌ 无效（无 nftables），**写了 `check` 失败** | ❌ | ⚠️ 仅 root 设备可用 | ✅ **官方推荐**，自动向 fw4 插规则 |
| `auto_detect_interface` | ✅ 必开 | ❌ 必须不写 | ✅ **必开**（否则无 `protect()`，全量自环断网） | ✅ **必开**（不开直接断网） |
| `strict_route` | 无实际语义 | ❌ 未实现 | ❌ 未实现 | ✅ Linux 上有明确语义（ICMP 行为、SO_BINDTODEVICE） |
| 进程/用户规则 | ✅ `process_name`/`process_path`/`user` | ❌ 不支持（越狱版才支持） | ❌ 用 `package_name` | ✅ 全支持，还有 `include_uid`/`source_mac_address` |
| 路径类字段 | 任意路径 | App 容器内 / Profile 方式 | App 容器内 / Profile 方式 | `/etc/sing-box` 等固定位置 |
| 配置下发 | 本地 JSON 文件 | **只能 Profile**（本地/iCloud/远程 URL） | **只能 Profile**（本地/远程 URL） | 本地 JSON；或面板/UCI 生成 |
| `clash_api` | ✅ 可用 | App 自管，通常不暴露 | App 自管 | ✅ 可用（Open-Box 靠它） |
| DNS 处理 | `dns_mode: native` 可写接口 DNS | App 把 `dns_address` 变成 tunnel DNS | `VpnService.addDnsServer()` | 自身即网关：`hijack-dns` + dnsmasq 接管 + 防火墙重定向 |
| `route_address_set` | ✅ | ✅ | ❌ **会 `DeadSystemException`** | ✅ 预匹配可用 |
| per-app 分流 | `process_name` | ❌ | ✅ `package_name`（`include_package` 与 `exclude_package` **不可同时非空**） | `include_uid`/`source_mac_address` |
| TUN `stack` | `system` 或 `mixed` | **仅 `system` / 省略**（已证伪，见下） | **仅 `system` / 省略** | `system` 或 `mixed` |
| 构建标签 | 官方包齐全 | **不含 `with_gvisor`（二进制实测）** | App 内置，用户不可换 | 官方包含 `with_gvisor,with_quic,with_dhcp,with_wireguard,with_utls,with_clash_api,with_tailscale…`（**不含 `with_grpc`**） |

> **`stack` 结论（2026-09，二进制实测）**：官方 Apple 客户端内核**不带 `with_gvisor`**。对 SFM 1.15.1000 的 `Library.framework` 做 `strings` 核验：`stack_gvisor_stub.go` 命中 2 次，而 `sagernet/gvisor`、`stack_gvisor.go`、`stack_mixed.go` 命中均为 **0**；正对照（Homebrew CLI，带 `with_gvisor`）的 `sagernet/gvisor` 命中 7209 次。因此 `stack: "mixed"`（= system TCP + gVisor UDP）与 `"gvisor"` 在官方客户端上**必然启动失败**，报 `gVisor is not included in this build, rebuild with -tags with_gvisor`；只有 `system` 与「省略」可用。
>
> 两个推论：① 本机 `sing-box check` 通过**不能**证明客户端可用——本机 CLI 恰好带该标签（假绿，golden 测试曾因此长期放过此缺陷）；② `stack` 在 1.15.0 已废弃、1.17.0 移除，故**省略是唯一同时满足 1.14/1.15/1.17 与全部官方客户端的选择**。SFM 另有一条约束：开启 `includeAllNetworks` 时栈被强制为 `gvisor`，此时 `system`/`mixed` 反而不可用。

### 2.4 各平台精确增量片段

**共享部分**：`log` / `dns` / `outbounds` / `route`（去掉平台专属规则）/ `route.rule_set`。

**macOS 增量**
```jsonc
"inbounds": [
  { "type": "tun", "tag": "tun-in",
    "address": ["172.19.0.1/30", "fdfe:dcba:9876::1/126"],
    "mtu": 9000,
    "auto_route": true, "strict_route": false }
  // ⚠ 删掉 auto_redirect 与 exclude_interface（仅 Linux）
],
"route": { "auto_detect_interface": true }
```
启动：`sudo sing-box run -c "<config-dir>/config.json"`（root 是硬要求）。

**iOS 增量**
```jsonc
"inbounds": [
  { "type": "tun", "tag": "tun-in",
    "address": ["172.19.0.1/30", "fdfe:dcba:9876::1/126"],
    "mtu": 9000,
    "auto_route": true,
    "dns_mode": "hijack",
    "dns_address": ["<TUN_PEER_IP>", "<TUN_PEER_IP6>"] }   // 1.14+；旧客户端报未知字段
]
// ⚠ 删掉：route.auto_detect_interface、strict_route、include_*/exclude_*、
//          以及 route.rules 里所有 process_*/user*/package_* 条件
```
下发方式：作为 **Profile** 导入官方 App；远程 Profile URL Scheme：
`sing-box://import-remote-profile?url=<encodeURIComponent(url)>#<名称>`，默认 60 分钟自动更新。

**Android 增量**
```jsonc
"inbounds": [
  { "type": "tun", "tag": "tun-in",
    "address": ["172.19.0.1/30", "fdfe:dcba:9876::1/126"],
    "mtu": 8500,
    "auto_route": true,
    "dns_mode": "hijack", "dns_address": ["<TUN_PEER_IP>", "<TUN_PEER_IP6>"] }
],
"route": {
  "auto_detect_interface": true,   // ★ 必写：唯一的 VpnService.protect() 入口（见 2.2.1）
  "override_android_vpn": true     // 让 TUN 把 Android VPN 当上游
}
// ⚠ 删掉：interface_name、gso（历史字段）、include_uid、include_interface、
//          route_address_set / route_exclude_address_set（VpnService 会崩）
// per-app 交给 App 界面选，不要写死在 JSON 里
```

**ImmortalWrt / OpenWrt 增量（TUN 路线，首选）**
```jsonc
"inbounds": [
  { "type": "tun", "tag": "tun-in",
    "interface_name": "singtun0",
    "address": ["172.19.0.1/30"],
    "mtu": 9000,
    "auto_route": true,
    "auto_redirect": true,
    "strict_route": true,
    "exclude_interface": ["br-lan", "pppoe-wan"] }
],
"route": { "auto_detect_interface": true }
```

**ImmortalWrt 增量（传统 tproxy/redirect 路线，须自行写 nftables）**
```jsonc
"inbounds": [
  { "type": "redirect", "tag": "redirect-in", "listen": "::", "listen_port": 5331 },
  { "type": "tproxy",   "tag": "tproxy-in",   "listen": "::", "listen_port": 5332, "network": "udp" },
  { "type": "direct",   "tag": "dns-in",      "listen": "127.0.0.1", "listen_port": 7853 }
]
```
> 官方 OpenWrt 包**不生成任何 nftables 透明代理规则**；走 TUN + `auto_redirect` 时内核自动向 fw4 插规则，走 tproxy/redirect 需自己维护。

**Windows / Linux（通用桌面，笔记未单列增量，由可比字段推导）**
- `tun` + `auto_route`；`route.auto_detect_interface: true` 必开；`strict_route` Windows 语义为"防多宿主 DNS 泄漏"；`stack` 可用 `system`/`mixed`；`auto_redirect` 仅 Linux；进程规则 `process_name`/`process_path`/`process_path_regex` 均支持；`set_system_proxy` 可用于 `mixed` 入站。

### 2.5 其它关键平台字段

| 字段 | 作用 | 平台 |
|---|---|---|
| `route.default_interface` | 手动指定出口网卡，替代 auto_detect | Linux/Win/macOS |
| `route.default_mark` | 给出站连接打路由标记（配合策略路由） | **仅 Linux** |
| `route.find_process` | 无进程规则时也扫描进程，仅用于输出日志 | Linux/Win/macOS |
| `route.find_neighbor` | 邻居解析，用于 `source_mac_address`/`source_hostname` | Linux/macOS（1.14+） |
| `route.dhcp_lease_files` | 自定义 DHCP 租约文件路径 | Linux/macOS（1.14+） |
| `route.override_android_vpn` | 允许 TUN 把 Android VPN 当上游 | **仅 Android** |
| `route.default_http_client` | 远程规则集默认用哪个 HTTP 客户端 | 全平台（1.14+） |
| `inbounds[].auto_redirect` | nftables 加速透明代理 | **仅 Linux** |
| `inbounds[].exclude_interface` | 排除回程接口 | 仅 Linux |
| `inbounds[].include_uid`/`exclude_uid` | 按用户分流 | 仅 Linux |
| `inbounds[].include_package`/`exclude_package` | per-app | 仅 Android（**不可同时非空**） |
| `inbounds[].dns_mode` | `disabled`/`native`/`hijack`（默认） | 1.14+；移动端/旧版本无此字段 |
| `inbounds[].dns_address` | TUN 的 tunnel DNS 地址 | 1.14+，移动端必要 |
| `inbounds[].set_system_proxy` | 顺便设系统代理 | 仅 Linux/Android/Windows/macOS |

### 2.6 绑定与优先级

- **出站绑定优先级**：`outbound.bind_interface` > `route.default_interface` > `route.auto_detect_interface`。
- **`clash_api.external_controller` 绑定**：一律 `127.0.0.1:9095`（**只监听回环**）；暴露到公网**必须**配 `secret`。移动端通常由 App 自管、不暴露。
- **`detour` 与拨号字段互斥**：设了 `detour` 后同层的其他拨号字段（`bind_interface`、`routing_mark` 等）**被忽略**。

### 2.7 "最小 delta"指导

1. **共享策略层（`outbounds` + `route` + `dns` + `rule_set`），分叉接入层（`inbounds`）与特权层。**
2. 用**模板 + 变量生成**而不是"一份配置到处塞"，这是唯一能保证四端 `route`/`dns` 语义完全一致的做法（另两种：远程 Profile URL、面板托管）。
3. 路由器端可由面板托管，桌面/macOS 手工维护，移动端复用同一份"逻辑配置"渲染出的 Profile。
4. **取最低版本的功能子集**（例如不写 `dns_mode`），或按端渲染。
5. **Apple 客户端对"未实现字段"是静默忽略还是报错，官方文档未说明 → 建议干脆不写，而不是赌它被忽略。**
6. 生成器应把 `route.rules` 中平台专属条件（`process_*` / `user*` / `package_*` / `wifi_*` / `source_mac_address`）在非对应平台上过滤掉，而不是原样下发。

---

## 3. 链式代理（detour）

### 3.1 语义与最小示例

```
本机 ──直连──> 前置(第一跳) ──> 落地(出口) ──> 目标网站
```
动机：落地机放家宽/落地 VPS 但**没有公网可达地址**（或不想暴露），用一台本机可直连的中转机当前置。

```jsonc
{
  "outbounds": [
    { "type": "shadowsocks", "tag": "前置",
      "server": "203.0.113.10", "server_port": 8388,
      "method": "aes-128-gcm", "password": "<PASSWORD>" },

    { "type": "shadowsocks", "tag": "落地",
      "server": "203.0.113.20", "server_port": 8388,     // 不要求本机可达
      "method": "aes-128-gcm", "password": "<PASSWORD>",
      "detour": "前置" },                                 // ★ 全部魔法就这一个字段

    { "type": "direct", "tag": "direct" }
  ],
  "route": { "final": "落地" }
}
```

### 3.2 规则要点

- **`detour` 属于"拨号字段"（`shared/dial` 组）**；设了 `detour` 后同层其他拨号字段（`bind_interface`、`routing_mark`）**被忽略**。
- 落地节点上写的 `server` 是**由前置去解析/连接**的地址，所以可以是前置能路由到的内网地址。
- 链是**线性的**：一个节点只能有一个上游（`detour` 单值）。多级就 A→B→C 串。
- **`selector` / `urltest` 组本身不接受 `detour`**：写上去 `check` 直接报 `unknown field "detour"`。
- **但节点的 `detour` 指向一个 selector／组是可行的**：实测 `land.detour = "GRP"`（GRP 为 selector）**能过 check 且能正常 start**（该组会被初始化）。这修正了"落地必须是具体节点、不能是组"的常见说法；**反向不行**（给 selector 本身写 `detour` 报 unknown field）。工程上仍建议上游用具体节点，因为组会被切走、链路语义模糊。
- **WireGuard endpoint 也可以作为链式的前置或落地**（endpoint 的 `tag` 可被 selector/urltest 与 `detour` 引用）。

### 3.3 在策略组里的用法

```jsonc
"outbounds": [
  { "type": "selector", "tag": "PROXY", "outbounds": ["落地", "direct"] },  // 全局入口
  { "type": "shadowsocks", "tag": "前置", "…": "…" },
  { "type": "shadowsocks", "tag": "落地", "detour": "前置", "…": "…" },
  { "type": "direct", "tag": "direct" }
]
```
`route.final` → `PROXY`，`PROXY` 里选 `落地`，实际链路 = 前置 → 落地。

### 3.4 DNS server 的 `detour`

- `dns.servers[]` 上的 `detour` 表示**这条 DNS 查询本身走哪个出站**。例：
  ```jsonc
  { "type": "https", "tag": "dns-proxy", "server": "1.1.1.1", "detour": "PROXY" }
  ```
  这保证查询报文本身也从代理出去，结果在代理侧产生，抗 DNS 污染。
- `dns.servers[]` 还可带 `domain_resolver`（解析本 server 的域名用哪个 resolver）、`client_subnet`。

### 3.5 致命盲区：`check` 不校验 detour（实测台账，官方 1.14.0）

| 配置 | `sing-box check` | `sing-box run` |
|---|---|---|
| `detour` 指向**不存在的 tag** | **通过（exit 0）** | `FATAL: start service: dependency[does-not-exist] not found for outbound[落地]` |
| `detour` 成环（`落地.detour = PROXY` 且 `PROXY` 含 `落地`） | **通过（exit 0）** | `FATAL: start service: circular outbound dependency: PROXY -> land -> PROXY` |
| 给 selector/urltest **本身**写 `detour` | 失败（`unknown field "detour"`） | — |
| 节点 `detour` 指向 selector（节点 → 组） | **通过** | **实测能正常启动** |
| selector 组写成环 / 成员不存在 | 0（通过） | 启动时 FATAL（`dependency[...] not found`） |

**生成器必须自己做的静态校验**（Open-Box `engine/chain.mjs` 的做法：自建依赖图，逐条判定并丢弃）：
- `landing-is-group`：落地是组 → 丢弃
- `missing-landing` / `missing-via`：落地/前置 tag 不存在 → 丢弃
- `cycle`：成环 → 丢弃

> Open-Box 明确写下："**生成的配置能过 check 不代表链路是对的。**"

### 3.6 三个更易忽略的坑

1. **订阅自带的链会被"改名"打散**：Clash(Meta) 的 `dialer-proxy` 与 sing-box 的 `detour` 语义一致，导入时会原样保留，但它们指向**订阅里的原始节点名**；面板按地区/特征**重命名**节点后名字对不上，该链会被当成"前置不存在"丢掉。
2. **测速测的不是链路延迟**：链路中每一跳的延迟都是"**本机直连该节点**"测出来的。`urltest` 按这个值排序，**自动择优在链式场景下不代表真实端到端延迟**。
3. **`final`/`PROXY` 指向落地时的自环**：最常见写法 `PROXY` 组包含**所有节点**（含落地），同时 `落地.detour = "PROXY"` ⇒ 落地 → PROXY → 落地 → 内核启动即 FATAL。解决：给落地单独建一个**不含落地自己**的前置组。

### 3.7 与"订阅转换拼链路"方案对比

| 维度 | sing-box 原生 `detour` | 订阅转换 `x-sc-underlying-proxy` |
|---|---|---|
| 生效位置 | 客户端/内核侧 | 转换服务侧（输出 Clash 的 `dialer-proxy`） |
| 适用客户端 | sing-box 系 | Clash.Meta 系（mihomo / Clash Verge） |
| 校验 | 无（需自己保证） | 由转换服务决定，通常无 |
| 密钥安全 | 自己持有 | 节点信息经第三方转换服务 |
| 适合 | 自建 + 长期 | 快速试、纯机场场景 |

> 对 subconverter 的直接启示：Clash 侧的 `dialer-proxy` 与 sing-box 的 `detour` 可**双向映射**；转换到 sing-box 时把 `dialer-proxy: <name>` 写成 `"detour": "<name>"`，并**在生成期做环检测与 tag 存在性校验**（内核不会替你挡）。

### 3.8 代理服务器域名解析与 `domain_resolver`（链式的自举前提）

- 代理服务器的域名（`outbounds[].server`）由 **`route.default_domain_resolver`** 或 **`outbounds[].domain_resolver`** 解析。
- 不配且存在多个 DNS 传输时，1.14 报 ERROR 并 FATAL（除非设 `ENABLE_DEPRECATED_MISSING_DOMAIN_RESOLVER=true`）。
- **被指向的那个 DNS server 自身必须是纯 IP 地址的 server**（如 `dns-direct` 用 `223.5.5.5`），否则循环依旧存在。

---

## 4. DNS 与分流

### 4.1 三条铁律

1. **DNS server 的 `server` 字段只写 IP 或纯域名，不要写 `https://…/dns-query`。** 1.12 起改为 `type` + `server` 强类型格式；旧写法在 **1.14 已删除**：`FATAL: legacy DNS server formats ... removed in sing-box 1.14.0`。
2. **用域名当 DNS 服务器地址时，必须给 `domain_resolver`**，否则 `missing domain resolver for domain server address`。用纯 IP 就没这个问题 → **推荐 DNS 服务器一律写 IP**。
3. **代理服务器的域名由 `route.default_domain_resolver` 解析**（见 §3.8）。

### 4.2 `dns` 段完整结构（1.14）

```jsonc
"dns": {
  "servers": [ /* 每个有 type + tag */ ],
  "rules":   [ /* 谁走哪台 */ ],
  "final": "dns-proxy",        // 没命中任何规则的查询用哪台；默认用第一台
  "strategy": "prefer_ipv4",   // prefer_ipv4 | prefer_ipv6 | ipv4_only | ipv6_only
  "disable_cache": false,      // 与 optimistic 冲突
  "disable_expire": false,     // 与 optimistic 冲突
  "cache_capacity": 0,         // LRU 容量，<1024 忽略（1.11+）
  "optimistic": false,         // 乐观缓存：过期也先返回、后台刷新（1.14+，默认超时 3d）
  "timeout": "10s",            // 每次查询默认超时（1.14+）
  "reverse_mapping": false,    // 存 IP→域名反向映射（macOS 上不可靠）
  "client_subnet": "",         // EDNS0 subnet
  "fakeip": {}                 // 已被 type: fakeip 的 server 取代，勿用
}
```

### 4.3 `servers[].type`（**不写 type = 旧格式，已删除**）

| type | 用途 | 主要字段 |
|---|---|---|
| `udp` | 明文 UDP DNS（最常用，直连解析） | `server: "223.5.5.5"` |
| `tcp` | 明文 TCP DNS | `server` |
| `tls` | DoT | `server`、`tls` |
| `https` | DoH（**推荐给 dns-proxy**） | `server`（可省路径） |
| `h3` | DoH over HTTP/3 | `server` |
| `quic` | DoQ | `server` |
| `local` | 系统/本机解析器 | 无 server |
| `hosts` | 本地 hosts 表 | `predefined: { "a.com": "1.2.3.4" }` |
| `fakeip` | 返回假 IP，配合 sniff | `inet4_range` / `inet6_range` |
| `dhcp` | 从 DHCP 拿上游 DNS | `interface`（可省） |
| `mdns` | 局域网 mDNS（1.14+） | — |
| `resolved` | systemd-resolved | 仅 Linux |
| `tailscale` | Tailscale MagicDNS | — |

每个 server 还可带：`tag`（唯一，被 rules 引用）、`detour`、`domain_resolver`、`client_subnet`。

### 4.4 `dns.rules[].action` 语义（1.11+ **必填**）

| action | 作用 |
|---|---|
| `route`（默认） | 把查询交给 `server` 指定的服务器 |
| `reject` | 拒绝（`method: default` 返回 REFUSED，`drop` 直接丢弃）——广告拦截首选 |
| `predefined` | 直接返回预定义记录（`rcode`/`answer`/`ns`/`extra`），取代旧的 `rcode://` 服务器 |
| `route-options` | 只改选项（`disable_cache`/`rewrite_ttl`/`client_subnet`…）不改目标 |
| `evaluate` | 1.14+：向服务器发查询并**保存响应**，供后续规则用 `match_response` 匹配 |
| `respond` | 1.14+：返回前面 `evaluate` 保存的响应 |

匹配字段：`domain` / `domain_suffix` / `domain_keyword` / `domain_regex` / `rule_set` / `query_type`（`["A","AAAA","HTTPS"]`）/ `inbound` / `process_name` / `clash_mode` / `wifi_ssid` / `source_ip_cidr` …

**推荐"显式兜底 + `final`"两条都写**：`final` 只影响没命中规则的查询；显式兜底让"规则表"本身完整可读、便于排查。

### 4.5 `strategy`

| 值 | 行为 | 何时用 |
|---|---|---|
| `prefer_ipv4` | 同时查 A/AAAA，优先 IPv4 | **默认推荐**（IPv6 半通环境最稳） |
| `prefer_ipv6` | 优先 IPv6 | 确认 IPv6 通畅 |
| `ipv4_only` | 只解析 A | 路由器 IPv6 被关闭时（Open-Box 就这么做） |
| `ipv6_only` | 只解析 AAAA | 少见 |

**IPv6 是 sing-box 最常见的泄漏源**：TUN 只写 IPv4 地址但应用仍会 AAAA 并从 v6 直连出去。两件事一起做才干净：① `dns.strategy: "ipv4_only"`（或 TUN 同时给 v6 地址并接管 v6 路由）；② 路由器防火墙层拒绝 LAN→WAN 的 IPv6（Open-Box 的 `openbox_v6block`）。

### 4.6 `domain_resolver` 打破自举死循环（三种写法，优先级由高到低）

```jsonc
// ① 单个出站指定
{ "type": "socks", "tag": "p", "server": "my.vps.example.com", "server_port": 1080,
  "domain_resolver": "dns-direct" }               // 字符串 = 用哪个 DNS server

// ② 全局兜底（推荐）
"route": { "default_domain_resolver": "dns-direct" }

// ③ 详细形式，可带参数
"domain_resolver": { "server": "dns-direct", "strategy": "prefer_ipv4", "rewrite_ttl": 60 }
```

### 4.7 fakeip（结论：多设备 + 路由器透明代理场景**不建议用**）

- **是什么**：先给域名返回假 IP（`198.18.0.0/15`），不真解析；连接到达时用假 IP 反查域名再按域名分流。
- **代价**：必须严格配对 `action: "sniff"`；`198.18.x.x` 会被日志/统计/防火墙看错；部分应用（P2P、本地服务发现）异常；调试困难。
- **1.14 注意事项**：`type: "fakeip"` 的 server **不能放在 `servers[0]`**（也不能当默认 server），否则报 `default server cannot be fakeip`。正确写法：
  ```jsonc
  "servers": [
    { "type": "udp",    "tag": "dns-remote", "server": "1.1.1.1" },
    { "type": "fakeip", "tag": "fakeip", "inet4_range": "198.18.0.0/15" }
  ],
  "rules": [
    { "query_type": ["A", "AAAA"], "action": "route", "server": "fakeip" },
    { "action": "route", "server": "dns-remote" }
  ],
  "final": "dns-remote"
  ```
  并在 `experimental.cache_file` 里开 `store_fakeip: true` 持久化映射。
- Open-Box 明确把"不支持 fake-ip"列为**非目标**。

### 4.8 缓存相关（1.14 变化）

- `experimental.cache_file.enabled: true` + `store_dns: true` → 完整 DNS 缓存持久化，重启后仍有效。
- `store_rdrc` **已在 1.14 弃用**（计划 1.16 移除）→ 改 `store_dns`。
- `independent_cache` **已在 1.14 弃用**（缓存现在恒按传输分离），计划 1.16 移除 → **直接删掉这个字段**。
- `optimistic`（1.14+）：过期条目先返回、后台刷新，适合移动网络切换；与 `disable_cache`/`disable_expire` 冲突。

### 4.9 三种推荐 DNS 配置

**极简（只求能用）**
```jsonc
"dns": {
  "servers": [ { "type": "udp", "tag": "dns-direct", "server": "223.5.5.5" } ],
  "final": "dns-direct",
  "strategy": "prefer_ipv4"
}
```

**官方推荐的双通道 ★（主推方案）**
```jsonc
"dns": {
  "servers": [
    { "type": "udp",   "tag": "dns-direct", "server": "223.5.5.5" },
    { "type": "https", "tag": "dns-proxy",  "server": "1.1.1.1", "detour": "PROXY" }
  ],
  "rules": [
    { "rule_set": "geosite-cn",  "action": "route", "server": "dns-direct" },
    { "rule_set": "geosite-ads", "action": "reject" },
    { "action": "route", "server": "dns-proxy" }
  ],
  "final": "dns-proxy",
  "strategy": "prefer_ipv4"
}
```
配合 `route.default_domain_resolver: "dns-direct"`。国内域名 → 就近 CDN 快；其他 → DoH + `detour: PROXY` 抗污染；广告 → 直接 REFUSED。

**路由器 + dnsmasq 接管**
```jsonc
"inbounds": [
  { "type": "direct", "tag": "dns-in", "listen": "127.0.0.1", "listen_port": 7853 }
],
"route": {
  "rules": [
    { "action": "sniff" },
    // ⚠ 必须限定入站！否则 tun 里转发到 dns-in 的查询也会被劫持回 dns-in，形成自环超时
    { "inbound": ["dns-in"], "action": "hijack-dns" },
    { "protocol": "dns", "action": "hijack-dns" }
  ]
}
```
dnsmasq 侧：`uci set dhcp.@dnsmasq[0].noresolv=1` + `uci add_list dhcp.@dnsmasq[0].server='127.0.0.1#7853'`。
> 这条"限定 `inbound`"的坑是 Open-Box 源码注释里明确记录的实战教训。

---

## 5. 路由与规则集

### 5.1 `route` 段结构（1.14）

```jsonc
"route": {
  "rules": [],
  "rule_set": [],
  "final": "PROXY",
  "auto_detect_interface": true,      // Linux/Win/macOS
  "default_domain_resolver": "dns-direct",
  "default_http_client": "hc-direct", // 1.14+
  "find_process": false,
  "find_neighbor": false,             // 1.14+，Linux/macOS
  "dhcp_lease_files": [],             // 1.14+
  "default_interface": "",
  "default_mark": 0,                  // 仅 Linux
  "override_android_vpn": false       // 仅 Android
}
```

### 5.2 `route.rules[].action` 语义

**终结性动作（命中即停止评估）**

| action | 参数 | 作用 |
|---|---|---|
| `route`（默认） | `outbound`（必填） | 把连接交给指定出站 |
| `reject` | `method` / `no_drop` | TCP 回 RST、UDP 回 ICMP 不可达；`method: "drop"` 静默丢弃 |
| `bypass` | `outbound`（可省） | 1.13+，**仅 Linux + `auto_redirect`**：在内核层绕过 sing-box |
| `hijack-dns` | 无 | 把 DNS 查询劫持到 sing-box 的 DNS 模块 |

**非终结性动作**

| action | 参数 | 作用 |
|---|---|---|
| `sniff` | `sniffer` / `timeout` | 嗅探协议与域名，**不终结**（放最前）。默认最多等 300ms |
| `resolve` | `server` / `strategy`… | 把目标从域名解析成 IP（不终结） |
| `route-options` | `override_address` / `override_port` / `udp_timeout` / `tls_fragment`… | 只改路由选项，不改出站 |

> **`outbound` 不是"规则里的一个字段"，而是"动作（action）的一个参数"。** 1.11 起 `route.rules[]` 必须有 `action`。

**`reject` 与旧 `block`**：`{"type":"block"}` 出站已无语义（官方称 1.13 移除，1.14 实测仍能过 check 的空壳）→ **一律用 `action: "reject"`**，别写 block 出站。`{"type":"dns"}` 出站确实已硬失败。

### 5.3 匹配字段（要点）

- 域名类：`domain`（精确）/ `domain_suffix` / `domain_keyword` / `domain_regex` / `rule_set`。
  - **`domain_suffix` 在 1.9 改过行为**：值**以 `.` 开头**时匹配字面前缀；**不以 `.` 开头**时匹配 `domain` 或 `.+\.domain`（即 `example.com` 也能匹配 `a.example.com`）。
- 地址/端口：`ip_cidr`、`ip_is_private`、`source_ip_cidr`、`source_ip_is_private`、`port`、`port_range`（`"1000:2000"` / `":3000"` / `"4000:"`）、`source_port`、`source_port_range`、`ip_version`。
- 进程/用户/平台：`process_name` / `process_path` / `process_path_regex`（Linux/Win/macOS）；`user` / `user_id`（仅 Linux）；`package_name` / `package_name_regex`（Android，1.14 新增 regex）；`network_type` / `network_is_expensive` / `network_is_constrained`（Android/Apple 图形客户端）；`wifi_ssid` / `wifi_bssid`；`source_mac_address` / `source_hostname`（1.14+，需 `find_neighbor`）；`interface_address` / `default_interface_address`（1.13+）；`preferred_by`（1.13+）。
- 通用：`clash_mode`（匹配面板当前模式 `Rule`/`Global`/`Direct`）、`inbound`、`protocol`（嗅探出的 `tls`/`http`/`quic`/`dns`/`stun`/`bittorrent`…）、`client`（`chromium`/`safari`/`firefox`/`quic-go`…）、`network`（`tcp`/`udp`/`icmp`，1.13+ 支持 icmp）、`auth_user`。
- 逻辑组合：
  ```jsonc
  { "type": "logical", "mode": "and", "rules": [ /* 子规则 */ ], "invert": false,
    "action": "route", "outbound": "direct" }
  ```
  `mode: "and"` 或 `"or"`；任何规则都能 `invert: true` 反选。

### 5.4 匹配的"默认逻辑"（生成器理解优先级的关键）

普通规则的字段之间**不是简单 OR，而是分组 AND**：
```
(domain || domain_suffix || domain_keyword || domain_regex || rule_set 的域名部分 || ip_cidr || ip_is_private)
&& (port || port_range)
&& (source_ip_cidr || source_ip_is_private)
&& (source_port || source_port_range)
&& (其他字段: inbound / protocol / process_name / network_type / clash_mode …)
```
- **多个 `rule_set` 值之间是 OR**；但若引用的规则集内部只有一条"默认规则"且没有 `invert`，其字段会与外层规则**合并**（相当于把规则集里的条件平铺进来）；否则整体当作一个"其他字段"来 AND。
- **1.14 修正了这个合并语义**（官方称非破坏性），所以"以前能用但没搞懂为什么能用"的配置在 1.14 上分流结果可能变化。

### 5.5 规则顺序语义 + 推荐骨架

`rules` 是**线性扫描、命中终结性动作即停止**。

```jsonc
"rules": [
  { "action": "sniff" },                                    // ① 先嗅探，否则域名规则全废
  { "protocol": "dns", "action": "hijack-dns" },            // ② DNS 交给 DNS 模块
  { "ip_is_private": true, "action": "route", "outbound": "direct" }, // ③ 内网直连
  { "process_name": ["curl"], "action": "route", "outbound": "PROXY" },// ④ 特殊进程（可省）
  { "rule_set": "geosite-ads", "action": "reject" },        // ⑤ 广告
  { "clash_mode": "Direct", "action": "route", "outbound": "direct" }, // ⑥ 面板一键直连
  { "rule_set": "geosite-cn",  "action": "route", "outbound": "direct" },// ⑦ 国内直连
  { "clash_mode": "Global", "action": "route", "outbound": "PROXY" }    // ⑧ 面板一键全局
],
"final": "PROXY"
```

两条经验规则：
1. **域名规则排在 IP 规则之前**（sniff 补上域名之前若有 `ip_cidr` 先命中，域名规则永远看不到）。
2. **`reject` 尽量靠前**（但要在内网直连规则之后），能省一次连接尝试。

### 5.6 `rule_set` 定义（三种类型）

```jsonc
// ① inline（1.10+，直接写规则，不需要文件）——适合少量自定义规则
{ "type": "inline", "tag": "my-direct",
  "rules": [ { "domain_suffix": [".internal.corp"] } ] }

// ② local（本地文件）——面板/脚本生成配置时最推荐
{ "type": "local", "tag": "geosite-cn", "format": "binary",
  "path": "<rulesets-dir>/geosite-cn.srs" }

// ③ remote（远程下载）——个人自用最方便，需配合 cache_file
{ "type": "remote", "tag": "geosite-cn", "format": "binary",
  "url": "https://raw.githubusercontent.com/SagerNet/sing-geosite/rule-set/geosite-cn.srs",
  "update_interval": "1d",
  "http_client": "hc-direct" }
```

- `format`：`source`（JSON）或 `binary`（`.srs`）。路径/URL 以 `.json`/`.srs` 结尾时可省略，但**建议显式写**。
- **`sing-box check` 会真的打开本地 `.srs` 文件**：路径不存在直接 `FATAL: open .../geosite-cn.srs: no such file or directory`。所以生成/部署顺序固定为：**先补文件，再 check**。
- **`remote` 规则集只有在 `experimental.cache_file.enabled: true` 时才会被缓存**，否则每次启动重新下载。
- 1.14 起 `download_detour` **已弃用**（计划 1.16 移除）→ 改 `http_client`；同时新增 `initial_path`（首次启动先用本地文件，不阻塞启动）。
- 1.14 起"隐式默认 HTTP 客户端"已弃用 → 显式配 `http_clients` + `route.default_http_client`，否则每次启动有 WARN：
  `WARN implicit default HTTP client using default outbound for remote rule-sets is deprecated in sing-box 1.14.0 and will be removed in sing-box 1.16.0.`
  （源码：`experimental/deprecated/constants.go` 的 `OptionImplicitDefaultHTTPClient`，只在 `box.go:414` 那个 fallback 闭包里 `Report`；一旦 `route.default_http_client` 或 `http_clients[0]` 生效，`Manager.Start` 先解析出 `m.defaultTransport`，该闭包不会被执行，WARN 消失。1.16 移除后这条 fallback 路径会消失，届时**必须**显式配置。）
- **隐式客户端走 `route.final`，不是直连**：旧的 fallback 构造 `HTTPClientOptions{DefaultOutbound: true}`（`box.go:415-416`），最终落到 `NewDefaultOutboundDetour` → `outboundManager.Default()`；而 `Default()` 由 `outbound.NewManager(..., routeOptions.Final)` 选定（`box.go:214`），也就是**当前的 `route.final`（代理组）**。
  因此显式化时若把 detour 写成直连，等于**改变了行为**：大陆直连 `raw.githubusercontent.com` 通常失败，而远程规则集首次下载失败是
  `FATAL start service: initialize rule-set[N]: initial rule-set: ...`（**启动即失败**，不是降级）。本仓库生成器因此让该客户端保持 `detour: <traffic selector group>`，与 DNS `dns-proxy` 的 detour 语义一致。
- 另外：`http_clients[].detour` 不能写成空 `direct` outbound（`detour to an empty direct outbound makes no sense`，实测 FATAL）；要么省略 detour，要么指向代理组。

**编译自己的规则集**
```bash
# source 规则集
{ "version": 1, "rules": [ { "domain_suffix": [".cn"] }, { "ip_cidr": ["203.0.113.0/24"] } ] }
sing-box rule-set compile --output my.srs my.json
sing-box rule-set decompile my.srs
```
规则集文件格式版本目前是 **`1`，必须写**。空文件、只有 `{"version":1}` 而 rules 缺失都会出问题。

**`.srs` 数据源**

| 来源 | 内容 |
|---|---|
| `SagerNet/sing-geosite` | 官方 geosite 规则集（`geosite-cn.srs`、`geosite-ads.srs`…） |
| `SagerNet/sing-geoip` | 官方 geoip 规则集（`geoip-cn.srs`…） |
| `MetaCubeX/meta-rules-dat`（sing 分支） | 更全：`geosite-*` 1899 类、`geoip-*` 260 类 |
| 自建 | `sing-box rule-set compile` |

**Open-Box 的工程做法（值得抄）**：按前缀自动映射仓库（`geoip-` → SagerNet/sing-geoip，`geosite-` → SagerNet/sing-geosite）；下载时**把"HTTP 200 但内容为空"当失败**——否则会写出一个"文件存在但加载必炸"的 `.srs`，而且下次部署因为"文件已存在"永久跳过它。

### 5.7 `.srs` vs source json 取舍

- `.srs`（binary）：加载快、体积小、**最稳（不依赖启动时网络）**，需要 `format: "binary"`；`check` 会打开文件，故文件必须先就位。
- source json：可读、可手改，`format: "source"`；体积大、启动解析慢。
- `inline`：不需要文件，适合少量自定义规则；**生成器输出的自定义规则首选**。

### 5.8 DNS 劫持的两种模式

```jsonc
// 模式 A：全局劫持（路由器/桌面通用，TUN 场景推荐）
{ "protocol": "dns", "action": "hijack-dns" }

// 模式 B：只劫持指定入站（dnsmasq 接管场景必须这样写）
{ "inbound": ["dns-in"], "action": "hijack-dns" }
```
模式 B 的原因：dnsmasq 把上游指到 `dns-in` 后，TUN 里"到 dns-in 的转发查询"本身也是 DNS 流量，若用全局规则会被劫持回同一个 `dns-in`，形成**自环**，表现为解析超时。

### 5.9 调试分流的利器

- `GET /connections`（clash_api）能看到每条活跃连接的 `rule` / `rulePayload` / `chains`，直接告诉你命中第几条规则、走哪条链。
- **规则本地回放**：拿一个域名，先按 `route.rules` 本地回放一遍（首条命中即停），再真发一次请求对比 —— Open-Box 的"域名穿透/真实路由"就是这么实现的，比盲猜快得多。**这是生成器/调试器最值得内置的功能。**

### 5.10 数组字段的"整段替换"（生成器必须注意）

- 面板 / UCI / 订阅生成的配置往往是**深合并**：对象递归合并、**数组整体替换**（Open-Box 存储层就这么实现，理由：链路列表、规则列表做元素级合并时删一项要重写整个对象）。
- 启示：**合并两份配置时，`inbounds`、`route.rules`、`route.rule_set` 是覆盖不是追加。** 出现"我明明加了规则却不生效"，先怀疑这里。

---

## 6. 完整可跑配置骨架（已脱敏）

### 6.1 模板 A：桌面 / 路由器（macOS、ImmortalWrt arm64）——来自《sing-box 配置模板与实战》

订阅节点 + 自建 vless-reality 混用，统一挂在一个 `PROXY` selector 下。

```jsonc
{
  "$schema": "https://sing-box.sagernet.org/schema.json",

  // ── 日志 ─────────────────────────────────────────────────────
  "log": {
    "level": "info",        // 路由器上改 "warn" + output 指向文件
    "timestamp": true
  },

  // ── DNS：双通道（国内直连解析 / 需代理域名走代理解析）────────
  "dns": {
    "servers": [
      // 直连 DNS：写纯 IP，避免"解析 DNS 服务器自身"的自举问题
      { "type": "udp", "tag": "dns-direct", "server": "223.5.5.5" },
      // 代理 DNS：DoH + detour，保证查询报文本身也从代理出去（抗污染）
      {
        "type": "https",
        "tag": "dns-proxy",
        "server": "1.1.1.1",
        "detour": "PROXY",
        "domain_resolver": "dns-direct"   // 双保险：万一写成域名也有解析器
      }
    ],
    "rules": [
      { "rule_set": "geosite-cn",  "action": "route", "server": "dns-direct" },
      { "rule_set": "geosite-ads", "action": "reject" },
      { "action": "route", "server": "dns-proxy" }     // 兜底
    ],
    "final": "dns-proxy",
    "strategy": "prefer_ipv4"     // 路由器关 IPv6 时改 "ipv4_only"
  },

  // ── 入站：TUN 全量接管 ───────────────────────────────────────
  "inbounds": [
    {
      "type": "tun",
      "tag": "tun-in",
      "address": ["172.19.0.1/30"],       // 关 IPv6 就只留 v4
      "mtu": 9000,
      "auto_route": true,
      "strict_route": true,
      // ↓↓↓ 仅 Linux（路由器）有效；macOS 上必须删掉这两行，否则 check 直接失败
      "auto_redirect": true,
      "exclude_interface": ["br-lan", "pppoe-wan"]
    },
    {
      "type": "mixed",                    // 本机调试：curl -x socks5h://127.0.0.1:2080
      "tag": "mixed-in",
      "listen": "127.0.0.1",
      "listen_port": 2080
    }
  ],

  // ── 出站 ─────────────────────────────────────────────────────
  "outbounds": [
    // 总入口：面板上点一下就能在"自建 / 订阅自动 / 直连"之间切换
    {
      "type": "selector",
      "tag": "PROXY",
      "outbounds": ["自建-REALITY", "订阅-AUTO", "direct"],
      "default": "订阅-AUTO"
    },
    // 订阅节点自动择优（把订阅里的节点 tag 填进 outbounds）
    {
      "type": "urltest",
      "tag": "订阅-AUTO",
      "outbounds": ["<订阅节点1>", "<订阅节点2>"],
      "url": "https://www.gstatic.com/generate_204",
      "interval": "5m",
      "tolerance": 50
    },
    // 自建：VLESS + REALITY
    {
      "type": "vless",
      "tag": "自建-REALITY",
      "server": "<你的VPS域名或IP>",
      "server_port": 443,
      "uuid": "<UUID>",
      "flow": "xtls-rprx-vision",
      "tls": {
        "enabled": true,
        "server_name": "<REALITY 伪装域名，如 www.microsoft.com>",
        "utls": { "enabled": true, "fingerprint": "chrome" },  // ★ reality 强制要求
        "reality": {
          "enabled": true,
          "public_key": "<PUBLIC_KEY，服务端 reality-keypair 的公钥>",
          "short_id": "<SHORT_ID，0-8 位十六进制>"
        }
      }
    },
    // 直连兜底（几乎所有配置都需要）
    { "type": "direct", "tag": "direct" }
  ],

  // ── 路由 ─────────────────────────────────────────────────────
  "route": {
    "auto_detect_interface": true,           // ★ 桌面/路由器必开，否则 TUN 环
    "default_domain_resolver": "dns-direct", // ★ 解析代理服务器域名用直连 DNS
    "rule_set": [
      // 本地 .srs：最稳（不依赖启动时网络），Open-Box 就是这么做的
      { "type": "local", "tag": "geosite-cn",  "format": "binary",
        "path": "<rulesets-dir>/geosite-cn.srs" },
      { "type": "local", "tag": "geosite-ads", "format": "binary",
        "path": "<rulesets-dir>/geosite-ads.srs" }
    ],
    "rules": [
      { "action": "sniff" },                                              // ① 嗅探域名
      { "protocol": "dns", "action": "hijack-dns" },                      // ② DNS 回 DNS 模块
      { "ip_is_private": true, "action": "route", "outbound": "direct" }, // ③ 内网直连
      { "rule_set": "geosite-ads", "action": "reject" },                  // ④ 广告拦截
      { "clash_mode": "Direct", "action": "route", "outbound": "direct" },// ⑤ 面板一键直连
      { "rule_set": "geosite-cn",  "action": "route", "outbound": "direct" }, // ⑥ 国内直连
      { "clash_mode": "Global", "action": "route", "outbound": "PROXY" }  // ⑦ 面板一键全局
    ],
    "final": "PROXY"
  },

  // ── 实验性 ───────────────────────────────────────────────────
  "experimental": {
    "cache_file": { "enabled": true, "path": "cache.db" },
    "clash_api": {
      "external_controller": "127.0.0.1:9095",
      "secret": "<RANDOM_SECRET>",
      "default_mode": "Rule"
    }
  }
}
```

**macOS 版要删掉的两行**
```diff
 -      "auto_redirect": true,
-      "exclude_interface": ["br-lan", "pppoe-wan"]
```
理由：`auto_redirect` 只在 Linux 有效，macOS 上 `check` 会失败（`initialize auto-redirect: invalid argument`）；`exclude_interface` 也只在 Linux 有意义。

**部署位置建议**

| 平台 | 配置路径 | 规则集路径 | 启动 |
|---|---|---|---|
| macOS | `~/.config/sing-box/config.json` | 同目录 `rulesets/` | `sudo sing-box run -c ~/.config/sing-box/config.json` |
| ImmortalWrt | `/etc/sing-box/config.json` | `<rulesets-dir>/` | `procd`（`/etc/init.d/sing-box`）或面板托管 |

### 6.2 模板 B：远程规则集（不想手动下载 `.srs`）

```jsonc
"http_clients": [
  { "tag": "hc-direct", "detour": "direct" }   // 规则集下载走直连
],
"route": {
  "default_http_client": "hc-direct",
  "rule_set": [
    { "type": "remote", "tag": "geosite-cn", "format": "binary",
      "url": "https://raw.githubusercontent.com/SagerNet/sing-geosite/rule-set/geosite-cn.srs",
      "update_interval": "1d",
      "http_client": "hc-direct" },
    { "type": "remote", "tag": "geosite-ads", "format": "binary",
      "url": "https://raw.githubusercontent.com/SagerNet/sing-geosite/rule-set/geosite-ads.srs",
      "update_interval": "1d",
      "http_client": "hc-direct" }
  ]
}
```
**必须配 `experimental.cache_file.enabled: true`**，否则每次启动重新下载。

**远程规则集的鸡生蛋问题（重点坑）**：`raw.githubusercontent.com` 在国内经常连不上 → 启动时下载失败 → **整个 sing-box 起不来**。两种解法：
- 用 `remote` 但把下载走代理（`"http_client": { "detour": "PROXY" }`）；注意此时 `default_domain_resolver` 必须能解析 GitHub 域名，首次启动可能因"还没有可用代理"而失败。
- **更稳**：本地 `.srs`（模板 A 的做法），一次性下载好或用面板/脚本定时更新。

下载后**必须校验非空**（有些加速站返回 200 + 空文件）：
```bash
for f in *.srs; do printf '%s %s\n' "$(wc -c <"$f")" "$f"; done
```

### 6.3 最小可用版本（来自《配置模板与实战》§5）

```jsonc
{
  "log": { "level": "info" },
  "dns": {
    "servers": [
      { "type": "udp",   "tag": "dns-direct", "server": "223.5.5.5" },
      { "type": "https", "tag": "dns-proxy",  "server": "1.1.1.1", "detour": "PROXY" }
    ],
    "rules": [
      { "rule_set": "geosite-cn", "action": "route", "server": "dns-direct" },
      { "action": "route", "server": "dns-proxy" }
    ],
    "final": "dns-proxy",
    "strategy": "prefer_ipv4"
  },
  "inbounds": [
    { "type": "tun", "tag": "tun-in", "address": ["172.19.0.1/30"],
      "auto_route": true, "strict_route": true }
  ],
  "outbounds": [
    { "type": "selector", "tag": "PROXY", "outbounds": ["node1", "direct"] },
    { "type": "shadowsocks", "tag": "node1", "server": "203.0.113.10", "server_port": 8388,
      "method": "aes-128-gcm", "password": "<PASSWORD>" },
    { "type": "direct", "tag": "direct" }
  ],
  "route": {
    "auto_detect_interface": true,
    "default_domain_resolver": "dns-direct",
    "rule_set": [
      { "type": "local", "tag": "geosite-cn", "format": "binary", "path": "<rulesets-dir>/geosite-cn.srs" }
    ],
    "rules": [
      { "action": "sniff" },
      { "protocol": "dns", "action": "hijack-dns" },
      { "ip_is_private": true, "action": "route", "outbound": "direct" },
      { "rule_set": "geosite-cn", "action": "route", "outbound": "direct" }
    ],
    "final": "PROXY"
  }
}
```

### 6.4 最短骨架（来自《配置学习总纲》§二，30 行）

```jsonc
{
  "log": { "level": "info", "timestamp": true },
  "dns": {
    "servers": [
      { "type": "udp",   "tag": "dns-direct", "server": "223.5.5.5" },
      { "type": "https", "tag": "dns-proxy",  "server": "1.1.1.1", "detour": "PROXY" }
    ],
    "rules": [
      { "rule_set": "geosite-cn", "action": "route", "server": "dns-direct" },
      { "action": "route", "server": "dns-proxy" }
    ],
    "final": "dns-proxy",
    "strategy": "prefer_ipv4"
  },
  "inbounds": [
    { "type": "tun", "tag": "tun-in", "address": ["172.19.0.1/30"],
      "auto_route": true, "strict_route": true }
  ],
  "outbounds": [
    { "type": "selector", "tag": "PROXY", "outbounds": ["节点A", "direct"] },
    { "type": "direct", "tag": "direct" }
  ],
  "route": {
    "auto_detect_interface": true,
    "default_domain_resolver": "dns-direct",
    "rules": [
      { "action": "sniff" },
      { "protocol": "dns", "action": "hijack-dns" },
      { "ip_is_private": true, "action": "route", "outbound": "direct" },
      { "rule_set": "geosite-cn", "action": "route", "outbound": "direct" }
    ],
    "final": "PROXY"
  },
  "experimental": {
    "cache_file": { "enabled": true },
    "clash_api": { "external_controller": "127.0.0.1:9095", "secret": "<RANDOM_SECRET>" }
  }
}
```

### 6.5 顶层 14 段（1.14 完整版）

```jsonc
{
  "$schema": "https://sing-box.sagernet.org/schema.json",  // 可选，编辑器补全
  "log": {},                    // 日志
  "dns": {},                    // DNS（流水线 1）
  "ntp": {},                    // NTP 时钟（1.13+）
  "certificate": {},            // 根证书（1.12+）
  "certificate_providers": [],  // 证书提供者，ACME 等（1.14+）
  "http_clients": [],           // 统一 HTTP 客户端（1.14+，规则集下载用）
  "network_namespaces": [],     // 网络命名空间（1.14+，Linux）
  "endpoints": [],              // 端点：WireGuard / Tailscale / OpenVPN…
  "inbounds": [],               // 入站（接入层）
  "outbounds": [],              // 出站（出口层）
  "route": {},                  // 路由（流水线 2）
  "services": [],               // 服务：API 面板 / DERP / resolved…
  "experimental": {}            // 实验性：clash_api / cache_file / v2ray_api
}
```
> 数组字段在**只有一项时可以省略方括号**，但为可读性建议一律写数组。

### 6.6 协议最小字段集（从订阅/分享链接解析时要知道在填什么）

```jsonc
// ── Shadowsocks
{ "type": "shadowsocks", "tag": "ss", "server": "203.0.113.10", "server_port": 8388,
  "method": "aes-128-gcm", "password": "<PASSWORD>" }

// ── VMess
{ "type": "vmess", "tag": "vmess", "server": "203.0.113.10", "server_port": 443,
  "uuid": "<UUID>", "alter_id": 0, "security": "auto",
  "transport": { "type": "ws", "path": "/path", "headers": { "Host": "cdn.example.com" } },
  "tls": { "enabled": true, "server_name": "cdn.example.com" } }

// ── VLESS（自建主流）
{ "type": "vless", "tag": "vless", "server": "203.0.113.10", "server_port": 443,
  "uuid": "<UUID>", "flow": "xtls-rprx-vision",
  "tls": { "enabled": true, "server_name": "www.microsoft.com",
           "utls": { "enabled": true, "fingerprint": "chrome" },
           "reality": { "enabled": true,
                        "public_key": "<PUBLIC_KEY>", "short_id": "<SHORT_ID>" } } }

// ── Trojan
{ "type": "trojan", "tag": "trojan", "server": "203.0.113.10", "server_port": 443,
  "password": "<PASSWORD>", "tls": { "enabled": true, "server_name": "example.com" } }

// ── Hysteria2
{ "type": "hysteria2", "tag": "hy2", "server": "203.0.113.10", "server_port": 443,
  "password": "<PASSWORD>", "obfs": { "type": "salamander", "password": "<PASSWORD>" },
  "tls": { "enabled": true, "server_name": "example.com" } }

// ── TUIC
{ "type": "tuic", "tag": "tuic", "server": "203.0.113.10", "server_port": 443,
  "uuid": "<UUID>", "password": "<PASSWORD>", "congestion_control": "bbr",
  "tls": { "enabled": true, "server_name": "example.com" } }

// ── AnyTLS
{ "type": "anytls", "tag": "anytls", "server": "203.0.113.10", "server_port": 443,
  "password": "<PASSWORD>", "tls": { "enabled": true, "server_name": "example.com" } }
```

通用 `transport` 子对象：`ws`（`path`/`headers`）、`grpc`（`service_name`）、`http`（`host`）。
**`tcp` 不写 transport**；分享链接里的 `h2` 要归一成 `http`。

### 6.7 selector / urltest

```jsonc
{ "type": "selector", "tag": "PROXY",
  "outbounds": ["自建-REALITY", "订阅-AUTO", "direct"],
  "default": "订阅-AUTO",                    // 可选
  "interrupt_exist_connections": false },    // 切换时是否断开既有连接（默认 false）

{ "type": "urltest", "tag": "订阅-AUTO",
  "outbounds": ["香港-01", "日本-02"],
  "url": "https://www.gstatic.com/generate_204",
  "interval": "5m",              // 默认 3m
  "tolerance": 50,               // 毫秒；新节点快不足 50ms 不切换，防抖动
  "idle_timeout": "",
  "interrupt_exist_connections": false }
```

- **selector/urltest 是"指针"，不是节点**：`outbounds` 里必须写**真实存在的 tag**；写错 `check` 不报错，启动时报 `dependency[x] not found` 或 `missing tags`（成员全空）。
- **组不能嵌套出环**（A 含 B、B 含 A）——`check` 同样不校验。
- Clash 的 `fallback` 组类型在 sing-box **不存在**（`unknown outbound type: fallback`）。

### 6.8 TLS / uTLS / REALITY

```jsonc
"tls": {
  "enabled": true,
  "server_name": "www.microsoft.com",   // SNI + 证书校验名
  "alpn": ["h2", "http/1.1"],
  "insecure": false,                    // 自签证书才 true（等于关校验）
  "utls": { "enabled": true, "fingerprint": "chrome" },
  "reality": { "enabled": true, "public_key": "<PUBLIC_KEY>", "short_id": "<SHORT_ID>" },
  "fragment": false,                    // 1.12+ TLS 分片（抗 SNI 阻断，有性能代价）
  "record_fragment": false              // 更推荐的抗阻断方式
}
```

硬性约束（实测）：
- **REALITY 客户端必须同时启用 `utls`**，否则 `check` 直接失败：`FATAL: uTLS is required by reality client`。推荐 `fingerprint: "chrome"`。
- `reality.short_id` 是 0–8 位十六进制字符串；`private_key`/`public_key` 由 `sing-box generate reality-keypair` 生成。
- sing-box 官方**不推荐 uTLS**（"指纹可被识别、库缺乏维护"），但 REALITY 又强制要求 —— 协议侧依赖，非偏好。
- `insecure: true` 是**关闭证书校验**；机场节点常见的 `allowInsecure` / `skip-cert-verify` 映射到这里。
- **Trojan / AnyTLS 协议本身隐含 TLS**：解析分享链接时若 `security` 参数缺省，要按 `tls.enabled = true` 处理，否则会丢掉 `insecure`/`sni`/`alpn`/`fp` 全部参数（机场发的 anytls 基本都带 `insecure=1`）。**这是 Open-Box 在解析器里专门处理的坑。**

### 6.9 `endpoints`（WireGuard）

```jsonc
"endpoints": [
  {
    "type": "wireguard",
    "tag": "wg-node",
    "system": false,                        // false = 内置实现，不建内核接口
    "address": ["10.0.0.2/32"],
    "private_key": "<WIREGUARD_PRIVATE_KEY，标准 base64，32 字节>",
    "peers": [
      { "address": "wg.example.com", "port": 51820,
        "public_key": "<WIREGUARD_PUBLIC_KEY>",
        "allowed_ips": ["0.0.0.0/0", "::/0"],
        "persistent_keepalive_interval": 30 }
    ]
  }
]
```
- 密钥是**标准 base64**（不是 base64url），用 `sing-box generate wg-keypair` 生成。
- **endpoint 的 `tag` 可像出站一样被 selector/urltest 引用**，也可作为链式代理的前置或落地。
- `system: true` 时才创建真正的内核接口（需要 `name`、`mtu` 等）。

### 6.10 Clash → sing-box 字段映射表（生成器直接可用）

| Clash / Clash.Meta | sing-box 1.14 |
|---|---|
| `proxies:` | `outbounds:`（type 化节点） |
| `proxy-groups: type: select` | `{ "type": "selector" }` |
| `proxy-groups: type: url-test` | `{ "type": "urltest" }` |
| `proxy-groups: type: fallback` | **不存在**（`unknown outbound type: fallback`） |
| `rules: DOMAIN-SUFFIX,x,PROXY` | `route.rules: { domain_suffix, action: "route", outbound: "PROXY" }` |
| `rule-providers` | `route.rule_set`（`local` / `remote` / `inline`） |
| `dns: nameserver-policy` | `dns.rules` + `dns.servers` |
| `dns: fake-ip` | `type: "fakeip"` 的 DNS server（**不建议**） |
| `dialer-proxy` | `detour` |
| `geodata` / `GEOSITE,cn` | `rule_set: ["geosite-cn"]`（`.srs` 文件） |
| `skip-cert-verify` | `tls.insecure` |
| `ws-opts` / `grpc-opts` / `h2-opts` | 统一 `transport`（`h2` → `http`） |
| 分享链接 `pbk`/`sid`/`fp` | `reality.public_key` / `reality.short_id` / `utls.fingerprint` |

**输入格式自动识别规则（Open-Box 的实现，可作为 subconverter 的解析清单）**：
- JSON 含 `outbounds` → sing-box；含 `proxies:` → Clash YAML；
- 首行匹配 `ss://|ssr://|vmess://|vless://|trojan://|hysteria2://|hy2://|tuic://|anytls://` → 分享链接；
- 整段像 base64 且无格式标志 → 先拆一层 base64 信封再判断。

**要归一化的字段**：`h2` transport → `http`；Clash 的 `ws-opts/grpc-opts/h2-opts` → 统一 `transport`；`skip-cert-verify` → `tls.insecure`；`dialer-proxy` → `detour`；分享链接的 `pbk/sid/fp` → `reality.public_key/short_id/utls.fingerprint`；**reality 缺 `fp` 时强制补 `chrome`**（否则内核起不来）。
**要拒绝的**：带 `plugin` 的 shadowsocks 节点（需要插件才能用）。

---

## 7. 排错与校验

### 7.1 运行时命令行速查

```bash
sing-box version                      # 版本 + 构建标签
sing-box check -c config.json         # 校验配置（会真的打开本地 .srs！）
sing-box format -c config.json -w     # 格式化/规范化配置（写回）
sing-box run -c config.json           # 前台运行（排查时用这个，不用服务）
sing-box run -c config.json -D ./dir  # 指定工作目录（影响相对路径与 cache.db）
sing-box generate uuid                # 生成 UUID
sing-box generate reality-keypair
sing-box generate wg-keypair
sing-box generate ech-keypair
sing-box rule-set compile  -o x.srs x.json
sing-box rule-set decompile x.srs
sing-box schema -o schema.json        # 1.14 导出本机版本的 JSON Schema
sing-box geoip/geosite …              # 把自定义 GeoIP/Geosite 转成规则集
```

### 7.2 `check` **会**拦截的（结构性错误，实测）

| 配置 | 退出码 | 报错（截断） |
|---|---|---|
| 完整现代配置（DNS 双通道 + TUN + selector + rules） | 0 | — |
| 旧 DNS 写法 `"address": "1.1.1.1"` | 1 | `dns.servers[0]: legacy DNS server formats are deprecated in sing-box 1.12.0 and removed in sing-box 1.14.0` |
| 旧 `geosite` 规则项 | 1 | `parse rule[0]: geosite database is deprecated in sing-box 1.8.0 and removed in sing-box 1.12.0` |
| DoH 用域名（`server: "dns.google"`）且无 `domain_resolver` | 1 | `initialize DNS server[0]: missing domain resolver for domain server address` |
| 出站用域名 + 多个 DNS server 但无 `default_domain_resolver` | 1 | `missing route.default_domain_resolver or domain_resolver ... set ENABLE_DEPRECATED_MISSING_DOMAIN_RESOLVER=true` |
| 旧入站字段 `inbounds[0].sniff` | 1 | `legacy inbound fields are deprecated in sing-box 1.11.0 and removed in sing-box 1.13.0` |
| REALITY 但没有 `utls` | 1 | `initialize outbound[0]: uTLS is required by reality client` |
| 未知字段 | 1 | `inbounds[0].no_such_field: json: unknown field "no_such_field"` |
| 空成员的 selector | 1 | `initialize outbound[0]: missing tags` |
| macOS 上写 `auto_redirect` | 1 | `initialize inbound[0]: initialize auto-redirect: invalid argument` |
| 本地 `.srs` 文件不存在 | 1 | `open .../x.srs: no such file or directory` |

### 7.3 `check` **不会**拦截、只在启动时炸的（语义错误）★

| 配置 | `check` | `run` 启动日志 |
|---|---|---|
| `detour` 指向不存在的 tag | **0（通过）** | `FATAL: start service: dependency[does-not-exist] not found for outbound[land]` |
| `detour` 成环（`land → PROXY → land`） | **0（通过）** | `FATAL: start service: circular outbound dependency: PROXY -> land -> PROXY` |
| selector 组写成环 / 成员不存在 | 0（通过） | 启动时 FATAL（`dependency[...] not found`） |
| 给 selector/urltest 本身写 `detour` | 1 | `unknown field "detour"` |
| 节点的 `detour` 指向一个 selector | **0（通过）** | **正常启动**（与"落地不能是组"的常见说法不符） |
| DNS 规则引用只含 `ip_cidr` 的规则集、未设 `match_response` | **0（通过）** | 未复现文档所述"启动被拒"（**存疑**） |
| 缺 `hijack-dns` 规则 | 0（通过） | 能启动，但 DNS 绕过 sing-box（**静默分流失效**） |
| 缺 `action: "sniff"` | 0（通过） | 能启动，但**域名规则全部失效** |
| 代理服务器域名解析不了 | 0（通过） | 启动时连不上节点（运行期报错） |

**由此得出的工作纪律**：**改完配置必须跑一次前台 `sing-box run` 看启动日志，看到 `sing-box started` 才算通过。只看 `check` 等于没验证。**

> 注意：取退出码时**不要用 `cmd | head` 后的 `$?`**（那是管道最后一个命令的退出码），要直接取 `$?`。

### 7.4 故障速查表（按症状）

| 症状 | 最可能原因 | 怎么确认 | 修法 |
|---|---|---|---|
| 启动即 FATAL，日志有 `legacy ... removed` | 配置是旧语法 | 看 FATAL 文案里的版本号 | 按 §7.2 对照表迁移 |
| 启动即 FATAL `dependency[x] not found` | 出站/组引用了不存在的 tag | 搜配置里所有 tag | 补上缺失 tag 或改引用 |
| 启动即 FATAL `circular outbound dependency` | 组 ↔ 节点成环；或链式代理绕回自己 | 画依赖图 | 给落地建一个不含自己的前置组 |
| 启动即 FATAL `missing domain resolver` | DNS server 用了域名地址 | 看 `dns.servers` | 改成纯 IP，或加 `domain_resolver` |
| 启动即 FATAL `uTLS is required by reality client` | REALITY 没配 utls | 看 outbound 的 `tls` | 加 `"utls": { "enabled": true, "fingerprint": "chrome" }` |
| 启动即 FATAL `open .../x.srs: no such file` | 本地规则集缺失 | `ls` 路径 | 先下载 `.srs` 再 check（部署顺序：先文件后 check） |
| 能启动，全站打不开 | `auto_detect_interface` 没开导致 TUN 环 | 看日志有无网络/接口信息 | 桌面/路由器加 `route.auto_detect_interface: true` |
| 能启动，只有国内站打得开/只有国外站打得开 | 分流规则方向或 DNS 通道配反 | `debug` 日志看命中哪条规则 | 检查 `route.rules` 顺序与 `dns.rules` |
| 域名规则完全不生效（只有 IP 规则生效） | 缺 `action: "sniff"` | 日志里看有无 sniff 结果 | 在 `route.rules` 最前面加 `{ "action": "sniff" }` |
| DNS 结果异常/污染/分流与流量不一致 | 缺 `hijack-dns`，或 DNS 规则没兜底 | 用 `dig` 对比两台 DNS | 补 `{ "protocol": "dns", "action": "hijack-dns" }` 与 `dns.final` |
| 路由器上局域网设备 DNS 全断 | dnsmasq 上游指向了已不存在的入站端口 | 看 dnsmasq 配置与 sing-box 入站 | 切换 DNS 模式前先还原 dnsmasq；`dns-in` 端口要与 `add_list server=127.0.0.1#xxxx` 一致 |
| dnsmasq 接管后解析超时 | `hijack-dns` 没限定 `inbound`，形成自环 | 看 `route.rules` | 改成 `{ "inbound": ["dns-in"], "action": "hijack-dns" }` |
| IPv6 流量绕过代理（真 IP 泄漏） | TUN 只接管 v4，或 DNS 仍解析 AAAA | 访问 IPv6 测试站 | `dns.strategy: "ipv4_only"` + 防火墙拒绝 LAN→WAN v6 |
| 规则集每次启动都重新下载 | `experimental.cache_file.enabled` 没开 | 看启动日志 | 开 `cache_file`，或改成本地 `.srs` |
| 面板连不上 / `fetch failed` | `clash_api` 没开或崩了 | `curl 127.0.0.1:9095/version` | 检查 `experimental.clash_api` 与内核是否在跑 |
| 切换节点后既有连接不断 | `interrupt_exist_connections` 默认 false | — | 需要立刻断旧连接就设 `true` |

### 7.5 部署前检查表

```bash
# ① 版本对得上吗
<abs-path>/sing-box version

# ② 语法与结构
<abs-path>/sing-box check -c config.json && echo "结构 OK"

# ③ 本地规则集文件真的存在且非空
ls -l rulesets/*.srs && for f in rulesets/*.srs; do printf '%s %s\n' "$(wc -c <"$f")" "$f"; done
#   ⚠ 空文件（0 字节）必须重下——有些加速站会返回 200 + 空体

# ④ 前台启动，看真实启动日志（最关键一步）
sudo <abs-path>/sing-box run -c config.json 2>&1 | head -30
#   期望：看到 "sing-box started"；没有 FATAL / dependency / circular

# ⑤ 端到端验证
curl -sS -x socks5h://127.0.0.1:2080 https://www.example.com -o /dev/null -w '%{http_code}\n'
curl -sS https://ipinfo.io/ip            # 看出口 IP 是否符合预期
dig @127.0.0.1 example.com +short        # 若监听 53

# ⑥ 看每条连接命中哪条规则（clash_api）
curl -sS -H 'Authorization: Bearer <SECRET>' http://127.0.0.1:9095/connections \
  | jq '.connections[] | {host, rule, rulePayload, chains}'
```

**一条命令生成"能给别人看的证据包"**
```bash
{
  echo "## version"; <abs-path>/sing-box version
  echo "## check";   <abs-path>/sing-box check -c config.json; echo "exit=$?"
  echo "## run";     sudo <abs-path>/sing-box run -c config.json 2>&1 | head -30
} > /tmp/singbox-verify.txt
```

### 7.6 升级顺序（避免升完就断网）

1. 先读对应版本的 Migration 与 Deprecated 页。
2. 用**目标版本的二进制**跑 `sing-box check`（不要用旧版本 check 新配置，反之亦然）。
3. `sing-box format -w` 规范化一遍，能自动修掉一批格式问题。
4. 备份当前可用的 `config.json` + `cache.db` + 规则集目录。
5. 前台 `sing-box run` 观察启动日志，确认 `sing-box started` 且没有 FATAL，再切服务。
6. 保留回滚路径（Open-Box 的做法：面板放行规则永远不删，保证"面板打不开时还能进去恢复直连"）。

### 7.7 版本兼容性命令与权威资料

- `sing-box schema -o schema.json`（1.14）导出本机版本 JSON Schema —— **防旧语法最有效的工具**；顶层 `$schema` 指向 `https://sing-box.sagernet.org/schema.json`。
- 权威资料（按可信度）：官方文档 `/zh/`、Migration 迁移指南、Deprecated 废弃清单、Changelog、JSON Schema、GitHub Releases、`sing-geosite`/`sing-geoip`；工程实践参考：`immortalwrt/homeproxy`（OpenWrt UCI 前端）、<Open-Box 仓库>（`panel/server/engine/*.mjs` 是最好的"配置生成器"教材）、`MetaCubeX/meta-rules-dat`；中文社区教程**普遍停留在 1.11 语法**，只能学思路不能抄配置。
- ⚠️ 反例警示：`nikkinikki-org/OpenWrt-nikki` 名字像 sing-box 的 OpenWrt 前端，但运行的是 **Mihomo**，不是 sing-box。

### 7.8 Open-Box 的配置生成（生成器参考实现）

- **Open-Box** = 面向 OpenWrt 的一体化透明代理方案：一条安装脚本铺好 **sing-box 内核 + Node 面板 + LuCI 兜底页**；订阅、节点、分流规则、DNS 接管与防火墙改动全由面板托管，不要求手写配置。上游 <Open-Box 仓库>（MIT，面板 fork 自 AnGe-ClashBoard / zashboard）。
- 它在路由器上安装；配置生成器源码是 `panel/server/engine/*.mjs`（分析结论落在本专题各页）。**这是 learn sing-box 配置的最佳教材。**
- 关键工程决策（可直接借鉴）：
  1. **只生成 `type` 化新语法**；rule_set 落本地 `.srs`；生成时自己校验 `detour` 环。
  2. **`log.level` 硬编码为 `warn`**，运行态信息全部走 `clash_api` 而不是日志（避免刷爆 syslog / 写坏闪存）。
  3. `clash_api.secret` **独立存储、不随配置轮换**（轮换会导致已部署配置失联）。
  4. 存储层做深合并：**对象递归合并、数组整体替换**。
  5. 部署流程：**先补规则集文件，再跑 `sing-box check`**；`check` 失败后**逐节点单独塞进最小配置再 check**，用来定位是哪个节点有问题。
  6. 规则集下载按前缀自动映射仓库（`geoip-` → sing-geoip，`geosite-` → sing-geosite）；**把"HTTP 200 但内容为空"当失败**。
  7. DNS：**不支持 fake-ip** 被列为明确非目标；用 `hijack-dns` + dnsmasq 接管 + 防火墙重定向；dnsmasq 场景必须把 `hijack-dns` **限定到 `inbound: ["dns-in"]`**（源码注释记录的实战坑）。
  8. 内核不放系统 PATH，装在固定目录；**生成器不要覆盖系统 `sing-box`**。面板部署前做冲突检测（要求先停 OpenClash）。
  9. 面板的「代理 / 连接」页面全部经 `clash_api`（`127.0.0.1:9095`）取数；**内核没在跑时** `/api/controller/*` 返回 502 `{"message":"fetch failed"}`（`cause: ECONNREFUSED`）。
- Open-Box 的协议覆盖（可作为"该支持哪些协议"的清单）：shadowsocks / vmess / vless（含 REALITY）/ trojan / hysteria2 / tuic / anytls / wireguard。

### 7.9 Open-Box 自编译内核与 1.14 TCP DNS 回归（影响生成器的 DNS 选择）

- 内核标识 **`1.14.0-openbox-tcp1`**：**基于上游 sing-box v1.14.0 源码加一个补丁的兼容构建，不是 SagerNet 官方发布版本**。它随上游发布包装箱分发，`install.sh` 下载的包内 `bin/sing-box` 就是它（**不是装机后本机编译**）。
- **问题与根因**：升级到 1.14.0 后"首次打开网站、或空闲一段时间后访问网站"明显变慢。
  - 1.13.14：每次 TCP DNS 查询**单独建连、用完即关**。
  - 1.14.0：增加复用探测、多条查询**共用连接**。被代理链路**静默丢弃**的空闲 TCP 流不会发 EOF，复用它的下一次未命中缓存的查询就一直等应答，直到 DNS 超时。
- 上游对照实验（同一线路、同一解析器与查询，独立进程）：

  | 内核 | 空闲 32 秒后 |
  |---|---|
  | 官方 1.13.14 | 约 40 ms 返回 |
  | 官方 1.14.0 | 连续两次超过 3 秒查询期限 |
  | 打补丁的 1.14.0 | 约 39 / 40 ms 返回 |

- **补丁 `tcp-dns-short-connections.patch` 只改 `dns/transport/tcp.go` 的两个入口**（`Exchange` / `ExchangeAsync`），复用其原有的 `exchangeSingle`，把行为恢复成"每次查询单独建连"。
- **边界（上游明确保持不变）**：DNS 协议不变（仍是原 TCP DNS 服务器与端口，**不改为 DoH/DoT/UDP**）；查询仍经原策略的 `detour` 出口；规则顺序、解析器地址、FakeIP、旁路配置不变；正常 DNS 应答缓存保留；HTTP/QUIC 业务连接复用不变；构建保留上游全部默认功能（含 Naive），CGO + 静态 musl。
- **代价**：未命中缓存的 TCP DNS 查询不再共享连接，每次需新建一条 TCP 连接（上游未给量化数据）。
- 上游划的边界：只确认"空闲连接超时"这一条触发机制，**不代表所有节点/网络路径都会触发**，也不代表所有网页延迟都由它造成；补丁**不覆盖首次建连的失败模式**。
- 分发：内核随发布包分发（`meta.json` 的 `singboxVersion` 记为 `1.14.0-openbox-tcp1`），同时单独发布内核资产与源码/补丁包；发布包内 `bin/sing-box.BUILD-INFO.json` 记录源码、工具链、构建标签与哈希，是溯源入口。
- 本地复现（Go 1.26.1 / darwin-arm64 / `CGO_ENABLED=0`）：未打补丁的 v1.14.0 源码 `TestOpenBoxTCPDNSAfterSilentIdle` **失败**（`query 2 after idle: context deadline exceeded`）；打补丁后两个用例全通过。补丁对上游 v1.14.0 tar 包干净应用，无 hunk 偏移。

> **对 subconverter 的启示**：生成器在 `dns.servers` 里选 `type: "udp"`（直连通道）不受影响；但若为"需代理域名"通道选 `type: "tcp"`（明文 TCP DNS + `detour: PROXY`），在官方 1.14.0 上会撞上这个回归，**优先选 `type: "https"`（DoH）或 `type: "udp"`**，或提示用户使用带补丁的内核。

---

## 8. 不确定项 / 风险（笔记明确标注为未验证）

来源：《配置学习总纲》§五 + 《核对与排错清单》§1.4 + 《平台差异与配置复用》§6 + 《入站与出站》。**这些不要当成事实写进设计文档的"确定"部分。**

1. **`block` 出站是否真已移除**：官方 deprecated 页写"已在 1.13.0 移除"，但用官方 **1.14.0** 实测 `{"type":"block"}` **仍能通过 `check`**。→ 以实测为准：能用但已无语义，一律改用 `action: "reject"`。
2. **DNS 规则引用仅含 `ip_cidr` 的规则集且未设 `match_response`**：官方 migration 页称会在启动时被拒绝，但笔记构造的用例**没有报错**（只有 `ip_version`/`query_type` 与旧地址过滤字段混用才硬失败）。→ 触发条件比文档描述更窄，**标记为待复核**。
3. ~~**iOS 上 `stack` 该选 `gvisor` 还是 `mixed`**~~ → **已解决（2026-09，二进制实测）**：**两个都不能选**。官方 Apple 客户端内核不含 `with_gvisor`，`gvisor`/`mixed` 均启动即失败；只有 `system` 或省略可用。由于该选项在 1.15 废弃、1.17 移除，**统统一律省略**（见 §2.3 的 `stack` 结论）。
4. **Apple 客户端对"未实现字段"（`strict_route`、`include_uid` 等）是静默忽略还是报错**：官方文档未说明。→ 建议**直接不写**，不要赌它被忽略。
5. **`route_address_set` 在 Android 图形客户端会崩**（`DeadSystemException`），但文档**没写具体阈值**；不要在大规模路由场景依赖它。
6. **`store_fakeip` 在 1.14 的实际行为**（是否真的持久化了 fakeip 映射）→ 未确认。
7. **`rule_set` 的 `tag` 数组（多标签）语法细节** → 未确认。
8. **`ip_version` / `query_type` 与旧地址过滤字段混用时的确切报错形状** → 未确认。
9. **iOS App Store 上架状态**：官方 `clients/apple/` 页面与 changelog 说法存在时间差，**以 App Store 实际条目为准**。
10. **1.15 的版本漂移**：`stack` 新增 `go` 并成为默认；`endpoint_independent_nat` 失效 —— 生成器若面向未来版本需要预留。
11. **TCP DNS 回归的适用范围**（§7.9）：只解释"空闲连接超时"一条机制，不代表所有节点/网络路径都会触发；补丁不覆盖首次建连失败模式；上游未给代价量化；若 SagerNet 上游自行修掉连接复用问题，该兼容构建应可退役。

> 另注：本摘要在生成过程中发现原笔记含少量**运维环境标识**（局域网网关地址、内网/文档用 IP、构建机路径与内核哈希），已按要求统一脱敏为占位符或直接省略；**原笔记中未发现真实凭据（UUID/密码/私钥/short_id 均为 `<…>` 占位形式）**。
