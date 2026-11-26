// src/gitcore/merge.h — the three-way merge of GitCore: two commits, the base the
// history gives them, and the tree that carries both sides' changes.
//
// The unit opens no file and holds no repository. Objects are reached through
// RepoAccess, the port receive.h names: the commits and trees and blobs are read
// with readObject, and everything the merge makes — the merged blobs, the merged
// trees, the commit that stands over them — goes back through writeLoose. What
// this unit owns is the merge itself: the merge-base as its own bounded ancestor
// walk, the recursive name-by-name tree merge, the line-level three-way over a
// blob both sides changed, and the words of the refusals.
//
// The engine writes objects and never moves a ref. commitRefs stays the door's
// call: a caller that wants the branch moved asks for the move with the oid this
// unit hands back in mergedCommitOid. That is why the port is held as a const
// reference and why a fast-forward answers with the head's own oid — no ref has
// to be anywhere for that answer to be true.
//
// The bounds are stated beside the engine and named in what they refuse: the
// commits an ancestry walk may read, the folders a tree walk may open, the
// conflicting paths one answer may carry, the bytes of one side whose text the
// answer will not hold. A side past the last of these is still named as a
// conflict, without its text. A line diff that came back from the fallback —
// approx, as diff.h says it — refuses its path as a budget conflict; a merge is
// never built on an answer the diff unit could not make exactly.
//
// Nothing here claims byte agreement with git's own merge. The tree it writes is
// the tree this unit's rules describe, and the commit is the commit those rules
// describe; a conflict names the path and the three sides it found, which is what
// a caller needs to answer a person.
//
// No call in this namespace throws. An object the port cannot read, an ancestry
// past the walk, and a pair of commits with no common ancestor are false with the
// reason in the error string. A merge that conflicts is not one of those: it was
// answered, and its answer is clean:false with the conflicts it found.

#ifndef OLS_GITCORE_MERGE_H
#define OLS_GITCORE_MERGE_H

#include <cstddef>
#include <string>
#include <vector>

#include "gitcore/receive.h" // RepoAccess: the port this unit reads and writes through

namespace ols
{
    namespace gitcore
    {

        // ------------------------------------------------------------------- the styles
        //
        // How the caller wants the answer shaped. A fast-forward makes no object at
        // all; a squash makes one commit with the target as its only parent; a merge
        // commit makes one with the target first and the head behind it.

        enum class MergeStyle
        {
            FastForward, // only legal when the target stands under the head
            Squash,      // one commit, one parent: the target
            MergeCommit  // one commit, two parents: the target, then the head
        };

        // ------------------------------------------------------------------ a conflict
        //
        // One path the merge could not settle. The three oids are as the trees carried
        // them, empty where that side does not have the path at all. The three texts
        // are the bytes of the three sides when the answer carries them: a side that is
        // absent, or over the size a text is carried at, leaves its text empty.
        //
        // The kind says which law the path fell under:
        //   "both-changed"      the two sides changed the same lines differently
        //   "add-add"           neither side had it in the base and the two added
        //                       different bytes
        //   "edit-vs-delete"    one side deleted what the other changed
        //   "file-vs-directory" the same name is a folder on one side and a file on
        //                       another
        //   "budget"            a side too big to carry, or a line diff the diff unit
        //                       could not make exactly

        struct MergeConflict
        {
            std::string path; // repo-relative, '/'-joined, as a tree path reads
            std::string baseOid;
            std::string oursOid;
            std::string theirsOid;
            std::string baseText;
            std::string oursText;
            std::string theirsText;
            std::string kind;
        };

        // ---------------------------------------------------------------- the resolution
        //
        // One path the caller settled for the merge. The content is the bytes the
        // settled blob carries; deleteFile is the other answer: the path does not
        // stand at all. The word is deleteFile because delete is a reserved word.

        struct MergeResolution
        {
            std::string path; // repo-relative, '/'-joined, as a tree path reads
            std::string content;
            bool deleteFile = false;
        };

        // ------------------------------------------------------------------- the answer
        //
        // What the merge decided. clean is true when no path conflicted, and only then
        // are mergedTreeOid and mergedCommitOid the objects the merge wrote. When the
        // merge brought nothing new — its tree is the target's tree — clean is true and
        // no object is written and neither oid beyond the base is filled: the caller has
        // nothing to move. multipleBases says the history offered more than one maximal
        // common commit; the merge went ahead on the first of them, which the base
        // commit oid names. truncated says the conflicts were more than one answer
        // carries.

        struct MergeOutcome
        {
            bool clean = false;
            std::string baseCommitOid;  // the merge-base the walk settled on
            std::string mergedTreeOid;  // written by this unit, or carried unchanged
            std::string mergedCommitOid;// the commit the caller moves the ref to
            std::vector<MergeConflict> conflicts;
            bool multipleBases = false;
            bool truncated = false;
            // With resolutions carried in: which paths the merge used and which
            // it did not. A resolution whose path never conflicted is unused —
            // the merge still answers clean when nothing else conflicted.
            std::vector<std::string> took, unused;
        };

        // ----------------------------------------------------------------------- merge
        //
        // Merge the head into the target tip. baseRefOid is the tip the target ref holds
        // — the "ours" of the merge, and the parent the new commit stands on. headOid is
        // the commit being pulled in — the "theirs". Both are lowercase hex of one width.
        //
        // The styles answer plainly:
        //   FastForward  legal only when the target is an ancestor of the head. No
        //                object is written and mergedCommitOid is the head itself; the
        //                caller moves the ref. Otherwise: "this pull is not a
        //                fast-forward: the base does not stand under the target".
        //   Squash       the merged tree under one commit whose parent is the target.
        //   MergeCommit  the merged tree under one commit whose parents are the target
        //                and then the head.
        //
        // The commit's message is the subject the caller hands and this unit's own
        // words beside it: "Merge <head-oid>" and "Squash <head-oid> into
        // <target-oid>", each with the trailing newline git keeps. A caller that
        // hands no subject gets these words alone; a caller that hands one gets
        // the commit carrying that line first and these words as its second
        // paragraph. Author and committer are both "studio" at the wall's current
        // UTC second, the shape every commit this server makes wears.
        //
        // false is the refusal, with the reason in error: an id that is not hex, an
        // object the port cannot read where the merge needs it, an ancestry past the
        // walk's bound, a tree past the folder bound, and two commits with no common
        // ancestor — "these branches have no common ancestor: the merge has no base".
        // true carries the answer in out, including the answer that it conflicted.

        bool mergePull(const RepoAccess &repo, const std::string &baseRefOid,
                       const std::string &headOid, MergeStyle style, MergeOutcome &out,
                       std::string &error, const std::string &subject = std::string());

        // The same merge with the settled paths carried in. A settled path takes
        // the resolution's answer instead of being noted as a conflict: settle
        // plans the resolved blob and stands the entry, deleteFile omits it. A
        // resolution whose path never conflicted is answered in out.unused; the
        // merge still answers clean when nothing else conflicted. out.took names
        // the paths the merge used.
        bool mergePull(const RepoAccess &repo, const std::string &baseRefOid,
                       const std::string &headOid, MergeStyle style,
                       const std::vector<MergeResolution> &resolutions, MergeOutcome &out,
                       std::string &error, const std::string &subject = std::string());

    } // namespace gitcore
} // namespace ols

#endif // OLS_GITCORE_MERGE_H
