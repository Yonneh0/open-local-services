// tests/gitcore/test_receive.cpp — the cases that prove receive.h's answer.
//
// No fixtures and no repository: the port receive.h names is stood in here
// with the ids and refs these cases name, exactly as the fetch cases stand in
// the object-info callback. The pack is the empty pack the packfile unit
// writes, so the commands are judged on the policy alone and their answers are
// the words this unit owns. The bytes of the answer are written here to
// gitprotocol-pack's report-status with the lengths counted in the comments,
// the way the wire cases keep their counts beside the bytes.

#include "gitcore/receive.h"
#include "gitcore/packfile.h" // the empty pack this case hands over

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// The door library (handleReceivePack, receivePackAdvertisement, RepoPort) is
// pulled in here as a unity include, the way the harness reaches the wire unit
// through test_wire.cpp's include. Its own include of gitcore/wire.cpp is
// skipped by that unit's guard, which wire.cpp stands against exactly this
// second landing; the inline wire definitions the door asks for already stand
// in this translation unit from test_wire.cpp. Compiling the door here is what
// makes the door's hardening verifiable: the service preamble, the sideband
// caps scan, the NUL-folded refusals, and the advertised capability set are
// claimed by the cases at the foot of this file as well as by the raw-socket
// probes against a scratch serve.
#include "GitSmartHttp.cpp"

namespace {

using ols::gitcore::ObjectKind;
using ols::gitcore::RecordKind;
using ols::gitcore::ReceiveCommand;
using ols::gitcore::ReceivePolicy;
using ols::gitcore::RepoAccess;
using ols::gitcore::RefUpdate;
// The all-zeros id, stated here as the cases have always stated it. The door
// library pulled in above keeps its own inside ols::git, which this file's
// global scope does not see, so the two do not meet.
const char kZeroOid[] = "0000000000000000000000000000000000000000";

// ids a fixture repo really has, so the ids here name values git made.
const char *const kStubCommit = "a867ef798b215db19eaf0c7b615c789792fe62b9";
const char *const kStubRoot = "d25250f5f7f874caffa476f3a69161b879d2cbc6";

std::vector<std::uint8_t> bytesOfText(const std::string &text)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size());
    for (char c : text)
    {
        bytes.push_back(static_cast<std::uint8_t>(c));
    }
    return bytes;
}

std::string textOfBytes(const std::vector<std::uint8_t> &bytes)
{
    std::string text;
    text.reserve(bytes.size());
    for (std::uint8_t b : bytes)
    {
        text.push_back(static_cast<char>(b));
    }
    return text;
}

// The port receive.h asks the caller to implement, stood in with what this
// case names: the ids it holds, and the refs it points at. It reads no
// object — so the ancestry a move claims is never proved here — and it
// counts the moves it is asked for, so a case can see that a refused
// command moved nothing while the one that stood did move.
class StubPort : public RepoAccess
{
public:
    explicit StubPort(const std::vector<std::string> &heldOids,
                      const std::vector<std::pair<std::string, std::string>> &refs)
        : held_(heldOids), refs_(refs)
    {
    }

    bool hasObject(const std::string &oidHex) const override
    {
        for (const std::string &held : held_)
        {
            if (held == oidHex)
            {
                return true;
            }
        }
        return false;
    }

    // nothing is read: the walk this unit runs ends at the head it was given.
    bool readObject(const std::string &oidHex, std::string &kindOut,
                    std::string &contentOut) const override
    {
        (void)oidHex;
        kindOut.clear();
        contentOut.clear();
        return false;
    }

    // no object is written: the empty pack carries none.
    bool writeLoose(const std::string &kind, const std::string &content,
                    std::string &oidHex) override
    {
        (void)kind;
        (void)content;
        oidHex.clear();
        return false;
    }

    bool refValue(const std::string &name, std::string &oidHex) const override
    {
        for (const std::pair<std::string, std::string> &ref : refs_)
        {
            if (ref.first == name)
            {
                oidHex = ref.second;
                return true;
            }
        }
        oidHex.clear();
        return false;
    }

    // the moves are recorded, not made: the case asks what the unit decided,
    // and a count is the honest answer to what a refused command left behind.
    bool commitRefs(const std::vector<RefUpdate> &updates) override
    {
        moves_ += updates.size();
        return true;
    }

    bool deleteRefs(const std::vector<std::string> &names) override
    {
        moves_ += names.size();
        return true;
    }

    std::size_t moves() const { return moves_; }

private:
    std::vector<std::string> held_;
    std::vector<std::pair<std::string, std::string>> refs_;
    std::size_t moves_ = 0u;
};

// The empty pack, as the packfile unit writes one: header, version 2, zero
// records, and the trailer that is the sha-1 of those twelve bytes.
std::vector<std::uint8_t> emptyPackBytes()
{
    std::vector<std::uint8_t> pack;
    std::vector<ols::gitcore::IndexEntry> ignored;
    if (!ols::gitcore::writePack(std::vector<ols::gitcore::LooseForPack>(), pack, ignored))
    {
        pack.clear();
    }
    return pack;
}

// The three commands this case answers: a create that stands, a move that
// rewrites history where the policy did not allow one, and the same move
// where the branch is protected.
std::vector<ReceiveCommand> stubCommands()
{
    std::vector<ReceiveCommand> cmds;
    ReceiveCommand create;
    create.name = "refs/tags/v1";
    create.oldOid = std::string(kZeroOid); // all zeros is a create
    create.newOid = std::string(kStubCommit);
    cmds.push_back(create);

    ReceiveCommand move;
    move.name = "refs/heads/main";
    move.oldOid = std::string(kStubRoot);
    move.newOid = std::string(kStubCommit);
    cmds.push_back(move);

    ReceiveCommand guarded;
    guarded.name = "refs/heads/protected";
    guarded.oldOid = std::string(kStubRoot);
    guarded.newOid = std::string(kStubCommit);
    cmds.push_back(guarded);
    return cmds;
}

// The port with objects behind its ids: a thin push leaves out the base a
// delta stands on, and the base of a real one stands in the repository. The
// held triples are (id, kind word, content); what the unit writes is kept, so
// a case sees the object that landed.
class HoldingPort : public RepoAccess
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
        if (at == held_.end()) {
            kindOut.clear();
            contentOut.clear();
            return false;
        }
        kindOut = at->second.first;
        contentOut = at->second.second;
        return true;
    }

    // written the way the repository holds it: the id of the bytes is what
    // comes back, and the case checks it against the id the pack names.
    bool writeLoose(const std::string &kind, const std::string &content,
                    std::string &oidHex) override
    {
        ObjectKind objectKind = ObjectKind::Blob;
        std::string why;
        if (!ols::gitcore::kindOfName(kind, objectKind, why)) {
            oidHex.clear();
            return false;
        }
        const std::string raw =
            ols::gitcore::objectHeaderMake(objectKind, content.size()) + content;
        oidHex = ols::gitcore::sha1Hex(raw);
        held_[oidHex] = std::make_pair(kind, content);
        written_.push_back(oidHex);
        return true;
    }

    bool refValue(const std::string &name, std::string &oidHex) const override
    {
        for (const std::pair<std::string, std::string> &ref : refs_) {
            if (ref.first == name) {
                oidHex = ref.second;
                return true;
            }
        }
        oidHex.clear();
        return false;
    }

    bool commitRefs(const std::vector<RefUpdate> &updates) override
    {
        moves_ += updates.size();
        return true;
    }

    bool deleteRefs(const std::vector<std::string> &names) override
    {
        moves_ += names.size();
        return true;
    }

    std::size_t moves() const { return moves_; }
    const std::vector<std::string> &written() const { return written_; }

    void addRef(const std::string &name, const std::string &oid) { refs_.push_back(std::make_pair(name, oid)); }

private:
    std::map<std::string, std::pair<std::string, std::string>> held_;
    std::vector<std::pair<std::string, std::string>> refs_;
    std::vector<std::string> written_;
    std::size_t moves_ = 0u;
};

// The id of an object as the bytes state it: the hash of the header and the
// content together.
std::string objectOidOf(ObjectKind kind, const std::string &content)
{
    return ols::gitcore::sha1Hex(ols::gitcore::objectHeaderMake(kind, content.size()) + content);
}

// One hex digit to its value.
int hexValueOf(char c)
{
    return (c >= '0' && c <= '9') ? static_cast<int>(c - '0') : static_cast<int>(c - 'a' + 10);
}

// The 20 bytes an id names, and the hex of bytes, the two directions the pack
// format carries its ids in.
std::vector<std::uint8_t> rawOfHex(const std::string &hex)
{
    std::vector<std::uint8_t> raw;
    for (std::size_t i = 0u; i + 1u < hex.size(); i += 2u) {
        raw.push_back(static_cast<std::uint8_t>((hexValueOf(hex[i]) << 4) | hexValueOf(hex[i + 1u])));
    }
    return raw;
}

std::string hexOfBytes(const std::vector<std::uint8_t> &bytes)
{
    std::string hex;
    for (std::uint8_t b : bytes) {
        hex.push_back("0123456789abcdef"[b >> 4u]);
        hex.push_back("0123456789abcdef"[b & 0x0fu]);
    }
    return hex;
}

// A record's header: the kind's code in the high nibbles of the first byte with
// the low four bits of the size riding in its low ones, and the size's
// remaining groups seven bits to a byte, low parts first. The mirror of what
// receive.cpp reads, written here so the case states its own bytes.
std::vector<std::uint8_t> recordHeadOf(int code, std::uint64_t size)
{
    std::vector<std::uint8_t> head;
    head.push_back(static_cast<std::uint8_t>((static_cast<unsigned>(code) << 4u) | (size & 0x0fu)));
    size >>= 4u;
    while (size != 0ull) {
        head.back() = static_cast<std::uint8_t>(head.back() | 0x80u);
        head.push_back(static_cast<std::uint8_t>(size & 0x7fu));
        size >>= 7u;
    }
    return head;
}

// A delta over a base as git writes it: the base's size, the target's, one copy
// of the whole base, then the bytes the target holds beyond the base. The base
// stays under 256 bytes, where one size byte carries the copy.
std::vector<std::uint8_t> deltaOverBase(const std::string &base, const std::string &extra)
{
    std::vector<std::uint8_t> delta;
    std::vector<std::uint8_t> varint;
    ols::gitcore::encodeSizeVarint(base.size(), varint);
    delta.insert(delta.end(), varint.begin(), varint.end());
    ols::gitcore::encodeSizeVarint(base.size() + extra.size(), varint);
    delta.insert(delta.end(), varint.begin(), varint.end());
    delta.push_back(static_cast<std::uint8_t>(0x80u | 0x10u)); // copy: size in one byte
    delta.push_back(static_cast<std::uint8_t>(base.size()));
    delta.push_back(static_cast<std::uint8_t>(extra.size())); // insert: the bytes that follow
    delta.insert(delta.end(), extra.begin(), extra.end());
    return delta;
}

// One record of a pack being built: its header and its deflated body.
struct BuiltRecord
{
    std::vector<std::uint8_t> head;
    std::vector<std::uint8_t> body;
};

BuiltRecord refDeltaRecord(const std::string &baseOid, const std::vector<std::uint8_t> &delta)
{
    BuiltRecord record;
    record.head = recordHeadOf(static_cast<int>(RecordKind::RefDelta), delta.size());
    const std::vector<std::uint8_t> rawBase = rawOfHex(baseOid);
    record.head.insert(record.head.end(), rawBase.begin(), rawBase.end());
    if (!ols::gitcore::deflateStored(delta, record.body)) {
        record.body.clear();
    }
    return record;
}

// A bare pack: the header, the records in order, and the trailer that is the
// sha-1 of all the bytes above them — what a v2 push hands over after the
// command's flush, the index left to the receiver to make.
std::vector<std::uint8_t> barePackOf(const std::vector<BuiltRecord> &records)
{
    std::vector<std::uint8_t> pack;
    pack.push_back(static_cast<std::uint8_t>('P'));
    pack.push_back(static_cast<std::uint8_t>('A'));
    pack.push_back(static_cast<std::uint8_t>('C'));
    pack.push_back(static_cast<std::uint8_t>('K'));
    pack.push_back(0u);
    pack.push_back(0u);
    pack.push_back(0u);
    pack.push_back(2u); // version 2
    const std::uint32_t count = static_cast<std::uint32_t>(records.size());
    pack.push_back(static_cast<std::uint8_t>(count >> 24));
    pack.push_back(static_cast<std::uint8_t>(count >> 16));
    pack.push_back(static_cast<std::uint8_t>(count >> 8));
    pack.push_back(static_cast<std::uint8_t>(count));
    for (const BuiltRecord &record : records) {
        pack.insert(pack.end(), record.head.begin(), record.head.end());
        pack.insert(pack.end(), record.body.begin(), record.body.end());
    }
    const std::string digest =
        ols::gitcore::sha1Hex(std::string(pack.begin(), pack.end()));
    const std::vector<std::uint8_t> trailer = rawOfHex(digest);
    pack.insert(pack.end(), trailer.begin(), trailer.end());
    return pack;
}

} // anonymous namespace

TEST(report_status_names_each_command_itself)
{
    std::string error;
    const std::vector<std::uint8_t> pack = emptyPackBytes();
    CHECK(!pack.empty());

    StubPort port({std::string(kStubCommit), std::string(kStubRoot)},
                  {{"refs/heads/main", std::string(kStubRoot)},
                   {"refs/heads/protected", std::string(kStubRoot)}});

    ReceivePolicy policy;
    policy.protectedBranches.push_back("refs/heads/protected");
    policy.allowNonFastForward = false;

    std::vector<std::pair<std::string, std::string>> results;
    bool packFailed = false;
    CHECK(ols::gitcore::receive(port, pack, stubCommands(), policy, results, packFailed, error));
    CHECK_EQ(results.size(), 3u);
    CHECK(!packFailed); // the body stood; what refused was a command, not the pack
    CHECK_EQ(results[0].first, std::string("refs/tags/v1"));
    CHECK_EQ(results[0].second, std::string()); // it moved
    CHECK_EQ(results[1].second, std::string("not a fast-forward: the history would be rewritten"));
    CHECK_EQ(results[2].second, std::string("a protected branch takes a fast-forward only"));
    CHECK_EQ(port.moves(), 1u); // only the command that stood reached the port

    // report-status: the unpack line reads "unpack ok", and each command
    // carries its own line. gitprotocol-pack reserves the unpack-ng line for
    // a body that did not unpack, so a command's refusal never leads with it.
    std::vector<std::uint8_t> out;
    CHECK(ols::gitcore::reportStatus(results, out, error));

    std::string expected;
    // "unpack ok\n": 10; +4 = 14 = 0x0e
    expected += "000eunpack ok\n";
    // "ok refs/tags/v1\n": 3+12+1 = 16; +4 = 20 = 0x14
    expected += "0014ok refs/tags/v1\n";
    // "ng refs/heads/main not a fast-forward: the history would be rewritten\n":
    // 3+15+1+50+1 = 70; +4 = 74 = 0x4a
    expected += "004ang refs/heads/main not a fast-forward: the history would be rewritten\n";
    // "ng refs/heads/protected a protected branch takes a fast-forward only\n":
    // 3+20+1+44+1 = 69; +4 = 73 = 0x49
    expected += "0049ng refs/heads/protected a protected branch takes a fast-forward only\n";
    expected += "0000";
    const std::string answer = textOfBytes(out);
    CHECK_EQ(answer, expected);
    CHECK(answer.find("unpack ng") == std::string::npos);

    // the same commands with a body that did not unpack: the pack is the
    // empty pack with a version this layer does not read.
    std::vector<std::uint8_t> wrongVersion = pack;
    wrongVersion[7u] = 0u;
    std::vector<std::pair<std::string, std::string>> bodyResults;
    bool bodyFailed = false;
    CHECK(!ols::gitcore::receive(port, wrongVersion, stubCommands(), policy, bodyResults, bodyFailed, error));
    CHECK(bodyFailed); // the claim a receive answer may lead "unpack ng" with
    CHECK_EQ(bodyResults.size(), 0u);
    CHECK(error.find("not the version this layer reads") != std::string::npos);

    // and that is the only answer that leads with it: the unpack-ng line, and
    // the per-command lines still follow it.
    out.clear();
    CHECK(ols::gitcore::reportStatus(results, std::string("the pack would not open"), out, error));
    std::string ngAnswer;
    // "unpack ng the pack would not open\n": 10+23+1 = 34; +4 = 38 = 0x26
    ngAnswer += "0026unpack ng the pack would not open\n";
    // the per-command lines are the ones the answer above just made: the
    // unpack line is replaced, the flush is kept.
    ngAnswer += expected.substr(14u, expected.size() - 14u - 4u);
    ngAnswer += "0000";
    CHECK_EQ(textOfBytes(out), ngAnswer);
}

// A thin push: git leaves the base a delta stands on out of the pack when the
// repository holds it, and names it by id. The base is read through the port,
// the delta applies against it, and the object the pair makes is the one the
// command names.
TEST(a_thin_pack_whose_base_the_repository_holds_is_received)
{
    const std::string baseContent = "the bytes a delta stands on, held in the repository";
    const std::string baseOid = objectOidOf(ObjectKind::Blob, baseContent);
    const std::string extra = " plus what the delta brings";
    const std::string target = baseContent + extra;
    const std::string targetOid = objectOidOf(ObjectKind::Blob, target);

    std::vector<BuiltRecord> records;
    records.push_back(refDeltaRecord(baseOid, deltaOverBase(baseContent, extra)));
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    HoldingPort port;
    port.hold(baseOid, "blob", baseContent); // the base the pack left out

    ReceiveCommand create;
    create.name = "refs/tags/thin";
    create.oldOid = std::string(kZeroOid);
    create.newOid = targetOid;
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(create);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(!bodyFailed);
    CHECK_EQ(error, std::string());
    CHECK_EQ(results.size(), 1u);
    CHECK_EQ(results[0].first, std::string("refs/tags/thin"));
    CHECK_EQ(results[0].second, std::string()); // the command moved
    CHECK_EQ(port.moves(), 1u);
    // the object the delta made landed; the base the repository already held
    // was not written again.
    CHECK_EQ(port.written().size(), 1u);
    if (!port.written().empty()) {
        CHECK_EQ(port.written()[0], targetOid);
    }
}

// The same pack with the base standing nowhere: neither the pack nor the
// repository carries it, and the refusal is the one this unit has always given.
TEST(a_thin_pack_whose_base_stands_nowhere_is_refused_as_before)
{
    const std::string baseContent = "the bytes a delta stands on, held by nobody";
    const std::string baseOid = objectOidOf(ObjectKind::Blob, baseContent);
    const std::string extra = " plus what the delta brings";

    std::vector<BuiltRecord> records;
    records.push_back(refDeltaRecord(baseOid, deltaOverBase(baseContent, extra)));
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    HoldingPort port; // holds nothing

    ReceiveCommand create;
    create.name = "refs/tags/thin";
    create.oldOid = std::string(kZeroOid);
    create.newOid = "1111111111111111111111111111111111111111";
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(create);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(!ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(bodyFailed); // the body itself did not stand
    CHECK_EQ(error, std::string("a delta in the pack names a base the pack does not carry"));
    CHECK_EQ(results.size(), 0u);
    CHECK_EQ(port.moves(), 0u);
    CHECK_EQ(port.written().size(), 0u);
}

// A chain behind the borrowed base: the second delta stands on the object the
// first one makes, so the repository's base is met once and the pack's own
// records resolve the rest of the way.
TEST(a_thin_pack_chain_resolves_through_the_base_it_borrows)
{
    const std::string baseContent = "the base";
    const std::string baseOid = objectOidOf(ObjectKind::Blob, baseContent);
    const std::string extra1 = "-one";
    const std::string step1 = baseContent + extra1;
    const std::string step1Oid = objectOidOf(ObjectKind::Blob, step1);
    const std::string extra2 = "-two";
    const std::string step2 = step1 + extra2;
    const std::string step2Oid = objectOidOf(ObjectKind::Blob, step2);

    std::vector<BuiltRecord> records;
    records.push_back(refDeltaRecord(baseOid, deltaOverBase(baseContent, extra1)));
    records.push_back(refDeltaRecord(step1Oid, deltaOverBase(step1, extra2)));
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    HoldingPort port;
    port.hold(baseOid, "blob", baseContent);

    ReceiveCommand create;
    create.name = "refs/tags/thin-chain";
    create.oldOid = std::string(kZeroOid);
    create.newOid = step2Oid;
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(create);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(!bodyFailed);
    CHECK_EQ(error, std::string());
    CHECK_EQ(results.size(), 1u);
    CHECK_EQ(results[0].second, std::string());
    CHECK_EQ(port.moves(), 1u);
    // both links of the chain landed as objects of their own.
    CHECK_EQ(port.written().size(), 2u);
    if (port.written().size() == 2u) {
        CHECK(port.written()[0] == step1Oid || port.written()[0] == step2Oid);
        CHECK(port.written()[1] == step1Oid || port.written()[1] == step2Oid);
        CHECK(port.written()[0] != port.written()[1]);
    }
}

// A push that only removes a ref brings no pack at all: git sends nothing
// behind the commands, and a removal names no object for a pack to carry.
TEST(a_delete_only_push_with_no_pack_removes_the_ref)
{
    HoldingPort port;
    port.addRef("refs/heads/gone", kStubCommit);

    ReceiveCommand drop;
    drop.name = "refs/heads/gone";
    drop.oldOid = std::string(kStubCommit);
    drop.newOid = std::string(kZeroOid);
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(drop);

    const std::vector<std::uint8_t> noPack;
    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(ols::gitcore::receive(port, noPack, cmds, policy, results, bodyFailed, error));
    CHECK(!bodyFailed); // an answer with nothing to unpack is not an unpack failure
    CHECK_EQ(error, std::string());
    CHECK_EQ(results.size(), 1u);
    CHECK_EQ(results[0].first, std::string("refs/heads/gone"));
    CHECK_EQ(results[0].second, std::string());
    CHECK_EQ(port.moves(), 1u);
    CHECK_EQ(port.written().size(), 0u);
}

// A pack whose delta stands on a base the pack itself carries: the record's
// A pack whose delta stands on a base the pack itself carries: the record's
// header states no kind of the object it makes, so the object's own header is
// the claim this layer reads, and the command moves.
TEST(a_delta_on_an_in_pack_base_is_received_by_its_objects_kind)
{
    const std::string baseContent = "the base the pack carries beside its delta";
    const std::string baseOid = objectOidOf(ObjectKind::Blob, baseContent);
    const std::string extra = "-and-then-some";
    const std::string target = baseContent + extra;
    const std::string targetOid = objectOidOf(ObjectKind::Blob, target);

    BuiltRecord full;
    full.head = recordHeadOf(static_cast<int>(RecordKind::Blob), baseContent.size());
    if (!ols::gitcore::deflateStored(std::vector<std::uint8_t>(baseContent.begin(),
                                                               baseContent.end()),
                                     full.body)) {
        full.body.clear();
    }
    std::vector<BuiltRecord> records;
    records.push_back(full);
    records.push_back(refDeltaRecord(baseOid, deltaOverBase(baseContent, extra)));
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    HoldingPort port; // the base is in the pack, not the repository

    ReceiveCommand create;
    create.name = "refs/tags/inpack";
    create.oldOid = std::string(kZeroOid);
    create.newOid = targetOid;
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(create);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(!bodyFailed);
    CHECK_EQ(error, std::string());
    CHECK_EQ(results.size(), 1u);
    CHECK_EQ(results[0].second, std::string());
    CHECK_EQ(port.moves(), 1u);
    CHECK_EQ(port.written().size(), 2u); // the base and the object it made
}

// The list the pack's index states runs in id order, not in the order objects
// stand on one another: a commit whose id sorts before its own tree's must not
// be refused a root the same pack carries. The commit is sought by its own
// hash until its id sorts first, which the loop below finds in a step or two.
TEST(a_commit_sorting_before_its_own_tree_in_the_pack_is_received)
{
    const std::string blobContent = "the file the tree names";
    const std::string blobOid = objectOidOf(ObjectKind::Blob, blobContent);
    std::vector<std::uint8_t> treeContent;
    const std::string entryHead("100644 a\0", 9);
    treeContent.insert(treeContent.end(), entryHead.begin(), entryHead.end());
    const std::vector<std::uint8_t> blobRaw = rawOfHex(blobOid);
    treeContent.insert(treeContent.end(), blobRaw.begin(), blobRaw.end());
    const std::string treeOid = objectOidOf(ObjectKind::Tree,
                                            std::string(treeContent.begin(), treeContent.end()));

    std::string commitContent;
    std::string commitOid;
    for (int tryNumber = 0; tryNumber < 5000; ++tryNumber) {
        commitContent = "tree " + treeOid +
                        "\nauthor T <t@example> 0 +0000\ncommitter T <t@example> 0 +0000\n\nm" +
                        std::to_string(tryNumber) + "\n";
        commitOid = objectOidOf(ObjectKind::Commit, commitContent);
        if (commitOid < treeOid) {
            break;
        }
    }
    CHECK(commitOid < treeOid); // the order the pack's list will run in

    std::vector<BuiltRecord> records;
    BuiltRecord blob;
    blob.head = recordHeadOf(static_cast<int>(RecordKind::Blob), blobContent.size());
    const std::vector<std::uint8_t> blobPlain(blobContent.begin(), blobContent.end());
    if (!ols::gitcore::deflateStored(blobPlain, blob.body)) {
        blob.body.clear();
    }
    records.push_back(blob);
    BuiltRecord tree;
    tree.head = recordHeadOf(static_cast<int>(RecordKind::Tree), treeContent.size());
    if (!ols::gitcore::deflateStored(treeContent, tree.body)) {
        tree.body.clear();
    }
    records.push_back(tree);
    BuiltRecord commit;
    commit.head = recordHeadOf(static_cast<int>(RecordKind::Commit), commitContent.size());
    const std::vector<std::uint8_t> commitPlain(commitContent.begin(), commitContent.end());
    if (!ols::gitcore::deflateStored(commitPlain, commit.body)) {
        commit.body.clear();
    }
    records.push_back(commit);
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    HoldingPort port; // nothing held: the pack carries the commit, the tree, and the blob

    ReceiveCommand create;
    create.name = "refs/heads/sorted";
    create.oldOid = std::string(kZeroOid);
    create.newOid = commitOid;
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(create);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(!bodyFailed);
    CHECK_EQ(error, std::string());
    CHECK_EQ(results.size(), 1u);
    CHECK_EQ(results[0].second, std::string());
    CHECK_EQ(port.moves(), 1u);
    CHECK_EQ(port.written().size(), 3u);
}

// ===========================================================================
// The adversarial audit's findings, each reproduced as a claim the unit must
// answer. A case states what the fixed behaviour is; where the unit refused
// wrongly, fell over, or answered unframed, the claim is what it must say
// instead. The door-level cases stand on a scratch repository: the bare repo
// initBare makes, with the objects and refs the commands name put there
// first. Every scratch dir is this case's own and is gone when it leaves.
// ===========================================================================

namespace {

// The scratch bare repo a door case stands on.
class ScratchRepo
{
public:
    explicit ScratchRepo(const std::string &tag)
    {
        dir_ = (ols_test::scratchRoot() / "ols-receive-tests" / tag).string();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        if (!ols::gitcore::initBare(dir_)) {
            dir_.clear();
        }
    }
    ~ScratchRepo()
    {
        std::error_code ec;
        if (!dir_.empty()) {
            std::filesystem::remove_all(dir_, ec);
        }
    }
    bool stood() const { return !dir_.empty(); }
    const std::string &dir() const { return dir_; }

private:
    std::string dir_;
};

// The port that says which mutation reached it, and in what order, with the
// two answers the receive can fail on put under the case's hand.
class TxPort : public RepoAccess
{
public:
    void hold(const std::string &oid) { held_.insert(oid); }
    void addRef(const std::string &name, const std::string &oid)
    {
        refs_.push_back(std::make_pair(name, oid));
    }

    bool hasObject(const std::string &oidHex) const override { return held_.count(oidHex) != 0u; }
    bool readObject(const std::string &, std::string &kindOut, std::string &contentOut) const override
    {
        kindOut.clear();
        contentOut.clear();
        return false;
    }
    bool writeLoose(const std::string &, const std::string &, std::string &oidHex) override
    {
        oidHex.clear();
        return false;
    }
    bool refValue(const std::string &name, std::string &oidHex) const override
    {
        for (const std::pair<std::string, std::string> &ref : refs_) {
            if (ref.first == name) {
                oidHex = ref.second;
                return true;
            }
        }
        oidHex.clear();
        return false;
    }
    bool commitRefs(const std::vector<RefUpdate> &updates) override
    {
        (void)updates;
        calls_.push_back("commit");
        return !failCommit;
    }
    bool deleteRefs(const std::vector<std::string> &names) override
    {
        for (const std::string &name : names) {
            calls_.push_back("delete:" + name);
        }
        return !failDelete;
    }

    bool failCommit = false;
    bool failDelete = false;
    const std::vector<std::string> &calls() const { return calls_; }

private:
    std::set<std::string> held_;
    std::vector<std::pair<std::string, std::string>> refs_;
    std::vector<std::string> calls_;
};

// One v0 command line framed: the caps after the NUL stand where v0 puts them.
std::vector<std::uint8_t> frameLine(const std::string &oldOid, const std::string &newOid,
                                    const std::string &name, const std::string &caps,
                                    std::string &why)
{
    std::string line = oldOid + " " + newOid + " " + name;
    if (!caps.empty()) {
        line.push_back('\0');
        line += caps;
    }
    line.push_back('\n');
    std::vector<std::uint8_t> out;
    if (!ols::gitcore::pktWrite(line, out, why)) {
        out.clear();
    }
    return out;
}

std::string appended(std::vector<std::uint8_t> &bytes, const std::vector<std::uint8_t> &more)
{
    bytes.insert(bytes.end(), more.begin(), more.end());
    return std::string();
}

// A delta of many copies over one base: the two size varints, then `copies`
// copies of 64 KiB at offset zero. What the delta's own varint promises the
// target to be is the case's to state; the instructions themselves would
// made `copies` times 64 KiB — a number a hostile delta can name with a few
// hundred kilobytes of instructions and no untruth to its own arithmetic.
std::vector<std::uint8_t> copyFloodDelta(std::size_t baseSize, std::uint64_t promisedTarget,
                                         std::size_t copies)
{
    std::vector<std::uint8_t> delta;
    std::vector<std::uint8_t> varint;
    ols::gitcore::encodeSizeVarint(baseSize, varint);
    delta.insert(delta.end(), varint.begin(), varint.end());
    ols::gitcore::encodeSizeVarint(promisedTarget, varint);
    delta.insert(delta.end(), varint.begin(), varint.end());
    for (std::size_t i = 0u; i < copies; ++i) {
        delta.push_back(static_cast<std::uint8_t>(0x80u | 0x10u | 0x20u)); // copy, size in two
        delta.push_back(0u);                                               // size 00 00
        delta.push_back(0u);                                               // ... means 64 KiB
        delta.push_back(0u);                                               // no offset bytes
    }
    return delta;
}

// A copy of the whole base as one instruction, for a chain step that also
// inserts its few bytes: the base stays under 64 KiB where two size bytes
// carry the copy.
std::vector<std::uint8_t> chainDelta(const std::string &base, const std::string &extra)
{
    std::vector<std::uint8_t> delta;
    std::vector<std::uint8_t> varint;
    ols::gitcore::encodeSizeVarint(base.size(), varint);
    delta.insert(delta.end(), varint.begin(), varint.end());
    ols::gitcore::encodeSizeVarint(base.size() + extra.size(), varint);
    delta.insert(delta.end(), varint.begin(), varint.end());
    delta.push_back(static_cast<std::uint8_t>(0x80u | 0x10u | 0x20u));
    delta.push_back(static_cast<std::uint8_t>(base.size() & 0xffu));
    delta.push_back(static_cast<std::uint8_t>((base.size() >> 8) & 0xffu));
    delta.push_back(static_cast<std::uint8_t>(extra.size()));
    delta.insert(delta.end(), extra.begin(), extra.end());
    return delta;
}

} // anonymous namespace

// M3: a transaction that does not hold is the one refusal where "no ref
// moved" is true. The removals must not have run when it says so.
TEST(a_refusing_transaction_leaves_the_removals_unrun)
{
    TxPort port;
    port.hold(std::string(kStubCommit));
    port.addRef("refs/heads/gone", std::string(kStubCommit));
    port.failCommit = true;

    std::vector<ReceiveCommand> cmds;
    ReceiveCommand create;
    create.name = "refs/tags/v";
    create.oldOid = std::string(kZeroOid);
    create.newOid = std::string(kStubCommit);
    cmds.push_back(create);
    ReceiveCommand drop;
    drop.name = "refs/heads/gone";
    drop.oldOid = std::string(kStubCommit);
    drop.newOid = std::string(kZeroOid);
    cmds.push_back(drop);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(!ols::gitcore::receive(port, std::vector<std::uint8_t>(), cmds, policy, results,
                                 bodyFailed, error));
    CHECK(!bodyFailed); // the transaction, not the body, is what did not stand
    CHECK(error.find("no ref moved") != std::string::npos);
    // the truth of those words: what reached the port is the transaction, and
    // nothing else — no removal ran behind the promise.
    CHECK_EQ(port.calls().size(), 1u);
    if (!port.calls().empty()) {
        CHECK_EQ(port.calls()[0], std::string("commit"));
    }
}

// M3: with the moves through, a removal that does not go is that one
// command's refusal — the ref that moved is ok, the one that stayed carries
// the words — and never a claim that nothing moved at all.
TEST(a_refused_removal_is_stated_on_its_own_row)
{
    TxPort port;
    port.hold(std::string(kStubCommit));
    port.addRef("refs/heads/gone", std::string(kStubCommit));
    port.failDelete = true;

    std::vector<ReceiveCommand> cmds;
    ReceiveCommand create;
    create.name = "refs/tags/v";
    create.oldOid = std::string(kZeroOid);
    create.newOid = std::string(kStubCommit);
    cmds.push_back(create);
    ReceiveCommand drop;
    drop.name = "refs/heads/gone";
    drop.oldOid = std::string(kStubCommit);
    drop.newOid = std::string(kZeroOid);
    cmds.push_back(drop);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(ols::gitcore::receive(port, std::vector<std::uint8_t>(), cmds, policy, results,
                                bodyFailed, error));
    CHECK(!bodyFailed);
    CHECK_EQ(results.size(), 2u);
    if (results.size() == 2u) {
        CHECK_EQ(results[0].first, std::string("refs/tags/v"));
        CHECK_EQ(results[0].second, std::string()); // it moved
        CHECK_EQ(results[1].first, std::string("refs/heads/gone"));
        CHECK(!results[1].second.empty()); // and its row carries the words
    }
    CHECK(!port.calls().empty());
    CHECK_EQ(port.calls()[0], std::string("commit")); // moves first, removals last
}

// M2: a delta chain past the depth the layer walks (packfile.h's
// kMaxDeltaChain, 1024 links) is refused, not worked through round on
// endless round. 1100 links stand here, each on the one before.
TEST(a_delta_chain_past_the_walked_depth_is_refused)
{
    const std::size_t depth = 1100u;
    std::string content = "chain base";
    std::string oid = objectOidOf(ObjectKind::Blob, content);

    std::vector<BuiltRecord> records;
    BuiltRecord base;
    base.head = recordHeadOf(static_cast<int>(RecordKind::Blob), content.size());
    if (!ols::gitcore::deflateStored(std::vector<std::uint8_t>(content.begin(), content.end()),
                                     base.body)) {
        base.body.clear();
    }
    records.push_back(base);
    for (std::size_t i = 0u; i < depth - 1u; ++i) {
        const std::string extra = "-" + std::to_string(i % 10);
        const std::string next = content + extra;
        records.push_back(refDeltaRecord(oid, chainDelta(content, extra)));
        content = next;
        oid = objectOidOf(ObjectKind::Blob, content);
    }
    // A ref delta names its base by id, so the file's order is the case's to
    // choose: standing the chain the other way up — the base last — is the
    // shape that costs a round per link, and the rounds are what the bound
    // keeps. A chain already in dependency order resolves in one round and is
    // refused later, at the read-back, by the same cap of the packfile unit.
    std::reverse(records.begin(), records.end());
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    HoldingPort port; // the chain stands whole in the pack
    ReceiveCommand create;
    create.name = "refs/tags/deep";
    create.oldOid = std::string(kZeroOid);
    create.newOid = oid;
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(create);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(!ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(bodyFailed);
    CHECK(error.find("chain") != std::string::npos);
    CHECK_EQ(port.moves(), 0u);
}

// C4 (last of the light claims): a delta whose two header varints promise a
// target past the object ceiling is refused at the number, before the
// instructions are run or the bytes are stood up. 512 MiB is the ceiling
// (packfile.h's kMaxObjectBytes); the promise here is 16 TiB, and the
// instructions would really make 12 GiB of it if they ran.
TEST(a_delta_promise_past_the_object_ceiling_is_refused_at_the_number)
{
    const std::string baseContent(512u * 1024u, 'b');
    const std::string baseOid = objectOidOf(ObjectKind::Blob, baseContent);

    std::vector<BuiltRecord> records;
    BuiltRecord base;
    base.head = recordHeadOf(static_cast<int>(RecordKind::Blob), baseContent.size());
    if (!ols::gitcore::deflateStored(std::vector<std::uint8_t>(baseContent.begin(),
                                                               baseContent.end()),
                                     base.body)) {
        base.body.clear();
    }
    records.push_back(base);
    records.push_back(refDeltaRecord(baseOid,
                                     copyFloodDelta(baseContent.size(), 1ull << 44, 400000u)));
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    HoldingPort port;
    ReceiveCommand create;
    create.name = "refs/tags/bomb";
    create.oldOid = std::string(kZeroOid);
    create.newOid = "2222222222222222222222222222222222222222";
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(create);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(!ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(bodyFailed); // the body itself did not stand
    CHECK_EQ(port.moves(), 0u);
    CHECK_EQ(port.written().size(), 0u);
}

// C3: the header's record count is a number the bytes must back before it
// drives any reserve: each record is a header byte and a deflated body, and
// more records than the bytes hold room for is the pack saying it is not a
// pack. 268 million records cannot stand in 32 bytes.
TEST(a_pack_header_naming_more_records_than_the_bytes_hold_is_refused)
{
    std::vector<std::uint8_t> pack = emptyPackBytes();
    CHECK(!pack.empty());
    pack[8u] = 0x0fu;
    pack[9u] = 0xffu;
    pack[10u] = 0xffu;
    pack[11u] = 0xffu;

    HoldingPort port;
    ReceiveCommand create;
    create.name = "refs/tags/count";
    create.oldOid = std::string(kZeroOid);
    create.newOid = "3333333333333333333333333333333333333333";
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(create);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(!ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(bodyFailed);
    CHECK(error.find("record") != std::string::npos);
    CHECK_EQ(port.moves(), 0u);
}

// M1: the pack a thin push is stood up into is read against the same bound
// as the bytes that came in. 33 deltas, each on one 2 MiB base only the
// repository holds, stand up whole as full records past the 64 MiB this
// layer indexes; the rewrite must be refused before it replaces the pack.
TEST(a_stood_up_pack_past_the_bound_is_refused_before_it_lands)
{
    const std::string baseContent(2u * 1024u * 1024u, 'z');
    const std::string baseOid = objectOidOf(ObjectKind::Blob, baseContent);

    std::vector<BuiltRecord> records; // the thin pack: deltas only, no base
    std::string lastOid = baseOid;
    for (int i = 0; i < 33; ++i) {
        const std::string extra = "+" + std::to_string(i / 10) + static_cast<char>('0' + i % 10);
        std::vector<std::uint8_t> delta;
        std::vector<std::uint8_t> varint;
        ols::gitcore::encodeSizeVarint(baseContent.size(), varint);
        delta.insert(delta.end(), varint.begin(), varint.end());
        ols::gitcore::encodeSizeVarint(baseContent.size() + extra.size(), varint);
        delta.insert(delta.end(), varint.begin(), varint.end());
        // the whole 2 MiB base as 32 copies of 64 KiB, each naming its offset
        for (std::size_t c = 0; c < 32u; ++c) {
            const std::uint64_t off = static_cast<std::uint64_t>(c) * 0x10000ull;
            delta.push_back(static_cast<std::uint8_t>(0x80u | 0x03u | 0x30u)); // off two, size two
            delta.push_back(static_cast<std::uint8_t>(off & 0xffu));
            delta.push_back(static_cast<std::uint8_t>((off >> 8) & 0xffu));
            delta.push_back(0u); // size 00 00 ...
            delta.push_back(0u); // ... means 64 KiB
        }
        delta.push_back(static_cast<std::uint8_t>(extra.size())); // insert
        delta.insert(delta.end(), extra.begin(), extra.end());
        records.push_back(refDeltaRecord(baseOid, delta));
        lastOid = objectOidOf(ObjectKind::Blob, baseContent + extra);
    }
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    HoldingPort port; // the base stands in the repository only: a thin push
    port.hold(baseOid, "blob", baseContent);
    ReceiveCommand create;
    create.name = "refs/tags/wide";
    create.oldOid = std::string(kZeroOid);
    create.newOid = lastOid;
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(create);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(!ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(bodyFailed);
    CHECK(error.find("bytes") != std::string::npos);
    CHECK_EQ(port.moves(), 0u);
}

// ===========================================================================
// The door's own contract, answered through ols::git::handleReceivePack and
// receivePackAdvertisement on a scratch bare repository. The four hardenings
// the audit named on the door stand here: a body whose service preamble the
// split must read past, the band ask read from the capabilities and nowhere
// else, a refusal whose words carry a NUL still written as report-status, and
// the capability set the advertisement states. Where the honest claim is about
// one helper, the helper is asked directly — the door travels in this
// translation unit, and so its guards are proven rather than assumed.
// ===========================================================================

namespace {

// One push body: the command line, the flush that closes the command section,
// and nothing behind it — a push that only removes a ref brings no pack.
// withPreamble puts the v0 client's service marker in front of the commands.
std::vector<std::uint8_t> v0PushBody(const std::vector<std::uint8_t> &commandLine,
                                     bool withPreamble, std::string &why)
{
    std::vector<std::uint8_t> body;
    if (withPreamble) {
        if (!ols::gitcore::pktWrite("# service=git-receive-pack\n", body, why)) {
            body.clear();
            return body;
        }
        ols::gitcore::pktFlush(body);
    }
    appended(body, commandLine);
    ols::gitcore::pktFlush(body);
    return body;
}

} // anonymous namespace

// C1: a body that opens with the service marker. The commands stand behind it,
// and the answer names the ref the command moved. A split that took the marker
// as the whole command section reads no commands at all: the report it answers
// with names nothing, and the ref stays where it was.
TEST(a_push_body_bearing_the_service_preamble_is_read_past_it)
{
    ScratchRepo scratch("door-preamble");
    CHECK(scratch.stood());
    ols::gitcore::ObjectId held;
    CHECK(ols::gitcore::ObjectId::parse(std::string(kStubCommit), held));
    ols::gitcore::Repository repo(scratch.dir());
    CHECK(repo.writeLooseRef("refs/heads/gone", held));

    std::string why;
    // v0 puts its capabilities after the NUL of the first command line, and git
    // always sends them: that NUL is what the dialect is read from.
    const std::vector<std::uint8_t> command = frameLine(
        std::string(kStubCommit), std::string(kZeroOid), "refs/heads/gone",
        "report-status delete-refs ofs-delta", why);
    CHECK(!command.empty());

    const std::vector<std::string> guarded;
    for (int pass = 0; pass < 2; ++pass) {
        // pass 0 sends the marker, pass 1 the same push without it: the shape
        // the client speaks is the client's, and the answer is the same.
        if (pass == 1) {
            CHECK(repo.writeLooseRef("refs/heads/gone", held)); // the removal ran in pass 0
        }
        const std::vector<std::uint8_t> request = v0PushBody(command, pass == 0, why);
        CHECK(!request.empty());
        std::vector<std::uint8_t> answer;
        std::string words;
        const bool stood =
            ols::git::handleReceivePack(scratch.dir(), request, guarded, answer, words);
        CHECK(stood);
        CHECK(words.empty());
        const std::string text = textOfBytes(answer);
        CHECK(text.find("unpack ok") != std::string::npos);
        // The row is the proof the commands were read: a section that held only
        // the marker parses to no commands, and no command names this ref.
        CHECK(text.find("ok refs/heads/gone") != std::string::npos);
        ols::git::RepoPort port(scratch.dir());
        std::string stillThere;
        CHECK(!port.refValue("refs/heads/gone", stillThere)); // and it really moved
    }
}

// The split itself, asked directly: where the command section begins. With the
// marker in front it begins after the marker's flush; without one, at byte 0;
// the commands either way are the same bytes.
TEST(the_command_section_begins_past_a_service_preamble_and_at_zero_without_one)
{
    std::string why;
    // v0 puts its capabilities after the NUL of the first command line, and git
    // always sends them: that NUL is what the dialect is read from.
    const std::vector<std::uint8_t> command = frameLine(
        std::string(kStubCommit), std::string(kZeroOid), "refs/heads/gone",
        "report-status delete-refs ofs-delta", why);
    CHECK(!command.empty());

    std::size_t start = 1u;
    std::size_t end = 0u;
    const std::vector<std::uint8_t> plain = v0PushBody(command, false, why);
    CHECK(ols::git::commandSection(plain, start, end, why));
    CHECK_EQ(start, 0u);
    CHECK_EQ(end, plain.size());

    const std::vector<std::uint8_t> marked = v0PushBody(command, true, why);
    CHECK(ols::git::commandSection(marked, start, end, why));
    CHECK(start > 0u);
    CHECK_EQ(end, marked.size());
    CHECK_EQ(marked.size() - start, plain.size());
    CHECK(std::equal(plain.begin(), plain.end(), marked.begin() + static_cast<long>(start)));
}

// M4: the band ask is read from the capabilities and from nowhere else. A ref
// whose name reads "side-band-64k" is not a client asking for the band: the
// report must come back plain, or the client waits for a report-status it is
// not reading — the whole answer would ride a channel no one opened.
TEST(a_ref_named_side_band_64k_does_not_band_the_answer)
{
    ScratchRepo scratch("door-band");
    CHECK(scratch.stood());
    const std::vector<std::string> guarded;
    std::string why;

    // Caps without the band and a name that reads like one. The removal is
    // refused — the ref is not there — which is the shape that answers with an
    // ng line, and it is answered plain.
    const std::vector<std::uint8_t> plainLine =
        frameLine(std::string(kStubCommit), std::string(kZeroOid), "refs/heads/side-band-64k",
                  "report-status delete-refs", why);
    CHECK(!plainLine.empty());
    const std::vector<std::uint8_t> plainRequest = v0PushBody(plainLine, false, why);
    CHECK(!ols::git::asksSideband(plainRequest.data(), plainRequest.size()));
    std::vector<std::uint8_t> plainAnswer;
    std::string words;
    ols::git::handleReceivePack(scratch.dir(), plainRequest, guarded, plainAnswer, words);
    CHECK(!plainAnswer.empty());
    // The framing, byte for byte: a plain report's first payload byte is the
    // "u" of its unpack line, where a banded answer carries channel 1.
    CHECK(plainAnswer.size() > 4u);
    if (plainAnswer.size() > 4u) {
        CHECK_EQ(static_cast<int>(plainAnswer[4]), static_cast<int>('u'));
    }
    const std::string plainText = textOfBytes(plainAnswer);
    CHECK(plainText.find("unpack ok") != std::string::npos);
    CHECK(plainText.find("the ref is not there") != std::string::npos);

    // The same line with the band named in its caps does get the band.
    const std::vector<std::uint8_t> bandedLine =
        frameLine(std::string(kStubCommit), std::string(kZeroOid), "refs/heads/side-band-64k",
                  "report-status side-band-64k", why);
    CHECK(!bandedLine.empty());
    const std::vector<std::uint8_t> bandedRequest = v0PushBody(bandedLine, false, why);
    CHECK(ols::git::asksSideband(bandedRequest.data(), bandedRequest.size()));
    std::vector<std::uint8_t> bandedAnswer;
    ols::git::handleReceivePack(scratch.dir(), bandedRequest, guarded, bandedAnswer, words);
    CHECK(!bandedAnswer.empty());
    if (bandedAnswer.size() > 4u) {
        CHECK_EQ(static_cast<int>(bandedAnswer[4]), 1); // channel 1
    }
    CHECK(textOfBytes(bandedAnswer).find("unpack ok") != std::string::npos);
}

// M5: a refusal whose words carry a NUL still answers report-status. A field
// of a report-status line carries no NUL, and words that kept one left the
// whole answer unwritable: the client got no report where a refusal in words
// was the honest answer.
TEST(a_refusal_whose_words_carry_a_nul_still_answers_report_status)
{
    CHECK_EQ(ols::git::oneLine(std::string("a\0b\rc\nd", 7)), std::string("a b c d"));

    ScratchRepo scratch("door-nul");
    CHECK(scratch.stood());
    // The v2 shape carries an argument line whose ref name holds a NUL; the
    // refusal echoes the name it turned down, and the answer echoes it as one
    // line too.
    std::string why;
    std::vector<std::uint8_t> request;
    CHECK(ols::gitcore::pktWrite("command=receive-pack\n", request, why));
    ols::gitcore::pktDelim(request);
    std::string argument = std::string(kStubCommit) + " " + std::string(kZeroOid) + " refs/heads/a";
    argument.push_back('\0');
    argument.push_back('b');
    argument.push_back('\n');
    CHECK(ols::gitcore::pktWrite(argument, request, why));
    ols::gitcore::pktFlush(request);

    const std::vector<std::string> guarded;
    std::vector<std::uint8_t> answer;
    std::string words;
    const bool stood = ols::git::handleReceivePack(scratch.dir(), request, guarded, answer, words);
    CHECK(!stood);
    CHECK(!words.empty());
    CHECK(!answer.empty()); // the empty answer is what cost the client its report
    const std::string text = textOfBytes(answer);
    CHECK(text.find("unpack ng") != std::string::npos);
    CHECK(text.find('\0') == std::string::npos);
    CHECK(text.find("refs/heads/a b") != std::string::npos);
    CHECK(words.find('\0') == std::string::npos);
}

// M6: the advertisement states the caps this door answers with. "atomic" is
// not among them: a refusal here says no ref moved, which is not the promise
// the word carries.
TEST(the_advertised_caps_do_not_name_atomic)
{
    ScratchRepo scratch("door-caps");
    CHECK(scratch.stood());
    std::vector<std::uint8_t> bytes;
    std::string words;
    CHECK(ols::git::receivePackAdvertisement(scratch.dir(), bytes, words));
    CHECK(words.empty());
    const std::string text = textOfBytes(bytes);
    CHECK(text.find("# service=git-receive-pack") != std::string::npos);
    CHECK(text.find("atomic") == std::string::npos);
    CHECK(text.find("report-status") != std::string::npos);
    CHECK(text.find("delete-refs") != std::string::npos);
    CHECK(text.find("side-band-64k") != std::string::npos);
    CHECK(text.find("ofs-delta") != std::string::npos);
    CHECK(text.find("agent=git/ols-dev") != std::string::npos);
}

// ===========================================================================
// parseCommands, asked directly: the parser of the "<old> <new> <name>"
// command lines, which both roads reach — handleReceivePack's v0 road through
// v0ArgumentLines and its v2 road through v2ParseRequest's argumentLines. The
// payloads the parser takes are plain strings, so the cases stand them here
// and ask the parser itself: what a refused request costs the answer is the
// door's framing, and that is pinned where the queued batch lands it.
// ===========================================================================

TEST(parseCommands_lands_a_well_formed_command_exactly_as_it_came)
{
    std::vector<ReceiveCommand> cmds;
    std::string error;
    const std::string line = std::string(kZeroOid) + " " + std::string(kStubCommit) + " refs/heads/new";
    std::vector<std::string> payloads;
    payloads.push_back(line);
    CHECK(ols::gitcore::parseCommands(payloads, cmds, error));
    CHECK_EQ(error, std::string());
    CHECK_EQ(cmds.size(), 1u);
    if (cmds.size() == 1u) {
        CHECK_EQ(cmds[0].name, std::string("refs/heads/new"));
        CHECK_EQ(cmds[0].oldOid, std::string(kZeroOid));
        CHECK_EQ(cmds[0].newOid, std::string(kStubCommit));
    }

    // The payload's trailing newline belongs to the frame, not to the
    // argument: the same line with one lands the same command.
    cmds.clear();
    payloads.clear();
    payloads.push_back(line + "\n");
    CHECK(ols::gitcore::parseCommands(payloads, cmds, error));
    CHECK_EQ(cmds.size(), 1u);
    if (cmds.size() == 1u) {
        CHECK_EQ(cmds[0].name, std::string("refs/heads/new"));
        CHECK_EQ(cmds[0].newOid, std::string(kStubCommit));
    }
}

TEST(all_zeros_on_either_side_of_a_command_is_a_standing_create_or_delete)
{
    // All zeros is hex of a width this layer reads, so both halves of the
    // wire's create/delete convention parse here; what they mean is the
    // receiver's, and the parser only lands them.
    std::vector<std::string> payloads;
    payloads.push_back(std::string(kZeroOid) + " " + std::string(kStubCommit) + " refs/tags/created");
    payloads.push_back(std::string(kStubRoot) + " " + std::string(kZeroOid) + " refs/heads/deleted");
    std::vector<ReceiveCommand> cmds;
    std::string error;
    CHECK(ols::gitcore::parseCommands(payloads, cmds, error));
    CHECK_EQ(error, std::string());
    CHECK_EQ(cmds.size(), 2u);
    if (cmds.size() == 2u) {
        CHECK_EQ(cmds[0].oldOid, std::string(kZeroOid)); // the create
        CHECK_EQ(cmds[0].name, std::string("refs/tags/created"));
        CHECK_EQ(cmds[1].newOid, std::string(kZeroOid)); // the delete
        CHECK_EQ(cmds[1].name, std::string("refs/heads/deleted"));
    }
}

TEST(uppercase_hex_ids_are_accepted_and_the_answer_carries_them_lowered)
{
    // parseOneArgument puts both fields through detail::lower_hex before it
    // asks whether they are hex of a width: the case the client typed is not
    // the case the commands carry on.
    const std::string upper = "A867EF798B215DB19EAF0C7B615C789792FE62B9";
    std::vector<std::string> payloads;
    payloads.push_back(upper + " " + std::string(kZeroOid) + " refs/heads/moved");
    std::vector<ReceiveCommand> cmds;
    std::string error;
    CHECK(ols::gitcore::parseCommands(payloads, cmds, error));
    CHECK_EQ(error, std::string());
    CHECK_EQ(cmds.size(), 1u);
    if (cmds.size() == 1u) {
        CHECK_EQ(cmds[0].oldOid, std::string(kStubCommit)); // the same id, lowered
        CHECK_EQ(cmds[0].newOid, std::string(kZeroOid));
    }
}

TEST(a_non_hex_or_wrong_width_id_is_refused_and_the_line_is_echoed_whole)
{
    // The refusal names the id's shape, and echoes the whole argument — not
    // just the field that failed — so the client can see which line it sent.
    const std::string words = "a command's id is not hex of a width this layer reads: ";
    std::vector<std::string> widths;
    widths.push_back(std::string(40u, 'x')); // the right width, no hex in it
    widths.push_back(std::string(39u, '1')); // one short of sha-1
    widths.push_back(std::string(41u, '1')); // one over
    for (std::size_t i = 0u; i < widths.size(); ++i) {
        const std::string line = widths[i] + " " + std::string(kZeroOid) + " refs/heads/x";
        std::vector<std::string> payloads;
        payloads.push_back(line);
        std::vector<ReceiveCommand> cmds;
        std::string error;
        CHECK(!ols::gitcore::parseCommands(payloads, cmds, error));
        CHECK_EQ(error, words + line);
        CHECK_EQ(cmds.size(), 0u); // nothing is handed over on a refusal
    }
}

TEST(one_ref_named_twice_in_one_request_is_refused_and_names_itself)
{
    // Two commands for one ref is not one push: the parser keeps the names it
    // has seen and refuses the repeat by name, before any policy is asked.
    std::vector<std::string> payloads;
    payloads.push_back(std::string(kZeroOid) + " " + std::string(kStubCommit) + " refs/heads/twice");
    payloads.push_back(std::string(kStubCommit) + " " + std::string(kStubRoot) + " refs/heads/twice");
    std::vector<ReceiveCommand> cmds;
    std::string error;
    CHECK(!ols::gitcore::parseCommands(payloads, cmds, error));
    CHECK_EQ(error, std::string("the same ref is named twice: refs/heads/twice"));
    CHECK_EQ(cmds.size(), 0u); // the command that stood before it is not handed over
}

TEST(ref_name_edges_answer_with_the_words_the_parser_gives)
{
    const std::string nameWords = "a command's ref name is not a name git would take: ";

    // A name with a space, one wearing the lock suffix, and no name at all:
    // each is the name's own refusal, and the words echo the name that failed.
    std::vector<std::string> names;
    names.push_back("refs/heads/a b");    // the name field carries a space
    names.push_back("refs/heads/x.lock"); // a lock suffix is the ref's own
    names.push_back(std::string());       // nothing after the second space
    for (std::size_t i = 0u; i < names.size(); ++i) {
        const std::string line = std::string(kZeroOid) + " " + std::string(kStubCommit) + " " + names[i];
        std::vector<std::string> payloads;
        payloads.push_back(line);
        std::vector<ReceiveCommand> cmds;
        std::string error;
        CHECK(!ols::gitcore::parseCommands(payloads, cmds, error));
        CHECK_EQ(error, nameWords + names[i]);
        CHECK_EQ(cmds.size(), 0u);
    }

    // A name git takes stands, and lands whole.
    {
        std::vector<std::string> payloads;
        payloads.push_back(std::string(kZeroOid) + " " + std::string(kStubCommit) + " refs/heads/ok");
        std::vector<ReceiveCommand> cmds;
        std::string error;
        CHECK(ols::gitcore::parseCommands(payloads, cmds, error));
        CHECK_EQ(error, std::string());
        CHECK_EQ(cmds.size(), 1u);
        if (cmds.size() == 1u) {
            CHECK_EQ(cmds[0].name, std::string("refs/heads/ok"));
        }
    }

    // An argument that states only two fields is refused by its shape, before
    // any field is read.
    {
        const std::string line = std::string(kZeroOid) + " " + std::string(kStubCommit);
        std::vector<std::string> payloads;
        payloads.push_back(line);
        std::vector<ReceiveCommand> cmds;
        std::string error;
        CHECK(!ols::gitcore::parseCommands(payloads, cmds, error));
        CHECK_EQ(error, std::string("a command argument is '<old-oid> <new-oid> <name>'"));
        CHECK_EQ(cmds.size(), 0u);
    }
}

TEST(trailing_garbage_after_the_name_lands_inside_the_name)
{
    // The parser takes three fields and no more: everything past the second
    // space is the name, so an extra token is a name with a space in it —
    // refused by the name's words, not by a field-count complaint.
    const std::string line = std::string(kZeroOid) + " " + std::string(kStubCommit) + " refs/heads/x extra";
    std::vector<std::string> payloads;
    payloads.push_back(line);
    std::vector<ReceiveCommand> cmds;
    std::string error;
    CHECK(!ols::gitcore::parseCommands(payloads, cmds, error));
    CHECK_EQ(error, std::string("a command's ref name is not a name git would take: ") + "refs/heads/x extra");
    CHECK_EQ(cmds.size(), 0u);
}

TEST(the_v0_road_hands_parseCommands_exactly_the_argument_lines_it_reads)
{
    // The road a v0 client speaks, up to the parser and no further: the
    // command section, its argument lines, and the parser's answer on them.
    // The caps after the first line's NUL are walked over, never parsed.
    std::string why;
    const std::vector<std::uint8_t> first = frameLine(
        std::string(kZeroOid), std::string(kStubCommit), "refs/tags/v0-created",
        "report-status side-band-64k", why);
    const std::vector<std::uint8_t> second = frameLine(
        std::string(kStubCommit), std::string(kZeroOid), "refs/heads/v0-deleted", "", why);
    CHECK(!first.empty());
    CHECK(!second.empty());

    std::vector<std::uint8_t> request;
    appended(request, first);
    appended(request, second);
    ols::gitcore::pktFlush(request);

    std::size_t start = 0u;
    std::size_t end = 0u;
    CHECK(ols::git::commandSection(request, start, end, why));
    CHECK_EQ(start, 0u);
    CHECK_EQ(end, request.size());

    std::vector<std::string> lines;
    CHECK(ols::git::v0ArgumentLines(request.data() + start, end - start, lines));
    CHECK_EQ(lines.size(), 2u);
    if (lines.size() == 2u) {
        // The first line's caps are cut at its NUL; the second keeps the
        // newline that belongs to its frame — the parser drops it, not the
        // splitter.
        CHECK_EQ(lines[0], std::string(kZeroOid) + " " + std::string(kStubCommit) + " refs/tags/v0-created");
        CHECK_EQ(lines[1], std::string(kStubCommit) + " " + std::string(kZeroOid) + " refs/heads/v0-deleted\n");
    }

    std::vector<ReceiveCommand> cmds;
    std::string error;
    CHECK(ols::gitcore::parseCommands(lines, cmds, error));
    CHECK_EQ(error, std::string());
    CHECK_EQ(cmds.size(), 2u);
    if (cmds.size() == 2u) {
        CHECK_EQ(cmds[0].name, std::string("refs/tags/v0-created"));
        CHECK_EQ(cmds[0].oldOid, std::string(kZeroOid));
        CHECK_EQ(cmds[1].name, std::string("refs/heads/v0-deleted"));
        CHECK_EQ(cmds[1].newOid, std::string(kZeroOid));
    }
}

// =============================================================================
// The path ban and the protected names, end to end: treeBannedPath and pathBanned
// (the segment that is ".." and the path that starts with a slash) and protectedName
// are judged here through receive itself — the pack the push brings, the tree it
// carries, and the ref that may not move. The words of the ban are quoted here
// exactly as receive.cpp stands them.
// =============================================================================

namespace {

// One full record of an object: the header states the kind, the body is the
// bytes deflated — the shape a_commit_sorting_before_its_own_tree already
// stands, named here so the ban cases can state their own trees.
BuiltRecord banPlainRecord(RecordKind kind, const std::string &content)
{
    BuiltRecord record;
    record.head = recordHeadOf(static_cast<int>(kind), content.size());
    if (!ols::gitcore::deflateStored(std::vector<std::uint8_t>(content.begin(), content.end()),
                                     record.body))
    {
        record.body.clear();
    }
    return record;
}

// One tree entry as the object carries it: mode, space, name, NUL, and the
// raw id — the bytes treeParse reads back.
std::string banTreeEntry(const std::string &mode, const std::string &name,
                         const std::string &oidHex)
{
    std::string entry = mode + " " + name;
    entry.push_back('\0');
    const std::vector<std::uint8_t> raw = rawOfHex(oidHex);
    entry.append(raw.begin(), raw.end());
    return entry;
}

// A commit's content: the tree it holds, the parents it carries, and the
// pair of persons git insists on.
std::string banCommitBody(const std::string &treeOid, const std::vector<std::string> &parents,
                          const std::string &message)
{
    std::string content = "tree " + treeOid + "\n";
    for (const std::string &parent : parents)
    {
        content += "parent " + parent + "\n";
    }
    content += "author T <t@example> 0 +0000\ncommitter T <t@example> 0 +0000\n\n" + message + "\n";
    return content;
}

// The words receive.cpp carries for a path no repository may hold, quoted
// exactly as the unit stands them.
std::string banWords(const std::string &path)
{
    return "the trees it brings name a path no repository may carry: '" + path + "'";
}

} // anonymous namespace

// The ban fires before a ref moves: the push's commit carries a tree whose
// entry is named ".." — a segment that is exactly ".." is what pathBanned
// refuses — and the refusal is the ban's words, not the fast-forward's: the
// move is a true fast-forward, the base chain held in the repository. Nothing
// moved, and the ref still names its old oid; the objects landed before the
// ref was asked about, the way a push keeps what it received.
TEST(a_tree_naming_a_dot_dot_segment_is_refused_before_the_ref_moves)
{
    const std::string oldBlob = "the file as the repository holds it";
    const std::string oldBlobOid = objectOidOf(ObjectKind::Blob, oldBlob);
    const std::string oldTree = banTreeEntry("100644", "a", oldBlobOid);
    const std::string oldTreeOid = objectOidOf(ObjectKind::Tree, oldTree);
    const std::string oldCommit = banCommitBody(oldTreeOid, {}, "base");
    const std::string oldCommitOid = objectOidOf(ObjectKind::Commit, oldCommit);

    HoldingPort port;
    port.hold(oldBlobOid, "blob", oldBlob);
    port.hold(oldTreeOid, "tree", oldTree);
    port.hold(oldCommitOid, "commit", oldCommit);
    port.addRef("refs/heads/main", oldCommitOid);

    const std::string newBlob = "what the push brings";
    const std::string newBlobOid = objectOidOf(ObjectKind::Blob, newBlob);
    const std::string newTree = banTreeEntry("100644", "..", newBlobOid);
    const std::string newTreeOid = objectOidOf(ObjectKind::Tree, newTree);
    const std::string newCommit = banCommitBody(newTreeOid, {oldCommitOid}, "move");
    const std::string newCommitOid = objectOidOf(ObjectKind::Commit, newCommit);

    std::vector<BuiltRecord> records;
    records.push_back(banPlainRecord(RecordKind::Blob, newBlob));
    records.push_back(banPlainRecord(RecordKind::Tree, newTree));
    records.push_back(banPlainRecord(RecordKind::Commit, newCommit));
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    ReceiveCommand move;
    move.name = "refs/heads/main";
    move.oldOid = oldCommitOid;
    move.newOid = newCommitOid;
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(move);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(!bodyFailed); // the pack stood; the ban is one command's refusal
    CHECK_EQ(results.size(), 1u);
    CHECK_EQ(results[0].first, std::string("refs/heads/main"));
    CHECK_EQ(results[0].second, banWords("..")); // the words, quoted, naming the segment
    CHECK_EQ(port.moves(), 0u); // no ref moved
    std::string stillHeld;
    CHECK(port.refValue("refs/heads/main", stillHeld)); // and the ref still names its old oid
    CHECK_EQ(stillHeld, oldCommitOid);
    // the objects landed before the ref was asked about: a push keeps what it
    // received even when a ref does not move.
    CHECK_EQ(port.written().size(), 3u);
}

// The ban names the whole path, not the segment: a ".." entry stands inside a
// folder, and the words carry "d/..". The same tree carries an entry named
// "a..b" — a segment that merely contains two dots is none of the ban's
// business — so the path the words name is the deeper one. A create carries
// it: the ban judges the content the pack brings before any fast-forward
// question could, and a create has none.
TEST(a_banned_segment_deeper_in_the_tree_is_named_with_its_whole_path)
{
    const std::string dotDotBlob = "the blob the .. entry carries";
    const std::string dotDotOid = objectOidOf(ObjectKind::Blob, dotDotBlob);
    const std::string subTree = banTreeEntry("100644", "..", dotDotOid);
    const std::string subTreeOid = objectOidOf(ObjectKind::Tree, subTree);

    const std::string aDotBlob = "the blob the a..b entry carries";
    const std::string aDotOid = objectOidOf(ObjectKind::Blob, aDotBlob);
    const std::string rootTree = banTreeEntry("100644", "a..b", aDotOid) +
                                 banTreeEntry("40000", "d", subTreeOid);
    const std::string rootTreeOid = objectOidOf(ObjectKind::Tree, rootTree);

    const std::string commit = banCommitBody(rootTreeOid, {}, "deep");
    const std::string commitOid = objectOidOf(ObjectKind::Commit, commit);

    std::vector<BuiltRecord> records;
    records.push_back(banPlainRecord(RecordKind::Blob, dotDotBlob));
    records.push_back(banPlainRecord(RecordKind::Blob, aDotBlob));
    records.push_back(banPlainRecord(RecordKind::Tree, subTree));
    records.push_back(banPlainRecord(RecordKind::Tree, rootTree));
    records.push_back(banPlainRecord(RecordKind::Commit, commit));
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    HoldingPort port;

    ReceiveCommand create;
    create.name = "refs/heads/deep";
    create.oldOid = std::string(kZeroOid);
    create.newOid = commitOid;
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(create);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(!bodyFailed);
    CHECK_EQ(results.size(), 1u);
    CHECK_EQ(results[0].second, banWords("d/..")); // the whole path, with the folder in front
    CHECK_EQ(port.moves(), 0u);
}

// The clean tree passes the ban: the same push as the .. case, with the entry
// named "a" instead — a true fast-forward, no protected name, the policy
// asked of nothing else — and the command moves.
TEST(a_clean_tree_passes_the_path_ban_on_a_valid_fast_forward)
{
    const std::string oldBlob = "the file as the repository holds it";
    const std::string oldBlobOid = objectOidOf(ObjectKind::Blob, oldBlob);
    const std::string oldTree = banTreeEntry("100644", "a", oldBlobOid);
    const std::string oldTreeOid = objectOidOf(ObjectKind::Tree, oldTree);
    const std::string oldCommit = banCommitBody(oldTreeOid, {}, "base");
    const std::string oldCommitOid = objectOidOf(ObjectKind::Commit, oldCommit);

    HoldingPort port;
    port.hold(oldBlobOid, "blob", oldBlob);
    port.hold(oldTreeOid, "tree", oldTree);
    port.hold(oldCommitOid, "commit", oldCommit);
    port.addRef("refs/heads/main", oldCommitOid);

    const std::string newBlob = "what the push brings";
    const std::string newBlobOid = objectOidOf(ObjectKind::Blob, newBlob);
    const std::string newTree = banTreeEntry("100644", "a", newBlobOid);
    const std::string newTreeOid = objectOidOf(ObjectKind::Tree, newTree);
    const std::string newCommit = banCommitBody(newTreeOid, {oldCommitOid}, "move");
    const std::string newCommitOid = objectOidOf(ObjectKind::Commit, newCommit);

    std::vector<BuiltRecord> records;
    records.push_back(banPlainRecord(RecordKind::Blob, newBlob));
    records.push_back(banPlainRecord(RecordKind::Tree, newTree));
    records.push_back(banPlainRecord(RecordKind::Commit, newCommit));
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    ReceiveCommand move;
    move.name = "refs/heads/main";
    move.oldOid = oldCommitOid;
    move.newOid = newCommitOid;
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(move);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(!bodyFailed);
    CHECK_EQ(results.size(), 1u);
    CHECK_EQ(results[0].second, std::string()); // the ban never fired
    CHECK_EQ(port.moves(), 1u); // and the move ran
}

// The leading-slash half of pathBanned, pinned as far as the trees reach it:
// no tree this layer reads can name such a path — treeParse refuses entry
// names that carry a slash — so the ban words never speak one. In the pack,
// the malformed tree is refused by the pack's tree words, a body refusal;
// held in the repository behind a pack commit, the ban walk reads no claim
// from a tree it cannot parse, and an otherwise-valid create stands.
TEST(a_tree_entry_wearing_a_leading_slash_never_reaches_the_ban_words)
{
    const std::string blob = "the blob the /x entry carries";
    const std::string blobOid = objectOidOf(ObjectKind::Blob, blob);
    const std::string slashTree = banTreeEntry("100644", "/x", blobOid);
    const std::string slashTreeOid = objectOidOf(ObjectKind::Tree, slashTree);
    const std::string commit = banCommitBody(slashTreeOid, {}, "slash");
    const std::string commitOid = objectOidOf(ObjectKind::Commit, commit);

    std::vector<BuiltRecord> records;
    records.push_back(banPlainRecord(RecordKind::Blob, blob));
    records.push_back(banPlainRecord(RecordKind::Tree, slashTree));
    records.push_back(banPlainRecord(RecordKind::Commit, commit));
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    ReceiveCommand create;
    create.name = "refs/heads/slash";
    create.oldOid = std::string(kZeroOid);
    create.newOid = commitOid;
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(create);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;

    HoldingPort port;
    CHECK(!ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    // the refusal names itself in the error; bodyFailed stays false — that
    // flag is reserved for the size cap and a body that does not open.
    CHECK(!bodyFailed);
    CHECK(error.find("not a tree this layer reads") != std::string::npos);
    CHECK(error.find("names a thing in one folder") != std::string::npos);
    CHECK(error.find("the trees it brings") == std::string::npos); // never the ban's words
    CHECK_EQ(results.size(), 0u);
    CHECK_EQ(port.moves(), 0u);

    // Half two: the same malformed tree held in the repository, the pack
    // carrying only the commit rooted at it. The walk reads no claim from a
    // tree it cannot parse, so the ban says nothing and the create stands.
    HoldingPort behind;
    behind.hold(slashTreeOid, "tree", slashTree);
    std::vector<BuiltRecord> commitOnly;
    commitOnly.push_back(banPlainRecord(RecordKind::Commit, commit));
    const std::vector<std::uint8_t> packBehind = barePackOf(commitOnly);
    CHECK(!packBehind.empty());

    results.clear();
    bodyFailed = false;
    error.clear();
    CHECK(ols::gitcore::receive(behind, packBehind, cmds, policy, results, bodyFailed, error));
    CHECK(!bodyFailed);
    CHECK_EQ(results.size(), 1u);
    CHECK_EQ(results[0].second, std::string()); // the ban never fired
    CHECK_EQ(behind.moves(), 1u);
}

// The protectedName matrix, answered through receive: the guard is an exact
// match on the full ref name. The default branch's name answers the protected
// words on a move that rewrites history; a case difference does not — it
// answers the plain non-fast-forward words; a protected deletion is refused
// by its own words, while the case-different deletion moves; and a create of
// a protected name stands, which is what a first push to one asks.
TEST(the_protected_name_matrix_answers_exact_and_case_sensitive)
{
    StubPort port({std::string(kStubCommit), std::string(kStubRoot)},
                  {{"refs/heads/main", std::string(kStubRoot)},
                   {"refs/heads/Main", std::string(kStubRoot)}});

    ReceivePolicy policy;
    policy.protectedBranches.push_back("refs/heads/main");
    policy.protectedBranches.push_back("refs/heads/trunk");
    policy.allowNonFastForward = false;

    std::vector<ReceiveCommand> cmds;
    ReceiveCommand guardedMove;
    guardedMove.name = "refs/heads/main";
    guardedMove.oldOid = std::string(kStubRoot);
    guardedMove.newOid = std::string(kStubCommit);
    cmds.push_back(guardedMove);

    ReceiveCommand caseMove;
    caseMove.name = "refs/heads/Main";
    caseMove.oldOid = std::string(kStubRoot);
    caseMove.newOid = std::string(kStubCommit);
    cmds.push_back(caseMove);

    ReceiveCommand guardedDelete;
    guardedDelete.name = "refs/heads/main";
    guardedDelete.oldOid = std::string(kStubRoot);
    guardedDelete.newOid = std::string(kZeroOid);
    cmds.push_back(guardedDelete);

    ReceiveCommand caseDelete;
    caseDelete.name = "refs/heads/Main";
    caseDelete.oldOid = std::string(kStubRoot);
    caseDelete.newOid = std::string(kZeroOid);
    cmds.push_back(caseDelete);

    ReceiveCommand firstPush;
    firstPush.name = "refs/heads/trunk";
    firstPush.oldOid = std::string(kZeroOid);
    firstPush.newOid = std::string(kStubCommit);
    cmds.push_back(firstPush);

    const std::vector<std::uint8_t> pack = emptyPackBytes();
    CHECK(!pack.empty());

    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(!bodyFailed);
    CHECK_EQ(results.size(), 5u);
    CHECK_EQ(results[0].first, std::string("refs/heads/main"));
    CHECK_EQ(results[0].second, "a protected branch takes a fast-forward only");
    CHECK_EQ(results[1].first, std::string("refs/heads/Main"));
    CHECK_EQ(results[1].second, "not a fast-forward: the history would be rewritten");
    CHECK_EQ(results[2].first, std::string("refs/heads/main"));
    CHECK_EQ(results[2].second, "the branch is protected and its deletion is not asked for");
    CHECK_EQ(results[3].first, std::string("refs/heads/Main"));
    CHECK_EQ(results[3].second, std::string()); // the case-different name is not guarded
    CHECK_EQ(results[4].first, std::string("refs/heads/trunk"));
    CHECK_EQ(results[4].second, std::string()); // a create of a protected name stands
    CHECK_EQ(port.moves(), 2u); // only the deletion and the first push reached the port
}

// The ban is asked before the fast-forward: the push rewrites history AND
// brings a banned tree, and the answer is the ban's words — the ff refusal
// never speaks. The repository holds two unrelated commits: the ref's old
// one, and the parent the pack's commit really carries.
TEST(a_banned_tree_outranks_the_fast_forward_words_on_a_rewritten_history)
{
    const std::string oldBlob = "the file as the repository holds it";
    const std::string oldBlobOid = objectOidOf(ObjectKind::Blob, oldBlob);
    const std::string oldTree = banTreeEntry("100644", "a", oldBlobOid);
    const std::string oldTreeOid = objectOidOf(ObjectKind::Tree, oldTree);
    const std::string oldCommit = banCommitBody(oldTreeOid, {}, "base");
    const std::string oldCommitOid = objectOidOf(ObjectKind::Commit, oldCommit);

    const std::string otherBlob = "the file the other history holds";
    const std::string otherBlobOid = objectOidOf(ObjectKind::Blob, otherBlob);
    const std::string otherTree = banTreeEntry("100644", "b", otherBlobOid);
    const std::string otherTreeOid = objectOidOf(ObjectKind::Tree, otherTree);
    const std::string otherCommit = banCommitBody(otherTreeOid, {}, "other");
    const std::string otherCommitOid = objectOidOf(ObjectKind::Commit, otherCommit);

    HoldingPort port;
    port.hold(oldBlobOid, "blob", oldBlob);
    port.hold(oldTreeOid, "tree", oldTree);
    port.hold(oldCommitOid, "commit", oldCommit);
    port.hold(otherBlobOid, "blob", otherBlob);
    port.hold(otherTreeOid, "tree", otherTree);
    port.hold(otherCommitOid, "commit", otherCommit);
    port.addRef("refs/heads/main", oldCommitOid);

    const std::string bannedBlob = "what the push brings";
    const std::string bannedBlobOid = objectOidOf(ObjectKind::Blob, bannedBlob);
    const std::string bannedTree = banTreeEntry("100644", "..", bannedBlobOid);
    const std::string bannedTreeOid = objectOidOf(ObjectKind::Tree, bannedTree);
    const std::string bannedCommit = banCommitBody(bannedTreeOid, {otherCommitOid}, "rewritten");
    const std::string bannedCommitOid = objectOidOf(ObjectKind::Commit, bannedCommit);

    std::vector<BuiltRecord> records;
    records.push_back(banPlainRecord(RecordKind::Blob, bannedBlob));
    records.push_back(banPlainRecord(RecordKind::Tree, bannedTree));
    records.push_back(banPlainRecord(RecordKind::Commit, bannedCommit));
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    ReceiveCommand move;
    move.name = "refs/heads/main";
    move.oldOid = oldCommitOid;
    move.newOid = bannedCommitOid;
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(move);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(!bodyFailed);
    CHECK_EQ(results.size(), 1u);
    CHECK_EQ(results[0].second, banWords("..")); // the ban's words, not the ff's
    CHECK(results[0].second.find("fast-forward") == std::string::npos);
    CHECK_EQ(port.moves(), 0u);
}

// ===========================================================================
// The answer-framing functions of the door, asked directly. The door reaches
// these only through handleReceivePack, so their own bytes were never pinned;
// here each is called on its own, the way the door-level cases above call
// commandSection and asksSideband directly. The bytes are stated with the
// lengths counted in the comments, the way the wire cases keep their counts
// beside the bytes. Nothing here asserts handleReceivePack's final answer for
// a zero-command or body-failed body — that shape moves in a queued fix and
// is out of this batch.
// ===========================================================================

// answerNg: the two report-status shapes it can make. A policy refusal is
// "unpack ok" with one ng line per requested ref; a body that did not stand
// leads with "unpack ng <reason>" and keeps those ng lines after it. Every
// row carries the same reason, so the per-ref lines are all ng.
TEST(answerNg_frames_the_ok_and_ng_lists_byte_for_byte)
{
    std::vector<ReceiveCommand> cmds;
    ReceiveCommand one;
    one.name = "refs/heads/a";
    one.oldOid = std::string(kZeroOid);
    one.newOid = std::string(kStubCommit);
    cmds.push_back(one);

    std::vector<std::uint8_t> out;

    // bodyFailed = false: the unpack-ok answer, then the per-ref ng line.
    CHECK(ols::git::answerNg(out, cmds, "no", false));
    std::string expected;
    // "unpack ok\n": 10; +4 = 14 = 0x0e
    expected += "000eunpack ok\n";
    // "ng refs/heads/a no\n": 3+12+1+2+1 = 19; +4 = 23 = 0x17
    expected += "0017ng refs/heads/a no\n";
    expected += "0000";
    CHECK_EQ(textOfBytes(out), expected);

    // bodyFailed = true: the same per-ref lines, led by the unpack-ng line.
    out.clear();
    CHECK(ols::git::answerNg(out, cmds, "no", true));
    std::string ngExpected;
    // "unpack ng no\n": 6+1+2+1+2+1 = 13; +4 = 17 = 0x11
    ngExpected += "0011unpack ng no\n";
    ngExpected += "0017ng refs/heads/a no\n";
    ngExpected += "0000";
    CHECK_EQ(textOfBytes(out), ngExpected);
}

// An empty ng list with bodyFailed = false answers just the unpack-ok line and
// the flush: no command, so no per-ref line.
TEST(answerNg_with_no_commands_answers_just_unpack_ok)
{
    const std::vector<ReceiveCommand> none;
    std::vector<std::uint8_t> out;
    CHECK(ols::git::answerNg(out, none, "ignored", false));
    std::string expected;
    // "unpack ok\n": 10; +4 = 14 = 0x0e
    expected += "000eunpack ok\n";
    expected += "0000";
    CHECK_EQ(textOfBytes(out), expected);
}

// A reason or a name that carries a break or a NUL is walked to one line by
// oneLine before it reaches a report-status field, so the answer stands with
// the folded words and no raw break or NUL inside a field.
TEST(answerNg_folds_a_reason_and_name_that_carry_breaks_into_one_line)
{
    // oneLine walks \n, \r, and NUL to spaces: the folded reason is one line.
    CHECK_EQ(ols::git::oneLine(std::string("bad\nreason\0here", 15)),
             std::string("bad reason here"));

    std::vector<ReceiveCommand> cmds;
    ReceiveCommand one;
    one.name = "refs/heads/y";
    one.oldOid = std::string(kZeroOid);
    one.newOid = std::string(kStubCommit);
    cmds.push_back(one);

    std::vector<std::uint8_t> out;
    CHECK(ols::git::answerNg(out, cmds, std::string("bad\nreason\0here", 15), true));
    std::string expected;
    // "unpack ng bad reason here\n": 10 + 15 + 1 = 26; +4 = 30 = 0x1e
    expected += "001eunpack ng bad reason here\n";
    // "ng refs/heads/y bad reason here\n": 3+12+1+15+1 = 32; +4 = 36 = 0x24
    expected += "0024ng refs/heads/y bad reason here\n";
    expected += "0000";
    CHECK_EQ(textOfBytes(out), expected);
    // the fold really happened: no NUL stands anywhere, and the raw break
    // never reaches the answer.
    const std::string text = textOfBytes(out);
    CHECK(text.find('\0') == std::string::npos);
    CHECK(text.find("bad\nreason") == std::string::npos);
    CHECK(text.find("bad reason here") != std::string::npos);
}

// frameSideband: the whole answer rides channel 1 in frames under the band's
// ceiling, and the multiplexing ends with one empty channel-2 frame and the
// flush. A short payload is one channel-1 frame.
TEST(frameSideband_bands_a_short_payload_then_closes_the_band)
{
    const std::vector<std::uint8_t> answer = bytesOfText("hello");
    std::vector<std::uint8_t> out;
    std::string error;
    CHECK(ols::git::frameSideband(answer, out, error));
    std::string expected;
    // one channel-1 frame: 0x01 + "hello" = 6 bytes; +4 = 10 = 0x0a
    expected += "000a";
    expected.push_back(static_cast<char>(1));
    expected += "hello";
    // the closing empty channel-2 frame: 0x02 alone; +4 = 5 = 0x05
    expected += "0005";
    expected.push_back(static_cast<char>(2));
    // the flush that ends the multiplexing
    expected += "0000";
    CHECK_EQ(textOfBytes(out), expected);
}

// One byte past the band's ceiling (kMaxSidebandData = 65515 data bytes, so a
// 65516-byte frame is the largest that fits one pkt-line): the first frame
// carries the whole ceiling, the second carries the one byte that did not fit.
TEST(frameSideband_splits_a_payload_at_the_band_bound)
{
    const std::size_t over = ols::gitcore::kMaxSidebandData + 1u; // 65516
    std::string payload(over, 'a');
    payload.back() = 'z'; // the byte that lands in the second frame
    const std::vector<std::uint8_t> answer(payload.begin(), payload.end());
    std::vector<std::uint8_t> out;
    std::string error;
    CHECK(ols::git::frameSideband(answer, out, error));

    // first frame: 0x01 + 65515 bytes = 65516; +4 = 65520 = 0xfff0
    // second frame: 0x01 + 1 byte = 2; +4 = 6 = 0x06
    // channel-2 frame: 0x02 alone; +4 = 5 = 0x05
    // flush: 4
    const std::size_t total = 65520u + 6u + 5u + 4u;
    CHECK_EQ(out.size(), total);
    if (out.size() == total) {
        // the first frame's length field, then its channel byte
        CHECK_EQ(out[0], static_cast<std::uint8_t>('f'));
        CHECK_EQ(out[1], static_cast<std::uint8_t>('f'));
        CHECK_EQ(out[2], static_cast<std::uint8_t>('f'));
        CHECK_EQ(out[3], static_cast<std::uint8_t>('0'));
        CHECK_EQ(static_cast<int>(out[4]), 1); // channel 1
        CHECK_EQ(out[5], static_cast<std::uint8_t>('a'));            // first payload byte
        CHECK_EQ(out[65519u], static_cast<std::uint8_t>('a'));       // last byte of the first frame
        // the second frame begins one past the first: 4 + 65516 = 65520
        CHECK_EQ(out[65520u], static_cast<std::uint8_t>('0'));
        CHECK_EQ(out[65521u], static_cast<std::uint8_t>('0'));
        CHECK_EQ(out[65522u], static_cast<std::uint8_t>('0'));
        CHECK_EQ(out[65523u], static_cast<std::uint8_t>('6'));
        CHECK_EQ(static_cast<int>(out[65524u]), 1); // channel 1 again
        CHECK_EQ(out[65525u], static_cast<std::uint8_t>('z')); // the byte that did not fit
        // the closing empty channel-2 frame and the flush
        CHECK_EQ(out[65526u], static_cast<std::uint8_t>('0'));
        CHECK_EQ(out[65527u], static_cast<std::uint8_t>('0'));
        CHECK_EQ(out[65528u], static_cast<std::uint8_t>('0'));
        CHECK_EQ(out[65529u], static_cast<std::uint8_t>('5'));
        CHECK_EQ(static_cast<int>(out[65530u]), 2); // channel 2
        CHECK_EQ(out[65531u], static_cast<std::uint8_t>('0'));
        CHECK_EQ(out[65532u], static_cast<std::uint8_t>('0'));
        CHECK_EQ(out[65533u], static_cast<std::uint8_t>('0'));
        CHECK_EQ(out[65534u], static_cast<std::uint8_t>('0'));
    }
}

// The two fatal shapes, pinned against each other: answerFatal is the v2 fatal
// — one sideband channel-3 "ERR <oneLine(reason)>\n" line then the flush —
// while answerReceiveFatal is the plain v0 report-status fatal — an "unpack
// ng <oneLine(reason)>" line then the flush, no band.
TEST(answerFatal_carries_the_v2_band_three_and_answerReceiveFatal_the_plain_ng)
{
    std::vector<std::uint8_t> out;

    // answerFatal: "ERR boom\n" framed on channel 3: 9 + 1 = 10; +4 = 14 = 0x0e
    ols::git::answerFatal(out, "boom");
    std::string banded;
    banded += "000e";
    banded.push_back(static_cast<char>(3));
    banded += "ERR boom\n";
    banded += "0000";
    CHECK_EQ(textOfBytes(out), banded);

    // answerReceiveFatal: "unpack ng boom\n" plain: 15; +4 = 19 = 0x13
    out.clear();
    CHECK(ols::git::answerReceiveFatal(out, "boom"));
    std::string plain;
    plain += "0013unpack ng boom\n";
    plain += "0000";
    CHECK_EQ(textOfBytes(out), plain);
}

// The dialect read from the first payload, and the v0 argument lines with the
// caps walked over. isV2ReceiveRequest is true only for a "command=" opening;
// v0ArgumentLines cuts each payload at its NUL. The end-to-end answer of
// handleReceivePack for the zero-argument shape is out of scope.
TEST(isV2ReceiveRequest_reads_the_dialect_and_v0ArgumentLines_skips_the_caps)
{
    std::string why;

    // A v0 body: the first payload carries its capabilities after a NUL.
    const std::vector<std::uint8_t> first = frameLine(
        std::string(kZeroOid), std::string(kStubCommit), "refs/tags/v0-a",
        "report-status side-band-64k", why);
    const std::vector<std::uint8_t> second = frameLine(
        std::string(kStubCommit), std::string(kZeroOid), "refs/heads/v0-b", "", why);
    CHECK(!first.empty());
    CHECK(!second.empty());
    std::vector<std::uint8_t> v0;
    appended(v0, first);
    appended(v0, second);
    ols::gitcore::pktFlush(v0);

    // A v0 command line opens with a hex id, never "command=".
    CHECK(!ols::git::isV2ReceiveRequest(v0.data(), v0.size()));
    std::vector<std::string> lines;
    CHECK(ols::git::v0ArgumentLines(v0.data(), v0.size(), lines));
    CHECK_EQ(lines.size(), 2u);
    if (lines.size() == 2u) {
        // the caps after the first line's NUL are cut; the second keeps the
        // newline that belongs to its frame — the splitter does not drop it.
        CHECK_EQ(lines[0], std::string(kZeroOid) + " " + std::string(kStubCommit) + " refs/tags/v0-a");
        CHECK_EQ(lines[1], std::string(kStubCommit) + " " + std::string(kZeroOid) + " refs/heads/v0-b\n");
    }

    // A command= body: the dialect is v2.
    std::vector<std::uint8_t> v2;
    CHECK(ols::gitcore::pktWrite("command=receive-pack\n", v2, why));
    ols::gitcore::pktDelim(v2);
    CHECK(ols::git::isV2ReceiveRequest(v2.data(), v2.size()));

    // A body whose first payload is empty: the empty payload is not "command=",
    // so the dialect reads as v0, and v0ArgumentLines hands the empty payload
    // as one empty argument line. Only the boolean/parser choice is pinned here.
    std::vector<std::uint8_t> emptyFirst;
    emptyFirst.push_back(static_cast<std::uint8_t>('0')); // "0004": a zero-length payload
    emptyFirst.push_back(static_cast<std::uint8_t>('0'));
    emptyFirst.push_back(static_cast<std::uint8_t>('0'));
    emptyFirst.push_back(static_cast<std::uint8_t>('4'));
    ols::gitcore::pktFlush(emptyFirst);
    CHECK(!ols::git::isV2ReceiveRequest(emptyFirst.data(), emptyFirst.size()));
    std::vector<std::string> emptyLines;
    CHECK(ols::git::v0ArgumentLines(emptyFirst.data(), emptyFirst.size(), emptyLines));
    CHECK_EQ(emptyLines.size(), 1u);
    if (emptyLines.size() == 1u) {
        CHECK_EQ(emptyLines[0], std::string());
    }
}

// ===========================================================================
// The delta-chain depth guard, pinned at its bound on both sides. The cap is
// packfile.h's kMaxDeltaChain — src/gitcore/packfile.h:105, the number 1024 —
// and receive.cpp holds it on every road: the bare-pack rounds run to
// kMaxDeltaChain + 2 (src/gitcore/receive.cpp:594), a chain is refused the
// moment its link count passes the cap (src/gitcore/receive.cpp:681-684,
// baseDepth > kMaxDeltaChain), and rounds that ran out rather than closed
// name the same bound (src/gitcore/receive.cpp:728-732). The pair road
// refuses at the same boundary inside the packfile reader
// (src/gitcore/packfile.cpp:1090-1091, links.size() >= kMaxDeltaChain), and
// a REF cycle is what that cap breaks as well — receive reads the refusal
// again as a walk and names the circle in words
// (baseWalkCircles, src/gitcore/receive.cpp:265-310; the words at
// src/gitcore/receive.cpp:1266-1269).
// ===========================================================================

// A REF cycle — one object's base is the other, and back again — can only be
// stated where the records' ids are the index's to name: on the bare-pack
// road an id is the hash of the resolved bytes (src/gitcore/receive.cpp:702),
// and a cycle never resolves, so its records carry no ids at all. Both halves
// are pinned here through the same receive entry point: the pair bytes carry
// the index beside the pack — findTrailingIdx reads it off the handed bytes
// (src/gitcore/receive.cpp:116-158) and the bare indexer is skipped — and the
// bare bytes carry the mutual naming as far as its rounds can see. Neither
// half hangs: the pair road's walk is capped at the same number
// (src/gitcore/packfile.cpp:1090-1092), and the bare road's rounds are
// bounded (src/gitcore/receive.cpp:594).
TEST(a_cyclic_delta_chain_is_refused_on_both_roads_and_never_loops)
{
    const std::string oidA(40u, 'a');
    const std::string oidB(40u, 'b');
    const std::vector<std::uint8_t> tinyDelta = deltaOverBase("x", "y");

    // The pair road: the index names the two records, and the records name
    // each other by those names. open() stands the pair — every base a
    // record names is present (src/gitcore/packfile.cpp:992-1013) — and the
    // first resolve walks A→B→A and meets the cap.
    std::vector<BuiltRecord> records;
    records.push_back(refDeltaRecord(oidB, tinyDelta)); // the record the index names oidA
    records.push_back(refDeltaRecord(oidA, tinyDelta)); // the record the index names oidB
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    const std::size_t firstOffset = 12u;
    const std::size_t secondOffset =
        firstOffset + records[0].head.size() + records[0].body.size();
    const std::size_t fileEnd = pack.size() - 20u;

    std::vector<ols::gitcore::IndexEntry> entries(2u);
    CHECK(ols::gitcore::ObjectId::parse(oidA, entries[0].oid));
    CHECK(ols::gitcore::ObjectId::parse(oidB, entries[1].oid));
    entries[0].offset = firstOffset;
    entries[0].crc = ols::gitcore::crc32Of(pack.data() + firstOffset,
                                           secondOffset - firstOffset);
    entries[1].offset = secondOffset;
    entries[1].crc = ols::gitcore::crc32Of(pack.data() + secondOffset,
                                           fileEnd - secondOffset);

    const std::string packDigest =
        ols::gitcore::sha1Hex(std::string(pack.begin(),
                                          pack.begin() + static_cast<std::ptrdiff_t>(fileEnd)));
    const std::vector<std::uint8_t> digestRaw = rawOfHex(packDigest);
    std::array<std::uint8_t, 20> checksum{};
    for (std::size_t i = 0u; i < 20u; ++i) {
        checksum[i] = digestRaw[i];
    }
    std::vector<std::uint8_t> idx;
    CHECK(ols::gitcore::writeIdx(entries, checksum, idx));
    CHECK(!idx.empty());

    std::vector<std::uint8_t> pairBytes = pack;
    pairBytes.insert(pairBytes.end(), idx.begin(), idx.end());

    HoldingPort port;
    ReceiveCommand create;
    create.name = "refs/tags/cycle";
    create.oldOid = std::string(kZeroOid);
    create.newOid = oidA;
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(create);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(!ols::gitcore::receive(port, pairBytes, cmds, policy, results, bodyFailed, error));
    CHECK(bodyFailed);
    // The circle's words, naming the id the base walk returned to: the walk
    // starts at the lower id and comes back to it, so the words name it
    // twice.
    const std::string circleWords =
        "the pack object " + oidA +
        " does not resolve: its delta chain runs in a circle; the base walk returns to " + oidA;
    CHECK_EQ(error, circleWords);
    CHECK_EQ(results.size(), 0u);
    CHECK_EQ(port.moves(), 0u);
    CHECK_EQ(port.written().size(), 0u);

    // The bare road, same two records without an index: their ids are the
    // hash of resolved bytes and a cycle has none, so the rounds find no
    // progress and close — the refusal is the base-stands-nowhere words,
    // not the circle's, and it is a refusal, not a loop.
    const std::vector<std::uint8_t> bare = barePackOf(records);
    CHECK(!bare.empty());
    results.clear();
    bodyFailed = false;
    error.clear();
    CHECK(!ols::gitcore::receive(port, bare, cmds, policy, results, bodyFailed, error));
    CHECK(bodyFailed);
    CHECK_EQ(error, std::string("a delta in the pack names a base the pack does not carry"));
    CHECK_EQ(results.size(), 0u);
    CHECK_EQ(port.moves(), 0u);
    CHECK_EQ(port.written().size(), 0u);
}

// One link past the cap, base-first so the rounds are not what ends the
// walk: the resolve itself refuses the 1025th link the moment its depth
// passes the cap (src/gitcore/receive.cpp:681-684), and the words name the
// bound at the number.
TEST(a_delta_chain_one_link_past_the_cap_is_refused_naming_the_bound)
{
    const std::size_t linksPastCap = ols::gitcore::kMaxDeltaChain + 1u;
    std::string content = "chain base";
    std::string oid = objectOidOf(ObjectKind::Blob, content);

    std::vector<BuiltRecord> records;
    BuiltRecord base;
    base.head = recordHeadOf(static_cast<int>(RecordKind::Blob), content.size());
    if (!ols::gitcore::deflateStored(std::vector<std::uint8_t>(content.begin(), content.end()),
                                     base.body)) {
        base.body.clear();
    }
    records.push_back(base);
    for (std::size_t i = 0u; i < linksPastCap; ++i) {
        const std::string extra = "-" + std::to_string(i);
        const std::string next = content + extra;
        records.push_back(refDeltaRecord(oid, chainDelta(content, extra)));
        content = next;
        oid = objectOidOf(ObjectKind::Blob, content);
    }
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    HoldingPort port;
    ReceiveCommand create;
    create.name = "refs/tags/cap-plus-one";
    create.oldOid = std::string(kZeroOid);
    create.newOid = oid;
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(create);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(!ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(bodyFailed);
    // The bound is named in the words, at the number: the 1025th link is
    // refused the moment its depth passes the cap, not after the rounds run.
    CHECK_EQ(error, std::string("a delta chain in the pack runs past the ") +
                        std::to_string(ols::gitcore::kMaxDeltaChain) + " links this layer walks");
    CHECK_EQ(results.size(), 0u);
    CHECK_EQ(port.moves(), 0u);
    CHECK_EQ(port.written().size(), 0u);
}

// Exactly at the cap: 1024 links. Both roads keep the same side — the bare
// indexer refuses only past the cap (src/gitcore/receive.cpp:681, baseDepth >
// kMaxDeltaChain) and the pair reader only when a further link would stand
// past it (src/gitcore/packfile.cpp:1090-1093: the cap's link is pushed,
// then the walk stands on the full base and ends) — so a chain of exactly
// kMaxDeltaChain deltas resolves end to end and the command moves.
TEST(a_delta_chain_exactly_at_the_cap_resolves)
{
    const std::size_t linksAtCap = ols::gitcore::kMaxDeltaChain;
    std::string content = "chain base";
    std::string oid = objectOidOf(ObjectKind::Blob, content);

    std::vector<BuiltRecord> records;
    BuiltRecord base;
    base.head = recordHeadOf(static_cast<int>(RecordKind::Blob), content.size());
    if (!ols::gitcore::deflateStored(std::vector<std::uint8_t>(content.begin(), content.end()),
                                     base.body)) {
        base.body.clear();
    }
    records.push_back(base);
    for (std::size_t i = 0u; i < linksAtCap; ++i) {
        const std::string extra = "-" + std::to_string(i);
        const std::string next = content + extra;
        records.push_back(refDeltaRecord(oid, chainDelta(content, extra)));
        content = next;
        oid = objectOidOf(ObjectKind::Blob, content);
    }
    const std::vector<std::uint8_t> pack = barePackOf(records);
    CHECK(!pack.empty());

    HoldingPort port;
    ReceiveCommand create;
    create.name = "refs/tags/cap-exact";
    create.oldOid = std::string(kZeroOid);
    create.newOid = oid;
    std::vector<ReceiveCommand> cmds;
    cmds.push_back(create);

    ReceivePolicy policy;
    std::vector<std::pair<std::string, std::string>> results;
    bool bodyFailed = false;
    std::string error;
    CHECK(ols::gitcore::receive(port, pack, cmds, policy, results, bodyFailed, error));
    CHECK(!bodyFailed);
    CHECK_EQ(error, std::string());
    CHECK_EQ(results.size(), 1u);
    if (results.size() == 1u) {
        CHECK_EQ(results[0].first, std::string("refs/tags/cap-exact"));
        CHECK_EQ(results[0].second, std::string()); // the chain stood, the move ran
    }
    CHECK_EQ(port.moves(), 1u);
    CHECK_EQ(port.written().size(), linksAtCap + 1u); // the base and every link
}

// ===========================================================================
// The framing refusals of the command section, pinned at the door and at the
// parser: a push whose command section holds no command at all, and a
// zero-length payload that is not the flush. Both are malformed
// conversations — the request never reached its commands — so the answer is
// the fatal shape (the unpack-ng line and the flush, no per-ref rows), the
// same shape the door's other never-reached-its-commands refusals take, and
// never the empty report-status that reads back to a client as success. The
// bytes are stated with the lengths counted in the comments, the way the
// report-status cases above keep them.
// ===========================================================================

namespace {

// The door's fatal report-status for a command section that stated nothing,
// quoted once: "unpack ng the push commands do not stand: the push carried no
// commands\n": 10 + 32 + 28 + 1 = 71; +4 = 75 = 0x4b — and the flush.
const char kZeroCommandFatalBytes[] =
    "004bunpack ng the push commands do not stand: the push carried no commands\n"
    "0000";

} // anonymous namespace

// A v0 body holding no command: the walk of the section takes the first
// packet and ends at the next flush, so two flushes with nothing between
// them state a section that carries no command. The door refuses it with the
// fatal shape — the unpack-ng line alone — and never with "unpack ok"
// carrying no rows, which git send-pack would read back as a push that
// succeeded.
TEST(a_zero_command_push_on_the_v0_road_answers_the_fatal_shape)
{
    ScratchRepo scratch("door-zero-v0");
    CHECK(scratch.stood());

    std::vector<std::uint8_t> request;
    ols::gitcore::pktFlush(request);
    ols::gitcore::pktFlush(request);

    const std::vector<std::string> guarded;
    std::vector<std::uint8_t> answer;
    std::string words;
    const bool stood =
        ols::git::handleReceivePack(scratch.dir(), request, guarded, answer, words);
    CHECK(!stood);
    CHECK(!words.empty());
    CHECK(words.find("carried no commands") != std::string::npos);

    const std::string text = textOfBytes(answer);
    CHECK_EQ(text, std::string(kZeroCommandFatalBytes));
    CHECK_EQ(answer.size(), 79u);
    // The success word never stands in this answer, and no row names a ref.
    CHECK(text.find("unpack ok") == std::string::npos);
    CHECK(text.find("ok refs") == std::string::npos);
    // Plain report-status, not the band: the first payload byte is the "u" of
    // the unpack line, where a banded answer carries channel 1.
    CHECK(answer.size() > 4u);
    if (answer.size() > 4u) {
        CHECK_EQ(static_cast<int>(answer[4]), static_cast<int>('u'));
    }
}

// The v2 road: the command line, its delimiter, and the flush with no
// argument line between them. The empty argument section parses, the same
// refusal answers, and the bytes are the same as the v0 road's — both roads
// reach parseCommands and share the door's words.
TEST(a_zero_command_push_on_the_v2_road_answers_the_fatal_shape)
{
    ScratchRepo scratch("door-zero-v2");
    CHECK(scratch.stood());
    std::string why;

    std::vector<std::uint8_t> request;
    CHECK(ols::gitcore::pktWrite("command=receive-pack\n", request, why));
    ols::gitcore::pktDelim(request);
    ols::gitcore::pktFlush(request);

    const std::vector<std::string> guarded;
    std::vector<std::uint8_t> answer;
    std::string words;
    const bool stood =
        ols::git::handleReceivePack(scratch.dir(), request, guarded, answer, words);
    CHECK(!stood);
    CHECK(!words.empty());
    CHECK(words.find("carried no commands") != std::string::npos);
    const std::string text = textOfBytes(answer);
    CHECK_EQ(text, std::string(kZeroCommandFatalBytes));
    CHECK_EQ(answer.size(), 79u);
    CHECK(text.find("unpack ok") == std::string::npos);
}

// The parser, asked directly: an empty payload list, and lists that carry
// only the newline that ends the arguments, state no command. The refusal is
// one thing said plainly, and nothing is handed over with it.
TEST(parseCommands_refuses_a_command_section_that_states_no_command)
{
    std::vector<std::vector<std::string> > sections;
    sections.push_back(std::vector<std::string>());
    sections.push_back(std::vector<std::string>(1u, std::string("\n")));
    sections.push_back(std::vector<std::string>(2u, std::string("\n")));
    for (std::size_t i = 0u; i < sections.size(); ++i) {
        std::vector<ReceiveCommand> cmds;
        std::string error;
        CHECK(!ols::gitcore::parseCommands(sections[i], cmds, error));
        CHECK_EQ(error, std::string("the push carried no commands"));
        CHECK_EQ(cmds.size(), 0u);
    }
}

// The empty payload is not the flush: "0000" is a frame and never a payload
// here, so a payload of zero bytes is neither a command nor an end. It
// refuses wherever it stands — first, between, or last — and the words name
// the empty payload that arrived. A good line standing before it does not
// save the section: the empty payload ended nothing, so the push stands with
// nothing handed over.
TEST(parseCommands_refuses_an_empty_payload_and_names_it)
{
    const std::string words =
        "a command payload arrived empty: a zero-length payload is not a flush and states no command";
    const std::string good =
        std::string(kZeroOid) + " " + std::string(kStubCommit) + " refs/heads/ok";

    std::vector<std::string> alone;
    alone.push_back(std::string());
    std::vector<std::string> first;
    first.push_back(std::string());
    first.push_back(good);
    std::vector<std::string> last;
    last.push_back(good);
    last.push_back(std::string());

    std::vector<std::vector<std::string> > sections;
    sections.push_back(alone);
    sections.push_back(first);
    sections.push_back(last);
    for (std::size_t i = 0u; i < sections.size(); ++i) {
        std::vector<ReceiveCommand> cmds;
        std::string error;
        CHECK(!ols::gitcore::parseCommands(sections[i], cmds, error));
        CHECK_EQ(error, words);
        CHECK_EQ(cmds.size(), 0u);
    }
}

// The door reads a zero-length payload as the parser refuses it: "0004"
// stands where a command or its terminator should, and the answer is the
// fatal shape naming the empty payload — "unpack ng the push commands do not
// stand: <the empty payload's words>\n": 10 + 32 + 91 + 1 = 134; +4 = 138 =
// 0x8a — and the flush. A command standing before the empty payload answers
// the same bytes: the section did not end, and one good line does not make
// a push.
TEST(an_empty_payload_on_the_v0_road_is_refused_and_not_read_as_the_flush)
{
    ScratchRepo scratch("door-empty-payload");
    CHECK(scratch.stood());
    const std::vector<std::string> guarded;
    std::string why;

    std::vector<std::uint8_t> request;
    CHECK(ols::gitcore::pktWrite(std::string(), request, why)); // "0004"
    ols::gitcore::pktFlush(request);
    std::vector<std::uint8_t> answer;
    std::string words;
    const bool stood =
        ols::git::handleReceivePack(scratch.dir(), request, guarded, answer, words);
    CHECK(!stood);
    const std::string text = textOfBytes(answer);
    CHECK_EQ(text, std::string(
        "008aunpack ng the push commands do not stand: a command payload arrived empty: "
        "a zero-length payload is not a flush and states no command\n0000"));
    CHECK_EQ(answer.size(), 142u);
    CHECK(text.find("unpack ok") == std::string::npos);

    // The same refusal with a command before the empty payload.
    const std::vector<std::uint8_t> command = frameLine(
        std::string(kStubCommit), std::string(kZeroOid), "refs/heads/gone",
        "report-status delete-refs", why);
    CHECK(!command.empty());
    std::vector<std::uint8_t> withCommand;
    appended(withCommand, command);
    CHECK(ols::gitcore::pktWrite(std::string(), withCommand, why));
    ols::gitcore::pktFlush(withCommand);
    std::vector<std::uint8_t> second;
    std::string secondWords;
    const bool secondStood =
        ols::git::handleReceivePack(scratch.dir(), withCommand, guarded, second, secondWords);
    CHECK(!secondStood);
    CHECK_EQ(textOfBytes(second), text);

    // The scratch repository saw nothing of either request: no command
    // stood, so no ref was asked about.
    ols::git::RepoPort port(scratch.dir());
    std::string held;
    CHECK(!port.refValue("refs/heads/gone", held));
}

// The regression these framing refusals must not touch: a legitimate
// single-command push — a delete with no pack behind it, the shape that
// stands today — answered with the exact report-status bytes it has always
// carried. The fold the door gives a standing report's fields is identity on
// clean bytes: the unpack-ok line, the command's ok line, the flush.
TEST(a_single_command_push_answers_the_same_report_status_bytes_as_before)
{
    ScratchRepo scratch("door-single-command");
    CHECK(scratch.stood());
    ols::gitcore::ObjectId held;
    CHECK(ols::gitcore::ObjectId::parse(std::string(kStubCommit), held));
    ols::gitcore::Repository repo(scratch.dir());
    CHECK(repo.writeLooseRef("refs/heads/gone", held));

    std::string why;
    const std::vector<std::uint8_t> command = frameLine(
        std::string(kStubCommit), std::string(kZeroOid), "refs/heads/gone",
        "report-status delete-refs", why);
    CHECK(!command.empty());
    const std::vector<std::uint8_t> request = v0PushBody(command, false, why);
    CHECK(!request.empty());

    const std::vector<std::string> guarded;
    std::vector<std::uint8_t> answer;
    std::string words;
    const bool stood =
        ols::git::handleReceivePack(scratch.dir(), request, guarded, answer, words);
    CHECK(stood);
    CHECK(words.empty());

    std::string expected;
    // "unpack ok\n": 10; +4 = 14 = 0x0e
    expected += "000eunpack ok\n";
    // "ok refs/heads/gone\n": 3+15+1 = 19; +4 = 23 = 0x17
    expected += "0017ok refs/heads/gone\n";
    expected += "0000";
    CHECK_EQ(textOfBytes(answer), expected);
    CHECK_EQ(answer.size(), 41u);

    // and the bytes carry a real removal: the ref is gone.
    ols::git::RepoPort port(scratch.dir());
    std::string stillThere;
    CHECK(!port.refValue("refs/heads/gone", stillThere));
}

// The live-door shape: a v0 body of two delimiter pkt-lines and the flush —
// 0001 0001 0000, the exact bytes the live probe sent. commandSection walks
// specials on their way to the section's flush, so the section stands; it
// holds no payload line, and the door answers it before the dialect fork
// with the shared zero-command fatal bytes — the same answer the two-flush
// section gets. It is never the framing complaint with an empty reason
// hanging after the colon.
TEST(a_v0_body_of_delimiters_and_a_flush_answers_the_same_fatal_bytes)
{
    ScratchRepo scratch("door-delim-flush");
    CHECK(scratch.stood());

    std::vector<std::uint8_t> request;
    ols::gitcore::pktDelim(request);
    ols::gitcore::pktDelim(request);
    ols::gitcore::pktFlush(request);

    const std::vector<std::string> guarded;
    std::vector<std::uint8_t> answer;
    std::string words;
    const bool stood =
        ols::git::handleReceivePack(scratch.dir(), request, guarded, answer, words);
    CHECK(!stood);
    const std::string text = textOfBytes(answer);
    CHECK_EQ(text, std::string(kZeroCommandFatalBytes));
    CHECK_EQ(answer.size(), 79u);
    CHECK(words.empty() || words[words.size() - 1u] != ':');
    CHECK(words.find("not framed as") == std::string::npos);
    CHECK(text.find("not framed as") == std::string::npos);
    CHECK(text.find("unpack ok") == std::string::npos);
}

// A delimiter standing among v0 command lines still refuses the framing —
// now in words that name the special that broke the section, so the door's
// refusal never ends on a colon over an empty reason.
TEST(a_delim_among_v0_command_lines_is_refused_in_words_that_name_it)
{
    ScratchRepo scratch("door-delim-among");
    CHECK(scratch.stood());
    std::string why;

    const std::vector<std::uint8_t> command = frameLine(
        std::string(kStubCommit), std::string(kZeroOid), "refs/heads/gone",
        "report-status delete-refs", why);
    CHECK(!command.empty());
    std::vector<std::uint8_t> request;
    appended(request, command);
    ols::gitcore::pktDelim(request);
    ols::gitcore::pktFlush(request);

    const std::vector<std::string> guarded;
    std::vector<std::uint8_t> answer;
    std::string words;
    const bool stood =
        ols::git::handleReceivePack(scratch.dir(), request, guarded, answer, words);
    CHECK(!stood);
    // "the v0 push request is not framed as command lines: a delimiter
    // pkt-line stands in the v0 command section": 51+1+53 = 105 words;
    // "unpack ng " + words + newline = 116; +4 = 120 = 0x78 — and the flush.
    const std::string text = textOfBytes(answer);
    CHECK_EQ(text, std::string(
        "0078unpack ng the v0 push request is not framed as command lines: "
        "a delimiter pkt-line stands in the v0 command section\n0000"));
    CHECK_EQ(answer.size(), 124u);
    CHECK(words.empty() || words[words.size() - 1u] != ':');
    CHECK(words.find("delimiter") != std::string::npos);
}

// The other road that once answered with a dangling colon: a service
// preamble line with no flush-pkt after it. The walk refuses it at the
// second packet, and the refusal names what stands behind the marker.
TEST(a_service_preamble_line_with_no_flush_after_it_is_refused_in_words)
{
    ScratchRepo scratch("door-preamble-noflush");
    CHECK(scratch.stood());
    std::string why;

    std::vector<std::uint8_t> request;
    CHECK(ols::gitcore::pktWrite("# service=git-receive-pack\n", request, why));
    const std::vector<std::uint8_t> command = frameLine(
        std::string(kStubCommit), std::string(kZeroOid), "refs/heads/gone",
        "report-status delete-refs", why);
    CHECK(!command.empty());
    appended(request, command);

    const std::vector<std::string> guarded;
    std::vector<std::uint8_t> answer;
    std::string words;
    const bool stood =
        ols::git::handleReceivePack(scratch.dir(), request, guarded, answer, words);
    CHECK(!stood);
    // "the push request is not framed as a command section: a service
    // preamble line stands with no flush-pkt after it": 52+1+57 = 110;
    // "unpack ng " + words + newline = 121; +4 = 125 = 0x7d — and the flush.
    const std::string text = textOfBytes(answer);
    CHECK_EQ(text, std::string(
        "007dunpack ng the push request is not framed as a command section: "
        "a service preamble line stands with no flush-pkt after it\n0000"));
    CHECK_EQ(answer.size(), 129u);
    CHECK(words.empty() || words[words.size() - 1u] != ':');
    CHECK(words.find("flush-pkt") != std::string::npos);
}
