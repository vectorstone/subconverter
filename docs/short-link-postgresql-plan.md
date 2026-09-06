# SubConverter 多用户短链与 Web UI 技术方案

状态：代码实现与 VPS/Cloudflare 部署完成

方案日期：2026-09-06

本文记录已批准方案、当前实现状态和部署结果。代码实现、镜像构建、VPS 容器、Nginx 和 Cloudflare Access 路径调整均已完成。

## 1. 目标

本方案把“Python 拼接长链接 + subconverter 转换”的流程升级为带 Web UI、多用户鉴权、PostgreSQL 持久化和短链管理的订阅转换服务。

首期目标：

- Web UI 输入多个节点链接或机场订阅链接。
- 复用当前 C++ 解析器和 Clash 生成器。
- 创建可过期、可撤销的短链。
- 短链可以直接用于 Clash Verge、mihomo/OpenClash 等客户端。
- 已有短链可以作为新的输入源，再叠加 SS、VLESS Reality、TUIC 或其他支持的机场订阅，生成新的短链。
- 多个用户之间相互隔离，每个用户只能管理自己的短链。
- 保留原有 /sub 长链接口作为兼容和故障备用路径。

首期不做：

- 不重写现有节点解析和规则生成核心。
- 不在 Web UI 首期开放 Surge、sing-box 等多目标配置。
- 不允许任意本地文件路径作为 Web UI 输入。
- 不使用 302 把短链跳转到长查询 URL。
- 不直接复用 sub2api 的业务表。

## 2. 当前状态与已发现边界

- 局域网样例 192.168.6.185:25500/version 正常返回 subconverter v0.9.0-cecfa0a backend。
- 样例服务根路径目前返回 404，尚无 Web UI。
- 当前服务可以识别 SS、VLESS Reality、TUIC 和 x-sc-underlying-proxy=dialer。
- base/pref.example.toml 已有 serve_file_root = web；加入 base/web 后可以随镜像提供静态页面。
- scripts/Dockerfile 会复制整个 base 目录，不需要 Node.js 运行时。
- 示例 SS 端口 65885 超过合法范围，当前服务会静默变成 349。新 API 应直接拒绝。
- TUIC 示例中 # 后面的 &insert=false 属于节点名称，不是转换参数。页面只提示，不擅自改写。
- Clash 输出使用 dialer-proxy，而 Clash 输入解析主要读取 underlying-proxy。实现阶段必须补齐这两个字段的往返兼容。

## 3. 短链模式决策

首期采用生成快照模式，而不是每次访问都递归转换。

创建短链 A 时：

1. 校验用户身份、配额和输入。
2. 使用当前转换核心生成 Clash YAML。
3. 加密保存原始链接、转换参数和最近一次生成的 YAML 快照。
4. 返回随机短码。

读取短链 A 时：

1. 根据短码读取 PostgreSQL 记录。
2. 检查是否过期或撤销。
3. 解密最近一次生成的 YAML。
4. 直接返回配置，不再次调用自身的 /sub。

这样可以支持：

短链 A + 新节点 → 短链 B → 短链 C

短链 B 会把短链 A 的 YAML 当作普通 Clash 订阅输入，解析后再与新节点合并。B 保存自己的快照，因此撤销 A 不会破坏已经创建的 B。

如果需要跟踪机场上游更新，提供“刷新短链”操作。刷新 A 只更新 A，不隐式修改 B。

当前服务的内部请求会带 SubConverter-Request: 1，并触发 Loop request detected。/s/<code> 只返回静态快照，因此可以对该路径做专门的循环检测豁免；不得让该路径递归调用转换器。

## 4. 总体架构

浏览器、Clash Verge 和 mihomo 通过 Cloudflare HTTPS 访问 Nginx。Nginx 将 Web UI、短链 API、短链读取和原有 /sub 转发到 subconverter Docker。subconverter 使用独立 PostgreSQL database 保存用户、API Key、短链和加密快照。

路由边界：

- /：Web UI，必须登录。
- /api/short-links：创建、列表、刷新、撤销，必须鉴权。
- /s/<code>：公开读取，短码本身作为 Bearer Credential。
- /sub：原有长链转换接口，保持兼容。
- /version：健康检查。

短链读取不使用 Cookie、登录态或自定义 header，否则 Clash Verge 等订阅客户端无法正常刷新。

## 5. HTTP 接口

### 5.1 创建短链

POST /api/short-links

请求字段：

- name：用户可读名称。
- target：首期固定为 clash。
- links：节点链接或机场订阅链接数组。
- expires_in：有效期，默认 30 天。

创建过程与原 Python 脚本保持一致：清理空行、去除首尾空格、使用 | 拼接、对完整字符串编码，再调用现有转换核心。

响应包含：

- 短链 ID。
- 短链名称。
- 节点数量。
- 状态。
- 过期时间。
- short_url。
- preview_url。
- download_url。

原始链接不放入响应日志，也不放入前端长期存储。

### 5.2 读取短链

GET /s/<code>

有效响应必须是 200，并直接返回 Clash YAML：

- Content-Type：text/yaml; charset=utf-8。
- Cache-Control：no-store。
- 保留 profile-update-interval 和 Subscription-UserInfo（如果存在）。
- 支持 HEAD。
- download=1 时增加 Content-Disposition。

短码不存在、已撤销或已过期时返回 410 Gone。

### 5.3 管理接口

- GET /api/short-links：只列出当前用户的短链。
- POST /api/short-links/<id>/refresh：重新生成指定短链快照。
- DELETE /api/short-links/<id>：撤销自己的短链。
- POST /api/keys：创建用户 API Key。
- DELETE /api/keys/<id>：撤销自己的 API Key。

管理员可以查看和管理全局记录。

## 6. 多用户鉴权

### 6.1 Web UI

首期使用 Cloudflare Access 保护 / 和 /api/*：

- 用户访问 Web UI 时先完成登录。
- Cloudflare Access 身份映射到 PostgreSQL users.external_subject。
- Nginx 清理外部伪造的身份 header，只信任 Cloudflare 或本机入口。
- Web UI 使用同源请求，不把管理员 Token 写入 localStorage。

如果以后需要脱离 Cloudflare Access，再增加 OIDC 或独立认证服务；不在 C++ 中临时实现明文密码登录。

### 6.2 API Key

自动化调用使用每个用户独立的 API Key，通过 X-API-Key 或 Authorization Bearer header 传递。数据库只保存 Key 的哈希值，支持过期、禁用和轮换。

### 6.3 api_access_token

当前 api_access_token 是配置项，不是自动发放的用户凭据。示例文件中的 password 只是占位值。

在新方案中它只用于：

- 系统初始化。
- 管理员维护。
- 数据迁移和紧急维护。

生产部署通过 Docker Secret 或 API_TOKEN 环境变量注入随机值。普通用户不共享该 Token。

### 6.4 管理员识别与管理

管理员身份有两条来源：

- Authorization: Bearer API_TOKEN：管理员 bootstrap 身份。
- SHORTLINK_ADMIN_SUBJECTS：逗号分隔的 Cloudflare Access 邮箱或 subject 白名单。

Access 用户首次访问 API 时会自动写入 shortlink_users；如果 subject 在白名单中，则角色为 admin。管理员也可以通过 PostgreSQL shortlink_users.role 持久化角色。

管理员调用 GET /api/short-links 时可以查看全部短链，调用 DELETE/refresh 接口时可以管理其他用户的短链。GET /api/admin/users 查看用户，POST /api/admin/users 使用 subject、email 和 role 字段新增、提升或降级用户。普通用户只能查看和修改自己的记录。

## 7. PostgreSQL 数据模型

建议使用独立 database、独立 role 和独立 migration 记录。表包括：

users：用户身份、邮箱、角色、状态、创建时间。

api_keys：用户 ID、Key 哈希、名称、创建时间、过期时间、撤销时间、最后使用时间。

short_links：所有者、随机短码、名称、目标格式、状态、来源密文、快照密文、内容类型、内容哈希、节点数量、创建/更新时间、过期/撤销时间、访问统计。

short_link_versions：短链刷新历史、版本号、快照密文、内容哈希和创建时间。

user_quotas：每用户最大有效短链、每小时创建次数、输入大小和节点数量限制。

短链记录保存原始来源和快照，但不保存明文凭据。节点密码、UUID、机场订阅地址和生成后的 YAML 都使用成熟 AEAD 库加密，密钥通过 Docker Secret 或环境变量注入。

## 8. 配额、限流和清理

推荐默认值：

- 每用户最多 100 条有效短链。
- 每小时最多创建 20 条。
- 单次最多 100 个节点。
- 单次请求体最多 64 KB。
- 默认有效期 30 天。
- 永久短链允许已鉴权用户创建，但仍受每用户有效短链配额、撤销和清理策略约束。

限流分三层：

- Cloudflare/Nginx 按 IP 限流。
- 应用层按用户和 API Key 限流。
- PostgreSQL 事务锁定配额记录，防止并发绕过额度。

超过配额返回 429 Too Many Requests。创建失败时事务整体回滚，不留下半成品。

过期或撤销的短链返回 410，并由后台清理任务删除密文和历史快照。保留短暂恢复窗口后再物理删除。

## 9. SSRF 和输入安全

允许机场 HTTPS 订阅作为输入，但必须：

- 保持 api_mode=true。
- 拒绝 file://、本地绝对路径和不支持的协议。
- 拒绝回环、私网、链路本地和本机元数据地址。
- 限制 DNS 解析结果，避免域名解析到内网。
- 继承现有下载大小、连接超时、总超时和重试限制。
- 首期不接受任意外部配置文件路径。

不支持的输入直接返回错误，不写入短链。

## 10. 链式短链兼容要求

实现阶段必须加入以下回归测试：

- A：SS + VLESS Reality + TUIC。
- B：A + 新 SS + 新 VLESS + 机场 HTTPS 订阅。
- C：B + 新节点。

验收 B、C 均能生成，A 的节点继续存在，Dialer 关系保持，A 撤销不影响 B，A 刷新不隐式修改 B。

Clash YAML 解析器要同时兼容 dialer-proxy 和 underlying-proxy。/s/<code> 在带 SubConverter-Request header 的内部读取中仍应返回快照，而不是 Loop request detected。

## 11. 代码组织

拟新增：

- src/handler/shortlink_api.h/.cpp
- src/storage/postgres_store.h/.cpp
- src/security/secretbox.h/.cpp
- base/web/index.html
- base/web/app.js
- base/web/app.css
- db/migrations/001_initial.sql
- tests/shortlink_api_smoke.sh

拟修改：

- src/main.cpp
- src/parser/subparser.cpp
- src/server/webserver_httplib.cpp
- CMakeLists.txt
- scripts/Dockerfile
- docker_compose.yml
- base/pref.example.ini
- base/pref.example.yml
- base/pref.example.toml
- README-cn.md
- README-docker.md

职责边界：

- shortlink_api：HTTP 路由、身份、配额和状态码。
- postgres_store：事务、查询和迁移。
- secretbox：敏感 payload 加解密。
- subconverter()：继续负责节点解析和配置生成。
- Web UI：输入、预览、复制、下载、刷新和撤销。

## 12. Docker、VPS 和 Cloudflare

生产环境使用两个 Docker 服务：

- subconverter：宿主机仅绑定 127.0.0.1:15052，容器内使用 25500。
- subconverter-postgres：仅 Docker 内网开放 5432，数据使用独立 volume。

关键配置：

- PUBLIC_BASE_URL=https://hi.nicetoken.win
- DATABASE_URL
- API_TOKEN
- SHORTLINK_ENCRYPTION_KEY
- SHORTLINK_ENABLED=true

镜像使用不可变 digest，保留旧镜像用于回滚。数据库发布前先备份。

为 hi.nicetoken.win 新增独立 Nginx server block，不覆盖现有 api.nicetoken.win。Cloudflare Access 只保护 Web UI 和 /api/*；/s/*、/sub、/version 必须设置为绕过登录，以便订阅客户端和健康检查访问。Nginx 对 /api、/s、/sub 分别应用限流和禁缓存策略。

截至 2026-09-06，VPS 已确认存在 api.nicetoken.win 的 Nginx 配置；hi.nicetoken.win 已指向该 VPS 并复用 *.nicetoken.win Origin Certificate。当前 Cloudflare Access 应用覆盖整个 hi.nicetoken.win，/s/*、/sub、/version 仍会被重定向到登录页，必须在 Cloudflare 中新增更具体的 Bypass 路径或调整应用范围。当前 ubuntu 用户已具备 root/sudo 权限。

## 13. 验证与验收

功能：未登录创建返回 401 或被 Cloudflare Access 拦截；用户不能管理他人的短链；超过配额返回 429；过期/撤销返回 410；有效短链返回 200 YAML；支持 HEAD 和下载文件名。

链式：A → B → C 能连续创建，Dialer 字段不丢失，循环检测不阻塞快照读取。

安全：日志不包含原始节点密码和完整订阅 URL；数据库 source/snapshot 不是明文；不能通过输入地址访问本机或私网；短码不可预测。

部署：Docker 构建、PostgreSQL migration、容器重启恢复、备份恢复、VPS 本机访问和 Cloudflare 域名端到端访问全部通过。

真实节点只用于手工验证，不写入仓库测试夹具。

## 14. 实施阶段

阶段 A：修复 Dialer 往返兼容、抽取转换入口、增加日志脱敏。

阶段 B：加入 PostgreSQL、migration、加密存储、快照和短链 API。

阶段 C：加入 Cloudflare Access 对接、API Key、配额、限流和 Web UI。

阶段 D：构建镜像，部署 PostgreSQL、Nginx、Cloudflare DNS/证书并完成端到端验收。

## 15. 回滚

- 镜像按 digest 固定，保留上一版本。
- migration 向前兼容，不直接删除旧字段。
- 发布前备份 PostgreSQL。
- 短链功能异常时可关闭 SHORTLINK_ENABLED，不影响原有 /sub。
- 保留原有长链接口作为临时替代方案。

## 16. 审核确认清单

- [ ] 使用 PostgreSQL 作为主存储。
- [ ] 使用快照型短链，以支持短链继续组合。
- [ ] /s/<code> 直接返回 YAML，不使用 302。
- [ ] Web UI 和短链管理 API 必须鉴权。
- [ ] 首期使用 Cloudflare Access 作为浏览器身份入口。
- [ ] 普通用户通过 Cloudflare Access 使用 Web UI；API Key 仅用于自动化调用，api_access_token 仅作管理员/初始化 Token。
- [ ] 支持过期、撤销、刷新和用户配额。
- [ ] 首期只生成 Clash YAML。
- [ ] 修复 dialer-proxy / underlying-proxy 往返兼容。
- [ ] 生产域名使用 hi.nicetoken.win，并单独配置 Nginx，不覆盖现有 api.nicetoken.win。
- [ ] 数据库和加密密钥通过 Docker Secret 或等价方式注入。

审核通过后才开始代码实现、数据库迁移、镜像构建和 VPS 部署。
