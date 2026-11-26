#ifndef OLS_GITCORE_FETCH_WIRE_H
#define OLS_GITCORE_FETCH_WIRE_H
// src/gitcore/fetch_wire.h — the client half of the fetch wire (Batch F1):
// the protocol bytes a client reads and the one request it writes.
//
// This unit reads bytes and fills structs. It opens no socket, opens no
// repository, and asks nothing of how the bytes got here. Each reader mirrors
// the writer whose bytes it reads, so a round trip through the two lands the
// same values: v2Advertise (wire.h:121, wire.cpp:318-342) for the v2
// advertisement, receivePackAdvertisement (GitSmartHttp.cpp:1285-1364) for the
// v0 one, lsRefsAnswer (wire.cpp:596-665) for the ls-refs answer, and
// fetchAnswer (wire.cpp:839-948) for the fetch answer's sections.
//
// The v0 advertisement is read in both of its shapes. receive-pack states the
// "# service=git-receive-pack" marker and a flush before the ref lines
// (GitSmartHttp.cpp:1319-1325); upload-pack states no marker line at all, and
// the ref lines open the advertisement. The capabilities stand after the NUL of
// the first ref line either way (GitSmartHttp.cpp:1333-1335), and a HEAD with
// no id under it is stated with the id of nothing (GitSmartHttp.cpp:1346).
//
// Nothing here throws. A stream whose shape does not hold is a refusal that
// names why; a channel-3 fatal stops the walk and reports the words it carries.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "wire.h"

namespace ols
{
    namespace gitcore
    {

        // ---------------------------------------------------------------- advertisement
        //
        // capability-advertisement = PKT-LINE("version 2\n") capability* flush-pkt
        // (wire.h:117). The lines are split by wire.h's v2CapabilityOf, so a
        // "command=ls-refs fetch" line lands as one entry with the list as its value.
        bool fetchReadV2Advertisement(const std::uint8_t *bytes, std::size_t count,
                                      std::vector<V2Capability> &caps,
                                      std::string &error);

        // One ref line of a v0 advertisement: "<oid> <name>" with the capabilities
        // after the NUL of the first line. The id of nothing on a line is an unborn
        // HEAD, and the entry says so rather than hiding the zeros behind an id.
        struct V0RefEntry
        {
            std::string name;
            ObjectId oid;
            bool unborn = false;
        };

        struct V0Advertisement
        {
            std::vector<std::string> caps;   // capability words, in the order they came
            std::vector<V0RefEntry> entries; // every ref line, in the order it came
            V0RefEntry head;                 // a copy of the HEAD line, HEAD first as git states it
            bool hasHead = false;
        };
        bool fetchReadV0Advertisement(const std::uint8_t *bytes, std::size_t count,
                                      V0Advertisement &out, std::string &error);

        // ------------------------------------------------------------------- ls-refs
        //
        // The lines of an ls-refs answer, read back as the entries lsRefsAnswer wrote
        // (wire.cpp:629-656): "<oid> <name>[ symref-target:<t>][ peeled:<oid>]", and
        // the "unborn <name> symref-target:<t>" line as an entry carrying the id of
        // nothing and the target it names.
        bool fetchReadLsRefsAnswer(const std::uint8_t *bytes, std::size_t count,
                                   std::vector<LsRefsEntry> &entries,
                                   std::string &error);

        // -------------------------------------------------------------------- fetch
        //
        // The sections of one fetch answer, as fetchAnswer frames them
        // (wire.cpp:888-948): the acknowledgments section — "acknowledgments", the
        // NAK or ACK lines, the "ready" line that may name an oid — a delim-pkt, then
        // the packfile section and its sideband packets. The pack is the channel-1
        // bytes concatenated; the other channels are kept apart. A fatal answer is the
        // "ERR want <oid> cannot be found on the server" road (wire.cpp:859-871) and
        // lands in errLines.
        struct FetchAnswerSections
        {
            bool acknowledgments = false;
            std::vector<std::string> acks; // the oids the ACK lines name, in order
            bool nak = false;
            bool ready = false;
            std::string readyOid; // the oid after "ready", when one stands
            bool packfile = false;
            std::string packBytes;             // channel 1 concatenated
            std::vector<std::string> progress; // channel 2 text, in order
            std::vector<std::string> fatal;    // channel 3 words, in order
            std::vector<std::string> errLines; // "ERR ..." lines as they came
        };
        bool fetchReadAnswer(const std::uint8_t *bytes, std::size_t count,
                             FetchAnswerSections &out, std::string &error);

        // A sideband stream on its own: the frames of a packfile section, or of a
        // chunk of one, without the section lines around them. A channel-3 frame
        // stops the walk: its words land in fatal, error names them, and the answer is
        // false — a client that reads a fatal has no pack to expect. What was read
        // before the fatal is kept.
        struct SidebandStream
        {
            std::string packBytes;
            std::vector<std::string> progress;
            std::string fatal; // the words of the frame that stopped the walk
        };
        bool fetchDemuxSideband(const std::uint8_t *bytes, std::size_t count,
                                SidebandStream &out, std::string &error);

        // ------------------------------------------------------- v0 upload-pack request
        //
        // upload-pkt-line = PKT-LINE("want" SP sha1 [NUL cap-list] LF)
        //                   / PKT-LINE("have" SP sha1 LF)
        //                   / PKT-LINE("done" [SP sha1] LF)
        //                   / flush-pkt (gitprotocol-pack as it stood 2026-11-23).
        // The capabilities stand after the NUL of the first want and nowhere else;
        // "done" is the last line when the request carries it.
        struct V0UploadPackRequest
        {
            std::vector<std::string> wants;
            std::vector<std::string> haves;
            std::vector<std::string> caps; // capability words after that NUL
            bool done = false;
            std::string doneOid; // the oid of a bounded done, when one is sent
        };
        bool v0BuildUploadPackRequest(const V0UploadPackRequest &request,
                                      std::vector<std::uint8_t> &out,
                                      std::string &error);

    } // namespace gitcore
} // namespace ols

#endif // OLS_GITCORE_FETCH_WIRE_H
