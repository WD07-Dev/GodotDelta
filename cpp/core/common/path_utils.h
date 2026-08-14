#pragma once

#include<filesystem>
#include<string>
#include<string_view>
namespace gddelta::common {
    [[nodiscard]] std::filesystem::path path_from_utf8(std::string_view path);
    [[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);
    [[nodiscard]] std::string normalize_pack_relative_path(std::string_view path);
    [[nodiscard]] bool is_safe_pack_relative_path(const std::filesystem::path& path);
}