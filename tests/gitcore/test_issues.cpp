// tests/gitcore/test_issues.cpp — the cases that prove ProjectIssues.h's record law.
//
// No fixtures and no server: the records stand flat in a scratch folder under
// the system's temp path, the way a project's repo/ols/issues folder stands
// on disk. The claims are the ones the header makes: an id the machine prints
// that never comes back, a record that round-trips at its own bytes, a closed
// issue that keeps its file and stops counting open, the caps that name their
// cut, the filters that match by the plain rule, and the refusals at their
// exact words.

#include "ProjectIssues.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// A scratch repoDir of its own, under the temp path and removed after.
struct IssuesScratch
{
    std::string repoDir;
    IssuesScratch(const std::string &tag)
    {
        std::error_code ec{};
        const std::filesystem::path base =
            ols_test::scratchRoot() / ("ols-issues-" + tag);
        repoDir = base.string();
        std::filesystem::remove_all(base, ec);
    }
    ~IssuesScratch()
    {
        std::error_code ec{};
        std::filesystem::remove_all(std::filesystem::path(repoDir), ec);
    }
};

// Ids of the shape makeId("issue") prints — "issue-", 12 hex digits, a dash,
// 8 more — for the cases that write a file by hand.
const char kIssueIdOne[] = "issue-000000000001-00000002";
const char kIssueIdTwo[] = "issue-000000000002-00000003";

bool issuesMakeRecord(const std::string &id, const std::string &state,
                      nlohmann::json &record)
{
    record = nlohmann::json::object();
    record["id"] = id;
    record["project"] = "scr";
    record["title"] = "hand made";
    record["body"] = "";
    record["state"] = state;
    record["severity"] = "note";
    record["files"] = nlohmann::json::array();
    record["labels"] = nlohmann::json::array();
    record["created_at"] = 100;
    record["updated_at"] = 100;
    record["comments"] = nlohmann::json::array();
    record["links"] = {{"prs", nlohmann::json::array()},
                       {"issues", nlohmann::json::array()},
                       {"commits", nlohmann::json::array()}};
    return true;
}

// One piece of words out of a record or an answer, read so a case that lost
// an earlier claim fails its later claim instead of throwing.
std::string issuesWord(const nlohmann::json &at, const char *key)
{
    if (at.is_object() && at.contains(key) && at[key].is_string())
        return at[key].get<std::string>();
    return std::string();
}

} // anonymous namespace

TEST(issue_id_shape_never_comes_back)
{
    IssuesScratch scratch("ids");
    nlohmann::json one;
    nlohmann::json two;
    std::string error;
    CHECK(ols::issues::createIssue(scratch.repoDir, "scr", "first", "", "",
                                   nlohmann::json::array(), nlohmann::json::array(),
                                   one, error));
    // An empty severity reads "note", the record law's default.
    CHECK_EQ(std::string("note"), issuesWord(one, "severity"));
    CHECK(ols::issues::createIssue(scratch.repoDir, "scr", "second", "", "bug",
                                   nlohmann::json::array(), nlohmann::json::array(),
                                   two, error));
    const std::string idOne = issuesWord(one, "id");
    const std::string idTwo = issuesWord(two, "id");
    CHECK(idOne.rfind("issue-", 0) == 0);
    // "%012llx" prints twelve hex at minimum — the stamp grows past twelve
    // once the steady clock's count wears past 48 bits — so the law is
    // "at least 27", never "exactly 27".
    CHECK(idOne.size() >= 27u);
    CHECK(idOne != idTwo);
    // The shape is checked before any path is built.
    nlohmann::json readBack;
    CHECK(!ols::issues::readRecord(scratch.repoDir, "1", readBack, error));
    CHECK(!ols::issues::readRecord(scratch.repoDir, "issue-../x", readBack, error));
    CHECK(!ols::issues::writeRecord(scratch.repoDir, "not-an-id", one, error));
}

TEST(issue_record_round_trip_byte_stable)
{
    IssuesScratch scratch("round-trip");
    nlohmann::json record;
    issuesMakeRecord(kIssueIdOne, "open", record);
    std::string error;
    CHECK(ols::issues::writeRecord(scratch.repoDir, kIssueIdOne, record, error));
    nlohmann::json readBack;
    CHECK(ols::issues::readRecord(scratch.repoDir, kIssueIdOne, readBack, error));
    CHECK_EQ(record.dump(2), readBack.dump(2));
}

TEST(issue_close_keeps_the_file_not_counted_open)
{
    IssuesScratch scratch("close");
    nlohmann::json out;
    std::string error;
    CHECK(ols::issues::createIssue(scratch.repoDir, "scr", "windy", "", "bug",
                                   nlohmann::json::array(), nlohmann::json::array(),
                                   out, error));
    const std::string id = issuesWord(out, "id");
    nlohmann::json closed;
    CHECK(ols::issues::closeIssue(scratch.repoDir, id, closed, error));
    std::error_code ec{};
    CHECK(std::filesystem::exists(
        std::filesystem::path(scratch.repoDir + "/ols/issues/" + id + ".json"), ec));
    nlohmann::json readBack;
    CHECK(ols::issues::readRecord(scratch.repoDir, id, readBack, error));
    CHECK_EQ(std::string("closed"), issuesWord(readBack, "state"));
    nlohmann::json rows;
    nlohmann::json answer;
    CHECK(ols::issues::listIssues(scratch.repoDir, "", "open", 50, rows, answer, error));
    CHECK(rows.size() == 0u);
    CHECK(ols::issues::listIssues(scratch.repoDir, "", "closed", 50, rows, answer, error));
    CHECK(rows.size() == 1u);
    // The machine's own words for standing where it already stands.
    CHECK(!ols::issues::closeIssue(scratch.repoDir, id, closed, error));
    CHECK_EQ(std::string("the issue is already closed"), error);
    CHECK(ols::issues::reopenIssue(scratch.repoDir, id, closed, error));
    CHECK(!ols::issues::reopenIssue(scratch.repoDir, id, closed, error));
    CHECK_EQ(std::string("the issue is already open"), error);
}

TEST(issue_comment_cap_cut_is_named)
{
    IssuesScratch scratch("comments");
    nlohmann::json out;
    std::string error;
    CHECK(ols::issues::createIssue(scratch.repoDir, "scr", "chatty", "", "note",
                                   nlohmann::json::array(), nlohmann::json::array(),
                                   out, error));
    const std::string id = issuesWord(out, "id");
    for (int i = 0; i < 200; ++i)
    {
        if (!ols::issues::addComment(scratch.repoDir, id, "person", "note",
                                     nlohmann::json(), out, error))
        {
            break;
        }
    }
    nlohmann::json answer;
    CHECK(!ols::issues::addComment(scratch.repoDir, id, "person", "one more",
                                   nlohmann::json(), answer, error));
    CHECK(error.find("200 comments") != std::string::npos);
    CHECK(answer.is_object() && answer.contains("truncated"));
}

TEST(issue_unknown_state_word_is_named_not_counted)
{
    IssuesScratch scratch("bad-state");
    nlohmann::json out;
    std::string error;
    CHECK(ols::issues::createIssue(scratch.repoDir, "scr", "true one", "", "note",
                                   nlohmann::json::array(), nlohmann::json::array(),
                                   out, error));
    nlohmann::json bad;
    issuesMakeRecord(kIssueIdTwo, "awake", bad);
    CHECK(ols::issues::writeRecord(scratch.repoDir, kIssueIdTwo, bad, error));
    nlohmann::json rows;
    nlohmann::json answer;
    CHECK(ols::issues::listIssues(scratch.repoDir, "", "", 50, rows, answer, error));
    CHECK(rows.size() == 1u);
    CHECK(answer.contains("unparseable"));
    CHECK(answer.is_object() && answer.contains("unparseable")
              && answer["unparseable"].is_array()
              && answer["unparseable"].size() == 1u);
    if (answer.contains("unparseable") && answer["unparseable"].is_array()
        && answer["unparseable"].size() == 1u)
    {
        CHECK_EQ(std::string(kIssueIdTwo) + ".json",
                 answer["unparseable"][0].get<std::string>());
    }
}

TEST(issue_file_filter_matches_a_tag_path_exactly)
{
    IssuesScratch scratch("file-filter");
    nlohmann::json files = nlohmann::json::array();
    files.push_back({{"path", "src/a.cpp"}, {"line", 42}});
    nlohmann::json out;
    std::string error;
    CHECK(ols::issues::createIssue(scratch.repoDir, "scr", "tagged", "", "note",
                                   files, nlohmann::json::array(), out, error));
    nlohmann::json rows;
    nlohmann::json answer;
    CHECK(ols::issues::listIssues(scratch.repoDir, "src/a", "", 50, rows, answer, error));
    CHECK(rows.size() == 0u);
    CHECK(ols::issues::listIssues(scratch.repoDir, "src/a.cpp", "", 50, rows, answer,
                                  error));
    CHECK(rows.size() == 1u);
}

TEST(issue_edit_whitelist_refuses_links_field)
{
    IssuesScratch scratch("whitelist");
    nlohmann::json out;
    std::string error;
    CHECK(ols::issues::createIssue(scratch.repoDir, "scr", "editable", "", "note",
                                   nlohmann::json::array(), nlohmann::json::array(),
                                   out, error));
    const std::string id = issuesWord(out, "id");
    const nlohmann::json badPatch = {{"links", nlohmann::json::object()}};
    CHECK(!ols::issues::editIssue(scratch.repoDir, id, badPatch, out, error));
    CHECK(error.find("links") != std::string::npos);
    const nlohmann::json patch = {{"title", "retitled"}};
    CHECK(ols::issues::editIssue(scratch.repoDir, id, patch, out, error));
    CHECK_EQ(std::string("retitled"), issuesWord(out, "title"));
}

TEST(issue_link_refuses_unknown_kind)
{
    IssuesScratch scratch("link");
    nlohmann::json out;
    std::string error;
    CHECK(ols::issues::createIssue(scratch.repoDir, "scr", "linked", "", "note",
                                   nlohmann::json::array(), nlohmann::json::array(),
                                   out, error));
    const std::string id = issuesWord(out, "id");
    CHECK(!ols::issues::linkIssue(scratch.repoDir, id, "blob", "cafe0000", out, error));
    CHECK_EQ(std::string("the link kind is pr, issue, or commit"), error);
    CHECK(ols::issues::linkIssue(scratch.repoDir, id, "commit", "cafe0000", out, error));
    CHECK(out["links"]["commits"].size() == 1u);
    CHECK(ols::issues::linkIssue(scratch.repoDir, id, "pr", "7", out, error));
    CHECK(out["links"]["prs"].size() == 1u);
    CHECK(ols::issues::linkIssue(scratch.repoDir, id, "issue", kIssueIdOne, out, error));
    CHECK(out["links"]["issues"].size() == 1u);
}

TEST(issue_closed_takes_no_edit)
{
    IssuesScratch scratch("closed-edit");
    nlohmann::json out;
    std::string error;
    CHECK(ols::issues::createIssue(scratch.repoDir, "scr", "settled", "", "want",
                                   nlohmann::json::array(), nlohmann::json::array(),
                                   out, error));
    const std::string id = issuesWord(out, "id");
    nlohmann::json closed;
    CHECK(ols::issues::closeIssue(scratch.repoDir, id, closed, error));
    const nlohmann::json patch = {{"title", "later words"}};
    CHECK(!ols::issues::editIssue(scratch.repoDir, id, patch, out, error));
    CHECK_EQ(std::string("this issue is closed and takes no edit"), error);
    // A closed record takes no chatter either.
    CHECK(!ols::issues::addComment(scratch.repoDir, id, "person", "late",
                                   nlohmann::json(), out, error));
    CHECK_EQ(std::string("this issue is closed and takes no comments"), error);
}

TEST(issue_create_refuses_empty_title)
{
    IssuesScratch scratch("no-title");
    nlohmann::json out;
    std::string error;
    CHECK(!ols::issues::createIssue(scratch.repoDir, "scr", "", "", "note",
                                    nlohmann::json::array(), nlohmann::json::array(),
                                    out, error));
    CHECK_EQ(std::string("an issue is opened with a title"), error);
    CHECK(!ols::issues::createIssue(scratch.repoDir, "scr", "t", "", "urgent",
                                    nlohmann::json::array(), nlohmann::json::array(),
                                    out, error));
    CHECK_EQ(std::string("the severity is one of note, bug, want, blocker"), error);
}

// The rename law the issues door's re-anchor will lean on, pinned at the
// gitcore level: detect_exact_renames (gitcore/diff.h:158) run between the
// commit a tag was read at and the head as it stands now. What test_diff.cpp
// already pins — a plain rename pairs, equal-count pairing follows the
// listings, an unchanged path never pairs, a changed path with no arrival
// holding its content never pairs, and an identity listing answers none —
// is not repeated here. These four cover the shapes the re-anchor actually
// meets: a tag path recreated with new bytes whose old bytes moved to a new
// name, an unchanged path whose bytes also stand at a second name, and the
// one-to-many and many-to-one pairings the door must answer once,
// deterministically.

#include "gitcore/diff.h"

TEST(issue_reanchor_reused_path_follows_the_content_that_moved)
{
    // The tag's path stands in both listings wearing different bytes, and
    // the bytes it was read at arrived at a new name: the follow must
    // report the new path, not stop at absent or changed.
    const std::vector<ols::gitcore::ListingEntry> from = {{"a.txt", "oidA"}};
    const std::vector<ols::gitcore::ListingEntry> to = {{"a.txt", "oidB"},
                                                        {"b.txt", "oidA"}};
    const std::vector<ols::gitcore::RenamePair> pairs =
        ols::gitcore::detect_exact_renames(from, to);
    CHECK_EQ(pairs.size(), 1u);
    CHECK_EQ(pairs[0].from, "a.txt");
    CHECK_EQ(pairs[0].to, "b.txt");
}

TEST(issue_reanchor_unchanged_path_never_pairs_with_its_own_copy)
{
    // The path stands in both with the same oid, and the same bytes also
    // stand at a second name: the tag keeps its unchanged claim — no pair,
    // no rename note wearing a second path.
    const std::vector<ols::gitcore::ListingEntry> from = {{"a.txt", "oidA"}};
    const std::vector<ols::gitcore::ListingEntry> to = {{"a.txt", "oidA"},
                                                        {"b.txt", "oidA"}};
    CHECK(ols::gitcore::detect_exact_renames(from, to).empty());
}

TEST(issue_reanchor_one_departure_two_arrivals_pair_the_first_arrival_named)
{
    // One path left, two arrived holding it: the choice is the first
    // arrival in listing order — the door's rename note names one path,
    // deterministically, never both.
    const std::vector<ols::gitcore::ListingEntry> from = {{"a.txt", "oidA"}};
    const std::vector<ols::gitcore::ListingEntry> to = {{"b.txt", "oidA"},
                                                        {"c.txt", "oidA"}};
    const std::vector<ols::gitcore::RenamePair> pairs =
        ols::gitcore::detect_exact_renames(from, to);
    CHECK_EQ(pairs.size(), 1u);
    CHECK_EQ(pairs[0].from, "a.txt");
    CHECK_EQ(pairs[0].to, "b.txt");
}

TEST(issue_reanchor_two_departures_one_arrival_pairs_only_the_first)
{
    // Two left holding the same bytes, one arrived: an arrival is
    // consumed once — the first departure in listing order pairs, the
    // second stays unpaired, so the door never claims two renames of
    // one arrival.
    const std::vector<ols::gitcore::ListingEntry> from = {{"a.txt", "oidA"},
                                                          {"d.txt", "oidA"}};
    const std::vector<ols::gitcore::ListingEntry> to = {{"b.txt", "oidA"}};
    const std::vector<ols::gitcore::RenamePair> pairs =
        ols::gitcore::detect_exact_renames(from, to);
    CHECK_EQ(pairs.size(), 1u);
    CHECK_EQ(pairs[0].from, "a.txt");
    CHECK_EQ(pairs[0].to, "b.txt");
}
