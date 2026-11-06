// A small console entry that starts the linked voice engine and speaks one
// line, used by the release scripts to prove the engine really runs inside
// the shipped binary before anything gets deployed. The engine is audio.cpp
// linked in as its C API: nothing here opens a process or a port, and the one
// line is asked through the same doors a page uses - the model table, the job
// record, and the answer - so a clean answer here is the whole of the proof.
#include "VoiceService.h"
#include "Common.h"

#include <chrono>
#include <cstdlib>
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
  const int threads = argc > 5 ? std::atoi(argv[5]) : 4;
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
