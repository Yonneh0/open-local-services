// What this service owns: the ledger of the project studio. The projects that
// stand under <dataDir>/projects - one folder each, with the index beside them
// that drives the dropdown, the facts each project states, and the change mark
// every project carries - all live here: in memory, beside plain json files
// read once at the start. Every write is atomic through ols::writeFileText;
// the whole ledger stands behind one mutex, the repo's serial habits; and no
// throw is allowed out of a door, because a throw on a serving thread would
// end the server. The git under the projects does not live here: it stands
// in GitCore - objects, refs, diffs, and the smart-http doors of git. The
// birth of a repo hands its folder to those doors and keeps repo/ what they
// expect: a HEAD at the default branch, and when the birth was asked for
// one, a first commit carrying a readme.
#include "ProjectService.h"
#include "Common.h"
#include "ProjectGit.h"
#include "ProjectPulls.h"
#include "GitSmartHttp.h"
#include "SshFetch.h"
#include "gitcore/fetch.h"
#include "gitcore/refs.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ols
{
  namespace project
  {

    using json = nlohmann::json;

    // ---------------- the state, all of it file-static ----------------

    static std::string gProjectsDir;
    // The data dir the studio was stood in, kept for the roads that need a
    // room of their own: the ssh key room (E2) stands under it, and nothing
    // else may say where the studio's keys live.
    static std::string gDataRoot;
    static bool gLedgerRead = false;
    static std::vector<std::string> gOrder;
    static std::map<std::string, json> gMeta;
    static std::map<std::string, long long> gBorn;
    // A slug that was let go is never answered again, so the ones that went
    // stand apart: without them in the index file, a deleted project's address
    // could be answered a second time after the server starts over.
    static std::set<std::string> gNeverReuse;
    static std::mutex gMutex;
    // What the last call said about itself: why it was refused, or - of an
    // ask that landed with a miss - the miss it landed with. The mutex
    // carries the ledger, not the note: a door writes the note under the
    // lock, and the door's answer
    // reads it back beside it, in the transport's refusalFromModule - two
    // separate lockings. A second connection's door can set its words in
    // between, and the first answer would speak them. So the note stands one
    // per thread, the way the git upload job does (OpenLocalServices.cpp:
    // 1233): a door and the answer that reads this note share one thread, and
    // no other connection's words can land in it.
    static thread_local std::string gLastError;

    // The facts a page is allowed to change, and nothing else: the rest
    // belongs to the machine or to the git under it.
    static std::set<std::string> gEditableFacts = {"description", "license", "name",
                                                   "planned_features", "purpose"};

    // ---------------- the shape rules ----------------

    // The length one page-changeable field of a project's facts may wear:
    // 1 KiB of words each, so the whole facts file stays far under the 4 MiB
    // a record is read at. A field past it is refused where it stands, the
    // way every other ask the studio cannot keep is refused.
    static const std::size_t gFieldBytes = 1024u;

    // A display name: any words a page could paint, up to a length a page
    // could wear, minus the control bytes a file would have to escape.
    static bool wordOk(const std::string &word)
    {
      if (word.empty() || word.size() > 200u)
        return false;
      for (std::string::size_type i = 0; i < word.size(); ++i)
      {
        const unsigned char c = static_cast<unsigned char>(word[i]);
        if (c < 0x20u || c == 0x7fu)
          return false;
      }
      return true;
    }

    // The folder names a Windows folder cannot wear: the ones the system
    // answers with a device instead - in any case, and with the digits it
    // numbers them by, zero among them (COM0 and LPT0 answer as devices too),
    // and the clock device beside them. A folder made there comes back under
    // a shorter name, so an index carrying one would point at a tree that is
    // not there.
    static bool reservedFolderName(const std::string &name)
    {
      std::string low;
      low.reserve(name.size());
      for (std::string::size_type i = 0; i < name.size(); ++i)
      {
        char c = name[i];
        if (c >= 'A' && c <= 'Z')
          c = static_cast<char>(c - 'A' + 'a');
        low.push_back(c);
      }
      if (low == "nul" || low == "con" || low == "aux" || low == "prn" || low == "clock$")
        return true;
      if (low.size() == 4u && low[3] >= '0' && low[3] <= '9')
        return low.compare(0, 3u, "com") == 0 || low.compare(0, 3u, "lpt") == 0;
      return false;
    }

    // A slug is a folder name under the studio's own word: short, plainly
    // typed, and never wearing a leading dot. A dot or a dash at the end is
    // refused too - the system takes those away from the name it answers, so
    // the address in the ledger would not be the address on the disk - and
    // neither is a name the system answers with a device. Both machines refuse
    // the same slugs: the pair of readings is the gate, not one of them.
    static bool slugOk(const std::string &slug)
    {
      if (slug.empty() || slug.size() > 64u)
        return false;
      if (slug[0] == '.')
        return false;
      if (slug[slug.size() - 1u] == '.' || slug[slug.size() - 1u] == '-')
        return false;
      for (std::string::size_type i = 0; i < slug.size(); ++i)
      {
        const char c = slug[i];
        const bool plain = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                           c == '.' || c == '_' || c == '-';
        if (!plain)
          return false;
      }
      return !reservedFolderName(slug);
    }

    // A branch name a ref could wear: plainly typed, no leading dash, no
    // doubled dash, no trailing dot or dash.
    static bool branchOk(const std::string &branch)
    {
      if (branch.empty() || branch.size() > 64u)
        return false;
      if (branch[0] == '-')
        return false;
      if (branch[branch.size() - 1u] == '.' || branch[branch.size() - 1u] == '-')
        return false;
      bool dash = false;
      for (std::string::size_type i = 0; i < branch.size(); ++i)
      {
        const char c = branch[i];
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!plain)
          return false;
        if (c != '-')
        {
          dash = false;
          continue;
        }
        if (dash)
          return false;
        dash = true;
      }
      return true;
    }

    // The proposal a name wears as a folder name: lowercase, spaces as
    // dashes, anything else a slug may not wear simply dropped - a name's
    // words survive in order, and the dialog shows this before the create
    // door asks for one.
    static std::string proposal(const std::string &name)
    {
      std::string out;
      for (std::string::size_type i = 0; i < name.size(); ++i)
      {
        char c = name[i];
        if (c >= 'A' && c <= 'Z')
          c = static_cast<char>(c - 'A' + 'a');
        if (c == ' ')
          c = '-';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
          out.push_back(c);
      }
      while (!out.empty() && (out[0] == '.'))
        out.erase(out.begin());
      if (out.size() > 64u)
        out.resize(64u);
      // Punctuation a name ends with is dropped rather than kept: a folder
      // would be made without it, and the studio writes this proposal down as
      // the address, so what it proposes is what it stands.
      while (!out.empty() && (out.back() == '.' || out.back() == '-'))
        out.pop_back();
      return out;
    }

    // ---------------- the paths the layout answers with ----------------

    static std::string indexPath()
    {
      return gProjectsDir + "/index.json";
    }

    static std::string projectDir(const std::string &slug)
    {
      return gProjectsDir + "/" + slug;
    }

    static std::string metaPath(const std::string &slug)
    {
      return projectDir(slug) + "/repo/ols/meta.json";
    }

    // ---------------- the notes ----------------

    static void noteError(const std::string &words)
    {
      gLastError = words;
    }

    static void clearNote()
    {
      gLastError.clear();
    }

    // The machine's own failure, in the studio's words: what kind of thing it
    // was, said plainly. The errno itself never travels into an answer - it
    // belongs to the console line, where the full message is written. The
    // Windows codes stand beside the standard names because the two machines
    // refuse the same things apart; one of the standard names is spelled
    // apart there too, and each side is asked for under its own spelling.
    static std::string ioWords(const std::error_code &ec)
    {
#ifdef _WIN32
      switch (ec.value())
      {
      case 5:  // the system refuses
      case 32: // another process holds it
        return "permissions";
      case 39:  // the disk the handle stands on is full
      case 112: // the disk is full
        return "the disk has no room left";
      case 3: // the way to it does not stand
        return "no folder stands where it was asked for";
      case 206: // the name runs past what the way allows
        return "the name is too long for the disk";
      default:
        break;
      }
      if (ec == std::errc::read_only_file_system)
        return "the disk is read-only";
#else
      if (ec == std::errc::read_only_filesystem)
        return "the disk is read-only";
#endif
      if (ec == std::errc::permission_denied)
        return "permissions";
      if (ec == std::errc::no_space_on_device)
        return "the disk has no room left";
      if (ec == std::errc::filename_too_long)
        return "the name is too long for the disk";
      if (ec == std::errc::file_exists || ec == std::errc::not_a_directory)
        return "what stands there is not a folder";
      if (ec == std::errc::no_such_file_or_directory)
        return "no folder stands where it was asked for";
      return "the machine would not say why";
    }

    // ---------------- facts the machine keeps straight ----------------

    static json localSource(const std::string &url)
    {
      json out;
      out["kind"] = "local";
      out["url"] = url;
      return out;
    }

    // A source the index can mirror: kind and url always, with a local
    // stand-in when nothing is stated; pr and branch only when the project
    // states them, so a source without a pull request carries no number.
    static json sourceOf(const json &meta)
    {
      json src;
      if (meta.is_object() && meta.contains("source") && meta["source"].is_object())
        src = meta["source"];
      json out;
      std::string kind = statedString(src, "kind");
      if (kind.empty())
        kind = "local";
      out["kind"] = kind;
      out["url"] = statedString(src, "url");
      const long long pr = statedCount(src, "pr");
      if (pr > 0)
        out["pr"] = pr;
      const std::string branch = statedString(src, "branch");
      if (!branch.empty())
        out["branch"] = branch;
      return out;
    }

    // Planned features as the machine keeps them: a list of well-formed
    // ideas; a row that is not an idea is dropped, the machine does not
    // paint half a request.
    static json tidyFeatures(const json &given)
    {
      json out = json::array();
      if (!given.is_array())
        return out;
      for (const json &item : given)
      {
        if (!item.is_object())
          continue;
        json row;
        row["text"] = statedString(item, "text");
        bool done = false;
        if (item.contains("done") && item["done"].is_boolean())
          done = static_cast<bool>(item["done"]);
        row["done"] = done;
        out.push_back(row);
      }
      return out;
    }

    // The change mark: it only ever moves forward, one step per change. A
    // number past what this machine could have counted is not a mark it made,
    // so it is not carried; and a mark with no room left to move on stays where
    // it stands, rather than wrapping around and looking like an older change.
    static const long long gRevCeiling = 1000000000000ll;

    static long long carriedRev(const json &holder)
    {
      const long long carried = statedCount(holder, "rev");
      if (carried <= 0 || carried > gRevCeiling)
        return 0;
      return carried;
    }

    // The facts as the machine keeps them: the project's own words where
    // they are usable, the studio's where they are not. Unknown fields
    // travel on untouched, so the facts a later phase adds are never
    // dropped by this one's load.
    static json tidyMeta(const json &given, const std::string &nameFallback,
                         const std::string &branchFallback, long long revFallback)
    {
      json src = given;
      if (!src.is_object())
        src = json::object();
      json out = src;
      std::string name = statedString(out, "name");
      if (!wordOk(name))
        name = wordOk(nameFallback) ? nameFallback : "untitled";
      out["name"] = name;
      std::string branch = statedString(out, "default_branch");
      if (!branchOk(branch))
        branch = branchOk(branchFallback) ? branchFallback : "main";
      out["default_branch"] = branch;
      out["description"] = statedString(out, "description");
      out["purpose"] = statedString(out, "purpose");
      out["license"] = statedString(out, "license");
      // The ideas the project stated travel with it: what the field holds is
      // tidied, never the whole facts - a list that is not a list comes back
      // empty, and a row that is not an idea is dropped.
      json carriedIdeas;
      if (out.contains("planned_features"))
        carriedIdeas = out["planned_features"];
      out["planned_features"] = tidyFeatures(carriedIdeas);
      out["source"] = sourceOf(out);
      const long long carried = carriedRev(out);
      const long long fallback = revFallback > 0 && revFallback <= gRevCeiling ? revFallback : 0;
      out["rev"] = carried > 0 ? carried : fallback;
      const long long carriedAt = statedCount(out, "updated_at");
      out["updated_at"] = carriedAt > 0 ? carriedAt : nowSeconds();
      return out;
    }

    // ---------------- the ledger reads and writes ----------------

    // One index row from the project's facts: the index is a mirror of the
    // facts the machine holds, never a separate truth. A slug the machine
    // holds no facts for states nothing rather than an empty project - the
    // ledger and its mirror are built together, and a read that invented one
    // would keep a project the facts never stated.
    static json indexRow(const std::string &slug)
    {
      const json nothing;
      const std::map<std::string, json>::const_iterator facts = gMeta.find(slug);
      const json &meta = facts != gMeta.end() ? facts->second : nothing;
      const std::map<std::string, long long>::const_iterator born = gBorn.find(slug);
      json row;
      row["slug"] = slug;
      row["name"] = statedString(meta, "name");
      row["created_at"] = born != gBorn.end() ? born->second : 0;
      // The as-of the pages watch: the project's own mark of its last change,
      // stated only when the facts state one.
      const long long asOf = statedCount(meta, "updated_at");
      if (asOf > 0)
        row["updated_at"] = asOf;
      row["default_branch"] = statedString(meta, "default_branch");
      row["source"] = sourceOf(meta);
      row["rev"] = carriedRev(meta);
      return row;
    }

    // The index file's document: the rows of every project in the order they
    // were registered, and the slugs that may never be answered again.
    static bool writeIndexOnly()
    {
      json doc;
      json rows = json::array();
      for (const std::string &slug : gOrder)
        rows.push_back(indexRow(slug));
      doc["projects"] = rows;
      json gone = json::array();
      for (const std::string &was : gNeverReuse)
        gone.push_back(was);
      doc["never_reuse"] = gone;
      if (!writeFileText(indexPath(), doc.dump(2)))
      {
        noteError("the index would not take the change");
        return false;
      }
      return true;
    }

    // The two files that carry one project's change: its facts first, the
    // index after. The facts are a project's truth and the index is only a
    // mirror rewritten whole, so a change that could not be written leaves the
    // mirror at the old words - and a caller putting its memory back is enough.
    static bool writeProjectFiles(const std::string &slug, const json &meta)
    {
      if (!writeFileText(metaPath(slug), meta.dump(2)))
      {
        noteError("the facts file would not take the change");
        return false;
      }
      return writeIndexOnly();
    }

    // A record the machine writes is a page of words: the ledger, a project's
    // facts, and the sidecar records a count is drawn from. A file wearing a
    // bigger shape is not this service's record - buffering and parsing an
    // unrelated file's whole size would cost the thread that read it, so a
    // file past the shape is kept out. A folder, a device, or a name that is
    // not there read as no file before any size is asked, which is also what
    // keeps a named pipe or a device off a read that would never finish.
    static const long long gRecordBytes = 4ll * 1024ll * 1024ll;

    static bool readRecordText(const std::string &path, std::string &body)
    {
      if (!fileExists(path))
        return false;
      const long long held = fileSize(path);
      if (held < 0 || held > gRecordBytes)
        return false;
      return readFileText(path, body);
    }

    // The same reading with the console on it: the ledger and a project's
    // facts are read once at the start, so what kept them out is worth saying
    // there - a reading that repeats on every dropdown is not.
    static bool readRecordFile(const std::string &path, std::string &body)
    {
      if (readRecordText(path, body))
        return true;
      if (fileExists(path) && fileSize(path) > gRecordBytes)
      {
        std::cout << "[ols project] " << path << " holds " << fileSize(path)
                  << " bytes, no longer the shape of a record; it is kept out of the reading"
                  << std::endl;
      }
      return false;
    }

    // One project into the ledger from what it states. given is its facts
    // file, or the index row that mirrors them when no facts could be read;
    // the fallbacks are the words that row carried, and a project's moment is
    // the row's - without one, the machine says it took it in now.
    static void carryIntoLedger(const std::string &slug, const json &given,
                                const std::string &nameFallback,
                                const std::string &branchFallback, long long revFallback,
                                long long born)
    {
      gMeta[slug] = tidyMeta(given, nameFallback, branchFallback, revFallback);
      gBorn[slug] = born > 0 ? born : nowSeconds();
      gOrder.push_back(slug);
    }

    // The projects the studio holds, as the folders that stand state them.
    // This is the reading for the case where the index could not be read: the
    // index is only a mirror, and each project's facts file is its truth. A
    // folder without facts is not a project this machine can state, so it
    // stays out - and so does a tree left behind by a write that never
    // completed. What was let go cannot be remembered from a ruined index;
    // that rule comes back with the next whole index. The order is by
    // address, so the dropdown is the same on every reading.
    static void rebuildFromDirs()
    {
      std::error_code ec{};
      if (!std::filesystem::is_directory(gProjectsDir, ec))
        return;
      std::map<std::string, json> found;
      std::filesystem::directory_iterator it(gProjectsDir, ec);
      if (ec)
        return;
      for (; it != std::filesystem::directory_iterator{}; it.increment(ec))
      {
        if (ec)
          break;
        std::error_code kind{};
        if (!it->is_directory(kind) || kind)
          continue;
        const std::string slug = it->path().filename().string();
        if (!slugOk(slug) || gNeverReuse.count(slug) != 0 || gMeta.count(slug) != 0)
          continue;
        std::string factsBody;
        if (!readRecordFile(metaPath(slug), factsBody))
          continue;
        json facts = json::parse(factsBody, nullptr, false);
        if (!facts.is_object())
          continue;
        found[slug] = std::move(facts);
      }
      for (std::map<std::string, json>::iterator one = found.begin(); one != found.end(); ++one)
        carryIntoLedger(one->first, one->second, std::string(), std::string(), 0, 0);
    }

    // The ledger comes in once. A missing index is an honest empty registry;
    // an index the machine cannot read is said so on the console, and the
    // projects that stand under the studio are taken back from their folders -
    // a half-written index is an error to report, never a silently empty
    // registry the pages would paint over real projects.
    static void readLedger()
    {
      gLedgerRead = true;
      const std::string indexFile = indexPath();
      if (!fileExists(indexFile))
        return;
      std::string body;
      if (!readRecordFile(indexFile, body))
      {
        std::cout << "[ols project] the index could not be read: " << indexFile
                  << "; the ledger was read again from the folders that stand" << std::endl;
        rebuildFromDirs();
        return;
      }
      const json doc = json::parse(body, nullptr, false);
      if (!doc.is_object())
      {
        std::cout << "[ols project] the index is half-written: " << indexFile
                  << "; the ledger was read again from the folders that stand" << std::endl;
        rebuildFromDirs();
        return;
      }
      if (doc.contains("never_reuse") && doc["never_reuse"].is_array())
      {
        for (const json &was : doc["never_reuse"])
        {
          if (was.is_string())
            gNeverReuse.insert(was.get<std::string>());
        }
      }
      if (!doc.contains("projects") || !doc["projects"].is_array())
      {
        std::cout << "[ols project] the index states no projects to carry: " << indexFile
                  << "; the ledger was read again from the folders that stand" << std::endl;
        rebuildFromDirs();
        return;
      }
      for (const json &row : doc["projects"])
      {
        if (!row.is_object())
          continue;
        const std::string slug = statedString(row, "slug");
        if (!slugOk(slug) || gMeta.count(slug) != 0)
          continue;
        // The facts file is the project's truth; an index row without one is
        // the bare mirror this machine can still keep.
        json given;
        const std::string facts = metaPath(slug);
        if (fileExists(facts))
        {
          std::string metaBody;
          if (readRecordFile(facts, metaBody))
          {
            given = json::parse(metaBody, nullptr, false);
            if (!given.is_object())
              given = json();
          }
          else
          {
            std::cout << "[ols project] the facts of " << slug << " could not be read; the index row keeps them" << std::endl;
          }
        }
        if (!given.is_object())
          given = row;
        carryIntoLedger(slug, given, statedString(row, "name"),
                        statedString(row, "default_branch"), carriedRev(row),
                        statedCount(row, "created_at"));
      }
    }

    static void ensureLedger()
    {
      if (!gLedgerRead)
        readLedger();
    }

    static void forget(const std::string &slug)
    {
      gMeta.erase(slug);
      gBorn.erase(slug);
      gOrder.erase(std::find(gOrder.begin(), gOrder.end(), slug));
    }

    // The change mark moves on. Called by every mutation, never by a reading.
    // A mark with no room left to move on stays where it stands: a number that
    // wrapped into a smaller one would read as an older change, and the pages
    // would hide the change that just happened.
    static void touchRev(json &meta)
    {
      const long long at = carriedRev(meta);
      meta["rev"] = at >= gRevCeiling ? at : at + 1;
      meta["updated_at"] = nowSeconds();
    }

    // ---------------- the open counts the dropdown carries ----------------

    // Every ordinary json file one folder holds, read into docs. False when
    // the folder could not be read at all; a folder that is simply not
    // there is an empty reading, which is the machine's own statement.
    static bool readRecordsAt(const std::string &dir, std::vector<json> &docs)
    {
      std::error_code ec{};
      if (!std::filesystem::is_directory(dir, ec))
        return !ec;
      std::filesystem::directory_iterator it(dir, ec);
      if (ec)
        return false;
      for (; it != std::filesystem::directory_iterator{}; it.increment(ec))
      {
        if (ec)
          return false;
        std::error_code kind{};
        const bool ordinary = it->is_regular_file(kind);
        if (kind)
          return false;
        if (!ordinary)
          continue;
        const std::string file = it->path().string();
        if (file.size() < 5u || file.compare(file.size() - 5u, 5u, ".json") != 0)
          continue;
        if (!fileExists(file))
          continue;
        std::string body;
        if (!readRecordText(file, body))
          return false;
        json one = json::parse(body, nullptr, false);
        if (one.is_discarded())
          return false;
        docs.push_back(one);
      }
      return true;
    }

    static bool openIssuesAt(const std::string &slug, long long &open)
    {
      std::vector<json> docs;
      if (!readRecordsAt(projectDir(slug) + "/repo/ols/issues", docs))
        return false;
      open = 0;
      for (const json &one : docs)
      {
        if (statedString(one, "state") == "open")
          ++open;
      }
      return true;
    }

    static bool openPullsAt(const std::string &slug, long long &open)
    {
      const std::string pulls = projectDir(slug) + "/repo/ols/pulls";
      std::error_code ec{};
      if (!std::filesystem::is_directory(pulls, ec))
        return !ec;
      open = 0;
      std::filesystem::directory_iterator it(pulls, ec);
      if (ec)
        return false;
      for (; it != std::filesystem::directory_iterator{}; it.increment(ec))
      {
        if (ec)
          return false;
        std::error_code kind{};
        const bool folder = it->is_directory(kind);
        if (kind)
          return false;
        if (!folder)
          continue;
        const std::string record = it->path().string() + "/record.json";
        if (!fileExists(record))
          continue;
        std::string body;
        if (!readRecordText(record, body))
          return false;
        const json one = json::parse(body, nullptr, false);
        if (one.is_discarded())
          return false;
        if (statedString(one, "state") == "open")
          ++open;
      }
      return true;
    }

    // The open counts the dropdown carries with each project. A sidecar the
    // machine can read gives its number, however true zero is; a sidecar it
    // cannot read leaves the field absent, and the page paints no field it
    // has no reading for.
    static void countsInto(const std::string &slug, json &row)
    {
      long long open = 0;
      if (openIssuesAt(slug, open))
        row["issues_open"] = open;
      open = 0;
      if (openPullsAt(slug, open))
        row["pulls_open"] = open;
    }

    // ---------------- the mutations ----------------

    // Make a project stand. The slug is the name's proposal, and a slug is an
    // address that was never answered twice - one let go is one never used
    // again. The repo born here is not empty: it stands with a HEAD at the
    // default branch, written unless the folder already carries one. The
    // readme the checkbox asked for lands as the first commit through
    // GitCore's door; a door that refuses it does not unmake the project -
    // the miss is kept as a fact beside the others. The license stays a
    // stored fact: no door writes it out yet.
    static bool makeProject(const std::string &name, const std::string &optsJson,
                            std::string &madeSlug)
    {
      if (!wordOk(name))
      {
        noteError("the name has no words the studio can wear");
        return false;
      }
      json opts = json::object();
      if (!optsJson.empty())
      {
        opts = json::parse(optsJson, nullptr, false);
        if (!opts.is_object())
        {
          noteError("the options were not words the machine could read");
          return false;
        }
      }
      std::string branch = statedString(opts, "default_branch");
      if (branch.empty())
        branch = "main";
      if (!branchOk(branch))
      {
        noteError("that default branch is not a name a ref could wear");
        return false;
      }
      const std::string slug = proposal(name);
      if (!slugOk(slug))
      {
        noteError("that name leaves no words that could stand as a folder");
        return false;
      }
      if (gNeverReuse.count(slug) != 0)
      {
        noteError("the slug " + slug + " was let go and is never used again");
        return false;
      }
      if (gMeta.count(slug) != 0)
      {
        noteError("a project already stands at " + slug);
        return false;
      }
      json meta;
      meta["name"] = name;
      meta["description"] = "";
      meta["purpose"] = "";
      // The bound the edit path asks of the same field (gFieldBytes), asked
      // where the word first stands: nothing is written before it.
      const std::string license = statedString(opts, "license");
      if (license.size() > gFieldBytes)
      {
        noteError("license is longer than the 1 KiB the studio keeps for one field");
        return false;
      }
      meta["license"] = license;
      meta["default_branch"] = branch;
      meta["planned_features"] = json::array();
      meta["source"] = localSource(std::string());
      // The marker says this machine made the project; when GitCore stands
      // the first commit up, that commit carries these words as its message
      // and replaces the marker.
      meta["first_commit"] = "studio: created";
      meta["rev"] = 0;
      touchRev(meta);
      std::error_code ec{};
      const std::string repoDir = projectDir(slug) + "/repo";
      std::filesystem::create_directories(repoDir, ec);
      if (ec)
      {
        std::cout << "[ols project] the folder would not stand at " << projectDir(slug) << ": "
                  << ec.message() << std::endl;
        noteError("the folder would not stand: " + ioWords(ec));
        return false;
      }
      gMeta[slug] = meta;
      gBorn[slug] = nowSeconds();
      gOrder.push_back(slug);
      // A repo without HEAD answers every read that resolves through HEAD
      // with nothing, forever, so the birth writes the file git itself would
      // have written. branchOk keeps the studio's plain rules, which let
      // through one shape git's door refuses - a name ending ".lock" - so the
      // branch is asked under git's own rules before it enters the file. The
      // ask is not gated on the HEAD being absent: a HEAD the last attempt
      // left behind guards nothing, because with no meta.json beside it the
      // studio holds no project at this folder - and git's own fresh init
      // re-states a HEAD it finds rather than keeping it (refs.h:193-195).
      // The folder wears the branch its facts state.
      const std::string headPath = repoDir + "/HEAD";
      if (!ols::gitcore::isValidRefName("refs/heads/" + branch))
      {
        noteError("that default branch is not a name a ref could wear");
        forget(slug);
        return false;
      }
      const std::string target = "refs/heads/" + branch;
      std::string stood;
      std::string held;
      const bool reads = readFileText(headPath, stood);
      if (!(reads && ols::gitcore::symrefTarget(stood, held) && held == target))
      {
        if (reads)
          std::cout << "[ols project] the HEAD at " << headPath
                    << " says something else; the birth re-states it" << std::endl;
        if (!writeFileText(headPath, "ref: " + target + "\n"))
        {
          std::cout << "[ols project] HEAD would not stand at " << headPath << std::endl;
          noteError("the repo would not take its HEAD");
          forget(slug);
          return false;
        }
      }
      if (opts.contains("with_readme") && opts["with_readme"].is_boolean() &&
          opts["with_readme"].get<bool>())
      {
        const std::string readme =
            "# " + name + "\n\n" + name + " was made by the studio on this machine.\n";
        CommitFile first;
        first.path = "README.md";
        first.content = readme;
        std::vector<CommitFile> files;
        files.push_back(first);
        std::string newOid;
        bool raced = false;
        // The branch is unborn at a birth, which is the one shape of
        // commitFiles that builds a root commit. A commit the door refuses
        // does not unmake the project: the miss stands as a fact beside the
        // others, in the door's own words, so the facts say it, the create
        // answer says it, and the console says it once where the machine was.
        // (The index says nothing of it: indexRow states its own fields, and
        // this is not one of them.)
        if (!commitFiles(repoDir, branch, files, statedString(meta, "first_commit"), "studio",
                         newOid, raced))
        {
          std::string miss = lastError();
          if (miss.empty())
            miss = "the commit did not land";
          meta["readme_miss"] = miss;
          gMeta[slug] = meta;
          // The note of a birth that landed carries the miss to the door that
          // made it, beside the success rather than in place of it.
          noteError(miss);
          std::cout << "[ols project] the readme " << slug << " asked for did not land: "
                    << miss << std::endl;
        }
      }
      if (!writeProjectFiles(slug, meta))
      {
        forget(slug);
        return false;
      }
      madeSlug = slug;
      return true;
    }

    // Register a folder that already stands: a project carried in from
    // another machine or another place on this one. Nothing is copied
    // - the studio's tree for it is thin this phase, and the words of the
    // carried folder's own studio side travel with the project, which is
    // what carrying means. The mark that belongs to the project keeps its
    // number here: registering is not a change to the project.
    static bool adoptProject(const std::string &path, std::string &madeSlug)
    {
      std::string clean = path;
      while (clean.size() > 1u && (clean.back() == '/' || clean.back() == '\\'))
        clean.pop_back();
      std::error_code ec{};
      if (clean.empty() || !std::filesystem::is_directory(clean, ec))
      {
        noteError("no folder stands at " + clean);
        return false;
      }
      // The studio's own folder is not a folder to take on: taken in, it
      // would wear its own name ("projects") as a project's slug, and the
      // studio would hold itself as one of its projects. Equivalent says the
      // two names stand at one place, whatever separators or cases they
      // wear; a comparison that could not be made is no self-reference.
      std::error_code sameEc{};
      if (std::filesystem::equivalent(clean, gProjectsDir, sameEc) && !sameEc)
      {
        noteError("the studio's own folder holds the projects; it is not one of them");
        return false;
      }
      const std::string slug = std::filesystem::path(clean).filename().string();
      if (!slugOk(slug))
      {
        noteError("that folder carries a name that could not be a slug");
        return false;
      }
      if (gNeverReuse.count(slug) != 0)
      {
        noteError("the slug " + slug + " was let go and is never used again");
        return false;
      }
      if (gMeta.count(slug) != 0)
      {
        noteError("a project already stands at " + slug);
        return false;
      }
      json given = json::object();
      const std::string facts = clean + "/repo/ols/meta.json";
      if (fileExists(facts))
      {
        const long long held = fileSize(facts);
        if (held < 0 || held > gRecordBytes)
        {
          noteError("the folder's facts are no longer the shape of a record: " + facts);
          return false;
        }
        std::string metaBody;
        if (!readFileText(facts, metaBody))
        {
          noteError("the folder's facts would not come in: " + facts);
          return false;
        }
        given = json::parse(metaBody, nullptr, false);
        if (!given.is_object())
        {
          noteError("the folder's facts were not a shape the machine could keep");
          return false;
        }
      }
      // A carried folder that states its own source keeps it - the facts
      // travel with the folder; only a folder that states none is
      // registered as a local stand on this machine.
      bool carries = false;
      if (given.contains("source") && given["source"].is_object())
      {
        carries = !statedString(given["source"], "kind").empty() &&
                  !statedString(given["source"], "url").empty();
      }
      if (!carries)
        given["source"] = localSource(clean);
      gMeta[slug] = tidyMeta(given, slug, std::string(), 0);
      gBorn[slug] = nowSeconds();
      gOrder.push_back(slug);
      std::filesystem::create_directories(projectDir(slug) + "/repo", ec);
      if (ec)
      {
        std::cout << "[ols project] the studio's tree would not stand at " << projectDir(slug)
                  << ": " << ec.message() << std::endl;
        noteError("the studio's tree would not stand: " + ioWords(ec));
        forget(slug);
        return false;
      }
      // The repo a project is read through answers every read that resolves
      // through HEAD, so the tree gets the one file an unborn repo must have.
      // An existing HEAD stands untouched: unlike a birth, the folder here may
      // be a real repo the studio only registered - a folder taken from inside
      // the studio's own tree carries its git with it - and registering is not
      // a change to the git under it.
      const std::string repoDir = projectDir(slug) + "/repo";
      const std::string headPath = repoDir + "/HEAD";
      if (!fileExists(headPath))
      {
        const std::string branch = statedString(gMeta[slug], "default_branch");
        if (!ols::gitcore::isValidRefName("refs/heads/" + branch))
        {
          noteError("that default branch is not a name a ref could wear");
          forget(slug);
          return false;
        }
        if (!writeFileText(headPath, "ref: refs/heads/" + branch + "\n"))
        {
          std::cout << "[ols project] HEAD would not stand at " << headPath << std::endl;
          noteError("the repo would not take its HEAD");
          forget(slug);
          return false;
        }
      }
      if (!writeProjectFiles(slug, gMeta[slug]))
      {
        forget(slug);
        return false;
      }
      madeSlug = slug;
      return true;
    }

    // The clone's port over the project's repo dir: the objects and the refs
    // answer through the git door's own RepoPort (GitSmartHttp.h:40-59), and
    // the one move a fetch client adds to that port is refs.h's own
    // setHeadSymref (refs.h:238). One target, one fetch, one thread - the
    // port's packfiles open and close within it.
    class CloneRepoTarget : public ols::gitcore::FetchTarget
    {
    public:
      explicit CloneRepoTarget(const std::string &repoDir) : dir_(repoDir), port_(repoDir) {}

      ols::gitcore::RepoAccess &repo() override { return port_; }

      bool setHeadSymref(const std::string &target) override
      {
        return ols::gitcore::Repository(dir_).setHeadSymref(target);
      }

    private:
      std::string dir_;
      ols::git::RepoPort port_;
    };

    // Take back the studio's tree for a clone that did not land. The slug
    // was fresh when the tree was made, so the whole folder is the studio's
    // to take. What would not come away is said on the console, where the
    // machine was; the caller answers with the clone's own refusal, not the
    // sweep's.
    static void clearTree(const std::string &slug)
    {
      std::error_code ec{};
      std::filesystem::remove_all(projectDir(slug), ec);
      if (ec)
        std::cout << "[ols project] the half-made tree at " << projectDir(slug)
                  << " would not come away: " << ec.message() << std::endl;
    }

    // Stand a project up from a link: the address is asked for its refs
    // first, so an address nobody answers never reaches the disk; the
    // studio's tree is made at the name's proposal with a bare repo whose
    // HEAD names the branch the advertisement calls default; the fetch
    // brings every advertised branch; and only then does the ledger row
    // stand whole. A refusal after the tree is made takes the tree away and
    // the row with it - the birth road, where a clone that did not land
    // leaves the project not at all, and its slug unburned.
    static bool cloneProject(const std::string &url, const std::string &nameHint,
                             const std::string &apiKey, std::string &madeSlug)
    {
      // The address, weighed where it first stands: trailing slashes off,
      // then splitUrl's own readings (HttpFetch.h:234-300) - the https
      // refusal naming TLS among them. A control byte would travel into the
      // request line, so it is refused here, where it is seen.
      std::string link = url;
      while (link.size() > 1u && link.back() == '/')
        link.pop_back();
      if (link.empty() || link.size() > 2048u)
      {
        noteError("the address is nothing to fetch, or longer than this road carries");
        return false;
      }
      for (std::string::size_type i = 0; i < link.size(); ++i)
      {
        const unsigned char c = static_cast<unsigned char>(link[i]);
        if (c < 0x20u || c == 0x7fu)
        {
          noteError("the address carries characters the machine will not put on a wire");
          return false;
        }
      }
      std::string host;
      std::uint16_t port = 0u;
      std::string target;
      std::string words;
      if (!ols::http::splitUrl(link, host, port, target, words))
      {
        noteError(words);
        return false;
      }
      // The name the project wears: the one offered, or the last word of the
      // address with the ".git" taken off - the repository's own name.
      std::string name = nameHint;
      if (name.empty())
      {
        const std::size_t last = target.rfind('/');
        name = last == std::string::npos ? target : target.substr(last + 1u);
        if (name.size() > 4u && name.compare(name.size() - 4u, 4u, ".git") == 0)
          name.resize(name.size() - 4u);
        if (!wordOk(name))
        {
          noteError("the address names no repository the studio could take a name from; say a name with it");
          return false;
        }
      }
      else if (!wordOk(name))
      {
        noteError("the name has no words the studio can wear");
        return false;
      }
      // The slug by the same readings every folder goes through.
      const std::string slug = proposal(name);
      if (!slugOk(slug))
      {
        noteError("that name leaves no words that could stand as a folder");
        return false;
      }
      if (gNeverReuse.count(slug) != 0)
      {
        noteError("the slug " + slug + " was let go and is never used again");
        return false;
      }
      if (gMeta.count(slug) != 0)
      {
        noteError("a project already stands at " + slug);
        return false;
      }
      // The advertisement stands first: its HEAD names the branch the
      // folder's HEAD will wear, and its born branches are what the fetch
      // brings. One transport for both asks - the advertisement and the
      // fetch it makes - as one conversation with one host. The road is
      // read from the address (E2): an ssh address rides the vendored
      // ssh.exe on the studio's own key room, and no ambient identity is
      // ever leaned on; every other address keeps standing where it stood.
      std::vector<ols::gitcore::AdvertisedRef> advertised;
      std::unique_ptr<ols::http::HttpFetch> transport;
      ols::ssh::SshTarget sshTarget;
      if (ols::ssh::looksSsh(link, sshTarget))
      {
        std::string binDir;
        std::string roadWords;
        if (!ols::ssh::sshBinDir(gDataRoot, binDir, roadWords))
        {
          noteError(roadWords);
          return false;
        }
        ols::ssh::SshIdentity identity;
        identity.knownHosts = ols::ssh::knownHostsPath(gDataRoot);
        identity.keyPath = ols::ssh::keysRoom(gDataRoot) + "/id_deploy";
        transport.reset(new ols::ssh::SshFetch(
            sshTarget, identity, (binDir + "/ssh.exe")));
      }
      else
      {
        transport.reset(new ols::http::SocketHttpFetch);
      }
      ols::http::HttpFetch &http = *transport;
      if (!ols::gitcore::lsRemote(http, link, apiKey, advertised, words))
      {
        noteError("the address would not state its refs: " + words);
        return false;
      }
      std::vector<std::string> heads;
      std::string advertisedHead;
      std::string headOid;
      for (const ols::gitcore::AdvertisedRef &ref : advertised)
      {
        if (ref.name.compare(0u, 11u, "refs/heads/") == 0 && !ref.unborn)
          heads.push_back(ref.name);
        if (ref.name == "HEAD")
        {
          if (advertisedHead.empty())
            advertisedHead = ref.symrefTarget;
          if (headOid.empty() && !ref.unborn)
            headOid = ref.oid;
        }
      }
      // The default branch: the one HEAD points through, when it is a name
      // both gateways let - the studio's branchOk and git's own - so the
      // facts and the folder wear one branch; else the plain "main". A v0
      // advertisement carries no symref target (fetch.cpp:214-222), but its
      // HEAD line states the default branch's own id, so the born branch
      // carrying that id is the one HEAD stands behind.
      std::string branch = "main";
      if (advertisedHead.compare(0u, 11u, "refs/heads/") == 0)
      {
        const std::string named = advertisedHead.substr(11u);
        if (branchOk(named) && ols::gitcore::isValidRefName(advertisedHead))
          branch = named;
      }
      else if (!headOid.empty())
      {
        for (const ols::gitcore::AdvertisedRef &ref : advertised)
        {
          if (ref.name.compare(0u, 11u, "refs/heads/") != 0 || ref.unborn || ref.oid != headOid)
            continue;
          const std::string named = ref.name.substr(11u);
          if (branchOk(named) && ols::gitcore::isValidRefName(ref.name))
          {
            branch = named;
            break;
          }
        }
      }
      json meta;
      meta["name"] = name;
      meta["description"] = "";
      meta["purpose"] = "";
      meta["license"] = "";
      meta["default_branch"] = branch;
      meta["planned_features"] = json::array();
      json src;
      src["kind"] = "git";
      src["url"] = link;
      meta["source"] = src;
      meta["rev"] = 0;
      touchRev(meta);
      gMeta[slug] = meta;
      gBorn[slug] = nowSeconds();
      gOrder.push_back(slug);
      const std::string dir = projectDir(slug) + "/repo";
      // The bare repo the fetch lands on, stood by refs.h's own initBare
      // (refs.h:191-197): its HEAD is the symref placeholder to the branch
      // above, and that placeholder is the guarantee - a HEAD stands before
      // a byte of the fetch is asked for, and nothing below overwrites the
      // HEAD a finished fetch moved.
      if (!ols::gitcore::initBare(dir, branch))
      {
        std::cout << "[ols project] the repo would not stand at " << dir << std::endl;
        noteError("the repo would not stand where the clone asked for it");
        forget(slug);
        clearTree(slug);
        return false;
      }
      if (!heads.empty())
      {
        CloneRepoTarget fetchTarget(dir);
        std::vector<std::string> moved;
        if (!ols::gitcore::fetchRepo(fetchTarget, http, link, heads, apiKey, moved, words))
        {
          noteError("the clone did not land: " + words);
          forget(slug);
          clearTree(slug);
          return false;
        }
        // fetchRepo has brought HEAD to the branch its advertisement named,
        // when that branch came with it (fetch.cpp:1103-1142); the facts
        // follow HEAD to the same name then, so folder and facts keep one
        // branch. A name the studio rules refuse to wear stays in the
        // folder alone, and the console says so.
        std::string held;
        if (ols::gitcore::Repository(dir).headTarget(held) && held != branch)
        {
          const std::string named = held.compare(0u, 11u, "refs/heads/") == 0
                                        ? held.substr(11u)
                                        : std::string();
          if (!named.empty() && branchOk(named) && ols::gitcore::isValidRefName(held))
          {
            branch = named;
            meta["default_branch"] = branch;
            gMeta[slug] = meta;
          }
          else
          {
            std::cout << "[ols project] the clone of " << slug << " stands with HEAD at " << held
                      << ", a name the studio's facts do not carry" << std::endl;
          }
        }
      }
      else
      {
        // Nothing to fetch: the address answered no born branch, so the
        // clone lands as the empty repo it is - HEAD at its unborn default,
        // the same shape a fresh birth leaves.
        std::cout << "[ols project] the clone of " << slug
                  << " advertised no branch; the repo stands empty at " << branch << std::endl;
      }
      if (!writeProjectFiles(slug, meta))
      {
        // The write's own words stand: they name what the machine refused.
        forget(slug);
        clearTree(slug);
        return false;
      }
      madeSlug = slug;
      return true;
    }

    // The display name of a project, changed. The slug and the folder keep
    // their names: a slug is an address, and an address does not move.
    static bool renameProject(const std::string &slug, const std::string &newName)
    {
      std::map<std::string, json>::iterator it = gMeta.find(slug);
      if (it == gMeta.end())
      {
        noteError("the studio holds no such project");
        return false;
      }
      if (!wordOk(newName))
      {
        noteError("that name has no words a page could wear");
        return false;
      }
      const json old = it->second;
      json next = old;
      next["name"] = newName;
      touchRev(next);
      it->second = next;
      if (!writeProjectFiles(slug, next))
      {
        it->second = old;
        return false;
      }
      return true;
    }

    static bool editProject(const std::string &slug, const std::string &patchJson)
    {
      std::map<std::string, json>::iterator it = gMeta.find(slug);
      if (it == gMeta.end())
      {
        noteError("the studio holds no such project");
        return false;
      }
      json patch = json::parse(patchJson, nullptr, false);
      if (!patch.is_object())
      {
        noteError("the edit was not words the machine could read");
        return false;
      }
      for (auto kv = patch.begin(); kv != patch.end(); ++kv)
      {
        if (gEditableFacts.count(std::string(kv.key())) == 0)
        {
          noteError("the facts of " + std::string(kv.key()) + " are not the page's to change");
          return false;
        }
      }
      bool asked = false;
      json next = it->second;
      if (patch.contains("name"))
      {
        if (!patch["name"].is_string() || !wordOk(patch["name"].get<std::string>()))
        {
          noteError("that name has no words a page could wear");
          return false;
        }
        next["name"] = patch["name"];
        asked = true;
      }
      for (const char *field : {"description", "purpose", "license"})
      {
        if (!patch.contains(field))
          continue;
        if (!patch[field].is_string())
        {
          noteError(std::string(field) + " was not words the machine could keep");
          return false;
        }
        const std::string value = patch[field].get<std::string>();
        if (value.size() > gFieldBytes)
        {
          noteError(std::string(field) + " is longer than the 1 KiB the studio keeps for one field");
          return false;
        }
        next[field] = patch[field];
        asked = true;
      }
      if (patch.contains("planned_features"))
      {
        if (!patch["planned_features"].is_array())
        {
          noteError("the features were not a list the machine could keep");
          return false;
        }
        json ideas = tidyFeatures(patch["planned_features"]);
        for (const json &idea : ideas)
        {
          if (statedString(idea, "text").size() > gFieldBytes)
          {
            noteError("an idea is longer than the 1 KiB the studio keeps for one field");
            return false;
          }
        }
        next["planned_features"] = ideas;
        asked = true;
      }
      if (!asked)
      {
        noteError("the edit asked for nothing the page may change");
        return false;
      }
      const json old = it->second;
      touchRev(next);
      it->second = next;
      if (!writeProjectFiles(slug, next))
      {
        it->second = old;
        return false;
      }
      return true;
    }

    // Every mutation moves the mark; the door exists so a page can watch it
    // move against the dropdown it read before.
    static bool bumpProject(const std::string &slug)
    {
      std::map<std::string, json>::iterator it = gMeta.find(slug);
      if (it == gMeta.end())
      {
        noteError("the studio holds no such project");
        return false;
      }
      const json old = it->second;
      json next = old;
      touchRev(next);
      it->second = next;
      if (!writeProjectFiles(slug, next))
      {
        it->second = old;
        return false;
      }
      return true;
    }

    // Let a project go: the name has to be said again to let it, the
    // studio's tree comes away, and the slug may never be answered again.
    // A folder the project was adopted from was never the studio's to take
    // away, and stays where it was. The answer is true when the ledger no
    // longer carries the project - which includes the tree that would not
    // come away whole, and the index that would not take the word: the
    // ledger's truth is the ledger's, and what still sits on disk is named
    // in residueWords for the door to say.
    static bool removeProject(const std::string &slug, const std::string &confirmName,
                              std::string &residueWords)
    {
      residueWords.clear();
      const std::map<std::string, json>::const_iterator held = gMeta.find(slug);
      if (held == gMeta.end())
      {
        noteError("the studio holds no such project");
        return false;
      }
      if (confirmName != slug)
      {
        noteError("the name " + slug + " was not said again: the project stays");
        return false;
      }
      // The spec also weighs the pull records: a project whose pulls stand
      // open does not let go, and a folder whose records will not read is
      // refused too - no letting-go on a count the machine cannot state.
      // Merged and closed pull nothing. The count is the pulls module's; this
      // ledger only says what its letting-go cannot do.
      long long openPulls = 0;
      std::string pullWords;
      if (!ols::pulls::countOpenPulls(projectDir(slug) + "/repo", openPulls, pullWords))
      {
        noteError(pullWords + "; the project stays");
        return false;
      }
      if (openPulls > 0)
      {
        noteError(std::to_string(openPulls) +
                  (openPulls == 1ll
                       ? " pull stands open in this project: the project stays"
                       : " pulls stand open in this project: the project stays"));
        return false;
      }
      const json carried = sourceOf(held->second);
      const std::string carriedUrl = statedString(carried, "url");
      std::vector<std::string> stays;
      if (statedString(carried, "kind") == "local" && !carriedUrl.empty() &&
          carriedUrl != projectDir(slug))
        stays.push_back("the folder it was taken on from stands on at " + carriedUrl +
                        "; it was never the studio's to take away");
      std::error_code ec{};
      const std::uintmax_t taken = std::filesystem::remove_all(projectDir(slug), ec);
      if (ec)
      {
        if (taken == 0u)
        {
          // Nothing came away - a tree another process holds locked. The
          // project keeps its place and its index row, since the tree it
          // points at still stands whole.
          std::cout << "[ols project] the tree at " << projectDir(slug)
                    << " would not come away: " << ec.message() << std::endl;
          noteError("the tree would not come away: " + ioWords(ec) + "; the project stays");
          return false;
        }
        // Part of the tree came away and the rest would not: an index entry
        // left standing there would point at a half-taken tree, so the
        // project goes out of the ledger, its address is burned, and what
        // still sits on disk is said so rather than chased.
        stays.push_back("part of its tree came away and the rest could not: it still stands "
                        "where it was made, and the studio holds no part of it");
      }
      forget(slug);
      gNeverReuse.insert(slug);
      // The tree came away, or the project left the ledger either way; a
      // stuck index is one the next write rewrites whole, and the answer
      // says so instead of pretending the letting-go did not happen.
      if (!writeIndexOnly())
        stays.push_back("the index would not take the word: the project is out of the ledger "
                        "this machine holds, and the next write rewrites the index whole");
      for (std::vector<std::string>::size_type i = 0; i < stays.size(); ++i)
      {
        if (i != 0u)
          residueWords += "; ";
        residueWords += stays[i];
      }
      return true;
    }

    // ---------------- the build/run door ----------------

    // The short root a job's records stand in, from the machine survey:
    // long paths are off on the machine, and the toolchain's own names
    // (a .obj beside a deep source, link's response files) blow the
    // limit fast. So nothing is copied and the toolchain reads the file
    // where it stands, while every artifact - the harness, the streams,
    // the exit record - lands in this short root beside short names.
#ifdef _WIN32
    static std::string kBuildRoot = "C:\\Projects\\TEMP\\ols-build";
#else
    static std::string kBuildRoot = "/tmp/ols-build";
#endif

    // One limit, two caps, one age: the sixty seconds the generated
    // harness keeps to itself, the megabyte no stream answer wears past,
    // the tail a standing job is answered with, the rows a Problems
    // panel may wear, and the day past which a job root is pruned.
    static constexpr long long kBuildSeconds = 60;
    static constexpr long long kPruneAgeSeconds = 24ll * 3600ll;
    static constexpr std::size_t kStreamCapBytes = 1024u * 1024u;
    static constexpr std::size_t kStreamTailBytes = 8u * 1024u;
    static constexpr std::size_t kMaxDiagRows = 50u;

    // A project's claim on a build: the slug of every project a build of
    // which stands, with the job that stands for it. The standing scan of
    // the roots is the reading that carries across a restart; the claim is
    // the reading within this one — a job that has just been named has no
    // records standing yet, and a second ask scanning behind it would see
    // nothing standing and start a build beside it. The claim goes in with
    // the job's name and comes away when the records say the job ended.
    static std::map<std::string, std::string> gBuildClaims;

    // The toolchains, probed once per process and never assumed: the
    // VsDevCmd the machine carries (discovered, not hardcoded - the
    // survey found Community 2022 beside older BuildTools), and the
    // exact paths the where-style probes answer for python, the
    // bundled node, and the JDK. A side of the machine with no probe
    // answering has no table, and says so.
    static bool gToolchainRead = false;
    static std::string gVsDevCmd;
    static std::string gPyExe;
    static std::string gNodeExe;
    static std::string gJavacExe;
    static std::string gJavaExe;

    static std::string pathSep()
    {
#ifdef _WIN32
      return std::string("\\");
#else
      return std::string("/");
#endif
    }

    static std::string pathUnder(const std::string &dir, const std::string &name)
    {
      std::string out = dir;
      if (!out.empty() && out.back() != pathSep()[0])
        out += pathSep();
      out += name;
      return out;
    }

    // A tree-relative path joined onto a folder, in the separator this
    // machine joins paths with; the door's own answer keeps the '/'-joined
    // words the doors ask and answer in.
    static std::string joinNative(const std::string &root, const std::string &rel)
    {
      std::string out = root;
      std::string::size_type at = 0;
      while (at < rel.size())
      {
        const std::string::size_type slash = rel.find('/', at);
        const std::string part = rel.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        out += pathSep() + part;
        if (slash == std::string::npos)
          break;
        at = slash + 1u;
      }
      return out;
    }

    static std::string lowerWords(const std::string &word)
    {
      std::string out = word;
      for (std::string::size_type i = 0; i < out.size(); ++i)
      {
        char c = out[i];
        if (c >= 'A' && c <= 'Z')
          c = static_cast<char>(c - 'A' + 'a');
        out[i] = c;
      }
      return out;
    }

    static bool hexDigitAt(char c)
    {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    // The plain name of a tree path, and the extension it wears.
    static std::string fileNameOf(const std::string &rel)
    {
      const std::string::size_type slash = rel.rfind('/');
      return slash == std::string::npos ? rel : rel.substr(slash + 1u);
    }

    static std::string extensionOf(const std::string &name)
    {
      const std::string::size_type dot = name.rfind('.');
      if (dot == std::string::npos || dot + 1u >= name.size())
        return std::string();
      return lowerWords(name.substr(dot + 1u));
    }

    static std::string baseNameOf(const std::string &path)
    {
      const std::string::size_type slash = path.rfind('\\');
      const std::string::size_type other = path.rfind('/');
      const std::string::size_type last = other == std::string::npos
                                              ? slash
                                              : (slash == std::string::npos ? other : (other > slash ? other : slash));
      if (last == std::string::npos)
        return path;
      return path.substr(last + 1u);
    }

    // A whole number out of a record's words, leading blanks given their
    // place; nothing else parses.
    static bool parseWholeNumber(const std::string &text, long long &value)
    {
      value = 0;
      std::string::size_type at = 0;
      while (at < text.size() && (text[at] == ' ' || text[at] == '\t'))
        ++at;
      bool digits = false;
      for (; at < text.size(); ++at)
      {
        const char c = text[at];
        if (c < '0' || c > '9')
          break;
        value = value * 10 + static_cast<long long>(c - '0');
        digits = true;
      }
      return digits;
    }

    // A path refused, never escaped: the door's words go both to the
    // out-parameter the new doors answer with and to the last-error
    // channel every other reading door already reads.
    static bool refuseInWords(const std::string &msg, std::string &words)
    {
      noteError(msg);
      words = msg;
      return false;
    }

    static bool jobIdOk(const std::string &id)
    {
      // The house name shape (Common.h's makeId) with its own kind: bld,
      // a twelve-place drawn from the clock, and a tag.
      if (id.size() != 25u || id.compare(0, 4u, "bld-") != 0 || id[16] != '-')
        return false;
      for (std::string::size_type i = 4u; i < 16u; ++i)
        if (!hexDigitAt(id[i]))
          return false;
      for (std::string::size_type i = 17u; i < 25u; ++i)
        if (!hexDigitAt(id[i]))
          return false;
      return true;
    }

    // The names of the folders under a folder; a folder that is not there
    // has no names, which is the machine's own statement.
    static std::vector<std::string> listDirNames(const std::string &dir)
    {
      std::vector<std::string> names;
      std::error_code ec{};
      if (!std::filesystem::is_directory(dir, ec))
        return names;
      std::filesystem::directory_iterator it(dir, ec);
      if (ec)
        return names;
      for (; it != std::filesystem::directory_iterator{}; it.increment(ec))
      {
        if (ec)
          return names;
        std::error_code kind{};
        if (!it->is_directory(kind))
          continue;
        names.push_back(it->path().filename().string());
      }
      return names;
    }

    static std::string findOnPath(const std::string &exeName)
    {
      // The where-probe in this machine's own words: walk the PATH the
      // process was started with, and answer the first exact path a
      // binary stands at.
      const char *pathVar = std::getenv("PATH");
      if (!pathVar)
        return std::string();
      const std::string full(pathVar);
      std::string::size_type at = 0;
      while (at <= full.size())
      {
        const std::string::size_type sep = full.find(';', at);
        const std::string part =
            full.substr(at, sep == std::string::npos ? std::string::npos : sep - at);
        if (!part.empty())
        {
          std::string candidate = part;
          if (candidate.back() != pathSep()[0])
            candidate += pathSep();
          candidate += exeName;
          if (ols::fileExists(candidate))
            return candidate;
        }
        if (sep == std::string::npos)
          break;
        at = sep + 1u;
      }
      return std::string();
    }

    // The newest name that stands under a folder holding a tail path -
    // the survey's rule that the machine carries several Visual Studios
    // and the door must discover, not hardcode, the newest.
    static std::string newestUnder(const std::string &root, const std::string &tail)
    {
      std::vector<std::string> names = listDirNames(root);
      std::sort(names.begin(), names.end(), std::greater<std::string>());
      for (const std::string &name : names)
      {
        const std::string candidate = root + pathSep() + name + pathSep() + tail;
        if (ols::fileExists(candidate))
          return candidate;
      }
      return std::string();
    }

    static std::string findVsDevCmd()
    {
      static const char *const bases[] = {
          "C:\\Program Files\\Microsoft Visual Studio",
          "C:\\Program Files (x86)\\Microsoft Visual Studio",
      };
      std::vector<std::string> found;
      for (const char *base : bases)
      {
        for (const std::string &year : listDirNames(base))
        {
          const std::string yearDir = std::string(base) + pathSep() + year;
          for (const std::string &edition : listDirNames(yearDir))
          {
            const std::string tail = std::string("Common7") + pathSep() + "Tools" + pathSep() + "VsDevCmd.bat";
            const std::string candidate = yearDir + pathSep() + edition + pathSep() + tail;
            if (ols::fileExists(candidate))
              found.push_back(candidate);
          }
        }
      }
      std::sort(found.begin(), found.end(), std::greater<std::string>());
      return found.empty() ? std::string() : found.front();
    }

    static void detectToolchains()
    {
      gToolchainRead = true;
#ifdef _WIN32
      gVsDevCmd = findVsDevCmd();
      gPyExe = findOnPath("python.exe");
      if (gPyExe.empty())
        gPyExe = newestUnder(std::string(std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : "") +
                                 pathSep() + "AppData" + pathSep() + "Local" + pathSep() + "Programs" +
                                 pathSep() + "Python",
                             "python.exe");
      gNodeExe = findOnPath("node.exe");
      if (gNodeExe.empty())
      {
        const std::string home = std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : "";
        gNodeExe = newestUnder(home + pathSep() + ".dsh" + pathSep() + "dsh-runtimes",
                               "dependencies" + pathSep() + "node" + pathSep() + "bin" + pathSep() + "node.exe");
      }
      gJavacExe = findOnPath("javac.exe");
      gJavaExe = findOnPath("java.exe");
#endif
    }

    static void detectToolchainsIfNeeded()
    {
      if (!gToolchainRead)
        detectToolchains();
    }

    // The extension map the editor's built-in table wears (spec §5.9):
    // the C-family through the machine's cl, python, the bundled node,
    // the JDK. An extension outside the map is not a toolchain this
    // door could have, and the words name it.
    static std::string toolForExtension(const std::string &extension)
    {
      std::string ext = lowerWords(extension);
      if (!ext.empty() && ext[0] == '.')
        ext.erase(ext.begin());
      if (ext == "c" || ext == "cpp" || ext == "cc" || ext == "h")
        return "msvc-cl";
      if (ext == "py")
        return "python";
      if (ext == "js" || ext == "mjs")
        return "node";
      if (ext == "java")
        return "jdk";
      return std::string();
    }

    static bool toolchainEngine(const std::string &extension, std::string &words)
    {
      std::string ext = lowerWords(extension);
      if (!ext.empty() && ext[0] == '.')
        ext.erase(ext.begin());
      const std::string tool = toolForExtension(ext);
      if (tool.empty())
        return refuseInWords("no toolchain stands for ." + ext + " on this machine", words);
      bool found = false;
      if (tool == "msvc-cl")
        found = !gVsDevCmd.empty();
      else if (tool == "python")
        found = !gPyExe.empty();
      else if (tool == "node")
        found = !gNodeExe.empty();
      else if (tool == "jdk")
        found = !gJavacExe.empty() && !gJavaExe.empty();
      if (!found)
        return refuseInWords("the " + tool + " toolchain was not found on this machine; " +
                                 "it waits for a tool that answers here",
                             words);
      words = tool;
      return true;
    }

    // The guard the file/tree doors answer with (ProjectGit's own words):
    // the path stays inside the tree it was asked about - never absolute,
    // never a '.' or '..' segment, never an empty name - and beside that,
    // the characters a command line could take as work: the path is put
    // between the quotes of the generated harness, so it is refused, never
    // escaped, exactly as the fetch doors refuse their parts.
    static bool buildPathOk(const std::string &path, std::string &why)
    {
      why = "the path must stay inside the project: no absolute paths, no '..' or '.' segments, "
            "no empty names, and no characters a command line could take as work";
      if (path.empty() || path[0] == '/')
        return false;
      for (const char c : path)
      {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20u || u == 0x7fu)
          return false;
        if (c == '\\' || c == ':' || c == '"' || c == '%' || c == '&' || c == '|' ||
            c == '<' || c == '>' || c == '^' || c == '!' || c == '`')
          return false;
      }
      std::string::size_type at = 0;
      while (true)
      {
        const std::string::size_type slash = path.find('/', at);
        const std::string part = path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        if (part.empty() || part == "." || part == "..")
          return false;
        if (slash == std::string::npos)
          break;
        at = slash + 1u;
      }
      return true;
    }

    // A field of a job's record file, or nothing when the record says
    // none: the record is the job's truth, wherever it lives.
    static std::string metaField(const std::string &dir, const std::string &key)
    {
      std::string body;
      if (!readRecordText(pathUnder(dir, "meta.txt"), body))
        return std::string();
      const std::string want = key + "=";
      std::string::size_type at = 0;
      while (at < body.size())
      {
        const std::string::size_type nl = body.find('\n', at);
        const std::string::size_type end = nl == std::string::npos ? body.size() : nl;
        std::string line = body.substr(at, end - at);
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (line.rfind(want, 0) == 0)
          return line.substr(want.size());
        at = end + 1u;
      }
      return std::string();
    }

    // Where a job's records stand: from its name alone, by the same mark
    // the root's folder name carries. The records are the answer of a
    // standing job after a restart as well, and nothing else is remembered.
    static std::string findJobDir(const std::string &jobId)
    {
      if (!jobIdOk(jobId))
        return std::string();
      const std::string want = "-" + jobId.substr(4u, 12u);
      std::error_code ec{};
      if (!std::filesystem::is_directory(kBuildRoot, ec))
        return std::string();
      for (const std::string &name : listDirNames(kBuildRoot))
      {
        if (name.size() < want.size() ||
            name.compare(name.size() - want.size(), want.size(), want) != 0)
          continue;
        const std::string dir = pathUnder(kBuildRoot, name);
        const std::string stated = metaField(dir, "job");
        if (stated == jobId)
          return dir;
      }
      return std::string();
    }

    // A job has ended when its records say so: the exit record, the
    // watchdog's word that it ended the run at the batch's own limit, or
    // the mark a stop left behind. None standing is a job still working.
    static bool endedAt(const std::string &dir)
    {
      return ols::fileExists(pathUnder(dir, "exit.txt")) ||
             ols::fileExists(pathUnder(dir, "timed-out.txt")) ||
             ols::fileExists(pathUnder(dir, "stopped.txt"));
    }

    // A stream read with a cap: whole, the first bytes of a finished job up
    // to the cap; tail, the last bytes a standing job has printed. The
    // skipped part is never hidden - the caller wears the word truncated.
    static bool readStreamCapped(const std::string &path, bool whole, std::size_t cap,
                                 std::string &text, bool &truncated)
    {
      text.clear();
      truncated = false;
      if (!ols::fileExists(path))
        return true;
      const long long size = ols::fileSize(path);
      if (size < 0)
        return false;
      const std::size_t held = static_cast<std::size_t>(size);
      std::ifstream in{path, std::ios::binary};
      if (!in.good())
        return false;
      std::size_t from = 0u;
      std::size_t take = held;
      if (whole)
      {
        if (held > cap)
        {
          take = cap;
          truncated = true;
        }
      }
      else if (held > cap)
      {
        from = held - cap;
        take = cap;
        truncated = true;
      }
      if (take > 0u)
      {
        if (from > 0u)
          in.seekg(static_cast<std::streamoff>(from), std::ios::binary);
        if (!in.good())
          return false;
        text.resize(take);
        in.read(&text[0], static_cast<std::streamsize>(take));
        const std::streamsize got = in.gcount();
        if (got >= 0)
          text.resize(static_cast<std::size_t>(got));
      }
      return true;
    }

    // The length a job spent: from the moment its record was written - the
    // job is made with its record - to the moment its end was marked. Both
    // are the filesystem's own clock, so their difference needs no clock of
    // ours; an end or a start not to be read stays unspoken.
    static long long durationMs(const std::string &dir)
    {
      std::error_code ec{};
      const auto started = std::filesystem::last_write_time(pathUnder(dir, "meta.txt"), ec);
      if (ec)
        return -1;
      const char *const ends[] = {"exit.txt", "timed-out.txt", "stopped.txt"};
      for (const char *end : ends)
      {
        std::error_code atEc{};
        const auto at = std::filesystem::last_write_time(pathUnder(dir, end), atEc);
        if (!atEc)
          return std::chrono::duration_cast<std::chrono::milliseconds>(at - started).count();
      }
      return -1;
    }

#ifdef _WIN32
    // The generated harness the survey left standing on this machine: the
    // batch changes to its own short root first and does its own
    // redirection into files beside it - the one shape that completed
    // where Start-Process stream redirection hung around VsDevCmd. The
    // streams stay split (stdout.txt / stderr.txt); the exit record says
    // which step made it and with which code. The batch names no process
    // of its own: the harness is started out of sight through CreateProcess
    // - %PID% is no word this cmd wears, so the number comes from the
    // creating itself - and the limit-keeper is started beside it with
    // that same number, the root, and the limit.
    static std::string batchFor(const std::string &tool, const std::string &srcAbs,
                                const std::string &jobDir)
    {
      std::string text = "@echo off\r\nsetlocal EnableDelayedExpansion\r\n";
      text += "cd /d \"" + jobDir + "\"\r\n";
      if (tool == "msvc-cl")
      {
        text += "call \"" + gVsDevCmd + "\" -arch=amd64 -no_logo\r\n";
        text += "cl /nologo /W3 /utf-8 \"" + srcAbs + "\" /Fe\"out.exe\" 1>>\"stdout.txt\" 2>>\"stderr.txt\"\r\n";
        text += "if errorlevel 1 (\r\n  >\"exit.txt\" echo compile !ERRORLEVEL!\r\n  goto :end\r\n)\r\n";
        text += "\"out.exe\" 1>>\"stdout.txt\" 2>>\"stderr.txt\"\r\n";
      }
      else if (tool == "python")
      {
        text += "\"" + gPyExe + "\" \"" + srcAbs + "\" 1>>\"stdout.txt\" 2>>\"stderr.txt\"\r\n";
      }
      else if (tool == "node")
      {
        text += "\"" + gNodeExe + "\" \"" + srcAbs + "\" 1>>\"stdout.txt\" 2>>\"stderr.txt\"\r\n";
      }
      else if (tool == "jdk")
      {
        text += "\"" + gJavacExe + "\" \"" + srcAbs + "\" 1>>\"stderr.txt\" 2>>\"stderr.txt\"\r\n";
        text += "if errorlevel 1 (\r\n  >\"exit.txt\" echo compile !ERRORLEVEL!\r\n  goto :end\r\n)\r\n";
        text += "\"" + gJavaExe + "\" \"" + srcAbs + "\" 1>>\"stdout.txt\" 2>>\"stderr.txt\"\r\n";
      }
      else
      {
        return std::string();
      }
      text += ">\"exit.txt\" echo run !ERRORLEVEL!\r\n:end\r\n";
      return text;
    }

    static std::string watchBatchText()
    {
      // One second at a time on a headless machine: ping is the sleep that
      // needs no console; the poll is for the exit record, and only when
      // none came down inside the limit does the tree get ended - through
      // the process number the door passes beside the job root.
      std::string text = "@echo off\r\n";
      text += "cd /d \"%~1\"\r\n";
      text += "for /l %%i in (1,1," + std::to_string(kBuildSeconds) + ") do (\r\n";
      text += "  if exist \"exit.txt\" goto :wd_end\r\n";
      text += "  if exist \"stopped.txt\" goto :wd_end\r\n";
      text += "  ping -n 2 127.0.0.1 >nul\r\n";
      text += ")\r\n";
      text += "if exist \"exit.txt\" goto :wd_end\r\n";
      text += "if exist \"stopped.txt\" goto :wd_end\r\n";
      text += "taskkill /T /F /PID %2 >nul 2>&1\r\n";
      text += ">\"timed-out.txt\" echo the batch ended the run at its own limit\r\n";
      text += ":wd_end\r\n";
      return text;
    }

    // Out of sight: the harness process is created and started, and no
    // handle is kept - the process stands on its own, and the number it
    // came out with is recorded beside the job for a stop to end the
    // tree through.
    static bool launchDetached(const std::string &batPath, const std::string &extraArgs,
                               long long &pid)
    {
      static const char *const kCmdExe = "C:\\Windows\\System32\\cmd.exe";
      STARTUPINFO info{};
      info.cb = sizeof(info);
      PROCESS_INFORMATION made{};
      // No quotes around the batch here: a quoted head after /c starts the
      // inner cmd's quote-stripping habit and eats the arguments after it.
      // What this door hands is its own fixed root and the slug-marked
      // folder under it - neither name wears a space to lose.
      std::string full = std::string(kCmdExe) + " /c " + batPath + " " + extraArgs;
      if (CreateProcessA(kCmdExe, full.data(), nullptr, nullptr, 0, 0u, nullptr, nullptr,
                         &info, &made) == 0)
      {
        pid = 0;
        return false;
      }
      pid = static_cast<long long>(made.dwProcessId);
      if (made.hThread)
        CloseHandle(made.hThread);
      if (made.hProcess)
        CloseHandle(made.hProcess);
      return true;
    }
#endif

    static void diagnosticsFromText(const std::string &text, const std::string &streamName,
                                    const std::string &srcAbs, const std::string &relPath,
                                    json &rows, bool &more)
    {
      // The two families of diagnosis the machine really speaks, from the
      // survey's verified lines: the MSVC form file(line[,col]): error Cxxxx:
      // (the column comes only for some codes), and the file:line[:col]:
      // family gcc/clang/javac speak, whose severity may be absent. They
      // stand here as function-local so a pattern the machine could not
      // compile is a door refusal with words, not a word at load time. A
      // line neither family reads is never dropped - it stays in the raw
      // streams above, and a LINK's own words have no line to name.
      static const std::regex kDiagMsvc(
          R"rgx(((?:[A-Za-z]:[\\/])?[^:()]+?)\((\d+)(?:,(\d+))?\):\s*(fatal error|error|warning|note)(?:\s+([A-Za-z][A-Za-z0-9]{1,6}))?:\s*(.*))rgx");
      static const std::regex kDiagGcc(
          R"rgx(((?:[A-Za-z]:\\[^:]+|[^:\s][^:]*?)):(\d+)(?::(\d+))?:\s*(?:(fatal error|error|warning|note)(?:\[([A-Za-z][A-Za-z0-9]{1,6})\])?:\s*)?(.*))rgx");
      if (!rows.is_array())
        rows = json::array();
      std::smatch match;
      std::string::size_type at = 0;
      std::size_t seen = 0u;
      while (at <= text.size() && seen < 20000u)
      {
        ++seen;
        const std::string::size_type nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() + 1u : nl + 1u;
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (line.empty())
          continue;
        std::string file, sev, code, msg;
        long long lineNo = 0;
        long long colNo = -1;
        if (std::regex_match(line, match, kDiagMsvc))
        {
          file = match[1].str();
          if (!match[2].matched || !parseWholeNumber(match[2].str(), lineNo))
            continue;
          if (match[3].matched)
            parseWholeNumber(match[3].str(), colNo);
          sev = match[4].str();
          if (match[5].matched)
            code = match[5].str();
          msg = match[6].str();
        }
        else if (std::regex_match(line, match, kDiagGcc))
        {
          file = match[1].str();
          if (!match[2].matched || !parseWholeNumber(match[2].str(), lineNo))
            continue;
          if (match[3].matched)
            parseWholeNumber(match[3].str(), colNo);
          if (match[4].matched)
            sev = match[4].str();
          if (match[5].matched)
            code = match[5].str();
          msg = match[6].str();
        }
        else
        {
          continue;
        }
        if (rows.size() >= kMaxDiagRows)
        {
          more = true;
          return;
        }
        json row;
        row["file"] = file;
        row["line"] = lineNo;
        if (colNo >= 0)
          row["col"] = colNo;
        if (!sev.empty())
          row["sev"] = sev;
        if (!code.empty())
          row["code"] = code;
        row["msg"] = msg;
        row["stream"] = streamName;
        // The path put back where it can be: a line naming the built file -
        // by its full path, or plainly by its name - is answered as the
        // project-relative path the door was asked with.
        if (!relPath.empty() && !srcAbs.empty() &&
            (lowerWords(file) == lowerWords(srcAbs) || baseNameOf(file) == baseNameOf(srcAbs)))
          row["path"] = relPath;
        rows.push_back(row);
      }
    }

    // A build stands: the file is one of the project's and stands on this
    // machine where the index says; the toolchain answering for its
    // extension is the machine's own finding. Nothing is copied - the
    // toolchain reads the file where it stands. While one build already
    // stands for this project a second is refused with words, and roots
    // a day past their making are pruned on this same ask.
    static bool startBuildEngine(const std::string &slug, const std::string &relPath,
                                 std::string &jobIdOut, std::string &words)
    {
      if (gMeta.find(slug) == gMeta.end())
        return refuseInWords("the studio holds no such project", words);
      std::string why;
      if (!buildPathOk(relPath, why))
        return refuseInWords(why, words);
      const std::string ext = extensionOf(fileNameOf(relPath));
      if (!toolchainEngine(ext, words))
        return false;
      const std::string tool = words;
      const std::string srcAbs = joinNative(projectDir(slug), relPath);
      if (!ols::fileExists(srcAbs))
        return refuseInWords("the file does not stand on this machine at " + slug + "/" + relPath +
                                 ": a build compiles what stands there, in place",
                             words);

      // The stale roots come away, and a standing job of this project
      // answers here instead of a second one starting beside it.
      {
        std::error_code ec{};
        if (std::filesystem::is_directory(kBuildRoot, ec))
        {
          for (const std::string &name : listDirNames(kBuildRoot))
          {
            const std::string dir = pathUnder(kBuildRoot, name);
            const std::string startedText = metaField(dir, "started");
            long long started = 0;
            if (parseWholeNumber(startedText, started) && nowSeconds() - started > kPruneAgeSeconds)
            {
              std::error_code rm{};
              std::filesystem::remove_all(dir, rm);
              continue;
            }
            if (metaField(dir, "slug") == slug && !endedAt(dir))
              return refuseInWords("a build is already standing for " + slug +
                                       "; its name is " + metaField(dir, "job") +
                                       " and the status door answers it",
                                   words);
          }
        }
      }
      // The claim says what the scan could have missed: a job of this
      // project named by an ask that is still writing its records. A claim
      // whose records say the job ended is not a standing build, and comes
      // away here - the claim and the records are read under one lock, so a
      // second ask never sees the one without the other.
      {
        const std::map<std::string, std::string>::const_iterator claim = gBuildClaims.find(slug);
        if (claim != gBuildClaims.end())
        {
          const std::string claimedDir = findJobDir(claim->second);
          if (claimedDir.empty() || endedAt(claimedDir))
            gBuildClaims.erase(claim);
          else
            return refuseInWords("a build is already standing for " + slug + "; its name is " +
                                     claim->second + " and the status door answers it",
                                 words);
        }
      }

      const std::string jobId = ols::makeId("bld");
      if (!jobIdOk(jobId))
        return refuseInWords("the job could not be given a name the status door could answer", words);
      gBuildClaims[slug] = jobId;
      // The root's name carries the project plainly and the job's mark
      // exactly, kept short: the project's name is a reading, the mark is
      // the address.
      std::string slugPart = slug;
      if (slugPart.size() > 16u)
        slugPart.resize(16u);
      const std::string dirName = slugPart + "-" + jobId.substr(4u, 12u);
      const std::string jobDir = pathUnder(kBuildRoot, dirName);

      // The record comes with the job before the harness does: the job is
      // named, and so is its source and the reading of the machine that
      // answered for it.
      std::string meta;
      meta += "job=" + jobId + "\n";
      meta += "slug=" + slug + "\n";
      meta += "rel=" + relPath + "\n";
      meta += "src=" + srcAbs + "\n";
      meta += "tool=" + tool + "\n";
      meta += "started=" + std::to_string(nowSeconds()) + "\n";
      if (!writeFileText(pathUnder(jobDir, "meta.txt"), meta))
      {
        gBuildClaims.erase(slug);
        return refuseInWords("the record of the job would not stand: " + jobDir, words);
      }

#ifdef _WIN32
      const std::string runText = batchFor(tool, srcAbs, jobDir);
      const std::string watchText = watchBatchText();
      if (runText.empty() || watchText.empty())
      {
        gBuildClaims.erase(slug);
        return refuseInWords("the harness of this toolchain is not written on this side of the machine", words);
      }
      const std::string runBat = pathUnder(jobDir, "run.bat");
      const std::string watchBat = pathUnder(jobDir, "watch.bat");
      if (!writeFileText(runBat, runText) || !writeFileText(watchBat, watchText))
      {
        gBuildClaims.erase(slug);
        return refuseInWords("the harness would not stand: " + runBat, words);
      }
      // Out of sight: the batch is started through CreateProcess, and this
      // call is not its hand; the number the process came out with is
      // recorded beside the job, and the limit-keeper is started beside
      // it with that same number.
      long long pid = 0;
      if (!launchDetached(runBat, std::string(), pid))
      {
        std::error_code rm{};
        std::filesystem::remove_all(jobDir, rm);
        gBuildClaims.erase(slug);
        return refuseInWords("the harness would not start: the machine refused " + runBat, words);
      }
      if (!writeFileText(pathUnder(jobDir, "pid.txt"), std::to_string(pid)))
        std::cout << "[ols project] the process record of " << jobId << " would not stand" << std::endl;
      const std::string watchArgs = jobDir + " " + std::to_string(pid);
      long long keeperPid = 0;
      if (!launchDetached(watchBat, watchArgs, keeperPid))
        std::cout << "[ols project] the limit-keeper of " << jobId << " would not stand: "
                  << watchBat << std::endl;
#else
      // The other side has no detected toolchains, so this point is not
      // reached there; the harness of that side waits with its table.
      static_cast<void>(tool);
      static_cast<void>(srcAbs);
      static_cast<void>(jobDir);
#endif
      jobIdOut = jobId;
      words.clear();
      return true;
    }

    // One build job as far as it has got. The state comes from the records
    // the batch leaves: no end mark is a job still working, answered with
    // the tails of its streams; an end mark answers with the whole capture
    // within the cap, the exit code and the step that made it, and the
    // duration the job spent.
    static bool buildStatusEngine(const std::string &jobId, std::string &answerJson, std::string &words)
    {
      const std::string jobDir = findJobDir(jobId);
      if (jobDir.empty())
        return refuseInWords("no build job stands under that name", words);
      const bool hasExit = ols::fileExists(pathUnder(jobDir, "exit.txt"));
      const bool hasTimed = ols::fileExists(pathUnder(jobDir, "timed-out.txt"));
      const bool hasStopped = ols::fileExists(pathUnder(jobDir, "stopped.txt"));
      const bool ended = hasExit || hasTimed || hasStopped;

      json out;
      out["job"] = jobId;
      const std::string rel = metaField(jobDir, "rel");
      const std::string srcAbs = metaField(jobDir, "src");
      const std::string tool = metaField(jobDir, "tool");
      const std::string slug = metaField(jobDir, "slug");
      if (!slug.empty())
        out["project"] = slug;
      if (!rel.empty())
        out["path"] = rel;
      if (!tool.empty())
        out["tool"] = tool;

      if (hasExit)
      {
        std::string endLine;
        if (readRecordText(pathUnder(jobDir, "exit.txt"), endLine))
        {
          const std::string::size_type nl = endLine.find('\n');
          if (nl != std::string::npos)
            endLine.erase(nl);
          if (!endLine.empty() && endLine.back() == '\r')
            endLine.pop_back();
        }
        const std::string::size_type sp = endLine.find(' ');
        const std::string stage = sp == std::string::npos ? endLine : endLine.substr(0, sp);
        if (stage == "compile" || stage == "run")
          out["stage"] = stage;
        long long code = 0;
        if (sp != std::string::npos && parseWholeNumber(endLine.substr(sp + 1u), code))
        {
          out["exit_code"] = code;
          out["state"] = code == 0 ? kStateDone : kStateError;
        }
        else
        {
          out["state"] = kStateError;
        }
      }
      else if (hasTimed)
      {
        out["state"] = kStateError;
        out["timed_out"] = true;
      }
      else if (hasStopped)
      {
        out["state"] = kStateCancelled;
      }
      else
      {
        out["state"] = kStateRunning;
      }

      // A job whose records say it ended keeps no claim on its project: the
      // next ask for that project reads the same records and starts clean.
      if (ended && !slug.empty())
      {
        const std::map<std::string, std::string>::iterator claim = gBuildClaims.find(slug);
        if (claim != gBuildClaims.end() && claim->second == jobId)
          gBuildClaims.erase(claim);
      }

      // The streams: whole to the cap once ended, a tail while standing.
      std::string outText, errText;
      bool outTruncated = false, errTruncated = false;
      const std::size_t cap = ended ? kStreamCapBytes : kStreamTailBytes;
      if (!readStreamCapped(pathUnder(jobDir, "stdout.txt"), ended, cap, outText, outTruncated))
        return refuseInWords("the job's words would not come in: " + pathUnder(jobDir, "stdout.txt"), words);
      if (!readStreamCapped(pathUnder(jobDir, "stderr.txt"), ended, cap, errText, errTruncated))
        return refuseInWords("the job's words would not come in: " + pathUnder(jobDir, "stderr.txt"), words);
      out["stdout"] = outText;
      out["stderr"] = errText;
      if (outTruncated)
        out["stdout_truncated"] = true;
      if (errTruncated)
        out["stderr_truncated"] = true;

      // The compiler's own lines, read for the Problems panel wherever
      // they name the built file; the raw streams are still whole in the
      // fields above, so an unknown line is never dropped.
      json rows = json::array();
      bool more = false;
      diagnosticsFromText(errText, "stderr", srcAbs, rel, rows, more);
      diagnosticsFromText(outText, "stdout", srcAbs, rel, rows, more);
      if (rows.is_array() && !rows.empty())
        out["diagnostics"] = rows;
      if (more)
        out["more_diagnostics"] = true;

      if (ended)
      {
        const long long ms = durationMs(jobDir);
        if (ms >= 0)
          out["duration_ms"] = ms;
      }

      answerJson = out.dump();
      return true;
    }

    // A standing build asked to go: the process the job wrote down first
    // is ended tree-wide - the batch, its tool, everything under the
    // batch's handle - and the mark a stopped job answers as is left
    // behind. A job that already ended is refused with words; a job that
    // never got to name its process is refused too, its batch's own
    // sixty-second limit still standing for it.
    static bool stopBuildEngine(const std::string &jobId, std::string &words)
    {
      const std::string jobDir = findJobDir(jobId);
      if (jobDir.empty())
        return refuseInWords("no build job stands under that name", words);
      if (endedAt(jobDir))
        return refuseInWords("the build has already ended; there is nothing to stop", words);
      std::string pidText;
      if (!readRecordText(pathUnder(jobDir, "pid.txt"), pidText))
        return refuseInWords("the job had not written down its process yet; the limit inside its batch still stands for it", words);
      long long pid = 0;
      if (!parseWholeNumber(pidText, pid) || pid <= 0)
        return refuseInWords("the job had not written down a process this machine could end; its batch's own limit still stands", words);
      const std::string kill = "taskkill /T /F /PID " + std::to_string(pid);
      std::system(kill.c_str());
      // Whether the tree was standing and is ended now, or it had gone
      // without saying, the stop was asked for and the job's record says
      // it went because one was asked.
      if (!writeFileText(pathUnder(jobDir, "stopped.txt"), "the stop was asked for this job"))
        return refuseInWords("the process was ended; the record would not take the mark: " + jobDir, words);
      // The stop is the end the records tell of, and the claim with it: the
      // next ask for that project reads the mark and starts clean.
      {
        const std::string stoppedSlug = metaField(jobDir, "slug");
        if (!stoppedSlug.empty())
        {
          const std::map<std::string, std::string>::iterator claim = gBuildClaims.find(stoppedSlug);
          if (claim != gBuildClaims.end() && claim->second == jobId)
            gBuildClaims.erase(claim);
        }
      }
      words.clear();
      return true;
    }

    // ---------------- the readings ----------------

    static std::string metaText(const std::string &slug)
    {
      const std::map<std::string, json>::const_iterator it = gMeta.find(slug);
      if (it == gMeta.end())
        return std::string();
      return it->second.dump();
    }

    static std::string listText()
    {
      json doc;
      json rows = json::array();
      for (const std::string &slug : gOrder)
      {
        json row = indexRow(slug);
        countsInto(slug, row);
        rows.push_back(row);
      }
      doc["projects"] = rows;
      return doc.dump();
    }

    static std::string revsText()
    {
      json out;
      for (const std::string &slug : gOrder)
      {
        const std::map<std::string, json>::const_iterator facts = gMeta.find(slug);
        if (facts != gMeta.end())
          out[slug] = carriedRev(facts->second);
      }
      return out.dump();
    }

    // ---- the member surface, thin over the file-static engine above ----
    // Every door takes the one lock, clears the note of the last call, and
    // lets no throw out: a throw on a serving thread would end the server,
    // and the words the engine set say what happened - a catch here only
    // covers what no engine named.

    ProjectService::ProjectService(const std::string &dataDir)
    {
      std::string root = dataDir;
      while (!root.empty() && (root.back() == '/' || root.back() == '\\'))
        root.pop_back();
      gProjectsDir = root + "/projects";
      gDataRoot = root;
      std::lock_guard<std::mutex> lock(gMutex);
      readLedger();
    }

    bool ProjectService::initIfNeeded()
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      try
      {
        ensureLedger();
        return true;
      }
      catch (...)
      {
        noteError("the ledger would not come in: it met something it cannot say");
        return false;
      }
    }

    std::string ProjectService::listJson() const
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      try
      {
        ensureLedger();
        return listText();
      }
      catch (...)
      {
        noteError("the list would not come in: it met something it cannot say");
        return std::string();
      }
    }

    // A word-play on the name alone: it touches no state, so it takes no
    // lock.
    std::string ProjectService::slugFor(const std::string &name) const
    {
      try
      {
        return proposal(name);
      }
      catch (...)
      {
        return std::string();
      }
    }

    bool ProjectService::create(const std::string &name, const std::string &optsJson,
                                std::string &madeSlug)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      madeSlug.clear();
      try
      {
        ensureLedger();
        return makeProject(name, optsJson, madeSlug);
      }
      catch (...)
      {
        noteError("the making of a project met something it cannot say");
        return false;
      }
    }

    bool ProjectService::adoptFolder(const std::string &path, std::string &madeSlug)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      madeSlug.clear();
      try
      {
        ensureLedger();
        return adoptProject(path, madeSlug);
      }
      catch (...)
      {
        noteError("the carrying-in of a folder met something it cannot say");
        madeSlug.clear();
        return false;
      }
    }

    bool ProjectService::adoptUrl(const std::string &url, const std::string &nameHint,
                                  std::string &madeSlug, const std::string &apiKey)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      madeSlug.clear();
      try
      {
        ensureLedger();
        return cloneProject(url, nameHint, apiKey, madeSlug);
      }
      catch (...)
      {
        noteError("the clone from a link met something it cannot say");
        madeSlug.clear();
        return false;
      }
    }

    bool ProjectService::rename(const std::string &slug, const std::string &newName)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      try
      {
        ensureLedger();
        return renameProject(slug, newName);
      }
      catch (...)
      {
        noteError("the change of a name met something it cannot say");
        return false;
      }
    }

    bool ProjectService::remove(const std::string &slug, const std::string &confirmName,
                                std::string &residueWords)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      residueWords.clear();
      try
      {
        ensureLedger();
        return removeProject(slug, confirmName, residueWords);
      }
      catch (...)
      {
        noteError("the letting-go of a project met something it cannot say");
        return false;
      }
    }

    std::string ProjectService::metaJson(const std::string &slug) const
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      try
      {
        ensureLedger();
        return metaText(slug);
      }
      catch (...)
      {
        noteError("the facts would not come in: it met something it cannot say");
        return std::string();
      }
    }

    bool ProjectService::editMeta(const std::string &slug, const std::string &patchJson)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      try
      {
        ensureLedger();
        return editProject(slug, patchJson);
      }
      catch (...)
      {
        noteError("the edit of the facts met something it cannot say");
        return false;
      }
    }

    bool ProjectService::bumpRev(const std::string &slug)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      try
      {
        ensureLedger();
        return bumpProject(slug);
      }
      catch (...)
      {
        noteError("the moving of a mark met something it cannot say");
        return false;
      }
    }

    std::string ProjectService::projectRevsJson() const
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      try
      {
        ensureLedger();
        return revsText();
      }
      catch (...)
      {
        noteError("the marks would not come in: it met something it cannot say");
        return std::string();
      }
    }

    std::string ProjectService::repoDir(const std::string &slug) const
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      try
      {
        ensureLedger();
        if (!slugOk(slug) || gMeta.count(slug) == 0)
          return std::string();
        return projectDir(slug) + "/repo";
      }
      catch (...)
      {
        noteError("the repo of a project would not be named: it met something it cannot say");
        return std::string();
      }
    }

    // The build/run doors answer through the engine above: the same lock,
    // the same cleared note, and no throw out - a job that met something
    // unexpected says so in words and nothing else is worn.

    bool ProjectService::toolchainFor(const std::string &extension, std::string &words) const
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      words.clear();
      try
      {
        detectToolchainsIfNeeded();
        return toolchainEngine(extension, words);
      }
      catch (...)
      {
        const std::string msg = "reading the toolchains met something it cannot say";
        noteError(msg);
        words = msg;
        return false;
      }
    }

    bool ProjectService::startBuild(const std::string &slug, const std::string &relPath,
                                    std::string &jobIdOut, std::string &words)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      words.clear();
      jobIdOut.clear();
      try
      {
        ensureLedger();
        detectToolchainsIfNeeded();
        return startBuildEngine(slug, relPath, jobIdOut, words);
      }
      catch (...)
      {
        const std::string msg = "the making of a build met something it cannot say";
        noteError(msg);
        words = msg;
        return false;
      }
    }

    bool ProjectService::buildStatus(const std::string &jobId, std::string &answerJson,
                                     std::string &words) const
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      answerJson.clear();
      words.clear();
      try
      {
        return buildStatusEngine(jobId, answerJson, words);
      }
      catch (...)
      {
        const std::string msg = "the reading of a build met something it cannot say";
        noteError(msg);
        words = msg;
        return false;
      }
    }

    bool ProjectService::stopBuild(const std::string &jobId, std::string &words)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      clearNote();
      words.clear();
      try
      {
        return stopBuildEngine(jobId, words);
      }
      catch (...)
      {
        const std::string msg = "the letting-go of a build met something it cannot say";
        noteError(msg);
        words = msg;
        return false;
      }
    }

    std::string ProjectService::lastError() const
    {
      std::lock_guard<std::mutex> lock(gMutex);
      try
      {
        return gLastError;
      }
      catch (...)
      {
        return std::string();
      }
    }

  } // namespace project
} // namespace ols
