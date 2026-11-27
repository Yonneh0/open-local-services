// test_packfile.cpp — the packfile unit of GitCore: spec §3.1's read side
// against fixtures git.exe wrote, and the write side against its own output.
//
// The fixtures (tests/gitcore/fixtures/packed/) carry a pack and its idx from a
// real repository; loose-manifest.json names every object in it with type and
// size, so resolution is checked against what git stated, not against this
// library's reading of itself. The harness pulls this file into main.cpp (see
// the note there); CHECK macros and ols_test::fixtures_root come from there.

#include "gitcore/packfile.h"
#include "gitcore/objects.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ols::gitcore;

bool readWhole(const std::string& path, std::vector<std::uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// The fixture pair: the pack/idx names carry a sha, so they are found by scan.
// Returns the .pack path, empty when the folder holds none; the idx stands
// beside it under its own extension.
std::string findFixturePack()
{
    std::error_code ec;
    std::filesystem::directory_iterator it(ols_test::fixtures_root + "/packed", ec);
    if (ec) {
        return std::string();
    }
    for (;;) {
        if (it->path().extension() == ".pack") {
            return it->path().string();
        }
        it.increment(ec);
        if (ec) {
            return std::string();
        }
    }
}

std::string idxFor(const std::string& packPath)
{
    return packPath.substr(0, packPath.size() - 4u) + "idx";
}

std::string fromBytes(const std::vector<std::uint8_t>& v)
{
    return std::string(v.begin(), v.end());
}

// sha1 of raw bytes as its twenty bytes, through the object layer's hex.
std::array<std::uint8_t, 20> sha1BytesOf(const std::vector<std::uint8_t>& v)
{
    const std::string hex = sha1Hex(fromBytes(v));
    std::array<std::uint8_t, 20> raw{};
    const auto digit = [](char c) { return (c <= '9') ? (c - '0') : (c - 'a' + 10); };
    for (std::size_t i = 0; i < 20u; ++i) {
        raw[i] = static_cast<std::uint8_t>((digit(hex[i * 2u]) << 4) | digit(hex[i * 2u + 1u]));
    }
    return raw;
}

// A record written by hand: the header the format states, what follows it for a
// delta kind (the back-offset varint of an ofs-delta, the twenty raw bytes of a
// ref-delta's base), and the body as a stored zlib stream. These cases state
// shapes the unit's own writer will not write, so they are spelled out here.
std::size_t appendRecord(std::vector<std::uint8_t>& pack, unsigned kind,
                         const std::vector<std::uint8_t>& body,
                         const std::vector<std::uint8_t>& afterHead)
{
    const std::size_t at = pack.size();
    const std::uint64_t size = body.size();
    pack.push_back(static_cast<std::uint8_t>((kind << 4) | (size & 0xFu) |
                                             (size > 0xFu ? 0x80u : 0u)));
    for (std::uint64_t rest = size >> 4; rest != 0; rest >>= 7) {
        pack.push_back(static_cast<std::uint8_t>((rest & 0x7Fu) | (rest > 0x7Fu ? 0x80u : 0u)));
    }
    pack.insert(pack.end(), afterHead.begin(), afterHead.end());
    std::vector<std::uint8_t> stream;
    deflateStored(body, stream);
    pack.insert(pack.end(), stream.begin(), stream.end());
    return at;
}

// The twenty bytes an oid stands for, and the same bytes as an ofs back-offset.
std::vector<std::uint8_t> rawOid(const ObjectId& o)
{
    std::vector<std::uint8_t> r;
    const auto digit = [](char c) { return (c <= '9') ? (c - '0') : (c - 'a' + 10); };
    for (std::size_t i = 0; i < 20u; ++i) {
        r.push_back(static_cast<std::uint8_t>((digit(o.hex[i * 2u]) << 4) | digit(o.hex[i * 2u + 1u])));
    }
    return r;
}

std::vector<std::uint8_t> ofsBytes(std::uint64_t back)
{
    std::vector<std::uint8_t> out;
    encodeOfsOffset(back, out);
    return out;
}

// The pack header of a stated count, and its trailer: the pair the reader starts
// and ends with.
void appendPackHead(std::vector<std::uint8_t>& pack, std::uint32_t count)
{
    pack.push_back('P'); pack.push_back('A'); pack.push_back('C'); pack.push_back('K');
    pack.push_back(0); pack.push_back(0); pack.push_back(0); pack.push_back(2);
    pack.push_back(static_cast<std::uint8_t>(count >> 24));
    pack.push_back(static_cast<std::uint8_t>(count >> 16));
    pack.push_back(static_cast<std::uint8_t>(count >> 8));
    pack.push_back(static_cast<std::uint8_t>(count));
}

void appendTrailer(std::vector<std::uint8_t>& pack)
{
    const std::array<std::uint8_t, 20> digest = sha1BytesOf(pack);
    pack.insert(pack.end(), digest.begin(), digest.end());
}

// An idx naming records at the given offsets, in pack order, under the given
// oids. Each crc covers that record's own bytes: from its start to the next
// start, or to the trailer for the last — the pair is checked against exactly
// those bytes, so a crc over the rest of the pack is a different claim.
bool idxForRecords(const std::vector<ObjectId>& oids, const std::vector<std::size_t>& starts,
                   const std::vector<std::uint8_t>& pack, std::vector<std::uint8_t>& idx)
{
    const std::size_t fileEnd = pack.size() - 20u;
    std::vector<IndexEntry> entries;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        IndexEntry e;
        e.oid = oids[i];
        e.offset = starts[i];
        std::size_t to = fileEnd;
        for (const std::size_t s : starts) {
            if (s > starts[i] && s < to) {
                to = s;
            }
        }
        e.crc = crc32Of(pack.data() + starts[i], to - starts[i]);
        entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(),
              [](const IndexEntry& a, const IndexEntry& b) { return a.oid.hex < b.oid.hex; });
    const std::array<std::uint8_t, 20> digest =
        sha1BytesOf(std::vector<std::uint8_t>(pack.begin(), pack.end() - 20));
    return writeIdx(entries, digest, idx);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// 1. the fixture pair opens, and its listing answers the manifest exactly:
//    every listed object is one git named, with the kind and size it stated.

TEST(the_packed_fixture_opens_and_lists_the_manifest_objects)
{
    const std::string packPath = findFixturePack();
    CHECK(!packPath.empty());
    if (packPath.empty()) {
        return;
    }
    std::vector<std::uint8_t> packBytes;
    std::vector<std::uint8_t> idxBytes;
    CHECK(readWhole(packPath, packBytes));
    CHECK(readWhole(idxFor(packPath), idxBytes));

    Packfile pf;
    CHECK(pf.open(packBytes, idxBytes));
    CHECK(pf.isOpen());
    if (!pf.isOpen()) {
        return;   // the listing claims below would only repeat this one
    }

    std::vector<PackObject> listed;
    CHECK(pf.list(listed));
    CHECK(!listed.empty());

    // The manifest names every object of the sample with type and size. A scan
    // plain enough to need no json library: find the oid, read the record span
    // it stands in. (The fixture pack holds a subset of the manifest's objects;
    // every listed one must still answer.)
    const std::string man = ols_test::read_fixture("loose-manifest.json");
    CHECK(!man.empty());
    for (const PackObject& po : listed) {
        const std::string key = "\"" + po.oid.hex + "\"";   // the manifest's value form
        const std::size_t at = man.find(key);
        CHECK(at != std::string::npos);
        if (at == std::string::npos) {
            continue;
        }
        const std::size_t open = man.rfind('{', at);
        const std::size_t end = man.find('}', at);
        CHECK(open != std::string::npos && end != std::string::npos);
        if (open == std::string::npos || end == std::string::npos) {
            continue;
        }
        const std::string rec = man.substr(open, end - open);

        const std::size_t tAt = rec.find("\"type\": \"");
        CHECK(tAt != std::string::npos);
        if (tAt == std::string::npos) {
            continue;
        }
        const std::size_t tEnd = rec.find('"', tAt + 9);
        const std::string type = rec.substr(tAt + 9, tEnd - tAt - 9);

        const std::size_t sAt = rec.find("\"size\": ");
        CHECK(sAt != std::string::npos);
        if (sAt == std::string::npos) {
            continue;
        }
        const std::uint64_t size = std::strtoull(rec.c_str() + sAt + 8, nullptr, 10);

        // The manifest sizes the raw bytes ("type size NUL content"); the record
        // header states the same number.
        CHECK_EQ(po.size, size);

        const char* name = nullptr;
        switch (po.kind) {
        case RecordKind::Commit: name = "commit"; break;
        case RecordKind::Tree:   name = "tree";   break;
        case RecordKind::Blob:   name = "blob";   break;
        case RecordKind::Tag:    name = "tag";    break;
        default: break;
        }
        CHECK(name != nullptr && type == name);
    }
}

// ---------------------------------------------------------------------------
// 2. every object resolves to exactly the bytes the manifest names: raw is
//    "type size NUL content" and hashes back to the oid asked for.

TEST(every_packed_object_resolves_to_the_bytes_the_manifest_names)
{
    const std::string packPath = findFixturePack();
    if (packPath.empty()) {
        CHECK(!"fixture pack unreadable");
        return;
    }
    std::vector<std::uint8_t> packBytes;
    std::vector<std::uint8_t> idxBytes;
    CHECK(readWhole(packPath, packBytes));
    CHECK(readWhole(idxFor(packPath), idxBytes));

    Packfile pf;
    if (!pf.open(packBytes, idxBytes)) {
        CHECK(!"fixture pair refused to open");
        return;
    }
    std::vector<PackObject> listed;
    CHECK(pf.list(listed));

    for (const PackObject& po : listed) {
        std::vector<std::uint8_t> raw;
        CHECK(pf.resolveOid(po.oid, raw));
        if (raw.empty()) {
            continue;   // the claim above already failed for this object
        }
        // The raw bytes must hash to the oid — the object layer's own sha1 says
        // so, which proves resolve rebuilt the right content and framed it right.
        ObjectId recomputed;
        std::string error;
        CHECK(objectOid(fromBytes(raw), ObjectFormat::Sha1, recomputed, error));
        CHECK(recomputed.hex == po.oid.hex);

        // And the header must state the declared size.
        ObjectKind kind{};
        std::uint64_t size = 0;
        CHECK(objectHeaderRead(fromBytes(raw), kind, size, error));
        CHECK_EQ(size, po.size);
    }
}

// ---------------------------------------------------------------------------
// 3. crafted refusals: a flipped byte in the pack's body, a scrambled idx
//    trailer, and a ref-delta whose base is absent — open() says false for
//    every one of them, and never crashes.

TEST(moved_and_crafted_records_are_refused_or_resolved_honestly)
{
    const std::string packPath = findFixturePack();
    if (packPath.empty()) {
        CHECK(!"fixture pack missing");
        return;
    }
    std::vector<std::uint8_t> packBytes;
    std::vector<std::uint8_t> idxBytes;
    CHECK(readWhole(packPath, packBytes));
    CHECK(readWhole(idxFor(packPath), idxBytes));

    // (a) one byte of the pack's body flipped: either a record stops inflating,
    // a crc disagrees, or the trailer does — all refusals.
    {
        std::vector<std::uint8_t> broken = packBytes;
        CHECK(broken.size() > 40u);
        broken[20] ^= 0xffu;
        Packfile pf;
        CHECK(!pf.open(broken, idxBytes));
        CHECK(!pf.isOpen());
    }
    // (b) the idx's trailer checksum scrambled.
    {
        std::vector<std::uint8_t> broken = idxBytes;
        broken[broken.size() - 1u] ^= 0xffu;
        Packfile pf;
        CHECK(!pf.open(packBytes, broken));
    }
    // (c) a ref-delta record whose base oid is not in the pack: crafted by hand
    // — REF_DELTA header for one byte of body, an absent base oid, a stored
    // zlib body inflating to that one byte. The idx names it; open must refuse
    // because no object carries the base.
    {
        std::vector<std::uint8_t> pack;
        const char* hdr = "PACK";
        pack.insert(pack.end(), hdr, hdr + 4);
        pack.insert(pack.end(), {0, 0, 0, 2});   // version 2
        pack.insert(pack.end(), {0, 0, 0, 1});   // one object
        const std::uint64_t recAt = pack.size();
        pack.push_back(static_cast<std::uint8_t>((7u << 4) | 1u));   // REF_DELTA, size 1
        for (std::size_t i = 0; i < 20u; ++i) {
            pack.push_back(0x00);   // the absent base: all-zero oid
        }
        // a stored zlib block carrying the single byte 0x05, then its adler32
        // (a1 = 1+5 = 6, a2 = 1+6 = 7).
        pack.insert(pack.end(), {0x78, 0x01, 0x01, 0x00, 0xfe, 0xff, 0x05});
        pack.insert(pack.end(), {0, 0, 0, 7, 0, 0, 0, 6});
        const std::array<std::uint8_t, 20> digest = sha1BytesOf(pack);
        pack.insert(pack.end(), digest.begin(), digest.end());

        // An idx naming the record under any consistent oid; its crc must match
        // the record bytes so open reaches — and refuses at — the base check.
        IndexEntry e;
        std::string fakeOid(40, '0');
        fakeOid[1] = '5';   // bucket 0x05 of a sha1-width name
        CHECK(ObjectId::parse(fakeOid, e.oid));
        e.offset = recAt;
        e.crc = crc32Of(pack.data() + static_cast<std::size_t>(recAt),
                        pack.size() - 20u - static_cast<std::size_t>(recAt));
        std::vector<std::uint8_t> idx;
        CHECK(writeIdx({e}, digest, idx));   // the pack checksum half is the digest

        Packfile pf;
        // The claim is only "false" — the base check or an earlier one. Never a
        // crash: if this line returns, the harness is still alive to say so.
        CHECK(!pf.open(pack, idx));
    }
}

// ---------------------------------------------------------------------------
// 4. write side: three objects written reach and return as a pack — writePack
//    then writeIdx then open answers lookup and resolveOid for each.

TEST(three_written_objects_reach_and_return_as_a_pack)
{
    std::vector<LooseForPack> objs(3);
    const std::string c0 = "hello";
    const std::string c1 = std::string("a\0b", 3);
    const std::string c2(70000, 'x');   // past one stored block: multi-block zlib
    objs[0].kind = RecordKind::Blob; objs[0].content.assign(c0.begin(), c0.end());
    objs[1].kind = RecordKind::Blob; objs[1].content.assign(c1.begin(), c1.end());
    objs[2].kind = RecordKind::Blob; objs[2].content.assign(c2.begin(), c2.end());

    std::vector<std::uint8_t> packBytes;
    std::vector<IndexEntry> entries;
    CHECK(writePack(objs, packBytes, entries));
    CHECK_EQ(entries.size(), 3u);
    if (entries.size() != 3u) {
        return;
    }

    // The oids are the object layer's hashes of "blob N\0content".
    for (std::size_t i = 0; i < 3u; ++i) {
        const std::string raw = objectHeaderMake(ObjectKind::Blob, objs[i].content.size()) +
                                fromBytes(objs[i].content);
        ObjectId want;
        std::string error;
        CHECK(objectOid(raw, ObjectFormat::Sha1, want, error));
        CHECK(entries[i].oid.hex == want.hex);
    }

    // Sort by oid for the idx (writeIdx demands strict order).
    std::sort(entries.begin(), entries.end(),
              [](const IndexEntry& a, const IndexEntry& b) { return a.oid.hex < b.oid.hex; });

    // The pack's own trailer sha goes into the idx as its pack checksum.
    const std::vector<std::uint8_t> body(packBytes.begin(), packBytes.end() - 20);
    const std::array<std::uint8_t, 20> packDigest = sha1BytesOf(body);
    std::vector<std::uint8_t> idxBytes;
    CHECK(writeIdx(entries, packDigest, idxBytes));

    // The round trip: open what was just written.
    Packfile pf;
    CHECK(pf.open(packBytes, idxBytes));
    if (!pf.isOpen()) {
        return;   // the claims below would only repeat this one
    }
    for (const IndexEntry& e : entries) {
        PackObject po;
        CHECK(pf.lookup(e.oid, po));
        std::vector<std::uint8_t> raw;
        CHECK(pf.resolveOid(e.oid, raw));
        if (raw.empty()) {
            continue;
        }
        ObjectId back;
        std::string error;
        CHECK(objectOid(fromBytes(raw), ObjectFormat::Sha1, back, error));
        CHECK(back.hex == e.oid.hex);   // it is what went in
    }
}

// ---------------------------------------------------------------------------
// 5. the idx writer answers git: the fixture's own entries, written out by
//    writeIdx, must equal the fixture .idx byte for byte — fanout, names, crcs,
//    offsets, and both trailer halves.

TEST(an_index_written_from_the_fixture_entries_answers_the_fixture_byte_for_byte)
{
    const std::string packPath = findFixturePack();
    if (packPath.empty()) {
        CHECK(!"fixture pack missing");
        return;
    }
    std::vector<std::uint8_t> idxBytes;
    CHECK(readWhole(idxFor(packPath), idxBytes));

    // Read the fixture idx with its own reader...
    std::vector<IndexEntry> entries;
    CHECK(readIdx(idxBytes, entries));
    if (entries.empty()) {
        return;
    }
    // ...and write it back. The pack checksum half of the trailer comes from the
    // fixture idx itself, which is what makes byte-exactness the claim.
    std::array<std::uint8_t, 20> packChecksum{};
    std::memcpy(packChecksum.data(), idxBytes.data() + idxBytes.size() - 40u, 20);
    std::vector<std::uint8_t> written;
    CHECK(writeIdx(entries, packChecksum, written));
    CHECK_EQ(written.size(), idxBytes.size());
    if (written.size() == idxBytes.size()) {
        // Byte-exact is the claim: every byte, not just a prefix.
        int firstDiff = -1;
        for (std::size_t i = 0; i < written.size(); ++i) {
            if (written[i] != idxBytes[i]) {
                firstDiff = static_cast<int>(i);
                break;
            }
        }
        CHECK_EQ(firstDiff, -1);
    }
}

// ---------------------------------------------------------------------------
// 6. an empty object is a pack this reader opens. The writer's stored block for
//    no bytes is where this was broken: LEN of zero carries the complement of
//    zero, and with the pair written the other way round the object layer's
//    reader refused the stream, so a repository holding one empty blob — the
//    commonest object in a tree that keeps a blank line anywhere — made a pack
//    nothing here could open.

TEST(a_pack_written_with_an_empty_object_reopens_and_reads_back)
{
    std::vector<LooseForPack> objs(2);
    objs[0].kind = RecordKind::Blob;   // no content at all
    objs[1].kind = RecordKind::Tree;   // an empty tree: the same zero-length body

    std::vector<std::uint8_t> packBytes;
    std::vector<IndexEntry> entries;
    CHECK(writePack(objs, packBytes, entries));
    CHECK_EQ(entries.size(), 2u);
    if (entries.size() != 2u) {
        return;
    }
    std::sort(entries.begin(), entries.end(),
              [](const IndexEntry& a, const IndexEntry& b) { return a.oid.hex < b.oid.hex; });
    const std::array<std::uint8_t, 20> digest =
        sha1BytesOf(std::vector<std::uint8_t>(packBytes.begin(), packBytes.end() - 20));
    std::vector<std::uint8_t> idxBytes;
    CHECK(writeIdx(entries, digest, idxBytes));

    Packfile pf;
    CHECK(pf.open(packBytes, idxBytes));
    if (!pf.isOpen()) {
        return;
    }
    for (const IndexEntry& e : entries) {
        std::vector<std::uint8_t> raw;
        CHECK(pf.resolveOid(e.oid, raw));
        // "blob 0\0" and "tree 0\0": the header of an object with nothing in it.
        const std::string text(raw.begin(), raw.end());
        CHECK_EQ(text.size(), 7u);
        const std::string asBlob("blob 0\0", 7);
        const std::string asTree("tree 0\0", 7);
        CHECK(text == asBlob || text == asTree);
    }
}

// ---------------------------------------------------------------------------
// 7. an index that states more than it holds: cut anywhere inside it, and with
//    its last fanout entry raised past the bytes behind it. The count is what
//    the reader bounds its allocation by, so an over-claim is refused at the
//    count instead of answered with a reservation the file cannot fill.

TEST(an_index_cut_short_or_overclaiming_its_entries_is_refused)
{
    const std::string packPath = findFixturePack();
    if (packPath.empty()) {
        CHECK(!"fixture pack missing");
        return;
    }
    std::vector<std::uint8_t> packBytes, idxBytes;
    CHECK(readWhole(packPath, packBytes));
    CHECK(readWhole(idxFor(packPath), idxBytes));
    if (idxBytes.size() < 1100u) {
        return;
    }

    // The header, the middle of the fanout, each boundary between the tables the
    // layout states, and inside the trailer: every one is a refusal.
    const std::size_t cuts[] = {4u, 8u, 100u, 1031u, 1032u, 1071u, 1072u, 1100u, 1150u,
                                idxBytes.size() - 41u, idxBytes.size() - 1u};
    for (const std::size_t cut : cuts) {
        if (cut >= idxBytes.size()) {
            continue;
        }
        const std::vector<std::uint8_t> shortIdx(idxBytes.begin(), idxBytes.begin() + cut);
        Packfile pf;
        CHECK(!pf.open(packBytes, shortIdx));
    }
    // The last fanout entry says a great many more objects than the tables hold.
    {
        std::vector<std::uint8_t> greedy = idxBytes;
        greedy[8u + 255u * 4u] = 0xffu;
        Packfile pf;
        CHECK(!pf.open(packBytes, greedy));
    }
    // The pack's own count disagreeing with the idx is refused too, in both
    // directions: more records than named, and fewer.
    {
        std::vector<std::uint8_t> lying = packBytes;
        lying[11u] = 0x40;
        Packfile pf;
        CHECK(!pf.open(lying, idxBytes));
    }
    {
        std::vector<std::uint8_t> lying = packBytes;
        lying[11u] = 0x01;
        Packfile pf;
        CHECK(!pf.open(lying, idxBytes));
    }
}

// ---------------------------------------------------------------------------
// 8. a record header stating a body size no body reaches. The size rides in the
//    type byte and a run of seven-bit groups; a run that runs past what the
//    number holds, or a group carrying bits above the sixty-fourth, is refused
//    at the header. A well-formed but absurd claim is refused at the ceiling the
//    reader holds an object to — before the stream behind it is inflated.

TEST(a_record_header_stating_an_absurd_body_size_is_refused)
{
    // openExpect(1): the record's header is sound enough that the pack opens,
    // and the refusal lands at the record's own resolve; openExpect(0): the
    // refusal is the open's. The lazy open inflates no body, so what it can
    // still refuse at open is the header's own arithmetic — a run whose bits
    // would drop, and a size past the ceiling the number is bound by — while
    // a claim only the stream can deny is denied where the stream is read.
    const auto openOneRecord = [](const std::vector<std::uint8_t>& headBytes, int openExpect) {
        std::vector<std::uint8_t> pack;
        appendPackHead(pack, 1);
        pack.insert(pack.end(), headBytes.begin(), headBytes.end());
        // a body of one byte, a valid stored stream, whatever the header claimed.
        pack.insert(pack.end(), {0x78, 0x01, 0x01, 0x00, 0xfe, 0xff, 0x05, 0, 0, 0, 7, 0, 0, 0, 6});
        appendTrailer(pack);

        IndexEntry e;
        CHECK(ObjectId::parse("1111111111111111111111111111111111111111", e.oid));
        e.offset = 12u;
        e.crc = crc32Of(pack.data() + 12u, pack.size() - 20u - 12u);
        std::vector<std::uint8_t> idx;
        CHECK(writeIdx({e}, sha1BytesOf(std::vector<std::uint8_t>(pack.begin(), pack.end() - 20)),
                        idx));
        Packfile pf;
        if (openExpect == 0) {
            CHECK(!pf.open(pack, idx));
            return;
        }
        CHECK(pf.open(pack, idx));   // a sound-enough header, a body that denies it
        if (!pf.isOpen()) {
            return;
        }
        std::vector<std::uint8_t> raw;
        CHECK(!pf.resolveOid(e.oid, raw));   // and this is where the claim is refused
    };

    // (a) nine groups, the last carrying seven bits where four are left: the
    // bits above the sixty-fourth would be dropped, and the record would be read
    // as a smaller thing than its bytes state.
    openOneRecord({0x9F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F}, 0);
    // (b) the same run with the last group inside what the number holds: a size
    // past 2^60, stated honestly, and no object is that large — the ceiling
    // meets the number at the open, before any of the body is.
    openOneRecord({0x9F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F}, 0);
    // (c) a run meant to run off the end of the pack and cut mid-varint — but
    // the stream behind it is there, and its leading zlib byte (0x78, high bit
    // clear) ends the run at a size of some 31 MiB. The header is no longer a
    // refusal in its own arithmetic; the stream denies the claim, and with the
    // lazy open the denial lands at the resolve that reads the body. Before the
    // lazy open, this pack was refused at the open itself.
    openOneRecord({0x9F, 0xFF, 0xFF}, 1);
}

// ---------------------------------------------------------------------------
// 9. a delta that promises more than one object may be. One copy instruction is
//    a handful of bytes and moves up to 64 KiB, so a delta of a few hundred
//    kilobytes can state a result of four gigabytes; the promise is refused where
//    it is read, before the bytes it asks for are stood up.

TEST(a_delta_promising_more_than_an_object_may_be_is_refused)
{
    const std::vector<std::uint8_t> base(64u * 1024u, 'x');

    std::vector<std::uint8_t> delta;
    encodeSizeVarint(base.size(), delta);                              // the base it expects
    encodeSizeVarint(std::uint64_t{4} * 1024u * 1024u * 1024u, delta); // what it promises
    for (int i = 0; i < 65536; ++i) {
        delta.push_back(0xF0);   // a copy: no offset bytes, three size bytes
        delta.push_back(0x00);
        delta.push_back(0x00);
        delta.push_back(0x01);   // 0x000100 = 64 KiB moved, sixty-four thousand times
    }

    std::vector<std::uint8_t> pack;
    appendPackHead(pack, 2);
    const std::size_t baseAt =
        appendRecord(pack, static_cast<unsigned>(RecordKind::Blob), base, {});
    const std::size_t deltaAt = pack.size();
    appendRecord(pack, static_cast<unsigned>(RecordKind::OfsDelta), delta,
                 ofsBytes(static_cast<std::uint64_t>(deltaAt - baseAt)));
    appendTrailer(pack);

    ObjectId baseOid, deltaOid;
    CHECK(ObjectId::parse("1111111111111111111111111111111111111111", baseOid));
    CHECK(ObjectId::parse("2222222222222222222222222222222222222222", deltaOid));
    std::vector<std::uint8_t> idx;
    CHECK(idxForRecords({baseOid, deltaOid}, {baseAt, deltaAt}, pack, idx));

    Packfile pf;
    CHECK(pf.open(pack, idx));   // sound as a pack: the refusal is about the promise
    if (!pf.isOpen()) {
        return;
    }
    std::vector<std::uint8_t> raw;
    CHECK(pf.resolveOid(baseOid, raw));    // the base is there and whole
    // "blob 65536\0": the header of the object, then its bytes.
    CHECK_EQ(raw.size(), base.size() + 11u);
    CHECK(!pf.resolveOid(deltaOid, raw));  // and this is where the promise is refused
}

// ---------------------------------------------------------------------------
// 10. a ref-delta that names the object it is a delta of, through another: the
//     chain walk is a loop, and it stops at the cap it is bounded by rather than
//     walking forever or growing a stack. An ofs base outside the pack is refused
//     at the record.

TEST(a_ref_delta_cycle_walks_to_its_cap_and_answers_false)
{
    const std::vector<std::uint8_t> deltaBytes = [] {
        std::vector<std::uint8_t> d;
        encodeSizeVarint(4, d);
        encodeSizeVarint(4, d);
        d.push_back(0x04);   // insert four bytes
        d.push_back('a'); d.push_back('b'); d.push_back('c'); d.push_back('d');
        return d;
    }();

    ObjectId aOid, bOid;
    CHECK(ObjectId::parse("3333333333333333333333333333333333333333", aOid));
    CHECK(ObjectId::parse("4444444444444444444444444444444444444444", bOid));

    std::vector<std::uint8_t> pack;
    appendPackHead(pack, 2);
    // two ref-delta records, each naming the other as its base.
    const std::size_t atA = appendRecord(pack, static_cast<unsigned>(RecordKind::RefDelta),
                                         deltaBytes, rawOid(bOid));
    const std::size_t atB = appendRecord(pack, static_cast<unsigned>(RecordKind::RefDelta),
                                         deltaBytes, rawOid(aOid));
    appendTrailer(pack);

    std::vector<std::uint8_t> idx;
    CHECK(idxForRecords({aOid, bOid}, {atA, atB}, pack, idx));

    Packfile pf;
    CHECK(pf.open(pack, idx));   // both bases are named, so the pack stands
    if (!pf.isOpen()) {
        return;
    }
    std::vector<std::uint8_t> raw;
    // The walk runs to its cap and answers false. Getting here at all is the
    // claim: the loop ends.
    CHECK(!pf.resolveOid(aOid, raw));
    CHECK(!pf.resolveOid(bOid, raw));
}

// An ofs-delta whose base offset points before the pack's first record.
TEST(an_ofs_delta_base_outside_the_pack_is_refused_at_the_record)
{
    const std::vector<std::uint8_t> base(16, 'y');
    const std::vector<std::uint8_t> deltaBytes = [] {
        std::vector<std::uint8_t> d;
        encodeSizeVarint(16, d);
        encodeSizeVarint(16, d);
        d.push_back(0x04);
        d.push_back('w'); d.push_back('x'); d.push_back('y'); d.push_back('z');
        return d;
    }();

    std::vector<std::uint8_t> pack;
    appendPackHead(pack, 2);
    const std::size_t baseAt =
        appendRecord(pack, static_cast<unsigned>(RecordKind::Blob), base, {});
    // The record names a base further back than the pack begins.
    const std::size_t deltaAt = pack.size();
    appendRecord(pack, static_cast<unsigned>(RecordKind::OfsDelta), deltaBytes,
                 ofsBytes(static_cast<std::uint64_t>(deltaAt) + 4096u - baseAt));
    appendTrailer(pack);

    ObjectId baseOid, deltaOid;
    CHECK(ObjectId::parse("5555555555555555555555555555555555555555", baseOid));
    CHECK(ObjectId::parse("6666666666666666666666666666666666666666", deltaOid));
    std::vector<std::uint8_t> idx;
    CHECK(idxForRecords({baseOid, deltaOid}, {baseAt, deltaAt}, pack, idx));

    Packfile pf;
    CHECK(!pf.open(pack, idx));
}

// ---------------------------------------------------------------------------
// 11. the lazy open: opening a pack inflates no body. The pack is crafted
//     whole — three full records, the middle one's header sound and its body
//     no zlib stream at all (0xFF answers no zlib method) — with a trailer and
//     an idx whose crcs answer to the bytes as they stand, so every structural
//     claim the open still makes (the header scan, the spans, the crcs, both
//     checksums, base presence) holds. A reader that inflated bodies at open
//     refused this pack before it opened; this one opens, and the refusal
//     lands where the corrupt bytes are actually met: at that one record's
//     resolve. The sound records before and after it resolve whole, hashed
//     back to the ids that name them — and an open that inflated the middle
//     record could never say any of this.

TEST(a_pack_whose_middle_record_cannot_inflate_opens_and_fails_only_that_resolve)
{
    std::vector<std::uint8_t> pack;
    appendPackHead(pack, 3);
    const std::vector<std::uint8_t> body0{'f', 'i', 'r', 's', 't'};
    const std::size_t at0 =
        appendRecord(pack, static_cast<unsigned>(RecordKind::Blob), body0, {});
    const std::size_t at1 = pack.size();
    pack.push_back(
        static_cast<std::uint8_t>((static_cast<unsigned>(RecordKind::Blob) << 4u) | 5u));
    pack.insert(pack.end(), {0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    const std::vector<std::uint8_t> body2{'l', 'a', 's', 't', '!'};
    const std::size_t at2 =
        appendRecord(pack, static_cast<unsigned>(RecordKind::Blob), body2, {});
    appendTrailer(pack);

    // The two sound records carry their real oids, so the resolve-after-open
    // claim is a hash claim. The corrupt one stands under any consistent name.
    // ("blob 5\0first" is 12 bytes: the four of the type, a space, the size,
    // the NUL, and the five of the content.)
    const std::string raw0 = std::string("blob 5\0first", 12);
    const std::string raw2 = std::string("blob 5\0last!", 12);
    ObjectId oid0, oid1, oid2;
    CHECK(ObjectId::parse(sha1Hex(raw0), oid0));
    CHECK(ObjectId::parse(sha1Hex(raw2), oid2));
    CHECK(ObjectId::parse("2222222222222222222222222222222222222222", oid1));
    std::vector<std::uint8_t> idx;
    CHECK(idxForRecords({oid0, oid1, oid2}, {at0, at1, at2}, pack, idx));

    Packfile pf;
    // The claim: the corrupt body never met the inflater on the way in.
    CHECK(pf.open(pack, idx));
    if (!pf.isOpen()) {
        return;
    }
    CHECK_EQ(pf.count(), 3u);   // the idx names the corrupt record too: the open
                                // states what the pack holds, not what it can read
    std::vector<PackObject> listed;
    CHECK(pf.list(listed));
    CHECK_EQ(listed.size(), 3u);

    std::vector<std::uint8_t> raw;
    CHECK(pf.resolveOid(oid0, raw));   // whole, before the corruption
    CHECK_EQ(std::string(raw.begin(), raw.end()), raw0);
    raw.clear();
    CHECK(!pf.resolveOid(oid1, raw));  // refused at its body, and only there
    raw.clear();
    CHECK(pf.resolveOid(oid2, raw));   // and the corruption reached no further
    CHECK_EQ(std::string(raw.begin(), raw.end()), raw2);
    ObjectId back;
    std::string error;
    CHECK(objectOid(std::string(raw.begin(), raw.end()), ObjectFormat::Sha1, back, error));
    CHECK(back.hex == oid2.hex);
}

// ---------------------------------------------------------------------------
// 12. a delta chain walked through the lazy path with its members standing in
//     the pack in the wrong order: A a full blob, C a ref-delta whose base is
//     B, and B an ofs-delta standing on A — written in the order A, C, B, so
//     the walk down crosses a REF link to a record that stands LATER in the
//     pack, and an OFS link back to one that stands earlier, meeting each by
//     the idx and the back-offset rather than by position. Each member's body
//     inflates only when the climb reaches it, and the chain composes exactly:
//     the content the instructions state comes back framed by the base's kind.

TEST(a_delta_chain_resolves_through_ref_and_ofs_links_whatever_pack_order_its_members_stand_in)
{
    // git's instructions, stated whole: the target is what the instructions
    // make — here one copy of the base in full (presence bits saying one size
    // byte, offset zero, no offset bytes) and one insert riding its bytes on
    // the end. The base the delta declares must be the size the base answers.
    const auto wholeBasePlus = [](std::uint64_t baseDeclared, const std::string& added) {
        std::vector<std::uint8_t> d;
        std::vector<std::uint8_t> v;
        CHECK(encodeSizeVarint(baseDeclared, v));   // encodeSizeVarint clears: append a temp
        d.insert(d.end(), v.begin(), v.end());
        CHECK(encodeSizeVarint(baseDeclared + added.size(), v));
        d.insert(d.end(), v.begin(), v.end());
        d.push_back(0x90);   // copy: no offset bytes, one size byte
        d.push_back(static_cast<std::uint8_t>(baseDeclared));
        d.push_back(static_cast<std::uint8_t>(added.size()));   // insert its bytes
        for (const char c : added) {
            d.push_back(static_cast<std::uint8_t>(c));
        }
        return d;
    };

    const std::vector<std::uint8_t> contentA{'a', 'b', 'c', 'd'};
    const std::string wantA = std::string("blob 4\0abcd", 11);
    ObjectId oidA;
    CHECK(ObjectId::parse(sha1Hex(wantA), oidA));
    ObjectId oidB, oidC;
    CHECK(ObjectId::parse("8555555555555555555555555555555555555555", oidB));
    CHECK(ObjectId::parse("8666666666666666666666666666666666666666", oidC));

    std::vector<std::uint8_t> pack;
    appendPackHead(pack, 3);
    const std::size_t atA =
        appendRecord(pack, static_cast<unsigned>(RecordKind::Blob), contentA, {});
    // C first: a ref-delta naming B — a record that does not stand yet.
    const std::size_t atC = appendRecord(pack, static_cast<unsigned>(RecordKind::RefDelta),
                                         wholeBasePlus(6, "ghi"), rawOid(oidB));
    // B last: an ofs-delta whose base is A, reached by the back-offset.
    const std::size_t atB = pack.size();
    appendRecord(pack, static_cast<unsigned>(RecordKind::OfsDelta), wholeBasePlus(4, "ef"),
                 ofsBytes(static_cast<std::uint64_t>(atB - atA)));
    appendTrailer(pack);

    std::vector<std::uint8_t> idx;
    CHECK(idxForRecords({oidA, oidB, oidC}, {atA, atB, atC}, pack, idx));

    Packfile pf;
    CHECK(pf.open(pack, idx));   // both bases are carried: the pack stands
    if (!pf.isOpen()) {
        return;
    }
    std::vector<std::uint8_t> raw;
    // C first: down the ref link to B, down the ofs link to A, and up through
    // both sets of instructions: "blob 9\0abcdefghi" — 16 bytes framed.
    CHECK(pf.resolveOid(oidC, raw));
    CHECK_EQ(std::string(raw.begin(), raw.end()), std::string("blob 9\0abcdefghi", 16));
    raw.clear();
    CHECK(pf.resolveOid(oidB, raw));   // one ofs link: "blob 6\0abcdef"
    CHECK_EQ(std::string(raw.begin(), raw.end()), std::string("blob 6\0abcdef", 13));
    raw.clear();
    CHECK(pf.resolveOid(oidA, raw));   // the base whole under all of it
    CHECK_EQ(std::string(raw.begin(), raw.end()), wantA);
}

// ---------------------------------------------------------------------------
// 13. what the pack folder is to the walk that discovers a repo's packs.
//     ProjectGit's discoverPacks opens <repo>/objects/pack with the error
//     code, and takes ENOENT — a repo that never packed — and ENOTDIR — a
//     file standing where the folder would be — as the same honest silence:
//     no packs, an empty listing, answered the way the never-packed repo is.
//     Any other error fails the call closed — "the pack folder could not be
//     read" — rather than folding a permission or IO failure into "no packs"
//     and a downstream "object not found". The chosen behavior, stated: the
//     file-in-its-place IS the not-a-folder case — ENOTDIR on Linux, the
//     ERROR_DIRECTORY walk error on Windows — and that case IS "no packs".
//     discoverPacks itself lives in ProjectGit.cpp, which is not in this
//     harness's link; what this case pins is the classification's input —
//     the walk's own words for each shape — which is what the module reads.

TEST(the_pack_folder_walk_answers_a_file_standing_in_its_place_as_not_a_folder)
{
    std::error_code ec;
    const std::filesystem::path root =
        ols_test::scratchRoot() /
        ("ols-packwalk-" + std::to_string(std::random_device{}()));
    CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    std::filesystem::create_directories(root / "objects", ec);
    CHECK(!ec);
    if (ec) {
        return;
    }
    // A regular FILE named pack where the folder would be.
    {
        std::ofstream f(root / "objects" / "pack", std::ios::binary);
        f << "not a folder";
    }
    std::error_code walkEc{};
    const std::filesystem::directory_iterator overFile{root / "objects" / "pack", walkEc};
    (void)overFile;
    // The walk does not succeed over a file: it answers the platform's own
    // word for "that is not a folder" — ENOTDIR on Linux, ERROR_DIRECTORY
    // (267) on Windows, where the system category does not tie 267 to
    // ENOTDIR. Both are the "no packs" side of the line, and ProjectGit's
    // classifier reads both that way; this is the fact the module's chosen
    // behavior rests on.
    CHECK(static_cast<bool>(walkEc));
    CHECK(walkEc == std::errc::not_a_directory || walkEc.value() == 267);
    // The folder that is simply not there: the same silence, from ENOENT.
    std::error_code goneEc{};
    const std::filesystem::directory_iterator overGone{root / "objects" / "no-such", goneEc};
    (void)overGone;
    CHECK(goneEc == std::errc::no_such_file_or_directory);
    // A real folder, walked clean: the walk stands ready and answers no error.
    std::error_code thereEc{};
    const std::filesystem::directory_iterator overReal{root / "objects", thereEc};
    CHECK(!thereEc);
    std::filesystem::remove_all(root, ec);
    CHECK(!ec);   // the case leaves no temp behind
}
