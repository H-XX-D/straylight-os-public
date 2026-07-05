// apps/greeter/ui.h
// Greeter login screen UI
#pragma once

#include <straylight/error.h>

#include <string>

namespace straylight::greeter {

/// ImGui-based login screen rendered on the session lock surface.
/// Centered card layout with username, password (masked), login button,
/// error display, and session selector.
class GreeterUI {
public:
    GreeterUI();
    ~GreeterUI();

    /// Render the login UI. Called once per frame.
    /// Returns true if the user submitted credentials (login button or Enter).
    bool render();

    /// Get the entered username.
    [[nodiscard]] const std::string& username() const;

    /// Get the entered password.
    [[nodiscard]] const std::string& password() const;

    /// Set an error message to display (e.g., after auth failure).
    void set_error(const std::string& message);

    /// Clear the error message.
    void clear_error();

    /// Get the selected session type.
    [[nodiscard]] const std::string& selected_session() const;

    /// Returns true if an error is currently displayed.
    [[nodiscard]] bool has_error() const;

private:
    char username_buf_[256] = {};
    char password_buf_[256] = {};
    std::string username_;
    std::string password_;
    std::string error_message_;
    std::string selected_session_ = "straylight";
    int session_index_ = 0;
    bool submitted_ = false;

    void render_background();
    void render_login_card(float screen_width, float screen_height);
};

} // namespace straylight::greeter
