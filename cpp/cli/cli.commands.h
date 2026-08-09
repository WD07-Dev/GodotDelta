#pragma once

#include<cstdint>
#include<filesystem>
#include<optional>
#include<vector>

namespace cli_internal {
    class CliSupport;
}

class CliCommands {
    private:
        cli_internal::CliSupport& support_;

    public:
        explicit CliCommands(cli_internal::CliSupport& support);

        void inspect_pack(const std::filesystem::path& path);
        void diff_workspace(const std::filesystem::path& base_dir, const std::filesystem::path& modified_dir);
        void build_patch_pack(
            const std::filesystem::path& base_dir,
            const std::filesystem::path& modified_dir,
            const std::filesystem::path& output_pck
        );
        void build_patch_pck_from_dirs(
            const std::filesystem::path& base_pck,
            const std::filesystem::path& base_dir,
            const std::filesystem::path& modified_dir,
            const std::filesystem::path& output_pck
        );
        void trace_runtime_paths(
            const std::filesystem::path& project_dir,
            const std::vector<std::string>& input_paths
        );
        void build_patch_pck_from_inputs(
            const std::filesystem::path& base_pck,
            const std::filesystem::path& project_dir,
            const std::filesystem::path& output_pck,
            const std::vector<std::string>& input_paths
        );
        void build_patch_pck_auto(
            const std::filesystem::path& base_pck,
            const std::filesystem::path& project_dir,
            const std::filesystem::path& output_pck
        );
        void build_gdmod(
            const std::filesystem::path& base_pck,
            const std::filesystem::path& project_dir,
            const std::filesystem::path& output_path
        );
        void watch_patch_pck(
            const std::filesystem::path& base_pck,
            const std::filesystem::path& project_dir,
            const std::filesystem::path& output_pck,
            std::uint64_t interval_ms,
            const std::optional<std::filesystem::path>& log_file_path = std::nullopt
        );
        void build_dev_sandbox(
            const std::filesystem::path& base_pck,
            const std::filesystem::path& project_dir,
            const std::filesystem::path& sandbox_dir
        );
        void build_dev_sandbox_from_pck(
            const std::filesystem::path& base_pck,
            const std::filesystem::path& patch_pck,
            const std::filesystem::path& sandbox_dir
        );
        void apply_pck_in_place(
            const std::filesystem::path& base_pck,
            const std::filesystem::path& patch_pck
        );
        void apply_gdmod(
            const std::filesystem::path& base_pck,
            const std::filesystem::path& gdmod_path,
            const std::optional<std::filesystem::path>& sandbox_dir
        );
        void watch_dev_sandbox_from_patch_pck(
            const std::filesystem::path& base_pck,
            const std::filesystem::path& project_dir,
            const std::filesystem::path& patch_pck,
            const std::filesystem::path& sandbox_dir,
            std::uint64_t interval_ms,
            const std::optional<std::filesystem::path>& log_file_path = std::nullopt
        );
        void watch_dev_sandbox(
            const std::filesystem::path& base_pck,
            const std::filesystem::path& project_dir,
            const std::filesystem::path& sandbox_dir,
            std::uint64_t interval_ms,
            const std::optional<std::filesystem::path>& log_file_path = std::nullopt
        );
        void compose_pck(
            const std::filesystem::path& base_pck,
            const std::filesystem::path& patch_pck,
            const std::filesystem::path& output_pck
        );
};
