#ifndef USAGE_TYPES_H_INCLUDED
#define USAGE_TYPES_H_INCLUDED

#include <cstdint>
#include <string>
#include <vector>

struct UsageMetrics
{
    std::string json;
};

struct UsageBinding
{
    std::string id;
    std::string owner_subject;
    std::string provider_id;
    std::string instance_id;
    std::string client_id;
    std::string identity_fingerprint;
    std::string label;
    std::string revision;
    std::int64_t created_at = 0;
    std::int64_t observed_at = 0;
    std::int64_t last_attempt_at = 0;
    std::string last_error_code;
    bool invalidated = false;
    std::string snapshot_json;
};

struct UsageProvider
{
    std::string provider_id;
    std::string label;
    std::string endpoint;
    std::string ca_file;
    std::string client_cert_file;
    std::string client_key_file;
};

struct UsageRemoteItem
{
    std::string client_id;
    std::string identity_fingerprint;
    std::string metrics_json;
};

struct UsageRemoteResponse
{
    std::string instance_id;
    std::int64_t observed_at = 0;
    std::vector<UsageRemoteItem> items;
    std::vector<std::string> missing;
    std::vector<std::string> invalid;
};

#endif
