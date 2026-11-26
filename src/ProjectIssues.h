#pragma once
// src/ProjectIssues.h — the issue records of one project's repo.
//
// One issue is one flat file: <repoDir>/ols/issues/<id>.json, the reader of
// that folder already standing in ProjectService.cpp's openIssuesAt
// (:675-687), which hands readRecordsAt (:641-673) the folder and counts the
// records whose state reads "open". That reader takes every ordinary .json in
// that ONE folder, so nothing else ever stands there: no sidecars, no extra
// .json — a file past the record law would be read as a record, and one it
// cannot read at all fails the whole count. This module is the writer beside
// that reader: it makes the record, moves a record through its states, and
// answers the list a page asks for. It owns no door and no page.
//
// Every function takes the project's repoDir — the folder whose ols/ holds
// the records — so ProjectService can call these under its own gMutex. The
// locking contract: this module holds no lock and takes none; a caller that
// mutates a project's records and its facts together holds the project's
// mutex across both, and the order is the record first and the rev bump
// after — a record that would not write refuses before any rev moves, the
// same order writeProjectFiles keeps at ProjectService.cpp:437-445.
//
// The record is small on purpose: openIssuesAt reads every .json on every
// dropdown read, so nothing derived — no diffs, no conflict texts — is ever
// stored here. The caps are stated beside the law that enforces them: a body
// of at most 64 KiB, at most 200 comments of at most 4 KiB each, at most 20
// file tags and 20 labels, at most 50 links, and a whole record past 1 MiB
// is refused with words rather than written.
//
// The id is the machine's own: makeId("issue") (Common.h:246-260) prints
// issue-<stamp>-<8hex>, the stamp drawn by "%012llx" — twelve hex digits at
// minimum, and longer once the steady clock's count wears past 48 bits —
// and no counter stands behind it, so an id never comes back. The shape is
// validated before any path is built, the way the pull law checks a folder
// name for digits, and a closed issue keeps its file — the folder keeps its
// gaps, the way a closed pull keeps its folder.
//
// No call in this namespace throws. A refusal is false with the reason in
// the error string; an answer that carries rows or a record carries it in
// the json the caller handed.

#ifndef OLS_PROJECT_ISSUES_H
#define OLS_PROJECT_ISSUES_H

#include <nlohmann/json.hpp>
#include <string>

namespace ols
{
    namespace issues
    {

        // ------------------------------------------------------------------ records

        // The record of issue <id>, parsed. The id shape is checked before
        // any path is built. False when the file is not there, will not
        // read, or will not parse — with the reason in error.
        bool readRecord(const std::string &repoDir, const std::string &id,
                        nlohmann::json &record, std::string &error);

        // The record as written: one writeFileText (Common.h:176-205, temp
        // plus rename) at <repoDir>/ols/issues/<id>.json, whose parent
        // folder comes with the write (ensureParentDir, Common.h:157-165).
        // A record whose words wear past 1 MiB is refused with words before
        // anything is written.
        bool writeRecord(const std::string &repoDir, const std::string &id,
                         const nlohmann::json &record, std::string &error);

        // --------------------------------------------------------------- the machine

        // Open an issue. Refuses an empty title and a body past 64 KiB.
        // severity is one of note, bug, want, blocker — empty reads "note".
        // filesJson is an array of at most 20 tags {path,line?,commit?} or
        // null for none; labelsJson an array of at most 20 words of at
        // most 64 bytes, or null for none. The record is written with state
        // "open"; the caller bumps the project's rev after it returns true.
        bool createIssue(const std::string &repoDir, const std::string &slug,
                         const std::string &title, const std::string &body,
                         const std::string &severity,
                         const nlohmann::json &filesJson,
                         const nlohmann::json &labelsJson,
                         nlohmann::json &out, std::string &error);

        // Edit an open issue. The patch is an object wearing only title,
        // body, severity, labels, and files — any other field is refused
        // by name. A closed issue takes no edit: "this issue is closed and
        // takes no edit".
        bool editIssue(const std::string &repoDir, const std::string &id,
                       const nlohmann::json &patch, nlohmann::json &out,
                       std::string &error);

        // Comment on an open issue, with an optional anchor
        // {path,line,commit,side} passed as a json object or null — the
        // comment always wears the anchor field, null when there is none.
        // Past 200 comments the new one is cut, the refusal is named in
        // words, and out carries truncated:true with the record as it
        // stands. A closed issue takes no comments.
        bool addComment(const std::string &repoDir, const std::string &id,
                        const std::string &who, const std::string &body,
                        const nlohmann::json &anchor, nlohmann::json &out,
                        std::string &error);

        // Close an open issue: state "closed", the file kept. Already
        // closed is refused: "the issue is already closed".
        bool closeIssue(const std::string &repoDir, const std::string &id,
                        nlohmann::json &out, std::string &error);

        // Reopen a closed issue. Already open is refused: "the issue is
        // already open".
        bool reopenIssue(const std::string &repoDir, const std::string &id,
                         nlohmann::json &out, std::string &error);

        // Cross-reference the issue with one thing. kind is "pr", "issue",
        // or "commit" — anything else is refused by name. The target lands
        // once in the links bucket its kind names ("prs", "issues",
        // "commits"), at most 50 links across the three.
        bool linkIssue(const std::string &repoDir, const std::string &id,
                       const std::string &kind, const std::string &target,
                       nlohmann::json &out, std::string &error);

        // ------------------------------------------------------------------ the list

        // The rows a page asks for: {id,title,state,severity,labels,
        // updated_at}, newest updated_at first, default 50 and at most 200
        // rows, with hasMore when more stand than were asked for.
        // stateFilter is "open", "closed", or empty for all; fileFilter,
        // when not empty, keeps the records that wear a tag whose path
        // matches it exactly. A .json that will not parse, wears no state
        // the machine knows, or wears no issue id as its name is NAMED in
        // the answer — never guessed around.
        bool listIssues(const std::string &repoDir, const std::string &fileFilter,
                        const std::string &stateFilter, long long limit,
                        nlohmann::json &rows, nlohmann::json &answer,
                        std::string &error);

        // Per-path counts of the open issues, for the tree door: an object
        // mapping each tagged path to the number of open issues that wear
        // a tag for it, each issue counted once per path. A file this read
        // cannot take is skipped here — listIssues is where an unreadable
        // file is named.
        bool issueTags(const std::string &repoDir, nlohmann::json &perPath,
                       std::string &error);

    } // namespace issues
} // namespace ols

#endif // OLS_PROJECT_ISSUES_H
