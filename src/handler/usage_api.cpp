#include <algorithm>
#include <chrono>
#include <cctype>
#include <map>
#include <mutex>
#include <set>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "handler/shortlink_api.h"
#include "security/secretbox.h"
#include "storage/postgres_store.h"
#include "usage/usage_service.h"
#include "utils/string.h"
#include "utils/system.h"
#include "usage_api.h"

namespace
{
std::int64_t now_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::string error_json(Response &response, int status, const std::string &code)
{
    response.status_code = status;
    response.content_type = "application/json;charset=utf-8";
    response.headers["Cache-Control"] = "private, no-store";
    return "{\"error\":\"" + code + "\"}";
}
bool digits(const std::string &v)
{
    static const std::string max_value = "9223372036854775807";
    return !v.empty() && v.size() <= 19 && v != "0" && v.front() != '0' &&
           std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c); }) &&
           (v.size() < 19 || v <= max_value);
}
bool fingerprint(const std::string &v)
{
    return v.size() == 64 &&
           std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c) || (c >= 'a' && c <= 'f'); });
}
bool uuid(const std::string &v)
{
    return v.size() == 36 && v[8] == '-' && v[13] == '-' && v[18] == '-' && v[23] == '-' &&
           std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isxdigit(c) || c == '-'; });
}
bool label_valid(const std::string &v)
{
    if (v.empty() || trim(v).empty())
        return false;
    std::size_t count = 0;
    for (unsigned char c : v)
        if ((c & 0xc0) != 0x80)
            ++count;
    return count <= 80;
}
std::string request_id(const Request &request)
{
    auto it = request.headers.find("X-Request-ID");
    if (it != request.headers.end() && !it->second.empty() && it->second.size() <= 128 &&
        std::all_of(it->second.begin(), it->second.end(),
                    [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; }))
        return it->second;
    return randomUrlToken(16);
}
bool authenticate(const Request &request, Response &response, ShortLinkAuthIdentity &identity, bool admin = false)
{
    if (!authenticateShortLinkRequest(request, identity))
    {
        error_json(response, 401, "authentication_required");
        return false;
    }
    if (admin && !identity.is_admin)
    {
        error_json(response, 403, "administrator_required");
        return false;
    }
    return true;
}
bool write_origin_allowed(const Request &request, const ShortLinkAuthIdentity &identity)
{
    auto it = request.headers.find("Origin");
    if (it == request.headers.end() || it->second.empty())
        return identity.auth_kind == "api_key" || identity.auth_kind == "bootstrap_token";
    const std::string configured = getEnv("SHORTLINK_PORTAL_ORIGIN");
    return !configured.empty() && it->second == configured;
}
bool json_request(const Request &request, const std::set<std::string> &allowed, rapidjson::Document &doc,
                  std::string &error)
{
    if (request.postdata.size() > 16 * 1024)
    {
        error = "request_too_large";
        return false;
    }
    auto ct = request.headers.find("Content-Type");
    if (ct == request.headers.end() || !startsWith(toLower(ct->second), "application/json"))
    {
        error = "json_required";
        return false;
    }
    doc.Parse(request.postdata.c_str());
    if (doc.HasParseError() || !doc.IsObject())
    {
        error = "invalid_json";
        return false;
    }
    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it)
        if (!allowed.count(it->name.GetString()))
        {
            error = "unknown_field";
            return false;
        }
    return true;
}
bool string_field(const rapidjson::Document &doc, const char *key, std::string &value)
{
    if (!doc.HasMember(key) || !doc[key].IsString())
        return false;
    value = doc[key].GetString();
    return true;
}
std::string if_match(const Request &request)
{
    auto it = request.headers.find("If-Match");
    if (it == request.headers.end())
        return "";
    const std::string &v = it->second;
    if (v.size() < 3 || v.front() != '"' || v.back() != '"')
        return "!";
    std::string r = v.substr(1, v.size() - 2);
    return digits(r) ? r : "!";
}
void metrics(rapidjson::Writer<rapidjson::StringBuffer> &writer, const std::string &json)
{
    if (json.empty())
    {
        writer.Null();
        return;
    }
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    if (doc.HasParseError())
        writer.Null();
    else
        doc.Accept(writer);
}
std::string availability(const UsageBinding &b, std::int64_t now)
{
    if (b.invalidated)
        return "binding_invalid";
    if (b.observed_at <= 0)
        return b.last_attempt_at > 0 ? "unavailable" : "pending";
    const std::int64_t age = std::max<std::int64_t>(0, now - b.observed_at);
    if (age > usageService().stale_seconds())
        return "unavailable";
    if (age > usageService().fresh_seconds() || !b.last_error_code.empty())
        return "stale";
    return "fresh";
}
void write_binding(rapidjson::Writer<rapidjson::StringBuffer> &w, const UsageBinding &b, bool admin,
                   bool include_metrics)
{
    const std::string avail = availability(b, now_seconds());
    w.StartObject();
    w.Key(admin ? "id" : "binding_id");
    w.String(b.id.c_str());
    if (admin)
    {
        w.Key("revision");
        w.String(b.revision.c_str());
        w.Key("owner_subject");
        w.String(b.owner_subject.c_str());
        w.Key("provider_id");
        w.String(b.provider_id.c_str());
        w.Key("client_id");
        w.String(b.client_id.c_str());
    }
    w.Key("label");
    w.String(b.label.c_str());
    if (admin)
    {
        w.Key("created_at");
        w.Int64(b.created_at);
    }
    w.Key("availability");
    w.String(avail.c_str());
    w.Key("observed_at");
    if (b.observed_at > 0)
        w.Int64(b.observed_at);
    else
        w.Null();
    w.Key("last_attempt_at");
    if (b.last_attempt_at > 0)
        w.Int64(b.last_attempt_at);
    else
        w.Null();
    w.Key("error_code");
    if (b.last_error_code.empty())
        w.Null();
    else
        w.String(b.last_error_code.c_str());
    if (include_metrics)
    {
        w.Key("metrics");
        if (avail == "fresh" || avail == "stale")
            metrics(w, b.snapshot_json);
        else
            w.Null();
    }
    w.EndObject();
}
bool rate_limit(const std::string &actor)
{
    static std::mutex mutex;
    static std::map<std::string, std::pair<std::int64_t, int>> counts;
    std::lock_guard<std::mutex> guard(mutex);
    const auto now = now_seconds();
    auto &entry = counts[actor];
    if (now - entry.first >= 60)
        entry = {now, 0};
    return ++entry.second <= 10;
}
std::string binding_id_from_path(const std::string &path)
{
    const std::string prefix = "/api/admin/usage-bindings/";
    if (!startsWith(path, prefix))
        return "";
    std::string id = path.substr(prefix.size());
    return digits(id) ? id : "";
}
} // namespace

bool initializeUsageService()
{
    return usageService().initialize(shortLinkStore());
}
void shutdownUsageService()
{
    usageService().shutdown();
}

std::string getUsage(RESPONSE_CALLBACK_ARGS)
{
    ShortLinkAuthIdentity identity;
    if (!authenticate(request, response, identity))
        return error_json(response, response.status_code, "authentication_required");
    if (request.argument.find("subject") != request.argument.end() ||
        request.argument.find("owner_subject") != request.argument.end() ||
        request.argument.find("client_id") != request.argument.end())
        return error_json(response, 400, "unsupported_parameter");
    response.headers["Cache-Control"] = "private, no-store";
    response.content_type = "application/json;charset=utf-8";
    if (!usageService().configured_enabled())
        return "{\"schema_version\":1,\"enabled\":false,\"state\":\"disabled\",\"server_time\":" +
               std::to_string(now_seconds()) + ",\"refresh_after_seconds\":30,\"items\":[]}";
    if (!usageService().ready())
        return error_json(response, 503, "usage_unavailable");
    std::vector<UsageBinding> items;
    if (!shortLinkStore().list_usage_bindings(identity.owner, false, "", 101, items))
        return error_json(response, 503, "usage_query_failed");
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    w.StartObject();
    w.Key("schema_version");
    w.Int(1);
    w.Key("enabled");
    w.Bool(true);
    w.Key("state");
    w.String(items.empty() ? "unbound" : "ready");
    w.Key("server_time");
    w.Int64(now_seconds());
    w.Key("refresh_after_seconds");
    w.Int(usageService().poll_seconds());
    w.Key("items");
    w.StartArray();
    for (const auto &item : items)
        write_binding(w, item, false, true);
    w.EndArray();
    w.EndObject();
    return b.GetString();
}

std::string listUsageProviders(RESPONSE_CALLBACK_ARGS)
{
    ShortLinkAuthIdentity identity;
    if (!authenticate(request, response, identity, true))
        return error_json(response, response.status_code, "administrator_required");
    if (!usageService().ready())
        return error_json(response, 503, "usage_unavailable");
    response.headers["Cache-Control"] = "private, no-store";
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    w.StartObject();
    w.Key("schema_version");
    w.Int(1);
    w.Key("items");
    w.StartArray();
    for (const auto &p : usageService().providers())
    {
        w.StartObject();
        w.Key("provider_id");
        w.String(p.provider_id.c_str());
        w.Key("label");
        w.String(p.label.c_str());
        w.Key("available");
        w.Bool(true);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return b.GetString();
}

std::string listUsageBindings(RESPONSE_CALLBACK_ARGS)
{
    ShortLinkAuthIdentity identity;
    if (!authenticate(request, response, identity, true))
        return error_json(response, response.status_code, "administrator_required");
    if (!usageService().ready())
        return error_json(response, 503, "usage_unavailable");
    std::string owner = getUrlArg(request.argument, "owner_subject"), cursor = getUrlArg(request.argument, "cursor"),
                limit_text = getUrlArg(request.argument, "limit");
    if (!cursor.empty() && !digits(cursor))
        return error_json(response, 400, "invalid_cursor");
    int limit = limit_text.empty() ? 50 : to_int(limit_text, 0);
    if (limit < 1 || limit > 100)
        return error_json(response, 400, "invalid_limit");
    std::vector<UsageBinding> items;
    if (!shortLinkStore().list_usage_bindings(owner, true, cursor, limit + 1, items))
        return error_json(response, 503, "usage_query_failed");
    std::string next;
    if (static_cast<int>(items.size()) > limit)
    {
        next = items[limit - 1].id;
        items.resize(limit);
    }
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    w.StartObject();
    w.Key("schema_version");
    w.Int(1);
    w.Key("items");
    w.StartArray();
    for (const auto &i : items)
        write_binding(w, i, true, false);
    w.EndArray();
    w.Key("next_cursor");
    if (next.empty())
        w.Null();
    else
        w.String(next.c_str());
    w.EndObject();
    response.headers["Cache-Control"] = "private, no-store";
    return b.GetString();
}

std::string previewUsageBinding(RESPONSE_CALLBACK_ARGS)
{
    ShortLinkAuthIdentity auth;
    if (!authenticate(request, response, auth, true))
        return error_json(response, response.status_code, "administrator_required");
    if (!usageService().ready())
        return error_json(response, 503, "usage_unavailable");
    if (!write_origin_allowed(request, auth))
        return error_json(response, 403, "origin_forbidden");
    if (!rate_limit(auth.owner))
        return error_json(response, 429, "rate_limited");
    rapidjson::Document doc;
    std::string error;
    if (!json_request(request, {"owner_subject", "provider_id", "client_id", "label"}, doc, error))
        return error_json(response, error == "request_too_large" ? 413 : 400, error);
    std::string owner, provider, client, label;
    if (!string_field(doc, "owner_subject", owner) || !string_field(doc, "provider_id", provider) ||
        !string_field(doc, "client_id", client) || !string_field(doc, "label", label) || !digits(client) ||
        !label_valid(label) || !shortLinkStore().usage_user_exists(owner))
        return error_json(response, 400, "invalid_binding");
    UsageRemoteItem item;
    std::string instance;
    std::int64_t observed = 0;
    if (!usageService().query_one(provider, client, item, instance, observed, error))
        return error_json(response, error == "client_missing" ? 404 : 502, error);
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    w.StartObject();
    w.Key("owner_subject");
    w.String(owner.c_str());
    w.Key("provider_id");
    w.String(provider.c_str());
    w.Key("client_id");
    w.String(client.c_str());
    w.Key("label");
    w.String(label.c_str());
    w.Key("expected_instance_id");
    w.String(instance.c_str());
    w.Key("expected_identity_fingerprint");
    w.String(item.identity_fingerprint.c_str());
    w.Key("observed_at");
    w.Int64(observed);
    w.Key("metrics");
    metrics(w, item.metrics_json);
    w.EndObject();
    response.headers["Cache-Control"] = "private, no-store";
    return b.GetString();
}

std::string createUsageBinding(RESPONSE_CALLBACK_ARGS)
{
    ShortLinkAuthIdentity auth;
    if (!authenticate(request, response, auth, true))
        return error_json(response, response.status_code, "administrator_required");
    if (!usageService().ready())
        return error_json(response, 503, "usage_unavailable");
    if (!write_origin_allowed(request, auth))
        return error_json(response, 403, "origin_forbidden");
    if (!rate_limit(auth.owner))
        return error_json(response, 429, "rate_limited");
    rapidjson::Document doc;
    std::string error;
    if (!json_request(request,
                      {"owner_subject", "provider_id", "client_id", "label", "expected_instance_id",
                       "expected_identity_fingerprint"},
                      doc, error))
        return error_json(response, error == "request_too_large" ? 413 : 400, error);
    UsageBinding requested;
    if (!string_field(doc, "owner_subject", requested.owner_subject) ||
        !string_field(doc, "provider_id", requested.provider_id) ||
        !string_field(doc, "client_id", requested.client_id) || !string_field(doc, "label", requested.label) ||
        !string_field(doc, "expected_instance_id", requested.instance_id) ||
        !string_field(doc, "expected_identity_fingerprint", requested.identity_fingerprint) ||
        !digits(requested.client_id) || !label_valid(requested.label) || !uuid(requested.instance_id) ||
        !fingerprint(requested.identity_fingerprint))
        return error_json(response, 400, "invalid_binding");
    UsageRemoteItem remote;
    std::string instance;
    std::int64_t observed = 0;
    if (!usageService().query_one(requested.provider_id, requested.client_id, remote, instance, observed, error))
        return error_json(response, error == "client_missing" ? 409 : 502, error);
    if (instance != requested.instance_id || remote.identity_fingerprint != requested.identity_fingerprint)
        return error_json(response, 409, "preview_changed");
    UsageBinding created;
    if (!shortLinkStore().create_usage_binding(requested, auth.owner, request_id(request), 10, created, error))
        return error_json(
            response, error == "conflict" || error == "limit" ? 409 : (error == "owner_not_found" ? 400 : 503), error);
    usageService().wake();
    response.status_code = 201;
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    write_binding(w, created, true, false);
    return b.GetString();
}

std::string renameUsageBinding(RESPONSE_CALLBACK_ARGS)
{
    ShortLinkAuthIdentity auth;
    if (!authenticate(request, response, auth, true))
        return error_json(response, response.status_code, "administrator_required");
    if (!usageService().ready())
        return error_json(response, 503, "usage_unavailable");
    if (!write_origin_allowed(request, auth))
        return error_json(response, 403, "origin_forbidden");
    const std::string id = binding_id_from_path(request.url), revision = if_match(request);
    if (id.empty())
        return error_json(response, 404, "binding_not_found");
    if (revision.empty())
        return error_json(response, 428, "if_match_required");
    if (revision == "!")
        return error_json(response, 400, "invalid_if_match");
    rapidjson::Document doc;
    std::string error;
    if (!json_request(request, {"label"}, doc, error))
        return error_json(response, error == "request_too_large" ? 413 : 400, error);
    std::string label;
    if (!string_field(doc, "label", label) || !label_valid(label))
        return error_json(response, 400, "invalid_label");
    UsageBinding updated;
    if (!shortLinkStore().rename_usage_binding(id, revision, label, auth.owner, request_id(request), updated, error))
        return error_json(response, error == "precondition" ? 412 : 503, error);
    if (!shortLinkStore().get_usage_binding(id, updated))
        return error_json(response, 503, "usage_query_failed");
    usageService().wake();
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    write_binding(w, updated, true, false);
    return b.GetString();
}

std::string revokeUsageBinding(RESPONSE_CALLBACK_ARGS)
{
    ShortLinkAuthIdentity auth;
    if (!authenticate(request, response, auth, true))
        return error_json(response, response.status_code, "administrator_required");
    if (!usageService().ready())
        return error_json(response, 503, "usage_unavailable");
    if (!write_origin_allowed(request, auth))
        return error_json(response, 403, "origin_forbidden");
    const std::string id = binding_id_from_path(request.url), revision = if_match(request);
    if (id.empty())
        return error_json(response, 404, "binding_not_found");
    if (revision.empty())
        return error_json(response, 428, "if_match_required");
    if (revision == "!")
        return error_json(response, 400, "invalid_if_match");
    std::string error;
    if (!shortLinkStore().revoke_usage_binding(id, revision, auth.owner, request_id(request), error))
        return error_json(response, error == "precondition" ? 412 : 503, error);
    response.status_code = 204;
    response.headers["Cache-Control"] = "private, no-store";
    return "";
}
