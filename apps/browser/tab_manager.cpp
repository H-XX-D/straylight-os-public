// apps/browser/tab_manager.cpp
// StrayLight Browser — multi-tab manager implementation
#include "tab_manager.h"
#include <imgui.h>
#include <algorithm>

namespace straylight::browser {

Result<void, SLError> TabManager::new_tab(const std::string& url,
                                           int width, int height) {
    auto engine_result = Engine::create(width, height);
    if (!engine_result.has_value()) {
        return Result<void, SLError>::error(engine_result.error());
    }

    Tab tab;
    tab.engine = std::make_unique<Engine>(std::move(engine_result).value());
    tab.url    = url;
    tab.title  = url.empty() ? "New Tab" : url;

    if (!url.empty()) {
        auto nav = tab.engine->navigate(url);
        (void)nav; // navigation errors surfaced via page_info().is_loading
    }

    tabs_.push_back(std::move(tab));
    active_ = tabs_.size() - 1;
    return Result<void, SLError>::ok();
}

void TabManager::close_tab(size_t index) {
    if (index >= tabs_.size()) return;
    tabs_.erase(tabs_.begin() + static_cast<ptrdiff_t>(index));
    if (!tabs_.empty() && active_ >= tabs_.size())
        active_ = tabs_.size() - 1;
}

void TabManager::switch_to(size_t index) {
    if (index < tabs_.size()) active_ = index;
}

bool TabManager::draw_tab_bar() {
    bool changed = false;

    if (ImGui::BeginTabBar("##BrowserTabs",
                           ImGuiTabBarFlags_Reorderable |
                           ImGuiTabBarFlags_AutoSelectNewTabs |
                           ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (size_t i = 0; i < tabs_.size(); ++i) {
            bool open = true;

            const std::string& raw_title = tabs_[i].title;
            std::string label = raw_title.empty() ? "Loading..." : raw_title;
            if (label.size() > 24) label = label.substr(0, 21) + "...";
            label += "##tab" + std::to_string(i);

            ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
            if (i == active_) flags |= ImGuiTabItemFlags_SetSelected;
            if (tabs_[i].pinned) flags |= ImGuiTabItemFlags_NoCloseWithMiddleMouseButton;

            if (ImGui::BeginTabItem(label.c_str(), &open, flags)) {
                if (i != active_) { active_ = i; changed = true; }
                ImGui::EndTabItem();
            }

            if (!open) {
                close_tab(i);
                --i;
                changed = true;
            }
        }

        // New-tab button (trailing)
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing |
                                       ImGuiTabItemFlags_NoTooltip)) {
            new_tab("", 1280, 720);
            changed = true;
        }

        ImGui::EndTabBar();
    }

    return changed;
}

} // namespace straylight::browser
