#include <algorithm>
#include <cctype>
#include <ctime>
#include <set>
#include <string>

#include <curl/curl.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "sui_usage_client.h"

namespace
{
constexpr std::size_t max_response_bytes = 256 * 1024;

struct ResponseBuffer
{
    std::string value;
    bool too_large = false;
};

size_t write_response(char *data, size_t size, size_t count, void *opaque)
{
    ResponseBuffer &buffer = *static_cast<ResponseBuffer *>(opaque);
    const std::size_t bytes = size * count;
    if (bytes > max_response_bytes - buffer.value.size())
    {
        buffer.too_large = true;
        return 0;
    }
    buffer.value.append(data, bytes);
    return bytes;
}

bool decimal_id(const std::string &value)
{
    static const std::string max_value = "9223372036854775807";
    return !value.empty() && value.size() <= max_value.size() && value != "0" && value.front() != '0' &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); }) &&
           (value.size() < max_value.size() || value <= max_value);
}

bool fingerprint(const std::string &value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(),
                                             [](unsigned char c) { return std::isdigit(c) || (c >= 'a' && c <= 'f'); });
}

bool decimal_bytes(const rapidjson::Value &value)
{
    static const std::string max_value = "18446744073709551615";
    if (!value.IsString() || value.GetStringLength() == 0 || value.GetStringLength() > max_value.size())
        return false;
    const std::string text = value.GetString();
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); }) &&
           (text.size() < max_value.size() || text <= max_value);
}

bool validate_metrics(const rapidjson::Value &m)
{
    if (!m.IsObject())
        return false;
    const char *bytes[] = {"upload_bytes", "download_bytes", "used_bytes", "over_limit_bytes"};
    for (const char *key : bytes)
        if (!m.HasMember(key) || !decimal_bytes(m[key]))
            return false;
    for (const char *key : {"limit_bytes", "remaining_bytes"})
        if (!m.HasMember(key) || (!m[key].IsNull() && !decimal_bytes(m[key])))
            return false;
    for (const char *key : {"expires_at", "next_reset_at", "last_traffic_at"})
        if (!m.HasMember(key) || (!m[key].IsNull() && (!m[key].IsInt64() || m[key].GetInt64() < 0)))
            return false;
    for (const char *key : {"enabled", "activation_pending", "auto_reset"})
        if (!m.HasMember(key) || !m[key].IsBool())
            return false;
    if (!m.HasMember("reset_days") || !m["reset_days"].IsInt64() || m["reset_days"].GetInt64() < 0 ||
        !m.HasMember("conditions") || !m["conditions"].IsArray())
        return false;
    static const std::set<std::string> allowed = {"expired", "quota_exceeded", "quota_at_limit", "reset_due",
                                                  "invalid_reset_policy"};
    for (const auto &condition : m["conditions"].GetArray())
        if (!condition.IsString() || !allowed.count(condition.GetString()))
            return false;
    return true;
}

std::string projected_metrics(const rapidjson::Value &m)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
    w.StartObject();
    for (const char *key : {"upload_bytes", "download_bytes", "used_bytes", "limit_bytes", "remaining_bytes",
                            "over_limit_bytes", "expires_at", "enabled", "activation_pending", "auto_reset",
                            "reset_days", "next_reset_at", "last_traffic_at", "conditions"})
    {
        w.Key(key);
        m[key].Accept(w);
    }
    w.EndObject();
    return buffer.GetString();
}
} // namespace

bool SuiUsageClient::query(const UsageProvider &provider, const std::vector<std::string> &client_ids,
                           UsageRemoteResponse &output, std::string &error) const
{
    if (client_ids.empty() || client_ids.size() > 100)
    {
        error = "invalid_request";
        return false;
    }
    std::set<std::string> requested;
    for (const auto &id : client_ids)
        if (!decimal_id(id) || !requested.insert(id).second)
        {
            error = "invalid_request";
            return false;
        }
    rapidjson::StringBuffer body;
    rapidjson::Writer<rapidjson::StringBuffer> writer(body);
    writer.StartObject();
    writer.Key("schema_version");
    writer.Int(1);
    writer.Key("client_ids");
    writer.StartArray();
    for (const auto &id : client_ids)
        writer.String(id.c_str());
    writer.EndArray();
    writer.EndObject();

    CURL *curl = curl_easy_init();
    if (!curl)
    {
        error = "transport";
        return false;
    }
    ResponseBuffer response;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    std::string url = provider.endpoint;
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    url += "/v1/clients/query";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.GetString());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.GetSize()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_CAINFO, provider.ca_file.c_str());
    curl_easy_setopt(curl, CURLOPT_SSLCERT, provider.client_cert_file.c_str());
    curl_easy_setopt(curl, CURLOPT_SSLKEY, provider.client_key_file.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
#ifdef CURLOPT_PROTOCOLS_STR
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK)
    {
        error =
            response.too_large ? "response_too_large" : (code == CURLE_OPERATION_TIMEDOUT ? "timeout" : "transport");
        return false;
    }
    if (status == 401 || status == 403)
    {
        error = "provider_auth";
        return false;
    }
    if (status == 429)
    {
        error = "provider_rate_limited";
        return false;
    }
    if (status < 200 || status >= 300)
    {
        error = "provider_error";
        return false;
    }
    rapidjson::Document doc;
    doc.Parse(response.value.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("schema_version") || !doc["schema_version"].IsInt() ||
        doc["schema_version"].GetInt() != 1 || !doc.HasMember("instance_id") || !doc["instance_id"].IsString() ||
        !doc.HasMember("observed_at") || !doc["observed_at"].IsInt64() || !doc.HasMember("items") ||
        !doc["items"].IsArray() || !doc.HasMember("missing_client_ids") || !doc["missing_client_ids"].IsArray() ||
        !doc.HasMember("errors") || !doc["errors"].IsArray())
    {
        error = "invalid_response";
        return false;
    }
    output.instance_id = doc["instance_id"].GetString();
    output.observed_at = doc["observed_at"].GetInt64();
    std::set<std::string> assigned;
    if (output.instance_id.size() != 36 || output.instance_id[8] != '-' || output.instance_id[13] != '-' ||
        output.instance_id[18] != '-' || output.instance_id[23] != '-')
    {
        error = "invalid_response";
        return false;
    }
    for (const auto &item : doc["items"].GetArray())
    {
        if (!item.IsObject() || !item.HasMember("client_id") || !item["client_id"].IsString() ||
            !item.HasMember("identity_fingerprint") || !item["identity_fingerprint"].IsString() ||
            !item.HasMember("metrics") || !validate_metrics(item["metrics"]))
        {
            error = "invalid_response";
            return false;
        }
        UsageRemoteItem remote{item["client_id"].GetString(), item["identity_fingerprint"].GetString(),
                               projected_metrics(item["metrics"])};
        if (!requested.count(remote.client_id) || !assigned.insert(remote.client_id).second ||
            !fingerprint(remote.identity_fingerprint))
        {
            error = "invalid_response";
            return false;
        }
        output.items.push_back(std::move(remote));
    }
    for (const auto &item : doc["missing_client_ids"].GetArray())
    {
        if (!item.IsString() || !requested.count(item.GetString()) || !assigned.insert(item.GetString()).second)
        {
            error = "invalid_response";
            return false;
        }
        output.missing.push_back(item.GetString());
    }
    for (const auto &item : doc["errors"].GetArray())
    {
        if (!item.IsObject() || !item.HasMember("client_id") || !item["client_id"].IsString() ||
            !item.HasMember("code") || !item["code"].IsString() ||
            std::string(item["code"].GetString()) != "invalid_data" ||
            !requested.count(item["client_id"].GetString()) || !assigned.insert(item["client_id"].GetString()).second)
        {
            error = "invalid_response";
            return false;
        }
        output.invalid.push_back(item["client_id"].GetString());
    }
    if (assigned != requested)
    {
        error = "invalid_response";
        return false;
    }
    const std::int64_t now = std::time(nullptr);
    if (output.observed_at <= 0 || output.observed_at > now + 5)
    {
        error = "clock_skew";
        return false;
    }
    return true;
}
