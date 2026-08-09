#pragma once

#include "core/pck/pck_format.h"
#include "core/pck/pck_reader.h"
#include "core/pck/pck_writer.h"
#include "core/workspace/workspace.h"
#include<cstdint>
#include<filesystem>
#include<functional>
#include<optional>
#include<string>
#include<vector>
namespace cli_internal {
    struct BaseInputPaths {
        std::filesystem::path requested_path;
        std::filesystem::path pack_path;
        bool uses_external_pack = false;
    };

    class CliSupport {
        private:
            std::filesystem::path cli_path_;

        public:
            void set_cli_path(std::filesystem::path cli_path);
            void launch_ui(const std::filesystem::path& cli_path) const;
            std::filesystem::path resolve_cli_directory(const std::filesystem::path& cli_path) const;
            std::filesystem::path resolve_tools_directory(const std::filesystem::path& cli_path) const;
            void ensure_gdre_tools(const std::filesystem::path& cli_path);
            [[nodiscard]] std::filesystem::path resolve_gdre_tools_path() const;
            [[nodiscard]] std::string run_gdre_tools_command(const std::vector<std::string>& args) const;
            [[nodiscard]] std::string detect_base_engine_version(const std::filesystem::path& base_pck) const;
            void compile_gdscript_files(
                const std::filesystem::path& base_pck,
                const std::vector<std::filesystem::path>& source_files,
                const std::filesystem::path& output_dir
            ) const;
            void compose_pck_from_project_files(
                const std::filesystem::path& base_pck,
                const std::vector<gddelta::pck::PckWriteFile>& files,
                const std::filesystem::path& output_path
            ) const;
            BaseInputPaths resolve_base_input(const std::filesystem::path& base_path) const;
            std::filesystem::path create_temporary_base_copy(const std::filesystem::path& base_pck) const;
            gddelta::pck::PckReader open_supported_base_pack(const std::filesystem::path& base_pck) const;
            gddelta::pck::PckWriteOptions build_pack_options_from_base(const std::filesystem::path& base_pck) const;
            void copy_runtime_support_files(
                const std::filesystem::path& base_path,
                const std::filesystem::path& sandbox_dir
            ) const;
            std::filesystem::path create_cleanup_patch(
                const std::filesystem::path& sandbox_dir,
                const gddelta::pck::PckWriteOptions& options
            ) const;
            void copy_base_into_sandbox(
                const std::filesystem::path& base_pck,
                const std::filesystem::path& sandbox_dir
            ) const;
            void watch_workspace_diff(
                const std::filesystem::path& project_dir,
                std::uint64_t interval_ms,
                const std::function<void(const gddelta::workspace::WorkspaceDiff &)>& on_diff
            ) const;
            void watch_stamp(
                std::uint64_t initial_stamp,
                std::uint64_t interval_ms,
                const std::function<std::uint64_t()>& stamp_provider,
                const std::function<void()>& on_change
            ) const;
            void print_rebuild_paths(
                const std::string& label,
                const std::vector<std::string>& paths
            ) const;
    };
}