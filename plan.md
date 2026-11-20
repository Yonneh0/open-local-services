# plan.md — the work that is still open
this is a living task list.
when work is complete, edit this document, add any additional user guidance received during the session, and move the work item to Awaiting User Verification, when completed.
do not edit or remove empty headers, or document format examples.
working rules, standards, and the commit and push discipline live in `dev.md`; machine access specifics live in `C:\Projects\local.md`.
always read this file immediately before making updates: the user may change it at any time. never revert user changes: write around them.

## 1. TODO - not actionable yet.
these tasks are upcoming, but some of the details have not been firmly decided.
do not edit, or act on anything in this section, unless instructed by user

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

## 3. Awaiting User Verification
when work is complete, edit this document, add any additional user guidance received during the session, and move the work item here, when completed.

- LAN fleet awareness: startup sweep, peer table, header host picker
  Prompt: "implement lan awareness: on startup, api should attempt to reach out to all lan accessible ips, on OLS_PORT (with api key if assigned); and query the api server for hostname and status, and also surrendering it's own hostname, status, api key, etc; if another ols server is found. when receiving this request, the api server should update it's awareness table. this information should be cached/persisted in a .json file in data/ or something between runs ... it should intellegently keep tabs on all other ols servers on the lan, with it's same port and api key. add a combobox dropdown, to the header bar of each page, with the currently host selected, and all other online ols hosts on the lan. when selecting another host, it should forward the browser to that the same page, on that server directly (handling api key entering and stuff). before you make any changes- please explain this back to me with better wording"
  Follow-on guidance: sweep the actual configured networks at their actual netmasks, batched; once a server is met it is always asked again at its saved address; startup-only cadence; a hit joins both machines' tables (the answered hello records the asker); peerhood requires answering this machine's key; card carries identity + liveness (hostname, version, backend, port, uptime).
  Changes: api/PeerService.{h,cpp} new - sweep, handshake, data/peers.json persistence; OpenLocalServices.{h,cpp} - peer member, card provider, /api/peers and /api/peers/hello doors, the auth door now says peers:true; main.cpp and CMakeLists carry it. webui: common.js + style.css header picker with one-hop key carry, key-shim.js peers flag so pages of young servers ask nothing. Built on X1; deployed to MSI (its former cuda75 install now runs this dev build - its home shape returns with the next release). Verified through both machines' running doors: sweep discovery, known-peer re-probe from the saved book (via words tell which way a row was met), 401 on wrong key (nothing recorded), offline aging, persistence, picker in both headers, cross-machine jump carrying the credential. X1 restarted onto the new binary (its /api/service soft hand; an in-flight 33-minute job held the old process to its finish first - the hand waits for standing work), and the empty-book sweep alone re-found MSI. The dev serve at X1 and the release install at MSI now keep their own peer books.
