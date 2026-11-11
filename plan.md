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

- A1 | found | api/VoiceService.cpp:934,1070 | the engine's spec tree and its bundled models are opened at the cwd-relative path `vendor/audio.cpp/...`; the server starts from the repo root, where the tree is `api/vendor/audio.cpp/...`. every voice card is therefore empty - `/voice/api/models`, `model-cards`, `model-stats` all answer 0 families/0 packages on X1, whole voice studio dead. | resolve the engine tree once from the places it really stands (repo root, api/, beside the binary), use it for both the specs and the bundled models.
- A2 | found | whole tree | unreadable wording: comments and some identifiers state the plain thing backwards ("a card whose files are not on this machine yet", "the words this machine's manager asks will be refused"). | rewrite the wording of every comment in the files touched, plain and forward, no poetry; keep comments that carry a fact or a measurement.
- A3 | found | whole tree | comments that repeat the code underneath them, and commented-out leftovers. | delete them; keep only what a reader cannot get from the code.
- A4 | found | whole tree | dead code and dead files: functions no caller reaches, unused struct members, stale build droppings under `api/` and stray logs in the tree. | list per file, remove what is provably unreferenced, leave vendored trees alone except where noted.

## 5. Audit coverage - files read end to end
- [ ] api: Common.h Platform.h WebAssets.h/.cpp OpenLocalServices.h/.cpp ImageService.h/.cpp VoiceService.h/.cpp MonitorService.h/.cpp Deployment.h/.cpp VerifiedRoom.h/.cpp voice-probe.cpp main.cpp CMakeLists.txt ols-sage-guard.cmake
- [ ] webui: index.html imagegen.html voice.html style.css common.js app.js voice.js monitor.js config.js status-cache.js key-shim.js
- [ ] root: run.bat run.sh build-rel.bat build-rel.sh open-local-services.service .env .gitignore .gitattributes README.md
- [ ] vendor: stable-diffusion.cpp ggml audio.cpp nlohmann - pin, upstream head, update decision
