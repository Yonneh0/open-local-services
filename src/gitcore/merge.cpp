// src/gitcore/merge.cpp — the ground merge.h stands on.
//
// The definitions stand external so the server build, which names this file
// among its sources, reads their symbols from its object, and so the harness —
// which names this file too and includes it nowhere — resolves them from the same
// single object. No case includes this file: merge.h is the header a case reads,
// and nothing here is inline.
//
// The shape of the walk and of the tree descent follows what already stands in
// this folder: the ancestor walk is receive.cpp's file-local walkCommits in the
// same dress (a stack, a visited set, a bound that refuses rather than guesses),
// and the name-by-name pairing of three trees follows the two-way pairing
// ProjectGit.cpp's collectTreeDiff keeps. Both are stood here again rather than
// reached across a unit: what they were for is not what this is for, and a
// private walk is cheaper to state than a shared one to reconcile.
//
// The merge plans before it writes. Every blob, tree, and commit it will make is
// hashed here from its own bytes — the object layer's sha1Hex over its own header,
// the same bytes writeLoose is handed — and nothing is written until the whole
// merge is known to be clean. A merge that conflicts therefore leaves no object
// behind, and a merge that stands writes exactly the objects its answer names.

#include "merge.h"

#include "gitcore/diff.h"
#include "gitcore/objects.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ols
{
    namespace gitcore
    {

        namespace
        {

            // The bounds the merge takes: past one, the answer the merge was making is
            // not a claim, and the refusal or the conflict says so.
            constexpr std::size_t kMaxMergeBaseWalk = 65536u;  // commits an ancestry walk reads
            constexpr std::size_t kMaxMergeTreeWalk = 65536u;  // folders a tree merge opens
            constexpr std::size_t kMaxMergeConflicts = 200u;   // paths one answer carries
            constexpr std::size_t kMergeSideBytesCap = 2u * 1024u * 1024u; // bytes of a side
                                                                        // whose text is carried

            // One pull's shared state. The port is held writable for the writing at the
            // end; the plans the merge makes are held here until that moment.
            struct MergeWork
            {
                explicit MergeWork(RepoAccess &portIn) : port(portIn) {}

                RepoAccess &port;
                std::vector<MergeConflict> conflicts;
                bool truncated = false;    // more paths conflicted than the answer carries
                std::size_t folders = 0u;  // folders the tree walk has opened
                bool stopWalk = false;     // the cap was reached: every level stops at once
                std::string error;         // why the walk gave up, carried out to mergePull

                // An object the merge will write: its kind word, its bytes, and the id
                // those bytes state. writeLoose is asked to agree with that id.
                struct Planned
                {
                    std::string kind;
                    std::string content;
                    std::string oid;
                };
                std::vector<Planned> pending;

                // The paths the caller settled, and the resolutions the merge
                // used. The settled map points into the caller's vector for as
                // long as the merge runs; took holds the very elements it used.
                std::map<std::string, const MergeResolution *> settled;
                std::set<const MergeResolution *> took;
            };

            // The id the object layer gives a kind and its bytes: the header and the
            // content hashed together, which is what the port hashes when it files them.
            std::string planOid(const std::string &kind, const std::string &content)
            {
                ObjectKind objectKind = ObjectKind::Blob;
                std::string why;
                if (!kindOfName(kind, objectKind, why))
                {
                    return std::string();
                }
                return sha1Hex(objectHeaderMake(objectKind, content.size()) + content);
            }

            // ---------------------------------------------------------------- the reads

            bool readCommitOf(const RepoAccess &repo, const std::string &oid, CommitObject &out,
                              std::string &error)
            {
                std::string kind, content;
                if (!repo.readObject(oid, kind, content))
                {
                    error = "the merge cannot read the commit " + oid;
                    return false;
                }
                if (kind != "commit")
                {
                    error = "the object " + oid + " is not a commit";
                    return false;
                }
                if (!commitParse(content, out, error))
                {
                    error = "the commit " + oid + " is not in a form this merge can take: " + error;
                    return false;
                }
                return true;
            }

            bool readTreeOf(const RepoAccess &repo, const std::string &oid, TreeObject &out,
                            std::string &error)
            {
                std::string kind, content;
                if (!repo.readObject(oid, kind, content))
                {
                    error = "the merge cannot read the tree " + oid;
                    return false;
                }
                if (kind != "tree")
                {
                    error = "the object " + oid + " is not a tree";
                    return false;
                }
                if (!treeParse(content, out, error))
                {
                    error = "the tree " + oid + " is not in a form this merge can take: " + error;
                    return false;
                }
                return true;
            }

            bool readBlobOf(const RepoAccess &repo, const std::string &oid, std::string &text,
                            std::string &error)
            {
                std::string kind, content;
                if (!repo.readObject(oid, kind, content))
                {
                    error = "the merge cannot read the blob " + oid;
                    return false;
                }
                if (kind != "blob")
                {
                    error = "the object " + oid + " is not a blob";
                    return false;
                }
                text = content;
                return true;
            }

            // ------------------------------------------------------------- the ancestry
            //
            // Parents from one commit through what the port holds: visited takes the ids
            // the walk read, and false says the bound was reached, which is not a claim
            // that the history does not continue. A tag is walked through what it tags; an
            // object the walk cannot read ends that branch.
            bool walkAncestors(const RepoAccess &repo, const std::string &start,
                               std::set<std::string> &visited)
            {
                std::vector<std::string> stack;
                stack.push_back(start);
                while (!stack.empty())
                {
                    const std::string oid = stack.back();
                    stack.pop_back();
                    if (!visited.insert(oid).second)
                    {
                        continue;
                    }
                    if (visited.size() > kMaxMergeBaseWalk)
                    {
                        return false;
                    }
                    std::string kind, content;
                    if (!repo.readObject(oid, kind, content))
                    {
                        continue;
                    }
                    if (kind == "commit")
                    {
                        CommitObject commit;
                        std::string why;
                        if (!commitParse(content, commit, why))
                        {
                            continue;
                        }
                        for (const ObjectId &parent : commit.parents)
                        {
                            stack.push_back(parent.to_hex());
                        }
                    }
                    else if (kind == "tag")
                    {
                        TagObject tag;
                        std::string why;
                        if (!tagParse(content, tag, why))
                        {
                            continue;
                        }
                        stack.push_back(tag.target.to_hex());
                    }
                }
                return true;
            }

            // The merge-base of two commits. Each side's ancestors are walked to the bound
            // and the two sets intersected; of the common commits the maximal ones are
            // kept — those no other common commit descends from, which is the same as
            // saying: a common commit an ancestor of another common commit is not one. The
            // walk of one common commit's parents never leaves the common set, because
            // every ancestor of a common commit is an ancestor of both tips.
            bool settleMergeBase(const RepoAccess &repo, const std::set<std::string> &fromHead,
                                 const std::set<std::string> &fromTarget, std::string &baseOut,
                                 bool &multipleBases, std::string &error)
            {
                std::set<std::string> common;
                for (const std::string &oid : fromHead)
                {
                    if (fromTarget.count(oid) != 0u)
                    {
                        common.insert(oid);
                    }
                }
                if (common.empty())
                {
                    error = "these branches have no common ancestor: the merge has no base";
                    return false;
                }

                std::set<std::string> descended;
                std::set<std::string> expanded; // taken once each: a cycle cannot spin the walk
                std::map<std::string, std::vector<std::string>> parentsOf;
                std::size_t read = 0u;
                for (std::set<std::string>::const_iterator at = common.begin(); at != common.end();
                     ++at)
                {
                    if (descended.count(*at) != 0u)
                    {
                        continue;
                    }
                    std::vector<std::string> stack;
                    expanded.insert(*at);
                    stack.push_back(*at);
                    while (!stack.empty())
                    {
                        const std::string oid = stack.back();
                        stack.pop_back();
                        std::map<std::string, std::vector<std::string>>::const_iterator known =
                            parentsOf.find(oid);
                        if (known == parentsOf.end())
                        {
                            if (++read > kMaxMergeBaseWalk)
                            {
                                error = "the merge base cannot be settled within the walk of " +
                                        std::to_string(kMaxMergeBaseWalk) + " commits";
                                return false;
                            }
                            std::vector<std::string> parents;
                            std::string kind, content;
                            if (repo.readObject(oid, kind, content) && kind == "commit")
                            {
                                CommitObject commit;
                                std::string why;
                                if (commitParse(content, commit, why))
                                {
                                    for (const ObjectId &parent : commit.parents)
                                    {
                                        parents.push_back(parent.to_hex());
                                    }
                                }
                            }
                            known = parentsOf.insert(std::make_pair(oid, parents)).first;
                        }
                        for (const std::string &parent : known->second)
                        {
                            if (common.count(parent) == 0u)
                            {
                                continue;
                            }
                            descended.insert(parent);
                            if (expanded.count(parent) != 0u)
                            {
                                continue; // its ancestors were marked by the walk that
                                          // reached it first
                            }
                            expanded.insert(parent);
                            stack.push_back(parent);
                        }
                    }
                }

                std::string base;
                std::size_t bases = 0u;
                for (std::set<std::string>::const_iterator at = common.begin(); at != common.end();
                     ++at)
                {
                    if (descended.count(*at) != 0u)
                    {
                        continue;
                    }
                    if (bases == 0u)
                    {
                        base = *at;
                    }
                    ++bases;
                }
                // A history with a cycle in it would leave nothing standing; the common
                // set itself is then the answer, and nothing here claims it is one base.
                if (bases == 0u)
                {
                    base = *common.begin();
                    bases = 1u;
                }
                multipleBases = bases > 1u;
                baseOut = base;
                return true;
            }

            // ------------------------------------------------------------ the line merge
            //
            // One side's change to a file as the hunks state it, cut down to what
            // changed: the hunk's unchanged lines at either edge are context, and the
            // range they describe is not part of the change. An edit says: base lines
            // [start,end) give way to these lines. An insertion keeps an empty range.
            struct LineEdit
            {
                int start = 0;
                int end = 0;
                std::vector<std::string> lines;
                std::vector<bool> noNewline; // one per line: it was the source's last, bare
            };

            // The edits of one side, from the diff of base into that side. The side tells
            // whether its last line lacked its newline, so a line that reaches the end of
            // the merged file carries that fact with it.
            void editsOf(const DiffResult &diff, const LineData &side, std::vector<LineEdit> &out)
            {
                out.clear();
                for (const Hunk &hunk : diff.hunks)
                {
                    const int firstBase = (hunk.countA == 0) ? hunk.startA : hunk.startA - 1;

                    std::size_t lead = 0u;
                    while (lead < hunk.lines.size() && hunk.lines[lead].kind == 'c')
                    {
                        ++lead;
                    }
                    std::size_t tail = 0u;
                    while (tail < hunk.lines.size() - lead &&
                           hunk.lines[hunk.lines.size() - 1u - tail].kind == 'c')
                    {
                        ++tail;
                    }

                    LineEdit edit;
                    edit.start = firstBase + static_cast<int>(lead);
                    edit.end = firstBase + static_cast<int>(hunk.countA) - static_cast<int>(tail);
                    if (edit.end < edit.start)
                    {
                        edit.end = edit.start;
                    }
                    // The b-side line a hunk line stands for: the hunk's b range walked
                    // alongside the lines, counted over the unchanged and the added ones.
                    // Only the side's own last line can be the one that lacks its newline.
                    const int firstB = (hunk.countB == 0) ? hunk.startB : hunk.startB - 1;
                    int bSeen = 0;
                    const std::size_t stop = hunk.lines.size() - tail;
                    for (std::size_t at = lead; at < stop; ++at)
                    {
                        const HunkLine &line = hunk.lines[at];
                        if (line.kind == 'c')
                        {
                            ++bSeen;
                            continue;
                        }
                        if (line.kind != 'a')
                        {
                            continue;
                        }
                        edit.lines.push_back(line.text);
                        edit.noNewline.push_back(side.noNewline && !side.lines.empty() &&
                                                 firstB + bSeen ==
                                                     static_cast<int>(side.lines.size() - 1u));
                        ++bSeen;
                    }
                    if (edit.lines.empty() && edit.start == edit.end)
                    {
                        continue;
                    }
                    out.push_back(edit);
                }
            }

            // The three-way merge of one file's lines. The two sides' edits are applied to
            // the base together: ranges that do not meet are both applied, a range the two
            // sides changed the same way is applied once, and a range they changed
            // differently is the conflict this reports. The answer says which of the two it
            // is; a third answer, that the diff unit could not make the change exactly, is
            // settled by the caller before this is reached.
            bool mergeLines(const std::string &baseText, const std::string &oursText,
                            const std::string &theirsText, std::string &mergedOut, bool &conflicted)
            {
                conflicted = false;
                const LineData base = split_lines(baseText);
                const LineData ours = split_lines(oursText);
                const LineData theirs = split_lines(theirsText);

                std::vector<LineEdit> ourEdits;
                std::vector<LineEdit> theirEdits;
                editsOf(diff_lines(base, ours), ours, ourEdits);
                editsOf(diff_lines(base, theirs), theirs, theirEdits);

                std::vector<std::pair<std::string, bool>> kept;
                const std::size_t n = base.lines.size();
                std::size_t i = 0u;
                std::size_t oi = 0u;
                std::size_t ti = 0u;
                while (i < n || oi < ourEdits.size() || ti < theirEdits.size())
                {
                    const bool useOurs = oi < ourEdits.size();
                    const bool useTheirs = ti < theirEdits.size();

                    // The next thing to do is at the lowest base line either side reaches
                    // for; unchanged lines in front of it are carried as they stand.
                    int next = static_cast<int>(n);
                    if (useOurs)
                    {
                        next = (std::min)(next, ourEdits[oi].start);
                    }
                    if (useTheirs)
                    {
                        next = (std::min)(next, theirEdits[ti].start);
                    }
                    while (static_cast<int>(i) < next && i < n)
                    {
                        kept.push_back(std::make_pair(
                            base.lines[i], i + 1u == n && base.noNewline));
                        ++i;
                    }
                    if (!useOurs && !useTheirs)
                    {
                        break;
                    }

                    const int at = next;
                    const bool oursHere = useOurs && ourEdits[oi].start == at;
                    const bool theirsHere = useTheirs && theirEdits[ti].start == at;

                    // Two edits that meet on the base: the same change applied once, a
                    // different one is the conflict.
                    if (oursHere && theirsHere)
                    {
                        const LineEdit &o = ourEdits[oi];
                        const LineEdit &t = theirEdits[ti];
                        const bool same =
                            o.start == t.start && o.end == t.end && o.lines == t.lines;
                        if (!same)
                        {
                            conflicted = true;
                            return true;
                        }
                        for (std::size_t at2 = 0u; at2 < o.lines.size(); ++at2)
                        {
                            kept.push_back(
                                std::make_pair(o.lines[at2], static_cast<bool>(o.noNewline[at2])));
                        }
                        i = static_cast<std::size_t>((std::max)(o.end, t.end));
                        ++oi;
                        ++ti;
                        continue;
                    }

                    // One side's change, applied where it stands. It may only be applied
                    // whole: the other side reaching into the range it covers — even at the
                    // line where it starts — is the two sides meeting, and a merge does not
                    // guess which of the two lines.
                    const LineEdit &take = oursHere ? ourEdits[oi] : theirEdits[ti];
                    const std::vector<LineEdit> &other = oursHere ? theirEdits : ourEdits;
                    const std::size_t otherAt = oursHere ? ti : oi;
                    if (otherAt < other.size() && other[otherAt].start < take.end)
                    {
                        conflicted = true;
                        return true;
                    }
                    for (std::size_t at2 = 0u; at2 < take.lines.size(); ++at2)
                    {
                        kept.push_back(std::make_pair(
                            take.lines[at2], static_cast<bool>(take.noNewline[at2])));
                    }
                    i = static_cast<std::size_t>(take.end);
                    if (oursHere)
                    {
                        ++oi;
                    }
                    else
                    {
                        ++ti;
                    }
                }

                std::string merged;
                for (std::size_t at = 0u; at < kept.size(); ++at)
                {
                    merged += kept[at].first;
                    if (!kept[at].second)
                    {
                        merged += '\n';
                    }
                }
                mergedOut = merged;
                return true;
            }

            // ------------------------------------------------------------ what conflicts
            //
            // The texts a conflict carries, when the answer carries them at all. A side
            // that is absent has none; a side the merge cannot read, or one over the size
            // a text is carried at, leaves the whole set out rather than half a picture.
            struct SideTexts
            {
                std::string base;
                std::string ours;
                std::string theirs;
            };

            bool carryTexts(const RepoAccess &repo, const TreeEntry *b, const TreeEntry *o,
                            const TreeEntry *t, SideTexts &hold)
            {
                const TreeEntry *sides[3] = {b, o, t};
                std::string *into[3] = {&hold.base, &hold.ours, &hold.theirs};
                for (std::size_t at = 0u; at < 3u; ++at)
                {
                    if (sides[at] == nullptr)
                    {
                        continue;
                    }
                    if (sides[at]->id.to_hex().empty())
                    {
                        return false;
                    }
                    std::string kind, content;
                    if (!repo.readObject(sides[at]->id.to_hex(), kind, content) || kind != "blob")
                    {
                        return false;
                    }
                    if (content.size() > kMergeSideBytesCap)
                    {
                        return false;
                    }
                    *into[at] = content;
                }
                return true;
            }

            // One path the merge could not settle. Past the bound of paths one answer
            // carries, the flag says so and every level stops at once: the answer is the
            // head of the list and the word that it was cut.
            void noteConflict(MergeWork &work, const std::string &path, const TreeEntry *b,
                              const TreeEntry *o, const TreeEntry *t, const char *kind,
                              const SideTexts *texts)
            {
                if (work.conflicts.size() >= kMaxMergeConflicts)
                {
                    work.truncated = true;
                    work.stopWalk = true;
                    return;
                }
                MergeConflict one;
                one.path = path;
                if (b != nullptr)
                {
                    one.baseOid = b->id.to_hex();
                }
                if (o != nullptr)
                {
                    one.oursOid = o->id.to_hex();
                }
                if (t != nullptr)
                {
                    one.theirsOid = t->id.to_hex();
                }
                if (texts != nullptr)
                {
                    one.baseText = texts->base;
                    one.oursText = texts->ours;
                    one.theirsText = texts->theirs;
                }
                one.kind = kind;
                work.conflicts.push_back(one);
            }

            // A conflict where the texts have to be read for: a path one side deleted, or
            // added, or made into a folder — the law already settled which, and the texts
            // are only what the answer carries with it.
            void noteConflictWithTexts(MergeWork &work, const RepoAccess &repo,
                                       const std::string &path, const TreeEntry *b,
                                       const TreeEntry *o, const TreeEntry *t, const char *kind)
            {
                SideTexts hold;
                const bool carried = carryTexts(repo, b, o, t, hold);
                noteConflict(work, path, b, o, t, kind, carried ? &hold : nullptr);
            }

            // A settled path answers the conflict before it is noted: the resolved
            // blob is planned through the same dress the line merge plans its own —
            // the plans-before-writes law holds, nothing is written here — and the
            // entry stands with the mode the side that has the path carries, ours'
            // when ours has it. deleteFile stands for the absence of the entry.
            // The resolution is marked used either way.
            bool settlePath(MergeWork &work, const std::string &path, const TreeEntry *b,
                            const TreeEntry *o, const TreeEntry *t, TreeObject &built)
            {
                const std::map<std::string, const MergeResolution *>::const_iterator at =
                    work.settled.find(path);
                if (at == work.settled.end())
                {
                    return false;
                }
                const MergeResolution *r = at->second;
                work.took.insert(r);
                if (!r->deleteFile)
                {
                    const std::string blobOid = planOid("blob", r->content);
                    if (blobOid.empty())
                    {
                        work.error = "the resolved blob did not come back with an object id";
                        return true;
                    }
                    MergeWork::Planned plan;
                    plan.kind = "blob";
                    plan.content = r->content;
                    plan.oid = blobOid;
                    work.pending.push_back(plan);
                    TreeEntry out;
                    out.mode = (o != nullptr) ? o->mode : t->mode;
                    out.name = (o != nullptr) ? o->name : t->name;
                    out.id = ObjectId(blobOid);
                    built.entries.push_back(out);
                }
                return true;
            }

            // --------------------------------------------------------------- the trees
            //
            // Two entries the same: the same bytes under the same mode.
            bool entrySame(const TreeEntry *x, const TreeEntry *y)
            {
                return x != nullptr && y != nullptr && x->mode == y->mode && x->id == y->id;
            }

            bool treeSame(const TreeObject &a, const TreeObject &b)
            {
                if (a.entries.size() != b.entries.size())
                {
                    return false;
                }
                for (std::size_t at = 0u; at < a.entries.size(); ++at)
                {
                    const TreeEntry &x = a.entries[at];
                    const TreeEntry &y = b.entries[at];
                    if (x.mode != y.mode || x.name != y.name || x.id != y.id)
                    {
                        return false;
                    }
                }
                return true;
            }

            // The merged folder put among the objects to write: sorted the way git keeps a
            // tree, serialized to the bytes that say it, and hashed to the id its entries
            // stand under. The write itself waits for the whole merge to be clean.
            bool planTree(MergeWork &work, TreeObject &folder, std::string &oid,
                          std::string &error)
            {
                std::sort(folder.entries.begin(), folder.entries.end(),
                          [](const TreeEntry &a, const TreeEntry &b) {
                              return treeEntryCompare(a, b) < 0;
                          });
                std::string content;
                if (!treeSerialize(folder, content, error))
                {
                    error = "the merged tree is not in a form git writes: " + error;
                    return false;
                }
                oid = planOid("tree", content);
                if (oid.empty())
                {
                    error = "the merged tree did not come back with an object id";
                    return false;
                }
                MergeWork::Planned plan;
                plan.kind = "tree";
                plan.content = content;
                plan.oid = oid;
                work.pending.push_back(plan);
                return true;
            }

            // A blob both sides changed, the base, and the line merge of the two changes.
            // A side over the bytes a text is carried at, and a diff the diff unit answered
            // from its fallback, both refuse the path as a budget conflict: the merge is
            // never built on an answer that was not exact.
            void mergeBlobThree(MergeWork &work, const std::string &path, const TreeEntry *b,
                                const TreeEntry *o, const TreeEntry *t, TreeObject &built)
            {
                std::string baseText, oursText, theirsText;
                if (!readBlobOf(work.port, b->id.to_hex(), baseText, work.error) ||
                    !readBlobOf(work.port, o->id.to_hex(), oursText, work.error) ||
                    !readBlobOf(work.port, t->id.to_hex(), theirsText, work.error))
                {
                    return;
                }
                if (baseText.size() > kMergeSideBytesCap || oursText.size() > kMergeSideBytesCap ||
                    theirsText.size() > kMergeSideBytesCap)
                {
                    if (settlePath(work, path, b, o, t, built))
                    {
                        return;
                    }
                    noteConflict(work, path, b, o, t, "budget", nullptr);
                    return;
                }

                const LineData baseLines = split_lines(baseText);
                const LineData oursLines = split_lines(oursText);
                const LineData theirsLines = split_lines(theirsText);
                const DiffResult ourDiff = diff_lines(baseLines, oursLines);
                const DiffResult theirDiff = diff_lines(baseLines, theirsLines);
                if (ourDiff.approx || theirDiff.approx)
                {
                    if (settlePath(work, path, b, o, t, built))
                    {
                        return;
                    }
                    SideTexts hold;
                    hold.base = baseText;
                    hold.ours = oursText;
                    hold.theirs = theirsText;
                    noteConflict(work, path, b, o, t, "budget", &hold);
                    return;
                }

                std::string merged;
                bool conflicted = false;
                if (!mergeLines(baseText, oursText, theirsText, merged, conflicted) || conflicted)
                {
                    if (settlePath(work, path, b, o, t, built))
                    {
                        return;
                    }
                    SideTexts hold;
                    hold.base = baseText;
                    hold.ours = oursText;
                    hold.theirs = theirsText;
                    noteConflict(work, path, b, o, t, "both-changed", &hold);
                    return;
                }

                const std::string blobOid = planOid("blob", merged);
                if (blobOid.empty())
                {
                    work.error = "the merged blob did not come back with an object id";
                    return;
                }
                MergeWork::Planned plan;
                plan.kind = "blob";
                plan.content = merged;
                plan.oid = blobOid;
                work.pending.push_back(plan);

                // The mode follows the content: a mode their side changed is kept with their
                // change, and otherwise the mode ours stands with.
                TreeEntry out;
                out.mode = (t->mode != b->mode) ? t->mode : o->mode;
                out.name = o->name;
                out.id = ObjectId(blobOid);
                built.entries.push_back(out);
            }

            // The merge of one folder, three sides paired by name — the way
            // collectTreeDiff pairs two. Every name comes from one of the three trees, and
            // the laws answer in the plain order they are stated: the two sides agree, so
            // take it; a side equals the base, so the other side's answer is the change; a
            // name is a folder on one side and a thing on another, which no line merge can
            // settle; two folders, so go in; three different blobs, so merge the lines.
            bool mergeTrees(MergeWork &work, const std::string &baseOid, const std::string &oursOid,
                            const std::string &theirsOid, const std::string &prefix,
                            TreeObject &built)
            {
                if (work.stopWalk)
                {
                    return true;
                }
                if (++work.folders > kMaxMergeTreeWalk)
                {
                    work.error = "the merge cannot be settled within the tree walk of " +
                                 std::to_string(kMaxMergeTreeWalk) + " folders";
                    return false;
                }

                TreeObject baseTree, oursTree, theirsTree;
                if (!baseOid.empty() && !readTreeOf(work.port, baseOid, baseTree, work.error))
                {
                    return false;
                }
                if (!oursOid.empty() && !readTreeOf(work.port, oursOid, oursTree, work.error))
                {
                    return false;
                }
                if (!theirsOid.empty() &&
                    !readTreeOf(work.port, theirsOid, theirsTree, work.error))
                {
                    return false;
                }

                auto byName = [](const TreeEntry *x, const TreeEntry *y) {
                    return x->name < y->name;
                };
                std::vector<const TreeEntry *> eb, eo, et;
                eb.reserve(baseTree.entries.size());
                eo.reserve(oursTree.entries.size());
                et.reserve(theirsTree.entries.size());
                for (const TreeEntry &e : baseTree.entries)
                {
                    eb.push_back(&e);
                }
                for (const TreeEntry &e : oursTree.entries)
                {
                    eo.push_back(&e);
                }
                for (const TreeEntry &e : theirsTree.entries)
                {
                    et.push_back(&e);
                }
                std::sort(eb.begin(), eb.end(), byName);
                std::sort(eo.begin(), eo.end(), byName);
                std::sort(et.begin(), et.end(), byName);

                std::size_t i = 0u, j = 0u, k = 0u;
                while (i < eb.size() || j < eo.size() || k < et.size())
                {
                    std::string name;
                    if (j < eo.size() && (name.empty() || eo[j]->name < name))
                    {
                        name = eo[j]->name;
                    }
                    if (k < et.size() && (name.empty() || et[k]->name < name))
                    {
                        name = et[k]->name;
                    }
                    if (i < eb.size() && (name.empty() || eb[i]->name < name))
                    {
                        name = eb[i]->name;
                    }

                    const TreeEntry *b = nullptr;
                    const TreeEntry *o = nullptr;
                    const TreeEntry *t = nullptr;
                    if (i < eb.size() && eb[i]->name == name)
                    {
                        b = eb[i];
                        ++i;
                    }
                    if (j < eo.size() && eo[j]->name == name)
                    {
                        o = eo[j];
                        ++j;
                    }
                    if (k < et.size() && et[k]->name == name)
                    {
                        t = et[k];
                        ++k;
                    }

                    const std::string path = prefix + name;
                    const bool bDir = b != nullptr && treeModeDirectory(b->mode);
                    const bool oDir = o != nullptr && treeModeDirectory(o->mode);
                    const bool tDir = t != nullptr && treeModeDirectory(t->mode);

                    // The two sides made the same thing of the name: it stands as they made
                    // it, whether or not the base had it and whatever the base held.
                    if (entrySame(o, t))
                    {
                        built.entries.push_back(*o);
                        continue;
                    }
                    // A side that left the name as the base had it: the other side's answer
                    // is the change, and its absence is that side's deletion.
                    if (entrySame(o, b))
                    {
                        if (t != nullptr)
                        {
                            built.entries.push_back(*t);
                        }
                        continue;
                    }
                    if (entrySame(t, b))
                    {
                        if (o != nullptr)
                        {
                            built.entries.push_back(*o);
                        }
                        continue;
                    }
                    // Both sides deleted it.
                    if (o == nullptr && t == nullptr)
                    {
                        continue;
                    }
                    // One side has the name and the other does not. With nothing in the
                    // base that is an addition one side made and the other never
                    // contested: it stands. With the base holding it, one side deleted
                    // what the other changed.
                    if (o == nullptr || t == nullptr)
                    {
                        if (b == nullptr)
                        {
                            built.entries.push_back(*(o != nullptr ? o : t));
                            continue;
                        }
                        if (settlePath(work, path, b, o, t, built))
                        {
                            if (!work.error.empty())
                            {
                                return false;
                            }
                            continue;
                        }
                        noteConflictWithTexts(work, work.port, path, b, o, t, "edit-vs-delete");
                        continue;
                    }
                    // Neither side had it in the base and the two added different bytes.
                    if (b == nullptr && !oDir && !tDir)
                    {
                        if (settlePath(work, path, b, o, t, built))
                        {
                            if (!work.error.empty())
                            {
                                return false;
                            }
                            continue;
                        }
                        noteConflictWithTexts(work, work.port, path, b, o, t, "add-add");
                        continue;
                    }
                    // A folder one place and a thing another.
                    if (bDir != oDir || bDir != tDir || oDir != tDir)
                    {
                        if (settlePath(work, path, b, o, t, built))
                        {
                            if (!work.error.empty())
                            {
                                return false;
                            }
                            continue;
                        }
                        noteConflict(work, path, b, o, t, "file-vs-directory", nullptr);
                        continue;
                    }
                    // Two folders — three, counting the base: in and merge them name by name.
                    if (oDir && tDir)
                    {
                        TreeObject child;
                        if (!mergeTrees(work, b == nullptr ? std::string() : b->id.to_hex(),
                                        o->id.to_hex(), t->id.to_hex(), path + "/", child))
                        {
                            return false;
                        }
                        if (work.stopWalk)
                        {
                            return true;
                        }
                        if (child.entries.empty())
                        {
                            continue; // nothing stands in the folder: it is not carried
                        }
                        std::string childOid;
                        if (!planTree(work, child, childOid, work.error))
                        {
                            return false;
                        }
                        TreeEntry out;
                        out.mode = "40000";
                        out.name = name;
                        out.id = ObjectId(childOid);
                        built.entries.push_back(out);
                        continue;
                    }
                    // Three different blobs, each side's own bytes: the lines are merged.
                    mergeBlobThree(work, path, b, o, t, built);
                    if (!work.error.empty() || work.stopWalk)
                    {
                        return !work.error.empty() ? false : true;
                    }
                }

                std::sort(built.entries.begin(), built.entries.end(),
                          [](const TreeEntry &a, const TreeEntry &b) {
                              return treeEntryCompare(a, b) < 0;
                          });
                return true;
            }

            // The wall's current UTC second, which is where every commit this server makes
            // says it was made.
            std::int64_t nowUtcSeconds()
            {
                return static_cast<std::int64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
            }

            // The commit over the merged tree. Its parents are the target and then the head
            // for a merge commit — the order the pull stands in — and the target alone for a
            // squash. A subject the caller hands stands first, and the words this unit
            // states follow it as the second paragraph; with no subject these words are
            // the whole message.
            bool planMergeCommit(MergeWork &work, const std::string &treeOid, MergeStyle style,
                                 const std::string &targetOid, const std::string &headOid,
                                 const std::string &subject, std::string &commitOid)
            {
                CommitObject co;
                if (!ObjectId::parse(treeOid, co.tree))
                {
                    work.error = "the merged tree did not come back with an object id";
                    return false;
                }
                co.parents.push_back(ObjectId(targetOid));
                if (style == MergeStyle::MergeCommit)
                {
                    co.parents.push_back(ObjectId(headOid));
                }
                Person who;
                who.name = "studio";
                who.email = "";
                who.seconds = nowUtcSeconds();
                who.zone = "+0000";
                co.author = who;
                co.committer = who;
                const std::string ownWords = (style == MergeStyle::Squash)
                                                 ? "Squash " + headOid + " into " + targetOid
                                                 : "Merge " + headOid;
                co.message = subject.empty() ? ownWords + "\n" : subject + "\n\n" + ownWords + "\n";

                std::string content;
                if (!commitSerialize(co, content, work.error))
                {
                    work.error = "the merge commit is not in a form git writes: " + work.error;
                    return false;
                }
                commitOid = planOid("commit", content);
                if (commitOid.empty())
                {
                    work.error = "the merge commit did not come back with an object id";
                    return false;
                }
                MergeWork::Planned plan;
                plan.kind = "commit";
                plan.content = content;
                plan.oid = commitOid;
                work.pending.push_back(plan);
                return true;
            }

        } // anonymous namespace

        // ----------------------------------------------------------------------- merge

        // The whole merge through the work: the settled map the work carries
        // answers the note sites; nothing else about the merge changes.
        static bool mergePullWork(const RepoAccess &repo, MergeWork &work,
                                  const std::string &baseRefOid, const std::string &headOid,
                                  MergeStyle style, MergeOutcome &out, std::string &error,
                                  const std::string &subject)
        {
            out = MergeOutcome();
            error.clear();

            ObjectId targetId, headId;
            if (!ObjectId::parse(baseRefOid, targetId))
            {
                error = "the target of the merge is not an object id: " + baseRefOid;
                return false;
            }
            if (!ObjectId::parse(headOid, headId))
            {
                error = "the head of the merge is not an object id: " + headOid;
                return false;
            }
            if (targetId.format != headId.format)
            {
                error = "the target and the head of the merge are ids of two different widths";
                return false;
            }
            const std::string targetOid = targetId.to_hex();
            const std::string headHex = headId.to_hex();

            // The port is held as a const reference, the shape of the brief, while writing
            // an object is what the port does: the constness is cast back at the one place
            // that stands. Nothing here moves a ref.
            RepoAccess &port = *const_cast<RepoAccess *>(&repo);

            // A fast-forward is the answer before any merge: the target standing under the
            // head, the head itself as the commit the caller moves to, and no object made.
            if (style == MergeStyle::FastForward)
            {
                std::set<std::string> underHead;
                if (!walkAncestors(repo, headHex, underHead))
                {
                    error = "the ancestry cannot be proved within the walk of " +
                            std::to_string(kMaxMergeBaseWalk) + " commits";
                    return false;
                }
                if (underHead.count(targetOid) == 0u)
                {
                    error = "this pull is not a fast-forward: the base does not stand under the target";
                    return false;
                }
                out.clean = true;
                out.baseCommitOid = targetOid;
                out.mergedCommitOid = headHex;
                return true;
            }

            std::set<std::string> fromHead, fromTarget;
            if (!walkAncestors(repo, headHex, fromHead) || !walkAncestors(repo, targetOid, fromTarget))
            {
                error = "the ancestry cannot be proved within the walk of " +
                        std::to_string(kMaxMergeBaseWalk) + " commits";
                return false;
            }

            bool multipleBases = false;
            std::string baseOid;
            if (!settleMergeBase(repo, fromHead, fromTarget, baseOid, multipleBases, error))
            {
                return false;
            }
            out.baseCommitOid = baseOid;
            out.multipleBases = multipleBases;

            // The head already stands in the target's history: the pull brings nothing, and
            // the plain answer is that there is nothing to write and nothing to move.
            if (fromTarget.count(headHex) != 0u)
            {
                CommitObject targetCommit;
                if (!readCommitOf(repo, targetOid, targetCommit, error))
                {
                    return false;
                }
                out.clean = true;
                out.mergedTreeOid = targetCommit.tree.to_hex();
                return true;
            }

            CommitObject baseCommit, targetCommit, headCommit;
            if (!readCommitOf(repo, baseOid, baseCommit, error) ||
                !readCommitOf(repo, targetOid, targetCommit, error) ||
                !readCommitOf(repo, headHex, headCommit, error))
            {
                return false;
            }

            TreeObject merged;
            if (!mergeTrees(work, baseCommit.tree.to_hex(), targetCommit.tree.to_hex(),
                            headCommit.tree.to_hex(), std::string(), merged))
            {
                error = work.error;
                return false;
            }
            out.truncated = work.truncated;
            if (!work.conflicts.empty())
            {
                out.clean = false;
                out.conflicts = work.conflicts;
                return true; // a merge that conflicted is an answer, and it writes nothing
            }

            // The tree the merge made, or the one it made is the same as: an unchanged tree
            // is carried by its own id and no object is made for it.
            out.mergedTreeOid = targetCommit.tree.to_hex();
            bool carried = false;
            {
                TreeObject targetTree, headTree, baseTree;
                if (!readTreeOf(repo, targetCommit.tree.to_hex(), targetTree, error) ||
                    !readTreeOf(repo, headCommit.tree.to_hex(), headTree, error) ||
                    !readTreeOf(repo, baseCommit.tree.to_hex(), baseTree, error))
                {
                    return false;
                }
                if (treeSame(merged, targetTree))
                {
                    carried = true;
                }
                else if (treeSame(merged, headTree))
                {
                    out.mergedTreeOid = headCommit.tree.to_hex();
                    carried = true;
                }
                else if (treeSame(merged, baseTree))
                {
                    out.mergedTreeOid = baseCommit.tree.to_hex();
                    carried = true;
                }
            }
            if (!carried && !planTree(work, merged, out.mergedTreeOid, error))
            {
                return false;
            }

            std::string commitOid;
            if (!planMergeCommit(work, out.mergedTreeOid, style, targetOid, headHex, subject,
                                  commitOid))
            {
                error = work.error;
                return false;
            }

            // Everything the merge planned, in the order it was made: the blobs and folders
            // below, the commit over them. The port's answer is the id of the bytes it was
            // handed, and an answer that disagrees is a port this merge cannot trust.
            for (const MergeWork::Planned &plan : work.pending)
            {
                std::string written;
                if (!port.writeLoose(plan.kind, plan.content, written))
                {
                    error = "the merged " + plan.kind + " could not be written";
                    return false;
                }
                if (written != plan.oid)
                {
                    error = "the merged " + plan.kind +
                            " came back under an id that is not the id of its bytes";
                    return false;
                }
            }

            out.clean = true;
            out.mergedCommitOid = commitOid;
            return true;
        }

        bool mergePull(const RepoAccess &repo, const std::string &baseRefOid,
                       const std::string &headOid, MergeStyle style, MergeOutcome &out,
                       std::string &error, const std::string &subject)
        {
            MergeWork work(*const_cast<RepoAccess *>(&repo));
            return mergePullWork(repo, work, baseRefOid, headOid, style, out, error, subject);
        }

        // The resolve round: the same merge, with the settled paths carried in.
        // The answer names which resolutions the merge used and which it did not;
        // a resolution whose path never conflicted is unused, and the merge still
        // answers clean when nothing else conflicted.
        bool mergePull(const RepoAccess &repo, const std::string &baseRefOid,
                      const std::string &headOid, MergeStyle style,
                      const std::vector<MergeResolution> &resolutions, MergeOutcome &out,
                      std::string &error, const std::string &subject)
        {
            MergeWork work(*const_cast<RepoAccess *>(&repo));
            for (const MergeResolution &r : resolutions)
            {
                work.settled[r.path] = &r;
            }
            if (!mergePullWork(repo, work, baseRefOid, headOid, style, out, error, subject))
            {
                return false;
            }
            for (std::vector<MergeResolution>::size_type at = 0u; at < resolutions.size(); ++at)
            {
                if (work.took.count(&resolutions[at]) != 0u)
                {
                    out.took.push_back(resolutions[at].path);
                }
                else
                {
                    out.unused.push_back(resolutions[at].path);
                }
            }
            return true;
        }

    } // namespace gitcore
} // namespace ols
