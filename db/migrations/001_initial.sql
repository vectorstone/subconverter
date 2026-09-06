CREATE TABLE IF NOT EXISTS shortlink_users (
    external_subject TEXT PRIMARY KEY,
    email TEXT NOT NULL DEFAULT '',
    role TEXT NOT NULL DEFAULT 'user',
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS shortlink_api_keys (
    id BIGSERIAL PRIMARY KEY,
    owner_subject TEXT NOT NULL REFERENCES shortlink_users(external_subject) ON DELETE CASCADE,
    key_hash TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL DEFAULT '',
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    expires_at TIMESTAMPTZ,
    revoked_at TIMESTAMPTZ,
    last_used_at TIMESTAMPTZ
);

CREATE TABLE IF NOT EXISTS short_links (
    id BIGSERIAL PRIMARY KEY,
    owner_subject TEXT NOT NULL REFERENCES shortlink_users(external_subject) ON DELETE CASCADE,
    code TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL DEFAULT '',
    target TEXT NOT NULL DEFAULT 'clash',
    source_payload TEXT NOT NULL,
    snapshot_payload TEXT NOT NULL,
    response_headers TEXT NOT NULL DEFAULT '{}',
    content_type TEXT NOT NULL DEFAULT 'text/yaml; charset=utf-8',
    content_hash TEXT NOT NULL DEFAULT '',
    links_count INTEGER NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    expires_at TIMESTAMPTZ,
    revoked_at TIMESTAMPTZ
);

CREATE TABLE IF NOT EXISTS short_link_versions (
    id BIGSERIAL PRIMARY KEY,
    short_link_id BIGINT NOT NULL REFERENCES short_links(id) ON DELETE CASCADE,
    snapshot_payload TEXT NOT NULL,
    response_headers TEXT NOT NULL DEFAULT '{}',
    content_hash TEXT NOT NULL DEFAULT '',
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS short_links_owner_idx ON short_links(owner_subject, created_at DESC);
CREATE INDEX IF NOT EXISTS short_links_expiry_idx ON short_links(expires_at);
