#include "path_utils.h"
#include<stdexcept>
using namespace gddelta::common;

namespace {
    bool has_windows_drive_prefix(const std::string& path) {
        return path.size() >= 2
            && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))
            && path[1] == ':';
    }
}

std::string gddelta::common::normalize_pack_relative_path(std::string_view path) {
    std::string normalized(path);
    for(auto& ch : normalized) {
        if(ch == '\\') {
            ch = '/';
        }
    }

    if(normalized.rfind("res://", 0) == 0) {
        normalized.erase(0, 6);
    }
    while(!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }
    while(!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }

    if(has_windows_drive_prefix(normalized)) {
        throw std::runtime_error("Pack path must not use an absolute drive path: " + normalized);
    }

    const std::filesystem::path normalized_path(normalized);
    if(normalized_path.is_absolute()) {
        throw std::runtime_error("Pack path must not be absolute: " + normalized);
    }
    if(!is_safe_pack_relative_path(normalized_path)) {
        throw std::runtime_error("Pack path must not contain traversal segments: " + normalized);
    }

    return normalized_path.generic_string();
}

bool gddelta::common::is_safe_pack_relative_path(const std::filesystem::path& path) {
    for(const auto& part : path) {
        if(part == "." || part == "..") {
            return false;
        }
    }
    return true;
}