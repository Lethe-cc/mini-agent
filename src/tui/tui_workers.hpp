#pragma once
// Internal TUI background workers (design 019 P4): models discovery, context
// probe, and /compact summary. Pure result computation given a config/chat
// snapshot; screen/logger stay on the main thread (join-then-read discipline).
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "mini_agent/chat.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/provider.hpp"

namespace mini_agent::tui {

struct ModelsResult {
    bool ok = false;
    std::vector<std::string> ids;
    std::vector<ModelCapabilities> caps;
    std::string error;
    int http_status = 0;
    bool network_error = false;
    struct Part {
        std::string name;
        bool ok = false;
        std::vector<std::string> ids;
        std::vector<ModelCapabilities> caps;
        std::string error;
        int http_status = 0;
        bool network_error = false;
    };
    std::vector<Part> parts;
};
struct ProbeResult {
    ProbeOutcome outcome;
    std::string error;
};
struct CompactResult {
    std::string summary;
    std::string error;
    bool aborted = false;
    std::vector<Message> tail;
    std::size_t pruned = 0;
    bool nudged = false;
};
using ModelTargets = std::vector<std::pair<std::string, std::pair<std::string, std::string>>>;

ModelsResult fetch_models(const AppConfig& snapshot, const ModelTargets& targets);
ProbeResult probe_window(const AppConfig& snapshot);
CompactResult compact_history(const AppConfig& snapshot, const Chat& history,
                              const std::function<bool()>& abort);
} // namespace mini_agent::tui
