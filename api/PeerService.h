// Fleet awareness: this machine keeps tabs on the other open-local-services
// servers it can meet on the lan. A start sweeps the configured networks once -
// only this server's own port, only addresses answered by this server's own key -
// and every meeting writes both sides of the acquaintance: the prober records who
// answered, the answered records whoever asked. The table lives in
// data/peers.json between runs, so a server that wakes beside a running fleet
// greets it from the address book it kept, before the sweep ever reaches it.
//
// Nothing secret rides here beyond what every api door already accepts: the key
// travels as a normal credential and is never written into the table or into a
// card - a row says "this machine answered our key", which is a fact about the
// meeting and never the key itself.
#ifndef OLS_PEER_SERVICE_H
#define OLS_PEER_SERVICE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace ols
{
  namespace peer
  {

    class PeerService
    {
    public:
      PeerService() = default;
      PeerService(const PeerService &) = delete;
      PeerService &operator=(const PeerService &) = delete;

      // Who this machine is and where it stands, beside the key it asks doors
      // with and the folder where the table of acquaintances is kept.
      void setSelf(const std::string &apiKey, const std::string &dataDir, uint16_t port);
      // The live half of this machine's card - hostname, version, backend,
      // uptime - read from the machine the moment the table or a hello needs it,
      // so a card is never a stale copy of a truth that has moved.
      void setCardProvider(std::function<nlohmann::json()> provider)
      {
        m_card = std::move(provider);
      }

      void load();
      // The one sweep of a start, in the background: known acquaintances at
      // their saved addresses first, then every configured network.
      void start();
      void stop();

      // The answer of the hello door: the asker is recorded - seen from this
      // address, holding the card they handed - and this machine's own card
      // answers back. The credential was already weighed at the door: whatever
      // reaches here answered our key.
      nlohmann::json helloAnswer(const std::string &askerIp, const nlohmann::json &given);

      // The table for the pages: this machine's own card beside every
      // acquaintance, each saying where it was met and whether it stood up
      // during this run.
      nlohmann::json tableJson() const;

    private:
      void run();
      void sweepNetwork(unsigned long network, int prefix);
      // One ask of one address: a card answered with the shape of this suite is
      // a peer, and is recorded with the address that answered.
      bool probe(const std::string &address, uint16_t port);
      void recordSeen(const nlohmann::json &card, const std::string &address,
                      const char *via);
      void save() const;

      std::string m_apiKey;
      std::string m_dataFile;
      uint16_t m_port = 0;
      long long m_startSeconds = 0;
      std::function<nlohmann::json()> m_card;

      mutable std::mutex m_mutex;
      nlohmann::json m_rows = nlohmann::json::array();
      // The addresses this machine stands at, read once when the sweep starts:
      // a server never knocks on its own door.
      std::mutex m_ownMutex;
      std::set<unsigned long> m_own;
      std::thread m_thread;
      std::atomic<bool> m_stopping{false};
      std::mutex m_sleepMutex;
      std::condition_variable m_sleepCv;
    };

  } // namespace peer
} // namespace ols

#endif // OLS_PEER_SERVICE_H
