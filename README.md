# open-local-services — one local server for pictures and voices

This machine answers from **one binary**. There is no python in the product, no
per-service process you have to start, and no remote service anywhere: an
`open-local-services.exe` carries the http surface, the pages, and every service behind
them. The picture engine and the voice engine are **linked into it** — no
process, no port, their models answering through handles. Nothing else runs as a
child: every engine this server owns stands inside that one process.

Everything a person sees is served by that same binary. The pages are plain
HTML and JS carried **inside** it, cut in at build time, and read from the
`webui/` folder instead when that folder sits beside the running server. A
release started on a machine with nothing but its own file still has every page.

---

## Starting it, and what it prints

Two entry points, one for each kind of work:

- `run.bat` / `run.sh` — the development path. It finds the toolchain that is
  actually installed (the CUDA toolkit with its nvcc, the MSVC compilers and the
  studio environment around them, ninja, and the Vulkan SDK only for the shape
  that asks for it), reads the card in the box for the `sm_<arch>` its code is
  written for, builds whatever is missing **incrementally**, and starts the
  server. Both scripts build whole on that CUDA device with no word at all;
  `run.bat vulkan` is the one word that names the other shape. It never wipes a
  build tree, and a run that changed no source recompiles only the file that
  carries the version.
  One thing a bare shell on Windows does not carry is the compiler's own include
  set and its import-library path: `cl.exe` and `link.exe`, called from an empty
  environment, fail on a fresh tree (`cstdint`, then `kernel32.lib` and
  `libcpmt.lib`). Nothing in `run.bat` changes for that; the environment is stood
  by calling Visual Studio's own `VC\Auxiliary\Build\vcvars64.bat` before it, which
  is what a machine whose shell has no Visual Studio environment needs.
- `build-rel.bat` / `build-rel.sh` — the release path. It refuses a tree with
  unfinished work in `api/` and `webui/`, names the commit it is building, and
  writes the finished set into a `Release/` folder beside a `build-info.txt`
  that says which commit every artifact there came from and which pinned engine
  sources went into them.

The server prints, as it starts, everything a reader needs to aim at it:

```
API server on http://0.0.0.0:8765
code version: dev, compiled Oct 25 2026 13:24:03
API key required (X-Api-Key header, Authorization: Bearer, or ?key= query)
Rate limit: 600 requests per minute
```

- **One port, and no second choice.** These doors answer on port **8765** on every
  box, and the line above names that address. Another server holding it means this
  one says so and does not start: every address a page has for this machine names
  this port, so a server that stepped to the next free one would open
  a door nothing points at and be pointed at nothing itself. The start scripts stop
  the running server for you, but only after the build they were asked for has
  succeeded. `OLS_PORT` is not a setting any more, and a `.env` that still names one
  is said out loud at start and left alone.
- **Two kinds of doors, and one credential per visit.** The api doors — everything
  that is not a page — open on the api key kept in `.env`, handed as an
  `X-Api-Key` header, an `Authorization: Bearer`, or a `?key=` in the address
  (a courtesy for programs: no page of this machine writes one), or on the
  `ols-key` cookie the door wrote for the rest of the visit. A machine that sets
  no api key leaves its api doors open. The rooms — every page and everything a
  page loads — open to that same credential and to nothing else: what differs
  between the doors is not what they ask of but how a refusal is answered, an
  api door saying so in words and a room walking the visitor there. Keys are
  compared in a way that does not stop at the first difference, so an answer
  never says more than it should.
- **Nothing that a person reads ever names a credential.** The api key is a
  secret: it is nowhere in the pages' words, addresses, image references, or
  answers — the config rows answer with `set`/`not set` for secret values, and
  only. A refused room walks the browser to that address naming the room it was
  asked for; a refused file the rooms load walks there plainly. That address
  answers ungated, so a visit reaches it before anything else can, and it asks
  of nothing — the visit that has no credential yet is the one that reads it. A
  answer says which doors it opens and, when the rooms open, writes it on the
  connection as a cookie — the rest of the visit, files, pictures and event
  streams alike, carries it without any address naming it. A browser holding
  what it was given opens the rooms by itself; one that is not carrying cookies
  is told so after the second try at the same room. What the browser was given
  is kept in that browser — the active one beside every other, newest first,
  marked by how it was given — and config options never move a credential: they
  change what the machine answers to. After three refusals in a row these doors
  go silent — connections get no response at all — until thirty seconds pass
  without a new refusal, which reopens one for an honest answer; any accepted
  credential lifts the count back.
- **The pages stand at those doors.** No page carries a field for a credential:
  a browser that comes without one is walked to the address that answers what is
  asked of, and the page it asked for is opened the moment the credential this
  machine answers to is handed there. What a page
  knows of the doors it states from the doors' own answers — a machine that
  refused a visit is never drawn as a machine that is absent, and a quiet door
  says it went quiet rather than nothing at all.
- **Starting this server by itself is a door, not an installer.** `GET
  /api/service` states what the machine's own manager says about it - name,
  manager, installed, running, `automatic`/`manual`/`disabled`/`not installed`,
  the binary it would start and the folder it would start from - and `POST
  /api/service` with `{"action": "startup_on" | "startup_off" | "remove" |
  "shutdown" | "restart"}` works that manager and answers with its words, stderr included.
  Linux goes through systemd, writing a unit only where none exists and then
  enabling it. Windows cannot carry a console program in the service control
  manager - the manager ends a program that answers no control signal - so its
  start-on-boot is a task the task scheduler starts when the machine starts,
  given `--home <folder>` so the server reads the `.env` of the folder it was
  set up in. That entry names SYSTEM: where the server runs as a plain person
  the ask is refused, and the same command is then put to the desktop's own
  elevation prompt once, the person at the machine answering it. **The server
  stands at that level by itself**: before anything is read, the program asks the
  machine for the leave its manager works at — Windows through its own prompt, a
  Linux box through `sudo`, and only where `sudo` answers without being asked for
  a password, because an attempt that waits for a password would look like a
  server that never started — and prints which level it ended up standing at, so
  a start says it rather than being guessed at. `/api/service` answers `elevated`
  with what the machine said, which is what a page needs to know before it offers
  a hand at all. `OLS_ELEVATE=off` is the one way a person declines that ask on a
  machine that would otherwise make one; the doors answer the same at either
  level, and only the machine's own manager asks answer differently. Whether the
  entry stands afterwards is read off the machine, not out of a message. Nothing
  is installed anywhere and nothing is copied: it is this program, out of this
  folder. `OLS_STARTUP` in `.env` is the same fact as the toggle on the page,
  and a change there carries to the manager. `shutdown` stops the entry and lets
  this process go; `remove` takes the entry away. `restart` is the soft hand for
  a person who has just built this server: its answer is written first, and this
  process then starts the program that stands on disk now as its own child — a
  child carries the level its parent stands at, so a machine that already raised
  this server is not asked again for permission. The child answers in its own
  window.
- **`code version:`** names what that binary was built from, in exactly two
  kinds. A development build prints `dev, compiled <time>` — the dev tag and
  its compile moment only; it names no commit, because a development binary
  runs the code of a working tree, not one fixed source tree. A release build
  prints `rel <commit>, compiled <time>`: the release path refuses an
  uncommitted tree, so that commit names the **one** source tree the artifact
  can be re-created from — an invaluable debugging anchor that ties any binary
  to the exact code inside it. The development path names its own version on
  **every** run, including a run that reuses a build cache another path
  configured — a name a binary prints is always a name that build gave it, so
  a page never shows a commit it was not built from.
- **`ggml_vulkan:` / CUDA lines** name the device the engines will use. A
  Windows development build reads this box's card (`AMD Radeon(TM) 890M Graphics`)
  with no configuration beyond what the entry point carries.

---

## The pages

The pages are one family: a shared header, a shared sheet, and one shared script
(`common.js`) that owns the things every page needs — the key, the polls, the
model list, the engine state. Each page adds its own controls. The nav carries
the home page first and the two studios to its right: **home**, **image studio**,
**voice studio** (the home page is one of them, and it is also the page the bare
address opens).

Where a reader reaches them: the server answers one address, the one its start line
names (`API server on http://<host>:<port>`), and every path below it belongs to it.
The bare address and `/index.html` open the home page; `/webui/<page>.html` is the
file on disk when a `webui/` folder sits beside the server, and the same bytes carried
inside the binary when none does; `/image`, `/voice`, and `/status` answer with a
redirect to that page, so a short address is a courtesy for a person, not a
one the running server states at start; a page opened without it says so rather than
showing half a machine.

On the home page, the hands over a table's rows belong to that table: the
`show:` and `search:` controls sit **above the rows they narrow, inside the card**,
and when a reader presses a row, that job opens **below the rows as one card** —
the model it works with at the top left in the loudest type, the job it is named
by across the middle of that line in italic, and the seconds it has stood here in
the right hand, with everything else the machine stated about that job drawn
under that line as labelled fields (kind, state, its step window, when it was
asked and settled, what it made, the form its transformer was opened as, and a
prompt or a source when the machine states them). The `+`/`-` hands on that line
open or close the fields, so a reader can keep the job named while deciding how
much of it to read. Nothing about one table stands elsewhere on the page, and a
field the machine did not state is not drawn.

The header of every page carries only what every page needs about the work:
how many models the engines hold open and what they are doing, with the line
beside those badges naming the work going on or saying nothing is. The machine's
own facts are not in a header - they stand where the machine is read, across the
top of the **machine** panel on the home page: three separate badges, cpu and
memory, gpu and vram, and the code version with its compile time, each keeping
its own border, its own fill from the bottom, and its own words, spaced apart
rather than merged into the wall of figures under them. What is not read says so
(`not read`, `unknown`) rather than being guessed at.
The machine's *health* is painted, not written
anywhere near those badges — the **home** tab in the nav wears a glow behind
its words instead, mostly transparent and breathing, whose colour is the state:
orange until a reading arrives, red when the server cannot be reached, yellow
while only models are being stood up, blue while counted work runs, green at
rest. One class on that tab is what says which (`common.js`), and the sheet holds the colours, the
wash, and the animation. The same reading is stated in words in one place only:
the **this page** panel at the top of the home page carries the line - `server
not read yet`, `server not reachable`, `server ok: models being stood up`, `server
ok: work running`, or `server ok: nothing running` - written by the same call that
paints the tab, so the words and the colour cannot disagree. No other page states
the health in words. The studios carry no such badges at all: a page with no
machine panel draws no machine badges.
The one hand
the header adds is **config options**, the panel the machine's own settings are
read and changed in - `GET /api/config` and `POST /api/config`, plus the service
door - and it is the only hand the header carries beside the pages themselves.

What an action *starts* is announced where a person can read it, not painted into
the header: a small stack of notices sits under the navigation on every page,
newest first, each carrying a bar that counts down its reading time and states the
time left. A notice follows one job through its whole life — work that has not
started yet keeps a bar that cannot be counted, a running job keeps the time left
measured from the machine's own recent steps (a page that measured none states no
time), and work that states no scale keeps that uncounted bar — and the notice is
taken down when the work itself says it is over, leaving one readable line about
how it ended. `pause` holds one notice still, `close` takes one off the stack, and
`close all` clears it. Refusals repeat the machine's own reason in their words.
Nothing is
announced there for information the page already shows.

How long a reading may be *reused* is stated by the machine on the answer itself, and
the browser keeps it that long between its own polls. What a job answers for is not
held: a model table, the counts read across one, what the machine
holds, the gallery and the kept pictures are read again on every poll. So the moment
a download's last byte is down, the card that offered `bring its files` states the
files as on disk, the model is offered by the pickers that offer only what is here, and
the counts say so — no restart of the server and no reload of the page, because a
reload would only read back the same held copy the page was already reading. What
changes when a person changes it — a setting, a kept voice's name — is held for
the seconds its door names, since asking again says nothing
new. The shorter hold is not more work for the machine: each service builds its table
once and hands that answer out until a job marks it stale again, and a page of cards is
drawn from the rows that answer was built from rather than by reading them back out of
it.

What a page *says* about its own work goes into one record rather than a rail
beside each page's controls: the image studio, the voice studio, and the home
page write the same lines, and the home page is the one that
draws them — a panel across its middle, oldest first, every line naming the page
that wrote it and coloured for progress, a stop a person caused, or an error. The
record is kept in the browser that wrote it, so it holds what this browser's pages
said, and `clear` takes those lines off this browser and touches nothing on the
machine. A line about something that stopped or was refused also goes on the
notice stack of the page that raised it, because that is where the hand that
caused it was.

**image studio** (`imagegen.html`) — model cards down the left, each naming what
that model is good at, its steps and guidance defaults, its size and picture
reading limits, whether its files are on disk and how much memory it would take,
and — for a model whose transformer is published in more than one precision
(flux-3 klein is: ten forms, from Q3_K_M through Q8_0 and the Unsloth Dynamic
files up to BF16) — how many forms it comes in, and the one the machine opens it
with now, which stands on the card by itself: the forms the card is not holding
are chosen from one dropdown under it rather than drawn as one hand each, and
every choice names where its bytes would come from — this machine's own file,
or the source. A download or
a load carries the form a page asked for, a name that model is not offered as is
refused with the names that exist, and the machine keeps that form so a later
load opens the same file; a machine that was asked nothing opens the files it
already has, and only falls back to the model's default form where none of it
has been fetched. The text encoders and the VAE keep their own files beside it.
hands on each card: `download` (the weights, checked against the source SHA-256),
`load` and `reload` (open the model on the card), and `unload` (give its memory
back while its files stay on disk). The prompt and the words to avoid each open five lines deep and stay
there, with the reference strip under them; the size/steps/guidance/closeness/
seed/count controls fill the settings panel, the size opening at the square the
model's own card names rather than at one the page guesses, and beside them the
switches: system memory, vae tiling, fast attention, and `work on cpu`, which leaves
the card to whatever else the machine is doing and opens every weight and every step
where the threads are - and a model standing open on the other device moves its
weights rather than drawing on the context that was standing. At its
foot the card of the model a run would draw with is restated — its name, what
its card says it is good at, the weights it wants and where those bytes stand,
the form its transformer is opened as and how many forms it comes in, its own
steps and guidance and the words it usually avoids, the size its card names, the
sampler and scheduler it asks the engine for, and whether a step cache has room in
the steps it names, and what it does with a
picture — the same card the rail draws, from the same reading, so what a blank
dial answers for is read beside that dial rather than found two panels away. A
card that states nothing of some kind says so rather than being filled in, and
with no model in use the foot names that. The two
panels share a row — the words take three parts of its width, the settings the
fourth — below a window narrow enough to squeeze either of them that row becomes a
stack again. The three generate hands are one bar read as a single
control — they stand edge to edge with one line between them, and only the
outer ends keep the corners a hand has. The plain `generate` keeps its place at
the left and takes the room, so it stays the obvious first press, and two quick
batches stand at its right as marks kept to the width they need: `X4` (the ask
once, for four pictures) and `X16` (four runs of four, one after another, ending
the batch at the first run that fails or is stopped). The mark is the whole ask,
so what the hand does is written in its own tip rather than in words across the
bar. A batch is followed picture by picture rather than run by run: the ask puts
one notice per picture on the timed stack before any of it runs, each drawn in the
shape the model cards are drawn in — the model it will draw with named at the top
in the loudest type, the prompt it will draw with smaller under that, the settings
the ask named (its steps, its size, guidance, seed, the pictures it draws from) in
italic, and its state as the last line, which says where it sits in the batch and
among the pictures standing on this machine. A notice keeps its line until its run
starts and becomes the full card, its bar counting the steps that run states and
its estimate counting down, from the moment its run runs; it is taken down with
what its picture ended as. Words are kept to what each of them states: a setting
the ask did not name is not claimed, a prompt the machine does not state is not
shown, and an estimate names the rate it was built from — this run's own steps,
or the steps the runs of that model here took — rather than being shown bare. The
stack is repainted from the machine's own records on every reading of the
machine, so the pictures that stand here are stated again after a page reload,
after a run finishes, and after another window asked for work of its own (the nearest of that work gets its
own card, and the rest are counted in the position rather than drawn); a run
whose picture count the machine does not state is kept as one notice that says so.
Each picture costs what one picture costs, so the batches are marked as
what they ask for. The gallery keeps what this machine has drawn and
groups repeats: one ask — model, prompt, size, steps, guidance, words to avoid and
seed — draws **one**
card, and every render that ask made is a thumbnail in the row under its picture.
The room is drawn a page at a time: the `older` and `newer` hands beside the
filters move the window this page stands on, a hundred pictures at a step, and the
line under the controls names which part of the room is on the screen — so a search
that finds nothing there is not a promise that nothing older matches, since the
hands reach the rest rather than leaving it unreachable.
Pressing a thumbnail puts that render in the picture above and rings the thumb
that was pressed; the hands under the picture — `open`, `save`, `again`, `keep`,
`delete` — then belong to that render. The opened viewer carries the same row
under its frame, so the renders of one ask can be compared in the frame itself.

A model that moves is drawn on the same page, in the same gallery, and its clip is
not a picture: the machine writes **five files** for one clip, each shaped for a
different hand. The clip itself is an **MPEG-J AVI with the stereo sound muxed
into it** — the RIFF layout, stream headers and chunk sizes that an outside reader
walks without complaint (its own probe names both streams, the frames at the size
they were drawn at, and a duration that matches the frame count). Beside it, named
from the same base, stand `.raw.avi` (the same frames as plain pixels, which no
player needs a codec for — the outside reader calls it by its pixel format; much
bigger, asked for only when it fits), `.poster.png` (the still first frame),
`.preview.apng` (a small thinned copy, which a browser draws as an *image*, so a
gallery card plays a clip without a video element and without transcoding), and
`.wav` (the track on its own, which browsers play). No `.mkv` and no `.mpg` are
offered: a Matroska header has to be written from the spec's binary element table,
which the prose alone does not give, and a program stream needs an MPEG-1 encoder
that this tree does not have — the shapes that carry whole frames are the ones that
can be written and verified here. No `.gif` either: the animated-picture writer
this tree hand-rolls was read at its header by both outside readers but not past
its first frame, and a shape that cannot be shown to play is not offered. The
record states the clip's kind, its frames, the rate they were encoded at, its
length in milliseconds, the sound's channels and rate, the reference pictures the
run started from, and which of those shapes came out; the card's words are read
from that record rather than guessed from a filename. What the gallery holds is
**one item per clip**, and its parts are never listed as items of their own; a
removal that drops a clip drops the poster, the preview, the extra shapes and the
track with it, and the room's limit counts clips. A card's picture is the
animated copy, its `save` hand takes the whole clip, the extra shapes are offered
beside it as downloads when the record names them, a `sound` hand plays the
clip's own track where one was written, and `keep` keeps the **poster** as a
starting picture — a model that moves starts from a frame, not from a clip.
`again` puts the prompt, the model, the clip length and the pictures that run
started back in use, naming any reference this machine no longer keeps rather
than drawing a different picture silently. The page offers a **frames** field only
to a model that moves: length is asked in frames, the unit the model's
engine aligns to, and the page says what that means in seconds at the rate the
clip is encoded at; the record states the length that was actually drawn. A clip's
length is part of an ask: a run at 39 frames and one at 124 are two amounts of work,
so they do not share a card, and the timing ledger files them apart — the cost of a
clip belongs to its length, not only to its steps.

The ask a model was given is kept on this browser, per model: the words, the avoid
words, the size, the steps, guidance, closeness, seed and count, and the starting
pictures that stood in the strip - the strip names them by what the machine calls
them, while the pictures themselves stay on the machine. Pressing another model's
card writes what the controls hold for the model being left and puts back what this
browser kept for the one being picked: a model's numbers are its card's numbers, so
twenty-eight steps for one model are not carried over onto a four-step distilled
one, and a prompt written for one model's strengths says nothing useful to another.
A starting picture the machine no longer holds is named as gone, and a strip longer
than the picked model can take stops at its limit with the rest of them named. The
`reset` hand in the prompt panel's title lets go of what the browser kept for the
model in use and takes that model's own defaults back: its prompt, the size its card
names, every dial blank, strip empty. A run keeps the ask it is about to take, so
reloading the page opens on what was last asked of that model.

The viewer's bar opens with the scroll, and its one hand is `play`: it moves
through the runs of the ask the picture on the frame belongs to — the renders one
prompt and one set of settings made — each standing for the time the first
dropdown names (**5s**, **15s**, **30s**, **1m**, **2m**, **5m**), moved to with
the second's move (`fade`, `slide`, `zoom`, `wipe`, `none`) made in the third's
speed, and **the last of them going back to the first**, so a scroll that reached
the oldest of four runs stands on the newest again. A line fills along the bottom
of the frame over the time a picture is kept, the countdown begins when that
picture's bytes land — a picture that spends four seconds coming through spends
none of the ten it was given — and the move is played on the incoming picture at
the zoom and place it stands at, so a scroll never snaps a zoomed picture back to
the middle of its frame. Closing the frame stops the scroll; a picture that is the
only one of its ask is told so on the press rather than given a hand that answers
nothing; prev, next and a press on a run under the frame give the picture they
opened its whole time; and the three choices are kept in this browser, so a page
opened again scrolls on the clock it was last given.

**voice studio** (`voice.html`) — the voice families and their cards, kept
voices, a listen and a speak hand, and a library. A package whose files are not on
this machine names the repo they come
from and offers `bring its files`; the page follows that job as it runs — its bar
counting the files taken and the seconds gone — and when the last file is down it
says so and draws that card from the machine's answer again, which now states the
files as here and offers the load hand. A card says honestly what kind of answer
that family gives: words
located as the audio plays appears only where that family states positions
itself; nothing is claimed for a family that does not. A player never makes
positions up: the page does not time the words off its own clock as the audio
runs, because words are not spoken evenly and a mark placed by a stopwatch is a
claim about the render that nothing made. What a render states is what lights, and
the playing line says which of the two it is — "with its stated groups lit", or
that the model states no positions at all.

**home** (`index.html`) — what this machine is doing now, as a wall of the
machine's own tiles: the CPU (the share of the time each processor of it spent
working — one line per thread, read from the kernel's per-processor record),
memory, the volume this server runs on (its used bytes drawn as a chart behind the
card's own words), the device its engines use, and the work those engines keep —
picture work and speech work — each rate placed at the moment the job that
made it was asked, the moment its own row names, so a line there joins the rates
this machine measured, one job to the next, and says nothing about the work
between them. Every trace is a fixed window of the page's own readings with
the newest at the right edge; the highest, the lowest and the mean taken inside
that window are drawn once, at the places they were read, and a figure that holds
several lines keeps its lines unlabelled and names one scale across all of them.
A line is broken, not drawn flat, past a stretch its own readings did not cover.
A read that states the same numbers as the last one draws nothing: the wall, the
cards and a reader's own choices in a table stay as they were, so the page only
redraws when this machine says something new.
It reads records only and measures nothing of its own, which is why an engine or a
metric that states nothing keeps its place and says so — an absent number is never
drawn as a zero. Across its middle sits the log: the one place the lines every
page wrote are drawn again, oldest first, each naming the page that wrote it.

### The files every page is built from

- `index.html`, `imagegen.html`, `voice.html` — the three pages: they lay out the
  rooms and the controls, name what each one is for, and leave the numbers to the
  scripts below them.
- `key-shim.js` — the key, kept once in this browser for this address, sent as a
  header on the reads that can carry one and as a word in the address on the ones
  that cannot; it also holds the standing answer to "is this key accepted", which
  is what the page in front of the doors believes.
- `status-cache.js` — this browser's copy of the machine's answers, kept only for
  the length a door's own `cache-control` states; a write clears the readings of
  that service, and callers asking the same path at once share one network read.
- `common.js` — the one library every page loads after those two: the header bar,
  the running-state line and the machine's badges, the readers of fields, options
  and form controls, the words for bytes, times and rates, the notice stack and
  the job card, the progress bars, and the record of what the pages said.
- `config.js` — the config panel: the machine's own options as the server states
  them, with the key row that rewrites that key and this browser's together.
- `monitor.js` — the home page: `/api/monitor` drawn as the machine's wall, the
  model cards and their tables, and the log read again.
- `app.js` — the image studio; `voice.js` — the voice studio.
- `style.css` — the one sheet all three pages ask for, holding the shapes and the
  colours; `favicon.ico` — the

---

## The doors

Every door takes the key. A path nothing answers is the caller's mistake —
`404 No route` — while `502` is kept for a module genuinely not wired into this
binary. A job is started with a `POST` that answers `202` and its `id`; the
states are `running`, `done`, `error`, `cancelled`, and an answer's words live
in the record's `text`. A record carries the elapsed time of the work, and the
counts that work states for its own kind - a picture run its steps, a video run
its steps and the length of the clip it drew, a speech render its blocks and the
seconds of speech it made - which is what the status page reads. A clip's arrival
reaches a page as its own event, `{"type":"video","clip":…,"poster":…,"preview":…,"audio":…,"frames":…,"fps":…}`,
so a running page can play the moving copy and name the length without asking the
gallery again; the same names stand in the render's record, where they are bare
file names and the door they come through is one path.

  CPU, memory, the device its engines use), `GET /api/monitor` (that, beside
  the running and finished jobs of every service and the words their records
  took), `GET`/`POST /api/webui/expand` (a GET says where the pages served come
  from - inside this binary, or out in a `webui` folder beside the server; a POST
  writes the pages it carries out beside it, so the on-disk copies are the ones
  served from then on, and a folder that already holds pages is left exactly as
  it is), `GET`/`POST /api/config`.
- **pictures** — `GET /api/models` (what this machine can draw with, and what
  each is good at), `POST /api/models/<model>/load` and `download`,
  `POST /api/unload`, `POST /api/generate` (a run), `GET /api/jobs` and
  `GET /api/jobs/<id>`, `POST /api/jobs/<id>/cancel`, `GET /api/gallery` (**a page**
  of the kept pictures, newest first, named by `?limit=` and `?offset=` — the answer
  states the window it served and how many records the machine holds, so a reader
  walks the room a page at a time rather than taking all of it in one breath; a
  window past the end answers from where the room ends rather than pretending to
  hold one), `GET`/`DELETE /api/gallery/<item>`,
  `GET`/`POST /api/refs` (the reference strip: keep a picture this machine has,
  or the bytes a browser is holding) and `DELETE /api/refs/<name>`,
  `GET`/`POST /api/prefs` (this page's own settings, remembered on the machine),
  and `GET /files/…` for what this machine drew.
- **voices** — under `/voice/api/`: `models`, `model-cards`, `model-stats`,
  `status`/`health`, `library` and `library/delete`, `voices/<name>` with
  `save`, `delete`, `rename`, `listen`, `speak`, `jobs` and `jobs/<id>`, and
  `GET /voice/files/…` for what this machine spoke.
---

## Devices, back-ends, and what a build aims at

- A Windows development build uses the **MSBuild generator** (no `-G`); a CUDA
  build on Windows must use **ninja**, with the compilers handed through the
  studio environment and `CMAKE_CUDA_COMPILER`, `CUDAToolkit_ROOT`, and
  `CMAKE_CUDA_ARCHITECTURES` named. Both entry scripts find that ninja on
  **PATH**, in the CMake bin folder, and in the Studio's CMake-tools folder
  beside the very compiler they chose — a box with CUDA but no ninja on PATH
  is a normal shape, not a missing tool.
- The per-thread CPU line the home page draws comes from the system module's
  per-processor record, asked through `NtQuerySystemInformation`. On Windows that
  symbol lives in a module the development tools do not link by default, so the api
  build names **ntdll's import library** when the compiler is MSVC's — the library is
  found under the installed Windows development kits for the target architecture, and
  a build that cannot find one links without it and asks the record at run time, where
  a kernel that keeps no such record states nothing and the page says so rather than
  drawing a flat line.
- On the boxes whose toolchain keeps one binary, the voice engine's ggml and the
  image engine's shared library are **two copies of ggml in one process**, and the
  order of the link decides which one answers the server's own ggml calls. The
  voice engine's archives are named **before** the image library, so the server
  answers with the copy it already contains; named after it, the server asks its
  neighbour for functions it already defines, the two copies pass each other's
  tensors, and the process stops with a shape assert inside the image library's
  ggml while it opens a voice model. The same rule keeps those symbols out of the
  dynamic table, which is the other half of the hazard: the shared library must
  not bind its own calls to the copy inside the binary. Windows keeps its engines
  in separate libraries and is left as it is. The check after such a build is that
  the built binary holds no undefined reference to the ggml tensor functions and
  the voice door answers instead of the process stopping.
- What a target needs is named **at build time, never guessed at run time**: the
  CPU flavour (`GGML_AVX512` and its siblings on or off — a Zen5 box has them,
  an i7-9750H does not) and the card generation (one binary per generation:
  `sm_70`, `sm_75`, `sm_86`). Engine trees keep one build root per shape, and
  the two entry scripts name those roots **alike** (`api/vendor/stable-diffusion.cpp/build-c75`,
  `api/build-c75`, …) so a release run reuses exactly what a development run of
  the same shape built; the vendor sources stand under `api/` on every platform,
  which is the name `api/CMakeLists.txt` asks for. Nothing is cleared between
  runs, and the dev path re-states its own version and image library on every
  run so a shared cache always answers for the path using it. The card
  generation and the backend are different: they decide the compile commands of
  every object in a tree, so no entry point restates them. A root whose cache
  names another of them is **refused** — the words name both the value that
  stands there and the one asked — and the caller who means that shape says
  `fresh-full-rebuild`. A root that is not there yet is the first build of that
  shape, which is not a rebuild of anything and goes ahead. Built files are
  taken from where the generator that ran writes them — a `bin\Release\` for
  the Visual Studio one, a plain `bin\` for ninja — never guessed.
- The device backend is **CUDA** on every box that carries a card answering to
  it, and both entry scripts take it with no word at all. `run.sh` finds the
  newest toolkit with an nvcc and asks this GPU its compute capability; `run.bat`
  names nvcc, ninja and the studio environment, reads the card off `nvidia-smi`
  (the `arch=` word overrides it), and asks nothing of the Vulkan SDK — the image
  engine is configured with `-DGGML_VULKAN=OFF` and the server's own device code
  with `-DOLS_CUDA=ON`. `run.bat vulkan` builds the SDK's shape in the plain
  roots, and a Windows box with no toolkit says so and names that word rather
  than flipping a shared tree's shape by itself: the CUDA work belongs on the
  CUDA boxes, and a binary aimed at another box's card keeps to that box's CUDA
  runtime so it can load it.
- A picture run asks the image engine's **step cache** (ucache): a step whose
  latent barely moved reuses the step before it rather than running the UNet
  again, which is where the work stands when the card is small. The library names
  this path for UNet models and refuses it, itself, for the families whose
  transformer is not a UNet - so one ask covers every model and the library
  decides per model, silently: with no log callback set its own words are dropped
  (`sd_log_dispatch` returns when no callback was set), which is also why its
  "UCache enabled" line never appears in the server's log. The numbers carried
  here are the ones that documentation gives, because its own init leaves the
  threshold at "never" and naming the mode alone would change no work at all. A
  run may say `"cache": false` to draw the same ask without the cache - the
  comparison those documents ask for before trusting one. Measured on this box
  (one picture, 768x768, 20 steps, same prompt and seed): with the cache 53.8 s,
  without it 75.6 s, both rendered. What was left alone is what its model card
  and its own file state: the weights stay **fp16** as the file holds them
  (nothing quantized - the card's fp8/int8 ladder belongs to another version of
  that model), no VAE tiling where the shared window has 78 GB free, and the
  thread count stays at that box's physical core count, since 22 of its 24 CPU
  threads sit idle while a run works and the 16-CU card is the limiter.
- The picture and voice engines are inside this binary and hold no port. There
  is no engine port on this server: every engine the server owns stands inside its
   own process, and there is no second address on the machine for a page to reach.
   Nothing loads a model by itself: only a card's `load` hand does.
- Two copies of ggml share this process: the one inside the binary, which the
  voice engines run on and the device probe answers from, and the one inside the
  image library. Whichever of them installs a crash handler second stops the
  process at the assert in `ggml.cpp:22`, before the server can print a word, so
  `GGML_NO_BACKTRACE` is set by the entry points and neither copy installs one. A
  crash still prints its own line, naming the check that failed. No ggml library
  is laid beside the server for it to load as a plugin: a backend the binary
  already carries is registered twice that way, which is what a cuda release on
  MSI failed on as a finished picture was handed over.
- A release folder started on its own needs the two things the entry points set
  anyway: `GGML_NO_BACKTRACE`, and the CUDA runtime reachable on PATH (the CUDA
  binaries import `cudart64_12.dll`, `nvcuda.dll`, `cublas64_12.dll` and
  `cufft64_11.dll` by bare name, and the folder does not hold them).
- The weights room, and the `.verified.json` records inside it, are read from the
  path the server starts from, so `OLS_WEIGHTS_DIR=weights` names the `weights`
  folder beside there. An absolute path is read the same way and answers the same:
  the same machine, the same room, the same cards state `on_disk: true` under
  either spelling.
- A weight file this box has never stood on is read once to a sum before any card
  can say it is there, and that reading is what the model table waits for: asked
  after ~35 GB of new weights landed, the table took 150 s to answer, and the ask
  after it answered in 51 ms. The sum is remembered beside the room, so the cost is
  paid once per file and a page waiting on it is not a page that is stuck.

## Where things live, and what a release carries with it

`weights/` holds the model files (its `voice/` group beside the image models),
`data/` holds what this machine keeps — its outputs and its kept records —,
and `.env` carries the key. Folders are created **when they are needed**:
a mistaken start does not lay down an empty data tree, and a release binary
started beside nothing but itself generates what it needs and downloads nothing
it was not asked for. Every file under `webui/` — the pages, their scripts, the
sheet, and the icon — is embedded at build time and served from disk
when a copy sits beside the running server; a build says so in its configure
output (`pages carried inside this binary: N files … (written again)` or
`(unchanged)`), and binary.

The kept pictures are a room with a ceiling of their owner's: at most **`OLS_GALLERY_MAX`**
renders stand in `data/outputs` (**10,000** when nothing is said), and the room is
trimmed only once it stands a slack past that number, which takes the oldest slack
off in one pass rather than the oldest file every time a picture lands — the walk
of the folder is what costs, not the delete. The same number is the deepest a
gallery page can be asked for, so a reader is never offered a page the machine
cannot fill; what a machine holds is stated by the gallery door itself.

A clip is one item of that room and stands as five files and one record: `gen-…-0.avi`,
`gen-…-0.raw.avi`, `gen-…-0.poster.png`, `gen-…-0.preview.apng`, `gen-…-0.wav`, and the
record `gen-…-0.avi.json` — the clip carries its container in its name, while the parts
are named from the run with a word that says what each is. A walk of the room finds them
with the clip and never counts them as renders of their own; the same is true of a
removal — dropping a clip drops its plain-frame copy, its poster, its preview and its
track. The served types say what stands at each address:
`video/x-msvideo` for a clip and for its plain-frame copy, `image/apng` for the animated
copy, `image/png` and `audio/wav` for the still and the track, and a clip, a preview or a
track is asked for by the same `/files/outputs/` door as a picture, with the same one-`Range`
answers and the same long-lived cache header — a render's bytes do not change, so
a browser that has read a clip's preview once does not ask again.

`Release/` is the artifact set a machine runs, and it is not carried in the
repository: a folder per platform, an
`open-local-services`/`open-local-services.exe` there with the
`stable-diffusion.dll` beside it, the `espeak-ng-data/` the voice engine reads,
and `build-info.txt` naming the commit and the pinned engine sources (the `.pin`
files beside the vendored trees are what makes a pinned source reproducible).
Those bytes are regenerated by the entry script on the box that runs them and
travel to another box over the link, so what a clone carries is the scripts that
make them - and `build-info.txt` is what a later build of the same shape is read
against. No ggml library sits beside the *server* in any shape: the server's
ggml is inside it. A
release carries its version in the name the binary prints and in that file; a
development build carries its own name and no commit. The rule both ways: **a
name a binary prints is always a name that build gave it.**

Bulk payload — weights, trees, artifacts — moves between the machines over the
2.5 G link, never as a zip of an installed toolchain, and a Windows flavour is
compiled on the Windows box that is meant to run it: the CUDA `x64-cuda-75` set
is built on the MSI laptop against its own card, whose build roots live there,
while a box with no toolchain of its own receives the built `Release/` folders
and their weights.
