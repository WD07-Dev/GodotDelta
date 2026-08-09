#include "cli.commands.h"
#include "cli.shared.h"
#include "core/patch/gdmod_package.h"
#include "core/patch/merged_pack_builder.h"
#include "core/patch/runtime_patch_resolver.h"
#include "core/pck/pck_embedded.h"
#include "core/pck/pck_reader.h"
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

    std::vector<std::string> collect_project_source_inputs(const std::filesystem::path& project_dir) {
        std::vector<std::string> input_paths;
        for(const auto& entry : std::filesystem::recursive_directory_iterator(project_dir)) {
            if(!entry.is_regular_file()) continue;
            const auto relative_path = std::filesystem::relative(entry.path(), project_dir);
            if(!gddelta::patch::RuntimePatchResolver::is_project_source_candidate(relative_path)) {
                continue;
            }

            input_paths.push_back(relative_path.generic_string());
        }
        return input_paths;
    }
}

void CliCommands::compose_pck(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& patch_pck,
    const std::filesystem::path& output_pck
) {
    const auto resolved_base = support_.resolve_base_input(base_pck);
    if(std::filesystem::is_directory(patch_pck)) {
        gddelta::patch::RuntimePatchResolver(patch_pck).warn_if_runtime_is_stale();
        const gddelta::patch::RuntimePatchResolver resolver(patch_pck);
        const auto input_paths = collect_project_source_inputs(patch_pck);
        const auto files = resolver.collect_patch_files(input_paths);
        support_.compose_pck_from_project_files(base_pck, files, output_pck);
    } else {
        const auto options = support_.build_pack_options_from_base(base_pck);
        const auto temp_base = support_.create_temporary_base_copy(base_pck);
        gddelta::patch::MergedPackBuilder builder;
        try {
            builder.build_merged_pack(temp_base, patch_pck, output_pck, options);
        } catch (...) {
            std::error_code ec;
            std::filesystem::remove(temp_base, ec);
            throw;
        }

        std::error_code ec;
        std::filesystem::remove(temp_base, ec);
    }

    if(resolved_base.pack_path.extension() == ".exe" && output_pck.extension() == ".exe") {
        gddelta::pck::EmbeddedPckHandler::fixup_embedded_executable_headers(resolved_base.pack_path, output_pck);
    }

    std::cout
    << "Created merged output " << output_pck
    << " from base " << base_pck
    << " and patch " << patch_pck << "\n";
}

void CliCommands::build_dev_sandbox(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& project_dir,
    const std::filesystem::path& sandbox_dir
) {
    const auto resolved_base = support_.resolve_base_input(base_pck);
    std::error_code ec;
    std::filesystem::create_directories(sandbox_dir, ec);
    if(ec) {
        throw std::runtime_error("Failed to create sandbox directory: " + sandbox_dir.string());
    }

    const auto sandbox_output = sandbox_dir / resolved_base.pack_path.filename();
    const auto runtime_patch_output = sandbox_dir / (sandbox_output.filename().string() + ".devbuild.runtime_patch.tmp.pck");

    try {
        build_patch_pck_auto(base_pck, project_dir, runtime_patch_output);
        compose_pck(base_pck, runtime_patch_output, sandbox_output);
    } catch (...) {
        std::error_code remove_error;
        std::filesystem::remove(runtime_patch_output, remove_error);
        throw;
    }

    std::error_code cleanup_error;
    std::filesystem::remove(runtime_patch_output, cleanup_error);
    support_.copy_runtime_support_files(resolved_base.pack_path, sandbox_dir);

    std::cout
    << "Prepared dev sandbox " << sandbox_dir
    << " from base " << base_pck
    << " and project " << project_dir << "\n";
}

void CliCommands::build_dev_sandbox_from_pck(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& patch_pck,
    const std::filesystem::path& sandbox_dir
) {
    const auto resolved_base = support_.resolve_base_input(base_pck);
    std::error_code ec;
    std::filesystem::create_directories(sandbox_dir, ec);
    if(ec) {
        throw std::runtime_error("Failed to create sandbox directory: " + sandbox_dir.string());
    }

    const auto sandbox_output = sandbox_dir / resolved_base.pack_path.filename();
    compose_pck(base_pck, patch_pck, sandbox_output);
    support_.copy_runtime_support_files(resolved_base.pack_path, sandbox_dir);

    std::cout
    << "Prepared dev sandbox " << sandbox_dir
    << " from base " << base_pck
    << " and patch " << patch_pck << "\n";
}

void CliCommands::apply_pck_in_place(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& patch_pck
) {
    const auto resolved_base = support_.resolve_base_input(base_pck);
    const auto temp_output = resolved_base.pack_path.parent_path() / (resolved_base.pack_path.stem().string() + ".apply.tmp" + resolved_base.pack_path.extension().string());
    compose_pck(base_pck, patch_pck, temp_output);

    std::error_code ec;
    std::filesystem::remove(resolved_base.pack_path, ec);
    ec.clear();
    std::filesystem::rename(temp_output, resolved_base.pack_path, ec);
    if(ec) {
        std::filesystem::remove(temp_output, ec);
        throw std::runtime_error("Failed to replace base file with applied patch: " + resolved_base.pack_path.string());
    }

    std::cout
    << "Applied patch " << patch_pck
    << " into base " << base_pck << "\n";
}

void CliCommands::apply_gdmod(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& gdmod_path,
    const std::optional<std::filesystem::path>& sandbox_dir
) {
    gddelta::patch::GdmodPackage package;
    const auto manifest = package.read_manifest(gdmod_path);
    const auto resolved_base = support_.resolve_base_input(base_pck);
    const auto temp_patch_path = resolved_base.pack_path.parent_path() / (gdmod_path.stem().string() + ".apply.tmp.pck");

    std::cout
    << "Applying gdmod " << gdmod_path
    << " built for " << manifest.base_file_name
    << " (" << manifest.engine_major << "." << manifest.engine_minor << "." << manifest.engine_patch << ")\n";

    if(manifest.base_file_name != resolved_base.pack_path.filename().string()) {
        std::cerr
        << "Warning: gdmod target base is " << manifest.base_file_name
        << ", but requested base is " << resolved_base.pack_path.filename().string() << "\n";
    }

    if(manifest.legacy_plain_payload) {
        package.extract_patch_pck(gdmod_path, temp_patch_path);
    } else {
        package.extract_protected_patch_pck(base_pck, gdmod_path, temp_patch_path);
    }
    try {
        if(sandbox_dir.has_value()) {
            build_dev_sandbox_from_pck(base_pck, temp_patch_path, *sandbox_dir);
        } else {
            apply_pck_in_place(base_pck, temp_patch_path);
        }
    } catch(...) {
        std::error_code ec;
        std::filesystem::remove(temp_patch_path, ec);
        throw;
    }

    std::error_code ec;
    std::filesystem::remove(temp_patch_path, ec);
}

void CliCommands::watch_dev_sandbox_from_patch_pck(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& project_dir,
    const std::filesystem::path& patch_pck,
    const std::filesystem::path& sandbox_dir,
    std::uint64_t interval_ms,
    const std::optional<std::filesystem::path>& log_file_path
) {
    const ScopedLogRedirect log_redirect(log_file_path);
    const gddelta::patch::RuntimePatchResolver resolver(project_dir);

    try {
        build_patch_pck_auto(base_pck, project_dir, patch_pck);
        build_dev_sandbox_from_pck(base_pck, patch_pck, sandbox_dir);
    } catch(const std::exception& exception) {
        std::cout
        << "Initial runtime patch build failed, falling back to base sandbox: "
        << exception.what() << "\n";
        support_.copy_base_into_sandbox(base_pck, sandbox_dir);
    }

    std::cout
    << "Watching runtime patch inputs in " << project_dir
    << " -> " << patch_pck
    << " -> " << sandbox_dir
    << " (" << interval_ms << " ms)\n";
    
    support_.watch_workspace_diff(project_dir, interval_ms, [&](const gddelta::workspace::WorkspaceDiff& diff) {
        const auto dirty_paths = resolver.collect_dirty_input_paths(diff);
        if(dirty_paths.empty()) return;

        try {
            std::cout
            << "Change detected, rebuilding runtime patch and sandbox:\n"
            << "  patch: " << patch_pck << "\n"
            << "  sandbox: " << sandbox_dir << "\n";
            support_.print_rebuild_paths("Runtime patch inputs", dirty_paths);
            build_patch_pck_from_inputs(base_pck, project_dir, patch_pck, dirty_paths);
            build_dev_sandbox_from_pck(base_pck, patch_pck, sandbox_dir);
            std::cout << "Watch rebuild complete.\n";
        } catch(const std::exception& exception) {
            std::cerr << "Watch rebuild failed: " << exception.what() << "\n";
        }
    });
}

void CliCommands::watch_dev_sandbox(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& project_dir,
    const std::filesystem::path& sandbox_dir,
    std::uint64_t interval_ms,
    const std::optional<std::filesystem::path>& log_file_path
) {
    const ScopedLogRedirect log_redirect(log_file_path);
    build_dev_sandbox(base_pck, project_dir, sandbox_dir);
    gddelta::patch::RuntimePatchResolver resolver(project_dir);
    const auto initial_stamp = resolver.calculate_watch_stamp();

    std::cout
    << "Watching " << project_dir
    << " -> " << sandbox_dir
    << " (" << interval_ms << " ms)\n";
    
    support_.watch_stamp(initial_stamp, interval_ms, [&resolver]() {
        return resolver.calculate_watch_stamp();
    }, [&]() {
        try {
            std::cout << "Workspace/runtime stamp changed, rebuilding sandbox: " << sandbox_dir << "\n";
            build_dev_sandbox(base_pck, project_dir, sandbox_dir);
            std::cout << "Watch rebuild complete.\n";
        } catch(const std::exception& exception) {
            std::cerr << "Watch rebuild failed: " << exception.what() << "\n";
        }
    });
}
