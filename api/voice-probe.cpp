// A small console entry that starts the linked voice engine and speaks one
// line, used by the release scripts to prove the engine really runs inside
// the shipped binary before anything gets deployed. The engine is audio.cpp
// linked in as its C API: nothing here opens a process or a port, and the one
// line is asked through the same doors a page uses - the model table, the job
// record, and the answer - so a clean answer here is the whole of the proof.
#include "VoiceService.h"
#include "Common.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>

int main(int argc, char **argv)
{
  // The rooms the server is given, the card to answer with, and the line to
  // speak: the rooms are asked for in the words a person would type, and the
  // voice room is taken the way the server takes it, so what answers is the
  // engine that stands linked in this binary and not another copy of it.
  const std::string weightsRoom = argc > 1 ? argv[1] : std::string("voice-models");
  const std::string voiceRoom = argc > 2 ? argv[2] : std::string("voice");
  const std::string card = argc > 3 ? argv[3] : std::string();
  const std::string line = argc > 4 ? argv[4] : std::string("The voice engine speaks one line.");
  // The threads asked for, read as digits and nothing else: a word typed where a
  // number stands is no threads at all, and this probe is the proof a release
  // script depends on, so it answers with its usage rather than starting a
  // voice on a figure it made up.
  int threads = 4;
  if (argc > 5)
  {
    const std::string asked = argv[5];
    long long count = 0;
    bool digits = !asked.empty();
    for (const char c : asked)
    {
      if (c < '0' || c > '9' || count > 4096)
      {
        digits = false;
        break;
      }
      count = count * 10 + (c - '0');
    }
    if (!digits || count < 1 || count > 4096)
    {
      std::cout << "threads must be a count from 1 to 4096, not " << asked << std::endl;
      return 1;
    }
    threads = static_cast<int>(count);
  }
  if (card.empty())
  {
    std::cout << "usage: voice-probe[.exe] <weights room> <voice room> <model card> [line] [threads]"
              << std::endl;
    return 1;
  }

  ols::voice::VoiceService voices(weightsRoom + "/voice", voiceRoom + "/voice", threads,
                                  std::string());
  if (!voices.known(card))
  {
    std::cout << "the voice table holds no card named " << card << std::endl;
    return 1;
  }

  nlohmann::json ask;
  ask["text"] = line;
  const std::string job = voices.startJob("speak", card, ask.dump());
  if (job.empty())
  {
    const std::string why = voices.engineError();
    std::cout << "the speak door refused: " << (why.empty() ? "it said nothing" : why) << std::endl;
    return 1;
  }
  std::cout << "asked the linked engine to speak, on job " << job << std::endl;

  for (int wait = 0; wait < 1200; ++wait)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::string story = voices.jobStateJson(job);
    const nlohmann::json record = nlohmann::json::parse(story, nullptr, false);
    if (record.is_discarded())
      continue; // nothing was written down for this job yet
    const std::string state = ols::jobState(record);
    if (state == ols::kStateDone)
    {
      std::string renderPath, contentType;
      if (voices.file("render", job, renderPath, contentType))
        std::cout << "the engine answered, and its render stands at " << renderPath << std::endl;
      else
        std::cout << "the engine answered, and its render stands in the voice room" << std::endl;
      return 0;
    }
    if (state == ols::kStateError || state == ols::kStateCancelled)
    {
      const std::string why = voices.engineError();
      std::cout << "the engine refused: " << (why.empty() ? "its words were not carried" : why)
                << std::endl;
      return 1;
    }
  }
  std::cout << "the line did not come out while the probe watched" << std::endl;
  return 1;
}
