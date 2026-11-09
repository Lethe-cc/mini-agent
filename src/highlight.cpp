#include "mini_agent/highlight.hpp"

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace mini_agent::hl {
namespace {

// SGR classes (basic 16 palette only, same posture as md.cpp): comment
// dim, string green, number magenta, keyword cyan, preprocessor/variable
// yellow, function-call name blue.
constexpr const char* kCmt = "\x1b[2m";
constexpr const char* kStr = "\x1b[32m";
constexpr const char* kNum = "\x1b[35m";
constexpr const char* kKw = "\x1b[36m";
constexpr const char* kPre = "\x1b[33m";
constexpr const char* kFn = "\x1b[34m";
constexpr const char* kRst = "\x1b[0m";

enum class Fam { None, C, Py, Sh, Json };

struct LangInfo {
    Fam fam = Fam::None;
    const char* keywords = ""; // space-separated; exact word match
};

// Keyword tables: language CORE words only -- over-marking common names
// reads worse than under-marking (vim default-filetype behaviour).
const LangInfo& lang_info(std::string_view lang) {
    static const std::array<std::pair<std::string_view, LangInfo>, 6> table = {{
        {"cpp",
         {Fam::C, "alignas alignof and asm auto bool break case catch char "
                  "class const constexpr continue decltype default delete do "
                  "double dynamic_cast else enum explicit export extern false "
                  "float for friend goto if inline int long namespace new "
                  "noexcept nullptr operator override private protected public "
                  "register reinterpret_cast return short signed sizeof static "
                  "static_assert static_cast struct switch template this throw "
                  "true try typedef typename union unsigned using virtual void "
                  "volatile while"}},
        {"js",
         {Fam::C, "async await break case catch class const continue debugger "
                  "default delete do else export extends finally for function "
                  "if import in instanceof let new return super switch this "
                  "throw try typeof var void while with"}},
        {"py",
         {Fam::Py, "and as assert async await break class continue def del elif "
                   "else except finally for from global if import in is lambda "
                   "nonlocal not or pass raise return try while with yield True "
                   "False None self"}},
        {"sh",
         {Fam::Sh, "if then else elif fi for while until do done case esac "
                   "function in select time coproc return break continue local "
                   "export readonly unset shift set eval exec source alias trap"}},
        {"json", {Fam::Json, "true false null"}},
        {"go",
         {Fam::C, "break case chan const continue default defer else fallthrough "
                  "for func go goto if import interface map package range return "
                  "select struct switch type var nil true false"}},
    }};
    static const LangInfo none{};
    for (const auto& entry : table) {
        if (entry.first == lang)
            return entry.second;
    }
    return none;
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_word(std::string_view w, std::string_view list) {
    // Space-separated exact-word lookup without allocations per miss.
    std::size_t pos = 0;
    while (pos < list.size()) {
        const auto end = list.find(' ', pos);
        const auto tok =
            list.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        if (tok == w)
            return true;
        if (end == std::string_view::npos)
            break;
        pos = end + 1;
    }
    return false;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void emit(std::string& out, const char* sgr, std::string_view text) {
    if (text.empty())
        return;
    out += sgr;
    out.append(text);
    out += kRst;
}

// Consecutive spans of one class share one SGR pair: `set` keeps the run
// open, `plain` closes it, `flush` ends the line.
struct Run {
    std::string& out;
    const char* cur = "";
    void set(const char* sgr, std::string_view text) {
        if (text.empty())
            return;
        if (sgr != cur) {
            if (cur != nullptr && cur[0] != '\0')
                out += kRst;
            out += sgr;
            cur = sgr;
        }
        out.append(text);
    }
    void plain(std::string_view text) {
        if (text.empty())
            return;
        if (cur != nullptr && cur[0] != '\0') {
            out += kRst;
            cur = "";
        }
        out.append(text);
    }
    void flush() {
        if (cur != nullptr && cur[0] != '\0') {
            out += kRst;
            cur = "";
        }
    }
};

// Word run shared by the C/Py/Sh families: keyword / call / plain.
void highlight_word(std::string_view sv, std::size_t& i, Run& run, const LangInfo& info) {
    const auto n = sv.size();
    std::size_t j = i;
    while (j < n && is_ident_char(sv[j]))
        ++j;
    const auto word = sv.substr(i, j - i);
    std::size_t k = j;
    while (k < n && sv[k] == ' ')
        ++k;
    if (is_word(word, info.keywords))
        run.set(kKw, word);
    else if (k < n && sv[k] == '(')
        run.set(kFn, word);
    else
        run.plain(word);
    i = j;
}

void highlight_c(std::string_view sv, std::size_t& i, Run& run, State& st, const LangInfo& info) {
    const auto n = sv.size();
    while (i < n) {
        const char c = sv[i];
        if (st.open == 1) { // inside /* */
            const auto close = sv.find("*/", i);
            if (close == std::string_view::npos) {
                run.set(kCmt, sv.substr(i));
                i = n;
            } else {
                run.set(kCmt, sv.substr(i, close + 2 - i));
                i = close + 2;
                st.open = 0;
            }
            continue;
        }
        if (c == '/' && i + 1 < n && sv[i + 1] == '/') {
            run.set(kCmt, sv.substr(i));
            i = n;
            continue;
        }
        if (c == '/' && i + 1 < n && sv[i + 1] == '*') {
            run.set(kCmt, "/*");
            i += 2;
            st.open = 1;
            continue;
        }
        if (c == '"' || c == '\'') {
            const char q = c;
            std::size_t j = i + 1;
            std::size_t end = n;
            bool terminated = false;
            while (j < n) {
                if (sv[j] == '\\')
                    j += 2;
                else if (sv[j] == q) {
                    end = j + 1;
                    terminated = true;
                    break;
                } else
                    ++j;
            }
            run.set(kStr, sv.substr(i, terminated ? end - i : n - i));
            i = terminated ? end : n;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
            (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(sv[i + 1])) != 0)) {
            std::size_t j = i;
            // Hex prefix (0x) then alnum/dots (suffixes like 123u, 1.5f).
            if (j + 1 < n && sv[j] == '0' && (sv[j + 1] == 'x' || sv[j + 1] == 'X'))
                j += 2;
            while (j < n && (std::isalnum(static_cast<unsigned char>(sv[j])) != 0 || sv[j] == '.'))
                ++j;
            run.set(kNum, sv.substr(i, j - i));
            i = j;
            continue;
        }
        if (is_ident_char(c) && (i == 0 || !is_ident_char(sv[i - 1]))) {
            highlight_word(sv, i, run, info);
            continue;
        }
        run.plain(sv.substr(i, 1));
        ++i;
    }
}

void highlight_py(std::string_view sv, std::size_t& i, Run& run, State& st, const LangInfo& info) {
    const auto n = sv.size();
    while (i < n) {
        const char c = sv[i];
        if (st.open == 2) { // inside a triple-quoted string
            const std::string close(3, st.quote != '\0' ? st.quote : '"');
            const auto found = sv.find(close, i);
            if (found == std::string_view::npos) {
                run.set(kStr, sv.substr(i));
                i = n;
            } else {
                run.set(kStr, sv.substr(i, found + close.size() - i));
                i = found + close.size();
                st.open = 0;
                st.quote = 0;
            }
            continue;
        }
        if (c == '#') {
            run.set(kCmt, sv.substr(i));
            i = n;
            continue;
        }
        if (c == '@' && i + 1 < n && is_ident_char(sv[i + 1]) &&
            (i == 0 || sv[i - 1] == ' ' || sv[i - 1] == '\t')) {
            std::size_t j = i + 1;
            while (j < n && (is_ident_char(sv[j]) || sv[j] == '.'))
                ++j;
            run.set(kPre, sv.substr(i, j - i));
            i = j;
            continue;
        }
        if (c == '"' || c == '\'') {
            const char q = c;
            if (i + 2 < n && sv[i + 1] == q && sv[i + 2] == q) {
                const std::string triple(3, q);
                const auto found = sv.find(triple, i + 3);
                if (found == std::string_view::npos) {
                    run.set(kStr, sv.substr(i));
                    st.open = 2;
                    st.quote = q;
                    i = n;
                } else {
                    run.set(kStr, sv.substr(i, found + triple.size() - i));
                    i = found + triple.size();
                }
                continue;
            }
            std::size_t j = i + 1;
            std::size_t end = n;
            bool terminated = false;
            while (j < n) {
                if (sv[j] == '\\')
                    j += 2;
                else if (sv[j] == q) {
                    end = j + 1;
                    terminated = true;
                    break;
                } else
                    ++j;
            }
            run.set(kStr, sv.substr(i, terminated ? end - i : n - i));
            i = terminated ? end : n;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            std::size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(sv[j])) != 0 || sv[j] == '.'))
                ++j;
            run.set(kNum, sv.substr(i, j - i));
            i = j;
            continue;
        }
        if (is_ident_char(c) && (i == 0 || !is_ident_char(sv[i - 1]))) {
            highlight_word(sv, i, run, info);
            continue;
        }
        run.plain(sv.substr(i, 1));
        ++i;
    }
}

void highlight_sh(std::string_view sv, std::size_t& i, Run& run, State& st, const LangInfo& info) {
    const auto n = sv.size();
    (void)st; // shell strings/comments never carry across lines here
    while (i < n) {
        const char c = sv[i];
        if (c == '#') {
            run.set(kCmt, sv.substr(i));
            i = n;
            continue;
        }
        if (c == '$') {
            std::size_t j = i + 1;
            if (j < n && sv[j] == '{') {
                const auto close = sv.find('}', j);
                j = close == std::string_view::npos ? n : close + 1;
            } else if (j < n && sv[j] == '(') {
                const auto close = sv.find(')', j);
                j = close == std::string_view::npos ? n : close + 1;
            } else {
                while (j < n && (is_ident_char(sv[j]) || sv[j] == '@' || sv[j] == '?'))
                    ++j;
            }
            if (j > i + 1) { // '$' alone stays plain
                run.set(kPre, sv.substr(i, j - i));
                i = j;
                continue;
            }
        }
        if (c == '\'') {
            const auto close = sv.find('\'', i + 1);
            const std::size_t end = close == std::string_view::npos ? n : close + 1;
            run.set(kStr, sv.substr(i, end - i));
            i = end;
            continue;
        }
        if (c == '"' || c == '`') {
            const char q = c;
            std::size_t j = i + 1;
            while (j < n) {
                if (sv[j] == '\\')
                    j += 2;
                else if (sv[j] == q)
                    break;
                else
                    ++j;
            }
            const std::size_t end = j < n ? j + 1 : n;
            run.set(q == '`' ? kPre : kStr, sv.substr(i, end - i));
            i = end;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            std::size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(sv[j])) != 0 || sv[j] == '.'))
                ++j;
            run.set(kNum, sv.substr(i, j - i));
            i = j;
            continue;
        }
        if (is_ident_char(c) && (i == 0 || !is_ident_char(sv[i - 1]))) {
            highlight_word(sv, i, run, info);
            continue;
        }
        run.plain(sv.substr(i, 1));
        ++i;
    }
}

void highlight_json(std::string_view sv, std::size_t& i, Run& run, State& st,
                    const LangInfo& info) {
    const auto n = sv.size();
    (void)st;
    while (i < n) {
        const char c = sv[i];
        if (c == '"') {
            std::size_t j = i + 1;
            while (j < n) {
                if (sv[j] == '\\')
                    j += 2;
                else if (sv[j] == '"')
                    break;
                else
                    ++j;
            }
            const std::size_t end = j < n ? j + 1 : n;
            const auto tok = sv.substr(i, end - i);
            // Key vs value: a ':' before the next ',' / '}' marks a key.
            std::size_t k = end;
            while (k < n && (sv[k] == ' ' || sv[k] == '\t'))
                ++k;
            const bool key = k < n && sv[k] == ':';
            run.set(key ? kFn : kStr, tok);
            i = end;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
            (c == '-' && i + 1 < n && std::isdigit(static_cast<unsigned char>(sv[i + 1])) != 0)) {
            std::size_t j = c == '-' ? i + 1 : i;
            while (j < n && (std::isdigit(static_cast<unsigned char>(sv[j])) != 0 || sv[j] == '.' ||
                             sv[j] == 'e' || sv[j] == 'E' || sv[j] == '-' || sv[j] == '+'))
                ++j;
            run.set(kNum, sv.substr(i, j - i));
            i = j;
            continue;
        }
        if (is_ident_char(c) && (i == 0 || !is_ident_char(sv[i - 1]))) {
            std::size_t j = i;
            while (j < n && is_ident_char(sv[j]))
                ++j;
            const auto word = sv.substr(i, j - i);
            if (is_word(word, info.keywords))
                run.set(kKw, word);
            else
                run.plain(word);
            i = j;
            continue;
        }
        run.plain(sv.substr(i, 1));
        ++i;
    }
}

} // namespace

std::string language_for_path(std::string_view path) {
    const auto sep = path.find_last_of('/');
    const auto base = path.substr(sep == std::string_view::npos ? 0 : sep + 1);
    const auto dot = base.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0)
        return {};
    const auto ext = lower(base.substr(dot + 1));
    if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "hpp" || ext == "hh" || ext == "h" ||
        ext == "c")
        return "cpp";
    if (ext == "py")
        return "py";
    if (ext == "sh" || ext == "bash" || ext == "zsh")
        return "sh";
    if (ext == "json")
        return "json";
    if (ext == "js" || ext == "ts" || ext == "jsx" || ext == "tsx" || ext == "mjs")
        return "js";
    if (ext == "go")
        return "go";
    return {};
}

std::string language_for_fence(std::string_view info) {
    // Fence info may carry flags ("cpp title=x"): the first word decides.
    const auto space = info.find_first_of(" \t");
    const auto word = lower(info.substr(0, space));
    if (word == "c" || word == "cpp" || word == "c++" || word == "cc" || word == "hpp")
        return "cpp";
    if (word == "py" || word == "python" ||
        (word.size() > 6 && word.substr(0, 6) == "python" &&
         std::isdigit(static_cast<unsigned char>(word[6]))))
        return "py"; // python3 / py3-style fence tags
    if (word == "sh" || word == "bash" || word == "shell" || word == "zsh" || word == "console")
        return "sh";
    if (word == "json" || word == "jsonc")
        return "json";
    if (word == "js" || word == "javascript" || word == "ts" || word == "typescript" ||
        word == "jsx" || word == "tsx")
        return "js";
    if (word == "go" || word == "golang")
        return "go";
    return {};
}

bool supports(std::string_view lang) {
    return lang_info(lang).fam != Fam::None;
}

std::string highlight_line(std::string_view line, std::string_view lang, State& state) {
    const auto& info = lang_info(lang);
    if (info.fam == Fam::None || line.empty())
        return std::string(line);
    std::string out;
    out.reserve(line.size() + 32);
    Run run{out};
    // Whole-line classes first: C preprocessor lines (leading '#').
    const auto first = line.find_first_not_of(" \t");
    if (info.fam == Fam::C && first != std::string_view::npos && line[first] == '#') {
        emit(out, kPre, line);
        return out;
    }
    std::size_t i = 0;
    switch (info.fam) {
    case Fam::C:
        highlight_c(line, i, run, state, info);
        break;
    case Fam::Py:
        highlight_py(line, i, run, state, info);
        break;
    case Fam::Sh:
        highlight_sh(line, i, run, state, info);
        break;
    case Fam::Json:
        highlight_json(line, i, run, state, info);
        break;
    case Fam::None:
        break;
    }
    run.flush();
    return out;
}

} // namespace mini_agent::hl
