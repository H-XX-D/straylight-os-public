// apps/browser/tab_manager.h
// StrayLight Browser — multi-tab state manager
#pragma once
#include "engine.h"
#include <straylight/result.h>
#include <straylight/error.h>
#include <vector>
#include <memory>
#include <string>
#include <cstddef>

namespace straylight::browser {

struct Tab {
    std::unique_ptr<Engine> engine;
    std::string             title = "New Tab";
    std::string             url;
    bool                    pinned = false;
};

class TabManager {
public:
    /// Create a new tab and optionally navigate to a URL.
    Result<void, SLError> new_tab(const std::string& url, int width, int height);

    /// Close the tab at the given index.  No-op if index is out of range.
    void close_tab(size_t index);

    /// Switch the active tab.  No-op if index is out of range.
    void switch_to(size_t index);

    size_t       active_index() const { return active_; }
    Tab&         active_tab()         { return tabs_[active_]; }
    const Tab&   active_tab() const   { return tabs_[active_]; }
    const std::vector<Tab>& tabs() const { return tabs_; }
    bool empty() const { return tabs_.empty(); }
    size_t size() const { return tabs_.size(); }

    /// Render the ImGui tab bar.  Returns true when the active tab changes.
    bool draw_tab_bar();

private:
    std::vector<Tab> tabs_;
    size_t           active_ = 0;
};

} // namespace straylight::browser
