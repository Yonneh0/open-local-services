#include "Deployment.h"

#include "Common.h"
#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define OLS_POPEN _popen
#define OLS_PCLOSE _pclose
#else
#define OLS_POPEN popen
#define OLS_PCLOSE pclose
#include <pwd.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <shellapi.h>
#include <fstream>
#include <system_error>

// The boot entry names SYSTEM, so the machine asks leave for it through its own
// elevation prompt: the person sitting at this machine is the one who says yes,
// and their saying no is an ordinary answer this door carries back in words
// instead of a shrug. The command goes out as a one-line script, so nothing has
// to survive a second round of quoting on the way to a program that is already
// being started by name.
bool raisedScript(const std::string &body, std::string &why)
{
  std::error_code ec{};
  const std::string script =
      (std::filesystem::temp_directory_path() / "ols-boot-entry.cmd").string();
  std::ofstream out{script, std::ios::binary | std::ios::trunc};
  if (!out.good())
  {
    why = "could not write the script that asks for leave";
    return false;
  }
  out << "@echo off\r\n"
      << body << "\r\nexit /b %ERRORLEVEL%\r\n";
  out.close();
  SHELLEXECUTEINFOA ask{};
  ask.cbSize = sizeof(ask);
  ask.fMask = SEE_MASK_NOCLOSEPROCESS;
  ask.lpVerb = "runas";
  ask.lpFile = "cmd.exe";
  const std::string params = "/c \"" + script + "\"";
  ask.lpParameters = params.c_str();
  ask.nShow = SW_HIDE;
  if (!ShellExecuteExA(&ask))
  {
    const DWORD last = GetLastError();
    why = (last == ERROR_CANCELLED)
              ? "the person at this machine said no to raising it"
              : "this machine would not raise it (win32 error " + std::to_string(last) + ")";
    return false;
  }
  if (ask.hProcess != nullptr)
  {
    // A prompt nobody answers is a prompt that goes away: two minutes, and then
    // this door answers from what the machine's own records say afterwards.
    WaitForSingleObject(ask.hProcess, 120 * 1000);
    CloseHandle(ask.hProcess);
  }
  std::filesystem::remove(script, ec);
  return true;
}
#endif // _WIN32

namespace ols
{
  namespace deploy
  {
    namespace
    {
      using json = nlohmann::json;

      std::mutex gWordsMutex;
      std::string gWords = "nothing has been asked of this machine's manager yet";

      void rememberWords(const std::string &words)
      {
        std::lock_guard<std::mutex> lock(gWordsMutex);
        gWords = words;
      }

      std::string rememberedWords()
      {
        std::lock_guard<std::mutex> lock(gWordsMutex);
        return gWords;
      }

      std::string trim(const std::string &text)
      {
        std::string::size_type from = 0;
        while (from < text.size() && std::isspace(static_cast<unsigned char>(text[from])))
          ++from;
        std::string::size_type to = text.size();
        while (to > from && std::isspace(static_cast<unsigned char>(text[to - 1])))
          --to;
        return text.substr(from, to - from);
      }

      std::string oneLine(const std::string &text)
      {
        std::string out;
        out.reserve(text.size());
        for (const char c : text)
          out += (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
        return trim(out);
      }

      bool holds(const std::string &haystack, const std::string &needle)
      {
        return haystack.find(needle) != std::string::npos;
      }

      // The machine's own words are read without regard to their case, because
      // the same fact is spelled differently by different hands: the scheduler
      // writes its states in title case, and a comparison that demanded upper
      // case read a running task as a stopped one.
      bool sameFold(const std::string &left, const std::string &right)
      {
        if (left.size() != right.size())
          return false;
        for (size_t i = 0; i < left.size(); ++i)
          if (std::tolower(static_cast<unsigned char>(left[i])) !=
              std::tolower(static_cast<unsigned char>(right[i])))
            return false;
        return true;
      }

      bool holdsFold(const std::string &haystack, const std::string &needle)
      {
        if (needle.empty() || haystack.size() < needle.size())
          return false;
        const std::string low = [&needle] {
          std::string out;
          for (const char c : needle)
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          return out;
        }();
        for (size_t i = 0; i + low.size() <= haystack.size(); ++i)
        {
          size_t j = 0;
          for (; j < low.size(); ++j)
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) != low[j])
              break;
          if (j == low.size())
            return true;
        }
        return false;
      }

      // One ask of the machine's own manager, with its answer kept whole. These
      // commands write nothing on their own: sc, schtasks and systemctl are the
      // machine's, and what they say back is what a page ends up showing. A
      // refusal - "Access is denied" on a server that is not elevated, for one -
      // comes on the error channel, so that channel is read too: a page that
      // showed an empty answer would leave a person guessing whether anything ran.
      struct Answer
      {
        int code = -1;
        std::string text;
      };

      Answer ask(const std::string &command)
      {
        Answer out;
        const std::string withErrors = command + " 2>&1";
        FILE *pipe = OLS_POPEN(withErrors.c_str(), "r");
        if (!pipe)
        {
          out.text = "this machine would not hand the command to its shell";
          return out;
        }
        std::ostringstream collected;
        char buffer[4096];
        while (fgets(buffer, static_cast<int>(sizeof(buffer)), pipe))
          collected << buffer;
        out.code = OLS_PCLOSE(pipe);
        out.text = trim(collected.str());
        return out;
      }

      // dotenvValue and configWord stand in the open namespace beside their
      // declaration in the header - the config panel's doors and the engine
      // loader both read them, so they are not this file's own.

      std::string serviceName()
      {
        // One name law for both roads - the environment first, the file beside
        // it next, the default last.
        const std::string named = configWord("OLS_SERVICE_NAME");
        return named.empty() ? "open-local-services" : named;
      }

      // The program this server is running as, read off the machine rather than
      // guessed from a path somebody typed.
      std::string selfExe()
      {
#ifdef _WIN32
        char buffer[32768];
        const DWORD len = GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(sizeof(buffer)));
        if (len == 0 || len >= sizeof(buffer))
          return std::string();
        return std::string(buffer, len);
#else
        std::error_code ec{};
        const auto self = std::filesystem::canonical("/proc/self/exe", ec);
        return ec ? std::string() : self.string();
#endif
      }

      // The folder this server stands in: the one that holds its .env, or its webui,
      // or failing both the folder the program itself sits in. This is what a boot
      // start is pointed at, so the server starts from the same place it does now.
      std::string selfRoot()
      {
        const std::string exe = selfExe();
        std::error_code ec{};
        std::filesystem::path at = exe.empty() ? std::filesystem::current_path(ec)
                                               : std::filesystem::path(exe).parent_path();
        for (int depth = 0; depth < 6 && !ec && at.has_relative_path(); ++depth)
        {
          if (std::filesystem::exists(at / ".env") || std::filesystem::exists(at / "webui"))
            return at.string();
          at = at.parent_path();
        }
        std::filesystem::path here = std::filesystem::current_path(ec);
        return ec ? std::string(".") : here.string();
      }

      // An environment this process was given and a boot start would otherwise lose.
      std::string carryEnv(const std::string &name)
      {
        if (const char *value = std::getenv(name.c_str()))
        {
          const std::string trimmed = trim(value);
          if (!trimmed.empty())
            return name + "=" + trimmed;
        }
        return std::string();
      }

#ifdef _WIN32
      // Reading the service control manager about this name. An empty answer means
      // the manager does not know it; 1060 is what sc says in that case.
      bool readService(State &state)
      {
        const Answer query = ask("sc query \"" + state.name + "\"");
        if (query.code != 0 || holds(query.text, "1060"))
          return false;
        state.installed = true;
        state.manager = "the service control manager";
        state.running = holds(query.text, "STATE") && holds(query.text, "RUNNING");
        const Answer config = ask("sc qc \"" + state.name + "\"");
        if (holds(config.text, "AUTO_START"))
          state.startup = "automatic";
        else if (holds(config.text, "DISABLED"))
          state.startup = "disabled";
        else if (holds(config.text, "DEMAND_START"))
          state.startup = "manual";
        const std::string::size_type at = config.text.find("BINARY_PATH_NAME");
        if (at != std::string::npos)
          state.target = oneLine(config.text.substr(at + 16));
        state.detail = "service \"" + state.name + "\"";
        return true;
      }

      // One field of the task scheduler's own document. The scheduler's xml writes
      // a value between a tag and its closing tag, and escapes the quotes a
      // command carries, so the value is unescaped before it is said again.
      std::string tagValue(const std::string &text, const std::string &tag)
      {
        const std::string open = "<" + tag + ">";
        const std::string::size_type from = text.find(open);
        if (from == std::string::npos)
          return std::string();
        const std::string::size_type start = from + open.size();
        const std::string::size_type end = text.find("</" + tag + ">", start);
        if (end == std::string::npos)
          return std::string();
        const std::string raw = trim(text.substr(start, end - start));
        std::string out;
        for (std::string::size_type i = 0; i < raw.size(); ++i)
        {
          if (raw.compare(i, 5, "&amp;") == 0)
          {
            out += '&';
            i += 4;
          }
          else if (raw.compare(i, 4, "&lt;") == 0)
          {
            out += '<';
            i += 3;
          }
          else if (raw.compare(i, 4, "&gt;") == 0)
          {
            out += '>';
            i += 3;
          }
          else if (raw.compare(i, 6, "&quot;") == 0)
          {
            out += '"';
            i += 5;
          }
          else if (raw.compare(i, 6, "&apos;") == 0)
          {
            out += '\'';
            i += 5;
          }
          else
            out += raw[i];
        }
        return out;
      }

      // Reading the task scheduler, which is where a console server is set to start
      // when the machine does. /xml is read rather than /fo LIST: LIST has no
      // machine-readable field for what the task is started by - and /nh cannot
      // even be asked of LIST, so the listing this used before was refused outright,
      // which left an entry that stood being read as nothing standing. The document
      // names the three facts that matter: the task's state, what starts it, and
      // the command the machine would run.
      bool readTask(State &state, bool &refused)

      {
        const Answer query = ask("schtasks /query /tn \"" + state.name + "\" /xml");
        if (query.code != 0)
        {
          // An entry the scheduler will not show this process is not the same fact
          // as no entry: a SYSTEM task is the scheduler's to read, and saying
          // "nothing is installed" about it would be a lie.
          refused = holds(query.text, "Access is denied");
          return false;
        }
        state.installed = true;
        state.manager = "the task scheduler";
        // The scheduler writes its states in title case - Ready, Running, Queued,
        // Disabled - and a reading that demanded upper case called every running
        // task a stopped one, which is how one door came within one ask of
        // starting a second copy of a live server.
        const std::string taskState = tagValue(query.text, "State");
        state.running = sameFold(taskState, "running");
        // What starts a task is written in this document as the name of its
        // trigger element: BootTrigger for the machine's own start, LogonTrigger
        // for a person signing in, TimeTrigger and CalendarTrigger for a date or
        // an hour, and the rest for idleness, registration, an event, a change of
        // session. ONSTART is a word of schtasks's listing, not of its document,
        // so a reading that looked for it here called every boot task a manual
        // one - and said nothing at all about what started it.
        bool atBoot = false;
        std::string startedBy;
        if (holdsFold(query.text, "<BootTrigger"))
        {
          atBoot = true;
          startedBy = "when the machine starts";
        }
        else if (holdsFold(query.text, "<LogonTrigger"))
          startedBy = "when a person signs in";
        else if (holdsFold(query.text, "<SessionStateChangeTrigger"))
          startedBy = "at a change of session";
        else if (holdsFold(query.text, "<EventTrigger"))
          startedBy = "at an event the scheduler names";
        else if (holdsFold(query.text, "<IdleTrigger"))
          startedBy = "when the machine stands idle";
        else if (holdsFold(query.text, "<RegistrationTrigger"))
          startedBy = "as soon as it is registered";
        else if (holdsFold(query.text, "<CalendarTrigger") || holdsFold(query.text, "<TimeTrigger"))
          startedBy = "at a date or an hour the scheduler names";
        if (sameFold(taskState, "disabled"))
          state.startup = "disabled";
        else if (atBoot)
          state.startup = "automatic";
        else
          state.startup = "manual";
        const std::string command = tagValue(query.text, "Command");
        if (!command.empty())
        {
          std::string args = tagValue(query.text, "Arguments");
          state.target = args.empty() ? command : command + " " + args;
        }
        state.detail = "task \"" + state.name + "\" stands; " +
                       (startedBy.empty()
                            ? "the reading above does not know the scheduler's words for what starts it"
                            : "the scheduler starts it " + startedBy);
        return true;
      }
#else
      std::string lineOf(const std::string &text, const std::string &key)
      {
        const std::string::size_type at = text.find(key);
        if (at == std::string::npos)
          return std::string();
        const std::string::size_type from = at + key.size();
        const std::string::size_type to = text.find('\n', from);
        return trim(text.substr(from, to == std::string::npos ? std::string::npos : to - from));
      }

      // Reading systemd. A machine that has no unit by this name answers "not-found"
      // and "inactive", and that is the state, not a failure to read it.
      void readUnit(State &state)
      {
        const Answer enabled = ask("systemctl is-enabled " + state.name + " 2>&1");
        const Answer active = ask("systemctl is-active " + state.name + " 2>&1");
        state.running = oneLine(active.text) == "active";
        const std::string word = oneLine(enabled.text);
        if (word == "not-found" || word == "no such file" || word == "masked" || word.empty())
        {
          if (word != "masked")
            return;
        }
        state.installed = true;
        state.manager = "systemd";
        if (word == "enabled" || word == "enabled-runtime" || word == "static" || word == "alias")
          state.startup = "automatic";
        else if (word == "disabled" || word == "masked")
          state.startup = "disabled";
        else if (word == "generated")
          state.startup = "automatic";
        else
          state.startup = "manual";
        const Answer shown =
            ask("systemctl show " + state.name + " --property=FragmentPath --property=ExecStart");
        state.target = lineOf(shown.text, "ExecStart=");
        state.detail = lineOf(shown.text, "FragmentPath=");
        if (state.detail.empty())
          state.detail = "no unit file named for it";
      }

      // The unit for a machine that has none yet: this server, this folder, these
      // folders it was handed. Nothing is copied and nothing is fetched.
      std::string unitText(const State &state)
      {
        std::ostringstream out;
        out << "[Unit]\n"
            << "Description=open-local-services - the local model server\n"
            << "After=network-online.target\n"
            << "Wants=network-online.target\n\n"
            << "[Service]\n"
            << "Type=simple\n"
            << "WorkingDirectory=" << state.root << "\n"
            << "ExecStart=" << state.target << "\n"
            << "Restart=on-failure\n"
            << "RestartSec=5\n";
        for (const std::string &name : {"OLS_WEIGHTS_DIR", "OLS_DATA_DIR", "OLS_HOST"})
        {
          const std::string carried = carryEnv(name);
          if (!carried.empty())
            out << "Environment=" << carried << "\n";
        }
        out << "\n[Install]\nWantedBy=multi-user.target\n";
        return out.str();
      }
#endif

      // Letting go of this process once the answer has been handed over. A stop that
      // came from the page is a stop of the very server answering it, so the reply
      // goes first and the process follows it out.
      void letGo()
      {
        std::thread([]()
                    {
          std::this_thread::sleep_for(std::chrono::milliseconds(600));
          std::_Exit(0); })
            .detach();
      }

      std::string quote(const std::string &text)
      {
        return "\"" + text + "\"";
      }

#ifdef _WIN32
      // The hand's own rights, asked of the machine once: this server is run by
      // its boot task as SYSTEM, and a folder that refuses it - by an inherited
      // deny nobody can name - leaves every door that writes there answering in
      // refusal. When a write proves refused, this server stands a one-shot
      // task of its own: the machine's icacls grants this very account modify
      // over the data room and the tree it was handed, the entry runs itself
      // once and is gone. The rights are the ones a running server needs to
      // update itself; nothing else is asked of anyone, and the entry names
      // only these two folders.
      bool standingRepairTask(const std::string &root, std::string &why)
      {
        const std::string name = "ols-acl-repair";
        // The entry names its command right on the create line: no script file
        // anywhere. A server whose every writable folder refuses cannot be told
        // to first write a helper somewhere else - that road was walked and it
        // ends at the same closed door. What the entry does is two icacls
        // grants over named folders, then its own deletion; nothing else runs.
        const std::string progDir = selfExe().empty()
                                        ? root
                                        : std::filesystem::path(selfExe()).parent_path().string();
        const std::string who = "S-1-5-18"; // this very account: LocalSystem
        // No inner quotes anywhere in what the entry runs: paths are given by
        // their 8.3 form, which this machine answers for every folder (probed -
        // short names resolve even where a long one carries no space), and the
        // self-delete names its task without quoting. The road through nested
        // quotes was walked first and it ended with schtasks reading "/f\" as
        // one mangled option - three layers of parser between these words and
        // the command line they finally ride, and cmd breaks \" inside a quoted
        // argument no matter how carefully the outer layer is escaped.
        std::string shortRoot = root;
        std::string shortProg = progDir;
        char shortBuf[1024];
        if (GetShortPathNameA(root.c_str(), shortBuf, sizeof(shortBuf)))
          shortRoot = shortBuf;
        if (GetShortPathNameA(progDir.c_str(), shortBuf, sizeof(shortBuf)))
          shortProg = shortBuf;
        const std::string inner = "icacls " + shortRoot + "\\data /grant *" + who +
                                  ":(OI)(CI)M /T & icacls " + shortProg + " /grant *" + who +
                                  ":(OI)(CI)M & schtasks /delete /tn " + name + " /f";
        const std::string createLine = "schtasks /create /f /tn " + name +
                                       " /tr \"cmd /c " + inner +
                                       "\" /sc once /st 00:00 /ru SYSTEM /rl HIGHEST";
        const Answer made = ask(createLine);
        if (made.code != 0)
        {
          why = "schtasks /create said: " + oneLine(made.text);
          return false;
        }
        const Answer started = ask("schtasks /run /tn \"" + name + "\"");
        if (started.code != 0)
        {
          why = "schtasks /run said: " + oneLine(started.text);
          return false;
        }
        // The entry is its own worker: give it a moment, and read the folder's
        // answer rather than the task's.
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return true;
      }

      // The hand's own one-shot entry, for the machine that will not let a child
      // leave the job it was born in. The entry runs the hand's script by its
      // short name, so no inner quote has to survive the scheduler's parser, and
      // it names itself `entry` - the word under which the script deletes this
      // entry as its last act. The scheduler holds this entry beside the job of
      // the task that started the server, never inside it, which is the whole
      // reason the hand can finish a move its parent was cut off from.
      constexpr const char *kHandEntry = "ols-update-hand";

      bool standsHandEntry(const std::string &script, std::string &why)
      {
        std::string runScript = script;
        char shortBuf[1024];
        if (GetShortPathNameA(script.c_str(), shortBuf, sizeof(shortBuf)))
          runScript = shortBuf;
        else if (script.find(' ') != std::string::npos)
        {
          // The entry's action line is read twice - once by this command and
          // once by the scheduler, which puts the action together again from one
          // string - and a path with a space in it is two words to that second
          // reading. The short name is what makes one word of it; where the
          // volume keeps no short names, standing the entry would leave a task
          // that names a file no one asked for, so this says so instead.
          why = "the hand's script stands where this volume keeps no short name for it, and "
                "its path holds a space the scheduler would read as two words";
          return false;
        }
        const std::string create = "schtasks /create /f /tn " + std::string(kHandEntry) +
                                   " /tr \"cmd /c " + runScript + " entry\""
                                   " /sc once /st 00:00 /ru SYSTEM /rl HIGHEST";
        const Answer made = ask(create);
        if (made.code != 0)
        {
          why = "schtasks /create said: " + oneLine(made.text);
          return false;
        }
        const Answer started = ask("schtasks /run /tn " + std::string(kHandEntry));
        if (started.code != 0)
        {
          why = "schtasks /run said: " + oneLine(started.text);
          ask("schtasks /delete /tn " + std::string(kHandEntry) + " /f");
          return false;
        }
        return true;
      }

      // One door that shows what this server can actually do, for when its
      // refusals need reading: every probe run in order, each with its honest
      // outcome. Who am I, where does my temp live, can a file land in the
      // data room, can a task be stood. Nothing is hidden here and nothing is
      // fixed - it only says true about itself. POST-only, like every door
      // that speaks about the machine.
      bool anonymousSelfTest(std::string &words)
      {
        std::ostringstream said;
        char whoBuf[256] = {0};
        DWORD whoLen = sizeof(whoBuf);
        if (GetUserNameA(whoBuf, &whoLen))
          said << "account: " << whoBuf << "; ";
        else
          said << "account: unknown (win32 error " << GetLastError() << "); ";
        const char *tmp = std::getenv("TEMP");
        said << "temp: " << (tmp && *tmp ? tmp : "(unset)") << "; ";
        // Where the refusals actually come from, read off the machine: the
        // process's own integrity level and whether a child of its shell can
        // even find schtasks. A Low-level process is refused writes to folders
        // that any Medium process walks into freely, and it cannot reach the
        // task scheduler at all - which reads exactly like "path not found".
        {
          HANDLE tok = nullptr;
          if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
          {
            DWORD needed = 0;
            GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &needed);
            std::vector<char> buf(needed);
            if (needed > 0 &&
                GetTokenInformation(tok, TokenIntegrityLevel, buf.data(),
                                    static_cast<int>(buf.size()), &needed))
            {
              const auto *label = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buf.data());
              DWORD rid = *GetSidSubAuthority(label->Label.Sid, 0);
              switch (rid)
              {
              case SECURITY_MANDATORY_UNTRUSTED_RID: said << "integrity: untrusted; "; break;
              case SECURITY_MANDATORY_LOW_RID: said << "integrity: low; "; break;
              case SECURITY_MANDATORY_MEDIUM_RID: said << "integrity: medium; "; break;
              case SECURITY_MANDATORY_HIGH_RID: said << "integrity: high; "; break;
              case SECURITY_MANDATORY_SYSTEM_RID: said << "integrity: system; "; break;
              default: said << "integrity: rid " << rid << "; "; break;
              }
            }
            else
              said << "integrity: unreadable (win32 error " << GetLastError() << "); ";
            CloseHandle(tok);
          }
          else
            said << "integrity: no token (win32 error " << GetLastError() << "); ";
        }
        // Whether this process stands inside a job - the machine's own word, and
        // the fact that decides which road the update hand takes. A task the
        // scheduler started keeps its whole process tree in a job the scheduler
        // closes when the task's action process lets go, so a hand left inside
        // one would be taken down beside the server it waits behind.
        {
          BOOL inJob = FALSE;
          if (IsProcessInJob(GetCurrentProcess(), nullptr, &inJob))
            said << (inJob ? "job: inside (the hand leaves it or stands as its own entry); "
                           : "job: outside; ");
          else
            said << "job: unreadable (win32 error " << GetLastError() << "); ";
        }
        const Answer whereTask = ask("where schtasks");
        said << "child where schtasks: "
             << (whereTask.code == 0 ? oneLine(whereTask.text)
                                     : ("refused - " + oneLine(whereTask.text)))
             << "; ";
        // A probe file in the data room, by name and gone after.
        const State now = state();
        const std::string probe = now.root.empty()
                                      ? std::string{}
                                      : now.root + "\\data\\ols-selftest-probe.tmp";
        if (probe.empty())
          said << "root: unknown; ";
        else
        {
          std::ofstream out{probe, std::ios::binary | std::ios::trunc};
          if (out.good())
          {
            out << "probe";
            out.close();
            said << "write data/: yes; ";
            std::error_code rm{};
            std::filesystem::remove(probe, rm);
          }
          else
            said << "write data/: refused (win32 error " << GetLastError() << "); ";
        }
        // And the same probe beside the program it runs from.
        const std::string selfFolder = selfExe();
        if (!selfFolder.empty())
        {
          const std::string probe2 =
              std::filesystem::path(selfFolder).parent_path().string() + "\\ols-selftest-probe.tmp";
          std::ofstream out{probe2, std::ios::binary | std::ios::trunc};
          if (out.good())
          {
            out << "probe";
            out.close();
            said << "write program folder: yes; ";
            std::error_code rm{};
            std::filesystem::remove(probe2, rm);
          }
          else
            said << "write program folder: refused (win32 error " << GetLastError() << "); ";
        }
        // Can it see a task query at all? Read-only ask, no rights spent.
        const Answer seen = ask("schtasks /query /fo LIST");
        said << "schtasks /query: " << (seen.code == 0 ? "yes" : ("refused - " + oneLine(seen.text)))
             << "; ";
        // And the child's own eyes: what shell did the pipe hand to, and who is
        // it. When a probe inside this process refuses where its children do
        // not, that is the shape of the trouble; when they refuse alike, the
        // whole line was born confined.
        const Answer echoed = ask("echo %TEMP%");
        said << "child echo: " << (echoed.code == 0 ? oneLine(echoed.text)
                                                    : ("refused - " + oneLine(echoed.text)))
             << "; ";
        const Answer childWho = ask("whoami");
        said << "child whoami: " << (childWho.code == 0 ? oneLine(childWho.text)
                                                        : ("refused - " + oneLine(childWho.text)))
             << "; ";
        // And can the child put a file in the data room, where this process may
        // not? The probe names itself so a stale one from an older round cannot
        // be mistaken for a fresh answer.
        if (!probe.empty())
        {
          const std::string childProbe = now.root + "\\data\\ols-child-probe.txt";
          ask(("echo from-child> \"" + childProbe + "\"").c_str());
          std::error_code pec{};
          if (std::filesystem::is_regular_file(childProbe, pec))
          {
            std::ifstream in{childProbe};
            std::string got;
            std::getline(in, got);
            said << "child write data/: yes (" << oneLine(got) << ")";
            std::error_code rm{};
            std::filesystem::remove(childProbe, rm);
          }
          else
            said << "child write data/: refused";
        }
        words = said.str();
        rememberWords(words);
        return true;
      }
#endif // _WIN32: the repair task and the selftest speak windows tools
      // Whether a staged file is one of the engine libraries: the same family of
      // names the hand scripts move by. Both spellings count, because the engine
      // wears `stable-diffusion-cuda.dll` on Windows and
      // `libstable-diffusion-cuda.so` here, and a hand that named only the
      // windows spelling reported an engine crossing beside it as though it had
      // not been seen at all.
      bool namesEngineLibrary(const std::string &name)
      {
        const bool spelled = name.rfind("stable-diffusion-", 0) == 0 ||
                             name.rfind("libstable-diffusion-", 0) == 0;
        if (!spelled)
          return false;
        return (name.size() > 4 && name.compare(name.size() - 4, 4, ".dll") == 0) ||
               (name.size() > 3 && name.compare(name.size() - 3, 3, ".so") == 0);
      }

      // The update hand, run by a detached helper running the script written
      // here. It waits for this process to let go of its file (a running image
      // is refused its own rename - that answer is the clock), moves what
      // stands staged into place with the old one set aside under .old and put
      // back if the staged file cannot land, then starts the machine's road:
      // schtasks /end + /run when a task holds this server, systemctl restart
      // where systemd does, and plain start of the new program as the last
      // honest resort. Everything here happens after this process is gone, so
      // nothing it asks needs rights beyond what its parent already carried -
      // which is why an elevated serve ends its own task and starts itself
      // again with no prompt anywhere. Words go to a log beside the data
      // folder; nobody reads a detached console.
      bool anonymousUpdateHand(const std::string &exeName, std::string &words)
      {
        const State now = state();
        // The folder the program itself sits in - not selfRoot's walk to .env:
        // the running binary stands under src\ while its tree's root stands a
        // level above, and the hand moves the file this process actually runs
        // from. The staging room belongs to the tree, so it rides the state's
        // reading of where the server is home.
        const std::string selfFolder = selfExe();
        if (selfFolder.empty())
        {
          words = "this server cannot name the program it runs as: nothing was handed.";
          rememberWords(words);
          return false;
        }
        const std::filesystem::path programPath(selfFolder);
        if (exeName.empty() || programPath.filename().string() != exeName)
        {
          words = "the program this server runs as is not named " + exeName +
                  ": nothing was handed to the wrong file.";
          rememberWords(words);
          return false;
        }
        const std::string root = now.root;
        if (root.empty())
        {
          words = "this server cannot name its own folder: nothing was handed.";
          rememberWords(words);
          return false;
        }
        std::error_code ec{};
        const std::filesystem::path staged = std::filesystem::path(root) / "bin" / exeName;
        if (!std::filesystem::is_regular_file(staged, ec))
        {
          words = "nothing stands staged at bin/" + exeName +
                  ": build first (run.bat or build-rel.bat stages there), and this door answers "
                  "without stopping anything.";
          rememberWords(words);
          return false;
        }
        std::ostringstream said;
        said << "the hand was taken: bin/" << exeName << " stands staged; ";
        // The engine libraries ride with the binary: however many stand staged
        // under the engine's own name cross the same hand, so a staged set
        // moves as one set.
        size_t stagedEngines = 0;
        {
          const std::filesystem::path binRoom = std::filesystem::path(root) / "bin";
          std::error_code seen{};
          if (std::filesystem::is_directory(binRoom, seen))
          {
            for (const auto &entry : std::filesystem::directory_iterator(binRoom, seen))
            {
              if (!entry.is_regular_file())
                continue;
              const std::string name = entry.path().filename().string();
              if (namesEngineLibrary(name))
                ++stagedEngines;
            }
          }
        }
        if (stagedEngines > 0)
          said << stagedEngines << " engine librar" << (stagedEngines == 1 ? "y stands" : "ies stand")
               << " staged with it; ";
#ifdef _WIN32
        const bool byTask = now.installed && now.manager == "the task scheduler";
        const bool byService = now.installed && now.manager == "the service control manager";
        // The program's own folder: the binary and the image library beside it
        // move where they stand, not where the tree's root happens to be. A
        // scheduled task that ends itself is left for its scheduler - ending a
        // running task takes the hand down with it, so /run alone brings the
        // new program up; only an already-stopped server needs both asks.
        const std::string progDir = programPath.parent_path().string();
        // The hand's script stands beside the log it writes, in the data folder
        // this server already owns - not %TEMP%, which for a boot-started task
        // belongs to another account and refuses even the write. When even that
        // folder refuses, these words name the file and the win32 behind it:
        // a door that only says "could not" leaves nobody to ask why.
        const std::filesystem::path handDir = std::filesystem::path(root) / "data";
        std::error_code mk{};
        std::filesystem::create_directories(handDir, mk);
        const std::string script = (handDir / "ols-update-hand.cmd").string();
        std::ofstream out{script, std::ios::binary | std::ios::trunc};
        if (!out.good())
        {
          // The write's own error is read here, before anything else spends a
          // win32 last-error of its own: standing the repair first handed
          // these words the repair's pipe error instead of the write's real
          // refusal, and a wrong number in a refusal line wastes the whole day.
          const DWORD writeErr = GetLastError();
          // The write was refused: stand the repair once and answer honestly.
          std::string why{};
          const bool stood = standingRepairTask(root, why);
          words = "could not write the hand's script at " + script + " (win32 error " +
                  std::to_string(writeErr) + "); " +
                  (stood ? "this server stood its own rights-repair task - ask this door "
                           "again in a moment, it should answer by then"
                         : ("the repair could not stand either: " + why)) +
                  ": nothing was handed.";
          rememberWords(words);
          return false;
        }
        out << "@echo off\r\n"
               "set \"ROOT=" << root << "\"\r\n"
               "set \"PROG=" << progDir << "\"\r\n"
               "set \"LOG=%ROOT%\\data\\update-hand.log\"\r\n"
               "set TRIES=0\r\n"
               "echo [%DATE% %TIME%] the hand waits for the running program to let go>>\"%LOG%\"\r\n"
               ":again\r\n"
               "set /a TRIES+=1\r\n"
               "move /y \"%PROG%\\" << exeName << "\" \"%PROG%\\" << exeName << ".old\" >nul 2>&1\r\n"
               "if errorlevel 1 (\r\n"
               "  if %TRIES% geq 300 (\r\n"
               "    echo [%DATE% %TIME%] the running program would not let go of its file in five minutes: the old one starts again>>\"%LOG%\"\r\n"
               "    goto :restart\r\n"
               "  )\r\n"
               "  ping -n 2 127.0.0.1 >nul\r\n"
               "  goto :again\r\n"
               ")\r\n"
               "move /y \"%ROOT%\\bin\\" << exeName << "\" \"%PROG%\\" << exeName << "\" >nul 2>&1\r\n"
               "if errorlevel 1 (\r\n"
               "  echo [%DATE% %TIME%] the staged binary would not take the old one's place: putting the old one back>>\"%LOG%\"\r\n"
               "  move /y \"%PROG%\\" << exeName << ".old\" \"%PROG%\\" << exeName << "\" >nul 2>&1\r\n"
               "  echo [%DATE% %TIME%] the old one stands again and is started>>\"%LOG%\"\r\n"
               "  goto :restart\r\n"
               ")\r\n";
        // The engine libraries move beside the binary as a set: every library
        // staged under the engine's own name crosses, and a library that will
        // not move leaves the old one standing - named in the log, not lost.
        // The set is asked for only where one stands: a `for` over a pattern
        // that matches nothing still runs its body once, with the pattern as
        // the file's name, and the log then blames a move that was never owed.
        out << "if exist \"%ROOT%\\bin\\stable-diffusion-*.dll\" for %%F in (\"%ROOT%\\bin\\stable-diffusion-*.dll\") do (\r\n"
            << "  move /y \"%%F\" \"%PROG%\\%%~nxF\" >nul 2>&1 || echo [%DATE% %TIME%] the staged engine library %%~nxF would not move: the old one stands>>\"%LOG%\"\r\n"
            << ")\r\n";
        out << "echo [%DATE% %TIME%] the staged binary took the running one's place>>\"%LOG%\"\r\n"
               ":restart\r\n";
        if (byTask)
        {
          // The task that holds this server is still its own cmd when the move
          // lands: the scheduler refuses /run of a task it still calls Running,
          // and the hand is a child of that very cmd. So the hand waits for the
          // scheduler's own word - Ready - and only then asks; the ask itself is
          // repeated a few times in case the task was winding down between the
          // query and the run. A scheduler that would not have it says so in the
          // log, with the new binary standing in place unstarted - the hand
          // never pretends a refused ask ran.
          out << "set RTRIES=0\r\n"
              << ":readyagain\r\n"
              << "set /a RTRIES+=1\r\n"
              << "schtasks /query /tn \"" << now.name << "\" /fo LIST 2>nul | findstr /i /c:\"Status\" | findstr /i /c:\"Ready\" >nul\r\n"
              << "if errorlevel 1 (\r\n"
              << "  if %RTRIES% geq 30 (\r\n"
              << "    echo [%DATE% %TIME%] the task never stood Ready within half a minute: asking anyway>>\"%LOG%\"\r\n"
              << "    goto :runask\r\n"
              << "  )\r\n"
              << "  ping -n 2 127.0.0.1 >nul\r\n"
              << "  goto :readyagain\r\n"
              << ")\r\n"
              << ":runask\r\n"
              << "schtasks /run /tn \"" << now.name << "\" >>\"%LOG%\" 2>&1\r\n"
              << "if errorlevel 1 (\r\n"
              << "  if %RTRIES% geq 33 (\r\n"
              << "    echo [%DATE% %TIME%] the scheduler refused to run the task: the new binary stands in place, unstarted>>\"%LOG%\"\r\n"
              << "    goto :handdone\r\n"
              << "  )\r\n"
              << "  ping -n 3 127.0.0.1 >nul\r\n"
              << "  goto :runask\r\n"
              << ")\r\n"
              << "echo [%DATE% %TIME%] the hand asked the task to run again>>\"%LOG%\"\r\n";
          said << "the task that holds this server is asked to run again on the new binary - "
                  "this server stops now, and its scheduler brings it back at the level the "
                  "task was set to.";
        }
        else if (byService)
        {
          out << "sc start \"" << now.name << "\" >>\"%LOG%\" 2>&1\r\n";
          said << "the service manager starts this name again on the new binary; this server stops now.";
        }
        else
        {
          // No manager holds this server, so the new program comes from the hand
          // itself, as its own plain child - which carries the level this server
          // stands at. No runas verb is asked here. The entry point's law is that
          // a server runs nowhere but at its manager's level, so the level this
          // process already holds is the level the next one needs, and asking the
          // machine again for a leave it gave once would plant a prompt in the one
          // road that promises there is none. A refusal leaves everything moved
          // and standing; the words say so.
          out << "cd /d \"%ROOT%\"\r\n"
              << "start \"open-local-services\" /d \"%ROOT%\" \"%PROG%\\" << exeName << "\"\r\n";
          said << "no manager holds this server: the new program starts from the hand, at the "
                  "level this server stands at, with no prompt asked of anyone. this server "
                  "stops now.";
        }
        out << ":handdone\r\n"
            << "echo [%DATE% %TIME%] the hand is done>>\"%LOG%\"\r\n"
            << "if \"%~1\"==\"entry\" schtasks /delete /tn " << kHandEntry << " /f >nul 2>&1\r\n"
               "exit /b 0\r\n";
        out.close();
        // Where the hand may stand. The machine's scheduler keeps every process of
        // a running task inside one job, and closes that job - with everything
        // still inside it - the moment the task's own action process lets go. A
        // hand born as a child of the server therefore goes down with the server
        // it was waiting behind, before a single file has moved; a box whose home
        // is a scheduled task measured exactly that, and left no log line behind
        // to show for it. So where a manager's road stands this server up, and
        // this process finds itself inside such a job, the hand stands as a
        // one-shot entry of its own: an entry the scheduler holds beside this
        // task's job, never inside it, which deletes itself as its last word.
        // Only where no such road holds the server - a program started from a
        // shell, with nothing behind it to bring it back - is the hand kept a
        // child of this process, and then it asks to be started outside any job
        // it was born in.
        BOOL insideJob = FALSE;
        const bool inJob =
            IsProcessInJob(GetCurrentProcess(), nullptr, &insideJob) != FALSE && insideJob != FALSE;
        bool handStands = false;
        std::string refusal;
        if (inJob && (byTask || byService))
        {
          std::string why{};
          if (standsHandEntry(script, why))
            handStands = true;
          else
            refusal = "standing the hand as an entry of its own was refused: " + why;
        }
        if (!handStands)
        {
          DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
          if (inJob)
            flags |= CREATE_BREAKAWAY_FROM_JOB;
          STARTUPINFOA look{};
          look.cb = sizeof(look);
          PROCESS_INFORMATION gone{};
          std::string line = "cmd.exe /c \"" + script + "\"";
          if (CreateProcessA(nullptr, line.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr,
                             &look, &gone))
          {
            CloseHandle(gone.hThread);
            CloseHandle(gone.hProcess);
            handStands = true;
          }
          else
          {
            const DWORD childErr = GetLastError();
            refusal = "this machine would not start the hand (win32 error " +
                      std::to_string(childErr) + ")";
            if (inJob)
              refusal += "; this server stands inside a job the machine closes when its own task "
                         "lets go, and a hand is not left to die in it mid-move";
            refusal += ": nothing was stopped.";
          }
        }
        if (!handStands)
        {
          words = refusal;
          rememberWords(words);
          return false;
        }
#else
        if (now.installed && now.manager == "systemd")
        {
          // systemd restarts the unit on its own clock; the move waits behind a
          // helper for the same reason the Windows hand does - a running
          // program keeps its file until it is gone. The script stands in the
          // data folder this server owns, not /tmp: what a root service writes
          // there belongs to root and refuses a hand started any other way.
          const std::filesystem::path handDir = std::filesystem::path(root) / "data";
          std::error_code mk{};
          std::filesystem::create_directories(handDir, mk);
          const std::string script = (handDir / "ols-update-hand.sh").string();
          std::ofstream out{script, std::ios::binary | std::ios::trunc};
          if (!out.good())
          {
            words = "could not write the script that waits behind this server: nothing was handed.";
            rememberWords(words);
            return false;
          }
          out << "#!/bin/sh\n"
              << "ROOT=\"" << root << "\"\n"
              << "PROG=\"" << programPath.parent_path().string() << "\"\n"
              << "LOG=\"$ROOT/data/update-hand.log\"\n"
              << "echo \"[$(date)] the hand waits for the running program to let go\" >>\"$LOG\"\n"
              << "tries=0; moved=0\n"
              << "while [ $tries -lt 300 ]; do\n"
              << "  tries=$((tries+1))\n"
              << "  if mv \"$PROG/" << exeName << "\" \"$PROG/" << exeName << ".old\" 2>/dev/null; then moved=1; break; fi\n"
              << "  sleep 1\n"
              << "done\n"
              << "if [ $moved -eq 0 ]; then\n"
              << "  echo \"[$(date)] the running program would not let go of its file in five minutes: the unit starts again on the old one\" >>\"$LOG\"\n"
              << "  systemctl restart \"" << now.name << "\" >>\"$LOG\" 2>&1\n"
              << "  exit 1\n"
              << "fi\n"
              << "if ! mv \"$ROOT/bin/" << exeName << "\" \"$PROG/" << exeName << "\" 2>/dev/null; then\n"
              << "  echo \"[$(date)] the staged binary would not take the old one's place: putting the old one back\" >>\"$LOG\"\n"
              << "  mv \"$PROG/" << exeName << ".old\" \"$PROG/" << exeName << "\" 2>/dev/null\n"
              << "  echo \"[$(date)] the old one stands again and the unit starts on it\" >>\"$LOG\"\n"
              << "  systemctl restart \"" << now.name << "\" >>\"$LOG\" 2>&1\n"
              << "  exit 1\n"
              << "fi\n";
          out << "for f in \"$ROOT\"/bin/libstable-diffusion-*.so; do\n"
              << "  [ -e \"$f\" ] || continue\n"
              << "  mv \"$f\" \"$PROG/$(basename \"$f\")\" 2>/dev/null || echo \"[$(date)] the staged engine library $(basename \"$f\") would not move: the old one stands\" >>\"$LOG\"\n"
              << "done\n";
          out << "echo \"[$(date)] the staged binary took the running one's place\" >>\"$LOG\"\n"
              << "systemctl restart \"" << now.name << "\" >>\"$LOG\" 2>&1\n"
              << "echo \"[$(date)] the hand is done\" >>\"$LOG\"\n";
          out.close();
          // The hand may not ride in the unit's own cgroup: when this server's
          // main process lets go, systemd sweeps every process the unit still
          // holds - and a helper forked here dies in that sweep before a single
          // file is moved. So the hand stands up as a transient unit of its
          // own: a sibling beside the service, invisible to its stopping. Both
          // names are single-quoted on the way to the shell: a folder with a
          // space in its name is an ordinary folder, and a bare working
          // directory would hand the rest of that path to systemd as arguments
          // that name no file at all.
          std::ostringstream handCommand;
          handCommand << "systemd-run --quiet --collect --unit=ols-update-hand"
                      << " --working-directory='" << root << "'"
                      << " /bin/sh '" << script << "'";
          const Answer stood = ask(handCommand.str());
          if (stood.code != 0)
          {
            // Without the transient road the hand would die in the sweep, so
            // this server says so plainly and stands aside unstopped - worse
            // than a hand that cannot hold is a hand that breaks mid-move. The
            // machine's own words are said with the refusal: the commonest one
            // is that a hand of this name is still running, and a second one
            // would move the same files twice.
            words = "this machine would not stand the hand beside the unit (code " +
                    std::to_string(stood.code) + "); " + oneLine(stood.text) +
                    ": nothing was stopped.";
            rememberWords(words);
            return false;
          }
          said << "systemd restarts its unit on the new binary; this server stops now.";
        }
        else
        {
          // No manager: the helper waits for the file, moves it, and starts the
          // new program itself - as this process's own detached child, which is
          // the level this server stands at. The script stands in the data
          // folder this server owns, not /tmp.
          const std::filesystem::path handDir = std::filesystem::path(root) / "data";
          std::error_code mk{};
          std::filesystem::create_directories(handDir, mk);
          const std::string script = (handDir / "ols-update-hand.sh").string();
          std::ofstream out{script, std::ios::binary | std::ios::trunc};
          if (!out.good())
          {
            words = "could not write the script that waits behind this server: nothing was handed.";
            rememberWords(words);
            return false;
          }
          out << "#!/bin/sh\n"
              << "ROOT=\"" << root << "\"\n"
              << "PROG=\"" << programPath.parent_path().string() << "\"\n"
              << "LOG=\"$ROOT/data/update-hand.log\"\n"
              << "tries=0; moved=0\n"
              << "while [ $tries -lt 300 ]; do\n"
              << "  tries=$((tries+1))\n"
              << "  if mv \"$PROG/" << exeName << "\" \"$PROG/" << exeName << ".old\" 2>/dev/null; then moved=1; break; fi\n"
              << "  sleep 1\n"
              << "done\n"
              << "if [ $moved -eq 0 ]; then\n"
              << "  echo \"[$(date)] the running program would not let go of its file in five minutes: starting the old one again\" >>\"$LOG\"\n"
              << "  cd \"$ROOT\" && nohup \"$PROG/" << exeName << "\" >>\"$LOG\" 2>&1 &\n"
              << "  exit 1\n"
              << "fi\n"
              << "if ! mv \"$ROOT/bin/" << exeName << "\" \"$PROG/" << exeName << "\" 2>/dev/null; then\n"
              << "  mv \"$PROG/" << exeName << ".old\" \"$PROG/" << exeName << "\" 2>/dev/null\n"
              << "  echo \"[$(date)] the old one stands again and starts\" >>\"$LOG\"\n"
              << "  cd \"$ROOT\" && nohup \"$PROG/" << exeName << "\" >>\"$LOG\" 2>&1 &\n"
              << "  exit 1\n"
              << "fi\n";
          out << "for f in \"$ROOT\"/bin/libstable-diffusion-*.so; do\n"
              << "  [ -e \"$f\" ] || continue\n"
              << "  mv \"$f\" \"$PROG/$(basename \"$f\")\" 2>/dev/null || echo \"[$(date)] the staged engine library $(basename \"$f\") would not move: the old one stands\" >>\"$LOG\"\n"
              << "done\n";
          out << "cd \"$ROOT\" && nohup \"$PROG/" << exeName << "\" >>\"$LOG\" 2>&1 &\n";
          out.close();
          const pid_t runner = fork();
          if (runner == 0)
          {
            execl("/bin/sh", "sh", script.c_str(), static_cast<char *>(nullptr));
            _exit(1);
          }
          if (runner < 0)
          {
            words = "this machine would not start the hand: nothing was stopped.";
            rememberWords(words);
            return false;
          }
          said << "no manager holds this server: the new program starts from the hand, at the "
                  "level this server stands at. this server stops now.";
        }
#endif
        words = said.str();
        rememberWords(words);
        letGo();
        return true;
      }
    } // namespace

    // The config words, in the open: the environment first, the .env beside the
    // program next, empty last. The engine loader reads its search folder and
    // shape this way, the deployment roads their service name, and a
    // machine-started server hears what the config panel wrote even though no
    // one exported it into its environment.
    std::string dotenvValue(const std::string &key)
    {
      const std::string want = key + "=";
      std::filesystem::path at{std::filesystem::path(selfProgram()).parent_path()};
      for (int depth = 0; depth < 8 && !at.empty(); ++depth)
      {
        const std::filesystem::path env = at / ".env";
        std::error_code seen{};
        if (std::filesystem::is_regular_file(env, seen))
        {
          std::ifstream in{env};
          std::string line{};
          while (std::getline(in, line))
          {
            if (line.rfind(want, 0) == 0)
            {
              const std::string trimmed = trim(line.substr(want.size()));
              if (!trimmed.empty())
                return trimmed;
            }
          }
        }
        const std::filesystem::path above = at.parent_path();
        if (above == at)
          break;
        at = above;
      }
      return std::string();
    }

    std::string configWord(const std::string &name)
    {
      if (const char *named = std::getenv(name.c_str()))
      {
        const std::string trimmed = trim(named);
        if (!trimmed.empty())
          return trimmed;
      }
      return dotenvValue(name);
    }

    // The header's update hand stands on the anonymous one above: it wrote and
    // started the waiting script, weighed what stands staged, and stopped this
    // server behind its own answer. Nothing else stands between a bin/ binary
    // and the machine's manager starting it.
    bool updateHand(const std::string &exeName, std::string &words)
    {
      return anonymousUpdateHand(exeName, words);
    }

    // What this server can truly do, told straight: account, temp, whether a
    // file lands in its data room and program folder, whether it can see the
    // task scheduler at all. For the day a hand refuses and nobody believes
    // the folders are innocent.
    bool selfTest(std::string &words)
    {
#ifdef _WIN32
      return anonymousSelfTest(words);
#else
      // Every probe above is a windows tool: tokens, schtasks, the 8.3 road.
      // Here the machine answers the same questions with its own words, and
      // the service doors state them - this one says what it cannot.
      words = "the standing probes speak windows tools (tokens, schtasks, icacls); "
              "on this machine the level is uid " + std::to_string(geteuid()) +
              ", and /api/service answers what stands";
      return true;
#endif
    }

    // The header's reading of the running program: the same path these helpers
    // read, named so it does not stand twice under one name.
    std::string selfProgram()
    {
      return selfExe();
    }

#ifdef _WIN32
    // The administrator group's SID (S-1-5-32-544), spelled out in bytes
    // instead of asked of the machine: revision 1, two sub-authorities, the NT
    // authority (00-00-00-00-00-05), then 32 and 544, little-endian.
    namespace
    {
      PSID adminsSid()
      {
        static unsigned char kAdmins[] = {
            0x01, 0x02,                          // revision 1, two sub-authorities
            0x00, 0x00, 0x00, 0x00, 0x00, 0x05,  // SECURITY_NT_AUTHORITY
            0x20, 0x00, 0x00, 0x00,              // 32   - builtin domain
            0x20, 0x02, 0x00, 0x00               // 544  - Administrators
        };
        return reinterpret_cast<PSID>(kAdmins);
      }

      // The machine's own account, SYSTEM (S-1-5-18), spelled the same way.
      // The boot task of a Windows box stands at this account, and it wears no
      // administrators alias at all: its own SID and the System label are the
      // whole of its leave.
      PSID systemAccountSid()
      {
        static unsigned char kSystem[] = {
            0x01, 0x01,                          // revision 1, one sub-authority
            0x00, 0x00, 0x00, 0x00, 0x00, 0x05,  // SECURITY_NT_AUTHORITY
            0x12, 0x00, 0x00, 0x00               // 18   - LOCAL_SYSTEM_RID
        };
        return reinterpret_cast<PSID>(kSystem);
      }

      struct LevelReading
      {
        bool admins = false;    // the administrators group, held for real
        bool machine = false;   // the SYSTEM account itself
        bool denyOnly = false;  // the group, marked deny-only by the filter
        bool labelRead = false;
        DWORD rid = 0;
        std::string label = "unread";
        std::string account = "unread";
      };

      std::string integrityName(DWORD rid)
      {
        if (rid >= SECURITY_MANDATORY_SYSTEM_RID)
          return "System";
        if (rid >= SECURITY_MANDATORY_HIGH_RID)
          return "High";
        if (rid >= SECURITY_MANDATORY_MEDIUM_PLUS_RID)
          return "MediumPlus";
        if (rid >= SECURITY_MANDATORY_MEDIUM_RID)
          return "Medium";
        return "Low";
      }

      LevelReading readLevel()
      {
        LevelReading read{};
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
          return read;
        DWORD needed = 0;
        // The machine's own account sits in the token's user, not necessarily
        // in its group list - a task running as SYSTEM proved exactly that: its
        // groups named the administrators alias and never the machine itself.
        // Ask the user first, then the groups, and either answer counts.
        GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        if (needed > 0)
        {
          std::vector<char> user(needed);
          if (GetTokenInformation(token, TokenUser, user.data(),
                                  static_cast<DWORD>(user.size()), &needed))
          {
            const auto *tu = reinterpret_cast<const TOKEN_USER *>(user.data());
            if (EqualSid(tu->User.Sid, systemAccountSid()))
              read.machine = true;
          }
        }
        needed = 0;
        GetTokenInformation(token, TokenGroups, nullptr, 0, &needed);
        if (needed > 0)
        {
          std::vector<char> groups(needed);
          if (GetTokenInformation(token, TokenGroups, groups.data(),
                                  static_cast<DWORD>(groups.size()), &needed))
          {
            const auto *list = reinterpret_cast<const TOKEN_GROUPS *>(groups.data());
            for (DWORD i = 0; i < list->GroupCount; ++i)
            {
              if (EqualSid(list->Groups[i].Sid, adminsSid()))
              {
                // Deny-only is the filtered token's shadow of the group, and a
                // disabled one is the same refusal written another way: neither
                // answers for the level.
                if (list->Groups[i].Attributes & SE_GROUP_USE_FOR_DENY_ONLY)
                  read.denyOnly = true;
                else if (list->Groups[i].Attributes & SE_GROUP_ENABLED)
                  read.admins = true;
              }
              else if (EqualSid(list->Groups[i].Sid, systemAccountSid()) &&
                       (list->Groups[i].Attributes & SE_GROUP_ENABLED))
                read.machine = true;
            }
          }
        }
        needed = 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &needed);
        if (needed > 0)
        {
          std::vector<char> label(needed);
          if (GetTokenInformation(token, TokenIntegrityLevel, label.data(),
                                  static_cast<DWORD>(label.size()), &needed))
          {
            const auto *ml =
                reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(label.data());
            read.rid = *GetSidSubAuthority(ml->Label.Sid, 0);
            read.label = integrityName(read.rid);
            read.labelRead = true;
          }
        }
        char who[256] = {0};
        DWORD whoLen = static_cast<DWORD>(sizeof(who));
        if (GetUserNameA(who, &whoLen))
          read.account = std::string(who, whoLen > 0 ? whoLen - 1 : 0);
        CloseHandle(token);
        return read;
      }
    } // namespace

    bool carriesManagerLevel()
    {
      const LevelReading read = readLevel();
      // The integrity floor proves one thing for the admins token: it is the
      // elevated one and not the filtered shadow UAC hands out. For the
      // machine account the floor answers a different question, and X1 proved
      // it the hard way: a task registered without the highest privileges
      // hands SYSTEM a LOW label, and that confined token cannot write inside
      // the tree it serves (folders carry an explicit no-write-up for Low),
      // cannot stand a task, cannot hand - a manager in name only. So the
      // machine answers for itself at the labels the machine actually works
      // at, medium and above, and only the scheduler's misregistration meets
      // the refusal now.
      if (read.machine)
        return read.labelRead && read.rid >= SECURITY_MANDATORY_MEDIUM_RID;
      return read.admins && read.labelRead &&
             read.rid >= SECURITY_MANDATORY_HIGH_RID;
    }

    std::string managerLevelWords()
    {
      const LevelReading read = readLevel();
      std::string words = "account " + read.account + ", integrity " + read.label;
      if (read.admins)
        words += ", the administrators group held";
      else if (read.denyOnly)
        words += ", the administrators group deny-only";
      else
        words += ", no administrators group";
      if (read.machine)
        words += ", the machine account itself";
      return words;
    }
#else
    bool carriesManagerLevel()
    {
      return geteuid() == 0;
    }

    std::string managerLevelWords()
    {
      std::string words = "uid " + std::to_string(geteuid());
      if (struct passwd *pw = getpwuid(geteuid()))
        words += " (" + std::string(pw->pw_name) + ")";
      return words;
    }
#endif

    State state()
    {
      State state;
      state.name = serviceName();
      state.root = selfRoot();
      state.target = selfExe();
      if (state.target.empty())
        state.target = "open-local-services";
#ifdef _WIN32
      if (readService(state))
        return state;
      state = State{};
      state.name = serviceName();
      state.root = selfRoot();
      bool refusedToSay = false;
      if (readTask(state, refusedToSay))
        return state;
      state = State{};
      state.name = serviceName();
      state.root = selfRoot();
      state.target = selfExe();
      state.startup = "not installed";
      state.detail = refusedToSay
                         ? "this machine's task scheduler will not show its entry for this name to "
                           "this process - reading it is the manager level's to do, and this server "
                           "does not stand at it"
                         : "this machine has neither a service nor a task named for this server";
#else
      readUnit(state);
      if (!state.installed)
      {
        state.startup = "not installed";
        state.detail = "systemd has no unit named \"" + state.name + "\"";
      }
#endif
      return state;
    }

    std::string stateJson()
    {
      const State now = state();
      json out;
      out["name"] = now.name;
      out["manager"] = now.manager;
      out["installed"] = now.installed;
      out["running"] = now.running;
      out["startup"] = now.startup;
      out["starts_on_boot"] = now.startup == "automatic";
      out["target"] = now.target;
      out["root"] = now.root;
      out["detail"] = now.detail;
      out["last_words"] = rememberedWords();
      // What a page needs to know before it offers a hand: setting this on a
      // Windows box means the task scheduler, and a hand may be refused because
      // this process does not carry the leave the manager needs. The level is
      // read off this process's own token - the same law the entry point stands
      // under - rather than asked of a child command: `net session` answers 0
      // even for a process that holds no elevation at all.
      out["elevated"] = carriesManagerLevel();
      out["level"] = managerLevelWords();
      out["platform"] =
#ifdef _WIN32
          "windows";
#else
          "linux";
#endif
      out["asks"] = json::array(
#ifdef _WIN32
          {"sc", "schtasks"}
#else
          {"systemctl"}
#endif
      );
      return out.dump();
    }

    // The soft hand: starting again with the program that stands on disk now, as
    // this process's own child. A child carries the level its parent stands at,
    // so a machine that already raised this server is not asked again - the words
    // of the answer are written first, and this process waits for the child until
    // that child stops, which is what keeps the window that started it honest.
    static std::vector<std::string> startWords;
    static std::atomic<bool> pendingHand{false};

    void rememberStart(const std::vector<std::string> &args)
    {
      startWords = args;
    }

    bool softReloadRequested()
    {
      return pendingHand;
    }

    void handOverNow()
    {
      pendingHand = false;
      const std::string program = selfExe();
      if (program.empty())
      {
        std::cout << "this server cannot name the program it runs as: it stays as it is." << std::endl;
        return;
      }
#ifdef _WIN32
      std::string params;
      for (const std::string &arg : startWords)
      {
        if (!params.empty())
          params += ' ';
        params += '"';
        params += arg;
        params += '"';
      }
      SHELLEXECUTEINFOA ask{};
      ask.cbSize = sizeof(ask);
      ask.fMask = SEE_MASK_NOCLOSEPROCESS;
      // No verb is asked of the machine: the child comes as this process's own,
      // and the level this process stands at is the level it carries.
      ask.lpFile = program.c_str();
      ask.lpParameters = params.empty() ? nullptr : params.c_str();
      // The child gets no window of its own: its words go to the console the
      // program itself chooses, which is why the note above says the new
      // program answers in its own window.
      ask.nShow = SW_HIDE;
      if (!ShellExecuteExA(&ask))
      {
        const DWORD last = GetLastError();
        std::cout << "this machine would not start the program that stands on disk (win32 error "
                  << last << "): this server keeps running as it stands." << std::endl;
        return;
      }
      if (ask.hProcess != nullptr)
      {
        WaitForSingleObject(ask.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(ask.hProcess, &code);
        CloseHandle(ask.hProcess);
        std::_Exit(static_cast<int>(code));
      }
#else
      // The words are put back the way a shell reads them, because the program is
      // asked of through one.
      auto shellQuote = [](const std::string &text)
      {
        std::string out = "'";
        for (const char c : text)
        {
          if (c == '\'')
            out += "'\\''";
          else
            out += c;
        }
        out += "'";
        return out;
      };
      std::string command = shellQuote(program);
      for (const std::string &arg : startWords)
        command += ' ' + shellQuote(arg);
      const int code = std::system(command.c_str());
      std::_Exit(code == -1 ? 1 : code);
#endif
    }

    bool act(const std::string &action, std::string &words)
    {
      const State now = state();
      std::ostringstream said;
      bool done = false;

      if (action == "restart")
      {
        // The soft hand needs nothing of the machine's manager: the child comes
        // as this process's own, so the level this server stands at is carried,
        // and no prompt is raised. What is replaced is the program on disk, which
        // is what a person who just built this server wants hand in hand.
        pendingHand = true;
        said << "this server will start the program that stands on disk now as its own child, "
                "at the level this server stands at; this server's words stop with its answer, "
                "and the new program answers in its own window.";
        words = said.str();
        return true;
      }

#ifdef _WIN32
      const bool byService = now.installed && now.manager == "the service control manager";
      const std::string name = quote(now.name);
      if (action == "startup_on")
      {
        if (byService)
        {
          const Answer set = ask("sc config " + name + " start= auto");
          said << "sc config said: " << oneLine(set.text) << "; ";
          const Answer start = ask("sc start " + name);
          said << "sc start said: " << oneLine(start.text);
          done = set.code == 0;
        }
        else if (now.installed && now.startup == "automatic")
        {
          // The entry that already starts this server at boot is one thing; a
          // second ask would put a second copy of the same server on the machine.
          said << "this machine already starts this server when the machine starts; ";
          done = true;
        }
        else if (now.installed && now.startup == "disabled")
        {
          // The machine was told to leave this entry alone at boot; taking that
          // word back is one ask, and the entry its owner set keeps everything
          // else it was given.
          const Answer set = ask("schtasks /change " + name + " /enable");
          said << "schtasks /change said: " << oneLine(set.text)
               << "; the entry that stands is enabled again, with the start it was given";
          done = set.code == 0;
        }
        else if (now.installed)
        {
          // An entry that stands and starts some other way belongs to the hand
          // that set it: its folder, its words and its trigger are that hand's
          // choice, and this door is not allowed to write a new entry over the
          // top of one it did not make. What stands is said again, with the two
          // asks that put this server's own entry in its place.
          said << "an entry named " << now.name << " already stands on this machine - "
               << now.detail << ". This door does not write over an entry it did not set: ask this "
               "door for \"remove\", then for this one again, to have this server set an entry of "
               "its own - or mend the standing entry where it was set";
          done = false;
        }
        else
        {
          // A console program is not carried by the service control manager, so this
          // machine's start-on-boot is a task that starts when the machine does and
          // runs this very server from the folder it stands in.
          const std::string runLine = quote(now.target) + " --home " + quote(now.root);
          const std::string createLine = "schtasks /create /f /tn " + name + " /tr " +
                                         quote(runLine) + " /sc onstart /ru SYSTEM /rl HIGHEST";
          const Answer made = ask(createLine);
          done = made.code == 0;
          if (!done)
          {
            // Refused outright means this server runs without the leave the entry
            // needs, which is how it sits when a person started it from a shell. The
            // same command is asked of the desktop once. What decides the answer is
            // whether the entry stands afterwards, read off the machine rather than
            // out of a message that says it worked.
            said << "asking this machine for leave to do it; ";
            std::string why{};
            if (!raisedScript(createLine, why))
              said << (why.empty() ? "this machine would not ask" : why) << "; ";
            const Answer after = ask("schtasks /query /tn " + name + " /fo LIST");
            done = after.code == 0;
            said << (done ? "the entry stands now; " : "the entry does not stand; ");
          }
          else
          {
            said << "schtasks /create said: " << oneLine(made.text) << "; ";
          }
          if (done && !now.running)
          {
            const Answer started = ask("schtasks /run /tn " + name);
            said << "schtasks /run said: " << oneLine(started.text);
          }
          else if (done)
          {
            said << "this server is already answering, so it was not started twice";
          }
        }
      }
      else if (action == "startup_off")
      {
        if (byService)
        {
          const Answer set = ask("sc config " + name + " start= disabled");
          said << "sc config said: " << oneLine(set.text)
               << "; the service was left running, as it was asked to be";
          done = set.code == 0;
        }
        else
        {
          if (!now.installed)
          {
            said << "nothing on this machine is set to start this server, so there is "
                    "nothing to disable";
            done = true;
          }
          else
          {
            const Answer set = ask("schtasks /change /tn " + name + " /disable");
            said << "schtasks /change said: " << oneLine(set.text)
                 << "; a server already running was left alone";
            done = set.code == 0;
          }
        }
      }
      else if (action == "remove")
      {
        if (byService)
        {
          const Answer gone = ask("sc delete " + name);
          said << "sc delete said: " << oneLine(gone.text);
          done = gone.code == 0;
        }
        else if (now.installed)
        {
          const Answer gone = ask("schtasks /delete /tn " + name + " /f");
          said << "schtasks /delete said: " << oneLine(gone.text)
               << "; a server already running was left alone";
          done = gone.code == 0;
        }
        else
        {
          said << "this machine has nothing named " << now.name << " to take away";
          done = true;
        }
      }
      else if (action == "shutdown")
      {
        if (byService)
        {
          const Answer stopped = ask("sc stop " + name);
          said << "sc stop said: " << oneLine(stopped.text) << "; ";
          done = stopped.code == 0;
        }
        else if (now.installed)
        {
          const Answer ended = ask("schtasks /end /tn " + name);
          said << "schtasks /end said: " << oneLine(ended.text) << "; ";
          done = ended.code == 0;
        }
        else
        {
          said << "no manager started this server, so this process is the only thing to stop; ";
          done = true;
        }
        said << "this server is letting go of the port now";
        rememberWords(said.str());
        words = said.str();
        letGo();
        return true;
      }
      else
      {
        words = "this machine does not know what to do with \"" + action + "\"";
        rememberWords(words);
        return false;
      }
#else
      const std::string name = now.name;
      if (action == "startup_on")
      {
        std::string unitPath = "/etc/systemd/system/" + name + ".service";
        if (!now.installed || now.detail.empty() || now.detail == "no unit file named for it")
        {
          State fresh = now;
          fresh.target = selfExe();
          const std::string text = unitText(fresh);
          std::error_code ec{};
          std::filesystem::create_directories("/etc/systemd/system", ec);
          std::ofstream out{unitPath, std::ios::binary | std::ios::trunc};
          if (!out)
          {
            words = "writing " + unitPath +
                    " was refused: this server is not root on this machine, and a unit "
                    "file is root's to write. The unit it would have written starts " +
                    fresh.target + " from " + fresh.root;
            rememberWords(words);
            return false;
          }
          out << text;
          out.close();
          said << "wrote " << unitPath << " for " << fresh.target << " out of " << fresh.root
               << "; ";
          const Answer reload = ask("systemctl daemon-reload");
          said << "systemctl daemon-reload said: " << oneLine(reload.text) << "; ";
        }
        else
        {
          // The machine already keeps a unit for this name: it is enabled where it
          // stands, not rewritten, because what it starts is this very server.
          said << "keeping the unit this machine already has at " << now.detail << "; ";
        }
        const Answer enabled = ask("systemctl enable " + name);
        said << "systemctl enable said: " << oneLine(enabled.text) << "; ";
        done = enabled.code == 0;
        if (!now.running)
        {
          const Answer started = ask("systemctl start " + name);
          said << "systemctl start said: " << oneLine(started.text);
        }
        else
        {
          said << "this server is already running, so it was not started twice";
        }
      }
      else if (action == "startup_off")
      {
        if (!now.installed)
        {
          said << "nothing on this machine is set to start this server, so there is nothing "
                  "to disable";
          done = true;
        }
        else
        {
          const Answer disabled = ask("systemctl disable " + name);
          said << "systemctl disable said: " << oneLine(disabled.text)
               << "; a server already running was left alone, as it was asked to be";
          done = disabled.code == 0;
        }
      }
      else if (action == "remove")
      {
        if (!now.installed)
        {
          said << "this machine has no unit named " << name << " to take away";
          done = true;
        }
        else
        {
          const Answer disabled = ask("systemctl disable " + name);
          said << "systemctl disable said: " << oneLine(disabled.text) << "; ";
          const std::string unitPath = now.detail;
          if (!unitPath.empty() && unitPath != "/dev/null" &&
              unitPath.find("/etc/systemd/system/") == 0)
          {
            std::error_code ec{};
            const bool removed = std::filesystem::remove(unitPath, ec);
            said << (removed ? "took away " + unitPath : "could not take away " + unitPath)
                 << "; ";
          }
          else
          {
            said << "its unit lives at " << unitPath << ", which this server did not write and leaves where it is; ";
          }
          const Answer reload = ask("systemctl daemon-reload");
          said << "systemctl daemon-reload said: " << oneLine(reload.text);
          done = disabled.code == 0;
        }
      }
      else if (action == "shutdown")
      {
        if (now.installed && now.running)
        {
          const Answer stopped = ask("systemctl stop " + name);
          said << "systemctl stop said: " << oneLine(stopped.text) << "; ";
          done = stopped.code == 0;
        }
        else
        {
          said << "no manager is holding this server, so this process is the only thing to "
                  "stop; ";
          done = true;
        }
        said << "this server is letting go of the port now";
        rememberWords(said.str());
        words = said.str();
        letGo();
        return true;
      }
      else
      {
        words = "this machine does not know what to do with \"" + action + "\"";
        rememberWords(words);
        return false;
      }
#endif

      rememberWords(said.str());
      words = said.str();
      return done;
    }
  } // namespace deploy
} // namespace ols
