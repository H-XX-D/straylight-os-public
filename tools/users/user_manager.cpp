// tools/users/user_manager.cpp
// Full implementation of user account management for StrayLight OS.

#include "user_manager.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

UserManager::UserManager() = default;
UserManager::~UserManager() = default;

Result<std::string, std::string> UserManager::run_cmd(const std::string& cmd) const {
    std::array<char, 8192> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error(
            "popen failed: " + std::string(strerror(errno)));
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    int rc = pclose(pipe);
    if (rc != 0) {
        return Result<std::string, std::string>::error(
            "command failed (rc=" + std::to_string(rc) + "): " + cmd +
            "\noutput: " + output);
    }
    return Result<std::string, std::string>::ok(output);
}

std::string UserManager::home_dir(const std::string& username) const {
    std::ifstream passwd("/etc/passwd");
    if (!passwd.is_open()) return "/home/" + username;

    std::string line;
    while (std::getline(passwd, line)) {
        // Format: username:x:uid:gid:gecos:home:shell
        auto first_colon = line.find(':');
        if (first_colon == std::string::npos) continue;
        std::string name = line.substr(0, first_colon);
        if (name != username) continue;

        // Skip to home field (6th field, 0-indexed field 5)
        size_t pos = 0;
        for (int i = 0; i < 5; ++i) {
            pos = line.find(':', pos + 1);
            if (pos == std::string::npos) return "/home/" + username;
        }
        auto home_end = line.find(':', pos + 1);
        return line.substr(pos + 1, home_end - pos - 1);
    }

    return "/home/" + username;
}

std::string UserManager::ssh_keys_path(const std::string& username) const {
    return home_dir(username) + "/.ssh/authorized_keys";
}

std::string UserManager::sudoers_path(const std::string& username) const {
    return "/etc/sudoers.d/straylight-" + username;
}

// ---------------------------------------------------------------------------
// Parsing /etc/passwd
// ---------------------------------------------------------------------------

std::vector<UserAccount> UserManager::parse_passwd(bool include_system) const {
    std::vector<UserAccount> users;
    std::ifstream passwd("/etc/passwd");
    if (!passwd.is_open()) return users;

    std::string line;
    while (std::getline(passwd, line)) {
        if (line.empty() || line[0] == '#') continue;

        // Split by ':'
        std::vector<std::string> fields;
        size_t pos = 0;
        while (pos < line.size()) {
            auto next = line.find(':', pos);
            if (next == std::string::npos) {
                fields.push_back(line.substr(pos));
                break;
            }
            fields.push_back(line.substr(pos, next - pos));
            pos = next + 1;
        }

        if (fields.size() < 7) continue;

        UserAccount user;
        user.username = fields[0];
        user.uid = std::stoul(fields[2]);
        user.gid = std::stoul(fields[3]);
        user.full_name = fields[4];
        // GECOS can have sub-fields separated by commas
        auto comma = user.full_name.find(',');
        if (comma != std::string::npos) {
            user.full_name = user.full_name.substr(0, comma);
        }
        user.home_dir = fields[5];
        user.shell = fields[6];

        // Filter system users (UID < 1000, except root)
        if (!include_system && user.uid != 0 && user.uid < 1000) continue;
        // Skip nologin/false shell users unless include_system
        if (!include_system &&
            (user.shell == "/usr/sbin/nologin" ||
             user.shell == "/bin/false" ||
             user.shell == "/sbin/nologin")) continue;

        // Get groups
        user.groups = get_groups(user.username);

        // Check sudo
        user.has_sudo = has_sudo(user.username);

        // Check if locked
        user.locked = is_locked(user.username);

        // Check for SSH keys
        std::string keys_path = ssh_keys_path(user.username);
        if (fs::exists(keys_path)) {
            std::ifstream kf(keys_path);
            std::string kline;
            while (std::getline(kf, kline)) {
                if (!kline.empty() && kline[0] != '#') {
                    user.ssh_key_count++;
                }
            }
        }

        // Check password status
        auto pw_res = run_cmd("passwd -S " + user.username + " 2>/dev/null");
        if (pw_res.has_value()) {
            std::string pw_status = pw_res.value();
            user.has_password = (pw_status.find(" P ") != std::string::npos);
        }

        users.push_back(user);
    }

    return users;
}

std::vector<std::string> UserManager::get_groups(const std::string& username) const {
    std::vector<std::string> groups;
    auto res = run_cmd("id -Gn " + username + " 2>/dev/null");
    if (!res.has_value()) return groups;

    std::istringstream stream(res.value());
    std::string group;
    while (stream >> group) {
        groups.push_back(group);
    }
    return groups;
}

bool UserManager::is_locked(const std::string& username) const {
    auto res = run_cmd("passwd -S " + username + " 2>/dev/null");
    if (!res.has_value()) return false;
    // "L" means locked, "P" means has password
    return (res.value().find(" L ") != std::string::npos);
}

bool UserManager::has_sudo(const std::string& username) const {
    // Check /etc/sudoers.d/
    if (fs::exists(sudoers_path(username))) return true;

    // Check if user is in sudo/wheel group
    auto groups = get_groups(username);
    for (const auto& g : groups) {
        if (g == "sudo" || g == "wheel") return true;
    }

    // Check main sudoers file
    auto res = run_cmd("grep -q '^" + username + "\\s' /etc/sudoers 2>/dev/null");
    return res.has_value();
}

// ---------------------------------------------------------------------------
// list / get
// ---------------------------------------------------------------------------

Result<std::vector<UserAccount>, std::string> UserManager::list_users(
    bool include_system) const {
    auto users = parse_passwd(include_system);

    // Get last login info
    auto lastlog_res = run_cmd("lastlog 2>/dev/null");
    if (lastlog_res.has_value()) {
        std::istringstream stream(lastlog_res.value());
        std::string line;
        std::getline(stream, line); // skip header
        while (std::getline(stream, line)) {
            if (line.find("**Never logged in**") != std::string::npos) continue;
            // "username  pts/0  192.0.2.1  Mon Mar 15 10:30:45 +0000 2024"
            std::istringstream ls(line);
            std::string uname;
            ls >> uname;
            for (auto& u : users) {
                if (u.username == uname) {
                    // Extract the rest as last login
                    std::string rest;
                    std::getline(ls, rest);
                    auto pos = rest.find_first_not_of(" \t");
                    if (pos != std::string::npos) {
                        u.last_login = rest.substr(pos);
                        // Trim trailing whitespace
                        auto end = u.last_login.find_last_not_of(" \t\n\r");
                        if (end != std::string::npos) {
                            u.last_login = u.last_login.substr(0, end + 1);
                        }
                    }
                    break;
                }
            }
        }
    }

    return Result<std::vector<UserAccount>, std::string>::ok(users);
}

Result<UserAccount, std::string> UserManager::get_user(const std::string& username) const {
    auto res = list_users(true);
    if (!res.has_value()) {
        return Result<UserAccount, std::string>::error(res.error());
    }

    for (const auto& u : res.value()) {
        if (u.username == username) {
            return Result<UserAccount, std::string>::ok(u);
        }
    }

    return Result<UserAccount, std::string>::error("user not found: " + username);
}

// ---------------------------------------------------------------------------
// add / delete / modify
// ---------------------------------------------------------------------------

Result<void, std::string> UserManager::add_user(
    const std::string& username,
    const std::string& shell,
    const std::string& home,
    const std::vector<std::string>& groups,
    const std::string& full_name,
    bool create_home) {

    std::ostringstream cmd;
    cmd << "useradd";
    if (create_home) cmd << " -m";
    if (!shell.empty()) cmd << " -s " << shell;
    if (!home.empty()) cmd << " -d " << home;
    if (!full_name.empty()) cmd << " -c '" << full_name << "'";
    if (!groups.empty()) {
        cmd << " -G ";
        for (size_t i = 0; i < groups.size(); ++i) {
            if (i > 0) cmd << ",";
            cmd << groups[i];
        }
    }
    cmd << " " << username << " 2>&1";

    auto res = run_cmd(cmd.str());
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to add user: " + res.error());
    }

    // Create .ssh directory
    std::string user_home = home.empty() ? ("/home/" + username) : home;
    std::string ssh_dir = user_home + "/.ssh";
    fs::create_directories(ssh_dir);
    run_cmd("chown " + username + ":" + username + " " + ssh_dir + " 2>/dev/null");
    run_cmd("chmod 700 " + ssh_dir + " 2>/dev/null");

    return Result<void, std::string>::ok();
}

Result<void, std::string> UserManager::delete_user(const std::string& username,
                                                     bool remove_home) {
    // Check if user exists
    auto user_res = get_user(username);
    if (!user_res.has_value()) {
        return Result<void, std::string>::error(user_res.error());
    }

    std::string cmd = "userdel";
    if (remove_home) cmd += " -r";
    cmd += " " + username + " 2>&1";

    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to delete user: " + res.error());
    }

    // Remove sudoers entry
    std::string sudoers = sudoers_path(username);
    if (fs::exists(sudoers)) {
        fs::remove(sudoers);
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> UserManager::modify_user(
    const std::string& username,
    const std::string& shell,
    const std::vector<std::string>& add_groups,
    const std::vector<std::string>& remove_groups,
    const std::string& full_name) {

    // Modify basic properties
    if (!shell.empty() || !full_name.empty()) {
        std::ostringstream cmd;
        cmd << "usermod";
        if (!shell.empty()) cmd << " -s " << shell;
        if (!full_name.empty()) cmd << " -c '" << full_name << "'";
        cmd << " " << username << " 2>&1";

        auto res = run_cmd(cmd.str());
        if (!res.has_value()) {
            return Result<void, std::string>::error("failed to modify user: " + res.error());
        }
    }

    // Add to groups
    for (const auto& group : add_groups) {
        auto res = run_cmd("usermod -aG " + group + " " + username + " 2>&1");
        if (!res.has_value()) {
            return Result<void, std::string>::error(
                "failed to add user to group " + group + ": " + res.error());
        }
    }

    // Remove from groups
    for (const auto& group : remove_groups) {
        auto res = run_cmd("gpasswd -d " + username + " " + group + " 2>&1");
        if (!res.has_value()) {
            return Result<void, std::string>::error(
                "failed to remove user from group " + group + ": " + res.error());
        }
    }

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Sudo management
// ---------------------------------------------------------------------------

Result<void, std::string> UserManager::grant_sudo(const std::string& username) {
    // Add to sudo group
    auto res = run_cmd("usermod -aG sudo " + username + " 2>&1");
    if (!res.has_value()) {
        // Try wheel group (RHEL/Fedora)
        res = run_cmd("usermod -aG wheel " + username + " 2>&1");
    }

    // Also create a sudoers.d entry for fine-grained control
    std::string path = sudoers_path(username);
    std::ofstream out(path);
    if (out.is_open()) {
        out << "# StrayLight OS sudo configuration for " << username << "\n"
            << username << " ALL=(ALL:ALL) ALL\n";
        out.close();
        run_cmd("chmod 440 " + path + " 2>/dev/null");
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> UserManager::revoke_sudo(const std::string& username) {
    // Remove from sudo group
    run_cmd("gpasswd -d " + username + " sudo 2>/dev/null");
    run_cmd("gpasswd -d " + username + " wheel 2>/dev/null");

    // Remove sudoers.d entry
    std::string path = sudoers_path(username);
    if (fs::exists(path)) {
        fs::remove(path);
    }

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Lock / Unlock
// ---------------------------------------------------------------------------

Result<void, std::string> UserManager::lock_user(const std::string& username) {
    auto res = run_cmd("passwd -l " + username + " 2>&1");
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to lock user: " + res.error());
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> UserManager::unlock_user(const std::string& username) {
    auto res = run_cmd("passwd -u " + username + " 2>&1");
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to unlock user: " + res.error());
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// SSH keys
// ---------------------------------------------------------------------------

Result<void, std::string> UserManager::add_ssh_key(const std::string& username,
                                                     const std::string& key) {
    std::string user_home = home_dir(username);
    std::string ssh_dir = user_home + "/.ssh";
    std::string keys_path = ssh_dir + "/authorized_keys";

    // Ensure .ssh directory exists
    if (!fs::exists(ssh_dir)) {
        fs::create_directories(ssh_dir);
        run_cmd("chown " + username + ":" + username + " " + ssh_dir + " 2>/dev/null");
        run_cmd("chmod 700 " + ssh_dir + " 2>/dev/null");
    }

    // Validate key format
    std::regex key_re(R"(^(ssh-rsa|ssh-ed25519|ecdsa-sha2-\S+|ssh-dss)\s+\S+.*)");
    if (!std::regex_match(key, key_re)) {
        // Try reading from file
        if (fs::exists(key)) {
            std::ifstream kf(key);
            std::string file_key;
            std::getline(kf, file_key);
            if (!std::regex_match(file_key, key_re)) {
                return Result<void, std::string>::error(
                    "invalid SSH public key format");
            }
            // Append to authorized_keys
            std::ofstream out(keys_path, std::ios::app);
            if (!out.is_open()) {
                return Result<void, std::string>::error("cannot write to " + keys_path);
            }
            out << file_key << "\n";
        } else {
            return Result<void, std::string>::error("invalid SSH public key format");
        }
    } else {
        std::ofstream out(keys_path, std::ios::app);
        if (!out.is_open()) {
            return Result<void, std::string>::error("cannot write to " + keys_path);
        }
        out << key << "\n";
    }

    // Set correct permissions
    run_cmd("chown " + username + ":" + username + " " + keys_path + " 2>/dev/null");
    run_cmd("chmod 600 " + keys_path + " 2>/dev/null");

    return Result<void, std::string>::ok();
}

Result<void, std::string> UserManager::remove_ssh_key(const std::string& username,
                                                        int index) {
    std::string keys_path = ssh_keys_path(username);
    std::ifstream f(keys_path);
    if (!f.is_open()) {
        return Result<void, std::string>::error("no authorized_keys file found");
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        lines.push_back(line);
    }
    f.close();

    // Count non-empty, non-comment lines to find the right one
    int key_idx = 0;
    int line_to_remove = -1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        if (!lines[i].empty() && lines[i][0] != '#') {
            if (key_idx == index) {
                line_to_remove = i;
                break;
            }
            ++key_idx;
        }
    }

    if (line_to_remove < 0) {
        return Result<void, std::string>::error(
            "key index " + std::to_string(index) + " not found");
    }

    lines.erase(lines.begin() + line_to_remove);

    std::ofstream out(keys_path);
    if (!out.is_open()) {
        return Result<void, std::string>::error("cannot write to " + keys_path);
    }
    for (const auto& l : lines) {
        out << l << "\n";
    }

    return Result<void, std::string>::ok();
}

Result<std::vector<SshKey>, std::string> UserManager::list_ssh_keys(
    const std::string& username) const {

    std::vector<SshKey> keys;
    std::string keys_path = ssh_keys_path(username);

    std::ifstream f(keys_path);
    if (!f.is_open()) {
        return Result<std::vector<SshKey>, std::string>::ok(keys);
    }

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;

        SshKey key;
        key.full_key = line;

        // Parse: type base64data [comment]
        std::istringstream ss(line);
        ss >> key.type;
        std::string b64;
        ss >> b64;
        std::getline(ss, key.comment);
        // Trim comment
        auto pos = key.comment.find_first_not_of(" \t");
        if (pos != std::string::npos) key.comment = key.comment.substr(pos);

        // Generate fingerprint
        // Write key to temp file and use ssh-keygen
        std::string tmp = "/tmp/.straylight_ssh_key_tmp";
        {
            std::ofstream tmp_f(tmp);
            if (tmp_f.is_open()) tmp_f << line << "\n";
        }
        auto fp_res = run_cmd("ssh-keygen -lf " + tmp + " 2>/dev/null");
        if (fp_res.has_value()) {
            // Output: "2048 SHA256:xxxxx comment (RSA)"
            std::string fp = fp_res.value();
            std::regex fp_re(R"((\d+)\s+(SHA256:\S+))");
            std::smatch m;
            if (std::regex_search(fp, m, fp_re)) {
                key.fingerprint = m[2].str();
            }
        }
        fs::remove(tmp);

        keys.push_back(key);
    }

    return Result<std::vector<SshKey>, std::string>::ok(keys);
}

// ---------------------------------------------------------------------------
// Login history
// ---------------------------------------------------------------------------

Result<std::vector<LoginRecord>, std::string> UserManager::login_history(
    const std::string& username,
    int max_entries) const {

    std::vector<LoginRecord> records;
    std::string cmd = "last -n " + std::to_string(max_entries);
    if (!username.empty()) cmd += " " + username;
    cmd += " 2>/dev/null";

    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<std::vector<LoginRecord>, std::string>::error(
            "failed to get login history: " + res.error());
    }

    std::istringstream stream(res.value());
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        if (line.find("wtmp begins") != std::string::npos) break;
        if (line.find("reboot") != std::string::npos) continue;

        LoginRecord record;

        // Format: "username  pts/0  192.0.2.1  Mon Mar 15 10:30 - 11:30 (01:00)"
        // or:     "username  pts/0  192.0.2.1  Mon Mar 15 10:30   still logged in"
        std::istringstream ls(line);
        ls >> record.username >> record.terminal >> record.from_host;

        // Rest is the time info
        std::string rest;
        std::getline(ls, rest);
        auto tpos = rest.find_first_not_of(" \t");
        if (tpos != std::string::npos) rest = rest.substr(tpos);

        record.still_logged_in = (rest.find("still logged in") != std::string::npos);

        // Parse login time and duration
        auto dash = rest.find(" - ");
        if (dash != std::string::npos) {
            record.login_time = rest.substr(0, dash);
            std::string after = rest.substr(dash + 3);
            auto paren = after.find('(');
            if (paren != std::string::npos) {
                record.logout_time = after.substr(0, paren);
                // Trim
                auto end = record.logout_time.find_last_not_of(" \t");
                if (end != std::string::npos) record.logout_time = record.logout_time.substr(0, end + 1);
                auto close_paren = after.find(')');
                if (close_paren != std::string::npos) {
                    record.duration = after.substr(paren + 1, close_paren - paren - 1);
                }
            }
        } else {
            record.login_time = rest;
        }

        records.push_back(record);
    }

    // Also check /var/log/auth.log for failed logins
    if (!username.empty()) {
        auto auth_res = run_cmd("grep 'Failed password.*" + username +
                                "' /var/log/auth.log 2>/dev/null | tail -5");
        // We include these in the output below through the CLI, not in the records
    }

    return Result<std::vector<LoginRecord>, std::string>::ok(records);
}

// ---------------------------------------------------------------------------
// Password
// ---------------------------------------------------------------------------

Result<void, std::string> UserManager::set_password(const std::string& username,
                                                      const std::string& password) {
    // Use chpasswd for non-interactive password setting
    std::string cmd = "echo '" + username + ":" + password + "' | chpasswd 2>&1";
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to set password: " + res.error());
    }
    return Result<void, std::string>::ok();
}

PasswordPolicy UserManager::get_password_policy() const {
    PasswordPolicy policy;

    // Read from /etc/login.defs
    std::ifstream f("/etc/login.defs");
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            std::string key;
            ss >> key;
            if (key == "PASS_MAX_DAYS") ss >> policy.max_days;
            else if (key == "PASS_MIN_DAYS") ss >> policy.min_days;
            else if (key == "PASS_WARN_AGE") ss >> policy.warn_days;
            else if (key == "PASS_MIN_LEN") ss >> policy.min_length;
        }
    }

    // Check PAM for complexity requirements
    std::ifstream pam("/etc/pam.d/common-password");
    if (pam.is_open()) {
        std::string line;
        while (std::getline(pam, line)) {
            if (line.find("pam_pwquality") != std::string::npos ||
                line.find("pam_cracklib") != std::string::npos) {
                if (line.find("ucredit") != std::string::npos) policy.require_uppercase = true;
                if (line.find("lcredit") != std::string::npos) policy.require_lowercase = true;
                if (line.find("dcredit") != std::string::npos) policy.require_digits = true;
                if (line.find("ocredit") != std::string::npos) policy.require_special = true;

                std::regex minlen_re(R"(minlen=(\d+))");
                std::smatch m;
                if (std::regex_search(line, m, minlen_re)) {
                    policy.min_length = std::stoi(m[1].str());
                }
            }
        }
    }

    return policy;
}

Result<void, std::string> UserManager::set_password_policy(const PasswordPolicy& policy) {
    // Update /etc/login.defs
    std::string login_defs = "/etc/login.defs";
    std::string content;
    {
        std::ifstream f(login_defs);
        if (f.is_open()) {
            content = std::string((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
        }
    }

    auto update_field = [&](const std::string& key, int value) {
        std::regex re("^" + key + "\\s+\\d+", std::regex_constants::multiline);
        std::string replacement = key + "\t" + std::to_string(value);
        if (std::regex_search(content, re)) {
            content = std::regex_replace(content, re, replacement);
        } else {
            content += "\n" + replacement + "\n";
        }
    };

    update_field("PASS_MAX_DAYS", policy.max_days);
    update_field("PASS_MIN_DAYS", policy.min_days);
    update_field("PASS_WARN_AGE", policy.warn_days);
    update_field("PASS_MIN_LEN", policy.min_length);

    std::ofstream out(login_defs);
    if (!out.is_open()) {
        return Result<void, std::string>::error("cannot write to " + login_defs);
    }
    out << content;

    // Update PAM configuration for complexity
    std::string pam_line = "password requisite pam_pwquality.so retry=3 minlen=" +
                           std::to_string(policy.min_length);
    if (policy.require_uppercase) pam_line += " ucredit=-1";
    if (policy.require_lowercase) pam_line += " lcredit=-1";
    if (policy.require_digits) pam_line += " dcredit=-1";
    if (policy.require_special) pam_line += " ocredit=-1";

    // Write PAM config
    std::string pam_path = "/etc/pam.d/common-password";
    if (fs::exists(pam_path)) {
        std::string pam_content;
        {
            std::ifstream f(pam_path);
            pam_content = std::string((std::istreambuf_iterator<char>(f)),
                                       std::istreambuf_iterator<char>());
        }

        std::regex pam_re(R"(^password\s+requisite\s+pam_pwquality.*$)",
                          std::regex_constants::multiline);
        if (std::regex_search(pam_content, pam_re)) {
            pam_content = std::regex_replace(pam_content, pam_re, pam_line);
        }

        std::ofstream pam_out(pam_path);
        if (pam_out.is_open()) pam_out << pam_content;
    }

    return Result<void, std::string>::ok();
}

} // namespace straylight
