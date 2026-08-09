#include "cli.commands.h"
#include "cli.shared.h"
#include "core/patch/gdmod_package.h"
#include "core/patch/patch_pack_builder.h"
#include "core/patch/runtime_patch_resolver.h"
#include "core/pck/pck_writer.h"
#include<filesystem>
#include<fstream>
#include<iostream>
#include<optional>

namespace {
    class ScopedLogRedirect {
        public:
            explicit ScopedLogRedirect(const std::optional<std::filesystem::path>& log_file_path) {
                if(!log_file_path.has_value()) return;
                std::filesystem::create_directories(log_file_path->parent_path());
                stream_.open(*log_file_path, std::ios::out | std::ios::trunc);
                if(!stream_) {
                    throw std::runtime_error("Failed to open watch log file: " + log_file_path->string());
                }

                old_cout_ = std::cout.rdbuf(stream_.rdbuf());
                old_cerr_ = std::cerr.rdbuf(stream_.rdbuf());
                old_cout_flags_ = std::cout.flags();
                old_cerr_flags_ = std::cerr.flags();
                std::cout.setf(std::ios::unitbuf);
                std::cerr.setf(std::ios::unitbuf);
            }

            ~ScopedLogRedirect() {
                std::cout.flush();
                std::cerr.flush();
                if(old_cout_ != nullptr) {
                    std::cout.rdbuf(old_cout_);
                    std::cout.flags(old_cout_flags_);
                }
                if(old_cerr_ != nullptr) {
                    std::cerr.rdbuf(old_cerr_);
                    std::cerr.flags(old_cerr_flags_);
                }
            }

        private:
            std::ofstream stream_;
            std::streambuf* old_cout_ = nullptr;
            std::streambuf* old_cerr_ = nullptr;
            std::ios::fmtflags old_cout_flags_{};
            std::ios::fmtflags old_cerr_flags_{};
    };
}

void CliCommands::build_patch_pck_from_dirs(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& base_dir,
    const std::filesystem::path& modified_dir,
    const std::filesystem::path& output_pck
) {
    gddelta::patch::PatchPackBuilder builder;
    const auto files = builder.build_file_list(base_dir, modified_dir);
    builder.build_patch_pack(base_dir, modified_dir, output_pck, support_.build_pack_options_from_base(base_pck));

    std::cout
    << "Created runtime patch PCK " << output_pck
    << " with " << files.size() << " changed entries"
    << " for base " << base_pck << "\n";
}

void CliCommands::trace_runtime_paths(
    const std::filesystem::path& project_dir,
    const std::vector<std::string>& input_paths
) {
    const gddelta::patch::RuntimePatchResolver resolver(project_dir);
    for(const auto& file : resolver.collect_patch_files(input_paths)) {
        std::cout << file.pack_path << "\n";
    }
}

void CliCommands::build_patch_pck_from_inputs(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& project_dir,
    const std::filesystem::path& output_pck,
    const std::vector<std::string>& input_paths
) {
    const auto options = support_.build_pack_options_from_base(base_pck);
    auto prepared = support_.prepare_runtime_patch_files(base_pck, project_dir, input_paths, true);
    if(prepared.files.empty()) {
        throw std::runtime_error("No runtime-related files were found for the requested paths.");
    }

    gddelta::pck::PckWriter writer;
    writer.write_files(prepared.files, output_pck, options);

    std::cout
    << "Created runtime patch PCK " << output_pck
    << " with " << prepared.files.size() << " runtime-related entries\n";
}

void CliCommands::build_patch_pck_auto(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& project_dir,
    const std::filesystem::path& output_pck
) {
    const auto temp_base = support_.create_temporary_base_copy(base_pck);
    std::vector<std::string> input_paths;
    {
        gddelta::pck::PckReader base_reader;
        base_reader.open(temp_base);
        const gddelta::patch::RuntimePatchResolver resolver(project_dir);
        input_paths = resolver.collect_auto_input_paths(base_reader);
    }
    std::error_code ec;
    std::filesystem::remove(temp_base, ec);
    if(input_paths.empty()) {
        throw std::runtime_error("No changed files were found in the included project scope.");
    }

    support_.print_rebuild_paths("Auto-detected patch inputs", input_paths);
    build_patch_pck_from_inputs(base_pck, project_dir, output_pck, input_paths);
}

void CliCommands::build_gdmod(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& project_dir,
    const std::filesystem::path& output_path
) {
    const auto resolved_base = support_.resolve_base_input(base_pck);
    const auto options = support_.build_pack_options_from_base(base_pck);
    const gddelta::patch::RuntimePatchResolver resolver(project_dir);
    const auto base_reader = support_.open_supported_base_pack(base_pck);
    auto input_paths = resolver.collect_auto_input_paths(base_reader);
    if(input_paths.empty()) {
        throw std::runtime_error("No changed files were found in the included project scope.");
    }

    support_.print_rebuild_paths("Auto-detected patch inputs", input_paths);
    auto prepared = support_.prepare_runtime_patch_files(base_pck, project_dir, input_paths, true);
    std::size_t threshold_chunk_slot_count = 0;

    if(prepared.files.empty()) {
        throw std::runtime_error("No runtime-related files were found for the requested paths.");
    }

    gddelta::patch::GdmodManifest manifest;
    manifest.base_file_name = resolved_base.pack_path.filename().string();
    manifest.project_name = project_dir.filename().string();
    manifest.format_version = options.format_version;
    manifest.engine_major = options.engine_major;
    manifest.engine_minor = options.engine_minor;
    manifest.engine_patch = options.engine_patch;
    manifest.entry_count = prepared.files.size();
    manifest.threshold_chunks = gddelta::patch::GdmodPackage::build_default_threshold_chunk_binding(resolved_base.pack_path);
    threshold_chunk_slot_count = manifest.threshold_chunks.slots.size();

    gddelta::patch::GdmodPackage package;
    package.write(resolved_base.pack_path, output_path, prepared.files, options, manifest);

    std::cout
    << "Created gdmod " << output_path
    << " with " << prepared.files.size() << " runtime-related entries"
    << " and " << threshold_chunk_slot_count << " threshold chunk slots\n";
}

void CliCommands::watch_patch_pck(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& project_dir,
    const std::filesystem::path& output_pck,
    std::uint64_t interval_ms,
    const std::optional<std::filesystem::path>& log_file_path
) {
    const ScopedLogRedirect log_redirect(log_file_path);
    const gddelta::patch::RuntimePatchResolver resolver(project_dir);

    std::cout
    << "Watching runtime patch inputs in " << project_dir
    << " -> " << output_pck
    << " (" << interval_ms << " ms)\n";
    
    support_.watch_workspace_diff(project_dir, interval_ms, [&](const gddelta::workspace::WorkspaceDiff& diff) {
        const auto dirty_paths = resolver.collect_dirty_input_paths(diff);
        if(dirty_paths.empty()) return;

        std::cout << "Change detected, rebuilding runtime patch: " << output_pck << "\n";
        support_.print_rebuild_paths("Runtime patch inputs", dirty_paths);
        build_patch_pck_from_inputs(base_pck, project_dir, output_pck, dirty_paths);
    });
}
