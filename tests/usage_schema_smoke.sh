#!/usr/bin/env bash
set -euo pipefail

: "${DATABASE_URL:?set DATABASE_URL to an isolated PostgreSQL database}"
schema="usage_smoke_${$}_$(date +%s)"
cleanup() {
    psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -c "DROP SCHEMA IF EXISTS ${schema} CASCADE" >/dev/null
}
trap cleanup EXIT

psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -c "CREATE SCHEMA ${schema}" >/dev/null
for pass in 1 2; do
    PGOPTIONS="-c search_path=${schema}" psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f db/migrations/001_initial.sql >/dev/null
    PGOPTIONS="-c search_path=${schema}" psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f db/migrations/002_usage.sql >/dev/null
done

PGOPTIONS="-c search_path=${schema}" psql "$DATABASE_URL" -v ON_ERROR_STOP=1 <<'SQL' >/dev/null
INSERT INTO shortlink_users(external_subject) VALUES ('usage-a'), ('usage-b');
INSERT INTO shortlink_usage_bindings(owner_subject,provider_id,instance_id,client_id,identity_fingerprint,label)
VALUES ('usage-a','test','11111111-1111-4111-8111-111111111111',1,repeat('a',64),'A');
DO $$
BEGIN
    BEGIN
        INSERT INTO shortlink_usage_bindings(owner_subject,provider_id,instance_id,client_id,identity_fingerprint,label)
        VALUES ('usage-b','test','11111111-1111-4111-8111-111111111111',1,repeat('b',64),'B');
        RAISE EXCEPTION 'active client uniqueness was not enforced';
    EXCEPTION WHEN unique_violation THEN NULL;
    END;
END $$;
INSERT INTO shortlink_usage_cache(binding_id,binding_revision,snapshot,observed_at,last_attempt_at)
SELECT id,revision,'{"used_bytes":"0"}'::jsonb,NOW(),NOW() FROM shortlink_usage_bindings;
DELETE FROM shortlink_usage_bindings;
DO $$ BEGIN IF EXISTS(SELECT 1 FROM shortlink_usage_cache) THEN RAISE EXCEPTION 'cache cascade failed'; END IF; END $$;
SQL

echo "usage schema smoke test passed"
