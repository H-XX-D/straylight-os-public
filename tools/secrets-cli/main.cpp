// tools/secrets-cli/main.cpp
// CLI front-end for straylight-secrets daemon.
#include <straylight/ipc.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static const char* SOCKET_PATH = "/run/straylight/secrets.sock";

static void print_usage() {
    std::cerr
        << "straylight-secrets — system secrets manager CLI\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-secrets set <key> <value>     Store a secret\n"
        << "  straylight-secrets get <key>             Retrieve a secret\n"
        << "  straylight-secrets delete <key>          Delete a secret\n"
        << "  straylight-secrets list                  List accessible secrets\n"
        << "  straylight-secrets rotate <key>          Rotate a secret\n"
        << "  straylight-secrets acl <key> add <uid>   Grant access to a UID\n"
        << "  straylight-secrets acl <key> remove <uid> Revoke access from a UID\n";
}

static nlohmann::json send_command(const std::string& cmd,
                                    const nlohmann::json& params = {}) {
    straylight::IpcClient client;
    auto connect = client.connect(SOCKET_PATH);
    if (!connect.has_value()) {
        std::cerr << "Error: cannot connect to secrets daemon: " << connect.error() << "\n";
        std::exit(1);
    }

    timeval tv{};
    tv.tv_sec = 5;
    setsockopt(client.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    nlohmann::json msg;
    msg["cmd"] = cmd;
    msg["uid"] = static_cast<uint32_t>(getuid());
    if (!params.empty()) {
        msg["params"] = params;
    }

    auto send = client.send(msg.dump());
    if (!send.has_value()) {
        std::cerr << "Error: " << send.error() << "\n";
        std::exit(1);
    }

    auto recv = client.receive();
    if (!recv.has_value()) {
        std::cerr << "Error: " << recv.error() << "\n";
        std::exit(1);
    }

    try {
        return nlohmann::json::parse(recv.value());
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "Error: daemon returned invalid JSON: " << e.what() << "\n";
        std::exit(1);
    }
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

    // -----------------------------------------------------------------------
    // set <key> <value>
    // -----------------------------------------------------------------------
    if (command == "set") {
        if (argc < 4) {
            std::cerr << "Error: 'set' requires a key and value\n";
            return 1;
        }
        nlohmann::json params;
        params["key"] = argv[2];
        params["value"] = argv[3];
        auto resp = send_command("set", params);
        if (resp.value("status", "") == "ok") {
            std::cout << "Secret '" << argv[2] << "' stored.\n";
        } else {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // get <key>
    // -----------------------------------------------------------------------
    if (command == "get") {
        if (argc < 3) {
            std::cerr << "Error: 'get' requires a key\n";
            return 1;
        }
        nlohmann::json params;
        params["key"] = argv[2];
        auto resp = send_command("get", params);
        if (resp.value("status", "") == "ok") {
            std::cout << resp.value("value", "") << "\n";
        } else {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // delete <key>
    // -----------------------------------------------------------------------
    if (command == "delete") {
        if (argc < 3) {
            std::cerr << "Error: 'delete' requires a key\n";
            return 1;
        }
        nlohmann::json params;
        params["key"] = argv[2];
        auto resp = send_command("delete", params);
        if (resp.value("status", "") == "ok") {
            std::cout << "Secret '" << argv[2] << "' deleted.\n";
        } else {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // list
    // -----------------------------------------------------------------------
    if (command == "list") {
        auto resp = send_command("list");
        if (resp.value("status", "") != "ok") {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        auto& keys = resp["keys"];
        if (keys.empty()) {
            std::cout << "No secrets accessible.\n";
        } else {
            std::cout << "Accessible secrets:\n";
            for (const auto& k : keys) {
                std::cout << "  " << k.get<std::string>() << "\n";
            }
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // rotate <key>
    // -----------------------------------------------------------------------
    if (command == "rotate") {
        if (argc < 3) {
            std::cerr << "Error: 'rotate' requires a key\n";
            return 1;
        }
        nlohmann::json params;
        params["key"] = argv[2];
        auto resp = send_command("rotate", params);
        if (resp.value("status", "") == "ok") {
            std::cout << "Secret '" << argv[2] << "' rotated.\n";
        } else {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // acl <key> add/remove <uid>
    // -----------------------------------------------------------------------
    if (command == "acl") {
        if (argc < 5) {
            std::cerr << "Usage: straylight-secrets acl <key> add|remove <uid>\n";
            return 1;
        }
        std::string key = argv[2];
        std::string action = argv[3];
        uint32_t target_uid = static_cast<uint32_t>(std::stoul(argv[4]));

        nlohmann::json params;
        params["key"] = key;
        params["uid"] = target_uid;

        std::string cmd = (action == "add") ? "acl-add" : "acl-remove";
        auto resp = send_command(cmd, params);
        if (resp.value("status", "") == "ok") {
            std::cout << "ACL updated for '" << key << "'.\n";
        } else {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage();
    return 1;
}
