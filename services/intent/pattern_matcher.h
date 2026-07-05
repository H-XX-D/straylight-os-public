/**
 * StrayLight Pattern Matcher — Regex + fuzzy fallback intent matching.
 *
 * Used when Alice AI is unavailable. Matches natural language input
 * against 30+ built-in patterns with Levenshtein distance for typo tolerance.
 */
#pragma once

#include "straylight/result.h"

#include <algorithm>
#include <cmath>
#include <regex>
#include <string>
#include <vector>

namespace straylight::intent {

struct PatternMatch {
    std::string action_name;
    double confidence = 0.0;
    std::vector<std::string> captured_args;
};

struct PatternRule {
    std::string action_name;
    std::vector<std::string> patterns;       // regex patterns
    std::vector<std::string> keywords;       // for fuzzy matching
    int capture_group = 0;                   // which regex group to capture as arg
};

class PatternMatcher {
public:
    PatternMatcher() {
        init_builtin_patterns();
    }

    /** Match input text against all patterns. Returns best match or error. */
    Result<PatternMatch, std::string> match(const std::string& input) const {
        std::string lower = to_lower(input);

        // Phase 1: Exact regex matching (high confidence)
        PatternMatch best;
        best.confidence = 0.0;

        for (const auto& rule : rules_) {
            for (const auto& pat_str : rule.patterns) {
                try {
                    std::regex pat(pat_str, std::regex::icase | std::regex::ECMAScript);
                    std::smatch m;
                    if (std::regex_search(lower, m, pat)) {
                        double conf = 0.85 + 0.05 * (static_cast<double>(m[0].length()) /
                                                       static_cast<double>(lower.size()));
                        conf = std::min(conf, 0.95);

                        if (conf > best.confidence) {
                            best.action_name = rule.action_name;
                            best.confidence = conf;
                            best.captured_args.clear();

                            // Capture groups beyond the full match
                            for (size_t i = 1; i < m.size(); ++i) {
                                if (m[i].matched) {
                                    std::string arg = m[i].str();
                                    // Trim whitespace
                                    auto start = arg.find_first_not_of(" \t");
                                    auto end = arg.find_last_not_of(" \t");
                                    if (start != std::string::npos) {
                                        best.captured_args.push_back(
                                            arg.substr(start, end - start + 1));
                                    }
                                }
                            }
                        }
                    }
                } catch (const std::regex_error&) {
                    // Skip invalid patterns
                }
            }
        }

        // Phase 2: Fuzzy keyword matching (lower confidence)
        if (best.confidence < 0.5) {
            auto words = split_words(lower);
            for (const auto& rule : rules_) {
                double score = fuzzy_keyword_score(words, rule.keywords);
                if (score > best.confidence) {
                    best.action_name = rule.action_name;
                    best.confidence = score;
                    best.captured_args.clear();
                }
            }
        }

        if (best.confidence < 0.2) {
            return Result<PatternMatch, std::string>::error(
                "no matching pattern (best confidence: " +
                std::to_string(best.confidence) + ")");
        }

        return Result<PatternMatch, std::string>::ok(std::move(best));
    }

private:
    std::vector<PatternRule> rules_;

    void init_builtin_patterns() {
        // Screen recording
        rules_.push_back({
            "record_screen",
            {"record.*screen", "capture.*display.*video", "screen.*record",
             "start.*recording", "rec.*screen"},
            {"record", "screen", "capture", "video", "display"},
            0
        });

        rules_.push_back({
            "record_screen_4k",
            {"record.*screen.*4k", "record.*4k", "capture.*4k"},
            {"record", "screen", "4k", "high", "quality"},
            0
        });

        // Screenshot
        rules_.push_back({
            "screenshot",
            {"take.*screenshot", "screenshot", "screen.*shot", "screen.*grab",
             "capture.*screen", "grab.*screen", "print.*screen"},
            {"screenshot", "screen", "capture", "grab", "snap"},
            0
        });

        rules_.push_back({
            "screenshot_region",
            {"screenshot.*region", "screenshot.*area", "capture.*area",
             "screenshot.*select", "partial.*screenshot"},
            {"screenshot", "region", "area", "select", "partial"},
            0
        });

        // Wi-Fi
        rules_.push_back({
            "wifi_off",
            {"turn.*off.*wi-?fi", "disable.*wi-?fi", "wi-?fi.*off",
             "kill.*wi-?fi", "stop.*wi-?fi", "no.*wi-?fi"},
            {"wifi", "off", "disable", "turn", "kill"},
            0
        });

        rules_.push_back({
            "wifi_on",
            {"turn.*on.*wi-?fi", "enable.*wi-?fi", "wi-?fi.*on",
             "start.*wi-?fi", "connect.*wi-?fi"},
            {"wifi", "on", "enable", "turn", "connect"},
            0
        });

        rules_.push_back({
            "wifi_status",
            {"list.*wi-?fi", "scan.*wi-?fi", "available.*network",
             "show.*wi-?fi", "wi-?fi.*list"},
            {"wifi", "list", "scan", "available", "networks"},
            0
        });

        // Benchmark
        rules_.push_back({
            "run_benchmark",
            {"run.*bench", "bench.*mark", "performance.*test",
             "speed.*test", "straylight.*bench"},
            {"benchmark", "bench", "performance", "speed", "test"},
            0
        });

        // Health
        rules_.push_back({
            "check_health",
            {"check.*health", "system.*health", "health.*check",
             "run.*doctor", "diagnos", "system.*status"},
            {"health", "check", "doctor", "diagnose", "status"},
            0
        });

        // Encryption
        rules_.push_back({
            "encrypt_file",
            {"encrypt.*(?:file|document)\\s+(.+)", "seal\\s+(.+)",
             "lock.*(?:file|document)\\s+(.+)"},
            {"encrypt", "seal", "lock", "file", "secure"},
            1
        });

        rules_.push_back({
            "decrypt_file",
            {"decrypt.*(?:file|document)\\s+(.+)", "unseal\\s+(.+)",
             "unlock.*(?:file|document)\\s+(.+)"},
            {"decrypt", "unseal", "unlock", "file"},
            1
        });

        // Network scanning
        rules_.push_back({
            "scan_network",
            {"scan.*network", "network.*scan", "probe.*network",
             "discover.*node", "find.*device", "straylight.*probe"},
            {"scan", "network", "probe", "discover", "devices", "nodes"},
            0
        });

        // GPU
        rules_.push_back({
            "show_gpu_usage",
            {"show.*gpu", "gpu.*usage", "gpu.*status", "vpu.*usage",
             "graphics.*usage", "show.*vpu"},
            {"gpu", "vpu", "graphics", "usage", "utilization"},
            0
        });

        // Sandbox
        rules_.push_back({
            "create_sandbox",
            {"create.*sandbox\\s*(.*)", "sandbox.*for\\s+(.+)",
             "new.*sandbox\\s*(.*)", "isolate\\s+(.+)"},
            {"sandbox", "create", "isolate", "container", "new"},
            1
        });

        // Snapshot
        rules_.push_back({
            "snapshot_system",
            {"snapshot.*system", "system.*snapshot", "create.*snapshot",
             "take.*snapshot", "backup.*system"},
            {"snapshot", "system", "backup", "save", "state"},
            0
        });

        // Window management
        rules_.push_back({
            "toggle_fullscreen",
            {"fullscreen", "toggle.*fullscreen", "maximize.*window",
             "go.*fullscreen"},
            {"fullscreen", "maximize", "toggle"},
            0
        });

        rules_.push_back({
            "close_window",
            {"close.*window", "kill.*window", "exit.*window"},
            {"close", "kill", "window", "exit"},
            0
        });

        rules_.push_back({
            "move_workspace",
            {"move.*workspace\\s+(\\d+)", "send.*workspace\\s+(\\d+)",
             "workspace\\s+(\\d+)"},
            {"move", "workspace", "send"},
            1
        });

        // System
        rules_.push_back({
            "system_update",
            {"update.*system", "system.*update", "upgrade.*package",
             "apt.*upgrade", "install.*update"},
            {"update", "system", "upgrade", "packages"},
            0
        });

        rules_.push_back({
            "show_processes",
            {"show.*process", "list.*process", "top.*process",
             "running.*process", "what.*running"},
            {"process", "top", "running", "list", "show"},
            0
        });

        rules_.push_back({
            "disk_usage",
            {"disk.*usage", "storage.*usage", "disk.*space",
             "free.*space", "how.*much.*space", "check.*disk"},
            {"disk", "storage", "space", "usage", "free"},
            0
        });

        rules_.push_back({
            "memory_usage",
            {"memory.*usage", "ram.*usage", "show.*memory",
             "check.*memory", "how.*much.*ram", "free.*memory"},
            {"memory", "ram", "usage", "free"},
            0
        });

        rules_.push_back({
            "show_uptime",
            {"show.*uptime", "system.*uptime", "how.*long.*running",
             "uptime"},
            {"uptime", "running", "how", "long"},
            0
        });

        rules_.push_back({
            "show_temperature",
            {"show.*temp", "cpu.*temp", "system.*temp",
             "thermal.*status", "how.*hot"},
            {"temperature", "temp", "thermal", "hot", "cpu"},
            0
        });

        // File operations
        rules_.push_back({
            "find_large_files",
            {"find.*large.*file", "biggest.*file", "largest.*file",
             "huge.*file"},
            {"find", "large", "big", "huge", "files"},
            0
        });

        rules_.push_back({
            "compress_file",
            {"compress\\s+(.+)", "zip\\s+(.+)", "zstd\\s+(.+)"},
            {"compress", "zip", "zstd", "squeeze"},
            1
        });

        // Network status
        rules_.push_back({
            "network_status",
            {"network.*status", "show.*network", "check.*network",
             "connection.*status", "internet.*status"},
            {"network", "status", "connection", "internet", "check"},
            0
        });

        // Service control
        rules_.push_back({
            "restart_compositor",
            {"restart.*compositor", "compositor.*restart",
             "reload.*compositor"},
            {"restart", "compositor", "reload", "display"},
            0
        });

        rules_.push_back({
            "restart_alice",
            {"restart.*alice", "alice.*restart", "reload.*alice",
             "reboot.*alice"},
            {"restart", "alice", "reload", "reboot", "ai"},
            0
        });

        // Pipeline
        rules_.push_back({
            "entropy_seal_transport",
            {"secure.*transfer\\s+(.+)\\s+to\\s+(.+)",
             "seal.*send\\s+(.+)\\s+to\\s+(.+)",
             "entropy.*seal.*transport\\s+(.+)\\s+(.+)"},
            {"secure", "transfer", "seal", "send", "entropy", "transport"},
            0
        });
    }

    // ─── String utilities ───────────────────────────────────────────────

    static std::string to_lower(const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return out;
    }

    static std::vector<std::string> split_words(const std::string& s) {
        std::vector<std::string> words;
        std::string word;
        for (char c : s) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                word += c;
            } else {
                if (!word.empty()) {
                    words.push_back(word);
                    word.clear();
                }
            }
        }
        if (!word.empty()) words.push_back(word);
        return words;
    }

    /** Levenshtein distance between two strings. */
    static int levenshtein(const std::string& a, const std::string& b) {
        int m = static_cast<int>(a.size());
        int n = static_cast<int>(b.size());
        std::vector<int> prev(n + 1), curr(n + 1);

        for (int j = 0; j <= n; ++j) prev[j] = j;

        for (int i = 1; i <= m; ++i) {
            curr[0] = i;
            for (int j = 1; j <= n; ++j) {
                int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
            }
            std::swap(prev, curr);
        }
        return prev[n];
    }

    /** Score how well input words match a set of keywords.
     *  Uses exact substring match and Levenshtein fuzzy match. */
    double fuzzy_keyword_score(const std::vector<std::string>& input_words,
                               const std::vector<std::string>& keywords) const {
        if (keywords.empty() || input_words.empty()) return 0.0;

        int matched = 0;
        double total_similarity = 0.0;

        for (const auto& keyword : keywords) {
            double best_sim = 0.0;
            for (const auto& word : input_words) {
                // Exact substring match
                if (word.find(keyword) != std::string::npos ||
                    keyword.find(word) != std::string::npos) {
                    best_sim = 1.0;
                    break;
                }
                // Levenshtein similarity
                int dist = levenshtein(word, keyword);
                int max_len = std::max(static_cast<int>(word.size()),
                                       static_cast<int>(keyword.size()));
                if (max_len > 0) {
                    double sim = 1.0 - static_cast<double>(dist) / max_len;
                    // Only count as fuzzy match if similarity > 0.6 (typo tolerance)
                    if (sim > 0.6) {
                        best_sim = std::max(best_sim, sim * 0.8); // Penalize fuzzy
                    }
                }
            }
            if (best_sim > 0.3) {
                ++matched;
                total_similarity += best_sim;
            }
        }

        if (matched == 0) return 0.0;

        // Score = (fraction of keywords matched) * (average similarity) * scaling
        double match_frac = static_cast<double>(matched) / keywords.size();
        double avg_sim = total_similarity / matched;

        // Cap fuzzy-only matches at 0.65 confidence
        return std::min(0.65, match_frac * avg_sim * 0.7);
    }
};

} // namespace straylight::intent
