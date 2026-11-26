// The ssh road's address readings, its bin room, and the quoting the pinned
// release needs (E2 batch one). The transport itself — pipes, keys, the class
// — arrives in the next batch; this file stands on its own and compiles
// without any of it.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#pragma comment(lib, "onecore.lib")
// The desktop CreatePipe the blueprint calls: this SDK's headers carry only a
// two-argument flavor under the plain name, so the classic three-argument form
// is named here once - at global scope with C linkage (inside ols::ssh the
// compiler mangles it as our own function and the link asks for a symbol no
// library ever had), and the linker is told to answer that import's name with
// kernel32's own thunk. Proven on this machine: the pipe comes up inheritable.
extern "C" __declspec(dllimport) BOOL WINAPI CreatePipeClassic(HANDLE *, HANDLE *, LPSECURITY_ATTRIBUTES);
#pragma comment(linker, "/alternatename:__imp_CreatePipeClassic=__imp_CreatePipe")
#endif
#include "SshFetch.h"
#include <cstdlib>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
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

#ifdef _WIN32
        namespace
        {
            // Words that come off a pipe end in the child's newlines; the
            // doors read them as one line, so the tail is trimmed once, here.
            std::string trimTail(const std::string &s)
            {
                std::size_t n = s.size();
                while (n > 0u && (s[n - 1u] == '\r' || s[n - 1u] == '\n' || s[n - 1u] == ' ' || s[n - 1u] == '\t'))
                {
                    --n;
                }
                return s.substr(0u, n);
            }
        } // anonymous

        // The vendored hands: three pipes, a child that never sees a console,
        // stdout and stderr read apart to EOF, the exit code kept. The
        // transport rides this; so do keygen and keyscan; the tests pin it
        // against any exe. Words when the child would not start.
        bool runVendored(const std::string &exePath, const std::vector<std::string> &args,
                         std::string &stdoutOut, std::string &stderrOut,
                         unsigned long &exitCodeOut, std::string &error)
        {
            stdoutOut.clear();
            stderrOut.clear();
            exitCodeOut = 0u;
            error.clear();
            if (exePath.empty())
            {
                error = "no binary was named to run";
                return false;
            }
            std::string line = quoteWindowsArg(exePath);
            for (const std::string &arg : args)
            {
                line += " " + quoteWindowsArg(arg);
            }
            SECURITY_ATTRIBUTES inheritable;
            inheritable.nLength = sizeof(SECURITY_ATTRIBUTES);
            inheritable.lpSecurityDescriptor = nullptr;
            inheritable.bInheritHandle = TRUE;
            HANDLE inRead = nullptr, inWrite = nullptr;
            HANDLE outRead = nullptr, outWrite = nullptr;
            HANDLE errRead = nullptr, errWrite = nullptr;
            if (!CreatePipeClassic(&inRead, &inWrite, &inheritable))
            {
                error = "the pipes for " + exePath + " could not be opened: win32 code " + std::to_string(GetLastError());
                return false;
            }
            if (!CreatePipeClassic(&outRead, &outWrite, &inheritable))
            {
                error = "the pipes for " + exePath + " could not be opened: win32 code " + std::to_string(GetLastError());
                CloseHandle(inRead);
                CloseHandle(inWrite);
                return false;
            }
            if (!CreatePipeClassic(&errRead, &errWrite, &inheritable))
            {
                error = "the pipes for " + exePath + " could not be opened: win32 code " + std::to_string(GetLastError());
                CloseHandle(inRead);
                CloseHandle(inWrite);
                CloseHandle(outRead);
                CloseHandle(outWrite);
                return false;
            }
            // The parent's own read ends must not ride into the child: two
            // readers on one pipe is the hang shape the probe measured.
            SetHandleInformation(inRead, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOA startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = inWrite;
            startup.hStdOutput = outWrite;
            startup.hStdError = errWrite;
            PROCESS_INFORMATION info{};
            std::vector<char> cmdBuf(line.begin(), line.end());
            cmdBuf.push_back('\0');
            if (CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &info) == 0u)
            {
                const DWORD code = GetLastError();
                error = exePath + " would not start: win32 code " + std::to_string(code);
                CloseHandle(inRead);
                CloseHandle(inWrite);
                CloseHandle(outRead);
                CloseHandle(outWrite);
                CloseHandle(errRead);
                CloseHandle(errWrite);
                return false;
            }
            // The child's copies close right away: a child that reads stdin
            // sees EOF, which is what keeps a bounded run bounded.
            CloseHandle(inWrite);
            CloseHandle(outWrite);
            CloseHandle(errWrite);
            char buf[4096];
            for (;;)
            {
                DWORD got = 0u;
                if (!ReadFile(outRead, buf, sizeof(buf), &got, nullptr))
                {
                    break;
                }
                if (got == 0u)
                {
                    break;
                }
                stdoutOut.append(buf, buf + got);
            }
            for (;;)
            {
                DWORD got = 0u;
                if (!ReadFile(errRead, buf, sizeof(buf), &got, nullptr))
                {
                    break;
                }
                if (got == 0u)
                {
                    break;
                }
                stderrOut.append(buf, buf + got);
            }
            WaitForSingleObject(info.hProcess, INFINITE);
            DWORD exitCode = 0u;
            GetExitCodeProcess(info.hProcess, &exitCode);
            exitCodeOut = static_cast<unsigned long>(exitCode);
            CloseHandle(info.hProcess);
            CloseHandle(inRead);
            CloseHandle(outRead);
            CloseHandle(errRead);
            return true;
        }

        // One ed25519 pair, made once in the key room. The studio never
        // replaces a standing key — the name is the door's, and a second
        // ask over it is a human's decision.
        bool generateDeployKey(const std::string &dataDir, const std::string &name,
                               std::string &outPublic, std::string &outFingerprint,
                               std::string &error)
        {
            outPublic.clear();
            outFingerprint.clear();
            error.clear();
            if (dataDir.empty())
            {
                error = "no data dir was named to make the key in";
                return false;
            }
            if (name.empty())
            {
                error = "no key name was named";
                return false;
            }
            for (unsigned char c : name)
            {
                if (!nameChar(c))
                {
                    error = "the key name " + name + " wears a character outside the small alphabet";
                    return false;
                }
            }
            const std::string room = keysRoom(dataDir);
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(room), ec);
            const std::string keyPath = room + "/" + name;
            if (std::filesystem::is_regular_file(keyPath, ec))
            {
                error = "a key already stands named " + name + "; the studio never replaces one";
                return false;
            }
            std::string keygen;
            if (!sshKeygenFor(std::string(), keygen, error))
            {
                return false;
            }
            const std::vector<std::string> args{
                "-q", "-t", "ed25519", "-N", "", "-C", "open-local-services:" + name, "-f", keyPath};
            std::string out;
            std::string errWords;
            unsigned long code = 0u;
            if (!runVendored(keygen, args, out, errWords, code, error))
            {
                return false;
            }
            if (code != 0u)
            {
                error = "the key " + name + " was not made: " + trimTail(errWords);
                return false;
            }
            const std::string pubPath = keyPath + ".pub";
            std::ifstream pub(pubPath, std::ios::binary);
            if (!pub)
            {
                error = "the public half of " + name + " did not come back";
                return false;
            }
            pub.seekg(0, std::ios::end);
            const std::streampos n = pub.tellg();
            outPublic.assign(static_cast<std::size_t>(n < 0 ? 0 : static_cast<std::streamoff>(n)), '\0');
            pub.seekg(0, std::ios::beg);
            if (!outPublic.empty())
            {
                pub.read(&outPublic[0], static_cast<std::streamsize>(outPublic.size()));
            }
            pub.close();
            // The fingerprint is a courtesy: keygen's -lf ask failing leaves
            // the key made, not refused.
            const std::vector<std::string> fpArgs{"-lf", pubPath};
            std::string fpOut;
            std::string fpErr;
            unsigned long fpCode = 0u;
            std::string fpError;
            if (runVendored(keygen, fpArgs, fpOut, fpErr, fpCode, fpError) && fpCode == 0u)
            {
                outFingerprint = trimTail(fpOut);
            }
            return true;
        }

        // One host trusted. known_hosts is read first and never touched: a
        // standing entry stays standing for a human, not for this door.
        bool trustHost(const std::string &dataDir, const std::string &host,
                       std::string &addedLine, std::string &error)
        {
            addedLine.clear();
            error.clear();
            if (dataDir.empty())
            {
                error = "no data dir was named to trust the host in";
                return false;
            }
            if (host.empty())
            {
                error = "no host was named to trust";
                return false;
            }
            const std::string knownHosts = knownHostsPath(dataDir);
            std::ifstream in(knownHosts);
            std::string line;
            while (std::getline(in, line))
            {
                std::string first;
                for (char c : line)
                {
                    if (c == ' ' || c == '\t')
                    {
                        break;
                    }
                    first.push_back(c);
                }
                if (first == host)
                {
                    return true;
                }
                if (first.size() > host.size() + 2u && first[0] == '[' &&
                    first.compare(1u, host.size(), host) == 0 && first[1u + host.size()] == ':')
                {
                    return true;
                }
            }
            in.close();
            std::string keyscan;
            if (!sshKeyscanFor(std::string(), keyscan, error))
            {
                return false;
            }
            const std::vector<std::string> args{host};
            std::string out;
            std::string errWords;
            unsigned long code = 0u;
            if (!runVendored(keyscan, args, out, errWords, code, error))
            {
                return false;
            }
            std::vector<std::string> keys;
            std::istringstream lines(out);
            std::string candidate;
            while (std::getline(lines, candidate))
            {
                if (candidate.empty() || candidate[0] == '#')
                {
                    continue;
                }
                keys.push_back(candidate);
            }
            if (keys.empty())
            {
                error = "the host " + host + " answered no key to scan";
                return false;
            }
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(keysRoom(dataDir)), ec);
            std::ofstream app(knownHosts, std::ios::app);
            if (!app)
            {
                error = "the known_hosts at " + knownHosts + " cannot be written to";
                return false;
            }
            for (const std::string &key : keys)
            {
                app << key << "\n";
            }
            app << std::flush;
            if (!app)
            {
                error = "the known_hosts at " + knownHosts + " could not be appended to";
                return false;
            }
            app.close();
            addedLine = keys.front();
            return true;
        }

        // The spawn line the transport runs: the argument spine SPIKE_REPORT.md
        // §4 proved — an empty -F config so no account's own is read, BatchMode
        // so no prompt can hang the child, IdentitiesOnly so only the -i key is
        // offered, a bounded ConnectTimeout, accept-new so the studio's own
        // known_hosts vouches, one -i key, an optional -p, and the upload-pack
        // word quoted as one piece. An ssh:// target carries its absolute path;
        // an scp-like one states it host-relative — the readings keep that
        // difference, and the spawn rides it unchanged.
        std::string sshSpawnLine(const std::string &sshExe, const SshTarget &target,
                                 const std::string &emptyConfig, const std::string &knownHosts,
                                 const std::string &keyPath, unsigned connectTimeoutMs)
        {
            std::string line = quoteWindowsArg(sshExe);
            line += " -F " + quoteWindowsArg(emptyConfig);
            line += " -o BatchMode=yes";
            line += " -o IdentitiesOnly=yes";
            line += " -o ConnectTimeout=" + std::to_string((connectTimeoutMs + 999u) / 1000u);
            line += " -o StrictHostKeyChecking=accept-new";
            line += " -o UserKnownHostsFile=" + quoteWindowsArg(knownHosts);
            line += " -i " + quoteWindowsArg(keyPath);
            if (target.port != 0u)
            {
                line += " -p " + std::to_string(target.port);
            }
            const std::string destination =
                target.user.empty() ? target.host : target.user + "@" + target.host;
            line += " " + quoteWindowsArg(destination);
            line += " " + quoteWindowsArg("git-upload-pack '" + target.path + "'");
            return line;
        }

        // The child the transport keeps between openChild and request: the
        // parent's own ends — the write end of stdin (a POST writes its body
        // through it), and the read ends of stdout and stderr. The child's
        // copies of the other ends close right away.
        struct SshFetch::Child
        {
            HANDLE stdinWrite = nullptr;
            HANDLE stdoutRead = nullptr;
            HANDLE stderrRead = nullptr;
            HANDLE process = nullptr;
            std::string stderrText;
        };

        SshFetch::SshFetch(const SshTarget &target, const SshIdentity &identity,
                           const std::string &sshExe)
            : target_(target), identity_(identity), sshExe_(sshExe)
        {
        }

        SshFetch::~SshFetch()
        {
            closeChild();
        }

        // Open one child for one ask. The road stands on the studio's known_hosts
        // and one key it holds — both are required before any spawn, and the
        // empty -F config is made beside known_hosts so the child reads no
        // account's own.
        bool SshFetch::openChild(std::string &error)
        {
            error.clear();
            const std::string knownHosts = identity_.knownHosts;
            if (knownHosts.empty())
            {
                error = "the ssh road needs the studio's known_hosts; none was named, so no host stands vouched";
                return false;
            }
            std::string dir;
            const std::filesystem::path knownPath(knownHosts);
            const std::filesystem::path parent = knownPath.parent_path();
            if (!parent.empty())
            {
                dir = parent.string();
            }
            else
            {
                dir = ".";
            }
            std::string emptyConfig;
            if (!ensureEmptyConfig(dir, emptyConfig, error))
            {
                return false;
            }
            const std::string keyPath = identity_.keyPath;
            if (keyPath.empty())
            {
                error = "the ssh road needs a key the studio holds; none stands in the key room";
                return false;
            }
            std::error_code ec;
            if (!std::filesystem::is_regular_file(keyPath, ec))
            {
                error = "the ssh road needs a key the studio holds; none stands at " + keyPath;
                return false;
            }
            SECURITY_ATTRIBUTES inheritable;
            inheritable.nLength = sizeof(SECURITY_ATTRIBUTES);
            inheritable.lpSecurityDescriptor = nullptr;
            inheritable.bInheritHandle = TRUE;
            HANDLE stdinRead = nullptr, stdinWrite = nullptr;
            HANDLE stdoutRead = nullptr, stdoutWrite = nullptr;
            HANDLE stderrRead = nullptr, stderrWrite = nullptr;
            if (!CreatePipeClassic(&stdinRead, &stdinWrite, &inheritable))
            {
                error = "the pipes for the ssh road could not be opened: win32 code " + std::to_string(GetLastError());
                return false;
            }
            if (!CreatePipeClassic(&stdoutRead, &stdoutWrite, &inheritable))
            {
                error = "the pipes for the ssh road could not be opened: win32 code " + std::to_string(GetLastError());
                CloseHandle(stdinRead);
                CloseHandle(stdinWrite);
                return false;
            }
            if (!CreatePipeClassic(&stderrRead, &stderrWrite, &inheritable))
            {
                error = "the pipes for the ssh road could not be opened: win32 code " + std::to_string(GetLastError());
                CloseHandle(stdinRead);
                CloseHandle(stdinWrite);
                CloseHandle(stdoutRead);
                CloseHandle(stdoutWrite);
                return false;
            }
            // The parent keeps its own ends — the stdin write end (a POST rides
            // through it) and the two read ends. The child's copies of the
            // other ends close right away, so a child that reads stdin sees
            // EOF: upload-pack ends its answer when stdin ends.
            SetHandleInformation(stdinRead, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOA startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = stdinRead;
            startup.hStdOutput = stdoutWrite;
            startup.hStdError = stderrWrite;
            PROCESS_INFORMATION info{};
            const std::string line = sshSpawnLine(sshExe_, target_, emptyConfig, knownHosts,
                                                  keyPath, static_cast<unsigned>(identity_.connectTimeoutMs));
            std::vector<char> cmdBuf(line.begin(), line.end());
            cmdBuf.push_back('\0');
            if (CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &info) == 0u)
            {
                const DWORD code = GetLastError();
                error = "the ssh road would not start: win32 code " + std::to_string(code);
                CloseHandle(stdinRead);
                CloseHandle(stdinWrite);
                CloseHandle(stdoutRead);
                CloseHandle(stdoutWrite);
                CloseHandle(stderrRead);
                CloseHandle(stderrWrite);
                return false;
            }
            CloseHandle(stdinRead);
            CloseHandle(stdoutWrite);
            CloseHandle(stderrWrite);
            Child *child = new Child;
            child->stdinWrite = stdinWrite;
            child->stdoutRead = stdoutRead;
            child->stderrRead = stderrRead;
            child->process = info.hProcess;
            child_ = child;
            return true;
        }

        // The child's end: its handles close, the wait is taken, the child is
        // deleted. Every ask opens its own child, so every ask ends with this.
        void SshFetch::closeChild()
        {
            if (child_ == nullptr)
            {
                return;
            }
            WaitForSingleObject(child_->process, INFINITE);
            CloseHandle(child_->process);
            CloseHandle(child_->stdinWrite);
            CloseHandle(child_->stdoutRead);
            CloseHandle(child_->stderrRead);
            delete child_;
            child_ = nullptr;
        }

        // One ask through the road. A fresh child stands for every ask: the
        // GET reads the advertisement off its stdout, the POST writes its
        // request through stdin and reads the pack to EOF. A child that ends
        // not-zero answers with its stderr words verbatim — the road refuses,
        // it does not translate.
        bool SshFetch::request(const std::string &method, const std::string &url,
                               const ols::http::Headers &headers, const std::string &body,
                               std::uint64_t maxReplyBytes, ols::http::Reply &replyOut,
                               std::string &error)
        {
            (void)headers;
            (void)url;
            replyOut = ols::http::Reply{};
            error.clear();
            closeChild();
            if (!openChild(error))
            {
                return false;
            }
            const bool isPost = (method == "POST");
            if (isPost && !body.empty())
            {
                std::size_t written = 0u;
                while (written < body.size())
                {
                    const std::size_t chunk = body.size() - written;
                    const std::size_t take = chunk > 4096u ? 4096u : chunk;
                    DWORD put = 0u;
                    if (!WriteFile(child_->stdinWrite, body.data() + written,
                                   static_cast<DWORD>(take), &put, nullptr) || put == 0u)
                    {
                        break;
                    }
                    written += static_cast<std::size_t>(put);
                }
            }
            // The stdin end closes either way: a GET has nothing to send, and
            // upload-pack ends its answer when stdin ends (spike §3).
            CloseHandle(child_->stdinWrite);
            child_->stdinWrite = nullptr;
            std::string out;
            bool overCap = false;
            char buf[4096];
            for (;;)
            {
                DWORD got = 0u;
                if (!ReadFile(child_->stdoutRead, buf, sizeof(buf), &got, nullptr))
                {
                    break;
                }
                if (got == 0u)
                {
                    break;
                }
                if (out.size() + got > maxReplyBytes)
                {
                    overCap = true;
                    break;
                }
                out.append(buf, buf + got);
            }
            for (;;)
            {
                DWORD got = 0u;
                if (!ReadFile(child_->stderrRead, buf, sizeof(buf), &got, nullptr))
                {
                    break;
                }
                if (got == 0u)
                {
                    break;
                }
                child_->stderrText.append(buf, buf + got);
            }
            if (overCap)
            {
                error = "the ssh answer runs past its cap of " + std::to_string(maxReplyBytes) + " bytes";
                closeChild();
                return false;
            }
            DWORD exitCode = 0u;
            GetExitCodeProcess(child_->process, &exitCode);
            if (exitCode != 0u)
            {
                const std::string words = trimTail(child_->stderrText);
                if (!words.empty())
                {
                    error = "the ssh road refused: " + words;
                }
                else
                {
                    error = "ssh ended with code " + std::to_string(exitCode);
                }
                closeChild();
                return false;
            }
            replyOut.status = 200;
            replyOut.body.swap(out);
            closeChild();
            return true;
        }
#else
        // The other side owes its pinned portable OpenSSH (run.sh names it);
        // until that stands, these three refuse in words.
        bool runVendored(const std::string &exePath, const std::vector<std::string> &args,
                         std::string &stdoutOut, std::string &stderrOut,
                         unsigned long &exitCodeOut, std::string &error)
        {
            (void)exePath;
            (void)args;
            stdoutOut.clear();
            stderrOut.clear();
            exitCodeOut = 0u;
            error = "no vendored run here: the Linux side owes its pinned portable OpenSSH (run.sh names it)";
            return false;
        }
        bool generateDeployKey(const std::string &dataDir, const std::string &name,
                               std::string &outPublic, std::string &outFingerprint,
                               std::string &error)
        {
            (void)dataDir;
            (void)name;
            outPublic.clear();
            outFingerprint.clear();
            error = "no key made here: the Linux side owes its pinned portable OpenSSH (run.sh names it)";
            return false;
        }
        bool trustHost(const std::string &dataDir, const std::string &host,
                       std::string &addedLine, std::string &error)
        {
            (void)dataDir;
            (void)host;
            addedLine.clear();
            error = "no host trusted here: the Linux side owes its pinned portable OpenSSH (run.sh names it)";
            return false;
        }
        // The transport class keeps its shape here, and every ask refuses in
        // words: the Linux side owes its pinned portable OpenSSH (run.sh names
        // it) before any child can stand.
        struct SshFetch::Child
        {
        };
        SshFetch::SshFetch(const SshTarget &target, const SshIdentity &identity,
                           const std::string &sshExe)
            : target_(target), identity_(identity), sshExe_(sshExe)
        {
        }
        SshFetch::~SshFetch()
        {
            closeChild();
        }
        bool SshFetch::openChild(std::string &error)
        {
            error = "no ssh child here: the Linux side owes its pinned portable OpenSSH (run.sh names it)";
            return false;
        }
        void SshFetch::closeChild()
        {
            if (child_ != nullptr)
            {
                delete child_;
                child_ = nullptr;
            }
        }
        bool SshFetch::request(const std::string &method, const std::string &url,
                               const ols::http::Headers &headers, const std::string &body,
                               std::uint64_t maxReplyBytes, ols::http::Reply &replyOut,
                               std::string &error)
        {
            (void)method;
            (void)url;
            (void)headers;
            (void)body;
            (void)maxReplyBytes;
            replyOut = ols::http::Reply{};
            error = "no ssh ask here: the Linux side owes its pinned portable OpenSSH (run.sh names it)";
            return false;
        }
#endif
    } // namespace ssh
} // namespace ols
