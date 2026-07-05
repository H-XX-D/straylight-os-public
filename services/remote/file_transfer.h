// services/remote/file_transfer.h
#pragma once

#include <straylight/result.h>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Handles chunked file upload and download with integrity verification.
/// Features:
///   - 64KB chunks with SHA-256 per-chunk verification
///   - Resume support via offset parameter
///   - Zstd compression for transfers > 1MB
///   - Progress tracking
class FileTransfer {
public:
    FileTransfer();
    ~FileTransfer();

    /// Write a base64-encoded chunk to a file at the given offset.
    /// Returns JSON with bytes_written, total_size, sha256 of chunk.
    Result<nlohmann::json, std::string> write_chunk(const std::string& path,
                                                      const std::string& data_b64,
                                                      int64_t offset);

    /// Read a chunk from a file. Returns base64-encoded data with metadata.
    /// If length is -1, reads up to chunk_size_ bytes from offset.
    Result<nlohmann::json, std::string> read_chunk(const std::string& path,
                                                     int64_t offset,
                                                     int64_t length);

    static constexpr size_t kChunkSize = 65536;  // 64KB
    static constexpr size_t kCompressionThreshold = 1024 * 1024;  // 1MB

private:
    // Base64 encode/decode
    static std::string base64_encode(const std::vector<unsigned char>& data);
    static std::vector<unsigned char> base64_decode(const std::string& encoded);

    // SHA-256 hash
    static std::string sha256_hex(const std::vector<unsigned char>& data);

    // Zstd compression/decompression
    static std::vector<unsigned char> zstd_compress(const std::vector<unsigned char>& data);
    static std::vector<unsigned char> zstd_decompress(const std::vector<unsigned char>& data,
                                                        size_t original_size);
};

} // namespace straylight
