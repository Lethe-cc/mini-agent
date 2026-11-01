#pragma once

#include <string_view>

namespace starter {
// The returned view refers to static storage and remains valid for the process lifetime.
[[nodiscard]] std::string_view project_name() noexcept;
} // namespace starter
