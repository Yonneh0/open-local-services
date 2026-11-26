// tests/gitcore/test_wire.cpp — the cases that prove wire.h.
//
// No fixtures: the vectors of this protocol are written here to the shapes of
// git's gitprotocol-v2 with the hex lengths counted in the comments, and the
// unit is held to exactly those bytes. The object-info callback stands as the
// small set these cases name; no repository is opened and no server is asked.
// The counts belong beside the bytes so a claim that does not hold is caught
// at the count, not at a length that was trusted.

#include "gitcore/wire.h"
#include "gitcore/wire.cpp" // pulled in like the cases: no build names wire.cpp as a source - the definitions are inline, so the harness reaches this ground through the include, the same way the two doors reach it beside their own sources

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ols::gitcore::PktKind;
using ols::gitcore::PktReader;
using ols::gitcore::V2Request;
using ols::gitcore::LsRefsRequest;
using ols::gitcore::LsRefsEntry;
using ols::gitcore::FetchRequest;
using ols::gitcore::MemoryObjectInfo;

// ids a fixture repo really has, so the bytes here name values git made.
const char *const kWireCommit = "a867ef798b215db19eaf0c7b615c789792fe62b9";
const char *const kWireTree = "d25250f5f7f874caffa476f3a69161b879d2cbc6";
const char *const kWireTag = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef";

std::vector<std::uint8_t> wireBytesOf(const std::string &text)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size());
    for (char c : text)
    {
        bytes.push_back(static_cast<std::uint8_t>(c));
    }
    return bytes;
}

std::string wireTextOf(const std::vector<std::uint8_t> &bytes)
{
    std::string text;
    text.reserve(bytes.size());
    for (std::uint8_t b : bytes)
    {
        text.push_back(static_cast<char>(b));
    }
    return text;
}

// A ref that points at an object; a null oidHex leaves the entry pointing at
// nothing, which is how an unborn HEAD stands.
LsRefsEntry makeWireEntry(const std::string &name, const char *oidHex)
{
    LsRefsEntry entry;
    entry.name = name;
    if (oidHex != nullptr)
    {
        entry.oid = ols::gitcore::ObjectId(std::string(oidHex));
    }
    return entry;
}

// The v2 request for ls-refs, by hand, to the shape of gitprotocol-v2.
const char *const kWireLsRefsRequest =
    "0014command=ls-refs\n" // "command=ls-refs\n" is 16; 16+4 = 20 = 0x14
    "0001"                 // the delimiter: no capabilities, straight to args
    "000bunborn\n"          // "unborn\n" is 7; +4 = 11 = 0x0b
    "0009peel\n"            // "peel\n" is 5; +4 = 9 = 0x09
    "001bref-prefix refs/heads/\n" // 23; +4 = 27 = 0x1b
    "0000";                 // the end of the request

const std::string kWireFetchRequestText = [] {
    std::string text = "0012command=fetch\n"; // 14; +4 = 18 = 0x12
    text += "0017object-format=sha1\n";       // 19; +4 = 23 = 0x17
    text += "0001";
    text += std::string("0032want ") + kWireCommit + "\n"; // "want "+40+"\n" is 46; +4 = 50 = 0x32
    text += std::string("0032have ") + kWireTree + "\n";
    text += "0009done\n"; // 5; +4 = 9 = 0x09
    text += "0000";
    return text;
}();

// the pack the caller supplies: the empty pack git sends — header, zero
// entries, trailer. The trailer bytes stand in here; framing is proved.
std::string wireEmptyPack()
{
    std::string pack = "PACK";
    for (int i = 0; i < 8; ++i)
    {
        pack.push_back(static_cast<char>(0)); // version and entry count
    }
    pack += std::string(kWireCommit).substr(0, 20u); // 20 trailer bytes
    return pack;
}

} // anonymous namespace

// ----------------------------------------------------------------- framing

TEST(wire_pkt_round_trip)
{
    std::string error;
    std::vector<std::uint8_t> written;

    ols::gitcore::pktWrite("hello\n", written, error); // 6; +4 = 10 = 0x0a
    CHECK_EQ(wireTextOf(written), "000ahello\n");

    written.clear();
    std::string bin;
    bin.push_back(static_cast<char>(0));
    bin.push_back(static_cast<char>(0xff));
    bin.push_back(static_cast<char>(1));
    bin.push_back('X');
    CHECK(ols::gitcore::pktWrite(bin, written, error)); // opaque bytes; 4; +4 = 8
    std::string expectedBin = "0008";
    expectedBin += bin;
    CHECK_EQ(wireTextOf(written), expectedBin);

    written.clear();
    CHECK(ols::gitcore::pktWrite(std::string(), written, error)); // empty payload: "0004"
    CHECK_EQ(wireTextOf(written), "0004");

    // all three back out of one buffer
    const std::string all =
        std::string("000ahello\n") + expectedBin + "0004";
    const std::vector<std::uint8_t> bytes = wireBytesOf(all);
    PktReader reader(bytes.data(), bytes.size());
    PktKind kind;
    std::string_view view;
    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Data);
    CHECK(view == "hello\n");
    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Data);
    CHECK(view == std::string_view(bin));
    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Data);
    CHECK(view.empty());
    CHECK(reader.exhausted());
}

TEST(wire_short_and_reserved_lengths)
{
    std::string error;
    PktKind kind;
    std::string payload;

    // the reserved specials are refusals, not empty data packets
    std::vector<std::uint8_t> bytes = wireBytesOf("0002");
    std::size_t pos = 0;
    CHECK(!ols::gitcore::pktRead(bytes.data(), bytes.size(), pos, kind, payload, error));
    CHECK(error.find("reserved") != std::string::npos);
    CHECK_EQ(pos, 0u);

    bytes = wireBytesOf("0003");
    pos = 0;
    CHECK(!ols::gitcore::pktRead(bytes.data(), bytes.size(), pos, kind, payload, error));

    // a length field that is not hex
    bytes = wireBytesOf("000g");
    pos = 0;
    CHECK(!ols::gitcore::pktRead(bytes.data(), bytes.size(), pos, kind, payload, error));
    CHECK(error.find("not hex") != std::string::npos);

    // a length claiming past the buffer: "00ff" with only two payload bytes
    bytes = wireBytesOf("00ffab");
    pos = 0;
    CHECK(!ols::gitcore::pktRead(bytes.data(), bytes.size(), pos, kind, payload, error));
    CHECK(error.find("past the buffer") != std::string::npos);

    // fewer than four bytes: no length field at all
    bytes = wireBytesOf("00f");
    pos = 0;
    CHECK(!ols::gitcore::pktRead(bytes.data(), bytes.size(), pos, kind, payload, error));
    CHECK(error.find("no length field") != std::string::npos);

    // the specials are kinds, and both stand in one buffer
    bytes = wireBytesOf("00010000");
    pos = 0;
    CHECK(ols::gitcore::pktRead(bytes.data(), bytes.size(), pos, kind, payload, error));
    CHECK(kind == PktKind::Delim);
    CHECK(ols::gitcore::pktRead(bytes.data(), bytes.size(), pos, kind, payload, error));
    CHECK(kind == PktKind::Flush);
    CHECK_EQ(pos, 8u);
}

TEST(wire_walk_request_bytes)
{
    std::string error;
    const std::vector<std::uint8_t> bytes = wireBytesOf(kWireLsRefsRequest);
    PktReader reader(bytes.data(), bytes.size());
    PktKind kind;
    std::string_view view;

    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Data);
    CHECK(view == "command=ls-refs\n");
    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Delim);
    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Data);
    CHECK(view == "unborn\n");
    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Data);
    CHECK(view == "peel\n");
    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Data);
    CHECK(view == "ref-prefix refs/heads/\n");
    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Flush);
    CHECK(reader.exhausted());
}

TEST(wire_lsrefs_request_parse)
{
    std::string error;
    const std::vector<std::uint8_t> bytes = wireBytesOf(kWireLsRefsRequest);
    V2Request parsed;
    CHECK(ols::gitcore::v2ParseRequest(bytes.data(), bytes.size(), parsed, error));
    CHECK_EQ(parsed.command, std::string("ls-refs"));
    CHECK_EQ(parsed.capabilityLines.size(), 0u);
    CHECK_EQ(parsed.argumentLines.size(), 3u);
    CHECK_EQ(parsed.argumentLines[0], std::string("unborn\n"));
    CHECK_EQ(parsed.argumentLines[1], std::string("peel\n"));
    CHECK_EQ(parsed.argumentLines[2], std::string("ref-prefix refs/heads/\n"));

    LsRefsRequest asked;
    CHECK(ols::gitcore::v2ParseLsRefs(parsed, asked, error));
    CHECK(asked.unborn);
    CHECK(asked.peel);
    CHECK(!asked.symrefs);
    CHECK_EQ(asked.prefixes.size(), 1u);
    CHECK_EQ(asked.prefixes[0], std::string("refs/heads/"));

    // the delimiter between capabilities and arguments is required
    const std::vector<std::uint8_t> noDelim =
        wireBytesOf("0014command=ls-refs\n0000");
    V2Request other;
    CHECK(!ols::gitcore::v2ParseRequest(noDelim.data(), noDelim.size(), other, error));
    CHECK(error.find("delimiter") != std::string::npos);

    // a lone flush is the empty request the grammar allows
    const std::vector<std::uint8_t> onlyFlush = wireBytesOf("0000");
    CHECK(ols::gitcore::v2ParseRequest(onlyFlush.data(), onlyFlush.size(), other, error));
    CHECK(other.command.empty());
    CHECK(other.capabilityLines.empty());
    CHECK(other.argumentLines.empty());

    // bytes after the request's flush are another request this unit refuses
    const std::vector<std::uint8_t> trailing = wireBytesOf("00000000");
    CHECK(!ols::gitcore::v2ParseRequest(trailing.data(), trailing.size(), other, error));
    CHECK(error.find("after") != std::string::npos);
}

TEST(wire_lsrefs_request_build)
{
    std::string error;
    std::vector<std::uint8_t> built;
    CHECK(ols::gitcore::v2BuildRequest(
        "ls-refs", std::vector<std::string>(),
        {"unborn\n", "peel\n", "ref-prefix refs/heads/\n"}, built, error));
    CHECK_EQ(wireTextOf(built), std::string(kWireLsRefsRequest));

    // a name outside the grammar is refused, not framed
    built.clear();
    CHECK(!ols::gitcore::v2BuildRequest("ls refs", {}, {}, built, error));
}

// -------------------------------------------------------- advertisement

TEST(wire_advertisement_bytes)
{
    std::string error;
    std::string commandLine;
    CHECK(ols::gitcore::v2CommandCapabilityLine(
        {"ls-refs", "fetch", "push-options"}, commandLine, error));
    CHECK_EQ(commandLine, std::string("command=ls-refs fetch push-options\n"));

    std::vector<std::uint8_t> out;
    CHECK(ols::gitcore::v2Advertise({commandLine, "agent=ols-git-tests\n"}, out, error));
    // "version 2\n" 10; +4 = 14 = 0x0e
    // "command=ls-refs fetch push-options\n" 35; +4 = 39 = 0x27
    // "agent=ols-git-tests\n" 20; +4 = 24 = 0x18
    CHECK_EQ(wireTextOf(out),
             std::string("000e" "version 2\n"
                         "0027" "command=ls-refs fetch push-options\n"
                         "0018" "agent=ols-git-tests\n"
                         "0000"));

    // the line shapes are checked before a byte is written
    CHECK(!ols::gitcore::v2Advertise({"agent=not a line!"}, out, error));
    CHECK(error.find("rejected") != std::string::npos);
}

// -------------------------------------------------------------- ls-refs

TEST(wire_lsrefs_answer_bytes)
{
    std::string error;
    std::vector<LsRefsEntry> table;

    LsRefsEntry mainRef = makeWireEntry("refs/heads/main", kWireCommit);
    table.push_back(mainRef);

    LsRefsEntry head = makeWireEntry("HEAD", kWireCommit);
    head.isSymref = true;
    head.symrefTarget = "refs/heads/main";
    table.push_back(head);

    LsRefsEntry tag = makeWireEntry("refs/tags/v1", kWireTag);
    tag.peeled = true;
    tag.peeledOid = ols::gitcore::ObjectId(std::string(kWireCommit));
    table.push_back(tag);

    LsRefsRequest asked;
    asked.peel = true;
    asked.symrefs = true;

    std::vector<std::uint8_t> out;
    CHECK(ols::gitcore::lsRefsAnswer(table, asked, out, error));

    std::string expected;
    // "<oid> refs/heads/main\n": 40 + 1 + 15 + 1 = 57; +4 = 61 = 0x3d
    expected += "003d";
    expected += std::string(kWireCommit) + " refs/heads/main\n";
    // "<oid> HEAD symref-target:refs/heads/main\n": 40+1+4+1+14+15+1 = 76; +4 = 80 = 0x50
    expected += "0050";
    expected += std::string(kWireCommit) + " HEAD symref-target:refs/heads/main\n";
    // "<tagoid> refs/tags/v1 peeled:<commit>\n": 40+1+12+1+7+40+1 = 102; +4 = 106 = 0x6a
    expected += "006a";
    expected += std::string(kWireTag) + " refs/tags/v1 peeled:" + std::string(kWireCommit) + "\n";
    expected += "0000";
    CHECK_EQ(wireTextOf(out), expected);
}

TEST(wire_lsrefs_unborn)
{
    std::string error;
    std::vector<LsRefsEntry> fresh;
    LsRefsEntry head = makeWireEntry("HEAD", nullptr);
    head.isSymref = true;
    head.symrefTarget = "refs/heads/main";
    fresh.push_back(head);

    LsRefsRequest asked;
    asked.unborn = true;
    std::vector<std::uint8_t> out;
    CHECK(ols::gitcore::lsRefsAnswer(fresh, asked, out, error));
    // "unborn HEAD symref-target:refs/heads/main\n": 6+1+4+1+14+15+1 = 42; +4 = 46 = 0x2e
    CHECK_EQ(wireTextOf(out),
             std::string("002eunborn HEAD symref-target:refs/heads/main\n0000"));

    // the line is not offered when the request did not ask for it
    LsRefsRequest plain;
    CHECK(!ols::gitcore::lsRefsAnswer(fresh, plain, out, error));
    CHECK(error.find("unborn") != std::string::npos);
}

TEST(wire_lsrefs_ref_prefix)
{
    std::string error;
    std::vector<LsRefsEntry> table;
    table.push_back(makeWireEntry("refs/heads/main", kWireCommit));
    table.push_back(makeWireEntry("refs/heads/next", kWireTree));
    table.push_back(makeWireEntry("refs/tags/v1", kWireTag));
    table.push_back(makeWireEntry("HEAD", kWireCommit));

    LsRefsRequest asked;
    asked.prefixes.push_back("refs/heads/");

    std::vector<std::uint8_t> out;
    CHECK(ols::gitcore::lsRefsAnswer(table, asked, out, error));

    std::string expected;
    expected += "003d"; // 40 + 1 + 15 + 1 = 57; +4 = 0x3d
    expected += std::string(kWireCommit) + " refs/heads/main\n";
    expected += "003d"; // "refs/heads/next" is the same width
    expected += std::string(kWireTree) + " refs/heads/next\n";
    expected += "0000";
    CHECK_EQ(wireTextOf(out), expected);
}

// ---------------------------------------------------------------- fetch

TEST(wire_fetch_request_parse)
{
    std::string error;
    const std::vector<std::uint8_t> bytes = wireBytesOf(kWireFetchRequestText);
    V2Request parsed;
    CHECK(ols::gitcore::v2ParseRequest(bytes.data(), bytes.size(), parsed, error));
    CHECK_EQ(parsed.command, std::string("fetch"));
    CHECK_EQ(parsed.capabilityLines.size(), 1u);
    CHECK_EQ(parsed.capabilityLines[0], std::string("object-format=sha1\n"));
    CHECK_EQ(parsed.argumentLines.size(), 3u);

    FetchRequest asked;
    CHECK(ols::gitcore::v2ParseFetch(parsed, asked, error));
    CHECK_EQ(asked.wants.size(), 1u);
    CHECK_EQ(asked.wants[0], std::string(kWireCommit));
    CHECK_EQ(asked.haves.size(), 1u);
    CHECK_EQ(asked.haves[0], std::string(kWireTree));
    CHECK(asked.done);
}

TEST(wire_fetch_answer_held)
{
    std::string error;
    FetchRequest asked;
    asked.wants.push_back(std::string(kWireCommit));
    asked.done = true;

    MemoryObjectInfo holds({std::string(kWireCommit)});
    const std::string pack = wireEmptyPack(); // 4 + 8 + 20 = 32 bytes
    std::vector<std::uint8_t> out;
    CHECK(ols::gitcore::fetchAnswer(asked, holds, pack, out, error));

    // the request carried "done", so there is no acknowledgments section at
    // all: the packfile section and the flush that ends the response. this
    // unit has no "OK" line — and no ACK line here either.
    std::string expected;
    // "packfile\n": 9; +4 = 13 = 0x0d
    expected += "000d";
    expected += "packfile\n";
    // one sideband packet: channel + 32 pack bytes = 33; +4 = 37 = 0x25
    expected += "0025";
    expected.push_back(static_cast<char>(1));
    expected += pack;
    expected += "0000";
    const std::string answer = wireTextOf(out);
    CHECK_EQ(answer, expected);
    // nothing stands before the packfile section, and no acknowledgment of
    // any kind is in this answer
    CHECK(answer.compare(0, 13u, "000dpackfile\n") == 0);
    CHECK(answer.find("acknowledgments") == std::string::npos);
    CHECK(answer.find("NAK") == std::string::npos);
    CHECK(answer.find("ACK ") == std::string::npos);
}

TEST(wire_fetch_answer_round)
{
    std::string error;
    FetchRequest asked;
    asked.wants.push_back(std::string(kWireCommit));
    asked.haves.push_back(std::string(kWireTree)); // the callback does not hold it
    asked.done = false;

    MemoryObjectInfo holds({std::string(kWireCommit)});
    const std::string pack = wireEmptyPack();
    std::vector<std::uint8_t> out;
    CHECK(ols::gitcore::fetchAnswer(asked, holds, pack, out, error));

    // the request carried no done, so the round is answered here: an
    // acknowledgments section, the ready line that says the pack follows
    // here, a delim-pkt between the two sections, and the packfile.
    std::string expected;
    // "acknowledgments\n": 16; +4 = 20 = 0x14
    expected += "0014acknowledgments\n";
    // none of the haves is held, so the section says NAK: "NAK\n" is 4; +4 = 8 = 0x08
    expected += "0008NAK\n";
    // "ready\n": 6; +4 = 10 = 0x0a
    expected += "000aready\n";
    expected += "0001"; // the delim-pkt that separates the sections
    // "packfile\n": 9; +4 = 13 = 0x0d
    expected += "000dpackfile\n";
    expected += "0025";
    expected.push_back(static_cast<char>(1));
    expected += pack;
    expected += "0000";
    const std::string answer = wireTextOf(out);
    CHECK_EQ(answer, expected);
    CHECK(answer.find("ACK ") == std::string::npos);

    // a have the callback does hold is acknowledged by name instead
    MemoryObjectInfo both({std::string(kWireCommit), std::string(kWireTree)});
    out.clear();
    CHECK(ols::gitcore::fetchAnswer(asked, both, pack, out, error));

    std::string acked;
    acked += "0014acknowledgments\n";
    // "ACK <oid>\n": 3+1+40+1 = 45; +4 = 49 = 0x31
    acked += "0031";
    acked += std::string("ACK ") + kWireTree + "\n";
    acked += "000aready\n";
    acked += "0001";
    acked += "000dpackfile\n";
    acked += "0025";
    acked.push_back(static_cast<char>(1));
    acked += pack;
    acked += "0000";
    const std::string roundAnswer = wireTextOf(out);
    CHECK_EQ(roundAnswer, acked);
    CHECK(roundAnswer.find("NAK") == std::string::npos);
}

TEST(wire_fetch_answer_already_held)
{
    std::string error;
    FetchRequest asked;
    asked.wants.push_back(std::string(kWireCommit));
    asked.wants.push_back(std::string(kWireTree));
    asked.haves.push_back(std::string(kWireCommit));
    asked.haves.push_back(std::string(kWireTree)); // the client sent every want it holds
    asked.done = true;

    MemoryObjectInfo holds({std::string(kWireCommit), std::string(kWireTree)});
    std::vector<std::uint8_t> out;
    // no pack is in hand: nothing is wanted, so this answer frames no pack
    CHECK(ols::gitcore::fetchAnswer(asked, holds, std::string(), out, error));

    // the packfile section carries no data packets: that is what a client that
    // already holds all of it reads.
    const std::string answer = wireTextOf(out);
    CHECK_EQ(answer, std::string("000d" "packfile\n" "0000"));
    CHECK(answer.find("PACK") == std::string::npos);
}

TEST(wire_fetch_answer_missing)
{
    std::string error;
    FetchRequest asked;
    asked.wants.push_back(std::string(kWireCommit));
    asked.wants.push_back(std::string(kWireTree));
    asked.done = true;

    MemoryObjectInfo holds({std::string(kWireTree)}); // the second want is held
    std::vector<std::uint8_t> out;
    CHECK(ols::gitcore::fetchAnswer(asked, holds, std::string(), out, error));

    // "ERR want <oid> cannot be found on the server\n": 4+5+40+30+1 = 80; +4 = 84 = 0x54
    std::string expected = "0054";
    expected += std::string("ERR want ") + kWireCommit + " cannot be found on the server\n";
    expected += "0000";
    CHECK_EQ(wireTextOf(out), expected);
}

TEST(wire_fetch_refusals)
{
    std::string error;

    // the arguments this unit will not answer, each refused with the reason it
    // is out of: shallow, partial, and ref-resolved requests only.
    V2Request req;
    req.command = "fetch";
    FetchRequest asked;

    req.argumentLines = {std::string("want ") + kWireCommit + "\n",
                         "deepen 2\n", "done\n"};
    CHECK(!ols::gitcore::v2ParseFetch(req, asked, error));
    CHECK(error.find("deepen asks a shallow") != std::string::npos);

    req.argumentLines[1] = "filter blob:none\n";
    CHECK(!ols::gitcore::v2ParseFetch(req, asked, error));
    CHECK(error.find("filter asks a partial") != std::string::npos);

    req.argumentLines[1] = "want-ref refs/heads/main\n";
    CHECK(!ols::gitcore::v2ParseFetch(req, asked, error));
    CHECK(error.find("want-ref resolves refs") != std::string::npos);

    req.argumentLines[1] = "depth 2\n";
    CHECK(!ols::gitcore::v2ParseFetch(req, asked, error));
    CHECK(error.find("depth asks a shallow") != std::string::npos);

    // the four optional arguments git sends are taken and recorded; the answer
    // of this unit does not change, so none of them is a refusal
    req.argumentLines = {std::string("want ") + kWireCommit + "\n",
                         "no-progress\n",
                         "thin-pack\n",
                         "ofs-delta\n",
                         "include-tag\n",
                         "done\n"};
    CHECK(ols::gitcore::v2ParseFetch(req, asked, error));
    CHECK_EQ(asked.wants.size(), 1u);
    CHECK(asked.noProgress);
    CHECK(asked.thinPack);
    CHECK(asked.ofsDelta);
    CHECK(asked.includeTag);
    CHECK(asked.done);

    // the answer refuses only what it cannot answer: no want at all, and a
    // pack that the answer would need but is not in hand
    std::vector<std::uint8_t> out;
    FetchRequest noWant;
    noWant.haves.push_back(std::string(kWireCommit));
    noWant.done = true;
    CHECK(!ols::gitcore::fetchAnswer(noWant, MemoryObjectInfo({std::string(kWireCommit)}), std::string(), out, error));
    CHECK(error.find("no want") != std::string::npos);

    FetchRequest wantOnly;
    wantOnly.wants.push_back(std::string(kWireCommit));
    wantOnly.done = true;
    CHECK(!ols::gitcore::fetchAnswer(wantOnly, MemoryObjectInfo({std::string(kWireCommit)}), std::string(), out, error));
    CHECK(error.find("no pack is in hand") != std::string::npos);

    // "done" is not asked of a request any more: without it the answer is the
    // acknowledgments round, not a refusal (see wire_fetch_answer_round)
    FetchRequest open;
    open.wants.push_back(std::string(kWireCommit));
    open.haves.push_back(std::string(kWireTree));
    CHECK(ols::gitcore::fetchAnswer(open, MemoryObjectInfo({std::string(kWireCommit)}), wireEmptyPack(), out, error));
}

TEST(wire_object_format_request)
{
    std::string error;
    const std::string sha256 =
        std::string(kWireCommit) + std::string(kWireCommit).substr(0, 24u); // 64 hex

    V2Request req;
    req.command = "fetch";
    req.capabilityLines = {"object-format=sha256\n"};
    req.argumentLines = {"want " + sha256 + "\n", "done\n"};
    FetchRequest asked;
    CHECK(ols::gitcore::v2ParseFetch(req, asked, error));
    CHECK_EQ(asked.wants.size(), 1u);
    CHECK_EQ(asked.wants[0], sha256);

    // the width is the format's: a sha1-width want under sha256 is refused
    req.argumentLines = {std::string("want ") + kWireCommit + "\n", "done\n"};
    FetchRequest other;
    CHECK(!ols::gitcore::v2ParseFetch(req, other, error));

    // a format this unit does not speak is refused by name
    req.capabilityLines = {"object-format=sha512\n"};
    req.argumentLines = {"want " + sha256 + "\n", "done\n"};
    CHECK(!ols::gitcore::v2ParseFetch(req, other, error));
    CHECK(error.find("does not speak") != std::string::npos);
}

// ------------------------------------------------------- the assembler

TEST(wire_assemble_walk)
{
    std::string error;
    std::vector<std::uint8_t> out;
    CHECK(ols::gitcore::assembleResponse({"a\n", "bb\n"}, out, error));
    // "a\n" is 2; +4 = 6 = 0x06
    // "bb\n" is 3; +4 = 7 = 0x07
    CHECK_EQ(wireTextOf(out), std::string("0006" "a\n" "0007" "bb\n" "0000"));

    PktReader reader(out.data(), out.size());
    PktKind kind;
    std::string_view view;
    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Data);
    CHECK(view == "a\n");
    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Data);
    CHECK(view == "bb\n");
    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Flush);
    CHECK(reader.exhausted());

    // a payload that cannot be framed leaves out untouched
    CHECK(!ols::gitcore::assembleResponse({"a\n", std::string(ols::gitcore::kMaxPktPayload + 1u, 'x')}, out, error));
    CHECK(error.find("does not fit") != std::string::npos);
}

// ------------------------------------------------------- the sideband channel

TEST(wire_v2_sideband_framing)
{
    std::string error;
    std::vector<std::uint8_t> out;

    // channel 1 (pack): one line, the channel byte then the bytes.
    CHECK(ols::gitcore::v2Sideband(1, "hello\n", out, error)); // "hello\n" 6; +channel = 7; +4 = 11 = 0x0b
    CHECK_EQ(wireTextOf(out),
             std::string("000b") + std::string(1, static_cast<char>(1)) + "hello\n");

    // channel 2 (progress).
    out.clear();
    CHECK(ols::gitcore::v2Sideband(2, "progress\n", out, error)); // 9; +1 = 10; +4 = 14 = 0x0e
    CHECK_EQ(wireTextOf(out),
             std::string("000e") + std::string(1, static_cast<char>(2)) + "progress\n");

    // channel 3 (fatal): the same framing, the channel byte is 3.
    out.clear();
    CHECK(ols::gitcore::v2Sideband(3, "fatal error\n", out, error)); // 12; +1 = 13; +4 = 17 = 0x11
    CHECK_EQ(wireTextOf(out),
             std::string("0011") + std::string(1, static_cast<char>(3)) + "fatal error\n");

    // an EMPTY payload still frames: the channel byte alone is one line.
    out.clear();
    CHECK(ols::gitcore::v2Sideband(1, std::string(), out, error)); // channel only: 1; +4 = 5 = 0x05
    CHECK_EQ(wireTextOf(out),
             std::string("0005") + std::string(1, static_cast<char>(1)));

    // channel 0 and channel 4 are refused, and out keeps no part of them.
    out.clear();
    CHECK(!ols::gitcore::v2Sideband(0, "x", out, error));
    CHECK(error.find("must be 1, 2, or 3") != std::string::npos);
    CHECK(out.empty());
    out.clear();
    CHECK(!ols::gitcore::v2Sideband(4, "x", out, error));
    CHECK(error.find("must be 1, 2, or 3") != std::string::npos);
    CHECK(out.empty());

    // a payload crossing the 1024-byte bound is NOT split here: v2Sideband
    // writes ONE line up to kMaxSidebandData; the chunking is the caller's
    // (fetchAnswer splits by kMaxSidebandData). Pin the single-line truth.
    out.clear();
    const std::string big(1024, 'x');
    CHECK(ols::gitcore::v2Sideband(1, big, out, error)); // 1024; +1 = 1025; +4 = 1029 = 0x0405
    std::string expected = "0405";
    expected.push_back(static_cast<char>(1));
    expected += big;
    CHECK_EQ(wireTextOf(out), expected);
}

TEST(wire_sideband_round_trip)
{
    std::string error;
    std::vector<std::uint8_t> out;
    // a progress frame on channel 2, written then read back through PktReader:
    // the channel byte and the bytes survive unchanged.
    CHECK(ols::gitcore::v2Sideband(2, "progress text\n", out, error)); // 14; +1 = 15; +4 = 19 = 0x13
    CHECK_EQ(wireTextOf(out),
             std::string("0013") + std::string(1, static_cast<char>(2)) + "progress text\n");

    PktReader reader(out.data(), out.size());
    PktKind kind;
    std::string_view view;
    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Data);
    CHECK_EQ(view.size(), 15u);
    CHECK_EQ(static_cast<unsigned char>(view[0]), 2u); // the channel is kept
    CHECK(view.substr(1) == "progress text\n");         // and the bytes
    CHECK(reader.exhausted());
}

// ------------------------------------------------- the capability grammar

TEST(wire_v2_capability_grammar)
{
    std::string error;
    ols::gitcore::V2Capability cap;

    // valid key=value
    CHECK(ols::gitcore::v2CapabilityOf("agent=ols-git-tests\n", cap, error));
    CHECK_EQ(cap.key, std::string("agent"));
    CHECK_EQ(cap.value, std::string("ols-git-tests"));

    // bare key, no value
    CHECK(ols::gitcore::v2CapabilityOf("unborn\n", cap, error));
    CHECK_EQ(cap.key, std::string("unborn"));
    CHECK(cap.value.empty());

    // space inside the value is allowed (VALUE_CHARS carries a space)
    CHECK(ols::gitcore::v2CapabilityOf("agent=hello world\n", cap, error));
    CHECK_EQ(cap.key, std::string("agent"));
    CHECK_EQ(cap.value, std::string("hello world"));

    // value with assorted allowed characters (space, dot, comma)
    CHECK(ols::gitcore::v2CapabilityOf("k=a b.c,d\n", cap, error));
    CHECK_EQ(cap.key, std::string("k"));
    CHECK_EQ(cap.value, std::string("a b.c,d"));

    // a dot, slash, plus, or colon in the KEY is refused (key grammar is [A-Za-z0-9_-])
    CHECK(!ols::gitcore::v2CapabilityOf("a.b=c\n", cap, error));
    CHECK(error.find("key carries a character outside") != std::string::npos);
    CHECK(!ols::gitcore::v2CapabilityOf("a/b=c\n", cap, error));
    CHECK(!ols::gitcore::v2CapabilityOf("a+b=c\n", cap, error));
    CHECK(!ols::gitcore::v2CapabilityOf("a:b=c\n", cap, error));

    // an empty capability line carries no key
    CHECK(!ols::gitcore::v2CapabilityOf("\n", cap, error));
    CHECK(error.find("no key") != std::string::npos);

    // an empty value is refused
    CHECK(!ols::gitcore::v2CapabilityOf("agent=\n", cap, error));
    CHECK(error.find("empty value") != std::string::npos);

    // a line that does not end a line is refused
    CHECK(!ols::gitcore::v2CapabilityOf("agent=x", cap, error));
    CHECK(error.find("does not end a line") != std::string::npos);
}

// ------------------------------------------------- the reader's position

TEST(wire_pkt_reader_position)
{
    std::string error;
    // the real smart-HTTP sequence: the service preamble, a flush, then a
    // command line, then a flush — assert the reader lands where commandSection
    // expects, byte for byte.
    const std::string buf =
        "001f# service=git-receive-pack\n" // 27; +4 = 31 = 0x1f
        "0000"                            // the flush after the preamble
        "0014command=ls-refs\n"           // 16; +4 = 20 = 0x14
        "0000";                           // the end of the request
    const std::vector<std::uint8_t> bytes = wireBytesOf(buf);
    PktReader reader(bytes.data(), bytes.size());
    PktKind kind;
    std::string_view view;

    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Data);
    CHECK(view == "# service=git-receive-pack\n");
    CHECK_EQ(reader.position(), 31u); // one past the service line

    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Flush);
    CHECK_EQ(reader.position(), 35u); // one past the flush: where the command section begins

    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Data);
    CHECK(view == "command=ls-refs\n");
    CHECK_EQ(reader.position(), 55u); // 35 + 20

    CHECK(reader.next(kind, view, error));
    CHECK(kind == PktKind::Flush);
    CHECK_EQ(reader.position(), 59u); // 55 + 4

    CHECK(reader.exhausted());
}
