#include "runtime_patch_resolver.h"
#include "core/common/include_rules.h"
#include "core/common/path_utils.h"
#include<algorithm>
#include<cstdint>
#include<fstream>
#include<iostream>
#include<optional>
#include<regex>
#include<queue>
#include<string>
#include<filesystem>
#include<string_view>
#include<unordered_set>
using namespace gddelta::patch;

namespace {
    inline constexpr const char *kProjectIncludeFileName = ".gddeltainclude";
    inline constexpr const char *kDefaultIncludeFilePath = "build/default.gddeltainclude";

    bool has_virtual_extension(std::string_view path, std::string_view extension) {
        return path.size() >= extension.size()
        && path.compare(path.size() - extension.size(), extension.size(), extension) == 0;
    }

    std::string replace_virtual_extension(std::string path, std::string_view extension) {
        const auto slash_pos = path.find_last_of('/');
        const auto dot_pos = path.find_last_of('.');
        if(dot_pos == std::string::npos || (slash_pos != std::string::npos && dot_pos < slash_pos)) {
            path += extension;
            return path;
        }

        path.erase(dot_pos);
        path += extension;
        return path;
    }

    bool is_text_reference_source_virtual(std::string_view path) {
        return has_virtual_extension(path, ".tscn")
        || has_virtual_extension(path, ".tres")
        || has_virtual_extension(path, ".gd")
        || has_virtual_extension(path, ".gdshader")
        || has_virtual_extension(path, ".json")
        || has_virtual_extension(path, ".cfg")
        || has_virtual_extension(path, ".txt");
    }
}

RuntimePatchResolver::RuntimePatchResolver(std::filesystem::path project_dir):
    project_dir_(std::move(project_dir)),
    export_map_(load_export_file_cache(project_dir_)) {
    for(const auto& [source_path, exported_path] : export_map_) {
        reverse_export_map_[exported_path] = source_path;
    }
}

bool RuntimePatchResolver::is_project_source_candidate(const std::filesystem::path &relative_path) {
    if(relative_path.empty()) return false;

    const auto first = *relative_path.begin();
    if(first == ".git" || first == ".godot" || first == "bin") {
        return false;
    }
    if(relative_path.parent_path().empty()) {
        const auto filename = gddelta::common::path_to_utf8(relative_path.filename());
        if(!filename.empty() && filename.front() == '.') {
            return false;
        }
    }

    const auto extension = gddelta::common::path_to_utf8(relative_path.extension());
    return extension != ".import" && extension != ".uid" && extension != ".tmp" && extension != ".remap" && extension != ".gdc";
}

void RuntimePatchResolver::warn_if_runtime_is_stale() const {
    const auto newest_source = find_newest_project_source();
    const auto newest_export = find_newest_export_marker();
    if(!newest_source || !newest_export || newest_source->time <= newest_export->time) return;

    std::cerr
    << "Warning: project runtime artifacts look stale.\n"
    << "Newest source: " << newest_source->path << "\n"
    << "Newest exported marker: " << newest_export->path << "\n"
    << "Run the project/export refresh in Godot before make/compose if changes are not applied.\n";
}

std::vector<gddelta::pck::PckWriteFile> RuntimePatchResolver::collect_patch_files(const std::vector<std::string>& input_paths) const {
    std::vector<pck::PckWriteFile> files;
    std::queue<std::string> pending_inputs;
    std::unordered_set<std::string> seen_inputs;
    std::unordered_set<std::string> seen_paths;

    const auto add_file = [&](const std::string &relative_path) {
        const auto normalized = normalize_project_relative_path(relative_path);
        if(normalized.empty() || seen_paths.contains(normalized)) return;
        seen_paths.insert(normalized);

        pck::PckWriteFile file;
        file.pack_path = normalized;
        file.source_path = project_dir_ / gddelta::common::path_from_utf8(normalized);
        file.removal = !std::filesystem::exists(file.source_path);
        files.push_back(std::move(file));
    };

    const auto add_existing_file = [&](const std::string& relative_path, const std::filesystem::path& source_path) {
        const auto normalized = normalize_project_relative_path(relative_path);
        if(normalized.empty() || seen_paths.contains(normalized) || !std::filesystem::exists(source_path)) return;
        seen_paths.insert(normalized);

        pck::PckWriteFile file;
        file.pack_path = normalized;
        file.source_path = source_path;
        file.removal = false;
        files.push_back(std::move(file));
    };

    const auto add_optional_file = [&](const std::string& relative_path) {
        const auto normalized = normalize_project_relative_path(relative_path);
        if(normalized.empty()) return;
        add_existing_file(normalized, project_dir_ / gddelta::common::path_from_utf8(normalized));
    };

    for(const auto& input_path : input_paths) {
        const auto normalized = normalize_project_relative_path(input_path);
        if(!normalized.empty() && seen_inputs.insert(normalized).second) {
            pending_inputs.push(normalized);
        }
    }

    while(!pending_inputs.empty()) {
        const auto normalized = pending_inputs.front();
        pending_inputs.pop();

        const auto full_path = project_dir_ / gddelta::common::path_from_utf8(normalized);
        const auto extension = gddelta::common::path_to_utf8(full_path.extension());
        const auto remap_outputs = collect_remap_outputs(normalized);
        const auto export_it = export_map_.find(normalized);
        const auto has_remap = std::filesystem::exists(
            project_dir_ / gddelta::common::path_from_utf8(normalized + ".remap")
        ) || !remap_outputs.empty()
        || (export_it != export_map_.end() && has_virtual_extension(export_it->second, ".converted.res"));
        if(!has_remap) add_file(normalized);
        if(extension == ".gd") {
            const auto gdc_path = replace_virtual_extension(normalized, ".gdc");
            const auto autoconverted_gdc = project_dir_ / ".autoconverted" / gddelta::common::path_from_utf8(gdc_path);
            if(std::filesystem::exists(autoconverted_gdc)) {
                add_existing_file(gdc_path, autoconverted_gdc);
            }else {
                add_optional_file(gdc_path);
            }
        }

        add_optional_file(normalized + ".remap");
        for(const auto& remap_output : remap_outputs) {
            add_optional_file(remap_output);
        }
        add_optional_file(normalized + ".uid");
        add_optional_file(normalized + ".import");
        for(const auto& import_output : collect_import_outputs(normalized)) {
            add_optional_file(import_output);
        }

        if(export_it != export_map_.end()) {
            add_optional_file(export_it->second);
        }

        if(!has_remap) {
            for(const auto& reference : collect_text_resource_references(normalized)) {
                if(seen_inputs.insert(reference).second) {
                    pending_inputs.push(reference);
                }
            }
        }
    }

    return files;
}

std::vector<std::string> RuntimePatchResolver::collect_auto_input_paths(const pck::PckReader& base_reader) const {
    const auto include_patterns = load_include_patterns(project_dir_);
    std::vector<std::string> dirty_inputs;
    for(const auto& input_path : collect_included_source_paths()) {
        if(matches_forced_include_patterns(input_path, include_patterns)) {
            dirty_inputs.push_back(input_path);
            continue;
        }

        const auto patch_files = collect_patch_files({input_path});
        const auto differs = std::any_of(patch_files.begin(), patch_files.end(), [&](const pck::PckWriteFile& file) {
            return patch_file_differs_from_base(base_reader, file);
        });
        if(differs) dirty_inputs.push_back(input_path);
    }
    return dirty_inputs;
}

std::uint64_t RuntimePatchResolver::calculate_watch_stamp() const {
    std::uint64_t stamp = 0;

    const auto newest_source = find_newest_project_source();
    if(newest_source) {
        stamp = std::max(stamp, static_cast<std::uint64_t>(newest_source->time.time_since_epoch().count()));
    }

    const auto newest_export = find_newest_export_marker();
    if(newest_export) {
        stamp = std::max(stamp, static_cast<std::uint64_t>(newest_export->time.time_since_epoch().count()));
    }

    return stamp;
}

std::vector<std::string> RuntimePatchResolver::collect_dirty_input_paths(const workspace::WorkspaceDiff& diff) const {
    std::unordered_set<std::string> dirty_paths;
    const auto collect_path = [&](const std::string& path) {
        if(is_project_source_candidate(path)) {
            dirty_paths.insert(path);
            return;
        }

        const auto export_it = reverse_export_map_.find(path);
        if(export_it != reverse_export_map_.end()) {
            dirty_paths.insert(export_it->second);
        }
    };

    for(const auto& file : diff.added) {
        collect_path(file.relative_path);
    }
    for(const auto& file : diff.modified) {
        collect_path(file.relative_path);
    }
    for(const auto& path : diff.removed) {
        collect_path(path);
    }

    return { dirty_paths.begin(), dirty_paths.end() };
}

std::string RuntimePatchResolver::normalize_project_relative_path(const std::string& path) {
    return gddelta::common::normalize_pack_relative_path(path);
}

std::optional<RuntimePatchResolver::TimestampedPath> RuntimePatchResolver::find_newest_project_source() const {
    std::optional<TimestampedPath> newest;
    std::error_code ec;
    auto it = std::filesystem::recursive_directory_iterator(project_dir_, ec);
    const auto end = std::filesystem::recursive_directory_iterator();
    while(it != end) {
        try {
            if(ec) {
                ec.clear();
                it.increment(ec);
                continue;
            }

            const auto relative_path = std::filesystem::relative(it->path(), project_dir_);
            if(it->is_directory() && !is_project_source_candidate(relative_path)) {
                it.disable_recursion_pending();
                it.increment(ec);
                continue;
            }

            if(it->is_regular_file() && is_project_source_candidate(relative_path)) {
                const auto write_time = std::filesystem::last_write_time(it->path());
                if(!newest || write_time > newest->time) {
                    newest = TimestampedPath { it->path(), write_time };
                }
            }
        } catch(const std::filesystem::filesystem_error&) {
            it.disable_recursion_pending();
        }
        it.increment(ec);
    }
    return newest;
}

std::vector<std::string> RuntimePatchResolver::collect_included_source_paths() const {
    const auto include_patterns = load_include_patterns(project_dir_);
    std::vector<std::string> source_paths;

    std::error_code ec;
    auto it = std::filesystem::recursive_directory_iterator(project_dir_, ec);
    const auto end = std::filesystem::recursive_directory_iterator();
    while(it != end) {
        try {
            if(ec) {
                ec.clear();
                it.increment(ec);
                continue;
            }

            const auto relative_path = std::filesystem::relative(it->path(), project_dir_);
            if(it->is_directory() && !is_project_source_candidate(relative_path)) {
                it.disable_recursion_pending();
                it.increment(ec);
                continue;
            }

            if(it->is_regular_file() && is_project_source_candidate(relative_path)) {
                const auto normalized_path = normalize_project_relative_path(gddelta::common::path_to_utf8(relative_path));
                if(matches_include_patterns(normalized_path, include_patterns)) {
                    source_paths.push_back(normalized_path);
                }
            }
        } catch(const std::filesystem::filesystem_error&) {
            it.disable_recursion_pending();
        }
        it.increment(ec);
    }

    std::sort(source_paths.begin(), source_paths.end());
    return source_paths;
}

std::optional<RuntimePatchResolver::TimestampedPath> RuntimePatchResolver::find_newest_export_marker() const {
    const auto exported_root = project_dir_ / ".godot" / "exported";
    if(!std::filesystem::exists(exported_root)) return std::nullopt;

    std::optional<TimestampedPath> newest;
    std::error_code ec;
    auto it = std::filesystem::recursive_directory_iterator(exported_root, ec);
    const auto end = std::filesystem::recursive_directory_iterator();
    while(it != end) {
        try {
            if(ec) {
                ec.clear();
                it.increment(ec);
                continue;
            }

            if(it->is_regular_file() && it->path().filename() == "file_cache") {
                const auto write_time = std::filesystem::last_write_time(it->path());
                if(!newest || write_time > newest->time) {
                    newest = TimestampedPath { it->path(), write_time };
                }
            }
        } catch(const std::filesystem::filesystem_error&) {
            it.disable_recursion_pending();
        }
        it.increment(ec);
    }
    return newest;
}

std::unordered_map<std::string, std::string> RuntimePatchResolver::load_export_file_cache(const std::filesystem::path& project_dir) {
    std::unordered_map<std::string, std::string> mappings;
    const auto exported_root = project_dir / ".godot" / "exported";
    if(!std::filesystem::exists(exported_root)) return mappings;

    std::error_code ec;
    auto it = std::filesystem::recursive_directory_iterator(exported_root, ec);
    const auto end = std::filesystem::recursive_directory_iterator();
    while(it != end) {
        try {
            if(ec) {
                ec.clear();
                it.increment(ec);
                continue;
            }

            if(it->is_regular_file() && it->path().filename() == "file_cache") {
                std::ifstream input(it->path());
                std::string line;
                while(std::getline(input, line)) {
                    const auto first = line.find("::");
                    const auto second = first == std::string::npos ? std::string::npos : line.find("::", first + 2);
                    const auto third = second == std::string::npos ? std::string::npos : line.find("::", second + 2);
                    if(first == std::string::npos || second == std::string::npos || third == std::string::npos) continue;

                    mappings[normalize_project_relative_path(line.substr(0, first))] = normalize_project_relative_path(line.substr(third + 2));
                }
            }
        } catch(const std::filesystem::filesystem_error&) {
            it.disable_recursion_pending();
        }
        it.increment(ec);
    }
    return mappings;
}

std::vector<RuntimePatchResolver::IncludeRule> RuntimePatchResolver::load_include_patterns(const std::filesystem::path& project_dir) {
    return gddelta::common::load_include_patterns(
        project_dir,
        std::filesystem::current_path() / kDefaultIncludeFilePath,
        kProjectIncludeFileName
    );
}

bool RuntimePatchResolver::matches_include_patterns(const std::string& path, const std::vector<IncludeRule>& patterns) {
    return gddelta::common::matches_include_patterns(path, patterns);
}

bool RuntimePatchResolver::matches_forced_include_patterns(const std::string& path, const std::vector<IncludeRule>& patterns) {
    return gddelta::common::matches_forced_include_patterns(path, patterns);
}

bool RuntimePatchResolver::patch_file_differs_from_base(const pck::PckReader& base_reader, const pck::PckWriteFile& file) {
    const auto base_entry = base_reader.find_entry("res://" + file.pack_path);
    if(file.removal) return base_entry.has_value();
    if(!base_entry.has_value()) return true;
    return !base_reader.entry_matches_file(*base_entry, file.source_path);
}

std::vector<std::string> RuntimePatchResolver::collect_text_resource_references(const std::string& relative_path) const {
    if(!is_text_reference_source_virtual(relative_path)) return {};

    const auto full_path = project_dir_ / gddelta::common::path_from_utf8(relative_path);
    if(!std::filesystem::exists(full_path)) return {};

    std::ifstream input(full_path);
    if(!input) {
        throw std::runtime_error("Failed to open text resource for dependency scan: " + gddelta::common::path_to_utf8(full_path));
    }

    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    static const std::regex resource_regex(R"(res://[^"\s\]]+)");

    std::unordered_set<std::string> references;
    for(std::sregex_iterator it(content.begin(), content.end(), resource_regex); it != std::sregex_iterator(); ++it) {
        references.insert(normalize_project_relative_path(it->str()));
    }

    return { references.begin(), references.end() };
}

std::vector<std::string> RuntimePatchResolver::collect_import_outputs(const std::string& relative_path) const {
    const auto import_path = project_dir_ / gddelta::common::path_from_utf8(relative_path + ".import");
    if(!std::filesystem::exists(import_path)) return {};

    std::ifstream input(import_path);
    if(!input) {
        throw std::runtime_error("Failed to open import file: " + gddelta::common::path_to_utf8(import_path));
    }

    std::unordered_set<std::string> outputs;
    std::string line;
    static const std::regex quoted_res_regex(R"(\"(res://[^\"]+)\")");
    while(std::getline(input, line)) {
        if(line.rfind("path=", 0) == 0) {
            const auto first_quote = line.find('"');
            const auto last_quote = line.rfind('"');
            if(first_quote != std::string::npos && last_quote != std::string::npos && last_quote > first_quote) {
                outputs.insert(normalize_project_relative_path(line.substr(first_quote + 1, last_quote - first_quote - 1)));
            }
            continue;
        }

        if(line.rfind("dest_files=", 0) == 0) {
            for(std::sregex_iterator it(line.begin(), line.end(), quoted_res_regex); it != std::sregex_iterator(); ++it) {
                outputs.insert(normalize_project_relative_path((*it)[1].str()));
            }
        }
    }

    return { outputs.begin(), outputs.end() };
}

std::vector<std::string> RuntimePatchResolver::collect_remap_outputs(const std::string& relative_path) const {
    const auto remap_path = project_dir_ / gddelta::common::path_from_utf8(relative_path + ".remap");
    if(!std::filesystem::exists(remap_path)) return {};

    std::ifstream input(remap_path);
    if(!input) {
        throw std::runtime_error("Failed to open remap file: " + gddelta::common::path_to_utf8(remap_path));
    }

    std::unordered_set<std::string> outputs;
    std::string line;
    while(std::getline(input, line)) {
        if(line.rfind("path=", 0) != 0) continue;

        const auto first_quote = line.find('"');
        const auto last_quote = line.rfind('"');
        if(first_quote == std::string::npos || last_quote == std::string::npos || last_quote <= first_quote) {
            continue;
        }

        outputs.insert(normalize_project_relative_path(line.substr(first_quote + 1, last_quote - first_quote - 1)));
    }

    return { outputs.begin(), outputs.end() };
}

bool RuntimePatchResolver::is_text_reference_source(const std::filesystem::path& path) {
    const auto extension = gddelta::common::path_to_utf8(path.extension());
    return extension == ".tscn" || extension == ".tres" 
    || extension == ".gd" || extension == ".gdshader" 
    || extension == ".json" || extension == ".cfg" 
    || extension == ".txt";
}
