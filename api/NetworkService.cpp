// Network management service: what this machine's network is, and the hands
// that change it. Windows answers through its own tools (iphlpapi calls,
// netsh, the sharing COM object); Linux answers through the ones that belong
// to it (sysfs, /proc, netplan, iw and nmcli, hostapd, iptables). The page in
// front reads one document either way, and every refusal says why in the same
// voice: this server never pretends a machine can do what it cannot.
//
// The house rules this module keeps:
//  - Reads are pure where they can be: structs and files first, shells only
//    for what only a shell can ask, and every command-backed answer cached a
//    few seconds so a two-second poll costs the machine nothing new.
//  - Changes run through the machine's own tools with quotableInShell
//    discipline: a name this server cannot carry through a shell is refused
//    before anything runs, and the tool's own words come back as the answer.
//  - On Linux the only netplan file this server writes is the one it owns
//    (/etc/netplan/70-open-local-services.yaml), kept whole under
//    data/network-state.json; `netplan generate` must accept what was written
//    before `netplan apply` ever sees it, and a refused file goes back exactly
//    the way it was found.
//  - windows.h is asked to leave its min/max macros alone: this file uses the
//    real ones.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "NetworkService.h"

#include "Common.h"
#include "Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <iphlpapi.h>
#include <wlanapi.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ols
{
  namespace net
  {
    namespace
    {

      using json = nlohmann::json;

      // ---------- plain helpers ----------
      std::string trim(const std::string &text)
      {
        size_t begin = 0;
        while (begin < text.size() &&
               std::isspace(static_cast<unsigned char>(text[begin])))
          ++begin;
        size_t end = text.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
          --end;
        return text.substr(begin, end - begin);
      }

      std::string lower(std::string text)
      {
        for (char &c : text)
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
      }

      std::string upper2(std::string text)
      {
        for (char &c : text)
          c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return text;
      }

      bool containsFold(const std::string &haystack, const std::string &needle)
      {
        return lower(haystack).find(lower(needle)) != std::string::npos;
      }

      std::vector<std::string> splitLines(const std::string &text)
      {
        std::vector<std::string> lines;
        std::istringstream in{text};
        std::string line;
        while (std::getline(in, line))
          lines.push_back(trim(line));
        return lines;
      }

      // The words after the first colon, as `netsh` and `iw` like to print them.
      std::string fieldAfter(const std::string &line, const std::string &key)
      {
        if (line.rfind(key, 0) != 0)
          return std::string();
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
          return std::string();
        return trim(line.substr(colon + 1));
      }

      // The same reading when the key has to stand whole: "Channel : 11" is
      // answered and "Channel Utilization : 46 (18 %)" is not. netsh nests a
      // Bss Load under the very BSSID it names, and the looser reading above
      // answers one for the other - which is how a channel of 11 once came
      // back as 46.
      std::string fieldExact(const std::string &line, const std::string &key)
      {
        if (line.rfind(key, 0) != 0)
          return std::string();
        size_t at = key.size();
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
          ++at;
        if (at >= line.size() || line[at] != ':')
          return std::string();
        return trim(line.substr(at + 1));
      }

      long long toNumber(const std::string &text, long long fallback)
      {
        if (text.empty())
          return fallback;
        char *end = nullptr;
        const long long value = std::strtoll(text.c_str(), &end, 10);
        return end == text.c_str() ? fallback : value;
      }

      // A part is only ever handed to a shell after this says yes: plain words
      // that need no quoting to survive it. Notably no percent sign either - a
      // cmd.exe eats those out of even quoted strings, and no honest adapter,
      // ssid or address ever needs one.
      bool safeShellPart(const std::string &text, bool spacesOk)
      {
        if (text.empty() || text.size() > 255)
          return false;
        for (const char c : text)
        {
          const unsigned char u = static_cast<unsigned char>(c);
          if (std::isalnum(u) || c == '_' || c == '-' || c == '.' || c == ':')
            continue;
          if (spacesOk && c == ' ')
            continue;
          return false;
        }
        return text[0] != '.';
      }

      bool safeIfname(const std::string &text)
      {
        if (text.size() > 128)
          return false;
        return safeShellPart(text, false);
      }

      bool safeAddressish(const std::string &text)
      {
        if (text.empty() || text.size() > 64)
          return false;
        for (const char c : text)
        {
          const unsigned char u = static_cast<unsigned char>(c);
          if (std::isalnum(u) || c == '.' || c == ':')
            continue;
          return false;
        }
        return true;
      }

      long long steadyMs()
      {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
      }

      long long nowSeconds()
      {
        return static_cast<long long>(std::time(nullptr));
      }

      std::string xmlEscape(const std::string &text)
      {
        std::string out;
        for (const char c : text)
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
          case '\'':
            out += "&apos;";
            break;
          default:
            out += c;
            break;
          }
        }
        return out;
      }

      // ---------- asking the machine, through its own tools ----------
      struct Run
      {
        bool ran = false;
        int code = -1;
        std::string out;
      };

      Run runShell(const std::string &command)
      {
        Run result;
#ifdef _WIN32
        FILE *pipe = _popen(command.c_str(), "r");
        const int openErr = errno;
#else
        FILE *pipe = popen(command.c_str(), "r");
        const int openErr = errno;
#endif
        if (!pipe)
        {
          result.out = "this server could not even open a shell to ask the machine (errno " +
                       std::to_string(openErr) + ")";
          return result;
        }
        char line[1024];
        while (std::fgets(line, sizeof(line), pipe))
          result.out += line;
        const int status =
#ifdef _WIN32
            _pclose(pipe);
#else
            pclose(pipe);
#endif
        result.ran = true;
#ifdef _WIN32
        result.code = status;
#else
        result.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
        // The machine's own words should not carry stray carriage returns.
        std::string clean;
        for (const char c : result.out)
          if (c != '\r')
            clean += c;
        result.out = clean;
        return result;
      }

      // ---------- the kept watches: rates and uptime between two peeks ----------
      struct Counters
      {
        unsigned long long rxBytes = 0;
        unsigned long long txBytes = 0;
        unsigned long long rxPkts = 0;
        unsigned long long txPkts = 0;
        unsigned long long rxErr = 0;
        unsigned long long txErr = 0;
        unsigned long long rxDrop = 0;
        unsigned long long txDrop = 0;
        bool operUp = false;
        bool adminEnabled = false;
      };

      struct RatePoint
      {
        long long atS = 0;
        double rxRate = 0;
        double txRate = 0;
        double rxPps = 0;
        double txPps = 0;
      };

      struct AdapterWatch
      {
        Counters last;
        bool hadPrevious = false;
        long long lastAtMs = 0;
        long long lastSeenMs = 0;
        bool firstSeen = true;
        bool up = false;
        long long upSinceEpoch = 0;
        bool upMeasured = false; // this server saw it go down and come back
        std::deque<RatePoint> history;
      };

      template <typename T>
      struct Cached
      {
        std::mutex mutex;
        T value{};
        bool filled = false;
        long long atMs = 0;
        bool fresh(long long maxAgeMs) const
        {
          return filled && (steadyMs() - atMs) < maxAgeMs;
        }
      };

      struct State
      {
        std::thread sampler;
        std::atomic<bool> running{false};

        std::mutex watchMutex;
        std::unordered_map<std::string, AdapterWatch> watches;

        std::mutex scanMutex;
        bool scanEverRun = false;
        std::string scanJsonText;
        long long scanAtMs = 0;

        std::mutex actionRunMutex; // one change at a time: netsh and netplan are serial
        std::mutex actionMutex;
        json lastAction;

        Cached<json> features;
        Cached<json> sharing;
        Cached<json> apState;
        Cached<json> wifiStatus;

#ifdef _WIN32
        Cached<bool> wifiRadio; // netsh wlan even answering on this box
#else
        struct OwnerCache
        {
          std::mutex mutex;
          long long atMs = 0;
          std::unordered_map<std::string, std::string> byInode;
        } owners;

        std::mutex netplanMutex;
        bool netplanLoaded = false;
        json netplanState;
#endif
      };

      State &state()
      {
        static State instance;
        return instance;
      }

      // What this kind of machine can do from here and why not otherwise - the
      // answer both platforms' parts ask of each other, so it stands ahead of
      // all of them.
      json featuresJson();

      // ---------- what this server remembers about its own changes ----------
      std::string stateFilePath()
      {
        return "data/network-state.json";
      }

      json loadStateFile()
      {
        std::string text;
        if (!ols::readFileText(stateFilePath(), text))
          return json::object();
        json parsed = json::parse(text, nullptr, false);
        return parsed.is_discarded() || !parsed.is_object() ? json::object() : parsed;
      }

      // The file is this server's notebook about the machine: what it made
      // (netplan devices), what it enabled (sharing, an access point), and the
      // last thing the page asked for. Merged, never trounced.
      bool saveStateFile(const json &value)
      {
        std::error_code ec{};
        std::filesystem::create_directories("data", ec);
        std::string text;
        json merged = json::object();
        if (ols::readFileText(stateFilePath(), text))
        {
          json old = json::parse(text, nullptr, false);
          if (!old.is_discarded() && old.is_object())
            merged = old;
        }
        for (const auto &item : value.items())
          merged[item.key()] = item.value();
        return ols::writeFileText(stateFilePath(), merged.dump(2));
      }

      // ---------- counters, both platforms ----------
#ifdef _WIN32
      // Windows names adapters in wide characters; this server keeps to UTF-8.
      std::string wide(const wchar_t *text)
      {
        if (!text || !*text)
          return std::string();
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 1)
          return std::string();
        std::string out(static_cast<size_t>(bytes) - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
        return out;
      }

      bool countersWin(std::unordered_map<std::string, Counters> &out)
      {
        const ULONG family = AF_UNSPEC;
        // Names and numbers only here; the friendly name (the alias a person
        // set, and the one every other part of Windows agrees on) must not be
        // skipped, so it is not on the skip list.
        const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                            GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_UNICAST;
        ULONG size = 0;
        if (GetAdaptersAddresses(family, flags, nullptr, nullptr, &size) !=
                ERROR_BUFFER_OVERFLOW ||
            size == 0)
          return false;
        std::vector<unsigned char> buffer(size);
        PIP_ADAPTER_ADDRESSES list =
            reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        if (GetAdaptersAddresses(family, flags, nullptr, list, &size) != NO_ERROR)
          return false;
        for (PIP_ADAPTER_ADDRESSES row = list; row; row = row->Next)
        {
          if (row->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
          MIB_IF_ROW2 entry{};
          entry.InterfaceLuid = row->Luid;
          entry.InterfaceIndex = row->IfIndex;
          if (GetIfEntry2(&entry) != NO_ERROR)
            continue;
          Counters counters;
          counters.rxBytes = entry.InOctets;
          counters.txBytes = entry.OutOctets;
          // The row countsunicast and non-unicast apart; both are packets.
          counters.rxPkts = entry.InUcastPkts + entry.InNUcastPkts;
          counters.txPkts = entry.OutUcastPkts + entry.OutNUcastPkts;
          counters.rxErr = entry.InErrors;
          counters.txErr = entry.OutErrors;
          counters.rxDrop = entry.InDiscards;
          counters.txDrop = entry.OutDiscards;
          // The status enums print as their numeric kind across sdk headers;
          // the numbers are the stable ones: oper 1 is up, admin 1 is up.
          counters.operUp = static_cast<int>(entry.OperStatus) == 1;
          counters.adminEnabled = static_cast<int>(entry.AdminStatus) == 1;
          const std::string name =
              !wide(row->FriendlyName).empty()
                  ? wide(row->FriendlyName)
                  : (row->AdapterName ? std::string(row->AdapterName) : std::string());
          if (name.empty())
            continue;
          out[name] = counters;
        }
        return true;
      }
#else
      std::string readTinyFile(const std::string &path)
      {
        std::string text;
        if (!ols::readFileText(path, text))
          return std::string();
        return trim(text);
      }

      void countersLinux(std::unordered_map<std::string, Counters> &out)
      {
        DIR *dir = opendir("/sys/class/net");
        if (!dir)
          return;
        while (dirent *entry = readdir(dir))
        {
          const std::string name = entry->d_name;
          if (name == "." || name == "..")
            continue;
          const std::string base = "/sys/class/net/" + name;
          Counters counters;
          const auto readStat = [&base](const char *file) -> unsigned long long {
            const std::string value = readTinyFile(base + "/statistics/" + file);
            return static_cast<unsigned long long>(
                std::strtoull(value.c_str(), nullptr, 10));
          };
          counters.rxBytes = readStat("rx_bytes");
          counters.txBytes = readStat("tx_bytes");
          counters.rxPkts = readStat("rx_packets");
          counters.txPkts = readStat("tx_packets");
          counters.rxErr = readStat("rx_errors");
          counters.txErr = readStat("tx_errors");
          counters.rxDrop = readStat("rx_dropped");
          counters.txDrop = readStat("tx_dropped");
          const std::string oper = lower(readTinyFile(base + "/operstate"));
          const std::string carrier = readTinyFile(base + "/carrier");
          counters.operUp = oper == "up" || carrier == "1";
          // The flags file is a hex bitfield; IFF_UP is 0x100.
          const std::string flags = readTinyFile(base + "/flags");
          const unsigned long bits = std::strtoul(flags.c_str(), nullptr, 16);
          counters.adminEnabled = (bits & 0x100UL) != 0;
          out[name] = counters;
        }
        closedir(dir);
      }
#endif

      void collectCounters(std::unordered_map<std::string, Counters> &out)
      {
#ifdef _WIN32
        countersWin(out);
#else
        countersLinux(out);
#endif
      }

      // The sampler keeps the bandwidth history and the up-times a person
      // watches: every two seconds it reads the counters once more and works
      // out the rates between two peeks, so the page draws real lines from the
      // first glance.
      void samplerBody()
      {
        State &s = state();
        while (s.running.load())
        {
          std::unordered_map<std::string, Counters> counters;
          collectCounters(counters);
          const long long atMs = steadyMs();
          const long long epoch = nowSeconds();
          std::lock_guard<std::mutex> lock(s.watchMutex);
          for (auto &item : counters)
          {
            AdapterWatch &watch = s.watches[item.first];
            const bool up = item.second.operUp;
            if (watch.firstSeen)
            {
              watch.firstSeen = false;
              watch.up = up;
              if (up)
              {
                // Standing already when this server began: it has been up at
                // least since now, and the page says so honestly.
                watch.upSinceEpoch = epoch;
                watch.upMeasured = false;
              }
            }
            else if (up != watch.up)
            {
              watch.up = up;
              if (up)
              {
                watch.upSinceEpoch = epoch;
                watch.upMeasured = true;
              }
            }
            if (watch.hadPrevious && atMs > watch.lastAtMs)
            {
              const double seconds = static_cast<double>(atMs - watch.lastAtMs) / 1000.0;
              const Counters &now = item.second;
              const Counters &was = watch.last;
              RatePoint point;
              point.atS = epoch;
              point.rxRate = now.rxBytes > was.rxBytes
                                 ? static_cast<double>(now.rxBytes - was.rxBytes) / seconds
                                 : 0.0;
              point.txRate = now.txBytes > was.txBytes
                                 ? static_cast<double>(now.txBytes - was.txBytes) / seconds
                                 : 0.0;
              point.rxPps = now.rxPkts > was.rxPkts
                                ? static_cast<double>(now.rxPkts - was.rxPkts) / seconds
                                : 0.0;
              point.txPps = now.txPkts > was.txPkts
                                ? static_cast<double>(now.txPkts - was.txPkts) / seconds
                                : 0.0;
              watch.history.push_back(point);
              if (watch.history.size() > 120)
                watch.history.pop_front();
            }
            watch.last = item.second;
            watch.hadPrevious = true;
            watch.lastAtMs = atMs;
            watch.lastSeenMs = atMs;
          }
          for (auto it = s.watches.begin(); it != s.watches.end();)
            if (atMs - it->second.lastSeenMs > 60000)
              it = s.watches.erase(it);
            else
              ++it;
          for (int i = 0; i < 20 && s.running.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }

      void initBody()
      {
        State &s = state();
        bool expected = false;
        if (s.running.compare_exchange_strong(expected, true))
        {
          s.sampler = std::thread(samplerBody);
          s.sampler.detach();
        }
      }

      // ---------- who and where this machine is ----------
#ifdef _WIN32
      std::string windowsOsName()
      {
        std::string product;
        std::string release;
        std::string build;
        {
          HKEY key = nullptr;
          if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                            KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS)
          {
            char text[256] = {0};
            DWORD size = sizeof(text);
            DWORD type = 0;
            if (RegQueryValueExA(key, "ProductName", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(text), &size) == ERROR_SUCCESS &&
                type == REG_SZ)
              product = text;
            char rel[64] = {0};
            DWORD relSize = sizeof(rel);
            DWORD relType = 0;
            // DisplayVersion names the release the machine stands on; ReleaseId
            // stopped naming one after 2009, so it is read only where the
            // machine is too old to carry DisplayVersion at all.
            if (RegQueryValueExA(key, "DisplayVersion", nullptr, &relType,
                                 reinterpret_cast<LPBYTE>(rel), &relSize) == ERROR_SUCCESS &&
                relType == REG_SZ)
              release = rel;
            else if (RegQueryValueExA(key, "ReleaseId", nullptr, &relType,
                                      reinterpret_cast<LPBYTE>(rel), &relSize) == ERROR_SUCCESS &&
                     relType == REG_SZ && std::string(rel) != "2009")
              release = rel;
            char bn[64] = {0};
            DWORD bnSize = sizeof(bn);
            DWORD bnType = 0;
            if (RegQueryValueExA(key, "CurrentBuildNumber", nullptr, &bnType,
                                 reinterpret_cast<LPBYTE>(bn), &bnSize) == ERROR_SUCCESS &&
                bnType == REG_SZ)
              build = bn;
            RegCloseKey(key);
          }
        }
        // The stored name lies, and does it on purpose: Microsoft froze the
        // ProductName value at "Windows 10" for programs that would not grow
        // past it, and it answers that still on machines whose own build
        // number says eleven. The build number is the truth of the family:
        // 22000 opened Windows 11, whatever the stored words insist.
        const long buildNumber = std::strtol(build.empty() ? "0" : build.c_str(), nullptr, 10);
        if (buildNumber >= 22000 && product.rfind("Windows 10", 0) == 0)
          product.replace(0, 10, "Windows 11");
        if (!product.empty() && !release.empty())
          product += " " + release;
        if (!product.empty() && !build.empty())
          product += " (build " + build + ")";
        if (product.empty())
          product = "Windows";
        return product;
      }

      std::string hostNameWin()
      {
        char text[MAX_COMPUTERNAME_LENGTH + 1] = {0};
        DWORD size = sizeof(text);
        if (GetComputerNameExA(ComputerNameDnsHostname, text, &size))
          return text;
        return "this machine";
      }

      bool elevatedWin()
      {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
          return false;
        TOKEN_ELEVATION elevation{};
        elevation.TokenIsElevated = 0;
        DWORD returned = 0;
        const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                            sizeof(elevation), &returned);
        CloseHandle(token);
        return ok && elevation.TokenIsElevated != 0;
      }
#else
      std::string linuxPrettyName()
      {
        std::string text;
        if (ols::readFileText("/etc/os-release", text))
        {
          for (const std::string &line : splitLines(text))
          {
            std::string value = fieldAfter(line, "PRETTY_NAME");
            if (value.empty())
              value = fieldAfter(line, "NAME");
            if (!value.empty())
            {
              if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);
              return value;
            }
          }
        }
        return "Linux";
      }

      std::string hostNameLinux()
      {
        char text[256] = {0};
        if (gethostname(text, sizeof(text) - 1) == 0)
          return text;
        return "this machine";
      }

      bool commandExists(const std::string &command)
      {
        if (!safeShellPart(command, false))
          return false;
        const Run run = runShell("command -v " + command + " >/dev/null 2>&1 && echo yes");
        return trim(run.out) == "yes";
      }
#endif

      // ---------- the adapters and their settings ----------
#ifdef _WIN32
      // The interface-status numbers are the stable contract between kernel,
      // iphlpapi and this file; the enum spellings move between sdk headers, so
      // the switch keeps the numbers and states what they have always meant.
      std::string operStatusText(int status)
      {
        switch (status)
        {
        case 1:
          return "up";
        case 2:
          return "down";
        case 3:
          return "testing";
        case 4:
          return "dormant";
        case 5:
          return "not-present";
        case 6:
          return "lower-layer-down";
        default:
          return "unknown";
        }
      }

      std::string macText(const unsigned char *bytes, unsigned long length)
      {
        if (!bytes || length == 0 || length > 64)
          return std::string();
        static const char *digits = "0123456789abcdef";
        std::string out;
        for (unsigned long i = 0; i < length; ++i)
        {
          if (i)
            out += ':';
          out += digits[bytes[i] >> 4];
          out += digits[bytes[i] & 0x0f];
        }
        return out;
      }

      std::string sockaddrText(const sockaddr *address)
      {
        if (!address)
          return std::string();
        char text[INET6_ADDRSTRLEN] = {0};
        if (address->sa_family == AF_INET)
        {
          const sockaddr_in *in = reinterpret_cast<const sockaddr_in *>(address);
          if (InetNtopA(AF_INET, &in->sin_addr, text, sizeof(text)))
            return text;
        }
        else if (address->sa_family == AF_INET6)
        {
          const sockaddr_in6 *in6 = reinterpret_cast<const sockaddr_in6 *>(address);
          if (InetNtopA(AF_INET6, &in6->sin6_addr, text, sizeof(text)))
            return text;
        }
        return std::string();
      }

      // What kind of thing this adapter is: the description carries most of the
      // truth, and the interface type settles the rest.
      std::string kindWin(ULONG ifType, const std::string &description)
      {
        if (containsFold(description, "loopback"))
          return "loopback";
        if (containsFold(description, "teaming"))
          return "bond";
        if (containsFold(description, "vlan"))
          return "vlan";
        if (containsFold(description, "bridge"))
          return "bridge";
        if (containsFold(description, "tunnel") || containsFold(description, "isatap") ||
            containsFold(description, "teredo") || containsFold(description, "6to4"))
          return "tunnel";
        if (containsFold(description, "wireguard") || containsFold(description, "tap") ||
            containsFold(description, "vpn") || containsFold(description, "virtual"))
          return "virtual";
        switch (ifType)
        {
        case IF_TYPE_ETHERNET_CSMACD:
          return "ethernet";
        case IF_TYPE_IEEE80211:
          return "wifi";
        case IF_TYPE_PPP:
        case IF_TYPE_SLIP:
          return "tunnel";
        case IF_TYPE_IEEE1394:
          return "virtual";
        case 135: // broadband
        case 237: // wimax
          return "virtual";
        default:
          if (ifType == IF_TYPE_SOFTWARE_LOOPBACK)
            return "loopback";
          return "virtual";
        }
      }

      // Windows keeps the answer to "does dhcp own this interface?" right in the
      // registry, under the adapter's own guid.
      json dhcpEnabledWin(const std::string &adapterGuid)
      {
        if (adapterGuid.size() > 128)
          return json(nullptr);
        HKEY key = nullptr;
        const std::string path =
            "SYSTEM\\CurrentControlSet\\Services\\TCPIP\\Parameters\\Interfaces\\" + adapterGuid;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY,
                          &key) != ERROR_SUCCESS)
          return json(nullptr);
        DWORD value = 0;
        DWORD size = sizeof(value);
        DWORD type = 0;
        const json out = RegQueryValueExA(key, "DhcpEnabled", nullptr, &type,
                                          reinterpret_cast<LPBYTE>(&value), &size) ==
                                 ERROR_SUCCESS &&
                                 type == REG_DWORD
                             ? json(value == 1)
                             : json(nullptr);
        RegCloseKey(key);
        return out;
      }

      void collectAdaptersWin(json &adapters)
      {
        const ULONG family = AF_UNSPEC;
        // Keep the unicast addresses (the settings themselves), the gateways,
        // the prefix lengths and the dns servers - and keep the friendly name,
        // the one alias every other part of Windows agrees on.
        const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                            GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_PREFIX;
        ULONG size = 0;
        if (GetAdaptersAddresses(family, flags, nullptr, nullptr, &size) !=
                ERROR_BUFFER_OVERFLOW ||
            size == 0)
          return;
        std::vector<unsigned char> buffer(size);
        PIP_ADAPTER_ADDRESSES list =
            reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        if (GetAdaptersAddresses(family, flags, nullptr, list, &size) != NO_ERROR)
          return;
        for (PIP_ADAPTER_ADDRESSES row = list; row; row = row->Next)
        {
          if (row->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
          MIB_IF_ROW2 entry{};
          entry.InterfaceLuid = row->Luid;
          entry.InterfaceIndex = row->IfIndex;
          const bool haveEntry = GetIfEntry2(&entry) == NO_ERROR;
          const std::string name = !wide(row->FriendlyName).empty()
                                       ? wide(row->FriendlyName)
                                       : (row->AdapterName ? row->AdapterName : "");
          if (name.empty())
            continue;
          const std::string description = wide(row->Description);
          json adapter;
          adapter["name"] = name;
          adapter["if_index"] = static_cast<long long>(row->IfIndex);
          adapter["description"] = description;
          adapter["kind"] = kindWin(row->IfType, description);
          adapter["admin"] = haveEntry ? static_cast<int>(entry.AdminStatus) == 1 : true;
          adapter["oper"] =
              haveEntry ? operStatusText(static_cast<int>(entry.OperStatus)) : "unknown";
          adapter["mac"] = macText(row->PhysicalAddress, row->PhysicalAddressLength);
          // Link speed is stated in bits per second; an absent link is
          // "nothing", not zero - 4294967295 is the machine's own "unknown".
          long long speed = -1;
          if (haveEntry && entry.TransmitLinkSpeed != 0 &&
              entry.TransmitLinkSpeed != 4294967295ULL)
            speed = static_cast<long long>(entry.TransmitLinkSpeed);
          adapter["speed_bps"] = speed;
          adapter["media"] = "";
          adapter["dns_suffix"] = wide(row->DnsSuffix);
          json ipv4 = json::array();
          json ipv6 = json::array();
          for (PIP_ADAPTER_UNICAST_ADDRESS address = row->FirstUnicastAddress; address;
               address = address->Next)
          {
            const std::string text = sockaddrText(address->Address.lpSockaddr);
            if (text.empty())
              continue;
            json item;
            item["address"] = text;
            const int prefix = static_cast<int>(address->OnLinkPrefixLength);
            item["prefix"] = prefix > 0 ? prefix : -1;
            if (address->Address.lpSockaddr->sa_family == AF_INET)
              ipv4.push_back(item);
            else if (address->Address.lpSockaddr->sa_family == AF_INET6)
              ipv6.push_back(item);
          }
          adapter["ipv4"] = ipv4;
          adapter["ipv6"] = ipv6;
          std::string gateway;
          if (row->FirstGatewayAddress)
            gateway = sockaddrText(row->FirstGatewayAddress->Address.lpSockaddr);
          adapter["gateway"] = gateway;
          json dns = json::array();
          for (PIP_ADAPTER_DNS_SERVER_ADDRESS server = row->FirstDnsServerAddress; server;
               server = server->Next)
          {
            const std::string text = sockaddrText(server->Address.lpSockaddr);
            if (!text.empty())
              dns.push_back(text);
          }
          adapter["dns"] = dns;
          adapter["dns_via_resolved"] = false;
          // The ip interface row carries both the metric and the mtu the
          // machine's ip stack runs this interface at.
          long long metric = -1;
          long long mtu = -1;
          MIB_IPINTERFACE_ROW ipRow{};
          ipRow.Family = AF_INET;
          ipRow.InterfaceLuid = row->Luid;
          ipRow.InterfaceIndex = row->IfIndex;
          if (GetIpInterfaceEntry(&ipRow) == NO_ERROR)
          {
            metric = static_cast<long long>(ipRow.Metric);
            mtu = static_cast<long long>(ipRow.NlMtu);
          }
          else
          {
            MIB_IPINTERFACE_ROW six{};
            six.Family = AF_INET6;
            six.InterfaceLuid = row->Luid;
            six.InterfaceIndex = row->IfIndex;
            if (GetIpInterfaceEntry(&six) == NO_ERROR)
            {
              metric = static_cast<long long>(six.Metric);
              mtu = static_cast<long long>(six.NlMtu);
            }
          }
          adapter["metric"] = metric;
          adapter["mtu"] = mtu;
          adapter["dhcp"] = dhcpEnabledWin(row->AdapterName ? row->AdapterName : "");
          adapter["managed_by_this_server"] = false;
          adapter["vlan"] = nullptr;
          adapter["master"] = nullptr;
          adapter["slaves"] = json::array();
          adapters.push_back(adapter);
        }
      }

      json wifiStatusWin()
      {
        State &s = state();
        {
          std::lock_guard<std::mutex> lock(s.wifiStatus.mutex);
          if (s.wifiStatus.fresh(2000))
            return s.wifiStatus.value;
        }
        json out = json::array();
        const Run shown = runShell("netsh wlan show interfaces");
        if (shown.code == 0)
        {
          json entry;
          bool open = false;
          auto flush = [&]()
          {
            if (open && entry.is_object())
              out.push_back(entry);
            entry = json();
            open = false;
          };
          for (const std::string &line : splitLines(shown.out))
          {
            std::string value;
            if (!(value = fieldAfter(line, "Name")).empty())
            {
              flush();
              entry = json::object();
              entry["name"] = value;
              entry["state"] = "disconnected";
              entry["ssid"] = "";
              entry["bssid"] = "";
              entry["band"] = "";
              entry["channel"] = -1;
              entry["signal"] = -1;
              entry["rx_rate"] = -1.0;
              entry["tx_rate"] = -1.0;
              entry["radio"] = "";
              entry["auth"] = "";
              entry["cipher"] = "";
              entry["profile"] = "";
              open = true;
              continue;
            }
            if (!open)
              continue;
            if (!(value = fieldAfter(line, "State")).empty())
              entry["state"] = lower(value);
            else if (!(value = fieldAfter(line, "SSID")).empty())
              entry["ssid"] = value;
            // This netsh names the associated access point's address on an
            // "AP BSSID" line; the colocated-AP lines under it open with a
            // compact "BSSID:" and are somebody else's address.
            else if (!(value = fieldExact(line, "AP BSSID")).empty())
              entry["bssid"] = value;
            else if (!(value = fieldExact(line, "Radio type")).empty())
              entry["radio"] = value;
            else if (!(value = fieldAfter(line, "Authentication")).empty())
              entry["auth"] = value;
            else if (!(value = fieldAfter(line, "Cipher")).empty())
              entry["cipher"] = value;
            else if (!(value = fieldExact(line, "Band")).empty())
              entry["band"] = value;
            else if (!(value = fieldExact(line, "Channel")).empty())
              entry["channel"] = toNumber(value, -1);
            else if (!(value = fieldAfter(line, "Receive rate (Mbps)")).empty())
              entry["rx_rate"] = std::strtod(value.c_str(), nullptr);
            else if (!(value = fieldAfter(line, "Transmit rate (Mbps)")).empty())
              entry["tx_rate"] = std::strtod(value.c_str(), nullptr);
            else if (!(value = fieldAfter(line, "Signal")).empty())
              entry["signal"] = toNumber(value, -1);
            else if (!(value = fieldAfter(line, "Profile")).empty())
              entry["profile"] = value;
          }
          flush();
        }
        std::lock_guard<std::mutex> lock(s.wifiStatus.mutex);
        s.wifiStatus.value = out;
        s.wifiStatus.filled = true;
        s.wifiStatus.atMs = steadyMs();
        return out;
      }

      std::string firstWifiNameWin()
      {
        for (const json &entry : wifiStatusWin())
          if (entry.contains("name") && entry["name"].is_string())
            return entry["name"].get<std::string>();
        return std::string();
      }

      // The access point of this machine is the mobile hotspot Windows runs -
      // the tethering manager's, the very one the machine's own settings page
      // pushes. The legacy hosted network is not that: its netsh commands
      // still answer while the radio behind them went away years ago, so this
      // asks the tethering manager directly. WinRT's async answers do not
      // always project into a shell's hands, so the calls here are fired and
      // the manager's own state is watched until it settles: what the machine
      // did stands above the object it handed back.
      const char *kApScript = R"OLSAP(
param([string]$mode = 'state')
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }
$ErrorActionPreference = 'Stop'
$out = [ordered]@{ ok = $true; error = ''; state = ''; ssid = ''; band = ''; auth = ''; maxClients = -1; clients = @() }
function Get-Inner($e) { $t = $e; while ($t.InnerException) { $t = $t.InnerException }; return $t.Message }
try {
  $tmType = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager,Windows.Networking.NetworkOperators,ContentType=WindowsRuntime]
  [void][Windows.Networking.Connectivity.NetworkInformation,Windows.Networking.Connectivity,ContentType=WindowsRuntime]
  $cp = [Windows.Networking.Connectivity.NetworkInformation]::GetInternetConnectionProfile()
  if (-not $cp) { throw 'this machine runs no Internet connection for a hotspot to share' }
  $cap = [string]$tmType::GetTetheringCapabilityFromConnectionProfile($cp)
  if ($cap -ne 'Enabled') { throw ('the machine says its tethering is ' + $cap) }
  $m = $tmType::CreateFromConnectionProfile($cp)
  if ($mode -eq 'start') {
    $a = @('', '', '')
    if (Test-Path 'data\ap-args.txt') { $a = ([IO.File]::ReadAllText('data\ap-args.txt')) -split "`n" }
    $ssid = ([string]$a[0]).TrimEnd("`r")
    $psk = ([string]$a[1]).TrimEnd("`r")
    $band = ([string]$a[2]).TrimEnd("`r")
    $cfg = $m.GetCurrentAccessPointConfiguration()
    $changed = $false
    if ($ssid -ne '' -and $cfg.Ssid -ne $ssid) { $cfg.Ssid = $ssid; $changed = $true }
    if ($psk -ne '' -and $cfg.Passphrase -ne $psk) { $cfg.Passphrase = $psk; $changed = $true }
    if ($band -ne '' -and [string]$cfg.Band -ne $band) { $cfg.Band = $band; $changed = $true }
    if ($changed) {
      $null = $m.ConfigureAccessPointAsync($cfg)
      $deadline = (Get-Date).AddSeconds(10)
      while ((Get-Date) -lt $deadline) {
        $c2 = $m.GetCurrentAccessPointConfiguration()
        if (($ssid -eq '' -or $c2.Ssid -eq $ssid) -and ($band -eq '' -or [string]$c2.Band -eq $band)) { break }
        Start-Sleep -Milliseconds 200
      }
    }
    $null = $m.StartTetheringAsync()
  } elseif ($mode -eq 'stop') {
    $null = $m.StopTetheringAsync()
  }
  if ($mode -ne 'state') {
    $goal = 'Off'
    if ($mode -eq 'start') { $goal = 'On' }
    $deadline = (Get-Date).AddSeconds(45)
    $state = [string]$m.TetheringOperationalState
    while ($state -ne $goal) {
      if ((Get-Date) -ge $deadline) { break }
      Start-Sleep -Milliseconds 300
      $state = [string]$m.TetheringOperationalState
    }
    if ($state -ne $goal) { throw ('the machine says ' + $state + ' where it should say ' + $goal) }
  }
  $c = $m.GetCurrentAccessPointConfiguration()
  $out.state = [string]$m.TetheringOperationalState
  $out.ssid = [string]$c.Ssid
  $out.band = [string]$c.Band
  $out.auth = [string]$c.AuthenticationKind
  $out.maxClients = [int]$m.MaxClientCount
  foreach ($cl in $m.GetTetheringClients()) {
    $name = ''
    foreach ($p in $cl.GetType().GetProperties()) {
      $v = $null
      try { $v = $p.GetValue($cl) } catch { continue }
      if ($v -is [string] -and [string]$v -ne '') { $name = [string]$v; break }
    }
    $out.clients += $name
  }
} catch {
  $out.ok = $false
  $out.error = [string](Get-Inner $_.Exception)
}
$out | ConvertTo-Json -Compress
)OLSAP";

      std::string apScriptPath()
      {
        return "data\\ap.ps1";
      }

      std::string apArgsPath()
      {
        return "data\\ap-args.txt";
      }

      // The ssid and its key pass to the script in a file, never in a command
      // line where every other program could read them; what the file held is
      // wiped as soon as the script has answered.
      void forgetApArgs()
      {
        ols::writeFileText(apArgsPath(), "");
        std::error_code ec{};
        std::filesystem::remove(apArgsPath(), ec);
      }

      // The script answers in json on one line; its own words about a failure
      // are that line's error field, and they come back whole.
      json apRunAnswered(const Run &run, json &parsed)
      {
        parsed = json::object();
        if (!run.ran)
          return json();
        std::string text = run.out;
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
          text = text.substr(3);
        json candidate = json::parse(text, nullptr, false);
        if (candidate.is_discarded() || !candidate.is_object())
          return json();
        parsed = candidate;
        if (!parsed.value("ok", false))
          return json();
        return parsed;
      }

      Run runApScript(const std::string &mode)
      {
        std::error_code ec{};
        std::filesystem::create_directories("data", ec);
        if (!ols::writeFileText(apScriptPath(), kApScript))
        {
          Run failed;
          failed.out = "{\"ok\":false,\"error\":\"this server could not write its tethering "
                       "script beside itself\"}";
          return failed;
        }
        return runShell("powershell -NoProfile -ExecutionPolicy Bypass -File " + apScriptPath() +
                        " " + mode);
      }

      // The answer the page reads about the access point: the same shape the
      // panel already knows (supported, reason, active, ssid, band, clients),
      // filled from the tethering manager's own words.
      json accessPointWin()
      {
        json out;
        out["supported"] = false;
        out["reason"] = "this machine did not answer the tethering manager";
        out["active"] = false;
        out["interface"] = "";
        out["ssid"] = "";
        out["band"] = "";
        out["channel"] = -1;
        out["clients"] = json::array();
        json parsed;
        const Run run = runApScript("state");
        if (apRunAnswered(run, parsed).is_null())
        {
          const std::string words =
              parsed.is_object() ? parsed.value("error", std::string("")) : trim(run.out);
          if (!words.empty())
            out["reason"] = words;
          return out;
        }
        const std::string apState = parsed.value("state", std::string(""));
        out["supported"] = true;
        out["reason"] = "";
        // A hotspot the machine is still raising or lowering counts as stood
        // up: its clients and its channel are the machine's, and the hand to
        // take it down has to answer for the in-between too.
        out["active"] = !apState.empty() && apState != "Off";
        out["state"] = apState;
        out["ssid"] = parsed.value("ssid", std::string(""));
        out["band"] = parsed.value("band", std::string(""));
        out["auth"] = parsed.value("auth", std::string(""));
        out["max_clients"] = parsed.value("maxClients", -1);
        if (parsed.contains("clients") && parsed["clients"].is_array())
          out["clients"] = parsed["clients"];
        return out;
      }

      json sharingWin()
      {
        json out;
        out["mode"] = "ics";
        out["service"] = "SharedAccess";
        out["supported"] = false;
        out["reason"] = "";
        out["active"] = nullptr;
        const Run svc = runShell("sc query SharedAccess");
        const bool installed =
            svc.ran && svc.out.find("SERVICE_NAME") != std::string::npos;
        if (!installed)
        {
          if (svc.out.find("1060") != std::string::npos)
            out["reason"] = "this machine has no Internet Connection Sharing service (Windows "
                            "ships it only on some editions); the machine's own settings offer "
                            "mobile hotspot instead, and this server cannot reach it";
          else
            out["reason"] = "this machine would not answer about the sharing service";
          return out;
        }
        out["supported"] = true;
        const bool running = svc.out.find("RUNNING") != std::string::npos;
        const json stored = loadStateFile();
        if (stored.is_object() && stored.contains("sharing") && stored["sharing"].is_object())
        {
          json active;
          active["method"] = "ics";
          active["service"] = running ? "running" : "stopped";
          for (const auto &field : stored["sharing"].items())
            active[field.key()] = field.value();
          out["active"] = active;
        }
        return out;
      }
#else
      long long channelFromFreq(long long freq, std::string &band)
      {
        if (freq >= 2400 && freq <= 2500)
        {
          band = "2.4";
          if (freq == 2484)
            return 14;
          return (freq - 2407) / 5;
        }
        if (freq >= 4910 && freq <= 4990)
        {
          band = "5";
          return (freq - 5000) / 5 + 60; // the low bands under 5 GHz proper
        }
        if (freq >= 5150 && freq <= 5895)
        {
          band = "5";
          return (freq - 5000) / 5;
        }
        if (freq >= 5925 && freq <= 7125)
        {
          band = "6";
          return (freq - 5950) / 5;
        }
        if (freq >= 57240 && freq <= 70200)
        {
          band = "60";
          return (freq - 56160) / 2160;
        }
        band = "";
        return -1;
      }

      std::string kindLinux(const std::string &name, const std::string &master)
      {
        std::error_code ec{};
        const std::string base = "/sys/class/net/" + name;
        if (std::filesystem::exists(base + "/wireless", ec) ||
            std::filesystem::exists(base + "/phy80211", ec))
          return "wifi";
        if (std::filesystem::exists(base + "/bonding", ec) || name.rfind("bond", 0) == 0)
          return "bond";
        if (std::filesystem::exists(base + "/bridge", ec))
          return "bridge";
        if (std::filesystem::exists("/proc/net/vlan/" + name, ec) ||
            std::filesystem::exists(base + "/lower_", ec) ||
            (name.find('.') != std::string::npos && name.find_first_of("0123456789") != std::string::npos &&
             isdigit(static_cast<unsigned char>(name.back()))))
          return "vlan";
        if (name == "lo")
          return "loopback";
        if (name.rfind("tun", 0) == 0 || name.rfind("tap", 0) == 0 ||
            name.rfind("ipip", 0) == 0 || name.rfind("sit", 0) == 0 ||
            name.rfind("ip6", 0) == 0 || name.rfind("gre", 0) == 0 ||
            name.rfind("gretap", 0) == 0 || name.rfind("erspan", 0) == 0 ||
            name.rfind("vxlan", 0) == 0 || name.rfind("ip_vti", 0) == 0 ||
            name.rfind("ppp", 0) == 0 || name.rfind("l2tp", 0) == 0 ||
            name.rfind("wg", 0) == 0)
          return "tunnel";
        if (name.rfind("veth", 0) == 0 || name.rfind("docker", 0) == 0 || name == "br-0" ||
            name.rfind("br-", 0) == 0 || name == "virbr0" || name.rfind("cni", 0) == 0 ||
            name.rfind("flannel", 0) == 0 || name.rfind("kube", 0) == 0 ||
            name.rfind("tailscale", 0) == 0 || name.rfind("utun", 0) == 0 ||
            name.rfind("wwan", 0) == 0 || name.rfind("usb", 0) == 0)
          return "virtual";
        if (!master.empty())
          return "virtual"; // a member: what it belongs to says what it is
        return "ethernet";
      }

      // vlan identity: the kernel's own file says the id, and the sysfs
      // lower_<name> entry says whose back it stands on.
      void vlanInfoLinux(const std::string &name, long long &id, std::string &parent)
      {
        id = -1;
        parent.clear();
        std::error_code ec{};
        if (std::filesystem::exists("/proc/net/vlan/" + name, ec))
        {
          std::string text;
          if (ols::readFileText("/proc/net/vlan/" + name, text))
            for (const std::string &line : splitLines(text))
            {
              const std::string value = fieldAfter(line, "VID");
              if (!value.empty())
              {
                id = toNumber(value, -1);
                break;
              }
            }
        }
        DIR *dir = opendir(("/sys/class/net/" + name).c_str());
        if (dir)
        {
          while (dirent *entry = readdir(dir))
          {
            const std::string n = entry->d_name;
            if (n.rfind("lower_", 0) == 0)
            {
              parent = n.substr(6);
              break;
            }
          }
          closedir(dir);
        }
      }

      std::string hexIp(const std::string &hex)
      {
        const unsigned long value = std::strtoul(hex.c_str(), nullptr, 16);
        in_addr addr{};
        addr.s_addr = static_cast<uint32_t>(value);
        char text[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &addr, text, sizeof(text)))
          return text;
        return std::string();
      }

      void collectAdaptersLinux(json &adapters)
      {
        // One pass of getifaddrs for the addresses and flags, sysfs for the
        // rest. The default gateway and the system's dns stand with the
        // interface that carries them, and nowhere else.
        std::unordered_map<std::string, json> byName;
        std::unordered_map<std::string, bool> adminUp;
        ifaddrs *list = nullptr;
        if (getifaddrs(&list) == 0)
        {
          for (ifaddrs *row = list; row; row = row->ifa_next)
          {
            const std::string name = row->ifa_name ? row->ifa_name : "";
            if (name.empty())
              continue;
            json &adapter = byName[name];
            if (adapter.is_null())
            {
              adapter = json::object();
              adapter["name"] = name;
              adapter["ipv4"] = json::array();
              adapter["ipv6"] = json::array();
            }
            adminUp[name] = (row->ifa_flags & IFF_UP) != 0;
            if (!row->ifa_addr)
              continue;
            char text[INET6_ADDRSTRLEN] = {0};
            if (row->ifa_addr->sa_family == AF_INET)
            {
              const sockaddr_in *in = reinterpret_cast<const sockaddr_in *>(row->ifa_addr);
              if (!inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)))
                continue;
              json item;
              item["address"] = text;
              int prefix = -1;
              if (row->ifa_netmask && row->ifa_netmask->sa_family == AF_INET)
              {
                unsigned long mask = ntohl(
                    reinterpret_cast<const sockaddr_in *>(row->ifa_netmask)->sin_addr.s_addr);
                prefix = 0;
                for (; mask; mask <<= 1)
                  ++prefix;
              }
              item["prefix"] = prefix;
              adapter["ipv4"].push_back(item);
            }
            else if (row->ifa_addr->sa_family == AF_INET6)
            {
              const sockaddr_in6 *in6 = reinterpret_cast<const sockaddr_in6 *>(row->ifa_addr);
              if (!inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof(text)))
                continue;
              json item;
              item["address"] = text;
              int prefix = -1;
              if (row->ifa_netmask && row->ifa_netmask->sa_family == AF_INET6)
              {
                const unsigned char *bytes =
                    reinterpret_cast<const sockaddr_in6 *>(row->ifa_netmask)->sin6_addr.s6_addr;
                prefix = 0;
                for (int i = 0; i < 16; ++i)
                  for (int bit = 7; bit >= 0; --bit)
                    if ((bytes[i] >> bit) & 1)
                      ++prefix;
              }
              item["prefix"] = prefix;
              adapter["ipv6"].push_back(item);
            }
          }
          freeifaddrs(list);
        }

        std::string gateway;
        std::string gatewayIface;
        {
          std::ifstream in{"/proc/net/route"};
          std::string line;
          std::getline(in, line); // the header line
          while (std::getline(in, line))
          {
            std::istringstream words{line};
            std::string iface, dest, gw;
            words >> iface >> dest >> gw;
            if (dest == "00000000")
            {
              gatewayIface = iface;
              gateway = hexIp(gw);
              break;
            }
          }
        }
        json dns = json::array();
        bool dnsViaResolved = false;
        {
          std::ifstream in{"/etc/resolv.conf"};
          std::string line;
          while (std::getline(in, line))
          {
            std::istringstream words{line};
            std::string keyword, value;
            words >> keyword >> value;
            if (keyword != "nameserver" || value.empty())
              continue;
            if (value == "127.0.0.53" || value == "127.0.0.54")
              dnsViaResolved = true;
            else
              dns.push_back(value);
          }
        }

        std::vector<std::string> names;
        DIR *dir = opendir("/sys/class/net");
        if (dir)
        {
          while (dirent *entry = readdir(dir))
          {
            const std::string name = entry->d_name;
            if (name != "." && name != ".." && name != "lo")
              names.push_back(name);
          }
          closedir(dir);
        }
        std::sort(names.begin(), names.end());
        for (const std::string &name : names)
        {
          const std::string base = "/sys/class/net/" + name;
          const auto known = byName.find(name);
          json adapter = known != byName.end() ? known->second : json::object();
          adapter["name"] = name;
          adapter["if_index"] = -1;

          std::error_code ec{};
          std::string master;
          if (std::filesystem::exists(base + "/master", ec))
          {
            const std::filesystem::path link =
                std::filesystem::read_symlink(base + "/master", ec);
            if (!ec)
              master = link.filename().string();
          }
          std::string description;
          const std::filesystem::path driver =
              std::filesystem::read_symlink(base + "/device/driver", ec);
          if (!ec)
            description = driver.filename().string();
          const std::string kind = kindLinux(name, master);
          if (description.empty())
            description = kind;
          adapter["description"] = description;
          adapter["kind"] = kind;
          adapter["master"] = master.empty() ? json(nullptr) : json(master);
          json slaves = json::array();
          if (kind == "bond" || kind == "bridge")
          {
            // whose back this bond stands on, from the kernel's own list
            const std::string listFile = kind == "bond" ? "/bonding/slaves" : "/brif";
            const std::string listed = readTinyFile(base + listFile);
            std::istringstream words{listed};
            std::string word;
            while (words >> word)
              slaves.push_back(word);
          }
          adapter["slaves"] = slaves;

          const std::string adminFlags = readTinyFile(base + "/flags");
          const unsigned long bits = std::strtoul(adminFlags.c_str(), nullptr, 16);
          bool admin = (bits & 0x100UL) != 0;
          const auto adminFound = adminUp.find(name);
          if (adminFound != adminUp.end())
            admin = adminFound->second;
          adapter["admin"] = admin;

          std::string oper = lower(readTinyFile(base + "/operstate"));
          const std::string carrier = readTinyFile(base + "/carrier");
          if (oper.empty() || oper == "unknown")
            oper = carrier == "1" ? "up" : "down";
          else if (oper == "lowerlayerdown")
            oper = "lower-layer-down";
          adapter["oper"] = oper;

          adapter["mac"] = readTinyFile(base + "/address");
          adapter["mtu"] = toNumber(readTinyFile(base + "/mtu"), -1);
          const long long speedMbps = toNumber(readTinyFile(base + "/speed"), -1);
          adapter["speed_bps"] = speedMbps > 0 ? speedMbps * 1000000ll : -1;
          adapter["media"] = "";
          adapter["dns_suffix"] = "";
          if (!adapter.contains("ipv4"))
            adapter["ipv4"] = json::array();
          if (!adapter.contains("ipv6"))
            adapter["ipv6"] = json::array();
          adapter["gateway"] = name == gatewayIface ? gateway : std::string();
          adapter["dns"] = name == gatewayIface ? dns : json::array();
          adapter["dns_via_resolved"] = name == gatewayIface && dnsViaResolved;
          adapter["metric"] = -1;
          adapter["dhcp"] = nullptr; // netplan state, merged by the snapshot
          adapter["managed_by_this_server"] = false;
          long long vlanId = -1;
          std::string vlanParent;
          if (kind == "vlan")
            vlanInfoLinux(name, vlanId, vlanParent);
          adapter["vlan"] = vlanId >= 0
                                ? json{{"id", vlanId}, {"parent", vlanParent}}
                                : json(nullptr);
          adapters.push_back(adapter);
        }
      }

      json wifiStatusLinux()
      {
        State &s = state();
        {
          std::lock_guard<std::mutex> lock(s.wifiStatus.mutex);
          if (s.wifiStatus.fresh(2000))
            return s.wifiStatus.value;
        }
        json out = json::array();
        std::vector<std::string> names;
        DIR *dir = opendir("/sys/class/net");
        if (dir)
        {
          std::error_code ec{};
          while (dirent *entry = readdir(dir))
          {
            const std::string name = entry->d_name;
            if (name == "." || name == "..")
              continue;
            if (std::filesystem::exists("/sys/class/net/" + name + "/phy80211", ec))
              names.push_back(name);
          }
          closedir(dir);
        }
        std::sort(names.begin(), names.end());
        const bool haveIw = commandExists("iw");
        for (const std::string &name : names)
        {
          json entry;
          entry["name"] = name;
          entry["state"] = "down";
          entry["ssid"] = "";
          entry["bssid"] = "";
          entry["band"] = "";
          entry["channel"] = -1;
          entry["signal"] = -1;
          entry["rssi"] = 0;
          entry["rx_rate"] = -1.0;
          entry["tx_rate"] = -1.0;
          entry["radio"] = "";
          entry["auth"] = "";
          entry["cipher"] = "";
          entry["profile"] = "";
          entry["mode"] = "managed";
          if (haveIw)
          {
            const Run link = runShell("iw dev " + name + " link");
            if (link.code == 0 && link.out.find("Connected:") == 0)
            {
              entry["state"] = "connected";
              const std::string first = trim(link.out.substr(0, link.out.find('\n')));
              entry["bssid"] = trim(first.substr(10));
              for (const std::string &line : splitLines(link.out))
              {
                std::string value;
                if (!(value = fieldAfter(line, "SSID")).empty())
                  entry["ssid"] = value;
                else if (!(value = fieldAfter(line, "freq")).empty())
                {
                  std::string band;
                  entry["channel"] = channelFromFreq(toNumber(value, -1), band);
                  entry["band"] = band;
                }
                else if (!(value = fieldAfter(line, "signal")).empty())
                {
                  const long long rssi = static_cast<long long>(
                      std::strtod(value.c_str(), nullptr));
                  entry["rssi"] = rssi;
                  long long signal = (rssi + 100) * 2;
                  if (signal < 4)
                    signal = 4;
                  if (signal > 100)
                    signal = 100;
                  entry["signal"] = signal;
                }
                else if (!(value = fieldAfter(line, "rx bitrate")).empty())
                  entry["rx_rate"] = std::strtod(value.c_str(), nullptr);
                else if (!(value = fieldAfter(line, "tx bitrate")).empty())
                  entry["tx_rate"] = std::strtod(value.c_str(), nullptr);
              }
            }
            else
              entry["state"] = "disconnected";
            const Run info = runShell("iw dev " + name + " info");
            if (info.out.find("type AP") != std::string::npos)
              entry["mode"] = "access-point";
            else if (info.out.find("type managed") != std::string::npos)
              entry["mode"] = "managed";
            else
              entry["mode"] = "other";
          }
          out.push_back(entry);
        }
        std::lock_guard<std::mutex> lock(s.wifiStatus.mutex);
        s.wifiStatus.value = out;
        s.wifiStatus.filled = true;
        s.wifiStatus.atMs = steadyMs();
        return out;
      }

      std::string firstWifiNameLinux()
      {
        for (const json &entry : wifiStatusLinux())
          if (entry.contains("name") && entry["name"].is_string())
            return entry["name"].get<std::string>();
        return std::string();
      }

      bool networkManagerRunning()
      {
        static Cached<bool> cache;
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.fresh(30000))
          return cache.value;
        cache.value = commandExists("nmcli") &&
                      lower(runShell("nmcli general status").out).find("running") !=
                          std::string::npos;
        cache.filled = true;
        cache.atMs = steadyMs();
        return cache.value;
      }

      std::string apPidPath(const std::string &iface)
      {
        return "/run/ols-hostapd-" + iface + ".pid";
      }

      json apStateLinux()
      {
        State &s = state();
        {
          std::lock_guard<std::mutex> lock(s.apState.mutex);
          if (s.apState.fresh(4000))
            return s.apState.value;
        }
        const json features = featuresJson();
        json out;
        out["supported"] = features.value("hostapd", false);
        out["reason"] = features.value("hostapd_reason", std::string());
        out["active"] = false;
        out["interface"] = "";
        out["ssid"] = "";
        out["band"] = "";
        out["channel"] = -1;
        out["clients"] = json::array();
        const json stored = loadStateFile();
        if (stored.is_object() && stored.contains("ap") && stored["ap"].is_object())
        {
          const std::string iface = stored["ap"].value("interface", std::string());
          if (safeIfname(iface))
          {
            std::error_code ec{};
            const bool pidHere = std::filesystem::exists(apPidPath(iface), ec);
            const Run status = runShell("hostapd_cli -p /var/run/hostapd-" + iface +
                                        " -i " + iface + " status 2>/dev/null");
            if (pidHere || status.out.find("state=ENABLED") != std::string::npos)
            {
              out["active"] = true;
              out["interface"] = iface;
              for (const std::string &line : splitLines(status.out))
              {
                if (line.rfind("ssid[", 0) == 0)
                {
                  const size_t bracket = line.find(']');
                  if (bracket != std::string::npos)
                    out["ssid"] = trim(line.substr(bracket + 1));
                }
                else if (line.rfind("channel=", 0) == 0)
                  out["channel"] = toNumber(line.substr(8), -1);
              }
              const Run stations = runShell("hostapd_cli -p /var/run/hostapd-" + iface +
                                            " -i " + iface + " all_sta 2>/dev/null");
              for (const std::string &line : splitLines(stations.out))
              {
                // station lines start with the sixteen characters of a mac
                if (line.size() >= 17 && std::isxdigit(static_cast<unsigned char>(line[0])) &&
                    line.find(':') != std::string::npos)
                  out["clients"].push_back(line.substr(0, 17));
              }
            }
          }
        }
        std::lock_guard<std::mutex> lock(s.apState.mutex);
        s.apState.value = out;
        s.apState.filled = true;
        s.apState.atMs = steadyMs();
        return out;
      }

      json sharingLinux()
      {
        State &s = state();
        {
          std::lock_guard<std::mutex> lock(s.sharing.mutex);
          if (s.sharing.fresh(5000))
            return s.sharing.value;
        }
        const json features = featuresJson();
        json out;
        out["supported"] = features.value("sharing", false);
        out["reason"] = features.value("sharing_reason", std::string());
        out["mode"] = "nat";
        out["service"] = out["supported"].get<bool>() ? "iptables" : "absent";
        out["active"] = nullptr;
        if (out["supported"].get<bool>())
        {
          const Run nat = runShell("iptables -t nat -S POSTROUTING 2>/dev/null | grep OLS-ICS");
          if (nat.code == 0 && !trim(nat.out).empty())
          {
            json active;
            active["method"] = "masquerade";
            std::istringstream in{nat.out};
            std::string line;
            while (std::getline(in, line))
            {
              const size_t via = line.find(" -o ");
              if (via != std::string::npos)
              {
                std::istringstream words{line.substr(via + 4)};
                std::string iface;
                words >> iface;
                active["external"] = iface;
              }
            }
            active["forwarding"] = readTinyFile("/proc/sys/net/ipv4/ip_forward") == "1";
            const json stored = loadStateFile();
            if (stored.is_object() && stored.contains("sharing") &&
                stored["sharing"].is_object())
              for (const auto &field : stored["sharing"].items())
                active[field.key()] = field.value();
            out["active"] = active;
          }
        }
        std::lock_guard<std::mutex> lock(s.sharing.mutex);
        s.sharing.value = out;
        s.sharing.filled = true;
        s.sharing.atMs = steadyMs();
        return out;
      }
#endif

      // ---------- the wifi scan ----------
#ifdef _WIN32
      std::string scanWin(bool force)
      {
        State &s = state();
        {
          std::lock_guard<std::mutex> lock(s.scanMutex);
          if (s.scanEverRun && !force && steadyMs() - s.scanAtMs < 25000)
            return s.scanJsonText;
        }
        // Ask the radio itself first: without a real scan netsh only answers
        // from whatever the last scan happened to leave in the service's mind.
        bool asked = false;
        HMODULE wlanapi = LoadLibraryA("wlanapi.dll");
        if (wlanapi)
        {
          typedef DWORD(WINAPI *OpenHandleFn)(DWORD, PVOID, PDWORD, PHANDLE);
          typedef DWORD(WINAPI *CloseHandleFn)(HANDLE, PVOID);
          typedef DWORD(WINAPI *EnumInterfacesFn)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST *);
          typedef DWORD(WINAPI *ScanFn)(HANDLE, const GUID *, const PDOT11_SSID, ULONG, PVOID);
          typedef void(WINAPI *FreeMemoryFn)(PVOID);
          const OpenHandleFn openHandle =
              reinterpret_cast<OpenHandleFn>(GetProcAddress(wlanapi, "WlanOpenHandle"));
          const CloseHandleFn closeHandle =
              reinterpret_cast<CloseHandleFn>(GetProcAddress(wlanapi, "WlanCloseHandle"));
          const EnumInterfacesFn enumInterfaces =
              reinterpret_cast<EnumInterfacesFn>(GetProcAddress(wlanapi, "WlanEnumInterfaces"));
          const ScanFn scan = reinterpret_cast<ScanFn>(GetProcAddress(wlanapi, "WlanScan"));
          const FreeMemoryFn freeMemory =
              reinterpret_cast<FreeMemoryFn>(GetProcAddress(wlanapi, "WlanFreeMemory"));
          if (openHandle && closeHandle && enumInterfaces && scan && freeMemory)
          {
            DWORD negotiated = 0;
            HANDLE client = nullptr;
            if (openHandle(2, nullptr, &negotiated, &client) == NO_ERROR)
            {
              PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
              if (enumInterfaces(client, nullptr, &interfaces) == NO_ERROR)
              {
                if (interfaces->dwNumberOfItems > 0)
                {
                  const GUID guid = interfaces->InterfaceInfo[0].InterfaceGuid;
                  if (scan(client, &guid, nullptr, 0, nullptr) == NO_ERROR)
                    asked = true;
                }
                freeMemory(interfaces);
              }
              closeHandle(client, nullptr);
            }
          }
          FreeLibrary(wlanapi);
        }
        if (asked)
          Sleep(1400); // the radio wants its moment to hear its neighbours
        json out;
        json networks = json::array();
        std::string note;
        const Run shown = runShell("netsh wlan show networks mode=bssid");
        if (shown.code != 0)
        {
          note = "netsh wlan would not answer about the networks around: " + trim(shown.out);
        }
        else
        {
          std::string ssid, auth, encryption, networkType, bssid;
          bool hidden = false;
          for (const std::string &line : splitLines(shown.out))
          {
            if (line.rfind("SSID ", 0) == 0)
            {
              const size_t colon = line.find(':');
              const std::string raw =
                  colon == std::string::npos ? std::string() : trim(line.substr(colon + 1));
              bssid.clear();
              auth.clear();
              encryption.clear();
              networkType.clear();
              hidden = raw.empty() || containsFold(raw, "hidden");
              ssid = hidden ? "" : raw;
              continue;
            }
            if (line.rfind("Network type", 0) == 0)
            {
              networkType = fieldAfter(line, "Network type");
              continue;
            }
            if (line.rfind("Authentication", 0) == 0)
            {
              auth = fieldAfter(line, "Authentication");
              continue;
            }
            if (line.rfind("Encryption", 0) == 0)
            {
              encryption = fieldAfter(line, "Encryption");
              continue;
            }
            if (line.rfind("BSSID ", 0) == 0)
            {
              bssid = fieldAfter(line, "BSSID");
              continue;
            }
            // A Signal line stands under one BSSID: it closes that row. The
            // lines that follow (radio, band, channel) belong to the row just
            // closed, and are patched onto it.
            if (line.rfind("Signal", 0) == 0)
            {
              if (bssid.empty())
                continue;
              json entry;
              entry["ssid"] = ssid;
              entry["hidden"] = hidden;
              entry["bssid"] = bssid;
              entry["signal"] = toNumber(fieldAfter(line, "Signal"), -1);
              entry["radio"] = "";
              entry["band"] = "";
              entry["channel"] = -1;
              entry["auth"] = auth;
              entry["encryption"] = encryption;
              entry["network_type"] = networkType;
              entry["max_rate"] = -1;
              networks.push_back(entry);
              bssid.clear();
              continue;
            }
            if (networks.empty())
              continue;
            json &last = networks.back();
            // These lines stand under the BSSID whose Signal line just closed
            // a row. netsh can slip other "Channel..." and "Band..." lines
            // into a Bss Load section, so a line that only shares a key's
            // first word is left unread rather than read as a -1.
            if (line.rfind("Radio type", 0) == 0)
            {
              const std::string radio = fieldExact(line, "Radio type");
              if (!radio.empty())
                last["radio"] = radio;
            }
            else if (line.rfind("Band", 0) == 0)
            {
              const std::string band = fieldExact(line, "Band");
              if (!band.empty())
                last["band"] = band;
            }
            else if (line.rfind("Channel", 0) == 0)
            {
              const std::string channel = fieldExact(line, "Channel");
              if (!channel.empty())
                last["channel"] = toNumber(channel, -1);
            }
            else if (line.rfind("Basic rates", 0) == 0 || line.rfind("Other rates", 0) == 0)
            {
              // The rates netsh prints beside a BSS are the ladder of frames
              // the radio states it can carry; the top rung is the fastest
              // rate the machine is willing to name for this network.
              const size_t colon = line.find(':');
              std::istringstream in(line.substr(colon + 1));
              long long best = last.value("max_rate", -1ll);
              double rate = 0;
              while (in >> rate)
                if (rate > static_cast<double>(best))
                  best = static_cast<long long>(rate);
              last["max_rate"] = best;
            }
          }
          note = asked ? "fresh from the radio"
                       : "from the machine's own list (the radio would not take a new scan)";
        }
        out["ok"] = true;
        out["at"] = nowSeconds();
        out["networks"] = networks;
        out["note"] = note;
        const std::string text = out.dump();
        std::lock_guard<std::mutex> lock(s.scanMutex);
        s.scanJsonText = text;
        s.scanAtMs = steadyMs();
        s.scanEverRun = true;
        return text;
      }
#else
      std::string scanLinux(bool force)
      {
        State &s = state();
        {
          std::lock_guard<std::mutex> lock(s.scanMutex);
          if (s.scanEverRun && !force && steadyMs() - s.scanAtMs < 25000)
            return s.scanJsonText;
        }
        json out;
        json networks = json::array();
        std::string note;
        const std::string iface = firstWifiNameLinux();
        if (iface.empty())
          note = "this machine has no wifi interface to scan with";
        else if (!commandExists("iw"))
          note = "this machine has no iw to ask the radio";
        else
        {
          const Run scan =
              runShell("iw dev " + iface + " scan" + (force ? " flush" : "") + " 2>&1");
          if (scan.code != 0)
            note = "the radio would not scan: " + trim(scan.out);
          else
          {
            // iw prints one block per network: a BSS line, then freq, signal,
            // SSID and the security elements indented beneath them.
            json entry;
            bool have = false;
            std::string akms;
            std::string groupCipher;
            bool sawWpa = false;
            std::string topRadio;
            auto finish = [&]()
            {
              if (!have)
                return;
              long long freq = entry.value("freq", -1ll);
              std::string band;
              entry["channel"] = channelFromFreq(freq, band);
              entry["band"] = band;
              entry["radio"] = topRadio;
              entry["max_rate"] = -1;
              if (!akms.empty())
              {
                const bool sae = containsFold(akms, "SAE");
                const bool psk = containsFold(akms, "PSK");
                const bool eap = containsFold(akms, "802.1X");
                if (eap)
                  entry["auth"] = "802.1X";
                else if (sae && psk)
                  entry["auth"] = "WPA2/WPA3-Personal";
                else if (sae)
                  entry["auth"] = "WPA3-Personal";
                else if (psk)
                  entry["auth"] = "WPA2-Personal";
                else
                  entry["auth"] = "RSN";
              }
              else if (sawWpa)
                entry["auth"] = "WPA-Personal";
              else
                entry["auth"] = entry.value("privacy", false) ? "WEP" : "open";
              entry.erase("privacy");
              entry["encryption"] = groupCipher;
              networks.push_back(entry);
              have = false;
            };
            for (const std::string &line : splitLines(scan.out))
            {
              if (line.rfind("BSS ", 0) == 0)
              {
                finish();
                entry = json::object();
                const size_t space = line.find(' ');
                const size_t bracket = line.find('(', space);
                entry["bssid"] =
                    trim(line.substr(space + 1, bracket == std::string::npos
                                                   ? std::string::npos
                                                   : bracket - space - 1));
                entry["signal"] = -1;
                entry["freq"] = -1;
                entry["ssid"] = "";
                entry["hidden"] = false;
                akms.clear();
                groupCipher.clear();
                sawWpa = false;
                topRadio.clear();
                have = true;
                continue;
              }
              if (!have)
                continue;
              std::string value;
              if (!(value = fieldAfter(line, "freq")).empty())
                entry["freq"] = toNumber(value, -1);
              else if (!(value = fieldAfter(line, "signal")).empty())
              {
                const long long rssi = static_cast<long long>(std::strtod(value.c_str(), nullptr));
                long long signal = (rssi + 100) * 2;
                if (signal < 4)
                  signal = 4;
                if (signal > 100)
                  signal = 100;
                entry["signal"] = signal;
                entry["rssi"] = rssi;
              }
              else if (line.rfind("SSID", 0) == 0)
              {
                const size_t colon = line.find(':');
                const std::string name =
                    colon == std::string::npos ? std::string() : trim(line.substr(colon + 1));
                if (name.empty())
                  entry["hidden"] = true;
                entry["ssid"] = name;
              }
              else if (!(value = fieldAfter(line, "capability")).empty())
                entry["privacy"] = containsFold(value, "Privacy");
              else if (line.rfind("* Authentication suites", 0) == 0)
              {
                if (!akms.empty())
                  akms += " ";
                akms += fieldAfter(line, "Authentication suites");
              }
              else if (line.rfind("* Group cipher", 0) == 0 && groupCipher.empty())
                groupCipher = fieldAfter(line, "Group cipher");
              else if (line == "WPA:")
                sawWpa = true;
              else if (line.rfind("EHT capabilities", 0) == 0)
                topRadio = "802.11be";
              else if (line.rfind("HE ", 0) == 0 && topRadio.empty())
                topRadio = "802.11ax";
              else if (line.rfind("VHT capabilities", 0) == 0 && topRadio.empty())
                topRadio = "802.11ac";
              else if (line.rfind("HT capabilities", 0) == 0 && topRadio.empty())
                topRadio = "802.11n";
            }
            finish();
            note = "fresh from the radio on " + iface;
          }
        }
        out["ok"] = true;
        out["at"] = nowSeconds();
        out["networks"] = networks;
        out["note"] = note;
        const std::string text = out.dump();
        std::lock_guard<std::mutex> lock(s.scanMutex);
        s.scanJsonText = text;
        s.scanAtMs = steadyMs();
        s.scanEverRun = true;
        return text;
      }
#endif

      // ---------- what this machine can do from here ----------
      json featuresJson()
      {
        State &s = state();
        {
          std::lock_guard<std::mutex> lock(s.features.mutex);
          if (s.features.fresh(60000))
            return s.features.value;
        }
        json out;
#ifdef _WIN32
        out["platform"] = "windows";
        const bool wifi = runShell("netsh wlan show interfaces").code == 0;
        out["wifi"] = wifi;
        out["scan"] = wifi;
        out["bonding"] = false;
        out["bonding_reason"] =
            "Windows keeps adapter teams in its own hands (the teaming manager of the switch or "
            "the driver); this server will not fake a bond over them";
        out["vlan"] = false;
        out["vlan_reason"] =
            "Windows has no netplan: a vlan there belongs to Hyper-V's virtual switch manager, "
            "not to this server";
        out["netplan"] = false;
        out["netplan_reason"] = "this machine is Windows: netsh carries the settings, not netplan";
        out["nm"] = false;
        out["rfkill"] = false;
        out["rfkill_reason"] =
            "Windows has no rfkill switch: enabling or disabling the adapter itself is the "
            "nearest the machine comes";
        out["hostapd"] = false;
        out["hostapd_reason"] =
            "Linux stands access points with hostapd; Windows runs its mobile hotspot through "
            "the tethering manager, which this server drives";
        const json ap = accessPointWin();
        out["ap"] = ap.value("supported", false);
        out["ap_reason"] = ap.value("reason", std::string(""));
        out["sharing"] = true;
        out["sharing_reason"] = "";
#else
        out["platform"] = "linux";
        const bool iw = commandExists("iw");
        const bool nm = commandExists("nmcli");
        out["wifi"] = iw || nm;
        out["scan"] = iw;
        std::error_code ec{};
        out["bonding"] = std::filesystem::exists("/sys/class/net/bonding_masters", ec);
        out["bonding_reason"] =
            out["bonding"].get<bool>()
                ? std::string()
                : "this kernel has no bonding module; a netplan bond would not stand on it";
        out["vlan"] = commandExists("ip");
        out["vlan_reason"] =
            out["vlan"].get<bool>() ? std::string()
                                    : "this machine has no ip command to shape vlans with";
        const bool netplan = commandExists("netplan");
        out["netplan"] = netplan;
        out["netplan_reason"] =
            netplan
                ? std::string()
                : "this machine has no netplan: this server will not write settings it cannot "
                  "apply, and changes are refused rather than half-done";
        out["nm"] = nm;
        out["rfkill"] = commandExists("rfkill");
        out["rfkill_reason"] =
            out["rfkill"].get<bool>()
                ? std::string()
                : "this machine has no rfkill to switch its radio with";
        const bool hostapd = commandExists("hostapd") && commandExists("hostapd_cli");
        out["hostapd"] = hostapd;
        out["hostapd_reason"] =
            hostapd ? std::string()
                    : "this machine has no hostapd: install it to stand an access point here";
        out["ap"] = hostapd && iw;
        out["ap_reason"] =
            out["ap"].get<bool>() ? std::string() : "an access point here needs hostapd and iw";
        out["sharing"] = commandExists("iptables");
        out["sharing_reason"] =
            out["sharing"].get<bool>() ? std::string()
                                       : "this machine has no iptables to stand sharing on";
#endif
        std::lock_guard<std::mutex> lock(s.features.mutex);
        s.features.value = out;
        s.features.filled = true;
        s.features.atMs = steadyMs();
        return out;
      }

      // ---------- netplan: one file, whole, and checked before applied ----------
#ifndef _WIN32
      const char *kNetplanPath = "/etc/netplan/70-open-local-services.yaml";

      std::string yamlQuote(const std::string &value)
      {
        std::string out = "\"";
        for (const char c : value)
        {
          if (c == '"' || c == '\\')
            out += '\\';
          out += c;
        }
        out += "\"";
        return out;
      }

      // The managed picture: this server's own notebook of every device it set,
      // under the state file. Read once; every device change lands here and
      // then into the rendered file.
      json &netplanStateRef()
      {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.netplanMutex);
        if (!s.netplanLoaded)
        {
          s.netplanLoaded = true;
          const json stored = loadStateFile();
          if (stored.is_object() && stored.contains("netplan") && stored["netplan"].is_object())
            s.netplanState = stored["netplan"];
          else
            s.netplanState = json::object();
          for (const char *key : {"ethernets", "bonds", "vlans"})
            if (!s.netplanState.contains(key) || !s.netplanState[key].is_object())
              s.netplanState[key] = json::object();
        }
        return s.netplanState;
      }

      bool storeNetplanState()
      {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.netplanMutex);
        json file = loadStateFile();
        file["netplan"] = s.netplanState;
        return saveStateFile(file);
      }

      void renderNetplanBlock(const json &spec, std::ostringstream &out, const std::string &pad)
      {
        // A member of a bond stands plain: no address of its own.
        if (spec.contains("member_of"))
        {
          out << pad << "dhcp4: false\n" << pad << "dhcp6: false\n";
          return;
        }
        const bool dhcp = spec.value("mode", std::string("dhcp")) == "dhcp";
        out << pad << "dhcp4: " << (dhcp ? "true" : "false") << "\n";
        out << pad << "dhcp6: " << (dhcp ? "true" : "false") << "\n";
        if (!dhcp)
        {
          const std::string address = spec.value("address", std::string());
          const long long prefix = spec.value("prefix", 24ll);
          if (!address.empty())
            out << pad << "addresses: [" << yamlQuote(address + "/" + std::to_string(prefix))
                << "]\n";
          const std::string gateway = spec.value("gateway", std::string());
          const long long metric = spec.value("metric", -1ll);
          if (!gateway.empty())
          {
            out << pad << "routes:\n" << pad << "  - to: default\n" << pad << "    via: "
                << yamlQuote(gateway);
            if (metric >= 0)
              out << "\n" << pad << "    metric: " << metric;
            out << "\n";
          }
        }
        const json dns =
            spec.contains("dns") && spec["dns"].is_array() ? spec["dns"] : json::array();
        if (!dns.empty())
        {
          out << pad << "nameservers:\n" << pad << "  addresses: [";
          for (size_t i = 0; i < dns.size(); ++i)
            out << (i ? ", " : "") << yamlQuote(dns[i].get<std::string>());
          out << "]\n";
        }
        const long long mtu = spec.value("mtu", -1ll);
        if (mtu >= 1280)
          out << pad << "mtu: " << mtu << "\n";
      }

      std::string renderNetplan()
      {
        std::ostringstream out;
        out << "# Managed by open-local-services. The network panel of this machine\n"
            << "# writes this file whole and runs `netplan apply` after it; edits\n"
            << "# made here by hand are overwritten the next time the panel changes\n"
            << "# a setting.\n";
        // Follow the renderer this machine's own netplan files chose, so the
        // settings land with the manager the machine actually runs.
        std::string renderer = "networkd";
        DIR *dir = opendir("/etc/netplan");
        if (dir)
        {
          while (dirent *entry = readdir(dir))
          {
            const std::string name = entry->d_name;
            if (name == "70-open-local-services.yaml" || name.size() < 5 ||
                name.compare(name.size() - 5, 5, ".yaml") != 0)
              continue;
            std::string text;
            if (ols::readFileText("/etc/netplan/" + name, text) &&
                text.find("renderer: NetworkManager") != std::string::npos)
            {
              renderer = "NetworkManager";
              break;
            }
          }
          closedir(dir);
        }
        out << "network:\n  version: 2\n  renderer: " << renderer << "\n";
        // A local copy: bond members that this server never set on their own
        // still have to stand plain in the file.
        json effective = netplanStateRef();
        for (const auto &bond : effective["bonds"].items())
        {
          const json slaves = bond.value().value("slaves", json::array());
          for (const json &slave : slaves)
            if (slave.is_string() &&
                !effective["ethernets"].contains(slave.get<std::string>()))
            {
              json placeholder = json::object();
              placeholder["member_of"] = bond.key();
              effective["ethernets"][slave.get<std::string>()] = placeholder;
            }
        }
        bool any = false;
        for (const char *group : {"ethernets", "bonds", "vlans"})
        {
          const json &entries = effective[group];
          if (entries.empty())
            continue;
          any = true;
          out << "  " << group << ":\n";
          for (const auto &item : entries.items())
          {
            out << "    " << yamlQuote(item.key()) << ":\n";
            if (std::string(group) == "vlans")
            {
              out << "      id: " << item.value().value("id", 0ll) << "\n"
                  << "      link: " << yamlQuote(item.value().value("parent", std::string()))
                  << "\n";
              renderNetplanBlock(item.value(), out, "      ");
            }
            else if (std::string(group) == "bonds")
            {
              out << "      interfaces: [";
              const json slaves = item.value().value("slaves", json::array());
              for (size_t i = 0; i < slaves.size(); ++i)
                out << (i ? ", " : "") << yamlQuote(slaves[i].get<std::string>());
              out << "]\n";
              out << "      parameters:\n        mode: "
                  << yamlQuote(item.value().value("mode", std::string("802.3ad"))) << "\n";
              const std::string primary = item.value().value("primary", std::string());
              if (!primary.empty())
                out << "        primary: " << yamlQuote(primary) << "\n";
              out << "        mii-monitor-interval: 100\n";
              if (item.value().value("mode", std::string("")) == "802.3ad")
                out << "        lacp-rate: 1\n";
              renderNetplanBlock(item.value(), out, "      ");
            }
            else
              renderNetplanBlock(item.value(), out, "      ");
          }
        }
        if (!any)
          out << "  ethernets: {}\n"; // netplan wants a document, not an empty one
        return out.str();
      }

      // Write, generate, apply - in that order. `netplan generate` runs first
      // and the kept file is put back the way it was if generation refuses what
      // was written: the machine is never left holding an unreadable netplan
      // file, and `netplan apply` only ever sees a document generate accepted.
      Run netplanApply(std::string &words)
      {
        Run result;
        const std::string yaml = renderNetplan();
        std::string backup;
        const bool hadOld = ols::readFileText(kNetplanPath, backup);
        std::error_code ec{};
        std::filesystem::create_directories("/etc/netplan", ec);
        if (!ols::writeFileText(kNetplanPath, yaml))
        {
          words = "the machine would not let this server write " + std::string(kNetplanPath) +
                  " (this needs the machine's manager: root)";
          return result;
        }
        const Run generate = runShell("netplan generate");
        if (generate.code != 0)
        {
          if (hadOld)
            ols::writeFileText(kNetplanPath, backup);
          else
            std::filesystem::remove(kNetplanPath, ec);
          words = "netplan refused the new settings and the previous file was kept: " +
                  trim(generate.out);
          return result;
        }
        const Run apply = runShell("netplan apply");
        result.ran = true;
        result.code = apply.code;
        result.out = apply.out;
        words = apply.code == 0 ? "netplan apply: the settings are in force"
                                : "netplan applied, but said: " + trim(apply.out);
        return result;
      }

      // One shape for how a netplan change answers the page.
      json netplanAnswer(const Run &applied, const std::string &words)
      {
        json out;
        out["did"] = applied.ran && applied.code == 0;
        out["machine_words"] = words;
        out["words"] = words.empty() ? "netplan did not answer" : words;
        return out;
      }
#endif // !_WIN32

      // ---------- the snapshot ----------
      void mergeHistory(json &adapter, const std::string &name)
      {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.watchMutex);
        const auto found = s.watches.find(name);
        if (found == s.watches.end())
          return;
        const AdapterWatch &watch = found->second;
        json t = json::array(), rx = json::array(), tx = json::array(), rxp = json::array(),
             txp = json::array();
        for (const RatePoint &point : watch.history)
        {
          t.push_back(point.atS);
          rx.push_back(static_cast<long long>(point.rxRate));
          tx.push_back(static_cast<long long>(point.txRate));
          rxp.push_back(static_cast<long long>(point.rxPps));
          txp.push_back(static_cast<long long>(point.txPps));
        }
        json history;
        history["t"] = t;
        history["rx"] = rx;
        history["tx"] = tx;
        history["rxp"] = rxp;
        history["txp"] = txp;
        adapter["history"] = history;
        // The newest reading and a ten-second mean stand beside the history:
        // the page draws the moment, and a person reads whether it is loud.
        if (!watch.history.empty())
        {
          const RatePoint &last = watch.history.back();
          double avgRx = 0, avgTx = 0;
          size_t averaged = 0;
          for (auto it = watch.history.rbegin();
               it != watch.history.rend() && averaged < 5; ++it, ++averaged)
          {
            avgRx += it->rxRate;
            avgTx += it->txRate;
          }
          json rate;
          rate["rx"] = static_cast<long long>(last.rxRate);
          rate["tx"] = static_cast<long long>(last.txRate);
          rate["rx_pps"] = static_cast<long long>(last.rxPps);
          rate["tx_pps"] = static_cast<long long>(last.txPps);
          rate["avg_rx"] = static_cast<long long>(avgRx / static_cast<double>(averaged));
          rate["avg_tx"] = static_cast<long long>(avgTx / static_cast<double>(averaged));
          adapter["rate"] = rate;
        }
        if (watch.up && watch.upSinceEpoch > 0)
        {
          json uptime;
          uptime["since"] = watch.upSinceEpoch;
          uptime["held_s"] = std::max(0ll, nowSeconds() - watch.upSinceEpoch);
          uptime["measured"] = watch.upMeasured;
          adapter["up_time"] = uptime;
        }
        else
          adapter["up_time"] = nullptr;
        json stats;
        stats["rx_bytes"] = watch.last.rxBytes;
        stats["tx_bytes"] = watch.last.txBytes;
        stats["rx_packets"] = watch.last.rxPkts;
        stats["tx_packets"] = watch.last.txPkts;
        stats["rx_errors"] = watch.last.rxErr;
        stats["tx_errors"] = watch.last.txErr;
        stats["rx_dropped"] = watch.last.rxDrop;
        stats["tx_dropped"] = watch.last.txDrop;
        adapter["stats"] = stats;
      }

      std::string snapshotBody()
      {
        json out;
        out["ok"] = true;
#ifdef _WIN32
        out["platform"] = "windows";
        out["os"] = windowsOsName();
        out["hostname"] = hostNameWin();
        out["uptime_s"] = static_cast<long long>(GetTickCount64() / 1000);
        out["elevated"] = elevatedWin();
#else
        out["platform"] = "linux";
        out["os"] = linuxPrettyName();
        out["hostname"] = hostNameLinux();
        {
          const std::string uptime = readTinyFile("/proc/uptime");
          out["uptime_s"] = std::max(0ll, static_cast<long long>(
                                              std::strtod(uptime.c_str(), nullptr)));
        }
        out["kernel"] = readTinyFile("/proc/sys/kernel/osrelease");
        out["elevated"] = geteuid() == 0;
#endif
        out["at"] = nowSeconds();
        out["features"] = featuresJson();

        json adapters = json::array();
#ifdef _WIN32
        collectAdaptersWin(adapters);
#else
        collectAdaptersLinux(adapters);
#endif
        for (json &adapter : adapters)
          mergeHistory(adapter, adapter.value("name", std::string()));
#ifndef _WIN32
        // What this server's netplan notebook says about a device it set:
        // whether dhcp owns it or an address was named, and which metric went
        // with it.
        json &managed = netplanStateRef();
        for (json &adapter : adapters)
        {
          const std::string name = adapter.value("name", std::string());
          for (const char *group : {"ethernets", "bonds", "vlans"})
          {
            if (!managed[group].contains(name))
              continue;
            const json spec = managed[group][name];
            if (spec.contains("member_of"))
              break;
            if (spec.value("mode", std::string("dhcp")) == "dhcp")
              adapter["dhcp"] = true;
            else if (!spec.value("address", std::string()).empty())
              adapter["dhcp"] = false;
            adapter["metric"] = spec.value("metric", adapter.value("metric", -1ll));
            adapter["managed_by_this_server"] = true;
            break;
          }
        }
#endif
        out["adapters"] = adapters;

        json wifi;
#ifdef _WIN32
        wifi["interfaces"] = wifiStatusWin();
#else
        wifi["interfaces"] = wifiStatusLinux();
#endif
        json blocked = json::array();
#ifndef _WIN32
        if (featuresJson().value("rfkill", false))
        {
          const Run rfkill = runShell("rfkill list 2>/dev/null");
          bool deviceIsWifi = true;
          for (const std::string &line : splitLines(rfkill.out))
          {
            if (containsFold(line, "Wifi"))
              deviceIsWifi = true;
            else if (containsFold(line, "Bluetooth"))
              deviceIsWifi = false;
            else if ((containsFold(line, "hard blocked: yes") ||
                      containsFold(line, "soft blocked: yes")) &&
                     deviceIsWifi && blocked.empty())
              blocked.push_back("wifi");
          }
        }
#endif
        // Windows states no separate rfkill: an adapter its admin disabled is
        // the nearest it gives, and the adapter's own admin flag carries that.
        wifi["blocked"] = blocked;
        out["wifi"] = wifi;

#ifdef _WIN32
        {
          State &s = state();
          std::lock_guard<std::mutex> lock(s.apState.mutex);
          if (!s.apState.fresh(15000))
          {
            s.apState.value = accessPointWin();
            s.apState.filled = true;
            s.apState.atMs = steadyMs();
          }
          out["ap"] = s.apState.value;
        }
#else
        out["ap"] = apStateLinux();
#endif
#ifdef _WIN32
        {
          State &s = state();
          std::lock_guard<std::mutex> lock(s.sharing.mutex);
          if (!s.sharing.fresh(15000))
          {
            s.sharing.value = sharingWin();
            s.sharing.filled = true;
            s.sharing.atMs = steadyMs();
          }
          out["sharing"] = s.sharing.value;
        }
#else
        out["sharing"] = sharingLinux();
#endif

        // The last kept scan if there is one: the page draws it at once, and
        // asks its own fresh scan only when a person presses the hand.
        {
          State &s = state();
          std::lock_guard<std::mutex> lock(s.scanMutex);
          if (s.scanEverRun)
          {
            json cached = json::parse(s.scanJsonText, nullptr, false);
            if (!cached.is_discarded())
            {
              cached["age_s"] = static_cast<long long>((steadyMs() - s.scanAtMs) / 1000);
              out["scan"] = cached;
            }
          }
        }

        State &s = state();
        std::lock_guard<std::mutex> lock(s.actionMutex);
        if (!s.lastAction.is_null())
          out["actions_last"] = s.lastAction;
        return out.dump();
      }

      std::string scanBody(bool force)
      {
#ifdef _WIN32
        return scanWin(force);
#else
        return scanLinux(force);
#endif
      }

      // ---------- the work door ----------
      json refused(const std::string &words)
      {
        json out;
        out["did"] = false;
        out["words"] = words;
        return out;
      }

      json didWords(const std::string &words)
      {
        json out;
        out["did"] = true;
        out["words"] = words;
        return out;
      }

      // The machine's own tools answer for their own refusals: their words come
      // back whole, trimmed to something a page can show.
      json answerOf(const Run &run, const std::string &doneWords, const std::string &failedLead)
      {
        json out;
        std::string words = trim(run.out);
        if (words.size() > 900)
          words = words.substr(0, 900) + "...";
        out["did"] = run.ran && run.code == 0;
        out["machine_words"] = words;
        out["words"] = out["did"].get<bool>()
                           ? doneWords + (words.empty() ? "" : " - " + words)
                           : failedLead + (words.empty() ? " (the machine said nothing)"
                                                         : ": " + words);
        return out;
      }

      void rememberAction(const std::string &action, const json &result)
      {
        State &s = state();
        json entry;
        entry["action"] = action;
        entry["did"] = result.value("did", false);
        entry["words"] = result.value("words", std::string(""));
        entry["at"] = nowSeconds();
        {
          std::lock_guard<std::mutex> lock(s.actionMutex);
          s.lastAction = entry;
        }
        json file;
        file["actions_last"] = entry;
        saveStateFile(file);
      }

#ifdef _WIN32
      std::string shellQuoted(const std::string &text)
      {
        return "\"" + text + "\"";
      }

      Run netsh(const std::string &arguments)
      {
        return runShell("netsh " + arguments);
      }

      Run netshInterfaceSetCall(const std::string &name, bool up)
      {
        return netsh("interface set interface name=" + shellQuoted(name) +
                     (up ? " admin=enabled" : " admin=disabled"));
      }

      bool wantElevation(json &answer)
      {
        if (elevatedWin())
          return true;
        answer = refused("refused: changing this belongs to the machine's manager, and Windows "
                         "runs this server as a plain person");
        return false;
      }
#else
      bool wantElevation(json &answer)
      {
        if (geteuid() == 0)
          return true;
        answer = refused("refused: changing this belongs to the machine's manager, and this "
                         "server runs without root (run it through sudo to change settings)");
        return false;
      }
#endif

      // True when the named address is a v6 one written with colons.
      bool isV6Address(const std::string &address)
      {
        return address.find(':') != std::string::npos;
      }

      std::string v4Mask(long long prefix)
      {
        unsigned long mask = prefix <= 0 ? 0UL : (prefix >= 32 ? 0xffffffffUL
                                                               : (0xffffffffUL << (32 - prefix)));
        return std::to_string((mask >> 24) & 0xff) + "." + std::to_string((mask >> 16) & 0xff) +
               "." + std::to_string((mask >> 8) & 0xff) + "." + std::to_string(mask & 0xff);
      }

      json actionAdapter(const std::string &action, const json &body)
      {
        const std::string name = body.value("name", std::string());
        if (!safeShellPart(name, true) && !safeIfname(name))
          return refused("refused: the name of an adapter is not something this server will "
                         "carry through a shell");
        const bool up = action == "adapter_up";
        json answer;
        if (!wantElevation(answer))
          return answer;
#ifdef _WIN32
        const Run run = netshInterfaceSetCall(name, up);
        return answerOf(run, up ? "the adapter is enabled again" : "the adapter is disabled",
                        "the machine refused");
#else
        const Run run = runShell("ip link set " + name + (up ? " up" : " down"));
        return answerOf(run, up ? "the link is up" : "the link is down", "the machine refused");
#endif
      }

      json actionMtu(const json &body)
      {
        const std::string name = body.value("name", std::string());
        const long long mtu = body.value("mtu", 0ll);
        if ((!safeShellPart(name, true) && !safeIfname(name)) || mtu < 1280 || mtu > 65535)
          return refused("refused: an adapter name this server can carry, and an mtu between "
                         "1280 and 65535 - that is what an mtu may be");
        json answer;
        if (!wantElevation(answer))
          return answer;
#ifdef _WIN32
        const Run run = netsh("interface set subinterface " + shellQuoted(name) + " mtu=" +
                              std::to_string(mtu) + " store=persistent");
        return answerOf(run, "the mtu is set to " + std::to_string(mtu), "the machine refused");
#else
        runShell("ip link set " + name + " mtu " + std::to_string(mtu));
        json &managed = netplanStateRef();
        const bool known = managed["ethernets"].contains(name) &&
                           managed["ethernets"][name].is_object();
        json spec = known ? managed["ethernets"][name] : json::object();
        if (!known)
          spec["mode"] = "dhcp";
        spec["mtu"] = mtu;
        managed["ethernets"][name] = spec;
        storeNetplanState();
        std::string words;
        const Run applied = netplanApply(words);
        json out = netplanAnswer(applied, words);
        out["words"] = "the link carries mtu " + std::to_string(mtu) + " now" +
                       (words.empty() ? "" : " - netplan: " + words);
        return out;
#endif
      }

      json actionIp(const json &body)
      {
        const std::string name = body.value("name", std::string());
        const std::string mode = lower(body.value("mode", std::string("dhcp")));
        const std::string address = body.value("address", std::string());
        const long long prefix = body.value("prefix", 24ll);
        const std::string gateway = body.value("gateway", std::string());
        const long long metric = body.value("metric", -1ll);
        json dns = json::array();
        if (body.contains("dns") && body["dns"].is_array())
          for (const json &entry : body["dns"])
            if (entry.is_string() && safeAddressish(entry.get<std::string>()))
              dns.push_back(entry.get<std::string>());
        if ((!safeShellPart(name, true) && !safeIfname(name)) || (mode != "dhcp" && mode != "static"))
          return refused("refused: mode is dhcp or static, and the adapter's name must be one "
                         "this server can carry through a shell");
        if (mode == "static")
        {
          if (address.empty() || !safeAddressish(address))
            return refused("refused: a static setting needs an address it states, as plain words");
          if (isV6Address(address) ? (prefix < 1 || prefix > 128) : (prefix < 1 || prefix > 32))
            return refused("refused: the prefix named is not a prefix of that kind of address");
          if (!gateway.empty() && !safeAddressish(gateway))
            return refused("refused: the gateway named is not a plain address");
        }
        if (metric < 0 || metric > 65535)
          return refused("refused: a metric is between 0 and 65535, or not named at all");
        json answer;
        if (!wantElevation(answer))
          return answer;
#ifdef _WIN32
        std::vector<std::string> steps;
        if (mode == "dhcp")
        {
          steps.push_back("interface ip set address name=" + shellQuoted(name) + " dhcp");
          steps.push_back("interface ip set dns name=" + shellQuoted(name) + " dhcp");
        }
        else
        {
          if (isV6Address(address))
          {
            std::string step = "interface ip set address name=" + shellQuoted(name) +
                               " source=static address=" + address + "/" + std::to_string(prefix);
            if (!gateway.empty())
              step += " gateway=" + gateway;
            steps.push_back(step);
          }
          else
          {
            std::string step = "interface ip set address name=" + shellQuoted(name) + " static " +
                               address + " " + v4Mask(prefix);
            if (!gateway.empty())
              step += " " + gateway;
            steps.push_back(step);
          }
          if (dns.empty())
            steps.push_back("interface ip set dns name=" + shellQuoted(name) + " dhcp");
          else
          {
            steps.push_back("interface ip set dns name=" + shellQuoted(name) + " static " +
                            dns[0].get<std::string>() + " index=1");
            for (size_t i = 1; i < dns.size() && i < 4; ++i)
              steps.push_back("interface ip add dns name=" + shellQuoted(name) + " addr=" +
                              dns[i].get<std::string>() + " index=" + std::to_string(i + 1));
          }
        }
        if (metric >= 0)
          steps.push_back("interface ip set interface " + shellQuoted(name) +
                          " metric=" + std::to_string(metric));
        for (const std::string &step : steps)
        {
          const Run run = netsh(step);
          if (!run.ran || run.code != 0)
            return answerOf(run, "", "the machine refused (netsh " + step + ")");
        }
        return didWords(mode == "dhcp" ? "dhcp owns the address of " + name + " again"
                                       : name + " stands on " + address + "/" +
                                             std::to_string(prefix));
#else
        json &managed = netplanStateRef();
        const bool known = managed["ethernets"].contains(name) &&
                           managed["ethernets"][name].is_object();
        json spec = known ? managed["ethernets"][name] : json::object();
        spec["mode"] = mode;
        spec["address"] = mode == "static" ? address : "";
        spec["prefix"] = prefix;
        spec["gateway"] = mode == "static" ? gateway : "";
        spec["dns"] = dns;
        if (metric >= 0)
          spec["metric"] = metric;
        managed["ethernets"][name] = spec;
        storeNetplanState();
        std::string words;
        const Run applied = netplanApply(words);
        return netplanAnswer(applied, words);
#endif
      }

      json actionWifiConnect(const json &body)
      {
        std::string iface = body.value("interface", std::string());
        const std::string ssid = body.value("ssid", std::string());
        const std::string psk = body.value("psk", std::string());
        const std::string auth = lower(body.value("auth", std::string("auto")));
        const bool hidden = body.value("hidden", false);
        if (ssid.empty() || ssid.size() > 64)
          return refused("refused: an ssid is up to 64 bytes, and this one was not");
        if (auth == "802.1x" || auth == "enterprise")
          return refused("refused: this server only joins personal networks; an 802.1X network "
                         "wants certificates, which belong in the machine's own wifi tools");
#ifdef _WIN32
        if (!safeShellPart(ssid, true))
          return refused("refused: this ssid carries characters this server will not hand a "
                         "shell; it can still be joined by hand in Windows' own wifi list");
        if (iface.empty())
          iface = firstWifiNameWin();
        if (iface.empty() || !safeShellPart(iface, true))
          return refused("refused: this machine has no wifi interface this server can name");
        if (!psk.empty() && !safeShellPart(psk, false))
          return refused("refused: this key carries characters this server will not hand a shell");
        std::string authentication = "WPA2";
        std::string encryption = "AES";
        std::string sharedKey;
        if (psk.empty() || auth == "open")
        {
          authentication = "open";
          encryption = "none";
        }
        else
        {
          if (auth == "sae" || auth == "wpa3")
            authentication = "WPA3";
          else if (auth == "wpa")
            authentication = "WPA";
          sharedKey = "\n      <sharedKey>\n        <keyType>passPhrase</keyType>\n"
                      "        <protected>false</protected>\n        <keyMaterial>" +
                      xmlEscape(psk) + "</keyMaterial>\n      </sharedKey>\n";
        }
        std::ostringstream xml;
        xml << "<?xml version=\"1.0\"?>\n"
            << "<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">\n"
            << "  <name>" << xmlEscape(ssid) << "</name>\n"
            << "  <SSIDConfig>\n    <SSID>\n      <name>" << xmlEscape(ssid)
            << "</name>\n    </SSID>\n  </SSIDConfig>\n"
            << "  <connectionType>ESS</connectionType>\n"
            << "  <connectionMode>auto</connectionMode>\n"
            << (hidden ? "  <nonBroadcast>true</nonBroadcast>\n" : "")
            << "  <MSM>\n    <security>\n      <authEncryption>\n        <authentication>"
            << authentication << "</authentication>\n        <encryption>" << encryption
            << "</encryption>\n        <useOneX>false</useOneX>\n      </authEncryption>\n"
            << sharedKey << "    </security>\n  </MSM>\n</WLANProfile>\n";
        std::error_code ec{};
        const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
        if (ec)
          return refused("refused: this server could not even find a place to write a file");
        const std::string temp =
            (tempDir / ("ols-wifi-profile-" + std::to_string(nowSeconds()) + ".xml")).string();
        if (!ols::writeFileText(temp, xml.str()))
          return refused("refused: this machine would not write the profile file it needs");
        // The whole netsh ask carried as one built string: clearer to read at
        // a glance than a four-way concatenation, and it dodges a compiler
        // puzzle this file would rather spend its lines on.
        std::string addProfile = "wlan add profile filename=";
        addProfile += shellQuoted(temp);
        addProfile += " interface=";
        addProfile += shellQuoted(iface);
        const Run add = netsh(addProfile);
        std::filesystem::remove(temp, ec);
        if (!add.ran || add.code != 0)
          return answerOf(add, "", "the machine would not take the profile");
        const Run connect = netsh("wlan connect name=" + shellQuoted(ssid) + " ssid=" + shellQuoted(ssid) +
                                  " interface=" + shellQuoted(iface));
        return answerOf(connect, "the radio is joining " + ssid,
                        "the profile is kept, but the machine would not join it");
#else
        if (iface.empty())
          iface = firstWifiNameLinux();
        if (iface.empty() || !safeIfname(iface))
          return refused("refused: this machine has no wifi interface this server can name");
        if (!safeShellPart(ssid, true))
          return refused("refused: this ssid carries characters this server will not hand a shell");
        if (!psk.empty() && !safeShellPart(psk, false))
          return refused("refused: this key carries characters this server will not hand a shell");
        json answer;
        if (!wantElevation(answer))
          return answer;
        if (!networkManagerRunning())
          return refused("refused: this machine runs no NetworkManager to hand the join to; a "
                         "wpa_supplicant-only machine needs its own tools for this one");
        std::string command = "nmcli dev wifi connect \"" + ssid + "\"";
        if (!psk.empty())
          command += " password \"" + psk + "\"";
        command += " ifname " + iface;
        const Run run = runShell(command);
        return answerOf(run, "the radio is joining " + ssid,
                        "NetworkManager would not join it");
#endif
      }

      json actionWifiDisconnect(const json &body)
      {
        std::string iface = body.value("interface", std::string());
#ifdef _WIN32
        if (iface.empty())
          iface = firstWifiNameWin();
        if (iface.empty() || !safeShellPart(iface, true))
          return refused("refused: this machine has no wifi interface to part from");
        const Run run = netsh("wlan disconnect interface=" + shellQuoted(iface));
        return answerOf(run, "the radio left the network", "the machine refused");
#else
        if (iface.empty())
          iface = firstWifiNameLinux();
        if (iface.empty() || !safeIfname(iface))
          return refused("refused: this machine has no wifi interface this server can name");
        json answer;
        if (!wantElevation(answer))
          return answer;
        if (networkManagerRunning())
        {
          const Run run = runShell("nmcli dev disconnect " + iface);
          return answerOf(run, "the radio left the network", "the machine refused");
        }
        const Run run = runShell("iw dev " + iface + " disconnect");
        return answerOf(run, "the radio left the network", "the machine refused");
#endif
      }

      json actionWifiForget(const json &body)
      {
        const std::string ssid = body.value("ssid", std::string());
        const std::string iface = body.value("interface", std::string());
        if (ssid.empty() || !safeShellPart(ssid, true))
          return refused("refused: the ssid whose profile should go must be quotable words");
#ifdef _WIN32
        std::string command = "wlan delete profile name=" + shellQuoted(ssid);
        if (!iface.empty() && safeShellPart(iface, true))
          command += " interface=" + shellQuoted(iface);
        const Run run = netsh(command);
        return answerOf(run, "the profile for " + ssid + " is forgotten", "the machine refused");
#else
        json answer;
        if (!wantElevation(answer))
          return answer;
        if (!networkManagerRunning())
          return refused("refused: this machine runs no NetworkManager that keeps such profiles");
        const Run run = runShell("nmcli connection delete \"" + ssid + "\"");
        return answerOf(run, "the profile for " + ssid + " is forgotten", "the machine refused");
#endif
      }

      json actionRadio(const json &body)
      {
        const bool block = body.value("on", true);
        std::string iface = body.value("interface", std::string());
        json answer;
        if (!wantElevation(answer))
          return answer;
#ifdef _WIN32
        if (iface.empty())
          iface = firstWifiNameWin();
        if (iface.empty() || !safeShellPart(iface, true))
          return refused("refused: this machine has no wifi interface to reach for");
        const Run run = netshInterfaceSetCall(iface, !block);
        return answerOf(run,
                        block ? "the wifi adapter is down - that is the closest this machine's "
                                "Windows comes to a radio switch"
                              : "the wifi adapter is up again",
                        "the machine refused");
#else
        if (!featuresJson().value("rfkill", false))
          return refused("refused: this machine has no rfkill to switch its radio with");
        (void)iface;
        const Run run = runShell(std::string("rfkill ") + (block ? "block wifi" : "unblock wifi"));
        return answerOf(run, block ? "the radio is blocked" : "the radio is unblocked",
                        "the machine refused");
#endif
      }

      json actionBond(const std::string &action, const json &body)
      {
#ifdef _WIN32
        (void)action;
        (void)body;
        const json features = featuresJson();
        return refused("refused: " +
                       features.value("bonding_reason", std::string("this machine does not "
                                                                    "manage bonds from here")));
#else
        json answer;
        if (!wantElevation(answer))
          return answer;
        if (!featuresJson().value("netplan", false))
          return refused("refused: " +
                         featuresJson().value("netplan_reason",
                                              std::string("this machine has no netplan")));
        const std::string name = body.value("name", std::string());
        if (!safeIfname(name))
          return refused("refused: a bond's name must be plain characters this server can carry");
        if (action == "bond_delete")
        {
          json &managed = netplanStateRef();
          if (!managed["bonds"].contains(name))
            return refused("refused: this server never made a bond named " + name +
                           ", and it will not delete one it did not make");
          managed["bonds"].erase(name);
          // Members this server left plain on the way in come off the notebook
          // on the way out too.
          for (auto it = managed["ethernets"].begin(); it != managed["ethernets"].end();)
          {
            if (it.value().is_object() && it.value().contains("member_of") &&
                it.value().value("member_of", std::string()) == name)
              it = managed["ethernets"].erase(it);
            else
              ++it;
          }
          storeNetplanState();
          std::string words;
          const Run applied = netplanApply(words);
          return netplanAnswer(applied, words);
        }
        if (action != "bond_create")
          return refused("refused: no bond action by that name");
        json slaves = json::array();
        if (body.contains("slaves") && body["slaves"].is_array())
          for (const json &entry : body["slaves"])
            if (entry.is_string() && safeIfname(entry.get<std::string>()))
              slaves.push_back(entry.get<std::string>());
        if (slaves.size() < 2)
          return refused("refused: a bond is made of two or more adapters, named as slaves");
        const std::string mode = lower(body.value("mode", std::string("802.3ad")));
        const char *const bondModes[] = {"balance-rr", "active-backup", "balance-xor",
                                         "broadcast", "802.3ad", "balance-tlb", "balance-alb"};
        bool modeOk = false;
        for (const char *candidate : bondModes)
          if (mode == candidate)
            modeOk = true;
        if (!modeOk)
          return refused("refused: the bond mode named is not one of balance-rr, active-backup, "
                         "balance-xor, broadcast, 802.3ad, balance-tlb, balance-alb");
        const std::string primary = body.value("primary", std::string());
        const std::string address = body.value("address", std::string());
        const long long prefix = body.value("prefix", 24ll);
        if (!address.empty() && !safeAddressish(address))
          return refused("refused: the address named for the bond is not a plain address");
        json spec;
        spec["slaves"] = slaves;
        spec["bond_mode"] = mode;
        spec["primary"] = safeIfname(primary) ? primary : "";
        spec["mode"] = address.empty() ? "dhcp" : "static";
        spec["address"] = address;
        spec["prefix"] = prefix;
        spec["gateway"] = body.value("gateway", std::string());
        json dns = json::array();
        if (body.contains("dns") && body["dns"].is_array())
          for (const json &entry : body["dns"])
            if (entry.is_string() && safeAddressish(entry.get<std::string>()))
              dns.push_back(entry.get<std::string>());
        spec["dns"] = dns;
        spec["metric"] = body.value("metric", -1ll);
        json &managed = netplanStateRef();
        managed["bonds"][name] = spec;
        for (const json &slave : slaves)
        {
          const std::string slaveName = slave.get<std::string>();
          if (!managed["ethernets"].contains(slaveName) ||
              !managed["ethernets"][slaveName].is_object())
          {
            json placeholder = json::object();
            placeholder["member_of"] = name;
            managed["ethernets"][slaveName] = placeholder;
          }
        }
        storeNetplanState();
        std::string words;
        const Run applied = netplanApply(words);
        return netplanAnswer(applied, words);
#endif
      }

      json actionVlan(const std::string &action, const json &body)
      {
#ifdef _WIN32
        (void)action;
        (void)body;
        const json features = featuresJson();
        return refused("refused: " +
                       features.value("vlan_reason", std::string("this machine does not manage "
                                                                  "vlans from here")));
#else
        json answer;
        if (!wantElevation(answer))
          return answer;
        if (!featuresJson().value("netplan", false))
          return refused("refused: " +
                         featuresJson().value("netplan_reason",
                                              std::string("this machine has no netplan")));
        if (action == "vlan_delete")
        {
          const std::string name = body.value("name", std::string());
          if (!safeIfname(name))
            return refused("refused: a vlan's name must be plain characters");
          json &managed = netplanStateRef();
          if (!managed["vlans"].contains(name))
            return refused("refused: this server never made a vlan named " + name);
          managed["vlans"].erase(name);
          storeNetplanState();
          std::string words;
          const Run applied = netplanApply(words);
          return netplanAnswer(applied, words);
        }
        if (action != "vlan_create")
          return refused("refused: no vlan action by that name");
        const std::string parent = body.value("parent", std::string());
        const long long id = body.value("id", 0ll);
        std::string name = body.value("name", std::string());
        if (name.empty())
          name = parent + "." + std::to_string(id);
        if (!safeIfname(parent) || !safeIfname(name) || id < 1 || id > 4094)
          return refused("refused: a vlan needs a parent it can name and an id between 1 and 4094");
        const std::string address = body.value("address", std::string());
        if (!address.empty() && !safeAddressish(address))
          return refused("refused: the address named for the vlan is not a plain address");
        json spec;
        spec["parent"] = parent;
        spec["id"] = id;
        spec["mode"] = address.empty() ? "dhcp" : "static";
        spec["address"] = address;
        spec["prefix"] = body.value("prefix", 24ll);
        spec["gateway"] = body.value("gateway", std::string());
        json dns = json::array();
        if (body.contains("dns") && body["dns"].is_array())
          for (const json &entry : body["dns"])
            if (entry.is_string() && safeAddressish(entry.get<std::string>()))
              dns.push_back(entry.get<std::string>());
        spec["dns"] = dns;
        spec["metric"] = body.value("metric", -1ll);
        json &managed = netplanStateRef();
        managed["vlans"][name] = spec;
        storeNetplanState();
        std::string words;
        const Run applied = netplanApply(words);
        return netplanAnswer(applied, words);
#endif
      }

      json actionShare(const std::string &action, const json &body)
      {
        const std::string external = body.value("external", std::string());
        const std::string internal = body.value("internal", std::string());
        json answer;
        if (!wantElevation(answer))
          return answer;
#ifdef _WIN32
        const json current = sharingWin();
        if (!current.value("supported", false))
          return refused("refused: " +
                         current.value("reason", std::string("this machine cannot share its "
                                                             "connection")));
        if (action == "share_disable")
        {
          // Take the sharing off every connection the machine says is sharing;
          // the machine's own tool answers.
          const Run run = runShell(
              "powershell -NoProfile -Command \"$ErrorActionPreference='Continue';$ns=New-Object "
              "-ComObject HNetCfg.HNetShare;$done=0;foreach($c in $ns.EnumEveryConnection)"
              "{try{$c.DisableInternetSharing();$done=1}catch{}};if($done){'the sharing was "
              "taken off'}else{'nothing was sharing'}\"");
          json file;
          file["sharing"] = nullptr;
          saveStateFile(file);
          return answerOf(run, "sharing is off", "the machine refused");
        }
        if (action != "share_enable")
          return refused("refused: no sharing action by that name");
        if (external.empty() || internal.empty() || external == internal ||
            !safeShellPart(external, true) || !safeShellPart(internal, true))
          return refused("refused: two different adapters are named, one that reaches the "
                         "internet and one to share it with");
        // The sharing service ships stopped: it learns to run first.
        runShell("sc config SharedAccess start= auto");
        runShell("net start SharedAccess");
        const Run run = runShell(
            "powershell -NoProfile -Command \"$ErrorActionPreference='Stop';$ns=New-Object "
            "-ComObject HNetCfg.HNetShare;if(-not $ns.SharingInstalled){Write-Output "
            "'this machine cannot share its connection';exit 2};$pub=$null;$priv=$null;foreach($c "
            "in $ns.EnumEveryConnection){$n=$c.NetConnectionProps.Name;if($n -eq '" + external +
            "'){$pub=$c};if($n -eq '" + internal + "'){$priv=$c}};if(-not $pub -or -not "
            "$priv){Write-Output 'no such adapter name on this machine';exit 2};try{"
            "$pub.EnableInternetSharing()}catch{Write-Output ('the public side refused: '+"
            "$_.Exception.Message);exit 3};$done='';try{$ns.SharingCollection.Add("
            "$priv.NetConnectionID);$done='the private connection was named'}catch{};if(-not "
            "$done){try{$ns.InternetConnectionSharingCollection.Add($priv.NetConnectionID);"
            "$done='the private connection was named'}catch{}};if(-not $done){$done='shared "
            "from the public side - name the private connection in the sharing page of the "
            "machine\\'s own settings to be sure'};Write-Output $done\"");
        if (run.ran && run.code == 0)
        {
          json state;
          state["external"] = external;
          state["internal"] = internal;
          json file;
          file["sharing"] = state;
          saveStateFile(file);
        }
        return answerOf(run, "the machine is sharing " + external + " with " + internal,
                        "the machine refused");
#else
        if (!featuresJson().value("sharing", false))
          return refused("refused: " +
                         featuresJson().value("sharing_reason",
                                              std::string("this machine has no iptables")));
        if (action == "share_disable")
        {
          if (!external.empty() && safeIfname(external))
          {
            runShell("iptables -t nat -D POSTROUTING -o " + external +
                     " -m comment --comment OLS-ICS -j MASQUERADE 2>/dev/null");
            runShell("iptables -D FORWARD -i " + external +
                     " -m conntrack --ctstate RELATED,ESTABLISHED -m comment --comment OLS-ICS "
                     "-j ACCEPT 2>/dev/null");
          }
          else
          {
            // Take off whatever this server left, by its own marks.
            const Run nat = runShell("iptables -t nat -S POSTROUTING 2>/dev/null | grep OLS-ICS");
            std::istringstream in{nat.out};
            std::string line;
            while (std::getline(in, line))
            {
              const std::string clean = trim(line);
              if (clean.rfind("-A ", 0) == 0)
                runShell("iptables -t nat -D " + clean.substr(3));
            }
            const Run fwd = runShell("iptables -S FORWARD 2>/dev/null | grep OLS-ICS");
            std::istringstream in2{fwd.out};
            while (std::getline(in2, line))
            {
              const std::string clean = trim(line);
              if (clean.rfind("-A ", 0) == 0)
                runShell("iptables -D " + clean.substr(3));
            }
          }
          runShell("rm -f /etc/sysctl.d/99-ols-ics.conf");
          json file;
          file["sharing"] = nullptr;
          saveStateFile(file);
          return didWords("the sharing rules this server wrote are taken off");
        }
        if (action != "share_enable")
          return refused("refused: no sharing action by that name");
        if (!safeIfname(external) || !safeIfname(internal) || external == internal)
          return refused("refused: two different adapters are named, by their plain names");
        runShell("sysctl -w net.ipv4.ip_forward=1");
        ols::writeFileText("/etc/sysctl.d/99-ols-ics.conf",
                           "# open-local-services: the network panel was asked to share\n"
                           "net.ipv4.ip_forward=1\n");
        const std::string mark = " -m comment --comment OLS-ICS";
        Run run = runShell("iptables -C POSTROUTING -t nat -o " + external + mark +
                           " -j MASQUERADE 2>/dev/null");
        if (!run.ran || run.code != 0)
          run = runShell("iptables -t nat -A POSTROUTING -o " + external + mark +
                         " -j MASQUERADE");
        if (run.ran && run.code == 0)
        {
          run = runShell("iptables -C FORWARD -i " + external +
                         " -m conntrack --ctstate RELATED,ESTABLISHED" + mark +
                         " -j ACCEPT 2>/dev/null");
          if (!run.ran || run.code != 0)
            run = runShell("iptables -A FORWARD -i " + external +
                           " -m conntrack --ctstate RELATED,ESTABLISHED" + mark + " -j ACCEPT");
        }
        if (run.ran && run.code == 0)
        {
          run = runShell("iptables -C FORWARD -i " + internal + " -o " + external + mark +
                         " -j ACCEPT 2>/dev/null");
          if (!run.ran || run.code != 0)
            run = runShell("iptables -A FORWARD -i " + internal + " -o " + external + mark +
                           " -j ACCEPT");
        }
        if (run.ran && run.code == 0)
        {
          json state;
          state["external"] = external;
          state["internal"] = internal;
          json file;
          file["sharing"] = state;
          saveStateFile(file);
        }
        return answerOf(run, "packets from " + internal + " leave through " + external +
                                 " by the rules this server wrote",
                        "the machine refused");
#endif
      }

      json actionAccessPoint(const std::string &action, const json &body)
      {
        std::string iface = body.value("interface", std::string());
        json answer;
        if (!wantElevation(answer))
          return answer;
#ifdef _WIN32
        {
          // The mobile hotspot is the machine's, shared through whatever
          // Internet connection stands; the tethering manager answers for the
          // radio, the band and every client, so this server only ever asks
          // it, in its own language, and reports what it says.
          json probe;
          const Run probeRun = runApScript("state");
          if (apRunAnswered(probeRun, probe).is_null())
          {
            const std::string words = probe.is_object()
                                          ? probe.value("error", std::string(""))
                                          : trim(probeRun.out);
            return refused("refused: " + (words.empty()
                                              ? std::string("this machine does not answer the "
                                                            "tethering manager")
                                              : words));
          }
          if (action == "ap_stop")
          {
            json answered;
            const Run run = runApScript("stop");
            const bool did = !apRunAnswered(run, answered).is_null();
            if (!did)
              forgetApArgs();
            json out;
            out["did"] = did;
            out["machine_words"] = answered.value("error", std::string(""));
            out["words"] = did ? "the mobile hotspot is down; its clients fell off with it"
                               : "the machine would not take the hotspot down: " +
                                     answered.value("error", std::string("(it said nothing)"));
            return out;
          }
          if (action != "ap_start")
            return refused("refused: no access-point action by that name");
          const std::string ssid = body.value("ssid", std::string());
          const std::string psk = body.value("psk", std::string());
          const std::string band = lower(body.value("band", std::string("auto")));
          if (ssid.empty() || ssid.size() > 32 || !safeShellPart(ssid, true))
            return refused("refused: an ssid is up to 32 plain characters");
          // A blank key is not an open hotspot - Windows does not run one - it
          // says leave the key the machine already carries.
          if (!psk.empty() && (psk.size() < 8 || psk.size() > 63 || !safeShellPart(psk, false)))
            return refused("refused: a hotspot key is 8 to 63 plain characters, or blank to "
                           "keep the machine's own");
          std::string bandEnum;
          if (band == "auto" || band.empty())
            bandEnum = "";
          else if (band == "2.4" || band == "2.4g" || band == "2")
            bandEnum = "TwoPointFourGigahertz";
          else if (band == "5" || band == "5g")
            bandEnum = "FiveGigahertz";
          else if (band == "6" || band == "6g")
            bandEnum = "SixGigahertz";
          else
            return refused("refused: the band named is not one of auto, 2.4, 5 and 6");
          std::error_code ec{};
          std::filesystem::create_directories("data", ec);
          if (!ols::writeFileText(apArgsPath(), ssid + "\n" + psk + "\n" + bandEnum + "\n"))
            return refused("refused: this server could not write its own argument file");
          json answered;
          const Run run = runApScript("start");
          forgetApArgs();
          const bool did = !apRunAnswered(run, answered).is_null();
          const std::string usedSsid =
              did && !answered.value("ssid", std::string("")).empty()
                  ? answered.value("ssid", std::string(""))
                  : ssid;
          const std::string usedBand = answered.value("band", std::string(""));
          json out;
          out["did"] = did;
          out["machine_words"] = answered.value("error", std::string(""));
          out["words"] =
              did ? "the mobile hotspot " + usedSsid + " is up, sharing what this machine "
                            "reaches" +
                        (bandEnum.empty()
                             ? ""
                             : ", on the " +
                                   (usedBand.empty() ? band : usedBand) +
                                   " band the machine picked") +
                        " - the channel is the radio's choice"
                  : "the machine would not stand the hotspot up: " +
                        answered.value("error", std::string("(it said nothing)"));
          return out;
        }
#else
        if (!featuresJson().value("hostapd", false))
          return refused("refused: " +
                         featuresJson().value("hostapd_reason",
                                              std::string("this machine has no hostapd")));
        if (iface.empty())
          iface = firstWifiNameLinux();
        if (iface.empty() || !safeIfname(iface))
          return refused("refused: this machine has no wifi interface to stand an access "
                         "point on");
        if (action == "ap_stop")
        {
          runShell("systemctl stop hostapd@" + iface + ".service 2>/dev/null");
          const Run run = runShell("if [ -f " + apPidPath(iface) + " ]; then kill $(cat " +
                                   apPidPath(iface) + ") 2>/dev/null; rm -f " + apPidPath(iface) +
                                   "; echo taken down; else echo nothing was standing; fi");
          json file;
          file["ap"] = nullptr;
          saveStateFile(file);
          return answerOf(run, "the access point is down", "the machine refused");
        }
        if (action != "ap_start")
          return refused("refused: no access-point action by that name");
        const std::string ssid = body.value("ssid", std::string());
        const std::string psk = body.value("psk", std::string());
        const std::string country = upper2(body.value("country", std::string("")));
        const std::string band = lower(body.value("band", std::string("2.4")));
        const long long channel = body.value("channel", 0ll);
        const long long maxClients = body.value("max_clients", 8ll);
        const long long beaconMs = body.value("beacon_ms", 100ll);
        if (ssid.empty() || ssid.size() > 63 || !safeShellPart(ssid, true))
          return refused("refused: the ssid named for the access point is quotable words of up "
                         "to 63 bytes");
        if (!psk.empty() && (psk.size() < 8 || psk.size() > 63))
          return refused("refused: an access-point key is 8 to 63 characters");
        if (!country.empty() &&
            !(country.size() == 2 && std::isalpha(static_cast<unsigned char>(country[0])) &&
              std::isalpha(static_cast<unsigned char>(country[1]))))
          return refused("refused: a country is two letters, as the radio's rules state them");
        if (channel < 0 || channel > 253)
          return refused("refused: a channel is a number up to 253, or 0 to let the radio choose");
        std::string hwMode = "g";
        bool sixGhz = false;
        if (band == "5")
          hwMode = "a";
        else if (band == "6")
        {
          hwMode = "a";
          sixGhz = true;
        }
        else if (band != "2.4" && band != "24" && band != "2" && band != "2g")
          return refused("refused: the band named is not one of 2.4, 5 and 6");
        long long settled = channel;
        if (settled <= 0)
          settled = hwMode == "g" ? 1 : (sixGhz ? 1 : 36);
        std::ostringstream conf;
        conf << "# open-local-services: the access point this machine's panel asked for\n"
             << "interface=" << iface << "\n"
             << "driver=nl80211\n"
             << "ctrl_interface=/var/run/hostapd-" << iface << "\n"
             << "ctrl_interface_group=0\n"
             << "ssid=" << ssid << "\n";
        if (!psk.empty())
          conf << "wpa=2\nwpa_passphrase=" << psk << "\nwpa_key_mgmt=WPA-PSK\n"
               << "rsn_pairwise=CCMP\n";
        conf << "hw_mode=" << hwMode << "\n"
             << "channel=" << settled << "\n"
             << "ieee80211n=1\n";
        if (hwMode == "a")
          conf << "ieee80211ac=1\nieee80211ax=1\n";
        if (sixGhz)
          conf << "he_6ghz_mode=1\n";
        conf << "beacon_int=" << beaconMs << "\n"
             << "max_num_sta=" << maxClients << "\n";
        if (!country.empty())
          conf << "country_code=" << country << "\n";
        std::error_code ec{};
        std::filesystem::create_directories("/etc/hostapd", ec);
        const std::string confPath = "/etc/hostapd/ols-" + iface + ".conf";
        if (!ols::writeFileText(confPath, conf.str()))
          return refused("refused: the machine would not write " + confPath);
        if (!country.empty())
          runShell("iw reg set " + country);
        runShell("mkdir -p /var/run/hostapd-" + iface);
        runShell("systemctl stop hostapd@" + iface + ".service 2>/dev/null");
        runShell("if [ -f " + apPidPath(iface) + " ]; then kill $(cat " + apPidPath(iface) +
                 ") 2>/dev/null; fi");
        const Run start = runShell("hostapd -P " + apPidPath(iface) + " -B " + confPath +
                                   " 2>&1; sleep 1; if [ -f " + apPidPath(iface) +
                                   " ]; then echo stood up; else echo hostapd would not stand "
                                   "up on this interface; exit 1; fi");
        if (start.ran && start.code == 0)
        {
          json ap;
          ap["interface"] = iface;
          json file;
          file["ap"] = ap;
          saveStateFile(file);
        }
        return answerOf(start,
                        "the access point " + ssid + " is up on channel " +
                            std::to_string(settled) + " of the " + band + " band",
                        "the machine refused");
#endif
      }

      std::string actBody(const std::string &bodyJson)
      {
        json body = json::parse(bodyJson, nullptr, false);
        if (body.is_discarded() || !body.is_object())
          return refused("refused: the ask was not one object of words").dump();
        const std::string action = body.value("action", std::string());
        State &s = state();
        // One change at a time: netsh and netplan are serial habits, and two
        // applies at once is how machines get confused.
        std::lock_guard<std::mutex> runLock(s.actionRunMutex);
        json result;
        if (action == "adapter_up" || action == "adapter_down")
          result = actionAdapter(action, body);
        else if (action == "mtu_set")
          result = actionMtu(body);
        else if (action == "ip_set")
          result = actionIp(body);
        else if (action == "wifi_connect")
          result = actionWifiConnect(body);
        else if (action == "wifi_disconnect")
          result = actionWifiDisconnect(body);
        else if (action == "wifi_forget")
          result = actionWifiForget(body);
        else if (action == "radio_block")
          result = actionRadio(body);
        else if (action == "bond_create" || action == "bond_delete")
          result = actionBond(action, body);
        else if (action == "vlan_create" || action == "vlan_delete")
          result = actionVlan(action, body);
        else if (action == "share_enable" || action == "share_disable")
          result = actionShare(action, body);
        else if (action == "ap_start" || action == "ap_stop")
          result = actionAccessPoint(action, body);
        else
          result = refused("refused: this door knows no action named " + action);
        result["action"] = action;
        rememberAction(action, result);
        return result.dump();
      }
    } // namespace

    void init()
    {
      // Started before the doors open: the history the page draws is already
      // filling when the first glance arrives.
      initBody();
    }

    std::string snapshotJson()
    {
      return snapshotBody();
    }

    std::string scanJson(bool force)
    {
      return scanBody(force);
    }

    std::string actJson(const std::string &bodyJson)
    {
      return actBody(bodyJson);
    }
  } // namespace net
} // namespace ols
