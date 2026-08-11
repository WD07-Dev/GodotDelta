#include "pck_reader.h"
#include "pck_embedded.h"
#include<algorithm>
#include<array>
#include<cstring>
#include<fstream>
#include<stdexcept>
#include<vector>
using namespace gddelta::pck;

namespace {
    template <typename T>
    T read_scalar(std::istream& stream) {
        T value {};
        stream.read(reinterpret_cast<char *>(&value), sizeof(T));
        if(!stream) {
            throw std::runtime_error("Failed to read scalar from PCK stream.");
        }
        return value;
    }
    
    std::string read_string(std::istream& stream, std::uint32_t packed_size) {
        std::vector<char> bytes(packed_size);
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if(!stream) {
            throw std::runtime_error("Failed to read path from PCK stream.");
        }
        
        const auto end = std::find(bytes.begin(), bytes.end(), '\0');
        return std::string(bytes.begin(), end);
    }
    
    std::uint64_t current_size(std::istream& stream) {
        const auto current = stream.tellg();
        stream.seekg(0, std::ios::end);
        const auto size = stream.tellg();
        stream.seekg(current);
        return static_cast<std::uint64_t>(size);
    }
}

std::uint64_t gddelta::pck::align_u64(std::uint64_t value, std::uint64_t alignment) {
    if(alignment == 0) return value;
    const auto remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

void PckReader::open(const std::filesystem::path& path) {
    path_ = path;
    header_ = {};
    entries_.clear();

    std::ifstream stream(path, std::ios::binary);
    if(!stream) {
        throw std::runtime_error("Failed to open PCK file: " + path.string());
    }

    const auto magic = read_scalar<std::uint32_t>(stream);
    if(magic == kPackHeaderMagic) {
        open_stream(stream, path, 0);
        return;
    }

    const auto embedded = EmbeddedPckHandler::find_embedded_pck(path);
    if(!embedded.has_value()) {
        throw std::runtime_error("Unsupported input. Expected a standalone PCK or executable with embedded PCK.");
    }

    stream.clear();
    open_stream(stream, path, embedded->actual_offset);
}

void PckReader::open_stream(std::ifstream& stream, const std::filesystem::path& path, std::uint64_t pck_start_offset) {
    header_ = {};
    entries_.clear();

    const auto pck_size = current_size(stream);
    stream.seekg(static_cast<std::streamoff>(pck_start_offset), std::ios::beg);

    const auto magic = read_scalar<std::uint32_t>(stream);
    if(magic != kPackHeaderMagic) {
        throw std::runtime_error("Invalid PCK header magic at detected offset for: " + path.string());
    }

    header_.pck_start_offset = pck_start_offset;
    header_.format_version = read_scalar<std::uint32_t>(stream);
    header_.engine_major = read_scalar<std::uint32_t>(stream);
    header_.engine_minor = read_scalar<std::uint32_t>(stream);
    header_.engine_patch = read_scalar<std::uint32_t>(stream);

    if(header_.format_version > kCurrentPackFormatVersion) {
        throw std::runtime_error("Unsupported PCK format version: " + std::to_string(header_.format_version));
    }

    if(header_.format_version >= kPackFormatVersion2) {
        header_.pack_flags = read_scalar<std::uint32_t>(stream);
        header_.file_base = read_scalar<std::uint64_t>(stream);
        if(header_.format_version >= kPackFormatVersion3) {
            header_.directory_offset = read_scalar<std::uint64_t>(stream);
        }
    }

    if(header_.format_version >= kPackFormatVersion3 || (header_.format_version == kPackFormatVersion2 && (header_.pack_flags & kPackRelFileBase) != 0)) {
        header_.file_base += pck_start_offset;
    }

    for(int i=0; i < 16; ++i) {
        static_cast<void>(read_scalar<std::uint32_t>(stream));
    }

    if(header_.format_version >= kPackFormatVersion3) {
        header_.directory_offset += pck_start_offset;
        if(header_.directory_offset >= pck_size) {
            throw std::runtime_error("PCK directory offset is out of bounds.");
        }
        stream.seekg(static_cast<std::streamoff>(header_.directory_offset), std::ios::beg);
    }

    header_.file_count = read_scalar<std::uint32_t>(stream);
    entries_.reserve(header_.file_count);

    for(std::uint32_t i=0; i < header_.file_count; ++i) {
        PckEntry entry;
        const auto stored_length = read_scalar<std::uint32_t>(stream);
        entry.path = read_string(stream, stored_length);

        const auto relative_offset = read_scalar<std::uint64_t>(stream);
        entry.size = read_scalar<std::uint64_t>(stream);
        stream.read(reinterpret_cast<char *>(entry.md5.data()), static_cast<std::streamsize>(entry.md5.size()));
        if(!stream) {
            throw std::runtime_error("Failed to read PCK entry hash.");
        }

        if(header_.format_version >= kPackFormatVersion2) {
            entry.flags = read_scalar<std::uint32_t>(stream);
        }

        entry.offset = header_.file_base + relative_offset;
        if(entry.offset > pck_size || entry.size > pck_size - entry.offset) {
            throw std::runtime_error("PCK entry is out of bounds: " + entry.path);
        }
        entries_.push_back(std::move(entry));
    }
}

const std::filesystem::path& PckReader::path() const noexcept {
    return path_;
}

const PckHeader& PckReader::header() const noexcept {
    return header_;
}

const std::vector<PckEntry>& PckReader::entries() const noexcept {
    return entries_;
}

std::optional<PckEntry> PckReader::find_entry(std::string_view pack_path) const {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const PckEntry &entry) {
        return entry.path == pack_path;
    });

    if(it == entries_.end()) {
        return std::nullopt;
    }else return *it;
}

std::vector<std::uint8_t> PckReader::read_entry_data(const PckEntry& entry) const {
    std::ifstream stream(path_, std::ios::binary);
    if(!stream) {
        throw std::runtime_error("Failed to reopen PCK file: " + path_.string());
    }

    stream.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(entry.size));
    stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if(!stream) {
        throw std::runtime_error("Failed to read PCK entry data: " + entry.path);
    }
    return bytes;
}

bool PckReader::entry_matches_file(const PckEntry& entry, const std::filesystem::path& file_path) const {
    std::error_code ec;
    const auto source_size = std::filesystem::file_size(file_path, ec);
    if(ec) {
        throw std::runtime_error("Failed to stat project file for comparison: " + file_path.string());
    }
    if(entry.size != source_size) {
        return false;
    }

    std::ifstream pck_stream(path_, std::ios::binary);
    if(!pck_stream) {
        throw std::runtime_error("Failed to reopen PCK file: " + path_.string());
    }

    std::ifstream source_stream(file_path, std::ios::binary);
    if(!source_stream) {
        throw std::runtime_error("Failed to open project file for comparison: " + file_path.string());
    }

    pck_stream.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
    if(!pck_stream) {
        throw std::runtime_error("Failed to seek PCK entry data: " + entry.path);
    }

    std::array<char, 64 * 1024> pck_buffer {};
    std::array<char, 64 * 1024> source_buffer {};
    auto remaining = entry.size;

    while(remaining > 0) {
        const auto chunk_size = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, pck_buffer.size()));

        pck_stream.read(pck_buffer.data(), static_cast<std::streamsize>(chunk_size));
        if(static_cast<std::size_t>(pck_stream.gcount()) != chunk_size) {
            throw std::runtime_error("Failed to read PCK entry data: " + entry.path);
        }

        source_stream.read(source_buffer.data(), static_cast<std::streamsize>(chunk_size));
        if(static_cast<std::size_t>(source_stream.gcount()) != chunk_size) {
            throw std::runtime_error("Failed to read project file for comparison: " + file_path.string());
        }

        if(std::memcmp(pck_buffer.data(), source_buffer.data(), chunk_size) != 0) {
            return false;
        }

        remaining -= static_cast<std::uint64_t>(chunk_size);
    }

    return true;
}