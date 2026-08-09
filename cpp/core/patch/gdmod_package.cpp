#include "gdmod_package.h"
#include "core/pck/pck_reader.h"
#include "core/pck/pck_writer.h"
#include<algorithm>
#include<array>
#include<cctype>
#include<cstddef>
#include<cstdint>
#include<fstream>
#include<iomanip>
#include<openssl/evp.h>
#include<openssl/hmac.h>
#include<openssl/kdf.h>
#include<openssl/rand.h>
#include<optional>
#include<span>
#include<sstream>
#include<stdexcept>
#include<string>
#include<string_view>

using namespace gddelta::patch;

namespace {
constexpr const char *kGdmodManifestPackPath = "res://.gddelta/gdmod.meta";
constexpr const char *kGdmodPayloadPackPath = "res://.gddelta/payload.bin";
constexpr const char *kGdmodSlotPackPathPrefix = "res://.gddelta/slots/";
constexpr std::uint32_t kGdmodManifestVersion1 = 1;
constexpr std::uint32_t kGdmodManifestVersion2 = 2;
constexpr std::uint32_t kEncryptedBlobVersion = 1;
constexpr std::size_t kPayloadKeySize = 32;
constexpr std::size_t kGcmNonceSize = 12;
constexpr std::size_t kGcmTagSize = 16;
constexpr std::size_t kSharePlaintextSize = 1 + kPayloadKeySize;

using ByteVector = std::vector<std::uint8_t>;

std::array<std::uint64_t, 256> build_gear_table() {
    std::array<std::uint64_t, 256> table{};
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    for(auto& value : table) {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        value = state * 0x2545f4914f6cdd1dULL;
    }
    return table;
}

const std::array<std::uint64_t, 256>& gear_table() {
    static const auto table = build_gear_table();
    return table;
}

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::vector<std::string> split_csv(std::string_view value) {
    std::vector<std::string> parts;
    std::string current;
    for(const auto character : value) {
        if(character == ',') {
            if(!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(character);
    }
    if(!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::string join_csv(const std::vector<std::string>& parts) {
    std::ostringstream output;
    for(std::size_t index = 0; index < parts.size(); ++index) {
        if(index != 0) {
            output << ",";
        }
        output << parts[index];
    }
    return output.str();
}

std::string encode_chunk_id(std::uint64_t hash) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::uint64_t fnv1a64_append(std::uint64_t hash, std::string_view value) {
    for(const auto character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t fnv1a64_append(std::uint64_t hash, std::uint64_t value) {
    for(int index = 0; index < 8; ++index) {
        hash ^= static_cast<std::uint8_t>((value >> (index * 8)) & 0xffU);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string build_chunk_slot_id(
    std::string_view source_path,
    std::uint64_t source_offset,
    std::uint64_t size
) {
    std::uint64_t hash = 1469598103934665603ULL;
    hash = fnv1a64_append(hash, "gdmod-threshold-chunk-id");
    hash = fnv1a64_append(hash, source_path);
    hash = fnv1a64_append(hash, source_offset);
    hash = fnv1a64_append(hash, size);
    return encode_chunk_id(hash);
}

std::string build_slot_pack_path(std::size_t index) {
    std::ostringstream output;
    output << kGdmodSlotPackPathPrefix << std::setw(3) << std::setfill('0') << index << ".share";
    return output.str();
}

void append_bytes(ByteVector& output, std::string_view value) {
    output.insert(output.end(), value.begin(), value.end());
}

void append_u32_le(ByteVector& output, std::uint32_t value) {
    for(int index = 0; index < 4; ++index) {
        output.push_back(static_cast<std::uint8_t>((value >> (index * 8)) & 0xffU));
    }
}

std::uint32_t read_u32_le(const ByteVector& input, std::size_t offset) {
    return
        static_cast<std::uint32_t>(input[offset]) |
        (static_cast<std::uint32_t>(input[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>(input[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>(input[offset + 3]) << 24U);
}

ByteVector random_bytes(std::size_t size) {
    ByteVector bytes(size);
    if(RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("Failed to generate cryptographic random bytes.");
    }
    return bytes;
}

ByteVector sha256_bytes(std::string_view input) {
    ByteVector digest(EVP_MD_size(EVP_sha256()));
    unsigned int digest_size = 0;
    if(EVP_Digest(
        input.data(),
        input.size(),
        digest.data(),
        &digest_size,
        EVP_sha256(),
        nullptr
    ) != 1) {
        throw std::runtime_error("Failed to compute SHA-256 digest.");
    }
    digest.resize(digest_size);
    return digest;
}

ByteVector sha256_bytes(const ByteVector& input) {
    return sha256_bytes(std::string_view(
        reinterpret_cast<const char *>(input.data()),
        input.size()
    ));
}

ByteVector hkdf_sha256(
    const ByteVector& ikm,
    std::string_view salt,
    std::string_view info,
    std::size_t output_size
) {
    auto *context = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if(context == nullptr) {
        throw std::runtime_error("Failed to create HKDF context.");
    }

    const auto cleanup = [&]() {
        EVP_PKEY_CTX_free(context);
    };

    if(EVP_PKEY_derive_init(context) != 1 ||
        EVP_PKEY_CTX_set_hkdf_md(context, EVP_sha256()) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_salt(
            context,
            reinterpret_cast<const unsigned char *>(salt.data()),
            static_cast<int>(salt.size())
        ) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_key(context, ikm.data(), static_cast<int>(ikm.size())) != 1 ||
        EVP_PKEY_CTX_add1_hkdf_info(
            context,
            reinterpret_cast<const unsigned char *>(info.data()),
            static_cast<int>(info.size())
        ) != 1) {
        cleanup();
        throw std::runtime_error("Failed to configure HKDF.");
    }

    ByteVector output(output_size);
    auto output_length = output.size();
    if(EVP_PKEY_derive(context, output.data(), &output_length) != 1) {
        cleanup();
        throw std::runtime_error("Failed to derive HKDF output.");
    }
    cleanup();
    output.resize(output_length);
    return output;
}

ByteVector derive_chunk_key(const ByteVector& chunk_bytes) {
    const auto digest = sha256_bytes(chunk_bytes);
    return hkdf_sha256(digest, "GodotDeltaChunkSalt", "GodotDeltaChunkKeyV1", kPayloadKeySize);
}

ByteVector encrypt_aes_256_gcm(const ByteVector& key, const ByteVector& plaintext) {
    const auto nonce = random_bytes(kGcmNonceSize);
    ByteVector ciphertext(plaintext.size() + kGcmTagSize + 8);

    auto *context = EVP_CIPHER_CTX_new();
    if(context == nullptr) {
        throw std::runtime_error("Failed to create AES-GCM context.");
    }
    const auto cleanup = [&]() {
        EVP_CIPHER_CTX_free(context);
    };

    int output_length = 0;
    int total_length = 0;
    ByteVector encrypted(plaintext.size());
    if(EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_EncryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) != 1 ||
        EVP_EncryptUpdate(context, encrypted.data(), &output_length, plaintext.data(), static_cast<int>(plaintext.size())) != 1) {
        cleanup();
        throw std::runtime_error("Failed to encrypt AES-GCM payload.");
    }
    total_length = output_length;
    if(EVP_EncryptFinal_ex(context, encrypted.data() + total_length, &output_length) != 1) {
        cleanup();
        throw std::runtime_error("Failed to finalize AES-GCM payload.");
    }
    total_length += output_length;
    encrypted.resize(total_length);

    ByteVector tag(kGcmTagSize);
    if(EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) != 1) {
        cleanup();
        throw std::runtime_error("Failed to extract AES-GCM tag.");
    }
    cleanup();

    ByteVector blob;
    append_bytes(blob, "GDEM");
    append_u32_le(blob, kEncryptedBlobVersion);
    append_u32_le(blob, static_cast<std::uint32_t>(nonce.size()));
    append_u32_le(blob, static_cast<std::uint32_t>(tag.size()));
    append_u32_le(blob, static_cast<std::uint32_t>(encrypted.size()));
    blob.insert(blob.end(), nonce.begin(), nonce.end());
    blob.insert(blob.end(), tag.begin(), tag.end());
    blob.insert(blob.end(), encrypted.begin(), encrypted.end());
    return blob;
}

ByteVector decrypt_aes_256_gcm(const ByteVector& key, const ByteVector& blob) {
    if(blob.size() < 20 || std::string_view(reinterpret_cast<const char *>(blob.data()), 4) != "GDEM") {
        throw std::runtime_error("Invalid encrypted gdmod blob.");
    }

    const auto version = read_u32_le(blob, 4);
    if(version != kEncryptedBlobVersion) {
        throw std::runtime_error("Unsupported encrypted gdmod blob version.");
    }

    const auto nonce_size = read_u32_le(blob, 8);
    const auto tag_size = read_u32_le(blob, 12);
    const auto ciphertext_size = read_u32_le(blob, 16);
    const auto header_size = static_cast<std::size_t>(20);
    const auto required_size = header_size + nonce_size + tag_size + ciphertext_size;
    if(blob.size() < required_size) {
        throw std::runtime_error("Encrypted gdmod blob is truncated.");
    }

    const auto* nonce = blob.data() + header_size;
    const auto* tag = nonce + nonce_size;
    const auto* ciphertext = tag + tag_size;

    auto *context = EVP_CIPHER_CTX_new();
    if(context == nullptr) {
        throw std::runtime_error("Failed to create AES-GCM context.");
    }
    const auto cleanup = [&]() {
        EVP_CIPHER_CTX_free(context);
    };

    ByteVector plaintext(ciphertext_size);
    int output_length = 0;
    int total_length = 0;
    if(EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce_size), nullptr) != 1 ||
        EVP_DecryptInit_ex(context, nullptr, nullptr, key.data(), nonce) != 1 ||
        EVP_DecryptUpdate(context, plaintext.data(), &output_length, ciphertext, static_cast<int>(ciphertext_size)) != 1) {
        cleanup();
        throw std::runtime_error("Failed to decrypt AES-GCM payload.");
    }
    total_length = output_length;

    if(EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag_size), const_cast<std::uint8_t *>(tag)) != 1) {
        cleanup();
        throw std::runtime_error("Failed to set AES-GCM tag.");
    }

    if(EVP_DecryptFinal_ex(context, plaintext.data() + total_length, &output_length) != 1) {
        cleanup();
        throw std::runtime_error("AES-GCM authentication failed.");
    }
    total_length += output_length;
    cleanup();
    plaintext.resize(total_length);
    return plaintext;
}

std::array<std::uint8_t, 512> gf256_exp_table;
std::array<std::uint8_t, 256> gf256_log_table;

struct Gf256TablesInitializer {
    Gf256TablesInitializer() {
        std::uint16_t value = 1;
        for(int index = 0; index < 255; ++index) {
            gf256_exp_table[index] = static_cast<std::uint8_t>(value);
            gf256_log_table[gf256_exp_table[index]] = static_cast<std::uint8_t>(index);
            value <<= 1U;
            if((value & 0x100U) != 0) {
                value ^= 0x11bU;
            }
        }
        for(int index = 255; index < 512; ++index) {
            gf256_exp_table[index] = gf256_exp_table[index - 255];
        }
    }
} gf256_tables_initializer;

std::uint8_t gf256_mul(std::uint8_t lhs, std::uint8_t rhs) {
    if(lhs == 0 || rhs == 0) {
        return 0;
    }
    return gf256_exp_table[gf256_log_table[lhs] + gf256_log_table[rhs]];
}

std::uint8_t gf256_div(std::uint8_t lhs, std::uint8_t rhs) {
    if(rhs == 0) {
        throw std::runtime_error("GF(256) division by zero.");
    }
    if(lhs == 0) {
        return 0;
    }
    const auto lhs_log = static_cast<int>(gf256_log_table[lhs]);
    const auto rhs_log = static_cast<int>(gf256_log_table[rhs]);
    auto diff = lhs_log - rhs_log;
    if(diff < 0) {
        diff += 255;
    }
    return gf256_exp_table[diff];
}

std::vector<ByteVector> split_secret_shares(
    const ByteVector& secret,
    std::uint32_t share_count,
    std::uint32_t threshold
) {
    if(secret.empty() || share_count == 0 || threshold == 0 || threshold > share_count || share_count > 255) {
        throw std::runtime_error("Invalid secret sharing parameters.");
    }

    std::vector<ByteVector> shares(share_count, ByteVector(secret.size() + 1));
    for(std::uint32_t share_index = 0; share_index < share_count; ++share_index) {
        shares[share_index][0] = static_cast<std::uint8_t>(share_index + 1);
    }

    for(std::size_t byte_index = 0; byte_index < secret.size(); ++byte_index) {
        const auto coefficients = random_bytes(threshold - 1);
        for(std::uint32_t share_index = 0; share_index < share_count; ++share_index) {
            const auto x = static_cast<std::uint8_t>(share_index + 1);
            std::uint8_t y = secret[byte_index];
            std::uint8_t x_power = 1;
            for(std::uint32_t coefficient_index = 0; coefficient_index < threshold - 1; ++coefficient_index) {
                x_power = gf256_mul(x_power, x);
                y ^= gf256_mul(coefficients[coefficient_index], x_power);
            }
            shares[share_index][byte_index + 1] = y;
        }
    }

    return shares;
}

ByteVector combine_secret_shares(const std::vector<ByteVector>& shares, std::uint32_t threshold) {
    if(shares.size() < threshold || shares.empty()) {
        throw std::runtime_error("Not enough shares to reconstruct secret.");
    }

    const auto share_size = shares.front().size();
    if(share_size < 2) {
        throw std::runtime_error("Invalid share size.");
    }

    ByteVector secret(share_size - 1);
    for(std::size_t byte_index = 1; byte_index < share_size; ++byte_index) {
        std::uint8_t value = 0;
        for(std::uint32_t share_index = 0; share_index < threshold; ++share_index) {
            const auto xi = shares[share_index][0];
            std::uint8_t basis = 1;
            for(std::uint32_t other_index = 0; other_index < threshold; ++other_index) {
                if(share_index == other_index) {
                    continue;
                }
                const auto xj = shares[other_index][0];
                basis = gf256_mul(basis, gf256_div(xj, static_cast<std::uint8_t>(xj ^ xi)));
            }
            value ^= gf256_mul(shares[share_index][byte_index], basis);
        }
        secret[byte_index - 1] = value;
    }

    return secret;
}

ByteVector read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if(!input) {
        throw std::runtime_error("Failed to open file: " + path.string());
    }
    return ByteVector(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void write_file_bytes(const std::filesystem::path& path, const ByteVector& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) {
        throw std::runtime_error("Failed to write file: " + path.string());
    }
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if(!output) {
        throw std::runtime_error("Failed to write file bytes: " + path.string());
    }
}

ByteVector read_chunk_from_base_pack(
    const std::filesystem::path& base_pck,
    const GdmodThresholdChunkSlot& slot
) {
    gddelta::pck::PckReader reader;
    reader.open(base_pck);
    const auto entry = reader.find_entry(slot.source_path);
    if(!entry.has_value()) {
        throw std::runtime_error("Base pack is missing chunk source path: " + slot.source_path);
    }
    if(slot.source_offset + slot.size > entry->size) {
        throw std::runtime_error("Chunk source range is out of bounds for: " + slot.source_path);
    }

    auto entry_bytes = reader.read_entry_data(*entry);
    return ByteVector(
        entry_bytes.begin() + static_cast<std::ptrdiff_t>(slot.source_offset),
        entry_bytes.begin() + static_cast<std::ptrdiff_t>(slot.source_offset + slot.size)
    );
}

bool is_low_entropy_chunk(std::span<const std::uint8_t> bytes) {
    if(bytes.empty()) {
        return true;
    }

    std::array<bool, 256> seen{};
    std::size_t unique_count = 0;
    std::size_t longest_run = 1;
    std::size_t current_run = 1;

    seen[bytes.front()] = true;
    unique_count = 1;
    for(std::size_t index = 1; index < bytes.size(); ++index) {
        if(!seen[bytes[index]]) {
            seen[bytes[index]] = true;
            ++unique_count;
        }
        if(bytes[index] == bytes[index - 1]) {
            ++current_run;
            longest_run = std::max(longest_run, current_run);
        } else {
            current_run = 1;
        }
    }

    return unique_count < 8 || longest_run > (bytes.size() * 9) / 10;
}

std::vector<GdmodThresholdChunkSlot> collect_cdc_chunk_slots(
    const gddelta::pck::PckEntry& entry,
    const ByteVector& entry_bytes,
    const GdmodThresholdChunkBinding& binding
) {
    std::vector<GdmodThresholdChunkSlot> slots;
    if(entry_bytes.size() < binding.chunk_min_size) {
        return slots;
    }

    const auto min_size = static_cast<std::size_t>(binding.chunk_min_size);
    const auto avg_size = static_cast<std::size_t>(binding.chunk_avg_size);
    const auto max_size = static_cast<std::size_t>(binding.chunk_max_size);
    const auto mask = static_cast<std::uint64_t>(avg_size - 1);

    std::size_t start = 0;
    std::uint64_t fingerprint = 0;
    const auto& gear = gear_table();
    for(std::size_t index = 0; index < entry_bytes.size(); ++index) {
        fingerprint = (fingerprint >> 1U) + gear[entry_bytes[index]];
        const auto chunk_size = index - start + 1;
        if(chunk_size < min_size) {
            continue;
        }

        const auto reached_boundary = ((fingerprint & mask) == 0) || chunk_size >= max_size || index + 1 == entry_bytes.size();
        if(!reached_boundary) {
            continue;
        }

        const auto actual_size = std::min<std::size_t>(chunk_size, entry_bytes.size() - start);
        const auto chunk_bytes = std::span<const std::uint8_t>(entry_bytes.data() + start, actual_size);
        if(!is_low_entropy_chunk(chunk_bytes)) {
            GdmodThresholdChunkSlot slot;
            slot.source_path = entry.path;
            slot.source_offset = start;
            slot.size = actual_size;
            slot.chunk_id = build_chunk_slot_id(slot.source_path, slot.source_offset, slot.size);
            slots.push_back(std::move(slot));
        }

        start = index + 1;
        fingerprint = 0;
    }

    return slots;
}

std::string build_manifest_text(const GdmodManifest& manifest) {
    std::ostringstream output;
    output
    << "type=gdmod\n"
    << "version=" << kGdmodManifestVersion2 << "\n"
    << "base_file_name=" << manifest.base_file_name << "\n"
    << "project_name=" << manifest.project_name << "\n"
    << "format_version=" << manifest.format_version << "\n"
    << "engine_major=" << manifest.engine_major << "\n"
    << "engine_minor=" << manifest.engine_minor << "\n"
    << "engine_patch=" << manifest.engine_patch << "\n"
    << "entry_count=" << manifest.entry_count << "\n"
    << "payload_pack_path=" << manifest.payload_pack_path << "\n";

    std::vector<std::string> sources;
    if(manifest.threshold_chunks.use_pck_source) {
        sources.push_back("pck");
    }
    if(manifest.threshold_chunks.use_executable_source) {
        sources.push_back("executable");
    }

    output
    << "binding_chunking=" << manifest.threshold_chunks.chunking << "\n"
    << "binding_sample_count=" << manifest.threshold_chunks.sample_count << "\n"
    << "binding_threshold=" << manifest.threshold_chunks.threshold << "\n"
    << "binding_chunk_min_size=" << manifest.threshold_chunks.chunk_min_size << "\n"
    << "binding_chunk_avg_size=" << manifest.threshold_chunks.chunk_avg_size << "\n"
    << "binding_chunk_max_size=" << manifest.threshold_chunks.chunk_max_size << "\n"
    << "binding_max_samples_per_file=" << manifest.threshold_chunks.max_samples_per_file << "\n"
    << "binding_sources=" << join_csv(sources) << "\n"
    << "binding_slot_count=" << manifest.threshold_chunks.slots.size() << "\n";

    for(std::size_t index = 0; index < manifest.threshold_chunks.slots.size(); ++index) {
        const auto& slot = manifest.threshold_chunks.slots[index];
        output
        << "binding_slot." << index << ".id=" << slot.chunk_id << "\n"
        << "binding_slot." << index << ".source_path=" << slot.source_path << "\n"
        << "binding_slot." << index << ".source_offset=" << slot.source_offset << "\n"
        << "binding_slot." << index << ".size=" << slot.size << "\n"
        << "binding_slot." << index << ".share_pack_path=" << slot.share_pack_path << "\n";
    }

    return output.str();
}

GdmodManifest parse_manifest_text(std::string_view text) {
    GdmodManifest manifest;
    std::istringstream input{std::string(text)};
    std::string line;
    std::string type;
    std::uint32_t version = 0;
    std::size_t binding_slot_count = 0;

    while(std::getline(input, line)) {
        const auto delimiter = line.find('=');
        if(delimiter == std::string::npos) {
            continue;
        }

        const auto key = line.substr(0, delimiter);
        const auto value = line.substr(delimiter + 1);
        if(key == "type") {
            type = value;
            continue;
        }
        if(key == "version") {
            version = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "base_file_name") {
            manifest.base_file_name = value;
            continue;
        }
        if(key == "project_name") {
            manifest.project_name = value;
            continue;
        }
        if(key == "format_version") {
            manifest.format_version = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "engine_major") {
            manifest.engine_major = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "engine_minor") {
            manifest.engine_minor = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "engine_patch") {
            manifest.engine_patch = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "entry_count") {
            manifest.entry_count = static_cast<std::size_t>(std::stoull(value));
            continue;
        }
        if(key == "payload_pack_path") {
            manifest.payload_pack_path = value;
            continue;
        }
        if(key == "binding_chunking") {
            manifest.threshold_chunks.chunking = value;
            continue;
        }
        if(key == "binding_sample_count") {
            manifest.threshold_chunks.sample_count = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "binding_threshold") {
            manifest.threshold_chunks.threshold = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "binding_chunk_min_size") {
            manifest.threshold_chunks.chunk_min_size = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "binding_chunk_avg_size") {
            manifest.threshold_chunks.chunk_avg_size = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "binding_chunk_max_size") {
            manifest.threshold_chunks.chunk_max_size = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "binding_max_samples_per_file") {
            manifest.threshold_chunks.max_samples_per_file = static_cast<std::uint32_t>(std::stoul(value));
            continue;
        }
        if(key == "binding_sources") {
            manifest.threshold_chunks.use_pck_source = false;
            manifest.threshold_chunks.use_executable_source = false;
            for(const auto& source : split_csv(value)) {
                const auto normalized = to_lower_copy(source);
                if(normalized == "pck") {
                    manifest.threshold_chunks.use_pck_source = true;
                } else if(normalized == "executable") {
                    manifest.threshold_chunks.use_executable_source = true;
                }
            }
            continue;
        }
        if(key == "binding_slot_count") {
            binding_slot_count = static_cast<std::size_t>(std::stoull(value));
            manifest.threshold_chunks.slots.resize(binding_slot_count);
            continue;
        }

        const auto binding_prefix = std::string_view("binding_slot.");
        if(key.rfind(binding_prefix, 0) == 0) {
            const auto field_offset = key.find('.', binding_prefix.size());
            if(field_offset == std::string::npos) {
                continue;
            }

            const auto index = static_cast<std::size_t>(std::stoull(key.substr(binding_prefix.size(), field_offset - binding_prefix.size())));
            if(index >= manifest.threshold_chunks.slots.size()) {
                manifest.threshold_chunks.slots.resize(index + 1);
            }

            auto& slot = manifest.threshold_chunks.slots[index];
            const auto field = key.substr(field_offset + 1);
            if(field == "id") {
                slot.chunk_id = value;
            } else if(field == "source_path") {
                slot.source_path = value;
            } else if(field == "source_offset") {
                slot.source_offset = static_cast<std::uint64_t>(std::stoull(value));
            } else if(field == "size") {
                slot.size = static_cast<std::uint64_t>(std::stoull(value));
            } else if(field == "share_pack_path") {
                slot.share_pack_path = value;
            }
            continue;
        }
    }

    if(type != "gdmod" || (version != kGdmodManifestVersion1 && version != kGdmodManifestVersion2)) {
        throw std::runtime_error("Invalid gdmod manifest.");
    }

    if(version == kGdmodManifestVersion1) {
        manifest.legacy_plain_payload = true;
        manifest.payload_pack_path.clear();
    }

    return manifest;
}

GdmodThresholdChunkBinding build_default_threshold_chunk_binding_impl(const std::filesystem::path& base_pck) {
    GdmodThresholdChunkBinding binding;
    gddelta::pck::PckReader reader;
    reader.open(base_pck);

    std::vector<std::vector<GdmodThresholdChunkSlot>> per_entry_slots;
    per_entry_slots.reserve(reader.entries().size());
    for(const auto& entry : reader.entries()) {
        if(entry.size < binding.chunk_min_size) {
            continue;
        }

        auto entry_bytes = reader.read_entry_data(entry);
        auto slots = collect_cdc_chunk_slots(entry, entry_bytes, binding);
        if(!slots.empty()) {
            per_entry_slots.push_back(std::move(slots));
        }
    }

    std::size_t total_samples = 0;
    std::size_t pass_index = 0;
    while(total_samples < binding.sample_count) {
        auto added_any = false;
        for(auto& slots : per_entry_slots) {
            if(pass_index >= slots.size() || pass_index >= binding.max_samples_per_file) {
                continue;
            }

            binding.slots.push_back(std::move(slots[pass_index]));
            ++total_samples;
            added_any = true;
            if(total_samples >= binding.sample_count) {
                break;
            }
        }

        if(!added_any) {
            break;
        }
        ++pass_index;
    }

    if(binding.threshold > binding.slots.size()) {
        binding.threshold = static_cast<std::uint32_t>(binding.slots.size());
    }
    return binding;
}
}

const char *GdmodPackage::manifest_pack_path() noexcept {
    return kGdmodManifestPackPath;
}

GdmodThresholdChunkBinding GdmodPackage::build_default_threshold_chunk_binding(
    const std::filesystem::path& base_pck
) {
    return build_default_threshold_chunk_binding_impl(base_pck);
}

void GdmodPackage::write(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& output_path,
    const std::vector<pck::PckWriteFile>& files,
    const pck::PckWriteOptions& options,
    const GdmodManifest& manifest
) const {
    auto package_files = std::vector<pck::PckWriteFile>{};

    const auto temp_patch_path = std::filesystem::path(output_path.string() + ".plain.tmp.pck");
    pck::PckWriter patch_writer;
    patch_writer.write_files(files, temp_patch_path, options);
    const auto patch_bytes = read_file_bytes(temp_patch_path);
    std::error_code cleanup_error;
    std::filesystem::remove(temp_patch_path, cleanup_error);

    auto protected_manifest = manifest;
    protected_manifest.payload_pack_path = kGdmodPayloadPackPath;

    const auto payload_key = random_bytes(kPayloadKeySize);
    const auto payload_blob = encrypt_aes_256_gcm(payload_key, patch_bytes);

    pck::PckWriteFile payload_file;
    payload_file.pack_path = protected_manifest.payload_pack_path;
    payload_file.inline_data = payload_blob;
    package_files.push_back(std::move(payload_file));

    const auto share_count = static_cast<std::uint32_t>(protected_manifest.threshold_chunks.slots.size());
    const auto threshold = protected_manifest.threshold_chunks.threshold;
    const auto shares = split_secret_shares(payload_key, share_count, threshold);

    for(std::size_t index = 0; index < protected_manifest.threshold_chunks.slots.size(); ++index) {
        auto& slot = protected_manifest.threshold_chunks.slots[index];
        if(slot.share_pack_path.empty()) {
            slot.share_pack_path = build_slot_pack_path(index);
        }

        const auto chunk_bytes = read_chunk_from_base_pack(base_pck, slot);
        const auto chunk_key = derive_chunk_key(chunk_bytes);
        const auto share_blob = encrypt_aes_256_gcm(chunk_key, shares[index]);

        pck::PckWriteFile share_file;
        share_file.pack_path = slot.share_pack_path;
        share_file.inline_data = share_blob;
        package_files.push_back(std::move(share_file));
    }

    pck::PckWriteFile manifest_file;
    manifest_file.pack_path = manifest_pack_path();
    const auto manifest_text = build_manifest_text(protected_manifest);
    manifest_file.inline_data.assign(manifest_text.begin(), manifest_text.end());
    package_files.push_back(std::move(manifest_file));

    pck::PckWriter writer;
    writer.write_files(package_files, output_path, options);
}

bool GdmodPackage::is_gdmod(const std::filesystem::path& input_path) const {
    pck::PckReader reader;
    reader.open(input_path);
    return reader.find_entry(manifest_pack_path()).has_value();
}

GdmodManifest GdmodPackage::read_manifest(const std::filesystem::path& input_path) const {
    pck::PckReader reader;
    reader.open(input_path);
    const auto manifest_entry = reader.find_entry(manifest_pack_path());
    if(!manifest_entry.has_value()) {
        throw std::runtime_error("Input is not a gdmod package: " + input_path.string());
    }

    const auto bytes = reader.read_entry_data(*manifest_entry);
    return parse_manifest_text(std::string_view(
        reinterpret_cast<const char *>(bytes.data()),
        bytes.size()
    ));
}

void GdmodPackage::extract_patch_pck(
    const std::filesystem::path& input_path,
    const std::filesystem::path& output_path
) const {
    pck::PckReader reader;
    reader.open(input_path);

    std::vector<pck::PckWriteFile> files;
    files.reserve(reader.entries().size());
    for(const auto& entry : reader.entries()) {
        if(entry.path == manifest_pack_path()) {
            continue;
        }

        pck::PckWriteFile file;
        file.pack_path = entry.path;
        file.removal = (entry.flags & pck::kPackFileRemoval) != 0;
        if(!file.removal) {
            file.source_pack_path = input_path;
            file.source_offset = entry.offset;
            file.source_size = entry.size;
        }
        files.push_back(std::move(file));
    }

    pck::PckWriteOptions options;
    options.format_version = reader.header().format_version;
    options.engine_major = reader.header().engine_major;
    options.engine_minor = reader.header().engine_minor;
    options.engine_patch = reader.header().engine_patch;

    pck::PckWriter writer;
    writer.write_files(files, output_path, options);
}

void GdmodPackage::extract_protected_patch_pck(
    const std::filesystem::path& base_pck,
    const std::filesystem::path& input_path,
    const std::filesystem::path& output_path
) const {
    const auto manifest = read_manifest(input_path);
    if(manifest.legacy_plain_payload) {
        extract_patch_pck(input_path, output_path);
        return;
    }

    pck::PckReader reader;
    reader.open(input_path);

    std::vector<ByteVector> recovered_shares;
    recovered_shares.reserve(manifest.threshold_chunks.threshold);
    for(const auto& slot : manifest.threshold_chunks.slots) {
        const auto share_entry = reader.find_entry(slot.share_pack_path);
        if(!share_entry.has_value()) {
            continue;
        }

        try {
            const auto chunk_bytes = read_chunk_from_base_pack(base_pck, slot);
            const auto chunk_key = derive_chunk_key(chunk_bytes);
            const auto share_blob = reader.read_entry_data(*share_entry);
            const auto share = decrypt_aes_256_gcm(chunk_key, share_blob);
            if(share.size() == kSharePlaintextSize) {
                recovered_shares.push_back(share);
            }
        } catch(...) {
            continue;
        }

        if(recovered_shares.size() >= manifest.threshold_chunks.threshold) {
            break;
        }
    }

    if(recovered_shares.size() < manifest.threshold_chunks.threshold) {
        throw std::runtime_error("Failed to recover enough threshold chunk shares from the base game.");
    }

    const auto payload_key = combine_secret_shares(recovered_shares, manifest.threshold_chunks.threshold);
    const auto payload_entry = reader.find_entry(manifest.payload_pack_path);
    if(!payload_entry.has_value()) {
        throw std::runtime_error("Protected gdmod is missing encrypted payload entry.");
    }

    const auto payload_blob = reader.read_entry_data(*payload_entry);
    const auto patch_bytes = decrypt_aes_256_gcm(payload_key, payload_blob);
    write_file_bytes(output_path, patch_bytes);
}
