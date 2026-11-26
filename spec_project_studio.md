# spec_project_studio.md — Project Studio

Status: design v1 (2026-11-22). The development plan lives in `plan.md` §2 (folded there 2026-11-26 from the retired `plan_project_studio.md`).
This spec is the contract; where code and this file disagree, one of them is wrong and must say so.

---

## 0. What it is

Project Studio is a fourth tab of this server: a **portable, local Git repository manager** — its own
instance of what GitHub/GitLab do for a team of one, tied to no vendor. Projects live under `data/projects/`,
one subfolder each. The studio browses their code and docs, tracks issues that are pinned to files, runs
builds like a CI panel, keeps a full pull-request/approval workflow, **is itself a git remote** (other
machines clone/push over smart-HTTP or ssh), and pushes out to GitHub/GitLab when asked. Everything the UI
does has an API door behind it; every answer is the machine's own statement; everything the browser holds
is cached and re-fetched only when the server says something changed.

Design posture, inherited from the house: one process, one port, plain pages, no framework, secrets stay
in `.env`, nothing on a page is guessed, refusals come back as words. Design posture, new here: **the git
layer is ground-up**, like the HTTP server — no libgit2, no shelling to `git.exe` for anything the studio
can do itself; and **TLS is native**, with the OpenSSH transport vendored rather than borrowed from the OS.

### 0.1 Verified environment facts (probed 2026-11-22; audits of src/, webui/, vendor/)

| fact | value | where |
|---|---|---|
| HTTP server | hand-written HTTP/1.1 over `ols::platform` sockets — **no cpp-httplib** (its vendored copies belong to the engines) | `src/Platform.h`, `OpenLocalServices.cpp` |
| routing | one if-chain `route()` at `OpenLocalServices.cpp:2349`; static `.html/.css/.js` auto-served (`webAssetForPath` :397-410) | |
| body cap | 2 MiB (`kMaxBodyBytes`, :1210), non-SSE responses fully buffered, `Connection: close` | |
| streaming | SSE only today (`/api/jobs/<id>/events`); chunk primitive `OpenLocalServices::writeStreamChunk(void*, frame)` is public static (`OpenLocalServices.h:85`) | |
| auth | api key via `X-Api-Key` / `Bearer` / `?key=` / cookie `ols-key`; gate runs at top of `route()`; 3 misses → silence 30 s | :486-523, :805-835 |
| freshness | per-path `Cache-Control: public, max-age=N` from `apiFreshSeconds()` (:581-617); measured live: `/api/monitor`, `/api/system` = 2 s | verified by curl this session |
| JSON | nlohmann/json 3.12.0 vendored at `vendor/nlohmann/json.hpp` | `src/CMakeLists.txt:229-233` |
| jobs | in-memory records, 200 kept, SSE events, cancel/reorder; durable state = per-artifact `.json` sidecars + atomic temp+rename writes (`Common.h:167-205`) | ImageService/VoiceService pattern |
| subprocess | popen precedents (`deploy::ask`, `net::runShell`); refuse-don't-escape quoting (`platform::quotableInShell`, `Platform.h:227-249`) | |
| tabs | exactly 3, one array: `NAV` at `webui/common.js:1366-1370`; badges fed by `engineBadges` (:268-301) | |
| caching client | `status-cache.js`: localStorage `ols-api-cache-v1`, server max-age as TTL, POST clears the family (:362-363), no etag for JSON; static files DO have etag+304 (`OpenLocalServices.cpp:1625-1664`) | |
| action idiom | `POST` → `{did, words}` → log + toast → re-read (network.js `ask()` :104-128) | |
| git on machine | 2.56 present — used only as test oracle and CLI interop check, never called by the server | probed §1 audit |
| serve identity | process runs elevated in Session 0; account unreadable from this shell → **all ssh identity must be explicit**, never `%USERPROFILE%`-dependent | audit §11 |
| data dir | `data/` gitignored (load-bearing); precedent for flat + `.json` sidecars and root-level state jsons | `.gitignore:41`, audit §8 |

### 0.2 The user's requirements, numbered for traceability

R-01 projects folder under `data/projects/`, one subfolder per project
R-02 header project dropdown; changing it re-renders the page to that project
R-03 [+] button → quick slide-out dialog: create new (with options) **or** attach a remote
R-04 accept GitLab/GitHub/other clone link, optional PR/branch selection; remember as source for updates
R-05 Code tab: README/doc parsing, interactive file list, project facts (languages, architectures, purpose,
     license, description, planned features, dependencies), CI/CD-style build control
R-06 Issues: issues tag specific files; viewing a file shows all its known issues
R-07 AI-assistant navigability through the browser (ids, stable DOM, machine-readable views)
R-08 functions as a git remote / git server for others
R-09 full pull-request system with approvals, managed from the same webui; branch list with status, diff
     links, PR links
R-10 every UI action has an API handler; instant effect; feedback comes from the real server answer
R-11 everything cached in browser localStorage; downloaded only when changed (treat the wire as expensive)
R-12 push to GitHub/GitLab via ssh or username:password, with validation/help (green check on a valid key)
R-13 fill the gaps — more functionality where cheap
R-14 careful UI/layout design: every control considered, functional, sensible placement; code view has
     syntax highlighting and an outline tree; large files get efficient patching
R-15 vendor OpenSSH; full TLS on the API with modern certs + automatic self-signed generation
R-16 **the editor is an IDE**: the file editor reaches feature parity with VS Code's *editor itself* (not its
     plugin ecosystem) — full syntax highlighting, beautification/indent controls, git change markers showing
     both previous and pending changes; and it works as a fully functional IDE: create files, compile, and
     run most common languages from inside the studio. Detail in §5.9.

---

## 1. Ground rules that shape everything

1. **Ground-up git.** A new `GitCore` library in `src/` implements the object model, packfiles, refs,
   diffs, merges, and the wire protocols directly — same philosophy as the hand-written HTTP server: no
   external library, internally optimized for this workflow (small repos, one writer, many cached reads).
   `git.exe` stays on the machine as a *test oracle* only; the server never invokes it.
2. **Ground-up-ish TLS.** MBED TLS (vendored single lib, MIT, Apache dual) terminates TLS inside the same
   process/listener — not a reverse proxy. Modern suites only: TLS 1.3 preferred, X25519, AES-GCM/ChaCha20.
3. **Vendored OpenSSH.** A pinned portable OpenSSH build (Windows via the WinPthreads-based port; Linux
   builds from upstream source) provides *both* directions: outbound push/fetch over ssh, and — optional,
   off by default — an inbound `git-receive-pack`/`upload-pack` ssh subsystem so the studio is a git server
   over ssh too. The OS's System32 ssh is never relied on.
4. **One port.** Everything (pages, API, git smart-HTTP, TLS) answers at `OLS_PORT`. Plain HTTP keeps
   working unless `OLS_TLS=on`; when on, an optional redirect door sends browsers up to https while still
   serving the key-gate page locally.
5. **Honesty rule.** Pages show only what doors answered. Absent fact → field absent + "the machine says
   nothing here". Errors are the module's own words (last-error channel pattern, `ImageService.cpp:618-623`).
6. **The wire is expensive.** See §9 — this is a first-class subsystem, not an afterthought.

---

## 2. Storage layout

```
data/projects/
  index.json                      project registry (atomic write; the only cross-project file)
  .ssh/                           studio-owned ssh identity: id_deploy(.pub), known_hosts, config
  remotes.json                    named remote endpoints + credential refs (secrets stay in .env)
  builds.json                     kept build records per project (cap ~200, newest-first)
  <slug>/                         one project; slug = [a-z0-9._-]{1,64}, unique, never reused
    repo/                         bare git repository — THE source of truth
      objects/ refs/ packs/ config
      hooks/                     studio-managed (see §8 server-side enforcement)
      ols/                       studio sidecar inside the repo dir (travels with the bundle):
        meta.json                 project facts (§5.3), source link, default branch
        issues/<id>.json          one issue record + comments
        pulls/<n>/record.json     PR record, approvals, merge state
        notes.json                UI annotations (reviewer pins etc.) — cosmetic only
    worktrees/<name>/             working trees for builds and PR merges (§7); disposable
    logs/                         fetch/push/build transcripts kept per project (cap by size)
```

- `index.json`: `{projects:[{slug,name,created_at,default_branch,source:{kind,url,pr,branch},badge}]}` —
  drives the dropdown in one read. Atomic temp+rename like peers.json.
- A project folder is **portable**: copy it to another machine's `data/projects/`, POST
  `/project/api/adopt {slug}`, and it appears — repo, issues, PRs and all. This is "not tied to GitHub"
  made physical.
- Repo layout is bare; browsing checks out content from objects (no permanent working tree except
  disposable build worktrees). This keeps the server immune to someone editing files behind its back and
  makes push/receive trivial.
- `data/` stays gitignored — project repos never leak into this outer repo.

---

## 3. GitCore — the ground-up git library (`src/gitcore/`)

Pure C++17, no dependencies beyond zlib (already linked via engines; vendored copy pinned if not present),
SHA-1 **and** SHA-256 object support, zstd optional later. Everything below is unit-testable without the
server or network, against `git.exe` as oracle (§14).

### 3.1 Objects
- Loose objects: read/write, zlib deflate stream-to-buffer with size pre-check; `objects/info/alternates` respected.
- Packfiles (v2): idx v2 read **and write**; multi-pack reverse index for lookup order; delta chains
  (OFS + REF) resolved iteratively with a bounded working set — never recursion depth = chain length.
- `gc`: repack loose→pack, prune unreachable after grace window, commit-graph-lite (a small `.ols-cg`
  sidecar mapping oid→{parents,treesum} for fast log/diff stats; rebuilt any time, never authoritative).
- Object access API: `getBlob(oid)`, `treeOf(commit)`, `log(ref, path?, since?)` — all streaming/callback
  so huge repos never materialize.

### 3.2 Refs & repository state
- Packed-refs + loose refs, symrefs (`HEAD → refs/heads/main`), reflog per ref (kept, shown in UI as
  "ref history", used by undo).
- `receive` safety: atomic ref transaction (all-or-nothing), `--force-with-lease` equivalent enforced
  server-side when the PR flow demands it.
- Repository config parsing (`core.*`, remotes) read-only for display; studio settings never live in repo
  config except standard keys.

### 3.3 Content & diffs
- Tree walk → file list with per-entry metadata (mode, size, last commit via bounded walk).
- Blob diff: myers-family line diff, byte-budgeted (big files switch to histogram fallback and report
  `approx:true` honestly); rename detection by content hash similarity at tree level.
- Unified diff + side-by-side hunk JSON; word-level intra-line spans for the viewer.
- **Delta endpoint (§9.4)**: server computes rolling checksums over the stored blob; browser's cached copy
  → rsync-style `need` list → minimal reconstruction. This is GitCore's delta machinery pointed at a file
  instead of an object.

### 3.4 Merge engine (for PR merges)
- Three-way merge on trees/blobs: clean → merge commit (or ff when possible, per PR setting); conflict →
  the studio answers `conflict:true` with per-file ours/theirs/base blobs and the UI offers resolve-in-UI
  (§6.4). No auto-commit of conflicted garbage — ever.

### 3.5 Wire protocols
- **Smart HTTP**: `GET/POST /git/<slug>/info/refs?service=...`, `/git/<slug>/git-upload-pack`,
  `/git/<slug>/git-receive-pack`. pkt-line framing, v2 protocol (want/have/depth), upload-pack negotiation
  against the browser/client's haves so clones of existing projects transfer only missing objects.
- **Client side** (outbound): fetch and push over http(s) with basic auth, and over ssh (§10). Same
  negotiator both directions.
- **Server-side receive**: unpack to temp, check integrity + policy (§8), atomic ref update, then studio
  hooks fire (PR sync, badges, SSE events).

---

## 4. Server transport changes (`OpenLocalServices.cpp`)

The hand-written server gains three general capabilities (each one small, each reusable beyond git):

1. **Streaming responses for non-SSE**: generalize the chunked path — a door may answer with a
   `ResponseSource` callback (write chunks until done) instead of a body string. `Connection: close` stays.
2. **Request streaming to disk**: when `Content-Length`/`Transfer-Encoding` exceeds a threshold *and* the
   path is under `/git/`, stream the body straight into a temp file handed to receive-pack (never all in
   memory; cap becomes configurable, default 512 MiB for git doors, 2 MiB elsewhere).
3. **TLS layer**: MBED TLS wraps accepted sockets when `OLS_TLS=on` (§10.3). Auth, routing, rate limiting
   run unchanged above it.

Git doors bypass the JSON body cap by (2); everything else keeps the old limits untouched — the change is
additive and cannot regress existing doors.

---

## 5. The web UI (`webui/project.html` + `project.js`)

### 5.1 Shell & header
- Fourth NAV entry (`📁`, "project studio"); same chrome: topbar state line, clock, host pick, config gear.
- **Project selector** sits in a second row under the nav (the *studio bar*): project dropdown on the left,
  [+] button beside it, then per-project quick facts (branch chip, dirty/PR counts), then the tab strip:
  **code · issues · pulls · builds · remotes · settings**. Tab strip is in-page state (not URL routes);
  selection persists to `ols-project-tab`. Deep-linking via `?project=<slug>&tab=<name>` so an AI agent or
  a bookmark lands exactly right.
- Changing the dropdown: swaps all panels from cache instantly, then validates against the server (§9).
  No full page reload — same page, re-rendered; the URL and every door call carry the slug.

### 5.2 [+] slide-out dialog (R-03/R-04)
Right-side sheet (new reusable `.sheet` component: fixed right panel, backdrop, ESC closes, focus-trapped):
- **New project**: name → slug preview/validation; default branch; checkbox init README; license picker
  (MIT/Apache-2.0/GPL-3.0/BSD-3/none) writes the file in-project via one commit door; optional "import from
  folder on this machine" (path must be inside the server's roots; copy-in, first commit).
- **From a link**: paste any clone URL (github/gitlab/gitea/ssh/https recognized — host + owner/repo parsed
  and echoed back for confirmation); optional **branch** picker and **PR number** picker — both fetched live
  from the remote via GitCore ls-remote/fetch once the URL validates (spinner honest about which step).
  The source is remembered in `index.json` (`source:{kind,url,pr,branch}`) and drives §10's "update from
  source" button forever after. Credentials: pick a stored credential (§10.2) or none (public repo).
- Everything in the dialog answers with real door results: validation ticks appear only when the server
  says so.

### 5.3 Code tab (R-05, R-14) — three columns
`files | viewer | facts`, resizable via CSS grid; each column collapsible under narrow widths.

**Files column**: lazy tree (folders open on demand, `?dir=` door), gitignore-aware dimming, per-entry
badges: issue count dot (n issues touching this file, §6), last-modified age chip, "in current PR" mark.
Search box filters by path substring with debounce (`watchFieldWords` idiom). Keyboard: arrows move the
selection, Enter opens — navigable without a mouse (R-07).

**Viewer column**: header strip = path + commit selector (branch/PR dropdown) + actions row
[`blame` `history` `raw` `download` `copy` `find`]. Features:
- **Syntax highlighting**, vendored highlight.js pinned under `webui/vendor/` (no build step; CSP allows
  self-hosted script). Languages auto-detected by extension, shown in the facts panel.
- **Outline tree** (the object-oriented view): a symbol index computed **server-side** per blob
  (`/project/api/file-outline`): classes/functions/methods/sections for C/C++/JS/TS/Python/Rust/Go/shell/
  markdown headings — a deliberately simple scanner, not a parser; cached by blob oid forever (content is
  immutable). Shown as a collapsible tree beside the gutter; click jumps + anchors (`#L123`), current line's
  symbol highlighted while scrolling.
- **Line numbers + anchors**; clicking a line number copies a `file:line` reference (the issue system eats
  these, §6). Diff mode renders hunks with +/- gutters, word-level emphasis, collapse-context controls.
- Blame view: per-line commit chips → click → commit page. History view: file log with mini-graph.
- Long files: virtualized render (only visible lines in DOM); the patch protocol (§9.4) keeps re-reads cheap.

**Facts panel**: what the machine parsed — *never* hand-typed unless edited through the edit door:
  purpose + description (README first paragraph / `meta.json`), **license** (detected by LICENSE file hash
  against a small built-in table; editable dropdown writes the file), **languages** with byte-share bars
  (extension map over tree), **architectures/platforms** (from CI files, CMakeLists, package.json engines,
  manifest hints — each row cites the file it came from), **dependencies** grouped by ecosystem
  (package.json / requirements / Cargo.toml / go.mod / CMake `find_package`), **planned features** = a real
  editable checklist stored in `meta.json` (cheap, useful; rows can convert to issues with one press).
  Every row that could not be determined says "the machine says nothing here" rather than guessing.

**Build control (CI/CD-style)** lives as its own tab (§7) but the facts panel wears a build-status chip for
HEAD and offers the run button — one press, same door as the builds tab.

### 5.4 Issues tab (R-06)
- List: filter chips (open/closed/all, severity, file-tagged), sort by updated; each row = id, title,
  tags (file chips!), state dot, age. New-issue form: title, body, severity (note/bug/want/blocker),
  **file picker** (type-ahead over the tree door) + optional line ref; a pasted `path/to/file.c:123` is
  parsed automatically.
- Detail: comments thread (markdown-lite rendered server-side — headings/bold/code only, sanitized), state
  changes as `{did,words}` doors, **linked PRs** (auto-detected when a PR branch name or commit message
  mentions `#<issue-id>`; manual link button too).
- **File pinning is the spine**: every issue carries `files:[{path,line?}]`; the file viewer shows a gutter
  badge + a "issues here" drawer listing them (open and closed, dimmed); clicking jumps to the issue. The
  facts panel shows per-directory rollups. An issue on a deleted path stays findable (marked orphaned).

### 5.5 Pulls tab (R-09) — the PR system
- **Branch list** first-class: every ref with ahead/behind vs default branch, last commit age, author,
  status pill (`merged` `open-pr` `stale` `protected`), hands: [diff vs base] [new PR] [delete (with
  confirm + lease check)]. Protected branches refuse force-push server-side (§8).
- **PR list**: open/merged/closed filters; row = #, title, branch → base, approvals dot (`2/1`), CI chip
  (last build on the head commit), conflict warning icon. PR detail page: description (editable), commits
  list (clickable), **Files changed** (the diff viewer, per-file expand, comment-on-line → creates a review
  comment that also becomes an issue tag optionally — one checkbox), conversation thread, and the approval
  bar: [approve] [request changes] [comment-only]; approvals are records `{who, verdict, at, body}`; merge
  button lives only when threshold met (§8 rules) else explains itself in its title. Merge options: ff /
  merge-commit / squash (all GitCore §3.4); on conflict → resolve-in-UI editor (three panes, pick-a-side +
  edit, then commit). Closing a PR refuses with words if the branch has unmerged commits not seen elsewhere.
- Branch/PR creation: [new branch] and [new PR from <branch>] dialogs; branch names validated server-side.

### 5.6 Builds tab (R-05)
- **Definitions** per project, stored `builds.json`: name, command line (validated against the refuse-set;
  run through a constrained shell), env additions (no secrets inline — reference `.env` keys by name),
  timeout, worktree policy (`fresh` = throwaway worktree at a ref; `reuse` = keep). Seeded with detected
  candidates (cmake present → configure+build suggestion etc.) but nothing runs without a press.
- **Runs**: job records in the house shape (202 + job_id, SSE `/api/jobs/<id>/events` reused — git/build
  jobs join the existing job system so badges/queue/header all work for free). Live log streamed as chunks;
  exit code, duration, artifact list. History table with [re-run] [kill] [show log]. A build on a PR head
  commit posts a status chip onto that PR automatically.

### 5.7 Remotes tab (R-12)
- **Endpoints**: list of configured remotes per project (`origin` = remembered source). Row: name, URL,
  kind badge (github/gitlab/gitea/custom), transport (ssh/https), last fetch/push stamp, hands:
  [fetch] [push all branches] [push tags] [update from source] (§10.4).
- **Credentials** panel: named credentials — ssh key path + passphrase? (no: passphrase-less deploy keys,
  enforced with an explanation) or username + token/password stored in `.env` (`OLS_GIT_CRED_<NAME>`).
- **Validation & help (the green checks)** — each is a real door result:
  - *key*: file exists, parses as private key (crypto check in-tree), public half derived → ✅; copy of the
    public key offered with one-click select.
  - *host trust*: hostname present in studio `known_hosts` → ✅ else ⚠️ with [trust this host] which does a
    TOFU probe and shows the fingerprint before storing.
  - *auth probe*: `ls-remote` against the URL → ✅ lists refs it can see; ❌ shows the server's own words
    (publickey refused / auth failed / not found) — never a guess.
  - *credential completeness* for https: username+token present and probe passes → ✅.
  - Push button disabled with the failing check named in its title until probes pass.

### 5.8 Settings tab
Per-project: default branch, PR approval threshold (1..n), protected-branch list, merge-style policy,
issue-label vocabulary, repo size stats + [gc now] (packfile housekeeping door), bundle export/import (§12).

### 5.9 The editor — an IDE in the viewer column (R-16)

The viewer's read mode stays exactly as §5.3 built it; editing is a **mode** of the same column, entered by
[edit] on any file (or `e`). One buffer at a time per browser tab (multi-file tabs are a later checkbox);
the buffer carries `{path, baseOid, text}` and nothing leaves the browser until a save.

**Editor core.** The vendored editor is **CodeMirror 6**, pinned under `webui/vendor/` as a pre-bundled
single ESM build (one file, no build step — same rule as highlight.js). Rationale for not hand-rolling:
a real editing surface (IME, selection model, multi-cursor, input methods) is the one component whose
correctness is measured in user-hours; everything *around* it stays ours. The editor pane gets stable ids
(`ps-editor`, `ps-editor-gutter`, `ps-editor-status`) per R-07.

**Feature parity with the VS Code editor (the actual editor, not its plugins):**
- **Syntax highlighting**: full language pack for the vendored set — C/C++, JS/TS/JSX, Python, Rust, Go,
  Java, C#, shell, cmake, make, JSON/YAML/TOML, markdown, html/css. Same extension map the outline scanner
  uses; unknown extension → plain text, said honestly in the status strip.
- **Editing**: multi-cursor (`Alt+click`, `Ctrl+D` word-select), rectangular/block select, find & replace
  with regex + case/word options and replace-all preview, jump to line (`Ctrl+G`), bracket match +
  auto-close, comment toggle (`Ctrl+/`), line move up/down, line duplicate/delete, undo/redo tree (CM6's
  own), tab-vs-spaces per language default with an override chip.
- **Beautification**: a whole-file format button per language — JS/JSON/CSS/HTML through vendored
  **js-beautify**; C-family, Python, and the rest through a house re-indenter (brace/indent-token scan that
  preserves strings/comments verbatim); every formatter takes options (indent width, tabs vs spaces, max
  column where meaningful) from a small per-project `.editorconfig`-style door read. Formatting is
  deterministic and diff-visible; the save preview shows the line-delta before anything commits.
- **Git change markers** — the gutter wears two kinds at once:
  - *pending*: against the file's `baseOid` (what HEAD holds) — added/modified/deleted bands computed
    client-side with GitCore's §3.3 diff algorithm ported to JS (vendored as one module), live on every
    edit, exactly VS Code's green/blue/red gutter behavior; a click on a band shows the hunk inline.
  - *previous*: against the file's **last commit** — which lines that commit touched (from `blame` door:
    per-line oid == last-commit-oid → "changed in HEAD" marks, dimmer than pending). So the gutter answers
    both questions at once: what this repo's newest commit changed, and what *you* are about to change.
  - a status dot per file in the tree column (M/A/D) follows from the same diff.
- **Problems panel** (§5.9 compile): diagnostics land as squiggles + a bottom panel; where no language
  server runs, the runner's own compiler errors are parsed into `{file,line,col,message}` and attached —
  for C/C++/Rust/Go/Java/gcc-family output this is reliable in-tree (regex over `path:line:col: message`),
  and the panel says which compiler spoke. Unknown output → raw log tab, never dropped.

**Compile & run (the IDE half).** A *toolchain* is a named entry in `builds.json` (§7) with an added
`run:` line and a `lang:` tag; §7's constrained runner does the work — the editor only drives it:
- **Run/Build buttons** (`Ctrl+Enter`, `F5`) on the editor strip act on the current file or project default:
  resolve a toolchain by language (a small built-in table: gcc/g++ → `gcc -o out src && ./out`, clang, cl,
  python, node, deno, bun, rustc/cargo, go run, javac/java, dotnet run, sh/bash, make/cmake targets); if the
  project defines one it wins; none found → the button explains which toolchain is missing and offers the
  definition form. Detection at project open (which compilers answer `--version` on this machine) feeds the
  picker — facts read from the machine, never assumed.
- **Run buffer**: live stdout/stderr streamed over the job SSE (§5.6), capped ring + full transcript to
  `logs/`, exit code and duration worn in the status strip; [stop] kills the job tree (the non-inheritable
  handle rule of §7 applies). stdin: a small input box for interactive runs, sent as one chunk.
- **Create**: new file / new folder from the tree column — path validation like every door, parent dirs
  auto-created in the commit; deleting through the UI asks once.
- **Save = commit** (house rule, unambiguous): [save] opens a one-line commit message field (prefilled with
  `Update <path>`), then POSTs `/project/api/commit` (§11) with `{branch, files:[{path,content}], message}`;
  on success the buffer's `baseOid` moves and the pending markers clear. A save that raced a push to the
  same file refuses with words ("HEAD moved under you — rebase your edit?") and offers reload-with-diff.
- **Safety**: editing is behind the same auth gate as everything; the runner stays inside §7's refuse-set
  and worktree rules (runs happen in `worktrees/`, never a permanent tree); the editor never holds secrets
  beyond what the door already saw.

**Cost bounds**: buffers ≤ 2 MB to edit (bigger opens read-only with the reason — VS Code draws the same
line); diff-markers recompute debounced 150 ms; on huge files markers degrade to "markers off for this
file" rather than stuttering, and say so.

**Checkpoint additions**: CD's gate gains one editor proof — edit a C file in the browser, watch the gutter
mark the pending change, press Run, see the compiler output stream into the run buffer, save as a commit,
and confirm blame/history show it. CF gains: format a 500 KB JS file → diff shows only formatting; and
markers recompute stays under one frame budget on a 10k-line file.

---

## 6. Issues — data model and the file spine

```jsonc
// data/projects/<slug>/repo/ols/issues/<id>.json   (issue-<12hex>-<8hex>)
{ "id": "...", "project": "<slug>", "title": "...", "body": "...",
  "state": "open|closed", "severity": "note|bug|want|blocker",
  "files": [ { "path": "src/main.cpp", "line": 123, "commit": "<oid-at-report>" } ],
  "labels": ["ci"], "comments": [ {"at":1704067200,"who":"webui","body":"..."} ],
  "links": { "prs": [7], "commits": ["<oid>"] },
  "created_at": "...", "updated_at": "..." }
```

- File tags are **paths + optional line + the commit they were seen at**; when browsing another commit the
  studio re-anchors by path (and shows a small "moved/renamed since" note via rename detection). This keeps
  issues honest across history instead of pretending lines are stable.
- Doors: list `GET /project/api/issues?project=&file=&state=...` (the `file=` filter is what the code view
  calls), item GET/POST (edit fields), `POST .../comments`, `POST .../close|reopen`. All `{did,words}` for
  mutations; instant effect.
- **Machine-readable issue index** (`GET /project/api/issues.json?compact=1`) — one line per issue with its
  file tags, designed for AI agents (§13).

---

## 7. Builds — the CI-style runner

- Runs on a detached job thread inside ProjectService (house job pattern), serialized per project under a
  mutex (git index habits are serial; builds too unless marked `parallel:true` with a fresh worktree).
- Execution: constrained shell (`cmd /c` / `/bin/sh -c`) with cwd = worktree, env from allow-list + named
  `.env` refs, wall timeout, output capped per run (ring buffer streamed to SSE; full transcript to
  `logs/`). Child handles non-inheritable (the listener-socket lesson, `Platform.h:91-108`).
- Status chips post to PRs automatically by head-commit oid. Build definitions are data, not code — no
  hooks that execute arbitrary committed files (§8 policy).

---

## 8. The studio as a git server (R-08) + enforcement

- **Smart HTTP** at `/git/<slug>/...` under the same port and the same api-key auth (key via header or
  `?key=`; documented on the remotes tab with copyable clone commands). Read (`upload-pack`) and write
  (`receive-pack`) behind the gate; streaming both ways (§4).
- **Server-side policy in receive** (GitCore §3.5, not hooks alone): reject non-fast-forward without lease
  unless branch unprotected; reject pushes to protected branches except via PR merge; reject commits whose
  trees contain paths the studio bans (absolute paths, `..`); size caps per object/push from config.
- **Hooks**: studio installs a `post-receive` that only ever calls back into the local API loopback — kept
  as interop surface for external tooling, never trusted for its own logic (logic lives in receive).
- **ssh subsystem** (optional, §10.5): vendored sshd serving `git-upload-pack/git-receive-pack` from
  GitCore for key-auth'd users; off unless `OLS_SSH_GIT=on`.

---

## 9. Caching — the wire is expensive (R-11, R-14)

Existing pattern (`status-cache.js`) keeps JSON readings with server-declared max-age as TTL, POST clears
the family, files are memory-only per page. Project Studio extends it in three layers:

### 9.1 Change stamps everywhere
Every project door answer carries `rev` — a monotonic per-project revision bumped by any mutation (commit,
issue write, PR change, build finish) and exposed in `index.json`. The browser sends nothing; it compares
its held `rev` against the dropdown read. **If rev matches, zero further fetches for that project.** This
is the cheap global change detector the studio adds on top of per-path max-age.

### 9.2 localStorage tiers (new: `ols-project-cache-v1`)
- **Tier M (metadata)** — index, facts, issue list, PR summaries, branch list: localStorage with server
  TTL like today, keyed by `(path, rev)`. Boot = replay from localStorage first paint always.
- **Tier B (blobs)** — file contents by `oid`: content-addressed → **immutable, stored until LRU-evicted**,
  never re-fetched while present. Size-capped store (~32 MB default, per-entry cap ~2 MB with honest
  "too big to cache" fallback); eviction = LRU on read-touch. Keying by oid means zero invalidation logic.
- **Tier D (derived)** — outlines, diffs: keyed `oid+kind`, same immutability rules as B.
- Quota discipline per house style: every setItem try/catch, failure degrades to memory-only + one log line.

### 9.3 Live invalidation without polling
Project mutations (UI action, git push from outside, build finish) bump `rev` and publish an SSE event on a
per-project stream `GET /project/api/events?project=<slug>` (uses the existing SSE plumbing generalized).
The page holds one open stream per visible project; on `changed{what,rev}` it fetches exactly the affected
doors. Polling remains as fallback (`sleepWindow` idiom) if the stream drops.

### 9.4 The patch protocol for large files (R-14)
When the browser holds blob `oid_old` (e.g. a file open in the viewer while its branch moved):
1. Browser POSTs `{path, have: oid_old, want: oid_new, sigs:[rolling-checksum-per-block]}`.
2. Server diffs old-blob-signatures against the new blob's blocks and returns only missing blocks + copy
   instructions (rsync-style; GitCore §3.3 reuses its delta machinery).
3. Browser reconstructs, verifies sha of `oid_new`, stores under Tier B. If signatures don't match anything
   held (browser cache was LRU'd), plain fetch by oid — still one request, content-addressed.
Typical case: a 500 KB source file edited in the UI → tens of bytes on the wire.

### 9.5 Server-side freshness table
New rows in `apiFreshSeconds()`: dropdown/index 2 s (cheap, keeps revs fresh), heavy list doors 5 s default,
blob/outline/diff doors effectively immutable client-side via Tier B (server still answers max-age=31536000
since oid-keyed paths never change).

---

## 10. Remotes: ssh + https identity done right (R-12, R-15)

### 10.1 Principle
The server runs elevated in Session 0 — `%USERPROFILE%` is not trustworthy. **All identity is explicit**:
every GitCore transport call receives its key/credential from the studio's own store; nothing falls back to
ambient config.

### 10.2 Credential store
- ssh: `data/projects/.ssh/id_deploy` (+ any named keys), created by a door (`POST /project/api/keys/new`
  generates an ed25519 pair in-tree, returns the public half). Passphrase-less (enforced; UI explains why —
  unattended probes).
- https: `username` + token/password stored as `.env` keys (`OLS_GIT_CRED_<NAME>`, secret rows in the config
  panel) — house secrets rule respected. Basic auth over TLS only.
- known_hosts: studio-owned file; TOFU probe door shows fingerprint before storing (§5.7).

### 10.3 TLS for the API (R-15)
- MBED TLS vendored pinned (`vendor/mbedtls/`), fetched by run.bat/run.sh like other vendors.
- `OLS_TLS=off|on|auto`: **auto** = if no cert exists, generate a small local CA + leaf on first start:
  X.25519 key, SANs for localhost, machine hostname, and the LAN addresses it binds (10.255.0.x etc.),
  ~825-day validity; leaf signed by studio CA. Files under `data/tls/`. Cert panel shows fingerprint/expiry/
  SANs + [regenerate] + download-CA button (so browsers/boxes trust once).
- Modern config: TLS 1.3 with 1.2 floor, AEAD suites only, no RC4/3DES/CBC-SHA1; HSTS header when TLS on.
- HTTP keeps answering unless `OLS_HTTPS_ONLY=on` (then plain port answers a redirect + the key-gate page).
- Git smart-HTTP rides the same listener — `git clone https://x1.lan:8765/git/<slug>` works once the CA is
  trusted (or `-c http.sslCAInfo=data/tls/ca.pem`).

### 10.4 "Update from source" (R-04)
One button per project + a global one in the dropdown row: for each remembered `source`, fetch → fast-forward
the matching local ref (branch or PR head), report `{did, words}` with real counts ("took 3 commits into
main"). Branch changes on the source are seen via ls-remote diff (refs added/removed/moved listed). Scheduled
auto-fetch is a later checkbox — deliberately not in v1.

### 10.5 Vendored OpenSSH (R-15)
- Windows: pinned portable OpenSSH build (WinPthreads port, e.g. the portable-openssh tree) fetched by run
  scripts into `vendor/openssh-win/`; Linux: source built from pinned tag. Server uses it for outbound ssh
  transport via a **pipe protocol** (subprocess of the vendored binary speaking stdio to GitCore's client —
  acceptable because the *library* logic stays ours; only the crypto transport is outsourced) and, when
  `OLS_SSH_GIT=on`, runs its sshd on `OLS_SSH_PORT` with a keys-only subsystem.
- Validation panel (§5.7) probes through this exact path — what it checks is what will run.

---

## 11. API door map (R-10)

Prefix `/project/api/...`; every door under the existing auth gate; mutations answer `{did,words}` (+ new
`rev`); long work answers 202 + job_id on the **existing** job system (`/api/jobs/<id>`, SSE included).
Git smart-HTTP at `/git/<slug>/...` (non-JSON). `?project=<slug>` required on project-scoped doors.

```
GET  /project/api/index                       dropdown data + revs + counts          POST create/adopt actions:
POST /project/api/projects/new                {name,opts} → {did,words,slug}         (all instant-effect)
POST /project/api/projects/adopt              {path|url,...} clone/import as project
POST /project/api/projects/delete             confirm-by-name; refuses if open PRs
POST /project/api/projects/rename

GET  /project/api/meta        POST edit        facts incl planned features
GET  /project/api/tree?dir=                    lazy tree w/ badges, issue counts
GET  /project/api/file?path=&ref=              blob (oid-keyed variant for Tier B)
POST /project/api/file/patch                   §9.4 delta request
GET  /project/api/file-outline?oid=            symbol tree (immutable by oid)
GET  /project/api/blame?path=&ref=             per-line commit map
GET  /project/api/history?path=&ref=           file log
POST /project/api/commit                       {branch, files:[{path,content}], message} — the UI editor's save
GET  /project/api/diff?base=&head=&path?       unified + side-by-side JSON
GET  /project/api/branches                     list + status (ahead/behind, PR link)
POST /project/api/branches/new|delete|rename   lease-checked

GET  /project/api/issues?file=&state=          list; compact=1 for agents
POST /project/api/issues                       new; item doors: edit/comment/close/reopen/link

GET  /project/api/pulls?state=                 PR list; GET/POST /pulls/<n> detail + actions:
POST /project/api/pulls                          create {head,base,title,body}
POST /project/api/pulls/<n>/approve|request-changes|comment
POST /project/api/pulls/<n>/merge                {style} → synchronous merge answer; a conflict lists its files
POST /project/api/pulls/<n>/resolve              {file, content} then merge retry
POST /project/api/pulls/<n>/close|reopen

GET  /project/api/builds                       definitions + recent runs
POST /project/api/builds/new|delete            definition CRUD (validated)
POST /project/api/builds/run                   {name, ref} → 202 job_id; kill via /api/jobs/<id>/cancel

GET  /project/api/remotes                      endpoints + probe states
POST /project/api/remotes/set                  add/edit endpoint (+source link change)
POST /project/api/remotes/fetch|push           real transfer, {did,words} with counts
POST /project/api/remotes/probe                validation battery → per-check results (§5.7)
GET  /project/api/keys                         studio ssh inventory (public halves + fingerprints)
POST /project/api/keys/new                     generate ed25519 pair
POST /project/api/keys/trust-host              TOFU store after fingerprint shown

GET  /project/api/events?project=              SSE: changed{what,rev}, job ticks relayed
GET  /project/api/search?q=&project=           path + issue text search (bounded)
GET  /project/api/gc                           repo stats; POST runs repack/prune
```

Every list door embeds `rev` and its own `as_of`; the page never paints a field it has no reading for.

---

## 12. Portability & bundles

- Folder copy = migration (bare repos + sidecars are self-contained); `POST /project/api/adopt` registers.
- **Bundle export**: GitCore writes a real git bundle (`*.bundle`, packfile v3-style index) + the `ols/`
  sidecar as one zip — moves a project with issues/PRs to any other OLS box; import reverses. Cheap because
  the machinery is §3 already.

---

## 13. AI-assistant navigability (R-07)

Designed for mcp__browser-style automation and for agents reading doors directly:
- **Stable ids**: every interactive element gets a semantic kebab id (`proj-dropdown`, `pr-merge-btn`,
  `issue-new-open`); the house idiom is `ui.E(id)` — enforced by checklist, not convention.
- **URL state**: `project.html?project=<slug>&tab=code&file=<path>#L123` fully reconstructs a view; an agent
  can screenshot any exact state.
- **Tooltips everywhere** (house style) describe what a press does and what answer to expect — the DOM is
  self-documenting to `snapshot -i`.
- **Machine views**: `?compact=1` JSON variants for issues/search; every door's response documented in this
  file; a `GET /project/api/doors` answers the live route table (path, method, params) so an agent can
  discover without reading source.
- Status lines are sentences (`{did,words}`), never codes-only; failures name the failing check (§5.7).

---

## 14. Testing strategy (feeds plan.md checkpoints)

1. **GitCore unit tests** (`ols-git-tests` target): golden files generated by `git.exe` once and committed
   as fixtures — packfile read/write round-trips, delta resolution vs oracle, diff correctness vs
   `git diff --no-index`, merge cases (clean/conflict/rename), pkt-line/v2 framing vectors.
2. **Protocol tests**: script a real `git clone/fetch/push` against the running studio (interop = oracle);
   refuse-cases: oversized push, protected branch, non-ff without lease.
3. **Door tests**: curl matrix per door (happy/400/401/404/405), rate-limit behavior under load.
4. **Browser gate** (house rule): every phase proven through the running server in a real browser, clean
   console, before commit; caching verified with Network panel — second visit = zero refetches while revs
   hold; patch protocol observed byte-counts on a big-file edit.
5. **Windows+Linux pair**: same tests green on X1 and UM780 before release stamp (dev.md gate).

---

## 15. Decisions log & open items

| # | decision | why |
|---|---|---|
| D-1 | Ground-up GitCore, git.exe oracle only | user directive; internal optimization for this workflow; no libgit2 anywhere |
| D-2 | MBED TLS in-process (not a proxy) | one port, one process house rule |
| D-3 | OpenSSH vendored for transport only; crypto/protocol logic of git stays ours | user directive; keeps identity explicit (§10.1) |
| D-4 | Bare repos + disposable worktrees | immune to out-of-band edits; receive trivial; builds isolated |
| D-5 | `rev` stamp + content-addressed blob cache + SSE invalidation | R-11 without inventing a new protocol per door |
| D-6 | Issues anchored at (path, line@commit) | lines move; anchoring keeps them honest across history |
| D-7 | Build definitions are data run through constrained shell, never committed hooks | cheap safety |
| D-8 | Editor = vendored CodeMirror 6 (one ESM bundle) + house diff-marker JS; compile/run rides §7's runner and a toolchain table | the editing surface is the one component not worth hand-rolling; everything around it stays ours (§5.9) |

Open items (answer before the phase that needs them):
- O-1: exact OpenSSH Windows port + tag to pin (plan Phase E spike).
- O-2: whether `/git/` smart-HTTP should accept a *separate* weaker credential than the api key for
  read-only clones (v1: same key, noted as future split).
- O-3: TLS default on X1's live serve — flip `OLS_TLS=auto` only after operator OK (dev.md: never restart
  serves casually; this is a config + restart action).
