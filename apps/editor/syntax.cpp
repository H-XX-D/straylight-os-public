// apps/editor/syntax.cpp
// StrayLight Editor — syntax highlighting implementation
#include "syntax.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace straylight::editor {

// ---------------------------------------------------------------------------
// Keyword tables
// ---------------------------------------------------------------------------

static const std::unordered_set<std::string_view> kCppKeywords = {
    "alignas","alignof","and","and_eq","asm","auto","bitand","bitor",
    "break","case","catch","class","co_await","co_return","co_yield",
    "compl","concept","const","const_cast","consteval","constexpr",
    "constinit","continue","decltype","default","delete","do","dynamic_cast",
    "else","enum","explicit","export","extern","false","for","friend",
    "goto","if","inline","mutable","namespace","new","noexcept","not",
    "not_eq","nullptr","operator","or","or_eq","private","protected",
    "public","register","reinterpret_cast","requires","return","sizeof",
    "static","static_assert","static_cast","struct","switch","template",
    "this","thread_local","throw","true","try","typedef","typeid",
    "typename","union","using","virtual","volatile","while","xor","xor_eq",
};

static const std::unordered_set<std::string_view> kCppTypes = {
    "bool","char","char8_t","char16_t","char32_t","double","float",
    "int","long","ptrdiff_t","short","signed","size_t","uint8_t",
    "uint16_t","uint32_t","uint64_t","int8_t","int16_t","int32_t",
    "int64_t","uintptr_t","intptr_t","unsigned","void","wchar_t",
    "string","string_view","vector","map","unordered_map","set",
    "unordered_set","pair","tuple","optional","variant","any",
    "unique_ptr","shared_ptr","weak_ptr","array","span","initializer_list",
};

static const std::unordered_set<std::string_view> kPyKeywords = {
    "False","None","True","and","as","assert","async","await",
    "break","class","continue","def","del","elif","else","except",
    "finally","for","from","global","if","import","in","is",
    "lambda","nonlocal","not","or","pass","raise","return","try",
    "while","with","yield",
};

static const std::unordered_set<std::string_view> kPyBuiltins = {
    "abs","all","any","ascii","bin","bool","breakpoint","bytearray",
    "bytes","callable","chr","classmethod","compile","complex","delattr",
    "dict","dir","divmod","enumerate","eval","exec","filter","float",
    "format","frozenset","getattr","globals","hasattr","hash","help",
    "hex","id","input","int","isinstance","issubclass","iter","len",
    "list","locals","map","max","memoryview","min","next","object",
    "oct","open","ord","pow","print","property","range","repr",
    "reversed","round","set","setattr","slice","sorted","staticmethod",
    "str","sum","super","tuple","type","vars","zip",
};

static const std::unordered_set<std::string_view> kRustKeywords = {
    "as","async","await","break","const","continue","crate","dyn",
    "else","enum","extern","false","fn","for","if","impl","in",
    "let","loop","match","mod","move","mut","pub","ref","return",
    "self","Self","static","struct","super","trait","true","type",
    "union","unsafe","use","where","while","yield",
};

static const std::unordered_set<std::string_view> kRustTypes = {
    "bool","char","f32","f64","i8","i16","i32","i64","i128","isize",
    "str","u8","u16","u32","u64","u128","usize",
    "String","Vec","Box","Rc","Arc","Option","Result","HashMap",
    "HashSet","BTreeMap","BTreeSet","Cow","Cell","RefCell","Mutex",
    "RwLock","AtomicBool","AtomicI32","AtomicU32","AtomicUsize",
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SyntaxHighlighter::SyntaxHighlighter() = default;

void SyntaxHighlighter::set_language(Language lang) { lang_ = lang; }

// ---------------------------------------------------------------------------
// Language detection
// ---------------------------------------------------------------------------

Language SyntaxHighlighter::detect(std::string_view filename) {
    const auto dot = filename.rfind('.');
    if (dot == std::string_view::npos) return Language::None;
    const auto ext = filename.substr(dot + 1);

    if (ext == "cpp" || ext == "cxx" || ext == "cc" || ext == "c" ||
        ext == "h"   || ext == "hpp" || ext == "hxx")
        return Language::Cpp;
    if (ext == "py" || ext == "pyw")
        return Language::Python;
    if (ext == "rs")
        return Language::Rust;
    if (ext == "json" || ext == "jsonc")
        return Language::Json;
    if (ext == "md" || ext == "markdown")
        return Language::Markdown;
    return Language::None;
}

// ---------------------------------------------------------------------------
// Colour lookup
// ---------------------------------------------------------------------------

Color SyntaxHighlighter::colour_for(TokenKind kind) const {
    switch (kind) {
    case TokenKind::Keyword:      return theme_.keyword;
    case TokenKind::Type:         return theme_.type_kw;
    case TokenKind::Builtin:      return theme_.builtin;
    case TokenKind::Literal:      return theme_.literal;
    case TokenKind::Comment:      return theme_.comment;
    case TokenKind::Preprocessor: return theme_.preprocessor;
    case TokenKind::Operator:     return theme_.op;
    case TokenKind::Punctuation:  return theme_.punctuation;
    case TokenKind::Identifier:   return theme_.identifier;
    default:                      return theme_.normal;
    }
}

// ---------------------------------------------------------------------------
// Top-level dispatch
// ---------------------------------------------------------------------------

LineTokens SyntaxHighlighter::tokenise(std::string_view line,
                                        bool& in_block_comment) const {
    switch (lang_) {
    case Language::Cpp:      return tokenise_cpp(line, in_block_comment);
    case Language::Python:   return tokenise_python(line, in_block_comment);
    case Language::Rust:     return tokenise_rust(line, in_block_comment);
    case Language::Json:     return tokenise_json(line, in_block_comment);
    case Language::Markdown: return tokenise_markdown(line, in_block_comment);
    default: {
        in_block_comment = false;
        LineTokens toks;
        if (!line.empty()) {
            toks.push_back({0, static_cast<int>(line.size()), TokenKind::Normal});
        }
        return toks;
    }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

static bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

static std::string_view read_word(std::string_view line, int start) {
    int end = start;
    while (end < static_cast<int>(line.size()) && is_ident_char(line[static_cast<size_t>(end)])) {
        ++end;
    }
    return line.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
}

// Emit a span for an entire remaining line starting at `pos`.
static void emit_rest(LineTokens& out, std::string_view line, int pos, TokenKind kind) {
    if (pos < static_cast<int>(line.size())) {
        out.push_back({pos, static_cast<int>(line.size()), kind});
    }
}

// Scan past a string literal starting at pos (pos is the opening quote char).
// Returns position after the closing quote (or end-of-line if unclosed).
static int scan_string(std::string_view line, int pos, char delim) {
    ++pos; // skip opening quote
    const int sz = static_cast<int>(line.size());
    while (pos < sz) {
        if (line[static_cast<size_t>(pos)] == '\\') {
            pos += 2;
            continue;
        }
        if (line[static_cast<size_t>(pos)] == delim) {
            return pos + 1;
        }
        ++pos;
    }
    return pos;
}

// ---------------------------------------------------------------------------
// C++ tokeniser
// ---------------------------------------------------------------------------

bool SyntaxHighlighter::is_cpp_keyword(std::string_view word) {
    return kCppKeywords.count(word) > 0;
}
bool SyntaxHighlighter::is_cpp_type(std::string_view word) {
    return kCppTypes.count(word) > 0;
}

LineTokens SyntaxHighlighter::tokenise_cpp(std::string_view line,
                                             bool& in_block) const {
    LineTokens out;
    const int sz = static_cast<int>(line.size());
    int i = 0;

    // If inside a block comment from a previous line
    if (in_block) {
        while (i < sz) {
            if (i + 1 < sz && line[static_cast<size_t>(i)] == '*' &&
                line[static_cast<size_t>(i + 1)] == '/') {
                i += 2;
                in_block = false;
                out.push_back({0, i, TokenKind::Comment});
                break;
            }
            ++i;
        }
        if (in_block) {
            emit_rest(out, line, 0, TokenKind::Comment);
            return out;
        }
    }

    while (i < sz) {
        const char c = line[static_cast<size_t>(i)];

        // Line comment
        if (c == '/' && i + 1 < sz && line[static_cast<size_t>(i + 1)] == '/') {
            emit_rest(out, line, i, TokenKind::Comment);
            return out;
        }

        // Block comment open
        if (c == '/' && i + 1 < sz && line[static_cast<size_t>(i + 1)] == '*') {
            const int start = i;
            i += 2;
            while (i < sz) {
                if (i + 1 < sz && line[static_cast<size_t>(i)] == '*' &&
                    line[static_cast<size_t>(i + 1)] == '/') {
                    i += 2;
                    in_block = false;
                    break;
                }
                ++i;
            }
            if (i >= sz) in_block = true;
            out.push_back({start, i, TokenKind::Comment});
            continue;
        }

        // Preprocessor directive
        if (c == '#') {
            emit_rest(out, line, i, TokenKind::Preprocessor);
            return out;
        }

        // String literal
        if (c == '"' || c == '\'') {
            const int start = i;
            i = scan_string(line, i, c);
            out.push_back({start, i, TokenKind::Literal});
            continue;
        }

        // Number literal
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < sz &&
             std::isdigit(static_cast<unsigned char>(line[static_cast<size_t>(i + 1)])))) {
            const int start = i;
            while (i < sz && (std::isalnum(static_cast<unsigned char>(line[static_cast<size_t>(i)])) ||
                               line[static_cast<size_t>(i)] == '.' || line[static_cast<size_t>(i)] == '_')) {
                ++i;
            }
            out.push_back({start, i, TokenKind::Literal});
            continue;
        }

        // Identifier / keyword
        if (is_ident_start(c)) {
            const int start = i;
            const auto word = read_word(line, i);
            i += static_cast<int>(word.size());

            TokenKind kind = TokenKind::Identifier;
            if (is_cpp_keyword(word))  kind = TokenKind::Keyword;
            else if (is_cpp_type(word)) kind = TokenKind::Type;

            out.push_back({start, i, kind});
            continue;
        }

        // Operator characters
        static constexpr std::string_view ops = "+-*/%=<>!&|^~?:";
        if (ops.find(c) != std::string_view::npos) {
            out.push_back({i, i + 1, TokenKind::Operator});
            ++i;
            continue;
        }

        // Punctuation
        static constexpr std::string_view punct = "(){}[];,.@\\";
        if (punct.find(c) != std::string_view::npos) {
            out.push_back({i, i + 1, TokenKind::Punctuation});
            ++i;
            continue;
        }

        // Whitespace / other — emit as Normal
        const int start = i;
        while (i < sz && std::isspace(static_cast<unsigned char>(line[static_cast<size_t>(i)]))) {
            ++i;
        }
        if (i == start) ++i;
        out.push_back({start, i, TokenKind::Normal});
    }
    return out;
}

// ---------------------------------------------------------------------------
// Python tokeniser
// ---------------------------------------------------------------------------

bool SyntaxHighlighter::is_py_keyword(std::string_view word) {
    return kPyKeywords.count(word) > 0;
}
bool SyntaxHighlighter::is_py_builtin(std::string_view word) {
    return kPyBuiltins.count(word) > 0;
}

LineTokens SyntaxHighlighter::tokenise_python(std::string_view line,
                                               bool& in_block) const {
    LineTokens out;
    const int sz = static_cast<int>(line.size());
    int i = 0;

    // Python triple-quoted strings can span lines — treat as block comment.
    if (in_block) {
        while (i + 2 < sz) {
            if (line[static_cast<size_t>(i)] == '"' &&
                line[static_cast<size_t>(i+1)] == '"' &&
                line[static_cast<size_t>(i+2)] == '"') {
                i += 3;
                in_block = false;
                out.push_back({0, i, TokenKind::Comment});
                break;
            }
            ++i;
        }
        if (in_block) {
            emit_rest(out, line, 0, TokenKind::Comment);
            return out;
        }
    }

    while (i < sz) {
        const char c = line[static_cast<size_t>(i)];

        // Line comment
        if (c == '#') {
            emit_rest(out, line, i, TokenKind::Comment);
            return out;
        }

        // Triple-quoted string
        if (i + 2 < sz && c == '"' &&
            line[static_cast<size_t>(i+1)] == '"' &&
            line[static_cast<size_t>(i+2)] == '"') {
            const int start = i;
            i += 3;
            while (i + 2 < sz) {
                if (line[static_cast<size_t>(i)] == '"' &&
                    line[static_cast<size_t>(i+1)] == '"' &&
                    line[static_cast<size_t>(i+2)] == '"') {
                    i += 3;
                    in_block = false;
                    goto triple_closed;
                }
                ++i;
            }
            in_block = true;
            i = sz;
            triple_closed:
            out.push_back({start, i, TokenKind::Comment});
            continue;
        }

        // String / f-string / b-string / r-string
        if (c == '"' || c == '\'' ||
            ((c == 'f' || c == 'b' || c == 'r') && i + 1 < sz &&
             (line[static_cast<size_t>(i+1)] == '"' || line[static_cast<size_t>(i+1)] == '\''))) {
            int start = i;
            char delim = c;
            if (c != '"' && c != '\'') {
                ++i;
                delim = line[static_cast<size_t>(i)];
            }
            i = scan_string(line, i, delim);
            out.push_back({start, i, TokenKind::Literal});
            continue;
        }

        // Decorator
        if (c == '@') {
            const int start = i;
            while (i < sz && !std::isspace(static_cast<unsigned char>(line[static_cast<size_t>(i)]))) {
                ++i;
            }
            out.push_back({start, i, TokenKind::Preprocessor});
            continue;
        }

        // Number
        if (std::isdigit(static_cast<unsigned char>(c))) {
            const int start = i;
            while (i < sz && (std::isalnum(static_cast<unsigned char>(line[static_cast<size_t>(i)])) ||
                               line[static_cast<size_t>(i)] == '.' || line[static_cast<size_t>(i)] == '_')) {
                ++i;
            }
            out.push_back({start, i, TokenKind::Literal});
            continue;
        }

        // Identifier / keyword / builtin
        if (is_ident_start(c)) {
            const int start = i;
            const auto word = read_word(line, i);
            i += static_cast<int>(word.size());

            TokenKind kind = TokenKind::Identifier;
            if (is_py_keyword(word))       kind = TokenKind::Keyword;
            else if (is_py_builtin(word))  kind = TokenKind::Builtin;

            out.push_back({start, i, kind});
            continue;
        }

        static constexpr std::string_view ops = "+-*/%=<>!&|^~";
        if (ops.find(c) != std::string_view::npos) {
            out.push_back({i, i + 1, TokenKind::Operator});
            ++i;
            continue;
        }

        out.push_back({i, i + 1, TokenKind::Normal});
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Rust tokeniser
// ---------------------------------------------------------------------------

bool SyntaxHighlighter::is_rust_keyword(std::string_view word) {
    return kRustKeywords.count(word) > 0;
}
bool SyntaxHighlighter::is_rust_type(std::string_view word) {
    return kRustTypes.count(word) > 0;
}

LineTokens SyntaxHighlighter::tokenise_rust(std::string_view line,
                                              bool& in_block) const {
    LineTokens out;
    const int sz = static_cast<int>(line.size());
    int i = 0;

    if (in_block) {
        while (i < sz) {
            if (i + 1 < sz && line[static_cast<size_t>(i)] == '*' &&
                line[static_cast<size_t>(i+1)] == '/') {
                i += 2;
                in_block = false;
                out.push_back({0, i, TokenKind::Comment});
                break;
            }
            ++i;
        }
        if (in_block) { emit_rest(out, line, 0, TokenKind::Comment); return out; }
    }

    while (i < sz) {
        const char c = line[static_cast<size_t>(i)];

        // Line comment or doc comment
        if (c == '/' && i + 1 < sz && line[static_cast<size_t>(i+1)] == '/') {
            emit_rest(out, line, i, TokenKind::Comment);
            return out;
        }

        // Block comment
        if (c == '/' && i + 1 < sz && line[static_cast<size_t>(i+1)] == '*') {
            const int start = i; i += 2;
            while (i < sz) {
                if (i + 1 < sz && line[static_cast<size_t>(i)] == '*' &&
                    line[static_cast<size_t>(i+1)] == '/') { i += 2; in_block = false; break; }
                ++i;
            }
            if (i >= sz) in_block = true;
            out.push_back({start, i, TokenKind::Comment});
            continue;
        }

        // Attribute #[…]
        if (c == '#') {
            emit_rest(out, line, i, TokenKind::Preprocessor);
            return out;
        }

        // Lifetime 'a
        if (c == '\'' && i + 1 < sz && is_ident_start(line[static_cast<size_t>(i+1)])) {
            const int start = i;
            i += 2;
            while (i < sz && is_ident_char(line[static_cast<size_t>(i)])) ++i;
            out.push_back({start, i, TokenKind::Type});
            continue;
        }

        // String / byte-string
        if (c == '"' || (c == 'b' && i + 1 < sz && line[static_cast<size_t>(i+1)] == '"')) {
            const int start = i;
            if (c == 'b') ++i;
            i = scan_string(line, i, '"');
            out.push_back({start, i, TokenKind::Literal});
            continue;
        }

        // Number
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '-' && i + 1 < sz &&
            std::isdigit(static_cast<unsigned char>(line[static_cast<size_t>(i+1)])))) {
            const int start = i; if (c == '-') ++i;
            while (i < sz && (std::isalnum(static_cast<unsigned char>(line[static_cast<size_t>(i)])) ||
                               line[static_cast<size_t>(i)] == '.' || line[static_cast<size_t>(i)] == '_')) {
                ++i;
            }
            out.push_back({start, i, TokenKind::Literal});
            continue;
        }

        // Identifier / keyword
        if (is_ident_start(c)) {
            const int start = i;
            const auto word = read_word(line, i);
            i += static_cast<int>(word.size());
            TokenKind kind = TokenKind::Identifier;
            if (is_rust_keyword(word))     kind = TokenKind::Keyword;
            else if (is_rust_type(word))   kind = TokenKind::Type;
            out.push_back({start, i, kind});
            continue;
        }

        static constexpr std::string_view ops = "+-*/%=<>!&|^~?";
        if (ops.find(c) != std::string_view::npos) {
            out.push_back({i, i+1, TokenKind::Operator}); ++i; continue;
        }

        out.push_back({i, i+1, TokenKind::Normal}); ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// JSON tokeniser
// ---------------------------------------------------------------------------

LineTokens SyntaxHighlighter::tokenise_json(std::string_view line,
                                              bool& in_block) const {
    in_block = false;
    LineTokens out;
    const int sz = static_cast<int>(line.size());
    int i = 0;

    while (i < sz) {
        const char c = line[static_cast<size_t>(i)];

        if (c == '"') {
            const int start = i;
            i = scan_string(line, i, '"');
            // Determine if it looks like a JSON key (followed by ':')
            int j = i;
            while (j < sz && std::isspace(static_cast<unsigned char>(line[static_cast<size_t>(j)]))) ++j;
            if (j < sz && line[static_cast<size_t>(j)] == ':') {
                out.push_back({start, i, TokenKind::Keyword});
            } else {
                out.push_back({start, i, TokenKind::Literal});
            }
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-') {
            const int start = i;
            if (c == '-') ++i;
            while (i < sz && (std::isdigit(static_cast<unsigned char>(line[static_cast<size_t>(i)])) ||
                               line[static_cast<size_t>(i)] == '.' || line[static_cast<size_t>(i)] == 'e' ||
                               line[static_cast<size_t>(i)] == 'E' || line[static_cast<size_t>(i)] == '+' ||
                               line[static_cast<size_t>(i)] == '-')) {
                ++i;
            }
            out.push_back({start, i, TokenKind::Literal});
            continue;
        }

        if (is_ident_start(c)) {
            const int start = i;
            const auto word = read_word(line, i);
            i += static_cast<int>(word.size());
            if (word == "true" || word == "false" || word == "null") {
                out.push_back({start, i, TokenKind::Builtin});
            } else {
                out.push_back({start, i, TokenKind::Normal});
            }
            continue;
        }

        static constexpr std::string_view punct = "{}[]:,";
        if (punct.find(c) != std::string_view::npos) {
            out.push_back({i, i+1, TokenKind::Punctuation}); ++i; continue;
        }

        out.push_back({i, i+1, TokenKind::Normal}); ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Markdown tokeniser
// ---------------------------------------------------------------------------

LineTokens SyntaxHighlighter::tokenise_markdown(std::string_view line,
                                                  bool& in_block) const {
    // Triple-backtick code fences
    LineTokens out;
    const int sz = static_cast<int>(line.size());

    if (sz >= 3 && line[0] == '`' && line[1] == '`' && line[2] == '`') {
        in_block = !in_block;
        out.push_back({0, sz, TokenKind::Comment});
        return out;
    }

    if (in_block) {
        out.push_back({0, sz, TokenKind::Comment});
        return out;
    }

    int i = 0;

    // ATX headings
    if (i < sz && line[static_cast<size_t>(i)] == '#') {
        int level = 0;
        while (i < sz && line[static_cast<size_t>(i)] == '#') { ++level; ++i; }
        (void)level;
        out.push_back({0, sz, TokenKind::Keyword});
        return out;
    }

    // Horizontal rule / list marker
    if (sz >= 3 && (line[0] == '-' || line[0] == '*' || line[0] == '_') &&
        line[0] == line[1] && line[1] == line[2]) {
        out.push_back({0, sz, TokenKind::Operator});
        return out;
    }

    while (i < sz) {
        const char c = line[static_cast<size_t>(i)];

        // Inline code `…`
        if (c == '`') {
            const int start = i;
            ++i;
            while (i < sz && line[static_cast<size_t>(i)] != '`') ++i;
            if (i < sz) ++i;
            out.push_back({start, i, TokenKind::Comment});
            continue;
        }

        // Bold **text**
        if (c == '*' && i + 1 < sz && line[static_cast<size_t>(i+1)] == '*') {
            const int start = i; i += 2;
            while (i + 1 < sz && !(line[static_cast<size_t>(i)] == '*' &&
                                    line[static_cast<size_t>(i+1)] == '*')) ++i;
            if (i + 1 < sz) i += 2;
            out.push_back({start, i, TokenKind::Type});
            continue;
        }

        // Italic *text*
        if (c == '*') {
            const int start = i; ++i;
            while (i < sz && line[static_cast<size_t>(i)] != '*') ++i;
            if (i < sz) ++i;
            out.push_back({start, i, TokenKind::Builtin});
            continue;
        }

        // Link [text](url)
        if (c == '[') {
            const int start = i;
            while (i < sz && line[static_cast<size_t>(i)] != ']') ++i;
            if (i < sz) ++i;
            out.push_back({start, i, TokenKind::Literal});
            if (i < sz && line[static_cast<size_t>(i)] == '(') {
                const int url_start = i;
                while (i < sz && line[static_cast<size_t>(i)] != ')') ++i;
                if (i < sz) ++i;
                out.push_back({url_start, i, TokenKind::Comment});
            }
            continue;
        }

        out.push_back({i, i+1, TokenKind::Normal}); ++i;
    }
    return out;
}

} // namespace straylight::editor
