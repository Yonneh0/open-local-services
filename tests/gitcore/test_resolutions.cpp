// tests/gitcore/test_resolutions.cpp — the cases that prove the side file law.
//
// The side file beside the record: <repoDir>/ols/pulls/<n>/resolutions.json.
// The claims are the ones ProjectPulls.h makes — the file round-trips whole,
// the item cap refuses naming 200, and setResolution replaces by path rather
// than adding a second entry — and that the listing never sees the file:
// nextNumber still answers from the folders alone.
//
// The helpers are named ResolutionsScratch-… : this file is pulled into the
// unity translation unit below, so nothing here shares a name with another
// case file.

#include "ProjectPulls.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace {

// A scratch repoDir of its own, under the temp path and removed after.
struct ResolutionsScratch
{
    std::string repoDir;
    ResolutionsScratch(const std::string &tag)
    {
        std::error_code ec{};
        const std::filesystem::path base =
            std::filesystem::temp_directory_path(ec) / ("ols-resolutions-" + tag);
        repoDir = base.string();
        std::filesystem::remove_all(base, ec);
    }
    ~ResolutionsScratch()
    {
        std::error_code ec{};
        std::filesystem::remove_all(std::filesystem::path(repoDir), ec);
    }
};

// One item, as the side file shape states it.
nlohmann::json resolutionsMakeItem(const std::string &path, const std::string &content,
                                   const std::string &action)
{
    nlohmann::json item = nlohmann::json::object();
    item["path"] = path;
    item["action"] = action;
    if (action == "settle")
        item["content"] = content;
    item["at"] = 100;
    item["by"] = "studio";
    return item;
}

} // anonymous namespace

TEST(resolutions_side_round_trip)
{
    ResolutionsScratch scratch("side");
    nlohmann::json out;
    std::string error;
    CHECK(ols::pulls::createPull(scratch.repoDir, "scr", "feat", "main",
                                 "first", "", "aaaa", "bbbb", out, error));
    nlohmann::json items = nlohmann::json::array();
    items.push_back(resolutionsMakeItem("a.txt", "x\n", "settle"));
    nlohmann::json written;
    CHECK(ols::pulls::writeResolutions(scratch.repoDir, 1, "aaaa", "bbbb", items,
                                       written, error));
    nlohmann::json held;
    CHECK(ols::pulls::readResolutions(scratch.repoDir, 1, held, error));
    CHECK(held.is_object() && held.contains("items") && held["items"].is_array() &&
          held["items"].size() == 1u);
    CHECK(held.contains("at") && held["at"].is_object() &&
          held["at"]["head"] == "aaaa" && held["at"]["base"] == "bbbb");
    // The listing never sees the side file: the number still answers from the
    // folders alone.
    CHECK(ols::pulls::nextNumber(scratch.repoDir) == 2);
}

TEST(resolutions_item_cap_refused)
{
    ResolutionsScratch scratch("cap");
    nlohmann::json items = nlohmann::json::array();
    for (int i = 0; i < 201; ++i)
    {
        items.push_back(resolutionsMakeItem("p" + std::to_string(i), "x", "settle"));
    }
    nlohmann::json written;
    std::string error;
    CHECK(!ols::pulls::writeResolutions(scratch.repoDir, 1, "h", "b", items,
                                        written, error));
    CHECK(error.find("200") != std::string::npos);
}

TEST(resolutions_replace_by_path_keeps_one)
{
    ResolutionsScratch scratch("replace");
    nlohmann::json out;
    std::string error;
    CHECK(ols::pulls::createPull(scratch.repoDir, "scr", "feat", "main",
                                 "first", "", "aaaa", "bbbb", out, error));
    CHECK(ols::pulls::setResolution(scratch.repoDir, 1, "a.txt", "one", false,
                                    "studio", out, error));
    CHECK(ols::pulls::setResolution(scratch.repoDir, 1, "a.txt", "two", false,
                                    "studio", out, error));
    nlohmann::json held;
    CHECK(ols::pulls::readResolutions(scratch.repoDir, 1, held, error));
    CHECK(held.is_object() && held.contains("items") && held["items"].is_array() &&
          held["items"].size() == 1u);
    CHECK(held["items"][0]["content"] == "two");
}

// The resolve round's live-verified-but-unpinned law: the stamps of the
// side file survive across two writes — a first file stamps from the
// record's lease (the oids at open, ProjectPulls.h:201-204) and the
// stamps of an existing file are kept (ProjectPulls.h:201) — and
// clearResolutions on a pull that was never resolved answers true with
// out null (ProjectPulls.h:211): there was nothing to clear, and
// nothing is written.

TEST(resolutions_stamps_survive_two_writes)
{
    ResolutionsScratch scratch("stamps");
    nlohmann::json out;
    std::string error;
    CHECK(ols::pulls::createPull(scratch.repoDir, "scr", "feat", "main",
                                 "first", "", "aaaa", "bbbb", out, error));
    // The first file stamps from the record's lease: the oids at open.
    CHECK(ols::pulls::setResolution(scratch.repoDir, 1, "a.txt", "one", false,
                                    "studio", out, error));
    nlohmann::json firstHeld;
    CHECK(ols::pulls::readResolutions(scratch.repoDir, 1, firstHeld, error));
    CHECK(firstHeld.is_object() && firstHeld.contains("at") &&
          firstHeld["at"]["head"] == "aaaa" && firstHeld["at"]["base"] == "bbbb");
    // The second write keeps them: the door weighs at.head/at.base against
    // the live lease at merge time, so a stamp lost between writes would
    // break a merge that was resolved honestly.
    CHECK(ols::pulls::setResolution(scratch.repoDir, 1, "b.txt", "two", false,
                                    "studio", out, error));
    nlohmann::json held;
    CHECK(ols::pulls::readResolutions(scratch.repoDir, 1, held, error));
    CHECK(held["at"]["head"] == firstHeld["at"]["head"] &&
          held["at"]["base"] == firstHeld["at"]["base"]);
    CHECK(held["items"].size() == 2u);
}

TEST(resolutions_clear_never_resolved_answers_true_null)
{
    ResolutionsScratch scratch("never-resolved");
    nlohmann::json out;
    std::string error;
    CHECK(ols::pulls::createPull(scratch.repoDir, "scr", "feat", "main",
                                 "first", "", "aaaa", "bbbb", out, error));
    // A pull with no side file is already clear: true with out null —
    // and nothing is written, so the reading still answers from absence.
    nlohmann::json cleared;
    CHECK(ols::pulls::clearResolutions(scratch.repoDir, 1, cleared, error));
    CHECK(cleared.is_null());
    nlohmann::json held;
    CHECK(ols::pulls::readResolutions(scratch.repoDir, 1, held, error));
    CHECK(held.is_null());
}
