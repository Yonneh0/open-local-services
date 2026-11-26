// src/gitcore/receive.h — the server side of git receive-pack: the commands a
// push asks for, the pack it brings, and the policy that decides whether the
// refs may move.
//
// The unit opens no file and holds no repository. Objects and refs are reached
// through RepoAccess, the port the caller implements — receive.cpp stands on it,
// and the far side of it is objects.h and refs.h. What this unit owns is the
// order of the answers and the words of the refusals: a pack that does not
// stand, an object neither side holds, a path the pack names that no
// repository may carry, a protected branch, a move that is not a fast-forward.
//
// The bytes handed to receive are the pack followed by its idx, or the pack
// alone: a v2 client sends only the pack after the command's flush, so when no
// index stands where the pack's entry count says one should (a v2 idx TOC magic
// ff 74 6f 63 00 00 00 02 at packsize + 1072 + 28*entries, plus 8 per
// large-offset row) the pack's own index is made here — its records are
// scanned, resolved through their delta chains, and written by writeIdx. Both
// roads end at the same opened pair. A section that closes with a flush-pkt
// after the pack — the way the form allows it to end, and the way a push that
// brings no pack says so — is read through the same pair of roads: the bytes
// as they came first, the flush off second.
//
// No call in this namespace throws. A pack that does not open, an object
// neither side holds, and a transaction that does not hold are false with the
// reason in the error string. A command that is refused is not one of those:
// the push was read and answered, and its answer is the ng line its result
// carries.

#ifndef OLS_GITCORE_RECEIVE_H
#define OLS_GITCORE_RECEIVE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The ref transaction of refs.h: RefUpdate is the row the apply step builds,
// so the header that states the port states it too.
#include "gitcore/refs.h"

namespace ols
{
    namespace gitcore
    {

        // ---------------------------------------------------------------- the command
        //
        // One command of a receive-pack: the move of one ref, as the client asked it.

        struct ReceiveCommand
        {
            std::string name;   // the full ref, as git names it: refs/heads/main
            std::string oldOid; // lowercase hex of one width; all zeros is a create
            std::string newOid; // lowercase hex of one width; all zeros is a delete
        };

        // The command arguments of a v2 receive-pack: each payload is
        // "<old-oid> <new-oid> <name>", the trailing newline not part of it. out
        // carries the commands in the order they came, the ids lowercased. An
        // argument that is not the three fields, an id that is not hex of one of the
        // two widths, a name refs.h would refuse, and one ref named twice are
        // refusals that name which one.
        bool parseCommands(const std::vector<std::string> &pktPayloads,
                           std::vector<ReceiveCommand> &out, std::string &error);

        // -------------------------------------------------------------------- the port
        //
        // What the caller implements so this unit never opens a file. The three
        // object calls are the brief's; the three ref calls are the same leg one
        // story down — a protected branch and a delete are questions about a ref, and
        // the transaction of refs.h is applied through them. writeLoose, commitRefs,
        // and deleteRefs are the port's mutations; receive holds the port as a const
        // reference, the shape of the brief, and takes the constness back at the one
        // place those three stand.

        class RepoAccess
        {
        public:
            virtual ~RepoAccess() = default;

            // True when the id is held. hex is lowercase hex of one width.
            virtual bool hasObject(const std::string &oidHex) const = 0;

            // The object's content — the bytes after the "type size NUL" header — and
            // the type word its header states: "commit", "tree", "blob", "tag".
            virtual bool readObject(const std::string &oidHex, std::string &kindOut,
                                    std::string &contentOut) const = 0;

            // An object written as the repository holds it; oidHex comes back the id
            // of the bytes that were written, whether the file was there or not.
            virtual bool writeLoose(const std::string &kind, const std::string &content,
                                    std::string &oidHex) = 0;

            // The oid the ref holds, lowercase hex; false when there is no such ref.
            virtual bool refValue(const std::string &name, std::string &oidHex) const = 0;

            // The move of concrete refs, refs.h's transaction: all of it or none of
            // it, and a row whose expectation disagrees is refused before a byte moves.
            virtual bool commitRefs(const std::vector<RefUpdate> &updates) = 0;

            // The removal of refs, one name at a time; false when one did not go.
            virtual bool deleteRefs(const std::vector<std::string> &names) = 0;
        };

        // ------------------------------------------------------------------- the policy

        struct ReceivePolicy
        {
            std::vector<std::string> protectedBranches; // full names: "refs/heads/main"
            bool allowNonFastForward = false;           // a move that rewrites history
            // The bytes of one push's body, stated once: the same bound the bare pack
            // indexing keeps to itself, so a push the cap lets through is a push the
            // indexer can stand up in memory without a second number to disagree.
            std::uint64_t maxPackBytes = 64ull * 1024ull * 1024ull;
        };

        // -------------------------------------------------------------------- receive
        //
        // The whole of one receive-pack, in the order the answers need:
        //
        //   the bytes handed are refused when they are over maxPackBytes, before
        //   anything of them is read;
        //   the pack and its index are opened as a pair, and every object in it must
        //   inflate, hash to the id its index names, and name only objects the pack or
        //   the port hold — a pack that references an object neither side holds fails
        //   here, as whole input, so nothing is written and no ref is asked about. A
        //   section that does not open as handed is read again with a closing flush-pkt
        //   taken off, which is how a client ends the section; a section that is that
        //   flush alone carries no pack at all, the delete-only push. A section that
        //   stands neither way keeps the refusal of the bytes as they were handed;
        //   the objects that pass are written through the port before any ref moves,
        //   the way a push keeps what it received even when a ref does not move;
        //   each command is then answered on its own: a path the trees it brings
        //   carries that no repository may hold, an old id that is not what the ref
        //   holds, a protected branch that is not a fast-forward, a move that is not a
        //   fast-forward where the policy did not allow one;
        //   the moves that survived go through the port first, as refs.h's transaction
        //   — all of them or none — and a transaction that does not hold is a failure
        //   of the whole receive: no ref moved, and the results are not the answer;
        //   the removals follow it, one name per call so that the name that stays put
        //   names itself: its own row carries the words, every other row keeps its
        //   answer, and the receive stands.
        //
        // resultsOut carries one row per command, in the order the commands came: the
        // name and the empty string when it moved, the name and the refusal in words
        // when it did not.
        bool receive(const RepoAccess &repo, const std::vector<std::uint8_t> &packBytes,
                     const std::vector<ReceiveCommand> &cmds, const ReceivePolicy &policy,
                     std::vector<std::pair<std::string, std::string>> &resultsOut,
                     std::string &error);

        // The same receive, naming the stage that refused: packFailed comes back true
        // when the refusal came from the body itself — the pack did not open, one of
        // its objects did not stand, the index of a bare pack could not be made, an
        // object it names is held nowhere, or the pack is over the accepted size. That
        // is the only claim a receive answer may lead with "unpack ng"; a policy
        // refusal and a transaction that did not hold are not such a claim.
        bool receive(const RepoAccess &repo, const std::vector<std::uint8_t> &packBytes,
                     const std::vector<ReceiveCommand> &cmds, const ReceivePolicy &policy,
                     std::vector<std::pair<std::string, std::string>> &resultsOut,
                     bool &packFailed, std::string &error);

        // -------------------------------------------------------------- report-status
        //
        // The answer of a receive in v2 report-status. The unpack line reads "unpack
        // ok": a command that refused is one command's refusal, stated as its own ng
        // line, and gitprotocol-pack reserves the unpack-ng line for a body that did
        // not unpack — which this form has no room to claim. Then one line per
        // command, "ok <name>" or "ng <name> <reason>", closed with the flush. A
        // name or a reason that would break the framing of one line is refused with
        // the whole answer left out untouched.
        bool reportStatus(const std::vector<std::pair<std::string, std::string>> &results,
                          std::vector<std::uint8_t> &out, std::string &error);

        // The same answer where the body itself did not unpack: the unpack line reads
        // "unpack ng <reason>", and the per-command ng lines still follow it.
        bool reportStatus(const std::vector<std::pair<std::string, std::string>> &results,
                          const std::string &bodyFailure,
                          std::vector<std::uint8_t> &out, std::string &error);

    } // namespace gitcore
} // namespace ols

#endif // OLS_GITCORE_RECEIVE_H
