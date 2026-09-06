#ifndef SHORTLINK_API_H_INCLUDED
#define SHORTLINK_API_H_INCLUDED

#include <string>

#include "server/webserver.h"

bool initializeShortLinkService();
bool shortLinkServiceEnabled();

std::string createShortLink(RESPONSE_CALLBACK_ARGS);
std::string listShortLinks(RESPONSE_CALLBACK_ARGS);
std::string revokeShortLink(RESPONSE_CALLBACK_ARGS);
std::string refreshShortLink(RESPONSE_CALLBACK_ARGS);
std::string createShortLinkApiKey(RESPONSE_CALLBACK_ARGS);
std::string revokeShortLinkApiKey(RESPONSE_CALLBACK_ARGS);
std::string listShortLinkUsers(RESPONSE_CALLBACK_ARGS);
std::string upsertShortLinkUser(RESPONSE_CALLBACK_ARGS);
std::string getShortLink(RESPONSE_CALLBACK_ARGS);

#endif // SHORTLINK_API_H_INCLUDED
