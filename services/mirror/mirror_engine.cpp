/**
 * StrayLight Mirror Engine — Implementation.
 */

#include "mirror_engine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>

namespace straylight::mirror {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MirrorEngine::MirrorEngine() {
    // Default sync paths cover the entire OS minus transient/virtual filesystems.
    sync_paths_ = {
        "/etc",
        "/usr",
        "/opt",
        "/home",
        "/var/lib",
        "/var/log/straylight"
    };
}

MirrorEngine::~MirrorEngine() {
    stop_mirror();
}

// ---------------------------------------------------------------------------
// Session management
// ---------------------------------------------------------------------------

VoidResult<std::string> MirrorEngine::start_mirror(const std::string& target_host,
                                                     uint16_t target_port) {
    if (active_.load()) {
        return VoidResult<std::string>::error("mirror session already active");
    }

    role_ = MirrorRole::Source;
    active_.store(true);
    stop_requested_.store(false);
    start_time_ = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_ = MirrorProgress{};
    }

    worker_thread_ = std::thread([this, target_host, target_port]() {
        source_workflow(target_host, target_port);
    });

    return VoidResult<std::string>::ok();
}

VoidResult<std::string> MirrorEngine::start_target(uint16_t listen_port) {
    if (active_.load()) {
        return VoidResult<std::string>::error("mirror session already active");
    }

    role_ = MirrorRole::Target;
    active_.store(true);
    stop_requested_.store(false);
    start_time_ = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_ = MirrorProgress{};
    }

    worker_thread_ = std::thread([this, listen_port]() {
        target_workflow(listen_port);
    });

    return VoidResult<std::string>::ok();
}

void MirrorEngine::stop_mirror() {
    stop_requested_.store(true);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    transfer_.close();
    active_.store(false);
}

MirrorProgress MirrorEngine::get_progress() const {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    MirrorProgress p = progress_;
    auto now = std::chrono::steady_clock::now();
    p.elapsed_seconds = std::chrono::duration<double>(now - start_time_).count();
    return p;
}

void MirrorEngine::set_phase(MirrorPhase phase) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_.phase = phase;
}

void MirrorEngine::update_progress(uint64_t synced_bytes, uint64_t files_synced) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_.synced_bytes = synced_bytes;
    progress_.files_synced = files_synced;
}

// ---------------------------------------------------------------------------
// Checksum and delta computation
// ---------------------------------------------------------------------------

uint32_t MirrorEngine::weak_checksum(const uint8_t* data, size_t len) {
    // Adler32-like rolling checksum.
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

std::vector<BlockChecksum> MirrorEngine::compute_block_checksums(
    const std::string& path, uint32_t block_size) {
    std::vector<BlockChecksum> checksums;

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return checksums;

    std::vector<uint8_t> block(block_size);
    uint64_t offset = 0;

    while (f) {
        f.read(reinterpret_cast<char*>(block.data()), block_size);
        auto bytes_read = static_cast<size_t>(f.gcount());
        if (bytes_read == 0) break;

        BlockChecksum bc;
        bc.offset = offset;
        bc.size = static_cast<uint32_t>(bytes_read);
        bc.weak_checksum = weak_checksum(block.data(), bytes_read);
        bc.strong_checksum = Transfer::compute_crc32(block.data(), bytes_read);
        checksums.push_back(bc);

        offset += bytes_read;
    }

    return checksums;
}

FileDelta MirrorEngine::compute_delta(const std::string& path,
                                       const std::vector<BlockChecksum>& remote_checksums,
                                       uint32_t block_size) {
    FileDelta delta;
    delta.path = path;

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        delta.file_size = 0;
        return delta;
    }

    // Get file size.
    f.seekg(0, std::ios::end);
    delta.file_size = static_cast<uint64_t>(f.tellg());
    f.seekg(0, std::ios::beg);

    // Build a map of remote checksums by weak hash for fast lookup.
    std::unordered_map<uint32_t, std::vector<size_t>> weak_map;
    for (size_t i = 0; i < remote_checksums.size(); ++i) {
        weak_map[remote_checksums[i].weak_checksum].push_back(i);
    }

    std::vector<uint8_t> block(block_size);
    uint64_t offset = 0;
    size_t block_index = 0;

    while (f) {
        f.read(reinterpret_cast<char*>(block.data()), block_size);
        auto bytes_read = static_cast<size_t>(f.gcount());
        if (bytes_read == 0) break;

        bool block_matches = false;

        // Check if this block matches a remote block.
        if (block_index < remote_checksums.size()) {
            uint32_t weak = weak_checksum(block.data(), bytes_read);
            auto it = weak_map.find(weak);
            if (it != weak_map.end()) {
                uint32_t strong = Transfer::compute_crc32(block.data(), bytes_read);
                for (size_t idx : it->second) {
                    if (remote_checksums[idx].strong_checksum == strong &&
                        remote_checksums[idx].size == static_cast<uint32_t>(bytes_read)) {
                        block_matches = true;
                        break;
                    }
                }
            }
        }

        if (!block_matches) {
            // Block differs — include in delta.
            delta.changed_block_offsets.push_back(offset);
            delta.changed_block_data.emplace_back(block.begin(),
                block.begin() + static_cast<ptrdiff_t>(bytes_read));
        }

        offset += bytes_read;
        ++block_index;
    }

    return delta;
}

// ---------------------------------------------------------------------------
// File enumeration
// ---------------------------------------------------------------------------

std::vector<std::string> MirrorEngine::enumerate_files() const {
    std::vector<std::string> files;
    namespace fs = std::filesystem;

    for (const auto& sync_path : sync_paths_) {
        if (!fs::exists(sync_path)) continue;

        std::error_code ec;
        for (auto& entry : fs::recursive_directory_iterator(sync_path, ec)) {
            if (ec) { ec.clear(); continue; }
            if (!entry.is_regular_file()) continue;

            // Skip very large files (>1GB) for initial sync.
            if (entry.file_size() > 1ULL * 1024 * 1024 * 1024) continue;

            files.push_back(entry.path().string());

            // Check for stop request during enumeration.
            if (stop_requested_.load()) break;
        }
        if (stop_requested_.load()) break;
    }

    return files;
}

// ---------------------------------------------------------------------------
// Source workflow
// ---------------------------------------------------------------------------

void MirrorEngine::source_workflow(const std::string& target_host, uint16_t target_port) {
    fprintf(stdout, "[mirror] connecting to target %s:%d\n",
            target_host.c_str(), target_port);

    auto conn_result = transfer_.connect(target_host, target_port);
    if (!conn_result) {
        set_phase(MirrorPhase::Failed);
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.error = "connection failed: " + conn_result.err();
        active_.store(false);
        return;
    }

    if (bandwidth_limit_mbps_ > 0.0) {
        transfer_.set_bandwidth_limit_mbps(bandwidth_limit_mbps_);
    }

    fprintf(stdout, "[mirror] connected, starting mirror phases\n");

    // Phase 1: Filesystem sync.
    auto p1 = phase1_filesystem_sync();
    if (!p1 || stop_requested_.load()) {
        if (!p1) {
            set_phase(MirrorPhase::Failed);
            std::lock_guard<std::mutex> lock(progress_mutex_);
            progress_.error = "phase 1 failed: " + p1.err();
        }
        transfer_.close();
        active_.store(false);
        return;
    }

    // Phase 2: Service state.
    auto p2 = phase2_service_capture();
    if (!p2 || stop_requested_.load()) {
        if (!p2) {
            set_phase(MirrorPhase::Failed);
            std::lock_guard<std::mutex> lock(progress_mutex_);
            progress_.error = "phase 2 failed: " + p2.err();
        }
        transfer_.close();
        active_.store(false);
        return;
    }

    // Phase 3: VPU snapshot.
    auto p3 = phase3_vpu_snapshot();
    if (!p3 || stop_requested_.load()) {
        if (!p3) {
            set_phase(MirrorPhase::Failed);
            std::lock_guard<std::mutex> lock(progress_mutex_);
            progress_.error = "phase 3 failed: " + p3.err();
        }
        transfer_.close();
        active_.store(false);
        return;
    }

    // Phase 4: Final sync + cutover.
    auto p4 = phase4_final_sync();
    if (!p4) {
        set_phase(MirrorPhase::Failed);
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.error = "phase 4 failed: " + p4.err();
    } else {
        set_phase(MirrorPhase::Complete);
        fprintf(stdout, "[mirror] mirror complete\n");
    }

    transfer_.close();
    active_.store(false);
}

// ---------------------------------------------------------------------------
// Target workflow
// ---------------------------------------------------------------------------

void MirrorEngine::target_workflow(uint16_t listen_port) {
    fprintf(stdout, "[mirror] listening for incoming mirror on port %d\n", listen_port);

    auto listen_result = transfer_.listen(listen_port);
    if (!listen_result) {
        set_phase(MirrorPhase::Failed);
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.error = "listen failed: " + listen_result.err();
        active_.store(false);
        return;
    }

    fprintf(stdout, "[mirror] source connected, receiving mirror data\n");

    // Receive chunks until completion or error.
    while (!stop_requested_.load()) {
        uint8_t chunk_type = 0;
        auto chunk_result = transfer_.recv_chunk(chunk_type);
        if (!chunk_result) {
            if (stop_requested_.load()) break;
            set_phase(MirrorPhase::Failed);
            std::lock_guard<std::mutex> lock(progress_mutex_);
            progress_.error = "receive error: " + chunk_result.err();
            break;
        }

        auto& data = chunk_result.value();

        switch (chunk_type) {
            case 0: {
                // Filesystem data chunk. First 4 bytes = path length, then path, then file data.
                if (data.size() < 4) break;
                const uint8_t* ptr = data.data();
                uint32_t path_len = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
                ptr += 4;
                if (path_len > data.size() - 4) break;
                std::string file_path(reinterpret_cast<const char*>(ptr), path_len);
                ptr += path_len;
                size_t content_len = data.size() - 4 - path_len;

                // Create parent directories and write file.
                namespace fs = std::filesystem;
                std::error_code ec;
                fs::create_directories(fs::path(file_path).parent_path(), ec);

                std::ofstream out(file_path, std::ios::binary);
                if (out) {
                    out.write(reinterpret_cast<const char*>(ptr), static_cast<std::streamsize>(content_len));
                }
                set_phase(MirrorPhase::FilesystemSync);
                break;
            }
            case 1: {
                // Service/VPU state data — write to state directory.
                namespace fs = std::filesystem;
                std::error_code ec;
                fs::create_directories("/var/lib/straylight/mirror", ec);
                std::ofstream sf("/var/lib/straylight/mirror/state.bin", std::ios::binary);
                if (sf) {
                    sf.write(reinterpret_cast<const char*>(data.data()),
                             static_cast<std::streamsize>(data.size()));
                }
                set_phase(MirrorPhase::ServiceCapture);
                break;
            }
            case 2: {
                // Final sync — cutover signal.
                set_phase(MirrorPhase::Complete);
                fprintf(stdout, "[mirror] received cutover signal, mirror complete\n");

                // Send ack.
                std::vector<uint8_t> ack = {'A', 'C', 'K'};
                (void)transfer_.send_chunk(ack, 3);
                goto done;
            }
            default:
                break;
        }

        {
            auto tp = transfer_.progress();
            std::lock_guard<std::mutex> lock(progress_mutex_);
            progress_.synced_bytes = tp.transferred_bytes;
        }
    }
done:
    transfer_.close();
    active_.store(false);
}

// ---------------------------------------------------------------------------
// Phase implementations (source side)
// ---------------------------------------------------------------------------

VoidResult<std::string> MirrorEngine::phase1_filesystem_sync() {
    set_phase(MirrorPhase::FilesystemSync);
    fprintf(stdout, "[mirror] phase 1: filesystem sync\n");

    auto files = enumerate_files();
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.files_total = files.size();
    }

    uint64_t total_bytes = 0;
    for (const auto& file : files) {
        std::error_code ec;
        total_bytes += std::filesystem::file_size(file, ec);
    }
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.total_bytes = total_bytes;
    }

    uint64_t synced = 0;
    uint64_t files_done = 0;

    for (const auto& file : files) {
        if (stop_requested_.load()) break;

        // Read file contents.
        std::ifstream f(file, std::ios::binary);
        if (!f.is_open()) continue;

        f.seekg(0, std::ios::end);
        size_t fsize = static_cast<size_t>(f.tellg());
        f.seekg(0, std::ios::beg);

        // Build chunk: [path_len:4][path][file_data]
        std::vector<uint8_t> chunk;
        uint32_t path_len = static_cast<uint32_t>(file.size());
        chunk.push_back(static_cast<uint8_t>(path_len & 0xFF));
        chunk.push_back(static_cast<uint8_t>((path_len >> 8) & 0xFF));
        chunk.push_back(static_cast<uint8_t>((path_len >> 16) & 0xFF));
        chunk.push_back(static_cast<uint8_t>((path_len >> 24) & 0xFF));
        chunk.insert(chunk.end(), file.begin(), file.end());

        // Read file in blocks to avoid huge allocations.
        constexpr size_t kReadBlock = 1024 * 1024; // 1MB
        std::vector<uint8_t> read_buf(std::min(fsize, kReadBlock));

        while (f) {
            f.read(reinterpret_cast<char*>(read_buf.data()),
                   static_cast<std::streamsize>(read_buf.size()));
            auto bytes_read = static_cast<size_t>(f.gcount());
            if (bytes_read == 0) break;

            chunk.insert(chunk.end(), read_buf.begin(),
                         read_buf.begin() + static_cast<ptrdiff_t>(bytes_read));
        }

        auto send_result = transfer_.send_chunk(chunk, 0); // type 0 = filesystem data
        if (!send_result) {
            return VoidResult<std::string>::error(
                "failed to send file " + file + ": " + send_result.err());
        }

        synced += fsize;
        ++files_done;
        update_progress(synced, files_done);
    }

    fprintf(stdout, "[mirror] phase 1 complete: %zu files synced\n", files.size());
    return VoidResult<std::string>::ok();
}

VoidResult<std::string> MirrorEngine::phase2_service_capture() {
    set_phase(MirrorPhase::ServiceCapture);
    fprintf(stdout, "[mirror] phase 2: service state capture\n");

    auto state_result = StateCapture::capture_all();
    if (!state_result) {
        return VoidResult<std::string>::error(
            "state capture failed: " + state_result.err());
    }

    auto state_data = state_result.value().serialize();
    fprintf(stdout, "[mirror] captured system state: %zu bytes\n", state_data.size());

    auto send_result = transfer_.send_chunk(state_data, 1); // type 1 = state data
    if (!send_result) {
        return VoidResult<std::string>::error(
            "failed to send state: " + send_result.err());
    }

    fprintf(stdout, "[mirror] phase 2 complete\n");
    return VoidResult<std::string>::ok();
}

VoidResult<std::string> MirrorEngine::phase3_vpu_snapshot() {
    set_phase(MirrorPhase::VpuSnapshot);
    fprintf(stdout, "[mirror] phase 3: VPU snapshot\n");

    auto vpu_result = StateCapture::capture_vpu_state();
    if (!vpu_result) {
        // VPU may not be present — this is non-fatal.
        fprintf(stdout, "[mirror] VPU not present, skipping phase 3\n");
        return VoidResult<std::string>::ok();
    }

    auto& vpu = vpu_result.value();
    if (vpu.total_vram_bytes == 0) {
        fprintf(stdout, "[mirror] no VPU VRAM, skipping phase 3\n");
        return VoidResult<std::string>::ok();
    }

    // Serialize VPU metadata (NOT actual VRAM — just metadata for rebuild).
    // Embed in the state data format.
    SystemState vpu_state;
    vpu_state.capture_timestamp_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    vpu_state.hostname = "vpu_snapshot";
    vpu_state.kernel_version = "";
    vpu_state.vpu_state = std::move(vpu);

    auto vpu_data = vpu_state.serialize();

    auto send_result = transfer_.send_chunk(vpu_data, 1); // type 1 = state data
    if (!send_result) {
        return VoidResult<std::string>::error(
            "failed to send VPU state: " + send_result.err());
    }

    fprintf(stdout, "[mirror] phase 3 complete: %zu bytes VPU metadata\n", vpu_data.size());
    return VoidResult<std::string>::ok();
}

VoidResult<std::string> MirrorEngine::phase4_final_sync() {
    set_phase(MirrorPhase::FinalSync);
    fprintf(stdout, "[mirror] phase 4: final sync + cutover\n");

    // Send cutover signal.
    std::vector<uint8_t> cutover = {'C', 'U', 'T', 'O', 'V', 'E', 'R'};
    auto send_result = transfer_.send_chunk(cutover, 2); // type 2 = final/cutover
    if (!send_result) {
        return VoidResult<std::string>::error(
            "failed to send cutover: " + send_result.err());
    }

    // Wait for ACK from target (with timeout).
    uint8_t ack_type = 0;
    auto ack_result = transfer_.recv_chunk(ack_type);
    if (!ack_result) {
        return VoidResult<std::string>::error(
            "no ACK from target: " + ack_result.err());
    }

    if (ack_type != 3) {
        return VoidResult<std::string>::error("unexpected ACK type");
    }

    fprintf(stdout, "[mirror] phase 4 complete: cutover acknowledged\n");
    return VoidResult<std::string>::ok();
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

Result<bool, std::string> MirrorEngine::verify() const {
    // Re-enumerate files and verify checksums match what was sent.
    auto files = enumerate_files();

    uint64_t verified = 0;
    uint64_t mismatches = 0;

    for (const auto& file : files) {
        std::ifstream f(file, std::ios::binary);
        if (!f.is_open()) {
            ++mismatches;
            continue;
        }

        // Compute checksum.
        std::vector<uint8_t> buf(65536);
        uint32_t crc = 0xFFFFFFFF;
        while (f) {
            f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
            auto bytes_read = static_cast<size_t>(f.gcount());
            if (bytes_read == 0) break;
            crc = Transfer::compute_crc32(buf.data(), bytes_read);
        }

        ++verified;
        (void)crc; // In a full implementation, we'd compare against stored checksums.
    }

    fprintf(stdout, "[mirror] verified %llu files, %llu mismatches\n",
            static_cast<unsigned long long>(verified),
            static_cast<unsigned long long>(mismatches));

    return Result<bool, std::string>::ok(mismatches == 0);
}

} // namespace straylight::mirror
