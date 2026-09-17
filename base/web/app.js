(() => {
    'use strict';

    const $ = (selector) => document.querySelector(selector);
    const form = $('#create-form');
    const nameInput = $('#name');
    const linksInput = $('#links');
    const expiresInput = $('#expires');
    const targetInput = $('#target');
    const platformInput = $('#platform');
    const message = $('#message');
    const resultCard = $('#result-card');
    const shortUrl = $('#short-url');
    const previewLink = $('#preview-link');
    const downloadLink = $('#download-link');
    const preview = $('#preview');
    const resultMeta = $('#result-meta');
    const linksList = $('#links-list');
    const adminCard = $('#admin-card');
    const adminMessage = $('#admin-message');
    const usersList = $('#users-list');
    const adminUserForm = $('#admin-user-form');
    const adminUsage = $('#admin-usage');
    const usageSection = $('#usage-section');
    const usageList = $('#usage-list');
    const usageSummary = $('#usage-summary');
    const usageRefresh = $('#usage-refresh');
    const bindingForm = $('#binding-form');
    const bindingOwner = $('#binding-owner');
    const bindingProvider = $('#binding-provider');
    const bindingClientId = $('#binding-client-id');
    const bindingLabel = $('#binding-label');
    const bindingPreview = $('#binding-preview');
    const bindingMessage = $('#binding-message');
    const bindingOwnerFilter = $('#binding-owner-filter');
    const bindingsList = $('#bindings-list');
    const bindingsMore = $('#bindings-more');

    let usageTimer = 0;
    let usageInFlight = false;
    let usageStopped = false;
    let usageFailures = 0;
    let previewIdentity = null;
    let nextBindingsCursor = '';
    let listTimer = 0;
    let listInFlight = false;
    let listFailures = 0;

    const el = (tag, className, text) => {
        const node = document.createElement(tag);
        if (className) node.className = className;
        if (text !== undefined) node.textContent = text;
        return node;
    };

    const setMessage = (text, error) => {
        message.textContent = text || '';
        message.className = 'message' + (error ? ' error' : '');
    };

    const setBindingMessage = (text, error) => {
        bindingMessage.textContent = text || '';
        bindingMessage.className = 'message' + (error ? ' error' : '');
    };

    const setAdminMessage = (text, error) => {
        if (!adminMessage) return;
        adminMessage.textContent = text || '';
        adminMessage.className = 'message' + (error ? ' error' : '');
    };

    const apiError = (response, data, fallback) => {
        const code = data && typeof data.error === 'string' ? data.error : '';
        return new Error(code ? fallback + '：' + code : fallback + '（' + response.status + '）');
    };

    const fetchJson = async (url, options) => {
        try {
            const response = await fetch(url, Object.assign({ cache: 'no-store' }, options || {}));
            const data = await response.json().catch(() => ({}));
            return { response, data };
        } catch (_) {
            return { response: { ok: false, status: 0, headers: { get: () => null } }, data: {} };
        }
    };

    const currentTarget = () => (targetInput ? targetInput.value : 'clash');
    const currentPlatform = () => (platformInput ? platformInput.value : '');

    const syncPlatformState = () => {
        if (!platformInput) return;
        const singbox = currentTarget() === 'singbox';
        platformInput.disabled = !singbox;
        if (downloadLink) {
            downloadLink.download = singbox ? 'config.json' : 'config.yaml';
            if (!downloadLink.classList.contains('disabled'))
                downloadLink.textContent = singbox ? '下载 JSON' : '下载 YAML';
        }
    };

    const copyText = async (value) => {
        await navigator.clipboard.writeText(value);
        setMessage('已复制到剪贴板。', false);
    };

    const formatDate = (timestamp, emptyText = '长期有效') => {
        if (timestamp === null || timestamp === undefined || timestamp === 0) return emptyText;
        if (!Number.isInteger(timestamp)) return '时间未知';
        return new Date(timestamp * 1000).toLocaleString();
    };

    const parseBytes = (value) => {
        if (typeof value !== 'string' || !/^(0|[1-9][0-9]*)$/.test(value)) return null;
        try { return BigInt(value); } catch (_) { return null; }
    };

    const formatBytes = (value) => {
        const bytes = parseBytes(value);
        if (bytes === null) return '数据不可用';
        const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB', 'PiB', 'EiB'];
        let unit = 0;
        let divisor = 1n;
        while (unit < units.length - 1 && bytes >= divisor * 1024n) {
            divisor *= 1024n;
            unit += 1;
        }
        if (unit === 0) return bytes.toString() + ' B';
        const scaled = (bytes * 100n) / divisor;
        const whole = scaled / 100n;
        const fraction = (scaled % 100n).toString().padStart(2, '0').replace(/0+$/, '');
        return whole.toString() + (fraction ? '.' + fraction : '') + ' ' + units[unit];
    };

    const percentOf = (usedValue, limitValue) => {
        const used = parseBytes(usedValue);
        const limit = parseBytes(limitValue);
        if (used === null || limit === null || limit === 0n) return 0;
        const tenths = used * 1000n / limit;
        return Number(tenths > 1000n ? 1000n : tenths) / 10;
    };

    const badge = (text, tone) => el('span', 'status ' + (tone || ''), text);

    const validLabel = (value) => value.length > 0 && Array.from(value).length <= 80;

    const validMetrics = (metrics) => {
        if (!metrics || typeof metrics !== 'object' || typeof metrics.enabled !== 'boolean'
            || typeof metrics.activation_pending !== 'boolean' || typeof metrics.auto_reset !== 'boolean'
            || !Array.isArray(metrics.conditions)) return false;
        if ([metrics.upload_bytes, metrics.download_bytes, metrics.used_bytes, metrics.over_limit_bytes].some((value) => parseBytes(value) === null)) return false;
        if (metrics.limit_bytes === null) return metrics.remaining_bytes === null;
        const limit = parseBytes(metrics.limit_bytes);
        return limit !== null && limit > 0n && parseBytes(metrics.remaining_bytes) !== null;
    };

    const availabilityLabel = (availability) => ({
        pending: ['待采集', 'warn'],
        stale: ['数据未更新', 'warn'],
        unavailable: ['数据不可用', 'bad'],
        binding_invalid: ['绑定信息已变化', 'bad']
    })[availability] || null;

    const accountBadges = (metrics) => {
        const badges = [];
        const conditions = Array.isArray(metrics.conditions) ? metrics.conditions : [];
        const hasAccountCondition = conditions.some((item) => ['expired', 'quota_exceeded', 'quota_at_limit'].includes(item));
        if (metrics.enabled === false) badges.push(badge('已停用', 'bad'));
        else if (!hasAccountCondition) badges.push(badge('可用', 'good'));
        if (conditions.includes('expired')) badges.push(badge('账户已过期', 'bad'));
        if (conditions.includes('quota_exceeded')) badges.push(badge('已超过额度', 'bad'));
        if (conditions.includes('quota_at_limit')) badges.push(badge('额度已用完', 'warn'));
        if (metrics.enabled === false && !hasAccountCondition) badges.push(badge('原因未提供', ''));
        if (metrics.activation_pending === true) badges.push(badge('周期待开始', 'warn'));
        if (conditions.includes('reset_due')) badges.push(badge('等待面板执行重置', 'warn'));
        if (conditions.includes('invalid_reset_policy')) badges.push(badge('重置设置异常', 'warn'));
        return badges;
    };

    const metricBlock = (label, value, detail) => {
        const block = el('div', 'metric');
        block.append(el('span', 'metric-label', label), el('strong', 'metric-value', value));
        if (detail) block.append(el('p', 'metric-detail', detail));
        return block;
    };

    const resetDescription = (metrics) => {
        const conditions = Array.isArray(metrics.conditions) ? metrics.conditions : [];
        if (!metrics.auto_reset) return '未设置自动重置';
        if (conditions.includes('invalid_reset_policy')) return '自动重置设置异常';
        if (metrics.activation_pending) return metrics.reset_days > 0 ? '首笔流量后每 ' + metrics.reset_days + ' 天重置' : '等待周期开始';
        if (conditions.includes('reset_due')) return '等待面板执行重置';
        if (metrics.next_reset_at) return '下次重置 ' + formatDate(metrics.next_reset_at, '未知');
        return metrics.reset_days > 0 ? '每 ' + metrics.reset_days + ' 天重置' : '重置时间未知';
    };

    const renderUsageItem = (item) => {
        const row = el('article', 'usage-row');
        const head = el('div', 'usage-head');
        head.append(el('strong', 'usage-name', typeof item.label === 'string' ? item.label : '未命名账户'));
        const statuses = el('div', 'status-group');
        const availability = availabilityLabel(item.availability);
        const metricsAreValid = validMetrics(item.metrics);
        if (metricsAreValid && ['fresh', 'stale'].includes(item.availability)) {
            for (const node of accountBadges(item.metrics)) statuses.append(node);
        }
        if (availability) statuses.append(badge(availability[0], availability[1]));
        head.append(statuses);
        row.append(head);
        if (!metricsAreValid || !['fresh', 'stale'].includes(item.availability)) {
            const text = item.availability === 'pending' ? '绑定已建立，正在等待首次采集。'
                : item.availability === 'binding_invalid' ? 'Client 不存在或身份信息已改变，请联系管理员重新核对。'
                    : item.metrics && !metricsAreValid ? '返回的数据格式无法安全显示，请联系管理员检查采集服务。'
                        : '当前无法读取账户统计，请稍后再试。';
            row.append(el('div', 'usage-empty', text));
            if (item.last_attempt_at) row.append(el('p', 'usage-foot', '最近尝试：' + formatDate(item.last_attempt_at, '未知')));
            return row;
        }
        const metrics = item.metrics;
        const grid = el('div', 'usage-metrics');
        const unlimited = metrics.limit_bytes === null;
        const limit = unlimited ? null : parseBytes(metrics.limit_bytes);
        const used = formatBytes(metrics.used_bytes);
        const usageBlock = metricBlock('已用 / 总额', unlimited ? used + ' / 无限流量' : used + ' / ' + formatBytes(metrics.limit_bytes), '上传 ' + formatBytes(metrics.upload_bytes) + ' · 下载 ' + formatBytes(metrics.download_bytes));
        if (!unlimited && limit !== null) {
            const over = parseBytes(metrics.over_limit_bytes);
            const progress = el('div', 'usage-progress' + (over !== null && over > 0n ? ' over' : ''));
            const percentage = percentOf(metrics.used_bytes, metrics.limit_bytes);
            progress.setAttribute('role', 'progressbar');
            progress.setAttribute('aria-valuemin', '0');
            progress.setAttribute('aria-valuemax', '100');
            progress.setAttribute('aria-valuenow', String(percentage));
            const fill = el('span');
            fill.style.width = percentage + '%';
            progress.append(fill);
            usageBlock.append(progress);
        }
        const remaining = unlimited ? '无限流量' : formatBytes(metrics.remaining_bytes);
        const over = parseBytes(metrics.over_limit_bytes);
        const remainingDetail = over !== null && over > 0n ? '已超出 ' + formatBytes(metrics.over_limit_bytes) : '';
        const expiry = metrics.expires_at === null ? (metrics.activation_pending ? '周期待开始' : '长期有效') : formatDate(metrics.expires_at, '长期有效');
        grid.append(usageBlock, metricBlock('剩余额度', remaining, remainingDetail), metricBlock('账户有效期', expiry, resetDescription(metrics)));
        row.append(grid);
        const footParts = ['采样于 ' + formatDate(item.observed_at, '未知')];
        if (metrics.last_traffic_at) footParts.push('最近有流量 ' + formatDate(metrics.last_traffic_at, '未知'));
        row.append(el('p', 'usage-foot', footParts.join(' · ')));
        return row;
    };

    const renderUsage = (data) => {
        if (data.enabled === false || data.state === 'disabled') {
            usageSection.classList.add('hidden');
            usageStopped = true;
            return;
        }
        usageSection.classList.remove('hidden');
        usageList.replaceChildren();
        if (data.state === 'unbound') {
            usageSummary.textContent = '账户状态与短链有效期相互独立';
            usageList.append(el('div', 'usage-empty', '尚未关联账户，请联系管理员建立用量绑定。'));
            return;
        }
        const items = Array.isArray(data.items) ? data.items : [];
        usageSummary.textContent = items.length ? items.length + ' 个独立账户 · 数据按采样时间更新' : '暂无可显示的数据';
        if (!items.length) usageList.append(el('div', 'usage-empty', '暂无可显示的账户。'));
        for (const item of items) usageList.append(renderUsageItem(item));
    };

    const scheduleUsage = (seconds) => {
        window.clearTimeout(usageTimer);
        if (usageStopped || document.hidden || !navigator.onLine) return;
        usageTimer = window.setTimeout(() => loadUsage(), Math.max(1, seconds) * 1000);
    };

    const retryAfterSeconds = (value, fallback = 60) => {
        if (/^[0-9]+$/.test(value)) return Number.parseInt(value, 10);
        const retryAt = Date.parse(value);
        return Number.isFinite(retryAt) ? Math.max(1, Math.ceil((retryAt - Date.now()) / 1000)) : fallback;
    };

    // 网关限流（nginx limit_req）不带 Retry-After，回退到一个短退避而不是默认的一分钟。
    const GATEWAY_RETRY_SECONDS = 5;

    // 只从 JSON 错误体里取 message；HTML 错误页（nginx 503/429）绝不回显，避免把整页 HTML 当文案。
    const errorDetail = (response, body, fallback) => {
        if (response.status === 429) {
            return '请求较频繁（429），请等待 '
                + retryAfterSeconds(response.headers.get('Retry-After') || '', GATEWAY_RETRY_SECONDS)
                + ' 秒后重试。';
        }
        const contentType = response.headers.get('Content-Type') || '';
        if (contentType.includes('application/json') && body) {
            try {
                const data = JSON.parse(body);
                if (data && typeof data.error === 'string' && data.error) return fallback + '：' + data.error;
            } catch (_) { /* 保持通用提示 */ }
        }
        return fallback + '（' + response.status + '）';
    };

    const loadUsage = async (manual = false) => {
        if (usageInFlight || usageStopped || (!manual && (document.hidden || !navigator.onLine))) return;
        usageInFlight = true;
        usageRefresh.classList.add('loading');
        usageRefresh.disabled = true;
        try {
            const { response, data } = await fetchJson('/api/usage');
            if (response.status === 401) {
                usageSection.classList.remove('hidden');
                usageSummary.textContent = '登录状态已失效';
                usageList.replaceChildren(el('div', 'usage-empty', '请重新登录后查看账户用量。'));
                usageStopped = true;
                return;
            }
            if (response.status === 404) {
                usageSection.classList.add('hidden');
                usageStopped = true;
                return;
            }
            if (response.status === 429) {
                const retry = retryAfterSeconds(response.headers.get('Retry-After') || '');
                usageSection.classList.remove('hidden');
                usageSummary.textContent = '请求较频繁，稍后自动重试';
                if (usageList.querySelector('.usage-placeholder')) usageList.replaceChildren(el('div', 'usage-empty', '用量请求已限流，页面将自动重试。'));
                scheduleUsage(retry);
                return;
            }
            if (!response.ok) throw apiError(response, data, '用量服务暂不可用');
            const validItems = Array.isArray(data.items) && data.items.every((item) => item && typeof item === 'object'
                && typeof item.binding_id === 'string' && typeof item.label === 'string' && typeof item.availability === 'string');
            if (data.schema_version !== 1 || typeof data.enabled !== 'boolean' || typeof data.state !== 'string' || !validItems) {
                throw new Error('invalid_usage_response');
            }
            usageFailures = 0;
            renderUsage(data);
            const seconds = Number.isInteger(data.refresh_after_seconds) ? Math.max(30, data.refresh_after_seconds) : 30;
            scheduleUsage(seconds);
        } catch (_) {
            usageFailures += 1;
            usageSection.classList.remove('hidden');
            usageSummary.textContent = '数据读取失败，已有短链功能不受影响';
            if (!usageList.children.length || usageList.querySelector('.usage-placeholder')) {
                usageList.replaceChildren(el('div', 'usage-empty', '暂时无法读取账户用量。'));
            } else {
                for (const group of usageList.querySelectorAll('.status-group')) {
                    if (!group.querySelector('.fetch-error')) {
                        const failed = badge('刷新失败', 'warn');
                        failed.classList.add('fetch-error');
                        group.append(failed);
                    }
                }
            }
            scheduleUsage(Math.min(300, 30 * (2 ** Math.min(usageFailures - 1, 4))));
        } finally {
            usageInFlight = false;
            usageRefresh.classList.remove('loading');
            usageRefresh.disabled = false;
        }
    };

    const authHeaders = () => ({});

    const shortLinkActionError = async (response, fallback) =>
        new Error(errorDetail(response, await response.text().catch(() => ''), fallback));

    const loadPreview = async (url) => {
        preview.textContent = '加载中……';
        const response = await fetch(url, { cache: 'no-store' });
        const content = await response.text().catch(() => '');
        if (!response.ok) {
            preview.textContent = '';
            throw new Error(errorDetail(response, content, '预览失败'));
        }
        const limit = 200000;
        preview.textContent = content.length > limit ? content.slice(0, limit) + '\n\n……预览已截断，完整内容请下载……' : content;
    };

    const loadList = async (manual = false) => {
        if (listInFlight) return;
        listInFlight = true;
        window.clearTimeout(listTimer);
        try {
            const response = await fetch('/api/short-links', { cache: 'no-store', headers: authHeaders() });
            if (response.status === 401) {
                linksList.replaceChildren(el('p', 'muted', '登录状态已失效，请重新登录后查看短链。'));
                setMessage('登录状态已失效，请重新登录。', true);
                return;
            }
            if (response.status === 429) {
                // 网关限流：保留已有列表不清空，按 Retry-After（缺失时用短退避）自动重试。
                listFailures += 1;
                const seconds = retryAfterSeconds(response.headers.get('Retry-After') || '', GATEWAY_RETRY_SECONDS);
                const retrying = listFailures <= 3;
                setMessage('请求较频繁，' + seconds + ' 秒后' + (retrying ? '自动重试' : '请手动刷新') + '；已有短链不受影响。', true);
                if (retrying) {
                    window.clearTimeout(listTimer);
                    listTimer = window.setTimeout(() => loadList(), Math.max(1, seconds) * 1000);
                }
                return;
            }
            if (!response.ok) {
                // 后端/网关返回 HTML 错误页（如关闭 limit_req_status 时的 503）时不再原样展示。
                throw new Error(errorDetail(response, await response.text().catch(() => ''), '加载失败'));
            }
            const data = await response.json();
            listFailures = 0;
            renderList(data.items || []);
        } catch (error) {
            // 网络故障等：已有列表保留，仅在从未加载成功时显示占位提示。
            if (!linksList.children.length || linksList.querySelector('p.muted:only-child')) {
                linksList.replaceChildren(el('p', 'muted', error.message));
            } else {
                setMessage(error.message + '；已保留当前列表。', true);
            }
        } finally {
            listInFlight = false;
        }
    };

    const renderList = (items) => {
        linksList.replaceChildren();
        if (!items.length) {
            linksList.append(el('p', 'muted', '暂无短链。'));
            return;
        }
        for (const item of items) {
            const row = el('div', 'link-row');
            const body = el('div');
            const title = el('strong', '', item.name || '未命名短链');
            const expired = Boolean(item.expires_at && item.expires_at * 1000 <= Date.now());
            const unavailable = Boolean(item.revoked_at) || expired;
            const state = item.revoked_at ? '已撤销' : (expired ? '短链已过期' : '短链有效');
            const itemFormat = item.target === 'singbox' ? 'sing-box/' + (item.platform || 'macos') : 'clash';
            const detail = el('p', '', (item.owner ? item.owner + ' · ' : '') + itemFormat + ' · ' + item.links_count + ' 个输入 · 更新于 ' + formatDate(item.updated_at, '未知') + ' · ' + state);
            body.append(title, detail, el('code', '', item.short_url));
            const actions = el('div', 'row-actions');
            const copy = el('button', 'secondary', '复制');
            copy.onclick = () => copyText(item.short_url);
            const download = el('a', 'button', item.target === 'singbox' ? '下载 JSON' : '下载 YAML');
            download.href = item.download_url || (item.short_url + (item.short_url.includes('?') ? '&' : '?') + 'download=1');
            download.setAttribute('download', '');
            download.rel = 'noreferrer';
            if (unavailable) {
                download.className += ' disabled';
                download.removeAttribute('href');
                download.setAttribute('aria-disabled', 'true');
                download.title = '短链已过期或已撤销，无法下载';
                download.onclick = (event) => event.preventDefault();
            }
            const refresh = el('button', 'secondary', '刷新配置');
            refresh.disabled = Boolean(item.revoked_at);
            refresh.onclick = async () => {
                refresh.disabled = true;
                try {
                    const response = await fetch('/api/short-links/' + encodeURIComponent(item.id) + '/refresh', { method: 'POST', headers: authHeaders() });
                    if (!response.ok) throw await shortLinkActionError(response, '刷新失败');
                    setMessage('刷新成功，正在重新加载列表……', false);
                    await loadList(true);
                    if (shortUrl.value === item.short_url) await loadPreview(item.short_url);
                } catch (error) {
                    setMessage(error.message, true);
                } finally {
                    refresh.disabled = Boolean(item.revoked_at);
                }
            };
            const revoke = el('button', 'danger', '撤销');
            revoke.disabled = Boolean(item.revoked_at);
            revoke.onclick = async () => {
                if (!confirm('确认撤销这条短链？')) return;
                revoke.disabled = true;
                try {
                    const response = await fetch('/api/short-links/' + encodeURIComponent(item.id), { method: 'DELETE', headers: authHeaders() });
                    if (!response.ok) throw await shortLinkActionError(response, '撤销失败');
                    setMessage('已撤销，正在重新加载列表……', false);
                    await loadList(true);
                } catch (error) {
                    setMessage(error.message, true);
                } finally {
                    revoke.disabled = Boolean(item.revoked_at);
                }
            };
            actions.append(copy, download, refresh, revoke);
            row.append(body, actions);
            linksList.append(row);
        }
    };

    const populateUserSelectors = (users) => {
        const currentOwner = bindingOwner.value;
        const currentFilter = bindingOwnerFilter.value;
        bindingOwner.replaceChildren(new Option('选择已有用户', ''));
        bindingOwnerFilter.replaceChildren(new Option('全部账号', ''));
        for (const user of users) {
            const label = user.email ? user.subject + ' · ' + user.email : user.subject;
            bindingOwner.append(new Option(label, user.subject));
            bindingOwnerFilter.append(new Option(label, user.subject));
        }
        bindingOwner.value = currentOwner;
        bindingOwnerFilter.value = currentFilter;
    };

    const loadAdminUsers = async () => {
        if (!adminCard || !usersList) return;
        const { response, data } = await fetchJson('/api/admin/users');
        if (!response.ok) return;
        adminCard.classList.remove('hidden');
        const users = Array.isArray(data.items) ? data.items : [];
        usersList.replaceChildren();
        for (const user of users) {
            const row = el('div', 'link-row');
            row.append(el('span', '', user.subject + ' · ' + (user.email || '-') + ' · ' + user.role));
            usersList.append(row);
        }
        populateUserSelectors(users);
        await Promise.all([loadProviders(), loadBindings(false)]);
    };

    const loadProviders = async () => {
        const { response, data } = await fetchJson('/api/admin/usage-providers');
        if (!response.ok) return;
        adminUsage.classList.remove('hidden');
        bindingProvider.replaceChildren(new Option('选择 Provider', ''));
        for (const provider of Array.isArray(data.items) ? data.items : []) {
            const option = new Option(provider.label + (provider.available ? '' : '（不可用）'), provider.provider_id);
            option.disabled = !provider.available;
            bindingProvider.append(option);
        }
    };

    const renderBinding = (item) => {
        const row = el('div', 'binding-row');
        const main = el('div', 'binding-row-main');
        const title = el('div', 'binding-row-title');
        title.append(el('strong', '', item.label));
        const state = availabilityLabel(item.availability) || ['已绑定', 'good'];
        title.append(badge(state[0], state[1]));
        main.append(title, el('p', 'binding-row-detail', item.owner_subject + ' · ' + item.provider_id + ' · Client ' + item.client_id + ' · revision ' + item.revision));
        const actions = el('div', 'binding-actions');
        const labelInput = document.createElement('input');
        labelInput.value = item.label;
        labelInput.setAttribute('aria-label', '修改 ' + item.label + ' 的显示名');
        const save = el('button', 'secondary', '改名');
        save.onclick = async () => {
            const label = labelInput.value.trim();
            if (!validLabel(label)) return setBindingMessage('显示名需为 1 至 80 个字符。', true);
            const { response, data } = await fetchJson('/api/admin/usage-bindings/' + encodeURIComponent(item.id), {
                method: 'POST', headers: { 'Content-Type': 'application/json', 'If-Match': '"' + item.revision + '"' }, body: JSON.stringify({ label })
            });
            if (!response.ok) return setBindingMessage(apiError(response, data, '改名失败').message, true);
            setBindingMessage('显示名已更新。', false);
            await loadBindings(false);
        };
        const revoke = el('button', 'danger', '撤销绑定');
        revoke.onclick = async () => {
            if (!confirm('确认撤销“' + item.label + '”的用量绑定？这不会删除 s-ui Client。')) return;
            const { response, data } = await fetchJson('/api/admin/usage-bindings/' + encodeURIComponent(item.id), { method: 'DELETE', headers: { 'If-Match': '"' + item.revision + '"' } });
            if (!response.ok) return setBindingMessage(apiError(response, data, '撤销失败').message, true);
            setBindingMessage('绑定已撤销，s-ui Client 未被删除。', false);
            await Promise.all([loadBindings(false), loadUsage(true)]);
        };
        actions.append(labelInput, save, revoke);
        row.append(main, actions);
        return row;
    };

    const loadBindings = async (append) => {
        const params = new URLSearchParams({ limit: '50' });
        if (bindingOwnerFilter.value) params.set('owner_subject', bindingOwnerFilter.value);
        if (append && nextBindingsCursor) params.set('cursor', nextBindingsCursor);
        const { response, data } = await fetchJson('/api/admin/usage-bindings?' + params.toString());
        if (!response.ok) {
            if (!append) bindingsList.replaceChildren(el('p', 'muted', '绑定列表暂时无法读取。'));
            return;
        }
        if (!append) bindingsList.replaceChildren();
        const items = Array.isArray(data.items) ? data.items : [];
        if (!append && !items.length) bindingsList.append(el('p', 'muted', '暂无用量绑定。'));
        for (const item of items) bindingsList.append(renderBinding(item));
        nextBindingsCursor = typeof data.next_cursor === 'string' ? data.next_cursor : '';
        bindingsMore.classList.toggle('hidden', !nextBindingsCursor);
    };

    const showBindingPreview = (data) => {
        previewIdentity = {
            owner_subject: data.owner_subject,
            provider_id: data.provider_id,
            client_id: data.client_id,
            label: data.label,
            expected_instance_id: data.expected_instance_id,
            expected_identity_fingerprint: data.expected_identity_fingerprint
        };
        bindingPreview.replaceChildren(el('strong', '', '校验结果'));
        const grid = el('div', 'preview-grid');
        grid.append(metricBlock('门户账号', data.owner_subject), metricBlock('Provider / Client', data.provider_id + ' / ' + data.client_id), metricBlock('显示名', data.label));
        if (data.metrics) {
            const expiry = data.metrics.expires_at === null ? (data.metrics.activation_pending ? '周期待开始' : '长期有效') : formatDate(data.metrics.expires_at);
            grid.append(metricBlock('已用流量', formatBytes(data.metrics.used_bytes)), metricBlock('总额度', data.metrics.limit_bytes === null ? '无限流量' : formatBytes(data.metrics.limit_bytes)), metricBlock('账户有效期', expiry));
        }
        const create = el('button', '', '确认建立绑定');
        create.type = 'button';
        create.onclick = createBinding;
        bindingPreview.append(grid, create);
        bindingPreview.classList.remove('hidden');
    };

    const createBinding = async () => {
        if (!previewIdentity) return;
        const { response, data } = await fetchJson('/api/admin/usage-bindings', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(previewIdentity) });
        if (!response.ok) {
            setBindingMessage(apiError(response, data, '建立绑定失败').message, true);
            if (response.status === 409) bindingPreview.classList.add('hidden');
            return;
        }
        previewIdentity = null;
        bindingPreview.classList.add('hidden');
        bindingForm.reset();
        setBindingMessage('用量绑定已建立，正在等待首次采集。', false);
        await Promise.all([loadBindings(false), loadUsage(true)]);
    };

    form.addEventListener('submit', async (event) => {
        event.preventDefault();
        const links = linksInput.value.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
        if (!links.length) return setMessage('至少输入一个节点或订阅链接。', true);
        setMessage('正在转换并保存短链……', false);
        resultCard.classList.add('hidden');
        try {
            const response = await fetch('/api/short-links', { method: 'POST', headers: Object.assign({ 'Content-Type': 'application/json' }, authHeaders()), body: JSON.stringify({ name: nameInput.value.trim(), target: currentTarget(), platform: currentTarget() === 'singbox' ? currentPlatform() : '', expires_in: Number(expiresInput.value), links }) });
            const data = await response.json().catch(() => ({}));
            if (!response.ok) throw new Error(data.error || ('创建失败（' + response.status + '）'));
            shortUrl.value = data.short_url;
            previewLink.href = data.preview_url;
            downloadLink.href = data.download_url;
            resultMeta.textContent = data.links_count + ' 个输入 · 短链过期时间：' + formatDate(data.expires_at, '永久');
            resultCard.classList.remove('hidden');
            await loadPreview(data.preview_url);
            setMessage('短链创建成功。', false);
            await loadList();
        } catch (error) { setMessage(error.message, true); }
    });

    if (targetInput) targetInput.addEventListener('change', syncPlatformState);
    syncPlatformState();

    $('#copy-button').onclick = () => copyText(shortUrl.value);
    $('#clear-button').onclick = () => {
        linksInput.value = '';
        nameInput.value = '';
        preview.textContent = '';
        resultCard.classList.add('hidden');
        setMessage('', false);
    };
    $('#refresh-button').onclick = () => loadList(true);
    usageRefresh.onclick = () => loadUsage(true);
    $('#binding-refresh').onclick = () => Promise.all([loadProviders(), loadBindings(false)]);
    bindingsMore.onclick = () => loadBindings(true);
    bindingOwnerFilter.onchange = () => loadBindings(false);

    bindingForm.addEventListener('submit', async (event) => {
        event.preventDefault();
        previewIdentity = null;
        bindingPreview.classList.add('hidden');
        setBindingMessage('正在校验 Client 身份和统计……', false);
        const request = { owner_subject: bindingOwner.value, provider_id: bindingProvider.value, client_id: bindingClientId.value.trim(), label: bindingLabel.value.trim() };
        if (!validLabel(request.label)) return setBindingMessage('显示名需为 1 至 80 个字符。', true);
        const { response, data } = await fetchJson('/api/admin/usage-bindings/preview', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(request) });
        if (!response.ok) return setBindingMessage(apiError(response, data, '校验失败').message, true);
        setBindingMessage('校验通过，请核对后建立绑定。', false);
        showBindingPreview(data);
    });

    if (adminUserForm) adminUserForm.addEventListener('submit', async (event) => {
        event.preventDefault();
        const submit = adminUserForm.querySelector('button[type="submit"]');
        if (submit) submit.disabled = true;
        try {
            const { response, data } = await fetchJson('/api/admin/users', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ subject: $('#admin-subject').value.trim(), email: $('#admin-email').value.trim(), role: $('#admin-role').value })
            });
            if (!response.ok) return setAdminMessage(apiError(response, data, '保存用户失败').message, true);
            setAdminMessage('用户已保存。', false);
        } catch (_) {
            setAdminMessage('保存用户失败，请稍后重试。', true);
        } finally {
            if (submit) submit.disabled = false;
        }
        await loadAdminUsers().catch(() => {});
    });

    document.addEventListener('visibilitychange', () => {
        if (document.hidden) window.clearTimeout(usageTimer);
        else if (!usageStopped) loadUsage();
    });
    window.addEventListener('online', () => { if (!usageStopped) loadUsage(); });
    window.addEventListener('offline', () => window.clearTimeout(usageTimer));

    loadList();
    loadUsage();
    loadAdminUsers().catch(() => {});
})();
