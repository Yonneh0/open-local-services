# DISCLAIMER
This project has been gingerly touched by the gentle hand of AI. If that tickles your raspberries in the wrong way, feel free to leave.
All jokes aside- this has not been validated by and first parties- let alone second parties. It very well could turn your H200 into a P100. No warranties, or promises. Brought to you by `Carls' Jr`.

- Here be dragons!
- This project is under development. It is not intended for public use, yet.
- Please wash your hands before returning to work.
- This product is not yet fit for human consumption. It has errors-a-many, and is a **wrok in porgress**. Feel free to pick it apart, and see what makes it tick, tho! <3

## License

MIT. Google it yourself <3 (Or read the full unabridged text, in the coveted `LICENSE` file beside this readme.)

## Aboot

`Half-Assed Solutions`™ is a project, ran by Yonneh, to release some "actually kinda useful" things that are developed during random hobby development. Yonneh wears most of the hats in this organization- in fact, he wears all of them; but that's only because he is the sole member. If literally ANYONE more competent was around- he'd be fired.
This project, and any others tagged with `Half-Assed Solutions` are completely open-source, and MIT Licensed.
If you found this project under a rock somewhere, and want to see if it was ever finished- it probably came from https://github.com/Yonneh0/open-local-services at some point.

---

if you have made it to this point, and are still reading- I would like to thank you, and question your sanity. have a nice day.

```
MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMCARL
MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM `MM' VMMMMM
MMMMMV  MV  MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM  VM  MMMMMM
MMMMMM  M  mMMMMMMMMMMMMMMMMMMMV'"     "`VMMMMMMMMMMMMMM  MMMA `M  MM  MM
MM  VM  M  MMMMMMMMMMMMMMMV"                 "VMMMMMMMMM.  'MM  M  M' .MM
MM.  M  M  MV  VMMMMMMMV'                      "VMMMMMMMMM.  "  V  V .MMM
MMA  V  M  M' ,MMMMMMV'                          "VMMMMMMMM.  ..     mMMM
MMMA `     V  MMMMMM'                              `VMMMMMMm  "S"   mMMMM
MMMM  .,.,   AMMMMV                                 `VMMM"""   :   .MMMMM
MMMM  "B"   MMMMMV                                    M"      .'  .MMMMMM
MMMM   :   AV"  V                                     `   .mm.    MMMMMMM
MMMM.  `.                                              ..MMMMMm   MMMMMMM
MMMMM.. .  .mMMV  .                                   . VMMMMMMA   VMMMMM
MMMMMM  AMMMMMM'  *         <^@^>        <==>        .* 'MMMMMMm    MMMMM
MMMMM'  MMMMMMV  .I                                 .a@. V'"MMMMA    MMMM
MMMMM   MMMMMM(  a@:.                             .' @@! .   "MMMm    MMM
MMMM'   MMMV""'  !@a :.                         .';.a@@R ,             MM
MMMV    MV"   :  :@@@: :.                     .:  a@@@@! ..............mM
MMM'          .  `@@@@ : `...             ..:' : a@@@@@' MMMMMMMMMMMMMMMM
MMM  ..........   @@@@@a  :  :'`:`------':  :  a@@@@@@@  MMMMMMMMMMMMMMMM
MMMMMMMMMMMMMMA   `@@@@@@@a  :  :   ::   :  a@@@@@@@@@' :MMMMMMMMMMMMMMMM
MMMMMMMMMMMMMMM.   `@@@@@@@@@@aaA. .;|. .Aaa@@@@@@@@@' .AMMMMMMMMMMMMMMMM
MMMMMMMMMMMMMMMM.   `@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@   mMMMMMMMMMMMMMMMMM
MMMMMMMMMMMMMMMMM.    @@@@@@@@@@@"oOo.oOOo"@@@@@@@'   mMMMMMMMMMMMMMMMMMM
MMMMMMMMMMMMMMMMMMm    `@@@@@@@"OOOOOOxOOOOO"@@@V    mMMMMMMMMMMMMMMMMMMM
MMMMMMMMMMMMMMMMMMMA.     `@@@"OOOOOOOOxOOOOO"@'   .AMMMMMMMMMMMMMMMMMMMM
MMMMMMMMMMMMMMMMMMMMMA.     ""V@@AOOOOOOxOOOOO.  .AMMMMMMMMMMMMMMMMMMMMMM
MMMMMMMMMMMMMMMMMMMMMMMMm.       `OOOOOOOxXOOOo.mMMMMMMMMMMMMMMMMMMMMMMMM
MMMMMMMMMMMMMMMMMMMMMMMMMMMAm..   `OOOOOOoxOOOO:MMMMMMMMMMMMMMMMMMMMMMMMM
MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMA`OOOOOOOxOOOO:MMMMMMMMMMMMMMMMMMMMMMMMM
MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMA`OOOOOOxOOOO;MMMMMMMMMMMMMMMMMMMMMMMMM
MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMA`OOOOOOOOO;AMMMMMMMMMMMMMMMMMMMMMMMMM
MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMA`OOOOOO;AMMMMMMMMMMMMMMMMMMMMMMMMMMM
WIZMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMmmmmmmMMMMMMMMMMMMMMMMMMMMMMMMM*MJJ

88888b   d888b  88b  88 8 888888    88888b   888    88b  88 88  d888b  88
88   88 88   88 888b 88 P   88      88   88 88 88   888b 88 88 88   `  88
88   88 88   88 88`8b88     88      88888P 88   88  88`8b88 88 88      88
88   88 88   88 88 `888     88      88    d8888888b 88 `888 88 88   ,  `"
88888P   T888P  88  `88     88      88    88     8b 88  `88 88  T888P  88
```

# open-local-services

A local server for image generation and video clips. One server process serves the web UI and all API endpoints from one fixed port. The server links no image engine and no ggml at all: the engine is a library beside the binary — `stable-diffusion-cuda.dll`, `-vulkan`, `-cpu-avx512`, `-cpu` on Windows, `libstable-diffusion-cuda.so`, `-cpu-avx512.so` or `-cpu.so` on Linux — and the server opens the one that answers the machine it stands on, in that order, when it starts. One server binary per platform therefore runs on every machine of that platform; what differs between machines is the library lying beside it.

## Repository

This is the canonical repository: [https://github.com/Yonneh0/open-local-services](https://github.com/Yonneh0/open-local-services).

```sh
git clone git@github.com:Yonneh0/open-local-services.git   # ssh
git clone https://github.com/Yonneh0/open-local-services.git
```

Weights, generated data, and `.env` are not in the repository and never are; they live beside the server as `weights/`, `data/`, and the settings file named below.

## Quick start

```sh
run.bat                 # Windows development build and start
run.sh                  # Linux development build and start
build-rel.bat           # Windows release artifacts
build-rel.sh            # Linux release artifacts
```

The development scripts build missing parts, then start the server. Pass `build` to stop before starting. `run.bat` accepts `vulkan`, `cuda`, `nodevice`, `key=`, `rate=`, `arch=`, and `fresh-full-rebuild`. With no shape word it reads the machine: a card the machine names — or one named by `arch=` or the environment — builds the CUDA engine, a box whose probe names no card builds the Vulkan engine from the SDK it carries, and a box with no SDK at all builds the cpu engine. The CUDA engine carries kernels for every supported card generation (`sm_70`, `sm_75`, `sm_86`) in one library, whichever card built it. The server itself builds the same way on every machine — it links no engine — and starts on the engine library its machine opens; `OLS_ENGINE` in `.env` names another choice. `run.sh` accepts `build`, `key=`, `rate=`, `cuda`, `cpu`, `cpu-avx512`, and `fresh-full-rebuild`; with no shape word a box with a CUDA toolkit gets the CUDA engine and one without builds the cpu engine, and naming `cuda` where no `nvcc` answers is refused rather than answered with another shape. `fresh-full-rebuild` is the word that clears the engine tree and configures it for the shape now asked for — a tree whose claim differs is refused before cmake is asked anything, because a flipped claim is a different library, not a rebuild.

What a shape's name is worth, measured rather than assumed: ggml's instruction-set
options are a claim made to the compiler, not a promise kept at run time. Asking
for avx512 hands `-mavx512f` to every object of the cpu backend, the compiler
then uses those registers wherever it likes — including where no dispatch guard
stands — and the engine's own `ggml_cpu_has_avx512()` answers from the macro
those options set, so the library never finds out that the chip beneath it has
none. Such a library dies of `SIGILL` at its first touch: seen 2026-11-27 on a
kaby lake box (avx2, no avx512) as a trap inside `ggml_cpu_init`, in a cpu engine
that its own door had named plain `cpu`. So an avx512 build is an avx512
artifact, named `cpu-avx512` and handed only to a chip that says yes to f, vbmi
and vnni, on either platform; the plain cpu engine asks for nothing above what
this fleet stands on (avx2, fma, f16c on Linux — Windows leaves them at the
engine's own defaults, which its measured artifacts were cut against). ggml does
have one setting that dispatches on the chip, `GGML_CPU_ALL_VARIANTS`, and it
requires `GGML_BACKEND_DL` — backends as separate libraries this project does not
stage; recorded as pending work in plan.md.

## How a release works

There is no release server here, no pipeline, and no version numbers — that is the whole design. **The version is the commit hash**, and the code it names is what gets stamped into the binary; nothing else publishes or deploys anything.

1. **Commit first.** A release build refuses an uncommitted `src/` or `webui/` — a binary stamped with an older commit than the code inside it would lie about what it is.
2. **Build the server and the engines.** The server is one shapeless build per platform — it links no engine — so each release target ships that same binary beside one engine library: `build-rel.bat` builds `x64-windows` (the AVX-512 cpu engine), `x64-windows-plain` (the AVX2-baseline cpu engine), and `x64-cuda` (one CUDA engine carrying kernels for `sm_70`, `sm_75` and `sm_86`); `build-rel.sh` builds `linux-x86_64`, `linux-x86_64-cuda` and the cross-built `linux-arm64`. Every binary carries the commit inside it, and the card generations inside the CUDA engine are read back off the artifact rather than trusted from what was asked.
3. **Each finished folder ships as one archive** — `Release/open-local-services-<commit>-win-<target>.zip`, or `…-linux-<target>.tar.gz` — and its sha256 goes into that platform's manifest, `Release/SHA256SUMS-win.txt` or `SHA256SUMS-linux.txt`: one release is built on the machines whose shapes it ships, and two builders keeping one file would erase each other. `build-info.txt` names the commit, the toolchain, and the pinned engine sources everything was built from. The archives are what carry between machines, and the small ones (the Vulkan and CPU shapes, some 50 MB) are committed — here the repository is the release, and what is committed is what runs. The CUDA archives are the measured exception: cuBLAS, cuBLASLt and cuFFT make them roughly 850 MB compressed, past what any GitHub push admits in one file, so they live beside the tree under their committed sha256 and move box to box over the link. Build scripts never commit anything themselves; they name any archive git has not been shown yet.
4. **A machine that compiles nothing opens one archive and runs it.** Inside: the server, the one engine library it opens at start (`stable-diffusion-cuda.dll`, `stable-diffusion-cpu-avx512.dll`, `stable-diffusion-cpu.dll`, or the `.so` twins), and a `webui/` folder served live the way the development tree's is. CUDA artifacts carry the CUDA runtime the engine really imports, and Windows artifacts the Microsoft C++ runtime, read off their import tables rather than guessed at. A `weights/` folder, a `data/` folder, and a `.env` beside the binary are the same folders and file the development tree uses.
5. **Built for one place, run anywhere of its kind.** On Linux the cpu part of every engine is baseline-plus-runtime-detection, so an artifact built on an AVX-512 machine runs untouched on one that has never seen it; on Windows, where the compiler writes the instruction sets in directly, the release ships two cpu engines and the server opens the AVX-512 one only on a chip that answers `cpuid` yes. Linux artifacts state the newest glibc symbol they ask for, and are built on the same Ubuntu release as the machines that will run them.

## Access

The server listens on `OLS_PORT`, defaulting to `8765`. If that port is already in use, it refuses to start and does not roll over to another port.

The UI and API use one key from `.env`. Acceptable key formats are:

```http
X-Api-Key: <key>
Authorization: Bearer <key>
GET /path?key=<key>
Cookie: ols-key=<key>
```

A successful API door may set `ols-key`, so later requests do not need to repeat it. If no key is configured, all doors open. After three consecutive failed auth attempts, connections get no response until a 30-second gap passes. API requests use a token bucket with timed refill; refused requests return HTTP 429 with `Retry-After` and do not execute or use the caller's input.

## Configuration

`.env` contains local settings:

```ini
OLS_HOST=0.0.0.0
OLS_PORT=8765
OLS_API_KEY=replace-this
OLS_RATE_LIMIT=600
OLS_THREADS=8
OLS_WEIGHTS_DIR=weights
OLS_DATA_DIR=data
OLS_STARTUP=false
OLS_ELEVATE=off
OLS_SERVICE_NAME=open-local-services
OLS_ENGINE=
OLS_ENGINE_DIR=
```

`OLS_ENGINE` names the engine library the server must open (`cuda`, `vulkan`, `cpu`, or a flavored file name like `cpu-avx512`); left empty, the server picks in order — CUDA if the card answers, Vulkan if the loader opens, then `cpu-avx512` where the chip itself answers f, vbmi and vnni, then the plain cpu engine. `OLS_ENGINE_DIR` names a folder to search for engine libraries before the folder the server itself stands in.

`OLS_SERVICE_NAME` names the machine's home entry for this server — the scheduled task on Windows, the systemd unit on Linux — where that home goes by anything other than the default (on X1 it is `ols-serve`). The environment wins over the `.env` line; the hand, the doors, and the state readings all ask the scheduler for this one name.

The config panel uses `GET /api/config` and `POST /api/config`. Secret values are shown only as `set` or `unset`.

## Standing on a machine — and being updated there

**Development.** `run.bat` / `run.sh` build what is missing and start the server, asking the machine's own elevation once — the server first stands at the level its machine's manager works at (a Windows admin's token at High integrity, or the machine account a task carries; on Linux, uid 0, reached through `sudo` when it answers without a password), and a server that does not stand there says so and refuses to start. Which level it stands at is one of the lines it prints as it starts. `OLS_ELEVATE=off` declines the prompt — on a machine that was not raised, that is a refusal to start, not a licence to serve half-armed.

**A machine home.** So a boot entry, a task ended, or a service started can hold the level the server must stand at, the server's home is the machine's own entry: a scheduled task on Windows (`OLS_SERVICE_NAME`, running `run.bat`, at startup), a systemd unit on Linux — the shape `startup_on` writes itself, `ExecStart` naming the program directly, so a hand's `systemctl restart` stands up exactly what was staged and no boot asks a compiler for anything; a unit that instead runs `run.sh` also stands, and builds at boot the way the Windows task does. The Windows task must be registered with **highest privileges**: a task without them hands the machine account a Low integrity label, and that confined token is refused the tree it serves (the folders carry an explicit no-write-up for Low) and the scheduler alike — the server reads the label and names the mending command (`schtasks /change /tn <task> /rl HIGHEST`). `GET /api/service` answers whether such an entry exists, running, starts at boot, and at which level the server stands; `POST /api/service` with `{"action":"startup_on"|"startup_off"|"restart"|"remove"}` walks its roads. `startup_on` stands the task/unit for this program when none exists and asks the machine to run it.

**Updating a running server.** Both roads stage into `bin/` first — the room `/api/update` reads. `run.bat`/`run.sh` and the release scripts do the staging — the binary and the engine libraries under their shape names; when the running server belongs to the machine's manager and this shell cannot stop it, the scripts stop honestly, leaving the fresh set staged. `POST /api/update` (key-gated) hands a running server its update behind its own answer: it checks what stands staged, stops itself, and a detached hand waits for the file to let go, moves the staged set — the binary and every `stable-diffusion-*` engine library — into place with the old ones set aside under `.old` (put back if the new one cannot land), and asks the machine's own entry — the task by its name, the unit by `systemctl restart` — to stand the new program up. Where no manager holds the server, the hand starts the new program itself, at the level the old one stood at. Everything the hand does goes to `data/update-hand.log` beside the answer's words; `last_words` in `/api/service` keeps the last answer for the next reading.

**Two ways a server comes up confined.** A machine's entry and the program file each carry a label, and the lower one wins. A Windows task registered without highest privileges hands the machine account a Low integrity token, which is refused the tree it serves and the scheduler alike - the entry above says how to mend that. And a program file carries a mandatory label of its own: Windows starts a process no higher than the label on its image, so a binary that reaches the runtime folder by rename - keeping the label of the build folder it was born in, which is inherited from the project root when the root wears one - comes up confined however the task was registered, and the refusal that follows looks like the task's fault and is not. Both are mended the same way, and the scripts do it without being told: the run script's replace step takes the destination away and lays a fresh file, and the update hand **copies** the binary rather than moving it, so the program the entry starts wears the runtime folder's own defaults. Engine libraries keep the rename (a library's label caps nothing, and the one the server runs on cannot be copied over), stepping aside under `.old` first so a new one can cross where renaming over a mapped file is refused. `GET /api/service`'s `level` and the start line now name which label they read, and say when it is the file's.

## Web UI

Pages are embedded in the binary at build time. If a `webui/` folder exists beside the running server, it is served from disk instead.

- `GET /` is the one address open to every visit: a browser opening it reads the home page, and a program asking it (no `Accept`, or `application/json`) gets what the doors ask of as json - `api_required` and `peers`, never a letter of a key. `POST /` is where a credential is handed as it is handed to any door: the answer says only whether it opens them, and a yes writes the `ols-key` cookie on the connection. Pages themselves are shells that stand open; when the machine asks for a key the browser does not carry, every page prompts for it before its first ask leaves for a door, and asks again when a key stops opening them.
- Every page's header carries the running-state line and the state-worn home tab; the two studio tabs each wear, across their top-right corner, a badge naming how many jobs that engine has running, how many wait their turn, and how many models stand in memory - the machine's own counts, worn only while something stands, each explaining itself on hover, and never in the way of a click.
- `/index.html` is the home page: cpu, gpu, and memory levels with their recent history, the hardware each engine stands on, the models open in memory, every job standing or running and what it says about itself, the network panel, local settings, and the service door. The corner clock tells the hour - or, while work stands in the queue, how far the queue is away, counted from the machine's own statements and never guessed by the page.
- `/imagegen.html` is the image studio: prompt and settings beside the gallery of what this machine drew, reference images for image-to-image, the weights room with its downloads and loads, a queue that answers cancelling and reordering, and preferences the page remembers. Still images and video clips are pressed through the same door.
- `POST /api/webui/expand` writes the binary's embedded pages out beside it, so a binary install can become an editable tree.

## API

Job doors answer `202` with a `job_id`; the work is watched at the job doors, and what it made is served from `/files/`. Every door below answers under the same key and rate limit.

The machine and the service:

```text
GET  /api/system             the machine: devices, memory pools, load, the dates of the pages
                             and of the binary
GET  /api/monitor            one merged reading: system levels, engine state, and the queue
                             block the corner clock counts
GET  /api/service            how this server is installed and run; POST acts with
                             startup_on, startup_off, remove, restart, or shutdown
POST /api/update             the update hand: what stands staged in bin/ (run.bat,
                             run.sh, build-rel.* lay a fresh build there) moves into
                             place while the server is down, and the machine's own
                             manager - the scheduled task or the systemd unit that
                             already holds this server - starts it again at the level
                             it set. No prompt stands anywhere; nothing staged is an
                             honest refusal that stops nothing. Answers {did,words}
                             before anything stops.
GET  /api/config             local settings; secrets read only as set or unset (POST to change)
GET  /api/timings            the cost ledger: what finished runs cost, per model, device, and kind
GET  /api/peers              the fleet table: other machines of this suite the machine knows,
     /api/peers/hello        their cards, and how they were met; the hello is the handshake -
                             it answers this machine's card and records the asker
GET  /api/network            the whole network document; POST runs one action;
     /api/network/scan       answers the last kept wifi scan, ?fresh=1 wakes the radio
```

Pictures and video:

```text
GET  /api/models             the weights room: every model card the folder holds
POST /api/models/<name>/load     ask the engine to take a model up     } each answers a job
POST /api/models/<name>/download fetch the weights a card asks for     }
POST /api/generate           one ask: prompt, settings, optional reference; answers 202 with job_id
POST /api/unload             let the engine put a model down
GET  /api/gallery            kept results, newest first (?limit= &offset=); GET or DELETE
                             one at /api/gallery/<id>
GET  /api/refs               the kept reference images; POST keeps one, drawn from the
                             gallery or from bytes the browser holds; DELETE /api/refs/<name>
GET  /api/prefs              the studio's remembered settings (POST to change)
```

Jobs:

```text
GET  /api/jobs               every job: queued, running, kept
GET  /api/jobs/<id>          one job's own record; POST /api/jobs/<id>/cancel stops it
POST /api/jobs/reorder       move a queued job along the line
```

Project Studio:

```text
GET  /project/api/index      the project ledger for the dropdown: every project with
                             its rev, counts, and remembered source
GET  /project/api/revs       {slug: rev} - the whole change detector in one read
GET  /project/api/events     the studio's live stream: an `event: changed` frame with
                             {slug, rev} when any project's stamp moves; a door closes
                             a stream it has held five minutes and the page opens the next
GET  /project/api/doors      the live table of the studio's doors: path, method, params
GET  /project/api/meta       one project's facts (?project=slug); POST edits them
POST /project/api/projects/new      {name, opts} -> {did, words, slug}; options choose
                                    a default branch and an initial README
POST /project/api/projects/adopt    {path} - register a folder that already stands
POST /project/api/projects/rename   display name only; the slug never moves
POST /project/api/projects/delete   confirm-by-name; refuses while open work stands
GET  /project/api/tree       one folder of one commit's tree (?project, ?dir?, ?ref?)
GET  /project/api/file       a file's bytes at a ref (?project, ?path, ?ref?); names the
                             blob oid and says plainly when the bytes are not text
GET  /project/api/file-outline  the symbols inside a file: functions, classes, headings
GET  /project/api/history    the log, whole or one path's (?project, ?path?, ?ref?,
                             ?limit? - 50 rows, 200 at most - ?after? a full object
                             id, ?first_parent?, ?no_merges?, ?author?, ?text?,
                             ?since? and ?until? whole unix seconds); a row carries
                             its parents[] and the refs[] standing on it beside the
                             author, the committer, and the message (4 KiB, the cut
                             named), with the first-parent diff's counts and its
                             changed[] list, and the answer says with hasMore when
                             the walk left commits behind
GET  /project/api/blame      one commit mark per line of a file (?project, ?path)
POST /project/api/commit     the editor's save: {project, branch?, message, author?,
                             files: [{path, content} or {path, delete: true}]}
                             -> {did, words, commit, rev}; an unnamed branch is the
                             project's own default, and an unborn default is how a
                             new project gets its root commit; a save whose branch
                             moved under it answers 409 with the reason
POST /project/api/build      one build of one file: {project, path} -> 202 {job,
                             state: running}; the toolchain is the extension's, read
                             from what the machine's probe found - .c/.cpp/.cc/.h
                             compile and run through msvc-cl, .py through python,
                             .js/.mjs through node, .java through the jdk; one build
                             stands per project, and the refusal names that one
GET  /project/api/build      that same build, read with ?job= - the reading below
GET  /project/api/build-status
                             ?job= -> {job, project, path, tool, state: running,
                             done, error, or cancelled; stdout and stderr - the whole
                             capture once ended, a tail while standing, each flagged
                             when cut; stage and exit_code once the harness ended;
                             timed_out when the sixty-second limit ended it;
                             duration_ms; and diagnostics: the tool's own lines read
                             as {file, line, col?, sev?, code?, msg, stream, path?}
                             rows, fifty at most, more_diagnostics when more stand}
POST /project/api/build-stop {job} -> {did, words}; the job's process tree is ended
                             and the job answers as cancelled from here on
```

The build harness is written out fresh for each job and keeps its own sixty
seconds: the toolchain reads the file where it stands, nothing is copied, and
the streams, the exit mark and the job's records land in a short root under the
studio's TEMP (`C:\Projects\TEMP\ols-build`). The harness is written for this
side of the machine - the other side refuses a build in words until its
toolchains answer there. The generated batch calls the toolchains, never git.

The same studio answers git's own smart-HTTP, under the same key gate every
other door stands behind. The address is `http://<host>:8765/git/<slug>`:

```text
GET  /git/<slug>/info/refs?service=git-upload-pack|git-receive-pack
                             the advertisement, in the dialect that half answers:
                             upload-pack the protocol v2 head - version line,
                             agent line, object-format=sha1, and ls-refs=unborn,
                             fetch=wait-for-done and server-option - and
                             receive-pack the v0 head a real `git push` asks for,
                             the marker line and its flush, then one ref line each
                             with HEAD first and the capability list after the NUL
                             of the first (report-status, delete-refs,
                             side-band-64k, quiet, atomic, ofs-delta, and the
                             agent), an unborn HEAD stated with the id of nothing -
                             asked fresh each time, never cacheable
POST /git/<slug>/git-upload-pack
                             v2 commands, chunked: ls-refs answers every ref the
                             repo carries, the unborn HEAD line included; fetch
                             answers the round it was asked - acknowledgments first
                             when no done came, then the pack wrapped in its own
                             sideband packfile section. The pack is full objects,
                             no deltas, read back through its own index before a
                             byte ships; the two walks share a bound of 65536
                             objects and say so over the wire when they reach it
POST /git/<slug>/git-receive-pack
                             the push, in either dialect it arrives: v0's command
                             lines - the first wearing its capabilities after a
                             NUL - or the v2 command section, one line per ref
                             moved ("<old> <new> <name>"), and the pack after the
                             flush; the body's own shape says which road stands.
                             A pack that travels alone has its index made
                             in-process, and a thin pack - the kind a push sends
                             by default, borrowing the base a delta stands on -
                             has that base read back through the repository and
                             checked against the id it names before the pack is
                             stood up whole. A push that only removes refs brings
                             no pack at all. The answer is report-status, the
                             protocol's own words and never a bare http error:
                             unpack ok, or unpack ng <why> for a body that did not
                             unpack, then ok or ng per ref - and when the command
                             section named side-band-64k, that whole report rides
                             channel 1 behind the band. A move that rewrites
                             history is refused everywhere; the project's default
                             branch stands protected with its own fast-forward-only
                             refusal, and its words reach the client as written;
                             512 MiB caps the pack
```

A git client carries its credential to its own doors as Basic auth, and this
server answers Basic at none of them - so for git, the key travels as a header
the client is told to add:

```sh
git -c http.extraHeader="X-Api-Key: <key>" clone http://x1.lan:8765/git/<slug>
git -c http.extraHeader="X-Api-Key: <key>" ls-remote http://x1.lan:8765/git/<slug>
git -c http.extraHeader="X-Api-Key: <key>" push origin <branch>   # from such a clone
# or named once, kept inside one clone:
git config http.<url>.extraHeader "X-Api-Key: <key>"
```

A real git 2.56 client clones a studio project, reads its refs, and pushes to it
here - a content push lands its commit, a forced push is refused and the door's
words are what the client prints, a branch is created and a branch deleted, a
5 MB push is under a second, and a pushed blob reads back byte for byte (gate
measured 2026-11-24 against scratch serves on this machine; the header form
above is what a keyed serve asks). Whether these doors should take a weaker
read-only credential than the full API key is open item O-2 (spec §15) -
until it lands, a clone address carries the machine's key with it.

Projects live under `data/projects/`, one folder each, with a bare repo and the
studio's sidecars inside; the ledger is `data/projects/index.json`, written
atomically. The fourth page is `/project.html`; its rooms answer only what these
doors state - a room whose door the machine has not been given says so on the
page instead of guessing. The code tab's file viewer wears an `edit` hand: a
vendored CodeMirror 6 pane whose save is the commit door above (a buffer past
2 MB stays read-only), and a `beautify` hand for the languages the vendored
re-formatter has words for - js, json, css, html and xml. Beside that buffer the
gutter keeps two truths: the lines this browser has typed against the bytes the
file door sent, and the lines the commit the reading stands on last wrote, the
blame door naming them one by one. A `build` hand asks the machine to compile
and run the file where it stands; its parsed problems come back as rows beside
the pane and as marks under the words, and the streams land whole once the job
has ended. GitCore
(`src/gitcore/`) reads and writes git objects, packfiles, refs, diffs, and the
wire protocol in-tree; `git.exe` stands on the machine as a test oracle only,
never called by the server.

## Network management

The home page carries a network panel: adapters and their live rates, wifi
status and scans, open ports with their processes, access-point and sharing
controls, and (on Linux) bonds and vlans.

```text
GET  /api/network          the whole document: adapters, rates, wifi, ap, sharing, ports, features
GET  /api/network/scan     the last kept scan; add ?fresh=1 to wake the radio (about 1.5 s)
POST /api/network          one action and its parts; answers {did, words} in the machine's own words
```

Actions: `adapter_up` / `adapter_down`, `mtu_set`, `ip_set` (dhcp or static),
`wifi_connect` / `wifi_disconnect` / `wifi_forget`, `radio_block`,
`bond_create` / `bond_delete`, `vlan_create` / `vlan_delete`,
`share_enable` / `share_disable`, `ap_start` / `ap_stop`. The panel is honest
about refusals: whatever the machine's own tools answer - `netsh`, `netplan`,
`nmcli`, `hostapd`, `iptables` - comes back as the words, and controls the
machine cannot honour are shown turned off beside the reason.

Windows reads through `GetAdaptersAddresses` and friends and changes through
`netsh`; a bond or vlan belongs to a teaming manager this server does not
claim to be, and the access point is the machine's own mobile hotspot - the
tethering manager behind its settings page, asked directly, watched until it
settles. Linux reads sysfs and `/proc` and writes only
`/etc/netplan/70-open-local-services.yaml`, which is checked with `netplan
generate` before `netplan apply` ever sees it; a refused file is rolled back.
Changing settings needs the machine's manager: elevation on Windows, root on
Linux - read-only either way without it. The server's own notes on the network
it made (bonds, vlans, sharing) live in `data/network-state.json`.

## Fleet awareness

At startup the server walks the networks it is attached to - each one at its real
netmask, in batches of a few dozen at this server's own port - and every door that
opens to this server's own key is an acquaintance: name, addresses, version, backend,
uptime. The walk happens once, at startup; machines already known from `data/peers.json`
are asked at their saved addresses first, and the sweep fills around them. A server
that is asked the hello records the asker in its own table too, so one meeting joins
both tables; a machine that answers a different key, or none, is not recorded at all -
peerhood is having this machine's credential, and the key itself is never written into
the table. Each page's header carries a pick of the fleet when the table has anyone
standing: choosing a machine opens this same room at that address, carrying the
credential once through the address, which the room then lets go of. A machine whose
server is too young to keep a table shows no pick and asks no doors for one.

## Performance

Measured on the machine this repository stands on: AMD Ryzen AI 9 HX 470,
Radeon 890M iGPU on the AMD Vulkan driver, 96 GiB of unified memory, nothing
else running. Every row is one whole job seen by the server - the model
unloaded first, so the row carries its own load from NVMe - at the card's own
default size, steps, sampler and scheduler, batch 1, fixed seed, a clean
memory pool before every row. Times are the engine's own wall clock.

### Images

| model | card default | cpu | vulkan | cuda |
|---|---|---|---|---|
| sdxl-turbo | 512x512 · 4 steps | | 11 s | |
| realvisxl-v5-lightning | 1024x1024 · 5 steps | | 48 s | |
| pony-diffusion-v6 | 1024x1024 · 25 steps | | 125 s | |
| cyberrealistic-xl | 1024x1024 · 28 steps | | 174 s | |
| animagine-xl-3.1 | 1024x1024 · 28 steps | | 197 s | |
| lustify-sdxl | 832x1152 · 30 steps | 4859 s | 152 s | |
| juggernaut-xl-v9 | 832x1216 · 35 steps | | 204 s | |
| flux-3-klein | 1024x1024 · 16 steps | | 312 s | |
| z-image-turbo | 1024x1024 · 8 steps | | 241 s † | |
| qwen-image-2.1-turbo | 1024x1024 · 8 steps | | 912 s † | |
| qwen-image-2.1 | 1024x1024 · 25 steps | | 1306 s ‡ | |

† The three clip DiT cards run their vae decode tiled - the way their cards
say, since the untiled conv routes this driver answers slowly. A run may
still name `tiling` itself and overrule the card.

‡ 1306 s, cold and alone - about 50 s a step for a 20B model on an 89 GB/s
pipe, plus a 44 s tiled decode. The slowest still image this machine draws,
and it draws it honestly.

z-image-turbo, qwen-image-2.1 and qwen-image-2.1-turbo come as int8-convrot
weights: their whole denoiser math rides INT8 GEMM, which the engine's CUDA
backend answers only from compute 7.5 up (its ggml gates on Turing) - the
rows above ran them over the vulkan road, which carries its own convrot
shaders. On a CUDA card below that floor the engine would place every matmul
on the cpu and an eight-step picture would crawl with the card idle; the
room now offers these models in every precision their source publishes, and
a card below the floor takes the form it can carry as soon as one stands on
disk. When only an uncarryable form is there, the open names the pairing and
refuses before the engine sees it - the weights room card reads *beyond this
card*, and the answer names the form to fetch.

cyberrealistic-xl costs more per step than its XL siblings because its card
runs the dpm++2m_sde_bt ancestral sampler, the most expensive sampler in the
house on this driver.

### Video

| model | workload | cpu | vulkan | cuda |
|---|---|---|---|---|
| minimax-h3 | 960x544 · 24 frames · 8 steps | | 23 min | |
| minimax-h3 | 960x544 · 124 frames · 8 steps | | 124 min | |

A 31 GB audio-video DiT on an 89 GB/s memory pipe is as slow as physics says.
At 124 frames the driver will not hand over the 5.4 GB pinned buffer the
graph asks for; ggml says so and splits the graph between cpu and card, and
the clip finishes.

### The columns that stand open

The cpu column carries only what finished on this machine: sdxl-turbo
measured 33-50 s at its card default on an idle machine (156-221 s when a DiT
decode was eating the memory bus beside it), lustify-sdxl 4859 s.
`run.bat nodevice` builds the cpu shape, and the measuring harness stands for
whatever pass fills the rest. The cuda column is a different machine's table:
`run.bat cuda` builds this repository in that shape and the same numbers are
waiting for it.

The machine runs at its hardware ceiling for these models: both memory
channels are full (2 x 48 GB DDR5-5600) and the 89 GB/s they give is the wall
every row above leans against. The one lever still on the table is the SoC's
power window, shipped at the 28 W cTDP default with room in the bios to 54 W.

How to reproduce: build the wanted shape with `run.bat` (or
`run.bat nodevice`), start the server, and send each card's defaults through
`POST /api/generate` with the prompt from `GET
/api/models`. `GET /api/timings` keeps what this machine measured - every
cost row filed under the device that did it, in that device's own name - and
the job records carry the same numbers the table was cut from.
