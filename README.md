# open-local-services

A local server for image generation, video clips, speech synthesis, transcription, and listening tasks. One server process serves the web UI and all API endpoints from one fixed port. The voice engine is linked into the server. The image engine is served through its own library because it has a separate ggml copy.

## Quick start

```sh
run.bat                 # Windows development build and start
run.sh                  # Linux development build and start
build-rel.bat           # Windows release artifacts
build-rel.sh            # Linux release artifacts
```

The development scripts build missing parts, then start the server. Pass `build` to stop before starting. `run.bat` accepts `vulkan`, `cuda`, `nodevice`, `key=`, `rate=`, `arch=`, and `fresh-full-rebuild`. With no shape word it reads the machine: a CUDA toolkit and a named card build the CUDA shape, a Vulkan SDK alone builds the Vulkan shape, and neither at all builds with no device backend. `run.sh` accepts `build`, `key=`, `rate=`, and `fresh-full-rebuild`.

Release builds require committed `api/` and `webui/` changes. They write finished binaries into `Release/<target>/` with `build-info.txt`.

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

- `GET /` returns the credential door.
- `/index.html` is the home page.
- `/imagegen.html` is the image studio.
- `/voice.html` is the voice studio.
- `POST /api/webui/expand` writes embedded pages to disk.

## API

Common endpoints:

```text
GET  /api/monitor
GET  /api/system
GET  /api/service
GET  /api/jobs
GET  /api/jobs/<id>
POST /api/jobs/<id>/cancel
```

Image, gallery, reference, and preference features use `/api/` paths. Voice features use `/voice/api/` paths. Generated files use `/files/` and `/voice/files/` paths.
Job endpoints return `202` with `job_id`; jobs are checked with `GET /api/jobs/<id>` or `GET /voice/api/jobs/<id>`.

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

## Performance

Measured on the machine this repository stands on: AMD Ryzen AI 9 HX 470,
Radeon 890M iGPU on the AMD Vulkan driver, 128 GiB of unified memory, nothing
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
say, since the untiled conv routes this driver answers slowly: z-image-turbo
took 725 s untiled against 241 s tiled, and qwen-image-2.1-turbo had never
come back at 35 minutes against 912 s tiled. A run may still name `tiling`
itself and overrule the card.

‡ The 25-step qwen-image-2.1 untiled sat in decode past 105 minutes and was
ended by hand; tiled it completes whole: 1306 s cold and alone - about 50 s
a step for a 20B model on an 89 GB/s pipe, plus a 44 s tiled decode. The
slowest still image this machine draws, and it draws it honestly.

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
graph asks for, ggml says so, splits the graph between cpu and card, and the
clip finishes - which it does now; while the weights were streamed instead of
resident, that same clip ended the server.

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
decode was eating the memory bus beside it), lustify-sdxl 4859 s. The pass
that would fill the rest was stopped by its owner; `run.bat nodevice` builds
it and the harness is kept. The cuda column is a different machine's table:
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
