// src/gitcore/packfile.h — packfiles and their index, the packfile contract of
// spec §3.1: idx v2 read and write, pack reading with OFS/REF delta chains
// resolved iteratively (never recursion depth = chain length; chains past the
// cap are refused), and a full-object pack writer that feeds Phase G bundles
// and later repacks. Delta compression on the write side is out of scope; it
// plugs in at writePack (see the note there).
//
// Failure is a return value, never a thrown exception: every call says false
// for malformed input, and nothing on this surface throws.
//
// zlib: B1's object layer owns the inflater. It is not in the tree yet, so
// inflateRaw is declared here as B2's forward of that link symbol; when B1's
// objects.h lands its own declaration takes over and the call sites are
// unchanged.

#ifndef OLS_GITCORE_PACKFILE_H
#define OLS_GITCORE_PACKFILE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ---- the object id: B1's when it is in the tree, a stand-in until then ----

#if defined(__has_include)
#if __has_include("objects.h")
// The include stands at file scope, not inside the namespaces below: an include
// from within ols::gitcore would nest whatever it declares (ols::gitcore::std
// among it) and poison every later std:: name in the translation unit. Same
// convention refs.h keeps; the macro marks whose ObjectId this is so the two
// headers never disagree about it.
#include "objects.h"
#define OLS_GITCORE_OID_IS_B1S 1
#endif
#endif

namespace ols
{
    namespace gitcore
    {

#ifndef OLS_GITCORE_OID_IS_B1S
        // The minimal forward interface: hex in, hex out, sortedness. Self-contained so
        // this TU asks nothing of B1's or of refs.cpp's symbols. A translation unit
        // that includes this header and refs.h both before B1 lands would carry two
        // stand-ins; whoever joins them reconciles this one place.
        struct ObjectId
        {
            std::string hex;

            static bool oid_hex_ok(const std::string &text)
            {
                if (text.size() != 40 && text.size() != 64)
                {
                    return false;
                }
                for (const char c : text)
                {
                    const bool digit = c >= '0' && c <= '9';
                    const bool upper = c >= 'A' && c <= 'F';
                    const bool lower = c >= 'a' && c <= 'f';
                    if (!digit && !upper && !lower)
                    {
                        return false;
                    }
                }
                return true;
            }

            // false unless the text is a well-formed oid hex; on true, out carries it
            // lowercased.
            static bool parse(const std::string &text, ObjectId &out)
            {
                if (!oid_hex_ok(text))
                {
                    return false;
                }
                out.hex.clear();
                for (const char c : text)
                {
                    out.hex.push_back(c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c);
                }
                return true;
            }

            bool valid() const { return oid_hex_ok(hex); }
            const std::string &to_hex() const { return hex; }

            bool operator==(const ObjectId &o) const { return hex == o.hex; }
            bool operator<(const ObjectId &o) const { return hex < o.hex; }
        };
#endif

        // ---- what a record's first byte states ----

        // The kinds exactly as git writes them in a pack record header. OfsDelta and
        // RefDelta are the two delta shapes; the three real kinds plus Tag are the
        // only kinds a writer here produces.
        enum class RecordKind : std::uint8_t
        {
            Commit = 1,
            Tree = 2,
            Blob = 3,
            Tag = 4,
            OfsDelta = 6,
            RefDelta = 7
        };

        // The longest delta chain resolve will walk: the number of delta links it
        // takes before it refuses. The walk is iterative; the cap is also what makes
        // a cyclic REF chain terminate.
        constexpr std::size_t kMaxDeltaChain = 1024;

        // The largest object one record of a pack is allowed to be: its declared body
        // size, the bytes its deflated stream yields, and the result of applying a
        // delta to a base are all read against this. A record that states more is
        // refused at the number, before a byte of it is stood up in memory — a header
        // claiming 2^63 bytes and a delta whose instructions copy a small base four
        // billion times are both words a hostile pack can say, and the answer here is
        // words back rather than an allocation. Past this sits nothing the studio
        // serves: its bulk stays out of its repos the way dev.md keeps it out of the
        // tree, and the one number is where a larger limit would be stated if a repo
        // ever needed one.
        constexpr std::size_t kMaxObjectBytes = std::size_t{512} * 1024u * 1024u;

        // ---- the index (idx v2) ----

        // One entry of the .idx: where an object is and the crc32 of its record.
        struct IndexEntry
        {
            ObjectId oid;             // lowercase hex
            std::uint32_t crc = 0;    // crc32 of the record: header through deflated body
            std::uint64_t offset = 0; // byte offset of the record in the pack
        };

        // The whole idx read and validated: the v2 magic, version 2, a fanout that
        // agrees entry-by-entry with the names, strictly sorted names of one length,
        // the offsets (with the large-offset table when any 4-byte slot has its high
        // bit set), and the trailer — the last 20 bytes must be the sha-1 of all the
        // bytes above them (git's index checksum; the 20 before those are the pack
        // checksum, cross-checked against the pack when a Packfile opens both
        // together).
        bool readIdx(const std::vector<std::uint8_t> &bytes, std::vector<IndexEntry> &out);

        // The idx written byte-exactly: fanout built from scratch, sorted entries,
        // crcs, 4-byte offsets or a large-offset table when any offset needs more
        // than 31 bits, then the pack checksum as given, then the index checksum.
        // Entries must be strictly sorted by oid and share one oid length; anything
        // else is refused.
        bool writeIdx(const std::vector<IndexEntry> &entries,
                      const std::array<std::uint8_t, 20> &pack_checksum,
                      std::vector<std::uint8_t> &out);

        // ---- the shared primitives, in-tree ----

        // The zlib crc32 of the bytes, exactly as git writes it in an idx: the IEEE
        // reflected polynomial, walk seeded with all-ones, result complemented.
        std::uint32_t crc32Of(const std::uint8_t *data, std::size_t n);

        // sha-1 of raw bytes as 40 lowercase hex. B1's object layer will own its own
        // hash; this one is what the packfile code uses for its trailers, and the
        // name stays inside this layer so the two never collide.
        bool sha1Hex(const std::uint8_t *data, std::size_t n, std::string &hex_out);

        // A valid zlib stream for the given bytes, made of stored blocks (the level-
        // zero equivalent): what writePack emits for record bodies.
        bool deflateStored(const std::vector<std::uint8_t> &raw, std::vector<std::uint8_t> &out);

        // The format's two varints, exposed so the writer's claims and the tests can
        // state the exact bytes: the "size encoding" (7 bits per byte, low parts
        // first) and the ofs-delta "offset encoding" (high bit set in all but the
        // last byte; the value is the seven-bit concatenation least-significant
        // first, plus 2^7 + 2^14 + ... for n >= 2 — the decode is
        // v = ((v + 1) << 7) | (b & 0x7f) running over the bytes).
        bool encodeSizeVarint(std::uint64_t value, std::vector<std::uint8_t> &out);
        bool encodeOfsOffset(std::uint64_t value, std::vector<std::uint8_t> &out);

        // ---- B1's inflater, forward-declared ----

        // Decompresses a zlib-wrapped DEFLATE stream into raw bytes. zlibwrap.cpp
        // resolves this at link (it reads through the object layer's inflateZlib);
        // the declaration stands whether or not objects.h is in the tree, because the
        // object layer names no such symbol itself.
        bool inflateRaw(const std::uint8_t *in, std::size_t n, std::vector<std::uint8_t> &out);

        // ---- the pack and its index, open together ----

        // What one object is: where it is, what its header says it is and holds.
        struct PackObject
        {
            ObjectId oid;
            std::uint32_t crc = 0;
            std::uint64_t offset = 0;
            RecordKind kind = RecordKind::Blob; // the kind its record header states
            std::uint64_t size = 0;             // declared size: content for a full object,
                                                // delta bytes for a delta record
        };

        class Packfile
        {
        public:
            // The pack and the idx, validated as a pair: the record scan reproduces
            // the idx exactly (same object set, same offsets, same crcs), both
            // checksums agree, and every base every record names is present. The scan
            // reads record headers and no body: opening a pack states what it holds
            // without standing any object up, and a record's deflated body inflates
            // only when resolve reaches it. (A record whose header is sound but whose
            // stream is not — corrupt bytes, a body that inflates to a size the header
            // denies — therefore opens, and is refused where its body is met, at
            // resolve; the crc and trailer of the pair still answer to every byte.)
            // False leaves the object closed and untouched.
            bool open(const std::vector<std::uint8_t> &pack_bytes,
                      const std::vector<std::uint8_t> &idx_bytes);
            bool isOpen() const { return open_; }
            std::size_t count() const;

            // The full listing, in idx (oid) order.
            bool list(std::vector<PackObject> &out) const;

            // oid → (offset, crc, kind, size).
            bool lookup(const ObjectId &oid, PackObject &out) const;

            // Raw object bytes ("type size NUL content") resolved through delta
            // chains: an iterative walk with an explicit stack of at most kMaxDeltaChain
            // links; an ofs base out of range, a ref base that is not in the pack,
            // a chain past the cap, a delta whose bounds do not fit its base, or a
            // record that does not inflate to its declared size are all refusals.
            // The walk inflates as it goes — the full base first, then each chain
            // member's instructions on the way back up — so a resolve touches one
            // inflate per record it walks and none for the rest of the pack; the
            // chain's members are met in whatever order the pack holds them.
            bool resolveOid(const ObjectId &oid, std::vector<std::uint8_t> &raw) const;
            bool resolveAt(std::uint64_t offset, std::vector<std::uint8_t> &raw) const;

        private:
            bool open_ = false;
            std::vector<std::uint8_t> pack_;
            std::vector<PackObject> entries_; // idx order

            // One scanned record of the pack, in pack order. The scan states where a
            // record's bytes are — its header's span, bounded by the next record the
            // index names — and carries no inflated body: that stands up on demand,
            // once per walk, inside resolveAt.
            struct Record
            {
                std::uint64_t offset = 0;
                std::uint64_t dataStart = 0; // the deflated body begins here, just past the header
                std::uint64_t dataEnd = 0;   // one past the record's last byte
                RecordKind kind = RecordKind::Commit;
                std::uint64_t size = 0;
                std::uint64_t back = 0; // ofs base: offset of base = this record's offset - back
                std::string baseOid;    // lowercase hex, for ref delta bases
            };
            std::vector<Record> records_;
            std::vector<std::size_t> entryToRecord_; // parallel to entries_

            // offsets of records_, ascending, with the index into records_ beside
            // each: what the chain walk looks in.
            std::vector<std::uint64_t> offsets_sorted_;
            std::vector<std::size_t> at_offset_;
        };

        // ---- the pack writer (full objects only) ----

        // One object to write: its kind and its content (the bytes after the
        // "type size NUL" header — the framing is the writer's).
        struct LooseForPack
        {
            RecordKind kind = RecordKind::Blob;
            std::vector<std::uint8_t> content;
        };

        // The pack, in the order given: correct varint headers, deflated (stored)
        // bodies, per-record crc32s, and the trailer over all of it. entries_out
        // carries the pack-order index entries; the caller sorts them by oid (or
        // later by pack order for reuse) and hands them to writeIdx.
        //
        // Delta compression plugs in here: a repacking writer would choose bases,
        // emit OfsDelta/RefDelta records, and get their bodies from git's delta
        // algorithm. B2's scope is full objects, so none are emitted, and anything
        // delta in is refused.
        bool writePack(const std::vector<LooseForPack> &objects,
                       std::vector<std::uint8_t> &pack_out,
                       std::vector<IndexEntry> &entries_out);

    } // namespace gitcore
} // namespace ols

#endif // OLS_GITCORE_PACKFILE_H
