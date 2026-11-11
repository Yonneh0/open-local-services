// Image service module: the image studio's engine, models, jobs, and files.
#ifndef OLS_IMAGEGEN_IMAGESERVICE_H
#define OLS_IMAGEGEN_IMAGESERVICE_H

#include <cstdint>
#include <string>

namespace ols
{
  namespace imagegen
  {

    class ImageService
    {
    public:
      // keptMax is how many of this machine's own renders stand on disk: the newest
      // that many, which is also the deepest a gallery page can be asked for. Zero
      // or less leaves the module's own default standing.
      ImageService(const std::string &weightsDir, const std::string &outputDir, int32_t threads,
                   const std::string &hfToken, const std::string &civitaiKey, int32_t keptMax);

      std::string modelsJson() const;
      std::string statusJson() const;
      std::string devicesJson() const;
      bool known(const std::string &modelId) const;

      std::string startJob(const std::string &kind, const std::string &modelId,
                           const std::string &payloadJson);
      bool unload(const std::string &modelId);
      std::string prefsJson() const;
      bool setPrefs(const std::string &bodyJson);
      void setHfToken(const std::string &token);
      void setCivitaiKey(const std::string &token);

      std::string jobsListJson() const;
      std::string jobStateJson(const std::string &id) const;
      bool cancelJob(const std::string &id);
      void streamEvents(void *client, const std::string &jobId);

      // What this machine's own work costs: every set of settings it has run, the
      // times those runs took, and what one step or one picture costs at them. A
      // page that wants to say what the next ask will cost reads this rather than
      // guessing from a model's size.
      std::string timingsJson() const;

      // One window of the kept renders, newest first: how many to take and how
      // many to start past. The answer states the window it served and how many
      // records the machine holds in all.
      std::string galleryJson(int limit, int offset) const;
      std::string galleryItemJson(const std::string &id) const;
      bool removeOutput(const std::string &id);
      std::string refsJson() const;
      bool addRef(const std::string &source, const std::string &name);
      bool addRefBytes(const std::string &name, const std::string &imageB64,
                       const std::string &format);
      std::string lastRefError() const;
      bool removeRef(const std::string &id);
      bool file(const std::string &kind, const std::string &name, std::string &path,
                std::string &contentType) const;
      std::string engineError() const;

    private:
      // What the module was pointed at when it was made. Nothing outside the
      // module reads or writes these, and the two keys are written only through
      // the setters above, which stand a guard over them while a job is running.
      int32_t mThreads;
      std::string mWeightsDir;
      std::string mOutputDir;
      std::string mHfToken;
      std::string mCivitaiKey;
      int32_t mKeptMax;
    };

  } // namespace imagegen
} // namespace ols

#endif // OLS_IMAGEGEN_IMAGESERVICE_H
