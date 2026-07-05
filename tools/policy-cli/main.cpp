// tools/policy-cli/main.cpp
// CLI for the straylight-policy declarative system roles daemon.

#include <straylight/ipc_client.h>
#include <straylight/log.h>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-policy -- Declarative system roles CLI\n\n"
        << "Usage:\n"
        << "  straylight-policy apply <role>                      Apply a system role\n"
        << "  straylight-policy status                            Show current role status\n"
        << "  straylight-policy list                              List available roles\n"
        << "  straylight-policy check                             Run compliance check\n"
        << "  straylight-policy create <name> --base=<role>       Create custom role\n"
        << "  straylight-policy diff <role1> <role2>              Compare two roles\n";
}

static std::string default_socket() {
    const char* env = std::getenv("POLICY_SOCKET");
    return env ? env : "/run/straylight/policy.sock";
}

static const char* BOLD = "\033[1m";
static const char* RESET = "\033[0m";
static const char* GREEN = "\033[32m";
static const char* YELLOW = "\033[33m";
static const char* RED = "\033[31m";
static const char* CYAN = "\033[36m";
static const char* DIM = "\033[2m";

static const char* severity_color(const std::string& sev) {
    if (sev == "critical") return RED;
    if (sev == "error") return RED;
    if (sev == "warning") return YELLOW;
    return DIM;
}

static int cmd_apply(straylight::IpcJsonClient& client, int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: apply requires <role>\n";
        return 1;
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "apply";
    request["params"]["role"] = argv[2];

    auto response = client.request(request);
    if (!response.has_value()) {
        std::cerr << "Error: " << response.error() << "\n";
        return 1;
    }

    const auto& resp = response.value();
    if (resp.contains("error")) {
        std::cerr << "Error: " << resp["error"].dump() << "\n";
        return 1;
    }

    if (resp.contains("result")) {
        const auto& r = resp["result"];
        bool success = r.value("success", false);

        std::cout << (success ? GREEN : YELLOW) << BOLD
                  << "Role '" << r.value("role", "") << "' applied"
                  << (success ? " successfully" : " with errors") << RESET << "\n\n";

        std::cout << "  Changes made:   " << r.value("changes_made", 0) << "\n"
                  << "  Changes failed: " << r.value("changes_failed", 0) << "\n";

        if (r.contains("actions") && r["actions"].is_array()) {
            std::cout << "\n  " << BOLD << "Actions:" << RESET << "\n";
            for (const auto& action : r["actions"]) {
                std::cout << "    " << GREEN << "+" << RESET << " "
                          << action.get<std::string>() << "\n";
            }
        }

        if (r.contains("errors") && r["errors"].is_array() && !r["errors"].empty()) {
            std::cout << "\n  " << BOLD << RED << "Errors:" << RESET << "\n";
            for (const auto& err : r["errors"]) {
                std::cout << "    " << RED << "x" << RESET << " "
                          << err.get<std::string>() << "\n";
            }
        }
    }

    return 0;
}

static int cmd_status(straylight::IpcJsonClient& client) {
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "status";

    auto response = client.request(request);
    if (!response.has_value()) {
        std::cerr << "Error: " << response.error() << "\n";
        return 1;
    }

    const auto& resp = response.value();
    if (resp.contains("error")) {
        std::cerr << "Error: " << resp["error"].dump() << "\n";
        return 1;
    }

    if (resp.contains("result")) {
        const auto& r = resp["result"];
        std::string current = r.value("current_role", "none");

        std::cout << BOLD << "System Policy Status" << RESET << "\n\n";
        std::cout << "  Current Role:     " << CYAN << BOLD << current << RESET << "\n";

        if (r.contains("description")) {
            std::cout << "  Description:      " << r["description"].get<std::string>() << "\n";
        }
        if (r.contains("autotune_profile")) {
            std::cout << "  Autotune Profile: " << r["autotune_profile"].get<std::string>() << "\n";
        }
        if (r.contains("compositor_mode")) {
            std::cout << "  Compositor Mode:  " << r["compositor_mode"].get<std::string>() << "\n";
        }

        if (r.contains("compliant")) {
            bool compliant = r["compliant"].get<bool>();
            double ratio = r.value("compliance_ratio", 1.0) * 100.0;
            std::cout << "  Compliance:       "
                      << (compliant ? GREEN : RED) << BOLD
                      << (compliant ? "COMPLIANT" : "NON-COMPLIANT")
                      << RESET << " (" << std::fixed << std::setprecision(0) << ratio << "%)"
                      << " [" << r.value("checks_passed", 0) << "/"
                      << (r.value("checks_passed", 0) + r.value("checks_failed", 0)) << " checks]\n";
        }

        if (r.contains("active_services") && r["active_services"].is_array()) {
            std::cout << "\n  " << BOLD << "Active Services:" << RESET << "\n";
            for (const auto& svc : r["active_services"]) {
                std::cout << "    " << GREEN << "+" << RESET << " " << svc.get<std::string>() << "\n";
            }
        }

        if (r.contains("disabled_services") && r["disabled_services"].is_array() &&
            !r["disabled_services"].empty()) {
            std::cout << "\n  " << BOLD << "Disabled Services:" << RESET << "\n";
            for (const auto& svc : r["disabled_services"]) {
                std::cout << "    " << DIM << "-" << RESET << " " << svc.get<std::string>() << "\n";
            }
        }

        if (r.contains("available_roles") && r["available_roles"].is_array()) {
            std::cout << "\n  " << BOLD << "Available Roles:" << RESET << " ";
            bool first = true;
            for (const auto& role : r["available_roles"]) {
                if (!first) std::cout << ", ";
                std::string name = role.get<std::string>();
                if (name == current) {
                    std::cout << CYAN << BOLD << name << RESET;
                } else {
                    std::cout << name;
                }
                first = false;
            }
            std::cout << "\n";
        }
    }

    return 0;
}

static int cmd_list(straylight::IpcJsonClient& client) {
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "list";

    auto response = client.request(request);
    if (!response.has_value()) {
        std::cerr << "Error: " << response.error() << "\n";
        return 1;
    }

    const auto& resp = response.value();
    if (resp.contains("error")) {
        std::cerr << "Error: " << resp["error"].dump() << "\n";
        return 1;
    }

    if (!resp.contains("result") || !resp["result"].is_array()) {
        std::cout << "No roles available.\n";
        return 0;
    }

    std::cout << std::left
              << std::setw(18) << "ROLE"
              << std::setw(16) << "PROFILE"
              << std::setw(12) << "COMPOSITOR"
              << std::setw(8)  << "ACTIVE"
              << "DESCRIPTION"
              << "\n"
              << std::string(90, '-') << "\n";

    for (const auto& r : resp["result"]) {
        bool active = r.value("active", false);
        std::string name = r.value("name", "");

        if (active) std::cout << CYAN << BOLD;
        std::cout << std::left
                  << std::setw(18) << name
                  << std::setw(16) << r.value("autotune_profile", "")
                  << std::setw(12) << r.value("compositor_mode", "")
                  << std::setw(8)  << (active ? "*" : "")
                  << r.value("description", "");
        if (active) std::cout << RESET;
        std::cout << "\n";
    }

    std::cout << "\n" << resp["result"].size() << " role(s) available. "
              << "Use 'straylight-policy apply <role>' to switch.\n";
    return 0;
}

static int cmd_check(straylight::IpcJsonClient& client) {
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "check";

    auto response = client.request(request);
    if (!response.has_value()) {
        std::cerr << "Error: " << response.error() << "\n";
        return 1;
    }

    const auto& resp = response.value();
    if (resp.contains("error")) {
        std::cerr << "Error: " << resp["error"].dump() << "\n";
        return 1;
    }

    if (resp.contains("result")) {
        const auto& r = resp["result"];
        bool compliant = r.value("compliant", false);
        double ratio = r.value("compliance_ratio", 0.0) * 100.0;

        std::cout << BOLD << "Compliance Report for role '" << r.value("role", "")
                  << "'" << RESET << "\n\n"
                  << "  Status:     " << (compliant ? GREEN : RED) << BOLD
                  << (compliant ? "COMPLIANT" : "NON-COMPLIANT") << RESET << "\n"
                  << "  Score:      " << std::fixed << std::setprecision(0) << ratio << "%\n"
                  << "  Total:      " << r.value("total_checks", 0) << " checks\n"
                  << "  Passed:     " << GREEN << r.value("passed", 0) << RESET << "\n"
                  << "  Failed:     " << RED << r.value("failed", 0) << RESET << "\n";

        if (r.contains("deviations") && r["deviations"].is_array() &&
            !r["deviations"].empty()) {
            std::cout << "\n  " << BOLD << "Deviations:" << RESET << "\n";

            for (const auto& d : r["deviations"]) {
                std::string sev = d.value("severity", "warning");
                std::cout << "    " << severity_color(sev)
                          << "[" << sev << "]" << RESET << " "
                          << d.value("component", "?") << "\n"
                          << "      Expected: " << d.value("expected", "?") << "\n"
                          << "      Actual:   " << d.value("actual", "?") << "\n"
                          << "      " << DIM << d.value("description", "") << RESET << "\n\n";
            }
        } else if (compliant) {
            std::cout << "\n  All checks passed.\n";
        }
    }

    return 0;
}

static int cmd_create(straylight::IpcJsonClient& client, int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: create requires <name>\n";
        return 1;
    }

    std::string name = argv[2];
    std::string base;
    nlohmann::json overrides;

    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--base=", 0) == 0) {
            base = arg.substr(7);
        } else if (arg.rfind("--autotune=", 0) == 0) {
            overrides["autotune_profile"] = arg.substr(11);
        } else if (arg.rfind("--compositor=", 0) == 0) {
            overrides["compositor"]["mode"] = arg.substr(13);
        } else if (arg.rfind("--description=", 0) == 0) {
            overrides["description"] = arg.substr(14);
        }
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "create";
    request["params"]["name"] = name;
    request["params"]["base"] = base;
    request["params"]["overrides"] = overrides;

    auto response = client.request(request);
    if (!response.has_value()) {
        std::cerr << "Error: " << response.error() << "\n";
        return 1;
    }

    const auto& resp = response.value();
    if (resp.contains("error")) {
        std::cerr << "Error: " << resp["error"].dump() << "\n";
        return 1;
    }

    std::cout << GREEN << "Custom role '" << name << "' created"
              << (base.empty() ? "" : " (based on '" + base + "')")
              << RESET << "\n"
              << "Use 'straylight-policy apply " << name << "' to activate.\n";
    return 0;
}

static int cmd_diff(straylight::IpcJsonClient& client, int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Error: diff requires <role1> <role2>\n";
        return 1;
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "diff";
    request["params"]["role_a"] = argv[2];
    request["params"]["role_b"] = argv[3];

    auto response = client.request(request);
    if (!response.has_value()) {
        std::cerr << "Error: " << response.error() << "\n";
        return 1;
    }

    const auto& resp = response.value();
    if (resp.contains("error")) {
        std::cerr << "Error: " << resp["error"].dump() << "\n";
        return 1;
    }

    if (resp.contains("result")) {
        const auto& r = resp["result"];
        std::string role_a = r.value("role_a", argv[2]);
        std::string role_b = r.value("role_b", argv[3]);
        int change_count = r.value("change_count", 0);

        std::cout << BOLD << "Diff: " << CYAN << role_a << RESET << BOLD
                  << " vs " << CYAN << role_b << RESET << "\n"
                  << "  " << change_count << " difference(s)\n\n";

        if (r.contains("changes") && r["changes"].is_array()) {
            for (const auto& c : r["changes"]) {
                std::string field = c.value("field", "?");
                std::cout << "  " << BOLD << field << RESET << ":\n";

                if (c.contains("role_a") && !c["role_a"].is_null()) {
                    std::cout << "    " << RED << "- " << role_a << ": "
                              << c["role_a"].dump() << RESET << "\n";
                }
                if (c.contains("role_b") && !c["role_b"].is_null()) {
                    std::cout << "    " << GREEN << "+ " << role_b << ": "
                              << c["role_b"].dump() << RESET << "\n";
                }
                std::cout << "\n";
            }
        }

        if (change_count == 0) {
            std::cout << "  Roles are identical.\n";
        }
    }

    return 0;
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

    straylight::IpcJsonClient client;
    auto conn = client.connect(default_socket());
    if (!conn.has_value()) {
        std::cerr << "Error: could not connect to policy daemon: " << conn.error() << "\n";
        std::cerr << "Is straylight-policy running?\n";
        return 1;
    }

    if (command == "apply")        return cmd_apply(client, argc, argv);
    else if (command == "status")  return cmd_status(client);
    else if (command == "list")    return cmd_list(client);
    else if (command == "check")   return cmd_check(client);
    else if (command == "create")  return cmd_create(client, argc, argv);
    else if (command == "diff")    return cmd_diff(client, argc, argv);
    else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }
}
