#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <string>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "handler/interfaces.h"
#include "handler/settings.h"
#include "security/secretbox.h"
#include "storage/postgres_store.h"
#include "utils/base64/base64.h"
#include "utils/logger.h"
#include "utils/string.h"
#include "utils/system.h"
#include "utils/urlencode.h"
#include "shortlink_api.h"

namespace
{
struct ShortLinkConfig
{
    bool enabled = false;
    bool trust_access_header = false;
    string_array admin_subjects;
    std::string connection_string;
    std::string public_base_url;
    std::string encryption_key;
    int max_active = 100;
    int max_per_hour = 20;
    int default_ttl = 30 * 24 * 60 * 60;
    int max_ttl = 365 * 24 * 60 * 60;
    std::size_t max_input_bytes = 64 * 1024;
    std::size_t max_output_bytes = 16 * 1024 * 1024;
    int max_links = 100;
    bool allow_private_hosts = false;
};

ShortLinkConfig config;
PostgresStore store;
SecretBox secret_box;
std::atomic<std::int64_t> last_cleanup{0};

std::int64_t unix_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void maybe_cleanup()
{
    const std::int64_t now = unix_now();
    std::int64_t expected = last_cleanup.load();
    if(now - expected < 3600 || !last_cleanup.compare_exchange_strong(expected, now))
        return;
    store.cleanup_expired(7 * 24 * 60 * 60);
}

bool env_bool(const std::string &name, bool fallback)
{
    const std::string value = toLower(getEnv(name));
    if(value.empty())
        return fallback;
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

int env_int(const std::string &name, int fallback)
{
    const std::string value = getEnv(name);
    return value.empty() ? fallback : to_int(value, fallback);
}

std::string random_code(std::size_t length)
{
    return randomUrlToken(length);
}

bool is_admin_token(const Request &request)
{
    if(global.accessToken.empty())
        return false;
    auto it = request.headers.find("Authorization");
    return it != request.headers.end() && it->second == "Bearer " + global.accessToken;
}

bool configured_admin_subject(const std::string &subject)
{
    return std::find(config.admin_subjects.begin(), config.admin_subjects.end(), subject) != config.admin_subjects.end();
}

bool authenticate_request(const Request &request, std::string &owner, bool &admin)
{
    admin = is_admin_token(request);
    if(admin)
    {
        owner = "admin";
        return store.ensure_user(owner);
    }

    auto key_it = request.headers.find("X-API-Key");
    if(key_it != request.headers.end() && store.authenticate_api_key(sha256Hex(key_it->second), owner))
        return store.ensure_user(owner);

    auto auth_it = request.headers.find("Authorization");
    if(auth_it != request.headers.end() && startsWith(auth_it->second, "Bearer "))
    {
        const std::string key = auth_it->second.substr(7);
        if(store.authenticate_api_key(sha256Hex(key), owner))
            return store.ensure_user(owner);
    }

    if(config.trust_access_header)
    {
        auto access_it = request.headers.find("Cf-Access-Authenticated-User-Email");
        if(access_it != request.headers.end() && !trim(access_it->second).empty())
        {
            owner = trim(access_it->second);
            admin = configured_admin_subject(owner) || store.user_is_admin(owner);
            return store.ensure_user(owner, owner, admin ? "admin" : "user");
        }
    }

    const std::string dev_user = getEnv("SHORTLINK_DEV_USER");
    if(!dev_user.empty())
    {
        owner = dev_user;
        admin = configured_admin_subject(owner) || store.user_is_admin(owner);
        return store.ensure_user(owner, owner, admin ? "admin" : "user");
    }
    return false;
}

std::string json_error(Response &response, int status, const std::string &message)
{
    response.status_code = status;
    response.content_type = "application/json;charset=utf-8";
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("error");
    writer.String(message.c_str());
    writer.EndObject();
    return buffer.GetString();
}

std::string json_headers(const string_icase_map &headers)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    for(const auto &header : headers)
    {
        if(header.first == "Content-Disposition")
            continue;
        writer.Key(header.first.c_str());
        writer.String(header.second.c_str());
    }
    writer.EndObject();
    return buffer.GetString();
}

void apply_headers(const std::string &encoded_headers, Response &response)
{
    rapidjson::Document document;
    document.Parse(encoded_headers.c_str());
    if(!document.IsObject())
        return;
    for(auto it = document.MemberBegin(); it != document.MemberEnd(); ++it)
    {
        if(it->name.IsString() && it->value.IsString())
            response.headers[it->name.GetString()] = it->value.GetString();
    }
}

bool valid_port_in_link(const std::string &link)
{
    const std::size_t scheme_end = link.find("://");
    if(scheme_end == std::string::npos)
        return true;
    const std::size_t authority_begin = scheme_end + 3;
    const std::size_t authority_end = link.find_first_of("/?#", authority_begin);
    const std::size_t end = authority_end == std::string::npos ? link.size() : authority_end;
    if(end <= authority_begin)
        return true;
    const std::size_t at = link.rfind('@', end - 1);
    const std::size_t host_begin = at == std::string::npos ? authority_begin : at + 1;
    if(host_begin >= end || link[host_begin] == '[')
        return true;
    const std::size_t colon = link.rfind(':', end - 1);
    if(colon == std::string::npos || colon < host_begin)
        return true;
    const std::string port = link.substr(colon + 1, end - colon - 1);
    if(port.empty() || !std::all_of(port.begin(), port.end(), [](unsigned char c){ return std::isdigit(c) != 0; }))
        return true;
    return to_number<int>(port, 65536) >= 1 && to_number<int>(port, 65536) <= 65535;
}

bool valid_source_link(const std::string &link)
{
    const std::string lower = toLower(link);
    static const string_array prefixes = {
        "ss://", "ssr://", "vmess://", "vmess1://", "vless://", "vless1://",
        "trojan://", "tuic://", "hysteria://", "hysteria2://", "hy2://", "socks://",
        "http://", "https://"
    };
    if(std::none_of(prefixes.begin(), prefixes.end(), [&](const std::string &prefix){ return startsWith(lower, prefix); }))
        return false;
    if(startsWith(lower, "file://") || startsWith(lower, "data:"))
        return false;
    if(!config.allow_private_hosts && (startsWith(lower, "http://") || startsWith(lower, "https://")))
    {
        const std::size_t authority_begin = lower.find("://") + 3;
        const std::size_t authority_end = lower.find_first_of("/?#", authority_begin);
        const std::size_t end = authority_end == std::string::npos ? lower.size() : authority_end;
        std::size_t host_begin = lower.rfind('@', end - 1);
        host_begin = host_begin == std::string::npos ? authority_begin : host_begin + 1;
        std::string host = lower.substr(host_begin, end - host_begin);
        if(!host.empty() && host.front() == '[')
        {
            const std::size_t close = host.find(']');
            if(close != std::string::npos)
                host = host.substr(1, close - 1);
        }
        else
        {
            const std::size_t port_separator = host.rfind(':');
            if(port_separator != std::string::npos)
                host.erase(port_separator);
        }
        if(host == "localhost" || endsWith(host, ".localhost") || endsWith(host, ".local") || endsWith(host, ".internal")
            || host == "127.0.0.1" || startsWith(host, "127.") || startsWith(host, "10.") || startsWith(host, "192.168.")
            || startsWith(host, "169.254.") || startsWith(host, "172.16.") || startsWith(host, "172.17.")
            || startsWith(host, "172.18.") || startsWith(host, "172.19.") || startsWith(host, "172.2")
            || startsWith(host, "172.30.") || startsWith(host, "172.31.")
            || startsWith(host, "198.18.") || host == "0.0.0.0" || host == "::1" || startsWith(host, "fc") || startsWith(host, "fd")
            || startsWith(host, "fe80:"))
            return false;
    }
    return valid_port_in_link(link);
}

std::string build_source_payload(const string_array &links, const std::string &target)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("target");
    writer.String(target.c_str());
    writer.Key("links");
    writer.StartArray();
    for(const std::string &link : links)
        writer.String(link.c_str());
    writer.EndArray();
    writer.Key("insert");
    writer.Bool(false);
    writer.EndObject();
    return buffer.GetString();
}

std::string shortlink_url(const std::string &code)
{
    if(config.public_base_url.empty())
        return "/s/" + code;
    return config.public_base_url + "/s/" + code;
}

bool parse_shortlink_request(const std::string &body, string_array &links, std::string &target, int &ttl, std::string &name, std::string &error)
{
    if(body.size() > config.max_input_bytes)
    {
        error = "request body is too large";
        return false;
    }
    rapidjson::Document document;
    document.Parse(body.c_str());
    if(document.HasParseError() || !document.IsObject())
    {
        error = "request body must be a JSON object";
        return false;
    }
    target = document.HasMember("target") && document["target"].IsString() ? document["target"].GetString() : "clash";
    if(target != "clash")
    {
        error = "only clash target is supported";
        return false;
    }
    if(document.HasMember("name") && document["name"].IsString())
        name = trim(document["name"].GetString());
    if(name.size() > 120)
    {
        error = "name is too long";
        return false;
    }
    ttl = config.default_ttl;
    if(document.HasMember("expires_in") && document["expires_in"].IsInt64())
        ttl = static_cast<int>(document["expires_in"].GetInt64());
    if(ttl < 0 || ttl > config.max_ttl)
    {
        error = "expires_in is outside the allowed range";
        return false;
    }
    if(!document.HasMember("links") || !document["links"].IsArray() || document["links"].Empty())
    {
        error = "at least one link is required";
        return false;
    }
    if(static_cast<int>(document["links"].Size()) > config.max_links)
    {
        error = "too many links";
        return false;
    }
    std::size_t link_index = 0;
    for(const auto &item : document["links"].GetArray())
    {
        link_index++;
        if(!item.IsString())
        {
            error = "第 " + std::to_string(link_index) + " 行必须是字符串";
            return false;
        }
        const std::string link = trim(item.GetString());
        if(link.empty())
        {
            error = "第 " + std::to_string(link_index) + " 行为空";
            return false;
        }
        if(link.size() > 8192)
        {
            error = "第 " + std::to_string(link_index) + " 行超过 8192 字节";
            return false;
        }
        if(!valid_port_in_link(link))
        {
            error = "第 " + std::to_string(link_index) + " 行端口必须在 1-65535 范围内";
            return false;
        }
        if(!valid_source_link(link))
        {
            error = "第 " + std::to_string(link_index) + " 行协议不支持，或地址被安全策略拒绝";
            return false;
        }
        links.emplace_back(link);
    }
    return true;
}

std::string conversion_snapshot(const string_array &links, Response &conversion_response)
{
    Request conversion_request;
    conversion_request.method = "GET";
    conversion_request.argument.emplace("target", "clash");
    conversion_request.argument.emplace("url", join(links, "|"));
    conversion_request.argument.emplace("insert", "false");
    conversion_request.argument.emplace("config", "");
    conversion_request.headers = {};
    std::string snapshot = subconverter(conversion_request, conversion_response);
    return snapshot;
}

std::string create_response(const ShortLinkRecord &record)
{
    const std::string url = shortlink_url(record.code);
    const std::string download = url + (url.find('?') == std::string::npos ? "?download=1" : "&download=1");
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("id"); writer.String(record.id.c_str());
    writer.Key("name"); writer.String(record.name.c_str());
    writer.Key("target"); writer.String(record.target.c_str());
    writer.Key("links_count"); writer.Int(record.links_count);
    writer.Key("short_url"); writer.String(url.c_str());
    writer.Key("preview_url"); writer.String(url.c_str());
    writer.Key("download_url"); writer.String(download.c_str());
    writer.Key("expires_at"); writer.Int64(record.expires_at);
    writer.EndObject();
    return buffer.GetString();
}

std::string extract_id(const std::string &path)
{
    const std::string prefix = "/api/short-links/";
    if(!startsWith(path, prefix))
        return "";
    return path.substr(prefix.size());
}

std::string extract_refresh_id(const std::string &path)
{
    const std::string prefix = "/api/short-links/";
    const std::string suffix = "/refresh";
    if(!startsWith(path, prefix) || !endsWith(path, suffix))
        return "";
    return path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
}

std::string extract_key_id(const std::string &path)
{
    const std::string prefix = "/api/keys/";
    if(!startsWith(path, prefix))
        return "";
    return path.substr(prefix.size());
}

std::string extract_code(const std::string &path)
{
    const std::string prefix = "/s/";
    if(!startsWith(path, prefix))
        return "";
    return path.substr(prefix.size());
}
}

bool initializeShortLinkService()
{
    config.enabled = env_bool("SHORTLINK_ENABLED", false);
    if(!config.enabled)
        return true;

    config.trust_access_header = env_bool("SHORTLINK_TRUST_ACCESS_HEADER", false);
    config.admin_subjects = split(getEnv("SHORTLINK_ADMIN_SUBJECTS"), ",");
    for(std::string &subject : config.admin_subjects)
        subject = trim(subject);
    config.admin_subjects.erase(std::remove(config.admin_subjects.begin(), config.admin_subjects.end(), ""), config.admin_subjects.end());
    config.connection_string = getEnv("DATABASE_URL");
    config.public_base_url = getEnv("PUBLIC_BASE_URL");
    while(!config.public_base_url.empty() && config.public_base_url.back() == '/')
        config.public_base_url.pop_back();
    config.encryption_key = getEnv("SHORTLINK_ENCRYPTION_KEY");
    config.max_active = std::max(env_int("SHORTLINK_MAX_ACTIVE", 100), 1);
    config.max_per_hour = std::max(env_int("SHORTLINK_MAX_PER_HOUR", 20), 1);
    config.default_ttl = std::max(env_int("SHORTLINK_DEFAULT_TTL", 30 * 24 * 60 * 60), 0);
    config.max_ttl = std::max(env_int("SHORTLINK_MAX_TTL", 365 * 24 * 60 * 60), config.default_ttl);
    config.max_input_bytes = static_cast<std::size_t>(std::max(env_int("SHORTLINK_MAX_INPUT_BYTES", 64 * 1024), 1024));
    config.max_output_bytes = static_cast<std::size_t>(std::max(env_int("SHORTLINK_MAX_OUTPUT_BYTES", 16 * 1024 * 1024), 1024));
    config.max_links = std::max(env_int("SHORTLINK_MAX_LINKS", 100), 1);
    config.allow_private_hosts = env_bool("SHORTLINK_ALLOW_PRIVATE_HOSTS", false);
    if(config.connection_string.empty() || config.encryption_key.empty())
    {
        writeLog(0, "SHORTLINK_ENABLED requires DATABASE_URL and SHORTLINK_ENCRYPTION_KEY.", LOG_LEVEL_ERROR);
        config.enabled = false;
        return false;
    }
    if(!secret_box.init(config.encryption_key) || !store.open(config.connection_string) || !store.ensure_schema())
    {
        writeLog(0, "Unable to initialize PostgreSQL short-link service.", LOG_LEVEL_ERROR);
        config.enabled = false;
        return false;
    }
    writeLog(0, "PostgreSQL short-link service initialized.", LOG_LEVEL_INFO);
    return true;
}

bool shortLinkServiceEnabled()
{
    return config.enabled && store.ready() && secret_box.ready();
}

std::string createShortLink(RESPONSE_CALLBACK_ARGS)
{
    if(!shortLinkServiceEnabled())
        return json_error(response, 503, "short-link service is unavailable");
    maybe_cleanup();
    std::string owner;
    bool admin = false;
    if(!authenticate_request(request, owner, admin))
        return json_error(response, 401, "authentication required");

    string_array links;
    std::string target, name, error;
    int ttl = config.default_ttl;
    if(!parse_shortlink_request(request.postdata, links, target, ttl, name, error))
        return json_error(response, error == "request body is too large" ? 413 : 400, error);
    Response conversion_response;
    const std::string snapshot = conversion_snapshot(links, conversion_response);
    if(snapshot.size() > config.max_output_bytes)
        return json_error(response, 413, "generated configuration is too large");
    if(conversion_response.status_code < 200 || conversion_response.status_code >= 300 || snapshot.empty())
    {
        response.status_code = conversion_response.status_code >= 400 ? conversion_response.status_code : 400;
        return json_error(response, response.status_code, snapshot.empty() ? "conversion produced no content" : snapshot);
    }

    std::string source_payload, snapshot_payload;
    if(!secret_box.encrypt(build_source_payload(links, target), source_payload) || !secret_box.encrypt(snapshot, snapshot_payload))
        return json_error(response, 500, "unable to encrypt short-link payload");

    ShortLinkRecord record;
    record.owner = owner;
    record.name = name;
    record.target = target;
    record.source_payload = source_payload;
    record.snapshot_payload = snapshot_payload;
    record.response_headers = json_headers(conversion_response.headers);
    record.content_type = "text/yaml; charset=utf-8";
    record.content_hash = sha256Hex(snapshot);
    record.links_count = static_cast<int>(links.size());
    record.expires_at = ttl > 0 ? unix_now() + ttl : 0;
    std::string id;
    for(int attempt = 0; attempt < 3 && id.empty(); attempt++)
    {
        record.code = random_code(24);
        if(record.code.empty())
            break;
        store.create_short_link(record, config.max_active, config.max_per_hour, id);
    }
    if(id.empty())
        return json_error(response, 429, "unable to allocate a short code or quota exceeded");
    record.id = id;
    response.status_code = 201;
    response.content_type = "application/json;charset=utf-8";
    return create_response(record);
}

std::string listShortLinks(RESPONSE_CALLBACK_ARGS)
{
    if(!shortLinkServiceEnabled())
        return json_error(response, 503, "short-link service is unavailable");
    maybe_cleanup();
    std::string owner;
    bool admin = false;
    if(!authenticate_request(request, owner, admin))
        return json_error(response, 401, "authentication required");
    std::vector<ShortLinkRecord> records;
    if(!store.list_short_links(owner, records, admin))
        return json_error(response, 500, "unable to list short links");
    response.content_type = "application/json;charset=utf-8";
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("items");
    writer.StartArray();
    for(const auto &record : records)
    {
        writer.StartObject();
        writer.Key("id"); writer.String(record.id.c_str());
        if(admin)
        {
            writer.Key("owner"); writer.String(record.owner.c_str());
        }
        writer.Key("name"); writer.String(record.name.c_str());
        writer.Key("target"); writer.String(record.target.c_str());
        writer.Key("links_count"); writer.Int(record.links_count);
        writer.Key("short_url"); writer.String(shortlink_url(record.code).c_str());
        writer.Key("created_at"); writer.Int64(record.created_at);
        writer.Key("updated_at"); writer.Int64(record.updated_at);
        writer.Key("expires_at"); writer.Int64(record.expires_at);
        writer.Key("revoked_at"); writer.Int64(record.revoked_at);
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();
    return buffer.GetString();
}

std::string revokeShortLink(RESPONSE_CALLBACK_ARGS)
{
    if(!shortLinkServiceEnabled())
        return json_error(response, 503, "short-link service is unavailable");
    std::string owner;
    bool admin = false;
    if(!authenticate_request(request, owner, admin))
        return json_error(response, 401, "authentication required");
    const std::string id = extract_id(request.url);
    if(id.empty() || !store.revoke_short_link(owner, id, admin))
        return json_error(response, 404, "short link not found");
    response.content_type = "application/json;charset=utf-8";
    return "{\"status\":\"revoked\"}";
}

std::string refreshShortLink(RESPONSE_CALLBACK_ARGS)
{
    if(!shortLinkServiceEnabled())
        return json_error(response, 503, "short-link service is unavailable");
    std::string owner;
    bool admin = false;
    if(!authenticate_request(request, owner, admin))
        return json_error(response, 401, "authentication required");
    ShortLinkRecord record;
    const std::string id = extract_refresh_id(request.url);
    if(id.empty() || !store.get_short_link_by_id(owner, id, record, admin))
        return json_error(response, 404, "short link not found");

    std::string source_json;
    if(!secret_box.decrypt(record.source_payload, source_json))
        return json_error(response, 500, "unable to decrypt short-link source");
    rapidjson::Document source;
    source.Parse(source_json.c_str());
    if(source.HasParseError() || !source.IsObject() || !source.HasMember("links") || !source["links"].IsArray())
        return json_error(response, 500, "short-link source is invalid");
    string_array links;
    for(const auto &item : source["links"].GetArray())
    {
        if(item.IsString())
            links.emplace_back(item.GetString());
    }
    if(links.empty())
        return json_error(response, 500, "short-link source has no links");
    Response conversion_response;
    const std::string snapshot = conversion_snapshot(links, conversion_response);
    std::string snapshot_payload;
    if(conversion_response.status_code < 200 || conversion_response.status_code >= 300 || snapshot.empty() || !secret_box.encrypt(snapshot, snapshot_payload))
        return json_error(response, conversion_response.status_code >= 400 ? conversion_response.status_code : 500, "unable to refresh short-link snapshot");
    if(!store.update_snapshot(owner, id, snapshot_payload, json_headers(conversion_response.headers), sha256Hex(snapshot), unix_now()))
        return json_error(response, 404, "short link not found");
    response.content_type = "application/json;charset=utf-8";
    return "{\"status\":\"refreshed\"}";
}

std::string createShortLinkApiKey(RESPONSE_CALLBACK_ARGS)
{
    if(!shortLinkServiceEnabled())
        return json_error(response, 503, "short-link service is unavailable");
    std::string owner;
    bool admin = false;
    if(!authenticate_request(request, owner, admin))
        return json_error(response, 401, "authentication required");

    rapidjson::Document document;
    document.Parse(request.postdata.c_str());
    const bool has_owner = document.IsObject() && document.HasMember("owner") && document["owner"].IsString();
    const std::string target_owner = has_owner ? trim(document["owner"].GetString()) : owner;
    if(document.HasParseError() || !document.IsObject() || (!admin && has_owner && target_owner != owner))
        return json_error(response, 400, admin ? "owner is required" : "invalid owner");
    if(target_owner.empty() || !store.ensure_user(target_owner, target_owner))
        return json_error(response, 400, "invalid owner");
    const std::string key = random_code(32);
    if(key.empty())
        return json_error(response, 500, "unable to generate API key");
    const std::string name = document.HasMember("name") && document["name"].IsString() ? document["name"].GetString() : "";
    const std::int64_t expires_at = document.HasMember("expires_in") && document["expires_in"].IsInt64() && document["expires_in"].GetInt64() > 0 ? unix_now() + document["expires_in"].GetInt64() : 0;
    std::string id;
    if(!store.create_api_key(target_owner, sha256Hex(key), name, expires_at, id))
        return json_error(response, 500, "unable to create API key");
    response.status_code = 201;
    response.content_type = "application/json;charset=utf-8";
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("id"); writer.String(id.c_str());
    writer.Key("owner"); writer.String(target_owner.c_str());
    writer.Key("api_key"); writer.String(key.c_str());
    writer.Key("expires_at"); writer.Int64(expires_at);
    writer.EndObject();
    return buffer.GetString();
}

std::string revokeShortLinkApiKey(RESPONSE_CALLBACK_ARGS)
{
    if(!shortLinkServiceEnabled())
        return json_error(response, 503, "short-link service is unavailable");
    std::string owner;
    bool admin = false;
    if(!authenticate_request(request, owner, admin))
        return json_error(response, 401, "authentication required");
    const std::string id = extract_key_id(request.url);
    if(id.empty() || !store.revoke_api_key(owner, id, admin))
        return json_error(response, 404, "API key not found");
    response.content_type = "application/json;charset=utf-8";
    return "{\"status\":\"revoked\"}";
}

std::string listShortLinkUsers(RESPONSE_CALLBACK_ARGS)
{
    if(!shortLinkServiceEnabled())
        return json_error(response, 503, "short-link service is unavailable");
    std::string owner;
    bool admin = false;
    if(!authenticate_request(request, owner, admin) || !admin)
        return json_error(response, 403, "administrator authentication required");
    std::vector<ShortLinkUserRecord> records;
    if(!store.list_users(records))
        return json_error(response, 500, "unable to list users");
    response.content_type = "application/json;charset=utf-8";
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("items");
    writer.StartArray();
    for(const auto &record : records)
    {
        writer.StartObject();
        writer.Key("subject"); writer.String(record.subject.c_str());
        writer.Key("email"); writer.String(record.email.c_str());
        writer.Key("role"); writer.String(record.role.c_str());
        writer.Key("created_at"); writer.Int64(record.created_at);
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();
    return buffer.GetString();
}

std::string upsertShortLinkUser(RESPONSE_CALLBACK_ARGS)
{
    if(!shortLinkServiceEnabled())
        return json_error(response, 503, "short-link service is unavailable");
    std::string owner;
    bool admin = false;
    if(!authenticate_request(request, owner, admin) || !admin)
        return json_error(response, 403, "administrator authentication required");
    rapidjson::Document document;
    document.Parse(request.postdata.c_str());
    if(document.HasParseError() || !document.IsObject() || !document.HasMember("subject") || !document["subject"].IsString())
        return json_error(response, 400, "subject is required");
    const std::string subject = trim(document["subject"].GetString());
    const std::string email = document.HasMember("email") && document["email"].IsString() ? trim(document["email"].GetString()) : subject;
    const std::string role = document.HasMember("role") && document["role"].IsString() ? trim(document["role"].GetString()) : "user";
    if(subject.empty() || (role != "user" && role != "admin") || !store.ensure_user(subject, email, role) || !store.set_user_role(subject, role))
        return json_error(response, 400, "invalid user or role");
    response.content_type = "application/json;charset=utf-8";
    return "{\"status\":\"updated\"}";
}

std::string getShortLink(RESPONSE_CALLBACK_ARGS)
{
    if(!shortLinkServiceEnabled())
    {
        response.status_code = 404;
        return "Not Found";
    }
    const std::string code = extract_code(request.url);
    ShortLinkRecord record;
    if(code.empty() || !store.get_short_link(code, record))
    {
        response.status_code = 404;
        return "Not Found";
    }
    const std::int64_t now = unix_now();
    if(record.revoked_at > 0 || (record.expires_at > 0 && record.expires_at <= now))
    {
        response.status_code = 410;
        return "Gone";
    }
    std::string snapshot;
    if(!secret_box.decrypt(record.snapshot_payload, snapshot))
    {
        response.status_code = 500;
        return "Unable to decrypt short-link snapshot";
    }
    response.content_type = record.content_type.empty() ? "text/yaml; charset=utf-8" : record.content_type;
    apply_headers(record.response_headers, response);
    response.headers["Cache-Control"] = "no-store";
    if(getUrlArg(request.argument, "download") == "1")
        response.headers["Content-Disposition"] = "attachment; filename=custom-clash.yaml";
    if(request.method == "HEAD")
        return "";
    return snapshot;
}
