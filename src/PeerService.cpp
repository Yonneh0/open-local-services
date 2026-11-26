// Fleet awareness: see PeerService.h. One sweep at startup over the machine's
// own networks, a hello door that records whoever asks, and a table kept in
// data/peers.json between runs.
#include "PeerService.h"

#include "Platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/select.h>
#endif

#ifndef _WIN32
#include <fcntl.h>
#endif

namespace ols
{
  namespace peer
  {

    namespace
    {

      constexpr int kConnectTimeoutMs = 400;
      constexpr int kReadTimeoutMs = 900;
      // The sweep walks the whole of a network up to this many addresses and
      // stops counting after it, so a machine wearing a wide netmask (a /8 is
      // not unknown in a lab) costs an evening of patience and nothing more.
      constexpr unsigned long kSweepCap = 1024;
      constexpr int kSweepWorkers = 48;
      constexpr size_t kAnswerCap = 64 * 1024;

      long long nowSeconds()
      {
        return static_cast<long long>(std::time(nullptr));
      }

      bool toAddress(const std::string &text, unsigned long &out)
      {
        in_addr parsed{};
        if (inet_pton(AF_INET, text.c_str(), &parsed) != 1)
          return false;
        out = ntohl(parsed.s_addr);
        return true;
      }

      std::string toText(unsigned long address)
      {
        in_addr packed{};
        packed.s_addr = htonl(address);
        char text[INET_ADDRSTRLEN] = {0};
        if (!inet_ntop(AF_INET, &packed, text, sizeof(text)))
          return {};
        return text;
      }

      bool isLoopbackText(const std::string &text)
      {
        return text.compare(0, 4, "127.") == 0;
      }

      // Addresses this machine stands at, and the networks they sit on. Only
      // running, non-loopback, non-link-local IPv4 interfaces count: a sweep of
      // a down interface's old network would be a sweep of guesswork.
      void localInterfaces(std::vector<std::pair<unsigned long, int>> &networks,
                           std::set<unsigned long> &ownAddresses)
      {
#ifdef _WIN32
        unsigned long size = 16 * 1024;
        std::vector<unsigned char> buffer(size);
        IP_ADAPTER_ADDRESSES *first = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME,
                                 nullptr, first, &size) != ERROR_SUCCESS)
          return;
        for (const IP_ADAPTER_ADDRESSES *adapter = first; adapter; adapter = adapter->Next)
        {
          if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
          for (const IP_ADAPTER_UNICAST_ADDRESS *one = adapter->FirstUnicastAddress; one;
               one = one->Next)
          {
            if (one->Address.lpSockaddr->sa_family != AF_INET)
              continue;
            const auto *sock =
                reinterpret_cast<const sockaddr_in *>(one->Address.lpSockaddr);
            const unsigned long address = ntohl(sock->sin_addr.s_addr);
            ownAddresses.insert(address);
            const int prefix = static_cast<int>(one->OnLinkPrefixLength);
            if (prefix < 1 || prefix > 30)
              continue;
            if ((address >> 24) == 127 || (address >> 24) == 169)
              continue; // loopback and link-local wear no fleet
            const unsigned long hostBits = 32 - static_cast<unsigned long>(prefix);
            networks.push_back({address & ~(0xffffffffUL << hostBits), prefix});
          }
        }
#else
        ifaddrs *rows = nullptr;
        if (getifaddrs(&rows) != 0)
          return;
        for (const ifaddrs *row = rows; row; row = row->ifa_next)
        {
          if (!row->ifa_addr || row->ifa_addr->sa_family != AF_INET || !row->ifa_netmask)
            continue;
          if (!(row->ifa_flags & IFF_UP) || !(row->ifa_flags & IFF_RUNNING) ||
              (row->ifa_flags & IFF_LOOPBACK))
            continue;
          const auto *sock = reinterpret_cast<const sockaddr_in *>(row->ifa_addr);
          const auto *mask = reinterpret_cast<const sockaddr_in *>(row->ifa_netmask);
          const unsigned long address = ntohl(sock->sin_addr.s_addr);
          const unsigned long maskBits = ntohl(mask->sin_addr.s_addr);
          ownAddresses.insert(address);
          int prefix = 0;
          for (unsigned long walk = maskBits; walk; walk <<= 1)
            ++prefix;
          if (prefix < 1 || prefix > 30)
            continue;
          if ((address >> 24) == 127 || (address >> 24) == 169)
            continue;
          networks.push_back({address & maskBits, prefix});
        }
        freeifaddrs(rows);
#endif
      }

      void setNonBlocking(platform::sock_t sock, bool on)
      {
#ifdef _WIN32
        unsigned long mode = on ? 1UL : 0UL;
        ioctlsocket(sock, FIONBIO, &mode);
#else
        const int flags = fcntl(sock, F_GETFL, 0);
        if (flags >= 0)
          fcntl(sock, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
      }

      std::string urlEncode(const std::string &text)
      {
        std::string out;
        for (const char c : text)
        {
          const unsigned char u = static_cast<unsigned char>(c);
          if (std::isalnum(u) || c == '-' || c == '.' || c == '_' || c == '~')
            out.push_back(c);
          else
          {
            char pad[4];
            std::snprintf(pad, sizeof(pad), "%%%02X", u);
            out += pad;
          }
        }
        return out;
      }

      // One plain http get with patience of its own: a closed port says so in a
      // moment and an address where nobody lives says it after the connect
      // timeout - either way the sweep keeps walking. The answer is taken whole
      // from the wire; the caller decides whether its shape belongs to this
      // suite.
      bool getJson(const std::string &address, uint16_t port, const std::string &path,
                   const std::string &key, nlohmann::json &out)
      {
        if (!platform::initSockets())
          return false;
        unsigned long packed = 0;
        if (!toAddress(address, packed))
          return false;
        const platform::sock_t sock = platform::newStreamSocket();
        if (sock == platform::kBadSocket)
          return false;

        bool connected = false;
        setNonBlocking(sock, true);
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(port);
        target.sin_addr.s_addr = htonl(packed);
        if (connect(sock, reinterpret_cast<const sockaddr *>(&target), sizeof(target)) == 0)
          connected = true;
        else
        {
          const int failure = platform::lastError();
#ifdef _WIN32
          const bool travelling = failure == WSAEWOULDBLOCK || failure == WSAEINPROGRESS;
#else
          const bool travelling = failure == EINPROGRESS || failure == EAGAIN || failure == EINTR;
#endif
          if (travelling)
          {
            fd_set writing{}, excepts{};
            FD_ZERO(&writing);
            FD_ZERO(&excepts);
            FD_SET(sock, &writing);
            FD_SET(sock, &excepts);
            timeval patience{};
            patience.tv_sec = kConnectTimeoutMs / 1000;
            patience.tv_usec = (kConnectTimeoutMs % 1000) * 1000;
            const int ready = select(static_cast<int>(sock) + 1, nullptr, &writing, &excepts,
                                     &patience);
            if (ready > 0 && FD_ISSET(sock, &writing) && !FD_ISSET(sock, &excepts))
            {
              int soError = 0;
              socklen_t length = sizeof(soError);
              getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soError), &length);
              connected = soError == 0;
            }
          }
        }
        if (!connected)
        {
          platform::discardSocket(sock);
          return false;
        }
        setNonBlocking(sock, false);
        platform::setWriteTimeout(sock, kReadTimeoutMs);
        platform::setReadTimeout(sock, kReadTimeoutMs);

        std::string request = "GET " + path + " HTTP/1.1\r\n";
        request += "Host: " + address + ":" + std::to_string(port) + "\r\n";
        request += "User-Agent: open-local-services fleet\r\n";
        request += "Accept: application/json\r\n";
        if (!key.empty())
          request += "X-Api-Key: " + key + "\r\n";
        request += "Connection: close\r\n\r\n";
        size_t sent = 0;
        while (sent < request.size())
        {
          const int wrote = platform::sendBytes(sock, request.data() + sent,
                                                static_cast<int>(request.size() - sent), 0);
          if (wrote <= 0)
          {
            platform::discardSocket(sock);
            return false;
          }
          sent += static_cast<size_t>(wrote);
        }

        std::string wire;
        char pad[4096];
        for (;;)
        {
          const int got = platform::receiveBytes(sock, pad, sizeof(pad), 0);
          if (got <= 0)
            break;
          wire.append(pad, static_cast<size_t>(got));
          if (wire.size() > kAnswerCap)
            break;
        }
        platform::closeSocket(sock);

        const size_t bodyAt = wire.find("\r\n\r\n");
        if (bodyAt == std::string::npos || wire.compare(0, 12, "HTTP/1.1 200") != 0)
          return false;
        out = nlohmann::json::parse(wire.substr(bodyAt + 4), nullptr, false);
        return !out.is_discarded();
      }

    } // namespace

    void PeerService::setSelf(const std::string &apiKey, const std::string &dataDir,
                              uint16_t port)
    {
      m_apiKey = apiKey;
      m_dataFile = (std::filesystem::path(dataDir) / "peers.json").string();
      m_port = port;
      m_startSeconds = nowSeconds();
    }

    void PeerService::load()
    {
      std::ifstream file(m_dataFile);
      if (!file)
        return;
      const nlohmann::json kept = nlohmann::json::parse(file, nullptr, false);
      if (kept.is_discarded() || !kept.is_object() || !kept.contains("peers") ||
          !kept["peers"].is_array())
        return;
      std::lock_guard<std::mutex> guard(m_mutex);
      m_rows = nlohmann::json::array();
      for (const nlohmann::json &row : kept["peers"])
      {
        if (!row.is_object() || !row.value("name", std::string()).size())
          continue;
        nlohmann::json fresh = row;
        fresh["online"] = false; // a remembered friend is not a standing one
        m_rows.push_back(fresh);
      }
    }

    void PeerService::start()
    {
      m_thread = std::thread([this]
                             { run(); });
    }

    void PeerService::stop()
    {
      m_stopping.store(true);
      m_sleepCv.notify_all();
      if (m_thread.joinable())
        m_thread.join();
      save();
    }

    nlohmann::json PeerService::helloAnswer(const std::string &askerIp,
                                            const nlohmann::json &given)
    {
      if (!askerIp.empty() && askerIp != "unknown" && !isLoopbackText(askerIp))
        recordSeen(given, askerIp, "hello");
      nlohmann::json card = m_card ? m_card() : nlohmann::json::object();
      card["ols"] = "peer-card";
      return card;
    }

    nlohmann::json PeerService::tableJson() const
    {
      nlohmann::json card = m_card ? m_card() : nlohmann::json::object();
      card["ols"] = "peer-card";
      card["port"] = m_port;
      nlohmann::json out;
      out["self"] = card;
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        out["peers"] = m_rows;
      }
      // The standing ones first, the recently met before the long unseen.
      std::sort(out["peers"].begin(), out["peers"].end(), [](const nlohmann::json &a, const nlohmann::json &b)
                {
        const bool onlineA = a.value("online", false);
        const bool onlineB = b.value("online", false);
        if (onlineA != onlineB)
          return onlineA;
        return a.value("seen", 0ll) > b.value("seen", 0ll); });
      return out;
    }

    void PeerService::run()
    {
      // A breath first: the sweep belongs after the first pages are answered,
      // not in their way.
      {
        std::unique_lock<std::mutex> lock(m_sleepMutex);
        m_sleepCv.wait_for(lock, std::chrono::milliseconds(1200), [this]
                           { return m_stopping.load(); });
      }
      if (m_stopping.load())
        return;

      std::vector<std::pair<unsigned long, int>> networks;
      {
        std::lock_guard<std::mutex> guard(m_ownMutex);
        localInterfaces(networks, m_own);
      }

      std::vector<std::pair<std::string, uint16_t>> known;
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        for (const nlohmann::json &row : m_rows)
        {
          uint16_t port = static_cast<uint16_t>(row.value("port", 0));
          if (!port)
            continue;
          if (row.contains("addrs") && row["addrs"].is_array())
            for (const nlohmann::json &addr : row["addrs"])
              if (addr.is_string())
                known.push_back({addr.get<std::string>(), port});
        }
      }
      // The address book first: a fleet that has been met is greeted at the
      // addresses it kept, whatever netmasks this machine happens to wear.
      for (const auto &one : known)
      {
        if (m_stopping.load())
          return;
        probe(one.first, one.second);
      }

      std::sort(networks.begin(), networks.end());
      networks.erase(std::unique(networks.begin(), networks.end()), networks.end());
      for (const auto &net : networks)
      {
        if (m_stopping.load())
          return;
        sweepNetwork(net.first, net.second);
      }
    }

    void PeerService::sweepNetwork(unsigned long network, int prefix)
    {
      const unsigned long hostBits = 32 - static_cast<unsigned long>(prefix);
      const unsigned long hosts = (hostBits >= 31) ? 0UL : ((1UL << hostBits) - 2UL);
      const unsigned long walk = hosts < kSweepCap ? hosts : kSweepCap;
      std::atomic<unsigned long> next{1};
      std::atomic<bool> stopping{false};
      std::vector<std::thread> workers;
      for (int i = 0; i < kSweepWorkers; ++i)
        workers.push_back(std::thread([this, network, walk, &next, &stopping]
                                      {
                                        for (;;)
                                        {
                                          if (m_stopping.load() || stopping.load())
                                            return;
                                          const unsigned long at = next.fetch_add(1);
                                          if (at > walk)
                                            return;
                                          const std::string address = toText(network + at);
                                          std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                          probe(address, m_port);
                                        } }));
      for (std::thread &worker : workers)
        worker.join();
    }

    bool PeerService::probe(const std::string &address, uint16_t port)
    {
      // Only the machine's own addresses stand down on its own port: a second
      // server of this suite standing beside this one on the same machine is a
      // friend worth meeting, and answers its own door.
      unsigned long packed = 0;
      if (port == m_port && toAddress(address, packed))
      {
        std::lock_guard<std::mutex> guard(m_ownMutex);
        if (m_own.count(packed))
          return false;
      }
      nlohmann::json mine = m_card ? m_card() : nlohmann::json::object();
      std::string path = "/api/peers/hello?name=" + urlEncode(mine.value("name", std::string())) +
                         "&port=" + std::to_string(m_port) +
                         "&version=" + urlEncode(mine.value("version", std::string())) +
                         "&backend=" + urlEncode(mine.value("backend", std::string()));
      nlohmann::json answer;
      if (!getJson(address, port, path, m_apiKey, answer))
        return false;
      // A peer is a peer by shape: a card of this suite answers with its name
      // under the marker. Anything else that lives on this port is someone
      // else's house and stays off the table.
      if (!answer.is_object() || answer.value("ols", std::string()) != "peer-card" ||
          !answer.contains("name"))
        return false;
      recordSeen(answer, address, "sweep");
      return true;
    }

    void PeerService::recordSeen(const nlohmann::json &card, const std::string &address,
                                 const char *via)
    {
      if (!card.is_object())
        return;
      nlohmann::json mine = m_card ? m_card() : nlohmann::json::object();
      const std::string name = card.value("name", address);
      const uint16_t port = static_cast<uint16_t>(card.value("port", 0));
      if (name.empty())
        return;
      if (name == mine.value("name", std::string()) && port == m_port)
        return; // this is this machine, not a peer
      std::string greeting;
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        nlohmann::json *row = nullptr;
        for (nlohmann::json &one : m_rows)
        {
          if (one.value("name", std::string()) == name &&
              one.value("port", 0) == static_cast<int>(port))
          {
            row = &one;
            break;
          }
        }
        if (!row)
        {
          nlohmann::json fresh;
          fresh["name"] = name;
          fresh["port"] = port;
          fresh["addrs"] = nlohmann::json::array();
          m_rows.push_back(fresh);
          row = &m_rows[m_rows.size() - 1];
        }
        nlohmann::json addrs = nlohmann::json::array();
        addrs.push_back(address);
        if (row->contains("addrs") && (*row)["addrs"].is_array())
          for (const nlohmann::json &old : (*row)["addrs"])
            if (old.is_string() && old.get<std::string>() != address &&
                addrs.size() < 4)
              addrs.push_back(old);
        (*row)["addrs"] = addrs;
        if (card.contains("version"))
          (*row)["version"] = card["version"];
        if (card.contains("backend"))
          (*row)["backend"] = card["backend"];
        if (card.contains("uptime_s"))
          (*row)["uptime_s"] = card["uptime_s"];
        (*row)["online"] = true;
        (*row)["seen"] = nowSeconds();
        (*row)["via"] = via;
        greeting = (*row)["version"].is_string() ? (*row)["version"].get<std::string>() : "";
      }
      std::cout << "fleet: met " << (name.empty() ? address : name) << " at " << address << ":"
                << port << (greeting.empty() ? "" : " (" + greeting + ")") << " by " << via
                << std::endl;
      save();
    }

    void PeerService::save() const
    {
      std::error_code ec{};
      std::filesystem::create_directories(std::filesystem::path(m_dataFile).parent_path(), ec);
      const std::string temp = m_dataFile + ".part";
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        nlohmann::json out;
        out["peers"] = m_rows;
        std::ofstream file(temp, std::ios::binary);
        if (!file)
          return;
        file << out.dump(2) << "\n";
      }
      std::filesystem::rename(temp, m_dataFile, ec);
    }

  } // namespace peer
} // namespace ols
