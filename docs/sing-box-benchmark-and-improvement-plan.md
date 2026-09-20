# sing-box 内核配置与“节点订阅短链”门户全景对标与演进方案

> **文档版本**：v2.0（全景评审与实施路线图）  
> **文档定位**：内核生成逻辑与短链管理门户协同演进方案  
> **关联源码**：
> - 内核生成：`src/generator/config/singbox.cpp`、`singbox.h`、`ruleconvert.cpp`、`base/rules/`
> - 短链门户：`base/web/index.html`、`base/web/app.js`、`base/web/app.css`、`src/handler/shortlink_api.cpp`
> - 测试验证：`tests/singbox_golden.sh`、`tests/shortlink_api_smoke.sh`、`tests/shortlink_portal_actions_ui.cjs`
>
> **对标业界项目**：
> 1. [SagerNet/serenity](https://github.com/SagerNet/serenity)（sing-box 官方团队 Go 配置生成器，官方架构规范）
> 2. [Toperlock/sing-box-subscribe](https://github.com/Toperlock/sing-box-subscribe)（2.6k★ Python 模板生成器，社区主流 1.12/1.14 方案）
> 3. [sub-store-org/Sub-Store](https://github.com/sub-store-org/Sub-Store) + [xream/scripts](https://github.com/xream/scripts)（10k★ 生态级节点流处理与动态模板注入）
> 4. [youshandefeiyang/sub-web-modify](https://github.com/youshandefeiyang/sub-web-modify) / [subconverter-ng](https://github.com/Jungley8/subconverter-ng)（主流订阅转换 Web UI 交互标准）

---

## 一、 调研背景与对标概况

当前 subconverter 项目已完成了 sing-box 1.14 基础骨架的构建，包括各平台 Profile 参数化、链式代理（`detour` 依赖图检测与成环校验）、`dns_mode: hijack`、`http_clients` 显式客户端，以及基于 Cloudflare Access 鉴权的短链持久化和用量监控面板。

但在**真实复杂网络场景（如 Android 移动端）**与**复杂机场订阅数据（200+ 节点共用短 TTL 域名）**的运行中，暴露出内核启动慢、周期性断流、Google 推送挂死等痛点；同时，前端**“节点订阅短链”门户**目前停留在基础的“文本框粘贴 + 链接复制”层面，在客户端一键导入、节点高级筛选、链式代理可视化引导等方面与主流成熟方案存在明显代差。

---

## 二、 核心生成逻辑（后端）对标与缺陷剖析

### 1. DNS 架构设计：缺乏自举防死锁与容灾（最脆弱的一环）
* **现状缺陷**：
  * **单点 DoH**：非 CN 流量仅配置 Cloudflare `1.1.1.1` DoH。境外链路一旦抖动，全机非 CN 解析全灭。
  * **DoH 自举死锁**：DoH 的 `domain_resolver` 指向 `dns-direct`（AliDNS UDP 223.5.5.5）。在大陆许多运营商环境下，境外 DoH 域名的 UDP 查询常遭丢包或污染，导致 DoH 握手失败陷入死锁。
  * **未阻断 HTTPS (Type 65) 记录**：现代系统高频发起 SVCB/HTTPS 查询，在双通道分流下易导致握手延迟或降级超时。
* **对标标杆**：
  * **Hosts 预置锁定（Toperlock 方案）**：在 `dns.servers` 中定义 `type: "hosts"`，预置 `cloudflare-dns.com`（`104.16.248.249` 等）、`dns.google`（`8.8.8.8`）等直连 IP。DoH 的 `domain_resolver` 指向该 hosts，**实现零外部依赖完成 DoH 握手**。
  * **双 DoH 冗余**：同时配置 `cf-doh` 与 `gg-doh`，提供互备。
  * **拦截 HTTPS 查询**：在规则中配置 `{"query_type": "HTTPS", "action": "reject"}`，规避运营商劣质劫持。

### 2. 远程规则集的冷启动自举断流（严重设计冲突）
* **现状缺陷**：
  * **移动端禁用 `cache_file` 导致启动风暴**：Android Profile 按 D13 决策关闭了 `cache_file`。生成的 16 个 remote binary `.srs` 规则集每次冷启动都必须经代理从 GitHub 全量拉取。
  * **启动卡死 60 秒**：实测日志证实，启动前 60 秒被 3 条 60s 级规则集下载打满，导致所有业务 DNS、节点解析排队挂死。
  * **硬编码 GitHub 裸源**：URL 硬编码为 `raw.githubusercontent.com`，弱网或节点尚未握手时极易超时崩溃。
* **对标标杆**：
  * **Serenity 官方方案**：抽象 `GitHubRuleSetOptions`，支持 CDN 加速地址（如 `testingcf.jsdelivr.net/gh/...`）。
  * **Toperlock 方案**：默认内建反代镜像（如 `gh-proxy.com`），且移动端全量启用 `cache_file`（相对路径 `cache.db`）+ `store_dns: true`。

### 3. 缺乏应对“短 TTL 机场节点域名”的防抖机制
* **现状缺陷**：
  * 现网订阅 205 个节点共用 `xaienaye.owolist.cn`，TTL 仅 4~5 秒。sing-box 每发起新连接均需重新拨号，4 秒过期引发密集 DNS 风暴，21% 查询耗时 2~5 秒，导致全机队周期性卡顿。
* **对标标杆**：
  * 启用 `dns.independent_cache: true`，并在规则中配合 `rewrite_ttl: 300` 建立本地保护池，平抑机场短 TTL 带来的冲击。

### 4. 内置直连白名单误伤境外服务（可复现的超时 Bug）
* **现状缺陷**：
  * 引入的 `Unbreak.list` 将 Google 移动推送（`mtalk.google.com`、`alt*-mtalk.google.com`、`clientservices.googleapis.com`）强制放进 `DIRECT`。
  * 大陆移动网络直连 Google 5228 端口 100% 阻断，日志中连续产生 `dial tcp 173.194.40.188:5228: i/o timeout (5.0s)`，导致 GCM/FCM 推送瘫痪且频繁阻塞。
* **对标标杆**：
  * 标杆项目均将 `mtalk.google.com` 归入代理或独立 Google 分组，严禁在大陆环境下强行 DIRECT。

### 5. 出站策略组过于扁平，缺乏容灾与调优
* **现状缺陷**：
  * 仅有单一扁平的 `proxy` 与 `auto`，200+ 节点混在一起。
  * `auto` 缺失 `interrupt_exist_connections: false`，切换节点时强制掐断现有下载与长连接；缺失 `idle_timeout`，息屏后仍在无效测速。
* **对标标杆**：
  * 配置 `interrupt_exist_connections: false` 与 `idle_timeout: "30m"`；节点支持地区分组细分（HK/TW/JP/SG/US）。

---

## 三、 “节点订阅短链”门户（前端）对标与演进设计

当前页面依托原生轻量 Vanilla JS，安全架构扎实，但交互与功能上落后于现代化订阅转换面板。对标 Sub-Store / Sub-Web 进行如下演进设计：

### 1. 客户端一键唤起（Deep Link）与二维码导入（移动端体验跃升）
* **问题痛点**：手机/电脑端生成短链后，用户必须手动复制文本、切换客户端、新建订阅并粘贴，多步操作繁琐。
* **借鉴设计**：
  * **一键导入按钮（URL Scheme）**：
    * Clash / mihomo：`clash://install-config?url=${encodeURIComponent(shortUrl)}`
    * sing-box（SFA / SFM）：`sing-box://import-remote-profile?url=${encodeURIComponent(shortUrl)}`
  * **纯前端无依赖 SVG 二维码**：
    * 点击“二维码”按钮弹出轻量级 Modal，手机直接用客户端扫码导入，免去剪贴板流转。

### 2. 节点高级筛选与转换控制面板（解决无效节点过多）
* **问题痛点**：机场订阅往往包含“官网提示”、“剩余流量”、“到期时间”等无用节点，且用户往往只想提取港美日等常用节点，目前在 Web 端无法筛选。
* **借鉴设计**：
  * 增加**“高级配置”（可折叠面板）**：
    * **包含节点（Include）**：输入正则或关键词（如 `香港|日本|美国`）。
    * **排除节点（Exclude）**：输入排除正则。
    * **快捷清洗开关**：一键勾选“过滤提示节点”（自动注入排除 `.*(剩余|到期|过期|重置|官网|群|Traffic).*` 的规则）。
    * **Emoji 国旗标准化**：开关勾选直接联动后端 `emoji=true`，美化节点列表。

### 3. 链式代理（Detour / Dialer）可视化绑定引导
* **问题痛点**：当前后端拥有非常强大的链式代理与成环检测能力，但前端完全隐蔽，用户不知如何配置“前置中继 + 落地节点”。
* **借鉴设计**：
  * 在高级面板中提供“链式代理前置”开关：
    * 粘贴完链接后，前端自动解析/列出可用前置节点（或手动指定名称），并提供成环防范提示。
    * 让“国外专有住宅 IP 落地 + 机场高速中继”的需求能够在 UI 上直观落地。

### 4. 短链卡片透视与诊断信息（可观测性增强）
* **问题痛点**：短链列表仅显示行数和时间，节点是否有解析错误、协议分布如何完全黑盒。
* **借鉴设计**：
  * **节点结构胶囊标签（Badges）**：卡片上直观展示协议与地区构成（如 `Shadowsocks: 104`、`VLESS: 101`、`🇭🇰 20`、`🇯🇵 15`）。
  * **快照诊断提示**：若某个节点格式不合规或被丢弃，在卡片上给予黄色预警，点击可查看丢弃原因。

---

## 四、 方案全面审视与关键技术决策（Review & Decisions）

| 决策编号 | 决策议题 | 推荐结论与技术依据 | 潜在风险与缓解对策 |
| :--- | :--- | :--- | :--- |
| **DEC-01** | Android 是否开启 `cache_file` | **是**。配置相对路径 `cache.db`，由 SFA 沙盒管理。 | **风险**：若沙盒不可写会导致 FATAL。<br>**缓解**：实测表明移动端工作目录可写；且这是解决 60s 规则集自举阻塞的唯一工程解法。 |
| **DEC-02** | Google 推送直连规则处置 | **彻底移除** `mtalk.google.com` 与 `alt*-mtalk` 的强制 DIRECT。 | **风险**：占用代理少量长连接。<br>**缓解**：推送心跳数据极小，走代理彻底消除 5s 超时断流。 |
| **DEC-03** | DNS Hosts 预置与备用 DoH | 引入 `type: "hosts"` 预置 CF/Google IP，DoH 增加 Google 作为备用。 | **风险**：Google DoH 在境内直连不通。<br>**缓解**：走 `detour: proxy` 代理通道，互备容灾。 |
| **DEC-04** | 规则集 CDN 加速源选择 | 优先采用 `testingcf.jsdelivr.net/gh/` 或 `gh-proxy.com`，提供回退。 | **风险**：公共 CDN 可能偶发故障。<br>**缓解**：结合 `cache_file`，冷启动只需成功一次即持久化。 |
| **DEC-05** | 短链前端技术栈约束 | **坚决维持 Vanilla JS + 原生 CSS**，禁止引入庞大的 NPM 框架（如 Vue/React）或厚重第三方打包链。 | **风险**：手写 UI 需注意兼容性。<br>**缓解**：现有代码架构清晰，模块化轻量组件开发效率极高。 |

---

## 五、 实施计划路线图（待审核确认后执行）

整个实施计划划分为三个阶段，严格遵循**“先解痛点断流，再做功能增强，最后体验打磨”**的节奏：

```
┌────────────────────────────────────────────────────────┐
│  Phase 1: P0 应急断流修复与基础体验（核心痛点对症）     │
│  - 恢复 Android cache_file，终结 60s 启动风暴           │
│  - 剔除 Google mtalk 强行直连，修复推送与超时          │
│  - DNS 引入 hosts 预置与备用 DoH                       │
│  - 门户增加一键导入 Scheme (Clash/sing-box) 与纯前端二维码 │
└───────────────────────────┬────────────────────────────┘
                            │ 验证通过
┌───────────────────────────▼────────────────────────────┐
│  Phase 2: P1 架构健全化与高级控制（对标 Serenity）      │
│  - 规则集支持 CDN 镜像加速拉取                         │
│  - 短 TTL 节点域名防抖 (independent_cache / rewrite_ttl)│
│  - auto 组加入防切断连接与休眠超时参数                  │
│  - 门户增加高级选项折叠面板（节点过滤正则、Emoji、清洗）│
└───────────────────────────┬────────────────────────────┘
                            │ 验证通过
┌───────────────────────────▼────────────────────────────┐
│  Phase 3: P2 进阶治理与长效观测（对标 Sub-Store）       │
│  - 节点地区二级分组生成 (HK/TW/JP/SG/US)               │
│  - 短链列表卡片增加协议/地区透视胶囊标签               │
│  - 链式代理前端引导与成环可视化诊断                    │
└────────────────────────────────────────────────────────┘
```

### Phase 1 详细任务拆解（本阶段先执行此部分）：
1. **P1-1（内核）**：修改 `src/generator/config/singbox.cpp`，将 `android_profile.cache_file` 置为 `true`。
2. **P1-2（规则）**：修改 `base/rules/DivineEngine/Surge/Ruleset/Unbreak.list`，移除 `mtalk.google.com` 及 `alt1~8-mtalk` 直连规则。
3. **P1-3（DNS）**：在 `singbox.cpp` 的 `dns.servers` 中增加 `hosts` 类型解析器（预置 DoH 直连 IP），新增 Google DoH 备用通道，并增加 HTTPS(65) 查询拦截。
4. **P1-4（门户）**：在 `base/web/index.html` 与 `app.js` 中增加“一键导入到 Clash / sing-box”按钮，并集成轻量无外部依赖的纯 SVG/Canvas 订阅二维码弹窗。
5. **P1-5（测试）**：运行 `tests/singbox_golden.sh` 与 `tests/shortlink_portal_actions_ui.cjs`，确保回归测试与 UI 交互断言 100% 通过。

---

## 六、 审核确认门禁

请审阅上述整体方案与 Phase 1 实施计划。**在您给出明确的确认指示之前，本助手不会对任何业务代码发起修改**。

若您认为方案可行或有特定细节需要微调，请随时告知，我们将立即进入 Phase 1 的代码实现与验证流程！
