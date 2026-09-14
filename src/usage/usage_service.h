#ifndef USAGE_SERVICE_H_INCLUDED
#define USAGE_SERVICE_H_INCLUDED

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "usage_types.h"

class PostgresStore;

class UsageService
{
  public:
    ~UsageService();
    bool initialize(PostgresStore &store);
    void shutdown();
    void wake();
    bool configured_enabled() const
    {
        return enabled_;
    }
    bool ready() const
    {
        return enabled_ && ready_;
    }
    int poll_seconds() const
    {
        return poll_seconds_;
    }
    int fresh_seconds() const
    {
        return fresh_seconds_;
    }
    int stale_seconds() const
    {
        return stale_seconds_;
    }
    const std::vector<UsageProvider> &providers() const
    {
        return providers_;
    }
    const UsageProvider *find_provider(const std::string &id) const;
    bool query_one(const std::string &provider_id, const std::string &client_id, UsageRemoteItem &item,
                   std::string &instance_id, std::int64_t &observed_at, std::string &error) const;

  private:
    void run();
    void collect_once();
    bool load_providers(const std::string &path);
    PostgresStore *store_ = nullptr;
    std::vector<UsageProvider> providers_;
    bool enabled_ = false;
    bool ready_ = false;
    int poll_seconds_ = 30;
    int fresh_seconds_ = 60;
    int stale_seconds_ = 900;
    int audit_days_ = 90;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped_ = false;
    bool wake_requested_ = false;
    std::thread thread_;
    struct ProviderBackoff
    {
        int failures = 0;
        std::int64_t next_attempt_at = 0;
    };
    std::map<std::string, ProviderBackoff> backoff_;
};

UsageService &usageService();

#endif
