// apps/browser/engine.h
// StrayLight Browser — WebKitGTK web view wrapper (off-screen rendering to GL texture)
#pragma once
#include <straylight/result.h>
#include <straylight/error.h>
#include <string>
#include <functional>
#include <cstdint>

// Forward declarations — WebKitGTK types
typedef struct _WebKitWebView    WebKitWebView;
typedef struct _WebKitSettings   WebKitSettings;

namespace straylight::browser {

enum class TlsStatus { None, Valid, Invalid };

struct PageInfo {
    std::string title;
    std::string url;
    float       load_progress  = 0.0f;   // 0.0 – 1.0
    TlsStatus   tls            = TlsStatus::None;
    bool        can_go_back    = false;
    bool        can_go_forward = false;
    bool        is_loading     = false;
};

using TitleChangedCb = std::function<void(const std::string&)>;
using LoadChangedCb  = std::function<void(float progress, bool finished)>;

class Engine {
public:
    /// Factory — initialises GTK (headless), creates an off-screen WebKitWebView
    /// and allocates an RGBA GL texture of the given dimensions.
    static Result<Engine, SLError> create(int width, int height);

    /// Load a URL.  Auto-prepends https:// when no scheme is present;
    /// bare terms without a dot become DuckDuckGo searches.
    Result<void, SLError> navigate(const std::string& url);

    void go_back();
    void go_forward();
    void reload();
    void stop();

    /// Evaluate JavaScript in page context; returns stringified result.
    Result<std::string, SLError> eval_js(const std::string& script);

    /// Resize the off-screen web view and re-allocate the GL texture.
    void resize(int width, int height);

    /// Pump WebKit/GTK events, snapshot the web view, upload pixels to the
    /// GL texture, and return the OpenGL texture ID for ImGui rendering.
    uint32_t render_to_texture();

    /// Forward synthetic input from ImGui into the WebKit process.
    void inject_mouse(float x, float y, bool lmb, bool rmb, float scroll_y);
    void inject_key(uint32_t keycode, uint32_t modifiers, bool pressed);

    PageInfo page_info() const;

    void on_title_changed(TitleChangedCb cb) { title_cb_ = std::move(cb); }
    void on_load_changed(LoadChangedCb  cb) { load_cb_  = std::move(cb); }

    ~Engine();
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

private:
    WebKitWebView*  view_       = nullptr;
    WebKitSettings* settings_   = nullptr;
    uint32_t        gl_texture_ = 0;
    int             width_      = 0;
    int             height_     = 0;
    PageInfo        info_;
    TitleChangedCb  title_cb_;
    LoadChangedCb   load_cb_;

    explicit Engine(WebKitWebView* view, WebKitSettings* settings,
                    uint32_t texture, int w, int h);

    static std::string normalize_url(const std::string& input);
};

} // namespace straylight::browser
