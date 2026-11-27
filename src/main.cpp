// open-local-services entry point: reads .env config, wires the service
// modules, and starts the server. Adding a service means constructing its
// module here and handing it to the OpenLocalServices.

#include "OpenLocalServices.h"
#include "Platform.h"
#include "ImageService.h"
#include "Deployment.h"
#include "NetworkService.h"
#include "ProjectService.h"

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

// How many of this machine's threads a run may use. The launcher's own rule is
// 1..4096, and the config door refuses anything else; this is the same rule for
// the one way a value still reaches here - a file edited by hand. A figure read
// off a line of digits that went on being something else ("12 of them, please")
// is not taken for the number it starts with.
static const int kThreadCap = 4096;

static int threadCount(const std::string &requested)
{
  const unsigned int hardware = std::thread::hardware_concurrency();
  int threads = hardware == 0 ? 4 : static_cast<int>(hardware);
  if (threads > 12)
    threads = 12;
  if (requested.empty())
    return threads;
  long long wanted = 0;
  for (const char c : requested)
  {
    if (c < '0' || c > '9')
      return threads;
    wanted = wanted * 10 + (c - '0');
    if (wanted > kThreadCap)
      return threads;
  }
  if (wanted < 1)
    return threads;
  return static_cast<int>(wanted);
}

static const int kRateLimitDefault = 600;
static const int kRateLimitMax = 1000000;

static int rateLimit(const std::string &requested)
{
  if (requested.empty())
    return kRateLimitDefault;
  unsigned long long value = 0;
  for (const char c : requested)
  {
    if (c < '0' || c > '9')
      return -1;
    value = value * 10ull + static_cast<unsigned long long>(c - '0');
    if (value > static_cast<unsigned long long>(kRateLimitMax))
      return -1;
  }
  return static_cast<int>(value);
}

// The doors answer the same either way; what changes is whether the machine will
// take a manager ask without a prompt. Setting this server up to start at boot,
// enabling or disabling its unit, ending its task: the machine asks those of the
// account its manager works for - SYSTEM, root - and a process started by a plain
// person is refused with "Access is denied" or a write to /etc that will not open.
// So the entry point raises itself once, before anything is read, and the person
// sitting at the machine answers that prompt. And a server that did not get
// raised does not stand at all: 2026-11-20 a whole evening was lost to plain
// servers that started gladly, served every page, and died at the first manager
// ask - a half-armed machine pretending to be armed. Now: raised, or refused
// loudly with a failing exit. There is no way to decline the law: OLS_ELEVATE=off
// declines the prompt, which lands on the refusal below, not on serving anyway.
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
// The law of the level lives in the deployment module (Deployment.h): both the
// entry point and the doors read the same reading of the token there.
static bool alreadyElevated()
{
  return ols::deploy::carriesManagerLevel();
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
  const std::string program = ols::deploy::selfProgram();
  ask.lpFile = program.c_str();
  ask.lpParameters = params.empty() ? nullptr : params.c_str();
  ask.nShow = SW_HIDE;
  if (!ShellExecuteExA(&ask))
  {
    const DWORD last = GetLastError();
    std::cout << (last == ERROR_CANCELLED
                      ? "The person at this machine said no to raising it: this server refuses to start."
                      : "This machine would not raise it (win32 error " + std::to_string(last) + "): this server refuses to start.")
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
  return ols::deploy::carriesManagerLevel();
}

static bool answersWithoutPassword()
{
  // Probed with a real command, not with `sudo -v`: on a host whose sudoers
  // carries both a rule that asks and a rule that does not, -v validates the
  // timestamp against the asking rule and refuses even where sudo -n would
  // run the command without a word. `true` asks the question the same way the
  // elevation below will be asked.
  return std::system("command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1") == 0;
}

static void standRaised(int argc, char **argv)
{
  const std::string program = ols::deploy::selfProgram();
  if (program.empty())
    return;
  if (!answersWithoutPassword())
  {
    // Replacing this process with a sudo that then waits for a password would
    // leave no server at all - over a pipe there is nobody to answer. So this
    // one declines to stand on that sudo, and the law below refuses the start.
    std::cout << "This machine's sudo would ask for a password nobody can answer here: "
                 "this server refuses to start."
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
  // Only reached when there is no sudo to exec: the refusal follows below.
  std::cout << "This machine has no sudo for this server to stand on: it refuses to start."
            << std::endl;
}
#endif

static void ensureElevated(int argc, char **argv)
{
  if (askedForElevation() && !alreadyElevated())
    standRaised(argc, argv);
  if (alreadyElevated())
  {
    // The scripts promise that the line this server prints names the level it
    // stands at; here is that line, read off the token or the uid, never
    // assumed from the account a person typed.
    std::cout << "this server stands at the level its manager works at: "
              << ols::deploy::managerLevelWords() << std::endl;
    return;
  }
  // One law on every machine, checked once and after every road the raise could
  // have taken: a server that does not stand at the manager's level does not
  // stand. Declining the prompt with OLS_ELEVATE=off is a refusal to be raised,
  // not a licence to serve half-armed. The words say what the token held, so a
  // refusal names the account and level it actually read.
  std::cerr << "open-local-services refuses to start: this process does not carry the "
               "level its manager works at (read: "
            << ols::deploy::managerLevelWords() << "). Start it through the machine's prompt "
            << "(run.bat answers it once), a task that raises it, or an account that "
               "already holds it"
            << std::endl;
  std::exit(EXIT_FAILURE);
}

// How many of this machine's own pictures it keeps on disk. Nothing named leaves
// zero here, and zero is the module's own default standing; a number named is a
// room set for that many. The same number is the deepest a gallery page can be
// asked for, so a reader is never offered a page the machine cannot fill - which
// is why a figure that is not a count of pictures is refused rather than taken:
// a negative one would leave a room that holds nothing at all.
static const int kKeptRendersCap = 1000000;

static int keptRenders(const std::string &requested, std::string &refusal)
{
  refusal.clear();
  if (requested.empty())
    return 0;
  long long wanted = 0;
  for (const char c : requested)
  {
    if (c < '0' || c > '9')
    {
      refusal = "a count of pictures, or nothing at all";
      return 0;
    }
    wanted = wanted * 10 + (c - '0');
    if (wanted > kKeptRendersCap)
    {
      refusal = "at most " + std::to_string(kKeptRendersCap) + " pictures";
      return 0;
    }
  }
  return static_cast<int>(wanted);
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
  // The configured port. It defaults to 8765, and the server refuses to start if
  // that port is already in use. It never rolls over to another port.
  const std::string requestedPort = configValue(fromFile, "OLS_PORT", "");
  uint16_t port = ols::kApiPort;
  if (!requestedPort.empty())
  {
    port = toPort(requestedPort, 0);
    if (port == 0)
    {
      std::cout << "Invalid OLS_PORT=" << requestedPort
                << ": the port must be a number from 1 to 65535." << std::endl;
      return 1;
    }
  }
  const std::string weightsDir = configValue(fromFile, "OLS_WEIGHTS_DIR", "weights");
  const std::string dataDir = configValue(fromFile, "OLS_DATA_DIR", "data");
  const std::string hfToken = configValue(fromFile, "HF_TOKEN", "");
  const std::string civitaiKey = configValue(fromFile, "CIVITAI_API_KEY", "");
  const std::string apiKey = configValue(fromFile, "OLS_API_KEY", "");
  const std::string requestedRate = configValue(fromFile, "OLS_RATE_LIMIT", "");
  const int rate = rateLimit(requestedRate);
  if (rate < 0)
  {
    std::cout << "Invalid OLS_RATE_LIMIT=" << requestedRate
              << ": use 0 to disable the rate limit, use a whole number from 1 to " << kRateLimitMax
              << ", or leave it unset to use the default " << kRateLimitDefault
              << " requests per minute." << std::endl;
    return 1;
  }
  const int threads = threadCount(configValue(fromFile, "OLS_THREADS", ""));
  // Which face these doors answer with (spec §10.3): three words, or none
  // where the clear face they have always answered stands. Anything else is
  // said out loud and the server does not start - a machine that quietly
  // ignored the word would serve a different machine than the one its owner
  // asked for, and the address in every page and peer record would answer
  // differently than named.
  const std::string tlsWord = configValue(fromFile, "OLS_TLS", "");
  if (!tlsWord.empty() && tlsWord != "off" && tlsWord != "on" && tlsWord != "auto")
  {
    std::cout << "Invalid OLS_TLS=" << tlsWord
              << ": use off, on or auto, or leave it unset to keep answering in the clear."
              << std::endl;
    return 1;
  }
  // A hand stood by a running server: it was started only to ask the soft hand
  // of the door that server stands at, once a newer build has landed. It binds
  // nothing of its own - the number named on this flag is the port to ask, not
  // a port to hold.
  bool handStand = false;
  uint16_t handPort = 0;
  for (int i = 1; i + 1 < argc; ++i)
  {
    if (std::string(argv[i]) != "--hand-ask")
      continue;
    handStand = true;
    handPort = toPort(argv[i + 1], 0);
    if (handPort == 0)
    {
      std::cout << "Invalid --hand-ask value: it must be a number from 1 to 65535." << std::endl;
      return 1;
    }
  }
  // The kept-picture room is sized here rather than left to the module's guess,
  // because what a machine keeps is its own owner's choice; nothing named leaves
  // the module's own default standing. A figure that cannot be a count of
  // pictures is said out loud and left at that default: a server that will not
  // start over the size of one room is harder to notice than a room that is.
  std::string keptRefusal;
  const int keptMax = keptRenders(configValue(fromFile, "OLS_GALLERY_MAX", ""), keptRefusal);
  if (!keptRefusal.empty())
    std::cout << "Ignoring OLS_GALLERY_MAX=" << configValue(fromFile, "OLS_GALLERY_MAX", "")
              << ": it holds " << keptRefusal << "." << std::endl;

  // The services these doors answer from, and the doors themselves, are left
  // standing for the life of the process: nothing takes them apart when this
  // function ends. A connection that outlives the shutdown grace
  // (OpenLocalServices::stop) keeps answering from exactly these objects, and a
  // destructor run here would be memory taken away from under its feet. The
  // machine takes the memory back when the process ends; a shutdown is not
  // worth racing a live thread over.
  ols::imagegen::ImageService *images =
      new ols::imagegen::ImageService(weightsDir, dataDir + "/outputs", threads, hfToken,
                                      civitaiKey, keptMax);

  // The project studio's ledger: the registry of projects under the data folder
  // and the facts each one states. It owns no engine and opens no port; the
  // doors answer from it, and GitCore joins its folders in a later phase.
  ols::project::ProjectService *projects = new ols::project::ProjectService(dataDir);

  ols::OpenLocalServices *api = new ols::OpenLocalServices();
  api->setBindHost(host);
  api->setStaticDir("webui");
  api->setImage(images);
  api->setProject(projects);

  api->setApiKey(apiKey);
  api->setRateLimit(rate);
  // The certificates, if this face asks for any, stand in the data folder's
  // tls room: auto makes them on first start, on refuses to start without
  // them, and the clear face never looks at the folder at all.
  api->setTls(tlsWord.empty() ? "off" : tlsWord, dataDir + "/tls");
  // the engine library beside this server - cuda, vulkan, or cpu, as the
  // machine answers - is opened here, before the port is taken: the card is
  // read once, the one library of the kind found is loaded, and the words it
  // answers with are printed as this server starts.
  api->preloadBackends();
  // The network sampler starts before the doors: by the time the first page is
  // read, the bandwidth history behind it already has a few seconds of real
  // readings, and the uptime trackers have seen the links once.
  ols::net::init();
  // nothing is read here: no model is opened, no table is built, no engine is
  // stood up. the port is taken next, and the first page read asks the machine.
  if (handStand)
  {
    // The hand binds nothing and serves no page: it asks one thing - the soft
    // hand - of the server still standing at the machine's door, over the
    // loopback that door answers on. The ask arrives behind the waiting
    // server's own answer, and the replacement is then made by that server,
    // which takes its hand when its listening closes. Asking a door of its own
    // instead, as this hand once did, sets a flag no loop is left to read.
    bool handed = false;
    {
      if (ols::platform::initSockets())
      {
        const ols::platform::sock_t askSock = ols::platform::newStreamSocket();
        if (askSock != ols::platform::kBadSocket)
        {
          // The hand asks the standing door over loopback, through the socket
          // API the server itself stands on.
          const sockaddr_in where = ols::platform::endpointFor(handPort, true);
          if (ols::platform::connectSocket(askSock, where) != ols::platform::kSocketError)
          {
            const std::string body = "{\"action\":\"restart\"}";
            std::string message = "POST /api/service HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
                                  std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            if (!apiKey.empty())
              message.insert(message.find("\r\n\r\n"), "X-Api-Key: " + apiKey + "\r\n");
            size_t sent = 0;
            while (sent < message.size())
            {
              const int went = ols::platform::sendBytes(askSock, message.data() + sent,
                                                        static_cast<int>(message.size() - sent), 0);
              if (went <= 0)
                break;
              sent += static_cast<size_t>(went);
            }
            char readOne[256] = {0};
            const int got = ols::platform::receiveBytes(askSock, readOne, static_cast<int>(sizeof(readOne)) - 1, 0);
            // The answer's own words say whether the hand was taken: a 200 that
            // says did=false is not a hand.
            handed = got > 0 && std::strstr(readOne, "200") != nullptr &&
                     std::strstr(readOne, "\"did\":true") != nullptr;
          }
          ols::platform::closeSocket(askSock);
        }
      }
    }
    std::cout << (handed ? "the hand has asked: the server standing at the door takes the machine from there."
                         : "the hand could not ask the door standing at port " +
                               std::to_string(handPort) + ": nothing was restarted.")
              << std::endl;
    return handed ? 0 : 1;
  }
  if (!api->start(port))
  {
    const std::string why = api->startRefusal();
    std::cout << (why.empty() ? "Could not bind a port" : why) << std::endl;
    return 1;
  }
  // The port answers from this moment on, in the clear.
  std::cout << "API server on http://" << api->bindHost() << ":" << api->port() << std::endl;
  // The version of the code this binary stands on: a development build says
  // dev plus the moment it was compiled; a released build names its commit.
  std::cout << "code version: " << ols::platform::versionLine() << std::endl;
  if (!apiKey.empty())
    std::cout << "API asks for a key (X-Api-Key header, Authorization: Bearer, or ?key= in the address)"
              << std::endl;
  else
    std::cout << "API doors ask for nothing: this machine sets no key" << std::endl;
  // The rooms ask of the same credential these doors do; a page that reaches
  // its doors without one asks the browser there, so no letter of it is printed
  // here.
  if (rate > 0)
    std::cout << "Rate limit: " << rate << " requests per minute (token bucket; refused requests return HTTP 429 with Retry-After)"
              << std::endl;
  else
    std::cout << "Rate limit: none" << std::endl;
  // Fleet awareness: the tables of acquaintances are read, and a walk of the
  // machine's own networks starts in the background - once, at this start. Every
  // server met at its door joins this table, and joins this machine to its own.
  api->startPeers(apiKey, dataDir);
  while (api->isRunning())
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  api->stop();
  // The soft hand's second half: the listening has closed, so the program that
  // stands on disk now can be started as this process's own child - it carries
  // the level this process stood at, and this process waits for it as long as it
  // answers.
  if (ols::deploy::softReloadRequested())
    ols::deploy::handOverNow();
  return 0;
}
