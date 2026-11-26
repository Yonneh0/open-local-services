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

- Project Studio — finish it (companion contract: `spec_project_studio.md`; the development plan folded here 2026-11-26 from the retired `plan_project_studio.md`)
  Prompt: "finish. use subagents on um780 for coding tasks and audits" + "ensure all code is extremely robust, above release-quality"; every checkpoint committed and pushed.
  Follow-on guidance: fleet caps per operator (currently ≤2 children r9 + 1 um780 + 1 X1 beside the lead; routes: r9 v31, um780 `qwen3.8-flash-next-coder-iq1_m`); the X1 serve runs CUDA (`cuda86`) — agents never run `run.bat`, never touch browsers of the operator (headless own-instance via CDP only), and land their own serve restarts through the elevated `schtasks /end`+`/run` door, verified by behavior; docs fold with every checkpoint; all implementation in subagents on disjoint file sets, shared files integrated by the lead.
  Changes: pushed through `6b80d4e` (HEAD, origin/master). Landed & proven: fetch client; merge engine + resolve round; pull/issue records; 23 doors incl. `/project/api/events`; webui Pulls/Issues rooms; six gate findings closed; packed-refs absence reads empty (`ec25ddd`); merge commits carry the pull title, conflict answers state counts in words, apiR carries refusal words (`b70b31b`); open pulls refuse project delete + build-moment restamps every link (`8c69387`); F1 — SSE events stream with `{slug,rev}` frames (proven live), watch-not-poll wiring, oid blob store Tier B/D (`1d9a497`); record doors beside a press answer no-store + every press re-reads its record fresh (`dd5fcb9`); TLS E1 batch 1 — mbedtls 3.6.8 pinned from its release page, one-port MSG_PEEK probe, EC P-256 certs under `data/tls`, `OLS_TLS off/on/auto` (proven on scratch :19765: plain+https same port, mode matrix, reuse) (`3d4e44a`); roomRefresh carries no-store and deep links watch from first paint (`6b80d4e`). Browser gate: press approve-on-merged **PASS** (refusal line exact, zero js errors); press issue-close→closed detail half PASS (badge flip 163 ms), list half owed a re-run on the rebuilt serve. Live battery **22/22**; harness `PASS 214 / FAIL 0`.
  REMAINING WORK — UI truth first: of the six studio tabs, code/issues/pulls carry real rooms; **builds, remotes and settings are still placeholder panels** (no doors behind them, so nothing is drawn but the honest empty words). The phases that write those rooms stand below and must not be dropped.
  · CD close-out: browser press issue-close→closed LIST half on the rebuilt serve (child running, lane gt-press3-x1).
  · E1 TLS batch 2: cert-panel doors (`caFingerprint`/`leafExpiry` + `ca.der` download), http→https redirect at the `m_tlsMode=="on"` plain return, HSTS header when sealed, per-connection negotiated line; Linux pair-build (gcc + run.sh parity) still unproven.
  · E2 remotes transport: ssh child running (lane gt-e2-r9c; spike GREEN — Win32-OpenSSH 10.4.0.0p1 pin `b67ff8be…`, SshFetch:HttpFetch sticky design, fetch.cpp untouched except url gate). After it lands: keys/new + trust-host doors (ssh-keygen/ssh-keyscan helpers already in the batch), probe-battery door `remotes/probe` with the five §5.7 checks. Linux pin of upstream portable openssh owed per spec §10.5 (honest stub left).
  · **E4 — the placeholder UI phase (um780 UI): Remotes tab** (endpoints, credentials panel, green-check validation UI wired to `remotes/probe`, push/fetch buttons with disabled-why titles), **Builds tab** (definitions CRUD list+sheet, live log streamer over the run buffer, history, PR status chips), **Settings tab** (project facts editor + TLS cert panel once E1 batch 2's doors stand). These three panels are placeholders today and this is the phase that writes them.
  · E3 backs E4's builds room: definitions CRUD (+ `run:`/`lang:` toolchain fields per spec §5.9), constrained runner as house jobs, compiler-diagnosis parser → structured diagnostics door, worktrees, SSE logs, `builds.json` cap.
  · E5 tail: tree status dots + keymap mount (multi-cursor, bracket/comment commands).
  · F2 patch protocol: rolling-checksum door + client reconstruction (§9.4) with sha verify. F3 navigability audit (`?compact=1`, stable ids, doors-table completeness, agent quick-reference in spec §13). F1 owes the Network-panel zero-refetch proof (rides on CG).
  · G: bundle export/import doors; Linux pass on UM780 (`run.sh` build + same tests); README door-map truth; dual-OS browser gate → move to Awaiting User Verification.
  Checkpoint ledger: CA/CB/CC done (`5d2caef`, `9909d17`+2, `e44e34d`). CD open — only the list-half press above. CE (TLS + push to GitHub/GitLab validated + editor proof **with the Remotes/Builds/Settings rooms real, not placeholders** — E3/E4) open. CF (caching table §9 + patch bytes) open — F2/F3. CG final — G.
  Risk notes still live: mbedtls fights accept loop → batch 1 proved the seam, fallback `OLS_TLS_PORT` separate listener; OpenSSH Windows pain → spike green, fallback vendored libssh2 transport-only (operator-OK deviation); CM6 packaging → pinned prebundle, fallback textarea editor.

## 3. Awaiting User Verification
when work is complete, edit this document, add any additional user guidance received during the session, and move the work item here, when completed.

- Spec documentation refactor — `spec.md` + `spec_image_studio.md` + `spec_voice_studio.md` + rename to `spec_project_studio.md`
  Prompt: "refactor spec documentation, and generate clear spec documentation from existing code base… spec.md core; spec_image_studio.md; spec_voice_studio.md; spec_project_studio.md: rename project_studio_spec.md — leave it alone short of formatting and clear error corrections… clear feature lists, related files, every feature with the interfaces spelled out at each level… where functionality is missing, write the spec how it should be and add a note here to implement/fix that portion" + "no subagents".
  Follow-on guidance: "Project Studio is still under development — treat that spec as accurate regardless of unimplemented features"; "ensure new spec documentation contains clear feature lists"; "I honestly can't tell: are you making progress, or stuck in a loop?" — deliver a finished file each pass; "reground yourself, and get back on track."
  Changes: new `spec.md` (15 sections — transport, rate gate, credential, elevation, configuration, job law + event stream, cache law, pages + expand, file doors, machine readings, fetching, service door + `deviceCard()` + VerifiedRoom, web shell, constants, refusal reading); new `spec_image_studio.md` (15 sections — every image door and the `ImageService.h` interface whole, model/quant tables, weights room, the run and clip laws, gallery/refs/prefs/timings; status and devices answers documented where they are actually read — the `loaded` block and devices array of `/api/system`, not doors of their own); new `spec_voice_studio.md` (12 sections — the engine `model_specs` card law, weights room, session and fallback law, the speaking and listening runs with every refusal word, download, voice library, kept renders, model tables, `VoiceService.h` interface, webui, constants); `project_studio_spec.md` → `spec_project_studio.md` (`git mv`; only its title, one `plan_project_studio.md` reference, and one `OpenLocalServices.cpp` comment touched). All doors cross-checked against `OpenLocalServices.cpp` and both webui pages; no missing or unimplemented image/voice functionality found, so no implement/fix notes are owed from this pass.

