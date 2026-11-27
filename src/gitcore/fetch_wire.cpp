// src/gitcore/fetch_wire.cpp — the ground fetch_wire.h stands on.
//
// Named as a source beside the other units whose definitions are external
// (receive.cpp is named the same way). This unit asks the wire unit's
// symbols, so it carries the wire ground itself - the same road fetch.cpp
// rides: a translation unit that calls inline definitions must see them,
// for the link folds copies across objects only on the compiler that
// promised to (whole-program MSVC folded them here for years; gcc answers
// the call with an undefined reference and no mercy). The guard inside
// wire.cpp keeps a unity host from landing it twice in one unit.
#ifndef OLS_GITCORE_FETCH_WIRE_CPP
#define OLS_GITCORE_FETCH_WIRE_CPP

#include "fetch_wire.h"
#include "gitcore/wire.cpp"

namespace ols
{
    namespace gitcore
    {

        namespace
        {

            // A pkt-line payload as its line: the trailing LF the writer added, taken off.
            std::string stripLineFeed(const std::string_view &payload)
            {
                if (!payload.empty() && payload.back() == '\n')
                {
                    return std::string(payload.substr(0, payload.size() - 1u));
                }
                return std::string(payload);
            }

            // The words of a space-separated list, in order, with the runs of spaces and a
            // leading or trailing space left out.
            std::vector<std::string> wordsOf(const std::string &text)
            {
                std::vector<std::string> words;
                std::size_t at = 0u;
                while (at < text.size())
                {
                    while (at < text.size() && text[at] == ' ')
                    {
                        ++at;
                    }
                    const std::size_t start = at;
                    while (at < text.size() && text[at] != ' ')
                    {
                        ++at;
                    }
                    if (at > start)
                    {
                        words.push_back(text.substr(start, at - start));
                    }
                }
                return words;
            }

            bool hasPrefix(const std::string &text, const std::string &prefix)
            {
                return text.size() >= prefix.size() &&
                       text.compare(0u, prefix.size(), prefix) == 0;
            }

            // The two halves of "<hex> <name>". A line with no space, or with nothing on
            // either side of one, is not a ref line.
            bool splitOidName(const std::string &line, std::string &hex, std::string &name)
            {
                const std::size_t space = line.find(' ');
                if (space == std::string::npos || space == 0u || space + 1u >= line.size())
                {
                    return false;
                }
                hex = line.substr(0u, space);
                name = line.substr(space + 1u);
                return true;
            }

        } // anonymous namespace

        // ---------------------------------------------------------------- advertisement

        bool fetchReadV2Advertisement(const std::uint8_t *bytes, std::size_t count,
                                      std::vector<V2Capability> &caps,
                                      std::string &error)
        {
            caps.clear();
            PktReader reader(bytes, count);
            PktKind kind;
            std::string_view payload;
            if (!reader.next(kind, payload, error))
            {
                return false;
            }
            if (kind != PktKind::Data || stripLineFeed(payload) != "version 2")
            {
                error = "the advertisement does not open with the version 2 line";
                return false;
            }
            while (true)
            {
                if (!reader.next(kind, payload, error))
                {
                    return false;
                }
                if (kind == PktKind::Flush)
                {
                    return true;
                }
                if (kind != PktKind::Data)
                {
                    error = "a section separator stands among the capability lines";
                    return false;
                }
                const std::string line(payload);
                V2Capability cap;
                if (!v2CapabilityOf(line, cap, error))
                {
                    error = "capability line rejected: " + error;
                    return false;
                }
                caps.push_back(cap);
            }
        }

        bool fetchReadV0Advertisement(const std::uint8_t *bytes, std::size_t count,
                                      V0Advertisement &out, std::string &error)
        {
            out = V0Advertisement();
            PktReader reader(bytes, count);
            PktKind kind;
            std::string_view payload;
            if (!reader.next(kind, payload, error))
            {
                return false;
            }

            // receive-pack opens with the marker and its flush (GitSmartHttp.cpp:1319-
            // 1325); upload-pack opens with the first ref line, so this packet is a
            // ref line and is read below as any other one.
            if (kind == PktKind::Data && stripLineFeed(payload) == "# service=git-receive-pack")
            {
                if (!reader.next(kind, payload, error))
                {
                    return false;
                }
                if (kind != PktKind::Flush)
                {
                    error = "the marker line is followed by a ref line, not the flush that ends it";
                    return false;
                }
                if (!reader.next(kind, payload, error))
                {
                    return false;
                }
            }

            bool firstLine = true;
            while (true)
            {
                if (kind == PktKind::Flush)
                {
                    break;
                }
                if (kind != PktKind::Data)
                {
                    error = "a section separator stands among the ref lines";
                    return false;
                }
                const std::string line = stripLineFeed(payload);
                std::string head;
                std::string capsText;
                const std::size_t nul = line.find('\0');
                if (nul == std::string::npos)
                {
                    head = line;
                }
                else
                {
                    head = line.substr(0u, nul);
                    capsText = line.substr(nul + 1u);
                    if (firstLine)
                    {
                        out.caps = wordsOf(capsText);
                    }
                }
                firstLine = false;

                std::string hex;
                std::string name;
                if (!splitOidName(head, hex, name))
                {
                    error = "the ref line does not state an id and a name: " + head;
                    return false;
                }
                V0RefEntry entry;
                entry.name = name;
                if (!ObjectId::parse(hex, entry.oid))
                {
                    error = "the ref line for " + name + " states no object id: " + hex;
                    return false;
                }
                entry.unborn = entry.oid.is_null();
                out.entries.push_back(entry);
                if (name == "HEAD")
                {
                    out.head = entry;
                    out.hasHead = true;
                }

                if (!reader.next(kind, payload, error))
                {
                    return false;
                }
            }

            if (!reader.exhausted())
            {
                error = "bytes stand after the advertisement's flush";
                return false;
            }
            return true;
        }

        // -------------------------------------------------------------------- ls-refs

        bool fetchReadLsRefsAnswer(const std::uint8_t *bytes, std::size_t count,
                                   std::vector<LsRefsEntry> &entries,
                                   std::string &error)
        {
            entries.clear();
            PktReader reader(bytes, count);
            PktKind kind;
            std::string_view payload;
            while (true)
            {
                if (!reader.next(kind, payload, error))
                {
                    return false;
                }
                if (kind == PktKind::Flush)
                {
                    break;
                }
                if (kind != PktKind::Data)
                {
                    error = "a section separator stands among the ls-refs lines";
                    return false;
                }
                const std::vector<std::string> words = wordsOf(stripLineFeed(payload));
                if (words.empty())
                {
                    error = "the ls-refs answer carries a line with nothing on it";
                    return false;
                }

                LsRefsEntry entry;
                if (words[0] == "unborn")
                {
                    if (words.size() < 3u || !hasPrefix(words[2], "symref-target:"))
                    {
                        error = "the unborn line names no target it points through";
                        return false;
                    }
                    entry.name = words[1];
                    entry.isSymref = true;
                    entry.symrefTarget = words[2].substr(14u);
                    if (entry.symrefTarget.empty())
                    {
                        error = "the unborn line points through an unnamed target";
                        return false;
                    }
                }
                else
                {
                    if (!ObjectId::parse(words[0], entry.oid))
                    {
                        error = "the ls-refs line states no object id: " + words[0];
                        return false;
                    }
                    if (words.size() < 2u)
                    {
                        error = "the ls-refs line names no ref: " + words[0];
                        return false;
                    }
                    entry.name = words[1];
                    for (std::size_t at = 2u; at < words.size(); ++at)
                    {
                        if (hasPrefix(words[at], "symref-target:"))
                        {
                            entry.isSymref = true;
                            entry.symrefTarget = words[at].substr(14u);
                            if (entry.symrefTarget.empty())
                            {
                                error = "the symref " + entry.name + " points through an unnamed target";
                                return false;
                            }
                        }
                        else if (hasPrefix(words[at], "peeled:"))
                        {
                            if (!ObjectId::parse(words[at].substr(7u), entry.peeledOid))
                            {
                                error = "the peeled value of " + entry.name + " states no object id";
                                return false;
                            }
                            entry.peeled = true;
                        }
                        else
                        {
                            error = "the ls-refs line for " + entry.name +
                                    " carries a word this reader does not name: " + words[at];
                            return false;
                        }
                    }
                }
                if (entry.name.empty())
                {
                    error = "the ls-refs line names no ref";
                    return false;
                }
                entries.push_back(entry);
            }

            if (!reader.exhausted())
            {
                error = "bytes stand after the ls-refs answer's flush";
                return false;
            }
            return true;
        }

        // ---------------------------------------------------------------------- fetch

        bool fetchReadAnswer(const std::uint8_t *bytes, std::size_t count,
                             FetchAnswerSections &out, std::string &error)
        {
            out = FetchAnswerSections();
            PktReader reader(bytes, count);
            PktKind kind;
            std::string_view payload;
            while (true)
            {
                if (!reader.next(kind, payload, error))
                {
                    return false;
                }
                if (kind == PktKind::Flush)
                {
                    break;
                }
                if (kind == PktKind::Delim)
                {
                    continue;
                }

                // A sideband frame is told from a section line by its first byte, the
                // channel; the pack bytes of a frame end at that byte and no further,
                // so the line feed is taken off only after the channel was read.
                if (!payload.empty() &&
                    (payload[0] == static_cast<char>(1u) ||
                     payload[0] == static_cast<char>(2u) ||
                     payload[0] == static_cast<char>(3u)))
                {
                    const std::string data(payload.substr(1));
                    if (payload[0] == static_cast<char>(1u))
                    {
                        out.packBytes += data;
                    }
                    else if (payload[0] == static_cast<char>(2u))
                    {
                        out.progress.push_back(data);
                    }
                    else
                    {
                        out.fatal.push_back(data);
                    }
                    continue;
                }

                const std::string line = stripLineFeed(payload);
                if (line == "acknowledgments")
                {
                    out.acknowledgments = true;
                    continue;
                }
                if (line == "NAK")
                {
                    out.nak = true;
                    continue;
                }
                if (hasPrefix(line, "ACK "))
                {
                    const std::vector<std::string> words = wordsOf(line);
                    ObjectId ack;
                    if (words.size() != 2u || !ObjectId::parse(words[1], ack))
                    {
                        error = "the acknowledgment line names no object: " + line;
                        return false;
                    }
                    out.acks.push_back(words[1]);
                    continue;
                }
                if (line == "ready")
                {
                    out.ready = true;
                    continue;
                }
                if (hasPrefix(line, "ready "))
                {
                    out.ready = true;
                    out.readyOid = line.substr(6u);
                    if (out.readyOid.find(' ') != std::string::npos || out.readyOid.empty())
                    {
                        error = "the ready line names more than one object: " + line;
                        return false;
                    }
                    continue;
                }
                if (line == "packfile")
                {
                    out.packfile = true;
                    continue;
                }
                if (hasPrefix(line, "ERR "))
                {
                    out.errLines.push_back(line);
                    continue;
                }
                error = "the answer carries a line this reader does not name: " + line;
                return false;
            }

            if (!reader.exhausted())
            {
                error = "bytes stand after the answer's flush";
                return false;
            }
            return true;
        }

        bool fetchDemuxSideband(const std::uint8_t *bytes, std::size_t count,
                                SidebandStream &out, std::string &error)
        {
            out = SidebandStream();
            PktReader reader(bytes, count);
            PktKind kind;
            std::string_view payload;
            while (true)
            {
                if (!reader.next(kind, payload, error))
                {
                    // A chunk ends where its bytes end; a malformed packet does not.
                    if (reader.exhausted())
                    {
                        return true;
                    }
                    return false;
                }
                if (kind == PktKind::Flush)
                {
                    return true;
                }
                if (kind != PktKind::Data || payload.empty())
                {
                    error = "a packet of the stream carries no channel byte";
                    return false;
                }
                const std::string data(payload.substr(1));
                if (payload[0] == static_cast<char>(1u))
                {
                    out.packBytes += data;
                }
                else if (payload[0] == static_cast<char>(2u))
                {
                    out.progress.push_back(data);
                }
                else if (payload[0] == static_cast<char>(3u))
                {
                    out.fatal = data;
                    error = "the fatal channel says: " + data;
                    return false;
                }
                else
                {
                    error = "the stream carries a channel outside 1, 2, and 3";
                    return false;
                }
            }
        }

        // ------------------------------------------------------- v0 upload-pack request

        bool v0BuildUploadPackRequest(const V0UploadPackRequest &request,
                                      std::vector<std::uint8_t> &out,
                                      std::string &error)
        {
            out.clear();
            if (request.wants.empty())
            {
                error = "the upload-pack request carries no want";
                return false;
            }
            for (const std::string &word : request.caps)
            {
                if (word.empty() || word.find(' ') != std::string::npos ||
                    word.find('\0') != std::string::npos)
                {
                    error = "the capability list carries an empty or split word: " + word;
                    return false;
                }
            }

            std::vector<std::uint8_t> local;
            for (std::size_t at = 0u; at < request.wants.size(); ++at)
            {
                std::string line = "want " + request.wants[at];
                if (at == 0u && !request.caps.empty())
                {
                    line.push_back('\0');
                    for (std::size_t word = 0u; word < request.caps.size(); ++word)
                    {
                        if (word != 0u)
                        {
                            line.push_back(' ');
                        }
                        line += request.caps[word];
                    }
                }
                line.push_back('\n');
                if (!pktWrite(line, local, error))
                {
                    return false;
                }
            }
            for (const std::string &have : request.haves)
            {
                if (!pktWrite("have " + have + "\n", local, error))
                {
                    return false;
                }
            }
            if (request.done)
            {
                std::string line = "done";
                if (!request.doneOid.empty())
                {
                    line += " " + request.doneOid;
                }
                line.push_back('\n');
                if (!pktWrite(line, local, error))
                {
                    return false;
                }
            }
            pktFlush(local);
            out.swap(local);
            return true;
        }

    } // namespace gitcore
} // namespace ols

#endif // OLS_GITCORE_FETCH_WIRE_CPP
