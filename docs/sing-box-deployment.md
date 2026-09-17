# sing-box 转换迭代 —— 部署与验证 Runbook

> 配套方案：`docs/sing-box-conversion-plan.md`（D1~D13）。
> **本文件必须保持脱敏**：真实主机、域名、密钥只写在服务器上的 `.env`，不得进入仓库。
> 提交前跑 `scripts/check-sensitive.sh --all`。

---

## 1. 组件

| 组件 | 路径 / 说明 |
|---|---|
| 镜像构建 | `scripts/Dockerfile`（Alpine 多阶段：编译 + 运行） |
| 短链栈编排 | `docker-compose.shortlink.yml`（subconverter + PostgreSQL） |
| 反向代理 | `deploy/nginx/*.conf`（宿主 nginx，Cloudflare 证书）。三个文件必须一起装：`50-subconverter-limit.conf` 与 `20-cloudflare-realip.conf` 是 http 上下文配置，`hi.example.com.conf` 是 vhost；步骤与限流语义见 `docs/short-link-usage-operations.md` 的部署顺序第 7 步与「反向代理、限流与真实 IP」 |
| 环境变量样例 | `deploy/shortlink.env.example` |
| 规则集同步 | `scripts/sync_singbox_rulesets.sh` |
| 生成器回归 | `tests/singbox_golden.sh` |
| 短链 API 冒烟 | `tests/shortlink_api_smoke.sh`（含 sing-box 用例） |

---

## 2. 构建镜像

在装有 Docker 的构建机上（本地无 Docker 时可直接在目标宿主机构建）：

```bash
rsync -az --delete \
  --exclude '.git' --exclude 'build' --exclude 'build-usage*' --exclude 'temp' --exclude '*.log' \
  ./ <BUILD_HOST>:/tmp/subconverter-build/

ssh <BUILD_HOST> 'cd /tmp/subconverter-build && \
  sudo docker build --build-arg THREADS=4 -f scripts/Dockerfile -t subconverter:<TAG> .'
```

> `scripts/Dockerfile` 会执行 `scripts/update_rules.py`，构建期需要能访问规则数据源；
> 完全离线时请先把 `base/rules` 备齐或改用带缓存的构建。

---

## 3. test 环境部署

```bash
ssh <TEST_HOST> 'mkdir -p /opt/subconverter-deploy && cd /opt/subconverter-deploy'
# 上传 docker-compose.shortlink.yml 与本仓库 base/ 之外的配置文件不需要（镜像已内置 base）
scp docker-compose.shortlink.yml <TEST_HOST>:/opt/subconverter-deploy/
```

在目标机生成 `.env`（**值不入库**）：

```bash
cat > /opt/subconverter-deploy/.env <<'EOF'
SUBCONVERTER_IMAGE=subconverter:<TAG>
SUBCONVERTER_PORT=15053
POSTGRES_USER=subconverter
POSTGRES_PASSWORD=<随机>
POSTGRES_DB=subconverter
DATABASE_URL=postgresql://subconverter:<随机>@subconverter-postgres:5432/subconverter
API_TOKEN=<openssl rand -base64 32>
SHORTLINK_ENCRYPTION_KEY=<openssl rand -base64 48>
PUBLIC_BASE_URL=<对外地址>
SHORTLINK_ADMIN_SUBJECTS=<管理员标识>
SHORTLINK_CLASH_CONFIG=config/default_clash_lite.ini
SHORTLINK_SINGBOX_PLATFORM=macos
EOF
chmod 600 /opt/subconverter-deploy/.env
```

启动与升级：

```bash
cd /opt/subconverter-deploy
sudo docker compose --env-file .env -p subconverter up -d
sudo docker compose --env-file .env -p subconverter logs -f subconverter | head -40
```

数据库迁移：`ensure_schema()` 在启动时幂等执行，包含
`ALTER TABLE short_links ADD COLUMN IF NOT EXISTS platform TEXT NOT NULL DEFAULT '';`；
如需手工执行，见 `db/migrations/003_shortlink_platform.sql`。

---

## 4. 验证清单

```bash
# 1. 服务存活
curl -fsS http://127.0.0.1:<PORT>/version

# 2. 六平台生成 + 结构断言 + 链式校验（本机或构建机）
tests/singbox_golden.sh

# 3. 短链 API（含 sing-box 平台、非法平台 400、下载文件名与 Content-Type，
#    以及 100 节点 sing-box 快照必须越过 Clash Lite 上限仍返回 201 的回归用例）
BASE_URL=http://127.0.0.1:<PORT> API_KEY=<API_TOKEN> tests/shortlink_api_smoke.sh

# 4. 生成的 sing-box 配置逐平台校验
curl -fsS "http://127.0.0.1:<PORT>/sub?target=singbox&singbox_platform=macos&url=<订阅>" -o macos.json
sing-box check -c macos.json
```

平台专属字段（`auto_redirect`、`override_android_vpn`）在本机内核上会被拒绝解码，
必须在目标设备上校验：

```bash
curl -fsS "...&singbox_platform=openwrt&..." -o openwrt.json
cat openwrt.json | ssh <GATEWAY> 'cat > /tmp/sc.json; <SINGBOX_BIN> check -c /tmp/sc.json'
```

---

## 5. OpenWrt / Open-Box 落地（方案 §4.5 路径 B）

替换 `config.json` 只在下次面板部署前有效，属已知取舍。

```bash
# 0) 备份（保留最近 3 份）
ssh <GATEWAY> 'cp /opt/open-box/etc/config.json /opt/open-box/etc/config.json.bak.$(date +%Y%m%d%H%M%S); \
               ls -1t /opt/open-box/etc/config.json.bak.* | tail -n +4 | xargs -r rm -f'

# 1) 先补齐规则集，再校验（check 会真的打开 .srs）
scripts/sync_singbox_rulesets.sh openwrt.json <GATEWAY> /opt/open-box/data/rulesets
cat openwrt.json | ssh <GATEWAY> 'cat > /opt/open-box/etc/config.json'
ssh <GATEWAY> '<SINGBOX_BIN> check -c /opt/open-box/etc/config.json && echo CHECK_OK'

# 2) 重启并观察
ssh <GATEWAY> '/etc/init.d/openbox restart; sleep 5; logread -e sing-box | tail -20'

# 3) 端到端
ssh <GATEWAY> 'curl -s -o /dev/null -w "%{http_code}\n" https://www.gstatic.com/generate_204'
# LAN 客户端：解析正常、出口 IP 变化

# 4) 回滚
ssh <GATEWAY> 'cp /opt/open-box/etc/config.json.bak.<STAMP> /opt/open-box/etc/config.json && /etc/init.d/openbox restart'
```

注意：
- 网关当前是 **dnsmasq 接管模式**（`dhcp.@dnsmasq[0].noresolv=1` + `server=127.0.0.1#7853`），
  产物必须保留 `dns-in`（`127.0.0.1:7853`）并只对该入站劫持 DNS。
- 同一时刻只能有一个进程接管 `auto_redirect`。

---

## 6. prod 灰度与回滚

1. 用 test 验证通过的同一镜像 tag 部署；保留上一 tag 以便一键回滚。
2. 变更前备份 `.env` 与 PostgreSQL 数据卷。
3. 部署后立即跑第 4 节的 1、3 两项；观察 `/s/*` 命中率与错误日志。
4. 回滚：`SUBCONVERTER_IMAGE=<上一 tag>` 后 `docker compose up -d`；必要时从备份恢复数据卷。

---

## 7. 脱敏要求

- 真实主机地址、域名、证书路径、`API_TOKEN`、`SHORTLINK_ENCRYPTION_KEY`、数据库口令
  **只能**出现在目标机的 `.env` 与私有运维记录中。
- 本文件与 `docs/sing-box-conversion-plan.md` 只使用 `<TEST_HOST>`、`<GATEWAY>` 一类占位符。
- 每次推送前执行 `scripts/check-sensitive.sh`（默认检查暂存区，`--all` 检查全树）。
