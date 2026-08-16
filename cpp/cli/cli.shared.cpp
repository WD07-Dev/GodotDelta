#include "cli.shared.h"
#include "core/patch/runtime_patch_resolver.h"
#include "core/common/path_utils.h"
#include "core/pck/pck_embedded.h"
#include "core/pck/pck_reader.h"
#include "core/pck/pck_writer.h"
#include "core/workspace/workspace.h"
#include<algorithm>
#include<array>
#include<cctype>
#include<chrono>
#include<cstring>
#include<cstdio>
#include<filesystem>
#include<fstream>
#include<iostream>
#include<optional>
#include<sstream>
#include<string>
#include<stdexcept>
#include<thread>
#include<unordered_set>

#ifdef _WIN32

#include<objbase.h>
#include<shldisp.h>
#include<urlmon.h>
#include<windows.h>

#else

#include<curl/curl.h>
#include<minizip/unzip.h>
#include<spawn.h>
#include<sys/types.h>
#include<unistd.h>
extern char **environ;

#endif

using namespace cli_internal;
namespace {
std::string path_to_utf8(const std::filesystem::path& path) {
    return gddelta::common::path_to_utf8(path);
}

constexpr const char *kGdreToolsWindowsUrl = "https://github.com/GDRETools/gdsdecomp/releases/download/v2.6.3/GDRE_tools-v2.6.3-windows.zip";
constexpr const char *kGdreToolsLinuxUrl = "https://github.com/GDRETools/gdsdecomp/releases/download/v2.6.3/GDRE_tools-v2.6.3-linux.zip";
constexpr const char *kGdreExtractDirectoryName = ".gdre_extract";
constexpr std::size_t kWindowsCommandLengthLimit = 28000;
constexpr std::size_t kUnixCommandLengthLimit = 120000;

std::filesystem::path find_ui_executable(const std::filesystem::path& cli_path) {
    const auto cli_dir = std::filesystem::absolute(cli_path).parent_path();
#ifdef _WIN32
    for(const auto& base_dir : {cli_dir, cli_dir.parent_path()}) {
        const auto ui_path = base_dir / "GodotDelta.exe";
        if(std::filesystem::exists(ui_path)) {
            return ui_path;
        }
    }
#else
    for(const auto& base_dir : {cli_dir, cli_dir.parent_path()}) {
        for(const auto& candidate_name : {"GodotDelta.x86_64", "GodotDelta"}) {
            const auto ui_path = base_dir / candidate_name;
            if(std::filesystem::exists(ui_path)) {
                return ui_path;
            }
        }
    }
#endif
    throw std::runtime_error("Failed to find GodotDelta UI executable next to CLI: " + cli_dir.string());
}

std::filesystem::path build_temporary_copy_path(const std::filesystem::path& source_path) {
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temp_path = std::filesystem::temp_directory_path();
    auto temp_name = std::filesystem::path(".gddelta_base_");
    temp_name += source_path.filename();
    temp_name += ".";
    temp_name += std::to_string(timestamp);
    temp_name += ".tmp";
    temp_path /= temp_name;
    return temp_path;
}

std::string quote_argument(const std::string& value) {
    std::string result = "\"";
    for(const auto character : value) {
        if(character == '"' || character == '\\') {
            result += '\\';
        }
        result += character;
    }
    result += '"';
    return result;
}

std::filesystem::path derive_project_root_from_source(
    const std::filesystem::path& source_path,
    std::string_view pack_path
) {
    auto project_root = source_path;
    for(const auto& component : std::filesystem::path(std::string(pack_path))) {
        static_cast<void>(component);
        project_root = project_root.parent_path();
    }
    return project_root;
}

std::vector<gddelta::patch::ProjectInputPath> to_identity_project_input_paths(
    const std::vector<std::string>& input_paths
) {
    std::vector<gddelta::patch::ProjectInputPath> mapped_inputs;
    mapped_inputs.reserve(input_paths.size());
    for(const auto& input_path : input_paths) {
        mapped_inputs.push_back(gddelta::patch::ProjectInputPath { input_path, input_path });
    }
    return mapped_inputs;
}

#ifdef _WIN32
std::wstring widen_native_string(const std::string& value) {
    if(value.empty()) return {};

    auto convert = [&](UINT code_page) -> std::wstring {
        const auto size = MultiByteToWideChar(code_page, 0, value.c_str(), -1, nullptr, 0);
        if(size <= 0) return {};

        std::wstring wide(static_cast<std::size_t>(size - 1), L'\0');
        if(MultiByteToWideChar(code_page, 0, value.c_str(), -1, wide.data(), size) <= 0) {
            return {};
        }
        return wide;
    };

    auto wide = convert(CP_UTF8);
    if(!wide.empty()) return wide;
    return convert(CP_ACP);
}

std::wstring quote_argument(const std::wstring& value) {
    std::wstring result = L"\"";
    for(const auto character : value) {
        if(character == L'"' || character == L'\\') {
            result += L'\\';
        }
        result += character;
    }
    result += L'"';
    return result;
}

std::string narrow_utf8_string(const std::wstring& value) {
    if(value.empty()) return {};

    const auto size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if(size <= 0) return {};

    std::string narrow(static_cast<std::size_t>(size - 1), '\0');
    if(WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, narrow.data(), size, nullptr, nullptr) <= 0) {
        return {};
    }
    return narrow;
}

std::string describe_windows_error(DWORD error_code) {
    LPWSTR message_buffer = nullptr;
    const auto size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&message_buffer),
        0,
        nullptr
    );

    std::string message;
    if(size > 0 && message_buffer != nullptr) {
        message = narrow_utf8_string(std::wstring(message_buffer, size));
        LocalFree(message_buffer);
    }

    while(!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
        message.pop_back();
    }

    if(message.empty()) {
        return "Win32 error " + std::to_string(error_code);
    }
    return "Win32 error " + std::to_string(error_code) + ": " + message;
}
#endif

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::uint64_t fnv1a_64(std::string_view value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for(const auto byte : value) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << value;
    return stream.str();
}

std::uint64_t hash_file_contents(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if(!input) {
        throw std::runtime_error("Failed to open file for hashing: " + path.string());
    }

    std::array<char, 64 * 1024> buffer{};
    std::uint64_t hash = 14695981039346656037ULL;

    while(input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        for(std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            hash *= 1099511628211ULL;
        }
    }

    return hash;
}

std::string cache_key_for_file(const std::filesystem::path& source_file, std::string_view discriminator) {
    std::error_code ec;
    const auto absolute_path = std::filesystem::absolute(source_file, ec);
    const auto content_hash = hash_file_contents(source_file);

    std::ostringstream metadata;
    metadata
    << path_to_utf8(ec ? source_file : absolute_path)
    << '|'
    << content_hash
    << '|'
    << discriminator;
    return hex_u64(fnv1a_64(metadata.str()));
}

bool has_virtual_extension(std::string_view path, std::string_view extension) {
    return path.size() >= extension.size()
    && path.compare(path.size() - extension.size(), extension.size(), extension) == 0;
}

std::string build_conversion_cache_key(
    const std::filesystem::path& source_file,
    std::string_view cache_prefix,
    std::string_view cache_discriminator
) {
    return cache_key_for_file(
        source_file,
        std::string(cache_prefix) + "|" + std::string(cache_discriminator)
    );
}

bool is_text_resource_extension(std::string_view extension) {
    return extension == ".tscn"
    || extension == ".tres"
    || extension == ".gdshader"
    || extension == ".theme"
    || extension == ".cfg"
    || extension == ".json";
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

bool is_preferred_text_resource_binary_output(
    const std::filesystem::path& source_file,
    const std::filesystem::path& candidate
) {
    const auto source_name = path_to_utf8(source_file.filename());
    const auto candidate_name = path_to_utf8(candidate.filename());
    const auto candidate_extension = to_lower_copy(path_to_utf8(candidate.extension()));

    if(candidate_name == source_name + ".converted.res") return true;
    if(candidate_name == source_name + ".converted.scn") return true;

    const auto source_extension = to_lower_copy(path_to_utf8(source_file.extension()));
    if(source_extension == ".tscn" && candidate_extension == ".scn") return true;
    if(source_extension == ".tres" && candidate_extension == ".res") return true;
    if(source_extension == ".godot" && candidate_extension == ".binary") return true;

    return false;
}

bool is_binary_text_resource_pack_path(std::string_view path) {
    return has_virtual_extension(path, ".converted.res")
    || has_virtual_extension(path, ".converted.scn")
    || has_virtual_extension(path, ".res")
    || has_virtual_extension(path, ".scn")
    || has_virtual_extension(path, ".gdc");
}

std::filesystem::path select_best_text_resource_binary_output(
    const std::filesystem::path& source_file,
    const std::vector<std::filesystem::path>& outputs
) {
    if(outputs.empty()) {
        return {};
    }

    for(const auto& output : outputs) {
        if(is_preferred_text_resource_binary_output(source_file, output)) {
            return output;
        }
    }

    return *std::max_element(outputs.begin(), outputs.end(), [](const auto& left, const auto& right) {
        std::error_code left_ec;
        std::error_code right_ec;
        const auto left_size = std::filesystem::file_size(left, left_ec);
        const auto right_size = std::filesystem::file_size(right, right_ec);
        if(left_ec || right_ec) {
            return path_to_utf8(left.filename()) < path_to_utf8(right.filename());
        }
        if(left_size != right_size) {
            return left_size < right_size;
        }
        return path_to_utf8(left.filename()) < path_to_utf8(right.filename());
    });
}

std::optional<std::filesystem::path> find_text_pack_source_path(
    const std::unordered_map<std::string, std::filesystem::path>& text_pack_sources,
    std::string_view pack_path
) {
    if(const auto it = text_pack_sources.find(std::string(pack_path)); it != text_pack_sources.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool is_binary_remap_target(const std::optional<std::string>& remap_target) {
    if(!remap_target.has_value()) {
        return false;
    }
    return is_binary_text_resource_pack_path(*remap_target);
}

void upsert_pack_file_source(
    std::vector<gddelta::pck::PckWriteFile>& files,
    std::string_view pack_path,
    const std::filesystem::path& source_path
) {
    for(auto& file : files) {
        if(file.removal || file.pack_path != pack_path) continue;
        file.source_path = source_path;
        return;
    }

    gddelta::pck::PckWriteFile added_file;
    added_file.pack_path = std::string(pack_path);
    added_file.source_path = source_path;
    added_file.removal = false;
    files.push_back(std::move(added_file));
}

std::vector<std::string> collect_all_project_source_inputs(const std::filesystem::path& project_dir) {
    std::vector<std::string> input_paths;
    std::error_code ec;
    auto it = std::filesystem::recursive_directory_iterator(project_dir, ec);
    const auto end = std::filesystem::recursive_directory_iterator();
    while(it != end) {
        try {
            if(ec) {
                ec.clear();
                it.increment(ec);
                continue;
            }

            const auto relative_path = std::filesystem::relative(it->path(), project_dir);
            if(it->is_directory() && !gddelta::patch::RuntimePatchResolver::is_project_source_candidate(relative_path)) {
                it.disable_recursion_pending();
                it.increment(ec);
                continue;
            }

            if(it->is_regular_file() && gddelta::patch::RuntimePatchResolver::is_project_source_candidate(relative_path)) {
                input_paths.push_back(gddelta::common::path_to_utf8(relative_path));
            }
        } catch(const std::filesystem::filesystem_error&) {
            it.disable_recursion_pending();
        }
        it.increment(ec);
    }

    std::sort(input_paths.begin(), input_paths.end());
    input_paths.erase(std::unique(input_paths.begin(), input_paths.end()), input_paths.end());
    return input_paths;
}

std::optional<std::string> read_remap_target_pack_path(const std::filesystem::path& remap_path) {
    std::ifstream input(remap_path);
    if(!input) return std::nullopt;

    std::string line;
    while(std::getline(input, line)) {
        if(line.rfind("path=", 0) != 0) continue;

        const auto first_quote = line.find('"');
        const auto last_quote = line.rfind('"');
        if(first_quote == std::string::npos || last_quote == std::string::npos || last_quote <= first_quote) {
            continue;
        }

        return gddelta::common::normalize_pack_relative_path(
            line.substr(first_quote + 1, last_quote - first_quote - 1)
        );
    }

    return std::nullopt;
}

std::optional<std::string> read_remap_target_pack_path_from_text(std::string_view content) {
    std::istringstream input{std::string(content)};
    std::string line;
    while(std::getline(input, line)) {
        if(line.rfind("path=", 0) != 0) continue;

        const auto first_quote = line.find('"');
        const auto last_quote = line.rfind('"');
        if(first_quote == std::string::npos || last_quote == std::string::npos || last_quote <= first_quote) {
            continue;
        }

        return gddelta::common::normalize_pack_relative_path(
            line.substr(first_quote + 1, last_quote - first_quote - 1)
        );
    }

    return std::nullopt;
}

std::optional<std::string> read_remap_target_pack_path_from_entry(
    const gddelta::pck::PckReader& reader,
    std::string_view remap_pack_path
) {
    const auto entry = reader.find_entry("res://" + std::string(remap_pack_path));
    if(!entry.has_value()) {
        return std::nullopt;
    }

    const auto bytes = reader.read_entry_data(*entry);
    return read_remap_target_pack_path_from_text(std::string_view(
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size()
    ));
}

std::optional<std::string> resolve_remap_target_pack_path(
    const gddelta::pck::PckReader& reader,
    const std::filesystem::path& project_dir,
    std::string_view input_path
) {
    const auto remap_pack_path = std::string(input_path) + ".remap";
    const auto local_remap_path = project_dir / gddelta::common::path_from_utf8(remap_pack_path);
    if(const auto local_target = read_remap_target_pack_path(local_remap_path); local_target.has_value()) {
        return local_target;
    }

    return read_remap_target_pack_path_from_entry(reader, remap_pack_path);
}

std::size_t gdre_command_length_limit() {
#ifdef _WIN32
    return kWindowsCommandLengthLimit;
#else
    return kUnixCommandLengthLimit;
#endif
}

void remove_if_exists(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void remove_all_if_exists(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

void normalize_runtime_patch_files(
    std::vector<gddelta::pck::PckWriteFile>& files,
    const gddelta::pck::PckWriteOptions& options
) {
    if(options.format_version == 1) {
        const auto original_size = files.size();
        files.erase(
            std::remove_if(files.begin(), files.end(), [](const gddelta::pck::PckWriteFile& file) {
                return file.removal;
            }),
            files.end()
        );

        const auto removed_count = original_size - files.size();
        if(removed_count > 0) {
            std::cout
            << "Skipped " << removed_count
            << " removal entr" << (removed_count == 1 ? "y" : "ies")
            << " because PCK format v1 does not support removals.\n";
        }

        std::unordered_set<std::string> gdc_targets;
        for(const auto& file : files) {
            if(!has_virtual_extension(file.pack_path, ".gdc")) continue;
            gdc_targets.insert(replace_virtual_extension(file.pack_path, ".gd"));
        }

        files.erase(
            std::remove_if(files.begin(), files.end(), [&](const gddelta::pck::PckWriteFile& file) {
                return has_virtual_extension(file.pack_path, ".gd")
                    && gdc_targets.contains(file.pack_path);
            }),
            files.end()
        );
    }
}

std::filesystem::path find_compiled_gdscript_output(
    const std::filesystem::path& output_dir,
    const std::filesystem::path& source_file
) {
    const auto expected_output = output_dir / source_file.filename().replace_extension(".gdc");
    if(std::filesystem::exists(expected_output)) {
        return expected_output;
    }

    std::error_code ec;
    for(const auto& entry : std::filesystem::directory_iterator(output_dir, ec)) {
        if(ec || !entry.is_regular_file()) continue;
        if(entry.path().filename() == source_file.filename().replace_extension(".gdc")) {
            return entry.path();
        }
    }
    return {};
}

bool is_gdre_binary_name(const std::filesystem::path& path) {
    const auto file_name = to_lower_copy(path_to_utf8(path.filename()));
#ifdef _WIN32
    return file_name == "gdre_tools.exe";
#else
    return file_name == "gdre_tools" || file_name == "gdre_tools.x86_64" || file_name == "gdre_tools.64";
#endif
}

std::filesystem::path find_gdre_binary(const std::filesystem::path& install_dir) {
    if(!std::filesystem::exists(install_dir)) return {};
    for(const auto& entry : std::filesystem::recursive_directory_iterator(install_dir)) {
        if(!entry.is_regular_file()) continue;
        if(is_gdre_binary_name(entry.path())) {
            return entry.path();
        }
    }

    return {};
}

std::filesystem::path gdre_binary_output_path(const std::filesystem::path& install_dir) {
#ifdef _WIN32
    return install_dir / "gdre_tools.exe";
#else
    return install_dir / "gdre_tools";
#endif
}

std::filesystem::path gdre_archive_output_path(const std::filesystem::path& install_dir) {
#ifdef _WIN32
    return install_dir / "gdre_tools-windows.zip";
#else
    return install_dir / "gdre_tools-linux.zip";
#endif
}

const char *gdre_download_url() {
#ifdef _WIN32
    return kGdreToolsWindowsUrl;
#else
    return kGdreToolsLinuxUrl;
#endif
}

void copy_directory_contents(const std::filesystem::path& source_dir, const std::filesystem::path& target_dir) {
    std::error_code ec;
    std::filesystem::create_directories(target_dir, ec);
    if(ec) {
        throw std::runtime_error("Failed to create destination directory: " + target_dir.string());
    }

    for(const auto& entry : std::filesystem::recursive_directory_iterator(source_dir)) {
        const auto relative_path = std::filesystem::relative(entry.path(), source_dir);
        const auto destination = target_dir / relative_path;

        if(entry.is_directory()) {
            std::filesystem::create_directories(destination, ec);
            if(ec) {
                throw std::runtime_error("Failed to prepare extracted directory: " + destination.string());
            }
            continue;
        }
        if(!entry.is_regular_file()) continue;

        std::filesystem::create_directories(destination.parent_path(), ec);
        if(ec) throw std::runtime_error("Failed to prepare extracted path: " + destination.parent_path().string());

        std::filesystem::copy_file(entry.path(), destination, std::filesystem::copy_options::overwrite_existing, ec);
        if(ec) throw std::runtime_error("Failed to copy extracted file: " + entry.path().string());
    }
}

void normalize_gdre_layout(const std::filesystem::path& install_dir) {
    auto binary_path = find_gdre_binary(install_dir);
    if(binary_path.empty()) {
        throw std::runtime_error("Failed to find GDRE tools binary after extraction.");
    }

    if(binary_path.parent_path() != install_dir) {
        copy_directory_contents(binary_path.parent_path(), install_dir);
        binary_path = find_gdre_binary(install_dir);
        if(binary_path.empty()) {
            throw std::runtime_error("Failed to normalize GDRE tools into install directory.");
        }
    }

    const auto normalized_path = gdre_binary_output_path(install_dir);
    if(binary_path != normalized_path) {
        std::error_code ec;
        std::filesystem::copy_file(binary_path, normalized_path, std::filesystem::copy_options::overwrite_existing, ec);
        if(ec) {
            throw std::runtime_error("Failed to create normalized GDRE tools binary: " + normalized_path.string());
        }
    }

#ifndef _WIN32
    std::filesystem::permissions(
        normalized_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add
    );
#endif
}

void ensure_text_line_in_section(
    const std::filesystem::path& file_path,
    std::string_view section_name,
    std::string_view line_to_insert
) {
    std::ifstream input(file_path);
    if(!input) {
        throw std::runtime_error("Failed to open config file: " + file_path.string());
    }

    std::vector<std::string> lines;
    std::string line;
    while(std::getline(input, line)) {
        if(!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }

    const std::string section_header = "[" + std::string(section_name) + "]";
    const std::string requested_line(line_to_insert);
    bool section_found = false;
    bool line_found = false;
    std::size_t insert_index = lines.size();

    for(std::size_t index = 0; index < lines.size(); ++index) {
        if(lines[index] != section_header) continue;

        section_found = true;
        insert_index = index + 1;
        for(std::size_t section_index = index + 1; section_index < lines.size(); ++section_index) {
            const auto& section_line = lines[section_index];
            if(!section_line.empty() && section_line.front() == '[') {
                insert_index = section_index;
                break;
            }
            if(section_line == requested_line) {
                line_found = true;
                break;
            }
            insert_index = section_index + 1;
        }
        break;
    }
    if(line_found) return;

    if(!section_found) {
        if(!lines.empty() && !lines.back().empty()) lines.emplace_back();
        lines.emplace_back(section_header);
        lines.emplace_back();
        lines.emplace_back(requested_line);
    }else {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_index), requested_line);
    }

    std::ofstream output(file_path, std::ios::out | std::ios::trunc);
    if(!output) {
        throw std::runtime_error("Failed to write config file: " + file_path.string());
    }

    for(const auto& output_line : lines) {
        output << output_line << '\n';
    }
}

#ifndef _WIN32

size_t curl_write_file_callback(void *contents, size_t size, size_t nmemb, void *user_data) {
    auto *stream = static_cast<std::ofstream *>(user_data);
    const auto bytes = size * nmemb;
    stream->write(static_cast<const char *>(contents), static_cast<std::streamsize>(bytes));
    return stream->good() ? bytes : 0;
}

void download_file_with_libcurl(const std::string& url, const std::filesystem::path& output_path) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    auto *curl = curl_easy_init();
    if(curl == nullptr) {
        throw std::runtime_error("Failed to initialize libcurl.");
    }

    std::ofstream output(output_path, std::ios::binary | std::ios::out | std::ios::trunc);
    if(!output.is_open()) {
        curl_easy_cleanup(curl);
        throw std::runtime_error("Failed to open download output file: " + output_path.string());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_file_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);

    const auto result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    output.close();

    if(result != CURLE_OK) {
        throw std::runtime_error("Failed to download GDRE tools archive: " + std::string(curl_easy_strerror(result)));
    }
}

void extract_zip_with_minizip(const std::filesystem::path& archive_path, const std::filesystem::path& output_dir) {
    auto *zip_file = unzOpen64(archive_path.string().c_str());
    if(zip_file == nullptr) {
        throw std::runtime_error("Failed to open GDRE tools archive: " + archive_path.string());
    }

    const auto close_zip = [&]() {
        unzClose(zip_file);
    };

    if(unzGoToFirstFile(zip_file) != UNZ_OK) {
        close_zip();
        throw std::runtime_error("Failed to read GDRE tools archive entries.");
    }

    do {
        unz_file_info64 file_info{};
        char file_name[1024] = {};
        if(unzGetCurrentFileInfo64(zip_file, &file_info, file_name, sizeof(file_name), nullptr, 0, nullptr, 0) != UNZ_OK) {
            close_zip();
            throw std::runtime_error("Failed to read GDRE tools archive file info.");
        }

        const auto relative_path = std::filesystem::path(file_name);
        const auto destination = output_dir / relative_path;
        if(relative_path.empty()) continue;

        if(file_name[std::strlen(file_name) - 1] == '/') {
            std::filesystem::create_directories(destination);
            continue;
        }

        std::filesystem::create_directories(destination.parent_path());
        if(unzOpenCurrentFile(zip_file) != UNZ_OK) {
            close_zip();
            throw std::runtime_error("Failed to open archive entry: " + relative_path.string());
        }

        std::ofstream output(destination, std::ios::binary | std::ios::out | std::ios::trunc);
        if(!output.is_open()) {
            unzCloseCurrentFile(zip_file);
            close_zip();
            throw std::runtime_error("Failed to open extracted file: " + destination.string());
        }

        std::array<char, 16384> buffer{};
        for(;;) {
            const auto bytes_read = unzReadCurrentFile(zip_file, buffer.data(), static_cast<unsigned int>(buffer.size()));
            if(bytes_read < 0) {
                output.close();
                unzCloseCurrentFile(zip_file);
                close_zip();
                throw std::runtime_error("Failed to extract archive entry: " + relative_path.string());
            }
            if(bytes_read == 0) break;
            output.write(buffer.data(), bytes_read);
        }

        output.close();
        unzCloseCurrentFile(zip_file);
    } while(unzGoToNextFile(zip_file) == UNZ_OK);

    close_zip();
}

#else

std::wstring to_wstring(const std::filesystem::path& path) {
    return path.wstring();
}

void download_file_with_urlmon(const std::string& url, const std::filesystem::path& output_path) {
    const auto wide_url = std::wstring(url.begin(), url.end());
    const auto wide_path = to_wstring(output_path);
    const auto result = URLDownloadToFileW(nullptr, wide_url.c_str(), wide_path.c_str(), 0, nullptr);
    if(FAILED(result)) {
        throw std::runtime_error("Failed to download GDRE tools archive.");
    }
}

class ScopedComInitializer {
    public:
        ScopedComInitializer() {
            result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        }

        ~ScopedComInitializer() {
            if(SUCCEEDED(result_)) {
                CoUninitialize();
            }
        }

        void ensure() const {
            if(FAILED(result_)) {
                throw std::runtime_error("Failed to initialize COM for GDRE tools extraction.");
            }
        }

    private:
        HRESULT result_ = E_FAIL;
};

void extract_zip_with_shell(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& output_dir,
    const std::function<bool(const std::filesystem::path&)>& ready_check
) {
    ScopedComInitializer com;
    com.ensure();

    IShellDispatch *shell = nullptr;
    if(FAILED(CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) {
        throw std::runtime_error("Failed to create Windows Shell for GDRE tools extraction.");
    }

    auto release_shell = [&]() {
        if(shell != nullptr) {
            shell->Release();
        }
    };

    Folder *zip_folder = nullptr;
    Folder *dest_folder = nullptr;
    FolderItems *items = nullptr;

    VARIANT archive_variant;
    VariantInit(&archive_variant);
    archive_variant.vt = VT_BSTR;
    archive_variant.bstrVal = SysAllocString(to_wstring(archive_path).c_str());

    VARIANT dest_variant;
    VariantInit(&dest_variant);
    dest_variant.vt = VT_BSTR;
    dest_variant.bstrVal = SysAllocString(to_wstring(output_dir).c_str());

    if(FAILED(shell->NameSpace(archive_variant, &zip_folder)) || zip_folder == nullptr) {
        VariantClear(&archive_variant);
        VariantClear(&dest_variant);
        release_shell();
        throw std::runtime_error("Failed to open GDRE tools zip folder.");
    }

    if(FAILED(shell->NameSpace(dest_variant, &dest_folder)) || dest_folder == nullptr) {
        zip_folder->Release();
        VariantClear(&archive_variant);
        VariantClear(&dest_variant);
        release_shell();
        throw std::runtime_error("Failed to open GDRE tools destination folder.");
    }

    if(FAILED(zip_folder->Items(&items)) || items == nullptr) {
        dest_folder->Release();
        zip_folder->Release();
        VariantClear(&archive_variant);
        VariantClear(&dest_variant);
        release_shell();
        throw std::runtime_error("Failed to read GDRE tools zip items.");
    }

    VARIANT items_variant;
    VariantInit(&items_variant);
    items_variant.vt = VT_DISPATCH;
    items_variant.pdispVal = items;

    VARIANT options_variant;
    VariantInit(&options_variant);
    options_variant.vt = VT_I4;
    options_variant.lVal = 16 | 1024;

    const auto copy_result = dest_folder->CopyHere(items_variant, options_variant);

    VariantClear(&options_variant);
    VariantClear(&items_variant);
    items->Release();
    dest_folder->Release();
    zip_folder->Release();
    VariantClear(&archive_variant);
    VariantClear(&dest_variant);
    release_shell();

    if(FAILED(copy_result)) {
        throw std::runtime_error("Failed to extract GDRE tools archive.");
    }

    for(int i = 0; i < 200; ++i) {
        if(ready_check(output_dir)) return;
        Sleep(100);
    }

    throw std::runtime_error("Timed out waiting for archive extraction.");
}

#endif

}

void CliSupport::set_cli_path(std::filesystem::path cli_path) {
    cli_path_ = std::move(cli_path);
}

void CliSupport::set_base_encryption_key(std::optional<std::string> key) {
    base_encryption_key_ = std::move(key);
}

void CliSupport::launch_ui(const std::filesystem::path& cli_path) const {
    const auto ui_path = find_ui_executable(cli_path);
#ifdef _WIN32
    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};
    auto command_line = ui_path.wstring();
    if(!CreateProcessW(
        nullptr,
        command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        ui_path.parent_path().wstring().c_str(),
        &startup_info,
        &process_info
    )) {
        throw std::runtime_error("Failed to launch GodotDelta UI: " + ui_path.string());
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
#else
    pid_t pid = -1;
    auto command = ui_path.string();
    char *argv[] = {command.data(), nullptr};
    const auto result = posix_spawn(
        &pid,
        command.c_str(),
        nullptr,
        nullptr,
        argv,
        environ
    );
    if(result != 0) {
        throw std::runtime_error("Failed to launch GodotDelta UI: " + ui_path.string());
    }
#endif

    std::cout << "Started GodotDelta UI: " << ui_path << "\n";
}

std::filesystem::path CliSupport::resolve_cli_directory(const std::filesystem::path& cli_path) const {
    return std::filesystem::absolute(cli_path).parent_path();
}

std::filesystem::path CliSupport::resolve_tools_directory(const std::filesystem::path& cli_path) const {
    return resolve_cli_directory(cli_path);
}

std::filesystem::path CliSupport::resolve_cache_directory() const {
    if(cli_path_.empty()) {
        throw std::runtime_error("CLI path is not initialized.");
    }

    const auto cache_dir = resolve_tools_directory(cli_path_) / ".gddelta_cache";
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if(ec) {
        throw std::runtime_error("Failed to create cache directory: " + cache_dir.string());
    }
    return cache_dir;
}

void CliSupport::ensure_gdre_tools(const std::filesystem::path& cli_path) const {
    const auto install_dir = resolve_tools_directory(cli_path);
    const auto normalized_binary_path = gdre_binary_output_path(install_dir);
    if(std::filesystem::exists(normalized_binary_path)) {
        if(!gdre_ready_announced_) {
            std::cout << "GDRE tools ready: " << normalized_binary_path << "\n";
            gdre_ready_announced_ = true;
        }
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(install_dir, ec);
    if(ec) {
        throw std::runtime_error("Failed to create install directory: " + install_dir.string());
    }

    const auto archive_path = gdre_archive_output_path(install_dir);
    const auto extract_dir = install_dir / kGdreExtractDirectoryName;
    std::filesystem::remove_all(extract_dir, ec);
    std::filesystem::create_directories(extract_dir, ec);
    if(ec) {
        throw std::runtime_error("Failed to prepare GDRE extraction directory: " + extract_dir.string());
    }

    const auto *download_url = gdre_download_url();

    std::cout << "[GDRE 1/3] Downloading GDRE tools from " << download_url << "\n";

#ifdef _WIN32
    download_file_with_urlmon(download_url, archive_path);
    std::cout << "[GDRE 2/3] Extracting GDRE tools archive\n";
    extract_zip_with_shell(archive_path, extract_dir, [](const std::filesystem::path& output_dir) {
        return !find_gdre_binary(output_dir).empty();
    });
#else
    download_file_with_libcurl(download_url, archive_path);
    std::cout << "[GDRE 2/3] Extracting GDRE tools archive\n";
    extract_zip_with_minizip(archive_path, extract_dir);
#endif

    std::cout << "[GDRE 3/3] Finalizing GDRE tools installation\n";
    copy_directory_contents(extract_dir, install_dir);
    normalize_gdre_layout(install_dir);

    std::filesystem::remove_all(extract_dir, ec);
    std::filesystem::remove(archive_path, ec);

    gdre_ready_announced_ = true;
    std::cout << "Prepared GDRE tools in " << install_dir << "\n";
}

std::filesystem::path CliSupport::resolve_gdre_tools_path() const {
    if(cli_path_.empty()) {
        throw std::runtime_error("CLI path is not initialized.");
    }

    const auto install_dir = resolve_tools_directory(cli_path_);
#ifdef _WIN32
    const auto candidate = install_dir / "gdre_tools.exe";
#else
    const auto candidate = install_dir / "gdre_tools";
#endif
    if(!std::filesystem::exists(candidate)) {
        throw std::runtime_error("Failed to find GDRE tools binary: " + candidate.string());
    }
    return candidate;
}

std::string CliSupport::run_gdre_tools_command(const std::vector<std::string>& args) const {
    ensure_gdre_tools(cli_path_);
    const auto gdre_path = resolve_gdre_tools_path();
    const auto output_path = build_temporary_copy_path("gdre_tools_output.txt");
    auto effective_args = args;
    const auto has_key_arg = std::any_of(effective_args.begin(), effective_args.end(), [](const std::string& arg) {
        return arg.rfind("--key=", 0) == 0;
    });
    if(base_encryption_key_.has_value() && !base_encryption_key_->empty() && !has_key_arg) {
        effective_args.push_back("--key=" + *base_encryption_key_);
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    const auto output_handle = CreateFileW(
        output_path.wstring().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security_attributes,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if(output_handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Failed to open GDRETools output capture file.");
    }

    const auto input_handle = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security_attributes,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if(input_handle == INVALID_HANDLE_VALUE) {
        CloseHandle(output_handle);
        throw std::runtime_error("Failed to open NUL handle for GDRETools stdin.");
    }

    std::wstring command_line = quote_argument(gdre_path.wstring());
    for(const auto& arg : effective_args) {
        command_line += L" ";
        command_line += quote_argument(widen_native_string(arg));
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = input_handle;
    startup_info.hStdOutput = output_handle;
    startup_info.hStdError = output_handle;

    PROCESS_INFORMATION process_info{};
    auto mutable_command_line = command_line;
    const auto created = CreateProcessW(
        gdre_path.wstring().c_str(),
        mutable_command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        gdre_path.parent_path().wstring().c_str(),
        &startup_info,
        &process_info
    );
    CloseHandle(output_handle);
    CloseHandle(input_handle);

    if(!created) {
        throw std::runtime_error("Failed to launch GDRETools process. " + describe_windows_error(GetLastError()));
    }

    WaitForSingleObject(process_info.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
#else
    std::ostringstream command;
    command << quote_argument(gdre_path.string());
    for(const auto& arg : effective_args) {
        command << " " << quote_argument(arg);
    }
    command << " > " << quote_argument(output_path.string()) << " 2>&1";

    const auto exit_code = std::system(command.str().c_str());
#endif

    std::ifstream output_stream(output_path, std::ios::binary);
    std::ostringstream output;
    output << output_stream.rdbuf();
    output_stream.close();

    std::error_code ec;
    std::filesystem::remove(output_path, ec);

    if(exit_code != 0) {
        throw std::runtime_error("GDRETools command failed.\n" + output.str());
    }

    return output.str();
}

std::string CliSupport::detect_base_engine_version(const std::filesystem::path& base_pck) const {
    const auto temp_copy = create_temporary_base_copy(base_pck);
    gddelta::pck::PckReader base_reader;
    try {
        base_reader.open(temp_copy);
    } catch(...) {
        std::error_code ec;
        std::filesystem::remove(temp_copy, ec);
        throw;
    }
    std::error_code ec;
    std::filesystem::remove(temp_copy, ec);

    return std::to_string(base_reader.header().engine_major) + "." +
    std::to_string(base_reader.header().engine_minor) + "." +
    std::to_string(base_reader.header().engine_patch);
}

CompiledGDScriptOutputs CliSupport::compile_gdscript_files(
    const std::filesystem::path& base_pck,
    const std::vector<std::filesystem::path>& source_files,
    const std::filesystem::path& output_dir,
    bool allow_partial_failures
) const {
    CompiledGDScriptOutputs compiled_outputs;
    if(source_files.empty()) return compiled_outputs;

    const auto bytecode_version = detect_base_engine_version(base_pck);
    const auto cache_root = resolve_cache_directory() / "gdscript" / bytecode_version;
    std::filesystem::create_directories(cache_root);
    std::cout
    << "Compiling " << source_files.size()
    << " GDScript file(s) for Godot " << bytecode_version << "\n";

    std::unordered_map<std::string, std::vector<std::filesystem::path>> files_by_basename;
    for(const auto& source_file : source_files) {
        files_by_basename[path_to_utf8(source_file.filename())].push_back(source_file);
    }

    std::vector<std::vector<std::filesystem::path>> compile_batches;
    std::vector<std::filesystem::path> unique_name_batch;
    std::size_t current_length_estimate = 128 + bytecode_version.size() + path_to_utf8(output_dir).size();
    const auto max_compile_command_length = gdre_command_length_limit();
    std::size_t reused_count = 0;

    for(const auto& [basename, grouped_files] : files_by_basename) {
        if(grouped_files.size() > 1) {
            for(const auto& source_file : grouped_files) {
                const auto cache_key = cache_key_for_file(
                    source_file,
                    "gdscript-cache-v2|" + bytecode_version
                );
                auto cached_output_path = cache_root / cache_key / source_file.filename().replace_extension(".gdc");
                if(std::filesystem::exists(cached_output_path)) {
                    compiled_outputs.outputs[path_to_utf8(source_file)] = cached_output_path;
                    ++reused_count;
                }else {
                    compile_batches.push_back({source_file});
                }
            }
            continue;
        }

        const auto& source_file = grouped_files.front();
        const auto cache_key = cache_key_for_file(
            source_file,
            "gdscript-cache-v2|" + bytecode_version
        );
        auto cached_output_path = cache_root / cache_key / source_file.filename().replace_extension(".gdc");
        if(std::filesystem::exists(cached_output_path)) {
            compiled_outputs.outputs[path_to_utf8(source_file)] = cached_output_path;
            ++reused_count;
            continue;
        }

        const auto next_length = current_length_estimate + path_to_utf8(source_file).size() + 16;
        if(!unique_name_batch.empty() && next_length > max_compile_command_length) {
            compile_batches.push_back(std::move(unique_name_batch));
            unique_name_batch.clear();
            current_length_estimate = 128 + bytecode_version.size() + path_to_utf8(output_dir).size();
        }

        unique_name_batch.push_back(source_file);
        current_length_estimate += path_to_utf8(source_file).size() + 16;
        static_cast<void>(basename);
    }

    if(!unique_name_batch.empty()) {
        compile_batches.push_back(std::move(unique_name_batch));
    }

    std::size_t compile_count = 0;
    for(const auto& batch : compile_batches) {
        compile_count += batch.size();
    }

    std::cout
    << "Prepared " << compile_batches.size()
    << " compile batch(es)\n";
    std::cout << "Reused " << reused_count << " cached GDScript output(s)\n";
    std::cout << "Compiling " << compile_count << " new GDScript output(s)\n";

    std::size_t batch_index = 0;
    for(const auto& batch : compile_batches) {
        const auto current_batch_index = batch_index + 1;
        std::cout
        << "Compiling batch " << current_batch_index
        << "/" << compile_batches.size()
        << " (" << batch.size() << " file(s))\n";

        const auto batch_output_dir = output_dir / std::to_string(batch_index++);
        std::filesystem::create_directories(batch_output_dir);

        std::vector<std::string> args = {
            "--headless",
            "--bytecode=" + bytecode_version,
            "--output=" + path_to_utf8(batch_output_dir),
        };
        for(const auto& source_file : batch) {
            args.push_back("--compile=" + path_to_utf8(source_file));
        }

        std::string command_output;
        try {
            command_output = run_gdre_tools_command(args);
        } catch(const std::exception& exception) {
            if(!allow_partial_failures) throw;
            command_output = exception.what();
        }
        for(const auto& source_file : batch) {
            const auto compiled_output_path = find_compiled_gdscript_output(batch_output_dir, source_file);
            if(!std::filesystem::exists(compiled_output_path)) {
                if(!allow_partial_failures) {
                    throw std::runtime_error(
                        "Failed to locate compiled GDScript bytecode output for: "
                        + path_to_utf8(source_file) + "\n" + command_output
                    );
                }
                compiled_outputs.failed_sources.push_back(source_file);
                std::cerr
                << "Warning: failed to compile GDScript, skipping: " << path_to_utf8(source_file) << "\n"
                << command_output << "\n";
                continue;
            }

            const auto cache_key = cache_key_for_file(
                source_file,
                "gdscript-cache-v2|" + bytecode_version
            );
            auto cached_output_path = cache_root / cache_key / source_file.filename().replace_extension(".gdc");
            std::error_code ec;
            std::filesystem::create_directories(cached_output_path.parent_path(), ec);
            std::filesystem::copy_file(compiled_output_path, cached_output_path, std::filesystem::copy_options::overwrite_existing, ec);
            if(ec) {
                compiled_outputs.outputs[path_to_utf8(source_file)] = compiled_output_path;
            }else {
                compiled_outputs.outputs[path_to_utf8(source_file)] = cached_output_path;
            }
        }
    }

    std::cout
    << "Finished compiling " << compiled_outputs.outputs.size()
    << " GDScript output file(s)\n";
    if(!compiled_outputs.failed_sources.empty()) {
        std::cerr
        << "Warning: skipped " << compiled_outputs.failed_sources.size()
        << " GDScript file(s) due to compile errors.\n";
    }
    return compiled_outputs;
}

std::filesystem::path CliSupport::convert_project_config_to_binary(
    const std::filesystem::path& source_file,
    const std::filesystem::path& output_dir,
    std::string_view cache_discriminator
) const {
    const auto source_extension = to_lower_copy(path_to_utf8(source_file.extension()));
    const auto output_file_name = source_extension == ".godot"
    ? (path_to_utf8(source_file.stem()) + ".binary") : (path_to_utf8(source_file.stem()) + ".cfb");
    const auto cache_key = build_conversion_cache_key(
        source_file,
        "project-config-cache-v1",
        cache_discriminator
    );
    auto cached_output_path = resolve_cache_directory() / "text_resource" / cache_key / output_file_name;
    if(std::filesystem::exists(cached_output_path)) {
        return cached_output_path;
    }

    std::filesystem::create_directories(output_dir);
    const auto output_path = output_dir / output_file_name;

    std::cout
    << "Converting project config " << source_file
    << " -> " << output_path << "\n";

    static_cast<void>(run_gdre_tools_command({
        "--headless",
        "--txt-to-bin=" + path_to_utf8(source_file),
        "--output=" + path_to_utf8(output_dir),
    }));

    if(!std::filesystem::exists(output_path)) {
        throw std::runtime_error("Failed to convert project config to binary: " + path_to_utf8(source_file));
    }

    std::error_code ec;
    std::filesystem::create_directories(cached_output_path.parent_path(), ec);
    std::filesystem::copy_file(output_path, cached_output_path, std::filesystem::copy_options::overwrite_existing, ec);
    return ec ? output_path : cached_output_path;
}

std::filesystem::path CliSupport::convert_text_resource_to_binary(
    const std::filesystem::path& source_file,
    const std::filesystem::path& output_dir,
    std::string_view cache_discriminator
) const {
    const auto cache_key = build_conversion_cache_key(
        source_file,
        "text-resource-cache-v1",
        cache_discriminator
    );
    const auto cache_dir = resolve_cache_directory() / "text_resource" / cache_key;
    if(std::filesystem::exists(cache_dir)) {
        for(const auto& entry : std::filesystem::directory_iterator(cache_dir)) {
            if(entry.is_regular_file()) {
                return entry.path();
            }
        }
    }

    std::filesystem::create_directories(output_dir);

    static_cast<void>(run_gdre_tools_command({
        "--headless",
        "--txt-to-bin=" + path_to_utf8(source_file),
        "--output=" + path_to_utf8(output_dir),
    }));

    std::vector<std::filesystem::path> outputs;
    for(const auto& entry : std::filesystem::recursive_directory_iterator(output_dir)) {
        if(entry.is_regular_file()) {
            outputs.push_back(entry.path());
        }
    }

    if(outputs.empty()) {
        throw std::runtime_error("Failed to convert text resource to binary: " + path_to_utf8(source_file));
    }

    std::sort(outputs.begin(), outputs.end());
    const auto selected_output = select_best_text_resource_binary_output(source_file, outputs);
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    const auto cached_output_path = cache_dir / selected_output.filename();
    std::filesystem::copy_file(selected_output, cached_output_path, std::filesystem::copy_options::overwrite_existing, ec);
    return ec ? selected_output : cached_output_path;
}

void CliSupport::compose_pck_from_project_files(
    const std::filesystem::path& base_pck,
    const std::vector<gddelta::pck::PckWriteFile>& files,
    const std::filesystem::path& output_path
) const {
    // For project-file based compose, let GDRETools read the base pack and patch files directly.
    if(files.empty()) {
        throw std::runtime_error("No files were provided for GDRETools patching.");
    }

    const auto resolved_base = resolve_base_input(base_pck);
    if(resolved_base.uses_external_pack && output_path.extension() == ".exe") {
        throw std::runtime_error("Cannot build a standalone EXE when the base input uses an external sibling PCK.");
    }

    const auto temp_base = create_temporary_base_copy(base_pck);
    gddelta::pck::PckReader base_reader;
    try {
        base_reader.open(temp_base);
    } catch(...) {
        remove_if_exists(temp_base);
        throw;
    }

    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temp_root = std::filesystem::temp_directory_path() / (".gddelta_gdre_patch_" + std::to_string(timestamp));
    const auto compiled_dir = temp_root / "compiled";
    std::filesystem::create_directories(compiled_dir);

    std::vector<std::filesystem::path> gd_files;
    for(const auto& file : files) {
        if(file.removal) {
            remove_if_exists(temp_base);
            remove_all_if_exists(temp_root);
            throw std::runtime_error("GDRETools file patch path does not support removal entries.");
        }
        if(file.source_path.extension() == ".gd") {
            gd_files.push_back(file.source_path);
        }
    }

    const auto compile_result = compile_gdscript_files(base_pck, gd_files, compiled_dir);
    const auto& compiled_gd_outputs = compile_result.outputs;
    const auto is_godot3 = base_reader.header().engine_major < 4;
    const auto cache_discriminator = detect_base_engine_version(base_pck);

    std::vector<std::string> patch_args;
    for(const auto& file : files) {
        if(file.pack_path == "project.godot") {
            const auto converted_config = convert_project_config_to_binary(
                file.source_path,
                compiled_dir / "project_config",
                cache_discriminator
            );
            patch_args.push_back("--patch-file=" + path_to_utf8(converted_config) + "=res://project.binary");
            continue;
        }

        if(file.source_path.extension() == ".gd") {
            const auto gd_entry = base_reader.find_entry("res://" + file.pack_path);
            if(gd_entry.has_value() && !is_godot3) {
                patch_args.push_back("--patch-file=" + path_to_utf8(file.source_path) + "=res://" + file.pack_path);
            }

            const auto gdc_pack_path_string = replace_virtual_extension(file.pack_path, ".gdc");
            const auto gdc_entry = base_reader.find_entry("res://" + gdc_pack_path_string);
            const auto autoconverted_gdc_entry = base_reader.find_entry("res://.autoconverted/" + gdc_pack_path_string);
            if(is_godot3 || gdc_entry.has_value() || autoconverted_gdc_entry.has_value()) {
                std::filesystem::path compiled_output_path;
                const auto compiled_it = compiled_gd_outputs.find(path_to_utf8(file.source_path));
                if(compiled_it != compiled_gd_outputs.end()) compiled_output_path = compiled_it->second;
                if(compiled_output_path.empty()) {
                    std::cerr
                    << "Warning: skipping script patch because bytecode compile failed: "
                    << path_to_utf8(file.source_path) << "\n";
                    continue;
                }
                if(gdc_entry.has_value()) {
                    patch_args.push_back("--patch-file=" + path_to_utf8(compiled_output_path) + "=res://" + gdc_pack_path_string);
                }else if(autoconverted_gdc_entry.has_value()) {
                    patch_args.push_back("--patch-file=" + path_to_utf8(compiled_output_path) + "=res://.autoconverted/" + gdc_pack_path_string);
                }else if(is_godot3) {
                    patch_args.push_back("--patch-file=" + path_to_utf8(compiled_output_path) + "=res://" + gdc_pack_path_string);
                }
            }
            continue;
        }

        patch_args.push_back("--patch-file=" + path_to_utf8(file.source_path) + "=res://" + file.pack_path);
    }

    try {
        std::filesystem::path current_input = resolved_base.pack_path;
        std::optional<std::filesystem::path> previous_intermediate_output;
        std::size_t batch_start = 0;
        std::size_t batch_index = 0;
        const auto max_patch_command_length = gdre_command_length_limit();

        while(batch_start < patch_args.size()) {
            std::vector<std::string> args = {
                "--headless",
                "--pck-patch=" + path_to_utf8(current_input),
            };

            std::size_t command_length_estimate = path_to_utf8(current_input).size() + 128;
            std::size_t batch_end = batch_start;

            while(batch_end < patch_args.size()) {
                const auto& patch_arg = patch_args[batch_end];
                const auto next_length = command_length_estimate + patch_arg.size() + 4;
                if(batch_end > batch_start && next_length > max_patch_command_length) break;
                command_length_estimate = next_length;
                args.push_back(patch_arg);
                ++batch_end;
            }

            const auto is_last_batch = batch_end >= patch_args.size();
            const auto batch_output = is_last_batch
            ? output_path : temp_root / ("batch_" + std::to_string(batch_index++) + ".pck");

            args.push_back("--output=" + path_to_utf8(batch_output));
            if(output_path.extension() == ".exe" && is_last_batch) {
                args.push_back("--embed=" + path_to_utf8(resolved_base.pack_path));
            }

            static_cast<void>(run_gdre_tools_command(args));

            if(previous_intermediate_output.has_value()) {
                remove_if_exists(*previous_intermediate_output);
            }

            if(!is_last_batch) {
                previous_intermediate_output = batch_output;
                current_input = batch_output;
            }
            batch_start = batch_end;
        }
    } catch(...) {
        remove_if_exists(temp_base);
        remove_all_if_exists(temp_root);
        throw;
    }

    remove_if_exists(temp_base);
    remove_all_if_exists(temp_root);
}

std::vector<gddelta::pck::PckWriteFile> CliSupport::collect_runtime_patch_files(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& project_dir,
    const std::vector<gddelta::patch::ProjectInputPath>& input_paths,
    bool legacy_simple
) const {
    const auto options = build_pack_options_from_base(base_pck);
    const gddelta::patch::RuntimePatchResolver resolver(project_dir);
    auto files = resolver.collect_patch_files(input_paths, legacy_simple);
    if(!legacy_simple) {
        normalize_runtime_patch_files(files, options);
    }
    return files;
}

std::vector<gddelta::pck::PckWriteFile> CliSupport::collect_runtime_patch_files(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& project_dir,
    const std::vector<std::string>& input_paths,
    bool legacy_simple
) const {
    const auto options = build_pack_options_from_base(base_pck);
    const gddelta::patch::RuntimePatchResolver resolver(project_dir);
    auto files = resolver.collect_patch_files(input_paths, legacy_simple);
    if(!legacy_simple) {
        normalize_runtime_patch_files(files, options);
    }
    return files;
}

void CliSupport::normalize_runtime_patch_files_for_base(
    const std::filesystem::path& base_pck,
    std::vector<gddelta::pck::PckWriteFile>& files
) const {
    const auto options = build_pack_options_from_base(base_pck);
    normalize_runtime_patch_files(files, options);
}

PreparedRuntimePatchFiles CliSupport::prepare_runtime_patch_files(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& project_dir,
    const std::vector<gddelta::patch::ProjectInputPath>& input_paths,
    bool compile_for_write
) const {
    PreparedRuntimePatchFiles prepared;
    const auto options = build_pack_options_from_base(base_pck);
    const gddelta::patch::RuntimePatchResolver resolver(project_dir);
    const auto base_reader = open_supported_base_pack(base_pck);
    std::cout << "[prepare] Collecting patch files from " << input_paths.size() << " input(s)\n";

    std::unordered_set<std::string> seen_pack_paths;
    const auto add_unique_file = [&](gddelta::pck::PckWriteFile file) {
        if(file.pack_path.empty() || seen_pack_paths.contains(file.pack_path)) return;
        seen_pack_paths.insert(file.pack_path);
        prepared.files.push_back(std::move(file));
    };

    for(const auto& input_path : input_paths) {
        const auto remap_pack_path = input_path.pack_path + ".remap";
        const auto remap_target = resolve_remap_target_pack_path(base_reader, project_dir, input_path.source_path);
        if(remap_target.has_value() && is_binary_text_resource_pack_path(*remap_target)) {
            gddelta::pck::PckWriteFile converted_file;
            converted_file.pack_path = *remap_target;
            converted_file.source_path = project_dir / gddelta::common::path_from_utf8(input_path.source_path);
            converted_file.removal = !std::filesystem::exists(converted_file.source_path);
            add_unique_file(std::move(converted_file));

            const auto local_remap_path = project_dir / gddelta::common::path_from_utf8(input_path.source_path + ".remap");
            if(std::filesystem::exists(local_remap_path)) {
                gddelta::pck::PckWriteFile remap_file;
                remap_file.pack_path = remap_pack_path;
                remap_file.source_path = local_remap_path;
                add_unique_file(std::move(remap_file));
            }
            continue;
        }

        for(auto& file : resolver.collect_patch_files({input_path})) {
            add_unique_file(std::move(file));
        }
    }

    if(compile_for_write) {
        std::cout << "[prepare] Preparing files for write/compile\n";
        prepared.temp_dir = prepare_runtime_patch_files_for_write(base_pck, prepared.files);
    }
    normalize_runtime_patch_files(prepared.files, options);
    return prepared;
}

PreparedRuntimePatchFiles CliSupport::prepare_runtime_patch_files(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& project_dir,
    const std::vector<std::string>& input_paths,
    bool compile_for_write
) const {
    PreparedRuntimePatchFiles prepared;
    const auto options = build_pack_options_from_base(base_pck);
    const gddelta::patch::RuntimePatchResolver resolver(project_dir);
    const auto base_reader = open_supported_base_pack(base_pck);
    std::cout << "[prepare] Collecting patch files from " << input_paths.size() << " input(s)\n";

    std::unordered_set<std::string> seen_pack_paths;
    const auto add_unique_file = [&](gddelta::pck::PckWriteFile file) {
        if(file.pack_path.empty() || seen_pack_paths.contains(file.pack_path)) return;
        seen_pack_paths.insert(file.pack_path);
        prepared.files.push_back(std::move(file));
    };

    for(const auto& input_path : input_paths) {
        const auto remap_pack_path = input_path + ".remap";
        const auto remap_target = resolve_remap_target_pack_path(base_reader, project_dir, input_path);
        if(remap_target.has_value() && is_binary_text_resource_pack_path(*remap_target)) {
            gddelta::pck::PckWriteFile converted_file;
            converted_file.pack_path = *remap_target;
            converted_file.source_path = project_dir / gddelta::common::path_from_utf8(input_path);
            converted_file.removal = !std::filesystem::exists(converted_file.source_path);
            add_unique_file(std::move(converted_file));

            const auto local_remap_path = project_dir / gddelta::common::path_from_utf8(remap_pack_path);
            if(std::filesystem::exists(local_remap_path)) {
                gddelta::pck::PckWriteFile remap_file;
                remap_file.pack_path = remap_pack_path;
                remap_file.source_path = local_remap_path;
                add_unique_file(std::move(remap_file));
            }
            continue;
        }

        for(auto& file : resolver.collect_patch_files({input_path})) {
            add_unique_file(std::move(file));
        }
    }

    if(compile_for_write) {
        std::cout << "[prepare] Preparing files for write/compile\n";
        prepared.temp_dir = prepare_runtime_patch_files_for_write(base_pck, prepared.files);
    }
    normalize_runtime_patch_files(prepared.files, options);
    return prepared;
}

std::filesystem::path CliSupport::prepare_runtime_patch_files_for_write(
    const std::filesystem::path& base_pck,
    std::vector<gddelta::pck::PckWriteFile>& files,
    bool legacy_simple
) const {
    const auto pack_options = build_pack_options_from_base(base_pck);
    const auto is_godot3 = pack_options.engine_major < 4;
    const auto cache_discriminator = detect_base_engine_version(base_pck);
    std::optional<gddelta::pck::PckReader> legacy_base_reader;
    std::vector<std::filesystem::path> gd_files;
    std::unordered_map<std::string, std::filesystem::path> gd_pack_sources;
    std::unordered_map<std::string, std::filesystem::path> text_pack_sources;
    std::unordered_map<std::string, std::filesystem::path> remap_pack_sources;
    for(const auto& file : files) {
        if(file.removal) continue;

        const auto extension = path_to_utf8(file.source_path.extension());
        if(extension == ".gd") {
            gd_files.push_back(file.source_path);
            gd_pack_sources[file.pack_path] = file.source_path;
        }
        if(is_text_resource_extension(extension)) {
            text_pack_sources[file.pack_path] = file.source_path;
        }
        if(extension == ".remap" && file.pack_path.size() > 6) {
            remap_pack_sources[file.pack_path.substr(0, file.pack_path.size() - 6)] = file.source_path;
        }
    }
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp_root = std::filesystem::temp_directory_path() / (".gddelta_compiled_patch_" + std::to_string(timestamp));
    std::filesystem::create_directories(temp_root);

    if(!gd_files.empty()) {
        const auto compiled_dir = temp_root / "compiled";
        std::filesystem::create_directories(compiled_dir);

        const auto compile_result = compile_gdscript_files(base_pck, gd_files, compiled_dir, !legacy_simple);
        const auto& compiled_outputs = compile_result.outputs;
        std::unordered_set<std::string> failed_sources;
        for(const auto& failed_source : compile_result.failed_sources) {
            failed_sources.insert(path_to_utf8(failed_source));
        }

        files.erase(
            std::remove_if(files.begin(), files.end(), [&](const gddelta::pck::PckWriteFile& file) {
                if(file.removal) return false;
                if(file.source_path.extension() == ".gd") {
                    return failed_sources.contains(path_to_utf8(file.source_path));
                }
                if(file.source_path.extension() != ".gdc") return false;

                const auto gd_pack_path = replace_virtual_extension(file.pack_path, ".gd");
                const auto gd_it = gd_pack_sources.find(gd_pack_path);
                return gd_it != gd_pack_sources.end() && failed_sources.contains(path_to_utf8(gd_it->second));
            }),
            files.end()
        );

        for(auto& file : files) {
            if(file.removal || file.source_path.extension() != ".gdc") continue;

            const auto gd_pack_path = replace_virtual_extension(file.pack_path, ".gd");
            const auto gd_it = gd_pack_sources.find(gd_pack_path);
            if(gd_it == gd_pack_sources.end()) continue;

            const auto compiled_it = compiled_outputs.find(path_to_utf8(gd_it->second));
            if(compiled_it == compiled_outputs.end()) {
                std::cerr
                << "Warning: skipping compiled bytecode patch because compile failed: "
                << path_to_utf8(gd_it->second) << "\n";
                continue;
            }

            file.source_path = compiled_it->second;
        }

        if(is_godot3) {
            files.erase(
                std::remove_if(files.begin(), files.end(), [](const gddelta::pck::PckWriteFile& file) {
                    return !file.removal && file.source_path.extension() == ".gd";
                }),
                files.end()
            );
        }
    }

    for(auto& file : files) {
        if(file.removal || file.pack_path != "project.godot") continue;

        const auto converted_config = convert_project_config_to_binary(
            file.source_path,
            temp_root / "project_config",
            cache_discriminator
        );
        file.pack_path = "project.binary";
        file.source_path = converted_config;
    }

    std::unordered_map<std::string, std::string> resolved_remap_targets;
    for(const auto& [source_pack_path, remap_source_path] : remap_pack_sources) {
        const auto remap_target = read_remap_target_pack_path(remap_source_path);
        if(!is_binary_remap_target(remap_target)) continue;
        resolved_remap_targets[source_pack_path] = *remap_target;
    }

    if(legacy_simple) {
        legacy_base_reader = open_supported_base_pack(base_pck);
        for(const auto& [source_pack_path, source_text_path] : text_pack_sources) {
            if(resolved_remap_targets.contains(source_pack_path)) continue;

            const auto project_root = derive_project_root_from_source(source_text_path, source_pack_path);
            const auto remap_target = resolve_remap_target_pack_path(*legacy_base_reader, project_root, source_pack_path);
            if(!is_binary_remap_target(remap_target)) continue;
            resolved_remap_targets[source_pack_path] = *remap_target;
        }
    }

    std::unordered_set<std::string> converted_remap_sources;
    for(const auto& [source_pack_path, remap_target] : resolved_remap_targets) {
        const auto source_text_path = find_text_pack_source_path(text_pack_sources, source_pack_path);
        if(!source_text_path.has_value() || !std::filesystem::exists(*source_text_path)) continue;

        const auto output_dir = temp_root / "converted_resources" / std::to_string(std::hash<std::string>{}(remap_target));
        const auto converted_source_path = convert_text_resource_to_binary(
            *source_text_path,
            output_dir,
            cache_discriminator
        );
        upsert_pack_file_source(files, remap_target, converted_source_path);
        converted_remap_sources.insert(source_pack_path);
    }

    if(!legacy_simple) {
        files.erase(
            std::remove_if(files.begin(), files.end(), [&](const gddelta::pck::PckWriteFile& file) {
                if(file.removal) return false;
                return converted_remap_sources.contains(file.pack_path);
            }),
            files.end()
        );

        for(auto& file : files) {
            if(file.removal || !is_binary_text_resource_pack_path(file.pack_path)) continue;
            if(!is_text_resource_extension(path_to_utf8(file.source_path.extension()))) continue;

            const auto output_dir = temp_root / "converted_resources" / std::to_string(std::hash<std::string>{}(file.pack_path));
            file.source_path = convert_text_resource_to_binary(
                file.source_path,
                output_dir,
                cache_discriminator
            );
        }
    }

    return temp_root;
}

std::vector<gddelta::patch::ProjectInputPath> CliSupport::collect_project_source_inputs(
    const std::filesystem::path& project_dir,
    bool legacy_full_scan
) const {
    const gddelta::patch::RuntimePatchResolver resolver(project_dir);
    return resolver.collect_included_source_paths();
}

BaseInputPaths CliSupport::resolve_base_input(const std::filesystem::path& base_path) const {
    BaseInputPaths resolved;
    resolved.requested_path = base_path;
    resolved.pack_path = base_path;
    if(base_path.extension() != ".exe") return resolved;

    if(gddelta::pck::EmbeddedPckHandler::find_embedded_pck(base_path).has_value()) {
        return resolved;
    }

    auto sibling_pck = base_path.parent_path() / base_path.stem();
    sibling_pck += ".pck";
    if(std::filesystem::exists(sibling_pck)) {
        resolved.pack_path = sibling_pck;
        resolved.uses_external_pack = true;
    }
    return resolved;
}

std::filesystem::path CliSupport::create_temporary_base_copy(const std::filesystem::path& base_pck) const {
    const auto resolved = resolve_base_input(base_pck);
    const auto temp_path = build_temporary_copy_path(resolved.pack_path);
    std::error_code ec;
    std::filesystem::copy_file(resolved.pack_path, temp_path, std::filesystem::copy_options::overwrite_existing, ec);
    if(ec) {
        throw std::runtime_error("Failed to create temporary base copy: " + resolved.pack_path.string());
    }

    return temp_path;
}

gddelta::pck::PckReader CliSupport::open_supported_base_pack(const std::filesystem::path& base_pck) const {
    const auto resolved = resolve_base_input(base_pck);
    gddelta::pck::PckReader base_reader;
    base_reader.open(resolved.pack_path);
    return base_reader;
}

gddelta::pck::PckWriteOptions CliSupport::build_pack_options_from_base(const std::filesystem::path& base_pck) const {
    const auto temp_copy = create_temporary_base_copy(base_pck);
    gddelta::pck::PckWriteOptions options;
    {
        gddelta::pck::PckReader base_reader;
        base_reader.open(temp_copy);
        options.format_version = base_reader.header().format_version;
        options.engine_major = base_reader.header().engine_major;
        options.engine_minor = base_reader.header().engine_minor;
        options.engine_patch = base_reader.header().engine_patch;
    }

    std::error_code ec;
    std::filesystem::remove(temp_copy, ec);

    return options;
}

bool CliSupport::is_legacy_v1_pack(const std::filesystem::path& base_pck) const {
    return build_pack_options_from_base(base_pck).format_version == 1;
}

void CliSupport::copy_runtime_support_files(
    const std::filesystem::path& base_path,
    const std::filesystem::path& sandbox_dir
) const {
    const auto base_dir = base_path.parent_path();
    for(const auto& entry : std::filesystem::recursive_directory_iterator(base_dir)) {
        const auto candidate = entry.path();
        if(candidate == base_path) continue;
        const auto relative_path = std::filesystem::relative(candidate, base_dir);
        const auto destination = sandbox_dir / relative_path;
        std::error_code ec;

        if(entry.is_directory()) {
            std::filesystem::create_directories(destination, ec);
            if(ec) {
                throw std::runtime_error("Failed to create runtime support directory: " + candidate.string());
            }
            continue;
        }
        if(!entry.is_regular_file()) continue;

        std::filesystem::create_directories(destination.parent_path(), ec);
        if(ec) {
            throw std::runtime_error("Failed to prepare runtime support directory: " + destination.parent_path().string());
        }

        std::filesystem::copy_file(candidate, destination, std::filesystem::copy_options::overwrite_existing, ec);
        if(ec) {
            throw std::runtime_error("Failed to copy runtime support file: " + candidate.string());
        }
    }
}

std::filesystem::path CliSupport::create_cleanup_patch(
    const std::filesystem::path& sandbox_dir,
    const gddelta::pck::PckWriteOptions& options
) const {
    std::vector<gddelta::pck::PckWriteFile> removals;
    for (const auto *path : {
        ".gddeltainclude",
        ".gitattributes",
        ".gitignore",
        "export_presets.cfg",
    }) {
        gddelta::pck::PckWriteFile file;
        file.pack_path = path;
        file.removal = true;
        removals.push_back(std::move(file));
    }

    const auto cleanup_patch = sandbox_dir / ".gddelta_cleanup.tmp.pck";
    gddelta::pck::PckWriter writer;
    writer.write_files(removals, cleanup_patch, options);
    return cleanup_patch;
}

void CliSupport::copy_base_into_sandbox(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& sandbox_dir
) const {
    const auto resolved = resolve_base_input(base_pck);
    std::error_code ec;
    std::filesystem::create_directories(sandbox_dir, ec);
    if(ec) {
        throw std::runtime_error("Failed to create sandbox directory: " + sandbox_dir.string());
    }

    const auto sandbox_output = sandbox_dir / resolved.pack_path.filename();
    std::filesystem::copy_file(resolved.pack_path, sandbox_output, std::filesystem::copy_options::overwrite_existing, ec);
    if(ec) {
        throw std::runtime_error("Failed to copy base file into sandbox: " + resolved.pack_path.string());
    }

    CliSupport::copy_runtime_support_files(resolved.pack_path, sandbox_dir);

    std::cout << "Prepared base sandbox " << sandbox_dir << " from " << base_pck << "\n";
}

void CliSupport::watch_workspace_diff(
    const std::filesystem::path& project_dir,
    std::uint64_t interval_ms,
    const std::function<void(const gddelta::workspace::WorkspaceDiff &)>& on_diff
) const {
    gddelta::workspace::Workspace builder;
    auto previous = builder.build(project_dir);
    
    for(;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        const auto current = builder.build(project_dir);
        const auto diff = builder.diff(previous, current);
        previous = current;
        on_diff(diff);
    }
}

void CliSupport::watch_stamp(
    std::uint64_t initial_stamp,
    std::uint64_t interval_ms,
    const std::function<std::uint64_t()>& stamp_provider,
    const std::function<void()>& on_change
) const {
    auto last_stamp = initial_stamp;

    for(;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        const auto next_stamp = stamp_provider();
        if(next_stamp == last_stamp) continue;

        last_stamp = next_stamp;
        on_change();
    }
}

void CliSupport::print_rebuild_paths(
    const std::string& label,
    const std::vector<std::string>& paths
) const {
    std::cout << label << " (" << paths.size() << ")\n";

    constexpr std::size_t max_lines = 8;
    const auto visible_count = std::min(paths.size(), max_lines);
    for(std::size_t i = 0; i < visible_count; ++i) {
        std::cout << "  - " << paths[i] << "\n";
    }

    if(paths.size() > max_lines) {
        std::cout << "  ... and " << (paths.size() - max_lines) << " more\n";
    }
}

void CliSupport::print_rebuild_paths(
    const std::string& label,
    const std::vector<gddelta::patch::ProjectInputPath>& paths
) const {
    std::cout << label << " (" << paths.size() << ")\n";

    constexpr std::size_t max_lines = 8;
    const auto visible_count = std::min(paths.size(), max_lines);
    for(std::size_t i = 0; i < visible_count; ++i) {
        if(paths[i].source_path == paths[i].pack_path) {
            std::cout << "  - " << paths[i].source_path << "\n";
        }else {
            std::cout << "  - " << paths[i].pack_path << " <- " << paths[i].source_path << "\n";
        }
    }

    if(paths.size() > max_lines) {
        std::cout << "  ... and " << (paths.size() - max_lines) << " more\n";
    }
}