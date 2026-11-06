// The pages this binary carries inside: webui-embedded.cpp, written by the api
// build from the tree's webui folder, holds the names and the bytes; this file
// only reads them and puts a file back together from the pieces it was stored
// in.
#include "WebAssets.h"
#include "Common.h"

#include <filesystem>
#include <string>

namespace ols
{
  namespace webres
  {
    int embeddedCount()
    {
      return kEmbeddedCount > 0 ? kEmbeddedCount : 0;
    }

    bool embeddedAt(int index, std::string &name, std::string &bytes)
    {
      if (index < 0 || index >= embeddedCount())
        return false;
      name = kEmbeddedNames[index];
      bytes.clear();
      const char *const *const parts = kEmbeddedParts[index];
      if (parts == nullptr)
        return false;
      for (const char *const *part = parts; *part != nullptr; ++part)
        bytes += *part;
      return true;
    }

    bool embeddedNamed(const std::string &wanted, std::string &bytes)
    {
      for (int i = 0; i < embeddedCount(); ++i)
      {
        if (wanted != kEmbeddedNames[i])
          continue;
        std::string ignoredName;
        return embeddedAt(i, ignoredName, bytes);
      }
      return false;
    }

    int pageNamed(const std::string &dir, const std::string &name, std::string &bytes)
    {
      std::string outside;
      if (ols::readFileText((std::filesystem::path(dir) / name).string(), outside) && !outside.empty())
      {
        bytes = outside;
        return 2;
      }
      bytes.clear();
      return embeddedNamed(name, bytes) ? 1 : 0;
    }

  } // namespace webres
} // namespace ols
