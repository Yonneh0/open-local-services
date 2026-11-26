// tests/gitcore/test_merge.cpp — the cases that prove merge.h's answer.
//
// No fixtures and no repository: the port merge.h leans on — receive.h's
// RepoAccess — is stood in here with the objects these cases name, the way
// test_receive.cpp stands its holding port. Every commit, tree, and blob is made
// in memory and hashed with the object layer's own sha1Hex, so the ids the cases
// check are the ids the bytes state. What the merge writes lands in the same map,
// counted apart from what was seeded, so a case can say both what the answer named
// and how many objects the merge actually put there.
//
// The claims are the ones merge.h makes: the tree of a clean merge down to its
// bytes, the oid the answer carries when nothing new was made, the three sides of
// a conflict as the trees held them, the kind a path fell under, the words of the
// two refusals, and that a fast-forward writes nothing at all.

#include "gitcore/merge.h"
#include "gitcore/objects.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

// The id of an object as its bytes state it. Stood before the port because the
// port's own writeLoose asks it.
std::string mergeOidOf(ols::gitcore::ObjectKind kind, const std::string &content)
{
    return ols::gitcore::sha1Hex(
        ols::gitcore::objectHeaderMake(kind, content.size()) + content);
}

// The port with objects behind its ids: what the cases seed, and what the merge
// writes. The seeded objects are held without a mark; what arrives through
// writeLoose is counted, so a case names the objects the merge made rather than
// trusting that it made them.
class MergeStubPort : public ols::gitcore::RepoAccess
{
public:
    void hold(const std::string &oid, const std::string &kind, const std::string &content)
    {
        held_[oid] = std::make_pair(kind, content);
    }

    bool hasObject(const std::string &oidHex) const override
    {
        return held_.count(oidHex) != 0u;
    }

    bool readObject(const std::string &oidHex, std::string &kindOut,
                    std::string &contentOut) const override
    {
        const std::map<std::string, std::pair<std::string, std::string>>::const_iterator at =
            held_.find(oidHex);
        if (at == held_.end())
        {
            kindOut.clear();
            contentOut.clear();
            return false;
        }
        kindOut = at->second.first;
        contentOut = at->second.second;
        return true;
    }

    // written the way a repository holds it: the id of the bytes comes back, and the
    // object is kept so a case can read the commit it merged and see its parents.
    bool writeLoose(const std::string &kind, const std::string &content,
                    std::string &oidHex) override
    {
        ols::gitcore::ObjectKind objectKind = ols::gitcore::ObjectKind::Blob;
        std::string why;
        if (!ols::gitcore::kindOfName(kind, objectKind, why))
        {
            oidHex.clear();
            return false;
        }
        oidHex = mergeOidOf(objectKind, content);
        held_[oidHex] = std::make_pair(kind, content);
        written_.push_back(oidHex);
        return true;
    }

    // The merge moves no ref, so these answer plainly and a case can see that they
    // were never asked: a pull's answer is an oid, not a move.
    bool refValue(const std::string &name, std::string &oidHex) const override
    {
        (void)name;
        oidHex.clear();
        return false;
    }

    bool commitRefs(const std::vector<ols::gitcore::RefUpdate> &updates) override
    {
        (void)updates;
        return false;
    }

    bool deleteRefs(const std::vector<std::string> &names) override
    {
        (void)names;
        return false;
    }

    const std::vector<std::string> &written() const { return written_; }

private:
    std::map<std::string, std::pair<std::string, std::string>> held_;
    std::vector<std::string> written_;
};

// One entry of a fixture tree, as the case names it.
struct MergeEntry
{
    std::string mode;
    std::string name;
    std::string oid;
};

// A blob put where the port can read it.
std::string mergeSeedBlob(MergeStubPort &port, const std::string &text)
{
    const std::string oid = mergeOidOf(ols::gitcore::ObjectKind::Blob, text);
    port.hold(oid, "blob", text);
    return oid;
}

// A tree put where the port can read it: the entries are put in the order git
// keeps and serialized to the bytes the object layer writes, so the id is the id
// of a real tree and the merge reads back exactly what the case meant.
std::string mergeSeedTree(MergeStubPort &port, const std::vector<MergeEntry> &entries)
{
    std::vector<ols::gitcore::TreeEntry> te;
    te.reserve(entries.size());
    for (const MergeEntry &e : entries)
    {
        ols::gitcore::TreeEntry one;
        one.mode = e.mode;
        one.name = e.name;
        one.id = ols::gitcore::ObjectId(e.oid);
        te.push_back(one);
    }
    std::sort(te.begin(), te.end(),
              [](const ols::gitcore::TreeEntry &a, const ols::gitcore::TreeEntry &b) {
                  return ols::gitcore::treeEntryCompare(a, b) < 0;
              });
    ols::gitcore::TreeObject tree;
    tree.entries = te;
    std::string content, why;
    if (!ols::gitcore::treeSerialize(tree, content, why))
    {
        return std::string();
    }
    const std::string oid = mergeOidOf(ols::gitcore::ObjectKind::Tree, content);
    port.hold(oid, "tree", content);
    return oid;
}

// A commit put where the port can read it. The fixture ident is the fixture's
// own; the commit the merge makes wears the one merge.h states.
std::string mergeSeedCommit(MergeStubPort &port, const std::string &treeOid,
                            const std::vector<std::string> &parents, const std::string &message)
{
    ols::gitcore::CommitObject co;
    co.tree = ols::gitcore::ObjectId(treeOid);
    for (const std::string &parent : parents)
    {
        co.parents.push_back(ols::gitcore::ObjectId(parent));
    }
    ols::gitcore::Person who;
    who.name = "fixture";
    who.email = "";
    who.seconds = 1700000000;
    who.zone = "+0000";
    co.author = who;
    co.committer = who;
    co.message = message + "\n";
    std::string content, why;
    if (!ols::gitcore::commitSerialize(co, content, why))
    {
        return std::string();
    }
    const std::string oid = mergeOidOf(ols::gitcore::ObjectKind::Commit, content);
    port.hold(oid, "commit", content);
    return oid;
}

// The three sides of a file, as the cases keep them.
const char kBaseText[] = "alpha\nbeta\ngamma\n";
const char kBothText[] = "the same on both sides\n";

// The fixture both the clean-merge cases and the commit-shape cases stand: one
// base, a target that changed a.txt and added ours.txt, a head that changed
// b.txt. Neither side touched c.txt.
struct CleanFixture
{
    std::string base;
    std::string target;
    std::string head;
    std::string baseTree;
    std::string targetTree;
    std::string headTree;
    std::string aBase, aOurs, bBase, bTheirs, cStay, oursOnly;
};

CleanFixture seedCleanFixture(MergeStubPort &port)
{
    CleanFixture f;
    f.aBase = mergeSeedBlob(port, kBaseText);
    f.aOurs = mergeSeedBlob(port, "ALPHA\nbeta\ngamma\n");
    f.bBase = mergeSeedBlob(port, "one\ntwo\n");
    f.bTheirs = mergeSeedBlob(port, "one\ntwo\nthree\n");
    f.cStay = mergeSeedBlob(port, "stay\n");
    f.oursOnly = mergeSeedBlob(port, "mine\n");

    f.baseTree = mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), f.aBase},
                                      {std::string("100644"), std::string("b.txt"), f.bBase},
                                      {std::string("100644"), std::string("c.txt"), f.cStay}});
    f.targetTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), f.aOurs},
                             {std::string("100644"), std::string("b.txt"), f.bBase},
                             {std::string("100644"), std::string("c.txt"), f.cStay},
                             {std::string("100644"), std::string("ours.txt"), f.oursOnly}});
    f.headTree = mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), f.aBase},
                                      {std::string("100644"), std::string("b.txt"), f.bTheirs},
                                      {std::string("100644"), std::string("c.txt"), f.cStay}});

    f.base = mergeSeedCommit(port, f.baseTree, std::vector<std::string>(), "base\n");
    f.target = mergeSeedCommit(port, f.targetTree, {f.base}, "target\n");
    f.head = mergeSeedCommit(port, f.headTree, {f.base}, "head\n");
    return f;
}

// The conflict of one path, found by kind: the cases ask for their path rather
// than walking the answer by hand.
const ols::gitcore::MergeConflict *mergeConflictAt(const ols::gitcore::MergeOutcome &out,
                                                   const std::string &path)
{
    for (const ols::gitcore::MergeConflict &c : out.conflicts)
    {
        if (c.path == path)
        {
            return &c;
        }
    }
    return nullptr;
}

// ------------------------------------------------------------------ the clean merge

TEST(merge_clean_takes_both_sides)
{
    MergeStubPort port;
    const CleanFixture f = seedCleanFixture(port);

    ols::gitcore::MergeOutcome out;
    std::string error;
    const bool stood = ols::gitcore::mergePull(port, f.target, f.head,
                                               ols::gitcore::MergeStyle::MergeCommit, out, error);
    CHECK(stood);
    CHECK(error.empty());
    CHECK(out.clean);
    CHECK_EQ(out.conflicts.size(), std::size_t(0));
    CHECK(out.multipleBases == false);
    CHECK(out.truncated == false);
    CHECK_EQ(out.baseCommitOid, f.base);

    // The tree the merge makes: the target's a.txt and ours.txt, the head's b.txt,
    // and the untouched c.txt. Built here from the ids the fixture hashed, so the
    // claim is byte for byte the tree merge.h describes.
    const std::string want =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), f.aOurs},
                             {std::string("100644"), std::string("b.txt"), f.bTheirs},
                             {std::string("100644"), std::string("c.txt"), f.cStay},
                             {std::string("100644"), std::string("ours.txt"), f.oursOnly}});
    CHECK_EQ(out.mergedTreeOid, want);

    // Two objects and no more: the tree, and the commit over it.
    CHECK_EQ(port.written().size(), std::size_t(2));
    CHECK_EQ(port.written()[0], want);
    CHECK_EQ(port.written()[1], out.mergedCommitOid);
}

TEST(merge_commit_carries_both_parents_in_order)
{
    MergeStubPort port;
    const CleanFixture f = seedCleanFixture(port);

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, f.target, f.head, ols::gitcore::MergeStyle::MergeCommit, out,
                                  error));
    CHECK(out.clean);

    std::string kind, content;
    CHECK(port.readObject(out.mergedCommitOid, kind, content));
    CHECK_EQ(kind, std::string("commit"));
    ols::gitcore::CommitObject co;
    std::string why;
    CHECK(ols::gitcore::commitParse(content, co, why));

    // The order of the parents is the order of the pull: the target the head is
    // coming into, then the head.
    CHECK_EQ(co.parents.size(), std::size_t(2));
    CHECK_EQ(co.parents[0].to_hex(), f.target);
    CHECK_EQ(co.parents[1].to_hex(), f.head);
    CHECK_EQ(co.tree.to_hex(), out.mergedTreeOid);
    CHECK_EQ(co.message, std::string("Merge ") + f.head + "\n");
    CHECK_EQ(co.author.name, std::string("studio"));
    CHECK_EQ(co.committer.name, std::string("studio"));
    CHECK_EQ(co.committer.zone, std::string("+0000"));
    CHECK(co.committer.seconds > 1700000000);
    CHECK(co.author.seconds == co.committer.seconds);
}

TEST(merge_squash_commit_has_one_parent)
{
    MergeStubPort port;
    const CleanFixture f = seedCleanFixture(port);

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, f.target, f.head, ols::gitcore::MergeStyle::Squash, out,
                                  error));
    CHECK(out.clean);

    std::string kind, content;
    CHECK(port.readObject(out.mergedCommitOid, kind, content));
    ols::gitcore::CommitObject co;
    std::string why;
    CHECK(ols::gitcore::commitParse(content, co, why));
    CHECK_EQ(co.parents.size(), std::size_t(1));
    CHECK_EQ(co.parents[0].to_hex(), f.target);
    CHECK_EQ(co.message, std::string("Squash ") + f.head + " into " + f.target + "\n");

    // The same tree the merge commit stands over: the style changes the commit, not
    // the merge.
    CHECK(!out.mergedTreeOid.empty());
    CHECK_EQ(out.baseCommitOid, f.base);
    CHECK_EQ(port.written().size(), std::size_t(2));
}

TEST(merge_commit_carries_the_handed_subject_first)
{
    MergeStubPort port;
    const CleanFixture f = seedCleanFixture(port);

    ols::gitcore::MergeOutcome out;
    std::string error;
    const std::string subject = "Merge pull request #7 from topic: state the ledger aright";
    CHECK(ols::gitcore::mergePull(port, f.target, f.head, ols::gitcore::MergeStyle::MergeCommit, out,
                                  error, subject));
    CHECK(out.clean);

    std::string kind, content;
    CHECK(port.readObject(out.mergedCommitOid, kind, content));
    ols::gitcore::CommitObject co;
    std::string why;
    CHECK(ols::gitcore::commitParse(content, co, why));

    // The caller's line stands first and this unit's words follow as its second
    // paragraph: the subject rides above the engine's words, never instead of them.
    CHECK_EQ(co.message, subject + "\n\nMerge " + f.head + "\n");
    CHECK_EQ(co.parents.size(), std::size_t(2));
}

TEST(merge_squash_commit_carries_the_handed_subject_too)
{
    MergeStubPort port;
    const CleanFixture f = seedCleanFixture(port);

    ols::gitcore::MergeOutcome out;
    std::string error;
    const std::string subject = "Merge pull request #8 from topic: the squashed pull's line";
    CHECK(ols::gitcore::mergePull(port, f.target, f.head, ols::gitcore::MergeStyle::Squash, out,
                                  error, subject));
    CHECK(out.clean);

    std::string kind, content;
    CHECK(port.readObject(out.mergedCommitOid, kind, content));
    ols::gitcore::CommitObject co;
    std::string why;
    CHECK(ols::gitcore::commitParse(content, co, why));
    CHECK_EQ(co.parents.size(), std::size_t(1));
    CHECK_EQ(co.message, subject + "\n\nSquash " + f.head + " into " + f.target + "\n");
}

TEST(merge_with_resolutions_carries_the_handed_subject_also)
{
    MergeStubPort port;
    const CleanFixture f = seedCleanFixture(port);

    // The clean fixture holds no conflict, so the settled path goes unused and the
    // merge answers clean: the answer a resolve round's retry brings, and the
    // subject still rides the commit it makes.
    ols::gitcore::MergeResolution settled;
    settled.path = "nowhere.txt";
    settled.content = "never asked for\n";
    std::vector<ols::gitcore::MergeResolution> resolutions;
    resolutions.push_back(settled);

    ols::gitcore::MergeOutcome out;
    std::string error;
    const std::string subject = "Merge pull request #9 from topic: the retried pull's line";
    CHECK(ols::gitcore::mergePull(port, f.target, f.head, ols::gitcore::MergeStyle::MergeCommit,
                                  resolutions, out, error, subject));
    CHECK(out.clean);
    CHECK_EQ(out.unused.size(), std::size_t(1));

    std::string kind, content;
    CHECK(port.readObject(out.mergedCommitOid, kind, content));
    ols::gitcore::CommitObject co;
    std::string why;
    CHECK(ols::gitcore::commitParse(content, co, why));
    CHECK_EQ(co.message, subject + "\n\nMerge " + f.head + "\n");
}

TEST(merge_disjoint_lines_of_one_file_merge)
{
    MergeStubPort port;
    const std::string base = mergeSeedBlob(port, "l1\nl2\nl3\nl4\n");
    const std::string ours = mergeSeedBlob(port, "L1\nl2\nl3\nl4\n");
    const std::string theirs = mergeSeedBlob(port, "l1\nl2\nl3\nL4\n");
    const std::string baseTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("f.txt"), base}});
    const std::string targetTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("f.txt"), ours}});
    const std::string headTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("f.txt"), theirs}});
    const std::string cBase = mergeSeedCommit(port, baseTree, std::vector<std::string>(), "b\n");
    const std::string cTarget = mergeSeedCommit(port, targetTree, {cBase}, "t\n");
    const std::string cHead = mergeSeedCommit(port, headTree, {cBase}, "h\n");

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, cTarget, cHead, ols::gitcore::MergeStyle::MergeCommit, out,
                                  error));
    CHECK(out.clean);
    CHECK_EQ(out.conflicts.size(), std::size_t(0));

    // The two changes stand together: each side's line, and the untouched lines.
    const std::string mergedBlob = mergeSeedBlob(port, "L1\nl2\nl3\nL4\n");
    const std::string want =
        mergeSeedTree(port, {{std::string("100644"), std::string("f.txt"), mergedBlob}});
    CHECK_EQ(out.mergedTreeOid, want);
}

// ------------------------------------------------------------------ the carried trees

TEST(merge_both_sides_make_the_same_change)
{
    MergeStubPort port;
    const std::string baseBlob = mergeSeedBlob(port, kBaseText);
    const std::string sameBlob = mergeSeedBlob(port, kBothText);
    const std::string baseTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), baseBlob}});
    const std::string sameTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), sameBlob}});
    const std::string cBase = mergeSeedCommit(port, baseTree, std::vector<std::string>(), "b\n");
    const std::string cTarget = mergeSeedCommit(port, sameTree, {cBase}, "t\n");
    const std::string cHead = mergeSeedCommit(port, sameTree, {cBase}, "h\n");
    CHECK(cTarget != cHead); // the commits differ by their messages; the trees do not

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, cTarget, cHead, ols::gitcore::MergeStyle::MergeCommit, out,
                                  error));
    CHECK(out.clean);
    CHECK_EQ(out.conflicts.size(), std::size_t(0));
    // The tree both sides made is carried under its own id: no tree is written twice.
    CHECK_EQ(out.mergedTreeOid, sameTree);
    CHECK_EQ(port.written().size(), std::size_t(1));
    CHECK_EQ(port.written()[0], out.mergedCommitOid);
}

TEST(merge_one_side_only_changes)
{
    MergeStubPort port;
    const CleanFixture f = seedCleanFixture(port);

    // The target has not moved since the base: everything the head changed is the
    // change, and the merged tree is the head's own.
    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, f.base, f.head, ols::gitcore::MergeStyle::MergeCommit, out,
                                  error));
    CHECK(out.clean);
    CHECK_EQ(out.baseCommitOid, f.base);
    CHECK_EQ(out.mergedTreeOid, f.headTree);
    CHECK_EQ(port.written().size(), std::size_t(1)); // the commit only
}

TEST(merge_clean_under_a_directory)
{
    MergeStubPort port;
    const std::string gBase = mergeSeedBlob(port, "g one\n");
    const std::string gTheirs = mergeSeedBlob(port, "g two\n");
    const std::string extra = mergeSeedBlob(port, "extra\n");
    const std::string keep = mergeSeedBlob(port, "keep\n");

    const std::string subBase =
        mergeSeedTree(port, {{std::string("100644"), std::string("g.txt"), gBase}});
    const std::string subOurs =
        mergeSeedTree(port, {{std::string("100644"), std::string("e.txt"), extra},
                             {std::string("100644"), std::string("g.txt"), gBase}});
    const std::string subTheirs =
        mergeSeedTree(port, {{std::string("100644"), std::string("g.txt"), gTheirs}});
    const std::string rootBase =
        mergeSeedTree(port, {{std::string("40000"), std::string("sub"), subBase},
                             {std::string("100644"), std::string("keep.txt"), keep}});
    const std::string rootOurs =
        mergeSeedTree(port, {{std::string("40000"), std::string("sub"), subOurs},
                             {std::string("100644"), std::string("keep.txt"), keep}});
    const std::string rootTheirs =
        mergeSeedTree(port, {{std::string("40000"), std::string("sub"), subTheirs},
                             {std::string("100644"), std::string("keep.txt"), keep}});
    const std::string cBase = mergeSeedCommit(port, rootBase, std::vector<std::string>(), "b\n");
    const std::string cTarget = mergeSeedCommit(port, rootOurs, {cBase}, "t\n");
    const std::string cHead = mergeSeedCommit(port, rootTheirs, {cBase}, "h\n");

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, cTarget, cHead, ols::gitcore::MergeStyle::MergeCommit, out,
                                  error));
    CHECK(out.clean);

    // The folder both sides changed: the head's g.txt and the target's e.txt, with
    // the untouched file beside the folder standing on.
    const std::string wantSub =
        mergeSeedTree(port, {{std::string("100644"), std::string("e.txt"), extra},
                             {std::string("100644"), std::string("g.txt"), gTheirs}});
    const std::string wantRoot =
        mergeSeedTree(port, {{std::string("40000"), std::string("sub"), wantSub},
                             {std::string("100644"), std::string("keep.txt"), keep}});
    CHECK_EQ(out.mergedTreeOid, wantRoot);
    CHECK_EQ(port.written().size(), std::size_t(3)); // the folder, the root, the commit
}

// --------------------------------------------------------------------- the conflicts

TEST(merge_overlapping_edit_conflicts)
{
    MergeStubPort port;
    const std::string baseBlob = mergeSeedBlob(port, kBaseText);
    const std::string oursBlob = mergeSeedBlob(port, "alpha\nBETA OURS\ngamma\n");
    const std::string theirsBlob = mergeSeedBlob(port, "alpha\nBETA THEIRS\ngamma\n");
    const std::string baseTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), baseBlob}});
    const std::string targetTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), oursBlob}});
    const std::string headTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), theirsBlob}});
    const std::string cBase = mergeSeedCommit(port, baseTree, std::vector<std::string>(), "b\n");
    const std::string cTarget = mergeSeedCommit(port, targetTree, {cBase}, "t\n");
    const std::string cHead = mergeSeedCommit(port, headTree, {cBase}, "h\n");

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, cTarget, cHead, ols::gitcore::MergeStyle::MergeCommit, out,
                                  error));
    // A conflicted merge is an answer, not a refusal.
    CHECK(out.clean == false);
    CHECK(error.empty());
    CHECK_EQ(out.baseCommitOid, cBase);
    CHECK_EQ(out.conflicts.size(), std::size_t(1));
    const ols::gitcore::MergeConflict *c = mergeConflictAt(out, "a.txt");
    CHECK(c != nullptr);
    if (c != nullptr)
    {
        CHECK_EQ(c->kind, std::string("both-changed"));
        CHECK_EQ(c->baseOid, baseBlob);
        CHECK_EQ(c->oursOid, oursBlob);
        CHECK_EQ(c->theirsOid, theirsBlob);
        CHECK_EQ(c->baseText, std::string(kBaseText));
        CHECK_EQ(c->oursText, std::string("alpha\nBETA OURS\ngamma\n"));
        CHECK_EQ(c->theirsText, std::string("alpha\nBETA THEIRS\ngamma\n"));
    }
    // Nothing was made: no tree, no commit, no object at all.
    CHECK(out.mergedTreeOid.empty());
    CHECK(out.mergedCommitOid.empty());
    CHECK_EQ(port.written().size(), std::size_t(0));
}

TEST(merge_add_add_conflicts)
{
    MergeStubPort port;
    const std::string keep = mergeSeedBlob(port, "keep\n");
    const std::string ours = mergeSeedBlob(port, "ours\n");
    const std::string theirs = mergeSeedBlob(port, "theirs\n");
    const std::string baseTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("keep.txt"), keep}});
    const std::string targetTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("keep.txt"), keep},
                             {std::string("100644"), std::string("new.txt"), ours}});
    const std::string headTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("keep.txt"), keep},
                             {std::string("100644"), std::string("new.txt"), theirs}});
    const std::string cBase = mergeSeedCommit(port, baseTree, std::vector<std::string>(), "b\n");
    const std::string cTarget = mergeSeedCommit(port, targetTree, {cBase}, "t\n");
    const std::string cHead = mergeSeedCommit(port, headTree, {cBase}, "h\n");

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, cTarget, cHead, ols::gitcore::MergeStyle::MergeCommit, out,
                                  error));
    CHECK(out.clean == false);
    const ols::gitcore::MergeConflict *c = mergeConflictAt(out, "new.txt");
    CHECK(c != nullptr);
    if (c != nullptr)
    {
        CHECK_EQ(c->kind, std::string("add-add"));
        CHECK(c->baseOid.empty());
        CHECK_EQ(c->oursOid, ours);
        CHECK_EQ(c->theirsOid, theirs);
        CHECK_EQ(c->oursText, std::string("ours\n"));
        CHECK_EQ(c->theirsText, std::string("theirs\n"));
    }
    CHECK_EQ(port.written().size(), std::size_t(0));
}

TEST(merge_edit_vs_delete_conflicts)
{
    MergeStubPort port;
    const std::string keep = mergeSeedBlob(port, "keep\n");
    const std::string baseBlob = mergeSeedBlob(port, kBaseText);
    const std::string edited = mergeSeedBlob(port, "alpha\nbeta\nGAMMA\n");
    const std::string baseTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), baseBlob},
                             {std::string("100644"), std::string("keep.txt"), keep}});
    const std::string targetTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("keep.txt"), keep}});
    const std::string headTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), edited},
                             {std::string("100644"), std::string("keep.txt"), keep}});
    const std::string cBase = mergeSeedCommit(port, baseTree, std::vector<std::string>(), "b\n");
    const std::string cTarget = mergeSeedCommit(port, targetTree, {cBase}, "t\n");
    const std::string cHead = mergeSeedCommit(port, headTree, {cBase}, "h\n");

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, cTarget, cHead, ols::gitcore::MergeStyle::MergeCommit, out,
                                  error));
    CHECK(out.clean == false);
    const ols::gitcore::MergeConflict *c = mergeConflictAt(out, "a.txt");
    CHECK(c != nullptr);
    if (c != nullptr)
    {
        CHECK_EQ(c->kind, std::string("edit-vs-delete"));
        CHECK_EQ(c->baseOid, baseBlob);
        CHECK(c->oursOid.empty());
        CHECK_EQ(c->theirsOid, edited);
        CHECK_EQ(c->baseText, std::string(kBaseText));
        CHECK(c->oursText.empty());
        CHECK_EQ(c->theirsText, std::string("alpha\nbeta\nGAMMA\n"));
    }
    CHECK_EQ(port.written().size(), std::size_t(0));
}

TEST(merge_file_vs_directory_conflicts)
{
    MergeStubPort port;
    const std::string keep = mergeSeedBlob(port, "keep\n");
    const std::string inner = mergeSeedBlob(port, "inner\n");
    const std::string fileD = mergeSeedBlob(port, "d as a file\n");
    const std::string dirD =
        mergeSeedTree(port, {{std::string("100644"), std::string("inner.txt"), inner}});
    const std::string baseTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("keep.txt"), keep}});
    const std::string targetTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("keep.txt"), keep},
                             {std::string("40000"), std::string("d"), dirD}});
    const std::string headTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("keep.txt"), keep},
                             {std::string("100644"), std::string("d"), fileD}});
    const std::string cBase = mergeSeedCommit(port, baseTree, std::vector<std::string>(), "b\n");
    const std::string cTarget = mergeSeedCommit(port, targetTree, {cBase}, "t\n");
    const std::string cHead = mergeSeedCommit(port, headTree, {cBase}, "h\n");

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, cTarget, cHead, ols::gitcore::MergeStyle::MergeCommit, out,
                                  error));
    CHECK(out.clean == false);
    const ols::gitcore::MergeConflict *c = mergeConflictAt(out, "d");
    CHECK(c != nullptr);
    if (c != nullptr)
    {
        // A folder one place and a thing another: the name is the path, the oids are
        // the three sides, and no text is carried for a name that is not a file.
        CHECK_EQ(c->kind, std::string("file-vs-directory"));
        CHECK(c->baseOid.empty());
        CHECK_EQ(c->oursOid, dirD);
        CHECK_EQ(c->theirsOid, fileD);
        CHECK(c->baseText.empty());
        CHECK(c->oursText.empty());
        CHECK(c->theirsText.empty());
    }
    CHECK_EQ(port.written().size(), std::size_t(0));
}

TEST(merge_side_over_the_cap_is_a_budget_conflict_without_texts)
{
    MergeStubPort port;
    const std::string baseBlob = mergeSeedBlob(port, "small\n");
    const std::string bigText(2u * 1024u * 1024u + 8u, 'x');
    const std::string bigBlob = mergeSeedBlob(port, bigText);
    const std::string otherBlob = mergeSeedBlob(port, "other small\n");
    const std::string baseTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("big.txt"), baseBlob}});
    const std::string targetTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("big.txt"), bigBlob}});
    const std::string headTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("big.txt"), otherBlob}});
    const std::string cBase = mergeSeedCommit(port, baseTree, std::vector<std::string>(), "b\n");
    const std::string cTarget = mergeSeedCommit(port, targetTree, {cBase}, "t\n");
    const std::string cHead = mergeSeedCommit(port, headTree, {cBase}, "h\n");

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, cTarget, cHead, ols::gitcore::MergeStyle::MergeCommit, out,
                                  error));
    CHECK(out.clean == false);
    const ols::gitcore::MergeConflict *c = mergeConflictAt(out, "big.txt");
    CHECK(c != nullptr);
    if (c != nullptr)
    {
        // The path is named and the three sides are named; the bytes over the bound of
        // a carried text are not carried.
        CHECK_EQ(c->kind, std::string("budget"));
        CHECK_EQ(c->baseOid, baseBlob);
        CHECK_EQ(c->oursOid, bigBlob);
        CHECK_EQ(c->theirsOid, otherBlob);
        CHECK(c->baseText.empty());
        CHECK(c->oursText.empty());
        CHECK(c->theirsText.empty());
    }
    CHECK_EQ(port.written().size(), std::size_t(0));
}

TEST(merge_conflicts_are_capped_and_truncated)
{
    MergeStubPort port;
    const std::string b = mergeSeedBlob(port, "b\n");
    const std::string o = mergeSeedBlob(port, "o\n");
    const std::string t = mergeSeedBlob(port, "t\n");
    std::vector<MergeEntry> baseTree, targetTree, headTree;
    for (int i = 0; i < 201; ++i)
    {
        const std::string name = "f" + std::to_string(i) + ".txt";
        baseTree.push_back(MergeEntry{std::string("100644"), name, b});
        targetTree.push_back(MergeEntry{std::string("100644"), name, o});
        headTree.push_back(MergeEntry{std::string("100644"), name, t});
    }
    const std::string baseT = mergeSeedTree(port, baseTree);
    const std::string targetT = mergeSeedTree(port, targetTree);
    const std::string headT = mergeSeedTree(port, headTree);
    const std::string cBase = mergeSeedCommit(port, baseT, std::vector<std::string>(), "b\n");
    const std::string cTarget = mergeSeedCommit(port, targetT, {cBase}, "t\n");
    const std::string cHead = mergeSeedCommit(port, headT, {cBase}, "h\n");

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, cTarget, cHead, ols::gitcore::MergeStyle::MergeCommit, out,
                                  error));
    CHECK(out.clean == false);
    // Two hundred paths and the word that there were more.
    CHECK_EQ(out.conflicts.size(), std::size_t(200));
    CHECK(out.truncated);
    CHECK_EQ(port.written().size(), std::size_t(0));
}

// ----------------------------------------------------------------------- the refusals

TEST(merge_no_common_ancestor_refuses)
{
    MergeStubPort port;
    const std::string rootTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"),
                              mergeSeedBlob(port, "one\n")}});
    const std::string otherTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("b.txt"),
                              mergeSeedBlob(port, "two\n")}});
    const std::string cOne = mergeSeedCommit(port, rootTree, std::vector<std::string>(), "one\n");
    const std::string cTwo = mergeSeedCommit(port, otherTree, std::vector<std::string>(), "two\n");

    ols::gitcore::MergeOutcome out;
    std::string error;
    const bool stood = ols::gitcore::mergePull(port, cOne, cTwo,
                                               ols::gitcore::MergeStyle::MergeCommit, out, error);
    CHECK(stood == false);
    CHECK_EQ(error, std::string("these branches have no common ancestor: the merge has no base"));
    CHECK_EQ(port.written().size(), std::size_t(0));
}

TEST(merge_refuses_an_id_that_is_not_one)
{
    MergeStubPort port;
    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, "not-an-oid",
                                  std::string(40u, 'a'), ols::gitcore::MergeStyle::Squash, out,
                                  error) == false);
    CHECK(!error.empty());
    CHECK(ols::gitcore::mergePull(port, std::string(40u, 'a'), "also-not-an-oid",
                                  ols::gitcore::MergeStyle::Squash, out, error) == false);
    CHECK(!error.empty());
    // An id of the other width beside an id of this one: the two are not comparable.
    CHECK(ols::gitcore::mergePull(port, std::string(40u, 'a'), std::string(64u, 'a'),
                                  ols::gitcore::MergeStyle::Squash, out, error) == false);
    CHECK(!error.empty());
    CHECK_EQ(port.written().size(), std::size_t(0));
}

// ----------------------------------------------------------------------- fast-forward

TEST(merge_fast_forward_moves_nothing)
{
    MergeStubPort port;
    const std::string t1 = mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"),
                                                 mergeSeedBlob(port, "one\n")}});
    const std::string t2 = mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"),
                                                 mergeSeedBlob(port, "two\n")}});
    const std::string t3 = mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"),
                                                 mergeSeedBlob(port, "three\n")}});
    const std::string c1 = mergeSeedCommit(port, t1, std::vector<std::string>(), "one\n");
    const std::string c2 = mergeSeedCommit(port, t2, {c1}, "two\n");
    const std::string c3 = mergeSeedCommit(port, t3, {c2}, "three\n");

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, c2, c3, ols::gitcore::MergeStyle::FastForward, out, error));
    CHECK(out.clean);
    // The head is the answer: the caller moves the ref, and no object is made.
    CHECK_EQ(out.mergedCommitOid, c3);
    CHECK_EQ(out.baseCommitOid, c2);
    CHECK(out.mergedTreeOid.empty());
    CHECK_EQ(out.conflicts.size(), std::size_t(0));
    CHECK_EQ(port.written().size(), std::size_t(0));
}

TEST(merge_fast_forward_refused_when_not_under)
{
    MergeStubPort port;
    const std::string one = mergeSeedBlob(port, "one\n");
    const std::string t1 =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), one}});
    const std::string t2 =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), one},
                             {std::string("100644"), std::string("b.txt"),
                              mergeSeedBlob(port, "two\n")}});
    const std::string t3 =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), one},
                             {std::string("100644"), std::string("c.txt"),
                              mergeSeedBlob(port, "sibling\n")}});
    const std::string c1 = mergeSeedCommit(port, t1, std::vector<std::string>(), "one\n");
    const std::string c2 = mergeSeedCommit(port, t2, {c1}, "two\n");
    const std::string c3 = mergeSeedCommit(port, t3, {c1}, "sibling\n");

    ols::gitcore::MergeOutcome out;
    std::string error;
    const bool stood =
        ols::gitcore::mergePull(port, c2, c3, ols::gitcore::MergeStyle::FastForward, out, error);
    CHECK(stood == false);
    CHECK_EQ(error,
             std::string("this pull is not a fast-forward: the base does not stand under the target"));
    CHECK_EQ(port.written().size(), std::size_t(0));

    // The same pair merges under the merge-commit style: the refusal is the style's,
    // not the history's, and each side's new file stands in the answer.
    ols::gitcore::MergeOutcome merged;
    std::string why;
    CHECK(ols::gitcore::mergePull(port, c2, c3, ols::gitcore::MergeStyle::MergeCommit, merged, why));
    CHECK(merged.clean);
    CHECK_EQ(merged.baseCommitOid, c1);
    const std::string wantTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), one},
                             {std::string("100644"), std::string("b.txt"),
                              mergeSeedBlob(port, "two\n")},
                             {std::string("100644"), std::string("c.txt"),
                              mergeSeedBlob(port, "sibling\n")}});
    CHECK_EQ(merged.mergedTreeOid, wantTree);
}

TEST(merge_brought_nothing_writes_no_commit)
{
    MergeStubPort port;
    const std::string t1 = mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"),
                                                 mergeSeedBlob(port, "one\n")}});
    const std::string t2 = mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"),
                                                 mergeSeedBlob(port, "two\n")}});
    const std::string c1 = mergeSeedCommit(port, t1, std::vector<std::string>(), "one\n");
    const std::string c2 = mergeSeedCommit(port, t2, {c1}, "two\n");

    // The head already stands under the target: the pull has nothing to bring, so
    // the answer is clean with the target's own tree and no commit at all.
    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, c2, c1, ols::gitcore::MergeStyle::Squash, out, error));
    CHECK(out.clean);
    CHECK_EQ(out.baseCommitOid, c1);
    CHECK_EQ(out.mergedTreeOid, t2);
    CHECK(out.mergedCommitOid.empty());
    CHECK_EQ(port.written().size(), std::size_t(0));
}

// ------------------------------------------------------------- the resolve round
//
// The same merge with resolutions carried in: a settled path takes the
// resolution's answer at the note site — the plans-before-writes law holds —
// and the answer names which resolutions were used and which were not.

TEST(resolve_settled_both_changed_takes_resolved)
{
    MergeStubPort port;
    const std::string baseBlob = mergeSeedBlob(port, kBaseText);
    const std::string oursBlob = mergeSeedBlob(port, "alpha\nBETA OURS\ngamma\n");
    const std::string theirsBlob = mergeSeedBlob(port, "alpha\nBETA THEIRS\ngamma\n");
    const std::string baseTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), baseBlob}});
    const std::string targetTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), oursBlob}});
    const std::string headTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), theirsBlob}});
    const std::string cBase = mergeSeedCommit(port, baseTree, std::vector<std::string>(), "b\n");
    const std::string cTarget = mergeSeedCommit(port, targetTree, {cBase}, "t\n");
    const std::string cHead = mergeSeedCommit(port, headTree, {cBase}, "h\n");

    const std::string resolvedBlob = mergeSeedBlob(port, "resolved\n");
    std::vector<ols::gitcore::MergeResolution> resolutions;
    ols::gitcore::MergeResolution res;
    res.path = "a.txt";
    res.content = "resolved\n";
    resolutions.push_back(res);

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, cTarget, cHead, ols::gitcore::MergeStyle::MergeCommit,
                                  resolutions, out, error));
    CHECK(out.clean);
    // The tree the resolution implies: the resolved blob under the ours' mode.
    const std::string wantTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), resolvedBlob}});
    CHECK_EQ(out.mergedTreeOid, wantTree);
    CHECK(!out.mergedCommitOid.empty());
    CHECK_EQ(out.conflicts.size(), std::size_t(0));
    CHECK_EQ(out.took.size(), std::size_t(1));
    CHECK_EQ(out.took[0], std::string("a.txt"));
    CHECK(out.unused.empty());
    // The blob, the tree, and the commit: the resolved content was planned,
    // and only then written.
    CHECK_EQ(port.written().size(), std::size_t(3));
}

TEST(resolve_settled_add_add_stands_resolved)
{
    MergeStubPort port;
    const std::string keep = mergeSeedBlob(port, "keep\n");
    const std::string ours = mergeSeedBlob(port, "ours\n");
    const std::string theirs = mergeSeedBlob(port, "theirs\n");
    const std::string baseTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("keep.txt"), keep}});
    const std::string targetTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("keep.txt"), keep},
                             {std::string("100644"), std::string("new.txt"), ours}});
    const std::string headTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("keep.txt"), keep},
                             {std::string("100644"), std::string("new.txt"), theirs}});
    const std::string cBase = mergeSeedCommit(port, baseTree, std::vector<std::string>(), "b\n");
    const std::string cTarget = mergeSeedCommit(port, targetTree, {cBase}, "t\n");
    const std::string cHead = mergeSeedCommit(port, headTree, {cBase}, "h\n");

    const std::string resolvedBlob = mergeSeedBlob(port, "resolved\n");
    std::vector<ols::gitcore::MergeResolution> resolutions;
    ols::gitcore::MergeResolution res;
    res.path = "new.txt";
    res.content = "resolved\n";
    resolutions.push_back(res);

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, cTarget, cHead, ols::gitcore::MergeStyle::MergeCommit,
                                  resolutions, out, error));
    CHECK(out.clean);
    const std::string wantTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("keep.txt"), keep},
                             {std::string("100644"), std::string("new.txt"), resolvedBlob}});
    CHECK_EQ(out.mergedTreeOid, wantTree);
    CHECK_EQ(out.conflicts.size(), std::size_t(0));
    CHECK_EQ(out.took.size(), std::size_t(1));
    CHECK(out.unused.empty());
}

TEST(resolve_delete_over_edit_vs_delete)
{
    MergeStubPort port;
    const std::string keep = mergeSeedBlob(port, "keep\n");
    const std::string baseBlob = mergeSeedBlob(port, kBaseText);
    const std::string edited = mergeSeedBlob(port, "alpha\nbeta\nGAMMA\n");
    const std::string baseTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), baseBlob},
                             {std::string("100644"), std::string("keep.txt"), keep}});
    const std::string targetTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("keep.txt"), keep}});
    const std::string headTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), edited},
                             {std::string("100644"), std::string("keep.txt"), keep}});
    const std::string cBase = mergeSeedCommit(port, baseTree, std::vector<std::string>(), "b\n");
    const std::string cTarget = mergeSeedCommit(port, targetTree, {cBase}, "t\n");
    const std::string cHead = mergeSeedCommit(port, headTree, {cBase}, "h\n");

    std::vector<ols::gitcore::MergeResolution> resolutions;
    ols::gitcore::MergeResolution res;
    res.path = "a.txt";
    res.deleteFile = true;
    resolutions.push_back(res);

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, cTarget, cHead, ols::gitcore::MergeStyle::MergeCommit,
                                  resolutions, out, error));
    CHECK(out.clean);
    // The deletion answers with the absence of the entry: the merged tree is the
    // target's own, carried by its id — only the commit is written.
    CHECK_EQ(out.mergedTreeOid, targetTree);
    CHECK(!out.mergedCommitOid.empty());
    CHECK_EQ(out.conflicts.size(), std::size_t(0));
    CHECK_EQ(out.took.size(), std::size_t(1));
    CHECK(out.unused.empty());
    CHECK_EQ(port.written().size(), std::size_t(1));
}

TEST(resolve_unused_named_when_nothing_conflicted)
{
    MergeStubPort port;
    const CleanFixture f = seedCleanFixture(port);

    // The resolution names a path the merge settles on its own: ours changed
    // a.txt and the head never touched it. Nothing conflicts, so the answer is
    // the clean merge and the resolution is named unused.
    std::vector<ols::gitcore::MergeResolution> resolutions;
    ols::gitcore::MergeResolution res;
    res.path = "a.txt";
    res.content = "not needed\n";
    resolutions.push_back(res);

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, f.target, f.head, ols::gitcore::MergeStyle::MergeCommit,
                                  resolutions, out, error));
    CHECK(out.clean);
    CHECK_EQ(out.conflicts.size(), std::size_t(0));
    CHECK(out.took.empty());
    CHECK_EQ(out.unused.size(), std::size_t(1));
    CHECK_EQ(out.unused[0], std::string("a.txt"));
}

TEST(resolve_partial_still_conflicts_writes_nothing)
{
    MergeStubPort port;
    const std::string baseBlob = mergeSeedBlob(port, kBaseText);
    const std::string oursBlob = mergeSeedBlob(port, "alpha\nBETA OURS\ngamma\n");
    const std::string theirsBlob = mergeSeedBlob(port, "alpha\nBETA THEIRS\ngamma\n");
    const std::string baseTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), baseBlob}});
    const std::string targetTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), oursBlob},
                             {std::string("100644"), std::string("b.txt"),
                              mergeSeedBlob(port, "ours b\n")}});
    const std::string headTree =
        mergeSeedTree(port, {{std::string("100644"), std::string("a.txt"), theirsBlob},
                             {std::string("100644"), std::string("b.txt"),
                              mergeSeedBlob(port, "theirs b\n")}});
    const std::string cBase = mergeSeedCommit(port, baseTree, std::vector<std::string>(), "b\n");
    const std::string cTarget = mergeSeedCommit(port, targetTree, {cBase}, "t\n");
    const std::string cHead = mergeSeedCommit(port, headTree, {cBase}, "h\n");

    std::vector<ols::gitcore::MergeResolution> resolutions;
    ols::gitcore::MergeResolution res;
    res.path = "a.txt";
    res.content = "resolved\n";
    resolutions.push_back(res);

    ols::gitcore::MergeOutcome out;
    std::string error;
    CHECK(ols::gitcore::mergePull(port, cTarget, cHead, ols::gitcore::MergeStyle::MergeCommit,
                                  resolutions, out, error));
    // The settled path is answered; the other still conflicts: the answer is
    // clean:false with only that path, and a still-conflicting merge writes
    // no object at all.
    CHECK(out.clean == false);
    CHECK(mergeConflictAt(out, "a.txt") == nullptr);
    const ols::gitcore::MergeConflict *c = mergeConflictAt(out, "b.txt");
    CHECK(c != nullptr);
    if (c != nullptr)
    {
        CHECK_EQ(c->kind, std::string("add-add"));
    }
    CHECK_EQ(port.written().size(), std::size_t(0));
    CHECK_EQ(out.took.size(), std::size_t(1));
    CHECK(out.unused.empty());
}

} // namespace
