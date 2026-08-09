#include "workspace.h"
#include<array>
#include<fstream>
#include<stdexcept>
using namespace gddelta::workspace;

namespace {
    constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    
    std::string normalize_relative_path(const std::filesystem::path& path) {
        auto normalized = path.generic_string();
        while(!normalized.empty() && normalized.front() == '/') {
            normalized.erase(normalized.begin());
        }
        return normalized;
    }
    
    std::uint64_t modified_time_to_u64(const std::filesystem::file_time_type& time) {
        return static_cast<std::uint64_t>(time.time_since_epoch().count());
    }
    
    std::uint64_t hash_file_contents(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if(!input) {
            throw std::runtime_error("Failed to open file for hashing: " + path.string());
        }
        
        std::array<char, 64 * 1024> buffer {};
        std::uint64_t hash = kFnvOffsetBasis;
        
        while(input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            for (std::streamsize index = 0; index < count; ++index) {
                hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
                hash *= kFnvPrime;
            }
        }
        
        return hash;
    }
}

WorkspaceSnapshot Workspace::build(const std::filesystem::path& root) const {
    std::error_code ec;
    if(!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        throw std::runtime_error("Workspace root is not a directory: " + root.string());
    }

    WorkspaceSnapshot snapshot;
    snapshot.root = root;

    for(const auto entry : std::filesystem::recursive_directory_iterator(root)) {
        if(!entry.is_regular_file()) continue;

        WorkspaceFile file;
        file.absolute_path = entry.path();
        file.relative_path = normalize_relative_path(std::filesystem::relative(entry.path(), root));
        file.size = entry.file_size();
        file.modified_time = modified_time_to_u64(entry.last_write_time());
        file.content_hash = hash_file_contents(entry.path());
        snapshot.files.emplace(file.relative_path, std::move(file));
    }

    return snapshot;
}