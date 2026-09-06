#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include <libpq-fe.h>

#include "utils/logger.h"
#include "postgres_store.h"

namespace
{
bool result_ok(PGresult *result, ExecStatusType expected)
{
    return result != nullptr && PQresultStatus(result) == expected;
}

std::string result_value(PGresult *result, int row, int column)
{
    if(!result || PQntuples(result) <= row || PQnfields(result) <= column || PQgetisnull(result, row, column))
        return "";
    return PQgetvalue(result, row, column);
}

bool exec_params(PGconn *connection, const std::string &sql, const std::vector<const char *> &values, PGresult **result)
{
    std::vector<int> lengths(values.size(), 0), formats(values.size(), 0);
    *result = PQexecParams(connection, sql.c_str(), static_cast<int>(values.size()), nullptr,
                           values.empty() ? nullptr : values.data(), lengths.data(), formats.data(), 0);
    return result_ok(*result, PGRES_COMMAND_OK) || result_ok(*result, PGRES_TUPLES_OK);
}

bool exec_command(PGconn *connection, const std::string &sql)
{
    PGresult *result = PQexec(connection, sql.c_str());
    const bool ok = result_ok(result, PGRES_COMMAND_OK);
    PQclear(result);
    return ok;
}

std::int64_t parse_timestamp(const std::string &value)
{
    if(value.empty())
        return 0;
    return std::strtoll(value.c_str(), nullptr, 10);
}

bool local_tm_safe(std::time_t timestamp, std::tm &value)
{
#if defined(_WIN32)
    return localtime_s(&value, &timestamp) == 0;
#else
    return localtime_r(&timestamp, &value) != nullptr;
#endif
}
}

PostgresStore::~PostgresStore()
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(connection_)
    {
        PQfinish(connection_);
        connection_ = nullptr;
    }
}

bool PostgresStore::open(const std::string &connection_string)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(connection_)
        return true;
    if(connection_string.empty())
        return false;
    connection_ = PQconnectdb(connection_string.c_str());
    if(!connection_ || PQstatus(connection_) != CONNECTION_OK)
    {
        if(connection_)
        {
            writeLog(0, "PostgreSQL connection failed: " + std::string(PQerrorMessage(connection_)), LOG_LEVEL_ERROR);
            PQfinish(connection_);
        }
        connection_ = nullptr;
        return false;
    }
    return true;
}

bool PostgresStore::ready() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return connection_ && PQstatus(connection_) == CONNECTION_OK;
}

bool PostgresStore::ensure_schema()
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_)
        return false;
    const char *schema = R"SQL(
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
)SQL";
    PGresult *result = PQexec(connection_, schema);
    const bool ok = result_ok(result, PGRES_COMMAND_OK);
    if(!ok)
        writeLog(0, "PostgreSQL schema initialization failed: " + std::string(PQerrorMessage(connection_)), LOG_LEVEL_ERROR);
    PQclear(result);
    return ok;
}

bool PostgresStore::ensure_user(const std::string &owner, const std::string &email, const std::string &role)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_ || owner.empty())
        return false;
    const char *values[] = {owner.c_str(), email.c_str(), role.c_str()};
    PGresult *result = nullptr;
    const bool ok = exec_params(connection_,
        "INSERT INTO shortlink_users(external_subject, email, role) VALUES($1, $2, CASE WHEN $3 = 'admin' THEN 'admin' ELSE 'user' END) ON CONFLICT(external_subject) DO UPDATE SET email = CASE WHEN $2 <> '' THEN $2 ELSE shortlink_users.email END, role = CASE WHEN $3 = 'admin' THEN 'admin' ELSE shortlink_users.role END, updated_at = NOW()",
        {values[0], values[1], values[2]}, &result);
    PQclear(result);
    return ok;
}

bool PostgresStore::user_is_admin(const std::string &owner)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_ || owner.empty())
        return false;
    const char *values[] = {owner.c_str()};
    PGresult *result = nullptr;
    const bool ok = exec_params(connection_, "SELECT role FROM shortlink_users WHERE external_subject = $1", {values[0]}, &result);
    const bool admin = ok && PQntuples(result) > 0 && result_value(result, 0, 0) == "admin";
    PQclear(result);
    return admin;
}

bool PostgresStore::list_users(std::vector<ShortLinkUserRecord> &records)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_)
        return false;
    PGresult *result = nullptr;
    const bool ok = exec_params(connection_, "SELECT external_subject, email, role, EXTRACT(EPOCH FROM created_at)::bigint::text FROM shortlink_users ORDER BY created_at ASC LIMIT 1000", {}, &result);
    if(ok)
    {
        for(int row = 0; row < PQntuples(result); row++)
        {
            ShortLinkUserRecord item;
            item.subject = result_value(result, row, 0);
            item.email = result_value(result, row, 1);
            item.role = result_value(result, row, 2);
            item.created_at = parse_timestamp(result_value(result, row, 3));
            records.emplace_back(std::move(item));
        }
    }
    PQclear(result);
    return ok;
}

bool PostgresStore::set_user_role(const std::string &owner, const std::string &role)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_ || owner.empty() || (role != "user" && role != "admin"))
        return false;
    const char *values[] = {owner.c_str(), role.c_str()};
    PGresult *result = nullptr;
    const bool ok = exec_params(connection_, "UPDATE shortlink_users SET role = $2, updated_at = NOW() WHERE external_subject = $1", {values[0], values[1]}, &result);
    const bool changed = ok && PQcmdTuples(result) && std::atoi(PQcmdTuples(result)) == 1;
    PQclear(result);
    return changed;
}

bool PostgresStore::authenticate_api_key(const std::string &key_hash, std::string &owner)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_ || key_hash.empty())
        return false;
    const char *values[] = {key_hash.c_str()};
    PGresult *result = nullptr;
    const bool ok = exec_params(connection_,
        "SELECT owner_subject FROM shortlink_api_keys WHERE key_hash = $1 AND revoked_at IS NULL AND (expires_at IS NULL OR expires_at > NOW())",
        {values[0]}, &result);
    if(ok && PQntuples(result) > 0)
    {
        owner = result_value(result, 0, 0);
        const char *update_values[] = {key_hash.c_str()};
        PGresult *update = nullptr;
        exec_params(connection_, "UPDATE shortlink_api_keys SET last_used_at = NOW() WHERE key_hash = $1", {update_values[0]}, &update);
        PQclear(update);
    }
    PQclear(result);
    return ok && !owner.empty();
}

bool PostgresStore::create_api_key(const std::string &owner, const std::string &key_hash, const std::string &name, std::int64_t expires_at, std::string &id)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_ || owner.empty() || key_hash.empty())
        return false;
    const std::string expiry = expires_at > 0 ? std::to_string(expires_at) : "";
    const char *values[] = {owner.c_str(), key_hash.c_str(), name.c_str(), expiry.c_str()};
    PGresult *result = nullptr;
    const bool ok = exec_params(connection_,
        "INSERT INTO shortlink_api_keys(owner_subject, key_hash, name, expires_at) VALUES($1, $2, $3, NULLIF(to_timestamp(NULLIF($4, '')::double precision), to_timestamp(0))) RETURNING id",
        {values[0], values[1], values[2], values[3]}, &result);
    if(ok && PQntuples(result) > 0)
        id = result_value(result, 0, 0);
    PQclear(result);
    return ok && !id.empty();
}

bool PostgresStore::revoke_api_key(const std::string &owner, const std::string &id, bool all_owners)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_ || owner.empty() || id.empty())
        return false;
    const char *values[] = {owner.c_str(), id.c_str()};
    PGresult *result = nullptr;
    const bool ok = all_owners
        ? exec_params(connection_, "UPDATE shortlink_api_keys SET revoked_at = NOW() WHERE id::text = $1 AND revoked_at IS NULL", {values[1]}, &result)
        : exec_params(connection_, "UPDATE shortlink_api_keys SET revoked_at = NOW() WHERE owner_subject = $1 AND id::text = $2 AND revoked_at IS NULL", {values[0], values[1]}, &result);
    const bool changed = ok && PQcmdTuples(result) && std::atoi(PQcmdTuples(result)) == 1;
    PQclear(result);
    return changed;
}

bool PostgresStore::cleanup_expired(std::int64_t grace_seconds)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_)
        return false;
    const std::string grace = std::to_string(std::max<std::int64_t>(grace_seconds, 0));
    const char *values[] = {grace.c_str()};
    PGresult *result = nullptr;
    const bool ok = exec_params(connection_, "DELETE FROM short_links WHERE (expires_at IS NOT NULL AND expires_at < NOW() - make_interval(secs => $1::double precision)) OR (revoked_at IS NOT NULL AND revoked_at < NOW() - make_interval(secs => $1::double precision))", {values[0]}, &result);
    PQclear(result);
    return ok;
}

bool PostgresStore::create_short_link(const ShortLinkRecord &record, int max_active, int max_per_hour, std::string &id)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_ || record.owner.empty() || record.code.empty() || record.snapshot_payload.empty())
        return false;

    if(!exec_command(connection_, "BEGIN"))
        return false;
    bool ok = true;
    PGresult *result = nullptr;
    const char *owner_value[] = {record.owner.c_str()};
    ok = exec_params(connection_, "SELECT pg_advisory_xact_lock(hashtext($1))", {owner_value[0]}, &result);
    PQclear(result);
    if(ok)
    {
        result = nullptr;
        ok = exec_params(connection_, "SELECT COUNT(*) FROM short_links WHERE owner_subject = $1 AND revoked_at IS NULL AND (expires_at IS NULL OR expires_at > NOW())", {owner_value[0]}, &result);
        const int active = ok && PQntuples(result) > 0 ? std::atoi(result_value(result, 0, 0).c_str()) : max_active;
        PQclear(result);
        if(active >= max_active)
            ok = false;
    }
    if(ok)
    {
        result = nullptr;
        ok = exec_params(connection_, "SELECT COUNT(*) FROM short_links WHERE owner_subject = $1 AND created_at > NOW() - INTERVAL '1 hour'", {owner_value[0]}, &result);
        const int recent = ok && PQntuples(result) > 0 ? std::atoi(result_value(result, 0, 0).c_str()) : max_per_hour;
        PQclear(result);
        if(recent >= max_per_hour)
            ok = false;
    }
    if(ok)
    {
        const std::string expiry = record.expires_at > 0 ? std::to_string(record.expires_at) : "";
        const std::string links_count = std::to_string(record.links_count);
        const char *values[] = {record.owner.c_str(), record.code.c_str(), record.name.c_str(), record.target.c_str(), record.source_payload.c_str(), record.snapshot_payload.c_str(), record.response_headers.c_str(), record.content_type.c_str(), record.content_hash.c_str(), links_count.c_str(), expiry.c_str()};
        result = nullptr;
        ok = exec_params(connection_,
            "INSERT INTO short_links(owner_subject, code, name, target, source_payload, snapshot_payload, response_headers, content_type, content_hash, links_count, expires_at) VALUES($1, $2, $3, $4, $5, $6, $7, $8, $9, $10::integer, NULLIF(to_timestamp(NULLIF($11, '')::double precision), to_timestamp(0))) RETURNING id",
            {values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8], values[9], values[10]}, &result);
        if(ok && PQntuples(result) > 0)
            id = result_value(result, 0, 0);
        PQclear(result);
    }
    if(ok && !id.empty())
        ok = exec_command(connection_, "COMMIT");
    else
        exec_command(connection_, "ROLLBACK");
    return ok && !id.empty();
}

bool PostgresStore::get_short_link(const std::string &code, ShortLinkRecord &record)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_ || code.empty())
        return false;
    const char *values[] = {code.c_str()};
    PGresult *result = nullptr;
    const bool ok = exec_params(connection_,
        "SELECT id::text, code, owner_subject, name, target, source_payload, snapshot_payload, response_headers, content_type, content_hash, links_count::text, EXTRACT(EPOCH FROM created_at)::bigint::text, EXTRACT(EPOCH FROM updated_at)::bigint::text, COALESCE(EXTRACT(EPOCH FROM expires_at)::bigint::text, ''), COALESCE(EXTRACT(EPOCH FROM revoked_at)::bigint::text, '') FROM short_links WHERE code = $1",
        {values[0]}, &result);
    if(ok && PQntuples(result) > 0)
    {
        record.id = result_value(result, 0, 0);
        record.code = result_value(result, 0, 1);
        record.owner = result_value(result, 0, 2);
        record.name = result_value(result, 0, 3);
        record.target = result_value(result, 0, 4);
        record.source_payload = result_value(result, 0, 5);
        record.snapshot_payload = result_value(result, 0, 6);
        record.response_headers = result_value(result, 0, 7);
        record.content_type = result_value(result, 0, 8);
        record.content_hash = result_value(result, 0, 9);
        record.links_count = std::atoi(result_value(result, 0, 10).c_str());
        record.created_at = parse_timestamp(result_value(result, 0, 11));
        record.updated_at = parse_timestamp(result_value(result, 0, 12));
        record.expires_at = parse_timestamp(result_value(result, 0, 13));
        record.revoked_at = parse_timestamp(result_value(result, 0, 14));
    }
    PQclear(result);
    return ok && !record.id.empty();
}

bool PostgresStore::get_short_link_by_id(const std::string &owner, const std::string &id, ShortLinkRecord &record, bool all_owners)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_ || owner.empty() || id.empty())
        return false;
    const char *values[] = {owner.c_str(), id.c_str()};
    PGresult *result = nullptr;
    const bool ok = all_owners
        ? exec_params(connection_, "SELECT id::text, code, owner_subject, name, target, source_payload, snapshot_payload, response_headers, content_type, content_hash, links_count::text, EXTRACT(EPOCH FROM created_at)::bigint::text, EXTRACT(EPOCH FROM updated_at)::bigint::text, COALESCE(EXTRACT(EPOCH FROM expires_at)::bigint::text, ''), COALESCE(EXTRACT(EPOCH FROM revoked_at)::bigint::text, '') FROM short_links WHERE id::text = $1", {values[1]}, &result)
        : exec_params(connection_, "SELECT id::text, code, owner_subject, name, target, source_payload, snapshot_payload, response_headers, content_type, content_hash, links_count::text, EXTRACT(EPOCH FROM created_at)::bigint::text, EXTRACT(EPOCH FROM updated_at)::bigint::text, COALESCE(EXTRACT(EPOCH FROM expires_at)::bigint::text, ''), COALESCE(EXTRACT(EPOCH FROM revoked_at)::bigint::text, '') FROM short_links WHERE owner_subject = $1 AND id::text = $2", {values[0], values[1]}, &result);
    if(ok && PQntuples(result) > 0)
    {
        record.id = result_value(result, 0, 0);
        record.code = result_value(result, 0, 1);
        record.owner = result_value(result, 0, 2);
        record.name = result_value(result, 0, 3);
        record.target = result_value(result, 0, 4);
        record.source_payload = result_value(result, 0, 5);
        record.snapshot_payload = result_value(result, 0, 6);
        record.response_headers = result_value(result, 0, 7);
        record.content_type = result_value(result, 0, 8);
        record.content_hash = result_value(result, 0, 9);
        record.links_count = std::atoi(result_value(result, 0, 10).c_str());
        record.created_at = parse_timestamp(result_value(result, 0, 11));
        record.updated_at = parse_timestamp(result_value(result, 0, 12));
        record.expires_at = parse_timestamp(result_value(result, 0, 13));
        record.revoked_at = parse_timestamp(result_value(result, 0, 14));
    }
    PQclear(result);
    return ok && !record.id.empty();
}

bool PostgresStore::list_short_links(const std::string &owner, std::vector<ShortLinkRecord> &records, bool all_owners)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_ || owner.empty())
        return false;
    const char *values[] = {owner.c_str()};
    PGresult *result = nullptr;
    const bool ok = all_owners
        ? exec_params(connection_, "SELECT id::text, code, owner_subject, name, target, content_type, content_hash, links_count::text, EXTRACT(EPOCH FROM created_at)::bigint::text, EXTRACT(EPOCH FROM updated_at)::bigint::text, COALESCE(EXTRACT(EPOCH FROM expires_at)::bigint::text, ''), COALESCE(EXTRACT(EPOCH FROM revoked_at)::bigint::text, '') FROM short_links ORDER BY created_at DESC LIMIT 200", {}, &result)
        : exec_params(connection_, "SELECT id::text, code, owner_subject, name, target, content_type, content_hash, links_count::text, EXTRACT(EPOCH FROM created_at)::bigint::text, EXTRACT(EPOCH FROM updated_at)::bigint::text, COALESCE(EXTRACT(EPOCH FROM expires_at)::bigint::text, ''), COALESCE(EXTRACT(EPOCH FROM revoked_at)::bigint::text, '') FROM short_links WHERE owner_subject = $1 ORDER BY created_at DESC LIMIT 200", {values[0]}, &result);
    if(ok)
    {
        for(int row = 0; row < PQntuples(result); row++)
        {
            ShortLinkRecord item;
            item.id = result_value(result, row, 0);
            item.code = result_value(result, row, 1);
            item.owner = result_value(result, row, 2);
            item.name = result_value(result, row, 3);
            item.target = result_value(result, row, 4);
            item.content_type = result_value(result, row, 5);
            item.content_hash = result_value(result, row, 6);
            item.links_count = std::atoi(result_value(result, row, 7).c_str());
            item.created_at = parse_timestamp(result_value(result, row, 8));
            item.updated_at = parse_timestamp(result_value(result, row, 9));
            item.expires_at = parse_timestamp(result_value(result, row, 10));
            item.revoked_at = parse_timestamp(result_value(result, row, 11));
            records.emplace_back(std::move(item));
        }
    }
    PQclear(result);
    return ok;
}

bool PostgresStore::get_download_sequence(const ShortLinkRecord &record, int &sequence)
{
    std::lock_guard<std::mutex> guard(mutex_);
    sequence = 1;
    if(!connection_ || record.id.empty() || record.updated_at <= 0)
        return false;

    const std::time_t raw_time = static_cast<std::time_t>(record.updated_at);
    std::tm local_day{};
    if(!local_tm_safe(raw_time, local_day))
        return false;
    local_day.tm_hour = 0;
    local_day.tm_min = 0;
    local_day.tm_sec = 0;
    const std::time_t day_start = std::mktime(&local_day);
    if(day_start < 0)
        return false;
    local_day.tm_mday += 1;
    const std::time_t day_end = std::mktime(&local_day);
    if(day_end <= day_start)
        return false;

    const std::string start = std::to_string(static_cast<long long>(day_start));
    const std::string end = std::to_string(static_cast<long long>(day_end));
    const std::string updated = std::to_string(record.updated_at);
    const char *values[] = {record.owner.c_str(), start.c_str(), end.c_str(), updated.c_str(), record.id.c_str()};
    PGresult *result = nullptr;
    const bool ok = exec_params(connection_,
        "SELECT COUNT(*)::text FROM short_links WHERE owner_subject = $1 AND revoked_at IS NULL AND (expires_at IS NULL OR expires_at > NOW()) AND updated_at >= to_timestamp($2::double precision) AND updated_at < to_timestamp($3::double precision) AND (EXTRACT(EPOCH FROM updated_at)::bigint < $4::bigint OR (EXTRACT(EPOCH FROM updated_at)::bigint = $4::bigint AND id <= $5::bigint))",
        {values[0], values[1], values[2], values[3], values[4]}, &result);
    if(ok && PQntuples(result) > 0)
        sequence = std::max(1, std::atoi(result_value(result, 0, 0).c_str()));
    PQclear(result);
    return ok && sequence > 0;
}

bool PostgresStore::revoke_short_link(const std::string &owner, const std::string &id, bool all_owners)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_ || owner.empty() || id.empty())
        return false;
    const char *values[] = {owner.c_str(), id.c_str()};
    PGresult *result = nullptr;
    const bool ok = all_owners
        ? exec_params(connection_, "UPDATE short_links SET revoked_at = NOW(), updated_at = NOW() WHERE id::text = $1 AND revoked_at IS NULL", {values[1]}, &result)
        : exec_params(connection_, "UPDATE short_links SET revoked_at = NOW(), updated_at = NOW() WHERE owner_subject = $1 AND id::text = $2 AND revoked_at IS NULL", {values[0], values[1]}, &result);
    const bool changed = ok && PQcmdTuples(result) && std::atoi(PQcmdTuples(result)) == 1;
    PQclear(result);
    return changed;
}

bool PostgresStore::update_snapshot(const std::string &owner, const std::string &id, const std::string &snapshot_payload, const std::string &response_headers, const std::string &content_hash, std::int64_t updated_at)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if(!connection_ || owner.empty() || id.empty() || snapshot_payload.empty())
        return false;
    const std::string timestamp = std::to_string(updated_at);
    const char *values[] = {owner.c_str(), id.c_str(), snapshot_payload.c_str(), response_headers.c_str(), content_hash.c_str(), timestamp.c_str()};
    if(!exec_command(connection_, "BEGIN"))
        return false;
    PGresult *result = nullptr;
    bool ok = exec_params(connection_, "INSERT INTO short_link_versions(short_link_id, snapshot_payload, response_headers, content_hash) SELECT id, snapshot_payload, response_headers, content_hash FROM short_links WHERE owner_subject = $1 AND id::text = $2 AND revoked_at IS NULL", {values[0], values[1]}, &result);
    PQclear(result);
    if(ok)
    {
        result = nullptr;
        ok = exec_params(connection_, "UPDATE short_links SET snapshot_payload = $3, response_headers = $4, content_hash = $5, updated_at = to_timestamp($6::double precision) WHERE owner_subject = $1 AND id::text = $2 AND revoked_at IS NULL", {values[0], values[1], values[2], values[3], values[4], values[5]}, &result);
        const bool changed = ok && PQcmdTuples(result) && std::atoi(PQcmdTuples(result)) == 1;
        PQclear(result);
        ok = changed;
    }
    if(ok)
        ok = exec_command(connection_, "COMMIT");
    else
        exec_command(connection_, "ROLLBACK");
    return ok;
}
