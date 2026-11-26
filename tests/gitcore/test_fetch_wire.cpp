// tests/gitcore/test_fetch_wire.cpp — the cases that prove fetch_wire.h.
//
// No fixtures and no repository: the bytes of these protocol shapes are
// written here with their lengths counted in the comments, the way the wire
// cases keep their counts beside the bytes (test_wire.cpp:69-86), and the
// readers are held to exactly those. Where a writer of wire.h or of the
// receive-pack door can stand the same bytes, the case builds with that writer
// and reads them back with this unit — the round trip is the load-bearing pin,
// because a reader that agrees with the writer agrees with what the server
// sends. The empty pack these cases hand over is the one test_wire.cpp:88-99
// stands (4 + 8 + 20 = 32 bytes: header, zero entries, trailer).
#include "gitcore/fetch_wire.h"
#include "gitcore/wire.h"
#include "gitcore/wire.cpp" // the ground the writers stand on, reached the way test_wire.cpp reaches it; the guard in that file keeps the second landing out

#include <cstdint>
#include <string>
#include <vector>

namespace {

// main.cpp pulls this file into the same translation unit as test_wire.cpp,
// so nothing is imported into a shared scope here: every name is qualified.
// ids a fixture repo really has, so the bytes here name values git made.
const char *const kFetchCommit = "a867ef798b215db19eaf0c7b615c789792fe62b9";
const char *const kFetchTree = "d25250f5f7f874caffa476f3a69161b879d2cbc6";
const char *const kFetchZero = "0000000000000000000000000000000000000000";

std::vector<std::uint8_t> fwBytesOf(const std::string &text)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size());
    for (char c : text)
    {
        bytes.push_back(static_cast<std::uint8_t>(c));
    }
    return bytes;
}

std::string fwTextOf(const std::vector<std::uint8_t> &bytes)
{
    std::string text;
    text.reserve(bytes.size());
    for (std::uint8_t b : bytes)
    {
        text.push_back(static_cast<char>(b));
    }
    return text;
}

// the empty pack git sends: header, version, zero entries, trailer.
std::string fwEmptyPack()
{
    std::string pack = "PACK";
    for (int i = 0; i < 8; ++i)
    {
        pack.push_back(static_cast<char>(0));
    }
    pack += std::string(kFetchCommit).substr(0, 20u);
    return pack; // 4 + 8 + 20 = 32 bytes
}

ols::gitcore::LsRefsEntry fwEntry(const std::string &name, const std::string &oidHex)
{
    ols::gitcore::LsRefsEntry entry;
    entry.name = name;
    entry.oid = ols::gitcore::ObjectId(oidHex);
    return entry;
}

} // anonymous namespace

// --------------------------------------------------------- v2 advertisement

TEST(fetch_v2_advertisement_round_trip)
{
    std::string error;
    const std::vector<std::string> lines = {
        "command=ls-refs fetch\n",
        "unborn\n",
        "symrefs\n",
        "peel\n",
        "refs-protocol=1\n",
    };
    std::vector<std::uint8_t> written;
    CHECK(ols::gitcore::v2Advertise(lines, written, error));

    std::vector<ols::gitcore::V2Capability> caps;
    CHECK(ols::gitcore::fetchReadV2Advertisement(
        written.data(), written.size(), caps, error));

    // the "version 2" line opens the advertisement and is not a capability
    // (wire.cpp:322), so five lines come back and no line was dropped.
    CHECK_EQ(caps.size(), 5u);
    if (caps.size() != 5u)
    {
        return; // the element claims below stand on the count above
    }
    CHECK_EQ(caps[0].key, std::string("command"));
    CHECK_EQ(caps[0].value, std::string("ls-refs fetch"));
    CHECK_EQ(caps[1].key, std::string("unborn"));
    CHECK(caps[1].value.empty());
    CHECK_EQ(caps[2].key, std::string("symrefs"));
    CHECK(caps[2].value.empty());
    CHECK_EQ(caps[3].key, std::string("peel"));
    CHECK(caps[3].value.empty());
    CHECK_EQ(caps[4].key, std::string("refs-protocol"));
    CHECK_EQ(caps[4].value, std::string("1"));

    // an advertisement that does not open with the version line is refused:
    // "unborn\n" is 7; +4 = 11 = 0x0b
    std::vector<std::uint8_t> other = fwBytesOf("000bunborn\n" "0000");
    caps.clear();
    CHECK(!ols::gitcore::fetchReadV2Advertisement(other.data(), other.size(), caps, error));
    CHECK(error.find("version 2") != std::string::npos);
    CHECK(caps.empty());
}

// --------------------------------------------------------- v0 advertisement

// The bytes receivePackAdvertisement writes (GitSmartHttp.cpp:1319-1362): the
// marker and its flush, then the ref lines with HEAD first and the
// capabilities after the NUL of the first line, then the flush.
const char kFetchV0ReceivePackBytes[] =
    "001f" // "# service=git-receive-pack\n" is 26 + LF = 27; +4 = 31 = 0x1f
    "# service=git-receive-pack\n"
    "0000" // the flush that ends the marker
    "007c" // 40 zeros + " HEAD" = 45; + NUL = 46; + 73 caps = 119; + LF = 120; +4 = 124 = 0x7c
    "0000000000000000000000000000000000000000 HEAD"
    "\0report-status delete-refs side-band-64k quiet ofs-delta agent=git/ols-dev\n"
    "003d" // "refs/heads/main" is 15: 40 + 1 + 15 = 56; + LF = 57; +4 = 61 = 0x3d
    "a867ef798b215db19eaf0c7b615c789792fe62b9 refs/heads/main\n"
    "0000"; // the flush that ends the advertisement

// The bytes are taken with their count: the literal carries the NUL that opens
// the capability list, and a std::string built from the pointer alone stops
// there, which leaves the 007c line claiming bytes that are not there.
const std::string kFetchV0ReceivePack(kFetchV0ReceivePackBytes,
                                      sizeof(kFetchV0ReceivePackBytes) - 1u);

TEST(fetch_v0_advertisement_receive_pack)
{
    std::string error;
    const std::vector<std::uint8_t> bytes = fwBytesOf(kFetchV0ReceivePack);
    ols::gitcore::V0Advertisement out;
    CHECK(ols::gitcore::fetchReadV0Advertisement(bytes.data(), bytes.size(), out, error));

    CHECK_EQ(out.caps.size(), 6u);
    CHECK_EQ(out.entries.size(), 2u);
    if (out.caps.size() != 6u || out.entries.size() != 2u)
    {
        return;
    }
    CHECK_EQ(out.caps[0], std::string("report-status"));
    CHECK_EQ(out.caps[1], std::string("delete-refs"));
    CHECK_EQ(out.caps[2], std::string("side-band-64k"));
    CHECK_EQ(out.caps[3], std::string("quiet"));
    CHECK_EQ(out.caps[4], std::string("ofs-delta"));
    CHECK_EQ(out.caps[5], std::string("agent=git/ols-dev"));

    // HEAD first, as the door orders it (GitSmartHttp.cpp:1299-1316), and the
    // id of nothing on its line is an unborn HEAD (GitSmartHttp.cpp:1346).
    CHECK_EQ(out.entries[0].name, std::string("HEAD"));
    CHECK(out.entries[0].unborn);
    CHECK_EQ(out.entries[0].oid.hex, std::string(kFetchZero));
    CHECK_EQ(out.entries[1].name, std::string("refs/heads/main"));
    CHECK(!out.entries[1].unborn);
    CHECK_EQ(out.entries[1].oid.hex, std::string(kFetchCommit));

    CHECK(out.hasHead);
    CHECK_EQ(out.head.name, std::string("HEAD"));
    CHECK(out.head.unborn);
}

// upload-pack states no marker line at all: the ref lines open the
// advertisement, and the capabilities still stand after the NUL of the first
// one. The same reader reads both shapes.
const char kFetchV0UploadPackBytes[] =
    "0049" // 40 + " HEAD" = 45; + NUL = 46; + "multi_ack side-effects" = 22 -> 68; + LF = 69; +4 = 73 = 0x49
    "0000000000000000000000000000000000000000 HEAD"
    "\0multi_ack side-effects\n"
    "0000";

// Taken with the count for the same reason as the advertisement above.
const std::string kFetchV0UploadPack(kFetchV0UploadPackBytes,
                                     sizeof(kFetchV0UploadPackBytes) - 1u);

TEST(fetch_v0_advertisement_no_marker)
{
    std::string error;
    const std::vector<std::uint8_t> bytes = fwBytesOf(kFetchV0UploadPack);
    ols::gitcore::V0Advertisement out;
    CHECK(ols::gitcore::fetchReadV0Advertisement(bytes.data(), bytes.size(), out, error));

    CHECK_EQ(out.caps.size(), 2u);
    CHECK_EQ(out.entries.size(), 1u);
    if (out.caps.size() != 2u || out.entries.size() != 1u)
    {
        return;
    }
    CHECK_EQ(out.caps[0], std::string("multi_ack"));
    CHECK_EQ(out.caps[1], std::string("side-effects"));
    CHECK_EQ(out.entries[0].name, std::string("HEAD"));
    CHECK(out.entries[0].unborn);
    CHECK(out.hasHead);
}

// --------------------------------------------------------- ls-refs answer

TEST(fetch_lsrefs_answer_round_trip)
{
    std::string error;
    ols::gitcore::LsRefsRequest asked;
    asked.unborn = true;
    asked.peel = true;
    asked.symrefs = true;

    std::vector<ols::gitcore::LsRefsEntry> entries;
    entries.push_back(fwEntry("refs/heads/main", kFetchCommit));
    ols::gitcore::LsRefsEntry head = fwEntry("HEAD", kFetchCommit);
    head.isSymref = true;
    head.symrefTarget = "refs/heads/main";
    entries.push_back(head);
    ols::gitcore::LsRefsEntry tag = fwEntry("refs/tags/v1", kFetchTree);
    tag.peeled = true;
    tag.peeledOid = ols::gitcore::ObjectId(std::string(kFetchCommit));
    entries.push_back(tag);
    ols::gitcore::LsRefsEntry gone;
    gone.name = "refs/heads/gone";
    gone.isSymref = true;
    gone.symrefTarget = "refs/heads/gone"; // no id under it: the unborn line
    entries.push_back(gone);

    std::vector<std::uint8_t> written;
    CHECK(ols::gitcore::lsRefsAnswer(entries, asked, written, error));

    std::vector<ols::gitcore::LsRefsEntry> read;
    CHECK(ols::gitcore::fetchReadLsRefsAnswer(
        written.data(), written.size(), read, error));

    CHECK_EQ(read.size(), entries.size());
    if (read.size() != entries.size())
    {
        return;
    }
    for (std::size_t at = 0u; at < entries.size(); ++at)
    {
        CHECK_EQ(read[at].name, entries[at].name);
        CHECK_EQ(read[at].oid.hex, entries[at].oid.hex);
        CHECK_EQ(read[at].isSymref, entries[at].isSymref);
        CHECK_EQ(read[at].symrefTarget, entries[at].symrefTarget);
        CHECK_EQ(read[at].peeled, entries[at].peeled);
        CHECK_EQ(read[at].peeledOid.hex, entries[at].peeledOid.hex);
    }

    // the unborn line is the one form the writer states (wire.cpp:642), and
    // the reader puts the id of nothing back under it.
    CHECK(fwTextOf(written).find("unborn refs/heads/gone symref-target:refs/heads/gone") !=
          std::string::npos);
    CHECK(read[3u].oid.is_null());
}

// ---------------------------------------------------------- fetch answer

TEST(fetch_answer_round_trip_done)
{
    std::string error;
    ols::gitcore::FetchRequest asked;
    asked.wants.push_back(std::string(kFetchCommit));
    asked.done = true;

    ols::gitcore::MemoryObjectInfo holds({std::string(kFetchCommit)});
    const std::string pack = fwEmptyPack();
    std::vector<std::uint8_t> written;
    CHECK(ols::gitcore::fetchAnswer(asked, holds, pack, written, error));

    ols::gitcore::FetchAnswerSections read;
    CHECK(ols::gitcore::fetchReadAnswer(written.data(), written.size(), read, error));

    // "done" means no acknowledgments section at all (wire.cpp:885-919).
    CHECK(read.packfile);
    CHECK_EQ(read.packBytes, pack);
    CHECK(!read.acknowledgments);
    CHECK(!read.nak);
    CHECK(!read.ready);
    CHECK(read.acks.empty());
    CHECK(read.errLines.empty());
    CHECK(read.progress.empty());
    CHECK(read.fatal.empty());
}

TEST(fetch_answer_round_trip_no_done)
{
    std::string error;
    ols::gitcore::FetchRequest asked;
    asked.wants.push_back(std::string(kFetchCommit));
    asked.haves.push_back(std::string(kFetchTree));
    asked.done = false;

    ols::gitcore::MemoryObjectInfo holds({std::string(kFetchCommit), std::string(kFetchTree)});
    const std::string pack = fwEmptyPack();
    std::vector<std::uint8_t> written;
    CHECK(ols::gitcore::fetchAnswer(asked, holds, pack, written, error));

    ols::gitcore::FetchAnswerSections read;
    CHECK(ols::gitcore::fetchReadAnswer(written.data(), written.size(), read, error));

    CHECK(read.acknowledgments);
    CHECK_EQ(read.acks.size(), 1u);
    if (read.acks.size() != 1u)
    {
        return;
    }
    CHECK_EQ(read.acks[0], std::string(kFetchTree));
    CHECK(!read.nak);
    CHECK(read.ready);
    CHECK(read.readyOid.empty());
    CHECK(read.packfile);
    CHECK_EQ(read.packBytes, pack);
}

TEST(fetch_answer_already_holds_everything)
{
    std::string error;
    ols::gitcore::FetchRequest asked;
    asked.wants.push_back(std::string(kFetchCommit));
    asked.haves.push_back(std::string(kFetchCommit));
    asked.done = true;

    ols::gitcore::MemoryObjectInfo holds({std::string(kFetchCommit)});
    std::vector<std::uint8_t> written;
    // no pack is in hand: the answer frames no pack at all (wire.cpp:925-930)
    CHECK(ols::gitcore::fetchAnswer(asked, holds, std::string(), written, error));
    CHECK_EQ(fwTextOf(written), std::string("000d" "packfile\n" "0000"));

    ols::gitcore::FetchAnswerSections read;
    CHECK(ols::gitcore::fetchReadAnswer(written.data(), written.size(), read, error));
    CHECK(read.packfile);
    CHECK(read.packBytes.empty());
    CHECK(read.progress.empty());
    CHECK(read.fatal.empty());
}

TEST(fetch_answer_err_want_road)
{
    std::string error;
    ols::gitcore::FetchRequest asked;
    asked.wants.push_back(std::string(kFetchCommit));
    asked.done = true;

    ols::gitcore::MemoryObjectInfo holds{std::vector<std::string>{}}; // nothing is held
    std::vector<std::uint8_t> written;
    CHECK(ols::gitcore::fetchAnswer(asked, holds, std::string(), written, error));

    ols::gitcore::FetchAnswerSections read;
    CHECK(ols::gitcore::fetchReadAnswer(written.data(), written.size(), read, error));

    // one "ERR want <oid> cannot be found on the server" line per such want
    // and the flush (wire.cpp:859-871): no packfile section, no pack.
    CHECK_EQ(read.errLines.size(), 1u);
    if (read.errLines.size() != 1u)
    {
        return;
    }
    CHECK_EQ(read.errLines[0],
             std::string("ERR want ") + kFetchCommit + " cannot be found on the server");
    CHECK(!read.packfile);
    CHECK(read.packBytes.empty());
    CHECK(!read.acknowledgments);
}

// ------------------------------------------------------------- sideband

TEST(fetch_sideband_progress_interleaved)
{
    std::string error;
    // channel 1, then a progress frame, then channel 1 again: the pack bytes
    // are concatenated in order and the progress text is kept apart.
    const std::string frames =
        "0008" "\1abc" // "abc" 3; +channel = 4; +4 = 8 = 0x08
        "0015" "\2Counting objects" // 16; +1 = 17; +4 = 21 = 0x15
        "0008" "\1def"
        "0000"; // the flush that ends the section
    const std::vector<std::uint8_t> bytes = fwBytesOf(frames);
    ols::gitcore::SidebandStream stream;
    CHECK(ols::gitcore::fetchDemuxSideband(bytes.data(), bytes.size(), stream, error));

    CHECK_EQ(stream.packBytes, std::string("abcdef"));
    CHECK_EQ(stream.progress.size(), 1u);
    if (stream.progress.size() != 1u)
    {
        return;
    }
    CHECK_EQ(stream.progress[0], std::string("Counting objects"));
    CHECK(stream.fatal.empty());
}

TEST(fetch_sideband_channel3_fatal)
{
    std::string error;
    const std::string frames =
        "0008" "\1abc"
        "0009" "\3boom" // 4; +1 = 5; +4 = 9 = 0x09
        "0008" "\1def"
        "0000";
    const std::vector<std::uint8_t> bytes = fwBytesOf(frames);
    ols::gitcore::SidebandStream stream;
    CHECK(!ols::gitcore::fetchDemuxSideband(bytes.data(), bytes.size(), stream, error));

    // the walk stops at the fatal frame and reports its words: what came
    // before it is kept, what stands after it is not read.
    CHECK_EQ(stream.fatal, std::string("boom"));
    CHECK_EQ(stream.packBytes, std::string("abc"));
    CHECK(stream.progress.empty());
    CHECK(error.find("boom") != std::string::npos);
}

// ------------------------------------------------- v0 upload-pack request

TEST(fetch_v0_upload_pack_request_bytes)
{
    std::string error;
    ols::gitcore::V0UploadPackRequest asked;
    asked.wants.push_back(std::string(kFetchCommit));
    asked.wants.push_back(std::string(kFetchTree));
    asked.haves.push_back(std::string(kFetchTree));
    asked.caps.push_back("multi_ack");
    asked.caps.push_back("agent=ols/f1");
    asked.done = true;

    std::vector<std::uint8_t> out;
    CHECK(ols::gitcore::v0BuildUploadPackRequest(asked, out, error));

    std::string expected;
    // "want "+40+NUL+"multi_ack agent=ols/f1" = 45+1+9+1+12 = 68; +LF = 69; +4 = 73 = 0x49
    expected += "0049";
    expected += std::string("want ") + kFetchCommit;
    expected.push_back('\0');
    expected += "multi_ack agent=ols/f1\n";
    // a later want carries no capabilities: "want "+40+LF = 46; +4 = 50 = 0x32
    expected += "0032";
    expected += std::string("want ") + kFetchTree + "\n";
    expected += "0032";
    expected += std::string("have ") + kFetchTree + "\n";
    // "done\n" is 5; +4 = 9 = 0x09, and the flush ends the request
    expected += "0009done\n";
    expected += "0000";
    CHECK_EQ(fwTextOf(out), expected);

    // the capabilities stand after the NUL of the first want and nowhere else:
    // 4 length bytes + "want " + 40 hex.
    CHECK_EQ(fwTextOf(out).find('\0'), 49u);
    CHECK_EQ(fwTextOf(out).find("multi_ack"), 50u);

    // a bounded done names the oid the client stopped at.
    asked.doneOid = std::string(kFetchTree);
    out.clear();
    CHECK(ols::gitcore::v0BuildUploadPackRequest(asked, out, error));
    // "done "+40+LF = 46; +4 = 50 = 0x32
    CHECK(fwTextOf(out).find("0032done " + std::string(kFetchTree) + "\n0000") !=
          std::string::npos);

    // a request with no want is refused and out keeps no part of it.
    asked.wants.clear();
    out.clear();
    CHECK(!ols::gitcore::v0BuildUploadPackRequest(asked, out, error));
    CHECK(error.find("no want") != std::string::npos);
    CHECK(out.empty());
}
