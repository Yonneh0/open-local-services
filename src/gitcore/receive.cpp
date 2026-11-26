// src/gitcore/receive.cpp — the ground receive.h stands on.
//
// The definitions stand external so the server build, which names this file
// among its sources, reads their symbols from its object: MSVC keeps an
// inline-only unit that no case pulls in in code it never emits. No case
// includes this file, so it is compiled as one object and its symbols once.
//
// That link shape is load-bearing and no tidiness applies to it. Both builds
// — the server's and the harness's — name this file among their sources and
// include it nowhere; test_receive.cpp carries the door's whole translation
// unit through its unity include of GitSmartHttp.cpp, and reaches these
// definitions from this file's own object, while the wire definitions that
// same unit shares land inline through test_wire.cpp's include. Naming this
// file a second time, inlining what stands here, or moving it beside the
// wire's inline-only ground breaks the link the harness was built on: the
// double-landing guard over there reaches inline definitions landed twice,
// not external ones meant to stand once.

#include "receive.h"
#include "gitcore/packfile.h"
#include "gitcore/wire.h"

#include <algorithm>
#include <array>
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

            // The bounds the walks take: past one, the answer the walk was making is not a
            // claim, and the refusal says so.
            constexpr std::size_t kMaxAncestorWalk = 65536u; // commits a parent walk reads
            constexpr std::size_t kMaxTreeWalk = 65536u;     // folders a path walk opens

            // The size of an idx follows from the entry count the pack header states: its
            // header, fanout, and trailer, plus 28 bytes per entry, plus 8 per large-offset
            // row.
            constexpr std::size_t kIdxFrameBytes = 1072u;
            constexpr std::size_t kIdxEntryBytes = 28u;
            constexpr std::size_t kIdxLargeRowBytes = 8u;

            // A bare pack is indexed within this bound of bytes: the scan reads each
            // record's stream once and the resolution stands what it finds in memory, so
            // the bound is the memory one indexing may ask for. Past the bound the answer
            // says what was not done.
            constexpr std::size_t kMaxBarePackBytes = 64ull * 1024ull * 1024ull;

            // The least a record can stand for: a header of one byte over a zlib stream,
            // and no zlib stream is shorter than its two bytes of header, one byte of
            // block, and the four of adler-32. A header's record count past what these
            // bytes could carry is a pack saying it is not one — and the bound is taken
            // before any reserve, which is the one allocation that word could drive.
            constexpr std::size_t kMinRecordBytes = 8u;

            struct ObjectView
            {
                std::string kind;    // "commit" | "tree" | "blob" | "tag"
                std::string content; // the bytes after the NUL of the header
            };

            struct PackContent
            {
                std::string kind;
                std::string content;
            };

            using PackMap = std::map<std::string, PackContent>;

            // The word a record header's kind carries, in the words the port speaks.
            const char *recordKindWord(RecordKind kind)
            {
                switch (kind)
                {
                case RecordKind::Commit:
                    return "commit";
                case RecordKind::Tree:
                    return "tree";
                case RecordKind::Blob:
                    return "blob";
                case RecordKind::Tag:
                    return "tag";
                case RecordKind::OfsDelta:
                    [[fallthrough]];
                case RecordKind::RefDelta:
                    return "delta";
                }
                return nullptr;
            }

            // The record kind the port's kind word names: the repository speaks "commit",
            // "tree", "blob", "tag", the pack its codes.
            bool recordKindFromWord(const std::string &word, RecordKind &out)
            {
                if (word == "commit")
                {
                    out = RecordKind::Commit;
                    return true;
                }
                if (word == "tree")
                {
                    out = RecordKind::Tree;
                    return true;
                }
                if (word == "blob")
                {
                    out = RecordKind::Blob;
                    return true;
                }
                if (word == "tag")
                {
                    out = RecordKind::Tag;
                    return true;
                }
                return false;
            }

            // Where an index stands after the pack, when one stands there. found false
            // says the bytes are a bare pack: git's client sends the pack alone after the
            // command's flush, and the index is this unit's to make.
            bool findTrailingIdx(const std::vector<std::uint8_t> &bytes, std::size_t &splitPos,
                                 bool &found, std::string &error)
            {
                found = false;
                if (bytes.size() < 12u + 20u)
                {
                    error = "fewer bytes stand than a pack header and its trailer take";
                    return false;
                }
                if (!(bytes[0] == 'P' && bytes[1] == 'A' && bytes[2] == 'C' && bytes[3] == 'K'))
                {
                    error = "the bytes do not begin with a pack header";
                    return false;
                }
                if (bytes[7] != 2u)
                {
                    error = "the pack is not the version this layer reads";
                    return false;
                }
                const std::size_t entries = (static_cast<std::size_t>(bytes[8]) << 24) |
                                            (static_cast<std::size_t>(bytes[9]) << 16) |
                                            (static_cast<std::size_t>(bytes[10]) << 8) |
                                            static_cast<std::size_t>(bytes[11]);
                // An entry costs at least its name, crc, and offset; a count the bytes
                // cannot carry as an index is not the pair's road at all. The room is
                // weighed before the subtraction: bytes shorter than the pair's least
                // shape — a pack header and an index frame — hold no index at any count,
                // and they are the bare road's to judge, which states in its own words
                // what such bytes do not stand for.
                if (bytes.size() >= 12u + kIdxFrameBytes &&
                    entries <= (bytes.size() - 12u - kIdxFrameBytes) / kIdxEntryBytes)
                {
                    for (std::size_t rows = 0u; rows <= entries; ++rows)
                    {
                        const std::size_t idxSize = kIdxFrameBytes + entries * kIdxEntryBytes +
                                                    rows * kIdxLargeRowBytes;
                        if (bytes.size() <= idxSize)
                        {
                            break; // further rows only grow the index
                        }
                        const std::size_t pos = bytes.size() - idxSize;
                        if (pos >= 12u && bytes[pos] == 0xffu && bytes[pos + 1u] == 0x74u &&
                            bytes[pos + 2u] == 0x4fu && bytes[pos + 3u] == 0x63u &&
                            bytes[pos + 4u] == 0u && bytes[pos + 5u] == 0u && bytes[pos + 6u] == 0u &&
                            bytes[pos + 7u] == 2u)
                        {
                            splitPos = pos;
                            found = true;
                            return true;
                        }
                    }
                }
                return true;
            }

            // The pack's own varints: 7 bits to a byte, low parts first, and the ofs
            // offset whose value is the concatenation least-significant first with a
            // place of 2^7 added for every byte past the first. The packfile unit keeps
            // its readers in its own object; this unit mirrors the format.
            bool readSizeVarint(const std::vector<std::uint8_t> &bytes, std::size_t &pos,
                                std::uint64_t &value)
            {
                value = 0ull;
                int shift = 0;
                for (;;)
                {
                    if (pos >= bytes.size())
                    {
                        return false;
                    }
                    const std::uint8_t b = bytes[pos++];
                    value |= static_cast<std::uint64_t>(b & 0x7fu) << static_cast<unsigned>(shift);
                    if ((b & 0x80u) == 0u)
                    {
                        return true;
                    }
                    shift += 7;
                    if (shift > 63)
                    {
                        return false;
                    }
                }
            }

            bool readOfsOffset(const std::vector<std::uint8_t> &bytes, std::size_t &pos,
                               std::uint64_t &value)
            {
                if (pos >= bytes.size())
                {
                    return false;
                }
                std::uint8_t b = bytes[pos++];
                value = b & 0x7fu;
                for (int count = 1;; ++count)
                {
                    if ((b & 0x80u) == 0u)
                    {
                        return true;
                    }
                    if (pos >= bytes.size() || count >= 10)
                    {
                        return false;
                    }
                    b = bytes[pos++];
                    value = ((value + 1ull) << 7) | (b & 0x7fu);
                }
            }

            // One record header: its kind, the size it declares, and the base it names —
            // a back offset for an ofs delta, a lowercase id for a ref delta. pos leaves
            // at the deflated body.
            bool readRecordHead(const std::vector<std::uint8_t> &pack, std::size_t &pos,
                                RecordKind &kind, std::uint64_t &size, std::uint64_t &back,
                                std::string &baseOid)
            {
                if (pos >= pack.size())
                {
                    return false;
                }
                const std::uint8_t first = pack[pos++];
                const std::uint8_t code = (first >> 4u) & 7u;
                if (code == 0u || code == 5u)
                {
                    return false;
                }
                kind = static_cast<RecordKind>(code);
                size = first & 0x0fu;
                int shift = 4;
                std::uint8_t b = first;
                while ((b & 0x80u) != 0u)
                {
                    if (pos >= pack.size())
                    {
                        return false;
                    }
                    b = pack[pos++];
                    size |= static_cast<std::uint64_t>(b & 0x7fu) << static_cast<unsigned>(shift);
                    shift += 7;
                    if (shift > 63)
                    {
                        return false;
                    }
                }
                back = 0ull;
                baseOid.clear();
                if (kind == RecordKind::OfsDelta)
                {
                    if (!readOfsOffset(pack, pos, back))
                    {
                        return false;
                    }
                }
                else if (kind == RecordKind::RefDelta)
                {
                    if (pos + 20u > pack.size())
                    {
                        return false;
                    }
                    for (int i = 0; i < 20; ++i)
                    {
                        const std::uint8_t raw = pack[pos++];
                        baseOid.push_back("0123456789abcdef"[raw >> 4u]);
                        baseOid.push_back("0123456789abcdef"[raw & 0x0fu]);
                    }
                }
                return true;
            }

            // The base walk of a refused resolve. The chain cap inside the packfile unit
            // is what breaks a REF cycle as well as a long chain, and its refusal states
            // reach; this walk, paid for only by a refusal that already happened, follows
            // the same base links — the index for the names, the record headers for the
            // links — and says true, with the link named, when a walk meets one it has
            // already walked. A walk that ends on a full object, a base outside the pack,
            // or its own cap says nothing about a circle.
            bool baseWalkCircles(const Packfile &opened, const std::vector<std::uint8_t> &pack,
                                 const std::vector<PackObject> &listed, const ObjectId &head,
                                 std::string &revisitedOut)
            {
                std::set<std::string> walked;
                ObjectId step = head;
                for (std::size_t link = 0u; link < kMaxDeltaChain; ++link)
                {
                    if (!walked.insert(step.hex).second)
                    {
                        revisitedOut = detail::lower_hex(step.hex);
                        return true;
                    }
                    PackObject found;
                    if (!opened.lookup(step, found) ||
                        (found.kind != RecordKind::RefDelta && found.kind != RecordKind::OfsDelta))
                    {
                        return false; // the walk stands on a full object, or on nothing it names
                    }
                    std::size_t at = static_cast<std::size_t>(found.offset);
                    RecordKind kind = RecordKind::Blob;
                    std::uint64_t size = 0ull;
                    std::uint64_t back = 0ull;
                    std::string baseOid;
                    if (!readRecordHead(pack, at, kind, size, back, baseOid))
                    {
                        return false;
                    }
                    if (kind == RecordKind::OfsDelta)
                    {
                        if (found.offset < back)
                        {
                            return false; // a base below the pack's beginning is not a link
                        }
                        const std::uint64_t target = found.offset - back;
                        bool stands = false;
                        for (const PackObject &other : listed)
                        {
                            if (other.offset == target)
                            {
                                step = other.oid;
                                stands = true;
                                break;
                            }
                        }
                        if (!stands)
                        {
                            return false;
                        }
                    }
                    else
                    {
                        step = ObjectId(baseOid);
                    }
                }
                return false; // the walk's own cap ran out before any link repeated
            }

            // A full record's header, the mirror of readRecordHead: the kind in the high
            // nibbles of the first byte with the low four bits of the size riding in its
            // low ones, and the size's remaining groups seven bits to a byte, low parts
            // first, the high bit set in all but the last. The records a thin pack stands
            // up as full objects are written with it.
            bool writeFullRecordHead(RecordKind kind, std::uint64_t size, std::vector<std::uint8_t> &out)
            {
                const std::uint8_t code = static_cast<std::uint8_t>(kind);
                if (code < 1u || code > 4u)
                {
                    return false;
                }
                out.clear();
                out.push_back(static_cast<std::uint8_t>((code << 4u) | (size & 0x0fu)));
                size >>= 4u;
                while (size != 0ull)
                {
                    out.back() = static_cast<std::uint8_t>(out.back() | 0x80u);
                    out.push_back(static_cast<std::uint8_t>(size & 0x7fu));
                    size >>= 7u;
                }
                return true;
            }

            // One hex digit to its value.
            int hexDigitValue(char c)
            {
                return (c >= '0' && c <= '9') ? static_cast<int>(c - '0') : static_cast<int>(c - 'a' + 10);
            }

            // The record's deflated body: one stream read over the bytes that remain, its
            // end stated by the stream's own structure. The object layer's span reader
            // decodes to the final block and looks for the adler32 where that block leaves
            // off, so a record is read once — no boundary is tried, and the bytes behind a
            // stream are neither part of it nor its digest. The window a read covers starts
            // at a body's commonest size and doubles while the stream it holds runs past
            // it, so the bytes a scan copies over a pack grow with the pack and not with
            // its square.
            bool findRecordBody(const std::vector<std::uint8_t> &pack, std::size_t dataStart,
                                std::size_t fileEnd, std::uint64_t declared, std::size_t &dataEnd,
                                std::vector<std::uint8_t> &body)
            {
                if (dataStart >= fileEnd || declared > static_cast<std::uint64_t>(kMaxObjectBytes))
                {
                    return false;
                }
                for (std::size_t window = 4096u;; window *= 2u)
                {
                    const std::size_t stop =
                        (fileEnd - dataStart <= window) ? fileEnd : dataStart + window;
                    const std::string span(pack.begin() + static_cast<std::ptrdiff_t>(dataStart),
                                           pack.begin() + static_cast<std::ptrdiff_t>(stop));
                    std::string out;
                    std::size_t consumed = 0u;
                    std::string why;
                    if (inflateZlibSpan(span, kMaxObjectBytes, out, consumed, why))
                    {
                        if (out.size() == declared)
                        {
                            body.assign(out.begin(), out.end());
                            dataEnd = dataStart + consumed;
                            return true;
                        }
                        // The object layer counts two of the adler32 bytes among the payload
                        // of a final stored block: the record's content is what remains once
                        // those two are taken off, and nothing else may differ.
                        if (out.size() == declared + 2ull && declared >= 1ull)
                        {
                            out.resize(static_cast<std::size_t>(declared));
                            body.assign(out.begin(), out.end());
                            dataEnd = dataStart + consumed;
                            return true;
                        }
                        // The stream the bytes state ends here and is not the record its
                        // header declares; a longer span would only hide the disagreement.
                        return false;
                    }
                    if (stop >= fileEnd)
                    {
                        return false;
                    }
                }
            }

            // git's delta instructions over a base whose content is in hand: 0xxxxxxx
            // inserts the following x bytes, 1xxxxxxx takes its low seven bits as the
            // presence mask of an offset and a length, and a length of zero is the 64 KiB
            // git writes as zero. The bounds are checked before any byte is made.
            bool applyDelta(const std::vector<std::uint8_t> &baseContent,
                            const std::vector<std::uint8_t> &delta,
                            std::vector<std::uint8_t> &out)
            {
                std::size_t pos = 0u;
                std::uint64_t baseDeclared = 0ull;
                std::uint64_t target = 0ull;
                if (!readSizeVarint(delta, pos, baseDeclared) || !readSizeVarint(delta, pos, target))
                {
                    return false;
                }
                // What the delta promises to make is read against the ceiling before the
                // instructions run — packfile.cpp's discipline for the same number: one
                // copy instruction is a handful of bytes and moves up to 64 KiB, so a
                // delta of a few hundred kilobytes can name a target of gigabytes
                // without a word of it being untrue to its own arithmetic.
                if (target > static_cast<std::uint64_t>(kMaxObjectBytes))
                {
                    return false;
                }
                if (baseDeclared != baseContent.size())
                {
                    return false;
                }
                std::uint64_t made = 0ull;
                out.clear();
                while (pos < delta.size())
                {
                    const std::uint8_t op = delta[pos++];
                    if ((op & 0x80u) != 0u)
                    {
                        std::uint64_t off = 0ull;
                        std::uint64_t len = 0ull;
                        for (int i = 0; i < 4; ++i)
                        {
                            if ((op & (1u << static_cast<unsigned>(i))) != 0u)
                            {
                                if (pos >= delta.size())
                                {
                                    return false;
                                }
                                off |= static_cast<std::uint64_t>(delta[pos]) << static_cast<unsigned>(i * 8);
                                ++pos;
                            }
                        }
                        for (int i = 0; i < 3; ++i)
                        {
                            if ((op & (0x10u << static_cast<unsigned>(i))) != 0u)
                            {
                                if (pos >= delta.size())
                                {
                                    return false;
                                }
                                len |= static_cast<std::uint64_t>(delta[pos]) << static_cast<unsigned>(i * 8);
                                ++pos;
                            }
                        }
                        if (len == 0ull)
                        {
                            len = 0x10000ull;
                        }
                        if (off > baseContent.size() || len > baseContent.size() - off)
                        {
                            return false;
                        }
                        made += len;
                        out.insert(out.end(), baseContent.begin() + static_cast<std::size_t>(off),
                                   baseContent.begin() + static_cast<std::size_t>(off + len));
                    }
                    else if (op != 0u)
                    {
                        const std::size_t n = op & 0x7fu;
                        if (pos + n > delta.size())
                        {
                            return false;
                        }
                        made += n;
                        out.insert(out.end(), delta.begin() + pos, delta.begin() + pos + n);
                        pos += n;
                    }
                    else
                    {
                        return false; // the reserved opcode
                    }
                    if (made > target)
                    {
                        return false;
                    }
                }
                return made == target;
            }

            // The object kind a record's kind word carries.
            bool objectKindOf(RecordKind kind, ObjectKind &out)
            {
                switch (kind)
                {
                case RecordKind::Commit:
                    out = ObjectKind::Commit;
                    return true;
                case RecordKind::Tree:
                    out = ObjectKind::Tree;
                    return true;
                case RecordKind::Blob:
                    out = ObjectKind::Blob;
                    return true;
                case RecordKind::Tag:
                    out = ObjectKind::Tag;
                    return true;
                default:
                    return false;
                }
            }

            // The index of a bare pack: the records are scanned, each resolved through its
            // delta chain — an ofs base stands at an earlier offset, a ref base is met by
            // the id it names and may stand behind, so the resolution runs in rounds —
            // and the ids the resolved bytes hash to are put in the v2 form writeIdx
            // writes, with the pack's own trailer as the pack checksum it asks for.
            //
            // A thin push — what git sends by default — leaves out the base a delta stands
            // on when the repository already holds it: the record names that base by an id
            // no record of the pack carries. Such a base is read through the port and
            // verified against the id it names, and the records that stand on it resolve
            // against what the repository holds. A pack that borrows a base this way is
            // stood up whole before it is indexed: every delta it carries is written out
            // as a full object over the bytes it resolves to, so the pair the rest of
            // receive opens carries its own bases. pack comes back those bytes; a pack that
            // borrowed nothing comes back untouched. A base that stands neither in the pack
            // nor in the repository is refused as it always was.
            bool indexBarePack(std::vector<std::uint8_t> &pack, const RepoAccess &repo,
                               std::vector<std::uint8_t> &idxOut, std::string &error)
            {
                idxOut.clear();
                if (pack.size() > kMaxBarePackBytes)
                {
                    error = "a bare pack over " + std::to_string(kMaxBarePackBytes) +
                            " bytes is not indexed here: its records are stood up in memory to be indexed";
                    return false;
                }
                if (pack.size() < 12u + 20u)
                {
                    error = "the bytes do not carry a pack header and a trailer";
                    return false;
                }
                if (!(pack[0] == 'P' && pack[1] == 'A' && pack[2] == 'C' && pack[3] == 'K'))
                {
                    error = "the bytes do not begin with a pack header";
                    return false;
                }
                if (pack[7] != 2u)
                {
                    error = "the pack is not the version this layer reads";
                    return false;
                }
                const std::size_t entries = (static_cast<std::size_t>(pack[8]) << 24) |
                                            (static_cast<std::size_t>(pack[9]) << 16) |
                                            (static_cast<std::size_t>(pack[10]) << 8) |
                                            static_cast<std::size_t>(pack[11]);
                const std::size_t fileEnd = pack.size() - 20u;
                // The count the header states is bounded by what the bytes could carry
                // before it drives a single allocation; a count past that is refused at
                // the number, the way the pair road bounds its index.
                const std::size_t entriesCanStand = (fileEnd - 12u) / kMinRecordBytes;
                if (entries > entriesCanStand)
                {
                    error = "the pack header names " + std::to_string(entries) +
                            " records when the bytes hold room for " + std::to_string(entriesCanStand) +
                            " at most";
                    return false;
                }

                struct ScanRecord
                {
                    std::size_t offset = 0u;
                    std::size_t dataEnd = 0u;
                    RecordKind kind = RecordKind::Blob;
                    std::vector<std::uint8_t> body;
                    std::uint64_t back = 0ull;
                    std::string baseOid;
                };
                std::vector<ScanRecord> records;
                records.reserve(entries);
                std::map<std::size_t, std::size_t> atOffset;
                std::size_t pos = 12u;
                for (std::size_t i = 0u; i < entries; ++i)
                {
                    ScanRecord record;
                    record.offset = pos;
                    std::uint64_t declared = 0ull;
                    if (!readRecordHead(pack, pos, record.kind, declared, record.back, record.baseOid))
                    {
                        error = "the record at " + std::to_string(record.offset) +
                                " does not state a kind and a size this layer reads";
                        return false;
                    }
                    if (pos >= fileEnd || !findRecordBody(pack, pos, fileEnd, declared, record.dataEnd, record.body))
                    {
                        error = "the record at " + std::to_string(record.offset) +
                                " has no deflated body that ends where the next record could begin";
                        return false;
                    }
                    atOffset[record.offset] = records.size();
                    records.push_back(std::move(record));
                    pos = record.dataEnd;
                }
                if (pos != fileEnd)
                {
                    error = "the records do not end where the pack's trailer begins";
                    return false;
                }

                std::vector<std::vector<std::uint8_t>> content(records.size());
                std::vector<char> done(records.size(), 0);
                std::vector<RecordKind> kindOf(records.size(), RecordKind::Blob);
                // How many delta links stand under each resolved record: a full object
                // stands on none, a delta on its base's count plus one. The walk takes
                // the same bound as the packfile unit's reader — kMaxDeltaChain links —
                // and refuses at the depth rather than opening a pack its own reader
                // would refuse record by record.
                std::vector<std::size_t> chainDepth(records.size(), 0u);
                std::map<std::string, std::size_t> resolvedByOid;

                // A base read through the port, kept once for every record that stands on
                // it, and marked when the port does not hold it either.
                struct RepoBase
                {
                    bool held = false;
                    RecordKind kind = RecordKind::Blob;
                    std::vector<std::uint8_t> content;
                };
                std::map<std::string, RepoBase> borrowed;
                bool borrowedABase = false;
                bool repoAllowed = false;
                // The pack's own rounds first: nothing is asked of the port while the pack
                // still carries the base a record stands on. A round that makes no progress
                // is the pack saying it carries no more, and the rounds run on with the
                // repository behind them, one link of a chain at a time — and no deeper
                // than the chain this layer walks: kMaxDeltaChain links, the packfile
                // unit's own cap, plus the round that flips to the repository and the
                // no-progress round that closes them.
                bool roundsClosed = false;
                for (std::size_t pass = 0u; pass < kMaxDeltaChain + 2u; ++pass)
                {
                    bool progressed = false;
                    for (std::size_t i = 0u; i < records.size(); ++i)
                    {
                        if (done[i] != 0)
                        {
                            continue;
                        }
                        const ScanRecord &record = records[i];
                        std::vector<std::uint8_t> made;
                        RecordKind kind = record.kind;
                        std::size_t linkDepth = 0u; // delta links standing under this record
                        if (record.kind == RecordKind::OfsDelta || record.kind == RecordKind::RefDelta)
                        {
                            const std::vector<std::uint8_t> *baseContent = nullptr;
                            RecordKind baseKind = RecordKind::Blob;
                            std::size_t baseDepth = 1u;
                            if (record.kind == RecordKind::OfsDelta)
                            {
                                if (record.back > record.offset)
                                {
                                    error = "the ofs delta at " + std::to_string(record.offset) +
                                            " names a base before the pack begins";
                                    return false;
                                }
                                const std::map<std::size_t, std::size_t>::const_iterator at =
                                    atOffset.find(record.offset - static_cast<std::size_t>(record.back));
                                if (at == atOffset.end() || done[at->second] == 0)
                                {
                                    continue; // its base is not resolved yet
                                }
                                baseContent = &content[at->second];
                                baseKind = kindOf[at->second];
                                baseDepth = chainDepth[at->second] + 1u;
                            }
                            else
                            {
                                const std::map<std::string, std::size_t>::const_iterator at =
                                    resolvedByOid.find(record.baseOid);
                                if (at != resolvedByOid.end() && done[at->second] != 0)
                                {
                                    baseContent = &content[at->second];
                                    baseKind = kindOf[at->second];
                                    baseDepth = chainDepth[at->second] + 1u;
                                }
                                else if (!repoAllowed)
                                {
                                    continue; // its base is not resolved yet
                                }
                                else
                                {
                                    // The id names no record of this pack: the thin push
                                    // left the base out, and the repository holds it or
                                    // nothing does. Read once, and checked against the id
                                    // the record names it by.
                                    std::map<std::string, RepoBase>::iterator known =
                                        borrowed.find(record.baseOid);
                                    if (known == borrowed.end())
                                    {
                                        RepoBase base;
                                        std::string kindWord;
                                        std::string contentText;
                                        base.held = repo.readObject(record.baseOid, kindWord, contentText);
                                        if (base.held)
                                        {
                                            if (!recordKindFromWord(kindWord, base.kind))
                                            {
                                                error = "the repository's object " + record.baseOid +
                                                        " is of no kind this layer reads";
                                                return false;
                                            }
                                            ObjectKind baseObjectKind = ObjectKind::Blob;
                                            if (!objectKindOf(base.kind, baseObjectKind))
                                            {
                                                error = "the repository's object " + record.baseOid +
                                                        " is of no kind this layer reads";
                                                return false;
                                            }
                                            std::string rawBase =
                                                objectHeaderMake(baseObjectKind, contentText.size());
                                            rawBase.append(contentText);
                                            const std::string hashed = sha1Hex(rawBase);
                                            if (hashed != record.baseOid)
                                            {
                                                error = "the object the repository hands over as " +
                                                        record.baseOid +
                                                        " is not the bytes that id names: it hashes to " +
                                                        hashed;
                                                return false;
                                            }
                                            base.content.assign(contentText.begin(), contentText.end());
                                        }
                                        known = borrowed.insert(
                                                            std::make_pair(record.baseOid, std::move(base)))
                                                    .first;
                                    }
                                    if (!known->second.held)
                                    {
                                        continue; // neither the pack nor the repository holds it
                                    }
                                    baseContent = &known->second.content;
                                    baseKind = known->second.kind;
                                    borrowedABase = true;
                                }
                            }
                            kind = baseKind;
                            linkDepth = baseDepth;
                            if (baseDepth > kMaxDeltaChain)
                            {
                                error = std::string("a delta chain in the pack runs past the ") +
                                        std::to_string(kMaxDeltaChain) + " links this layer walks";
                                return false;
                            }
                            if (!applyDelta(*baseContent, record.body, made))
                            {
                                error = "the delta at " + std::to_string(record.offset) +
                                        " does not fit the object its base holds";
                                return false;
                            }
                        }
                        else
                        {
                            made = record.body;
                        }
                        ObjectKind objectKind = ObjectKind::Blob;
                        if (!objectKindOf(kind, objectKind))
                        {
                            error = "the record at " + std::to_string(record.offset) +
                                    " carries a delta kind of no object";
                            return false;
                        }
                        std::string raw = objectHeaderMake(objectKind, made.size());
                        raw.append(reinterpret_cast<const char *>(made.data()), made.size());
                        const std::string oid = sha1Hex(raw);
                        if (!isValidOidHex(oid))
                        {
                            error = "the resolved bytes of the record at " + std::to_string(record.offset) +
                                    " do not hash to an id";
                            return false;
                        }
                        content[i].swap(made);
                        kindOf[i] = kind;
                        chainDepth[i] = linkDepth;
                        done[i] = 1;
                        resolvedByOid[oid] = i;
                        progressed = true;
                    }
                    if (progressed)
                    {
                        continue;
                    }
                    if (!repoAllowed)
                    {
                        repoAllowed = true; // the pack holds no further base; ask the repository
                        continue;
                    }
                    roundsClosed = true;
                    break;
                }
                // Rounds that closed themselves found the pack and the repository saying
                // they carry no more; rounds that ran out found a chain past the depth
                // both of them walk.
                const std::string unresolvedWords =
                    roundsClosed
                        ? std::string("a delta in the pack names a base the pack does not carry")
                        : std::string("a delta chain in the pack runs past the ") +
                              std::to_string(kMaxDeltaChain) + " links this layer walks";
                for (std::size_t i = 0u; i < records.size(); ++i)
                {
                    if (done[i] == 0)
                    {
                        error = unresolvedWords;
                        return false;
                    }
                }

                // The pack that borrowed a base does not carry the objects its records
                // stand on. Stand those records up as full objects over the bytes they
                // resolved to, in the order the scan found them, and let the pack the rest
                // of receive opens be one that carries its own bases. The trailer of the
                // bytes as they came is checked here, where the pair opened below would
                // check it: it is the sha-1 of the records.
                if (borrowedABase)
                {
                    std::string hashed;
                    sha1Hex(pack.data(), fileEnd, hashed);
                    bool trailerStands = true;
                    for (std::size_t i = 0u; i < 20u && trailerStands; ++i)
                    {
                        const std::uint8_t byte = static_cast<std::uint8_t>(
                            (static_cast<unsigned>(hexDigitValue(hashed[i * 2u])) << 4u) |
                            static_cast<unsigned>(hexDigitValue(hashed[i * 2u + 1u])));
                        trailerStands = byte == pack[fileEnd + i];
                    }
                    if (!trailerStands)
                    {
                        error = "the bare pack's own trailer is not the sha-1 of its records";
                        return false;
                    }
                    std::vector<std::pair<std::size_t, std::size_t>> spans;
                    spans.reserve(records.size());
                    for (const ScanRecord &record : records)
                    {
                        spans.push_back(std::make_pair(record.offset, record.dataEnd));
                    }
                    std::vector<std::uint8_t> built;
                    built.insert(built.end(), pack.begin(), pack.begin() + 12); // header, version, count
                    for (std::size_t i = 0u; i < records.size(); ++i)
                    {
                        ScanRecord &record = records[i];
                        record.offset = built.size();
                        if (record.kind == RecordKind::OfsDelta || record.kind == RecordKind::RefDelta)
                        {
                            std::vector<std::uint8_t> head;
                            if (!writeFullRecordHead(kindOf[i], content[i].size(), head))
                            {
                                error = "the record at " + std::to_string(spans[i].first) +
                                        " carries a delta kind of no object";
                                return false;
                            }
                            built.insert(built.end(), head.begin(), head.end());
                            std::vector<std::uint8_t> deflated;
                            if (!deflateStored(content[i], deflated))
                            {
                                error = "the object made of the record at " + std::to_string(spans[i].first) +
                                        " does not deflate";
                                return false;
                            }
                            built.insert(built.end(), deflated.begin(), deflated.end());
                        }
                        else
                        {
                            built.insert(built.end(), pack.begin() + static_cast<std::ptrdiff_t>(spans[i].first),
                                         pack.begin() + static_cast<std::ptrdiff_t>(spans[i].second));
                        }
                        record.dataEnd = built.size();
                    }
                    std::string trailer;
                    sha1Hex(built.data(), built.size(), trailer);
                    for (std::size_t i = 0u; i < 20u; ++i)
                    {
                        built.push_back(static_cast<std::uint8_t>(
                            (static_cast<unsigned>(hexDigitValue(trailer[i * 2u])) << 4u) |
                            static_cast<unsigned>(hexDigitValue(trailer[i * 2u + 1u]))));
                    }
                    // The rewrite stands deltas up whole, so it outgrows the bytes that
                    // came in; the bound that refused an over-large pack refuses an
                    // over-large rewrite too, and refuses it before the swap rather than
                    // opening what it just made only to index it.
                    if (built.size() > kMaxBarePackBytes)
                    {
                        error = "the pack stood up whole would be over " +
                                std::to_string(kMaxBarePackBytes) +
                                " bytes: its records are bigger off their bases than this layer indexes";
                        return false;
                    }
                    pack.swap(built);
                }

                std::vector<IndexEntry> indexEntries;
                indexEntries.reserve(records.size());
                for (std::size_t i = 0u; i < records.size(); ++i)
                {
                    const ScanRecord &record = records[i];
                    ObjectKind objectKind = ObjectKind::Blob;
                    if (!objectKindOf(kindOf[i], objectKind))
                    {
                        error = "the record at " + std::to_string(record.offset) +
                                " carries a delta kind of no object";
                        return false;
                    }
                    std::string raw = objectHeaderMake(objectKind, content[i].size());
                    raw.append(reinterpret_cast<const char *>(content[i].data()), content[i].size());
                    IndexEntry entry;
                    if (!ObjectId::parse(sha1Hex(raw), entry.oid))
                    {
                        error = "the object at " + std::to_string(record.offset) +
                                " does not hash to an id this layer reads";
                        return false;
                    }
                    entry.crc = crc32Of(pack.data() + record.offset, record.dataEnd - record.offset);
                    entry.offset = record.offset;
                    indexEntries.push_back(entry);
                }
                std::sort(indexEntries.begin(), indexEntries.end(),
                          [](const IndexEntry &a, const IndexEntry &b)
                          { return a.oid.hex < b.oid.hex; });
                for (std::size_t i = 1u; i < indexEntries.size(); ++i)
                {
                    if (!(indexEntries[i - 1u].oid.hex < indexEntries[i].oid.hex))
                    {
                        error = "two records of the pack hold the same object";
                        return false;
                    }
                }
                // The trailer of the bytes as they stand now — the borrowed pack's own new
                // trailer, and the one every pack carries above it.
                const std::size_t packEnd = pack.size() - 20u;
                const std::array<std::uint8_t, 20> checksum{
                    pack[packEnd], pack[packEnd + 1u], pack[packEnd + 2u], pack[packEnd + 3u],
                    pack[packEnd + 4u], pack[packEnd + 5u], pack[packEnd + 6u], pack[packEnd + 7u],
                    pack[packEnd + 8u], pack[packEnd + 9u], pack[packEnd + 10u], pack[packEnd + 11u],
                    pack[packEnd + 12u], pack[packEnd + 13u], pack[packEnd + 14u], pack[packEnd + 15u],
                    pack[packEnd + 16u], pack[packEnd + 17u], pack[packEnd + 18u], pack[packEnd + 19u]};
                if (!writeIdx(indexEntries, checksum, idxOut))
                {
                    error = "the index of the pack's own records does not write";
                    idxOut.clear();
                    return false;
                }
                return true;
            }

            // The object, from the pack first and the port behind it.
            bool readAny(const PackMap &packContent, const RepoAccess &repo, const std::string &oidHex,
                         ObjectView &out)
            {
                const PackMap::const_iterator at = packContent.find(oidHex);
                if (at != packContent.end())
                {
                    out.kind = at->second.kind;
                    out.content = at->second.content;
                    return true;
                }
                return repo.readObject(oidHex, out.kind, out.content);
            }

            // The id is held somewhere: the pack carries it, or the port does.
            bool holds(const std::set<std::string> &packOids, const RepoAccess &repo,
                       const std::string &oidHex)
            {
                return packOids.count(oidHex) != 0u || repo.hasObject(oidHex);
            }

            // A path no repository may carry: a segment that is "..", or one that starts
            // with a slash.
            bool pathBanned(const std::string &path)
            {
                if (path.empty() || path[0] == '/')
                {
                    return true;
                }
                std::size_t at = 0u;
                while (at < path.size())
                {
                    const std::size_t next = path.find('/', at);
                    const std::size_t end = (next == std::string::npos) ? path.size() : next;
                    if (path.compare(at, end - at, "..") == 0)
                    {
                        return true;
                    }
                    if (end == path.size())
                    {
                        break;
                    }
                    at = end + 1u;
                }
                return false;
            }

            // True with the path that broke the ban when a tree reachable from root names
            // one; the folders it opens are bounded, and past the bound no claim is made.
            bool treeBannedPath(const PackMap &packContent, const RepoAccess &repo,
                                const std::string &rootOid, std::string &pathOut)
            {
                struct Step
                {
                    std::string prefix;
                    std::string oid;
                };
                std::vector<Step> stack;
                Step root;
                root.oid = rootOid;
                stack.push_back(root);
                std::size_t opened = 0u;
                while (!stack.empty())
                {
                    if (opened >= kMaxTreeWalk)
                    {
                        return false;
                    }
                    ++opened;
                    const Step step = stack.back();
                    stack.pop_back();
                    ObjectView view;
                    if (!readAny(packContent, repo, step.oid, view))
                    {
                        continue;
                    }
                    if (view.kind != "tree")
                    {
                        continue;
                    }
                    TreeObject tree;
                    std::string why;
                    if (!treeParse(view.content, tree, why))
                    {
                        continue;
                    }
                    for (const TreeEntry &entry : tree.entries)
                    {
                        const std::string full = step.prefix + entry.name;
                        if (pathBanned(full))
                        {
                            pathOut = full;
                            return true;
                        }
                        if (treeModeDirectory(entry.mode))
                        {
                            Step child;
                            child.prefix = full + "/";
                            child.oid = detail::lower_hex(entry.id.hex);
                            stack.push_back(child);
                        }
                    }
                }
                return false;
            }

            // Parents from head through what the pack and the port hold: visited takes the
            // ids the walk read, and false says the bound was reached, which is not a
            // claim that the history does not continue. A tag is walked through what it
            // tags; an object the walk cannot read ends that branch.
            bool walkCommits(const PackMap &packContent, const RepoAccess &repo,
                             const std::string &headOid, std::size_t limit,
                             std::set<std::string> &visited)
            {
                std::vector<std::string> stack;
                stack.push_back(headOid);
                while (!stack.empty())
                {
                    const std::string oid = stack.back();
                    stack.pop_back();
                    if (!visited.insert(oid).second)
                    {
                        continue;
                    }
                    if (visited.size() > limit)
                    {
                        return false;
                    }
                    ObjectView view;
                    if (!readAny(packContent, repo, oid, view))
                    {
                        continue;
                    }
                    if (view.kind == "commit")
                    {
                        CommitObject commit;
                        std::string why;
                        if (!commitParse(view.content, commit, why))
                        {
                            continue;
                        }
                        for (const ObjectId &parent : commit.parents)
                        {
                            stack.push_back(detail::lower_hex(parent.hex));
                        }
                    }
                    else if (view.kind == "tag")
                    {
                        TagObject tag;
                        std::string why;
                        if (!tagParse(view.content, tag, why))
                        {
                            continue;
                        }
                        stack.push_back(detail::lower_hex(tag.target.hex));
                    }
                }
                return true;
            }

            // The policy names a ref exactly, as the branch it guards.
            bool protectedName(const std::vector<std::string> &protectedNames,
                               const std::string &name)
            {
                for (const std::string &guarded : protectedNames)
                {
                    if (guarded == name)
                    {
                        return true;
                    }
                }
                return false;
            }

            // One field of a line: a name or a reason that carries a break or a NUL would
            // stand as two lines, and the answer would be a lie.
            bool oneLineField(const std::string &text)
            {
                return !text.empty() && text.find('\n') == std::string::npos &&
                       text.find('\0') == std::string::npos;
            }

            // The fields of "<old-oid> <new-oid> <name>", the payload's trailing newline
            // the frame of the argument and not part of it.
            bool parseOneArgument(const std::string &payload, ReceiveCommand &cmd,
                                  std::string &error)
            {
                std::string text = payload;
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                {
                    text.pop_back();
                }
                const std::size_t first = text.find(' ');
                if (first == std::string::npos)
                {
                    error = "a command argument is '<old-oid> <new-oid> <name>'";
                    return false;
                }
                const std::size_t second = text.find(' ', first + 1u);
                if (second == std::string::npos)
                {
                    error = "a command argument is '<old-oid> <new-oid> <name>'";
                    return false;
                }
                cmd.oldOid = detail::lower_hex(text.substr(0u, first));
                cmd.newOid = detail::lower_hex(text.substr(first + 1u, second - first - 1u));
                cmd.name = text.substr(second + 1u);
                if (!isValidOidHex(cmd.oldOid) || !isValidOidHex(cmd.newOid))
                {
                    error = "a command's id is not hex of a width this layer reads: " + text;
                    return false;
                }
                if (!isValidRefName(cmd.name))
                {
                    error = "a command's ref name is not a name git would take: " + cmd.name;
                    return false;
                }
                return true;
            }

        } // anonymous namespace

        // ------------------------------------------------------------------ commands

        bool parseCommands(const std::vector<std::string> &pktPayloads,
                           std::vector<ReceiveCommand> &out, std::string &error)
        {
            out.clear();
            std::vector<ReceiveCommand> parsed;
            std::set<std::string> named;
            for (const std::string &payload : pktPayloads)
            {
                // A framed line carrying only its newline ends the arguments; the
                // "0000" flush is a frame, never a payload handed to this parser.
                if (payload == "\n")
                {
                    break;
                }
                if (payload.empty())
                {
                    error = "a command payload arrived empty: a zero-length payload is not a flush and states no command";
                    out.clear();
                    return false;
                }
                ReceiveCommand cmd;
                if (!parseOneArgument(payload, cmd, error))
                {
                    out.clear();
                    return false;
                }
                if (!named.insert(cmd.name).second)
                {
                    error = "the same ref is named twice: " + cmd.name;
                    out.clear();
                    return false;
                }
                parsed.push_back(cmd);
            }
            // A push that states no command is a malformed conversation, not a push
            // that moved nothing: answered as one, before an empty report could read
            // back as success.
            if (parsed.empty())
            {
                error = "the push carried no commands";
                out.clear();
                return false;
            }
            out.swap(parsed);
            return true;
        }

        // --------------------------------------------------------------- report-status

        bool reportStatus(const std::vector<std::pair<std::string, std::string>> &results,
                          const std::string &bodyFailure,
                          std::vector<std::uint8_t> &out, std::string &error)
        {
            // The shapes are settled before a byte is framed.
            if (!bodyFailure.empty() && !oneLineField(bodyFailure))
            {
                error = "the reason the body did not stand is not one line";
                return false;
            }
            for (const std::pair<std::string, std::string> &row : results)
            {
                if (!oneLineField(row.first))
                {
                    error = "a result names an empty ref, or carries a line break";
                    return false;
                }
                if (!row.second.empty() && !oneLineField(row.second))
                {
                    error = "a refusal carries a line break where its reason should be one line: " + row.first;
                    return false;
                }
            }

            std::vector<std::uint8_t> built;
            // gitprotocol-pack reserves the unpack-ng line for a body that did not
            // unpack; a command that refused is its own ng line, and the unpack line
            // stays ok.
            if (!bodyFailure.empty())
            {
                if (!pktWrite("unpack ng " + bodyFailure + "\n", built, error))
                {
                    return false;
                }
            }
            else if (!pktWrite("unpack ok\n", built, error))
            {
                return false;
            }
            for (const std::pair<std::string, std::string> &row : results)
            {
                std::string line = row.second.empty()
                                       ? "ok " + row.first + "\n"
                                       : "ng " + row.first + " " + row.second + "\n";
                if (!pktWrite(line, built, error))
                {
                    return false;
                }
            }
            pktFlush(built);
            out.swap(built);
            return true;
        }

        bool reportStatus(const std::vector<std::pair<std::string, std::string>> &results,
                          std::vector<std::uint8_t> &out, std::string &error)
        {
            return reportStatus(results, std::string(), out, error);
        }

        // -------------------------------------------------------------------- receive

        bool receive(const RepoAccess &repo, const std::vector<std::uint8_t> &packBytes,
                     const std::vector<ReceiveCommand> &cmds, const ReceivePolicy &policy,
                     std::vector<std::pair<std::string, std::string>> &resultsOut,
                     std::string &error)
        {
            bool bodyFailed = false;
            return receive(repo, packBytes, cmds, policy, resultsOut, bodyFailed, error);
        }

        bool receive(const RepoAccess &repo, const std::vector<std::uint8_t> &packBytes,
                     const std::vector<ReceiveCommand> &cmds, const ReceivePolicy &policy,
                     std::vector<std::pair<std::string, std::string>> &resultsOut,
                     bool &bodyFailed, std::string &error)
        {
            resultsOut.clear();
            error.clear();
            bodyFailed = false;

            // The port is handed as a const reference, the shape of the brief, while
            // writing an object and moving a ref are what the port does: the constness
            // is cast back at the one place those three calls stand.
            RepoAccess &port = *const_cast<RepoAccess *>(&repo);

            // The cap is on the bytes as handed, before any of them is read.
            if (static_cast<std::uint64_t>(packBytes.size()) > policy.maxPackBytes)
            {
                error = "the pack is over the accepted size: " + std::to_string(packBytes.size()) +
                        " bytes were handed, the cap is " + std::to_string(policy.maxPackBytes);
                bodyFailed = true;
                return false;
            }

            // The pair the wire's road hands over, or the pack alone, whose index is
            // made here. A push that only removes refs brings no pack: git sends
            // nothing behind the commands, and a removal names no object to stand.
            // gitprotocol-pack states the pack section as "1*a-fragment [flush-pkt]":
            // the closing flush is optional, and a client that brings no pack may
            // state that with a flush alone. The bytes are read whole first; the
            // flush comes off only when they do not open as handed and it is what
            // stands at their end — a pack whose trailer happens to close with four
            // zero bytes opens on the first read and is never touched by the trim.
            std::vector<std::uint8_t> pack;
            std::vector<std::uint8_t> idx;
            Packfile opened;
            std::vector<PackObject> listed;
            if (!packBytes.empty())
            {
                auto readBody = [&](const std::vector<std::uint8_t> &bytes) -> bool
                {
                    if (bytes.empty())
                    {
                        return true; // the empty section: a push that brings no pack
                    }
                    std::size_t splitPos = 0u;
                    bool indexFound = false;
                    if (!findTrailingIdx(bytes, splitPos, indexFound, error))
                    {
                        return false;
                    }
                    std::vector<std::uint8_t> packCandidate;
                    std::vector<std::uint8_t> idxCandidate;
                    if (indexFound)
                    {
                        packCandidate.assign(bytes.begin(), bytes.begin() + splitPos);
                        idxCandidate.assign(bytes.begin() + splitPos, bytes.end());
                    }
                    else
                    {
                        packCandidate.assign(bytes.begin(), bytes.end());
                        if (!indexBarePack(packCandidate, repo, idxCandidate, error))
                        {
                            return false;
                        }
                    }
                    Packfile openedTry;
                    if (!openedTry.open(packCandidate, idxCandidate))
                    {
                        error = "the pack and its index do not stand as a pair: the record scan and the index disagree, or a checksum does not match";
                        return false;
                    }
                    std::vector<PackObject> listedTry;
                    if (!openedTry.list(listedTry))
                    {
                        error = "the opened pack does not list the objects its index names";
                        return false;
                    }
                    // From the pack standing to the objects being landed, every
                    // refusal is a claim about the body itself: it is what an
                    // "unpack ng" answer may state.
                    pack.swap(packCandidate);
                    idx.swap(idxCandidate);
                    opened = openedTry;
                    listed.swap(listedTry);
                    return true;
                };

                bool bodyStood = readBody(packBytes);
                if (!bodyStood && packBytes.size() >= 4u)
                {
                    const std::size_t tail = packBytes.size() - 4u;
                    const bool closesWithFlush =
                        packBytes[tail] == static_cast<std::uint8_t>('0') &&
                        packBytes[tail + 1u] == static_cast<std::uint8_t>('0') &&
                        packBytes[tail + 2u] == static_cast<std::uint8_t>('0') &&
                        packBytes[tail + 3u] == static_cast<std::uint8_t>('0');
                    if (closesWithFlush)
                    {
                        // The words of the bytes as handed are the honest statement of
                        // what was refused, so they are kept while the shorter reading
                        // is tried, and come back when it does not stand either.
                        const std::string directError = error;
                        const std::vector<std::uint8_t> without(
                            packBytes.begin(), packBytes.end() - 4u);
                        bodyStood = readBody(without);
                        if (bodyStood)
                        {
                            error.clear(); // it stands: the first reading's words are not the answer
                        }
                        else
                        {
                            error = directError;
                        }
                    }
                }
                if (!bodyStood)
                {
                    bodyFailed = true;
                    return false;
                }
            }

            // The objects are stood up first, every one of them, and the references
            // between them are judged only once the set is whole: the list runs in id
            // order, not in the order one object stands on another, and a commit whose
            // id sorts before its own tree's would otherwise be refused a root the same
            // pack carries.
            std::set<std::string> packOids;
            PackMap packContent;
            // commit oid -> the root of its trees, so the path walk starts where the
            // pack put a commit's content begins.
            std::map<std::string, std::string> commitTrees;

            for (const PackObject &po : listed)
            {
                const std::string oid = detail::lower_hex(po.oid.hex);
                std::vector<std::uint8_t> raw;
                if (!opened.resolveOid(po.oid, raw))
                {
                    // The cap is what ends a REF cycle as well as a long chain, so a
                    // refusal here is read again as a walk: when it meets a link it
                    // has already walked, the circle is named plainly instead of
                    // blaming reach.
                    std::string circle;
                    if (baseWalkCircles(opened, pack, listed, po.oid, circle))
                    {
                        error = "the pack object " + oid +
                                " does not resolve: its delta chain runs in a circle; the base walk returns to " +
                                circle;
                    }
                    else
                    {
                        error = "the pack object " + oid +
                                " does not resolve: its record does not inflate, or its delta base is out of reach";
                    }
                    bodyFailed = true;
                    return false;
                }
                const std::string rawBytes(raw.begin(), raw.end());
                const std::string hashed = sha1Hex(rawBytes);
                if (hashed != oid)
                {
                    error = "the pack object " + oid + " is not the bytes its index names: it hashes to " + hashed;
                    bodyFailed = true;
                    return false;
                }
                ObjectKind kind = ObjectKind::Blob;
                std::uint64_t size = 0ull;
                std::string content;
                std::string why;
                if (!objectSplit(rawBytes, kind, size, content, why))
                {
                    error = "the pack object " + oid + " is not an object this layer reads: " + why;
                    bodyFailed = true;
                    return false;
                }
                const std::string headerKind = kindName(kind);
                // A delta record states no kind of its own — the base's kind is the
                // object's, and the header only says a delta stands here — so the
                // cross-check speaks to the records that do state one.
                if (po.kind != RecordKind::OfsDelta && po.kind != RecordKind::RefDelta)
                {
                    const char *listedKind = recordKindWord(po.kind);
                    if (listedKind == nullptr || headerKind != listedKind)
                    {
                        error = "the index states an object of one kind where the record's own header states another: " + oid;
                        bodyFailed = true;
                        return false;
                    }
                }
                packOids.insert(oid);
                packContent[oid] = PackContent{headerKind, content};
            }

            for (const PackObject &po : listed)
            {
                const std::string oid = detail::lower_hex(po.oid.hex);
                const PackMap::const_iterator held = packContent.find(oid);
                if (held == packContent.end())
                {
                    continue;
                }
                const std::string &content = held->second.content;
                if (held->second.kind == "tree")
                {
                    TreeObject tree;
                    std::string why;
                    if (!treeParse(content, tree, why))
                    {
                        // The pack opened and the object stands under the id its
                        // index names; its content is what this layer will not read.
                        // That refusal travels as the command's own words — the flag
                        // is the body's claim, and a_tree_entry_wearing_a_leading_
                        // slash_never_reaches_the_ban_words pins the line the claim
                        // stops at. The same doctrine stands the commit and tag
                        // refusals below.
                        error = "the pack tree " + oid + " is not a tree this layer reads: " + why;
                        return false;
                    }
                    for (const TreeEntry &entry : tree.entries)
                    {
                        const std::string child = detail::lower_hex(entry.id.hex);
                        if (!holds(packOids, repo, child))
                        {
                            error = "the pack tree " + oid + " names an entry " + child +
                                    " that neither the pack nor the repository holds";
                            bodyFailed = true;
                            return false;
                        }
                    }
                }
                else if (held->second.kind == "commit")
                {
                    CommitObject commit;
                    std::string why;
                    if (!commitParse(content, commit, why))
                    {
                        error = "the pack commit " + oid + " is not a commit this layer reads: " + why;
                        return false; // content refused, as the tree's above
                    }
                    const std::string root = detail::lower_hex(commit.tree.hex);
                    if (!holds(packOids, repo, root))
                    {
                        error = "the pack commit " + oid + " is rooted at " + root +
                                " that neither the pack nor the repository holds";
                        bodyFailed = true;
                        return false;
                    }
                    for (const ObjectId &parent : commit.parents)
                    {
                        const std::string parentOid = detail::lower_hex(parent.hex);
                        if (!holds(packOids, repo, parentOid))
                        {
                            error = "the pack commit " + oid + " names a parent " + parentOid +
                                    " that neither the pack nor the repository holds";
                            bodyFailed = true;
                            return false;
                        }
                    }
                    commitTrees[oid] = root;
                }
                else if (held->second.kind == "tag")
                {
                    TagObject tag;
                    std::string why;
                    if (!tagParse(content, tag, why))
                    {
                        error = "the pack tag " + oid + " is not a tag this layer reads: " + why;
                        return false; // content refused, as the tree's above
                    }
                    const std::string target = detail::lower_hex(tag.target.hex);
                    if (!holds(packOids, repo, target))
                    {
                        error = "the pack tag " + oid + " points at " + target +
                                " that neither the pack nor the repository holds";
                        bodyFailed = true;
                        return false;
                    }
                }
            }

            // The objects that stand are written before a ref is asked about, the way a
            // push keeps what it received even when a ref does not move.
            for (const PackObject &po : listed)
            {
                const std::string oid = detail::lower_hex(po.oid.hex);
                const PackMap::const_iterator at = packContent.find(oid);
                if (at == packContent.end() || repo.hasObject(oid))
                {
                    continue;
                }
                std::string written;
                if (!port.writeLoose(at->second.kind, at->second.content, written))
                {
                    error = "the object " + oid + " could not be written where the repository holds objects";
                    bodyFailed = true;
                    return false;
                }
                if (detail::lower_hex(written) != oid)
                {
                    error = "the bytes the port wrote are not the object the pack carries: " + oid +
                            " came back as " + detail::lower_hex(written);
                    bodyFailed = true;
                    return false;
                }
            }

            // The content the pack brings, judged on its paths; history already in the
            // repository was accepted before.
            std::map<std::string, std::string> bannedCommit;
            for (const std::pair<const std::string, std::string> &rooted : commitTrees)
            {
                std::string path;
                if (treeBannedPath(packContent, repo, rooted.second, path))
                {
                    bannedCommit[rooted.first] = path;
                }
            }

            // Past here a refusal is one command's, never the body's.
            bodyFailed = false;

            std::vector<std::pair<std::string, std::string>> results;
            results.reserve(cmds.size());
            std::vector<RefUpdate> updates;
            // The rows a removal stands on, in the order the commands came; the row
            // carries the name, so a removal that does not go names itself.
            std::vector<std::size_t> deleteRows;

            for (const ReceiveCommand &cmd : cmds)
            {
                const std::string name = cmd.name;
                const std::string oldOid = detail::lower_hex(cmd.oldOid);
                const std::string newOid = detail::lower_hex(cmd.newOid);
                const bool make = detail::all_zero_hex(oldOid);
                const bool drop = detail::all_zero_hex(newOid);
                const bool guarded = protectedName(policy.protectedBranches, name);
                std::string refusal;

                std::string heldOid;
                const bool haveRef = repo.refValue(name, heldOid);
                heldOid = detail::lower_hex(heldOid);

                if (make && drop)
                {
                    refusal = "both ids are zero, so the command moves nothing";
                }
                else if (drop)
                {
                    if (guarded)
                    {
                        refusal = "the branch is protected and its deletion is not asked for";
                    }
                    else if (!haveRef)
                    {
                        refusal = "the ref is not there, so there is nothing to delete";
                    }
                    else if (heldOid != oldOid)
                    {
                        refusal = "the old id is not what the ref holds";
                    }
                }
                else if (make)
                {
                    if (haveRef)
                    {
                        refusal = "the ref is already there and the command names no old id";
                    }
                    else if (!holds(packOids, repo, newOid))
                    {
                        // A create of a protected name is what a first push asks; the
                        // repository holding nothing under it is the match of a zero
                        // old id.
                        refusal = "the new id is not held in the pack or the repository";
                    }
                }
                else if (!haveRef)
                {
                    refusal = "the old id is not what the ref holds";
                }
                else if (heldOid != oldOid)
                {
                    refusal = "the old id is not what the ref holds";
                }

                if (refusal.empty() && !drop)
                {
                    if (!holds(packOids, repo, newOid))
                    {
                        refusal = "the new id is not held in the pack or the repository";
                    }
                    else
                    {
                        // One walk answers both the path ban and the fast-forward.
                        std::set<std::string> visited;
                        const bool walked = walkCommits(packContent, repo, newOid,
                                                        kMaxAncestorWalk, visited);
                        for (const std::string &oid : visited)
                        {
                            const std::map<std::string, std::string>::const_iterator found =
                                bannedCommit.find(oid);
                            if (found != bannedCommit.end())
                            {
                                refusal = "the trees it brings name a path no repository may carry: '" +
                                          found->second + "'";
                                break;
                            }
                        }
                        if (refusal.empty() && !make)
                        {
                            if (!walked)
                            {
                                refusal = "the ancestry cannot be proved within the walk of " +
                                          std::to_string(kMaxAncestorWalk) + " commits";
                            }
                            else if (visited.count(oldOid) == 0u)
                            {
                                if (guarded)
                                {
                                    refusal = "a protected branch takes a fast-forward only";
                                }
                                else if (!policy.allowNonFastForward)
                                {
                                    refusal = "not a fast-forward: the history would be rewritten";
                                }
                            }
                        }
                    }
                }

                results.push_back(std::make_pair(name, refusal));
                if (!refusal.empty())
                {
                    continue;
                }
                if (drop)
                {
                    deleteRows.push_back(results.size() - 1u);
                    continue;
                }
                RefUpdate update;
                update.name = name;
                update.newOid = ObjectId(newOid);
                // The zero expectation is a create: the ref must not be there.
                update.expectedOldOid = ObjectId(oldOid);
                updates.push_back(update);
            }

            // The moves go through the transaction first: it is all of them or none,
            // so a transaction that does not hold is the one refusal where "no ref
            // moved" is the truth — the removals have not run, and no move did.
            if (!updates.empty() && !port.commitRefs(updates))
            {
                error = "the ref transaction did not hold: no ref moved";
                resultsOut.clear();
                return false;
            }

            // The removals run behind it, one name per call, so a removal that does
            // not go names itself: that row carries the words and every other row
            // keeps its own answer, the push standing with what it moved.
            for (const std::size_t row : deleteRows)
            {
                const std::vector<std::string> one(1u, results[row].first);
                if (!port.deleteRefs(one))
                {
                    results[row].second = "the removal did not go";
                }
            }

            resultsOut.swap(results);
            return true;
        }

    } // namespace gitcore
} // namespace ols
