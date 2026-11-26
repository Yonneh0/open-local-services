// src/HttpFetch.h — the outbound HTTP/1.1 client, over the platform's own
// socket API (Platform.h). Plain http only: an https address is refused in
// words that name TLS, because the tree carries no TLS layer to stand it on.
//
// Two things live here.
//
// The reader (parseReply) makes a status, case-insensitive headers, and a
// body out of the bytes gathered so far, and it owns the frame rules. A
// declared Content-Length gives the body's width. Else a chunked stream —
// read with the rules this server's own request decoder runs
// (OpenLocalServices.cpp:1718-1852): the size line is hex with any ';'
// extension read and set aside, each chunk's payload ends at its own CRLF,
// and the zero chunk is followed by trailer lines that end at a blank line
// or at the stream itself. Else, when Connection: close says the stream's
// end is the body's end, the rest is the body once the stream has ended.
// The response-size cap is stated and weighed before any body byte is taken.
//
// The transport (HttpFetch) is the seam: one request in, one answer out.
// SocketHttpFetch stands it on Platform.h primitives — a non-blocking
// connect followed by select and SO_ERROR, the patience PeerService's own
// client keeps (PeerService.cpp:191-232), and read/write timeouts on the
// socket (Platform.h:129-155). Everything above the socket reads through the
// seam alone, so a test can hand the same road canned answers with no socket
// in sight.
#ifndef OLS_HTTPFETCH_H
#define OLS_HTTPFETCH_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The include stands at file scope, not inside the namespaces below — the same
// rule packfile.h:29-33 and refs.h:29-35 keep, because an include from within
// ols::http would nest the whole of Platform.h and poison every later std::
// name in the translation unit.
#include "Platform.h"
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/time.h>
#endif

namespace ols
{
    namespace http
    {

        // The two codes a waiting road reads: the socket that would block, and
        // the call a signal cut short — the pair PeerService.cpp's patience
        // reads by the platform's own names (PeerService.cpp:204-206).
#ifdef _WIN32
        constexpr int kWouldBlock = WSAEWOULDBLOCK;
        constexpr int kInterrupt = WSAEINTR;
#else
        constexpr int kWouldBlock = EAGAIN;
        constexpr int kInterrupt = EINTR;
#endif

        // ------------------------------------------------------------------ headers

        inline bool equalsInsensitive(const std::string &a, const std::string &b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (std::size_t at = 0u; at < a.size(); ++at)
            {
                char x = a[at];
                char y = b[at];
                if (x >= 'A' && x <= 'Z')
                {
                    x = static_cast<char>(x - 'A' + 'a');
                }
                if (y >= 'A' && y <= 'Z')
                {
                    y = static_cast<char>(y - 'A' + 'a');
                }
                if (x != y)
                {
                    return false;
                }
            }
            return true;
        }

        inline bool containsInsensitive(const std::string &text, const std::string &word)
        {
            if (word.size() > text.size())
            {
                return false;
            }
            for (std::size_t at = 0u; at + word.size() <= text.size(); ++at)
            {
                if (equalsInsensitive(text.substr(at, word.size()), word))
                {
                    return true;
                }
            }
            return false;
        }

        // Header names are matched case-insensitively, as HTTP says they must
        // be; the stored spelling is the one that was set.
        struct Headers
        {
            std::vector<std::pair<std::string, std::string>> lines;

            void set(const std::string &name, const std::string &value)
            {
                erase(name);
                lines.push_back(std::make_pair(name, value));
            }
            void erase(const std::string &name)
            {
                for (std::vector<std::pair<std::string, std::string>>::iterator it = lines.begin();
                     it != lines.end();)
                {
                    if (equalsInsensitive(it->first, name))
                    {
                        it = lines.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            // The last line of that name, which is the one a repeated header
            // has left standing.
            bool find(const std::string &name, std::string &out) const
            {
                for (std::size_t at = lines.size(); at > 0u; --at)
                {
                    if (equalsInsensitive(lines[at - 1u].first, name))
                    {
                        out = lines[at - 1u].second;
                        return true;
                    }
                }
                return false;
            }
        };

        // -------------------------------------------------------------------- reply

        struct Reply
        {
            int status = 0;
            Headers headers;
            std::string body;
        };

        // The cap on the head: status line and header lines together. A head
        // past this is not a response worth any more waiting.
        constexpr std::uint64_t kMaxHeadBytes = 64ull * 1024ull;

        // Reads one response out of the bytes gathered so far.
        //   true                       — the answer is whole: status, headers, body.
        //   false, needMore true       — the bytes stand short: read on.
        //   false, needMore false      — refuse; error says why.
        // maxBodyBytes is weighed against a declared length and against each
        // chunk before its bytes are taken, so a body over the cap is refused
        // without the body being held. streamEnded tells the reader the socket
        // has said its last word — it is what makes a close-delimited body, or
        // a chunked stream whose trailer ends at the stream, a whole answer.
        bool parseReply(const std::string &wire, std::uint64_t maxBodyBytes, bool streamEnded,
                        Reply &out, bool &needMore, std::string &error);

        // --------------------------------------------------------------- the seam

        // One outbound request. The url is absolute ("http://host[:port]/path");
        // headers travel as they are given; maxReplyBytes caps the response
        // body. false means no answer: the words say where the conversation
        // died, and replyOut keeps nothing of a refused answer.
        struct HttpFetch
        {
            virtual ~HttpFetch() = default;
            virtual bool request(const std::string &method, const std::string &url,
                                 const Headers &headers, const std::string &body,
                                 std::uint64_t maxReplyBytes, Reply &replyOut,
                                 std::string &error) = 0;
        };

        inline bool get(HttpFetch &http, const std::string &url, const Headers &headers,
                        std::uint64_t maxReplyBytes, Reply &replyOut, std::string &error)
        {
            return http.request("GET", url, headers, std::string(), maxReplyBytes, replyOut, error);
        }

        inline bool post(HttpFetch &http, const std::string &url, const Headers &headers,
                         const std::string &body, std::uint64_t maxReplyBytes, Reply &replyOut,
                         std::string &error)
        {
            return http.request("POST", url, headers, body, maxReplyBytes, replyOut, error);
        }

        // ------------------------------------------------------------ the socket road

        // Splits an absolute http url into host, port, and the request target
        // (path and query, starting at "/"). An https address is refused: the
        // words name TLS, the layer this tree does not carry. Other schemes are
        // refused the same way.
        bool splitUrl(const std::string &url, std::string &hostOut, std::uint16_t &portOut,
                      std::string &targetOut, std::string &error);

        // The seam on Platform.h: initSockets, a non-blocking connect with the
        // select/SO_ERROR patience of PeerService.cpp:191-232, setWriteTimeout
        // and setReadTimeout, sendBytes/receiveBytes to the response's last
        // byte, closeSocket. One request, one socket, one thread.
        class SocketHttpFetch : public HttpFetch
        {
        public:
            explicit SocketHttpFetch(int connectTimeoutMs = 10000, int readTimeoutMs = 60000)
                : connectTimeoutMs_(connectTimeoutMs), readTimeoutMs_(readTimeoutMs)
            {
            }

            bool request(const std::string &method, const std::string &url,
                         const Headers &headers, const std::string &body,
                         std::uint64_t maxReplyBytes, Reply &replyOut,
                         std::string &error) override;

        private:
            int connectTimeoutMs_;
            int readTimeoutMs_;
        };

        // ------------------------------------------------------------------ socket impl

        inline bool splitUrl(const std::string &url, std::string &hostOut, std::uint16_t &portOut,
                             std::string &targetOut, std::string &error)
        {
            hostOut.clear();
            portOut = 80u;
            targetOut = "/";
            if (url.size() >= 8u && equalsInsensitive(url.substr(0u, 8u), "https://"))
            {
                error = "the address " + url +
                        " asks for https, which is TLS, and this tree carries no TLS layer for it: "
                        "send a plain http:// address";
                return false;
            }
            if (url.size() < 7u || !equalsInsensitive(url.substr(0u, 7u), "http://"))
            {
                error = "the address " + url +
                        " is not an http:// address, and this client speaks no other scheme";
                return false;
            }
            std::size_t rest = 7u;
            const std::size_t slash = url.find('/', rest);
            std::string authority =
                (slash == std::string::npos) ? url.substr(rest) : url.substr(rest, slash - rest);
            if (authority.empty())
            {
                error = "the address " + url + " names no host";
                return false;
            }
            const std::size_t colon = authority.rfind(':');
            if (colon != std::string::npos)
            {
                const std::string portText = authority.substr(colon + 1u);
                if (portText.empty() || portText.size() > 5u)
                {
                    error = "the address " + url + " names no port this client can reach";
                    return false;
                }
                unsigned long port = 0ul;
                for (char c : portText)
                {
                    if (c < '0' || c > '9')
                    {
                        error = "the port in " + url + " is not a number";
                        return false;
                    }
                    port = port * 10ul + static_cast<unsigned long>(c - '0');
                }
                if (port == 0ul || port > 65535ul)
                {
                    error = "the port in " + url + " stands outside the reachable range";
                    return false;
                }
                portOut = static_cast<std::uint16_t>(port);
                authority = authority.substr(0u, colon);
            }
            if (authority.empty())
            {
                error = "the address " + url + " names no host";
                return false;
            }
            hostOut = authority;
            if (slash != std::string::npos)
            {
                targetOut = url.substr(slash);
            }
            return true;
        }

        // The hex and decimal digits the frames are read in.
        inline bool hexValue(char c, unsigned &out)
        {
            if (c >= '0' && c <= '9')
            {
                out = static_cast<unsigned>(c - '0');
                return true;
            }
            if (c >= 'a' && c <= 'f')
            {
                out = static_cast<unsigned>(c - 'a') + 10u;
                return true;
            }
            if (c >= 'A' && c <= 'F')
            {
                out = static_cast<unsigned>(c - 'A') + 10u;
                return true;
            }
            return false;
        }

        inline bool decimalValue(const std::string &text, std::uint64_t &out)
        {
            if (text.empty())
            {
                return false;
            }
            std::uint64_t value = 0ull;
            for (char c : text)
            {
                if (c < '0' || c > '9')
                {
                    return false;
                }
                if (value > (1ull << 56))
                {
                    return false; // a width past this is a claim, not a length
                }
                value = value * 10ull + static_cast<std::uint64_t>(c - '0');
            }
            out = value;
            return true;
        }

        inline std::string trimSpaces(const std::string &text)
        {
            std::size_t from = 0u;
            while (from < text.size() && (text[from] == ' ' || text[from] == '\t'))
            {
                ++from;
            }
            std::size_t to = text.size();
            while (to > from && (text[to - 1u] == ' ' || text[to - 1u] == '\t'))
            {
                --to;
            }
            return text.substr(from, to - from);
        }

        inline bool parseReply(const std::string &wire, std::uint64_t maxBodyBytes, bool streamEnded,
                               Reply &out, bool &needMore, std::string &error)
        {
            needMore = false;
            const std::size_t headEnd = wire.find("\r\n\r\n");
            if (headEnd == std::string::npos)
            {
                if (wire.size() > kMaxHeadBytes)
                {
                    error = "the response head runs past " + std::to_string(kMaxHeadBytes) +
                            " bytes without ending";
                    return false;
                }
                if (!streamEnded)
                {
                    needMore = true;
                    return false;
                }
                error = "the stream ends with no response head in it";
                return false;
            }
            const std::string head = wire.substr(0u, headEnd);
            const std::size_t bodyStart = headEnd + 4u;

            Reply local;
            std::size_t lineAt = 0u;
            const std::size_t firstBreak = head.find("\r\n");
            // "HTTP/1.1 200" stands in twelve bytes: the version holds 5, 6
            // and 7, and the space before the digits is at 8.
            if (firstBreak == std::string::npos || firstBreak < 12u ||
                head.compare(0u, 5u, "HTTP/") != 0 || head[5u] != '1' || head[6u] != '.' ||
                head[8u] != ' ')
            {
                error = "the first line of the response is not a status line";
                return false;
            }
            const std::string codeText = head.substr(9u, firstBreak - 9u);
            // "200 OK" and a bare "200" are both a status; anything else is not.
            std::string codeOnly = codeText;
            const std::size_t codeSpace = codeOnly.find(' ');
            if (codeSpace != std::string::npos)
            {
                codeOnly = codeOnly.substr(0u, codeSpace);
            }
            std::uint64_t code = 0ull;
            if (codeOnly.size() != 3u || !decimalValue(codeOnly, code))
            {
                error = "the status line states no three-digit status";
                return false;
            }
            local.status = static_cast<int>(code);
            lineAt = firstBreak + 2u;
            while (lineAt < head.size())
            {
                std::size_t breakAt = head.find("\r\n", lineAt);
                if (breakAt == std::string::npos)
                {
                    breakAt = head.size(); // the head ended where the blank line began
                }
                const std::string line = head.substr(lineAt, breakAt - lineAt);
                lineAt = breakAt + 2u;
                if (line.empty())
                {
                    continue;
                }
                const std::size_t colon = line.find(':');
                if (colon == std::string::npos)
                {
                    error = "a header line carries no name: " + line;
                    return false;
                }
                local.headers.set(trimSpaces(line.substr(0u, colon)),
                                  trimSpaces(line.substr(colon + 1u)));
            }

            std::string transfer;
            local.headers.find("Transfer-Encoding", transfer);
            std::string contentLength;
            local.headers.find("Content-Length", contentLength);
            std::string connection;
            local.headers.find("Connection", connection);

            if (containsInsensitive(transfer, "chunked"))
            {
                // The frames, by the rules of the decoder this server itself
                // runs (OpenLocalServices.cpp:1718-1852).
                std::string body;
                std::size_t at = bodyStart;
                for (;;)
                {
                    const std::size_t nl = wire.find("\r\n", at);
                    if (nl == std::string::npos)
                    {
                        if (!streamEnded)
                        {
                            needMore = true;
                            return false;
                        }
                        error = "the chunked stream ends with its size line unfinished";
                        return false;
                    }
                    std::string token = wire.substr(at, nl - at);
                    at = nl + 2u;
                    const std::size_t semi = token.find(';'); // read and set aside
                    if (semi != std::string::npos)
                    {
                        token.resize(semi);
                    }
                    std::uint64_t size = 0ull;
                    bool digits = !token.empty();
                    for (char c : token)
                    {
                        unsigned nibble = 0u;
                        if (!hexValue(c, nibble))
                        {
                            digits = false;
                            break;
                        }
                        if (size > (1ull << 56))
                        {
                            digits = false; // a size line past this is not a frame
                            break;
                        }
                        size = size * 16ull + nibble;
                    }
                    if (!digits)
                    {
                        error = "a chunk size line is not hex: " + token;
                        return false;
                    }
                    if (size == 0ull)
                    {
                        // The end: each trailer line keeps its own line, and
                        // the blank one closes the body — or the stream does.
                        for (;;)
                        {
                            const std::size_t tl = wire.find("\r\n", at);
                            if (tl == std::string::npos)
                            {
                                if (!streamEnded)
                                {
                                    needMore = true;
                                    return false;
                                }
                                at = wire.size(); // the stream ends at its terminator
                                break;
                            }
                            if (tl == at)
                            {
                                at = tl + 2u;
                                break;
                            }
                            at = tl + 2u; // one trailer line, read and left aside
                        }
                        local.body.swap(body);
                        out = local;
                        return true;
                    }
                    if (body.size() + size > maxBodyBytes)
                    {
                        error = "the response body runs past its cap of " +
                                std::to_string(maxBodyBytes) + " bytes";
                        return false;
                    }
                    if (wire.size() < at + size + 2u)
                    {
                        if (streamEnded)
                        {
                            error = "the chunked stream ends inside a chunk";
                            return false;
                        }
                        needMore = true;
                        return false;
                    }
                    if (wire.compare(at + size, 2u, "\r\n") != 0)
                    {
                        error = "a chunk does not end where its CRLF should stand";
                        return false;
                    }
                    body.append(wire, at, size);
                    at += size + 2u;
                }
            }

            if (!contentLength.empty())
            {
                std::uint64_t declared = 0ull;
                if (!decimalValue(contentLength, declared))
                {
                    error = "the Content-Length is not a number: " + contentLength;
                    return false;
                }
                if (declared > maxBodyBytes)
                {
                    error = "the response declares " + std::to_string(declared) +
                            " bytes, past the cap of " + std::to_string(maxBodyBytes);
                    return false;
                }
                if (wire.size() - bodyStart < declared)
                {
                    if (streamEnded)
                    {
                        error = "the stream ends short of the body its Content-Length declares";
                        return false;
                    }
                    needMore = true;
                    return false;
                }
                local.body.assign(wire, bodyStart, static_cast<std::size_t>(declared));
                out = local;
                return true;
            }

            if (containsInsensitive(connection, "close"))
            {
                if (!streamEnded)
                {
                    needMore = true;
                    return false;
                }
                const std::size_t rest = wire.size() - bodyStart;
                if (rest > maxBodyBytes)
                {
                    error = "the response body runs past its cap of " +
                            std::to_string(maxBodyBytes) + " bytes";
                    return false;
                }
                local.body.assign(wire, bodyStart, rest);
                out = local;
                return true;
            }

            // No length, no chunking, no close: the body is nothing, and any
            // bytes standing behind the head are nobody's answer.
            out = local;
            return true;
        }

        inline bool SocketHttpFetch::request(const std::string &method, const std::string &url,
                                             const Headers &headers, const std::string &body,
                                             std::uint64_t maxReplyBytes, Reply &replyOut,
                                             std::string &error)
        {
            if (!platform::initSockets())
            {
                error = "the socket layer would not start on this machine";
                return false;
            }
            std::string host;
            std::uint16_t port = 80u;
            std::string target;
            if (!splitUrl(url, host, port, target, error))
            {
                return false;
            }

            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo *resolved = nullptr;
            const int nameCode =
                getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &resolved);
            if (nameCode != 0 || resolved == nullptr)
            {
                error = "the host " + host + " does not resolve: code " + std::to_string(nameCode);
                return false;
            }

            const platform::sock_t sock = platform::newStreamSocket();
            if (sock == platform::kBadSocket)
            {
                freeaddrinfo(resolved);
                error = "no socket could be opened: code " + std::to_string(platform::lastError());
                return false;
            }

            // The connect travels: a refused port says so in a moment and an
            // address where nobody lives says it after the patience runs out,
            // the way PeerService::getJson keeps its own (PeerService.cpp:191-
            // 232): non-blocking connect, select for writing with the timeout,
            // and SO_ERROR as the last word.
#ifdef _WIN32
            u_long blocking = 0ul;
            ioctlsocket(sock, FIONBIO, &blocking);
#else
            const int flags = fcntl(sock, F_GETFL, 0);
            if (flags >= 0)
            {
                fcntl(sock, F_SETFL, flags | O_NONBLOCK);
            }
#endif
            bool connected = false;
            if (connect(sock, resolved->ai_addr, static_cast<int>(resolved->ai_addrlen)) == 0)
            {
                connected = true;
            }
            else
            {
                const int failure = platform::lastError();
#ifdef _WIN32
                const bool travelling =
                    failure == WSAEWOULDBLOCK || failure == WSAEINPROGRESS || failure == kInterrupt;
#else
                const bool travelling =
                    failure == EINPROGRESS || failure == kWouldBlock || failure == kInterrupt;
#endif
                if (travelling)
                {
                    fd_set writing{}, excepts{};
                    FD_ZERO(&writing);
                    FD_ZERO(&excepts);
                    FD_SET(sock, &writing);
                    FD_SET(sock, &excepts);
                    timeval patience{};
                    patience.tv_sec = connectTimeoutMs_ / 1000;
                    patience.tv_usec = (connectTimeoutMs_ % 1000) * 1000;
                    const int ready =
                        select(static_cast<int>(sock) + 1, nullptr, &writing, &excepts, &patience);
                    if (ready > 0 && FD_ISSET(sock, &writing) && !FD_ISSET(sock, &excepts))
                    {
                        int soError = 0;
                        socklen_t length = sizeof(soError);
                        getsockopt(sock, SOL_SOCKET, SO_ERROR,
                                   reinterpret_cast<char *>(&soError), &length);
                        connected = soError == 0;
                        if (!connected)
                        {
                            error = "the connection to " + host + ":" + std::to_string(port) +
                                    " was refused: SO_ERROR " + std::to_string(soError);
                        }
                    }
                    else
                    {
                        error = "the connection to " + host + ":" + std::to_string(port) +
                                " stood still past " + std::to_string(connectTimeoutMs_) + " ms";
                    }
                }
                else
                {
                    error = "the connection to " + host + ":" + std::to_string(port) +
                            " failed: code " + std::to_string(failure);
                }
            }
            freeaddrinfo(resolved);
            if (!connected)
            {
                platform::discardSocket(sock);
                return false;
            }

#ifdef _WIN32
            blocking = 1ul;
            ioctlsocket(sock, FIONBIO, &blocking);
#else
            if (flags >= 0)
            {
                fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
            }
#endif
            platform::setWriteTimeout(sock, readTimeoutMs_);
            platform::setReadTimeout(sock, readTimeoutMs_);

            std::string request = method + " " + target + " HTTP/1.1\r\n";
            request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
            request += "User-Agent: open-local-services fetch\r\n";
            for (const std::pair<std::string, std::string> &line : headers.lines)
            {
                request += line.first + ": " + line.second + "\r\n";
            }
            if (method == "POST")
            {
                request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            }
            request += "Connection: close\r\n\r\n";
            request += body;

            std::size_t sent = 0u;
            bool wrote = true;
            while (sent < request.size())
            {
                const int made = platform::sendBytes(
                    sock, request.data() + sent,
                    static_cast<int>(request.size() - sent < 65536ull
                                         ? request.size() - sent
                                         : static_cast<std::size_t>(65536ull)),
                    0u);
                if (made <= 0)
                {
                    error = "the request did not go out: code " +
                            std::to_string(platform::lastError());
                    wrote = false;
                    break;
                }
                sent += static_cast<std::size_t>(made);
            }
            if (!wrote)
            {
                platform::discardSocket(sock);
                return false;
            }

            std::string wire;
            bool streamEnded = false;
            Reply reply;
            bool done = false;
            while (!done)
            {
                bool needMore = false;
                if (parseReply(wire, maxReplyBytes, streamEnded, reply, needMore, error))
                {
                    done = true;
                    break;
                }
                if (!needMore)
                {
                    break; // a refusal, not a shortage
                }
                if (streamEnded)
                {
                    error = "the stream ended with the response unfinished";
                    break;
                }
                char pad[16384];
                const int got = platform::receiveBytes(sock, pad, static_cast<int>(sizeof(pad)), 0u);
                if (got > 0)
                {
                    wire.append(pad, static_cast<std::size_t>(got));
                    continue;
                }
                if (got == 0)
                {
                    streamEnded = true;
                    continue;
                }
                const int why = platform::lastError();
                if (why == kWouldBlock)
                {
                    error = "the answer stood still past " + std::to_string(readTimeoutMs_) + " ms";
                    break;
                }
                streamEnded = true; // the conversation ended where it ended
            }
            platform::closeSocket(sock);
            if (!done)
            {
                return false;
            }
            replyOut = reply;
            return true;
        }

    } // namespace http
} // namespace ols

#endif // OLS_HTTPFETCH_H
