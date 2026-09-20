#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "rapidjson/document.h"

/**
 * sing-box (>= 1.14) configuration generation helpers.
 *
 * Everything in this namespace is self-contained: it does not depend on global
 * settings or on the Proxy model, so it can be unit-tested through the public
 * CLI endpoints without touching the rest of the converter.
 */
namespace singbox
{

/**
 * Target platform. Determines the inbound layout, the privileged options, the
 * rule-set source and the experimental block.
 */
enum class Platform
{
    MacOS,
    Windows,
    Linux,
    Android,
    IOS,
    OpenWrt
};

/**
 * Parse a platform name or alias.
 */
bool parsePlatform(const std::string &name, Platform &platform);

/**
 * Canonical lowercase name of a platform.
 */
std::string platformName(Platform platform);

/**
 * Comma separated list of accepted platform names, used in error messages.
 */
std::string platformList();

/**
 * Static, per-platform decisions. Only the fields that differ between
 * platforms live here so the differences stay auditable in one place.
 */
struct Profile
{
    bool mixed_inbound;
    bool tun_inbound;
    int tun_mtu;
    bool route_auto_detect_interface;
    bool tun_strict_route;
    bool tun_auto_redirect;
    bool tun_dns_hijack;          // emit tun dns_mode/dns_address
    bool dnsmasq_inbound;         // emit the direct dns-in inbound and scope DNS hijacking to it
    bool override_android_vpn;
    bool clash_mode_rules;
    bool clash_api;
    bool cache_file;
    const char *default_cache_path;
    bool local_rulesets;
    const char *log_level;
    bool log_timestamp;
    const char *dns_strategy;
};

const Profile &profileOf(Platform platform);

/**
 * Whether the platform's client can supply the data a `process_name` /
 * `process_path` / `user` condition needs.
 *
 * The Apple clients only implement connection-owner lookup in the macOS
 * standalone and jailbroken-iOS builds; the App Store/TestFlight builds throw
 * "Not implemented". Android exposes package names, never a process path.
 * Emitting such a condition anyway is not merely noisy: sing-box ANDs a process
 * condition with every other condition of the same rule, so an unmatchable one
 * silently disables the whole rule.
 */
bool platformSupportsProcessConditions(Platform platform);

/**
 * Whether the platform exposes `package_name` (Android only).
 */
bool platformSupportsPackageConditions(Platform platform);

/**
 * Whether `remark` matches any keyword of a comma separated, case-insensitive
 * substring list. An empty list matches nothing, so callers that treat "unset"
 * as "no filtering" keep their unfiltered set.
 */
bool remarkMatchesAny(const std::string &remark, const std::string &keywords);

/**
 * Runtime settings forwarded from preferences / request arguments.
 */
struct Settings
{
    Platform platform = Platform::MacOS;
    bool clash_modes = true;
    bool skeleton = false;         /// generated skeleton mode: minimal groups + remote rule-set mapping
    std::string clash_api_controller = "127.0.0.1:9095";
    std::string clash_api_secret;
    std::string dns_direct_server = "223.5.5.5";
    std::string dns_proxy_server = "1.1.1.1";
    std::string dns_direct_ruleset = "geosite-cn";
    std::string direct_tag = "DIRECT";
    std::string reject_tag = "REJECT";
    std::string proxy_tag = "proxy";
    std::string tun_address = "172.19.0.1/30";
    std::string tun_address6 = "fdfe:dcba:9876::1/126";
    bool ipv6 = false;
    std::string local_ruleset_dir = "/opt/open-box/data/rulesets";
    int ruleset_source = -1;   // -1 = platform default, 0 = remote srs, 1 = local srs
    std::string geosite_url_prefix = "https://testingcf.jsdelivr.net/gh/SagerNet/sing-geosite@rule-set/";
    std::string geoip_url_prefix = "https://testingcf.jsdelivr.net/gh/SagerNet/sing-geoip@rule-set/";
    std::string cache_path;
    /// Requested default outbound of the traffic selector. Resolved against the
    /// selector's members: a value that no longer matches one is dropped rather
    /// than emitted, because the kernel only rejects a dangling `default` at
    /// runtime ("default outbound not found"), not in `sing-box check`.
    std::string default_outbound;
    /// Comma separated remark keywords restricting the members of the `auto`
    /// urltest group. Empty keeps every node.
    std::string auto_include;
    /// Tag of the explicit default HTTP client used for remote rule-set downloads.
    std::string http_client_tag = "hc-default";
};

struct RuleSetSpec
{
    std::string tag;
    bool geoip = false;

    bool operator==(const RuleSetSpec &other) const
    {
        return tag == other.tag && geoip == other.geoip;
    }
};

/**
 * Build the platform specific skeleton (schema/log/dns/inbounds/outbounds-base/
 * route/experimental) and register the rule sets the DNS rules reference.
 */
void applySkeleton(rapidjson::Document &doc, const Settings &settings, std::vector<RuleSetSpec> &rule_sets);

/**
 * Write route.rule_set from the collected specifications.
 */
void applyRuleSets(rapidjson::Document &doc, const std::vector<RuleSetSpec> &rule_sets, const Settings &settings);

/**
 * Post-process the document once outbounds and rules are final: pick a usable
 * proxy tag, keep DNS detour and route.final consistent, drop empty values.
 */
void finalize(rapidjson::Document &doc, const Settings &settings, std::vector<std::string> &warnings);

/**
 * Translate a ruleset rule type to a sing-box rule set tag.
 * Returns false when the type is not a rule set reference.
 */
bool ruleTypeToRuleSetTag(const std::string &raw_type, const std::string &value, std::string &tag, bool &geoip);

/**
 * Chained proxy resolution result.
 */
struct ChainResult
{
    std::vector<std::pair<std::string, std::string>> detour;   // ordered (landing, via)
    std::vector<std::string> dropped;                          // human readable "<landing> <- <via> (<reason>)"
};

/**
 * Resolve upstream (dialer-proxy / underlying-proxy) declarations into `detour`
 * assignments, dropping declarations the kernel would only reject at runtime.
 *
 * @param tags          every outbound/endpoint tag that will exist
 * @param group_tags    subset of `tags` that are selector/urltest groups
 * @param group_members group tag -> member tags
 * @param upstreams     ordered (landing tag, resolved via tag) pairs
 * @param direct_tag    tag of the plain `direct` outbound (invalid as a detour target)
 */
ChainResult resolveChain(const std::vector<std::string> &tags,
                         const std::vector<std::string> &group_tags,
                         const std::map<std::string, std::vector<std::string>> &group_members,
                         const std::vector<std::pair<std::string, std::string>> &upstreams,
                         const std::string &direct_tag);

/**
 * Structural validation covering the checks `sing-box check` does not perform.
 * Returns false and fills `error` on the first fatal problem.
 */
bool validate(rapidjson::Document &doc, std::string &error);

}
