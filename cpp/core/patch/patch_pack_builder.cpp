#include "patch_manifest.h"
#include "patch_pack_builder.h"
#include "core/pck/pck_writer.h"
using namespace gddelta::patch;

gddelta::workspace::WorkspaceDiff PatchPackBuilder::diff(
    const std::filesystem::path& base_dir,
    const std::filesystem::path& modified_dir
) const {
    const auto base_snapshot = workspace_.build(base_dir);
    const auto modified_snapshot = workspace_.build(modified_dir);
    return workspace_.diff(base_snapshot, modified_snapshot);
}

std::vector<gddelta::pck::PckWriteFile> PatchPackBuilder::build_file_list(
    const std::filesystem::path& base_dir,
    const std::filesystem::path& modified_dir
) const {
    const auto workspace_diff = diff(base_dir, modified_dir);

    std::vector<pck::PckWriteFile> files;
    files.reserve(workspace_diff.added.size() + workspace_diff.modified.size() + workspace_diff.removed.size());
    for(const auto& file : workspace_diff.added) {
        pck::PckWriteFile patch_file;
        patch_file.source_path = file.absolute_path;
        patch_file.pack_path = file.relative_path;
        files.push_back(std::move(patch_file));
    }
    for(const auto& file : workspace_diff.modified) {
        pck::PckWriteFile patch_file;
        patch_file.source_path = file.absolute_path;
        patch_file.pack_path = file.relative_path;
        files.push_back(std::move(patch_file));
    }
    for(const auto& relative_path : workspace_diff.removed) {
        pck::PckWriteFile patch_file;
        patch_file.pack_path = relative_path;
        patch_file.removal = true;
        files.push_back(std::move(patch_file));
    }

    return files;
}

void PatchPackBuilder::build_patch_pack(
    const std::filesystem::path& base_dir,
    const std::filesystem::path& modified_dir,
    const std::filesystem::path& output_pck,
    const pck::PckWriteOptions& options
) const {
    const auto workspace_diff = diff(base_dir, modified_dir);
    pck::PckWriter writer;
    writer.write_files(build_file_list(base_dir, modified_dir), output_pck, options);

    PatchManifestWriter manifest_writer;
    auto manifest_path = output_pck;
    manifest_path += ".manifest.txt";
    manifest_writer.write(workspace_diff, base_dir, modified_dir, manifest_path);
}