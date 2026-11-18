#include "OpenLocalServices.h"
#include "Deployment.h"
#include "ImageService.h"
#include "MonitorService.h"
#include "NetworkService.h"
#include "VoiceService.h"
#include "WebAssets.h"
#include "Common.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include "ggml-backend.h"
#include "Platform.h"

namespace ols
{
  namespace
  {

    bool apiEnsureWsa()
    {
      return platform::initSockets();
    }

    // Keys are compared in a way that does not stop at the first difference: a
    // short answer that depends on where a key first disagrees would say more
    // about the key than a response ever should.
    bool keysMatch(const std::string &given, const std::string &wanted)
    {
      if (given.size() != wanted.size())
        return false;
      unsigned char diff = 0;
      for (size_t at = 0; at < given.size(); ++at)
        diff |= static_cast<unsigned char>(given[at] ^ wanted[at]);
      return diff == 0;
    }

    // Does any credential this request handed open the door? A request may hand
    // the same key in two shapes at once, and any one of them is enough.
    bool credentialsMatch(const std::vector<std::string> &given, const std::string &wanted)
    {
      for (const std::string &offered : given)
        if (keysMatch(offered, wanted))
          return true;
      return false;
    }

    bool apiPortInUse(uint16_t port)
    {
      if (port == 0)
        return false;
      const platform::sock_t probe = platform::newStreamSocket();
      if (probe == platform::kBadSocket)
        return false;
      platform::setWriteTimeout(probe, 300);
      platform::setReadTimeout(probe, 300);
      const sockaddr_in addr = platform::endpointFor(port, true);
      const int rc = platform::connectSocket(probe, addr);
      platform::discardSocket(probe);
      return rc == 0;
    }

    std::string lowerText(const std::string &text)
    {
      std::string out = text;
      for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return out;
    }

    bool equalsIgnoreCase(const std::string &left, const std::string &right)
    {
      return lowerText(left) == lowerText(right);
    }

    std::string contentTypeForPath(const std::string &path)
    {
      const size_t dot = path.find_last_of('.');
      if (dot == std::string::npos)
        return "application/octet-stream";
      const std::string ext = lowerText(path.substr(dot));
      if (ext == ".html" || ext == ".htm")
        return "text/html; charset=utf-8";
      if (ext == ".js" || ext == ".mjs")
        return "application/javascript; charset=utf-8";
      if (ext == ".css")
        return "text/css; charset=utf-8";
      if (ext == ".json" || ext == ".map")
        return "application/json; charset=utf-8";
      if (ext == ".png")
        return "image/png";
      if (ext == ".jpg" || ext == ".jpeg")
        return "image/jpeg";
      if (ext == ".wav")
        return "audio/wav";
      if (ext == ".svg")
        return "image/svg+xml";
      if (ext == ".ico")
        return "image/x-icon";
      return "application/octet-stream";
    }

    bool isEventStreamPath(const std::string &path)
    {
      if (path.size() < 8 || path.compare(path.size() - 7, 7, "/events") != 0)
        return false;
      return path.rfind("/api/jobs/", 0) == 0;
    }

    // Options editable through /api/config and the config panel. These are the
    // .env values the entry point reads at startup. "live" options apply to the
    // running server too; the others take effect at the next start.
    struct ConfigOption
    {
      const char *name;
      bool secret;
      bool live;
    };
    const ConfigOption kConfigOptions[] = {
        {"OLS_API_KEY", true, true},
        {"CIVITAI_API_KEY", true, true},
        {"HF_TOKEN", true, true},
        // The port to bind at startup. The server refuses to start if that port
        // is already in use; it does not roll over to another port.
        {"OLS_PORT", false, false},
        {"OLS_DATA_DIR", false, false},
        {"OLS_HOST", false, false},
        {"OLS_RATE_LIMIT", false, true},
        // Whether this machine is set to start this server by itself. Writing it
        // is the same ask as the buttons on the service block below: the line in the
        // file and what the machine does at boot are kept one thing, never two.
        {"OLS_STARTUP", false, false},
        {"OLS_THREADS", false, false},
        {"OLS_WEIGHTS_DIR", false, false},
        // Whether the server asks this machine to stand at the level its manager
        // works at before it starts - the level a boot entry, a unit, a task are
        // the manager's to take. Left alone means it asks; off is the one word
        // that declines, and the door's own words then name the level it stands at.
        {"OLS_ELEVATE", false, false},
    };

    std::string configTrim(const std::string &text)
    {
      size_t from = 0;
      while (from < text.size() && std::isspace(static_cast<unsigned char>(text[from])))
        ++from;
      size_t to = text.size();
      while (to > from && std::isspace(static_cast<unsigned char>(text[to - 1])))
        --to;
      return text.substr(from, to - from);
    }

    std::vector<std::pair<std::string, std::string>> configFileValues()
    {
      std::vector<std::pair<std::string, std::string>> out;
      std::ifstream in{".env"};
      std::string line;
      while (std::getline(in, line))
      {
        const std::string clean = configTrim(line);
        if (clean.empty() || clean[0] == '#')
          continue;
        const size_t mark = clean.find('=');
        if (mark == std::string::npos)
          continue;
        out.push_back({configTrim(clean.substr(0, mark)), configTrim(clean.substr(mark + 1))});
      }
      return out;
    }

    std::string configEffectiveValue(const std::vector<std::pair<std::string, std::string>> &fileValues,
                                     const std::string &key)
    {
      const char *raw = std::getenv(key.c_str());
      if (raw && *raw)
        return raw;
      for (const auto &entry : fileValues)
        if (entry.first == key)
          return entry.second;
      return std::string();
    }

    // The words a person uses for yes and no on a setting that is one or the other.
    // Blank means unset, which is the machine's own default: leave it alone.
    bool truthyValue(const std::string &text)
    {
      std::string word;
      for (const char c : text)
        word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return word == "true" || word == "1" || word == "yes" || word == "on";
    }

    bool booleanShape(const std::string &text)
    {
      std::string word;
      for (const char c : text)
        word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return word.empty() || word == "true" || word == "false" || word == "1" || word == "0" ||
             word == "yes" || word == "no" || word == "on" || word == "off";
    }

    // A leading ~ is the home this process was started in: HOME where a shell
    // states one, USERPROFILE where only Windows states one. A path that does not
    // start with ~ is taken as it stands.
    std::string expandHomePath(const std::string &path)
    {
      if (path.empty() || path[0] != '~')
        return path;
      const char *home = std::getenv("HOME");
      if (!home || !*home)
        home = std::getenv("USERPROFILE");
      if (!home || !*home)
        return path;
      std::string out(home);
      while (!out.empty() && (out.back() == '/' || out.back() == '\\'))
        out.pop_back();
      out += path.substr(1);
      return out;
    }

    // list of algorithm names: the key material is the long base64 field, and the
    // word that names the algorithm is the field that starts with it. A line can
    // be answered with as its whole self, as algorithm plus material, as the
    // material alone, or - when a person wrote a name at its end - by that name,
    // which is what a reader types instead of a page of characters. Option fields
    // and comments carry no key of their own.
    bool base64Field(const std::string &field)
    {
      if (field.size() < 32)
        return false;
      for (const char c : field)
      {
        const bool word = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == '+' || c == '/' || c == '=';
        if (!word)
          return false;
      }
      return true;
    }

    bool configValueShapeOk(const std::string &key, const std::string &value)
    {
      if (value.find('\r') != std::string::npos || value.find('\n') != std::string::npos)
        return false;
      // A setting that is one thing or the other refuses every other word for it:
      // a page cannot save a maybe about what the machine does at boot.
      if (key == "OLS_STARTUP")
        return booleanShape(value);
      // The level ask is one thing or the other: an empty value asks, "off"
      // declines, and any other word is a maybe the machine cannot act on.
      if (key == "OLS_ELEVATE")
        return value.empty() || value == "on" || value == "off";
      if (key == "OLS_PORT")
      {
        if (value.empty())
          return true;
        unsigned long long number = 0;
        for (char c : value)
        {
          if (c < '0' || c > '9')
            return false;
          number = number * 10ull + static_cast<unsigned long long>(c - '0');
          if (number > 65535ull)
            return false;
        }
        return number > 0;
      }
      if (key == "OLS_RATE_LIMIT")
      {
        if (value.empty())
          return true;
        unsigned long long number = 0;
        for (char c : value)
        {
          if (c < '0' || c > '9')
            return false;
          number = number * 10ull + static_cast<unsigned long long>(c - '0');
          if (number > 1000000ull)
            return false;
        }
        return true;
      }
      // Threads are whole workers, so whole numbers only, and never more than the
      // launcher itself will hand over: a figure past that is a slip of the key,
      // and by the time it reaches the engines it has already become nonsense.
      if (key == "OLS_THREADS")
      {
        if (value.empty())
          return true;
        unsigned long long number = 0;
        for (char c : value)
        {
          if (c < '0' || c > '9')
            return false;
          number = number * 10ull + static_cast<unsigned long long>(c - '0');
          if (number > 4096ull)
            return false;
        }
        return number > 0;
      }
      return true;
    }

    bool configUpdateFile(const std::vector<std::pair<std::string, std::string>> &changes,
                          std::string &error)
    {
      std::ifstream in{".env"};
      std::vector<std::string> lines;
      std::string line;
      while (std::getline(in, line))
        lines.push_back(line);
      for (const auto &change : changes)
      {
        bool seen = false;
        for (auto &entry : lines)
        {
          const size_t mark = entry.find('=');
          if (mark == std::string::npos)
            continue;
          if (configTrim(entry.substr(0, mark)) != change.first)
            continue;
          entry = change.first + "=" + change.second;
          seen = true;
        }
        if (!seen)
          lines.push_back(change.first + "=" + change.second);
      }
      // The whole file goes over at once, through a temporary name and a rename:
      // .env holds the keys this server runs on, and a write that died halfway
      // would leave a person with half a config or none.
      std::string text;
      for (const auto &entry : lines)
      {
        text += entry;
        text.push_back('\n');
      }
      if (!ols::writeFileText(".env", text))
      {
        error = "could not write .env";
        return false;
      }
      return true;
    }

    // What the settings of this machine are, as the config panel reads them. The
    // place the keys file stands is passed in rather than read from the file here:
    // the panel must show what this server reads, including the default with its
    // home taken out, and a reader who turned it off sees that.
    std::string configStateJson()
    {
      const auto fileValues = configFileValues();
      nlohmann::json out = nlohmann::json::array();
      for (const ConfigOption &option : kConfigOptions)
      {
        std::string value = configEffectiveValue(fileValues, option.name);
        nlohmann::json entry = {{"name", option.name}, {"secret", option.secret}, {"live", option.live}};
        entry["value"] = option.secret ? (value.empty() ? std::string("unset") : std::string("set")) : value;
        out.push_back(entry);
      }
      nlohmann::json doc;
      doc["options"] = out;
      return doc.dump();
    }

    // Auth lockout: the first kAuthSilentAfter wrong keys get real 401s; past
    // that, connections get no response at all until kAuthSilentResetMs passes
    // without a new miss, which reopens the gate for one honest answer.
    constexpr int kAuthSilentAfter = 3;
    constexpr long long kAuthSilentResetMs = 30000;
    constexpr int kAuthDelayMinMs = 6;
    constexpr int kAuthDelayMaxMs = 36;

    bool webAssetForPath(const std::string &path)
    {
      if (path == "/webui" || path.rfind("/webui/", 0) == 0)
        return true;
      // Browsers request /favicon.ico before any page asks for it; the icon
      // lives with the pages, so map that address like any other web asset.
      if (path == "/favicon.ico")
        return true;
      const size_t dot = path.find_last_of('.');
      if (dot == std::string::npos)
        return false;
      const std::string ext = lowerText(path.substr(dot));
      return ext == ".html" || ext == ".css" || ext == ".js";
    }

    // Percent escapes are read back the way a request wrote them. A form field
    // in a query also writes a space as '+', a path never does, so the caller
    // says which shape it is reading.
    std::string decodeText(const std::string &raw, bool plusIsSpace)
    {
      std::string out;
      for (size_t i = 0; i < raw.size(); ++i)
      {
        if (raw[i] == '%' && i + 2 < raw.size())
        {
          const std::string hex = raw.substr(i + 1, 2);
          char *end = nullptr;
          const long value = std::strtol(hex.c_str(), &end, 16);
          if (end && !*end)
          {
            out.push_back(static_cast<char>(value));
            i += 2;
            continue;
          }
        }
        out.push_back(plusIsSpace && raw[i] == '+' ? ' ' : raw[i]);
      }
      return out;
    }

    bool pathInside(const std::string &child, const std::string &parent)
    {
      if (child == parent)
        return true;
      return child.size() > parent.size() + 1 &&
             child.compare(0, parent.size(), parent) == 0 &&
             child[parent.size()] == '/';
    }

    std::string queryValue(const std::string &query, const std::string &key)
    {
      const std::string prefix = key + "=";
      size_t start = 0;
      while (start <= query.size())
      {
        const size_t amp = query.find('&', start);
        const size_t end = amp == std::string::npos ? query.size() : amp;
        if (end - start >= prefix.size() &&
            query.compare(start, prefix.size(), prefix) == 0)
          return decodeText(query.substr(start + prefix.size(), end - start - prefix.size()), true);
        if (amp == std::string::npos)
          break;
        start = amp + 1;
      }
      return std::string();
    }

    long long queryInt(const std::string &query, const std::string &key,
                       long long fallback)
    {
      const std::string text = queryValue(query, key);
      if (text.empty())
        return fallback;
      try
      {
        return std::stoll(text);
      }
      catch (const std::exception &)
      {
        return fallback;
      }
    }

    // What a request offered as a credential, in the shapes it may be handed:
    // the header, the standard Authorization form, a ?key= query - a courtesy
    // for programs, since no page of this machine writes one into an address -
    // and the cookie a door put on a visiting browser when it accepted.
    std::vector<std::string> givenCredentials(const HttpRequest &req)
    {
      std::vector<std::string> given;
      const auto header = req.headers.find("x-api-key");
      if (header != req.headers.end())
        given.push_back(header->second);
      const auto bearer = req.headers.find("authorization");
      if (bearer != req.headers.end() && bearer->second.size() > 7 &&
          lowerText(bearer->second.substr(0, 7)) == "bearer ")
        given.push_back(bearer->second.substr(7));
      const std::string asked = queryValue(req.query, "key");
      if (!asked.empty())
        given.push_back(asked);
      // A cookie field is matched by its whole name, and what the door wrote
      // is taken back through its escapes whole.
      const auto cookie = req.headers.find("cookie");
      if (cookie != req.headers.end())
      {
        const std::string &jar = cookie->second;
        for (size_t at = 0; at < jar.size();)
        {
          const size_t nameEnd = jar.find_first_of("=;", at);
          if (nameEnd == std::string::npos)
            break;
          const std::string field = configTrim(lowerText(jar.substr(at, nameEnd - at)));
          size_t valueEnd = jar.find(';', nameEnd + 1);
          if (valueEnd == std::string::npos)
            valueEnd = jar.size();
          // A field written without its = is nothing, and the text after it
          // belongs to the next field: only a name that was given a value reads
          // one.
          if (field == "ols-key" && nameEnd < jar.size() && jar[nameEnd] == '=')
            given.push_back(decodeText(jar.substr(nameEnd + 1, valueEnd - nameEnd - 1), false));
          at = valueEnd + 1;
        }
      }
      return given;
    }

    // A cookie never carries an ambiguous byte: everything outside a short set
    // of safe characters is percent-escaped, and a literal percent always
    // escapes - so a credential that holds one is written out and read back
    // whole, never half.
    std::string encodeCookieValue(const std::string &value)
    {
      static const std::string safe = "!#$&'*+-./:<=>?@^_{|}~";
      static const char *hex = "0123456789ABCDEF";
      std::string out;
      for (const char c : value)
      {
        const bool letterOrDigit =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (letterOrDigit || safe.find(c) != std::string::npos)
        {
          out.push_back(c);
          continue;
        }
        const unsigned char byte = static_cast<unsigned char>(c);
        out.push_back('%');
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0x0F]);
      }
      return out;
    }

    // Text a page speaks: only the characters that carry a meaning of their own
    // are written as words, so a name read off a request cannot open a tag of
    // its own or close one early.
    std::string escapeHtmlText(const std::string &raw)
    {
      std::string out;
      for (const char c : raw)
      {
        switch (c)
        {
        case '&':
          out += "&amp;";
          break;
        case '<':
          out += "&lt;";
          break;
        case '>':
          out += "&gt;";
          break;
        case '"':
          out += "&quot;";
          break;
        default:
          out.push_back(c);
          break;
        }
      }
      return out;
    }

    // The room a refused visit says it came from, kept to what a room's name can
    // be: one page's name, with no folders and no address in it. The door writes
    // this word back into its own page, into a link and into a refresh, so a name
    // holding a quote would have written part of that page itself, and a name
    // holding a host would have carried the visit off this machine.
    std::string onePageName(const std::string &room)
    {
      size_t from = 0;
      for (size_t at = 0; at < room.size(); ++at)
        if (room[at] == '/' || room[at] == '\\')
          from = at + 1;
      std::string name;
      for (size_t at = from; at < room.size(); ++at)
      {
        const char c = room[at];
        const bool whole = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (whole)
          name.push_back(c);
      }
      const bool nothing = name.empty() || name.find_first_not_of('.') == std::string::npos;
      return nothing ? std::string("index.html") : name;
    }

    std::string idJson(const std::string &id)
    {
      return nlohmann::json{{"job_id", id}}.dump();
    }

    std::string idFieldJson(const std::string &key, const std::string &id)
    {
      nlohmann::json out;
      out[key] = id;
      return out.dump();
    }

    // Seconds an answer for this path may be reused. The API states this on
    // the answer itself (Cache-Control); the browser cache in webui keeps it
    // that long and re-asks only past that point. Entries cover their
    // sub-paths; unlisted paths get a short default.
    //
    // A model table is not a thing that goes stale by the minute: its truth
    // changes when a job changes it - a download that landed, a model put down.
    // Held for a minute, a page that started a download reads the copy made
    // while its bytes were still arriving, and keeps reading it after they
    // arrived: the model is on the machine, and no page will say so until the
    // hold runs out or the reader reloads, and a reload reads the same held
    // copy. So whatever a job answers for is read fresh: the model tables, the
    // counts across them, what the machine holds, and what a run left behind.
    // That costs the machine nothing it was not already paying: each service
    // builds its answer once and keeps it until a job marks it stale, so a page
    // reading every couple of seconds is handed an answer already built.
    // What changes only when a person changes it - a setting, a kept voice's
    // name, the manuals - may still be held.
    int apiFreshSeconds(const std::string &path)
    {
      static const std::pair<const char *, int> FRESH[] = {
          {"/api/monitor", 2},
          {"/api/jobs", 2},
          {"/api/timings", 2},
          {"/api/system", 2},
          // The network document is composed fresh every read and its every
          // number moves within seconds; and the scan door answers a forced
          // scan, which no browser may answer out of its own pocket.
          {"/api/network", 0},
          {"/api/service", 4},
          {"/api/config", 15},
          {"/api/models", 2},
          {"/api/gallery", 2},
          {"/api/refs", 2},
          {"/api/prefs", 20},
          {"/voice/api/status", 2},
          {"/voice/api/health", 2},
          {"/voice/api/jobs", 2},
          {"/voice/api/models", 2},
          {"/voice/api/model-cards", 2},
          {"/voice/api/model-stats", 2},
          {"/voice/api/voices", 2},
          {"/voice/api/library", 2},
      };
      for (const auto &listed : FRESH)
        if (path == listed.first)
          return listed.second;
      for (const auto &listed : FRESH)
        if (path.rfind(std::string(listed.first) + "/", 0) == 0)
          return listed.second;
      return 5;
    }

    HttpResponse jsonResponse(int status, const std::string &body)
    {
      HttpResponse r;
      r.status = status;
      r.setJson(body);
      return r;
    }

    // Writing for the port: the bytes go out as they are.
    bool sendAll(const ConnectionSink &sink, const std::string &data)
    {
      const char *p = data.data();
      size_t remaining = data.size();
      while (remaining > 0)
      {
        const int chunk = remaining > 1024u * 1024u ? 1024 * 1024
                                                    : static_cast<int>(remaining);
        const int sent = platform::sendBytes(sink.sock, p, chunk, 0);
        if (sent <= 0)
          return false;
        remaining -= static_cast<size_t>(sent);
        p += sent;
      }
      return true;
    }

    // Reading for the port: appends up to cap bytes, and says how many came
    // (0 or fewer means the stream ended or gave up). A request line and its
    // body are read through this.
    int readSome(const ConnectionSink &sink, std::string &out, size_t cap)
    {
      char buf[4096];
      const size_t want = cap < sizeof(buf) ? cap : sizeof(buf);
      const int got = platform::receiveBytes(sink.sock, buf,
                                             static_cast<int>(want == 0 ? 1 : want), 0);
      if (got > 0)
        out.append(buf, static_cast<size_t>(got));
      return got;
    }

    // Bytes asked for that nobody wants: read and dropped, nothing kept.
    bool drainTo(const ConnectionSink &sink, unsigned long long n)
    {
      while (n > 0)
      {
        char buf[4096];
        const int got = platform::receiveBytes(
            sink.sock, buf, static_cast<int>(n < sizeof(buf) ? n : sizeof(buf)), 0);
        if (got <= 0)
          return false;
        n -= static_cast<unsigned long long>(got);
      }
      return true;
    }

    // Closing for the port: the socket closes.
    void closeFor(const ConnectionSink &sink)
    {
      platform::closeSocket(sink.sock);
    }

  } // namespace

  void HttpResponse::setJson(const std::string &json)
  {
    headers["Content-Type"] = "application/json; charset=utf-8";
    headers["Content-Length"] = std::to_string(json.size());
    body = json;
  }

  void HttpResponse::setPlain(const std::string &text,
                              const std::string &contentType)
  {
    headers["Content-Type"] = contentType;
    headers["Content-Length"] = std::to_string(text.size());
    body = text;
  }

  bool OpenLocalServices::isLoopbackHost(const std::string &host)
  {
    if (host.empty() || host == "*")
      return false;
    if (equalsIgnoreCase(host, "localhost") || host == "127.0.0.1" || host == "::1")
      return true;
    in_addr parsed{};
    if (inet_pton(AF_INET, host.c_str(), &parsed) == 1)
      return parsed.s_addr == htonl(0x7F000001u);
    return false;
  }

  bool OpenLocalServices::writeStreamChunk(void *client, const std::string &frame)
  {
    // The client is a connection's sink — a handle the doors pass along
    // without opening it; the port answers in the clear only.
    const ConnectionSink &sink = *static_cast<const ConnectionSink *>(client);
    char prefix[24];
    const int len = std::snprintf(prefix, sizeof(prefix), "%zx\r\n", frame.size());
    std::string out;
    out.append(prefix, static_cast<size_t>(len));
    out += frame;
    out += "\r\n";
    return sendAll(sink, out);
  }

  std::string OpenLocalServices::statusLine(int status)
  {
    switch (status)
    {
    case 200:
      return "200 OK";
    case 201:
      return "201 Created";
    case 202:
      return "202 Accepted";
    case 206:
      return "206 Partial Content";
    case 307:
      return "307 Temporary Redirect";
    case 304:
      return "304 Not Modified";
    case 400:
      return "400 Bad Request";
    case 404:
      return "404 Not Found";
    case 405:
      return "405 Method Not Allowed";
    case 408:
      return "408 Request Timeout";
    case 409:
      return "409 Conflict";
    case 401:
      return "401 Unauthorized";
    case 413:
      return "413 Payload Too Large";
    case 416:
      return "416 Range Not Satisfiable";
    case 429:
      return "429 Too Many Requests";
    case 500:
      return "500 Internal Server Error";
    case 502:
      return "502 Bad Gateway";
    case 503:
      return "503 Service Unavailable";
    default:
      return "500 Internal Server Error";
    }
  }

  OpenLocalServices::OpenLocalServices() = default;

  std::string OpenLocalServices::bindHost() const
  {
    return m_bindHost.empty() ? std::string("0.0.0.0") : m_bindHost;
  }

  void OpenLocalServices::setImage(imagegen::ImageService *image) { m_image = image; }

  void OpenLocalServices::setVoice(voice::VoiceService *voices) { m_voice = voices; }

  HttpResponse OpenLocalServices::noResponse() const
  {
    HttpResponse r;
    r.status = 0;
    r.body.clear();
    r.headers.clear();
    return r;
  }

  // Whether this machine would open a door to this request: its own key alone,
  // handed in any of the ways a credential may be carried - a header, the
  // standard Authorization form, an address a program wrote, or the cookie a
  // door put on a visiting browser. One credential carries a visit through both
  // kinds of door, and nothing here reads a letter of a key into a response.
  bool OpenLocalServices::authorize(const HttpRequest &req)
  {
    if (m_apiKey.empty())
      return true;
    return credentialsMatch(givenCredentials(req), m_apiKey);
  }

  // Three-state gate before the doors: 0 allow, 1 send the refusal, -1 send
  // nothing. The first kAuthSilentAfter refusals answer honestly; past that the
  // connection is swallowed until kAuthSilentResetMs pass without a new miss,
  // which reopens a door for one honest answer - and any accepted credential
  // lifts the count back, so a machine that is being used is never left silent.
  int OpenLocalServices::authGate(const HttpRequest &req)
  {
    const std::vector<std::string> given = givenCredentials(req);
    const bool asked = !m_apiKey.empty();
    if (!asked || credentialsMatch(given, m_apiKey))
    {
      m_authFailures.store(0);
      // A short random delay keeps a correct credential from being recognised by
      // response timing alone. It is worth paying only where there is a secret to
      // recognise: with no key set, or with nothing handed to compare against a
      // key, a clock has nothing to give away, so those requests are answered
      // straight away.
      if (asked && !given.empty())
      {
        static thread_local std::mt19937_64 authNoise{
            static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count())};
        std::uniform_int_distribution<int> jitter(kAuthDelayMinMs, kAuthDelayMaxMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(jitter(authNoise)));
      }
      return 0;
    }
    const long long now = ols::steadyMs();
    const long long lastMiss = m_lastAuthFailMs.exchange(now);
    const int failures = m_authFailures.fetch_add(1) + 1;
    // Silence is only ever for a fresh miss inside the window: a miss arriving
    // past the quiet stretch gets its honest answer, whatever the count says.
    if (failures > kAuthSilentAfter && now - lastMiss < kAuthSilentResetMs)
      return -1;
    return 1;
  }

  // What an api door says when a credential was wanted and none acceptable
  // came: the ways it may be handed, and nothing else.
  HttpResponse OpenLocalServices::authChallenge() const
  {
    HttpResponse r = jsonError(
        401,
        "This machine asks for a credential at its api doors - its own key. "
        "Hand it over as described below.");
    r.headers["X-OLS-Auth"] = "required";
    r.headers["WWW-Authenticate"] = "Key";
    nlohmann::json hint = nlohmann::json::parse(r.body, nullptr, false);
    if (hint.is_discarded())
      hint = nlohmann::json::object();
    hint["ways"] = "X-Api-Key header, Authorization: Bearer, or ?key= in the address";
    r.body = hint.dump();
    return r;
  }

  // What the two doors ask of at this moment: counts only. A letter of a key is
  // never answered here, or anywhere, so nothing a person typed can be read off
  // a machine's words. The pages read this before they carry a credential - it
  // is how a room learns what is asked of, and how many forms a visit may
  // hand it.
  std::string OpenLocalServices::authInfoJson()
  {
    const bool asked = !m_apiKey.empty();
    nlohmann::json out;
    out["api_required"] = asked;
    out["page_required"] = asked;
    out["page_keys"] = asked ? 1 : 0;
    out["rate_limit_per_minute"] = m_rateLimit.load();
    return out.dump();
  }

  // The door that answers a credential as a program hands it: the same decision
  // every door takes - one key, one answer - stated in words that never repeat
  // what was handed, and a key this machine takes is written on the connection
  // so the rest of the visit carries it.
  HttpResponse OpenLocalServices::certifyAnswer(const HttpRequest &req)
  {
    const std::string offered = configTrim(req.body);
    const bool asked = !m_apiKey.empty();
    const bool taken = asked && keysMatch(offered, m_apiKey);
    nlohmann::json out;
    out["handed"] = !offered.empty();
    out["api_required"] = asked;
    out["opens_doors"] = taken || !asked;
    out["ways"] = "X-Api-Key header, Authorization: Bearer, or ?key= in the address";
    HttpResponse r = jsonResponse(200, out.dump());
    if (taken)
      r.headers["Set-Cookie"] =
          "ols-key=" + encodeCookieValue(offered) + "; Path=/; SameSite=Lax";
    return r;
  }

  // Rate gate: a token bucket with timed refill. Each minute, the bucket refills
  // one request for each configured request per minute. A request consumes one
  // token only when it is allowed; a blocked request returns no operation and
  // asks the caller to send 429/Retry-After. The value is whole seconds until
  // enough tokens exist for one request, or 0 when this request is allowed.
  int OpenLocalServices::rateGate()
  {
    const int limit = m_rateLimit.load();
    if (limit <= 0)
      return 0;
    const long long now = ols::steadyMs();
    std::lock_guard<std::mutex> lock(m_rateMutex);

    // Store tokens in thousandths so fractional refill rates can be used safely.
    const long long capacity = static_cast<long long>(limit) * 1000LL;
    if (!m_rateInitialized)
    {
      m_rateInitialized = true;
      m_rateLastMs = now;
      m_rateTokens = capacity;
    }
    else
    {
      long long elapsed = now - m_rateLastMs;
      if (elapsed < 0)
        elapsed = 0;
      m_rateLastMs = now;

      if (elapsed >= 60000LL)
      {
        m_rateTokens = capacity;
      }
      else
      {
        const long long refill = (elapsed * static_cast<long long>(limit)) / 60LL;
        m_rateTokens += refill;
        if (m_rateTokens > capacity)
          m_rateTokens = capacity;
      }
    }

    if (m_rateTokens >= 1000LL)
    {
      m_rateTokens -= 1000LL;
      return 0;
    }

    const long long needed = 1000LL - m_rateTokens;
    long long retryMs = (needed * 60LL + static_cast<long long>(limit) - 1LL) /
                        static_cast<long long>(limit);
    if (retryMs < 1000LL)
      retryMs = 1000LL;
    int retrySeconds = static_cast<int>((retryMs + 999LL) / 1000LL);
    if (retrySeconds < 1)
      retrySeconds = 1;
    return retrySeconds;
  }

  // A rate refusal must not carry caller input, must not count the request, and
  // must be machine-readable by browsers and clients. Retry-After is the standard
  // backoff signal; the X-RateLimit headers are the common client hint set.
  HttpResponse OpenLocalServices::rateLimitAnswer(int retrySeconds) const
  {
    HttpResponse r = jsonError(429, "Rate limit exceeded");
    if (retrySeconds < 1)
      retrySeconds = 1;
    r.headers["Retry-After"] = std::to_string(retrySeconds);
    r.headers["X-RateLimit-Limit"] = std::to_string(m_rateLimit.load());
    r.headers["X-RateLimit-Remaining"] = "0";
    const long long epochSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
    r.headers["X-RateLimit-Reset"] = std::to_string(epochSeconds + retrySeconds);
    r.headers["Access-Control-Allow-Headers"] = "X-Api-Key, Authorization, Content-Type, Range, If-Modified-Since, If-None-Match";
    r.headers["Access-Control-Exposed-Headers"] = "X-RateLimit-Limit, X-RateLimit-Remaining, X-RateLimit-Reset, X-OLS-Auth";
    return r;
  }

  bool OpenLocalServices::start(uint16_t port)
  {
    if (m_running.load())
      return true;
    // the pages are looked at once, here: whether a folder of them stands
    // beside this server is decided now, and nothing else looks at it while the
    // server runs.
    {
      std::error_code pageEc{};
      const std::string pageDir = m_staticDir.empty() ? "webui" : m_staticDir;
      m_externalPages =
          std::filesystem::is_directory(std::filesystem::path(pageDir).lexically_normal(), pageEc);
    }
    if (!apiEnsureWsa())
      return false;
    void *handle = nullptr;
    uint16_t actual = 0;
    if (!openListener(port, handle, actual))
      return false;
    m_listenHandle = handle;
    m_port.store(actual);
    m_running.store(true);
    if (m_thread.joinable())
      m_thread.join();
    m_thread = std::thread([this, h = handle]
                           { run(h); });
    return true;
  }

  bool OpenLocalServices::openListener(uint16_t preferredPort, void *&outHandle,
                                       uint16_t &outPort)
  {
    const std::string host = bindHost();
    const bool loopback = isLoopbackHost(host);
    uint32_t bindAddr = INADDR_ANY;
    if (loopback)
    {
      bindAddr = htonl(0x7F000001u);
    }
    else if (host != "0.0.0.0" && host != "*")
    {
      in_addr parsed{};
      if (inet_pton(AF_INET, host.c_str(), &parsed) != 1)
      {
        return false;
      }
      else
      {
        bindAddr = parsed.s_addr;
      }
    }

    // One port, the one asked for: there is no next port to step to. A server that
    // finds it held says so and does not run, because every address another box or
    // a page has for this machine names this port.
    m_startRefusal.clear();
    if (apiPortInUse(preferredPort))
    {
      m_startRefusal = "Port " + std::to_string(preferredPort) +
                       " is held by another server. These doors answer on that port only, so this"
                       " server will not run beside it.";
      return false;
    }
    const platform::sock_t probe = platform::newStreamSocket();
    if (probe == platform::kBadSocket)
    {
      m_startRefusal = "Could not make a socket to answer on";
      return false;
    }
    platform::reuseAddress(probe);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(preferredPort);
    addr.sin_addr.s_addr = bindAddr;
    const bool bound =
        (platform::bindSocket(probe, addr) != platform::kSocketError) &&
        (platform::listenSocket(probe, SOMAXCONN) != platform::kSocketError);
    if (!bound)
    {
      platform::discardSocket(probe);
      m_startRefusal = "Could not bind port " + std::to_string(preferredPort);
      return false;
    }
    outHandle = reinterpret_cast<void *>(probe);
    outPort = preferredPort;
    return true;
  }

  void OpenLocalServices::closeListener(void *handle)
  {
    if (handle == nullptr)
      return;
    const platform::sock_t s = platform::fromHandle(handle);
    platform::closeSocket(s);
  }

  void OpenLocalServices::stop()
  {
    // Asking to stop twice is the same as asking once: the listening is given up
    // if it stands, and the words below only tell the loop to leave. This is what
    // makes the soft hand safe - after this returns, the port stands free.
    m_running.store(false);
    {
      std::lock_guard<std::mutex> lock(m_lifecycleMutex);
      if (m_listenHandle != nullptr)
      {
        closeListener(m_listenHandle);
        m_listenHandle = nullptr;
      }
    }
    if (m_thread.joinable())
      m_thread.join();
    for (int waited = 0; waited < 40 && m_activeConnections.load() > 0; ++waited)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // The soft hand's first half: the door that was asked to start this server
  // again is answered first, and this is only the word that tells the loop to
  // give up its listening. The port is made free where the process ends, before
  // the program that stands on disk is started as this process's own child.
  void OpenLocalServices::stopAfterThisAnswer()
  {
    m_running.store(false);
  }

  void OpenLocalServices::run(void *listenHandle)
  {
    const platform::sock_t listener = platform::fromHandle(listenHandle);
    while (m_running.load())
    {
      const platform::sock_t client = platform::acceptClient(listener);
      if (client == platform::kBadSocket)
      {
        const int err = platform::lastError();
        if (err == platform::kWouldBlock || err == platform::kConnReset ||
            err == platform::kInterrupt)
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          continue;
        }
        m_running.store(false);
        {
          std::lock_guard<std::mutex> lock(m_lifecycleMutex);
          if (m_listenHandle != nullptr)
          {
            closeListener(m_listenHandle);
            m_listenHandle = nullptr;
          }
        }
        break;
      }
      m_activeConnections.fetch_add(1);
      std::thread([this, client]
                  {
      struct Done {
        std::atomic<int> &count;
        ~Done() { count.fetch_sub(1); }
      } done{m_activeConnections};
      try {
        handleConnection(platform::toHandle(client));
      } catch (const std::exception &) {
      } catch (...) {
      } })
          .detach();
    }
  }

  // The address a request came in on, in the shape the doors are named by: an
  // address a person typed, or one a proxy handed over, can end in a slash or
  // repeat one, and the door beside it is the same door. A name a file door was
  // given cannot hold a slash, so joining slashes cannot move a name.
  static std::string normalPath(const std::string &address)
  {
    std::string path;
    path.reserve(address.size());
    for (const char c : address)
    {
      if (c == '/' && !path.empty() && path.back() == '/')
        continue;
      path.push_back(c);
    }
    while (path.size() > 1 && path.back() == '/')
      path.pop_back();
    return path.empty() ? std::string(1, '/') : path;
  }

  void OpenLocalServices::handleConnection(void *clientHandle)
  {
    const platform::sock_t sock = platform::fromHandle(clientHandle);

    std::string clientIp;
    platform::peerAddress(sock, clientIp);
    platform::setReadTimeout(sock, 10000);

    // The connection's first bytes are read whole before any answer: none of
    // them are thrown away, and a request that starts mid-block still finds
    // its line once the rest is read.
    std::string raw;
    {
      char buf[4096];
      const int first = platform::receiveBytes(sock, buf, sizeof(buf), 0);
      if (first <= 0)
      {
        platform::closeSocket(sock);
        return;
      }
      raw.assign(buf, static_cast<size_t>(first));
    }

    ConnectionSink sink{sock};

    size_t headerEnd = raw.find("\r\n\r\n");
    constexpr size_t kMaxHeaderBytes = 1u << 20u;
    while (headerEnd == std::string::npos)
    {
      const int n = readSome(sink, raw, 4096);
      if (n <= 0)
        break;
      headerEnd = raw.find("\r\n\r\n");
      if (raw.size() > kMaxHeaderBytes)
        break;
    }

    HttpRequest req;
    req.ip = clientIp.empty() ? std::string("unknown") : clientIp;
    HttpResponse resp;

    if (headerEnd == std::string::npos)
    {
      resp = jsonError(400, "Malformed request");
    }
    else
    {
      const std::string headerBlock = raw.substr(0, headerEnd);
      const size_t bodyStart = headerEnd + 4;
      constexpr size_t kMaxBodyBytes = 2u * 1024u * 1024u;
      unsigned long long declaredLength = 0;
      {
        std::istringstream hs(headerBlock);
        std::string requestLine;
        std::getline(hs, requestLine);
        std::istringstream rl(requestLine);
        std::string target;
        std::string httpVersion;
        rl >> req.method >> target >> httpVersion;
        const size_t q = target.find('?');
        if (q != std::string::npos)
        {
          req.path = normalPath(target.substr(0, q));
          req.query = target.substr(q + 1);
        }
        else
        {
          req.path = normalPath(target);
        }
        std::string line;
        while (std::getline(hs, line))
        {
          if (!line.empty() && line.back() == '\r')
            line.pop_back();
          const size_t colon = line.find(':');
          if (colon == std::string::npos)
            continue;
          const std::string key = lowerText(line.substr(0, colon));
          std::string value = line.substr(colon + 1);
          while (!value.empty() && value.front() == ' ')
            value.erase(0, 1);
          if (key == "content-length")
          {
            try
            {
              declaredLength = std::stoull(value);
            }
            catch (const std::exception &)
            {
              declaredLength = 0;
            }
          }
          else
          {
            req.headers[key] = value;
          }
        }
      }

      if (declaredLength > kMaxBodyBytes)
      {
        // The body is refused but still drained to its declared end, so the
        // answer is not mistaken for a dropped connection. Nothing is buffered:
        // what arrives is discarded as it comes.
        const unsigned long long drained = raw.size() - bodyStart;
        if (drained < declaredLength)
          drainTo(sink, declaredLength - drained);
        resp = jsonError(413, "Request body too large");
      }
      else
      {
        const size_t contentLength = static_cast<size_t>(declaredLength);
        while (raw.size() - bodyStart < contentLength)
        {
          if (readSome(sink, raw, 4096) <= 0)
            break;
        }
        req.body = raw.substr(bodyStart, contentLength);
        try
        {
          if (isEventStreamPath(req.path) && req.method == "GET")
          {
            const int auth = authGate(req);
            if (auth == -1)
            {
              closeFor(sink);
              return;
            }
            if (auth == 1)
            {
              resp = authChallenge();
            }
            else if (const int retry = rateGate())
            {
              resp = rateLimitAnswer(retry);
            }
            else
            {
              const std::string head =
                  "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                  "Cache-Control: no-store\r\nTransfer-Encoding: chunked\r\n"
                  "Connection: keep-alive\r\n\r\n";
              if (sendAll(sink, head))
              {
                try
                {
                  streamToClient(&sink, req.path);
                  writeStreamChunk(&sink, "event: end\n\n");
                }
                catch (const std::exception &)
                {
                  writeStreamChunk(&sink, "event: error\ndata: stream failed\n\n");
                }
                sendAll(sink, "0\r\n\r\n");
              }
              closeFor(sink);
              return;
            }
          }
          else
          {
            resp = route(req);
            // Every successful JSON answer states how long it may be reused; the
            // browser cache in webui reads this header and asks again only past
            // the window. A refusal carries no-store from jsonError itself, so
            // this only answers for the readings that are worth keeping.
            const auto ct = resp.headers.find("Content-Type");
            if (ct != resp.headers.end() && ct->second.find("json") != std::string::npos &&
                resp.status == 200)
            {
              resp.headers["Cache-Control"] =
                  "public, max-age=" + std::to_string(apiFreshSeconds(req.path));
            }
          }
        }
        catch (const nlohmann::json::exception &)
        {
          resp = jsonError(400, "Invalid request body");
        }
        catch (const std::exception &)
        {
          resp = jsonError(500, "Internal error");
        }
      }
    }

    if (resp.status == 0)
    {
      // Lockout: no response at all goes back over the wire.
      closeFor(sink);
      return;
    }

    if (req.method == "HEAD")
    {
      // A HEAD asks for the answer a GET would give, without its body: the
      // length a reader would find there is the point of asking.
      resp.body.clear();
    }

    std::string out = "HTTP/1.1 " + statusLine(resp.status) + "\r\n";
    for (const auto &kv : resp.headers)
      out += kv.first + ": " + kv.second + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += resp.body;
    sendAll(sink, out);
    closeFor(sink);
  }

  void OpenLocalServices::streamToClient(void *clientHandle, const std::string &path)
  {
    const size_t tail = path.find("/events");
    if (tail == std::string::npos)
      return;
    if (path.rfind("/api/jobs/", 0) == 0 && m_image)
    {
      m_image->streamEvents(clientHandle, path.substr(10, tail - 10));
    }
  }

  // Where this server's pages come from, in the words a person reading the panel
  // needs. The expand door answers a GET with this, so a machine whose pages
  // already sit outside is not offered a hand that can only refuse.
  std::string OpenLocalServices::servedPagesJson() const
  {
    const std::string dirName = m_staticDir.empty() ? "webui" : m_staticDir;
    std::error_code ec{};
    const std::filesystem::path dir = std::filesystem::path(dirName).lexically_normal();
    bool holds = false;
    if (std::filesystem::is_directory(dir, ec))
    {
      for (std::filesystem::directory_iterator it{dir, ec}; it != std::filesystem::directory_iterator{};
           it.increment(ec))
      {
        holds = true;
        break;
      }
    }
    return nlohmann::json{{"folder", dir.generic_string()},
                          {"outside", holds},
                          {"words", holds
                                        ? "the pages served are the copies in " + dir.generic_string() +
                                              " beside this server"
                                        : "the pages served are the copies carried inside this binary: "
                                          "nothing stands in " +
                                              dir.generic_string() + " yet"}}
        .dump();
  }

  // The expand door: write the pages this binary carries out beside it, so they
  // sit in a folder a person can read, and start serving those copies. A folder
  // that already holds pages is left exactly as it is - nothing here overwrites
  // a webui that is already outside.
  HttpResponse OpenLocalServices::expandStaticDir()
  {
    const std::string dirName = m_staticDir.empty() ? "webui" : m_staticDir;
    const std::filesystem::path dir = std::filesystem::path(dirName).lexically_normal();
    std::error_code ec{};
    if (std::filesystem::is_directory(dir, ec))
    {
      bool holds = false;
      for (std::filesystem::directory_iterator it{dir, ec}; it != std::filesystem::directory_iterator{};
           it.increment(ec))
      {
        holds = true;
        break;
      }
      if (holds)
        return jsonError(400, "the pages are already outside: " + dirName + " holds files");
    }
    else
    {
      std::filesystem::create_directories(dir, ec);
      if (ec)
        return jsonError(500, "Could not make the pages folder: " + ec.message());
    }
    const int total = ols::webres::embeddedCount();
    nlohmann::json written = nlohmann::json::array();
    long long bytes = 0;
    for (int i = 0; i < total; ++i)
    {
      std::string name;
      std::string content;
      if (!ols::webres::embeddedAt(i, name, content) || name.empty())
        continue;
      const std::filesystem::path target = (dir / name).lexically_normal();
      if (!pathInside(target.generic_string(), dir.generic_string()))
        continue;
      if (!ols::writeFileText(target.string(), content))
        return jsonError(500, "Could not write the page " + name);
      bytes += static_cast<long long>(content.size());
      written.push_back({{"name", name}, {"bytes", static_cast<long long>(content.size())}});
    }
    m_externalPages = true;
    nlohmann::json doc = {{"expanded", true},
                          {"folder", dir.generic_string()},
                          {"outside", true},
                          {"files", written},
                          {"bytes", bytes}};
    return jsonResponse(200, doc.dump());
  }

  // One page taken from the folder outside, whole: the file has to be there,
  // inside the folder, readable, and hold bytes. Any short reading says so and
  // leaves the caller to the copy carried inside this binary. On a true return
  // `full` names the page that was read (a folder standing for its index page)
  // and `pageName` is its name, which is what the page headers are chosen by.
  static bool readOutsidePage(std::filesystem::path &full, const std::filesystem::path &dirPath,
                              std::string &body, std::string &etag, std::string &pageName)
  {
    std::error_code ec;
    if (std::filesystem::is_directory(full, ec))
      full = full / "index.html";
    if (!ols::fileExists(full.string()))
      return false;
    const std::filesystem::path real = std::filesystem::weakly_canonical(full, ec);
    const std::filesystem::path realDir = std::filesystem::weakly_canonical(dirPath, ec);
    if (ec || !pathInside(real.generic_string(), realDir.generic_string()))
      return false;
    const std::uintmax_t size = std::filesystem::file_size(real, ec);
    if (ec || size == 0 || size > 64ull * 1024ull * 1024ull)
      return false;
    const auto modified = std::filesystem::last_write_time(real, ec);
    if (ec)
      return false;
    std::ifstream in(real, std::ios::binary);
    if (!in.good())
      return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    body = ss.str();
    if (body.empty() || body.size() != size)
      return false;
    // The etag is the file's own identity (size + write time). An unchanged
    // file revalidates to a bare 304; an edited one is sent in full.
    etag = "\"" + std::to_string(size) + "-" +
           std::to_string(modified.time_since_epoch().count()) + "\"";
    pageName = real.filename().string();
    return true;
  }

  HttpResponse OpenLocalServices::serveStatic(const std::string &path, const HttpRequest &req)
  {
    std::string rel = path == "/" || path.empty() ? "/index.html" : path;
    if (rel == "/webui" || rel.rfind("/webui/", 0) == 0)
      rel = rel.substr(6);
    rel = decodeText(rel, false);
    if (rel.empty() || rel == "/")
      rel = "/index.html";
    if (rel.front() == '/')
      rel.erase(0, 1);

    const std::string dir = m_staticDir.empty() ? "webui" : m_staticDir;
    const std::filesystem::path dirPath = std::filesystem::path(dir).lexically_normal();
    std::filesystem::path full = (dirPath / rel).lexically_normal();
    const std::string fullNorm = full.generic_string();
    const std::string dirNorm = dirPath.generic_string();
    if (!pathInside(fullNorm, dirNorm))
      return jsonError(404, "Not found: " + path);

    // The pages come from the folder outside only when that folder was there
    // when this server was pointed at it; anything short in an outside reading
    // falls back to the copy carried inside, so a browser never gets half a
    // page and a server that stands alone still answers with its own.
    std::string body;
    std::string etag;
    std::string pageName;
    if (m_externalPages)
    {
      std::filesystem::path attempt = full;
      if (readOutsidePage(attempt, dirPath, body, etag, pageName))
        full = attempt;
    }
    if (pageName.empty())
    {
      if (!ols::webres::embeddedNamed(rel, body))
        return jsonError(404, "Not found: " + path);
      if (body.empty())
        return jsonError(404, "Not found: " + path);
      // an inside copy has no write time; its own bytes are its identity.
      etag = "\"e-" + std::to_string(body.size()) + "\"";
      pageName = std::filesystem::path(rel).filename().string();
    }
    const auto inm = req.headers.find("if-none-match");
    const bool unchanged =
        inm != req.headers.end() && (inm->second == etag || inm->second == "*");
    HttpResponse r;
    const std::string contentType = contentTypeForPath(pageName);
    r.setPlain(body, contentType);
    r.headers["X-Content-Type-Options"] = "nosniff";
    r.headers["Referrer-Policy"] = "no-referrer";
    // Page files change between visits, so they are never served stale:
    // revalidation plus the etag makes an unchanged file cost only a 304.
    r.headers["Cache-Control"] = "public, must-revalidate, max-age=0";
    if (contentType.find("text/html") == 0 || contentType.find("image/svg") == 0)
    {
      // Our pages use only same-origin scripts and styles; no page embeds a
      // stylesheet of its own, but they all apply inline style attributes to
      // stand their parts at read numbers - the config panel's place, a
      // progress bar's reach, a zoom's size - so only the attribute position
      // is opened ('unsafe-inline' on style-src-attr alone), never an inline
      // <style>. data: images are allowed because the webui draws kept files
      // as data URLs. No page puts a frame of another origin on it now, so
      // no page asks for frame-src.
      std::string csp =
          "default-src 'self'; script-src 'self'; style-src 'self'; style-src-attr "
          "'unsafe-inline'; img-src 'self' data:; connect-src "
          "'self'; frame-ancestors 'none'";
      r.headers["Content-Security-Policy"] = csp;
    }
    r.headers["ETag"] = etag;
    if (unchanged)
    {
      // The client's copy still matches the page: send only the 304.
      r.status = 304;
      r.body.clear();
      r.headers.erase("Content-Length");
    }
    return r;
  }

  HttpResponse OpenLocalServices::serveFile(const std::string &kind, const std::string &name,
                                            const std::string &origin,
                                            const HttpRequest &req) const
  {
    std::string body;
    std::string contentType;
    {
      std::string path;
      const bool known = origin == "voice"
                             ? (m_voice && m_voice->file(kind, name, path, contentType))
                             : (m_image && m_image->file(kind, name, path, contentType));
      if (!known)
        return jsonError(404, "Not found: " + name);
      std::error_code ec;
      if (!ols::fileExists(path))
        return jsonError(404, "Not found: " + name);
      const std::uintmax_t size = std::filesystem::file_size(path, ec);
      if (ec || size > 64ull * 1024ull * 1024ull)
        return jsonError(404, "Not found: " + name);
      std::ifstream in(path, std::ios::binary);
      if (!in.good())
        return jsonError(404, "Not found: " + name);
      std::ostringstream ss;
      ss << in.rdbuf();
      body = ss.str();
    }

    HttpResponse r;
    const auto range = req.headers.find("range");
    if (range != req.headers.end() &&
        (req.method == "GET" || req.method == "HEAD") &&
        range->second.find(',') == std::string::npos)
    {
      const std::string spec = lowerText(range->second);
      const std::string part = spec.rfind("bytes=", 0) == 0 ? spec.substr(6) : spec;
      const size_t dash = part.find('-');
      // A byte range whose two ends the machine can read as byte counts; an
      // empty end side means the file's own end. Digits only: "8x" is not a byte
      // count. A count too big to count on this machine is kept as a count past
      // any file's end, so it is answered as nothing to give rather than as a
      // different file.
      auto readCount = [](const std::string &digits)
      {
        if (digits.empty())
          return std::make_pair(false, 0LL);
        long long value = 0;
        for (const char c : digits)
        {
          if (c < '0' || c > '9')
            return std::make_pair(false, 0LL);
          value = value * 10 + (c - '0');
          if (value > 100000000000LL)
            return std::make_pair(true, 10000000000LL);
        }
        return std::make_pair(true, value);
      };
      long long from = 0;
      long long to = -1; // nothing given for the end means the file's own end
      bool understood = dash != std::string::npos;
      if (understood)
      {
        const auto startEnd = readCount(std::string(part, 0, dash));
        const bool givenEnd = dash + 1 < part.size();
        const auto endEnd = readCount(givenEnd ? part.substr(dash + 1) : std::string());
        if (!startEnd.first || (givenEnd && !endEnd.first))
          understood = false;
        else
        {
          from = startEnd.second;
          if (givenEnd)
          {
            to = endEnd.second;
            // A part that ends before it starts asks for nothing: the ask is not
            // one this machine can carry out, so it is left alone.
            if (to < from)
              understood = false;
          }
        }
      }
      // A Range this machine cannot read is not an answer about this file: it is
      // ignored, and the whole file is served, which is what HTTP asks of an
      // unknown ask. Only a range that reads well but reaches past the file's
      // end is answered as nothing to give.
      if (!understood)
      {
        r.headers["Accept-Ranges"] = "bytes";
        r.headers["Cache-Control"] = "public, max-age=31536000, immutable";
        r.setPlain(body, contentType);
        return r;
      }
      if (from < 0 || static_cast<unsigned long long>(from) >= body.size())
      {
        r.status = 416;
        r.headers["Content-Range"] = "bytes */" + std::to_string(body.size());
        r.headers["Content-Length"] = "0";
        // nothing was given for this ask, so nothing of it may be kept: the file
        // itself is still worth caching forever under its own name, which is what
        // the whole-file answers above say.
        r.headers["Cache-Control"] = "no-store";
        r.headers["Accept-Ranges"] = "bytes";
        return r;
      }
      if (to < 0 || static_cast<unsigned long long>(to) >= body.size())
        to = static_cast<long long>(body.size() - 1);
      r.status = 206;
      r.headers["Content-Range"] =
          "bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" +
          std::to_string(body.size());
      r.headers["Accept-Ranges"] = "bytes";
      // A served file's name is its job/item id: contents under a name never
      // change, so these may be cached forever.
      r.headers["Cache-Control"] = "public, max-age=31536000, immutable";
      r.setPlain(body.substr(static_cast<size_t>(from),
                             static_cast<size_t>(to - from + 1)),
                 contentType);
      return r;
    }
    r.headers["Accept-Ranges"] = "bytes";
    r.headers["Cache-Control"] = "public, max-age=31536000, immutable";
    r.setPlain(body, contentType);
    return r;
  }

  HttpResponse OpenLocalServices::jsonError(int status,
                                            const std::string &message) const
  {
    HttpResponse r;
    r.status = status;
    nlohmann::json j;
    std::string safe;
    constexpr size_t kMaxErrorLength = 200;
    for (char c : message)
    {
      if (safe.size() >= kMaxErrorLength)
      {
        safe += "...";
        break;
      }
      const unsigned char u = static_cast<unsigned char>(c);
      safe.push_back(u >= 0x20 && u < 0x7f ? c : '?');
    }
    j["error"] = safe;
    r.setJson(j.dump());
    // a refusal is never worth keeping: every one carries these words from here,
    // so a throw that answered outside the reading block below cannot leave a
    // refusal that a browser or a shared cache could hold onto.
    r.headers["Cache-Control"] = "no-store";
    return r;
  }

  // The newest AMD Vulkan driver package the Windows Driver Store holds: the
  // .inf_ directory that carries its manifest. The Vulkan loader reads a driver
  // only at the manifest it is pointed at, and with nothing pointing it there it
  // answers no drivers at all - which is what left the gpu chip reading "no gpu
  // driver" and vram not stated on a server started outside run.bat, whose
  // environment carried no VK_DRIVER_FILES. This is run.bat's rule, kept here so
  // the device report and the engines hold whichever way the server was started.
#ifdef _WIN32
  static std::string newestVulkanDriverManifest()
  {
    std::error_code ec{};
    const char *systemRoot = std::getenv("SystemRoot");
    std::filesystem::path store = (systemRoot && *systemRoot)
                                      ? std::filesystem::path{systemRoot}
                                      : std::filesystem::path{"C:\\Windows"};
    store /= "System32\\DriverStore\\FileRepository";
    if (!std::filesystem::is_directory(store, ec))
      return {};
    std::filesystem::path newest;
    std::filesystem::file_time_type newestStamp{};
    for (std::filesystem::directory_iterator it{store, ec}; it != std::filesystem::directory_iterator{};
         it.increment(ec))
    {
      std::error_code here{};
      if (!std::filesystem::is_directory(it->path(), here))
        continue;
      if (it->path().filename().string().find(".inf_") == std::string::npos)
        continue;
      if (!std::filesystem::is_regular_file(it->path() / "amd-vulkan64.json", here))
        continue;
      const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(it->path(), here);
      if (here)
        continue;
      if (newest.empty() || stamp > newestStamp)
      {
        newest = it->path();
        newestStamp = stamp;
      }
    }
    return newest.string();
  }
#endif

  // The ggml the voice models run on is linked into this binary, and it is the
  // machine's device probe. The backend this build was asked for is compiled into
  // it, so there is nothing here to go looking for: what this function used to do
  // was read the plugin libraries lying beside the server, and on a release folder
  // those are the chat engine's own ggml dlls.
  //
  // Measured on MSI's cuda-75 release (2026-10-28): the same RTX 2080 was
  // registered twice - once by the ggml inside the server, once by ggml-cuda.dll
  // loaded as a plugin from the folder - the start log printed ggml_cuda_init
  // twice for the one card, and the process failed (0xC0000409) as a finished
  // picture was handed back, with one copy's tensor standing in the other's
  // hands. The chat engine reads the ggml dlls it needs from chat-engine\ beside
  // its loader, in its own process, which is where those libraries belong; the
  // release scripts no longer lay a second copy of them beside the server.
  //
  // The probe is still asked for here, before the port is taken, so the first page
  // read states the device instead of paying for finding it.
  void OpenLocalServices::preloadBackends()
  {
#ifdef _WIN32
    // The manifest the loader reads its driver at. A name the environment
    // already carries is a person's own choice and stays, the same as every
    // other setting; the child engines this server starts inherit what is set
    // here, so they reach the same device.
    if (!std::getenv("VK_DRIVER_FILES"))
    {
      const std::string manifest = newestVulkanDriverManifest();
      if (!manifest.empty())
        SetEnvironmentVariableA("VK_DRIVER_FILES", manifest.c_str());
    }
#endif
    static_cast<void>(ggml_backend_dev_count());
  }

  // What share of each thread of the machine's processor is in use right now.
  // The kernel keeps, for every processor the machine has, a running total of the
  // wall time it has been on the clock and, of that, the time it spent working;
  // the share is what of the time between two readings was spent working, so the
  // first reading after a start states nothing and a processor that did nothing
  // between two readings is 0. A kernel that keeps no such record states nothing,
  // and the pages say so rather than drawing a flat line.
#ifdef _WIN32
  namespace
  {
    // The shape the kernel hands back for one processor, in its own order; only
    // the two running totals are read, the rest are named so the reading lands
    // in place.
    struct ProcessorTimes
    {
      std::int64_t stamp;
      std::int64_t idleAtStart;
      std::int64_t unused1;
      std::int64_t unused2;
      std::int64_t unused3;
      std::int64_t realTime; // the wall time this processor has been on the clock
      std::int64_t unused4;
      std::int64_t unused5;
      std::int64_t unused6;
      std::int64_t working; // of that, the time this processor spent working
    };
    std::mutex gThreadTimesMutex;
    std::vector<std::int64_t> gThreadWorking;
    std::vector<std::int64_t> gThreadReal;
    bool gThreadTimesRead = false;
  }
  // Asked of the kernel directly, the same way the task manager of the machine
  // reads it to draw one bar per thread; the number it is asked by names the
  // per-processor record.
  extern "C" __declspec(dllimport) long NtQuerySystemInformation(int, void *, unsigned long,
                                                                 unsigned long *);

  static nlohmann::json threadLoadJson()
  {
    unsigned long needed = 0;
    const int perProcessorRecord = 61;
    NtQuerySystemInformation(perProcessorRecord, nullptr, 0, &needed);
    if (needed == 0)
      return nullptr;
    const std::size_t each = sizeof(ProcessorTimes);
    const std::size_t count = static_cast<std::size_t>(needed) / each;
    if (count == 0)
      return nullptr;
    std::vector<ProcessorTimes> rows(count);
    if (NtQuerySystemInformation(perProcessorRecord, rows.data(),
                                 static_cast<unsigned long>(count * each), &needed) != 0)
      return nullptr;
    nlohmann::json out = nlohmann::json::array();
    std::lock_guard<std::mutex> guard(gThreadTimesMutex);
    if (!gThreadTimesRead || gThreadWorking.size() != count)
    {
      gThreadWorking.assign(count, 0);
      gThreadReal.assign(count, 0);
      gThreadTimesRead = false;
    }
    for (std::size_t at = 0; at < count; ++at)
    {
      const std::int64_t worked = rows[at].working;
      const std::int64_t onTheClock = rows[at].realTime;
      if (!gThreadTimesRead)
      {
        out.push_back(nullptr);
      }
      else
      {
        const std::int64_t grew = worked - gThreadWorking[at];
        const std::int64_t elapsed = onTheClock - gThreadReal[at];
        double share = 0.0;
        if (elapsed > 0 && grew > 0)
          share = 100.0 * static_cast<double>(grew) / static_cast<double>(elapsed);
        if (share > 100.0)
          share = 100.0;
        out.push_back(static_cast<double>(static_cast<long long>(share * 10.0 + 0.5)) / 10.0);
      }
      gThreadWorking[at] = worked;
      gThreadReal[at] = onTheClock;
    }
    gThreadTimesRead = true;
    return out;
  }
#endif

  std::string OpenLocalServices::systemJson() const
  {
    nlohmann::json devices = nlohmann::json::array();
    std::vector<std::string> names;
    // The same ggml the chat engines run on knows the machine's devices: it is
    // asked first, so the gpu and its memory are stated with the numbers the
    // backends themselves see. What it cannot name, the image service may still
    // list by name, and a device no library names is not drawn.
    const size_t count = ggml_backend_dev_count();
    for (size_t i = 0; i < count; ++i)
    {
      ggml_backend_dev_t dev = ggml_backend_dev_get(i);
      const enum ggml_backend_dev_type kind = ggml_backend_dev_type(dev);
      const std::string name = ggml_backend_dev_name(dev);
      if (std::find(names.begin(), names.end(), name) == names.end())
      {
        nlohmann::json entry = {{"name", name},
                                {"description", ggml_backend_dev_description(dev)},
                                {"type", kind == GGML_BACKEND_DEVICE_TYPE_GPU || kind == GGML_BACKEND_DEVICE_TYPE_IGPU
                                             ? "gpu"
                                             : (kind == GGML_BACKEND_DEVICE_TYPE_ACCEL ? "accel" : "cpu")}};
        size_t freeBytes = 0;
        size_t totalBytes = 0;
        ggml_backend_dev_memory(dev, &freeBytes, &totalBytes);
        if (totalBytes > 0)
        {
          entry["total_bytes"] = totalBytes;
          entry["free_bytes"] = freeBytes;
        }
        devices.push_back(entry);
        names.push_back(name);
      }
    }
    if (m_image)
    {
      const nlohmann::json extra = nlohmann::json::parse(m_image->devicesJson(), nullptr, false);
      if (!extra.is_discarded())
      {
        for (const auto &entry : extra)
        {
          const std::string name = entry.value("name", std::string());
          if (std::find(names.begin(), names.end(), name) == names.end())
          {
            devices.push_back(entry);
            names.push_back(name);
          }
        }
      }
    }
    nlohmann::json out = {{"devices", devices}};
    std::string processorName;
    unsigned long long memTotal = 0;
    unsigned long long memFree = 0;
    for (const auto &entry : devices)
    {
      if (entry.value("type", std::string()) != "cpu")
        continue;
      processorName = configTrim(entry.value("description", std::string()));
      if (!(memTotal > 0) && entry.contains("total_bytes"))
      {
        memTotal = entry.value("total_bytes", static_cast<std::size_t>(0));
        memFree = entry.value("free_bytes", static_cast<std::size_t>(0));
      }
    }
    out["cpu"] = {{"cores", static_cast<int>(std::thread::hardware_concurrency())},
                  {"name", processorName}};
    // The machine's own operating numbers: CPU load averages where the system
    // keeps them, and free space on the volume this server runs on. A value
    // the platform does not keep is omitted, and pages say so instead of
    // drawing a zero.
#ifndef _WIN32
    {
      double load[3] = {0.0, 0.0, 0.0};
      if (getloadavg(load, 3) == 3)
      {
        out["cpu"]["load"] = {{"one", load[0]}, {"five", load[1]}, {"fifteen", load[2]}};
      }
    }
#endif
#ifdef _WIN32
    {
      const nlohmann::json shares = threadLoadJson();
      if (!shares.is_null())
        out["cpu"]["threads_load"] = shares;
    }
#endif
    {
      unsigned long long diskTotal = 0;
      unsigned long long diskFree = 0;
      {
        std::error_code spaceError{};
        const std::filesystem::space_info space = std::filesystem::space(".", spaceError);
        if (!spaceError && space.capacity > 0)
        {
          diskTotal = static_cast<unsigned long long>(space.capacity);
          diskFree = static_cast<unsigned long long>(space.available);
        }
      }
      if (diskTotal > 0)
        out["disk"] = {{"total_bytes", diskTotal}, {"free_bytes", diskFree}};
    }
    // The code version this binary stands on, exactly as the build named it,
    // beside the compile moment it was built at: a development name is the dev
    // tag alone, a release names its commit - an artifact is tied to the one
    // source tree it can be re-created from.
    const std::string buildAt = platform::buildVersion();
    if (!buildAt.empty())
    {
      out["version"] = buildAt;
      out["compiled"] = platform::buildStamp();
    }
    if (memTotal > 0)
      out["memory"] = {{"total_bytes", memTotal}, {"free_bytes", memFree}};
    if (m_image)
      out["loaded"] = nlohmann::json::parse(m_image->statusJson(), nullptr, false);
    if (m_voice)
    {
      // The voice module's own door counts what stands loaded and names the ones
      // that do, in a short list; the packages themselves, with the words their
      // own files state, belong to the models door. The system block carries the
      // counts and that short list only, so a page that polls this snapshot
      // every few seconds never pays for a table of four hundred.
      nlohmann::json voiceBlock = nlohmann::json::parse(m_voice->statusJson(), nullptr, false);
      if (voiceBlock.is_discarded())
        voiceBlock = nlohmann::json::object();
      out["voice"] = voiceBlock;
    }
    return out.dump();
  }

  // A cancel that lands on a job already stopped is not an ask for a job nothing
  // knows: the words say which of the two this id is, so a page that pressed stop
  // twice learns the job stopped rather than that the id went missing.
  static std::string cancelRefusalWords(const nlohmann::json &state, const std::string &id)
  {
    if (state.is_discarded() || ols::jobState(state) == ols::kStateUnknown)
      return "Unknown job: " + id;
    return "This job has already stopped";
  }

  // A job's doors open with a post, since starting a job is the work the answer
  // goes on with: the words name that, so a page that reached the door with the
  // wrong method learns what to do about the ask rather than reading a bare
  // refusal.
  static const char *postOnlyDoorWords()
  {
    return "That door opens with a post";
  }

  // What a refused room answers with: a browser cannot read a refusal, so the
  // visitor is walked to the one address of this machine that stands open to
  // nothing it asks of and takes a credential as it is handed. The room walked
  // away from is named there, in the address as well as in the answer, so the
  // page stood there can offer that room back. No credential is ever carried
  // in an address.
  static HttpResponse roomWalksToTheDoor(const std::string &room)
  {
    HttpResponse r;
    r.status = 307;
    nlohmann::json body;
    body["address"] = "/";
    body["room"] = room;
    body["how"] = "hand the key there, then read the room again";
    r.setJson(body.dump());
    r.headers["Location"] =
        room.empty() ? std::string("/") : "/?room=" + encodeCookieValue(room);
    r.headers["Cache-Control"] = "no-store";
    return r;
  }

  // The page a person is stood at that address on: what happened, the form that
  // hands a credential here, and - when one was taken - the way back to the room
  // this visit came from. It carries its own sheet rather than the rooms', since
  // the rooms' sheet is one of the rooms and would be refused again before any
  // key had been handed.
  static HttpResponse theDoorAnswer(const std::string &whatHappened,
                                    const std::string &room, bool offersForm,
                                    bool walksBack)
  {
    std::string here = onePageName(room);
    const std::string back = "/" + here;
    const std::string backSafe = escapeHtmlText(back);
    std::string page =
        "<!doctype html>\n<html lang=\"en\">\n<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>open-local-services - the door of this machine</title>\n"
        "</head>\n";
    if (walksBack)
      page += "<meta http-equiv=\"refresh\" content=\"2; url=" + backSafe + "\">\n";
    // The door stands open to every visit, so the rooms' sheet cannot answer it -
    // it is one of the rooms, and a room that refuses is walked away from before
    // any key has been handed. What this page is dressed in is therefore its own,
    // and a browser reads a page's own words for its own parts as an attribute on
    // each part rather than a sheet written into the page, which its policy of
    // these pages keeps out.
    page += "<body style=\"background:#151a22;color:#dce3ec;font-family:system-ui,"
            "sans-serif;max-width:38rem;margin:auto;padding:2rem\">\n";
    page += "<h1 style=\"font-size:1.3rem\">This machine asks for a key at its doors</h1>\n";
    page += "<p>" + escapeHtmlText(whatHappened) + "</p>\n";
    page += "<p>Nothing of this machine - not its pages, not its api doors - is "
            "served before a key it set for itself opens it. A key is never "
            "written into an address.</p>\n";
    if (offersForm)
    {
      page += "<p>Hand the key to the form below. A key this machine takes is written on "
              "this connection, so the rest of this visit carries it.</p>\n";
      page += "<form method=\"post\" action=\"/\">\n";
      page += "<p><label for=\"handed\">the key this machine asks for: <input type=\"text\" "
              "name=\"handed\" placeholder=\"its key\" style=\"color-scheme:dark\"></label></p>\n";
      page += "<input type=\"hidden\" name=\"room\" value=\"" + escapeHtmlText(here) + "\">\n";
      page += "<p><button type=\"submit\">hand it over</button></p>\n";
      page += "</form>\n";
    }
    if (walksBack)
      page += "<p><a href=\"" + backSafe + "\" style=\"color:#8ecff6\">" + escapeHtmlText(here) +
              "</a> is the room this visit came from. It opens now that the key is on the "
              "connection.</p>\n";
    page += "<p>Reading this as a program? The same words as json are at this same "
        "<p>Reading this as a program? The same words as json are at this same "
        "address, and a credential a program hands goes in a header: <code>X-Api-Key</code> "
        "or <code>Authorization: Bearer</code>.</p>\n";
    page += "</body>\n</html>\n";
    HttpResponse r;
    r.setPlain(page, "text/html; charset=utf-8");
    r.headers["X-Content-Type-Options"] = "nosniff";
    r.headers["Referrer-Policy"] = "no-referrer";
    r.headers["Cache-Control"] = "no-store";
    return r;
  }

  // What the form on that page hands at the same address: the same decision every
  // door takes a credential at, carried as a page, and a key this machine takes
  // is written on the connection exactly as it is written there.
  static HttpResponse handedAtTheDoor(const HttpRequest &req, const std::string &wanted)
  {
    const std::string offered = queryValue(req.body, "handed");
    const std::string room = queryValue(req.body, "room");
    const bool asked = !wanted.empty();
    if (offered.empty())
      return theDoorAnswer("Nothing was handed to the door.", room, true, false);
    if (!asked)
      return theDoorAnswer(
          "This machine asks for no key, so its rooms are open as they are.", room, false, true);
    if (!asked || !keysMatch(offered, wanted))
      return theDoorAnswer(
          "That is not the key this machine asks for. Nothing was written on this connection.",
          room, true, false);
    HttpResponse r = theDoorAnswer(
        "This machine took the key you handed. It is written on this connection now, and no key "
        "is named in an address.",
        room, false, true);
    r.headers["Set-Cookie"] = "ols-key=" + encodeCookieValue(offered) + "; Path=/; SameSite=Lax";
    return r;
  }

  HttpResponse OpenLocalServices::route(const HttpRequest &req)
  {
    const std::string path = req.path;
    // A HEAD is a GET asked without its body, so it is read by the very doors a
    // GET is read by - a reader that asks only how long an answer is, or whether
    // it stands at all, is turned away by a door that would have answered it. The
    // body is taken off the answer afterwards, by the one place that writes an
    // answer out.
    const bool get = req.method == "GET" || req.method == "HEAD";
    const bool head = req.method == "HEAD";
    const bool post = req.method == "POST";
    const bool del = req.method == "DELETE";
    const bool fetch = get;

    // The one address that stands open to nothing it asks of, so a visit reaches
    // it before anything else can: the bare address of this machine, where a
    // credential is handed over and where what the doors ask of is stated. A
    // program that asks with no Accept, or with */*, gets the json this machine
    // has always answered; a person who was walked here from a refused room gets
    // a page they can answer, because a person cannot read a json answer.
    if (path == "/")
    {
      if (post)
      {
        if (browserOpeningDocument(req))
          return handedAtTheDoor(req, apiKey());
        return certifyAnswer(req);
      }
      if (!get && !head)
        return jsonError(405,
                         "This address is read with a GET and handed a credential with a POST");
      if (browserOpeningDocument(req))
      {
        const std::string room = queryValue(req.query, "room");
        std::string lead =
            "This address is where a visit hands over a key. No room was named on this visit - someone opened the door itself.";
        if (!room.empty())
          lead = "The room " + room + " refused this visit: no credential came with it. This is the door where a key is handed over.";
        return theDoorAnswer(lead, room, true, false);
      }
      return jsonResponse(200, authInfoJson());
    }

    // The rooms and everything they load: nothing of the webui is served
    // before a credential opens it. A refused room walks to the door naming
    // the room, because a browser cannot read a refusal and the door is
    // where a credential is handed over; a refused file the rooms load walks
    // there plainly, since it asked for no page of its own.
    if (fetch && (path == "/index.html" || webAssetForPath(path)))
    {
      const int open = authGate(req);
      if (open == -1)
        return noResponse();
      if (open == 1)
      {
        const size_t dot = path.find_last_of('.');
        const std::string ext =
            dot == std::string::npos ? std::string() : lowerText(path.substr(dot));
        std::string room;
        if (ext == ".html")
        {
          const size_t slash = path.find_last_of('/');
          room = path.substr(slash == std::string::npos ? 0 : slash + 1);
        }
        return roomWalksToTheDoor(room);
      }
      return serveStatic(path, req);
    }

    // Every door answers to the api door's ask: this machine's key.
    const int auth = authGate(req);
    if (auth == 1)
      return authChallenge();
    if (auth == -1)
      return noResponse();

    if (const int retry = rateGate())
      return rateLimitAnswer(retry);

    if (get && path == "/api/models")
      return m_image ? jsonResponse(200, m_image->modelsJson())
                     : jsonError(502, "No image service");
    if (get && path == "/api/jobs")
      return m_image ? jsonResponse(200, m_image->jobsListJson())
                     : jsonError(502, "No image service");
    if (get && path == "/api/timings")
      return m_image ? jsonResponse(200, m_image->timingsJson())
                     : jsonError(502, "No image service");

    if (get && path == "/api/gallery")
    {
      if (!m_image)
        return jsonError(502, "No image service");
      static const long long kGalleryPage = 100;
      return jsonResponse(200, m_image->galleryJson(
                                   static_cast<int>(queryInt(req.query, "limit", kGalleryPage)),
                                   static_cast<int>(queryInt(req.query, "offset", 0))));
    }
    if (path.rfind("/api/gallery/", 0) == 0)
    {
      if (!m_image)
        return jsonError(502, "No image service");
      const std::string item = path.substr(13);
      if (get)
      {
        const std::string one = m_image->galleryItemJson(item);
        if (one.empty())
          return jsonError(404, "Unknown item: " + item);
        return jsonResponse(200, one);
      }
      if (del)
      {
        if (!m_image->removeOutput(item))
          return jsonError(404, "Unknown item: " + item);
        return jsonResponse(200, "{}");
      }
      return jsonError(405, "A gallery item accepts only GET or DELETE");
    }

    if (get && path == "/api/refs")
    {
      if (!m_image)
        return jsonError(502, "No image service");
      return jsonResponse(200, m_image->refsJson());
    }
    if (post && path == "/api/refs")
    {
      if (!m_image)
        return jsonError(502, "No image service");
      const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
      if (body.is_discarded() || !body.is_object())
        return jsonError(400, "Invalid request body");
      const std::string keptName = ols::statedString(body, "name");
      const std::string pictureB64 = ols::statedString(body, "image_b64");
      if (!pictureB64.empty())
      {
        // a picture the browser is holding has no path on this machine, so it
        // arrives as its own bytes; the answer is the same list as a keep from
        // the gallery, so the page draws what it just sent without asking again
        if (!m_image->addRefBytes(keptName, pictureB64, ols::statedString(body, "format")))
        {
          const std::string why = m_image->lastRefError();
          if (!why.empty())
            return jsonError(400, why);
          return jsonError(500, "Reference could not be kept");
        }
        return jsonResponse(200, m_image->refsJson());
      }
      const std::string sourceName = ols::statedString(body, "source");
      if (sourceName.empty())
        return jsonError(400, "Nothing to keep: the ask names neither a picture nor a source");
      if (!m_image->addRef(sourceName, keptName))
      {
        // The words the module gave are about this ask, not about a door that
        // broke, so they answer as a bad ask does - same as the picture-by-bytes
        // hand above.
        const std::string why = m_image->lastRefError();
        if (!why.empty())
          return jsonError(400, why);
        return jsonError(404, "Unknown source: " + sourceName);
      }
      return jsonResponse(200, m_image->refsJson());
    }
    if (path.rfind("/api/refs/", 0) == 0)
    {
      if (!m_image)
        return jsonError(502, "No image service");
      const std::string kept = path.substr(10);
      if (del)
      {
        if (!m_image->removeRef(kept))
          return jsonError(404, "Unknown reference: " + kept);
        return jsonResponse(200, "{}");
      }
      return jsonError(405, "A saved reference accepts only DELETE");
    }

    if (post && path == "/api/unload")
    {
      if (!m_image)
        return jsonError(502, "No image service");
      const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
      if (body.is_discarded())
        return jsonError(400, "Invalid request body");
      const std::string model = body.value("model", std::string());
      if (!m_image->unload(model))
        return jsonError(404, "That model holds no memory: " + model);
      return jsonResponse(200, "{}");
    }
    if (get && path == "/api/prefs")
    {
      return m_image ? jsonResponse(200, m_image->prefsJson()) : jsonError(502, "No image service");
    }
    if (post && path == "/api/prefs")
    {
      if (!m_image)
        return jsonError(502, "No image service");
      const nlohmann::json given = nlohmann::json::parse(req.body, nullptr, false);
      if (given.is_discarded() || !given.is_object())
        return jsonError(400, "Invalid request body");
      if (!m_image->setPrefs(req.body))
        return jsonError(500, "Preferences could not be saved on this machine");
      return jsonResponse(200, m_image->prefsJson());
    }
    if (path == "/api/webui/expand")
    {
      // Read with a GET, which says where the pages come from now; posted, which
      // writes them out beside the server and serves those copies from then on.
      // The panel reads this before it offers the hand, so a machine already
      // serving from a folder does not offer a hand that can only refuse.
      if (get)
        return jsonResponse(200, servedPagesJson());
      if (!post)
        return jsonError(405, "The pages are read with a GET and taken out of the server with a POST");
      return expandStaticDir();
    }
    // What this machine does about starting this server by itself. Nothing here
    // installs a server anywhere: it is this program, out of this folder, set to
    // start when the machine starts, disabled without stopping anything that is
    // running, taken off the machine's list, started again with the program on
    // disk as this process's own child (the soft hand, which carries the level
    // this server stands at and so never asks the machine again), or stopped. The
    // words a person's machine's own manager answered with come back whole,
    // because a person reading this needs what the machine said and not what was
    // hoped.
    if (path == "/api/service")
    {
      if (get)
        return jsonResponse(200, ols::deploy::stateJson());
      if (!post)
        return jsonError(405, "That door is read with a GET and worked with a POST");
      const nlohmann::json given = nlohmann::json::parse(req.body, nullptr, false);
      if (given.is_discarded() || !given.is_object())
        return jsonError(400, "Send {\"action\": \"startup_on\", \"startup_off\", \"remove\", "
                              "\"restart\" or \"shutdown\"}");
      const std::string action = given.value("action", std::string());
      std::string words;
      const bool done = ols::deploy::act(action, words);
      nlohmann::json out;
      out["did"] = done;
      out["words"] = words;
      if (action == "restart" && done)
      {
        // The soft hand's first half: this server stops answering at its door,
        // and the process that carries it on is started where main ends, once the
        // listening has closed - a child that found the port still held would
        // only refuse and leave nothing standing.
        stopAfterThisAnswer();
      }
      else if (action != "shutdown")
      {
        const nlohmann::json after = nlohmann::json::parse(ols::deploy::stateJson(), nullptr, false);
        if (!after.is_discarded())
          out["service"] = after;
      }
      // The .env value and what the machine now does are one fact, so the ask that
      // changes one writes the other beside it. Removing the entry is such an ask
      // too: a line that still says this machine starts the server at boot would
      // be a lie once nothing stands to start it.
      if (done && (action == "startup_on" || action == "startup_off" || action == "remove"))
      {
        const std::string want = action == "startup_on" ? "true" : "false";
        const std::vector<std::pair<std::string, std::string>> changes = {{"OLS_STARTUP", want}};
        std::string fileError;
        out["env_words"] = configUpdateFile(changes, fileError)
                               ? ("OLS_STARTUP=" + want + " written to .env")
                               : ("the machine did it, but the .env line was not written: " +
                                  fileError);
      }
      return jsonResponse(done ? 200 : 500, out.dump());
    }

    if (path == "/api/config")
    {
      if (get)
        return jsonResponse(200, configStateJson());
      if (!post)
        return jsonError(405, "That door is read with a GET and changed with a POST");
      const nlohmann::json given = nlohmann::json::parse(req.body, nullptr, false);
      if (given.is_discarded() || !given.is_object() || given.empty())
        return jsonError(400, "Invalid request body");
      std::vector<std::pair<std::string, std::string>> changes;
      for (const auto &entry : given.items())
      {
        if (!entry.value().is_string())
          return jsonError(400, "Config option values must be strings");
        const std::string name = entry.key();
        bool known = false;
        for (const ConfigOption &option : kConfigOptions)
          if (name == option.name)
          {
            known = true;
            break;
          }
        if (!known)
          return jsonError(400, "Unknown option '" + name + "'");
        const std::string value = entry.value().get<std::string>();
        if (!configValueShapeOk(name, value))
        {
          // A refusal says which rule the value broke, because the page cannot
          // guess whether it sent a word that means nothing or a number the
          // machine would go on to ignore.
          if (name == "OLS_STARTUP")
            return jsonError(400, "Invalid value for 'OLS_STARTUP': this one is true or false"
                                  " - true sets this machine to start this server when it boots,"
                                  " false leaves it alone, and a blank leaves it unset");
          if (name == "OLS_PORT")
            return jsonError(400, "Invalid value for 'OLS_PORT': a blank leaves the default 8765,"
                                  " otherwise this must be a port number from 1 to 65535.");
          if (name == "OLS_RATE_LIMIT")
            return jsonError(400, "Invalid value for 'OLS_RATE_LIMIT': a blank leaves the default 600,"
                                  " 0 disables the rate limit, and otherwise this must be requests per minute from 1 to 1000000.");
          return jsonError(400, "Invalid value for '" + name +
                                    "': a blank leaves it unset, a number must be one the "
                                    "machine can use"
                                    " (at least 1 for the counts)");
        }
        changes.push_back({name, value});
      }
      std::string error;
      if (!configUpdateFile(changes, error))
        return jsonError(500, error);
      for (const auto &change : changes)
      {
        if (change.first == "HF_TOKEN")
        {
          if (m_image)
            m_image->setHfToken(change.second);
        }
        else if (change.first == "CIVITAI_API_KEY")
        {
          if (m_image)
            m_image->setCivitaiKey(change.second);
        }
        else if (change.first == "OLS_STARTUP")
        {
          // A line in a file that the machine never reads would be a lie about the
          // boot, so the wish is carried to the manager the moment it is written.
          // What the manager answered is kept for the page to read back.
          std::string words;
          ols::deploy::act(truthyValue(change.second) ? "startup_on" : "startup_off", words);
        }
        else if (change.first == "OLS_API_KEY")
        {
          m_apiKey = change.second;
        }
        else if (change.first == "OLS_RATE_LIMIT")
        {
          setRateLimit(change.second.empty() ? 600 : std::atoi(change.second.c_str()));
        }
      }
      // A save answers with the same rows it read: the page reads its rows back
      // from the answer, and a save that carried no rows back would leave the
      // page to guess whether its own change stood.
      return jsonResponse(200, configStateJson());
    }
    if (post && path == "/api/generate")
    {
      if (!m_image)
        return jsonError(502, "No image service");
      const std::string job = m_image->startJob("generate", std::string(), req.body);
      if (job.empty())
        return jsonError(400, m_image->engineError());
      return jsonResponse(202, idJson(job));
    }

    if (get && path == "/voice/api/models")
      return m_voice ? jsonResponse(200, m_voice->modelsJson())
                     : jsonError(502, "No voice service");
    // A page of the cards, and the counts of the whole set: the words of the
    // question pass through untouched, so the page asks in the same words its
    // own controls show and pays for one page rather than for the table.
    if (get && path == "/voice/api/model-cards")
      return m_voice ? jsonResponse(200, m_voice->modelCardsJson(req.query))
                     : jsonError(502, "No voice service");
    if (get && path == "/voice/api/model-stats")
      return m_voice ? jsonResponse(200, m_voice->modelsSummaryJson())
                     : jsonError(502, "No voice service");
    if (get && (path == "/voice/api/status" || path == "/voice/api/health"))
      return m_voice ? jsonResponse(200, m_voice->statusJson())
                     : jsonError(502, "No voice service");
    if (get && path == "/voice/api/jobs")
      return m_voice ? jsonResponse(200, m_voice->jobsListJson())
                     : jsonError(502, "No voice service");
    if (get && path == "/voice/api/library")
      return m_voice ? jsonResponse(200, m_voice->libraryJson())
                     : jsonError(502, "No voice service");
    if (path.rfind("/voice/api/voices/", 0) == 0)
    {
      const std::string model = path.substr(18);
      // save, delete, and rename are doors this server answers below; they are
      // not model names, and a page that posts to one must not be read as
      // asking for a model called save.
      if (model != "save" && model != "delete" && model != "rename")
      {
        if (!m_voice)
          return jsonError(502, "No voice service");
        if (!model.empty() && !m_voice->known(model))
          return jsonError(404, "Unknown model: " + model);
        if (!get)
          return jsonError(404, "Not found");
        return jsonResponse(200, m_voice->voicesJson(model));
      }
    }
    if (post && path == "/voice/api/voices/save")
    {
      if (!m_voice)
        return jsonError(502, "No voice service");
      const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
      if (body.is_discarded())
        return jsonError(400, "Invalid request body");
      if (!m_voice->saveVoice(body.value("name", std::string()), body.value("audio_b64", std::string()),
                              body.value("prompt_text", std::string())))
        return jsonError(400, "The recording was not kept: it is not a plain wav, or the name is not one");
      return jsonResponse(200, "{}");
    }
    if (post && path == "/voice/api/voices/delete")
    {
      if (!m_voice)
        return jsonError(502, "No voice service");
      const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
      const std::string refused =
          body.is_discarded()
              ? std::string("Invalid request body")
              : m_voice->removeVoiceReason(body.value("name", std::string()));
      if (!refused.empty())
        return jsonError(400, refused);
      return jsonResponse(200, "{}");
    }
    if (post && path == "/voice/api/voices/rename")
    {
      if (!m_voice)
        return jsonError(502, "No voice service");
      const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
      const std::string refused =
          body.is_discarded()
              ? std::string("Invalid request body")
              : m_voice->renameVoice(body.value("name", std::string()),
                                     body.value("new_name", std::string()));
      if (!refused.empty())
        return jsonError(400, refused);
      return jsonResponse(200, "{}");
    }
    if (post && path == "/voice/api/listen")
    {
      if (!m_voice)
        return jsonError(502, "No voice service");
      const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
      if (body.is_discarded())
        return jsonError(400, "Invalid request body");
      const std::string job = m_voice->startJob("listen", body.value("model", std::string()), req.body);
      if (job.empty())
        return jsonError(400, "Unknown model for this machine, or no recording given");
      return jsonResponse(202, idJson(job));
    }
    if (post && path == "/voice/api/speak")
    {
      if (!m_voice)
        return jsonError(502, "No voice service");
      const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
      if (body.is_discarded())
        return jsonError(400, "Invalid request body");
      const std::string job = m_voice->startJob("speak", body.value("model", std::string()), req.body);
      if (job.empty())
        return jsonError(400, "Unknown model for this machine, or nothing to say");
      return jsonResponse(202, idJson(job));
    }
    // A voice model is opened or put down on this door: the engine is this
    // process, so the answer is the engine's own yes - no port is started and
    // no child is asked.
    if (path.rfind("/voice/api/models/", 0) == 0)
    {
      const std::string rest = path.substr(18);
      const size_t slash = rest.find('/');
      if (slash == std::string::npos)
        return jsonError(404, "Not found");
      const std::string model = rest.substr(0, slash);
      const std::string action = rest.substr(slash + 1);
      if (!m_voice || !m_voice->known(model))
        return jsonError(404, "Unknown model: " + model);
      if (!post)
        return jsonError(405, postOnlyDoorWords());
      if (action == "unload")
      {
        if (!m_voice->unloadModel(model))
          return jsonError(404, "That model holds no memory");
        return jsonResponse(200, "{}");
      }
      if (action == "download")
      {
        const std::string job = m_voice->startDownload(model);
        if (job.empty())
          return jsonError(404, "That package names no files to fetch");
        return jsonResponse(202, idJson(job));
      }
      if (action != "load")
        return jsonError(404, "Not found");
      if (!m_voice->loadModel(model))
      {
        // the reason belongs to this card: the words another model gave last
        // would be read as about this one
        const std::string why = m_voice->loadErrorFor(model);
        return jsonError(400, why.empty() ? "That model's files are not on this machine" : why);
      }
      return jsonResponse(200, "{}");
    }
    if (path.rfind("/voice/api/jobs/", 0) == 0)
    {
      if (!m_voice)
        return jsonError(502, "No voice service");
      const std::string rest = path.substr(16);
      const size_t slash = rest.find('/');
      const std::string id = slash == std::string::npos ? rest : rest.substr(0, slash);
      const std::string action = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
      nlohmann::json state = nlohmann::json::parse(m_voice->jobStateJson(id), nullptr, false);
      // this same reading is what the stop ask below answers with, so the record
      // is read once for both doors.
      if (action == "cancel" && post)
      {
        if (m_voice->cancelJob(id))
          return jsonResponse(200, "{}");
        return jsonError(404, cancelRefusalWords(state, id));
      }
      if (!get)
        return jsonError(404, "Not found");
      if (state.is_discarded() || ols::jobState(state) == ols::kStateUnknown)
        return jsonError(404, "Unknown job: " + id);
      return jsonResponse(200, state.dump());
    }
    if (post && path == "/voice/api/library/delete")
    {
      if (!m_voice)
        return jsonError(502, "No voice service");
      const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
      if (body.is_discarded())
        return jsonError(400, "Invalid request body");
      const std::string renderId = body.value("id", std::string());
      if (!m_voice->removeRender(renderId))
        return jsonError(404, "Unknown render: " + renderId);
      return jsonResponse(200, "{}");
    }

    if (path.rfind("/api/models/", 0) == 0)
    {
      const std::string rest = path.substr(12);
      const size_t slash = rest.find('/');
      if (slash == std::string::npos)
        return jsonError(404, "Not found");
      const std::string model = rest.substr(0, slash);
      const std::string action = rest.substr(slash + 1);
      if (!m_image || !m_image->known(model))
        return jsonError(404, "Unknown model: " + model);
      if (action != "download" && action != "load")
        return jsonError(404, "Not found");
      if (!post)
        return jsonError(405, postOnlyDoorWords());
      const std::string job = m_image->startJob(action, model, req.body);
      if (job.empty())
        return jsonError(400, m_image->engineError());
      return jsonResponse(202, idJson(job));
    }

    if (path.rfind("/api/jobs/", 0) == 0)
    {
      if (!m_image)
        return jsonError(502, "No image service");
      const std::string rest = path.substr(10);
      const size_t slash = rest.find('/');
      const std::string id = slash == std::string::npos ? rest : rest.substr(0, slash);
      const std::string action = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
      if (action == "cancel" && post)
      {
        if (!m_image->cancelJob(id))
        {
          const nlohmann::json state = nlohmann::json::parse(m_image->jobStateJson(id), nullptr, false);
          return jsonError(404, cancelRefusalWords(state, id));
        }
        return jsonResponse(200, "{}");
      }
      if (!get)
        return jsonError(404, "Not found");
      const nlohmann::json state = nlohmann::json::parse(m_image->jobStateJson(id), nullptr, false);
      if (state.is_discarded() || ols::jobState(state) == ols::kStateUnknown)
        return jsonError(404, "Unknown job: " + id);
      return jsonResponse(200, state.dump());
    }

    if (fetch && path.rfind("/voice/files/", 0) == 0)
    {
      const std::string rest = path.substr(13);
      const size_t slash = rest.find('/');
      if (slash == std::string::npos)
        return jsonError(404, "Not found");
      return serveFile(rest.substr(0, slash), rest.substr(slash + 1), "voice", req);
    }

    if (fetch && path.rfind("/files/", 0) == 0)
    {
      const std::string rest = path.substr(7);
      const size_t slash = rest.find('/');
      if (slash == std::string::npos)
        return jsonError(404, "Not found");
      return serveFile(rest.substr(0, slash), rest.substr(slash + 1), "image", req);
    }

    if (get && path == "/api/system")
      return jsonResponse(200, systemJson());

    if (get && path == "/api/monitor")
    {
      const std::string imageJobs = m_image ? m_image->jobsListJson() : std::string("[]");
      const std::string voiceJobs = m_voice ? m_voice->jobsListJson() : std::string("[]");
      return jsonResponse(200, monitor::snapshot(systemJson(), imageJobs, voiceJobs));
    }

    // The network door. Reading it hands back the whole document this machine's
    // network makes up: its adapters and their settings, the rates between two
    // peeks of the counters, the wifi state, and what this kind of machine can
    // change from here at all.
    // Working it posts one action and its parts, and the answer is the machine's
    // own tools' words - netsh on Windows, netplan and the rest on Linux - never
    // a guess about what probably happened.
    if (path == "/api/network")
    {
      if (get)
        return jsonResponse(200, ols::net::snapshotJson());
      if (!post)
        return jsonError(405, "That door is read with a GET and worked with a POST");
      if (nlohmann::json::parse(req.body, nullptr, false).is_discarded())
        return jsonError(400, "Send one json object naming an action and its parts");
      return jsonResponse(200, ols::net::actJson(req.body));
    }

    if (get && path == "/api/network/scan")
    {
      // A plain read answers from the scan the machine last ran; asking with
      // fresh=1 wakes the radio itself, which costs about a second and a half.
      const bool fresh = truthyValue(queryValue(req.query, "fresh"));
      return jsonResponse(200, ols::net::scanJson(fresh));
    }

    // A path nothing answers is the caller's mistake, not a broken service:
    // 404 says this server has no such door, while 502 is kept for a module
    // that is genuinely not wired into this binary.
    return jsonError(404, "No route");
  }

  // Whether the reader at this door asked for a document: a person says so with
  // their Accept line, and a program that says nothing, or says anything at all,
  // keeps the json this machine has always answered. Nothing here changes what a
  // door asks of - only the shape its answer is carried in.
  bool OpenLocalServices::browserOpeningDocument(const HttpRequest &req) const
  {
    const auto accept = req.headers.find("accept");
    if (accept == req.headers.end())
      return false;
    return accept->second.find("text/html") != std::string::npos;
  }

} // namespace ols
