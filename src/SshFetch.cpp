// The ssh road's address readings, its bin room, and the quoting the pinned
// release needs (E2 batch one). The transport itself — pipes, keys, the class
// — arrives in the next batch; this file stands on its own and compiles
// without any of it.
#include "SshFetch.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace ols
{
    namespace ssh
    {
        namespace
        {
            // Plain prefix test — the doors ask "does this address open with
            // ssh://" before they parse anything, and that question is asked
            // more than once, so it gets its own honest name.
            bool hasPrefix(const std::string &s, const std::string &p)
            {
                return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
            }

            // Case-insensitive compare at a spot: ssh:// and SCP-like may be
            // spelled any way a human types it; the rest of the address is
            // read exactly, so only the scheme word is folded.
            bool equalsInsensitiveAt(const std::string &s, std::size_t at, const std::string &word)
            {
                if (at + word.size() > s.size())
                {
                    return false;
                }
                for (std::size_t i = 0u; i < word.size(); ++i)
                {
                    const unsigned char a = static_cast<unsigned char>(s[at + i]);
                    const unsigned char b = static_cast<unsigned char>(word[i]);
                    const unsigned char la = (a >= 'A' && a <= 'Z') ? static_cast<unsigned char>(a + 32u) : a;
                    const unsigned char lb = (b >= 'A' && b <= 'Z') ? static_cast<unsigned char>(b + 32u) : b;
                    if (la != lb)
                    {
                        return false;
                    }
                }
                return true;
            }

            // The characters a host name and a key name are made of. Names
            // are read from the address straight onto a command line, so
            // anything outside this small alphabet stops the reading rather
            // than riding through unchecked.
            bool nameChar(unsigned char c)
            {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '.' || c == '-' || c == '_';
            }

            // The characters a path may wear: printable, no control bytes.
            // Control bytes are refused at the door — nothing here reaches a
            // command line unchecked (a tab in an address would split the
            // word the spawn runs).
            bool pathCharOk(unsigned char c)
            {
                return c >= 0x20u && c != 0x7fu;
            }
        } // anonymous

        // The rules CommandLineToArgvW reads the command line back by: a
        // word is wrapped in quotes when it needs them (holds a space, a
        // tab, an empty word), and inside the quotes every run of n
        // backslashes before a quote doubles to 2n plus one so the quote
        // itself reaches argv literally; a run of backslashes with no quote
        // after it stands as it is.
        std::string quoteWindowsArg(const std::string &word)
        {
            std::string out;
            bool needsQuotes = word.empty();
            for (char c : word)
            {
                if (c == ' ' || c == '\t' || c == '"')
                {
                    needsQuotes = true;
                    break;
                }
            }
            if (!needsQuotes)
            {
                return word;
            }
            out.push_back('"');
            std::size_t backslashes = 0u;
            for (char c : word)
            {
                if (c == '\\')
                {
                    ++backslashes;
                    continue;
                }
                if (c == '"')
                {
                    out.append(backslashes * 2u + 1u, '\\');
                    out.push_back('"');
                }
                else
                {
                    out.append(backslashes, '\\');
                    backslashes = 0u;
                    out.push_back(c);
                }
            }
            out.append(backslashes * 2u, '\\'); // the run closing the word takes the quote with it
            out.push_back('"');
            return out;
        }

        // Read an address into its parts. False with words when the form is
        // named but wrong (a bad port, an empty path); NotSsh kind, true, for
        // everything that is simply another road. Control bytes are refused —
        // nothing here reaches a command line unchecked.
        bool classifySshUrl(const std::string &url, SshTarget &out, std::string &error)
        {
            out = SshTarget{};
            error.clear();
            if (url.empty())
            {
                error = "the address is nothing to fetch";
                return false;
            }
            for (char c : url)
            {
                if (!pathCharOk(static_cast<unsigned char>(c)))
                {
                    error = "the address carries a control byte; nothing reaches a command line unchecked";
                    return false;
                }
            }

            if (equalsInsensitiveAt(url, 0u, "ssh://"))
            {
                out.kind = UrlKind::SshUrl;
                const std::size_t body0 = 6u;
                // The path is everything from the first '/' on; it keeps its
                // leading slash because the ssh:// form states an absolute
                // repository path on the far side.
                const std::size_t slash = url.find('/', body0);
                const std::string authority =
                    slash == std::string::npos ? url.substr(body0) : url.substr(body0, slash - body0);
                const std::string path =
                    slash == std::string::npos ? std::string() : url.substr(slash);

                std::string head = authority;
                const std::size_t at = head.find('@');
                if (at != std::string::npos)
                {
                    out.user = head.substr(0u, at);
                    head = head.substr(at + 1u);
                    for (char c : out.user)
                    {
                        if (!nameChar(static_cast<unsigned char>(c)))
                        {
                            error = "the user of an ssh address wears a character no name may wear";
                            return false;
                        }
                    }
                }
                // The port rides the host word: host:port, digits only,
                // nonzero, under the ssh ceiling. Bad port words refuse — a
                // named form that is wrong is an error, not another road.
                std::string host = head;
                const std::size_t colon = host.find(':');
                if (colon != std::string::npos)
                {
                    const std::string portWord = host.substr(colon + 1u);
                    host = host.substr(0u, colon);
                    if (portWord.empty())
                    {
                        error = "the ssh address names a port with no number on it";
                        return false;
                    }
                    unsigned long port = 0ul;
                    for (char c : portWord)
                    {
                        if (c < '0' || c > '9')
                        {
                            error = "the port of an ssh address is not a number: " + portWord;
                            return false;
                        }
                        port = port * 10ul + static_cast<unsigned long>(c - '0');
                        if (port > 65534ul)
                        {
                            error = "the port of an ssh address rides past what ssh answers";
                            return false;
                        }
                    }
                    if (port == 0ul)
                    {
                        error = "the port of an ssh address is zero, which names no port";
                        return false;
                    }
                    out.port = static_cast<unsigned>(port);
                }
                if (host.empty())
                {
                    error = "the ssh address names no host";
                    return false;
                }
                for (char c : host)
                {
                    if (!nameChar(static_cast<unsigned char>(c)))
                    {
                        error = "the host of an ssh address wears a character no host may wear";
                        return false;
                    }
                }
                if (path.empty())
                {
                    error = "the ssh address names no repository path";
                    return false;
                }
                out.host = host;
                out.path = path; // the leading slash stands: absolute on the far side
                return true;
            }

            // No scheme: the scp-like [user@]host:path form git also accepts.
            // The colon is what makes it an address at all; without one this
            // is simply another road.
            const std::size_t colon = url.find(':');
            if (colon == std::string::npos)
            {
                out.kind = UrlKind::NotSsh;
                return true;
            }
            const std::string head = url.substr(0u, colon);
            const std::string path = url.substr(colon + 1u);
            // A single letter before the colon is a drive, not a host:
            // "C:\repos\x.git" names a folder on this machine.
            if (head.size() == 1u &&
                ((head[0] >= 'a' && head[0] <= 'z') || (head[0] >= 'A' && head[0] <= 'Z')))
            {
                error = "a single letter before the colon names a drive, not a host: " + url;
                return false;
            }
            if (path.empty())
            {
                error = "the address ends at the colon: no repository path follows";
                return false;
            }
            const bool hasLogin = head.find('@') != std::string::npos;
            // A head that opens with '/' is a path on this machine, and a
            // bare "word:/path" with no login reads as a scheme on another
            // road (http:/…, file:/…) — not an ssh address. A login vouches
            // for the host, so user@host:/abs/path stays scp-like and its
            // path keeps its slash.
            if (!hasLogin && (head.empty() || head[0] == '/' ||
                              (path[0] == '/' && head.find('.') == std::string::npos)))
            {
                out.kind = UrlKind::NotSsh;
                return true;
            }
            std::string host = head;
            const std::size_t at = host.find('@');
            if (at != std::string::npos)
            {
                out.user = host.substr(0u, at);
                host = host.substr(at + 1u);
                for (char c : out.user)
                {
                    if (!nameChar(static_cast<unsigned char>(c)))
                    {
                        error = "the user of an address wears a character no name may wear";
                        return false;
                    }
                }
            }
            if (host.empty())
            {
                error = "the address names no host before the colon";
                return false;
            }
            for (char c : host)
            {
                if (!nameChar(static_cast<unsigned char>(c)))
                {
                    error = "the host of an address wears a character no host may wear";
                    return false;
                }
            }
            out.kind = UrlKind::ScpLike;
            out.host = host;
            out.path = path; // scp-like paths are host-relative; a login may still vouch for an absolute one
            return true;
        }

        // The one question the fetch doors ask a url before choosing its road.
        // True for both ssh kinds; and true for any url carrying the ssh://
        // prefix even when the reading refused it — a named-but-broken ssh
        // address still belongs to the ssh road, and its words say what is
        // wrong. Everything else is quiet: an http url is another road, not
        // an error.
        bool looksSsh(const std::string &url, SshTarget &out)
        {
            std::string error;
            const bool ok = classifySshUrl(url, out, error);
            if (out.kind == UrlKind::SshUrl || out.kind == UrlKind::ScpLike)
            {
                return true;
            }
            if (equalsInsensitiveAt(url, 0u, "ssh://"))
            {
                // The road stays ssh even though the reading refused: the
                // caller's words will carry what was wrong.
                out.kind = UrlKind::SshUrl;
                return true;
            }
            return false;
        }

        // The vendored OpenSSH folder: OLS_SSH_DIR when named, else
        // <root>/vendor/openssh beside the tree the server stands in. True
        // with the folder only when ssh.exe really stands directly in it —
        // the pinned zip unpacks flat (--strip-components=1, spike-proven),
        // so a nested folder is not a bin room.
        bool sshBinDir(const std::string &root, std::string &dirOut, std::string &error)
        {
            dirOut.clear();
            error.clear();
            std::string dir;
            const char *env = std::getenv("OLS_SSH_DIR");
            if (env && *env)
            {
                dir = env;
            }
            else
            {
                if (root.empty())
                {
                    error = "no root was named to find the vendored OpenSSH beside";
                    return false;
                }
                dir = root + "/vendor/openssh";
            }
            std::error_code ec;
            const std::filesystem::path binDir(dir);
            if (!binDir.empty() && std::filesystem::is_regular_file(binDir / "ssh.exe", ec))
            {
                dirOut = dir;
                return true;
            }
            error = "no vendored ssh.exe stands at " + dir +
                    ": run the build script to take the pinned OpenSSH release";
            return false;
        }

        namespace
        {
            // The pinned release's siblings stand beside the ssh.exe it came
            // with — or beside the folder OLS_SSH_DIR names when none is
            // handed. Words when no such binary stands: the studio never
            // leans on an OS-installed keygen or keyscan.
            bool siblingBin(const std::string &sshExe, const std::string &name,
                            std::string &out, std::string &error)
            {
                out.clear();
                error.clear();
                std::string dir;
                const char *env = std::getenv("OLS_SSH_DIR");
                if (!sshExe.empty())
                {
                    const std::filesystem::path p(sshExe);
                    const std::filesystem::path parent = p.parent_path();
                    dir = parent.string();
                }
                else if (env && *env)
                {
                    dir = env;
                }
                if (dir.empty())
                {
                    error = "no pinned binary and no OLS_SSH_DIR: the sibling " + name + " cannot be found";
                    return false;
                }
                const std::filesystem::path candidate = std::filesystem::path(dir) / name;
                std::error_code ec;
                if (std::filesystem::is_regular_file(candidate, ec))
                {
                    out = candidate.string();
                    return true;
                }
                error = "no " + name + " stands beside " + dir +
                        ": the pinned OpenSSH release carries it; run the build script";
                return false;
            }
        } // anonymous

        // The pinned release's other two hands: keygen makes deploy keys,
        // keyscan reads host keys.
        bool sshKeygenFor(const std::string &sshExe, std::string &pathOut, std::string &error)
        {
            return siblingBin(sshExe, "ssh-keygen.exe", pathOut, error);
        }

        bool sshKeyscanFor(const std::string &sshExe, std::string &pathOut, std::string &error)
        {
            return siblingBin(sshExe, "ssh-keyscan.exe", pathOut, error);
        }

        // The empty ssh config the spawn reads with -F: a file of nothing but
        // a comment, made in dir if absent. Written once, read forever — the
        // studio never reads an account's own config.
        bool ensureEmptyConfig(const std::string &dir, std::string &pathOut, std::string &error)
        {
            pathOut.clear();
            error.clear();
            if (dir.empty())
            {
                error = "no dir was named to make the empty ssh config in";
                return false;
            }
            const std::filesystem::path path = std::filesystem::path(dir) / "ols-empty-config";
            std::error_code ec;
            if (std::filesystem::is_regular_file(path, ec))
            {
                pathOut = path.string();
                return true;
            }
            std::ofstream out(path);
            if (!out)
            {
                error = "the empty ssh config cannot be written at " + path.string();
                return false;
            }
            out << "# an empty ssh config: the studio reads no account's own\n";
            out.close();
            if (!out)
            {
                error = "the empty ssh config could not be closed at " + path.string();
                return false;
            }
            pathOut = path.string();
            return true;
        }
    } // namespace ssh
} // namespace ols
