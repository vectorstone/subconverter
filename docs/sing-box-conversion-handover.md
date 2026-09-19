# sing-box 转换逻辑交接文档 — macOS 客户端排障结论

> **用途**：把一次 macOS 客户端「开启 sing-box 后 1–2 分钟无法访问」的端到端排障结论固化下来，
> 供后续迭代 `src/generator/config/singbox.cpp` 的转换逻辑时直接引用。
>
> **脱敏说明**：本仓库公开，本文所有真实主机名、域名、IP、绝对路径、节点名均替换为占位符
> （`<PROVIDER_DOMAIN>`、`<FRONT_IP>`、`<LANDING_IP>`、`<PROVIDER_HOST>` 等）。
> 对照真实值的映射留在私有记录里，不要写进本文件。

---

## 0. 一句话结论

转换逻辑没有把 sing-box 配到「起不来」，但在**流量入口的默认落地**、**urltest 成员集**、
**规则集缓存路径**三处存在缺陷，会让客户端在受限网络下表现不稳定；其中「入口默认落地」
已实证修复并端到端验证通过。

三处缺陷已于 2026-09-19 在生成器侧落地修复（参数名与结论修订见 §6、§10），
其中 §3.3 的**原因判定与修复方向经内核实测后被修正**，请以 §3.3 / §10 为准。

同时必须说清楚：**原始主诉「1–2 分钟无法访问」最终没有被完全归因到配置缺陷上** ——
排障后期发现 curl 三站全通（HTTP 200）而浏览器全挂，最终定位为**浏览器自身陈旧状态**。
详见 §5。

---

## 1. 环境与复现条件

| 项 | 值 |
|---|---|
| 客户端 | macOS 官方 GUI（沙箱 App，bundle id `io.nekohasekai.sfamt`，核心跑在 `Extension.appex` 的 packet-tunnel 里） |
| 对照客户端 | 本机 CLI 内核 `sing-box 1.14.1`（homebrew，非沙箱，`with_gvisor`） |
| 配置来源 | `/sub?target=singbox` 生成，平台 `macos` |
| 网络 | 受限链路（手机热点，`expensive, constrained`） |
| 主诉 | 开启后 YouTube / X / 百度**全部**无法访问，约 1–2 分钟后自行恢复 |
| 后续变体 | 改配置后变为：X 可访问、YouTube/百度不可访问（浏览器侧） |

---

## 2. 已排除的假设（**不要重复排查**）

| 假设 | 排除依据 |
|---|---|
| 规则集下载阻塞启动 | 每次启动都是 `sing-box started (0.42s)` / `(0.46s)`；`cache_file` 有缓存，启动窗口无下载 |
| TUN `mtu: 9000` 造成 PMTU 黑洞 | 改 1500 后无可归因改善；耗时双峰台阶消失发生在**把出口固定到自建节点**之后；curl 在 1500 下三站全通。**mtu 不是主因**，但 1500 是无害且更常规的取值 |
| DNS 故障 | DNS 全程健康：p50 ≈ 200ms、`NOERROR`、无超时；`inbound DNS packet` 正常计数 |
| TUN 接管导致旧连接全部失效 | 浏览器流量确实进入了 TUN，且大量请求亚秒完成 |
| 规则集加载失败导致规则不匹配 | 规则集在 1s 内加载完成，`match[N] rule_set=...` 正常命中 |
| 浏览器网络层不通 | curl 经同一 TUN 三站 9/9 HTTP 200，日志时间与 curl `time_total` 吻合到小数点后一位 |

---

## 3. 确认的缺陷（按证据强度排序）

### 3.1 `proxy` selector 未输出 `default`，流量入口被 urltest 支配 ★★★

**结构证据**
- 生成的配置里 `route.final = "proxy"`，`proxy` 是 `selector` 且**没有任何 `default`**，
  `outbounds[0] = "auto"`（`auto` 是 `urltest`）。
- `tests/singbox_golden.sh:280` **强制断言** `proxy` 的第一个成员必须是 `auto`。
- 生成器全程不输出 `default`：`grep -n '"default"' src/generator/config/singbox.cpp` 无命中。
  上游 Clash 模板没有 `default` 概念，sing-box 于是按「第一个成员」取隐式默认。

**行为证据**
- 一次 GUI 运行在 `t=6s` 出现：
  `ERROR [456777369 5.0s] connection: open connection to <Cloudflare IP>:443 using outbound/selector[proxy]: dial tcp <FRONT_IP>:443: i/o timeout`
  —— `proxy` 组的实际出口落在了某个具体节点上，而该节点当时不可达。
- `auto` 的 `interval` 为 `5m`、`tolerance` 为 `50`，即**每 5 分钟全量重测一次**，
  选择抖动窗口不只在启动时出现。

**影响**：流量入口的落地完全由「延迟优先」的 urltest 决定，启动收敛期与每 5 分钟的重测期内
都可能落到不可用节点，且**用户无法从配置层面指定一个稳定默认落地**。

**已实证的修复**：给 `proxy` 加显式 `default` 指向自建节点后，代理路径 214 次出站
**0 次拨号失败**，中位终态 0.88s。

> 注意：给 `proxy` 加 `default` **不会**破坏 `tests/singbox_golden.sh:280` ——
> 该断言只检查 `outbounds[0]`，不涉及 `default`。这使得「输出显式 default」是一个低风险改动。

---

### 3.2 `auto` 收录全部节点，启动即全量测速 ★★★

**实测数据**（`auto` 含 206 个成员时）
- 启动后 `auto` 对全部成员发起健康检查：**308 次 `www.gstatic.com:443` 探测，
  从 t=0 一直铺到 t=29s**（每 5 秒约 64 / 86 / 34 / 48 / 48 / 28 次）。
- 收窄到 8 个成员后：**9 次探测**，全部在 t≤1s 完成。

**影响**
- 受限链路（热点）下，启动瞬间数百个并发出站连接本身就是对链路的冲击。
- 收敛窗口（实测 ~29s）内 `auto` 的选择不稳定，叠加 3.1 直接暴露在用户路径上。
- `interval: 5m` 意味着这个全量扫描每 5 分钟重复一次。

---

### 3.3 `experimental.cache_file` 未输出 `path`，缓存位置不可控 ★★☆（原因判定已修订）

**证据**
- 生成的配置是 `{"cache_file": {"enabled": true}}`，**没有 `path`**。
- CLI 运行的日志里出现真实下载：
  `outbound/<FRONT_NODE>: outbound connection to raw.githubusercontent.com:443`
  —— 16 个远程规则集在启动时被真实重新下载。
- 对照：GUI 客户端自己维护的配置**显式写了** `cache_file.path`。

**复核修订（本机 sing-box 1.14.1 实测）**：缺 `path` **不等于缓存不落盘**。
内核按默认名 `cache.db` 落在**进程工作目录**；同一目录第二次运行 **0 次下载**（缓存有效），
换一个目录启动才会重下。因此真实缺陷是「**位置不可控**」而非「不缓存」：CLI 从别的目录启动、
或工作目录不可写时，每次启动都会重下全部规则集。

**同一次实测否掉了「输出绝对路径」这条修复方向**：

| 配置 | 结果 |
|---|---|
| 无 `path` | 正常启动，缓存落工作目录 |
| `"path": "cache.db"`（相对） | 正常，缓存可跨工作目录复用 |
| `"path": "~/x/cache.db"` | `FATAL start service: initialize cache-file: ... no such file or directory`（**`~` 不展开**） |
| `"path": "$HOME/x/cache.db"` | 同上（**`$HOME` 不展开**） |
| `"path": "/nonexistent/cache.db"` | 同上（**父目录必须已存在，否则内核起不来**） |

即：绝对路径一旦猜错，就是把「重下规则集」升级成「内核启动失败」。正确修复是显式写**相对**
文件名，把缓存钉在已知名字上而不改变解析位置（见 §6 #2）。

**影响**
- 位置不可控时每次启动重复下载约 16 个规则集；在受限链路上既是带宽浪费也是启动延迟。
- 更严重：**启动因此硬依赖代理可用**（见 3.4）。

---

### 3.4 远程规则集下载经由代理，构成启动期自举依赖 ★★★

**证据**
- 生成器刻意让规则集 HTTP 客户端带上 `detour: <traffic selector group>`（`hc-default`），
  并在注释里说明了原因：大陆直连 rule-set 主机通常失败，而**首次下载失败是 FATAL 启动错误**。
- 该设计的代价是：启动成功与否取决于 `proxy` 组当前选中的节点是否可用。
  结合 3.1（默认落地不确定）与 3.3（每次都重下），这是一个**不稳定的自举链**。

**影响**：默认落地节点不可用时，不是「降级运行」而是「整个内核起不来」。

---

### 3.5 `auto` 成员共用单一域名，启动竞态把整组标记为不可用 ★★

**证据**：两次 GUI 运行的**同一位置**都出现（时刻分别在启动后 0s）：

```
dns: lookup failed for <PROVIDER_DOMAIN>: (exchange6: use of closed network connection | exchange4: ...)
outbound/urltest[auto]: outbound <节点> unavailable: lookup <PROVIDER_DOMAIN>: ...
```

**恰好 7 条** —— 正是 `auto` 里挂在 `<PROVIDER_DOMAIN>` 上的 7 个成员
（第 8 个成员是自有 IP 节点，不受影响）。

**根因**：urltest 的首轮健康检查发出的 DNS 查询，在 DNS 传输尚未就绪时被关闭
（启动期竞态）。约 1 秒后探测成功、恢复正常。

**影响**：每次启动的最初一瞬，`auto` **整组不可用**；`default` 为 `auto` 的 selector
会落到下一个成员。这是「同一个配置两次运行表现不同」的直接来源。

**注意**：该现象**不是每次都复现**（最终验证运行中为 0 次），属时序相关偶发。

---

### 3.6 路由规则存在逐字重复 ★★ **（已修复）**

> 生成器已在 `finalize()` 阶段按规范化 JSON 去重（`dedupeRules()`），`tests/singbox_golden.sh`
> 也加了「任何规则不得与更早的规则逐字相同」的断言。复核时实跑产物为 `rules=21 duplicates=[]`，
> 本节保留作为历史记录。

- `route.rules[8]` 与 `[9]` **逐字相同**：`{"rule_set": ["geosite-category-ads-all"], "action": "reject"}`
- `route.rules[14]`、`[15]`、`[21]` **逐字相同**：`{"rule_set": ["geosite-cn"], "action": "route", "outbound": "DIRECT"}`

**影响**：白跑匹配、快照体积无谓增大（快照大小对短链有实际上限约束）。

---

### 3.7 macOS 客户端的 `process_name` 条件在沙箱下永远不匹配 ★★★（危害范围已修订）

**证据**（同一份配置，两种运行方式）

| 运行方式 | 日志 |
|---|---|
| GUI（沙箱 App） | `router: failed to search process: Not implemented`（每个连接一条） |
| CLI（非沙箱，root） | `router: found process path: /Applications/... , user: <USER>` —— **正常** |

**结论修正**：这不是「macOS 不支持」，而是**沙箱客户端不支持连接属主查询**。
受影响的是含 `process_name` 的规则（下载器 / BT 客户端直连），在 GUI 下静默失效。

**危害范围修订（复核实测）**：失效的不只是「进程匹配」这一条。sing-box 把 process 条件与**同一
规则内**的其他条件**硬 AND**（域名与 ip_cidr 共用一个 OR 组，process/user 属硬 AND 组），
而生成器把一个列表文件的所有行合并成**同一条**规则：

```json
{"process_name": ["aria2c", "Thunder", ...],
 "domain_keyword": ["aria2", "xunlei", "yunpan", ...],
 "action": "route", "outbound": "DIRECT"}
```

沙箱客户端解析不出进程 → 整条规则永不命中 → **同文件里的域名条件一起陪葬**，
下载器域名不再直连。修复方式是把 process 条件拆成**独立规则**（见 §6 #5），
这样解析不出进程的客户端只让那条独立规则空转，域名条件照常生效。

---

### 3.8 链式落地不可用会被入口设计放大（转换逻辑的间接影响）★

**观察**：链式落地节点（`ChainProxyExit` 暴露的落地）在某次运行中 10 条连接只有 1 条有终态，
p50 35.9s —— 该落地路径本身不可用。

**为什么和转换逻辑有关**：`proxy` 的成员列表里同时包含 `auto`、落地、以及各前置节点，
且没有 `default`（见 3.1）。当 `auto` 因 3.5 短暂不可用时，选择会落到落地节点，
而落地一旦不可用就会**静默地把全部代理流量带坏**。入口的默认选择越不确定，
这种放大效应越明显。

---

## 4. 已验证有效的配置改动

在生成产物上手工验证过、且端到端有效的改动：

| 改动 | 效果 |
|---|---|
| `proxy` 增加 `"default"` 指向自建节点 | 代理路径 214 次出站 **0 失败**，三站 curl 均 200 |
| `auto` 成员 206 → 8（自有节点 + 港/日/台少量） | 启动探测 **308 → 9 次**，收敛窗口基本消失 |
| `log.output` 指向 GUI 沙箱可写目录 | 首次拿到覆盖 t=0 的全量 debug 日志（见 §7） |
| TUN `mtu` 9000 → 1500 | 无可归因的改善，但取值更常规、无副作用，建议保留 1500 |

> 前三项已参数化落地（`singbox_default` / `singbox_auto_include` / `singbox_cache_path`，门户侧为
> 同名 `SHORTLINK_*` env），`mtu` 已统一为 1500；`log.output` 是排障手段，不进生成器。

**最终验证运行**（`mtu=1500` + 出口固定为自建节点）：

| 站点 | curl | 日志内对应连接 |
|---|---|---|
| baidu | 200 · 0.204 / 0.370 / 0.509s | DIRECT `connection finished` 0.3 / 0.4 / 0.5s |
| X | 200 · 1.185 / 1.255 / 1.524s | 代理 `upload finished` 1.2 / 1.3s |
| YouTube | 200 · 1.810 / 2.420 / 2.435s | 代理 `upload finished` 1.8 / 2.4 / 2.4s |

---

## 5. 必须如实记录的结论边界

排障后期出现反转，务必不要在新对话里过度归因：

1. **curl 三站全通（9/9 HTTP 200），浏览器三站全挂。**
   Safari 打开三站正常；随后 Chrome 也自行恢复正常。
   → 最终定位为**浏览器自身陈旧状态**（socket 池死连接 / DNS 负缓存 / 被标记为 broken 的 origin
   之类），与 sing-box 和转换逻辑无关。
2. **原始主诉「1–2 分钟无法访问」没有被严格归因到配置缺陷。**
   已确证的配置侧问题（urltest 启动收敛 ~29s、默认落地不确定、每 5 分钟重测）
   只能解释其中**数十秒**；剩下的大头更像是浏览器/系统层的陈旧状态。
   换句话说：§3 的缺陷都是**真实且值得修**的，但不要把主诉当作它们的必然结果。
3. **多次测试之间变量没有锁死**，导致两轮结论相反（一轮组选择在 `ssOracle`、
   一轮在自建节点），一度误判 MTU。**后续任何验证都必须一次只动一个变量**，
   并在日志里确认「实际出口节点」而不是相信配置文件。

---

## 6. 建议的转换逻辑改动（按性价比排序）

> **状态（2026-09-19）**：下表 1–5 已落地，参数名与最终实现写在「落地」列；
> 复核结论见 §10。真实节点名不进仓库，只写在服务器 `.env`。

| # | 改动 | 落点 | 落地 |
|---|---|---|---|
| 1 | 为 `proxy` 输出显式 `default`（指向自建/首选节点，而非 `auto`） | 组生成逻辑，参考 `sanitizeGroups()` 与 route/final 处理段 | ✅ `singbox_default`（URL 参数）/ `SHORTLINK_SINGBOX_DEFAULT`（门户 env）。**只在该值能解析到 `proxy` 的成员时才输出**：`tests/singbox_golden.sh:280` 只断言 `outbounds[0]`，加 `default` 不破坏它；解析不到则退化为不输出（内核在 `check` 阶段放过悬空 `default`、却在运行时 `FATAL`，所以不能瞎写） |
| 2 | 输出 `experimental.cache_file.path` | 生成 `experimental` 的段落 | ✅ 输出**相对**文件名 `cache.db`（openwrt 保留其部署绝对路径）。**不要写绝对路径**：`~`/`$HOME` 不展开、父目录不存在即 `FATAL`（见 §3.3 实测表） |
| 3 | 收窄 `auto` 成员，或让 `auto` 只收录自有节点 | 组生成逻辑 | ✅ `singbox_auto_include`（URL 参数，备注关键词逗号分隔，子串不区分大小写）/ `SHORTLINK_SINGBOX_AUTO_INCLUDE`（门户 env）。只收窄 `auto`；`ChainProxyEntry`/`proxy`/`GLOBAL` 不动（selector 不做健康检查）；无命中时回退全量并告警 |
| 4 | 去重 `route.rules` | 规则装配段 | ✅ 已在 `dedupeRules()` 完成，并有 golden 断言（见 §3.6） |
| 5 | macOS 平台过滤掉 `process_name` 条件 | 平台化规则生成 | ✅ 改为**拆分成独立 rule**（而非丢弃）：保留 macOS CLI 的进程匹配能力，同时让沙箱客户端只空转那一条，不再连带废掉同文件的域名条件（见 §3.7） |
| 6 | 重新评估「远程规则集下载经由代理」的自举依赖 | `http_clients` / `default_http_client` 生成段 | ⏸ 保留现状：直连规则集主机在大陆会失败，而首次下载失败是 FATAL，显式 `detour` 是更小的问题；#1 落地后 detour 指向的组已有稳定默认 |

**另外两处随本轮一并修正**（文档初版未列）：

- `GLOBAL` selector 隐式默认是第一个成员 `DIRECT`，而它正是 clash_api「Global 模式」的落点 ——
  切到 Global 等于全部直连。现显式输出 `"default": "proxy"`。
- TUN `mtu` 统一为 `1500`（原桌面 9000 / 移动 8500），与 §4 的实测建议一致。

---

## 7. 运维知识：如何抓到 GUI 客户端的全量日志

排障过程中最耗时的环节，记录下来避免重复踩坑。

**问题**：GUI 客户端是**沙箱 App**（`com.apple.security.app-sandbox`），
核心跑在其 `Extension.appex` 内。日志由该扩展进程打开，因此：

- 只能写入**它自己的容器**或 **App Group 容器**。写 `~/.config/...` 等外部路径会
  `operation not permitted`，且会**导致整个内核启动失败**（不是降级）。
- 若该路径是**软链接**（例如 `~/.config/sing-box` 指向 dotfiles 目录），沙箱会因
  解析后落在白名单外而拒绝。
- **设置 `output` 后控制台输出被关闭**（官方文档原文："Will not write log to console after enable."），
  因此 GUI 自带的日志视图会变空 —— 这是预期行为，不是故障。
- 相对路径按**核心进程的 CWD** 解析，而 CWD 是容器内目录，不是 `$HOME`，所以不可预测。

**可行做法**：把 `log.output` 指向 App Group 容器内的绝对路径（通过 `codesign -d --entitlements -`
可查到该 App 的 `com.apple.security.application-groups`），例如
`<APP_GROUP_CONTAINER>/singbox-debug.log`。

**读取方式**：终端默认**没有权限**读容器内部（TCC），`find` 会**静默返回空**。
需用 Finder（`⌘⇧G`）或给终端 App 授予「完全磁盘访问权限」后**重启终端**。

**其它**：日志文件是**追加**写，多次运行会拼在一个文件里；
分析时需按 `sing-box started` 行切分。带时间戳的文件格式为
`+0800 YYYY-MM-DD HH:MM:SS LEVEL msg`（非 TTY 输出），终端格式则是 `LEVEL[NNNN]`（相对秒）。

---

## 8. 证据文件清单

均在 `temp/`（已 gitignore，不入库）：

| 文件 | 内容 |
|---|---|
| `temp/singbox-debug.log` | CLI 运行（自建节点直连出口，488 次完成）—— 健康的对照组 |
| `temp/singbox-debug-2609191755.log` | 两次 GUI 运行（一次走链式落地、一次走自建节点）—— 对比样本 |
| `temp/singbox-debug-2609171742.log` | 单次 GUI 运行，含 `handshake timeout` |
| `.lody/attachments/file-c60-0-singbox-debug-2609191807.log` | 三次运行拼接；含「改了 mtu 但组选择同时回退」的混淆样本 |
| `.lody/attachments/file-7ca-0-singbox-debug-mtu1500-proxy-vlessreality-2609191821.log` | **最终验证运行**：mtu=1500 + 出口固定，三站 curl 全通 |
| `temp/custom-singbox-260918-macos-2.json` | 客户端实际使用的那份配置 |

**分析时注意**：日志里的 `reference: outbound/... is unreferenced, closing idle connections`
是 sing-box 关闭非当前选择路径上的空闲连接，属噪音（在某次运行中占全部行数的 22%）。

---

## 9. 复现与验证方法

```bash
# 1) 抓日志（GUI 客户端）：确认 log.output 指向沙箱可写目录，删旧文件后启动
# 2) 关键：确认「实际出口节点」而不是相信配置文件 —— 看日志里该 outbound 拨向的目标
#    出口节点应拨向真实目标 IP；若只拨向另一个节点，说明它只是链式前置
# 3) 绕过浏览器做定量验证（curl 走 TUN，日志里能一一对上）
for i in 1 2 3; do curl -sS -o /dev/null -w '%{http_code} %{time_total}s\n' --max-time 40 <URL>; done
# 4) 浏览器异常但 curl 正常时，先换 Safari 或全新 Chrome profile，再查系统代理
scutil --proxy
networksetup -getwebproxy Wi-Fi
```

**单变量原则**：任何验证都必须一次只改一个变量，并在日志中确认实际生效状态。
本次排障中「同时改了 mtu 和组选择」直接导致一轮结论作废。

---

## 10. 修订记录（生成器侧复核与落地）

复核方式：读生成器源码 + 跑 `tests/singbox_golden.sh` + 用本机 `sing-box 1.14.1`
在临时目录构造最小配置做内核实测。

### 10.1 结论修订

| 条目 | 文档初版结论 | 复核结论 | 依据 |
|---|---|---|---|
| §3.1 `proxy` 无 `default` | 属实 | **属实**，已修 | 生成器全文件无 `"default"`；实跑 `proxy.default=None`、`route.final=proxy` |
| §3.2 `auto` 全量成员 | 属实 | **属实**，已修 | `auto` 由「除落地外全部节点」构造；`interval=5m`、`tolerance=50` |
| §3.3 `cache_file` 缺 `path` | 「缓存不落盘」 | **原因修正**：缓存落工作目录、位置不可控才是缺陷；且**绝对路径不可用**（实测 FATAL） | 见 §3.3 实测表 |
| §3.4 规则集经代理下载 | 缺陷 | **设计取舍**，保留 | 直连在大陆会失败，首次下载失败 FATAL |
| §3.5 DNS 竞态致整组不可用 | 属实 | **属实（内核侧）**，靠 §3.1 的显式 `default` 缓解 | 生成器无法直接修 |
| §3.6 `route.rules` 重复 | 属实 | **已修复**（复核前已由 `dedupeRules()` 落地） | 实跑 `rules=21 duplicates=[]` |
| §3.7 沙箱下 `process_name` 失效 | 「受影响的是含 process 的规则」 | **危害更大**：跨族硬 AND 会连带废掉同文件的域名条件；已改为拆分成独立 rule | 实跑产物第 19 条同时含 `process_name` 与 `domain_keyword` |
| §3.8 落地不可用被放大 | 属实（间接） | **属实（§3.1 的推论）**，已随 §3.1 消解 | — |
| （新增）`GLOBAL` 隐式默认 | 未列 | **新缺陷**，已修 | GLOBAL 第一个成员是 `DIRECT`，而它是 clash_api「Global 模式」的落点 |

### 10.2 新增的生成器开关

| 参数（`/sub`） | 门户 env | 语义 |
|---|---|---|
| `singbox_default` | `SHORTLINK_SINGBOX_DEFAULT` | 流量入口 selector 的默认落地：组 tag 或节点备注（精确匹配优先，其次不区分大小写的唯一子串）。解析不到或命中多个 → 不输出并告警 |
| `singbox_auto_include` | `SHORTLINK_SINGBOX_AUTO_INCLUDE` | 逗号分隔的备注关键词，只把这些节点放进 `auto`（health check 只发生在该组）。无命中 → 回退全量并告警 |

**注意**：子串匹配不区分大小写，`SS` 这类短关键词会命中 `VLESS-REALITY`（含 `ss`）。
请用足够独特的关键词，并在门户「刷新配置」后用客户端确认实际成员。

### 10.3 未做与待办

- §3.4 的「规则集下载经代理」保留现状，不引入新的自举风险。
- 沙箱客户端下 process 独立规则仍会每连接产生一条 `failed to search process: Not implemented`
  日志（只是不再影响其他条件），未额外处理。
- 真实节点备注/默认落地只写在服务器 `.env`，仓库内一律用占位符。
