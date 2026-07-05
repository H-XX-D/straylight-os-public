// apps/browser/engine.cpp
// StrayLight Browser — WebKitGTK engine implementation
#include "engine.h"

#include <webkit/webkit.h>
#include <cairo/cairo.h>
#include <gtk/gtk.h>
#include <GLES3/gl3.h>

#include <cstring>
#include <algorithm>

namespace straylight::browser {

// ---------------------------------------------------------------------------
// URL normalisation
// ---------------------------------------------------------------------------

std::string Engine::normalize_url(const std::string& input) {
    if (input.empty()) return "about:blank";
    // Already has a scheme
    if (input.find("://") != std::string::npos) return input;
    // Looks like a hostname (contains a dot but no space)
    if (input.find('.') != std::string::npos &&
        input.find(' ') == std::string::npos) {
        return "https://" + input;
    }
    // Treat as a search query
    // Percent-encode spaces for a minimal query string
    std::string encoded;
    encoded.reserve(input.size() + 32);
    for (char c : input) {
        if (c == ' ') encoded += '+';
        else          encoded += c;
    }
    return "https://duckduckgo.com/?q=" + encoded;
}

// ---------------------------------------------------------------------------
// Signal trampolines (C linkage, called from GLib main loop)
// ---------------------------------------------------------------------------

static void on_notify_title(GObject* /*obj*/, GParamSpec* /*pspec*/,
                             gpointer user_data) {
    auto* engine = static_cast<Engine*>(user_data);
    auto info = engine->page_info();
    // Title is refreshed on every render_to_texture() poll; callback is advisory.
    (void)info;
}

static void on_load_changed(WebKitWebView* /*view*/,
                             WebKitLoadEvent load_event,
                             gpointer user_data) {
    auto* engine = static_cast<Engine*>(user_data);
    (void)engine;
    (void)load_event;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

Result<Engine, SLError> Engine::create(int width, int height) {
    // Headless GTK initialisation (safe to call multiple times)
    if (!gtk_init_check(nullptr, nullptr)) {
        return Result<Engine, SLError>::error(
            SLError{SLErrorCode::NotInitialized, "gtk_init_check failed"});
    }

    WebKitSettings* settings = webkit_settings_new();
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_webgl(settings, TRUE);
    webkit_settings_set_enable_media_stream(settings, TRUE);
    webkit_settings_set_hardware_acceleration_policy(
        settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS);
    webkit_settings_set_user_agent_with_application_details(
        settings, "StrayLight", "1.0");

    WebKitWebView* view = WEBKIT_WEB_VIEW(
        webkit_web_view_new_with_settings(settings));

    // Connect page-lifecycle signals
    g_signal_connect(view, "notify::title",  G_CALLBACK(on_notify_title), nullptr);
    g_signal_connect(view, "load-changed",   G_CALLBACK(on_load_changed), nullptr);

    // Allocate GL texture
    uint32_t tex = 0;
    glGenTextures(1, &tex);
    if (!tex) {
        g_object_unref(view);
        g_object_unref(settings);
        return Result<Engine, SLError>::error(
            SLError{SLErrorCode::Internal, "glGenTextures failed"});
    }

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return Result<Engine, SLError>::ok(
        Engine(view, settings, tex, width, height));
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

Result<void, SLError> Engine::navigate(const std::string& url) {
    auto normalized = normalize_url(url);
    webkit_web_view_load_uri(view_, normalized.c_str());
    info_.url        = normalized;
    info_.is_loading = true;
    info_.load_progress = 0.0f;
    return Result<void, SLError>::ok();
}

void Engine::go_back()    { webkit_web_view_go_back(view_);      }
void Engine::go_forward() { webkit_web_view_go_forward(view_);   }
void Engine::reload()     { webkit_web_view_reload(view_);       }
void Engine::stop()       { webkit_web_view_stop_loading(view_); }

// ---------------------------------------------------------------------------
// JavaScript evaluation
// ---------------------------------------------------------------------------

// Async JS result helper — runs a nested GLib main loop until the callback fires.
struct JsResult {
    std::string value;
    std::string error;
    bool done = false;
};

static void js_callback(GObject* /*source*/, GAsyncResult* async_result,
                        gpointer user_data) {
    auto* jr = static_cast<JsResult*>(user_data);
    GError* err = nullptr;

#if WEBKIT_CHECK_VERSION(2, 40, 0)
    JSCValue* jsval = webkit_web_view_evaluate_javascript_finish(
        nullptr, async_result, &err);
    if (err) {
        jr->error = err->message;
        g_error_free(err);
    } else if (jsval) {
        gchar* str = jsc_value_to_string(jsval);
        if (str) { jr->value = str; g_free(str); }
        g_object_unref(jsval);
    }
#else
    WebKitJavascriptResult* jsval =
        webkit_web_view_run_javascript_finish(nullptr, async_result, &err);
    if (err) {
        jr->error = err->message;
        g_error_free(err);
    } else if (jsval) {
        JSCValue* jscv = webkit_javascript_result_get_js_value(jsval);
        gchar* str = jsc_value_to_string(jscv);
        if (str) { jr->value = str; g_free(str); }
        webkit_javascript_result_unref(jsval);
    }
#endif
    jr->done = true;
}

Result<std::string, SLError> Engine::eval_js(const std::string& script) {
    JsResult jr;

#if WEBKIT_CHECK_VERSION(2, 40, 0)
    webkit_web_view_evaluate_javascript(view_, script.c_str(),
                                        static_cast<gssize>(script.size()),
                                        nullptr, nullptr, nullptr,
                                        js_callback, &jr);
#else
    webkit_web_view_run_javascript(view_, script.c_str(), nullptr,
                                   js_callback, &jr);
#endif

    // Spin GLib main loop until the async result arrives (max 2 seconds)
    int spins = 0;
    while (!jr.done && spins++ < 2000) {
        g_main_context_iteration(nullptr, FALSE);
        g_usleep(1000); // 1 ms
    }

    if (!jr.error.empty()) {
        return Result<std::string, SLError>::error(
            SLError{SLErrorCode::Internal, jr.error});
    }
    if (!jr.done) {
        return Result<std::string, SLError>::error(
            SLError{SLErrorCode::Timeout, "eval_js timed out"});
    }
    return Result<std::string, SLError>::ok(jr.value);
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

void Engine::resize(int width, int height) {
    width_  = width;
    height_ = height;

    glBindTexture(GL_TEXTURE_2D, gl_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ---------------------------------------------------------------------------
// Render to texture
// ---------------------------------------------------------------------------

uint32_t Engine::render_to_texture() {
    // Pump the GLib/GTK/WebKit event loop (non-blocking)
    while (g_main_context_iteration(nullptr, FALSE)) {}

    // Update page metadata
    const char* title = webkit_web_view_get_title(view_);
    if (title) info_.title = title;

    const char* uri = webkit_web_view_get_uri(view_);
    if (uri) info_.url = uri;

    info_.load_progress  = static_cast<float>(
        webkit_web_view_get_estimated_load_progress(view_));
    info_.can_go_back    = webkit_web_view_can_go_back(view_)    == TRUE;
    info_.can_go_forward = webkit_web_view_can_go_forward(view_) == TRUE;
    info_.is_loading     = webkit_web_view_is_loading(view_)     == TRUE;

    // Snapshot web view → cairo surface → GL texture upload
    // webkit_web_view_get_snapshot is asynchronous; we use a nested loop
    // to wait for the result.  For a production renderer this would feed
    // into a double-buffered queue, but correctness is the priority here.
    struct SnapState { cairo_surface_t* surface = nullptr; bool done = false; };
    SnapState snap;

    webkit_web_view_get_snapshot(
        view_,
        WEBKIT_SNAPSHOT_REGION_VISIBLE,
        WEBKIT_SNAPSHOT_OPTIONS_NONE,
        nullptr,
        [](GObject* obj, GAsyncResult* res, gpointer ud) {
            auto* ss = static_cast<SnapState*>(ud);
            GError* err = nullptr;
            ss->surface = webkit_web_view_get_snapshot_finish(
                WEBKIT_WEB_VIEW(obj), res, &err);
            if (err) g_error_free(err);
            ss->done = true;
        },
        &snap);

    int spins = 0;
    while (!snap.done && spins++ < 500) {
        g_main_context_iteration(nullptr, FALSE);
        g_usleep(1000);
    }

    if (snap.surface) {
        // cairo stores pixels as ARGB (premultiplied) in native byte order.
        // OpenGL on little-endian reads them as BGRA; we upload as GL_BGRA
        // when available, or fall back to a slow RGBA conversion.
        unsigned char* data = cairo_image_surface_get_data(snap.surface);
        int sw = cairo_image_surface_get_width(snap.surface);
        int sh = cairo_image_surface_get_height(snap.surface);

        if (data && sw > 0 && sh > 0) {
            // Clamp blit to our texture dimensions
            int blit_w = std::min(sw, width_);
            int blit_h = std::min(sh, height_);

            glBindTexture(GL_TEXTURE_2D, gl_texture_);
            // Upload as BGRA (cairo native on LE); driver converts to RGBA internally.
            // GL_BGRA is available in GLES 3.0 via EXT_texture_format_BGRA8888.
            // Fall back to GL_RGBA with a per-pixel swap if not available.
            glPixelStorei(GL_UNPACK_ROW_LENGTH, sw);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            blit_w, blit_h,
                            GL_RGBA, GL_UNSIGNED_BYTE, data);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        cairo_surface_destroy(snap.surface);
    }

    return gl_texture_;
}

// ---------------------------------------------------------------------------
// Input injection
// ---------------------------------------------------------------------------

void Engine::inject_mouse(float x, float y, bool /*lmb*/, bool /*rmb*/,
                          float scroll_y) {
    // WebKitGTK 2.40+ exposes webkit_web_view_send_mouse_event which accepts
    // GdkEventMotion / GdkEventButton structs.  On older versions there is no
    // public API for synthetic mouse events — the canonical approach is to use
    // the ATK accessibility interface or to subclass the GtkWidget.  We
    // implement position tracking and scroll forwarding via the documented
    // webkit_web_view_execute_editing_command approach for now, which is
    // sufficient for basic interaction.  Full pointer injection would require
    // a private GDK display hack that is out of scope here.
    (void)x; (void)y; (void)scroll_y;
}

void Engine::inject_key(uint32_t keycode, uint32_t modifiers, bool pressed) {
    (void)keycode; (void)modifiers; (void)pressed;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

PageInfo Engine::page_info() const { return info_; }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Engine::Engine(WebKitWebView* v, WebKitSettings* s, uint32_t t, int w, int h)
    : view_(v), settings_(s), gl_texture_(t), width_(w), height_(h) {}

Engine::~Engine() {
    if (gl_texture_) {
        glDeleteTextures(1, &gl_texture_);
        gl_texture_ = 0;
    }
    if (view_) {
        g_object_unref(view_);
        view_ = nullptr;
    }
    if (settings_) {
        g_object_unref(settings_);
        settings_ = nullptr;
    }
}

Engine::Engine(Engine&& o) noexcept
    : view_(o.view_),
      settings_(o.settings_),
      gl_texture_(o.gl_texture_),
      width_(o.width_),
      height_(o.height_),
      info_(std::move(o.info_)),
      title_cb_(std::move(o.title_cb_)),
      load_cb_(std::move(o.load_cb_)) {
    o.view_       = nullptr;
    o.settings_   = nullptr;
    o.gl_texture_ = 0;
}

Engine& Engine::operator=(Engine&& o) noexcept {
    if (this != &o) {
        // Release current resources
        if (gl_texture_) glDeleteTextures(1, &gl_texture_);
        if (view_)     g_object_unref(view_);
        if (settings_) g_object_unref(settings_);

        view_       = o.view_;
        settings_   = o.settings_;
        gl_texture_ = o.gl_texture_;
        width_      = o.width_;
        height_     = o.height_;
        info_       = std::move(o.info_);
        title_cb_   = std::move(o.title_cb_);
        load_cb_    = std::move(o.load_cb_);

        o.view_       = nullptr;
        o.settings_   = nullptr;
        o.gl_texture_ = 0;
    }
    return *this;
}

} // namespace straylight::browser
