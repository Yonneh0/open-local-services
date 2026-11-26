#pragma once
// src/ProjectPulls.h — the pull-request records of one project's repo.
//
// One pull request is one folder: <repoDir>/ols/pulls/<n>/record.json, the
// reader of that folder already standing in ProjectService.cpp's openPullsAt
// (:687-720), which walks it and counts the records whose state reads "open".
// This module is the writer beside that reader: it makes the record, moves a
// record through its states, and answers the list a dropdown asks for. It
// owns no door and no page. countOpenPulls stands here as the module's own
// reader beside that one: the open count the letting-go of a project weighs
// before it takes the tree away.
//
// Every function takes the project's repoDir — the folder whose ols/ holds
// the records — so ProjectService can call these under its own gMutex. The
// locking contract: this module holds no lock and takes none; a caller that
// mutates a project's records and its facts together holds the project's
// mutex across both, and the order is the record first and the rev bump
// after — a record that would not write refuses before any rev moves, the
// same order writeProjectFiles keeps at ProjectService.cpp:437-445.
//
// The record is small on purpose: openPullsAt reads every record.json on
// every dropdown read, so nothing derived — no diffs, no conflict texts —
// is ever stored here. The caps are stated beside the law that enforces
// them: at most 200 comments of at most 4 KiB each, at most 200 reviews,
// and a whole record past 1 MiB is refused with words rather than written.
//
// Numbers are never reused: <n> is max(existing)+1 from the directory
// listing, and a folder is never deleted — a closed pull keeps its folder
// with state "closed", so the listing keeps its gaps.
//
// No call in this namespace throws. A refusal is false with the reason in
// the error string; an answer that carries rows or a record carries it in
// the json the caller handed.

#ifndef OLS_PROJECT_PULLS_H
#define OLS_PROJECT_PULLS_H

#include <nlohmann/json.hpp>
#include <string>

namespace ols
{
    namespace pulls
    {

        // The states a record wears. "open" is what openPullsAt counts
        // (ProjectService.cpp:716); "merged" and "closed" are the two ways
        // an open pull stops, and only one of them comes back.
        enum class PullState
        {
            Open,
            Merged,
            Closed
        };

        // The three verdicts a review carries. approve stands beside the
        // approvals list; request-changes takes a reviewer's standing
        // approval away; comment records the review alone and says nothing
        // about approval.
        enum class ReviewVerdict
        {
            Approve,
            RequestChanges,
            Comment
        };

        // The three shapes markMerged records in the merge field, named as
        // the merge engine names them (gitcore/merge.h:56-61): the style is
        // stored as its word, so this module keeps the words, not the enum.

        // ------------------------------------------------------------------ numbers

        // The number the next pull of this repo would take: one past the
        // highest folder the listing shows, from 1 when the folder is not
        // there yet. Gaps survive — a closed pull keeps its folder — so the
        // answer is max+1, never a count.
        long long nextNumber(const std::string &repoDir);

        // ------------------------------------------------------------------ records

        // The record of pull n, parsed. false when the folder is not there,
        // the file will not read, or the words will not parse — with the
        // reason in error.
        bool readRecord(const std::string &repoDir, long long n,
                        nlohmann::json &record, std::string &error);

        // The record as written: one writeFileText (Common.h:176-205, temp
        // plus rename) at <repoDir>/ols/pulls/<n>/record.json, whose parent
        // folder comes with the write (ensureParentDir, Common.h:157-165).
        // A record whose words wear past 1 MiB is refused with words before
        // anything is written.
        bool writeRecord(const std::string &repoDir, long long n,
                         const nlohmann::json &record, std::string &error);

        // --------------------------------------------------------------- the machine

        // Open a pull. The caller checked that both branches exist — doors
        // read refs — so this only refuses what the record itself can see:
        // the same branch on both sides, and a head whose oid already
        // stands at the base ("the head already stands at the base: there
        // is nothing to pull"). The record is written with state "open";
        // the caller bumps the project's rev after it returns true.
        bool createPull(const std::string &repoDir, const std::string &slug,
                        const std::string &headBranch, const std::string &baseBranch,
                        const std::string &title, const std::string &body,
                        const std::string &headOid, const std::string &baseOid,
                        nlohmann::json &out, std::string &error);

        // Review a pull. A merged pull takes no more reviews. approve adds
        // the reviewer to the approvals list once; request-changes records
        // the review and removes that reviewer's standing approval; comment
        // records the review only. The review is kept either way.
        bool addReview(const std::string &repoDir, long long n,
                       const std::string &who, ReviewVerdict verdict,
                       const std::string &body, nlohmann::json &out,
                       std::string &error);

        // Comment on a pull, with an optional anchor {path,line,commit,side}
        // passed as a json object or null. Past 200 comments the new one is
        // cut, the refusal is named in words, and out carries truncated:true
        // with the record as it stands.
        bool addComment(const std::string &repoDir, long long n,
                       const std::string &who, const std::string &body,
                       const nlohmann::json &anchor, nlohmann::json &out,
                       std::string &error);

        // Close an open pull: state "closed", the folder kept.
        bool closePull(const std::string &repoDir, long long n,
                       nlohmann::json &out, std::string &error);

        // Reopen a closed pull. A merged pull will not reopen: "the pull is
        // merged and will not reopen".
        bool reopenPull(const std::string &repoDir, long long n,
                       nlohmann::json &out, std::string &error);

        // Record the merge this repo's engine made: style is one of the
        // merge engine's words ("fast-forward", "squash", "merge"), oid is
        // the merged commit, by is who merged it. The record's state moves
        // to "merged"; the caller moves the refs and bumps the rev.
        bool markMerged(const std::string &repoDir, long long n,
                        const std::string &style, const std::string &oid,
                        const std::string &by, nlohmann::json &out,
                        std::string &error);

        // ------------------------------------------------------------------ the list

        // The count of the pulls whose records state state "open"; merged and
        // closed records count nothing. The folder's absence is a true zero,
        // not a failure. false when the folder would not list or a record it
        // met would not read or parse — the count then stands unweighed, and
        // the asking that weighs a project's letting-go refuses on those
        // words rather than guesses that no pull stands open.
        bool countOpenPulls(const std::string &repoDir, long long &open, std::string &error);

        // The rows a dropdown asks for: {number,title,state,head,base,
        // approvals:{n,threshold},updated_at}, newest-first, default 50 and
        // at most 200 rows, with hasMore when more stand than were asked
        // for. stateFilter is "open", "merged", "closed", or empty for all.
        // A record.json that will not parse is NAMED in the answer —
        // unparseable names the folder — never guessed around. The
        // threshold is passed in, the same way approvalMet takes it: this
        // module never reads meta.json.
        bool listPulls(const std::string &repoDir, const std::string &stateFilter,
                       long long limit, long long threshold, nlohmann::json &rows,
                       nlohmann::json &answer, std::string &error);

        // ---------------------------------------------------------------- approval

        // Whether pull n has the approvals the project asks for. The
        // threshold is passed in — ProjectService reads pulls_threshold from
        // meta.json and hands it here, so this module never reads meta and
        // never includes a ProjectService header.
        bool approvalMet(const std::string &repoDir, long long n,
                        long long threshold);

        // ------------------------------------------------------------- the side file
        //
        // The resolutions of one pull: <repoDir>/ols/pulls/<n>/resolutions.json,
        // a side file beside the record. It is invisible to the listing by
        // construction: openPullsAt reads only <folder>/record.json
        // (ProjectService.cpp:709), listPulls reads only record.json per numeric
        // folder (ProjectPulls.cpp:523), and nextNumber counts only numeric
        // directories (ProjectPulls.cpp:116-165) — a file inside the folder is
        // never listed, never counted, and never parsed as a record.
        //
        // The shape is {at:{head,base},items:[{path,content,action,at,by}]},
        // action "settle" or "delete". The at stamps name the lease the
        // resolutions were made under: the DOOR checks at.head/at.base against
        // the live lease at merge time — this module never checks them, it only
        // carries them.
        //
        // The caps stand beside the law that enforces them: at most 200 items —
        // the merge engine's kMaxMergeConflicts (gitcore/merge.cpp:50) — each
        // item's content at most 2 MiB — the merge engine's kMergeSideBytesCap
        // (gitcore/merge.cpp:51) — and a whole file past the 1 MiB record law
        // is refused with words naming the size, nothing written.

        // The resolutions file as it stands. Absence is nothing resolved: true
        // with out null. A file that will not parse is false with the reason.
        bool readResolutions(const std::string &repoDir, long long n,
                             nlohmann::json &out, std::string &error);

        // The resolutions as written: the items list under the lease stamps.
        // Past the item cap or an item's content cap the refusal names the
        // bound; past 1 MiB the whole file is refused naming the size.
        bool writeResolutions(const std::string &repoDir, long long n,
                              const std::string &atHead, const std::string &atBase,
                              const nlohmann::json &items, nlohmann::json &out,
                              std::string &error);

        // Add or replace one item by path. The stamps of an existing file are
        // kept; a first file stamps from the record's lease (the oids at open).
        // The item wears at = the wall's current second and by = the caller's
        // word.
        bool setResolution(const std::string &repoDir, long long n,
                           const std::string &path, const std::string &content,
                           bool deleteFile, const std::string &by,
                           nlohmann::json &out, std::string &error);

        // No resolutions: the file is rewritten with no items, its stamps kept.
        // A pull with no side file is already clear: true with out null.
        bool clearResolutions(const std::string &repoDir, long long n,
                              nlohmann::json &out, std::string &error);

    } // namespace pulls
} // namespace ols

#endif // OLS_PROJECT_PULLS_H
