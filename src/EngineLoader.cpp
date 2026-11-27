// The engine library beside the program, opened at start. See EngineLoader.h.
#include "EngineLoader.h"

#include "Deployment.h"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <mutex>
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
      // was compiled against. The windows compiler writes those instruction
      // sets straight into the code - no run-time dispatch stands behind them
      // like the linux engines have - so the avx512 engine may only be handed
      // to a chip that says yes to f, vbmi and vnni itself.
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
#endif
        order.push_back("cpu");
        return order;
      }

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
      const long pages = sysconf(_SC_PHYS_PAGES);
      const long freePages = sysconf(_SC_AVPHYS_PAGES);
      const long pageSize = sysconf(_SC_PAGE_SIZE);
      if (pages > 0 && pageSize > 0)
        totalBytes = static_cast<long long>(pages) * pageSize;
      if (freePages > 0 && pageSize > 0)
        freeBytes = static_cast<long long>(freePages) * pageSize;
#endif
    }

  } // namespace imagegen
} // namespace ols
