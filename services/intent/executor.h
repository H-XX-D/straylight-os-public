/**
 * StrayLight Intent Executor — Runs resolved intents as system commands.
 *
 * Supports single commands, pipelines (chained commands), confirmation
 * mode for destructive actions, and an audit log of all executed intents.
 */
#pragma once

#include "intent_engine.h"
#include "straylight/result.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace straylight::intent {

// ─── Types ──────────────────────────────────────────────────────────────────

struct ExecutionResult {
    bool success = false;
    int exit_code = -1;
    std::string output;
    std::string error_output;
    std::chrono::milliseconds duration{0};
};

struct AuditEntry {
    std::string timestamp;
    std::string natural_text;
    std::string action_name;
    std::vector<std::string> commands;
    bool executed = false;
    bool success = false;
    int exit_code = -1;
    std::string output_summary;
};

// ─── Executor ───────────────────────────────────────────────────────────────

class Executor {
public:
    Executor() {
        ensure_audit_dir();
    }

    /** Execute a resolved intent. Returns per-command results. */
    std::vector<ExecutionResult> execute(const IntentResult& intent,
                                        const std::string& original_text = "") {
        std::vector<ExecutionResult> results;

        if (intent.action_type == ActionType::pipeline) {
            results = execute_pipeline(intent.commands);
        } else {
            for (const auto& cmd : intent.commands) {
                results.push_back(execute_single(cmd));
            }
        }

        // Write audit log
        AuditEntry entry;
        entry.timestamp = now_iso8601();
        entry.natural_text = original_text;
        entry.action_name = intent.action_name;
        entry.commands = intent.commands;
        entry.executed = true;

        bool all_ok = true;
        std::string combined_output;
        for (const auto& r : results) {
            if (!r.success) all_ok = false;
            if (!r.output.empty()) {
                combined_output += r.output;
                if (combined_output.back() != '\n') combined_output += '\n';
            }
        }
        entry.success = all_ok;
        entry.exit_code = results.empty() ? -1 : results.back().exit_code;
        // Truncate output summary to 500 chars
        entry.output_summary = combined_output.substr(0, 500);

        write_audit(entry);

        return results;
    }

    /** Execute a single shell command, capturing stdout and stderr. */
    ExecutionResult execute_single(const std::string& command) {
        ExecutionResult result;
        auto start = std::chrono::steady_clock::now();

        // Create pipes for stdout and stderr
        int stdout_pipe[2];
        int stderr_pipe[2];
        if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
            result.success = false;
            result.error_output = "pipe() failed: " + std::string(strerror(errno));
            return result;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(stdout_pipe[0]); close(stdout_pipe[1]);
            close(stderr_pipe[0]); close(stderr_pipe[1]);
            result.success = false;
            result.error_output = "fork() failed: " + std::string(strerror(errno));
            return result;
        }

        if (pid == 0) {
            // Child process
            close(stdout_pipe[0]);
            close(stderr_pipe[0]);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            dup2(stderr_pipe[1], STDERR_FILENO);
            close(stdout_pipe[1]);
            close(stderr_pipe[1]);

            execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
            _exit(127);
        }

        // Parent process
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        // Read stdout
        {
            char buf[4096];
            ssize_t n;
            while ((n = read(stdout_pipe[0], buf, sizeof(buf) - 1)) > 0) {
                buf[n] = '\0';
                result.output += buf;
            }
            close(stdout_pipe[0]);
        }

        // Read stderr
        {
            char buf[4096];
            ssize_t n;
            while ((n = read(stderr_pipe[0], buf, sizeof(buf) - 1)) > 0) {
                buf[n] = '\0';
                result.error_output += buf;
            }
            close(stderr_pipe[0]);
        }

        int status = 0;
        waitpid(pid, &status, 0);

        auto end = std::chrono::steady_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
            result.success = (result.exit_code == 0);
        } else {
            result.exit_code = -1;
            result.success = false;
        }

        return result;
    }

    /** Execute commands as a pipeline (output of each feeds into next). */
    std::vector<ExecutionResult> execute_pipeline(const std::vector<std::string>& commands) {
        std::vector<ExecutionResult> results;
        if (commands.empty()) return results;

        // For a true pipeline, join commands with | and execute as one shell command
        if (commands.size() == 1) {
            results.push_back(execute_single(commands[0]));
            return results;
        }

        // Chain commands: if any fails, stop the pipeline
        std::string previous_output;
        for (size_t i = 0; i < commands.size(); ++i) {
            std::string cmd = commands[i];

            // If previous command produced output, pipe it via echo
            if (!previous_output.empty() && i > 0) {
                // Create a temp file with the previous output for commands that need file input
                cmd = "echo " + shell_quote(previous_output) + " | " + cmd;
            }

            auto r = execute_single(cmd);
            results.push_back(r);

            if (!r.success) break; // Pipeline stops on first failure

            previous_output = r.output;
        }

        return results;
    }

    /** Log an intent that was NOT executed (e.g., dry-run or user declined). */
    void audit_skipped(const IntentResult& intent, const std::string& original_text,
                       const std::string& reason) {
        AuditEntry entry;
        entry.timestamp = now_iso8601();
        entry.natural_text = original_text;
        entry.action_name = intent.action_name;
        entry.commands = intent.commands;
        entry.executed = false;
        entry.success = false;
        entry.output_summary = "SKIPPED: " + reason;

        write_audit(entry);
    }

    /** Get the path to the audit log file. */
    std::string audit_log_path() const {
        return audit_dir_ + "/intent_audit.log";
    }

private:
    std::string audit_dir_ = "/var/log/straylight";
    std::mutex audit_mtx_;

    void ensure_audit_dir() {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(audit_dir_, ec);
        // If /var/log/straylight isn't writable, fall back to user dir
        if (ec || !fs::is_directory(audit_dir_, ec)) {
            const char* home = getenv("HOME");
            if (home) {
                audit_dir_ = std::string(home) + "/.local/share/straylight/logs";
                fs::create_directories(audit_dir_, ec);
            }
        }
    }

    void write_audit(const AuditEntry& entry) {
        std::lock_guard<std::mutex> lock(audit_mtx_);
        std::ofstream f(audit_log_path(), std::ios::app);
        if (!f) return;

        f << "[" << entry.timestamp << "] ";
        f << (entry.executed ? "EXEC" : "SKIP") << " ";
        f << (entry.success ? "OK" : "FAIL") << " ";
        f << "action=" << entry.action_name << " ";
        f << "text=\"" << entry.natural_text << "\" ";
        f << "commands=[";
        for (size_t i = 0; i < entry.commands.size(); ++i) {
            if (i > 0) f << " | ";
            f << entry.commands[i];
        }
        f << "] ";
        if (entry.executed) {
            f << "exit=" << entry.exit_code << " ";
        }
        if (!entry.output_summary.empty()) {
            // Replace newlines in summary for single-line log
            std::string summary = entry.output_summary;
            for (auto& c : summary) {
                if (c == '\n') c = ' ';
            }
            if (summary.size() > 200) {
                summary = summary.substr(0, 200) + "...";
            }
            f << "output=\"" << summary << "\"";
        }
        f << "\n";
    }

    static std::string now_iso8601() {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf{};
        localtime_r(&tt, &tm_buf);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
        return buf;
    }

    static std::string shell_quote(const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out += c;
            }
        }
        out += "'";
        return out;
    }
};

} // namespace straylight::intent
