#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace mini_agent {

// v4.7 (design 010 §18): models.dev catalog lookup, the pi-style static
// source for the context window. The table is a generated snapshot
// (tools/refresh_models_dev.py, MIT data); matching is exact on both the
// provider base_url (trailing '/' trimmed) and the model id, so an unknown
// gateway or model simply misses. Misses stay misses: the caller must not
// fall back to a guessed window.
struct ModelsDevLimits {
    long long context = 0;
    long long output = 0; // 0 = catalog unknown
};

// Returns the catalog row for (base_url, model), or nullopt on miss.
std::optional<ModelsDevLimits> lookup_models_dev(std::string_view base_url, std::string_view model);

// Same matching over a caller-supplied sorted TSV (parses per call, no
// cache): the test seam that keeps catalog assertions independent of the
// embedded snapshot, which CMake may refresh from live models.dev data
// (design 010 §21). Rows must be sorted by whole line, as the generator
// emits them.
std::optional<ModelsDevLimits>
lookup_models_dev_tsv(std::string_view tsv, std::string_view base_url, std::string_view model);

} // namespace mini_agent
