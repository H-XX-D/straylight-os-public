// apps/widgets/widget_registry.h
#pragma once

#include <straylight/widget.h>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight::widgets {

/// Factory function type for creating widget instances.
using WidgetFactory = std::function<std::unique_ptr<WidgetBase>()>;

/// Category grouping for shell sidebar / dashboard organization.
enum class WidgetCategory {
    ML,         // Machine learning monitoring
    HPC,        // High-performance computing
    System,     // OS-level system metrics
    Research,   // Scientific visualization
};

inline const char* category_name(WidgetCategory cat) {
    switch (cat) {
        case WidgetCategory::ML:       return "Machine Learning";
        case WidgetCategory::HPC:      return "HPC";
        case WidgetCategory::System:   return "System";
        case WidgetCategory::Research: return "Research";
    }
    return "Unknown";
}

struct WidgetEntry {
    std::string id;                      // unique slug e.g. "gpu_hud"
    std::string display_name;            // human-readable
    WidgetCategory category;
    WidgetFactory factory;
    float default_poll_interval = 1.0f;  // seconds
};

/// Singleton widget registry — widgets self-register at static init time.
class WidgetRegistry {
public:
    static WidgetRegistry& instance();

    /// Register a widget factory. Called by REGISTER_WIDGET macro.
    void add(WidgetEntry entry);

    /// Create a widget by ID. Returns nullptr if not found.
    std::unique_ptr<WidgetBase> create(const std::string& id) const;

    /// Get all registered entries (for building dashboards).
    const std::vector<WidgetEntry>& entries() const { return entries_; }

    /// Get entries filtered by category.
    std::vector<const WidgetEntry*> by_category(WidgetCategory cat) const;

    /// Check if a widget ID is registered.
    bool has(const std::string& id) const;

    /// Total registered widget count.
    size_t size() const { return entries_.size(); }

private:
    WidgetRegistry() = default;
    std::vector<WidgetEntry> entries_;
    std::unordered_map<std::string, size_t> index_; // id → entries_ index
};

/// Auto-registration helper. Place in .cpp files:
///   static WidgetRegistrar<GpuHudWidget> reg_("gpu_hud", "GPU HUD", WidgetCategory::ML);
template<typename W>
struct WidgetRegistrar {
    WidgetRegistrar(const char* id, const char* name, WidgetCategory cat, float poll = 1.0f) {
        WidgetRegistry::instance().add({
            .id = id,
            .display_name = name,
            .category = cat,
            .factory = []() -> std::unique_ptr<WidgetBase> {
                return std::make_unique<W>();
            },
            .default_poll_interval = poll,
        });
    }
};

/// Convenience macro for widget self-registration.
#define STRAYLIGHT_WIDGET_CONCAT_IMPL(a, b) a##b
#define STRAYLIGHT_WIDGET_CONCAT(a, b) STRAYLIGHT_WIDGET_CONCAT_IMPL(a, b)
#define REGISTER_WIDGET(Class, id, name, cat) \
    static ::straylight::widgets::WidgetRegistrar<Class> \
        STRAYLIGHT_WIDGET_CONCAT(_straylight_widget_reg_, __COUNTER__)(id, name, cat)

} // namespace straylight::widgets
