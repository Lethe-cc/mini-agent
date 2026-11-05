// Property fuzz for src/diff.cpp (design 009 5.3): random small file pairs,
// every NON-truncated diff must replay OLD -> NEW hunk by hunk; @@ starts
// strictly increase; added/removed counts match row kinds.
//
// Not a CTest target (200k iterations ~ seconds); run manually after
// touching diff.cpp:
//   g++ -std=c++17 -O1 -fsanitize=address,undefined -Iinclude \
//       tools/diff_fuzz.cpp src/diff.cpp -o /tmp/diff_fuzz && /tmp/diff_fuzz
#include "mini_agent/diff.hpp"

#include <cassert>
#include <iostream>
#include <random>
#include <string>
using namespace mini_agent;

static std::string replay(const std::string& old_text, const DiffResult& r) {
    const auto old_lines = diff_split_lines(old_text);
    std::vector<std::string> out_lines;
    std::size_t oi = 0;
    for (const auto& row : r.rows) {
        if (row.kind == '@') {
            // "@@ -a,b +c,d @@": seek to a-1; gap lines copy through.
            const std::size_t a = std::stoul(row.text.substr(4, row.text.find(',', 4) - 4));
            assert(a >= 1 && a >= oi + 1);
            for (; oi + 1 < a; ++oi)
                out_lines.push_back(std::string(old_lines[oi]));
            oi = a - 1;
        } else if (row.kind == ' ') {
            assert(oi < old_lines.size() && old_lines[oi] == row.text);
            out_lines.push_back(row.text);
            ++oi;
        } else if (row.kind == '-') {
            assert(oi < old_lines.size() && old_lines[oi] == row.text);
            ++oi;
        } else if (row.kind == '+') {
            out_lines.push_back(row.text);
        }
    }
    while (oi < old_lines.size())
        out_lines.push_back(std::string(old_lines[oi++]));
    std::string joined;
    for (auto& l : out_lines) {
        joined += l;
        joined += '\n';
    }
    return joined;
}

int main() {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> len_d(0, 12);
    std::uniform_int_distribution<int> sym_d(0, 4);
    int checked = 0;
    for (int iter = 0; iter < 200000; ++iter) {
        std::string a, b;
        const int la = len_d(rng), lb = len_d(rng);
        for (int i = 0; i < la; ++i)
            a += char('A' + sym_d(rng)), a += '\n';
        for (int i = 0; i < lb; ++i)
            b += char('A' + sym_d(rng)), b += '\n';
        auto r = diff_texts(a, b);
        if (!r.changed || r.too_large)
            continue;
        std::size_t plus = 0, minus = 0;
        const bool truncated = r.note.find("truncated") != std::string::npos;
        for (const auto& row : r.rows) {
            plus += row.kind == '+';
            minus += row.kind == '-';
        }
        assert(plus == r.added && minus == r.removed);
        if (!truncated) {
            assert(diff_split_lines(replay(a, r)) == diff_split_lines(b));
            ++checked;
        }
    }
    std::cout << "diff fuzz ok, full-replay checks: " << checked << "\n";
}
