#!/usr/bin/env node
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');

let chromium;
try {
    ({ chromium } = require('playwright'));
} catch (_) {
    if (!process.env.PLAYWRIGHT_PATH) {
        throw new Error('Set PLAYWRIGHT_PATH to the installed Playwright module directory.');
    }
    ({ chromium } = require(process.env.PLAYWRIGHT_PATH));
}

const root = path.resolve(__dirname, '..');
const webRoot = path.join(root, 'base', 'web');
const screenshotDir = process.env.UI_QA_DIR || '/tmp/subconverter-usage-ui';
const now = 1788912000;

const metrics = (overrides = {}) => Object.assign({
    upload_bytes: '1073741824',
    download_bytes: '9663676416',
    used_bytes: '10737418240',
    limit_bytes: '107374182400',
    remaining_bytes: '96636764160',
    over_limit_bytes: '0',
    expires_at: now + 86400 * 30,
    enabled: true,
    activation_pending: false,
    auto_reset: false,
    reset_days: 0,
    next_reset_at: null,
    last_traffic_at: now - 10,
    conditions: []
}, overrides);

const usagePayload = {
    schema_version: 1,
    enabled: true,
    state: 'ready',
    server_time: now + 10,
    refresh_after_seconds: 30,
    items: [
        {
            binding_id: '11',
            label: '主账户 · 超大精度校验 9007199254740992 bytes',
            availability: 'fresh',
            observed_at: now,
            last_attempt_at: now,
            error_code: null,
            metrics: metrics({
                upload_bytes: '4503599627370496',
                download_bytes: '4503599627370496',
                used_bytes: '9007199254740992',
                limit_bytes: '18014398509481984',
                remaining_bytes: '9007199254740992'
            })
        },
        {
            binding_id: '12',
            label: '停用且等待周期的账户',
            availability: 'stale',
            observed_at: now - 120,
            last_attempt_at: now,
            error_code: 'upstream_unavailable',
            metrics: metrics({
                enabled: false,
                activation_pending: true,
                auto_reset: true,
                reset_days: 30,
                next_reset_at: 0,
                expires_at: null,
                used_bytes: '118111600640',
                limit_bytes: '107374182400',
                remaining_bytes: '0',
                over_limit_bytes: '10737418240',
                conditions: ['quota_exceeded', 'reset_due']
            })
        },
        {
            binding_id: '13', label: '尚未采集', availability: 'pending',
            observed_at: null, last_attempt_at: null, error_code: null, metrics: null
        },
        {
            binding_id: '14', label: '远端暂不可用', availability: 'unavailable',
            observed_at: null, last_attempt_at: now, error_code: 'timeout', metrics: null
        },
        {
            binding_id: '15', label: '身份已变化', availability: 'binding_invalid',
            observed_at: null, last_attempt_at: now, error_code: 'identity_changed', metrics: null
        }
    ]
};

const users = [
    { subject: 'usage-admin@example.invalid', email: 'usage-admin@example.invalid', role: 'admin' },
    { subject: 'usage-a@example.invalid', email: 'usage-a@example.invalid', role: 'user' }
];

let binding = {
    id: '21', revision: '7', owner_subject: 'usage-a@example.invalid', provider_id: 'fixture',
    client_id: '3', label: '现有账户', created_at: now - 3600, availability: 'fresh',
    observed_at: now, last_attempt_at: now, error_code: null
};

const requests = [];

const jsonResponse = (route, body, status = 200, headers = {}) => route.fulfill({
    status,
    contentType: 'application/json; charset=utf-8',
    headers: Object.assign({ 'Cache-Control': 'private, no-store' }, headers),
    body: status === 204 ? '' : JSON.stringify(body)
});

const installApiFixture = async (page, mode = 'enabled') => {
    await page.route('**/api/**', async (route) => {
        const request = route.request();
        const url = new URL(request.url());
        const method = request.method();
        let body = null;
        try { body = request.postDataJSON(); } catch (_) {}
        requests.push({ method, path: url.pathname, search: url.search, headers: await request.allHeaders(), body });

        if (url.pathname === '/api/usage') {
            if (mode === 'disabled') return jsonResponse(route, { schema_version: 1, enabled: false, state: 'disabled', server_time: now, refresh_after_seconds: 30, items: [] });
            return jsonResponse(route, usagePayload);
        }
        if (url.pathname === '/api/short-links' && method === 'GET') return jsonResponse(route, { items: [] });
        if (url.pathname === '/api/admin/users' && method === 'GET') return jsonResponse(route, { items: users });
        if (url.pathname === '/api/admin/usage-providers' && method === 'GET') {
            if (mode === 'disabled') return jsonResponse(route, { error: 'usage_unavailable' }, 503);
            return jsonResponse(route, { schema_version: 1, items: [{ provider_id: 'fixture', label: 'Fixture s-ui', available: true }] });
        }
        if (url.pathname === '/api/admin/usage-bindings' && method === 'GET') {
            return jsonResponse(route, { schema_version: 1, items: binding ? [binding] : [], next_cursor: null });
        }
        if (url.pathname === '/api/admin/usage-bindings/preview' && method === 'POST') {
            return jsonResponse(route, Object.assign({}, body, {
                expected_instance_id: '11111111-1111-4111-8111-111111111111',
                expected_identity_fingerprint: 'a'.repeat(64),
                observed_at: now,
                metrics: metrics()
            }));
        }
        if (url.pathname === '/api/admin/usage-bindings' && method === 'POST') {
            binding = Object.assign({}, binding, body, { id: '22', revision: '1', created_at: now });
            return jsonResponse(route, binding, 201);
        }
        if (url.pathname === '/api/admin/usage-bindings/21' && method === 'POST') {
            binding = Object.assign({}, binding, { label: body.label, revision: '8' });
            return jsonResponse(route, binding);
        }
        if (url.pathname === '/api/admin/usage-bindings/21' && method === 'DELETE') {
            binding = null;
            return jsonResponse(route, null, 204);
        }
        return jsonResponse(route, { error: 'fixture_not_found' }, 404);
    });
};

const startStaticServer = () => new Promise((resolve) => {
    const server = http.createServer((request, response) => {
        const pathname = new URL(request.url, 'http://127.0.0.1').pathname;
        const file = pathname === '/' ? 'index.html' : pathname.slice(1);
        if (!['index.html', 'app.js', 'app.css'].includes(file)) {
            response.writeHead(404).end();
            return;
        }
        const contentType = file.endsWith('.css') ? 'text/css' : file.endsWith('.js') ? 'application/javascript' : 'text/html';
        response.writeHead(200, { 'Content-Type': contentType + '; charset=utf-8', 'Cache-Control': 'no-store' });
        fs.createReadStream(path.join(webRoot, file)).pipe(response);
    });
    server.listen(0, '127.0.0.1', () => resolve(server));
});

const assertNoHorizontalOverflow = async (page, label) => {
    const dimensions = await page.evaluate(() => ({
        viewport: window.innerWidth,
        document: document.documentElement.scrollWidth,
        body: document.body.scrollWidth,
        offenders: Array.from(document.querySelectorAll('body *')).map((node) => {
            const rect = node.getBoundingClientRect();
            return { tag: node.tagName, id: node.id, className: String(node.className), left: rect.left, right: rect.right, scrollWidth: node.scrollWidth, clientWidth: node.clientWidth };
        }).filter((item) => item.right > window.innerWidth + 0.5 || item.left < -0.5).slice(0, 12)
    }));
    assert.ok(dimensions.document <= dimensions.viewport, label + ' document overflow: ' + JSON.stringify(dimensions));
    assert.ok(dimensions.body <= dimensions.viewport, label + ' body overflow: ' + JSON.stringify(dimensions));
};

const run = async () => {
    fs.mkdirSync(screenshotDir, { recursive: true });
    const server = await startStaticServer();
    const address = server.address();
    const baseUrl = 'http://127.0.0.1:' + address.port;
    const browser = await chromium.launch({ headless: true });
    try {
        const desktop = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
        await installApiFixture(desktop);
        await desktop.goto(baseUrl, { waitUntil: 'networkidle' });
        await desktop.locator('#usage-section:not(.hidden)').waitFor();
        assert.equal(await desktop.locator('.usage-row').count(), 5);
        for (const text of ['可用', '已停用', '周期待开始', '数据未更新', '已超过额度', '等待面板执行重置', '待采集', '数据不可用', '绑定信息已变化']) {
            await desktop.getByText(text, { exact: true }).first().waitFor();
        }
        await desktop.getByText('8 PiB / 16 PiB', { exact: true }).waitFor();
        await assertNoHorizontalOverflow(desktop, 'desktop');
        await desktop.screenshot({ path: path.join(screenshotDir, 'usage-desktop-1440x1000.png'), fullPage: true });

        await desktop.locator('#binding-owner').selectOption('usage-a@example.invalid');
        await desktop.locator('#binding-provider').selectOption('fixture');
        await desktop.locator('#binding-client-id').fill('3');
        await desktop.locator('#binding-label').fill('个人账户');
        await desktop.getByRole('button', { name: '校验绑定' }).click();
        await desktop.getByRole('button', { name: '确认建立绑定' }).waitFor();
        const previewRequest = requests.find((item) => item.path === '/api/admin/usage-bindings/preview' && item.method === 'POST');
        assert.deepEqual(previewRequest.body, { owner_subject: 'usage-a@example.invalid', provider_id: 'fixture', client_id: '3', label: '个人账户' });
        await desktop.getByRole('button', { name: '确认建立绑定' }).click();
        const createRequest = requests.find((item) => item.path === '/api/admin/usage-bindings' && item.method === 'POST');
        assert.deepEqual(createRequest.body, {
            owner_subject: 'usage-a@example.invalid', provider_id: 'fixture', client_id: '3', label: '个人账户',
            expected_instance_id: '11111111-1111-4111-8111-111111111111', expected_identity_fingerprint: 'a'.repeat(64)
        });
        assert.ok(!Object.hasOwn(createRequest.body, 'metrics'));
        await desktop.getByText('用量绑定已建立，正在等待首次采集。', { exact: true }).waitFor();

        binding = Object.assign({}, binding, { id: '21', revision: '7', label: '现有账户' });
        await desktop.locator('#binding-refresh').click();
        await desktop.waitForTimeout(100);
        const bindingRow = desktop.locator('.binding-row').filter({ hasText: 'Client 3' });
        await bindingRow.locator('input').fill('已改名账户');
        assert.equal(await bindingRow.locator('input').inputValue(), '已改名账户');
        await bindingRow.getByRole('button', { name: '改名' }).click();
        await desktop.getByText('显示名已更新。', { exact: true }).waitFor();
        const renameRequest = requests.find((item) => item.path === '/api/admin/usage-bindings/21' && item.method === 'POST');
        assert.equal(renameRequest.headers['if-match'], '"7"');
        assert.deepEqual(renameRequest.body, { label: '已改名账户' });

        desktop.once('dialog', (dialog) => dialog.accept());
        await desktop.getByRole('button', { name: '撤销绑定' }).click();
        const revokeRequest = requests.find((item) => item.path === '/api/admin/usage-bindings/21' && item.method === 'DELETE');
        assert.equal(revokeRequest.headers['if-match'], '"8"');
        await desktop.getByText('暂无用量绑定。', { exact: true }).waitFor();
        await desktop.screenshot({ path: path.join(screenshotDir, 'usage-admin-workflow.png'), fullPage: true });

        const mobile = await browser.newPage({ viewport: { width: 390, height: 844 } });
        await installApiFixture(mobile);
        await mobile.goto(baseUrl, { waitUntil: 'networkidle' });
        await mobile.locator('#usage-section:not(.hidden)').waitFor();
        await assertNoHorizontalOverflow(mobile, 'mobile');
        const adminControlWidths = await mobile.locator('#admin-user-form input, #admin-user-form select, #admin-user-form button').evaluateAll((nodes) => nodes.map((node) => node.getBoundingClientRect().width));
        assert.ok(adminControlWidths.every((width) => width >= 300), 'mobile admin controls were compressed: ' + adminControlWidths.join(','));
        await mobile.screenshot({ path: path.join(screenshotDir, 'usage-mobile-390x844.png'), fullPage: true });

        const disabled = await browser.newPage({ viewport: { width: 390, height: 844 } });
        await installApiFixture(disabled, 'disabled');
        await disabled.goto(baseUrl, { waitUntil: 'networkidle' });
        await disabled.waitForTimeout(50);
        assert.equal(await disabled.locator('#usage-section').evaluate((node) => node.classList.contains('hidden')), true);
        assert.equal(await disabled.locator('#admin-usage').evaluate((node) => node.classList.contains('hidden')), true);
        assert.equal(await disabled.locator('#create-form').isVisible(), true);
        await assertNoHorizontalOverflow(disabled, 'feature-disabled mobile');

        console.log('shortlink-usage-ui-ok');
        console.log('screenshots=' + screenshotDir);
    } finally {
        await browser.close();
        await new Promise((resolve) => server.close(resolve));
    }
};

run().catch((error) => {
    console.error(error.stack || error);
    process.exitCode = 1;
});
