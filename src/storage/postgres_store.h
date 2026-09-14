#ifndef POSTGRES_STORE_H_INCLUDED
#define POSTGRES_STORE_H_INCLUDED

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <libpq-fe.h>

#include "usage/usage_types.h"

struct ShortLinkRecord
{
    std::string id;
    std::string code;
    std::string owner;
    std::string name;
    std::string target;
    std::string source_payload;
    std::string snapshot_payload;
    std::string response_headers;
    std::string content_type;
    std::string content_hash;
    int links_count = 0;
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0;
    std::int64_t expires_at = 0;
    std::int64_t revoked_at = 0;
};

struct ShortLinkUserRecord
{
    std::string subject;
    std::string email;
    std::string role;
    std::int64_t created_at = 0;
};

class PostgresStore
{
public:
    ~PostgresStore();

    bool open(const std::string &connection_string);
    bool ensure_schema();
    bool ensure_usage_schema();
    bool ready() const;

    bool ensure_user(const std::string &owner, const std::string &email = "", const std::string &role = "user");
    bool user_is_admin(const std::string &owner);
    bool list_users(std::vector<ShortLinkUserRecord> &records);
    bool set_user_role(const std::string &owner, const std::string &role);
    bool authenticate_api_key(const std::string &key_hash, std::string &owner);
    bool create_api_key(const std::string &owner, const std::string &key_hash, const std::string &name, std::int64_t expires_at, std::string &id);
    bool revoke_api_key(const std::string &owner, const std::string &id, bool all_owners = false);
    bool cleanup_expired(std::int64_t grace_seconds);

    bool create_short_link(const ShortLinkRecord &record, int max_active, int max_per_hour, std::string &id);
    bool get_short_link(const std::string &code, ShortLinkRecord &record);
    bool get_short_link_by_id(const std::string &owner, const std::string &id, ShortLinkRecord &record, bool all_owners = false);
    bool list_short_links(const std::string &owner, std::vector<ShortLinkRecord> &records, bool all_owners = false);
    bool get_download_sequence(const ShortLinkRecord &record, int &sequence);
    bool revoke_short_link(const std::string &owner, const std::string &id, bool all_owners = false);
    bool update_snapshot(const std::string &owner, const std::string &id, const std::string &snapshot_payload, const std::string &response_headers, const std::string &content_hash, std::int64_t updated_at);

    bool usage_user_exists(const std::string &owner);
    bool list_usage_bindings(const std::string &owner, bool all_owners, const std::string &cursor, int limit, std::vector<UsageBinding> &records);
    bool get_usage_binding(const std::string &id, UsageBinding &record);
    bool create_usage_binding(const UsageBinding &record, const std::string &actor, const std::string &request_id, int max_active, UsageBinding &created, std::string &error);
    bool rename_usage_binding(const std::string &id, const std::string &expected_revision, const std::string &label, const std::string &actor, const std::string &request_id, UsageBinding &updated, std::string &error);
    bool revoke_usage_binding(const std::string &id, const std::string &expected_revision, const std::string &actor, const std::string &request_id, std::string &error);
    bool record_usage_success(const UsageBinding &expected, const std::string &snapshot_json, std::int64_t observed_at, std::int64_t attempted_at);
    bool record_usage_failure(const UsageBinding &expected, const std::string &error_code, std::int64_t attempted_at, bool invalidate);
    bool cleanup_usage_audit(int retention_days);

private:
    PGconn *connection_ = nullptr;
    mutable std::mutex mutex_;
};

#endif // POSTGRES_STORE_H_INCLUDED
