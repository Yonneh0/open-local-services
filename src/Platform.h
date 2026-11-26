// open-local-services platform layer: one socket API for the server and every
// service module (Winsock on Windows, BSD sockets elsewhere).
#ifndef OLS_PLATFORM_H
#define OLS_PLATFORM_H

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/syscall.h>
#endif

namespace ols
{
  namespace platform
  {

#ifdef _WIN32
    using sock_t = SOCKET;
    constexpr int kSocketError = SOCKET_ERROR;
    constexpr sock_t kBadSocket = INVALID_SOCKET;
    constexpr int kWouldBlock = WSAEWOULDBLOCK;
    constexpr int kConnReset = WSAECONNRESET;
    constexpr int kInterrupt = WSAEINTR;
#else
    using sock_t = int;
    constexpr int kSocketError = -1;
    constexpr sock_t kBadSocket = -1;
    constexpr int kWouldBlock = EAGAIN;
    constexpr int kConnReset = ECONNRESET;
    constexpr int kInterrupt = EINTR;
#endif

    inline bool initSockets()
    {
#ifdef _WIN32
      static std::once_flag wsaInitFlag;
      static bool wsaReady = false;
      std::call_once(wsaInitFlag, []()
                     {
    WSAData wsaData;
    wsaReady = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0; });
      return wsaReady;
#else
      return true;
#endif
    }

    inline sock_t newStreamSocket()
    {
      return socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }

    inline sockaddr_in endpointFor(uint16_t port, bool loopback)
    {
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_port = htons(port);
      address.sin_addr.s_addr = loopback ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
      return address;
    }

    inline int connectSocket(sock_t sock, const sockaddr_in &address)
    {
      return connect(sock, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
    }

    inline int bindSocket(sock_t sock, const sockaddr_in &address)
    {
      return bind(sock, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
    }

    // The listening socket is made so that it does not outlive the process that
    // opened it. Engines are started with std::system, which hands this process's
    // handles to the child on Windows and its file descriptors on the other side
    // unless they are marked otherwise, so an engine left running can keep the
    // requested port open after the server it came from is gone. Measured on X1
    // 2026-10-28: the server was stopped, a `llama-server.exe` still standing held
    // the requested port, and the next start said the port was taken and refused
    // to run.
    inline int listenSocket(sock_t sock, int backlog)
    {
#ifdef _WIN32
      SetHandleInformation(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(sock)),
                           HANDLE_FLAG_INHERIT, 0);
#else
      fcntl(sock, F_SETFD, FD_CLOEXEC);
#endif
      return listen(sock, backlog);
    }

    inline sock_t acceptClient(sock_t listener) { return accept(listener, nullptr, 0); }

    inline int sendBytes(sock_t sock, const char *data, int length, unsigned flags)
    {
      return send(sock, data, length, static_cast<int>(flags));
    }

    inline int receiveBytes(sock_t sock, char *data, int length, unsigned flags)
    {
      return recv(sock, data, length, static_cast<int>(flags));
    }

    inline void reuseAddress(sock_t sock)
    {
      const int enabled = 1;
      setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&enabled),
                 sizeof(enabled));
    }

    inline void setReadTimeout(sock_t sock, int milliseconds)
    {
#ifdef _WIN32
      const DWORD timeout = static_cast<DWORD>(milliseconds);
      setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
                 sizeof(timeout));
#else
      struct timeval timeout{};
      timeout.tv_sec = milliseconds / 1000;
      timeout.tv_usec = (milliseconds % 1000) * 1000;
      setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    }

    inline void setWriteTimeout(sock_t sock, int milliseconds)
    {
#ifdef _WIN32
      const DWORD timeout = static_cast<DWORD>(milliseconds);
      setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout),
                 sizeof(timeout));
#else
      struct timeval timeout{};
      timeout.tv_sec = milliseconds / 1000;
      timeout.tv_usec = (milliseconds % 1000) * 1000;
      setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    }

    // Let a socket go the way a conversation ends: the words already said are
    // finished with, and the other side hears that this side has.
    inline void closeSocket(sock_t sock)
    {
#ifdef _WIN32
      shutdown(sock, SD_BOTH);
      closesocket(sock);
#else
      close(sock);
#endif
    }

    // Let a socket go without saying anything - for a socket this machine only
    // ever probed with, where there is no conversation to end and a word sent
    // would be a word about a door this machine never opened.
    inline void discardSocket(sock_t sock)
    {
#ifdef _WIN32
      closesocket(sock);
#else
      close(sock);
#endif
    }

    inline int lastError()
    {
#ifdef _WIN32
      return WSAGetLastError();
#else
      return errno;
#endif
    }

    inline bool peerAddress(sock_t sock, std::string &text)
    {
      sockaddr_in peer{};
      socklen_t length = sizeof(peer);
      if (getpeername(sock, reinterpret_cast<sockaddr *>(&peer), &length) != 0)
        return false;
      text = inet_ntoa(peer.sin_addr);
      return true;
    }

    inline void *toHandle(sock_t sock)
    {
      return reinterpret_cast<void *>(static_cast<std::uintptr_t>(sock));
    }

    inline sock_t fromHandle(void *handle)
    {
      return static_cast<sock_t>(reinterpret_cast<std::uintptr_t>(handle));
    }

    inline const char *curlBinary()
    {
#ifdef _WIN32
      return "curl.exe";
#else
      return "curl";
#endif
    }

    // Can this text stand inside the quotes a command line is read in by? A
    // fetch is asked of curl through a command line, so every part of it - the
    // folder a file lands in, the address it comes from, the key that travels
    // with it - is put between quotes. Most of those parts were written by this
    // machine's own settings, but a file list comes from the source it is
    // fetching from, and a name carrying a quote or a separator there would stop
    // being a name and become a command. Such a part is refused, never escaped:
    // a shell has no quoting rule that makes every character safe.
    inline bool quotableInShell(const std::string &text)
    {
      for (const char c : text)
      {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f)
          return false;
#ifdef _WIN32
        if (c == '"' || c == '%' || c == '&' || c == '|' || c == '<' || c == '>' || c == '^' ||
            c == '`')
          return false;
#else
        // The parts sit between double quotes, which on this side of the water
        // do not hold everything in: a quote of its own ends the quoting, and a
        // dollar, a backtick and a backslash still do their work inside it. Those
        // are the whole of what can act from between the quotes, beside the
        // single quote no part needs.
        if (c == '"' || c == '\'' || c == '$' || c == '`' || c == '\\')
          return false;
#endif
      }
      return true;
    }

    // The fetch of one file over the network, asked the same way by every
    // module: the patience of a slow reader, a place to put the file even when
    // its folders are not there yet, and a part of a file already taken left
    // in place. A key is added only when the module holds one; it travels with
    // the request whichever side of the address it stands on, so a module
    // cannot drift away from the others in how it asks.
    //
    // The resume is asked first and given up on once, because an address that
    // does not answer a resume ask is not an error in the file: curl stops with
    // 33 - "HTTP server does not seem to support byte ranges. Cannot resume" -
    // and --retry asks the same refused question again. That is what three weight
    // downloads on X1 did on 2026-10-28: two 33s and then (18) end of response
    // with 2,723,119,751 bytes missing, with the page left showing a model that
    // is on this disk as weights not downloaded. So a refused resume (33) or a
    // resume range the address rejected (36) starts the file clean here; whether
    // the bytes that arrive are the bytes meant is decided by the digest the
    // caller takes afterwards, never by the fetch having finished.
    //
    // The answer is curl's own code, or -1 when this machine would not ask at
    // all because one of the parts cannot be quoted (see quotableInShell).
    //
    // A sliced fetch (maxSeconds above zero) asks for no retries of its own:
    // --retry re-reads --max-time as the patience of each try rather than of
    // the command - measured 2026-11-19, a --max-time 20 ask with --retry 3
    // ran 87 seconds - and the round would report nothing for all of them.
    // The slicing loop outside this call is the retry there, resuming from the
    // bytes the round left behind.
    inline int curlFetch(const std::string &dest, const std::string &url, const std::string &token,
                         int maxSeconds = 0, int retries = 3)
    {
      if (!quotableInShell(dest) || !quotableInShell(url) || !quotableInShell(token))
        return -1;
      const std::string base = std::string(curlBinary()) + " -sSL --fail --retry " +
                               std::to_string(retries) + " --create-dirs";
      const std::string bound = maxSeconds > 0 ? " --max-time " + std::to_string(maxSeconds) : "";
      const std::string auth =
          token.empty() ? std::string() : " -H \"Authorization: Bearer " + token + "\"";
      const std::string tail = " -o \"" + dest + "\" \"" + url + "\"";
      const int resumed = std::system((base + " -C -" + bound + auth + tail).c_str());
      if (resumed == 33 || resumed == 36)
        return std::system((base + bound + auth + tail).c_str());
      return resumed;
    }

    // The name this machine answers to on the network, in its own words. The
    // record a render leaves beside its file names it, because two machines of
    // one suite draw the same settings at different speeds and the file should
    // say which one drew it.
    inline std::string hostName()
    {
#ifdef _WIN32
      char text[128] = {0};
      unsigned long size = sizeof(text);
      if (GetComputerNameExA(ComputerNameDnsHostname, text, &size) && size > 0)
        return std::string(text, size);
      return "this machine";
#else
      char text[128] = {0};
      if (gethostname(text, sizeof(text) - 1) == 0)
        return std::string(text);
      return "this machine";
#endif
    }

    // The version of this code is what the build names it, in two kinds: a
    // development script names the dev tag, and a release builder names the
    // commit its code stands on - only after checking the tree is committed,
    // which is what makes that commit an invaluable debugging tool, tying the
    // artifact to one re-creatable source tree. A build that named no version
    // says so; never a guess. The value is not handed over on the compiler's
    // command line - a define there would not make an already-compiled object
    // out of date, and a release version is a commit - the build writes it into
    // a file of its own, which every build of the server compiles. The pages,
    // the start-up line and the status snapshot read these same words through
    // versionLine below, so the wording lives here, in one place.
    std::string buildVersion();

    // The moment this binary was built, carried in a file of its own that the
    // build step beside it writes afresh for every build. Not __DATE__ and
    // __TIME__: those are words each object says for itself, and an incremental
    // build re-links objects it never touched - their dates would speak for a
    // binary made later than every byte they describe. The build step stands
    // outside the up-to-date rules, so what this answers is always the moment
    // this binary was made, never the moment some older object was.
    std::string buildMoment();
    inline std::string buildStamp()
    {
      return buildMoment();
    }

    // The same moment the line above spells, as whole seconds since 1970: a
    // page that counts up from the build cannot read a date name, but it can
    // subtract a number. The build writes both together, so they cannot disagree.
    long long buildMomentSeconds();

    // The one line every surface states the version with. A development name is
    // the dev tag and the compile moment only - a development binary carries no
    // commit, its code is a working tree. Any other name is a release build's
    // commit, stated as such beside the compile moment, so an artifact always
    // names the exact source tree it can be re-created from.
    inline std::string versionLine()
    {
      const std::string at = buildVersion();
      if (at.empty())
        return std::string("version not stated: built without one");
      if (at == "dev")
        return "dev, compiled " + buildStamp();
      return "rel " + at + ", compiled " + buildStamp();
    }

  } // namespace platform
} // namespace ols

#endif // OLS_PLATFORM_H
