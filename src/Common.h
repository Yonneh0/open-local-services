#pragma once
// The helpers every module of this server shares: reading words out of a json
// value, base64 decoding, asking what a file is, reading and replacing a text
// file, the job clock and the state words a job wears, SHA-256 over a file, and
// where a model's files stand in the weights tree.

// Reading words out of a json value. A page's words and an engine's answer are
// read the same way everywhere: a string comes out only when the value really
// holds words, because these reads run on the thread serving a request and a
// thrown json error there would end the whole server.

#include <nlohmann/json.hpp>
#include <string>

namespace ols
{
  // One piece of words out of what an engine stated or a page sent: a value of
  // another shape reads as nothing rather than throwing.
  inline std::string statedString(const nlohmann::json &at, const char *key)
  {
    if (!at.is_object() || !at.contains(key) || !at.at(key).is_string())
      return std::string();
    return at.at(key).get<std::string>();
  }

  // A whole number out of what an engine stated or a record holds: only a value
  // that really holds a whole number counts, so a value of another shape reads
  // as nothing and cannot end the thread this read runs on.
  inline long long statedCount(const nlohmann::json &at, const char *key)
  {
    if (!at.is_object() || !at.contains(key) || !at.at(key).is_number_integer())
      return 0;
    return at.at(key).get<long long>();
  }

  // A count that may carry a fraction - seconds of speech, a rate.
  inline double statedMeasure(const nlohmann::json &at, const char *key)
  {
    if (!at.is_object() || !at.contains(key) || !at.at(key).is_number())
      return 0.0;
    return at.at(key).get<double>();
  }

} // namespace ols

// Base64 decoding for two paths: the voice module reads audio a page sends as
// base64, and the image studio reads back a picture the browser kept (a dragged
// file or a pasted screenshot), which has no path on this machine and arrives
// as bytes only. Characters outside the alphabet are skipped, so input broken
// over lines still decodes.

#include <string>

namespace ols
{
  inline std::string decodeBase64(const std::string &encoded)
  {
    auto sextet = [](char c) -> int
    {
      if (c >= 'A' && c <= 'Z')
        return c - 'A';
      if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
      if (c >= '0' && c <= '9')
        return c - '0' + 52;
      if (c == '+')
        return 62;
      if (c == '/')
        return 63;
      return -1;
    };
    // Decoding in one pass: the bytes are written as the sextets arrive, so a
    // picture or an audio clip costs only its own decoded size here, not a
    // second table holding every sextet of the input.
    std::string out;
    out.reserve(encoded.size() / 4u * 3u);
    unsigned gathered = 0;
    unsigned held = 0;
    for (const char c : encoded)
    {
      const int v = sextet(c);
      if (v < 0)
        continue;
      gathered = (gathered << 6) | static_cast<unsigned>(v);
      ++held;
      if (held == 4u)
      {
        out.push_back(static_cast<char>((gathered >> 16) & 0xFFu));
        out.push_back(static_cast<char>((gathered >> 8) & 0xFFu));
        out.push_back(static_cast<char>(gathered & 0xFFu));
        gathered = 0;
        held = 0;
      }
    }
    // A group that stopped early holds its whole bytes at the top: the sextets
    // are shifted into the place a full group would have put them, and only the
    // bytes they carry in full are written.
    if (held > 1u)
    {
      const unsigned aligned = gathered << (6u * (4u - held));
      for (unsigned at = 0u; at + 1u < held; ++at)
      {
        out.push_back(static_cast<char>((aligned >> (16u - 8u * at)) & 0xFFu));
      }
    }
    return out;
  }
}

// What a file this machine holds is asked: whether an ordinary file is there,
// and how many bytes it holds. Every module reads these the same way, so a
// model's files cannot be seen as present on one page and missing on another.

#include <filesystem>
#include <system_error>

namespace ols
{
  // An ordinary file, and nothing else: a folder, a device, or a name that is
  // not there all read as no file. One look answers both the presence and the
  // shape, which a pair of separate checks used to ask twice.
  inline bool fileExists(const std::string &path)
  {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
  }

  // The bytes an ordinary file really holds on this machine. A name that cannot
  // be measured holds none, and the callers keep that out of what they state
  // rather than writing a size of nought.
  inline long long fileSize(const std::string &path)
  {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<long long>(size);
  }
} // namespace ols

// Read a small text file, and replace one atomically: the server keeps its own
// records (jobs, sessions, prefs) in plain text files beside the
// things they describe. Writing goes through a temporary file and a rename, so
// the final name is settled in one move and a record written by one user stays
// replaceable while another process is serving.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <sstream>

namespace ols
{
  // A record is written beside the thing it describes, and on a machine that
  // never needed one the folder it lives in is not there: the folder comes with
  // the write, never with the start, so a launch that writes nothing creates
  // nothing.
  inline bool ensureParentDir(const std::string &path)
  {
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty())
      return true;
    std::error_code ec{};
    std::filesystem::create_directories(parent, ec);
    return !ec;
  }

  inline bool readFileText(const std::string &path, std::string &out)
  {
    std::ifstream in{path, std::ios::binary};
    if (!in.good())
      return false;
    out.assign((std::istreambuf_iterator<char>{in}), std::istreambuf_iterator<char>{});
    return true;
  }

  inline bool writeFileText(const std::string &path, const std::string &text)
  {
    if (!ensureParentDir(path))
      return false;
    static std::atomic<unsigned> seq{0};
    std::ostringstream name;
    name << path << ".writing" << ++seq;
    const std::string temp = name.str();
    {
      std::ofstream out{temp, std::ios::binary};
      if (!out.good())
        return false;
      out << text;
      out.flush();
      if (!(out.good() || out.eof()))
      {
        std::error_code ignored{};
        std::filesystem::remove(temp, ignored);
        return false;
      }
    }
    std::error_code ec{};
    std::filesystem::rename(temp, path, ec);
    if (ec)
    {
      std::filesystem::remove(temp, ec);
      return false;
    }
    return true;
  }
}

// The clock readings and the name a job record is kept under, shared by every
// service: a duration is measured on the steady clock, a record's moment is
// stated in whole seconds, and a job's name carries its kind, a part drawn from
// the clock, and a tag so two jobs asked for in the same moment still get
// different names.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>

namespace ols
{
  // Whole milliseconds since an arbitrary point. Only the difference between two
  // readings means anything, so moving the wall clock cannot make a run look
  // shorter than it was or a rate impossible to measure.
  inline long long steadyMs()
  {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  // The whole second something happened, as a record states it. A run's own
  // length is measured with the clock above; this is only for the moment a
  // record says a job started, settled, or was made.
  inline long long nowSeconds()
  {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  // The name of a job: its kind, a part drawn from the clock, and a tag. Two
  // jobs asked for in the same moment still get different names, and the pages
  // carry these words as they are.
  inline std::string makeId(const std::string &prefix)
  {
    static std::minstd_rand engine{std::random_device{}()};
    static std::mutex engineMutex;
    std::lock_guard<std::mutex> lock(engineMutex);
    std::uniform_int_distribution<uint64_t> timePart{0, 0xFFFFFFFFFFFFull};
    std::uniform_int_distribution<uint32_t> tagPart{0, 0xFFFFFFFFu};
    const uint64_t stamp =
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
        timePart(engine);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s-%012llx-%08x", prefix.c_str(),
                  static_cast<unsigned long long>(stamp), tagPart(engine));
    return std::string(buffer);
  }
} // namespace ols

// What the machine keeps of the jobs it ran: a job's record lives under one
// name in several maps, so when it runs past what it keeps, the oldest name
// goes out of every one of them at once - a record left in one map while its
// rest stands in another would be a lie a page could read.

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

namespace ols
{
  // The cap every engine here keeps its job records to. One number, kept beside
  // the helper that enforces it, so the modules that own a job store cannot
  // state four different ones.
  inline constexpr std::size_t kKeptJobs = 200;

  // Let go of the oldest names until the queue holds only what it may keep.
  // Every map named here is keyed by the same names, so each loses the same
  // entry. A caller that already holds its module's lock holds it here too.
  template <typename... Kept>
  inline void forgetPastKept(std::deque<std::string> &order, std::size_t keep,
                             Kept &...kept)
  {
    while (order.size() > keep)
    {
      const std::string old = order.front();
      order.pop_front();
      (kept.erase(old), ...);
    }
  }

  // The machine's one working device is one card, whichever engine works with
  // it. Its graph builder is one-thread work, and every engine in this binary
  // shares the one backend behind it: a voice graph built while a picture is
  // drawing walks the same device state from two threads at once, and the file
  // the crash leaves behind names it an access violation in the allocator.
  // Every hand that stands weights on the card, draws with them, or takes
  // memory back off it waits here - whatever module the hand belongs to.
  inline std::mutex &deviceCard()
  {
    static std::mutex card;
    return card;
  }
} // namespace ols

// The words a job wears for how far it has got. They are stated once here: no
// module spells one of them differently, the pages draw only what the machine
// writes, and the questions asked of a state - is it still working, has it
// nowhere further to go - are asked the same way everywhere. A record that
// names no state names nothing, and nothing stated is neither standing nor
// ended.

#include <nlohmann/json.hpp>
#include <string>

namespace ols
{
  // The state a job record states, if it states one.
  inline std::string jobState(const nlohmann::json &record)
  {
    return statedString(record, "state");
  }

  // Still working.
  inline constexpr const char *kStateRunning = "running";

  // Believed in and named, standing for its turn at the card.
  inline constexpr const char *kStateQueued = "queued";

  // Got all of its own work done.
  inline constexpr const char *kStateDone = "done";

  // Stopped with a reason.
  inline constexpr const char *kStateError = "error";

  // Stopped because someone asked it to.
  inline constexpr const char *kStateCancelled = "cancelled";

  // What a record door says about a name the machine does not hold: not a state
  // a job wears, but the answer that no job was found.
  inline constexpr const char *kStateUnknown = "unknown";

  // Is the job still working, or standing for its turn? Both answer the asks a
  // standing job bears - a stop, a count of what is on the machine - and
  // neither has ended.
  inline bool jobStanding(const nlohmann::json &record)
  {
    const std::string state = jobState(record);
    return state == kStateRunning || state == kStateQueued;
  }

  // Is this state word one the job has nowhere further to go in?
  inline bool stateEnded(const std::string &state)
  {
    return state == kStateDone || state == kStateError || state == kStateCancelled;
  }
} // namespace ols

// The one sum this tree uses: SHA-256, written once here and read the same way
// by every module that checks a file's bytes. The words a file is checked
// against are its card's own sum (a repository states an LFS oid, which is a
// SHA-256 of the file), so the arithmetic below must answer the standard value
// and nothing else.
//
// A file is read in two shapes. Whole, when a card states one sum. And in
// fixed chunks, keyed by the chunk's place in the file, so a record of a weight
// states what it saw piece by piece and can answer about one piece later without
// the file being read again. Both shapes come out of one pass over the file,
// because these are files of a hundred-plus gigabytes and a second read of one
// costs real time.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <fstream>
#include <string>
#include <vector>

namespace ols
{
  // The fraction part of a double, taken as the low 32 bits: the standing way
  // SHA-256 gets its constants out of the square roots and cube roots of the
  // first primes.
  inline uint32_t frac32(double value)
  {
    return static_cast<uint32_t>((value - std::floor(value)) * 4294967296.0);
  }

  // The first 64 primes, built once.
  inline const std::vector<uint32_t> &firstPrimes()
  {
    static const std::vector<uint32_t> primes = []()
    {
      std::vector<uint32_t> out;
      for (uint32_t candidate = 2; out.size() < 64; ++candidate)
      {
        bool prime = true;
        for (const uint32_t p : out)
        {
          if (static_cast<uint64_t>(p) * p > candidate)
            break;
          if (candidate % p == 0)
          {
            prime = false;
            break;
          }
        }
        if (prime)
          out.push_back(candidate);
      }
      return out;
    }();
    return primes;
  }

  // The round constants, built once from those primes.
  inline const std::vector<uint32_t> &kRound()
  {
    static const std::vector<uint32_t> round = []()
    {
      std::vector<uint32_t> out;
      for (const uint32_t p : firstPrimes())
        out.push_back(frac32(std::cbrt(p)));
      return out;
    }();
    return round;
  }

  inline uint32_t rotateRight(uint32_t v, int bits)
  {
    return (v >> bits) | (v << (32 - bits));
  }

  // A running SHA-256. Bytes go in as they are read, and only 63 of them are
  // ever held, so the sum of a 34 GB shard costs the same memory as the sum of
  // a page.
  class Sha256
  {
  public:
    Sha256()
    {
      for (int i = 0; i < 8; ++i)
        state_[i] = frac32(std::sqrt(firstPrimes()[i]));
    }

    void feed(const void *data, size_t length)
    {
      const uint8_t *bytes = static_cast<const uint8_t *>(data);
      bits_ += static_cast<uint64_t>(length) * 8ull;
      size_t at = 0;
      // A partial block is finished before anything new goes into it.
      while (!tail_.empty() && at < length)
      {
        tail_.push_back(bytes[at++]);
        if (tail_.size() == 64)
        {
          mix(tail_.data());
          tail_.clear();
        }
      }
      // Whole blocks go straight in; whatever is left over waits for the next
      // feed or the finish.
      while (at + 64 <= length)
      {
        mix(bytes + at);
        at += 64;
      }
      while (at < length)
        tail_.push_back(bytes[at++]);
    }

    std::string finalHex()
    {
      // The padding the format fixes on the message's own bit length, then the
      // last blocks.
      std::vector<uint8_t> tail = tail_;
      const uint64_t bits = bits_;
      tail.push_back(0x80);
      while (tail.size() % 64 != 56)
        tail.push_back(0);
      for (int i = 0; i < 8; ++i)
        tail.push_back(static_cast<uint8_t>((bits >> (56 - i * 8)) & 0xffull));
      for (size_t at = 0; at < tail.size(); at += 64)
        mix(tail.data() + at);
      char out[65];
      for (int i = 0; i < 8; ++i)
        std::snprintf(out + i * 8, 9, "%08x", state_[i]);
      out[64] = '\0';
      return std::string(out);
    }

  private:
    uint32_t state_[8];
    uint64_t bits_ = 0;
    std::vector<uint8_t> tail_;

    void mix(const uint8_t *data)
    {
      uint32_t w[64];
      for (size_t i = 0; i < 64; ++i)
      {
        w[i] = (static_cast<uint32_t>(data[i * 4]) << 24) |
               (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(data[i * 4 + 2]) << 8) | data[i * 4 + 3];
      }
      for (size_t i = 16; i < 64; ++i)
      {
        const uint32_t s0 = rotateRight(w[i - 15], 7) ^ rotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotateRight(w[i - 2], 17) ^ rotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }
      uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
      uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
      for (size_t i = 0; i < 64; ++i)
      {
        const uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + kRound()[i] + w[i];
        const uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
      }
      state_[0] += a;
      state_[1] += b;
      state_[2] += c;
      state_[3] += d;
      state_[4] += e;
      state_[5] += f;
      state_[6] += g;
      state_[7] += h;
    }
  };

  // One pass over a file, answering both shapes at once: its whole sum, and the
  // sum of each fixed chunk of it in file order. The last chunk is whatever is
  // left over, so a table describes a file exactly whatever its length. False
  // when the file cannot be read.
  inline bool fileDigests(const std::string &path, size_t chunkBytes, std::string &wholeHex,
                          std::vector<std::string> &partHex, uint64_t &bytes)
  {
    if (chunkBytes == 0)
      return false;
    std::ifstream in{path, std::ios::binary};
    if (!in)
      return false;
    Sha256 whole;
    std::unique_ptr<Sha256> part;
    std::vector<uint8_t> buffer(65536);
    uint64_t read = 0;
    uint64_t inChunk = 0;
    while (true)
    {
      const std::streamsize got =
          in.rdbuf()->sgetn(reinterpret_cast<char *>(buffer.data()),
                            static_cast<std::streamsize>(buffer.size()));
      if (got <= 0)
        break;
      const uint8_t *bytesRead = buffer.data();
      size_t at = 0;
      while (at < static_cast<size_t>(got))
      {
        if (inChunk == 0)
          part = std::make_unique<Sha256>();
        const size_t room = static_cast<size_t>(chunkBytes) - inChunk;
        const size_t take = (static_cast<size_t>(got) - at) < room ? (static_cast<size_t>(got) - at) : room;
        whole.feed(bytesRead + at, take);
        part->feed(bytesRead + at, take);
        at += take;
        inChunk += take;
        read += static_cast<uint64_t>(take);
        if (inChunk == static_cast<uint64_t>(chunkBytes))
        {
          partHex.push_back(part->finalHex());
          part.reset();
          inChunk = 0;
        }
      }
    }
    if (inChunk != 0)
      partHex.push_back(part->finalHex());
    wholeHex = whole.finalHex();
    bytes = read;
    return true;
  }
} // namespace ols

// A model's own files in the weights tree: the tree, the model's name, and the
// file. Every module reads that shape the same way, so a model's files cannot
// stand in one place for one page and in another for the next.

#include <initializer_list>
#include <string>
#include <vector>

namespace ols
{
  // The folder a model's files stand in.
  inline std::string modelFileDir(const std::string &weightsDir, const std::string &name)
  {
    return weightsDir + "/" + name + "/";
  }

  // One of a model's files.
  inline std::string modelFilePath(const std::string &weightsDir, const std::string &name,
                                   const std::string &file)
  {
    return modelFileDir(weightsDir, name) + file;
  }

  // A model and its optional side files, one path each: a side file a model does
  // not name brings no path.
  inline std::vector<std::string> modelFiles(const std::string &weightsDir, const std::string &name,
                                             const std::string &file,
                                             const std::initializer_list<const char *> &sideFiles)
  {
    std::vector<std::string> out;
    out.push_back(modelFilePath(weightsDir, name, file));
    for (const char *extra : sideFiles)
    {
      if (extra && *extra)
        out.push_back(modelFilePath(weightsDir, name, extra));
    }
    return out;
  }

} // namespace ols
