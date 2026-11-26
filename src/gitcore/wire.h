// src/gitcore/wire.h — the wire-protocol unit of GitCore (spec §3.5): the
// framing of pkt-lines and the command framing of protocol v2.
//
// This unit owns the bytes and asks nothing else of them. The answer of
// ls-refs is written from a ref table the caller hands over, and the answer
// of fetch is decided from an object-info callback the caller hands over - a
// later door wires those to B1's object layer and B3's refs; the unit stands
// with no repository open. Nothing here throws: a request whose shape does not
// hold, a length that does not fit its buffer, and a command argument this
// unit does not answer are refusals that say why.
//
// The shapes follow git's Documentation/gitprotocol-v2.adoc as it stood
// 2026-11-23: the specials (0000 flush, 0001 delim, 0002 reserved for
// stateless ends and never answered by this unit), the capability
// advertisement ("version 2" line, key[=value] lines, flush), the command
// request ("command=<name>", capabilities, delim-pkt, arguments, flush), the
// ls-refs line "<oid> <refname>[ symref-target:<t>][ peeled:<oid>]" and the
// unborn line "unborn HEAD symref-target:<t>", and the sideband-multiplexed
// packfile section. Two things the document leaves open are fixed here and
// named where they answer: a want this unit cannot find is answered with the
// brief's "ERR <message>" line (the document has no error line outside the
// sideband channel), and a done fetch is answered in one round - wants held
// where the callback says are acknowledged, the rest are fatal - because this
// unit has no reachability and so cannot negotiate. Shallow, partial, and
// ref-resolved requests are refused with that reason, never guessed around.

#ifndef OLS_GITCORE_WIRE_H
#define OLS_GITCORE_WIRE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// B1's object layer: the ids in a request and in a ref entry are its
// ObjectFormat and ObjectId. The include stands at file scope the way refs.h
// and packfile.h keep it - from inside ols::gitcore it would nest the whole
// of objects.h and poison std.
#include "objects.h"

namespace ols
{
    namespace gitcore
    {

        // ------------------------------------------------------------------- framing
        //
        // A pkt-line is four lowercase hex bytes - the byte count of the line, the
        // four among them - and the payload. The specials are their four bytes whole:
        // "0000" ends a message, "0001" separates sections, and 0002/0003 are
        // reserved and never answered by this unit.

        // The longest payload that fits one line: git allows 65520 bytes for the
        // whole line.
        constexpr std::size_t kMaxPktPayload = 65516;

        // One sideband packet holds a channel byte and this many payload bytes.
        constexpr std::size_t kMaxSidebandData = kMaxPktPayload - 1;

        // What the reader found. Data carries bytes; Flush and Delim carry none.
        enum class PktKind
        {
            Data,
            Flush, // "0000" - end of a message
            Delim  // "0001" - section separator
        };

        // Appends one pkt-line of exactly the payload. False names the reason, and
        // out keeps no part of the refused line.
        bool pktWrite(const std::string &payload, std::vector<std::uint8_t> &out,
                      std::string &error);

        // The specials. They cannot fail.
        void pktFlush(std::vector<std::uint8_t> &out);
        void pktDelim(std::vector<std::uint8_t> &out);

        // Consumes one packet standing at pos; on true pos stands one past it, kind
        // says what came, and payload carries the bytes (empty for the specials). A
        // length field that is not hex, fewer than four bytes standing, a length
        // claiming bytes past the buffer, and the reserved specials are refusals that
        // leave pos and payload untouched.
        bool pktRead(const std::uint8_t *bytes, std::size_t count, std::size_t &pos,
                     PktKind &kind, std::string &payload, std::string &error);

        // The walk of a whole buffer: next hands over each packet's kind and a view
        // into the buffer the reader was made on - no bytes copied. False with a
        // reason when the bytes are exhausted or a packet is malformed.
        class PktReader
        {
        public:
            explicit PktReader(const std::uint8_t *bytes, std::size_t count);

            bool next(PktKind &kind, std::string_view &payload, std::string &error);
            bool exhausted() const;
            std::size_t position() const;

        private:
            const std::uint8_t *bytes_;
            std::size_t count_;
            std::size_t pos_;
        };

        // The response assembler: given the payload strings, the exact bytes - every
        // payload framed and the whole closed with a flush.
        bool assembleResponse(const std::vector<std::string> &payloads,
                              std::vector<std::uint8_t> &out, std::string &error);

        // A sideband packet: one channel byte (1 pack, 2 progress, 3 fatal) and the
        // bytes, as one line.
        bool v2Sideband(std::uint8_t channel, const std::string &payload,
                        std::vector<std::uint8_t> &out, std::string &error);

        // --------------------------------------------------- capability advertisement
        //
        // capability-advertisement = PKT-LINE("version 2\n") capability* flush-pkt.
        // The commands a server can run are one capability line -
        // "command=ls-refs fetch push-options" - stood here by v2CommandCapabilityLine
        // and handed to v2Advertise with the other lines the caller states.
        bool v2Advertise(const std::vector<std::string> &capabilityLines,
                         std::vector<std::uint8_t> &out, std::string &error);

        // The "command=<list>" line for a list of command names.
        bool v2CommandCapabilityLine(const std::vector<std::string> &commands,
                                     std::string &line, std::string &error);

        // A capability line - key ([A-Za-z0-9_-]+) with an optional value in the
        // document's character set - split. A line that does not fit that shape is
        // refused.
        struct V2Capability
        {
            std::string key;
            std::string value; // empty when the line carries none
        };
        bool v2CapabilityOf(const std::string &line, V2Capability &out,
                            std::string &error);

        // -------------------------------------------------------- command request
        //
        // command-request = PKT-LINE("command=<name>\n") capability-lines* delim-pkt
        // argument-lines* flush-pkt; a lone flush-pkt is the empty request and leaves
        // command empty.
        struct V2Request
        {
            std::string command;
            std::vector<std::string> capabilityLines; // payload lines as they came
            std::vector<std::string> argumentLines;   // payload lines as they came
        };

        bool v2ParseRequest(const std::uint8_t *bytes, std::size_t count,
                            V2Request &out, std::string &error);
        bool v2BuildRequest(const std::string &command,
                            const std::vector<std::string> &capabilityLines,
                            const std::vector<std::string> &argumentLines,
                            std::vector<std::uint8_t> &out, std::string &error);

        // ------------------------------------------------------------------- ls-refs

        struct LsRefsRequest
        {
            bool unborn = false;
            bool peel = false;
            bool symrefs = false;
            std::vector<std::string> prefixes; // ref-prefix arguments
        };
        bool v2ParseLsRefs(const V2Request &request, LsRefsRequest &out,
                           std::string &error);

        // One ref to answer with: what it points at, whether it is a symref and what
        // it points through, and the peeled value of a tag when one was asked for.
        struct LsRefsEntry
        {
            std::string name;
            ObjectId oid; // B1's: the hex and the format of the id
            bool isSymref = false;
            std::string symrefTarget;
            bool peeled = false;
            ObjectId peeledOid;
        };

        // The answer: one line per ref the request lets through -
        // "<oid> <refname>[ symref-target:<target>][ peeled:<oid>]" - or the "unborn"
        // line for a symref pointing at no object, only when the request asked for
        // unborn; then the flush. The table is answered in the order it is given;
        // entries the prefixes exclude are not looked at.
        bool lsRefsAnswer(const std::vector<LsRefsEntry> &entries,
                          const LsRefsRequest &request,
                          std::vector<std::uint8_t> &out, std::string &error);

        // --------------------------------------------------------------------- fetch
        //
        // The callback the answer is decided from: whether one object is held where
        // the door is mounted. The unit asks nothing of how it is held.
        class ObjectInfo
        {
        public:
            virtual ~ObjectInfo() = default;
            virtual bool held(const std::string &oidHex) const = 0;
        };

        // The trivial one: the ids in hand.
        class MemoryObjectInfo : public ObjectInfo
        {
        public:
            explicit MemoryObjectInfo(std::vector<std::string> heldOids);

            bool held(const std::string &oidHex) const override;

        private:
            std::vector<std::string> names_; // lowercase hex
        };

        struct FetchRequest
        {
            ObjectFormat format = ObjectFormat::Sha1;
            std::vector<std::string> wants; // lowercase hex, the width format names
            std::vector<std::string> haves; // lowercase hex
            bool done = false;
            // The optional arguments git sends routinely and this unit takes without
            // acting on them: its pack is a full-object pack of stored blocks, it
            // sends no progress of its own, and its delta bases are named by id.
            // Recording them is the honest record of what was asked; the answer does
            // not change.
            bool thinPack = false;
            bool noProgress = false;
            bool includeTag = false;
            bool ofsDelta = false;
        };
        bool v2ParseFetch(const V2Request &request, FetchRequest &out,
                          std::string &error);

        // The answer of one fetch round, in the document's sections.
        //
        // A request that carried "done" gets no acknowledgments at all: the packfile
        // section — "packfile\n" and the pack as sideband-channel-1 packets — and the
        // flush that ends the response. When every want was also sent as a have, the
        // section carries no data packets: an empty packfile section is what a client
        // that already holds all of it reads.
        //
        // A request that carried no done is answered in the same round: an
        // acknowledgments section — "acknowledgments\n", then "NAK\n" when none of
        // the haves is held or "ACK <oid>\n" for the ones that are, then the
        // "ready\n" line that says the packfile follows here — a delim-pkt, the
        // packfile section, and the flush.
        //
        // A want the callback does not hold is a fatal instead: one "ERR want <oid>
        // cannot be found on the server" line per such want and the flush, the
        // document's sideband-3 channel having no place outside a pack this answer
        // would only be a framing lie about. A want this unit cannot answer needs a
        // pack in hand and says so when there is none.
        bool fetchAnswer(const FetchRequest &request, const ObjectInfo &objectInfo,
                         const std::string &packBytes,
                         std::vector<std::uint8_t> &out, std::string &error);

    } // namespace gitcore
} // namespace ols

#endif // OLS_GITCORE_WIRE_H
