// src/gitcore/diff.cpp — the line diff of GitCore (spec §3.3).
//
// The core is the Myers O(ND) edit graph over lines, taken only when the
// trimmed core is small enough that its search is bounded: the prefix/suffix
// trim always runs first, as git's own diff-core does, and the cell budget
// that decides between the exact search and the patience fallback is the
// caller's. The one number stands for both of the search's sizes: the core
// it may afford, and the trace it may stand up while walking. When the
// fallback answers the result says so with approx:true.
//
// A line of a blob is the bytes between two newlines together with the
// newline that ends them: the last line of a text that has no final newline
// is a different line from the same words with one, exactly as git reads it,
// and diff_lines folds the flag into that one comparison so a change of the
// trailing newline alone is a hunk and not silence.
//
// The hunks are then grouped the git way — a gap of unchanged lines longer
// than twice the context is what splits — and the unified output carries
// git's header format, its one-line bare-count form, and its no-newline
// markers.

#include "diff.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

namespace ols
{
    namespace gitcore
    {

        namespace detail
        {

            // ---- the edit script --------------------------------------------------------

            // An edit-script operation over the two line arrays: a line common to both,
            // a line only in a, a line only in b. Each names the row and column it stands
            // at; a common line advances both, a line only in a advances the row, a line
            // only in b advances the column.
            struct Op
            {
                char kind; // 'k' common, 'd' only in a, 'a' only in b
                std::size_t a, b;
            };

            static void push_common(std::vector<Op> &ops, std::size_t a, std::size_t b)
            {
                ops.push_back(Op{'k', a, b});
            }

            // The line counts the search below works in. It walks its grid in int: the
            // diagonal offset, the row, the column, and the width of the working set all
            // stand in one. A blob of two billion newlines is a hostile thing to hand a
            // diff, and what passes what an int holds is the SUM of the two sides: at
            // 2^30 lines apiece max_d = n+m wraps negative, the width wraps with it,
            // the cast back to a size makes it enormous, and the allocation of the
            // working set answers with an exception — the one thing this API does not
            // do. The guard therefore states the span n+m, not the sides apart: a window
            // past the domain is refused here and the caller cuts it, saying approx.
            constexpr int kLineCountLimit = 1 << 30;

            // The widest grid the search's numbering holds: width = 2*(n+m)+1 stays
            // inside an int only while the span stays under kLineCountLimit.
            constexpr std::size_t kGridSpanLimit = static_cast<std::size_t>(kLineCountLimit) - 1u;

            // The span test stated as a name, so a case can state the domain without
            // handing two billion lines to a diff: this is the whole of what the search
            // refuses before it allocates anything.
            bool gridFitsSpan(std::size_t n, std::size_t m)
            {
                return n + m <= kGridSpanLimit;
            }

            // The Myers search over the window [a0,a1) x [b0,b1), appending operations at
            // the positions inside it. Refuses — false, having appended nothing — when
            // the window stands past what the search's own numbering holds, or when the
            // trace it would need passes the working set the caller's cell budget states
            // (the same number that decided the search was worth taking, so that one
            // truth governs both gates), so the caller can fall back.
            static bool myers_window(const std::vector<std::string> &a, const std::vector<std::string> &b,
                                     std::size_t a0, std::size_t a1, std::size_t b0, std::size_t b1,
                                     std::uint64_t budget, std::vector<Op> &ops)
            {
                const std::size_t nAll = a1 - a0, mAll = b1 - b0;
                if (!gridFitsSpan(nAll, mAll))
                {
                    return false;
                }
                if (nAll == 0)
                {
                    for (std::size_t j = 0; j < mAll; ++j)
                    {
                        ops.push_back(Op{'a', a0, b0 + j});
                    }
                    return true;
                }
                if (mAll == 0)
                {
                    for (std::size_t i = 0; i < nAll; ++i)
                    {
                        ops.push_back(Op{'d', a0 + i, b0});
                    }
                    return true;
                }

                const int n = static_cast<int>(nAll);
                const int m = static_cast<int>(mAll);
                const int max_d = n + m;
                const int width = 2 * max_d + 1;
                const int off = max_d;

                // The working set of the trace: one vector of width per edit distance,
                // bounded by the caller's cell budget — the very number diff.h promises,
                // so the exactness the header holds out inside the budget is held, and a
                // walk longer than the budget is the documented fallback rather than a
                // second, unstated limit. The row is measured before it is stood up: a
                // width past the budget is an allocation the answer is not worth, and
                // this API takes its refusals as return values.
                const std::size_t trace_cell_limit = static_cast<std::size_t>(budget);
                if (static_cast<std::size_t>(width) > trace_cell_limit)
                {
                    return false;
                }

                std::vector<std::vector<int>> trace;
                std::vector<int> v(static_cast<std::size_t>(width), 0);

                for (int d = 0; d <= max_d; ++d)
                {
                    if (trace.size() + 1 > trace_cell_limit / static_cast<std::size_t>(width))
                    {
                        return false;
                    }
                    trace.push_back(v);
                    for (int k = -d; k <= d; k += 2)
                    {
                        const int kx = k + off;
                        int x;
                        if (k == -d || (k != d && v[kx - 1] < v[kx + 1]))
                        {
                            x = v[kx + 1]; // down: a line taken only from b
                        }
                        else
                        {
                            x = v[kx - 1] + 1; // right: a line taken only from a
                        }
                        int y = x - k;
                        while (x < n && y < m &&
                               a[a0 + static_cast<std::size_t>(x)] == b[b0 + static_cast<std::size_t>(y)])
                        {
                            ++x;
                            ++y;
                        }
                        v[kx] = x;
                        if (x >= n && y >= m)
                        {
                            // The path is found; walk it back from the end of the grid.
                            std::vector<Op> back;
                            int cx = n, cy = m;
                            for (int dd = static_cast<int>(trace.size()) - 1; dd >= 0; --dd)
                            {
                                const std::vector<int> &vd = trace[static_cast<std::size_t>(dd)];
                                const int kk = cx - cy;
                                int prev_k;
                                if (kk == -dd || (kk != dd && vd[kk - 1 + off] < vd[kk + 1 + off]))
                                {
                                    prev_k = kk + 1;
                                }
                                else
                                {
                                    prev_k = kk - 1;
                                }
                                const int px = vd[prev_k + off];
                                const int py = px - prev_k;
                                while (cx > px && cy > py)
                                {
                                    back.push_back(Op{'k', a0 + static_cast<std::size_t>(cx - 1),
                                                      b0 + static_cast<std::size_t>(cy - 1)});
                                    --cx;
                                    --cy;
                                }
                                if (dd > 0)
                                {
                                    if (prev_k == kk + 1)
                                    {
                                        back.push_back(Op{'a', a0 + static_cast<std::size_t>(px),
                                                          b0 + static_cast<std::size_t>(py)});
                                    }
                                    else
                                    {
                                        back.push_back(Op{'d', a0 + static_cast<std::size_t>(px),
                                                          b0 + static_cast<std::size_t>(py)});
                                    }
                                }
                                cx = px;
                                cy = py;
                            }
                            std::reverse(back.begin(), back.end());
                            ops.insert(ops.end(), back.begin(), back.end());
                            return true;
                        }
                    }
                }
                return false; // the grid was not reached: cannot happen for its sizes
            }

            // Solves one window exactly within the budget — and cuts it honestly when it
            // cannot: every line of a removed, every line of b added.
            static void solve_window(const std::vector<std::string> &a, const std::vector<std::string> &b,
                                     std::size_t a0, std::size_t a1, std::size_t b0, std::size_t b1,
                                     std::uint64_t budget, std::vector<Op> &ops)
            {
                const std::size_t n = a1 - a0, m = b1 - b0;
                if (n == 0 && m == 0)
                {
                    return;
                }
                if (n == 0)
                {
                    for (std::size_t j = 0; j < m; ++j)
                    {
                        ops.push_back(Op{'a', a0, b0 + j});
                    }
                    return;
                }
                if (m == 0)
                {
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        ops.push_back(Op{'d', a0 + i, b0});
                    }
                    return;
                }
                if (n <= budget / m)
                {
                    if (myers_window(a, b, a0, a1, b0, b1, budget, ops))
                    {
                        return;
                    }
                }
                for (std::size_t i = 0; i < n; ++i)
                {
                    ops.push_back(Op{'d', a0 + i, b0});
                }
                for (std::size_t j = 0; j < m; ++j)
                {
                    ops.push_back(Op{'a', a0, b0 + j});
                }
            }

            // The patience-style fallback for one window: lines whose content appears
            // exactly once on each side are anchors, the longest increasing run of their
            // positions (patience's own LIS) is the common subsequence, and the windows
            // between the anchors are solved exactly when the budget allows and cut
            // honestly when it does not. The caller has already set the result's approx
            // flag — this only says what the lines are.
            static void patience_window(const std::vector<std::string> &a, const std::vector<std::string> &b,
                                        std::size_t a0, std::size_t a1, std::size_t b0, std::size_t b1,
                                        std::uint64_t budget, std::vector<Op> &ops)
            {
                const std::size_t n = a1 - a0, m = b1 - b0;
                if (n == 0 || m == 0)
                {
                    solve_window(a, b, a0, a1, b0, b1, budget, ops);
                    return;
                }
                // The anchors are held as int positions for the longest-increasing run, so
                // the same domain binds this pass too: past it the run is not what the
                // lines say, and the honest answer is the cut.
                if (n > static_cast<std::size_t>(kLineCountLimit) ||
                    m > static_cast<std::size_t>(kLineCountLimit))
                {
                    solve_window(a, b, a0, a1, b0, b1, budget, ops);
                    return;
                }

                // Lines that appear exactly once on each side, as (row, column) anchors:
                // a first position, or -1 once the content stands more than once.
                std::unordered_map<std::string, int> onceInA;
                for (std::size_t i = 0; i < n; ++i)
                {
                    const std::string &line = a[a0 + i];
                    auto found = onceInA.find(line);
                    if (found == onceInA.end())
                    {
                        onceInA.emplace(line, static_cast<int>(i));
                    }
                    else if (found->second >= 0)
                    {
                        found->second = -1;
                    }
                }
                std::unordered_map<std::string, int> onceInB;
                for (std::size_t j = 0; j < m; ++j)
                {
                    const std::string &line = b[b0 + j];
                    auto found = onceInB.find(line);
                    if (found == onceInB.end())
                    {
                        onceInB.emplace(line, static_cast<int>(j));
                    }
                    else if (found->second >= 0)
                    {
                        found->second = -1;
                    }
                }
                std::vector<std::pair<std::size_t, std::size_t>> anchors; // ordered by row
                for (std::size_t i = 0; i < n; ++i)
                {
                    const std::string &line = a[a0 + i];
                    auto inA = onceInA.find(line);
                    if (inA == onceInA.end() || inA->second < 0)
                    {
                        continue;
                    }
                    auto inB = onceInB.find(line);
                    if (inB == onceInB.end() || inB->second < 0)
                    {
                        continue;
                    }
                    anchors.push_back({i, static_cast<std::size_t>(inB->second)});
                }

                // Patience: the longest increasing run of the anchors' columns.
                std::vector<std::size_t> tails; // tails[len] = index into anchors
                std::vector<int> parent(anchors.size(), -1);
                for (std::size_t t = 0; t < anchors.size(); ++t)
                {
                    const std::size_t col = anchors[t].second;
                    auto at = std::lower_bound(tails.begin(), tails.end(), col,
                                               [&](std::size_t idx, std::size_t value)
                                               {
                                                   return anchors[idx].second < value;
                                               });
                    const int p = at == tails.begin() ? -1 : parent[static_cast<std::size_t>(*(at - 1))];
                    if (at == tails.end())
                    {
                        tails.push_back(t);
                    }
                    else
                    {
                        *at = t;
                    }
                    parent[t] = p;
                }
                std::vector<std::pair<std::size_t, std::size_t>> run; // the anchors kept, by row
                if (!tails.empty())
                {
                    std::size_t t = tails.back();
                    for (;;)
                    {
                        run.push_back(anchors[t]);
                        const int prev = parent[t];
                        if (prev < 0)
                        {
                            break;
                        }
                        t = static_cast<std::size_t>(prev);
                    }
                    std::reverse(run.begin(), run.end());
                }

                // The windows between the kept anchors.
                std::size_t rowCut = a0, colCut = b0;
                for (const std::pair<std::size_t, std::size_t> &anchor : run)
                {
                    const std::size_t row = a0 + anchor.first;
                    const std::size_t col = b0 + anchor.second;
                    solve_window(a, b, rowCut, row, colCut, col, budget, ops);
                    push_common(ops, row, col);
                    rowCut = row + 1;
                    colCut = col + 1;
                }
                solve_window(a, b, rowCut, a1, colCut, b1, budget, ops);
            }

            // The whole edit script: the trim always runs, the core is taken exactly when
            // the budget allows, and the flag says honestly whether the fallback answered.
            static bool compute_ops(const std::vector<std::string> &a, const std::vector<std::string> &b,
                                    std::uint64_t budget, std::vector<Op> &ops)
            {
                const std::size_t n = a.size(), m = b.size();

                std::size_t prefix = 0;
                while (prefix < n && prefix < m && a[prefix] == b[prefix])
                {
                    ++prefix;
                }
                std::size_t suffix = 0;
                while (suffix < n - prefix && suffix < m - prefix &&
                       a[n - 1 - suffix] == b[m - 1 - suffix])
                {
                    ++suffix;
                }

                ops.clear();
                for (std::size_t i = 0; i < prefix; ++i)
                {
                    push_common(ops, i, i);
                }

                const std::size_t coreA = n - prefix - suffix;
                const std::size_t coreB = m - prefix - suffix;
                bool approx = false;
                if (coreA == 0 || coreB == 0 || coreA <= budget / coreB)
                {
                    // inside the cell budget: the exact search answers the core
                    std::vector<Op> core;
                    if (!myers_window(a, b, prefix, n - suffix, prefix, m - suffix, budget, core))
                    {
                        approx = true; // the trace working set was passed: the fallback answers
                        patience_window(a, b, prefix, n - suffix, prefix, m - suffix, budget, ops);
                    }
                    else
                    {
                        ops.insert(ops.end(), core.begin(), core.end());
                    }
                }
                else
                {
                    approx = true; // over the cell budget: the patience fallback answers
                    patience_window(a, b, prefix, n - suffix, prefix, m - suffix, budget, ops);
                }

                for (std::size_t i = 0; i < suffix; ++i)
                {
                    push_common(ops, n - suffix + i, m - suffix + i);
                }
                return approx;
            }

            // ---- hunks ------------------------------------------------------------------

            // A run of the script that changes something: operations between two spans of
            // common lines. The lines it takes from each side are read back from the
            // operations themselves; the run is what the hunk grouping works on.
            struct Change
            {
                std::size_t opStart, opEnd; // the operations, [opStart, opEnd)
            };

            static std::vector<Change> find_changes(const std::vector<Op> &ops)
            {
                std::vector<Change> changes;
                std::size_t i = 0;
                while (i < ops.size())
                {
                    if (ops[i].kind == 'k')
                    {
                        ++i;
                        continue;
                    }
                    const std::size_t start = i;
                    while (i < ops.size() && ops[i].kind != 'k')
                    {
                        ++i;
                    }
                    Change c;
                    c.opStart = start;
                    c.opEnd = i;
                    changes.push_back(c);
                }
                return changes;
            }

            // Groups the changes into hunks the git way — a gap of common lines longer
            // than twice the context is what splits — and renders each with its context.
            // Inside a change, a side's deletions are written before its insertions, as
            // git writes a pair of edited lines.
            static void build_hunks(const std::vector<Op> &ops,
                                    const std::vector<std::string> &aLines,
                                    const std::vector<std::string> &bLines,
                                    int context, std::vector<Hunk> &hunks)
            {
                const std::vector<Change> changes = find_changes(ops);
                if (changes.empty())
                {
                    return;
                }

                const std::size_t want = context < 0 ? 0u : static_cast<std::size_t>(context);
                std::size_t group = 0;
                while (group < changes.size())
                {
                    const std::size_t groupStart = group;
                    std::size_t groupEnd = group + 1;
                    while (groupEnd < changes.size() &&
                           changes[groupEnd].opStart - changes[groupEnd - 1].opEnd <= 2u * want)
                    {
                        ++groupEnd;
                    }

                    // The operations this hunk covers: the changes of the group joined by
                    // their common lines, with at most context of the common lines that
                    // bound the group on each side.
                    const std::size_t firstOp = changes[groupStart].opStart;
                    const std::size_t lastOpEnd = changes[groupEnd - 1].opEnd;
                    const std::size_t availableBefore =
                        groupStart == 0 ? firstOp : firstOp - changes[groupStart - 1].opEnd;
                    const std::size_t availableAfter =
                        groupEnd == changes.size() ? ops.size() - lastOpEnd
                                                   : changes[groupEnd].opStart - lastOpEnd;
                    const std::size_t spanStart = firstOp - std::min(want, availableBefore);
                    const std::size_t spanEnd = lastOpEnd + std::min(want, availableAfter);

                    Hunk h;
                    std::size_t countA = 0, countB = 0;
                    bool firstA = false, firstB = false;
                    int startA = 0, startB = 0;
                    std::size_t i = spanStart;
                    while (i < spanEnd)
                    {
                        if (ops[i].kind == 'k')
                        {
                            h.lines.push_back(HunkLine{'c', aLines[ops[i].a]});
                            if (!firstA)
                            {
                                startA = static_cast<int>(ops[i].a);
                                firstA = true;
                            }
                            if (!firstB)
                            {
                                startB = static_cast<int>(ops[i].b);
                                firstB = true;
                            }
                            ++countA;
                            ++countB;
                            ++i;
                            continue;
                        }
                        // The whole change run at once: its deletions first, then its
                        // insertions — a run is always inside the hunk's span.
                        std::size_t runEnd = i;
                        while (runEnd < spanEnd && ops[runEnd].kind != 'k')
                        {
                            ++runEnd;
                        }
                        for (std::size_t t = i; t < runEnd; ++t)
                        {
                            if (ops[t].kind != 'd')
                            {
                                continue;
                            }
                            h.lines.push_back(HunkLine{'d', aLines[ops[t].a]});
                            if (!firstA)
                            {
                                startA = static_cast<int>(ops[t].a);
                                firstA = true;
                            }
                            ++countA;
                        }
                        for (std::size_t t = i; t < runEnd; ++t)
                        {
                            if (ops[t].kind != 'a')
                            {
                                continue;
                            }
                            h.lines.push_back(HunkLine{'a', bLines[ops[t].b]});
                            if (!firstB)
                            {
                                startB = static_cast<int>(ops[t].b);
                                firstB = true;
                            }
                            ++countB;
                        }
                        i = runEnd;
                    }

                    // A range of no lines starts at the line before it (0 when there is
                    // none); every other range starts at its first line.
                    h.countA = static_cast<int>(countA);
                    h.countB = static_cast<int>(countB);
                    h.startA = countA == 0 ? static_cast<int>(ops[spanStart].a) : startA + 1;
                    h.startB = countB == 0 ? static_cast<int>(ops[spanStart].b) : startB + 1;
                    hunks.push_back(std::move(h));

                    group = groupEnd;
                }
            }

        } // namespace detail

        // ---- the public surface -------------------------------------------------------

        LineData split_lines(const std::string &text)
        {
            LineData out;
            if (text.empty())
            {
                return out;
            }
            std::size_t pos = 0;
            while (pos < text.size())
            {
                const std::size_t nl = text.find('\n', pos);
                if (nl == std::string::npos)
                {
                    out.lines.push_back(text.substr(pos));
                    out.noNewline = true;
                    break;
                }
                out.lines.push_back(text.substr(pos, nl - pos));
                pos = nl + 1;
            }
            return out;
        }

        // The lines as the bytes had them: each line with the newline that ended it
        // back on it, and the last one only when the side ended with one. Under this
        // identity "x" and "x\n" are different lines — git's own reading, which
        // compares the bytes behind the last newline whether they end in one or not.
        // On every line but the last the newline stands on both sides alike, so the
        // comparison between two sides that agree about the final newline is the
        // comparison of the bare lines; the fold only matters when they disagree.
        static std::vector<std::string> with_terminators(const std::vector<std::string> &lines,
                                                         bool noNewline)
        {
            std::vector<std::string> out;
            out.reserve(lines.size());
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                std::string line = lines[i];
                if (i + 1 < lines.size() || !noNewline)
                {
                    line.push_back('\n');
                }
                out.push_back(std::move(line));
            }
            return out;
        }

        DiffResult diff_lines(const LineData &a, const LineData &b, const DiffOptions &options)
        {
            DiffResult d;
            d.aLineCount = a.lines.size();
            d.bLineCount = b.lines.size();
            d.aNoNewline = a.noNewline;
            d.bNoNewline = b.noNewline;

            // The flag belongs to the last line's identity: a side that lost its
            // final newline changed a line, and the studio that shows the diff must
            // see the change. The search and the trim both compare through these
            // vectors, so the flag is folded in once, here, and nowhere else; the
            // hunks are rendered from the bare lines, as git renders them.
            std::vector<std::string> cmpA, cmpB;
            const std::vector<std::string> *linesA = &a.lines;
            const std::vector<std::string> *linesB = &b.lines;
            if (a.noNewline != b.noNewline)
            {
                cmpA = with_terminators(a.lines, a.noNewline);
                cmpB = with_terminators(b.lines, b.noNewline);
                linesA = &cmpA;
                linesB = &cmpB;
            }

            std::vector<detail::Op> ops;
            d.approx = detail::compute_ops(*linesA, *linesB, options.cellBudget, ops);
            detail::build_hunks(ops, a.lines, b.lines, options.context, d.hunks);
            return d;
        }

        DiffResult diff_blobs(const std::string &textA, const std::string &textB, const DiffOptions &options)
        {
            return diff_lines(split_lines(textA), split_lines(textB), options);
        }

        // The header number of a range: a range of one line is its number alone, as
        // git writes it; every other range says where it starts and how long it is.
        static void append_range(std::string &out, int start, int count)
        {
            out += std::to_string(start);
            if (count != 1)
            {
                out += ',';
                out += std::to_string(count);
            }
        }

        std::string render_unified(const DiffResult &d, const UnifiedInput &input)
        {
            if (d.hunks.empty())
            {
                return std::string(); // git prints nothing at all for identical blobs
            }

            std::string out;
            out += "diff --git a/" + input.pathA + " b/" + input.pathB + "\n";
            out += "index " + input.oidA + ".." + input.oidB + " 100644\n";
            out += "--- a/" + input.pathA + "\n";
            out += "+++ b/" + input.pathB + "\n";

            // Where the hunk lines stand on each side, so a line that is the last of
            // a side can be told, and its side's missing newline marked.
            std::size_t aAt = 0, bAt = 0;
            for (const Hunk &h : d.hunks)
            {
                out += "@@ -";
                append_range(out, h.startA, h.countA);
                out += " +";
                append_range(out, h.startB, h.countB);
                out += " @@\n";

                // A range of no lines stands after its start; every other range
                // starts at its start.
                aAt = h.countA == 0 ? static_cast<std::size_t>(h.startA) : static_cast<std::size_t>(h.startA - 1);
                bAt = h.countB == 0 ? static_cast<std::size_t>(h.startB) : static_cast<std::size_t>(h.startB - 1);

                for (const HunkLine &line : h.lines)
                {
                    switch (line.kind)
                    {
                    case 'c':
                        out += ' ';
                        out += line.text;
                        out += '\n';
                        ++aAt;
                        ++bAt;
                        break;
                    case 'd':
                        out += '-';
                        out += line.text;
                        out += '\n';
                        ++aAt;
                        break;
                    default:
                        out += '+';
                        out += line.text;
                        out += '\n';
                        ++bAt;
                        break;
                    }
                    const bool atEndA = d.aNoNewline && (line.kind == 'c' || line.kind == 'd') &&
                                        aAt == d.aLineCount;
                    const bool atEndB = d.bNoNewline && (line.kind == 'a' || line.kind == 'c') &&
                                        bAt == d.bLineCount;
                    if (atEndA || atEndB)
                    {
                        out += "\\ No newline at end of file\n";
                    }
                }
            }
            return out;
        }

        // ---- word spans ---------------------------------------------------------------

        // The tokens of a line: a run of word characters, or one character off them.
        // The off-word characters stand as tokens of their own so that the byte
        // offsets of the spans they join stay exact.
        static std::vector<std::pair<std::size_t, std::size_t>> tokenize_words(const std::string &line)
        {
            auto isWordChar = [](unsigned char c)
            {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_';
            };
            std::vector<std::pair<std::size_t, std::size_t>> tokens; // [start, len)
            std::size_t i = 0;
            while (i < line.size())
            {
                std::size_t j = i + 1;
                if (isWordChar(static_cast<unsigned char>(line[i])))
                {
                    while (j < line.size() && isWordChar(static_cast<unsigned char>(line[j])))
                    {
                        ++j;
                    }
                }
                tokens.push_back({i, j - i});
                i = j;
            }
            return tokens;
        }

        // Two tokens of two lines stand for the same word when they read the same.
        static bool same_word(const std::string &lineA, const std::pair<std::size_t, std::size_t> &ta,
                              const std::string &lineB, const std::pair<std::size_t, std::size_t> &tb)
        {
            return ta.second == tb.second &&
                   lineA.compare(ta.first, ta.second, lineB.substr(tb.first, tb.second)) == 0;
        }

        // The cells the token grid of one line pair is afforded. The LCS below is the
        // square of the token counts, and a "line" is only what the bytes between two
        // newlines happen to be: one side of a minified asset, or a blob that is not
        // text at all, hands over a line of tens of millions of tokens, and the square
        // of that is an allocation the answer is not worth — it asks for an exception
        // out of a serving thread, which this API does not do. Past the affordance the
        // spans say the plain thing instead: these two lines differ, and the difference
        // is somewhere inside them.
        constexpr std::size_t kWordSpanCellLimit = 4u * 1024u * 1024u;

        std::vector<WordSpan> word_spans(const std::string &lineA, const std::string &lineB)
        {
            const std::vector<std::pair<std::size_t, std::size_t>> ta = tokenize_words(lineA);
            const std::vector<std::pair<std::size_t, std::size_t>> tb = tokenize_words(lineB);
            const std::size_t n = ta.size(), m = tb.size();
            if (lineA == lineB)
            {
                return {};
            }

            // The LCS of the tokens, at the small scale a line is — with the scale
            // checked as a product before the grid is stood up, since either count
            // alone says nothing about what the two of them cost.
            if (n + 1 > kWordSpanCellLimit / (m + 1))
            {
                WordSpan whole;
                whole.aStart = 0;
                whole.aLen = static_cast<int>(lineA.size() < static_cast<std::size_t>((std::numeric_limits<int>::max)())
                                                  ? lineA.size()
                                                  : (std::numeric_limits<int>::max)());
                whole.bStart = 0;
                whole.bLen = static_cast<int>(lineB.size() < static_cast<std::size_t>((std::numeric_limits<int>::max)())
                                                  ? lineB.size()
                                                  : (std::numeric_limits<int>::max)());
                return {whole};
            }
            std::vector<std::vector<std::uint32_t>> dp(n + 1, std::vector<std::uint32_t>(m + 1, 0));
            for (std::size_t i = n; i-- > 0;)
            {
                for (std::size_t j = m; j-- > 0;)
                {
                    if (same_word(lineA, ta[i], lineB, tb[j]))
                    {
                        dp[i][j] = dp[i + 1][j + 1] + 1;
                    }
                    else
                    {
                        dp[i][j] = std::max(dp[i + 1][j], dp[i][j + 1]);
                    }
                }
            }

            // Between two consecutive common tokens, whatever stands between them on
            // either side is one span.
            std::vector<WordSpan> spans;
            auto emit_gap = [&](std::size_t a0, std::size_t a1, std::size_t b0, std::size_t b1)
            {
                if (a0 >= a1 && b0 >= b1)
                {
                    return;
                }
                WordSpan s;
                if (a0 < a1)
                {
                    s.aStart = static_cast<int>(ta[a0].first);
                    s.aLen = static_cast<int>(ta[a1 - 1].first + ta[a1 - 1].second - ta[a0].first);
                }
                else
                {
                    s.aStart = a0 > 0 ? static_cast<int>(ta[a0 - 1].first + ta[a0 - 1].second) : 0;
                    s.aLen = 0;
                }
                if (b0 < b1)
                {
                    s.bStart = static_cast<int>(tb[b0].first);
                    s.bLen = static_cast<int>(tb[b1 - 1].first + tb[b1 - 1].second - tb[b0].first);
                }
                else
                {
                    s.bStart = b0 > 0 ? static_cast<int>(tb[b0 - 1].first + tb[b0 - 1].second) : 0;
                    s.bLen = 0;
                }
                spans.push_back(s);
            };

            std::size_t lastA = 0, lastB = 0; // one past the last common token
            std::size_t x = 0, y = 0;
            while (x < n && y < m)
            {
                if (same_word(lineA, ta[x], lineB, tb[y]) && dp[x][y] == dp[x + 1][y + 1] + 1)
                {
                    emit_gap(lastA, x, lastB, y);
                    lastA = x + 1;
                    lastB = y + 1;
                    ++x;
                    ++y;
                }
                else if (dp[x + 1][y] >= dp[x][y + 1])
                {
                    ++x;
                }
                else
                {
                    ++y;
                }
            }
            emit_gap(lastA, n, lastB, m);
            return spans;
        }

        // ---- rename pairing -----------------------------------------------------------

        std::vector<RenamePair> detect_exact_renames(const std::vector<ListingEntry> &listingFrom,
                                                     const std::vector<ListingEntry> &listingTo)
        {
            // What arrived, by path and by oid.
            std::map<std::string, std::string> arrivedAtPath;
            std::map<std::string, std::vector<std::size_t>> arrivedByOid;
            for (std::size_t i = 0; i < listingTo.size(); ++i)
            {
                arrivedAtPath[listingTo[i].path] = listingTo[i].oid;
                arrivedByOid[listingTo[i].oid].push_back(i);
            }

            std::vector<char> taken(listingTo.size(), 0);
            std::vector<RenamePair> pairs;
            for (const ListingEntry &left : listingFrom)
            {
                auto at = arrivedAtPath.find(left.path);
                if (at != arrivedAtPath.end() && at->second == left.oid)
                {
                    continue; // the path is unchanged: never a rename
                }
                auto byOid = arrivedByOid.find(left.oid);
                if (byOid == arrivedByOid.end())
                {
                    continue; // no path arrived holding it
                }
                for (std::size_t candidate : byOid->second)
                {
                    if (taken[candidate])
                    {
                        continue;
                    }
                    // A path that stayed with a different oid is a change, not a
                    // rename of the path that left.
                    if (listingTo[candidate].path == left.path)
                    {
                        continue;
                    }
                    taken[candidate] = 1;
                    pairs.push_back(RenamePair{left.path, listingTo[candidate].path});
                    break;
                }
            }
            return pairs;
        }

    } // namespace gitcore
} // namespace ols
