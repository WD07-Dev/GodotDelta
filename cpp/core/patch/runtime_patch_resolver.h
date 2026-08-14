#pragma once

#include "core/common/include_rules.h"
#include "core/pck/pck_format.h"
#include "core/pck/pck_reader.h"
#include "core/workspace/workspace.h"
#include<cstdint>
#include<optional>
#include<regex>
#include<string>
#include<vector>
#include<filesystem>
#include<unordered_map>
namespace gddelta::patch {
    class RuntimePatchResolver {
        public:
            using IncludeRuleMode = gddelta::common::IncludeRuleMode;
            using IncludeRule = gddelta::common::IncludeRule;

            explicit RuntimePatchResolver(std::filesystem::path project_dir);
            void warn_if_runtime_is_stale() const;
            [[nodiscard]] std::vector<pck::PckWriteFile> collect_patch_files(
                const std::vector<std::string> &input_paths,
                bool legacy_simple = false
            ) const;
            [[nodiscard]] std::vector<std::string> collect_auto_input_paths(
                const pck::PckReader& base_reader
            ) const;
            [[nodiscard]] std::vector<std::string> collect_included_source_paths() const;
            [[nodiscard]] std::uint64_t calculate_watch_stamp() const;
            [[nodiscard]] static bool is_project_source_candidate(const std::filesystem::path& relative_path);
            [[nodiscard]] std::vector<std::string> collect_dirty_input_paths(const workspace::WorkspaceDiff& diff) const;

        private:
            struct TimestampedPath {
                std::filesystem::path path;
                std::filesystem::file_time_type time;
            };
            std::filesystem::path project_dir_;
            std::unordered_map<std::string, std::string> export_map_;
            std::unordered_map<std::string, std::string> reverse_export_map_;

            [[nodiscard]] static std::string normalize_project_relative_path(const std::string& path);
            [[nodiscard]] std::optional<TimestampedPath> find_newest_project_source() const;
            [[nodiscard]] std::optional<TimestampedPath> find_newest_export_marker() const;
            [[nodiscard]] static std::vector<IncludeRule> load_include_patterns(const std::filesystem::path& project_dir);
            [[nodiscard]] static bool matches_include_patterns(
                const std::string& path,
                const std::vector<IncludeRule>& patterns
            );
            [[nodiscard]] static bool matches_forced_include_patterns(
                const std::string& path,
                const std::vector<IncludeRule>& patterns
            );
            [[nodiscard]] static bool patch_file_differs_from_base(
                const pck::PckReader& base_reader,
                const pck::PckWriteFile& file
            );
            [[nodiscard]] std::vector<std::string> collect_text_resource_references(
                const std::string& relative_path
            ) const;
            [[nodiscard]] std::vector<std::string> collect_import_outputs(
                const std::string& relative_path
            ) const;
            [[nodiscard]] std::vector<std::string> collect_remap_outputs(
                const std::string& relative_path
            ) const;
            [[nodiscard]] static bool is_text_reference_source(const std::filesystem::path& path);
            [[nodiscard]] static std::unordered_map<std::string, std::string> load_export_file_cache(const std::filesystem::path& project_dir);
    };
}