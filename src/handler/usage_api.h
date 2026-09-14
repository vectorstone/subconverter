#ifndef USAGE_API_H_INCLUDED
#define USAGE_API_H_INCLUDED

#include "server/webserver.h"

bool initializeUsageService();
void shutdownUsageService();
std::string getUsage(RESPONSE_CALLBACK_ARGS);
std::string listUsageProviders(RESPONSE_CALLBACK_ARGS);
std::string listUsageBindings(RESPONSE_CALLBACK_ARGS);
std::string previewUsageBinding(RESPONSE_CALLBACK_ARGS);
std::string createUsageBinding(RESPONSE_CALLBACK_ARGS);
std::string renameUsageBinding(RESPONSE_CALLBACK_ARGS);
std::string revokeUsageBinding(RESPONSE_CALLBACK_ARGS);

#endif
