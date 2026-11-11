// Voice service module: the models table, the voice library, the job records,
// and the kept renders. The voice engine is audio.cpp, built as its C API and
// linked into this binary - there is no separate engine process and nothing to
// open or close: the models answer through handles inside this process, and
// what a model can do is read off its own handle while it stands loaded.
#ifndef OLS_VOICE_VOICESERVICE_H
#define OLS_VOICE_VOICESERVICE_H

#include <cstdint>
#include <string>

namespace ols
{
  namespace voice
  {

    class VoiceService
    {
    public:
      VoiceService(const std::string &weightsDir, const std::string &dataDir, int32_t threads,
                   const std::string &hfToken);

      bool known(const std::string &modelId) const;
      std::string modelsJson() const;

      // The same cards, asked for a page of them: the house answer that counts
      // what this machine can offer (its packages, its families, the tasks and
      // languages those families name), and one page of cards narrowed by the
      // words of the question - family, task, state, and free text - in the
      // order asked, from the offset asked, up to the limit asked. A page that
      // draws 400 cards one at a time asks for pages, never for the whole
      // table, and the counts it shows are this answer's, not its own guess.
      std::string modelsSummaryJson() const;
      std::string modelCardsJson(const std::string &query) const;
      std::string statusJson() const;
      std::string engineError() const;

      std::string startJob(const std::string &kind, const std::string &modelId,
                           const std::string &bodyJson);
      std::string jobsListJson() const;
      std::string jobStateJson(const std::string &id) const;
      bool cancelJob(const std::string &id);

      std::string voicesJson(const std::string &modelId) const;
      bool saveVoice(const std::string &name, const std::string &wavB64,
                     const std::string &promptText);
      // Letting a recording go answers with nothing when it went, and with the
      // reason it did not otherwise: which name could not be used, or which name
      // holds no recording. A person hears the difference between a name they
      // mistyped and a recording that was already let go.
      std::string removeVoiceReason(const std::string &name);
      // The name move answers with nothing when it moved the recording, and with
      // the reason it did not otherwise: the reason says which of the names was
      // unusable, which recording was not there, or which name was already taken.
      std::string renameVoice(const std::string &name, const std::string &wanted);

      std::string libraryJson() const;
      bool removeRender(const std::string &id);

      // One card asked for by name: its weights into the engine, or its memory
      // back. Both answer the engine's own yes or no.
      bool loadModel(const std::string &modelId);
      bool unloadModel(const std::string &modelId);

      // Why this model's own last attempt refused, in the engine's words, or
      // empty when nothing was recorded for it: a door that answers for one card
      // must not repeat whatever model failed last.
      std::string loadErrorFor(const std::string &modelId) const;

      // A card whose files are not on this machine yet: its package's own files
      // are fetched into its place in the weights room, and the fetch rides a
      // job record. Answers the job's id, or empty when there is nothing to
      // fetch.
      std::string startDownload(const std::string &modelId);

      // Resolves one of this service's files for the shared file server:
      // "render" is a kept render wav, "voice" a saved voice recording.
      // Answers with the full path and content type.
      bool file(const std::string &kind, const std::string &name,
                std::string &path, std::string &contentType) const;

    private:
      // What the module was pointed at when it was made; nothing outside the
      // module reads or writes these.
      int32_t mThreads;
      std::string mWeightsDir;
      std::string mDataDir;
    };

  } // namespace voice
} // namespace ols

#endif // OLS_VOICE_VOICESERVICE_H
