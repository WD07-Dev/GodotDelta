#include "cli.app.h"
#include<iostream>
#include<stdexcept>
using namespace std;

CliApplication::CliApplication():
    commands_(support_) {
}

int CliApplication::run(int argc, char **argv) {
    support_.set_cli_path(argv[0]);
    const auto is_bootstrap_command = argc >= 2 && std::string_view(argv[1]) == "bootstrap";
    if(!is_bootstrap_command) {
        support_.ensure_gdre_tools(argv[0]);
    }

    if(argc < 2) {
        print_usage();
        return 1;
    }

    return run_command(argv[1], argc, argv);
}

void CliApplication::print_usage() {
    cout
    << "Usage:\n"
    << "  Supported target runtime: Godot 4.x\n"
    << "  gddelta ui\n"
    << "\n"
    << "  Distribution:\n"
    << "  gddelta make <base.pck|base.exe> <project_dir> <output.gdmod>\n"
    << "  gddelta apply <base.pck|base.exe> <input.pck|input.gdmod> [sandbox_dir]\n"
    << "\n"
    << "  Development:\n"
    << "  gddelta watch-dev-build-patch <base.pck|base.exe> <project_dir> <patch.pck> <sandbox_dir> [interval_ms] [--log-file path]\n"
    << "  gddelta dev-build-patch <base.pck|base.exe> <patch.pck> <sandbox_dir>\n"
    << "  gddelta watch <base.pck|base.exe> <project_dir> <sandbox_dir> [interval_ms] [--log-file path]\n"
    << "  gddelta dev-build <base.pck|base.exe> <project_dir> <sandbox_dir>\n"
    << "\n"
    << "  Inspection And Advanced Commands:\n"
    << "  gddelta inspect <input.pck|input.exe>\n"
    << "  gddelta diff <base_dir> <modified_dir>\n"
    << "  gddelta patch <base_dir> <modified_dir> <output.pck>\n"
    << "  gddelta make-pck <base.pck|base.exe> <project_dir> <output.pck>\n"
    << "  gddelta compose <base.pck|base.exe> <patch.pck|project_dir> <output.pck|output.exe>\n";
}

bool CliApplication::require_arg_count(int argc, int required_argc) {
    if(argc >= required_argc) {
        return true;
    }

    print_usage();
    return false;
}

namespace {
bool require_non_empty_args(std::initializer_list<const char*> args) {
    for(const auto* arg : args) {
        if(arg == nullptr || std::string_view(arg).empty()) {
            return false;
        }
    }

    return true;
}
}

std::uint64_t CliApplication::parse_interval_ms(int argc, char **argv, int index) {
    return argc > index ? static_cast<std::uint64_t>(std::stoull(argv[index])) : 1000ULL;
}

CliApplication::WatchCommandOptions CliApplication::parse_watch_options(int argc, char **argv, int index) {
    WatchCommandOptions options;
    auto current_index = index;

    if(argc > current_index && std::string_view(argv[current_index]) != "--log-file") {
        options.interval_ms = static_cast<std::uint64_t>(std::stoull(argv[current_index]));
        ++current_index;
    }

    if(argc > current_index) {
        if(std::string_view(argv[current_index]) != "--log-file" || argc <= current_index + 1) {
            throw std::runtime_error("Invalid watch options. Expected [interval_ms] [--log-file path].");
        }
        options.log_file_path = std::filesystem::path(argv[current_index + 1]);
        current_index += 2;
    }

    if(argc > current_index) {
        throw std::runtime_error("Invalid watch options. Expected [interval_ms] [--log-file path].");
    }

    return options;
}

int CliApplication::run_command(std::string_view command, int argc, char **argv) {
    if(command == "ui") {
        support_.launch_ui(argv[0]);
        return 0;
    }

    if(command == "bootstrap") {
        support_.ensure_gdre_tools(argv[0]);
        return 0;
    }

    if(command == "inspect") {
        if(!require_arg_count(argc, 3)) return 1;
        commands_.inspect_pack(argv[2]);
        return 0;
    }

    if(command == "diff") {
        if(!require_arg_count(argc, 4)) return 1;
        commands_.diff_workspace(argv[2], argv[3]);
        return 0;
    }

    if(command == "patch") {
        if(!require_arg_count(argc, 5)) return 1;
        commands_.build_patch_pack(argv[2], argv[3], argv[4]);
        return 0;
    }

    // Legacy advanced alias kept for compatibility.
    if(command == "runtime-patch") {
        if(!require_arg_count(argc, 6)) return 1;
        commands_.build_patch_pck_from_dirs(argv[2], argv[3], argv[4], argv[5]);
        return 0;
    }

    // Legacy advanced alias kept for compatibility.
    if(command == "trace-runtime") {
        if(!require_arg_count(argc, 4)) return 1;
        commands_.trace_runtime_paths(argv[2], collect_input_paths(3, argc, argv));
        return 0;
    }

    if(command == "make-pck") {
        if(!require_arg_count(argc, 5)) return 1;
        commands_.build_patch_pck_auto(argv[2], argv[3], argv[4]);
        return 0;
    }

    if(command == "make") {
        if(!require_arg_count(argc, 5)) return 1;
        commands_.build_gdmod(argv[2], argv[3], argv[4]);
        return 0;
    }

    // Legacy advanced alias kept for compatibility.
    if(command == "runtime-patch-files") {
        if(!require_arg_count(argc, 6)) return 1;
        commands_.build_patch_pck_from_inputs(argv[2], argv[3], argv[4], collect_input_paths(5, argc, argv));
        return 0;
    }

    // Legacy advanced alias kept for compatibility.
    if(command == "watch-runtime-patch") {
        if(!require_arg_count(argc, 5)) return 1;
        const auto options = parse_watch_options(argc, argv, 5);
        commands_.watch_patch_pck(argv[2], argv[3], argv[4], options.interval_ms, options.log_file_path);
        return 0;
    }

    if(command == "dev-build-patch") {
        if(!require_arg_count(argc, 4)) return 1;
        if(argc >= 5) {
            commands_.build_dev_sandbox_from_pck(argv[2], argv[3], argv[4]);
        } else {
            commands_.apply_pck_in_place(argv[2], argv[3]);
        }
        return 0;
    }

    if(command == "apply") {
        if(!require_arg_count(argc, 4)) return 1;
        const auto input_path = std::filesystem::path(argv[3]);
        const auto is_gdmod_input = input_path.extension() == ".gdmod";
        if(is_gdmod_input) {
            if(argc >= 5) {
                commands_.apply_gdmod(argv[2], argv[3], std::filesystem::path(argv[4]));
            } else {
                commands_.apply_gdmod(argv[2], argv[3], std::nullopt);
            }
        } else {
            if(argc >= 5) {
                commands_.build_dev_sandbox_from_pck(argv[2], argv[3], argv[4]);
            } else {
                commands_.apply_pck_in_place(argv[2], argv[3]);
            }
        }
        return 0;
    }

    if(command == "watch-dev-build-patch") {
        if(!require_arg_count(argc, 6)) return 1;
        if(!require_non_empty_args({argv[2], argv[3], argv[4], argv[5]})) {
            throw std::runtime_error("watch-dev-build-patch requires non-empty base, project, patch, and sandbox paths.");
        }
        const auto options = parse_watch_options(argc, argv, 6);
        commands_.watch_dev_sandbox_from_patch_pck(argv[2], argv[3], argv[4], argv[5], options.interval_ms, options.log_file_path);
        return 0;
    }

    if(command == "dev-build") {
        if(!require_arg_count(argc, 5)) return 1;
        commands_.build_dev_sandbox(argv[2], argv[3], argv[4]);
        return 0;
    }

    if(command == "watch") {
        if(!require_arg_count(argc, 5)) return 1;
        const auto options = parse_watch_options(argc, argv, 5);
        commands_.watch_dev_sandbox(argv[2], argv[3], argv[4], options.interval_ms, options.log_file_path);
        return 0;
    }

    if(command == "compose") {
        if(!require_arg_count(argc, 5)) return 1;
        commands_.compose_pck(argv[2], argv[3], argv[4]);
        return 0;
    }

    print_usage();
    return 1;
}

vector<string> CliApplication::collect_input_paths(int start_index, int argc, char **argv) {
    vector<string> input_paths;
    for(int index = start_index; index < argc; ++index) {
        input_paths.emplace_back(argv[index]);
    }
    return input_paths;
}