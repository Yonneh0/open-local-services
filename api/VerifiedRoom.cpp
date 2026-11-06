// The verification records of one weights room: what this machine saw about its
// own bytes, kept in one file beside them. The rules a room follows are written
// out in the header.
#include "VerifiedRoom.h"

#include "Common.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ols
{
  namespace room
  {
    using json = nlohmann::json;

    // The one word for the voucher a format leaves its reader when it states no
    // sum of its own: the file's own header says its tensors are all there. It is
    // a weaker word than a card's sum, and it is stated in the row like any other
    // voucher, so a reader can see which of the two a file stands on.
    const char *kVoucherWhole()
    {
      return "whole";
    }

    namespace
    {
      // Every digest in a room is taken at this size. A weight too big to read
      // twice is checked in fixed pieces, so a record states the sum of each
      // piece beside the sum of the whole: at 256 MB a 34 GB shard is 134 sums -
      // a few kilobytes of record for a file that is never read a second time.
      uint64_t chunkBytes()
      {
        static const uint64_t chunk = 256ull * 1024ull * 1024ull;
        return chunk;
      }

      // The map of rooms is guarded apart from the rooms themselves: a method
      // holds one room's lock while it works on that room, and finding the room
      // must never need it.
      std::mutex gRoomsMutex;

      struct RoomState
      {
        explicit RoomState(std::string root_) : root(std::move(root_)) {}

        std::string root;
        std::string recordPath;
        std::mutex mutex;
        // The records as they stand, keyed by a file's place under the room.
        json records = json::object();
        bool recordsRead = false;
      };

      std::map<std::string, std::shared_ptr<RoomState>> gRooms;

      std::shared_ptr<RoomState> stateFor(const std::string &root)
      {
        std::lock_guard<std::mutex> lock(gRoomsMutex);
        const auto found = gRooms.find(root);
        if (found != gRooms.end())
          return found->second;
        auto state = std::make_shared<RoomState>(root);
        state->recordPath = root + "/.verified.json";
        gRooms[root] = state;
        return state;
      }

      // A file's place under its room, which is the key its row is kept under.
      // Rows live in the room itself, so a room named by an absolute path and
      // the same room named from where a server stands are the same rows - what
      // a path spelled from the current folder could not do.
      std::string relOf(const std::string &root, const std::string &path)
      {
        if (path.rfind(root + "/", 0) == 0)
          return path.substr(root.size() + 1);
        return path;
      }

      // The moment a file's bytes were last written, as this machine counts it.
      // Only ever a short cut for "unchanged since it was read".
      std::string touchedOf(const std::string &path)
      {
        std::error_code ec{};
        const auto when = std::filesystem::last_write_time(path, ec);
        if (ec)
          return std::string();
        return std::to_string(when.time_since_epoch().count());
      }

      void ensureRecords(RoomState &state)
      {
        if (state.recordsRead)
          return;
        state.recordsRead = true;
        std::string body;
        if (!readFileText(state.recordPath, body))
          return;
        const json doc = json::parse(body, nullptr, false);
        if (!doc.is_discarded() && doc.is_object())
          state.records = doc;
      }

      bool saveRecords(RoomState &state)
      {
        return writeFileText(state.recordPath, state.records.dump());
      }

      // What this machine can say about a file right now: its length and the
      // sums of its chunks, in one pass over it.
      json digestRow(const std::string &path)
      {
        std::string whole;
        std::vector<std::string> parts;
        uint64_t bytes = 0;
        if (!fileDigests(path, static_cast<size_t>(chunkBytes()), whole, parts, bytes))
          return json();
        json row = json::object();
        row["bytes"] = static_cast<long long>(bytes);
        row["chunk"] = chunkBytes();
        row["whole"] = whole;
        row["parts"] = parts;
        row["touched"] = touchedOf(path);
        return row;
      }

      // The row a file is written down on when nothing vouched for it. Its whole
      // length is kept so the next ask about the same bytes answers without a
      // reading, and so a rewritten file - a different length, or the same length
      // touched later - is asked again.
      json unpaidRow(const std::string &path, long long bytes)
      {
        return json{{"bytes", bytes}, {"unverified", touchedOf(path)}};
      }

      // Stand a row for these bytes, or stand the unpaid one there instead.
      void writeRow(RoomState &state, const std::string &rel, const json &row)
      {
        state.records[rel] = row;
        saveRecords(state);
      }
    } // anonymous namespace

    // A room is named by the folder its rows stand in; the rows themselves are
    // shared, so two modules that work on the same weights room read one set and
    // cannot drift apart.
    VerifiedRoom::VerifiedRoom(const std::string &root) : m_root(root) {}

    VerifiedRoom &roomFor(const std::string &root)
    {
      static std::map<std::string, std::unique_ptr<VerifiedRoom>> rooms;
      std::lock_guard<std::mutex> lock(gRoomsMutex);
      auto &slot = rooms[root];
      if (!slot)
        slot = std::make_unique<VerifiedRoom>(root);
      return *slot;
    }

    bool VerifiedRoom::proved(const std::string &path)
    {
      auto state = stateFor(m_root);
      std::lock_guard<std::mutex> lock(state->mutex);
      ensureRecords(*state);
      if (!fileExists(path))
        return false;
      std::error_code ec{};
      const uintmax_t size = std::filesystem::file_size(path, ec);
      if (ec)
        return false;
      const auto found = state->records.find(relOf(state->root, path));
      if (found == state->records.end() || !found->is_object())
        return false;
      // A proved row stands for these bytes while its length and its date stand
      // for it still; anything else - an unpaid row, a row nothing vouched, a row
      // for other bytes - is answered here without reading a byte of the file.
      const json &row = *found;
      if (!row.contains("parts") || !row.contains("vouch"))
        return false;
      return row.value("bytes", -1LL) == static_cast<long long>(size) &&
             row.value(std::string("touched"), std::string()) == touchedOf(path);
    }

    bool VerifiedRoom::prove(const std::string &path, const std::string &voucher,
                             std::string *wholeHex)
    {
      auto state = stateFor(m_root);
      std::lock_guard<std::mutex> lock(state->mutex);
      ensureRecords(*state);
      if (!fileExists(path))
        return false;
      std::error_code ec{};
      const uintmax_t size = std::filesystem::file_size(path, ec);
      if (ec)
        return false;
      const std::string rel = relOf(state->root, path);
      const std::string tick = touchedOf(path);
      const auto found = state->records.find(rel);
      if (found != state->records.end() && found->is_object() &&
          !found->contains("parts") && found->contains("unverified") &&
          found->value("bytes", -1LL) == static_cast<long long>(size) &&
          found->value(std::string("unverified"), std::string()) == tick)
      {
        // These very bytes were read and refused. Saying so again costs no
        // reading; a rewritten file is a different date, and is read again.
        return false;
      }
      const json row = digestRow(path);
      if (row.is_null())
        return false;
      const std::string whole = row.value("whole", std::string());
      if (wholeHex)
        *wholeHex = whole;
      if (voucher.empty())
      {
        // Nothing stands for these bytes yet. What was read is written down -
        // the sums are true, and the caller may still settle the file is whole
        // and vouch it - but it is not proved until something does.
        writeRow(*state, rel, row);
        return false;
      }
      if (voucher != kVoucherWhole() && voucher != whole)
      {
        writeRow(*state, rel, unpaidRow(path, static_cast<long long>(size)));
        return false;
      }
      json proved = row;
      proved["vouch"] = voucher;
      writeRow(*state, rel, proved);
      return true;
    }

    bool VerifiedRoom::vouch(const std::string &path, const std::string &voucher)
    {
      if (voucher.empty())
        return false;
      auto state = stateFor(m_root);
      std::lock_guard<std::mutex> lock(state->mutex);
      ensureRecords(*state);
      const auto found = state->records.find(relOf(state->root, path));
      if (found == state->records.end() || !found->is_object() ||
          !found->contains("parts"))
        return false;
      // Only bytes standing as they were read can be vouched after the fact: the
      // voucher belongs to the sums that were taken, not to whatever the name
      // holds by the time the caller got around to saying so.
      if (found->value("touched", std::string()) != touchedOf(path))
        return false;
      (*found)["vouch"] = voucher;
      saveRecords(*state);
      return true;
    }
  } // namespace room
} // namespace ols
