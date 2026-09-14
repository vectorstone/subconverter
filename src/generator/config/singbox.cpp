#include <algorithm>
#include <cctype>
#include <functional>
#include <set>

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
 * hijacking) from the tun address, e.g. "172.19.0.1/30" -> "172.19.0.2".
 */
std::string tunGateway(const std::string &address)
{
    const std::string ip = address.substr(0, address.find('/'));
    const std::size_t dot = ip.rfind('.');
    if(dot == std::string::npos)
        return ip;
    const std::string last = ip.substr(dot + 1);
    if(last.empty() || !std::all_of(last.begin(), last.end(), [](unsigned char c) { return std::isdigit(c); }))
        return ip;
    return ip.substr(0, dot + 1) + std::to_string(std::stoi(last) + 1);
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

const Profile &profileOf(Platform platform)
{
    static const Profile macos_profile = {
        true, true, 9000, "mixed", true, true, false, false, false, false,
        true, true, true, "", false, "warn", true, "prefer_ipv4"
    };
    static const Profile windows_profile = {
        true, true, 9000, "mixed", true, true, false, false, false, false,
        true, true, true, "", false, "warn", true, "prefer_ipv4"
    };
    static const Profile linux_profile = {
        true, true, 9000, "mixed", true, true, false, false, false, false,
        true, true, true, "", false, "warn", true, "prefer_ipv4"
    };
    static const Profile android_profile = {
        false, true, 8500, "mixed", false, false, false, true, false, true,
        false, false, false, "", false, "warn", true, "prefer_ipv4"
    };
    static const Profile ios_profile = {
        false, true, 8500, "system", false, false, false, true, false, false,
        false, false, false, "", false, "warn", true, "prefer_ipv4"
    };
    static const Profile openwrt_profile = {
        false, true, 9000, "mixed", true, true, true, false, true, false,
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
            tun.AddMember("stack", makeString(profile.tun_stack, allocator), allocator);
            if(profile.tun_auto_redirect)
                tun.AddMember("auto_redirect", true, allocator);
            if(profile.tun_dns_hijack)
            {
                tun.AddMember("dns_mode", makeString("hijack", allocator), allocator);
                std::vector<std::string> dns_address{tunGateway(settings.tun_address)};
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
    const Profile &profile = profileOf(settings.platform);
    const bool local_rulesets = settings.ruleset_source < 0 ? profile.local_rulesets : settings.ruleset_source > 0;
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
        }
        array.PushBack(entry, allocator);
    }
    doc["route"] | AddMemberOrReplace("rule_set", array, allocator);
}

void finalize(Document &doc, const Settings &settings, std::vector<std::string> &warnings)
{
    sanitizeGroups(doc, settings.direct_tag, warnings);
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
