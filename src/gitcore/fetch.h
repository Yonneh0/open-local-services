// src/gitcore/fetch.h — the client driver of an outbound fetch: the words a
// caller asks with, and the road the bytes and the repository take.
//
// This unit opens no socket and holds no repository. The bytes travel through
// the seam HttpFetch.h states, so a test can hand the whole conversation as
// canned answers; the repository is reached through the port receive.h states
// (receive.h:77-104) plus the one HEAD move the client adds, and git's
// RepoPort is a hair's breadth from answering both. What this unit owns is
// the order: advertisement, request, answer, pack stood up, objects written —
// every one of them before any ref is asked about, the same order a receive
// keeps — and then one ref transaction with HEAD brought to the branch the
// advertisement named.
//
// No call in this namespace throws. A transport that does not answer, an
// advertisement that does not read, a want the advertisement does not carry, a
// pack that does not stand, and a transaction that does not hold are all
// false with the reason in the words.
#ifndef OLS_GITCORE_FETCH_H
#define OLS_GITCORE_FETCH_H

#include <cstdint>
#include <string>
#include <vector>

#include "gitcore/receive.h" // the port: RepoAccess, and RefUpdate with its include
#include "HttpFetch.h"

namespace ols
{
    namespace gitcore
    {

        // The cap on one fetch's pack: the same bound the receive door keeps
        // (receive.h:115, receive.cpp:56), so one number rules both halves of
        // the conversation. A pack past it is refused before a byte of it is
        // stood up.
        constexpr std::uint64_t kMaxFetchPackBytes = 64ull * 1024ull * 1024ull;

        // The cap on the whole answer the transport may hand back: the pack
        // bound beside room for the sideband and HTTP framing around it. The
        // frames cost bytes of their own even when the pack fits, and the
        // reader refuses past this before the body is held.
        constexpr std::uint64_t kMaxFetchAnswerBytes = kMaxFetchPackBytes + 1ull * 1024ull * 1024ull;

        // One advertised ref, read from either dialect. The oid is lowercase
        // hex; a ref the advertisement states with the id of nothing (an
        // unborn HEAD) says so and carries the target it points through
        // where the dialect states one.
        struct AdvertisedRef
        {
            std::string name;
            std::string oid;
            bool unborn = false;
            std::string symrefTarget;
        };

        // What a fetch asks of the repository it fetches into: receive's port
        // for the objects and the refs — the same road a push lands on — and
        // the one move a client adds: HEAD to the branch the advertisement
        // named. The concrete answer is a RepoPort over a repo dir and
        // refs.h's setHeadSymref behind it (refs.h:238).
        class FetchTarget
        {
        public:
            virtual ~FetchTarget() = default;

            // The objects and refs of the repository, as receive.h states them.
            virtual RepoAccess &repo() = 0;

            // HEAD becomes a symref to the named branch; refs.h's own move
            // (refs.h:238), and false when it does not go.
            virtual bool setHeadSymref(const std::string &target) = 0;
        };

        // The refs one url advertises. The GET goes to
        // "<url>/info/refs?service=git-upload-pack"; a v2 advertisement is
        // followed by one ls-refs command to the same service, so its refs —
        // peeled, with the symref target of HEAD — come back whole; a v0
        // advertisement carries its refs itself. The X-Api-Key header travels
        // with every request when the caller hands a key, which is how one
        // open-local-services asks another (the PeerService.cpp:241 shape).
        // The entries come back in the order they were advertised, HEAD
        // among them when the advertisement states one.
        bool lsRemote(ols::http::HttpFetch &http, const std::string &url,
                      const std::string &apiKey, std::vector<AdvertisedRef> &refsOut,
                      std::string &outWords);

        // The fetch itself: the advertisement, one request for the refs named
        // (v2 wants/haves/done on the v2 road, the v0 request builder's bytes
        // on the v0 one), the answer through the sideband demux, the pack
        // stood up and its objects written through the port — and only then
        // the refs: one commitRefs transaction (refs.h:254) moving each named
        // ref to the oid the advertisement stated, then HEAD set to the
        // branch HEAD points through when that branch moved with this fetch.
        // The haves are the advertised tips the port already holds. movedOut
        // carries the refs whose value this call changed; an empty packfile
        // section — the answer to a client that already holds every want —
        // still moves the refs. Nothing is written and no ref is asked about
        // when the answer carries a fatal or a pack that does not stand.
        bool fetchRepo(FetchTarget &target, ols::http::HttpFetch &http, const std::string &url,
                       const std::vector<std::string> &refsToFetch, const std::string &apiKey,
                       std::vector<std::string> &movedOut, std::string &outWords);

    } // namespace gitcore
} // namespace ols

#endif // OLS_GITCORE_FETCH_H
