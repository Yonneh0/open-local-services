# spec_image_studio.md — Image Studio

The picture side of this machine: the models it knows, the weights it holds, the runs it
makes, and every door a page or a program walks through to ask for any of it.

This file states the image module whole — its tables, its jobs, its files on disk, and its
doors. The law shared by every module (transport, credential, rate gate, job law, cache
law, the file doors, the pages) lives in **spec.md** and is not restated here; where this
file needs it, it points there. The project
side is **spec_project_studio.md**. Where code and this file disagree, one of them is
wrong.

---

## 0. Shape and related files

One engine stands behind this module: the vendored **stable-diffusion.cpp** library,
linked into the same process as everything else (see spec.md §0). There is no second
executable, no python, no subprocess for drawing — the library answers in-process, and
what it hands back (pixels, frames, sound) this machine writes to disk itself.

| File | What stands in it |
| --- | --- |
| `src/ImageService.h` | The module's whole public surface: the constructor and every hand the doors call. |
| `src/ImageService.cpp` | Everything else: the model tables, the weights room law, the open/line/card law, the run, the clip writers, the gallery, the refs, the timings. |
| `src/OpenLocalServices.cpp` | The doors. Every `/api/*` image route lives in `route()` there and is a thin read/answer over this module. |
| `src/VerifiedRoom.h/.cpp` | The weights room's records — which files stand proved (spec.md §12 law, applied here). |
| `src/Common.h` | `makeId`, `kKeptJobs`, `deviceCard()`, the atomic file writers, the sha readers. |
| `src/Platform.h` | `curlFetch` / `quotableInShell` — the only road this module takes to the internet. |
| `webui/app.js` | The image studio page: models, the ask, the line, the gallery, the reference strip. |
| `webui/index.html`, `webui/style.css`, `webui/common.js` | The shell the page hangs in (spec.md §13). |

### The doors at a glance

Every door below is behind the credential and the rate gate like everything else
(spec.md §2, §3), and answers with the cache stamps of spec.md §7.

| Door | Method | What it opens |
| --- | --- | --- |
| `/api/models` | GET | Every model this machine knows, with what is on disk. |
| `/api/generate` | POST | A run: a picture or a clip. Answers 202 with a job id. |
| `/api/models/<id>/download` | POST | Fetch a model's weights. Answers 202 with a job id. |
| `/api/models/<id>/load` | POST | Open a model's weights. Answers 202 with a job id. |
| `/api/unload` | POST | Let one model go. |
| `/api/jobs` | GET | Every job this engine keeps, newest first, with its place in the line. |
| `/api/jobs/<id>` | GET | One job's whole record. |
| `/api/jobs/<id>/cancel` | POST | Stop a standing job. |
| `/api/jobs/<id>/events` | GET | The job's event stream (SSE) — spec.md §6. |
| `/api/jobs/reorder` | POST | Move a queued run inside the line. |
| `/api/timings` | GET | What this machine's own runs cost. |
| `/api/gallery` | GET | The room of finished work, newest first, one window. |
| `/api/gallery/<id>` | GET | One record by its id. |
| `/api/gallery/<id>` | DELETE | Take one output and its parts off the disk. |
| `/api/refs` | GET | The kept pictures. |
| `/api/refs` | POST | Keep a picture (from the gallery, or from the browser's own bytes). |
| `/api/refs/<id>` | DELETE | Drop a kept picture. |
| `/api/prefs` | GET | The switches the page left standing. |
| `/api/prefs` | POST | Turn the switches. |
| `/files/outputs/<name>` | GET | A made thing and its parts (spec.md §9). |
| `/files/refs/<name>` | GET | A kept picture. |
| `/files/weights/<name>` | GET | A weights file, by name. |

---

## 1. The model tables

What this machine can draw stands in three hand-written tables beside one row list, all in
`ImageService.cpp`. A model is **known** because it is in the row list — nothing is
discovered from disk, and nothing is offered that the tables do not name.

### 1.1 The row list (`kModelList`)

One row per model: `id`, `name`, `repo`, `steps`, `guidance`, a demo `prompt`, a shipped
`negative`, optional `sigmas`, an optional `civitaiVersion`, the `files` the model is made
of, and an optional `wtype`. The rows as they stand:

| id | name | repo | steps | guidance | files (slot → path) |
| --- | --- | --- | --- | --- | --- |
| `flux-3-klein` | FLUX.3 Klein | (its card's) | 16 | 0 | `diffusion_model` model.safetensors, `vae` ae.safetensors, `llm` hf:Qwen/Qwen3-VL-4B-Instruct-GGUF/Qwen3VL-4B-Instruct-Q4_K_M.gguf; wtype q8_0 |
| `z-image-turbo` | Z-Image Turbo | Comfy-Org/z_image_turbo | 8 | 1 | `diffusion_model` int8_convrot, `vae` ae, `llm` hf:unsloth/Qwen3-4B-Instruct-2507-GGUF Q4_K_M |
| `qwen-image-2.1` | Qwen Image 2.1 | Comfy-Org/Qwen-Image-2.1 | 25 | 6 | `diffusion_model`, `vae`, `llm` (int8_convrot set) |
| `qwen-image-2.1-turbo` | Qwen Image 2.1 Turbo | Comfy-Org/Qwen-Image-2.1 | 8 | 1 | the same set, turbo denoiser, own sigmas |
| `sdxl-turbo` | SDXL Turbo | stabilityai/sdxl-turbo | 4 | 0 | `model` sd_xl_turbo_1.0_fp16.safetensors |
| `realvisxl-v5-lightning` | RealVisXL V5 Lightning | SG161222/RealVisXL_V5.0_Lightning | 5 | 1.5 | `model` fp16 safetensors, shipped negative prompt |
| `pony-diffusion-v6` | Pony Diffusion V6 XL | LyliaEngine/Pony_Diffusion_V6_XL | 25 | 7 | `model` + `vae` sdxl_vae |
| `cyberrealistic-xl` | CyberRealistic XL | its card's | 28 | 7.5 | `model` |
| `animagine-xl-3.1` | Animagine XL 3.1 | its card's | 25 | 6 | `model` |
| `lustify-sdxl` | Lustify NSFW Checkpoint | civitaiVersion 638929 | 30 | 5 | `model` |
| `juggernaut-xl-v9` | Juggernaut XL v9 | its card's | 30 | 7 | `model` |
| `minimax-h3` | MiniMax H3 | unsloth/MiniMax-H3-GGUF + Comfy-Org/MiniMax-H3 | (video) | — | `diffusion_model` fl2va_pruned-Q4_K.gguf, `llm` qwen3vl_32b Q4_K_M, `vae` video vae, `audio_vae` audio vae |

A file path may carry the `hf:<repo>/<path>` spelling — the repo the file comes from
beside the row's own, and the name it stands under locally. A path naming `-00001-of-`
is a shard set: the whole family of pieces is read from the repo's tree listing
(§ 4.2) and every piece must stand before the model opens.

### 1.2 Specialty (`kSpecialty`)

One line per model, stated by the card, saying what the model is good at — "photoreal
scenes, diagrams, and crisp rendered words in a small fast package" for FLUX.3 Klein,
"moving pictures with their own stereo sound, from words and from frames" for MiniMax H3.
The words a run asks with are typed on the page; nothing about scenes travels with the
model table.

### 1.3 Capability (`kCapability`)

How a model uses pictures, and how many it takes:

| id | kind | refs |
| --- | --- | --- |
| `flux-3-klein`, `qwen-image-2.1`, `qwen-image-2.1-turbo` | edit | 10 |
| `minimax-h3` | edit | 2 (first frame, last frame) |
| every other model | redraw | 1 |

An **editing** model reads each picture handed to it and keeps them apart by position. A
**redraw** model takes one picture and changes it, followed by `strength` (default 0.6,
clamped 0..1).

### 1.4 Tuning (`kTuning`)

What a model's own card and its own weight files say about running it, in the engine's
words: the size it was trained at, the sampler and scheduler its release names, whether a
step cache has room in the steps it asks for, and (trailing) `clip_skip`, `flow_shift`,
and for a moving model the `video` flag with the `frames` and `fps` its card states it
runs at home at. One row per model; a model with no row keeps the engine's own choice for
its family. The rows as they stand:

| id | size | sampler | scheduler | cache |
| --- | --- | --- | --- | --- |
| `sdxl-turbo` | 512² | euler_a | sgm_uniform | off |
| `flux-3-klein` | 1024² | euler | — | off |
| `z-image-turbo` | 1024² | — | — | off |
| `qwen-image-2.1` / `-turbo` | 1024² | euler | — | off |
| `realvisxl-v5-lightning` | 1024² | dpm++2m_sde | karras | off |
| `pony-diffusion-v6` | 1024² | euler_a | — | on |
| `cyberrealistic-xl` | 1024² | dpm++2m_sde_bt | karras | on |
| `animagine-xl-3.1` | 1024² | euler_a | — | on |
| `lustify-sdxl` | 832×1152 | dpm++2m_sde | karras | on |
| `juggernaut-xl-v9` | 832×1216 | dpm++2m | karras | on |
| `minimax-h3` | 960×544 | — | — | off; flow_shift 12.0; video, 124 frames at 24 fps |

The step cache is the engine's UCache path: it starts at fifteen percent of a run's steps
and stops at ninety, so a distilled model trained for four or five steps names the cache
off — the window would be offered the whole sampling.

---

## 2. Forms: what a model can be taken as

A **form** (`ModelForm`) is the same model with some of its parts stood under other
files — a set of slot substitutions. Most models come in rungs only for the transformer
("the autoencoder and the text encoder keep their own files, since a quantized package
states it does not re-quantize them"); a model whose parts are separate checkpoints can be
offered in forms that move either of them.

The offers as they stand (`kQuantOffers`):

- **flux-3-klein** — ten rungs of the transformer from
  `hf:unsloth/flux-3-image-klein-GGUF/flux-3-image-klein-<rung>.gguf`:
  `Q3_K_M, Q4_K_M, Q5_K_M, Q6_K, Q8_0, UD-Q2_K_XL, UD-Q3_K_XL, UD-Q4_K_XL, UD-Q5_K_XL, BF16`.
  Default: **Q8_0**.
- **minimax-h3** — six denoiser rungs from
  `hf:unsloth/MiniMax-H3-GGUF/minimax_h3_fl2va_pruned-<rung>.gguf`:
  `Q2_K, Q3_K, Q4_K, Q5_0, Q6_K, Q8_0`. The two smallest rungs pair with the
  `qwen3vl_32b_minimax_h3-Q2_K_M` encoder; everything above pairs with the row's Q4_K_M
  encoder. Two further forms replace the **encoder** instead: `Sakura-Q4_K_M` and
  `Sakura-Q8_0` from `hf:webmp3/Sakura-MiniMax-H3-Uncensored-GGUF/…` — an abliterated
  text encoder, not-for-all-audiences, which is said on the card rather than hidden here.
  Default: **Q4_K**.

The form a model is taken as (`formFor`), in order: what a person last asked for (kept in
`weights/.chosen-quants.json`, a machine-wide fact that outlives a restart) → the base
package if its files all stand proved here → the first offered form whose files all stand
proved here → the default above. Nothing is invented: a form with no files behind it is
never chosen over files that are here. A null answer means the model's own files,
unchanged.

The name a page marks as in use (`uses_quant`) is the form this machine would open with —
and where the row's own files *are* one of the offered forms (H3's row stands a Q4_K
denoiser), that form's name is said, so a page never reads "no form in use" about bytes
that are one of the named forms.

---

## 3. The weights room and its records

The weights live under `OLS_WEIGHTS_DIR` (default `data/weights`), one folder per model
(`weights/<model-id>/`). What stands proved about their bytes is the room's own record —
`VerifiedRoom` (spec.md §12): `proved()` reads nothing but the records file plus the
file's length and date; `prove()` reads a file once (whole sha and per-chunk sums against
the card's stated sum); `vouch()` stands the `whole` voucher after the fact when a card
states no sum. **A weight is read once on a machine, and never again while its bytes
stand** — nothing a page asks costs a reading of a hundred gigabytes.

When a card states no sum, the file's own header says whether it is whole, in whichever
dialect it keeps:

- **safetensors** (`safetensorsComplete`): the eight bytes that open the file count its
  json header; the furthest `data_offsets` end the header names must end inside the file.
  A figure past any conceivable file (`1<<48`) is refused before it is added to anything.
- **GGUF** (`ggufComplete`): magic, version, counts, metadata key-values (four-byte type
  tags, thirteen type widths, arrays walked), then tensor records; the data starts at the
  header's end padded to the alignment the file names (32 when it names none), and the
  furthest tensor offset must end inside the file. Little-endian on every platform; a
  host that reads the other way answers honestly that nothing was stated.
- **Neither dialect** (a config, a tokenizer): its length is what it can be asked, and
  that was asked already.

`integrityOk` asks the size the card states, then the sum (lower-cased, `sha256:` prefix
stripped), then the header. A file that came down and does not answer is refused:
"the file that came down does not match the sum its card states" / "the file that came
down is not whole".

Beside the room's records, the module keeps its own small files in the weights room:

| File | What stands in it |
| --- | --- |
| `.chosen-quants.json` | Which precision of a model a person last asked for. |
| `.ui-prefs.json` | The page's switches (§ 10). |
| `.run-timings.json` | The timings ledger (§ 11). |
| `.hf-tree-<repo>.json` | A repo's file listing, kept beside the weights it describes. |

---

## 4. Fetching weights

### 4.1 Where a file comes from

`downloadUrl`: a row with a `civitaiVersion` comes from
`https://civitai.com/api/download/models/<version>`; an `hf:`-spelled file from
`https://huggingface.co/<repo>/resolve/main/<path>`; anything else from the row's repo the
same way. The fetch rides `ols::platform::curlFetch` (spec.md §11) with the key that
belongs to the address: `CIVITAI_API_KEY` for civitai.com, `HF_TOKEN` for huggingface.

### 4.2 Listings and shards

A repo's tree is fetched once (`https://huggingface.co/api/models/<repo>/tree/main?recursive=true`)
and kept in `.hf-tree-<repo>.json`; a listing that stopped halfway is taken away rather
than left to answer wrong. A slot naming `-00001-of-` expands through that listing to
every piece of its family (`.safetensors` and `.gguf` sets are both split the same way),
sorted, and every piece is fetched and proved.

### 4.3 Rounds

A file comes down in rounds of `kFetchSliceSeconds` = 20 seconds each, every round
resuming from the bytes already down — so a stop is believed inside twenty seconds and a
bar has something to read while thirty gigabytes arrive. Five rounds that carry nothing
give the file up on. What arrived stays where it arrived: the next round — today or after
a restart — continues from that byte. Each round stands its numbers on the job's record
(`bytes`, `total_bytes`, `speed`, `avg`, `file`) and pushes a `fetch` event.

### 4.4 The download job

`POST /api/models/<id>/download` (body may name `quant`). The job walks the model's files
as the chosen form stands them; a file that already stands proved is passed over without
being read. Sizes and sums for a file not yet here are read from the repo's listing (the
`lfs` block, when it ships one, states both and wins). A stopped fetch says so and where
it stopped: "the fetch was stopped; what is down stands where it is and the next fetch
continues from that byte".

---

## 5. Opening a model

`openContext` opens a model's weights or takes the one already standing. It is the one
place the engine is asked for a context, and the law it keeps:

- **The form is resolved once, out here** — the ask's chosen quant, else what the files
  standing here answer with (§ 2). Every file of every slot must be here before the engine
  is asked for anything; the engine is handed the first of a shard set, which is the one
  it reads the rest from.
- **Slots** the tables may name: `model`, `clip_l`, `clip_g`, `t5xxl`, `diffusion_model`,
  `vae`, `llm`, `llm_vision`, `audio_vae`. A table naming a part the engine has no door
  for logs "names a part this machine cannot hand on" rather than loading short and never
  saying so.
- **The device**: the card of this machine, named in the engine's words (hip/vulkan
  preferred over others), or `cpu` when the run asks for it or no card answers. mmap on;
  threads from `OLS_THREADS`. MiniMax H3 takes its noise from the cpu (`CPU_RNG`), as both
  of its pages name. Flash-attention is turned off for cards under compute 7.5 — the
  engine's kernel has no device code for them and aborts the whole process; the rule is
  held to the cards the abort was ever seen on, and the card is asked once.
- **The vram budget** the engine cuts its graphs against: the card's GiB minus four, floor
  four, and six for an 8 GiB card (measured: the manager counts its runtime buffers, and
  four GiB starved a twelve-frame clip while the card stood 6.6 GiB free).
- **Standing weights move** when the ask names another device or another form than the
  context that stands — a run is never answered with a device it did not ask for.
- **Room is made after the new weights stand, never before**, and asked for on reuse too:
  idle models close least-recently-used first until the resident weights fit three
  quarters of the card's heap (floor: ¾ of 24 GiB when no card answers). A model that is
  drawing is never the victim.
- **When the card refuses**: close the LRU idle model and try again; if nothing else
  stands in the way and this is a video model, ask once with the engine's streaming recipe
  (`params_backend "*=cpu"` — weights on the cpu, the card holding the working set).
  Still refused, the words are "No room for X on this machine: closing the loaded models
  did not free enough" or "This machine cannot hold X even with nothing else loaded".
- **The engine's own last complaint** is kept from its log callback; a failed run speaks
  it (§ 8).

Every open, close, and draw stands on `ols::deviceCard()` — the one global mutex
every engine in this binary shares (spec.md §12): the device does not know
which module asked.

---

## 6. The card and the line

One render works on the card at a time, whatever model it stands for. The order a
generate job keeps in `gJobOrder` is the line it stands in: whoever reaches the head of
the standing generate jobs while the card is free is the one that goes
(`waitForTurn`), and the job holds the card the moment it sees its turn — before it lets
the lock go — so two waiters never cross the head together. A stop asked while standing
is believed there: the engine is never started for a picture nobody wants.

`POST /api/jobs/reorder` `{id, shift}` moves a **queued generate** job inside the line —
it trades places with the job `shift` steps away. Anything else (not queued, not a
generate, moving past either end) is refused at the door: "that job does not stand in the
line, or cannot move that far".

`GET /api/jobs` stamps each generate job with `queue_pos`: **0** stands on the card, the
queued ones are numbered **1..** in the order they will go, whether or not anyone is on
the card this instant.

---

## 7. The run

`POST /api/generate` takes a json body and answers 202 with `{"job_id": "img-…"}`. The
body's fields, and what they mean:

| Field | Default | Meaning |
| --- | --- | --- |
| `model` | — | Which model to draw with (required; may ride the door's `<id>`). |
| `prompt` | "a quiet street at dusk" | The words. |
| `negative_prompt` | the model's shipped negative | What to keep out. |
| `width` / `height` | the model's card size, else 768 | The sides. 64..4096. |
| `steps` | the model's card | 1..150. |
| `guidance` | the model's card | 0..100, finite. 0 is answered as 1.0 — the same sentence in the engine's words for a distilled model. |
| `seed` | -1 | -1 draws a fresh one. |
| `count` | 1 | Pictures in one run, clamped to 16. A video model is always 1. |
| `frames` | the model's card | A clip's length, 5..243 (the engine keeps counts on its 17n+5 grid; the ceiling is the last grid point under it). |
| `fps` | — | Recorded; the engine keeps the rate its weights were trained at and reports the one it encoded with. |
| `strength` | 0.6 (redraw) / engine's own (video) | How far an opening picture is followed. |
| `references` / `reference` | — | Names of kept pictures or paths, in order (§ 9). |
| `tiling` | the page's switch | The vae road. Forced on for `z-image-turbo`, `qwen-image-2.1`, `qwen-image-2.1-turbo` unless the run names `false` — their untiled decode crawls on this driver (725 s against 241 tiled). |
| `cache` | true (where the card allows) | The step cache; a run may name `false` to compare the same generation with and without it. |
| `sage_attn` / `cpu` | the page's switches | Which part of the machine works, and the sage-attention switch. |
| `quant` | the held choice | The form to open as; a name the model is not offered as is refused at the door with the names it can be opened as. |

The bounds are named once and the refusal words are built from the same numbers: "the
side asked for is outside 64..4096", "the steps asked for are outside 1..150", "the
guidance asked for is outside 0..100", "the clip asked for is outside 5..243 frames".

**The card's window for a clip.** A card holds only so many picture-frames at once — the
maker's own recipe runs 56 frames of 864×480 on a 24 GiB card, so that many scaled by the
card's size is the window (two thirds of it when the clip starts from a picture — the
picture's features cost more per frame). An ask past the window bends: without a source
picture, the length comes down (to a multiple of four, floor five); with one, the
**picture bends before the length** — a five-frame clip reads as a still no matter what
the model drew, so the sides shrink (floor 256, quarter grid kept) until twenty-two whole
moved-from frames fit, and the length follows the grid. What was asked and what stands
are both kept in the record (`frames_asked` beside `frames`), and the log says the bend
out loud.

**The sampler and scheduler** the card names are asked in the engine's words; the
engine's choice for the family stands where the card names nothing, and a name the engine
does not know is kept as a note — "the card names a sampler the engine does not know (X);
the engine's own choice stands" — rather than a failure. The scheduler's default is asked
*after* the solver is settled, because the grid a solver walks belongs to the solver. The
solver and grid this run really sampled with are kept beside it, so a run's cost is filed
under the solver that did the work.

**Progress.** The engine's one hook hears every phase, and every phase climbs a ladder of
its own — weights count tensors, tiles count tile passes. Only the ladder whose total
reads the number of steps the ask named, seen from its first step, is the sampling; every
other ladder never teaches the page. The bar holds at zero rather than lying about a step
it never took, and moves forward only — except the engine's own zero tick, which lifts
the bar off whatever the loading left it at. A batch's window is `steps × pictures`.

**Stopping.** A stop asked after a run has drawn pictures of its batch is believed from
the start of the next picture, so what came out stays; a stop before the first picture is
believed at once. A stop asked while the job waited behind the run before it is believed
before the engine starts at all.

**When the engine says no**, the job carries what the engine last complained of: "the
picture did not come out: <the engine's words>" (or "the clip did not come out").

---

## 8. What a run leaves on disk

### 8.1 Pictures

`outputs/gen-<job>-<n>.png`, each with its record `gen-<job>-<n>.png.json`:
`id, file, kind:"image", created, model, name, width, height, prompt, steps, seed,
references, quant, device, machine, vram_bytes, sampler, scheduler, tiling, ms, open_ms`.
The words are the same readings the job record carries, so a file and its record can
never tell two different stories about one run. Each picture lands as an `image` event
with its `/files/outputs/…` url.

### 8.2 Clips

One video run stands as five files, named from the clip (`gen-<job>-0`), each the thing a
different hand needs:

| File | What it is |
| --- | --- |
| `.avi` | The clip: MJPEG frames (JPEG quality capped at 90 — above it the encoder's sampling breaks MJPG playback) with the model's stereo sound muxed in. What a person saves. |
| `.raw.avi` | The same frames with no codec to know — raw bottom-up rows. Only stood under 48 MiB, where a person can still carry the file. |
| `.poster.png` | The first frame, so a room of clips can be named by its pictures. |
| `.preview.apng` | A small animated copy — 256 side, 8 fps, at most 32 frames spread across the whole clip — that a browser draws as an image. Stored deflate blocks, no compressor in this machine; fcTL/fdAT numbered in one ascending sequence with no gaps. |
| `.wav` | The clip's sound on its own, PCM 16-bit at the rate the engine carried it. Browsers play WAV natively. |

The parts are **not outputs of their own**: the gallery never lists them, and every
removal that drops a clip drops them with it. A clip's audio is part of what the model
made, not something added afterwards, so the track is written rather than dropped, and a
clip that came without sound says so in its record instead of standing as a silent one.

The clip record adds `frames, motion, fps, duration_ms, poster, preview, audio, raw,
channels, sample_rate, frames_asked` to the picture's fields. **motion** is the one
number that says whether a clip moves — the mean change of a picture between the frames
it stands between, read off the pixels the run made. Measured: clips that move stand at
three to twelve of 255 between neighbours; a still sits at zero.

The AVI is written by this machine, byte for byte, following the vendored engine's own
example writer's layout; every size field is read off the bytes that were built, so a
field count cannot drift from the size a reader is told.

### 8.3 The room's depth

The newest `gKeptMax` outputs stay (default 10 000, set by `OLS_GALLERY_MAX`); the room is
trimmed once it stands 128 past its ceiling, oldest slack off in one pass. A job's
exception on its own thread becomes the job's error state — "the run stopped without
words" — never the process's end.

---

## 9. Kept pictures (refs)

A working set of pictures a later run can start from, in `data/refs` (the folder beside
the outputs). The newest **24** stay.

- `GET /api/refs` → `{items: [{id, file, name, created, source, url}]}`, newest first.
- `POST /api/refs` takes either `{source, name}` — a gallery id, a clip id, or any image
  path on this machine — or `{name, image_b64, format}` for a picture the browser is
  holding (a dragged file, a pasted screenshot; the studio shrinks it before sending).
  A kept picture comes as a png, a jpg or a webp, and the bytes are capped at 2 MB.
  Keeping a **clip** keeps its poster — what a model that moves can start from is the
  still, not the container — and the record names the clip it came from. A browser-kept
  picture records `source: "kept in the browser"`.
  Refusals: "what to keep was not named", "nothing by that name is here to keep",
  "a kept picture comes as a png, a jpg or a webp", "no picture came with the ask",
  "Picture exceeds the keep limit (2 MB)", and the folder/copy/record failures.
- `DELETE /api/refs/<id>` tries every ending a kept thing can stand under.

**How a run resolves a reference name** (`resolveReference`): the refs folder → the
outputs folder (`.png/.jpg/.jpeg/.webp`) → a clip's poster or preview, when the name
stands as a clip → any absolute path on this machine. So a page can name a gallery
picture, a kept picture, or a clip's still, and one answer serves every door.

---

## 10. The switches (prefs)

Kept on the server so a page reload shows the same page: `tiling`, `sage_attn`, `cpu`
(booleans, stored as "1"/"0" in `.ui-prefs.json`) and `model` (the last model chosen,
which must be one this machine knows). A run that names none of the switches takes the
switch the page left standing — a switch that only works when a run happens to name it is
a switch the page should not be asked to leave standing.

- `GET /api/prefs` → `{"tiling": bool, "sage_attn": bool, "cpu": bool, "model": str}`.
- `POST /api/prefs` takes any of them; a switch this machine does not keep, or a value
  worded no way a switch speaks, is passed over rather than answered as a failure.
  Nothing touched → the door answers the refusal.

---

## 11. What a run costs (timings)

The honest answer to "what will this draw take" is what these same settings cost here
before. Every run that finished every step of its window is filed in the ledger
(`.run-timings.json`, at most 400 rows, oldest-touched off the end) under a key of
`kind|model|settings` — the sides, steps, guidance as used, the form really opened, the
solver and grid really sampled with, the cache, the tiling road really taken, the device
in its own name, the sage switch, the batch, the reference count, and for a clip the
length. **The seed is in no key**: another seed is the same work at these settings. A run
that was cancelled or failed is not filed.

`GET /api/timings` → `{generations: [...], loads: [...], row_count}`, newest first, each
row carrying `count, mean_ms, min_ms, max_ms, last_ms, mean_ms_per_step` (the mean over
the steps the run really sampled — steps × pictures — so a batch never prices a step four
times over) and `mean_ms_per_picture`. A set of settings nobody has run is simply not in
the answer, and a page says that rather than inventing a number.

---

## 12. The gallery

`GET /api/gallery?limit=&offset=` → `{total, offset, items: [record…]}`, newest first.
The records are ordered by the moment each was written; a page that starts deep in the
room still costs one directory listing and only the reads of the window it asked for, so
a gallery of ten thousand pictures costs the same handful of file reads per page as one
of forty. `limit` is clamped to what the machine keeps; `total` is stated from the
listing, so a reader that took a shorter window can say what it left out rather than look
like it had seen everything. Every item carries its `/files/outputs/<id>` url.

`GET /api/gallery/<id>` answers one record by its id — looked up behind each ending a
record can hang under, since a page that holds an id does not say whether it named a
picture or a clip. `DELETE /api/gallery/<id>` takes the output, its record, and a clip's
parts.

A name a page gives is checked by the one rule every door that names a file uses
(`safeItemName`): no step out through `..`, no backslash, no colon, no dot at the start.

---

## 13. The module's hands (ImageService.h)

The whole public surface, and what each hand answers:

| Hand | Answers |
| --- | --- |
| `ImageService(weightsDir, outputDir, threads, hfToken, civitaiKey, keptMax)` | Stands the module up. Makes no folders itself — they come into being with the first thing written into them. |
| `modelsJson()` | `{models: [row…]}` — every known model: `id, name, repo, steps, guidance, width, height, sampler, scheduler, cache, clip_skip, flow_shift (-1 for "the family's own"), prompt, specialty, negative, sigmas, edit, redraw, refs, video, frames, fps, files, on_disk, bytes, present_bytes, unchecked, quants[{name, here, present, bytes?}], default_quant, asked_quant, uses_quant`. `on_disk` says the files stand **proved**; `present_bytes` says what physically sits in the room whether or not anything proved it; `unchecked` says some of it stands with nothing behind it. A machine holding the bytes is never called a machine without them. |
| `statusJson()` | `{loaded[], generating[], active, resident_bytes, resident[{model, bytes, device: cpu|card, quant}], budget_bytes, lru[]}`. There is no door of its own for this: the whole block rides `/api/system` as its `loaded` answer (spec.md §10), and the page reads it there. |
| `devicesJson()` | The engine's device list: `{name, description, type: cpu|gpu}`. Likewise no door of its own — it feeds the devices array of `/api/system` beside the readings of the other engines (spec.md §10). |
| `known(id)` | Whether the tables name the model. |
| `startJob(kind, modelId, payloadJson)` | `generate` \| `download` \| `load`. Refusals ride `engineError()`: "the body of the ask is not one this machine can read", "that kind of work is not one this machine runs: X", "no model was named: the picture tables list what this machine holds", "that model is not one this machine knows: X", "that model is not offered in quants: it comes as one package", "that name is not one X is offered as: it can be opened as <names>". A standing job's record carries `id, kind, model, state, started_at`, the chosen `quant`, and for a generate the `ask` — the words, settings and reference urls as they were made, so a page can draw the picture's card while its run still stands in the line. |
| `jobsListJson()` / `jobStateJson(id)` | The job tables; an unknown id answers `{id, state: "unknown"}`. |
| `cancelJob(id)` / `reorderJob(id, shift)` | The stop and the move of § 6/§ 7. |
| `streamEvents(client, jobId)` | The SSE pump (spec.md §6): frames as `data: {…}`; an unknown id gets `{"type":"error","message":"no such job: <id>"}` rather than a stream that ends without a word. |
| `timingsJson()` | § 11. |
| `galleryJson(limit, offset)` / `galleryItemJson(id)` / `removeOutput(id)` | § 12. |
| `refsJson()` / `addRef(source, name)` / `addRefBytes(name, b64, format)` / `removeRef(id)` / `lastRefError()` | § 9. |
| `prefsJson()` / `setPrefs(bodyJson)` | § 10. |
| `unload(modelId)` | Lets a model go — on the card. A model that is drawing is marked pending and closes when its run ends, never from under it. |
| `file(kind, name, path, contentType)` | The `/files/` resolver: kinds `outputs`, `refs`, `weights`; any other word names no folder. Content types: png, jpeg, webp, apng (`image/apng`), avi (`video/x-msvideo`), wav, else octet-stream. |
| `engineError()` | Why the last call **this thread** made was refused — one per thread, so two requests refused at the same moment never write over each other's words. |
| `setHfToken` / `setCivitaiKey` | The download keys, read under their own lock. |

### The job record, whole

Beyond `id/kind/model/state/started_at`: `ended_at`, `elapsed_ms`, `error` (only when the
job stopped), `notes[]` (words about a step that did not stop the job — a sampler the
engine replaced, a fetch that stopped with bytes down), `done`/`max` (progress, forward
only; a job that finished finished every step of its own count), `urls[]` and `url` (the
first), `settings` (what the run resolved to, § 11), `queue_pos` (in the list answer),
and for a download `bytes/total_bytes/speed/avg/file`.

### The event stream

Every frame is one json object: `progress {done,max}` · `image {url}` · `video {clip,
poster, preview, audio, raw, frames, fps}` · `fetch {bytes,total,speed}` · `error
{message}` · `cancelled`. The stream's framing and end are spec.md §6.

---

## 14. The webui (app.js)

The image studio page (`webui/app.js`, mounted in the shell of spec.md §13) is where a
person: picks a model from the cards (specialty, size, quants and what is on disk, all
read from `/api/models`); types the ask and turns the switches; watches the line and
reorders it; follows a run through its event stream; keeps pictures into the reference
strip; and walks the gallery — posters and previews drawn from the record's own urls.
The page shrinks a browser-held picture before sending it as a kept reference, so what
arrives fits one request body. It hands its batches in runs of its own fill, which is why
the per-run batch ceiling only binds another client that asks deeper.

---

## 15. Constants

| Name | Value | Where it bites |
| --- | --- | --- |
| `kDefaultSide` | 768 | The last-resort side when neither the ask nor the card names one. |
| sides | 64..4096 | One run's width and height. |
| steps | 1..150 | One run's sampling. |
| guidance | 0..100, finite | Refused before the sampler, not-a-number included. |
| `kMaxBatchPerRun` | 16 | Pictures in one run (video: 1). |
| `kMaxReferenceImages` | 10 | References one run may take, whatever a card allows. |
| frames | 5..243 | A clip's length (the 17n+5 grid's last point under the ceiling). |
| `kFetchSliceSeconds` | 20 | One round of an internet fetch. |
| stalls | 5 | Rounds that carry nothing before a file is given up on. |
| `kKeptRefs` | 24 | Kept pictures. |
| `kGalleryCeilingDefault` | 10 000 | Outputs kept, and the deepest the gallery answers (`OLS_GALLERY_MAX`). |
| `kPruneSlack` | 128 | How far past the ceiling before the room is trimmed. |
| `kMaxTimingRows` | 400 | Rows in the timings ledger. |
| `kKeptJobs` | 200 | Jobs this engine remembers (spec.md §6). |
| preview | 32 frames · 256 side · 8 fps | The APNG a gallery card moves. |
| raw clip | < 48 MiB | When the codec-free copy is stood. |
| kept bytes | 2 MiB | A browser-sent picture. |
| vram budget | ¾ of the card's heap (floor ¾ of 24 GiB) | When resident weights get evicted. |
| engine budget | GiB − 4, floor 4, 8 GiB cards → 6 | What the engine cuts its graphs against. |
