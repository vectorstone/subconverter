#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <set>

#include <curl/curl.h>
#include <rapidjson/document.h>

#include "storage/postgres_store.h"
#include "utils/logger.h"
#include "utils/string.h"
#include "utils/system.h"
#include "sui_usage_client.h"
#include "usage_service.h"

namespace
{
UsageService service;
bool env_bool(const std::string &name)
{
    const std::string value = toLower(getEnv(name));
    return value == "1" || value == "true" || value == "yes" || value == "on";
}
int env_int(const std::string &name, int fallback)
{
    const std::string value = getEnv(name);
    return value.empty() ? fallback : to_int(value, fallback);
}
std::int64_t now_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
bool provider_id_valid(const std::string &id)
{
    static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$");
    return std::regex_match(id, pattern);
}
} // namespace

UsageService::~UsageService()
{
    shutdown();
}
UsageService &usageService()
{
    return service;
}

bool UsageService::load_providers(const std::string &path)
{
    std::ifstream input(path);
    if (!input)
        return false;
    std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (json.size() > 64 * 1024)
        return false;
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("schema_version") || !doc["schema_version"].IsInt() ||
        doc["schema_version"].GetInt() != 1 || !doc.HasMember("providers") || !doc["providers"].IsArray() ||
        doc["providers"].Empty() || doc["providers"].Size() > 4)
        return false;
    std::set<std::string> ids;
    for (const auto &value : doc["providers"].GetArray())
    {
        if (!value.IsObject())
            return false;
        for (const char *key : {"provider_id", "label", "endpoint", "ca_file", "client_cert_file", "client_key_file"})
            if (!value.HasMember(key) || !value[key].IsString() || value[key].GetStringLength() == 0)
                return false;
        UsageProvider p{value["provider_id"].GetString(),      value["label"].GetString(),
                        value["endpoint"].GetString(),         value["ca_file"].GetString(),
                        value["client_cert_file"].GetString(), value["client_key_file"].GetString()};
        if (!provider_id_valid(p.provider_id) || !ids.insert(p.provider_id).second ||
            !startsWith(p.endpoint, "https://") || p.label.size() > 80)
            return false;
        providers_.push_back(std::move(p));
    }
    return true;
}

bool UsageService::initialize(PostgresStore &store)
{
    enabled_ = env_bool("SHORTLINK_USAGE_ENABLED");
    if (!enabled_)
        return true;
    poll_seconds_ = std::max(30, env_int("SHORTLINK_USAGE_POLL_SECONDS", 30));
    fresh_seconds_ = std::max(poll_seconds_, env_int("SHORTLINK_USAGE_FRESH_SECONDS", 60));
    stale_seconds_ = env_int("SHORTLINK_USAGE_STALE_SECONDS", 900);
    audit_days_ = std::max(1, env_int("SHORTLINK_USAGE_AUDIT_DAYS", 90));
    if (stale_seconds_ <= fresh_seconds_ || !load_providers(getEnv("SHORTLINK_USAGE_PROVIDERS_FILE")) ||
        !store.ensure_usage_schema())
    {
        writeLog(0, "Usage service configuration or schema is unavailable; short links remain enabled.",
                 LOG_LEVEL_ERROR);
        return false;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    store_ = &store;
    ready_ = true;
    stopped_ = false;
    thread_ = std::thread(&UsageService::run, this);
    writeLog(0, "Short-link usage service initialized.", LOG_LEVEL_INFO);
    return true;
}

void UsageService::shutdown()
{
    {
        std::lock_guard<std::mutex> guard(mutex_);
        stopped_ = true;
        cv_.notify_all();
    }
    if (thread_.joinable())
        thread_.join();
    ready_ = false;
}

void UsageService::wake()
{
    std::lock_guard<std::mutex> guard(mutex_);
    wake_requested_ = true;
    cv_.notify_all();
}

const UsageProvider *UsageService::find_provider(const std::string &id) const
{
    auto it =
        std::find_if(providers_.begin(), providers_.end(), [&](const UsageProvider &p) { return p.provider_id == id; });
    return it == providers_.end() ? nullptr : &*it;
}

bool UsageService::query_one(const std::string &provider_id, const std::string &client_id, UsageRemoteItem &item,
                             std::string &instance_id, std::int64_t &observed_at, std::string &error) const
{
    const UsageProvider *provider = find_provider(provider_id);
    if (!provider)
    {
        error = "provider_not_found";
        return false;
    }
    UsageRemoteResponse response;
    SuiUsageClient client;
    if (!client.query(*provider, {client_id}, response, error))
        return false;
    if (!response.missing.empty())
    {
        error = "client_missing";
        return false;
    }
    if (!response.invalid.empty() || response.items.size() != 1)
    {
        error = "invalid_data";
        return false;
    }
    item = std::move(response.items.front());
    instance_id = response.instance_id;
    observed_at = response.observed_at;
    return true;
}

void UsageService::run()
{
    while (true)
    {
        collect_once();
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(poll_seconds_), [&] { return stopped_ || wake_requested_; });
        if (stopped_)
            break;
        wake_requested_ = false;
    }
}

void UsageService::collect_once()
{
    if (!store_)
        return;
    std::vector<UsageBinding> all;
    std::string cursor;
    while (true)
    {
        std::vector<UsageBinding> page;
        if (!store_->list_usage_bindings("", true, cursor, 101, page))
            return;
        all.insert(all.end(), page.begin(), page.end());
        if (page.size() < 101)
            break;
        cursor = page.back().id;
        if (all.size() > 4000)
            return;
    }
    std::map<std::string, std::vector<UsageBinding>> grouped;
    for (auto &b : all)
        if (!b.invalidated)
            grouped[b.provider_id].push_back(std::move(b));
    const std::int64_t attempted = now_seconds();
    for (auto &[provider_id, bindings] : grouped)
    {
        ProviderBackoff &backoff = backoff_[provider_id];
        if (attempted < backoff.next_attempt_at)
            continue;
        const UsageProvider *provider = find_provider(provider_id);
        if (!provider)
        {
            for (const auto &b : bindings)
                store_->record_usage_failure(b, "provider_not_found", attempted, false);
            continue;
        }
        if (bindings.size() > 1000)
        {
            for (const auto &b : bindings)
                store_->record_usage_failure(b, "provider_capacity", attempted, false);
            continue;
        }
        for (std::size_t start = 0; start < bindings.size(); start += 100)
        {
            const std::size_t end = std::min(start + 100, bindings.size());
            std::vector<std::string> ids;
            for (std::size_t i = start; i < end; ++i)
                ids.push_back(bindings[i].client_id);
            UsageRemoteResponse response;
            std::string error;
            SuiUsageClient client;
            if (!client.query(*provider, ids, response, error))
            {
                for (std::size_t i = start; i < end; ++i)
                    store_->record_usage_failure(bindings[i], error, attempted, false);
                backoff.failures = std::min(backoff.failures + 1, 4);
                const int delays[] = {30, 60, 120, 300};
                backoff.next_attempt_at = attempted + delays[backoff.failures - 1];
                break;
            }
            backoff = {};
            std::map<std::string, UsageRemoteItem> items;
            for (auto &item : response.items)
                items.emplace(item.client_id, std::move(item));
            std::set<std::string> missing(response.missing.begin(), response.missing.end()),
                invalid(response.invalid.begin(), response.invalid.end());
            for (std::size_t i = start; i < end; ++i)
            {
                UsageBinding &b = bindings[i];
                if (response.instance_id != b.instance_id)
                    store_->record_usage_failure(b, "instance_changed", attempted, true);
                else if (missing.count(b.client_id))
                    store_->record_usage_failure(b, "client_missing", attempted, true);
                else if (invalid.count(b.client_id))
                    store_->record_usage_failure(b, "invalid_data", attempted, false);
                else
                {
                    const auto found = items.find(b.client_id);
                    if (found == items.end() || found->second.identity_fingerprint != b.identity_fingerprint)
                        store_->record_usage_failure(b, "identity_changed", attempted, true);
                    else
                        store_->record_usage_success(b, found->second.metrics_json, response.observed_at, attempted);
                }
            }
        }
    }
    static std::int64_t last_cleanup = 0;
    if (attempted - last_cleanup >= 86400)
    {
        store_->cleanup_usage_audit(audit_days_);
        last_cleanup = attempted;
    }
}
