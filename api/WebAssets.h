#pragma once
// What the binary carries inside it of the pages: the api build reads the tree's
// webui folder whole when it configures, and those bytes are compiled into the
// server. At run time the server answers from the folder outside when that
// folder is there - a bool set once when the server is pointed at its pages,
// and turned on by the expand door; nothing watches the folder while the server
// runs - and falls back to the bytes here whenever an outside read comes up
// short, so a released .exe stands on its own.
#ifndef OLS_WEBASSETS_H
#define OLS_WEBASSETS_H

#include <string>

namespace ols
{
  namespace webres
  {
    // defined by the build in webui-embedded.cpp: the names inside the webui
    // folder, and for each one the pieces its bytes were compiled as.
    extern const char *const kEmbeddedNames[];
    extern const char *const *const kEmbeddedParts[];
    extern const int kEmbeddedCount;

    // how many files this binary carries
    int embeddedCount();
    // the file at index, whole: its name as seen inside the webui folder and
    // its bytes. false when the index is out of range.
    bool embeddedAt(int index, std::string &name, std::string &bytes);
    // the bytes of one named file. false when this binary carries no such name.
    bool embeddedNamed(const std::string &name, std::string &bytes);
    // the current copy of a file that sits beside the pages: the outside copy
    // when it holds bytes, the copy carried inside otherwise. 0 when neither
    // has it, 1 when the bytes came from inside this binary, 2 when they came
    // from the file beside the pages.
    int pageNamed(const std::string &dir, const std::string &name, std::string &bytes);
  }
}

#endif // OLS_WEBASSETS_H
