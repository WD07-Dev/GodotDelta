#include "path_utils.h"
#include<stdexcept>
using namespace gddelta::common;

namespace {
    bool has_windows_drive_prefix(const std::string& path) {
        return path.size() >= 2
        && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':';
    }

    bool has_traversal_segment(std::string_view path) {
        std::size_t start = 0;
        while(start <= path.size()) {
            const auto end = path.find('/', start);
            const auto length = end == std::string_view::npos ? path.size() - start : end - start;
            const auto part = path.substr(start, length);
            if(part == "." || part == "..") return true;
            if(end == std::string_view::npos) break;
            start = end + 1;
        }
        return false;
    }
}

std::string gddelta::common::path_to_utf8(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::filesystem::path gddelta::common::path_from_utf8(std::string_view path) {
    const std::u8string utf8(path.begin(), path.end());
    return std::filesystem::path(utf8);
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

    if(!normalized.empty() && normalized.front() == '/') {
        throw std::runtime_error("Pack path must not be absolute: " + normalized);
    }
    if(has_traversal_segment(normalized)) {
        throw std::runtime_error("Pack path must not contain traversal segments: " + normalized);
    }

    return normalized;
}

bool gddelta::common::is_safe_pack_relative_path(const std::filesystem::path& path) {
    for(const auto& part : path) {
        if(part == "." || part == "..") return false;
    }
    return true;
}