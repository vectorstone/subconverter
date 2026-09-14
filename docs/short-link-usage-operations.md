# 个人用量 v1 运维说明

## 组件与凭据

- `services/sui-usage-adapter/` 只读取 s-ui 本机详情 API，不提供任何节点写操作。
- `services/access-auth/` 在源站校验 Cloudflare Access JWT 的签名、issuer、audience、exp、nbf、应用类型和邮箱。
- `src/usage/` 每 30 秒通过 mTLS 采样；`/api/usage` 只读取当前账号的缓存。
- 管理员通过门户的“用量绑定”预览、建立、改名或撤销绑定。Client ID 是 s-ui 数字 ID，不是 Client name 或订阅密码。
- 既有用户身份、短链快照、短链创建额度不变。流量额度和有效期仍在 s-ui 中设置。

管理 Token 仅存在于 s-ui 主机 `/etc/sui-usage-adapter/sui-api-token`，root:root、0600。不要输出文件、把 Token 放进命令参数或提交仓库。

每个实例永久保留 `instance_id` 和 `identity.key`。HMAC 文件为 64 个十六进制字符，解码为 32 字节。误换 HMAC/instance_id 会令现有绑定失效。Client 改名、删除或身份变化也会失效，需撤销后重新预览绑定；改显示名称不能解除失效。

## 构建与验证

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
go -C services/sui-usage-adapter test -race ./...
go -C services/access-auth test -race ./...
go -C services/sui-usage-adapter build -o /tmp/sui-usage-adapter .
```

`tests/shortlink_usage_integration.py` 需要本机、空白、可丢弃的 PostgreSQL 数据库，通过 `DATABASE_URL` 传入连接。脚本启动真实 C++ 后端、Go 适配器、mTLS 和合成 s-ui API，覆盖普通用户隔离、管理员绑定、乐观锁、Origin、停用账号、过时/不可用缓存、身份变化、永久失效与短链快照不变；同时运行既有短链 Lite smoke。它不会访问生产 Token。

```bash
python3 tests/shortlink_usage_integration.py --binary build/subconverter --adapter /tmp/sui-usage-adapter
bash tests/usage_schema_smoke.sh
python3 tests/usage_schema_failure.py --binary build/subconverter
PLAYWRIGHT_PATH=/path/to/node_modules/playwright node tests/shortlink_usage_ui.cjs
git diff --check
```

UI 测试自启 loopback 服务，使用合成 API，覆盖 1440px/390px、超大整数、错误状态和管理员完整绑定流程。`--serve --port 25501` 可保留集成测试的合成账号页面供本机检查；不要把开发账号配置用于生产。

迁移故障测试同样需要新的空白本机数据库，故意创建不兼容的 usage 表，再验证用量初始化失败后共享连接已回滚、旧短链全流程仍可用。

## 部署顺序

1. 备份现有短链 Compose、环境文件和 PostgreSQL；记录旧镜像不可变 digest。确认数据库端口和容器入口只对内开放。
2. 在受限本地目录运行 `bash scripts/create_usage_pki.sh OUTPUT_DIRECTORY ADAPTER_IPV4`。CA 私钥保留在部署机，服务器只获得各自的叶子私钥、证书和 CA 公钥。
3. 在 s-ui 主机安装适配器到 `/usr/local/bin/sui-usage-adapter`，配置目录 `/etc/sui-usage-adapter/`。根据示例配置本机端口/路径、实例 UUID 和 HMAC。
4. 安装 `deploy/sui-usage-firewall.service`、适配器 systemd 单元及专用 nft 表。修改 firewall 示例的来源 IP 为短链主机实际出口 IP。只放行来源到 8443，不改现有 s-ui 管理端口规则。
5. 运行 `nft -c -f /etc/sui-usage-adapter/firewall.nft`、`systemd-analyze verify`，再 `systemctl enable --now sui-usage-adapter`。不重启 s-ui。
6. 在短链主机安装 Access 验证器，配置实际 issuer 与 Access 应用 AUD。启动后 `/healthz` 应为 204，无 JWT 的 `/verify` 应为 401。
7. 备份并安装 `deploy/nginx/hi.example.com.conf`，通过 `nginx -t` 后 reload。`/api/` 的邮件头只能来自验证器，显式 API Key/Bearer 仍由 C++ 验证。`SHORTLINK_TRUST_ACCESS_HEADER=true` 只可用于该受保护源站，禁止把后端端口暴露公网。
8. 把 `providers.json`、CA、客户端证书和私钥放在 `/opt/subconverter/usage/`。Provider JSON 合约见 `deploy/usage-providers.example.json`；私钥 0600。容器以只读方式挂载到 `/run/usage/`。
9. 使用 `docker-compose.usage.yml` 叠加到现有 Compose。`PUBLIC_BASE_URL` 必须等于实际门户 Origin，不能带末尾斜杠。先设置 `SHORTLINK_USAGE_ENABLED=false`，只更新 subconverter，检查 `/version` 和旧快照。
10. 设置 `SHORTLINK_USAGE_ENABLED=true` 并仅重建 subconverter。启动时增量创建 `002_usage.sql` 三表，不修改原表或 s-ui 数据。初始化失败时用量返回 503，短链仍工作。
11. 管理员逐个预览和绑定事先确认的账号/Client ID。核对缓存与 s-ui 的采样时间差。不要使用部署脚本替用户设置额度、期限或重置计数。

可用的环境配置：`SHORTLINK_USAGE_POLL_SECONDS=30`、`SHORTLINK_USAGE_FRESH_SECONDS=60`、`SHORTLINK_USAGE_STALE_SECONDS=900`、`SHORTLINK_USAGE_AUDIT_DAYS=90`。每用户最多 10 个有效绑定；单次适配查询最多 100 个 Client。

证书默认叶子有效期 1 年；提前至少 30 天检查并续期。换叶子证书时保留 CA 和客户端 URI SAN `spiffe://subconverter/usage-client`，重启适配器并重建短链容器以加载新证书。不要通过 `-k` 或关闭 TLS 验证解决证书故障。

## 状态解释

`volume=0` 显示无限额，`expiry=0` 显示未设置到期。待首次启用时显示“周期待开始”。停用账户仍能读取统计；剩余流量不是服务器总流量，也不是短链数量额度。

最后成功采样在 60 秒内且无最近错误时为 fresh；临时错误或超过 60 秒显示 stale 和采样时间；超过 900 秒时数值变为不可用，不伪装成 0。重置后的计数可下降，不在门户累计为终身流量。第一期没有历史曲线。

## 回滚

首先关闭 `SHORTLINK_USAGE_ENABLED` 并仅重建 subconverter；必要时恢复部署前记录的镜像 digest。保留新增数据库表与审计，不回滚 PostgreSQL 数据、不改节点计数、不触碰短链快照。

Access 验签是源站身份保护，回滚用量功能时应继续保留。只有验证器自身故障需要恢复 Nginx 时，才按独立备份处理，并先确认源站仍有等效身份验证。

若停用适配器，先 `systemctl disable --now sui-usage-adapter`，保留 nft 拒绝规则或单独关闭 8443；不要清空现有 nft 表。保留实例/HMAC/CA 的受限备份，以免重新启用时误换身份。

## 验证边界

本地 Go 竞态测试、C++ 构建、真实 PostgreSQL 集成和 Playwright 合成页面可验证代码行为。生产 API、证书、防火墙和缓存检查是部署时的时间点证据。真实 Cloudflare Access 浏览器登录与订阅客户端最终体验应单独记录，不能由合成 JWT 或本地截图替代。

## 部署验收记录（时间点，已去标识）

- 门户域名与具体账号已省略；上线后逐账号隔离、管理员只读自身、Client ID 注入拒绝均已验证。
- 当次查询中，被绑定的 Client 均为无限流量、未配置到期。未修改 s-ui 的 Client、计数、额度或期限。
- 补齐了尚未出现在门户中的指定邮箱对应的普通用户记录，既有管理员角色保留。未修改 Cloudflare Access 的允许登录策略。
- 适配器、专用防火墙和 Access 验证器已启用开机启动；跨服务器合法 mTLS 可读取全部已绑定 Client，无证书请求失败。源站伪造邮箱、无效 API Key 均为 401，真实 bootstrap 凭据通过源站验证。
- 发布前后的数据库快照与有效 HTTP 下载内容一致；PostgreSQL 容器 ID 不变，s-ui 启动时间不变。
- 本地通过 C++ 主程序/静态库构建、两个 Go 模块的 race/vet、真实 PostgreSQL 集成、DDL 故障隔离、Lite 短链 smoke、Playwright 1440/390 视口及真实后端浏览器改名/Origin 流程。

实际运行的构建镜像以不可变 digest 固定：

```text
subconverter-local@sha256:<IMAGE_DIGEST>
```

镜像保存在部署主机的私有镜像库，未发布到 Docker Hub。源码归档另存 SHA-256 校验值；镜像的 revision label 对应该归档，`/version` 保留基线构建标识。数据库回滚修复使用同一缓存 builder 增量编译，构建文件与源码归档一并保存。

生产叠加配置是部署主机上的 `docker-compose.override.yml`，采用 JSON（Compose 支持），固定 digest 和 `pull_policy: never`；常规 `docker compose` 自动加载它。停用用量时修改其中的 `SHORTLINK_USAGE_ENABLED`，仅重建 subconverter。完整回退时归档移走这个 override，再使用原 `docker-compose.yml`；原文件仍固定发布前的镜像 digest。

短链备份与验证记录位于短链主机的受限备份目录（`<SHORTLINK_HOST>:/path/to/backups/`），包含数据库 dump、原配置、源码归档、构建文件、快照校验和 `verification.json`。适配器配置备份位于 s-ui 主机的受限备份目录（`<SUI_HOST>:/path/to/backups/`），均限 root 访问。

CA 私钥仅保存在部署机的受限本地目录（`<DEPLOY_HOST>:/path/to/pki/`），未放到任何服务器或仓库。叶子证书按创建时设置的有效期到期，需提前续期。服务器临时传输目录已清理。
