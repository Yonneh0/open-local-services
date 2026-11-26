// src/ProjectIssues.cpp — the record law beside the reader that already stands.
//
// openIssuesAt (ProjectService.cpp:675-687) hands readRecordsAt (:641-673)
// the folder <repoDir>/ols/issues and counts the records whose state reads
// "open"; this file is the writer that keeps that folder honest, the state
// machine a record moves through, and the list a page asks for. One record
// is one flat file whose name is the issue's own id, so the folder holds
// nothing but those .json files — the reader takes every ordinary .json in
// that ONE folder, and a sidecar there would be read as a record.
//
// Nothing here reaches a door, a page, or a ProjectService header: every
// function takes the repoDir, and the caller holds the project's mutex and
// bumps the rev after a true answer — the record first, the mark after, the
// order writeProjectFiles keeps at ProjectService.cpp:437-445.
//
// One record is one writeFileText (Common.h:176-205): a temp beside the
// target and a rename, so a reader never sees half a record. The folder
// comes with the write (ensureParentDir, Common.h:157-165), never with the
// start. A record past 1 MiB is refused with words before anything is
// written, and nothing derived is ever stored in it.

#include "ProjectIssues.h"

#include "Common.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ols
{
    namespace issues
    {

    // ---------------------------------------------------------------- the shape

    // The folder the records of one repo stand in, beside the reader's path
    // at ProjectService.cpp:678.
    static std::string issuesDir(const std::string &repoDir)
    {
        return repoDir + "/ols/issues";
    }

    static std::string recordPath(const std::string &repoDir, const std::string &id)
    {
        return issuesDir(repoDir) + "/" + id + ".json";
    }

    // A whole run of the id's words, from one bound to the other, is hex.
    static bool hexRun(const std::string &id, size_t from, size_t to)
    {
        for (size_t i = from; i < to; ++i)
        {
            const char c = id[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;
        }
        return true;
    }

    // The shape makeId("issue") prints (Common.h:246-260): "issue-", the
    // stamp, a dash, 8 tag digits. The stamp comes from "%012llx" — twelve
    // hex digits at minimum, and longer once the steady clock's whole count
    // wears past 48 bits, which on this machine's clock is past ~78 hours of
    // uptime — so the law is twelve or more, never exactly twelve. The shape
    // is checked before any path is built, so no words a caller hands can
    // name a folder — the way the pull law keeps a folder name to digits
    // (ProjectPulls.cpp:137-147).
    static bool idIsWellFormed(const std::string &id)
    {
        static const char prefix[] = "issue-";
        if (id.size() < 27u || id.compare(0u, 6u, prefix) != 0)
            return false;
        const size_t tail = id.size() - 9u;
        return tail >= 18u && id[tail] == '-' && hexRun(id, 6u, tail)
            && hexRun(id, tail + 1u, id.size());
    }

    // A record past this is not this module's record: the words are refused
    // with the size named, and nothing is written.
    static const std::size_t gRecordBytes = 1024u * 1024u;

    // The caps the record law keeps: a body of at most 64 KiB, comments of
    // at most 4 KiB each at most 200 of them, at most 20 file tags and 20
    // labels of at most 64 bytes, and at most 50 links across the buckets.
    // Past a cap the new thing is cut and the cut is named in words.
    static const long long gMaxComments = 200;
    static const std::size_t gCommentBytes = 4096u;
    static const std::size_t gBodyBytes = 64u * 1024u;
    static const long long gMaxTags = 20;
    static const long long gMaxLabels = 20;
    static const std::size_t gLabelBytes = 64u;
    static const long long gMaxLinks = 50;

    // The severity words a record wears.
    static bool severityIsKnown(const std::string &severity)
    {
        return severity == "note" || severity == "bug" || severity == "want"
            || severity == "blocker";
    }

    // The state a record wears, as its word. Anything the record does not
    // state reads as empty, and the machine refuses where it stands.
    static std::string stateOf(const nlohmann::json &record)
    {
        return statedString(record, "state");
    }

    // ------------------------------------------------------------------ records

    bool readRecord(const std::string &repoDir, const std::string &id,
                    nlohmann::json &record, std::string &error)
    {
        record = nullptr;
        if (!idIsWellFormed(id))
        {
            error = "this is not an issue id: " + id;
            return false;
        }
        const std::string path = recordPath(repoDir, id);
        std::string body;
        if (!readFileText(path, body))
        {
            error = "the record of issue " + id + " is not there to read";
            return false;
        }
        const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
        if (parsed.is_discarded())
        {
            error = "the record of issue " + id + " would not parse: " + path;
            return false;
        }
        if (!parsed.is_object())
        {
            error = "the record of issue " + id + " is not a record: " + path;
            return false;
        }
        record = parsed;
        error.clear();
        return true;
    }

    bool writeRecord(const std::string &repoDir, const std::string &id,
                     const nlohmann::json &record, std::string &error)
    {
        if (!idIsWellFormed(id))
        {
            error = "this is not an issue id: " + id;
            return false;
        }
        const std::string text = record.dump(2);
        if (text.size() > gRecordBytes)
        {
            error = "this record wears " + std::to_string(text.size()) +
                    " bytes, past the 1 MiB a record is kept at";
            return false;
        }
        if (!writeFileText(recordPath(repoDir, id), text))
        {
            error = "the record file would not take the write";
            return false;
        }
        error.clear();
        return true;
    }

    // ------------------------------------------------------------- the two arrays

    // The file tags as the record keeps them: {path,line?,commit?}, at most
    // 20. Null means none. A tag must name a path; a line that is not a
    // whole number and a commit that is not words are dropped.
    static bool tagsFrom(const nlohmann::json &filesJson, nlohmann::json &tags,
                         std::string &error)
    {
        tags = nlohmann::json::array();
        if (filesJson.is_null())
            return true;
        if (!filesJson.is_array())
        {
            error = "the file tags of an issue are an array";
            return false;
        }
        if (static_cast<long long>(filesJson.size()) > gMaxTags)
        {
            error = "an issue carries at most 20 file tags";
            return false;
        }
        for (const nlohmann::json &held : filesJson)
        {
            if (!held.is_object())
            {
                error = "a file tag names a path";
                return false;
            }
            const std::string path = statedString(held, "path");
            if (path.empty())
            {
                error = "a file tag names a path";
                return false;
            }
            nlohmann::json tag = {{"path", path}};
            if (held.contains("line") && held["line"].is_number_integer())
                tag["line"] = held["line"];
            const std::string commit = statedString(held, "commit");
            if (!commit.empty())
                tag["commit"] = commit;
            tags.push_back(tag);
        }
        return true;
    }

    // The labels as the record keeps them: words of at most 64 bytes, at
    // most 20. Null means none.
    static bool labelsFrom(const nlohmann::json &labelsJson, nlohmann::json &labels,
                           std::string &error)
    {
        labels = nlohmann::json::array();
        if (labelsJson.is_null())
            return true;
        if (!labelsJson.is_array())
        {
            error = "the labels of an issue are an array";
            return false;
        }
        if (static_cast<long long>(labelsJson.size()) > gMaxLabels)
        {
            error = "an issue carries at most 20 labels";
            return false;
        }
        for (const nlohmann::json &held : labelsJson)
        {
            if (!held.is_string())
            {
                error = "a label is a word";
                return false;
            }
            const std::string label = held.get<std::string>();
            if (label.empty())
            {
                error = "a label is a word";
                return false;
            }
            if (label.size() > gLabelBytes)
            {
                error = "this label wears " + std::to_string(label.size()) +
                        " bytes, past the 64 a label is kept at";
                return false;
            }
            labels.push_back(label);
        }
        return true;
    }

    static bool bodyIsKept(const std::string &body, std::string &error)
    {
        if (body.size() > gBodyBytes)
        {
            error = "this body wears " + std::to_string(body.size()) +
                    " bytes, past the 64 KiB a body is kept at";
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------- open an issue

    bool createIssue(const std::string &repoDir, const std::string &slug,
                     const std::string &title, const std::string &body,
                     const std::string &severity,
                     const nlohmann::json &filesJson,
                     const nlohmann::json &labelsJson,
                     nlohmann::json &out, std::string &error)
    {
        out = nullptr;
        if (title.empty())
        {
            error = "an issue is opened with a title";
            return false;
        }
        if (!bodyIsKept(body, error))
            return false;
        std::string word = severity;
        if (word.empty())
            word = "note";
        if (!severityIsKnown(word))
        {
            error = "the severity is one of note, bug, want, blocker";
            return false;
        }
        nlohmann::json tags;
        if (!tagsFrom(filesJson, tags, error))
            return false;
        nlohmann::json labels;
        if (!labelsFrom(labelsJson, labels, error))
            return false;
        const std::string id = makeId("issue");
        const long long at = nowSeconds();
        nlohmann::json record = nlohmann::json::object();
        record["id"] = id;
        record["project"] = slug;
        record["title"] = title;
        record["body"] = body;
        record["state"] = "open";
        record["severity"] = word;
        record["files"] = tags;
        record["labels"] = labels;
        record["created_at"] = at;
        record["updated_at"] = at;
        record["comments"] = nlohmann::json::array();
        record["links"] = {{"prs", nlohmann::json::array()},
                           {"issues", nlohmann::json::array()},
                           {"commits", nlohmann::json::array()}};
        if (!writeRecord(repoDir, id, record, error))
            return false;
        out = record;
        return true;
    }

    // ---------------------------------------------------------------- edit an issue

    bool editIssue(const std::string &repoDir, const std::string &id,
                   const nlohmann::json &patch, nlohmann::json &out,
                   std::string &error)
    {
        out = nullptr;
        nlohmann::json record;
        if (!readRecord(repoDir, id, record, error))
            return false;
        const std::string state = stateOf(record);
        if (state == "closed")
        {
            error = "this issue is closed and takes no edit";
            return false;
        }
        if (state != "open")
        {
            error = "this record states no state the machine knows";
            return false;
        }
        if (!patch.is_object())
        {
            error = "an edit is an object of the fields to change";
            return false;
        }
        bool asked = false;
        for (auto field = patch.begin(); field != patch.end(); ++field)
        {
            const std::string &name = field.key();
            if (name != "title" && name != "body" && name != "severity"
                && name != "labels" && name != "files")
            {
                error = "the edit asks for " + name +
                        ", which the edit does not take";
                return false;
            }
            asked = true;
        }
        if (!asked)
        {
            error = "the edit names no field to change";
            return false;
        }
        if (patch.contains("title"))
        {
            const std::string title = statedString(patch, "title");
            if (title.empty())
            {
                error = "the edit takes a title with words";
                return false;
            }
            record["title"] = title;
        }
        if (patch.contains("body"))
        {
            const std::string body = statedString(patch, "body");
            if (!bodyIsKept(body, error))
                return false;
            record["body"] = body;
        }
        if (patch.contains("severity"))
        {
            const std::string word = statedString(patch, "severity");
            if (!severityIsKnown(word))
            {
                error = "the severity is one of note, bug, want, blocker";
                return false;
            }
            record["severity"] = word;
        }
        if (patch.contains("files"))
        {
            nlohmann::json tags;
            if (!tagsFrom(patch["files"], tags, error))
                return false;
            record["files"] = tags;
        }
        if (patch.contains("labels"))
        {
            nlohmann::json labels;
            if (!labelsFrom(patch["labels"], labels, error))
                return false;
            record["labels"] = labels;
        }
        record["updated_at"] = nowSeconds();
        if (!writeRecord(repoDir, id, record, error))
            return false;
        out = record;
        return true;
    }

    // ------------------------------------------------------------ comment an issue

    bool addComment(const std::string &repoDir, const std::string &id,
                    const std::string &who, const std::string &body,
                    const nlohmann::json &anchor, nlohmann::json &out,
                    std::string &error)
    {
        out = nullptr;
        nlohmann::json record;
        if (!readRecord(repoDir, id, record, error))
            return false;
        const std::string state = stateOf(record);
        if (state == "closed")
        {
            error = "this issue is closed and takes no comments";
            return false;
        }
        if (state != "open")
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
            error = "this issue already carries " + std::to_string(gMaxComments) +
                    " comments; the new comment is cut";
            return false;
        }
        const long long at = nowSeconds();
        record["comments"].push_back(
            {{"at", at}, {"who", who}, {"body", body}, {"anchor", anchor}});
        record["updated_at"] = at;
        if (!writeRecord(repoDir, id, record, error))
            return false;
        out = record;
        return true;
    }

    // ------------------------------------------------------- close and reopen

    bool closeIssue(const std::string &repoDir, const std::string &id,
                    nlohmann::json &out, std::string &error)
    {
        out = nullptr;
        nlohmann::json record;
        if (!readRecord(repoDir, id, record, error))
            return false;
        const std::string state = stateOf(record);
        if (state == "closed")
        {
            error = "the issue is already closed";
            return false;
        }
        if (state != "open")
        {
            error = "this record states no state the machine knows";
            return false;
        }
        record["state"] = "closed";
        record["updated_at"] = nowSeconds();
        if (!writeRecord(repoDir, id, record, error))
            return false;
        out = record;
        return true;
    }

    bool reopenIssue(const std::string &repoDir, const std::string &id,
                     nlohmann::json &out, std::string &error)
    {
        out = nullptr;
        nlohmann::json record;
        if (!readRecord(repoDir, id, record, error))
            return false;
        const std::string state = stateOf(record);
        if (state == "open")
        {
            error = "the issue is already open";
            return false;
        }
        if (state != "closed")
        {
            error = "this record states no state the machine knows";
            return false;
        }
        record["state"] = "open";
        record["updated_at"] = nowSeconds();
        if (!writeRecord(repoDir, id, record, error))
            return false;
        out = record;
        return true;
    }

    // ---------------------------------------------------------------- link a thing

    bool linkIssue(const std::string &repoDir, const std::string &id,
                   const std::string &kind, const std::string &target,
                   nlohmann::json &out, std::string &error)
    {
        out = nullptr;
        const char *bucket = nullptr;
        if (kind == "pr")
            bucket = "prs";
        else if (kind == "issue")
            bucket = "issues";
        else if (kind == "commit")
            bucket = "commits";
        else
        {
            error = "the link kind is pr, issue, or commit";
            return false;
        }
        if (target.empty())
        {
            error = "a link is made with the thing it names";
            return false;
        }
        nlohmann::json record;
        if (!readRecord(repoDir, id, record, error))
            return false;
        const std::string state = stateOf(record);
        if (state != "open" && state != "closed")
        {
            error = "this record states no state the machine knows";
            return false;
        }
        if (!record.contains("links") || !record["links"].is_object())
            record["links"] = nlohmann::json::object();
        long long held = 0;
        for (const char *name : {"prs", "issues", "commits"})
        {
            if (!record["links"].contains(name) || !record["links"][name].is_array())
                record["links"][name] = nlohmann::json::array();
            held += static_cast<long long>(record["links"][name].size());
        }
        for (const nlohmann::json &entry : record["links"][bucket])
        {
            if (entry.is_string() && entry.get<std::string>() == target)
            {
                // The link already stands; the record does not change.
                out = record;
                error.clear();
                return true;
            }
        }
        if (held >= gMaxLinks)
        {
            error = "this issue already carries " + std::to_string(gMaxLinks) +
                    " links; the new link is cut";
            return false;
        }
        record["links"][bucket].push_back(target);
        record["updated_at"] = nowSeconds();
        if (!writeRecord(repoDir, id, record, error))
            return false;
        out = record;
        return true;
    }

    // ---------------------------------------------------------------- the list

    bool listIssues(const std::string &repoDir, const std::string &fileFilter,
                    const std::string &stateFilter, long long limit,
                    nlohmann::json &rows, nlohmann::json &answer,
                    std::string &error)
    {
        rows = nlohmann::json::array();
        answer = nlohmann::json::object();
        long long asked = limit;
        if (asked <= 0)
            asked = 50;
        if (asked > 200)
            asked = 200;
        const std::string dir = issuesDir(repoDir);
        std::error_code ec{};
        if (!std::filesystem::is_directory(dir, ec))
        {
            answer["rows"] = rows;
            answer["hasMore"] = false;
            return true;
        }
        struct Row
        {
            long long updated = 0;
            std::string id;
            nlohmann::json json;
        };
        std::vector<Row> matched;
        nlohmann::json unparseable = nlohmann::json::array();
        ec = std::error_code{};
        std::filesystem::directory_iterator it(dir, ec);
        if (ec)
        {
            error = "the issues folder would not list: " + dir;
            return false;
        }
        for (; it != std::filesystem::directory_iterator{}; it.increment(ec))
        {
            if (ec)
            {
                error = "the issues folder stopped listing: " + dir;
                return false;
            }
            std::error_code kind{};
            if (!it->is_regular_file(kind) || kind)
                continue;
            const std::string name = it->path().filename().string();
            if (name.size() < 5u || name.compare(name.size() - 5u, 5u, ".json") != 0)
                continue;
            const std::string stem = name.substr(0, name.size() - 5u);
            if (!idIsWellFormed(stem))
            {
                unparseable.push_back(name);
                continue;
            }
            std::string body;
            if (!readFileText(it->path().string(), body))
                continue;
            const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object())
            {
                unparseable.push_back(name);
                continue;
            }
            const std::string state = stateOf(parsed);
            if (state != "open" && state != "closed")
            {
                unparseable.push_back(name);
                continue;
            }
            if (!stateFilter.empty() && state != stateFilter)
                continue;
            if (!fileFilter.empty())
            {
                bool tagged = false;
                if (parsed.contains("files") && parsed["files"].is_array())
                {
                    for (const nlohmann::json &tag : parsed["files"])
                    {
                        if (tag.is_object() && statedString(tag, "path") == fileFilter)
                        {
                            tagged = true;
                            break;
                        }
                    }
                }
                if (!tagged)
                    continue;
            }
            nlohmann::json row = nlohmann::json::object();
            row["id"] = statedString(parsed, "id");
            row["title"] = statedString(parsed, "title");
            row["state"] = state;
            row["severity"] = statedString(parsed, "severity");
            row["labels"] = (parsed.contains("labels") && parsed["labels"].is_array())
                                ? parsed["labels"]
                                : nlohmann::json::array();
            row["updated_at"] = statedCount(parsed, "updated_at");
            Row held;
            held.updated = row["updated_at"].get<long long>();
            held.id = row["id"].get<std::string>();
            held.json = row;
            matched.push_back(std::move(held));
        }
        std::sort(matched.begin(), matched.end(), [](const Row &a, const Row &b)
        {
            if (a.updated != b.updated)
                return a.updated > b.updated;
            return a.id > b.id;
        });
        const bool more = static_cast<long long>(matched.size()) > asked;
        for (size_t i = 0; i < matched.size() && static_cast<long long>(i) < asked; ++i)
            rows.push_back(matched[i].json);
        answer["rows"] = rows;
        answer["hasMore"] = more;
        if (!unparseable.empty())
            answer["unparseable"] = unparseable;
        error.clear();
        return true;
    }

    // ------------------------------------------------------------- per-path counts

    bool issueTags(const std::string &repoDir, nlohmann::json &perPath,
                   std::string &error)
    {
        perPath = nlohmann::json::object();
        const std::string dir = issuesDir(repoDir);
        std::error_code ec{};
        if (!std::filesystem::is_directory(dir, ec))
        {
            error.clear();
            return true;
        }
        ec = std::error_code{};
        std::filesystem::directory_iterator it(dir, ec);
        if (ec)
        {
            error = "the issues folder would not list: " + dir;
            return false;
        }
        for (; it != std::filesystem::directory_iterator{}; it.increment(ec))
        {
            if (ec)
            {
                error = "the issues folder stopped listing: " + dir;
                return false;
            }
            std::error_code kind{};
            if (!it->is_regular_file(kind) || kind)
                continue;
            const std::string name = it->path().filename().string();
            if (name.size() < 5u || name.compare(name.size() - 5u, 5u, ".json") != 0)
                continue;
            std::string body;
            if (!readFileText(it->path().string(), body))
                continue;
            const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object())
                continue;
            if (stateOf(parsed) != "open")
                continue;
            if (!parsed.contains("files") || !parsed["files"].is_array())
                continue;
            std::vector<std::string> counted;
            for (const nlohmann::json &tag : parsed["files"])
            {
                if (!tag.is_object())
                    continue;
                const std::string path = statedString(tag, "path");
                if (path.empty())
                    continue;
                if (std::find(counted.begin(), counted.end(), path) != counted.end())
                    continue;
                counted.push_back(path);
                if (!perPath[path].is_number_integer())
                    perPath[path] = 0;
                perPath[path] = perPath[path].get<long long>() + 1;
            }
        }
        error.clear();
        return true;
    }

    } // namespace issues
} // namespace ols
