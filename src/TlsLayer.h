// The TLS layer: mbedtls terminating a sealed conversation on the very socket
// the accept loop hands over, so the one port answers plain and sealed
// visitors alike (spec §10.3). The process builds its certificate, key and
// config once - the TlsMaterial, read-only after startup - and every
// connection rides it with its own TlsLayer over its own socket. The two bio
// callbacks inside the layer are the only place mbedtls touches the wire.
#ifndef OLS_TLS_LAYER_H
#define OLS_TLS_LAYER_H

#include <memory>
#include <string>

#include "Platform.h"

namespace ols
{
  class TlsMaterial;

  // What a connection rides: the one config, certificate and key this process
  // made at startup. A connection only reads it - mbedtls copies out of a
  // config into each per-connection context - so no lock stands between it
  // and the threads.
  class TlsMaterial
  {
  public:
    // Load the CA and leaf standing in certDir, or - when none stand there
    // and autoGenerate is asked - make an EC P-256 CA and leaf in code,
    // write them under certDir for the next start, and use them. bindHost
    // names the address the port stands at; its address, the machine's own
    // name and its .lan name, localhost and the loopback join the leaf's
    // names. Null with the reason in why when neither road stands; madeFresh
    // says whether these are certificates this start wrote.
    static std::unique_ptr<TlsMaterial> build(const std::string &certDir, bool autoGenerate,
                                              const std::string &bindHost, bool &madeFresh,
                                              std::string &why);

    ~TlsMaterial();
    TlsMaterial(const TlsMaterial &) = delete;
    TlsMaterial &operator=(const TlsMaterial &) = delete;

    // The two facts the start-up line states about the material: the CA's
    // SHA-256 fingerprint in hex, and the day the leaf stops answering -
    // words for the log now, the same words the cert panel will show (a
    // later batch) so the page and the file it describes cannot disagree.
    const std::string &caFingerprint() const { return m_fingerprint; }
    const std::string &leafExpiry() const { return m_leafExpiry; }

  private:
    TlsMaterial();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_fingerprint;
    std::string m_leafExpiry;

    friend class TlsLayer;
  };

  // The sealed half of one connection. The accept loop peeks the visitor's
  // first bytes; when they open a TLS record this layer is built, handshakes
  // with its own cap, and then replaces the socket's own read and write for
  // everything above it.
  class TlsLayer
  {
  public:
    // Which kind of visitor this connection is. A TLS record opens 0x16 0x03;
    // no HTTP method ever does. One MSG_PEEK of two bytes decides, and the
    // bytes stay unread for whichever road follows. False - plain, or a
    // stream that said nothing - leaves the connection to the plain path,
    // which answers a confused sealed visitor the way it answers any
    // unframeable request.
    static bool looksLikeTls(platform::sock_t sock);

    TlsLayer(platform::sock_t sock, const TlsMaterial &material);
    ~TlsLayer();
    TlsLayer(const TlsLayer &) = delete;
    TlsLayer &operator=(const TlsLayer &) = delete;

    // Finish the handshake. Its own ten-second cap lives here: the socket's
    // read timeout bounds each wait inside the bio, this cap bounds the
    // whole, so a client that connects and stalls before its first record
    // holds a connection thread no longer than the loop would tolerate of a
    // plain one. why carries the reason when it returns false.
    bool handshake(std::string &why);

    // Plaintext through the layer. read hands back what came (0 or fewer
    // means the stream ended or gave up, exactly as the plain read it
    // replaces); write takes the bytes whole or says it could not.
    int read(char *out, int length);
    bool write(const char *data, size_t length);

    // End the conversation: close_notify, said once. The sink calls this
    // before letting the socket go.
    void close();

    // What the handshake settled on, for the log line a sealed connection
    // prints - "TLSv1.3 / TLS1-3-AES-256-GCM-SHA384" and the like.
    std::string negotiatedSummary() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace ols

#endif // OLS_TLS_LAYER_H
