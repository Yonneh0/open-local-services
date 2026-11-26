// src/gitcore/fetch.cpp — the ground fetch.h stands on.
//
// The definitions stand external and the file is named as a source beside the
// other units of this shape (receive.cpp:1-17, fetch_wire.cpp:1-9); nothing
// here is inline and no door unity-includes it. The wire unit's ground rides
// in through its own include below - the same road GitSmartHttp.cpp:27 walks,
// guarded so a host that carries both lands it once (wire.cpp:9-23). Reading
// only wire.h would ask this object for symbols no named source emits.
#include "fetch.h"
#include "gitcore/fetch_wire.h"
#include "gitcore/objects.h"
#include "gitcore/packfile.h"
#include "gitcore/wire.cpp"

#include <cstddef>
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

            // The service address the advertisement and the commands share, the
            // way the smart-HTTP doors stand them (OpenLocalServices.cpp:3672,
            // OpenLocalServices.cpp:3762).
            std::string advertisementUrl(const std::string &url)
            {
                return url + "/info/refs?service=git-upload-pack";
            }
            std::string serviceUrl(const std::string &url)
            {
                return url + "/git-upload-pack";
            }

            bool hasPrefix(const std::string &text, const std::string &prefix)
            {
                return text.size() >= prefix.size() &&
                       text.compare(0u, prefix.size(), prefix) == 0;
            }

            // The request headers every ask carries: the two git names, and the
            // key when the caller holds one — the PeerService.cpp:241 shape, the
            // same header one open-local-services answers.
            ols::http::Headers askHeaders(const std::string &apiKey, bool forCommand, bool v2)
            {
                ols::http::Headers headers;
                if (forCommand)
                {
                    headers.set("Content-Type", "application/x-git-upload-pack-request");
                    headers.set("Accept", "application/x-git-upload-pack-result");
                    if (v2)
                    {
                        headers.set("Git-Protocol", "version=2");
                    }
                }
                else
                {
                    headers.set("Accept", "application/x-git-upload-pack-advertisement");
                }
                if (!apiKey.empty())
                {
                    headers.set("X-Api-Key", apiKey);
                }
                return headers;
            }

            std::vector<std::uint8_t> bytesOf(const std::string &text)
            {
                std::vector<std::uint8_t> bytes;
                bytes.reserve(text.size());
                for (char c : text)
                {
                    bytes.push_back(static_cast<std::uint8_t>(c));
                }
                return bytes;
            }

            bool statusStands(int status, std::string &error)
            {
                if (status == 200)
                {
                    return true;
                }
                error = "the door answered with status " + std::to_string(status);
                return false;
            }

            // Whether one capability line of a v2 advertisement offers the named
            // command: the "command=" line's value is a space-separated list.
            bool advertises(const std::vector<V2Capability> &caps, const std::string &command)
            {
                for (const V2Capability &cap : caps)
                {
                    if (cap.key != "command")
                    {
                        continue;
                    }
                    std::size_t at = 0u;
                    while (at < cap.value.size())
                    {
                        while (at < cap.value.size() && cap.value[at] == ' ')
                        {
                            ++at;
                        }
                        const std::size_t start = at;
                        while (at < cap.value.size() && cap.value[at] != ' ')
                        {
                            ++at;
                        }
                        if (cap.value.compare(start, at - start, command) == 0)
                        {
                            return true;
                        }
                    }
                }
                return false;
            }

            // The advertisement of one url, read on either dialect. A v2 answer
            // is followed by one ls-refs command so its refs — with the unborn
            // line and the symref target — come back whole; a v0 advertisement
            // carries its refs itself.
            bool fetchAdvertisement(ols::http::HttpFetch &http, const std::string &url,
                                    const std::string &apiKey, bool wantFetch,
                                    bool &v2Out, std::vector<AdvertisedRef> &refsOut,
                                    std::string &error)
            {
                ols::http::Reply reply;
                if (!ols::http::get(http, advertisementUrl(url), askHeaders(apiKey, false, false),
                                    kMaxFetchAnswerBytes, reply, error))
                {
                    return false;
                }
                if (!statusStands(reply.status, error))
                {
                    return false;
                }
                const std::vector<std::uint8_t> bytes = bytesOf(reply.body);

                std::vector<V2Capability> caps;
                std::string v2Error;
                if (fetchReadV2Advertisement(bytes.data(), bytes.size(), caps, v2Error))
                {
                    v2Out = true;
                    if (!advertises(caps, "ls-refs"))
                    {
                        error = "the v2 advertisement names no ls-refs command, so its refs cannot be asked";
                        return false;
                    }
                    if (wantFetch && !advertises(caps, "fetch"))
                    {
                        error = "the v2 advertisement names no fetch command";
                        return false;
                    }
                    std::vector<std::uint8_t> request;
                    std::string built;
                    if (!v2BuildRequest("ls-refs", std::vector<std::string>{"peel\n", "symrefs\n", "unborn\n"},
                                        std::vector<std::string>(), request, built))
                    {
                        error = "the ls-refs command could not be framed: " + built;
                        return false;
                    }
                    ols::http::Reply answer;
                    const std::string body(request.begin(), request.end());
                    if (!ols::http::post(http, serviceUrl(url), askHeaders(apiKey, true, true), body,
                                         kMaxFetchAnswerBytes, answer, error))
                    {
                        return false;
                    }
                    if (!statusStands(answer.status, error))
                    {
                        return false;
                    }
                    const std::vector<std::uint8_t> answerBytes = bytesOf(answer.body);
                    std::vector<LsRefsEntry> entries;
                    std::string readError;
                    if (!fetchReadLsRefsAnswer(answerBytes.data(), answerBytes.size(), entries, readError))
                    {
                        error = "the ls-refs answer does not read: " + readError;
                        return false;
                    }
                    refsOut.clear();
                    for (const LsRefsEntry &entry : entries)
                    {
                        AdvertisedRef ref;
                        ref.name = entry.name;
                        ref.oid = entry.oid.hex;
                        ref.unborn = entry.oid.is_null();
                        ref.symrefTarget = entry.symrefTarget;
                        refsOut.push_back(ref);
                    }
                    return true;
                }

                v2Out = false;
                V0Advertisement advertisement;
                std::string readError;
                if (!fetchReadV0Advertisement(bytes.data(), bytes.size(), advertisement, readError))
                {
                    error = "the advertisement reads neither v2 nor v0: " + readError;
                    return false;
                }
                refsOut.clear();
                for (const V0RefEntry &entry : advertisement.entries)
                {
                    AdvertisedRef ref;
                    ref.name = entry.name;
                    ref.oid = entry.oid.hex;
                    ref.unborn = entry.unborn;
                    refsOut.push_back(ref);
                }
                return true;
            }

            // ---------------------------------------------------- the pack, stood up
            //
            // indexBarePack of the receive door (receive.cpp:581) stands inside that
            // unit's anonymous namespace, so it is not this file's to call; what it
            // calls the same objects do — the record head, the deflated body through
            // the object layer's span reader, the delta rounds, and the resolution
            // through the repository for a thin pack's borrowed base — stand here in
            // that unit's shape, mirrored from receive.cpp:189-856. The end is this
            // road's own: every resolved object goes through the port's writeLoose
            // with its bytes re-hashed, the pattern receive.cpp:1642-1655 keeps.

            struct StoodRecord
            {
                std::size_t offset = 0u;
                std::size_t dataEnd = 0u;
                RecordKind kind = RecordKind::Blob;
                std::vector<std::uint8_t> body;
                std::uint64_t back = 0ull;
                std::string baseOid;
            };

            // The least a record can stand for — the receive unit's number for the
            // same bound (receive.cpp:63).
            constexpr std::size_t kMinStoodRecordBytes = 8u;

            // The pack's two varints: 7 bits to a byte low parts first, and the ofs
            // offset with its place of 2^7 added for every byte past the first
            // (receive.cpp:189-236).
            bool readStoodSizeVarint(const std::vector<std::uint8_t> &bytes, std::size_t &pos,
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

            bool readStoodOfsOffset(const std::vector<std::uint8_t> &bytes, std::size_t &pos,
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

            // One record header: kind, declared size, and the base it names — a back
            // offset for an ofs delta, a lowercase id for a ref delta. pos leaves at
            // the deflated body (receive.cpp:241-296).
            bool readStoodRecordHead(const std::vector<std::uint8_t> &pack, std::size_t &pos,
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
                    if (!readStoodOfsOffset(pack, pos, back))
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

            // The record's deflated body: one stream read over the bytes that remain,
            // its end stated by the stream's own structure through the object layer's
            // span reader, with the receive unit's window and its declared+2 escape
            // for a final stored block whose two adler bytes the object layer counts
            // among the payload (receive.cpp:393-445).
            bool findStoodRecordBody(const std::vector<std::uint8_t> &pack, std::size_t dataStart,
                                     std::size_t fileEnd, std::uint64_t declared,
                                     std::size_t &dataEnd, std::vector<std::uint8_t> &body)
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
                        if (out.size() == declared + 2ull && declared >= 1ull)
                        {
                            out.resize(static_cast<std::size_t>(declared));
                            body.assign(out.begin(), out.end());
                            dataEnd = dataStart + consumed;
                            return true;
                        }
                        return false;
                    }
                    if (stop >= fileEnd)
                    {
                        return false;
                    }
                }
            }

            // git's delta instructions over a base whose content is in hand, with the
            // bounds checked before any byte is made (receive.cpp:447-541).
            bool applyStoodDelta(const std::vector<std::uint8_t> &baseContent,
                                 const std::vector<std::uint8_t> &delta,
                                 std::vector<std::uint8_t> &out)
            {
                std::size_t pos = 0u;
                std::uint64_t baseDeclared = 0ull;
                std::uint64_t target = 0ull;
                if (!readStoodSizeVarint(delta, pos, baseDeclared) ||
                    !readStoodSizeVarint(delta, pos, target))
                {
                    return false;
                }
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
                                off |= static_cast<std::uint64_t>(delta[pos])
                                       << static_cast<unsigned>(i * 8);
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
                                len |= static_cast<std::uint64_t>(delta[pos])
                                       << static_cast<unsigned>(i * 8);
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

            bool stoodObjectKind(RecordKind kind, ObjectKind &out)
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

            // The record kind the port's kind word names (receive.cpp:102-125).
            bool stoodKindFromWord(const std::string &word, RecordKind &out)
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

            // Stand the pack up and write it: the records are scanned and each
            // resolved through its delta chain in rounds, an ofs base standing at an
            // earlier offset and a ref base met by the id it names — behind both,
            // when the pack carries no more, the repository answers a thin pack's
            // borrowed base and the answer is checked against the id (receive.cpp:
            // 663-852). Nothing is written while any record stands unresolved; then
            // every object goes through the port's writeLoose and the id it comes
            // back with must be the id its bytes hash to (receive.cpp:1642-1655).
            bool standUpPack(RepoAccess &port, const std::vector<std::uint8_t> &pack,
                             std::vector<std::string> &writtenOut, std::string &error)
            {
                writtenOut.clear();
                if (pack.size() < 12u + 20u)
                {
                    error = "the answer carries no pack header and trailer";
                    return false;
                }
                if (!(pack[0] == 'P' && pack[1] == 'A' && pack[2] == 'C' && pack[3] == 'K'))
                {
                    error = "the pack does not begin with a pack header";
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
                const std::size_t entriesCanStand =
                    (fileEnd - 12u) / kMinStoodRecordBytes;
                if (entries > entriesCanStand)
                {
                    error = "the pack header names " + std::to_string(entries) +
                            " records when the bytes hold room for " +
                            std::to_string(entriesCanStand) + " at most";
                    return false;
                }

                std::vector<StoodRecord> records;
                records.reserve(entries);
                std::map<std::size_t, std::size_t> atOffset;
                std::size_t pos = 12u;
                for (std::size_t i = 0u; i < entries; ++i)
                {
                    StoodRecord record;
                    record.offset = pos;
                    std::uint64_t declared = 0ull;
                    if (!readStoodRecordHead(pack, pos, record.kind, declared, record.back,
                                             record.baseOid))
                    {
                        error = "the record at " + std::to_string(record.offset) +
                                " does not state a kind and a size this layer reads";
                        return false;
                    }
                    if (!findStoodRecordBody(pack, pos, fileEnd, declared, record.dataEnd,
                                             record.body))
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
                std::vector<std::size_t> chainDepth(records.size(), 0u);
                std::map<std::string, std::size_t> resolvedByOid;

                struct RepoBase
                {
                    bool held = false;
                    RecordKind kind = RecordKind::Blob;
                    std::vector<std::uint8_t> content;
                };
                std::map<std::string, RepoBase> borrowed;
                bool repoAllowed = false;
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
                        const StoodRecord &record = records[i];
                        std::vector<std::uint8_t> made;
                        RecordKind kind = record.kind;
                        std::size_t linkDepth = 0u;
                        if (record.kind == RecordKind::OfsDelta ||
                            record.kind == RecordKind::RefDelta)
                        {
                            const std::vector<std::uint8_t> *baseContent = nullptr;
                            RecordKind baseKind = RecordKind::Blob;
                            std::size_t baseDepth = 1u;
                            if (record.kind == RecordKind::OfsDelta)
                            {
                                if (record.back > static_cast<std::uint64_t>(record.offset))
                                {
                                    error = "the ofs delta at " + std::to_string(record.offset) +
                                            " names a base before the pack begins";
                                    return false;
                                }
                                const std::map<std::size_t, std::size_t>::const_iterator at =
                                    atOffset.find(record.offset -
                                                  static_cast<std::size_t>(record.back));
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
                                    std::map<std::string, RepoBase>::iterator known =
                                        borrowed.find(record.baseOid);
                                    if (known == borrowed.end())
                                    {
                                        RepoBase base;
                                        std::string kindWord;
                                        std::string contentText;
                                        base.held = port.readObject(record.baseOid, kindWord,
                                                                     contentText);
                                        if (base.held)
                                        {
                                            ObjectKind baseObjectKind = ObjectKind::Blob;
                                            if (!stoodKindFromWord(kindWord, base.kind))
                                            {
                                                error = "the repository's object " +
                                                        record.baseOid +
                                                        " is of no kind this layer reads";
                                                return false;
                                            }
                                            if (!stoodObjectKind(base.kind, baseObjectKind))
                                            {
                                                error = "the repository's object " +
                                                        record.baseOid +
                                                        " is of no kind this layer reads";
                                                return false;
                                            }
                                            std::string rawBase = objectHeaderMake(
                                                baseObjectKind, contentText.size());
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
                                            base.content.assign(contentText.begin(),
                                                                contentText.end());
                                        }
                                        known = borrowed.insert(std::make_pair(record.baseOid,
                                                                              std::move(base)))
                                                    .first;
                                    }
                                    if (!known->second.held)
                                    {
                                        continue; // neither the pack nor the repository holds it
                                    }
                                    baseContent = &known->second.content;
                                    baseKind = known->second.kind;
                                }
                            }
                            kind = baseKind;
                            linkDepth = baseDepth;
                            if (baseDepth > kMaxDeltaChain)
                            {
                                error = std::string("a delta chain in the pack runs past the ") +
                                        std::to_string(kMaxDeltaChain) +
                                        " links this layer walks";
                                return false;
                            }
                            if (!applyStoodDelta(*baseContent, record.body, made))
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
                        if (!stoodObjectKind(kind, objectKind))
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
                            error = "the resolved bytes of the record at " +
                                    std::to_string(record.offset) + " do not hash to an id";
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
                if (!roundsClosed)
                {
                    error = std::string("a delta chain in the pack runs past the ") +
                            std::to_string(kMaxDeltaChain) + " links this layer walks";
                    return false;
                }
                for (std::size_t i = 0u; i < records.size(); ++i)
                {
                    if (done[i] == 0)
                    {
                        error = "a delta in the pack names a base neither the pack nor the "
                                "repository holds";
                        return false;
                    }
                }

                // The write, every object of it: the port's answer to writeLoose is
                // the id of the bytes it wrote, and it must be the id these bytes
                // hash to (receive.cpp:1642-1655).
                for (std::size_t i = 0u; i < records.size(); ++i)
                {
                    ObjectKind objectKind = ObjectKind::Blob;
                    if (!stoodObjectKind(kindOf[i], objectKind))
                    {
                        error = "a resolved record carries a kind no object wears";
                        return false;
                    }
                    const std::string contentText(content[i].begin(), content[i].end());
                    std::string raw = objectHeaderMake(objectKind, content[i].size());
                    raw.append(contentText);
                    const std::string hashed = sha1Hex(raw);
                    std::string written;
                    if (!port.writeLoose(kindName(objectKind), contentText, written))
                    {
                        error = "the object " + hashed +
                                " could not be written where the repository holds objects";
                        return false;
                    }
                    if (detail::lower_hex(written) != hashed)
                    {
                        error = "the bytes the port wrote are not the object the pack carries: " +
                                hashed + " came back as " + detail::lower_hex(written);
                        return false;
                    }
                    writtenOut.push_back(hashed);
                }
                return true;
            }

        } // anonymous namespace

        // ------------------------------------------------------------------ lsRemote

        bool lsRemote(ols::http::HttpFetch &http, const std::string &url,
                      const std::string &apiKey, std::vector<AdvertisedRef> &refsOut,
                      std::string &outWords)
        {
            refsOut.clear();
            outWords.clear();
            // The address is weighed for the road it rides (E2): an ssh ask
            // belongs to the transport handed here, and splitUrl - which
            // speaks http only - must not be the last thing it ever says. An
            // ssh:// form with no path at all still refuses here, in splitUrl's
            // own words; the scp-like form carries its path with no slash to
            // miss. Everything else keeps walking the road it walked.
            if (url.compare(0u, 6u, "ssh://") != 0 || url.find('/', 6u) == std::string::npos)
            {
                std::string targetUrl;
                std::uint16_t port = 0u;
                std::string path;
                if (!ols::http::splitUrl(url, targetUrl, port, path, outWords))
                {
                    return false; // the words name the refusal, TLS among them
                }
            }
            bool v2 = false;
            return fetchAdvertisement(http, url, apiKey, false, v2, refsOut, outWords);
        }

        // ----------------------------------------------------------------- fetchRepo

        bool fetchRepo(FetchTarget &target, ols::http::HttpFetch &http, const std::string &url,
                       const std::vector<std::string> &refsToFetch, const std::string &apiKey,
                       std::vector<std::string> &movedOut, std::string &outWords)
        {
            movedOut.clear();
            outWords.clear();
            // The same road reading lsRemote stands on (E2): ssh asks ride the
            // handed transport and skip http's weighing; an ssh:// form with no
            // path at all still refuses here, in splitUrl's own words.
            if (url.compare(0u, 6u, "ssh://") != 0 || url.find('/', 6u) == std::string::npos)
            {
                std::string host;
                std::uint16_t urlPort = 0u;
                std::string path;
                if (!ols::http::splitUrl(url, host, urlPort, path, outWords))
                {
                    return false; // the words name the refusal, TLS among them
                }
            }
            if (refsToFetch.empty())
            {
                outWords = "the fetch names no ref";
                return false;
            }
            RepoAccess &port = target.repo();

            bool v2 = false;
            std::vector<AdvertisedRef> advertised;
            if (!fetchAdvertisement(http, url, apiKey, true, v2, advertised, outWords))
            {
                return false;
            }

            // The wants: every named ref, by the oid the advertisement states.
            std::vector<std::pair<std::string, std::string>> wanted; // name -> want oid
            std::set<std::string> wantOids;
            for (const std::string &name : refsToFetch)
            {
                std::string asked = name;
                if (asked == "HEAD")
                {
                    // HEAD travels to where the advertisement says it points.
                    bool found = false;
                    for (const AdvertisedRef &ref : advertised)
                    {
                        if (ref.name == "HEAD" && !ref.symrefTarget.empty())
                        {
                            asked = ref.symrefTarget;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        outWords = "the fetch asks HEAD and the advertisement does not say where it points";
                        return false;
                    }
                }
                const AdvertisedRef *carried = nullptr;
                for (const AdvertisedRef &ref : advertised)
                {
                    if (ref.name == asked)
                    {
                        carried = &ref;
                        break;
                    }
                }
                if (carried == nullptr)
                {
                    outWords = "the advertisement does not carry " + asked;
                    return false;
                }
                if (carried->unborn)
                {
                    outWords = asked + " is stated with the id of nothing, so there is nothing to fetch for it";
                    return false;
                }
                wanted.push_back(std::make_pair(asked, carried->oid));
                wantOids.insert(carried->oid);
            }

            // The haves: the advertised tips the port already holds. The port
            // answers for an id, and the advertisement is the list of candidates.
            std::vector<std::string> haves;
            std::set<std::string> haveOids;
            for (const AdvertisedRef &ref : advertised)
            {
                if (ref.unborn || wantOids.count(ref.oid) != 0u || haveOids.count(ref.oid) != 0u)
                {
                    continue;
                }
                if (port.hasObject(ref.oid))
                {
                    haves.push_back(ref.oid);
                    haveOids.insert(ref.oid);
                }
            }

            // The request, on the dialect the advertisement spoke.
            std::string body;
            if (v2)
            {
                std::vector<std::string> arguments;
                for (const std::pair<std::string, std::string> &entry : wanted)
                {
                    arguments.push_back("want " + entry.second + "\n");
                }
                for (const std::string &have : haves)
                {
                    arguments.push_back("have " + have + "\n");
                }
                arguments.push_back("done\n");
                std::vector<std::uint8_t> framed;
                std::string words;
                if (!v2BuildRequest("fetch", std::vector<std::string>{"object-format=sha1\n"},
                                    arguments, framed, words))
                {
                    outWords = "the fetch command could not be framed: " + words;
                    return false;
                }
                body.assign(framed.begin(), framed.end());
            }
            else
            {
                V0UploadPackRequest request;
                for (const std::pair<std::string, std::string> &entry : wanted)
                {
                    request.wants.push_back(entry.second);
                }
                request.haves = haves;
                request.caps.push_back("side-band-64k");
                request.caps.push_back("ofs-delta");
                request.caps.push_back("agent=open-local-services/gitcore");
                request.done = true;
                std::vector<std::uint8_t> framed;
                std::string words;
                if (!v0BuildUploadPackRequest(request, framed, words))
                {
                    outWords = "the upload-pack request could not be framed: " + words;
                    return false;
                }
                body.assign(framed.begin(), framed.end());
            }

            ols::http::Reply answer;
            if (!ols::http::post(http, serviceUrl(url), askHeaders(apiKey, true, v2), body,
                                 kMaxFetchAnswerBytes, answer, outWords))
            {
                return false;
            }
            if (!statusStands(answer.status, outWords))
            {
                return false;
            }

            const std::vector<std::uint8_t> answerBytes = bytesOf(answer.body);
            FetchAnswerSections sections;
            std::string readError;
            if (!fetchReadAnswer(answerBytes.data(), answerBytes.size(), sections, readError))
            {
                outWords = sections.fatal.empty()
                               ? "the fetch answer does not read: " + readError
                               : "the server ended the fetch over the fatal channel: " + sections.fatal.front();
                return false; // nothing has been written and no ref has been asked about
            }
            if (!sections.fatal.empty())
            {
                outWords = "the server ended the fetch over the fatal channel: " + sections.fatal.front();
                return false;
            }
            if (!sections.errLines.empty())
            {
                outWords = "the server refused the fetch: " + sections.errLines.front();
                return false;
            }

            // The pack, stood up and written whole before any ref is asked about.
            if (!sections.packBytes.empty())
            {
                if (static_cast<std::uint64_t>(sections.packBytes.size()) > kMaxFetchPackBytes)
                {
                    outWords = "the answer carries a pack of " +
                               std::to_string(sections.packBytes.size()) +
                               " bytes, past the bound of " +
                               std::to_string(kMaxFetchPackBytes) +
                               "; no part of it is stood up";
                    return false;
                }
                std::vector<std::uint8_t> pack;
                pack.reserve(sections.packBytes.size());
                for (char c : sections.packBytes)
                {
                    pack.push_back(static_cast<std::uint8_t>(c));
                }
                std::vector<std::string> written;
                if (!standUpPack(port, pack, written, outWords))
                {
                    return false;
                }
            }
            for (const std::pair<std::string, std::string> &entry : wanted)
            {
                if (!port.hasObject(entry.second))
                {
                    outWords = "the answer did not bring " + entry.second +
                               ", the object " + entry.first + " is asked to hold";
                    return false;
                }
            }

            // The refs: one transaction, all of it or none of it (refs.h:247-254).
            std::vector<RefUpdate> updates;
            for (const std::pair<std::string, std::string> &entry : wanted)
            {
                std::string held;
                const bool have = port.refValue(entry.first, held);
                const std::string heldLower = detail::lower_hex(held);
                if (have && heldLower == entry.second)
                {
                    continue; // the ref already holds exactly this; it does not move
                }
                RefUpdate update;
                update.name = entry.first;
                update.newOid = ObjectId(entry.second);
                update.expectedOldOid = have ? ObjectId(heldLower) : ObjectId();
                updates.push_back(update);
            }
            if (!updates.empty() && !port.commitRefs(updates))
            {
                outWords = "the ref transaction refused; no ref moved";
                return false;
            }
            for (const RefUpdate &update : updates)
            {
                movedOut.push_back(update.name);
            }

            // HEAD, to the branch the advertisement named it pointing through —
            // only when that branch moved with this fetch, so the symref never
            // names a branch this repository does not hold.
            for (const AdvertisedRef &ref : advertised)
            {
                if (ref.name != "HEAD" || ref.symrefTarget.empty())
                {
                    continue;
                }
                bool fetchedBranch = false;
                for (const std::string &moved : movedOut)
                {
                    if (moved == ref.symrefTarget)
                    {
                        fetchedBranch = true;
                        break;
                    }
                }
                if (!fetchedBranch)
                {
                    for (const std::pair<std::string, std::string> &entry : wanted)
                    {
                        if (entry.first == ref.symrefTarget)
                        {
                            fetchedBranch = true; // already at the advertised value
                            break;
                        }
                    }
                }
                if (!fetchedBranch)
                {
                    break;
                }
                if (!target.setHeadSymref(ref.symrefTarget))
                {
                    outWords = "the refs moved; HEAD did not: " + ref.symrefTarget;
                    return false;
                }
                break;
            }
            return true;
        }

    } // namespace gitcore
} // namespace ols
