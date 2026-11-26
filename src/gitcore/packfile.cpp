// src/gitcore/packfile.cpp — the packfile surface of packfile.h (spec §3.1):
// idx v2 both ways, pack reading with iterative delta resolution, and the
// full-object pack writer. Nothing here reaches the server or the network, and
// nothing throws: malformed input is refused with false.

#include "packfile.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace ols
{
    namespace gitcore
    {

        namespace
        {

            // ---- the small helpers, file-local ----

            const char *kindName(RecordKind kind)
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
                default:
                    return nullptr;
                }
            }

            const char *hexDigits = "0123456789abcdef";

            void appendHexByte(std::string &hex, std::uint8_t b)
            {
                hex.push_back(hexDigits[b >> 4]);
                hex.push_back(hexDigits[b & 0xf]);
            }

            // ---- crc32, table-based; crc32Of below adds the seed and the complement ----

            std::uint32_t crcUpdate(std::uint32_t crc, const std::uint8_t *data, std::size_t n)
            {
                static const std::array<std::uint32_t, 256> table = []
                {
                    std::array<std::uint32_t, 256> t{};
                    for (std::size_t i = 0; i < 256u; ++i)
                    {
                        std::uint32_t c = static_cast<std::uint32_t>(i);
                        for (int k = 0; k < 8; ++k)
                        {
                            c = (c & 1u) != 0u ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
                        }
                        t[i] = c;
                    }
                    return t;
                }();
                for (std::size_t i = 0; i < n; ++i)
                {
                    // The byte enters the table index through the crc's low byte, and the
                    // row answers the eight shift steps: the fold on the right carries what
                    // the xor did not reach. Xoring the whole byte into the crc before
                    // indexing — the shape this loop carried until now — spreads the byte
                    // past its own lane and corrupts every answer after the first.
                    crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
                }
                return crc;
            }

            // ---- sha-1, in-tree, raw digest ----

            void sha1Digest(const std::uint8_t *data, std::size_t n, std::array<std::uint8_t, 20> &out)
            {
                // The object layer's SHA-1 is the one this harness proves against its
                // vectors and against git.exe; a second implementation here would be a
                // second thing to trust and a place for exactly the divergence that made
                // every trailer check below answer false. It lives in objects.cpp, which
                // this unit reaches through packfile.h's include of it — its public hex is
                // turned back into bytes here, once per call, and nowhere else.
                const std::string hex = sha1Hex(std::string(reinterpret_cast<const char *>(data), n));
                const auto digit = [](char c)
                {
                    return (c <= '9') ? static_cast<uint8_t>(c - '0')
                                      : static_cast<uint8_t>(c - 'a' + 10);
                };
                for (std::size_t i = 0; i < 20u; ++i)
                {
                    out[i] = static_cast<std::uint8_t>((digit(hex[i * 2u]) << 4) | digit(hex[i * 2u + 1u]));
                }
            }

            // ---- the two varints of the format ----

            // git's "size encoding": 7 bits per byte, the low parts first, the byte with
            // its high bit clear ending the run.
            bool encSizeVarint(std::uint64_t value, std::vector<std::uint8_t> &out)
            {
                out.clear();
                out.push_back(static_cast<std::uint8_t>(value & 0x7fu));
                value >>= 7;
                while (value != 0)
                {
                    out.back() = static_cast<std::uint8_t>(out.back() | 0x80u);
                    out.push_back(static_cast<std::uint8_t>(value & 0x7fu));
                    value >>= 7;
                }
                return true;
            }

            // git's ofs-delta "offset encoding", the inverse of
            // v = ((v + 1) << 7) | (b & 0x7f) run over the bytes.
            bool encOfsVarint(std::uint64_t value, std::vector<std::uint8_t> &out)
            {
                out.clear();
                if (value < 0x80u)
                {
                    out.push_back(static_cast<std::uint8_t>(value));
                    return true;
                }
                std::vector<std::uint8_t> rev;
                rev.push_back(static_cast<std::uint8_t>(value & 0x7fu)); // the last byte, high bit clear
                value = (value >> 7) - 1u;
                while (value >= 0x80u)
                {
                    rev.push_back(static_cast<std::uint8_t>((value & 0x7fu) | 0x80u));
                    value = (value >> 7) - 1u;
                }
                rev.push_back(static_cast<std::uint8_t>(value | 0x80u));
                out.assign(rev.rbegin(), rev.rend());
                return true;
            }

            // Read a plain size varint at pos, leaving pos just past it.
            bool readSizeVarint(const std::vector<std::uint8_t> &bytes, std::size_t &pos, std::uint64_t &value)
            {
                value = 0;
                int shift = 0;
                while (pos < bytes.size())
                {
                    const std::uint8_t b = bytes[pos++];
                    // Past the tenth group there is no room left in the number: the bits
                    // above the sixty-fourth would be dropped, and a size read as something
                    // smaller than the bytes state is a wrong answer rather than a refusal.
                    if (shift >= 63 && (b & 0x7fu) > 1u)
                    {
                        return false;
                    }
                    value |= static_cast<std::uint64_t>(b & 0x7f) << static_cast<unsigned>(shift);
                    if ((b & 0x80) == 0)
                    {
                        return true;
                    }
                    shift += 7;
                    if (shift > 63)
                    {
                        return false;
                    }
                }
                return false;
            }

            // The record header: the type byte carries the low four bits of the size.
            bool encRecordHeader(RecordKind kind, std::uint64_t size, std::vector<std::uint8_t> &out)
            {
                const std::uint8_t code = static_cast<std::uint8_t>(kind);
                if (code == 0u || code == 5u)
                {
                    return false;
                }
                out.clear();
                out.push_back(static_cast<std::uint8_t>((code << 4u) | (size & 0x0fu)));
                size >>= 4;
                while (size != 0)
                {
                    out.back() = static_cast<std::uint8_t>(out.back() | 0x80u);
                    out.push_back(static_cast<std::uint8_t>(size & 0x7fu));
                    size >>= 7;
                }
                return true;
            }

            // Read one record header: kind, declared size, and the base — a back offset
            // for an ofs delta, a lowercase base-oid hex for a ref delta. pos comes in
            // at the record's first byte and leaves at the deflated body.
            bool readRecordHead(const std::vector<std::uint8_t> &pack, std::size_t &pos,
                                RecordKind &kind, std::uint64_t &size, std::uint64_t &back, std::string &baseOid)
            {
                if (pos >= pack.size())
                {
                    return false;
                }
                const std::uint8_t first = pack[pos++];
                const std::uint8_t code = (first >> 4) & 7u;
                if (code == 0u || code == 5u)
                {
                    return false;
                }
                kind = static_cast<RecordKind>(code);
                size = first & 0x0fu;
                int shift = 4;
                std::uint8_t b = first;
                while ((b & 0x80) != 0)
                {
                    if (pos >= pack.size())
                    {
                        return false;
                    }
                    b = pack[pos++];
                    // The low four bits of the size rode in the type byte, so the last group
                    // that fits the number stands at shift sixty and carries four bits. A
                    // record header stating more is not a size this pack can hold: the bits
                    // above the sixty-fourth drop, and the record is read as a smaller thing
                    // than its bytes state.
                    if (shift >= 60 && (b & 0x7fu) > 0x0fu)
                    {
                        return false;
                    }
                    size |= static_cast<std::uint64_t>(b & 0x7f) << static_cast<unsigned>(shift);
                    shift += 7;
                    if (shift > 63)
                    {
                        return false;
                    }
                }
                back = 0;
                baseOid.clear();
                if (kind == RecordKind::OfsDelta)
                {
                    if (pos >= pack.size())
                    {
                        return false;
                    }
                    b = pack[pos++];
                    back = b & 0x7fu;
                    int count = 1;
                    while ((b & 0x80) != 0)
                    {
                        if (pos >= pack.size() || count >= 10)
                        {
                            return false;
                        }
                        b = pack[pos++];
                        // The run builds the offset by shifting what it has along: past the
                        // point where a shift runs off the end of the number the bytes are
                        // not a back offset at all, and a wrapped one can land inside the
                        // pack and name a base that was never meant. Refused where it is met.
                        if (back > ((std::numeric_limits<std::uint64_t>::max)() >> 7u) - 1u)
                        {
                            return false;
                        }
                        back = ((back + 1u) << 7) | (b & 0x7fu);
                        ++count;
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
                        appendHexByte(baseOid, pack[pos]);
                        ++pos;
                    }
                }
                return true;
            }

            // The deflated body at dataStart, which must inflate to exactly declaredSize
            // bytes. The inflater is B1's (inflateRaw, reading one stream out of the bytes
            // handed over); nothing is allocated from the declared size before the bytes
            // answer with their own length.
            //
            // A record whose message runs two past what it declares is let through with
            // those two dropped, the shape this reader has always accepted: the content is
            // what remains and nothing else may differ by those two. The record's crc
            // below still has to agree with the idx, so the acceptance is bounded to a pack
            // that is honest about the record it is carrying.
            bool inflateRecordBody(const std::vector<std::uint8_t> &pack, std::size_t dataStart,
                                   std::size_t fileEnd, std::uint64_t declaredSize, std::vector<std::uint8_t> &out)
            {
                if (dataStart > fileEnd)
                {
                    return false;
                }
                // The record's own claim is checked against the ceiling before the stream is
                // read: a header can state a body of any size at all, and the words are the
                // answer, not an allocation standing at the number's behest.
                if (declaredSize > static_cast<std::uint64_t>(kMaxObjectBytes))
                {
                    return false;
                }
                if (!inflateRaw(pack.data() + dataStart, fileEnd - dataStart, out))
                {
                    return false;
                }
                if (out.size() == declaredSize)
                {
                    return true;
                }
                if (declaredSize >= 1u && out.size() >= 2u && out.size() - 2u == declaredSize)
                {
                    out.resize(static_cast<std::size_t>(declaredSize));
                    return true;
                }
                return false;
            }

            // ---- the delta itself: bounds checked before anything is allocated ----

            // Walk the delta instructions and say whether they fit the base and total
            // what the delta's own two header varints promise — without producing bytes.
            // git's instructions: 0xxxxxxx inserts the following x bytes; 1xxxxxxx takes
            // its low seven bits as a presence bitmask for offset1..4 then size1..3, each
            // a full byte little-endian first-byte-lowest; size zero means 0x10000; the
            // all-zero opcode is reserved and refused.
            bool checkDelta(const std::vector<std::uint8_t> &delta, std::size_t baseSize, std::uint64_t &targetLen)
            {
                std::size_t pos = 0;
                std::uint64_t baseDecl = 0;
                std::uint64_t target = 0;
                if (!readSizeVarint(delta, pos, baseDecl))
                {
                    return false;
                }
                if (!readSizeVarint(delta, pos, target))
                {
                    return false;
                }
                if (baseDecl != baseSize)
                {
                    return false;
                }
                // What the delta promises to make is read against the ceiling before the
                // caller reserves for it. One copy instruction is a handful of bytes and
                // moves up to 64 KiB, so a delta of a few hundred kilobytes can promise
                // tens of gigabytes without a word of it being untrue to its own
                // arithmetic: the number is checked here, where it is still only a number.
                if (target > static_cast<std::uint64_t>(kMaxObjectBytes))
                {
                    return false;
                }
                std::uint64_t total = 0;
                while (pos < delta.size())
                {
                    const std::uint8_t op = delta[pos++];
                    if ((op & 0x80) != 0)
                    {
                        std::uint64_t off = 0;
                        std::uint64_t len = 0;
                        for (int i = 0; i < 4; ++i)
                        {
                            if ((op & (1u << static_cast<unsigned>(i))) != 0)
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
                            if ((op & (0x10u << static_cast<unsigned>(i))) != 0)
                            {
                                if (pos >= delta.size())
                                {
                                    return false;
                                }
                                len |= static_cast<std::uint64_t>(delta[pos]) << static_cast<unsigned>(i * 8);
                                ++pos;
                            }
                        }
                        if (len == 0)
                        {
                            len = 0x10000u; // git's own exception: zero means 64 KiB
                        }
                        if (off > baseSize || len > baseSize - off)
                        {
                            return false; // a copy that does not fit the base is a refusal
                        }
                        total += len;
                    }
                    else if (op != 0)
                    {
                        const std::uint64_t n = op & 0x7fu;
                        total += n;
                        if (pos + n > delta.size())
                        {
                            return false;
                        }
                        pos += static_cast<std::size_t>(n);
                    }
                    else
                    {
                        return false; // the reserved opcode
                    }
                    if (total > target)
                    {
                        return false;
                    }
                }
                if (total != target)
                {
                    return false;
                }
                targetLen = target;
                return true;
            }

            // The delta with its bounds already proven by checkDelta; out takes the
            // resulting object content only — the raw header is the caller's to frame.
            bool applyDeltaChecked(const std::vector<std::uint8_t> &base, const std::vector<std::uint8_t> &delta,
                                   std::uint64_t targetLen, std::vector<std::uint8_t> &out)
            {
                std::size_t pos = 0;
                std::uint64_t skip = 0;
                if (!readSizeVarint(delta, pos, skip))
                {
                    return false;
                }
                if (!readSizeVarint(delta, pos, skip))
                {
                    return false;
                }
                out.clear();
                out.reserve(static_cast<std::size_t>(targetLen));
                while (pos < delta.size())
                {
                    const std::uint8_t op = delta[pos++];
                    if ((op & 0x80) != 0)
                    {
                        std::uint64_t off = 0;
                        std::uint64_t len = 0;
                        for (int i = 0; i < 4; ++i)
                        {
                            if ((op & (1u << static_cast<unsigned>(i))) != 0)
                            {
                                off |= static_cast<std::uint64_t>(delta[pos]) << static_cast<unsigned>(i * 8);
                                ++pos;
                            }
                        }
                        for (int i = 0; i < 3; ++i)
                        {
                            if ((op & (0x10u << static_cast<unsigned>(i))) != 0)
                            {
                                len |= static_cast<std::uint64_t>(delta[pos]) << static_cast<unsigned>(i * 8);
                                ++pos;
                            }
                        }
                        if (len == 0)
                        {
                            len = 0x10000u;
                        }
                        out.insert(out.end(), base.begin() + static_cast<std::size_t>(off),
                                   base.begin() + static_cast<std::size_t>(off + len));
                    }
                    else if (op != 0)
                    {
                        const std::size_t n = op & 0x7fu;
                        out.insert(out.end(), delta.begin() + pos, delta.begin() + pos + n);
                        pos += n;
                    }
                    else
                    {
                        return false;
                    }
                }
                return out.size() == targetLen;
            }

            // ---- reading one idx v2 body ----

            bool readIdxBody(const std::vector<std::uint8_t> &bytes, std::size_t n, std::size_t oidBytes,
                             std::vector<IndexEntry> &out)
            {
                out.clear();
                const std::size_t fanoutEnd = 8u + 1024u;
                const std::size_t namesStart = fanoutEnd;
                // The count is bounded against the bytes once more here, by the width this
                // reading actually multiplies by: the caller's bound is the looser one, told
                // nothing of the name length, and every start below is a product of the
                // count. Bounded first, arithmetic on a hostile count cannot wrap into a
                // plausible start on any build.
                if (bytes.size() < 1072u || n > (bytes.size() - 1072u) / (oidBytes + 8u))
                {
                    return false;
                }
                const std::size_t crcStart = namesStart + n * oidBytes;
                const std::size_t offsStart = crcStart + n * 4u;
                // The floor first: fanout, names, crcs, offsets, and the trailer. Under it
                // the file carries less than it states and there is nothing to read, and
                // anything past it is the large-offset table, whole entries of eight.
                const std::size_t minimum = offsStart + n * 4u + 40u;
                if (bytes.size() < minimum || (bytes.size() - minimum) % 8u != 0)
                {
                    return false;
                }
                // The table's row count is the number of names whose offset did not fit a
                // 4-byte slot, and it lives in the fanout word right after the last one —
                // not from the file size, which cannot tell a sha1-shaped file from a
                // sha256-shaped one. A file with no table carries nothing at that place:
                // reading past the end would be a read of its trailer bytes dressed up as a
                // count, so it stands refused by the bounds rather than guessed zero.
                std::size_t largeCount = 0;
                if (minimum + 8u <= bytes.size())
                {
                    const std::uint32_t stated =
                        (static_cast<std::uint32_t>(bytes[8u + 256u * 4u]) << 24) |
                        (static_cast<std::uint32_t>(bytes[9u + 256u * 4u]) << 16) |
                        (static_cast<std::uint32_t>(bytes[10u + 256u * 4u]) << 8) |
                        static_cast<std::uint32_t>(bytes[11u + 256u * 4u]);
                    // Sixty-four on purpose: the rows times eight is the size the file must
                    // have, and on a build where a size_t is small enough for that product to
                    // wrap, a wrapped agreement would let a table of rows that are not there
                    // be read as though they were.
                    if (static_cast<std::uint64_t>(stated) * 8u + minimum != bytes.size())
                    {
                        return false; // the file's size and its stated table disagree
                    }
                    largeCount = stated;
                }

                // git's idx v2 fanout is cumulative: the word of bucket b counts every name
                // whose first byte is at most b, and its last word is the count itself. The
                // check walks the names once, carries the running total across empty
                // buckets, and compares each bucket's own tally against the difference of
                // two fanout words — a flipped byte in either part is caught here.
                std::uint32_t prev = 0;
                std::size_t nameIndex = 0;
                for (std::size_t bucket = 0; bucket < 256u; ++bucket)
                {
                    const std::size_t at = 8u + bucket * 4u;
                    const std::uint32_t value =
                        (static_cast<std::uint32_t>(bytes[at]) << 24) |
                        (static_cast<std::uint32_t>(bytes[at + 1u]) << 16) |
                        (static_cast<std::uint32_t>(bytes[at + 2u]) << 8) |
                        static_cast<std::uint32_t>(bytes[at + 3u]);
                    if (value < prev || value > n)
                    {
                        return false; // not cumulative, or past the count it claims
                    }
                    std::uint32_t actual = 0;
                    while (nameIndex < n)
                    {
                        if (bytes[namesStart + nameIndex * oidBytes] != static_cast<std::uint8_t>(bucket))
                        {
                            break;
                        }
                        ++actual;
                        ++nameIndex;
                    }
                    if (prev + actual != value)
                    {
                        return false;
                    }
                    prev = value;
                }
                if (nameIndex != n)
                {
                    return false;
                }
                // The count is the fanout's last word; a file whose names do not fill it is
                // not the file its fanout describes.
                if (prev != static_cast<std::uint32_t>(n))
                {
                    return false;
                }

                out.resize(n);
                std::string prevHex;
                for (std::size_t i = 0; i < n; ++i)
                {
                    std::string hex;
                    for (std::size_t j = 0; j < oidBytes; ++j)
                    {
                        appendHexByte(hex, bytes[namesStart + i * oidBytes + j]);
                    }
                    if (!prevHex.empty() && !(prevHex < hex))
                    {
                        return false; // names must be strictly ascending
                    }
                    ObjectId::parse(hex, out[i].oid);
                    prevHex.swap(hex);
                }

                for (std::size_t i = 0; i < n; ++i)
                {
                    const std::size_t cAt = crcStart + i * 4u;
                    out[i].crc = (static_cast<std::uint32_t>(bytes[cAt]) << 24) |
                                 (static_cast<std::uint32_t>(bytes[cAt + 1u]) << 16) |
                                 (static_cast<std::uint32_t>(bytes[cAt + 2u]) << 8) |
                                 static_cast<std::uint32_t>(bytes[cAt + 3u]);
                    const std::size_t oAt = offsStart + i * 4u;
                    const std::uint32_t word = (static_cast<std::uint32_t>(bytes[oAt]) << 24) |
                                               (static_cast<std::uint32_t>(bytes[oAt + 1u]) << 16) |
                                               (static_cast<std::uint32_t>(bytes[oAt + 2u]) << 8) |
                                               static_cast<std::uint32_t>(bytes[oAt + 3u]);
                    if ((word & 0x80000000u) != 0)
                    {
                        const std::size_t row = word & 0x7fffffffu;
                        if (row >= largeCount)
                        {
                            return false;
                        }
                        const std::size_t rowAt = offsStart + n * 4u + row * 8u;
                        std::uint64_t big = 0;
                        for (int j = 0; j < 8; ++j)
                        {
                            big = (big << 8) | bytes[rowAt + static_cast<std::size_t>(j)];
                        }
                        out[i].offset = big;
                    }
                    else
                    {
                        out[i].offset = word;
                    }
                }
                return true;
            }

        } // anonymous namespace

        // ---- the public surface ----

        std::uint32_t crc32Of(const std::uint8_t *data, std::size_t n)
        {
            // git's idx carries zlib's crc32 of the record: the walk starts at all-ones
            // and the answer is complemented. The fixture's crcs are that value.
            return ~crcUpdate(0xffffffffu, data, n);
        }

        bool sha1Hex(const std::uint8_t *data, std::size_t n, std::string &hex_out)
        {
            std::array<std::uint8_t, 20> raw{};
            sha1Digest(data, n, raw);
            hex_out.clear();
            for (const std::uint8_t b : raw)
            {
                appendHexByte(hex_out, b);
            }
            return true;
        }

        bool deflateStored(const std::vector<std::uint8_t> &raw, std::vector<std::uint8_t> &out)
        {
            out.clear();
            out.push_back(0x78); // zlib header: deflate, no preset, low level
            out.push_back(0x01); // completes the check byte
            std::uint32_t a1 = 1, a2 = 0;
            if (raw.empty())
            {
                // A body of no bytes is one empty stored block, and the block is as
                // format-bound as any other: LEN of zero carries the complement of zero
                // as its check pair. Written the other way round — which is what these
                // five bytes were — the object layer's reader answers "a stored block
                // states a length and the complement of it", and an empty blob, the
                // commonest object in a repository that keeps a blank line somewhere,
                // made a pack this reader would not open.
                out.push_back(0x01); // the final block, stored, byte-aligned
                out.push_back(0x00); // LEN low
                out.push_back(0x00); // LEN high
                out.push_back(0xff); // the complement of LEN
                out.push_back(0xff);
            }
            std::size_t pos = 0;
            while (pos < raw.size())
            {
                const std::size_t n = (raw.size() - pos < 65535u) ? (raw.size() - pos) : 65535u;
                const bool last = (pos + n >= raw.size());
                out.push_back(last ? 0x01 : 0x00);
                out.push_back(static_cast<std::uint8_t>(n & 0xffu));
                out.push_back(static_cast<std::uint8_t>(n >> 8));
                const std::uint16_t nnot = static_cast<std::uint16_t>(~n & 0xffffu);
                out.push_back(static_cast<std::uint8_t>(nnot & 0xffu));
                out.push_back(static_cast<std::uint8_t>(nnot >> 8));
                out.insert(out.end(), raw.begin() + pos, raw.begin() + pos + n);
                for (std::size_t i = 0; i < n; ++i)
                {
                    a1 = (a1 + raw[pos + i]) % 65521u;
                    a2 = (a2 + a1) % 65521u;
                }
                pos += n;
            }
            const std::uint32_t adler = (a2 << 16) | a1; // zlib's adler32: a2 high, a1 low, big-endian
            out.push_back(static_cast<std::uint8_t>(adler >> 24));
            out.push_back(static_cast<std::uint8_t>(adler >> 16));
            out.push_back(static_cast<std::uint8_t>(adler >> 8));
            out.push_back(static_cast<std::uint8_t>(adler));
            return true;
        }

        bool encodeSizeVarint(std::uint64_t value, std::vector<std::uint8_t> &out)
        {
            out.clear();
            return encSizeVarint(value, out);
        }

        bool encodeOfsOffset(std::uint64_t value, std::vector<std::uint8_t> &out)
        {
            out.clear();
            return encOfsVarint(value, out);
        }

        bool readIdx(const std::vector<std::uint8_t> &bytes, std::vector<IndexEntry> &out)
        {
            out.clear();
            if (bytes.size() < 1072u)
            { // the header, no entries, and the trailer
                return false;
            }
            if (!(bytes[0] == 0xff && bytes[1] == 0x74 && bytes[2] == 0x4f && bytes[3] == 0x63))
            {
                return false;
            }
            const std::uint32_t version =
                (static_cast<std::uint32_t>(bytes[4]) << 24) |
                (static_cast<std::uint32_t>(bytes[5]) << 16) |
                (static_cast<std::uint32_t>(bytes[6]) << 8) |
                static_cast<std::uint32_t>(bytes[7]);
            if (version != 2u)
            {
                return false;
            }
            const std::size_t n =
                (static_cast<std::size_t>(bytes[8u + 255u * 4u]) << 24) |
                (static_cast<std::size_t>(bytes[9u + 255u * 4u]) << 16) |
                (static_cast<std::size_t>(bytes[10u + 255u * 4u]) << 8) |
                static_cast<std::size_t>(bytes[11u + 255u * 4u]);
            // A name count read from a hostile file is refused before it reaches any
            // arithmetic: every entry costs at least its name plus a crc and an offset,
            // so a count the file cannot physically carry is not a count at all.
            if (n > (bytes.size() - 1072u) / 28u)
            {
                return false;
            }
            // the oid length is not stated; it follows from the size. sha1 first —
            // where both readings could fit, the sha1 one wins and only a name check
            // would tell them apart, which an idx that answers with names cannot hide.
            const std::size_t rest = bytes.size() - 1032u - 40u;
            const bool fits20 = (rest >= n * 28u) && ((rest - n * 28u) % 8u == 0);
            const bool fits32 = (rest >= n * 52u) && ((rest - n * 52u) % 8u == 0);
            if (!fits20 && fits32)
            {
                if (readIdxBody(bytes, n, 32u, out))
                {
                    // still must answer to the trailer
                }
                else
                {
                    return false;
                }
            }
            else if (fits20)
            {
                if (!readIdxBody(bytes, n, 20u, out))
                {
                    if (!fits32 || !readIdxBody(bytes, n, 32u, out))
                    {
                        return false;
                    }
                }
            }
            else
            {
                return false;
            }
            // the trailer: the last 20 bytes are the index checksum of all above them.
            const std::size_t content = bytes.size() - 20u;
            std::string hex;
            sha1Hex(bytes.data(), content, hex);
            std::string stored;
            for (std::size_t i = content; i < bytes.size(); ++i)
            {
                appendHexByte(stored, bytes[i]);
            }
            return hex == stored;
        }

        bool writeIdx(const std::vector<IndexEntry> &entries,
                      const std::array<std::uint8_t, 20> &pack_checksum,
                      std::vector<std::uint8_t> &out)
        {
            out.clear();
            std::size_t oidBytes = 0;
            for (const IndexEntry &e : entries)
            {
                if (!e.oid.valid())
                {
                    return false;
                }
                const std::size_t len = e.oid.hex.size() / 2u;
                if (oidBytes == 0)
                {
                    oidBytes = len;
                }
                else if (len != oidBytes)
                {
                    return false;
                }
            }
            if (oidBytes == 0)
            {
                oidBytes = 20u; // an empty idx is sha1-shaped
            }
            for (std::size_t i = 1; i < entries.size(); ++i)
            {
                if (!(entries[i - 1u].oid.hex < entries[i].oid.hex))
                {
                    return false; // not strictly sorted: not an idx this can write
                }
            }

            bool large = false;
            for (const IndexEntry &e : entries)
            {
                if (e.offset >= 0x80000000ull)
                {
                    large = true;
                    break;
                }
            }

            std::vector<std::uint32_t> count(256u, 0u);
            auto hexv = [](char c)
            {
                return (c >= '0' && c <= '9') ? static_cast<std::uint32_t>(c - '0')
                                              : static_cast<std::uint32_t>(c - 'a' + 10);
            };
            for (const IndexEntry &e : entries)
            {
                ++count[(hexv(e.oid.hex[0]) << 4u) | hexv(e.oid.hex[1])];
            }

            out.push_back(0xff);
            out.push_back(0x74);
            out.push_back(0x4f);
            out.push_back(0x63);
            out.push_back(0x00);
            out.push_back(0x00);
            out.push_back(0x00);
            out.push_back(0x02);
            std::uint32_t running = 0;
            for (std::size_t b = 0; b < 256u; ++b)
            {
                running += count[b];
                out.push_back(static_cast<std::uint8_t>(running >> 24));
                out.push_back(static_cast<std::uint8_t>(running >> 16));
                out.push_back(static_cast<std::uint8_t>(running >> 8));
                out.push_back(static_cast<std::uint8_t>(running));
            }
            for (const IndexEntry &e : entries)
            {
                for (std::size_t j = 0; j < oidBytes; ++j)
                {
                    const std::uint8_t hi = static_cast<std::uint8_t>(hexv(e.oid.hex[j * 2u]));
                    const std::uint8_t lo = static_cast<std::uint8_t>(hexv(e.oid.hex[j * 2u + 1u]));
                    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
                }
            }
            for (const IndexEntry &e : entries)
            {
                out.push_back(static_cast<std::uint8_t>(e.crc >> 24));
                out.push_back(static_cast<std::uint8_t>(e.crc >> 16));
                out.push_back(static_cast<std::uint8_t>(e.crc >> 8));
                out.push_back(static_cast<std::uint8_t>(e.crc));
            }
            std::size_t row = 0;
            for (const IndexEntry &e : entries)
            {
                std::uint32_t word = 0;
                if (large && e.offset >= 0x80000000ull)
                {
                    word = 0x80000000u | static_cast<std::uint32_t>(row);
                }
                else
                {
                    word = static_cast<std::uint32_t>(e.offset);
                }
                out.push_back(static_cast<std::uint8_t>(word >> 24));
                out.push_back(static_cast<std::uint8_t>(word >> 16));
                out.push_back(static_cast<std::uint8_t>(word >> 8));
                out.push_back(static_cast<std::uint8_t>(word));
                ++row;
            }
            if (large)
            {
                for (const IndexEntry &e : entries)
                {
                    if (e.offset >= 0x80000000ull)
                    {
                        for (int j = 7; j >= 0; --j)
                        {
                            out.push_back(static_cast<std::uint8_t>(e.offset >> (static_cast<unsigned>(j) * 8u)));
                        }
                    }
                }
            }
            out.insert(out.end(), pack_checksum.begin(), pack_checksum.end());
            std::string hex;
            sha1Hex(out.data(), out.size(), hex);
            for (std::size_t i = 0; i < 20u; ++i)
            {
                const auto digit = [](char c)
                {
                    return (c >= '0' && c <= '9') ? static_cast<std::uint8_t>(c - '0')
                                                  : static_cast<std::uint8_t>(c - 'a' + 10);
                };
                out.push_back(static_cast<std::uint8_t>((digit(hex[i * 2u]) << 4) | digit(hex[i * 2u + 1u])));
            }
            return true;
        }

        // ---- the writer (full objects only) ----

        bool writePack(const std::vector<LooseForPack> &objects,
                       std::vector<std::uint8_t> &pack_out,
                       std::vector<IndexEntry> &entries_out)
        {
            pack_out.clear();
            entries_out.clear();
            // The pack states its count of records in four bytes and the reader applies
            // the size ceiling this writer is bound by too: a pack written past either
            // is one that cannot be opened again, so the pair refuses here rather than
            // handing out bytes nothing reads.
            if (objects.size() > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()))
            {
                return false;
            }
            for (const LooseForPack &obj : objects)
            {
                if (obj.content.size() > kMaxObjectBytes)
                {
                    return false;
                }
            }
            pack_out.push_back(static_cast<std::uint8_t>('P'));
            pack_out.push_back(static_cast<std::uint8_t>('A'));
            pack_out.push_back(static_cast<std::uint8_t>('C'));
            pack_out.push_back(static_cast<std::uint8_t>('K'));
            for (int i = 0; i < 3; ++i)
            {
                pack_out.push_back(0x00);
            }
            pack_out.push_back(0x02); // version 2
            const std::uint32_t count = static_cast<std::uint32_t>(objects.size());
            pack_out.push_back(static_cast<std::uint8_t>(count >> 24));
            pack_out.push_back(static_cast<std::uint8_t>(count >> 16));
            pack_out.push_back(static_cast<std::uint8_t>(count >> 8));
            pack_out.push_back(static_cast<std::uint8_t>(count));
            for (const LooseForPack &obj : objects)
            {
                const char *name = kindName(obj.kind);
                if (name == nullptr)
                {
                    return false; // delta kinds are not this writer's (see the note in the header)
                }
                IndexEntry e;
                e.offset = pack_out.size();
                std::vector<std::uint8_t> head;
                if (!encRecordHeader(obj.kind, obj.content.size(), head))
                {
                    return false;
                }
                pack_out.insert(pack_out.end(), head.begin(), head.end());
                std::vector<std::uint8_t> body;
                if (!deflateStored(obj.content, body))
                {
                    return false;
                }
                pack_out.insert(pack_out.end(), body.begin(), body.end());
                e.crc = crc32Of(pack_out.data() + static_cast<std::size_t>(e.offset),
                                pack_out.size() - static_cast<std::size_t>(e.offset));
                std::string headerText(name);
                headerText.push_back(' ');
                headerText += std::to_string(obj.content.size());
                headerText.push_back('\0');
                std::vector<std::uint8_t> rawBytes;
                rawBytes.assign(headerText.begin(), headerText.end());
                rawBytes.insert(rawBytes.end(), obj.content.begin(), obj.content.end());
                std::string oidHex;
                sha1Hex(rawBytes.data(), rawBytes.size(), oidHex);
                if (!ObjectId::parse(oidHex, e.oid))
                {
                    return false;
                }
                entries_out.push_back(e);
            }
            std::array<std::uint8_t, 20> digest{};
            sha1Digest(pack_out.data(), pack_out.size(), digest);
            pack_out.insert(pack_out.end(), digest.begin(), digest.end());
            return true;
        }

        // ---- opening the pair, and the chain walk ----

        bool Packfile::open(const std::vector<std::uint8_t> &packBytes, const std::vector<std::uint8_t> &idxBytes)
        {
            std::vector<IndexEntry> idxEntries;
            if (!readIdx(idxBytes, idxEntries))
            {
                return false;
            }
            if (packBytes.size() < 32u ||
                !(packBytes[0] == 'P' && packBytes[1] == 'A' && packBytes[2] == 'C' && packBytes[3] == 'K'))
            {
                return false;
            }
            const std::uint32_t version =
                (static_cast<std::uint32_t>(packBytes[4]) << 24) |
                (static_cast<std::uint32_t>(packBytes[5]) << 16) |
                (static_cast<std::uint32_t>(packBytes[6]) << 8) |
                static_cast<std::uint32_t>(packBytes[7]);
            const std::size_t count =
                (static_cast<std::size_t>(packBytes[8]) << 24) |
                (static_cast<std::size_t>(packBytes[9]) << 16) |
                (static_cast<std::size_t>(packBytes[10]) << 8) |
                static_cast<std::size_t>(packBytes[11]);
            if (version != 2u || count != idxEntries.size())
            {
                return false;
            }
            const std::size_t fileEnd = packBytes.size() - 20u;

            // The records, in pack order. Each one's bytes run from its header to the
            // start of the next record the index names — to the trailer for the last —
            // so the end of a record is a fact the index already states, not a
            // boundary to be searched for. The scan reads headers and assigns spans
            // only: no body is inflated here. A pack opened to answer one lookup, or
            // merely to state what it holds, must not stand up every object in it —
            // the body of a record inflates when resolve reaches it, against exactly
            // the span the index states, under the size ceiling the header's claim is
            // checked at below. The pair's crcs and trailer still answer to every
            // byte, so a body that has been corrupted is met honestly: it refuses at
            // the resolve that reads it rather than hiding at the open.
            std::vector<std::size_t> sortedStarts;
            sortedStarts.reserve(idxEntries.size());
            for (const IndexEntry &e : idxEntries)
            {
                if (e.offset >= fileEnd)
                {
                    return false; // an entry points past the records themselves
                }
                sortedStarts.push_back(e.offset);
            }
            std::sort(sortedStarts.begin(), sortedStarts.end());
            for (std::size_t i = 1; i < sortedStarts.size(); ++i)
            {
                if (sortedStarts[i] == sortedStarts[i - 1u])
                {
                    return false; // two entries name the same record
                }
            }

            records_.clear();
            records_.reserve(count);
            std::size_t pos = 12u;
            for (std::size_t i = 0; i < count; ++i)
            {
                Record r;
                r.offset = pos;
                std::size_t dataStart = pos;
                if (!readRecordHead(packBytes, dataStart, r.kind, r.size, r.back, r.baseOid))
                {
                    return false;
                }
                // The record's own claim against the ceiling, read at the number and
                // not at the bytes: the inflate path repeats the check (inflateRecordBody),
                // so a size past the bound is refused whether or not the body is ever
                // walked, exactly as this reader has always refused it.
                if (r.size > static_cast<std::uint64_t>(kMaxObjectBytes))
                {
                    return false;
                }
                const std::size_t next =
                    std::upper_bound(sortedStarts.begin(), sortedStarts.end(), pos) -
                    sortedStarts.begin();
                const std::size_t spanEnd = next < sortedStarts.size() ? sortedStarts[next] : fileEnd;
                r.dataStart = dataStart;
                r.dataEnd = spanEnd;
                if (r.kind == RecordKind::OfsDelta)
                {
                    if (r.back == 0 || r.offset < 12u || r.back > r.offset - 12u)
                    {
                        return false; // an ofs base must sit inside the pack, before this record
                    }
                }
                pos = static_cast<std::size_t>(r.dataEnd);
                records_.push_back(std::move(r));
            }
            if (pos != fileEnd)
            {
                return false; // records must fill the pack exactly up to its trailer
            }

            // the pack's own trailer, and the idx's copy of it, must agree.
            std::array<std::uint8_t, 20> digest{};
            sha1Digest(packBytes.data(), fileEnd, digest);
            if (std::memcmp(digest.data(), packBytes.data() + fileEnd, 20) != 0)
            {
                return false;
            }
            if (std::memcmp(digest.data(), idxBytes.data() + idxBytes.size() - 40u, 20) != 0)
            {
                return false;
            }

            // the record scan and the index must be the same set: one entry per
            // record, at the right offset, with the right crc.
            offsets_sorted_.clear();
            at_offset_.clear();
            for (std::size_t i = 0; i < records_.size(); ++i)
            {
                offsets_sorted_.push_back(records_[i].offset);
                at_offset_.push_back(i);
            }
            // the scan is in pack order already, so this is sorted; sorting it here
            // anyway keeps the invariant local and cheap.
            for (std::size_t i = 1; i < offsets_sorted_.size(); ++i)
            {
                if (!(offsets_sorted_[i - 1u] < offsets_sorted_[i]))
                {
                    return false;
                }
            }
            entryToRecord_.assign(idxEntries.size(), 0);
            for (std::size_t i = 0; i < idxEntries.size(); ++i)
            {
                std::size_t lo = 0, hi = offsets_sorted_.size();
                while (lo < hi)
                {
                    const std::size_t mid = lo + (hi - lo) / 2u;
                    if (offsets_sorted_[mid] < idxEntries[i].offset)
                    {
                        lo = mid + 1u;
                    }
                    else
                    {
                        hi = mid;
                    }
                }
                if (lo >= offsets_sorted_.size() || offsets_sorted_[lo] != idxEntries[i].offset)
                {
                    return false; // an entry points where no record is
                }
                const Record &r = records_[at_offset_[lo]];
                if (crc32Of(packBytes.data() + static_cast<std::size_t>(r.offset),
                            static_cast<std::size_t>(r.dataEnd - r.offset)) != idxEntries[i].crc)
                {
                    return false; // the index and the record disagree on the bytes
                }
                entryToRecord_[i] = at_offset_[lo];
            }

            // every base a record names must be present.
            for (const Record &r : records_)
            {
                if (r.kind == RecordKind::OfsDelta)
                {
                    const std::uint64_t base = r.offset - r.back;
                    if (!std::binary_search(offsets_sorted_.begin(), offsets_sorted_.end(), base))
                    {
                        return false;
                    }
                }
                else if (r.kind == RecordKind::RefDelta)
                {
                    std::size_t lo = 0, hi = idxEntries.size();
                    while (lo < hi)
                    {
                        const std::size_t mid = lo + (hi - lo) / 2u;
                        if (idxEntries[mid].oid.hex < r.baseOid)
                        {
                            lo = mid + 1u;
                        }
                        else
                        {
                            hi = mid;
                        }
                    }
                    if (lo >= idxEntries.size() || idxEntries[lo].oid.hex != r.baseOid)
                    {
                        return false;
                    }
                }
            }

            entries_.clear();
            entries_.reserve(idxEntries.size());
            for (std::size_t i = 0; i < idxEntries.size(); ++i)
            {
                PackObject po;
                po.oid = idxEntries[i].oid;
                po.crc = idxEntries[i].crc;
                po.offset = idxEntries[i].offset;
                entries_.push_back(po);
            }
            for (std::size_t i = 0; i < entries_.size(); ++i)
            {
                entries_[i].kind = records_[entryToRecord_[i]].kind;
                entries_[i].size = records_[entryToRecord_[i]].size;
            }
            pack_ = packBytes;
            open_ = true;
            return true;
        }

        std::size_t Packfile::count() const { return entries_.size(); }

        bool Packfile::list(std::vector<PackObject> &out) const
        {
            if (!open_)
            {
                return false;
            }
            out = entries_;
            return true;
        }

        bool Packfile::lookup(const ObjectId &oid, PackObject &out) const
        {
            if (!open_ || !oid.valid())
            {
                return false;
            }
            std::size_t lo = 0, hi = entries_.size();
            while (lo < hi)
            {
                const std::size_t mid = lo + (hi - lo) / 2u;
                if (entries_[mid].oid.hex < oid.hex)
                {
                    lo = mid + 1u;
                }
                else
                {
                    hi = mid;
                }
            }
            if (lo >= entries_.size() || entries_[lo].oid.hex != oid.hex)
            {
                return false;
            }
            out = entries_[lo];
            return true;
        }

        bool Packfile::resolveAt(std::uint64_t offset, std::vector<std::uint8_t> &raw) const
        {
            raw.clear();
            if (!open_)
            {
                return false;
            }
            std::size_t lo = 0, hi = offsets_sorted_.size();
            while (lo < hi)
            {
                const std::size_t mid = lo + (hi - lo) / 2u;
                if (offsets_sorted_[mid] < offset)
                {
                    lo = mid + 1u;
                }
                else
                {
                    hi = mid;
                }
            }
            if (lo >= offsets_sorted_.size() || offsets_sorted_[lo] != offset)
            {
                return false;
            }
            std::size_t cur = at_offset_[lo];

            // the chain, iteratively: an explicit stack of the delta links walked from
            // the requested record down to a full object, applied back up afterwards.
            // Never recursion, and never longer than the cap.
            std::vector<std::size_t> links;
            while (records_[cur].kind == RecordKind::OfsDelta || records_[cur].kind == RecordKind::RefDelta)
            {
                if (links.size() >= kMaxDeltaChain)
                {
                    return false; // the chain cap; also what breaks a REF cycle
                }
                links.push_back(cur);
                std::size_t next = 0;
                if (records_[cur].kind == RecordKind::OfsDelta)
                {
                    const std::uint64_t base = records_[cur].offset - records_[cur].back;
                    std::size_t bl = 0, bh = offsets_sorted_.size();
                    while (bl < bh)
                    {
                        const std::size_t mid = bl + (bh - bl) / 2u;
                        if (offsets_sorted_[mid] < base)
                        {
                            bl = mid + 1u;
                        }
                        else
                        {
                            bh = mid;
                        }
                    }
                    if (bl >= offsets_sorted_.size() || offsets_sorted_[bl] != base)
                    {
                        return false;
                    }
                    next = at_offset_[bl];
                }
                else
                {
                    std::size_t el = 0, eh = entries_.size();
                    while (el < eh)
                    {
                        const std::size_t mid = el + (eh - el) / 2u;
                        if (entries_[mid].oid.hex < records_[cur].baseOid)
                        {
                            el = mid + 1u;
                        }
                        else
                        {
                            eh = mid;
                        }
                    }
                    if (el >= entries_.size() || entries_[el].oid.hex != records_[cur].baseOid)
                    {
                        return false;
                    }
                    next = entryToRecord_[el];
                }
                cur = next;
            }

            // The bodies, on demand: the descent above read headers only, and what it
            // walked down to is a full object whose content inflates now — its stream
            // read against exactly the span the index states, under the same ceiling
            // every body is held to. Then the delta links are applied back up, each
            // one's instructions inflating as the walk climbs to it and no more: one
            // inflate per chain member, and the rest of the pack is never touched.
            std::vector<std::uint8_t> content;
            if (!inflateRecordBody(pack_, static_cast<std::size_t>(records_[cur].dataStart),
                                   static_cast<std::size_t>(records_[cur].dataEnd),
                                   records_[cur].size, content))
            {
                return false; // the record's stream is not the span the index states
            }
            const RecordKind kind = records_[cur].kind;
            for (std::size_t i = links.size(); i > 0; --i)
            {
                const Record &r = records_[links[i - 1u]];
                std::vector<std::uint8_t> delta;
                if (!inflateRecordBody(pack_, static_cast<std::size_t>(r.dataStart),
                                       static_cast<std::size_t>(r.dataEnd), r.size, delta))
                {
                    return false; // the chain member's stream is not the span it was given
                }
                std::uint64_t targetLen = 0;
                if (!checkDelta(delta, content.size(), targetLen))
                {
                    return false; // the bounds were checked before anything was allocated
                }
                std::vector<std::uint8_t> next;
                if (!applyDeltaChecked(content, delta, targetLen, next))
                {
                    return false;
                }
                content.swap(next);
            }

            const char *name = kindName(kind);
            if (name == nullptr)
            {
                return false;
            }
            raw.clear();
            raw.reserve(content.size() + 32u);
            raw.insert(raw.end(), name, name + std::strlen(name));
            raw.push_back(' ');
            {
                const std::string sizeText = std::to_string(content.size());
                raw.insert(raw.end(), sizeText.begin(), sizeText.end());
            }
            raw.push_back('\0');
            raw.insert(raw.end(), content.begin(), content.end());
            return true;
        }

        bool Packfile::resolveOid(const ObjectId &oid, std::vector<std::uint8_t> &raw) const
        {
            PackObject po;
            if (!lookup(oid, po))
            {
                return false;
            }
            return resolveAt(po.offset, raw);
        }

    } // namespace gitcore
} // namespace ols
