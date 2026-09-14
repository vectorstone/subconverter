BEGIN;
SELECT pg_advisory_xact_lock(hashtext('subconverter:usage-schema:v1'));

CREATE TABLE IF NOT EXISTS shortlink_usage_bindings (
    id BIGSERIAL PRIMARY KEY,
    owner_subject TEXT NOT NULL REFERENCES shortlink_users(external_subject) ON DELETE CASCADE,
    provider_id TEXT NOT NULL,
    instance_id UUID NOT NULL,
    client_id BIGINT NOT NULL CHECK (client_id > 0),
    identity_fingerprint TEXT NOT NULL CHECK (identity_fingerprint ~ '^[0-9a-f]{64}$'),
    label TEXT NOT NULL CHECK (char_length(label) BETWEEN 1 AND 80),
    revision BIGINT NOT NULL DEFAULT 1 CHECK (revision > 0),
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    revoked_at TIMESTAMPTZ
);
CREATE UNIQUE INDEX IF NOT EXISTS shortlink_usage_active_client_uidx
    ON shortlink_usage_bindings(provider_id, instance_id, client_id) WHERE revoked_at IS NULL;
CREATE INDEX IF NOT EXISTS shortlink_usage_active_owner_idx
    ON shortlink_usage_bindings(owner_subject, id) WHERE revoked_at IS NULL;

CREATE TABLE IF NOT EXISTS shortlink_usage_cache (
    binding_id BIGINT PRIMARY KEY REFERENCES shortlink_usage_bindings(id) ON DELETE CASCADE,
    binding_revision BIGINT NOT NULL,
    snapshot JSONB,
    observed_at TIMESTAMPTZ,
    last_attempt_at TIMESTAMPTZ NOT NULL,
    last_error_code TEXT,
    invalidated_at TIMESTAMPTZ,
    CHECK ((snapshot IS NULL) = (observed_at IS NULL))
);

CREATE TABLE IF NOT EXISTS shortlink_usage_audit (
    id BIGSERIAL PRIMARY KEY,
    actor_subject TEXT NOT NULL,
    action TEXT NOT NULL,
    binding_id BIGINT,
    owner_subject TEXT NOT NULL,
    provider_id TEXT NOT NULL,
    client_id BIGINT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    request_id TEXT NOT NULL DEFAULT '',
    details JSONB NOT NULL DEFAULT '{}'::jsonb
);
CREATE INDEX IF NOT EXISTS shortlink_usage_audit_created_idx ON shortlink_usage_audit(created_at);
COMMIT;
