#include "cli.app.h"
#include<iostream>
#include<stdexcept>
using namespace std;

CliApplication::CliApplication():
    commands_(support_) {
}

int CliApplication::run(int argc, char **argv) {
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
    << "  gddelta make-patch <base.pck|base.exe> <project_dir> <output.pck>\n"
    << "  gddelta apply <base.pck|base.exe> <patch.pck> [sandbox_dir]\n"
    << "\n"
    << "  Development:\n"
    << "  gddelta watch-dev-build-patch <base.pck|base.exe> <project_dir> <patch.pck> <sandbox_dir> [interval_ms] [--log-file path]\n"
    << "  gddelta dev-build-patch <base.pck|base.exe> <patch.pck> <sandbox_dir>\n"
    << "  gddelta watch <base.pck|base.exe> <project_dir> <sandbox_dir> [interval_ms] [--log-file path]\n"
    << "  gddelta dev-build <base.pck|base.exe> <project_dir> <sandbox_dir>\n"
    << "\n"
    << "  Inspection And Advanced Commands:\n"
    << "  gddelta inspect <input.pck|input.exe>\n"
    << "  gddelta create <source_dir> <output.pck>\n"
    << "  gddelta diff <base_dir> <modified_dir>\n"
    << "  gddelta patch <base_dir> <modified_dir> <output.pck>\n"
    << "  gddelta runtime-patch <base.pck|base.exe> <base_dir> <modified_dir> <output.pck>\n"
    << "  gddelta trace-runtime <project_dir> <relative_path> [more_paths...]\n"
    << "  gddelta runtime-patch-files <base.pck|base.exe> <project_dir> <output.pck> <relative_path> [more_paths...]\n"
    << "  gddelta watch-runtime-patch <base.pck|base.exe> <project_dir> <output.pck> [interval_ms] [--log-file path]\n"
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

    if(command == "create") {
        if(!require_arg_count(argc, 4)) return 1;
        commands_.create_pack(argv[2], argv[3]);
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

    if(command == "runtime-patch") {
        if(!require_arg_count(argc, 6)) return 1;
        commands_.build_runtime_patch_pack(argv[2], argv[3], argv[4], argv[5]);
        return 0;
    }

    if(command == "trace-runtime") {
        if(!require_arg_count(argc, 4)) return 1;
        commands_.trace_runtime_paths(argv[2], collect_input_paths(3, argc, argv));
        return 0;
    }

    if(command == "make-patch") {
        if(!require_arg_count(argc, 5)) return 1;
        commands_.build_runtime_patch_auto(argv[2], argv[3], argv[4]);
        return 0;
    }

    if(command == "runtime-patch-files") {
        if(!require_arg_count(argc, 6)) return 1;
        commands_.build_runtime_patch_from_files(argv[2], argv[3], argv[4], collect_input_paths(5, argc, argv));
        return 0;
    }

    if(command == "watch-runtime-patch") {
        if(!require_arg_count(argc, 5)) return 1;
        const auto options = parse_watch_options(argc, argv, 5);
        commands_.watch_runtime_patch(argv[2], argv[3], argv[4], options.interval_ms, options.log_file_path);
        return 0;
    }

    if(command == "dev-build-patch" || command == "apply") {
        if(!require_arg_count(argc, 4)) return 1;
        if(argc >= 5) {
            commands_.build_dev_sandbox_from_patch(argv[2], argv[3], argv[4]);
        } else {
            commands_.apply_patch_in_place(argv[2], argv[3]);
        }
        return 0;
    }

    if(command == "watch-dev-build-patch") {
        if(!require_arg_count(argc, 6)) return 1;
        if(!require_non_empty_args({argv[2], argv[3], argv[4], argv[5]})) {
            throw std::runtime_error("watch-dev-build-patch requires non-empty base, project, patch, and sandbox paths.");
        }
        const auto options = parse_watch_options(argc, argv, 6);
        commands_.watch_dev_sandbox_from_runtime_patch(argv[2], argv[3], argv[4], argv[5], options.interval_ms, options.log_file_path);
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
        commands_.compose_pack(argv[2], argv[3], argv[4]);
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
