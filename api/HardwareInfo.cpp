// What the silicon itself states about the machine. On Windows this is the
// operating system's own inventory (WMI), reached through COM exactly as any
// management tool reaches it - the machine naming its own memory sticks in
// words. On Linux the same inventory stands as the firmware's structure table
// under /sys, and it is walked the way the machine's own dmidecode walks it -
// a string area ended by the run of zeros the firmware pads, and type-17
// fields read only at the offsets a real machine has been checked against,
// with any field a record is too short to hold left unstated. The picture
// card's speed is asked of the vendor management library, which is loaded only
// if it stands with the driver; a card that names nothing names nothing here
// too.
#include "HardwareInfo.h"

#include <string>

#if defined(__linux__)
#include <dlfcn.h>
#include <cstdio>
#include <vector>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wbemidl.h>
#include <oleauto.h>
#include <cwchar>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#endif

namespace ols
{
  namespace platform
  {
    // The firmware names the memory technology with a number. Only the ones
    // this fleet runs are named in words; an unnamed number says nothing
    // rather than guessing at the machine.
    namespace
    {
      std::string memoryKind(long long smbiosCode)
      {
        switch (smbiosCode)
        {
          case 34: return "DDR5";
          case 26: return "DDR4";
          case 24: return "DDR3";
          default: return {};
        }
      }
    } // namespace

#if defined(_WIN32)
    namespace
    {
      // One property of one inventory object, read as a whole number. The
      // inventory keeps its numbers in several widths and once kept a count in a
      // string, so the value is asked to become a 64-bit integer whatever shape
      // it came in; one that cannot become a number is no answer (found false).
      long long getInt(IWbemClassObject *obj, const wchar_t *name, bool &found)
      {
        found = false;
        VARIANT value;
        VariantInit(&value);
        if (FAILED(obj->Get(name, 0, &value, nullptr, nullptr)))
        {
          VariantClear(&value);
          return 0;
        }
        long long out = 0;
        if (value.vt != VT_EMPTY && value.vt != VT_NULL &&
            SUCCEEDED(VariantChangeType(&value, &value, 0, VT_I8)))
        {
          out = value.llVal;
          found = true;
        }
        VariantClear(&value);
        return out;
      }

      // One property read as text, trimmed of the padding the inventory leaves
      // on the part numbers it carries in a fixed field.
      std::string getStr(IWbemClassObject *obj, const wchar_t *name)
      {
        VARIANT value;
        VariantInit(&value);
        if (FAILED(obj->Get(name, 0, &value, nullptr, nullptr)))
        {
          VariantClear(&value);
          return {};
        }
        std::string out;
        if (value.vt == VT_BSTR && value.bstrVal)
        {
          const int len = SysStringLen(value.bstrVal);
          char buf[256];
          const int n = WideCharToMultiByte(CP_UTF8, 0, value.bstrVal, len, buf, sizeof(buf) - 1, nullptr,
                                            nullptr);
          if (n > 0)
          {
            buf[n] = '\0';
            out = buf;
          }
        }
        VariantClear(&value);
        const size_t first = out.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
          return {};
        const size_t last = out.find_last_not_of(" \t\r\n");
        return out.substr(first, last - first + 1);
      }

      MemorySticks queryMemorySticks()
      {
        MemorySticks result;
        // The apartment and its security are set only where this call is the one
        // that opens them; a process already living in another model still has
        // the inventory to reach, so a mode clash is carried on past, not failed
        // on. A standing server reaches the inventory once, from the first page
        // that asks for the machine's memory.
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool ownsApartment = SUCCEEDED(apartment);
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                             RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);

        IWbemLocator *locator = nullptr;
        IWbemServices *services = nullptr;
        IEnumWbemClassObject *rows = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                       reinterpret_cast<void **>(&locator))))
        {
          BSTR path = SysAllocString(L"ROOT\\CIMV2");
          const HRESULT connected = locator->ConnectServer(path, nullptr, nullptr, nullptr, 0, nullptr,
                                                           nullptr, &services);
          SysFreeString(path);
          if (SUCCEEDED(connected) && services)
          {
            CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                              RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
            BSTR lang = SysAllocString(L"WQL");
            BSTR query = SysAllocString(L"SELECT Capacity, Speed, ConfiguredClockSpeed, DataWidth, "
                                        L"SMBIOSMemoryType, Manufacturer, PartNumber FROM Win32_PhysicalMemory");
            const HRESULT asked = services->ExecQuery(lang, query,
                                                      WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                                      nullptr, &rows);
            SysFreeString(query);
            SysFreeString(lang);
            if (SUCCEEDED(asked) && rows)
            {
              long long widest = 0; // fastest configured speed any stick states
              long long top = 0;    // fastest rated speed any stick states
              bool kindNamed = false;
              for (;;)
              {
                IWbemClassObject *row = nullptr;
                ULONG taken = 0;
                if (FAILED(rows->Next(WBEM_INFINITE, 1, &row, &taken)) || taken == 0)
                  break;
                if (row)
                {
                  bool has = false;
                  const long long capacity = getInt(row, L"Capacity", has);
                  if (has && capacity > 0) // a filled place the firmware describes
                  {
                    ++result.modules;
                    result.bytes += capacity;
                    bool seen = false;
                    const long long width = getInt(row, L"DataWidth", seen);
                    if (seen && width > 0)
                      result.dataBits += width;
                    bool confSeen = false;
                    const long long configured = getInt(row, L"ConfiguredClockSpeed", confSeen);
                    if (confSeen && configured > widest)
                      widest = configured;
                    bool ratedSeen = false;
                    const long long rated = getInt(row, L"Speed", ratedSeen);
                    if (ratedSeen && rated > top)
                      top = rated;
                    bool kindSeen = false;
                    const long long code = getInt(row, L"SMBIOSMemoryType", kindSeen);
                    if (!kindNamed && kindSeen && code > 0)
                    {
                      const std::string named = memoryKind(code);
                      if (!named.empty())
                      {
                        result.kind = named;
                        kindNamed = true;
                      }
                    }
                    if (result.maker.empty())
                      result.maker = getStr(row, L"Manufacturer");
                    if (result.part.empty())
                      result.part = getStr(row, L"PartNumber");
                  }
                  row->Release();
                }
              }
              result.configuredMts = widest;
              result.ratedMts = top;
              // A statement is worth making only when the bus can be summed:
              // sticks, and a data path wide enough to name a rate over.
              result.ok = result.modules > 0 && result.dataBits > 0 && widest > 0;
            }
          }
        }
        if (rows)
          rows->Release();
        if (services)
          services->Release();
        if (locator)
          locator->Release();
        if (ownsApartment)
          CoUninitialize();
        return result;
      }
    } // namespace
#endif // _WIN32

#if defined(__linux__)
    namespace
    {
      // One file, whole, as bytes; a file the kernel does not offer is no answer.
      bool readWhole(const char *path, std::string &out)
      {
        std::FILE *f = std::fopen(path, "rb");
        if (!f)
          return false;
        char buf[4096];
        size_t got;
        out.clear();
        while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0)
          out.append(buf, got);
        const bool good = !ferror(f);
        std::fclose(f);
        return good && !out.empty();
      }

      // The firmware's structure table, walked as its own readers walk it: a
      // record is its header, a formatted area, and a string area ended by a
      // run of zeros - two where a record carries no strings, which is how
      // this fleet's machines pad them. The next record starts at the first
      // byte after the last zero, so a table is read whole without trusting
      // any record's shape but its own.
      MemorySticks readMemorySticks()
      {
        MemorySticks result;
        std::string d;
        if (!readWhole("/sys/firmware/dmi/tables/DMI", d))
          return result;
        const unsigned char *b = reinterpret_cast<const unsigned char *>(d.data());
        const size_t n = d.size();
        auto wordAt = [b](size_t at) { return static_cast<long long>(b[at]) | (static_cast<long long>(b[at + 1]) << 8); };
        auto textAt = [&](size_t at, const std::vector<std::string> &strs) -> std::string {
          const int index = b[at]; // string areas number their strings from one
          if (index < 1 || static_cast<size_t>(index) > strs.size())
            return {};
          // Firmware carries part numbers in padded fields; the padding is not
          // part of the number.
          const std::string &raw = strs[static_cast<size_t>(index) - 1];
          const size_t first = raw.find_first_not_of(" \t\r\n");
          if (first == std::string::npos)
            return {};
          return raw.substr(first, raw.find_last_not_of(" \t\r\n") - first + 1);
        };

        size_t i = 0;
        long long widest = 0; // fastest configured speed any stick states
        long long top = 0;    // fastest rated speed any stick states
        bool kindNamed = false;
        while (i + 4 <= n)
        {
          const int type = b[i];
          const int length = b[i + 1];
          if (length < 4 || i + static_cast<size_t>(length) > n)
            break;
          std::vector<std::string> strs;
          size_t j = i + static_cast<size_t>(length);
          while (j < n && b[j] != 0)
          {
            const size_t end = d.find('\0', j);
            strs.push_back(d.substr(j, end - j));
            j = end + 1;
          }
          while (j < n && b[j] == 0)
            ++j;

          if (type == 17 && length >= 0x17) // a memory device, old enough to hold a speed
          {
            // The size word carries its own unit in its top bits, the way this
            // fleet's firmwares fill it; a word left at zero, unknown, or
            // "no module" marks an empty place, which adds nothing to the bus.
            long long sizeMb = 0;
            const long long w = wordAt(i + 0x0C);
            if (w != 0 && w != 0x8000 && w != 0xFFFF)
            {
              if (w & 0x8000)
                sizeMb = (w & 0x7FFF) * 1024; // gigabytes
              else if (w & 0x4000)
                sizeMb = w & 0x7FFF; // megabytes
              else
                sizeMb = (w & 0x7FFF) / 1024; // kilobytes
            }
            if (sizeMb > 0 && sizeMb <= 64LL * 1024 * 1024)
            {
              ++result.modules;
              result.bytes += sizeMb * 1048576;
              const long long width = wordAt(i + 0x0A);
              if (width > 0 && width != 0xFFFF)
                result.dataBits += width;
              const long long rated = wordAt(i + 0x15);
              if (rated > top)
                top = rated;
              // The speed the stick runs at now, where the record is long
              // enough to state it; where it does not, the rated speed is the
              // only speed the machine has named for it.
              const long long running = length >= 0x22 ? wordAt(i + 0x20) : 0;
              if (running > widest)
                widest = running;
              else if (rated > widest && length < 0x22)
                widest = rated;
              const std::string kind = memoryKind(b[i + 0x12]);
              if (!kindNamed && !kind.empty())
              {
                result.kind = kind;
                kindNamed = true;
              }
              if (length >= 0x18)
              {
                if (result.maker.empty())
                  result.maker = textAt(i + 0x17, strs);
                if (length >= 0x1B && result.part.empty())
                  result.part = textAt(i + 0x1A, strs);
              }
            }
          }
          i = j;
        }
        // What an "Unknown" names is not known; it is kept as no word at all.
        if (result.maker == "Unknown" || result.maker == "unknown")
          result.maker.clear();
        result.configuredMts = widest;
        result.ratedMts = top;
        result.ok = result.modules > 0 && result.dataBits > 0 && widest > 0;
        return result;
      }
    } // namespace
#endif // __linux__

    const MemorySticks &memorySticks()
    {
#if defined(_WIN32)
      static const MemorySticks sticks = queryMemorySticks();
      return sticks;
#elif defined(__linux__)
      // The kernel has offered the structure table to root since long enough
      // for every machine in this fleet; a machine that offers nothing keeps
      // no rating, and the page says so rather than guessing.
      static const MemorySticks sticks = readMemorySticks();
      return sticks;
#else
      static const MemorySticks none = {};
      return none;
#endif
    }

    GpuClocks gpuClocks()
    {
      GpuClocks out;
#if defined(_WIN32) || defined(__linux__)
      // The card answers about its own clocks through the management library the
      // driver stands with. The functions are bound by name at run time, exactly
      // as the picture service asks the same library for the card's whole bytes,
      // so a machine without an NVIDIA driver simply has no library to load and
      // the card says nothing - the honest reading for an AMD core, whose clocks
      // this server will not guess.
      // The calling convention is the one every x64 platform fixes, so the
      // pointers are declared plain.
      using InitFn = int (*)();
      using CountFn = int (*)(unsigned *);
      using HandleFn = int (*)(unsigned, void **);
      using ClockFn = int (*)(void *, int, unsigned *);
      using ShutdownFn = int (*)();
#if defined(_WIN32)
      const HMODULE library = LoadLibraryA("nvml.dll");
      auto bind = [](HMODULE module, const char *name) { return reinterpret_cast<void *>(GetProcAddress(module, name)); };
#else
      void *library = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
      auto bind = [](void *module, const char *name) { return module ? dlsym(module, name) : nullptr; };
#endif
      if (library)
      {
        const InitFn init = reinterpret_cast<InitFn>(bind(library, "nvmlInit_v2"));
        const CountFn count = reinterpret_cast<CountFn>(bind(library, "nvmlDeviceGetCount_v2"));
        const HandleFn handle = reinterpret_cast<HandleFn>(bind(library, "nvmlDeviceGetHandleByIndex_v2"));
        const ClockFn clock = reinterpret_cast<ClockFn>(bind(library, "nvmlDeviceGetClockInfo"));
        const ClockFn maxClock = reinterpret_cast<ClockFn>(bind(library, "nvmlDeviceGetMaxClockInfo"));
        const ShutdownFn shutdown = reinterpret_cast<ShutdownFn>(bind(library, "nvmlShutdown"));
        unsigned devices = 0;
        void *card = nullptr;
        // The clock kinds the library names by number: 0 is the graphics clock,
        // 2 the memory clock, fixed across the library's versions.
        if (init && count && handle && clock && init() == 0 && count(&devices) == 0 && devices > 0 &&
            handle(0, &card) == 0 && card)
        {
          unsigned value = 0;
          if (clock(card, 0, &value) == 0 && value > 0)
          {
            out.coreMhz = value;
            out.ok = true;
          }
          if (maxClock && maxClock(card, 0, &value) == 0 && value > 0)
            out.coreMaxMhz = value;
          if (clock(card, 2, &value) == 0 && value > 0)
          {
            out.memMhz = value;
            out.ok = true;
          }
          if (maxClock && maxClock(card, 2, &value) == 0 && value > 0)
            out.memMaxMhz = value;
        }
        if (shutdown)
          shutdown();
      }
#if defined(_WIN32)
      if (library)
        FreeLibrary(library);
#else
      if (library)
        dlclose(library);
#endif
#endif // _WIN32 || __linux__
      return out;
    }
  } // namespace platform
} // namespace ols
