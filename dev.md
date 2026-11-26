# dev.md — working rules for open-local-services

Read before touching this repo. Situational awareness - what the machines are, their addresses, access,
availability - lives in the parent doc, `C:\Projects\local.md` (machine dossiers beside it); this file
carries the rules for working in this folder. Machines change faster than docs: trust probes over memory.

## the shape of the work

- one local server process (C++, a hand-written HTTP/1.1 server over the socket API in `src/Platform.h` - no
  cpp-httplib; the vendored httplib copies belong to the engines, not the server): one web UI, all API doors,
  one port, with the image, video, voice, and listening engines running beside it. pages are plain JavaScript
  in webui/ - no framework, no build step.
- build and start through the scripts as they stand: run.bat (Windows), run.sh (Linux). do not invent
  new build or deployment machinery.
- a webui/ folder beside a running server is served from disk: page and style changes are live on
  reload. server code needs a rebuild through the scripts; a release binary carries its pages from
  build time, so embedded pages update only with the next release build (build-rel).
- the same code must compile and behave the same on Windows and Linux; that pair of readings is the
  release gate, not either one alone.
- all code changes compile on, and are thoroughly tested on, X1: http://10.255.0.3:8765/ - never
  127.0.0.1. the key is in .env.

## servers have machine-level homes

- a permanent serve never starts inside an agent session or a background job - it dies with the
  session. homes: X1 = scheduled task ols-serve (SYSTEM, AtStartup, runs run.bat; log
  data\ols-serve.log); UM780 = systemd unit open-local-services.service; MSI = binary install with the
  ols-release task. restart a task or unit through its own door or the owner - never rehouse it.
- R9 serves the operator's inference: v31 across both RTX 3090s. It is a live box and is **not off limits**
  (operator, 2026-11-22); its serve is production - never reboot it or restart its task from an agent.

## every change gets committed

- commit frequently, and push every change to github.com/Yonneh0/open-local-services (branch master).
- before a commit, review the documentation the change touches and update it in the same commit: the
  README door map and prose when a door speaks, answers differently, or falls silent; plan.md when work
  moves through its sections; local.md when a machine's facts move. a commit carries the code and the
  truth about the code together - never commit a change that leaves a document lying.
- commit messages are complete sentences in the house voice that say what changed and why.
- version is the commit hash and nothing else; panels read rel <hash>.
- plan.md is the living ledger: read it immediately before updating it - the owner edits it at any
  time - and never revert the owner's changes, write around them. it is LF: edit it with an editor,
  never by script splicing. when work is complete, fold in the session's user guidance and move the
  item to Awaiting User Verification. keep change reports brief.

## coding standards

- code is readable, cleanly separated, properly documented; minimum comments, and none that repeat the
  line under them. wording states the plain thing forward.
- network, io, and resource efficiency are always weighed: this application loads and moves around
  100+ GB models, and simple inefficiencies multiply quickly.
- numbers on the pages are the machine's own statements, read from API answers. a page guesses nothing:
  where the server states no fact, the field is absent and the page says so.
- clocks keep honest time: an hour holds sixty minutes and never one more - a sixtieth minute carries
  into the hour, an hour is floored from what the minutes actually hold, and a pace fed by clocks and
  engine ticks it does not own is clamped to what the machine could plausibly spend.
- secrets and bulk stay out of the tree: .env, weights/, and data/ are ignored and stay that way.

## validating - the done gate

- nothing is done until it is proven through the running server in a real browser (mcp__browser__*)
  with a clean console; a compiled change is proven against its new binary, not the old one.
- state only what was observed. when readings conflict, go measure; never invent a mechanism to
  explain one.
- no subagents and no workflows: the work happens in the session.
- no sleeping, no delays over 5 seconds, no waiting on timeouts - long builds and long runs go to
  background tasks, and you poll them.
- temp files go in C:\Projects\TEMP and get cleaned up.
- the environment rules of local.md §0 apply while working here: no DSH built-in web search (web work
  is mcp__websearch__* and mcp__browser__*); pwsh on X1 is not elevated (one Start-Process -Verb
  RunAs when elevation is truly needed); emitted text can silently drop characters, so verify
  important writes through an honest reader.

first read: this file, then plan.md for what is open, README.md for the doors and configuration,
src/ for the services, and C:\Projects\local.md for the machines.
