// lib/common/include/straylight/widget.h
#pragma once

#include <string>
#include <chrono>

namespace straylight {

class WidgetBase {
public:
    virtual ~WidgetBase() = default;
    virtual const char* name() const = 0;
    virtual void update() = 0;
    virtual void render(bool* p_open) = 0;
    virtual float poll_interval() const { return 1.0f; }
    virtual bool supports_embed() const { return true; }

protected:
    std::chrono::steady_clock::time_point last_update_{};

    bool should_update() {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<float>(now - last_update_).count() >= poll_interval()) {
            last_update_ = now;
            return true;
        }
        return false;
    }
};

} // namespace straylight
