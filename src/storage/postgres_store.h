#ifndef POSTGRES_STORE_H_INCLUDED
#define POSTGRES_STORE_H_INCLUDED

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <libpq-fe.h>

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

private:
    PGconn *connection_ = nullptr;
    mutable std::mutex mutex_;
};

#endif // POSTGRES_STORE_H_INCLUDED
