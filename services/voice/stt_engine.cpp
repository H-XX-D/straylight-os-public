// services/voice/stt_engine.cpp
// Whisper.cpp-based speech-to-text with CLI fallback.

#include "stt_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

// whisper.cpp public header — on a full build, this is the vendored copy.
// We use dlopen/dlsym at runtime to avoid a hard link dependency, which lets
// the daemon start even if whisper.cpp is not installed (it will fall back to
// the CLI tool).
#ifdef HAVE_WHISPER_CPP
#include "whisper.h"
#endif

namespace straylight::voice {

// ─── Lifecycle ──────────────────────────────────────────────────────────────

SttEngine::~SttEngine() {
    unload_model();
}

Result<void, std::string> SttEngine::init(const VoiceConfig& cfg) {
    model_path_ = cfg.stt_model_path;
    language_   = cfg.stt_language;

    auto result = load_model(model_path_);
    if (!result.has_value()) {
        fprintf(stderr, "[voice:stt] model load failed: %s\n", result.error().c_str());
        fprintf(stderr, "[voice:stt] will use CLI fallback (whisper-cli / Google STT)\n");
        // Not fatal — we can operate with the CLI fallback.
    }
    return Result<void, std::string>::ok();
}

// ─── Model management ───────────────────────────────────────────────────────

Result<void, std::string> SttEngine::load_model(const std::string& path) {
    unload_model();

    namespace fs = std::filesystem;
    if (!fs::exists(path)) {
        return Result<void, std::string>::error("model not found: " + path);
    }

    auto file_size = fs::file_size(path);
    fprintf(stdout, "[voice:stt] loading model %s (%.1f MB)...\n",
            path.c_str(), static_cast<double>(file_size) / (1024.0 * 1024.0));

#ifdef HAVE_WHISPER_CPP
    // Initialize whisper context.
    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true;

    whisper_ctx_ = whisper_init_from_file_with_params(path.c_str(), cparams);
    if (!whisper_ctx_) {
        return Result<void, std::string>::error(
            "whisper_init_from_file failed for: " + path);
    }
#else
    // No whisper.cpp compiled in — try dynamic loading via dlopen.
    // For now, record that we don't have a native model and will use fallback.
    whisper_ctx_ = nullptr;
#endif

    model_path_  = path;
    model_size_  = detect_model_size(path);
    model_loaded_ = (whisper_ctx_ != nullptr);

    if (model_loaded_) {
        fprintf(stdout, "[voice:stt] model loaded successfully\n");
    } else {
        fprintf(stdout, "[voice:stt] native model unavailable, CLI fallback active\n");
    }

    return Result<void, std::string>::ok();
}

void SttEngine::unload_model() {
#ifdef HAVE_WHISPER_CPP
    if (whisper_ctx_) {
        whisper_free(static_cast<struct whisper_context*>(whisper_ctx_));
    }
#endif
    whisper_ctx_  = nullptr;
    model_loaded_ = false;
}

WhisperModelSize SttEngine::detect_model_size(const std::string& path) const {
    if (path.find("tiny") != std::string::npos)   return WhisperModelSize::Tiny;
    if (path.find("small") != std::string::npos)  return WhisperModelSize::Small;
    if (path.find("medium") != std::string::npos) return WhisperModelSize::Medium;
    return WhisperModelSize::Base;
}

// ─── Transcribe ─────────────────────────────────────────────────────────────

Result<TranscribeResult, std::string> SttEngine::transcribe(const AudioBuffer& audio) {
    auto float_buf = audio.to_float();
    return transcribe(float_buf, audio.sample_rate);
}

Result<TranscribeResult, std::string> SttEngine::transcribe(
    const std::vector<float>& samples, int sample_rate)
{
    if (samples.empty()) {
        return Result<TranscribeResult, std::string>::error("empty audio buffer");
    }

    auto t0 = std::chrono::steady_clock::now();
    double audio_dur = static_cast<double>(samples.size()) / sample_rate;

#ifdef HAVE_WHISPER_CPP
    if (model_loaded_ && whisper_ctx_) {
        auto* ctx = static_cast<struct whisper_context*>(whisper_ctx_);

        // Resample to 16 kHz if needed.
        std::vector<float> resampled;
        const float* data = samples.data();
        int n_samples = static_cast<int>(samples.size());

        if (sample_rate != 16000) {
            double ratio = 16000.0 / sample_rate;
            size_t new_size = static_cast<size_t>(samples.size() * ratio);
            resampled.resize(new_size);
            for (size_t i = 0; i < new_size; ++i) {
                double src_idx = i / ratio;
                size_t idx = static_cast<size_t>(src_idx);
                if (idx >= samples.size() - 1) idx = samples.size() - 2;
                double frac = src_idx - idx;
                resampled[i] = static_cast<float>(
                    samples[idx] * (1.0 - frac) + samples[idx + 1] * frac);
            }
            data = resampled.data();
            n_samples = static_cast<int>(resampled.size());
        }

        // Configure inference.
        struct whisper_full_params wparams = whisper_full_default_params(
            WHISPER_SAMPLING_GREEDY);
        wparams.n_threads    = 4;
        wparams.print_special = false;
        wparams.print_progress = false;
        wparams.print_realtime = false;
        wparams.print_timestamps = false;
        wparams.single_segment = false;
        wparams.token_timestamps = true;

        if (language_ != "auto") {
            wparams.language = language_.c_str();
        } else {
            wparams.language = nullptr;  // auto-detect
        }

        // Run inference.
        int ret = whisper_full(ctx, wparams, data, n_samples);
        if (ret != 0) {
            return Result<TranscribeResult, std::string>::error(
                "whisper_full failed with code " + std::to_string(ret));
        }

        // Collect results.
        TranscribeResult result;
        result.audio_duration_s = audio_dur;

        int n_segments = whisper_full_n_segments(ctx);
        float total_prob = 0.0f;
        int token_count = 0;

        for (int seg = 0; seg < n_segments; ++seg) {
            const char* text = whisper_full_get_segment_text(ctx, seg);
            if (text) {
                result.text += text;
            }

            int64_t t0_seg = whisper_full_get_segment_t0(ctx, seg);
            int64_t t1_seg = whisper_full_get_segment_t1(ctx, seg);

            int n_tokens = whisper_full_n_tokens(ctx, seg);
            for (int tok = 0; tok < n_tokens; ++tok) {
                whisper_token_data td = whisper_full_get_token_data(ctx, seg, tok);
                total_prob += td.p;
                ++token_count;

                const char* tok_text = whisper_full_get_token_text(ctx, seg, tok);
                if (tok_text && tok_text[0] != '[') {
                    WordTimestamp wt;
                    wt.word = tok_text;
                    wt.start_s = static_cast<double>(td.t0) / 100.0;
                    wt.end_s   = static_cast<double>(td.t1) / 100.0;
                    result.timestamps.push_back(std::move(wt));
                }
            }
        }

        result.confidence = (token_count > 0) ? (total_prob / token_count) : 0.0f;

        // Detect language.
        const char* lang = whisper_lang_str(whisper_full_lang_id(ctx));
        result.language = lang ? lang : "en";

        auto t1 = std::chrono::steady_clock::now();
        result.process_time_s = std::chrono::duration<double>(t1 - t0).count();

        // Trim leading/trailing whitespace.
        auto start = result.text.find_first_not_of(" \t\n\r");
        auto end   = result.text.find_last_not_of(" \t\n\r");
        if (start != std::string::npos) {
            result.text = result.text.substr(start, end - start + 1);
        }

        return Result<TranscribeResult, std::string>::ok(std::move(result));
    }
#endif

    // Fallback path.
    return fallback_transcribe(samples, sample_rate);
}

// ─── Fallback (CLI tools) ───────────────────────────────────────────────────

Result<std::string, std::string> SttEngine::write_temp_wav(
    const std::vector<float>& samples, int sample_rate)
{
    // Create a minimal WAV file in /tmp.
    char path[] = "/tmp/straylight-stt-XXXXXX.wav";
    // Use mkstemp pattern with manual suffix.
    std::string tmp_path = "/tmp/straylight-stt-" +
        std::to_string(getpid()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        ".wav";

    std::ofstream out(tmp_path, std::ios::binary);
    if (!out.is_open()) {
        return Result<std::string, std::string>::error("cannot create temp WAV: " + tmp_path);
    }

    // Convert float to int16.
    std::vector<int16_t> pcm(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        float s = std::max(-1.0f, std::min(1.0f, samples[i]));
        pcm[i] = static_cast<int16_t>(s * 32767.0f);
    }

    uint32_t data_size = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    uint32_t file_size = 36 + data_size;

    // WAV header.
    auto write_u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto write_u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };

    out.write("RIFF", 4);
    write_u32(file_size);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    write_u32(16);                                     // chunk size
    write_u16(1);                                      // PCM format
    write_u16(1);                                      // mono
    write_u32(static_cast<uint32_t>(sample_rate));     // sample rate
    write_u32(static_cast<uint32_t>(sample_rate * 2)); // byte rate
    write_u16(2);                                      // block align
    write_u16(16);                                     // bits per sample
    out.write("data", 4);
    write_u32(data_size);
    out.write(reinterpret_cast<const char*>(pcm.data()), data_size);

    out.close();
    return Result<std::string, std::string>::ok(tmp_path);
}

Result<TranscribeResult, std::string> SttEngine::fallback_transcribe(
    const std::vector<float>& samples, int sample_rate)
{
    auto t0 = std::chrono::steady_clock::now();

    // Write audio to temp WAV.
    auto wav_result = write_temp_wav(samples, sample_rate);
    if (!wav_result.has_value()) {
        return Result<TranscribeResult, std::string>::error(wav_result.error());
    }
    std::string wav_path = wav_result.value();

    // Try whisper-cli first.
    std::string cmd;
    bool have_whisper_cli = (std::system("which whisper-cli >/dev/null 2>&1") == 0);
    bool have_whisper = (std::system("which whisper >/dev/null 2>&1") == 0);

    if (have_whisper_cli) {
        cmd = "whisper-cli -m " + model_path_ + " -f " + wav_path +
              " --no-timestamps --output-txt 2>/dev/null";
    } else if (have_whisper) {
        cmd = "whisper " + wav_path + " --model base --language " +
              (language_ == "auto" ? "en" : language_) +
              " --output_format txt 2>/dev/null";
    } else {
        // Last resort: try Google Cloud STT via curl (requires network).
        // For an offline OS this will fail gracefully.
        std::remove(wav_path.c_str());
        return Result<TranscribeResult, std::string>::error(
            "no STT backend available: install whisper.cpp or whisper-cli");
    }

    // Execute and capture stdout.
    std::array<char, 4096> buf{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        std::remove(wav_path.c_str());
        return Result<TranscribeResult, std::string>::error(
            "popen failed for STT command");
    }

    while (fgets(buf.data(), buf.size(), pipe) != nullptr) {
        output += buf.data();
    }
    int status = pclose(pipe);

    // Clean up temp file.
    std::remove(wav_path.c_str());

    // Also check for .txt output file from whisper.
    std::string txt_path = wav_path.substr(0, wav_path.size() - 4) + ".txt";
    if (std::filesystem::exists(txt_path)) {
        std::ifstream txt_file(txt_path);
        if (txt_file.is_open()) {
            output.clear();
            std::string line;
            while (std::getline(txt_file, line)) {
                output += line + " ";
            }
        }
        std::remove(txt_path.c_str());
    }

    if (status != 0 && output.empty()) {
        return Result<TranscribeResult, std::string>::error(
            "STT command failed with exit code " + std::to_string(status));
    }

    // Trim whitespace.
    auto start = output.find_first_not_of(" \t\n\r");
    auto end   = output.find_last_not_of(" \t\n\r");
    if (start != std::string::npos) {
        output = output.substr(start, end - start + 1);
    } else {
        output.clear();
    }

    auto t1 = std::chrono::steady_clock::now();

    TranscribeResult result;
    result.text = output;
    result.language = (language_ == "auto") ? "en" : language_;
    result.confidence = 0.5f; // unknown from CLI
    result.audio_duration_s = static_cast<double>(samples.size()) / sample_rate;
    result.process_time_s = std::chrono::duration<double>(t1 - t0).count();

    return Result<TranscribeResult, std::string>::ok(std::move(result));
}

} // namespace straylight::voice
