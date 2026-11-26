// tests/gitcore/main.cpp — the whole harness of the ols-git-tests target.
//
// A case states what a fixture proves, and never names itself to the runner:
//
//   TEST(loose_object_round_trip) {
//       CHECK_EQ(inflated.size(), expected_size);
//       CHECK(oid.rfind("a", 0) == 0);
//   }
//
// TEST registers the case. CHECK states a claim and reports the one that does
// not hold; CHECK_EQ states an equality and reports the two sides it found. The
// report is the counts and the failed claims — never a case that passed quietly
// while a claim was not made. The exit code is 1 when any case failed, so a
// build that runs this test stops on a broken claim.
//
// Nothing outside the standard library is asked of it, and nothing of the
// server is: the cases read the golden files beside them and state what
// git.exe said when they were generated.
//
// The macro set lives here, in one place. A test file is pulled into this
// translation unit below rather than compiled beside it: the include is what
// the compiler reads as a source dependency, so a new case file is one line
// added below and nothing in CMakeLists.txt at all.

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

// The crash guard stands in its own translation unit. windows.h defines `small`
// as `char`, and this file pulls the case files in below: an include here turns
// any case naming a variable small into a syntax error a hundred lines away
// from the include. crash_guard.cpp carries windows.h and keeps it out of reach.
namespace ols_test {
void install_crash_guard();
}

namespace ols_test {

// ---- the registry ----
// A case is named, and the vector of cases is asked for by name rather than
// stood as a global: a registrar in a test file runs before main, and a
// function-local static is initialized the first time it is asked for, so the
// registration of a case never depends on the order of statics across files.
struct Case
{
    std::string name;
    void (*run)();
};

static std::vector<Case>& cases()
{
    static std::vector<Case> all;
    return all;
}

struct Registrar
{
    Registrar(const std::string& name, void (*run)()) { cases().push_back({name, run}); }
};

// ---- the claims of one case ----
static std::string running_case;   // the case whose body is running
static int case_checks_failed;     // claims that failed, in the case running

static void claim(bool holds, const char* source, int line)
{
    if (holds) {
        return;
    }
    std::cout << "  " << running_case << " line " << line << ": "
              << source << " does not hold\n";
    ++case_checks_failed;
}

// Each side is stated as it was handed over, so a failed equality says what it
// found. The types are left open: the sides of a fixture claim are sizes, oids,
// and text, and all of them reach a stream.
template <class T, class U>
static void claim_eq(const char* left_source, const char* right_source,
                     const T& left, const U& right, int line)
{
    if (left == right) {
        return;
    }
    std::cout << "  " << running_case << " line " << line << ": "
              << left_source << " == " << right_source
              << ", found `" << left << "' and `" << right << "'\n";
    ++case_checks_failed;
}

// ---- where the fixtures of this folder stand ----
// Built in by CMakeLists.txt as the folder beside the target, so a case names a
// golden file by its own name and the build root does not matter. A path given
// to the program still outranks that.
#ifdef OLS_TEST_FIXTURE_DIR
static const std::string built_in_fixtures = OLS_TEST_FIXTURE_DIR;
#else
static const std::string built_in_fixtures;
#endif

static std::string fixtures_root = built_in_fixtures;

// A fixture read as its bytes stand on disk, whole file at once. An empty
// result is the one honest answer to a file that is not there; a case that
// asks for content then says the file is absent rather than claiming the
// content is not what it was. The read is binary on every platform: a fixture
// is LF-only by its own making, and a read that translated line endings would
// hide exactly the thing a case checks for.
static std::string read_fixture(const std::string& name)
{
    if (fixtures_root.empty()) {
        return std::string();
    }
    std::ifstream f(fixtures_root + "/" + name, std::ios::binary);
    if (!f) {
        return std::string();
    }
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

} // namespace ols_test

#define TEST(name)                                        \
    static void name();                                   \
    static ols_test::Registrar ols_register_##name(#name, name); \
    static void name()

#define CHECK(cond) ols_test::claim((cond), #cond, __LINE__)
#define CHECK_EQ(left, right)                                        \
    ols_test::claim_eq(#left, #right, (left), (right), __LINE__)

int main(int argc, char** argv)
{
    ols_test::install_crash_guard();
    if (argc > 1) {
        ols_test::fixtures_root = argv[1];
    }
    if (ols_test::cases().empty()) {
        std::cout << "no test case is registered: this target carries no case\n";
        return 1;
    }

    std::cout << "ols-git-tests: " << ols_test::cases().size() << " cases"
              << ", fixtures from "
              << (ols_test::fixtures_root.empty()
                      ? std::string("(none named)")
                      : ols_test::fixtures_root)
              << "\n";

    int passed = 0;
    int failed = 0;
    for (const ols_test::Case& c : ols_test::cases()) {
        ols_test::running_case = c.name;
        ols_test::case_checks_failed = 0;
        c.run();
        if (ols_test::case_checks_failed == 0) {
            std::cout << c.name << " ok\n";
            ++passed;
        } else {
            std::cout << c.name << ": " << ols_test::case_checks_failed
                      << " claim(s) do not hold\n";
            ++failed;
        }
    }

    std::cout << "PASS " << passed << " / FAIL " << failed << "\n";
    return failed == 0 ? 0 : 1;
}

// The cases of this folder, pulled in rather than compiled beside them: see the
// note at the top of this file. A test file stands beside this one and is named
// here once; Phase B adds the cases of objects, packfiles, refs, and diffs the
// same way. Nothing here depends on the order: the headers a case includes
// decide whose ObjectId stands, and each carries the guard that keeps the other
// out of the translation unit.
#include "test_objects.cpp"
#include "test_packfile.cpp"
#include "test_diff.cpp"
#include "test_refs.cpp"
#include "test_smoke.cpp"
#include "test_wire.cpp"
#include "test_receive.cpp"
#include "test_fetch_wire.cpp"
#include "test_merge.cpp"
#include "test_pulls.cpp"
#include "test_resolutions.cpp"
#include "test_issues.cpp"
#include "test_fetch.cpp"
// The ssh road's cases stand under _WIN32 inside their file: the pinned release
// they ride is the Windows side's, and the other platform reads an empty file.
#include "test_ssh.cpp"
