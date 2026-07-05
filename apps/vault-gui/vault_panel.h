// apps/vault-gui/vault_panel.h
// StrayLight Vault GUI — Secret Manager panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <ctime>

namespace straylight::vault {

struct SecretEntry {
    std::string path;
    std::string value;
    std::string created;
    std::string modified;
    bool        is_folder = false;
};

struct VaultState {
    bool locked = true;
    char master_password[256] = {};
    char search_filter[256] = {};
    char new_path[256] = {};
    char new_value[1024] = {};
    char edit_value[1024] = {};
    char import_path[512] = {};
    char export_path[512] = {};

    int  selected_index = -1;
    bool show_add_dialog = false;
    bool show_edit_dialog = false;
    bool show_delete_confirm = false;
    bool show_import_dialog = false;
    bool show_export_dialog = false;
    bool show_unlock_dialog = true;

    float clipboard_timer = 0.0f;
    bool  clipboard_active = false;
    std::string clipboard_path;

    std::vector<SecretEntry> secrets;
    std::vector<SecretEntry> filtered_secrets;

    void init() {
        secrets.push_back({"system/db/password", "<example-db-password>", "2026-01-15 10:30", "2026-03-10 14:22", false});
        secrets.push_back({"system/api/token", "<example-api-token>", "2026-02-01 09:00", "2026-03-12 11:45", false});
        secrets.push_back({"system/ssh/private_key", "<example-private-key>", "2026-01-20 16:00", "2026-01-20 16:00", false});
        secrets.push_back({"user/email/smtp_pass", "<example-smtp-password>", "2026-02-14 08:15", "2026-03-01 09:30", false});
        secrets.push_back({"user/vpn/certificate", "<example-certificate>", "2026-03-01 12:00", "2026-03-01 12:00", false});
        secrets.push_back({"services/redis/auth", "<example-redis-auth>", "2026-02-20 14:00", "2026-02-20 14:00", false});
        secrets.push_back({"services/mqtt/password", "<example-mqtt-password>", "2026-03-05 10:00", "2026-03-05 10:00", false});
        secrets.push_back({"deploy/registry/token", "<example-registry-token>", "2026-03-10 08:00", "2026-03-10 08:00", false});
        apply_filter();
    }

    void apply_filter() {
        filtered_secrets.clear();
        std::string f(search_filter);
        for (auto& s : secrets) {
            if (f.empty() || s.path.find(f) != std::string::npos) {
                filtered_secrets.push_back(s);
            }
        }
    }

    std::string now_str() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
        return buf;
    }
};

inline void render_vault_panel(VaultState& st) {
    // Unlock dialog
    if (st.locked) {
        ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f - 200,
                                        ImGui::GetIO().DisplaySize.y * 0.5f - 100), ImGuiCond_Always);
        if (ImGui::Begin("Unlock Vault", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "STRAYLIGHT VAULT");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Enter master password to unlock:");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-1);
            bool enter_pressed = ImGui::InputText("##master_pw", st.master_password, sizeof(st.master_password),
                                                   ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Spacing();
            if (ImGui::Button("Unlock", ImVec2(-1, 36)) || enter_pressed) {
                if (strlen(st.master_password) > 0) {
                    st.locked = false;
                    st.init();
                }
            }
        }
        ImGui::End();
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("STRAYLIGHT VAULT");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth() - 120);
    if (ImGui::SmallButton("Lock Vault")) {
        st.locked = true;
        st.secrets.clear();
        st.filtered_secrets.clear();
        memset(st.master_password, 0, sizeof(st.master_password));
    }
    ImGui::Separator();
    ImGui::Spacing();

    // Toolbar
    ImGui::SetNextItemWidth(300);
    if (ImGui::InputTextWithHint("##search", "Search secrets...", st.search_filter, sizeof(st.search_filter))) {
        st.apply_filter();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Secret")) {
        st.show_add_dialog = true;
        memset(st.new_path, 0, sizeof(st.new_path));
        memset(st.new_value, 0, sizeof(st.new_value));
    }
    ImGui::SameLine();
    if (ImGui::Button("Import")) st.show_import_dialog = true;
    ImGui::SameLine();
    if (ImGui::Button("Export")) st.show_export_dialog = true;

    // Clipboard timer
    if (st.clipboard_active) {
        st.clipboard_timer -= ImGui::GetIO().DeltaTime;
        if (st.clipboard_timer <= 0.0f) {
            st.clipboard_active = false;
            st.clipboard_path.clear();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f),
                           "Clipboard: %s (%.0fs)", st.clipboard_path.c_str(), st.clipboard_timer);
    }

    ImGui::Spacing();

    // Secret tree / list
    float list_width = ImGui::GetContentRegionAvail().x * 0.45f;
    if (ImGui::BeginChild("##secret_list", ImVec2(list_width, -1), true)) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Secrets (%zu)", st.filtered_secrets.size());
        ImGui::Separator();

        // Group by prefix
        std::string current_prefix;
        bool tree_open = false;
        for (int i = 0; i < (int)st.filtered_secrets.size(); ++i) {
            auto& s = st.filtered_secrets[i];
            // Extract first path component
            size_t slash = s.path.find('/');
            std::string prefix = (slash != std::string::npos) ? s.path.substr(0, slash) : "";
            std::string leaf = (slash != std::string::npos) ? s.path.substr(slash + 1) : s.path;

            if (prefix != current_prefix) {
                if (tree_open) ImGui::TreePop();
                current_prefix = prefix;
                if (!prefix.empty()) {
                    tree_open = ImGui::TreeNodeEx(prefix.c_str(),
                                                   ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
                } else {
                    tree_open = true;
                }
            }

            if (tree_open || prefix.empty()) {
                bool selected = (st.selected_index == i);
                if (ImGui::Selectable(leaf.c_str(), selected, ImGuiSelectableFlags_None, ImVec2(0, 24))) {
                    st.selected_index = i;
                }
            }
        }
        if (tree_open && !current_prefix.empty()) ImGui::TreePop();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Detail panel
    if (ImGui::BeginChild("##secret_detail", ImVec2(0, -1), true)) {
        if (st.selected_index >= 0 && st.selected_index < (int)st.filtered_secrets.size()) {
            auto& s = st.filtered_secrets[st.selected_index];
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "%s", s.path.c_str());
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Created:  %s", s.created.c_str());
            ImGui::Text("Modified: %s", s.modified.c_str());
            ImGui::Spacing();

            ImGui::Text("Value:");
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.05f, 0.05f, 0.08f, 1.0f));
            char display_val[1024];
            snprintf(display_val, sizeof(display_val), "%s", s.value.c_str());
            ImGui::InputTextMultiline("##val_display", display_val, sizeof(display_val),
                                       ImVec2(-1, 120), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();

            ImGui::Spacing();
            if (ImGui::Button("Copy to Clipboard", ImVec2(180, 30))) {
                ImGui::SetClipboardText(s.value.c_str());
                st.clipboard_active = true;
                st.clipboard_timer = 30.0f;
                st.clipboard_path = s.path;
            }
            ImGui::SameLine();
            if (ImGui::Button("Edit", ImVec2(80, 30))) {
                st.show_edit_dialog = true;
                snprintf(st.edit_value, sizeof(st.edit_value), "%s", s.value.c_str());
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button("Delete", ImVec2(80, 30))) {
                st.show_delete_confirm = true;
            }
            ImGui::PopStyleColor(2);
        } else {
            ImGui::TextDisabled("Select a secret from the list");
        }
    }
    ImGui::EndChild();

    // Add Secret Dialog
    if (st.show_add_dialog) {
        ImGui::OpenPopup("Add Secret");
        st.show_add_dialog = false;
    }
    if (ImGui::BeginPopupModal("Add Secret", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Path:");
        ImGui::SetNextItemWidth(400);
        ImGui::InputTextWithHint("##new_path", "e.g. system/db/password", st.new_path, sizeof(st.new_path));
        ImGui::Spacing();
        ImGui::Text("Value:");
        ImGui::SetNextItemWidth(400);
        ImGui::InputTextMultiline("##new_value", st.new_value, sizeof(st.new_value), ImVec2(400, 100));
        ImGui::Spacing();
        if (ImGui::Button("Save", ImVec2(120, 30))) {
            if (strlen(st.new_path) > 0) {
                SecretEntry e;
                e.path = st.new_path;
                e.value = st.new_value;
                e.created = st.now_str();
                e.modified = e.created;
                st.secrets.push_back(e);
                st.apply_filter();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Edit Dialog
    if (st.show_edit_dialog) {
        ImGui::OpenPopup("Edit Secret");
        st.show_edit_dialog = false;
    }
    if (ImGui::BeginPopupModal("Edit Secret", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (st.selected_index >= 0 && st.selected_index < (int)st.filtered_secrets.size()) {
            ImGui::Text("Editing: %s", st.filtered_secrets[st.selected_index].path.c_str());
            ImGui::Separator();
            ImGui::Text("New Value:");
            ImGui::InputTextMultiline("##edit_val", st.edit_value, sizeof(st.edit_value), ImVec2(400, 100));
            ImGui::Spacing();
            if (ImGui::Button("Save", ImVec2(120, 30))) {
                // Find in main list and update
                for (auto& s : st.secrets) {
                    if (s.path == st.filtered_secrets[st.selected_index].path) {
                        s.value = st.edit_value;
                        s.modified = st.now_str();
                        break;
                    }
                }
                st.apply_filter();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 30))) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // Delete Confirmation
    if (st.show_delete_confirm) {
        ImGui::OpenPopup("Confirm Delete");
        st.show_delete_confirm = false;
    }
    if (ImGui::BeginPopupModal("Confirm Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (st.selected_index >= 0 && st.selected_index < (int)st.filtered_secrets.size()) {
            ImGui::Text("Delete secret '%s'?", st.filtered_secrets[st.selected_index].path.c_str());
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "This action cannot be undone.");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 0.8f));
            if (ImGui::Button("Delete", ImVec2(120, 30))) {
                std::string target_path = st.filtered_secrets[st.selected_index].path;
                st.secrets.erase(
                    std::remove_if(st.secrets.begin(), st.secrets.end(),
                                   [&](const SecretEntry& e) { return e.path == target_path; }),
                    st.secrets.end());
                st.selected_index = -1;
                st.apply_filter();
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 30))) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // Import Dialog
    if (st.show_import_dialog) {
        ImGui::OpenPopup("Import Secrets");
        st.show_import_dialog = false;
    }
    if (ImGui::BeginPopupModal("Import Secrets", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Import from file:");
        ImGui::SetNextItemWidth(400);
        ImGui::InputTextWithHint("##import_path", "/path/to/secrets.json", st.import_path, sizeof(st.import_path));
        ImGui::Spacing();
        if (ImGui::Button("Import", ImVec2(120, 30))) {
            // Simulate import: add sample entries
            SecretEntry e;
            e.path = "imported/key1";
            e.value = "imported_value_1";
            e.created = st.now_str();
            e.modified = e.created;
            st.secrets.push_back(e);
            e.path = "imported/key2";
            e.value = "imported_value_2";
            st.secrets.push_back(e);
            st.apply_filter();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Export Dialog
    if (st.show_export_dialog) {
        ImGui::OpenPopup("Export Secrets");
        st.show_export_dialog = false;
    }
    if (ImGui::BeginPopupModal("Export Secrets", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Export to file:");
        ImGui::SetNextItemWidth(400);
        ImGui::InputTextWithHint("##export_path", "/path/to/secrets.json", st.export_path, sizeof(st.export_path));
        ImGui::Spacing();
        ImGui::Text("Secrets to export: %zu", st.secrets.size());
        ImGui::Spacing();
        if (ImGui::Button("Export", ImVec2(120, 30))) {
            // In production: write JSON to file
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace straylight::vault
