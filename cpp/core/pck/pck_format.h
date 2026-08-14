#pragma once

#include<array>
#include<cstdint>
#include<filesystem>
#include<string>
#include<vector>
namespace gddelta::pck {
    inline constexpr std::uint32_t kPackHeaderMagic = 0x43504447; // "GDPC" in little-endian.
    inline constexpr std::uint32_t kPackFormatVersion2 = 2;
    inline constexpr std::uint32_t kPackFormatVersion3 = 3;
    inline constexpr std::uint32_t kPackFormatVersion4 = 4;
    inline constexpr std::uint32_t kCurrentPackFormatVersion = kPackFormatVersion4;
    inline constexpr std::uint32_t kPackAlignment = 32;
    
    inline constexpr std::uint32_t kPackDirEncrypted = 1U << 0;
    inline constexpr std::uint32_t kPackRelFileBase = 1U << 1;
    inline constexpr std::uint32_t kPackSparseBundle = 1U << 2;
    
    inline constexpr std::uint32_t kPackFileEncrypted = 1U << 0;
    inline constexpr std::uint32_t kPackFileRemoval = 1U << 1;
    inline constexpr std::uint32_t kPackFileDelta = 1U << 2;
    
    struct PckHeader {
        std::uint32_t format_version = 0;
        std::uint32_t engine_major = 0;
        std::uint32_t engine_minor = 0;
        std::uint32_t engine_patch = 0;
        std::uint32_t pack_flags = 0;
        std::uint64_t file_base = 0;
        std::uint64_t directory_offset = 0;
        std::uint64_t pck_start_offset = 0;
        std::uint32_t file_count = 0;
    };
    
    struct PckEntry {
        std::string path;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        std::array<std::uint8_t, 16> md5 {};
        std::uint32_t flags = 0;
    };
    
    struct PckWriteOptions {
        std::uint32_t format_version = kCurrentPackFormatVersion;
        std::uint32_t engine_major = 4;
        std::uint32_t engine_minor = 4;
        std::uint32_t engine_patch = 0;
        std::uint32_t alignment = kPackAlignment;
    };
    
    struct PckWriteFile {
        std::filesystem::path source_path;
        std::filesystem::path source_pack_path;
        std::uint64_t source_offset = 0;
        std::uint64_t source_size = 0;
        std::array<std::uint8_t, 16> source_md5 {};
        std::uint32_t source_flags = 0;
        std::string pack_path;
        std::vector<std::uint8_t> inline_data;
        bool removal = false;
    };
    
    [[nodiscard]] std::uint64_t align_u64(std::uint64_t value, std::uint64_t alignment);
}