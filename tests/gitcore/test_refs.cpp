// tests/gitcore/test_refs.cpp — the cases that prove refs.h.
//
// Every repo here is real and lives in a temp folder of its own: initBare
// writes the layout, the cases read and move refs the way the studio will, and
// each claim states what the bytes on disk must therefore be. The fixture case
// reads packed-refs-sample.txt as git.exe wrote it; the fixture itself carries
// one real ref line among git's own comment lines, and the parse is checked
// against exactly that. The oids are the fixture repo's own five objects, so
// every value named here is a value git made.
//
// What git says about a repo initBare made is checked beside this harness, not
// inside it: the case cannot run git, and the task's self-check runs
// git fsck and git log over the same layout the first case describes.

#include "gitcore/refs.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

using ols::gitcore::ObjectId;
using ols::gitcore::PackedRefEntry;
using ols::gitcore::RefInfo;
using ols::gitcore::ReflogEntry;
using ols::gitcore::RefStatus;
using ols::gitcore::RefUpdate;
using ols::gitcore::Repository;

// ---- the fixture repo's own oids ----
const char* const kCommitA = "a867ef798b215db19eaf0c7b615c789792fe62b9";  // the commit
const char* const kTreeD = "d25250f5f7f874caffa476f3a69161b879d2cbc6";    // its tree
const char* const kBlobE = "e0396ffc6284bb3b460e027c49d05bbb31b273e1";    // a1.txt
const char* const kBlobF = "f63ac6b34ade6c27cb84d19886049042a8bc2ce4";    // nested/sub.txt
const char* const kTree77 = "77626866b9dc450241b7ee8de07a25a327480109";   // nested/

// ---- temp folders, one per case, gone when the case is over ----

std::filesystem::path make_temp(const std::string& tag)
{
    static int seq = 0;
    std::error_code ec;
    std::filesystem::path base = ols_test::scratchRoot();
    if (base.empty()) {
        return std::filesystem::path();  // a case with no temp says so through
                                         // its own failed claims
    }
    base /= "ols-b3-" + tag + "-" + std::to_string(static_cast<long long>(std::time(nullptr))) +
            "-" + std::to_string(++seq);
    std::filesystem::remove_all(base, ec);
    return base;
}

struct TempDir {
    std::filesystem::path p;
    std::string gone;   // p as a plain string, for the API's door
    explicit TempDir(const std::string& tag) : p(make_temp(tag)), gone(p.string()) {}
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
    }
};

bool is_dir(const std::filesystem::path& p)
{
    std::error_code ec;
    return std::filesystem::is_directory(p, ec);
}

bool is_file(const std::filesystem::path& p)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

std::string slurp(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return std::string();
    }
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

bool has_file_named_ending(const std::filesystem::path& root, const std::string& tail)
{
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code tec;
        if (it->is_regular_file(tec)) {
            const std::string name = it->path().filename().u8string();
            if (name.size() >= tail.size() &&
                name.compare(name.size() - tail.size(), tail.size(), tail) == 0) {
                return true;
            }
        }
    }
    return false;
}

} // anonymous namespace

// The layout initBare promises: every directory of the studio layout, the
// three files, HEAD a symref to the default branch, config saying bare, and
// hooks/ left empty because the studio manages its own.
TEST(refs_init_bare_layout)
{
    TempDir t("init");
    CHECK(!t.p.empty());
    CHECK(ols::gitcore::initBare(t.gone, "main"));

    CHECK(is_dir(t.p / "objects" / "info"));
    CHECK(is_dir(t.p / "objects" / "pack"));
    CHECK(is_dir(t.p / "refs" / "heads"));
    CHECK(is_dir(t.p / "refs" / "tags"));
    CHECK(is_dir(t.p / "hooks"));
    CHECK(is_file(t.p / "HEAD"));
    CHECK(is_file(t.p / "config"));
    CHECK(is_file(t.p / "description"));

    std::error_code ec;
    CHECK(std::filesystem::is_empty(t.p / "hooks", ec));

    CHECK_EQ(slurp(t.p / "HEAD"), "ref: refs/heads/main\n");
    const std::string config = slurp(t.p / "config");
    CHECK(config.find("repositoryformatversion = 0") != std::string::npos);
    CHECK(config.find("bare = true") != std::string::npos);
    CHECK(config.find("defaultBranch = main") != std::string::npos);
    CHECK(slurp(t.p / "description").find("Unnamed repository") != std::string::npos);

    // A default branch of its own names itself in HEAD and in config.
    TempDir t2("initdev");
    CHECK(ols::gitcore::initBare(t2.gone, "develop"));
    CHECK_EQ(slurp(t2.p / "HEAD"), "ref: refs/heads/develop\n");
    CHECK(slurp(t2.p / "config").find("defaultBranch = develop") != std::string::npos);

    // A name no ref may wear is refused before the directory is even made.
    TempDir t3("initbad");
    CHECK(!ols::gitcore::initBare(t3.gone, "no..good"));
}

// HEAD is a symref at birth and resolves once its branch exists; detaching
// moves it to an oid, and it answers as a plain ref again.
TEST(refs_head_symref_resolves)
{
    TempDir t("head");
    CHECK(ols::gitcore::initBare(t.gone, "main"));
    Repository repo(t.gone);

    CHECK(repo.isHeadSymref());
    std::string target;
    CHECK(repo.headTarget(target));
    CHECK_EQ(target, "refs/heads/main");

    ObjectId out;
    // The branch does not exist yet: HEAD says where it would look, and finds
    // nothing.
    CHECK(!repo.readHead(out));

    CHECK(repo.writeLooseRef("refs/heads/main", ObjectId(kCommitA)));
    CHECK(repo.readHead(out));
    CHECK_EQ(out.to_hex(), std::string(kCommitA));

    CHECK(repo.detachHead(ObjectId(kBlobE)));
    CHECK(!repo.isHeadSymref());
    CHECK(repo.readHead(out));
    CHECK_EQ(out.to_hex(), std::string(kBlobE));
}

// A loose ref written is a file "<hex>\n" that reads back through resolve —
// and writing it again moves it, atomically replaced.
TEST(refs_loose_round_trip)
{
    TempDir t("loose");
    CHECK(ols::gitcore::initBare(t.gone, "main"));
    Repository repo(t.gone);

    CHECK(repo.writeLooseRef("refs/heads/topic", ObjectId(kTreeD)));
    CHECK(is_file(t.p / "refs" / "heads" / "topic"));
    CHECK_EQ(slurp(t.p / "refs" / "heads" / "topic"), std::string(kTreeD) + "\n");

    ObjectId out;
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/topic", out)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(out.to_hex(), std::string(kTreeD));

    // An uppercase oid arrives lowercase, because the hex on disk is lowercase.
    CHECK(repo.writeLooseRef("refs/heads/topic",
                             ObjectId("F63AC6B34ADE6C27CB84D19886049042A8BC2CE4")));
    CHECK_EQ(slurp(t.p / "refs" / "heads" / "topic"), std::string(kBlobF) + "\n");

    // An id that is not an id, and a name that is not a name, move nothing.
    CHECK(!repo.writeLooseRef("refs/heads/topic", ObjectId("not-an-oid")));
    CHECK_EQ(slurp(t.p / "refs" / "heads" / "topic"), std::string(kBlobF) + "\n");
    CHECK(!repo.writeLooseRef("refs/heads/bad name", ObjectId(kBlobE)));
}

// The golden packed-refs, as git.exe wrote it: one real line among header and
// comment lines, and the parse sees exactly the one ref. Then the shapes the
// fixture does not carry but git writes: a "^oid" peel line, and a file the
// parse must refuse.
TEST(refs_packed_parse_and_write)
{
    const std::string sample = ols_test::read_fixture("packed-refs-sample.txt");
    CHECK(!sample.empty());

    std::vector<PackedRefEntry> entries;
    CHECK(ols::gitcore::parsePackedRefs(sample, entries));
    CHECK_EQ(entries.size(), 1u);
    if (entries.size() == 1) {
        CHECK_EQ(entries[0].oid, std::string(kCommitA));
        CHECK_EQ(entries[0].name, std::string("refs/heads/master"));
        CHECK(!entries[0].peeled);
    }

    // A peel line folds into the tag entry it follows.
    const std::string peeled =
        "# pack-refs with: peeled fully-peeled sorted \n"
        "e0396ffc6284bb3b460e027c49d05bbb31b273e1 refs/tags/v1\n"
        "^a867ef798b215db19eaf0c7b615c789792fe62b9\n"
        "d25250f5f7f874caffa476f3a69161b879d2cbc6 refs/heads/main\n";
    CHECK(ols::gitcore::parsePackedRefs(peeled, entries));
    CHECK_EQ(entries.size(), 2u);
    if (entries.size() == 2) {
        CHECK(entries[0].peeled);
        CHECK_EQ(entries[0].peeledOid, std::string(kCommitA));
        CHECK(!entries[1].peeled);
    }

    // A line that is neither comment, blank, ref, nor peel refuses the whole
    // file: half a packed-refs read is worse than none.
    const std::string junk =
        "# pack-refs with: peeled fully-peeled sorted \n"
        "zz00000000000000000000000000000000000000 refs/heads/main\n";
    CHECK(!ols::gitcore::parsePackedRefs(junk, entries));
    CHECK_EQ(entries.size(), 0u);

    // And the round trip through a repo: write, read, peel intact; a bad
    // entry refuses the write and leaves the old file alone.
    TempDir t("packed");
    CHECK(ols::gitcore::initBare(t.gone, "main"));
    Repository repo(t.gone);
    std::vector<PackedRefEntry> two = {
        {std::string(kBlobE), "refs/tags/v1", true, std::string(kCommitA)},
        {std::string(kTreeD), "refs/heads/main", false, std::string()},
    };
    CHECK(repo.writePackedRefs(two));
    std::vector<PackedRefEntry> back;
    CHECK(repo.readPackedRefs(back));
    CHECK_EQ(back.size(), 2u);
    if (back.size() == 2) {
        CHECK_EQ(back[0].name, std::string("refs/heads/main"));  // sorted by name
        CHECK_EQ(back[1].name, std::string("refs/tags/v1"));
        CHECK(back[1].peeled);
        CHECK_EQ(back[1].peeledOid, std::string(kCommitA));
    }
    std::vector<PackedRefEntry> bad = {{std::string("zz"), "refs/heads/x", false, {}}};
    CHECK(!repo.writePackedRefs(bad));
    CHECK(repo.readPackedRefs(back));
    CHECK_EQ(back.size(), 2u);
}

// A repo that has never packed has no packed-refs at all: the read answers an
// empty set true, not a refusal - the file is optional, and a delete of
// loose-only refs must not be held off by its absence (measured live: every
// studio repo was undeletable while absence read as failure). A file that
// stands and does not parse still refuses.
TEST(refs_packed_absent_reads_empty_not_refused)
{
    TempDir t("packedabsent");
    CHECK(ols::gitcore::initBare(t.gone, "main"));
    Repository repo(t.gone);
    std::vector<PackedRefEntry> out;
    CHECK(repo.readPackedRefs(out));
    CHECK_EQ(out.size(), 0u);
    // Junk standing where the file should be refuses, as ever.
    {
        std::ofstream junk(t.p / "packed-refs", std::ios::binary);
        junk << "nonsense not a ref line\n";
    }
    CHECK(!repo.readPackedRefs(out));
}

// Loose wins over packed, the way git resolves it: a packed master, then a
// loose master written, and the loose value is the ref's value — while the
// packed file still states its old one underneath.
TEST(refs_loose_over_packed)
{
    TempDir t("prec");
    CHECK(ols::gitcore::initBare(t.gone, "main"));
    Repository repo(t.gone);

    std::vector<PackedRefEntry> packed = {
        {std::string(kCommitA), "refs/heads/master", false, std::string()},
        {std::string(kBlobE), "refs/tags/stale", false, std::string()},
    };
    CHECK(repo.writePackedRefs(packed));

    ObjectId out;
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/master", out)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(out.to_hex(), std::string(kCommitA));

    CHECK(repo.writeLooseRef("refs/heads/master", ObjectId(kBlobF)));
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/master", out)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(out.to_hex(), std::string(kBlobF));
    CHECK_EQ(slurp(t.p / "packed-refs").find(kCommitA) != std::string::npos, true);

    // The listing: HEAD, both heads and tags rows, loose stated over packed.
    CHECK(repo.writeLooseRef("refs/tags/v1", ObjectId(kTreeD)));
    std::vector<RefInfo> refs;
    CHECK(repo.allRefs(refs));
    CHECK_EQ(refs.size(), 4u);
    if (refs.size() == 4) {
        CHECK_EQ(refs[0].name, std::string("HEAD"));
        CHECK(refs[0].is_symref);
        CHECK_EQ(refs[0].target, std::string("refs/heads/main"));
        CHECK(refs[0].oid.to_hex().empty());  // main is nowhere; HEAD resolves to no oid
        CHECK_EQ(refs[1].name, std::string("refs/heads/master"));
        CHECK_EQ(refs[1].oid.to_hex(), std::string(kBlobF));  // loose, not packed
        CHECK_EQ(refs[2].name, std::string("refs/tags/stale"));
        CHECK_EQ(refs[2].oid.to_hex(), std::string(kBlobE));  // packed, no loose
        CHECK_EQ(refs[3].name, std::string("refs/tags/v1"));
        CHECK_EQ(refs[3].oid.to_hex(), std::string(kTreeD));
    }

    // A symref loose ref states its target in the listing too.
    CHECK(repo.writeLooseRef("refs/heads/main", ObjectId(kTree77)));
    CHECK(repo.writeLooseSymref("refs/heads/trunk", "refs/heads/main"));
    CHECK(repo.allRefs(refs));
    bool saw_trunk = false;
    for (const RefInfo& r : refs) {
        if (r.name == "refs/heads/trunk") {
            saw_trunk = true;
            CHECK(r.is_symref);
            CHECK_EQ(r.target, std::string("refs/heads/main"));
            CHECK_EQ(r.oid.to_hex(), std::string(kTree77));  // resolved through
        }
    }
    CHECK(saw_trunk);
}

// Two updates commit together: each with its expected old value where it has
// one, both moved when the whole set validates.
TEST(refs_transaction_commits_together)
{
    TempDir t("txn");
    CHECK(ols::gitcore::initBare(t.gone, "main"));
    Repository repo(t.gone);
    CHECK(repo.writeLooseRef("refs/heads/a", ObjectId(kCommitA)));
    CHECK(repo.writeLooseRef("refs/heads/b", ObjectId(kTreeD)));

    std::vector<RefUpdate> updates;
    RefUpdate ua;
    ua.name = "refs/heads/a";
    ua.newOid = ObjectId(kBlobE);
    ua.expectedOldOid = ObjectId(kCommitA);
    updates.push_back(ua);
    RefUpdate ub;
    ub.name = "refs/heads/b";
    ub.newOid = ObjectId(kBlobF);  // unconditional
    updates.push_back(ub);
    CHECK(repo.commitRefs(updates));

    ObjectId out;
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/a", out)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(out.to_hex(), std::string(kBlobE));
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/b", out)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(out.to_hex(), std::string(kBlobF));

    // A brand-new ref rides the same transaction.
    std::vector<RefUpdate> fresh;
    RefUpdate uc;
    uc.name = "refs/tags/v2";
    uc.newOid = ObjectId(kTree77);
    fresh.push_back(uc);
    CHECK(repo.commitRefs(fresh));
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/tags/v2", out)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(out.to_hex(), std::string(kTree77));
}

// One wrong expectedOldOid aborts the whole set — including an update earlier
// in the list that would have passed on its own — and the refusal leaves no
// ".new" staging file anywhere and every ref exactly where it was. That is the
// crash-safety claim: validate before moving, so a refusal never had a byte to
// undo.
TEST(refs_transaction_aborts_all)
{
    TempDir t("txnabort");
    CHECK(ols::gitcore::initBare(t.gone, "main"));
    Repository repo(t.gone);
    CHECK(repo.writeLooseRef("refs/heads/a", ObjectId(kCommitA)));
    CHECK(repo.writeLooseRef("refs/heads/c", ObjectId(kTreeD)));
    const std::string a_before = slurp(t.p / "refs" / "heads" / "a");
    const std::string c_before = slurp(t.p / "refs" / "heads" / "c");

    std::vector<RefUpdate> updates;
    RefUpdate ua;
    ua.name = "refs/heads/a";
    ua.newOid = ObjectId(kBlobE);
    ua.expectedOldOid = ObjectId(kCommitA);  // would pass alone
    updates.push_back(ua);
    RefUpdate uc;
    uc.name = "refs/heads/c";
    uc.newOid = ObjectId(kBlobF);
    uc.expectedOldOid = ObjectId(kBlobE);  // the truth is kTreeD
    updates.push_back(uc);
    CHECK(!repo.commitRefs(updates));

    // Nothing moved, and nothing left behind.
    CHECK_EQ(slurp(t.p / "refs" / "heads" / "a"), a_before);
    CHECK_EQ(slurp(t.p / "refs" / "heads" / "c"), c_before);
    ObjectId out;
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/a", out)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(out.to_hex(), std::string(kCommitA));
    CHECK(!has_file_named_ending(t.p, ".new"));

    // Other refusals of the same door: a duplicate ref, a null new value, a
    // bad name, a move through a symref — each still leaves no staging file.
    RefUpdate dup;
    dup.name = "refs/heads/a";  // the same ref ua wants: two truths for one ref
    dup.newOid = ObjectId(kBlobF);
    CHECK(!repo.commitRefs({ua, dup}));
    RefUpdate null_oid;
    null_oid.name = "refs/heads/a";
    null_oid.newOid = ObjectId(std::string(40, '0'));
    CHECK(!repo.commitRefs({null_oid}));
    RefUpdate bad_name;
    bad_name.name = "refs/heads/bad..name";
    bad_name.newOid = ObjectId(kBlobF);
    CHECK(!repo.commitRefs({bad_name}));
    CHECK(repo.writeLooseSymref("refs/heads/branch", "refs/heads/a"));
    RefUpdate through_symref;
    through_symref.name = "refs/heads/branch";
    through_symref.newOid = ObjectId(kBlobF);
    CHECK(!repo.commitRefs({through_symref}));
    CHECK(!has_file_named_ending(t.p, ".new"));
    CHECK_EQ(slurp(t.p / "refs" / "heads" / "a"), a_before);
}

// The reflog: "<old> <new> <who> <when>\t<msg>" appended and read back field
// for field, and the keep policy leaving the last N standing.
TEST(refs_reflog_append_read_prune)
{
    TempDir t("reflog");
    CHECK(ols::gitcore::initBare(t.gone, "main"));
    Repository repo(t.gone);
    CHECK_EQ(repo.reflogKeep(), ols::gitcore::kDefaultReflogKeep);

    ReflogEntry e1;
    e1.oldOid = std::string(40, '0');
    e1.newOid = kCommitA;
    e1.who = "Studio Bot <studio@local>";
    e1.when = "1704067200 +0000";
    e1.msg = "commit: first fixture commit";
    CHECK(repo.reflogAppend("refs/heads/main", e1));

    ReflogEntry e2;
    e2.oldOid = kCommitA;
    e2.newOid = kTreeD;
    e2.who = "Studio Bot <studio@local>";
    e2.when = "1704070800 +0100";
    e2.msg = "reset: moved to the tree";
    CHECK(repo.reflogAppend("refs/heads/main", e2));

    // The line on disk is the git-shaped line.
    const std::string raw = slurp(t.p / "logs" / "refs" / "heads" / "main");
    CHECK_EQ(raw, std::string(40, '0') + " " + kCommitA +
                      " Studio Bot <studio@local> 1704067200 +0000\tcommit: first fixture commit\n" +
                      kCommitA + " " + kTreeD +
                      " Studio Bot <studio@local> 1704070800 +0100\treset: moved to the tree\n");

    std::vector<ReflogEntry> log;
    CHECK(repo.reflogRead("refs/heads/main", log));
    CHECK_EQ(log.size(), 2u);
    if (log.size() == 2) {
        CHECK_EQ(log[0].oldOid, std::string(40, '0'));
        CHECK_EQ(log[0].newOid, std::string(kCommitA));
        CHECK_EQ(log[0].who, std::string("Studio Bot <studio@local>"));
        CHECK_EQ(log[0].when, std::string("1704067200 +0000"));
        CHECK_EQ(log[0].msg, std::string("commit: first fixture commit"));
        CHECK_EQ(log[1].msg, std::string("reset: moved to the tree"));
    }

    // HEAD logs beside its branch.
    ReflogEntry eh = e1;
    eh.msg = "checkout: moving from nowhere to main";
    CHECK(repo.reflogAppend("HEAD", eh));
    CHECK(is_file(t.p / "logs" / "HEAD"));

    // Prune within the keep leaves the log alone; a keep of one leaves the
    // last entry standing, newest first as always.
    CHECK(repo.reflogPrune("refs/heads/main"));
    CHECK(repo.reflogRead("refs/heads/main", log));
    CHECK_EQ(log.size(), 2u);
    repo.setReflogKeep(1);
    ReflogEntry e3 = e2;
    e3.msg = "commit: second";
    CHECK(repo.reflogAppend("refs/heads/main", e3));
    CHECK(repo.reflogPrune("refs/heads/main"));
    CHECK(repo.reflogRead("refs/heads/main", log));
    CHECK_EQ(log.size(), 1u);
    if (!log.empty()) {
        CHECK_EQ(log[0].msg, std::string("commit: second"));
    }

    // A line the format does not describe is refused, not folded in.
    ReflogEntry bad = e1;
    bad.msg = "two\tthings";
    CHECK(!repo.reflogAppend("refs/heads/main", bad));
    ReflogEntry worse = e1;
    worse.when = "not a stamp";
    CHECK(!repo.reflogAppend("refs/heads/main", worse));
    // A ref with no log reads empty — history is not a failure.
    std::vector<ReflogEntry> none;
    CHECK(repo.reflogRead("refs/heads/never", none));
    CHECK_EQ(none.size(), 0u);
}

// The ten symref hops and the two ways a chain misbehaves: a loop and a well
// formed ref pointing nowhere. The listing of a junk ref file is refused, as
// a junk listing is worse than none.
TEST(refs_resolve_depth_and_junk)
{
    TempDir t("depth");
    CHECK(ols::gitcore::initBare(t.gone, "main"));
    Repository repo(t.gone);
    ObjectId out;

    // A nine-hop chain stays under the cap of ten.
    CHECK(repo.writeLooseRef("refs/heads/c9", ObjectId(kBlobE)));
    for (int i = 8; i >= 0; --i) {
        CHECK(repo.writeLooseSymref("refs/heads/c" + std::to_string(i),
                                    "refs/heads/c" + std::to_string(i + 1)));
    }
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/c0", out)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(out.to_hex(), std::string(kBlobE));

    // A twelve-hop chain is refused by the cap, and named so.
    CHECK(repo.writeLooseRef("refs/heads/f11", ObjectId(kBlobE)));
    for (int i = 10; i >= 0; --i) {
        CHECK(repo.writeLooseSymref("refs/heads/f" + std::to_string(i),
                                    "refs/heads/f" + std::to_string(i + 1)));
    }
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/f0", out)),
             static_cast<int>(RefStatus::TooDeep));

    // A loop, direct and mutual, is TooDeep the same way.
    CHECK(repo.writeLooseSymref("refs/heads/loop", "refs/heads/loop"));
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/loop", out)),
             static_cast<int>(RefStatus::TooDeep));
    CHECK(repo.writeLooseSymref("refs/heads/mut", "refs/heads/loop"));
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/mut", out)),
             static_cast<int>(RefStatus::TooDeep));

    // Missing is Missing; junk is Bad.
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/nothing", out)),
             static_cast<int>(RefStatus::Missing));
    {
        std::ofstream junk(t.p / "refs" / "heads" / "junk", std::ios::binary);
        junk << "not an oid at all\n";
    }
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/junk", out)),
             static_cast<int>(RefStatus::Bad));
    // A symref whose target is no name is Bad rather than a chase.
    {
        std::ofstream f(t.p / "refs" / "heads" / "broken", std::ios::binary);
        f << "ref: not a target\n";
    }
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/broken", out)),
             static_cast<int>(RefStatus::Bad));

    // delete takes the loose file away and the packed value underneath shows
    // through; a ref with no loose file says there was nothing to delete.
    std::vector<PackedRefEntry> packed = {
        {std::string(kTree77), "refs/heads/dup", false, std::string()},
    };
    CHECK(repo.writePackedRefs(packed));
    CHECK(repo.writeLooseRef("refs/heads/dup", ObjectId(kBlobF)));
    CHECK(repo.deleteLooseRef("refs/heads/dup"));
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/dup", out)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(out.to_hex(), std::string(kTree77));  // back to the packed truth
    CHECK(!repo.deleteLooseRef("refs/heads/dup"));  // no loose file any more
}

// The name rules, git's own, refused not escaped — ten that pass and ten that
// must not.
TEST(refs_name_validation_matrix)
{
    // Ten names that are refs.
    CHECK(ols::gitcore::isValidRefName("refs/heads/main"));
    CHECK(ols::gitcore::isValidRefName("refs/heads/feature/nested-thing"));
    CHECK(ols::gitcore::isValidRefName("refs/tags/v1.0.0-rc.2"));
    CHECK(ols::gitcore::isValidRefName("refs/pull/12/head"));
    CHECK(ols::gitcore::isValidRefName("main"));              // one component is a ref too
    CHECK(ols::gitcore::isValidRefName("HEAD"));              // and so is the handle
    CHECK(ols::gitcore::isValidRefName("refs/heads/user@example.com"));  // lone @ passes
    CHECK(ols::gitcore::isValidRefName("refs/heads/mühlhausen"));        // utf8-ish printable
    CHECK(ols::gitcore::isValidRefName("refs/heads/a/b/c/d/e/f"));
    CHECK(ols::gitcore::isValidRefName("refs/heads/x.lockx"));  // .lock only at the end

    // Ten that are not.
    CHECK(!ols::gitcore::isValidRefName(""));
    CHECK(!ols::gitcore::isValidRefName("refs/heads/a..b"));   // the ".." escape
    CHECK(!ols::gitcore::isValidRefName("/refs/heads/x"));     // leading /
    CHECK(!ols::gitcore::isValidRefName("refs/heads/x/"));     // trailing /
    CHECK(!ols::gitcore::isValidRefName("refs//heads"));       // empty component
    CHECK(!ols::gitcore::isValidRefName("refs/heads/.hidden"));// component starts with .
    CHECK(!ols::gitcore::isValidRefName("refs/heads/x.lock")); // a staging name
    CHECK(!ols::gitcore::isValidRefName("refs/heads/x y"));    // space
    CHECK(!ols::gitcore::isValidRefName("refs/heads/x\ty"));   // and any control char
    CHECK(!ols::gitcore::isValidRefName("refs/heads/x@{y}"));  // the reflog syntax
    // The rest of git's banned punctuation, refused too.
    CHECK(!ols::gitcore::isValidRefName("refs/heads/x~1"));
    CHECK(!ols::gitcore::isValidRefName("refs/heads/x^2"));
    CHECK(!ols::gitcore::isValidRefName("refs/heads/x:y"));
    CHECK(!ols::gitcore::isValidRefName("refs/heads/x?"));
    CHECK(!ols::gitcore::isValidRefName("refs/heads/x*"));
    CHECK(!ols::gitcore::isValidRefName("refs/heads/x[y]"));
    CHECK(!ols::gitcore::isValidRefName("refs\\heads\\x"));
    CHECK(!ols::gitcore::isValidRefName("refs/heads/"));       // empty last component
    CHECK(!ols::gitcore::isValidRefName("@"));                 // the lone @
    CHECK(!ols::gitcore::isValidRefName("refs/heads/x."));     // trailing dot

    // The symref helpers, and the id, on the same refuse-don't-escape footing.
    std::string target;
    CHECK(ols::gitcore::looksLikeSymref("ref: refs/heads/main"));
    CHECK(ols::gitcore::symrefTarget("ref: refs/heads/main\n", target));
    CHECK_EQ(target, std::string("refs/heads/main"));
    CHECK(!ols::gitcore::symrefTarget("ref: bad name", target));
    CHECK(!ols::gitcore::looksLikeSymref("ref refs/heads/main"));
    CHECK(ols::gitcore::isValidOidHex(kCommitA));
    CHECK(!ols::gitcore::isValidOidHex("zz67ef798b215db19eaf0c7b615c789792fe62b9"));
    CHECK(!ols::gitcore::isValidOidHex("a867ef798b215db19eaf0c7b615c789792fe62b"));  // 39
    ObjectId oid;
    CHECK(ObjectId::parse("A867EF798B215DB19EAF0C7B615C789792FE62B9", oid));
    CHECK_EQ(oid.to_hex(), std::string(kCommitA));  // lowered as it came in
    CHECK(!ObjectId::parse("nope", oid));
    CHECK(ObjectId(std::string(40, '0')).is_null());
    CHECK(!oid.is_null());
}

// A branch name carrying a slash stands for a folder of its own, and a
// repository sitting under a few folders passes the 260 characters the short
// Windows file API reads without the path having done anything unusual. Such a
// ref has to be written, read, listed and deleted like any other: an answer of
// "not here" to a ref that is here is worse than a crash, because the next
// write over it is a lost branch, and this is the case that was failing.
TEST(refs_loose_beyond_the_short_path_limit)
{
    TempDir t("longpath");
    if (t.p.empty()) {
        return;
    }
    CHECK(initBare(t.gone, "main"));
    Repository repo(t.gone);

    std::string deep = "refs/heads";
    while ((t.gone + "/" + deep).size() < 300u) {
        deep += "/adeeperfoldername";
    }
    const std::string name = deep + "/leaf";
    // The claim of the case, stated once as a number: the path of the file the
    // ref lives in is past what the short API takes.
    CHECK((t.gone + "/" + name).size() > 260u);
    CHECK(ols::gitcore::isValidRefName(name));

    const ObjectId want(kCommitA);
    CHECK(repo.writeLooseRef(name, want));

    ObjectId out;
    CHECK(repo.resolveRef(name, out) == RefStatus::Ok);
    CHECK_EQ(out.hex, want.hex);

    std::vector<RefInfo> all;
    CHECK(repo.allRefs(all));
    bool named = false;
    for (const RefInfo& r : all) {
        if (r.name == name) {
            named = true;
            CHECK_EQ(r.oid.hex, want.hex);
        }
    }
    CHECK(named);   // listed under its own name, not skipped as unreadable

    // It is a ref like any other on the way out too: the file goes, and the
    // folders it stood in go with it, which is the same API at the same depth.
    CHECK(repo.deleteLooseRef(name));
    CHECK(repo.resolveRef(name, out) == RefStatus::Missing);
    std::vector<RefInfo> after;
    CHECK(repo.allRefs(after));
    for (const RefInfo& r : after) {
        CHECK(r.name != name);
    }
}

// Two writers, one ref, the same expected old value, both ready before
// either is let go: the compare-and-swap of commitRefs must crown exactly
// one. Without the transaction's exclusive lock both read the old value in
// the validation phase, both staged at the one shared "<ref>.new" — one
// clobbering the other's bytes — and both renamed and were told they won;
// the loser's caller believed a swap that had not happened. With the lock,
// the loser is refused at the exclusive create (or, when it enters after
// the winner's move, at the expected-old), and the ref ends as the
// winner's stated truth with nothing left behind.
TEST(refs_transaction_lock_resolves_the_two_writer_race)
{
    TempDir t("txn2w");
    CHECK(initBare(t.gone, "main"));
    Repository repo(t.gone);
    CHECK(repo.writeLooseRef("refs/heads/x", ObjectId(kCommitA)));

    std::atomic<bool> go{false};
    std::atomic<int> ready{0};
    bool firstWon = false;
    bool secondWon = false;

    auto runOne = [&](const char* newOid, bool* answer) {
        Repository r(t.gone);  // a handle of its own: nothing is cached
        RefUpdate u;
        u.name = "refs/heads/x";
        u.newOid = ObjectId(newOid);
        u.expectedOldOid = ObjectId(kCommitA);
        ++ready;
        while (!go.load()) {
            std::this_thread::yield();
        }
        *answer = r.commitRefs({u});
    };

    std::thread first(runOne, kBlobE, &firstWon);
    std::thread second(runOne, kBlobF, &secondWon);
    while (ready.load() < 2) {
        std::this_thread::yield();
    }
    go.store(true);
    first.join();
    second.join();

    // Exactly one won. The other got the house's word — false, and it
    // stands for the answer either way this interleaving landed.
    CHECK(firstWon != secondWon);

    // The ref is the winner's truth: not the old value, not the loser's,
    // and not half of either.
    ObjectId out;
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/x", out)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(out.to_hex(), std::string(firstWon ? kBlobE : kBlobF));

    // Nothing survives the transaction: no lock file, no staging file of
    // any of its shapes, anywhere under the repository.
    CHECK(!is_file(t.p / "refs" / "heads" / "x.lock"));
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(t.p, ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().u8string();
        CHECK(name.find(".lock") == std::string::npos);
        CHECK(name.find(".new") == std::string::npos);
    }
}

// The crashed holder's half of the discipline, on the file's own age: a
// lock that stands inside any transaction's possible run is held — the
// holder is alive as far as the disk says, and the door is refused with
// the ref and the lock both untouched. A lock past the stale age is the
// abandoned file of a process that died holding it, and the next writer
// takes over, completes, and leaves nothing standing.
TEST(refs_transaction_lock_takes_over_a_dead_holder)
{
    TempDir t("stalelock");
    CHECK(initBare(t.gone, "main"));
    Repository repo(t.gone);
    CHECK(repo.writeLooseRef("refs/heads/x", ObjectId(kCommitA)));

    const std::filesystem::path lock = t.p / "refs" / "heads" / "x.lock";
    {
        std::ofstream f(lock, std::ios::binary);
        f << "held\n";
    }

    RefUpdate u;
    u.name = "refs/heads/x";
    u.newOid = ObjectId(kBlobE);
    u.expectedOldOid = ObjectId(kCommitA);

    // Young: held. The transaction says no and moves nothing.
    CHECK(!repo.commitRefs({u}));
    ObjectId out;
    CHECK_EQ(static_cast<int>(repo.resolveRef("refs/heads/x", out)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(out.to_hex(), std::string(kCommitA));
    CHECK(is_file(lock));  // the holder's file is none of this transaction's business

    // Aged past the stale age — no live transaction runs that long — the
    // holder is presumed dead, and the lock is taken over.
    std::error_code ec;
    std::filesystem::last_write_time(
        lock, std::filesystem::file_time_type::clock::now() - std::chrono::minutes(30), ec);
    CHECK(!ec);
    CHECK(repo.commitRefs({u}));
    CHECK(repo.resolveRef("refs/heads/x", out) == RefStatus::Ok);
    CHECK_EQ(out.to_hex(), std::string(kBlobE));
    CHECK(!is_file(lock));  // and the taken-over lock went out with the transaction
}

// The transaction stages at its own unique name, "<ref>.new.<pid>.<seq>":
// a plain "<ref>.new" standing on disk — left by a crash, or by git itself
// beside the studio, or by anything that does not hold the lock — is
// neither consumed by the transaction nor renamed into the ref. What a
// stranger planted is not what the move runs; the transaction plants its
// own bytes under its own name, moves those, and leaves nothing standing
// of its own.
TEST(refs_transaction_stages_under_its_own_unique_name)
{
    TempDir t("stagename");
    CHECK(initBare(t.gone, "main"));
    Repository repo(t.gone);
    CHECK(repo.writeLooseRef("refs/heads/x", ObjectId(kCommitA)));

    const std::filesystem::path planted = t.p / "refs" / "heads" / "x.new";
    {
        std::ofstream f(planted, std::ios::binary);
        f << "not the transaction's bytes\n";
    }

    RefUpdate u;
    u.name = "refs/heads/x";
    u.newOid = ObjectId(kBlobE);
    u.expectedOldOid = ObjectId(kCommitA);
    CHECK(repo.commitRefs({u}));

    ObjectId out;
    CHECK(repo.resolveRef("refs/heads/x", out) == RefStatus::Ok);
    CHECK_EQ(out.to_hex(), std::string(kBlobE));  // the transaction's own bytes moved
    CHECK_EQ(slurp(planted), "not the transaction's bytes\n");  // the planted file stayed planted

    // The transaction's staging is all gone: the only file wearing ".new"
    // anywhere is the one this case planted itself.
    int stagingFiles = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(t.p, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code tec;
        if (it->is_regular_file(tec) &&
            it->path().filename().u8string().find(".new") != std::string::npos) {
            ++stagingFiles;
        }
    }
    CHECK_EQ(stagingFiles, 1);
}

// readRawRef, at its own door rather than through resolveRef: the value is the
// file's bytes with only the whitespace around them taken off — a symref comes
// back as its raw "ref: <target>" text, not followed — a loose file is the
// answer over a packed line that says otherwise underneath, and a ref that is
// nowhere answers Missing with nothing handed back. A name no ref may wear is
// Bad before the disk is looked at.
TEST(refs_read_raw_ref_states_the_file_itself)
{
    TempDir t("rawref");
    CHECK(ols::gitcore::initBare(t.gone, "main"));
    Repository repo(t.gone);
    std::string value;

    // The loose answer, and the trim: the file carries its newline, a stray
    // carriage return, and a trailing space; the value is the hex alone.
    {
        std::ofstream f(t.p / "refs" / "heads" / "trimmed", std::ios::binary);
        f << " " << kCommitA << " \r\n";
    }
    CHECK_EQ(static_cast<int>(repo.readRawRef("refs/heads/trimmed", value)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(value, std::string(kCommitA));

    // Loose over packed, stated at the raw door: the packed file says one oid,
    // the loose file says another, and the value is the loose one — while the
    // packed line still stands underneath.
    std::vector<PackedRefEntry> packed = {
        {std::string(kCommitA), "refs/heads/master", false, std::string()},
    };
    CHECK(repo.writePackedRefs(packed));
    CHECK_EQ(static_cast<int>(repo.readRawRef("refs/heads/master", value)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(value, std::string(kCommitA));
    CHECK(repo.writeLooseRef("refs/heads/master", ObjectId(kBlobF)));
    CHECK_EQ(static_cast<int>(repo.readRawRef("refs/heads/master", value)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(value, std::string(kBlobF));  // the loose file's, not the packed
    CHECK(slurp(t.p / "packed-refs").find(kCommitA) != std::string::npos);

    // A symref is TEXT, not resolved: the value is the "ref: " line itself,
    // and the target behind it is none of this door's business.
    CHECK(repo.writeLooseRef("refs/heads/main", ObjectId(kTreeD)));
    CHECK(repo.writeLooseSymref("refs/heads/trunk", "refs/heads/main"));
    CHECK_EQ(static_cast<int>(repo.readRawRef("refs/heads/trunk", value)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(value, std::string("ref: refs/heads/main"));
    CHECK(ols::gitcore::looksLikeSymref(value));
    // HEAD, the symref initBare wrote, answers the same way.
    CHECK_EQ(static_cast<int>(repo.readRawRef("HEAD", value)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(value, std::string("ref: refs/heads/main"));

    // Missing is honest: no such ref, loose or packed, and nothing handed
    // back. The value the previous call answered with is not carried over.
    value.clear();
    CHECK_EQ(static_cast<int>(repo.readRawRef("refs/heads/never-was", value)),
             static_cast<int>(RefStatus::Missing));
    CHECK(value.empty());

    // A name no ref may wear is Bad before the disk is looked at.
    CHECK_EQ(static_cast<int>(repo.readRawRef("refs/heads/bad..name", value)),
             static_cast<int>(RefStatus::Bad));
}

// The writer of packed-refs, against the fixture git itself wrote: the file
// that comes out of the writer is the fixture's own bytes — git's header line,
// trailing space included, which the fixture carries and the writer states
// byte for byte — and a set with a peel comes back through the read with the
// peel standing, sorted by name as the writer promises.
TEST(refs_packed_refs_written_is_the_file_git_writes)
{
    const std::string sample = ols_test::read_fixture("packed-refs-sample.txt");
    CHECK(!sample.empty());
    std::vector<PackedRefEntry> one;
    CHECK(ols::gitcore::parsePackedRefs(sample, one));
    CHECK_EQ(one.size(), 1u);
    if (one.size() == 1) {
        CHECK_EQ(one[0].oid, std::string(kCommitA));
        CHECK_EQ(one[0].name, std::string("refs/heads/master"));
    }

    // The writer's answer for that one entry: git's header line, trailing
    // space included — the fixture's own first two lines, which the fixture
    // carries on with comment lines of its record that the writer does not
    // write.
    std::string text;
    CHECK(ols::gitcore::serializePackedRefs(one, text));
    CHECK_EQ(text,
             std::string("# pack-refs with: peeled fully-peeled sorted \n") +
             std::string(kCommitA) + " refs/heads/master\n");
    CHECK_EQ(sample.compare(0, text.size(), text), 0);  // the fixture begins with it

    TempDir t("packedwrite");
    CHECK(ols::gitcore::initBare(t.gone, "main"));
    Repository repo(t.gone);
    CHECK(repo.writePackedRefs(one));
    CHECK_EQ(slurp(t.p / "packed-refs"), text);  // what the writer states, restated

    // The shapes the fixture does not carry: a peel line, and the sort.
    std::vector<PackedRefEntry> two = {
        {std::string(kBlobE), "refs/tags/v1", true, std::string(kCommitA)},
        {std::string(kTreeD), "refs/heads/main", false, std::string()},
    };
    CHECK(repo.writePackedRefs(two));
    const std::string written = slurp(t.p / "packed-refs");
    CHECK_EQ(written,
             std::string("# pack-refs with: peeled fully-peeled sorted \n") +
             std::string(kTreeD) + " refs/heads/main\n" +
             std::string(kBlobE) + " refs/tags/v1\n" +
             "^" + std::string(kCommitA) + "\n");  // sorted by name, peel under the tag

    // And the read takes it back: the peel survives the round trip.
    std::vector<PackedRefEntry> back;
    CHECK(repo.readPackedRefs(back));
    CHECK_EQ(back.size(), 2u);
    if (back.size() == 2) {
        CHECK_EQ(back[0].name, std::string("refs/heads/main"));
        CHECK(!back[0].peeled);
        CHECK_EQ(back[1].name, std::string("refs/tags/v1"));
        CHECK(back[1].peeled);
        CHECK_EQ(back[1].peeledOid, std::string(kCommitA));
    }
    // The raw door sees the packed value with no loose file anywhere: the
    // same answer resolve would start from.
    std::string raw;
    CHECK_EQ(static_cast<int>(repo.readRawRef("refs/tags/v1", raw)),
             static_cast<int>(RefStatus::Ok));
    CHECK_EQ(raw, std::string(kBlobE));
}

// The reflog codec at its own door: the line is "<old> <new> <who> <when>"
// and then exactly one tab before the message — the tab is the format's
// discipline, not the message's — and a who or a msg carrying a tab or a
// newline is refused rather than folded in. A line with no tab is refused;
// a line whose message carries a second tab is refused; a line whose message
// ends at a carriage return reads back without it, which is what a log
// written on Windows reads as.
TEST(refs_reflog_codec_keeps_its_one_tab)
{
    ReflogEntry good;
    good.oldOid = std::string(40, '0');
    good.newOid = kCommitA;
    good.who = "Studio Bot <studio@local>";
    good.when = "1704067200 +0000";
    good.msg = "commit: first fixture commit";
    std::string line;
    CHECK(formatReflogLine(good, line));
    CHECK_EQ(line,
             std::string(40, '0') + " " + kCommitA +
             " Studio Bot <studio@local> 1704067200 +0000\tcommit: first fixture commit\n");
    // Exactly one tab, and it stands before the message.
    CHECK_EQ(line.find('\t'), line.size() - good.msg.size() - 2u);

    ReflogEntry back;
    // The line as the reader takes it: one line, ending at its newline.
    std::string whole = line;
    whole.pop_back();
    CHECK(parseReflogLine(whole, back));
    CHECK_EQ(back.oldOid, std::string(40, '0'));
    CHECK_EQ(back.newOid, std::string(kCommitA));
    CHECK_EQ(back.who, std::string("Studio Bot <studio@local>"));
    CHECK_EQ(back.when, std::string("1704067200 +0000"));
    CHECK_EQ(back.msg, std::string("commit: first fixture commit"));

    // The refusals of the writer, each on its own field.
    ReflogEntry tabInMsg = good;
    tabInMsg.msg = "two\tthings";
    std::string refused;
    CHECK(!formatReflogLine(tabInMsg, refused));
    ReflogEntry newlineInWho = good;
    newlineInWho.who = "Two\nLines <studio@local>";
    CHECK(!formatReflogLine(newlineInWho, refused));
    ReflogEntry emptyWho = good;
    emptyWho.who = "";
    CHECK(!formatReflogLine(emptyWho, refused));
    ReflogEntry emptyMsg = good;
    emptyMsg.msg = "";
    CHECK(!formatReflogLine(emptyMsg, refused));
    ReflogEntry notAnOid = good;
    notAnOid.newOid = "zz";
    CHECK(!formatReflogLine(notAnOid, refused));
    ReflogEntry notAStamp = good;
    notAStamp.when = "not a stamp";
    CHECK(!formatReflogLine(notAStamp, refused));

    // The refusals of the reader, on lines the writer could never have made.
    ReflogEntry read;
    CHECK(!parseReflogLine("no tab at all here", read));
    CHECK(!parseReflogLine(std::string(40, '0') + " " + kCommitA +
                               " Studio Bot <studio@local> 1704067200 +0000\ttwo\tthings",
                               read));
    CHECK(!parseReflogLine(std::string(40, '0') + " " + kCommitA +
                               " Studio Bot <studio@local> 1704067200 +0000\t",
                               read));  // an empty message is not a line
    // The carriage return at the end of a line is not part of the message.
    CHECK(parseReflogLine(std::string(40, '0') + " " + kCommitA +
                             " Studio Bot <studio@local> 1704067200 +0000\tcommit one\r",
                             read));
    CHECK_EQ(read.msg, std::string("commit one"));

    // The same line through the repo's door: appended, it stands on disk as
    // the codec's line, and reads back field for field.
    TempDir t("codec");
    CHECK(ols::gitcore::initBare(t.gone, "main"));
    Repository repo(t.gone);
    CHECK(repo.reflogAppend("refs/heads/main", good));
    CHECK_EQ(slurp(t.p / "logs" / "refs" / "heads" / "main"), line);
    std::vector<ReflogEntry> log;
    CHECK(repo.reflogRead("refs/heads/main", log));
    CHECK_EQ(log.size(), 1u);
    if (!log.empty()) {
        CHECK_EQ(log[0].msg, std::string("commit: first fixture commit"));
    }
    // A message carrying a tab never reaches the disk at all.
    CHECK(!repo.reflogAppend("refs/heads/main", tabInMsg));
    CHECK_EQ(slurp(t.p / "logs" / "refs" / "heads" / "main"), line);
}
