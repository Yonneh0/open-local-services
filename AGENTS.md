# AGENTS.md

Working rules for anyone - person or agent - working on this repository.

## what this is

A single local server process (C++, cpp-httplib) that serves one web UI and all
API doors on one fixed port, and drives image generation, video, speech, and
listening engines linked or loaded beside it. The pages are plain JavaScript in
`webui/` with no framework and no build step.

## building and running

- Build and start through the scripts as they stand: `run.bat` on Windows,
  `run.sh` on Linux. Do not invent new build or deployment machinery.
- Long builds and the running server belong in background tasks. No sleeping,
  no waiting on timeouts; poll.
- A `webui/` folder beside the running server is served from disk, so page and
  style changes need no rebuild - they are live on reload. Server code needs a
  rebuild.
- The same code must compile and behave the same on Windows and Linux; that
  pair of readings is the release gate, not either one alone.

## working rules

- `plan.md` at the root is the living ledger of open work. Read it immediately
  before updating it; the owner edits it at any time. Never revert the owner's
  changes - write around them.
- Numbers on the pages are the machine's own statements, read from API
  answers. A page guesses nothing: where the server states no fact, the field
  is absent and the page says so.
- Keep secrets and bulk out of the tree: `.env`, `weights/`, and `data/` are
  ignored and stay that way.
- Commit frequently. Commit messages are complete sentences in the house voice
  that say what changed and why.
- Validate through the running server with a real browser before calling
  anything done, and keep the browser console clean.

## first read

`README.md` for the doors and configuration; `plan.md` for what is open;
`api/` for the services, which each own their state and answer through thin
doors in `OpenLocalServices.cpp`.
