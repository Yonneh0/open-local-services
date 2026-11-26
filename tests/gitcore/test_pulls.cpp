// tests/gitcore/test_pulls.cpp — the cases that prove ProjectPulls.h's record law.
//
// No fixtures and no server: the records stand in a scratch folder under the
// system's temp path, the way a project's repo/ folder stands on disk. The
// claims are the ones the header makes: numbers that never come back, a
// closed pull that keeps its folder, a record that round-trips at its own
// bytes, the state machine from open to merged, the caps that name their
// cut, and the refusals at their exact words.

#include "ProjectPulls.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// A scratch repoDir of its own, under the temp path and removed after.
struct PullsScratch
{
    std::string repoDir;
    PullsScratch(const std::string &tag)
    {
        std::error_code ec{};
        const std::filesystem::path base =
            std::filesystem::temp_directory_path(ec) / ("ols-pulls-" + tag);
        repoDir = base.string();
        std::filesystem::remove_all(base, ec);
    }
    ~PullsScratch()
    {
        std::error_code ec{};
        std::filesystem::remove_all(std::filesystem::path(repoDir), ec);
    }
};

bool pullsMakeRecord(const std::string &title, const std::string &head,
                     const std::string &base, nlohmann::json &record)
{
    record = nlohmann::json::object();
    record["number"] = 1;
    record["project"] = "scr";
    record["title"] = title;
    record["body"] = "";
    record["state"] = "open";
    record["head"] = {{"branch", head}, {"oidAtOpen", "a"} };
    record["base"] = {{"branch", base}, {"oidAtOpen", "b"} };
    record["created_at"] = 100;
    record["updated_at"] = 100;
    record["reviews"] = nlohmann::json::array();
    record["approvals"] = nlohmann::json::array();
    record["comments"] = nlohmann::json::array();
    record["links"] = {{"issues", nlohmann::json::array()} };
    return true;
}

// The state word a record wears, read the way the record law states it.
std::string pullsStateWord(const nlohmann::json &record)
{
    if (record.is_object() && record.contains("state") && record["state"].is_string())
        return record["state"].get<std::string>();
    return std::string();
}

} // anonymous namespace

TEST(pull_number_never_comes_back)
{
    PullsScratch scratch("numbers");
    nlohmann::json out;
    std::string error;
    CHECK(ols::pulls::nextNumber(scratch.repoDir) == 1);
    CHECK(ols::pulls::createPull(scratch.repoDir, "scr", "feat", "main",
                                 "first", "", "aaaa", "bbbb", out, error));
    CHECK(ols::pulls::nextNumber(scratch.repoDir) == 2);
    CHECK(ols::pulls::createPull(scratch.repoDir, "scr", "other", "main",
                                 "second", "", "cccc", "dddd", out, error));
    CHECK(ols::pulls::nextNumber(scratch.repoDir) == 3);
    nlohmann::json closed;
    CHECK(ols::pulls::closePull(scratch.repoDir, 1, closed, error));
    // The closed pull keeps its folder, so the next number is still max+1.
    CHECK(ols::pulls::nextNumber(scratch.repoDir) == 3);
}

TEST(pull_close_keeps_the_folder)
{
    PullsScratch scratch("folder");
    nlohmann::json out;
    std::string error;
    CHECK(ols::pulls::createPull(scratch.repoDir, "scr", "feat", "main",
                                 "keep me", "", "aaaa", "bbbb", out, error));
    nlohmann::json closed;
    CHECK(ols::pulls::closePull(scratch.repoDir, 1, closed, error));
    nlohmann::json readBack;
    CHECK(ols::pulls::readRecord(scratch.repoDir, 1, readBack, error));
    CHECK_EQ(std::string("closed"), pullsStateWord(readBack));
}

TEST(pull_record_round_trip_byte_stable)
{
    PullsScratch scratch("round-trip");
    nlohmann::json record;
    pullsMakeRecord("stable", "feat", "main", record);
    std::string error;
    CHECK(ols::pulls::writeRecord(scratch.repoDir, 7, record, error));
    nlohmann::json readBack;
    CHECK(ols::pulls::readRecord(scratch.repoDir, 7, readBack, error));
    CHECK_EQ(record.dump(2), readBack.dump(2));
}

TEST(pull_state_machine_open_approve_merged)
{
    PullsScratch scratch("machine");
    nlohmann::json out;
    std::string error;
    CHECK(ols::pulls::createPull(scratch.repoDir, "scr", "feat", "main",
                                 "ship it", "", "aaaa", "bbbb", out, error));
    CHECK(ols::pulls::addReview(scratch.repoDir, 1, "reviewer-one",
                                ols::pulls::ReviewVerdict::Approve, "yes", out, error));
    CHECK(ols::pulls::approvalMet(scratch.repoDir, 1, 1));
    nlohmann::json merged;
    CHECK(ols::pulls::markMerged(scratch.repoDir, 1, "merge", "cafe0000",
                                 "reviewer-one", merged, error));
    CHECK_EQ(std::string("merged"), pullsStateWord(merged));
}

TEST(pull_threshold_not_met_below_the_ask)
{
    PullsScratch scratch("threshold");
    nlohmann::json out;
    std::string error;
    CHECK(ols::pulls::createPull(scratch.repoDir, "scr", "feat", "main",
                                 "needs two", "", "aaaa", "bbbb", out, error));
    CHECK(ols::pulls::addReview(scratch.repoDir, 1, "one",
                                ols::pulls::ReviewVerdict::Approve, "", out, error));
    CHECK(!ols::pulls::approvalMet(scratch.repoDir, 1, 2));
    CHECK(ols::pulls::addReview(scratch.repoDir, 1, "two",
                                ols::pulls::ReviewVerdict::Approve, "", out, error));
    CHECK(ols::pulls::approvalMet(scratch.repoDir, 1, 2));
}

TEST(pull_comment_cap_cut_is_named)
{
    PullsScratch scratch("comments");
    nlohmann::json out;
    std::string error;
    CHECK(ols::pulls::createPull(scratch.repoDir, "scr", "feat", "main",
                                 "chatty", "", "aaaa", "bbbb", out, error));
    for (int i = 0; i < 200; ++i)
    {
        if (!ols::pulls::addComment(scratch.repoDir, 1, "person", "note",
                                    nlohmann::json(), out, error))
        {
            break;
        }
    }
    nlohmann::json answer;
    CHECK(!ols::pulls::addComment(scratch.repoDir, 1, "person", "one more",
                                  nlohmann::json(), answer, error));
    CHECK(error.find("200 comments") != std::string::npos);
    CHECK(answer.is_object() && answer.contains("truncated"));
}

TEST(pull_merged_takes_no_review)
{
    PullsScratch scratch("merged-review");
    nlohmann::json out;
    std::string error;
    CHECK(ols::pulls::createPull(scratch.repoDir, "scr", "feat", "main",
                                 "done", "", "aaaa", "bbbb", out, error));
    nlohmann::json merged;
    CHECK(ols::pulls::markMerged(scratch.repoDir, 1, "squash", "cafe0000",
                                 "someone", merged, error));
    CHECK(!ols::pulls::addReview(scratch.repoDir, 1, "late",
                                 ols::pulls::ReviewVerdict::Approve, "", out, error));
    CHECK_EQ(std::string("this pull is merged and takes no more reviews"), error);
}

TEST(pull_merged_will_not_reopen)
{
    PullsScratch scratch("reopen");
    nlohmann::json out;
    std::string error;
    CHECK(ols::pulls::createPull(scratch.repoDir, "scr", "feat", "main",
                                 "done", "", "aaaa", "bbbb", out, error));
    nlohmann::json merged;
    CHECK(ols::pulls::markMerged(scratch.repoDir, 1, "merge", "cafe0000",
                                 "someone", merged, error));
    CHECK(!ols::pulls::reopenPull(scratch.repoDir, 1, out, error));
    CHECK_EQ(std::string("the pull is merged and will not reopen"), error);
}

TEST(pull_count_open_blocks_and_clears)
{
    // The count a project delete weighs: open pulls hold it, closed and
    // merged ones let go, and the folder's absence is a true zero.
    PullsScratch scratch("count-open");
    nlohmann::json out;
    std::string error;
    long long open = -1;
    CHECK(ols::pulls::countOpenPulls(scratch.repoDir, open, error));
    CHECK_EQ(0ll, open);
    CHECK(ols::pulls::createPull(scratch.repoDir, "scr", "feat", "main",
                                 "one", "", "aaaa", "bbbb", out, error));
    CHECK(ols::pulls::createPull(scratch.repoDir, "scr", "two", "main",
                                 "two", "", "cccc", "dddd", out, error));
    CHECK(ols::pulls::countOpenPulls(scratch.repoDir, open, error));
    CHECK_EQ(2ll, open);
    nlohmann::json closed;
    CHECK(ols::pulls::closePull(scratch.repoDir, 1, closed, error));
    CHECK(ols::pulls::countOpenPulls(scratch.repoDir, open, error));
    CHECK_EQ(1ll, open);
    nlohmann::json merged;
    CHECK(ols::pulls::markMerged(scratch.repoDir, 2, "merge", "cafe0000",
                                 "someone", merged, error));
    CHECK(ols::pulls::countOpenPulls(scratch.repoDir, open, error));
    CHECK_EQ(0ll, open);
}
