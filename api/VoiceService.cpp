// Voice service: the model table, the voice library, the job records, and the
// kept renders. The voice engine is audio.cpp linked in as its C API - there
// is no engine process, port, or config file: models answer through handles in
// this process. Everything a page claims about a model that stands loaded is
// read off the model's own handle - its supported tasks, its languages, and
// every option its family declares - rather than repeated here from memory. A
// model stands only while its own card asked for it: nothing here opens a model
// at boot, at first ask, or on the way through a job.
#include "VoiceService.h"
#include "Common.h"
#include "Platform.h"

#include "audiocpp.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <nlohmann/json.hpp>

namespace ols
{
  namespace voice
  {

    using json = nlohmann::json;

    // Where in a sound a sample sits, in seconds: a rate of nought would
    // otherwise divide by nothing.
    static double sampleSeconds(long long sample, int rate)
    {
      return static_cast<double>(sample) / static_cast<double>(rate > 0 ? rate : 1);
    }

    // Four characters at a place in a file's bytes: the chunks of a wav are
    // named by four characters, and this is how the ones that matter are found.
    static bool chunkNamedAt(const std::vector<char> &bytes, size_t at, const char *name)
    {
      for (size_t i = 0; i < 4; ++i)
      {
        if (at + i >= bytes.size() || bytes[at + i] != name[i])
          return false;
      }
      return true;
    }

    // A whole number stored lowest byte first: wav chunk lengths are stored this
    // way.
    static unsigned long long littleEndianAt(const std::vector<char> &bytes, size_t at)
    {
      return static_cast<unsigned char>(bytes[at]) |
             (static_cast<unsigned long long>(static_cast<unsigned char>(bytes[at + 1])) << 8) |
             (static_cast<unsigned long long>(static_cast<unsigned char>(bytes[at + 2])) << 16) |
             (static_cast<unsigned long long>(static_cast<unsigned char>(bytes[at + 3])) << 24);
    }

    // A render's name is lower-case letters, digits and dashes, and no longer
    // than eighty of them: the names this machine makes follow that shape, so a
    // name outside it cannot stand for one of its own files.
    static bool renderNameOk(const std::string &name)
    {
      if (name.empty() || name.size() > 80)
        return false;
      for (const char c : name)
      {
        const bool letterOrDigit =
            std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c));
        if (!letterOrDigit && c != '-')
          return false;
      }
      return true;
    }

    // The words a card gives when nothing stands behind it, and when a run's
    // controls came in the wrong shape: both are said the same way everywhere.
    static const char kNoMemoryWords[] =
        "that model holds no memory: its card's load hand is what puts its weights in";
    static const char kRunControlWords[] =
        "the run controls of a standing model have to be an object of names and values";

    // A family's spec sometimes states how many tongues it covers as free text
    // ("50+ languages", "zh dialects", "model dependent"). Those words are not
    // the name of a language, so a page never counts them as one.
    static bool languageWordOk(const std::string &word)
    {
      if (word.empty())
        return false;
      std::string lower;
      lower.reserve(word.size());
      for (const unsigned char ch : word)
      {
        if (ch == ' ' || ch == '+' || (ch >= '0' && ch <= '9'))
          return false;
        lower.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + 32) : static_cast<char>(ch));
      }
      static const char *fillers[] = {"language_agnostic", "model_dependent", "nospeech",
                                      "multilingual", "polyglot", "unknown", "other", "language", "languages"};
      for (const char *filler : fillers)
        if (lower == filler)
          return false;
      return true;
    }

    // Every model this machine can offer comes from the engine's own model
    // specs - the same files the engine reads to know its families, packages,
    // tasks, languages, options, and voice presets - kept only for the families
    // this build actually registered. Nothing here names a model by hand: a
    // model the engine knows is a model the app offers, with the exact weight
    // layout its spec asks for. The cards are built once, on the first ask.
    struct VoiceModelDef
    {
      // the package's own id
      std::string id;
      // the package's own display name
      std::string name;
      // the family's own description
      std::string description;
      // the engine's family name
      std::string family;
      // what the family's own spec calls it
      std::string familyName;
      // the weights, under this model's weights directory
      std::string file;
      // raw json for the family's load options, or empty
      std::string loadOptions;
      // the tasks this build accepts, in the C API's words
      std::vector<std::string> tasks;
      // what the family says it does
      std::vector<std::string> capabilities;
      // packaged voice ids the spec documents
      std::vector<std::string> presetVoices;
      // every file the package names, under the weights room
      std::vector<std::string> files;
      // where the package's own files come from
      std::string repo;
      std::vector<std::string> declaredLanguages; // the languages the family's spec names
    };

    static std::vector<VoiceModelDef> gCardList;

    // What the specs name and this build cannot run: the engine's registry names
    // exactly the families its loaders were compiled with, so a family outside
    // that list is never offered as a model that cannot run. It is counted here,
    // with the packages it names, so the browser can state the whole set it read
    // and the part this machine leaves out - a table that showed only what runs
    // would read as a smaller collection than the one it was made from.
    static std::vector<std::string> gNoLoaderFamilies;
    static size_t gNoLoaderPackages = 0;

    // The models answer, kept between calls: what it states changes only when a
    // model stands up, is put down, or a package's files arrive, so a page that
    // re-reads the table does not ask for every weight file on disk to be
    // looked up again. Anything that changes that truth marks it stale.
    static std::string gModelsAnswer;
    // The same table as the answer is built from, kept beside it in its parsed
    // shape: a page of cards is drawn out of that, so a read of one page is a
    // filter over rows already in memory rather than a JSON reading of the whole
    // four hundred on every poll.
    static json gModelsTable = json::array();
    static bool gModelsAnswerFresh = false;
    // The table's two shapes - the words a page is handed and the rows those words
    // were built from - are stood or taken away together, and read while they stand
    // together: a table read halfway through its own rewrite is not a table.
    static std::mutex gModelsAnswerMutex;
    // The house table is the same truth read across the families, so it is kept
    // and marked stale beside the table it is counted from.
    static std::string gHouseAnswer;
    static bool gHouseAnswerFresh = false;
    static std::string gHfToken;

    // ---- the engine's own handles, kept alive for the life of the process ----

    static audiocpp_registry *gRegistry = nullptr;
    static std::map<std::string, audiocpp_model *> gModels;
    static std::map<std::string, audiocpp_session *> gSessions;
    // A session is named by the model and by the kind of work it runs at: the
    // engine ties a session to one task, and a model declares several. Model
    // names are letters, digits and dash, so the separator cannot be part of one.
    static constexpr char sessionKeySplit = ':';
    static std::map<std::string, std::string> gLoadErrors;
    static std::mutex gEngineMutex;

    static std::string gWeightsDir = "weights/voice";
    static std::string gDataDir = "data/voice";
    static int32_t gThreads = 4;

    // The last failure's detail, read right after a call refused: the C API
    // keeps it per thread and clears it on the next success.
    static std::string lastEngineError()
    {
      const char *text = audiocpp_last_error();
      return text ? std::string(text) : std::string();
    }
    // The kinds a door asked for, in the words a record can carry. When the engine
    // kept no words for itself, these are the honest detail: a model that names none
    // of them, or a build that has no loader for them, is what a reader needs to hear.
    static std::string wantedWords(const std::vector<std::string> &wanted)
    {
      std::string words;
      for (const auto &want : wanted)
      {
        if (!words.empty())
          words += ", ";
        words += want;
      }
      return words;
    }

    static std::string absolutePath(const std::string &relative)
    {
      std::filesystem::path full(relative);
      if (!full.is_absolute())
      {
        std::error_code ec;
        const auto base = std::filesystem::current_path(ec);
        if (!ec)
          full = base / full;
      }
      std::string text = full.string();
      for (char &c : text)
        if (c == '\\')
          c = '/';
      return text;
    }

    // Where a package's files are read: in the weights room, except for a name
    // marked with '@', which names the engine's own bundled model tree beside
    // this build - those small models ship with the engine and are never
    // downloaded, so they live where the engine was built, not in the room a
    // person fills.
    static std::string packagePath(const std::string &name)
    {
      if (!name.empty() && name[0] == '@')
        return absolutePath(name.substr(1));
      return absolutePath(gWeightsDir + "/" + name);
    }

    // The weights room, read once: the model table asks what every package's
    // files weigh, and asking the machine about a path on its own - resolving
    // the directory it sits in each time - costs far more than walking the
    // room once and reading from that walk. The walk is redone when something
    // arrives or leaves the room, which is the only thing that changes it.
    static std::map<std::string, unsigned long long> gWeightSizes;
    static bool gWeightWalkDone = false;

    static void walkWeightsRoom()
    {
      gWeightSizes.clear();
      gWeightWalkDone = true;
      std::error_code ec;
      const std::filesystem::path root = absolutePath(gWeightsDir);
      if (!std::filesystem::is_directory(root, ec))
        return;
      // The room is shallow (a family, a package, its files), so the walk is
      // stopped rather than trusted to end.
      std::vector<std::pair<std::filesystem::path, int>> queue;
      queue.push_back({root, 0});
      while (!queue.empty())
      {
        const std::pair<std::filesystem::path, int> here = queue.back();
        queue.pop_back();
        if (here.second > 4)
          continue;
        std::error_code step;
        std::filesystem::directory_iterator it{here.first, step};
        if (step)
          continue;
        for (const std::filesystem::directory_entry &entry : it)
        {
          const std::filesystem::path &name = entry.path();
          if (std::filesystem::is_directory(name, step))
          {
            queue.push_back({name, here.second + 1});
            continue;
          }
          const unsigned long long size = entry.file_size(step);
          if (!step)
            gWeightSizes[absolutePath(name.string())] = size;
        }
      }
    }

    static bool weightOnDisk(const std::string &absolute)
    {
      if (!gWeightWalkDone)
        walkWeightsRoom();
      if (gWeightSizes.find(absolute) != gWeightSizes.end())
        return true;
      std::error_code ec;
      return std::filesystem::exists(absolute, ec);
    }

    static unsigned long long weightBytes(const std::string &absolute)
    {
      if (!gWeightWalkDone)
        walkWeightsRoom();
      const auto found = gWeightSizes.find(absolute);
      if (found != gWeightSizes.end())
        return found->second;
      std::error_code ec;
      const auto size = std::filesystem::file_size(absolute, ec);
      return ec ? 0ull : static_cast<unsigned long long>(size);
    }

    // Ask the engine for its models: the registry first (NULL config: the
    // default loader set), then the cards built from the engine's own specs,
    // then each model whose files are on disk. A model that refuses to load
    // records the engine's own words for why, beside the row; the others still
    // stand up.
    static void buildCards(); // reads the specs: defined with the file helpers below

    // Run controls in the engine's own shape: every name beside its value, as the
    // engine spells a value. Nothing here checks which names a model accepts -
    // the engine states that for the model it is handed, and answers in its own
    // words when a name does not belong.
    static audiocpp_options *makeOptions(const json &values)
    {
      if (!values.is_object() || values.empty())
        return nullptr;
      audiocpp_options *options = audiocpp_options_create();
      if (!options)
        return nullptr;
      for (auto it = values.begin(); it != values.end(); ++it)
        audiocpp_options_set(options, it.key().c_str(), it->dump().c_str());
      return options;
    }

    // One card, loaded: the weights its own spec names are read into the engine,
    // with the load options that spec declared. The caller holds the engine
    // lock and has a registry; a card with no files on disk is skipped quietly.
    static bool loadCard(const VoiceModelDef &def)
    {
      const std::string id = def.id;
      if (gModels.count(id))
        return true;
      const std::string weight = packagePath(def.file);
      if (!fileExists(weight))
        return false; // no files: nothing to load, and nothing failed
      audiocpp_model_config config = {def.family.c_str(), nullptr, nullptr, nullptr};
      audiocpp_options *options = nullptr;
      if (!def.loadOptions.empty())
      {
        json parsed = json::parse(def.loadOptions, nullptr, false);
        if (!parsed.is_discarded())
          options = makeOptions(parsed);
      }
      audiocpp_model *model = nullptr;
      const audiocpp_status status =
          audiocpp_model_load(gRegistry, absolutePath(weight).c_str(), &config, options, &model);
      if (options)
        audiocpp_options_free(options);
      if (status == AUDIOCPP_OK && model)
      {
        gModels[id] = model;
        return true;
      }
      gLoadErrors[id] = lastEngineError();
      return false;
    }

    // One card, put down: its sessions are closed before the model, then the
    // model's memory is given back to the engine. A name the engine does not
    // hold answers false, and changes nothing.
    static bool unloadCard(const std::string &id)
    {
      const auto model = gModels.find(id);
      if (model == gModels.end())
        return false;
      for (auto session = gSessions.begin(); session != gSessions.end();)
      {
        if (session->first.rfind(id + sessionKeySplit, 0) != 0)
        {
          ++session;
          continue;
        }
        if (session->second)
          audiocpp_session_free(session->second);
        session = gSessions.erase(session);
      }
      for (auto refusal = gLoadErrors.begin(); refusal != gLoadErrors.end();)
      {
        if (refusal->first != id && refusal->first.rfind(id + sessionKeySplit, 0) != 0)
        {
          ++refusal;
          continue;
        }
        refusal = gLoadErrors.erase(refusal);
      }
      audiocpp_model_free(model->second);
      gModels.erase(model);
      gLoadErrors.erase(id);
      return true;
    }

    // The engine, stood up to the point its cards can be read. Asked once per
    // run: an engine that would not stand is not asked again, and its own words
    // for why stay in the error notes. The caller holds the engine lock.
    static bool ensureRegistryLocked()
    {
      static bool asked = false;
      if (asked)
        return gRegistry != nullptr;
      asked = true;
      if (!gRegistry)
      {
        if (audiocpp_registry_create(nullptr, &gRegistry) != AUDIOCPP_OK)
        {
          gRegistry = nullptr;
          gLoadErrors["(registry)"] = lastEngineError();
          return false;
        }
        buildCards();
      }
      return true;
    }

    // The same, for a caller that holds no lock.
    static bool ensureRegistry()
    {
      std::lock_guard<std::mutex> lock(gEngineMutex);
      return ensureRegistryLocked();
    }

    // A session belongs to one kind of work: the engine is handed a task when a
    // session is made. A model declares several kinds, so one session per model
    // would be the wrong one for whichever door asked for a different kind - a
    // model that both makes a voice and hears one could only ever answer as the
    // task its own card happened to name first. The door that speaks asks for a
    // speaking task and the door that listens for a listening one, each taking
    // the first task of that kind the model's card declares; the engine's
    // capability question is asked before a session is made, so a task this
    // build has no loader for is never asked for, and says so. Sessions are
    // kept per model per task, so a model asked again for the same kind of work
    // does not build its weights and graphs again. The engine is asked for the
    // GPU first; a family that has no GPU implementation of this task says so
    // itself, and the session is made again on the CPU - the decision comes from
    // the engine's refusal, never from a guess here.
    static audiocpp_session *sessionFor(const std::string &id,
                                        const std::vector<std::string> &wanted,
                                        const json &sessionValues)
    {
      std::lock_guard<std::mutex> lock(gEngineMutex);
      const auto model = gModels.find(id);
      if (model == gModels.end())
        return nullptr;
      const VoiceModelDef *def = nullptr;
      for (const VoiceModelDef &entry : gCardList)
        if (id == entry.id)
          def = &entry;
      if (!def)
        return nullptr;
      // The controls this session is stood with, in one stable name. A session is
      // kept per model per kind of work already; settings belong to a session too,
      // so the same settings find the session that already runs with them and a
      // changed control stands a new one up rather than answering with the wrong
      // settings. Nothing here checks the names: the engine states which belong to
      // this model and answers in its own words when one does not.
      std::string optionWords;
      if (sessionValues.is_object())
      {
        for (auto it = sessionValues.begin(); it != sessionValues.end(); ++it)
        {
          if (!optionWords.empty())
            optionWords += sessionKeySplit;
          optionWords += it.key() + "=" + (it->is_string() ? it->get<std::string>() : it->dump());
        }
      }
      audiocpp_backend_config gpu = {"vulkan", 0, static_cast<int>(gThreads)};
      audiocpp_backend_config cpu = {"cpu", 0, static_cast<int>(gThreads)};
      for (const auto &want : wanted)
      {
        bool declared = false;
        for (const auto &have : def->tasks)
        {
          if (have != want)
            continue;
          declared = true;
          break;
        }
        if (!declared)
          continue;
        const std::string key = id + sessionKeySplit + want +
                                (optionWords.empty() ? std::string() : std::string(1, sessionKeySplit) + optionWords);
        const auto found = gSessions.find(key);
        if (found != gSessions.end())
          return found->second;
        if (!audiocpp_model_supports(model->second, want.c_str(), "offline"))
        {
          gLoadErrors[key] = "this build has no loader that runs " + want + " for this model";
          continue;
        }
        audiocpp_session *session = nullptr;
        audiocpp_options *options = makeOptions(sessionValues);
        if (audiocpp_session_create(model->second, want.c_str(), "offline", &gpu, options, &session) ==
            AUDIOCPP_OK)
        {
          if (options)
            audiocpp_options_free(options);
          gSessions[key] = session;
          return session;
        }
        gLoadErrors[key + " (gpu)"] = lastEngineError();
        session = nullptr;
        if (audiocpp_session_create(model->second, want.c_str(), "offline", &cpu, options, &session) ==
            AUDIOCPP_OK)
        {
          if (options)
            audiocpp_options_free(options);
          gSessions[key] = session;
          return session;
        }
        if (options)
          audiocpp_options_free(options);
        gLoadErrors[key + " (cpu)"] = lastEngineError();
      }
      return nullptr;
    }

    // A wav standing on disk, read as interleaved float PCM: what the voice
    // library keeps are the engine's own shape - short clear recordings - so
    // 16-bit PCM is all that is read; anything else is said so, not guessed at.
    static std::vector<float> readWavPcm(const std::string &path, size_t &frames,
                                         int &rate, int &channels)
    {
      std::ifstream in(path, std::ios::binary);
      if (!in.good())
        return {};
      const std::string all = [&in]()
      {
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
      }();
      if (all.size() < 12 || all.compare(0, 4, "RIFF") != 0 || all.compare(8, 4, "WAVE") != 0)
        return {};
      size_t fmt = 0, dataAt = 0, dataBytes = 0;
      uint16_t width = 0, count = 0;
      int sampleRate = 0;
      for (size_t at = 12; at + 8 <= all.size();)
      {
        const std::string tag = all.substr(at, 4);
        const unsigned char *sz = reinterpret_cast<const unsigned char *>(all.data() + at + 4);
        const uint32_t len = static_cast<uint32_t>(sz[0]) | (static_cast<uint32_t>(sz[1]) << 8) |
                             (static_cast<uint32_t>(sz[2]) << 16) |
                             (static_cast<uint32_t>(sz[3]) << 24);
        const size_t body = at + 8;
        if (body + len > all.size())
          break; // a truncated tail: the header was taken, the file is short
        if (tag == "fmt ")
        {
          if (len >= 16)
          {
            const unsigned char *f = reinterpret_cast<const unsigned char *>(all.data() + body);
            const uint16_t format = static_cast<uint16_t>(f[0] | (f[1] << 8));
            if (format != 1)
              return {}; // only plain uncompressed PCM is read back
            // The fields a fmt chunk holds, in the order the wave format fixes
            // them: 0 the sample format, 2 how many channels, 4 the sample
            // rate, 8 the bytes every frame takes at once, 12 how many frames
            // share a block, 14 the bits in one sample (a longer fmt chunk
            // carries more, and nothing read here is past that).
            count = static_cast<uint16_t>(f[2] | (f[3] << 8));
            sampleRate = static_cast<int>(f[4] | (f[5] << 8) | (f[6] << 16) | (f[7] << 24));
            width = static_cast<uint16_t>(f[14] | (f[15] << 8));
          }
        }
        else if (tag == "data")
        {
          dataAt = body;
          dataBytes = len;
        }
        at = body + ((len + 1) & ~static_cast<uint32_t>(1)); // chunks pad to even
      }
      (void)fmt;
      if (!dataBytes || !count || !width || !sampleRate)
        return {};
      if (width != 16)
        return {}; // 16 bits per sample is what this server writes, and all it reads
      const size_t total = dataBytes / 2;
      const std::vector<int16_t> pcm(reinterpret_cast<const int16_t *>(all.data() + dataAt),
                                     reinterpret_cast<const int16_t *>(all.data() + dataAt) + total);
      channels = static_cast<int>(count);
      rate = sampleRate;
      if (rate <= 0)
        return {};
      frames = total / static_cast<size_t>(channels);
      std::vector<float> out(total);
      for (size_t at = 0; at < total; ++at)
        out[at] = static_cast<float>(pcm[at]) / 32768.0f;
      return out;
    }

    // How long a sound runs, from the words its own header states: the bytes of
    // its samples, the channels, and the samples a second. The sound itself is
    // not read for that: a room that asks this for every file it holds was paying
    // for the bytes of every sound, and for a float built for every sample, to
    // reach a fact its headers state before a single sample.
    static double wavSeconds(const std::string &path)
    {
      std::error_code sizeEc;
      const auto fileSizeValue = std::filesystem::file_size(path, sizeEc);
      if (sizeEc || fileSizeValue < 12)
        return 0.0;
      const size_t fileSize = static_cast<size_t>(fileSizeValue);
      std::ifstream in(path, std::ios::binary);
      if (!in.good())
        return 0.0;
      // The head is where the chunks are named; the sound beyond them stays
      // unread, and the file's real length is what says whether a declared
      // sound is really there rather than standing in for bytes that are not.
      const size_t headBytes = (fileSize < 8192u) ? fileSize : 8192u;
      std::vector<char> buffer(headBytes);
      in.read(buffer.data(), static_cast<std::streamsize>(headBytes));
      if (static_cast<size_t>(in.gcount()) < headBytes)
        return 0.0;
      if (!chunkNamedAt(buffer, 0, "RIFF") || !chunkNamedAt(buffer, 8, "WAVE"))
        return 0.0;
      size_t dataAt = 0, dataBytes = 0;
      uint16_t count = 0, width = 0;
      int sampleRate = 0;
      for (size_t at = 12; at + 8 <= buffer.size();)
      {
        const uint32_t len = static_cast<uint32_t>(littleEndianAt(buffer, at + 4));
        const size_t body = at + 8;
        const bool isFormat = chunkNamedAt(buffer, at, "fmt ");
        const bool isData = chunkNamedAt(buffer, at, "data");
        if (!isData && body + len > buffer.size())
          break;
        if (isFormat && len >= 16)
        {
          const unsigned char *f = reinterpret_cast<const unsigned char *>(buffer.data() + body);
          if (f[0] != 1 || f[1] != 0)
            return 0.0;
          count = static_cast<uint16_t>(f[2] | (f[3] << 8));
          sampleRate = static_cast<int>(f[4] | (f[5] << 8) | (f[6] << 16) | (f[7] << 24));
          width = static_cast<uint16_t>(f[14] | (f[15] << 8));
        }
        else if (isData)
        {
          dataAt = body;
          dataBytes = len;
        }
        at = body + ((len + 1) & ~static_cast<uint32_t>(1));
      }
      if (!dataBytes || !count || !width || !sampleRate)
        return 0.0;
      if (width != 16)
        return 0.0;
      if (dataAt + dataBytes > fileSize)
        return 0.0;
      const size_t frames = (dataBytes / 2) / static_cast<size_t>(count);
      return static_cast<double>(frames) / static_cast<double>(sampleRate);
    }

    // Where the bytes of a recording a request carries inline are kept for the
    // moment the engine reads them from a file. The name belongs to the job: two
    // recordings at once - a clone while a listening runs - would otherwise read,
    // or remove, each other's copy, and a speaker could be answered with the wrong
    // recording rather than the one that was asked for.
    static std::string inlineAudioPath(const std::string &jobId, const std::string &kind)
    {
      return gDataDir + "/." + kind + "-tmp-" + jobId + ".wav";
    }

    // The engine's answer is interleaved float PCM; the file on disk is the
    // same shape the library reads back: 16-bit PCM wav.
    static bool writeWavPcm(const std::string &path, const float *samples, size_t frames,
                            int rate, int channels)
    {
      if (!frames || rate <= 0 || channels <= 0)
        return false;
      const size_t total = frames * static_cast<size_t>(channels);
      std::vector<int16_t> pcm(total);
      for (size_t at = 0; at < total; ++at)
      {
        float value = samples[at];
        if (value > 1.0f)
          value = 1.0f;
        if (value < -1.0f)
          value = -1.0f;
        pcm[at] = static_cast<int16_t>(value * 32767.0f);
      }
      const size_t dataBytes = pcm.size() * 2;
      const int byteRate = rate * channels * 2;
      auto put32 = [](std::vector<char> &out, uint32_t value)
      {
        out.push_back(static_cast<char>(value & 0xFF));
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
        out.push_back(static_cast<char>((value >> 16) & 0xFF));
        out.push_back(static_cast<char>((value >> 24) & 0xFF));
      };
      auto put16 = [](std::vector<char> &out, uint16_t value)
      {
        out.push_back(static_cast<char>(value & 0xFF));
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
      };
      std::vector<char> file;
      const std::string text = "RIFF";
      file.insert(file.end(), text.begin(), text.end());
      put32(file, static_cast<uint32_t>(36 + dataBytes));
      const std::string wave = "WAVE";
      file.insert(file.end(), wave.begin(), wave.end());
      const std::string fmt = "fmt ";
      file.insert(file.end(), fmt.begin(), fmt.end());
      put32(file, 16);
      put16(file, 1); // uncompressed PCM
      put16(file, static_cast<uint16_t>(channels));
      put32(file, static_cast<uint32_t>(rate));
      put32(file, static_cast<uint32_t>(byteRate));
      put16(file, static_cast<uint16_t>(channels * 2));
      put16(file, 16);
      const std::string data = "data";
      file.insert(file.end(), data.begin(), data.end());
      put32(file, static_cast<uint32_t>(dataBytes));
      const unsigned char *raw = reinterpret_cast<const unsigned char *>(pcm.data());
      file.insert(file.end(), raw, raw + dataBytes);
      std::ofstream out(path, std::ios::binary);
      if (!out.good())
        return false;
      out.write(file.data(), file.size());
      return out.good();
    }

    // base64 in, bytes out: the page sends recordings whole, so this is a
    // plain pass over the alphabet rather than a streaming tool.
    static std::string fromBase64(const std::string &b64)
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
      std::string out;
      uint32_t bits = 0;
      int taken = 0;
      for (const char c : b64)
      {
        const int value = sextet(c);
        if (value < 0)
          continue;
        bits = (bits << 6) | static_cast<uint32_t>(value);
        taken += 6;
        if (taken >= 24)
        {
          out.push_back(static_cast<char>((bits >> 16) & 0xFF));
          out.push_back(static_cast<char>((bits >> 8) & 0xFF));
          out.push_back(static_cast<char>(bits & 0xFF));
          bits = 0;
          taken = 0;
        }
      }
      if (taken == 12)
        out.push_back(static_cast<char>((bits >> 4) & 0xFF));
      else if (taken == 18)
      {
        out.push_back(static_cast<char>((bits >> 10) & 0xFF));
        out.push_back(static_cast<char>((bits >> 2) & 0xFF));
      }
      return out;
    }

    static bool isVoiceName(const std::string &name)
    {
      if (name.empty() || name.size() > 64 || name[0] == '.')
        return false;
      for (const char c : name)
      {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
        if (!ok)
          return false;
      }
      return true;
    }

    static std::string voiceDir() { return gDataDir + "/voices"; }
    static std::string renderDir() { return gDataDir + "/renders"; }

    // A name that came from an engine, made into a file name: only letters,
    // digits, dots, dashes and underscores survive, so a stream the engine named
    // can be kept and served without inventing a name for it.
    static std::string safeFileName(const std::string &name)
    {
      std::string out;
      for (const char ch : name)
      {
        const bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
        if (keep)
          out.push_back(ch);
        else
          out.push_back('_');
      }
      return out.empty() ? std::string("stream") : out;
    }

    // What kind of thing the engine answered with, in the words the engine
    // itself uses, and the file name its payload is kept under. The payload
    // bytes are the engine's own: they are written out unchanged, so a person
    // can take the file and read it wherever the format belongs.
    static std::string artifactKindName(const audiocpp_artifact_kind kind)
    {
      switch (kind)
      {
      case AUDIOCPP_ARTIFACT_SPEAKER_EMBEDDING:
        return "speaker_embedding";
      case AUDIOCPP_ARTIFACT_STYLE_EMBEDDING:
        return "style_embedding";
      case AUDIOCPP_ARTIFACT_PROMPT_EMBEDDING:
        return "prompt_embedding";
      case AUDIOCPP_ARTIFACT_ACOUSTIC_TOKENS:
        return "acoustic_tokens";
      case AUDIOCPP_ARTIFACT_MIDI:
        return "midi";
      case AUDIOCPP_ARTIFACT_TRANSCRIPT_ALIGNMENT:
        return "transcript_alignment";
      case AUDIOCPP_ARTIFACT_DIARIZATION_STATE:
        return "diarization_state";
      case AUDIOCPP_ARTIFACT_VAD_STATE:
        return "vad_state";
      default:
        return "custom";
      }
    }

    static const char *artifactExtension(const audiocpp_artifact_kind kind)
    {
      return kind == AUDIOCPP_ARTIFACT_MIDI ? ".mid" : ".bin";
    }

    // The transcript that came with a saved voice, beside its recording under
    // the same name: models that clone by listening need the words of what the
    // recording says. A voice without one still clones where the model works
    // from the speaker embedding alone, so the sidecar is kept, not demanded.
    static std::string transcriptName(const std::string &name)
    {
      return voiceDir() + "/" + name + ".txt";
    }

    static std::string readFileText(const std::string &path)
    {
      std::ifstream in(path, std::ios::binary);
      if (!in.good())
        return std::string();
      std::ostringstream ss;
      ss << in.rdbuf();
      return ss.str();
    }

    static bool writeFileBytes(const std::string &path, const std::string &bytes)
    {
      const std::string parent = path.substr(0, path.find_last_of("/\\"));
      std::error_code ec;
      if (!parent.empty())
        std::filesystem::create_directories(parent, ec);
      std::ofstream out(path, std::ios::binary);
      if (!out.good())
        return false;
      out << bytes;
      return out.good();
    }

    // The engine's own model specs: the same files the engine reads to know its
    // families, packages, tasks, options, and voices. Asking a child that is not
    // there answers with an empty object, never with a thrown key.
    static const json &specChild(const json &at, const char *key)
    {
      static const json empty = json::object();
      if (!at.is_object())
        return empty;
      const auto found = at.find(key);
      return found == at.end() ? empty : found.value();
    }

    static std::string specText(const json &at, const char *key)
    {
      const json &value = specChild(at, key);
      return value.is_string() ? value.get<std::string>() : std::string();
    }

    static std::vector<std::string> specArray(const json &at, const char *key)
    {
      std::vector<std::string> out;
      const json &value = specChild(at, key);
      if (!value.is_array())
        return out;
      for (const auto &item : value)
        if (item.is_string())
          out.push_back(item.get<std::string>());
      return out;
    }

    // A family this build can run: the registry the engine made names exactly
    // the families its loaders were compiled with.
    static bool familyRegistered(const std::string &family)
    {
      if (!gRegistry)
        return false;
      for (size_t at = 0; at < audiocpp_registry_family_count(gRegistry); ++at)
      {
        const char *known = nullptr;
        if (audiocpp_registry_family(gRegistry, at, &known) == AUDIOCPP_OK && known &&
            family == known)
          return true;
      }
      return false;
    }

    // The engine's own model specs, in the vendored tree this build came from.
    static std::string specsDir() { return "vendor/audio.cpp/model_specs"; }

    static void buildCards()
    {
      gCardList.clear();
      gNoLoaderFamilies.clear();
      gNoLoaderPackages = 0;
      std::error_code ec;
      if (!std::filesystem::is_directory(specsDir(), ec))
        return;
      std::vector<std::string> specFiles;
      for (const auto &item : std::filesystem::directory_iterator(specsDir(), ec))
      {
        if (item.path().extension().string() == ".json")
          specFiles.push_back(item.path().string());
      }
      std::sort(specFiles.begin(), specFiles.end());
      for (const auto &specFile : specFiles)
      {
        const json spec = json::parse(readFileText(specFile), nullptr, false);
        if (!spec.is_object())
          continue;
        const std::string family = specText(spec, "family");
        if (family.empty())
          continue; // a spec that names no family belongs to no family
        if (!familyRegistered(family))
        {
          // Not offered: this build has no loader that runs this family, and a
          // card would read as a model that can be asked. Counted, with the
          // packages it names, so the table states the set it was made from.
          if (std::find(gNoLoaderFamilies.begin(), gNoLoaderFamilies.end(), family) ==
              gNoLoaderFamilies.end())
            gNoLoaderFamilies.push_back(family);
          const json skipped = specChild(spec, "packages");
          if (skipped.is_array())
            gNoLoaderPackages += skipped.size();
          continue;
        }
        const std::string description = specText(spec, "description");
        const std::string familyName = specText(spec, "display_name");
        std::vector<std::string> tasks;
        for (const auto &want : specArray(spec, "tasks"))
        {
          const char *token = audiocpp_task_from_spec_name(want.c_str());
          tasks.push_back(token ? std::string(token) : want);
        }
        const json ui = specChild(spec, "ui");
        std::vector<std::string> presets = specArray(ui, "builtin_voices");
        // The languages the family's own spec names. A loaded model states its
        // languages from its handle; a package that never stood still says
        // plainly what its spec asks for, and a page may filter by it.
        std::vector<std::string> declaredLanguages = specArray(spec, "languages");
        json wanted = json::object();
        const json options = specChild(spec, "options");
        const json loadOptions = specChild(options, "load");
        if (loadOptions.is_array())
        {
          for (const auto &option : loadOptions)
          {
            if (!option.is_object() || !option.contains("name") ||
                !option.at("name").is_string() || !option.contains("default"))
              continue;
            const json &at = option.at("default");
            if (at.is_string() || at.is_number() || at.is_boolean())
              wanted[option.at("name").get<std::string>()] = at;
          }
        }
        const std::string loadJson = wanted.empty() ? std::string() : wanted.dump();
        std::vector<std::string> capabilities;
        const json declaredCaps = specChild(spec, "capabilities");
        if (declaredCaps.is_object())
          for (auto it = declaredCaps.begin(); it != declaredCaps.end(); ++it)
            capabilities.push_back(it.key());
        const json packages = specChild(spec, "packages");
        if (!packages.is_array())
          continue;
        for (const auto &package : packages)
        {
          if (!package.is_object())
            continue;
          const std::string format = specText(package, "format");
          if (format != "gguf" && format != "safetensors")
            continue; // a package this box cannot load (an NPU pair, say) is not offered
          const std::string packageId = specText(package, "id");
          const std::string target = specText(package, "target_directory");
          const std::string strip = specText(package, "strip_prefix");
          const json files = specChild(package, "files");
          std::string repo = specText(specChild(package, "download"), "repo");
          if (repo.empty())
            repo = specText(specChild(specChild(spec, "package_defaults"), "download"), "repo");
          if (packageId.empty() || target.empty() || !files.is_array())
            continue;
          std::vector<std::string> namedFiles;
          std::string weightsFile;
          for (const auto &file : files)
          {
            if (!file.is_string())
              continue;
            std::string named = file.get<std::string>();
            if (!strip.empty() && named.rfind(strip + "/", 0) == 0)
              named = named.substr(strip.size() + 1);
            if (named.empty())
              continue;
            namedFiles.push_back(target + "/" + named);
            const size_t dot = named.find_last_of('.');
            const std::string ext = dot == std::string::npos ? std::string() : named.substr(dot);
            if (weightsFile.empty() && (ext == ".gguf" || ext == ".safetensors"))
              weightsFile = target + "/" + named;
          }
          if (weightsFile.empty())
            continue;
          VoiceModelDef def;
          def.id = packageId;
          const std::string packageName = specText(package, "display_name");
          def.name = packageName.empty() ? specText(spec, "display_name") : packageName;
          def.description = description;
          def.family = family;
          def.familyName = familyName;
          def.file = weightsFile;
          def.files = namedFiles;
          def.loadOptions = loadJson;
          def.tasks = tasks;
          def.capabilities = capabilities;
          def.presetVoices = presets;
          def.declaredLanguages = declaredLanguages;
          def.repo = repo;
          gCardList.push_back(def);
        }
      }
      // The engine's own bundled models: small assets that ship inside the tree
      // the engine was built in and that no spec names, so a table built from
      // specs alone leaves them out - and a family with no card is a task no
      // page can reach. They are offered at the path the engine keeps them at,
      // marked with '@' so the weights room is not searched for something a
      // person was never meant to download.
      {
        const std::string bundledRoot = "vendor/audio.cpp/assets/framework/models";
        std::error_code bundledEc;
        if (std::filesystem::is_directory(bundledRoot, bundledEc))
        {
          std::vector<std::string> bundledNames;
          for (const auto &item : std::filesystem::directory_iterator(bundledRoot, bundledEc))
          {
            std::error_code step;
            if (!std::filesystem::is_directory(item.path(), step))
              continue;
            const std::string name = item.path().filename().string();
            if (!name.empty())
              bundledNames.push_back(name);
          }
          std::sort(bundledNames.begin(), bundledNames.end());
          for (const std::string &family : bundledNames)
          {
            if (!familyRegistered(family))
              continue;
            bool carded = false;
            for (const VoiceModelDef &entry : gCardList)
            {
              if (entry.family == family)
              {
                carded = true;
                break;
              }
            }
            if (carded)
              continue;
            VoiceModelDef def;
            def.id = family;
            def.name = family;
            def.family = family;
            def.file = "@" + bundledRoot + "/" + family;
            std::vector<std::string> namedFiles;
            for (const auto &item :
                 std::filesystem::directory_iterator(bundledRoot + "/" + family, bundledEc))
            {
              std::error_code step;
              if (std::filesystem::is_directory(item.path(), step))
                continue;
              namedFiles.push_back("@" + bundledRoot + "/" + family + "/" +
                                   item.path().filename().string());
            }
            std::sort(namedFiles.begin(), namedFiles.end());
            def.files = namedFiles;
            gCardList.push_back(def);
          }
        }
      }
      // The cards were just (re)read, so any kept table answer is behind them.
      gModelsAnswerFresh = false;
      gHouseAnswerFresh = false;
    }

    static bool storeVoice(const std::string &name, const std::string &wavB64,
                           const std::string &promptText)
    {
      if (!isVoiceName(name))
        return false;
      const std::string wav = fromBase64(wavB64);
      if (wav.size() < 44 || wav.compare(0, 4, "RIFF") != 0 || wav.compare(8, 4, "WAVE") != 0)
        return false;
      if (!writeFileBytes(voiceDir() + "/" + name + ".wav", wav))
        return false;
      if (!promptText.empty())
        writeFileBytes(transcriptName(name), promptText);
      else
        std::filesystem::remove(transcriptName(name));
      return true;
    }

    static std::string dropVoiceReason(const std::string &name)
    {
      if (!isVoiceName(name))
        return "That name cannot be used: a voice name is letters, digits, dot, dash";
      const std::string wav = voiceDir() + "/" + name + ".wav";
      if (!fileExists(wav))
        return "No recording is kept under that name";
      std::error_code ec;
      std::filesystem::remove(wav, ec);
      std::filesystem::remove(transcriptName(name));
      if (ec)
        return "The files would not move";
      return std::string();
    }

    // The move answers with nothing when it moved, and with the reason it did not
    // otherwise. The reason is one of the three things a person can tell apart
    // themselves - a name that cannot be used, a recording that is not kept under
    // that name, a name another recording already keeps - so a refusal does not
    // leave them guessing which of the two names to change.
    static std::string keepVoiceUnder(const std::string &name, const std::string &wanted)
    {
      if (!isVoiceName(name) || !isVoiceName(wanted))
        return "That name cannot be used: a voice name is letters, digits, dot, dash";
      const std::string wav = voiceDir() + "/" + name + ".wav";
      if (!fileExists(wav))
        return "No recording is kept under that name";
      if (fileExists(voiceDir() + "/" + wanted + ".wav"))
        return "Another recording already keeps that name";
      std::error_code ec;
      std::filesystem::rename(wav, voiceDir() + "/" + wanted + ".wav", ec);
      if (ec)
        return "The files would not move";
      const std::string transcript = readFileText(transcriptName(name));
      std::filesystem::remove(transcriptName(name));
      if (!transcript.empty())
        writeFileBytes(transcriptName(wanted), transcript);
      return std::string();
    }

    // What a model offers as voices: its library is the shared room every
    // voice model reads; the packaged names are the model card's own list, or
    // the embeddings standing beside the weights where the card says they live
    // on disk. Nothing is invented here.
    static std::string voicesListJson(const std::string &modelId)
    {
      json out = {{"model", modelId}, {"library", json::array()}, {"presets", json::array()}};
      std::vector<std::string> names;
      std::error_code ec;
      if (std::filesystem::is_directory(voiceDir(), ec))
      {
        for (const auto &item : std::filesystem::directory_iterator(voiceDir(), ec))
        {
          const std::string fileName = item.path().filename().string();
          if (item.path().extension().string() == ".wav")
            names.push_back(fileName.substr(0, fileName.size() - 4));
        }
      }
      std::sort(names.begin(), names.end());
      for (const auto &name : names)
      {
        json row = {{"name", name}, {"wav", "/voice/files/voice/" + name}};
        row["has_transcript"] = fileExists(transcriptName(name));
        const double seconds = wavSeconds(voiceDir() + "/" + name + ".wav");
        if (seconds > 0.0)
          row["audio_s"] = seconds;
        out["library"].push_back(row);
      }

      const VoiceModelDef *def = nullptr;
      for (const VoiceModelDef &entry : gCardList)
        if (modelId == entry.id)
          def = &entry;
      std::vector<std::string> presets;
      if (def)
        presets = def->presetVoices;
      if (def)
      {
        // a package that ships its packaged voices as embeddings beside the
        // weights offers those names too: they are the files' own names.
        const std::string weight = packagePath(def->file);
        const size_t slash = weight.find_last_of('/');
        const std::string embeddings =
            (slash == std::string::npos ? std::string() : weight.substr(0, slash)) + "/embeddings";
        std::error_code disc;
        if (!embeddings.empty() && std::filesystem::is_directory(embeddings, disc))
        {
          for (const auto &item : std::filesystem::directory_iterator(embeddings, disc))
          {
            const std::string fileName = item.path().filename().string();
            if (item.path().extension().string() != ".safetensors" ||
                fileName.size() <= 12)
              continue;
            presets.push_back(fileName.substr(0, fileName.size() - 12));
          }
        }
      }
      std::sort(presets.begin(), presets.end());
      presets.erase(std::unique(presets.begin(), presets.end()), presets.end());
      for (const auto &name : presets)
      {
        json row = {{"name", name}};
        const size_t under = name.find('_');
        if (under != std::string::npos)
          row["prefix"] = name.substr(0, under);
        out["presets"].push_back(row);
      }
      return out.dump();
    }

    // ---- job records, the same shape every service keeps ----

    static std::mutex gMutex;
    static std::map<std::string, json> gJobs;
    static std::deque<std::string> gJobOrder;
    static std::map<std::string, std::vector<std::string>> gEvents;
    static std::map<std::string, bool> gCancel;
    static std::map<std::string, long long> gStartedMs;

    static void pushEvent(const std::string &job, const std::string &event)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      gEvents[job].push_back(event);
    }

    static void markState(const std::string &job, const std::string &state)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      const auto found = gJobs.find(job);
      if (found == gJobs.end())
        return;
      found->second["state"] = state;
      if (ols::stateEnded(state))
      {
        found->second["ended_at"] = ols::nowSeconds();
        const auto started = gStartedMs.find(job);
        if (started != gStartedMs.end())
        {
          found->second["elapsed_ms"] = steadyMs() - started->second;
          gStartedMs.erase(job);
        }
        // A finished job finished every step of its own count, so a row that
        // reads "0 of 1" under a job that just completed would be a lie; a
        // stopped or failed job keeps the part it really did.
        if (state == ols::kStateDone)
        {
          const auto max = found->second.find("max");
          const auto done = found->second.find("done");
          if (max != found->second.end() && done != found->second.end())
            *done = *max;
        }
      }
    }

    static void noteError(const std::string &job, const std::string &reason)
    {
      {
        std::lock_guard<std::mutex> lock(gMutex);
        const auto found = gJobs.find(job);
        if (found != gJobs.end())
          found->second["error"] = reason;
      }
      pushEvent(job, json{{"type", "error"}, {"message", reason}}.dump());
    }

    static void setRecordField(const std::string &job, const std::string &key,
                               const json &value)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      const auto found = gJobs.find(job);
      if (found != gJobs.end())
        found->second[key] = value;
    }

    static void setRecordText(const std::string &job, const std::string &key,
                              const std::string &value)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      const auto found = gJobs.find(job);
      if (found != gJobs.end())
        found->second[key] = value;
    }

    static bool cancelRequested(const std::string &job)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      const auto found = gCancel.find(job);
      return found != gCancel.end() && found->second;
    }

    // the newest jobs this engine keeps: past this its oldest record is dropped, so
    // the merged status answer stays a few kilobytes however long the machine has
    // been up. the cap is the one number the shared ledger states, and the status
    // page's words about what is kept follow it.

    static void pruneJobs()
    {
      std::lock_guard<std::mutex> lock(gMutex);
      ols::forgetPastKept(gJobOrder, ols::kKeptJobs, gJobs, gEvents, gCancel, gStartedMs);
    }

    static void startJobRecord(const std::string &id, const std::string &kind,
                               const std::string &modelId, const std::string &voiceName,
                               const std::string &textPreview)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      json record = {{"id", id},
                     {"kind", kind},
                     {"model", modelId},
                     {"state", ols::kStateRunning},
                     {"started_at", ols::nowSeconds()}};
      // a spoken line and a listen are one answer each, so they state no window
      // of their own; what a job counts - a download its files - is stated by
      // the one that starts it, where the real count is known.
      if (!voiceName.empty())
        record["voice"] = voiceName;
      if (!textPreview.empty())
        record["text"] = textPreview;
      gJobs[id] = record;
      gJobOrder.push_back(id);
      gEvents[id] = {};
      gCancel[id] = false;
      gStartedMs[id] = steadyMs();
    }

    static std::string jobsListText()
    {
      std::lock_guard<std::mutex> lock(gMutex);
      json out = json::array();
      for (const auto &id : gJobOrder)
      {
        const auto found = gJobs.find(id);
        if (found != gJobs.end())
          out.push_back(found->second);
      }
      return out.dump();
    }

    static std::string jobStateOf(const std::string &id)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      const auto found = gJobs.find(id);
      if (found == gJobs.end())
        return std::string("no such job");
      return found->second.dump();
    }

    static bool cancelJobOf(const std::string &id)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      const auto found = gJobs.find(id);
      // only a run still in motion can be stopped: a record that finished is
      // named by the door's own words instead of answered as though the stop
      // had done something.
      if (found == gJobs.end() || !ols::jobStanding(found->second))
        return false;
      gCancel[id] = true;
      return true;
    }

    // The name this family asks its word positions with, when it has one: the
    // request option that names timestamps. An empty answer means the family
    // states no positions at all.
    static std::string timestampsOptionOf(audiocpp_model *model)
    {
      if (!model)
        return std::string();
      const size_t count = audiocpp_model_option_count(model, AUDIOCPP_OPTION_SCOPE_REQUEST);
      for (size_t at = 0; at < count; ++at)
      {
        const char *name = nullptr;
        const char *valueName = nullptr;
        const char *description = nullptr;
        const char *defaultValue = nullptr;
        const char *minValue = nullptr;
        const char *maxValue = nullptr;
        int required = 0;
        if (audiocpp_model_option(model, AUDIOCPP_OPTION_SCOPE_REQUEST, at, &name, &valueName,
                                  &description, &defaultValue, &minValue, &maxValue,
                                  &required) != AUDIOCPP_OK)
          continue;
        if (!name)
          continue;
        const std::string found(name);
        if (found.find("timestamp") != std::string::npos)
          return found;
      }
      return std::string();
    }

    // ---------- the words of a page question ----------
    // A page asks for exactly what it is about to draw, in the same words its
    // own controls show: a family, a task, a state, free text, an order, an
    // offset, a limit. The answer is that page and its count; the page never
    // pulls the whole table to learn how many there are.

    static void addUnique(std::vector<std::string> &words, const std::string &word)
    {
      if (word.empty())
        return;
      if (std::find(words.begin(), words.end(), word) == words.end())
        words.push_back(word);
    }

    static void addUnique(json &row, const std::string &word)
    {
      if (word.empty() || !row.is_array())
        return;
      for (const json &held : row)
        if (held.is_string() && held.get<std::string>() == word)
          return;
      row.push_back(word);
    }

    static void addUnique(json &row, const std::vector<std::string> &more)
    {
      for (const std::string &word : more)
        addUnique(row, word);
    }

    static int hexValue(char digit)
    {
      if (digit >= '0' && digit <= '9')
        return digit - '0';
      if (digit >= 'a' && digit <= 'f')
        return digit - 'a' + 10;
      if (digit >= 'A' && digit <= 'F')
        return digit - 'A' + 10;
      return -1;
    }

    // The plain words behind a question's value: percent escapes back to their
    // bytes, and a plus is a space, which is what a browser sends.
    static std::string unescapeWords(const std::string &raw)
    {
      std::string text;
      for (size_t at = 0; at < raw.size();)
      {
        if (raw[at] == '%' && at + 2 < raw.size())
        {
          const int high = hexValue(raw[at + 1]);
          const int low = hexValue(raw[at + 2]);
          if (high >= 0 && low >= 0)
          {
            text.push_back(static_cast<char>((high * 16) + low));
            at += 3;
            continue;
          }
        }
        if (raw[at] == '+')
        {
          text.push_back(' ');
          ++at;
          continue;
        }
        text.push_back(raw[at]);
        ++at;
      }
      return text;
    }

    static std::string lowerWords(std::string text)
    {
      for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return text;
    }

    static std::map<std::string, std::string> parseQuestion(const std::string &query)
    {
      std::map<std::string, std::string> asked;
      size_t at = 0;
      while (at < query.size())
      {
        const size_t end = query.find('&', at);
        const std::string pair =
            query.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const size_t equals = pair.find('=');
        if (equals != std::string::npos)
          asked[unescapeWords(pair.substr(0, equals))] = unescapeWords(pair.substr(equals + 1));
        if (end == std::string::npos)
          break;
        at = end + 1;
      }
      return asked;
    }

    // A card read as the words a question is made of.
    static std::string rowField(const json &row, const char *key)
    {
      const auto found = row.find(key);
      if (found == row.end())
        return std::string();
      const json &held = *found;
      if (!held.is_string())
        return std::string();
      return lowerWords(held.get<std::string>());
    }

    // Does a card name this exact word in one of its lists? A filter matches
    // whole words, so asking for one task never answers with a family whose
    // name merely contains it.
    static bool rowNamesWord(const json &row, const std::vector<const char *> &keys,
                             const std::string &word)
    {
      for (const char *key : keys)
      {
        const auto found = row.find(key);
        if (found == row.end())
          continue;
        const json &names = *found;
        if (!names.is_array())
          continue;
        for (const json &held : names)
        {
          if (held.is_string() && lowerWords(held.get<std::string>()) == word)
            return true;
        }
      }
      return false;
    }

    static long long rowBytes(const json &row)
    {
      const auto found = row.find("weights_bytes");
      if (found == row.end())
        return 0;
      const json &held = *found;
      if (!held.is_number())
        return 0;
      return held.get<long long>();
    }

    // A number out of a question, kept inside what the answer can say.
    static size_t clampNumber(const std::map<std::string, std::string> &asked,
                              const std::string &key, size_t fallback, size_t highest)
    {
      const auto found = asked.find(key);
      if (found == asked.end() || found->second.empty())
        return fallback;
      std::istringstream in(found->second);
      long long askedNumber = 0;
      if (!(in >> askedNumber) || askedNumber < 0)
        return fallback;
      const size_t value = static_cast<size_t>(askedNumber);
      return value > highest ? highest : value;
    }

    static bool rowMatchesQuestion(const json &row, const std::map<std::string, std::string> &asked)
    {
      const auto family = asked.find("family");
      if (family != asked.end() && !family->second.empty())
      {
        const std::string want = lowerWords(family->second);
        if (rowField(row, "family") != want && rowField(row, "family_name") != want)
          return false;
      }
      const auto task = asked.find("task");
      if (task != asked.end() && !task->second.empty())
      {
        const std::string want = lowerWords(task->second);
        if (!rowNamesWord(row, {"declared_tasks", "tasks_offline", "can"}, want))
          return false;
      }
      const auto language = asked.find("language");
      if (language != asked.end() && !language->second.empty())
      {
        if (!rowNamesWord(row, {"languages", "declared_languages"}, lowerWords(language->second)))
          return false;
      }
      const auto state = asked.find("state");
      if (state != asked.end() && !state->second.empty())
      {
        const bool loaded = row.value("loaded", false);
        const bool present = row.value("weights_present", false);
        const std::string want = lowerWords(state->second);
        if (want == "memory" || want == "loaded")
        {
          if (!loaded)
            return false;
        }
        else if (want == "disk" || want == "files")
        {
          if (!present)
            return false;
        }
        else if (want == "missing" || want == "fetch")
        {
          if (present || loaded)
            return false;
        }
        else if (want == "here")
        {
          if (!loaded && !present)
            return false;
        }
      }
      const auto words = asked.find("q");
      if (words != asked.end() && !words->second.empty())
      {
        std::string haystack = rowField(row, "id") + " " + rowField(row, "name") + " " +
                               rowField(row, "family") + " " + rowField(row, "family_name") + " " +
                               rowField(row, "description") + " " + rowField(row, "repo");
        for (const char *key : {"declared_languages", "languages", "preset_voices", "tasks_offline"})
        {
          const auto list = row.find(key);
          if (list == row.end())
            continue;
          const json &names = *list;
          if (!names.is_array())
            continue;
          for (const json &held : names)
            if (held.is_string())
              haystack += " " + lowerWords(held.get<std::string>());
        }
        std::istringstream in(words->second);
        std::string word;
        while (in >> word)
        {
          if (haystack.find(lowerWords(word)) == std::string::npos)
            return false;
        }
      }
      return true;
    }

    // The order a page asked for. With no order asked, cards stand as the page
    // draws them: by family, and inside a family the ones that answer first.
    static void sortCards(std::vector<json> &rows, const std::map<std::string, std::string> &asked)
    {
      const std::string sort = asked.count("sort") ? lowerWords(asked.at("sort")) : std::string();
      std::stable_sort(rows.begin(), rows.end(), [&](const json &a, const json &b)
                       {
        if (sort == "bytes")
        {
          const long long leftBytes = rowBytes(a);
          const long long rightBytes = rowBytes(b);
          if (leftBytes != rightBytes)
            return leftBytes > rightBytes;
          const std::string leftName = rowField(a, "name"), rightName = rowField(b, "name");
          if (leftName != rightName)
            return leftName < rightName;
          return rowField(a, "id") < rowField(b, "id");
        }
        if (sort == "name")
        {
          const std::string leftName = rowField(a, "name"), rightName = rowField(b, "name");
          if (leftName != rightName)
            return leftName < rightName;
          return rowField(a, "id") < rowField(b, "id");
        }
        const int leftState = (a.value("loaded", false) ? 0 : 1) + (a.value("weights_present", false) ? 0 : 1);
        const int rightState = (b.value("loaded", false) ? 0 : 1) + (b.value("weights_present", false) ? 0 : 1);
        if (leftState != rightState)
          return leftState < rightState;
        const std::string leftFamily = rowField(a, "family"), rightFamily = rowField(b, "family");
        if (leftFamily != rightFamily)
          return leftFamily < rightFamily;
        return rowField(a, "id") < rowField(b, "id"); });
    }

    // What the model table answers: the house rows, and for every model that
    // stands loaded the truth its own handle gives - supported tasks, speaker
    // references, style conditioning, the languages it speaks, and every
    // option it declares at request, session, and load scope. A model that
    // was never loaded says so plainly and carries no invented parameters.
    static std::string modelsListJson()
    {
      {
        std::lock_guard<std::mutex> lock(gModelsAnswerMutex);
        if (gModelsAnswerFresh && !gModelsAnswer.empty())
          return gModelsAnswer;
      }
      ensureRegistry();
      json out = json::array();
      for (const VoiceModelDef &def : gCardList)
      {
        const std::string id = def.id;
        const std::string weight = packagePath(def.file);
        const bool weightPresent = weightOnDisk(absolutePath(weight));
        json keptLanguages = json::array();
        for (const std::string &word : def.declaredLanguages)
          if (languageWordOk(word))
            keptLanguages.push_back(word);
        json row = {{"id", id},
                    {"name", def.name},
                    {"description", def.description},
                    {"engine", "audio.cpp"},
                    {"family", def.family},
                    {"family_name", def.familyName},
                    {"weights_present", weightPresent},
                    {"loaded", false},
                    {"can", def.capabilities},
                    {"declared_tasks", def.tasks},
                    {"declared_languages", keptLanguages},
                    {"preset_voices", def.presetVoices}};
        row["repo"] = def.repo;
        {
          json files = json::array();
          long long bytes = 0;
          for (const auto &named : def.files)
          {
            const std::string full = packagePath(named);
            const unsigned long long size = weightBytes(absolutePath(full));
            if (size == 0ull)
              continue;
            files.push_back(full);
            bytes += static_cast<long long>(size);
          }
          row["files"] = files;
          row["weights_bytes"] = bytes;
        }
        if (row["weights_present"].get<bool>())
        {
          std::lock_guard<std::mutex> lock(gEngineMutex);
          const auto model = gModels.find(id);
          if (model != gModels.end())
          {
            row["loaded"] = true;
            row["family"] = audiocpp_model_family(model->second);
            row["description_engine"] = audiocpp_model_description(model->second);
            json can = json::array();
            for (size_t at = 0; at < audiocpp_task_count(); ++at)
            {
              const char *token = audiocpp_task_name(at);
              if (!token)
                continue;
              if (audiocpp_model_supports(model->second, token, "offline"))
                can.push_back(token);
            }
            row["tasks_offline"] = can;
            row["speaker_reference"] =
                audiocpp_model_supports_speaker_reference(model->second) != 0;
            row["style_condition"] = audiocpp_model_supports_style_condition(model->second) != 0;
            // whether this model can say where the words land in its answer: the
            // page only offers the word-tracking door to a model that states it.
            const std::string positionOption = timestampsOptionOf(model->second);
            row["timestamps"] = !positionOption.empty();
            row["timestamps_option"] = positionOption;
            json languages = json::array();
            for (size_t at = 0; at < audiocpp_model_language_count(model->second); ++at)
            {
              const char *language = nullptr;
              if (audiocpp_model_language(model->second, at, &language) == AUDIOCPP_OK && language && languageWordOk(language))
                languages.push_back(std::string(language));
            }
            row["languages"] = languages;
            json params = json::array();
            for (int scopeValue = 0; scopeValue <= 2; ++scopeValue)
            {
              const auto scope = static_cast<audiocpp_option_scope>(scopeValue);
              const size_t count = audiocpp_model_option_count(model->second, scope);
              for (size_t at = 0; at < count; ++at)
              {
                const char *name = nullptr;
                const char *valueName = nullptr;
                const char *description = nullptr;
                const char *defaultValue = nullptr;
                const char *minValue = nullptr;
                const char *maxValue = nullptr;
                int required = 0;
                if (audiocpp_model_option(model->second, scope, at, &name, &valueName, &description,
                                          &defaultValue, &minValue, &maxValue, &required) !=
                    AUDIOCPP_OK)
                  continue;
                json param = {{"scope",
                               scopeValue == 0 ? "request" : (scopeValue == 1 ? "session" : "load")},
                              {"name", std::string(name)},
                              {"value_name", std::string(valueName)},
                              {"description", std::string(description)},
                              {"default", std::string(defaultValue)},
                              {"min", std::string(minValue)},
                              {"max", std::string(maxValue)},
                              {"required", required != 0}};
                params.push_back(param);
              }
            }
            row["params"] = params;
          }
          else
          {
            const auto refusal = gLoadErrors.find(id);
            if (refusal != gLoadErrors.end())
              row["load_error"] = refusal->second;
          }
        }
        out.push_back(row);
      }
      if (!out.empty())
      {
        // The words and the rows they were built from stand together, under the one
        // lock that says they stand together.
        std::lock_guard<std::mutex> lock(gModelsAnswerMutex);
        gModelsAnswer = out.dump();
        // The parsed table beside its words: a page of cards filters and sorts
        // this rather than reading the string back into a table on every read.
        gModelsTable = out;
        gModelsAnswerFresh = true;
        return gModelsAnswer;
      }
      // Nothing was read: the table was not built yet, so nothing is kept, and
      // the next call reads again rather than repeating an empty answer.
      return out.dump();
    }

    // A page of the table, asked in words:
    // "family=pocket_tts&task=tts&state=memory&q=hello&sort=bytes&offset=40&limit=20".
    // A page that draws 400 packages one at a time asks for a page of them and
    // for the house answer that counts them, never for the whole table; the
    // numbers it shows are these answers', not anything the page keeps by heart.
    //
    // The table it draws out of is the one the whole-table answer was built from,
    // kept in its parsed shape beside those words: filtering rows already in
    // memory is what a page of forty costs, where reading four hundred back out of
    // a string on every read was work the same table paid again and again.
    static json modelsTable()
    {
      modelsListJson();
      std::lock_guard<std::mutex> lock(gModelsAnswerMutex);
      return gModelsTable;
    }

    static std::string modelCardsPageJson(const std::string &query)
    {
      const json whole = modelsTable();
      const std::map<std::string, std::string> asked = parseQuestion(query);
      std::vector<json> matched;
      if (whole.is_array())
      {
        matched.reserve(whole.size());
        for (const json &row : whole)
          if (rowMatchesQuestion(row, asked))
            matched.push_back(row);
      }
      sortCards(matched, asked);
      const size_t total = matched.size();
      const size_t offset = clampNumber(asked, "offset", 0, total);
      const size_t limit = clampNumber(asked, "limit", 40, 200);
      json rows = json::array();
      for (size_t at = offset; at < total && rows.size() < limit; ++at)
        rows.push_back(matched[at]);
      const json out = {{"total", total},
                        {"offset", offset},
                        {"limit", limit},
                        {"returned", rows.size()},
                        {"more", offset + rows.size() < total},
                        {"sort", asked.count("sort") ? asked.at("sort") : std::string()},
                        {"rows", rows}};
      return out.dump();
    }

    // The house answer: what this machine can offer, counted from its own
    // cards - families, their packages, which of those packages have files here
    // and which stand loaded, the bytes they weigh, and the tasks and
    // languages those families name. Nothing here is a list the page remembers.
    // The version of the engine linked into this binary, in the words a person
    // reads. the library states its own build version when its build asked for one
    // and says "unknown" when it does not; the version this binary was built to is
    // always knowable from the same header, which spells it as a number (major in
    // the top bits, minor next, patch last), so a page can name the engine it is
    // really talking to rather than reading "unknown".
    static std::string engineAbiWords()
    {
      const uint32_t packed = audiocpp_abi_version();
      return std::to_string(packed >> 16) + "." + std::to_string((packed >> 8) & 0xFFu) +
             "." + std::to_string(packed & 0xFFu);
    }

    static std::string modelsHouseJson()
    {
      ensureRegistry();
      if (gHouseAnswerFresh && !gHouseAnswer.empty())
        return gHouseAnswer;
      json groups = json::array();
      std::map<std::string, size_t> at;
      std::vector<std::string> taskWords;
      std::vector<std::string> languageWords;
      long long grandBytes = 0;
      size_t presentAll = 0, loadedAll = 0;
      // Read with the card table below, under the same lock: the families the
      // specs name that this build has no loader for, and the packages they name.
      json noLoaderFamilies = json::array();
      size_t noLoaderPackages = 0;
      {
        std::lock_guard<std::mutex> lock(gEngineMutex);
        for (const VoiceModelDef &def : gCardList)
        {
          long long bytes = 0;
          for (const std::string &named : def.files)
            bytes += static_cast<long long>(weightBytes(packagePath(named)));
          const bool present = weightOnDisk(packagePath(def.file));
          const bool loaded = gModels.find(def.id) != gModels.end();
          if (present)
            ++presentAll;
          if (loaded)
            ++loadedAll;
          const auto found = at.find(def.family);
          size_t slot = 0;
          if (found == at.end())
          {
            slot = groups.size();
            at[def.family] = slot;
            groups.push_back({{"family", def.family},
                              {"name", def.familyName},
                              {"packages", 0},
                              {"present", 0},
                              {"loaded", 0},
                              {"bytes", 0},
                              {"tasks", json::array()},
                              {"languages", json::array()},
                              {"repos", json::array()}});
          }
          else
          {
            slot = found->second;
          }
          json &row = groups.at(slot);
          row["packages"] = row.at("packages").get<int>() + 1;
          row["present"] = row.at("present").get<int>() + (present ? 1 : 0);
          row["loaded"] = row.at("loaded").get<int>() + (loaded ? 1 : 0);
          row["bytes"] = row.at("bytes").get<long long>() + bytes;
          grandBytes += bytes;
          for (const std::string &word : def.tasks)
            addUnique(taskWords, word);
          for (const std::string &word : def.declaredLanguages)
          {
            if (!languageWordOk(word))
              continue;
            addUnique(languageWords, word);
            addUnique(row.at("languages"), word);
          }
          if (!def.repo.empty())
            addUnique(row.at("repos"), def.repo);
          addUnique(row.at("tasks"), def.tasks);
        }
        noLoaderFamilies = gNoLoaderFamilies;
        noLoaderPackages = gNoLoaderPackages;
      }
      // The task words this build accepts, straight from the engine: a page
      // offers its filter from these, never from a list of its own.
      json engineTasks = json::array();
      for (size_t index = 0; index < audiocpp_task_count(); ++index)
      {
        const char *token = audiocpp_task_name(index);
        if (token)
          addUnique(engineTasks, std::string(token));
      }
      json out = {{"ok", true},
                  {"engine", "audio.cpp (C API, linked into this binary)"},
                  {"build", std::string(audiocpp_build_version())},
                  {"abi_named", engineAbiWords()},
                  {"families", static_cast<int>(groups.size())},
                  {"packages", static_cast<int>(gCardList.size())},
                  {"present", static_cast<int>(presentAll)},
                  {"loaded", static_cast<int>(loadedAll)},
                  {"bytes", grandBytes},
                  {"tasks", taskWords},
                  {"tasks_of_this_build", engineTasks},
                  {"languages", languageWords},
                  {"families_without_loaders", static_cast<int>(noLoaderFamilies.size())},
                  {"families_without_loaders_named", noLoaderFamilies},
                  {"packages_without_loaders", static_cast<int>(noLoaderPackages)},
                  {"family_rows", groups}};
      gHouseAnswer = out.dump();
      gHouseAnswerFresh = true;
      return gHouseAnswer;
    }

    static std::string modelCardsJson(const std::string &query)
    {
      return modelCardsPageJson(query);
    }

    static std::string modelsSummaryJson()
    {
      return modelsHouseJson();
    }

    static std::string engineStateJson()
    {
      ensureRegistry();
      // This door counts what stands loaded and names the ones that do. The
      // packages themselves - with the words their own files state and the
      // reason a load was refused - belong to the models door, which answers all
      // of that once for every package. Repeating that table here was a list of
      // four hundred the system snapshot then threw away again, and it asked the
      // disk for each package's files on a door read every few seconds.
      json standing = json::array();
      size_t loadedCount = 0;
      {
        std::lock_guard<std::mutex> lock(gEngineMutex);
        for (const VoiceModelDef &def : gCardList)
        {
          if (gModels.find(std::string(def.id)) != gModels.end())
          {
            ++loadedCount;
            standing.push_back(std::string(def.id));
          }
        }
      }
      json out = {{"ok", true},
                  {"engine", "audio.cpp (C API, linked into this binary)"},
                  {"build", std::string(audiocpp_build_version())},
                  {"abi", audiocpp_abi_version()},
                  {"abi_named", engineAbiWords()},
                  {"models_total", static_cast<int>(gCardList.size())},
                  {"models_loaded", static_cast<int>(loadedCount)},
                  {"open_models", standing}};
      {
        std::lock_guard<std::mutex> lock(gEngineMutex);
        const auto registry = gLoadErrors.find("(registry)");
        if (registry != gLoadErrors.end())
          out["error"] = registry->second;
      }
      return out.dump();
    }

    static std::string engineErrorText()
    {
      std::lock_guard<std::mutex> lock(gEngineMutex);
      std::string text;
      for (const auto &entry : gLoadErrors)
      {
        if (!text.empty())
          text += "; ";
        text += entry.first + ": " + entry.second;
      }
      return text;
    }

    // The engine, stood up to the point its cards can be read, and then this
    // model's name looked up in them.
    static bool knownModel(const std::string &modelId)
    {
      ensureRegistry();
      for (const VoiceModelDef &def : gCardList)
        if (modelId == def.id)
          return true;
      return false;
    }

    // One speak: the request the family declares, built from what the page
    // asked for. Unknown option names are passed to the family untouched - the
    // family says which it refuses in its own words, and those words land on
    // the job's record rather than being filtered out here.
    // A job runs on a thread of its own: an exception that reached the top of
    // that thread would end the process, so the failure becomes the job's own
    // error state, which is what the pages read.
    template <class Fn>
    static void runJobGuarded(const std::string &id, const Fn &body)
    {
      try
      {
        body();
      }
      catch (const std::exception &e)
      {
        // the words a stopped run carries become the job's own; a throw that
        // names none says so rather than answering with an empty reason.
        const std::string words = e.what();
        noteError(id, words.empty() ? std::string("the run stopped without words") : words);
        markState(id, ols::kStateError);
      }
      catch (...)
      {
        // a throw outside the standard kinds carries no words at all.
        noteError(id, "the run stopped in a way it does not state");
        markState(id, ols::kStateError);
      }
    }

    static void runSpeakJob(const std::string &id, const std::string &modelId,
                            const std::string &bodyJson)
    {
      json request = json::parse(bodyJson, nullptr, false);
      if (request.is_discarded())
      {
        noteError(id, "the request was not readable");
        markState(id, ols::kStateError);
        return;
      }
      const std::string text = request.value("text", std::string());
      const std::string language = request.value("language", std::string());
      const std::string voice = request.value("voice", std::string());
      const std::string referenceB64 = request.value("reference_b64", std::string());
      const std::string referenceText = request.value("reference_text", std::string());
      if (text.empty())
      {
        noteError(id, "nothing to say");
        markState(id, ols::kStateError);
        return;
      }
      if (cancelRequested(id))
      {
        markState(id, ols::kStateCancelled);
        return;
      }

      ensureRegistry();
      audiocpp_model *model = nullptr;
      {
        std::lock_guard<std::mutex> lock(gEngineMutex);
        const auto found = gModels.find(modelId);
        if (found != gModels.end())
          model = found->second;
      }
      if (!model)
      {
        std::string why = kNoMemoryWords;
        {
          std::lock_guard<std::mutex> lock(gEngineMutex);
          const auto refusal = gLoadErrors.find(modelId);
          if (refusal != gLoadErrors.end())
            why += ": " + refusal->second;
        }
        noteError(id, why);
        markState(id, ols::kStateError);
        return;
      }
      // A session belongs to one kind of work, so the speaking door names the
      // kinds it can answer with, best first: a recording in hand means the
      // speaker is the one the model listens to; words alone mean the model
      // makes a voice, and words that describe a voice are what a designing
      // family takes as its own instruction. The first kind a model declares,
      // and this build can run, is the one asked for.
      std::vector<std::string> wanted;
      // The same test the request below uses: a name the library holds a recording
      // under is a speaker the model listens to, and that is a cloning session. A
      // name the model carries among its own voices is plain speaking - it travels
      // to the family as a voice name - so asking it for a cloning session refused
      // every packaged voice a page could offer (measured on the UM780: kokoro-82m
      // with af_heart, "this model or this build answers none of clon, vc, s2s").
      const bool savedVoice =
          isVoiceName(voice) && fileExists(voiceDir() + "/" + voice + ".wav");
      if (!referenceB64.empty() || savedVoice)
        wanted = {"clon", "vc", "s2s"};
      else
        wanted = {"tts", "vdes", "gen", "clon"};
      // The controls a standing model is configured with. A card names them; the
      // page offers exactly those; anything the model does not accept is refused
      // by the engine in its own words, and a request that is not a set of names
      // and values is refused here before the engine is asked at all.
      json sessionValues = json::object();
      const auto sessionGiven = request.find("session_options");
      if (sessionGiven != request.end() && !sessionGiven->is_object())
      {
        noteError(id, kRunControlWords);
        markState(id, ols::kStateError);
        return;
      }
      if (sessionGiven != request.end())
        sessionValues = *sessionGiven;
      audiocpp_session *session = sessionFor(modelId, wanted, sessionValues);
      if (!session)
      {
        const std::string detail = lastEngineError();
        // The engine states why in its own words when it can; when it cannot, name the
        // kinds this door asked for so a reader learns which of the two happened.
        noteError(id, detail.empty()
                          ? "no session could be made for this kind of work: this model or this build answers none of " + wantedWords(wanted)
                          : "no session could be made for this kind of work: " + detail);
        markState(id, ols::kStateError);
        return;
      }

      audiocpp_request *req = audiocpp_request_create();
      if (!req)
      {
        noteError(id, "the request could not be made");
        markState(id, ols::kStateError);
        return;
      }
      bool ok = true;
      const char *reason = nullptr;
      if (audiocpp_request_set_text(req, text.c_str(),
                                    language.empty() ? nullptr : language.c_str()) != AUDIOCPP_OK)
      {
        ok = false;
        reason = "the text was refused";
      }

      // a voice for this run: an inline recording wins, then a saved one from
      // the library (its recording and its words), then the name is passed to
      // the family as its own preset voice.
      if (ok && (!referenceB64.empty() || !voice.empty()))
      {
        std::vector<float> samples;
        size_t frames = 0;
        int rate = 0;
        int channels = 0;
        std::string transcript = referenceText;
        bool wantAudio = false;
        if (!referenceB64.empty())
        {
          wantAudio = true;
          // write it to a momentary file so the reader sees a real file
          const std::string scratch = inlineAudioPath(id, "reference");
          if (!writeFileBytes(scratch, fromBase64(referenceB64)))
          {
            ok = false;
            reason = "the inline recording could not be kept";
          }
          else
          {
            samples = readWavPcm(scratch, frames, rate, channels);
            std::error_code gone;
            std::filesystem::remove(scratch, gone);
          }
        }
        else if (isVoiceName(voice) && fileExists(voiceDir() + "/" + voice + ".wav"))
        {
          wantAudio = true;
          samples = readWavPcm(voiceDir() + "/" + voice + ".wav", frames, rate, channels);
          if (transcript.empty())
            transcript = readFileText(transcriptName(voice));
        }
        if (ok && wantAudio && samples.empty())
        {
          ok = false;
          reason = "the recording could not be read as plain 16-bit PCM";
        }
        if (ok && !samples.empty())
        {
          if (audiocpp_request_set_voice_audio(req, samples.data(), frames, rate, channels) !=
              AUDIOCPP_OK)
          {
            ok = false;
            reason = "the family refused the reference recording";
          }
          else if (!transcript.empty())
          {
            // the words of the reference travel as the family's own request
            // option; families that do not declare it ignore it or say so, and
            // the run carries either answer through untouched.
            audiocpp_request_set_option(req, "reference_text", transcript.c_str());
          }
        }
        else if (ok && !voice.empty())
        {
          // a packaged voice name travels both ways a family can ask for it: the
          // structured speaker field is what families that prepare a packaged
          // voice read, and the plain option is what families that pick it from
          // the request table read. both carry the same word, so a family cannot
          // hear a different voice than the page offered.
          if (audiocpp_request_set_voice_id(req, voice.c_str()) != AUDIOCPP_OK)
          {
            ok = false;
            reason = "the family refused the voice name";
          }
          else if (audiocpp_request_set_option(req, "voice_id", voice.c_str()) != AUDIOCPP_OK)
          {
            ok = false;
            reason = "the family refused the voice name";
          }
        }
      }

      // everything else the page asks for travels as the family's own
      // request options, untouched.
      bool askedPositions = false;
      if (ok && request.contains("options") && request["options"].is_object())
      {
        for (json::iterator it = request["options"].begin(); it != request["options"].end(); ++it)
        {
          const std::string name = it.key();
          const std::string asText = it->is_string() ? it->get<std::string>() : it->dump();
          if (name.find("timestamp") != std::string::npos)
            askedPositions = true;
          // The style dials a family advertises have their own calls in the
          // engine, mirroring the engine's own flags; the names the page uses
          // are the engine's names, so the match is exact. The numeric ones
          // carry a number, and a number this page cannot read is refused here
          // rather than sent as nonsense.
          bool styleCall = true;
          audiocpp_status setStyle = AUDIOCPP_ERR_INVALID_ARGUMENT;
          if (name == "emotion")
          {
            setStyle = audiocpp_request_set_emotion(req, asText.c_str());
          }
          else if (name == "style_language")
          {
            setStyle = audiocpp_request_set_style_language(req, asText.c_str());
          }
          else if (name == "speaking_rate" || name == "pitch_shift" || name == "energy_scale")
          {
            float value = 0.f;
            char after = 0;
            const bool readNumber = (std::sscanf(asText.c_str(), "%f %c", &value, &after) == 1);
            if (!readNumber)
            {
              ok = false;
              reason = "that dial was given something that is not a number";
              break;
            }
            if (name == "speaking_rate")
              setStyle = audiocpp_request_set_speaking_rate(req, value);
            else if (name == "pitch_shift")
              setStyle = audiocpp_request_set_pitch_shift(req, value);
            else
              setStyle = audiocpp_request_set_energy_scale(req, value);
          }
          else if (name == "style_tag")
          {
            // one tag: its name and what it is set to, split where the page
            // wrote them, since the engine takes the pair.
            const size_t split = asText.find('=');
            if (split == std::string::npos)
            {
              ok = false;
              reason = "a style tag needs its name and its value";
              break;
            }
            const std::string key = asText.substr(0, split);
            const std::string value = asText.substr(split + 1);
            setStyle = audiocpp_request_set_style_tag(req, key.c_str(), value.c_str());
          }
          else
          {
            styleCall = false;
          }
          if (styleCall)
          {
            if (setStyle != AUDIOCPP_OK)
            {
              ok = false;
              reason = "the family refused that style dial";
              break;
            }
            continue;
          }
          if (audiocpp_request_set_option(req, name.c_str(), asText.c_str()) != AUDIOCPP_OK)
          {
            ok = false;
            reason = "the family refused an option";
            break;
          }
        }
      }

      // word positions come by default, so the player can track the words: a
      // model that has an option for them is asked with it on, unless the page
      // already named one.
      const std::string positionOption = timestampsOptionOf(model);
      if (ok && !positionOption.empty() && !askedPositions)
      {
        if (audiocpp_request_set_option(req, positionOption.c_str(), "true") != AUDIOCPP_OK)
        {
          ok = false;
          reason = "the family refused its word positions";
        }
      }

      if (!ok)
      {
        const std::string detail = lastEngineError();
        noteError(id, (reason ? std::string(reason) : std::string()) +
                          (detail.empty() ? std::string() : (": " + detail)));
        markState(id, ols::kStateError);
        audiocpp_request_free(req);
        return;
      }

      audiocpp_result *result = nullptr;
      const audiocpp_status ran = audiocpp_session_run(session, req, &result);
      audiocpp_request_free(req);
      if (ran != AUDIOCPP_OK || !result)
      {
        const std::string detail = lastEngineError();
        noteError(id, "the run refused" + (detail.empty() ? std::string() : (": " + detail)));
        if (result)
          audiocpp_result_free(result);
        markState(id, ols::kStateError);
        return;
      }

      const float *samples = nullptr;
      size_t frames = 0;
      int rate = 0;
      int channels = 0;
      const audiocpp_status audio = audiocpp_result_audio(result, &samples, &frames, &rate, &channels);
      if (audio != AUDIOCPP_OK || !samples || !frames)
      {
        noteError(id, "the answer carried no audio");
        audiocpp_result_free(result);
        markState(id, ols::kStateError);
        return;
      }

      if (cancelRequested(id))
      {
        // the run cannot be interrupted mid-call; stopping one keeps the
        // record's truth and lets the answer go.
        audiocpp_result_free(result);
        markState(id, ols::kStateCancelled);
        return;
      }

      const std::string target = renderDir() + "/" + id + ".wav";
      std::error_code ec;
      if (!std::filesystem::is_directory(renderDir(), ec))
        std::filesystem::create_directories(renderDir(), ec);
      if (!writeWavPcm(target, samples, frames, rate, channels))
      {
        noteError(id, "the answer could not be kept as a file");
        audiocpp_result_free(result);
        markState(id, ols::kStateError);
        return;
      }
      // the words this render was told to say, kept whole: the player lights
      // them against the audio in the words a person typed rather than in
      // whatever alphabet that family names its groups, and a kept render
      // carries them in its own story so a machine that started later can still
      // read an older render along.
      setRecordText(id, "said_words", text);
      // where the words land in the answer, when the model says it: the page
      // tracks them against the audio; a model that states nothing carries
      // nothing here, and the page says so rather than guessing positions.
      const size_t wordCount = audiocpp_result_word_count(result);
      if (wordCount > 0 && rate > 0)
      {
        json words = json::array();
        for (size_t at = 0; at < wordCount; ++at)
        {
          const char *word = nullptr;
          int64_t startSample = 0;
          int64_t endSample = 0;
          float confidence = 0.f;
          if (audiocpp_result_word(result, at, &word, &startSample, &endSample,
                                   &confidence) != AUDIOCPP_OK)
            continue;
          if (!word)
            continue;
          json entry = {{"word", std::string(word)},
                        {"start_s", static_cast<double>(startSample) / static_cast<double>(rate)},
                        {"end_s", static_cast<double>(endSample) / static_cast<double>(rate)}};
          words.push_back(entry);
        }
        if (!words.empty())
          setRecordField(id, "word_times", words);
      }
      audiocpp_result_free(result);
      setRecordText(id, "url", "/voice/files/render/" + id);
      setRecordField(id, "audio_s",
                     static_cast<double>(frames) / static_cast<double>(rate > 0 ? rate : 1));
      markState(id, ols::kStateDone);
      // the story is written once the run is finished, so it carries the whole
      // record of it: what was said, how long the sound is, where the words land,
      // and how long the work took.
      {
        const std::string story = jobStateOf(id);
        std::ofstream out(renderDir() + "/" + id + ".json", std::ios::binary);
        if (!story.empty())
          out << story;
      }
    }

    static std::string startSpeak(const std::string &modelId, const std::string &bodyJson,
                                  const std::string &voiceName, const std::string &text)
    {
      const std::string id = makeId("voice");
      startJobRecord(id, "speak", modelId, voiceName,
                     text.size() > 120 ? text.substr(0, 120) + "..." : text);
      std::thread([id, modelId, bodyJson]
                  { runJobGuarded(id, [&]
                                  { runSpeakJob(id, modelId, bodyJson); }); })
          .detach();
      pruneJobs();
      return id;
    }

    // A recording, listened to: the same engine, the other direction. A family
    // that runs speech-to-words answers with the words it heard, the segments
    // and speaker turns it found and where they sit in the recording; a family
    // that converts, separates, or cleans audio answers with audio - one stream
    // when it has one, and every named stream when it has more (separated
    // sources, per-speaker tracks). Nothing here is chosen by this program: what
    // comes back is what that model's own handle states.
    static void runListenJob(const std::string &id, const std::string &modelId,
                             const std::string &bodyJson)
    {
      json request = json::parse(bodyJson, nullptr, false);
      if (request.is_discarded())
      {
        noteError(id, "the request was not readable");
        markState(id, ols::kStateError);
        return;
      }
      const std::string voice = request.value("voice", std::string());
      const std::string audioB64 = request.value("audio_b64", std::string());
      std::vector<float> samples;
      size_t frames = 0;
      int rate = 0;
      int channels = 0;
      if (!audioB64.empty())
      {
        const std::string scratch = inlineAudioPath(id, "listen");
        if (writeFileBytes(scratch, fromBase64(audioB64)))
        {
          samples = readWavPcm(scratch, frames, rate, channels);
          std::error_code gone;
          std::filesystem::remove(scratch, gone);
        }
      }
      else if (isVoiceName(voice) && fileExists(voiceDir() + "/" + voice + ".wav"))
      {
        samples = readWavPcm(voiceDir() + "/" + voice + ".wav", frames, rate, channels);
      }
      if (samples.empty() || frames == 0)
      {
        // what the request carried decides the words it is answered with: a name
        // that stands for no kept recording, and bytes that are not a sound, are
        // different facts from a request that carried neither - a page repeats the
        // door's own words, and a person who named a recording that was just put
        // down or renamed learns that rather than being told nothing was given.
        std::string why = "no recording was given to listen to";
        if (!audioB64.empty())
          why = "the recording handed over could not be read as a sound";
        else if (!voice.empty())
          why = isVoiceName(voice) ? "no kept recording stands under that name: " + voice
                                   : "that name is not the shape of a kept recording: " + voice;
        noteError(id, why);
        markState(id, ols::kStateError);
        return;
      }
      if (cancelRequested(id))
      {
        markState(id, ols::kStateCancelled);
        return;
      }

      ensureRegistry();
      audiocpp_model *model = nullptr;
      {
        std::lock_guard<std::mutex> lock(gEngineMutex);
        const auto found = gModels.find(modelId);
        if (found != gModels.end())
          model = found->second;
      }
      if (!model)
      {
        std::string why = kNoMemoryWords;
        {
          std::lock_guard<std::mutex> lock(gEngineMutex);
          const auto refusal = gLoadErrors.find(modelId);
          if (refusal != gLoadErrors.end())
            why += ": " + refusal->second;
        }
        noteError(id, why);
        markState(id, ols::kStateError);
        return;
      }
      // The listening door names the kinds of work that answer about a
      // recording it is handed, in the order of what a page can show from
      // each: the words a model heard, who spoke and where, then the kinds
      // whose answer is the audio or the labels a model left. The first kind
      // the model declares, and this build can run, is the one asked for.
      std::vector<std::string> wanted = {"asr", "diar", "align", "vad",
                                         "spk", "cls", "wake", "turn", "sep", "vc", "s2s", "svc", "midi"};
      json sessionValues = json::object();
      const auto sessionGiven = request.find("session_options");
      if (sessionGiven != request.end() && !sessionGiven->is_object())
      {
        noteError(id, kRunControlWords);
        markState(id, ols::kStateError);
        return;
      }
      if (sessionGiven != request.end())
        sessionValues = *sessionGiven;
      audiocpp_session *session = sessionFor(modelId, wanted, sessionValues);
      if (!session)
      {
        const std::string detail = lastEngineError();
        // The engine states why in its own words when it can; when it cannot, name the
        // kinds this door asked for so a reader learns which of the two happened.
        noteError(id, detail.empty()
                          ? "no session could be made for this kind of work: this model or this build answers none of " + wantedWords(wanted)
                          : "no session could be made for this kind of work: " + detail);
        markState(id, ols::kStateError);
        return;
      }

      audiocpp_request *req = audiocpp_request_create();
      if (!req)
      {
        noteError(id, "the request could not be made");
        markState(id, ols::kStateError);
        return;
      }
      if (audiocpp_request_set_audio(req, samples.data(), frames, rate, channels) != AUDIOCPP_OK)
      {
        noteError(id, "this model does not listen to a recording: " + lastEngineError());
        audiocpp_request_free(req);
        markState(id, ols::kStateError);
        return;
      }
      if (request.contains("options") && request["options"].is_object())
      {
        bool refused = false;
        for (json::iterator it = request["options"].begin(); it != request["options"].end(); ++it)
        {
          const std::string name = it.key();
          const std::string asText = it->is_string() ? it->get<std::string>() : it->dump();
          if (audiocpp_request_set_option(req, name.c_str(), asText.c_str()) != AUDIOCPP_OK)
          {
            refused = true;
            break;
          }
        }
        if (refused)
        {
          noteError(id, "this model refused an option the page named: " + lastEngineError());
          audiocpp_request_free(req);
          markState(id, ols::kStateError);
          return;
        }
      }
      audiocpp_result *result = nullptr;
      const audiocpp_status ran = audiocpp_session_run(session, req, &result);
      audiocpp_request_free(req);
      if (ran != AUDIOCPP_OK || !result)
      {
        const std::string detail = lastEngineError();
        noteError(id, "the listen refused" + (detail.empty() ? std::string() : (": " + detail)));
        if (result)
          audiocpp_result_free(result);
        markState(id, ols::kStateError);
        return;
      }

      bool answered = false;
      const double seconds = static_cast<double>(frames) / static_cast<double>(rate > 0 ? rate : 1);
      setRecordField(id, "audio_s", seconds);

      const char *heard = nullptr;
      const char *heardLanguage = nullptr;
      if (audiocpp_result_text(result, &heard, &heardLanguage) == AUDIOCPP_OK && heard)
      {
        setRecordText(id, "words", std::string(heard));
        if (heardLanguage)
          setRecordText(id, "language", std::string(heardLanguage));
        answered = true;
      }

      const size_t segments = audiocpp_result_segment_count(result);
      if (segments > 0)
      {
        json out = json::array();
        for (size_t at = 0; at < segments; ++at)
        {
          int64_t startSample = 0;
          int64_t endSample = 0;
          float confidence = 0.f;
          const char *text = nullptr;
          if (audiocpp_result_segment(result, at, &startSample, &endSample, &confidence,
                                      &text) != AUDIOCPP_OK)
            continue;
          out.push_back({{"start_s", sampleSeconds(startSample, rate)},
                         {"end_s", sampleSeconds(endSample, rate)},
                         {"words", text ? std::string(text) : std::string()}});
        }
        if (!out.empty())
        {
          setRecordField(id, "segments", out);
          answered = true;
        }
      }

      const size_t turns = audiocpp_result_speaker_turn_count(result);
      if (turns > 0)
      {
        json out = json::array();
        for (size_t at = 0; at < turns; ++at)
        {
          int64_t startSample = 0;
          int64_t endSample = 0;
          float confidence = 0.f;
          const char *speaker = nullptr;
          const char *text = nullptr;
          if (audiocpp_result_speaker_turn(result, at, &startSample, &endSample, &speaker,
                                           &confidence, &text) != AUDIOCPP_OK)
            continue;
          out.push_back({{"start_s", sampleSeconds(startSample, rate)},
                         {"end_s", sampleSeconds(endSample, rate)},
                         {"speaker", speaker ? std::string(speaker) : std::string()},
                         {"words", text ? std::string(text) : std::string()}});
        }
        if (!out.empty())
        {
          setRecordField(id, "speaker_turns", out);
          answered = true;
        }
      }

      const size_t named = audiocpp_result_named_audio_count(result);
      if (named > 0)
      {
        json streams = json::array();
        std::error_code ec;
        if (!std::filesystem::is_directory(renderDir(), ec))
          std::filesystem::create_directories(renderDir(), ec);
        for (size_t at = 0; at < named; ++at)
        {
          const char *name = nullptr;
          const float *stream = nullptr;
          size_t streamFrames = 0;
          int streamRate = 0;
          int streamChannels = 0;
          if (audiocpp_result_named_audio(result, at, &name, &stream, &streamFrames,
                                          &streamRate, &streamChannels) != AUDIOCPP_OK)
            continue;
          if (!stream || !streamFrames)
            continue;
          const std::string safeName = safeFileName(name ? name : "stream");
          const std::string target = renderDir() + "/" + id + "-" + safeName + ".wav";
          if (!writeWavPcm(target, stream, streamFrames, streamRate, streamChannels))
            continue;
          streams.push_back({{"name", safeName},
                             {"url", "/voice/files/render/" + id + "-" + safeName},
                             {"audio_s", sampleSeconds(static_cast<long long>(streamFrames), streamRate)}});
        }
        if (!streams.empty())
        {
          setRecordField(id, "streams", streams);
          answered = true;
        }
      }

      const size_t artifactCount = audiocpp_result_artifact_count(result);
      if (artifactCount > 0)
      {
        json artifacts = json::array();
        std::error_code ec;
        if (!std::filesystem::is_directory(renderDir(), ec))
          std::filesystem::create_directories(renderDir(), ec);
        for (size_t at = 0; at < artifactCount; ++at)
        {
          audiocpp_artifact_kind kind = AUDIOCPP_ARTIFACT_CUSTOM;
          const char *artifactId = nullptr;
          const void *payload = nullptr;
          size_t payloadBytes = 0;
          if (audiocpp_result_artifact(result, at, &kind, &artifactId, &payload, &payloadBytes) !=
              AUDIOCPP_OK)
            continue;
          if (!payload || !payloadBytes)
            continue;
          const std::string kindName = artifactKindName(kind);
          const std::string safeId = safeFileName(artifactId ? artifactId : kindName);
          const std::string target =
              renderDir() + "/" + id + "-" + safeId + artifactExtension(kind);
          const auto *bytes = static_cast<const unsigned char *>(payload);
          std::string raw;
          raw.resize(payloadBytes);
          for (size_t at2 = 0; at2 < payloadBytes; ++at2)
            raw[at2] = static_cast<char>(bytes[at2]);
          if (!writeFileBytes(target, raw))
            continue;
          json meta = json::object();
          const size_t metaCount = audiocpp_result_artifact_meta_count(result, at);
          for (size_t at2 = 0; at2 < metaCount; ++at2)
          {
            const char *key = nullptr;
            const char *value = nullptr;
            if (audiocpp_result_artifact_meta(result, at, at2, &key, &value) != AUDIOCPP_OK)
              continue;
            if (key)
              meta[key] = value ? std::string(value) : std::string();
          }
          artifacts.push_back({{"kind", kindName},
                               {"name", safeId},
                               {"bytes", static_cast<long long>(payloadBytes)},
                               {"url", "/voice/files/render/" + id + "-" + safeId +
                                           artifactExtension(kind)},
                               {"meta", meta}});
        }
        if (!artifacts.empty())
        {
          setRecordField(id, "artifacts", artifacts);
          answered = true;
        }
      }

      const float *plain = nullptr;
      size_t plainFrames = 0;
      int plainRate = 0;
      int plainChannels = 0;
      if (audiocpp_result_audio(result, &plain, &plainFrames, &plainRate, &plainChannels) ==
              AUDIOCPP_OK &&
          plain && plainFrames)
      {
        std::error_code ec;
        if (!std::filesystem::is_directory(renderDir(), ec))
          std::filesystem::create_directories(renderDir(), ec);
        if (writeWavPcm(renderDir() + "/" + id + ".wav", plain, plainFrames, plainRate,
                        plainChannels))
        {
          setRecordText(id, "url", "/voice/files/render/" + id);
          setRecordField(id, "out_audio_s",
                         static_cast<double>(plainFrames) /
                             static_cast<double>(plainRate > 0 ? plainRate : 1));
          answered = true;
        }
      }

      audiocpp_result_free(result);
      if (!answered)
      {
        noteError(id, "that model answered with nothing this page can show");
        markState(id, ols::kStateError);
        return;
      }
      markState(id, ols::kStateDone);
    }

    static std::string startListenJob(const std::string &modelId, const std::string &bodyJson,
                                      const std::string &voiceName)
    {
      const std::string id = makeId("listen");
      startJobRecord(id, "listen", modelId, voiceName, std::string());
      std::thread([id, modelId, bodyJson]
                  { runJobGuarded(id, [&]
                                  { runListenJob(id, modelId, bodyJson); }); })
          .detach();
      pruneJobs();
      return id;
    }

    // A package's own files, put in place: every file its spec names is fetched

    // from the repo its own spec names, into that package's place in the weights
    // room. Nothing is invented here - a package that names no repo has nothing
    // to fetch, and the fetch answers which file failed.
    static void runDownloadJob(const std::string &id, const std::string &modelId)
    {
      const VoiceModelDef *def = nullptr;
      for (const VoiceModelDef &entry : gCardList)
        if (entry.id == modelId)
          def = &entry;
      if (!def || def->files.empty() || def->repo.empty())
      {
        noteError(id, "that package names no files to fetch");
        markState(id, ols::kStateError);
        return;
      }
      long long step = 0;
      for (const auto &named : def->files)
      {
        if (cancelRequested(id))
        {
          pushEvent(id, json{{"type", "cancelled"}}.dump());
          markState(id, ols::kStateCancelled);
          return;
        }
        const std::string full = packagePath(named);
        if (!fileExists(full))
        {
          pushEvent(id, json{{"type", "file"}, {"name", named}}.dump());
          const std::string url = "https://huggingface.co/" + def->repo + "/resolve/main/" + named;
          if (ols::platform::curlFetch(full, url, gHfToken) != 0)
          {
            noteError(id, "the fetch failed for " + named);
            markState(id, ols::kStateError);
            return;
          }
        }
        ++step;
        setRecordField(id, "done", step);
      }
      gModelsAnswerFresh = false;
      gHouseAnswerFresh = false;
      gWeightWalkDone = false;
      markState(id, ols::kStateDone);
    }

    static std::string startDownloadJob(const std::string &modelId)
    {
      const VoiceModelDef *def = nullptr;
      for (const VoiceModelDef &entry : gCardList)
        if (entry.id == modelId)
          def = &entry;
      if (!def || def->files.empty() || def->repo.empty())
        return std::string();
      const std::string id = makeId("fetch");
      startJobRecord(id, "download", modelId, std::string(), std::string());
      // a download counts its own files, so its window is real from the first
      // moment: none down yet, all of them to go.
      setRecordField(id, "done", static_cast<long long>(0));
      setRecordField(id, "max", static_cast<long long>(def->files.size()));
      std::thread([id, modelId]
                  { runJobGuarded(id, [&]
                                  { runDownloadJob(id, modelId); }); })
          .detach();
      pruneJobs();
      return id;
    }

    // how many kept renders a page is shown at once: the newest are what a page
    // can act on; a machine that kept more still holds every one of them, and
    // each keeps its own file door.
    static constexpr size_t kShownRenders = 200u;
    static std::string libraryListJson()
    {
      json out = json::array();
      std::error_code ec;
      if (std::filesystem::is_directory(renderDir(), ec))
      {
        for (const auto &item : std::filesystem::directory_iterator(renderDir(), ec))
        {
          if (item.path().extension().string() != ".wav")
            continue;
          const std::string fileName = item.path().filename().string();
          const std::string id = fileName.substr(0, fileName.size() - 4);
          long long bytes = 0;
          std::error_code szEc;
          const auto bytesValue = std::filesystem::file_size(item.path(), szEc);
          if (!szEc)
            bytes = static_cast<long long>(bytesValue);
          json row = {{"id", id},
                      {"wav", "/voice/files/render/" + id},
                      {"bytes", bytes}};
          {
            std::lock_guard<std::mutex> lock(gMutex);
            const auto found = gJobs.find(id);
            if (found != gJobs.end())
            {
              row["model"] = ols::statedString(found->second, "model");
              row["voice"] = ols::statedString(found->second, "voice");
              row["made_at"] = found->second.value("started_at", 0LL);
              // where the words of a kept render land, when the model said it:
              // the play door lights them against the audio it draws.
              if (found->second.contains("word_times"))
                row["word_times"] = found->second.at("word_times");
              if (found->second.contains("said_words"))
                row["said_words"] = found->second.at("said_words");
            }
          }
          if (!row.contains("model") || !row.contains("made_at"))
          {
            // a render made before this machine started has no record here, but
            // it kept its own story beside its file: the words, where they land,
            // and who made it come from that story rather than standing lost.
            std::error_code storyEc;
            const auto storyBytes = std::filesystem::file_size(renderDir() + "/" + id + ".json", storyEc);
            if (!storyEc && storyBytes > 0 && storyBytes <= 64u * 1024u * 1024u)
            {
              std::ifstream in(renderDir() + "/" + id + ".json", std::ios::binary);
              const std::string story((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
              const json kept = json::parse(story, nullptr, false);
              if (!kept.is_discarded() && kept.is_object())
              {
                row["model"] = ols::statedString(kept, "model");
                row["voice"] = ols::statedString(kept, "voice");
                row["made_at"] = kept.value("started_at", 0LL);
                if (kept.contains("word_times") && kept.at("word_times").is_array())
                  row["word_times"] = kept.at("word_times");
                if (kept.contains("said_words") && kept.at("said_words").is_string())
                  row["said_words"] = kept.at("said_words");
              }
            }
          }
          const double seconds = wavSeconds(item.path().string());
          if (seconds > 0.0)
            row["audio_s"] = seconds;
          out.push_back(row);
        }
      }
      std::sort(out.begin(), out.end(), [](const json &a, const json &b)
                {
        const long long atA = a.value("made_at", 0LL);
        const long long atB = b.value("made_at", 0LL);
        if (atA != atB)
          return atA > atB; // the newest kept render leads; those with no
        // record fall in after, ordered by name.
        return ols::statedString(a, "id") < ols::statedString(b, "id"); });
      // the newest renders are what a page can act on; a machine that kept more
      // than this still holds the rest, and each keeps its own file door.
      if (out.size() > kShownRenders)
        out.erase(out.begin() + kShownRenders, out.end());
      return out.dump();
    }

    static bool forgetRender(const std::string &id)
    {
      if (id.empty() || id.size() > 80)
        return false;
      if (!renderNameOk(id))
        return false;
      const std::string path = renderDir() + "/" + id + ".wav";
      if (!fileExists(path))
        return false;
      std::error_code ec;
      std::filesystem::remove(path, ec);
      std::error_code storyEc;
      std::filesystem::remove(renderDir() + "/" + id + ".json", storyEc);
      return !ec && !fileExists(path);
    }

    static bool serveFileOf(const std::string &kind, const std::string &name,
                            std::string &path, std::string &contentType)
    {
      if (name.empty() || name.size() > 96 || name.find('/') != std::string::npos)
        return false;
      if (kind == "render")
      {
        if (!renderNameOk(name))
          return false;
        path = renderDir() + "/" + name + ".wav";
      }
      else if (kind == "voice")
      {
        if (!isVoiceName(name))
          return false;
        path = voiceDir() + "/" + name + ".wav";
      }
      else
        return false;
      contentType = "audio/wav";
      return fileExists(path);
    }

    // ---- the member surface, thin over the file-static engine above ----

    VoiceService::VoiceService(const std::string &weightsDir, const std::string &dataDir, int32_t threads,
                               const std::string &hfToken)
        : mThreads(threads),
          mWeightsDir(weightsDir),
          mDataDir(dataDir)
    {
      gWeightsDir = weightsDir;
      gDataDir = dataDir;
      gThreads = threads > 0 ? threads : 1;
      gHfToken = hfToken;
    }

    bool VoiceService::known(const std::string &modelId) const { return knownModel(modelId); }

    std::string VoiceService::modelsJson() const { return modelsListJson(); }

    std::string VoiceService::modelsSummaryJson() const { return modelsHouseJson(); }

    std::string VoiceService::modelCardsJson(const std::string &query) const
    {
      return modelCardsPageJson(query);
    }

    std::string VoiceService::statusJson() const { return engineStateJson(); }

    std::string VoiceService::engineError() const { return engineErrorText(); }

    // The words this model's own last attempt gave, if it gave any. The engine
    // keeps a note per model - the model itself, and the sessions it could not
    // make - and a door that answers for one card repeats only that card's note:
    // the reason another model gave last says nothing about this one.
    std::string VoiceService::loadErrorFor(const std::string &modelId) const
    {
      std::lock_guard<std::mutex> lock(gEngineMutex);
      const std::string keys[] = {modelId, modelId + " (session)", modelId + " (session cpu)"};
      for (const std::string &key : keys)
      {
        const auto found = gLoadErrors.find(key);
        if (found != gLoadErrors.end())
          return found->second;
      }
      return std::string();
    }

    std::string VoiceService::startJob(const std::string &kind, const std::string &modelId,
                                       const std::string &bodyJson)
    {
      if (kind == "listen")
      {
        if (!knownModel(modelId))
          return std::string();
        const json request = json::parse(bodyJson, nullptr, false);
        if (request.is_discarded())
          return std::string();
        return startListenJob(modelId, bodyJson, request.value("voice", std::string()));
      }
      if (kind != "speak" || !knownModel(modelId))
        return std::string();
      const json request = json::parse(bodyJson, nullptr, false);
      if (request.is_discarded())
        return std::string();
      const std::string voice = request.value("voice", std::string());
      const std::string text = request.value("text", std::string());
      return startSpeak(modelId, bodyJson, voice, text);
    }

    std::string VoiceService::jobsListJson() const { return jobsListText(); }

    std::string VoiceService::jobStateJson(const std::string &id) const
    {
      return jobStateOf(id);
    }

    bool VoiceService::cancelJob(const std::string &id) { return cancelJobOf(id); }

    std::string VoiceService::voicesJson(const std::string &modelId) const
    {
      return voicesListJson(modelId);
    }

    bool VoiceService::saveVoice(const std::string &name, const std::string &wavB64,
                                 const std::string &promptText)
    {
      return storeVoice(name, wavB64, promptText);
    }

    std::string VoiceService::removeVoiceReason(const std::string &name)
    {
      return dropVoiceReason(name);
    }

    std::string VoiceService::renameVoice(const std::string &name, const std::string &wanted)
    {
      return keepVoiceUnder(name, wanted);
    }

    std::string VoiceService::libraryJson() const { return libraryListJson(); }

    bool VoiceService::removeRender(const std::string &id) { return forgetRender(id); }

    bool VoiceService::file(const std::string &kind, const std::string &name,
                            std::string &path, std::string &contentType) const
    {
      return serveFileOf(kind, name, path, contentType);
    }

    // A card the page asked for by name: its weights go into the engine, or its
    // memory is given back. This hand is the only way a model comes to stand -
    // nothing else reads a model's weights. The answer is the engine's own.
    bool VoiceService::loadModel(const std::string &modelId)
    {
      std::lock_guard<std::mutex> lock(gEngineMutex);
      if (!ensureRegistryLocked())
        return false;
      for (const VoiceModelDef &def : gCardList)
      {
        if (def.id != modelId)
          continue;
        const bool loaded = loadCard(def);
        if (loaded)
        {
          gModelsAnswerFresh = false;
          gHouseAnswerFresh = false;
        }
        return loaded;
      }
      return false;
    }

    bool VoiceService::unloadModel(const std::string &modelId)
    {
      std::lock_guard<std::mutex> lock(gEngineMutex);
      const bool put_down = unloadCard(modelId);
      if (put_down)
      {
        gModelsAnswerFresh = false;
        gHouseAnswerFresh = false;
      }
      return put_down;
    }

    std::string VoiceService::startDownload(const std::string &modelId)
    {
      if (!knownModel(modelId))
        return std::string();
      return startDownloadJob(modelId);
    }

  } // namespace voice
} // namespace ols
