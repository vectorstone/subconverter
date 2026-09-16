#include <algorithm>
#include <map>
#include <string>

#include "handler/settings.h"
#include "utils/logger.h"
#include "utils/network.h"
#include "utils/regexp.h"
#include "utils/string.h"
#include "utils/rapidjson_extra.h"
#include "subexport.h"

/// rule type lists
#define basic_types "DOMAIN", "DOMAIN-SUFFIX", "DOMAIN-KEYWORD", "IP-CIDR", "SRC-IP-CIDR", "GEOIP", "MATCH", "FINAL"
string_array ClashRuleTypes = {basic_types, "IP-CIDR6", "SRC-PORT", "DST-PORT", "PROCESS-NAME"};
string_array Surge2RuleTypes = {basic_types, "IP-CIDR6", "USER-AGENT", "URL-REGEX", "PROCESS-NAME", "IN-PORT", "DEST-PORT", "SRC-IP"};
string_array SurgeRuleTypes = {basic_types, "IP-CIDR6", "USER-AGENT", "URL-REGEX", "AND", "OR", "NOT", "PROCESS-NAME", "IN-PORT", "DEST-PORT", "SRC-IP"};
string_array QuanXRuleTypes = {basic_types, "USER-AGENT", "HOST", "HOST-SUFFIX", "HOST-KEYWORD"};
string_array SurfRuleTypes = {basic_types, "IP-CIDR6", "PROCESS-NAME", "IN-PORT", "DEST-PORT", "SRC-IP"};
string_array SingBoxRuleTypes = {basic_types, "IP-VERSION", "INBOUND", "PROTOCOL", "NETWORK", "GEOSITE", "SRC-GEOIP", "DOMAIN-REGEX", "PROCESS-NAME", "PROCESS-PATH", "PACKAGE-NAME", "PORT", "PORT-RANGE", "SRC-PORT", "SRC-PORT-RANGE", "USER", "USER-ID"};

std::string convertRuleset(const std::string &content, int type)
{
    /// Target: Surge type,pattern[,flag]
    /// Source: QuanX type,pattern[,group]
    ///         Clash payload:\n  - 'ipcidr/domain/classic(Surge-like)'

    std::string output, strLine;

    if(type == RULESET_SURGE)
        return content;

    if(regFind(content, "^payload:\\r?\\n")) /// Clash
    {
        output = regReplace(regReplace(content, "payload:\\r?\\n", "", true), R"(\s?^\s*-\s+('|"?)(.*)\1$)", "\n$2", true);
        if(type == RULESET_CLASH_CLASSICAL) /// classical type
            return output;
        std::stringstream ss;
        ss << output;
        char delimiter = getLineBreak(output);
        output.clear();
        string_size pos, lineSize;
        while(getline(ss, strLine, delimiter))
        {
            strLine = trim(strLine);
            lineSize = strLine.size();
            if(lineSize && strLine[lineSize - 1] == '\r') //remove line break
                strLine.erase(--lineSize);

            if(strFind(strLine, "//"))
            {
                strLine.erase(strLine.find("//"));
                strLine = trimWhitespace(strLine);
            }

            if(!strLine.empty() && (strLine[0] != ';' && strLine[0] != '#' && !(lineSize >= 2 && strLine[0] == '/' && strLine[1] == '/')))
            {
                pos = strLine.find('/');
                if(pos != std::string::npos) /// ipcidr
                {
                    if(isIPv4(strLine.substr(0, pos)))
                        output += "IP-CIDR,";
                    else
                        output += "IP-CIDR6,";
                }
                else
                {
                    if(strLine[0] == '.' || (lineSize >= 2 && strLine[0] == '+' && strLine[1] == '.')) /// suffix
                    {
                        bool keyword_flag = false;
                        while(endsWith(strLine, ".*"))
                        {
                            keyword_flag = true;
                            strLine.erase(strLine.size() - 2);
                        }
                        output += "DOMAIN-";
                        if(keyword_flag)
                            output += "KEYWORD,";
                        else
                            output += "SUFFIX,";
                        strLine.erase(0, 2 - (strLine[0] == '.'));
                    }
                    else
                        output += "DOMAIN,";
                }
            }
            output += strLine;
            output += '\n';
        }
        return output;
    }
    else /// QuanX
    {
        output = regReplace(regReplace(content, "^(?i:host)", "DOMAIN", true), "^(?i:ip6-cidr)", "IP-CIDR6", true); //translate type
        output = regReplace(output, "^((?i:DOMAIN(?:-(?:SUFFIX|KEYWORD))?|IP-CIDR6?|USER-AGENT),)\\s*?(\\S*?)(?:,(?!no-resolve).*?)(,no-resolve)?$", "\\U$1\\E$2${3:-}", true); //remove group
        return output;
    }
}

static std::string transformRuleToCommon(string_view_array &temp, const std::string &input, const std::string &group, bool no_resolve_only = false)
{
    temp.clear();
    std::string strLine;
    split(temp, input, ',');
    if(temp.size() < 2)
    {
        strLine = temp[0];
        strLine += ",";
        strLine += group;
    }
    else
    {
        strLine = temp[0];
        strLine += ",";
        strLine += temp[1];
        strLine += ",";
        strLine += group;
        if(temp.size() > 2 && (!no_resolve_only || temp[2] == "no-resolve"))
        {
            strLine += ",";
            strLine += temp[2];
        }
    }
    return strLine;
}

void rulesetToClash(YAML::Node &base_rule, std::vector<RulesetContent> &ruleset_content_array, bool overwrite_original_rules, bool new_field_name)
{
    string_array allRules;
    std::string rule_group, retrieved_rules, strLine;
    std::stringstream strStrm;
    const std::string field_name = new_field_name ? "rules" : "Rule";
    YAML::Node rules;
    size_t total_rules = 0;

    if(!overwrite_original_rules && base_rule[field_name].IsDefined())
        rules = base_rule[field_name];

    std::vector<std::string_view> temp(4);
    for(RulesetContent &x : ruleset_content_array)
    {
        if(global.maxAllowedRules && total_rules > global.maxAllowedRules)
            break;
        rule_group = x.rule_group;
        retrieved_rules = x.rule_content.get();
        if(retrieved_rules.empty())
        {
            writeLog(0, "Failed to fetch ruleset or ruleset is empty: '" + x.rule_path + "'!", LOG_LEVEL_WARNING);
            continue;
        }
        if(startsWith(retrieved_rules, "[]"))
        {
            strLine = retrieved_rules.substr(2);
            if(startsWith(strLine, "FINAL"))
                strLine.replace(0, 5, "MATCH");
            strLine = transformRuleToCommon(temp, strLine, rule_group);
            allRules.emplace_back(strLine);
            total_rules++;
            continue;
        }
        retrieved_rules = convertRuleset(retrieved_rules, x.rule_type);
        char delimiter = getLineBreak(retrieved_rules);

        strStrm.clear();
        strStrm<<retrieved_rules;
        std::string::size_type lineSize;
        while(getline(strStrm, strLine, delimiter))
        {
            if(global.maxAllowedRules && total_rules > global.maxAllowedRules)
                break;
            strLine = trimWhitespace(strLine, true, true); //remove whitespaces
            lineSize = strLine.size();
            if(!lineSize || strLine[0] == ';' || strLine[0] == '#' || (lineSize >= 2 && strLine[0] == '/' && strLine[1] == '/')) //empty lines and comments are ignored
                continue;
            if(std::none_of(ClashRuleTypes.begin(), ClashRuleTypes.end(), [strLine](const std::string& type){return startsWith(strLine, type);}))
                continue;
            if(strFind(strLine, "//"))
            {
                strLine.erase(strLine.find("//"));
                strLine = trimWhitespace(strLine);
            }
            strLine = transformRuleToCommon(temp, strLine, rule_group);
            allRules.emplace_back(strLine);
        }
    }

    for(std::string &x : allRules)
    {
        rules.push_back(x);
    }

    base_rule[field_name] = rules;
}

std::string rulesetToClashStr(YAML::Node &base_rule, std::vector<RulesetContent> &ruleset_content_array, bool overwrite_original_rules, bool new_field_name)
{
    std::string rule_group, retrieved_rules, strLine;
    std::stringstream strStrm;
    const std::string field_name = new_field_name ? "rules" : "Rule";
    std::string output_content = "\n" + field_name + ":\n";
    size_t total_rules = 0;

    if(!overwrite_original_rules && base_rule[field_name].IsDefined())
    {
        for(size_t i = 0; i < base_rule[field_name].size(); i++)
            output_content += "  - " + safe_as<std::string>(base_rule[field_name][i]) + "\n";
    }
    base_rule.remove(field_name);

    string_view_array temp(4);
    for(RulesetContent &x : ruleset_content_array)
    {
        if(global.maxAllowedRules && total_rules > global.maxAllowedRules)
            break;
        rule_group = x.rule_group;
        retrieved_rules = x.rule_content.get();
        if(retrieved_rules.empty())
        {
            writeLog(0, "Failed to fetch ruleset or ruleset is empty: '" + x.rule_path + "'!", LOG_LEVEL_WARNING);
            continue;
        }
        if(startsWith(retrieved_rules, "[]"))
        {
            strLine = retrieved_rules.substr(2);
            if(startsWith(strLine, "FINAL"))
                strLine.replace(0, 5, "MATCH");
            strLine = transformRuleToCommon(temp, strLine, rule_group);
            output_content += "  - " + strLine + "\n";
            total_rules++;
            continue;
        }
        retrieved_rules = convertRuleset(retrieved_rules, x.rule_type);
        char delimiter = getLineBreak(retrieved_rules);

        strStrm.clear();
        strStrm<<retrieved_rules;
        std::string::size_type lineSize;
        while(getline(strStrm, strLine, delimiter))
        {
            if(global.maxAllowedRules && total_rules > global.maxAllowedRules)
                break;
            strLine = trimWhitespace(strLine, true, true); //remove whitespaces
            lineSize = strLine.size();
            if(!lineSize || strLine[0] == ';' || strLine[0] == '#' || (lineSize >= 2 && strLine[0] == '/' && strLine[1] == '/')) //empty lines and comments are ignored
                continue;
            if(std::none_of(ClashRuleTypes.begin(), ClashRuleTypes.end(), [strLine](const std::string& type){ return startsWith(strLine, type); }))
                continue;
            if(strFind(strLine, "//"))
            {
                strLine.erase(strLine.find("//"));
                strLine = trimWhitespace(strLine);
            }
            strLine = transformRuleToCommon(temp, strLine, rule_group);
            output_content += "  - " + strLine + "\n";
            total_rules++;
        }
    }
    return output_content;
}

void rulesetToSurge(INIReader &base_rule, std::vector<RulesetContent> &ruleset_content_array, int surge_ver, bool overwrite_original_rules, const std::string &remote_path_prefix)
{
    string_array allRules;
    std::string rule_group, rule_path, rule_path_typed, retrieved_rules, strLine;
    std::stringstream strStrm;
    size_t total_rules = 0;

    switch(surge_ver) //other version: -3 for Surfboard, -4 for Loon
    {
    case 0:
        base_rule.set_current_section("RoutingRule"); //Mellow
        break;
    case -1:
        base_rule.set_current_section("filter_local"); //Quantumult X
        break;
    case -2:
        base_rule.set_current_section("TCP"); //Quantumult
        break;
    default:
        base_rule.set_current_section("Rule");
    }

    if(overwrite_original_rules)
    {
        base_rule.erase_section();
        switch(surge_ver)
        {
        case -1:
            base_rule.erase_section("filter_remote");
            break;
        case -4:
            base_rule.erase_section("Remote Rule");
            break;
        default:
            break;
        }
    }

    const std::string rule_match_regex = "^(.*?,.*?)(,.*)(,.*)$";

    string_view_array temp(4);
    for(RulesetContent &x : ruleset_content_array)
    {
        if(global.maxAllowedRules && total_rules > global.maxAllowedRules)
            break;
        rule_group = x.rule_group;
        rule_path = x.rule_path;
        rule_path_typed = x.rule_path_typed;
        if(rule_path.empty())
        {
            strLine = x.rule_content.get().substr(2);
            if(strLine == "MATCH")
                strLine = "FINAL";
            if(surge_ver == -1 || surge_ver == -2)
            {
                strLine = transformRuleToCommon(temp, strLine, rule_group, true);
            }
            else
            {
                if(!startsWith(strLine, "AND") && !startsWith(strLine, "OR") && !startsWith(strLine, "NOT"))
                    strLine = transformRuleToCommon(temp, strLine, rule_group);
            }
            strLine = replaceAllDistinct(strLine, ",,", ",");
            allRules.emplace_back(strLine);
            total_rules++;
            continue;
        }
        else
        {
            if(surge_ver == -1 && x.rule_type == RULESET_QUANX && isLink(rule_path))
            {
                strLine = rule_path + ", tag=" + rule_group + ", force-policy=" + rule_group + ", enabled=true";
                base_rule.set("filter_remote", "{NONAME}", strLine);
                continue;
            }
            if(fileExist(rule_path))
            {
                if(surge_ver > 2 && !remote_path_prefix.empty())
                {
                    strLine = "RULE-SET," + remote_path_prefix + "/getruleset?type=1&url=" + urlSafeBase64Encode(rule_path_typed) + "," + rule_group;
                    if(x.update_interval)
                        strLine += ",update-interval=" + std::to_string(x.update_interval);
                    allRules.emplace_back(strLine);
                    continue;
                }
                else if(surge_ver == -1 && !remote_path_prefix.empty())
                {
                    strLine = remote_path_prefix + "/getruleset?type=2&url=" + urlSafeBase64Encode(rule_path_typed) + "&group=" + urlSafeBase64Encode(rule_group);
                    strLine += ", tag=" + rule_group + ", enabled=true";
                    base_rule.set("filter_remote", "{NONAME}", strLine);
                    continue;
                }
                else if(surge_ver == -4 && !remote_path_prefix.empty())
                {
                    strLine = remote_path_prefix + "/getruleset?type=1&url=" + urlSafeBase64Encode(rule_path_typed) + "," + rule_group;
                    base_rule.set("Remote Rule", "{NONAME}", strLine);
                    continue;
                }
            }
            else if(isLink(rule_path))
            {
                if(surge_ver > 2)
                {
                    if(x.rule_type != RULESET_SURGE)
                    {
                        if(!remote_path_prefix.empty())
                            strLine = "RULE-SET," + remote_path_prefix + "/getruleset?type=1&url=" + urlSafeBase64Encode(rule_path_typed) + "," + rule_group;
                        else
                            continue;
                    }
                    else
                        strLine = "RULE-SET," + rule_path + "," + rule_group;

                    if(x.update_interval)
                        strLine += ",update-interval=" + std::to_string(x.update_interval);

                    allRules.emplace_back(strLine);
                    continue;
                }
                else if(surge_ver == -1 && !remote_path_prefix.empty())
                {
                    strLine = remote_path_prefix + "/getruleset?type=2&url=" + urlSafeBase64Encode(rule_path_typed) + "&group=" + urlSafeBase64Encode(rule_group);
                    strLine += ", tag=" + rule_group + ", enabled=true";
                    base_rule.set("filter_remote", "{NONAME}", strLine);
                    continue;
                }
                else if(surge_ver == -4)
                {
                    strLine = rule_path + "," + rule_group;
                    base_rule.set("Remote Rule", "{NONAME}", strLine);
                    continue;
                }
            }
            else
                continue;
            retrieved_rules = x.rule_content.get();
            if(retrieved_rules.empty())
            {
                writeLog(0, "Failed to fetch ruleset or ruleset is empty: '" + x.rule_path + "'!", LOG_LEVEL_WARNING);
                continue;
            }

            retrieved_rules = convertRuleset(retrieved_rules, x.rule_type);
            char delimiter = getLineBreak(retrieved_rules);

            strStrm.clear();
            strStrm<<retrieved_rules;
            std::string::size_type lineSize;
            while(getline(strStrm, strLine, delimiter))
            {
                if(global.maxAllowedRules && total_rules > global.maxAllowedRules)
                    break;
                strLine = trimWhitespace(strLine, true, true);
                lineSize = strLine.size();
                if(!lineSize || strLine[0] == ';' || strLine[0] == '#' || (lineSize >= 2 && strLine[0] == '/' && strLine[1] == '/')) //empty lines and comments are ignored
                    continue;

                /// remove unsupported types
                switch(surge_ver)
                {
                case -2:
                    if(startsWith(strLine, "IP-CIDR6"))
                        continue;
                    [[fallthrough]];
                case -1:
                    if(!std::any_of(QuanXRuleTypes.begin(), QuanXRuleTypes.end(), [strLine](const std::string& type){return startsWith(strLine, type);}))
                        continue;
                    break;
                case -3:
                    if(!std::any_of(SurfRuleTypes.begin(), SurfRuleTypes.end(), [strLine](const std::string& type){return startsWith(strLine, type);}))
                        continue;
                    break;
                default:
                    if(surge_ver > 2)
                    {
                        if(!std::any_of(SurgeRuleTypes.begin(), SurgeRuleTypes.end(), [strLine](const std::string& type){return startsWith(strLine, type);}))
                            continue;
                    }
                    else
                    {
                        if(!std::any_of(Surge2RuleTypes.begin(), Surge2RuleTypes.end(), [strLine](const std::string& type){return startsWith(strLine, type);}))
                            continue;
                    }
                }

                if(strFind(strLine, "//"))
                {
                    strLine.erase(strLine.find("//"));
                    strLine = trimWhitespace(strLine);
                }

                if(surge_ver == -1 || surge_ver == -2)
                {
                    if(startsWith(strLine, "IP-CIDR6"))
                        strLine.replace(0, 8, "IP6-CIDR");
                    strLine = transformRuleToCommon(temp, strLine, rule_group, true);
                }
                else
                {
                    if(!startsWith(strLine, "AND") && !startsWith(strLine, "OR") && !startsWith(strLine, "NOT"))
                        strLine = transformRuleToCommon(temp, strLine, rule_group);
                }
                allRules.emplace_back(strLine);
                total_rules++;
            }
        }
    }

    for(std::string &x : allRules)
    {
        base_rule.set("{NONAME}", x);
    }
}

static void registerRuleSet(std::vector<singbox::RuleSetSpec> &rule_sets, const std::string &tag, bool geoip)
{
    singbox::RuleSetSpec spec;
    spec.tag = tag;
    spec.geoip = geoip;
    if(std::find(rule_sets.begin(), rule_sets.end(), spec) == rule_sets.end())
        rule_sets.push_back(spec);
}

static bool isRejectGroup(const std::string &group)
{
    return toUpper(trim(group)) == "REJECT";
}

/// Map preference group names onto the minimal skeleton groups. Only active in
/// skeleton mode; custom base configurations keep their original references.
static std::string remapSingBoxGroup(const std::string &group, const singbox::Settings &settings)
{
    if (!settings.skeleton)
        return group;
    if (group == settings.proxy_tag || group == settings.direct_tag || group == settings.reject_tag
        || group == "GLOBAL" || group == "auto" || group == "ChainProxyEntry" || group == "ChainProxyExit")
        return group;
    if (group == "🔰 节点选择" || group == "♻️ 自动选择" || group == "🎥 NETFLIX" || group == "🌍 国外媒体"
        || group == "📲 电报信息" || group == "🍎 苹果服务" || group == "🐟 漏网之鱼")
        return settings.proxy_tag;
    if (group == "🎯 全球直连" || group == "🌏 国内媒体" || group == "Ⓜ️ 微软服务")
        return settings.direct_tag;
    if (group == "🛑 全球拦截" || group == "⛔️ 广告拦截" || group == "🚫 运营劫持")
        return settings.reject_tag;
    return settings.proxy_tag;
}

/// Map a bundled Surge list onto sing-geosite / sing-geoip rule-set tags so the
/// generated configuration references remote .srs files instead of inlining
/// thousands of domains. Keyed on the lowercased basename without extension.
/// Returns false when no mapping exists (the ruleset stays inline).
static bool mapSingBoxRuleSet(const std::string &rule_path, std::vector<singbox::RuleSetSpec> &specs)
{
    static const std::map<std::string, std::vector<singbox::RuleSetSpec>> table = {
        {"localareanetwork", {{"geosite-private"}}},
        {"msservices", {{"geosite-microsoft"}}},
        {"adrule", {{"geosite-category-ads-all"}}},
        {"hijacking", {{"geosite-category-ads-all"}}},
        {"netflix", {{"geosite-netflix"}}},
        {"streaming", {{"geosite-youtube"}, {"geosite-netflix"}, {"geosite-disney"}, {"geosite-spotify"}, {"geosite-hbo"}}},
        {"bilibili", {{"geosite-bilibili"}}},
        {"iqiyi", {{"geosite-iqiyi"}}},
        {"youku", {{"geosite-youku"}}},
        {"tencentvideo", {{"geosite-tencent"}}},
        {"letv", {{"geosite-cn"}}},
        {"moo", {{"geosite-cn"}}},
        {"global", {{"geosite-geolocation-!cn"}}},
        {"apple", {{"geosite-apple"}}},
        {"china", {{"geosite-cn"}}},
    };

    std::string name = rule_path;
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);
    name = toLower(replaceAllDistinct(trim(name), " ", ""));
    if (name.empty())
        return false;
    auto it = table.find(name);
    if (it == table.end())
        return false;
    specs = it->second;
    return true;
}

static void applySingBoxTarget(rapidjson::Value &rule_obj, const std::string &group, rapidjson::MemoryPoolAllocator<> &allocator, const singbox::Settings &settings)
{
    using namespace rapidjson_ext;
    const std::string target = remapSingBoxGroup(group, settings);
    if(isRejectGroup(target))
    {
        rule_obj | AddMemberOrReplace("action", rapidjson::Value("reject", allocator), allocator);
        return;
    }
    rule_obj | AddMemberOrReplace("action", rapidjson::Value("route", allocator), allocator);
    rule_obj | AddMemberOrReplace("outbound", rapidjson::Value(target.c_str(), allocator), allocator);
}

static rapidjson::Value transformRuleToSingBox(std::vector<std::string_view> &args, const std::string& rule, const std::string &group, rapidjson::MemoryPoolAllocator<>& allocator, std::vector<singbox::RuleSetSpec> &rule_sets, bool &is_final, std::string &final_group, const singbox::Settings &settings)
{
    using namespace rapidjson_ext;
    args.clear();
    split(args, rule, ',');
    if (args.size() < 2) return rapidjson::Value(rapidjson::kObjectType);
    const std::string raw_type = trim(std::string(args[0]));
    const std::string value = trim(std::string(args[1]));
//    std::string_view option;
//    if (args.size() >= 3) option = args[2];

    const std::string upper_type = toUpper(raw_type);
    if (upper_type == "MATCH" || upper_type == "FINAL")
    {
        is_final = true;
        final_group = (!value.empty() && !isRejectGroup(value)) ? value : group;
        final_group = remapSingBoxGroup(final_group, settings);
        return rapidjson::Value(rapidjson::kObjectType);
    }

    rapidjson::Value rule_obj(rapidjson::kObjectType);

    std::string rule_set_tag;
    bool geoip = false;
    if (singbox::ruleTypeToRuleSetTag(upper_type, value, rule_set_tag, geoip))
    {
        rapidjson::Value refs(rapidjson::kArrayType);
        refs.PushBack(rapidjson::Value(rule_set_tag.c_str(), allocator), allocator);
        rule_obj.AddMember("rule_set", refs, allocator);
        registerRuleSet(rule_sets, rule_set_tag, geoip);
    }
    else
    {
        std::string key = toLower(raw_type);
        key = replaceAllDistinct(key, "-", "_");
        key = replaceAllDistinct(key, "ip_cidr6", "ip_cidr");
        key = replaceAllDistinct(key, "src_", "source_");
        /// Rule values are kept verbatim: process_name / process_path / package_name /
        /// domain_regex are case sensitive.
        rule_obj.AddMember(rapidjson::Value(key.c_str(), allocator), rapidjson::Value(value.c_str(), allocator), allocator);
    }
    applySingBoxTarget(rule_obj, group, allocator, settings);
    return rule_obj;
}

static void appendSingBoxRule(std::vector<std::string_view> &args, rapidjson::Value &rules, const std::string& rule, rapidjson::MemoryPoolAllocator<>& allocator, std::vector<singbox::RuleSetSpec> &rule_sets)
{
    using namespace rapidjson_ext;
    args.clear();
    split(args, rule, ',');
    if (args.size() < 2) return;
    const std::string raw_type = trim(std::string(args[0]));
    const std::string value = trim(std::string(args[1]));
//    std::string_view option;
//    if (args.size() >= 3) option = args[2];

    if (none_of(SingBoxRuleTypes, [&](const std::string& t){ return raw_type == t; }))
        return;

    std::string rule_set_tag;
    bool geoip = false;
    if (singbox::ruleTypeToRuleSetTag(raw_type, value, rule_set_tag, geoip))
    {
        rules | AppendToArray("rule_set", rapidjson::Value(rule_set_tag.c_str(), allocator), allocator);
        registerRuleSet(rule_sets, rule_set_tag, geoip);
        return;
    }

    auto realType = toLower(raw_type);
    realType = replaceAllDistinct(realType, "-", "_");
    realType = replaceAllDistinct(realType, "ip_cidr6", "ip_cidr");

    rules | AppendToArray(realType.c_str(), rapidjson::Value(value.c_str(), allocator), allocator);
}

void rulesetToSingBox(rapidjson::Document &base_rule, std::vector<RulesetContent> &ruleset_content_array, bool overwrite_original_rules, const singbox::Settings &settings, std::vector<singbox::RuleSetSpec> &rule_sets)
{
    using namespace rapidjson_ext;
    std::string rule_group, retrieved_rules, strLine, final;
    std::stringstream strStrm;
    size_t total_rules = 0;
    auto &allocator = base_rule.GetAllocator();

    rapidjson::Value rules(rapidjson::kArrayType);
    if (!overwrite_original_rules && base_rule.HasMember("route") && base_rule["route"].IsObject() && base_rule["route"].HasMember("rules") && base_rule["route"]["rules"].IsArray())
        rules.Swap(base_rule["route"]["rules"]);

    std::vector<std::string_view> temp(4);
    for(RulesetContent &x : ruleset_content_array)
    {
        if(global.maxAllowedRules && total_rules > global.maxAllowedRules)
            break;
        rule_group = x.rule_group;
        retrieved_rules = x.rule_content.get();

        /// In skeleton mode, bundled Surge lists that map onto sing-geosite /
        /// sing-geoip categories become remote rule_set references instead of
        /// being inlined into route.rules.
        if(settings.skeleton && !retrieved_rules.empty() && !startsWith(retrieved_rules, "[]"))
        {
            std::vector<singbox::RuleSetSpec> mapped_specs;
            if(mapSingBoxRuleSet(x.rule_path, mapped_specs))
            {
                rapidjson::Value rule(rapidjson::kObjectType);
                rapidjson::Value refs(rapidjson::kArrayType);
                for(const singbox::RuleSetSpec &spec : mapped_specs)
                {
                    refs.PushBack(rapidjson::Value(spec.tag.c_str(), allocator), allocator);
                    registerRuleSet(rule_sets, spec.tag, spec.geoip);
                }
                rule.AddMember("rule_set", refs, allocator);
                applySingBoxTarget(rule, rule_group, allocator, settings);
                if(!rule.ObjectEmpty())
                {
                    rules.PushBack(rule, allocator);
                    total_rules++;
                }
                continue;
            }
        }

        if(retrieved_rules.empty())
        {
            writeLog(0, "Failed to fetch ruleset or ruleset is empty: '" + x.rule_path + "'!", LOG_LEVEL_WARNING);
            continue;
        }
        if(startsWith(retrieved_rules, "[]"))
        {
            strLine = retrieved_rules.substr(2);
            bool is_final = false;
            std::string final_group;
            rapidjson::Value rule_obj = transformRuleToSingBox(temp, strLine, rule_group, allocator, rule_sets, is_final, final_group, settings);
            if(is_final)
            {
                final = final_group;
                continue;
            }
            if(rule_obj.ObjectEmpty())
                continue;
            rules.PushBack(rule_obj, allocator);
            total_rules++;
            continue;
        }
        retrieved_rules = convertRuleset(retrieved_rules, x.rule_type);
        char delimiter = getLineBreak(retrieved_rules);

        strStrm.clear();
        strStrm<<retrieved_rules;

        std::string::size_type lineSize;
        rapidjson::Value rule(rapidjson::kObjectType);

        while(getline(strStrm, strLine, delimiter))
        {
            if(global.maxAllowedRules && total_rules > global.maxAllowedRules)
                break;
            strLine = trimWhitespace(strLine, true, true); //remove whitespaces
            lineSize = strLine.size();
            if(!lineSize || strLine[0] == ';' || strLine[0] == '#' || (lineSize >= 2 && strLine[0] == '/' && strLine[1] == '/')) //empty lines and comments are ignored
                continue;
            if(strFind(strLine, "//"))
            {
                strLine.erase(strLine.find("//"));
                strLine = trimWhitespace(strLine);
            }
            appendSingBoxRule(temp, rule, strLine, allocator, rule_sets);
        }
        if (rule.ObjectEmpty()) continue;
        applySingBoxTarget(rule, rule_group, allocator, settings);
        rules.PushBack(rule, allocator);
    }

    if (!base_rule.HasMember("route"))
        base_rule.AddMember("route", rapidjson::Value(rapidjson::kObjectType), allocator);

    base_rule["route"] | AddMemberOrReplace("rules", rules, allocator);

    if(!final.empty())
        base_rule["route"] | AddMemberOrReplace("final", rapidjson::Value(final.c_str(), allocator), allocator);
    else
    {
        const bool has_final = base_rule["route"].HasMember("final") && base_rule["route"]["final"].IsString() && base_rule["route"]["final"].GetStringLength() > 0;
        if(!has_final && !settings.proxy_tag.empty())
            base_rule["route"] | AddMemberOrReplace("final", rapidjson::Value(settings.proxy_tag.c_str(), allocator), allocator);
    }
}
