// apps/morph-gui/morph_panel.h
#pragma once
#include <imgui.h>
#include <string>
#include <vector>
#include <cmath>
// === STRAYLIGHT_MORPH_WIRED: real OS-file-read data source (no morph daemon/socket exists) ===
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace straylight::morph {

struct ModelInfo {
    std::string name;
    std::string arch;
    float size_gb;
    int   params_m;
    float accuracy;
    float latency_ms;
};

struct MorphOp {
    std::string type;
    float       reduction;
    float       accuracy_delta;
    float       latency_delta;
    bool        enabled = true;
};

struct PruneState {
    float   sparsity_target = 0.5f;
    int     method_idx = 0;
    bool    iterative = true;
    float   progress = 0.0f;
    bool    running = false;
    float   curve_x[32];
    float   curve_y[32];
};

struct DistillState {
    char    teacher_buf[128] = "straylight-llm-70b";
    char    student_buf[128] = "straylight-llm-7b";
    int     temp_idx = 1;
    float   alpha = 0.7f;
    int     epochs = 3;
    float   progress = 0.0f;
    bool    running = false;
    float   loss_history[64];
};

struct QuantizeState {
    int    bits_idx = 1;
    int    scheme_idx = 0;
    bool   per_channel = true;
    bool   calibrate = true;
    float  progress = 0.0f;
    bool   running = false;
    std::vector<std::pair<std::string, float>> layer_errors;
};

struct MorphState {
    std::vector<ModelInfo> models;
    int   selected_model = 0;
    int   active_tab = 0;
    PruneState    prune;
    DistillState  distill;
    QuantizeState quantize;
    std::vector<MorphOp> history;

    // === STRAYLIGHT_MORPH_WIRED: real OS data-source state (no daemon/socket) ===
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    static std::string models_dir() {
        if (const char* env = std::getenv("STRAYLIGHT_MODELS_DIR")) {
            if (*env) return env;
        }
        if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
            if (*xdg) return std::string(xdg) + "/straylight/models";
        }
        if (const char* home = std::getenv("HOME")) {
            if (*home) return std::string(home) + "/.local/share/straylight/models";
        }
        return "/usr/share/straylight/models";
    }

    // Sum product of each tensor "shape" from the safetensors header.
    // Layout: [8-byte LE header length][JSON header][tensor data].
    static long long safetensors_params(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return 0;
        uint64_t hlen = 0;
        unsigned char lenbuf[8];
        f.read(reinterpret_cast<char*>(lenbuf), 8);
        if (f.gcount() != 8) return 0;
        for (int i = 0; i < 8; ++i)
            hlen |= static_cast<uint64_t>(lenbuf[i]) << (8 * i);  // little-endian
        if (hlen == 0 || hlen > (64ull << 20)) return 0;          // sanity cap
        std::string hdr(hlen, '\0');
        f.read(hdr.data(), static_cast<std::streamsize>(hlen));
        if (static_cast<uint64_t>(f.gcount()) != hlen) return 0;
        nlohmann::json j = nlohmann::json::parse(hdr, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return 0;
        long long total = 0;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() == "__metadata__") continue;
            const auto& t = it.value();
            if (!t.is_object() || !t.contains("shape")) continue;
            long long prod = 1;
            for (const auto& d : t["shape"]) prod *= d.get<long long>();
            total += prod;
        }
        return total;
    }

    static std::string read_arch(const std::string& cfg_path) {
        std::ifstream f(cfg_path);
        if (!f) return std::string();
        nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return std::string();
        if (j.contains("architectures") && j["architectures"].is_array() &&
            !j["architectures"].empty())
            return j["architectures"][0].get<std::string>();
        return j.value("model_type", std::string());
    }

    // Enumerate model dirs and load REAL ModelInfo fields render() reads.
    void refresh() {
        namespace fs = std::filesystem;
        ok_ = false;
        err_.clear();
        std::error_code ec;
        const std::string model_dir = models_dir();
        if (!fs::exists(model_dir, ec) || !fs::is_directory(model_dir, ec)) {
            err_ = std::string("model dir unavailable: ") + model_dir;
            return;
        }
        std::vector<ModelInfo> loaded;
        std::vector<fs::path> dirs;
        for (const auto& e : fs::directory_iterator(model_dir, ec)) {
            if (e.is_directory(ec)) dirs.push_back(e.path());
        }
        std::sort(dirs.begin(), dirs.end());
        for (const auto& dir : dirs) {
            fs::path st = dir / "model.safetensors";
            if (!fs::exists(st, ec)) continue;
            ModelInfo mi;
            mi.name       = dir.filename().string();
            std::uintmax_t bytes = fs::file_size(st, ec);
            mi.size_gb    = ec ? 0.0f : static_cast<float>(bytes) / 1.0e9f;
            mi.params_m   = static_cast<int>(safetensors_params(st.string()) / 1000000LL);
            mi.arch       = read_arch((dir / "config.json").string());
            // accuracy and latency_ms have no real source on this box.
            mi.accuracy   = 0.0f;
            mi.latency_ms = 0.0f;
            loaded.push_back(std::move(mi));
        }
        models = std::move(loaded);
        if (selected_model >= static_cast<int>(models.size()))
            selected_model = models.empty() ? 0 : static_cast<int>(models.size()) - 1;
        ok_ = true;
    }

    void maybe_refresh() {
        double now = ImGui::GetTime();
        if (now - last_refresh_ >= 2.0) {
            last_refresh_ = now;
            refresh();
        }
    }

    void init() { refresh(); }
};

class MorphPanel {
public:
    void init() { state_.init(); }
    void render();
private:
    MorphState state_;
    void render_model_selector();
    void render_prune_tab();
    void render_distill_tab();
    void render_quantize_tab();
    void render_history();
};

} // namespace straylight::morph
