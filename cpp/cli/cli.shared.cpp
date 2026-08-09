#include "cli.shared.h"
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
constexpr const char *kGdreToolsWindowsUrl = "https://github.com/GDRETools/gdsdecomp/releases/download/v2.6.3/GDRE_tools-v2.6.3-windows.zip";
constexpr const char *kGdreToolsLinuxUrl = "https://github.com/GDRETools/gdsdecomp/releases/download/v2.6.3/GDRE_tools-v2.6.3-linux.zip";
constexpr const char *kGdreExtractDirectoryName = ".gdre_extract";

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
    temp_path /= ".gddelta_base_" + source_path.filename().string() + "." + std::to_string(timestamp) + ".tmp";
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

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool is_gdre_binary_name(const std::filesystem::path& path) {
    const auto file_name = to_lower_copy(path.filename().string());
#ifdef _WIN32
    return file_name == "gdre_tools.exe";
#else
    return file_name == "gdre_tools" || file_name == "gdre_tools.x86_64" || file_name == "gdre_tools.64";
#endif
}

std::filesystem::path find_gdre_binary(const std::filesystem::path& install_dir) {
    if(!std::filesystem::exists(install_dir)) {
        return {};
    }

    for(const auto& entry : std::filesystem::recursive_directory_iterator(install_dir)) {
        if(!entry.is_regular_file()) {
            continue;
        }

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

void extract_zip_with_shell(const std::filesystem::path& archive_path, const std::filesystem::path& output_dir) {
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
        if(!find_gdre_binary(output_dir).empty()) {
            return;
        }
        Sleep(100);
    }

    throw std::runtime_error("Timed out waiting for GDRE tools extraction.");
}

#endif

}

void CliSupport::set_cli_path(std::filesystem::path cli_path) {
    cli_path_ = std::move(cli_path);
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

void CliSupport::ensure_gdre_tools(const std::filesystem::path& cli_path) {
    const auto install_dir = resolve_tools_directory(cli_path);
    const auto normalized_binary_path = gdre_binary_output_path(install_dir);
    if(std::filesystem::exists(normalized_binary_path)) {
        std::cout << "GDRE tools already installed: " << normalized_binary_path << "\n";
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

    std::cout << "Downloading GDRE tools from " << download_url << "\n";

#ifdef _WIN32
    download_file_with_urlmon(download_url, archive_path);
    extract_zip_with_shell(archive_path, extract_dir);
#else
    download_file_with_libcurl(download_url, archive_path);
    extract_zip_with_minizip(archive_path, extract_dir);
#endif

    copy_directory_contents(extract_dir, install_dir);
    normalize_gdre_layout(install_dir);

    std::filesystem::remove_all(extract_dir, ec);
    std::filesystem::remove(archive_path, ec);

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
    const auto gdre_path = resolve_gdre_tools_path();
    const auto output_path = build_temporary_copy_path("gdre_tools_output.txt");

    std::ostringstream command;
    command << quote_argument(gdre_path.string());
    for(const auto& arg : args) {
        command << " " << quote_argument(arg);
    }
    command << " > " << quote_argument(output_path.string()) << " 2>&1";

    const auto exit_code = std::system(command.str().c_str());

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
    return
        std::to_string(base_reader.header().engine_major) + "." +
        std::to_string(base_reader.header().engine_minor) + "." +
        std::to_string(base_reader.header().engine_patch);
}

void CliSupport::compile_gdscript_files(
    const std::filesystem::path& base_pck,
    const std::vector<std::filesystem::path>& source_files,
    const std::filesystem::path& output_dir
) const {
    if(source_files.empty()) return;
    std::vector<std::string> args = {
        "--headless",
        "--bytecode=" + detect_base_engine_version(base_pck),
        "--output=" + output_dir.string(),
    };
    for(const auto& source_file : source_files) {
        args.push_back("--compile=" + source_file.string());
    }
    static_cast<void>(run_gdre_tools_command(args));
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
        std::error_code ec;
        std::filesystem::remove(temp_base, ec);
        throw;
    }

    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temp_root = std::filesystem::temp_directory_path() / (".gddelta_gdre_patch_" + std::to_string(timestamp));
    const auto compiled_dir = temp_root / "compiled";
    std::filesystem::create_directories(compiled_dir);

    std::vector<std::filesystem::path> gd_files;
    for(const auto& file : files) {
        if(file.removal) {
            std::error_code ec;
            std::filesystem::remove(temp_base, ec);
            std::filesystem::remove_all(temp_root, ec);
            throw std::runtime_error("GDRETools file patch path does not support removal entries.");
        }
        if(file.source_path.extension() == ".gd") {
            gd_files.push_back(file.source_path);
        }
    }

    if(!gd_files.empty()) {
        compile_gdscript_files(base_pck, gd_files, compiled_dir);
    }

    std::vector<std::string> args = {
        "--headless",
        "--pck-patch=" + resolved_base.pack_path.string(),
        "--output=" + output_path.string(),
    };
    if(resolved_base.pack_path.extension() == ".exe" && output_path.extension() == ".exe") {
        args.push_back("--embed=" + resolved_base.pack_path.string());
    }

    for(const auto& file : files) {
        if(file.source_path.extension() == ".gd") {
            const auto gd_entry = base_reader.find_entry("res://" + file.pack_path);
            if(gd_entry.has_value()) {
                args.push_back("--patch-file=" + file.source_path.string() + "=res://" + file.pack_path);
            }

            auto gdc_pack_path = std::filesystem::path(file.pack_path);
            gdc_pack_path.replace_extension(".gdc");
            const auto gdc_entry = base_reader.find_entry("res://" + gdc_pack_path.generic_string());
            if(gdc_entry.has_value()) {
                std::filesystem::path compiled_output_path;
                for(const auto& entry : std::filesystem::recursive_directory_iterator(compiled_dir)) {
                    if(entry.is_regular_file() && entry.path().filename() == gdc_pack_path.filename()) {
                        compiled_output_path = entry.path();
                        break;
                    }
                }
                if(compiled_output_path.empty()) {
                    std::error_code ec;
                    std::filesystem::remove(temp_base, ec);
                    std::filesystem::remove_all(temp_root, ec);
                    throw std::runtime_error("Failed to locate compiled GDScript bytecode output for: " + file.source_path.string());
                }
                args.push_back("--patch-file=" + compiled_output_path.string() + "=res://" + gdc_pack_path.generic_string());
            }
            continue;
        }

        args.push_back("--patch-file=" + file.source_path.string() + "=res://" + file.pack_path);
    }

    try {
        static_cast<void>(run_gdre_tools_command(args));
    } catch(...) {
        std::error_code ec;
        std::filesystem::remove(temp_base, ec);
        std::filesystem::remove_all(temp_root, ec);
        throw;
    }

    std::error_code ec;
    std::filesystem::remove(temp_base, ec);
    std::filesystem::remove_all(temp_root, ec);
}

BaseInputPaths CliSupport::resolve_base_input(const std::filesystem::path& base_path) const {
    BaseInputPaths resolved;
    resolved.requested_path = base_path;
    resolved.pack_path = base_path;

    if(base_path.extension() != ".exe") {
        return resolved;
    }

    if(gddelta::pck::EmbeddedPckHandler::find_embedded_pck(base_path).has_value()) {
        return resolved;
    }

    const auto sibling_pck = base_path.parent_path() / (base_path.stem().string() + ".pck");
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
    if(base_reader.header().engine_major < 4) {
        throw std::runtime_error(
            "Godot 3.x is not supported. Base pack engine version is " +
            std::to_string(base_reader.header().engine_major) + "." +
            std::to_string(base_reader.header().engine_minor) + "." +
            std::to_string(base_reader.header().engine_patch)
        );
    }
    return base_reader;
}

gddelta::pck::PckWriteOptions CliSupport::build_pack_options_from_base(const std::filesystem::path& base_pck) const {
    const auto temp_copy = create_temporary_base_copy(base_pck);
    gddelta::pck::PckWriteOptions options;
    std::optional<std::string> version_error;
    {
        gddelta::pck::PckReader base_reader;
        base_reader.open(temp_copy);
        if(base_reader.header().engine_major < 4) {
            version_error = 
                "Godot 3.x is not supported. Base pack engine version is " +
                std::to_string(base_reader.header().engine_major) + "." +
                std::to_string(base_reader.header().engine_minor) + "." +
                std::to_string(base_reader.header().engine_patch);
        } else {
            options.format_version = base_reader.header().format_version;
            options.engine_major = base_reader.header().engine_major;
            options.engine_minor = base_reader.header().engine_minor;
            options.engine_patch = base_reader.header().engine_patch;
        }
    }

    std::error_code ec;
    std::filesystem::remove(temp_copy, ec);
    if(version_error.has_value()) {
        throw std::runtime_error(*version_error);
    }

    return options;
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

        if(!entry.is_regular_file()) {
            continue;
        }

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
