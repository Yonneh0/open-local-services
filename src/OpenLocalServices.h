// Open-local-services: the HTTP server and route table. Foundational code —
// service-specific behavior lives in the service modules (ols::imagegen for
// pictures, ols::project for the project rooms).
#ifndef OLS_OPEN_LOCAL_SERVICES_H
#define OLS_OPEN_LOCAL_SERVICES_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "PeerService.h"
#include "Platform.h"

namespace ols
{
  class TlsLayer;
  class TlsMaterial;

  // One client of a connection: what handleConnection passes along the
  // streaming doors as one handle. A request in the clear answers through
  // the socket held below; every sender and reader takes this. A sealed
  // visitor carries the layer that speaks TLS for it - the readers and
  // writers below answer through it when it stands, and through the bare
  // socket when it does not.
  // The sink owns the socket: closing is its destructor's, and its one act, so
  // every exit from a connection - the answer, a refusal, a throw - leaves the
  // socket closed exactly once. A hand that has already closed says so by
  // holding nothing.
  struct ConnectionSink
  {
    platform::sock_t sock;
    // the sealed conversation of this connection, or null when the visitor
    // came in the clear and today's plain path answers it untouched
    std::unique_ptr<TlsLayer> tls;

    // The socket is taken whole here and let go there: constructor and
    // destructor are both defined beside the socket code, where the layer
    // they carry is a complete type.
    explicit ConnectionSink(platform::sock_t s);
    // Defined beside the socket code, once: a sealed connection says its
    // goodbye (close_notify) before the socket lets go.
    ~ConnectionSink();

    ConnectionSink(const ConnectionSink &) = delete;
    ConnectionSink &operator=(const ConnectionSink &) = delete;
  };

  // The ols root namespace; each service owns its own sub-namespace: imagegen
  // (image studio) and project (project studio).
  namespace imagegen
  {
    class ImageService;
  } // namespace imagegen
  namespace project
  {
    class ProjectService;
  } // namespace project
  struct HttpRequest
  {
    std::string method;
    std::string path;
    std::string query;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string ip;
  };

  struct HttpResponse
  {
    int status = 200;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    void setJson(const std::string &json);
    void setPlain(const std::string &text, const std::string &contentType);

    // A door that answers bigger than memory hands the writing over instead of
    // the bytes: each call writes one chunk and says whether it reached the
    // wire. The server frames them as a chunked answer and ends the stream when
    // one returns false or zero length comes back.
    using ChunkWriter = bool (*)(void *handle, std::string &chunkOut);
    ChunkWriter chunkSource = nullptr;
    void *chunkHandle = nullptr;

    // A git smart-HTTP door writes its own answer over the connection - it is
    // not a json body and not framed chunks: the route hands the connection
    // itself over, the writer speaks the protocol whole, and nothing else of
    // this answer is written. The sink arrives as the same opaque handle the
    // streaming path already passes.
    using GitWriter = void (*)(void *sinkHandle, const HttpRequest &req);
    GitWriter gitWriter = nullptr;
  };

  // The default port these doors stand on. `OLS_PORT` may ask for another one at
  // startup, but the server never chooses another port because the first one is
  // held. A peer's word carries the address and port it actually answered on.
  static const uint16_t kApiPort = 8765;

  class OpenLocalServices
  {
  public:
    OpenLocalServices();

    OpenLocalServices(const OpenLocalServices &) = delete;
    OpenLocalServices &operator=(const OpenLocalServices &) = delete;

    // Nothing that answers out of these doors may outlive the doors: the
    // destructor asks the server to stop, which joins the connection threads it
    // registered (see stop()).
    ~OpenLocalServices();

    bool start(uint16_t port);
    // Why a start was refused, in words for whoever ran the binary: the port, and
    // that something already holds it.
    const std::string &startRefusal() const { return m_startRefusal; }
    void stop();
    // The soft hand: this server is asked to stop at its own door, which is
    // where the words of that ask have already been written.
    void stopAfterThisAnswer();
    bool isRunning() const { return m_running.load(); }
    uint16_t port() const { return m_port.load(); }
    void setBindHost(const std::string &host) { m_bindHost = host; }
    std::string bindHost() const;
    static bool isLoopbackHost(const std::string &host);
    static std::string statusLine(int status);
    static bool writeStreamChunk(void *client, const std::string &frame);

    void setStaticDir(const std::string &dir) { m_staticDir = dir; }
    std::string staticDir() const { return m_staticDir; }
    void setImage(imagegen::ImageService *image);
    void setProject(project::ProjectService *projects);

    // Fleet awareness: stand the peer table up beside these doors. The sweep of
    // the networks runs once, in the background, after the doors already
    // answer; the hello door is worked from the route table.
    void startPeers(const std::string &apiKey, const std::string &dataDir);

    HttpResponse jsonError(int status, const std::string &message) const;
    std::string systemJson() const;
    // Whether the pages served come from a folder beside the server or from the
    // copies carried inside this binary, as json text a page can state.
    std::string servedPagesJson() const;

    // Ask the image engine's ggml, linked into this binary, for the machine's
    // devices, now rather than on the first page read, and on Windows name the
    // live Vulkan driver manifest so the device report and the engines agree on
    // the gpu. No plugin library is loaded: the backend this build was asked
    // for is compiled in, and loading a second copy of it registers the same
    // card twice.
    void preloadBackends();
    // The key is written through the config door while every connection thread
    // reads it, so it is never edited in place: a whole immutable copy is
    // swapped in under a lock, and a reader holds one copy for its whole request.
    void setApiKey(const std::string &key)
    {
      const std::shared_ptr<const std::string> fresh =
          std::make_shared<const std::string>(key);
      std::lock_guard<std::mutex> lock(m_apiKeyMutex);
      m_apiKey = fresh;
    }
    void setRateLimit(int requestsPerMinute)
    {
      if (requestsPerMinute < 0)
        requestsPerMinute = 600;
      else if (requestsPerMinute > 1000000)
        requestsPerMinute = 1000000;
      m_rateLimit.store(requestsPerMinute);
    }
    int rateLimit() const { return m_rateLimit.load(); }
    // TLS on the one port (spec §10.3), named from .env before start(). The
    // word is OLS_TLS: blank or "off" answers in the clear as it always has;
    // "auto" looks at each visitor's first bytes and serves both kinds,
    // making the certificates under the folder when none stand there; "on"
    // serves sealed connections only. The material - the one read-only
    // config every connection's layer rides - is built at start(), not in
    // this call, so a start that cannot stand says so in words.
    void setTls(const std::string &mode, const std::string &certDir)
    {
      m_tlsMode = mode;
      m_tlsCertDir = certDir;
    }
    // The api door: open when this machine sets no key, and otherwise answered
    // by that key alone - one credential carries a visit through both kinds of
    // door.
    bool authorize(const HttpRequest &req);
    // 0 = allow, 1 = send the refusal, -1 = no response (lockout). One shared
    // miss counter behind both doors, so a storm of refused rooms silences the
    // api doors too.
    int authGate(const HttpRequest &req);
    // What an api door says when a credential was wanted and none acceptable
    // came: the ways it may be handed, and nothing else.
    HttpResponse authChallenge() const;
    HttpResponse noResponse() const;
    // 0 = allow, >0 = whole seconds until this request could next be accepted.
    // The gate consumes the request only when it allows it, so a blocked caller
    // gets a proper 429/Retry-After response and no route side effect.
    int rateGate();
    // Build the refusal for a rate-limited request. It carries backoff fields,
    // including Retry-After, without using any of the caller's request input.
    HttpResponse rateLimitAnswer(int retrySeconds) const;
    // What the two doors ask of at this moment: counts only - never a letter of
    // any credential this machine holds.
    std::string authInfoJson();
    // The door that answers a credential as it is handed to it: whether this
    // machine accepts it at either kind of door, in words that never repeat
    // what was handed. When the rooms would take it, the credential is put on
    // the connection as a cookie so the rest of the visit carries it.
    HttpResponse certifyAnswer(const HttpRequest &req);
    // A door answers in the shape its reader can read. A program that asks with
    // no Accept, or with */*, gets the json this machine has always answered; a
    // browser opening the bare address is a person opening the front room, and
    // gets the room rather than the json.
    bool browserOpeningDocument(const HttpRequest &req) const;

  private:
    // The most client connections these doors hold open at one time. A stated
    // cap rather than a guess at capacity: past it an accepted client is closed
    // at once with no answer, because a server that refuses still stands and a
    // server that runs out of threads does not.
    static constexpr int kMaxLiveConnections = 256;
    // How long stop() waits for the connections it registered to finish the
    // answers they were already carrying. A connection standing past that is
    // working for a client that is no longer waiting, so it is left standing on
    // purpose - detached, and safe only because the services these doors answer
    // from outlive them (the entry point leaves them to the process's end).
    static constexpr int kStopGraceMs = 5000;

    // One connection thread as the accept loop keeps it: its socket, the thread,
    // and the word the thread writes when its work is over. The finished ones
    // are reaped from the registry; the ones still answering stay in it until
    // stop() joins or detaches them.
    struct ConnectionThread
    {
      platform::sock_t sock = platform::kBadSocket;
      std::atomic<bool> finished{false};
      std::thread worker;
    };

    void run(void *listenHandle);
    // The whole life of one client, from this thread's first read to the
    // connection's close: the sink owns the socket, so every exit closes it.
    void handleConnection(platform::sock_t client);
    void connectionThread(const std::shared_ptr<ConnectionThread> &slot);
    // One of the standing connection places, taken before any thread is started;
    // false when all kMaxLiveConnections are held.
    bool reserveConnectionPlace();
    // Threads whose work is over leave the registry: the accept loop calls this
    // so the registry holds the connections standing, not the ones that have
    // gone.
    void reapFinishedConnections();
    // Join the registered threads that have finished, and what still stands
    // past the grace.
    void finishConnectionThreads();
    void streamToClient(void *clientHandle, const std::string &path);
    bool openListener(uint16_t preferredPort, void *&outHandle, uint16_t &outPort);
    void closeListener(void *handle);
    HttpResponse route(const HttpRequest &req);
    HttpResponse serveStatic(const std::string &path, const HttpRequest &req);
    // write the pages this binary carries into its static dir, and start
    // serving those copies; a folder that already holds pages is left as it is
    HttpResponse expandStaticDir();
    HttpResponse serveFile(const std::string &kind, const std::string &name,
                           const HttpRequest &req) const;
    // The key as it stands at this moment: the copy is a snapshot no writer can
    // move under a reader's feet.
    std::shared_ptr<const std::string> apiKey() const
    {
      std::lock_guard<std::mutex> lock(m_apiKeyMutex);
      return m_apiKey;
    }

    std::string m_bindHost;
    std::string m_startRefusal;
    std::string m_staticDir;
    // The TLS ask (blank/off/on/auto) and the folder its certificates stand
    // in; the material itself is built once at start() and read-only after,
    // every connection thread only riding it with its own layer.
    std::string m_tlsMode;
    std::string m_tlsCertDir;
    std::unique_ptr<TlsMaterial> m_tlsMaterial;
    // whether the pages come from the folder outside rather than from the copy
    // carried inside: looked at once, when the server is pointed at its folder,
    // and turned on by the expand door. nothing watches the folder while the
    // server runs; the read is a plain load from another thread's eyes.
    std::atomic<bool> m_externalPages{false};
    std::atomic<uint16_t> m_port{0};
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    void *m_listenHandle = nullptr;
    std::mutex m_lifecycleMutex;
    // The exact number of connection threads standing: one is taken before a
    // thread is started and given back by that thread as it ends, on every road
    // it leaves by. The cap is read from it before the machine is asked for a
    // thread, never after.
    std::atomic<int> m_activeConnections{0};
    std::mutex m_connMutex;
    std::vector<std::shared_ptr<ConnectionThread>> m_connectionThreads;
    imagegen::ImageService *m_image = nullptr;
    project::ProjectService *m_projects = nullptr;
    peer::PeerService m_peers;
    mutable std::mutex m_apiKeyMutex;
    std::shared_ptr<const std::string> m_apiKey{std::make_shared<const std::string>()};
    std::atomic<int> m_rateLimit{600};
    std::mutex m_rateMutex;
    long long m_rateLastMs{0};
    long long m_rateTokens{0};
    bool m_rateInitialized{false};
    std::atomic<int> m_authFailures{0};
    std::atomic<long long> m_lastAuthFailMs{0};
  };

} // namespace ols

#endif // OLS_OPEN_LOCAL_SERVICES_H
