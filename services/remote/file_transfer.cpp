// services/remote/file_transfer.cpp
#include "file_transfer.h"
#include <straylight/log.h>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <zstd.h>

#include <filesystem>
#include <fstream>
#include <cstring>

namespace straylight {

FileTransfer::FileTransfer() = default;
FileTransfer::~FileTransfer() = default;

Result<nlohmann::json, std::string> FileTransfer::write_chunk(const std::string& path,
                                                                const std::string& data_b64,
                                                                int64_t offset) {
    // Decode base64 data
    auto data = base64_decode(data_b64);
    if (data.empty() && !data_b64.empty()) {
        return Result<nlohmann::json, std::string>::error("Failed to decode base64 data");
    }

    // Check if data might be zstd-compressed (has zstd magic number)
    bool was_compressed = false;
    if (data.size() >= 4) {
        uint32_t magic = 0;
        std::memcpy(&magic, data.data(), 4);
        if (magic == 0xFD2FB528) {
            // Data is zstd-compressed; get original size from frame header
            auto original_size = ZSTD_getFrameContentSize(data.data(), data.size());
            if (original_size != ZSTD_CONTENTSIZE_UNKNOWN &&
                original_size != ZSTD_CONTENTSIZE_ERROR) {
                data = zstd_decompress(data, static_cast<size_t>(original_size));
                was_compressed = true;
            }
        }
    }

    // Compute SHA-256 of the (decompressed) data
    std::string hash = sha256_hex(data);

    // Ensure parent directory exists
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        // Ignore error — the write will fail if the directory truly doesn't exist
    }

    // Open file for writing at offset
    std::ofstream file;
    if (offset == 0) {
        file.open(path, std::ios::binary | std::ios::trunc);
    } else {
        file.open(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!file.is_open()) {
            // File might not exist yet for non-zero offset
            return Result<nlohmann::json, std::string>::error(
                "Cannot open file for resume at offset " + std::to_string(offset));
        }
        file.seekp(offset);
    }

    if (!file.is_open()) {
        return Result<nlohmann::json, std::string>::error(
            "Cannot open file for writing: " + path);
    }

    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));

    if (!file.good()) {
        return Result<nlohmann::json, std::string>::error("Write failed");
    }

    file.close();

    // Get total file size after write
    auto total_size = std::filesystem::file_size(path);

    nlohmann::json result;
    result["path"] = path;
    result["bytes_written"] = data.size();
    result["offset"] = offset;
    result["total_size"] = total_size;
    result["sha256"] = hash;
    result["compressed"] = was_compressed;

    SL_DEBUG("file-transfer: wrote {} bytes to {} at offset {} (sha256={})",
             data.size(), path, offset, hash.substr(0, 16));

    return Result<nlohmann::json, std::string>::ok(result);
}

Result<nlohmann::json, std::string> FileTransfer::read_chunk(const std::string& path,
                                                               int64_t offset,
                                                               int64_t length) {
    if (!std::filesystem::exists(path)) {
        return Result<nlohmann::json, std::string>::error("File not found: " + path);
    }

    if (!std::filesystem::is_regular_file(path)) {
        return Result<nlohmann::json, std::string>::error("Not a regular file: " + path);
    }

    auto total_size = static_cast<int64_t>(std::filesystem::file_size(path));

    if (offset >= total_size) {
        return Result<nlohmann::json, std::string>::error("Offset beyond end of file");
    }

    // Determine read length
    int64_t read_len = (length < 0) ? static_cast<int64_t>(kChunkSize) : length;
    if (read_len > static_cast<int64_t>(kChunkSize)) {
        read_len = static_cast<int64_t>(kChunkSize);
    }
    if (offset + read_len > total_size) {
        read_len = total_size - offset;
    }

    // Read the chunk
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return Result<nlohmann::json, std::string>::error("Cannot open file: " + path);
    }

    file.seekg(offset);
    std::vector<unsigned char> data(static_cast<size_t>(read_len));
    file.read(reinterpret_cast<char*>(data.data()), read_len);
    auto bytes_read = file.gcount();
    data.resize(static_cast<size_t>(bytes_read));

    // Compute SHA-256
    std::string hash = sha256_hex(data);

    // Optionally compress if chunk is large enough
    bool compressed = false;
    std::vector<unsigned char> output_data = data;
    if (data.size() >= kCompressionThreshold) {
        auto compressed_data = zstd_compress(data);
        if (compressed_data.size() < data.size()) {
            output_data = std::move(compressed_data);
            compressed = true;
        }
    }

    // Encode as base64
    std::string encoded = base64_encode(output_data);

    nlohmann::json result;
    result["data"] = encoded;
    result["path"] = path;
    result["offset"] = offset;
    result["length"] = bytes_read;
    result["total"] = total_size;
    result["sha256"] = hash;
    result["compressed"] = compressed;
    result["original_size"] = data.size();

    SL_DEBUG("file-transfer: read {} bytes from {} at offset {} (sha256={})",
             bytes_read, path, offset, hash.substr(0, 16));

    return Result<nlohmann::json, std::string>::ok(result);
}

std::string FileTransfer::base64_encode(const std::vector<unsigned char>& data) {
    if (data.empty()) return "";

    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);

    BIO_write(bio, data.data(), static_cast<int>(data.size()));
    BIO_flush(bio);

    BUF_MEM* buf_mem = nullptr;
    BIO_get_mem_ptr(bio, &buf_mem);

    std::string result(buf_mem->data, buf_mem->length);
    BIO_free_all(bio);

    return result;
}

std::vector<unsigned char> FileTransfer::base64_decode(const std::string& encoded) {
    if (encoded.empty()) return {};

    BIO* bio = BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()));
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);

    std::vector<unsigned char> result(encoded.size());
    int decoded_len = BIO_read(bio, result.data(), static_cast<int>(result.size()));
    BIO_free_all(bio);

    if (decoded_len < 0) return {};
    result.resize(static_cast<size_t>(decoded_len));
    return result;
}

std::string FileTransfer::sha256_hex(const std::vector<unsigned char>& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);

    std::string hex;
    hex.reserve(SHA256_DIGEST_LENGTH * 2);
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        hex.push_back(hex_chars[hash[i] >> 4]);
        hex.push_back(hex_chars[hash[i] & 0x0f]);
    }
    return hex;
}

std::vector<unsigned char> FileTransfer::zstd_compress(const std::vector<unsigned char>& data) {
    size_t bound = ZSTD_compressBound(data.size());
    std::vector<unsigned char> compressed(bound);

    size_t compressed_size = ZSTD_compress(compressed.data(), bound,
                                            data.data(), data.size(),
                                            3);  // compression level 3

    if (ZSTD_isError(compressed_size)) {
        SL_WARN("file-transfer: zstd compression failed: {}", ZSTD_getErrorName(compressed_size));
        return data;  // Return uncompressed on failure
    }

    compressed.resize(compressed_size);
    return compressed;
}

std::vector<unsigned char> FileTransfer::zstd_decompress(const std::vector<unsigned char>& data,
                                                           size_t original_size) {
    std::vector<unsigned char> decompressed(original_size);

    size_t result = ZSTD_decompress(decompressed.data(), original_size,
                                     data.data(), data.size());

    if (ZSTD_isError(result)) {
        SL_WARN("file-transfer: zstd decompression failed: {}", ZSTD_getErrorName(result));
        return data;  // Return as-is on failure
    }

    decompressed.resize(result);
    return decompressed;
}

} // namespace straylight
