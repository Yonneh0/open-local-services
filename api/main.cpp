// open-local-services entry point: reads .env config, wires the service
// modules, and starts the server. Adding a service means constructing its
// module here and handing it to the OpenLocalServices.

#include "OpenLocalServices.h"
#include "Platform.h"
#include "ImageService.h"
#include "VoiceService.h"
#include "Deployment.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>
#else
#include <unistd.h>
#endif

static std::string trimText(const std::string &text)
{
  size_t from = 0;
  while (from < text.size() && std::isspace(static_cast<unsigned char>(text[from])))
    ++from;
  size_t to = text.size();
  while (to > from && std::isspace(static_cast<unsigned char>(text[to - 1])))
    --to;
  return text.substr(from, to - from);
}

static void readDotEnv(const std::string &file, std::map<std::string, std::string> &values)
{
  std::ifstream in{file};
  std::string line;
  while (std::getline(in, line))
  {
    const std::string clean = trimText(line);
    if (clean.empty() || clean[0] == '#')
      continue;
    const size_t mark = clean.find('=');
    if (mark == std::string::npos)
      continue;
    values[trimText(clean.substr(0, mark))] = trimText(clean.substr(mark + 1));
  }
}

static std::string configValue(const std::map<std::string, std::string> &fromFile,
                               const std::string &key, const std::string &fallback)
{
  const char *raw = std::getenv(key.c_str());
  if (raw && *raw)
    return raw;
  const auto found = fromFile.find(key);
  return found == fromFile.end() ? fallback : found->second;
}

// Ports above 65535 do not exist. Nothing on this machine counts its ports from
// this one: the doors answer on the single port named in the header, and the
// beacon a pair meets on is stated in full where it is asked for. So a port that
// is not a port is refused here, like any other unreadable value.
static uint16_t toPort(const std::string &text, uint16_t fallback)
{
  unsigned long long value = 0;
  for (const char c : text)
  {
    if (c < '0' || c > '9')
      return fallback;
    value = value * 10ull + static_cast<unsigned long long>(c - '0');
    if (value > 65535ull)
      return fallback;
  }
  return value == 0 ? fallback : static_cast<uint16_t>(value);
}

static int threadCount(const std::string &requested)
{
  const unsigned int hardware = std::thread::hardware_concurrency();
  int threads = hardware == 0 ? 4 : static_cast<int>(hardware);
  if (threads > 12)
    threads = 12;
  if (!requested.empty())
  {
    const int wanted = std::atoi(requested.c_str());
    if (wanted > 0)
      threads = wanted;
  }
  return threads;
}

static int rateLimit(const std::string &requested)
{
  if (requested.empty())
    return 600;
  const int wanted = std::atoi(requested.c_str());
  return wanted < 0 ? 0 : wanted;
}

// The doors answer the same either way; what changes is whether the machine will
// take a manager ask without a prompt. Setting this server up to start at boot,
// enabling or disabling its unit, ending its task: the machine asks those of the
// account its manager works for - SYSTEM, root - and a process started by a plain
// person is refused with "Access is denied" or a write to /etc that will not open.
// So the entry point raises itself once, before anything is read, and the person
// sitting at the machine answers that prompt. A machine that will not raise this
// one - no sudo that answers without a password, a prompt nobody answered, a
// person who said no - still runs these doors, and says which level it is
// standing at. OLS_ELEVATE=off is the one way a person declines the raise on a
// machine that would otherwise make one.
static bool askedForElevation()
{
  if (const char *named = std::getenv("OLS_ELEVATE"))
    return trimText(named) != "off";
  // The same fact can be set in the file the config panel writes. This entry
  // point runs before that file is read anywhere else, and the file is read
  // here for this one question only.
  std::map<std::string, std::string> fromFile;
  readDotEnv(".env", fromFile);
  const std::map<std::string, std::string>::const_iterator named =
      fromFile.find("OLS_ELEVATE");
  if (named == fromFile.end())
    return true;
  return trimText(named->second) != "off";
}

#ifdef _WIN32
// The machine's own way of saying whether this process carries the leave its
// manager needs: an account without it answers "Access is denied".
static bool alreadyElevated()
{
  return std::system("net session >nul 2>&1") == 0;
}

// The program this server is running as, read off the machine rather than
// guessed from a path somebody typed.
static std::string selfExe()
{
  char buffer[32768];
  const DWORD len = GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(sizeof(buffer)));
  if (len == 0 || len >= sizeof(buffer))
    return std::string();
  return std::string(buffer, len);
}

// Starting this very program again through the machine's elevation prompt. The
// window keeps the child, so the lines the server prints are the lines of this
// window, and the number this process hands back is the child's.
static void standRaised(int argc, char **argv)
{
  std::string params;
  for (int i = 1; i < argc; ++i)
  {
    if (i > 1)
      params += ' ';
    params += '"';
    params += argv[i];
    params += '"';
  }
  SHELLEXECUTEINFOA ask{};
  ask.cbSize = sizeof(ask);
  ask.fMask = SEE_MASK_NOCLOSEPROCESS;
  ask.lpVerb = "runas";
  const std::string program = selfExe();
  ask.lpFile = program.c_str();
  ask.lpParameters = params.empty() ? nullptr : params.c_str();
  ask.nShow = SW_HIDE;
  if (!ShellExecuteExA(&ask))
  {
    const DWORD last = GetLastError();
    std::cout << (last == ERROR_CANCELLED
                      ? "The person at this machine said no to raising it: this server runs as a plain person, and a manager ask will be refused with the machine's own words."
                      : "This machine would not raise it (win32 error " + std::to_string(last) + "): this server runs as a plain person, and a manager ask will be refused with the machine's own words.")
              << std::endl;
    return;
  }
  if (ask.hProcess != nullptr)
  {
    std::cout << "This machine raised this server: it runs at the level its manager "
                 "works at, and this process waits for it until it stops."
              << std::endl;
    WaitForSingleObject(ask.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(ask.hProcess, &code);
    CloseHandle(ask.hProcess);
    std::_Exit(static_cast<int>(code));
  }
}
#else
// On this kind of machine the level is root, and the tool that reaches it is
// sudo. Asked without a prompt (-n), sudo states whether it will answer for a
// password it would otherwise have to ask for: a server started over a pipe has
// no one to answer, and an attempt that silently waits for a password would
// look like a server that never started.
static bool alreadyElevated()
{
  return geteuid() == 0;
}

static bool answersWithoutPassword()
{
  return std::system("command -v sudo >/dev/null 2>&1 && sudo -n -v >/dev/null 2>&1") == 0;
}

static std::string selfExe()
{
  std::error_code ec{};
  const auto self = std::filesystem::canonical("/proc/self/exe", ec);
  return ec ? std::string() : self.string();
}

static void standRaised(int argc, char **argv)
{
  const std::string program = selfExe();
  if (program.empty())
    return;
  if (!answersWithoutPassword())
  {
    // Replacing this process with a sudo that then waits for a password would
    // leave no server at all - over a pipe there is nobody to answer. The doors
    // still run as this person's, and the machine states the level it stands at.
    std::cout << "This machine's sudo would ask for a password nobody can answer here: "
                 "this server runs as a plain person, and a manager ask will be refused "
                 "with the machine's own words."
              << std::endl;
    return;
  }
  std::vector<std::string> parts{"-E", "--", program};
  for (int i = 1; i < argc; ++i)
    parts.push_back(argv[i]);
  std::vector<char *> passed;
  passed.reserve(parts.size() + 1);
  for (const std::string &part : parts)
    passed.push_back(const_cast<char *>(part.c_str()));
  passed.push_back(nullptr);
  execvp("sudo", passed.data());
  // Only reached when there is no sudo to exec: the doors still run, as a plain
  // person's, and say so.
  std::cout << "This machine has no sudo for this server to stand on: it runs as a plain "
               "person, and a manager ask will be refused with the machine's own words."
            << std::endl;
}
#endif

static void ensureElevated(int argc, char **argv)
{
  if (!askedForElevation())
  {
    std::cout << "Not raising this server: OLS_ELEVATE=off. This machine's manager asks will be refused with its own words." << std::endl;
    return;
  }
  if (alreadyElevated())
    return;
  standRaised(argc, argv);
}

// How many of this machine's own pictures it keeps on disk. Nothing named leaves
// zero here, and zero is the module's own default standing; a number named is a
// room set for that many. The same number is the deepest a gallery page can be
// asked for, so a reader is never offered a page the machine cannot fill.
static int keptRenders(const std::string &requested)
{
  if (requested.empty())
    return 0;
  return std::atoi(requested.c_str());
}

int main(int argc, char **argv)
{
  // Raised before anything is read: the level this process stands at decides
  // whether the machine will take a manager ask, and nothing else about a start
  // depends on it.
  ensureElevated(argc, argv);
  // The words this server was started with are kept so a person can ask for the
  // soft hand: this server starting again with the program that stands on disk
  // now, as its own child.
  {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
      args.emplace_back(argv[i]);
    ols::deploy::rememberStart(std::move(args));
  }
  // A start at boot has no working directory of a person's to borrow, so the
  // machine's own entry for this server names the folder it was set up in. The
  // config file, the web pages, the data and the weights are all read from there,
  // which is the whole meaning of running it from wherever it stands.
  for (int i = 1; i + 1 < argc; ++i)
  {
    const std::string flag = argv[i];
    if (flag != "--home")
      continue;
    std::error_code homeEc{};
    std::filesystem::current_path(argv[i + 1], homeEc);
    if (homeEc)
      std::cout << "Cannot start from " << argv[i + 1] << ": " << homeEc.message()
                << "; starting from where I am\n";
    else
      std::cout << "Starting from " << argv[i + 1]
                << ", the folder this machine was set up in\n";
  }

  std::map<std::string, std::string> fromFile;
  readDotEnv(".env", fromFile);
  const std::string host = configValue(fromFile, "OLS_HOST", "0.0.0.0");
  // The port is not a setting. These doors stand on one port on every box, and
  // nothing on the machine answers on another. A .env naming another port is said
  // out loud and then left alone.
  const uint16_t port = ols::kApiPort;
  const std::string askedPort = configValue(fromFile, "OLS_PORT", "");
  const std::string weightsDir = configValue(fromFile, "OLS_WEIGHTS_DIR", "weights");
  const std::string dataDir = configValue(fromFile, "OLS_DATA_DIR", "data");
  const std::string hfToken = configValue(fromFile, "HF_TOKEN", "");
  const std::string civitaiKey = configValue(fromFile, "CIVITAI_API_KEY", "");
  const std::string apiKey = configValue(fromFile, "OLS_API_KEY", "");
  const int rate = rateLimit(configValue(fromFile, "OLS_RATE_LIMIT", ""));
  const int threads = threadCount(configValue(fromFile, "OLS_THREADS", ""));
  // The kept-picture room is sized here rather than left to the module's guess,
  // because what a machine keeps is its own owner's choice; nothing named leaves
  // the module's own default standing.
  const int keptMax = keptRenders(configValue(fromFile, "OLS_GALLERY_MAX", ""));

  ols::imagegen::ImageService images(weightsDir, dataDir + "/outputs", threads, hfToken, civitaiKey,
                                     keptMax);

  // The voice engine has no process and no port: it is linked into this binary,
  // and its models answer through handles. It is given the weights room, the
  // voice room on disk, and the token, so a package a card asks for can have its
  // own files fetched into place.
  ols::voice::VoiceService voices(weightsDir + "/voice", dataDir + "/voice", threads, hfToken);

  ols::OpenLocalServices api;
  api.setBindHost(host);
  api.setStaticDir("webui");
  api.setImage(&images);
  api.setVoice(&voices);

  api.setApiKey(apiKey);
  api.setRateLimit(rate);
  // the ggml the voice models run on is asked for the machine's devices before
  // anything else does; it is linked in, so there is nothing to load here - the
  // ask only takes the one-time trouble of finding the card out of the first
  // page read.
  api.preloadBackends();
  // nothing is read here: no model is opened, no table is built, no engine is
  // stood up. the port is taken next, and the first page read asks the machine.
  if (!askedPort.empty() && toPort(askedPort, 0) != port)
    std::cout << "Ignoring OLS_PORT=" << askedPort << ": these doors answer on port " << port
              << " and no other" << std::endl;
  if (!api.start(port))
  {
    const std::string why = api.startRefusal();
    std::cout << (why.empty() ? "Could not bind a port" : why) << std::endl;
    return 1;
  }
  // The port answers from this moment on, in the clear.
  std::cout << "API server on http://" << api.bindHost() << ":" << api.port() << std::endl;
  // The version of the code this binary stands on: a development build says
  // dev plus the moment it was compiled; a released build names its commit.
  std::cout << "code version: " << ols::platform::versionLine() << std::endl;
  if (!apiKey.empty())
    std::cout << "API asks for a key (X-Api-Key header, Authorization: Bearer, or ?key= in the address)"
              << std::endl;
  else
    std::cout << "API doors ask for nothing: this machine sets no key" << std::endl;
  // The rooms ask of the same credential these doors do; a page that arrives
  // without one is walked to the door, so no letter of it is printed here.
  if (rate > 0)
    std::cout << "Rate limit: " << rate << " requests per minute" << std::endl;
  while (api.isRunning())
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  api.stop();
  // The soft hand's second half: the listening has closed, so the program that
  // stands on disk now can be started as this process's own child - it carries
  // the level this process stood at, and this process waits for it as long as it
  // answers.
  if (ols::deploy::softReloadRequested())
    ols::deploy::handOverNow();
  return 0;
}
