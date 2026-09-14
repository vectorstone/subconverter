#ifndef SHORTLINK_API_H_INCLUDED
#define SHORTLINK_API_H_INCLUDED

#include <string>

#include "server/webserver.h"

class PostgresStore;

struct ShortLinkAuthIdentity
{
    std::string owner;
    bool is_admin = false;
    std::string auth_kind;
};

bool initializeShortLinkService();
bool shortLinkServiceEnabled();
bool authenticateShortLinkRequest(const Request &request, ShortLinkAuthIdentity &identity);
PostgresStore &shortLinkStore();

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
