// tests/gitcore/test_fetch.cpp — the cases that prove fetch.h.
//
// No socket is reached: the seam HttpFetch.h states is stood in here with
// canned answers, framed as raw HTTP text and read back through the unit's
// own parseReply, so the frame rules travel with every case. The repository
// is the fake port the receive cases' StubPort stands beside (test_receive.cpp:
// 79-153): the objects it holds, the refs it moves, and the HEAD it sets are
// all recorded, so each case says what landed and what did not. The pack the
// fetch answer carries is the real fixture pack of this folder, and the answer
// is built with wire.cpp's fetchAnswer — the same writer the serving door
// answers with — so the road from those bytes to objects and refs is the
// road one open-local-services actually walks. Where bytes are spelled out,
// the lengths are counted in the comments, the way the wire cases keep theirs.

#include "gitcore/fetch.h"
#include "gitcore/objects.h"
#include "gitcore/packfile.h"
#include "gitcore/wire.h"
#include "gitcore/wire.cpp" // the writers these cases build with, reached the way test_wire.cpp reaches them; the guard in that file keeps a second landing out
#include "HttpFetch.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using ols::gitcore::AdvertisedRef;
using ols::gitcore::FetchRequest;
using ols::gitcore::FetchTarget;
using ols::gitcore::LsRefsEntry;
using ols::gitcore::LsRefsRequest;
using ols::gitcore::MemoryObjectInfo;
using ols::gitcore::ObjectId;
using ols::gitcore::Packfile;
using ols::gitcore::PackObject;
using ols::gitcore::RecordKind;
using ols::gitcore::RefUpdate;
using ols::gitcore::RepoAccess;

// ids a fixture repo really has, so the bytes here name values git made.
const char *const kF2Commit = "a867ef798b215db19eaf0c7b615c789792fe62b9";
const char *const kF2Tree = "d25250f5f7f874caffa476f3a69161b879d2cbc6";
const char *const kF2Main = "refs/heads/main";
const char *const kF2Url = "http://repo.invalid:8787/git/demo";

// the real fixture pack of this folder and its index, by the names they carry.
const char *const kF2Pack =
    "packed/pack-1acbc3216b21d77e43f3b053a228ff624830366b.pack";
const char *const kF2Idx =
    "packed/pack-1acbc3216b21d77e43f3b053a228ff624830366b.idx";

std::string fdLower(const std::string &text)
{
    std::string out = text;
    for (char &c : out)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::vector<std::uint8_t> fdBytesOf(const std::string &text)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size());
    for (char c : text)
    {
        bytes.push_back(static_cast<std::uint8_t>(c));
    }
    return bytes;
}

std::string fdTextOf(const std::vector<std::uint8_t> &bytes)
{
    std::string text;
    text.reserve(bytes.size());
    for (std::uint8_t b : bytes)
    {
        text.push_back(static_cast<char>(b));
    }
    return text;
}

// One canned HTTP answer as the raw text the reader reads: one chunked frame
// with an extension on its size line and a trailer after the last chunk — so
// the chunk rules are exercised by every chunked answer, not just the plain
// ones — or a declared Content-Length.
std::string fdHttpText(int status, const std::string &contentType, const std::string &payload,
                       bool chunked)
{
    std::string wire = "HTTP/1.1 " + std::to_string(status) + " Answer\r\n";
    wire += "Content-Type: " + contentType + "\r\n";
    if (chunked)
    {
        wire += "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%lx", static_cast<unsigned long>(payload.size()));
        wire += std::string(hex) + ";f2=1\r\n";
        wire += payload;
        wire += "\r\n0\r\nX-F2-Trailer: yes\r\n\r\n";
    }
    else
    {
        wire += "Content-Length: " + std::to_string(payload.size()) + "\r\n";
        wire += "Connection: close\r\n\r\n";
        wire += payload;
    }
    return wire;
}

// The seam, stood in: each answer is chosen by method and by a needle in the
// request body, the way the driver's own commands differ. What was asked is
// kept, so a case can say what shape went out — headers folded to lowercase
// names, the way the driver set them.
struct FdGiven
{
    std::string method;
    std::string needle; // must stand in the request body; empty matches any body
    int status = 200;
    std::string contentType;
    std::string payload;
    bool chunked = false;
};

class FdHttp : public ols::http::HttpFetch
{
public:
    struct FdAsk
    {
        std::string method;
        std::string url;
        std::string body;
        std::map<std::string, std::string> headers;
    };

    std::vector<FdGiven> givens;
    std::vector<FdAsk> asks;

    bool request(const std::string &method, const std::string &url,
                 const ols::http::Headers &headers, const std::string &body,
                 std::uint64_t maxReplyBytes, ols::http::Reply &replyOut,
                 std::string &error) override
    {
        FdAsk ask;
        ask.method = method;
        ask.url = url;
        ask.body = body;
        for (const std::pair<std::string, std::string> &line : headers.lines)
        {
            ask.headers[fdLower(line.first)] = line.second;
        }
        asks.push_back(ask);
        for (const FdGiven &given : givens)
        {
            if (given.method != method)
            {
                continue;
            }
            if (!given.needle.empty() && body.find(given.needle) == std::string::npos)
            {
                continue;
            }
            const std::string wire =
                fdHttpText(given.status, given.contentType, given.payload, given.chunked);
            bool needMore = false;
            if (!ols::http::parseReply(wire, maxReplyBytes, true, replyOut, needMore, error))
            {
                return false;
            }
            return true;
        }
        error = "the canned road holds no answer for " + method + " " + url;
        return false;
    }
};

// The port a fetch moves: the objects it holds (kind word and content), the
// refs it points at, and the HEAD it was set through — each written down, so
// a case can say what landed. writeLoose does the port's own work: the id it
// hands back is the hash of the bytes it was given, which is what makes the
// driver's re-hash check a claim and not a formality.
class FdTarget : public FetchTarget, public RepoAccess
{
public:
    // hex -> (kind word, content); the order of the writes is kept beside it.
    std::map<std::string, std::pair<std::string, std::string>> objects;
    std::vector<std::string> wrote;
    std::map<std::string, std::string> refs; // name -> lowercase hex
    std::size_t commits = 0u;
    std::vector<std::string> headSet; // what setHeadSymref was asked for, in order

    RepoAccess &repo() override { return *this; }

    bool setHeadSymref(const std::string &target) override
    {
        headSet.push_back(target);
        return true;
    }

    bool hasObject(const std::string &oidHex) const override
    {
        return objects.count(fdLower(oidHex)) != 0u;
    }

    bool readObject(const std::string &oidHex, std::string &kindOut,
                    std::string &contentOut) const override
    {
        const std::map<std::string, std::pair<std::string, std::string>>::const_iterator at =
            objects.find(fdLower(oidHex));
        if (at == objects.end())
        {
            return false;
        }
        kindOut = at->second.first;
        contentOut = at->second.second;
        return true;
    }

    bool writeLoose(const std::string &kind, const std::string &content,
                    std::string &oidHex) override
    {
        ols::gitcore::ObjectKind objectKind = ols::gitcore::ObjectKind::Blob;
        std::string why;
        if (!ols::gitcore::kindOfName(kind, objectKind, why))
        {
            return false;
        }
        const std::string raw =
            ols::gitcore::objectHeaderMake(objectKind, content.size()) + content;
        oidHex = ols::gitcore::sha1Hex(raw);
        objects[oidHex] = std::make_pair(kind, content);
        wrote.push_back(oidHex);
        return true;
    }

    bool refValue(const std::string &name, std::string &oidHex) const override
    {
        const std::map<std::string, std::string>::const_iterator at = refs.find(name);
        if (at == refs.end())
        {
            return false;
        }
        oidHex = at->second;
        return true;
    }

    // The all-or-nothing shape of refs.h's commitRefs: every expectation is
    // weighed first, and the null expectation says the ref must not exist.
    bool commitRefs(const std::vector<RefUpdate> &updates) override
    {
        ++commits;
        for (const RefUpdate &update : updates)
        {
            const std::map<std::string, std::string>::const_iterator at = refs.find(update.name);
            if (update.expectedOldOid.has_value())
            {
                if (update.expectedOldOid->is_null())
                {
                    if (at != refs.end())
                    {
                        return false;
                    }
                }
                else if (at == refs.end() || fdLower(at->second) != update.expectedOldOid->hex)
                {
                    return false;
                }
            }
        }
        for (const RefUpdate &update : updates)
        {
            refs[update.name] = update.newOid.hex;
        }
        return true;
    }

    bool deleteRefs(const std::vector<std::string> &names) override
    {
        for (const std::string &name : names)
        {
            refs.erase(name);
        }
        return true;
    }
};

// The fixture pair, whole; false and empty text when the folder does not hold it.
bool fdFixture(std::string &packTextOut, std::string &idxTextOut)
{
    packTextOut = ols_test::read_fixture(kF2Pack);
    idxTextOut = ols_test::read_fixture(kF2Idx);
    return !packTextOut.empty() && !idxTextOut.empty();
}

// The v2 advertisement this door answers with, built by wire.cpp's writer.
std::string fdV2Advertisement()
{
    std::vector<std::uint8_t> out;
    std::string error;
    CHECK(ols::gitcore::v2Advertise(
        std::vector<std::string>{"command=ls-refs fetch\n", "unborn\n", "symrefs\n", "peel\n"},
        out, error));
    return fdTextOf(out);
}

// HEAD pointing through refs/heads/main at tip, and the branch itself.
std::string fdLsRefsAnswer(const std::string &tip)
{
    std::vector<LsRefsEntry> entries(2);
    entries[0].name = "HEAD";
    entries[0].oid = ObjectId(tip);
    entries[0].isSymref = true;
    entries[0].symrefTarget = kF2Main;
    entries[1].name = kF2Main;
    entries[1].oid = ObjectId(tip);
    LsRefsRequest request;
    request.unborn = true;
    request.peel = true;
    request.symrefs = true;
    std::vector<std::uint8_t> out;
    std::string error;
    CHECK(ols::gitcore::lsRefsAnswer(entries, request, out, error));
    return fdTextOf(out);
}

// The v0 advertisement upload-pack states: HEAD first, the capability list
// after the NUL of that first line, the branch under it, the flush.
std::string fdV0Advertisement(const std::string &headOid, const std::string &mainOid)
{
    std::vector<std::uint8_t> out;
    std::string error;
    std::string first = headOid + " HEAD";
    first.push_back('\0'); // the caps stand after the NUL of the first line, where a strlen stop would lose them
    first += "side-band-64k ofs-delta agent=git/ols-test\n";
    CHECK(ols::gitcore::pktWrite(first, out, error));
    CHECK(ols::gitcore::pktWrite(mainOid + " " + kF2Main + "\n", out, error));
    ols::gitcore::pktFlush(out);
    return fdTextOf(out);
}

// The fetch answer, built by the door's own writer: wants done against the
// held set, the pack riding the sideband — or no pack at all, which is what
// a client that already holds every want reads.
std::string fdFetchAnswer(const std::string &want, const std::string &packText)
{
    FetchRequest asked;
    asked.wants.push_back(want);
    if (packText.empty())
    {
        asked.haves.push_back(want); // every want sent as a have: the section rides with no data packets (wire.h:237)
    }
    asked.done = true;
    const MemoryObjectInfo holds(std::vector<std::string>{want});
    std::vector<std::uint8_t> out;
    std::string error;
    CHECK(ols::gitcore::fetchAnswer(asked, holds, packText, out, error));
    return fdTextOf(out);
}

bool fdHoldsAllObjects(Packfile &fixture, const std::vector<PackObject> &listed,
                       FdTarget &target)
{
    for (const PackObject &object : listed)
    {
        std::vector<std::uint8_t> raw;
        if (!fixture.resolveOid(object.oid, raw))
        {
            return false;
        }
        const std::string rawText(raw.begin(), raw.end());
        ols::gitcore::ObjectKind kind = ols::gitcore::ObjectKind::Blob;
        std::uint64_t size = 0ull;
        std::string content;
        std::string why;
        if (!ols::gitcore::objectSplit(rawText, kind, size, content, why))
        {
            return false;
        }
        target.objects[object.oid.hex] =
            std::make_pair(ols::gitcore::kindName(kind), content);
    }
    return true;
}

// every object the fake port holds must hash to the id it stands under
bool fdRehashHolds(const FdTarget &target)
{
    for (const std::pair<const std::string, std::pair<std::string, std::string>> &held :
         target.objects)
    {
        ols::gitcore::ObjectKind kind = ols::gitcore::ObjectKind::Blob;
        std::string why;
        if (!ols::gitcore::kindOfName(held.second.first, kind, why))
        {
            return false;
        }
        const std::string raw =
            ols::gitcore::objectHeaderMake(kind, held.second.second.size()) +
            held.second.second;
        if (ols::gitcore::sha1Hex(raw) != held.first)
        {
            return false;
        }
    }
    return true;
}

} // anonymous namespace

// ------------------------------------------------------------------ lsRemote

TEST(fetch_lsremote_v2_reads_advertisement_and_ls_refs)
{
    FdHttp http;
    http.givens.push_back({"GET", "", 200, "application/x-git-upload-pack-advertisement",
                           fdV2Advertisement(), false});
    http.givens.push_back(
        {"POST", "command=ls-refs", 200, "application/x-git-upload-pack-result",
         fdLsRefsAnswer(std::string(kF2Commit)), false});

    std::vector<AdvertisedRef> refs;
    std::string words;
    CHECK(ols::gitcore::lsRemote(http, kF2Url, std::string(), refs, words));
    if (refs.size() != 2u)
    {
        return; // the element claims below stand on the count above
    }
    CHECK(refs[0].name == std::string("HEAD"));
    CHECK(refs[0].oid == kF2Commit);
    CHECK(!refs[0].unborn);
    CHECK(refs[0].symrefTarget == std::string(kF2Main));
    CHECK(refs[1].name == std::string(kF2Main));
    CHECK(refs[1].oid == kF2Commit);

    // the two asks: the advertisement path, then the command to the same service
    CHECK(http.asks.size() == 2u);
    if (http.asks.size() != 2u)
    {
        return;
    }
    CHECK(http.asks[0].method == std::string("GET"));
    CHECK(http.asks[0].url == std::string(kF2Url) + "/info/refs?service=git-upload-pack");
    CHECK(http.asks[1].method == std::string("POST"));
    CHECK(http.asks[1].url == std::string(kF2Url) + "/git-upload-pack");
    CHECK(http.asks[1].headers["content-type"] == "application/x-git-upload-pack-request");
    CHECK(http.asks[1].headers["accept"] == "application/x-git-upload-pack-result");
    CHECK(http.asks[1].headers["git-protocol"] == "version=2");
    // no key given, no X-Api-Key header on any ask
    CHECK(http.asks[0].headers.count("x-api-key") == 0u);
    CHECK(http.asks[1].headers.count("x-api-key") == 0u);
}

TEST(fetch_lsremote_v0_reads_the_advertisement_itself)
{
    FdHttp http;
    http.givens.push_back({"GET", "", 200, "application/x-git-upload-pack-advertisement",
                           fdV0Advertisement(std::string(kF2Commit), std::string(kF2Tree)),
                           false});

    std::vector<AdvertisedRef> refs;
    std::string words;
    CHECK(ols::gitcore::lsRemote(http, kF2Url, std::string("k-9"), refs, words));
    CHECK(refs.size() == 2u);
    if (refs.size() != 2u)
    {
        return;
    }
    CHECK(refs[0].name == std::string("HEAD"));
    CHECK(refs[0].oid == kF2Commit);
    CHECK(refs[1].name == std::string(kF2Main));
    CHECK(refs[1].oid == kF2Tree);
    // v0 needs no second ask: the advertisement carried its refs — and the key
    // the caller handed travels with the one that did (PeerService.cpp:241 shape)
    CHECK(http.asks.size() == 1u);
    if (http.asks.size() != 1u)
    {
        return;
    }
    CHECK(http.asks[0].headers["x-api-key"] == std::string("k-9"));
}

// ----------------------------------------------------------------- fetchRepo

TEST(fetch_repo_lands_the_fixture_pack_and_moves_the_refs)
{
    std::string packText;
    std::string idxText;
    CHECK(fdFixture(packText, idxText));
    if (packText.empty() || idxText.empty())
    {
        return; // the fixture is absent; nothing below stands
    }
    const std::vector<std::uint8_t> packBytes = fdBytesOf(packText);
    const std::vector<std::uint8_t> idxBytes = fdBytesOf(idxText);
    Packfile fixture;
    CHECK(fixture.open(packBytes, idxBytes));
    std::vector<PackObject> listed;
    CHECK(fixture.list(listed));
    CHECK(!listed.empty());
    std::string tip;
    for (const PackObject &object : listed)
    {
        if (object.kind == RecordKind::Commit)
        {
            tip = object.oid.hex;
            break;
        }
    }
    CHECK(!tip.empty());
    if (tip.empty())
    {
        return; // the want below stands on a real commit
    }

    FdHttp http;
    http.givens.push_back({"GET", "", 200, "application/x-git-upload-pack-advertisement",
                           fdV2Advertisement(), false});
    http.givens.push_back(
        {"POST", "command=ls-refs", 200, "application/x-git-upload-pack-result",
         fdLsRefsAnswer(tip), false});
    http.givens.push_back({"POST", "command=fetch", 200, "application/x-git-upload-pack-result",
                           fdFetchAnswer(tip, packText), true});

    FdTarget target;
    std::vector<std::string> moved;
    std::string words;
    CHECK(ols::gitcore::fetchRepo(target, http, kF2Url, std::vector<std::string>{kF2Main},
                                  std::string("k-1"), moved, words));
    CHECK(words.empty());

    // every object of the real fixture pack stands in the port, each hashing
    // to the id it wears, written once
    CHECK(target.wrote.size() == listed.size());
    for (const PackObject &object : listed)
    {
        CHECK(target.objects.count(object.oid.hex) != 0u);
    }
    CHECK(fdRehashHolds(target));

    // one transaction, the ref at the advertised oid, HEAD to the branch
    CHECK(target.commits == 1u);
    CHECK(moved.size() == 1u);
    CHECK(target.refs.count(kF2Main) != 0u);
    if (!target.refs.empty())
    {
        CHECK(target.refs[kF2Main] == tip);
    }
    CHECK(target.headSet.size() == 1u);
    if (!target.headSet.empty())
    {
        CHECK(target.headSet[0] == std::string(kF2Main));
    }

    // the fetch command carried the want and the done
    CHECK(http.asks.size() == 3u);
    if (http.asks.size() == 3u)
    {
        CHECK(http.asks[2].body.find("command=fetch") != std::string::npos);
        CHECK(http.asks[2].body.find("want " + tip) != std::string::npos);
        CHECK(http.asks[2].body.find("done") != std::string::npos);
        // the key travels with every ask (PeerService.cpp:241 shape)
        CHECK(http.asks[0].headers["x-api-key"] == std::string("k-1"));
        CHECK(http.asks[1].headers["x-api-key"] == std::string("k-1"));
        CHECK(http.asks[2].headers["x-api-key"] == std::string("k-1"));
    }
}

TEST(fetch_repo_empty_packfile_section_moves_the_refs_alone)
{
    std::string packText;
    std::string idxText;
    CHECK(fdFixture(packText, idxText));
    if (packText.empty() || idxText.empty())
    {
        return;
    }
    Packfile fixture;
    CHECK(fixture.open(fdBytesOf(packText), fdBytesOf(idxText)));
    std::vector<PackObject> listed;
    CHECK(fixture.list(listed));
    CHECK(!listed.empty());
    std::string tip;
    for (const PackObject &object : listed)
    {
        if (object.kind == RecordKind::Commit)
        {
            tip = object.oid.hex;
            break;
        }
    }
    CHECK(!tip.empty());
    if (tip.empty())
    {
        return;
    }

    // the port already holds every object of the pack — the fetch brings none
    FdTarget target;
    CHECK(fdHoldsAllObjects(fixture, listed, target));
    target.wrote.clear();

    FdHttp http;
    http.givens.push_back({"GET", "", 200, "application/x-git-upload-pack-advertisement",
                           fdV2Advertisement(), false});
    http.givens.push_back(
        {"POST", "command=ls-refs", 200, "application/x-git-upload-pack-result",
         fdLsRefsAnswer(tip), false});
    http.givens.push_back({"POST", "command=fetch", 200, "application/x-git-upload-pack-result",
                           fdFetchAnswer(tip, std::string()), true});

    std::vector<std::string> moved;
    std::string words;
    CHECK(ols::gitcore::fetchRepo(target, http, kF2Url, std::vector<std::string>{kF2Main},
                                  std::string(), moved, words));
    CHECK(words.empty());
    CHECK(target.wrote.empty()); // nothing written: the answer carried no pack
    CHECK(target.commits == 1u); // and the refs still moved, honestly
    CHECK(moved.size() == 1u);
    if (!target.refs.empty())
    {
        CHECK(target.refs[kF2Main] == tip);
    }
    CHECK(target.headSet.size() == 1u);
}

TEST(fetch_repo_fatal_on_channel_three_writes_nothing)
{
    // the pack section opens, one channel-1 frame of a real pack arrives, and
    // channel 3 ends the answer mid-pack: the client reads a fatal, and a
    // fatal means there is no pack to expect.
    std::string packText;
    std::string idxText;
    CHECK(fdFixture(packText, idxText));
    if (packText.empty())
    {
        return;
    }
    std::vector<std::uint8_t> framed;
    std::string error;
    CHECK(ols::gitcore::pktWrite("packfile\n", framed, error));
    std::string first;
    first.push_back(static_cast<char>(1));
    first += packText.substr(0, 40u); // the header and the start of a record
    CHECK(ols::gitcore::pktWrite(first, framed, error));
    std::string last;
    last.push_back(static_cast<char>(3));
    last += "the pack went bad on the way";
    CHECK(ols::gitcore::pktWrite(last, framed, error));
    ols::gitcore::pktFlush(framed);

    FdHttp http;
    http.givens.push_back({"GET", "", 200, "application/x-git-upload-pack-advertisement",
                           fdV2Advertisement(), false});
    http.givens.push_back(
        {"POST", "command=ls-refs", 200, "application/x-git-upload-pack-result",
         fdLsRefsAnswer(std::string(kF2Commit)), false});
    http.givens.push_back({"POST", "command=fetch", 200, "application/x-git-upload-pack-result",
                           fdTextOf(framed), true});

    FdTarget target;
    std::vector<std::string> moved;
    std::string words;
    CHECK(!ols::gitcore::fetchRepo(target, http, kF2Url, std::vector<std::string>{kF2Main},
                                   std::string(), moved, words));
    CHECK(words.find("the pack went bad on the way") != std::string::npos);
    CHECK(target.wrote.empty());
    CHECK(target.commits == 0u);
    CHECK(target.refs.empty());
    CHECK(target.headSet.empty());
    CHECK(moved.empty());
}

TEST(fetch_repo_https_refuses_naming_tls)
{
    FdHttp http;
    FdTarget target;
    std::vector<std::string> moved;
    std::vector<AdvertisedRef> refs;
    std::string words;
    const std::string httpsUrl = "https://repo.invalid/git/demo";
    CHECK(!ols::gitcore::lsRemote(http, httpsUrl, std::string(), refs, words));
    CHECK(words.find("TLS") != std::string::npos);
    words.clear();
    CHECK(!ols::gitcore::fetchRepo(target, http, httpsUrl,
                                   std::vector<std::string>{kF2Main}, std::string(), moved,
                                   words));
    CHECK(words.find("TLS") != std::string::npos);
    CHECK(http.asks.empty()); // refused before any ask left the house
    CHECK(target.wrote.empty());
    CHECK(target.commits == 0u);
}

// ---------------------------------------------------------------- HttpFetch.h

TEST(httpfetch_reply_reader_keeps_the_frame_rules_and_the_cap)
{
    // one chunked answer through the reader: the extension on the size line is
    // read and set aside, the trailer after the last chunk is read and left
    // aside, and the body comes back whole.
    const std::string payload = "abcdefghij";
    const std::string wire = fdHttpText(206, "application/x-git-upload-pack-result", payload,
                                        true);
    ols::http::Reply reply;
    bool needMore = false;
    std::string error;
    CHECK(ols::http::parseReply(wire, 1024ull, true, reply, needMore, error));
    CHECK(reply.status == 206);
    CHECK(reply.body == payload);
    // header names are matched case-insensitively
    std::string found;
    CHECK(reply.headers.find("content-TYPE", found));
    CHECK(found == "application/x-git-upload-pack-result");

    // a declared length past the cap is refused before the body is held
    const std::string bigHead =
        "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\nConnection: close\r\n\r\nshort";
    needMore = false;
    ols::http::Reply refused;
    CHECK(!ols::http::parseReply(bigHead, 10ull, true, refused, needMore, error));
    CHECK(!needMore);
    CHECK(error.find("1000") != std::string::npos);
    CHECK(refused.body.empty());

    // Connection: close makes the stream's end the body's end — and until the
    // stream has ended, there is no answer yet
    const std::string toClose = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nthe rest of it";
    needMore = false;
    ols::http::Reply rest;
    CHECK(!ols::http::parseReply(toClose, 1024ull, false, rest, needMore, error));
    CHECK(needMore);
    needMore = false;
    CHECK(ols::http::parseReply(toClose, 1024ull, true, rest, needMore, error));
    CHECK(rest.body == std::string("the rest of it"));

    // a head that has not ended is a shortage, not an answer
    needMore = false;
    ols::http::Reply waiting;
    CHECK(!ols::http::parseReply(std::string("HTTP/1.1 200 OK\r\n"), 1024ull, false, waiting,
                                 needMore, error));
    CHECK(needMore);

    // the address rules: https names TLS, other schemes are refused outright,
    // and a bare host asks for the root
    std::string host;
    std::uint16_t port = 0u;
    std::string target;
    CHECK(!ols::http::splitUrl("https://repo.invalid/x", host, port, target, error));
    CHECK(error.find("TLS") != std::string::npos);
    CHECK(!ols::http::splitUrl("ftp://repo.invalid/x", host, port, target, error));
    CHECK(ols::http::splitUrl("http://repo.invalid:8787/a/b?q=1", host, port, target, error));
    CHECK(host == std::string("repo.invalid"));
    CHECK(port == 8787u);
    CHECK(target == std::string("/a/b?q=1"));
    CHECK(ols::http::splitUrl("http://repo.invalid", host, port, target, error));
    CHECK(port == 80u);
    CHECK(target == std::string("/"));
    CHECK(!ols::http::splitUrl("http://repo.invalid:70000/x", host, port, target, error));
}
