// The engine library beside the program, opened at start. See EngineLoader.h.
#include "EngineLoader.h"

#include "Deployment.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace ols
{
  namespace imagegen
  {
    namespace
    {
      // One library, opened once, kept for the life of the process: the engine
      // keeps state in itself, and no road here asks for it to be put down.
      struct Loaded
      {
        EngineApi api;
        std::string shape;
        std::string words;
        bool stands = false;
      };
      Loaded gLoaded;
      std::once_flag gWarmOnce;

#ifdef _WIN32
      using Library = HMODULE;
      Library openLibrary(const std::string &path) { return LoadLibraryA(path.c_str()); }
      void *findName(Library library, const char *name) { return reinterpret_cast<void *>(GetProcAddress(library, name)); }
      const char *libraryWords() { return "the machine's loader refused this file"; }
#else
      using Library = void *;
      Library openLibrary(const std::string &path) { return dlopen(path.c_str(), RTLD_NOW); }
      void *findName(Library library, const char *name) { return dlsym(library, name); }
      const char *libraryWords()
      {
        const char *why = dlerror();
        return why && *why ? why : "the loader refused this file";
      }
#endif

      // The names the engine must answer with, and where they land. A library
      // that keeps any of them back is not an engine for this server.
      template <typename Fn>
      bool take(Library library, const char *name, Fn &into)
      {
        void *found = findName(library, name);
        if (!found)
          return false;
        into = reinterpret_cast<Fn>(found);
        return true;
      }

      // Where the libraries live: a named room first, then the folder the
      // program itself stands in, then the name as the machine's loader
      // searches it. One law for every shape.
      std::vector<std::string> candidates(const std::string &file)
      {
        std::vector<std::string> out;
        const std::string room = ols::deploy::configWord("OLS_ENGINE_DIR");
        if (!room.empty())
          out.push_back((std::filesystem::path(room) / file).string());
        const std::filesystem::path here{ols::deploy::selfProgram()};
        out.push_back((here.parent_path() / file).string());
        out.push_back(file);
        return out;
      }

      // The card answers through nvml: it stands with the driver on any machine
      // that runs one of these cards, asking nothing of toolkits and costing no
      // CUDA context. The function names are the driver's stable ones.
      struct Nvml
      {
        long long totalBytes = 0;
        long long freeBytes = 0;
        int major = -1;
        int minor = 0;
        bool answers = false;
      };
      Nvml probeNvml()
      {
        Nvml out;
        struct NvmlMemory
        {
          unsigned long long total, free, used;
        };
        using InitFn = int (*)();
        using CountFn = int (*)(unsigned *);
        using HandleFn = int (*)(unsigned, void **);
        using MemoryFn = int (*)(void *, NvmlMemory *);
        using CapFn = int (*)(void *, int *, int *);
#ifdef _WIN32
        Library nvml = LoadLibraryA("nvml.dll");
        auto sym = [nvml](const char *name)
        { return reinterpret_cast<void *>(GetProcAddress(nvml, name)); };
#else
        Library nvml = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
        auto sym = [nvml](const char *name)
        { return nvml ? dlsym(nvml, name) : nullptr; };
#endif
        if (!nvml)
          return out;
        const InitFn init = reinterpret_cast<InitFn>(sym("nvmlInit_v2"));
        const CountFn count = reinterpret_cast<CountFn>(sym("nvmlDeviceGetCount_v2"));
        const HandleFn handle = reinterpret_cast<HandleFn>(sym("nvmlDeviceGetHandleByIndex_v2"));
        const MemoryFn memory = reinterpret_cast<MemoryFn>(sym("nvmlDeviceGetMemoryInfo"));
        const CapFn cap = reinterpret_cast<CapFn>(sym("nvmlDeviceGetCudaComputeCapability"));
        unsigned devices = 0;
        void *card = nullptr;
        if (!init || !count || !handle || !memory || init() != 0 || count(&devices) != 0 ||
            devices == 0 || handle(0, &card) != 0 || !card)
          return out;
        NvmlMemory mem{};
        if (memory(card, &mem) == 0 && mem.total > 0)
        {
          out.totalBytes = static_cast<long long>(mem.total);
          out.freeBytes = static_cast<long long>(mem.free);
        }
        int major = -1;
        int minor = 0;
        if (cap && cap(card, &major, &minor) == 0)
        {
          out.major = major;
          out.minor = minor;
        }
        out.answers = out.totalBytes > 0 || out.major >= 0;
        return out;
      }

      Nvml gCard;
      std::once_flag gCardOnce;

      // What this box is: the card's word when nvml answers it, the vulkan
      // loader's name when it opens, and the cpu under all of it. The kinds
      // come back in the order worth trying.
#ifdef _WIN32
      // Whether this chip answers the avx512 questions the avx512 cpu engine
      // was compiled against. That claim is written straight into the code on
      // either platform: the cmake option hands -mavx512f to every object of
      // the cpu backend, the compiler then uses those registers wherever it
      // likes - including where no dispatch guard stands - and the engine's own
      // ggml_cpu_has_avx512() answers from the macro the option set, so a
      // library built that way never finds out that the chip below it has none.
      // Seen on the linux side 2026-11-27: such an engine, built by a door that
      // named no flavour, died of SIGILL inside ggml_cpu_init on a chip with
      // avx2 and nothing above it. So the flavoured engine goes only to a chip
      // that says yes to f, vbmi and vnni itself, on either road.
      bool chipAnswersAvx512()
      {
        int regs[4] = {0, 0, 0, 0};
        __cpuid(regs, 0);
        if (regs[0] < 7)
          return false;
        __cpuidex(regs, 7, 0);
        const bool f = (regs[1] & (1 << 16)) != 0;
        const bool vbmi = (regs[1] & (1 << 1)) != 0;
        const bool vnni = (regs[1] & (1 << 11)) != 0;
        return f && vbmi && vnni;
      }
#elif defined(__x86_64__) || defined(__i386__)
      bool chipAnswersAvx512()
      {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx512f") != 0 && __builtin_cpu_supports("avx512vbmi") != 0 &&
               __builtin_cpu_supports("avx512vnni") != 0;
      }
#else
      bool chipAnswersAvx512()
      {
        return false;
      }
#endif
      std::vector<std::string> machineOrder()
      {
        std::vector<std::string> order;
        const std::string named = ols::deploy::configWord("OLS_ENGINE");
        if (!named.empty())
        {
          // The owner's word names the engine library beside the server: it may
          // be one of the three kinds or a flavored name like cpu-avx512, so any
          // plain lower-case word is honored, and anything that could not be a
          // file's name is passed by.
          bool namesAFile = !named.empty();
          for (const char c : named)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
              namesAFile = false;
          if (namesAFile)
            return {named};
        }
        std::call_once(gCardOnce, [] { gCard = probeNvml(); });
        if (gCard.answers)
          order.push_back("cuda");
#ifdef _WIN32
        if (HMODULE loader = LoadLibraryA("vulkan-1.dll"))
        {
          (void)loader;
          order.push_back("vulkan");
        }
        if (chipAnswersAvx512())
          order.push_back("cpu-avx512");
#else
        if (void *loader = dlopen("libvulkan.so.1", RTLD_LAZY))
        {
          dlclose(loader);
          order.push_back("vulkan");
        }
        // the flavoured cpu engine is asked for here as it is on the windows
        // road: by name, and only of a chip that answers for the claim that
        // name carries. a tree that built no such library simply is not there,
        // and the plain cpu engine below answers.
        if (chipAnswersAvx512())
          order.push_back("cpu-avx512");
#endif
        order.push_back("cpu");
        return order;
      }

      // The CUDA runtime the engine library asks for by bare name, found on this
      // machine and put in this process's way - once, and only after an engine
      // has already been refused. Every road that starts this server is entitled
      // to know nothing about toolkits: run.bat carries the CUDA bin room in its
      // own PATH, and a machine's scheduled task or systemd unit starts this
      // program directly, with no such thought behind it. A box that resolves
      // cuBLAS happily is never disturbed by any of this, because none of it
      // runs until a load has been refused.
#ifdef _WIN32
      bool holdsCublas(const std::filesystem::path &folder)
      {
        std::error_code ec{};
        if (!std::filesystem::is_directory(folder, ec))
          return false;
        for (const auto &entry : std::filesystem::directory_iterator(folder, ec))
        {
          const std::string name = entry.path().filename().string();
          if (name.rfind("cublas", 0) == 0 && name.size() > 4 &&
              name.compare(name.size() - 4, 4, ".dll") == 0)
            return true;
        }
        return false;
      }

      // Where a CUDA toolkit of this machine stands, read from the machine: the
      // folders its own registry records, the environment's own names, and the
      // folder every toolkit installs under when nobody says otherwise.
      std::vector<std::string> toolkitRooms()
      {
        std::vector<std::string> rooms;
        const auto addRoom = [&rooms](const std::string &home) {
          if (home.empty())
            return;
          std::error_code ec{};
          std::filesystem::path trimmed(home);
          while (!trimmed.string().empty() && (trimmed.string().back() == '\\' ||
                                               trimmed.string().back() == '/'))
            trimmed = trimmed.parent_path();
          const std::filesystem::path bin = trimmed / "bin";
          if (holdsCublas(bin))
            rooms.push_back(bin.string());
          else if (holdsCublas(trimmed))
            rooms.push_back(trimmed.string());
        };
        for (const char *named : {"CUDA_PATH", "CUDA_HOME"})
          if (const char *value = std::getenv(named))
            addRoom(value);
        const HKEY roots[] = {HKEY_LOCAL_MACHINE};
        const char *keys[] = {"SOFTWARE\\NVIDIA Corporation\\GPU Computing Toolkit\\CUDA",
                              "SOFTWARE\\WOW6432Node\\NVIDIA Corporation\\GPU Computing Toolkit\\CUDA"};
        for (HKEY root : roots)
        {
          for (const char *key : keys)
          {
            HKEY toolkits = nullptr;
            if (RegOpenKeyExA(root, key, 0, KEY_READ | KEY_WOW64_64KEY, &toolkits) != ERROR_SUCCESS)
              continue;
            for (DWORD index = 0;; ++index)
            {
              char version[256];
              DWORD versionLen = sizeof(version);
              if (RegEnumKeyExA(toolkits, index, version, &versionLen, nullptr, nullptr, nullptr,
                                nullptr) != ERROR_SUCCESS)
                break;
              HKEY one = nullptr;
              if (RegOpenKeyExA(toolkits, version, 0, KEY_READ | KEY_WOW64_64KEY, &one) !=
                  ERROR_SUCCESS)
                continue;
              char folder[1024];
              DWORD folderLen = sizeof(folder);
              DWORD kind = 0;
              if (RegQueryValueExA(one, "InstallDir", nullptr, &kind,
                                   reinterpret_cast<LPBYTE>(folder), &folderLen) == ERROR_SUCCESS &&
                  (kind == REG_SZ || kind == REG_EXPAND_SZ))
                addRoom(std::string(folder, folderLen > 0 ? folderLen - 1 : 0));
              RegCloseKey(one);
            }
            RegCloseKey(toolkits);
          }
        }
        const char *underPrograms = "C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA";
        std::error_code ec{};
        if (std::filesystem::is_directory(underPrograms, ec))
        {
          std::vector<std::string> found;
          for (const auto &entry : std::filesystem::directory_iterator(underPrograms, ec))
            if (entry.is_directory())
              found.push_back(entry.path().string());
          // Newest version first: a box with two toolkits is asked to run the one
          // its engine was compiled against, and the run scripts look the same way.
          std::sort(found.begin(), found.end(),
                    [](const std::string &a, const std::string &b) { return a > b; });
          for (const std::string &home : found)
            addRoom(home);
        }
        return rooms;
      }

      // The machine speaks its paths in wide letters; these roads read them in
      // the narrow ones the rest of the server carries.
      std::wstring toWide(const std::string &text)
      {
        if (text.empty())
          return std::wstring();
        const int needed = MultiByteToWideChar(CP_ACP, 0, text.c_str(),
                                               static_cast<int>(text.size()), nullptr, 0);
        if (needed <= 0)
          return std::wstring();
        std::wstring out(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), &out[0],
                            needed);
        return out;
      }

      bool carryRuntimeOnce()
      {
        static const bool widened = [] {
          const std::vector<std::string> rooms = toolkitRooms();
          if (rooms.empty())
            return false;
          // The documented way to widen a process's own search: the default
          // directories stay in it - the program's folder, the system's, and any
          // room added here - and only this process is affected.
          if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                        LOAD_LIBRARY_SEARCH_USER_DIRS))
            return false;
          bool any = false;
          for (const std::string &room : rooms)
          {
            const std::wstring wide = toWide(room);
            if (!wide.empty() && AddDllDirectory(wide.c_str()))
              any = true;
          }
          return any;
        }();
        return widened;
      }
#else
      bool holdsCublas(const std::filesystem::path &folder)
      {
        std::error_code ec{};
        if (!std::filesystem::is_directory(folder, ec))
          return false;
        for (const auto &entry : std::filesystem::directory_iterator(folder, ec))
        {
          const std::string name = entry.path().filename().string();
          if (name.rfind("libcublas", 0) == 0 && name.find(".so") != std::string::npos)
            return true;
        }
        return false;
      }

      // The families an engine of this shape names in its own list, taken by the
      // soname it would ask for - the shortest name the folder offers for each.
      void openRuntimeFamily(const std::filesystem::path &room, const char *family)
      {
        std::error_code ec{};
        std::string shortest;
        for (const auto &entry : std::filesystem::directory_iterator(room, ec))
        {
          const std::string name = entry.path().filename().string();
          if (name.rfind(family, 0) != 0 || name.find(".so") == std::string::npos)
            continue;
          if (shortest.empty() || name.size() < shortest.size())
            shortest = name;
        }
        if (!shortest.empty())
          dlopen((room / shortest).string().c_str(), RTLD_LAZY | RTLD_GLOBAL);
      }

      bool carryRuntimeOnce()
      {
        static const bool widened = [] {
          std::vector<std::string> rooms;
          if (const char *value = std::getenv("CUDA_HOME"))
            rooms.push_back(std::string(value) + "/lib64");
          std::error_code ec{};
          if (std::filesystem::is_directory("/usr/local", ec))
          {
            std::vector<std::string> found;
            for (const auto &entry : std::filesystem::directory_iterator("/usr/local", ec))
            {
              if (!entry.is_directory())
                continue;
              const std::string name = entry.path().filename().string();
              if (name.rfind("cuda", 0) == 0)
                found.push_back((entry.path() / "lib64").string());
            }
            std::sort(found.begin(), found.end(),
                      [](const std::string &a, const std::string &b) { return a > b; });
            for (const std::string &room : found)
              rooms.push_back(room);
          }
          bool any = false;
          for (const std::string &room : rooms)
          {
            if (!holdsCublas(room))
              continue;
            for (const char *family :
                 {"libcublasLt.", "libcublas.", "libcudart.", "libcufft.", "libnvrtc."})
              openRuntimeFamily(room, family);
            any = true;
          }
          return any;
        }();
        return widened;
      }
#endif

      // Open one shape. The file's own name says which shape the machine asked
      // for; nothing here guesses at what the file holds.
      bool tryShape(const std::string &shape, std::vector<std::string> &tries)
      {
#ifdef _WIN32
        const std::string file = "stable-diffusion-" + shape + ".dll";
#else
        const std::string file = "libstable-diffusion-" + shape + ".so";
#endif
        for (const std::string &path : candidates(file))
        {
          Library library = openLibrary(path);
          if (!library && carryRuntimeOnce())
          {
            // The refusal may name nothing but a runtime this road did not
            // carry. Ask the same file once more with the machine's own CUDA
            // rooms in the way; where there was nothing to add, this line is
            // never reached, and where there was, the second answer is the
            // loader's own.
            library = openLibrary(path);
          }
          if (!library)
          {
            tries.push_back(file + ": " + libraryWords());
            continue;
          }
          EngineApi api;
          bool whole = true;
          whole = whole && take(library, "new_sd_ctx", api.new_sd_ctx);
          whole = whole && take(library, "free_sd_ctx", api.free_sd_ctx);
          whole = whole && take(library, "generate_image", api.generate_image);
          whole = whole && take(library, "generate_video", api.generate_video);
          whole = whole && take(library, "sd_cancel_generation", api.sd_cancel_generation);
          whole = whole && take(library, "sd_list_devices", api.sd_list_devices);
          whole = whole && take(library, "sd_set_log_callback", api.sd_set_log_callback);
          whole = whole && take(library, "sd_set_progress_callback", api.sd_set_progress_callback);
          whole = whole && take(library, "sd_ctx_params_init", api.sd_ctx_params_init);
          whole = whole && take(library, "sd_sample_params_init", api.sd_sample_params_init);
          whole = whole && take(library, "sd_img_gen_params_init", api.sd_img_gen_params_init);
          whole = whole && take(library, "sd_vid_gen_params_init", api.sd_vid_gen_params_init);
          whole = whole && take(library, "sd_get_default_sample_method", api.sd_get_default_sample_method);
          whole = whole && take(library, "sd_get_default_scheduler", api.sd_get_default_scheduler);
          whole = whole && take(library, "free_sd_audio", api.free_sd_audio);
          whole = whole && take(library, "free_sd_images", api.free_sd_images);
          whole = whole && take(library, "str_to_sd_type", api.str_to_sd_type);
          whole = whole && take(library, "str_to_sample_method", api.str_to_sample_method);
          whole = whole && take(library, "str_to_scheduler", api.str_to_scheduler);
          if (!whole)
          {
            // A library under this name that does not answer the engine's own
            // names is something else wearing the file: say so and keep looking.
            tries.push_back(file + ": it does not answer the engine's names");
#ifdef _WIN32
            FreeLibrary(library);
#else
            dlclose(library);
#endif
            continue;
          }
          gLoaded.api = api;
          gLoaded.shape = shape;
          gLoaded.stands = true;
          std::string words = "the image engine stands: " + shape + ", opened from " + path;
          if (shape == "cuda" && gCard.major >= 0)
            words += ", card sm_" + std::to_string(gCard.major) + std::to_string(gCard.minor);
          else if (shape == "cuda")
            words += ", no card answered nvml";
          gLoaded.words = words;
          return true;
        }
        tries.push_back(file + ": no such file on any of its roads");
        return false;
      }

      void warmNow()
      {
        // The ggml inside the engine library installs its own crash handler
        // when it is opened, and a ggml that finds the handler claimed twice
        // stops the process before it can listen - the reason the run scripts
        // export GGML_NO_BACKTRACE. A systemd unit starting this binary never
        // sees a script's exports: the production road must carry the same
        // posture as the development one, so the word is said from inside the
        // process, before the first engine open, whatever road started this.
        // A crash still prints its own words; it just does not reach for a
        // handler twice.
#ifndef _WIN32
        setenv("GGML_NO_BACKTRACE", "1", 1);
#else
        _putenv_s("GGML_NO_BACKTRACE", "1");
#endif
        std::call_once(gCardOnce, [] { gCard = probeNvml(); });
        std::vector<std::string> tries;
        for (const std::string &shape : machineOrder())
        {
          if (tryShape(shape, tries))
            break;
        }
        if (!gLoaded.stands)
        {
          std::string why;
          for (const std::string &tryWord : tries)
            why += (why.empty() ? "" : "; ") + tryWord;
          gLoaded.words = "no image engine library stands beside this server: " + why +
                          " - the doors that draw pictures will say so";
        }
        std::cout << gLoaded.words << std::endl;
      }

      // One number, alone in a file the kernel wrote.
      bool readNumberFile(const std::filesystem::path &path, long long &out)
      {
        std::ifstream in(path);
        if (!in)
          return false;
        long long value = -1;
        if (!(in >> value) || value < 0)
          return false;
        out = value;
        return true;
      }
    } // namespace

    void warmEngine() { std::call_once(gWarmOnce, warmNow); }

    bool engineStands() { return gLoaded.stands; }

    std::string engineShape() { return gLoaded.shape; }

    std::string engineWords() { return gLoaded.words; }

    const EngineApi &engineApi() { return gLoaded.api; }

    void cardFacts(long long &totalBytes, long long &freeBytes, int &computeMajor, int &computeMinor)
    {
      std::call_once(gCardOnce, [] { gCard = probeNvml(); });
      totalBytes = gCard.totalBytes;
      freeBytes = gCard.freeBytes;
      computeMajor = gCard.major;
      computeMinor = gCard.minor;
    }

    // What a card answers when there is no nvml to ask it. The driver that owns
    // the silicon keeps its own books: amdgpu writes the card's VRAM, whole and
    // in use, under that card's folder in /sys/class/drm, so a machine whose
    // engine library names a Vulkan card can still say what the card holds and
    // what stands free. A driver that states nothing is answered with nothing -
    // false, and no bytes invented.
    //
    // This is how the pages describe a card. It is deliberately not what the
    // working-set ceiling asks for, and that stays with nvml on purpose: the
    // nvml road is the one that pins weights into the card's own heap, and a
    // ceiling drawn from these records would refuse the very model the vulkan
    // engine carries from system memory - measured on the nuc, 6.9 GB of
    // weights standing resident on a 4 GB card.
    bool cardBytesFromDriver(long long &totalBytes, long long &freeBytes)
    {
      totalBytes = 0;
      freeBytes = 0;
#ifdef _WIN32
      return false;
#else
      std::error_code ec{};
      const std::filesystem::path drm("/sys/class/drm");
      if (!std::filesystem::is_directory(drm, ec))
        return false;
      long long bestTotal = 0;
      long long bestUsed = 0;
      for (const auto &entry : std::filesystem::directory_iterator(drm, ec))
      {
        if (ec)
          break;
        // the cards are "cardN"; "cardN-DP-1" and friends are its connectors
        const std::string name = entry.path().filename().string();
        if (name.compare(0, 4, "card") != 0)
          continue;
        size_t digits = 4;
        while (digits < name.size() && std::isdigit(static_cast<unsigned char>(name[digits])))
          ++digits;
        if (digits == 4 || digits != name.size())
          continue;
        long long total = 0;
        if (!readNumberFile(entry.path() / "device" / "mem_info_vram_total", total) || total <= 0)
          continue;
        long long used = 0;
        readNumberFile(entry.path() / "device" / "mem_info_vram_used", used);
        // where more than one card keeps such books, the one the pages call the
        // card is the one that can carry the most
        if (total > bestTotal)
        {
          bestTotal = total;
          bestUsed = used;
        }
      }
      if (bestTotal <= 0)
        return false;
      totalBytes = bestTotal;
      freeBytes = bestTotal > bestUsed ? bestTotal - bestUsed : 0;
      return true;
#endif
    }

    std::string engineCardWord()
    {
      std::call_once(gCardOnce, [] { gCard = probeNvml(); });
      if (gCard.major < 0)
        return std::string();
      return std::to_string(gCard.major) + std::to_string(gCard.minor);
    }

    void systemMemory(long long &totalBytes, long long &freeBytes)
    {
      totalBytes = 0;
      freeBytes = 0;
#ifdef _WIN32
      MEMORYSTATUSEX status{};
      status.dwLength = sizeof(status);
      if (GlobalMemoryStatusEx(&status))
      {
        totalBytes = static_cast<long long>(status.ullTotalPhys);
        freeBytes = static_cast<long long>(status.ullAvailPhys);
      }
#else
      // Two different things go by "free memory" here. The kernel's counter of
      // untouched pages can sit near zero on a box that has just copied a few
      // hundred gigabytes past them, because the page cache holds those pages
      // until someone asks - and it hands them right back. MemAvailable is the
      // kernel's own sum of what a process could still be given, cache and all,
      // so that is the number the pages show; sysconf answers only where the
      // file cannot be read. Measured on the nuc, 2026-11-27: mid-transfer the
      // untouched count said 0.5 GiB while MemAvailable said 29.9 GiB.
      long long totalKb = 0;
      long long availKb = 0;
      std::ifstream meminfo("/proc/meminfo");
      std::string line;
      while (std::getline(meminfo, line))
      {
        std::istringstream words(line);
        std::string key;
        long long kb = 0;
        if (!(words >> key >> kb))
          continue;
        if (key == "MemTotal:")
          totalKb = kb;
        else if (key == "MemAvailable:")
          availKb = kb;
        if (totalKb > 0 && availKb > 0)
          break;
      }
      if (totalKb > 0)
        totalBytes = totalKb * 1024LL;
      if (availKb > 0)
        freeBytes = availKb * 1024LL;
      if (totalBytes <= 0 || freeBytes <= 0)
      {
        const long pages = sysconf(_SC_PHYS_PAGES);
        const long freePages = sysconf(_SC_AVPHYS_PAGES);
        const long pageSize = sysconf(_SC_PAGE_SIZE);
        if (totalBytes <= 0 && pages > 0 && pageSize > 0)
          totalBytes = static_cast<long long>(pages) * pageSize;
        if (freeBytes <= 0 && freePages > 0 && pageSize > 0)
          freeBytes = static_cast<long long>(freePages) * pageSize;
      }
#endif
    }

  } // namespace imagegen
} // namespace ols
