// src/GitSmartHttp.cpp — the ground GitSmartHttp.h stands on: the repository
// port over a bare repo dir, the push answer, the fetch pack and its answer.
//
// The walks state their bounds where the refusals can carry them: one fetch
// walks at most 65536 objects across both walks — the reachability of the
// haves and the missing part of the wants together — and says so over the
// cap rather than answering short. The pack the builder makes is full
// objects, no deltas, and it is read back through its own index before a
// byte of it is offered: a malformed pack is never an answer. Nothing here
// throws.

#include "GitSmartHttp.h"
#include "gitcore/objects.h"
#include "gitcore/packfile.h"
#include "gitcore/refs.h"
// The wire unit's ground, taken here as a unity include instead of a named
// source: its definitions are inline, so a source of its own would emit no
// symbols, and this door is one of the two that asks them. The same ground
// stands behind the harness cases through test_wire.cpp's include; inline
// definitions standing in several objects of one link fold to one.
// The wire unit travels with this translation unit: its definitions are
// inline-only and no build names wire.cpp as a source, so whoever folds this
// file in must carry the wire ground too - except a host that already pulled
// wire.cpp in earlier in the same translation unit (the harness does, through
// test_wire.cpp), which would see the anonymous namespace land twice.
#ifndef OLS_GITCORE_WIRE_CPP
#include "gitcore/wire.cpp"
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ols
{
namespace git
{

namespace {

// The one bound both fetch walks share, stated in the header too.
constexpr std::size_t kFetchWalkCap = 65536u;

// The id of nothing, as an unborn HEAD is stated. This tree keeps one hash
// format — the v2 advertisement says object-format=sha1 — so the zeros are
// the sha1 width.
constexpr const char kZeroOid[] = "0000000000000000000000000000000000000000";

// The one words a push that stated no command is refused with. The parser in
// gitcore refuses an argument list that parses empty with its own copy of
// these words (src/gitcore/receive.cpp:1084); the door's fork answers the
// sections that never reach the parser with this copy, so every no-command
// road — v0 specials, v2 empty arguments — answers one byte-exact thing, as
// the harness pins it.
constexpr const char kNoCommandsWords[] = "the push carried no commands";

// Whole-file read, the shape Common.h keeps; this file reads a whole file
// only when opening one of the repository's packfiles, whose sizes the
// pack's own bounds keep honest.
bool readFileText(const std::string &path, std::string &out)
{
    out.clear();
    std::ifstream in{path, std::ios::binary};
    if (!in.good())
    {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    return true;
}

// An id as this file moves it: lowercase hex of one of the two widths, or
// nothing when the text is not one. ProjectGit.cpp's normHex rule, restated
// here because that helper belongs to that file.
std::string normHex(const std::string &text)
{
    ols::gitcore::ObjectId id;
    if (!ols::gitcore::ObjectId::parse(text, id))
    {
        return std::string();
    }
    return id.to_hex();
}

// A reason that will stand in a report-status ng line or a fatal sideband
// line is one line: breaks are walked over, never trusted, and a NUL goes with
// them — report-status states no field with one, and a reason carrying a NUL
// would leave the whole answer unwritable, so the client would get no report
// where a refusal was the honest answer.
std::string oneLine(std::string text)
{
    for (char &c : text)
    {
        if (c == '\n' || c == '\r' || c == '\0')
        {
            c = ' ';
        }
    }
    return text;
}

// The kind word the port speaks, as the kind the pack writer wants. The two
// enums stay apart; the word is the honest middle.
bool kindWordToRecord(const std::string &word, ols::gitcore::RecordKind &out)
{
    if (word == "commit")
    {
        out = ols::gitcore::RecordKind::Commit;
        return true;
    }
    if (word == "tree")
    {
        out = ols::gitcore::RecordKind::Tree;
        return true;
    }
    if (word == "blob")
    {
        out = ols::gitcore::RecordKind::Blob;
        return true;
    }
    if (word == "tag")
    {
        out = ols::gitcore::RecordKind::Tag;
        return true;
    }
    return false;
}

// gitprotocol-v2 ends the command section at its flush-pkt and the binary
// follows the next byte. The walk stops at that flush and hands its
// position back; bytes that never carry one are not a request.
//
// A v0 client may put the service preamble in front of its commands: the
// "# service=git-receive-pack" line it stated to /info/refs, framed, with a
// flush-pkt after it. git's own receive-pack reads past that marker, and a
// client that sends it is not sending a command with it. So the section is
// taken to begin after the marker and its flush when both stand at the front
// of the body, and at byte 0 otherwise — a command line opens with a hex id
// (v0) or with "command=" (v2), never with the marker, so the test cannot
// mistake one for the other. startOut is where the commands begin and endOut
// one past the flush that closes them; the pack follows endOut.
bool commandSection(const std::vector<std::uint8_t> &bytes,
                    std::size_t &startOut, std::size_t &endOut, std::string &error)
{
    startOut = 0u;
    endOut = 0u;
    ols::gitcore::PktReader reader(bytes.data(), bytes.size());
    ols::gitcore::PktKind kind;
    std::string_view view;
    if (!reader.next(kind, view, error))
    {
        return false;
    }
    static const char marker[] = "# service=";
    constexpr std::size_t markerLen = sizeof(marker) - 1u;
    if (kind == ols::gitcore::PktKind::Data && view.size() >= markerLen &&
        view.compare(0u, markerLen, marker) == 0)
    {
        if (!reader.next(kind, view, error))
        {
            return false;
        }
        if (kind != ols::gitcore::PktKind::Flush)
        {
            // A marker with nothing after it is not the preamble. The bytes
            // are still refused from the front, and the refusal names what
            // stands behind the marker — no door road hangs a colon on an
            // empty reason.
            error = "a service preamble line stands with no flush-pkt after it";
            return false;
        }
        startOut = reader.position();
    }
    for (;;)
    {
        if (!reader.next(kind, view, error))
        {
            return false;
        }
        if (kind == ols::gitcore::PktKind::Flush)
        {
            endOut = reader.position();
            return true;
        }
    }
}

// Which dialect a receive-pack body speaks: gitprotocol-v2's command-request
// opens with its "command=" line, and no v0 command line can — a v0 line
// opens with a hex id. So the first payload names the dialect: "command=" is
// v2, and everything else is v0's command lines, whether or not the client
// put its capabilities after a NUL. The bytes walked are the command section,
// the part before the first flush-pkt that commandSection found.
bool isV2ReceiveRequest(const std::uint8_t *bytes, std::size_t sectionEnd)
{
    static const char opening[] = "command=";
    constexpr std::size_t openingLen = sizeof(opening) - 1u;
    ols::gitcore::PktReader reader(bytes, sectionEnd);
    ols::gitcore::PktKind kind;
    std::string_view view;
    std::string err;
    while (reader.next(kind, view, err))
    {
        if (kind != ols::gitcore::PktKind::Data)
        {
            return false; // a section that opens with a special states no command
        }
        return view.size() >= openingLen && view.compare(0u, openingLen, opening) == 0;
    }
    return false;
}

// v0's command section as the argument lines gitcore's parser takes: each
// payload is "<old> <new> <name>" with the first one's capabilities standing
// after a NUL, which is walked over; a payload's trailing newline belongs to
// the frame, and the parser drops it. A delim in the section is not v0's
// shape, and the refusal names the special that broke it; a section framed by
// specials alone carries no command at all and is answered at the door's
// fork before this splitter runs.
bool v0ArgumentLines(const std::uint8_t *bytes, std::size_t sectionEnd,
                     std::vector<std::string> &lines, std::string &error)
{
    lines.clear();
    error.clear();
    ols::gitcore::PktReader reader(bytes, sectionEnd);
    ols::gitcore::PktKind kind;
    std::string_view view;
    std::string err;
    while (reader.next(kind, view, err))
    {
        if (kind == ols::gitcore::PktKind::Flush)
        {
            break;
        }
        if (kind != ols::gitcore::PktKind::Data)
        {
            lines.clear();
            error = "a delimiter pkt-line stands in the v0 command section";
            return false;
        }
        std::string line(view);
        const std::string::size_type nul = line.find('\0');
        if (nul != std::string::npos)
        {
            line.resize(nul);
        }
        lines.push_back(std::move(line));
    }
    if (!err.empty())
    {
        lines.clear();
        error = err;
        return false;
    }
    return true;
}

// The splitter as a boolean alone; the version beside it names the refusal.
bool v0ArgumentLines(const std::uint8_t *bytes, std::size_t sectionEnd,
                     std::vector<std::string> &lines)
{
    std::string error;
    return v0ArgumentLines(bytes, sectionEnd, lines, error);
}

// Whether a payload line stands in the command section at all. The walk of
// commandSection crosses specials on its way to the section's flush, so a
// section can reach the fork holding nothing but specials — the two-flush
// body, the body framed by delimiters alone — and hold no command for either
// dialect. isV2ReceiveRequest reads the same fact from a section that opens
// with a special; this reads it from the whole span.
bool sectionCarriesPayload(const std::uint8_t *bytes, std::size_t sectionEnd)
{
    ols::gitcore::PktReader reader(bytes, sectionEnd);
    ols::gitcore::PktKind kind;
    std::string_view view;
    std::string err;
    while (reader.next(kind, view, err))
    {
        if (kind == ols::gitcore::PktKind::Data)
        {
            return true;
        }
    }
    return false;
}

// Words a report can carry: one line, and short enough that the frame the
// answer states them in holds them. A refusal's words are the door's own over
// what the receive or the parser said; losing the whole report to a reason of
// no length is not among the answers.
std::string framingWords(const std::string &text)
{
    std::string line = oneLine(text);
    constexpr std::size_t room = 4096u; // well inside one pkt-line, with its fields
    if (line.size() > room)
    {
        line.resize(room);
    }
    return line;
}

// The answer of a receive that refused, in the shape git send-pack reads back:
// report-status lines, not a sideband frame — a receive answer is parsed as
// plain pkt-lines. "unpack ng <reason>" stands only when the body itself did
// not stand: the pack or the request never became commands. A policy refusal
// is "unpack ok" with one ng line per requested ref, and a body failure keeps
// those ng lines after its unpack line too.
bool answerNg(std::vector<std::uint8_t> &out,
              const std::vector<ols::gitcore::ReceiveCommand> &cmds,
              std::string reason, bool bodyFailed)
{
    out.clear();
    reason = framingWords(reason);
    std::vector<std::pair<std::string, std::string>> rows;
    rows.reserve(cmds.size());
    for (const ols::gitcore::ReceiveCommand &cmd : cmds)
    {
        // The name is a field of the same line as the reason, so it is walked
        // by the same rule: a name the client sent with a break or a NUL in it
        // is stated as one line or the answer does not stand at all.
        rows.push_back(std::make_pair(oneLine(cmd.name), reason));
    }
    std::string err;
    const bool framed = bodyFailed
                            ? ols::gitcore::reportStatus(rows, reason, out, err)
                            : ols::gitcore::reportStatus(rows, out, err);
    if (!framed)
    {
        out.clear();
        return false;
    }
    return true;
}

// The answer of a request that never reached its commands: no ref is known, so
// the unpack line carries the reason alone and no ng line can name one. The
// reason is walked like every other word a report carries.
bool answerReceiveFatal(std::vector<std::uint8_t> &out, const std::string &reason)
{
    out.clear();
    std::string err;
    if (!ols::gitcore::reportStatus(std::vector<std::pair<std::string, std::string>>(),
                                    framingWords(reason), out, err))
    {
        out.clear();
        return false;
    }
    return true;
}

// Whether a capability word stands whole in a list of them: bounded before by
// the start of the list, a space, or a NUL, and after by its end, a space, a
// newline, or a NUL.
bool namesCapability(const std::string_view list, const char *cap, std::size_t capLen)
{
    const char *text = list.data();
    for (std::size_t at = 0; at + capLen <= list.size();)
    {
        const std::string_view rest(text + at, list.size() - at);
        const std::string_view::size_type found = rest.find(cap);
        if (found == std::string_view::npos)
        {
            return false;
        }
        at += found;
        const bool beforeStands = at == 0u || text[at - 1u] == ' ' || text[at - 1u] == '\0';
        const std::size_t after = at + capLen;
        const bool afterStands = after == list.size() || text[after] == ' ' ||
                                 text[after] == '\n' || text[after] == '\0';
        if (beforeStands && afterStands)
        {
            return true;
        }
        ++at;
    }
    return false;
}

// Whether a push asked to be answered through the sideband. The ask stands in
// the capabilities, and v0 puts them in one place: after the NUL of the first
// command line. v2 names no band at all — what a v2 client sends behind its
// command line are command arguments, and the answer of a v2 command is
// framed by that command's own sections, not by a capability — so the caps of
// that one payload are the whole place the ask can stand, in either dialect.
// Walking further is how a ref named "side-band-64k" banded an answer no
// client reads: the whole report would ride a channel the client never asked
// for, and the plain report-status it waited for never arrived.
bool asksSideband(const std::uint8_t *bytes, std::size_t count)
{
    static const char cap[] = "side-band-64k";
    constexpr std::size_t capLen = sizeof(cap) - 1u;
    ols::gitcore::PktReader reader(bytes, count);
    ols::gitcore::PktKind kind;
    std::string_view view;
    std::string err;
    if (!reader.next(kind, view, err) || kind != ols::gitcore::PktKind::Data)
    {
        return false;
    }
    const std::string_view::size_type nul = view.find('\0');
    if (nul == std::string_view::npos)
    {
        return false; // the line carries no capabilities, so no ask
    }
    return namesCapability(view.substr(nul + 1u), cap, capLen);
}

// The whole of an answer, framed for the sideband a banded client reads:
// every byte rides channel 1 in frames under the band's ceiling, and the
// multiplexing ends the way git ends it — one empty channel-2 frame, then
// the flush.
bool frameSideband(const std::vector<std::uint8_t> &answer,
                   std::vector<std::uint8_t> &out, std::string &error)
{
    out.clear();
    const std::string body(answer.begin(), answer.end());
    for (std::size_t at = 0; at < body.size(); at += ols::gitcore::kMaxSidebandData)
    {
        const std::size_t left = body.size() - at;
        const std::size_t take =
            left < ols::gitcore::kMaxSidebandData ? left : ols::gitcore::kMaxSidebandData;
        if (!ols::gitcore::v2Sideband(1u, body.substr(at, take), out, error))
        {
            out.clear();
            return false;
        }
    }
    if (!ols::gitcore::v2Sideband(2u, std::string(), out, error))
    {
        out.clear();
        return false;
    }
    ols::gitcore::pktFlush(out);
    return true;
}

// The answer of a request that never reached its commands: one fatal
// sideband line, closed with the flush. The reason is walked down to what
// fits one line.
void answerFatal(std::vector<std::uint8_t> &out, const std::string &reason)
{
    out.clear();
    std::string payload = "ERR " + oneLine(reason);
    payload.push_back('\n');
    if (payload.size() > ols::gitcore::kMaxSidebandData)
    {
        payload = payload.substr(0u, ols::gitcore::kMaxSidebandData - 1u);
        payload.push_back('\n');
    }
    std::string err;
    if (!ols::gitcore::v2Sideband(static_cast<std::uint8_t>(3), payload, out, err))
    {
        out.clear();
        return;
    }
    ols::gitcore::pktFlush(out);
}

// The children an object's own content names: a commit offers its root and
// its parents, a tree its entries, a tag its target. Content this layer
// cannot read stops the walk with the reason it names.
bool pushChildren(const std::string &kind, const std::string &content,
                  std::vector<std::string> &stack, std::string &error)
{
    if (kind == "blob")
    {
        return true;
    }
    if (kind == "commit")
    {
        ols::gitcore::CommitObject commit;
        if (!ols::gitcore::commitParse(content, commit, error))
        {
            error = "the commit is not one this layer reads: " + error;
            return false;
        }
        if (!commit.tree.is_null())
        {
            stack.push_back(commit.tree.hex);
        }
        for (const ols::gitcore::ObjectId &parent : commit.parents)
        {
            if (!parent.is_null())
            {
                stack.push_back(parent.hex);
            }
        }
        return true;
    }
    if (kind == "tree")
    {
        ols::gitcore::TreeObject tree;
        if (!ols::gitcore::treeParse(content, tree, error))
        {
            error = "the tree is not one this layer reads: " + error;
            return false;
        }
        for (const ols::gitcore::TreeEntry &entry : tree.entries)
        {
            if (!entry.id.is_null())
            {
                stack.push_back(entry.id.hex);
            }
        }
        return true;
    }
    if (kind == "tag")
    {
        ols::gitcore::TagObject tag;
        if (!ols::gitcore::tagParse(content, tag, error))
        {
            error = "the tag is not one this layer reads: " + error;
            return false;
        }
        if (!tag.target.is_null())
        {
            stack.push_back(tag.target.hex);
        }
        return true;
    }
    error = "the kind word is not one of the four git has: " + kind;
    return false;
}

// The pack for a fetch: everything reachable from the wants the haves do
// not already reach, as a standalone pack of full objects the writer's own
// index reads back. The two walks share one cap: an over-cap is a refusal,
// never a short answer. A want the repository does not hold refuses the
// whole fetch — an unheld have is simply left out of the reachability, as
// it states nothing the server could reuse.
bool buildWantPack(const std::string &repoDir,
                   const std::vector<std::string> &wants,
                   const std::vector<std::string> &haves,
                   std::vector<std::uint8_t> &packOut,
                   std::string &words)
{
    packOut.clear();
    words.clear();
    RepoPort port(repoDir);

    auto refuse = [&](const std::string &why) {
        words = oneLine(why);
        packOut.clear();
        return false;
    };

    std::vector<std::string> wantIds;
    wantIds.reserve(wants.size());
    for (const std::string &text : wants)
    {
        const std::string hex = normHex(text);
        if (hex.empty())
        {
            return refuse("want " + text + " is not a lowercase hex id of one of the two widths");
        }
        if (!port.hasObject(hex))
        {
            return refuse("the server does not hold the want " + hex +
                          "; a v2 client asks only for what the advertisement stated");
        }
        wantIds.push_back(hex);
    }
    std::vector<std::string> haveIds;
    for (const std::string &text : haves)
    {
        const std::string hex = normHex(text);
        if (hex.empty())
        {
            return refuse("have " + text + " is not a lowercase hex id of one of the two widths");
        }
        if (port.hasObject(hex))
        {
            haveIds.push_back(hex);
        }
    }

    // The reachability the haves state.
    std::unordered_set<std::string> reachable;
    std::size_t walked = 0u;
    std::vector<std::string> stack = haveIds;
    while (!stack.empty())
    {
        const std::string oid = stack.back();
        stack.pop_back();
        if (reachable.count(oid) != 0u)
        {
            continue;
        }
        if (walked >= kFetchWalkCap)
        {
            return refuse("the reachability walk over the haves reached its bound of " +
                          std::to_string(kFetchWalkCap) + " objects");
        }
        ++walked;
        reachable.insert(oid);
        std::string kind;
        std::string content;
        if (!port.readObject(oid, kind, content))
        {
            return refuse("the walk reached " + oid + " whose bytes do not stand");
        }
        if (!pushChildren(kind, content, stack, words))
        {
            return refuse(words);
        }
    }

    // The wants' part the haves do not reach, collected in one walk.
    std::unordered_set<std::string> sent;
    std::vector<ols::gitcore::LooseForPack> objects;
    std::vector<std::string> names;
    stack = wantIds;
    while (!stack.empty())
    {
        const std::string oid = stack.back();
        stack.pop_back();
        if (reachable.count(oid) != 0u || sent.count(oid) != 0u)
        {
            continue;
        }
        if (walked >= kFetchWalkCap)
        {
            return refuse("the fetch walk reached its bound of " +
                          std::to_string(kFetchWalkCap) + " objects");
        }
        ++walked;
        sent.insert(oid);
        std::string kind;
        std::string content;
        if (!port.readObject(oid, kind, content))
        {
            return refuse("the walk reached " + oid + " whose bytes do not stand");
        }
        ols::gitcore::RecordKind record = ols::gitcore::RecordKind::Blob;
        if (!kindWordToRecord(kind, record))
        {
            return refuse("the walk reached an object whose kind this writer cannot carry: " + kind);
        }
        ols::gitcore::LooseForPack entry;
        entry.kind = record;
        entry.content.assign(content.begin(), content.end());
        objects.push_back(std::move(entry));
        names.push_back(oid);
        if (!pushChildren(kind, content, stack, words))
        {
            return refuse(words);
        }
    }

    // The pack and the index the writer states for it.
    std::vector<ols::gitcore::IndexEntry> entries;
    if (!ols::gitcore::writePack(objects, packOut, entries))
    {
        return refuse("pack builder incomplete: the pack writer refused the object set it was given");
    }
    if (entries.size() != names.size())
    {
        return refuse("pack builder incomplete: the writer did not state an index entry for every record");
    }
    for (std::size_t at = 0u; at < names.size(); ++at)
    {
        if (entries[at].oid.hex != names[at])
        {
            return refuse("pack builder incomplete: the record at " + names[at] +
                          " is not the bytes that name says");
        }
    }

    // The pack stands alone: the idx the writer states, read back against
    // the bytes, opens the pair and names the same object count. An answer
    // that does not pass this is refused, never sent.
    if (packOut.size() < 12u + 20u)
    {
        return refuse("pack builder incomplete: the bytes the writer made are fewer than a pack header and its trailer take");
    }
    std::array<std::uint8_t, 20> checksum{};
    std::copy(packOut.end() - 20, packOut.end(), checksum.begin());
    std::vector<ols::gitcore::IndexEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const ols::gitcore::IndexEntry &a, const ols::gitcore::IndexEntry &b) {
                  return a.oid.hex < b.oid.hex;
              });
    std::vector<std::uint8_t> idx;
    if (!ols::gitcore::writeIdx(sorted, checksum, idx))
    {
        return refuse("pack builder incomplete: the index the writer stated is not one this layer writes");
    }
    ols::gitcore::Packfile check;
    if (!check.open(packOut, idx))
    {
        return refuse("pack builder incomplete: the pack does not stand when its own index is read back");
    }
    if (check.count() != objects.size())
    {
        return refuse("pack builder incomplete: the pack opened against its own index names a different count");
    }
    return true;
}

} // anonymous namespace

// --------------------------------------------------------------------------
// The port.
// --------------------------------------------------------------------------

RepoPort::RepoPort(std::string repoDir) : dir_(std::move(repoDir)) {}

void RepoPort::ensurePacks() const
{
    if (packsOpened_)
    {
        return;
    }
    std::vector<std::string> found;
    std::error_code ec{};
    const std::filesystem::path packDir{dir_ + "/objects/pack"};
    for (std::filesystem::directory_iterator it{packDir, ec}, end{}; it != end; it.increment(ec))
    {
        if (it->is_directory(ec))
        {
            continue;
        }
        const std::filesystem::path full = it->path();
        if (full.filename().extension() != ".pack")
        {
            continue;
        }
        found.push_back(full.u8string());
    }
    std::sort(found.begin(), found.end());
    for (const std::string &packPath : found)
    {
        const std::string::size_type dot = packPath.rfind(".pack");
        if (dot == std::string::npos)
        {
            continue;
        }
        const std::string idxPath = packPath.substr(0u, dot) + ".idx";
        std::error_code missing{};
        if (!std::filesystem::is_regular_file(idxPath, missing))
        {
            continue;
        }
        std::string packText, idxText;
        if (!readFileText(packPath, packText))
        {
            continue;
        }
        if (!readFileText(idxPath, idxText))
        {
            continue;
        }
        const std::vector<std::uint8_t> packBytes(packText.begin(), packText.end());
        const std::vector<std::uint8_t> idxBytes(idxText.begin(), idxText.end());
        std::unique_ptr<ols::gitcore::Packfile> pack = std::make_unique<ols::gitcore::Packfile>();
        if (!pack->open(packBytes, idxBytes))
        {
            continue;
        }
        packs_.push_back(std::move(pack));
    }
    packsOpened_ = true;
}

bool RepoPort::hasObject(const std::string &oidHex) const
{
    const std::string hex = normHex(oidHex);
    if (hex.empty())
    {
        return false;
    }
    ols::gitcore::ObjectId id;
    if (!ols::gitcore::ObjectId::parse(hex, id))
    {
        return false;
    }
    ensurePacks();
    for (const std::unique_ptr<ols::gitcore::Packfile> &pack : packs_)
    {
        ols::gitcore::PackObject po;
        if (pack->lookup(id, po))
        {
            return true;
        }
    }
    // The loose claim is the path one: a file standing under the name its
    // bytes should hash to. Whether those bytes read is readObject's claim.
    std::string path;
    if (ols::gitcore::looseObjectPath(dir_ + "/objects", id, path))
    {
        std::error_code ec{};
        if (std::filesystem::is_regular_file(path, ec))
        {
            return true;
        }
    }
    return false;
}

bool RepoPort::readObject(const std::string &oidHex, std::string &kindOut,
                          std::string &contentOut) const
{
    kindOut.clear();
    contentOut.clear();
    ols::gitcore::ObjectId id;
    if (!ols::gitcore::ObjectId::parse(oidHex, id))
    {
        return false;
    }
    ols::gitcore::LooseObject lo;
    std::string why;
    if (ols::gitcore::looseObjectReadAt(dir_ + "/objects", id, lo, why))
    {
        kindOut = ols::gitcore::kindName(lo.kind);
        contentOut.swap(lo.content);
        return true;
    }
    ensurePacks();
    for (const std::unique_ptr<ols::gitcore::Packfile> &pack : packs_)
    {
        std::vector<std::uint8_t> raw;
        if (!pack->resolveOid(id, raw))
        {
            continue;
        }
        const std::string rawBytes(raw.begin(), raw.end());
        ols::gitcore::ObjectKind kind = ols::gitcore::ObjectKind::Blob;
        std::uint64_t size = 0ull;
        std::string content;
        if (!ols::gitcore::objectSplit(rawBytes, kind, size, content, why))
        {
            // The pack holds the name and its bytes are not an object: a
            // refusal, not an absence.
            return false;
        }
        kindOut = ols::gitcore::kindName(kind);
        contentOut.swap(content);
        return true;
    }
    return false;
}

bool RepoPort::writeLoose(const std::string &kind, const std::string &content,
                          std::string &oidHex)
{
    oidHex.clear();
    ols::gitcore::ObjectKind type = ols::gitcore::ObjectKind::Blob;
    std::string why;
    if (!ols::gitcore::kindOfName(kind, type, why))
    {
        return false;
    }
    ols::gitcore::ObjectId id;
    if (!ols::gitcore::looseObjectWrite(dir_ + "/objects", type, content, id, why))
    {
        return false;
    }
    oidHex = id.to_hex();
    return true;
}

bool RepoPort::refValue(const std::string &name, std::string &oidHex) const
{
    oidHex.clear();
    const ols::gitcore::Repository repo(dir_);
    ols::gitcore::ObjectId out;
    // resolveRef is the loose-over-packed read, symref chains walked inside.
    if (repo.resolveRef(name, out) != ols::gitcore::RefStatus::Ok)
    {
        return false;
    }
    oidHex = out.to_hex();
    return true;
}

bool RepoPort::commitRefs(const std::vector<ols::gitcore::RefUpdate> &updates)
{
    ols::gitcore::Repository repo(dir_);
    return repo.commitRefs(updates);
}

bool RepoPort::deleteRefs(const std::vector<std::string> &names)
{
    if (names.empty())
    {
        return true;
    }
    ols::gitcore::Repository repo(dir_);
    // Nothing moves until every name is there and concrete.
    for (const std::string &name : names)
    {
        if (!ols::gitcore::isValidRefName(name))
        {
            return false;
        }
        std::string raw;
        if (repo.readRawRef(name, raw) != ols::gitcore::RefStatus::Ok)
        {
            return false;
        }
        if (ols::gitcore::looksLikeSymref(raw))
        {
            return false;
        }
    }
    // No packed-refs file is an empty packed set, not a refusal: the file is
    // optional, and a delete of loose-only refs must not be held off by its
    // absence. A file that stands and does not parse still refuses.
    std::vector<ols::gitcore::PackedRefEntry> packed;
    std::error_code packedMissing{};
    if (std::filesystem::exists(dir_ + "/packed-refs", packedMissing) &&
        !repo.readPackedRefs(packed))
    {
        return false;
    }
    std::vector<ols::gitcore::PackedRefEntry> kept;
    kept.reserve(packed.size());
    bool rewrote = false;
    for (const ols::gitcore::PackedRefEntry &entry : packed)
    {
        bool named = false;
        for (const std::string &name : names)
        {
            if (entry.name == name)
            {
                named = true;
                break;
            }
        }
        if (named)
        {
            rewrote = true;
            continue;
        }
        kept.push_back(entry);
    }
    for (const std::string &name : names)
    {
        // A delete that took no loose file is only a no-op where the name
        // stood packed alone — there, the rewrite below is the removal. Every
        // other false is a ref that stays put, and saying so is the honest
        // answer: presence was checked above, where readRawRef reads loose
        // first and packed after, so what is left here is a delete that did
        // not go for a reason the read did not see.
        if (!repo.deleteLooseRef(name))
        {
            bool stoodPacked = false;
            for (const ols::gitcore::PackedRefEntry &entry : packed)
            {
                if (entry.name == name)
                {
                    stoodPacked = true;
                    break;
                }
            }
            if (!stoodPacked)
            {
                return false;
            }
        }
    }
    if (rewrote && !repo.writePackedRefs(kept))
    {
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// The two commands and the helper.
// --------------------------------------------------------------------------

static bool receivePackCommands(const std::string &repoDir,
                                const std::vector<std::uint8_t> &requestBytes,
                                const std::vector<std::string> &protectedBranches,
                                std::vector<std::uint8_t> &answerOut,
                                std::string &words)
{
    answerOut.clear();
    words.clear();

    std::size_t sectionStart = 0u;
    std::size_t sectionEnd = 0u;
    std::string why;
    if (!commandSection(requestBytes, sectionStart, sectionEnd, why))
    {
        words = oneLine("the push request is not framed as a command section: " + why);
        if (!answerReceiveFatal(answerOut, words))
        {
            answerOut.clear();
        }
        return false;
    }
    // The commands as one span: past the service preamble when the body
    // carried one, up to the flush that closes them.
    const std::uint8_t *const commands = requestBytes.data() + sectionStart;
    const std::size_t commandCount = sectionEnd - sectionStart;

    // A command section that walked to its flush holding nothing but specials
    // — the two-flush body, the body framed by delimiters alone — carries no
    // command for either dialect's reader, so it is answered before the
    // dialect is read, with the parser's own zero-command words: every
    // no-command road answers one byte-exact thing.
    if (!sectionCarriesPayload(commands, commandCount))
    {
        words = oneLine("the push commands do not stand: " + std::string(kNoCommandsWords));
        if (!answerReceiveFatal(answerOut, words))
        {
            answerOut.clear();
        }
        return false;
    }

    std::vector<ols::gitcore::ReceiveCommand> cmds;
    if (!isV2ReceiveRequest(commands, commandCount))
    {
        std::vector<std::string> lines;
        if (!v0ArgumentLines(commands, commandCount, lines, why))
        {
            words = oneLine("the v0 push request is not framed as command lines: " + why);
            if (!answerReceiveFatal(answerOut, words))
            {
                answerOut.clear();
            }
            return false;
        }
        // The parser keeps the zero-command refusal for the lines that do
        // reach it stating no command — a section whose only payload is a
        // bare newline — with the words the fork answers, so the roads stay
        // one answer.
        if (!ols::gitcore::parseCommands(lines, cmds, why))
        {
            words = oneLine("the push commands do not stand: " + why);
            if (!answerReceiveFatal(answerOut, words))
            {
                answerOut.clear();
            }
            return false;
        }
    }
    else
    {
        ols::gitcore::V2Request v2;
        if (!ols::gitcore::v2ParseRequest(commands, commandCount, v2, why))
        {
            words = oneLine("the push command request does not stand: " + why);
            if (!answerReceiveFatal(answerOut, words))
            {
                answerOut.clear();
            }
            return false;
        }
        if (v2.command != "receive-pack")
        {
            words = "the request carries the command " +
                    (v2.command.empty() ? std::string("(none)") : v2.command) +
                    ", not receive-pack";
            if (!answerReceiveFatal(answerOut, words))
            {
                answerOut.clear();
            }
            return false;
        }
        if (!ols::gitcore::parseCommands(v2.argumentLines, cmds, why))
        {
            words = oneLine("the push commands do not stand: " + why);
            if (!answerReceiveFatal(answerOut, words))
            {
                answerOut.clear();
            }
            return false;
        }
    }

    const std::vector<std::uint8_t> body(requestBytes.begin() + sectionEnd,
                                         requestBytes.end());

    RepoPort port(repoDir);
    ols::gitcore::ReceivePolicy policy;
    policy.protectedBranches = protectedBranches;
    // The rest of the policy keeps its stated defaults: no non-fast-forward,
    // and the pack at its stated cap — 64 MiB, the one bound the bare-pack
    // indexer keeps as well.

    std::vector<std::pair<std::string, std::string>> results;
    std::string err;
    bool bodyFailed = false;
    if (!ols::gitcore::receive(port, body, cmds, policy, results, bodyFailed, err))
    {
        const std::string reason = oneLine(err);
        if (!answerNg(answerOut, cmds, reason, bodyFailed))
        {
            answerOut.clear();
        }
        words = reason;
        return false;
    }

    // The same fold the ng road gives its words, and for the same reason: a
    // standing push's refusal may quote a path a tree entry named, and a name
    // git writes is bytes between slashes — a break or an over-long quote is
    // walked to one short line, never allowed to cost the report its shape.
    for (std::pair<std::string, std::string> &row : results)
    {
        row.first = oneLine(row.first);
        row.second = framingWords(row.second);
    }
    if (!ols::gitcore::reportStatus(results, answerOut, why))
    {
        words = oneLine("the report-status answer does not fit the framing: " + why);
        if (!answerReceiveFatal(answerOut, words))
        {
            answerOut.clear();
        }
        return false;
    }
    words.clear();
    return true;
}

// The door's whole of one push: the report the command builder made, answered
// in whichever framing the command section asked for. A client whose caps
// name side-band-64k reads the whole report behind the band, and gets it
// there; the rest are answered plain, as the writer framed them.
bool handleReceivePack(const std::string &repoDir,
                       const std::vector<std::uint8_t> &requestBytes,
                       const std::vector<std::string> &protectedBranches,
                       std::vector<std::uint8_t> &answerOut,
                       std::string &words)
{
    answerOut.clear();
    // The same split the command builder walks, so the two agree on where the
    // commands stand — a service preamble in front of them read past once,
    // here as there. A body whose section does not stand asks no band: the
    // refusal it gets is stated by the builder, plain.
    std::size_t sectionStart = 0u;
    std::size_t sectionEnd = 0u;
    std::string why;
    const bool banded = commandSection(requestBytes, sectionStart, sectionEnd, why) &&
                        asksSideband(requestBytes.data() + sectionStart,
                                     sectionEnd - sectionStart);

    std::vector<std::uint8_t> plain;
    const bool stood =
        receivePackCommands(repoDir, requestBytes, protectedBranches, plain, words);
    if (!banded || plain.empty())
    {
        answerOut.swap(plain);
    }
    else
    {
        std::string err;
        if (!frameSideband(plain, answerOut, err))
        {
            answerOut.clear();
            words = oneLine("the banded answer does not fit the framing: " + err);
        }
    }
    if (answerOut.empty())
    {
        // The answer of a push is never nothing. A client that sent its
        // commands waits on a report, and an empty body reads to it as a pipe
        // that went dead mid-word; where the shape the answer wanted did not
        // frame, the refusal is stated in the shape that always does — the
        // band's fatal line for a client that asked for the band, the
        // unpack-ng line for every other.
        const std::string refusalLine =
            words.empty() ? std::string("the push was refused") : framingWords(words);
        if (banded)
        {
            answerFatal(answerOut, refusalLine);
        }
        else
        {
            answerReceiveFatal(answerOut, refusalLine);
        }
    }
    return stood;
}

bool handleUploadPackFetch(const std::string &repoDir,
                           const ols::gitcore::V2Request &request,
                           const std::vector<std::uint8_t> &bodyRest,
                           std::vector<std::uint8_t> &answerOut,
                           std::string &words)
{
    answerOut.clear();
    words.clear();

    auto refuse = [&](const std::string &why) {
        words = oneLine(why);
        answerFatal(answerOut, words);
        return false;
    };

    if (!bodyRest.empty())
    {
        return refuse("a v2 upload-pack fetch command carries no body; bytes stand after its flush");
    }

    ols::gitcore::FetchRequest fr;
    std::string why;
    if (!ols::gitcore::v2ParseFetch(request, fr, why))
    {
        return refuse("the fetch command does not stand: " + why);
    }

    std::vector<std::uint8_t> pack;
    if (!buildWantPack(repoDir, fr.wants, fr.haves, pack, words))
    {
        if (words.empty())
        {
            words = "the pack builder refused without words";
        }
        answerFatal(answerOut, words);
        return false;
    }

    // The v2 answer's sections, not an OK line: a client that sent "done" is
    // answered with the packfile section alone, one that did not gets its
    // acknowledgments section first, and every want that was also sent as a
    // have is answered by an empty packfile section. The pack builder resolved
    // every want, so the ids the answer is decided on are held where the
    // callback is asked.
    ols::gitcore::MemoryObjectInfo held(fr.wants);
    const std::string packText(pack.begin(), pack.end());
    if (!ols::gitcore::fetchAnswer(fr, held, packText, answerOut, why))
    {
        return refuse("the fetch answer does not stand: " + why);
    }
    words.clear();
    return true;
}

bool advertisedRefs(const std::string &repoDir, std::vector<std::string> &linesOut)
{
    linesOut.clear();
    const ols::gitcore::Repository repo(repoDir);
    std::vector<ols::gitcore::RefInfo> refs;
    if (!repo.allRefs(refs))
    {
        return false;
    }
    for (const ols::gitcore::RefInfo &info : refs)
    {
        const std::string hex = info.oid.to_hex();
        if (!ols::gitcore::isValidOidHex(hex))
        {
            // A symref that resolves nowhere: the one line the document states
            // for it is HEAD's own, "unborn HEAD symref-target:<target>", and
            // only HEAD carries that form. Any other ref with no id has nothing
            // this line could honestly state.
            if (info.is_symref && info.name == "HEAD" && !info.target.empty())
            {
                linesOut.push_back("unborn HEAD symref-target:" + info.target);
            }
            continue;
        }
        std::string line = hex + " " + info.name;
        if (info.is_symref)
        {
            line += " symref-target:" + info.target;
        }
        linesOut.push_back(line);
    }
    return true;
}

// The v0 capability advertisement of receive-pack, already framed: the marker
// line and its flush, then the ref lines with HEAD first — the capabilities
// after the NUL stand on the first line stated, as git put them there — and
// the flush that ends the advertisement. A HEAD with no id under it is stated
// with the id of nothing, the way an unborn repository is stated; any other
// ref with no id has nothing this line could honestly state.
bool receivePackAdvertisement(const std::string &repoDir,
                              std::vector<std::uint8_t> &bytesOut,
                              std::string &words)
{
    bytesOut.clear();
    words.clear();
    const ols::gitcore::Repository repo(repoDir);
    std::vector<ols::gitcore::RefInfo> refs;
    if (!repo.allRefs(refs))
    {
        words = "the repository's refs do not stand: one of them, or its packed-refs, does not read";
        return false;
    }

    // HEAD first, then the rest as the read left them: git states HEAD's line
    // ahead of every ref, and the capabilities stand on the first line.
    std::vector<ols::gitcore::RefInfo> ordered;
    ordered.reserve(refs.size());
    for (const ols::gitcore::RefInfo &info : refs)
    {
        if (info.name == "HEAD")
        {
            ordered.push_back(info);
        }
    }
    for (const ols::gitcore::RefInfo &info : refs)
    {
        if (info.name != "HEAD")
        {
            ordered.push_back(info);
        }
    }

    std::string err;
    if (!ols::gitcore::pktWrite("# service=git-receive-pack\n", bytesOut, err))
    {
        words = oneLine("the marker line does not fit the framing: " + err);
        bytesOut.clear();
        return false;
    }
    ols::gitcore::pktFlush(bytesOut);

    // The capability list, NUL first. A std::string read from a pointer ends at
    // that NUL and would carry nothing, so the length is taken from the array,
    // which holds it whole. "atomic" is not among them: a refusal under atomic
    // leaves every other ref in place, and this layer's transaction either
    // moves all of them or says no ref moved — advertising the word would let a
    // client trust a promise the push does not keep.
    const char capsText[] =
        "\0report-status delete-refs side-band-64k quiet ofs-delta agent=git/ols-dev";
    const std::string caps(capsText, sizeof(capsText) - 1);
    bool firstLine = true;
    for (const ols::gitcore::RefInfo &info : ordered)
    {
        std::string hex = info.oid.to_hex();
        if (!ols::gitcore::isValidOidHex(hex))
        {
            if (info.name != "HEAD")
            {
                continue;
            }
            hex = kZeroOid;
        }
        std::string line = hex + " " + info.name;
        if (firstLine)
        {
            line += caps;
            firstLine = false;
        }
        line.push_back('\n');
        if (!ols::gitcore::pktWrite(line, bytesOut, err))
        {
            words = oneLine("a ref line does not fit the framing: " + err);
            bytesOut.clear();
            return false;
        }
    }
    ols::gitcore::pktFlush(bytesOut);
    return true;
}

} // namespace git
} // namespace ols
