// tools/users/main.cpp
// CLI front-end for straylight-users — user account management.

#include "user_manager.h"

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

static void print_usage() {
    std::cerr
        << "straylight-users — user account management\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-users list [--all]                                  List user accounts\n"
        << "  straylight-users info <username>                               Show user details\n"
        << "  straylight-users add <name> [--shell=X] [--groups=X,Y] [--name='Full Name']\n"
        << "  straylight-users delete <name> [--remove-home]                 Delete user\n"
        << "  straylight-users modify <name> [--shell=X] [--groups=X,Y] [--name='Full Name']\n"
        << "  straylight-users sudo <name> [grant|revoke]                    Manage sudo access\n"
        << "  straylight-users ssh-keys <name> list                          List SSH keys\n"
        << "  straylight-users ssh-keys <name> add <key-or-file>             Add SSH key\n"
        << "  straylight-users ssh-keys <name> remove <index>                Remove SSH key\n"
        << "  straylight-users history <name> [--max=N]                      Login history\n"
        << "  straylight-users lock <name>                                   Lock account\n"
        << "  straylight-users unlock <name>                                 Unlock account\n"
        << "  straylight-users passwd <name>                                 Set password\n";
}

static std::string get_arg(int argc, char* argv[], const std::string& prefix, int start = 2) {
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    return "";
}

static bool has_flag(int argc, char* argv[], const std::string& flag, int start = 2) {
    for (int i = start; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> result;
    size_t pos = 0;
    while (pos < s.size()) {
        auto next = s.find(',', pos);
        result.push_back(s.substr(pos, next == std::string::npos ? next : next - pos));
        pos = (next == std::string::npos) ? s.size() : next + 1;
    }
    return result;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];
    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    straylight::UserManager mgr;

    // -----------------------------------------------------------------------
    // list
    // -----------------------------------------------------------------------
    if (command == "list") {
        bool include_system = has_flag(argc, argv, "--all");
        auto res = mgr.list_users(include_system);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& users = res.value();
        if (users.empty()) {
            std::cout << "No users found.\n";
            return 0;
        }

        std::cout << std::left
                  << std::setw(16) << "USERNAME"
                  << std::setw(6) << "UID"
                  << std::setw(20) << "NAME"
                  << std::setw(16) << "SHELL"
                  << std::setw(6) << "SUDO"
                  << std::setw(8) << "STATUS"
                  << "GROUPS\n";
        std::cout << std::string(84, '-') << "\n";

        for (const auto& u : users) {
            std::string groups_str;
            for (size_t i = 0; i < u.groups.size(); ++i) {
                if (i > 0) groups_str += ",";
                groups_str += u.groups[i];
            }

            std::string status;
            if (u.locked) status = "locked";
            else if (!u.has_password) status = "no-pw";
            else status = "active";

            std::cout << std::left
                      << std::setw(16) << u.username
                      << std::setw(6) << u.uid
                      << std::setw(20) << u.full_name
                      << std::setw(16) << u.shell
                      << std::setw(6) << (u.has_sudo ? "yes" : "no")
                      << std::setw(8) << status
                      << groups_str << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // info <username>
    // -----------------------------------------------------------------------
    if (command == "info") {
        if (argc < 3) {
            std::cerr << "Error: 'info' requires a username\n";
            return 1;
        }
        auto res = mgr.get_user(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& u = res.value();
        std::cout << "User: " << u.username << "\n"
                  << "  UID:        " << u.uid << "\n"
                  << "  GID:        " << u.gid << "\n"
                  << "  Name:       " << u.full_name << "\n"
                  << "  Home:       " << u.home_dir << "\n"
                  << "  Shell:      " << u.shell << "\n"
                  << "  Sudo:       " << (u.has_sudo ? "yes" : "no") << "\n"
                  << "  Locked:     " << (u.locked ? "yes" : "no") << "\n"
                  << "  Password:   " << (u.has_password ? "set" : "not set") << "\n"
                  << "  SSH keys:   " << u.ssh_key_count << "\n";
        if (!u.last_login.empty()) {
            std::cout << "  Last login: " << u.last_login << "\n";
        }
        std::cout << "  Groups:     ";
        for (size_t i = 0; i < u.groups.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << u.groups[i];
        }
        std::cout << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // add <name> [options]
    // -----------------------------------------------------------------------
    if (command == "add") {
        if (argc < 3) {
            std::cerr << "Error: 'add' requires a username\n";
            return 1;
        }
        std::string username = argv[2];
        std::string shell = get_arg(argc, argv, "--shell=", 3);
        std::string groups_str = get_arg(argc, argv, "--groups=", 3);
        std::string full_name = get_arg(argc, argv, "--name=", 3);
        std::string home = get_arg(argc, argv, "--home=", 3);

        if (shell.empty()) shell = "/bin/bash";

        std::vector<std::string> groups;
        if (!groups_str.empty()) groups = split_csv(groups_str);

        auto res = mgr.add_user(username, shell, home, groups, full_name);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "User '" << username << "' created.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // delete <name> [--remove-home]
    // -----------------------------------------------------------------------
    if (command == "delete") {
        if (argc < 3) {
            std::cerr << "Error: 'delete' requires a username\n";
            return 1;
        }
        bool remove_home = has_flag(argc, argv, "--remove-home", 3);
        auto res = mgr.delete_user(argv[2], remove_home);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "User '" << argv[2] << "' deleted.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // modify <name> [options]
    // -----------------------------------------------------------------------
    if (command == "modify") {
        if (argc < 3) {
            std::cerr << "Error: 'modify' requires a username\n";
            return 1;
        }
        std::string username = argv[2];
        std::string shell = get_arg(argc, argv, "--shell=", 3);
        std::string groups_str = get_arg(argc, argv, "--groups=", 3);
        std::string full_name = get_arg(argc, argv, "--name=", 3);
        std::string rm_groups_str = get_arg(argc, argv, "--remove-groups=", 3);

        std::vector<std::string> add_groups;
        if (!groups_str.empty()) add_groups = split_csv(groups_str);

        std::vector<std::string> rm_groups;
        if (!rm_groups_str.empty()) rm_groups = split_csv(rm_groups_str);

        auto res = mgr.modify_user(username, shell, add_groups, rm_groups, full_name);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "User '" << username << "' modified.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // sudo <name> [grant|revoke]
    // -----------------------------------------------------------------------
    if (command == "sudo") {
        if (argc < 3) {
            std::cerr << "Error: 'sudo' requires a username\n";
            return 1;
        }
        std::string username = argv[2];
        std::string action = (argc >= 4) ? argv[3] : "grant";

        if (action == "grant") {
            auto res = mgr.grant_sudo(username);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Sudo access granted to '" << username << "'.\n";
        } else if (action == "revoke") {
            auto res = mgr.revoke_sudo(username);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Sudo access revoked from '" << username << "'.\n";
        } else {
            std::cerr << "Error: unknown sudo action '" << action << "' (use grant/revoke)\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // ssh-keys <name> <list|add|remove>
    // -----------------------------------------------------------------------
    if (command == "ssh-keys") {
        if (argc < 4) {
            std::cerr << "Error: 'ssh-keys' requires username and subcommand\n";
            return 1;
        }
        std::string username = argv[2];
        std::string sub = argv[3];

        if (sub == "list") {
            auto res = mgr.list_ssh_keys(username);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            const auto& keys = res.value();
            if (keys.empty()) {
                std::cout << "No SSH keys for '" << username << "'.\n";
                return 0;
            }
            std::cout << "SSH keys for '" << username << "':\n\n";
            for (size_t i = 0; i < keys.size(); ++i) {
                std::cout << "  [" << i << "] " << keys[i].type;
                if (!keys[i].fingerprint.empty()) {
                    std::cout << " " << keys[i].fingerprint;
                }
                if (!keys[i].comment.empty()) {
                    std::cout << " " << keys[i].comment;
                }
                std::cout << "\n";
            }
            return 0;
        }

        if (sub == "add") {
            if (argc < 5) {
                std::cerr << "Error: 'ssh-keys add' requires a key or key file path\n";
                return 1;
            }
            // Collect key (may span multiple args if not quoted)
            std::string key;
            for (int i = 4; i < argc; ++i) {
                if (i > 4) key += " ";
                key += argv[i];
            }
            auto res = mgr.add_ssh_key(username, key);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "SSH key added for '" << username << "'.\n";
            return 0;
        }

        if (sub == "remove") {
            if (argc < 5) {
                std::cerr << "Error: 'ssh-keys remove' requires a key index\n";
                return 1;
            }
            int index = std::atoi(argv[4]);
            auto res = mgr.remove_ssh_key(username, index);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "SSH key " << index << " removed for '" << username << "'.\n";
            return 0;
        }

        std::cerr << "Error: unknown ssh-keys subcommand '" << sub << "'\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // history <name>
    // -----------------------------------------------------------------------
    if (command == "history") {
        if (argc < 3) {
            std::cerr << "Error: 'history' requires a username\n";
            return 1;
        }
        std::string username = argv[2];
        std::string max_str = get_arg(argc, argv, "--max=", 3);
        int max_entries = max_str.empty() ? 20 : std::atoi(max_str.c_str());

        auto res = mgr.login_history(username, max_entries);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& records = res.value();
        if (records.empty()) {
            std::cout << "No login history for '" << username << "'.\n";
            return 0;
        }

        std::cout << std::left
                  << std::setw(14) << "TERMINAL"
                  << std::setw(18) << "FROM"
                  << std::setw(28) << "LOGIN"
                  << "DURATION\n";
        std::cout << std::string(68, '-') << "\n";

        for (const auto& r : records) {
            std::cout << std::left
                      << std::setw(14) << r.terminal
                      << std::setw(18) << r.from_host
                      << std::setw(28) << r.login_time;
            if (r.still_logged_in) {
                std::cout << "still logged in";
            } else if (!r.duration.empty()) {
                std::cout << r.duration;
            }
            std::cout << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // lock / unlock
    // -----------------------------------------------------------------------
    if (command == "lock") {
        if (argc < 3) {
            std::cerr << "Error: 'lock' requires a username\n";
            return 1;
        }
        auto res = mgr.lock_user(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Account '" << argv[2] << "' locked.\n";
        return 0;
    }

    if (command == "unlock") {
        if (argc < 3) {
            std::cerr << "Error: 'unlock' requires a username\n";
            return 1;
        }
        auto res = mgr.unlock_user(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Account '" << argv[2] << "' unlocked.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // passwd <name>
    // -----------------------------------------------------------------------
    if (command == "passwd") {
        if (argc < 3) {
            std::cerr << "Error: 'passwd' requires a username\n";
            return 1;
        }
        // For non-interactive use, read from --password= flag
        std::string password = get_arg(argc, argv, "--password=", 3);
        if (password.empty()) {
            // Interactive: just call passwd directly
            std::string cmd = "passwd " + std::string(argv[2]);
            return std::system(cmd.c_str());
        }

        auto res = mgr.set_password(argv[2], password);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Password set for '" << argv[2] << "'.\n";
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
