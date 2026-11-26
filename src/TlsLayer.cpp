// The TLS layer over the socket the accept loop hands over - see TlsLayer.h.
// Everything mbedtls is asked from here, and the two bio callbacks are the
// whole of its contact with the wire: they wrap this project's own send and
// receive on the hand-owned socket, and turn the socket's would-block waits
// into mbedtls's WANT_READ/WANT_WRITE, which the callers below spin on.
//
// The certificate road is the spike's, unchanged in shape (SPIKE_REPORT.md
// Appendix A): an EC P-256 CA and leaf made in code with x509write, DER
// landing at the buffer's tail, the leaf's names carried as a linked list.
// What it adds to the spike is where the bytes live: under data/tls/ so the
// next start reads the same certificates instead of making new ones, and
// the validity computed from this moment (825 days ahead) instead of fixed
// words in the source.
#include "TlsLayer.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/md.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

namespace
{
  // One handshake, from hello to keys settled, may spend no more than this -
  // the layer's own cap, taken while the loop's overall deadline only just
  // began. Each individual wait inside it is the socket's shorter handshake
  // timeout, so the cap is checked in whole seconds of spinning, not in one
  // unbroken block; when the handshake ends, the loop's own read patience
  // stands again for every later read.
  constexpr long long kHandshakeCapMs = 10000;
  constexpr int kHandshakeSocketWaitMs = 2000;
  constexpr int kReadPatienceMs = 10000;

  // A generated leaf stands this long: ~825 days (spec §10.3 - the span a
  // person can live with before the cert panel's regenerate button matters).
  constexpr long long kLeafLifetimeSeconds = 825LL * 24 * 60 * 60;

  // The three files one machine's TLS stands on. DER throughout: nothing
  // between these bytes and mbedtls's own parser, no armor to mistranscribe.
  const char kFileCa[] = "ca.der";
  const char kFileLeaf[] = "leaf.der";
  const char kFileKey[] = "leaf.key.der";

  const char kCaDn[] = "CN=open-local-services CA";
  const char kSeed[] = "open-local-services tls";

  std::string errorWords(int rc)
  {
    char text[128] = {0};
    mbedtls_strerror(rc, text, sizeof(text));
    char head[24] = {0};
    std::snprintf(head, sizeof(head), "-0x%04x ", static_cast<unsigned>(-rc));
    return std::string(head) + text;
  }

  bool readFileBytes(const std::filesystem::path &path, std::vector<unsigned char> &out)
  {
    std::ifstream in{path, std::ios::binary};
    if (!in)
      return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad() && !out.empty();
  }

  bool writeFileBytes(const std::filesystem::path &path, const std::vector<unsigned char> &bytes)
  {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out)
      return false;
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return out.good();
  }

  std::string hexOf(const unsigned char *bytes, size_t length)
  {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t at = 0; at < length; ++at)
    {
      out += digits[bytes[at] >> 4];
      out += digits[bytes[at] & 0x0f];
    }
    return out;
  }

  // The date words x509write asks for: UTC "YYYYMMDDHHMMSS", this instant
  // plus the leaf's span. The CA shares them; a CA that outlives its leaf
  // would say nothing a person could act on.
  void validityWords(long long nowSeconds, long long spanSeconds, char *notBefore,
                     char *notAfter)
  {
    const auto stamp = [](long long seconds, char *out, size_t cap)
    {
      const std::time_t when = static_cast<std::time_t>(seconds);
      std::tm parts{};
#ifdef _WIN32
      gmtime_s(&parts, &when);
#else
      gmtime_r(&when, &parts);
#endif
      std::snprintf(out, cap, "%04d%02d%02d%02d%02d%02d", parts.tm_year + 1900, parts.tm_mon + 1,
                    parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
    };
    stamp(nowSeconds, notBefore, 16);
    stamp(nowSeconds + spanSeconds, notAfter, 16);
  }

  // The address this machine would leave by. A datagram socket is pointed at
  // an off-machine port and never sends a word: the route this machine would
  // take decides which of its own addresses answers getsockname, which is
  // the LAN address the port will actually be visited at. A box with no
  // route answers nothing, and its leaf simply carries no such name.
  void routeAddress(std::vector<std::string> &addresses)
  {
    ols::platform::sock_t sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == ols::platform::kBadSocket)
      return;
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    remote.sin_addr.s_addr = htonl(0x08080808u);
    if (ols::platform::connectSocket(sock, remote) != ols::platform::kSocketError)
    {
      sockaddr_in here{};
      socklen_t length = sizeof(here);
      if (getsockname(sock, reinterpret_cast<sockaddr *>(&here), &length) == 0 &&
          here.sin_addr.s_addr != 0)
      {
        char text[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &here.sin_addr, text, sizeof(text)))
        {
          const std::string address = text;
          // loopback wears no name of its own and a down link's 169.254 is a
          // placeholder, not an address anyone visits this port at.
          if (address.compare(0, 4, "127.") != 0 && address.compare(0, 4, "169.") != 0)
            addresses.push_back(address);
        }
      }
    }
    ols::platform::discardSocket(sock);
  }

  // One key and one certificate, made in code. The spike's road unchanged:
  // EC P-256, SHA-256, DER written at the buffer's tail; a fresh random
  // serial for every generation. isCa decides between the two shapes the
  // certificate can wear: a CA that can sign, or a leaf that serves.
  bool makeKeyAndCert(mbedtls_ctr_drbg_context &rng, mbedtls_pk_context &key, const char *dn,
                      const mbedtls_x509_san_list *sans, mbedtls_pk_context *issuerKey,
                      const char *issuerDn, bool isCa, const char *notBefore, const char *notAfter,
                      std::vector<unsigned char> &der, std::string &why)
  {
    mbedtls_pk_init(&key);
    int rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc != 0)
    {
      why = "no P-256 key: " + errorWords(rc);
      return false;
    }
    rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random,
                             &rng);
    if (rc != 0)
    {
      why = "no P-256 key: " + errorWords(rc);
      return false;
    }
    unsigned char serial[8];
    mbedtls_ctr_drbg_random(&rng, serial, sizeof(serial));
    serial[0] &= 0x7f; // a serial is a positive number
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
    mbedtls_x509write_crt_set_subject_name(&crt, dn);
    mbedtls_x509write_crt_set_issuer_name(&crt, issuerDn);
    mbedtls_x509write_crt_set_validity(&crt, notBefore, notAfter);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, issuerKey);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    if (sans != nullptr && mbedtls_x509write_crt_set_subject_alternative_name(&crt, sans) != 0)
    {
      mbedtls_x509write_crt_free(&crt);
      why = std::string("the leaf's names were refused");
      return false;
    }
    if (isCa)
    {
      mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0);
      mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_KEY_CERT_SIGN);
    }
    else
    {
      mbedtls_x509write_crt_set_basic_constraints(&crt, 0, 0);
      mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
      static const unsigned char serverAuth[] = MBEDTLS_OID_SERVER_AUTH;
      const mbedtls_asn1_sequence eku = {{MBEDTLS_ASN1_OID, sizeof(serverAuth) - 1,
                                          const_cast<unsigned char *>(serverAuth)},
                                         nullptr};
      mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku);
    }
    unsigned char store[2048];
    rc = mbedtls_x509write_crt_der(&crt, store, sizeof(store), mbedtls_ctr_drbg_random, &rng);
    mbedtls_x509write_crt_free(&crt);
    if (rc < 0)
    {
      why = "no certificate: " + errorWords(rc);
      return false;
    }
    der.assign(store + sizeof(store) - static_cast<size_t>(rc), store + sizeof(store));
    return true;
  }

  // The names a sealed visitor's browser will accept for this machine: the
  // loopback names, this machine's own name and its .lan name, the address
  // the port was told to stand at (when it named one), and the address this
  // machine would leave by. Duplicates are out - a name stated twice in a
  // certificate says nothing twice - and both kinds ride: DNS and IP.
  struct NameItem
  {
    bool isIp;
    std::string text;
  };

  void collectNames(const std::string &bindHost, std::vector<NameItem> &names)
  {
    const auto add = [&names](bool isIp, const std::string &text)
    {
      if (text.empty())
        return;
      for (const NameItem &held : names)
        if (held.isIp == isIp && held.text == text)
          return;
      names.push_back({isIp, text});
    };
    add(false, "localhost");
    add(true, "127.0.0.1");
    const std::string machine = ols::platform::hostName();
    add(false, machine);
    if (!machine.empty())
      add(false, machine + ".lan");
    if (bindHost != "0.0.0.0" && bindHost != "*" && bindHost != "localhost")
    {
      in_addr parsed{};
      add(inet_pton(AF_INET, bindHost.c_str(), &parsed) == 1, bindHost);
    }
    std::vector<std::string> routes;
    routeAddress(routes);
    for (const std::string &address : routes)
      add(true, address);
  }

  // The SAN list mbedtls writes: one linked node per name, the bytes held
  // alive by the caller's storage. A DNS name's bytes are its text; an IP's
  // are the packed four, which is what mbedtls demands of an iPAddress when
  // it reads the names back (its parser refuses an IP stated any other way,
  // a rule the spike's all-DNS list never met). The node array is sized
  // once, so the links cannot outlive a reallocation.
  struct SanChain
  {
    std::vector<std::vector<unsigned char>> bytes;
    std::vector<mbedtls_x509_san_list> nodes;
    std::vector<int> types;
  };

  void buildSanChain(const std::vector<NameItem> &names, SanChain &chain)
  {
    chain.bytes.reserve(names.size());
    chain.types.reserve(names.size());
    for (const NameItem &item : names)
    {
      if (item.isIp)
      {
        in_addr packed{};
        if (inet_pton(AF_INET, item.text.c_str(), &packed) != 1)
          continue; // an address that will not pack cannot wear a name
        chain.types.push_back(MBEDTLS_X509_SAN_IP_ADDRESS);
        chain.bytes.push_back(std::vector<unsigned char>(
            reinterpret_cast<const unsigned char *>(&packed),
            reinterpret_cast<const unsigned char *>(&packed) + sizeof(packed)));
      }
      else
      {
        chain.types.push_back(MBEDTLS_X509_SAN_DNS_NAME);
        chain.bytes.push_back(std::vector<unsigned char>(item.text.begin(), item.text.end()));
      }
    }
    chain.nodes.resize(chain.bytes.size());
    for (size_t at = 0; at < chain.bytes.size(); ++at)
    {
      chain.nodes[at].node.type = chain.types[at];
      chain.nodes[at].node.san.unstructured_name.p = chain.bytes[at].data();
      chain.nodes[at].node.san.unstructured_name.len = chain.bytes[at].size();
      chain.nodes[at].next = at + 1 < chain.bytes.size() ? &chain.nodes[at + 1] : nullptr;
    }
  }

} // namespace

namespace ols
{
  namespace
  {

    // The two bio callbacks, wrapping the hand-owned socket: the spike's pair
    // on this project's socket API. A blocking socket with a read timeout
    // answers its own timed-out wait as WSAETIMEDOUT on Windows (EAGAIN
    // there); both wear mbedtls's WANT_READ, which the handshake loop spins
    // on within its cap. A hard failure says so with mbedtls's own word.
    int bio_send(void *ctx, const unsigned char *buf, size_t len)
    {
      const int n = platform::sendBytes(platform::fromHandle(ctx),
                                        reinterpret_cast<const char *>(buf),
                                        static_cast<int>(len), 0);
      if (n == platform::kSocketError)
      {
        const int err = platform::lastError();
        if (err == platform::kWouldBlock || err == platform::kInterrupt)
          return MBEDTLS_ERR_SSL_WANT_WRITE;
#ifdef _WIN32
        if (err == WSAETIMEDOUT)
          return MBEDTLS_ERR_SSL_WANT_WRITE;
#endif
        return MBEDTLS_ERR_NET_SEND_FAILED;
      }
      return n;
    }

    int bio_recv(void *ctx, unsigned char *buf, size_t len)
    {
      const int n = platform::receiveBytes(platform::fromHandle(ctx),
                                           reinterpret_cast<char *>(buf),
                                           static_cast<int>(len), 0);
      if (n == platform::kSocketError)
      {
        const int err = platform::lastError();
        if (err == platform::kWouldBlock || err == platform::kInterrupt)
          return MBEDTLS_ERR_SSL_WANT_READ;
#ifdef _WIN32
        if (err == WSAETIMEDOUT)
          return MBEDTLS_ERR_SSL_WANT_READ;
#endif
        return MBEDTLS_ERR_NET_RECV_FAILED;
      }
      return n;
    }

  } // namespace

  // ---------------------------------------------------------------- material

  struct TlsMaterial::Impl
  {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_x509_crt leaf;
    mbedtls_pk_context leafKey;
    bool rngSeeded = false;
    bool confBuilt = false;
  };

  TlsMaterial::TlsMaterial() : m_impl(new Impl())
  {
  }

  std::unique_ptr<TlsMaterial> TlsMaterial::build(const std::string &certDir, bool autoGenerate,
                                                  const std::string &bindHost, bool &madeFresh,
                                                  std::string &why)
  {
    madeFresh = false;
    why.clear();
    std::unique_ptr<TlsMaterial> material{new TlsMaterial()};
    Impl &impl = *material->m_impl;
    mbedtls_entropy_init(&impl.entropy);
    mbedtls_ctr_drbg_init(&impl.rng);
    mbedtls_ssl_config_init(&impl.conf);
    mbedtls_x509_crt_init(&impl.ca);
    mbedtls_x509_crt_init(&impl.leaf);
    mbedtls_pk_init(&impl.leafKey);

    if (const int rc = mbedtls_ctr_drbg_seed(&impl.rng, mbedtls_entropy_func, &impl.entropy,
                                             reinterpret_cast<const unsigned char *>(kSeed),
                                             sizeof(kSeed) - 1))
    {
      why = "no random: " + errorWords(rc);
      return nullptr;
    }
    impl.rngSeeded = true;

    const std::filesystem::path folder{certDir};
    std::vector<unsigned char> caDer, leafDer, keyDer;
    const bool allThree =
        readFileBytes(folder / kFileCa, caDer) && readFileBytes(folder / kFileLeaf, leafDer) &&
        readFileBytes(folder / kFileKey, keyDer);
    bool standing = false;
    if (allThree)
    {
      int rc = mbedtls_x509_crt_parse_der(&impl.ca, caDer.data(), caDer.size());
      if (rc == 0)
        rc = mbedtls_x509_crt_parse_der(&impl.leaf, leafDer.data(), leafDer.size());
      if (rc == 0)
        rc = mbedtls_pk_parse_key(&impl.leafKey, keyDer.data(), keyDer.size(), nullptr, 0,
                                  mbedtls_ctr_drbg_random, &impl.rng);
      if (rc == 0)
        rc = mbedtls_pk_check_pair(&impl.leaf.pk, &impl.leafKey, mbedtls_ctr_drbg_random,
                                   &impl.rng);
      if (rc != 0)
      {
        why = "the certificates standing in " + certDir + " do not read as one: " + errorWords(rc);
      }
      else if (mbedtls_x509_time_is_past(&impl.leaf.valid_to))
      {
        why = "the certificate in " + certDir + " has passed its expiry";
      }
      else
      {
        standing = true;
      }
    }

    if (!standing)
    {
      if (!autoGenerate)
      {
        if (why.empty())
          why = "no certificates stand in " + certDir;
        return nullptr;
      }
      // Make the pair in code and let it stand on disk for every start
      // after this one. The CA's key is used for the one signature below
      // and then let go: only the public half rides on the file, which is
      // what the download-CA door will hand out.
      std::vector<NameItem> names;
      collectNames(bindHost, names);
      SanChain chain;
      buildSanChain(names, chain);
      char notBefore[16], notAfter[16];
      validityWords(static_cast<long long>(std::time(nullptr)), kLeafLifetimeSeconds, notBefore,
                    notAfter);
      const std::string leafDn =
          "CN=" + (platform::hostName().empty() ? std::string("localhost") : platform::hostName());
      std::vector<unsigned char> caDerFresh, leafDerFresh, keyDerFresh;
      mbedtls_pk_context caKey;
      mbedtls_pk_init(&caKey);
      if (makeKeyAndCert(impl.rng, caKey, kCaDn, nullptr, &caKey, kCaDn, true, notBefore, notAfter,
                         caDerFresh, why) &&
          makeKeyAndCert(impl.rng, impl.leafKey, leafDn.c_str(), chain.nodes.data(), &caKey, kCaDn,
                         false, notBefore, notAfter, leafDerFresh, why))
      {
        unsigned char keyStore[2048];
        const int keyLen = mbedtls_pk_write_key_der(&impl.leafKey, keyStore, sizeof(keyStore));
        if (keyLen > 0)
          keyDerFresh.assign(keyStore + sizeof(keyStore) - static_cast<size_t>(keyLen),
                             keyStore + sizeof(keyStore));
        std::error_code mkEc;
        std::filesystem::create_directories(folder, mkEc);
        if (keyLen > 0 && !mkEc && writeFileBytes(folder / kFileCa, caDerFresh) &&
            writeFileBytes(folder / kFileLeaf, leafDerFresh) &&
            writeFileBytes(folder / kFileKey, keyDerFresh))
        {
          // Read the written bytes back: the material answers from the same
          // files every later start reads, never from a copy generation kept
          // warm in RAM beside them.
          mbedtls_x509_crt_free(&impl.ca);
          mbedtls_x509_crt_init(&impl.ca);
          mbedtls_x509_crt_free(&impl.leaf);
          mbedtls_x509_crt_init(&impl.leaf);
          int rc = mbedtls_x509_crt_parse_der(&impl.ca, caDerFresh.data(), caDerFresh.size());
          if (rc == 0)
            rc = mbedtls_x509_crt_parse_der(&impl.leaf, leafDerFresh.data(), leafDerFresh.size());
          if (rc != 0)
            why = "the fresh certificates do not read back: " + errorWords(rc);
          else
          {
            caDer = caDerFresh; // the fingerprint is taken of the bytes that stand
            standing = true;
          }
        }
        else if (why.empty())
          why = "the certificate files could not be written under " + certDir;
      }
      mbedtls_pk_free(&caKey);
      if (!standing)
        return nullptr;
      madeFresh = true;
    }

    if (const int rc = mbedtls_ssl_config_defaults(&impl.conf, MBEDTLS_SSL_IS_SERVER,
                                                   MBEDTLS_SSL_TRANSPORT_STREAM,
                                                   MBEDTLS_SSL_PRESET_DEFAULT))
    {
      why = "no TLS config: " + errorWords(rc);
      return nullptr;
    }
    impl.confBuilt = true;
    // TLS 1.2 floor; the 3.6 default preset above already prefers 1.3, keeps
    // X25519 and AEAD-only suites, and refuses the old shapes (spec §10.3).
    mbedtls_ssl_conf_min_tls_version(&impl.conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(&impl.conf, mbedtls_ctr_drbg_random, &impl.rng);
    if (const int rc = mbedtls_ssl_conf_own_cert(&impl.conf, &impl.leaf, &impl.leafKey))
    {
      why = "the leaf will not stand at a TLS config: " + errorWords(rc);
      return nullptr;
    }

    unsigned char sum[32];
    if (const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256))
    {
      if (mbedtls_md(sha256, caDer.data(), caDer.size(), sum) == 0)
        material->m_fingerprint = hexOf(sum, sizeof(sum));
    }
    material->m_leafExpiry =
        std::to_string(impl.leaf.valid_to.year) + "-" +
        std::to_string(impl.leaf.valid_to.mon) + "-" + std::to_string(impl.leaf.valid_to.day);
    return material;
  }

  TlsMaterial::~TlsMaterial()
  {
    if (!m_impl)
      return;
    Impl &impl = *m_impl;
    if (impl.confBuilt)
      mbedtls_ssl_config_free(&impl.conf);
    mbedtls_x509_crt_free(&impl.ca);
    mbedtls_x509_crt_free(&impl.leaf);
    mbedtls_pk_free(&impl.leafKey);
    if (impl.rngSeeded)
      mbedtls_ctr_drbg_free(&impl.rng);
    mbedtls_entropy_free(&impl.entropy);
  }

  // -------------------------------------------------------------- the layer

  struct TlsLayer::Impl
  {
    platform::sock_t sock = platform::kBadSocket;
    mbedtls_ssl_context ssl;
    bool ready = false;
    bool notified = false;
  };

  bool TlsLayer::looksLikeTls(platform::sock_t sock)
  {
    unsigned char head[2] = {0, 0};
    size_t seen = 0;
    while (seen < sizeof(head))
    {
      const int got = platform::receiveBytes(sock, reinterpret_cast<char *>(head) + seen,
                                             static_cast<int>(sizeof(head) - seen), MSG_PEEK);
      if (got <= 0)
        break;
      seen += static_cast<size_t>(got);
    }
    return seen == sizeof(head) && head[0] == 0x16 && head[1] == 0x03;
  }

  TlsLayer::TlsLayer(platform::sock_t sock, const TlsMaterial &material) : m_impl(new Impl())
  {
    m_impl->sock = sock;
    mbedtls_ssl_init(&m_impl->ssl);
    m_impl->ready = mbedtls_ssl_setup(&m_impl->ssl, &material.m_impl->conf) == 0;
    if (m_impl->ready)
      mbedtls_ssl_set_bio(&m_impl->ssl, platform::toHandle(sock), bio_send, bio_recv, nullptr);
  }

  TlsLayer::~TlsLayer()
  {
    mbedtls_ssl_free(&m_impl->ssl);
  }

  bool TlsLayer::handshake(std::string &why)
  {
    if (!m_impl->ready)
    {
      why = "no TLS context for this connection";
      return false;
    }
    platform::setReadTimeout(m_impl->sock, kHandshakeSocketWaitMs);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kHandshakeCapMs);
    int rc = 0;
    while ((rc = mbedtls_ssl_handshake(&m_impl->ssl)) != 0)
    {
      if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
      {
        if (std::chrono::steady_clock::now() >= deadline)
        {
          rc = 0; // the cap ends it; the words below are the honest reason
          why = "the handshake waited its ten seconds on a client that said nothing more";
          break;
        }
        continue; // spins on the socket's short wait, like the loop would
      }
      break;
    }
    platform::setReadTimeout(m_impl->sock, kReadPatienceMs); // the loop's patience again
    if (why.empty() && rc != 0)
      why = "the handshake failed: " + errorWords(rc);
    return why.empty();
  }

  int TlsLayer::read(char *out, int length)
  {
    // A WANT here is the socket's own wait (no bytes, or only part of a
    // record so far). Three waits of the loop's patience, then this reads as
    // an ended stream - the way a plain read past its timeout ends it, so
    // the deadline above still speaks for the connection, not this layer.
    for (int waits = 0; waits < 3; ++waits)
    {
      const int rc = mbedtls_ssl_read(&m_impl->ssl, reinterpret_cast<unsigned char *>(out),
                                      static_cast<size_t>(length));
      if (rc > 0)
        return rc;
      if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_CONN_EOF)
        return 0;
      if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE ||
          rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
        continue;
      return -1;
    }
    return 0;
  }

  bool TlsLayer::write(const char *data, size_t length)
  {
    size_t sent = 0;
    while (sent < length)
    {
      const int rc = mbedtls_ssl_write(
          &m_impl->ssl, reinterpret_cast<const unsigned char *>(data) + sent, length - sent);
      if (rc > 0)
      {
        sent += static_cast<size_t>(rc);
        continue;
      }
      if (rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_WANT_READ)
        continue; // the blocking socket waited inside already; go around
      return false;
    }
    return true;
  }

  void TlsLayer::close()
  {
    if (m_impl->notified)
      return;
    m_impl->notified = true;
    mbedtls_ssl_close_notify(&m_impl->ssl);
  }

  std::string TlsLayer::negotiatedSummary() const
  {
    const char *version = mbedtls_ssl_get_version(&m_impl->ssl);
    const char *suite = mbedtls_ssl_get_ciphersuite(&m_impl->ssl);
    return std::string(version ? version : "?") + " / " + (suite ? suite : "?");
  }

} // namespace ols
