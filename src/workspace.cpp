#include "mini_agent/tools.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace mini_agent {
bool resolve_within_workspace(const std::filesystem::path& root, std::string_view raw,
                              std::filesystem::path* resolved, std::string* error) {
    if (raw.empty()) {
        if (error)
            *error = "empty path";
        return false;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    auto root_canonical = fs::weakly_canonical(root, ec);
    if (ec) {
        if (error)
            *error = "workspace root unresolvable: " + ec.message();
        return false;
    }
    const fs::path raw_path(raw);
    auto candidate = raw_path.is_absolute() ? raw_path : (root_canonical / raw_path);
    // Lexical normalization first (handles .. against non-existent tails),
    // then canonical where possible: weakly_canonical resolves existing
    // symlinks/components and keeps the not-yet-existing tail.
    candidate = fs::weakly_canonical(candidate, ec);
    if (ec) {
        // Path components with wrong types (e.g. file used as dir).
        if (error)
            *error = "path unresolvable: " + ec.message();
        return false;
    }
    const auto* root_str = root_canonical.native().c_str();
    const auto* cand_str = candidate.native().c_str();
    const auto root_len = root_canonical.native().size();
    (void)root_str;
    if (candidate.native().size() >= root_len &&
        std::char_traits<char>::compare(cand_str, root_str, root_len) == 0) {
        // Boundary: equal to root or a real child (next char is '/').
        if (candidate.native().size() == root_len || candidate.native()[root_len] == '/') {
            // Symlink escape recheck for existing paths: read the final
            // component; a symlink's target must also live inside root.
            if (fs::is_symlink(candidate, ec)) {
                auto target =
                    fs::canonical(candidate.parent_path() / fs::read_symlink(candidate, ec), ec);
                if (!ec) {
                    const auto* t_str = target.native().c_str();
                    const auto t_len = target.native().size();
                    const bool inside =
                        t_len >= root_len &&
                        std::char_traits<char>::compare(t_str, root_str, root_len) == 0 &&
                        (t_len == root_len || target.native()[root_len] == '/');
                    if (!inside) {
                        if (error)
                            *error = "symlink escapes workspace";
                        return false;
                    }
                }
            }
            if (resolved)
                *resolved = std::move(candidate);
            return true;
        }
    }
    if (error)
        *error = "path escapes workspace";
    return false;
}
} // namespace mini_agent
