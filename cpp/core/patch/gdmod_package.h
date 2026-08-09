#pragma once

#include "core/pck/pck_format.h"
#include<filesystem>
#include<string>
#include<vector>

namespace gddelta::patch {
    struct GdmodManifest {
        std::string base_file_name;
        std::string project_name;
        std::uint32_t format_version = 0;
        std::uint32_t engine_major = 0;
        std::uint32_t engine_minor = 0;
        std::uint32_t engine_patch = 0;
        std::size_t entry_count = 0;
    };

    class GdmodPackage {
        public:
            [[nodiscard]] static const char *manifest_pack_path() noexcept;

            void write(
                const std::filesystem::path& output_path,
                const std::vector<pck::PckWriteFile>& files,
                const pck::PckWriteOptions& options,
                const GdmodManifest& manifest
            ) const;

            [[nodiscard]] bool is_gdmod(const std::filesystem::path& input_path) const;
            [[nodiscard]] GdmodManifest read_manifest(const std::filesystem::path& input_path) const;
            void extract_patch_pck(
                const std::filesystem::path& input_path,
                const std::filesystem::path& output_path
            ) const;
    };
}
