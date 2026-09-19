#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <sstream>

#include <rapidjson/stringbuffer.h>

#include "generator/config/singbox.h"
#include "utils/rapidjson_extra.h"
#include "utils/string.h"

using namespace rapidjson;
using namespace rapidjson_ext;

namespace singbox
{

namespace
{

Value makeString(const std::string &value, Document::AllocatorType &allocator)
{
    Value result(kStringType);
    result.SetString(value.c_str(), static_cast<SizeType>(value.size()), allocator);
    return result;
}

/**
 * Case-insensitive substring test, used to match a node remark against the
 * operator supplied keywords: remarks carry emoji, provider prefixes and
 * varying case, so an exact comparison would be unusable in practice.
 */
bool containsIgnoreCase(const std::string &haystack, const std::string &needle)
{
    if(needle.empty())
        return false;
    const std::string lower_haystack = toLower(haystack);
    const std::string lower_needle = toLower(needle);
    return lower_haystack.find(lower_needle) != std::string::npos;
}

Value makeArray(const std::vector<std::string> &values, Document::AllocatorType &allocator)
{
    Value result(kArrayType);
    for(const std::string &value : values)
        result.PushBack(makeString(value, allocator), allocator);
    return result;
}

void addSpec(std::vector<RuleSetSpec> &specs, const std::string &tag)
{
    if(tag.empty())
        return;
    RuleSetSpec spec;
    spec.tag = tag;
    spec.geoip = startsWith(tag, "geoip-");
    if(std::find(specs.begin(), specs.end(), spec) == specs.end())
        specs.push_back(spec);
}

/**
 * Derive the tun gateway address (the peer address used for tun level DNS
 * hijacking) from the tun address, e.g. "172.19.0.1/30" -> "172.19.0.2" and
 * "fdfe:dcba:9876::1/126" -> "fdfe:dcba:9876::2".
 *
 * Both families are handled the same way: the last group is incremented in
 * place. Keeping this family agnostic matters because the tun inbound may carry
 * an IPv4 and an IPv6 address at once, and each one needs its own DNS listener.
 * An address the parser does not understand is returned without its prefix
 * rather than guessed at.
 */
std::string tunGateway(const std::string &address)
{
    const std::string ip = address.substr(0, address.find('/'));
    const bool ipv6 = ip.find(':') != std::string::npos;
    const std::size_t separator = ip.rfind(ipv6 ? ':' : '.');
    if(separator == std::string::npos || separator == 0)
        return ip;
    const std::string last = ip.substr(separator + 1);
    const bool valid = !last.empty() && std::all_of(last.begin(), last.end(), [ipv6](unsigned char c) {
        return ipv6 ? std::isxdigit(c) != 0 : std::isdigit(c) != 0;
    });
    if(!valid)
        return ip;
    if(ipv6)
    {
        const unsigned long group = std::stoul(last, nullptr, 16);
        if(group == 0xffff)
            return ip;
        std::ostringstream next;
        next << std::hex << group + 1;
        return ip.substr(0, separator + 1) + next.str();
    }
    return ip.substr(0, separator + 1) + std::to_string(std::stoi(last) + 1);
}

std::string memberString(const Value &value, const char *name)
{
    if(!value.IsObject() || !value.HasMember(name) || !value[name].IsString())
        return "";
    return value[name].GetString();
}

std::vector<std::string> stringArray(const Value &value, const char *name)
{
    std::vector<std::string> result;
    if(!value.IsObject() || !value.HasMember(name) || !value[name].IsArray())
        return result;
    for(const Value &item : value[name].GetArray())
        if(item.IsString())
            result.emplace_back(item.GetString());
    return result;
}

std::vector<std::string> collectTags(const Document &doc)
{
    std::vector<std::string> tags;
    for(const char *member : {"outbounds", "endpoints"})
    {
        if(!doc.HasMember(member) || !doc[member].IsArray())
            continue;
        for(const Value &item : doc[member].GetArray())
        {
            const std::string tag = memberString(item, "tag");
            if(!tag.empty())
                tags.push_back(tag);
        }
    }
    return tags;
}

bool isPlainDirect(const Value &outbound, const std::string &tag)
{
    if(memberString(outbound, "type") != "direct" || memberString(outbound, "tag") != tag)
        return false;
    return outbound.MemberCount() <= 2;
}

/**
 * Whether the emitted configuration downloads no rule set at all, i.e. every
 * rule-set reference resolves to a local `.srs` file. Both the skeleton (which
 * decides whether to declare an HTTP client) and applyRuleSets (which writes the
 * `remote` entries) must agree, so the decision lives in one place.
 */
bool usesLocalRuleSets(const Settings &settings)
{
    const Profile &profile = profileOf(settings.platform);
    return settings.ruleset_source < 0 ? profile.local_rulesets : settings.ruleset_source > 0;
}

void ensureProxyReference(Document &doc, const Settings &settings, std::vector<std::string> &warnings)
{
    if(!doc.HasMember("route") || !doc["route"].IsObject())
        return;
    if(!doc.HasMember("outbounds") || !doc["outbounds"].IsArray())
        return;
    Value &route = doc["route"];
    auto &allocator = doc.GetAllocator();

    std::vector<std::string> tags = collectTags(doc);
    std::vector<std::string> group_tags;
    for(const Value &outbound : doc["outbounds"].GetArray())
    {
        const std::string type = memberString(outbound, "type");
        if(type == "selector" || type == "urltest")
            group_tags.push_back(memberString(outbound, "tag"));
    }

    std::string final_tag;
    if(route.HasMember("final") && route["final"].IsString())
        final_tag = route["final"].GetString();

    const bool final_valid = !final_tag.empty() &&
        (std::find(tags.begin(), tags.end(), final_tag) != tags.end());
    if(!final_valid)
    {
        std::string fallback = group_tags.empty() ? settings.direct_tag : group_tags.front();
        /// The skeleton seeds route.final with the default proxy tag; resolve it to a
        /// real group silently instead of warning on every request.
        if(!final_tag.empty() && final_tag != settings.proxy_tag)
            warnings.push_back("route.final '" + final_tag + "' is not an outbound tag, using '" + fallback + "'");
        final_tag = fallback;
    }
    route | AddMemberOrReplace("final", makeString(final_tag, allocator), allocator);

    // Keep the proxy DNS server's detour pointing at a real outbound.
    if(doc.HasMember("dns") && doc["dns"].IsObject() && doc["dns"].HasMember("servers"))
    {
        for(Value &server : doc["dns"]["servers"].GetArray())
        {
            if(memberString(server, "tag") != "dns-proxy")
                continue;
            if(final_tag == settings.direct_tag)
            {
                if(server.HasMember("detour"))
                    server.RemoveMember("detour");
                continue;
            }
            server | AddMemberOrReplace("detour", makeString(final_tag, allocator), allocator);
        }
    }

    // Keep the rule-set download client on the traffic selector too, mirroring
    // the DNS detour above: the implicit client this replaces dialled through
    // route.final, and a direct dial cannot reach the rule-set hosts from
    // mainland China, where a failed initial download is a fatal startup error.
    if(doc.HasMember("http_clients") && doc["http_clients"].IsArray())
    {
        for(Value &client : doc["http_clients"].GetArray())
        {
            if(!client.HasMember("detour"))
                continue;
            if(final_tag == settings.direct_tag)
                client.RemoveMember("detour");
            else
                client | AddMemberOrReplace("detour", makeString(final_tag, allocator), allocator);
        }
    }
}

void sanitizeGroups(Document &doc, const std::string &direct_tag, std::vector<std::string> &warnings)
{
    if(!doc.HasMember("outbounds") || !doc["outbounds"].IsArray())
        return;
    auto &allocator = doc.GetAllocator();
    const std::vector<std::string> tags = collectTags(doc);
    for(Value &outbound : doc["outbounds"].GetArray())
    {
        const std::string type = memberString(outbound, "type");
        if(type != "selector" && type != "urltest")
            continue;
        if(outbound.HasMember("detour"))
        {
            warnings.push_back("group '" + memberString(outbound, "tag") + "' cannot carry detour, removed");
            outbound.RemoveMember("detour");
        }
        if(type == "urltest")
        {
            const std::string url = memberString(outbound, "url");
            if(url.empty())
                outbound | AddMemberOrReplace("url", makeString("https://www.gstatic.com/generate_http_204", allocator), allocator);
            if(outbound.HasMember("interval") && outbound["interval"].IsString() && outbound["interval"].GetStringLength() == 0)
                outbound.RemoveMember("interval");
        }
        if(!outbound.HasMember("outbounds") || !outbound["outbounds"].IsArray())
            continue;
        Value filtered(kArrayType);
        for(const Value &member : outbound["outbounds"].GetArray())
        {
            if(!member.IsString())
                continue;
            const std::string tag = member.GetString();
            if(std::find(tags.begin(), tags.end(), tag) == tags.end())
            {
                warnings.push_back("group '" + memberString(outbound, "tag") + "' member '" + tag + "' does not exist, removed");
                continue;
            }
            filtered.PushBack(makeString(tag, allocator), allocator);
        }
        if(filtered.Empty())
            filtered.PushBack(makeString(direct_tag, allocator), allocator);
        outbound | AddMemberOrReplace("outbounds", filtered, allocator);
    }
}

}

bool remarkMatchesAny(const std::string &remark, const std::string &keywords)
{
    std::size_t begin = 0;
    while(begin <= keywords.size())
    {
        const std::size_t end = keywords.find(',', begin);
        const std::size_t length = end == std::string::npos ? std::string::npos : end - begin;
        if(containsIgnoreCase(remark, trim(keywords.substr(begin, length))))
            return true;
        if(end == std::string::npos)
            break;
        begin = end + 1;
    }
    return false;
}

bool parsePlatform(const std::string &name, Platform &platform)
{
    const std::string key = toLower(trim(name));
    if(key == "macos" || key == "mac" || key == "darwin" || key == "osx")
        platform = Platform::MacOS;
    else if(key == "windows" || key == "win")
        platform = Platform::Windows;
    else if(key == "linux" || key == "desktop")
        platform = Platform::Linux;
    else if(key == "android")
        platform = Platform::Android;
    else if(key == "ios" || key == "apple" || key == "iphone" || key == "ipados")
        platform = Platform::IOS;
    else if(key == "openwrt" || key == "immortalwrt" || key == "router")
        platform = Platform::OpenWrt;
    else
        return false;
    return true;
}

std::string platformName(Platform platform)
{
    switch(platform)
    {
    case Platform::MacOS: return "macos";
    case Platform::Windows: return "windows";
    case Platform::Linux: return "linux";
    case Platform::Android: return "android";
    case Platform::IOS: return "ios";
    case Platform::OpenWrt: return "openwrt";
    }
    return "macos";
}

std::string platformList()
{
    return "macos, windows, linux, android, ios, openwrt";
}

bool platformSupportsProcessConditions(Platform platform)
{
    // sing-box matches process_name / process_path against
    // ConnectionOwner.ProcessPath, which only the macOS standalone build and
    // jailbroken iOS can obtain; the official App Store/TestFlight clients
    // throw "Not implemented" (ExtensionPlatformInterface.swift), and Android
    // reports package names instead of a process path.
    return platform == Platform::MacOS || platform == Platform::Windows || platform == Platform::Linux || platform == Platform::OpenWrt;
}

bool platformSupportsPackageConditions(Platform platform)
{
    return platform == Platform::Android;
}

const Profile &profileOf(Platform platform)
{
    // No platform emits tun `stack`: the value is deprecated in sing-box 1.15.0
    // and removed in 1.17.0, and `gvisor`/`mixed` additionally hard-fail on every
    // client built without `with_gvisor` (all official Apple clients). Omitting
    // the field lets each kernel pick its best available implementation.
    //
    // Every platform shares one tun mtu (1500). A larger value (the previous
    // 9000/8500) only pays off when the whole path carries jumbo frames, which
    // it never does over a mobile link, while it does invite PMTU blackholes on
    // the segments that clamp lower. Measured as neutral in the macOS field
    // report; kept conventional because being neutral is the point.
    //
    // `default_cache_path` is only read where `cache_file` is enabled. It stays
    // a *relative* name on purpose: the kernel expands neither `~` nor `$HOME`
    // and refuses to start when the parent directory is missing ("FATAL start
    // service: initialize cache-file"), so an absolute path would only be safe
    // where the deployment itself guarantees the directory (see OpenWrt). A
    // relative name resolves against the process working directory, which is
    // what the kernel already defaults to — writing it out keeps the rule-set
    // cache pinned to a known name instead of relying on that default.
    static const Profile macos_profile = {
        true, true, 1500, true, true, false, false, false, false,
        true, true, true, "cache.db", false, "warn", true, "prefer_ipv4"
    };
    static const Profile windows_profile = {
        true, true, 1500, true, true, false, false, false, false,
        true, true, true, "cache.db", false, "warn", true, "prefer_ipv4"
    };
    static const Profile linux_profile = {
        true, true, 1500, true, true, false, false, false, false,
        true, true, true, "cache.db", false, "warn", true, "prefer_ipv4"
    };
    // android: route_auto_detect_interface must stay true. `constant.IsLinux`
    // covers Android, so the kernel accepts the field, and it is the only path
    // that reaches NetworkManager.ProtectFunc() ->
    // PlatformInterface.AutoDetectInterfaceControl() -> VpnService.protect().
    // Without it every outbound socket (including the dns-direct/dns-proxy DNS
    // transports) is routed back into the tun: the tunnel starts, then the
    // device forwards DNS to itself in a loop and has no usable traffic.
    // iOS is the opposite: ExtensionPlatformInterface.usePlatformAutoDetectControl()
    // returns false, so the field would merely bind physical NICs.
    static const Profile android_profile = {
        false, true, 1500, true, false, false, true, false, true,
        false, false, false, "cache.db", false, "warn", true, "prefer_ipv4"
    };
    static const Profile ios_profile = {
        false, true, 1500, false, false, false, true, false, false,
        false, false, false, "cache.db", false, "warn", true, "prefer_ipv4"
    };
    static const Profile openwrt_profile = {
        false, true, 1500, true, true, true, false, true, false,
        true, true, true, "/opt/open-box/data/cache.db", true, "warn", false, "ipv4_only"
    };

    switch(platform)
    {
    case Platform::MacOS: return macos_profile;
    case Platform::Windows: return windows_profile;
    case Platform::Linux: return linux_profile;
    case Platform::Android: return android_profile;
    case Platform::IOS: return ios_profile;
    case Platform::OpenWrt: return openwrt_profile;
    }
    return macos_profile;
}

bool ruleTypeToRuleSetTag(const std::string &raw_type, const std::string &value, std::string &tag, bool &geoip)
{
    const std::string type = toUpper(trim(raw_type));
    std::string name = toLower(trim(value));
    if(name.empty())
        return false;
    if(type == "GEOSITE")
    {
        tag = "geosite-" + name;
        geoip = false;
        return true;
    }
    if(type == "GEOIP")
    {
        tag = "geoip-" + name;
        geoip = true;
        return true;
    }
    return false;
}

void applySkeleton(Document &doc, const Settings &settings, std::vector<RuleSetSpec> &rule_sets)
{
    const Profile &profile = profileOf(settings.platform);
    auto &allocator = doc.GetAllocator();
    doc.SetObject();

    doc.AddMember("$schema", makeString("https://sing-box.sagernet.org/schema.json", allocator), allocator);

    {
        Value log(kObjectType);
        log.AddMember("level", makeString(profile.log_level, allocator), allocator);
        if(profile.log_timestamp)
            log.AddMember("timestamp", true, allocator);
        doc.AddMember("log", log, allocator);
    }

    {
        Value servers(kArrayType);
        Value direct(kObjectType);
        direct.AddMember("type", makeString("udp", allocator), allocator);
        direct.AddMember("tag", makeString("dns-direct", allocator), allocator);
        direct.AddMember("server", makeString(settings.dns_direct_server, allocator), allocator);
        servers.PushBack(direct, allocator);

        Value proxy(kObjectType);
        proxy.AddMember("type", makeString("https", allocator), allocator);
        proxy.AddMember("tag", makeString("dns-proxy", allocator), allocator);
        proxy.AddMember("server", makeString(settings.dns_proxy_server, allocator), allocator);
        proxy.AddMember("detour", makeString(settings.proxy_tag, allocator), allocator);
        proxy.AddMember("domain_resolver", makeString("dns-direct", allocator), allocator);
        servers.PushBack(proxy, allocator);

        Value rules(kArrayType);
        if(!settings.dns_direct_ruleset.empty())
        {
            Value rule(kObjectType);
            std::vector<std::string> refs{settings.dns_direct_ruleset};
            rule.AddMember("rule_set", makeArray(refs, allocator), allocator);
            rule.AddMember("action", makeString("route", allocator), allocator);
            rule.AddMember("server", makeString("dns-direct", allocator), allocator);
            rules.PushBack(rule, allocator);
            addSpec(rule_sets, settings.dns_direct_ruleset);
        }
        {
            Value rule(kObjectType);
            rule.AddMember("action", makeString("route", allocator), allocator);
            rule.AddMember("server", makeString("dns-proxy", allocator), allocator);
            rules.PushBack(rule, allocator);
        }

        Value dns(kObjectType);
        dns.AddMember("servers", servers, allocator);
        dns.AddMember("rules", rules, allocator);
        dns.AddMember("final", makeString("dns-proxy", allocator), allocator);
        dns.AddMember("strategy", makeString(profile.dns_strategy, allocator), allocator);
        doc.AddMember("dns", dns, allocator);
    }

    {
        Value inbounds(kArrayType);
        if(profile.tun_inbound)
        {
            Value tun(kObjectType);
            tun.AddMember("type", makeString("tun", allocator), allocator);
            tun.AddMember("tag", makeString("tun-in", allocator), allocator);
            std::vector<std::string> addresses{settings.tun_address};
            if(settings.ipv6)
                addresses.push_back(settings.tun_address6);
            tun.AddMember("address", makeArray(addresses, allocator), allocator);
            if(profile.tun_mtu > 0)
                tun.AddMember("mtu", profile.tun_mtu, allocator);
            tun.AddMember("auto_route", true, allocator);
            if(profile.tun_strict_route)
                tun.AddMember("strict_route", true, allocator);
            if(profile.tun_auto_redirect)
                tun.AddMember("auto_redirect", true, allocator);
            if(profile.tun_dns_hijack)
            {
                tun.AddMember("dns_mode", makeString("hijack", allocator), allocator);
                /// One listener per tun address: a client resolving over the IPv6
                /// address must not fall outside the hijacked range.
                std::vector<std::string> dns_address{tunGateway(settings.tun_address)};
                if(settings.ipv6)
                    dns_address.push_back(tunGateway(settings.tun_address6));
                tun.AddMember("dns_address", makeArray(dns_address, allocator), allocator);
            }
            inbounds.PushBack(tun, allocator);
        }
        if(profile.mixed_inbound)
        {
            Value mixed(kObjectType);
            mixed.AddMember("type", makeString("mixed", allocator), allocator);
            mixed.AddMember("tag", makeString("mixed-in", allocator), allocator);
            mixed.AddMember("listen", makeString("127.0.0.1", allocator), allocator);
            mixed.AddMember("listen_port", 2080, allocator);
            inbounds.PushBack(mixed, allocator);
        }
        if(profile.dnsmasq_inbound)
        {
            Value dns_in(kObjectType);
            dns_in.AddMember("type", makeString("direct", allocator), allocator);
            dns_in.AddMember("tag", makeString("dns-in", allocator), allocator);
            dns_in.AddMember("listen", makeString("127.0.0.1", allocator), allocator);
            dns_in.AddMember("listen_port", 7853, allocator);
            inbounds.PushBack(dns_in, allocator);
        }
        doc.AddMember("inbounds", inbounds, allocator);
    }

    {
        Value outbounds(kArrayType);
        Value direct(kObjectType);
        direct.AddMember("type", makeString("direct", allocator), allocator);
        direct.AddMember("tag", makeString(settings.direct_tag, allocator), allocator);
        outbounds.PushBack(direct, allocator);
        doc.AddMember("outbounds", outbounds, allocator);
    }

    /// Remote rule sets must name their HTTP client: relying on the implicit
    /// default (which dials through the default outbound) is deprecated in
    /// 1.14.0 and removed in 1.16.0. The explicit client keeps the same egress
    /// the implicit one used — the traffic selector group — instead of falling
    /// back to a direct dial: the bundled rule-set hosts are unreachable
    /// directly from mainland China, and a failed initial remote rule-set
    /// download is a fatal startup error.
    if(!usesLocalRuleSets(settings) && !settings.http_client_tag.empty())
    {
        Value clients(kArrayType);
        Value client(kObjectType);
        client.AddMember("tag", makeString(settings.http_client_tag, allocator), allocator);
        if(!settings.proxy_tag.empty())
            client.AddMember("detour", makeString(settings.proxy_tag, allocator), allocator);
        clients.PushBack(client, allocator);
        doc.AddMember("http_clients", clients, allocator);
    }

    {
        Value rules(kArrayType);
        {
            Value rule(kObjectType);
            rule.AddMember("action", makeString("sniff", allocator), allocator);
            rules.PushBack(rule, allocator);
        }
        {
            Value rule(kObjectType);
            if(profile.dnsmasq_inbound)
            {
                rule.AddMember("inbound", makeArray({"dns-in"}, allocator), allocator);
                rule.AddMember("action", makeString("hijack-dns", allocator), allocator);
            }
            else
            {
                rule.AddMember("protocol", makeString("dns", allocator), allocator);
                rule.AddMember("action", makeString("hijack-dns", allocator), allocator);
            }
            rules.PushBack(rule, allocator);
        }
        if(profile.tun_auto_redirect)
        {
            Value rule(kObjectType);
            rule.AddMember("ip_cidr", makeArray({settings.tun_address}, allocator), allocator);
            rule.AddMember("action", makeString("reject", allocator), allocator);
            rules.PushBack(rule, allocator);
        }
        {
            Value rule(kObjectType);
            rule.AddMember("ip_is_private", true, allocator);
            rule.AddMember("action", makeString("route", allocator), allocator);
            rule.AddMember("outbound", makeString(settings.direct_tag, allocator), allocator);
            rules.PushBack(rule, allocator);
        }
        if(profile.clash_mode_rules && settings.clash_modes)
        {
            Value direct_mode(kObjectType);
            direct_mode.AddMember("clash_mode", makeString("Direct", allocator), allocator);
            direct_mode.AddMember("action", makeString("route", allocator), allocator);
            direct_mode.AddMember("outbound", makeString(settings.direct_tag, allocator), allocator);
            rules.PushBack(direct_mode, allocator);

            Value global_mode(kObjectType);
            global_mode.AddMember("clash_mode", makeString("Global", allocator), allocator);
            global_mode.AddMember("action", makeString("route", allocator), allocator);
            global_mode.AddMember("outbound", makeString("GLOBAL", allocator), allocator);
            rules.PushBack(global_mode, allocator);
        }

        Value route(kObjectType);
        if(profile.route_auto_detect_interface)
            route.AddMember("auto_detect_interface", true, allocator);
        if(profile.override_android_vpn)
            route.AddMember("override_android_vpn", true, allocator);
        route.AddMember("default_domain_resolver", makeString("dns-direct", allocator), allocator);
        if(!usesLocalRuleSets(settings) && !settings.http_client_tag.empty())
            route.AddMember("default_http_client", makeString(settings.http_client_tag, allocator), allocator);
        route.AddMember("rule_set", Value(kArrayType), allocator);
        route.AddMember("rules", rules, allocator);
        route.AddMember("final", makeString(settings.proxy_tag, allocator), allocator);
        doc.AddMember("route", route, allocator);
    }

    {
        Value experimental(kObjectType);
        if(profile.cache_file)
        {
            Value cache(kObjectType);
            cache.AddMember("enabled", true, allocator);
            /// The request may override the location (singbox_cache_path); the
            /// platform default keeps the rule-set cache out of the caller's
            /// hands so a short link cannot point it at an unwritable path.
            const std::string path = settings.cache_path.empty() ? profile.default_cache_path : settings.cache_path;
            if(!path.empty())
                cache.AddMember("path", makeString(path, allocator), allocator);
            experimental.AddMember("cache_file", cache, allocator);
        }
        if(profile.clash_api)
        {
            Value api(kObjectType);
            api.AddMember("external_controller", makeString(settings.clash_api_controller, allocator), allocator);
            if(!settings.clash_api_secret.empty())
                api.AddMember("secret", makeString(settings.clash_api_secret, allocator), allocator);
            api.AddMember("default_mode", makeString("Rule", allocator), allocator);
            experimental.AddMember("clash_api", api, allocator);
        }
        if(!experimental.ObjectEmpty())
            doc.AddMember("experimental", experimental, allocator);
    }
}

void applyRuleSets(Document &doc, const std::vector<RuleSetSpec> &rule_sets, const Settings &settings)
{
    if(rule_sets.empty() || !doc.HasMember("route") || !doc["route"].IsObject())
        return;
    auto &allocator = doc.GetAllocator();
    const bool local_rulesets = usesLocalRuleSets(settings);
    Value array(kArrayType);
    std::set<std::string> seen;
    for(const RuleSetSpec &spec : rule_sets)
    {
        if(spec.tag.empty() || !seen.insert(spec.tag).second)
            continue;
        Value entry(kObjectType);
        entry.AddMember("tag", makeString(spec.tag, allocator), allocator);
        entry.AddMember("format", makeString("binary", allocator), allocator);
        if(local_rulesets)
        {
            entry.AddMember("type", makeString("local", allocator), allocator);
            entry.AddMember("path", makeString(settings.local_ruleset_dir + "/" + spec.tag + ".srs", allocator), allocator);
        }
        else
        {
            entry.AddMember("type", makeString("remote", allocator), allocator);
            const std::string prefix = spec.geoip ? settings.geoip_url_prefix : settings.geosite_url_prefix;
            entry.AddMember("url", makeString(prefix + spec.tag + ".srs", allocator), allocator);
            entry.AddMember("update_interval", makeString("1d", allocator), allocator);
            if(!settings.http_client_tag.empty())
                entry.AddMember("http_client", makeString(settings.http_client_tag, allocator), allocator);
        }
        array.PushBack(entry, allocator);
    }
    doc["route"] | AddMemberOrReplace("rule_set", array, allocator);
}

/**
 * Deterministic serialization with object members sorted by name, so two rules
 * compare equal whenever they carry the same content regardless of the order
 * their keys were inserted in.
 */
void appendCanonicalJson(const Value &value, std::string &out)
{
    if(value.IsObject())
    {
        std::vector<std::pair<std::string, const Value *>> members;
        members.reserve(value.MemberCount());
        for(auto it = value.MemberBegin(); it != value.MemberEnd(); ++it)
            members.emplace_back(std::string(it->name.GetString(), it->name.GetStringLength()), &it->value);
        std::sort(members.begin(), members.end(),
                  [](const auto &left, const auto &right) { return left.first < right.first; });
        out += '{';
        for(std::size_t i = 0; i < members.size(); ++i)
        {
            if(i)
                out += ',';
            const Value name(members[i].first.c_str(), static_cast<SizeType>(members[i].first.size()));
            appendCanonicalJson(name, out);
            out += ':';
            appendCanonicalJson(*members[i].second, out);
        }
        out += '}';
        return;
    }
    if(value.IsArray())
    {
        out += '[';
        for(SizeType i = 0; i < value.Size(); ++i)
        {
            if(i)
                out += ',';
            appendCanonicalJson(value[i], out);
        }
        out += ']';
        return;
    }
    StringBuffer buffer;
    Writer<StringBuffer> writer(buffer);
    value.Accept(writer);
    out += buffer.GetString();
}

/**
 * Drop route rules that repeat an earlier rule verbatim.
 *
 * Routing inside route.rules is first-match, so an exact duplicate is
 * unreachable: removing it changes no routing decision while keeping the
 * emitted list — and therefore the `match[N]` indices sing-box logs — one-to-one
 * with the rules that actually decide traffic. A rule that merely overlaps an
 * earlier one is left alone; it still owns the traffic that rule misses.
 */
void dedupeRules(Document &doc, std::vector<std::string> &warnings)
{
    if(!doc.HasMember("route") || !doc["route"].IsObject())
        return;
    Value &route = doc["route"];
    if(!route.HasMember("rules") || !route["rules"].IsArray())
        return;

    auto &allocator = doc.GetAllocator();
    std::set<std::string> seen;
    Value unique(kArrayType);
    SizeType removed = 0;
    for(Value &rule : route["rules"].GetArray())
    {
        std::string key;
        appendCanonicalJson(rule, key);
        if(!seen.insert(key).second)
        {
            ++removed;
            continue;
        }
        unique.PushBack(rule, allocator);
    }
    if(!removed)
        return;
    route | AddMemberOrReplace("rules", unique, allocator);
    warnings.push_back("removed " + std::to_string(removed) + " duplicate route rule(s)");
}

/**
 * Pin the requested outbound as the traffic selector's explicit `default`.
 *
 * Without it a selector falls back to its first member, which here is `auto` —
 * a urltest group, so the entry landing is re-decided by latency during the
 * startup convergence window and again on every `interval`, and a transient
 * whole-group failure (a DNS race at startup marks every member unreachable)
 * silently moves the traffic. An explicit `default` is the only way a generated
 * configuration can name a stable landing.
 *
 * The value is a hint resolved against the selector's *current* members: one
 * that matches no member (the node left the subscription) or several (an
 * ambiguous keyword) is dropped with a warning instead of shipped. That is not
 * cosmetic — the kernel accepts a dangling `default` in `sing-box check` and
 * then refuses to start at runtime.
 */
void applyGroupDefault(Document &doc, const Settings &settings, std::vector<std::string> &warnings)
{
    const std::string requested = trim(settings.default_outbound);
    if(requested.empty())
        return;
    if(!doc.HasMember("outbounds") || !doc["outbounds"].IsArray())
        return;
    auto &allocator = doc.GetAllocator();

    bool found_group = false;
    for(Value &outbound : doc["outbounds"].GetArray())
    {
        if(memberString(outbound, "type") != "selector" || memberString(outbound, "tag") != settings.proxy_tag)
            continue;
        found_group = true;
        const std::vector<std::string> members = stringArray(outbound, "outbounds");

        std::string resolved;
        std::size_t hits = 0;
        for(const std::string &member : members)
        {
            if(toLower(member) != toLower(requested))
                continue;
            resolved = member;
            hits = 1;
            break;
        }
        if(resolved.empty())
            for(const std::string &member : members)
                if(containsIgnoreCase(member, requested))
                {
                    resolved = member;
                    ++hits;
                }
        if(hits > 1)
        {
            warnings.push_back("singbox_default '" + requested + "' matches " +
                               std::to_string(hits) + " members of '" + settings.proxy_tag + "', ignored");
            return;
        }
        if(resolved.empty())
        {
            warnings.push_back("singbox_default '" + requested + "' is not a member of '" +
                               settings.proxy_tag + "', ignored");
            return;
        }
        outbound | AddMemberOrReplace("default", makeString(resolved, allocator), allocator);
        return;
    }
    if(!found_group)
        warnings.push_back("singbox_default '" + requested + "' ignored: no '" + settings.proxy_tag + "' selector exists");
}

void finalize(Document &doc, const Settings &settings, std::vector<std::string> &warnings)
{
    dedupeRules(doc, warnings);
    sanitizeGroups(doc, settings.direct_tag, warnings);
    /// After sanitizeGroups: it drops members that no longer resolve, and the
    /// default must be picked from the surviving set.
    applyGroupDefault(doc, settings, warnings);
    ensureProxyReference(doc, settings, warnings);
}

ChainResult resolveChain(const std::vector<std::string> &tags,
                         const std::vector<std::string> &group_tags,
                         const std::map<std::string, std::vector<std::string>> &group_members,
                         const std::vector<std::pair<std::string, std::string>> &upstreams,
                         const std::string &direct_tag)
{
    ChainResult result;
    std::set<std::string> known(tags.begin(), tags.end());
    std::set<std::string> groups(group_tags.begin(), group_tags.end());

    std::map<std::string, std::vector<std::string>> edges;
    for(const auto &entry : group_members)
        edges[entry.first] = entry.second;
    for(const auto &entry : upstreams)
        if(!entry.second.empty())
            edges[entry.first] = {entry.second};

    std::map<std::string, std::string> accepted;
    auto depends_on = [&](const std::string &from, const std::string &target) {
        std::set<std::string> visited;
        std::vector<std::string> stack{from};
        while(!stack.empty())
        {
            const std::string current = stack.back();
            stack.pop_back();
            if(current == target)
                return true;
            if(!visited.insert(current).second)
                continue;
            auto overridden = accepted.find(current);
            std::vector<std::string> next;
            if(overridden != accepted.end())
                next.push_back(overridden->second);
            else
            {
                auto it = edges.find(current);
                if(it != edges.end())
                    next = it->second;
            }
            for(const std::string &tag : next)
                if(!visited.count(tag))
                    stack.push_back(tag);
        }
        return false;
    };

    for(const auto &entry : upstreams)
    {
        const std::string &landing = entry.first;
        const std::string &via = entry.second;
        auto drop = [&](const std::string &reason) {
            result.dropped.push_back(landing + " <- " + (via.empty() ? std::string("?") : via) + " (" + reason + ")");
        };
        if(via.empty())
        {
            drop("unresolved-via");
            continue;
        }
        if(groups.count(landing))
        {
            drop("landing-is-group");
            continue;
        }
        if(!known.count(landing))
        {
            drop("missing-landing");
            continue;
        }
        if(!known.count(via))
        {
            drop("missing-via");
            continue;
        }
        if(landing == via)
        {
            drop("self-loop");
            continue;
        }
        if(via == direct_tag)
        {
            drop("empty-direct");
            continue;
        }
        if(depends_on(via, landing))
        {
            drop("cycle");
            continue;
        }
        accepted[landing] = via;
        result.detour.emplace_back(landing, via);
    }
    return result;
}

bool validate(Document &doc, std::string &error)
{
    if(!doc.IsObject())
    {
        error = "configuration is not a JSON object";
        return false;
    }

    const std::vector<std::string> tags = collectTags(doc);
    std::set<std::string> known(tags.begin(), tags.end());

    if(!doc.HasMember("outbounds") || !doc["outbounds"].IsArray() || doc["outbounds"].Empty())
    {
        error = "no outbounds were generated";
        return false;
    }

    std::set<std::string> group_tags;
    std::map<std::string, std::vector<std::string>> edges;
    bool group_carries_detour = false;
    for(const Value &outbound : doc["outbounds"].GetArray())
    {
        const std::string type = memberString(outbound, "type");
        const std::string tag = memberString(outbound, "tag");
        if(tag.empty())
        {
            error = "an outbound is missing its tag";
            return false;
        }
        if(type == "selector" || type == "urltest")
        {
            group_tags.insert(tag);
            if(outbound.HasMember("detour"))
                group_carries_detour = true;
            const std::vector<std::string> members = stringArray(outbound, "outbounds");
            if(members.empty())
            {
                error = "group '" + tag + "' has no members";
                return false;
            }
            for(const std::string &member : members)
                if(!known.count(member))
                {
                    error = "group '" + tag + "' references unknown tag '" + member + "'";
                    return false;
                }
            /// `sing-box check` accepts a `default` that is not a member and the
            /// kernel then refuses to start ("default outbound not found"), so
            /// the check has to live here.
            if(outbound.HasMember("default"))
            {
                const std::string def = memberString(outbound, "default");
                if(def.empty())
                {
                    error = "group '" + tag + "' has a malformed default";
                    return false;
                }
                if(std::find(members.begin(), members.end(), def) == members.end())
                {
                    error = "group '" + tag + "' defaults to '" + def + "', which is not one of its members";
                    return false;
                }
            }
            edges[tag] = members;
        }
        else if(outbound.HasMember("detour"))
        {
            if(!outbound["detour"].IsString() || std::string(outbound["detour"].GetString()).empty())
            {
                error = "outbound '" + tag + "' has a malformed detour";
                return false;
            }
            const std::string via = outbound["detour"].GetString();
            if(!known.count(via))
            {
                error = "outbound '" + tag + "' detours to unknown tag '" + via + "'";
                return false;
            }
            if(isPlainDirect(outbound, tag))
            {
                error = "outbound '" + tag + "' is a plain direct outbound and cannot be used";
                return false;
            }
            edges[tag] = {via};
        }
    }

    if(group_carries_detour)
    {
        error = "a selector/urltest group carries a detour";
        return false;
    }

    // Cycle detection over detour and group membership edges.
    std::map<std::string, int> state;   // 0 = new, 1 = visiting, 2 = done
    std::vector<std::string> stack;
    std::function<bool(const std::string &)> visit = [&](const std::string &node) {
        state[node] = 1;
        auto it = edges.find(node);
        if(it != edges.end())
            for(const std::string &next : it->second)
            {
                if(state[next] == 1)
                {
                    error = "circular outbound dependency involving '" + next + "'";
                    return false;
                }
                if(state[next] == 0 && !visit(next))
                    return false;
            }
        state[node] = 2;
        return true;
    };
    for(const std::string &tag : tags)
        if(state[tag] == 0 && !visit(tag))
            return false;

    if(doc.HasMember("route") && doc["route"].IsObject())
    {
        const Value &route = doc["route"];
        const std::string final_tag = memberString(route, "final");
        if(final_tag.empty())
        {
            error = "route.final is empty";
            return false;
        }
        if(!known.count(final_tag))
        {
            error = "route.final references unknown tag '" + final_tag + "'";
            return false;
        }
        if(route.HasMember("rule_set") && route["rule_set"].IsArray() && route.HasMember("rules") && route["rules"].IsArray())
        {
            std::set<std::string> rule_set_tags;
            for(const Value &entry : route["rule_set"].GetArray())
                rule_set_tags.insert(memberString(entry, "tag"));
            for(const Value &rule : route["rules"].GetArray())
            {
                std::vector<std::string> refs = stringArray(rule, "rule_set");
                for(const std::string &ref : refs)
                    if(!rule_set_tags.count(ref))
                    {
                        error = "route rule references undefined rule_set '" + ref + "'";
                        return false;
                    }
            }
        }
    }

    if(doc.HasMember("dns") && doc["dns"].IsObject())
    {
        const Value &dns = doc["dns"];
        std::set<std::string> server_tags;
        if(dns.HasMember("servers") && dns["servers"].IsArray())
            for(const Value &server : dns["servers"].GetArray())
                server_tags.insert(memberString(server, "tag"));
        if(server_tags.size() >= 2)
        {
            const bool has_route = doc.HasMember("route") && doc["route"].IsObject();
            const std::string resolver = has_route ? memberString(doc["route"], "default_domain_resolver") : "";
            if(resolver.empty())
            {
                error = "multiple dns servers require route.default_domain_resolver";
                return false;
            }
            if(!server_tags.count(resolver))
            {
                error = "route.default_domain_resolver references unknown dns server '" + resolver + "'";
                return false;
            }
        }
    }
    return true;
}

}
