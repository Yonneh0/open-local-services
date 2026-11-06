#pragma once
// The shapes an answer may be asked to come in, and the reading that takes
// those words out of what a model answered. A model that was asked for one run
// of words sometimes answers with it split over lines, wrapped in quotes, put
// behind a bullet, or fenced as a code block: the shape says which of those
// marks belong to the answer's layout rather than to its words, and this takes
// them off. An answer asked for as a run of labelled fields may carry the same
// marks and be wrapped in sentences: only the lines that name a field are
// fields, and the first line that names one is what stands. A shape that is not
// one the machine names asks nothing of the answer, and an answer that comes
// out with no words in it after the reading says so - a shape never invents
// words a model did not write.
#ifndef OLS_ANSWER_SHAPE_H
#define OLS_ANSWER_SHAPE_H

#include <string>
#include <vector>

namespace ols
{
  // One run of words, as a prompt is written: one line, with the marks a model
  // used to lay its answer out taken off.
  inline constexpr const char *kShapeOnePrompt = "one-prompt";

  // A run of labelled fields: one thing per line, each line naming what it
  // states.
  inline constexpr const char *kShapeLabelledFields = "labelled-fields";

  // One tool call: the same labelled reading, read by the words a call is
  // written in - the thing named on a line of its own, and each argument the
  // call needs named under "arg".
  inline constexpr const char *kShapeToolCall = "tool-call";

  namespace
  {
    inline bool isSpace(char c)
    {
      return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    // How many characters the layout mark at the front of a line is, or nought
    // when none is there. A bullet, and a number followed by a dot or a
    // bracket, are marks; a number that is part of a word is not.
    inline size_t layoutMarkAt(const std::string &line, size_t at)
    {
      if (at >= line.size())
        return 0;
      const unsigned char c = static_cast<unsigned char>(line[at]);
      if (c == '-' || c == '*' || c == '+')
        return at + 1 < line.size() && isSpace(line[at + 1]) ? 2 : 0;
      // one of the characters the list bullets are written with
      if (c == 0xE2 && at + 2 < line.size() && static_cast<unsigned char>(line[at + 1]) == 0x80)
      {
        const unsigned char last = static_cast<unsigned char>(line[at + 2]);
        if (last >= 0xA0 && last <= 0xAF)
          return at + 3 < line.size() && isSpace(line[at + 3]) ? 4 : 0;
      }
      size_t digits = 0;
      while (at + digits < line.size() && line[at + digits] >= '0' && line[at + digits] <= '9')
        ++digits;
      if (digits == 0 || digits > 3)
        return 0;
      const char after = line[at + digits];
      if ((after == '.' || after == ')') && at + digits + 1 < line.size() &&
          isSpace(line[at + digits + 1]))
        return digits + 2;
      return 0;
    }

    // How many characters a quote that opens this line's words is, or nought.
    inline size_t quoteOpenAt(const std::string &text, size_t at)
    {
      if (at >= text.size())
        return 0;
      const unsigned char c = static_cast<unsigned char>(text[at]);
      if (c == '"' || c == '\'')
        return 1;
      if (c != 0xE2 || at + 2 >= text.size())
        return 0;
      const unsigned char mid = static_cast<unsigned char>(text[at + 1]);
      const unsigned char last = static_cast<unsigned char>(text[at + 2]);
      if ((mid == 0x9C || mid == 0x80) &&
          (last == 0x98 || last == 0x99 || last == 0x9C || last == 0x9D))
        return 3;
      return 0;
    }

    // How many characters a quote that closes at this place is, or nought.
    inline size_t quoteCloseBefore(const std::string &text, size_t end)
    {
      if (end == 0)
        return 0;
      const unsigned char c = static_cast<unsigned char>(text[end - 1]);
      if (c == '"' || c == '\'')
        return 1;
      if (c != 0x9D && c != 0x99)
        return 0;
      if (end < 3)
        return 0;
      const unsigned char mid = static_cast<unsigned char>(text[end - 2]);
      const unsigned char first = static_cast<unsigned char>(text[end - 3]);
      if (first == 0xE2 && (mid == 0x9C || mid == 0x80))
        return 3;
      return 0;
    }

    // The words of one line with the marks a model used to lay it out taken
    // off: a bullet or a number that leads the line, quotes wrapped around the
    // whole line, and runs of blanks inside it. A fence line is layout, so it
    // leaves no words. Words are returned in place, with nothing added.
    inline std::string cleanLine(const std::string &line)
    {
      size_t lead = 0;
      while (lead < line.size() && isSpace(line[lead]))
        ++lead;
      if (lead + 3 <= line.size() && line[lead] == '`' && line[lead + 1] == '`' && line[lead + 2] == '`')
        return std::string();
      lead += layoutMarkAt(line, lead);
      size_t tail = line.size();
      while (tail > lead && isSpace(line[tail - 1]))
        --tail;
      const size_t open = quoteOpenAt(line, lead);
      const size_t close = quoteCloseBefore(line, tail);
      if (open > 0 && close > 0 && tail - lead > open + close)
      {
        lead += open;
        tail -= close;
      }
      std::string words;
      bool pending = false;
      for (size_t i = lead; i < tail; ++i)
      {
        if (!isSpace(line[i]))
        {
          if (pending && !words.empty())
            words += ' ';
          pending = false;
          words.push_back(line[i]);
          continue;
        }
        pending = !words.empty();
      }
      return words;
    }

    // A line that names a field, taken apart: the name it states in lower case
    // with one blank between its words, and the words after it. A name is a
    // short run of letters; nothing else is a field.
    inline bool fieldAtLine(const std::string &line, std::string &name, std::string &value)
    {
      size_t at = 0;
      while (at < line.size() && isSpace(line[at]))
        ++at;
      std::string letters;
      bool pendingBlank = false;
      while (at < line.size())
      {
        const char c = line[at];
        if (c >= 'A' && c <= 'Z')
        {
          if (pendingBlank && !letters.empty())
            letters.push_back(' ');
          pendingBlank = false;
          letters.push_back(static_cast<char>(c - 'A' + 'a'));
          ++at;
          continue;
        }
        if (c >= 'a' && c <= 'z')
        {
          if (pendingBlank && !letters.empty())
            letters.push_back(' ');
          pendingBlank = false;
          letters.push_back(c);
          ++at;
          continue;
        }
        if (c == ' ' || c == '\t')
        {
          pendingBlank = !letters.empty();
          ++at;
          continue;
        }
        break;
      }
      if (letters.empty() || letters.size() > 24)
        return false;
      if (at >= line.size() || line[at] != ':')
        return false;
      ++at;
      std::string rest;
      while (at < line.size())
      {
        rest.push_back(line[at]);
        ++at;
      }
      // A field asked for with nothing after its name is a field the model
      // answered as empty, which is a fact and not a missing field. The blanks a
      // model put between its field's name and its words are layout, so the words
      // are returned in place with those taken off; the reading that asks whether
      // a value was stated reads the same words every field carries.
      name = letters;
      size_t from = 0;
      while (from < rest.size() && isSpace(rest[from]))
        ++from;
      size_t to = rest.size();
      while (to > from && isSpace(rest[to - 1]))
        --to;
      value = rest.substr(from, to - from);
      return true;
    }
  } // unnamed

  // What the shape asked for, out of what a model answered.
  inline std::string applyShape(const std::string &shape, const std::string &text)
  {
    if (shape != kShapeOnePrompt && shape != kShapeLabelledFields && shape != kShapeToolCall)
      return text;
    std::string joined;
    std::string out;
    std::vector<std::string> seen;
    size_t at = 0;
    while (at < text.size())
    {
      const size_t lineEnd = text.find('\n', at);
      const size_t stop = lineEnd == std::string::npos ? text.size() : lineEnd;
      const std::string words = cleanLine(text.substr(at, stop - at));
      at = lineEnd == std::string::npos ? text.size() : lineEnd + 1;
      if (words.empty())
        continue;
      if (shape == kShapeOnePrompt)
      {
        if (!joined.empty())
          joined += ' ';
        joined += words;
        continue;
      }
      std::string name;
      std::string value;
      if (!fieldAtLine(words, name, value))
        continue;
      bool already = false;
      for (const std::string &held : seen)
        if (held == name)
          already = true;
      if (already)
        continue;
      seen.push_back(name);
      size_t from = 0;
      while (from < value.size() && isSpace(value[from]))
        ++from;
      size_t to = value.size();
      while (to > from && isSpace(value[to - 1]))
        --to;
      if (!out.empty())
        out += '\n';
      out += name + ": " + value.substr(from, to - from);
    }
    if (shape == kShapeOnePrompt)
      return joined;
    return out;
  }
} // namespace ols

#endif // OLS_ANSWER_SHAPE_H
#pragma once
// The words taken out of an answer or a request. A page's own words and an
// engine's answer are read the same way by every module: a stated string comes
// out of a json value only when that value really holds words - a value of
// another shape reads as nothing, since a read runs on its own thread and a
// throw there would end the whole server - and a model's answer is handed over
// without the blanks it padded its own lines with.
#ifndef OLS_ANSWERWORDS_H
#define OLS_ANSWERWORDS_H

#include <nlohmann/json.hpp>
#include <string>

namespace ols
{
  // One piece of words out of what an engine stated or a page sent: a value of
  // another shape reads as nothing rather than throwing.
  inline std::string statedString(const nlohmann::json &at, const char *key)
  {
    if (!at.is_object() || !at.contains(key) || !at.at(key).is_string())
      return std::string();
    return at.at(key).get<std::string>();
  }

  // A whole number out of what an engine stated or a record holds: only a value
  // that really holds a whole number counts, so a value of another shape reads
  // as nothing and cannot end the thread this read runs on.
  inline long long statedCount(const nlohmann::json &at, const char *key)
  {
    if (!at.is_object() || !at.contains(key) || !at.at(key).is_number_integer())
      return 0;
    return at.at(key).get<long long>();
  }

  // Is that whole number stated at all? A record keeps a count only when
  // something stated it; a nought never stands in for one.
  inline bool statedCountPresent(const nlohmann::json &at, const char *key)
  {
    return at.is_object() && at.contains(key) && at.at(key).is_number_integer();
  }

  // A count that may carry a fraction - seconds of speech, a rate.
  inline double statedMeasure(const nlohmann::json &at, const char *key)
  {
    if (!at.is_object() || !at.contains(key) || !at.at(key).is_number())
      return 0.0;
    return at.at(key).get<double>();
  }

  // Is this one of the characters that pad the edges of a line? The blanks inside
  // a word are its own, so they are not named here.
  inline bool isPaddingSpace(char c)
  {
    return c == ' ' || c == '\n' || c == '\t' || c == '\r';
  }

  // A model pads the ends of its own lines with blanks, whole runs of them,
  // including the blank characters that are not the ordinary space; the line
  // breaks a model put in its answer are its own and kept, while the padding at
  // their edges is not part of the words.
  inline std::string trimLineEdges(const std::string &text)
  {
    auto blankAt = [](const std::string &s, size_t i) -> size_t
    {
      if (i >= s.size())
        return 0;
      const unsigned char c = (unsigned char)s[i];
      if (c == ' ' || c == '\t')
        return 1;
      if (c == 0xC2 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0xA0)
        return 2;
      if (c == 0xE2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80 &&
          (unsigned char)s[i + 2] >= 0x89 && (unsigned char)s[i + 2] <= 0x8F)
        return 3;
      return 0;
    };
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();)
    {
      const size_t run = blankAt(text, i);
      if (run == 0)
      {
        out.push_back(text[i]);
        ++i;
        continue;
      }
      size_t j = i;
      while (j < text.size())
      {
        const size_t next = blankAt(text, j);
        if (next == 0)
          break;
        j += next;
      }
      const bool opensLine = out.empty() || out.back() == '\n' || out.back() == '\r';
      const bool endsLine = j == text.size() || text[j] == '\n' || text[j] == '\r';
      if (opensLine || endsLine)
      {
        i = j;
        continue;
      }
      out.append(text.begin() + i, text.begin() + j);
      i = j;
    }
    return out;
  }
} // namespace ols

#endif // OLS_ANSWERWORDS_H
#pragma once
// Base64 decoding for two paths: the voice module reads audio a page sends as
// base64, and the image studio reads back a picture the browser kept (a dragged
// file or a pasted screenshot), which has no path on this machine and arrives
// as bytes only. Characters outside the alphabet are skipped, so input broken
// over lines still decodes.
#ifndef OLS_BASE64_H
#define OLS_BASE64_H

#include <string>

namespace ols
{
  inline std::string decodeBase64(const std::string &encoded)
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
    // Decoding in one pass: the bytes are written as the sextets arrive, so a
    // picture or an audio clip costs only its own decoded size here, not a
    // second table holding every sextet of the input.
    std::string out;
    out.reserve(encoded.size() / 4u * 3u);
    unsigned gathered = 0;
    unsigned held = 0;
    for (const char c : encoded)
    {
      const int v = sextet(c);
      if (v < 0)
        continue;
      gathered = (gathered << 6) | static_cast<unsigned>(v);
      ++held;
      if (held == 4u)
      {
        out.push_back(static_cast<char>((gathered >> 16) & 0xFFu));
        out.push_back(static_cast<char>((gathered >> 8) & 0xFFu));
        out.push_back(static_cast<char>(gathered & 0xFFu));
        gathered = 0;
        held = 0;
      }
    }
    // A group that stopped early holds its whole bytes at the top: the sextets
    // are shifted into the place a full group would have put them, and only the
    // bytes they carry in full are written.
    if (held > 1u)
    {
      const unsigned aligned = gathered << (6u * (4u - held));
      for (unsigned at = 0u; at + 1u < held; ++at)
      {
        out.push_back(static_cast<char>((aligned >> (16u - 8u * at)) & 0xFFu));
      }
    }
    return out;
  }
}

#endif // OLS_BASE64_H
#pragma once
// The words a model names its own work with: one string of pieces separated by
// single spaces, as the model table carries it. The pages are shown that list as
// the model names it, and a door asks it whether one piece names the work it is
// about - a piece counts only whole, so a short piece cannot answer for a longer
// one that begins with it.
#ifndef OLS_CAPABILITY_WORDS_H
#define OLS_CAPABILITY_WORDS_H

#include <nlohmann/json.hpp>
#include <string>

namespace ols
{
  // Is this piece named whole among the words?
  inline bool hasWord(const std::string &words, const char *want)
  {
    const std::string needle(want);
    size_t at = 0;
    while (at < words.size())
    {
      const size_t space = words.find(' ', at);
      const size_t end = space == std::string::npos ? words.size() : space;
      if (end - at == needle.size() && words.compare(at, needle.size(), needle) == 0)
        return true;
      at = end + 1;
    }
    return false;
  }

  // The pieces as a list, in the order the model names them.
  inline nlohmann::json wordsAsList(const std::string &words)
  {
    nlohmann::json out = nlohmann::json::array();
    size_t at = 0;
    while (at < words.size())
    {
      const size_t space = words.find(' ', at);
      const size_t end = space == std::string::npos ? words.size() : space;
      if (end > at)
        out.push_back(words.substr(at, end - at));
      at = end + 1;
    }
    return out;
  }
} // namespace ols

#endif // OLS_CAPABILITY_WORDS_H
#pragma once
// What a page asks of an engine process this machine opened on its own port:
// whether the engine is awake, and one request answered back in full. The
// engines in this tree frame an answer with chunked pieces, though a promised
// length is read as well, so one answer cannot be read differently by one
// module than by another.
#ifndef OLS_ENGINESOCKET_H
#define OLS_ENGINESOCKET_H

#include <cctype>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include "Platform.h"

namespace ols
{
  // One fresh key shared by every engine this server opens, for when the server
  // itself runs without a key: the pages show it so other tools can reach an
  // engine directly, and an engine only ever answers with the key it was
  // started with. Made once, behind a lock, because engines are opened from
  // more than one thread.
  inline std::string sharedEngineKey()
  {
    static std::mutex keyMutex;
    static std::string fresh;
    std::lock_guard<std::mutex> lock(keyMutex);
    if (fresh.empty())
    {
      std::ostringstream name;
      std::random_device seed;
      name << "local-" << std::hex << std::setw(8) << std::setfill('0')
           << static_cast<uint32_t>(seed()) << std::setw(8) << std::setfill('0')
           << static_cast<uint32_t>(seed());
      fresh = name.str();
    }
    return fresh;
  }

  // Plain socket check of an engine: /health needs no key, and the answer text
  // says whether the server on that port is awake.
  inline bool engineHealthOn(uint16_t port)
  {
    if (!ols::platform::initSockets())
      return false;
    const auto address = ols::platform::endpointFor(port, true);
    const auto sock = ols::platform::newStreamSocket();
    if (sock == ols::platform::kBadSocket)
      return false;
    if (ols::platform::connectSocket(sock, address) != 0)
    {
      ols::platform::discardSocket(sock);
      return false;
    }
    ols::platform::setReadTimeout(sock, 2000);
    static const char request[] = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    if (ols::platform::sendBytes(sock, request, static_cast<int>(sizeof(request) - 1), 0) <= 0)
    {
      ols::platform::discardSocket(sock);
      return false;
    }
    char buffer[512] = {};
    int got = 0;
    for (int tries = 0; tries < 4 && got < static_cast<int>(sizeof(buffer)) - 1; ++tries)
    {
      const int n = ols::platform::receiveBytes(sock, buffer + got,
                                                static_cast<int>(sizeof(buffer)) - 1 - got, 0);
      if (n <= 0)
        break;
      got += n;
      buffer[got] = '\0';
      if (std::string(buffer, static_cast<size_t>(got)).find("\"status\":\"ok\"") != std::string::npos)
        break;
    }
    ols::platform::closeSocket(sock);
    return std::string(buffer, static_cast<size_t>(got)).find("\"status\":\"ok\"") != std::string::npos;
  }

  // One request to an engine with a readable answer: a fresh socket to the port,
  // carrying the key the engine was started with. Only the body of a complete
  // answer is returned, so callers can read the engine's own timings.
  inline bool engineAskWithKey(uint16_t port, const std::string &key,
                               const std::string &method, const std::string &path,
                               const std::string &body, std::string &answer,
                               int timeoutMs)
  {
    if (!ols::platform::initSockets())
      return false;
    const auto address = ols::platform::endpointFor(port, true);
    const auto sock = ols::platform::newStreamSocket();
    if (sock == ols::platform::kBadSocket)
      return false;
    if (ols::platform::connectSocket(sock, address) != 0)
    {
      ols::platform::discardSocket(sock);
      return false;
    }
    std::string request = method + " " + path + " HTTP/1.1\r\n";
    request += "Host: localhost\r\n";
    if (!key.empty())
      request += "X-Api-Key: " + key + "\r\n";
    if (method == "POST")
    {
      request += "Content-Type: application/json\r\n";
      request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    request += body;
    // the words of a question can be pages long, so the write is given the same
    // patience as the read: a full request would otherwise come back as a
    // zero-length write, which is not a refusal - it is the socket saying it has
    // not taken the bytes yet.
    ols::platform::setWriteTimeout(sock, timeoutMs);
    size_t sentSoFar = 0;
    int sendTries = 0;
    while (sentSoFar < request.size())
    {
      const size_t rest = request.size() - sentSoFar;
      const int chunk = rest > 1024u ? 1024 : static_cast<int>(rest);
      const int sent = ols::platform::sendBytes(sock, request.c_str() + sentSoFar, chunk, 0);
      if (sent > 0)
      {
        sentSoFar += static_cast<size_t>(sent);
        sendTries = 0;
        continue;
      }
      if (++sendTries > 400)
      {
        ols::platform::discardSocket(sock);
        return false;
      }
    }
    // a slow answer is not a closed connection: these engines send nothing until
    // the whole answer is ready, so the ask waits for as long as its own door
    // allows and gives up only past that limit.
    ols::platform::setReadTimeout(sock, 2000);
    const auto askDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string all;
    char buffer[8192];
    while (all.size() < 4u * 1024u * 1024u)
    {
      const int got = ols::platform::receiveBytes(sock, buffer, static_cast<int>(sizeof(buffer)), 0);
      if (got <= 0)
      {
        if (std::chrono::steady_clock::now() >= askDeadline)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      all.append(buffer, static_cast<size_t>(got));
      const size_t head = all.find("\r\n\r\n");
      if (head == std::string::npos)
        continue;
      // a lowercase copy of the headers answers one question: how the body is
      // framed - a promised length, or chunked pieces.
      std::string headers = all.substr(0, head);
      for (char &c : headers)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      const size_t atLength = headers.find("content-length:");
      const bool chunked = headers.find("transfer-encoding: chunked") != std::string::npos;
      if (chunked)
      {
        // walk the pieces: a size line in hex, its bytes, and the end at the
        // zero-size piece; a piece not fully arrived waits for more.
        std::string body2;
        size_t at = head + 4;
        bool complete = false;
        bool broken = false;
        while (!complete)
        {
          const size_t lineEnd = all.find("\r\n", at);
          if (lineEnd == std::string::npos)
            break;
          size_t from = at;
          unsigned long long size = 0;
          bool digits = false;
          while (from < lineEnd && std::isxdigit(static_cast<unsigned char>(all[from])))
          {
            const unsigned char c = static_cast<unsigned char>(
                std::tolower(static_cast<unsigned char>(all[from])));
            const unsigned value = std::isdigit(c) ? static_cast<unsigned>(c - '0')
                                                   : static_cast<unsigned>(c - 'a' + 10);
            size = size * 16ull + value;
            digits = true;
            ++from;
          }
          if (!digits || from != lineEnd)
          {
            broken = true;
            break;
          }
          const size_t dataStart = lineEnd + 2;
          if (size == 0)
          {
            complete = true;
            break;
          }
          if (dataStart + static_cast<size_t>(size) + 2 > all.size())
            break;
          body2.append(all.data() + dataStart, static_cast<size_t>(size));
          at = dataStart + static_cast<size_t>(size) + 2;
        }
        if (broken)
          break;
        if (complete)
        {
          answer = body2;
          break;
        }
        continue;
      }
      if (atLength == std::string::npos || atLength > head)
        break;
      long long promised = -1;
      {
        size_t from = atLength + 15;
        while (from < all.size() && all[from] == ' ')
          ++from;
        size_t digitCount = 0;
        while (from + digitCount < all.size() &&
               std::isdigit(static_cast<unsigned char>(all[from + digitCount])))
          ++digitCount;
        if (digitCount > 0)
          promised = std::stoll(all.substr(from, digitCount));
      }
      if (promised < 0)
        break;
      if (all.size() >= head + 4 + static_cast<size_t>(promised))
      {
        answer = all.substr(head + 4, static_cast<size_t>(promised));
        break;
      }
    }
    ols::platform::closeSocket(sock);
    if (!answer.empty())
      return answer.size() > 2;
    return false;
  }
} // namespace ols

#endif // OLS_ENGINESOCKET_H
#pragma once
// What a file this machine holds is asked: whether an ordinary file is there,
// and how many bytes it holds. Every module reads these the same way, so a
// model's files cannot be seen as present on one page and missing on another.
#ifndef OLS_FILEPROBE_H
#define OLS_FILEPROBE_H

#include <filesystem>
#include <system_error>

namespace ols
{
  // An ordinary file, and nothing else: a folder, a device, or a name that is
  // not there all read as no file. One look answers both the presence and the
  // shape, which a pair of separate checks used to ask twice.
  inline bool fileExists(const std::string &path)
  {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
  }

  // The bytes an ordinary file really holds on this machine. A name that cannot
  // be measured holds none, and the callers keep that out of what they state
  // rather than writing a size of nought.
  inline long long fileSize(const std::string &path)
  {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<long long>(size);
  }
} // namespace ols

#endif // OLS_FILEPROBE_H
#pragma once
// Read a small text file, and replace one atomically: the server keeps its own
// records (jobs, sessions, prefs) in plain text files beside the
// things they describe. Writing goes through a temporary file and a rename, so
// the final name is settled in one move and a record written by one user stays
// replaceable while another process is serving.
#ifndef OLS_FILETEXT_H
#define OLS_FILETEXT_H

#include <atomic>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <sstream>

namespace ols
{
  // A record is written beside the thing it describes, and on a machine that
  // never needed one the folder it lives in is not there: the folder comes with
  // the write, never with the start, so a launch that writes nothing creates
  // nothing.
  inline bool ensureParentDir(const std::string &path)
  {
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty())
      return true;
    std::error_code ec{};
    std::filesystem::create_directories(parent, ec);
    return !ec;
  }

  inline bool readFileText(const std::string &path, std::string &out)
  {
    std::ifstream in{path, std::ios::binary};
    if (!in.good())
      return false;
    out.assign((std::istreambuf_iterator<char>{in}), std::istreambuf_iterator<char>{});
    return true;
  }

  inline bool writeFileText(const std::string &path, const std::string &text)
  {
    if (!ensureParentDir(path))
      return false;
    static std::atomic<unsigned> seq{0};
    std::ostringstream name;
    name << path << ".writing" << ++seq;
    const std::string temp = name.str();
    {
      std::ofstream out{temp, std::ios::binary};
      if (!out.good())
        return false;
      out << text;
      out.flush();
      if (!(out.good() || out.eof()))
      {
        std::error_code ignored{};
        std::filesystem::remove(temp, ignored);
        return false;
      }
    }
    std::error_code ec{};
    std::filesystem::rename(temp, path, ec);
    if (ec)
    {
      std::filesystem::remove(temp, ec);
      return false;
    }
    return true;
  }
}

#endif // OLS_FILETEXT_H
#pragma once
// The clock readings and the name a job record is kept under, shared by every
// service: a duration is measured on the steady clock, a record's moment is
// stated in whole seconds, and a job's name carries its kind, a part drawn from
// the clock, and a tag so two jobs asked for in the same moment still get
// different names.
#ifndef OLS_JOB_CLOCK_H
#define OLS_JOB_CLOCK_H

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>

namespace ols
{
  // Whole milliseconds since an arbitrary point. Only the difference between two
  // readings means anything, so moving the wall clock cannot make a run look
  // shorter than it was or a rate impossible to measure.
  inline long long steadyMs()
  {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  // The whole second something happened, as a record states it. A run's own
  // length is measured with the clock above; this is only for the moment a
  // record says a job started, settled, or was made.
  inline long long nowSeconds()
  {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  // The name of a job: its kind, a part drawn from the clock, and a tag. Two
  // jobs asked for in the same moment still get different names, and the pages
  // carry these words as they are.
  inline std::string makeId(const std::string &prefix)
  {
    static std::minstd_rand engine{std::random_device{}()};
    static std::mutex engineMutex;
    std::lock_guard<std::mutex> lock(engineMutex);
    std::uniform_int_distribution<uint64_t> timePart{0, 0xFFFFFFFFFFFFull};
    std::uniform_int_distribution<uint32_t> tagPart{0, 0xFFFFFFFFu};
    const uint64_t stamp =
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
        timePart(engine);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s-%012llx-%08x", prefix.c_str(),
                  static_cast<unsigned long long>(stamp), tagPart(engine));
    return std::string(buffer);
  }
} // namespace ols

#endif // OLS_JOB_CLOCK_H
#pragma once
// What the machine keeps of the jobs it ran: a job's record lives under one
// name in several maps, so when it runs past what it keeps, the oldest name
// goes out of every one of them at once - a record left in one map while its
// rest stands in another would be a lie a page could read.
#ifndef OLS_JOBLEDGER_H
#define OLS_JOBLEDGER_H

#include <cstddef>
#include <deque>
#include <string>

namespace ols
{
  // The cap every engine here keeps its job records to. One number, kept beside
  // the helper that enforces it, so the modules that own a job store cannot
  // state four different ones.
  inline constexpr std::size_t kKeptJobs = 200;

  // Let go of the oldest names until the queue holds only what it may keep.
  // Every map named here is keyed by the same names, so each loses the same
  // entry. A caller that already holds its module's lock holds it here too.
  template <typename... Kept>
  inline void forgetPastKept(std::deque<std::string> &order, std::size_t keep,
                             Kept &...kept)
  {
    while (order.size() > keep)
    {
      const std::string old = order.front();
      order.pop_front();
      (kept.erase(old), ...);
    }
  }
} // namespace ols

#endif // OLS_JOBLEDGER_H
#pragma once
// The words a job wears for how far it has got. They are stated once here: no
// module spells one of them differently, the pages draw only what the machine
// writes, and the questions asked of a state - is it still working, has it
// nowhere further to go - are asked the same way everywhere. A record that
// names no state names nothing, and nothing stated is neither standing nor
// ended.
#ifndef OLS_JOBSTATE_H
#define OLS_JOBSTATE_H

#include <nlohmann/json.hpp>
#include <string>

namespace ols
{
  // The state a job record states, if it states one.
  inline std::string jobState(const nlohmann::json &record)
  {
    return statedString(record, "state");
  }

  // Still working.
  inline constexpr const char *kStateRunning = "running";

  // Got all of its own work done.
  inline constexpr const char *kStateDone = "done";

  // Stopped with a reason.
  inline constexpr const char *kStateError = "error";

  // Stopped because someone asked it to.
  inline constexpr const char *kStateCancelled = "cancelled";

  // What a record door says about a name the machine does not hold: not a state
  // a job wears, but the answer that no job was found.
  inline constexpr const char *kStateUnknown = "unknown";

  // Is the job still working?
  inline bool jobStanding(const nlohmann::json &record)
  {
    return jobState(record) == kStateRunning;
  }

  // Is this state word one the job has nowhere further to go in?
  inline bool stateEnded(const std::string &state)
  {
    return state == kStateDone || state == kStateError || state == kStateCancelled;
  }
} // namespace ols

#endif // OLS_JOBSTATE_H
#pragma once
// The one sum this tree uses: SHA-256, written once here and read the same way
// by every module that checks a file's bytes. The words a file is checked
// against are its card's own sum (a repository states an LFS oid, which is a
// SHA-256 of the file), so the arithmetic below must answer the standard value
// and nothing else.
//
// A file is read in two shapes. Whole, when a card states one sum. And in
// fixed chunks, keyed by the chunk's place in the file, so a record of a weight
// states what it saw piece by piece and can answer about one piece later without
// the file being read again. Both shapes come out of one pass over the file,
// because these are files of a hundred-plus gigabytes and a second read of one
// costs real time.
#ifndef OLS_SHA256_H
#define OLS_SHA256_H

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <fstream>
#include <string>
#include <vector>

namespace ols
{
  // The fraction part of a double, taken as the low 32 bits: the standing way
  // SHA-256 gets its constants out of the square roots and cube roots of the
  // first primes.
  inline uint32_t frac32(double value)
  {
    return static_cast<uint32_t>((value - std::floor(value)) * 4294967296.0);
  }

  // The first 64 primes, built once.
  inline const std::vector<uint32_t> &firstPrimes()
  {
    static const std::vector<uint32_t> primes = []()
    {
      std::vector<uint32_t> out;
      for (uint32_t candidate = 2; out.size() < 64; ++candidate)
      {
        bool prime = true;
        for (const uint32_t p : out)
        {
          if (static_cast<uint64_t>(p) * p > candidate)
            break;
          if (candidate % p == 0)
          {
            prime = false;
            break;
          }
        }
        if (prime)
          out.push_back(candidate);
      }
      return out;
    }();
    return primes;
  }

  // The round constants, built once from those primes.
  inline const std::vector<uint32_t> &kRound()
  {
    static const std::vector<uint32_t> round = []()
    {
      std::vector<uint32_t> out;
      for (const uint32_t p : firstPrimes())
        out.push_back(frac32(std::cbrt(p)));
      return out;
    }();
    return round;
  }

  inline uint32_t rotateRight(uint32_t v, int bits)
  {
    return (v >> bits) | (v << (32 - bits));
  }

  // A running SHA-256. Bytes go in as they are read, and only 63 of them are
  // ever held, so the sum of a 34 GB shard costs the same memory as the sum of
  // a page.
  class Sha256
  {
  public:
    Sha256()
    {
      for (int i = 0; i < 8; ++i)
        state_[i] = frac32(std::sqrt(firstPrimes()[i]));
    }

    void feed(const void *data, size_t length)
    {
      const uint8_t *bytes = static_cast<const uint8_t *>(data);
      bits_ += static_cast<uint64_t>(length) * 8ull;
      size_t at = 0;
      // A partial block is finished before anything new goes into it.
      while (!tail_.empty() && at < length)
      {
        tail_.push_back(bytes[at++]);
        if (tail_.size() == 64)
        {
          mix(tail_.data());
          tail_.clear();
        }
      }
      // Whole blocks go straight in; whatever is left over waits for the next
      // feed or the finish.
      while (at + 64 <= length)
      {
        mix(bytes + at);
        at += 64;
      }
      while (at < length)
        tail_.push_back(bytes[at++]);
    }

    std::string finalHex()
    {
      // The padding the format fixes on the message's own bit length, then the
      // last blocks.
      std::vector<uint8_t> tail = tail_;
      const uint64_t bits = bits_;
      tail.push_back(0x80);
      while (tail.size() % 64 != 56)
        tail.push_back(0);
      for (int i = 0; i < 8; ++i)
        tail.push_back(static_cast<uint8_t>((bits >> (56 - i * 8)) & 0xffull));
      for (size_t at = 0; at < tail.size(); at += 64)
        mix(tail.data() + at);
      char out[65];
      for (int i = 0; i < 8; ++i)
        std::snprintf(out + i * 8, 9, "%08x", state_[i]);
      out[64] = '\0';
      return std::string(out);
    }

  private:
    uint32_t state_[8];
    uint64_t bits_ = 0;
    std::vector<uint8_t> tail_;

    void mix(const uint8_t *data)
    {
      uint32_t w[64];
      for (size_t i = 0; i < 64; ++i)
      {
        w[i] = (static_cast<uint32_t>(data[i * 4]) << 24) |
               (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(data[i * 4 + 2]) << 8) | data[i * 4 + 3];
      }
      for (size_t i = 16; i < 64; ++i)
      {
        const uint32_t s0 = rotateRight(w[i - 15], 7) ^ rotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotateRight(w[i - 2], 17) ^ rotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }
      uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
      uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
      for (size_t i = 0; i < 64; ++i)
      {
        const uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + kRound()[i] + w[i];
        const uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
      }
      state_[0] += a;
      state_[1] += b;
      state_[2] += c;
      state_[3] += d;
      state_[4] += e;
      state_[5] += f;
      state_[6] += g;
      state_[7] += h;
    }
  };

  // The sum of a file as it stands whole. False when the file cannot be read.
  inline bool sha256OfFile(const std::string &path, std::string &hex)
  {
    std::ifstream in{path, std::ios::binary};
    if (!in)
      return false;
    Sha256 sum;
    std::vector<uint8_t> buffer(65536);
    while (true)
    {
      const std::streamsize got =
          in.rdbuf()->sgetn(reinterpret_cast<char *>(buffer.data()),
                            static_cast<std::streamsize>(buffer.size()));
      if (got <= 0)
        break;
      sum.feed(buffer.data(), static_cast<size_t>(got));
    }
    hex = sum.finalHex();
    return true;
  }

  // One pass over a file, answering both shapes at once: its whole sum, and the
  // sum of each fixed chunk of it in file order. The last chunk is whatever is
  // left over, so a table describes a file exactly whatever its length. False
  // when the file cannot be read.
  inline bool fileDigests(const std::string &path, size_t chunkBytes, std::string &wholeHex,
                          std::vector<std::string> &partHex, uint64_t &bytes)
  {
    if (chunkBytes == 0)
      return false;
    std::ifstream in{path, std::ios::binary};
    if (!in)
      return false;
    Sha256 whole;
    std::unique_ptr<Sha256> part;
    std::vector<uint8_t> buffer(65536);
    uint64_t read = 0;
    uint64_t inChunk = 0;
    while (true)
    {
      const std::streamsize got =
          in.rdbuf()->sgetn(reinterpret_cast<char *>(buffer.data()),
                            static_cast<std::streamsize>(buffer.size()));
      if (got <= 0)
        break;
      const uint8_t *bytesRead = buffer.data();
      size_t at = 0;
      while (at < static_cast<size_t>(got))
      {
        if (inChunk == 0)
          part = std::make_unique<Sha256>();
        const size_t room = static_cast<size_t>(chunkBytes) - inChunk;
        const size_t take = (static_cast<size_t>(got) - at) < room ? (static_cast<size_t>(got) - at) : room;
        whole.feed(bytesRead + at, take);
        part->feed(bytesRead + at, take);
        at += take;
        inChunk += take;
        read += static_cast<uint64_t>(take);
        if (inChunk == static_cast<uint64_t>(chunkBytes))
        {
          partHex.push_back(part->finalHex());
          part.reset();
          inChunk = 0;
        }
      }
    }
    if (inChunk != 0)
      partHex.push_back(part->finalHex());
    wholeHex = whole.finalHex();
    bytes = read;
    return true;
  }
} // namespace ols

#endif // OLS_SHA256_H
#pragma once
// The words a tool call is written in, and the reading that takes those words
// apart. The standing model is asked to name one thing the machine can do,
// called by the name the machine lists it under, and to state each thing that
// call needs on its own line, naming it. The answer may still carry the marks a
// model uses to lay words out - bullets, quotes, a fence, or a sentence before
// the call - so the lines that name nothing are layout, and the first line that
// names a field is what stands. A call whose name the machine does not list is
// answered as it was written: a reading never guesses at a name, and a name is
// never corrected by inventing one.
#ifndef OLS_TOOL_CALL_H
#define OLS_TOOL_CALL_H

#include <string>
#include <vector>

namespace ols
{
  // A call, as the shape left it.
  struct ToolCall
  {
    // The name the answer called, or empty when none of its lines named one.
    std::string name;
    // The arguments it stated, in the order the answer wrote them. The same
    // name twice keeps both: which one a machine uses is its own choice, and
    // an answer that states one thing twice is a fact a page can see.
    std::vector<std::pair<std::string, std::string>> args;
  };

  // One call out of an answer, taken apart.
  inline ToolCall readToolCall(const std::string &block)
  {
    ToolCall call;
    size_t at = 0;
    while (at <= block.size())
    {
      const size_t lineEnd = block.find('\n', at);
      const size_t stop = lineEnd == std::string::npos ? block.size() : lineEnd;
      const std::string line = block.substr(at, stop - at);
      at = lineEnd == std::string::npos ? block.size() + 1 : lineEnd + 1;
      std::string found;
      std::string value;
      if (!ols::fieldAtLine(line, found, value))
        continue;
      size_t from = 0;
      while (from < value.size() && isSpace(value[from]))
        ++from;
      size_t to = value.size();
      while (to > from && isSpace(value[to - 1]))
        --to;
      value = value.substr(from, to - from);
      if (found == "tool")
      {
        if (call.name.empty())
          call.name = value;
        continue;
      }
      // the arguments are named under "arg", with the argument's own name after
      // it: "arg model", "arg width", and so on.
      if (found.size() > 4 && found.compare(0, 4, "arg ") == 0)
      {
        const std::string argName = found.substr(4);
        if (!argName.empty())
          call.args.push_back({argName, value});
        continue;
      }
      // a line that names something else is words the model wrote around the
      // call, kept out of it.
    }
    return call;
  }
} // namespace ols

#endif // OLS_TOOL_CALL_H
#pragma once
// A model's own files in the weights tree: the tree, the model's name, and the
// file. Every module reads that shape the same way, so a model's files cannot
// stand in one place for one page and in another for the next.
#ifndef OLS_WEIGHT_PATHS_H
#define OLS_WEIGHT_PATHS_H

#include <initializer_list>
#include <string>
#include <vector>

namespace ols
{
  // The folder a model's files stand in.
  inline std::string modelFileDir(const std::string &weightsDir, const std::string &name)
  {
    return weightsDir + "/" + name + "/";
  }

  // One of a model's files.
  inline std::string modelFilePath(const std::string &weightsDir, const std::string &name,
                                   const std::string &file)
  {
    return modelFileDir(weightsDir, name) + file;
  }

  // A model and its optional side files, one path each: a side file a model does
  // not name brings no path.
  inline std::vector<std::string> modelFiles(const std::string &weightsDir, const std::string &name,
                                             const std::string &file,
                                             const std::initializer_list<const char *> &sideFiles)
  {
    std::vector<std::string> out;
    out.push_back(modelFilePath(weightsDir, name, file));
    for (const char *extra : sideFiles)
    {
      if (extra && *extra)
        out.push_back(modelFilePath(weightsDir, name, extra));
    }
    return out;
  }

  // Are all of a model's files on this machine?
  inline bool filesAllPresent(const std::vector<std::string> &files)
  {
    for (const auto &file : files)
    {
      if (!fileExists(file))
        return false;
    }
    return true;
  }
} // namespace ols

#endif // OLS_WEIGHT_PATHS_H
