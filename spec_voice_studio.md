# spec_voice_studio.md — Voice Studio

The sound side of this machine: the models it can offer, the voices a person keeps, the
runs that speak and listen, and every door a page or a program walks through to ask for
any of it.

This file states the voice module whole — its cards, its engine law, its jobs, its files
on disk, and its doors. The law shared by every module (transport, credential, rate gate,
job law, cache law, the file doors, the pages) lives in **spec.md** and is not restated
here; where this file needs it, it points there. The picture side is
**spec_image_studio.md**; the project side is **spec_project_studio.md**. Where code and
this file disagree, one of them is wrong.

---

## 0. Shape and related files

One engine stands behind this module: the vendored **audio.cpp** library, built as its C
API (`audiocpp_*`) and linked into the same process as everything else (see spec.md §0).
There is no separate engine process and nothing to open or close: the models answer
through handles inside this process, and **what a model can do is read off its own handle
while it stands loaded** — nothing here names a model's abilities by hand.

| File | What stands in it |
| --- | --- |
| `src/VoiceService.h` | The module's whole public surface: the constructor and every hand the doors call. |
| `src/VoiceService.cpp` | Everything else: the card table built from the engine's specs, the registry and sessions, the wav readers and writers, the voice library, the jobs, the renders. |
| `src/OpenLocalServices.cpp` | The doors. Every `/voice/api/*` route lives in `route()` there and is a thin read/answer over this module. |
| `src/Common.h` | `makeId`, `kKeptJobs`, `deviceCard()`, the atomic writers. |
| `src/Platform.h` | `curlFetch` — the only road to the internet. |
| `vendor/audio.cpp/model_specs/*.json` | The engine's own family specs — the source of every card. |
| `webui/voice.js` | The voice studio page: the model room, the speaking door, the listening door, the voice library, the renders. |
| `webui/index.html`, `webui/style.css`, `webui/common.js` | The shell the page hangs in (spec.md §13). |

### The doors at a glance

Every door below is behind the credential and the rate gate like everything else
(spec.md §2, §3), and answers with the cache stamps of spec.md §7.

| Door | Method | What it opens |
| --- | --- | --- |
| `/voice/api/models` | GET | Every model this machine offers, whole table. |
| `/voice/api/model-cards` | GET | One page of the same cards, narrowed and ordered by the words of the question. |
| `/voice/api/model-stats` | GET | The house answer: families, packages, bytes, tasks, languages — counted. |
| `/voice/api/status` · `/voice/api/health` | GET | The engine's state: what stands loaded and what it weighs. |
| `/voice/api/jobs` | GET | Every job this engine keeps. |
| `/voice/api/jobs/<id>` | GET | One job's whole record. |
| `/voice/api/jobs/<id>/cancel` | POST | Stop a standing job. |
| `/voice/api/speak` | POST | A speaking run. Answers 202 with a job id. |
| `/voice/api/listen` | POST | A listening run. Answers 202 with a job id. |
| `/voice/api/models/<id>/load` | POST | A card's weights into the engine. The engine's own yes. |
| `/voice/api/models/<id>/unload` | POST | The card's memory back. |
| `/voice/api/models/<id>/download` | POST | Fetch a package's files. Answers 202 with a job id. |
| `/voice/api/voices/<model>` | GET | What a model offers as voices: the shared library and the model's own presets. |
| `/voice/api/voices/save` | POST | Keep a recording (with its words, if they are known). |
| `/voice/api/voices/delete` | POST | Let a recording go. |
| `/voice/api/voices/rename` | POST | Move a recording to a new name. |
| `/voice/api/library` | GET | The kept renders, newest first. |
| `/voice/api/library/delete` | POST | Take a render and its story off the disk. |
| `/voice/files/render/<name>` | GET | A kept render's wav (spec.md §9). |
| `/voice/files/voice/<name>` | GET | A kept recording's wav. |

---

## 1. Where the cards come from

Every model this machine can offer comes from the engine's own model specs — the same
files the engine reads to know its families, packages, tasks, languages, options, and
voice presets — kept only for the families **this build actually registered**. The
registry names exactly the families its loaders were compiled with, so a family outside
that list is never offered as a model that cannot run; it is counted instead
(`families_without_loaders`, with its packages) so the table states the set it was made
from. A package whose format is neither `gguf` nor `safetensors` (an NPU pair, say) is
not offered either. Nothing names a model by hand: a model the engine knows is a model
the app offers, with the exact weight layout its spec asks for. The cards are built once,
on the first ask.

The engine's tree is found where the server runs from: `vendor/audio.cpp`, or
`src/vendor/audio.cpp` for a tree copied without the vendor folder beside it. The first
place that holds `model_specs` wins, and the answer is kept for the life of the process.

**Bundled models.** Small models that ship inside the engine tree
(`assets/framework/models/`) and that no spec names are offered too — a family with no
card is a task no page could reach. Their path is marked with `@`, which means "the
engine's own tree", not the weights room a person fills: those models are never
downloaded and the weights room is never searched for them.

A card (`VoiceModelDef`) carries: the package's own `id` and `name`, the family's
`description` and `family_name`, the family, the weights file (the first `.gguf`/
`.safetensors` the package names), the family's load options (its spec's declared
defaults), the tasks this build accepts (in the C API's words — several of a family's
spec names can stand for one task, and a card names each task once), the capabilities,
the packaged preset voices, every file the package names, the repo its files come from,
and the languages the family's spec names.

**Language words.** A family's spec sometimes states how many tongues it covers as free
text ("50+ languages", "zh dialects", "model dependent"). Those words are not the name of
a language — a word with a digit, a `+`, or a space in it, or one of the fillers
(`language_agnostic`, `model_dependent`, `nospeech`, `multilingual`, `polyglot`,
`unknown`, `other`, `language`, `languages`), never counts as one anywhere a language is
listed or filtered.

---

## 2. The weights room

A package's files stand under `OLS_WEIGHTS_DIR` (default `data/weights`), in the
directories its own spec names (`target_directory`, with `strip_prefix` taken off). A
name marked `@` stands where it says, in the engine's tree.

The room is **walked once** (depth ≤ 4 — the room is shallow: a family, a package, its
files) and every "is it here / what does it weigh" answer reads from that walk; the walk
is redone when something arrives or leaves, which is the only thing that changes it.

Unlike the image side, the voice side keeps **no verification records**: a package's spec
names its files but states no size and no sum for them. So a fetch that stopped leaves no
half-file behind — what a stopped fetch leaves is a file that is not the file, and nothing
here can tell it from the real thing later, so the half goes and the next fetch starts
that file again rather than finding a weight standing there and calling the package whole.

---

## 3. Standing a model up

`loadModel` is the only way a model comes to stand — nothing else reads a model's
weights. Standing the weights up is the device's work: the hand waits its turn on
`ols::deviceCard()` (spec.md §12) and stands there through the load, so no picture or
voice is ever drawing while the weights move onto it. A card with no files on disk is
skipped quietly — no files means nothing to load and nothing failed. A card that refuses
records the engine's own words for why, beside its row; the others still stand up.

`unloadModel` gives a model's memory back — on the card first, so nothing is ever freed
while a run draws with it. Its sessions close before the model, and the load notes kept
for it go with it.

**Sessions.** A session belongs to one kind of work: the engine is handed a task when a
session is made. A model declares several kinds, so one session per model would be the
wrong one for whichever door asked for a different kind. The door that speaks asks for a
speaking task and the door that listens for a listening one, each taking the first task
of that kind the model's card declares; the engine's capability question is asked before
a session is made, so a task this build has no loader for is never asked for, and says so
("this build has no loader that runs X for this model"). Sessions are kept per model, per
task, **per run control** — the same settings find the session that already runs with
them, and a changed control stands a new one up rather than answering with the wrong
settings. The engine is asked for this build's device backend (`"best"` — the CUDA or
Vulkan device, whichever the binary carries) first; when the family has no device
implementation of the task, the engine says so and the session is made again on the CPU.
The choice comes from the engine's answer, not from a guess here.

---

## 4. The speaking run

`POST /voice/api/speak` takes a json body and answers 202 with `{"job_id": "voice-…"}`:

| Field | Meaning |
| --- | --- |
| `model` | Which model speaks (required). |
| `text` | The words to say (required — empty is "nothing to say"). |
| `language` | Spoken with the text when named. |
| `voice` | A saved recording's name, or one of the model's preset voice names. |
| `reference_b64` | A recording the speaker listens to, inline (wins over `voice`). |
| `reference_text` | The words of that recording. |
| `session_options` | The controls the standing model is configured with — must be an object of names and values, or the run is refused before the engine is asked. |
| `options` | Per-run request options, passed to the family untouched. |

**The kinds it asks for.** A recording in hand (inline, or a name the library holds a
wav under) means the speaker is the one the model listens to: `clon, vc, s2s`, best
first. Words alone mean the model makes a voice: `tts, vdes, gen, clon`. A name the model
carries among its own presets is plain speaking — it travels to the family as a voice
name — not a cloning session.

**The voice for the run**, in order: an inline recording → a saved one from the library
(its recording, and its words from the sidecar when none were named) → the name passed to
the family as its own preset voice, travelling both ways a family can ask for it (the
structured speaker field and the plain `voice_id` option carry the same word, so a family
cannot hear a different voice than the page offered). The words of a reference recording
travel as the family's own `reference_text` option; families that do not declare it
ignore it or say so, and the run carries either answer through untouched.

**The style dials** a family advertises have their own calls in the engine, and the names
the page uses are the engine's names: `emotion`, `style_language`, `speaking_rate`,
`pitch_shift`, `energy_scale` (a dial given something that is not a number is refused
here rather than sent as nonsense), `style_tag` (its name and its value split where the
page wrote them — "a style tag needs its name and its value"). Everything else in
`options` travels as the family's own request option, untouched; a family that refuses
one says so in its own words.

**Word positions come by default**, so the player can track the words: a model with a
timestamps option is asked with it on, unless the page already named one.

**The run.** The whole visit to the engine — finding the model, standing its session,
running, handing the answer back — happens on the card. A run cannot be interrupted
mid-call; a stop asked after the run kept the record's truth and let the answer go
(cancelled, nothing written). The answer's audio is written to `data/renders/<id>.wav`
(16-bit PCM) with its story `<id>.json` — the whole finished record: `said_words` (the
words as typed, so the player lights them against the audio rather than against whatever
alphabet the family names its groups), `word_times [{word, start_s, end_s}]` when the
model states them (a model that states nothing carries nothing, and the page says so
rather than guessing positions), `url /voice/files/render/<id>`, `audio_s`, `elapsed_ms`.

**Refusal words:** "nothing to say" · "that model holds no memory: its card's load hand
is what puts its weights in" (with the card's own last refusal appended when it left one)
· "the run controls of a standing model have to be an object of names and values" · "no
session could be made for this kind of work: …" (naming the kinds asked when the engine
gives no words) · "the text was refused" · "the inline recording could not be kept" ·
"the recording could not be read as plain 16-bit PCM" · "the family refused the reference
recording" · "the family refused the voice name" · "that dial was given something that is
not a number" · "a style tag needs its name and its value" · "the family refused that
style dial" · "the family refused an option" · "the family refused its word positions" ·
"the run refused: <the engine's words>" · "the answer carried no audio" · "the answer
could not be kept as a file".

---

## 5. The listening run

`POST /voice/api/listen` takes `{model, voice | audio_b64, session_options, options}` and
answers 202 with `{"job_id": "listen-…"}`. The same engine, the other direction: a family
that runs speech-to-words answers with the words it heard, the segments and speaker turns
it found and where they sit; a family that converts, separates, or cleans audio answers
with audio. Nothing here is chosen by this program: what comes back is what that model's
own handle states.

**The kinds it asks for**, in the order of what a page can show from each:
`asr, diar, align, vad, spk, cls, wake, turn, sep, vc, s2s, svc, midi`.

**What the record carries**, from whatever the model answered:

- `words` + `language` — the words heard.
- `segments [{start_s, end_s, words}]`.
- `speaker_turns [{start_s, end_s, speaker, words}]`.
- `streams [{name, url, audio_s}]` — every named audio stream (separated sources,
  per-speaker tracks), each written to `data/renders/<id>-<name>.wav`.
- `artifacts [{kind, name, bytes, url, meta}]` — the payload bytes are the engine's own,
  written out unchanged so a person can take the file and read it wherever the format
  belongs. Kinds: `speaker_embedding`, `style_embedding`, `prompt_embedding`,
  `acoustic_tokens`, `midi` (`.mid`), `transcript_alignment`, `diarization_state`,
  `vad_state`, `custom` (`.bin`).
- plain audio → `url /voice/files/render/<id>` + `out_audio_s`.
- `audio_s` — the length of what was handed in.

A model that answered with nothing a page can show is an error, not an empty success:
"that model answered with nothing this page can show".

**Refusal words:** "no recording was given to listen to" · "the recording handed over
could not be read as a sound" · "no kept recording stands under that name: X" · "that
name is not the shape of a kept recording: X" (what the request carried decides the words
it is answered with — a page repeats the door's own words, and a person who named a
recording that was just put down or renamed learns that rather than being told nothing was
given) · "this model does not listen to a recording: …" · "this model refused an option
the page named: …" · "the listen refused: …" — plus the shared session and no-memory
words of § 4.

---

## 6. Fetching a package

`POST /voice/api/models/<id>/download` rides a job (`makeId("fetch")`) that walks the
package's own files, each fetched from `https://huggingface.co/<repo>/resolve/main/<file>`
with `HF_TOKEN`, into the package's place in the weights room. A package that names no
repo has nothing to fetch, and the door says so ("That package names no files to fetch").
The job's window is real from its first moment — none down, all of them to go — and each
file pushes a `file` event. A failed fetch removes its half-file (§ 2) and answers "the
fetch failed for X (curl answered N)", or the quotable-in-shell refusal when the name or
address holds characters a command line cannot carry.

---

## 7. The voice library

A shared room every voice model reads: `data/voices/`. A kept recording is
`<name>.wav` plus, when its words are known, the sidecar `<name>.txt` — models that clone
by listening need the words of what the recording says; a voice without one still clones
where the model works from the speaker embedding alone, so the sidecar is kept, not
demanded.

- A **voice name** is letters, digits, dot, dash or underscore, at most 64, not starting
  with a dot.
- A kept recording is a plain wav: at least 44 bytes, `RIFF`/`WAVE` — anything else is
  not kept. Only 16-bit uncompressed PCM is read back; anything else is said so, not
  guessed at.
- `GET /voice/api/voices/<model>` → `{model, library: [{name, wav, has_transcript,
  audio_s?}], presets: [{name, prefix?}]}`. The library is the shared room; the presets
  are the model card's own list, plus — where the card says they live on disk — the
  embeddings standing beside the weights (`embeddings/*.safetensors`, named with their
  last twelve characters taken off). Nothing is invented.
- `POST /voice/api/voices/save` `{name, audio_b64, prompt_text}`.
- `POST /voice/api/voices/delete` `{name}` and `POST /voice/api/voices/rename`
  `{name, new_name}` answer with nothing when they did, and with the reason they did not
  otherwise — one of the three things a person can tell apart themselves: "That name
  cannot be used: a voice name is letters, digits, dot, dash" · "No recording is kept
  under that name" · "Another recording already keeps that name" (rename) · "The files
  would not move". A refusal does not leave them guessing which of the two names to
  change.

---

## 8. The kept renders

`data/renders/` holds every answer this engine wrote: a speak's wav + story, a listen's
streams and artifacts. The newest **200** are what a page is shown; a machine that kept
more still holds every one of them, and each keeps its own file door.

- `GET /voice/api/library` → `[{id, wav, bytes, model, voice, made_at, word_times?,
  said_words?, audio_s?}]`, newest first (those with no live record fall in after,
  ordered by name). A render made before this machine started has no record here, but it
  kept its own story beside its file — the words, where they land, and who made it — and
  the row is filled from that story rather than standing lost.
- `POST /voice/api/library/delete` `{id}` takes the wav and its story. A render's name is
  lower-case letters, digits and dashes, at most 80 — the names this machine makes follow
  that shape, so a name outside it cannot stand for one of its own files.

---

## 9. The model tables (what the doors answer)

### `GET /voice/api/models`

The whole table, cached until something changes it (a load, an unload, a download):
`[{id, name, description, engine: "audio.cpp", family, family_name, weights_present,
loaded, can, declared_tasks, declared_languages, preset_voices, repo, files,
weights_bytes}]`. A model that **stands loaded** adds the truth its own handle gives:
`tasks_offline`, `speaker_reference`, `style_condition`, `timestamps` +
`timestamps_option` (whether it can say where the words land — the page only offers the
word-tracking door to a model that states it), `languages`, and `params` — every option
it declares at request, session, and load scope, with `{scope, name, value_name,
description, default, min, max, required}`. A model that was never loaded says so plainly
and carries no invented parameters; a model whose load was refused carries `load_error`.

### `GET /voice/api/model-cards?<question>`

A page of the same cards, asked in the words a page's own controls show:
`family=pocket_tts&task=tts&state=memory&q=hello&sort=bytes&offset=40&limit=20`.
A page that draws four hundred packages asks for a page of them, never for the whole
table, and the counts it shows are this answer's, not its own guess.

- `family` matches the family or its display name; `task` and `language` match **whole
  words** in the card's lists, so asking for one task never answers with a family whose
  name merely contains it.
- `state`: `memory`/`loaded` · `disk`/`files` · `missing`/`fetch` · `here` (loaded or on
  disk).
- `q`: every word of the question found somewhere in the card's id, name, family,
  description, repo, languages, presets, or tasks.
- `sort`: `bytes` (heaviest first), `name`, or nothing — by state (loaded, then on disk,
  then missing), then family, then id.
- `offset` and `limit` (default 40, at most 200).
- Answers `{total, offset, limit, returned, more, sort, rows}`.

### `GET /voice/api/model-stats`

The house answer, counted from the machine's own cards: `{ok, engine, build, abi_named,
families, packages, present, loaded, bytes, tasks, tasks_of_this_build, languages,
families_without_loaders, families_without_loaders_named, packages_without_loaders,
family_rows[{family, name, packages, present, loaded, bytes, tasks, languages, repos}]}`.
The task words this build accepts come straight from the engine — a page offers its
filter from these, never from a list of its own.

### `GET /voice/api/status` (and `/health`)

`{ok, engine, build, abi, abi_named, models_total, models_loaded, open_models[],
open_bytes, error?}`. Only the standing models are weighed — a handful of files at most,
which is what lets the weight ride a door read every few seconds. The packages themselves,
with the words their files state and the reason a load was refused, belong to the models
door, which answers all of that once for every package. The engine's version: the library
states its own build version when its build asked for one and says "unknown" when it does
not; the ABI this binary was built to is always knowable from the same header, spelled
`major.minor.patch`, so a page can name the engine it is really talking to.

---

## 10. The module's hands (VoiceService.h)

| Hand | Answers |
| --- | --- |
| `VoiceService(weightsDir, dataDir, threads, hfToken)` | Stands the module up. |
| `known(id)` | Whether the cards name the model. |
| `modelsJson()` / `modelCardsJson(query)` / `modelsSummaryJson()` | § 9. |
| `statusJson()` / `engineError()` | The engine's state; every note the engine left, joined. |
| `startJob(kind, modelId, bodyJson)` | `speak` \| `listen`. Answers the job's id, or empty when the model is not one this machine knows or the body cannot be read — the door then answers its own words ("Unknown model for this machine, or nothing to say" / "…or no recording given"). |
| `jobsListJson()` / `jobStateJson(id)` / `cancelJob(id)` | The job tables of § 11. |
| `voicesJson(modelId)` / `saveVoice(name, wavB64, promptText)` / `removeVoiceReason(name)` / `renameVoice(name, wanted)` | § 7. |
| `libraryJson()` / `removeRender(id)` | § 8. |
| `loadModel(id)` / `unloadModel(id)` | The engine's own yes or no (§ 3). |
| `loadErrorFor(id)` | Why **this** model's own last attempt refused, in the engine's words, or empty when nothing was recorded for it: a door that answers for one card must not repeat whatever model failed last. A session refusal is kept under the model, the kind of work, and the backend tried — the cpu note is the engine's last word, so a refusal answers with that, then the device's, then the plain note. |
| `startDownload(id)` | § 6. |
| `file(kind, name, path, contentType)` | The `/voice/files/` resolver: `render` → `renders/<name>.wav`, `voice` → `voices/<name>.wav`, content type `audio/wav`. A name is at most 96 characters and carries no slash; each kind is checked by its own name rule. |

### The job record, whole

`{id, kind: speak|listen|download, model, state, started_at}` plus `voice` (when a
recording was named) and `text` (a 120-character preview of what was said). A download
carries `done`/`max` — its real file count. A finished job finished every step of its own
count, so a row that reads "0 of 1" under a job that just completed would be a lie; a
stopped or failed job keeps the part it really did. A speak that finished adds
`said_words`, `word_times`, `url`, `audio_s`; a listen adds what § 5 lists. Ended jobs
carry `ended_at` and `elapsed_ms`. The newest `kKeptJobs` (200, spec.md §6) stand; past
that the oldest record is dropped, so the merged status answer stays a few kilobytes
however long the machine has been up.

### Stopping

Only a run still in motion can be stopped; a record that finished is named by the door's
own words ("Unknown job: <id>" / "This job has already stopped", spec.md §6) instead of
answered as though the stop had done something. A run cannot be interrupted mid-call: a
stop believed after the engine answered keeps the record's truth and lets the answer go.

---

## 11. The webui (voice.js)

The voice studio page (`webui/voice.js`, mounted in the shell of spec.md §13) is where a
person: browses the model room a page at a time with the house counts beside it
(`/voice/api/model-cards` + `/voice/api/model-stats`); loads, fetches, and puts down
cards; speaks — typing words, picking a model's preset voice, a saved recording, or a
recording held in the browser — and follows the run through its job record; listens —
handing over a recording inline or by name — and reads back the words, segments, speaker
turns, streams, and artifacts the model answered with; keeps recordings into the library
(with their words); and walks the kept renders, the player lighting the words against the
audio from the render's own story.

---

## 12. Constants

| Name | Value | Where it bites |
| --- | --- | --- |
| voice name | ≤ 64, `[alnum . _ -]`, not starting `.` | A kept recording's name. |
| render name | ≤ 80, `[a-z0-9-]` | A kept render's name — the shape this machine makes. |
| served name | ≤ 96, no `/` | The file doors' resolver. |
| kept wav | ≥ 44 bytes, `RIFF`/`WAVE` | What the library keeps. |
| read wav | PCM format 1, 16-bit | All that is read back; anything else is said so. |
| `kShownRenders` | 200 | Kept renders a page is shown. |
| story read | ≤ 64 MiB | A render's own story, read back when no live record stands. |
| `kKeptJobs` | 200 | Jobs this engine remembers (spec.md §6). |
| text preview | 120 characters | What a speak's record shows of the line. |
| card page | default 40, at most 200 | `/voice/api/model-cards` limit. |
| room walk | depth ≤ 4 | The weights room's one shallow walk. |
| inline scratch | `.<kind>-tmp-<job>.wav` | Where a request's inline recording stands for the moment the engine reads it — named by the job, so two recordings at once never read or remove each other's copy. |
