// apps/installer/disk_select.cpp
// Block device enumeration via lsblk + ImGui selection UI
#include "disk_select.h"

#include <straylight/log.h>

#include <imgui.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace straylight::installer {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ── Enumeration ─────────────────────────────────────────────────────────────

/// Read the live-boot device path so we can exclude it from the list.
static std::string live_boot_dev() {
    // The kernel records the live-boot block device in /proc/cmdline as
    // "root=..." or via the "live-media-path" parameter.  Simplest heuristic:
    // read /run/live/medium → symlink target → parent block device.
    std::error_code ec;
    auto target = fs::read_symlink("/run/live/medium", ec);
    if (!ec) {
        // target is something like /dev/sdb1 → strip partition suffix
        std::string s = target.string();
        // strip trailing digit(s) for partition number
        while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.back())))
            s.pop_back();
        // nvme devices: nvme0n1p1 → nvme0n1
        if (s.size() > 1 && s.back() == 'p')
            s.pop_back();
        return s;  // e.g. "/dev/sdb"
    }
    return "";
}

/// Run lsblk and return raw JSON output.
static std::string run_lsblk() {
    // -J = JSON, -b = bytes (easier to parse), -o = columns
    // -d = disks only (no partitions in top-level list, but they appear
    //       under "children" when we add -p)
    // We want full tree so we omit -d
    FILE* pipe = popen(
        "lsblk -J -b -o NAME,SIZE,TYPE,MOUNTPOINT,LABEL,FSTYPE,MODEL,"
        "TRAN,RM,HOTPLUG 2>/dev/null",
        "r");
    if (!pipe) return "";

    std::string out;
    std::array<char, 4096> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        out += buf.data();
    pclose(pipe);
    return out;
}

/// Check whether any children (partitions) of a disk have a known OS fstype.
static bool children_have_os(const json& dev) {
    static const char* os_fstypes[] = {
        "ntfs", "vfat", "ext4", "ext3", "btrfs", "xfs", "apfs", nullptr};

    auto it = dev.find("children");
    if (it == dev.end()) return false;
    for (const auto& child : *it) {
        std::string fstype =
            child.value("fstype", std::string{});
        for (int i = 0; os_fstypes[i]; ++i) {
            if (fstype == os_fstypes[i]) return true;
        }
    }
    return false;
}

std::vector<DiskInfo> parse_lsblk_json(const std::string& raw) {
    std::vector<DiskInfo> result;
    if (raw.empty()) return result;

    try {
        auto root = json::parse(raw);
        for (const auto& dev : root.at("blockdevices")) {
            std::string type = dev.value("type", std::string{});
            if (type != "disk") continue;

            std::string name  = dev.value("name",  std::string{});
            std::string tran  = dev.value("tran",  std::string{});
            bool        rm    = dev.value("rm",    false);
            bool        hot   = dev.value("hotplug", false);

            // Skip loop, ROM, and very small devices (< 4 GiB)
            if (name.rfind("loop", 0) == 0) continue;
            if (name.rfind("sr",   0) == 0) continue;

            uint64_t size_bytes = 0;
            try {
                auto sv = dev["size"];
                if (sv.is_number()) size_bytes = sv.get<uint64_t>();
                else if (sv.is_string()) size_bytes = std::stoull(sv.get<std::string>());
            } catch (...) {}
            if (size_bytes < 4ULL * 1024 * 1024 * 1024) continue;

            // Build human-readable size string
            auto human = [](uint64_t b) -> std::string {
                char buf[32];
                if (b >= (1ULL << 40))
                    snprintf(buf, sizeof(buf), "%.1f TiB", b / (double)(1ULL << 40));
                else if (b >= (1ULL << 30))
                    snprintf(buf, sizeof(buf), "%.1f GiB", b / (double)(1ULL << 30));
                else
                    snprintf(buf, sizeof(buf), "%.0f MiB", b / (double)(1ULL << 20));
                return buf;
            };

            DiskInfo di;
            di.name        = name;
            di.path        = "/dev/" + name;
            di.model       = dev.value("model", std::string{"Unknown disk"});
            di.size        = human(size_bytes);
            di.transport   = tran;
            di.removable   = rm || hot;
            di.has_partitions = dev.contains("children") && !dev["children"].empty();
            di.has_known_os   = children_have_os(dev);

            // Trim trailing whitespace from model string
            while (!di.model.empty() &&
                   (di.model.back() == ' ' || di.model.back() == '\t'))
                di.model.pop_back();

            result.push_back(std::move(di));
        }
    } catch (const std::exception& e) {
        SL_ERROR("parse_lsblk_json: {}", e.what());
    }
    return result;
}

std::vector<DiskInfo> enumerate_disks() {
    std::string raw = run_lsblk();
    auto disks = parse_lsblk_json(raw);

    // Remove the live-boot medium so we don't accidentally overwrite it
    std::string live = live_boot_dev();
    if (!live.empty()) {
        disks.erase(
            std::remove_if(disks.begin(), disks.end(),
                [&live](const DiskInfo& d) { return d.path == live; }),
            disks.end());
    }
    return disks;
}

// ── DiskSelectPage ──────────────────────────────────────────────────────────

DiskSelectPage::DiskSelectPage() = default;

void DiskSelectPage::do_scan() {
    disks_       = enumerate_disks();
    scanned_     = true;
    selected_idx_ = -1;
    SL_INFO("DiskSelectPage: found {} candidate disk(s)", disks_.size());
}

const DiskInfo* DiskSelectPage::selected() const {
    if (selected_idx_ < 0 ||
        selected_idx_ >= static_cast<int>(disks_.size()))
        return nullptr;
    return &disks_[static_cast<size_t>(selected_idx_)];
}

bool DiskSelectPage::render() {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("##InstallerDisk", nullptr, flags);

    // ── Header ───────────────────────────────────────────────────────────
    ImGui::SetCursorPos(ImVec2(40.0f, 30.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("STRAYLIGHT INSTALLER");
    ImGui::PopStyleColor();

    ImGui::SetCursorPosX(40.0f);
    ImGui::TextDisabled("Select the drive to install StrayLight OS on.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Scan button / auto-scan ──────────────────────────────────────────
    if (!scanned_) {
        ImGui::SetCursorPosX(40.0f);
        if (ImGui::Button("Scan for drives", ImVec2(160, 36)))
            do_scan();
        ImGui::SameLine();
        ImGui::TextDisabled("(auto-scans on first open)");
        // Auto-scan on first render
        do_scan();
    } else {
        ImGui::SetCursorPosX(40.0f);
        if (ImGui::SmallButton("Re-scan"))
            do_scan();
    }

    ImGui::Spacing();

    if (disks_.empty() && scanned_) {
        ImGui::SetCursorPosX(40.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "No eligible drives found. Check connections and re-scan.");
        ImGui::End();
        return false;
    }

    // ── Drive table ───────────────────────────────────────────────────────
    constexpr float kRowH = 56.0f;
    float avail_h = io.DisplaySize.y - 180.0f;

    ImGui::BeginChild("##disklist",
                      ImVec2(io.DisplaySize.x - 80.0f, avail_h), true);

    if (ImGui::BeginTable("##disks", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_ScrollY)) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed,   32.0f);
        ImGui::TableSetupColumn("Drive",   ImGuiTableColumnFlags_WidthFixed,  140.0f);
        ImGui::TableSetupColumn("Model",   ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Size",    ImGuiTableColumnFlags_WidthFixed,   80.0f);
        ImGui::TableSetupColumn("Notes",   ImGuiTableColumnFlags_WidthFixed,  200.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(disks_.size()); ++i) {
            const DiskInfo& d = disks_[static_cast<size_t>(i)];
            ImGui::TableNextRow(0, kRowH);

            // Radio + select on row click
            ImGui::TableSetColumnIndex(0);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12.0f);
            bool sel = (i == selected_idx_);
            if (ImGui::RadioButton(("##sel" + d.name).c_str(), sel))
                selected_idx_ = i;

            // Whole-row click
            if (ImGui::IsItemClicked() || ImGui::TableGetHoveredColumn() >= 0) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    selected_idx_ = i;
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("/dev/%s", d.name.c_str());
            ImGui::TextDisabled("%s", d.transport.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(d.model.c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(d.size.c_str());

            ImGui::TableSetColumnIndex(4);
            if (d.removable)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f),
                                   "Removable");
            if (d.has_known_os) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "! Existing OS detected");
            } else if (d.has_partitions) {
                ImGui::TextDisabled("Has partitions");
            } else {
                ImGui::TextDisabled("Empty");
            }
        }

        ImGui::EndTable();
    }
    ImGui::EndChild();

    // ── Warning ───────────────────────────────────────────────────────────
    if (selected_idx_ >= 0 &&
        disks_[static_cast<size_t>(selected_idx_)].has_known_os) {
        ImGui::Spacing();
        ImGui::SetCursorPosX(40.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.1f, 1.0f),
            "WARNING: an existing operating system was detected on this "
            "drive.  All data will be erased during installation.");
    }

    // ── Confirm dialog ────────────────────────────────────────────────────
    bool confirmed = false;
    if (show_confirm_) {
        const DiskInfo& d = disks_[static_cast<size_t>(selected_idx_)];
        ImGui::OpenPopup("Confirm Installation Target");
        ImVec2 centre = ImVec2(io.DisplaySize.x * 0.5f,
                               io.DisplaySize.y * 0.5f);
        ImGui::SetNextWindowPos(centre, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Confirm Installation Target",
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Install StrayLight OS to:");
            ImGui::Spacing();
            ImGui::Text("  %s — %s (%s)",
                        d.path.c_str(), d.model.c_str(), d.size.c_str());
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                               "This will ERASE ALL DATA on the chosen drive.");
            ImGui::Spacing();
            if (ImGui::Button("Install now", ImVec2(140, 36))) {
                confirmed = true;
                ImGui::CloseCurrentPopup();
                show_confirm_ = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 36))) {
                show_confirm_ = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // ── Action buttons ────────────────────────────────────────────────────
    ImGui::SetCursorPos(ImVec2(40.0f, io.DisplaySize.y - 70.0f));
    const bool can_install = (selected_idx_ >= 0 && !show_confirm_);

    if (!can_install) ImGui::BeginDisabled();
    if (ImGui::Button("Install StrayLight OS", ImVec2(220, 40)))
        show_confirm_ = true;
    if (!can_install) ImGui::EndDisabled();

    ImGui::End();
    return confirmed;
}

} // namespace straylight::installer
