#include "gdmod_package.h"
#include "core/pck/pck_reader.h"
#include "core/pck/pck_writer.h"
#include<sstream>
#include<stdexcept>
#include<string_view>

using namespace gddelta::patch;

namespace {
constexpr const char *kGdmodManifestPackPath = "res://.gddelta/gdmod.meta";

std::string build_manifest_text(const GdmodManifest& manifest) {
    std::ostringstream output;
    output
    << "type=gdmod\n"
    << "version=1\n"
    << "base_file_name=" << manifest.base_file_name << "\n"
    << "project_name=" << manifest.project_name << "\n"
    << "format_version=" << manifest.format_version << "\n"
    << "engine_major=" << manifest.engine_major << "\n"
    << "engine_minor=" << manifest.engine_minor << "\n"
    << "engine_patch=" << manifest.engine_patch << "\n"
    << "entry_count=" << manifest.entry_count << "\n";
    return output.str();
}

GdmodManifest parse_manifest_text(std::string_view text) {
    GdmodManifest manifest;
    std::istringstream input{std::string(text)};
    std::string line;
    std::string type;
    std::string version;

    while(std::getline(input, line)) {
        const auto delimiter = line.find('=');
        if(delimiter == std::string::npos) {
            continue;
        }

        const auto key = line.substr(0, delimiter);
        const auto value = line.substr(delimiter + 1);
        if(key == "type") {
            type = value;
            continue;
        }
        if(key == "version") {
            version = value;
            continue;
        }
        if(key == "base_file_name") {
            manifest.base_file_name = value;
            continue;
        }
        if(key == "project_name") {
            manifest.project_name = value;
            continue;
        }
        if(key == "format_version") {
            manifest.format_version = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "engine_major") {
            manifest.engine_major = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "engine_minor") {
            manifest.engine_minor = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "engine_patch") {
            manifest.engine_patch = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "entry_count") {
            manifest.entry_count = static_cast<std::size_t>(std::stoull(value));
            continue;
        }
    }

    if(type != "gdmod" || version != "1") {
        throw std::runtime_error("Invalid gdmod manifest.");
    }

    return manifest;
}
}

const char *GdmodPackage::manifest_pack_path() noexcept {
    return kGdmodManifestPackPath;
}

void GdmodPackage::write(
    const std::filesystem::path& output_path,
    const std::vector<pck::PckWriteFile>& files,
    const pck::PckWriteOptions& options,
    const GdmodManifest& manifest
) const {
    auto package_files = files;

    pck::PckWriteFile manifest_file;
    manifest_file.pack_path = manifest_pack_path();
    const auto manifest_text = build_manifest_text(manifest);
    manifest_file.inline_data.assign(manifest_text.begin(), manifest_text.end());
    package_files.push_back(std::move(manifest_file));

    pck::PckWriter writer;
    writer.write_files(package_files, output_path, options);
}

bool GdmodPackage::is_gdmod(const std::filesystem::path& input_path) const {
    pck::PckReader reader;
    reader.open(input_path);
    return reader.find_entry(manifest_pack_path()).has_value();
}

GdmodManifest GdmodPackage::read_manifest(const std::filesystem::path& input_path) const {
    pck::PckReader reader;
    reader.open(input_path);
    const auto manifest_entry = reader.find_entry(manifest_pack_path());
    if(!manifest_entry.has_value()) {
        throw std::runtime_error("Input is not a gdmod package: " + input_path.string());
    }

    const auto bytes = reader.read_entry_data(*manifest_entry);
    return parse_manifest_text(std::string_view(
        reinterpret_cast<const char *>(bytes.data()),
        bytes.size()
    ));
}

void GdmodPackage::extract_patch_pck(
    const std::filesystem::path& input_path,
    const std::filesystem::path& output_path
) const {
    pck::PckReader reader;
    reader.open(input_path);

    std::vector<pck::PckWriteFile> files;
    files.reserve(reader.entries().size());
    for(const auto& entry : reader.entries()) {
        if(entry.path == manifest_pack_path()) {
            continue;
        }

        pck::PckWriteFile file;
        file.pack_path = entry.path;
        file.removal = (entry.flags & pck::kPackFileRemoval) != 0;
        if(!file.removal) {
            file.source_pack_path = input_path;
            file.source_offset = entry.offset;
            file.source_size = entry.size;
        }
        files.push_back(std::move(file));
    }

    pck::PckWriteOptions options;
    options.format_version = reader.header().format_version;
    options.engine_major = reader.header().engine_major;
    options.engine_minor = reader.header().engine_minor;
    options.engine_patch = reader.header().engine_patch;

    pck::PckWriter writer;
    writer.write_files(files, output_path, options);
}
