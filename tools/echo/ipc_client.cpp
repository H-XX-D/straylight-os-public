// tools/echo/ipc_client.cpp
#include "ipc_client.h"

namespace straylight {

// ─── Snapshot Client ────────────────────────────────────────────────

Result<nlohmann::json, std::string> SnapshotClient::call(
    const std::string& method, const nlohmann::json& params) {
    IpcJsonClient client;
    auto conn = client.connect(SOCKET);
    if (!conn.has_value()) {
        return Result<nlohmann::json, std::string>::error(
            "Cannot connect to snapshot service: " + conn.error());
    }

    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["method"] = method;
    req["params"] = params;
    req["id"] = 1;

    auto resp = client.request(req);
    if (!resp.has_value()) {
        return Result<nlohmann::json, std::string>::error(resp.error());
    }

    auto& j = resp.value();
    if (j.contains("error")) {
        return Result<nlohmann::json, std::string>::error(
            j["error"].value("message", "Unknown error"));
    }

    return Result<nlohmann::json, std::string>::ok(j.value("result", nlohmann::json()));
}

Result<std::string, std::string> SnapshotClient::snapshot_create(const std::string& name) {
    nlohmann::json params;
    if (!name.empty()) params["name"] = name;

    auto result = call("save", params);
    if (!result.has_value()) {
        return Result<std::string, std::string>::error(result.error());
    }

    std::string id = result.value().value("id", "");
    if (id.empty()) {
        return Result<std::string, std::string>::error("Snapshot service returned no ID");
    }

    return Result<std::string, std::string>::ok(std::move(id));
}

Result<void, std::string> SnapshotClient::snapshot_restore(const std::string& snapshot_id) {
    nlohmann::json params;
    params["id"] = snapshot_id;

    auto result = call("restore", params);
    if (!result.has_value()) {
        return Result<void, std::string>::error(result.error());
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> SnapshotClient::snapshot_delete(const std::string& snapshot_id) {
    nlohmann::json params;
    params["id"] = snapshot_id;

    auto result = call("delete", params);
    if (!result.has_value()) {
        return Result<void, std::string>::error(result.error());
    }

    return Result<void, std::string>::ok();
}

Result<nlohmann::json, std::string> SnapshotClient::snapshot_list() {
    return call("list");
}

Result<nlohmann::json, std::string> SnapshotClient::snapshot_info(const std::string& snapshot_id) {
    nlohmann::json params;
    params["id"] = snapshot_id;
    return call("info", params);
}

// ─── Rewind Client ──────────────────────────────────────────────────

Result<nlohmann::json, std::string> RewindClient::call(
    const std::string& method, const nlohmann::json& params) {
    IpcJsonClient client;
    auto conn = client.connect(SOCKET);
    if (!conn.has_value()) {
        return Result<nlohmann::json, std::string>::error(
            "Cannot connect to rewind service: " + conn.error());
    }

    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["method"] = method;
    req["params"] = params;
    req["id"] = 1;

    auto resp = client.request(req);
    if (!resp.has_value()) {
        return Result<nlohmann::json, std::string>::error(resp.error());
    }

    auto& j = resp.value();
    if (j.contains("error")) {
        return Result<nlohmann::json, std::string>::error(
            j["error"].value("message", "Unknown error"));
    }

    return Result<nlohmann::json, std::string>::ok(j.value("result", nlohmann::json()));
}

Result<std::string, std::string> RewindClient::rewind_checkpoint(pid_t pid) {
    nlohmann::json params;
    params["pid"] = pid;

    auto result = call("checkpoint", params);
    if (!result.has_value()) {
        return Result<std::string, std::string>::error(result.error());
    }

    std::string id = result.value().value("checkpoint_id", "");
    if (id.empty()) {
        return Result<std::string, std::string>::error("Rewind service returned no checkpoint ID");
    }

    return Result<std::string, std::string>::ok(std::move(id));
}

Result<void, std::string> RewindClient::rewind_restore(
    pid_t pid, const std::string& checkpoint_id) {
    nlohmann::json params;
    params["pid"] = pid;
    params["checkpoint_id"] = checkpoint_id;

    auto result = call("restore", params);
    if (!result.has_value()) {
        return Result<void, std::string>::error(result.error());
    }

    return Result<void, std::string>::ok();
}

Result<nlohmann::json, std::string> RewindClient::rewind_list(pid_t pid) {
    nlohmann::json params;
    params["pid"] = pid;
    return call("list", params);
}

Result<void, std::string> RewindClient::rewind_delete(const std::string& checkpoint_id) {
    nlohmann::json params;
    params["checkpoint_id"] = checkpoint_id;

    auto result = call("delete", params);
    if (!result.has_value()) {
        return Result<void, std::string>::error(result.error());
    }

    return Result<void, std::string>::ok();
}

// ─── Replay Client ──────────────────────────────────────────────────

Result<nlohmann::json, std::string> ReplayClient::call(
    const std::string& method, const nlohmann::json& params) {
    IpcJsonClient client;
    auto conn = client.connect(SOCKET);
    if (!conn.has_value()) {
        return Result<nlohmann::json, std::string>::error(
            "Cannot connect to replay service: " + conn.error());
    }

    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["method"] = method;
    req["params"] = params;
    req["id"] = 1;

    auto resp = client.request(req);
    if (!resp.has_value()) {
        return Result<nlohmann::json, std::string>::error(resp.error());
    }

    auto& j = resp.value();
    if (j.contains("error")) {
        return Result<nlohmann::json, std::string>::error(
            j["error"].value("message", "Unknown error"));
    }

    return Result<nlohmann::json, std::string>::ok(j.value("result", nlohmann::json()));
}

Result<uint64_t, std::string> ReplayClient::replay_get_position() {
    auto result = call("get_position");
    if (!result.has_value()) {
        return Result<uint64_t, std::string>::error(result.error());
    }

    uint64_t position = result.value().value("position", static_cast<uint64_t>(0));
    return Result<uint64_t, std::string>::ok(position);
}

Result<void, std::string> ReplayClient::replay_seek(uint64_t position) {
    nlohmann::json params;
    params["position"] = position;

    auto result = call("seek", params);
    if (!result.has_value()) {
        return Result<void, std::string>::error(result.error());
    }

    return Result<void, std::string>::ok();
}

Result<uint64_t, std::string> ReplayClient::replay_get_size() {
    auto result = call("get_size");
    if (!result.has_value()) {
        return Result<uint64_t, std::string>::error(result.error());
    }

    uint64_t size = result.value().value("size", static_cast<uint64_t>(0));
    return Result<uint64_t, std::string>::ok(size);
}

} // namespace straylight
