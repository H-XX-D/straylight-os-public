// apps/widgets/widget_registry.cpp
#include "widget_registry.h"
#include <algorithm>

namespace straylight::widgets {

WidgetRegistry& WidgetRegistry::instance() {
    static WidgetRegistry reg;
    return reg;
}

void WidgetRegistry::add(WidgetEntry entry) {
    index_[entry.id] = entries_.size();
    entries_.push_back(std::move(entry));
}

std::unique_ptr<WidgetBase> WidgetRegistry::create(const std::string& id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return nullptr;
    return entries_[it->second].factory();
}

std::vector<const WidgetEntry*> WidgetRegistry::by_category(WidgetCategory cat) const {
    std::vector<const WidgetEntry*> result;
    for (const auto& e : entries_) {
        if (e.category == cat) {
            result.push_back(&e);
        }
    }
    return result;
}

bool WidgetRegistry::has(const std::string& id) const {
    return index_.count(id) > 0;
}

} // namespace straylight::widgets
