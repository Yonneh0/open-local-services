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

A local server for image generation, video clips, speech synthesis, transcription, and listening tasks. One server process serves the web UI and all API endpoints from one fixed port. The voice engine is linked into the server. The image engine is served through its own library because it has a separate ggml copy.

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

The development scripts build missing parts, then start the server. Pass `build` to stop before starting. `run.bat` accepts `vulkan`, `cuda`, `nodevice`, `key=`, `rate=`, `arch=`, and `fresh-full-rebuild`. With no shape word it reads the machine: a CUDA toolkit and a named card build the CUDA shape, a Vulkan SDK alone builds the Vulkan shape, and neither at all builds with no device backend. `run.sh` accepts `build`, `key=`, `rate=`, and `fresh-full-rebuild`.

## How a release works

There is no release server here, no pipeline, and no version numbers — that is the whole design. **The version is the commit hash**, and the code it names is what gets stamped into the binary; nothing else publishes or deploys anything.

1. **Commit first.** A release build refuses an uncommitted `api/` or `webui/` — a binary stamped with an older commit than the code inside it would lie about what it is.
2. **Build the shapes.** `build-rel.bat` builds the Windows targets, `build-rel.sh` the Linux ones. Each target is one machine's shape, stated at its configure rather than left to the machine that compiled it: the CPU shapes (up to AVX-512, or plain AVX2 — both carrying the Vulkan backend) and one CUDA binary per card generation (`sm_70`, `sm_75`, `sm_86`). Every finished binary carries the commit inside it, and a CUDA binary's card generation is read back off the artifact rather than trusted from what was asked.
3. **Each finished folder ships as one archive** — `Release/open-local-services-<commit>-win-<target>.zip`, or `…-linux-<target>.tar.gz` — and its sha256 goes into that platform's manifest, `Release/SHA256SUMS-win.txt` or `SHA256SUMS-linux.txt`: one release is built on the machines whose shapes it ships, and two builders keeping one file would erase each other. `build-info.txt` names the commit, the toolchain, and the pinned engine sources everything was built from. The archives are what carry between machines, and the small ones (the Vulkan and CPU shapes, some 50 MB) are committed — here the repository is the release, and what is committed is what runs. The CUDA archives are the measured exception: cuBLAS, cuBLASLt and cuFFT make them roughly 850 MB compressed, past what any GitHub push admits in one file, so they live beside the tree under their committed sha256 and move box to box over the link. Build scripts never commit anything themselves; they name any archive git has not been shown yet.
4. **A machine that compiles nothing opens one archive and runs it.** Inside: the server with the voice engine compiled in, the image engine beside it, the phonemizer data, and a `webui/` folder served live the way the development tree's is. CUDA artifacts carry the CUDA runtime their binaries really import, and Windows artifacts the Microsoft C++ runtime, read off their import tables rather than guessed at. A `weights/` folder, a `data/` folder, and a `.env` beside the binary are the same folders and file the development tree uses.
5. **Built for one place, run anywhere of its kind.** The CPU part of every binary is baseline-plus-runtime-detection, so an artifact built on an AVX-512 machine runs untouched on one that has never seen it. Linux artifacts state the newest glibc symbol they ask for, and are built on the same Ubuntu release as the machines that will run them.

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
```

The config panel uses `GET /api/config` and `POST /api/config`. Secret values are shown only as `set` or `unset`.

## Web UI

Pages are embedded in the binary at build time. If a `webui/` folder exists beside the running server, it is served from disk instead.

- `GET /` returns the credential door: hand over the key once and the room opens.
- `/index.html` is the home page: cpu, gpu, and memory levels with their recent history, the hardware each engine stands on, the models open in memory, every job standing or running and what it says about itself, the network panel, local settings, and the service door. The corner clock tells the hour - or, while work stands in the queue, how far the queue is away, counted from the machine's own statements and never guessed by the page.
- `/imagegen.html` is the image studio: prompt and settings beside the gallery of what this machine drew, reference images for image-to-image, the weights room with its downloads and loads, a queue that answers cancelling and reordering, and preferences the page remembers. Still images and video clips are pressed through the same door.
- `/voice.html` is the voice studio: speak with any opened model and voice, save, rename, and delete voices, hold a text library, transcribe recordings, and read what the engine keeps open and what it costs.
- `POST /api/webui/expand` writes the binary's embedded pages out beside it, so a binary install can become an editable tree.

## API

Job doors answer `202` with a `job_id`; the work is watched at the job doors, and what it made is served from `/files/` and `/voice/files/`. Every door below answers under the same key and rate limit.

The machine and the service:

```text
GET  /api/system             the machine: devices, memory pools, load, the dates of the pages
                             and of the binary
GET  /api/monitor            one merged reading: system levels, engine state, and the queue
                             block the corner clock counts
GET  /api/service            how this server is installed and run; POST acts with
                             startup_on, startup_off, remove, restart, or shutdown
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

Voice:

```text
GET  /voice/api/status  /voice/api/health   the voice engine's own statement
GET  /voice/api/models       the voice models; /voice/api/model-cards and
                             /voice/api/model-stats read their cards and their costs;
                             /voice/api/models/<name> works one model
GET  /voice/api/jobs         voice jobs; /voice/api/jobs/<id> for one of them
POST /voice/api/speak        text to speech, with a named model and voice
POST /voice/api/listen       a recording to text
GET  /voice/api/voices       the saved voices; POST voices/save, voices/delete,
                             voices/rename
GET  /voice/api/library      the held text library; POST library/delete forgets one
```

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

### Speech

| model | task | cpu | vulkan | cuda |
|---|---|---|---|---|
| kokoro-82m-q8 (af_heart) | speak, five varied prompts | | first 15.5 s cold, then 1.4-1.9 s | |
| qwen3-tts-customvoice-1.7b-q8 (serena) | speak, the same five | | first 34.9 s cold, then 9.3-12.0 s | |
| moonshine-streaming-small-q8 | listen, 8.4 s recording | | 2.2 s | |

The speech rows ran on the same server; the speech engines choose their own
device, and these three numbers are the same model cold and warm - the cold
first call carries the engine's own start, the warm ones are what a page
conversation feels.

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
`POST /api/generate` / `/voice/api/speak` with the prompt from `GET
/api/models`. `GET /api/timings` keeps what this machine measured - every
cost row filed under the device that did it, in that device's own name - and
the job records carry the same numbers the table was cut from.
