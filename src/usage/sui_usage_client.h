#ifndef SUI_USAGE_CLIENT_H_INCLUDED
#define SUI_USAGE_CLIENT_H_INCLUDED

#include <string>
#include <vector>

#include "usage_types.h"

class SuiUsageClient
{
  public:
    bool query(const UsageProvider &provider, const std::vector<std::string> &client_ids, UsageRemoteResponse &response,
               std::string &error) const;
};

#endif
