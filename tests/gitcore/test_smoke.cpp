// tests/gitcore/test_smoke.cpp — the placeholder cases of the harness.
//
// They are the first thing Phase 0 asks to prove: that a case runs, that a
// claim is reported when it does not hold, that the counts and the exit code
// answer to it, and that a case can open a golden file below fixtures/ and
// read what is really on disk. They state nothing about GitCore, which has no
// code yet. The real cases arrive with Phase B beside the objects, packfiles,
// refs, and diffs they exercise.
//
// The harness is pulled in by main.cpp, so TEST, CHECK, and CHECK_EQ are
// already in sight, and so is ols_test::read_fixture.

#include <string>

namespace {

// A small question asked of the bytes of a fixture: how often a byte stands in
// them. The fixtures of this folder are LF-only by their own making, so a
// carriage return in one is a fixture that was not normalized, and a case that
// reads it should say so.
std::size_t count_byte(const std::string& bytes, const char byte)
{
    std::size_t found = 0;
    for (const char c : bytes) {
        if (c == byte) {
            ++found;
        }
    }
    return found;
}

} // anonymous namespace

// The first placeholder: the claims a case can make, and that the harness
// answers to each of them. An equality over text is the common shape of a
// fixture claim — an object header, an oid, a type — and a size is the other.
TEST(smoke_claims_answer)
{
    const std::string header = "blob 5";
    CHECK_EQ(header.substr(0, 4), "blob");
    CHECK_EQ(header.size(), 6u);
    CHECK(header.find("tree") == std::string::npos);
    CHECK_EQ(header.compare(5, 1, "5"), 0);
}

// The second placeholder: a golden file of this folder opened and read as it
// stands on disk. Nothing of a fixture is asserted about its meaning here —
// Phase B states that — only that the file is there, is not empty, and carries
// the line endings it was written with.
TEST(smoke_fixture_reads)
{
    const std::string manifest = ols_test::read_fixture("loose-manifest.json");
    CHECK(!manifest.empty());
    CHECK_EQ(manifest.substr(0, 1), "{");
    CHECK(manifest.find("\"objects\"") != std::string::npos);
    CHECK_EQ(count_byte(manifest, '\r'), 0u);

    const std::string text_a = ols_test::read_fixture("diff/a1.txt");
    const std::string diff = ols_test::read_fixture("diff/a1-a2.diff");
    CHECK(!text_a.empty());
    CHECK(!diff.empty());
    CHECK_EQ(diff.substr(0, 10), "diff --git");
    CHECK_EQ(count_byte(text_a, '\r'), 0u);
    CHECK_EQ(count_byte(diff, '\r'), 0u);
}
