// src/ProjectGit.h — the read/browse engine behind the code doors of §5.3:
// what HEAD is, what refs stand, what a commit's tree holds, what its blobs
// are, who wrote each line, and what symbols a blob carries. It reads a
// bare repo dir — data/projects/<slug>/repo — exactly as git.exe itself
// writes one (loose objects, packfiles, packed-refs, a HEAD symref),
// through the gitcore layers; it owns nothing else except this file's
// last-error channel. Every call is independent: nothing here is cached
// across calls and no call holds state. A refused answer says false and
// names its reason in plain words in lastError. Paths handed in are tree
// paths — repo-relative, '/'-joined, never absolute, never wearing a ".."
// segment — and are answered only under repoDir.
//
// Cost bounds, stated where callers can see them: historyFor walks at most
// 2000 commits from its start and says in lastError when it stopped short;
// each row's changed list holds at most 200 files and counts lines only in
// blobs up to 2 MiB — a file past that bound is still named as changed,
// without counts, and a row whose list stopped at the bound says so; a
// row's message holds at most 4 KiB and says when it was cut. blameFile
// walks at most 200 commits and leaves lines older than the walk
// unattributed with the truncated flag set. outlineForBlob scans every
// line of the blob once and nothing more.
#ifndef OLS_PROJECT_PROJECTGIT_H
#define OLS_PROJECT_PROJECTGIT_H

#include <cstddef>
#include <cstdint>
#include <climits>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace ols
{
    namespace project
    {

        // One ref, and where it points. For a symref the target is its immediate
        // target and the oid carries the resolution where the chain resolves; a
        // symref is exactly a row with a non-empty target. Oids are lowercase hex.
        struct RefInfo
        {
            std::string name;   // "HEAD", "refs/heads/main", ...
            std::string oid;    // lowercase hex; empty when nothing resolves
            std::string target; // empty unless this is a symref
        };

        // One child of one directory of a tree. The path is repo-relative
        // ("sub/file"), the mode is the raw mode the tree carries, and isDir reads
        // that mode.
        struct TreeEntry
        {
            std::string path;
            std::string name;
            std::string mode;
            std::string oid; // lowercase hex
            bool isDir = false;
        };

        // One changed file of a row's first-parent diff: the line counts when they
        // were counted, and when they were not, the mark that says why — the
        // content is binary, or one side wears more than the 2 MiB bound. A path
        // whose two sides are not both blobs (a file and a directory trading
        // places) carries neither mark and no counts.
        struct ChangeRow
        {
            std::string path;
            long long added = 0;
            long long deleted = 0;
            bool counted = false;
            bool binary = false;
            bool large = false;
        };

        // One commit, as the code doors read logs. authorWhen and committerWhen
        // are the time texts their lines carry ("1704067200 +0000"), kept as they
        // are read. parents are the commit's parents in their own order. refs are
        // the short names of the refs standing exactly at this commit, decorated
        // the way git decorates: "HEAD -> main", "tag: v1", "HEAD" alone when a
        // detached HEAD rests here. message is the whole message, bounded at
        // 4 KiB, and messageTruncated says when it was cut. files, added and
        // deleted and the changed[] list are the first-parent diff — a root
        // counts everything as added — bounded as the cost bounds above state,
        // with changedTruncated marking the row whose list stopped at the bound.
        struct CommitInfo
        {
            std::string oid;                  // lowercase hex
            std::vector<std::string> parents; // lowercase hex
            std::string authorName;
            std::string authorWhen;
            std::string committerName;
            std::string committerWhen;
            std::string messageFirstLine;
            std::string message;
            bool messageTruncated = false;
            std::vector<std::string> refs;
            long long files = 0;
            long long added = 0;
            long long deleted = 0;
            std::vector<ChangeRow> changed;
            bool changedTruncated = false;
        };

        // Who wrote one line. An unattributed line carries an empty oid. The flag
        // is this call's, not the line's — the signature has one result shape —
        // so a walk that stopped at its bound and left history unwalked sets
        // truncated on every row and says so in lastError.
        struct BlameLine
        {
            std::string commitOid; // lowercase hex; empty when unattributed
            bool truncated = false;
        };

        // HEAD, however it stands: a symref to a branch resolves through it, and a
        // raw oid in HEAD answers as itself.
        bool readHeadOid(const std::string &repoDir, std::string &oidHex);

        // Every ref under refs/ plus HEAD, loose over packed as git resolves, in
        // name order. An empty listing is a repo with no refs, not a refusal.
        bool listRefs(const std::string &repoDir, std::vector<RefInfo> &out);

        // A raw object id checked against the repo before any door reads through it:
        // the name must be forty lowercase hex — sha1's own width, the only shape
        // this module looks objects up by, uppercase and the 64-hex sha256 shape are
        // refused at the shape itself — and it must name a commit object the repo
        // holds, loose or packed; the lowercase hex comes back in outHex. A name not
        // shaped as an id is refused here — a ref name is looked up by listRefs.
        bool resolveCommitOid(const std::string &repoDir, const std::string &oidHex,
                              std::string &outHex);

        // One level of the tree of a commit — lazy: a door walks deeper by asking
        // for the child directory. An empty dirPath is the root; the dirPath must
        // name a directory that the tree holds.
        bool treeAt(const std::string &repoDir, const std::string &commitOid,
                    const std::string &dirPath, std::vector<TreeEntry> &out);

        // A file's bytes at a commit, and the blob oid they belong to — the
        // content-addressed name the doors cache by.
        bool blobAt(const std::string &repoDir, const std::string &commitOid,
                    const std::string &path, std::vector<std::uint8_t> &outBytes,
                    std::string &oidHex);

        // What a history walk is asked for. The needles are substrings matched
        // case-insensitively — authorNeedle against the author name, textNeedle
        // against the whole message. sinceSecs and untilSecs bound the author
        // date in whole seconds. afterOrEmpty names a commit by full oid: the
        // walk shows nothing until it stands on that commit, then shows what
        // comes after it in its own order. limit is the rows to emit. firstParent
        // follows only the first parent of each commit; noMerges never shows a
        // commit that carries more than one parent — a dropped merge still has
        // its lines walked, exactly as git walks them.
        struct HistoryAsk
        {
            std::string pathOrEmpty;
            std::string afterOrEmpty;
            std::size_t limit = 50u;
            bool firstParent = false;
            bool noMerges = false;
            std::string authorNeedle;
            std::string textNeedle;
            long long sinceSecs = LLONG_MIN;
            long long untilSecs = LLONG_MAX;
        };

        // What the walk left beyond the rows. hasMore is the walk's own statement
        // that it stopped with commits still queued or at its bound — the next
        // page may still answer with none of them showing. truncatedWhy joins, in
        // this file's words, every bound that cut the answer; it is empty when
        // nothing was cut.
        struct HistoryTail
        {
            bool hasMore = false;
            std::string truncatedWhy;
        };

        // The log, newest first by the order of the walk: every parent is followed,
        // each commit once, except as the ask's firstParent says. A non-empty
        // pathOrEmpty keeps only the commits that changed that path — its entry
        // oid differing from every parent's. Every row carries the fields the
        // struct above states; the bounds on the walk, the row's changed list and
        // its counts are the cost bounds of this file's head, and every one that
        // cut is named in the tail and, for the walk itself, in lastError too.
        bool historyFor(const std::string &repoDir, const std::string &startOid,
                        const HistoryAsk &ask, std::vector<CommitInfo> &out,
                        HistoryTail &tail);

        // The lines of a file at a commit, each credited to the newest walked
        // commit that introduced it — a commit owns a line when the line is in the
        // file there and in no parent's version of it. The walk is bounded at 200
        // commits; lines older than the walk stay unattributed and the call sets
        // the truncated flag on the rows and says in lastError what it cut.
        bool blameFile(const std::string &repoDir, const std::string &commitOid,
                       const std::string &path, std::vector<BlameLine> &out);

        // The deliberately simple symbol scan the outline door wants: line-based,
        // not a parser. The language comes from the file name's extension; an
        // unknown extension is "text" with no symbols and still true. The oid is
        // the blob's content address — the doors cache by it and this does not.
        bool outlineForBlob(const std::string &oidHex, const std::vector<std::uint8_t> &bytes,
                            const std::string &pathName, nlohmann::json &out);

        // One file of a save as it stands: content is the bytes of the blob it
        // becomes, and deleteFile takes the path away instead. The path is the
        // shape the read half answers — repo-relative, '/'-joined, never
        // absolute, never wearing a ".." segment — and is validated segment by
        // segment exactly as the read half validates.
        struct CommitFile
        {
            std::string path;
            std::string content;
            bool deleteFile = false;
        };

        // The write half: a save lands as a real commit (spec §5.9). The branch
        // — a bare name or refs/heads/… — is read at its current oid; an unborn
        // branch is allowed and builds the root commit, which is how a project's
        // first save gets its first commit. The new tree is the base commit's
        // tree with the files applied as blob writes — later states of one path
        // win, unchanged parts stay by oid, and directories along changed paths
        // are rebuilt with mode 40000; entries are written in git's own order.
        // The commit carries the tree, the old oid as parent when the branch had
        // one, and author and committer both from authorName at the current UTC
        // time. The branch moves through the refs transaction with the old value
        // as expectation, so a save that raced another refuses with the branch
        // having moved under it; the objects it wrote stay loose. The branch's
        // reflog gets the entry for the move. Bounds stated where callers see
        // them: a commit carries at most 200 files, one file's content at most
        // 2 MiB, the message may not be empty, and a save with no files is not
        // a commit. Loose objects only — no packfile, no gc — and Sha1 only,
        // the format every project repo here stands in. The new commit's oid
        // comes back in newCommitOidHex. The race is stated beside the answer:
        // refusedByRace is set when the branch refused the move because it had
        // moved under the save — the door answers that shape as a conflict
        // without reading the words for one, since a file's own name can wear
        // any word the refusal says.
        bool commitFiles(const std::string &repoDir, const std::string &branchName,
                         const std::vector<CommitFile> &files, const std::string &message,
                         const std::string &authorName, std::string &newCommitOidHex,
                         bool &refusedByRace);

        // A branch at a commit the repo holds, created through the same
        // transaction: an existing name is refused — a create never moves — and
        // the same name rules and the same validation as commitFiles.
        bool createBranch(const std::string &repoDir, const std::string &name,
                          const std::string &fromOidHex);

        // Take a branch away: the loose file, and out of packed-refs as well when
        // it stands there — a loose delete alone would leave the packed one
        // reading back. The branch HEAD points at is refused; HEAD moves through
        // its own doors.
        bool deleteBranch(const std::string &repoDir, const std::string &name);

        // Why the last call was refused, in this file's own words; empty when
        // nothing was refused. A walk that stopped at a bound is not a refusal and
        // still answers true; when it does, the cut is named here too.
        std::string lastError();

    } // namespace project
} // namespace ols

#endif // OLS_PROJECT_PROJECTGIT_H
