#pragma once
// The ssh road out of the studio's fetch doors (E2, spec §10). The seam is
// ols::http::HttpFetch: an address that names a host over ssh rides a spawned
// vendor\openssh\ssh.exe running git-upload-pack on the far side — the GET
// reads its advertisement from the child's stdout, the POST writes its
// request to stdin and reads the pack to EOF. Every ask opens its own child:
// smart-http v0 rides this exactly; a server that answers protocol v2 is
// refused by these same honest readings, which say what came back was not a
// v0 advertisement.
//
// Nothing here leans on an account's ambient identity (spec §10.1): the spawn
// names -F at an empty config, -i at one key file, and UserKnownHostsFile at
// the studio's own known_hosts under the data dir. The pinned release quotes
// itself (quoteWindowsArg below), so a repository path wearing spaces arrives
// as one word with no shell ever in between.
#include <cstdint>
#include <string>
#include <vector>
#include "HttpFetch.h"
namespace ols
{
    namespace ssh
    {
        // What an address is, to the doors: not-ssh (another road), a full
        // ssh:// form, or the scp-like host:path form git also accepts.
        enum class UrlKind
        {
            NotSsh,
            SshUrl,
            ScpLike
        };
        struct SshTarget
        {
            UrlKind kind = UrlKind::NotSsh;
            std::string user; // empty means ssh's own to fill
            std::string host;
            unsigned port = 0u; // zero names no port: ssh's default stands
            std::string path; // ssh:// keeps its leading slash (absolute); scp-like never carried one
        };
        // Read an address into its parts. False with words when the form is
        // named but wrong (a bad port, an empty path); NotSsh kind, true, for
        // everything that is simply another road. Control bytes are refused —
        // nothing here reaches a command line unchecked.
        bool classifySshUrl(const std::string &url, SshTarget &out, std::string &error);
        // The one question the fetch doors ask a url before choosing its road:
        // is this an ssh address at all. True with the readings when yes; false
        // and quiet (no error) for every other form — an http url is not an
        // error, it is just another road.
        bool looksSsh(const std::string &url, SshTarget &out);
        // One word put on a Windows command line: quoted when it holds a space
        // or a tab or is empty, with every run of n backslashes before a quote
        // doubled to 2n+1 — the rules CommandLineToArgvW reads them back by.
        std::string quoteWindowsArg(const std::string &word);
        // The vendored OpenSSH folder: OLS_SSH_DIR when named, else
        // <root>/vendor/openssh beside the tree the server stands in. True with
        // the folder when ssh.exe is really there; words naming what is missing
        // when not (run.bat fetches it — the studio never leans on an OS ssh).
        bool sshBinDir(const std::string &root, std::string &dirOut, std::string &error);
        // The key room under the data dir, and the known_hosts that stands in
        // it. Plain paths: the doors to come (E2's next batch) put keys here;
        // this batch only reads them.
        inline std::string keysRoom(const std::string &dataDir)
        {
            return dataDir + "/gitkeys";
        }
        inline std::string knownHostsPath(const std::string &dataDir)
        {
            return keysRoom(dataDir) + "/known_hosts";
        }
        // The empty ssh config the spawn reads with -F: a file of nothing but
        // a comment, made in dir if absent. Written once, read forever.
        bool ensureEmptyConfig(const std::string &dir, std::string &pathOut, std::string &error);
        // The pinned release's other two hands, beside a given ssh.exe (or the
        // folder OLS_SSH_DIR names when none is handed): keygen makes deploy
        // keys, keyscan reads host keys. Words when no such binary stands.
        bool sshKeygenFor(const std::string &sshExe, std::string &pathOut, std::string &error);
        bool sshKeyscanFor(const std::string &sshExe, std::string &pathOut, std::string &error);
        // Make one ed25519 pair in the key room under `name` (chars checked; a
        // name that already holds a key is refused — the studio never replaces
        // one). The public half and its fingerprint come back. On platforms
        // without the pinned build yet (the Linux side, run.sh names it) this
        // refuses honestly in words.
        bool generateDeployKey(const std::string &dataDir, const std::string &name,
                               std::string &outPublic, std::string &outFingerprint,
                               std::string &error);
        // One host trusted: read known_hosts first and never touch a standing
        // entry (the hard wall — changed keys stay changed for a human), else
        // ssh-keyscan the host and append what it answered. Words when nothing
        // came back. Never overwrites, never removes.
        bool trustHost(const std::string &dataDir, const std::string &host,
                       std::string &addedLine, std::string &error);
        // The spawn line the transport runs, built from the readings: the
        // argument spine SPIKE_REPORT.md §4 proved (empty -F config,
        // BatchMode, IdentitiesOnly, ConnectTimeout, accept-new, absolute
        // UserKnownHostsFile, one -i key, optional -p) and the upload-pack
        // word quoted as one piece. Exposed for the tests to pin.
        std::string sshSpawnLine(const std::string &sshExe, const SshTarget &target,
                                 const std::string &emptyConfig, const std::string &knownHosts,
                                 const std::string &keyPath, unsigned connectTimeoutMs);
        // Run one vendored binary with three pipes (CreatePipe x3,
        // STARTF_USESTDHANDLES): stdout and stderr read apart to EOF, exit
        // code kept. The transport rides this; so do keygen and keyscan; the
        // tests pin it against any exe. Words when the child would not start.
        bool runVendored(const std::string &exePath, const std::vector<std::string> &args,
                         std::string &stdoutOut, std::string &stderrOut,
                         unsigned long &exitCodeOut, std::string &error);
        // What a road needs to stand on: one key the studio holds, and the
        // known_hosts that vouches for hosts. A fetch without both is refused
        // before any child starts — no ambient identity, ever (spec §10.1).
        struct SshIdentity
        {
            std::string keyPath;
            std::string knownHosts;
            int connectTimeoutMs = 10000;
        };
        // The transport: HttpFetch's request() answered by a spawned ssh.exe
        // running git-upload-pack. GET reads the advertisement (exit 0 answers
        // 200); POST writes its body to stdin and reads the pack to EOF; an
        // exit that is not zero answers with the child's stderr words verbatim,
        // prefixed "the ssh road refused: ". One child per ask.
        class SshFetch : public ols::http::HttpFetch
        {
        public:
            SshFetch(const SshTarget &target, const SshIdentity &identity,
                     const std::string &sshExe);
            ~SshFetch() override;
            bool request(const std::string &method, const std::string &url,
                         const ols::http::Headers &headers, const std::string &body,
                         std::uint64_t maxReplyBytes, ols::http::Reply &replyOut,
                         std::string &error) override;
        private:
            struct Child;
            Child *child_ = nullptr;
            bool openChild(std::string &error);
            void closeChild();
            // The ctor stores these so every later ask (openChild, request)
            // reads the same address, key, and pinned binary it was built on.
            SshTarget target_;
            SshIdentity identity_;
            std::string sshExe_;
        };
    } // namespace ssh
} // namespace ols
