#include "OpenLocalServices.h"
#include "Deployment.h"
#include "ImageService.h"
#include "MonitorService.h"
#include "NetworkService.h"
#include "ProjectService.h"
#include "ProjectGit.h"
#include "ProjectPulls.h"
#include "ProjectIssues.h"
#include "gitcore/merge.h"
#include "gitcore/diff.h" // the exact rename pairing and the blob diff the issue re-anchor reads
#include "GitSmartHttp.h"
#include "SshFetch.h"
#include "gitcore/wire.h"
// The wire unit's ground again, on the same road GitSmartHttp.cpp takes: the
// definitions are inline and no build names wire.cpp as a source, so the doors
// that answer with these functions carry the ground in their own objects, and
// one foldable copy is all a link needs.
#include "gitcore/wire.cpp"
#include "WebAssets.h"
#include "Common.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
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
#include "TlsLayer.h"
#include "HardwareInfo.h"

#ifndef _WIN32
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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
      // The studio's live-invalidations stream (§9.3) stands beside the job
      // streams: one address, every project's stamps ride it.
      if (path == "/project/api/events")
        return true;
      if (path.size() < 8 || path.compare(path.size() - 7, 7, "/events") != 0)
        return false;
      return path.rfind("/api/jobs/", 0) == 0;
    }

    // The git smart-HTTP doors carry bodies the json cap cannot hold: a push's
    // pack and a fetch's answer are streams, not messages. Their body cap is
    // stated here once, read from OLS_GIT_MAX_BODY_MIB at startup (default 512);
    // every other door keeps kMaxBodyBytes untouched.
    unsigned long long gitBodyCap()
    {
      static const unsigned long long cap = []
      {
        const char *env = std::getenv("OLS_GIT_MAX_BODY_MIB");
        if (env != nullptr)
        {
          try
          {
            const unsigned long long mib = std::stoull(env);
            if (mib > 0 && mib <= 16384ull)
              return mib * 1024ull * 1024ull;
          }
          catch (const std::exception &)
          {
          }
        }
        return 512ull * 1024ull * 1024ull;
      }();
      return cap;
    }

    bool isGitDoorPath(const std::string &path)
    {
      return path.rfind("/git/", 0) == 0;
    }

    // How long one connection may live, from its first byte to the end of its
    // answer, whatever pace the client keeps its bytes at. The header read, the
    // body reads, and the draining of a refused body all look at this clock: a
    // slow drip may hold a thread for two minutes and no longer. The per-read
    // timeout bounds the overshoot - the read running when the deadline passes
    // may still run out its ten seconds.
    constexpr long long kConnectionOverallMs = 120000;

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
        // that declines the ask - and a server that does not stand raised does
        // not serve at all, it says so and exits.
        {"OLS_ELEVATE", false, false},
        // Which face these doors answer with: blank or off is the clear one
        // they have always had; auto answers plain and sealed alike on the
        // one port, making the certificates under data/tls on first start;
        // on answers sealed connections only - a credential-free GET gets
        // one https redirect there, everything else stands. Read at the next
        // start; the cert panel stands at /project/api/cert beside it.
        {"OLS_TLS", false, false},
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
      // The TLS face is one of three words or unset: a word beside those
      // three is a maybe about how the port answers, and no face is a maybe.
      if (key == "OLS_TLS")
        return value.empty() || value == "off" || value == "on" || value == "auto";
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

    // The sealed face's own two rules about a plain visitor (spec §10.3). A
    // redirect moves one GET - no method that could carry a body is ever
    // moved - to the https shape of its own address. An ask wearing a
    // credential in its query or headers stands exactly where it is: moving
    // it would send what it carries over the clear again. The bytes are read
    // with MSG_PEEK only, so the visitor's real request line survives for
    // whatever road follows; nothing here answers before a whole first line
    // stands, so a probe of two bytes learns nothing about this door.
    bool sealedRedirectOk(const HttpRequest &req)
    {
      if (req.method != "GET")
        return false;
      // A credential in the query never travels: the ?key= form is spelled
      // with its own name, read straight off the query string here rather
      // than through the map helper that lives further down this block.
      for (size_t at = req.query.find("key="); at != std::string::npos;
           at = req.query.find("key=", at + 1))
      {
        if (at == 0u || req.query[at - 1u] == '&')
          return false;
      }
      for (const auto &entry : req.headers)
      {
        if ((entry.first == "authorization" || entry.first == "cookie") && !entry.second.empty())
          return false;
      }
      return true;
    }

    bool blockHasHeaderEnd(const std::string &block)
    {
      return block.find("\r\n\r\n") != std::string::npos;
    }

    // The request line, read the way the transport reads it - verb and target,
    // split at the query mark - into the parts the redirect decision asks
    // about. normalPath belongs to the class itself (below this block), so
    // only the raw target is kept here; the path never leaves this decision.
    void parseRequestLine(const std::string &line, HttpRequest &into)
    {
      std::istringstream rl(line);
      std::string httpVersion;
      rl >> into.method >> into.path >> httpVersion;
      const size_t q = into.path.find('?');
      if (q != std::string::npos)
      {
        into.query = into.path.substr(q + 1);
        into.path.resize(q);
      }
    }

    // The Host line of a header block, in its own trimmed words. Case does not
    // matter; a later Host replaces an earlier one, exactly as the transport's
    // own header map would leave it standing last.
    void readHostHeader(const std::string &headerBlock, std::string &hostOut)
    {
      hostOut.clear();
      std::istringstream hs(headerBlock);
      std::string line;
      std::getline(hs, line); // the request line itself is not a header
      while (std::getline(hs, line))
      {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
          continue;
        if (lowerText(line.substr(0, colon)) == "host")
          hostOut = configTrim(line.substr(colon + 1));
      }
    }

    // Where a plain visitor is pointed, in the browser's own words: the Host
    // header carries the address that visitor asked for - which may name its
    // port and may not - so an absent port comes back as the port this
    // server actually stands at. A host no machine can dress in https (no
    // letters, or characters outside the address alphabet) is refused here:
    // those bytes never enter a Location header.
    bool sealedRedirectTarget(const std::string &hostHeader, uint16_t port,
                              const std::string &pathAndQuery, std::string &urlOut)
    {
      urlOut.clear();
      std::string host = configTrim(hostHeader);
      if (host.size() >= 2u && host[0] == '[')
      {
        // A bracketed literal address keeps its brackets through the port.
        const size_t close = host.find(']');
        if (close == std::string::npos)
          return false;
        std::string tailHost = host.substr(0, close + 1);
        if (host.size() > close + 1u && host[close + 1u] == ':')
        {
          const std::string digits = host.substr(close + 2u);
          if (!digits.empty())
            tailHost += ":" + digits;
        }
        host = tailHost;
      }
      else
      {
        const size_t colon = host.rfind(':');
        if (colon != std::string::npos)
        {
          // The last colon splits a port from the name only when digits
          // follow it all the way down; a bare IPv6 address carries colons
          // with no port and keeps them whole.
          const std::string digits = host.substr(colon + 1u);
          bool allDigits = !digits.empty();
          for (char c : digits)
            if (c < '0' || c > '9')
              allDigits = false;
          if (allDigits)
            host = host.substr(0, colon);
        }
      }
      if (host.empty() || host.size() > 253u)
        return false;
      for (char c : host)
      {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                        (u >= '0' && u <= '9') || c == '.' || c == '-' || c == ':' ||
                        c == '[' || c == ']';
        if (!ok)
          return false;
      }
      std::string target = host;
      if (port != 443u)
        target += ":" + std::to_string(port);
      urlOut = "https://" + target + (pathAndQuery.empty() ? "/" : pathAndQuery);
      return true;
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
    // the header, the standard Authorization form, the cookie a door put on a
    // visiting browser when it accepted, and a ?key= in the address - the shape
    // for programs, and for a browser walking over from a machine next door:
    // a page that wakes with a key in its address takes it up at once and wipes
    // the address clean.
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
    // What changes only when a person changes it - a setting, a kept project's
    // name, the manuals - may still be held.
    int apiFreshSeconds(const std::string &path)
    {
      static const std::pair<const char *, int> FRESH[] = {
          {"/api/monitor", 2},
          {"/api/jobs", 2},
          {"/api/timings", 2},
          {"/api/system", 2},
          {"/api/network", 2},
          {"/api/network/scan", 0},
          // The network document is composed here rather than stored, and the
          // machine samples its counters every two seconds, so two seconds is
          // as long as one of its numbers is worth calling current; the scan
          // door is left out entirely because it does work on being asked, and
          // a forced scan may not be answered out of a browser's pocket.
          {"/api/service", 4},
          {"/api/config", 15},
          {"/api/models", 2},
          {"/api/gallery", 2},
          {"/api/refs", 2},
          {"/api/prefs", 20},
          // The studio's dropdown read and its change stamps are the two readings
          // a page asks for most often, and both are documents the module holds
          // ready: two seconds, like the monitor and the queue. The facts of one
          // project and the studio's own route table are read less than they are
          // held, so they stand on this function's own five-second default.
          {"/project/api/index", 2},
          {"/project/api/revs", 2},
          // A tree is asked again on every folder opened, and its answer is one
          // commit's listing - cheap to hold a moment. A file's bytes are the
          // opposite: content-addressed and immutable, they belong in the page's
          // own store by oid (§9.2); a door that says no-store keeps the fresh
          // reading off every cache that is not the page's own.
          {"/project/api/tree", 2},
          {"/project/api/file", -1},
          {"/project/api/file-outline", -1},
          // A record's own reading answers a detail pane that stands right
          // beside the hands that work it: an answer held past the press is
          // how a closed issue went on saying open under the page's own nose.
          // The list doors keep their short hold; what sits under a hand goes
          // fresh - the record's truth is worth one read.
          {"/project/api/pulls/", -1},
          {"/project/api/issues/", -1},
          // A build is followed by the page that started it: the status door
          // composes its answer from the job's records on every ask, so the
          // reading belongs fresh like the monitor and the queue - two seconds.
          // The start and the stop doors are left off the table the way the
          // scan is: each does work on being asked, and a second start out of
          // a browser's pocket would be a second job.
          {"/project/api/build-status", 2},
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

    // Writing for the port: the bytes go out as they are - through the
    // layer's encryption when the visitor came sealed, and the layer takes
    // the whole of a write or says it could not.
    bool sendAll(const ConnectionSink &sink, const std::string &data)
    {
      if (sink.tls)
        return sink.tls->write(data.data(), data.size());
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
    // body are read through this; a sealed visitor's bytes arrive already
    // opened, the chunked decoder above none the wiser.
    int readSome(const ConnectionSink &sink, std::string &out, size_t cap)
    {
      char buf[4096];
      const size_t want = cap < sizeof(buf) ? cap : sizeof(buf);
      const int got = sink.tls ? sink.tls->read(buf, static_cast<int>(want == 0 ? 1 : want))
                               : platform::receiveBytes(sink.sock, buf,
                                                        static_cast<int>(want == 0 ? 1 : want), 0);
      if (got > 0)
        out.append(buf, static_cast<size_t>(got));
      return got;
    }

    // Bytes asked for that nobody wants: read and dropped, nothing kept. The
    // drain stops at the connection's own deadline: a client that will not
    // finish feeding bytes it was refused does not get to hold the thread.
    bool drainTo(const ConnectionSink &sink, unsigned long long n,
                 const std::chrono::steady_clock::time_point &deadline)
    {
      while (n > 0)
      {
        if (std::chrono::steady_clock::now() >= deadline)
          return false;
        char buf[4096];
        const int ask = static_cast<int>(n < sizeof(buf) ? n : sizeof(buf));
        const int got =
            sink.tls ? sink.tls->read(buf, ask) : platform::receiveBytes(sink.sock, buf, ask, 0);
        if (got <= 0)
          return false;
        n -= static_cast<unsigned long long>(got);
      }
      return true;
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
    // The client is a connection's sink - a handle the doors pass along
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
    case 501:
      return "501 Not Implemented";
    case 502:
      return "502 Bad Gateway";
    case 503:
      return "503 Service Unavailable";
    default:
      // A code this table does not name still states its own number: rewriting
      // it to 500 would tell a reader something the door never said.
      return std::to_string(status) + " Error";
    }
  }

  OpenLocalServices::OpenLocalServices() = default;

  // Nothing answers out of these doors after this returns. The exception is a
  // connection deliberately left standing past the shutdown grace, which then
  // answers from the services and not from here - which is why the entry point
  // leaves those standing to the end of the process.
  OpenLocalServices::~OpenLocalServices()
  {
    stop();
  }

  std::string OpenLocalServices::bindHost() const
  {
    return m_bindHost.empty() ? std::string("0.0.0.0") : m_bindHost;
  }

  void OpenLocalServices::setImage(imagegen::ImageService *image) { m_image = image; }

  void OpenLocalServices::setProject(project::ProjectService *projects) { m_projects = projects; }

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
    const std::shared_ptr<const std::string> key = apiKey();
    if (key->empty())
      return true;
    return credentialsMatch(givenCredentials(req), *key);
  }

  // Three-state gate before the doors: 0 allow, 1 send the refusal, -1 send
  // nothing. The first kAuthSilentAfter refusals answer honestly; past that the
  // connection is swallowed until kAuthSilentResetMs pass without a new miss,
  // which reopens a door for one honest answer - and any accepted credential
  // lifts the count back, so a machine that is being used is never left silent.
  int OpenLocalServices::authGate(const HttpRequest &req)
  {
    const std::vector<std::string> given = givenCredentials(req);
    const std::shared_ptr<const std::string> key = apiKey();
    const bool asked = !key->empty();
    if (!asked || credentialsMatch(given, *key))
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

  // What the doors ask of at this moment: counts only. A letter of a key is
  // never answered here, or anywhere, so nothing a person typed can be read off
  // a machine's words. The pages read this before they carry a credential - it
  // is how a room learns what is asked of, and whether to ask this browser for
  // the key it does not yet carry.
  std::string OpenLocalServices::authInfoJson()
  {
    nlohmann::json out;
    out["api_required"] = !apiKey()->empty();
    // This server keeps a table of its fleet: pages of any age read this word
    // and only ask /api/peers where such a table stands, so a room never
    // rattles a door a younger machine does not have.
    out["peers"] = true;
    return out.dump();
  }

  // The door that answers a credential as it is handed - by a program, and by a
  // page whose person answered the prompt: the same decision every door takes -
  // one key, one answer - stated in words that never repeat what was handed, and
  // a key this machine takes is written on the connection so the rest of the
  // visit carries it.
  HttpResponse OpenLocalServices::certifyAnswer(const HttpRequest &req)
  {
    const std::string offered = configTrim(req.body);
    const std::shared_ptr<const std::string> key = apiKey();
    const bool asked = !key->empty();
    const bool taken = asked && keysMatch(offered, *key);
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
    return r;
  }

  bool OpenLocalServices::start(uint16_t port)
  {
    if (m_running.load())
      return true;
    // The pages are looked at once, here, and the answer stands for the life of
    // this process: whether a folder of them stands beside this server is decided
    // now, and nothing else looks at it while the server runs. A hand to a new
    // program asks again where the new program starts - which is why a page that
    // was written out behind a running server stands on its pages after the hand.
    {
      std::error_code pageEc{};
      const std::string pageDir = m_staticDir.empty() ? "webui" : m_staticDir;
      m_externalPages =
          std::filesystem::is_directory(std::filesystem::path(pageDir).lexically_normal(), pageEc);
    }
    if (!apiEnsureWsa())
      return false;
    // The sealed face of these doors, stood before the listener is: a start
    // asked to serve sealed only (OLS_TLS=on) and left without certificates
    // it cannot honour is a start refused, said in words - never a port that
    // quietly answers plain behind an https address. Asked auto, a machine
    // that cannot make certificates keeps serving in the clear, and says so.
    if (m_tlsMode == "on" || m_tlsMode == "auto")
    {
      bool madeFresh = false;
      std::string why;
      m_tlsMaterial = TlsMaterial::build(m_tlsCertDir, m_tlsMode == "auto", bindHost(), madeFresh,
                                         why);
      if (!m_tlsMaterial)
      {
        if (m_tlsMode == "on")
        {
          m_startRefusal = "TLS was asked for (OLS_TLS=on) and it cannot stand: " + why;
          return false;
        }
        std::cout << "[tls] no certificates and none could be made (" << why
                  << "): this port answers in the clear" << std::endl;
      }
      else
      {
        std::cout << (madeFresh ? "[tls] certificates made under "
                                : "[tls] certificates read from ")
                  << m_tlsCertDir << " (CA fingerprint sha256:" << m_tlsMaterial->caFingerprint()
                  << ", leaf valid to " << m_tlsMaterial->leafExpiry() << ")" << std::endl;
        std::cout << "[tls] this port answers "
                  << (m_tlsMode == "on" ? "sealed connections only"
                                        : "plain and sealed connections alike")
                  << std::endl;
      }
    }
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
    // The sweep belongs to this process: it stops, and the table it holds is
    // written down before anything else gives up its hold.
    m_peers.stop();
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
    // What the accept loop registered is joined here, within the grace it
    // states, rather than watched from a distance. Threads that are not in this
    // hand are the whole reason a detached connection used to be able to
    // outlive everything below.
    finishConnectionThreads();
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
      // Threads whose work is over leave the registry here, so what stop()
      // finds in it are the connections standing at that moment.
      reapFinishedConnections();
      // The place is taken before the machine is asked for a thread, so the
      // count is never one ahead of the truth and never holds a leak: over the
      // cap the client is closed at once with nothing said - the honest answer
      // of a server spending all the hands it means to spend.
      if (!reserveConnectionPlace())
      {
        platform::closeSocket(client);
        continue;
      }
      std::shared_ptr<ConnectionThread> slot;
      try
      {
        slot = std::make_shared<ConnectionThread>();
        slot->sock = client;
        {
          // Registered before the thread starts: a stop() landing between the
          // two still finds this connection and waits for it.
          std::lock_guard<std::mutex> lock(m_connMutex);
          m_connectionThreads.push_back(slot);
        }
        slot->worker = std::thread([this, slot]
                                   { connectionThread(slot); });
      }
      catch (const std::exception &exc)
      {
        // The machine refused a thread. The place goes back, the client is
        // closed with nothing said, and the loop stands: a CreateThread failure
        // under load is a busy machine, not a dead one - and it never reaches
        // the caller of this loop, which would end the server.
        if (slot)
        {
          std::lock_guard<std::mutex> lock(m_connMutex);
          const auto at = std::find(m_connectionThreads.begin(),
                                    m_connectionThreads.end(), slot);
          if (at != m_connectionThreads.end())
            m_connectionThreads.erase(at);
        }
        m_activeConnections.fetch_sub(1);
        platform::closeSocket(client);
        std::cerr << "[http] no thread to answer a client with: " << exc.what() << std::endl;
      }
      catch (...)
      {
        if (slot)
        {
          std::lock_guard<std::mutex> lock(m_connMutex);
          const auto at = std::find(m_connectionThreads.begin(),
                                    m_connectionThreads.end(), slot);
          if (at != m_connectionThreads.end())
            m_connectionThreads.erase(at);
        }
        m_activeConnections.fetch_sub(1);
        platform::closeSocket(client);
        std::cerr << "[http] no thread to answer a client with" << std::endl;
      }
    }
  }

  // One of the standing places, taken from the count of live connections: the
  // count is exact, so the cap is a cap and not a wish.
  bool OpenLocalServices::reserveConnectionPlace()
  {
    int held = m_activeConnections.load();
    while (held < kMaxLiveConnections)
    {
      if (m_activeConnections.compare_exchange_weak(held, held + 1))
        return true;
    }
    return false;
  }

  // A finished thread leaves the registry the way it left the wire: the thread
  // object is released first - detach of a thread whose work is over asks
  // nothing of it - and only then the hand holding its slot. Erasing a slot
  // before releasing its thread could leave the last reference in the hands of
  // the very thread it stands for.
  void OpenLocalServices::reapFinishedConnections()
  {
    std::lock_guard<std::mutex> lock(m_connMutex);
    for (const std::shared_ptr<ConnectionThread> &slot : m_connectionThreads)
    {
      if (slot->finished.load() && slot->worker.joinable())
        slot->worker.detach();
    }
    m_connectionThreads.erase(
        std::remove_if(m_connectionThreads.begin(), m_connectionThreads.end(),
                       [](const std::shared_ptr<ConnectionThread> &slot)
                       {
                         return slot->finished.load();
                       }),
        m_connectionThreads.end());
  }

  // The life of one connection thread. The sink inside handleConnection owns
  // the socket, so a throw past its boundary closes the client just as an
  // answer does; the word that this thread is over is written after the socket
  // is closed, and the place goes back with it.
  void OpenLocalServices::connectionThread(const std::shared_ptr<ConnectionThread> &slot)
  {
    struct Done
    {
      std::atomic<int> &count;
      std::atomic<bool> &finished;
      ~Done()
      {
        finished.store(true);
        count.fetch_sub(1);
      }
    } done{m_activeConnections, slot->finished};
    try
    {
      handleConnection(slot->sock);
    }
    catch (const std::exception &exc)
    {
      std::cerr << "[http] connection thread threw: " << exc.what() << std::endl;
    }
    catch (...)
    {
      std::cerr << "[http] connection thread threw something else" << std::endl;
    }
  }

  // stop()'s last word, and the bound it promises: everything the accept loop
  // registered is joined, each one as its own answer ends, up to the grace.
  // What still stands past the grace is left standing on purpose - it answers
  // from the services, and the entry point leaves those to the process's end,
  // so there is nothing under its feet for it to fall through.
  void OpenLocalServices::finishConnectionThreads()
  {
    const std::chrono::steady_clock::time_point until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kStopGraceMs);
    for (;;)
    {
      std::vector<std::shared_ptr<ConnectionThread>> handed;
      {
        std::lock_guard<std::mutex> lock(m_connMutex);
        // The accept loop is already joined, so nothing registers behind this.
        handed.swap(m_connectionThreads);
      }
      std::vector<std::shared_ptr<ConnectionThread>> stillStanding;
      for (const std::shared_ptr<ConnectionThread> &slot : handed)
      {
        if (!slot->worker.joinable())
          continue; // already joined, or already left standing
        if (slot->finished.load())
          slot->worker.join();
        else if (std::chrono::steady_clock::now() < until)
          stillStanding.push_back(slot);
        else
        {
          std::cerr << "[http] a connection outlived the shutdown grace: left standing on purpose"
                    << std::endl;
          slot->worker.detach();
        }
      }
      if (stillStanding.empty())
        break;
      {
        std::lock_guard<std::mutex> lock(m_connMutex);
        m_connectionThreads = std::move(stillStanding);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
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

  // One request's work for upload-pack. A gitWriter lambda cannot capture, so
  // the job rides on a thread-local set by routeGitSmart just before it
  // returns and taken by the writer when the transport starts writing; one
  // connection runs per thread. The thread-local owns it: the writer takes it
  // out and holds it by its own unique_ptr, so a throw inside the writer ends
  // the job with the throw, and a connection that never reaches its writer
  // leaves it to this hand - which frees it when the thread ends, the way the
  // take-back beside the writer's call does a little earlier.
  namespace
  {
    struct UploadJob
    {
      std::string repoDir;
      std::string request; // the v2 command bytes, exactly as they arrived
    };
    thread_local std::unique_ptr<UploadJob> gGitUploadJob;

    // A git body's temp file belongs to the connection that streamed it: the
    // file goes when that connection's own scope ends, whichever road the
    // answer took - the writer, a refusal, or a throw.
    struct TempBodyCleanup
    {
      const std::string &path;
      ~TempBodyCleanup()
      {
        if (path.empty())
          return;
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
      }
    };
  } // namespace

  ConnectionSink::ConnectionSink(platform::sock_t s) : sock(s)
  {
  }

  ConnectionSink::~ConnectionSink()
  {
    // A sealed conversation ends with its word (close_notify) before the
    // socket lets go; a plain one ends exactly as it always did.
    if (tls)
      tls->close();
    if (sock != platform::kBadSocket)
      platform::closeSocket(sock);
  }

  void OpenLocalServices::handleConnection(platform::sock_t sock)
  {
    // From this line the sink owns the socket: every way out of here - an
    // answer, a refusal, a throw - closes it, and closes it once.
    ConnectionSink sink{sock};

    std::string clientIp;
    platform::peerAddress(sock, clientIp);
    platform::setReadTimeout(sock, 10000);

    // One port, two kinds of visitor: the first bytes, peeked and left
    // unread, say which. A sealed visitor gets its layer, and the handshake
    // ends on the layer's own ten seconds so a client that connects and
    // stalls cannot hold this thread past what the loop expects of a plain
    // one. A plain visitor reaches today's path untouched; with OLS_TLS=on
    // it is answered nothing - the redirect door belongs to the next batch -
    // and with OLS_TLS off or absent this peek never happens at all.
    if (m_tlsMaterial)
    {
      if (TlsLayer::looksLikeTls(sock))
      {
        sink.tls.reset(new TlsLayer(sock, *m_tlsMaterial));
        std::string why;
        if (!sink.tls->handshake(why))
        {
          std::cerr << "[tls] a sealed visitor left before it could be served: " << why << " ("
                    << clientIp << ")" << std::endl;
          return;
        }
      }
      else if (m_tlsMode == "on")
      {
        // Sealed answers only. A plain visitor is refused the way it can be
        // acted on: with one https answer, pointed at the sealed shape of
        // its own address - but only once its first line has been read whole
        // and weighed (sealedRedirectOk). Until that line stands, no byte
        // crosses: a probe of two bytes never learns what lives behind this
        // door. Everything is read with MSG_PEEK, so an ask that decides to
        // stay plain still finds every one of its own bytes waiting for the
        // road below it. The Host header names where a redirect points;
        // without it there is no honest address to point at.
        std::string firstBlock;
        bool blockFull = false;
        for (int guard = 0; guard < 64; ++guard)
        {
          char buf[2048];
          const int got = platform::receiveBytes(sock, buf, static_cast<int>(sizeof(buf)), MSG_PEEK);
          if (got <= 0)
            break;
          firstBlock.assign(buf, static_cast<size_t>(got)); // peek sees from the front every time
          if (static_cast<size_t>(got) < sizeof(buf))
          {
            blockFull = true; // the stream has nothing more for now
            break;
          }
          if (blockHasHeaderEnd(firstBlock))
          {
            blockFull = true;
            break;
          }
        }
        const size_t lineEnd = firstBlock.find("\r\n");
        if (lineEnd != std::string::npos)
        {
          HttpRequest probe;
          parseRequestLine(firstBlock.substr(0, lineEnd), probe);
          std::string url;
          // Only a block read whole answers yes or no - a block cut at the
          // cap never redirects, because its Host may still stand unread.
          if (blockFull && sealedRedirectOk(probe))
          {
            std::string hostHeader;
            readHostHeader(firstBlock.substr(0, firstBlock.find("\r\n\r\n")), hostHeader);
            if (!hostHeader.empty())
              sealedRedirectTarget(hostHeader, m_port.load(),
                                   probe.path + (probe.query.empty() ? "" : "?" + probe.query),
                                   url);
          }
          if (!url.empty())
          {
            const std::string body = "This address answers sealed only - open it over https.";
            const std::string answer = "HTTP/1.1 301 Moved Permanently\r\n"
                                       "Location: " + url + "\r\n" +
                                       "Content-Type: text/plain; charset=utf-8\r\n"
                                       "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                       "Connection: close\r\n\r\n" +
                                       body;
            platform::sendBytes(sock, answer.data(), static_cast<int>(answer.size()), 0);
          }
        }
        return;
      }
    }

    // The connection's own deadline, taken once: every read that waits on this
    // client looks at it, so a client that drips bytes cannot hold a thread for
    // hours. `timedOut` says which clock ended a read, so the answer is a
    // timeout's and not a malformed request's.
    const std::chrono::steady_clock::time_point connDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectionOverallMs);
    const auto outOfTime = [connDeadline]
    {
      return std::chrono::steady_clock::now() >= connDeadline;
    };
    bool timedOut = false;

    // The connection's first bytes are read whole before any answer: none of
    // them are thrown away, and a request that starts mid-block still finds
    // its line once the rest is read. A sealed visitor's first bytes arrive
    // already opened through the layer it stands behind.
    std::string raw;
    if (readSome(sink, raw, 4096) <= 0)
      return;

    size_t headerEnd = raw.find("\r\n\r\n");
    constexpr size_t kMaxHeaderBytes = 1u << 20u;
    while (headerEnd == std::string::npos)
    {
      if (outOfTime())
      {
        timedOut = true;
        break;
      }
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
      resp = timedOut ? jsonError(408, "Request timed out")
                      : jsonError(400, "Malformed request");
    }
    else
    {
      const std::string headerBlock = raw.substr(0, headerEnd);
      const size_t bodyStart = headerEnd + 4;
      constexpr size_t kMaxBodyBytes = 2u * 1024u * 1024u;
      // What the framing itself refuses, in words, before any of the body is
      // asked for. A request the transport cannot frame is answered where it
      // stands - not routed on a guess at what it meant.
      unsigned long long declaredLength = 0;
      bool lengthStated = false; // a Content-Length stood at all
      std::string framingWords;
      {
        std::istringstream hs(headerBlock);
        std::string requestLine;
        std::getline(hs, requestLine);
        std::istringstream rl(requestLine);
        std::string target;
        std::string httpVersion;
        rl >> req.method >> target >> httpVersion;
        // A line without a method, a target, or a version token that names
        // HTTP is not a request this transport can route: a target read as
        // "/" would answer the wrong door, and a missing version is a client
        // this machine cannot speak with.
        if (req.method.empty() || target.empty() || httpVersion.compare(0, 5, "HTTP/") != 0)
          framingWords = "the request line carries no method, no target, or no HTTP version";
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
            // The length a request states is either a whole number of bytes
            // or the framing does not stand: "abc" is not zero bytes, and an
            // ask stating two different lengths says two different things.
            // Two states of one number are one statement; no length stated
            // at all stays legal, the way a GET and a bodiless POST always
            // have been.
            std::string number = value;
            while (!number.empty() && (number.back() == ' ' || number.back() == '\t'))
              number.pop_back();
            unsigned long long parsed = 0;
            bool whole = !number.empty();
            try
            {
              size_t read = 0;
              parsed = std::stoull(number, &read);
              if (read != number.size())
                whole = false; // digits with other things beside them
            }
            catch (const std::exception &)
            {
              whole = false; // nothing this side of a number, or past one
            }
            if (!whole)
              framingWords = "the length this ask states is not a whole number of bytes";
            else if (lengthStated && parsed != declaredLength)
              framingWords = "the ask states two different lengths for one body";
            else if (!lengthStated)
            {
              declaredLength = parsed;
              lengthStated = true;
            }
          }
          else
          {
            req.headers[key] = value;
          }
        }
      }

      // The body's cap follows the door: git smart-HTTP carries pack streams no
      // json door would dream of, so it answers to its own stated limit; every
      // other door keeps kMaxBodyBytes exactly as it was.
      const bool gitDoor = isGitDoorPath(req.path);
      const unsigned long long bodyCap = gitDoor ? gitBodyCap() : kMaxBodyBytes;

      // A git smart-HTTP client frames its POST bodies chunked rather than
      // counting them, so the transport decodes those frames into the git
      // temp file below and hands the route the decoded size. The road stays
      // on the git doors: every other door keeps a content-length body
      // exactly as it was - and an ask that frames itself chunked at such a
      // door is refused rather than read as though it had counted its bytes.
      const bool chunkedRequest = gitDoor && [&]
      {
        const auto te = req.headers.find("transfer-encoding");
        if (te == req.headers.end())
          return false;
        return lowerText(te->second).find("chunked") != std::string::npos;
      }();
      if (framingWords.empty() && !gitDoor &&
          req.headers.find("transfer-encoding") != req.headers.end())
        framingWords = "this door does not take chunked bodies";

      // A client that sends Expect: 100-continue holds its body back until the
      // server answers, so a reader that routed on empty headers would refuse a
      // request it never gave the client the chance to finish. The interim
      // answer goes out before any body read — git doors' temp-file streaming
      // included — and routing still waits for the body. An Expect naming any
      // other token is ignored rather than answered 417: RFC 7231 makes that
      // answer optional, and a client sending an expectation this server does
      // not know does not want its request turned away for it.
      const bool wantsContinue = [&]
      {
        const auto ex = req.headers.find("expect");
        if (ex == req.headers.end())
          return false;
        std::string v = lowerText(ex->second);
        for (char &c : v)
          if (c == '\t')
            c = ' ';
        while (v.find("  ") != std::string::npos)
          v.erase(v.find("  "), 1);
        const char *needle = "100-continue";
        for (size_t at = v.find(needle); at != std::string::npos;
             at = v.find(needle, at + strlen(needle)))
        {
          const bool leftOk = at == 0 || !isalnum(static_cast<unsigned char>(v[at - 1]));
          const size_t after = at + strlen(needle);
          const bool rightOk = after >= v.size() ||
                               !isalnum(static_cast<unsigned char>(v[after]));
          if (leftOk && rightOk)
            return true;
        }
        return false;
      }();
      // A framing the transport cannot stand behind answers before anything
      // else - and before the interim invitation to send a body it never got
      // the framing to carry: the bytes still in the client's hand are not
      // what stands in the way.
      if (framingWords.empty() && wantsContinue)
        sendAll(sink, "HTTP/1.1 100 Continue\r\n\r\n");

      if (!framingWords.empty())
      {
        resp = jsonError(400, framingWords);
      }
      else if (!chunkedRequest && declaredLength > bodyCap)
      {
        // The body is refused but still drained to its declared end, so the
        // answer is not mistaken for a dropped connection. Nothing is buffered:
        // what arrives is discarded as it comes.
        const unsigned long long drained = raw.size() - bodyStart;
        if (drained < declaredLength)
          drainTo(sink, declaredLength - drained, connDeadline);
        resp = jsonError(413, "Request body too large");
      }
      else
      {
        // A git door's body goes to a temp file rather than into req.body: the
        // route reads it through that path and its size, so a push of hundreds
        // of megabytes never stands in memory. The file is this connection's
        // own - created here, and removed when this scope ends, however the
        // answer leaves it.
        std::string gitBodyFile;
        const TempBodyCleanup removeBodyFile{gitBodyFile};
        unsigned long long keptBytes = declaredLength;
        bool bodyTooLarge = false;
        std::string streamRefusal; // why a refused body stream says so
        if (gitDoor && (declaredLength > 0 || chunkedRequest))
        {
          std::error_code fec;
          // Unpredictable and exclusive: a 128-bit hex tail drawn from a
          // seeded random_device — mixed with the clock's moment, because
          // this compiler's random_device repeats between runs — and a name
          // already standing on disk is asked for again, never truncated
          // over: two connections never share one body file.
          auto hex16 = [](std::uint64_t v)
          {
            char digits[17];
            for (int at = 15; at >= 0; --at)
            {
              digits[at] = "0123456789abcdef"[v & 0xFull];
              v >>= 4;
            }
            digits[16] = '\0';
            return std::string(digits);
          };
          std::mt19937_64 draw([]
                               {
            std::random_device rd;
            std::uint64_t seed = rd();
            seed ^= static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            return seed; }());
          for (int pick = 0; pick < 8 && gitBodyFile.empty(); ++pick)
          {
            const std::string candidate =
                (std::filesystem::temp_directory_path(fec) /
                 ("ols-git-body-" + hex16(draw()) + hex16(draw()) + ".bin"))
                    .generic_string();
            std::error_code seen;
            if (!std::filesystem::exists(candidate, seen))
              gitBodyFile = candidate;
          }
          if (gitBodyFile.empty())
          {
            // eight names in a row standing is never two live connections;
            // if it ever happens, this is the honest last try
            gitBodyFile = (std::filesystem::temp_directory_path(fec) /
                           ("ols-git-body-" + std::to_string(draw()) + ".bin"))
                              .generic_string();
          }
          std::ofstream out(gitBodyFile, std::ios::binary | std::ios::trunc);
          unsigned long long written = 0;
          if (chunkedRequest)
          {
            // The frames of a chunked body: a hex size line, that many
            // payload bytes appended to this body's file, then the next
            // frame; a zero frame ends the body after its trailers.
            std::string buf = raw.substr(bodyStart);
            std::size_t at = 0;
            auto more = [&]
            {
              if (outOfTime())
              {
                timedOut = true;
                return false;
              }
              std::string extra;
              if (readSome(sink, extra, 65536) <= 0)
                return false;
              buf.append(extra);
              return true;
            };
            bool framed = true;
            while (framed)
            {
              std::size_t nl = std::string::npos;
              while (true)
              {
                nl = buf.find("\r\n", at);
                if (nl != std::string::npos)
                  break;
                if (!more())
                {
                  framed = false;
                  break;
                }
              }
              if (!framed)
                break;
              std::string token = buf.substr(at, nl - at);
              at = nl + 2;
              const std::size_t semi = token.find(';'); // a chunk extension is read and set aside
              if (semi != std::string::npos)
                token.resize(semi);
              unsigned long long size = 0;
              bool digits = !token.empty();
              for (char c : token)
              {
                unsigned nibble = 0;
                if (c >= '0' && c <= '9')
                  nibble = static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f')
                  nibble = static_cast<unsigned>(c - 'a') + 10u;
                else if (c >= 'A' && c <= 'F')
                  nibble = static_cast<unsigned>(c - 'A') + 10u;
                else
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
                framed = false;
                break;
              }
              if (size == 0ull)
              {
                // The end: trailer lines each keep their own line, and the
                // blank one after the last chunk closes the body.
                bool closed = false;
                while (!closed)
                {
                  const std::size_t tl = buf.find("\r\n", at);
                  if (tl == std::string::npos)
                  {
                    if (!more())
                      closed = true; // the stream ends at its terminator
                    continue;
                  }
                  if (tl == at)
                  {
                    at = tl + 2;
                    closed = true;
                    break;
                  }
                  at = tl + 2; // one trailer line, read and left aside
                }
                break;
              }
              if (written + size > bodyCap)
              {
                bodyTooLarge = true; // a chunked stream has no declared end to drain: the answer states the cap where it stands
                break;
              }
              // The chunk travels to this body's file as it arrives: the buffer
              // holds one read's bytes, never the whole frame, however the
              // sender cut it. The cap above stands exactly as it was - nothing
              // of a chunk over it is written - and the frame's end is still
              // checked byte for byte before the next frame is read.
              buf.erase(0, at);
              at = 0ull;
              unsigned long long left = size;
              while (left > 0ull)
              {
                if (buf.empty())
                {
                  if (!more())
                    break;
                  continue;
                }
                const std::size_t take =
                    buf.size() < static_cast<std::size_t>(left) ? buf.size()
                                                                : static_cast<std::size_t>(left);
                out.write(buf.data(), static_cast<std::streamsize>(take));
                written += static_cast<unsigned long long>(take);
                left -= static_cast<unsigned long long>(take);
                buf.erase(0, take);
              }
              if (left > 0ull)
              {
                framed = false; // the stream ended inside a chunk
                break;
              }
              while (buf.size() < 2ull)
              {
                if (!more())
                  break;
              }
              if (buf.size() < 2ull || buf.compare(0, 2, "\r\n") != 0)
              {
                framed = false;
                break;
              }
              buf.erase(0, 2ull);
            }
            out.close();
            if (!out || (!framed && !timedOut))
              streamRefusal = "The chunked body did not frame as chunked";
            keptBytes = written;
          }
          else
          {
            if (out && declaredLength > raw.size() - bodyStart)
            {
              // The bytes already read go in first; the rest arrives through the
              // same reader every other body uses.
              out.write(raw.data() + bodyStart,
                        static_cast<std::streamsize>(raw.size() - bodyStart));
              written = raw.size() - bodyStart;
              while (written < declaredLength)
              {
                if (outOfTime())
                {
                  timedOut = true;
                  break;
                }
                std::string more;
                const int got = readSome(sink, more, 65536);
                if (got <= 0)
                  break;
                out.write(more.data(), static_cast<std::streamsize>(more.size()));
                written += static_cast<unsigned long long>(more.size());
              }
            }
            else if (out)
            {
              out.write(raw.data() + bodyStart,
                        static_cast<std::streamsize>(declaredLength));
              written = declaredLength; // the body stood whole in the first read
            }
            out.close();
            if (!out || written < declaredLength)
              streamRefusal = "The request body ended before its length";
            keptBytes = written;
          }
        }

        const size_t contentLength = gitDoor ? 0u : static_cast<size_t>(declaredLength);
        if (!gitDoor)
        {
          while (raw.size() - bodyStart < contentLength)
          {
            if (outOfTime())
            {
              timedOut = true;
              break;
            }
            if (readSome(sink, raw, 4096) <= 0)
              break;
          }
          req.body = raw.substr(bodyStart, contentLength);
        }
        // The route sees a git door's body through the published path and size,
        // never in memory — so the pair is published before route() is called,
        // and a body stream that did not hold refuses before the door's work
        // begins.
        bool bodyRefused = false;
        if (timedOut)
        {
          // A body the client would not finish inside the connection's time is
          // refused where it stands: nothing is routed on half a body.
          resp = jsonError(408, "Request timed out");
          bodyRefused = true;
        }
        else if (gitDoor)
        {
          if (bodyTooLarge)
          {
            resp = jsonError(413, "Request body too large");
            bodyRefused = true;
          }
          else if (!streamRefusal.empty())
          {
            resp = jsonError(400, streamRefusal);
            bodyRefused = true;
          }
          else if (!gitBodyFile.empty())
          {
            req.headers["x-ols-body-file"] = gitBodyFile;
            req.headers["x-ols-body-size"] = std::to_string(keptBytes);
          }
        }
        try
        {
          if (isEventStreamPath(req.path) && req.method == "GET")
          {
            const int auth = authGate(req);
            if (auth == -1)
              return;
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
              // The head says what the transport actually does when this stream
              // ends: the connection closes, so it is not promised to anybody.
              const std::string head =
                  "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                  "Cache-Control: no-store\r\nTransfer-Encoding: chunked\r\n"
                  "Connection: close\r\n\r\n";
              if (sendAll(sink, head))
              {
                try
                {
                  streamToClient(&sink, req.path);
                  writeStreamChunk(&sink, "event: end\n\n");
                }
                catch (const std::exception &)
                {
                  // The reason travels as the machine's own words, in the shape
                  // every other frame of this stream is written in: a page that
                  // reads a stream does not have to guess why it stopped.
                  writeStreamChunk(&sink,
                                   "event: error\ndata: " + nlohmann::json{{"type", "error"}, {"message", "stream failed"}}.dump() + "\n\n");
                }
                sendAll(sink, "0\r\n\r\n");
              }
              return;
            }
          }
          else if (!bodyRefused)
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
              // A door listed with -1 keeps the no-store it came with: bytes a
              // page may hold forever by their own name are not bytes to hand
              // to a cache keyed on an address.
              const int fresh = apiFreshSeconds(req.path);
              resp.headers["Cache-Control"] =
                  fresh < 0 ? "no-store" : "public, max-age=" + std::to_string(fresh);
            }
            // A sealed conversation says so for a year after this one ends
            // (spec §10.3): a browser that came here once over https keeps
            // coming back sealed even if the address is typed plain again.
            // Only the connections that truly rode TLS wear it - a plain
            // answer on an auto port never speaks of a seal it did not keep.
            if (sink.tls)
              resp.headers["Strict-Transport-Security"] = "max-age=31536000";
          }
        }
        catch (const nlohmann::json::exception &exc)
        {
          std::cerr << "[http] " << req.path << " read the body as a stranger: " << exc.what() << std::endl;
          resp = jsonError(400, "Invalid request body");
        }
        catch (const std::exception &exc)
        {
          // The answer keeps its words short, but the server's own line names
          // what threw, so a door that fails on another machine can be read
          // where it stands instead of guessed at from a bare 500.
          std::cerr << "[http] " << req.path << " threw: " << exc.what() << std::endl;
          resp = jsonError(500, "Internal error");
        }

        if (gitDoor && resp.gitWriter != nullptr)
        {
          // A git door answers itself over this connection: the route hands back
          // a writer and the bytes go out as they are produced, so no answer of
          // hundreds of megabytes ever stands here whole. The sink closes this
          // connection as this scope ends - the writer throwing included - and
          // the job the writer took is ended by its own hand, not by a line
          // after the call that a throw skips.
          resp.gitWriter(&sink, req);
          return;
        }

        // A job the route set but whose writer this connection never reached is
        // given up here, exactly where the writer would have owned it. The
        // thread-local holds it by unique_ptr, so even a throw between the route
        // and this line loses nothing: the thread's own end frees it.
        gGitUploadJob.reset();
      }
    }

    if (resp.status == 0)
    {
      // Lockout: no response at all goes back over the wire.
      return;
    }

    if (req.method == "HEAD")
    {
      // A HEAD asks for the answer a GET would give, without its body: the
      // length a reader would find there is the point of asking.
      resp.body.clear();
    }

    // A door that handed over chunk writing answers through them: the head says
    // chunked, each call produces one frame's bytes, and an empty one ends it.
    // No door hands one over today - the streams this server speaks frame their
    // own bytes - so this road stands unused, not dead code to delete.
    if (resp.chunkSource != nullptr)
    {
      std::string out = "HTTP/1.1 " + statusLine(resp.status) + "\r\n";
      for (const auto &kv : resp.headers)
        out += kv.first + ": " + kv.second + "\r\n";
      out += "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
      if (sendAll(sink, out))
      {
        std::string chunk;
        while (resp.chunkSource(resp.chunkHandle, chunk) && !chunk.empty())
        {
          if (!writeStreamChunk(&sink, chunk))
            break;
          chunk.clear();
        }
        sendAll(sink, "0\r\n\r\n");
      }
      return;
    }

    std::string out = "HTTP/1.1 " + statusLine(resp.status) + "\r\n";
    for (const auto &kv : resp.headers)
      out += kv.first + ": " + kv.second + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += resp.body;
    sendAll(sink, out);
  }

  // §9.3 live invalidation: the studio's event stream. The list is process-
  // wide and carries one mailbox per open /project/api/events connection. A
  // mutation's stamp is recorded beside the bump and published after the
  // record-doors lock is out of the way - bumpStamp records on the
  // connection thread, route() publishes on that same thread once the door's
  // answer is done - so no publish ever runs inside the record-doors lock,
  // and the door beside the stream never holds it at all. The mailbox lock
  // is its own and no socket is written under it, so the two locks never
  // cross.
  static std::mutex gProjectEventsMutex;
  static std::condition_variable gProjectEventsSeen;
  namespace {
    struct ProjectEventMailbox
    {
      std::deque<std::string> frames;
    };
    std::vector<std::shared_ptr<ProjectEventMailbox>> gProjectEventMailboxes;
    // A stamp moved by this connection's door, waiting for the door's own
    // lock to leave: one thread's pending frames, never another's.
    thread_local std::vector<std::string> gPendingProjectEventFrames;
  } // anonymous namespace

  // Hand the stamps this connection just moved to every open stream. Called
  // only where the record-doors lock has already left (the route, after the
  // door's answer); the copy is a few bytes per mailbox, so the lock holds
  // nothing slow.
  static void publishPendingProjectEvents()
  {
    if (gPendingProjectEventFrames.empty())
      return;
    std::lock_guard<std::mutex> hold(gProjectEventsMutex);
    for (const auto &frame : gPendingProjectEventFrames)
      for (auto &mailbox : gProjectEventMailboxes)
        mailbox->frames.push_back(frame);
    gPendingProjectEventFrames.clear();
    gProjectEventsSeen.notify_all();
  }

  // The studio's stream (§9.3): frames arrive from the process-wide publisher
  // into this connection's mailbox; the door writes them as they come and
  // holds the connection until the client drops its end or the house cap of
  // five minutes closes it - the caller beside it writes the end frame and
  // the connection goes. The mailbox is taken off the list when this scope
  // ends, whichever way it ends, so a stream that threw stops being written
  // to. The record-doors lock is never held here: the mailbox lock is its
  // own, and frames are written with it open.
  static void streamProjectEvents(void *clientHandle)
  {
    auto mailbox = std::make_shared<ProjectEventMailbox>();
    {
      std::lock_guard<std::mutex> hold(gProjectEventsMutex);
      gProjectEventMailboxes.push_back(mailbox);
    }
    struct Gone
    {
      std::shared_ptr<ProjectEventMailbox> mailbox;
      ~Gone()
      {
        std::lock_guard<std::mutex> hold(gProjectEventsMutex);
        const auto at =
            std::find(gProjectEventMailboxes.begin(), gProjectEventMailboxes.end(), mailbox);
        if (at != gProjectEventMailboxes.end())
          gProjectEventMailboxes.erase(at);
      }
    } gone{mailbox};
    const auto close = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (true)
    {
      while (true)
      {
        std::string frame;
        {
          std::lock_guard<std::mutex> hold(gProjectEventsMutex);
          if (mailbox->frames.empty())
            break;
          frame = std::move(mailbox->frames.front());
          mailbox->frames.pop_front();
        }
        if (!OpenLocalServices::writeStreamChunk(clientHandle, frame))
          return; // the client dropped its end of the pipe
      }
      if (std::chrono::steady_clock::now() >= close)
        return;
      std::unique_lock<std::mutex> hold(gProjectEventsMutex);
      gProjectEventsSeen.wait_for(hold, std::chrono::seconds(1));
    }
  }

  void OpenLocalServices::streamToClient(void *clientHandle, const std::string &path)
  {
    if (path == "/project/api/events")
    {
      streamProjectEvents(clientHandle);
      return;
    }
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

  // The moment a file was last written, as whole seconds since 1970. C++17's
  // file clock keeps no promise about where its count starts: Windows counts
  // hundred-nanosecond steps from 1601 and Linux counts from 1970, so the
  // crossing goes platform by platform rather than being trusted to the clock.
  static long long fileMomentSeconds(const std::filesystem::file_time_type when)
  {
#if defined(_MSC_VER)
    constexpr long long kFrom1601To1970 = 11644473600LL; // 369 years of seconds
    return std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count() -
           kFrom1601To1970;
#else
    return std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count();
#endif
  }

  // Which page file the pages this server answers with were last written at,
  // and its name. Those files may be edited under a standing server - the
  // folder outside answers every request as it stands - so the question is
  // re-answered when asked, never more than once a minute: walking a folder of
  // page files is nothing once a minute and not nothing on every request. The
  // name comes back beside the moment so a page can say what it counts from.
  struct NewestPage
  {
    long long at = 0;
    std::string name;
  };
  static NewestPage newestPageFile(const std::string &dirName)
  {
    static std::mutex gate;
    static NewestPage kept;
    static long long keptAt = 0;
    std::lock_guard<std::mutex> lock(gate);
    const long long now = nowSeconds();
    if (keptAt != 0 && now - keptAt < 60)
      return kept;
    NewestPage fresh;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator walk(dirName,
                                                       std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec)
    {
      kept = fresh;
      keptAt = now;
      return kept;
    }
    for (const auto &entry : walk)
    {
      std::error_code one;
      if (!entry.is_regular_file(one) || one)
        continue;
      std::string leaf = entry.path().filename().generic_string();
      std::transform(leaf.begin(), leaf.end(), leaf.begin(),
                     [](unsigned char c)
                     { return static_cast<char>(std::tolower(c)); });
      const std::string ext = std::filesystem::path(leaf).extension().generic_string();
      if (ext != ".html" && ext != ".js" && ext != ".css")
        continue;
      const auto when = entry.last_write_time(one);
      if (one)
        continue;
      const long long at = fileMomentSeconds(when);
      if (at > fresh.at)
      {
        fresh.at = at;
        fresh.name = leaf;
      }
    }
    kept = fresh;
    keptAt = now;
    return kept;
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
      // progress bar's reach, a zoom's size - so the attribute position is
      // opened. The attribute's own spelling below is the narrow statement of
      // that intent, for the readers that know it; the same 'unsafe-inline' is
      // put on style-src as well because a reader that does not know that
      // directive falls back to style-src, and the browser that reads this
      // header here is such a reader - asked without it, it refuses the styles
      // the pages stand their parts at, and says so in its console. data:
      // images are allowed because the webui draws kept files as data URLs. No
      // page plays media of its own making or puts a frame of another origin on
      // it now, so no page asks for media-src or frame-src.
      std::string csp =
          "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; style-src-attr "
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
                                            const HttpRequest &req) const
  {
    std::string body;
    std::string contentType;
    {
      std::string path;
      const bool known = m_image && m_image->file(kind, name, path, contentType);
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

  // The ggml the image engine links into this binary is the machine's device
  // probe: the backend this build was asked for is compiled into it, so the
  // probe only enumerates what this binary already carries.
  // Plugin ggml libraries must never be read from beside the server: measured
  // on the 2026-10-28 release, a plugin dll the loader picked up registered the
  // same card a second time beside the ggml inside the process, and the process
  // failed (0xC0000409) as a finished picture was handed back, with one copy's
  // tensor standing in the other's hands. The probe is still asked for here,
  // before the port is taken, so the first page read states the device instead
  // of paying for finding it.
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

  namespace
  {
    // The words a browser tab carries so three open tabs say which machine
    // stands behind each: the name the machine goes by on the network, the
    // operating system it says it runs, and the device this binary was built
    // to drive. Each states only what the machine itself answers; where it
    // answers nothing, the page keeps the plain program name instead.
    std::string selfHostName()
    {
      std::string host;
#ifdef _WIN32
      char text[256] = {0};
      DWORD length = static_cast<DWORD>(sizeof(text));
      if (GetComputerNameA(text, &length))
        host = text;
#else
      char text[256] = {0};
      if (gethostname(text, sizeof(text) - 1) == 0)
        host = text;
#endif
      // Written in capitals whichever platform it came from, so the tab reads
      // the machine's name as a name - UM780 beside X1 - and not whatever
      // letter case the box happened to be named under.
      for (char &c : host)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      return host;
    }

    std::string selfOsName()
    {
#ifdef _WIN32
      // The kernel's own answer, asked of ntdll directly: the version api a
      // manifest-less process is handed otherwise lies about being ten.
      using RtlGetVersion_t = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
      const HMODULE ntdll = GetModuleHandleA("ntdll.dll");
      auto rtl = ntdll ? reinterpret_cast<RtlGetVersion_t>(
                             GetProcAddress(ntdll, "RtlGetVersion"))
                       : nullptr;
      OSVERSIONINFOW info = {};
      info.dwOSVersionInfoSize = sizeof(info);
      if (!rtl || rtl(&info) != 0)
        return std::string();
      if (info.dwMajorVersion < 10)
        return "win" + std::to_string(info.dwMajorVersion);
      return info.dwBuildNumber >= 22000 ? "win11" : "win10";
#else
      // The distribution's own record: its name and version, told apart from
      // a desktop by the session managers a desktop would have installed.
      std::ifstream record("/etc/os-release");
      std::string id, version, line;
      while (std::getline(record, line))
      {
        for (const char *key : {"ID=", "VERSION_ID="})
        {
          std::string &into = key[1] == 'D' ? id : version;
          const std::string word(key);
          if (line.rfind(word, 0) != 0)
            continue;
          std::string value = line.substr(word.size());
          if (value.size() > 1 && value.front() == '"')
            value = value.substr(1, value.size() - 2);
          into = value;
        }
      }
      if (id.empty())
        return std::string();
      std::error_code ignored;
      const bool desktop = std::filesystem::exists("/usr/bin/gnome-session", ignored) ||
                           std::filesystem::exists("/usr/share/xsessions", ignored);
      std::string word = id + (desktop ? "" : "-server");
      if (!version.empty())
        word += "-" + version;
      return word;
#endif
    }

    std::string selfBackend()
    {
#ifndef OLS_BACKEND
      return std::string();
#else
      std::string word = OLS_BACKEND;
#ifdef OLS_CUDA_ARCH
      word += OLS_CUDA_ARCH;
#endif
#ifdef OLS_CPU_ISA
      word += "-" OLS_CPU_ISA;
#endif
      if (word == "vulkan")
      {
        // A vulkan build asks the machine's own loader the highest version its
        // devices can speak. A loader too old to be asked says nothing, and
        // the word stands alone rather than invented.
        uint32_t version = 0;
#ifdef _WIN32
        const HMODULE loader = LoadLibraryA("vulkan-1.dll");
        using Entry_t = uint32_t(__cdecl *)(uint32_t *);
        const Entry_t entry = loader ? reinterpret_cast<Entry_t>(
                                           GetProcAddress(loader, "vkEnumerateInstanceVersion"))
                                     : nullptr;
#else
        void *loader = dlopen("libvulkan.so.1", RTLD_LAZY);
        using Entry_t = uint32_t (*)(uint32_t *);
        const Entry_t entry = loader ? reinterpret_cast<Entry_t>(
                                           dlsym(loader, "vkEnumerateInstanceVersion"))
                                     : nullptr;
#endif
        if (entry && entry(&version) == 0 && version > 0)
          word += std::to_string(version >> 22) + "." + std::to_string((version >> 12) & 0x3FFu);
#ifndef _WIN32
        if (loader)
          dlclose(loader);
#endif
      }
      return word;
#endif
    }
  } // namespace

  // Fleet awareness: stand the peer table up beside these doors - the card
  // provider reads this machine as it stands, and the sweep of the networks
  // walks in the background once, after the doors already answer.
  void OpenLocalServices::startPeers(const std::string &apiKey, const std::string &dataDir)
  {
    m_peers.setSelf(apiKey, dataDir, m_port.load());
    const long long begunAt = steadyMs();
    m_peers.setCardProvider([this, begunAt]
                            {
      // The card is this machine as it stands at the moment of asking: its
      // name, the code it runs, the shape it was built in, and how long this
      // server has been standing. Nothing secret rides on it - the key is the
      // credential on the ask itself, never a word inside it.
      nlohmann::json card;
      card["name"] = selfHostName();
      card["port"] = m_port.load();
      const std::string at = platform::buildVersion();
      if (!at.empty())
        card["version"] = at;
      const std::string backend = selfBackend();
      if (!backend.empty())
        card["backend"] = backend;
      card["uptime_s"] = static_cast<long long>((steadyMs() - begunAt) / 1000);
      return card; });
    m_peers.load();
    m_peers.start();
  }

  std::string OpenLocalServices::systemJson() const
  {
    nlohmann::json devices = nlohmann::json::array();
    std::vector<std::string> names;
    // The ggml the image engine links into this binary knows the machine's
    // devices: it is asked first, so the gpu and its memory are stated with the
    // numbers the backends themselves see. What it cannot name, the image
    // service may still list by name, and a device no library names is not drawn.
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
      out["compiled_at"] = platform::buildMomentSeconds();
    }
    // The machine by name and shape, for the browser's tab to tell one
    // machine's pages from another's: X1 (win11 vulkan1.4) reads differently
    // from UM780 (ubuntu-server-26.10 cuda70), and every word of it is what
    // this machine states about itself, nothing the page could have guessed.
    {
      const std::string host = selfHostName();
      if (!host.empty())
        out["hostname"] = host;
      const std::string os = selfOsName();
      if (!os.empty())
        out["os"] = os;
      const std::string device = selfBackend();
      if (!device.empty())
        out["backend"] = device;
    }
    // And the moment the newest page file was written: a server answering from
    // the folder outside hands over whatever was last written there - that one
    // file stands for the whole of them, being the last of the pages any
    // browser has been told. A server standing on the copy carried inside
    // knows no fresher truth than its own making, and hands over that moment.
    {
      const NewestPage page = m_externalPages
                                  ? newestPageFile(m_staticDir.empty() ? "webui" : m_staticDir)
                                  : NewestPage{};
      if (page.at > 0)
      {
        out["webui_at"] = page.at;
        out["webui_file"] = page.name;
      }
      else
        out["webui_at"] = platform::buildMomentSeconds();
    }
    if (memTotal > 0)
      out["memory"] = {{"total_bytes", memTotal}, {"free_bytes", memFree}};
    // What the memory sticks themselves are, as the machine's inventory states
    // them - the technology, the speed they run at, how wide a path they lay
    // across the board together, and the rate that path can carry. This is the
    // rating a reader brings to the memory graph: what this machine could move
    // when it moved perfectly. A machine whose inventory names no sticks states
    // no rating here, and the memory panel says nothing rather than inventing one.
    {
      const platform::MemorySticks &sticks = platform::memorySticks();
      if (sticks.ok)
      {
        nlohmann::json hw = {{"speed_mts", sticks.configuredMts},
                             {"rated_mts", sticks.ratedMts},
                             {"modules", sticks.modules},
                             {"data_bits", sticks.dataBits},
                             {"bytes", sticks.bytes},
                             {"bandwidth_gbps", sticks.bandwidthGbps()}};
        if (!sticks.kind.empty())
          hw["kind"] = sticks.kind;
        if (!sticks.maker.empty())
          hw["maker"] = sticks.maker;
        if (!sticks.part.empty())
          hw["part"] = sticks.part;
        out["memory_hw"] = hw;
      }
    }
    // How fast the picture card is running, and its ceiling, only where the card
    // itself states it through the vendor management library. The library answers
    // for cards of the one maker whose clocks this server will bind to; an AMD
    // core keeps its clocks out of any such library, and reads as no clocks here
    // - the panel then says the card states none rather than guessing a figure
    // off the product name, the same way it refuses to draw a line with no numbers
    // behind it. Where the card shares the machine's own memory, the memory panel's
    // rating is the graphics memory's rating, so the graphics panel borrows it.
    {
      const platform::GpuClocks clocks = platform::gpuClocks();
      if (clocks.ok)
      {
        nlohmann::json g = {{"core_mhz", clocks.coreMhz}, {"mem_mhz", clocks.memMhz}};
        if (clocks.coreMaxMhz > 0)
          g["core_max_mhz"] = clocks.coreMaxMhz;
        if (clocks.memMaxMhz > 0)
          g["mem_max_mhz"] = clocks.memMaxMhz;
        out["gpu_clocks"] = g;
      }
    }
    if (m_image)
      out["loaded"] = nlohmann::json::parse(m_image->statusJson(), nullptr, false);
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

  // ===========================================================================
  // A3 - the Project Studio doors, as one paste-ready block for
  // src/OpenLocalServices.cpp (spec_project_studio.md §11; the Project Studio
  // plan's Phase A, task A3 - now folded into plan.md §2).
  //
  // PASTE WHERE: inside namespace ols, immediately above
  //   `HttpResponse OpenLocalServices::route(const HttpRequest &req)` - :2349 as
  // the file stands today - so these helpers stand beside the door helpers that
  // are already file-static there (cancelRefusalWords :2333, postOnlyDoorWords
  // :2344).
  //
  // IT STANDS ON, already in this translation unit: queryValue (:446) and
  // jsonResponse (:619) - names of the file's earlier anonymous namespace, and so
  // visible to the end of the file - ols::statedString (Common.h:19),
  // platform::quotableInShell (Platform.h:227-249), and
  // <cctype> (:11) for the way a switch is read back (switchValue below follows
  // truthyValue :209 and booleanShape :217). One include goes at the top of the
  // file:
  //     #include "ProjectService.h"
  // The dispatch line in route(), the member and the setter are written out in
  // integration-notes.md.
  //
  // Every call to the project module goes through ProjectDoorService below, and
  // nowhere else: whatever names A1's header lands with, the bend is made in that
  // one struct.
  // ===========================================================================

  // Every Project Studio door stands under this prefix; route() asks this block
  // for nothing else.
  constexpr size_t kProjectApiPrefixLen = 13; // strlen("/project/api/")

  // A refusal out of these doors. The member jsonError (:1791) is not reachable
  // from a function standing outside the class, so these doors carry its shape
  // unchanged: the words kept to what prints, capped where the member caps them,
  // and never left for a cache to hold.
  static HttpResponse projectDoorRefusal(int status, const std::string &words)
  {
    HttpResponse r;
    r.status = status;
    nlohmann::json stated;
    std::string safe;
    constexpr size_t kMaxRefusalLength = 200;
    for (const char c : words)
    {
      if (safe.size() >= kMaxRefusalLength)
      {
        safe += "...";
        break;
      }
      const unsigned char u = static_cast<unsigned char>(c);
      safe.push_back(u >= 0x20 && u < 0x7f ? c : '?');
    }
    stated["error"] = safe;
    r.setJson(stated.dump());
    r.headers["Cache-Control"] = "no-store";
    return r;
  }

  // An answer in the shape the studio's action idiom reads: {did, words} and,
  // where the machine states one, the project's change stamp (§9.1). A refusal
  // is not worth holding, so anything but a 200 says so on itself, as the member
  // jsonError does.
  static HttpResponse projectDoorAnswer(int status, const nlohmann::json &body)
  {
    HttpResponse r = jsonResponse(status, body.dump());
    if (status != 200)
      r.headers["Cache-Control"] = "no-store";
    return r;
  }

  // Nothing these doors hand toward the machine's folders carries a control
  // character. A name with one in it is not a name typed in a hurry: it is a
  // name with something else attached to it. Refused, never escaped, as the
  // fetches refuse what they cannot quote (Platform.h:227-249).
  static bool plainWords(const std::string &text)
  {
    for (const char c : text)
    {
      const unsigned char u = static_cast<unsigned char>(c);
      if (u < 0x20 || u == 0x7f)
        return false;
    }
    return true;
  }

  // A switch the studio asks with: it says what it means as a boolean, a number,
  // or one of the words this machine already reads for yes and no (truthyValue
  // :209, booleanShape :217 - both stand on <cctype>, already included at :11). A
  // value worded no way a switch speaks is refused by name, the way the config
  // door refuses a value that breaks its own rule (:2638-2647).
  static bool switchValue(const nlohmann::json &flag, bool &wants)
  {
    if (flag.is_boolean())
    {
      wants = flag.get<bool>();
      return true;
    }
    if (flag.is_number())
    {
      wants = flag.get<double>() != 0.0;
      return true;
    }
    if (!flag.is_string())
      return false;
    std::string word;
    for (const char c : flag.get<std::string>())
      word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (word == "true" || word == "1" || word == "yes" || word == "on")
      wants = true;
    else if (word == "false" || word == "0" || word == "no" || word == "off")
      wants = false;
    else
      return false;
    return true;
  }

  // The one place these doors touch the project module. A1 owns
  // src/ProjectService.h; if that header lands with other names or other
  // argument shapes, this struct is the single place that changes.
  struct ProjectDoorService
  {
    explicit ProjectDoorService(ols::project::ProjectService *service) : m_service(service) {}

    std::string listJson() const { return m_service->listJson(); }
    std::string revsJson() const { return m_service->projectRevsJson(); }
    std::string metaJson(const std::string &slug) const { return m_service->metaJson(slug); }
    bool create(const std::string &name, const nlohmann::json &opts, std::string &madeSlug) const
    {
      return m_service->create(name, opts.dump(), madeSlug);
    }
    bool adoptFolder(const std::string &folder, std::string &madeSlug) const
    {
      return m_service->adoptFolder(folder, madeSlug);
    }
    bool adoptUrl(const std::string &url, const std::string &nameHint, std::string &madeSlug,
                  const std::string &key) const
    {
      return m_service->adoptUrl(url, nameHint, madeSlug, key);
    }
    bool rename(const std::string &slug, const std::string &newName) const
    {
      return m_service->rename(slug, newName);
    }
    bool remove(const std::string &slug, const std::string &confirm, std::string &residueWords) const
    {
      return m_service->remove(slug, confirm, residueWords);
    }
    bool editMeta(const std::string &slug, const nlohmann::json &patch) const
    {
      return m_service->editMeta(slug, patch);
    }
    // The build doors bend here as well. As with the others, the service is
    // held through a pointer, so only the handle comes through a wrapping as
    // const; the arguments are the header's, one to one.
    bool toolchainFor(const std::string &extension, std::string &words) const
    {
      return m_service->toolchainFor(extension, words);
    }
    bool startBuild(const std::string &slug, const std::string &relPath,
                    std::string &jobIdOut, std::string &words) const
    {
      return m_service->startBuild(slug, relPath, jobIdOut, words);
    }
    bool buildStatus(const std::string &jobId, std::string &answerJsonOut,
                     std::string &words) const
    {
      return m_service->buildStatus(jobId, answerJsonOut, words);
    }
    bool stopBuild(const std::string &jobId, std::string &words) const
    {
      return m_service->stopBuild(jobId, words);
    }
    std::string lastError() const { return m_service->lastError(); }

  private:
    ols::project::ProjectService *m_service;
  };

  // The projects array of the index, read tolerantly: A1 words it either as a
  // bare array or wrapped in {projects:[...]}. An answer this reader cannot read
  // yields nothing rather than a guess.
  static const nlohmann::json *projectsArray(const nlohmann::json &list)
  {
    if (list.is_array())
      return &list;
    if (list.is_object() && list.contains("projects") && list.at("projects").is_array())
      return &list.at("projects");
    return nullptr;
  }

  static bool listCarriesSlug(const nlohmann::json &list, const std::string &slug)
  {
    const nlohmann::json *projects = projectsArray(list);
    if (!projects)
      return false;
    for (const auto &entry : *projects)
      if (entry.is_object() && statedString(entry, "slug") == slug)
        return true;
    return false;
  }

  // Whether the machine's own index carries this slug. The doors ask the index
  // rather than reading a module's refusal wording to tell a missing project
  // from a broken one.
  static bool indexCarries(const ProjectDoorService &svc, const std::string &slug)
  {
    return listCarriesSlug(nlohmann::json::parse(svc.listJson(), nullptr, false), slug);
  }

  // The change stamp of one project (§9.1) out of the module's revs document.
  // Three shapes are read because the wording of that document is the module's:
  // {"revs":{"<slug>":N}}, {"projects":[{"slug":"..","rev":N},..]}, or the slug
  // keyed at the top level. A stamp the machine does not state stays off the
  // answer rather than being made up for it.
  static void putRev(nlohmann::json &answer, const ProjectDoorService &svc, const std::string &slug)
  {
    if (slug.empty())
      return;
    const nlohmann::json revs = nlohmann::json::parse(svc.revsJson(), nullptr, false);
    if (revs.is_discarded() || !revs.is_object())
      return;
    long long stamp = 0;
    bool stated = false;
    if (revs.contains("revs") && revs.at("revs").is_object() && revs.at("revs").contains(slug) &&
        revs.at("revs").at(slug).is_number_integer())
    {
      stamp = revs.at("revs").at(slug).get<long long>();
      stated = true;
    }
    else if (const nlohmann::json *projects = projectsArray(revs))
    {
      for (const auto &entry : *projects)
      {
        if (entry.is_object() && statedString(entry, "slug") == slug && entry.contains("rev") &&
            entry.at("rev").is_number_integer())
        {
          stamp = entry.at("rev").get<long long>();
          stated = true;
          break;
        }
      }
    }
    else if (revs.contains(slug) && revs.at(slug).is_number_integer())
    {
      stamp = revs.at(slug).get<long long>();
      stated = true;
    }
    if (stated)
      answer["rev"] = stamp;
  }

  // A project named at one of these doors: stated, plain of control characters,
  // and one the index carries. What a slug may be - the folders are the module's
  // - is the module's rule; these doors pass the words through once a control
  // character is out of them.
  static bool projectNamed(const ProjectDoorService &svc, const std::string &slug,
                           const char *missingWords, HttpResponse &refusal)
  {
    if (slug.empty())
    {
      refusal = projectDoorRefusal(400, missingWords);
      return false;
    }
    if (!plainWords(slug))
    {
      refusal = projectDoorRefusal(400,
                                   "That project name carries characters this machine will not take");
      return false;
    }
    if (!indexCarries(svc, slug))
    {
      refusal = projectDoorRefusal(404, "Unknown project: " + slug);
      return false;
    }
    return true;
  }

  // The words and the status of a refusal, out of the module's own last-error
  // channel. Words about this ask answer as a bad ask does, the way the kept
  // reference door reads its module (:2489-2495); no words at all means the
  // module did not finish, which is the answer for a module that broke.
  static int refusalFromModule(const ProjectDoorService &svc, std::string &words)
  {
    const std::string why = svc.lastError();
    if (why.empty())
    {
      words = "The project module refused this ask and stated no reason";
      return 500;
    }
    words = why;
    return 400;
  }

  // {did, words} with the change stamp of the project the ask was about, as the
  // machine states it now - the shape every mutation of this studio answers with
  // (spec §11), so a page that pressed something reads the fact of it and the
  // stamp to compare against, in one answer.
  static HttpResponse actionAnswer(bool did, const std::string &words,
                                   const ProjectDoorService &svc, const std::string &slug,
                                   int status)
  {
    nlohmann::json body;
    body["did"] = did;
    body["words"] = words;
    putRev(body, svc, slug);
    return projectDoorAnswer(status, body);
  }

  // The live route table of these doors, so an agent can discover them without
  // reading this file (spec §13). KEPT IN STEP BY HAND - nothing checks this
  // literal against the doors below: a new door means a new row here, and
  // doors-test.sh asks this door for every path it tests, so a row left behind
  // shows up there. A path both read and written carries one row per method,
  // each naming the params that method takes; a trailing ? marks a param that
  // may be left out.
  static const nlohmann::json &projectDoorsTable()
  {
    static const nlohmann::json kDoors = nlohmann::json::array(
        {
            nlohmann::json{{"path", "/project/api/index"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array()}},
            nlohmann::json{{"path", "/project/api/revs"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array()}},
            nlohmann::json{{"path", "/project/api/events"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array()}},
            nlohmann::json{{"path", "/project/api/doors"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array()}},
            nlohmann::json{{"path", "/project/api/cert"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array()}},
            nlohmann::json{{"path", "/project/api/cert/ca.der"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array()}},
            nlohmann::json{{"path", "/project/api/meta"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array({"project"})}},
            nlohmann::json{{"path", "/project/api/meta"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"project", "patch"})}},
            nlohmann::json{{"path", "/project/api/projects/new"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params",
                            nlohmann::json::array({"name", "default_branch?", "with_readme?",
                                                   "license?"})}},
            nlohmann::json{{"path", "/project/api/projects/adopt"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"path?", "url?"})}},
            nlohmann::json{{"path", "/project/api/projects/rename"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"slug", "name"})}},
            nlohmann::json{{"path", "/project/api/projects/delete"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"slug", "confirm"})}},
            nlohmann::json{{"path", "/project/api/tree"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array({"project", "dir?", "ref?"})}},
            nlohmann::json{{"path", "/project/api/file"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array({"project", "path", "ref?"})}},
            nlohmann::json{{"path", "/project/api/file-outline"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array({"project", "path", "ref?"})}},
            nlohmann::json{{"path", "/project/api/history"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params",
                            nlohmann::json::array({"project", "path?", "ref?", "limit?",
                                                   "after?", "first_parent?", "no_merges?",
                                                   "author?", "text?", "since?", "until?"})}},
            nlohmann::json{{"path", "/project/api/blame"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array({"project", "path", "ref?"})}},
            nlohmann::json{{"path", "/project/api/commit"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params",
                            nlohmann::json::array({"project", "branch?", "message", "author?",
                                                   "files[{path,content,delete?}]"})}},
            nlohmann::json{{"path", "/project/api/build"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array({"job"})}},
            nlohmann::json{{"path", "/project/api/build"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"project", "path"})}},
            nlohmann::json{{"path", "/project/api/build-status"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array({"job"})}},
            nlohmann::json{{"path", "/project/api/build-stop"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"job"})}},
            nlohmann::json{{"path", "/project/api/pulls"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array({"project", "state?", "limit?"})}},
            nlohmann::json{{"path", "/project/api/pulls"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params",
                            nlohmann::json::array({"project", "head", "base?", "title", "body?"})}},
            nlohmann::json{{"path", "/project/api/pulls/<n>"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array({"project"})}},
            nlohmann::json{{"path", "/project/api/pulls/<n>/approve"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"project", "by?", "body?"})}},
            nlohmann::json{{"path", "/project/api/pulls/<n>/request-changes"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"project", "by?", "body?"})}},
            nlohmann::json{{"path", "/project/api/pulls/<n>/comment"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params",
                            nlohmann::json::array({"project", "by?", "body", "anchor?"})}},
            nlohmann::json{{"path", "/project/api/pulls/<n>/merge"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"project", "style?", "by?"})}},
            nlohmann::json{{"path", "/project/api/pulls/<n>/resolve"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params",
                            nlohmann::json::array({"project", "file", "content?", "delete?", "by?"})}},
            nlohmann::json{{"path", "/project/api/pulls/<n>/close"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"project"})}},
            nlohmann::json{{"path", "/project/api/pulls/<n>/reopen"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"project"})}},
            nlohmann::json{{"path", "/project/api/branches"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array({"project"})}},
            nlohmann::json{{"path", "/project/api/branches/new"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"project", "branch", "start?"})}},
            nlohmann::json{{"path", "/project/api/branches/delete"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"project", "branch"})}},
            nlohmann::json{{"path", "/project/api/issues"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params",
                            nlohmann::json::array({"project", "file?", "state?", "limit?"})}},
            nlohmann::json{{"path", "/project/api/issues"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params",
                            nlohmann::json::array({"project", "title", "body?", "severity?",
                                                   "files?", "labels?"})}},
            nlohmann::json{{"path", "/project/api/issues.json"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params",
                            nlohmann::json::array({"project", "file?", "state?", "limit?"})}},
            nlohmann::json{{"path", "/project/api/issues/<id>"},
                           {"methods", nlohmann::json::array({"GET"})},
                           {"params", nlohmann::json::array({"project"})}},
            nlohmann::json{{"path", "/project/api/issues/<id>/edit"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"project", "patch"})}},
            nlohmann::json{{"path", "/project/api/issues/<id>/comments"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params",
                            nlohmann::json::array({"project", "by?", "body", "anchor?"})}},
            nlohmann::json{{"path", "/project/api/issues/<id>/close"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"project"})}},
            nlohmann::json{{"path", "/project/api/issues/<id>/reopen"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"project"})}},
            nlohmann::json{{"path", "/project/api/issues/<id>/link"},
                           {"methods", nlohmann::json::array({"POST"})},
                           {"params", nlohmann::json::array({"project", "kind", "target"})}},
        });
    return kDoors;
  }

  // One body, read as every action door reads it: not one json object is a bad
  // ask, answered before any part of it is looked at.
  static bool bodyObject(const HttpRequest &req, nlohmann::json &given)
  {
    given = nlohmann::json::parse(req.body, nullptr, false);
    return !given.is_discarded() && given.is_object();
  }

  // The Project Studio doors (spec §11). route() hands this everything under
  // /project/api/, after the key gate and the rate gate it runs for every door.
  // True means this block answered; false means the path is not one of these
  // doors, and route() answers it as it answers any address it does not know.
  // The code doors of §5.3, standing on the browse engine (ProjectGit) over a
  // project's bare repo. A ref arrives as its short name ("main", "HEAD", or
  // nothing - the machine's own default) or as a raw commit id - 40 lowercase
  // hex, which resolves when the repo holds that commit, the way the history
  // UI points at one; a name resolves through listRefs, so only a ref the
  // repository itself carries is ever read, and an unknown one says so in the
  // module's words. Tree paths arrive already checked by ProjectGit:
  // repo-relative, no "..", never absolute.

  // One row of a tree listing, in the shape the files column reads: name, path,
  // mode, oid, and isDir - plus "dir" so a reader that came from the old table
  // still finds the flag it knows.
  static nlohmann::json codeTreeRow(const ols::project::TreeEntry &entry)
  {
    nlohmann::json row;
    row["name"] = entry.name;
    row["path"] = entry.path;
    row["mode"] = entry.mode;
    row["oid"] = entry.oid;
    row["isDir"] = entry.isDir;
    row["dir"] = entry.isDir;
    return row;
  }

  // The ref in effect and the commit under it, out of the ask's words: empty
  // refName means HEAD; a name is looked up among the repo's own refs (the bare
  // "heads/x" spelling accepted the way git accepts it, and so is a bare "main"
  // - the name as it stands first, then under refs/heads, which is how git reads
  // it); a raw commit id - 40 lowercase hex, the only shape this git layer looks
  // objects up by, so a shorter prefix stays a name - resolves when the repo
  // holds that commit. False with the engine's own words, naming every form the
  // ask was tried in, when nothing named stands there.
  static bool codeLooksLikeOid(const std::string &text)
  {
    if (text.size() != 40u)
      return false;
    for (const char c : text)
      if ((c < '0' || c > '9') && (c < 'a' || c > 'f'))
        return false;
    return true;
  }

  static bool codeResolveRef(const std::string &repoDir, const std::string &refName,
                             std::string &oidHex, std::string &words)
  {
    if (refName.empty() || refName == "HEAD")
    {
      if (!ols::project::readHeadOid(repoDir, oidHex))
      {
        words = ols::project::lastError();
        return false;
      }
      return true;
    }
    std::vector<ols::project::RefInfo> refs;
    if (!ols::project::listRefs(repoDir, refs))
    {
      words = ols::project::lastError();
      return false;
    }
    const std::string want = (refName.rfind("refs/", 0) == 0) ? refName : ("refs/" + refName);
    for (const auto &info : refs)
    {
      if (info.name == want || info.name == "refs/tags/" + refName)
      {
        if (info.oid.empty())
        {
          words = "the ref named no commit it resolves to: " + refName;
          return false;
        }
        oidHex = info.oid;
        return true;
      }
    }
    // A bare name - one wearing no '/' - gets the reading git gives it: the
    // same name under refs/heads. The pass runs only after the forms above
    // came up empty, so a name that already stood anywhere is read there,
    // exactly as it always was.
    const bool bare = refName.find('/') == std::string::npos;
    if (bare)
    {
      const std::string headForm = "refs/heads/" + refName;
      for (const auto &info : refs)
      {
        if (info.name != headForm)
          continue;
        if (info.oid.empty())
        {
          words = "the ref named no commit it resolves to: " + headForm;
          return false;
        }
        oidHex = info.oid;
        return true;
      }
    }
    // The history UI points at a commit by its raw id: an oid-shaped ask is
    // answered when the repo holds that commit. Named refs had their turn
    // above, so a name that stands is still read exactly as it was.
    if (codeLooksLikeOid(refName))
    {
      if (!ols::project::resolveCommitOid(repoDir, refName, oidHex))
      {
        words = ols::project::lastError();
        return false;
      }
      return true;
    }
    // The words state every form the ask was tried in, so the refusal says
    // what was looked for and not merely what was typed.
    std::string asked = "asked as " + want;
    asked += " and refs/tags/" + refName;
    if (bare)
      asked += " and refs/heads/" + refName;
    words = "no such ref in this repo: " + refName + " (" + asked + ")";
    return false;
  }

  // One code door answered from a resolved commit: the callback does the read
  // and states its own body on success; a refusal is the engine's words, at 404
  // when the answer was that nothing stands where the ask pointed. The commit
  // arrives as an argument, not through the body - a door's stamps are added
  // after its work, never before it.
  static HttpResponse codeDoorAnswer(const std::string &repoDir, const std::string &refName,
                                     const char *defaultRefWords,
                                     const std::function<bool(const std::string &, nlohmann::json &, std::string &)> &read)
  {
    std::string oidHex;
    std::string words;
    if (!codeResolveRef(repoDir, refName, oidHex, words))
      return projectDoorRefusal(404, words);
    nlohmann::json body;
    if (!read(oidHex, body, words))
      return projectDoorRefusal(404, words.empty() ? ols::project::lastError() : words);
    // The stamp of what the answer stands on: the commit read and the ref in
    // effect, so a page can say which head it is looking at (§5.3's chip).
    body["ref"] = refName.empty() ? defaultRefWords : refName;
    body["commit"] = oidHex;
    return projectDoorAnswer(200, body);
  }

  // The browse doors of the code tab (spec §11), reached from routeProject once
  // the slug is known to be the index's own. Every read goes through ProjectGit,
  // which takes the repo's folder and nothing else - a path that leaves the tree
  // was refused before any byte of it was looked at.

  // The smart-HTTP doors of git itself (spec §8): /git/<slug>/info/refs,
  // /git/<slug>/git-upload-pack. Protocol v2 through the wire layer; an answer
  // too big to hold is written over the connection as it is produced. The key
  // gate and the rate gate ran for this path like every other before route()
  // was asked.

  // git's upload-pack requests name their command and their bare options in
  // pkt-lines that carry no trailing line byte ("command=fetch" measured at
  // 17, "thin-pack" at 13, while ls-refs writes its lines whole). The wire
  // grammar reads one grammar — every data line ends a line — so the route
  // restores missing line bytes here, for every data pkt of this request
  // (a request carries no pack bytes; flush- and delim pkts stay untouched).
  static std::string normalizeGitRequestLines(const std::string &bytes)
  {
    auto hex4 = [](const std::string &s, std::size_t p, unsigned &n)
    {
      n = 0u;
      for (std::size_t at = 0u; at < 4u; ++at)
      {
        const char c = s[p + at];
        unsigned d = 255u;
        if (c >= '0' && c <= '9')
          d = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f')
          d = static_cast<unsigned>(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F')
          d = static_cast<unsigned>(c - 'A') + 10u;
        else
          return false;
        n = n * 16u + d;
      }
      return true;
    };
    std::string out;
    out.reserve(bytes.size() + 16u);
    std::size_t at = 0u;
    while (at + 4u <= bytes.size())
    {
      unsigned n = 0u;
      if (!hex4(bytes, at, n))
        break;
      if (n == 0u || n == 1u)
      { // flush-pkt and delim-pkt travel as they stand
        out.append(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                   bytes.begin() + static_cast<std::ptrdiff_t>(at + 4u));
        at += 4u;
        continue;
      }
      if (n < 5u || at + n > bytes.size())
        break; // not a well-formed pkt stream: hand the rest back as it came
      const std::size_t payload = n - 4u;
      if (bytes[at + 4u + payload - 1u] == '\n')
      {
        out.append(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                   bytes.begin() + static_cast<std::ptrdiff_t>(at + n));
      }
      else
      {
        char prefix[5];
        std::snprintf(prefix, sizeof(prefix), "%04x", n + 1u);
        out += prefix;
        out.append(bytes.begin() + static_cast<std::ptrdiff_t>(at + 4u),
                   bytes.begin() + static_cast<std::ptrdiff_t>(at + n));
        out += '\n';
      }
      at += n;
    }
    if (at < bytes.size())
      out.append(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                 bytes.end());
    return out;
  }

  bool routeGitSmart(ols::project::ProjectService *projects, const HttpRequest &req,
                     HttpResponse &res)
  {
    if (!projects)
    {
      res = projectDoorRefusal(502, "No project service");
      return true;
    }
    const ols::project::ProjectService &svc = *projects;
    const std::string tail = req.path.substr(5); // after "/git/"
    const size_t slash = tail.find('/');
    const std::string slug = slash == std::string::npos ? tail : tail.substr(0, slash);
    const std::string rest = slash == std::string::npos ? std::string() : tail.substr(slash + 1);

    // The index is asked first: a repo folder the index does not name is nobody's
    // business, and this says so before any path under data/ is looked at.
    {
      const ProjectDoorService named(projects);
      HttpResponse refusal;
      if (!projectNamed(named, slug, "No such project to clone.", refusal))
      {
        res = refusal;
        return true;
      }
    }
    const std::string repoDir = svc.repoDir(slug);
    if (repoDir.empty())
    {
      res = projectDoorRefusal(404, "No repo stands for project: " + slug);
      return true;
    }

    if (rest == "info/refs")
    {
      // GET only, with ?service= naming which half git came for; the two
      // halves answer in their own dialects: upload-pack keeps the v2 head
      // (version line, capability lines, flush), and receive-pack answers
      // the v0 advertisement — git's own push speaks v0, and it is built
      // whole on the next door.
      const std::string service = queryValue(req.query, "service");
      if (req.method != "GET" && req.method != "HEAD")
      {
        res = projectDoorRefusal(405, "A git advertisement is read with a GET");
        return true;
      }
      const bool upload = service == "git-upload-pack";
      const bool receive = service == "git-receive-pack";
      if (!upload && !receive)
      {
        res = projectDoorRefusal(400, "Which git service? Send ?service=git-upload-pack or git-receive-pack");
        return true;
      }
      if (receive)
      {
        // The v0 capability advertisement, framed: the marker line and its
        // flush, the ref lines with HEAD first — the capability list stands
        // after the NUL of the first line, and an unborn HEAD is the all-
        // zeros id — closed with the flush. Real git 2.x send-pack reads
        // what it gets here and refuses a v2 advertisement outright
        // ("support for protocol v2 not implemented yet" — it never POSTs),
        // so this half of the conversation is v0 end to end: the body the
        // door below reads carries v0's command lines for the same reason.
        std::vector<std::uint8_t> bytes;
        std::string words;
        if (!ols::git::receivePackAdvertisement(repoDir, bytes, words))
        {
          res = projectDoorRefusal(500, words.empty() ? std::string("the advertisement could not be read") : words);
          return true;
        }
        HttpResponse wire;
        wire.status = 200;
        wire.setPlain(std::string(bytes.begin(), bytes.end()),
                      "application/x-git-receive-pack-advertisement");
        // An advertisement is asked fresh each time: the refs behind it move with
        // every push, so no cache may hold one.
        wire.headers["Cache-Control"] = "no-cache";
        res = wire;
        return true;
      }
      std::vector<std::string> caps;
      std::string err;
      // git's v2 advertisement of upload-pack, the way the v2 document states it:
      // a server names the capabilities it answers — a "command=" line is what a
      // client asks with, never what a server says. What is said here is what
      // this door answers: ls-refs takes its unborn argument (the unborn HEAD
      // line stands), a fetch waits for its done before answering one round
      // (wait-for-done); shallow is not answered and so is not advertised,
      // and a server-option the client sends is read through without being
      // asked for anything.
      const std::string version = platform::buildVersion();
      std::string agentToken;
      for (char c : version)
      {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
          agentToken.push_back(c);
      }
      caps.push_back("agent=git/ols-" + (agentToken.empty() ? std::string("unknown") : agentToken) + "\n");
      if (upload)
      {
        caps.push_back("ls-refs=unborn\n");
        caps.push_back("fetch=wait-for-done\n");
        caps.push_back("server-option\n");
      }
      caps.push_back("object-format=sha1\n");
      std::vector<std::uint8_t> bytes;
      if (!ols::gitcore::v2Advertise(caps, bytes, err))
      {
        res = projectDoorRefusal(500, err);
        return true;
      }
      HttpResponse wire;
      wire.status = 200;
      wire.setPlain(std::string(bytes.begin(), bytes.end()),
                    "application/x-git-" + service.substr(4) + "-advertisement");
      // An advertisement is asked fresh each time: the refs behind it move with
      // every push, so no cache may hold one.
      wire.headers["Cache-Control"] = "no-cache";
      res = wire;
      return true;
    }

    if (rest == "git-upload-pack")
    {
      if (req.method != "POST")
      {
        res = projectDoorRefusal(405, "An upload-pack command is sent with a POST");
        return true;
      }
      // Git-Protocol: version=2 names the v2 conversation. This door speaks v2
      // only: whether the header stands or not, the v2 shape answers — presence
      // changes what a client promised, and naming nothing still gets v2,
      // because the v0 fallback is out of scope: building a conversation this
      // door never held would be a guess, and a body that is not a v2 command
      // request is refused below in v2's own words rather than guessed as v0.
      const auto gp = req.headers.find("git-protocol");
      if (gp == req.headers.end() ||
          lowerText(gp->second).find("version=2") == std::string::npos)
        std::cerr << "[git] " << req.path << " did not name Git-Protocol: version=2; answering the v2 shape." << std::endl;
      // The body is this request's own bytes: for a git door they stand in the
      // temp file the transport wrote them to, and only the route reads them.
      std::string body = req.body;
      const auto bf = req.headers.find("x-ols-body-file");
      if (bf != req.headers.end() && !bf->second.empty())
      {
        std::ifstream in(bf->second, std::ios::binary);
        std::ostringstream all;
        all << in.rdbuf();
        body = all.str();
      }
      body = normalizeGitRequestLines(body);
      res.status = 200;
      res.gitWriter = [](void *sinkHandle, const HttpRequest &r)
      {
        (void)r;
        // The job comes out of the thread-local into this hand, and this hand
        // ends it on every road out of the writer - the throw included.
        std::unique_ptr<UploadJob> job = std::move(gGitUploadJob);
        if (job == nullptr)
          return;
        UploadJob &kept = *job;
        const ConnectionSink &sink = *static_cast<const ConnectionSink *>(sinkHandle);
        std::string err;
        ols::gitcore::V2Request v2;
        const auto *reqBytes = reinterpret_cast<const std::uint8_t *>(kept.request.data());
        const std::string outHead =
            "HTTP/1.1 200 OK\r\nContent-Type: application/x-git-upload-pack-result\r\n"
            "Cache-Control: no-store\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
        auto v2Refusal = [&](bool withHead, const std::string &why)
        {
          // Once this door has spoken — or is about to — its stream ends with
          // the protocol's own refusal, not a cutoff: an ERR pkt-line, a
          // flush-pkt, and the terminating chunk. A bare 400 or a short
          // stream is not something a git client reads. (A fatal inside a
          // fetch's pack section travels wrapped in the sideband instead —
          // that belongs to the pack answer itself.)
          if (withHead && !sendAll(sink, outHead))
            return;
          std::vector<std::uint8_t> framed;
          std::string wide;
          const std::string line = "ERR " + why + "\n";
          if (!ols::gitcore::pktWrite(line, framed, wide))
            ols::gitcore::pktWrite("ERR\n", framed, wide); // a reason longer than one line still gets a refusal
          ols::gitcore::pktFlush(framed);
          void *sinkPtr = const_cast<void *>(static_cast<const void *>(&sink));
          (void)OpenLocalServices::writeStreamChunk(sinkPtr, std::string(framed.begin(), framed.end()));
          sendAll(sink, "0\r\n\r\n");
        };
        if (!ols::gitcore::v2ParseRequest(reqBytes, kept.request.size(), v2, err))
        {
          // A body that is not a v2 command request is answered in v2's own
          // shape: the ERR line ends the chunked stream, so the client reads
          // a refusal rather than a 400 it cannot carry.
          v2Refusal(true, err);
          return;
        }
        std::vector<ols::project::RefInfo> refs;
        if (!ols::project::listRefs(kept.repoDir, refs))
        {
          sendAll(sink, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
          return;
        }
        if (!sendAll(sink, outHead))
          return;
        auto emit = [&](const std::vector<std::uint8_t> &bytes)
        {
          // The sink travels as the same opaque handle the streaming doors use.
          void *sinkPtr = const_cast<void *>(static_cast<const void *>(&sink));
          return OpenLocalServices::writeStreamChunk(sinkPtr, std::string(bytes.begin(), bytes.end()));
        };
        if (v2.command == "ls-refs")
        {
          ols::gitcore::LsRefsRequest lr;
          std::vector<ols::gitcore::LsRefsEntry> entries;
          if (!ols::gitcore::v2ParseLsRefs(v2, lr, err))
          {
            v2Refusal(false, err);
            return;
          }
          for (const auto &info : refs)
          {
            ols::gitcore::LsRefsEntry row;
            row.name = info.name;
            // An empty oid hex becomes the id of nothing; with isSymref and a
            // target standing beside it, the wire writes the "unborn" line —
            // exactly the unborn HEAD a client reads before any first save.
            row.oid = info.oid;
            // ProjectGit.h states a symref is exactly a row with a non-empty
            // target (its immediate target); pass both halves through so a
            // born HEAD travels with its "symref-target:" when the client
            // asked for symrefs.
            row.isSymref = !info.target.empty();
            row.symrefTarget = info.target;
            entries.push_back(row);
          }
          std::vector<std::uint8_t> answer;
          if (!ols::gitcore::lsRefsAnswer(entries, lr, answer, err))
          {
            v2Refusal(false, err);
            return;
          }
          if (emit(answer))
            sendAll(sink, "0\r\n\r\n");
          return;
        }
        if (v2.command == "fetch")
        {
          // The pack builder answers now: wants minus what every have already
          // reaches, packed whole and verified before it ships. A refusal still
          // travels as the protocol's own fatal line - never a short pack.
          std::vector<std::uint8_t> answer;
          std::string words;
          const std::vector<std::uint8_t> noBody;
          const bool built = ols::git::handleUploadPackFetch(kept.repoDir, v2, noBody, answer, words);
          if (built || !answer.empty())
          {
            // either the pack answer or the builder's own protocol refusal —
            // both are framed bytes, both end the stream with the terminator
            if (emit(answer))
              sendAll(sink, "0\r\n\r\n");
          }
          else
            v2Refusal(false, words.empty() ? std::string("the fetch was refused without words") : words);
          return;
        }
        v2Refusal(false, "the command '" + (v2.command.empty() ? std::string("none") : v2.command) +
                             "' is not answered by this door");
      };
      // The job rides on the thread-local, which owns it: the writer takes it
      // out by hand, and a connection whose writer this never becomes leaves it
      // here - given up beside the writer's call, or by this thread's end.
      gGitUploadJob.reset(new UploadJob{repoDir, std::move(body)});
      // The transport runs this answer's writer just after route() returns.
      return true;
    }

    if (rest == "git-receive-pack")
    {
      if (req.method != "POST")
      {
        res = projectDoorRefusal(405, "A receive-pack command is sent with a POST");
        return true;
      }
      // The push arrives in whichever dialect the advertisement above chose:
      // v0's command lines, as a real git 2.x push sends them, or v2's
      // command= shape — handleReceivePack reads the body itself and answers
      // either. The Git-Protocol header names nothing here: presence records
      // what a client promised, and the body's own shape says which road
      // stands; a command section neither road fits answers with report-
      // status words below, not an http error the client cannot read.
      const auto gp = req.headers.find("git-protocol");
      if (gp == req.headers.end() ||
          lowerText(gp->second).find("version=2") == std::string::npos)
        std::cerr << "[git] " << req.path << " did not name Git-Protocol: version=2; the body's own dialect answers." << std::endl;
      // The push arrives whole: its command section and the pack that follows
      // the flush. A git door's bytes stand in the temp file the transport
      // streamed them to; the small ones ride req.body.
      std::string body = req.body;
      const auto bf = req.headers.find("x-ols-body-file");
      if (bf != req.headers.end() && !bf->second.empty())
      {
        std::ifstream in(bf->second, std::ios::binary);
        std::ostringstream all;
        all << in.rdbuf();
        body = all.str();
      }
      const std::vector<std::uint8_t> bytes(body.begin(), body.end());
      // The policy: the project's default branch is protected from history
      // rewrites - a push to it must be a fast-forward. Read off meta.json;
      // nothing stated protects nothing, and nothing is guessed.
      std::vector<std::string> protectedBranches;
      {
        nlohmann::json facts = nlohmann::json::parse(svc.metaJson(slug), nullptr, false);
        if (!facts.is_discarded() && facts.contains("default_branch") &&
            facts.at("default_branch").is_string())
        {
          const std::string branch = facts.at("default_branch").get<std::string>();
          if (!branch.empty())
            protectedBranches.push_back("refs/heads/" + branch);
        }
      }
      std::vector<std::uint8_t> answer;
      std::string words;
      HttpResponse wire;
      // A refused push still answers report-status: the protocol's own words,
      // not an http error the client cannot read.
      wire.status = 200;
      if (ols::git::handleReceivePack(repoDir, bytes, protectedBranches, answer, words) ||
          !answer.empty())
      {
        wire.setPlain(std::string(answer.begin(), answer.end()),
                      "application/x-git-receive-pack-result");
        res = wire;
      }
      else
        res = projectDoorRefusal(400, words);
      return true;
    }

    res = projectDoorRefusal(404, "No such git door.");
    return true;
  }

  bool routeProjectCode(const ols::project::ProjectService &svc, const std::string &slug,
                        const HttpRequest &req, HttpResponse &res)
  {
    // The slug is the index's own; this names its repo's folder and says whether
    // one stands there at all.
    const std::string repoDir = svc.repoDir(slug);
    if (repoDir.empty())
    {
      res = projectDoorRefusal(404, "No repo stands for project: " + slug);
      return true;
    }
    const bool get = req.method == "GET" || req.method == "HEAD";
    const std::string door = req.path.substr(kProjectApiPrefixLen);
    // The ref arrives as "ref" (the contract's word) or the short form "r".
    std::string refName = queryValue(req.query, "ref");
    if (refName.empty())
      refName = queryValue(req.query, "r");

    if (door == "tree")
    {
      if (!get)
      {
        res = projectDoorRefusal(405, "A tree is read with a GET");
        return true;
      }
      const std::string dirPath = queryValue(req.query, "dir");
      res = codeDoorAnswer(repoDir, refName, "HEAD", [&](const std::string &commit, nlohmann::json &body, std::string &words)
                           {
        std::vector<ols::project::TreeEntry> entries;
        if (!ols::project::treeAt(repoDir, commit, dirPath, entries))
        {
          words = ols::project::lastError();
          return false;
        }
        nlohmann::json rows = nlohmann::json::array();
        for (const auto &entry : entries)
          rows.push_back(codeTreeRow(entry));
        body["entries"] = rows;
        return true; });
      return true;
    }

    if (door == "file")
    {
      if (!get)
      {
        res = projectDoorRefusal(405, "A file is read with a GET");
        return true;
      }
      const std::string path = queryValue(req.query, "path");
      if (path.empty())
      {
        res = projectDoorRefusal(400, "Which file? These doors name it with ?path=");
        return true;
      }
      res = codeDoorAnswer(repoDir, refName, "HEAD", [&](const std::string &commit, nlohmann::json &body, std::string &words)
                           {
        std::vector<std::uint8_t> bytes;
        std::string oidHex;
        if (!ols::project::blobAt(repoDir, commit, path, bytes, oidHex))
        {
          words = ols::project::lastError();
          return false;
        }
        // Bytes with a NUL in their first 8 KiB are said to be what they are:
        // the viewer shows that honestly and offers nothing else.
        const std::size_t sniff = bytes.size() < 8192 ? bytes.size() : 8192;
        bool binary = false;
        for (std::size_t i = 0; i < sniff; ++i)
          if (bytes[i] == 0)
          {
            binary = true;
            break;
          }
        body["path"] = path;
        body["oid"] = oidHex;
        body["size"] = bytes.size();
        if (binary)
        {
          body["binary"] = true;
          return true;
        }
        body["content"] = std::string(bytes.begin(), bytes.end());
        return true; });
      return true;
    }

    if (door == "file-outline")
    {
      if (!get)
      {
        res = projectDoorRefusal(405, "An outline is read with a GET");
        return true;
      }
      // The bytes are named by path at a ref - the same way every other code
      // door reads them. An oid alone cannot be answered: this machine holds no
      // index from an id back to a name, and inventing one would state more
      // than it knows.
      const std::string path = queryValue(req.query, "path");
      if (path.empty())
      {
        res = projectDoorRefusal(400, "Which file? This door names it with ?path=");
        return true;
      }
      res = codeDoorAnswer(repoDir, refName, "HEAD", [&](const std::string &commit, nlohmann::json &body, std::string &words)
                           {
        std::vector<std::uint8_t> bytes;
        std::string oidHex;
        if (!ols::project::blobAt(repoDir, commit, path, bytes, oidHex))
        {
          words = ols::project::lastError();
          return false;
        }
        nlohmann::json outline;
        if (!ols::project::outlineForBlob(oidHex, bytes, path, outline))
        {
          words = ols::project::lastError();
          return false;
        }
        body["path"] = path;
        body["oid"] = oidHex;
        // The scanner's own document carries language and symbols; it stands
        // beside the stamps, not inside them.
        if (outline.contains("language"))
          body["language"] = outline.at("language");
        if (outline.contains("symbols"))
          body["symbols"] = outline.at("symbols");
        return true; });
      return true;
    }

    if (door == "history")
    {
      if (!get)
      {
        res = projectDoorRefusal(405, "A history is read with a GET");
        return true;
      }
      const std::string path = queryValue(req.query, "path"); // empty: the whole log
      // The walk's ask, read from the query with the bounds the engine
      // states where callers can see them: 50 rows by default, 200 at
      // most; a flag is 0 or 1; since/until are whole unix seconds on the
      // author date; after is a full object oid; an empty needle or name
      // is one not asked for. A malformed ask is refused in plain words
      // before any byte of the repo is read for it.
      ols::project::HistoryAsk ask;
      ask.pathOrEmpty = path;
      bool bad = false;
      std::string badWords;
      const auto flag = [&](const char *name, bool &into)
      {
        const std::string text = queryValue(req.query, name);
        if (text.empty() || text == "0")
          return;
        if (text == "1")
        {
          into = true;
          return;
        }
        bad = true;
        badWords = std::string(name) + " is 0 or 1";
      };
      const auto seconds = [&](const char *name, long long &into)
      {
        const std::string text = queryValue(req.query, name);
        if (text.empty())
          return;
        try
        {
          std::size_t at = 0;
          const long long n = std::stoll(text, &at);
          if (at == text.size())
          {
            into = n;
            return;
          }
        }
        catch (const std::exception &)
        {
        }
        bad = true;
        badWords = std::string(name) + " must be whole unix seconds";
      };
      ask.limit = 50; // the page's own default, as it always was
      const std::string asked = queryValue(req.query, "limit");
      if (!asked.empty())
      {
        bool whole = false;
        long long n = 0;
        try
        {
          std::size_t at = 0;
          n = std::stoll(asked, &at);
          whole = at == asked.size();
        }
        catch (const std::exception &)
        {
        }
        if (!whole)
        {
          bad = true;
          badWords = "limit must be a whole number of rows";
        }
        else if (n < 1)
        {
          bad = true;
          badWords = "limit is at least one row";
        }
        else if (n > 200)
        {
          ask.limit = 200; // the row cap this door answers with
        }
        else
        {
          ask.limit = static_cast<std::size_t>(n);
        }
      }
      ask.afterOrEmpty = queryValue(req.query, "after");
      if (!ask.afterOrEmpty.empty())
      {
        bool hexWidth = ask.afterOrEmpty.size() == 40 || ask.afterOrEmpty.size() == 64;
        for (const char c : ask.afterOrEmpty)
        {
          const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
          if (!hex)
            hexWidth = false;
        }
        if (!hexWidth)
        {
          bad = true;
          badWords = "after is a full object id, 40 or 64 hex digits";
        }
      }
      ask.authorNeedle = queryValue(req.query, "author");
      ask.textNeedle = queryValue(req.query, "text");
      flag("first_parent", ask.firstParent);
      flag("no_merges", ask.noMerges);
      seconds("since", ask.sinceSecs);
      seconds("until", ask.untilSecs);
      if (bad)
      {
        res = projectDoorRefusal(400, badWords);
        return true;
      }
      res = codeDoorAnswer(repoDir, refName, "HEAD", [&](const std::string &commit, nlohmann::json &body, std::string &words)
                           {
        std::vector<ols::project::CommitInfo> commits;
        ols::project::HistoryTail tail;
        if (!ols::project::historyFor(repoDir, commit, ask, commits, tail))
        {
          words = ols::project::lastError();
          return false;
        }
        nlohmann::json rows = nlohmann::json::array();
        for (const auto &c : commits)
        {
          nlohmann::json row;
          row["oid"] = c.oid;
          row["authorName"] = c.authorName;
          row["authorWhen"] = c.authorWhen;
          row["messageFirstLine"] = c.messageFirstLine;
          row["parents"] = c.parents;
          row["refs"] = c.refs;
          row["committerName"] = c.committerName;
          row["committerWhen"] = c.committerWhen;
          row["message"] = c.message;
          row["messageTruncated"] = c.messageTruncated;
          row["stats"] = nlohmann::json{{"files", c.files},
                                        {"added", c.added},
                                        {"deleted", c.deleted}};
          nlohmann::json changes = nlohmann::json::array();
          for (const auto &ch : c.changed)
          {
            nlohmann::json crow;
            crow["path"] = ch.path;
            if (ch.counted)
            {
              crow["added"] = ch.added;
              crow["deleted"] = ch.deleted;
            }
            if (ch.binary)
              crow["binary"] = true;
            if (ch.large)
              crow["large"] = true;
            changes.push_back(crow);
          }
          row["changed"] = changes;
          row["changedTruncated"] = c.changedTruncated;
          rows.push_back(row);
        }
        body["commits"] = rows;
        if (!path.empty())
          body["path"] = path;
        body["hasMore"] = tail.hasMore;
        if (!tail.truncatedWhy.empty())
          body["truncatedWhy"] = tail.truncatedWhy;
        return true; });
      return true;
    }

    if (door == "blame")
    {
      if (!get)
      {
        res = projectDoorRefusal(405, "A blame is read with a GET");
        return true;
      }
      const std::string path = queryValue(req.query, "path");
      if (path.empty())
      {
        res = projectDoorRefusal(400, "Which file? This door names it with ?path=");
        return true;
      }
      res = codeDoorAnswer(repoDir, refName, "HEAD", [&](const std::string &commit, nlohmann::json &body, std::string &words)
                           {
        std::vector<ols::project::BlameLine> lines;
        if (!ols::project::blameFile(repoDir, commit, path, lines))
        {
          words = ols::project::lastError();
          return false;
        }
        nlohmann::json rows = nlohmann::json::array();
        bool truncated = false;
        for (const auto &line : lines)
        {
          rows.push_back(line.commitOid);
          if (line.truncated)
            truncated = true;
        }
        body["path"] = path;
        body["lines"] = rows;
        body["truncated"] = truncated;
        return true; });
      return true;
    }

    return false; // not a code door: routeProject answers as it knows how
  }

  // ===========================================================================
  // (1) The pulls and branches doors (spec_project_studio.md §5.6, §11).
  //
  // These doors answer everything under "pulls", "pulls/<n>", "pulls/<n>/
  // <action>", "branches", and "branches/...". The records themselves are
  // ProjectPulls' law and the merge belongs to gitcore/merge.h; what the
  // doors add is the ref reading a record cannot make for itself - the tips
  // a pull stands between, read fresh - and the house answer shape. The
  // branch law: a name must be one git stands (gitcore's isValidRefName)
  // and the repo must carry refs/heads/<name> with a commit resolving under
  // it; both tips of a pull must stand, at create and again at merge.
  //
  // (2) The issues doors stand beside them, under "issues", "issues.json",
  // and "issues/<id>(/action)"; the id shape is ProjectIssues' own law -
  // these doors pass an id through and let the module refuse a malformed
  // one in its words.
  //
  // The law these doors keep beside both modules: the record first and the
  // rev bump after, the order writeProjectFiles keeps (ProjectService.cpp:
  // 437-445). ProjectService's own mutex is file-static there and reaches
  // none of this, so these record doors hold one of their own across a
  // record write and its bump; the merge door holds it across the ref move
  // too, so the lease it checked is the lease it moved.
  // ===========================================================================

  // One process, one record-writer at a time - the pulls and issues doors
  // and the rev bump they carry beside the write.
  static std::mutex gRecordDoorsMutex;
  // The studio's event stream stands beside these doors: bumpStamp records
  // a moved stamp for it, and the route publishes it after this lock has
  // left. The state itself stands above streamToClient, where the door
  // beside the streams is written.

  // The project a record door names, and the folder of its repo beside it.
  // The slug arrives as a GET ?project= or a body "project" - the caller
  // reads it - and is weighed here by the same projectNamed law every studio
  // door uses; the repo folder is named only once the index carries the
  // slug, the way the commit door names it.
  static bool doorProject(ols::project::ProjectService *projects, const std::string &slug,
                          const char *missingWords, std::string &repoDir, HttpResponse &refusal)
  {
    const ProjectDoorService svc(projects);
    if (!projectNamed(svc, slug, missingWords, refusal))
      return false;
    repoDir = projects->repoDir(slug);
    if (repoDir.empty())
    {
      refusal = projectDoorRefusal(404, "No repo stands for project: " + slug);
      return false;
    }
    return true;
  }

  // The words a record module stated answer as a bad ask does, the way
  // refusalFromModule weighs the ledger's channel; no words at all is the
  // answer for a module that broke.
  static HttpResponse recordWords(const std::string &why, const ProjectDoorService &svc,
                                  const std::string &slug, int status)
  {
    if (why.empty())
      return actionAnswer(false, "The record module refused this ask and stated no reason",
                          svc, slug, 500);
    return actionAnswer(false, why, svc, slug, status);
  }

  // The record stood: move the change stamp, and say in the answer's words
  // if it would not move - the record stands either way, and the answer
  // must not claim a stamp that did not happen. The moved stamp is also
  // handed to the studio's event stream (§9.3): the frame is made here,
  // where the new stamp can be read, and published by the route after the
  // record-doors lock beside this call has left.
  static std::string bumpStamp(ols::project::ProjectService *projects, const std::string &slug)
  {
    if (!projects->bumpRev(slug))
      return "; the record stands but the change stamp did not move";
    // The new stamp, read back from the module's own revs document; a stamp
    // the machine does not state is not invented for the stream - the page
    // keeps its polling fallback beside a stream that stays silent.
    const nlohmann::json revs =
        nlohmann::json::parse(std::string(projects->projectRevsJson()), nullptr, false);
    long long stamp = -1;
    if (!revs.is_discarded() && revs.is_object())
    {
      if (revs.contains(slug) && revs.at(slug).is_number_integer())
        stamp = revs.at(slug).get<long long>();
      else if (revs.contains("revs") && revs.at("revs").is_object() &&
               revs.at("revs").contains(slug) && revs.at("revs").at(slug).is_number_integer())
        stamp = revs.at("revs").at(slug).get<long long>();
    }
    if (stamp >= 0)
      gPendingProjectEventFrames.push_back(
          "event: changed\ndata: " + nlohmann::json{{"slug", slug}, {"rev", stamp}}.dump() + "\n\n");
    return std::string();
  }

  // The approvals threshold a project asks of its pulls (spec §5.6):
  // pulls_threshold as its own facts state it, 1 when they state none.
  static long long doorPullsThreshold(const ProjectDoorService &svc, const std::string &slug)
  {
    const nlohmann::json facts = nlohmann::json::parse(svc.metaJson(slug), nullptr, false);
    if (!facts.is_discarded() && facts.contains("pulls_threshold") &&
        facts.at("pulls_threshold").is_number_integer())
      return facts.at("pulls_threshold").get<long long>();
    return 1;
  }

  // The project's default branch, out of its own facts - stated, never guessed.
  static std::string doorDefaultBranch(const ProjectDoorService &svc, const std::string &slug)
  {
    const nlohmann::json facts = nlohmann::json::parse(svc.metaJson(slug), nullptr, false);
    if (!facts.is_discarded())
      return statedString(facts, "default_branch");
    return std::string();
  }

  // A branch tip as these doors read one: the name must be one git stands
  // (refs.h's own rule), and the repo must carry refs/heads/<name> with a
  // commit resolving under it. False in words when either half says no.
  static bool doorBranchTip(const std::string &repoDir, const std::string &branch,
                            std::string &oidHex, std::string &words)
  {
    if (!ols::gitcore::isValidRefName("refs/heads/" + branch))
    {
      words = "that branch name is not one git stands: " + branch;
      return false;
    }
    std::vector<ols::project::RefInfo> refs;
    if (!ols::project::listRefs(repoDir, refs))
    {
      words = ols::project::lastError();
      return false;
    }
    const std::string want = "refs/heads/" + branch;
    for (const auto &info : refs)
    {
      if (info.name != want)
        continue;
      if (info.oid.empty())
      {
        words = "the branch named no commit it resolves to: " + branch;
        return false;
      }
      oidHex = info.oid;
      return true;
    }
    words = "no such branch in this repo: " + branch;
    return false;
  }

  // The <n> of a pulls/<n> door: whole digits, from 1 up. A shape that is
  // not digits is a bad ask; a number no folder wears is the record's own
  // refusal.
  static bool doorPullNumber(const std::string &text, long long &n)
  {
    if (text.empty() || text.size() > 15)
      return false;
    long long value = 0;
    for (char c : text)
    {
      if (c < '0' || c > '9')
        return false;
      value = value * 10 + (c - '0');
    }
    if (value < 1)
      return false;
    n = value;
    return true;
  }

  // The bounds of the tree walk below. A repo wearing more paths, or more of
  // them in a second and deeper commit to compare, is outside what this door
  // walks for one tag: the walk stops and the door makes no rename claim.
  constexpr size_t kIssuesTreeWalkFiles = 20000u;
  constexpr size_t kIssuesTreeWalkDirs = 4000u;
  constexpr size_t kIssuesLineMapBytes = 2u * 1024u * 1024u; // ProjectGit's own 2 MiB bound

  // Every blob path a commit's tree holds, with the oid it holds there, walked
  // level by level through treeAt (ProjectGit.h:133) the way the code door
  // walks one directory at a time. False - and nothing is claimed from the
  // half-walk - when a level will not read or a bound above is reached.
  static bool issuesListTree(const std::string &repoDir, const std::string &commitOid,
                             std::vector<ols::gitcore::ListingEntry> &out)
  {
    out.clear();
    std::vector<std::string> dirs;
    dirs.emplace_back(); // the root, which treeAt names with an empty path
    size_t walkedDirs = 0;
    while (!dirs.empty())
    {
      const std::string dir = dirs.back();
      dirs.pop_back();
      ++walkedDirs;
      std::vector<ols::project::TreeEntry> entries;
      if (!ols::project::treeAt(repoDir, commitOid, dir, entries))
        return false;
      for (const ols::project::TreeEntry &entry : entries)
      {
        if (entry.isDir)
        {
          if (walkedDirs + dirs.size() >= kIssuesTreeWalkDirs)
            return false;
          dirs.push_back(entry.path); // repo-relative, as treeAt gives it
        }
        else
        {
          if (out.size() >= kIssuesTreeWalkFiles)
            return false;
          out.push_back(ols::gitcore::ListingEntry{entry.path, entry.oid});
        }
      }
    }
    return true;
  }

  // Where the very bytes an issue tag was read at stand at the head now, when
  // the rename law can say. Both trees are listed and paired by gitcore's
  // phase one, detect_exact_renames (gitcore/diff.h:158), which pairs a path
  // that left with a path that arrived holding the same oid exactly. True with
  // movedTo named only for the pair whose from is this path and whose oid is
  // the tag's own thenOid: an arrival of other bytes is not this tag's file,
  // and the tag keeps its absent words. The pairing consumes one arrival per
  // departure and names the first, so moved_to is one path, never two.
  //
  // The pairing law already stands pinned, harness-free, at:
  //   tests/gitcore/test_issues.cpp:303 issue_reanchor_reused_path_follows_the_content_that_moved -
  //     the tag's path recreated with new bytes while its own bytes arrive at
  //     a new name; the follow names that name.
  //   tests/gitcore/test_issues.cpp:318 issue_reanchor_unchanged_path_never_pairs_with_its_own_copy -
  //     an unchanged path never pairs, so the unchanged claim is never touched.
  //   tests/gitcore/test_issues.cpp:329 and :344, the one-to-many and many-to-one
  //     cases - moved_to names one arrival, deterministically, and an arrival
  //     is never claimed twice.
  //   tests/gitcore/test_diff.cpp:305 exact_oid_matches_across_trees_pair_only_with_their_own_content -
  //     unchanged, deleted, and content-changed paths never pair.
  // tests/gitcore/test_resolutions.cpp pins the pull side-file law, not this
  // pairing; nothing there covers the re-anchor.
  static bool issuesFollowExactRename(const std::string &repoDir, const std::string &thenCommit,
                                      const std::string &headOid, const std::string &path,
                                      const std::string &thenOid, std::string &movedTo)
  {
    movedTo.clear();
    std::vector<ols::gitcore::ListingEntry> thenListing, headListing;
    if (!issuesListTree(repoDir, thenCommit, thenListing) ||
        !issuesListTree(repoDir, headOid, headListing))
      return false;
    std::string fromOid;
    for (const ols::gitcore::ListingEntry &left : thenListing)
      if (left.path == path)
      {
        fromOid = left.oid;
        break;
      }
    if (fromOid != thenOid)
      return false;
    for (const ols::gitcore::RenamePair &pair :
         ols::gitcore::detect_exact_renames(thenListing, headListing))
    {
      if (pair.from == path)
      {
        movedTo = pair.to;
        return true;
      }
    }
    return false;
  }

  // One line of the old blob read through a diff to the line it stands at in
  // the new one. A line outside every hunk moves by the net change of the
  // hunks before it. A line inside one is walked: an unchanged line maps to
  // the number it wears now; a deleted line stands nowhere, so false - the
  // door claims no line for a line the new blob does not hold. A line the old
  // blob never carried is refused the same way.
  static bool issuesMapLine(const ols::gitcore::DiffResult &diff, long long line,
                            long long &lineNow)
  {
    if (line < 1 || static_cast<size_t>(line) > diff.aLineCount)
      return false;
    long long delta = 0;
    for (const ols::gitcore::Hunk &hunk : diff.hunks)
    {
      if (hunk.countA == 0)
      {
        // git's own shape: a hunk wearing no old lines names the line before
        // it, so its added lines land after that line and shift what follows.
        if (line > hunk.startA)
          delta += hunk.countB;
        continue;
      }
      if (line < hunk.startA)
        break; // the hunks come in order
      if (line > hunk.startA + hunk.countA - 1)
      {
        delta += hunk.countB - hunk.countA;
        continue;
      }
      long long atA = hunk.startA, atB = hunk.startB;
      for (const ols::gitcore::HunkLine &row : hunk.lines)
      {
        if (row.kind == 'a')
        {
          ++atB;
          continue;
        }
        if (atA == line)
        {
          if (row.kind != 'c')
            return false; // the line was deleted: none stands for it
          lineNow = atB;
          return true;
        }
        ++atA;
        if (row.kind == 'c')
          ++atB;
      }
      return false; // the hunk did not hold the line the header promised
    }
    lineNow = line + delta;
    return true;
  }

  // The honest re-anchor of an issue's file tags against the head as it
  // stands now (spec §6): a tag whose bytes at the head are the very bytes
  // it was read at still claims its line; the same path wearing different
  // bytes has moved, and its line is claimed only where an exact diff maps it
  // on; a path gone from the head is named absent - or renamed, beside the
  // path its own bytes arrived at, when the pairing above can hold that claim.
  static void issuesAnchorNow(const std::string &repoDir, const nlohmann::json &record,
                              nlohmann::json &anchorNow)
  {
    anchorNow = nlohmann::json::array();
    if (!record.contains("files") || !record["files"].is_array())
      return;
    std::string headOid;
    const bool haveHead = ols::project::readHeadOid(repoDir, headOid);
    for (const nlohmann::json &tag : record["files"])
    {
      if (!tag.is_object())
        continue;
      const std::string path = statedString(tag, "path");
      if (path.empty())
        continue;
      nlohmann::json row;
      row["path"] = path;
      if (tag.contains("line") && tag["line"].is_number_integer())
        row["line"] = tag["line"];
      const std::string atCommit = statedString(tag, "commit");
      std::vector<std::uint8_t> bytes;
      std::string thenOid, nowOid;
      if (!haveHead || atCommit.empty() ||
          !ols::project::blobAt(repoDir, atCommit, path, bytes, thenOid))
      {
        row["state"] = "no-anchor-reading";
        row["moved"] = false; // nothing has moved that this reading could see
      }
      else
      {
        bytes.clear();
        if (!ols::project::blobAt(repoDir, headOid, path, bytes, nowOid))
        {
          // Gone from the head by path - the bytes may only have moved. The
          // pairing chases them once and names where they came to rest.
          std::string movedTo;
          if (issuesFollowExactRename(repoDir, atCommit, headOid, path, thenOid, movedTo))
          {
            row["state"] = "renamed";
            row["moved"] = true;
            row["moved_to"] = movedTo;
            // The bytes that moved are the bytes the line was read in, so the
            // line stands - at moved_to, not at the path the tag wears.
            if (tag.contains("line") && tag["line"].is_number_integer())
              row["line_stands"] = true;
          }
          else
          {
            row["state"] = "absent";
            row["moved"] = true;
          }
        }
        else if (nowOid == thenOid)
        {
          row["state"] = "unchanged";
          row["moved"] = false;
          row["line_stands"] = true; // the bytes the line was read in are the bytes that stand
        }
        else
        {
          row["state"] = "changed";
          row["moved"] = true;
          // The same path wearing other bytes: where the tag carries a line and
          // both blobs stand within the 2 MiB bound, an exact diff says which
          // line the old one is now. A fallback diff, a blob past the bound, or
          // a line the diff shows deleted or never there claims nothing at all,
          // as this branch has always claimed nothing.
          if (bytes.size() <= kIssuesLineMapBytes && tag.contains("line") &&
              tag["line"].is_number_integer())
          {
            std::vector<std::uint8_t> thenBytes;
            std::string thenBytesOid;
            if (ols::project::blobAt(repoDir, atCommit, path, thenBytes, thenBytesOid) &&
                thenBytes.size() <= kIssuesLineMapBytes)
            {
              const ols::gitcore::DiffResult diff = ols::gitcore::diff_blobs(
                  std::string(thenBytes.begin(), thenBytes.end()),
                  std::string(bytes.begin(), bytes.end()));
              long long lineNow = 0;
              if (!diff.approx && issuesMapLine(diff, tag["line"].get<long long>(), lineNow))
              {
                row["line_now"] = lineNow;
                row["line_exact"] = true;
              }
            }
          }
        }
      }
      anchorNow.push_back(std::move(row));
    }
  }

  // The pulls and branches doors. routeProject hands this everything under
  // /project/api/; this block answers what its names cover and says false
  // for every other door. True always means res stands.
  static bool routeProjectPullsDoors(ols::project::ProjectService *projects,
                                     const HttpRequest &req, HttpResponse &res)
  {
    const std::string door = req.path.substr(kProjectApiPrefixLen);
    const bool isPulls = door == "pulls" || door.compare(0, 6, "pulls/") == 0;
    const bool isBranches = door == "branches" || door.compare(0, 9, "branches/") == 0;
    if (!isPulls && !isBranches)
      return false;
    const bool get = req.method == "GET" || req.method == "HEAD";
    const bool post = req.method == "POST";
    const ProjectDoorService svc(projects);

    // GET pulls?project=&state=&limit= - the list, newest first, with the
    // threshold the project's own facts ask for.
    if (isPulls && door == "pulls" && get)
    {
      const std::string slug = queryValue(req.query, "project");
      std::string repoDir;
      HttpResponse refusal;
      if (!doorProject(projects, slug,
                       "Which project? Its pulls are read with a GET: send ?project=<slug>", repoDir,
                       refusal))
      {
        res = refusal;
        return true;
      }
      const std::string state = queryValue(req.query, "state");
      if (!state.empty() && state != "open" && state != "merged" && state != "closed")
      {
        res = projectDoorRefusal(400, "The state filter is open, merged, closed - or nothing at all");
        return true;
      }
      nlohmann::json rows, answer;
      std::string error;
      if (!ols::pulls::listPulls(repoDir, state, queryInt(req.query, "limit", 50),
                                 doorPullsThreshold(svc, slug), rows, answer, error))
      {
        res = projectDoorRefusal(500, error.empty() ? "the pull records could not be listed" : error);
        return true;
      }
      putRev(answer, svc, slug);
      res = projectDoorAnswer(200, answer);
      return true;
    }

    // POST pulls {project,head,base?,title,body?} - open a pull. Both tips
    // must stand now; what the record refuses about them, the module says.
    if (isPulls && door == "pulls" && post)
    {
      nlohmann::json given;
      if (!bodyObject(req, given))
      {
        res = projectDoorRefusal(400, "Invalid request body");
        return true;
      }
      const std::string slug = statedString(given, "project");
      std::string repoDir;
      HttpResponse refusal;
      if (!doorProject(projects, slug, "Nothing to open: the ask names no project", repoDir, refusal))
      {
        res = refusal;
        return true;
      }
      const std::string head = statedString(given, "head");
      if (head.empty())
      {
        res = projectDoorRefusal(400, "A pull is opened between two branches: send \"head\"");
        return true;
      }
      std::string base = statedString(given, "base");
      if (base.empty())
        base = doorDefaultBranch(svc, slug);
      if (base.empty())
      {
        res = projectDoorRefusal(400,
                                 "The pull has no base to land in: send \"base\", or give the project a default branch");
        return true;
      }
      if (!plainWords(head) || !plainWords(base))
      {
        res = projectDoorRefusal(400, "That branch name carries characters this machine will not take");
        return true;
      }
      std::string headOid, baseOid, words;
      if (!doorBranchTip(repoDir, head, headOid, words))
      {
        res = projectDoorRefusal(404, words);
        return true;
      }
      if (!doorBranchTip(repoDir, base, baseOid, words))
      {
        res = projectDoorRefusal(404, words);
        return true;
      }
      std::lock_guard<std::mutex> hold(gRecordDoorsMutex);
      nlohmann::json out;
      std::string error;
      if (!ols::pulls::createPull(repoDir, slug, head, base, statedString(given, "title"),
                                  statedString(given, "body"), headOid, baseOid, out, error))
      {
        res = recordWords(error, svc, slug, 400);
        return true;
      }
      std::string okWords = "Opened pull " + std::to_string(statedCount(out, "number")) +
                            " of project " + slug + bumpStamp(projects, slug);
      nlohmann::json body;
      body["did"] = true;
      body["words"] = okWords;
      body["pull"] = out;
      putRev(body, svc, slug);
      res = projectDoorAnswer(200, body);
      return true;
    }

    // The numbered pull doors: pulls/<n> reads one, pulls/<n>/<action>
    // works it.
    if (isPulls && door.compare(0, 6, "pulls/") == 0)
    {
      std::string number, action;
      const std::string rest = door.substr(6);
      const size_t slash = rest.find('/');
      if (slash != std::string::npos)
      {
        number = rest.substr(0, slash);
        action = rest.substr(slash + 1);
        if (action.find('/') != std::string::npos)
        {
          res = projectDoorRefusal(400, "The pull door is pulls/<n> or pulls/<n>/<action>");
          return true;
        }
      }
      else
        number = rest;
      long long n = 0;
      if (!doorPullNumber(number, n))
      {
        res = projectDoorRefusal(400, "A pull is numbered with whole digits: pulls/<n>");
        return true;
      }
      std::string slug;
      nlohmann::json given;
      if (get)
        slug = queryValue(req.query, "project");
      else
      {
        if (!bodyObject(req, given))
        {
          res = projectDoorRefusal(400, "Invalid request body");
          return true;
        }
        slug = statedString(given, "project");
      }
      std::string repoDir;
      HttpResponse refusal;
      if (!doorProject(projects, slug,
                       get ? "Which project? Send ?project=<slug>" : "Which project? The ask names none",
                       repoDir, refusal))
      {
        res = refusal;
        return true;
      }
      std::string error, okWords;
      nlohmann::json out;

      if (action.empty())
      {
        if (!get)
        {
          res = projectDoorRefusal(405,
                                   "A pull is read with a GET; its actions are named after the number");
          return true;
        }
        nlohmann::json record;
        if (!ols::pulls::readRecord(repoDir, n, record, error))
        {
          res = projectDoorRefusal(404, error.empty() ? "the pull record could not be read" : error);
          return true;
        }
        // head_now/base_now: the tips as they stand at this answer, and the
        // honest word on whether they moved since the pull opened. A branch
        // taken away is named gone - that is not the same thing as a move,
        // and the answer keeps the difference.
        nlohmann::json answer;
        answer["pull"] = record;
        bool moved = false;
        for (const char *side : {"head", "base"})
        {
          nlohmann::json now = nlohmann::json::object();
          std::string branch, openedAt;
          if (record.contains(side) && record[side].is_object())
          {
            branch = statedString(record[side], "branch");
            openedAt = statedString(record[side], "oidAtOpen");
          }
          now["branch"] = branch;
          std::string oid, words;
          if (!branch.empty() && doorBranchTip(repoDir, branch, oid, words))
          {
            now["oid"] = oid;
            if (openedAt != oid)
              moved = true;
          }
          else
            now["gone"] = true;
          answer[std::string(side) + "_now"] = now;
        }
        answer["moved_since_open"] = moved;
        putRev(answer, svc, slug);
        res = projectDoorAnswer(200, answer);
        return true;
      }

      if (!post)
      {
        res = projectDoorRefusal(405, postOnlyDoorWords());
        return true;
      }
      std::string who = statedString(given, "by");
      if (who.empty())
        who = "webui";
      const std::string bodyText = statedString(given, "body");

      if (action == "approve" || action == "request-changes" || action == "comment")
      {
        std::lock_guard<std::mutex> hold(gRecordDoorsMutex);
        bool done = false;
        if (action == "comment")
        {
          const nlohmann::json anchor =
              given.contains("anchor") ? given.at("anchor") : nlohmann::json(nullptr);
          done = ols::pulls::addComment(repoDir, n, who, bodyText, anchor, out, error);
        }
        else
        {
          const ols::pulls::ReviewVerdict verdict =
              action == "approve" ? ols::pulls::ReviewVerdict::Approve
                                  : ols::pulls::ReviewVerdict::RequestChanges;
          done = ols::pulls::addReview(repoDir, n, who, verdict, bodyText, out, error);
        }
        if (!done)
        {
          if (out.is_object() && out.contains("truncated"))
          {
            // The 200-comment cut: the record stands as it stands, and the
            // answer says so with the record's own flag beside the words.
            nlohmann::json b;
            b["did"] = false;
            b["words"] = error;
            b["truncated"] = true;
            putRev(b, svc, slug);
            res = projectDoorAnswer(400, b);
            return true;
          }
          res = recordWords(error, svc, slug, 400);
          return true;
        }
        okWords = action == "approve"    ? "Approved pull " + std::to_string(n)
                  : action == "request-changes"
                      ? "Asked for changes on pull " + std::to_string(n)
                      : "Commented on pull " + std::to_string(n);
        okWords += bumpStamp(projects, slug);
        res = actionAnswer(true, okWords, svc, slug, 200);
        return true;
      }

      if (action == "close" || action == "reopen")
      {
        std::lock_guard<std::mutex> hold(gRecordDoorsMutex);
        const bool done = action == "close" ? ols::pulls::closePull(repoDir, n, out, error)
                                            : ols::pulls::reopenPull(repoDir, n, out, error);
        if (!done)
        {
          res = recordWords(error, svc, slug, 400);
          return true;
        }
        okWords = std::string(action == "close" ? "Closed pull " : "Reopened pull ") +
                  std::to_string(n) + bumpStamp(projects, slug);
        res = actionAnswer(true, okWords, svc, slug, 200);
        return true;
      }

      if (action == "resolve")
      {
        const std::string file = statedString(given, "file");
        if (file.empty())
        {
          res = projectDoorRefusal(400, "Nothing to resolve: the ask names no file");
          return true;
        }
        const auto delIt = given.find("delete");
        const bool deleteFile = delIt != given.end() && delIt->is_boolean() && delIt->get<bool>();
        if (!deleteFile && !given.contains("content"))
        {
          res = projectDoorRefusal(
              400, "A resolution settles with content or takes the file away: send \"content\" or \"delete\"");
          return true;
        }
        const std::string content = deleteFile ? std::string() : statedString(given, "content");
        // The stamps of the side file are the module's law - a first file
        // stamps from the record's lease, and stamps once kept are kept -
        // and the merge door weighs them against the live tips.
        std::lock_guard<std::mutex> hold(gRecordDoorsMutex);
        if (!ols::pulls::setResolution(repoDir, n, file, content, deleteFile, who, out, error))
        {
          res = recordWords(error, svc, slug, 400);
          return true;
        }
        okWords = (deleteFile ? "Took " + file + " away from pull " : "Settled " + file + " on pull ") +
                  std::to_string(n) + bumpStamp(projects, slug);
        res = actionAnswer(true, okWords, svc, slug, 200);
        return true;
      }

      if (action == "merge")
      {
        std::string styleWord = "merge";
        if (given.contains("style"))
        {
          if (!given.at("style").is_string())
          {
            res = projectDoorRefusal(400, "'style' must be a word: merge, squash, or fast-forward");
            return true;
          }
          styleWord = given.at("style").get<std::string>();
        }
        ols::gitcore::MergeStyle style = ols::gitcore::MergeStyle::MergeCommit;
        if (styleWord == "fast-forward")
          style = ols::gitcore::MergeStyle::FastForward;
        else if (styleWord == "squash")
          style = ols::gitcore::MergeStyle::Squash;
        else if (styleWord != "merge")
        {
          res = projectDoorRefusal(400, "The merge style is merge, squash, or fast-forward");
          return true;
        }
        std::string merger = statedString(given, "by");
        if (merger.empty())
          merger = "studio";

        std::lock_guard<std::mutex> hold(gRecordDoorsMutex);
        nlohmann::json record;
        if (!ols::pulls::readRecord(repoDir, n, record, error))
        {
          res = recordWords(error, svc, slug, 404);
          return true;
        }
        const std::string state = statedString(record, "state");
        if (state != "open")
        {
          res = recordWords(state.empty() ? "this record states no state the machine knows"
                                          : "this pull is " + state + " and takes no merge",
                            svc, slug, 400);
          return true;
        }
        std::string headBranch, baseBranch;
        if (record.contains("head") && record["head"].is_object())
          headBranch = statedString(record["head"], "branch");
        if (record.contains("base") && record["base"].is_object())
          baseBranch = statedString(record["base"], "branch");
        if (headBranch.empty() || baseBranch.empty())
        {
          res = recordWords("this record names no head and base branch to merge between", svc, slug,
                            400);
          return true;
        }
        // Both tips read now: the merge works on what stands, not on what
        // the record was opened with.
        std::string headNow, baseNow, words;
        if (!doorBranchTip(repoDir, headBranch, headNow, words))
        {
          res = projectDoorRefusal(404, words);
          return true;
        }
        if (!doorBranchTip(repoDir, baseBranch, baseNow, words))
        {
          res = projectDoorRefusal(404, words);
          return true;
        }
        // The settled paths, if any stand: their stamps must name the very
        // tips the merge is built on, or they were written against a view
        // that is no more.
        nlohmann::json held;
        if (!ols::pulls::readResolutions(repoDir, n, held, error))
        {
          res = recordWords(error, svc, slug, 400);
          return true;
        }
        std::vector<ols::gitcore::MergeResolution> resolutions;
        bool withResolutions = false;
        if (held.is_object() && held.contains("items") && held["items"].is_array() &&
            !held["items"].empty())
        {
          std::string atHead, atBase;
          if (held.contains("at") && held["at"].is_object())
          {
            atHead = statedString(held["at"], "head");
            atBase = statedString(held["at"], "base");
          }
          if (atHead != headNow || atBase != baseNow)
          {
            res = actionAnswer(
                false,
                "the branch moved under this pull since the resolutions: read the conflicts again", svc,
                slug, 409);
            return true;
          }
          for (const nlohmann::json &item : held["items"])
          {
            if (!item.is_object() || !item.contains("path") || !item["path"].is_string())
            {
              res = recordWords("an item of the resolutions is not an item: it carries no path", svc,
                                slug, 400);
              return true;
            }
            ols::gitcore::MergeResolution settled;
            settled.path = item["path"].get<std::string>();
            const std::string act = statedString(item, "action");
            if (act == "delete")
              settled.deleteFile = true;
            else if (act == "settle")
            {
              if (!item.contains("content") || !item["content"].is_string())
              {
                res = recordWords("an item that settles carries no content", svc, slug, 400);
                return true;
              }
              settled.content = item["content"].get<std::string>();
            }
            else
            {
              res = recordWords(
                  "an item of the resolutions wears an action that is not settle or delete", svc,
                  slug, 400);
              return true;
            }
            resolutions.push_back(std::move(settled));
          }
          withResolutions = true;
        }
        // The commit this merge makes speaks of the pull it carries: the door
        // states the pull's own line - the number, the head it stands under,
        // and the title the record states - and the engine carries its own
        // words beneath it. A title that carries a fold is read as its first
        // line.
        std::string subject = "Merge pull request #" + std::to_string(n) + " from " + headBranch;
        std::string title = statedString(record, "title");
        const size_t fold = title.find_first_of("\r\n");
        if (fold != std::string::npos)
          title.erase(fold);
        if (!title.empty())
          subject += ": " + title;
        ols::git::RepoPort port(repoDir);
        ols::gitcore::MergeOutcome outcome;
        const bool merged =
            withResolutions
                ? ols::gitcore::mergePull(port, baseNow, headNow, style, resolutions, outcome, error,
                                          subject)
                : ols::gitcore::mergePull(port, baseNow, headNow, style, outcome, error, subject);
        if (!merged)
        {
          res = recordWords(error, svc, slug, 400);
          return true;
        }
        if (!outcome.clean)
        {
          // A conflict is an answer, not a refusal: the paths it names, the
          // three sides each one carries, and the merge-base they were read
          // against. The sides' texts ride while a door budget holds them,
          // and a cut is said; resolutions the merge never needed are named
          // as well.
          nlohmann::json rows = nlohmann::json::array();
          std::size_t textBudget = 4u * 1024u * 1024u;
          bool textsCut = false;
          for (const auto &c : outcome.conflicts)
          {
            nlohmann::json row;
            row["path"] = c.path;
            row["kind"] = c.kind;
            row["base"] = c.baseOid;
            row["ours"] = c.oursOid;
            row["theirs"] = c.theirsOid;
            auto carry = [&row, &textBudget, &textsCut](const char *key, const std::string &text)
            {
              if (text.empty())
                return;
              if (text.size() > textBudget)
              {
                textsCut = true;
                return;
              }
              textBudget -= text.size();
              row[key] = text;
            };
            carry("base_text", c.baseText);
            carry("ours_text", c.oursText);
            carry("theirs_text", c.theirsText);
            rows.push_back(std::move(row));
          }
          nlohmann::json body;
          body["did"] = false;
          body["conflict"] = true;
          // What stands between this ask and a commit, said once plainly beside
          // the rows. A cut answer does not know the whole count, so it states none.
          if (outcome.truncated)
            body["words"] =
                "these paths stand in the way and the answer is cut short of all of them: settle what is named and press merge again";
          else if (outcome.conflicts.size() == 1u)
            body["words"] = "one path stands in the way: settle it and press merge again";
          else
            body["words"] = std::to_string(outcome.conflicts.size()) +
                            " paths stand in the way: settle them and press merge again";
          body["conflicts"] = rows;
          body["merge_base"] = outcome.baseCommitOid;
          if (outcome.truncated)
            body["conflicts_truncated"] = true;
          if (textsCut)
            body["texts_truncated"] = true;
          if (outcome.multipleBases)
            body["multiple_bases"] = true;
          if (!outcome.unused.empty())
            body["unused"] = outcome.unused;
          putRev(body, svc, slug);
          res = projectDoorAnswer(200, body);
          return true;
        }
        if (outcome.mergedCommitOid.empty())
        {
          // Clean with nothing to move: the tree the engine compared is the
          // target's own. Nothing was written and nothing moved, and the
          // pull stays open - marking it merged would claim a commit that
          // was never made.
          res = actionAnswer(
              false,
              "the merge brought nothing new: the head carries no tree the base does not already hold",
              svc, slug, 400);
          return true;
        }
        // One commitRefs move with the tip read now as the expectation - the
        // lease law of refs.h - and the reflog entry beside it, dressed the
        // way ProjectGit dresses its own moves.
        ols::gitcore::Repository repo(repoDir);
        ols::gitcore::RefUpdate update;
        update.name = "refs/heads/" + baseBranch;
        update.newOid = ols::gitcore::ObjectId(outcome.mergedCommitOid);
        update.expectedOldOid = ols::gitcore::ObjectId(baseNow);
        std::vector<ols::gitcore::RefUpdate> updates;
        updates.push_back(update);
        if (!repo.commitRefs(updates))
        {
          res = actionAnswer(false,
                             "the base branch moved under the merge: it no longer holds the tip the merge was built on - read the branch again",
                             svc, slug, 409);
          return true;
        }
        // A log that would not take the line does not unsay the merge - the
        // branch moved - so it is named beside a true answer, the way a walk
        // at its bound names the cut.
        ols::gitcore::ReflogEntry entry;
        entry.oldOid = baseNow;
        entry.newOid = outcome.mergedCommitOid;
        entry.who = merger + " <>";
        entry.when = std::to_string(nowSeconds()) + " +0000";
        entry.msg = "merge: pull " + std::to_string(n) + " (" + styleWord + ") into " + baseBranch;
        const bool logStood = repo.reflogAppend(update.name, entry);
        nlohmann::json marked;
        if (!ols::pulls::markMerged(repoDir, n, styleWord, outcome.mergedCommitOid, merger, marked,
                                    error))
        {
          // The branch moved and the record did not: say exactly that, with
          // where the branch stands now, rather than hide either half.
          nlohmann::json body;
          body["did"] = false;
          body["words"] =
              "the base branch stands at the merged commit; the pull record would not take the merge mark: " + error;
          body["branch"] = baseBranch;
          body["commit"] = outcome.mergedCommitOid;
          putRev(body, svc, slug);
          res = projectDoorAnswer(500, body);
          return true;
        }
        nlohmann::json cleared;
        if (!ols::pulls::clearResolutions(repoDir, n, cleared, error) && !error.empty())
          okWords = "; the merge stands but the resolutions file would not clear: " + error;
        else
          okWords.clear();
        std::string mergeWords =
            "Merged pull " + std::to_string(n) + " into " + baseBranch + " (" + styleWord + ")";
        if (!logStood)
          mergeWords += "; the reflog entry for the branch could not be written";
        mergeWords += okWords + bumpStamp(projects, slug);
        nlohmann::json body;
        body["did"] = true;
        body["words"] = mergeWords;
        if (marked.contains("merge"))
          body["merge"] = marked["merge"];
        body["base"] = outcome.baseCommitOid;
        body["style"] = styleWord;
        if (!outcome.took.empty())
          body["took"] = outcome.took;
        if (!outcome.unused.empty())
          body["unused"] = outcome.unused;
        putRev(body, svc, slug);
        res = projectDoorAnswer(200, body);
        return true;
      }

      res = projectDoorRefusal(
          404, "the action on a pull is approve, request-changes, comment, merge, resolve, close, or reopen");
      return true;
    }

    // ------------------------------------------------------------ branches
    // GET branches?project= - the rows, the default branch marked protected,
    // and the open pulls each head carries.
    if (isBranches && door == "branches" && get)
    {
      const std::string slug = queryValue(req.query, "project");
      std::string repoDir;
      HttpResponse refusal;
      if (!doorProject(projects, slug,
                       "Which project? Its branches are read with a GET: send ?project=<slug>", repoDir,
                       refusal))
      {
        res = refusal;
        return true;
      }
      std::vector<ols::project::RefInfo> refs;
      if (!ols::project::listRefs(repoDir, refs))
      {
        const std::string why = ols::project::lastError();
        res = projectDoorRefusal(404, why.empty() ? "the refs could not be read" : why);
        return true;
      }
      const std::string def = doorDefaultBranch(svc, slug);
      // The open pulls, read once, counted per head branch. A list that ran
      // out at its cap is said - the counts would understate.
      nlohmann::json openRows, openAnswer;
      std::string pullWords;
      const bool pullsStood = ols::pulls::listPulls(repoDir, "open", 200,
                                                    doorPullsThreshold(svc, slug), openRows,
                                                    openAnswer, pullWords);
      nlohmann::json rows = nlohmann::json::array();
      for (const auto &info : refs)
      {
        if (info.name.compare(0, 11, "refs/heads/") != 0)
          continue;
        const std::string shortName = info.name.substr(11);
        nlohmann::json row;
        row["name"] = shortName;
        row["oid"] = info.oid;
        row["protected"] = shortName == def;
        if (pullsStood && openAnswer.contains("rows"))
        {
          long long openCount = 0;
          for (const nlohmann::json &pr : openAnswer["rows"])
            if (statedString(pr, "head") == shortName)
              ++openCount;
          row["open_pr"] = openCount;
        }
        rows.push_back(std::move(row));
      }
      nlohmann::json answer;
      answer["branches"] = rows;
      if (!def.empty())
        answer["default"] = def;
      if (pullsStood && openAnswer.contains("hasMore") && openAnswer["hasMore"].is_boolean() &&
          openAnswer["hasMore"].get<bool>())
        answer["open_pr_truncated"] = true;
      if (!pullsStood)
        answer["open_pr_note"] =
            pullWords.empty() ? "the pull records could not be read" : pullWords;
      putRev(answer, svc, slug);
      res = projectDoorAnswer(200, answer);
      return true;
    }

    // POST branches/new {project,branch,start?} - a branch at a commit the
    // repo holds, through the git engine's own creation.
    if (isBranches && door == "branches/new" && post)
    {
      nlohmann::json given;
      if (!bodyObject(req, given))
      {
        res = projectDoorRefusal(400, "Invalid request body");
        return true;
      }
      const std::string slug = statedString(given, "project");
      std::string repoDir;
      HttpResponse refusal;
      if (!doorProject(projects, slug, "Nothing to stand up: the ask names no project", repoDir,
                       refusal))
      {
        res = refusal;
        return true;
      }
      const std::string branch = statedString(given, "branch");
      if (branch.empty())
      {
        res = projectDoorRefusal(400, "Nothing to stand up: the ask names no branch");
        return true;
      }
      if (!plainWords(branch))
      {
        res = projectDoorRefusal(400, "That branch name carries characters this machine will not take");
        return true;
      }
      std::string start = statedString(given, "start");
      if (start.empty())
        start = doorDefaultBranch(svc, slug);
      if (start.empty())
      {
        res = projectDoorRefusal(
            400,
            "Nothing to start the branch from: send \"start\", or give the project a default branch");
        return true;
      }
      std::string startOid, words;
      if (!codeResolveRef(repoDir, start, startOid, words))
      {
        res = projectDoorRefusal(404, words);
        return true;
      }
      if (!ols::project::createBranch(repoDir, branch, startOid))
      {
        res = recordWords(ols::project::lastError(), svc, slug, 400);
        return true;
      }
      const std::string okWords = "Stood up branch " + branch + " of project " + slug +
                                  bumpStamp(projects, slug);
      res = actionAnswer(true, okWords, svc, slug, 200);
      return true;
    }

    // POST branches/delete {project,branch} - taken away through the same
    // engine; the branch HEAD points at is refused there, in its words.
    if (isBranches && door == "branches/delete" && post)
    {
      nlohmann::json given;
      if (!bodyObject(req, given))
      {
        res = projectDoorRefusal(400, "Invalid request body");
        return true;
      }
      const std::string slug = statedString(given, "project");
      std::string repoDir;
      HttpResponse refusal;
      if (!doorProject(projects, slug, "Nothing to take away: the ask names no project", repoDir,
                       refusal))
      {
        res = refusal;
        return true;
      }
      const std::string branch = statedString(given, "branch");
      if (branch.empty())
      {
        res = projectDoorRefusal(400, "Nothing to take away: the ask names no branch");
        return true;
      }
      if (!plainWords(branch))
      {
        res = projectDoorRefusal(400, "That branch name carries characters this machine will not take");
        return true;
      }
      if (!ols::project::deleteBranch(repoDir, branch))
      {
        res = recordWords(ols::project::lastError(), svc, slug, 400);
        return true;
      }
      const std::string okWords =
          "Took branch " + branch + " away from project " + slug + bumpStamp(projects, slug);
      res = actionAnswer(true, okWords, svc, slug, 200);
      return true;
    }

    // One of these names in the wrong shape or the wrong way.
    if (isBranches)
    {
      if (door == "branches" || door == "branches/new" || door == "branches/delete")
        res = projectDoorRefusal(405, "Branches are read with a GET and worked with a POST");
      else
        res = projectDoorRefusal(404, "No such branch door.");
      return true;
    }
    if (door == "pulls")
      res = projectDoorRefusal(405,
                               "The pulls are read with a GET and a new pull is opened with a POST");
    else
      res = projectDoorRefusal(405, postOnlyDoorWords());
    return true;
  }

  // (2) The issues doors: the list, the machine view, the record beside its
  // honest re-anchor, and the six working doors. True means res stands; the
  // id shape is ProjectIssues' law - passed through, refused there in words.
  static bool routeProjectIssuesDoors(ols::project::ProjectService *projects,
                                      const HttpRequest &req, HttpResponse &res)
  {
    const std::string door = req.path.substr(kProjectApiPrefixLen);
    const bool plain = door == "issues";
    const bool compact = door == "issues.json";
    const bool item = door.compare(0, 7, "issues/") == 0;
    if (!plain && !compact && !item)
      return false;
    const bool get = req.method == "GET" || req.method == "HEAD";
    const bool post = req.method == "POST";
    const ProjectDoorService svc(projects);

    if ((plain || compact) && get)
    {
      const std::string slug = queryValue(req.query, "project");
      std::string repoDir;
      HttpResponse refusal;
      if (!doorProject(projects, slug,
                       "Which project? Its issues are read with a GET: send ?project=<slug>", repoDir,
                       refusal))
      {
        res = refusal;
        return true;
      }
      const std::string file = queryValue(req.query, "file");
      const std::string state = queryValue(req.query, "state");
      if (!state.empty() && state != "open" && state != "closed")
      {
        res = projectDoorRefusal(400, "The state filter is open, closed - or nothing at all");
        return true;
      }
      nlohmann::json rows, answer;
      std::string error;
      if (!ols::issues::listIssues(repoDir, file, state, queryInt(req.query, "limit", 50), rows,
                                   answer, error))
      {
        res = projectDoorRefusal(500, error.empty() ? "the issue records could not be listed" : error);
        return true;
      }
      if (compact)
      {
        // issues.json: one compact row per issue with its file tags as
        // plain paths - the machine view for agents (spec §6, §13). A row
        // whose record this reading cannot take is named, not guessed
        // around.
        nlohmann::json lines = nlohmann::json::array();
        nlohmann::json unreadable = nlohmann::json::array();
        for (const nlohmann::json &row : rows)
        {
          const std::string id = statedString(row, "id");
          nlohmann::json rec;
          if (!ols::issues::readRecord(repoDir, id, rec, error))
          {
            unreadable.push_back(id);
            continue;
          }
          nlohmann::json line;
          line["id"] = id;
          line["state"] = statedString(row, "state");
          line["severity"] = statedString(row, "severity");
          line["title"] = statedString(row, "title");
          nlohmann::json tags = nlohmann::json::array();
          if (rec.contains("files") && rec["files"].is_array())
            for (const nlohmann::json &tag : rec["files"])
              if (tag.is_object() && !statedString(tag, "path").empty())
                tags.push_back(statedString(tag, "path"));
          line["files"] = tags;
          lines.push_back(std::move(line));
        }
        nlohmann::json body;
        body["issues"] = lines;
        if (answer.contains("hasMore"))
          body["hasMore"] = answer["hasMore"];
        if (answer.contains("unparseable"))
          body["unparseable"] = answer["unparseable"];
        if (!unreadable.empty())
          body["unreadable"] = unreadable;
        putRev(body, svc, slug);
        res = projectDoorAnswer(200, body);
        return true;
      }
      putRev(answer, svc, slug);
      res = projectDoorAnswer(200, answer);
      return true;
    }

    if (plain && post)
    {
      nlohmann::json given;
      if (!bodyObject(req, given))
      {
        res = projectDoorRefusal(400, "Invalid request body");
        return true;
      }
      const std::string slug = statedString(given, "project");
      std::string repoDir;
      HttpResponse refusal;
      if (!doorProject(projects, slug, "Nothing to open: the ask names no project", repoDir, refusal))
      {
        res = refusal;
        return true;
      }
      nlohmann::json filesJson =
          given.contains("files") ? given.at("files") : nlohmann::json(nullptr);
      // A tag is the place an issue was seen at, and the honest re-anchor reads
      // the bytes it was read against: a tag that arrives wearing no commit is
      // stamped here at the head as it stands now, the commit these very bytes
      // were opened against. An unborn repo states no head - readHeadOid says
      // so - and its tags stay unstamped, weighing nothing, as they always did.
      if (filesJson.is_array())
      {
        std::string headNow;
        if (ols::project::readHeadOid(repoDir, headNow))
        {
          for (nlohmann::json &tag : filesJson)
          {
            if (tag.is_object() && statedString(tag, "commit").empty())
              tag["commit"] = headNow;
          }
        }
      }
      const nlohmann::json labelsJson =
          given.contains("labels") ? given.at("labels") : nlohmann::json(nullptr);
      std::lock_guard<std::mutex> hold(gRecordDoorsMutex);
      nlohmann::json out;
      std::string error;
      if (!ols::issues::createIssue(repoDir, slug, statedString(given, "title"),
                                    statedString(given, "body"), statedString(given, "severity"),
                                    filesJson, labelsJson, out, error))
      {
        res = recordWords(error, svc, slug, 400);
        return true;
      }
      const std::string okWords = "Opened issue " + statedString(out, "id") + " of project " + slug +
                                  bumpStamp(projects, slug);
      nlohmann::json body;
      body["did"] = true;
      body["words"] = okWords;
      body["id"] = statedString(out, "id");
      putRev(body, svc, slug);
      res = projectDoorAnswer(200, body);
      return true;
    }

    if (item)
    {
      std::string id, action;
      const std::string rest = door.substr(7);
      const size_t slash = rest.find('/');
      if (slash != std::string::npos)
      {
        id = rest.substr(0, slash);
        action = rest.substr(slash + 1);
        if (action.find('/') != std::string::npos)
        {
          res = projectDoorRefusal(400, "The issue door is issues/<id> or issues/<id>/<action>");
          return true;
        }
      }
      else
        id = rest;
      std::string slug;
      nlohmann::json given;
      if (get)
        slug = queryValue(req.query, "project");
      else
      {
        if (!bodyObject(req, given))
        {
          res = projectDoorRefusal(400, "Invalid request body");
          return true;
        }
        slug = statedString(given, "project");
      }
      std::string repoDir;
      HttpResponse refusal;
      if (!doorProject(projects, slug,
                       get ? "Which project? Send ?project=<slug>" : "Which project? The ask names none",
                       repoDir, refusal))
      {
        res = refusal;
        return true;
      }
      std::string error, okWords;
      nlohmann::json out;

      if (action.empty())
      {
        if (!get)
        {
          res = projectDoorRefusal(405,
                                   "An issue is read with a GET; its actions are named after the id");
          return true;
        }
        nlohmann::json record;
        if (!ols::issues::readRecord(repoDir, id, record, error))
        {
          res = projectDoorRefusal(404, error.empty() ? "the issue record could not be read" : error);
          return true;
        }
        nlohmann::json anchorNow;
        issuesAnchorNow(repoDir, record, anchorNow);
        nlohmann::json answer;
        answer["issue"] = record;
        answer["anchor_now"] = anchorNow;
        putRev(answer, svc, slug);
        res = projectDoorAnswer(200, answer);
        return true;
      }

      if (!post)
      {
        res = projectDoorRefusal(405, postOnlyDoorWords());
        return true;
      }
      std::string who = statedString(given, "by");
      if (who.empty())
        who = "webui";

      if (action == "edit")
      {
        if (!given.contains("patch") || !given.at("patch").is_object())
        {
          res = projectDoorRefusal(400, "Nothing to change: the ask states no patch of fields");
          return true;
        }
        std::lock_guard<std::mutex> hold(gRecordDoorsMutex);
        if (!ols::issues::editIssue(repoDir, id, given.at("patch"), out, error))
        {
          res = recordWords(error, svc, slug, 400);
          return true;
        }
        okWords = "Edited issue " + id + bumpStamp(projects, slug);
      }
      else if (action == "comments")
      {
        nlohmann::json anchor =
            given.contains("anchor") ? given.at("anchor") : nlohmann::json(nullptr);
        // The opening door's law for its tags, kept here for the place a
        // comment stands: an anchor naming a path with no commit is stamped at
        // the head as it stands now - the commit those bytes stood at when the
        // comment was written. A comment with no anchor is untouched, and an
        // unborn repo states no head, so an anchor stays as it was given.
        if (anchor.is_object() && !statedString(anchor, "path").empty() &&
            statedString(anchor, "commit").empty())
        {
          std::string headNow;
          if (ols::project::readHeadOid(repoDir, headNow))
            anchor["commit"] = headNow;
        }
        std::lock_guard<std::mutex> hold(gRecordDoorsMutex);
        if (!ols::issues::addComment(repoDir, id, who, statedString(given, "body"), anchor, out,
                                     error))
        {
          if (out.is_object() && out.contains("truncated"))
          {
            nlohmann::json b;
            b["did"] = false;
            b["words"] = error;
            b["truncated"] = true;
            putRev(b, svc, slug);
            res = projectDoorAnswer(400, b);
            return true;
          }
          res = recordWords(error, svc, slug, 400);
          return true;
        }
        okWords = "Commented on issue " + id + bumpStamp(projects, slug);
      }
      else if (action == "close" || action == "reopen")
      {
        std::lock_guard<std::mutex> hold(gRecordDoorsMutex);
        const bool done = action == "close" ? ols::issues::closeIssue(repoDir, id, out, error)
                                            : ols::issues::reopenIssue(repoDir, id, out, error);
        if (!done)
        {
          res = recordWords(error, svc, slug, 400);
          return true;
        }
        okWords = std::string(action == "close" ? "Closed issue " : "Reopened issue ") + id +
                  bumpStamp(projects, slug);
      }
      else if (action == "link")
      {
        const std::string kind = statedString(given, "kind");
        const std::string target = statedString(given, "target");
        if (target.empty())
        {
          res = projectDoorRefusal(400,
                                   "A link is made with the thing it names: send \"kind\" and \"target\"");
          return true;
        }
        std::lock_guard<std::mutex> hold(gRecordDoorsMutex);
        if (!ols::issues::linkIssue(repoDir, id, kind, target, out, error))
        {
          res = recordWords(error, svc, slug, 400);
          return true;
        }
        okWords = "Linked " + kind + " " + target + " to issue " + id + bumpStamp(projects, slug);
      }
      else
      {
        res = projectDoorRefusal(404, "the action on an issue is edit, comments, close, reopen, or link");
        return true;
      }
      res = actionAnswer(true, okWords, svc, slug, 200);
      return true;
    }

    // The list name in the wrong way: the reading door is a GET, the
    // opening door is a POST, as the ledger doors answer.
    res = projectDoorRefusal(405, "Issues are read with a GET and opened with a POST");
    return true;
  }

  // The project block's doors, as a member: the cert panel reads the very
  // material this server stands on (m_tlsMode, m_tlsMaterial), and everything
  // else the block answers rides the same arguments it always did.
  bool OpenLocalServices::routeProject(ols::project::ProjectService *projects,
                                       const std::shared_ptr<const std::string> &key,
                                       const HttpRequest &req, HttpResponse &res)
  {
    if (req.path.size() <= kProjectApiPrefixLen ||
        req.path.compare(0, kProjectApiPrefixLen, "/project/api/") != 0)
      return false;
    const std::string door = req.path.substr(kProjectApiPrefixLen);

    // Read with a GET the way every other reading door of this server is read;
    // a HEAD is that reading without its body, taken off by the one place that
    // writes an answer out (:2352-2357).
    const bool get = req.method == "GET" || req.method == "HEAD";
    const bool post = req.method == "POST";

    // The door stands in this binary; the module behind it may not. That is the
    // 502 every unwired module answers with (:2400, :2692 onward), and these
    // doors answer no differently - the table included, because a list of doors
    // that cannot answer would promise more than it can do.
    if (!projects)
    {
      res = projectDoorRefusal(502, "No project service");
      return true;
    }
    const ProjectDoorService svc(projects);

    // The browse doors of the code tab stand on the git engine, not the index.
    // A project is named first - every one of them takes ?project= - and only a
    // slug the index carries gets as far as the repo's folder; what stands under
    // an unknown slug is none of these doors' business, so it answers here with
    // the same words every other door uses for one.
    {
      const std::string codeDoor = req.path.substr(kProjectApiPrefixLen);
      if (codeDoor == "tree" || codeDoor == "file" || codeDoor == "file-outline" ||
          codeDoor == "history" || codeDoor == "blame")
      {
        HttpResponse refusal;
        const std::string slug = queryValue(req.query, "project");
        if (!projectNamed(svc, slug, "Which project? These doors read one repo: send ?project=<slug>",
                          refusal))
        {
          res = refusal;
          return true;
        }
        HttpResponse codeAnswer;
        if (routeProjectCode(*projects, slug, req, codeAnswer))
        {
          res = codeAnswer;
          return true;
        }
      }
    }

    if (door == "doors")
    {
      if (!get)
      {
        res = projectDoorRefusal(405, "The route table is read with a GET");
        return true;
      }
      res = jsonResponse(200, projectDoorsTable().dump());
      return true;
    }

    // The cert panel's two readings (spec §10.3). The facts are the same ones
    // the start-up line prints, read off the material this process stands on
    // - never re-derived from the files, so the page and the log cannot
    // disagree. No certificates standing is answered as it is: mode, and the
    // words saying there is nothing to show.
    if (door == "cert")
    {
      if (!get)
      {
        res = projectDoorRefusal(405, "The certificate facts are read with a GET");
        return true;
      }
      nlohmann::json body;
      body["mode"] = m_tlsMode.empty() ? std::string("off") : m_tlsMode;
      if (m_tlsMaterial)
      {
        body["sealed"] = true;
        body["ca_fingerprint"] = m_tlsMaterial->caFingerprint();
        body["leaf_expiry"] = m_tlsMaterial->leafExpiry();
        body["cert_dir"] = m_tlsCertDir;
      }
      else
      {
        body["sealed"] = false;
        body["words"] = "this port answers in the clear - no certificate stands";
      }
      res = jsonResponse(200, body.dump());
      return true;
    }

    if (door == "cert/ca.der")
    {
      // The CA in the dress a browser or another box imports: DER is what
      // Windows' own certificate prompt takes. Bytes straight from the file
      // that stands - the fingerprint above names exactly this material.
      if (!get)
      {
        res = projectDoorRefusal(405, "The CA is downloaded with a GET");
        return true;
      }
      if (!m_tlsMaterial)
      {
        res = projectDoorRefusal(404, "No certificate stands to download - this port answers in the clear");
        return true;
      }
      const std::string caPath = m_tlsCertDir + "/ca.der";
      std::error_code ec;
      if (!std::filesystem::is_regular_file(caPath, ec))
      {
        res = projectDoorRefusal(404, "No ca.der stands under the cert room");
        return true;
      }
      std::ifstream in(caPath, std::ios::binary);
      if (!in.good())
      {
        res = projectDoorRefusal(404, "The ca.der standing could not be read");
        return true;
      }
      std::ostringstream ss;
      ss << in.rdbuf();
      HttpResponse fileAnswer;
      fileAnswer.status = 200;
      fileAnswer.setPlain(ss.str(), "application/x-x509-ca-cert");
      fileAnswer.headers["Content-Disposition"] = "attachment; filename=\"ols-ca.der\"";
      fileAnswer.headers["Cache-Control"] = "no-store";
      res = fileAnswer;
      return true;
    }

    if (door == "index")
    {
      // The dropdown read: one answer carrying every project, its facts the
      // index keeps, and the stamps §9.1 compares against.
      if (!get)
      {
        res = projectDoorRefusal(405, "The project list is read with a GET");
        return true;
      }
      res = jsonResponse(200, svc.listJson());
      return true;
    }

    if (door == "revs")
    {
      // The cheap read the dropdown row asks for to see whether anything moved.
      if (!get)
      {
        res = projectDoorRefusal(405, "The change stamps are read with a GET");
        return true;
      }
      res = jsonResponse(200, svc.revsJson());
      return true;
    }

    if (door == "meta")
    {
      if (get)
      {
        const std::string slug = queryValue(req.query, "project");
        HttpResponse refusal;
        if (!projectNamed(svc, slug, "Which project? These facts stand one per project: send ?project=<slug>",
                          refusal))
        {
          res = refusal;
          return true;
        }
        const std::string facts = svc.metaJson(slug);
        if (facts.empty())
        {
          res = projectDoorRefusal(404, "No facts kept for project: " + slug);
          return true;
        }
        res = jsonResponse(200, facts);
        return true;
      }
      if (!post)
      {
        res = projectDoorRefusal(405, "A project's facts are read with a GET and changed with a POST");
        return true;
      }
      nlohmann::json given;
      if (!bodyObject(req, given))
      {
        res = projectDoorRefusal(400, "Invalid request body");
        return true;
      }
      const std::string slug = statedString(given, "project");
      HttpResponse refusal;
      if (!projectNamed(svc, slug, "Nothing to change: the ask names no project", refusal))
      {
        res = refusal;
        return true;
      }
      if (!given.contains("patch") || !given.at("patch").is_object() || given.at("patch").empty())
      {
        res = projectDoorRefusal(400, "Nothing to change: the ask states no patch of fields");
        return true;
      }
      // The patch goes on as it stands. Its values are prose the studio is
      // keeping, and a line break inside one is not a control character in a
      // path; what this machine will not hold, the module says.
      const bool done = svc.editMeta(slug, given.at("patch").dump());
      if (!done)
      {
        std::string words;
        const int status = refusalFromModule(svc, words);
        res = actionAnswer(false, words, svc, slug, status);
        return true;
      }
      res = actionAnswer(true, "Changed the facts of project " + slug, svc, slug, 200);
      return true;
    }

    if (door == "projects/new")
    {
      if (!post)
      {
        res = projectDoorRefusal(405, "A new project is made with a POST");
        return true;
      }
      nlohmann::json given;
      if (!bodyObject(req, given))
      {
        res = projectDoorRefusal(400, "Invalid request body");
        return true;
      }
      const std::string name = statedString(given, "name");
      if (name.empty())
      {
        res = projectDoorRefusal(400, "Nothing to make: the ask names no project");
        return true;
      }
      if (!plainWords(name))
      {
        res = projectDoorRefusal(400,
                                 "That project name carries characters this machine will not take");
        return true;
      }
      // Only the options that were stated are carried on, and a field of no
      // usable shape is refused by name rather than passed on as nothing: what
      // the page leaves out, the module decides, and a door that filled in
      // defaults would speak for a choice nobody made.
      nlohmann::json opts = nlohmann::json::object();
      if (given.contains("default_branch"))
      {
        if (!given.at("default_branch").is_string())
        {
          res = projectDoorRefusal(400, "'default_branch' must be a word");
          return true;
        }
        const std::string branch = given.at("default_branch").get<std::string>();
        if (!plainWords(branch))
        {
          res = projectDoorRefusal(400, "That branch name carries characters this machine will not take");
          return true;
        }
        if (!branch.empty())
          opts["default_branch"] = branch;
      }
      if (given.contains("with_readme"))
      {
        bool wants = false;
        if (!switchValue(given.at("with_readme"), wants))
        {
          res = projectDoorRefusal(400, "'with_readme' must be true or false");
          return true;
        }
        opts["with_readme"] = wants;
      }
      if (given.contains("license"))
      {
        if (!given.at("license").is_string())
        {
          res = projectDoorRefusal(400, "'license' must be a word");
          return true;
        }
        const std::string license = given.at("license").get<std::string>();
        if (!plainWords(license))
        {
          res = projectDoorRefusal(400, "That license name carries characters this machine will not take");
          return true;
        }
        if (!license.empty())
          opts["license"] = license;
      }
      std::string made;
      const bool done = svc.create(name, opts, made);
      nlohmann::json body;
      body["did"] = done;
      std::string words = "Created project " + name;
      int status = 200;
      if (done)
      {
        // The slug is the module's naming, and the module states it beside
        // the ask it just answered. Reading it back off a diff of the index
        // would name whatever project appeared last: on a machine answering
        // two creates at once, that is the other connection's project, not
        // this one's.
        if (!made.empty())
        {
          words += " as " + made;
          body["slug"] = made;
          putRev(body, svc, made);
        }
        // A birth that landed may still have a miss to state: the module keeps
        // it in the note beside the success, and only this door's own create
        // could have set it on this thread.
        const std::string miss = svc.lastError();
        if (!miss.empty())
        {
          words += ". " + miss;
          body["readme_miss"] = miss;
        }
      }
      else
        status = refusalFromModule(svc, words);
      body["words"] = words;
      res = projectDoorAnswer(status, body);
      return true;
    }

    if (door == "projects/adopt")
    {
      if (!post)
      {
        res = projectDoorRefusal(405, "A folder already standing on this machine is taken on with a POST");
        return true;
      }
      nlohmann::json given;
      if (!bodyObject(req, given))
      {
        res = projectDoorRefusal(400, "Invalid request body");
        return true;
      }
      const std::string folder = statedString(given, "path");
      const std::string link = statedString(given, "url");
      if (given.contains("url") && !given.at("url").is_string())
      {
        res = projectDoorRefusal(400, "'url' must be a word");
        return true;
      }
      if (!folder.empty() && !link.empty())
      {
        res = projectDoorRefusal(400,
                                 "One form at a time: the ask names both a folder and a link");
        return true;
      }
      if (!link.empty())
      {
        // The "From a link" form. A git address this door reads is http(s) over
        // a host with a repository path standing under it, or an ssh address -
        // either form the ssh readings take (E2), which rides the vendored road
        // from here: the shape check passes it and the clone answers with the
        // ssh words when the pinned hands or the studio's key room are not yet
        // standing. The shape is read first, so an ask that is no git address
        // is refused for what it is.
        if (!plainWords(link))
        {
          res = projectDoorRefusal(400,
                                   "That link carries characters this machine will not take");
          return true;
        }
        const size_t schemeEnd = link.find("://");
        ols::ssh::SshTarget sshShape;
        const bool sshRoad = ols::ssh::looksSsh(link, sshShape);
        if (schemeEnd == std::string::npos && !sshRoad)
        {
          res = projectDoorRefusal(400,
                                   "That link names no scheme: an http or https git address, please");
          return true;
        }
        std::string scheme = schemeEnd == std::string::npos ? std::string()
                                                            : link.substr(0, schemeEnd);
        for (char &c : scheme)
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!sshRoad && scheme != "http" && scheme != "https")
        {
          res = projectDoorRefusal(400,
                                   "That link speaks " + scheme +
                                       "; this door takes http and https git addresses only");
          return true;
        }
        const std::string rest = schemeEnd == std::string::npos ? link : link.substr(schemeEnd + 3);
        const size_t hostEnd = rest.find_first_of("/?#");
        const std::string host = hostEnd == std::string::npos ? rest : rest.substr(0, hostEnd);
        if (host.empty() && !sshRoad)
        {
          res = projectDoorRefusal(400, "That link names no host: the address ends at its scheme");
          return true;
        }
        // An ssh address may carry its login inside it - that is the address,
        // and the key room decides what answers the far side. For http(s) a
        // name inside the address stays refused as before.
        if (host.find('@') != std::string::npos && !sshRoad)
        {
          res = projectDoorRefusal(400,
                                   "That link carries a name and a key inside its address, which "
                                   "this door does not take");
          return true;
        }
        std::string repoPath;
        if (sshRoad)
        {
          // The ssh readings already refused a form with no repository path,
          // so what stands on this road carries one: the shape check ends.
        }
        else
        {
          if (hostEnd != std::string::npos && rest[hostEnd] == '/')
          {
            const size_t tail = rest.find_first_of("?#", hostEnd + 1);
            repoPath = tail == std::string::npos ? rest.substr(hostEnd + 1)
                                                 : rest.substr(hostEnd + 1, tail - hostEnd - 1);
          }
          if (repoPath.empty())
          {
            res = projectDoorRefusal(400, "That link names no repository under its host");
            return true;
          }
          if (scheme == "https")
          {
            res = projectDoorRefusal(
                501,
                "the address " + link +
                    " asks for https, which is TLS, and this tree carries no TLS layer for it: "
                    "send a plain http:// address");
            return true;
          }
        }
        // The http address goes to the module: it asks the advertisement,
        // makes the folder, clones, and refuses in its own words when it
        // must. The key this process answers with travels with the ask.
        std::string made;
        const bool cloned = svc.adoptUrl(link, std::string(), made, *key);        nlohmann::json body;
        body["did"] = cloned;
        std::string words = "Took on the repository at " + link;
        int status = 200;
        if (cloned)
        {
          if (!made.empty())
          {
            words += " as " + made;
            body["slug"] = made;
            putRev(body, svc, made);
          }
        }
        else
          status = refusalFromModule(svc, words);
        body["words"] = words;
        res = projectDoorAnswer(status, body);
        return true;
      }
      if (folder.empty())
      {
        res = projectDoorRefusal(400, "Nothing to take on: the ask names no folder");
        return true;
      }
      // A folder taken on is a place a git command may later be pointed, so this
      // part is tested by the same rule the fetches use: refused outright, never
      // escaped (Platform.h:227-249). Where the folder may be, and what may
      // stand there, is the module's rule.
      if (!platform::quotableInShell(folder))
      {
        res = projectDoorRefusal(400,
                                 "That path carries characters this machine will not put in a command");
        return true;
      }
      std::string made;
      const bool done = svc.adoptFolder(folder, made);
      nlohmann::json body;
      body["did"] = done;
      std::string words = "Took on the folder at " + folder;
      int status = 200;
      if (done)
      {
        // The slug is the module's naming, stated beside the ask: the same
        // reason the create door reads it there, stated just above.
        if (!made.empty())
        {
          words += " as " + made;
          body["slug"] = made;
          putRev(body, svc, made);
        }
      }
      else
        status = refusalFromModule(svc, words);
      body["words"] = words;
      res = projectDoorAnswer(status, body);
      return true;
    }

    if (door == "projects/rename")
    {
      if (!post)
      {
        res = projectDoorRefusal(405, "A project is renamed with a POST");
        return true;
      }
      nlohmann::json given;
      if (!bodyObject(req, given))
      {
        res = projectDoorRefusal(400, "Invalid request body");
        return true;
      }
      const std::string slug = statedString(given, "slug");
      HttpResponse refusal;
      if (!projectNamed(svc, slug, "Which project? The ask names none", refusal))
      {
        res = refusal;
        return true;
      }
      const std::string newName = statedString(given, "name");
      if (newName.empty())
      {
        res = projectDoorRefusal(400, "Nothing to call it: the ask states no new name");
        return true;
      }
      if (!plainWords(newName))
      {
        res = projectDoorRefusal(400,
                                 "That project name carries characters this machine will not take");
        return true;
      }
      const bool done = svc.rename(slug, newName);
      if (!done)
      {
        std::string words;
        const int status = refusalFromModule(svc, words);
        res = actionAnswer(false, words, svc, slug, status);
        return true;
      }
      res = actionAnswer(true, "Renamed project " + slug + " to " + newName, svc, slug, 200);
      return true;
    }

    if (door == "projects/delete")
    {
      if (!post)
      {
        res = projectDoorRefusal(405, "A project is taken out with a POST");
        return true;
      }
      nlohmann::json given;
      if (!bodyObject(req, given))
      {
        res = projectDoorRefusal(400, "Invalid request body");
        return true;
      }
      const std::string slug = statedString(given, "slug");
      HttpResponse refusal;
      if (!projectNamed(svc, slug, "Which project? The ask names none", refusal))
      {
        res = refusal;
        return true;
      }
      // The ask that takes a project out carries its own confirmation; these
      // doors only see that it was carried, and in plain words. Whether it
      // confirms - and whether anything still stands in the way, open pulls
      // among them - is the module's answer (spec §11).
      const std::string confirm = statedString(given, "confirm");
      if (confirm.empty())
      {
        res = projectDoorRefusal(400,
                                 "A project is not taken out on a bare ask: send confirm naming the project again");
        return true;
      }
      if (!plainWords(confirm))
      {
        res = projectDoorRefusal(400, "That confirm word carries characters this machine will not take");
        return true;
      }
      std::string residue;
      const bool done = svc.remove(slug, confirm, residue);
      if (!done)
      {
        std::string words;
        const int status = refusalFromModule(svc, words);
        res = actionAnswer(false, words, svc, slug, status);
        return true;
      }
      // What the letting-go leaves standing is the module's to know and to
      // state: the folder a carried project was taken on from, which was
      // never the studio's to take away, and the part of a tree that would
      // not come away, if any. The project left the ledger either way, and
      // the answer says what stayed on disk rather than only what went.
      std::string words = "Took project " + slug + " out of the studio";
      if (!residue.empty())
        words += "; " + residue;
      res = actionAnswer(true, words, svc, slug, 200);
      return true;
    }

    if (door == "commit")
    {
      // The editor's save (spec §5.9): files become a commit on a branch, or a
      // ref taken straight from what HEAD holds when the project has no refs
      // yet - the unborn case is how a new project gets its first commit.
      if (!post)
      {
        res = projectDoorRefusal(405, "A commit lands with a POST");
        return true;
      }
      nlohmann::json given;
      if (!bodyObject(req, given))
      {
        res = projectDoorRefusal(400, "Invalid request body");
        return true;
      }
      const std::string slug = statedString(given, "project");
      HttpResponse refusal;
      if (!projectNamed(svc, slug, "Which project? The ask names none", refusal))
      {
        res = refusal;
        return true;
      }
      const std::string repoDir = projects->repoDir(slug);
      if (repoDir.empty())
      {
        res = projectDoorRefusal(404, "No repo stands for project: " + slug);
        return true;
      }
      const std::string message = statedString(given, "message");
      if (message.empty())
      {
        res = projectDoorRefusal(400, "A commit without words is not a commit: send \"message\"");
        return true;
      }
      std::string branch = statedString(given, "branch");
      if (!branch.empty() && !plainWords(branch))
      {
        res = projectDoorRefusal(400, "That branch name carries characters this machine will not take");
        return true;
      }
      // No branch named: the project's own default is where a save lands -
      // stated in its facts, never guessed. An unborn default builds the root
      // commit; that is how a new project gets its first one.
      if (branch.empty())
      {
        nlohmann::json facts = nlohmann::json::parse(svc.metaJson(slug), nullptr, false);
        if (!facts.is_discarded() && facts.contains("default_branch") &&
            facts.at("default_branch").is_string())
          branch = facts.at("default_branch").get<std::string>();
      }
      // The author is the studio's own hand: a commit says who it ran as.
      std::string author = statedString(given, "author");
      if (author.empty())
        author = "studio";
      const auto filesIt = given.find("files");
      if (filesIt == given.end() || !filesIt->is_array() || filesIt->empty())
      {
        res = projectDoorRefusal(400, "Nothing to commit: the ask carries no files array");
        return true;
      }
      std::vector<ols::project::CommitFile> files;
      for (const auto &row : *filesIt)
      {
        if (!row.is_object())
        {
          res = projectDoorRefusal(400, "Every file of a commit is an object with a path");
          return true;
        }
        ols::project::CommitFile entry;
        entry.path = statedString(row, "path");
        if (entry.path.empty())
        {
          res = projectDoorRefusal(400, "Every file of a commit names its path");
          return true;
        }
        const auto delIt = row.find("delete");
        entry.deleteFile = delIt != row.end() && delIt->is_boolean() && delIt->get<bool>();
        if (!entry.deleteFile)
        {
          const auto contentIt = row.find("content");
          if (contentIt == row.end() || !contentIt->is_string())
          {
            res = projectDoorRefusal(400, "A file written by a commit carries its content as text: " + entry.path);
            return true;
          }
          entry.content = contentIt->get<std::string>();
        }
        files.push_back(std::move(entry));
      }
      std::string newOid;
      bool raced = false;
      const bool done = ols::project::commitFiles(repoDir, branch, files, message, author, newOid,
                                                  raced);
      if (!done)
      {
        std::string words = ols::project::lastError();
        if (words.empty())
          words = "The commit did not land.";
        // A save that raced another is a refusal the reader can act on; every
        // other refusal stands as it was said. The race is the module's own
        // stated flag - reading it out of the words would answer a 400 about a
        // file named notes/moved.md as though the branch had moved.
        res = actionAnswer(false, words, svc, slug, raced ? 409 : 400);
        return true;
      }
      nlohmann::json body;
      body["did"] = true;
      // A commit is a mutation of the project: the stamp moves beside it, the
      // way every other record door carries its bump (§9.1).
      std::string okWords = "Committed " + std::to_string(files.size()) + " file(s) on " + slug;
      okWords += bumpStamp(projects, slug);
      body["words"] = okWords;
      body["commit"] = newOid;
      putRev(body, svc, slug);
      res = projectDoorAnswer(200, body);
      return true;
    }

    if (door == "build")
    {
      // The build doors (§7 of the studio): a build is started with a POST that
      // names the project and one of its files, and followed with a GET naming
      // the job - the same reading the build-status door carries, so the page
      // that pressed run keeps one address for its build.
      if (get)
      {
        const std::string jobId = queryValue(req.query, "job");
        if (jobId.empty())
        {
          res = projectDoorRefusal(400, "Which build? This door reads a standing job with ?job=");
          return true;
        }
        std::string answerJson;
        std::string words;
        if (!svc.buildStatus(jobId, answerJson, words))
        {
          const int status = refusalFromModule(svc, words);
          res = projectDoorRefusal(status, words);
          return true;
        }
        res = jsonResponse(200, answerJson);
        return true;
      }
      if (!post)
      {
        res = projectDoorRefusal(405, "A build is started with a POST and read with a GET");
        return true;
      }
      nlohmann::json given;
      if (!bodyObject(req, given))
      {
        res = projectDoorRefusal(400, "Invalid request body");
        return true;
      }
      const std::string slug = statedString(given, "project");
      HttpResponse refusal;
      if (!projectNamed(svc, slug, "Which project? The build compiles one of its files: send \"project\" and \"path\"",
                        refusal))
      {
        res = refusal;
        return true;
      }
      const std::string relPath = statedString(given, "path");
      if (relPath.empty())
      {
        res = projectDoorRefusal(400, "Nothing to build: the ask names no file of the project");
        return true;
      }
      // What the extension wears is the machine's own finding, asked once
      // before the start: the ask arms the toolchain probe, and an extension
      // outside the map is refused in the module's words before the ask goes
      // any further.
      const std::string::size_type dot = relPath.rfind('.');
      const std::string::size_type slash = relPath.rfind('/');
      std::string ext;
      if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        ext = relPath.substr(dot);
      std::string toolWords;
      if (!svc.toolchainFor(ext, toolWords))
      {
        const int status = refusalFromModule(svc, toolWords);
        res = projectDoorRefusal(status, toolWords);
        return true;
      }
      // Where the file stands and whether a build already stands for this
      // project are the module's rules, stated in its own words; nothing here
      // guesses them.
      std::string jobId;
      std::string words;
      if (!svc.startBuild(slug, relPath, jobId, words))
      {
        const int status = refusalFromModule(svc, words);
        res = projectDoorRefusal(status, words);
        return true;
      }
      // The house shape for a job: the name it answers by, and the state it
      // wears at the start.
      nlohmann::json body;
      body["job"] = jobId;
      body["state"] = "running";
      res = projectDoorAnswer(202, body);
      return true;
    }

    if (door == "build-status")
    {
      // One build job as far as it has got: the module's own answer document -
      // the state it wears, what it has printed, and once it ended the code it
      // came out with, its duration, and the compiler's lines read as rows.
      if (!get)
      {
        res = projectDoorRefusal(405, "A build status is read with a GET");
        return true;
      }
      const std::string jobId = queryValue(req.query, "job");
      if (jobId.empty())
      {
        res = projectDoorRefusal(400, "Which build? This door names the job with ?job=");
        return true;
      }
      std::string answerJson;
      std::string words;
      if (!svc.buildStatus(jobId, answerJson, words))
      {
        const int status = refusalFromModule(svc, words);
        res = projectDoorRefusal(status, words);
        return true;
      }
      res = jsonResponse(200, answerJson);
      return true;
    }

    if (door == "build-stop")
    {
      // Let a standing build go: the ask names the job, the process the job
      // wrote down is ended tree-wide, and the job's record takes the mark -
      // the status door answers it as cancelled from here on.
      if (!post)
      {
        res = projectDoorRefusal(405, "A build is asked to go with a POST");
        return true;
      }
      nlohmann::json given;
      if (!bodyObject(req, given))
      {
        res = projectDoorRefusal(400, "Invalid request body");
        return true;
      }
      const std::string jobId = statedString(given, "job");
      if (jobId.empty())
      {
        res = projectDoorRefusal(400, "Nothing to stop: the ask names no job");
        return true;
      }
      std::string words;
      const bool done = svc.stopBuild(jobId, words);
      if (!done)
      {
        const int status = refusalFromModule(svc, words);
        res = actionAnswer(false, words, svc, "", status);
        return true;
      }
      res = actionAnswer(true, "Asked the build " + jobId + " to go; it answers as cancelled", svc, "", 200);
      return true;
    }

    // The pulls, branches, and issues doors (spec §5.6, §6): the blocks
    // beside this function answer what their names cover, every one of them
    // through the same named-project law the ledger doors use. False from
    // both means the address is none of these doors.
    {
      HttpResponse recordAnswer;
      if (routeProjectPullsDoors(projects, req, recordAnswer) ||
          routeProjectIssuesDoors(projects, req, recordAnswer))
      {
        res = recordAnswer;
        return true;
      }
    }

    // Not one of these doors. The address belongs to route(), and route() answers
    // an address it does not know with a 404 saying this server has no such door
    // (:2999-3002) - a 502 is kept for a module genuinely not wired in, and was
    // answered above when that was the case.
    return false;
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

    // The one address that stands open to nothing it asks of: the bare address
    // of this machine, where what the doors ask of is stated and where a
    // credential is handed over exactly as a program hands one. A program that
    // asks with no Accept, or with */*, gets the json this machine has always
    // answered; a person opening this address opens the front room, which asks
    // the browser for a key itself when this machine asks of one - no address of
    // this machine is a place a key has to be written.
    if (path == "/")
    {
      if (post)
        return certifyAnswer(req);
      if (!get && !head)
        return jsonError(405,
                         "This address is read with a GET and handed a credential with a POST");
      if (browserOpeningDocument(req))
        return serveStatic("/index.html", req);
      return jsonResponse(200, authInfoJson());
    }

    // The rooms and everything they load: the pages are shells and stand open,
    // and every fact behind them - every api door and every file - answers only
    // to the key. A page that reaches its doors with no credential asks for one
    // where it stands, and hands it over the same doors any other ask uses.
    if (fetch && webAssetForPath(path))
      return serveStatic(path, req);

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

    // Fleet awareness: the table of other servers this machine has met, and the
    // hello that makes the meeting both ways. The credential was weighed at the
    // door before this point: whatever reaches the hello answered our key, and
    // a server that answers a different one never records anything.
    if (get && path == "/api/peers")
      return jsonResponse(200, m_peers.tableJson().dump());
    if (get && path == "/api/peers/hello")
    {
      nlohmann::json given;
      given["name"] = queryValue(req.query, "name");
      given["port"] = queryInt(req.query, "port", 0);
      given["version"] = queryValue(req.query, "version");
      given["backend"] = queryValue(req.query, "backend");
      return jsonResponse(200, m_peers.helloAnswer(req.ip, given).dump());
    }

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
      // The soft hand takes this server's place with the program that stands on
      // disk, at once and as it always did. When a person is rebuilding while the
      // server runs they can name where the build puts its new binary: the door
      // reads whether that build already stands newer than the program running
      // now and says so plainly - if not, it states how long it will wait (its
      // own default ten minutes) and watches behind the answer, handing over the
      // moment a newer binary stands, or writing in its log that none came. The
      // watch never stops this server itself: it starts a copy of the program on
      // disk at an address no one else holds (--hand-where below), and that copy
      // - which binds nothing anyone visits - asks for the hand once the newer
      // binary stands, so the running server is replaced by something already
      // gone rather than fighting itself for the port.
      const std::string waitFor = given.value("wait_for", std::string());
      int waitSeconds = given.value("wait_seconds", 0);
      if (action == "restart" && !waitFor.empty() && waitSeconds < 1)
        waitSeconds = 600;
      if (waitSeconds > 3600)
        waitSeconds = 3600;
      const bool waits = action == "restart" && !waitFor.empty();
      std::string waitedWords;
      bool watchFresh = false;
      // The port this server stands at is read before the answer goes out: the
      // watch that runs behind it names it, and a detached thread may not reach
      // back into this object.
      const uint16_t herePort = m_port.load();
      if (waits)
      {
        std::error_code ec{};
        const auto running = std::filesystem::last_write_time(ols::deploy::selfProgram(), ec);
        const auto found = std::filesystem::last_write_time(std::filesystem::path(waitFor), ec);
        if (ec)
          waitedWords = "the build named here cannot be read: " + ec.message();
        else if (!(found > running))
        {
          watchFresh = true;
          waitedWords = "the build named here does not stand newer than the program that runs yet: this server will wait " +
                        std::to_string(waitSeconds) + "s for one, and hand to it when it stands";
        }
        else
          waitedWords = "the build named here already stands newer: handing over now";
      }
      const bool done = ols::deploy::act(action, words);
      nlohmann::json out;
      out["did"] = done;
      if (waits)
        out["waiting"] = watchFresh;
      out["words"] = waits ? waitedWords : words;
      if (watchFresh)
      {
        // The answer goes first; the watch for the new binary runs behind it, and
        // starts the hand only once that build stands newer than this program.
        std::thread([waitFor, waitSeconds, herePort]() {
          const std::filesystem::path wanted(waitFor);
          bool fresh = false;
          const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(waitSeconds);
          for (;;)
          {
            std::error_code ec{};
            const auto running = std::filesystem::last_write_time(ols::deploy::selfProgram(), ec);
            const auto found = std::filesystem::last_write_time(wanted, ec);
            if (!ec && found > running)
            {
              fresh = true;
              break;
            }
            if (std::chrono::steady_clock::now() >= until)
              break;
            std::this_thread::sleep_for(std::chrono::seconds(2));
          }
          if (!fresh)
          {
            std::cout << "the build named for the hand never stood newer than this server: nothing was restarted." << std::endl;
            return;
          }
          // The program on disk is started as this process's own child at the
          // level this server stands at, and asked to stop with its listening
          // closed (--hand-stand below): it binds no port of anyone's - a
          // replacement reaching for the address while the running server still
          // holds it would only refuse and leave nothing standing - so it hands
          // on from something already gone. The number named is drawn from what
          // this server stands at, and nothing ever visits that address: two
          // servers waiting on one machine each watch behind their own.
          const int handPort = herePort + 3000;
          const std::string handArgs = "--hand-stand " +
                                       std::to_string(handPort > 65535 ? handPort - 1000 : handPort);
          const std::string program = ols::deploy::selfProgram();
          if (program.empty())
          {
            std::cout << "this server cannot name the program it runs as: nothing was restarted." << std::endl;
            return;
          }
#ifdef _WIN32
          STARTUPINFOA look{};
          look.cb = sizeof(look);
          PROCESS_INFORMATION gone{};
          std::string line = "\"" + program + "\" " + handArgs;
          if (!CreateProcessA(program.c_str(), line.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &look, &gone))
          {
            std::cout << "this machine would not start the hand (win32 error " << GetLastError()
                      << "): nothing was restarted." << std::endl;
            return;
          }
          WaitForSingleObject(gone.hProcess, INFINITE);
          CloseHandle(gone.hThread);
          CloseHandle(gone.hProcess);
#else
          const pid_t runner = fork();
          if (runner == 0)
          {
            execl(program.c_str(), program.c_str(), handArgs.c_str(), static_cast<char *>(nullptr));
            _exit(127);
          }
          if (runner < 0)
          {
            std::cout << "the machine would not start the hand: nothing was restarted." << std::endl;
            return;
          }
          int gone = 0;
          waitpid(runner, &gone, 0);
#endif
        }).detach();
      }
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
          setApiKey(change.second);
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

    if (post && path == "/api/jobs/reorder")
    {
      if (!m_image)
        return jsonError(502, "No image service");
      const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
      const std::string id = !body.is_discarded() && body.contains("id") && body["id"].is_string()
                                 ? body["id"].get<std::string>()
                                 : std::string();
      const int shift = !body.is_discarded() && body.contains("shift") && body["shift"].is_number_integer()
                            ? body["shift"].get<int>()
                            : 0;
      if (id.empty() || shift == 0)
        return jsonError(400, "which job to move and by how many places are both needed");
      if (!m_image->reorderJob(id, shift))
        return jsonError(404, "that job does not stand in the line, or cannot move that far");
      return jsonResponse(200, "{}");
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

    if (fetch && path.rfind("/files/", 0) == 0)
    {
      const std::string rest = path.substr(7);
      const size_t slash = rest.find('/');
      if (slash == std::string::npos)
        return jsonError(404, "Not found");
      return serveFile(rest.substr(0, slash), rest.substr(slash + 1), req);
    }

    if (get && path == "/api/system")
      return jsonResponse(200, systemJson());

    if (get && path == "/api/monitor")
    {
      const std::string imageJobs = m_image ? m_image->jobsListJson() : std::string("[]");
      // The queue estimate asks for the picture engine's cost ledger only
      // while jobs stand, so the way to it is passed rather than the answer.
      return jsonResponse(200, monitor::snapshot(systemJson(), imageJobs,
                                                 [this]
                                                 { return m_image ? m_image->timingsJson() : std::string("{}"); }));
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

    // The git smart-HTTP doors: /git/<slug>/... answers from the project's bare
    // repo through the wire layer, under the same key gate every other door
    // stands behind.
    if (isGitDoorPath(path))
    {
      HttpResponse gitAnswer;
      if (routeGitSmart(m_projects, req, gitAnswer))
        return gitAnswer;
    }

    // The Project Studio doors: everything under this prefix belongs to the
    // project module's block, and an address under the prefix that the block
    // does not know is answered below as any unknown address is.
    if (path.rfind("/project/api/", 0) == 0)
    {
      HttpResponse projectAnswer;
      const bool answered = routeProject(m_projects, apiKey(), req, projectAnswer);
      // The record doors' lock left with their answer: hand the stamps this
      // door moved to the streams beside the doors now, outside that lock
      // (§9.3).
      publishPendingProjectEvents();
      if (answered)
        return projectAnswer;
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
