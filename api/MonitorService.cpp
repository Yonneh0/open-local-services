#include "MonitorService.h"
#include "Common.h"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ols
{
  namespace monitor
  {

    using json = nlohmann::json;

    static json parseList(const std::string &text)
    {
      const json parsed = json::parse(text, nullptr, false);
      return parsed.is_discarded() ? json::array() : parsed;
    }

    static void tagJobs(json &out, const json &list, const char *engine)
    {
      if (!list.is_array())
        return;
      for (const auto &entry : list)
      {
        if (!entry.is_object())
          continue;
        json record = entry;
        record["engine"] = engine;
        out.push_back(record);
      }
    }

    static long long wallclockSeconds(const json &record)
    {
      const long long started = record.value("started_at", 0LL);
      const long long ended = record.value("ended_at", 0LL);
      if (started <= 0 || ended < started)
        return 0;
      return ended - started;
    }

    // Totals for one engine across its kept jobs: how many ran, finished, and
    // failed, plus the work they put down (steps, blocks, speech seconds).
    // The page footer reads these totals directly; every count is exact, never
    // an estimate.
    static json engineSummary(const json &jobs, const std::string &engine)
    {
      int running = 0;
      int queued = 0;
      int done = 0;
      int failed = 0;
      long long latestEnd = 0;
      long long lastSeconds = 0;
      long long steps = 0;
      long long blocks = 0;
      long long audioCs = 0; // tenths of a second of speech, kept whole
      long long workMs = 0;
      long long firstAt = 0;
      for (const auto &record : jobs)
      {
        if (ols::statedString(record, "engine") != engine)
          continue;
        const std::string state = ols::jobState(record);
        if (state == ols::kStateRunning)
        {
          ++running;
          continue;
        }
        if (state == ols::kStateQueued)
        {
          ++queued;
          continue;
        }
        if (state == ols::kStateDone)
          ++done;
        if (state == ols::kStateError || state == ols::kStateCancelled)
          ++failed;
        const long long ended = record.value("ended_at", 0LL);
        if (ended >= latestEnd)
        {
          latestEnd = ended;
          lastSeconds = wallclockSeconds(record);
        }
        const long long started = record.value("started_at", 0LL);
        if (started > 0 && (firstAt == 0 || started < firstAt))
          firstAt = started;
        // How long the work itself took, when the engine stated it.
        workMs += ols::statedCount(record, "elapsed_ms");
        // What the job put to work, counted under the kind that names it.
        const std::string kind = ols::statedString(record, "kind");
        if (kind == "generate")
          steps += ols::statedCount(record, "max");
        if (kind == "synth")
        {
          blocks += ols::statedCount(record, "blocks");
          audioCs += static_cast<long long>(ols::statedMeasure(record, "audio_s") * 10.0 + 0.5);
        }
      }
      json out = {{"running", running},
                  {"queued", queued},
                  {"done", done},
                  {"failed", failed},
                  {"kept", running + queued + done + failed}};
      if (lastSeconds > 0)
        out["last_s"] = lastSeconds;
      if (firstAt > 0)
        out["first_at"] = firstAt;
      if (workMs > 0)
        out["work_ms"] = workMs;
      if (steps > 0)
        out["steps"] = steps;
      if (blocks > 0)
        out["blocks"] = blocks;
      if (audioCs > 0)
        out["audio_s"] = static_cast<double>(audioCs) / 10.0;
      return out;
    }

    // What the standing queue owes the waiting person: the seconds until every
    // job that runs or stands in line has finished. Each job is weighed by the
    // first yardstick that honestly names it - its own held pace, once it has
    // done measurable steps of its window; else what this machine states this
    // model has cost, from the settings ledger it keeps of its own finished
    // runs; else the average of the engine's own kept completions. A job none
    // of these reaches is counted unmeasured, and the answer that states no
    // number is preferred to one that guesses: left_s rides only for what was
    // weighed, and unmeasured rides beside it when some jobs were not.
    static json queueBlock(const json &jobs, long long now,
                           const std::function<std::string()> &timings)
    {
      int standing = 0;
      for (const auto &record : jobs)
      {
        if (ols::jobStanding(record))
          ++standing;
      }
      json out = {{"standing", standing}};
      if (standing == 0)
        return out;

      // What the engines' own kept completions say a job of theirs costs.
      long long imageMs = 0;
      int imageDone = 0;
      long long voiceMs = 0;
      int voiceDone = 0;
      for (const auto &record : jobs)
      {
        if (ols::jobState(record) != ols::kStateDone)
          continue;
        const long long ms = ols::statedCount(record, "elapsed_ms");
        if (ms <= 0)
          continue;
        if (ols::statedString(record, "engine") == "voice")
        {
          voiceMs += ms;
          ++voiceDone;
        }
        else
        {
          imageMs += ms;
          ++imageDone;
        }
      }

      // The picture engine's cost ledger, read only while a queue stands.
      json ledger = json::object();
      const json parsed = json::parse(timings(), nullptr, false);
      if (!parsed.is_discarded() && parsed.is_object())
        ledger = parsed;

      // What one run of one model costs, stated as the ledger's own weighted
      // mean across every row it kept for that model.
      const auto modelMeanSeconds = [&ledger](const char *ledgerKey, const std::string &model) -> long long
      {
        if (!ledger.contains(ledgerKey) || !ledger.at(ledgerKey).is_array())
          return 0;
        long long total = 0;
        long long count = 0;
        for (const auto &row : ledger.at(ledgerKey))
        {
          if (!row.is_object() || ols::statedString(row, "model") != model)
            continue;
          const long long at = ols::statedCount(row, "count");
          const long long ms = ols::statedCount(row, "total_ms");
          if (at <= 0 || ms <= 0)
            continue;
          total += ms;
          count += at;
        }
        return count > 0 ? total / count / 1000 : 0;
      };

      long long left = 0;
      int unmeasured = 0;
      for (const auto &record : jobs)
      {
        if (!ols::jobStanding(record))
          continue;
        const bool running = ols::jobState(record) == ols::kStateRunning;
        const long long started = ols::statedCount(record, "started_at");
        const long long held = (running && started > 0 && now > started) ? now - started : 0;
        const long long max = ols::statedCount(record, "max");
        const long long done = ols::statedCount(record, "done");
        long long seconds = -1;
        if (running && max > 0 && done >= 1 && done < max)
        {
          // A pace is a guess that scales the rest of a window by the time the
          // job has stood, and it is trusted no further than the machine's own
          // cost of a whole job: an engine that starts its count over inside
          // one window, or a wall clock that steps between two readings, can
          // otherwise make up hours this machine has never spent. A day is the
          // most any one job may claim toward the queue's time.
          const long long pace = (max - done) * held / done;
          seconds = pace > 86400 ? 86400 : pace;
        }
        else
        {
          const std::string kind = ols::statedString(record, "kind");
          const std::string model = ols::statedString(record, "model");
          long long average = 0;
          if (!model.empty() && (kind == "generate" || kind == "load"))
            average = modelMeanSeconds(kind == "load" ? "loads" : "generations", model);
          if (average <= 0)
          {
            const bool voice = ols::statedString(record, "engine") == "voice";
            const long long engineMs = voice ? voiceMs : imageMs;
            const long long engineDone = voice ? voiceDone : imageDone;
            if (engineDone > 0)
              average = engineMs / engineDone / 1000;
          }
          if (average > 0)
            seconds = running && average > held ? average - held : (running ? 0 : average);
        }
        if (seconds < 0)
        {
          ++unmeasured;
          continue;
        }
        left += seconds;
      }
      if (unmeasured > 0)
        out["unmeasured"] = unmeasured;
      if (unmeasured < standing)
        out["left_s"] = left;
      return out;
    }

    // What this status document remembers across its readings: the level each
    // pool has reached over time, so the page can draw the memory line, not
    // just its current point. This is the monitor's own record of its own
    // readings; the engines keep their jobs, and nothing here is a fact about
    // a job. Samples are taken every kHistoryStepSeconds and only kHistoryPoints
    // are kept, so the cost stays a few kilobytes regardless of uptime.
    constexpr int kHistoryPoints = 120;
    constexpr long long kHistoryStepSeconds = 2;
    static std::mutex gHistoryMutex;
    static std::deque<json> gHistory;
    static std::vector<std::string> gDeviceNames;
    static std::vector<std::string> gModelNames;
    static long long gLastSampleAt = 0;

    // One device pool as this machine reports it. A device that states no
    // byte counts has nothing to say and is left out, not represented as zero.
    static void poolOfDevice(const json &device, long long &used, bool &known)
    {
      used = 0;
      known = false;
      if (!device.is_object() || !device.contains("total_bytes"))
        return;
      const long long total = ols::statedCount(device, "total_bytes");
      const long long free = ols::statedCount(device, "free_bytes");
      if (total <= 0)
        return;
      used = (free > total) ? 0 : (total - free);
      known = true;
    }

    // One sample: the moment, the device pools in the order their names are
    // stated beside the samples, the machine's memory, what the engines hold
    // open, and how many jobs were in motion at that moment.
    static json sampleOf(const json &system, int running)
    {
      const long long at = nowSeconds();
      json pools = json::array();
      // Devices are ordered by first appearance so the arrays beside the names
      // line up across samples, even if a driver starts answering late.
      for (const auto &name : gDeviceNames)
      {
        long long used = 0;
        bool known = false;
        if (system.contains("devices") && system.at("devices").is_array())
        {
          for (const auto &device : system.at("devices"))
          {
            if (ols::statedString(device, "name") == name)
            {
              poolOfDevice(device, used, known);
              break;
            }
          }
        }
        if (known)
          pools.push_back(used);
        else
          pools.push_back(nullptr);
      }
      json ram = nullptr;
      const json memory = system.value("memory", json::object());
      if (memory.is_object() && memory.contains("total_bytes"))
      {
        const long long total = ols::statedCount(memory, "total_bytes");
        const long long free = ols::statedCount(memory, "free_bytes");
        if (total > 0)
          ram = json{total, (free > total) ? 0LL : (total - free)};
      }
      const json loaded = system.value("loaded", json::object());
      const long long heldBytes = loaded.is_object() ? ols::statedCount(loaded, "resident_bytes") : 0;
      // How many models the engines hold open at this moment: the picture
      // engine names its own, and a model loaded in the linked voice engine is
      // open with no port to name. All of them are open models, and this page's
      // own count is the same sum, so the line and the number describe one set.
      int openModels = 0;
      if (loaded.is_object() && loaded.contains("loaded") && loaded.at("loaded").is_array())
        openModels = static_cast<int>(loaded.at("loaded").size());
      const json voiceState = system.value("voice", json::object());
      if (voiceState.is_object() && voiceState.contains("open_models") && voiceState.at("open_models").is_array())
        openModels += static_cast<int>(voiceState.at("open_models").size());
      // What each model holds of the machine at this moment, in the order its
      // name is stated beside the samples. A model that is not open now holds
      // nothing to say about itself and stays a gap, not a zero.
      json held = json::array();
      for (const auto &name : gModelNames)
      {
        const json *item = nullptr;
        if (loaded.is_object() && loaded.contains("resident") && loaded.at("resident").is_array())
        {
          for (const auto &entry : loaded.at("resident"))
          {
            if (ols::statedString(entry, "model") == name)
            {
              item = &entry;
              break;
            }
          }
        }
        if (item && item->contains("bytes"))
          held.push_back(ols::statedCount(*item, "bytes"));
        else
          held.push_back(nullptr);
      }
      // Disk totals and CPU load are kept the same way: a number that is not
      // stated stays a gap in the line, never a zero.
      json disk = nullptr;
      const json diskJson = system.value("disk", json::object());
      if (diskJson.is_object() && diskJson.contains("total_bytes"))
      {
        const long long total = ols::statedCount(diskJson, "total_bytes");
        const long long free = ols::statedCount(diskJson, "free_bytes");
        if (total > 0)
          disk = json{total, (free > total) ? 0LL : (total - free)};
      }
      json load = nullptr;
      const json cpu = system.value("cpu", json::object());
      // A machine that states no load average (Windows keeps none) keeps this a
      // gap in the line, never a zero; every part is checked before it is read.
      if (cpu.is_object() && cpu.contains("load") && cpu.at("load").is_object() &&
          cpu.at("load").contains("one") && cpu.at("load").at("one").is_number())
        load = cpu.at("load").at("one");
      // What the kernel says of each thread of the processor, kept in thread
      // order, so one thread keeps one row across samples the way a device does;
      // a thread that states nothing between two readings stays a gap.
      json threads = nullptr;
      if (cpu.is_object() && cpu.contains("threads_load") && cpu.at("threads_load").is_array())
      {
        json shares = json::array();
        for (const auto &share : cpu.at("threads_load"))
          shares.push_back(share.is_number() ? share : nullptr);
        threads = shares;
      }
      return json{{"t", at},
                  {"p", pools},
                  {"ram", ram},
                  {"m", held},
                  {"open", openModels},
                  {"bytes", heldBytes},
                  {"run", running},
                  {"disk", disk},
                  {"load", load},
                  {"th", threads}};
    }

    static void rememberSample(const json &system, int running)
    {
      const long long at = nowSeconds();
      std::lock_guard<std::mutex> lock(gHistoryMutex);
      if (at - gLastSampleAt < kHistoryStepSeconds)
        return;
      // The names the sample arrays follow, kept in first-seen order.
      if (system.contains("devices") && system.at("devices").is_array())
      {
        for (const auto &device : system.at("devices"))
        {
          const std::string name = ols::statedString(device, "name");
          if (name.empty())
            continue;
          if (std::find(gDeviceNames.begin(), gDeviceNames.end(), name) == gDeviceNames.end())
            gDeviceNames.push_back(name);
        }
      }
      // Models the image engine holds open join the same first-seen order, so
      // a model's held-bytes row stays the same row across samples.
      if (system.contains("loaded") && system.at("loaded").is_object() &&
          system.at("loaded").contains("resident") && system.at("loaded").at("resident").is_array())
      {
        for (const auto &entry : system.at("loaded").at("resident"))
        {
          const std::string name = ols::statedString(entry, "model");
          if (name.empty())
            continue;
          if (std::find(gModelNames.begin(), gModelNames.end(), name) == gModelNames.end())
            gModelNames.push_back(name);
        }
      }
      gHistory.push_back(sampleOf(system, running));
      while (static_cast<int>(gHistory.size()) > kHistoryPoints)
        gHistory.pop_front();
      gLastSampleAt = at;
    }

    static json drawHistory()
    {
      std::lock_guard<std::mutex> lock(gHistoryMutex);
      json samples = json::array();
      for (const auto &sample : gHistory)
        samples.push_back(sample);
      return json{{"devices", gDeviceNames}, {"models", gModelNames}, {"samples", samples}};
    }

    // The rows a model's card draws: that engine's jobs, newest first, each
    // carrying only the numbers its own work states - steps for a picture,
    // blocks and speech seconds for a render. Unstated numbers stay out, not
    // zero. The names a record states its numbers under are kept as they are,
    // so a page reads one name for one fact.
    constexpr int kRunsKept = 60;
    static void noteRun(std::vector<json> &runs, const json &record)
    {
      // Jobs arrive newest first, so the first kRunsKept are the card's rows.
      if (static_cast<int>(runs.size()) >= kRunsKept)
        return;
      json run = {{"id", ols::statedString(record, "id")},
                  {"t", record.value("started_at", 0LL)},
                  {"kind", ols::statedString(record, "kind")},
                  {"state", ols::jobState(record)}};
      const long long elapsed = ols::statedCount(record, "elapsed_ms");
      if (elapsed > 0)
        run["elapsed_ms"] = elapsed;
      if (record.contains("done"))
        run["done"] = record.at("done");
      if (record.contains("max"))
        run["max"] = record.at("max");
      for (const char *key : {"audio_s",
                              "blocks",
                              "voice",
                              "port",
                              "error",
                              "url",
                              "queue_pos",
                              "bytes",
                              "total_bytes",
                              "speed",
                              "avg",
                              "file"})
      {
        if (record.contains(key) && (record.at(key).is_number() || record.at(key).is_string()))
          run[key] = record.at(key);
      }
      // What the job was asked with, so a standing line on a card can say what
      // it stands for before any record door is asked: the ask snapshot the job
      // was filed with, or the settings it wrote when its run took the card.
      // The words are clipped here - a card line is not the room.
      const json *source = nullptr;
      const auto ask = record.find("ask");
      if (ask != record.end() && ask->is_object())
        source = &(*ask);
      else
      {
        const auto settings = record.find("settings");
        if (settings != record.end() && settings->is_object())
          source = &(*settings);
      }
      if (source != nullptr)
      {
        const std::string prompt = source->value("prompt", std::string());
        if (!prompt.empty())
          run["prompt"] = prompt.size() > 200 ? prompt.substr(0, 200) : prompt;
        for (const char *key : {"width", "height", "steps"})
          if (source->contains(key) && source->at(key).is_number())
            run[key] = source->at(key);
      }
      if (source != nullptr && source->contains("ref_urls") && (*source)["ref_urls"].is_array())
      {
        json urls = json::array();
        for (const auto &one : (*source)["ref_urls"])
        {
          if (urls.size() >= 4)
            break;
          if (one.is_string())
            urls.push_back(one);
        }
        if (!urls.empty())
          run["ref_urls"] = urls;
      }
      runs.push_back(run);
    }

    // What a model is doing now, in its own engine's words: how many of its
    // bytes are in memory, and whether a job of its kind is in motion at this
    // moment.
    struct ModelRow
    {
      std::string engine;
      std::string model;
      bool open = false;
      long long residentBytes = -1; // unknown until an engine states it
      std::string quant;           // the quant an open picture model stands as, when stated
      std::vector<json> runs;
      long long started = 0;
    };

    static std::map<std::string, ModelRow> rowsOf(const json &jobs, const json &system)
    {
      std::map<std::string, ModelRow> rows;
      auto rowFor = [&rows](const std::string &engine, const std::string &model) -> ModelRow &
      {
        const std::string key = engine + "\t" + model;
        const auto found = rows.find(key);
        if (found != rows.end())
          return found->second;
        ModelRow &row = rows[key];
        row.engine = engine;
        row.model = model;
        return row;
      };
      for (const auto &record : jobs)
      {
        const std::string engine = ols::statedString(record, "engine");
        if (engine.empty())
          continue;
        const std::string model = ols::statedString(record, "model");
        if (model.empty())
          continue;
        ModelRow &row = rowFor(engine, model);
        const long long started = record.value("started_at", 0LL);
        if (started > row.started)
          row.started = started;
        noteRun(row.runs, record);
      }
      // A model an engine holds open gets a card even with no jobs kept about
      // it, because memory occupancy is a fact the page states.
      const json loaded = system.value("loaded", json::object());
      if (loaded.is_object())
      {
        if (loaded.contains("resident") && loaded.at("resident").is_array())
        {
          for (const auto &item : loaded.at("resident"))
          {
            const std::string model = ols::statedString(item, "model");
            if (model.empty())
              continue;
            ModelRow &row = rowFor("image", model);
            row.open = true;
            row.residentBytes = ols::statedCount(item, "bytes");
            const std::string quant = ols::statedString(item, "quant");
            if (!quant.empty())
              row.quant = quant;
          }
        }
        if (loaded.contains("loaded") && loaded.at("loaded").is_array())
        {
          for (const auto &model : loaded.at("loaded"))
          {
            if (!model.is_string())
              continue;
            ModelRow &row = rowFor("image", model.get<std::string>());
            row.open = true;
          }
        }
      }
      const json voice = system.value("voice", json::object());
      if (voice.is_object() && voice.contains("open_models") && voice.at("open_models").is_array())
      {
        for (const auto &id : voice.at("open_models"))
        {
          if (!id.is_string())
            continue;
          const std::string model = id.get<std::string>();
          if (model.empty())
            continue;
          ModelRow &row = rowFor("voice", model);
          row.open = true;
        }
      }
      return rows;
    }

    static json drawModels(const std::map<std::string, ModelRow> &rows)
    {
      json out = json::array();
      for (const auto &entry : rows)
      {
        const ModelRow &row = entry.second;
        int running = 0;
        int queued = 0;
        int done = 0;
        int failed = 0;
        json runs = json::array();
        for (const auto &run : row.runs)
        {
          const std::string state = ols::jobState(run);
          if (state == ols::kStateRunning)
            ++running;
          else if (state == ols::kStateQueued)
            ++queued;
          else if (state == ols::kStateDone)
            ++done;
          else
            ++failed;
          runs.push_back(run);
        }
        json model = {{"engine", row.engine},
                      {"model", row.model},
                      {"open", row.open},
                      {"kept", static_cast<int>(row.runs.size())},
                      {"running", running},
                      {"queued", queued},
                      {"done", done},
                      {"failed", failed},
                      {"runs", runs}};
        if (row.residentBytes >= 0)
          model["resident_bytes"] = row.residentBytes;
        if (!row.quant.empty())
          model["quant"] = row.quant;
        if (row.started != 0)
          model["last_started_at"] = row.started;
        out.push_back(model);
      }
      // Open models first, then most recently used: memory occupancy is what a
      // reader looks for before anything else.
      std::sort(out.begin(), out.end(), [](const json &a, const json &b)
                {
                  const bool aOpen = a.value("open", false);
                  const bool bOpen = b.value("open", false);
                  if (aOpen != bOpen)
                    return aOpen;
                  return a.value("last_started_at", 0LL) > b.value("last_started_at", 0LL); });
      return out;
    }

    std::string snapshot(const std::string &systemJson, const std::string &imageJobsJson,
                         const std::string &voiceJobsJson,
                         const std::function<std::string()> &timings)
    {
      json system = json::parse(systemJson, nullptr, false);
      if (system.is_discarded())
        system = json::object();

      // The engines keep their own jobs: a picture and a render are the same
      // kind of work, so both ride in one list.
      json jobs = json::array();
      tagJobs(jobs, parseList(imageJobsJson), "image");
      tagJobs(jobs, parseList(voiceJobsJson), "voice");
      std::sort(jobs.begin(), jobs.end(), [](const json &a, const json &b)
                { return a.value("started_at", 0LL) > b.value("started_at", 0LL); });

      int active = 0;
      for (const auto &record : jobs)
      {
        if (ols::jobStanding(record))
          ++active;
      }

      json summary = {{"retained", static_cast<int>(jobs.size())},
                      {"active", active},
                      {"image", engineSummary(jobs, "image")},
                      {"voice", engineSummary(jobs, "voice")}};

      rememberSample(system, active);

      json out = {{"updated", nowSeconds()},
                  {"system", system},
                  {"jobs", jobs},
                  {"summary", summary},
                  {"queue", queueBlock(jobs, nowSeconds(), timings)},
                  {"history", drawHistory()},
                  {"models", drawModels(rowsOf(jobs, system))}};
      return out.dump();
    }

  } // namespace monitor
} // namespace ols
