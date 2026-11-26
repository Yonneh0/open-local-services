// The read/browse engine behind the code doors: HEAD, refs, trees, blobs,
// history, blame, and the symbol outline of a blob. A repo dir is read
// through the gitcore layers exactly as git.exe itself writes it — loose
// objects, packfiles, packed-refs, and a HEAD symref — and this file adds
// nothing of its own: every call is independent, nothing is cached across
// calls, and a refused answer says false and names its reason in plain
// words. No throw crosses this edge.
#include "ProjectGit.h"
#include "Common.h"
#include "gitcore/diff.h"
// The road wire.cpp already travels in OpenLocalServices.cpp and
// GitSmartHttp.cpp: no build of this server names diff.cpp as a source,
// so the history row's line counts carry the diff's definitions in this
// file's own object, where the link reads exactly one copy. Should a
// build ever name diff.cpp as a source beside this one, the doubled
// definitions will fail the link in plain words — the fix will be to
// drop this line then.

#include "gitcore/objects.h"
#include "gitcore/refs.h"
#include "gitcore/packfile.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ols
{
    namespace project
    {

        using json = nlohmann::json;

        // ---- this file's last-error channel ----
        //
        // The shape of ProjectService.cpp's gLastError and its noteError/clearNote
        // pair, its own here — and one per thread for the same reason that file's
        // stands per thread: the call that was refused and the lastError read
        // beside it run on one connection thread, and a lock cannot make that pair
        // one. A mutex around a shared note keeps every write whole by itself and
        // still lets one thread answer with another's words; the per-thread note is
        // the thing that holds, and the threads share nothing that needs a lock.
        static thread_local std::string gLastError;

        static void noteError(const std::string &words)
        {
            gLastError = words;
        }

        static void clearNote()
        {
            gLastError.clear();
        }

        std::string lastError()
        {
            return gLastError;
        }

        // ---- the cost bounds ----
        //
        // historyFor follows every parent of every walked commit from its start,
        // each commit once, and stops at this many visits; a walk that stops with
        // history still queued says so in lastError and still answers true.
        static constexpr std::size_t kHistoryWalkCap = 2000u;

        // blameFile walks this many commits from the file's start at most. Lines
        // older than the walk stay unattributed and carry the truncated flag.
        static constexpr std::size_t kBlameWalkCap = 200u;

        // A history row's changed list holds at most this many files; the walk
        // collects to one past the bound so it can say the list was cut.
        static constexpr std::size_t kHistoryChangedCap = 200u;

        // Lines are counted only in blobs up to this size; a bigger file is
        // still named as changed, without counts.
        static constexpr std::size_t kHistoryBlobCap = 2u * 1024u * 1024u;

        // A history row's message holds at most this many bytes of the commit
        // message, cut at a whole character boundary.
        static constexpr std::size_t kHistoryMessageCap = 4096u;

        // The bytes of a blob sniffed for a NUL to call it binary — the same
        // reading the file door gives its bytes.
        static constexpr std::size_t kHistoryBinarySniff = 8192u;

        // ---- the shape rules ----

        // The '/'-joined path cut into its parts.
        static void splitPath(const std::string &path, std::vector<std::string> &parts)
        {
            parts.clear();
            std::string::size_type at = 0u;
            while (at < path.size())
            {
                const std::string::size_type slash = path.find('/', at);
                if (slash == std::string::npos)
                {
                    parts.push_back(path.substr(at));
                    return;
                }
                parts.push_back(path.substr(at, slash - at));
                at = slash + 1u;
            }
        }

        // A tree path this file answers: repo-relative, '/'-joined, never absolute,
        // never empty-named, never wearing a "." or ".." segment and never wearing
        // a backslash or a colon. The empty path is the root, allowed where the
        // caller says so.
        static bool pathOk(const std::string &path, std::string &why)
        {
            why = "the path must stay inside the repo: no absolute paths, no '..' segments";
            if (path.empty())
            {
                return true;
            }
            if (path[0] == '/')
            {
                return false;
            }
            for (const char c : path)
            {
                if (c == '\\' || c == ':')
                {
                    return false;
                }
                if (c == '\0')
                {
                    why = "a path carries no zero byte";
                    return false;
                }
            }
            std::vector<std::string> parts;
            splitPath(path, parts);
            for (const std::string &part : parts)
            {
                if (part.empty() || part == "." || part == "..")
                {
                    return false;
                }
                // The repo's own folder, whatever case it wears: a tree entry named
                // like it is how a later read - this file's own or git's - is fooled
                // about what stands where.
                bool wearsGit = part.size() == 4u && part[0] == '.';
                if (wearsGit)
                {
                    const char gitWord[3] = {'g', 'i', 't'};
                    for (std::size_t k = 0u; k < 3u && wearsGit; ++k)
                    {
                        char c = part[k + 1u];
                        if (c >= 'A' && c <= 'Z')
                        {
                            c = static_cast<char>(c - 'A' + 'a');
                        }
                        if (c != gitWord[k])
                        {
                            wearsGit = false;
                        }
                    }
                }
                if (wearsGit)
                {
                    why = "a path may not wear the repo's own .git name";
                    return false;
                }
            }
            return true;
        }

        // An id as this file moves it: lowercase hex of one of the two widths, or
        // nothing when the text is not one.
        static std::string normHex(const std::string &text)
        {
            ols::gitcore::ObjectId id;
            if (!ols::gitcore::ObjectId::parse(text, id))
            {
                return std::string();
            }
            return id.to_hex();
        }

        static std::string firstLine(const std::string &text)
        {
            const std::string::size_type nl = text.find('\n');
            if (nl == std::string::npos)
            {
                return text;
            }
            return text.substr(0, nl);
        }

        static std::vector<std::string> splitLines(const std::string &bytes)
        {
            std::vector<std::string> out;
            std::string cur;
            for (const char c : bytes)
            {
                if (c == '\n')
                {
                    out.push_back(cur);
                    cur.clear();
                    continue;
                }
                cur.push_back(c);
            }
            if (!cur.empty())
            {
                out.push_back(cur);
            }
            return out;
        }

        // ---- the per-call reader ----

        // One call's reader: the repo dir, its packfiles opened on the first miss,
        // and the raw object bytes it has read. Nothing about it lives past the
        // call.
        struct Reader
        {
            std::string repoDir;
            std::vector<std::pair<std::string, std::string>> packPairs; // .pack and .idx
            std::vector<std::unique_ptr<ols::gitcore::Packfile>> packs; // opened on demand
            std::unordered_map<std::string, std::string> raws;          // oid hex -> raw object
            bool packed = false;
        };

        // Whether an error from the pack-folder walk says the folder simply is not
        // there to walk — the repo has never packed, or a path that is not a folder
        // stands where the folder would be, which holds no packs — as against a real
        // read failure the walk cannot see through. Only the first is "no packs".
        static bool packFolderAbsent(const std::error_code &ec)
        {
            if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory)
            {
                return true;
            }
#ifdef _WIN32
            // Windows answers a walk over a path that is not a folder with
            // ERROR_DIRECTORY (267), which its system category does not tie to
            // ENOTDIR. It is the same fact — no folder there to walk — so the
            // classification states it alike on both platforms the release gate reads.
            return ec.category() == std::system_category() && ec.value() == 267;
#else
            return false;
#endif
        }

        // The packfiles the repo holds, sorted by name so the walk is the same
        // walk every time. A .pack with no .idx beside it is left out: nothing
        // here can answer from it. False — naming the failure in why — when the
        // folder or one of its entries could not be read at all: that answer
        // refuses the call rather than folding a real error into "no packs" and a
        // downstream "object not found".
        static bool discoverPacks(Reader &rd, std::string &why)
        {
            std::vector<std::string> found;
            std::error_code ec{};
            const std::filesystem::path packDir{rd.repoDir + "/objects/pack"};
            // A repo that has never packed has no objects/pack folder at all, and a
            // file standing where the folder would be is no folder of packs either:
            // the walk is asked with the error code, so both are an empty listing
            // and not a throw that would end the serving thread. Anything else the
            // walk answers with is a failure to read, and is named as one.
            std::filesystem::directory_iterator it{packDir, ec};
            if (ec && !packFolderAbsent(ec))
            {
                why = "the pack folder could not be read: " + ec.message();
                return false;
            }
            if (!ec)
            {
                for (const std::filesystem::directory_iterator end{}; it != end; it.increment(ec))
                {
                    if (ec)
                    {
                        why = "the pack folder could not be read: " + ec.message();
                        return false;
                    }
                    std::error_code entryEc{};
                    if (it->is_directory(entryEc))
                    {
                        continue;
                    }
                    if (entryEc)
                    {
                        // The entry's type cannot be stated, so it does not proceed
                        // as a pack file. One that is simply gone was never one to
                        // read; any other failure is a failure to read the folder.
                        if (entryEc != std::errc::no_such_file_or_directory)
                        {
                            why = "the pack folder could not be read: " + entryEc.message();
                            return false;
                        }
                        continue;
                    }
                    const std::filesystem::path full = it->path();
                    if (full.filename().extension() != ".pack")
                    {
                        continue;
                    }
                    found.push_back(full.u8string());
                }
            }
            std::sort(found.begin(), found.end());
            for (const std::string &pack : found)
            {
                const std::string::size_type dot = pack.rfind(".pack");
                if (dot == std::string::npos)
                {
                    continue;
                }
                const std::string idx = pack.substr(0, dot) + ".idx";
                std::error_code missing{};
                const bool idxThere = std::filesystem::is_regular_file(idx, missing);
                // No .idx beside the .pack leaves the pack out — including the error
                // code's own "it is not there". But a .idx whose state the call
                // cannot read is a folder it cannot read, and that is named.
                if (!idxThere && missing && !packFolderAbsent(missing))
                {
                    why = "the pack folder could not be read: " + missing.message();
                    return false;
                }
                if (idxThere)
                {
                    rd.packPairs.emplace_back(pack, idx);
                }
            }
            return true;
        }

        // The raw bytes of an object — header through content — loose first, then
        // the packs opened on the way, and cached for the call.
        static bool findRaw(Reader &rd, const std::string &oidHex, std::string &raw, std::string &why)
        {
            const auto hit = rd.raws.find(oidHex);
            if (hit != rd.raws.end())
            {
                raw = hit->second;
                return true;
            }
            ols::gitcore::ObjectId id;
            if (!ols::gitcore::ObjectId::parse(oidHex, id))
            {
                why = "not a valid object id";
                return false;
            }
            const std::string hex = id.to_hex();
            ols::gitcore::LooseObject lo;
            std::string err;
            if (ols::gitcore::looseObjectReadAt(rd.repoDir + "/objects", id, lo, err))
            {
                rd.raws[hex] = lo.raw;
                raw = lo.raw;
                return true;
            }
            if (!rd.packed)
            {
                // A pack folder the call cannot read is a refusal, not an absence:
                // the words travel the same why-channel as any other and come back
                // through lastError, never folded into "object not found".
                if (!discoverPacks(rd, why))
                {
                    return false;
                }
                rd.packed = true;
            }
            for (std::size_t at = rd.packs.size(); at < rd.packPairs.size(); ++at)
            {
                std::string packBytes, idxBytes;
                if (!ols::readFileText(rd.packPairs[at].first, packBytes))
                {
                    continue;
                }
                if (!ols::readFileText(rd.packPairs[at].second, idxBytes))
                {
                    continue;
                }
                const std::vector<std::uint8_t> packRaw(packBytes.begin(), packBytes.end());
                const std::vector<std::uint8_t> idxRaw(idxBytes.begin(), idxBytes.end());
                auto pack = std::make_unique<ols::gitcore::Packfile>();
                if (!pack->open(packRaw, idxRaw))
                {
                    continue;
                }
                rd.packs.push_back(std::move(pack));
            }
            for (const auto &pack : rd.packs)
            {
                ols::gitcore::PackObject po;
                if (!pack->lookup(id, po))
                {
                    continue;
                }
                std::vector<std::uint8_t> bytes;
                if (!pack->resolveOid(id, bytes))
                {
                    continue;
                }
                raw.assign(bytes.begin(), bytes.end());
                rd.raws[hex] = raw;
                return true;
            }
            why = "object not found in the repo";
            return false;
        }

        // The object of an id read as one of the three structured types.
        static bool readCommit(Reader &rd, const std::string &oid, ols::gitcore::CommitObject &out, std::string &why)
        {
            std::string raw;
            if (!findRaw(rd, oid, raw, why))
            {
                return false;
            }
            ols::gitcore::ObjectKind kind{};
            std::uint64_t size = 0u;
            std::string content, err;
            if (!ols::gitcore::objectSplit(raw, kind, size, content, err))
            {
                why = "the object bytes do not hold an object";
                return false;
            }
            if (kind != ols::gitcore::ObjectKind::Commit)
            {
                why = "the object is not a commit";
                return false;
            }
            if (!ols::gitcore::commitParse(content, out, err))
            {
                why = "the commit is not in a form this reader takes";
                return false;
            }
            return true;
        }

        static bool readTree(Reader &rd, const std::string &oid, ols::gitcore::TreeObject &out, std::string &why)
        {
            std::string raw;
            if (!findRaw(rd, oid, raw, why))
            {
                return false;
            }
            ols::gitcore::ObjectKind kind{};
            std::uint64_t size = 0u;
            std::string content, err;
            if (!ols::gitcore::objectSplit(raw, kind, size, content, err))
            {
                why = "the object bytes do not hold an object";
                return false;
            }
            if (kind != ols::gitcore::ObjectKind::Tree)
            {
                why = "the object is not a tree";
                return false;
            }
            if (!ols::gitcore::treeParse(content, out, err))
            {
                why = "the tree is not in a form this reader takes";
                return false;
            }
            return true;
        }

        static bool blobContent(Reader &rd, const std::string &oid, std::string &content)
        {
            std::string raw, why;
            if (!findRaw(rd, oid, raw, why))
            {
                return false;
            }
            ols::gitcore::ObjectKind kind{};
            std::uint64_t size = 0u;
            std::string err;
            if (!ols::gitcore::objectSplit(raw, kind, size, content, err))
            {
                return false;
            }
            return kind == ols::gitcore::ObjectKind::Blob;
        }

        // The entry a path names in a tree: 1 with its oid out, 0 when the tree
        // does not hold it, -1 when an object along the way could not be read.
        static int entryInTree(Reader &rd, const std::string &treeOid, const std::string &path, std::string &oidOut)
        {
            std::vector<std::string> parts;
            splitPath(path, parts);
            if (parts.empty())
            {
                return 0;
            }
            std::string cur = normHex(treeOid);
            for (std::size_t at = 0u; at < parts.size(); ++at)
            {
                ols::gitcore::TreeObject tree;
                std::string why;
                if (!readTree(rd, cur, tree, why))
                {
                    return -1;
                }
                bool found = false;
                std::string next;
                for (const ols::gitcore::TreeEntry &te : tree.entries)
                {
                    if (te.name != parts[at])
                    {
                        continue;
                    }
                    const bool dirHere = ols::gitcore::treeModeDirectory(te.mode);
                    if (at + 1u < parts.size() && !dirHere)
                    {
                        continue;
                    }
                    found = true;
                    next = te.id.to_hex();
                    break;
                }
                if (!found)
                {
                    return 0;
                }
                cur = next;
            }
            oidOut = cur;
            return 1;
        }

        // The bytes of a file at a commit, with the same three answers as entryInTree.
        static int fileAtCommit(Reader &rd, const std::string &commitOid, const std::string &path, std::string &contentOut)
        {
            ols::gitcore::CommitObject co;
            std::string why;
            if (!readCommit(rd, commitOid, co, why))
            {
                return -1;
            }
            std::string oidOut;
            const int st = entryInTree(rd, co.tree.to_hex(), path, oidOut);
            if (st != 1)
            {
                return st;
            }
            if (!blobContent(rd, oidOut, contentOut))
            {
                return -1;
            }
            return 1;
        }

        // Whether this commit changed the path: its entry state (present with an
        // oid, or absent) differs from every parent's. A root that holds the path
        // changed it; a root that does not, did not.
        static bool changedHere(Reader &rd, const ols::gitcore::CommitObject &co, const std::string &path, bool &changed)
        {
            std::string selfOid;
            const int self = entryInTree(rd, co.tree.to_hex(), path, selfOid);
            if (self < 0)
            {
                return false;
            }
            bool differsFromEvery = true;
            if (co.parents.empty())
            {
                differsFromEvery = (self == 1);
            }
            for (const ols::gitcore::ObjectId &p : co.parents)
            {
                ols::gitcore::CommitObject pc;
                std::string why;
                if (!readCommit(rd, p.to_hex(), pc, why))
                {
                    return false;
                }
                std::string parentOid;
                const int st = entryInTree(rd, pc.tree.to_hex(), path, parentOid);
                if (st < 0)
                {
                    return false;
                }
                const bool same = (st == self) && (self == 0 || parentOid == selfOid);
                if (same)
                {
                    differsFromEvery = false;
                    break;
                }
            }
            changed = differsFromEvery;
            return true;
        }

        // ---- the first-parent diff of a history row ----

        static std::string asciiLower(const std::string &text)
        {
            std::string out;
            out.reserve(text.size());
            for (const char c : text)
            {
                out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
            }
            return out;
        }

        // One path where two trees differ: the blob oid each side holds, empty
        // when the side is absent or stands there with something that is not a
        // blob (a directory where the other side has a file).
        struct PathChange
        {
            std::string path;
            std::string aOid;
            std::string bOid;
        };

        // Every path where two trees differ, paired by name, directories
        // descended in step; entries are taken in name order so the list is
        // deterministic. An entry both sides hold with the same oid and the same
        // mode is never a change. The collector holds cap+1 so the caller can
        // tell "exactly at the bound" from "cut by it"; once cut, every level
        // stops at once. A tree that cannot be read is a hard no with its words.
        static bool collectTreeDiff(Reader &rd, const std::string &aTree, const std::string &bTree,
                                    const std::string &prefix, std::vector<PathChange> &out,
                                    std::size_t cap, bool &capped, std::string &why)
        {
            if (capped)
            {
                return true;
            }
            ols::gitcore::TreeObject at;
            ols::gitcore::TreeObject bt;
            if (!aTree.empty() && !readTree(rd, aTree, at, why))
            {
                return false;
            }
            if (!bTree.empty() && !readTree(rd, bTree, bt, why))
            {
                return false;
            }
            auto byName = [](const ols::gitcore::TreeEntry *x, const ols::gitcore::TreeEntry *y)
            {
                return x->name < y->name;
            };
            std::vector<const ols::gitcore::TreeEntry *> ea;
            std::vector<const ols::gitcore::TreeEntry *> eb;
            ea.reserve(at.entries.size());
            eb.reserve(bt.entries.size());
            for (const ols::gitcore::TreeEntry &te : at.entries)
            {
                ea.push_back(&te);
            }
            for (const ols::gitcore::TreeEntry &te : bt.entries)
            {
                eb.push_back(&te);
            }
            std::sort(ea.begin(), ea.end(), byName);
            std::sort(eb.begin(), eb.end(), byName);
            std::size_t i = 0u;
            std::size_t j = 0u;
            while (i < ea.size() || j < eb.size())
            {
                const bool takeA = j >= eb.size() || (i < ea.size() && ea[i]->name < eb[j]->name);
                const std::string name = takeA ? ea[i]->name : eb[j]->name;
                const ols::gitcore::TreeEntry *a = nullptr;
                const ols::gitcore::TreeEntry *b = nullptr;
                if (i < ea.size() && ea[i]->name == name)
                {
                    a = ea[i];
                    ++i;
                }
                if (j < eb.size() && eb[j]->name == name)
                {
                    b = eb[j];
                    ++j;
                }
                if (a && b && a->id == b->id && a->mode == b->mode)
                {
                    continue;
                }
                const bool aDir = a && ols::gitcore::treeModeDirectory(a->mode);
                const bool bDir = b && ols::gitcore::treeModeDirectory(b->mode);
                if (aDir && bDir)
                {
                    if (!collectTreeDiff(rd, a->id.to_hex(), b->id.to_hex(), prefix + name + "/", out,
                                         cap, capped, why))
                    {
                        return false;
                    }
                    if (capped)
                    {
                        return true;
                    }
                    continue;
                }
                if (out.size() > cap)
                {
                    capped = true;
                    return true;
                }
                PathChange pc;
                pc.path = prefix + name;
                if (a && !aDir)
                {
                    pc.aOid = a->id.to_hex();
                }
                if (b && !bDir)
                {
                    pc.bOid = b->id.to_hex();
                }
                out.push_back(pc);
            }
            return true;
        }

        // The counts of one changed path: both sides read as blobs when they are
        // blobs and small enough and the line diff run over them; the honest mark
        // taken when they are not. An absent side reads as no bytes, which is
        // what makes an add or a delete count its lines whole. A blob the read
        // leaves in the call's cache past a size worth keeping is let go — a
        // walk that counts many files does not hold them all. The answer comes
        // back true when the diff itself ran past its cell budget: an approximate
        // answer still describes the pair, but its hunks count more change than
        // the change holds, so no numbers are claimed from one — the path is
        // named as changed, without counts, exactly as a file past the size bound
        // is, and the walk's tail says the budget is what cut it.
        static bool countChange(Reader &rd, const PathChange &pc, ChangeRow &row)
        {
            const auto dropBig = [&rd](const std::string &oid)
            {
                const auto hit = rd.raws.find(oid);
                if (hit != rd.raws.end() && hit->second.size() > 65536u)
                {
                    rd.raws.erase(hit);
                }
            };
            std::string a;
            std::string b;
            if (!pc.aOid.empty() && !blobContent(rd, pc.aOid, a))
            {
                dropBig(pc.aOid);
                return false;
            }
            if (!pc.bOid.empty() && !blobContent(rd, pc.bOid, b))
            {
                dropBig(pc.aOid);
                dropBig(pc.bOid);
                return false;
            }
            if (a.size() > kHistoryBlobCap || b.size() > kHistoryBlobCap)
            {
                row.large = true;
                dropBig(pc.aOid);
                dropBig(pc.bOid);
                return false;
            }
            const std::size_t sniffA = a.size() < kHistoryBinarySniff ? a.size() : kHistoryBinarySniff;
            const std::size_t sniffB = b.size() < kHistoryBinarySniff ? b.size() : kHistoryBinarySniff;
            for (std::size_t k = 0u; k < sniffA; ++k)
            {
                if (a[k] == 0)
                {
                    row.binary = true;
                    break;
                }
            }
            if (!row.binary)
            {
                for (std::size_t k = 0u; k < sniffB; ++k)
                {
                    if (b[k] == 0)
                    {
                        row.binary = true;
                        break;
                    }
                }
            }
            if (row.binary)
            {
                dropBig(pc.aOid);
                dropBig(pc.bOid);
                return false;
            }
            const ols::gitcore::DiffResult d = ols::gitcore::diff_blobs(a, b);
            if (d.approx)
            {
                dropBig(pc.aOid);
                dropBig(pc.bOid);
                return true; // the budget cut the exact search: no counts claimed
            }
            for (const ols::gitcore::Hunk &h : d.hunks)
            {
                for (const ols::gitcore::HunkLine &hl : h.lines)
                {
                    if (hl.kind == 'a')
                    {
                        ++row.added;
                    }
                    else if (hl.kind == 'd')
                    {
                        ++row.deleted;
                    }
                }
            }
            row.counted = true;
            dropBig(pc.aOid);
            dropBig(pc.bOid);
            return false;
        }

        // The row's identity as the walk read it: parents, both hands and their
        // times, the decorated refs standing at this commit, and the message cut
        // at the bound with its boundary walked back to a whole character.
        static void historyRow(const std::string &oid, const ols::gitcore::CommitObject &co,
                               const std::unordered_map<std::string, std::vector<std::string>> &refMap,
                               CommitInfo &row)
        {
            row.oid = oid;
            for (const ols::gitcore::ObjectId &p : co.parents)
            {
                row.parents.push_back(p.to_hex());
            }
            row.authorName = co.author.name;
            row.authorWhen = std::to_string(co.author.seconds) + " " + co.author.zone;
            row.committerName = co.committer.name;
            row.committerWhen = std::to_string(co.committer.seconds) + " " + co.committer.zone;
            row.messageFirstLine = firstLine(co.message);
            std::string text = co.message;
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            {
                text.pop_back();
            }
            if (text.size() > kHistoryMessageCap)
            {
                std::size_t cut = kHistoryMessageCap;
                while (cut > 0u && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u)
                {
                    --cut;
                }
                text.resize(cut);
                row.messageTruncated = true;
            }
            row.message = text;
            const auto hit = refMap.find(oid);
            if (hit != refMap.end())
            {
                row.refs = hit->second;
            }
        }

        // The row's first-parent diff: changed[] to the bound with its counts,
        // and the totals over the counted files. A root commit diffs against
        // nothing and counts its whole tree as added. anyLarge records that some
        // file somewhere gave up its counts at the size bound; anyApprox records
        // that some diff somewhere ran past its cell budget and gave up its
        // counts the same way — named as changed, with no numbers stated.
        static bool addStats(Reader &rd, const ols::gitcore::CommitObject &co, CommitInfo &row,
                             bool &anyLarge, bool &anyApprox, std::string &why)
        {
            std::string aTree;
            if (!co.parents.empty())
            {
                ols::gitcore::CommitObject pc;
                if (!readCommit(rd, co.parents[0].to_hex(), pc, why))
                {
                    return false;
                }
                aTree = pc.tree.to_hex();
            }
            std::vector<PathChange> pcs;
            bool capped = false;
            if (!collectTreeDiff(rd, aTree, co.tree.to_hex(), "", pcs, kHistoryChangedCap, capped, why))
            {
                return false;
            }
            if (capped)
            {
                pcs.resize(kHistoryChangedCap);
                row.changedTruncated = true;
            }
            for (const PathChange &pc : pcs)
            {
                ChangeRow cr;
                cr.path = pc.path;
                if (countChange(rd, pc, cr))
                {
                    anyApprox = true;
                }
                if (cr.large)
                {
                    anyLarge = true;
                }
                if (cr.counted)
                {
                    row.added += cr.added;
                    row.deleted += cr.deleted;
                }
                row.changed.push_back(std::move(cr));
            }
            row.files = static_cast<long long>(row.changed.size());
            return true;
        }

        // The refs standing at each commit, in short decorated form the way git
        // decorates: "HEAD -> main" for the branch HEAD wears when it stands
        // here, "tag: v1" for tags, short names for branches and remotes, and a
        // plain "HEAD" when HEAD itself is an oid resting on the commit. One
        // label per ref, in listRefs' own name order.
        static void buildTipRefMap(const std::string &repoDir,
                                   std::unordered_map<std::string, std::vector<std::string>> &refMap,
                                   bool &ok)
        {
            ok = true;
            std::vector<RefInfo> refs;
            if (!listRefs(repoDir, refs))
            {
                ok = false;
                return;
            }
            std::string headTarget;
            std::string headOid;
            for (const RefInfo &r : refs)
            {
                if (r.name == "HEAD")
                {
                    headTarget = r.target;
                    headOid = r.oid;
                }
            }
            for (const RefInfo &r : refs)
            {
                if (r.name == "HEAD" || r.oid.empty())
                {
                    continue;
                }
                std::string label;
                if (r.name.compare(0, 10, "refs/tags/") == 0)
                {
                    label = "tag: " + r.name.substr(10);
                }
                else if (r.name.compare(0, 11, "refs/heads/") == 0)
                {
                    label = r.name.substr(11);
                    if (!headTarget.empty() && headTarget == r.name)
                    {
                        label = "HEAD -> " + label;
                    }
                }
                else if (r.name.compare(0, 13, "refs/remotes/") == 0)
                {
                    label = r.name.substr(13);
                }
                else
                {
                    label = r.name;
                }
                refMap[r.oid].push_back(label);
            }
            if (headTarget.empty() && !headOid.empty())
            {
                refMap[headOid].push_back("HEAD");
            }
        }

        // ---- the small words of the scanners ----

        static bool isIdent(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '$';
        }

        static std::string trim(const std::string &text)
        {
            std::string::size_type b = 0u;
            while (b < text.size() && (text[b] == ' ' || text[b] == '\t' || text[b] == '\r'))
            {
                ++b;
            }
            std::string::size_type e = text.size();
            while (e > b && (text[e - 1u] == ' ' || text[e - 1u] == '\t' || text[e - 1u] == '\r'))
            {
                --e;
            }
            return text.substr(b, e - b);
        }

        static std::string firstWord(const std::string &text)
        {
            std::string::size_type e = 0u;
            while (e < text.size() && isIdent(text[e]))
            {
                ++e;
            }
            return text.substr(0, e);
        }

        // The first ident token, wherever it starts.
        static std::string nextWord(const std::string &text)
        {
            std::string::size_type b = 0u;
            while (b < text.size() && !isIdent(text[b]))
            {
                ++b;
            }
            std::string::size_type e = b;
            while (e < text.size() && isIdent(text[e]))
            {
                ++e;
            }
            if (b == e)
            {
                return std::string();
            }
            return text.substr(b, e - b);
        }

        // The ident ending just before a position, with spaces let through.
        static bool identBefore(const std::string &text, std::string::size_type at, std::string &name)
        {
            std::string::size_type e = at;
            while (e > 0u && (text[e - 1u] == ' ' || text[e - 1u] == '\t'))
            {
                --e;
            }
            if (e == 0u)
            {
                return false;
            }
            std::string::size_type b = e;
            while (b > 0u && isIdent(text[b - 1u]))
            {
                --b;
            }
            if (b == e)
            {
                return false;
            }
            name = text.substr(b, e - b);
            return true;
        }

        // Whether the line opens with a word.
        static bool wordStart(const std::string &text, const char *word)
        {
            const std::string w{word};
            if (text.size() <= w.size())
            {
                return false;
            }
            if (text.compare(0, w.size(), w) != 0)
            {
                return false;
            }
            return !isIdent(text[w.size()]);
        }

        static std::string afterWord(const std::string &text, const char *word)
        {
            return trim(text.substr(std::string(word).size()));
        }

        static bool isControlWord(const std::string &w)
        {
            return w == "if" || w == "for" || w == "while" || w == "switch" || w == "catch" ||
                   w == "return" || w == "sizeof" || w == "else" || w == "do" || w == "not";
        }

        // The language a file name's extension names, or text when none does.
        static std::string langForName(const std::string &name)
        {
            std::string low;
            for (const char c : name)
            {
                low.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
            }
            const std::string::size_type dot = low.rfind('.');
            if (dot == std::string::npos)
            {
                return "text";
            }
            const std::string ext = low.substr(dot + 1u);
            if (ext == "c" || ext == "h" || ext == "cc" || ext == "cpp" || ext == "cxx" || ext == "hpp" ||
                ext == "hh" || ext == "hxx")
            {
                return "cpp";
            }
            if (ext == "js" || ext == "mjs" || ext == "cjs" || ext == "jsx" || ext == "ts" || ext == "tsx")
            {
                return "js";
            }
            if (ext == "py")
            {
                return "py";
            }
            if (ext == "rs")
            {
                return "rs";
            }
            if (ext == "go")
            {
                return "go";
            }
            if (ext == "sh" || ext == "bash" || ext == "ksh" || ext == "zsh")
            {
                return "sh";
            }
            if (ext == "md" || ext == "markdown")
            {
                return "md";
            }
            return "text";
        }

        // A symbol row, kept to a name short enough to be worth showing.
        static void addSymbol(json &symbols, const std::string &name, const std::string &kind, std::size_t line)
        {
            if (name.empty())
            {
                return;
            }
            std::string kept = trim(name);
            if (kept.empty())
            {
                return;
            }
            if (kept.size() > 120u)
            {
                kept.resize(120u);
            }
            symbols.push_back(json{{"name", kept},
                                   {"kind", kind},
                                   {"line", static_cast<long long>(line)}});
        }

        // The code of a line with comments and the words inside strings taken out;
        // inBlock carries an unterminated block comment across the lines.
        static std::string codeOfLine(const std::string &line, bool &inBlock)
        {
            std::string out;
            std::string::size_type at = 0u;
            char quote = 0;
            while (at < line.size())
            {
                const char c = line[at];
                if (inBlock)
                {
                    if (c == '*' && at + 1u < line.size() && line[at + 1u] == '/')
                    {
                        inBlock = false;
                        at += 2u;
                    }
                    else
                    {
                        ++at;
                    }
                    continue;
                }
                if (c == '/' && at + 1u < line.size() && line[at + 1u] == '*')
                {
                    inBlock = true;
                    at += 2u;
                    continue;
                }
                if (c == '/' && at + 1u < line.size() && line[at + 1u] == '/')
                {
                    break;
                }
                if (quote != 0)
                {
                    if (c == '\\')
                    {
                        at += 2u;
                        continue;
                    }
                    if (c == quote)
                    {
                        quote = 0;
                    }
                    ++at;
                    continue;
                }
                if (c == '"' || c == '\'')
                {
                    quote = c;
                    out.push_back(c);
                    ++at;
                    continue;
                }
                out.push_back(c);
                ++at;
            }
            return out;
        }

        // ---- the scanners, deliberately simple ----

        // C/C++: definitions anchored where their body opens, read at top-level
        // only; what sits inside a body belongs to it and is not listed.
        static void scanCpp(const std::vector<std::string> &lines, json &symbols)
        {
            bool inBlock = false;
            int depth = 0;
            bool pendActive = false;
            std::string pendKind, pendName;
            std::size_t pendLine = 0u;
            int pendWait = 0;
            for (std::size_t i = 0u; i < lines.size(); ++i)
            {
                const std::string code = codeOfLine(lines[i], inBlock);
                const std::string t = trim(code);
                const bool hasBrace = code.find('{') != std::string::npos;
                const bool hasSemicolon = code.find(';') != std::string::npos;
                if (pendActive)
                {
                    if (hasBrace)
                    {
                        addSymbol(symbols, pendName, pendKind, pendLine);
                        pendActive = false;
                    }
                    else if (hasSemicolon)
                    {
                        pendActive = false;
                    }
                    else if (++pendWait > 6)
                    {
                        pendActive = false;
                    }
                }
                if (t.empty())
                {
                    continue;
                }
                if (depth == 0)
                {
                    bool claimed = false;
                    std::string name;
                    if (wordStart(t, "namespace"))
                    {
                        name = nextWord(afterWord(t, "namespace"));
                        if (!name.empty())
                        {
                            if (hasBrace)
                            {
                                addSymbol(symbols, name, "namespace", i + 1u);
                            }
                            else
                            {
                                pendActive = true;
                                pendKind = "namespace";
                                pendName = name;
                                pendLine = i + 1u;
                                pendWait = 0;
                            }
                            claimed = true;
                        }
                    }
                    else if (wordStart(t, "class") || wordStart(t, "struct"))
                    {
                        const bool isClass = wordStart(t, "class");
                        const char *w = isClass ? "class" : "struct";
                        name = nextWord(afterWord(t, w));
                        if (!name.empty())
                        {
                            if (hasBrace)
                            {
                                addSymbol(symbols, name, isClass ? "class" : "struct", i + 1u);
                            }
                            else
                            {
                                pendActive = true;
                                pendKind = isClass ? "class" : "struct";
                                pendName = name;
                                pendLine = i + 1u;
                                pendWait = 0;
                            }
                            claimed = true;
                        }
                    }
                    if (!claimed && !isControlWord(firstWord(t)))
                    {
                        const std::string::size_type open = t.find('(');
                        if (open != std::string::npos)
                        {
                            std::string fname;
                            if (identBefore(t, open, fname) && !isControlWord(fname) && fname != "operator")
                            {
                                if (hasBrace)
                                {
                                    addSymbol(symbols, fname, "function", i + 1u);
                                }
                                else
                                {
                                    pendActive = true;
                                    pendKind = "function";
                                    pendName = fname;
                                    pendLine = i + 1u;
                                    pendWait = 0;
                                }
                            }
                        }
                    }
                }
                for (const char c : code)
                {
                    if (c == '{')
                    {
                        ++depth;
                    }
                    else if (c == '}')
                    {
                        --depth;
                    }
                }
                if (depth < 0)
                {
                    depth = 0;
                }
            }
        }

        // JS/TS: functions, class, and export by their words; an arrow takes the
        // name it is assigned to.
        static void scanJs(const std::vector<std::string> &lines, json &symbols)
        {
            bool inBlock = false;
            for (std::size_t i = 0u; i < lines.size(); ++i)
            {
                const std::string code = codeOfLine(lines[i], inBlock);
                const std::string t = trim(code);
                if (t.empty())
                {
                    continue;
                }
                if (wordStart(t, "class"))
                {
                    addSymbol(symbols, nextWord(afterWord(t, "class")), "class", i + 1u);
                    continue;
                }
                if (wordStart(t, "export"))
                {
                    const std::string rest = afterWord(t, "export");
                    if (wordStart(rest, "function") || wordStart(rest, "class") || wordStart(rest, "const") ||
                        wordStart(rest, "let") || wordStart(rest, "var"))
                    {
                        addSymbol(symbols, nextWord(rest), "export", i + 1u);
                    }
                    continue;
                }
                if (wordStart(t, "function"))
                {
                    addSymbol(symbols, nextWord(afterWord(t, "function")), "function", i + 1u);
                    continue;
                }
                const std::string::size_type arrow = t.find("=>");
                if (arrow != std::string::npos)
                {
                    std::string::size_type at = arrow;
                    while (at > 0u && (t[at - 1u] == ' ' || t[at - 1u] == '\t'))
                    {
                        --at;
                    }
                    std::string name;
                    if (at > 0u && t[at - 1u] == ')')
                    {
                        std::ptrdiff_t walk = static_cast<std::ptrdiff_t>(at) - 1;
                        long balance = 1;
                        std::ptrdiff_t open = -1;
                        for (; walk >= 0; --walk)
                        {
                            if (t[static_cast<std::size_t>(walk)] == ')')
                            {
                                ++balance;
                            }
                            else if (t[static_cast<std::size_t>(walk)] == '(')
                            {
                                --balance;
                                if (balance == 0)
                                {
                                    open = walk;
                                    break;
                                }
                            }
                        }
                        if (open >= 0)
                        {
                            identBefore(t, static_cast<std::size_t>(open), name);
                        }
                    }
                    else if (at > 0u && isIdent(t[at - 1u]))
                    {
                        identBefore(t, at, name);
                    }
                    if (!name.empty())
                    {
                        addSymbol(symbols, name, "function", i + 1u);
                    }
                    continue;
                }
                const std::string::size_type kw = t.find("function");
                if (kw != std::string::npos && (kw == 0u || !isIdent(t[kw - 1u])))
                {
                    const std::string named = nextWord(t.substr(kw + 8u));
                    if (!named.empty())
                    {
                        addSymbol(symbols, named, "function", i + 1u);
                        continue;
                    }
                    const std::string::size_type eq = t.rfind('=', kw);
                    std::string name;
                    if (eq != std::string::npos && identBefore(t, eq, name))
                    {
                        addSymbol(symbols, name, "function", i + 1u);
                    }
                }
            }
        }

        // Python: def and class, read where they stand with their indent.
        static void scanPy(const std::vector<std::string> &lines, json &symbols)
        {
            for (std::size_t i = 0u; i < lines.size(); ++i)
            {
                const std::string t = trim(lines[i]);
                if (wordStart(t, "def"))
                {
                    addSymbol(symbols, nextWord(afterWord(t, "def")), "function", i + 1u);
                }
                else if (wordStart(t, "class"))
                {
                    addSymbol(symbols, nextWord(afterWord(t, "class")), "class", i + 1u);
                }
            }
        }

        // Rust: fn, struct, trait, impl, mod.
        static void scanRs(const std::vector<std::string> &lines, json &symbols)
        {
            for (std::size_t i = 0u; i < lines.size(); ++i)
            {
                std::string t = trim(lines[i]);
                while (wordStart(t, "pub") || wordStart(t, "unsafe") || wordStart(t, "inline"))
                {
                    t = wordStart(t, "pub")      ? afterWord(t, "pub")
                        : wordStart(t, "unsafe") ? afterWord(t, "unsafe")
                                                 : afterWord(t, "inline");
                }
                if (wordStart(t, "fn"))
                {
                    addSymbol(symbols, nextWord(afterWord(t, "fn")), "function", i + 1u);
                }
                else if (wordStart(t, "struct") || wordStart(t, "union"))
                {
                    addSymbol(symbols, nextWord(afterWord(t, wordStart(t, "struct") ? "struct" : "union")), "struct", i + 1u);
                }
                else if (wordStart(t, "trait"))
                {
                    addSymbol(symbols, nextWord(afterWord(t, "trait")), "trait", i + 1u);
                }
                else if (wordStart(t, "mod"))
                {
                    addSymbol(symbols, nextWord(afterWord(t, "mod")), "mod", i + 1u);
                }
                else if (wordStart(t, "impl"))
                {
                    const std::string rest = afterWord(t, "impl");
                    std::string name = nextWord(rest);
                    const std::string::size_type forAt = rest.find(" for ");
                    if (forAt != std::string::npos)
                    {
                        const std::string after = trim(rest.substr(forAt + 5u));
                        const std::string named = nextWord(after);
                        if (!named.empty())
                        {
                            name = named;
                        }
                    }
                    addSymbol(symbols, name, "impl", i + 1u);
                }
            }
        }

        // Go: func and type.
        static void scanGo(const std::vector<std::string> &lines, json &symbols)
        {
            for (std::size_t i = 0u; i < lines.size(); ++i)
            {
                const std::string t = trim(lines[i]);
                if (wordStart(t, "func"))
                {
                    const std::string rest = afterWord(t, "func");
                    std::string name;
                    if (!rest.empty() && rest[0] == '(')
                    {
                        const std::string::size_type paramsClose = t.find(')');
                        const std::string::size_type open = paramsClose == std::string::npos
                                                                ? std::string::npos
                                                                : t.find('(', paramsClose);
                        if (open != std::string::npos)
                        {
                            identBefore(t, open, name);
                        }
                    }
                    else
                    {
                        name = nextWord(rest);
                    }
                    addSymbol(symbols, name, "function", i + 1u);
                }
                else if (wordStart(t, "type"))
                {
                    addSymbol(symbols, nextWord(afterWord(t, "type")), "type", i + 1u);
                }
            }
        }

        // Shell: function, and the top-level VAR= that names a variable.
        static void scanSh(const std::vector<std::string> &lines, json &symbols)
        {
            for (std::size_t i = 0u; i < lines.size(); ++i)
            {
                const std::string &raw = lines[i];
                if (raw.empty() || raw[0] == ' ' || raw[0] == '\t' || raw[0] == '#')
                {
                    continue; // only top-level words answer
                }
                const std::string t = trim(raw);
                if (t.empty())
                {
                    continue;
                }
                if (wordStart(t, "function"))
                {
                    addSymbol(symbols, nextWord(afterWord(t, "function")), "function", i + 1u);
                    continue;
                }
                std::string::size_type e = 0u;
                while (e < t.size() && (isIdent(t[e]) || t[e] == '.'))
                {
                    ++e;
                }
                if (e == 0u)
                {
                    continue;
                }
                const std::string name = t.substr(0, e);
                if (isControlWord(name))
                {
                    continue;
                }
                if (e < t.size() && t[e] == '(')
                {
                    if (t.find('{') != std::string::npos)
                    {
                        addSymbol(symbols, name, "function", i + 1u);
                    }
                }
                else if (e + 1u < t.size() && t[e] == '=' && t[e + 1u] != '=')
                {
                    addSymbol(symbols, name, "variable", i + 1u);
                }
            }
        }

        // Markdown: the headings, outside fenced blocks.
        static void scanMd(const std::vector<std::string> &lines, json &symbols)
        {
            bool fence = false;
            for (std::size_t i = 0u; i < lines.size(); ++i)
            {
                const std::string t = trim(lines[i]);
                if (t.size() >= 3u && t.compare(0, 3u, "```") == 0)
                {
                    fence = !fence;
                    continue;
                }
                if (fence)
                {
                    continue;
                }
                std::size_t marks = 0u;
                while (marks < t.size() && t[marks] == '#')
                {
                    ++marks;
                }
                if (marks == 0u || marks > 6u)
                {
                    continue;
                }
                if (marks < t.size() && t[marks] != ' ')
                {
                    continue;
                }
                addSymbol(symbols, trim(t.substr(marks)), "heading", i + 1u);
            }
        }

        // ---- the doors ----

        bool readHeadOid(const std::string &repoDir, std::string &oidHex)
        {
            clearNote();
            oidHex.clear();
            ols::gitcore::Repository repo{repoDir};
            ols::gitcore::ObjectId oid;
            if (!repo.readHead(oid))
            {
                noteError("the repo has no HEAD that resolves to an object");
                return false;
            }
            oidHex = oid.to_hex();
            return true;
        }

        bool listRefs(const std::string &repoDir, std::vector<RefInfo> &out)
        {
            clearNote();
            out.clear();
            ols::gitcore::Repository repo{repoDir};
            std::vector<ols::gitcore::RefInfo> rows;
            if (!repo.allRefs(rows))
            {
                noteError("the refs could not be read: the packed-refs is not in a form git writes");
                return false;
            }
            for (const ols::gitcore::RefInfo &r : rows)
            {
                RefInfo info;
                info.name = r.name;
                if (!r.oid.hex.empty())
                {
                    info.oid = r.oid.to_hex();
                }
                info.target = r.target;
                out.push_back(info);
            }
            return true;
        }

        // The shape a raw commit id carries at this edge: forty lowercase hex —
        // sha1's own width, lowercase, exactly as the door's own gate (its
        // codeLooksLikeOid in OpenLocalServices.cpp) states it. normHex alone would
        // take uppercase and the 64-hex sha256 shape through to a lookup that this
        // module then answers with "object not found" words over a name it should
        // never have tried; the shape is required here, before anything is looked
        // up, so the refusal is about the name and nothing else.
        static bool commitOidShapeOk(const std::string &text)
        {
            if (text.size() != 40u)
            {
                return false;
            }
            for (const char c : text)
            {
                const bool digit = c >= '0' && c <= '9';
                const bool lower = c >= 'a' && c <= 'f';
                if (!digit && !lower)
                {
                    return false;
                }
            }
            return true;
        }

        // A raw id checked against the repo: the object is read as a commit, so a
        // blob or tree wearing the same id is refused as honestly as a missing one.
        bool resolveCommitOid(const std::string &repoDir, const std::string &oidHex,
                              std::string &outHex)
        {
            clearNote();
            outHex.clear();
            if (!commitOidShapeOk(oidHex))
            {
                noteError("the name is not a full object id");
                return false;
            }
            const std::string hex = oidHex;
            Reader rd;
            rd.repoDir = repoDir;
            ols::gitcore::CommitObject co;
            std::string why;
            if (!readCommit(rd, hex, co, why))
            {
                noteError(oidHex + ": " + why);
                return false;
            }
            outHex = hex;
            return true;
        }

        bool treeAt(const std::string &repoDir, const std::string &commitOid, const std::string &dirPath,
                    std::vector<TreeEntry> &out)
        {
            clearNote();
            out.clear();
            std::string why;
            if (!pathOk(dirPath, why))
            {
                noteError(why);
                return false;
            }
            Reader rd;
            rd.repoDir = repoDir;
            ols::gitcore::CommitObject co;
            if (!readCommit(rd, commitOid, co, why))
            {
                noteError(why);
                return false;
            }
            std::vector<std::string> parts;
            splitPath(dirPath, parts);
            std::string cur = co.tree.to_hex();
            bool namedFile = false;
            for (std::size_t at = 0u; at < parts.size(); ++at)
            {
                ols::gitcore::TreeObject tree;
                if (!readTree(rd, cur, tree, why))
                {
                    noteError(why);
                    return false;
                }
                bool found = false;
                for (const ols::gitcore::TreeEntry &te : tree.entries)
                {
                    if (te.name != parts[at])
                    {
                        continue;
                    }
                    const bool dirHere = ols::gitcore::treeModeDirectory(te.mode);
                    if (!dirHere)
                    {
                        if (at + 1u == parts.size())
                        {
                            namedFile = true;
                        }
                        continue;
                    }
                    found = true;
                    cur = te.id.to_hex();
                    break;
                }
                if (!found)
                {
                    noteError(namedFile ? "the path names a file, not a directory" : "path not found in tree");
                    return false;
                }
            }
            ols::gitcore::TreeObject tree;
            if (!readTree(rd, cur, tree, why))
            {
                noteError(why);
                return false;
            }
            for (const ols::gitcore::TreeEntry &te : tree.entries)
            {
                TreeEntry row;
                row.name = te.name;
                row.path = dirPath.empty() ? te.name : dirPath + "/" + te.name;
                row.mode = te.mode;
                row.oid = te.id.to_hex();
                row.isDir = ols::gitcore::treeModeDirectory(te.mode);
                out.push_back(row);
            }
            return true;
        }

        bool blobAt(const std::string &repoDir, const std::string &commitOid, const std::string &path,
                    std::vector<std::uint8_t> &outBytes, std::string &oidHex)
        {
            clearNote();
            outBytes.clear();
            oidHex.clear();
            std::string why;
            if (path.empty() || !pathOk(path, why))
            {
                noteError(path.empty() ? "the file path is empty" : why);
                return false;
            }
            Reader rd;
            rd.repoDir = repoDir;
            ols::gitcore::CommitObject co;
            if (!readCommit(rd, commitOid, co, why))
            {
                noteError(why);
                return false;
            }
            std::string fileOid;
            const int st = entryInTree(rd, co.tree.to_hex(), path, fileOid);
            if (st == 0)
            {
                noteError("path not found in tree");
                return false;
            }
            if (st < 0)
            {
                noteError("the tree along the path cannot be read");
                return false;
            }
            std::string content;
            if (!blobContent(rd, fileOid, content))
            {
                noteError("the path is not a file in this tree");
                return false;
            }
            outBytes.assign(content.begin(), content.end());
            oidHex = fileOid;
            return true;
        }

        bool historyFor(const std::string &repoDir, const std::string &startOid, const HistoryAsk &ask,
                        std::vector<CommitInfo> &out, HistoryTail &tail)
        {
            clearNote();
            out.clear();
            tail = HistoryTail{};
            std::string why;
            if (!ask.pathOrEmpty.empty() && !pathOk(ask.pathOrEmpty, why))
            {
                noteError(why);
                return false;
            }
            const std::string start = normHex(startOid);
            if (start.empty())
            {
                noteError("not a valid object id");
                return false;
            }
            std::string after;
            if (!ask.afterOrEmpty.empty())
            {
                after = normHex(ask.afterOrEmpty);
                if (after.empty())
                {
                    noteError("the after cursor is not a full object id");
                    return false;
                }
            }
            if (ask.limit == 0u)
            {
                return true;
            }
            const std::string authorNeedle = asciiLower(ask.authorNeedle);
            const std::string textNeedle = asciiLower(ask.textNeedle);
            Reader rd;
            rd.repoDir = repoDir;
            ols::gitcore::CommitObject head;
            if (!readCommit(rd, start, head, why))
            {
                noteError(why);
                return false;
            }
            // A page that names its cursor starts its walk at the cursor, not again
            // at the head. Restarting at the head and skipping to the cursor made
            // page N walk everything from the head down to the cursor - and left the
            // walk's own 2000-commit bound standing over the very history the cursor
            // reaches. The cursor row itself stays off the page, exactly as it did
            // when the walk skipped past it; every filter and flag walks the same
            // road below.
            std::string walkStart = start;
            bool cursorMissed = false;
            if (!after.empty())
            {
                ols::gitcore::CommitObject cursor;
                if (readCommit(rd, after, cursor, why))
                {
                    walkStart = after;
                }
                else
                {
                    // The object the cursor names does not stand here - a ref that
                    // moved under an open page is how it comes about. Nothing past
                    // it can be walked, so nothing is: the page answers with no
                    // rows, no more to ask, and the words below say why.
                    cursorMissed = true;
                }
            }
            std::unordered_map<std::string, std::vector<std::string>> refMap;
            bool refsRead = false;
            buildTipRefMap(repoDir, refMap, refsRead);
            std::deque<std::string> walk;
            std::unordered_set<std::string> seen;
            if (!cursorMissed)
            {
                walk.push_back(walkStart);
                seen.insert(walkStart);
            }
            std::size_t visited = 0u;
            bool walkedOut = false;
            bool anyChangedCap = false;
            bool anyLarge = false;
            bool anyApprox = false;
            while (!walk.empty())
            {
                if (visited >= kHistoryWalkCap)
                {
                    walkedOut = true;
                    break;
                }
                const std::string oid = walk.front();
                walk.pop_front();
                ++visited;
                ols::gitcore::CommitObject co;
                if (!readCommit(rd, oid, co, why))
                {
                    noteError(why);
                    return false;
                }
                bool show = true;
                if (ask.noMerges && co.parents.size() > 1u)
                {
                    show = false;
                }
                if (show && (co.author.seconds < ask.sinceSecs || co.author.seconds > ask.untilSecs))
                {
                    show = false;
                }
                if (show && !authorNeedle.empty() &&
                    asciiLower(co.author.name).find(authorNeedle) == std::string::npos)
                {
                    show = false;
                }
                if (show && !textNeedle.empty() &&
                    asciiLower(co.message).find(textNeedle) == std::string::npos)
                {
                    show = false;
                }
                if (show && !ask.pathOrEmpty.empty())
                {
                    bool touched = false;
                    if (!changedHere(rd, co, ask.pathOrEmpty, touched))
                    {
                        noteError("the walk hit a tree it could not read");
                        return false;
                    }
                    show = touched;
                }
                // The cursor's own row stays out of the page, exactly as it did when
                // the walk found it and skipped past it.
                if (show && oid != after)
                {
                    CommitInfo row;
                    historyRow(oid, co, refMap, row);
                    if (!addStats(rd, co, row, anyLarge, anyApprox, why))
                    {
                        noteError(why);
                        return false;
                    }
                    if (row.changedTruncated)
                    {
                        anyChangedCap = true;
                    }
                    out.push_back(std::move(row));
                }
                const std::size_t parentsToWalk =
                    ask.firstParent && !co.parents.empty() ? 1u : co.parents.size();
                for (std::size_t p = 0u; p < parentsToWalk; ++p)
                {
                    const std::string ph = co.parents[p].to_hex();
                    if (seen.insert(ph).second)
                    {
                        walk.push_back(ph);
                    }
                }
                if (out.size() >= ask.limit)
                {
                    tail.hasMore = !walk.empty();
                    break;
                }
            }
            if (walkedOut)
            {
                tail.hasMore = true;
                noteError("the history walk stopped at the 2000-commit bound; older commits were not walked");
            }
            std::string cut;
            if (walkedOut)
            {
                cut += "the walk stopped at the 2000-commit bound";
            }
            if (anyChangedCap)
            {
                if (!cut.empty())
                {
                    cut += "; ";
                }
                cut += "a row's changed list stopped at the 200-file bound";
            }
            if (anyLarge)
            {
                if (!cut.empty())
                {
                    cut += "; ";
                }
                cut += "files past the 2 MiB bound are named as changed without counts";
            }
            if (anyApprox)
            {
                if (!cut.empty())
                {
                    cut += "; ";
                }
                cut += "files whose diff ran past its counting budget are named as changed without counts";
            }
            if (!refsRead)
            {
                if (!cut.empty())
                {
                    cut += "; ";
                }
                cut += "the refs could not be read; refs[] holds no names";
            }
            if (cursorMissed)
            {
                // The named shape the pages print: the cursor did not stand in this
                // walk, and there is no more to ask for. An empty page with more
                // still standing would have the pages asking the same walk forever.
                if (!cut.empty())
                {
                    cut += "; ";
                }
                cut += "the after cursor does not stand in this repository; it was not found in this walk";
                tail.hasMore = false;
            }
            tail.truncatedWhy = cut;
            return true;
        }

        bool blameFile(const std::string &repoDir, const std::string &commitOid, const std::string &path,
                       std::vector<BlameLine> &out)
        {
            clearNote();
            out.clear();
            std::string why;
            if (path.empty() || !pathOk(path, why))
            {
                noteError(path.empty() ? "the file path is empty" : why);
                return false;
            }
            const std::string start = normHex(commitOid);
            if (start.empty())
            {
                noteError("not a valid object id");
                return false;
            }
            Reader rd;
            rd.repoDir = repoDir;
            ols::gitcore::CommitObject head;
            if (!readCommit(rd, start, head, why))
            {
                noteError(why);
                return false;
            }
            std::string fileOid;
            const int at = entryInTree(rd, head.tree.to_hex(), path, fileOid);
            if (at == 0)
            {
                noteError("path not found in tree");
                return false;
            }
            if (at < 0)
            {
                noteError("the tree along the path cannot be read");
                return false;
            }
            std::string headContent;
            if (!blobContent(rd, fileOid, headContent))
            {
                noteError("the path is not a file in this tree");
                return false;
            }
            const std::vector<std::string> lines = splitLines(headContent);
            out.assign(lines.size(), BlameLine());
            if (lines.empty())
            {
                return true;
            }
            std::vector<char> owned(lines.size(), 0);
            std::size_t unowned = lines.size();

            std::deque<std::string> walk;
            std::unordered_set<std::string> seen;
            walk.push_back(start);
            seen.insert(start);
            std::size_t visited = 0u;
            bool truncated = false;
            while (!walk.empty())
            {
                if (visited >= kBlameWalkCap)
                {
                    truncated = true;
                    break;
                }
                const std::string oid = walk.front();
                walk.pop_front();
                ++visited;
                ols::gitcore::CommitObject co;
                if (!readCommit(rd, oid, co, why))
                {
                    noteError(why);
                    return false;
                }
                bool changed = false;
                if (!changedHere(rd, co, path, changed))
                {
                    noteError("the walk hit a tree it could not read");
                    return false;
                }
                if (changed)
                {
                    std::set<std::string> here;
                    std::string content;
                    const int fs = fileAtCommit(rd, oid, path, content);
                    if (fs < 0)
                    {
                        noteError("the blob on the path cannot be read");
                        return false;
                    }
                    if (fs == 1)
                    {
                        for (const std::string &l : splitLines(content))
                        {
                            here.insert(l);
                        }
                    }
                    std::set<std::string> parentLines;
                    for (const ols::gitcore::ObjectId &p : co.parents)
                    {
                        std::string pc;
                        const int ps = fileAtCommit(rd, p.to_hex(), path, pc);
                        if (ps < 0)
                        {
                            noteError("a parent's version of the file cannot be read");
                            return false;
                        }
                        if (ps == 1)
                        {
                            for (const std::string &l : splitLines(pc))
                            {
                                parentLines.insert(l);
                            }
                        }
                    }
                    for (std::size_t i = 0u; i < lines.size(); ++i)
                    {
                        if (owned[i] != 0)
                        {
                            continue;
                        }
                        if (here.count(lines[i]) != 0u && parentLines.count(lines[i]) == 0u)
                        {
                            out[i].commitOid = oid;
                            owned[i] = 1;
                            --unowned;
                        }
                    }
                    if (unowned == 0u)
                    {
                        break;
                    }
                }
                for (const ols::gitcore::ObjectId &p : co.parents)
                {
                    if (seen.insert(p.to_hex()).second)
                    {
                        walk.push_back(p.to_hex());
                    }
                }
            }
            if (truncated)
            {
                for (BlameLine &row : out)
                {
                    row.truncated = true;
                }
                noteError("the blame walk stopped at its 200-commit bound; lines older than the walk are left unattributed");
            }
            else if (unowned != 0u)
            {
                noteError("some lines of the file are older than any commit the walk saw; they are left unattributed");
            }
            return true;
        }

        bool outlineForBlob(const std::string &oidHex, const std::vector<std::uint8_t> &bytes,
                            const std::string &pathName, json &out)
        {
            clearNote();
            if (!ols::gitcore::isValidOidHex(oidHex))
            {
                noteError("the blob name is not an object id");
                return false;
            }
            std::string content;
            content.assign(bytes.begin(), bytes.end());
            const std::vector<std::string> lines = splitLines(content);
            const std::string language = langForName(pathName);
            json symbols = json::array();
            if (language == "cpp")
            {
                scanCpp(lines, symbols);
            }
            else if (language == "js")
            {
                scanJs(lines, symbols);
            }
            else if (language == "py")
            {
                scanPy(lines, symbols);
            }
            else if (language == "rs")
            {
                scanRs(lines, symbols);
            }
            else if (language == "go")
            {
                scanGo(lines, symbols);
            }
            else if (language == "sh")
            {
                scanSh(lines, symbols);
            }
            else if (language == "md")
            {
                scanMd(lines, symbols);
            }
            out = json{{"language", language}, {"symbols", symbols}};
            return true;
        }

        // ---- the write half: a save lands as a real commit (spec §5.9) ----
        //
        // What the read half lacked, added: files become objects — blobs, rebuilt
        // trees, one commit — all loose, through the object layer, and the
        // branch moves through the refs transaction, so a save that raced
        // another save is refused with the branch having moved under it rather
        // than dropping either. No packfile and no gc: a file saved here stays
        // loose and the read half reads it as it is. Sha1 only, the format
        // every project repo here stands in — a sha-256 id is refused by the
        // object layer's own words.

        // A commit carries at most this many files.
        static constexpr std::size_t kCommitFilesCap = 200u;

        // A file's content in one commit carries at most this many bytes: 2 MiB.
        static constexpr std::size_t kCommitContentCap = 2097152u;

        // A branch as a ref name under refs/heads/: a bare name and an explicit
        // refs/heads/… name the same branch; anything else wearing refs/ belongs
        // to the other doors.
        static bool branchRefName(const std::string &name, std::string &refName, std::string &why)
        {
            why = "not a branch name this door takes: no '..' segments, no empty parts, no leading or trailing '/'";
            if (!ols::gitcore::isValidRefName(name))
            {
                return false;
            }
            if (name.rfind("refs/heads/", 0u) == 0u)
            {
                refName = name;
                return true;
            }
            if (name.rfind("refs/", 0u) == 0u)
            {
                why = "only a bare name or a refs/heads/… names a branch here; the bare name reaches the same place";
                return false;
            }
            refName = "refs/heads/" + name;
            return true;
        }

        // The branch's current value: 1 with its oid when the ref is there, 0
        // when it is not — an unborn branch, which is how a project's first
        // commit lands — and -1 when it is there but is not a concrete branch
        // ref. The raw form is read because this half moves concrete refs only:
        // a branch pointing at another ref is not what it moves.
        static int branchCurrentOid(ols::gitcore::Repository &repo, const std::string &refName,
                                    std::string &oid, std::string &why)
        {
            std::string value;
            const ols::gitcore::RefStatus st = repo.readRawRef(refName, value);
            if (st == ols::gitcore::RefStatus::Missing)
            {
                return 0;
            }
            if (st == ols::gitcore::RefStatus::TooDeep)
            {
                why = "the branch is a chain of refs that does not resolve";
                return -1;
            }
            if (st == ols::gitcore::RefStatus::Bad)
            {
                why = "the branch ref does not hold a well-formed object id";
                return -1;
            }
            if (ols::gitcore::looksLikeSymref(value))
            {
                why = "a branch that points at another ref is not a concrete branch this door can move";
                return -1;
            }
            oid = normHex(value);
            if (oid.empty())
            {
                why = "the branch ref does not hold a well-formed object id";
                return -1;
            }
            return 1;
        }

        // One change of a save as seen from one directory of the tree: rest is
        // the path below that directory, and the change is to the file its last
        // part names.
        struct DirChange
        {
            std::string rest;
            bool isDelete = false;
            std::string content;
        };

        // A path cut at its first '/'.
        static void pathHead(const std::string &path, std::string &head, std::string &tail)
        {
            const std::string::size_type slash = path.find('/');
            if (slash == std::string::npos)
            {
                head = path;
                tail.clear();
                return;
            }
            head = path.substr(0, slash);
            tail = path.substr(slash + 1u);
        }

        // The new tree of one directory: its entries from the base with the
        // changes applied. A blob on a changed path is written and its entry
        // made; a directory under a changed path is rebuilt, and every entry
        // no change names stays as the base's oid — nothing about an untouched
        // corner of the repo is read or rewritten. prefix only spells the
        // current path in the words of the refusals. The rebuilt tree's oid
        // comes back in oidOut.
        static bool buildTreeWith(Reader &rd, const std::string &baseOid,
                                  const std::vector<DirChange> &changes, const std::string &prefix,
                                  std::string &why, std::string &oidOut)
        {
            ols::gitcore::TreeObject base;
            if (!baseOid.empty() && !readTree(rd, baseOid, base, why))
            {
                return false;
            }

            // Group the changes by the name they touch at this level.
            struct Level
            {
                std::string name;
                bool hasDirect = false;
                bool isDelete = false;
                std::string content;
                std::vector<DirChange> under; // a change under the named thing
            };
            std::vector<Level> levels;
            for (const DirChange &ch : changes)
            {
                std::string head, tail;
                pathHead(ch.rest, head, tail);
                Level *lv = nullptr;
                for (Level &held : levels)
                {
                    if (held.name == head)
                    {
                        lv = &held;
                        break;
                    }
                }
                if (lv == nullptr)
                {
                    levels.push_back(Level());
                    levels.back().name = head;
                    lv = &levels.back();
                }
                if (tail.empty())
                {
                    lv->hasDirect = true;
                    lv->isDelete = ch.isDelete;
                    lv->content = ch.content; // last stated wins, as if applied in turn
                }
                else
                {
                    DirChange sub;
                    sub.rest = tail;
                    sub.isDelete = ch.isDelete;
                    sub.content = ch.content;
                    lv->under.push_back(sub);
                }
            }

            // Keep the base's entries no change names; the named ones are
            // removed, replaced, or rebuilt below.
            std::vector<ols::gitcore::TreeEntry> entries;
            for (const ols::gitcore::TreeEntry &e : base.entries)
            {
                bool named = false;
                for (const Level &lv : levels)
                {
                    if (lv.name == e.name)
                    {
                        named = true;
                        break;
                    }
                }
                if (!named)
                {
                    entries.push_back(e);
                }
            }

            for (const Level &lv : levels)
            {
                const std::string full = prefix.empty() ? lv.name : prefix + "/" + lv.name;
                const ols::gitcore::TreeEntry *at = nullptr;
                for (const ols::gitcore::TreeEntry &e : base.entries)
                {
                    if (e.name == lv.name)
                    {
                        at = &e;
                        break;
                    }
                }
                const bool dirHere = at != nullptr && ols::gitcore::treeModeDirectory(at->mode);
                if (lv.hasDirect && !lv.under.empty())
                {
                    why = "the save names both a file and a deeper path at '" + full + "': a file and a directory cannot share a name";
                    return false;
                }
                if (lv.hasDirect)
                {
                    if (lv.isDelete)
                    {
                        if (at == nullptr)
                        {
                            why = "deleteFile names a path the base tree does not hold: '" + full + "'";
                            return false;
                        }
                        if (dirHere)
                        {
                            why = "deleteFile names a directory, not a file: '" + full + "'";
                            return false;
                        }
                        continue; // taken away
                    }
                    if (dirHere)
                    {
                        why = "the save needs a file where the base holds a directory: '" + full + "'";
                        return false;
                    }
                    ols::gitcore::ObjectId blobId;
                    std::string err;
                    if (!ols::gitcore::looseObjectWrite(rd.repoDir + "/objects",
                                                        ols::gitcore::ObjectKind::Blob, lv.content, blobId, err))
                    {
                        why = "the blob for '" + full + "' could not be written: " + err;
                        return false;
                    }
                    ols::gitcore::TreeEntry fresh;
                    fresh.mode = "100644"; // a saved file is a regular file; the exec bit is not the save's to carry
                    fresh.name = lv.name;
                    fresh.id = blobId;
                    entries.push_back(fresh);
                    continue;
                }
                if (at != nullptr && !dirHere)
                {
                    why = "the path needs a directory where the base holds a file: '" + full + "'";
                    return false;
                }
                std::string childBase;
                if (dirHere)
                {
                    childBase = at->id.to_hex();
                }
                std::string childOid;
                if (!buildTreeWith(rd, childBase, lv.under, full, why, childOid))
                {
                    return false;
                }
                ols::gitcore::ObjectId childId;
                if (!ols::gitcore::ObjectId::parse(childOid, childId))
                {
                    why = "the rebuilt directory at '" + full + "' did not come back with an object id";
                    return false;
                }
                ols::gitcore::TreeEntry fresh;
                fresh.mode = "40000";
                fresh.name = lv.name;
                fresh.id = childId;
                entries.push_back(fresh);
            }

            // The order git keeps tree entries in — byte order of the names, a
            // directory compared as if a slash followed its name — and only then
            // the bytes.
            std::sort(entries.begin(), entries.end(),
                      [](const ols::gitcore::TreeEntry &a, const ols::gitcore::TreeEntry &b)
                      {
                          return ols::gitcore::treeEntryCompare(a, b) < 0;
                      });
            ols::gitcore::TreeObject tree;
            tree.entries = entries;
            std::string content, err;
            if (!ols::gitcore::treeSerialize(tree, content, err))
            {
                why = "the new tree is not in a form git writes: " + err;
                return false;
            }
            ols::gitcore::ObjectId treeId;
            if (!ols::gitcore::looseObjectWrite(rd.repoDir + "/objects",
                                                ols::gitcore::ObjectKind::Tree, content, treeId, err))
            {
                why = "the rebuilt tree could not be written: " + err;
                return false;
            }
            oidOut = treeId.to_hex();
            return true;
        }

        // Land the save. First everything is checked; only then do objects get
        // written, and the branch moves in one transaction that expects the old
        // value the tree was built on — the only way a save can quietly cover
        // another save. When that move is refused, the objects written stay,
        // named by their own hashes and changing nothing else.
        bool commitFiles(const std::string &repoDir, const std::string &branchName,
                         const std::vector<CommitFile> &files, const std::string &message,
                         const std::string &authorName, std::string &newCommitOidHex,
                         bool &refusedByRace)
        {
            clearNote();
            newCommitOidHex.clear();
            refusedByRace = false;
            std::string why;

            if (files.empty())
            {
                noteError("a save with no files is not a commit");
                return false;
            }
            if (files.size() > kCommitFilesCap)
            {
                noteError("a commit carries at most " + std::to_string(kCommitFilesCap) + " files; this save names " +
                          std::to_string(files.size()));
                return false;
            }
            if (trim(message).empty())
            {
                noteError("the commit message is empty");
                return false;
            }
            const std::string author = trim(authorName);
            if (author.empty())
            {
                noteError("no author name was given for the commit");
                return false;
            }
            // '<' and '>' would let a name paint an identity line of its own; a
            // newline splits the record. A tab is refused for the same reason the
            // reflog's own line refuses one - an author wearing it lands the commit
            // and silently drops its reflog entry, and an entry silently missing is
            // one nobody can say where it went. The author simply never lands, and
            // nothing is dropped on the way.
            for (const char c : author)
            {
                if (c == '<' || c == '>' || c == '\n' || c == '\t' || c == '\r' || c == '\0')
                {
                    noteError("an author name carries no '<', '>', newline, tab, carriage return or zero byte");
                    return false;
                }
            }

            std::string refName;
            if (!branchRefName(branchName, refName, why))
            {
                noteError(why);
                return false;
            }

            // Fold the save to one state per path, last stated wins — the same
            // as applying the changes in turn. Paths are validated segment by
            // segment, with the read half's own words.
            std::vector<DirChange> top;
            for (const CommitFile &f : files)
            {
                if (f.path.empty())
                {
                    noteError("the file path is empty");
                    return false;
                }
                if (!pathOk(f.path, why))
                {
                    noteError(why);
                    return false;
                }
                // A save path that ends at a '/' names a folder, and git keeps no
                // folder that holds nothing: a folder stands in a tree only by the
                // entries of the things it holds. The tree builder takes files and
                // writes every entry it is handed whole — it drops nothing, and it
                // cannot stand an empty folder as such — so a save naming one is
                // refused in its own words. What it used to do was worse than a
                // drop: the trailing empty segment fell away and an empty file
                // landed wearing the folder's name.
                if (f.path.back() == '/')
                {
                    noteError("an empty folder is not a thing a commit carries: '" + f.path +
                              "' names a folder, not a file — a folder arrives in a commit only with a file under it");
                    return false;
                }
                if (f.content.size() > kCommitContentCap)
                {
                    noteError("the file at '" + f.path + "' carries more than the 2 MiB a commit takes");
                    return false;
                }
                DirChange ch;
                ch.rest = f.path;
                ch.isDelete = f.deleteFile;
                ch.content = f.content;
                bool placed = false;
                for (DirChange &held : top)
                {
                    if (held.rest == f.path)
                    {
                        held = ch;
                        placed = true;
                        break;
                    }
                }
                if (!placed)
                {
                    top.push_back(ch);
                }
            }

            // The branch as it stands now.
            ols::gitcore::Repository repo{repoDir};
            std::string baseOid;
            const int have = branchCurrentOid(repo, refName, baseOid, why);
            if (have < 0)
            {
                noteError(why);
                return false;
            }
            Reader rd;
            rd.repoDir = repoDir;
            std::string baseTree;
            if (have == 1)
            {
                ols::gitcore::CommitObject baseCommit;
                if (!readCommit(rd, baseOid, baseCommit, why))
                {
                    noteError("the branch does not stand at a commit this reader can take: " + why);
                    return false;
                }
                baseTree = baseCommit.tree.to_hex();
            }

            std::string rootTree;
            if (!buildTreeWith(rd, baseTree, top, std::string(), why, rootTree))
            {
                noteError(why);
                return false;
            }

            // The commit: tree, the branch's old oid as parent when the branch
            // had one, author and committer both from authorName at the wall's
            // current UTC second. The message keeps its bytes; git's own
            // trailing newline is added when it is missing, since
            // commitSerialize appends nothing.
            ols::gitcore::CommitObject co;
            if (!ols::gitcore::ObjectId::parse(rootTree, co.tree))
            {
                noteError("the new tree did not come back with an object id");
                return false;
            }
            if (have == 1)
            {
                co.parents.push_back(ols::gitcore::ObjectId(baseOid));
            }
            const long long when = ols::nowSeconds();
            ols::gitcore::Person who;
            who.name = author;
            who.seconds = when;
            who.zone = "+0000";
            co.author = who;
            co.committer = who;
            co.message = message;
            if (co.message.back() != '\n')
            {
                co.message.push_back('\n');
            }
            std::string raw, err;
            if (!ols::gitcore::commitSerialize(co, raw, err))
            {
                noteError("the commit is not in a form git writes: " + err);
                return false;
            }
            ols::gitcore::ObjectId commitId;
            if (!ols::gitcore::looseObjectWrite(rd.repoDir + "/objects",
                                                ols::gitcore::ObjectKind::Commit, raw, commitId, err))
            {
                noteError("the commit object could not be written: " + err);
                return false;
            }
            const std::string newOid = commitId.to_hex();

            // The move. The expectation is the value the tree was built on; the
            // null expectation says the branch must still not exist — the first
            // two saves cannot both make the first commit.
            ols::gitcore::RefUpdate update;
            update.name = refName;
            update.newOid = commitId;
            if (have == 1)
            {
                update.expectedOldOid = ols::gitcore::ObjectId(baseOid);
            }
            else
            {
                // The null expectation is the all-zero oid at the width the new
                // one carries, never an empty id: the transaction validates an
                // expectation with valid() — which only the zeros of a real
                // width pass — and then reads is_null() as "this name must
                // still not stand": force-lease on a fresh ref, the unborn
                // branch's first commit.
                update.expectedOldOid = ols::gitcore::ObjectId(std::string(newOid.size(), '0'));
            }
            std::vector<ols::gitcore::RefUpdate> updates;
            updates.push_back(update);
            if (!repo.commitRefs(updates))
            {
                // The refusal is shaped by the machine, not by its words: the branch
                // stood elsewhere when the move came, and the answer says so with the
                // flag as well as with the words, so a reader never tells a race from
                // a file whose own name happens to carry one of the words.
                refusedByRace = true;
                noteError("the branch moved under you while the commit was being written: it no longer holds the value the new commit was built on, so the branch was not moved and the new commit stands loose in the repo — read the branch again and save against its new tip");
                return false;
            }

            // The reflog entry for the move, at logs/refs/heads/<branch>: who
            // did it, when, and what for. A log that could not be written does
            // not unsay the commit — the branch moved — so the call still
            // answers true and names the cut here, the way a walk at its bound
            // does.
            ols::gitcore::ReflogEntry entry;
            entry.oldOid = have == 1 ? baseOid : std::string(newOid.size(), '0');
            entry.newOid = newOid;
            entry.who = author + " <>";
            entry.when = std::to_string(when) + " +0000";
            std::string subject = firstLine(message);
            for (char &c : subject)
            {
                if (c == '\t')
                {
                    c = ' ';
                }
            }
            entry.msg = "commit: " + subject;
            if (!repo.reflogAppend(refName, entry))
            {
                noteError("the commit and the branch stand; the reflog entry for the branch could not be written");
            }
            newCommitOidHex = newOid;
            return true;
        }

        // A branch at a commit the repo holds, through the same transaction: the
        // null expectation says the name must still not exist, so two creates
        // cannot both land.
        bool createBranch(const std::string &repoDir, const std::string &name,
                          const std::string &fromOidHex)
        {
            clearNote();
            std::string why;
            std::string refName;
            if (!branchRefName(name, refName, why))
            {
                noteError(why);
                return false;
            }
            const std::string from = normHex(fromOidHex);
            if (from.empty())
            {
                noteError("not a valid object id");
                return false;
            }
            Reader rd;
            rd.repoDir = repoDir;
            ols::gitcore::CommitObject target;
            if (!readCommit(rd, from, target, why))
            {
                noteError("the branch must point at a commit the repo holds: " + why);
                return false;
            }
            ols::gitcore::Repository repo{repoDir};
            std::string old;
            const int have = branchCurrentOid(repo, refName, old, why);
            if (have < 0)
            {
                noteError(why);
                return false;
            }
            if (have == 1)
            {
                noteError("a branch of that name is already there; a create does not move it");
                return false;
            }
            ols::gitcore::RefUpdate update;
            update.name = refName;
            update.newOid = ols::gitcore::ObjectId(from);
            // The same null expectation as commitFiles' unborn case: the all-zero
            // oid at the width of the target — valid(), and is_null() is the
            // transaction's "this name must not stand yet".
            update.expectedOldOid = ols::gitcore::ObjectId(std::string(from.size(), '0'));
            std::vector<ols::gitcore::RefUpdate> updates;
            updates.push_back(update);
            if (!repo.commitRefs(updates))
            {
                noteError("the branch moved under you: its name was taken between the read and the create, so the create refused");
                return false;
            }
            ols::gitcore::ReflogEntry entry;
            entry.oldOid = std::string(from.size(), '0');
            entry.newOid = from;
            entry.who = "unknown <>"; // this call carries no author of its own
            entry.when = std::to_string(ols::nowSeconds()) + " +0000";
            entry.msg = "branch: Created from " + from;
            if (!repo.reflogAppend(refName, entry))
            {
                noteError("the branch stands; its reflog entry could not be written");
            }
            return true;
        }

        // Take a branch away. The loose file first, the way git removes a
        // branch; a branch that also stands in packed-refs is rewritten out of
        // it, or the packed one would read back after the loose delete. The
        // branch HEAD points at is refused, not moved behind HEAD's back.
        bool deleteBranch(const std::string &repoDir, const std::string &name)
        {
            clearNote();
            std::string why;
            std::string refName;
            if (!branchRefName(name, refName, why))
            {
                noteError(why);
                return false;
            }
            ols::gitcore::Repository repo{repoDir};
            if (repo.isHeadSymref())
            {
                std::string target;
                if (repo.headTarget(target) && (target == refName || target == name))
                {
                    noteError("refusing to delete the branch HEAD points at: move HEAD first and the delete is a plain one");
                    return false;
                }
            }
            std::string oldOid;
            const int have = branchCurrentOid(repo, refName, oldOid, why);
            if (have < 0)
            {
                noteError(why);
                return false;
            }
            if (have == 0)
            {
                noteError("there is no branch of that name to delete");
                return false;
            }
            std::vector<ols::gitcore::PackedRefEntry> rows;
            if (!repo.readPackedRefs(rows))
            {
                // Read the packed file before anything is touched: a refusal here
                // then states what it means - the branch stands, loose and packed.
                noteError("the packed-refs is not in a form git writes; the branch was not touched");
                return false;
            }
            const bool removedLoose = repo.deleteLooseRef(refName);
            std::vector<ols::gitcore::PackedRefEntry> kept;
            bool removedPacked = false;
            for (const ols::gitcore::PackedRefEntry &row : rows)
            {
                if (row.name == refName)
                {
                    removedPacked = true; // the row carries its peel line; dropping it drops both
                    continue;
                }
                kept.push_back(row);
            }
            if (removedPacked && !repo.writePackedRefs(kept))
            {
                noteError("the packed-refs could not be rewritten to take the branch out: the branch may still stand in it");
                return false;
            }
            if (!removedLoose && !removedPacked)
            {
                noteError("the branch was gone between the read and the delete: nothing was removed");
                return false;
            }
            ols::gitcore::ReflogEntry entry;
            entry.oldOid = oldOid;
            entry.newOid = std::string(oldOid.size(), '0');
            entry.who = "unknown <>";
            entry.when = std::to_string(ols::nowSeconds()) + " +0000";
            entry.msg = "(deleted)";
            if (!repo.reflogAppend(refName, entry))
            {
                noteError("the branch is gone; its reflog entry could not be written");
            }
            return true;
        }

    } // namespace project
} // namespace ols
