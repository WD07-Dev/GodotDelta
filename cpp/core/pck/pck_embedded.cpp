#include "pck_embedded.h"
#include "pck_format.h"
#include "core/common/path_utils.h"
#include<array>
#include<cstring>
#include<fstream>
#include<string>
#include<vector>
#include<stdexcept>
using namespace gddelta::pck;

namespace {
    template <typename T>
    T read_scalar(std::istream& stream) {
        T value {};
        stream.read(reinterpret_cast<char *>(&value), sizeof(T));
        if(!stream) {
            throw std::runtime_error("Failed to read executable header field.");
        }
        return value;
    }
    
    std::uint64_t stream_size(std::istream& stream) {
        const auto current = stream.tellg();
        stream.seekg(0, std::ios::end);
        const auto size = stream.tellg();
        stream.seekg(current);
        return static_cast<std::uint64_t>(size);
    }
    
    void copy_stream_range(std::istream& input, std::ostream& output, std::uint64_t count) {
        std::array<char, 64 * 1024> buffer {};
        std::uint64_t remaining = count;
        while (remaining > 0) {
            const auto chunk = std::min<std::uint64_t>(remaining, buffer.size());
            input.read(buffer.data(), static_cast<std::streamsize>(chunk));
            const auto read_count = input.gcount();
            if(read_count <= 0) {
                throw std::runtime_error("Failed to copy executable payload.");
            }
            output.write(buffer.data(), read_count);
            if(!output) {
                throw std::runtime_error("Failed to write executable payload.");
            }
            remaining -= static_cast<std::uint64_t>(read_count);
        }
    }
    
    template <typename T>
    void write_scalar(std::ostream& stream, T value) {
        stream.write(reinterpret_cast<const char *>(&value), sizeof(T));
        if(!stream) {
            throw std::runtime_error("Failed to write executable header field.");
        }
    }
    
    void write_padding(std::ostream& stream, std::uint64_t count) {
        static constexpr std::array<char, 32> zeroes {};
        while (count > 0) {
            const auto chunk = std::min<std::uint64_t>(count, zeroes.size());
            stream.write(zeroes.data(), static_cast<std::streamsize>(chunk));
            if(!stream) {
                throw std::runtime_error("Failed to write embedded PCK padding.");
            }
            count -= chunk;
        }
    }
    
    template <typename T>
    void patch_scalar(std::vector<char>& bytes, std::uint64_t offset, T value) {
        if(offset + sizeof(T) > bytes.size()) {
            throw std::runtime_error("Executable header patch is out of bounds.");
        }
        std::memcpy(bytes.data() + offset, &value, sizeof(T));
    }
    
    bool seek_after_elf_magic(std::istream& stream) {
        stream.seekg(0, std::ios::beg);
        return read_scalar<std::uint32_t>(stream) == 0x464c457fU;
    }
    
    bool seek_after_pe_magic(std::istream& stream) {
        stream.seekg(0x3c, std::ios::beg);
        const auto pe_offset = read_scalar<std::uint32_t>(stream);
        if(pe_offset > stream_size(stream)) return false;
        stream.seekg(static_cast<std::streamoff>(pe_offset), std::ios::beg);
        return read_scalar<std::uint32_t>(stream) == 0x00004550U;
    }
    
    std::optional<std::uint64_t> find_pe_pck_section_header_offset(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        if(!stream || !seek_after_pe_magic(stream)) return std::nullopt;
        
        const auto header_position = static_cast<std::uint64_t>(stream.tellg());
        stream.seekg(static_cast<std::streamoff>(header_position + 2), std::ios::beg);
        const auto section_count = read_scalar<std::uint16_t>(stream);
        stream.seekg(static_cast<std::streamoff>(header_position + 16), std::ios::beg);
        const auto optional_header_size = read_scalar<std::uint16_t>(stream);
        stream.seekg(static_cast<std::streamoff>(header_position + 24 + optional_header_size), std::ios::beg);
        const auto section_table_position = static_cast<std::uint64_t>(stream.tellg());
        
        for(std::uint16_t index = 0; index < section_count; ++index) {
            const auto section_header_position = section_table_position + static_cast<std::uint64_t>(index) * 40U;
            stream.seekg(static_cast<std::streamoff>(section_header_position), std::ios::beg);
            std::array<char, 9> section_name {};
            stream.read(section_name.data(), 8);
            if(!stream) return std::nullopt;

            if(std::strcmp(section_name.data(), "pck") == 0) {
                return section_header_position;
            }
        }
        
        return std::nullopt;
    }
    
    std::optional<EmbeddedPckInfo> get_elf_section_info(std::istream& stream) {
        if(!seek_after_elf_magic(stream)) return std::nullopt;
        
        EmbeddedPckInfo info;
        info.executable_type = EmbeddedPckInfo::ExecutableType::elf;
        info.section_bit_size = static_cast<std::uint32_t>(read_scalar<std::uint8_t>(stream)) * 32U;
        
        std::uint64_t section_table_position = 0;
        std::uint64_t section_header_size = 0;
        if(info.section_bit_size == 32) {
            section_header_size = 40;
            stream.seekg(0x20, std::ios::beg);
            section_table_position = read_scalar<std::uint32_t>(stream);
            stream.seekg(0x30, std::ios::beg);
        }else {
            section_header_size = 64;
            stream.seekg(0x28, std::ios::beg);
            section_table_position = read_scalar<std::uint64_t>(stream);
            stream.seekg(0x3c, std::ios::beg);
        }
        
        const auto section_count = read_scalar<std::uint16_t>(stream);
        const auto string_section_index = read_scalar<std::uint16_t>(stream);
        stream.seekg(static_cast<std::streamoff>(section_table_position + string_section_index * section_header_size), std::ios::beg);
        
        std::uint64_t string_data_position = 0;
        std::uint64_t string_data_size = 0;
        if(info.section_bit_size == 32) {
            stream.seekg(0x10, std::ios::cur);
            string_data_position = read_scalar<std::uint32_t>(stream);
            string_data_size = read_scalar<std::uint32_t>(stream);
        }else {
            stream.seekg(0x18, std::ios::cur);
            string_data_position = read_scalar<std::uint64_t>(stream);
            string_data_size = read_scalar<std::uint64_t>(stream);
        }
        
        stream.seekg(static_cast<std::streamoff>(string_data_position), std::ios::beg);
        std::vector<char> strings(static_cast<std::size_t>(string_data_size));
        stream.read(strings.data(), static_cast<std::streamsize>(strings.size()));
        if(!stream) {
            throw std::runtime_error("Failed to read ELF string table.");
        }
        
        for(std::uint16_t index = 0; index < section_count; ++index) {
            const auto header_position = section_table_position + static_cast<std::uint64_t>(index) * section_header_size;
            stream.seekg(static_cast<std::streamoff>(header_position), std::ios::beg);
            const auto name_offset = read_scalar<std::uint32_t>(stream);
            if(name_offset >= strings.size() || std::strcmp(strings.data() + name_offset, "pck") != 0) continue;
            
            info.section_header_offset = header_position;
            if(info.section_bit_size == 32) {
                stream.seekg(static_cast<std::streamoff>(header_position + 0x10), std::ios::beg);
                info.embedded_offset = read_scalar<std::uint32_t>(stream);
                info.embedded_size = read_scalar<std::uint32_t>(stream);
            }else {
                stream.seekg(static_cast<std::streamoff>(header_position + 0x18), std::ios::beg);
                info.embedded_offset = read_scalar<std::uint64_t>(stream);
                info.embedded_size = read_scalar<std::uint64_t>(stream);
            }
            return info;
        }
        
        return std::nullopt;
    }
    
    std::optional<EmbeddedPckInfo> get_pe_section_info(std::istream& stream) {
        if(!seek_after_pe_magic(stream)) return std::nullopt;
        EmbeddedPckInfo info;
        info.executable_type = EmbeddedPckInfo::ExecutableType::pe;
        
        const auto header_position = static_cast<std::uint64_t>(stream.tellg());
        stream.seekg(static_cast<std::streamoff>(header_position + 2), std::ios::beg);
        const auto section_count = read_scalar<std::uint16_t>(stream);
        stream.seekg(static_cast<std::streamoff>(header_position + 16), std::ios::beg);
        const auto optional_header_size = read_scalar<std::uint16_t>(stream);
        stream.seekg(static_cast<std::streamoff>(header_position + 24 + optional_header_size), std::ios::beg);
        const auto section_table_position = static_cast<std::uint64_t>(stream.tellg());
        
        for(std::uint16_t index = 0; index < section_count; ++index) {
            const auto section_header_position = section_table_position + static_cast<std::uint64_t>(index) * 40U;
            stream.seekg(static_cast<std::streamoff>(section_header_position), std::ios::beg);
            
            std::array<char, 9> section_name {};
            stream.read(section_name.data(), 8);
            if(!stream) {
                throw std::runtime_error("Failed to read PE section header.");
            }
            if(std::strcmp(section_name.data(), "pck") != 0) continue;
            
            info.section_header_offset = section_header_position;
            stream.seekg(static_cast<std::streamoff>(section_header_position + 16), std::ios::beg);
            info.embedded_size = read_scalar<std::uint32_t>(stream);
            stream.seekg(static_cast<std::streamoff>(section_header_position + 20), std::ios::beg);
            info.embedded_offset = read_scalar<std::uint32_t>(stream);
            return info;
        }
        
        return std::nullopt;
    }
    
    std::optional<EmbeddedPckInfo> detect_self_contained_pck(std::istream& stream) {
        const auto total_size = stream_size(stream);
        if(total_size < 12) return std::nullopt;
        
        stream.seekg(-4, std::ios::end);
        if(read_scalar<std::uint32_t>(stream) != kPackHeaderMagic) return std::nullopt;
        
        stream.seekg(-12, std::ios::end);
        EmbeddedPckInfo info;
        info.actual_size = read_scalar<std::uint64_t>(stream);
        const auto expected_embedded_size = info.actual_size + 12;
        if(expected_embedded_size > total_size) {
            return std::nullopt;
        }
        
        const auto expected_offset = total_size - expected_embedded_size;
        for(std::uint64_t padding = 0; padding <= 8 && padding <= expected_offset; ++padding) {
            info.actual_offset = expected_offset - padding;
            stream.seekg(static_cast<std::streamoff>(info.actual_offset), std::ios::beg);
            if(read_scalar<std::uint32_t>(stream) != kPackHeaderMagic) continue;
            info.embedded_offset = info.actual_offset;
            info.embedded_size = total_size - info.embedded_offset;
            return info;
        }
        
        return std::nullopt;
    }
}    

bool EmbeddedPckHandler::is_probable_windows_executable(const std::filesystem::path& path) {
    const auto extension = gddelta::common::path_to_utf8(path.extension());
    return extension == ".exe" || extension == ".dll";
}

std::optional<EmbeddedPckInfo> EmbeddedPckHandler::find_embedded_pck(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if(!stream) {
        throw std::runtime_error("Failed to open executable for embedded PCK scan: " + path.string());
    }

    auto info = EmbeddedPckHandler::is_probable_windows_executable(path) ? get_pe_section_info(stream) : get_elf_section_info(stream);
    if(info.has_value() && info->embedded_offset != 0) {
        info->actual_offset = info->embedded_offset;
        for(int i=0; i < 8; ++i) {
            stream.seekg(static_cast<std::streamoff>(info->actual_offset), std::ios::beg);
            if(read_scalar<std::uint32_t>(stream) == kPackHeaderMagic) {
                stream.seekg(static_cast<std::streamoff>(info->embedded_offset + info->embedded_size - 4), std::ios::beg);
                if(read_scalar<std::uint32_t>(stream) == kPackHeaderMagic) {
                    stream.seekg(static_cast<std::streamoff>(info->embedded_offset + info->embedded_size - 12), std::ios::beg);
                    info->actual_size = read_scalar<std::uint64_t>(stream);
                    return info;
                }
                break;
            }
            ++info->actual_offset;
        }
    }

    return detect_self_contained_pck(stream);
}

void EmbeddedPckHandler::embed_pck_into_executable(
    const std::filesystem::path& base_executable,
    const std::filesystem::path& source_pck,
    const std::filesystem::path& output_executable
) {
    const auto embedded = EmbeddedPckHandler::find_embedded_pck(base_executable);

    std::ifstream base_stream(base_executable, std::ios::binary);
    if(!base_stream) {
        throw std::runtime_error("Failed to open base executable: " + base_executable.string());
    }

    std::ifstream pck_stream(source_pck, std::ios::binary);
    if(!pck_stream) {
        throw std::runtime_error("Failed to open source PCK for embedding: " + source_pck.string());
    }

    auto temp_output = output_executable;
    temp_output += ".tmp";
    std::ofstream output(temp_output, std::ios::binary | std::ios::trunc);
    if(!output) {
        throw std::runtime_error("Failed to open output executable: " + output_executable.string());
    }

    const auto base_size = stream_size(base_stream);
    const auto source_size = stream_size(pck_stream);
    const auto executable_prefix_size = embedded.has_value() ? embedded->embedded_offset : base_size;

    base_stream.seekg(0, std::ios::beg);
    copy_stream_range(base_stream, output, executable_prefix_size);

    const auto embedded_start = static_cast<std::uint64_t>(output.tellp());
    const auto aligned_embedded_start = align_u64(embedded_start, 8);
    write_padding(output, aligned_embedded_start - embedded_start);

    pck_stream.seekg(0, std::ios::beg);
    copy_stream_range(pck_stream, output, source_size);

    const auto embedded_payload_size = static_cast<std::uint64_t>(output.tellp()) - aligned_embedded_start;
    const auto aligned_embedded_end = align_u64(embedded_payload_size + 12, 8);
    write_padding(output, aligned_embedded_end - (embedded_payload_size + 12));
    write_scalar<std::uint64_t>(output, source_size);
    write_scalar<std::uint32_t>(output, kPackHeaderMagic);

    const auto embedded_size = static_cast<std::uint64_t>(output.tellp()) - aligned_embedded_start;

    output.flush();
    output.close();

    std::error_code ec;
    std::filesystem::rename(temp_output, output_executable, ec);
    if(ec) {
        std::filesystem::remove(output_executable, ec);
        ec.clear();
        std::filesystem::rename(temp_output, output_executable, ec);
    }
    if(ec) {
        throw std::runtime_error("Failed to move generated executable into place: " + output_executable.string());
    }

    static_cast<void>(aligned_embedded_start);
    static_cast<void>(embedded_size);
}

void EmbeddedPckHandler::fixup_embedded_executable_headers(
    const std::filesystem::path& template_executable,
    const std::filesystem::path& output_executable
) {
    const auto template_info = EmbeddedPckHandler::find_embedded_pck(template_executable);
    const auto pe_section_header_offset = EmbeddedPckHandler::is_probable_windows_executable(template_executable)
    ? find_pe_pck_section_header_offset(template_executable) : std::nullopt;

    std::optional<EmbeddedPckInfo> output_info;
    {
        std::ifstream output_stream(output_executable, std::ios::binary);
        if(!output_stream) {
            throw std::runtime_error("Failed to open output executable for footer scan: " + output_executable.string());
        }
        output_info = detect_self_contained_pck(output_stream);
    }

    const auto section_header_offset = pe_section_header_offset.has_value()
    ? *pe_section_header_offset : (template_info.has_value() ? template_info->section_header_offset : 0);

    if(!template_info.has_value() || section_header_offset == 0 || !output_info.has_value()) return;

    std::ifstream input(output_executable, std::ios::binary);
    if(!input) {
        throw std::runtime_error("Failed to reopen generated executable for header patching: " + output_executable.string());
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();

    if(template_info->executable_type == EmbeddedPckInfo::ExecutableType::pe) {
        patch_scalar<std::uint32_t>(bytes, section_header_offset + 8, 8);
        patch_scalar<std::uint32_t>(bytes, section_header_offset + 16, static_cast<std::uint32_t>(output_info->embedded_size));
        patch_scalar<std::uint32_t>(bytes, section_header_offset + 20, static_cast<std::uint32_t>(output_info->embedded_offset));
    }else if(template_info->section_bit_size == 32) {
        patch_scalar<std::uint32_t>(bytes, section_header_offset + 0x10, static_cast<std::uint32_t>(output_info->embedded_offset));
        patch_scalar<std::uint32_t>(bytes, section_header_offset + 0x14, static_cast<std::uint32_t>(output_info->embedded_size));
    }else {
        patch_scalar<std::uint64_t>(bytes, section_header_offset + 0x18, output_info->embedded_offset);
        patch_scalar<std::uint64_t>(bytes, section_header_offset + 0x20, output_info->embedded_size);
    }

    auto temp_path = output_executable;
    temp_path += ".fixup.tmp";
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
    if(!output) {
        throw std::runtime_error("Failed to open temporary executable for header patching: " + temp_path.string());
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if(!output) {
        throw std::runtime_error("Failed to write patched executable: " + output_executable.string());
    }
    output.close();

    std::error_code ec;
    std::filesystem::rename(temp_path, output_executable, ec);
    if(ec) {
        std::filesystem::remove(output_executable, ec);
        ec.clear();
        std::filesystem::rename(temp_path, output_executable, ec);
    }
    if(ec) {
        throw std::runtime_error("Failed to replace executable after header patch: " + output_executable.string());
    }
}