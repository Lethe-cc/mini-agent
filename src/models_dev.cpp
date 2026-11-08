#include "mini_agent/models_dev.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "mini_agent/models_dev_data.hpp"

namespace mini_agent {
namespace {

// One catalog row, pointing into the embedded TSV literal (static storage).
struct Row {
    std::string_view line; // "base\tmodel\tcontext\toutput"
};

std::vector<Row> parse_rows() {
    std::vector<Row> out;
    std::string_view tsv(kModelsDevCatalogTsv);
    std::size_t pos = 0;
    while (pos < tsv.size()) {
        const auto nl = tsv.find('\n', pos);
        const auto end = nl == std::string_view::npos ? tsv.size() : nl;
        if (end > pos)
            out.push_back(Row{tsv.substr(pos, end - pos)});
        if (nl == std::string_view::npos)
            break;
        pos = nl + 1;
    }
    return out;
}

// Rows are generated sorted by the whole line, so "base\tmodel\t" is a
// lexicographically unique prefix: binary search for it, then verify the
// exact key before reading the numbers.
const std::vector<Row>& table() {
    static const std::vector<Row> cached = parse_rows();
    return cached;
}

std::string_view field(std::string_view line, int index) {
    std::size_t from = 0;
    for (int i = 0; i < index; ++i) {
        const auto tab = line.find('\t', from);
        if (tab == std::string_view::npos)
            return {};
        from = tab + 1;
    }
    const auto end = line.find_first_of("\t\n", from);
    return line.substr(from, end == std::string_view::npos ? std::string_view::npos : end - from);
}

long long parse_number(std::string_view text) {
    return text.empty() ? 0 : std::strtoll(std::string(text).c_str(), nullptr, 10);
}

std::string trim_trailing_slash(std::string_view url) {
    while (!url.empty() && url.back() == '/')
        url.remove_suffix(1);
    return std::string(url);
}

} // namespace

std::optional<ModelsDevLimits> lookup_models_dev(std::string_view base_url,
                                                 std::string_view model) {
    if (base_url.empty() || model.empty())
        return std::nullopt;
    const std::string key = trim_trailing_slash(base_url) + '\t' + std::string(model) + '\t';
    const auto& rows = table();
    const auto it = std::lower_bound(rows.begin(), rows.end(), key,
                                     [](const Row& row, const std::string& probe) {
                                         return row.line.compare(0, probe.size(), probe) < 0;
                                     });
    if (it == rows.end() || it->line.compare(0, key.size(), key) != 0)
        return std::nullopt;
    ModelsDevLimits limits;
    limits.context = parse_number(field(it->line, 2));
    limits.output = parse_number(field(it->line, 3));
    if (limits.context <= 0)
        return std::nullopt;
    return limits;
}

} // namespace mini_agent
