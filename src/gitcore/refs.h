// src/gitcore/refs.h — the refs and repository state of GitCore (spec §3.2).
//
// Loose refs, packed-refs, symrefs, reflogs, the repository layout of a bare
// studio repo, and the all-or-nothing ref transaction a receive needs. The
// ids move as lowercase hex strings; the oid itself is the object layer's, and
// this header includes that layer's ObjectId when it is in the tree, standing
// in with a minimal one until B1's objects.h lands.
//
// Failure is a return value, never a thrown exception: every filesystem call
// takes its error_code overload, the parses say no by returning false, and a
// bad name or a bad id is refused — never escaped into a shape that would
// happen to fit.

#ifndef OLS_GITCORE_REFS_H
#define OLS_GITCORE_REFS_H

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#if defined(__has_include)
#if __has_include("objects.h")
// B1's object layer is in the tree: GitCore's ObjectId is theirs, declared
// there, and this header asks it only for the names refs.cpp uses — default
// construction, .hex, parse(), valid(), is_null(), to_hex(), and the three
// comparisons. Wherever their shape is richer, that is the one place to
// reconcile, and refs.cpp is the only consumer. The include stands at file
// scope, not inside the namespaces below: an include from within ols::gitcore
// would nest whatever it declares (ols::gitcore::std among it) and poison
// every later std:: name in the translation unit.
#include "objects.h"
#define OLS_GITCORE_OID_IS_B1S 1
#endif
#endif

namespace ols
{
    namespace gitcore
    {

        // ---- names and ids, stated before the object id so the id can lean on them ----

        // A lowercase-or-uppercase hex string of exactly sha1 (40) or sha256 (64)
        // length. GitCore speaks both hash formats (spec §3).
        bool isValidOidHex(const std::string &text);

        // The rules a ref name must keep — git's rules, refuse-don't-escape: no "..",
        // no leading or trailing '/', no empty component, no ASCII control character
        // or space, none of "~^:?*[\", no ".lock" ending, no component that begins or
        // ends with '.', no lone "@", no "@{" sequence. Bytes at 0x80 and above pass
        // as utf8-ish printable, as git's own check leaves them.
        bool isValidRefName(const std::string &name);

        namespace detail
        {
            std::string lower_hex(std::string s);
            bool all_zero_hex(const std::string &s);
        } // namespace detail

#ifndef OLS_GITCORE_OID_IS_B1S
        // The stand-in: the smallest id refs.h can state its API with, declared so the
        // refs code builds today. When objects.h arrives, __has_include above finds it,
        // this block vanishes from the build, and nothing collides. The hex is held
        // lowercase; an empty hex is "no id", and a hex of all zeros is git's null.
        struct ObjectId
        {
            std::string hex;

            ObjectId() = default;
            explicit ObjectId(const std::string &hex_in) : hex(detail::lower_hex(hex_in)) {}

            // false unless the text is a well-formed oid hex; on true, out carries it
            // lowercased.
            static bool parse(const std::string &text, ObjectId &out)
            {
                if (!isValidOidHex(text))
                {
                    return false;
                }
                out.hex = detail::lower_hex(text);
                return true;
            }

            bool valid() const { return isValidOidHex(hex); }
            bool is_null() const { return hex.empty() || detail::all_zero_hex(hex); }
            const std::string &to_hex() const { return hex; }

            bool operator==(const ObjectId &o) const { return hex == o.hex; }
            bool operator!=(const ObjectId &o) const { return hex != o.hex; }
            bool operator<(const ObjectId &o) const { return hex < o.hex; }
        };
#endif

        // ---- the settled numbers ----

        // The symref hops resolveRef follows before it calls the chain a loop and
        // refuses. Ten is past the working depth of any honest symref; anything longer
        // is a cycle, and git caps its own following not much differently.
        constexpr int kResolveDepthCap = 10;

        // 5198 — the keep count git uses for reflog entries by default; it is git's
        // own number and GitCore inherits it. Configurable per Repository.
        constexpr std::size_t kDefaultReflogKeep = 5198;

        // ---- the small records ----

        // One line of packed-refs, with the "^oid" peel line of a tag folded into the
        // entry it peels, which is the shape of the file.
        struct PackedRefEntry
        {
            std::string oid; // lowercase hex
            std::string name;
            bool peeled = false;   // a "^oid" line followed it
            std::string peeledOid; // lowercase hex, when peeled
        };

        // One row of allRefs(): what a ref is and where it points. For a symref the
        // oid carries the resolution when the chain resolves and is empty when it
        // does not; the target is always the immediate target.
        struct RefInfo
        {
            std::string name;
            ObjectId oid;
            bool is_symref = false;
            std::string target;
        };

        // One reflog entry. who is "Name <email>"; when is "<epoch> <tz>", as
        // "1704067200 +0000". The oids are hex, and either may be the all-zero one.
        struct ReflogEntry
        {
            std::string oldOid;
            std::string newOid;
            std::string who;
            std::string when;
            std::string msg;
        };

        // One intended move of commitRefs(). An expectedOldOid that is not present
        // makes the update unconditional; a present one must match the ref's current
        // value, and the null oid as expectation states that the ref must not exist.
        struct RefUpdate
        {
            std::string name;
            ObjectId newOid;
            std::optional<ObjectId> expectedOldOid;
        };

        // What a read or a resolve found — or why it did not.
        enum class RefStatus
        {
            Ok,      // the ref is there and well formed
            Missing, // no such ref, loose or packed
            TooDeep, // a symref chain past the depth cap, which is how a loop shows up
            Bad      // there is something, but it is not a well-formed ref
        };

        const char *toString(RefStatus status);

        // ---- symref content, as it stands inside a ref file ----

        // A ref file's content is "<hex>\n" or "ref: <target>\n"; these read the
        // second shape. looksLikeSymref is the prefix test; symrefTarget takes the
        // target out and says whether it is a name worth following.
        bool looksLikeSymref(const std::string &content);
        bool symrefTarget(const std::string &content, std::string &target);

        // ---- packed-refs, as text in and text out ----

        // Every line of the file read: comments ("#" leads, git's own header line
        // among them) and blank lines pass, "oid name" lines become entries in the
        // order they stand, and a "^oid" line folds its peel into the entry before it.
        // A line that is neither is a file this parse refuses whole.
        bool parsePackedRefs(const std::string &text, std::vector<PackedRefEntry> &out);

        // The file as git would write it: git's header line, entries sorted by name,
        // a peel line under the entry it peels.
        bool serializePackedRefs(const std::vector<PackedRefEntry> &entries, std::string &out);

        // ---- reflog lines ----

        // "<old> <new> <who> <when>\t<msg>", one line, and back. A msg carrying a tab
        // or a newline is refused rather than folded into the format.
        bool formatReflogLine(const ReflogEntry &entry, std::string &line);
        bool parseReflogLine(const std::string &line, ReflogEntry &entry);

        // ---- the repository layout (spec §2) ----

        // The bare layout: objects/{info,pack}, refs/{heads,tags}, an empty hooks/
        // (the studio manages its own, so no .sample files), HEAD as a symref to the
        // default branch, a config saying bare with formatversion 0, and description.
        // The directory is made if it is not there; files it writes are replaced
        // atomically, so the call on an existing repo re-states its HEAD and config
        // rather than failing. default_branch must be a name fit for refs/heads/.
        // Verified against git.exe: fsck is clean on the result (see the task notes).
        bool initBare(const std::string &dir, const std::string &default_branch = "main");

        // ---- the repository, open ----

        // A thin handle on a repo directory. Cheap to make; every call reads what is
        // on disk now. Nothing is cached, so a repo another writer moved is seen moved.
        class Repository
        {
        public:
            explicit Repository(std::string dir);

            const std::string &dir() const { return dir_; }

            // The raw content of a ref — no symref following — loose winning over
            // packed, exactly as git reads. value comes back trimmed: "<hex>" or
            // "ref: <target>".
            RefStatus readRawRef(const std::string &name, std::string &value) const;

            // Follow symrefs to the oid underneath, at most depth_cap hops; a chain
            // past the cap (a cycle, in practice) is TooDeep, not an answer.
            RefStatus resolveRef(const std::string &name, ObjectId &out,
                                 int depth_cap = kResolveDepthCap) const;

            // Loose refs: hex file "<hex>\n" or symref file "ref: <target>\n", each
            // written through a ".new" temp and one replace, the way packed-refs is
            // written. delete removes the file and prunes the empty dirs it leaves,
            // like git; it is false when there was no loose file to remove.
            bool writeLooseRef(const std::string &name, const ObjectId &oid);
            bool writeLooseSymref(const std::string &name, const std::string &target);
            bool deleteLooseRef(const std::string &name);

            // packed-refs, whole file, both directions. writePackedRefs validates
            // before anything is touched and replaces the file atomically.
            bool readPackedRefs(std::vector<PackedRefEntry> &out) const;
            bool writePackedRefs(const std::vector<PackedRefEntry> &entries);

            // HEAD. headTarget answers only for a symref HEAD; readHead resolves
            // however HEAD stands — symref to a branch, or detached to an oid.
            bool isHeadSymref() const;
            bool headTarget(std::string &target) const;
            bool readHead(ObjectId &out) const;
            bool setHeadSymref(const std::string &target);
            bool detachHead(const ObjectId &oid);

            // Every ref under refs/ — loose over packed, loose walked from the disk —
            // plus HEAD, sorted by name. A symref row states its target and its oid
            // where the chain resolves. A repo whose packed-refs is junk is refused
            // whole, as honest as git's failure to list.
            bool allRefs(std::vector<RefInfo> &out) const;

            // The transaction: validate every update — names, ids, duplicates, every
            // expectedOldOid — before a byte moves. Then every temp is written, and
            // only when all are down do the replaces run; a replace that fails puts
            // back what each ref held before, and no ".new" file survives either way.
            // A name the update is given must not be a symref — this moves concrete
            // refs, and HEAD moves through its helpers. The null oid as a new value
            // is refused: a delete is not an update.
            bool commitRefs(const std::vector<RefUpdate> &updates);

            // Reflogs, at logs/<name> — logs/refs/heads/main for refs/heads/main,
            // logs/HEAD for HEAD. append is an honest append; prune applies the keep
            // policy (last N, default 5198, rewritten atomically) on request, which
            // mirrors git expiring logs at gc rather than on every entry. A missing
            // log reads back empty — no history is a fact, not a failure.
            void setReflogKeep(std::size_t keep) { reflog_keep_ = keep; }
            std::size_t reflogKeep() const { return reflog_keep_; }
            bool reflogAppend(const std::string &ref, const ReflogEntry &entry);
            bool reflogRead(const std::string &ref, std::vector<ReflogEntry> &out) const;
            bool reflogPrune(const std::string &ref);

        private:
            std::string loosePath(const std::string &name) const;
            std::string logPath(const std::string &name) const;
            // The packed file looked in, parsed fresh each call: the parse is cheap
            // against the read, and a cache here would be a lie to the second writer.
            RefStatus findInPacked(const std::string &name, std::string &oid_lower) const;

            std::string dir_;
            std::size_t reflog_keep_ = kDefaultReflogKeep;
        };

    } // namespace gitcore
} // namespace ols

#endif // OLS_GITCORE_REFS_H
