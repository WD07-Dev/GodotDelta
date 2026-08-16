#pragma once

#include<filesystem>
#include<optional>
#include<regex>
#include<string>
#include<vector>
namespace gddelta::common {
    enum class IncludeRuleMode {
        Include,
        ForceInclude,
        Exclude,
    };

    struct IncludeRule {
        std::regex pattern;
        IncludeRuleMode mode = IncludeRuleMode::Include;
        std::optional<std::string> mapped_pack_path;
    };

    [[nodiscard]] std::string trim_copy(const std::string& value);
    [[nodiscard]] std::regex compile_include_pattern(const std::string& pattern);
    [[nodiscard]] std::vector<IncludeRule> load_include_patterns_from_file(const std::filesystem::path& include_path);
    [[nodiscard]] std::vector<IncludeRule> load_include_patterns(
        const std::filesystem::path& root,
        const std::filesystem::path& default_include_path,
        const std::filesystem::path& project_include_name
    );
    [[nodiscard]] bool matches_include_patterns(
        const std::string& path,
        const std::vector<IncludeRule>& patterns
    );
    [[nodiscard]] bool matches_forced_include_patterns(
        const std::string& path,
        const std::vector<IncludeRule>& patterns
    );
}