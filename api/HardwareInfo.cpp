// What the silicon itself states about the machine. On Windows this is the
// operating system's own inventory (WMI), reached through COM exactly as any
// management tool reaches it - the machine naming its own memory sticks in
// words, so no byte offset of the firmware table is trusted from a hand that
// could miscount it. The picture card's speed is asked of the vendor management
// library, which is loaded only if it stands with the driver; a card that names
// nothing names nothing here too.
#include "HardwareInfo.h"

#include <string>

#if defined(__linux__)
#include <dlfcn.h>
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

      // The firmware names the memory technology with a number. Only the ones
      // this fleet runs are named in words; an unnamed number says nothing
      // rather than guessing at the machine.
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

    const MemorySticks &memorySticks()
    {
#if defined(_WIN32)
      static const MemorySticks sticks = queryMemorySticks();
      return sticks;
#else
      // Linux keeps the same inventory in the firmware table under /sys, but a
      // machine has been seen to lay that table out by a different plan than the
      // one written - so it is not read here by a hand-picked byte until it has
      // been read whole against a machine that proves the layout. Until then the
      // Linux memory keeps no rating, and the page says so rather than guessing.
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
      using InitFn = int(__cdecl *)();
      using CountFn = int(__cdecl *)(unsigned *);
      using HandleFn = int(__cdecl *)(unsigned, void **);
      using ClockFn = int(__cdecl *)(void *, int, unsigned *);
      using ShutdownFn = int(__cdecl *)();
#if defined(_WIN32)
      const HMODULE library = LoadLibraryA("nvml.dll");
      auto bind = [](HMODULE module, const char *name) { return reinterpret_cast<void *>(GetProcAddress(module, name)); };
#else
      const void *library = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
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
