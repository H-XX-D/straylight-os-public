// services/voice/conversation.h
// Conversation state management with sliding window and tool call tracking.
#pragma once

#include "straylight/result.h"

#include <chrono>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace straylight::voice {

/// Role of a conversation participant.
enum class Role {
    System,
    User,
    Assistant,
    Tool
};

inline const char* role_str(Role r) {
    switch (r) {
        case Role::System:    return "system";
        case Role::User:      return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool:      return "tool";
    }
    return "unknown";
}

inline Role role_from_str(const std::string& s) {
    if (s == "system")    return Role::System;
    if (s == "user")      return Role::User;
    if (s == "assistant") return Role::Assistant;
    if (s == "tool")      return Role::Tool;
    return Role::User;
}

/// A single turn in the conversation.
struct ConversationTurn {
    Role        role = Role::User;
    std::string content;
    std::string tool_name;       // if role == Tool, which tool produced this
    std::string tool_call_id;    // correlation ID for tool call/result pairs
    double      audio_duration = 0.0; // if from speech, how long the audio was
    std::chrono::system_clock::time_point timestamp =
        std::chrono::system_clock::now();

    /// Serialize to JSON string.
    std::string to_json() const {
        std::ostringstream out;
        out << "{\"role\": \"" << role_str(role) << "\", \"content\": \"";
        // Escape content.
        for (char c : content) {
            switch (c) {
                case '"':  out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:   out << c;
            }
        }
        out << "\"";
        if (!tool_name.empty()) {
            out << ", \"tool_name\": \"" << tool_name << "\"";
        }
        if (!tool_call_id.empty()) {
            out << ", \"tool_call_id\": \"" << tool_call_id << "\"";
        }
        if (audio_duration > 0.0) {
            out << ", \"audio_duration\": " << audio_duration;
        }
        auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
            timestamp.time_since_epoch()).count();
        out << ", \"timestamp\": " << epoch;
        out << "}";
        return out.str();
    }
};

/// Manages conversation history with a sliding window.
class Conversation {
public:
    explicit Conversation(int max_turns = 10) : max_turns_(max_turns) {}

    /// Add a user turn.
    void add_user(const std::string& text, double audio_dur = 0.0) {
        ConversationTurn turn;
        turn.role = Role::User;
        turn.content = text;
        turn.audio_duration = audio_dur;
        add_turn(std::move(turn));
    }

    /// Add an assistant turn.
    void add_assistant(const std::string& text) {
        ConversationTurn turn;
        turn.role = Role::Assistant;
        turn.content = text;
        add_turn(std::move(turn));
    }

    /// Add a tool result turn.
    void add_tool_result(const std::string& tool_name,
                         const std::string& result,
                         const std::string& call_id = "") {
        ConversationTurn turn;
        turn.role = Role::Tool;
        turn.content = result;
        turn.tool_name = tool_name;
        turn.tool_call_id = call_id;
        add_turn(std::move(turn));
    }

    /// Get all turns in the current window.
    const std::deque<ConversationTurn>& turns() const { return turns_; }

    /// Get the number of turns.
    size_t size() const { return turns_.size(); }

    /// Clear all history.
    void clear() {
        turns_.clear();
        total_turns_ = 0;
    }

    /// Total turns ever added (including evicted ones).
    int total_turns() const { return total_turns_; }

    /// Build a text representation of the conversation for LLM context.
    std::string to_prompt_text() const {
        std::ostringstream out;
        for (const auto& turn : turns_) {
            switch (turn.role) {
                case Role::User:
                    out << "<|user|>\n" << turn.content << "\n";
                    break;
                case Role::Assistant:
                    out << "<|assistant|>\n" << turn.content << "\n";
                    break;
                case Role::Tool:
                    out << "<|tool|> [" << turn.tool_name << "]\n"
                        << turn.content << "\n";
                    break;
                case Role::System:
                    out << "<|system|>\n" << turn.content << "\n";
                    break;
            }
        }
        return out.str();
    }

    /// Save conversation to a JSON file.
    Result<void, std::string> save(const std::string& path) const {
        std::ofstream out(path);
        if (!out.is_open()) {
            return Result<void, std::string>::error("cannot open: " + path);
        }

        out << "[\n";
        for (size_t i = 0; i < turns_.size(); ++i) {
            if (i > 0) out << ",\n";
            out << "  " << turns_[i].to_json();
        }
        out << "\n]\n";
        out.close();

        return Result<void, std::string>::ok();
    }

    /// Load conversation from a JSON file.
    Result<void, std::string> load(const std::string& path) {
        std::ifstream in(path);
        if (!in.is_open()) {
            return Result<void, std::string>::error("cannot open: " + path);
        }

        // Simple JSON array parser.
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());

        turns_.clear();
        total_turns_ = 0;

        // Find each object in the array.
        size_t pos = 0;
        while (pos < content.size()) {
            auto obj_start = content.find('{', pos);
            if (obj_start == std::string::npos) break;

            // Find matching closing brace.
            int depth = 0;
            size_t obj_end = obj_start;
            for (size_t i = obj_start; i < content.size(); ++i) {
                if (content[i] == '{') ++depth;
                if (content[i] == '}') {
                    --depth;
                    if (depth == 0) { obj_end = i; break; }
                }
            }

            std::string obj = content.substr(obj_start, obj_end - obj_start + 1);

            // Extract fields.
            ConversationTurn turn;

            auto extract_str = [&](const std::string& key) -> std::string {
                std::string search = "\"" + key + "\": \"";
                auto p = obj.find(search);
                if (p == std::string::npos) {
                    search = "\"" + key + "\":\"";
                    p = obj.find(search);
                }
                if (p == std::string::npos) return "";
                p += search.size();
                std::string result;
                while (p < obj.size() && obj[p] != '"') {
                    if (obj[p] == '\\' && p + 1 < obj.size()) {
                        ++p;
                        switch (obj[p]) {
                            case 'n': result += '\n'; break;
                            case 't': result += '\t'; break;
                            case '"': result += '"'; break;
                            case '\\': result += '\\'; break;
                            default: result += obj[p]; break;
                        }
                    } else {
                        result += obj[p];
                    }
                    ++p;
                }
                return result;
            };

            turn.role = role_from_str(extract_str("role"));
            turn.content = extract_str("content");
            turn.tool_name = extract_str("tool_name");
            turn.tool_call_id = extract_str("tool_call_id");

            turns_.push_back(std::move(turn));
            ++total_turns_;
            pos = obj_end + 1;
        }

        // Trim to window size.
        while (static_cast<int>(turns_.size()) > max_turns_) {
            turns_.pop_front();
        }

        return Result<void, std::string>::ok();
    }

    /// Set the maximum window size.
    void set_max_turns(int n) { max_turns_ = n; }

private:
    std::deque<ConversationTurn> turns_;
    int max_turns_ = 10;
    int total_turns_ = 0;

    void add_turn(ConversationTurn turn) {
        turns_.push_back(std::move(turn));
        ++total_turns_;

        // Evict old turns beyond the window (but keep System turns).
        while (static_cast<int>(turns_.size()) > max_turns_) {
            if (turns_.front().role != Role::System) {
                turns_.pop_front();
            } else {
                // Move system turn to stay, evict the next one.
                if (turns_.size() > 1) {
                    turns_.erase(turns_.begin() + 1);
                } else {
                    break;
                }
            }
        }
    }
};

} // namespace straylight::voice
