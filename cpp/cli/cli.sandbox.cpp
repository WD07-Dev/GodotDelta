#include "cli.commands.h"
#include "cli.shared.h"
#include "core/common/path_utils.h"
#include "core/patch/gdmod_package.h"
#include "core/patch/merged_pack_builder.h"
#include "core/patch/runtime_patch_resolver.h"
#include "core/pck/pck_embedded.h"
#include "core/pck/pck_reader.h"
#include "core/pck/pck_writer.h"
#include<chrono>
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

    void remove_if_exists(const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    void remove_all_if_exists(const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    void ensure_directory(const std::filesystem::path& path, const char* error_prefix) {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        if(ec) {
            throw std::runtime_error(std::string(error_prefix) + path.string());
        }
    }

    std::filesystem::path resolve_dev_patch_output(
        const std::filesystem::path& sandbox_dir,
        const std::filesystem::path& pack_path
    ) {
        auto patch_name = pack_path.filename();
        patch_name += ".devbuild.runtime_patch.tmp.pck";
        return sandbox_dir / patch_name;
    }

    std::filesystem::path resolve_apply_backup_output(const std::filesystem::path& pack_path) {
        auto backup_output = pack_path.parent_path() / pack_path.stem();
        backup_output += ".apply.backup";
        backup_output += pack_path.extension();
        return backup_output;
    }

    class LegacyV1Pipeline {
        public:
            explicit LegacyV1Pipeline(const cli_internal::CliSupport& support):
                support_(support) {
            }

            [[nodiscard]] cli_internal::PreparedRuntimePatchFiles collect_dev_sandbox_files(
                const std::filesystem::path& base_pck,
                const std::filesystem::path& project_dir
            ) const {
                std::cout << "[legacy] Scanning project inputs\n";
                gddelta::patch::RuntimePatchResolver resolver(project_dir);
                resolver.warn_if_runtime_is_stale();
                const auto input_paths = support_.collect_project_source_inputs(project_dir, true);
                support_.print_rebuild_paths("Legacy runtime patch inputs", input_paths);
                std::cout << "[legacy] Preparing runtime patch files from " << input_paths.size() << " input(s)\n";

                cli_internal::PreparedRuntimePatchFiles prepared;
                prepared.files = support_.collect_runtime_patch_files(base_pck, project_dir, input_paths, true);
                if(!prepared.files.empty()) {
                    prepared.temp_dir = support_.prepare_runtime_patch_files_for_write(base_pck, prepared.files, true);
                    support_.normalize_runtime_patch_files_for_base(base_pck, prepared.files);
                }
                if(prepared.files.empty()) {
                    throw std::runtime_error("No runtime-related files were found for the requested paths.");
                }
                return prepared;
            }

            void build_dev_sandbox(
                const std::filesystem::path& base_pck,
                const std::filesystem::path& project_dir,
                const std::filesystem::path& sandbox_output
            ) const {
                const auto prepared = collect_dev_sandbox_files(base_pck, project_dir);
                std::cout << "[dev-build] Composing legacy sandbox output\n";
                support_.compose_pck_from_project_files(base_pck, prepared.files, sandbox_output);
            }

            [[nodiscard]] cli_internal::PreparedRuntimePatchFiles extract_patch_pack(
                const std::filesystem::path& patch_pck
            ) const {
                cli_internal::PreparedRuntimePatchFiles prepared;
                gddelta::pck::PckReader reader;
                reader.open(patch_pck);

                const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
                prepared.temp_dir = std::filesystem::temp_directory_path() / (".gddelta_legacy_patch_" + std::to_string(timestamp));
                std::filesystem::create_directories(prepared.temp_dir);

                for(const auto& entry : reader.entries()) {
                    if((entry.flags & gddelta::pck::kPackFileRemoval) != 0) {
                        throw std::runtime_error("Legacy v1 apply path does not support removal entries in patch packs.");
                    }

                    const auto normalized_path = gddelta::common::normalize_pack_relative_path(entry.path);
                    if(normalized_path.empty()) {
                        throw std::runtime_error("Legacy patch pack contains an invalid empty entry path.");
                    }

                    const auto relative_path = std::filesystem::path(normalized_path);
                    const auto destination_path = prepared.temp_dir / relative_path;
                    std::filesystem::create_directories(destination_path.parent_path());

                    const auto bytes = reader.read_entry_data(entry);
                    std::ofstream file(destination_path, std::ios::binary | std::ios::trunc);
                    if(!file) {
                        throw std::runtime_error("Failed to write extracted legacy patch file: " + destination_path.string());
                    }
                    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    if(!file) {
                        throw std::runtime_error("Failed to write extracted legacy patch file: " + destination_path.string());
                    }

                    gddelta::pck::PckWriteFile write_file;
                    write_file.pack_path = relative_path.generic_string();
                    write_file.source_path = destination_path;
                    prepared.files.push_back(std::move(write_file));
                }

                if(prepared.files.empty()) {
                    throw std::runtime_error("No runtime-related files were found in the legacy patch pack.");
                }
                return prepared;
            }

        private:
            const cli_internal::CliSupport& support_;
    };
}

void CliCommands::compose_pck(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& patch_pck,
    const std::filesystem::path& output_pck
) {
    const auto resolved_base = support_.resolve_base_input(base_pck);
    if(std::filesystem::is_directory(patch_pck)) {
        std::cout << "[compose 1/2] Preparing project patch files\n";
        gddelta::patch::RuntimePatchResolver(patch_pck).warn_if_runtime_is_stale();
        const auto input_paths = support_.collect_project_source_inputs(patch_pck);
        const auto prepared = support_.prepare_runtime_patch_files(
            base_pck,
            patch_pck,
            input_paths,
            support_.is_legacy_v1_pack(base_pck)
        );
        std::cout << "[compose 2/2] Merging project patch files into output\n";
        support_.compose_pck_from_project_files(base_pck, prepared.files, output_pck);
    }else if(support_.is_legacy_v1_pack(base_pck)) {
        const LegacyV1Pipeline legacy_pipeline(support_);
        std::cout << "[compose 1/2] Extracting legacy patch pack files\n";
        const auto prepared = legacy_pipeline.extract_patch_pack(patch_pck);
        std::cout << "[compose 2/2] Merging legacy patch files into output\n";
        support_.compose_pck_from_project_files(base_pck, prepared.files, output_pck);
    }else {
        std::cout << "[compose 1/2] Preparing base and patch packs\n";
        const auto options = support_.build_pack_options_from_base(base_pck);
        const auto temp_base = support_.create_temporary_base_copy(base_pck);
        gddelta::patch::MergedPackBuilder builder;
        try {
            std::cout << "[compose 2/2] Merging patch pack into output\n";
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
    ensure_directory(sandbox_dir, "Failed to create sandbox directory: ");
    const auto sandbox_output = sandbox_dir / resolved_base.pack_path.filename();
    const auto runtime_patch_output = resolve_dev_patch_output(sandbox_dir, resolved_base.pack_path);

    try {
        if(support_.is_legacy_v1_pack(base_pck)) {
            const LegacyV1Pipeline legacy_pipeline(support_);
            std::cout << "[dev-build] Building legacy dev sandbox\n";
            legacy_pipeline.build_dev_sandbox(base_pck, project_dir, sandbox_output);
        }else {
            std::cout << "[dev-build] Building runtime patch PCK\n";
            build_patch_pck_auto(base_pck, project_dir, runtime_patch_output);
            std::cout << "[dev-build] Composing sandbox output\n";
            compose_pck(base_pck, runtime_patch_output, sandbox_output);
        }
        support_.copy_runtime_support_files(resolved_base.pack_path, sandbox_dir);
    } catch(...) {
        remove_if_exists(runtime_patch_output);
        throw;
    }
    remove_if_exists(runtime_patch_output);

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
    ensure_directory(sandbox_dir, "Failed to create sandbox directory: ");

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
    std::cout << "[1/3] Building merged output from base and patch\n";
    const auto resolved_base = support_.resolve_base_input(base_pck);
    auto temp_output = resolved_base.pack_path.parent_path() / resolved_base.pack_path.stem();
    temp_output += ".apply.tmp";
    temp_output += resolved_base.pack_path.extension();
    compose_pck(base_pck, patch_pck, temp_output);

    std::cout << "[2/3] Replacing base file with merged output\n";
    const auto backup_output = resolve_apply_backup_output(resolved_base.pack_path);
    std::error_code ec;
    std::filesystem::remove(backup_output, ec);
    ec.clear();

    std::filesystem::rename(resolved_base.pack_path, backup_output, ec);
    if(ec) {
        std::filesystem::remove(temp_output, ec);
        throw std::runtime_error("Failed to move base file into backup location: " + resolved_base.pack_path.string());
    }

    ec.clear();
    std::filesystem::rename(temp_output, resolved_base.pack_path, ec);
    if(ec) {
        std::error_code restore_ec;
        std::filesystem::rename(backup_output, resolved_base.pack_path, restore_ec);
        std::filesystem::remove(temp_output, ec);
        if(restore_ec) {
            throw std::runtime_error(
                "Failed to replace base file with applied patch and failed to restore backup: "
                + resolved_base.pack_path.string()
            );
        }
        throw std::runtime_error("Failed to replace base file with applied patch: " + resolved_base.pack_path.string());
    }

    std::filesystem::remove(backup_output, ec);

    std::cout
    << "[3/3] Applied patch " << patch_pck
    << " into base " << base_pck << "\n";
}

void CliCommands::apply_gdmod(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& gdmod_path,
    const std::optional<std::filesystem::path>& sandbox_dir
) {
    gddelta::patch::GdmodPackage package;
    std::cout << "[1/4] Reading gdmod manifest\n";
    const auto manifest = package.read_manifest(gdmod_path);
    const auto resolved_base = support_.resolve_base_input(base_pck);
    auto temp_patch_path = resolved_base.pack_path.parent_path() / gdmod_path.stem();
    temp_patch_path += ".apply.tmp.pck";

    std::cout
    << "Applying gdmod " << gdmod_path
    << " built for " << manifest.base_file_name
    << " (" << manifest.engine_major << "." << manifest.engine_minor << "." << manifest.engine_patch << ")\n";

    if(manifest.base_file_name != resolved_base.pack_path.filename().string()) {
        std::cerr
        << "Warning: gdmod target base is " << manifest.base_file_name
        << ", but requested base is " << resolved_base.pack_path.filename().string() << "\n";
    }

    std::cout << "[2/4] Recovering patch payload from gdmod\n";
    if(manifest.legacy_plain_payload) {
        package.extract_patch_pck(gdmod_path, temp_patch_path);
    }else {
        package.extract_protected_patch_pck(resolved_base.pack_path, gdmod_path, temp_patch_path);
    }
    try {
        if(sandbox_dir.has_value()) {
            std::cout << "[3/4] Building sandbox output from recovered patch\n";
            build_dev_sandbox_from_pck(base_pck, temp_patch_path, *sandbox_dir);
        }else {
            std::cout << "[3/4] Applying recovered patch into base game\n";
            apply_pck_in_place(base_pck, temp_patch_path);
        }
    } catch(...) {
        remove_if_exists(temp_patch_path);
        throw;
    }

    remove_if_exists(temp_patch_path);
    std::cout << "[4/4] Finished applying gdmod\n";
}

void CliCommands::extract_gdmod_to_pck(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& gdmod_path,
    const std::filesystem::path& output_pck
) {
    gddelta::patch::GdmodPackage package;
    std::cout << "[1/3] Reading gdmod manifest\n";
    const auto manifest = package.read_manifest(gdmod_path);
    const auto resolved_base = support_.resolve_base_input(base_pck);

    std::cout
    << "Recovering patch PCK from gdmod " << gdmod_path
    << " built for " << manifest.base_file_name
    << " (" << manifest.engine_major << "." << manifest.engine_minor << "." << manifest.engine_patch << ")\n";

    if(manifest.base_file_name != resolved_base.pack_path.filename().string()) {
        std::cerr
        << "Warning: gdmod target base is " << manifest.base_file_name
        << ", but requested base is " << resolved_base.pack_path.filename().string() << "\n";
    }

    std::cout << "[2/3] Recovering patch payload from gdmod\n";
    if(manifest.legacy_plain_payload) {
        package.extract_patch_pck(gdmod_path, output_pck);
    }else {
        package.extract_protected_patch_pck(resolved_base.pack_path, gdmod_path, output_pck);
    }

    std::cout
    << "[3/3] Wrote recovered patch PCK to " << output_pck << "\n";
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
    const auto resolved_base = support_.resolve_base_input(base_pck);
    const auto sandbox_output = sandbox_dir / resolved_base.pack_path.filename();

    try {
        std::cout << "[watch] Preparing sandbox\n";
        build_patch_pck_auto(base_pck, project_dir, patch_pck);
        compose_pck(base_pck, patch_pck, sandbox_output);
        support_.copy_runtime_support_files(resolved_base.pack_path, sandbox_dir);
    } catch(const std::exception& exception) {
        std::cerr << "Initial dev sandbox build failed: " << exception.what() << "\n";
        throw;
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
            << "Change detected, rebuilding development patch state:\n"
            << "  patch: " << patch_pck << "\n"
            << "  sandbox: " << sandbox_dir << "\n";
            support_.print_rebuild_paths("Runtime patch inputs", dirty_paths);
            build_patch_pck_from_inputs(base_pck, project_dir, patch_pck, dirty_paths);
            compose_pck(base_pck, patch_pck, sandbox_output);
            support_.copy_runtime_support_files(resolved_base.pack_path, sandbox_dir);
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
    const auto legacy_v1 = support_.is_legacy_v1_pack(base_pck);
    const LegacyV1Pipeline legacy_pipeline(support_);
    const auto resolved_base = support_.resolve_base_input(base_pck);
    const auto runtime_patch_output = resolve_dev_patch_output(sandbox_dir, resolved_base.pack_path);
    const auto sandbox_output = sandbox_dir / resolved_base.pack_path.filename();

    std::cout
    << "Watching " << project_dir
    << " -> " << sandbox_dir
    << " (" << interval_ms << " ms)\n";

    const gddelta::patch::RuntimePatchResolver resolver(project_dir);
    support_.watch_workspace_diff(project_dir, interval_ms, [&](const gddelta::workspace::WorkspaceDiff& diff) {
        const auto dirty_paths = resolver.collect_dirty_input_paths(diff);
        if(dirty_paths.empty()) return;

        try {
            std::cout << "Workspace/runtime diff changed, rebuilding development patch state: " << sandbox_dir << "\n";
            support_.print_rebuild_paths("Runtime patch inputs", dirty_paths);
            if(legacy_v1) {
                legacy_pipeline.build_dev_sandbox(base_pck, project_dir, sandbox_output);
            }else {
                build_patch_pck_from_inputs(base_pck, project_dir, runtime_patch_output, dirty_paths);
                compose_pck(base_pck, runtime_patch_output, sandbox_output);
            }
            support_.copy_runtime_support_files(resolved_base.pack_path, sandbox_dir);
            std::cout << "Watch rebuild complete.\n";
        } catch(const std::exception& exception) {
            std::cerr << "Watch rebuild failed: " << exception.what() << "\n";
        }
    });
}