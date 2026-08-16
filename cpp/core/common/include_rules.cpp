#include "include_rules.h"
#include "path_utils.h"
#include<fstream>
#include<stdexcept>
using namespace gddelta::common;

namespace {
    std::string unquote_include_value(std::string value) {
        value = gddelta::common::trim_copy(value);
        if(value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        return gddelta::common::trim_copy(value);
    }
}

std::string gddelta::common::trim_copy(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if(first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::regex gddelta::common::compile_include_pattern(const std::string& pattern) {
    std::string regex_pattern = "^";
    for(std::size_t i = 0; i < pattern.size(); ++i) {
        const char ch = pattern[i];
        if(ch == '*') {
            const bool is_double_star = i + 1 < pattern.size() && pattern[i + 1] == '*';
            if(is_double_star) {
                regex_pattern += ".*";
                ++i;
            }else {
                regex_pattern += "[^/]*";
            }
            continue;
        }

        switch(ch) {
            case '.':
            case '^':
            case '$':
            case '+':
            case '?':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '|':
            case '\\':
                regex_pattern += '\\';
                break;
            default:
                break;
        }
        regex_pattern += ch;
    }
    regex_pattern += "$";
    return std::regex(regex_pattern, std::regex::ECMAScript);
}

std::vector<IncludeRule> gddelta::common::load_include_patterns_from_file(const std::filesystem::path& include_path) {
    if(!std::filesystem::exists(include_path)) return {};

    std::ifstream input(include_path);
    if(!input) {
        throw std::runtime_error("Failed to open include file: " + include_path.string());
    }

    std::vector<IncludeRule> patterns;
    std::string line;
    while(std::getline(input, line)) {
        auto trimmed = trim_copy(line);
        if(trimmed.empty() || trimmed.front() == '#') continue;

        auto mode = IncludeRuleMode::Include;
        if(trimmed.front() == '+') {
            mode = IncludeRuleMode::ForceInclude;
            trimmed.erase(trimmed.begin());
            trimmed = trim_copy(trimmed);
        }else if(trimmed.front() == '!') {
            mode = IncludeRuleMode::Exclude;
            trimmed.erase(trimmed.begin());
            trimmed = trim_copy(trimmed);
        }

        std::optional<std::string> mapped_pack_path;
        auto source_pattern = trimmed;
        if(mode != IncludeRuleMode::Exclude) {
            const auto separator = trimmed.find('=');
            if(separator != std::string::npos) {
                auto mapped_target = unquote_include_value(trimmed.substr(0, separator));
                source_pattern = unquote_include_value(trimmed.substr(separator + 1));
                const auto normalized_target = normalize_pack_relative_path(mapped_target);
                if(!normalized_target.empty()) {
                    mapped_pack_path = normalized_target;
                }
            }
        }

        source_pattern = unquote_include_value(source_pattern);
        const auto normalized = normalize_pack_relative_path(source_pattern);
        if(normalized.empty()) continue;
        patterns.push_back(IncludeRule {
            compile_include_pattern(normalized),
            mode,
            mapped_pack_path
        });
    }
    return patterns;
}

std::vector<IncludeRule> gddelta::common::load_include_patterns(
    const std::filesystem::path& root,
    const std::filesystem::path& default_include_path,
    const std::filesystem::path& project_include_name
) {
    auto patterns = load_include_patterns_from_file(default_include_path);
    const auto project_patterns = load_include_patterns_from_file(root / project_include_name);
    patterns.insert(patterns.end(), project_patterns.begin(), project_patterns.end());
    return patterns;
}

bool gddelta::common::matches_include_patterns(
    const std::string& path,
    const std::vector<IncludeRule>& patterns
) {
    if(patterns.empty()) return true;

    auto matched_any = false;
    auto include_path = false;
    for(const auto& pattern : patterns) {
        if(!std::regex_match(path, pattern.pattern)) continue;
        matched_any = true;
        if(pattern.mode == IncludeRuleMode::Exclude) {
            include_path = false;
            continue;
        }
        include_path = true;
    }
    return matched_any && include_path;
}

bool gddelta::common::matches_forced_include_patterns(
    const std::string& path,
    const std::vector<IncludeRule>& patterns
) {
    auto matched_any = false;
    auto forced_include = false;
    for(const auto& pattern : patterns) {
        if(!std::regex_match(path, pattern.pattern)) continue;
        matched_any = true;
        if(pattern.mode == IncludeRuleMode::Exclude) {
            forced_include = false;
            continue;
        }
        if(pattern.mode == IncludeRuleMode::ForceInclude) {
            forced_include = true;
        }
    }
    return matched_any && forced_include;
}