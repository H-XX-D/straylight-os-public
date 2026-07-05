// apps/editor/syntax.h
// StrayLight Editor — syntax highlighting engine
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace straylight::editor {

/// RGBA colour packed as 0xRRGGBBAA (same layout as ImGui's IM_COL32).
using Color = uint32_t;

/// Token kinds — each maps to a distinct colour in the theme.
enum class TokenKind : uint8_t {
    Normal,
    Keyword,
    Type,
    Builtin,
    Literal,       ///< string / char / number literals
    Comment,
    Preprocessor,  ///< #include, #define …
    Operator,
    Punctuation,
    Identifier,
};

/// A coloured span within a single line.
struct Token {
    int       col_start = 0;  ///< 0-based column within the line
    int       col_end   = 0;  ///< exclusive
    TokenKind kind      = TokenKind::Normal;
};

/// Per-line tokenisation result.
using LineTokens = std::vector<Token>;

/// Language selector.
enum class Language : uint8_t {
    None,
    Cpp,
    Python,
    Rust,
    Json,
    Markdown,
};

/// Cyberpunk-themed colour palette.
struct SyntaxTheme {
    Color normal      = 0xD4D4D4FF;
    Color keyword     = 0xFF79C6FF;
    Color type_kw     = 0x8BE9FDFF;
    Color builtin     = 0x50FA7BFF;
    Color literal     = 0xF1FA8CFF;
    Color comment     = 0x6272A4FF;
    Color preprocessor= 0xFFB86CFF;
    Color op          = 0xFF5555FF;
    Color punctuation = 0xBD93F9FF;
    Color identifier  = 0xF8F8F2FF;
};

/// Stateless syntax highlighter — tokenises one line at a time.
class SyntaxHighlighter {
public:
    SyntaxHighlighter();

    /// Set the active language (detected or user-chosen).
    void set_language(Language lang);
    Language language() const { return lang_; }

    /// Detect language from file extension.
    static Language detect(std::string_view filename);

    /// Tokenise a single line. Context-free (no cross-line state).
    /// For block-comments spanning lines, pass `in_block_comment` state.
    LineTokens tokenise(std::string_view line,
                        bool& in_block_comment) const;

    /// Colour for a given token kind.
    Color colour_for(TokenKind kind) const;

    const SyntaxTheme& theme() const { return theme_; }
    SyntaxTheme& theme() { return theme_; }

private:
    Language    lang_  = Language::None;
    SyntaxTheme theme_;

    LineTokens tokenise_cpp    (std::string_view line, bool& in_block) const;
    LineTokens tokenise_python (std::string_view line, bool& in_block) const;
    LineTokens tokenise_rust   (std::string_view line, bool& in_block) const;
    LineTokens tokenise_json   (std::string_view line, bool& in_block) const;
    LineTokens tokenise_markdown(std::string_view line, bool& in_block) const;

    static bool is_cpp_keyword (std::string_view word);
    static bool is_cpp_type    (std::string_view word);
    static bool is_py_keyword  (std::string_view word);
    static bool is_py_builtin  (std::string_view word);
    static bool is_rust_keyword(std::string_view word);
    static bool is_rust_type   (std::string_view word);
};

} // namespace straylight::editor
