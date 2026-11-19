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
  after implementing, conduct a full end-to-end test, locally, using the browser mcp.


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
  Follow-on guidance: "additionally: review all vendored code, and its source: see if it can be updated to a more recent version." / "only use run.bat, in a background job, to compile and run the server. do not attempt to come up with new deployment machinery." / "clean up plan.md. remove completed verified steps" / "clean up plan.md - validate all claims of completion; and remove finished work/logs. ensure only instructions, and pending/todo items remain."
  Changes: open findings are section 4; completed lines are removed once verified through the running server. nothing here is marked fixed until it has been compiled on X1 by run.bat and re-checked through the running server.

- raise the vendored image engine and audio.cpp toward their source heads, rebuild, re-benchmark
  Prompt: "additionally: review all vendored code, and its source: see if it can be updated to a more recent version."
  Follow-on guidance: none.
  Changes: none yet. the vendored engine's audio.cpp lags its source by ~200 commits (CJK long-text chunking for several engines, nan-as-token sampler fixes, a loader that asks for less memory at once); the engine's vision side lags ~60 (big-vision resize, batched conditioning, flux3 cached graphs). run.bat now rebuilds the engine incrementally, so a bump is one sync plus one build. nlohmann/json.hpp is 3.12.0, current; nothing to do.

## 3. Awaiting User Verification
when work is complete, edit this document, add any additional user guidance received during the session, and move the work item here, when completed.

## 4. Audit ledger - full bug list and intended changes
the running detail behind the brief entry in section 2. one line per finding:
`id | state | where | what is wrong | what will be done`
state: `found` (recorded, not yet changed) `fixing` `fixed` `won't` (with reason).
this section lists what is still open; lines for findings completed and verified through the running server are removed.

- A5 | fixing | whole tree | wording that states the plain thing backwards, and sentences that take a paragraph to say one thing. | being rewritten file by file, in the files the audit is already opened.
- A6 | fixing | whole tree | comments that repeat the line under them, and comment paragraphs broken apart by an edit. | being deleted file by file.
- A12 | fixed (live download still to measure) | api/Platform.h `curlFetch` | the destination, the address and the key are put between quotes on a command line, and a file list comes from the source being fetched from: a name carrying a quote or a separator there stopped being a name. | resolved: a part that cannot stand inside quotes is refused (-1) and both callers say so; the voice download gives up rather than looping. No escaping is attempted, because a shell has no quoting rule that carries every character. still open: one real download measured end to end through the door.
- A14 | found | api/VoiceService.cpp | `setRecordField` and `setRecordText` are the same helper twice; `dropVoiceReason` refuses a delete with "The files would not move"; `isVoiceName` admits `_` while the refusal says letters, digits, dot and dash; `voicesListJson` asks `if (def)` twice in a row; `unloadCard` erases `gLoadErrors` by prefix and then again by name, and that map is keyed by session combinations that never stop arriving. | dedupe the two helpers, fix the two wordings, drop the doubled test, and bound the load-error map the way the job ledger is bound.
- A15 | found | api/OpenLocalServices.cpp `serveFile` | a kept file over 64 MB answers 404, and every ranged read goes through the whole file in memory first. A long clip or a large artifact is simply not served. | state the refusal for what it is rather than "not found", or read only the part asked for; decide by what the biggest kept file really reaches.
- A16 | found | api/VoiceService.cpp:746 `fromBase64` | a second base64 decoder, beside `ols::decodeBase64` in `Common.h`, used at three places in the same module. | keep the one in `Common.h`.
- A17 | found | api/OpenLocalServices.cpp `handleConnection` | a HEAD answer says `Content-Length: 0` instead of the length the body would have had. | keep the length and drop the body.
- A32 | found, not changed | api/VoiceService.cpp `loadEngine` | the engine load is serialized by `gModelMutex` on pain of an engine-side crash. The model it guards lives in the engine's own device map, which the shared load could populate under one name; a real fix touches the vendored loader, not this file. | leave the serialization as the honest guard; record where the lock really belongs.
- A50 | fixed (rejection wording unexercised live) | api/VoiceService.cpp `loadEngine` | an engine refused for its weight file, and the door answered with one sentence that belonged to a missing file. | when a read fails, the record now keeps the loader's own words beside the refusal. Proven: a weight read by a path that is not a file was refused through the door with "Not a directory" carrying the loader's own sentence. still open: one live rejection read back through the new lookup.

## 5. Audit coverage - files read end to end
not yet read line by line:
- [ ] webui/ (15 js files, ~10 700 lines) - route paths and the request builder were read closely while fixing A37 and A41, and the whole file was read wherever an audit finding pointed at it, but the pages have not been read end to end line by line.
- [ ] root config/script files not yet read whole: .clang-format, .editorconfig, .gitattributes, .gitignore, .gitlab-ci.yml, .gitmodules, .gitignore for weights.
