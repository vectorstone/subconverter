#!/usr/bin/env node
'use strict';

// Portal regression for the short-link row actions and gateway failures:
//  - hard delete calls POST /api/short-links/<id>/delete and clears the preview
//  - a gateway 429 (HTML body, no Retry-After) keeps the list and retries shortly
//  - a 503 HTML error page is never rendered into the UI (list, preview, delete, admin form)
//  - the admin user form reports failures instead of raising an unhandled rejection
//
// Run with Playwright available either in node_modules or through PLAYWRIGHT_PATH:
//   PLAYWRIGHT_PATH=/path/to/node_modules/playwright node tests/shortlink_portal_actions_ui.cjs

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

const webRoot = path.resolve(__dirname, '..', 'base', 'web');
const code = 'Abcdefghijklmnopqrstuvwx';
const html503 = '<html> <head><title>503 Service Temporarily Unavailable</title></head> <body>'
    + ' <center><h1>503 Service Temporarily Unavailable</h1></center> <hr><center>nginx/1.24.0 (Ubuntu)</center>'
    + ' </body> </html>';

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

const run = async () => {
    const server = await startStaticServer();
    const baseUrl = 'http://127.0.0.1:' + server.address().port;
    const item = {
        id: 7,
        name: 'portal-actions',
        target: 'clash',
        links_count: 1,
        short_url: baseUrl + '/s/' + code,
        download_url: baseUrl + '/s/' + code + '?download=1',
        created_at: 1788912000,
        updated_at: 1788912000,
        expires_at: 0,
        revoked_at: 0,
        owner: 'admin'
    };

    let listMode = 'ok';
    let deleteMode = 'ok';
    let previewMode = 'ok';
    let adminMode = 'ok';
    let deleteHits = 0;

    const browser = await chromium.launch({ headless: true, executablePath: process.env.PLAYWRIGHT_CHROME_PATH || undefined });
    try {
        const page = await browser.newPage();
        page.on('dialog', (dialog) => dialog.accept());
        const pageErrors = [];
        page.on('pageerror', (error) => pageErrors.push(error.message));

        await page.route('**/api/**', async (route) => {
            const request = route.request();
            const url = new URL(request.url());
            const json = (status, body) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
            const html = (status) => route.fulfill({ status, contentType: 'text/html', body: html503 });

            if (url.pathname === '/api/short-links' && request.method() === 'GET') {
                if (listMode === 'html503') return html(503);
                if (listMode === 'plain429') return html(429);
                return json(200, { items: [item] });
            }
            if (url.pathname === '/api/short-links/7/delete' && request.method() === 'POST') {
                deleteHits += 1;
                if (deleteMode === 'plain429') return html(429);
                return json(200, { status: 'deleted' });
            }
            if (url.pathname === '/api/short-links' && request.method() === 'POST') {
                return json(201, {
                    id: 9,
                    short_url: item.short_url,
                    preview_url: item.short_url,
                    download_url: item.download_url,
                    links_count: 1,
                    expires_at: 0
                });
            }
            if (url.pathname === '/api/admin/users' && request.method() === 'GET') return json(200, { items: [] });
            if (url.pathname === '/api/admin/users' && request.method() === 'POST') {
                if (adminMode === 'html500') return html(500);
                return json(200, { status: 'updated' });
            }
            return json(401, { error: 'authentication required' });
        });

        await page.route('**/s/**', async (route) => {
            if (previewMode === 'html503') return route.fulfill({ status: 503, contentType: 'text/html', body: html503 });
            return route.fulfill({ status: 200, contentType: 'text/yaml; charset=utf-8', body: 'proxies: []\n' });
        });

        await page.goto(baseUrl, { waitUntil: 'domcontentloaded' });
        await page.waitForSelector('.link-row');
        const messageText = () => page.locator('#message').innerText();
        const assertNoHtml = (text) => assert.ok(!/<html|<body|nginx\//i.test(text), 'HTML leaked into the UI: ' + text);

        // Show the previewed link first so the "delete clears the card" assertion is meaningful.
        await page.fill('#links', 'ss://YWVzLTEyOC1nY206Zml4dHVyZQ==@198.51.100.10:443#x');
        await page.locator('#create-form button[type="submit"]').click();
        await page.locator('#result-card:not(.hidden)').waitFor();
        assert.equal(await page.locator('#short-url').inputValue(), item.short_url, 'fixture must reuse the row short URL');

        await page.getByRole('button', { name: '删除' }).click();
        await page.waitForFunction(() => document.querySelector('#message').textContent.includes('已删除'));
        assert.equal(deleteHits, 1, 'delete must call POST /api/short-links/<id>/delete once');
        assertNoHtml(await messageText());
        assert.equal(await page.locator('#result-card.hidden').count(), 1, 'deleting the previewed link must hide the result card');

        await page.waitForSelector('.link-row');
        listMode = 'plain429';
        await page.locator('#refresh-button').click();
        await page.waitForFunction(() => document.querySelector('#message').textContent.includes('请求较频繁'));
        let text = await messageText();
        assertNoHtml(text);
        assert.match(text, /5 秒/, 'a gateway 429 without Retry-After must fall back to the short backoff');
        assert.equal(await page.locator('.link-row').count(), 1, 'a 429 must keep the existing rows');

        listMode = 'html503';
        await page.locator('#refresh-button').click();
        await page.waitForFunction(() => document.querySelector('#message').textContent.includes('加载失败'));
        text = await messageText();
        assertNoHtml(text);
        assert.match(text, /503/);
        assert.match(text, /已保留当前列表/);
        assert.equal(await page.locator('.link-row').count(), 1, 'a 503 must keep the existing rows');

        listMode = 'ok';
        previewMode = 'html503';
        await page.fill('#links', 'ss://YWVzLTEyOC1nY206Zml4dHVyZQ==@198.51.100.10:443#x');
        await page.locator('#create-form button[type="submit"]').click();
        await page.waitForFunction(() => document.querySelector('#message').textContent.includes('预览失败'));
        text = await messageText();
        assertNoHtml(text);
        assert.match(text, /预览失败（503）/);
        assert.equal((await page.locator('#preview').innerText()).trim(), '', 'the preview must stay empty on failure');

        await page.waitForSelector('.link-row');
        deleteMode = 'plain429';
        await page.getByRole('button', { name: '删除' }).click();
        await page.waitForFunction(() => document.querySelector('#message').textContent.includes('请求较频繁'));
        text = await messageText();
        assertNoHtml(text);
        assert.match(text, /请求较频繁（429）/);

        adminMode = 'html500';
        await page.locator('#admin-card:not(.hidden)').waitFor();
        await page.locator('#admin-subject').fill('someone@example.com');
        await page.locator('#admin-user-form').evaluate((form) => form.requestSubmit());
        await page.waitForFunction(() => document.querySelector('#admin-message').textContent.includes('保存用户失败'));
        const adminText = await page.locator('#admin-message').innerText();
        assertNoHtml(adminText);
        assert.match(adminText, /保存用户失败（500）/);
        assert.ok(await page.locator('#admin-user-form button[type="submit"]').isEnabled(), 'the admin submit button must be re-enabled after a failure');

        await page.waitForTimeout(200);
        assert.deepEqual(pageErrors, [], 'unexpected page errors: ' + pageErrors.join('; '));
        console.log('shortlink-portal-actions-ui-ok');
    } finally {
        await browser.close();
        server.close();
    }
};

run().catch((error) => {
    console.error('shortlink-portal-actions-ui-failed:', error);
    process.exit(1);
});
