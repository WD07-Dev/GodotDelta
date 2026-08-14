#include "core/common/include_rules.h"
#include "core/common/path_utils.h"
#include "pck_writer.h"
#include<algorithm>
#include<array>
#include<cstdint>
#include<fstream>
#include<regex>
#include<stdexcept>
#include<string>
#include<vector>
using namespace gddelta::pck;

namespace {
    inline constexpr const char *kProjectIncludeFileName = ".gddeltainclude";
    inline constexpr const char *kDefaultIncludeFileName = "default.gddeltainclude";
    inline constexpr const char *kDefaultIncludeFilePath = "build/default.gddeltainclude";

    struct PendingEntry {
        std::filesystem::path source_path;
        std::filesystem::path source_pack_path;
        std::uint64_t source_offset = 0;
        std::vector<std::uint8_t> inline_data;
        std::string pack_path;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        std::array<std::uint8_t, 16> md5 {};
        std::uint32_t flags = 0;
    };
    
    template <typename T>
    void write_scalar(std::ostream& stream, T value) {
        stream.write(reinterpret_cast<const char *>(&value), sizeof(T));
        if(!stream) {
            throw std::runtime_error("Failed to write scalar to PCK stream.");
        }
    }
    
    void write_padding(std::ostream& stream, std::uint64_t count) {
        static constexpr std::array<char, 32> zeroes {};
        while(count > 0) {
            const auto chunk = std::min<std::uint64_t>(count, zeroes.size());
            stream.write(zeroes.data(), static_cast<std::streamsize>(chunk));
            if(!stream) {
                throw std::runtime_error("Failed to write padding to PCK stream.");
            }
            count -= chunk;
        }
    }
    
    std::uint64_t file_size_checked(const std::filesystem::path& path) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if(ec) {
            throw std::runtime_error("Failed to stat file: " + path.string());
        }
        return size;
    }
    
    std::string normalize_pack_path(const std::filesystem::path& path) {
        return gddelta::common::normalize_pack_relative_path(path.generic_string());
    }

    std::vector<gddelta::common::IncludeRule> load_include_patterns(const std::filesystem::path &root) {
        const auto current_dir = std::filesystem::current_path();
        auto default_patterns = gddelta::common::load_include_patterns_from_file(current_dir / kDefaultIncludeFileName);
        if(default_patterns.empty()) {
            default_patterns = gddelta::common::load_include_patterns_from_file(current_dir / kDefaultIncludeFilePath);
        }
        auto patterns = std::move(default_patterns);
        const auto project_patterns = gddelta::common::load_include_patterns_from_file(root / kProjectIncludeFileName);
        patterns.insert(patterns.end(), project_patterns.begin(), project_patterns.end());
        return patterns;
    }

    bool should_skip_project_entry(const std::filesystem::path& relative_path) {
        if(relative_path.empty()) return false;

        const auto first = *relative_path.begin();
        if(first == ".git" || first == "bin") return true;
        const auto filename = relative_path.filename().generic_string();
        if(relative_path.parent_path().empty() &&
            !filename.empty() &&
            filename.front() == '.' &&
            filename != ".godot") {
            return true;
        }

        const auto normalized = relative_path.generic_string();
        if(normalized.ends_with(".tmp") || normalized.find(".tmp") != std::string::npos) {
            return true;
        }
        if(normalized.ends_with(".uid")) return true;

        for(const auto &part : relative_path) {
            if(part == "editor" || part == "shader_cache") {
                return true;
            }
        }

        return false;
    }
    std::vector<PendingEntry> collect_directory_entries(const std::filesystem::path& root) {
        std::vector<PendingEntry> entries;
        const auto include_patterns = load_include_patterns(root);
        for(auto it = std::filesystem::recursive_directory_iterator(root); it != std::filesystem::recursive_directory_iterator(); ++it) {
            const auto item = *it;
            const auto relative_path = std::filesystem::relative(item.path(), root);
            if(should_skip_project_entry(relative_path)) {
                if(item.is_directory() && relative_path != std::filesystem::path(".godot")) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if(!item.is_regular_file()) continue;

            const auto pack_path = normalize_pack_path(relative_path);
            if(!gddelta::common::matches_include_patterns(pack_path, include_patterns)) continue;
            
            PendingEntry entry;
            entry.source_path = item.path();
            entry.pack_path = pack_path;
            entry.size = file_size_checked(item.path());
            entries.push_back(std::move(entry));
        }

        std::sort(entries.begin(), entries.end(), [](const PendingEntry& lhs, const PendingEntry& rhs) {
            return lhs.pack_path < rhs.pack_path;
        });
        
        return entries;
    }
    
    std::vector<PendingEntry> collect_file_entries(const std::vector<PckWriteFile>& files) {
        std::vector<PendingEntry> entries;
        entries.reserve(files.size());
        
        for(const auto& file : files) {
            PendingEntry entry;
            entry.source_path = file.source_path;
            entry.source_pack_path = file.source_pack_path;
            entry.source_offset = file.source_offset;
            entry.inline_data = file.inline_data;
            entry.pack_path = normalize_pack_path(file.pack_path);
            entry.md5 = file.source_md5;
            entry.flags = file.source_flags | (file.removal ? kPackFileRemoval : 0);
            if(file.removal) {
                entry.size = 0;
            }else if(!entry.inline_data.empty()) {
                entry.size = static_cast<std::uint64_t>(entry.inline_data.size());
            }else if(!file.source_pack_path.empty()) {
                entry.size = file.source_size;
            }else {
                entry.size = file_size_checked(file.source_path);
            }
            entries.push_back(std::move(entry));
        }
        
        std::sort(entries.begin(), entries.end(), [](const PendingEntry& lhs, const PendingEntry& rhs) {
            return lhs.pack_path < rhs.pack_path;
        });
        return entries;
    }
    
    std::uint64_t compute_directory_size(const std::vector<PendingEntry>& entries, std::uint32_t format_version) {
        std::uint64_t size = sizeof(std::uint32_t);
        for(const auto& entry : entries) {
            const auto path_size = static_cast<std::uint32_t>(entry.pack_path.size());
            const auto stored_path_size = static_cast<std::uint32_t>(align_u64(path_size, 4));
            size += sizeof(std::uint32_t);
            size += stored_path_size;
            size += sizeof(std::uint64_t);
            size += sizeof(std::uint64_t);
            size += 16;
            if(format_version >= kPackFormatVersion2) {
                size += sizeof(std::uint32_t);
            }
        }
        return size;
    }
    
    void copy_file_bytes(const std::filesystem::path& source_path, std::ostream& output) {
        std::ifstream input(source_path, std::ios::binary);
        if(!input) {
            throw std::runtime_error("Failed to open source file: " + source_path.string());
        }
        
        std::array<char, 64 * 1024> buffer {};
        while(input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if(count <= 0) break;
            output.write(buffer.data(), count);
            if(!output) {
                throw std::runtime_error("Failed to write source file into PCK: " + source_path.string());
            }
        }
    }

    void copy_file_range_bytes(
        const std::filesystem::path& source_pack_path,
        std::uint64_t source_offset,
        std::uint64_t source_size,
        std::ostream &output
    ) {
        std::ifstream input(source_pack_path, std::ios::binary);
        if(!input) {
            throw std::runtime_error("Failed to open source pack file: " + source_pack_path.string());
        }

        input.seekg(static_cast<std::streamoff>(source_offset), std::ios::beg);
        std::array<char, 64 * 1024> buffer {};
        std::uint64_t remaining = source_size;

        while(remaining > 0) {
            const auto chunk = std::min<std::uint64_t>(remaining, buffer.size());
            input.read(buffer.data(), static_cast<std::streamsize>(chunk));
            const auto count = input.gcount();
            if(count <= 0) {
                throw std::runtime_error("Failed to read source range from pack: " + source_pack_path.string());
            }
            output.write(buffer.data(), count);
            if(!output) {
                throw std::runtime_error("Failed to write source range into PCK.");
            }
            remaining -= static_cast<std::uint64_t>(count);
        }
    }

    void write_inline_bytes(const std::vector<std::uint8_t>& bytes, std::ostream& output) {
        if(bytes.empty()) return;
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if(!output) {
            throw std::runtime_error("Failed to write inline file data into PCK.");
        }
    }
}

void PckWriter::write_from_directory(
    const std::filesystem::path& source_directory,
    const std::filesystem::path& output_pck,
    const PckWriteOptions& options
) {
    const auto entries = collect_directory_entries(source_directory);
    std::vector<PckWriteFile> files;
    files.reserve(entries.size());
    for(const auto& entry : entries) {
        files.push_back({ entry.source_path, {}, 0, 0, {}, 0, entry.pack_path, {}, false });
    }
    write_files(files, output_pck, options);
}


void PckWriter::write_files(
    const std::vector<PckWriteFile>& files,
    const std::filesystem::path& output_pck,
    const PckWriteOptions& options
) {
    auto entries = collect_file_entries(files);
    if(options.format_version == 1) {
        entries.erase(
            std::remove_if(entries.begin(), entries.end(), [](const PendingEntry& entry) {
                return (entry.flags & kPackFileRemoval) != 0;
            }),
            entries.end()
        );
    }

    if(entries.empty()) {
        throw std::runtime_error("Refusing to write an empty PCK.");
    }

    auto temp_path = output_pck;
    temp_path += ".tmp";
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
    if(!output) {
        throw std::runtime_error("Failed to open output PCK: " + output_pck.string());
    }

    write_scalar<std::uint32_t>(output, kPackHeaderMagic);
    write_scalar<std::uint32_t>(output, options.format_version);
    write_scalar<std::uint32_t>(output, options.engine_major);
    write_scalar<std::uint32_t>(output, options.engine_minor);
    write_scalar<std::uint32_t>(output, options.engine_patch);

    const auto pack_flags = options.format_version >= kPackFormatVersion2 ? kPackRelFileBase : 0U;
    std::streampos file_base_position = 0;
    std::streampos dir_offset_position = 0;

    if(options.format_version >= kPackFormatVersion2) {
        write_scalar<std::uint32_t>(output, pack_flags);
        file_base_position = output.tellp();
        write_scalar<std::uint64_t>(output, 0);
        if(options.format_version >= kPackFormatVersion3) {
            dir_offset_position = output.tellp();
            write_scalar<std::uint64_t>(output, 0);
        }
    }

    for(int i=0; i < 16; ++i) {
        write_scalar<std::uint32_t>(output, 0);
    }

    const auto pck_start = 0ULL;
    if(options.format_version >= kPackFormatVersion3) {
        const auto aligned_header_end = align_u64(static_cast<std::uint64_t>(output.tellp()), options.alignment);
        write_padding(output, aligned_header_end - static_cast<std::uint64_t>(output.tellp()));

        const auto files_start = static_cast<std::uint64_t>(output.tellp());
        const auto file_base = files_start - pck_start;

        for(std::size_t index = 0; index < entries.size(); ++index) {
            auto& entry = entries[index];
            entry.offset = static_cast<std::uint64_t>(output.tellp()) - files_start;
            const auto &source = entries[index];
            if((entry.flags & kPackFileRemoval) == 0) {
                if(!source.inline_data.empty()) {
                    write_inline_bytes(source.inline_data, output);
                }else if(!source.source_pack_path.empty()) {
                    copy_file_range_bytes(source.source_pack_path, source.source_offset, source.size, output);
                }else {
                    copy_file_bytes(source.source_path, output);
                }
            }
            const auto next_position = align_u64(static_cast<std::uint64_t>(output.tellp()), options.alignment);
            write_padding(output, next_position - static_cast<std::uint64_t>(output.tellp()));
        }

        const auto directory_offset = align_u64(static_cast<std::uint64_t>(output.tellp()), options.alignment);
        write_padding(output, directory_offset - static_cast<std::uint64_t>(output.tellp()));

        output.seekp(dir_offset_position);
        write_scalar<std::uint64_t>(output, directory_offset - pck_start);
        output.seekp(file_base_position);
        write_scalar<std::uint64_t>(output, file_base);
        output.seekp(static_cast<std::streamoff>(directory_offset), std::ios::beg);

        write_scalar<std::uint32_t>(output, static_cast<std::uint32_t>(entries.size()));
        for(const auto& entry : entries) {
            const auto raw_length = static_cast<std::uint32_t>(entry.pack_path.size());
            const auto stored_length = static_cast<std::uint32_t>(align_u64(raw_length, 4));
            write_scalar<std::uint32_t>(output, stored_length);
            output.write(entry.pack_path.data(), static_cast<std::streamsize>(entry.pack_path.size()));
            write_padding(output, stored_length - raw_length);
            write_scalar<std::uint64_t>(output, entry.offset);
            write_scalar<std::uint64_t>(output, entry.size);
            output.write(reinterpret_cast<const char *>(entry.md5.data()), static_cast<std::streamsize>(entry.md5.size()));
            write_scalar<std::uint32_t>(output, entry.flags);
        }
    }else if(options.format_version == kPackFormatVersion2) {
        std::vector<std::streampos> offset_positions;
        offset_positions.reserve(entries.size());

        write_scalar<std::uint32_t>(output, static_cast<std::uint32_t>(entries.size()));
        for(const auto& entry : entries) {
            const auto raw_length = static_cast<std::uint32_t>(entry.pack_path.size());
            const auto stored_length = static_cast<std::uint32_t>(align_u64(raw_length, 4));
            write_scalar<std::uint32_t>(output, stored_length);
            output.write(entry.pack_path.data(), static_cast<std::streamsize>(entry.pack_path.size()));
            write_padding(output, stored_length - raw_length);
            offset_positions.push_back(output.tellp());
            write_scalar<std::uint64_t>(output, 0);
            write_scalar<std::uint64_t>(output, entry.size);
            output.write(reinterpret_cast<const char *>(entry.md5.data()), static_cast<std::streamsize>(entry.md5.size()));
            write_scalar<std::uint32_t>(output, entry.flags);
        }

        const auto files_start = align_u64(static_cast<std::uint64_t>(output.tellp()), options.alignment);
        write_padding(output, files_start - static_cast<std::uint64_t>(output.tellp()));
        const auto file_base = files_start - pck_start;

        for(std::size_t index = 0; index < entries.size(); ++index) {
            auto& entry = entries[index];
            entry.offset = static_cast<std::uint64_t>(output.tellp()) - files_start;
            const auto &source = entries[index];
            if((entry.flags & kPackFileRemoval) == 0) {
                if(!source.inline_data.empty()) {
                    write_inline_bytes(source.inline_data, output);
                }else if(!source.source_pack_path.empty()) {
                    copy_file_range_bytes(source.source_pack_path, source.source_offset, source.size, output);
                }else {
                    copy_file_bytes(source.source_path, output);
                }
            }
            const auto next_position = align_u64(static_cast<std::uint64_t>(output.tellp()), options.alignment);
            write_padding(output, next_position - static_cast<std::uint64_t>(output.tellp()));
        }

        output.seekp(file_base_position);
        write_scalar<std::uint64_t>(output, file_base);
        for(std::size_t index = 0; index < entries.size(); ++index) {
            output.seekp(offset_positions[index]);
            write_scalar<std::uint64_t>(output, entries[index].offset);
        }
    }else if(options.format_version == 1) {
        write_scalar<std::uint32_t>(output, static_cast<std::uint32_t>(entries.size()));
        std::vector<std::streampos> offset_positions;
        offset_positions.reserve(entries.size());
        for(const auto& entry : entries) {
            const auto raw_length = static_cast<std::uint32_t>(entry.pack_path.size());
            const auto stored_length = static_cast<std::uint32_t>(align_u64(raw_length, 4));
            write_scalar<std::uint32_t>(output, stored_length);
            output.write(entry.pack_path.data(), static_cast<std::streamsize>(entry.pack_path.size()));
            write_padding(output, stored_length - raw_length);
            offset_positions.push_back(output.tellp());
            write_scalar<std::uint64_t>(output, 0);
            write_scalar<std::uint64_t>(output, entry.size);
            output.write(reinterpret_cast<const char *>(entry.md5.data()), static_cast<std::streamsize>(entry.md5.size()));
        }

        const auto files_start = align_u64(static_cast<std::uint64_t>(output.tellp()), options.alignment);
        write_padding(output, files_start - static_cast<std::uint64_t>(output.tellp()));

        for(std::size_t index = 0; index < entries.size(); ++index) {
            auto& entry = entries[index];
            entry.offset = static_cast<std::uint64_t>(output.tellp());
            const auto& source = entries[index];
            if(!source.inline_data.empty()) {
                write_inline_bytes(source.inline_data, output);
            }else if(!source.source_pack_path.empty()) {
                copy_file_range_bytes(source.source_pack_path, source.source_offset, source.size, output);
            }else {
                copy_file_bytes(source.source_path, output);
            }

            const auto next_position = align_u64(static_cast<std::uint64_t>(output.tellp()), options.alignment);
            write_padding(output, next_position - static_cast<std::uint64_t>(output.tellp()));
        }

        for(std::size_t index = 0; index < entries.size(); ++index) {
            output.seekp(offset_positions[index]);
            write_scalar<std::uint64_t>(output, entries[index].offset);
        }
    }else {
        throw std::runtime_error("Only PCK format v1/v2/v3/v4 writing is implemented right now.");
    }

    output.flush();
    output.close();

    std::error_code ec;
    std::filesystem::rename(temp_path, output_pck, ec);
    if(ec) {
        std::filesystem::remove(output_pck, ec);
        ec.clear();
        std::filesystem::rename(temp_path, output_pck, ec);
    }
    if(ec) {
        throw std::runtime_error("Failed to move generated PCK into place: " + output_pck.string());
    }
}