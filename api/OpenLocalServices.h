// Open-local-services: the HTTP server and route table. Foundational code —
// service-specific behavior lives in the service modules (ols::imagegen for
// pictures, ols::voice for speech).
#ifndef OLS_OPEN_LOCAL_SERVICES_H
#define OLS_OPEN_LOCAL_SERVICES_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "PeerService.h"
#include "Platform.h"

namespace ols
{
  // One client of a connection: what handleConnection passes along the
  // streaming doors as one handle. A request in the clear answers through
  // the socket held below; every sender and reader takes this.
  struct ConnectionSink
  {
    platform::sock_t sock;
  };

  // The ols root namespace; each service owns its own sub-namespace: imagegen
  // (image studio) and voice (voice studio, audio.cpp linked in through its
  // C API).
  namespace imagegen
  {
    class ImageService;
  } // namespace imagegen
  namespace voice
  {
    class VoiceService;
  } // namespace voice
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
    void setVoice(voice::VoiceService *voices);

    // Fleet awareness: stand the peer table up beside these doors. The sweep of
    // the networks runs once, in the background, after the doors already
    // answer; the hello door is worked from the route table.
    void startPeers(const std::string &apiKey, const std::string &dataDir);

    HttpResponse jsonError(int status, const std::string &message) const;
    std::string systemJson() const;
    // Whether the pages served come from a folder beside the server or from the
    // copies carried inside this binary, as json text a page can state.
    std::string servedPagesJson() const;

    // Ask the ggml linked into this binary for the machine's devices, now rather
    // than on the first page read, and on Windows name the live Vulkan driver
    // manifest so the device report and the engines agree on the gpu. No plugin
    // library is loaded: the backend this build was asked for is compiled in, and
    // loading a second copy of it registers the same card twice.
    void preloadBackends();
    void setApiKey(const std::string &key) { m_apiKey = key; }
    std::string apiKey() const { return m_apiKey; }
    void setRateLimit(int requestsPerMinute)
    {
      if (requestsPerMinute < 0)
        requestsPerMinute = 600;
      else if (requestsPerMinute > 1000000)
        requestsPerMinute = 1000000;
      m_rateLimit.store(requestsPerMinute);
    }
    int rateLimit() const { return m_rateLimit.load(); }
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
    // browser opening a document gets the same words as a page, because the
    // walk from a refused room lands a person on this door and a person cannot
    // read a json answer.
    bool browserOpeningDocument(const HttpRequest &req) const;

  private:
    void run(void *listenHandle);
    void handleConnection(void *clientHandle);
    void streamToClient(void *clientHandle, const std::string &path);
    bool openListener(uint16_t preferredPort, void *&outHandle, uint16_t &outPort);
    void closeListener(void *handle);
    HttpResponse route(const HttpRequest &req);
    HttpResponse serveStatic(const std::string &path, const HttpRequest &req);
    // write the pages this binary carries into its static dir, and start
    // serving those copies; a folder that already holds pages is left as it is
    HttpResponse expandStaticDir();
    HttpResponse serveFile(const std::string &kind, const std::string &name,
                           const std::string &origin, const HttpRequest &req) const;

    std::string m_bindHost;
    std::string m_startRefusal;
    std::string m_staticDir;
    // whether the pages come from the folder outside rather than from the copy
    // carried inside: looked at once, when the server is pointed at its folder,
    // and turned on by the expand door. nothing watches the folder while the
    // server runs.
    bool m_externalPages = false;
    std::atomic<uint16_t> m_port{0};
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    void *m_listenHandle = nullptr;
    std::mutex m_lifecycleMutex;
    std::atomic<int> m_activeConnections{0};
    imagegen::ImageService *m_image = nullptr;
    voice::VoiceService *m_voice = nullptr;
    peer::PeerService m_peers;
    std::string m_apiKey;
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
