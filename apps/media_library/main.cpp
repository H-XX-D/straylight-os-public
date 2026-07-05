// apps/media_library/main.cpp
// StrayLight Media Library — Wayland + EGL + ImGui grid/list browser
// with sidebar categories, search bar, and metadata panel.
#include "scanner.h"
#include "catalog.h"
#include "thumbnailer.h"

#include <straylight/log.h>

#include <wayland-client.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <wayland-egl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <xdg-shell-client-protocol.h>

#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <future>
#include <map>
#include <unistd.h>

namespace {

std::atomic<bool> g_running{true};
void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Wayland boilerplate (same pattern as settings/backup)
// ---------------------------------------------------------------------------

struct WaylandState {
    wl_display*    display          = nullptr;
    wl_registry*   registry         = nullptr;
    wl_compositor* compositor       = nullptr;
    wl_seat*       seat             = nullptr;
    xdg_wm_base*   xdg_wm_base_ptr = nullptr;
    wl_surface*    surface          = nullptr;
    xdg_surface*   xdg_surface_ptr  = nullptr;
    xdg_toplevel*  toplevel         = nullptr;
    wl_egl_window* egl_window       = nullptr;
    int  width = 1280, height = 800;
    bool configured = false, needs_resize = false;
};

void registry_global(void* data, wl_registry* reg, uint32_t name,
                     const char* iface, uint32_t ver);
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener reg_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};
void wm_base_ping(void*, xdg_wm_base* b, uint32_t s) { xdg_wm_base_pong(b, s); }
const xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };
void xdg_surf_configure(void* d, xdg_surface* s, uint32_t serial) {
    xdg_surface_ack_configure(s, serial);
    static_cast<WaylandState*>(d)->configured = true;
}
const xdg_surface_listener xdg_surf_listener = { .configure = xdg_surf_configure };
void tl_configure(void* d, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
    auto* ws = static_cast<WaylandState*>(d);
    if (w > 0 && h > 0 && (w != ws->width || h != ws->height)) {
        ws->width = w; ws->height = h; ws->needs_resize = true;
    }
}
void tl_close(void*, xdg_toplevel*) { g_running.store(false, std::memory_order_relaxed); }
void tl_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void tl_caps(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener tl_listener = {
    .configure        = tl_configure,
    .close            = tl_close,
    .configure_bounds = tl_bounds,
    .wm_capabilities  = tl_caps,
};
void seat_caps(void*, wl_seat*, uint32_t) {}
void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

void registry_global(void* data, wl_registry* reg, uint32_t name,
                     const char* iface, uint32_t ver) {
    auto* ws = static_cast<WaylandState*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0)
        ws->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, std::min(ver, 4u)));
    else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
        ws->xdg_wm_base_ptr = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, std::min(ver, 2u)));
        xdg_wm_base_add_listener(ws->xdg_wm_base_ptr, &wm_base_listener, ws);
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0) {
        ws->seat = static_cast<wl_seat*>(
            wl_registry_bind(reg, name, &wl_seat_interface, std::min(ver, 5u)));
        wl_seat_add_listener(ws->seat, &seat_listener, ws);
    }
}

// ---------------------------------------------------------------------------
// OpenGL texture helper
// ---------------------------------------------------------------------------

GLuint upload_texture(const straylight::media::Thumbnail& t) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 static_cast<GLsizei>(t.width),
                 static_cast<GLsizei>(t.height),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// ---------------------------------------------------------------------------
// Apply StrayLight dark theme
// ---------------------------------------------------------------------------

void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f; s.FrameRounding = 3.0f;
    s.ItemSpacing = ImVec2(8.0f, 6.0f); s.FramePadding = ImVec2(6.0f, 4.0f);
    s.WindowPadding = ImVec2(12.0f, 12.0f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.08f, 0.08f, 0.13f, 1.0f);
    c[ImGuiCol_ChildBg]       = ImVec4(0.10f, 0.10f, 0.16f, 1.0f);
    c[ImGuiCol_FrameBg]       = ImVec4(0.14f, 0.14f, 0.22f, 1.0f);
    c[ImGuiCol_Button]        = ImVec4(0.0f,  0.55f, 0.38f, 0.8f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.0f,  0.80f, 0.55f, 1.0f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.0f,  1.00f, 0.67f, 1.0f);
    c[ImGuiCol_Header]        = ImVec4(0.0f,  0.55f, 0.38f, 0.6f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.0f,  0.80f, 0.55f, 0.8f);
    c[ImGuiCol_HeaderActive]  = ImVec4(0.0f,  1.00f, 0.67f, 1.0f);
    c[ImGuiCol_Separator]     = ImVec4(0.20f, 0.20f, 0.32f, 1.0f);
    c[ImGuiCol_Text]          = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    c[ImGuiCol_TextDisabled]  = ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
    c[ImGuiCol_Border]        = ImVec4(0.20f, 0.20f, 0.32f, 1.0f);
    c[ImGuiCol_PopupBg]       = ImVec4(0.12f, 0.12f, 0.18f, 0.97f);
}

// ---------------------------------------------------------------------------
// Sidebar category enum
// ---------------------------------------------------------------------------

enum class Category { All, Images, Audio, Video };

} // anonymous namespace

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    using namespace straylight;
    using namespace straylight::media;

    Log::init("straylight-media-library");
    SL_INFO("StrayLight Media Library starting");

    gst_init(&argc, &argv);

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);

    // --- Wayland ----------------------------------------------------------
    WaylandState ws;
    ws.display = wl_display_connect(nullptr);
    if (!ws.display) { SL_CRITICAL("No Wayland display"); return EXIT_FAILURE; }
    ws.registry = wl_display_get_registry(ws.display);
    wl_registry_add_listener(ws.registry, &reg_listener, &ws);
    wl_display_roundtrip(ws.display);

    if (!ws.compositor || !ws.xdg_wm_base_ptr) {
        SL_CRITICAL("Missing Wayland globals");
        wl_display_disconnect(ws.display);
        return EXIT_FAILURE;
    }
    ws.surface         = wl_compositor_create_surface(ws.compositor);
    ws.xdg_surface_ptr = xdg_wm_base_get_xdg_surface(ws.xdg_wm_base_ptr, ws.surface);
    xdg_surface_add_listener(ws.xdg_surface_ptr, &xdg_surf_listener, &ws);
    ws.toplevel = xdg_surface_get_toplevel(ws.xdg_surface_ptr);
    xdg_toplevel_add_listener(ws.toplevel, &tl_listener, &ws);
    xdg_toplevel_set_title(ws.toplevel, "StrayLight Media Library");
    xdg_toplevel_set_app_id(ws.toplevel, "straylight-media-library");
    xdg_toplevel_set_min_size(ws.toplevel, 900, 600);
    wl_surface_commit(ws.surface);
    wl_display_roundtrip(ws.display);

    // --- EGL --------------------------------------------------------------
    EGLDisplay egl_disp = eglGetDisplay(
        reinterpret_cast<EGLNativeDisplayType>(ws.display));
    EGLint major = 0, minor = 0;
    eglInitialize(egl_disp, &major, &minor);
    constexpr EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE
    };
    EGLConfig egl_cfg = nullptr; EGLint num_cfgs = 0;
    eglChooseConfig(egl_disp, cfg_attribs, &egl_cfg, 1, &num_cfgs);
    ws.egl_window = wl_egl_window_create(ws.surface, ws.width, ws.height);
    EGLSurface egl_surf = eglCreateWindowSurface(egl_disp, egl_cfg,
        reinterpret_cast<EGLNativeWindowType>(ws.egl_window), nullptr);
    constexpr EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE
    };
    EGLContext egl_ctx = eglCreateContext(egl_disp, egl_cfg, EGL_NO_CONTEXT, ctx_attribs);
    eglMakeCurrent(egl_disp, egl_surf, egl_surf, egl_ctx);

    // --- ImGui ------------------------------------------------------------
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize  = ImVec2(float(ws.width), float(ws.height));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplOpenGL3_Init("#version 300 es");
    apply_theme();

    // --- Catalog ----------------------------------------------------------
    const char* home = std::getenv("HOME");
    fs::path db_path = home
        ? fs::path(home) / ".local" / "share" / "straylight" / "media-catalog.db"
        : fs::path("/tmp/straylight-media.db");

    Catalog catalog;
    if (auto r = catalog.open(db_path); !r.has_value()) {
        SL_ERROR("Cannot open catalog: {}", r.error().message());
    }

    Scanner    scanner;
    Thumbnailer thumbnailer;

    // --- App state --------------------------------------------------------
    char scan_path[1024] = {};
    if (home) std::strncpy(scan_path, home, sizeof(scan_path) - 1);

    char search_buf[256] = {};
    Category category       = Category::All;
    std::string sel_artist, sel_album, sel_genre;
    int  sel_media_idx = -1;
    bool view_grid      = true;
    bool scanning       = false;

    std::future<size_t> scan_future;
    std::string scan_status;

    // Cached search results and textures
    std::vector<CatalogRow> results;
    std::map<std::string, GLuint> tex_cache; // path -> GL texture ID

    // Initial search to populate
    auto do_search = [&]() {
        CatalogQuery q;
        q.text   = search_buf[0] ? search_buf : "";
        q.artist = sel_artist;
        q.album  = sel_album;
        q.genre  = sel_genre;
        switch (category) {
            case Category::Images: q.type = MediaType::Image; break;
            case Category::Audio:  q.type = MediaType::Audio; break;
            case Category::Video:  q.type = MediaType::Video; break;
            default: break;
        }
        q.limit = 500;
        auto r = catalog.search(q);
        if (r.has_value()) results = std::move(r.value());
        sel_media_idx = -1;
    };
    do_search();

    // Helper: get or generate texture for a result entry
    auto get_tex = [&](const CatalogRow& row) -> GLuint {
        auto it = tex_cache.find(row.path.string());
        if (it != tex_cache.end()) return it->second;
        if (row.type == MediaType::Audio) return 0; // no thumbnail for audio
        auto tr = thumbnailer.get(row);
        if (!tr.has_value()) return 0;
        GLuint tex = upload_texture(tr.value());
        tex_cache[row.path.string()] = tex;
        return tex;
    };

    SL_INFO("Media Library UI ready; {} items in catalog",
            catalog.count().value_or(0));

    // --- Main loop --------------------------------------------------------
    while (g_running.load(std::memory_order_relaxed)) {
        wl_display_dispatch_pending(ws.display);
        wl_display_flush(ws.display);

        if (ws.needs_resize) {
            ws.needs_resize = false;
            wl_egl_window_resize(ws.egl_window, ws.width, ws.height, 0, 0);
            io.DisplaySize = ImVec2(float(ws.width), float(ws.height));
        }

        // Check scan completion
        if (scanning && scan_future.valid() &&
            scan_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            size_t n = scan_future.get();
            scan_status = "Scan complete: " + std::to_string(n) + " files indexed.";
            scanning = false;
            do_search();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        constexpr ImGuiWindowFlags kWin =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus;

        if (ImGui::Begin("##MediaLib", nullptr, kWin)) {
            // Top bar
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
            ImGui::Text("STRAYLIGHT MEDIA LIBRARY");
            ImGui::PopStyleColor();
            ImGui::SameLine(io.DisplaySize.x - 60.0f);
            if (ImGui::SmallButton("Close"))
                g_running.store(false, std::memory_order_relaxed);
            ImGui::Separator();

            // Search bar row
            ImGui::SetNextItemWidth(300.0f);
            bool search_changed = ImGui::InputText("##search", search_buf,
                                                    sizeof(search_buf));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            if (search_changed) do_search();

            ImGui::SameLine(0.0f, 16.0f);
            ImGui::Text("View:");
            ImGui::SameLine();
            if (ImGui::RadioButton("Grid", view_grid))  view_grid = true;
            ImGui::SameLine();
            if (ImGui::RadioButton("List", !view_grid)) view_grid = false;

            ImGui::SameLine(0.0f, 16.0f);
            if (!scanning) {
                ImGui::SetNextItemWidth(300.0f);
                ImGui::InputText("##scanpath", scan_path, sizeof(scan_path));
                ImGui::SameLine();
                if (ImGui::Button("Scan")) {
                    scanning = true;
                    scan_status = "Scanning...";
                    fs::path root = scan_path;
                    scan_future = std::async(std::launch::async,
                        [&catalog, &scanner, root]() -> size_t {
                            size_t n = 0;
                            (void)catalog.begin();
                            (void)scanner.scan(root, [&](MediaEntry e) {
                                (void)catalog.upsert(e);
                                ++n;
                            });
                            (void)catalog.commit();
                            return n;
                        });
                }
            } else {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f),
                                   "%s", scan_status.c_str());
            }

            ImGui::Separator();
            ImGui::Spacing();

            float content_h = ImGui::GetContentRegionAvail().y;
            constexpr float kSidebarW  = 190.0f;
            constexpr float kMetaW     = 220.0f;

            // --- LEFT SIDEBAR (categories + artists/albums/genres) --------
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.10f, 1.0f));
            if (ImGui::BeginChild("##sidebar", ImVec2(kSidebarW, content_h), false)) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
                ImGui::TextUnformatted("CATEGORIES");
                ImGui::PopStyleColor();
                ImGui::Separator();

                auto cat_item = [&](const char* label, Category cat) {
                    if (ImGui::Selectable(label, category == cat)) {
                        category = cat; sel_artist.clear(); sel_album.clear();
                        sel_genre.clear(); do_search();
                    }
                };
                cat_item("  All Media",   Category::All);
                cat_item("  Images",      Category::Images);
                cat_item("  Audio",       Category::Audio);
                cat_item("  Video",       Category::Video);

                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
                ImGui::TextUnformatted("ARTISTS");
                ImGui::PopStyleColor();
                ImGui::Separator();

                if (auto ar = catalog.artists(); ar.has_value()) {
                    for (const auto& a : ar.value()) {
                        bool sel = (sel_artist == a);
                        std::string lbl = "  " + a + "##artist";
                        if (ImGui::Selectable(lbl.c_str(), sel)) {
                            sel_artist = sel ? "" : a;
                            sel_album.clear(); do_search();
                        }
                    }
                }

                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
                ImGui::TextUnformatted("ALBUMS");
                ImGui::PopStyleColor();
                ImGui::Separator();

                if (auto al = catalog.albums(sel_artist); al.has_value()) {
                    for (const auto& a : al.value()) {
                        bool sel = (sel_album == a);
                        std::string lbl = "  " + a + "##album";
                        if (ImGui::Selectable(lbl.c_str(), sel)) {
                            sel_album = sel ? "" : a; do_search();
                        }
                    }
                }

                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
                ImGui::TextUnformatted("GENRES");
                ImGui::PopStyleColor();
                ImGui::Separator();

                if (auto gn = catalog.genres(); gn.has_value()) {
                    for (const auto& g : gn.value()) {
                        bool sel = (sel_genre == g);
                        std::string lbl = "  " + g + "##genre";
                        if (ImGui::Selectable(lbl.c_str(), sel)) {
                            sel_genre = sel ? "" : g; do_search();
                        }
                    }
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::SameLine();

            // --- CENTER: grid or list view --------------------------------
            float center_w = io.DisplaySize.x - kSidebarW - kMetaW - 20.0f;
            if (ImGui::BeginChild("##center", ImVec2(center_w, content_h), false,
                                  ImGuiWindowFlags_HorizontalScrollbar)) {

                ImGui::Text("%zu item(s)", results.size());
                ImGui::Separator();

                if (view_grid) {
                    constexpr float kThumbSize = 120.0f;
                    constexpr float kPad       = 8.0f;
                    float avail_w = ImGui::GetContentRegionAvail().x;
                    int cols = std::max(1, int((avail_w + kPad) / (kThumbSize + kPad)));

                    int col = 0;
                    for (int i = 0; i < static_cast<int>(results.size()); ++i) {
                        const auto& row = results[i];
                        if (col > 0) ImGui::SameLine(0.0f, kPad);

                        bool selected = (sel_media_idx == i);
                        ImGui::PushID(i);

                        GLuint tex = get_tex(row);
                        if (tex != 0) {
                            ImVec4 border = selected
                                ? ImVec4(0.0f, 1.0f, 0.67f, 1.0f)
                                : ImVec4(0.2f, 0.2f, 0.3f, 1.0f);
                            if (ImGui::ImageButton("##tb",
                                    reinterpret_cast<ImTextureID>(
                                        static_cast<uintptr_t>(tex)),
                                    ImVec2(kThumbSize, kThumbSize),
                                    ImVec2(0, 0), ImVec2(1, 1),
                                    border)) {
                                sel_media_idx = i;
                            }
                        } else {
                            // No thumbnail: show a labelled button
                            std::string lbl = row.title.empty()
                                ? row.path.filename().string() : row.title;
                            if (lbl.size() > 14) lbl = lbl.substr(0, 12) + "..";
                            if (ImGui::Button(lbl.c_str(),
                                              ImVec2(kThumbSize, kThumbSize))) {
                                sel_media_idx = i;
                            }
                        }

                        // Filename tooltip
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s",
                                row.path.filename().string().c_str());
                        }

                        ImGui::PopID();

                        col = (col + 1) % cols;
                        if (col == 0) ImGui::NewLine();
                    }
                } else {
                    // List view
                    if (ImGui::BeginTable("##list", 4,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                            ImVec2(0.0f, content_h - 30.0f))) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("Title",  ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Artist", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                        ImGui::TableSetupColumn("Album",  ImGuiTableColumnFlags_WidthFixed, 140.0f);
                        ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed, 60.0f);
                        ImGui::TableHeadersRow();

                        for (int i = 0; i < static_cast<int>(results.size()); ++i) {
                            const auto& row = results[i];
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            bool sel = (sel_media_idx == i);
                            const std::string& display_name =
                                row.title.empty()
                                    ? row.path.filename().string()
                                    : row.title;
                            char lbl[256];
                            std::snprintf(lbl, sizeof(lbl), "%s##row%d",
                                          display_name.c_str(), i);
                            if (ImGui::Selectable(lbl, sel,
                                    ImGuiSelectableFlags_SpanAllColumns)) {
                                sel_media_idx = i;
                            }
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(row.artist.c_str());
                            ImGui::TableSetColumnIndex(2);
                            ImGui::TextUnformatted(row.album.c_str());
                            ImGui::TableSetColumnIndex(3);
                            switch (row.type) {
                                case MediaType::Image: ImGui::TextUnformatted("IMG");   break;
                                case MediaType::Audio: ImGui::TextUnformatted("AUDIO"); break;
                                case MediaType::Video: ImGui::TextUnformatted("VIDEO"); break;
                                default:               ImGui::TextUnformatted("?");     break;
                            }
                        }
                        ImGui::EndTable();
                    }
                }
            }
            ImGui::EndChild();
            ImGui::SameLine();

            // --- RIGHT: Metadata panel -----------------------------------
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.10f, 1.0f));
            if (ImGui::BeginChild("##meta", ImVec2(kMetaW, content_h), false)) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
                ImGui::TextUnformatted("METADATA");
                ImGui::PopStyleColor();
                ImGui::Separator();
                ImGui::Spacing();

                if (sel_media_idx >= 0 &&
                    sel_media_idx < static_cast<int>(results.size())) {
                    const auto& row = results[sel_media_idx];

                    // Thumbnail preview
                    GLuint tex = get_tex(row);
                    if (tex != 0) {
                        float preview_w = kMetaW - 24.0f;
                        ImGui::Image(reinterpret_cast<ImTextureID>(
                                         static_cast<uintptr_t>(tex)),
                                     ImVec2(preview_w, preview_w));
                        ImGui::Spacing();
                    }

                    auto field = [](const char* label, const std::string& val) {
                        if (!val.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                            ImGui::TextUnformatted(label);
                            ImGui::PopStyleColor();
                            ImGui::SameLine(80.0f);
                            ImGui::TextWrapped("%s", val.c_str());
                        }
                    };

                    field("File:",   row.path.filename().string());
                    field("Title:",  row.title);
                    field("Artist:", row.artist);
                    field("Album:",  row.album);
                    field("Genre:",  row.genre);
                    if (row.year > 0)
                        field("Year:", std::to_string(row.year));
                    if (row.duration_ms > 0) {
                        uint64_t secs = row.duration_ms / 1000;
                        char dur[32];
                        std::snprintf(dur, sizeof(dur), "%llu:%02llu",
                                      static_cast<unsigned long long>(secs / 60),
                                      static_cast<unsigned long long>(secs % 60));
                        field("Dur:", dur);
                    }
                    if (row.width > 0 && row.height > 0) {
                        field("Size:", std::to_string(row.width) + "x" +
                                       std::to_string(row.height));
                    }

                    // File size
                    char fsz[32];
                    if (row.file_size >= 1024 * 1024)
                        std::snprintf(fsz, sizeof(fsz), "%.1f MB",
                                      double(row.file_size) / (1024.0 * 1024.0));
                    else
                        std::snprintf(fsz, sizeof(fsz), "%.1f KB",
                                      double(row.file_size) / 1024.0);
                    field("File sz:", fsz);

                    ImGui::Spacing();
                    ImGui::TextWrapped("%s", row.path.c_str());
                } else {
                    ImGui::TextDisabled("Select an item to see details.");
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, ws.width, ws.height);
        glClearColor(0.08f, 0.08f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(egl_disp, egl_surf);

        usleep(16000);
    }

    // --- Cleanup ----------------------------------------------------------
    SL_INFO("Media Library shutting down");

    for (auto& [path, tex] : tex_cache) glDeleteTextures(1, &tex);

    catalog.close();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();

    eglMakeCurrent(egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl_disp, egl_surf);
    eglDestroyContext(egl_disp, egl_ctx);
    eglTerminate(egl_disp);

    wl_egl_window_destroy(ws.egl_window);
    xdg_toplevel_destroy(ws.toplevel);
    xdg_surface_destroy(ws.xdg_surface_ptr);
    wl_surface_destroy(ws.surface);
    if (ws.seat) wl_seat_destroy(ws.seat);
    xdg_wm_base_destroy(ws.xdg_wm_base_ptr);
    wl_compositor_destroy(ws.compositor);
    wl_registry_destroy(ws.registry);
    wl_display_disconnect(ws.display);

    gst_deinit();
    SL_INFO("Media Library exited cleanly");
    return EXIT_SUCCESS;
}
