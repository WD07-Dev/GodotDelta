#pragma once

#include "core/pck/pck_format.h"
#include<filesystem>
#include<cstdint>
#include<string>
#include<vector>

namespace gddelta::patch {
    struct GdmodThresholdChunkSlot {
        std::string chunk_id;
        std::string source_path;
        std::uint64_t source_offset = 0;
        std::uint64_t size = 0;
        std::string share_pack_path;
    };

    struct GdmodThresholdChunkBinding {
        std::string chunking = "fastcdc";
        std::uint32_t sample_count = 96;
        std::uint32_t threshold = 20;
        std::uint32_t chunk_min_size = 32 * 1024;
        std::uint32_t chunk_avg_size = 64 * 1024;
        std::uint32_t chunk_max_size = 128 * 1024;
        std::uint32_t max_samples_per_file = 3;
        bool use_pck_source = true;
        bool use_executable_source = true;
        std::vector<GdmodThresholdChunkSlot> slots;
    };

    struct GdmodManifest {
        std::string base_file_name;
        std::string project_name;
        std::uint32_t format_version = 0;
        std::uint32_t engine_major = 0;
        std::uint32_t engine_minor = 0;
        std::uint32_t engine_patch = 0;
        std::size_t entry_count = 0;
        std::string payload_pack_path;
        GdmodThresholdChunkBinding threshold_chunks;
        bool legacy_plain_payload = false;
    };

    class GdmodPackage {
        public:
            [[nodiscard]] static const char *manifest_pack_path() noexcept;
            [[nodiscard]] static GdmodThresholdChunkBinding build_default_threshold_chunk_binding(
                const std::filesystem::path& base_pck
            );

            void write(
                const std::filesystem::path& base_pck,
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
            void extract_protected_patch_pck(
                const std::filesystem::path& base_pck,
                const std::filesystem::path& input_path,
                const std::filesystem::path& output_path
            ) const;
    };
}