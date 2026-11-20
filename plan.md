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

- Git up the MSI and UM780 against the github repo and run build-rel on both
  Prompt: "on MSI and UM780, setup git, to pull from that repo, with \"git pull\", and get it working WITHOUT spoiling the existing weights or data folders on either system. run build-rel on both"
  Follow-on guidance: "that is for after your current task" - queued behind the clock work.
  Changes: none yet. both machines hold working trees from bundles with no remote; weights/ and data/ are gitignored on both, so fetch/reset touches neither. UM780: remote + ssh identity for github, pull to tip (its tree is dirty with the five scp'd files - reset is the move), remember chmod +x run.sh build-rel.sh after any checkout (the windows-baked tree records 644), then ./build-rel.sh. MSI: existing git tree at its project folder (local.md 5), key identity for github, git pull, then build-rel.bat in the cuda 75 shape - long build belongs in a scheduled task with a log file, per local.md.

## 3. Awaiting User Verification
when work is complete, edit this document, add any additional user guidance received during the session, and move the work item here, when completed.

- The blinking 12:00 keeps a random heartbeat
  Prompt: "next up- update the clock blink rate slightly: when blinking 12:00, make it on for a random full interger duration between 1 and 5s, (1s, 2s, 3s, 4s, or 5s) off for 1s"
  Follow-on guidance: none.
  Changes: the corner clock's fixed 3s/900ms interval became a self-scheduling beat: awake a whole number of seconds drawn anew 1 to 5, dark a full second, drawn afresh each round. The queue's colon keeps its three-second rhythm and 900ms dark untouched (the user's beat belongs to the hour nobody set); a set clock idle still never blinks - and a blink already on its way has no right to fall on a clock tapped set in the meantime, so the mode is re-asked as it strikes. Observed through the browser on both machines: awake draws of 3, 2, and 4 seconds and darks measured 1008/1004/1010 ms; set-clock watch of 14 s stayed lit; console clean. common.js only - live from disk on both, no rebuild.

- The browser's tab names the machine and its shape
  Prompt: "quick webui fix: update all pages to display hostname and platform in title, instead of \"open-local-services\"; retain separate icon depending on selected tab. tabs should read `X1 (win-vulkan)`, `MSI (win-cuda)`, `UM780 (linux-cuda)`"
  Follow-on guidance: "platform should contain basic version info: win11, ubuntu-server-26.10, backend too - cuda architecture (cuda70, cuda75, cuda86), likewise for vulkan, cpu, etc" / "hostname should be capitalized on both platforms".
  Changes: /api/system gained hostname, os, and backend - the machine's own three words, so the tab reads X1 (win11 vulkan1.4), UM780 (ubuntu-server-26.10 cuda70), and the still-old-binary MSI keeps its written title until its server is rebuilt (the page states nothing the server did not). hostname: GetComputerName/gethostname, capitals either platform. os: win11/win10 by the kernel's own build number through ntdll (the manifestless version api lies about being ten); linux takes ID and VERSION_ID from /etc/os-release and calls itself -server where no desktop session stands. backend from the build itself (CMake defines shape and first CMAKE_CUDA_ARCHITECTURES): cuda70/cuda75/cuda86, cpu-x86_64 for the no-device shape, and vulkan asks its own loader the highest version the machine speaks (vulkan1.4) - an early 27/17 bit decode read vulkan0.32 and was fixed to the 22/12 packing the header states. common.js mountTitle writes document.title once from the first /api/system read; the pages' per-page icons are untouched and verified. built and live on X1 (run.bat) and UM780 (run.sh) - X1 and UM780 browser tabs read the new titles; console clean.

- The corner clock serves the queue: amber, counting down, colon blinking
  Prompt: "next up, slightly repurpose the blinking clock in the top right: while jobs are processing and queued, it should change color to orange; and it should display the hours and minutes until the entire queue is completed, blinking the \":\" every 3 seconds instead of the numbers. once the queue is complete, and the server is idle, it should change normal."
  Follow-on guidance: none.
  Changes: while any job runs or stands the face turns amber and paints H:MM - all four digits, leading zeros - and the three-second blink moves from the digits to the colon; queue empty, the clock is itself again (unset 12:00 blink or its set hour) within a poll. The number is the machine's statement, not the page's guess: /api/monitor gained a queue block (MonitorService queueBlock + timings provider wired through OpenLocalServices) - each standing job weighed by the pace it has itself held, else the ledger mean across rows of its model (loads and generations), else its engine's kept-completion mean; a job no yardstick reaches counts unmeasured, and where nothing could be weighed left_s is absent and the face burns with dark digits and blinking colon rather than inventing a time. The ledger is read only while a queue stands; the clock's 4s read shares the page polls through the fetch cache. Proven live on both machines: X1 showed 00:03 from its rich ledger with amber colon mid-blink and 12:00 restored after drain; UM780 showed queue mode from its first-ever job and returned home on empty; consoles quiet. Also along the way: the machine's git found a home - pushed to github.com/Yonneh0/open-local-services (public), README carries the clone doors, AGENTS.md states the working rules once; the owner's front-door disclaimer took its own commit.

- The machine panel states what the hardware is: sticks and clocks, both machines
  Prompt: "next up: index, machine panel: / remove open/running graph and \"engine memory\" graph / add actual system memory bandwidth and rating text below system memory graph / add actual gpu processor speed, and memory speeds below associated graphs"
  Follow-on guidance: "I brought up um780 just for you! copy the latest ols over, compile it, and end to end test all of the recent changes. refresh your memory of c:\projects\local.md" / "ensure you transfer the missing weights to um780 over the lan- it will be missing some of the video stuff"
  Changes: deleted the open·running and engine memory gauges (and their orphaned helper and tint rule); panel() gained a foot line - 10px mono, dimmed below every chip, ellipsized, quotable - which the memory panel wears as the stick inventory its machine states (DDR5-5600 · 2 × 48 GB · 128-bit · ≈89.6 GB/s on X1, 2 × 16 GB on UM780) and the gpu panel wears as the card's own NVML clocks when the driver answers (core 1260 MHz (max 1380) · mem 1107 MHz (max 1107) on UM780's Tesla) or the system bus the card's memory truly runs on where it does not (X1's 890M). backend: new api/HardwareInfo.{h,cpp} - windows reads WMI Win32_PhysicalMemory through COM; linux walks /sys/firmware/dmi/tables/DMI the way dmidecode walks it (string areas ended by the whole zero run; size words in the unit their top bits name; fields only from records long enough to hold them), every offset proven against UM780's dmidecode word for word before shipping - an earlier probe's "shifted firmware" proved to be the probe's own off-by-two walk, not the table. gpuClocks() soft-loads nvml and says nothing where no card answers. /api/system gains memory_hw and gpu_clocks, absent when not known. three gcc-only compile faults fixed en route (ImageService ternary, dlopen const, __cdecl). UM780 deployed from a git bundle at tree tip, built by run.sh (CUDA sm_70), and run as root; its 97.5 GB of missing weights - the MiniMax-H3 video family and chat/read additions - streamed over the LAN and verified byte-identical. both machines' indexes, both studios, age rows, and consoles verified through the browser; X1 binary rebuilt to the tip.

- Panel names set as plates on their panels' top edge
  Prompt: "update the the panel headers a bit: for example, on index.html, the text \"this page\", at the top of the first panel / increase font size by 100%, increase spacing between characters by about half a space; use a bolder, or fuller font- use some creative smarts here please :) / move the actual text up, so the center of the text lines up with the top edge of the panel- making the panel form a kind if \"strikethrough\" thru the center of the text / add a sharp contrasting border to the header text- but darken the actual text closer to the panel background. the text should be easily readable- but the top edge of the panel should show through slightly / add a colored dot or something before the header name, for easy visual recognition / not all tasks will be styling, or entirely html like this- don't forget about the api stuff for your following tasks :)"
  Follow-on guidance: "there is no need to reload the server to make webui changes; unless it is actually being recompiled. it pulls live changes from webui folder." / "remove the background mask from behind the letters; reduce the size of the dot to about 20% of what it is- and move it more to the left, so it's :centered: on the top-left corner of the panel."
  Changes: webui/style.css (one block) - a panel's first name is set as a plate straddling the panel's top rule: 20px (double), weight 700, 0.3em tracking (about half a space at that size), fill darkened toward the panel (42% fg into bg2) kept readable by a sharp 1.25px --fg text-stroke painted under the fill; no mask behind the letters, so the rule strikes clean through them. The mark is a 2px accent dot with a faint halo, set at the panel's own top-left corner. A second name further inside a panel (settings' "the model a run draws with") keeps its old inline voice. .panel gains position:relative (no absolutely-positioned descendant re-anchors: every one already has a nearer positioned ancestor); #left/#studio stack as flex columns and main/#left keep 6px of top air so plates never clip or cross the panel above. All three pages verified through the running server; webui-only, no rebuild.

- The stove's colon, and the machine's ages beside the clock
  Prompt: "fix the flashing clock, so the : does not flash with the numbers- like an actual stove clock" / "in the header row, to the left of the flashing clock, add 2 rows of text, stacked, aligned to the right: first row: \"time since last bug fix: \", with a timer counting up from the api's compile date / second row: \"time since last new bug: \", with a timer counting up from the api's newest webui file (html, js, css) / the second row should probably have some backend integration- with proper caching/updates/etc / timers should be updated every 60s (every minute), and only have minute-scale granularity. again, use some creative smarts here- use your best judgement on making it look good, and blend in"
  Follow-on guidance: (none)
  Changes: webui/style.css - the unset blink dims only .c7s segments, the colon burns straight through (committed in 5889ae8). New: a .age-stack of two right-aligned 10px mono rows beside the clock - the counts keep the clock company as its footnotes, green for the fix and red for the fresh fault, tabular minutes that never jiggle. common.js mountAgeRows asks /api/system once a minute and renders <1m / 5m / 5h 04m / 4d 06h; each row's tooltip names its exact source moment and file. Backend: write-build-moment.cmake now embeds buildMomentSeconds() beside the spelled moment; /api/system gains compiled_at, and webui_at + webui_file from a newest-mtime walk of the folder the pages answer from (.html/.js/.css, re-walked at most once a minute, name kept; a server on its embedded copy reports its compile moment). Both ages verified live on all three pages, consoles clean; the file-clock epoch crossing (1601 on MSVC) proven against the wall clock.

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
