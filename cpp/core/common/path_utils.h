#pragma once

#include<filesystem>
#include<string>
#include<string_view>
namespace gddelta::common {
    [[nodiscard]] std::string normalize_pack_relative_path(std::string_view path);
    [[nodiscard]] bool is_safe_pack_relative_path(const std::filesystem::path& path);
}