// src/gitcore/diff.h — the line diff of GitCore (spec §3.3).
//
// Blob diff over lines: a Myers O(ND) diff behind a cell budget, git's own
// prefix/suffix trim always running first, and a patience-style fallback
// (the LCS over lines whose content is unique on both sides) when the core
// exceeds the budget — which the result says so with approx:true, honestly.
// The unified output follows git's shape: the @@ header, the hunk split at
// more than 2*context unchanged lines, the bare line number when a range is
// one line, and "\ No newline at end of file" when a side's last line lacks
// one.
//
// Word spans are the viewer's, not git's: tokens split at word boundaries,
// LCS'd at small scale, coalesced into byte ranges. No claim of byte
// agreement with git's own word-diff is made or tested anywhere.
//
// Failure is a return value, never a thrown exception: the functions here
// take what they are handed and answer; nothing they refuse escapes as an
// exception crossing this API.

#ifndef OLS_GITCORE_DIFF_H
#define OLS_GITCORE_DIFF_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ols
{
    namespace gitcore
    {

        // A blob as lines: the lines with their terminating '\n' removed, and what the
        // bytes said about the last one. An empty text is no lines and no missing
        // newline; a last line without '\n' is still a line and says so.
        struct LineData
        {
            std::vector<std::string> lines;
            bool noNewline = false;
        };

        // Bytes to lines.
        LineData split_lines(const std::string &text);

        // What a diff may be asked for.
        struct DiffOptions
        {
            // The unchanged lines kept around a change; git's own default.
            int context = 3;
            // The one budget of the exact search, and the one truth about it: the
            // trimmed core is taken by the search when it is at most this many cells
            // (coreA × coreB), and the trace the search stands up while walking is
            // bounded by the same number — never a second, unstated limit. Past
            // either gate the patience fallback runs and the result carries
            // approx:true; within both the answer is minimal.
            std::uint64_t cellBudget = 4000000ULL;
        };

        // One line of a hunk, as it stands in the output: 'c' unchanged, 'd' only in
        // a, 'a' only in b — the kinds the spec asks for as ctx/add/del.
        struct HunkLine
        {
            char kind = 'c';
            std::string text;
        };

        // A hunk. The start/count pairs are exactly the values of the "@@ -l,c +l,c"
        // header: one-based line numbers, and a range of one line carries its count
        // nowhere (git's own shape). A range of no lines starts at the line before it
        // (0 when there is none).
        struct Hunk
        {
            int startA = 0, countA = 0;
            int startB = 0, countB = 0;
            std::vector<HunkLine> lines;
        };

        // A whole diff: the hunks, and what the two sides' bytes said about their
        // last lines — the unified emitter needs them for its no-newline markers.
        struct DiffResult
        {
            std::vector<Hunk> hunks;

            std::size_t aLineCount = 0, bLineCount = 0;
            bool aNoNewline = false, bNoNewline = false;

            // Says whether the answer came from the budget's fallback. When it is
            // false the diff is a minimal one over the lines.
            bool approx = false;
        };

        // The diff of two blobs given as lines.
        DiffResult diff_lines(const LineData &a, const LineData &b,
                              const DiffOptions &options = DiffOptions());

        // The diff of two blobs given as bytes.
        DiffResult diff_blobs(const std::string &textA, const std::string &textB,
                              const DiffOptions &options = DiffOptions());

        namespace detail
        {

            // The domain of the exact search's numbering: the grid the walk stands has
            // width 2*(n+m)+1, which its int holds only while the span n+m stays under
            // 2^30 lines. The search refuses a window past the span, and the fallback
            // answers — so a case can state the guard at 2^30 without standing two
            // billion lines to diff.
            bool gridFitsSpan(std::size_t n, std::size_t m);

        } // namespace detail

        // What the unified output stands for: the paths and the oids exactly as the
        // caller holds them — the oracles of this folder recorded 7-hex shorthands,
        // and nothing here recomputes or pads an oid. The mode is 100644 for both
        // sides: a blob diff of two files, as the oracles were made with.
        struct UnifiedInput
        {
            std::string pathA, pathB;
            std::string oidA, oidB;
        };

        // The unified diff, byte for byte the shape git writes for the fixtures of
        // this folder. Nothing is emitted for a diff without hunks: so does git,
        // which prints not even the file header for two identical blobs.
        std::string render_unified(const DiffResult &d, const UnifiedInput &input);

        // The word-level spans of a changed line pair, for the viewer: byte ranges
        // where the two lines differ, each pair of ranges as one span. Not git-exact;
        // nothing claims it is.
        struct WordSpan
        {
            int aStart = 0, aLen = 0;
            int bStart = 0, bLen = 0;
        };

        std::vector<WordSpan> word_spans(const std::string &lineA, const std::string &lineB);

        // A tree listing: a path and the oid it holds. The object layer declares its
        // own TreeEntry for the bytes of a tree (mode, name, id); this one is what the
        // rename pass reads - a flattened path and a hex string - so it carries its
        // own name and never collides with that header in one translation unit.
        struct ListingEntry
        {
            std::string path, oid;
        };

        // A rename: the path it was at, and the path it is at.
        struct RenamePair
        {
            std::string from, to;
        };

        // Phase one of rename detection: of two tree listings, pair each path that
        // left with a path that arrived holding the same oid exactly. A path in both
        // listings with the same oid is unchanged and never a rename; a pair whose
        // oids differ is not a rename. Pure: the listings are not touched, and the
        // pairs come out in the order of the listing the renames left.
        std::vector<RenamePair> detect_exact_renames(const std::vector<ListingEntry> &listingFrom,
                                                     const std::vector<ListingEntry> &listingTo);

    } // namespace gitcore
} // namespace ols

#endif // OLS_GITCORE_DIFF_H
