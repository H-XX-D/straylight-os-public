// tools/nerve-cli/main.cpp
// CLI for straylight-nerve — hardware interrupt affinity management.

#include <straylight/ipc_client.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static const char* NERVE_SOCKET = "/run/straylight/nerve.sock";

static void print_usage() {
    std::cerr
        << "straylight-nerve-cli — hardware interrupt affinity management\n"
        << "\n"
        << "Usage:\n"
        << "  nerve status                   Show IRQ map with affinities\n"
        << "  nerve optimize                 Run IRQ affinity optimization\n"
        << "  nerve set <irq> <cpus>         Set affinity for IRQ (e.g., 'nerve set 42 0,2,4')\n"
        << "  nerve monitor                  Show interrupt rates\n"
        << "  nerve balance-report           Show interrupt distribution analysis\n"
        << "  nerve alerts                   Show recent alerts\n";
}

static straylight::Result<nlohmann::json, std::string> rpc_call(
    const std::string& method,
    const nlohmann::json& params = nlohmann::json::object()) {
    straylight::IpcJsonClient client;
    auto conn = client.connect(NERVE_SOCKET);
    if (!conn.has_value()) {
        return straylight::Result<nlohmann::json, std::string>::error(
            "Cannot connect to nerve daemon: " + conn.error());
    }

    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["method"] = method;
    req["params"] = params;
    req["id"] = 1;

    auto resp = client.request(req);
    if (!resp.has_value()) {
        return straylight::Result<nlohmann::json, std::string>::error(resp.error());
    }

    auto& j = resp.value();
    if (j.contains("error")) {
        return straylight::Result<nlohmann::json, std::string>::error(
            j["error"].value("message", "Unknown error"));
    }

    return straylight::Result<nlohmann::json, std::string>::ok(j["result"]);
}

static int cmd_status() {
    auto result = rpc_call("status");
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    const auto& irqs = result.value();
    if (!irqs.is_array() || irqs.empty()) {
        std::cout << "No IRQs found.\n";
        return 0;
    }

    std::cout << std::left
              << std::setw(8)  << "IRQ"
              << std::setw(8)  << "TYPE"
              << std::setw(20) << "CPU AFFINITY"
              << std::setw(14) << "TOTAL"
              << "DEVICE"
              << "\n";
    std::cout << std::string(70, '-') << "\n";

    for (const auto& irq : irqs) {
        std::string cpus_str;
        if (irq.contains("cpus") && irq["cpus"].is_array()) {
            for (size_t i = 0; i < irq["cpus"].size(); ++i) {
                if (i > 0) cpus_str += ",";
                cpus_str += std::to_string(irq["cpus"][i].get<uint32_t>());
            }
        }

        std::cout << std::left
                  << std::setw(8)  << irq.value("irq", 0u)
                  << std::setw(8)  << irq.value("type", "?")
                  << std::setw(20) << cpus_str
                  << std::setw(14) << irq.value("total_count", static_cast<uint64_t>(0))
                  << irq.value("device", "unknown")
                  << "\n";
    }

    return 0;
}

static int cmd_optimize() {
    std::cout << "Running IRQ affinity optimization...\n";

    auto result = rpc_call("optimize");
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    const auto& res = result.value();
    auto count = res.value("count", static_cast<size_t>(0));

    if (count == 0) {
        std::cout << "All IRQs already optimally placed.\n";
        return 0;
    }

    std::cout << count << " IRQ affinity changes applied:\n\n";

    if (res.contains("changes") && res["changes"].is_array()) {
        for (const auto& change : res["changes"]) {
            std::cout << "  IRQ " << change.value("irq", 0u)
                      << " (" << change.value("device", "?") << "): ";

            // Previous CPUs
            if (change.contains("previous_cpus") && change["previous_cpus"].is_array()) {
                std::cout << "[";
                for (size_t i = 0; i < change["previous_cpus"].size(); ++i) {
                    if (i > 0) std::cout << ",";
                    std::cout << change["previous_cpus"][i].get<uint32_t>();
                }
                std::cout << "]";
            }

            std::cout << " -> ";

            // New CPUs
            if (change.contains("new_cpus") && change["new_cpus"].is_array()) {
                std::cout << "[";
                for (size_t i = 0; i < change["new_cpus"].size(); ++i) {
                    if (i > 0) std::cout << ",";
                    std::cout << change["new_cpus"][i].get<uint32_t>();
                }
                std::cout << "]";
            }

            std::cout << "\n";
            std::cout << "    " << change.value("reason", "") << "\n";
        }
    }

    return 0;
}

static int cmd_set(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "Usage: nerve set <irq> <cpu_mask>\n"
                  << "  Example: nerve set 42 0,2,4    (set IRQ 42 to CPUs 0,2,4)\n"
                  << "  Example: nerve set 42 ff       (hex mask for CPUs 0-7)\n";
        return 1;
    }

    uint32_t irq = static_cast<uint32_t>(std::stoul(args[0]));
    std::string mask = args[1];

    // If input contains commas, it's a CPU list — convert to hex mask
    if (mask.find(',') != std::string::npos) {
        std::vector<uint32_t> cpus;
        std::istringstream iss(mask);
        std::string tok;
        while (std::getline(iss, tok, ',')) {
            cpus.push_back(static_cast<uint32_t>(std::stoul(tok)));
        }

        // Convert to hex mask
        uint64_t bitmask = 0;
        for (uint32_t cpu : cpus) {
            if (cpu < 64) bitmask |= (1ULL << cpu);
        }
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%lx", static_cast<unsigned long>(bitmask));
        mask = hex;
    }

    nlohmann::json params;
    params["irq"] = irq;
    params["mask"] = mask;

    auto result = rpc_call("set_affinity", params);
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    std::cout << "IRQ " << irq << " affinity set to " << mask << "\n";
    return 0;
}

static int cmd_monitor() {
    auto result = rpc_call("monitor");
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    const auto& rates = result.value();
    if (!rates.is_array() || rates.empty()) {
        std::cout << "No interrupt rate data available (need at least 2 samples).\n";
        return 0;
    }

    std::cout << std::left
              << std::setw(8)  << "IRQ"
              << std::setw(16) << "RATE (ints/s)"
              << "\n";
    std::cout << std::string(24, '-') << "\n";

    for (const auto& entry : rates) {
        double rate = entry.value("rate", 0.0);
        if (rate < 0.1) continue; // Skip idle IRQs

        std::cout << std::left
                  << std::setw(8) << entry.value("irq", 0u)
                  << std::setw(16) << static_cast<uint64_t>(rate)
                  << (rate > 100000 ? " [STORM!]" : "")
                  << "\n";
    }

    return 0;
}

static int cmd_balance_report() {
    auto result = rpc_call("balance_report");
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    std::cout << result.value().value("report", "No report available.") << "\n";
    return 0;
}

static int cmd_alerts() {
    auto result = rpc_call("alerts");
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    const auto& alerts = result.value();
    if (!alerts.is_array() || alerts.empty()) {
        std::cout << "No alerts.\n";
        return 0;
    }

    for (const auto& alert : alerts) {
        std::string severity = alert.value("severity", "info");
        std::string prefix = severity == "critical" ? "[CRIT]" :
                            severity == "warning" ? "[WARN]" : "[INFO]";

        std::cout << prefix << " " << alert.value("message", "") << "\n";
    }

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    if (command == "status")         return cmd_status();
    if (command == "optimize")       return cmd_optimize();
    if (command == "set")            return cmd_set(args);
    if (command == "monitor")        return cmd_monitor();
    if (command == "balance-report") return cmd_balance_report();
    if (command == "alerts")         return cmd_alerts();

    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage();
    return 1;
}
