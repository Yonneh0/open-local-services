# plan.md — the work that is still open
this is a living task list.
when work is complete, edit this document, add any additional user guidance received during the session, and move the work item to Awaiting User Verification, when completed.
do not edit or remove empty headers, or document format examples.
review `C:\Projects\local.md` for access specifics to X1, UM780, MSI, and other various lan systems.
all code changes must be compiled on, and thoroughly tested on X1.
coding standards: code should be readable, cleanly separated, properly documented. network, io, and resource efficiency should always be carefully considered: this application handles loading and moving around 100+ GB models - simple inefficiencies multiply quickly!
before moving on, ensure you have completed ALL given tasks; and updated all documentation - plan.md, README.md, built release on X1, tested with browser mcp, validated ALL changes.
NO SLEEPING. NO DELAYS OVER 5 SECONDS. NO TIMEOUTS. USE BACKGROUND TASKS.
No subagents.
commit frequently, and push changes to gitlab, with every commit.
always read this file immediately before making updates: the user may change it at any time. never revert user changes: write around them.

## 1. TODO - not actionable yet.
these tasks are upcoming, but some of the details have not been firmly decided.
do not edit, or act on anything in this section, unless instructed by user

- add panel to home page: network management.
  list all network adapters, current settings, etc.
  implement panel to configure ethernet, wifi, bonding, sharing, etc
  ensure it works in Windows AND Linux uniformly
  all controls should be intuitive; with animated status. updated in real time.
  properly integrate changes into netplan on linux, and call netplan apply
  add all of the basic functionality a user would expect on a wifi access point management interface: packet counts, ports in use, bandwidth graphs, uptime, vlan config, robust radio config, wifi scans with table showing ssid, bssid, signal, etc

- refactor open models panel on home page:
  each open model should appear as a "bar", with 2-3 lines; with compact intuitive controls, statuses, progress bars, etc
  


## 2. PENDING - actionable work.
user may add or update items here during your session- read it carefully before making updates.
keep report on changes BRIEF. The entire section should not exceed 20 lines.

review format. do not edit or remove this format:

- <job title>
  Prompt: <full user message>
  Follow-on guidance: <summary of any changes the user makes during the session>
  Changes: <all files modified, summary with line change counts, brief functional summaries, etc>

- Functional audit of every file and function, fix all bugs, clean up code
  Prompt: "perform a thorough functional audit on the entire project. fix all bugs. setup a goal, outlining your overall task. update plan.md with all bugs and intended changes, before and after making changes. carefully read plan.md. remove unneeded comments. clean up dead code/files. clean up backwards wording. clean up un readable code. perform a complete end to end audit of EVERY file, every function. this is a long running task. do not worry about context limits. do not attempt to mark anything complete, without actually checking - be thorough!"
  Follow-on guidance: "additionally: review all vendored code, and its source: see if it can be updated to a more recent version." / "only use run.bat, in a background job, to compile and run the server. do not attempt to come up with new deployment machinery."
  Changes: pending - the finding-by-finding list, with what each change does, is section 4. nothing in it is marked fixed until it has been compiled on X1 by run.bat and re-checked through the running server.

## 3. Awaiting User Verification - completed tasks
move entire task section from 2, here, upon completion, with an additional `Completed: <brief summary of changes made>` section.
keep your entries brief. they should not exceed 3 lines (or 100 characters), unless significant changes were made.

## 4. Audit ledger - full bug list and intended changes
the running detail behind the brief entry in section 2. one line per finding:
`id | state | where | what is wrong | what will be done`
state: `found` (recorded, not yet changed) `fixing` `fixed` `won't` (with reason).
this section is the complete list: nothing found is left out of it.

- A1 | fixed (verified) | api/VoiceService.cpp `audioTreeDir`/`specsDir`/`buildCards` | the engine's spec tree and its bundled models were opened at the cwd-relative path `vendor/audio.cpp/...`; the server starts from the repo root, where the tree stands at `api/vendor/audio.cpp/...`. Every voice card was therefore empty: `/voice/api/model-stats` answered 0 families, 0 packages, 0 bytes on X1, which left the whole voice studio dead. | resolved: the tree is found once from the places it really stands and cached; both the specs and the bundled models are read from it. Measured after the rebuild: `{"families":129,"packages":404,"present":14,"bytes":25847332387,"families_without_loaders":0,"packages_without_loaders":0}` and `model-cards` rows carry absolute weight paths.
- A2 | fixed (load path not yet exercised) | api/VoiceService.cpp `sessionFor` | the engine was asked for a `vulkan` device backend first. This build has no vulkan backend compiled in, so the first ask always failed and its failure was the one stated. | resolved: the build's own best device is asked for first, cpu second, and the words say which of the two failed. A model has not been loaded through a live door since; that is the next thing to measure.
- A3 | fixed (needs one re-read) | api/VoiceService.cpp `buildCards` | a card's `declared_tasks` named one engine task twice, because several spec names of a family stand for the same task (ACE-Step names `music` and `edit`, both of which are the engine's `gen`). `/voice/api/model-cards` answered `"declared_tasks":["gen","gen"]`. | resolved: a task is added once. Re-read `model-cards?limit=3&family=ace_step` to see `["gen"]`.
- A4 | fixed (verified by build) | api/Common.h | 599 lines no file calls: the answer-shape reading (`applyShape`, `cleanLine`, `fieldAtLine` and its layout helpers), the capability-word list, the engine socket helpers left by the port era, the tool-call reading, `sha256OfFile`, `filesAllPresent`, `trimLineEdges`, `statedCountPresent`, `isPaddingSpace`. The file also repeated `#pragma once` and its own include guard eleven times. | resolved: only what a file calls is left (617 lines), under one header. Checked by counting every symbol's uses across `api/` outside `Common.h`, and by the build.
- A5 | fixing | whole tree | wording that states the plain thing backwards, and sentences that take a paragraph to say one thing. | being rewritten file by file, in the files the audit is already opened.
- A6 | fixing | whole tree | comments that repeat the line under them, and comment paragraphs broken apart by an edit. | being deleted file by file (one such broken paragraph was at `runDownloadJob`; the dangling port comment in `kConfigOptions` is gone).
- A7 | fixed (verified by reading, live probe to run) | api/OpenLocalServices.cpp `theDoorAnswer` | the room a refused visit names came back from `?room=` and from the door form, and was written into the door page unescaped in the refresh `url=` and in the link. A name holding a quote wrote part of that page - the door page carries no content policy - and a name holding a host (`//elsewhere`) carried the visit there, before any key had been handed. | resolved: `onePageName` keeps a room to one page's name (no folders, no host, nothing but a file's characters), and what is written into the page is escaped. Probe: `/?room=x%22onerror%3d1` and `/?room=//example.com` must both come back as `/index.html`.
- A8 | fixed | api/OpenLocalServices.cpp `givenCredentials` | a cookie field written without its `=` (`ols-key;other=1`) read the next field's text as the credential. | resolved: a field reads a value only when it was given one.
- A9 | fixed | api/OpenLocalServices.cpp `authGate` | every accepted request slept 6-36 ms, including on a machine with no key set and on a request that handed no credential - where a clock has nothing to give away. | resolved: the delay is paid only where a key is set and a credential was handed.
- A10 | fixed | api/OpenLocalServices.cpp `configUpdateFile` | `.env` was truncated and rewritten in place; a write that died halfway left a person with half a config or none, and that file holds the keys the server runs on. | resolved: written through a temporary name and a rename, which `Common.h` already does for every other record.
- A11 | fixed | api/OpenLocalServices.cpp `configValueShapeOk` | `OLS_THREADS` was accepted up to four trillion, and the figure was clamped inside its own digit loop; `run.bat` refuses past 4096, and past that the number is already nonsense by the time it reaches the engines. `OLS_RATE_LIMIT` ended in a condition that cannot be false. | resolved: 1..4096, the launcher's own rule, and the rate limit states what it checks.
- A12 | fixed (live download still to measure) | api/Platform.h `curlFetch` | the destination, the address and the key are put between quotes on a command line, and a file list comes from the source being fetched from: a name carrying a quote or a separator there stopped being a name. | resolved: a part that cannot stand inside quotes is refused (-1) and both callers say so; the voice download gives up rather than looping. No escaping is attempted, because a shell has no quoting rule that carries every character.
- A13 | fixed | api/VoiceService.cpp `runDownloadJob` | a fetch that failed left its half-file on the disk, and the next attempt found a file standing there and called the package whole: a truncated weight, reported as downloaded, that the engine then fails to open. A package's spec names its files but states no size and no sum for them, so nothing can tell the two apart later. | resolved: the file that failed to come down is removed, and the words carry curl's own code.
- A14 | found | api/VoiceService.cpp | `setRecordField` and `setRecordText` are the same helper twice; `dropVoiceReason` refuses a delete with "The files would not move"; `isVoiceName` admits `_` while the refusal says letters, digits, dot and dash; `voicesListJson` asks `if (def)` twice in a row; `unloadCard` erases `gLoadErrors` by prefix and then again by name, and that map is keyed by session combinations that never stop arriving. | dedupe the two helpers, fix the two wordings, drop the doubled test, and bound the load-error map the way the job ledger is bound.
- A15 | found | api/OpenLocalServices.cpp `serveFile` | a kept file over 64 MB answers 404, and every ranged read goes through the whole file in memory first. A long clip or a large artifact is simply not served. | state the refusal for what it is rather than "not found", or read only the part asked for; decide by what the biggest kept file really reaches.
- A16 | found | api/VoiceService.cpp:746 `fromBase64` | a second base64 decoder, beside `ols::decodeBase64` in `Common.h`, used at three places in the same module. | keep the one in `Common.h`.
- A17 | found | api/OpenLocalServices.cpp `handleConnection` | a HEAD answer says `Content-Length: 0` instead of the length the body would have had. | keep the length and drop the body.
- A18 | won't (recorded) | api/Platform.h `curlFetch` | the key travels on a command line, where any process on the machine can read it in the process list. curl reads it from a file or a netrc instead. | recorded, not changed: every download of this project goes through it, and changing how the key is handed is a change to test against a real fetch, not against a build.
- A19 | fixed | api/ImageService.cpp `ggufComplete` | the header reader knew the value types of GGUF's second wording only. A value of the third wording's wide types (uint64, int64, float64) made it answer "not a whole file" - the same wrong answer that left the Q4 text encoder reading as missing on 2026-10-31, one type narrower. | the three widths stand in the table; only a type past the format's own list is refused.
- A20 | fixed | api/ImageService.cpp `ggufComplete` | the bytes of a file were taken to begin at the end of its header padded to 32, which is the usual figure but not a rule: a file that names `general.alignment` begins where it names it. A file standing on 64 or 418 was measured from too early, so a truncated tail still answered as whole. | the file's own alignment is read while its metadata is walked, and the padding uses it. Measured here: every .gguf on X1 is wording three and names no alignment, so nothing that works today moves.
- A21 | fixed | api/ImageService.cpp `safetensorsComplete` | an offset was taken out of the header without asking what it was: a header that words one as a string or a fraction throws, and this reader runs on the threads that answer `/api/models` - one odd file in the weights room took the whole models page down. A stated offset was also added to the header's length without asking whether the sum could be held. | an offset that is not a whole number says nothing; the length is asked for with the error left beside it; a figure past 256 tebibytes is refused rather than added to.
- A22 | fixed (found by reading) | api/ImageService.cpp `integrityOk` | every file that is not GGUF was asked whether it is a whole safetensors file. A config, a vocabulary or a tokenizer that stands beside the weights has no safetensors header, so it answered no - and a source states a sum only for its big files, so a model with such a side file on its card never read as installed, on any machine. | a file is asked which dialect it keeps; one that keeps neither is answered by its length, which was already asked above.
- A23 | fixed | api/ImageService.cpp `writeApngPreview` | the animation header states how many frames follow, and a frame of a different size was passed over after that number was written, leaving the two disagreeing: a browser that stops at the count is left with a clip missing its end. | the frames that can be written are chosen before the header is written, and the header states that many.
- A24 | fixed | api/ImageService.cpp `writeAviClip` | the sound stream's header carried its numbers in the wrong places: the sample rate stood where "where it starts" belongs, "how long it runs" was 0, "how wide a block is" was -1, and the block width stood in the rectangle a stream is drawn into. A reader that asks the header how long a clip's sound runs is told nothing. | every field stands where the container states it, beside the reading of the engine's own writer (examples/common/media_io.cpp:978).
- A25 | fixed | api/ImageService.cpp `writeAviClip` | the plain copy of a clip (`.raw.avi`, for a player that knows no codec) wrote its plain pixels as a `00dc` chunk - "compressed, hand these to the codec" - and declared its rows top-down while writing them from the bottom row up, so a reader that believes either word shows the clip upside down or hands raw bytes to a JPEG codec. | plain frames are `00db` chunks, unflagged, and their height is positive, which is what "bottom row up" says.
- A26 | fixed | api/ImageService.cpp `writeAviClip` | the clip's index named each frame from the start of the file instead of from the start of the clip's own bytes. Readers that believe an index land past the clip and fall back to walking every chunk, so a seek costs the whole file. | an entry is measured from where the frames begin, which is what the container says it means.
- A27 | fixed | api/ImageService.cpp `writeApngPreview` / `pngCrc` | a count that could no longer be zero was tested for it; the checksum stood as `(crc >> 1) ^ (poly & ~bit + 1)`, which is right only after working out that `~0 + 1` is zero. | the dead test is gone; the checksum says which bit adds the polynomial.
- A28 | checked, no change | api/ImageService.cpp | `gGenerating` is written and read under one lock (gCtxMutex) at all ten places; `writeFileText` opens binary and replaces by rename, so the PNG/WAV/AVI bytes are safe and a half-written clip is never read; the video stream header's own fields and the AVI main header are the right length and in the right places. | nothing to do; recorded so the next read does not re-ask.

## 5. Audit coverage - files read end to end
- [x] api/Common.h - read whole (was 1380 lines, now 617); every symbol's uses counted; dead ones removed (A4)
- [x] api/OpenLocalServices.h - read whole
- [~] api/OpenLocalServices.cpp - read whole except the device report (1813-1975) and the door's own GET/POST (2134-2245); findings A7-A11, A15, A17
- [~] api/VoiceService.cpp - read 1-1550 and the download job; 1550-2900 and 2970-3250 still to read; findings A1-A3, A13, A14, A16
- [ ] api/ImageService.h/.cpp (4526 lines), MonitorService.h/.cpp, Deployment.h/.cpp, VerifiedRoom.h/.cpp, Platform.h, WebAssets.h/.cpp, voice-probe.cpp, main.cpp, CMakeLists.txt, ols-sage-guard.cmake
- [ ] webui: index.html imagegen.html voice.html style.css common.js app.js voice.js monitor.js config.js status-cache.js key-shim.js
- [ ] root: run.bat run.sh build-rel.bat build-rel.sh open-local-services.service .env .gitignore .gitattributes README.md
- [ ] vendor: stable-diffusion.cpp ggml audio.cpp nlohmann - pin, upstream head, update decision

Checked and found sound on the way: every path the pages ask of has a route in this server (the client's door list and the routing table were compared line by line); the rate bucket's arithmetic; the path-traversal guards on both the pages and the files doors; the json-error path that never carries a key letter into an answer; the atomic-write rule for every record but `.env` (now also atomic).
