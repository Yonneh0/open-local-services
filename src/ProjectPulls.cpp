// src/ProjectPulls.cpp — the record law beside the reader that already stands.
//
// openPullsAt (ProjectService.cpp:687-720) walks <repoDir>/ols/pulls and
// counts the records whose state reads "open"; this file is the writer that
// keeps that folder honest, the state machine a record moves through, and the
// list a dropdown asks for. Nothing here reaches a door, a page, or a
// ProjectService header: every function takes the repoDir, and the caller
// holds the project's mutex and bumps the rev after a true answer — the
// record first, the mark after, the order writeProjectFiles keeps at
// ProjectService.cpp:437-445.
//
// One record is one writeFileText (Common.h:176-205): a temp beside the
// target and a rename, so a reader never sees half a record. The folder
// comes with the write (ensureParentDir, Common.h:157-165), never with the
// start. A record past 1 MiB is refused with words before anything is
// written — openPullsAt reads every record.json on every dropdown read, so
// the record stays small, and nothing derived is ever stored in it.

#include "ProjectPulls.h"

#include "Common.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ols
{
    namespace pulls
    {

    // ---------------------------------------------------------------- the shape

    // The folder the records of one repo stand in, beside the reader's path
    // at ProjectService.cpp:689.
    static std::string pullsDir(const std::string &repoDir)
    {
        return repoDir + "/ols/pulls";
    }

    static std::string recordPath(const std::string &repoDir, long long n)
    {
        return pullsDir(repoDir) + "/" + std::to_string(n) + "/record.json";
    }

    // A record past this is not this module's record: the words are refused
    // with the size named, and nothing is written.
    static const std::size_t gRecordBytes = 1024u * 1024u;

    // The caps the record law keeps: comments of at most 4 KiB each, at most
    // 200 of them, and at most 200 reviews. Past a cap the new entry is cut
    // and the cut is named in words.
    static const long long gMaxComments = 200;
    static const std::size_t gCommentBytes = 4096u;
    static const long long gMaxReviews = 200;

    // ------------------------------------------------------------------ records

    bool readRecord(const std::string &repoDir, long long n,
                    nlohmann::json &record, std::string &error)
    {
        record = nullptr;
        const std::string path = recordPath(repoDir, n);
        std::string body;
        if (!readFileText(path, body))
        {
            error = "the record of pull " + std::to_string(n) + " is not there to read";
            return false;
        }
        const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
        if (parsed.is_discarded())
        {
            error = "the record of pull " + std::to_string(n) +
                    " would not parse: " + path;
            return false;
        }
        if (!parsed.is_object())
        {
            error = "the record of pull " + std::to_string(n) +
                    " is not a record: " + path;
            return false;
        }
        record = parsed;
        error.clear();
        return true;
    }

    bool writeRecord(const std::string &repoDir, long long n,
                     const nlohmann::json &record, std::string &error)
    {
        const std::string text = record.dump(2);
        if (text.size() > gRecordBytes)
        {
            error = "this record wears " + std::to_string(text.size()) +
                    " bytes, past the 1 MiB a record is kept at";
            return false;
        }
        if (!writeFileText(recordPath(repoDir, n), text))
        {
            error = "the record file would not take the write";
            return false;
        }
        error.clear();
        return true;
    }

    // ----------------------------------------------------------------- numbers

    // One past the highest folder the listing shows, from 1 when the folder
    // is not there yet. Only a folder wearing a whole number counts; a
    // closed pull keeps its folder, so the gaps survive and the answer is
    // max+1, never a count.
    static long long nextNumberAt(const std::string &repoDir)
    {
        const std::string dir = pullsDir(repoDir);
        std::error_code ec{};
        if (!std::filesystem::is_directory(dir, ec))
            return 1;
        long long highest = 0;
        ec = std::error_code{};
        std::filesystem::directory_iterator it(dir, ec);
        if (ec)
            return 1;
        for (; it != std::filesystem::directory_iterator{}; it.increment(ec))
        {
            if (ec)
                break;
            std::error_code kind{};
            if (!it->is_directory(kind) || kind)
                continue;
            const std::string name = it->path().filename().string();
            if (name.empty())
                continue;
            bool digits = true;
            for (char c : name)
            {
                if (c < '0' || c > '9')
                {
                    digits = false;
                    break;
                }
            }
            if (!digits)
                continue;
            long long shown = 0;
            bool overflow = false;
            for (char c : name)
            {
                if (shown > 1000000000000ll)
                {
                    overflow = true;
                    break;
                }
                shown = shown * 10 + (c - '0');
            }
            if (overflow)
                continue;
            if (shown > highest)
                highest = shown;
        }
        return highest + 1;
    }

    long long nextNumber(const std::string &repoDir)
    {
        return nextNumberAt(repoDir);
    }

    // ------------------------------------------------------------- open a pull

    bool createPull(const std::string &repoDir, const std::string &slug,
                    const std::string &headBranch, const std::string &baseBranch,
                    const std::string &title, const std::string &body,
                    const std::string &headOid, const std::string &baseOid,
                    nlohmann::json &out, std::string &error)
    {
        out = nullptr;
        if (title.empty())
        {
            error = "a pull is opened with a title";
            return false;
        }
        if (headBranch == baseBranch)
        {
            error = "the head and the base are the same branch: there is nothing to pull";
            return false;
        }
        if (headOid == baseOid)
        {
            error = "the head already stands at the base: there is nothing to pull";
            return false;
        }
        const long long n = nextNumberAt(repoDir);
        const long long at = nowSeconds();
        nlohmann::json record = nlohmann::json::object();
        record["number"] = n;
        record["project"] = slug;
        record["title"] = title;
        record["body"] = body;
        record["state"] = "open";
        record["head"] = {{"branch", headBranch}, {"oidAtOpen", headOid}};
        record["base"] = {{"branch", baseBranch}, {"oidAtOpen", baseOid}};
        record["created_at"] = at;
        record["updated_at"] = at;
        record["reviews"] = nlohmann::json::array();
        record["approvals"] = nlohmann::json::array();
        record["comments"] = nlohmann::json::array();
        record["links"] = {{"issues", nlohmann::json::array()}};
        if (!writeRecord(repoDir, n, record, error))
            return false;
        out = record;
        return true;
    }

    // ---------------------------------------------------------- the state word

    // The state a record wears, as its word. Anything the record does not
    // state reads as empty, and the machine refuses where it stands.
    static std::string stateOf(const nlohmann::json &record)
    {
        return statedString(record, "state");
    }

    // ------------------------------------------------------------- review a pull

    bool addReview(const std::string &repoDir, long long n,
                   const std::string &who, ReviewVerdict verdict,
                   const std::string &body, nlohmann::json &out,
                   std::string &error)
    {
        out = nullptr;
        nlohmann::json record;
        if (!readRecord(repoDir, n, record, error))
            return false;
        const std::string state = stateOf(record);
        if (state == "merged")
        {
            error = "this pull is merged and takes no more reviews";
            return false;
        }
        if (state != "open" && state != "closed")
        {
            error = "this record states no state the machine knows";
            return false;
        }
        if (!record.contains("reviews") || !record["reviews"].is_array())
            record["reviews"] = nlohmann::json::array();
        if (static_cast<long long>(record["reviews"].size()) >= gMaxReviews)
        {
            error = "this pull already carries " + std::to_string(gMaxReviews) +
                    " reviews; the new review is cut";
            return false;
        }
        const long long at = nowSeconds();
        const char *word = "comment";
        if (verdict == ReviewVerdict::Approve)
            word = "approve";
        else if (verdict == ReviewVerdict::RequestChanges)
            word = "request-changes";
        record["reviews"].push_back(
            {{"who", who}, {"verdict", word}, {"at", at}, {"body", body}});
        if (!record.contains("approvals") || !record["approvals"].is_array())
            record["approvals"] = nlohmann::json::array();
        if (verdict == ReviewVerdict::Approve)
        {
            bool standing = false;
            for (const nlohmann::json &held : record["approvals"])
            {
                if (held.is_object() && statedString(held, "who") == who)
                {
                    standing = true;
                    break;
                }
            }
            if (!standing)
                record["approvals"].push_back({{"who", who}, {"at", at}});
        }
        else if (verdict == ReviewVerdict::RequestChanges)
        {
            nlohmann::json kept = nlohmann::json::array();
            for (const nlohmann::json &held : record["approvals"])
            {
                if (held.is_object() && statedString(held, "who") == who)
                    continue;
                kept.push_back(held);
            }
            record["approvals"] = kept;
        }
        record["updated_at"] = at;
        if (!writeRecord(repoDir, n, record, error))
            return false;
        out = record;
        return true;
    }

    // ------------------------------------------------------------ comment a pull

    bool addComment(const std::string &repoDir, long long n,
                    const std::string &who, const std::string &body,
                    const nlohmann::json &anchor, nlohmann::json &out,
                    std::string &error)
    {
        out = nullptr;
        nlohmann::json record;
        if (!readRecord(repoDir, n, record, error))
            return false;
        const std::string state = stateOf(record);
        if (state == "merged")
        {
            error = "this pull is merged and takes no more comments";
            return false;
        }
        if (state != "open" && state != "closed")
        {
            error = "this record states no state the machine knows";
            return false;
        }
        if (!anchor.is_null() && !anchor.is_object())
        {
            error = "an anchor is the place a comment stands, or nothing";
            return false;
        }
        if (body.size() > gCommentBytes)
        {
            error = "this comment wears " + std::to_string(body.size()) +
                    " bytes, past the 4 KiB a comment is kept at";
            return false;
        }
        if (!record.contains("comments") || !record["comments"].is_array())
            record["comments"] = nlohmann::json::array();
        if (static_cast<long long>(record["comments"].size()) >= gMaxComments)
        {
            out = record;
            out["truncated"] = true;
            error = "this pull already carries " + std::to_string(gMaxComments) +
                    " comments; the new comment is cut";
            return false;
        }
        const long long at = nowSeconds();
        nlohmann::json entry = {{"at", at}, {"who", who}, {"body", body}};
        if (!anchor.is_null())
            entry["anchor"] = anchor;
        record["comments"].push_back(entry);
        record["updated_at"] = at;
        if (!writeRecord(repoDir, n, record, error))
            return false;
        out = record;
        return true;
    }

    // ------------------------------------------------------- close and reopen

    bool closePull(const std::string &repoDir, long long n,
                   nlohmann::json &out, std::string &error)
    {
        out = nullptr;
        nlohmann::json record;
        if (!readRecord(repoDir, n, record, error))
            return false;
        const std::string state = stateOf(record);
        if (state == "merged")
        {
            error = "the pull is merged; closing it changes nothing";
            return false;
        }
        if (state == "closed")
        {
            error = "the pull is already closed";
            return false;
        }
        if (state != "open")
        {
            error = "this record states no state the machine knows";
            return false;
        }
        record["state"] = "closed";
        record["updated_at"] = nowSeconds();
        if (!writeRecord(repoDir, n, record, error))
            return false;
        out = record;
        return true;
    }

    bool reopenPull(const std::string &repoDir, long long n,
                    nlohmann::json &out, std::string &error)
    {
        out = nullptr;
        nlohmann::json record;
        if (!readRecord(repoDir, n, record, error))
            return false;
        const std::string state = stateOf(record);
        if (state == "merged")
        {
            error = "the pull is merged and will not reopen";
            return false;
        }
        if (state == "open")
        {
            error = "the pull is already open";
            return false;
        }
        if (state != "closed")
        {
            error = "this record states no state the machine knows";
            return false;
        }
        record["state"] = "open";
        record["updated_at"] = nowSeconds();
        if (!writeRecord(repoDir, n, record, error))
            return false;
        out = record;
        return true;
    }

    // ------------------------------------------------------------- mark merged

    bool markMerged(const std::string &repoDir, long long n,
                    const std::string &style, const std::string &oid,
                    const std::string &by, nlohmann::json &out,
                    std::string &error)
    {
        out = nullptr;
        if (style != "fast-forward" && style != "squash" && style != "merge")
        {
            error = "the merge style is one of the merge engine's words";
            return false;
        }
        if (oid.empty())
        {
            error = "a merge is recorded with the commit it made";
            return false;
        }
        nlohmann::json record;
        if (!readRecord(repoDir, n, record, error))
            return false;
        const std::string state = stateOf(record);
        if (state == "merged")
        {
            error = "the pull is already merged";
            return false;
        }
        if (state == "closed")
        {
            error = "the pull is closed and was not merged";
            return false;
        }
        if (state != "open")
        {
            error = "this record states no state the machine knows";
            return false;
        }
        const long long at = nowSeconds();
        record["state"] = "merged";
        record["merge"] = {{"style", style}, {"oid", oid}, {"at", at}, {"by", by}};
        record["updated_at"] = at;
        if (!writeRecord(repoDir, n, record, error))
            return false;
        out = record;
        return true;
    }

    // ---------------------------------------------------------------- the list

    // The open count, the same walk ProjectService's own openPullsAt takes
    // over the folders under ols/pulls: every folder that carries a
    // record.json, the ones stating "open" counted. So when the folder
    // stands, a delete that refuses on this number refuses on the number the
    // page can show beside the project. Absence of the folder is the true
    // zero: MSVC reports a missing path with an error code here, so only an
    // error that is not "not there" refuses - no letting-go on a folder the
    // machine cannot see, and no holding on a folder that was never made.
    bool countOpenPulls(const std::string &repoDir, long long &open, std::string &error)
    {
        open = 0;
        const std::string dir = pullsDir(repoDir);
        std::error_code ec{};
        if (!std::filesystem::is_directory(dir, ec))
        {
            if (ec && ec != std::errc::no_such_file_or_directory)
            {
                error = "the pulls folder would not list: " + dir;
                return false;
            }
            error.clear();
            return true;
        }
        ec = std::error_code{};
        std::filesystem::directory_iterator it(dir, ec);
        if (ec)
        {
            error = "the pulls folder would not list: " + dir;
            return false;
        }
        for (; it != std::filesystem::directory_iterator{}; it.increment(ec))
        {
            if (ec)
            {
                error = "the pulls folder stopped listing: " + dir;
                return false;
            }
            std::error_code kind{};
            if (!it->is_directory(kind) || kind)
                continue;
            const std::string recordFile = it->path().string() + "/record.json";
            if (!fileExists(recordFile))
                continue;
            std::string body;
            if (!readFileText(recordFile, body))
            {
                error = "a pull record would not come in: " + recordFile;
                return false;
            }
            const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object())
            {
                error = "a pull record would not parse: " + recordFile;
                return false;
            }
            if (stateOf(parsed) == "open")
                ++open;
        }
        error.clear();
        return true;
    }

    bool listPulls(const std::string &repoDir, const std::string &stateFilter,
                   long long limit, long long threshold, nlohmann::json &rows,
                   nlohmann::json &answer, std::string &error)
    {
        rows = nlohmann::json::array();
        answer = nlohmann::json::object();
        long long asked = limit;
        if (asked <= 0)
            asked = 50;
        if (asked > 200)
            asked = 200;
        const std::string dir = pullsDir(repoDir);
        std::error_code ec{};
        if (!std::filesystem::is_directory(dir, ec))
        {
            answer["rows"] = rows;
            answer["hasMore"] = false;
            answer["threshold"] = threshold;
            return true;
        }
        struct Row
        {
            long long updated = 0;
            long long number = 0;
            nlohmann::json json;
        };
        std::vector<Row> matched;
        nlohmann::json unparseable = nlohmann::json::array();
        ec = std::error_code{};
        std::filesystem::directory_iterator it(dir, ec);
        if (ec)
        {
            error = "the pulls folder would not list: " + dir;
            return false;
        }
        for (; it != std::filesystem::directory_iterator{}; it.increment(ec))
        {
            if (ec)
            {
                error = "the pulls folder stopped listing: " + dir;
                return false;
            }
            std::error_code kind{};
            if (!it->is_directory(kind) || kind)
                continue;
            const std::string name = it->path().filename().string();
            bool digits = !name.empty();
            for (char c : name)
            {
                if (c < '0' || c > '9')
                {
                    digits = false;
                    break;
                }
            }
            if (!digits)
                continue;
            const std::string recordFile = it->path().string() + "/record.json";
            std::string body;
            if (!readFileText(recordFile, body))
                continue;
            const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object())
            {
                unparseable.push_back(name);
                continue;
            }
            const std::string state = stateOf(parsed);
            if (state != "open" && state != "merged" && state != "closed")
            {
                unparseable.push_back(name);
                continue;
            }
            if (!stateFilter.empty() && state != stateFilter)
                continue;
            long long approvals = 0;
            if (parsed.contains("approvals") && parsed["approvals"].is_array())
                approvals = static_cast<long long>(parsed["approvals"].size());
            nlohmann::json row = nlohmann::json::object();
            row["number"] = statedCount(parsed, "number");
            row["title"] = statedString(parsed, "title");
            row["state"] = state;
            row["head"] = "";
            row["base"] = "";
            if (parsed.contains("head") && parsed["head"].is_object())
                row["head"] = statedString(parsed["head"], "branch");
            if (parsed.contains("base") && parsed["base"].is_object())
                row["base"] = statedString(parsed["base"], "branch");
            row["approvals"] = {{"n", approvals}, {"threshold", threshold}};
            row["updated_at"] = statedCount(parsed, "updated_at");
            Row held;
            held.updated = row["updated_at"].get<long long>();
            held.number = row["number"].get<long long>();
            held.json = row;
            matched.push_back(std::move(held));
        }
        std::sort(matched.begin(), matched.end(), [](const Row &a, const Row &b)
        {
            if (a.updated != b.updated)
                return a.updated > b.updated;
            return a.number > b.number;
        });
        const bool more = static_cast<long long>(matched.size()) > asked;
        for (size_t i = 0; i < matched.size() && static_cast<long long>(i) < asked; ++i)
            rows.push_back(matched[i].json);
        answer["rows"] = rows;
        answer["hasMore"] = more;
        answer["threshold"] = threshold;
        if (!unparseable.empty())
            answer["unparseable"] = unparseable;
        error.clear();
        return true;
    }

    // --------------------------------------------------------------- approval

    bool approvalMet(const std::string &repoDir, long long n, long long threshold)
    {
        nlohmann::json record;
        std::string ignored;
        if (!readRecord(repoDir, n, record, ignored))
            return false;
        long long needed = threshold;
        if (needed < 1)
            needed = 1;
        long long held = 0;
        if (record.contains("approvals") && record["approvals"].is_array())
            held = static_cast<long long>(record["approvals"].size());
        return held >= needed;
    }

    // ------------------------------------------------------------- the side file

    // The side file beside the record. The listing never sees it: openPullsAt
    // reads only record.json (ProjectService.cpp:709), listPulls reads only
    // record.json per numeric folder (ProjectPulls.cpp:523), and nextNumber
    // counts only numeric directories (ProjectPulls.cpp:116-165).
    static std::string resolutionsPath(const std::string &repoDir, long long n)
    {
        return pullsDir(repoDir) + "/" + std::to_string(n) + "/resolutions.json";
    }

    // The merge engine's bounds, kept beside the law that enforces them here:
    // the items one file may carry are the paths one merge answer carries
    // (gitcore/merge.cpp:50), and one item's content is the bytes one merge
    // side is carried at (gitcore/merge.cpp:51).
    static const long long gMaxResolutionItems = 200;
    static const std::size_t gResolutionItemBytes = 2u * 1024u * 1024u;

    bool readResolutions(const std::string &repoDir, long long n,
                         nlohmann::json &out, std::string &error)
    {
        out = nullptr;
        const std::string path = resolutionsPath(repoDir, n);
        std::string body;
        if (!readFileText(path, body))
        {
            // Absence is nothing resolved.
            error.clear();
            return true;
        }
        const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
        if (parsed.is_discarded())
        {
            error = "the resolutions of pull " + std::to_string(n) +
                    " would not parse: " + path;
            return false;
        }
        if (!parsed.is_object())
        {
            error = "the resolutions of pull " + std::to_string(n) +
                    " are not a resolutions file: " + path;
            return false;
        }
        out = parsed;
        error.clear();
        return true;
    }

    static bool itemWellFormed(const nlohmann::json &item, std::string &why)
    {
        if (!item.is_object() || !item.contains("path") ||
            !item["path"].is_string() || item["path"].get<std::string>().empty())
        {
            why = "an item of the resolutions is not an item: it carries no path";
            return false;
        }
        if (!item.contains("action") || !item["action"].is_string())
        {
            why = "an item of the resolutions names no action";
            return false;
        }
        const std::string action = item["action"].get<std::string>();
        if (action != "settle" && action != "delete")
        {
            why = "an item of the resolutions wears an action that is not settle or delete";
            return false;
        }
        if (action == "settle")
        {
            if (!item.contains("content") || !item["content"].is_string())
            {
                why = "an item that settles carries no content";
                return false;
            }
            if (item["content"].get<std::string>().size() > gResolutionItemBytes)
            {
                why = "an item's content wears " +
                      std::to_string(item["content"].get<std::string>().size()) +
                      " bytes, past the 2 MiB a merge side is carried at";
                return false;
            }
        }
        return true;
    }

    bool writeResolutions(const std::string &repoDir, long long n,
                          const std::string &atHead, const std::string &atBase,
                          const nlohmann::json &items, nlohmann::json &out,
                          std::string &error)
    {
        out = nullptr;
        if (!items.is_array())
        {
            error = "the resolutions are not a list of items";
            return false;
        }
        if (static_cast<long long>(items.size()) > gMaxResolutionItems)
        {
            error = "this resolutions file carries " + std::to_string(items.size()) +
                    " items, past the 200 one merge answer carries";
            return false;
        }
        for (const nlohmann::json &item : items)
        {
            if (!itemWellFormed(item, error))
                return false;
        }
        nlohmann::json file = nlohmann::json::object();
        file["at"] = {{"head", atHead}, {"base", atBase}};
        file["items"] = items;
        const std::string text = file.dump(2);
        if (text.size() > gRecordBytes)
        {
            error = "this resolutions file wears " + std::to_string(text.size()) +
                    " bytes, past the 1 MiB a record is kept at";
            return false;
        }
        if (!writeFileText(resolutionsPath(repoDir, n), text))
        {
            error = "the resolutions file would not take the write";
            return false;
        }
        out = file;
        error.clear();
        return true;
    }

    bool setResolution(const std::string &repoDir, long long n,
                       const std::string &path, const std::string &content,
                       bool deleteFile, const std::string &by,
                       nlohmann::json &out, std::string &error)
    {
        out = nullptr;
        if (path.empty())
        {
            error = "a resolution is kept for a path";
            return false;
        }
        nlohmann::json held;
        if (!readResolutions(repoDir, n, held, error))
            return false;
        std::string atHead, atBase;
        nlohmann::json items = nlohmann::json::array();
        if (held.is_object())
        {
            // The stamps of an existing file are kept: the door checks them.
            if (held.contains("at") && held["at"].is_object())
            {
                atHead = statedString(held["at"], "head");
                atBase = statedString(held["at"], "base");
            }
            if (held.contains("items") && held["items"].is_array())
                items = held["items"];
        }
        else
        {
            // A first file stamps from the record's lease: the oids at open.
            nlohmann::json record;
            std::string ignored;
            if (!readRecord(repoDir, n, record, ignored))
            {
                error = "the pull record is not there to stamp the resolution against";
                return false;
            }
            if (record.contains("head") && record["head"].is_object())
                atHead = statedString(record["head"], "oidAtOpen");
            if (record.contains("base") && record["base"].is_object())
                atBase = statedString(record["base"], "oidAtOpen");
        }
        bool replaced = false;
        for (auto it = items.begin(); it != items.end(); ++it)
        {
            if (!it->is_object() || !it->contains("path") ||
                !it->at("path").is_string() || it->at("path").get<std::string>() != path)
                continue;
            nlohmann::json item = nlohmann::json::object();
            item["path"] = path;
            item["action"] = deleteFile ? "delete" : "settle";
            if (!deleteFile)
                item["content"] = content;
            item["at"] = nowSeconds();
            item["by"] = by;
            *it = item;
            replaced = true;
            break;
        }
        if (!replaced)
        {
            if (static_cast<long long>(items.size()) >= gMaxResolutionItems)
            {
                error = "this resolutions file already carries 200 items, and a new one is cut";
                return false;
            }
            nlohmann::json item = nlohmann::json::object();
            item["path"] = path;
            item["action"] = deleteFile ? "delete" : "settle";
            if (!deleteFile)
                item["content"] = content;
            item["at"] = nowSeconds();
            item["by"] = by;
            items.push_back(item);
        }
        if (!writeResolutions(repoDir, n, atHead, atBase, items, out, error))
        {
            out = nullptr;
            return false;
        }
        error.clear();
        return true;
    }

    bool clearResolutions(const std::string &repoDir, long long n,
                          nlohmann::json &out, std::string &error)
    {
        out = nullptr;
        nlohmann::json held;
        if (!readResolutions(repoDir, n, held, error))
            return false;
        if (!held.is_object())
        {
            // Nothing was resolved: there is nothing to clear.
            error.clear();
            return true;
        }
        std::string atHead, atBase;
        if (held.contains("at") && held["at"].is_object())
        {
            atHead = statedString(held["at"], "head");
            atBase = statedString(held["at"], "base");
        }
        if (!writeResolutions(repoDir, n, atHead, atBase, nlohmann::json::array(),
                              out, error))
        {
            out = nullptr;
            return false;
        }
        error.clear();
        return true;
    }

} // namespace pulls
} // namespace ols
