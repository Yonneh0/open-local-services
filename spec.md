# spec.md — the core of open-local-services

The contract of the server itself: one process, one port, one web UI, and the
doors every studio answers through. This file speaks for the transport, the
credential, the rate gate, the config, the job law, the cache law, the pages,
the file doors, and the machine-facing services (system, monitor, network,
peers, deployment). It says nothing about what the studios do behind their
doors:

- `spec_image_studio.md` — the image studio: models, jobs, gallery, references, `/api/*` and `/files/*`.
- `spec_project_studio.md` — Project Studio: the git ledger, the code doors, `/project/api/*` and `/git/*`. That file is the contract for its studio, including the parts not yet built; where it names a core rule (the key gate, the rate gate, the body caps), this file is the core's own statement of it.

Where code and this file disagree, one of them is wrong.

## 0. The shape of the thing

One C++ binary. A hand-written HTTP/1.1 server over the socket layer in
`src/Platform.h` — no HTTP library, no framework, no build step for the pages.
The service modules (`ols::imagegen`, `ols::project`) own their
state and answer through thin doors in `src/OpenLocalServices.cpp`.

The laws the whole server stands on:

1. **One port.** The doors answer on the port named at startup (default 8765,
   `OLS_PORT`). A server that finds it held says so and does not run; it never
   rolls over to another port, because every address another box or a page has
   for this machine names this port.
2. **Raised, or refused.** The server stands at the level its machine's manager
   works at (Windows admin, Linux root) or it does not start at all — see §4.
3. **One credential.** One api key (`OLS_API_KEY`) opens every api door. With no
   key set, the doors ask nothing. The pages are shells and stand open; every
   fact behind them answers only to the key.
4. **Plain answers, in words.** A refusal states the rule the ask broke. Nothing
   is guessed: a machine that states nothing about a fact leaves that field out
   rather than inventing one.
5. **The pages are the machine's.** The binary carries its pages inside it and
   can stand alone; a folder of pages beside it answers instead, live, and the
   expand door writes the copies out for a person to read.

### 0.1 Related files

| file | what it owns |
| --- | --- |
| `src/main.cpp` | the entry point: elevation law, `.env` reading, wiring the modules, the soft hand's second half |
| `src/OpenLocalServices.h` | the server's public API: `HttpRequest`/`HttpResponse`, `ConnectionSink`, the gates, the constants |
| `src/OpenLocalServices.cpp` | the transport, the gates, the route table, the static/file servers, the system snapshot, the Project Studio door block |
| `src/Platform.h` | sockets (Winsock/BSD), `curlFetch`, `quotableInShell`, host name, build version/stamp |
| `src/Common.h` | shared helpers: json reads, base64, files, `makeId`, job states, `kKeptJobs`, `deviceCard()`, SHA-256, weights paths |
| `src/HardwareInfo.h/.cpp` | SMBIOS memory sticks, GPU clocks |
| `src/WebAssets.h` + build-generated `webui-embedded.cpp` | the pages compiled into the binary |
| `src/MonitorService.h/.cpp` | the composed monitor document |
| `src/NetworkService.h/.cpp` | the network document and its work door |
| `src/PeerService.h/.cpp` | the fleet table, the sweep, the hello |
| `src/Deployment.h/.cpp` | the boot entry, the manager's words, the soft hand |
| `webui/` | the pages: shell (`index.html`, `common.js`, `key-shim.js`, `status-cache.js`, `monitor.js`, `config.js`, `network.js`, `style.css`) plus each studio's own files |
| `run.bat` / `run.sh`, `build-rel.bat` / `build-rel.sh` | the dev start (answers the level ask once) and the release build |
| `open-local-services.service` | the Linux unit the deployment door writes/reads |

### 0.2 The route table at a glance

| address | read | worked | spec |
| --- | --- | --- | --- |
| `/` | auth info (or the front room for a browser) | hand a credential | §3 |
| `/<page>.html`, `/webui/*`, `/favicon.ico`, `*.css`, `*.js` | the pages, before any key | — | §8 |
| `/api/system`, `/api/monitor`, `/api/network`, `/api/network/scan`, `/api/peers`, `/api/peers/hello`, `/api/service`, `/api/config`, `/api/webui/expand` | the machine's own readings | config, service, expand, network actions | this file |
| `/api/models*`, `/api/jobs*`, `/api/generate`, `/api/gallery*`, `/api/refs*`, `/api/prefs`, `/api/timings`, `/api/unload`, `/files/*` | the image studio | its work | `spec_image_studio.md` |
| `/project/api/*` | Project Studio | its work | `spec_project_studio.md` |
| `/git/<slug>/*` | the git smart-HTTP doors | clone/fetch/push | `spec_project_studio.md` §8 |

An address nothing answers gets `404 {"error":"No route"}`. A 502 is kept for a
module genuinely not wired into the binary ("No image service", "No project
service").

## 1. The transport

### 1.1 Listening

- Bind host from `OLS_HOST` (default `0.0.0.0`; `localhost`/`127.0.0.1`/`::1`
  bind loopback, any other name must parse as an IPv4 address or the start
  fails).
- One port, the one asked for. Before binding, the server probes the port with a
  connect; held → refusal, kept whole for the person who ran the binary:
  *"Port N is held by another server. These doors answer on that port only, so
  this server will not run beside it."*
- The listening socket is marked non-inheritable (`HANDLE_FLAG_INHERIT` off /
  `FD_CLOEXEC`): an engine child must never carry the port open after the server
  that started it is gone — measured on X1 2026-10-28, a stray `llama-server.exe`
  held the port and the next start refused.
- Backlog `SOMAXCONN`; `SO_REUSEADDR` on the probe socket.
- The port answers in the clear. TLS is an open item of Project Studio
  (`spec_project_studio.md` §4, E1), not a core promise.

### 1.2 One connection's life

Every connection runs on its own thread and is owned by a `ConnectionSink` from
its first line: the sink's destructor closes the socket, and that is its one
act, so every way out — an answer, a refusal, a throw — closes it exactly once.

- **Deadlines.** Per-read timeout 10 s; the connection's own deadline 120 s
  (`kConnectionOverallMs`) from its first byte, watched by every read including
  the drain of a refused body. A read that ends on the clock answers
  `408 {"error":"Request timed out"}`; the read already running when the
  deadline passes may still run out its ten seconds.
- **First read.** 4096 bytes, kept whole — nothing that arrived is thrown away.
- **Headers.** Read until `\r\n\r\n`, capped at 1 MiB. No terminator and no
  bytes coming → 408; over the cap or no parse → `400 {"error":"Malformed request"}`.
- **The request line** needs a method, a target, and a version token starting
  `HTTP/`; anything else is refused before routing:
  *"the request line carries no method, no target, or no HTTP version"*.
- **The path** is normalised: repeated `/` collapse to one, a trailing `/` is
  stripped (an empty path stays `/`). The query is everything after the first
  `?`, kept raw; readers decode percent escapes, and a query reader also reads
  `+` as space (a form field writes it; a path never does).
- **Headers** parse to lowercase keys with leading spaces stripped. A line with
  no colon is skipped.
- **`Content-Length`** must be a whole number of bytes
  (*"the length this ask states is not a whole number of bytes"*); two
  statements of one number are one statement, two different numbers are a
  refusal (*"the ask states two different lengths for one body"*). No length at
  all is legal — the way a GET and a bodiless POST always have been.
- **`HEAD` is a GET** asked without its body: the same doors read it, and the
  body is taken off the answer at the one place that writes an answer out.
- **Every answer ends the connection** (`Connection: close`). Keep-alive is not
  spoken.
- **Status lines** come from one table (200/201/202/206/304/307/400/401/404/405/
  408/409/413/416/429/500/501/502/503); a code the table does not name states
  its own number (`"<N> Error"`) rather than being rewritten to 500.

### 1.3 Bodies

- **Cap:** 2 MiB (`kMaxBodyBytes`) for every door except the git doors
  (`/git/…`), which answer to `OLS_GIT_MAX_BODY_MIB` (default 512, valid
  1..16384).
- **Over the cap, counted:** the body is refused but still drained to its
  declared end — so the answer is not mistaken for a dropped connection — then
  `413 {"error":"Request body too large"}`. Nothing is buffered while draining.
- **Git doors stream to a temp file.** A git body never stands in memory: it is
  written to `ols-git-body-<32 hex>.bin` in the machine's temp folder (name
  drawn from a seeded `mt19937_64`, eight tries, then one honest last try),
  created exclusive, and removed by an RAII hand when the connection's scope
  ends — the writer, a refusal, or a throw, every road. The route sees the body
  through the internal headers `x-ols-body-file` and `x-ols-body-size`, which
  the transport publishes before routing and which no client can set (they are
  set after parsing, over any that arrived).
- **Chunked request bodies are spoken only on git doors** — git frames its push
  packs and fetch answers that way. Any other door handed a
  `Transfer-Encoding` is refused: *"this door does not take chunked bodies"*.
  The decoder reads hex size lines, sets chunk extensions and trailers aside,
  checks the cap per chunk (over → 413, stated where it stands — a chunked
  stream has no declared end to drain), and a stream that stops framing
  mid-flight answers `400 {"error":"The chunked body did not frame as chunked"}`.
  A counted git body that ends before its length answers
  *"The request body ended before its length"*.
- **`Expect: 100-continue`** (matched on word boundaries) earns
  `HTTP/1.1 100 Continue\r\n\r\n` before any body read — git temp streaming
  included. An `Expect` naming any other token is ignored, not answered 417:
  RFC 7231 makes that answer optional, and a client with an expectation this
  server does not know does not want its request turned away for it. A framing
  refusal answers before the interim word — the bytes still in the client's
  hand are not what stands in the way.
- **A body read as json that is not json** answers
  `400 {"error":"Invalid request body"}` — the throw is caught at the transport
  and named on the server's own line. Any other throw from a door answers
  `500 {"error":"Internal error"}` and logs the path and the words.

### 1.4 Streaming answers

Three roads carry an answer bigger than one string:

- **`gitWriter`** — a git smart-HTTP door takes the connection itself and speaks
  the protocol whole (§1 of the Project Studio spec); nothing else of that
  answer is written.
- **The event stream** — `/api/jobs/<id>/events` (§6).
- **`chunkSource`** — the generic chunked road (each call produces one frame;
  an empty one ends it). No door hands one over today; the road stands unused,
  not dead.

`OpenLocalServices::writeStreamChunk(client, frame)` is the one framer: hex
size line, the frame, CRLF.

### 1.5 Connection limits and shutdown

- At most **256 live connections** (`kMaxLiveConnections`). The place is taken
  from an exact counter *before* the machine is asked for a thread, so the cap
  is a cap and not a wish; over it, the client is closed at once with nothing
  said — the honest answer of a server spending all the hands it means to
  spend. A thread-creation failure returns the place, closes the client, logs,
  and the loop stands: a busy machine, not a dead one.
- `stop()` stops the peer sweep, gives up the listening socket (so the port is
  free when it returns), joins the accept loop, then joins every registered
  connection thread within a **5 s grace** (`kStopGraceMs`). A connection still
  answering past the grace is detached on purpose — it answers from the service
  modules, and the entry point leaves those standing to the process's end, so
  there is nothing under its feet to fall through. The server's line:
  *"a connection outlived the shutdown grace: left standing on purpose"*.
- Asking to stop twice is the same as asking once.
- **The soft hand** (`/api/service` restart, §12): the door answers first, the
  loop gives up its listening where the process ends, and the program on disk
  is started as this process's own child — which carries the level this process
  stood at, so the machine is never asked again mid-answer.

## 2. The rate gate

A token bucket in thousandths so refill can be fractional:

- `OLS_RATE_LIMIT` requests per minute, default **600**, `0` disables,
  at most 1 000 000. The config door can move it while the server runs.
- Each request consumes one token **only when it is allowed**; a blocked
  request has no side effect at all.
- Refill is timed; a bucket idle past 60 s comes back full.
- A refused request answers `429 {"error":"Rate limit exceeded"}` with
  `Retry-After` (whole seconds until one token could exist, at least 1),
  `X-RateLimit-Limit`, `X-RateLimit-Remaining: 0`, and `X-RateLimit-Reset`
  (unix seconds). The refusal carries none of the caller's input.
- The gate stands after the key gate and before every door — including the
  event stream and the git doors.

## 3. The credential

One key, one answer. With no key set every door is open; with a key set, every
api door (and every git door) answers to that key alone.

### 3.1 How a credential may be handed

In this order, all read on every ask (`givenCredentials`):

1. `X-Api-Key: <key>`
2. `Authorization: Bearer <key>`
3. `?key=<key>` in the address — the shape for programs, and for a browser
   walking over from a machine next door; a page that wakes with a key in its
   address takes it up at once and wipes the address clean
4. the cookie `ols-key` — written by the certify answer (§3.4), percent-escaped
   on the way out and read back whole

Comparison is constant-time (`keysMatch`). Nothing ever reads a letter of a key
into a response, a log, or the peer table.

### 3.2 The gate

`authGate` answers three ways: **0** allow, **1** send the refusal, **-1** send
nothing.

- An accepted credential resets the miss count — a machine being used is never
  left silent — and, when a key is set *and* a credential was handed, the
  answer carries a random 6–36 ms delay, so a correct credential cannot be
  recognised by response timing alone. With no key, or with nothing handed to
  compare, a clock has nothing to give away and the request answers straight
  away.
- The first **3** misses (`kAuthSilentAfter`) get honest 401s. Past that, a miss
  arriving within **30 s** (`kAuthSilentResetMs`) of the last one gets **no
  response at all** — the connection is swallowed. A miss arriving past the
  quiet stretch gets its honest answer whatever the count says, which reopens
  the gate for one honest answer.
- The miss counter is shared by every door, so a storm of refused rooms silences
  the api doors too.

### 3.3 The refusal

`401` with body:

```json
{"error":"This machine asks for a credential at its api doors - its own key. Hand it over as described below.",
 "ways":"X-Api-Key header, Authorization: Bearer, or ?key= in the address"}
```

plus `X-OLS-Auth: required` and `WWW-Authenticate: Key`.

### 3.4 The bare address `/`

The one address that stands open to nothing it asks of:

- `GET /` from a program (no `Accept`, or `*/*`) → `200 {"api_required":bool,"peers":true}`
  — counts only, never a letter of anything. `peers` is always true: pages of
  any age read this word before asking `/api/peers`, so a room never rattles a
  door a younger machine does not have.
- `GET /` from a browser (`Accept` contains `text/html`) → the front room
  (`index.html`), which asks the browser for a key itself when this machine asks
  of one. No address of this machine is a place a key has to be written.
- `POST /` with the key as its whole body (surrounding whitespace trimmed) →
  the certify answer:

```json
{"handed":bool,"api_required":bool,"opens_doors":bool,
 "ways":"X-Api-Key header, Authorization: Bearer, or ?key= in the address"}
```

  always `200`, and when the key is taken, `Set-Cookie: ols-key=<escaped>; Path=/; SameSite=Lax`
  so the rest of the visit carries it. The words never repeat what was handed.
- Any other method → `405 {"error":"This address is read with a GET and handed a credential with a POST"}`.

### 3.5 What stands open before the key

Pages only: `/webui` and everything under it, `/favicon.ico`, and any path
ending `.html`/`.css`/`.js` (read GET/HEAD). They are shells; every fact behind
them answers only to the key. A page that reaches its doors with no credential
asks for one where it stands (§3.4) and hands it over the same doors any other
ask uses.

## 4. The elevation law

The doors answer the same either way; what the level decides is whether the
machine will take a manager ask (boot entry, unit, task). A plain process is
refused by its own machine, so:

- Before anything is read, the entry point checks the level (Windows: `net
  session`; Linux: `geteuid()==0`). Not raised → it asks the machine to raise
  it once (Windows: the elevation prompt through `runas`, the window keeps the
  child and the parent waits for it; Linux: `sudo -E --`, only when sudo answers
  without a password nobody can't answer — over a pipe, waiting for one would
  look like a server that never started).
- **Raised, or refused loudly with a failing exit.** A server that did not get
  raised does not stand at all: 2026-11-20 an evening was lost to plain servers
  that started gladly, served every page, and died at the first manager ask.
- `OLS_ELEVATE=off` (env or `.env`) declines the ask — and still refuses the
  start. Declining is a refusal to be raised, not a licence to serve
  half-armed.
- `OLS_SCRATCH=1` exempts a scratch instance from the law entirely: a server
  answering another port than the manager's own is nobody's boot entry. Naming
  another port alone does not exempt — a boot entry could be built wrong.
- `run.bat` answers the prompt once for a dev start.

## 5. Configuration

### 5.1 `.env`

A plain `NAME=value` file beside the server, read at startup (and by the config
door on every read). Blank lines and `#` comments are skipped; whitespace is
trimmed. **The environment wins over the file.** A leading `~` in a path value
is the home this process started in (`HOME`, else `USERPROFILE`).

The entry point reads: `OLS_HOST`, `OLS_PORT`, `OLS_WEIGHTS_DIR` (default
`weights`), `OLS_DATA_DIR` (default `data`), `HF_TOKEN`, `CIVITAI_API_KEY`,
`OLS_API_KEY`, `OLS_RATE_LIMIT`, `OLS_THREADS`, `OLS_GALLERY_MAX`,
`OLS_ELEVATE`.

- `OLS_PORT`: digits only, 1..65535, or the start says
  *"Invalid OLS_PORT=…: the port must be a number from 1 to 65535."* and exits 1.
- `OLS_RATE_LIMIT`: empty → 600; `0` disables; else 1..1000000 or exit 1.
- `OLS_THREADS`: default `min(hardware cores, 12)`; a named value must be
  digits 1..4096 or the default stands (the same rule the config door refuses
  with — this is the one way a value still reaches here, a file edited by hand).
- `OLS_GALLERY_MAX`: a count of pictures up to 1 000 000, or the module's own
  default stands with a line saying so — a server that will not start over the
  size of one room is harder to notice than a room that is.
- `--home <dir>` (from the machine's boot entry): start from that folder — the
  config file, the pages, the data and the weights are all read from there.

### 5.2 The config door

`GET /api/config` → `{"options":[{"name":…,"secret":bool,"value":…,"live":bool}]}`.
A secret's value is `"set"` or `"unset"`, never the key itself. `live` says
whether a change reaches the running server or waits for the next start.

`POST /api/config` with `{"NAME":"value", …}` (values must be strings; unknown
names refused by name) writes the whole file at once through a temporary name
and a rename — `.env` holds the keys this server runs on, and a write that died
halfway would leave a person with half a config or none. Then every live option
is applied to the running server. The answer is the same rows a GET reads, so a
page reads its change back rather than guessing.

| option | secret | live | shape |
| --- | --- | --- | --- |
| `OLS_API_KEY` | yes | yes | any; changing it changes the key every door asks with |
| `CIVITAI_API_KEY` | yes | yes | any; to the image module |
| `HF_TOKEN` | yes | yes | any; to both modules |
| `OLS_PORT` | no | no | blank → default 8765; else 1..65535 |
| `OLS_DATA_DIR` | no | no | |
| `OLS_HOST` | no | no | |
| `OLS_RATE_LIMIT` | no | yes | blank → 600; 0 disables; else 1..1000000 |
| `OLS_STARTUP` | no | no | true/false/blank — and writing it carries the wish to the machine's manager the moment it is written (§12), so the line in the file and what the machine does at boot stay one fact |
| `OLS_THREADS` | no | no | blank or digits 1..4096 |
| `OLS_WEIGHTS_DIR` | no | no | |
| `OLS_ELEVATE` | no | no | blank (asks) or `on`/`off` — any other word is a maybe the machine cannot act on |

A value that breaks its own rule is refused naming the rule, e.g.
*"Invalid value for 'OLS_PORT': a blank leaves the default 8765, otherwise this
must be a port number from 1 to 65535."* No value carries a line break.

## 6. The job law

Every engine's work rides a job record. The law is shared (`src/Common.h`); the
studios own their records.

- **Names:** `makeId(prefix)` = `<prefix>-<12 hex>-<8 hex>` — kind, a part drawn
  from the clock, a tag, so two jobs asked for in the same moment still get
  different names.
- **States, spelled once:** `queued` (believed in, standing for its turn),
  `running`, `done` (got all of its own work done), `error` (stopped with a
  reason), `cancelled` (stopped because someone asked). `unknown` is not a state
  a job wears — it is a door's answer that no job was found.
  Standing = `running|queued`; ended = `done|error|cancelled`.
- **Kept:** each engine keeps its newest **200** job records (`kKeptJobs`); when
  one runs past the cap, the oldest name leaves every map at once — a record
  left in one map while its rest stands in another would be a lie a page could
  read.
- **A started job answers `202 {"job_id":"…"}`.** Its record is read with
  `GET …/api/jobs/<id>`; a name the machine does not hold answers
  `404 {"error":"Unknown job: <id>"}`.
- **A stop ask** (`POST …/api/jobs/<id>/cancel`) answers `200 {}` or, when it
  could not, names which of the two this id is: *"Unknown job: <id>"* against a
  name nothing holds, *"This job has already stopped"* against a job that ended —
  so a page that pressed stop twice learns the job stopped, not that the id went
  missing.
- **Doors that start work open with a POST:** *"That door opens with a post"*.
- **The card is one:** every hand that stands weights on the working device,
  draws with them, or takes memory back — in any module — waits on the one
  `deviceCard()` mutex. Two graphs built at once walk the same device state
  from two threads, and the file the crash leaves behind names it.

### 6.1 The event stream

`GET /api/jobs/<id>/events` (image studio) is the one server-sent-events door.
The transport answers it before routing, after the key gate and the rate gate:

```
HTTP/1.1 200 OK
Content-Type: text/event-stream
Cache-Control: no-store
Transfer-Encoding: chunked
Connection: close
```

Frames are the image module's own events (§ of the image spec); the stream ends
with `event: end\n\n` and the terminating chunk. A stream that threw mid-flight
says so in the shape every other frame is written in —
`event: error` with `data: {"type":"error","message":"stream failed"}` — so a
page reading a stream never has to guess why it stopped. The head says what the
transport actually does when the stream ends: the connection closes, so it is
not promised to anybody.

## 7. The cache law

Every successful json answer states how long it may be reused
(`Cache-Control: public, max-age=N`); a refusal always carries `no-store` —
from `jsonError` itself, so a throw answering outside the reading block cannot
leave a refusal a cache could hold. The browser cache in `webui/status-cache.js`
reads the header and re-asks only past the window.

`apiFreshSeconds(path)` — entries cover their sub-paths; unlisted paths get 5 s:

| path | seconds | why |
| --- | --- | --- |
| `/api/monitor`, `/api/jobs`, `/api/timings`, `/api/system`, `/api/network` | 2 | whatever a job answers for is read fresh; the machine samples its counters every two seconds |
| `/api/network/scan` | (not listed → 5) | left out on purpose: it does work on being asked, and a forced scan may not be answered out of a browser's pocket |
| `/api/service` | 4 | |
| `/api/config` | 15 | changes only when a person changes it |
| `/api/models`, `/api/gallery`, `/api/refs` | 2 | a model table's truth changes when a job changes it |
| `/api/prefs` | 20 | |
| `/project/api/index`, `/revs`, `/tree`, `/build-status` | 2 | the reads a page asks for most often, held ready by the module |
| `/project/api/file`, `/file-outline` | −1 → `no-store` | content-addressed bytes belong in the page's own store by oid, not in a cache keyed on an address |
| everything else | 5 | |

The rule behind the table: **what a job can change is read fresh; what changes
only when a person changes it may be held.** It costs the machine nothing it
was not already paying — each service builds its answer once and keeps it until
a job marks it stale.

## 8. The pages

### 8.1 Where they come from

The build compiles the tree's `webui/` folder into the binary
(`webui-embedded.cpp`, read through `src/WebAssets.h`). At run time:

- Whether a folder of pages stands beside the server is decided **once**, when
  the server is pointed at its pages (`setStaticDir("webui")` at startup, or the
  expand door). Nothing watches the folder while the server runs.
- With the folder outside, every request reads it as it stands — pages may be
  edited under a running server. Any short reading (missing, empty, unreadable,
  outside the folder, over 64 MiB) falls back to the copy carried inside, so a
  browser never gets half a page and a released `.exe` stands on its own.
- A folder standing for a path answers its `index.html`.

### 8.2 Serving a page

- Path percent-decoded (a path never reads `+` as space); must stay inside the
  pages folder after normalisation and canonicalisation, or `404`.
- **ETag:** outside files `"size-modifiedcount"`; embedded copies `"e-<size>"`
  (an inside copy has no write time; its own bytes are its identity).
  `If-None-Match` matching (or `*`) answers a bare `304` — body and
  `Content-Length` cleared.
- `Cache-Control: public, must-revalidate, max-age=0` — page files change
  between visits, so they are never served stale; revalidation plus the etag
  makes an unchanged file cost only a 304.
- `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer` on every page.
- `Content-Security-Policy` on html and svg:
  `default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; style-src-attr 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'`
  — the pages apply inline style attributes to stand their parts at read
  numbers, and the browser that reads this header falls back `style-src-attr` →
  `style-src`, so both carry it; `data:` images because the webui draws kept
  files as data URLs.
- Content types by extension: `.html`/`.htm`, `.js`/`.mjs`, `.css`,
  `.json`/`.map`, `.png`, `.jpg`/`.jpeg`, `.wav`, `.svg`, `.ico`; anything else
  `application/octet-stream`.

### 8.3 The expand door

`GET /api/webui/expand` → where the pages come from now:

```json
{"folder":"webui","outside":bool,"words":"the pages served are the copies in webui beside this server" | "the pages served are the copies carried inside this binary: nothing stands in webui yet"}
```

`POST /api/webui/expand` writes every page this binary carries out beside the
server and starts serving those copies:

- A folder that already holds files is left exactly as it is:
  `400 {"error":"the pages are already outside: webui holds files"}` — nothing
  here overwrites a webui that is already outside.
- Every written file's name must stay inside the folder; a write that fails
  answers `500 {"error":"Could not write the page <name>"}`.
- Answer: `{"expanded":true,"folder":…,"outside":true,"files":[{"name":…,"bytes":N}],"bytes":N}`.
- Any other method → `405 {"error":"The pages are read with a GET and taken out of the server with a POST"}`.

The config panel reads the GET before it offers the hand, so a machine already
serving from a folder does not offer a hand that can only refuse.

## 9. The file doors

`GET|HEAD /files/<kind>/<name>` (image studio) resolve the name through
the owning module — the module decides which kinds exist and what a name may be
(never a path; a name cannot hold a slash) — and serve the bytes:

- Unknown name, missing file, or a file over **64 MiB** →
  `404 {"error":"Not found: <name>"}`.
- **One byte range** is honoured (GET/HEAD, no comma): `bytes=from-to`, digits
  only; an empty end means the file's own end. A range that cannot be read as
  byte counts, or that ends before it starts, is **ignored** — the whole file is
  served, which is what HTTP asks of an unknown ask. A range that reads well but
  starts past the file's end answers `416` with `Content-Range: bytes */size`,
  `Content-Length: 0`, and `no-store` (nothing was given for this ask, so
  nothing of it may be kept). A valid range answers `206` with
  `Content-Range: bytes from-to/total`.
- Whole and partial answers carry `Accept-Ranges: bytes` and
  `Cache-Control: public, max-age=31536000, immutable` — a served file's name is
  its item id: contents under a name never change, so these may be cached
  forever.

## 10. The machine's readings

### 10.1 `GET /api/system`

One snapshot document, composed fresh each read:

- `devices[]` — from the ggml linked into this binary first (name, description,
  `type: gpu|accel|cpu`, `total_bytes`/`free_bytes` where the device states
  them), then anything the image module names that ggml does not, deduped by
  name. A device no library names is not drawn.
- `cpu` — `cores`, `name`; `load {one,five,fifteen}` where the system keeps
  load averages (Linux); `threads_load[]` — the share of each thread in use
  between two readings of the kernel's per-processor record (Windows; the first
  reading states nulls, a kernel that keeps no record states nothing at all).
- `disk {total_bytes,free_bytes}` — the volume this server runs on.
- `version`, `compiled`, `compiled_at` — the code this binary stands on, exactly
  as the build named it (a development name is the dev tag alone; a release
  names its commit — an artifact tied to the one source tree it can be re-created
  from). The build writes the moment into a file of its own; never `__DATE__`.
- `hostname` (capitals, so the tab reads a name as a name), `os`
  (`win11`/`win10` from the kernel's own answer; the distribution's name plus
  `-server` on a headless Linux), `backend` (the build's device word, e.g.
  `vulkan1.4`, `cuda70-x86-64-v3`; a vulkan build asks the machine's loader the
  highest version its devices speak, and a loader too old to be asked says
  nothing).
- `webui_at` (+ `webui_file` when the pages stand outside) — the moment the
  newest page file was written (walked at most once a minute), or the build
  moment for a server standing on the copy inside.
- `memory {total_bytes,free_bytes}`; `memory_hw` — the sticks as the machine's
  firmware states them (`kind`, `maker`, `part`, `rated_mts`, `speed_mts`,
  `modules`, `data_bits`, `bytes`, `bandwidth_gbps` = configured MT/s × data
  path ÷ 8, decimal GB/s). Firmware that names no sticks states no rating, and
  the panel says nothing rather than inventing one.
- `gpu_clocks` — the card's own running speed and ceiling where the vendor
  management library answers (NVIDIA); an AMD core keeps its clocks out of any
  such library and reads as no clocks here — the page says the card states none
  rather than guessing a figure off the product name.
- `loaded` — the image module's status document.

### 10.2 `GET /api/monitor`

The composed home-page document (`src/MonitorService.cpp`): every image job in
one list, newest first, each tagged with the engine the work belongs to, plus
the summary counts and the queue estimate. The engines own
their jobs; this only reads them. The way to the picture engine's cost ledger
is passed as a function, so an idle machine never reads the file.

### 10.3 `GET|POST /api/network` and `GET /api/network/scan`

One cross-platform view of this machine's network — Windows through its own
calls (iphlpapi, netsh, netcon), Linux through the ones that belong to it
(sysfs, /proc, netplan, iw/nmcli, hostapd) — and the hands that change it.

- `GET /api/network` → the whole document composed fresh: platform, elevation,
  uptime, features (what this kind of machine can do from here and why not
  otherwise), adapters with settings, counters and rate history, wifi state,
  access-point state, sharing state, and the ports in use.
- `GET /api/network/scan[?fresh=1]` → the wifi scan table; a plain read answers
  from the kept scan, `fresh=1` wakes the radio itself (about a second and a
  half).
- `POST /api/network` with one json object naming an action and its parts:
  `adapter_up/adapter_down {name}`, `mtu_set {name,mtu}`,
  `ip_set {name,mode:dhcp|static,address,prefix,gateway,dns[],metric}`,
  `wifi_connect {interface?,ssid,psk?,auth?,hidden?}`,
  `wifi_disconnect {interface?}`, `wifi_forget {ssid,interface?}`,
  `radio_block {interface?,on}`,
  `bond_create {name,slaves[],mode,primary?,address?,prefix?,gateway?,dns[],metric?}`,
  `bond_delete {name}`,
  `vlan_create {name?,parent,id,address?,prefix?,gateway?,dns[],metric?}`,
  `vlan_delete {name}`, `share_enable {external,internal}`, `share_disable {}`,
  `ap_start {interface?,ssid,psk?,band?,channel?,country?,max_clients?,beacon_ms?}`,
  `ap_stop {}`.
  The answer is always `{"did":bool,"words":…,"action":…}` — and the words are
  the machine's own tools' words, never a guess about what probably happened.

### 10.4 The fleet: `GET /api/peers`, `GET /api/peers/hello`

This machine keeps tabs on the other open-local-services servers it can meet on
the LAN (`src/PeerService.h`):

- A start reads the table (`data/peers.json`) and sweeps the machine's networks
  **once**, in the background, after the doors already answer — known
  acquaintances at their saved addresses first, then every configured network,
  only this server's own port, only addresses answered by this server's own
  key. A server never knocks on its own door.
- Every meeting writes both sides: the prober records who answered; the answered
  records whoever asked (`/api/peers/hello?name&port&version&backend` — the
  credential was weighed at the door before this point; a server that answers a
  different key never records anything).
- A card is this machine as it stands at the moment of asking: `name`, `port`,
  `version`, `backend`, `uptime_s`. Nothing secret rides on it — the key is the
  credential on the ask, never a word inside it, and is never written into the
  table.
- `GET /api/peers` → this machine's own card beside every acquaintance, each
  saying where it was met and whether it stood up during this run. The header's
  machine picker reads it; choosing a peer opens that machine's pages carrying
  this visit's credential once, in the one place a credential is ever named in
  an address.

## 11. Fetching over the network

Every module asks the same way (`platform::curlFetch`): `curl -sSL --fail
--retry 3 --create-dirs`, resuming (`-C -`) first and giving the resume up
**once** — an address that refuses a resume (curl 33) or rejects the range (36)
starts the file clean here, because a refused resume is not an error in the
file. Measured 2026-10-28 on X1: three weight downloads died retrying a refused
resume and left a page showing on-disk models as not downloaded. Whether the
bytes that arrived are the bytes meant is decided by the digest the caller takes
afterwards, never by the fetch having finished.

A token travels as `Authorization: Bearer` when the module holds one. Every
part of the command — destination, url, token — must pass `quotableInShell` or
the fetch is refused (returns −1) rather than escaped: a shell has no quoting
rule that makes every character safe, and a name carrying a quote from a source
this machine does not control would stop being a name and become a command. The
same rule guards the paths Project Studio's doors take toward the machine's
folders.

A sliced fetch (`maxSeconds` above zero) asks for no retries of its own —
`--retry` re-reads `--max-time` as the patience of each try rather than of the
command (measured 2026-11-19) — and the slicing loop outside is the retry,
resuming from the bytes the round left behind.

## 12. The service door: `GET|POST /api/service`

What this machine does about starting this server by itself. Nothing here
installs a server anywhere: it is this program, out of this folder, set to start
when the machine starts.

- `GET` → the state the machine's manager states: `{name, manager, installed,
  running, startup: automatic|manual|disabled|"not installed", target, root,
  detail}` plus the words of the last action asked.
- `POST {"action": …}` with one of:
  - `startup_on` — set up and start on boot;
  - `startup_off` — leave it alone at boot, without stopping anything running;
  - `remove` — take it off the machine's list (and write `OLS_STARTUP=false`,
    because a line that still says this machine starts the server at boot would
    be a lie once nothing stands to start it);
  - `restart` — the soft hand: this server stops answering at its door and the
    program on disk starts as its own child (§1.5);
  - `shutdown` — stop it, and let this process go with it.
- The answer is `{"did":bool,"words":…}` at 200 (500 when it did not happen),
  plus `service` (the state after) except for `shutdown`, plus `env_words` for
  the startup/remove asks — the `.env` line and what the machine does are one
  fact, so the ask that changes one writes the other beside it:
  `"OLS_STARTUP=true written to .env"` or
  `"the machine did it, but the .env line was not written: <err>"`.
- The words are the machine's own manager's words, kept whole rather than
  tidied. A server that is not elevated is refused by its own machine and says
  so with that machine's words rather than pretending the change was made.
- Linux: the systemd unit (`open-local-services.service`). Windows: a console
  program cannot ride the service control manager — it is written off as failed
  at boot — so boot is a system task carrying the folder to start from; where a
  real service of this name exists, it is worked with `sc`.

## 13. The web shell

Plain JavaScript, no framework, no build step. `webui/` is served live from disk
when present, else from the copies inside the binary (§8).

- **The chrome** (`common.js`) is mounted on every page: the header (brand, the
  line naming the work going on, the machine picker) and the nav —
  `home` (`index.html`), `image studio` (`imagegen.html`), `project studio`
  (`project.html`). The bare address opens the home page.
- **`key-shim.js`** wakes first: a page that woke with `?key=` in its address
  takes the credential up and wipes the address clean; a page whose doors
  refuse asks the person where it stands and hands the answer to `POST /`.
- **`status-cache.js`** is the browser cache the §7 law speaks of: it keeps the
  last copy of a reading with the time it was read, honours the answer's own
  freshness, and hands a page its last copy (up to an hour old) when the machine
  cannot be reached — a page says when it is drawing a held copy.
- **The log** (`index.html`, `common.js`) is one record for the whole machine —
  every studio writes its lines there; green is progress, amber a stop you
  caused, red a refusal or an error. Clearing it touches only this browser.
- Nothing on a page is claimed before a reading arrives: every panel starts at
  "not read".

## 14. Constants, in one place

| name | value | where |
| --- | --- | --- |
| `kApiPort` | 8765 | `OpenLocalServices.h` |
| `kMaxLiveConnections` | 256 | `OpenLocalServices.h` |
| `kStopGraceMs` | 5000 | `OpenLocalServices.h` |
| `kConnectionOverallMs` | 120000 | `OpenLocalServices.cpp` |
| per-read timeout | 10000 ms | `OpenLocalServices.cpp` |
| `kMaxHeaderBytes` | 1 MiB | `OpenLocalServices.cpp` |
| `kMaxBodyBytes` | 2 MiB | `OpenLocalServices.cpp` |
| git body cap | `OLS_GIT_MAX_BODY_MIB`, default 512 (1..16384) | `OpenLocalServices.cpp` |
| served file / page file cap | 64 MiB | `OpenLocalServices.cpp` |
| `kAuthSilentAfter` / `kAuthSilentResetMs` | 3 / 30000 ms | `OpenLocalServices.cpp` |
| auth jitter | 6–36 ms | `OpenLocalServices.cpp` |
| rate limit default / max | 600 / 1000000 per minute | `main.cpp`, `OpenLocalServices.h` |
| `kKeptJobs` | 200 | `Common.h` |
| threads default / cap | min(cores,12) / 4096 | `main.cpp` |
| `OLS_GALLERY_MAX` cap | 1000000 | `main.cpp` |
| error words cap | 200 characters | `OpenLocalServices.cpp` |

## 15. How to read a refusal

Every refusal is `{"error":"…"}` at a status from the table in §1.2, with
`Cache-Control: no-store`. The words are for the reader who made the ask: they
name the rule the ask broke, or which of two things the reader's id or name is,
and they never carry a credential, a stack, or more than 200 characters (non-
printables become `?`). A door that could not reach its module answers 502
naming the module; an address that does not exist answers 404 "No route".
