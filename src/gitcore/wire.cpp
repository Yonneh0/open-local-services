// src/gitcore/wire.cpp — the ground wire.h stands on.
//
// The definitions are marked inline because no build names this file as a
// source: the doors that ask its symbols - OpenLocalServices.cpp and
// GitSmartHttp.cpp - pull the unit in beside their own with a unity include,
// and the harness reaches the same ground through test_wire.cpp's include;
// one contract, one ground, whichever unit carries it into the link.
//
// The unit is guarded against landing twice in one translation unit: a host
// that pulls in several door files (the harness does - test_wire.cpp, then
// GitSmartHttp.cpp through test_receive.cpp) would otherwise see this file's
// anonymous namespace defined twice in the same scope.
//
// That shape is load-bearing and no tidiness applies to it. This file must
// never be named as a source: every definition here is inline, and a named
// object beside the unity includes would land its symbols a second time -
// or, behind the guard, none at all in the object that was meant to carry
// them. No unit may include this file twice without the OLS_GITCORE_WIRE_CPP
// macro standing between the two, and no door may push its include up into a
// header: the include is the whole way this unit enters a link, and the
// harness rides on it - test_wire.cpp lands the ground first, and the door's
// include behind the guard reads the symbols back rather than landing them
// again.
#ifndef OLS_GITCORE_WIRE_CPP
#define OLS_GITCORE_WIRE_CPP
#include "wire.h"

#include <algorithm>
#include <iterator>

namespace ols
{
    namespace gitcore
    {

        // ------------------------------------------------------------------ the hex
        //
        // The length field is written lowercase and read in either case, the way
        // git's own reader accepts.

        namespace
        {

            constexpr const char *HEX_DIGITS = "0123456789abcdef";

            bool hexValue(char c, unsigned &value)
            {
                if (c >= '0' && c <= '9')
                {
                    value = static_cast<unsigned>(c - '0');
                    return true;
                }
                if (c >= 'a' && c <= 'f')
                {
                    value = static_cast<unsigned>(c - 'a') + 10u;
                    return true;
                }
                if (c >= 'A' && c <= 'F')
                {
                    value = static_cast<unsigned>(c - 'A') + 10u;
                    return true;
                }
                return false;
            }

            bool isKeyChar(char c)
            {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_';
            }

            // The value characters the document's value production allows.
            constexpr std::string_view VALUE_CHARS = " -_.,?\\()/{}[]()<>!@#$%^&*+=:;";

            bool isValueChar(char c)
            {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                {
                    return true;
                }
                return VALUE_CHARS.find(c) != std::string_view::npos;
            }

            bool allKeyChars(const std::string &text)
            {
                for (const char c : text)
                {
                    if (!isKeyChar(c))
                    {
                        return false;
                    }
                }
                return true;
            }

            bool endsLine(const std::string &text)
            {
                return !text.empty() && text.back() == '\n';
            }

            bool carriesLineBytes(const std::string &text)
            {
                return text.find('\0') != std::string::npos ||
                       text.find('\n') != std::string::npos ||
                       text.find(' ') != std::string::npos;
            }

            void lowerInPlace(std::string &text)
            {
                for (char &c : text)
                {
                    if (c >= 'A' && c <= 'Z')
                    {
                        c = static_cast<char>(c - 'A' + 'a');
                    }
                }
            }

            // The one reading both doors share: a packet, as a view of the buffer.
            bool readPacket(const std::uint8_t *bytes, std::size_t count, std::size_t &pos,
                            PktKind &kind, const std::uint8_t *&payloadOut,
                            std::size_t &payloadSize, std::string &error)
            {
                if (pos > count)
                {
                    error = "the position stands past the buffer";
                    return false;
                }
                if (count - pos < 4u)
                {
                    error = "fewer than four bytes stand: no length field";
                    return false;
                }
                std::size_t len = 0;
                for (std::size_t i = 0; i < 4u; ++i)
                {
                    unsigned nibble = 0;
                    if (!hexValue(static_cast<char>(bytes[pos + i]), nibble))
                    {
                        error = "the length field is not hex";
                        return false;
                    }
                    len = len * 16u + nibble;
                }
                if (len == 0u)
                {
                    kind = PktKind::Flush;
                    payloadOut = nullptr;
                    payloadSize = 0;
                    pos += 4u;
                    return true;
                }
                if (len == 1u)
                {
                    kind = PktKind::Delim;
                    payloadOut = nullptr;
                    payloadSize = 0;
                    pos += 4u;
                    return true;
                }
                if (len < 4u)
                {
                    error = "the reserved special (0002/0003) is not answered by this unit";
                    return false;
                }
                if (len > count - pos)
                {
                    error = "the length claims bytes past the buffer";
                    return false;
                }
                kind = PktKind::Data;
                payloadOut = bytes + pos + 4u;
                payloadSize = len - 4u;
                pos += len;
                return true;
            }

            bool prefixMatched(const std::string &name, const std::vector<std::string> &prefixes)
            {
                for (const std::string &prefix : prefixes)
                {
                    if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0)
                    {
                        return true;
                    }
                }
                return false;
            }

        } // anonymous namespace

        // ------------------------------------------------------------------- framing

        inline bool pktWrite(const std::string &payload, std::vector<std::uint8_t> &out,
                             std::string &error)
        {
            if (payload.size() > kMaxPktPayload)
            {
                error = "payload does not fit one pkt-line: " + std::to_string(payload.size()) +
                        " bytes, over " + std::to_string(kMaxPktPayload);
                return false;
            }
            const std::size_t width = payload.size() + 4u;
            out.reserve(out.size() + width);
            out.push_back(static_cast<std::uint8_t>(HEX_DIGITS[(width >> 12) & 0xFu]));
            out.push_back(static_cast<std::uint8_t>(HEX_DIGITS[(width >> 8) & 0xFu]));
            out.push_back(static_cast<std::uint8_t>(HEX_DIGITS[(width >> 4) & 0xFu]));
            out.push_back(static_cast<std::uint8_t>(HEX_DIGITS[width & 0xFu]));
            out.insert(out.end(),
                       reinterpret_cast<const unsigned char *>(payload.data()),
                       reinterpret_cast<const unsigned char *>(payload.data()) + payload.size());
            return true;
        }

        inline void pktFlush(std::vector<std::uint8_t> &out)
        {
            out.push_back(static_cast<std::uint8_t>('0'));
            out.push_back(static_cast<std::uint8_t>('0'));
            out.push_back(static_cast<std::uint8_t>('0'));
            out.push_back(static_cast<std::uint8_t>('0'));
        }

        inline void pktDelim(std::vector<std::uint8_t> &out)
        {
            out.push_back(static_cast<std::uint8_t>('0'));
            out.push_back(static_cast<std::uint8_t>('0'));
            out.push_back(static_cast<std::uint8_t>('0'));
            out.push_back(static_cast<std::uint8_t>('1'));
        }

        inline bool pktRead(const std::uint8_t *bytes, std::size_t count, std::size_t &pos,
                            PktKind &kind, std::string &payload, std::string &error)
        {
            const std::uint8_t *found = nullptr;
            std::size_t foundSize = 0;
            std::size_t at = pos;
            if (!readPacket(bytes, count, at, kind, found, foundSize, error))
            {
                return false;
            }
            payload.assign(reinterpret_cast<const char *>(found), foundSize);
            pos = at;
            return true;
        }

        inline PktReader::PktReader(const std::uint8_t *bytes, std::size_t count)
            : bytes_(bytes), count_(count), pos_(0)
        {
        }

        inline bool PktReader::next(PktKind &kind, std::string_view &payload,
                                    std::string &error)
        {
            if (pos_ >= count_)
            {
                error = "the bytes are exhausted";
                return false;
            }
            const std::uint8_t *found = nullptr;
            std::size_t foundSize = 0;
            if (!readPacket(bytes_, count_, pos_, kind, found, foundSize, error))
            {
                return false;
            }
            payload = std::string_view(reinterpret_cast<const char *>(found), foundSize);
            return true;
        }

        inline bool PktReader::exhausted() const
        {
            return pos_ >= count_;
        }

        inline std::size_t PktReader::position() const
        {
            return pos_;
        }

        // The response assembler: every payload framed, the whole closed with a
        // flush. A refused payload leaves out untouched, not half a response.
        inline bool assembleResponse(const std::vector<std::string> &payloads,
                                     std::vector<std::uint8_t> &out, std::string &error)
        {
            std::vector<std::uint8_t> local;
            for (const std::string &payload : payloads)
            {
                if (!pktWrite(payload, local, error))
                {
                    return false;
                }
            }
            pktFlush(local);
            out.swap(local);
            return true;
        }

        inline bool v2Sideband(std::uint8_t channel, const std::string &payload,
                               std::vector<std::uint8_t> &out, std::string &error)
        {
            if (channel < 1u || channel > 3u)
            {
                error = "the sideband channel must be 1, 2, or 3";
                return false;
            }
            if (payload.size() > kMaxSidebandData)
            {
                error = "the sideband payload does not fit one line";
                return false;
            }
            std::string framed;
            framed.reserve(payload.size() + 1u);
            framed.push_back(static_cast<char>(channel));
            framed += payload;
            return pktWrite(framed, out, error);
        }

        // ------------------------------------------------- capability advertisement

        inline bool v2Advertise(const std::vector<std::string> &capabilityLines,
                                std::vector<std::uint8_t> &out, std::string &error)
        {
            std::vector<std::uint8_t> local;
            if (!pktWrite("version 2\n", local, error))
            {
                return false;
            }
            for (const std::string &line : capabilityLines)
            {
                V2Capability cap;
                if (!v2CapabilityOf(line, cap, error))
                {
                    error = "capability line rejected: " + error;
                    return false;
                }
                if (!pktWrite(line, local, error))
                {
                    return false;
                }
            }
            pktFlush(local);
            out.swap(local);
            return true;
        }

        inline bool v2CommandCapabilityLine(const std::vector<std::string> &commands,
                                            std::string &line, std::string &error)
        {
            if (commands.empty())
            {
                error = "no command to list";
                return false;
            }
            std::string listed;
            for (const std::string &name : commands)
            {
                if (name.empty() || !allKeyChars(name))
                {
                    error = "the command name carries a character outside the grammar: " + name;
                    return false;
                }
                if (!listed.empty())
                {
                    listed += " ";
                }
                listed += name;
            }
            line = "command=" + listed + "\n";
            return true;
        }

        inline bool v2CapabilityOf(const std::string &line, V2Capability &out,
                                   std::string &error)
        {
            if (!endsLine(line))
            {
                error = "capability line does not end a line";
                return false;
            }
            const std::string body = line.substr(0, line.size() - 1u);
            const std::size_t eq = body.find('=');
            const std::size_t keyEnd = (eq == std::string::npos) ? body.size() : eq;
            if (keyEnd == 0u)
            {
                error = "capability line carries no key";
                return false;
            }
            for (std::size_t i = 0; i < keyEnd; ++i)
            {
                if (!isKeyChar(body[i]))
                {
                    error = "capability key carries a character outside the grammar";
                    return false;
                }
            }
            V2Capability local;
            local.key = body.substr(0, keyEnd);
            if (eq != std::string::npos)
            {
                local.value = body.substr(eq + 1u);
                if (local.value.empty())
                {
                    error = "capability line carries an empty value";
                    return false;
                }
                for (const char c : local.value)
                {
                    if (!isValueChar(c))
                    {
                        error = "capability value carries a character outside the grammar";
                        return false;
                    }
                }
            }
            out = std::move(local);
            return true;
        }

        // --------------------------------------------------------- command request

        inline bool v2ParseRequest(const std::uint8_t *bytes, std::size_t count,
                                   V2Request &out, std::string &error)
        {
            V2Request local;
            PktReader reader(bytes, count);
            PktKind kind;
            std::string_view view;
            if (!reader.next(kind, view, error))
            {
                return false;
            }
            if (kind == PktKind::Flush)
            {
                if (!reader.exhausted())
                {
                    error = "bytes stand after the empty request";
                    return false;
                }
                out = std::move(local);
                return true;
            }
            if (kind != PktKind::Data)
            {
                error = "the request does not start with a command line";
                return false;
            }
            const std::string first(view.begin(), view.end());
            if (first.size() < 10u || first.compare(0, 8, "command=") != 0 || !endsLine(first))
            {
                error = "the command line does not fit the grammar";
                return false;
            }
            const std::string name = first.substr(8u, first.size() - 9u);
            if (name.empty() || !allKeyChars(name))
            {
                error = "the command names no key the grammar allows";
                return false;
            }
            local.command = name;
            while (true)
            {
                if (!reader.next(kind, view, error))
                {
                    error = "the request carries no delimiter after its command";
                    return false;
                }
                if (kind == PktKind::Delim)
                {
                    break;
                }
                if (kind == PktKind::Flush)
                {
                    error = "the request reaches its end without a delimiter";
                    return false;
                }
                local.capabilityLines.emplace_back(view.begin(), view.end());
            }
            while (true)
            {
                if (!reader.next(kind, view, error))
                {
                    error = "the request never ends: no flush-pkt";
                    return false;
                }
                if (kind == PktKind::Flush)
                {
                    break;
                }
                if (kind == PktKind::Delim)
                {
                    error = "the request carries a second delimiter";
                    return false;
                }
                local.argumentLines.emplace_back(view.begin(), view.end());
            }
            if (!reader.exhausted())
            {
                error = "bytes stand after the request's flush";
                return false;
            }
            out = std::move(local);
            return true;
        }

        inline bool v2BuildRequest(const std::string &command,
                                   const std::vector<std::string> &capabilityLines,
                                   const std::vector<std::string> &argumentLines,
                                   std::vector<std::uint8_t> &out, std::string &error)
        {
            if (command.empty() || !allKeyChars(command))
            {
                error = "the command names no key the grammar allows";
                return false;
            }
            std::vector<std::uint8_t> local;
            if (!pktWrite("command=" + command + "\n", local, error))
            {
                return false;
            }
            for (const std::string &line : capabilityLines)
            {
                V2Capability cap;
                if (!v2CapabilityOf(line, cap, error))
                {
                    error = "capability line rejected: " + error;
                    return false;
                }
                if (!pktWrite(line, local, error))
                {
                    return false;
                }
            }
            pktDelim(local);
            for (const std::string &line : argumentLines)
            {
                if (!pktWrite(line, local, error))
                {
                    return false;
                }
            }
            pktFlush(local);
            out.swap(local);
            return true;
        }

        // ------------------------------------------------------------------ ls-refs

        inline bool v2ParseLsRefs(const V2Request &request, LsRefsRequest &out,
                                  std::string &error)
        {
            if (request.command != "ls-refs")
            {
                error = "the request carries the command " + request.command + ", not ls-refs";
                return false;
            }
            LsRefsRequest local;
            for (const std::string &line : request.argumentLines)
            {
                if (!endsLine(line))
                {
                    error = "ls-refs argument line does not end a line";
                    return false;
                }
                const std::string body = line.substr(0, line.size() - 1u);
                if (body == "unborn")
                {
                    local.unborn = true;
                    continue;
                }
                if (body == "peel")
                {
                    local.peel = true;
                    continue;
                }
                if (body == "symrefs")
                {
                    local.symrefs = true;
                    continue;
                }
                if (body.size() > 11u && body.compare(0, 11, "ref-prefix ") == 0)
                {
                    const std::string prefix = body.substr(11u);
                    if (prefix.empty() || carriesLineBytes(prefix))
                    {
                        error = "ref-prefix carries no prefix a name could match";
                        return false;
                    }
                    local.prefixes.push_back(prefix);
                    continue;
                }
                error = "ls-refs argument not understood: " + body;
                return false;
            }
            out = std::move(local);
            return true;
        }

        inline bool lsRefsAnswer(const std::vector<LsRefsEntry> &entries,
                                 const LsRefsRequest &request,
                                 std::vector<std::uint8_t> &out, std::string &error)
        {
            std::vector<std::uint8_t> local;
            for (const LsRefsEntry &entry : entries)
            {
                if (!request.prefixes.empty() && !prefixMatched(entry.name, request.prefixes))
                {
                    continue;
                }
                if (entry.name.empty() || carriesLineBytes(entry.name))
                {
                    error = "the entry name carries a byte a ref name cannot carry: " + entry.name;
                    return false;
                }
                const bool nullOid = entry.oid.is_null();
                if (!nullOid && !entry.oid.valid())
                {
                    error = "the entry " + entry.name + " carries no object id";
                    return false;
                }
                if (entry.isSymref &&
                    (entry.symrefTarget.empty() || carriesLineBytes(entry.symrefTarget)))
                {
                    error = "the symref " + entry.name + " states no target it can name";
                    return false;
                }
                if (request.peel && entry.peeled && !entry.peeledOid.valid())
                {
                    error = "the peeled value of " + entry.name + " carries no object id";
                    return false;
                }
                std::string line;
                if (nullOid)
                {
                    if (!entry.isSymref)
                    {
                        error = "the ref " + entry.name + " states no object and passes no symref";
                        return false;
                    }
                    if (!request.unborn)
                    {
                        error = "answering " + entry.name + " needs the unborn line and the request did not ask for it";
                        return false;
                    }
                    line = "unborn " + entry.name + " symref-target:" + entry.symrefTarget;
                }
                else
                {
                    line = entry.oid.hex + " " + entry.name;
                    if (entry.isSymref && request.symrefs)
                    {
                        line += " symref-target:" + entry.symrefTarget;
                    }
                    if (request.peel && entry.peeled)
                    {
                        line += " peeled:" + entry.peeledOid.hex;
                    }
                }
                line += "\n";
                if (!pktWrite(line, local, error))
                {
                    return false;
                }
            }
            pktFlush(local);
            out.swap(local);
            return true;
        }

        // -------------------------------------------------------------------- fetch

        inline MemoryObjectInfo::MemoryObjectInfo(std::vector<std::string> heldOids)
        {
            names_ = std::move(heldOids);
            for (std::string &name : names_)
            {
                lowerInPlace(name);
            }
        }

        inline bool MemoryObjectInfo::held(const std::string &oidHex) const
        {
            std::string probe = oidHex;
            lowerInPlace(probe);
            return std::find(names_.begin(), names_.end(), probe) != names_.end();
        }

        inline bool v2ParseFetch(const V2Request &request, FetchRequest &out,
                                 std::string &error)
        {
            if (request.command != "fetch")
            {
                error = "the request carries the command " + request.command + ", not fetch";
                return false;
            }
            FetchRequest local;
            bool askedFormat = false;
            for (const std::string &line : request.capabilityLines)
            {
                V2Capability cap;
                if (!v2CapabilityOf(line, cap, error))
                {
                    error = "capability line rejected: " + error;
                    return false;
                }
                if (cap.key != "object-format")
                {
                    continue;
                }
                ObjectFormat asked = ObjectFormat::Sha1;
                if (cap.value == "sha1")
                {
                    asked = ObjectFormat::Sha1;
                }
                else if (cap.value == "sha256")
                {
                    asked = ObjectFormat::Sha256;
                }
                else
                {
                    error = "object-format names a format this unit does not speak: " + cap.value;
                    return false;
                }
                if (askedFormat && asked != local.format)
                {
                    error = "object-format is asked twice with different formats";
                    return false;
                }
                local.format = asked;
                askedFormat = true;
            }
            for (const std::string &line : request.argumentLines)
            {
                if (!endsLine(line))
                {
                    error = "fetch argument line does not end a line";
                    return false;
                }
                const std::string body = line.substr(0, line.size() - 1u);
                if (body.empty())
                {
                    error = "the fetch carries an empty argument line";
                    return false;
                }
                const std::size_t space = body.find(' ');
                const std::string token =
                    (space == std::string::npos) ? body : body.substr(0, space);
                const std::string rest =
                    (space == std::string::npos) ? std::string() : body.substr(space + 1u);
                if (token == "want" || token == "have")
                {
                    if (rest.empty())
                    {
                        error = token + " names no object id";
                        return false;
                    }
                    ObjectId id;
                    std::string why;
                    if (!oidParseHex(rest, local.format, id, why))
                    {
                        error = token + " names no id of the requested format: " + why;
                        return false;
                    }
                    if (token == "want")
                    {
                        local.wants.push_back(id.hex);
                    }
                    else
                    {
                        local.haves.push_back(id.hex);
                    }
                    continue;
                }
                if (token == "done")
                {
                    if (!rest.empty())
                    {
                        error = "done takes no argument";
                        return false;
                    }
                    local.done = true;
                    continue;
                }
                if (token == "deepen" || token == "deepen-relative" ||
                    token == "deepen-since" || token == "deepen-not" ||
                    token == "shallow" || token == "depth")
                {
                    error = token + " asks a shallow fetch; this unit answers full, deep, unfiltered fetches only";
                    return false;
                }
                if (token == "filter")
                {
                    error = "filter asks a partial fetch; this unit answers full, unfiltered fetches only";
                    return false;
                }
                if (token == "want-ref")
                {
                    error = "want-ref resolves refs to ids; this unit answers wants by id only";
                    return false;
                }
                // The four optional arguments of the command. git sends them and asks
                // nothing of this unit that it cannot already answer: the pack is a
                // full-object pack of stored blocks, so thin-pack is answered by the
                // complete non-thin pack, ofs-delta describes a pack this unit never
                // writes, it sends no progress of its own, and its annotated tags
                // travel with the objects they point at. Taking the argument and doing
                // nothing is the protocol-legal reading; it is recorded so the door
                // above can see what was asked.
                if (token == "thin-pack" || token == "no-progress" ||
                    token == "include-tag" || token == "ofs-delta")
                {
                    if (!rest.empty())
                    {
                        error = token + " takes no argument";
                        return false;
                    }
                    if (token == "thin-pack")
                    {
                        local.thinPack = true;
                    }
                    else if (token == "no-progress")
                    {
                        local.noProgress = true;
                    }
                    else if (token == "include-tag")
                    {
                        local.includeTag = true;
                    }
                    else
                    {
                        local.ofsDelta = true;
                    }
                    continue;
                }
                error = "fetch argument not understood: " + token;
                return false;
            }
            out = std::move(local);
            return true;
        }

        inline bool fetchAnswer(const FetchRequest &request, const ObjectInfo &objectInfo,
                                const std::string &packBytes,
                                std::vector<std::uint8_t> &out, std::string &error)
        {
            if (request.wants.empty())
            {
                error = "the fetch carries no want";
                return false;
            }
            std::vector<std::uint8_t> local;
            std::vector<std::string> missing;
            for (const std::string &want : request.wants)
            {
                if (!objectInfo.held(want))
                {
                    missing.push_back(want);
                }
            }
            if (!missing.empty())
            {
                for (const std::string &want : missing)
                {
                    const std::string line =
                        "ERR want " + want + " cannot be found on the server\n";
                    if (!pktWrite(line, local, error))
                    {
                        return false;
                    }
                }
                pktFlush(local);
                out.swap(local);
                return true;
            }

            // The client was sent every want it already holds: the pack would carry
            // nothing, and the document's empty packfile section is the answer.
            bool nothingWanted = true;
            for (const std::string &want : request.wants)
            {
                if (std::find(request.haves.begin(), request.haves.end(), want) == request.haves.end())
                {
                    nothingWanted = false;
                    break;
                }
            }

            // A request that carried "done" is answered without an acknowledgments
            // section at all; one that did not is answered in this round, so the
            // section says what was common and that the pack follows here.
            if (!request.done)
            {
                if (!pktWrite("acknowledgments\n", local, error))
                {
                    return false;
                }
                bool common = false;
                for (const std::string &have : request.haves)
                {
                    if (!objectInfo.held(have))
                    {
                        continue;
                    }
                    if (!pktWrite("ACK " + have + "\n", local, error))
                    {
                        return false;
                    }
                    common = true;
                }
                if (!common)
                {
                    if (!pktWrite("NAK\n", local, error))
                    {
                        return false;
                    }
                }
                if (!pktWrite("ready\n", local, error))
                {
                    return false;
                }
                pktDelim(local);
            }

            if (!pktWrite("packfile\n", local, error))
            {
                return false;
            }
            if (nothingWanted)
            {
                pktFlush(local);
                out.swap(local);
                return true;
            }
            if (packBytes.empty())
            {
                error = "no pack is in hand: the answer ends with a pack, and an empty fetch sends its 24-byte empty pack";
                return false;
            }
            for (std::size_t at = 0; at < packBytes.size(); at += kMaxSidebandData)
            {
                const std::size_t left = packBytes.size() - at;
                const std::size_t take = (left < kMaxSidebandData) ? left : kMaxSidebandData;
                if (!v2Sideband(static_cast<std::uint8_t>(1u), packBytes.substr(at, take), local, error))
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

#endif // OLS_GITCORE_WIRE_CPP
