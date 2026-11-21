#!/usr/bin/env python3
"""One-shot P4 migration tool (019 §8): scope-aware code-segment rewriting.

Walks the source char by char, tracking string / char / line / block comment
/ raw-string state; regexes are applied ONLY to code segments, never to
literals or comments. Control-flow statements (continue / goto done / break)
are NEVER rewritten — those are translated manually afterwards per the
019 §8 hazard note. Used for the tui.cpp -> tui_keys.hpp key-dispatch move.
"""
import re
import sys


def code_segments(src):
    """Yield (start, end, is_code) covering src; raw strings handled."""
    i, n = 0, len(src)
    seg_start = 0
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            yield seg_start, i, True
            j = src.find('\n', i)
            j = n if j < 0 else j
            yield i, j, False
            i = seg_start = j
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            yield seg_start, i, True
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            yield i, j, False
            i = seg_start = j
        elif c == 'R' and i + 1 < n and src[i + 1] == '"':
            m = re.match(r'R"([^(]*)\(', src[i:])
            if m:
                yield seg_start, i, True
                delim = ')' + m.group(1) + '"'
                j = src.find(delim, i + m.end())
                j = n if j < 0 else j + len(delim)
                yield i, j, False
                i = seg_start = j
                continue
            i += 1
        elif c in '"\'':
            yield seg_start, i, True
            q, j = c, i + 1
            while j < n:
                if src[j] == '\\':
                    j += 2
                    continue
                if src[j] == q:
                    j += 1
                    break
                j += 1
            yield i, j, False
            i = seg_start = j
        else:
            i += 1
    yield seg_start, n, True


def rewrite(src, patterns):
    out = []
    for a, b, is_code in code_segments(src):
        part = src[a:b]
        if is_code:
            for pat, repl in patterns:
                part = re.sub(pat, repl, part)
        out.append(part)
    return ''.join(out)


def build_patterns(names):
    # longest first so e.g. hist_pos never gets clipped by a shorter name
    names = sorted(names, key=len, reverse=True)
    pats = []
    for name in names:
        # skip member access (x.foo / x->foo) and declarations; only bare uses
        pats.append((r'(?<![\w.>])\b' + re.escape(name) + r'\b(?!\s*&\s*\w+\s*[;=])',
                     'S.' + name))
    return pats


# handle_slash/hist_up/hist_down/mark_edited/insert_sidebar_selection stay
# local lambdas in the moved body (they forward to *_impl(S)), not ctx members.
NAMES = """abort_requested active_profile_for_edit answer_approval approval_pending
auth auth_probes close_preview_pane compact_running composer config cursor
enter_sidebar_dir hist_pos history history_draft
last_ctrl_c logger maybe_autofetch_models
model_choice_targets model_choices model_menu_open open_preview_pane pending
preview_pane provider_name_list reasoning_choices reasoning_menu_open save_and_report
screen secret_field secret_input select_model session_ids show_sidebar sidebar_focus
sidebar_result sidebar_sel start_turn streaming tab_state""".split()


def main():
    tui = open('src/tui/tui.cpp').read().split('\n')
    # extract byte loop: tui.cpp lines 868..1273 (1-based, inclusive)
    assert tui[867].strip().startswith('for (ssize_t i = 0'), tui[867]
    assert tui[1272].rstrip() == '            }', repr(tui[1272])
    block = '\n'.join(tui[867:1273])
    moved = rewrite(block, build_patterns(NAMES))
    header = open('src/tui/tui_keys.hpp').read()
    assert header.endswith('insert_sidebar_selection_impl(S); };\n')
    with open('src/tui/tui_keys.hpp', 'w') as f:
        f.write(header + moved + '\n    return LoopCtl::Render;\n}\n\n} // namespace mini_agent::tui\n')
    # replace the block in tui.cpp with the dispatch call
    call = """            if (dispatch_input_impl(slash_ctx, buf, n) == LoopCtl::Exit) {
                goto done;
            }"""
    tui[867:1273] = call.split('\n')
    open('src/tui/tui.cpp', 'w').write('\n'.join(tui))
    print('moved', len(block.split(chr(10))), 'lines; tui.cpp now', len(tui), 'lines')


if __name__ == '__main__':
    sys.exit(main())
