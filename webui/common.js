// Shared library every page loads after key-shim and status-cache: page
// chrome (header bar and nav), the running-state line and machine chips,
// field/option readers, formatters, API and job-event plumbing, progress
// bars, and replay of this browser's last reading.
(function () {
  const UI = {};
  window.ols = UI;

  const E = (id) => document.getElementById(id);
  UI.E = E;

  // ---------- the header: one line every page carries ----------
  // While a job runs, the topbar doubles as a progress bar behind the words.
  // Each engine has its own tint, matching the row tags on the pages.
  const TINT = {
    image: 'var(--accent)',
    voice: 'var(--accent2)',
  };

  // The machine's health, held in one place: the home tab in the nav is
  // painted with the state (a glow whose colour is the state), and the page
  // about the machine - the home page, which the bare address opens - states the
  // same reading in words in its own panel. A page with no such line paints the
  // tab and states nothing, and nothing is claimed in words before a reading
  // arrives, which is why an unread machine says it was not read. The key the
  // colours and the words are both read by: nothing read is orange, an
  // unreachable machine is red, one only standing models up is yellow, counted
  // work is blue, a machine at rest is green. Only the state class is swapped;
  // everything else the chrome set on that tab - including the mark of the page
  // you are on - stays where it was.
  const STATE_WORDS = {
    unknown: 'server not read yet',
    bad: 'server not reachable',
    loading: 'server ok: models being stood up',
    running: 'server ok: work running',
    idle: 'server ok: nothing running',
  };

  UI.sysinfo = function (text) {
    const node = E('sysinfo');
    if (node) node.textContent = text;
  };

  UI.serverGlow = function (state) {
    const known = STATE_WORDS[state] ? state : 'unknown';
    UI.sysinfo(STATE_WORDS[known]);
    const tab = document.querySelector('#nav a[href="index.html"]');
    if (!tab) return;
    tab.classList.remove('state-unknown', 'state-bad', 'state-loading',
      'state-running', 'state-idle');
    tab.classList.add('state-' + known);
  };

  UI.headProgress = function (pct, text, engine, asOfText, indeterminate) {
    const bar = E('topbar');
    if (bar) {
      bar.style.setProperty('--job-progress', Math.max(0, Math.min(100, pct)) + '%');
      bar.style.setProperty('--job-tint', TINT[engine] || TINT.image);
      if (indeterminate) bar.classList.add('indeterminate');
    }
    UI.serverGlow('running');
    const node = E('head-status');
    if (!node) return;
    node.textContent = text + (asOfText || '');
    node.className = 'head-status run';
  };

  UI.headIdle = function (asOfText) {
    const bar = E('topbar');
    if (bar) {
      bar.style.setProperty('--job-progress', '0%');
      bar.style.removeProperty('--job-tint');
    }
    UI.serverGlow('idle');
    const node = E('head-status');
    if (!node) return;
    node.textContent = 'nothing running' + (asOfText || '');
    node.className = 'head-status idle';
  };

  // Offline header: everything is labeled unknown, not absent — an
  // unreachable server cannot show what it has; it can only fail to show it.
  UI.headUnknown = function () {
    UI.serverGlow('bad');
    const bar = E('topbar');
    if (bar) bar.style.setProperty('--job-progress', '0%');
    const unknown = 'Server unreachable: value is unknown, not absent';
    UI.chip(E('chip-cpu'), 'cpu: unknown\nmemory: unknown', 'err', unknown);
    UI.chip(E('chip-gpu'), 'gpu: unknown\nvram: unknown', 'err', unknown);
    UI.chip(E('chip-jobs'), 'jobs: unknown', 'err', unknown);
    // An unreachable machine is not a machine holding nothing: the bubbles
    // simply stop wearing a claim they can no longer read.
    if (bubbles.image) bubbles.image.hidden = true;
    if (bubbles.voice) bubbles.voice.hidden = true;
    UI.chip(E('chip-version'), 'code: not read', 'err',
      'The code version (a development build says dev, a released one names its commit) was not ' +
      'read.');
    const node = E('head-status');
    if (!node) return;
    node.textContent = 'not connected';
    node.className = 'head-status err';
  };

  // Only engine kinds this server actually tags; anything else is a job of
  // unknown kind, and a guessed label would state a fact that is not true.
  UI.jobEngine = function (job) {
    const engine = String(job.engine || '');
    if (engine === 'image' || engine === 'voice') return engine;
    return 'job';
  };

  UI.jobLabel = function (job) {
    return String(job.model || job.voice || job.kind || job.id);
  };

  // How long a stretch of seconds is, in the fewest words that still say it:
  // seconds under a minute, minutes under an hour, hours and minutes past that.
  function spanWords(seconds) {
    const s = Math.round(Number(seconds) || 0);
    if (s < 60) return s + ' s';
    let h = Math.floor(s / 3600);
    let m = h > 0 ? Math.round((s - h * 3600) / 60) : Math.round(s / 60);
    // A rounded minute that lands on sixty is the next hour standing in its
    // clothes: carry it, so no reading ever claims sixty minutes of an hour.
    if (m >= 60) { h += 1; m = 0; }
    if (h > 0) return h + ' h' + (m > 0 ? ' ' + m + ' min' : '');
    return m + ' min';
  }

  // Header state from the merged snapshot: the bar tracks only counted work
  // (generation, synthesis, downloads); a load has no percentage and stands
  // striped. The header is where a reader looks for how the
  // drawing goes, so it carries the time estimate too: the work furthest into
  // its window is measured against the pace it has itself held since its start,
  // and a run whose pace cannot be measured is said without an estimate rather
  // than with an invented one. Work standing in the line before the engine has
  // taken it up is named beside it, with no bar to draw - its turn has not come.
  // Several jobs of one engine and model on a line of their own is noise: a
  // header that spells "image flux-3-klein" six times has stopped describing
  // anything. One place folds identical jobs into a count.
  function collapseJobs(list) {
    const seen = [];
    const counts = {};
    for (const job of list) {
      const key = UI.jobEngine(job) + ' ' + UI.jobLabel(job);
      if (counts[key] === undefined) { counts[key] = 0; seen.push(key); }
      counts[key] += 1;
    }
    return seen.map(function (key) {
      return counts[key] > 1 ? key + ' ×' + counts[key] : key;
    }).join(', ');
  }

  UI.headerState = function (jobs) {
    const list = Array.isArray(jobs) ? jobs : [];
    const active = list.filter((j) => j.state === 'running');
    const waiting = list.filter((j) => j.state === 'queued');
    if (!active.length) {
      if (waiting.length) {
        UI.headProgress(0, waiting.length + ' queued: ' + collapseJobs(waiting), UI.jobEngine(waiting[0]));
        return;
      }
      UI.headIdle();
      return;
    }
    let furthest = 0;
    let furthestEngine = 'job';
    let furthestJob = null;
    let onlyLoads = true;
    for (const job of active) {
      if (String(job.kind || '') === 'load') continue;
      onlyLoads = false;
      const max = Number(job.max || 0);
      const pct = max > 0 ? Math.round((Number(job.done || 0) / max) * 100) : 0;
      if (pct > furthest) {
        furthest = pct;
        furthestEngine = UI.jobEngine(job);
        furthestJob = job;
      }
    }
    // The header names one job in full - the one furthest along - and counts
    // the rest. Its estimate rides on that job's own held pace.
    let line;
    if (furthestJob) {
      const max = Number(furthestJob.max || 0);
      const done = Number(furthestJob.done || 0);
      line = UI.jobEngine(furthestJob) + ' ' + UI.jobLabel(furthestJob);
      if (max > 0) line += ' · step ' + done + '/' + max;
      const since = Number(furthestJob.started_at || 0);
      const heldSeconds = since > 0 ? Date.now() / 1000 - since : 0;
      if (max > 0 && done > 1 && heldSeconds > 0 && done < max) {
        const left = ((max - done) * heldSeconds) / done;
        if (left > 0 && left < 86400) line += ' · about ' + spanWords(left) + ' left, at the pace it has held';
      }
      if (active.length > 1) line += ' · ' + (active.length - 1) + ' more running';
    } else {
      line = collapseJobs(active);
    }
    if (waiting.length) line += ' · ' + waiting.length + ' waiting for the card';
    // A run that names no window has no percentage to draw: the bar stripes
    // instead, so the header says work is going on without naming a share of it.
    UI.headProgress(furthest, line, furthestEngine, '', Number((furthestJob || {}).max || 0) <= 0);
    // Standing a model up is the one kind of work that has no count to draw:
    // with only loads running, the tab glow is yellow rather than blue.
    if (onlyLoads) UI.serverGlow('loading');
  };

  // ---------- the studios' load bubbles ----------
  // Each studio wears, on the top-right corner of its tab, what its engine
  // stands in memory: the count of models, with the weight the machine states
  // for them underneath. The numbers are the machine's own statements - the
  // picture engine names its resident models and their bytes, the voice engine
  // names the models standing in it and weighs them - and the bubble copies
  // that stating, claiming nothing of its own. A studio whose engine has
  // nothing standing wears nothing at all: the absence is the message, an
  // unread marker being only worn when there is something to read. The bubble
  // catches no click: a press on it lands on the tab it stands on, exactly as
  // it did before the bubble was there. It is painted from the same merged
  // reading the header chips come from, so a bubble and a chip can never tell
  // different stories, and an unread machine leaves them silent rather than
  // inventing a zero.
  const bubbles = {};
  const mountBubble = function (kind, href) {
    const tab = document.querySelector('#nav a[href="' + href + '"]');
    if (!tab) return null;
    const bubble = document.createElement('span');
    bubble.className = 'tab-bubble';
    bubble.setAttribute('aria-hidden', 'true');
    bubble.hidden = true;
    tab.appendChild(bubble);
    return bubble;
  };
  const paintBubble = function (kind, href, count, bytes, names, words) {
    let bubble = bubbles[kind];
    if (bubble === undefined) bubble = bubbles[kind] = mountBubble(kind, href);
    if (!bubble) return;
    if (!(count > 0)) {
      bubble.hidden = true;
      return;
    }
    let face = '<b>' + count + '</b>';
    if (bytes > 0) face += '<small>' + UI.sizeText(bytes) + '</small>';
    bubble.innerHTML = face;
    bubble.hidden = false;
    bubble.title = words + ': ' + names.join(', ') +
      (bytes > 0 ? ' - weighing ' + UI.sizeText(bytes) : '');
  };
  const engineBubbles = function (system) {
    const weights = (system.loaded && system.loaded.loaded) ? system.loaded : {};
    const resident = Array.isArray(weights.resident) ? weights.resident : [];
    let imageBytes = 0;
    const imageNames = [];
    for (const item of resident) {
      imageBytes += Number(item.bytes || 0);
      imageNames.push(String(item.model) + (item.quant ? ' as ' + String(item.quant) : ''));
    }
    if (!resident.length && Array.isArray(weights.loaded)) {
      for (const model of weights.loaded) imageNames.push(String(model));
    }
    paintBubble('image', 'imagegen.html', imageNames.length, imageBytes, imageNames,
      'Picture models standing in memory');
    const voice = system.voice || {};
    const standing = Array.isArray(voice.open_models) ? voice.open_models.map(String) : [];
    paintBubble('voice', 'voice.html', standing.length, Number(voice.open_bytes || 0), standing,
      'Voice models standing in memory');
  };

  // The standard header chips from a merged /api/monitor reading, identical on
  // every page: CPU and machine memory (two rows in the cpu chip), GPU and its
  // memory (two rows in the gpu chip), and jobs running. What the engines hold
  // open in memory is no longer a header word - each studio tab wears that
  // count as its own bubble, mounted below. Only ever drawn from a valid
  // reading - live, or a copy this browser kept while it still described the
  // machine.
  UI.monitorChrome = function (snapshot) {
    const system = (snapshot && snapshot.system) || {};
    engineBubbles(system);

    const poolPercent = function (used, total) {
      return total > 0 ? Math.round((used / total) * 100) : 0;
    };
    const level = function (used, total) {
      return UI.sizeText(used) + ' used (' + poolPercent(used, total) + '%)';
    };

    const cpu = system.cpu || {};
    let memTotal = Number((system.memory || {}).total_bytes || 0);
    let memFree = Number((system.memory || {}).free_bytes || 0);
    if (!(memTotal > 0)) {
      const cpuDev = (system.devices || []).find((d) => d.type === 'cpu');
      if (cpuDev) {
        memTotal = Number(cpuDev.total_bytes || 0);
        memFree = Number(cpuDev.free_bytes || 0);
      }
    }
    const cpuLine = cpu.name ? String(cpu.name).trim() + ' · ' + (cpu.cores || '?') + ' cores'
      : 'cpu: not reported';
    const memLine = memTotal > 0 ? 'memory: ' + UI.sizeText(memTotal) + ' total · ' + level(memTotal - memFree, memTotal)
      : 'memory: not reported';
    UI.chip(E('chip-cpu'), cpuLine + '\n' + memLine,
      (cpu.name && memTotal > 0) ? 'ok' : 'warn',
      'The processor and cores of the server machine, and its physical memory usage — read from the' +
      ' machine, not the browser.',
      poolPercent(memTotal - memFree, memTotal));

    const gpu = (system.devices || []).find((d) => d.type === 'gpu' || d.type === 'accel');
    const vramTotal = gpu ? Number(gpu.total_bytes || 0) : 0;
    const gpuLine = gpu ? String(gpu.name) + ' ' + String(gpu.description).trim() : 'gpu: no gpu driver';
    const vramLine = vramTotal > 0 ? 'vram: ' + UI.sizeText(vramTotal) + ' total · ' + level(vramTotal - Number(gpu.free_bytes || 0), vramTotal)
      : 'vram: not reported';
    UI.chip(E('chip-gpu'), gpuLine + '\n' + vramLine,
      gpu ? 'ok' : 'warn',
      gpu ? 'The GPU this server sees and its driver report, with memory usage for that device — read from the machine, not the browser.'
        : 'No GPU driver was found on this machine.',
      (gpu && vramTotal) ? poolPercent(vramTotal - Number(gpu.free_bytes || 0), vramTotal) : 0);

    const jobs = (snapshot && Array.isArray(snapshot.jobs)) ? snapshot.jobs : [];

    const active = jobs.filter((j) => j.state === 'running');
    UI.chip(E('chip-jobs'),
      active.length + ' running',
      active.length ? 'warn' : 'ok',
      'Jobs every engine currently has running — read from the machine now.');

    // The code version, exactly as the build named it: a development build is
    // the dev tag and carries its compile time only - its code is a working
    // tree; a released build names the commit its code stands on beside that
    // same compile time, so the artifact is tied to one re-creatable source
    // tree. A build that named nothing says so; no version is ever guessed.
    const version = String(system.version || '');
    const compiledAt = String(system.compiled || '');
    const dev = version === 'dev';
    const commit = /^[0-9a-f]{40}$/i.test(version) ? version : '';
    let versionWords = 'code: not stated\nthis binary carries no version';
    let versionTip = 'This binary carries no version: the build that made it named none. Nothing here can say which code it runs.';
    let versionKind = 'warn';
    if (dev) {
      versionWords = 'code: dev\ncompiled ' + (compiledAt.length ? compiledAt : 'not stated');
      versionTip = 'Development build: the version is dev, compiled ' +
        (compiledAt.length ? compiledAt : 'at a time this binary does not state') +
        '. This is the code of a working tree, not a released version.';
      versionKind = 'warn';
    }
    else if (commit) {
      versionWords = 'code: rel ' + commit.slice(0, 12) +
        '\ncompiled ' + (compiledAt.length ? compiledAt : 'not stated');
      versionTip = 'The exact code this server runs: the version is the release commit ' + commit +
        ', compiled ' + (compiledAt.length ? compiledAt : 'at a time this binary does not state') +
        '. That commit names the one source tree this binary can be re-created from; ' +
        'the pages, the start-up line, and this chip name the same code.';
      versionKind = 'ok';
    }
    UI.chip(E('chip-version'), versionWords, versionKind, versionTip);
  };

  // ---------- shared controls ----------
  // A chip's optional level paints as a fill rising from its bottom; a fill may
  // carry the colour of the engine it is about, and one that names no colour
  // stays the neutral wash the sheet falls back to.
  UI.chip = function (node, text, kind, tip, fill, tint) {
    if (!node) return;
    // One line per <span class="cr"> row so multi-line text reads across.
    node.textContent = '';
    const lines = String(text).split('\n');
    for (const line of lines) {
      const row = document.createElement('span');
      row.className = 'cr';
      row.textContent = line;
      node.appendChild(row);
    }
    node.className = 'chip ' + (kind || '');
    if (tip !== undefined) node.title = tip;
    node.style.removeProperty('--fill');
    node.style.removeProperty('--fill-tint');
    const at = Math.round(Number(fill));
    if (Number.isFinite(at) && at > 0) node.style.setProperty('--fill', Math.min(at, 100) + '%');
    if (tint) node.style.setProperty('--fill-tint', String(tint));
  };

  // A card's fact row: a short state/tag/size shown down the card's right
  // edge, hairline-separated, hoverable for its detail. All pages share this.
  // The optional mark names one of the shapes style.css draws at low contrast
  // under the row's words, so a control can be recognised by its shape as well
  // as by its text; the mark sits behind the first letters and never replaces
  // them.
  UI.finger = function (text, kind, tip, mark) {
    const row = document.createElement('div');
    row.className = 'finger' + (kind ? ' ' + kind : '');
    row.textContent = text;
    if (tip !== undefined) row.title = tip;
    if (mark) row.dataset.icon = mark;
    return row;
  };

  // A fact row that is an action: styled like a fact row but it is a button,
  // and tapping it flashes first so the action reads as taken on screen.
  UI.fingerAct = function (text, handler, tip, mark) {
    const row = document.createElement('button');
    row.className = 'finger';
    row.textContent = text;
    row.addEventListener('click', function () {
      UI.flash(row);
      handler();
    });
    if (tip !== undefined) row.title = tip;
    if (mark) row.dataset.icon = mark;
    return row;
  };

  // The shell every card is drawn in: its words on the left and its facts down
  // the right edge, so a fact runs to the card's border on every page. A page
  // that builds a card without this shell draws facts that stop in mid-card.
  // The extra names a card may carry - lit, chosen, framed - go on the card; a
  // page of its own design passes its own card class instead.
  UI.cardShell = function (extra, base) {
    const card = document.createElement('div');
    card.className = (base || 'model-card') + (extra ? ' ' + extra : '');
    const main = document.createElement('div');
    main.className = 'model-main';
    const fingers = document.createElement('div');
    fingers.className = 'model-fingers';
    card.appendChild(main);
    card.appendChild(fingers);
    return { card: card, main: main, fingers: fingers };
  };

  // What the pages say as they work. One page keeps the written record - the
  // home page, whose whole job is what this machine is doing - and every page
  // writes into the same store, so a line raised on the image studio or the
  // voice studio is read again there rather than being forgotten when that page
  // is left. A line that stopped or refused is also put on the notice stack of
  // the page that raised it, because that is where the hand that caused it was.
  const LOG_STORE = 'ols-log-v1';
  const LOG_KEEP = 200;
  const LOG_PAGE_WORDS = {
    '': 'status', index: 'status', imagegen: 'image', voice: 'voice',
  };

  function logWords() {
    const at = String(location.pathname).lastIndexOf('/');
    const file = decodeURIComponent(String(location.pathname).slice(at + 1));
    const stem = file.replace(/\.html?$/i, '');
    return LOG_PAGE_WORDS[stem] || stem || 'page';
  }

  function logRead() {
    try {
      const kept = JSON.parse(localStorage.getItem(LOG_STORE) || 'null');
      return Array.isArray(kept) ? kept : [];
    } catch (badStore) {
      return [];
    }
  }

  UI.log = function (text, kind) {
    const words = String(text);
    const kept = logRead();
    kept.push({ at: Date.now(), page: logWords(), kind: kind || '', words: words });
    try {
      localStorage.setItem(LOG_STORE, JSON.stringify(kept.slice(-LOG_KEEP)));
    } catch (storageFull) {
      // A store that will not take the record loses it; the page still says the
      // line the same way it did before the record existed.
    }
    // A page that draws the whole log from the record says the line itself.
    if (E('log')) return;
    // A page with one line to speak with says the newest line there, in the
    // line's own colour, rather than stacking toasts over the work it describes.
    const spoken = E('status-line');
    if (spoken) {
      spoken.textContent = words;
      spoken.className = 'status-line' + (kind ? ' ' + kind : '');
      return;
    }
    // With nowhere else to say it, only a hand that needs an answer makes noise.
    if (kind === 'err' || kind === 'warn') UI.toastPush(words, kind);
  };

  // The record as the page that draws it wants it: newest last, the plainest
  // lines dropped first. Nothing here is invented - a line the pages never wrote
  // is not a line this page may show.
  UI.logLines = function (limit) {
    const kept = logRead();
    const at = Number(limit) > 0 ? Number(limit) : LOG_KEEP;
    return kept.length > at ? kept.slice(-at) : kept;
  };

  UI.logClear = function () {
    try {
      localStorage.removeItem(LOG_STORE);
    } catch (badStore) {
    }
  };

  // A hand that was refused is the one moment a person must see, so the page
  // that pressed it says so on the stack in plain words, and keeps the machine's
  // own reason verbatim beside it - the reason is the machine's, never rewritten.
  UI.toastNo = function (whatWords, answer) {
    const reason = UI.askWhy(answer);
    const line = 'Could not ' + String(whatWords) + (reason ? ': ' + reason : '.');
    UI.toastPush(line, 'err');
  };

  // The immediate answer a control gives when taken: it flashes, so a press is
  // visibly registered. The page may still be working on the request.
  UI.flash = function (node) {
    if (!node) return;
    node.classList.add('act-flash');
    setTimeout(function () { node.classList.remove('act-flash'); }, 700);
  };

  // Wire a click handler to a control: the press flashes, then runs the
  // handler. Promise-returning handlers are started, not awaited, so page
  // order stays with the page.
  UI.onPress = function (node, handler) {
    if (!node) return;
    node.addEventListener('click', function () {
      UI.flash(node);
      handler();
    });
  };

  // Wire a change handler to a dropdown field: the field flashes when a value
  // is chosen, and the handler applies it. A page that repopulates the field
  // itself raises the same event, so it says so with a flag of its own and its
  // handler leaves the screen alone - a page matching machine state is not a
  // human choice.
  UI.onChoose = function (node, handler) {
    if (!node) return;
    node.addEventListener('change', function () {
      UI.flash(node);
      handler();
    });
  };

  // Draw a file the machine holds from the machine's own address. The file doors
  // answer that such a file does not change - a year, marked immutable - so a
  // browser that has drawn it keeps it, and a page reload asks the machine for
  // nothing. The credential of the visit rides on the connection as the cookie
  // the door wrote, so the address stays a bare path. Nothing that has an
  // address is copied into this page's store.
  UI.fileSrc = function (path) {
    const store = window.olsCache;
    if (store) {
      // Only a file this page made of its own bytes is drawn from those bytes:
      // the machine has no address for what was made here.
      const madeHere = store.file(path);
      if (madeHere) return madeHere;
    }
    return path;
  };

  // Keep a picture this page made of bytes it read here - a dragged file, a
  // clipboard picture - under the name the machine gave it, so drawing it does
  // not decode it a second time while this page is open.
  UI.fileKeep = function (path, data, type) {
    const store = window.olsCache;
    if (store) store.keep(path, data, type);
  };

  UI.textField = function (id) {
    const node = E(id);
    return node ? String(node.value || '') : '';
  };

  UI.numberField = function (id, fallback) {
    const text = UI.textField(id).trim();
    if (text === '') return fallback;
    const value = Number(text);
    return Number.isFinite(value) ? value : fallback;
  };

  UI.isOn = function (id) {
    const node = E(id);
    return !!node && node.classList.contains('on');
  };

  // A <select> has no value of its own: the option a person chose is the one
  // at selectedIndex (what the screen shows). The selected attribute marks the
  // markup default, not the choice, so read the option item, not the select.
  UI.optionValue = function (option) {
    if (!option) return '';
    const direct = option.value;
    if (typeof direct === 'string') return direct;
    if (option.getAttribute) {
      const attr = option.getAttribute('value');
      return attr === null || attr === true ? '' : String(attr);
    }
    return '';
  };

  UI.optionList = function (select) {
    if (!select || !select.options) return [];
    const options = select.options;
    const list = [];
    for (let at = 0; at < options.length; ++at) {
      list.push(options.item ? options.item(at) : options[at]);
    }
    return list;
  };

  UI.chosenOption = function (select) {
    const list = UI.optionList(select);
    if (!list.length) return '';
    let at = Number(select ? select.selectedIndex : -1);
    if (!Number.isInteger(at) || at < 0 || at >= list.length) at = 0;
    return UI.optionValue(list[at]);
  };

  UI.selectValue = function (id) {
    return UI.chosenOption(E(id));
  };

  // Restore a choice after refilling a list: an unmatched value falls back to
  // the first option, which is what the browser itself shows, so the form and
  // the screen cannot disagree.
  UI.chooseOption = function (select, value) {
    const list = UI.optionList(select);
    if (!list.length) return;
    const wanted = String(value || '');
    let hit = -1;
    for (let at = 0; at < list.length; ++at) {
      if (UI.optionValue(list[at]) === wanted) { hit = at; break; }
    }
    if (hit < 0) hit = 0;
    for (let at = 0; at < list.length; ++at) {
      list[at].selected = at === hit;
    }
  };

  // ---------- formatters ----------
  UI.sizeText = function (bytes) {
    const value = Number(bytes);
    if (!Number.isFinite(value) || value <= 0) return '';
    if (value >= 1073741824) return (value / 1073741824).toFixed(1) + ' GB';
    if (value >= 1048576) return Math.round(value / 1048576) + ' MB';
    if (value >= 1024) return Math.round(value / 1024) + ' KB';
    return value + ' B';
  };

  UI.pad2 = function (n) {
    return (n < 10 ? '0' : '') + n;
  };

  // Seconds-since-epoch to a clock string; job and gallery records keep
  // seconds, browser readings keep milliseconds (clockMs below).
  UI.clock = function (epochSeconds) {
    if (typeof epochSeconds !== 'number' || epochSeconds <= 0) return '';
    return new Date(epochSeconds * 1000).toLocaleTimeString();
  };

  UI.clockMs = function (epochMillis) {
    if (typeof epochMillis !== 'number' || epochMillis <= 0) return '';
    return new Date(epochMillis).toLocaleTimeString();
  };

  UI.whenText = function (createdSeconds) {
    const value = Number(createdSeconds || 0);
    if (!value) return '';
    const d = new Date(value * 1000);
    return d.getFullYear() + '-' + UI.pad2(d.getMonth() + 1) + '-' + UI.pad2(d.getDate()) +
      ' ' + UI.pad2(d.getHours()) + ':' + UI.pad2(d.getMinutes());
  };

  // the largest request body the server reads: a page that sends past it gets a
  // refusal of its own, so an ask is kept whole or named as too large rather
  // than half carried. a page that must fit a body derives its own margin.
  UI.bodyBytes = 2 * 1024 * 1024;

  // Whether a body would fit one door, measured exactly as it will travel - the
  // words in it counted where they belong, not a fixed allowance - so a long ask
  // or a whole document's words are part of the count. A body that would not fit
  // is answered here with the code the door gives for that same refusal, which
  // saves the browser uploading two megabytes only to be told they were too many,
  // and the words name which part of the body carries the most, because naming it
  // is what tells a person which of their own words or files to trim. Answers
  // null when the body would fit.
  UI.tooBigBody = function (body) {
    const text = JSON.stringify(body === undefined ? {} : body);
    if (text.length <= UI.bodyBytes) return null;
    let widest = '';
    let widestLength = 0;
    if (body) {
      for (const key in body) {
        const value = body[key];
        const length = typeof value === 'string' ? value.length : JSON.stringify(value).length;
        if (length > widestLength) {
          widestLength = length;
          widest = key;
        }
      }
    }
    const holdsAFile = widest === 'image_b64' || widest === 'audio_b64' || widest === 'reference_b64';
    return {
      status: 413,
      error: holdsAFile
        ? 'that file is bigger than one request can carry: the request stands at ' +
        UI.sizeText(text.length) + ', and a door reads at most two megabytes at a time. trim the file, or pick a shorter part of it.'
        : 'the words this request carries are more than one door can read: the request stands at ' +
        UI.sizeText(text.length) + ', and a door reads at most two megabytes at a time. send a shorter passage, or fewer words.',
    };
  };

  UI.clip = function (text, width) {
    const value = String(text || '');
    if (value.length <= width) return value;
    return value.slice(0, width - 1) + '…';
  };

  // ---------- api and jobs ----------
  // An API answer that is not well-formed reads as no answer. The status code
  // of the last answer is kept beside it: a machine whose door refused the visit
  // and a machine that is absent are different facts, and pages say which happened.
  UI.apiStatus = 0;
  UI.apiError = '';
  UI.api = function (path, method, body, opts) {
    return (async function () {
      UI.apiStatus = 0;
      UI.apiError = '';
      const init = {
        method: method || 'GET',
        body: body === undefined ? undefined : JSON.stringify(body),
        headers: body === undefined ? undefined : { 'content-type': 'application/json' },
      };
      // A reading a page takes to find out what changed may not be answered from
      // a kept copy of the answer; {cache: 'no-store'} says so to the fetch, and
      // to this machine's own browser cache, which reads the same word.
      if (opts && opts.cache) init.cache = String(opts.cache);
      const res = await fetch(path, init);
      UI.apiStatus = res.status;
      // a refusal carries the reason the server gave, so a page can repeat what
      // it said rather than guess why its own ask was turned down.
      if (!res.ok) {
        try {
          const bad = await res.json();
          UI.apiError = bad && bad.error ? String(bad.error) : '';
        } catch (badBody) {
        }
        return null;
      }
      try {
        return await res.json();
      } catch (badBody) {
        return null;
      }
    })();
  };

  // a door that only answers yes or no, with no job behind it: a kept recording,
  // a name moved, a card put down. {ok: true} when the door said yes, and
  // {ok: false, error: words} when it said no in its own words - a page repeats
  // those rather than calling a kept recording a failure. A door that starts a
  // job answers differently, and pages read it with postJob.
  UI.actDoor = function (path, body) {
    return (async function () {
      const tooBig = UI.tooBigBody(body);
      if (tooBig) return tooBig;
      const res = await fetch(path, {
        method: 'POST',
        body: JSON.stringify(body === undefined ? {} : body),
        headers: { 'content-type': 'application/json' },
      });
      if (res.ok) return { ok: true };
      let words = '';
      try {
        const bad = await res.json();
        words = bad && bad.error ? String(bad.error) : '';
      } catch (badBody) {
      }
      return { ok: false, error: words, status: res.status };
    })();
  };

  // start a job and answer with either the job id or the refusal:
  // {job: id} on a good start, {status: code, error: text} otherwise
  UI.postJob = function (path, body) {
    return (async function () {
      const tooBig = UI.tooBigBody(body);
      if (tooBig) return tooBig;
      const res = await fetch(path, {
        method: 'POST',
        body: JSON.stringify(body === undefined ? {} : body),
        headers: { 'content-type': 'application/json' },
      });
      let parsed = null;
      try {
        parsed = await res.json();
      } catch (badBody) {
      }
      if (res.ok && parsed && parsed.job_id) return { job: parsed.job_id };
      return { status: res.status, error: parsed && parsed.error ? String(parsed.error) : '' };
    })();
  };

  // Why an ask was refused, in words a person can act on. Doors answer with their
  // own words most of the time; one that answers without them still states a
  // status, and the status names what to do next.
  UI.askWhy = function (answer) {
    if (typeof answer === 'string') return answer || 'no reason given';
    const said = answer && answer.error ? String(answer.error) : '';
    if (said) return said;
    const code = Number(answer && answer.status || 0);
    if (code === 429) return 'the machine is busy and asked for a moment between requests';
    if (code === 401 || code === 403) return 'this page was not let through the door of this machine';
    if (code === 404) return 'that door is not on this machine';
    if (code === 0) return 'the machine gave no answer at all';
    return 'no reason given';
  };

  // The event stream of a running job: SSE frames arrive as "data: {...}"
  // blocks until an "event: end" frame; a frame that fails to parse is skipped
  // without losing the run. Answers false only if the stream itself is absent.
  UI.streamEvents = function (url, onEvent) {
    return (async function () {
      const res = await fetch(url);
      if (!res.ok) return false;
      const reader = res.body.getReader();
      const decoder = new TextDecoder();
      let raw = '';
      while (true) {
        const chunk = await reader.read();
        if (chunk.done) break;
        raw += decoder.decode(chunk.value, { stream: true });
        let split = raw.indexOf('\n\n');
        while (split >= 0) {
          const frame = raw.slice(0, split);
          raw = raw.slice(split + 2);
          if (frame.indexOf('event: end') === 0) return true;
          if (frame.indexOf('data: ') === 0) {
            try {
              onEvent(JSON.parse(frame.slice(6)));
            } catch (badFrame) {
            }
          }
          split = raw.indexOf('\n\n');
        }
      }
      return true;
    })();
  };

  UI.sleep = function (ms) {
    return new Promise(function (done) { setTimeout(done, ms); });
  };

  // ---------- the timed stack ----------
  // A thing a person should see once - a hand that was refused, an engine that
  // just opened or would not, a limit that stopped an ask - lives here rather
  // than in a line of words that stays on the page. the stack hangs in the top
  // -right corner, newest on top, and keeps at most a few at a time; each one
  // carries its own countdown: a bar that empties as its time runs out, the
  // seconds left stated beside it, and two hands of its own - one pauses the
  // countdown, one closes it. the same words already in the stack keep their
  // place rather than repeating, and words answered again inside a quiet window
  // raise nothing at all, so a condition the machine keeps stating is not
  // announced over and over - the page's own rail still holds every line.
  UI.toastKinds = ['ok', 'warn', 'err', 'wait'];
  UI.toastStack = null;
  UI.toastLive = [];
  UI.toastRecent = {};
  UI.toastTicker = null;
  UI.toastAt = 0;
  // The room the stack keeps for work that lives as long as it runs - a page
  // that lays out a batch of its own raises this to the room its batch needs.
  UI.toastTrackedCap = 24;

  UI.toastBuild = function () {
    if (UI.toastStack) return;
    const stack = document.createElement('div');
    stack.id = 'toasts';
    stack.setAttribute('title',
      'what this machine told you this moment, and how long is left to read it');
    const all = document.createElement('button');
    all.className = 'toast-close-all';
    all.textContent = 'close all';
    all.setAttribute('title', 'take everything off the stack now');
    all.addEventListener('click', function () {
      for (const live of UI.toastLive.slice()) UI.toastClose(live);
    });
    stack.appendChild(all);
    document.body.appendChild(stack);
    UI.toastStack = stack;
    UI.toastAt = Date.now();
    UI.toastTicker = setInterval(UI.toastTick, 250);
  };

  UI.toastClose = function (live) {
    if (!live || live.closing) return;
    live.closing = Date.now();
    live.el.classList.add('closing');
    if (live.leftEl) live.leftEl.textContent = 'closed';
  };

  UI.toastTick = function () {
    const now = Date.now();
    const since = Math.max(0, now - UI.toastAt);
    UI.toastAt = now;
    for (const live of UI.toastLive.slice()) {
      if (!live.closing) {
        if (live.counted) {
          // a job's bar counts its own steps, and its time is the job's, not a
          // countdown: the notice stays while the work runs and is taken down
          // when the work says it is over.
          if (live.opened) {
            // work that states no scale - a task waiting its turn, an engine
            // being opened - keeps the moving bar the app draws for work that
            // cannot be counted, and states the stage rather than a time.
            if (live.leftEl) live.leftEl.textContent = live.paused ? 'paused' : 'waiting to start';
          } else if (live.eta) {
            // the bar of a running job is its estimated time left, and the
            // estimate comes from the machine's measured rate: a page that
            // measured none states no time.
            if (!live.paused) live.left -= since;
            if (live.fill) {
              const share = live.left > 0 ? Math.round((live.left / live.total) * 100) : 0;
              live.fill.style.width = share + '%';
            }
            if (live.leftEl) {
              if (live.paused) live.leftEl.textContent = 'paused';
              else if (live.left > 0) live.leftEl.textContent = 'about ' + Math.ceil(live.left / 1000) + 's left';
              else live.leftEl.textContent = 'still running';
            }
          } else {
            if (live.fill && live.total > 0) {
              const share = live.done > live.total ? 100 : Math.round((live.done / live.total) * 100);
              live.fill.style.width = share + '%';
            }
            if (live.leftEl) {
              // work whose scale nothing stated is not counted as a step of zero:
              // it says its stage, which is all the machine stated about it.
              live.leftEl.textContent = live.paused ? 'paused'
                : (live.total > 0
                  ? 'step ' + live.done + ' of ' + live.total
                  : 'waiting to start');
            }
          }
        } else {
          if (!live.paused) live.left -= since;
          if (live.fill) {
            const share = live.left > 0 ? Math.round((live.left / live.total) * 100) : 0;
            live.fill.style.width = share + '%';
          }
          if (live.leftEl) {
            if (live.paused) live.leftEl.textContent = 'paused';
            else if (live.left > 0) live.leftEl.textContent = Math.ceil(live.left / 1000) + 's left';
            else live.leftEl.textContent = '';
          }
          if (live.left <= 0 && !live.paused) live.closing = now;
        }
        if (live.closing) live.el.classList.add('closing');
      }
      if (live.closing && now - live.closing > 520) {
        UI.toastLive.splice(UI.toastLive.indexOf(live), 1);
        if (live.el) live.el.remove();
      }
    }
    if (!UI.toastLive.length) {
      if (UI.toastTicker) clearInterval(UI.toastTicker);
      UI.toastTicker = null;
      const stack = UI.toastStack;
      UI.toastStack = null;
      if (stack) stack.remove();
    }
  };

  // Put one thing on the stack. kind names only the colour the app already uses
  // for that state; the words are the machine's or the page's own, never made up
  // here. secs is the reading time, and a notice about something that failed
  // keeps a little more of it.
  UI.toastPush = function (words, kind, secs, key) {
    const line = String(words || '');
    if (!line) return null;
    if (UI.toastKinds.indexOf(kind) < 0) kind = 'warn';
    const wanted = key === null || key === undefined ? '' : String(key);
    UI.toastBuild();
    if (!UI.toastStack) return null;
    const life = Math.max(2, Math.round(Number(secs) || (kind === 'err' ? 14 : 9))) * 1000;
    if (wanted) {
      // a job keeps one notice through its whole life: the same key updates the
      // words and the bar rather than stacking another notice for the same work.
      for (const live of UI.toastLive) {
        if (live.key !== wanted || live.closing) continue;
        live.words = line;
        if (live.wordsEl) live.wordsEl.textContent = line;
        live.kind = kind;
        if (live.el) {
          live.el.className = 'toast ' + kind + (live.paused ? ' paused' : '');
        }
        return live;
      }
    } else {
      // The same words already on the stack keep their place rather than repeating,
      // and words answered again inside a quiet window - twice the notice's own
      // reading time - raise nothing at all, so a condition the machine keeps
      // stating is not announced over and over. The window is kept as the moment it
      // ends, and windows that have passed are dropped here, so this map stays as
      // small as the words still quiet rather than growing with everything the
      // machine ever stated.
      const now = Date.now();
      for (const quiet in UI.toastRecent) {
        if (Number(UI.toastRecent[quiet]) <= now) delete UI.toastRecent[quiet];
      }
      const last = UI.toastRecent[line];
      if (last && now < last) return null;
      for (const live of UI.toastLive) {
        // an outcome that is already being taken off the stack does not answer a
        // fresh notice with the same words.
        if (live.words !== line || live.closing) continue;
        live.left = life;
        live.total = life;
        return live;
      }
      UI.toastRecent[line] = now + life * 2;
    }
    const card = document.createElement('div');
    card.className = 'toast ' + kind;
    const bar = document.createElement('div');
    bar.className = 'toast-bar';
    const fill = document.createElement('i');
    fill.className = 'toast-fill';
    fill.style.width = '100%';
    bar.appendChild(fill);
    card.appendChild(bar);
    const said = document.createElement('span');
    said.className = 'toast-words';
    said.textContent = line;
    card.appendChild(said);
    const left = document.createElement('span');
    left.className = 'toast-left';
    card.appendChild(left);
    const pause = document.createElement('button');
    pause.className = 'toast-pause';
    pause.setAttribute('title', 'hold this one still: its time and its bar stop moving');
    pause.textContent = 'pause';
    card.appendChild(pause);
    const close = document.createElement('button');
    close.className = 'toast-close';
    close.setAttribute('title', 'take this one off the stack');
    close.textContent = 'close';
    card.appendChild(close);
    UI.toastStack.insertBefore(card, UI.toastStack.firstChild);
    const record = {
      el: card, fill: fill, leftEl: left, wordsEl: said, words: line,
      key: wanted, total: life, left: life
    };
    UI.toastLive.push(record);
    pause.addEventListener('click', function () {
      record.paused = !record.paused;
      record.el.classList.toggle('paused', record.paused);
      UI.toastTick();
    });
    close.addEventListener('click', function () {
      UI.toastClose(record);
    });
    while (true) {
      // a notice that is only worth one look is kept few; a job's own notice is
      // kept while the job runs, since it is the running work that a person is
      // watching. A quick batch is one notice per picture it asked for - the room
      // is that many pictures, the biggest batch this studio asks for, plus headroom
      // for the work another window asked for on the same machine. past a room full,
      // the oldest of that kind is taken down.
      // A notice already on its way off the stack is not counted and is not a
      // candidate: taking one down only starts its closing - it leaves the list
      // when the ticker has run past that animation - so counting the ones being
      // taken down would keep the totals at the same number forever, and this loop
      // would never end: the page stops answering rather than trimming the stack.
      let notices = 0;
      let tracked = 0;
      for (const live of UI.toastLive) {
        if (live.closing) continue;
        if (live.counted) ++tracked;
        else ++notices;
      }
      if (notices > 4 || tracked > UI.toastTrackedCap) {
        let oldest = null;
        // past half the room for one kind, that kind is the one over-represented,
        // and its oldest notice is taken down; a page that raised the room for its
        // own batch keeps that batch whole.
        const overTracked = tracked * 2 > UI.toastTrackedCap;
        for (const live of UI.toastLive) {
          if (live.closing) continue;
          if (!!live.counted === overTracked) oldest = live;
        }
        if (!oldest) break;
        UI.toastClose(oldest);
        continue;
      }
      break;
    }
    return record;
  };

  // A running job lives on the stack as long as it runs: its bar counts the
  // steps the job itself states, and its words follow the job through its life -
  // taken, opened, steps answered, finished, refused - in the job's own record,
  // never guessed. Work that has stated no scale yet states its stage rather than
  // a step count nothing stated.
  UI.toastTrack = function (key, words, kind) {
    const wanted = key === null || key === undefined ? '' : String(key);
    if (!wanted) return null;
    for (const live of UI.toastLive) {
      if (live.key !== wanted) continue;
      if (words && live.wordsEl) live.wordsEl.textContent = String(words);
      return live;
    }
    const live = UI.toastPush(String(words || 'Working'), kind || 'wait', 0, wanted);
    if (!live) return null;
    live.counted = true;
    live.opened = true;
    live.done = 0;
    return live;
  };

  // Say where a tracked job stands now: its own done/max, its own state words.
  UI.toastStep = function (live, done, max, words) {
    if (!live || live.paused) return;
    if (max > 0) {
      live.total = max;
      // a stated scale replaces the stage: the count bar is what the work costs
      // now, and the moving bar of work that states no scale is not drawn for it.
      if (live.opened) {
        live.opened = false;
        if (live.el) live.el.classList.remove('toast-open');
      }
    }
    live.done = Math.max(0, Number(done) || 0);
    if (live.wordsEl && words) live.wordsEl.textContent = String(words);
    UI.toastTick();
  };

  // Work that states no scale yet: a queued task, an engine being opened. Its
  // bar is the moving one the app already draws for work that cannot be counted,
  // and no time is invented for it.
  UI.toastOpen = function (live) {
    if (!live) return;
    live.opened = true;
    if (live.el) live.el.classList.add('toast-open');
    if (live.fill) live.fill.style.width = '';
  };

  // A job's own card, drawn below the rows it belongs to when a reader presses
  // one. The card is one uniform shape whatever the job is: what it works with
  // named at the top left in the loudest type, the job it is named by across the
  // middle of that top line in italic, and the seconds it has stood here in the
  // right hand - the three things a person reads first. Everything else the
  // machine states about that job is drawn under that line as labelled fields,
  // and a field the machine does not state is not drawn at all. The pair of hands
  // on the top line opens or closes that body, so a reader can keep the card named
  // while deciding how much of it to read.
  UI.jobCard = function (host, head, fields) {
    if (!host) return;
    const top = document.createElement('div');
    top.className = 'job-card-head';
    for (const part of [['job-card-model', head.model], ['job-card-id', head.id], ['job-card-time', head.time]]) {
      if (!part || !part[1]) continue;
      const cell = document.createElement('span');
      cell.className = part[0];
      cell.textContent = String(part[1]);
      top.appendChild(cell);
    }
    host.appendChild(top);
    const body = document.createElement('div');
    body.className = 'job-card-fields';
    for (const field of fields) {
      if (!field || !field[1]) continue;
      const row = document.createElement('div');
      row.className = 'job-card-field';
      const label = document.createElement('span');
      label.className = 'job-card-field-label';
      label.textContent = String(field[0]);
      const value = document.createElement('span');
      value.className = 'job-card-field-value';
      value.textContent = String(field[1]);
      row.appendChild(label);
      row.appendChild(value);
      body.appendChild(row);
    }
    host.appendChild(body);
    const hands = function (className, words, title, open) {
      const hand = document.createElement('button');
      hand.className = 'job-card-hand ' + className;
      hand.textContent = words;
      hand.setAttribute('title', title);
      hand.addEventListener('click', function () {
        body.hidden = !open;
      });
      top.appendChild(hand);
    };
    hands('job-card-open', '+', 'read the whole card: every field the machine stated', true);
    hands('job-card-close', '-', 'keep the top line and take the rest away', false);
  };

  // The bar of running work is its estimated time left, and the estimate is a
  // measured number: a page passes the seconds it derived from the machine's own
  // records of this model's recent runs. A page that measured none calls this
  // with zero, which keeps the count-only bar and says no time.
  UI.toastEta = function (live, ms, words) {
    if (!live) return;
    if (words && live.wordsEl) live.wordsEl.textContent = String(words);
    const estimate = Math.max(0, Math.round(Number(ms) || 0));
    if (!estimate) {
      live.eta = false;
      live.opened = false;
      if (live.el) live.el.classList.remove('toast-open');
      UI.toastTick();
      return;
    }
    live.eta = true;
    live.opened = false;
    live.total = estimate;
    live.left = estimate;
    if (live.el) {
      live.el.classList.remove('toast-open');
    }
    UI.toastTick();
  };

  // The end of a tracked job: the running notice is taken down, and the outcome
  // is handed to the stack as its own notice, so a person can read what the work
  // ended as rather than watch it fade away.
  UI.toastEnd = function (live, words, kind) {
    if (live) {
      if (live.wordsEl && words) live.wordsEl.textContent = String(words);
      if (live.leftEl) live.leftEl.textContent = String(words || '');
      live.closing = Date.now();
      if (live.el) live.el.classList.add('closing');
    }
    if (!words) return;
    if (UI.toastKinds.indexOf(kind) < 0) kind = 'ok';
    UI.toastPush(String(words), kind);
  };

  // The self-refreshing page: read, paint, wait, repeat. A failed read calls
  // onError with the page's own words about what is unknown; the next read is
  // still tried.
  UI.poll = async function (refresh, onError, intervalMs) {
    while (true) {
      try {
        await refresh();
      } catch (failed) {
        onError(failed);
      }
      await UI.sleep(intervalMs);
    }
  };

  // How often a studio re-reads the machine. A page's reading is a whole set of
  // answers, so it waits rather than crowding the door: four seconds is soon
  // enough to see a job start and long enough to leave the machine alone. A page
  // that shows the machine's own live state names its own pace beside it.
  UI.pollMs = 4000;

  // ---------- progress bars ----------
  // A bar may state its kind: "load" is indeterminate work and draws moving
  // striping; other bars track counted steps.
  UI.progressSet = function (panel, key, done, max, text, kind) {
    if (!panel || !key) return;
    let row = panel.querySelector('[data-job="' + key + '"]');
    if (!row) {
      row = document.createElement('div');
      row.className = 'prog-row';
      row.dataset.job = key;
      const bar = document.createElement('div');
      bar.className = 'prog-bar';
      const fill = document.createElement('div');
      fill.className = 'prog-fill' + (kind === 'load' ? ' loading' : '');
      bar.appendChild(fill);
      const label = document.createElement('div');
      label.className = 'prog-text';
      row.appendChild(bar);
      row.appendChild(label);
      panel.appendChild(row);
    }
    const pct = max > 0 ? Math.min(100, Math.round((done / max) * 100)) : 0;
    row.querySelector('.prog-fill').style.width = pct + '%';
    row.querySelector('.prog-text').textContent = text;
  };

  UI.progressClear = function (panel, key) {
    if (!panel) return;
    const row = panel.querySelector('[data-job="' + key + '"]');
    if (row) panel.removeChild(row);
  };

  // ---------- replay of this browser's last reading ----------
  // What this browser last read, so pages can paint machine state before the
  // first live answer. Within an hour the copy still describes this machine
  // and pages draw it as a plain valid reading; past an hour it describes
  // nothing and is not offered. This never answers an API request — only the
  // cache's live copy does that — so anything drawn from here is labeled with
  // the time it was read at.
  UI.lastReading = function (path) {
    const hit = window.olsCache ? window.olsCache.lastCopy(path) : null;
    if (!hit || hit.age > 3600000) return null;
    let data;
    try {
      data = JSON.parse(hit.text);
    } catch (badText) {
      return null;
    }
    return { data: data, at: hit.at, readTime: UI.clockMs(hit.at), live: hit.live };
  };

  // ---------- page chrome: the header bar and the nav, defined once ----------
  // Every page carries the same header (brand, the badge of the work going on,
  // running-state line) and the same nav, which also marks the page you are on.
  // Page content itself lives in each page's own file.
  // The home tab stands at the left, and the two studios read to its right: the
  // machine is what a person looks at first, and the page the bare address opens.
  const NAV = [
    ['index.html', 'home', 'This machine: a card for every model with what it uses, and every job the engines have run.'],
    ['imagegen.html', 'image studio', 'Generate images: models, prompts, gallery.'],
    ['voice.html', 'voice studio', 'Speak text with the audio.cpp engines: voices, models, renders.'],
  ];
  // The header carries only what every page needs about the work: what the
  // engines are doing. What they hold open in memory is not a header word -
  // each studio tab wears that count as its own bubble, and what an engine
  // stands in memory is stated where it is used. The machine's own badges - its
  // cpu, its gpu, the code it runs - are the home page's machine panel, and
  // that page's own markup carries them; where no such element stands, the
  // readings for them are simply not drawn.
  const CHIPS = [
    ['chip-jobs', 'jobs: not read'],
  ];

  UI.mountPageChrome = function () {
    if (E('topbar') || !document.body) return;
    const at = location.pathname.lastIndexOf('/');
    const served = at === location.pathname.length - 1 ? 'index.html' : location.pathname.slice(at + 1);
    let chips = '';
    for (const chip of CHIPS) {
      const lines = chip[1].split('\n');
      let rows = '';
      for (const line of lines) {
        rows += '<span class="cr">' + line + '</span>';
      }
      chips += '  <span class="chip" id="' + chip[0] + '">' + rows + '</span>\n';
    }
    let links = '';
    for (const link of NAV) {
      const current = link[0] === served;
      links += '    <a' + (current ? ' class="on" title="You are here."' : ' title="' + link[2] + '"') +
        ' href="' + link[0] + '">' + link[1] + '</a>\n';
    }
    document.body.insertAdjacentHTML('afterbegin',
      '<header id="topbar">\n' +
      '  <span class="brand">open-local-services</span>\n' +
      chips +
      '  <span class="head-status" id="head-status">nothing running</span>\n' +
      '</header>\n' +
      '<nav id="nav">\n' + links + '</nav>');
  };

  UI.mountPageChrome();
})();

// ---------- the machine picker ----------
// One machine's pages belong to the machine they were served by, and the fleet
// is only a hop away: when this machine knows of other servers of its suite -
// met at startup, by its own sweep or at its own door - the header carries a
// pick of them. Choosing one leaves this machine entirely: the browser opens
// the same room on that address, carrying this visit's credential once, in the
// one place a credential is ever named in an address. If the other machine
// keeps a different key it simply walks the visit to its own door, as honest
// as any stranger's visit. With no other server standing there is nothing to
// pick, and the header says nothing.
(function mountHostPick() {
  const top = document.getElementById('topbar');
  if (!top) return;
  // A jump that arrived by the picker still had the credential in its address;
  // the door wrote it onto the connection on the way in, and the room lets go
  // of it before anything else reads the address.
  if (window.location.search.indexOf('key=') >= 0) {
    try { window.history.replaceState(null, '', window.location.pathname); } catch (noHistory) {}
  }
  const read = function () {
    fetch('/api/peers').then(function (res) {
      // A machine too young to keep a table has none to read; the header stays
      // as it was rather than drawing a box of nothing.
      if (!res.ok) return null;
      return res.json();
    }).then(function (doc) {
    if (!doc || !Array.isArray(doc.peers)) return;
    const met = doc.peers.filter(function (row) {
      return row && row.online && Array.isArray(row.addrs) && row.addrs.length;
    });
    if (!met.length) return;
    const self = doc.self || {};
    const pick = document.createElement('select');
    pick.className = 'host-pick';
    pick.title = 'the machines of this suite that stood when this one started - ' +
      'choose one and this room opens there';
    const here = document.createElement('option');
    here.value = '';
    here.textContent = (self.name || window.location.hostname) + ' · here';
    pick.appendChild(here);
    for (const row of met) {
      const opt = document.createElement('option');
      const addr = String(row.addrs[0]);
      const port = Number(row.port) || 8765;
      opt.value = window.location.protocol + '//' + addr + ':' + port + window.location.pathname;
      const words = [];
      if (row.version) words.push(String(row.version));
      if (row.backend) words.push(String(row.backend));
      opt.textContent = (row.name || addr) + ' · ' + addr +
        (words.length ? ' · ' + words.join(' ') : '');
      opt.title = 'open this room on ' + addr + ':' + port +
        (row.name ? ' - ' + row.name : '');
      pick.appendChild(opt);
    }
    pick.onchange = function () {
      const to = pick.value;
      if (!to) return;
      let url = to;
      const carried = window.olsKey ? window.olsKey.get() : '';
      if (carried) url += '?key=' + encodeURIComponent(carried);
      window.location.href = url;
    };
    top.appendChild(pick);
    }).catch(function () { });
  };
  // That a machine keeps a table is a word its own door carries: the page
  // waits its few seconds for that answer and says nothing past them - the
  // doors not having answered yet is a table neither way.
  const waitFlag = function (tries) {
    const state = window.olsKeyState;
    if (state && state.peers === true) { read(); return; }
    if (state && state.peers === false) return;
    if (tries > 60) return;
    setTimeout(function () { waitFlag(tries + 1); }, 100);
  };
  waitFlag(0);
})();

// The corner clock: a seven-segment face in the header that blinks 12:00 like
// the one on a microwave whose clock nobody ever set. Every segment stands on
// the face while dark - the ghosts behind the lit ones are what make a real
// panel read as a panel - and the lighting of each is a class a script toggles.
// Tapping the face lets it tell the browser's real time, minute by minute;
// tapping again hands it back to blinking. No server is asked for the hour,
// because no server is keeping this clock.
(function mountClock() {
  const top = document.getElementById('topbar');
  if (!top) return;
  const SEG = {
    '0': 'abcdef', '1': 'bc', '2': 'abged', '3': 'abgcd', '4': 'afgbc',
    '5': 'afgcd', '6': 'afgedc', '7': 'abc', '8': 'abcdefg', '9': 'abcfgd', ' ': ''
  };
  const KEY = 'ols-clock-set';
  const isSet = () => { try { return localStorage.getItem(KEY) === '1'; } catch (e) { return false; } };
  const four = [];
  const face = document.createElement('div');
  face.className = 'clock7';
  face.title = 'a clock nobody set blinks 12:00. tap it to let it tell the time, tap again to put it back to blinking.';
  for (let i = 0; i < 4; i++) {
    if (i === 2) {
      const colon = document.createElement('div');
      colon.className = 'c7col';
      for (let d = 0; d < 2; d++) {
        const dot = document.createElement('i');
        dot.className = 'c7dot on';
        colon.appendChild(dot);
      }
      face.appendChild(colon);
    }
    const digit = document.createElement('div');
    digit.className = 'c7num';
    for (const letter of 'abcdefg') {
      const s = document.createElement('i');
      s.className = 'c7s c7' + letter;
      s.dataset.s = letter;
      digit.appendChild(s);
    }
    face.appendChild(digit);
    four.push(digit);
  }
  top.appendChild(face);
  const paint = (text) => {
    for (let i = 0; i < 4; i++) {
      const want = SEG[text[i]] || '';
      for (const s of four[i].children)
        s.classList.toggle('on', want.indexOf(s.dataset.s) >= 0);
    }
  };
  // The queue's claim on the clock. While jobs run or stand in line the face
  // turns orange and counts the hours and minutes the whole queue away - a
  // statement the machine makes about its own work, read from the monitor
  // document's queue block, never guessed here: each standing job is weighed
  // by the pace it has itself held, or by what this machine states its model
  // has cost, or by its engine's own kept completions. What blinks every three
  // seconds in this mode is the colon, not the digits: the digits count real
  // remaining work and deserve to stand while it does. Where no job could be
  // weighed the face burns orange with its digits dark and its colon blinking,
  // saying work without inventing a time. The reading is one poll cycle old at
  // most; past four seconds the cache copy answers and the page's own polling
  // has already paid for the request, so the clock adds no ask of its own to a
  // busy page. When the queue stands empty the claim lapses and the clock is
  // itself again.
  const IDLE_TITLE = 'a clock nobody set blinks 12:00. tap it to let it tell the time, tap again to put it back to blinking.';
  const CLOCK_AGE_MAX = 15000;
  let drain = null; // { standing, left (seconds|null), unmeasured, at }
  const drainHolds = () => drain && Date.now() - drain.at < CLOCK_AGE_MAX;
  const askQueue = async function () {
    try {
      const res = await fetch('/api/monitor');
      if (!res.ok) return;
      const data = await res.json();
      const q = data && data.queue ? data.queue : {};
      const left = Number(q.left_s);
      drain = Number(q.standing) > 0
        ? {
          standing: Number(q.standing),
          left: isFinite(left) && left >= 0 ? left : null,
          unmeasured: Number(q.unmeasured || 0),
          at: Date.now(),
        }
        : null;
    } catch (gone) {
      // Unreachable machine: the last reading stands until it ages out, and a
      // dead server is not entitled to hold an orange clock up forever.
    }
  };
  askQueue();
  setInterval(askQueue, 4000);
  const drainWords = function () {
    if (drain.left === null)
      return 'the queue is working, and this machine has stated no cost yet to set a clock against.';
    const total = Math.round(drain.left);
    let h = Math.floor(total / 3600);
    let m = Math.round((total - h * 3600) / 60);
    if (m >= 60) { h += 1; m = 0; }
    const left = h > 0 ? h + ' h' + (m > 0 ? ' ' + m + ' min' : '') : (m > 0 ? m + ' min' : 'under a minute');
    let line = 'the queue is working: ' + drain.standing + ' job' + (drain.standing > 1 ? 's' : '') +
      ' · about ' + left + ' to go, at what this machine states its work costs';
    if (drain.unmeasured > 0) line += ' · ' + drain.unmeasured + ' it cannot yet weigh';
    return line + '.';
  };
  const drainPaint = function () {
    face.classList.add('queue');
    face.title = drainWords();
    if (drain.left === null) { paint('    '); return; }
    let total = Math.round(drain.left);
    let h = Math.floor(total / 3600);
    let m = Math.round((total - h * 3600) / 60);
    if (m >= 60) { h += 1; m = 0; }
    if (h > 99) { h = 99; m = 59; }
    paint(String(h).padStart(2, '0') + String(m).padStart(2, '0'));
  };
  const show = () => {
    if (drainHolds()) { drainPaint(); return; }
    face.classList.remove('queue');
    face.title = IDLE_TITLE;
    if (!isSet()) { paint('1200'); return; }
    const now = new Date();
    const hour = now.getHours() % 12 || 12;
    paint(('   ' + hour + String(now.getMinutes()).padStart(2, '0')).slice(-4));
  };
  show();
  // Something gives way to its ghosts on the beat and the light comes back -
  // the segments stay standing, only the light leaves them. An unset clock
  // holds awake a random whole number of seconds - one to five, drawn afresh
  // each time, the way a stove's coal gives - then stands dark a full second.
  // A working queue keeps its count lit and lets the colon go dark every
  // three seconds instead, because the thing to notice there is that the
  // time is counting, not that the hour is unknown. A set clock idle does not
  // blink at all.
  const beat = function () {
    const queue = drainHolds();
    if (!queue && isSet()) { setTimeout(beat, 1000); return; }
    const awake = queue ? 3000 : (1 + Math.floor(Math.random() * 5)) * 1000;
    setTimeout(() => {
      // The hour may have been set while this beat was on its way: a blink
      // asked for by a clock nobody set has no right to fall on one that is
      // now set, so the mode is asked again as it strikes, not only as it
      // was scheduled.
      if (!queue && isSet()) { beat(); return; }
      face.classList.add('blink');
      setTimeout(() => {
        face.classList.remove('blink');
        beat();
      }, queue ? 900 : 1000);
    }, awake);
  };
  beat();
  setInterval(show, 1000);
  face.addEventListener('click', () => {
    try { localStorage.setItem(KEY, isSet() ? '0' : '1'); } catch (e) { }
    face.classList.remove('blink');
    show();
  });
})();

// Two counts beside the clock, in minutes: the time since the last bug fix -
// the moment this machine's binary was compiled - and the time since the last
// new bug, which the machine confesses as the newest page file it answers
// with, html, js or css alike. Both moments are the server's own facts, read
// from its system snapshot (which re-walks its own page folder at most once a
// minute), and the rows are re-asked once a minute: the counts show nothing
// finer than minutes, so nothing finer need move.
(function mountAgeRows() {
  const top = document.getElementById('topbar');
  const face = top ? top.querySelector('.clock7') : null;
  if (!top || !face) return;
  const rows = [
    { kind: 'fix', label: 'time since last bug fix:', at: 0 },
    { kind: 'bug', label: 'time since last new bug:', at: 0 },
  ];
  const box = document.createElement('div');
  box.className = 'age-stack';
  const cells = rows.map(function (row) {
    const line = document.createElement('div');
    line.className = 'age-row ' + row.kind;
    const label = document.createElement('span');
    label.className = 'age-label';
    label.textContent = row.label;
    const value = document.createElement('span');
    value.className = 'age-count';
    value.textContent = '…';
    line.appendChild(label);
    line.appendChild(value);
    box.appendChild(line);
    return { row: row, line: line, value: value };
  });
  top.insertBefore(box, face);
  // Minutes, told plainly: under an hour counts itself, under a day counts
  // hours and the quarter-hours, and past a day the hours have their say alone.
  const since = function (at) {
    if (!(at > 0)) return '-';
    const minutes = Math.floor((Date.now() / 1000 - at) / 60);
    if (minutes < 1) return '<1m';
    const days = Math.floor(minutes / 1440);
    const hours = Math.floor((minutes % 1440) / 60);
    const mins = minutes % 60;
    if (days > 0) return days + 'd ' + String(hours).padStart(2, '0') + 'h';
    if (hours > 0) return hours + 'h ' + String(mins).padStart(2, '0') + 'm';
    return minutes + 'm';
  };
  const paint = function () {
    for (const cell of cells) cell.value.textContent = since(cell.row.at);
  };
  const ask = async function () {
    let doc = null;
    try {
      const res = await fetch('/api/system');
      if (res.ok) doc = await res.json();
    } catch (e) { /* the rows keep the last truth they were given */ }
    if (doc === null || typeof doc !== 'object') return;
    cells[0].row.at = Number(doc.compiled_at) || 0;
    cells[1].row.at = Number(doc.webui_at) || 0;
    cells[0].line.title = 'counted from the moment this binary was compiled' +
      (typeof doc.compiled === 'string' ? ': ' + doc.compiled : '') + '.';
    cells[1].line.title = 'counted from the newest page file this server answers with' +
      (typeof doc.webui_file === 'string' && doc.webui_file ? ': ' + doc.webui_file : '') + '.';
    paint();
  };
  ask();
  // One minute is the grain of everything shown here: the count is re-asked on
  // that beat, so a fresh build or an edited page joins the rows within a
  // minute of the server learning of it, and nothing stirs between minutes.
  setInterval(ask, 60000);
})();

// The browser's tab names the machine, not the program: with three machines
// answered from one desk, three tabs all reading open-local-services say
// nothing. The machine states its name, its operating system and the device
// its binary drives (X1 (win11 vulkan1.4)), once, at the first read of the
// system snapshot - none of it moves while the server stands. The pages' own
// icons, one per page, ride on beside the words untouched.
(function mountTitle() {
  const ask = async function () {
    let doc = null;
    try {
      const res = await fetch('/api/system');
      if (res.ok) doc = await res.json();
    } catch (e) { /* the tab keeps the plain program name it was written with */ }
    if (doc === null || typeof doc !== 'object') return;
    const host = typeof doc.hostname === 'string' ? doc.hostname : '';
    const shape = [doc.os, doc.backend]
      .filter(function (word) { return typeof word === 'string' && word; }).join(' ');
    if (!host && !shape) return;
    document.title = (host || 'open-local-services') + (shape ? ' (' + shape + ')' : '');
  };
  ask();
})();

// the one alias every page script uses
const ui = window.ols;
