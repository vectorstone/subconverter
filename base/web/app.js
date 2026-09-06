(() => {
    const form = document.querySelector('#create-form');
    const nameInput = document.querySelector('#name');
    const linksInput = document.querySelector('#links');
    const expiresInput = document.querySelector('#expires');
    const message = document.querySelector('#message');
    const resultCard = document.querySelector('#result-card');
    const shortUrl = document.querySelector('#short-url');
    const previewLink = document.querySelector('#preview-link');
    const downloadLink = document.querySelector('#download-link');
    const preview = document.querySelector('#preview');
    const resultMeta = document.querySelector('#result-meta');
    const linksList = document.querySelector('#links-list');
    const adminCard = document.querySelector('#admin-card');
    const usersList = document.querySelector('#users-list');
    const adminUserForm = document.querySelector('#admin-user-form');

    const setMessage = (text, error) => {
        message.textContent = text || '';
        message.className = 'message' + (error ? ' error' : '');
    };

    const copyText = async (value) => {
        await navigator.clipboard.writeText(value);
        setMessage('已复制到剪贴板。', false);
    };

    const formatDate = (timestamp) => {
        if (!timestamp) return '永久';
        return new Date(timestamp * 1000).toLocaleString();
    };

    const authHeaders = () => ({});

    const loadPreview = async (url) => {
        preview.textContent = '加载中……';
        const response = await fetch(url, { cache: 'no-store' });
        const content = await response.text();
        if (!response.ok) throw new Error(content || ('预览失败（' + response.status + '）'));
        const limit = 200000;
        preview.textContent = content.length > limit
            ? content.slice(0, limit) + '\n\n……预览已截断，完整内容请下载……'
            : content;
    };

    const loadList = async () => {
        try {
            const response = await fetch('/api/short-links', { cache: 'no-store', headers: authHeaders() });
            if (!response.ok) throw new Error((await response.text()) || ('加载失败（' + response.status + '）'));
            const data = await response.json();
            renderList(data.items || []);
        } catch (error) {
            linksList.replaceChildren();
            const p = document.createElement('p');
            p.className = 'muted';
            p.textContent = error.message;
            linksList.append(p);
        }
    };

    const renderList = (items) => {
        linksList.replaceChildren();
        if (!items.length) {
            const p = document.createElement('p');
            p.className = 'muted';
            p.textContent = '暂无短链。';
            linksList.append(p);
            return;
        }
        for (const item of items) {
            const row = document.createElement('div');
            row.className = 'link-row';
            const body = document.createElement('div');
            const title = document.createElement('strong');
            title.textContent = item.name || '未命名短链';
            const detail = document.createElement('p');
            const state = item.revoked_at ? '已撤销' : (item.expires_at && item.expires_at * 1000 < Date.now() ? '已过期' : '有效');
            detail.textContent = (item.owner ? item.owner + ' · ' : '') + item.links_count + ' 个输入 · 更新于 ' + formatDate(item.updated_at) + ' · ' + state;
            const url = document.createElement('code');
            url.textContent = item.short_url;
            body.append(title, detail, url);
            const actions = document.createElement('div');
            actions.className = 'row-actions';
            const copy = document.createElement('button');
            copy.className = 'secondary';
            copy.textContent = '复制';
            copy.onclick = () => copyText(item.short_url);
            const revoke = document.createElement('button');
            revoke.className = 'danger';
            revoke.textContent = '撤销';
            revoke.disabled = Boolean(item.revoked_at);
            revoke.onclick = async () => {
                if (!confirm('确认撤销这条短链？')) return;
                const response = await fetch('/api/short-links/' + encodeURIComponent(item.id), { method: 'DELETE', headers: authHeaders() });
                if (!response.ok) throw new Error(await response.text());
                await loadList();
            };
            const refresh = document.createElement('button');
            refresh.className = 'secondary';
            refresh.textContent = '刷新';
            refresh.disabled = Boolean(item.revoked_at);
            refresh.onclick = async () => {
                const response = await fetch('/api/short-links/' + encodeURIComponent(item.id) + '/refresh', { method: 'POST', headers: authHeaders() });
                if (!response.ok) throw new Error(await response.text());
                await loadList();
                if (shortUrl.value === item.short_url) await loadPreview(item.short_url);
            };
            actions.append(copy, refresh, revoke);
            row.append(body, actions);
            linksList.append(row);
        }
    };

    const loadAdminUsers = async () => {
        if (!adminCard || !usersList) return;
        const response = await fetch('/api/admin/users', { cache: 'no-store' });
        if (!response.ok) return;
        const data = await response.json();
        adminCard.classList.remove('hidden');
        usersList.replaceChildren();
        for (const user of data.items || []) {
            const row = document.createElement('div');
            row.className = 'link-row';
            const text = document.createElement('span');
            text.textContent = user.subject + ' · ' + (user.email || '-') + ' · ' + user.role;
            row.append(text);
            usersList.append(row);
        }
    };

    form.addEventListener('submit', async (event) => {
        event.preventDefault();
        const links = linksInput.value.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
        if (!links.length) {
            setMessage('至少输入一个节点或订阅链接。', true);
            return;
        }
        setMessage('正在转换并保存短链……', false);
        resultCard.classList.add('hidden');
        try {
            const response = await fetch('/api/short-links', {
                method: 'POST',
                headers: Object.assign({ 'Content-Type': 'application/json' }, authHeaders()),
                body: JSON.stringify({ name: nameInput.value.trim(), target: 'clash', expires_in: Number(expiresInput.value), links })
            });
            const data = await response.json().catch(() => ({}));
            if (!response.ok) throw new Error(data.error || ('创建失败（' + response.status + '）'));
            shortUrl.value = data.short_url;
            previewLink.href = data.preview_url;
            downloadLink.href = data.download_url;
            resultMeta.textContent = data.links_count + ' 个输入 · 过期时间：' + formatDate(data.expires_at);
            resultCard.classList.remove('hidden');
            await loadPreview(data.preview_url);
            setMessage('短链创建成功。', false);
            await loadList();
        } catch (error) {
            setMessage(error.message, true);
        }
    });

    document.querySelector('#copy-button').onclick = () => copyText(shortUrl.value);
    document.querySelector('#clear-button').onclick = () => {
        linksInput.value = '';
        nameInput.value = '';
        preview.textContent = '';
        resultCard.classList.add('hidden');
        setMessage('', false);
    };
    document.querySelector('#refresh-button').onclick = loadList;
    if (adminUserForm) adminUserForm.addEventListener('submit', async (event) => {
        event.preventDefault();
        const response = await fetch('/api/admin/users', {
            method: 'POST',
            headers: Object.assign({ 'Content-Type': 'application/json' }, authHeaders()),
            body: JSON.stringify({ subject: document.querySelector('#admin-subject').value.trim(), email: document.querySelector('#admin-email').value.trim(), role: document.querySelector('#admin-role').value })
        });
        if (!response.ok) throw new Error(await response.text());
        await loadAdminUsers();
    });
    loadList();
    loadAdminUsers();
})();
