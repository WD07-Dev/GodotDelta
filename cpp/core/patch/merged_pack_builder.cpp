#include "merged_pack_builder.h"
#include "core/pck/pck_embedded.h"
#include "core/pck/pck_reader.h"
#include "core/pck/pck_writer.h"
#include<map>
#include<string>
#include<utility>
#include<vector>
using namespace gddelta::patch;

void MergedPackBuilder::build_merged_pack(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& patch_pck,
    const std::filesystem::path& output_pck,
    const pck::PckWriteOptions& options
) const {
    pck::PckReader base_reader;
    base_reader.open(base_pck);

    pck::PckReader patch_reader;
    patch_reader.open(patch_pck);

    std::map<std::string, pck::PckWriteFile> merged_files;
    for(const auto& entry : base_reader.entries()) {
        if((entry.flags & pck::kPackFileRemoval) != 0) continue;

        pck::PckWriteFile file;
        file.pack_path = entry.path;
        file.source_pack_path = base_reader.path();
        file.source_offset = entry.offset;
        file.source_size = entry.size;
        file.source_md5 = entry.md5;
        file.source_flags = entry.flags & ~pck::kPackFileRemoval;
        merged_files.emplace(file.pack_path, std::move(file));
    }

    for(const auto& entry : patch_reader.entries()) {
        if((entry.flags & pck::kPackFileRemoval) != 0) {
            merged_files.erase(entry.path);
            continue;
        }

        pck::PckWriteFile file;
        file.pack_path = entry.path;
        file.source_pack_path = patch_reader.path();
        file.source_offset = entry.offset;
        file.source_size = entry.size;
        file.source_md5 = entry.md5;
        file.source_flags = entry.flags & ~pck::kPackFileRemoval;
        merged_files[file.pack_path] = std::move(file);
    }

    std::vector<pck::PckWriteFile> files;
    files.reserve(merged_files.size());
    for(auto &[path, file] : merged_files) {
        static_cast<void>(path);
        files.push_back(std::move(file));
    }

    auto effective_options = options;
    effective_options.format_version = base_reader.header().format_version;
    effective_options.engine_major = base_reader.header().engine_major;
    effective_options.engine_minor = base_reader.header().engine_minor;
    effective_options.engine_patch = base_reader.header().engine_patch;

    pck::PckWriter writer;
    const auto base_embedded = pck::EmbeddedPckHandler::find_embedded_pck(base_pck);
    const auto wants_executable_output = output_pck.extension() == ".exe";
    if(!base_embedded.has_value() || !wants_executable_output) {
        writer.write_files(files, output_pck, effective_options);
        return;
    }

    auto temp_output_pck = output_pck;
    temp_output_pck += ".pck.tmp";
    writer.write_files(files, temp_output_pck, effective_options);
    try {
        pck::EmbeddedPckHandler::embed_pck_into_executable(base_pck, temp_output_pck, output_pck);
        pck::EmbeddedPckHandler::fixup_embedded_executable_headers(base_pck, output_pck);
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(temp_output_pck, ec);
        throw;
    }

    std::error_code ec;
    std::filesystem::remove(temp_output_pck, ec);
}