// tests/gitcore/test_diff.cpp — the cases of the line diff of GitCore (spec §3.3).
//
// The two fixture pairs are compared against their recorded oracles byte for
// byte: whatever the engine answers, the output is what git's own diff says
// of the same inputs. The rest of the cases hold the claims the fixtures do
// not carry — where a hunk begins when the change stands at the edge of a
// file, what splits a hunk from the next one, what a range of one line is
// called in a header, and what the fallback is allowed to promise.

#include "gitcore/diff.h"

#include <string>
#include <vector>

namespace {

namespace gitcore = ols::gitcore;

// A fixture read on a Windows checkout can carry CRLF; the oracles were made
// with LF only. Normalized here, so a case states what the bytes say once.
static std::string without_crlf(const std::string& bytes)
{
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n') {
            continue;
        }
        out += bytes[i];
    }
    return out;
}

// The text of a list of lines, with or without the terminating newline of
// the last one — the only way a test can hold a file that ends mid-line.
static std::string as_text(const std::vector<std::string>& lines, bool lastNewline)
{
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size() || lastNewline) {
            out += '\n';
        }
    }
    return out;
}

// A file of known length, so the hunk arithmetic of a case can be read.
static std::vector<std::string> numbered(int n)
{
    std::vector<std::string> lines;
    for (int i = 1; i <= n; ++i) {
        lines.push_back("line" + std::to_string(i));
    }
    return lines;
}

static std::size_t count_of(const std::string& text, const std::string& what)
{
    std::size_t found = 0;
    for (std::size_t at = text.find(what); at != std::string::npos;
         at = text.find(what, at + what.size())) {
        ++found;
    }
    return found;
}

// The unified output for two inputs standing at the fixture's own names and
// oids — both oracles name a.txt on each side.
static std::string render_of(const gitcore::DiffResult& d, const std::string& oidA, const std::string& oidB)
{
    gitcore::UnifiedInput input;
    input.pathA = input.pathB = "a.txt";
    input.oidA = oidA;
    input.oidB = oidB;
    return gitcore::render_unified(d, input);
}

// A label for the header of a rendered diff. The pairs written in these cases
// stand for no repository, and render_unified writes what it is given: the two
// names here say nothing but "the side before" and "the side after".
const char* const kNoRepoOid = "0000000000000000000000000000000000000000";

// What the hunks of a diff would turn a into: a fallback answers no oracle,
// but its hunks must still be a true description of the inputs.
static std::vector<std::string> apply_of(const std::vector<std::string>& lines,
                                         const gitcore::DiffResult& d)
{
    std::vector<std::string> out;
    std::size_t at = 0;
    for (const gitcore::Hunk& h : d.hunks) {
        // A range of no lines stands after its start; every other range
        // starts at its start.
        const std::size_t stop = h.countA == 0 ? static_cast<std::size_t>(h.startA)
                                               : static_cast<std::size_t>(h.startA - 1);
        for (; at < stop && at < lines.size(); ++at) {
            out.push_back(lines[at]);
        }
        for (const gitcore::HunkLine& line : h.lines) {
            if (line.kind == 'c') {
                out.push_back(line.text);
                ++at;
            } else if (line.kind == 'd') {
                ++at;
            } else {
                out.push_back(line.text);
            }
        }
    }
    for (; at < lines.size(); ++at) {
        out.push_back(lines[at]);
    }
    return out;
}

// The claims about a rendered diff that the cases keep repeating.
static void claim_markers(const std::string& rendered, std::size_t expected)
{
    CHECK_EQ(count_of(rendered, "\\ No newline at end of file"), expected);
}

} // anonymous namespace

TEST(unified_diff_matches_the_a_pair_fixture)
{
    const std::string textA = without_crlf(ols_test::read_fixture("diff/a1.txt"));
    const std::string textB = without_crlf(ols_test::read_fixture("diff/a2.txt"));
    const std::string oracle = without_crlf(ols_test::read_fixture("diff/a1-a2.diff"));
    CHECK(!textA.empty());
    CHECK(!textB.empty());
    CHECK(!oracle.empty());

    const gitcore::DiffResult d = gitcore::diff_blobs(textA, textB);
    CHECK(!d.approx); // the default budget answers these exactly
    CHECK_EQ(d.hunks.size(), 1u);
    CHECK_EQ(d.hunks[0].startA, 1);
    CHECK_EQ(d.hunks[0].countA, 8);
    CHECK_EQ(d.hunks[0].startB, 1);
    CHECK_EQ(d.hunks[0].countB, 12);
    CHECK_EQ(render_of(d, "e0396ff", "471c2d9"), oracle);
}

TEST(unified_diff_matches_the_b_pair_fixture)
{
    const std::string textA = without_crlf(ols_test::read_fixture("diff/b1.txt"));
    const std::string textB = without_crlf(ols_test::read_fixture("diff/b2.txt"));
    const std::string oracle = without_crlf(ols_test::read_fixture("diff/b1-b2.diff"));
    CHECK(!textA.empty());
    CHECK(!textB.empty());
    CHECK(!oracle.empty());

    const gitcore::DiffResult d = gitcore::diff_blobs(textA, textB);
    CHECK(!d.approx);
    CHECK_EQ(d.hunks.size(), 1u);
    CHECK_EQ(d.hunks[0].startA, 1);
    CHECK_EQ(d.hunks[0].countA, 5);
    CHECK_EQ(d.hunks[0].startB, 1);
    CHECK_EQ(d.hunks[0].countB, 6);
    CHECK_EQ(render_of(d, "41a586c", "6dd1ac6"), oracle);
}

TEST(a_change_at_the_first_line_leads_its_hunk_from_one)
{
    const std::vector<std::string> before = numbered(10);
    std::vector<std::string> after = before;
    after[0] = "changed at the top";

    const gitcore::DiffResult d = gitcore::diff_blobs(as_text(before, true), as_text(after, true));
    CHECK_EQ(d.hunks.size(), 1u);
    CHECK_EQ(d.hunks[0].startA, 1); // the change is the first line: no context above
    CHECK_EQ(d.hunks[0].countA, 4); // the change and the three lines under it
    CHECK_EQ(d.hunks[0].startB, 1);
    CHECK_EQ(d.hunks[0].countB, 4);

    const std::string rendered = render_of(d, "aaaaaaa", "bbbbbbb");
    CHECK(rendered.find("@@ -1,4 +1,4 @@") != std::string::npos);
    CHECK(rendered.find("-line1\n") != std::string::npos);
    claim_markers(rendered, 0u); // both files end with a newline: nothing to mark
}

TEST(a_change_at_the_end_keeps_the_newline_of_a_whole_last_line)
{
    const std::vector<std::string> before = numbered(10);
    std::vector<std::string> after = before;
    after[9] = "changed at the end";

    const gitcore::DiffResult d = gitcore::diff_blobs(as_text(before, true), as_text(after, true));
    CHECK_EQ(d.hunks.size(), 1u);
    CHECK_EQ(d.hunks[0].startA, 7); // the three lines above the last one
    CHECK_EQ(d.hunks[0].countA, 4);
    CHECK_EQ(d.hunks[0].startB, 7);
    CHECK_EQ(d.hunks[0].countB, 4);

    const std::string rendered = render_of(d, "aaaaaaa", "bbbbbbb");
    CHECK(rendered.find("@@ -7,4 +7,4 @@") != std::string::npos);
    claim_markers(rendered, 0u);
}

TEST(a_last_line_without_its_newline_is_marked_on_the_side_that_lost_it)
{
    const std::vector<std::string> before = numbered(10);
    std::vector<std::string> after = before;
    after[9] = "changed at the end";

    // a ends with a newline, b does not: one side lost it, the other did not.
    const gitcore::DiffResult d = gitcore::diff_blobs(as_text(before, true), as_text(after, false));
    const std::string rendered = render_of(d, "aaaaaaa", "bbbbbbb");
    CHECK(rendered.find("@@ -7,4 +7,4 @@") != std::string::npos);
    claim_markers(rendered, 1u);
    CHECK(rendered.find("+changed at the end\n\\ No newline at end of file\n") != std::string::npos);
}

TEST(a_last_line_without_its_newline_on_both_sides_is_marked_once_each)
{
    const std::vector<std::string> before = numbered(10);
    std::vector<std::string> after = before;
    after[9] = "changed at the end";

    const gitcore::DiffResult d = gitcore::diff_blobs(as_text(before, false), as_text(after, false));
    const std::string rendered = render_of(d, "aaaaaaa", "bbbbbbb");
    CHECK(rendered.find("@@ -7,4 +7,4 @@") != std::string::npos);
    claim_markers(rendered, 2u);
    CHECK(rendered.find("-line10\n\\ No newline at end of file\n") != std::string::npos);
    CHECK(rendered.find("+changed at the end\n\\ No newline at end of file\n") != std::string::npos);
}

TEST(six_unchanged_lines_between_two_changes_keep_one_hunk)
{
    const std::vector<std::string> before = numbered(20);
    std::vector<std::string> after = before;
    after[4] = "CHANGED_A";
    after[11] = "CHANGED_B"; // six lines stand between the two changes

    const gitcore::DiffResult d = gitcore::diff_blobs(as_text(before, true), as_text(after, true));
    CHECK_EQ(d.hunks.size(), 1u);
    CHECK_EQ(d.hunks[0].startA, 2);
    CHECK_EQ(d.hunks[0].countA, 14); // three lines above, both changes, three below
    claim_markers(render_of(d, "aaaaaaa", "bbbbbbb"), 0u);
}

TEST(seven_unchanged_lines_between_two_changes_split_two_hunks)
{
    const std::vector<std::string> before = numbered(20);
    std::vector<std::string> after = before;
    after[4] = "CHANGED_A";
    after[12] = "CHANGED_B"; // seven lines stand between the two changes

    const gitcore::DiffResult d = gitcore::diff_blobs(as_text(before, true), as_text(after, true));
    CHECK_EQ(d.hunks.size(), 2u);
    CHECK_EQ(d.hunks[0].startA, 2);
    CHECK_EQ(d.hunks[0].countA, 7);
    CHECK_EQ(d.hunks[1].startA, 10); // the three lines above the second change
    CHECK_EQ(d.hunks[1].countA, 7);
    claim_markers(render_of(d, "aaaaaaa", "bbbbbbb"), 0u);
}

TEST(a_hunk_of_one_line_names_it_without_a_count)
{
    // Asked without any context, a change is only its own lines — and the
    // header of a range of one line is the line's number alone.
    gitcore::DiffOptions none;
    none.context = 0;

    std::vector<std::string> before = numbered(20);
    std::vector<std::string> after = before;
    after[4] = "CHANGED";
    const gitcore::DiffResult d = gitcore::diff_blobs(as_text(before, true), as_text(after, true), none);
    CHECK_EQ(d.hunks.size(), 1u);
    CHECK_EQ(d.hunks[0].startA, 5);
    CHECK_EQ(d.hunks[0].countA, 1);
    CHECK_EQ(d.hunks[0].startB, 5);
    CHECK_EQ(d.hunks[0].countB, 1);
    CHECK(render_of(d, "aaaaaaa", "bbbbbbb").find("@@ -5 +5 @@") != std::string::npos);

    // An inserted line names no line of a at all: a range of none stands at
    // the line before it, and the one line of b is named bare.
    std::vector<std::string> withExtra = numbered(20);
    std::vector<std::string> inserted = withExtra;
    inserted.insert(inserted.begin() + 4, "inserted");
    const gitcore::DiffResult added = gitcore::diff_blobs(as_text(withExtra, true), as_text(inserted, true), none);
    CHECK_EQ(added.hunks.size(), 1u);
    CHECK_EQ(added.hunks[0].startA, 4);
    CHECK_EQ(added.hunks[0].countA, 0);
    CHECK_EQ(added.hunks[0].startB, 5);
    CHECK_EQ(added.hunks[0].countB, 1);
    CHECK(render_of(added, "aaaaaaa", "bbbbbbb").find("@@ -4,0 +5 @@") != std::string::npos);

    // The whole of a one-line file, and nothing at all beside it.
    const gitcore::DiffResult only = gitcore::diff_blobs("x\n", "", none);
    CHECK_EQ(only.hunks.size(), 1u);
    CHECK_EQ(only.hunks[0].countA, 1);
    CHECK_EQ(only.hunks[0].countB, 0);
    CHECK_EQ(render_of(only, "aaaaaaa", "bbbbbbb"),
             std::string("diff --git a/a.txt b/a.txt\nindex aaaaaaa..bbbbbbb 100644\n"
                         "--- a/a.txt\n+++ b/a.txt\n@@ -1 +0,0 @@\n-x\n"));
    const gitcore::DiffResult grown = gitcore::diff_blobs("", "x\n", none);
    CHECK_EQ(grown.hunks.size(), 1u);
    CHECK_EQ(grown.hunks[0].countA, 0);
    CHECK_EQ(grown.hunks[0].countB, 1);
    CHECK_EQ(render_of(grown, "aaaaaaa", "bbbbbbb"),
             std::string("diff --git a/a.txt b/a.txt\nindex aaaaaaa..bbbbbbb 100644\n"
                         "--- a/a.txt\n+++ b/a.txt\n@@ -0,0 +1 @@\n+x\n"));
}

TEST(exact_oid_matches_across_trees_pair_only_with_their_own_content)
{
    // What left, and what arrived: one path renamed, one path unchanged, one
    // deleted, a second path carrying the same content to a second name, and
    // one path whose content changed.
    const std::vector<gitcore::ListingEntry> from = {
        {"old/a.txt", "oidAlpha"},
        {"keep/b.txt", "oidKeep"},
        {"gone/c.txt", "oidGone"},
        {"moved/d.txt", "oidAlpha"},
        {"same/e.txt", "oidSame"},
    };
    const std::vector<gitcore::ListingEntry> to = {
        {"new/a.txt", "oidAlpha"},
        {"keep/b.txt", "oidKeep"},
        {"extra/f.txt", "oidOther"},
        {"second/g.txt", "oidAlpha"},
        {"same/e.txt", "oidChanged"},
    };

    const std::vector<gitcore::RenamePair> pairs = gitcore::detect_exact_renames(from, to);
    CHECK_EQ(pairs.size(), 2u);
    CHECK_EQ(pairs[0].from, "old/a.txt");
    CHECK_EQ(pairs[0].to, "new/a.txt");
    CHECK_EQ(pairs[1].from, "moved/d.txt");
    CHECK_EQ(pairs[1].to, "second/g.txt");
    // keep/b.txt stands in both with the same oid: unchanged, never a pair.
    // gone/c.txt and extra/f.txt hold no content in common with anything.
    // same/e.txt changed its content: a path that stayed is not a rename.

    // A path that stands in both with different content is a change, not a
    // rename: the pairing asks for content that moved, and none did.
    const std::vector<gitcore::ListingEntry> stayed = {{"p.txt", "aaaaaaa"}};
    CHECK_EQ(gitcore::detect_exact_renames(stayed, stayed).size(), 0u);
}

TEST(a_pure_insertion_into_a_line_is_one_span_of_the_added_words)
{
    const std::vector<gitcore::WordSpan> atStart = gitcore::word_spans("omega", "alpha omega");
    CHECK_EQ(atStart.size(), 1u); // the words before the common tail, and nothing of a
    CHECK_EQ(atStart[0].aStart, 0);
    CHECK_EQ(atStart[0].aLen, 0);
    CHECK_EQ(atStart[0].bStart, 0);
    CHECK_EQ(atStart[0].bLen, 6);

    const std::vector<gitcore::WordSpan> atEnd = gitcore::word_spans("keep this", "keep this added word");
    CHECK_EQ(atEnd.size(), 1u);
    CHECK_EQ(atEnd[0].aStart, 9); // the words end where a's line does
    CHECK_EQ(atEnd[0].aLen, 0);
    CHECK_EQ(atEnd[0].bStart, 9);
    CHECK_EQ(atEnd[0].bLen, 11); // " added word": the gap is eleven bytes wide
}

TEST(a_line_that_did_not_change_has_no_word_spans)
{
    const std::string line = "the line that gets edited in place";
    CHECK(gitcore::word_spans(line, line).empty());
}

TEST(the_spans_of_an_edited_line_stay_inside_both_of_its_lines)
{
    // What the spans of a changed pair are is not claimed against git — the
    // library says so; what is claimed is that they describe places in the
    // two lines, and that they are only ever asked for a pair that differs.
    const std::string lineA = "the line that gets edited in place";
    const std::string lineB = "the line edited in place instead";
    const std::vector<gitcore::WordSpan> spans = gitcore::word_spans(lineA, lineB);
    CHECK(!spans.empty());
    for (const gitcore::WordSpan& s : spans) {
        CHECK(s.aStart >= 0);
        CHECK(s.bStart >= 0);
        CHECK(static_cast<std::size_t>(s.aStart) + static_cast<std::size_t>(s.aLen) <= lineA.size());
        CHECK(static_cast<std::size_t>(s.bStart) + static_cast<std::size_t>(s.bLen) <= lineB.size());
        CHECK(s.aLen != 0 || s.bLen != 0); // a span always says something about one side
    }
}

TEST(the_fallback_answers_only_when_say_so_and_still_describes_the_input)
{
    const std::string textA = without_crlf(ols_test::read_fixture("diff/b1.txt"));
    const std::string textB = without_crlf(ols_test::read_fixture("diff/b2.txt"));

    gitcore::DiffOptions tight;
    tight.cellBudget = 8; // the trimmed core of this pair is 3x4: outside it
    const gitcore::DiffResult d = gitcore::diff_blobs(textA, textB, tight);
    CHECK(d.approx); // the search that could answer exactly was not afforded

    const gitcore::LineData a = gitcore::split_lines(textA);
    const gitcore::LineData b = gitcore::split_lines(textB);
    // The two sides are stated as text: what the harness prints for a failed
    // equality is a pair of values it can write, and lines it cannot.
    CHECK_EQ(as_text(apply_of(a.lines, d), true), as_text(b.lines, true));
}

// The budget is a promise about work, not about whether there is an answer.
// With nothing afforded at all, the exact search is refused at once, the
// fallback answers, and what it answers is still a true description of the
// pair — reached in one pass over the lines, which is the whole point of the
// number.
TEST(a_cell_budget_of_nothing_still_answers_and_describes_its_input)
{
    const std::vector<std::string> a = numbered(4000);
    std::vector<std::string> b = a;
    b[1999] = "changed here";
    b.insert(b.begin() + 3000, "inserted here");
    b.erase(b.begin());

    const std::string textA = as_text(a, true);
    const std::string textB = as_text(b, true);

    gitcore::DiffOptions none;
    none.cellBudget = 0;   // nothing at all is afforded
    const gitcore::DiffResult d = gitcore::diff_blobs(textA, textB, none);
    CHECK(d.approx);       // the exact answer was not afforded, and it says so
    CHECK_EQ(as_text(apply_of(a, d), true), textB);

    const std::string rendered = render_of(d, kNoRepoOid, kNoRepoOid);
    CHECK(!rendered.empty());
    CHECK(count_of(rendered, "changed here") == 1);
    CHECK(count_of(rendered, "+inserted here") == 1);
    CHECK(count_of(rendered, "-line1\n") == 1);
}

// Empty on both sides, and empty on one: the arithmetic of a document with no
// lines in it is where a diff can go wrong on a subtraction, and there is
// nothing here to subtract from.
TEST(documents_with_nothing_in_one_side_or_both_have_their_answer)
{
    const gitcore::DiffOptions def;

    const gitcore::DiffResult neither = gitcore::diff_blobs("", "", def);
    CHECK(neither.hunks.empty());
    CHECK(!neither.approx);
    CHECK_EQ(render_of(neither, kNoRepoOid, kNoRepoOid).size(), 0u);

    const gitcore::DiffResult onlyB = gitcore::diff_blobs("", "one\ntwo\n", def);
    CHECK_EQ(onlyB.aLineCount, 0u);
    CHECK_EQ(onlyB.bLineCount, 2u);
    CHECK_EQ(onlyB.hunks.size(), 1u);
    if (onlyB.hunks.size() == 1u) {
        CHECK_EQ(onlyB.hunks[0].countA, 0);
        CHECK_EQ(onlyB.hunks[0].countB, 2);
    }
    CHECK_EQ(as_text(apply_of({}, onlyB), true), "one\ntwo\n");

    const gitcore::DiffResult onlyA = gitcore::diff_blobs("one\ntwo\n", "", def);
    CHECK_EQ(onlyA.hunks.size(), 1u);
    if (onlyA.hunks.size() == 1u) {
        CHECK_EQ(onlyA.hunks[0].countA, 2);
        CHECK_EQ(onlyA.hunks[0].countB, 0);
    }
    CHECK_EQ(as_text(apply_of(gitcore::split_lines("one\ntwo\n").lines, onlyA), true), "");
}

// A pair that is one long run of the same line, with a single change inside it:
// the run is what a search over lines multiplies out, and here it is answered
// exactly, as one hunk, with the run standing as common on both sides.
TEST(a_long_run_of_identical_lines_is_one_block_around_its_change)
{
    std::vector<std::string> a(4000, "the same line over and over");
    std::vector<std::string> b = a;
    b[2000] = "one of them is not";

    const gitcore::DiffOptions def;
    const gitcore::DiffResult d =
        gitcore::diff_blobs(as_text(a, true), as_text(b, true), def);
    CHECK(!d.approx);
    CHECK_EQ(d.hunks.size(), 1u);
    if (d.hunks.size() == 1u) {
        CHECK_EQ(d.hunks[0].countA, 7);   // three lines of context each side of the change
        CHECK_EQ(d.hunks[0].countB, 7);
    }
    CHECK_EQ(as_text(apply_of(a, d), true), as_text(b, true));
}

// The line endings as the only difference: git calls every line of such a pair
// changed, and so does this — the carriage return is part of the line, which is
// what makes the pair differ at all. Neither side ends mid-line, so neither
// carries the marker.
TEST(a_pair_differing_only_in_its_line_endings_diffes_every_line)
{
    const gitcore::DiffOptions def;
    const gitcore::DiffResult d = gitcore::diff_blobs("alpha\r\nbeta\r\ngamma\r\n",
                                                      "alpha\nbeta\ngamma\n", def);
    CHECK_EQ(d.aNoNewline, false);
    CHECK_EQ(d.bNoNewline, false);
    CHECK_EQ(d.hunks.size(), 1u);
    if (d.hunks.size() != 1u) {
        return;
    }
    CHECK_EQ(d.hunks[0].countA, 3);
    CHECK_EQ(d.hunks[0].countB, 3);
    std::size_t removed = 0, added = 0;
    for (const gitcore::HunkLine& line : d.hunks[0].lines) {
        if (line.kind == 'd') {
            ++removed;
            CHECK(!line.text.empty() && line.text.back() == '\r');
        } else if (line.kind == 'a') {
            ++added;
            CHECK(line.text.empty() || line.text.back() != '\r');
        }
    }
    CHECK_EQ(removed, 3u);
    CHECK_EQ(added, 3u);
    claim_markers(render_of(d, kNoRepoOid, kNoRepoOid), 0);
}

// The spans of a line pair are the token grid of the two lines, which is their
// product, and a "line" is only what the bytes between two newlines are. Past
// what the grid is afforded the answer is the plain thing — these two lines
// differ, and the difference is somewhere inside them — rather than a
// reservation the pair is not worth.
TEST(a_line_pair_past_the_grid_affordance_answers_its_whole_lines)
{
    std::string lineA, lineB;
    for (int i = 0; i < 1000; ++i) {
        lineA += "word" + std::to_string(i) + " ";
        lineB += "word" + std::to_string(i) + " ";
    }
    lineA += "tail";
    lineB += "other tail";

    // Inside the affordance: the grid answers, and the one word that stands on
    // one side only is the one span there is.
    const std::vector<gitcore::WordSpan> inside = gitcore::word_spans(lineA, lineB);
    CHECK_EQ(inside.size(), 1u);
    for (const gitcore::WordSpan& s : inside) {
        CHECK(s.aStart >= 0);
        CHECK(s.bStart >= 0);
        CHECK(static_cast<std::size_t>(s.aStart) + static_cast<std::size_t>(s.aLen) <= lineA.size());
        CHECK(static_cast<std::size_t>(s.bStart) + static_cast<std::size_t>(s.bLen) <= lineB.size());
    }
    if (inside.size() == 1u) {
        CHECK_EQ(inside[0].aLen, 0);
        CHECK(inside[0].bLen != 0);
    }

    // Past it: 20001 tokens against 20002 is a grid of some four hundred million
    // cells, which the affordance refuses, and the pair is answered as whole
    // lines.
    std::string bigA, bigB;
    for (int i = 0; i < 20000; ++i) {
        bigA += "t" + std::to_string(i) + " ";
        bigB += "t" + std::to_string(i) + " ";
    }
    bigA += "one end";
    bigB += "the other end";
    const std::vector<gitcore::WordSpan> whole = gitcore::word_spans(bigA, bigB);
    CHECK_EQ(whole.size(), 1u);
    if (whole.size() == 1u) {
        CHECK_EQ(whole[0].aStart, 0);
        CHECK_EQ(static_cast<std::size_t>(whole[0].aLen), bigA.size());
        CHECK_EQ(whole[0].bStart, 0);
        CHECK_EQ(static_cast<std::size_t>(whole[0].bLen), bigB.size());
    }
    // And a pair that is one line has no spans whether or not the grid could
    // hold it: no difference, nothing to point at.
    CHECK(gitcore::word_spans(bigA, bigA).empty());
}

// A line is the bytes between two newlines together with the newline that
// ends them, and the last line of a text that has none is a different line
// from the same words with one — git's own reading. "x" against "x\n" is
// therefore one hunk, -x and +x, the side that lost the newline marked
// under its own line; a trailing-newline-only change is never an invisible
// commit. Bytes that agree answer zero hunks, and the carriage return is
// part of the line as ever: every fold of the flag is identity of the LAST
// line, not of the endings.
TEST(a_change_to_the_trailing_newline_alone_is_a_hunk)
{
    const gitcore::DiffResult grew = gitcore::diff_blobs("x", "x\n");
    CHECK(!grew.approx);
    CHECK_EQ(grew.hunks.size(), 1u);
    if (grew.hunks.size() == 1u) {
        CHECK_EQ(grew.hunks[0].startA, 1);
        CHECK_EQ(grew.hunks[0].countA, 1);
        CHECK_EQ(grew.hunks[0].startB, 1);
        CHECK_EQ(grew.hunks[0].countB, 1);
        CHECK_EQ(grew.hunks[0].lines.size(), 2u);
        if (grew.hunks[0].lines.size() == 2u) {
            CHECK_EQ(grew.hunks[0].lines[0].kind, 'd');
            CHECK_EQ(grew.hunks[0].lines[0].text, "x");
            CHECK_EQ(grew.hunks[0].lines[1].kind, 'a');
            CHECK_EQ(grew.hunks[0].lines[1].text, "x");
        }
    }
    // The rendered answer is what git says of this pair: the marker stands
    // under the side that ends mid-line — here the deletion, whose side a
    // has no final newline.
    const std::string grewRendered = render_of(grew, kNoRepoOid, kNoRepoOid);
    CHECK(grewRendered.find("@@ -1 +1 @@\n-x\n\\ No newline at end of file\n+x\n") !=
          std::string::npos);
    claim_markers(grewRendered, 1u);

    // The mirror, by the same token: the newline lost on the way out marks
    // the added line.
    const gitcore::DiffResult shrank = gitcore::diff_blobs("x\n", "x");
    CHECK_EQ(shrank.hunks.size(), 1u);
    const std::string shrankRendered = render_of(shrank, kNoRepoOid, kNoRepoOid);
    CHECK(shrankRendered.find("+x\n\\ No newline at end of file\n") != std::string::npos);
    claim_markers(shrankRendered, 1u);

    // Bytes that agree answer nothing at all — the fold must not invent work
    // out of equal inputs, with or without the newline.
    CHECK(gitcore::diff_blobs("x\n", "x\n").hunks.empty());
    CHECK(gitcore::diff_blobs("x", "x").hunks.empty());
    CHECK(gitcore::diff_blobs("", "").hunks.empty());

    // With a line above standing in the middle, the newline-only change
    // reads as an ordinary pair: the context holds, the last line leaves
    // changed, and the new side's missing newline carries its marker.
    const gitcore::DiffResult tail = gitcore::diff_blobs("a\nb\n", "a\nb");
    CHECK_EQ(tail.hunks.size(), 1u);
    if (tail.hunks.size() == 1u) {
        CHECK_EQ(tail.hunks[0].startA, 1);
        CHECK_EQ(tail.hunks[0].countA, 2);
        CHECK_EQ(tail.hunks[0].countB, 2);
    }
    const std::string tailRendered = render_of(tail, kNoRepoOid, kNoRepoOid);
    CHECK(tailRendered.find("@@ -1,2 +1,2 @@\n a\n-b\n+b\n\\ No newline at end of file\n") !=
          std::string::npos);

    // Empty against a line: a side that has a line is a change whether or
    // not it ends terminated.
    CHECK_EQ(gitcore::diff_blobs("", "x").hunks.size(), 1u);
    CHECK_EQ(gitcore::diff_blobs("x\n", "").hunks.size(), 1u);

    // A pair whose lines all differ through the carriage return still
    // differs on every line, endings equal on both sides and nothing to
    // fold: the fold is the last line's identity, not the EOL's.
    const gitcore::DiffResult crlf = gitcore::diff_blobs("a\r\nb\r\n", "a\nb\n");
    CHECK_EQ(crlf.hunks.size(), 1u);
    if (crlf.hunks.size() == 1u) {
        CHECK_EQ(crlf.hunks[0].countA, 2);
        CHECK_EQ(crlf.hunks[0].countB, 2);
    }
    claim_markers(render_of(crlf, kNoRepoOid, kNoRepoOid), 0u);
}

// The budget is one number, not two. The core the search may afford — the
// product of its cells — and the trace it stands up while walking answer
// to the same cellBudget: there is no second, unstated limit under which a
// search the header promised exactly would answer approx anyway. The trace
// of a 100×100 all-disjoint core is the walk's own 201 rows of 2*200+1 =
// 401 cells — 80,601 of them — and these three numbers bound that: just
// under, the walk is refused and the fallback answers, honestly; at it,
// the exact one does.
TEST(the_search_and_the_trace_answer_to_the_same_budget)
{
    std::vector<std::string> before;
    std::vector<std::string> after;
    for (int i = 0; i < 100; ++i) {
        before.push_back("gone line " + std::to_string(i));
        after.push_back("new line " + std::to_string(i));
    }
    const std::string textA = as_text(before, true);
    const std::string textB = as_text(after, true);

    // The core's product — 100×100 = 10,000 cells — stands well inside
    // both budgets below: what separates the two answers is the trace.
    gitcore::DiffOptions justBelow;
    justBelow.cellBudget = 80000;  // 199 rows of this width: the walk is refused
    const gitcore::DiffResult refused = gitcore::diff_blobs(textA, textB, justBelow);
    CHECK(refused.approx);  // the documented fallback, saying so
    CHECK_EQ(as_text(apply_of(before, refused), true), textB);  // and still a true description

    gitcore::DiffOptions atTheTrace;
    atTheTrace.cellBudget = 80601;  // the walk's 201 rows, exactly
    const gitcore::DiffResult exact = gitcore::diff_blobs(textA, textB, atTheTrace);
    CHECK(!exact.approx);  // inside both gates, and the answer is minimal
    CHECK_EQ(exact.hunks.size(), 1u);
    CHECK_EQ(as_text(apply_of(before, exact), true), textB);
}

// The domain of the search's numbering, stated at the guard itself: what
// the walk's int holds is the SPAN n+m — its width is 2*(n+m)+1 and its
// offset n+m — under 2^30 lines. The two sides taken apart were the old
// guard's undo: each under 2^30 passed it while max_d = n+m wrapped
// negative, the width wrapped with it, and the working set answered with
// an exception, the one thing this API does not do. A case cannot hand
// two billion lines to a diff — and needs none: the guard is this shape,
// and past the domain at any affordable budget the patience road answers,
// approx and honest, with no allocation to regret.
TEST(the_span_guard_refuses_the_overflowing_window_before_it_stands)
{
    const std::size_t big = static_cast<std::size_t>(1) << 30;
    CHECK(gitcore::detail::gridFitsSpan(0, 0));
    CHECK(gitcore::detail::gridFitsSpan(1000, 1000));
    CHECK(gitcore::detail::gridFitsSpan(big - 1, 0));
    CHECK(!gitcore::detail::gridFitsSpan(big, 0));  // width 2*(2^30)+1 passes the int
    CHECK(!gitcore::detail::gridFitsSpan(big - 1, 1));
    CHECK(!gitcore::detail::gridFitsSpan(big, big));      // the finding's pair
    CHECK(!gitcore::detail::gridFitsSpan(big / 2, big / 2));  // the sum, not the sides
    CHECK(gitcore::detail::gridFitsSpan(big / 2 - 1, big / 2));

    // Past the affordable domain at the default budget: the patience road
    // answers, and nothing throws.
    std::vector<std::string> many;
    for (int i = 0; i < 30000; ++i) {
        many.push_back("line " + std::to_string(i));
    }
    std::vector<std::string> other = many;
    for (int i = 0; i < 30000; i += 3) {
        other[i] = "moved " + std::to_string(i);
    }
    const gitcore::DiffResult wide =
        gitcore::diff_blobs(as_text(many, true), as_text(other, true));
    CHECK(wide.approx);  // 900,000,000 cells were never afforded
    CHECK(!wide.hunks.empty());
    CHECK_EQ(as_text(apply_of(many, wide), true), as_text(other, true));
}

// The counts a door reads off an approximate answer are not the counts the
// change holds. The patience fallback cuts whole windows the exact search
// matches line for line, so an approximate answer's hunks describe the
// pair truly while their numbers state far more change than the pair
// carries. This is the ground the changed[] rows of the history door stand
// on when a cut diff names a path as changed and states no counts: the
// hunks are honest about the pair, and silence is the law about numbers
// that are not facts.
TEST(an_approx_answer_describes_the_pair_and_overstates_its_counts)
{
    // Two files sharing a long unanchored middle: no line of it stands
    // exactly once on both sides, so patience finds no anchor inside the
    // core, cuts the whole window, and states its changes by the thousand
    // where the exact search sees one edited run at either end.
    std::vector<std::string> a;
    a.push_back("the files begin apart");
    for (int i = 0; i < 2100; ++i) {
        a.push_back("the same line over and over");
    }
    a.push_back("the files end apart");
    std::vector<std::string> b;
    b.push_back("the files begin apart, differently");
    for (int i = 0; i < 2099; ++i) {
        b.push_back("the same line over and over");
    }
    b.push_back("one of them is not");
    b.push_back("the files end apart, differently");
    const std::string textA = as_text(a, true);
    const std::string textB = as_text(b, true);

    // The default budget: a core of 2102×2102 cells stands past its
    // 4,000,000, the fallback answers, and it says so.
    const gitcore::DiffOptions def;
    const gitcore::DiffResult cut = gitcore::diff_blobs(textA, textB, def);
    CHECK(cut.approx);
    long long cutAdded = 0, cutDeleted = 0;
    for (const gitcore::Hunk& h : cut.hunks) {
        for (const gitcore::HunkLine& hl : h.lines) {
            if (hl.kind == 'a') {
                ++cutAdded;
            } else if (hl.kind == 'd') {
                ++cutDeleted;
            }
        }
    }
    CHECK_EQ(cutAdded, 2102LL);    // the whole window, cut
    CHECK_EQ(cutDeleted, 2102LL);
    CHECK_EQ(as_text(apply_of(a, cut), true), textB); // and it still describes the pair

    // The same core taken exactly — the budget the door would have had to
    // afford — counts what git counts: one line traded at either end and
    // one change inside, three each way, not two thousand each way.
    gitcore::DiffOptions afford;
    afford.cellBudget = 5000000; // the core's cells and the walk's trace stand inside
    const gitcore::DiffResult exact = gitcore::diff_blobs(textA, textB, afford);
    CHECK(!exact.approx);
    long long exactAdded = 0, exactDeleted = 0;
    for (const gitcore::Hunk& h : exact.hunks) {
        for (const gitcore::HunkLine& hl : h.lines) {
            if (hl.kind == 'a') {
                ++exactAdded;
            } else if (hl.kind == 'd') {
                ++exactDeleted;
            }
        }
    }
    CHECK_EQ(exactAdded, 3LL);
    CHECK_EQ(exactDeleted, 3LL);
    CHECK(cutAdded > exactAdded);      // the cut counts state more change
    CHECK(cutDeleted > exactDeleted);  // than the change: not a fact to state
}
