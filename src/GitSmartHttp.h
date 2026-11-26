// src/GitSmartHttp.h — the library the smart-HTTP door calls: a repository port
// over the gitcore layers, receive-pack answering a push with report-status,
// upload-pack fetch answering with a locally built pack, the advertised-refs
// helper, and the v0 advertisement the receive-pack door answers with. Nothing
// crosses this surface as an exception: every
// call answers bool, and a refusal says why in words. Bounds, stated where a
// caller can see them: one fetch walks at most 65536 objects across both its
// walks and refuses over the cap rather than answering short; a pushed pack is
// capped by the ReceivePolicy the door hands (its default is 64 MiB, the same
// bound the bare-pack indexer keeps); a sideband
// frame holds kMaxSidebandData payload bytes and the answer chunks itself at that.
#ifndef OLS_GIT_GITSMARTHTTP_H
#define OLS_GIT_GITSMARTHTTP_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The gitcore layers this library stands on: receive.h carries the port shape
// (RepoAccess) and its refs include; wire.h carries the v2 request shape and
// the framing; packfile.h carries the pack writer and the Packfile the port
// may hold open. objects.h comes with them; nothing here throws.
#include "gitcore/receive.h"
#include "gitcore/wire.h"
#include "gitcore/packfile.h"

namespace ols
{
namespace git
{

// The repository the door stands on: a bare repo dir read and written through
// the gitcore objects and refs — objects loose first and the repo's packfiles
// after, refs resolved loose-over-packed. The packfiles open on the first
// object miss and stay open for the port's life: one request, one thread.
// Every other read is fresh. A .pack without its .idx beside it, or one that
// does not open, is left out of the reads — the answer is what the readable
// files state, the same reading rule ProjectGit keeps.
class RepoPort : public ols::gitcore::RepoAccess
{
public:
    explicit RepoPort(std::string repoDir);

    bool hasObject(const std::string &oidHex) const override;
    bool readObject(const std::string &oidHex, std::string &kindOut,
                    std::string &contentOut) const override;
    bool writeLoose(const std::string &kind, const std::string &content,
                    std::string &oidHex) override;
    bool refValue(const std::string &name, std::string &oidHex) const override;
    bool commitRefs(const std::vector<ols::gitcore::RefUpdate> &updates) override;
    bool deleteRefs(const std::vector<std::string> &names) override;

private:
    std::string dir_;
    mutable std::vector<std::unique_ptr<ols::gitcore::Packfile>> packs_;
    mutable bool packsOpened_ = false;
    void ensurePacks() const;
};

// The whole of one push: the bytes are the request as git sends it — either
// dialect the push has: v0's command lines, whose first carries the
// capabilities after a NUL, or v2's "command=receive-pack" shape; both end at
// a flush-pkt, and the pack followed by its index stands after it. A v0 client
// may put its service marker — "# service=git-receive-pack" and a flush-pkt —
// in front of the commands, and the request is read the same with it and
// without it. The dialect is read from the body — a first command line with a
// NUL is v0 — and in either case the commands are read as gitcore's commands.
// The answer that comes back is always bytes when the answer is: on true it
// is the report-status of the receive — "unpack ok" or the ng lines, the
// flush — through gitcore's own report writer; and when the command section
// named side-band-64k among its capabilities, that whole report rides
// channel 1 in sideband frames, closed by the empty channel-2 frame and the
// flush. On false the pack or the request did not stand: the answer then
// carries either the ng lines naming why for every command the request did
// parse, or a fatal sideband line when none parsed; words names the reason
// either way. Nothing of a push whose pack was refused moved.
bool handleReceivePack(const std::string &repoDir,
                       const std::vector<std::uint8_t> &requestBytes,
                       const std::vector<std::string> &protectedBranches,
                       std::vector<std::uint8_t> &answerOut,
                       std::string &words);

// The v2 fetch command of upload-pack, already parsed to a V2Request by the
// door: wants and haves are read with gitcore's parser, and the answer is
// built in one round — the pack the local builder makes for the objects the
// haves do not already reach, sideband-framed, behind an "OK" line and
// closed with the flush: the ackless v2 fetch-ok shape. bodyRest is what
// stood after the request's flush; an upload-pack fetch sends nothing there,
// and bytes that do are a refusal. On false the pack or the request did not
// stand, answerOut carries a fatal sideband line, and words names the reason
// — including "pack builder incomplete: ..." if the pack writer cannot
// produce a pack that reads back as valid; a malformed pack is never sent.
bool handleUploadPackFetch(const std::string &repoDir,
                           const ols::gitcore::V2Request &request,
                           const std::vector<std::uint8_t> &bodyRest,
                           std::vector<std::uint8_t> &answerOut,
                           std::string &words);

// The ls-refs lines of a repo as plain payload strings — "<oid> <name>", a
// resolved symref carrying its "symref-target:" — for a later door to frame
// through wire.h; the request's options (unborn, peel) are the door's to
// apply, this states what the repository resolves. False means the refs
// could not be read.
bool advertisedRefs(const std::string &repoDir, std::vector<std::string> &linesOut);

// The v0 capability advertisement a receive-pack door answers with, already
// framed: the "# service=git-receive-pack" marker line and its flush, then one
// ref line per ref — HEAD first, the capability list after the NUL of the
// first line, every other line without one — closed with the flush that ends
// the advertisement. A HEAD that resolves nowhere is stated with the all-zeros
// id, the way an unborn repository is stated; any other ref with no id states
// nothing and is left out. The capability list is the fixed one this door
// answers with: report-status, delete-refs, side-band-64k, quiet, ofs-delta,
// and the agent; "atomic" is not in it, because a refusal here states that no
// ref moved rather than that the others stand. The push door answers a client
// that names the band behind the band. False names the reason the refs could
// not be read.
bool receivePackAdvertisement(const std::string &repoDir,
                              std::vector<std::uint8_t> &bytesOut,
                              std::string &words);

} // namespace git
} // namespace ols

#endif // OLS_GIT_GITSMARTHTTP_H
