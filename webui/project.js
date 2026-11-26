// Project Studio: the fourth tab of this machine. One reading — the machine's
// own index of the projects it keeps (GET /project/api/index) — is what paints
// the dropdown, the quick facts, and the list of what exists at all. Every other
// room on the page answers to that one question, and carries the name of the
// project in its own door call. Which project and which room are in use are the
// page's state, not the machine's: the room is kept in this browser and in the
// address, so a bookmark or an agent lands on exactly this view, and the machine
// is never told that a room was chosen.
//
// Phase A is the shell. The doors this phase may find are the index, the tree,
// the meta facts, and the two adding hands. A door that is not on the machine
// yet answers nothing, and every room that has no reading says so in its own
// words rather than being left blank or filled in with what a page would guess.
// Controls whose doors are not there are not drawn: a control that answers to
// nothing is a lie about the machine.
(function () {
  const ui = window.ols;
  const E = ui.E;

  // The hosts this page fills and the lines that state where it stands.
  const dropdown = E('proj-dropdown');
  const addBtn = E('proj-add');
  const factsHost = E('proj-facts');
  const tabsHost = E('proj-tabs');
  if (!dropdown || !addBtn || !factsHost || !tabsHost) return; // the page has no studio bar

  const sheet = E('ps-sheet');
  const backdrop = E('ps-backdrop');
  const sheetError = E('proj-sheet-error');
  const sheetSubmit = E('ps-sheet-submit');
  const sheetClose = E('ps-sheet-close');
  const statusLine = E('status-line');

  // The six rooms. The doors named here are the only ones this phase asks, and
  // only when the room is the one in use and a project is open in it. A room
  // with no door named is a room this phase was not given an answer for.
  const ROOMS = [
    {
      tab: 'code',
      words: 'code',
      panel: 'ps-panel-code',
      list: 'ps-tree',
      note: 'ps-tree-note',
      door: '/project/api/tree',
    },
    {
      tab: 'issues',
      words: 'issues',
      panel: 'ps-panel-issues',
      list: 'ps-issues',
      note: 'ps-issues-note',
      door: '/project/api/issues.json',
      filterWords: issuesFilterWords,
      filterQuery: issuesFilterQuery,
    },
    {
      tab: 'pulls',
      words: 'pulls',
      panel: 'ps-panel-pulls',
      list: 'ps-pulls',
      note: 'ps-pulls-note',
      door: '/project/api/pulls',
      filterWords: pullsFilterWords,
      filterQuery: pullsFilterQuery,
    },
    { tab: 'builds', words: 'builds', panel: 'ps-panel-builds', list: 'ps-builds', note: 'ps-builds-note' },
    { tab: 'remotes', words: 'remotes', panel: 'ps-panel-remotes', list: 'ps-remotes', note: 'ps-remotes-note' },
    {
      tab: 'settings',
      words: 'settings',
      panel: 'ps-panel-settings',
      list: 'ps-meta-read',
      note: 'ps-meta-note',
      door: '/project/api/meta',
    },
  ];
  for (const room of ROOMS) {
    room.panelEl = E(room.panel);
    room.listEl = E(room.list);
    room.noteEl = E(room.note);
    room.tabBtn = E('proj-tab-' + room.tab);
    if (!room.panelEl || !room.listEl || !room.noteEl || !room.tabBtn) return; // the page has no tab strip
  }

  const kIndexDoor = '/project/api/index';
  const kTabStore = 'ols-project-tab';
  const kProjectsNew = '/project/api/projects/new';
  const kProjectsAdopt = '/project/api/projects/adopt';
  const kQuiet = 'the machine has not answered here';
  const kNoDoor = 'the machine has not been given a door to answer here yet';

  // ---------- the page's own state ----------
  const state = { tab: 'code', project: '', listed: [] };
  let indexStamp = '';
  let deepLinkProject = '';
  // What a room was last told, by project: changing the dropdown draws the
  // rooms from here at once, and the machine is checked after, never before.
  let heldRooms = {};
  let offlineMentioned = false;

  function quiet(host, words, tip) {
    host.textContent = '';
    const line = document.createElement('div');
    line.className = 'ps-quiet';
    line.textContent = words;
    if (tip) line.title = tip;
    host.appendChild(line);
  }

  function note(host, words, kind, tip) {
    host.textContent = '';
    const line = document.createElement('div');
    line.className = 'ps-note' + (kind ? ' ' + kind : '');
    line.textContent = words;
    if (tip) line.title = tip;
    host.appendChild(line);
  }

  // The one line this page speaks with. It carries the page's own state, so a
  // page beside a quiet machine still says where it stands.
  function say(words, kind, tip) {
    if (!statusLine) return;
    statusLine.textContent = words;
    statusLine.className = 'status-line' + (kind ? ' ' + kind : '');
    if (tip) statusLine.title = tip;
  }

  function el(tag, cls, text, title) {
    const node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text !== undefined && text !== null) node.textContent = String(text);
    if (title !== undefined) node.title = title;
    return node;
  }

  function oneLiner(words, width) {
    const text = String(words || '');
    return text.length > width ? text.slice(0, width) + '…' : text;
  }

  // Words from an input, as they stand now. An empty field means "keep what the
  // machine stated", never "make it empty".
  function fieldText(input) {
    if (!input) return '';
    const value = String(input.value || '');
    return value.trim();
  }

  // The state carried in the address: the last place this page was aimed at.
  function askedInAddress() {
    try {
      const here = new URL(window.location.href);
      return {
        project: here.searchParams.get('project') || '',
        tab: here.searchParams.get('tab') || '',
      };
    } catch (noUrl) {
      return { project: '', tab: '' };
    }
  }

  // The room this browser last used, for a visit that names none. The address
  // wins over this; the address is what a deep link carries.
  function storedTab() {
    let kept = null;
    try {
      kept = localStorage.getItem(kTabStore);
    } catch (noStore) {
      kept = null;
    }
    for (const room of ROOMS) {
      if (room.tab === kept) return kept;
    }
    return '';
  }

  function roomNamed(words) {
    for (const room of ROOMS) {
      if (room.tab === words) return room;
    }
    return null;
  }

  // Drawing the address after the page's state: the view this page stands on is
  // the view an address names, so a bookmark or an agent lands exactly here.
  function paintAddress() {
    try {
      const here = new URL(window.location.href);
      if (state.project) here.searchParams.set('project', state.project);
      else here.searchParams.delete('project');
      here.searchParams.set('tab', state.tab);
      window.history.replaceState(null, '', here.pathname + here.search + here.hash);
    } catch (noHistory) {
      // An address this browser will not rewrite still keeps the page's state.
    }
  }

  // ---------- the rooms ----------
  // Only the room the tab strip names is drawn; the others are taken off the
  // page rather than hidden behind paint, so nothing on the screen describes a
  // project through two rooms at once.
  function showRoom(room) {
    for (const other of ROOMS) {
      const chosen = other === room;
      other.panelEl.hidden = !chosen;
      other.tabBtn.className = 'ps-tab' + (chosen ? ' ps-tab-on' : '');
    }
    state.tab = room.tab;
    try {
      localStorage.setItem(kTabStore, room.tab);
    } catch (noStore) {
    }
    paintAddress();
    paintRoom(room);
  }

  // A room drawn from what the page holds. A room this phase has a door for is
  // asked of the machine once per project; after that it is drawn from what was
  // last told, because a heavy reading is never taken again to paint the same
  // room the same way.
  function paintRoom(room) {
    if (!state.project) {
      quiet(room.listEl, 'no project open',
        'This page answers to one project at a time, and none is chosen. The dropdown names what this machine keeps.');
      note(room.noteEl, 'this room has not been asked of the machine yet', '',
        'A room is only asked about once a project is open in it, because every answer about a project carries the name of that project.');
      return;
    }
    if (!room.door) {
      quiet(room.listEl, kNoDoor,
        'The answer for this room comes with its own door. This phase of the page was not given one to ask, so nothing is drawn here rather than a picture of what the room would hold.');
      note(room.noteEl, 'nothing was asked of the machine for this room, so nothing here is a guess about it.', '',
        'A room with no door to ask holds no claim about the machine.');
      return;
    }
    const held = heldRooms[roomHeld(room, state.project)];
    if (held) {
      paintFrom(room, held.answer, state.project, held.at);
      return;
    }
    void askRoom(room);
  }

  // A room that can be asked in more than one way is held under the way it was
  // asked: the filter a room stands on is part of the reading, and a room whose
  // filter moved is not painted from a reading of the other ask. A room with no
  // filter is held under its own name, as it always was.
  function roomHeld(room, slug) {
    const words = room.filterWords ? String(room.filterWords()) : '';
    return room.tab + ':' + slug + (words ? ':' + words : '');
  }

  // Everything the room's own filter adds to the ask, in the door's own words.
  function roomAsk(room, slug) {
    return room.door + '?project=' + encodeURIComponent(slug) +
      (room.filterQuery ? String(room.filterQuery()) : '');
  }

  // Every held reading of one room, or of one project's rooms, put away so the
  // next drawing asks again: a record this page just changed has an answer the
  // page has not heard yet.
  function forgetHeld(prefix) {
    for (const key of Object.keys(heldRooms)) {
      if (key.indexOf(prefix) === 0) delete heldRooms[key];
    }
  }

  async function askRoom(room, noStore) {
    if (!state.project) return;
    const slug = state.project;
    const path = roomAsk(room, slug);
    // The ask carries its own answer, status, and words: the page reads what
    // THIS door said, whatever else is asking beside it, and a room whose read
    // could not be carried is the same room either way - the page keeps
    // standing, and the words say which door gave what. A no-store ask rides
    // the word down to the browser's own cache (status-cache replays a GET
    // inside its stated freshness for any other ask); the held copy behind a
    // room that just moved is put away by forgetHeld, so no key at all answers
    // it - and a live re-ask from the stream carries noStore too.
    const read = await ui.apiR(path, 'GET', undefined, noStore ? { cache: 'no-store' } : undefined);
    const answer = read.answer;
    if (!answer) {
      if (read.status === 404) {
        quiet(room.listEl, kQuiet,
          'That door is not on this machine. Nothing here describes the project by way of a door it could not reach.');
        note(room.noteEl, 'asked at ' + room.door + ' and the machine has no such door', 'unknown',
          'The room names the door it asked, so a later phase landing there can be seen to land in the same place.');
      } else if (read.status === 401 || read.status === 403) {
        quiet(room.listEl, 'the key this page carries was refused here',
          'The machine stands here and refused this page at that door: ' + ui.askWhy(read));
      } else {
        quiet(room.listEl, read.error ? 'the machine refused this reading: ' + read.error
          : 'the machine gave no answer at all at this door',
          read.error ? 'The door answered, and its own words are the words here.'
            : 'The door was reached and nothing came back; the page keeps what it last read instead.');
      }
      return;
    }
    heldRooms[roomHeld(room, slug)] = { answer: answer, at: Date.now() };
    paintFrom(room, answer, slug, 0);
  }

  // A reading taken past a mutation - the live re-ask beside the stream - may
  // not be answered from this machine's own browser cache: the word rides on
  // its fetch above. The held copy is put away by forgetHeld when a write
  // stands behind the ask, so no key at all answers a room that just moved.

  // The room's own answer, painted without adding to it. What the machine named
  // is drawn as the machine named it; a reading held from earlier is labeled
  // with the moment it was taken.
  function paintFrom(room, answer, slug, heldAt) {
    if (room.tab === 'code') paintTree(room, answer, slug, heldAt);
    else if (room.tab === 'pulls') paintPullsRoom(room, answer, slug, heldAt);
    else if (room.tab === 'issues') paintIssuesRoom(room, answer, slug, heldAt);
    else paintMeta(room, answer, slug, heldAt);
  }

  // ---------- the code room: three columns ----------
  // files | viewer | outline: what the machine listed, the file this page is
  // reading, and what the machine made of that file. Which folder is open, which
  // file is open, which ref is in effect and which line is anchored are the
  // page's state: kept here and in the address, never told to the machine.
  const kFileDoor = '/project/api/file';
  const kOutlineDoor = '/project/api/file-outline';
  const kHistoryDoor = '/project/api/history';
  const kBlameDoor = '/project/api/blame';
  const kMaxViewLines = 5000;
  const kSearchWait = 250;
  const kOutlineWatchWait = 200;

  const view = { path: '', ref: '', line: 0, mode: '', binary: false };
  const readDirs = {};
  const readFiles = {};
  const readOutlines = {};
  const readHistories = {};
  // Every commit this page has been told, by oid, so the blame chips can name a
  // line without asking a second door. A long walk would grow this past any
  // sense - it is a memory of names, not a claim about the machine - so it is
  // held to a bound: the commit remembered first gives way when the bound is
  // passed, and a chip that finds nothing kept says the machine named no commit
  // for the line rather than inventing one.
  const kKnownCommitCap = 8000;
  const knownCommits = new Map();
  let treeRows = [];
  let rowByPath = {};
  let selAt = -1;
  let codeArmed = false;
  let searchField = null;
  let searchQueued = false;
  let outlineWatchedAt = 0;

  function fileKey(path, ref) {
    return String(path) + '@' + (ref ? String(ref) : '@head');
  }

  // A door this room asks with the project already named in it. Every ask goes
  // through ui.apiR, so the browser's own window over that door is what keeps a
  // folder, a blob, or a blame map from being paid for twice - and every ask
  // carries its own answer, its own status, and its own words: the truth of
  // one reading is read from that reading's result, never from a shared pair
  // another ask running beside it may have written over.
  async function codeAsk(door, params) {
    let path = door + '?project=' + encodeURIComponent(state.project);
    for (const param of params) {
      path += '&' + param[0] + '=' + encodeURIComponent(param[1]);
    }
    return ui.apiR(path);
  }

  // A door this page asked that gave nothing: the same three truths the room's
  // own first ask states - no such door, a refused key, or no answer at all -
  // read from the result of the ask that stands for this part of the room.
  function refusedAt(read, listHost, noteHost, door, words) {
    const status = read && read.status !== undefined ? read.status : 0;
    const why = (read && read.error) || ui.askWhy(read || { status: 0 });
    if (status === 404) {
      quiet(listHost, kQuiet,
        'That door is not on this machine. Nothing here describes the project by way of a door it could not reach.');
      note(noteHost, 'asked at ' + door + ' and the machine has no such door', 'unknown',
        'The room names the door it asked, so a later phase landing there can be seen to land in the same place.');
    } else if (status === 401 || status === 403) {
      quiet(listHost, 'the key this page carries was refused here',
        'The machine stands here and refused this page at that door: ' + why);
    } else {
      quiet(listHost, 'the machine gave no answer at all at this door',
        'The door was reached and nothing came back; the page keeps what it last read instead.');
    }
    if (words) {
      note(noteHost, words, '', 'This part of the room holds no claim about the machine past what its own door said.');
    }
  }

  // How many issue rows this page holds carry a tag for a path. The rows are
  // the issues readings this browser is keeping - the same ones the issues room
  // draws - and the count is said as this page's own of those rows: no door
  // states a per-file count. A reading bounded by a filter is counted from that
  // reading alone, and a project this page has never asked about holds none.
  let issueTagCache = { slug: '', answers: [], counts: {} };
  function issueTagCounts(slug) {
    const answers = [];
    for (const key of Object.keys(heldRooms)) {
      if (key !== 'issues:' + slug && key.indexOf('issues:' + slug + ':') !== 0) continue;
      const held = heldRooms[key];
      if (held && held.answer && Array.isArray(held.answer.issues)) answers.push(held.answer);
    }
    let same = issueTagCache.slug === slug && issueTagCache.answers.length === answers.length;
    if (same) {
      for (let i = 0; i < answers.length; ++i) {
        if (issueTagCache.answers[i] !== answers[i]) { same = false; break; }
      }
    }
    if (same) return issueTagCache.counts;
    const counts = {};
    for (const answer of answers) {
      for (const one of answer.issues) {
        const files = one && Array.isArray(one.files) ? one.files : [];
        for (const tag of files) {
          const tagPath = String(tag);
          if (!tagPath) continue;
          counts[tagPath] = (counts[tagPath] || 0) + 1;
        }
      }
    }
    issueTagCache = { slug: slug, answers: answers, counts: counts };
    return counts;
  }

  function buildRow(entry, depth) {
    const path = entry && entry.path ? String(entry.path) : '';
    if (!path) return null;
    const isDir = !!entry && (entry.isDir === true || entry.kind === 'dir' || entry.kind === 'folder');
    const row = el('div', 'ps-row' + (isDir ? ' ps-row-dir' : ''));
    row.style.setProperty('--depth', String(depth));
    if (isDir) {
      const hand = el('span', 'ps-row-hand', 'open');
      hand.title = 'A folder is listed only when its row is pressed; pressing it again takes the listing away.';
      row.appendChild(hand);
    }
    const name = el('span', 'ps-row-name', path);
    name.title = isDir
      ? 'A folder. Press its row and the machine lists it, one reading per folder. Press again and the listing is taken away.'
      : 'A file of this project. Press its row and the viewer beside this column reads it.';
    row.appendChild(name);
    if (entry && Number(entry.size) >= 0) {
      row.appendChild(el('span', 'ps-row-size', ui.sizeText(Number(entry.size))));
    }
    const touched = issueTagCounts(state.project)[path] || 0;
    if (touched > 0) {
      const dot = el('i', 'ps-issue-dot', '•');
      dot.title = 'This page counted ' + touched + (touched === 1 ? ' issue' : ' issues') +
        ' among the issue rows it holds that wear a tag for ' + path +
        '. The count is this browser\'s own, taken from the issues reading it keeps - no door states a per-file count.';
      row.appendChild(dot);
    }
    const record = {
      node: row, path: path, isDir: isDir, depth: depth, shown: true,
      // The machine's own name for a file's bytes, carried with the row:
      // content-addressed, it lets the viewer answer from the browser's
      // oid store without asking the file door again (§9.2 Tier B).
      oid: entry && entry.oid ? String(entry.oid) : '',
    };
    ui.onPress(row, function () {
      ui.flash(row);
      pressRow(record);
    });
    return record;
  }

  function mountRows(made, after) {
    const list = roomNamed('code').listEl;
    if (!list) return;
    for (const record of made) {
      if (after) after.parentNode.insertBefore(record.node, after);
      else list.appendChild(record.node);
    }
  }

  function holdRows(made, at) {
    treeRows.splice(at, 0, ...made);
    for (const record of made) rowByPath[record.path] = record;
  }

  function clearTree() {
    const list = roomNamed('code').listEl;
    if (list) list.textContent = '';
    treeRows = [];
    rowByPath = {};
    selAt = -1;
  }

  // The rows of one folder, under the row that opened it. The flat list and the
  // indents are the tree; a folder's rows sit in order after their row, so the
  // keyboard and the filter walk the same order the machine listed.
  function listDir(dir, answer, depth, at) {
    const made = [];
    for (const entry of answer.entries) {
      const record = buildRow(entry, depth);
      if (record) made.push(record);
    }
    if (at < 0) mountRows(made, null);
    else {
      const under = treeRows[at].node;
      mountRows(made, under.nextSibling);
    }
    holdRows(made, at < 0 ? treeRows.length : at + 1);
    if (at >= 0) {
      treeRows[at].node.classList.add('ps-row-open');
      treeRows[at].node.appendChild(el('span', 'ps-row-open', 'opened'));
    }
    applySearch();
  }

  // What the address aims a fresh load at: the file and the line the page was
  // opened on. The room's own plumbing reads project and tab; this reads the
  // part that belongs to the code room alone.
  function aimInAddress() {
    try {
      const here = new URL(window.location.href);
      const anchored = /^#L(\d+)$/.exec(String(here.hash || ''));
      return { path: here.searchParams.get('file') || '', line: anchored ? Number(anchored[1]) : 0 };
    } catch (noUrl) {
      return { path: '', line: 0 };
    }
  }

  // The view this page stands on is the view an address names: the code room's
  // ask extends the page's own address with the file open and the line anchored,
  // in the same words paintAddress already writes project and tab in.
  function paintFileAddress() {
    try {
      const here = new URL(window.location.href);
      if (state.project) here.searchParams.set('project', state.project);
      else here.searchParams.delete('project');
      here.searchParams.set('tab', state.tab);
      if (view.path) here.searchParams.set('file', view.path);
      else here.searchParams.delete('file');
      const anchor = view.path && view.line > 0 ? '#L' + view.line : '';
      window.history.replaceState(null, '', here.pathname + here.search + anchor);
    } catch (noHistory) {
    }
  }

  function paintTree(room, answer, slug, heldAt) {
    const code = roomNamed('code');
    if (!code) return;
    armCodeRoom(code);
    const dir = answer.dir === undefined ? '' : String(answer.dir);
    if (!Array.isArray(answer.entries)) {
      quiet(room.listEl, 'the machine answered this door without naming any files',
        'An answer that names no files is not drawn as an empty project: the room keeps what it heard and says it heard nothing of the tree.');
      return;
    }
    readDirs[dir] = { answer: answer, at: heldAt || Date.now() };
    clearTree();
    listDir(dir, answer, 0, -1);
    note(code.noteEl, 'listed by the machine for ' + slug + ' in ' + (dir || 'the project root') +
      (answer.truncated === true ? ' · the machine cut this list: more is here than is shown' : '') +
      asOfTail(answer) + heldTail(heldAt), '',
      'The list is what the machine named for this folder; the page adds nothing to it. A folder not opened has no rows on the screen.');
    if (!view.path) {
      const aim = aimInAddress();
      if (aim.path) void openFile(aim.path, aim.line, view.ref);
    }
  }

  async function openDir(dir, depth) {
    if (!state.project) return;
    const code = roomNamed('code');
    const held = readDirs[dir];
    let answer = held ? held.answer : null;
    if (!answer) {
      const folderRead = await codeAsk(code.door, [['dir', dir], ['ref', view.ref]]);
      answer = folderRead.answer;
      if (!answer) {
        refusedAt(folderRead, code.listEl, code.noteEl, code.door,
          'This folder holds no reading, so nothing is drawn for it.');
        return;
      }
      if (!Array.isArray(answer.entries)) {
        note(code.noteEl, 'that folder came back without naming any files', 'unknown',
          'An answer that names no files is not drawn as an empty folder: the room says what it heard.');
        return;
      }
      readDirs[dir] = { answer: answer, at: Date.now() };
    }
    const under = rowByPath[dir];
    if (!under) return;
    listDir(dir, answer, depth, treeRows.indexOf(under));
  }

  function collapseDir(record) {
    const at = treeRows.indexOf(record);
    if (at < 0) return;
    record.node.classList.remove('ps-row-open');
    const mark = record.node.querySelector('.ps-row-open');
    if (mark) record.node.removeChild(mark);
    let cut = at + 1;
    while (cut < treeRows.length && treeRows[cut].depth > record.depth) {
      const gone = treeRows.splice(cut, 1)[0];
      if (rowByPath[gone.path] === gone) delete rowByPath[gone.path];
      if (gone.node) gone.node.remove();
    }
    applySearch();
  }

  function pressRow(record) {
    if (!record) return;
    if (!record.isDir) {
      void openFile(record.path, record.path === view.path ? view.line : 0, view.ref, record.oid);
      return;
    }
    if (record.node.classList.contains('ps-row-open')) {
      collapseDir(record);
      return;
    }
    void openDir(record.path, record.depth + 1);
  }

  // The filter runs over the paths this page has loaded, never of the machine:
  // a folder that was not opened has nothing on the screen to hide.
  function applySearch() {
    const words = fieldText(searchField).toLowerCase();
    for (const record of treeRows) {
      record.match = !words || record.path.toLowerCase().indexOf(words) >= 0;
      record.shown = record.match;
    }
    for (let at = treeRows.length - 1; at >= 0; --at) {
      const record = treeRows[at];
      if (!record.isDir) continue;
      for (let deeper = at + 1; deeper < treeRows.length; ++deeper) {
        if (treeRows[deeper].depth <= record.depth) break;
        if (treeRows[deeper].shown) {
          record.shown = true;
          break;
        }
      }
    }
    for (const record of treeRows) {
      record.node.classList.toggle('ps-row-out', !record.shown);
    }
    if (selAt >= 0 && !treeRows[selAt].shown) selAt = -1;
  }

  // The keyboard this room answers to: arrows move the selection across the rows
  // in screen order, Enter takes it. R-07 navigability - a room a person can
  // walk without a mouse is a room an agent can walk too.
  function walkCodeRoom(event) {
    if (!event || sheetOpen || state.tab !== 'code') return;
    const key = String(event.key || '');
    // While the editor holds the browser's focus, Enter is a line going into
    // the buffer and arrows are the caret moving: only Escape through here.
    if (editorFocusHeld() && key !== 'Escape') return;
    if (key === 'ArrowUp' || key === 'ArrowDown') {
      stepSelection(key === 'ArrowDown' ? 1 : -1);
      return;
    }
    if (key !== 'Enter' || selAt < 0) return;
    if (searchField && searchField === document.activeElement) return;
    const record = treeRows[selAt];
    if (record) pressRow(record);
  }

  function stepSelection(step) {
    let found = -1;
    let at = selAt;
    for (let tries = 0; tries < treeRows.length; ++tries) {
      at += step;
      if (at < 0) at = treeRows.length - 1;
      if (at >= treeRows.length) at = 0;
      if (treeRows[at] && treeRows[at].shown) {
        found = at;
        break;
      }
    }
    if (selAt >= 0 && treeRows[selAt]) treeRows[selAt].node.classList.remove('ps-row-sel');
    selAt = found;
    if (found >= 0) treeRows[found].node.classList.add('ps-row-sel');
  }

  // The three columns, built once against whatever the page carries. The listing
  // hosts the room already owns are moved into the files column rather than
  // copied, so the room's own hosts stay the hosts the room paints.
  function codeShell(room) {
    const cols = E('ps-code-columns');
    if (cols) return cols;
    const panel = room.panelEl;
    if (!panel) return null;
    const built = el('div', 'ps-code-columns');
    built.id = 'ps-code-columns';
    built.title = 'Three columns: the files the machine listed, the file this page is reading, and the outline the machine made of it. A narrow screen folds them into one.';
    panel.appendChild(built);

    const files = el('div', 'ps-code-files');
    files.id = 'ps-code-files';
    files.title = 'The files column: what the machine listed, one folder at a time.';
    built.appendChild(files);
    for (const host of [room.listEl, room.noteEl]) {
      if (host && host.parentNode !== files) files.appendChild(host);
    }
    const fieldRow = el('div', 'ps-field-row');
    const fieldLabel = el('label', null, 'search these paths');
    fieldLabel.htmlFor = 'proj-search';
    fieldLabel.title = 'Words to look for in the paths this page has loaded. Nothing is asked of the machine.';
    searchField = el('input');
    searchField.id = 'proj-search';
    searchField.type = 'text';
    searchField.title = 'Words to filter these paths by. The filter runs over what this page has loaded; a folder never opened was never searched.';
    fieldRow.appendChild(fieldLabel);
    fieldRow.appendChild(searchField);
    files.appendChild(fieldRow);

    const viewer = el('div', 'ps-code-viewer');
    viewer.id = 'code-viewer';
    viewer.title = 'The viewer column: one line per line of the file this page is reading, at the ref its head names.';
    built.appendChild(viewer);
    const head = el('div', 'ps-code-head');
    head.id = 'code-viewer-head';
    viewer.appendChild(head);
    const path = el('div', 'ps-code-path');
    path.id = 'code-viewer-path';
    head.appendChild(path);
    const actions = el('div', 'ps-code-actions');
    actions.id = 'code-viewer-actions';
    head.appendChild(actions);
    const ref = el('span', 'ps-chip');
    ref.id = 'code-viewer-ref';
    head.appendChild(ref);
    for (const action of [
      ['raw', 'Read the file as it came: one block of words, no line numbers, no anchors.'],
      ['history', 'The dense file log the machine walks: the lane of the walk, who changed what and when, its refs and line counts, and a chosen commit told in full. Press again for the numbered reading.'],
      ['blame', 'One commit mark per line: who wrote it. Press again for the numbered reading.'],
      ['edit', 'Type into this file in an editor pane beside the reading: the words you type stay in this browser, and only a save asks the machine to commit them. Press again for the numbered reading.'],
    ]) {
      const hand = el('button', 'ps-code-hand', action[0]);
      hand.id = 'code-viewer-' + action[0];
      hand.title = action[1];
      actions.appendChild(hand);
    }
    const body = el('div', 'ps-code-body');
    body.id = 'code-viewer-body';
    viewer.appendChild(body);
    const viewNote = el('div');
    viewNote.id = 'code-viewer-note';
    viewer.appendChild(viewNote);

    const outline = el('div', 'ps-code-outline');
    outline.id = 'code-outline';
    outline.title = 'The outline column: the shapes the machine named inside the file open. A press jumps there and anchors the line.';
    built.appendChild(outline);
    const outlineNote = el('div');
    outlineNote.id = 'code-outline-note';
    outline.appendChild(outlineNote);
    return built;
  }

  // Wiring the room holds once: the presses the columns answer to, the keyboard
  // that walks the files column, and the beat a typed search owes before it
  // filters. The house's watchFieldWords lives in the image studio's own page
  // rather than in the shared library, so this room keeps its own wait.
  function armCodeRoom(room) {
    if (codeArmed) return;
    codeArmed = true;
    codeShell(room);
    for (const modeName of ['raw', 'history', 'blame', 'edit']) {
      ui.onPress(E('code-viewer-' + modeName), function () {
        stepCodeMode(modeName);
      });
    }
    const body = E('code-viewer-body');
    if (body) {
      // One handler for a body that can hold thousands of lines, rather than a
      // listener per line.
      body.addEventListener('click', function (event) {
        const node = event && event.target ? event.target : null;
        if (!node || !node.classList || !node.classList.length) return;
        ui.flash(node);
        bodyPress(node);
      });
      body.addEventListener('scroll', function () {
        const now = Date.now();
        if (now - outlineWatchedAt < kOutlineWatchWait) return;
        outlineWatchedAt = now;
        watchOutline();
      });
    }
    const outline = E('code-outline');
    if (outline) {
      outline.addEventListener('click', function (event) {
        const node = event && event.target ? event.target : null;
        if (!node || !node.dataset.line) return;
        ui.flash(node);
        anchorLine(Number(node.dataset.line));
      });
    }
    if (!searchField) searchField = E('proj-search');
    if (searchField) {
      const hear = function () {
        if (searchQueued) return;
        searchQueued = true;
        setTimeout(function () {
          searchQueued = false;
          applySearch();
        }, kSearchWait);
      };
      searchField.addEventListener('change', hear);
      searchField.addEventListener('input', hear);
      searchField.addEventListener('keyup', hear);
    }
    document.addEventListener('keydown', walkCodeRoom);
    // The lane canvas answers the window it is drawn under: a resize, or a
    // display whose devicePixelRatio changed under the page, repaints blocks.
    window.addEventListener('resize', histResized);
  }

  function paintViewHead() {
    const head = E('code-viewer-path');
    if (!head) return;
    head.textContent = '';
    const ref = E('code-viewer-ref');
    if (!view.path) {
      const none = el('span', 'ps-quiet', 'no file open in this room');
      none.title = 'The viewer reads one file at a time: press a row in the files column, or name a file in the address.';
      head.appendChild(none);
      if (ref) {
        ref.textContent = '';
        ref.appendChild(el('span', 'ps-chip-name', 'ref'));
        ref.appendChild(el('span', 'ps-fact-quiet', 'nothing asked for, with no file open'));
      }
      return;
    }
    head.appendChild(el('span', 'ps-code-file', view.path));
    head.appendChild(el('span', 'ps-code-at', view.line ? 'at L' + view.line : 'no line anchored'));
    if (!ref) return;
    ref.textContent = '';
    ref.appendChild(el('span', 'ps-chip-name', 'ref'));
    if (view.ref) {
      ref.appendChild(el('span', 'ps-chip-said', view.ref));
      ref.title = 'The commit this reading stands on. It came from a commit row or a blame mark here, and every door of this room is asked at it.';
    } else {
      ref.appendChild(el('span', 'ps-fact-quiet', 'none named: the machine answered at its own default'));
      ref.title = 'No ref was named, so the machine answered at whatever its own state says. Nothing here is a branch this page picked.';
    }
  }

  // The oidHeld is the tree row's own name for these bytes: when the
  // browser already holds that name in its oid store, the viewer paints
  // from there and the file door is not asked at all (§9.2 Tier B -
  // content-addressed, the bytes under a name never move).
  async function openFile(path, line, ref, oidHeld) {
    const wanted = String(path || '');
    if (!wanted || !state.project) return;
    view.path = wanted;
    view.ref = String(ref || '');
    view.line = Number(line) > 0 ? Number(line) : 0;
    view.mode = '';
    paintActiveHand();
    paintFileAddress();
    paintViewHead();
    const body = E('code-viewer-body');
    const viewNote = E('code-viewer-note');
    if (!body || !viewNote) return;
    body.textContent = '';
    viewNote.textContent = '';
    body.classList.remove('ps-code-body-hist');
    if (hist.surface) histTeardown();
    const key = fileKey(view.path, view.ref);
    let read = readFiles[key];
    if (!read) {
      const store = window.olsProjectCache;
      const heldOid = String(oidHeld || '');
      const heldBytes = heldOid && store ? store.get(heldOid) : null;
      if (heldBytes !== null) {
        // The bytes under this name are already in this browser's store:
        // content-addressed, they are the bytes the machine named. The
        // file door is not asked.
        read = {
          answer: { path: view.path, oid: heldOid, size: heldBytes.length },
          at: Date.now(),
          oid: heldOid,
          text: heldBytes,
          binary: false,
          fromStore: true,
        };
      } else {
        const fileRead = await codeAsk(kFileDoor, [['path', view.path], ['ref', view.ref]]);
        const answer = fileRead.answer;
        if (!answer) {
          refusedAt(fileRead, body, viewNote, kFileDoor,
            'No reading of this file came back, so the viewer holds nothing about it.');
          outlineNothing('the file this page is reading has no reading of its own, so no blob was named to outline');
          return;
        }
        const text = typeof answer.content === 'string' ? answer.content : null;
        const binary = answer.binary === true || text === null;
        read = {
          answer: answer,
          at: Date.now(),
          oid: answer.oid ? String(answer.oid) : '',
          text: text,
          binary: binary,
        };
        if (read.oid && read.text !== null && store) void store.put(read.oid, read.text);
      }
      readFiles[key] = read;
    }
    view.binary = !!read.binary;
    if (read.binary) {
      quiet(body, 'this file came back with no words: the machine sent bytes, not text',
        'The file door named no text and marked the bytes binary. Nothing else is offered: a binary file has no lines to number, no outline this machine can name, and no blame this page can draw.');
      if (Number(read.answer.size) >= 0) {
        note(viewNote, 'the machine named ' + ui.sizeText(Number(read.answer.size)) + ' for this file', '',
          'That size is what the machine stated; this page read no bytes of its own.');
      }
      outlineNothing('a binary file has no outline: the machine reads its bytes as data, not as words');
      return;
    }
    const actions = E('code-viewer-actions');
    if (actions) actions.hidden = false;
    renderBody(read);
    if (view.line > 0) anchorLine(view.line);
    void askOutline(read);
  }

  function renderBody(read) {
    const body = E('code-viewer-body');
    if (!body) return;
    body.textContent = '';
    const lines = String(read.text === null ? '' : read.text).split('\n');
    const shown = lines.length > kMaxViewLines ? kMaxViewLines : lines.length;
    for (let at = 0; at < shown; ++at) {
      body.appendChild(codeLine(String(at + 1), lines[at]));
    }
    if (lines.length > shown) {
      const cut = el('div', 'ps-code-cut', 'showing first ' + shown + ' of ' + lines.length + ' lines');
      cut.title = 'The whole file came from one door; drawing every line of a very large file is what this page leaves out, not the file. The count is counted from the bytes the machine sent, not from a file this page opened.';
      body.appendChild(cut);
    }
    const readWords = read.fromStore
      ? 'these bytes were taken from this browser\'s store by the name ' + read.oid +
        ' · content-addressed, the same bytes the machine named before · for ' + state.project + ' at ' + view.path +
        ' at ' + (view.ref ? view.ref : 'the machine\'s own default')
      : 'read by the machine for ' + state.project + ' at ' + view.path +
        ' at ' + (view.ref ? view.ref : 'the machine\'s own default') +
        (read.oid ? ' · the machine named these bytes as ' + read.oid : ' · the machine named no name for these bytes') +
        asOfTail(read.answer);
    note(E('code-viewer-note'), readWords + heldTail(read.at), '',
      'Every line here is one door\'s answer about this path, or the bytes this browser kept under the name a door gave. The line count is a count of the bytes it carries, not of a file this page opened.');
  }

  function codeLine(number, words) {
    const row = el('div', 'ps-code-line');
    row.id = 'L' + number;
    const num = el('button', 'ps-code-no', number);
    num.dataset.line = number;
    num.title = 'Copy ' + view.path + ':L' + number + ' to the clipboard, and anchor this page at that line.';
    row.appendChild(num);
    const text = el('pre', 'ps-code-text');
    if (words) text.appendChild(document.createTextNode(words));
    else text.appendChild(el('br'));
    row.appendChild(text);
    return row;
  }

  function bodyPress(node) {
    if (node.classList.contains('ps-code-no')) {
      const line = Number(node.dataset.line);
      if (line > 0) {
        anchorLine(line);
        copyRef(view.path + ':L' + line);
      }
      return;
    }
    if (node.classList.contains('ps-code-chip') || node.classList.contains('ps-code-commit')) {
      const oid = node.dataset.oid;
      if (oid) void openFile(view.path, view.line, oid);
      return;
    }
    histPress(node);
  }

  // The history surface answers presses through the body's own one handler: the
  // row chooses, the oid copies, the hand opens the body in place, the tail
  // walks on, and the detail pane's links jump the list or open the file at
  // the commit — the same openFile idiom every other reading of this room uses.
  function histPress(node) {
    if (!hist.surface || !node || !node.closest) return false;
    const hit = node.closest('.ps-hist-oid,.ps-hist-expand,.ps-hist-more,.ps-hist-again,.ps-hist-det-oid,.ps-hist-det-parent,.ps-hist-det-open,.ps-hist-change,.ps-hist-row');
    if (!hit) return false;
    const cls = hit.classList;
    if (cls.contains('ps-hist-oid') || cls.contains('ps-hist-det-oid')) {
      copyRef(String(hit.dataset.oid || ''));
      return true;
    }
    if (cls.contains('ps-hist-expand')) {
      histToggle(hit.dataset.oid);
      return true;
    }
    if (cls.contains('ps-hist-more')) {
      const last = hist.rows.length ? hist.rows[hist.rows.length - 1] : null;
      if (last) void histAsk(last.oid);
      return true;
    }
    if (cls.contains('ps-hist-again')) {
      void histAsk(null, true);
      return true;
    }
    if (cls.contains('ps-hist-det-parent')) {
      void histJump(hit.dataset.oid);
      return true;
    }
    if (cls.contains('ps-hist-det-open')) {
      void openFile(hit.dataset.path, view.line, hit.dataset.oid);
      return true;
    }
    if (cls.contains('ps-hist-change')) {
      void openFile(hit.dataset.path, 0, hit.dataset.oid);
      return true;
    }
    if (cls.contains('ps-hist-row')) {
      histSelect(String(hit.dataset.oid || ''), false);
      return true;
    }
    return false;
  }

  // Which reading of this file the viewer stands on, worn on the hand that
  // opened it: the page says which control would take the numbered view back.
  function paintActiveHand() {
    const head = E('code-viewer-head');
    if (!head) return;
    for (const other of head.querySelectorAll('.ps-code-hand')) other.classList.remove('ps-code-hand-active');
    if (!view.mode) return;
    const chosen = E('code-viewer-' + view.mode);
    if (chosen) chosen.classList.add('ps-code-hand-active');
  }

  // The views the actions row opens. A press of the hand already in use takes the
  // numbered reading back, so one control holds one room rather than a stack of
  // modes the page cannot leave.
  function stepCodeMode(hand) {
    if (!view.path) {
      ui.toastPush('no file is open in this room, so there is nothing to read of a file', 'warn');
      return;
    }
    view.mode = view.mode === hand ? '' : hand;
    paintActiveHand();
    const read = readFiles[fileKey(view.path, view.ref)];
    const body = E('code-viewer-body');
    if (!body) return;
    // The history surface owns the body's scroll while it stands; every other
    // reading scrolls the body itself, and leaving the surface tears it down.
    body.classList.toggle('ps-code-body-hist', view.mode === 'history');
    if (view.mode !== 'history' && hist.surface) histTeardown();
    if (view.mode === 'history') {
      void renderHistory();
      return;
    }
    if (view.mode === 'blame') {
      void renderBlame();
      return;
    }
    if (!read || read.binary) {
      if (read && read.binary) {
        quiet(body, 'this file came back with no words: the machine sent bytes, not text',
          'A binary file has no lines to number and no words to read as they came.');
      }
      return;
    }
    if (view.mode === 'edit') {
      // The pane is made beside the reading, and the reading stays as it is:
      // the editor holds its own copy of the words the machine sent. A file the
      // machine sent as bytes is answered for above, and never reaches here.
      const pane = editorPane();
      if (pane) void renderEdit(pane, read);
      return;
    }
    if (view.mode === 'raw') {
      body.textContent = '';
      const whole = el('pre', 'ps-code-raw');
      const lines = String(read.text).split('\n');
      if (lines.length > kMaxViewLines) {
        whole.appendChild(document.createTextNode(lines.slice(0, kMaxViewLines).join('\n')));
        const cut = el('div', 'ps-code-cut', 'showing the first ' + kMaxViewLines + ' of ' + lines.length + ' lines');
        cut.title = 'The whole file came from one door; the words past what this page draws are not written here.';
        body.appendChild(cut);
      } else {
        whole.appendChild(document.createTextNode(String(read.text)));
      }
      whole.title = 'The file as it came: no numbers, no anchors, no marks. The bytes are the machine\'s.';
      body.appendChild(whole);
      return;
    }
    renderBody(read);
    if (view.line > 0) anchorLine(view.line);
  }

  // One remembered commit, oldest-gives-way: the Map keeps the order the names
  // arrived in, so the first one remembered is the first one let go of.
  function rememberCommit(oid, commit) {
    knownCommits.set(oid, commit);
    while (knownCommits.size > kKnownCommitCap) {
      const oldest = knownCommits.keys().next();
      if (oldest.done) break;
      knownCommits.delete(oldest.value);
    }
  }

  async function askHistory() {
    const key = fileKey(view.path, view.ref);
    const held = readHistories[key];
    if (held) return held.answer;
    const historyRead = await codeAsk(kHistoryDoor, [['path', view.path], ['ref', view.ref]]);
    const answer = historyRead.answer;
    if (!answer) return null;
    readHistories[key] = { answer: answer, at: Date.now() };
    if (Array.isArray(answer.commits)) {
      for (const commit of answer.commits) {
        if (commit && commit.oid) rememberCommit(String(commit.oid), commit);
      }
    }
    return answer;
  }

  // A moment the machine named for a commit, in the fewest words that still say
  // it. The house keeps epochs as seconds elsewhere; a door that already wrote
  // the moment in words is repeated as it wrote it.
  function whenWords(when) {
    if (typeof when === 'string') return when;
    if (typeof when === 'number' && when > 0) return ui.whenText(when);
    return '';
  }

  // ---------- the history surface (research plan §5 options 1 + 2) ----------
  // The door tells the walk; this page draws it. The rows are DOM — day groups
  // with human labels, and on every row the fields the widened door names:
  // short oid, author and relative moment, the message's first line with the
  // body revealed in place, ref chips, the first-parent diff's "+n −m" and the
  // count of paths it moved — and a chosen row is told in full in the detail
  // pane beside the list. The lane down the left is canvas.
  //
  // One canvas per day block, chosen over one tall canvas with a scroll offset
  // for three reasons, and this is the documented call: the block scrolls with
  // its own rows, so no scroll offset is ever kept in step; a long load-more
  // walk of a real repository would grow one tall canvas past the browser's
  // canvas-size ceiling, which per-block never happens; and a block off-screen
  // costs nothing until the IntersectionObserver brings it back, which is the
  // same reading the rows' own content-visibility gives - and it is the reason
  // the observer, not the walk, is what draws: a page of rows marks every block
  // as needing a picture and paints only the blocks standing in view, and a
  // block leaving view gives its store back on the way out. A walk of fifty
  // pages therefore costs the screen at each step, not the whole walk, and a
  // block whose lanes changed while it was away is drawn again the moment it is
  // seen. Lane geometry is computed once over every row loaded, so an edge
  // crossing a block boundary leaves one block's canvas at the column it enters
  // the next at, and the two halves meet.
  //
  // The lane rules are git's own, ported from graph.c: the first parent
  // inherits the commit's column, a branch never moves to the right (the fixed
  // position is what makes the lane legible), and the lane count is capped —
  // what gives up its column is marked with ~ on the row that folded, and the
  // first-parent spine is never the thing folded. A parent the rows do not
  // hold — older than what was loaded, or dropped by the machine's filter —
  // ends in a dashed stub: threading a lane through a commit this page has
  // never been shown would be a guess, and the page guesses nothing. The lane
  // decorates the walk; every fact it draws also stands in the row's words.
  const kHistPage = 100;      // rows asked of the door at one reading
  const kHistRowH = 30;       // the fixed visual line one row occupies
  const kHistLaneW = 14;      // px of canvas one lane column holds
  const kHistLanePad = 8;     // px of air left of the first lane
  const kHistLaneCap = 8;     // the house's log.maxLanes, git's own knob
  const kHistMonths = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun',
    'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

  const hist = {
    surface: null,   // the surface root, while history mode stands
    list: null,
    detail: null,
    tail: null,
    bar: null,
    fAuthor: null,
    fText: null,
    fNoMerges: null,
    fFirstParent: null,
    rows: [],
    byOid: {},
    days: [],
    selected: '',
    hasMore: false,
    why: '',
    gen: 0,
    more: false,
    io: null,
    resizeQueued: false,
    laneWidth: 0,
    fold: false,
    foldLane: 0,        // the column the ~ of a folded row stands in
    visible: new Set(), // the day blocks the observer reports as standing in view
    busy: null,         // the walk this surface has in flight, if any
    filterQueued: false,
  };
  let histColors = null;
  let histPaper = '';

  function histSeconds(when) {
    const secs = Number(String(when === undefined ? '' : when).trim().split(/\s+/)[0]);
    return Number.isFinite(secs) && secs > 0 ? secs : 0;
  }

  // A moment in the fewest words that still say it: relative while it is near,
  // and after a month the date itself, spelled the way the day headers spell it.
  function histAgo(secs) {
    if (!secs) return 'the machine stated no moment';
    const span = Date.now() / 1000 - secs;
    if (span < 60) return 'moments ago';
    if (span < 3600) {
      const n = Math.floor(span / 60);
      return n + (n === 1 ? ' minute ago' : ' minutes ago');
    }
    if (span < 86400) {
      const n = Math.floor(span / 3600);
      return n + (n === 1 ? ' hour ago' : ' hours ago');
    }
    if (span < 86400 * 30) {
      const n = Math.floor(span / 86400);
      return n + (n === 1 ? ' day ago' : ' days ago');
    }
    const d = new Date(secs * 1000);
    return kHistMonths[d.getMonth()] + ' ' + d.getDate() + ', ' + d.getFullYear();
  }

  function histDayLabel(secs) {
    if (!secs) return 'Commits whose moment the machine did not state';
    const d = new Date(secs * 1000);
    return 'Commits on ' + kHistMonths[d.getMonth()] + ' ' + d.getDate() + ', ' + d.getFullYear();
  }

  // The door decorates refs the way git does: "HEAD -> main", "tag: v1", and
  // "HEAD" alone where a detached head rests. Each chip says what kind it is.
  function histRefsChips(refs) {
    const made = [];
    const list = Array.isArray(refs) ? refs : [];
    for (const one of list) {
      for (const part of String(one).split(', ')) {
        const text = part.trim();
        if (!text) continue;
        let m = /^HEAD -> (.+)$/.exec(text);
        if (m) { made.push({ kind: 'head', name: m[1] }); continue; }
        m = /^tag: (.+)$/.exec(text);
        if (m) { made.push({ kind: 'tag', name: m[1] }); continue; }
        if (text === 'HEAD') { made.push({ kind: 'detached', name: 'HEAD' }); continue; }
        made.push({ kind: 'branch', name: text });
      }
    }
    return made;
  }

  function histRefTip(ref) {
    if (ref.kind === 'tag') return 'A tag standing exactly at this commit.';
    if (ref.kind === 'head') return 'The branch HEAD stands on, and it stands here.';
    if (ref.kind === 'detached') return 'HEAD itself rests here, detached from any branch.';
    return 'A branch tip standing exactly at this commit.';
  }

  function histBodyText(commit) {
    const full = typeof commit.message === 'string' ? commit.message : '';
    const first = commit.messageFirstLine ? String(commit.messageFirstLine) : '';
    if (!full) return '';
    if (first && full.slice(0, first.length) === first) return full.slice(first.length).replace(/^\s+/, '');
    return full;
  }

  function histNormRow(commit) {
    const oid = String(commit.oid);
    const when = histSeconds(commit.authorWhen);
    const row = {
      oid: oid,
      commit: commit,
      index: -1,
      parents: Array.isArray(commit.parents) ? commit.parents.map(String) : [],
      when: when,
      whenText: typeof commit.authorWhen === 'string' ? commit.authorWhen : whenWords(commit.authorWhen),
      refs: histRefsChips(commit.refs),
      expanded: false,
      node: null,
      bodyNode: null,
      block: null,
      lane: 0,
      incoming: [],
      verticals: [],
      outgoing: [],
      stub: false,
      fold: false,
    };
    row.merge = row.parents.length > 1;
    row.dayKey = histDayLabel(when);
    return row;
  }

  function histRowNode(row) {
    const c = row.commit;
    const node = el('div', 'ps-hist-row');
    node.dataset.oid = row.oid;
    // The fields stand in a row of their own that does not wrap: a narrow list
    // squeezes them and ellipsises what will not fit, rather than breaking the
    // row onto a second line. One row is one 30 px line - the unit the lane is
    // drawn against and the size an unwalked row answers with - and a row that
    // wrapped would stand twice as tall as every canvas drawn over it.
    const fields = el('span', 'ps-hist-fields');
    node.appendChild(fields);
    fields.appendChild(el('span', 'ps-hist-lanespace', null,
      'The lane the page draws for this commit stands here. Every fact the lane shows also stands in the words of this row.'));
    const chip = el('button', 'ps-hist-oid', row.oid.slice(0, 10),
      'Copy this commit\'s full object id to the clipboard: ' + row.oid);
    chip.dataset.oid = row.oid;
    fields.appendChild(chip);
    fields.appendChild(el('span', c.authorName ? 'ps-hist-author' : 'ps-fact-quiet',
      c.authorName ? String(c.authorName) : 'no author named',
      c.authorName ? 'The author, as the machine read the commit: ' + String(c.authorName) : 'the door named no author'));
    fields.appendChild(el('span', 'ps-hist-when', histAgo(row.when),
      row.when ? ui.whenText(row.when) + ' · the door stated ' + (row.whenText || 'only this number') : 'the machine stated no moment'));
    const firstLine = c.messageFirstLine ? String(c.messageFirstLine) : '';
    fields.appendChild(el('span', firstLine ? 'ps-hist-msg' : 'ps-fact-quiet',
      firstLine ? firstLine : 'the machine named no first line',
      firstLine || 'the door named no message'));
    if (row.merge) {
      fields.appendChild(el('span', 'ps-hist-merge', 'merge',
        'This commit carries ' + row.parents.length + ' parents; the lane shows where the lines join. A filter that drops merges would drop this row.'));
    }
    for (const ref of row.refs) {
      fields.appendChild(el('span', 'ps-hist-ref ps-hist-ref-' + ref.kind, ref.name, histRefTip(ref)));
    }
    const stats = c.stats;
    // The count of paths first, from either voice the door states it, so the
    // line of counts and its tip never say a number the machine did not state.
    const files = stats && Number.isFinite(Number(stats.files))
      ? Number(stats.files)
      : (Array.isArray(c.changed) ? c.changed.length : 0);
    if (stats && Number.isFinite(Number(stats.added)) && Number.isFinite(Number(stats.deleted))) {
      const stat = el('span', 'ps-hist-stat', null,
        'The first-parent diff, as the machine counted it: ' + Number(stats.added) + ' lines added, ' +
        Number(stats.deleted) + ' deleted, across ' + files + ' paths.');
      stat.appendChild(el('span', 'ps-hist-add', '+' + Number(stats.added)));
      stat.appendChild(el('span', 'ps-hist-del', '−' + Number(stats.deleted)));
      fields.appendChild(stat);
    } else {
      fields.appendChild(el('span', 'ps-fact-quiet', 'no counts stated',
        'The door stated no numbers for this commit, so no numbers are written here.'));
    }
    fields.appendChild(el('span', 'ps-hist-files', files + (files === 1 ? ' file' : ' files'),
      'The number of paths the machine named for this commit' +
      (c.changedTruncated === true ? ' — and it cut the list of paths at its bound' : '') +
      '. The paths themselves are named in full in the pane beside the list.'));
    const bodyWords = histBodyText(c);
    if (bodyWords || c.messageTruncated === true) {
      const hand = el('button', 'ps-hist-expand', 'show body',
        'Show the rest of this message in place, without leaving the list. The words are the machine\'s, bounded where the door bounds them.');
      hand.dataset.oid = row.oid;
      fields.appendChild(hand);
    }
    row.node = node;
    return node;
  }

  function histDayBlock(label) {
    const node = el('div', 'ps-hist-day');
    node.appendChild(el('div', 'ps-hist-dayhead', label,
      'A day group: every row under it carries its author moment on this day, as the machine stated it.'));
    const rowsEl = el('div', 'ps-hist-rows');
    const canvas = el('canvas', 'ps-hist-lane');
    canvas.title = 'The walk lane: one column per line of development. The dot is this commit; the line straight down it is its first parent; a curve is a branch joining. It decorates the walk — nothing it shows is only here.';
    rowsEl.appendChild(canvas);
    node.appendChild(rowsEl);
    // A block's canvas is drawn while the block stands in view and let go of
    // again when it leaves: painted says a store is held, dirty says the picture
    // held - if any - no longer matches the walk.
    const block = {
      label: label, node: node, rowsEl: rowsEl, canvas: canvas, rows: [],
      painted: false, dirty: true,
    };
    hist.days.push(block);
    hist.list.appendChild(node);
    if (hist.io) hist.io.observe(node);
    return block;
  }

  // The lane allocation, over every row loaded. graph.c's column rules: the
  // first parent inherits the commit's column; extra parents curve to a column
  // of their own and a branch never moves to the right; past the cap, extra
  // parents fold and the row wears ~. Lanes are assigned once, in walk order,
  // and are never re-taken to tidy a picture.
  function histLayout() {
    const lanes = []; // each: {await: oid|null, from: row index that took it}
    let maxLane = -1;
    let fold = false;
    for (let i = 0; i < hist.rows.length; ++i) {
      const row = hist.rows[i];
      row.incoming = [];
      row.verticals = [];
      row.outgoing = [];
      row.stub = false;
      row.fold = false;
      let mine = -1;
      for (let l = 0; l < lanes.length; ++l) {
        if (lanes[l].await === row.oid) { mine = l; break; }
      }
      if (mine < 0) {
        for (let l = 0; l < lanes.length; ++l) {
          if (lanes[l].await === null) { mine = l; break; }
        }
        if (mine < 0) {
          lanes.push({ await: null, from: -1 });
          mine = lanes.length - 1;
        }
      }
      row.lane = mine;
      for (let l = 0; l < lanes.length; ++l) {
        if (lanes[l].await === row.oid) {
          row.incoming.push({ lane: l, straight: l === mine });
          lanes[l].await = null;
          lanes[l].from = -1;
        }
      }
      for (let l = 0; l < lanes.length; ++l) {
        if (l !== mine && lanes[l].await !== null && lanes[l].from < i) row.verticals.push(l);
      }
      let firstHeld = false;
      for (let k = 0; k < row.parents.length; ++k) {
        const p = row.parents[k];
        const pRow = hist.byOid[p];
        const shown = !!pRow && pRow.index > i;
        if (k === 0) {
          if (shown) {
            lanes[mine].await = p;
            lanes[mine].from = i;
            row.outgoing.push({ lane: mine, straight: true });
            firstHeld = true;
          } else {
            // Older than what is loaded, or dropped by a filter: the spine
            // says it continues, and does not pretend where it lands.
            row.stub = true;
          }
        } else if (shown) {
          let l = -1;
          for (let n = 0; n < lanes.length; ++n) {
            if (lanes[n].await === p) { l = n; break; }
          }
          if (l < 0) {
            for (let n = 0; n < lanes.length && n < kHistLaneCap; ++n) {
              if (lanes[n].await === null) { l = n; break; }
            }
            if (l < 0 && lanes.length < kHistLaneCap) {
              lanes.push({ await: null, from: -1 });
              l = lanes.length - 1;
            }
            if (l < 0) {
              row.fold = true;
              fold = true;
              continue;
            }
            lanes[l].await = p;
            lanes[l].from = i;
          }
          row.outgoing.push({ lane: l, straight: l === mine });
        }
      }
      if (mine > maxLane) maxLane = mine;
      for (const out of row.outgoing) if (out.lane > maxLane) maxLane = out.lane;
      for (const inw of row.incoming) if (inw.lane > maxLane) maxLane = inw.lane;
      if (!firstHeld && row.parents.length === 0) {
        lanes[mine].await = null;
        lanes[mine].from = -1;
      }
    }
    hist.fold = fold;
    // The width covers every lane the walk took, the ~ column when anything
    // folded, and the rare spine lane taken past the cap: the cap folds
    // extra parents, never the line a row stands in. The ~ stands in that same
    // last column - past every lane the walk took, which is no longer the
    // cap's own column once the first-parent path has pushed wider than the cap.
    hist.foldLane = Math.max(maxLane + 1, kHistLaneCap);
    hist.laneWidth = kHistLanePad +
      (hist.foldLane + (fold ? 1 : 0)) * kHistLaneW + 4;
  }

  // Colours are the house's own tokens, read once, cycled the way git cycles
  // graphColors — no colour here is invented.
  function histPalette() {
    if (histColors) return histColors;
    const names = ['--accent', '--accent2', '--accent4', '--ok', '--unknown', '--warn', '--err', '--fg-dim'];
    const read = getComputedStyle(document.documentElement);
    histColors = [];
    for (const name of names) {
      const value = String(read.getPropertyValue(name) || '').trim();
      histColors.push(value ? value : '#8a95a5');
    }
    histPaper = String(read.getPropertyValue('--bg') || '').trim() || '#0c0f13';
    return histColors;
  }

  function histLaneX(lane) {
    return kHistLanePad + lane * kHistLaneW + kHistLaneW / 2;
  }

  function histLaneStyle(ctx, lane) {
    const colors = histPalette();
    ctx.strokeStyle = colors[lane % colors.length];
    ctx.fillStyle = colors[lane % colors.length];
    ctx.lineWidth = 1.4;
    ctx.setLineDash([]);
  }

  // The blocks standing in view are drawn, and the ones the walk changed while
  // out of view are drawn when the observer brings them back. Painting every
  // block on every page of a walk is work nobody sees - and a store kept for a
  // block scrolled past is memory nobody has, which is the whole reason the
  // lanes are per block. So a walk marks every block needing a picture and
  // draws only the few the observer says are on the screen.
  function histRepaint() {
    for (const block of hist.days) block.dirty = true;
    for (const block of hist.visible) {
      if (block.rows.length) histPaintBlock(block);
    }
  }

  // Let go of a block's picture: the canvas keeps the CSS box it was sized to,
  // so the rows under it never shift, and only the store behind the pixels goes.
  function histReleaseBlock(block) {
    if (!block || !block.canvas) return;
    if (block.painted) {
      block.canvas.width = 0;
      block.canvas.height = 0;
    }
    block.painted = false;
    block.dirty = true;
  }

  function histPaintBlock(block) {
    if (!block || !block.canvas || !block.rows.length) return;
    const dpr = Math.max(1, Number(window.devicePixelRatio) || 1);
    const w = hist.laneWidth;
    const h = block.rowsEl.offsetHeight;
    if (h <= 0) return;
    const canvas = block.canvas;
    canvas.style.width = w + 'px';
    canvas.style.height = h + 'px';
    if (canvas.width !== Math.round(w * dpr)) canvas.width = Math.round(w * dpr);
    if (canvas.height !== Math.round(h * dpr)) canvas.height = Math.round(h * dpr);
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    // Heights are summed from the DOM rather than read per row, so a body
    // opened in place shifts the rows below it and the lane shifts with them.
    // The sum is honest about a row this window has never laid out because the
    // row is one 30 px line by construction - ps-style.css keeps every field on
    // the one line (nowrap, with the fields ellipsised rather than wrapped) and
    // states that same height as the row's contain-intrinsic-size, so a row
    // skipped under content-visibility contributes what it would have occupied.
    // Only an opened body grows a row, and a row whose body is open has been
    // laid out, so its measured height is what this running sum reads.
    const ys = [];
    let running = 0;
    for (const row of block.rows) {
      ys.push(running);
      const rh = row.node && row.node.offsetHeight > 0 ? row.node.offsetHeight : kHistRowH;
      running += rh;
    }
    for (let at = 0; at < block.rows.length; ++at) {
      const row = block.rows[at];
      const y = ys[at];
      const bottom = at + 1 < ys.length ? ys[at + 1] : h; // straight runs carry through an opened body
      const cy = y + kHistRowH / 2;
      const xDot = histLaneX(row.lane);
      for (const inw of row.incoming) {
        histLaneStyle(ctx, inw.lane);
        ctx.beginPath();
        if (inw.straight) {
          ctx.moveTo(histLaneX(inw.lane), y);
          ctx.lineTo(xDot, cy);
        } else {
          const mid = (y + cy) / 2;
          ctx.moveTo(histLaneX(inw.lane), y);
          ctx.bezierCurveTo(histLaneX(inw.lane), mid, xDot, mid, xDot, cy);
        }
        ctx.stroke();
      }
      for (const lane of row.verticals) {
        histLaneStyle(ctx, lane);
        ctx.beginPath();
        ctx.moveTo(histLaneX(lane), y);
        ctx.lineTo(histLaneX(lane), bottom);
        ctx.stroke();
      }
      for (const out of row.outgoing) {
        histLaneStyle(ctx, out.lane);
        ctx.beginPath();
        if (out.straight) {
          ctx.moveTo(xDot, cy);
          ctx.lineTo(xDot, bottom);
        } else {
          const mid = (cy + bottom) / 2;
          ctx.moveTo(xDot, cy);
          ctx.bezierCurveTo(xDot, mid, histLaneX(out.lane), mid, histLaneX(out.lane), bottom);
        }
        ctx.stroke();
      }
      if (row.stub) {
        histLaneStyle(ctx, row.lane);
        ctx.save();
        ctx.globalAlpha = 0.55;
        ctx.setLineDash([3, 3]);
        ctx.beginPath();
        ctx.moveTo(xDot, cy + 5);
        ctx.lineTo(xDot, bottom);
        ctx.stroke();
        ctx.restore();
      }
      const dotColor = histPalette()[row.lane % histPalette().length];
      ctx.beginPath();
      ctx.arc(xDot, cy, 3, 0, Math.PI * 2);
      if (row.merge) {
        ctx.fillStyle = histPaper;
        ctx.fill();
        histLaneStyle(ctx, row.lane);
        ctx.lineWidth = 1.8;
        ctx.stroke();
      } else {
        ctx.fillStyle = dotColor;
        ctx.fill();
      }
      if (row.fold) {
        ctx.fillStyle = histPalette()[histPalette().length - 1];
        ctx.font = '13px "Cascadia Mono", Consolas, monospace';
        ctx.fillText('~', kHistLanePad + hist.foldLane * kHistLaneW, cy + 4);
      }
    }
    block.painted = true;
    block.dirty = false;
  }

  function histBlockFor(node) {
    for (const block of hist.days) if (block.node === node) return block;
    return null;
  }

  function histResized() {
    if (!hist.surface || hist.resizeQueued) return;
    hist.resizeQueued = true;
    setTimeout(function () {
      hist.resizeQueued = false;
      // The window the lane is drawn under changed: every block needs a picture
      // at the new size, and only the blocks on the screen are drawn now. The
      // rest are drawn by the observer as they come in - a resize moves blocks
      // into view as often as it moves them out.
      if (hist.surface) histRepaint();
    }, 200);
  }

  // The list is emptied: every block it held is detached from the page, so the
  // rows and day groups that pointed at those blocks go with it. Keeping them
  // would leave the next "older rows" appending into a node nobody can see.
  function histForgetRows() {
    hist.rows = [];
    hist.byOid = {};
    hist.days = [];
    hist.visible.clear();
    if (hist.io) hist.io.disconnect();
  }

  function histTeardown() {
    if (hist.io) {
      hist.io.disconnect();
      hist.io = null;
    }
    hist.surface = null;
    hist.list = null;
    hist.detail = null;
    hist.tail = null;
    hist.bar = null;
    hist.fAuthor = null;
    hist.fText = null;
    hist.fNoMerges = null;
    hist.fFirstParent = null;
    hist.rows = [];
    hist.byOid = {};
    hist.days = [];
    hist.selected = '';
    hist.hasMore = false;
    hist.why = '';
    hist.more = false;
    hist.visible.clear();
    hist.busy = null;
    hist.filterQueued = false;
    ++hist.gen;
  }

  // The surface walks one project at one path, so it does not survive the
  // project this page stands on: the room comes down out of the walk first and
  // states what it put away, rather than holding a list of rows the machine
  // walked about a project nobody is looking at. The teardown bumps the walk's
  // generation, so an answer the old project's ask brings back is heard and
  // dropped, and the filter fields that could mix the two projects are gone with
  // the surface they stood in.
  function histStandDown() {
    if (!hist.surface) return;
    const body = E('code-viewer-body');
    histTeardown();
    view.mode = '';
    paintActiveHand();
    if (body) {
      body.classList.remove('ps-code-body-hist');
      body.textContent = '';
      quiet(body, 'the history this room showed walked the project that was open here',
        'The room stands on another project now: a walk of one project is not a walk of another, so the list came down rather than carry its rows across the two. Press history again to walk this project at the file this room names.');
    }
  }

  function histPaintTail() {
    if (!hist.tail) return;
    hist.tail.textContent = '';
    if (hist.hasMore) {
      const hand = el('button', 'ps-code-hand ps-hist-more',
        hist.more ? 'the machine is answering' : 'older rows',
        'Ask the door for the rows older than the last row drawn here: the same walk, the same filters, continuing at its cursor.');
      if (hist.more) hand.disabled = true;
      hist.tail.appendChild(hand);
    } else if (hist.rows.length) {
      hist.tail.appendChild(el('span', 'ps-quiet', 'no older rows stand at this door',
        'The walk this page asked ends here: the machine said no row waits beyond the last one drawn.'));
    }
    if (hist.why) {
      hist.tail.appendChild(el('div', 'ps-code-cut', 'the machine cut this walk: ' + hist.why,
        'The door states the bound it stopped at. This page draws what it heard and names what it did not.'));
    }
  }

  function histSelect(oid, scroll) {
    const row = hist.byOid[String(oid)];
    if (!row || !row.node || !hist.list) return;
    hist.selected = row.oid;
    for (const other of hist.list.querySelectorAll('.ps-hist-sel')) other.classList.remove('ps-hist-sel');
    row.node.classList.add('ps-hist-sel');
    if (scroll) {
      // Read from the rects, not the offsetTop chain: none of the hosts in
      // between are positioned, so an offsetParent walk would have nothing
      // honest to add. This stands the row a third down the window.
      const rect = row.node.getBoundingClientRect();
      const room = hist.list.getBoundingClientRect();
      hist.list.scrollTop += rect.top - room.top - hist.list.clientHeight / 3;
    }
    histPaintDetail(row);
  }

  function histToggle(oid) {
    const row = hist.byOid[String(oid)];
    if (!row || !row.node) return;
    row.expanded = !row.expanded;
    const hand = row.node.querySelector('.ps-hist-expand');
    if (row.expanded) {
      const words = histBodyText(row.commit);
      row.bodyNode = el('div', 'ps-hist-bodywrap');
      row.bodyNode.appendChild(el('pre', 'ps-hist-body',
        words ? words : (row.commit.messageTruncated === true ? 'the machine named no body words before cutting the message at its bound' : '')));
      if (row.commit.messageTruncated === true) {
        row.bodyNode.appendChild(el('div', 'ps-code-cut', 'the machine cut this message at its bound'));
      }
      row.node.appendChild(row.bodyNode);
      row.node.classList.add('ps-hist-open');
      if (hand) {
        hand.textContent = 'hide body';
        hand.title = 'Take the body away and leave the row as it stood.';
      }
    } else {
      if (row.bodyNode) {
        row.bodyNode.remove();
        row.bodyNode = null;
      }
      row.node.classList.remove('ps-hist-open');
      if (hand) {
        hand.textContent = 'show body';
        hand.title = 'Show the rest of this message in place, without leaving the list. The words are the machine\'s, bounded where the door bounds them.';
      }
    }
    histPaintBlock(row.block);
  }

  // The detail pane: one commit told completely, out of the same door fields
  // the row reads — the message entire, both hands and both moments, the
  // parents as links into the list, and the paths the commit moved, each a
  // press that opens the viewer at this commit.
  function histPaintDetail(row) {
    if (!hist.detail) return;
    const host = hist.detail;
    host.textContent = '';
    if (!row) {
      quiet(host, 'no row is chosen here, so nothing is told about one',
        'A press of a row in the list beside it opens its full telling here: the message, the hands and their moments, the parents as links, and the paths the commit moved.');
      return;
    }
    const c = row.commit;
    const headRow = el('div', 'ps-hist-det-head');
    const oidBtn = el('button', 'ps-hist-det-oid', row.oid, 'Copy this commit\'s full object id to the clipboard.');
    oidBtn.dataset.oid = row.oid;
    headRow.appendChild(oidBtn);
    for (const ref of row.refs) {
      headRow.appendChild(el('span', 'ps-hist-ref ps-hist-ref-' + ref.kind, ref.name, histRefTip(ref)));
    }
    host.appendChild(headRow);
    const hands = el('div', 'ps-hist-det-hands');
    hands.appendChild(el('div', c.authorName ? null : 'ps-fact-quiet',
      (c.authorName ? String(c.authorName) : 'no author named') + ' authored' +
      (row.when ? ' · ' + ui.whenText(row.when) + ' · ' + histAgo(row.when) : ' · a moment the machine did not state'),
      row.whenText ? 'The door stated the moment as ' + row.whenText : 'the door stated no author moment'));
    const cSecs = histSeconds(c.committerWhen);
    if (c.committerName || cSecs) {
      hands.appendChild(el('div', null,
        (c.committerName ? String(c.committerName) : 'a committer the machine did not name') + ' committed' +
        (cSecs ? ' · ' + ui.whenText(cSecs) + ' · ' + histAgo(cSecs) : '') +
        (cSecs && row.when && cSecs !== row.when ? ' · ' + Math.round(Math.abs(cSecs - row.when) / 60) + ' minutes apart' : ''),
        c.committerWhen ? 'The door stated the moment as ' + String(c.committerWhen) : 'the door stated the committer but no moment'));
    } else {
      hands.appendChild(el('div', 'ps-fact-quiet', 'the machine named no committer for this commit'));
    }
    host.appendChild(hands);
    const fullMessage = typeof c.message === 'string' ? c.message : '';
    host.appendChild(el('pre', 'ps-hist-det-msg',
      fullMessage ? fullMessage : (c.messageFirstLine ? String(c.messageFirstLine) : 'the machine named no message')));
    if (c.messageTruncated === true) {
      host.appendChild(el('div', 'ps-code-cut', 'the machine cut this message at its bound'));
    }
    const parRow = el('div', 'ps-hist-det-par');
    parRow.appendChild(el('span', 'ps-hist-det-name', row.parents.length ? 'parents' : 'parentless'));
    if (row.parents.length) {
      for (const p of row.parents) {
        const link = el('button', 'ps-hist-det-parent', String(p).slice(0, 10),
          'Jump the list to this parent: the rows load at the walk\'s cursor until it stands here, and it is chosen.');
        link.dataset.oid = String(p);
        parRow.appendChild(link);
      }
    } else {
      parRow.appendChild(el('span', 'ps-fact-quiet', 'the machine names no parent for this commit'));
    }
    host.appendChild(parRow);
    const st = c.stats;
    const detFiles = st && Number.isFinite(Number(st.files))
      ? Number(st.files)
      : (Array.isArray(c.changed) ? c.changed.length : 0);
    // A sum is drawn only when every number in it was stated: a door that named
    // the paths it moved but not the lines it added or cut gets the honest
    // absence, not a line of +NaN.
    if (st && Number.isFinite(Number(st.added)) && Number.isFinite(Number(st.deleted))) {
      host.appendChild(el('div', 'ps-hist-det-sum',
        detFiles + (detFiles === 1 ? ' file changed' : ' files changed') +
        ' · +' + Number(st.added) + ' −' + Number(st.deleted) + ' · the first-parent diff, as the door counted it'));
    } else {
      host.appendChild(el('div', 'ps-fact-quiet', 'the machine stated no counts for this commit'));
    }
    if (c.changedTruncated === true) {
      host.appendChild(el('div', 'ps-code-cut', 'the machine cut the list of paths here at its bound'));
    }
    const changes = Array.isArray(c.changed) ? c.changed : [];
    if (!changes.length) {
      host.appendChild(el('div', 'ps-quiet', 'the machine named no changed paths for this commit',
        'The door walked this commit and stated no paths it moved. A cut list says so above rather than here.'));
    }
    for (const ch of changes) {
      const path = ch && ch.path ? String(ch.path) : '';
      if (!path) continue;
      const rowEl = el('button', 'ps-hist-change', null,
        'Open ' + path + ' in the viewer as this commit holds it: the ref at the head is then this commit\'s own.');
      rowEl.dataset.path = path;
      rowEl.dataset.oid = row.oid;
      rowEl.appendChild(el('span', 'ps-hist-change-path', path));
      let mark = 'no counts stated';
      let markCls = 'ps-hist-change-mark';
      if (ch.binary === true) mark = 'binary';
      else if (ch.large === true) mark = 'past the counting bound';
      else if (Number.isFinite(Number(ch.added)) || Number.isFinite(Number(ch.deleted))) {
        mark = '+' + Number(ch.added || 0) + ' −' + Number(ch.deleted || 0);
        markCls = 'ps-hist-change-mark ps-hist-stat-ok';
      }
      rowEl.appendChild(el('span', markCls, mark,
        mark === 'no counts stated'
          ? 'the path is here and the machine stated no counts for it: not a pair it could count exactly — binary, past the size it counts, or a change past the diff\'s budget (the walk\'s own note says when the budget cut)'
          : ''));
      host.appendChild(rowEl);
    }
    if (view.path) {
      const open = el('button', 'ps-code-hand ps-hist-det-open', 'read ' + view.path + ' at this commit',
        'Open the file this room stands on as this commit holds it: the ref named at the head is then this commit\'s oid, and every door of this room is asked at it.');
      open.dataset.oid = row.oid;
      open.dataset.path = view.path;
      host.appendChild(open);
    }
  }

  // A parent pressed in the detail pane: the list walks on at its cursor until
  // the parent stands in it, then chooses it. Six pages is this page's bound
  // on the search; past it the absence is stated, not hunted.
  async function histJump(oid) {
    const surface = hist.surface;
    let row = hist.byOid[String(oid)];
    let tries = 0;
    while (!row && hist.hasMore && tries < 6) {
      // A walk already in flight is waited for, not walked past: an ask beside
      // it answers nothing - the surface has an ask standing, so the six tries
      // would all be spent on refusals and the absence stated as though the
      // list had been searched. The page that walk brings back is the next step
      // of this one, and the wait is heard again each time, so a second ask
      // landing beside it cannot leave this one asking into a busy surface.
      while (hist.busy) {
        await hist.busy;
        if (hist.surface !== surface) return;
      }
      if (hist.surface !== surface) return;
      const last = hist.rows.length ? hist.rows[hist.rows.length - 1] : null;
      if (!last) break;
      await histAsk(last.oid);
      if (hist.surface !== surface) return;
      row = hist.byOid[String(oid)];
      ++tries;
    }
    if (!row) {
      ui.toastPush('that commit is not among the rows this page has walked: the list here covers the walk from its head, and the parent is older than what was asked or dropped by a filter', 'warn');
      return;
    }
    histSelect(row.oid, true);
  }

  function histFilterParams() {
    const params = [['path', view.path], ['ref', view.ref], ['limit', kHistPage]];
    const author = fieldText(hist.fAuthor);
    const text = fieldText(hist.fText);
    if (author) params.push(['author', author]);
    if (text) params.push(['text', text]);
    if (hist.fNoMerges && hist.fNoMerges.checked) params.push(['no_merges', '1']);
    if (hist.fFirstParent && hist.fFirstParent.checked) params.push(['first_parent', '1']);
    return params;
  }

  // One walk asked, one page answered: after= is the last row drawn here, the
  // same cursor the door states; absent, the walk is asked from its head. The
  // walk's generation: an answer to an ask this surface has re-aimed in the
  // meantime — a filter changed while the machine was thinking, a teardown —
  // is heard and dropped, never drawn over the ask that stands.
  //
  // The ask standing is also stated as a promise, so a caller that needs the
  // page it brings back - a jump to a commit not drawn yet - can wait for it
  // instead of asking beside it and spending its tries on answers that go
  // nowhere.
  async function histAsk(after, force) {
    if (!hist.surface || !hist.list) return;
    if (hist.more && !force) return;
    const gen = ++hist.gen;
    hist.more = true;
    histPaintTail();
    const asking = histWalkAsk(after, gen);
    hist.busy = asking;
    try {
      await asking;
    } finally {
      // Only the ask that still stands lets go of the surface's word for busy:
      // an answer dropped by a newer ask must not free the newer one.
      if (hist.busy === asking) hist.busy = null;
    }
  }

  async function histWalkAsk(after, gen) {
    const walkRead = await codeAsk(kHistoryDoor, histFilterParams().concat(after ? [['after', String(after)]] : []));
    if (gen !== hist.gen) return;
    const answer = walkRead.answer;
    hist.more = false;
    if (!answer) {
      refusedAt(walkRead, hist.list, E('code-viewer-note'), kHistoryDoor,
        'No file log came back, so nothing here is a story of who changed this file.');
      // The refusal is drawn in the list's own place: the rows that stood there
      // are off the page now, so they are put away with it. A surface that kept
      // pointing at detached blocks would append the next "older rows" into
      // nothing, and the walk would look alive while it drew into a void.
      histForgetRows();
      hist.selected = '';
      histPaintDetail(null);
      histPaintTail();
      return;
    }
    if (!Array.isArray(answer.commits)) {
      quiet(hist.list, 'the machine answered this door without naming any commits',
        'An answer that names no commits is not drawn as a file with no history: the room keeps what it heard and says it heard nothing.');
      histForgetRows();
      hist.selected = '';
      histPaintDetail(null);
      histPaintTail();
      return;
    }
    hist.hasMore = answer.hasMore === true;
    hist.why = answer.truncatedWhy ? String(answer.truncatedWhy) : '';
    for (const commit of answer.commits) {
      if (commit && commit.oid) rememberCommit(String(commit.oid), commit);
    }
    if (!after) {
      hist.list.textContent = '';
      histForgetRows();
    }
    if (!answer.commits.length && !hist.rows.length) {
      quiet(hist.list, 'the machine names no commits for this file',
        'The file log door listed the file and no commit carries it. A file that was never committed, or one the filters named leave nothing of, comes back here.');
    }
    for (const commit of answer.commits) {
      if (!commit || !commit.oid) continue;
      const row = histNormRow(commit);
      row.index = hist.rows.length;
      hist.rows.push(row);
      hist.byOid[row.oid] = row;
      let block = hist.days.length && hist.days[hist.days.length - 1].label === row.dayKey
        ? hist.days[hist.days.length - 1]
        : histDayBlock(row.dayKey);
      row.block = block;
      block.rows.push(row);
      const rowNode = histRowNode(row);
      // A row redrawn under a choice this page still holds wears the choice
      // again: the pane beside the list has been telling it throughout, and the
      // list must agree with the telling rather than point at nothing.
      if (row.oid === hist.selected) rowNode.classList.add('ps-hist-sel');
      block.rowsEl.appendChild(rowNode);
    }
    if (hist.io) for (const block of hist.days) hist.io.observe(block.node);
    histLayout();
    hist.list.style.setProperty('--hist-lane-w', hist.laneWidth + 'px');
    histRepaint();
    histPaintTail();
    if (hist.selected && hist.byOid[hist.selected]) histPaintDetail(hist.byOid[hist.selected]);
    else {
      if (hist.selected) hist.selected = '';
      histPaintDetail(null);
    }
    note(E('code-viewer-note'), 'walked by the machine for ' + view.path + ' at ' +
      (answer.ref ? String(answer.ref) : 'the machine\'s own head') +
      ' · ' + hist.rows.length + ' rows drawn of this walk' + asOfTail(answer), '',
      'Every row is a commit the machine named for this path, in the order the walk found it. A press of a row tells it in the pane beside the list; the hand in that telling opens the file as the commit holds it.');
  }

  async function renderHistory() {
    const body = E('code-viewer-body');
    if (!body) return;
    body.classList.add('ps-code-body-hist');
    histTeardown();
    const surface = el('div', 'ps-hist');
    surface.title = 'The history of this file as the machine walks it: the lane of the walk down the left, one row per commit, and a chosen commit told in full beside the list.';
    hist.surface = surface;

    const bar = el('div', 'ps-hist-bar');
    hist.bar = bar;
    bar.appendChild(el('span', 'ps-hist-scope', 'the log of ' + view.path + ' at ' +
      (view.ref ? view.ref : 'the machine\'s own head'),
      'Every filter here scopes to this path and this head. The door answers the walk it walked at this ask, and the room says so.'));
    const mkField = function (id, words, tip) {
      const label = el('label', 'ps-hist-toggle', words);
      label.htmlFor = id;
      label.title = tip;
      const field = el('input', 'ps-hist-field');
      field.id = id;
      field.type = 'text';
      field.title = tip;
      bar.appendChild(label);
      bar.appendChild(field);
      return field;
    };
    hist.fAuthor = mkField('ps-hist-author', 'author',
      'Words to look for in the author name, case-insensitive, as the door matches them. The answer is the walk with those rows left in.');
    hist.fText = mkField('ps-hist-text', 'message',
      'Words to look for in the whole commit message, case-insensitive, as the door matches them.');
    const mkToggle = function (id, words, tip) {
      const label = el('label', 'ps-hist-toggle');
      label.htmlFor = id;
      label.title = tip;
      const box = el('input', null);
      box.id = id;
      box.type = 'checkbox';
      box.title = tip;
      label.appendChild(box);
      label.appendChild(el('span', null, words));
      bar.appendChild(label);
      return box;
    };
    hist.fNoMerges = mkToggle('ps-hist-nomerges', 'no merges',
      'Leave merge commits out of the list. The walk still follows their lines — a dropped merge is walked through, not around, the way the engine states.');
    hist.fFirstParent = mkToggle('ps-hist-firstparent', 'first parent',
      'Follow only each commit\'s first parent: the mainline of the walk, and the lane that never branches.');
    const again = el('button', 'ps-code-hand', 'ask again',
      'Re-ask the machine from the head of the walk: rows already drawn are put away and the current filters are sent again.');
    again.classList.add('ps-hist-again');
    bar.appendChild(again);

    const mainRow = el('div', 'ps-hist-main');
    const list = el('div', 'ps-hist-list');
    list.title = 'One row per commit, newest first in the walk: day groups, the lane down the left, and the fields the door named for each commit. This window scrolls; the pane beside it keeps its telling.';
    const detail = el('div', 'ps-hist-detail');
    detail.title = 'The chosen commit, told in full. Nothing here is a second reading of the machine: it is the same answer the row stands on.';
    mainRow.appendChild(list);
    mainRow.appendChild(detail);
    const tail = el('div', 'ps-hist-tail');
    surface.appendChild(bar);
    surface.appendChild(mainRow);
    surface.appendChild(tail);
    body.textContent = '';
    body.appendChild(surface);
    hist.list = list;
    hist.detail = detail;
    hist.tail = tail;
    histPaintDetail(null);

    // The observer is what draws the lanes: a block coming into the list's view
    // is painted, and a block leaving it gives its store back and marks itself
    // as needing a picture. So a block that left and came back is drawn again
    // whatever moved while it was away - a longer walk reaching a lane it holds,
    // a resize, a body opened in a block above - and no block out of view is
    // ever painted or held.
    hist.io = new IntersectionObserver(function (entries) {
      for (const entry of entries) {
        const block = histBlockFor(entry.target);
        if (!block) continue;
        if (!entry.isIntersecting) {
          hist.visible.delete(block);
          histReleaseBlock(block);
          continue;
        }
        hist.visible.add(block);
        if (block.painted && !block.dirty) continue;
        histPaintBlock(block);
      }
    }, { root: list });

    // A typed filter is one ask, not one per keystroke: the same beat the files
    // column holds its search to, and the same one beat standing in the queue -
    // a keystroke beside a beat already waiting adds nothing to the line.
    const hear = function () {
      if (!hist.surface || hist.filterQueued) return;
      hist.filterQueued = true;
      setTimeout(function () {
        hist.filterQueued = false;
        if (hist.surface) void histAsk(null, true);
      }, kSearchWait);
    };
    if (hist.fAuthor) {
      hist.fAuthor.addEventListener('change', hear);
      hist.fAuthor.addEventListener('input', hear);
      hist.fAuthor.addEventListener('keyup', hear);
    }
    if (hist.fText) {
      hist.fText.addEventListener('change', hear);
      hist.fText.addEventListener('input', hear);
      hist.fText.addEventListener('keyup', hear);
    }
    if (hist.fNoMerges) {
      hist.fNoMerges.addEventListener('change', function () {
        if (hist.surface) void histAsk(null, true);
      });
    }
    if (hist.fFirstParent) {
      hist.fFirstParent.addEventListener('change', function () {
        if (hist.surface) void histAsk(null, true);
      });
    }
    await histAsk(null);
  }

  async function renderBlame() {
    const body = E('code-viewer-body');
    if (!body) return;
    body.textContent = '';
    const blameRead = await codeAsk(kBlameDoor, [['path', view.path], ['ref', view.ref]]);
    const answer = blameRead.answer;
    if (!answer) {
      refusedAt(blameRead, body, E('code-viewer-note'), kBlameDoor,
        'No blame came back, so nothing here names who wrote a line.');
      return;
    }
    const lines = Array.isArray(answer.lines) ? answer.lines : null;
    if (!lines) {
      quiet(body, 'the machine answered this door without naming any lines',
        'An answer that names no lines is not drawn as a file no one wrote: the room says what it heard.');
      return;
    }
    await askHistory();
    const read = readFiles[fileKey(view.path, view.ref)];
    const words = read && read.text !== null ? String(read.text).split('\n') : [];
    const shown = lines.length > kMaxViewLines ? kMaxViewLines : lines.length;
    for (let at = 0; at < shown; ++at) {
      const line = lines[at];
      // The blame door answers lines as bare commit-oid strings per line.
      const oid = typeof line === 'string' ? line : (line && line.commitOid ? String(line.commitOid) : '');
      const row = codeLine(String(at + 1), at < words.length ? words[at] : '');
      if (oid) {
        const fact = knownCommits.get(oid);
        const chip = el('button', 'ps-code-chip', oid.slice(0, 8));
        chip.dataset.oid = oid;
        chip.title = 'This line as the machine read it was written by ' +
          (fact && fact.authorName ? String(fact.authorName) : 'an author the machine did not name') +
          ' at ' + (fact && fact.authorWhen ? whenWords(fact.authorWhen) : 'a moment the machine did not state') +
          '. Pressing it opens the file as this commit holds it.';
        row.appendChild(chip);
      } else {
        row.appendChild(el('span', 'ps-code-chip-none', 'the machine named no commit for this line'));
      }
      body.appendChild(row);
    }
    if (lines.length > shown) {
      const cut = el('div', 'ps-code-cut', 'showing the blame of the first ' + shown + ' of ' + lines.length + ' lines');
      cut.title = 'The machine named a map for more lines than this page draws.';
      body.appendChild(cut);
    }
    note(E('code-viewer-note'), 'the blame map the machine holds for ' + view.path +
      (answer.truncated === true ? ' · the machine cut this map: more lines are covered than are shown' : '') +
      asOfTail(answer), '',
      'A mark per line, exactly as the door named it. The words of a line come from the file door, which this page read separately.');
  }

  // The anchor: the line named on the page, in the address, and lit where it is
  // drawn. A body taller than its window is scrolled to it, measured from the
  // rows on the screen rather than from a row height this page would guess.
  function anchorLine(line) {
    const wanted = Number(line);
    if (!(wanted > 0)) return;
    view.line = wanted;
    paintFileAddress();
    const body = E('code-viewer-body');
    if (!body) return;
    for (const other of body.querySelectorAll('.ps-code-here')) other.classList.remove('ps-code-here');
    const row = body.querySelector('#L' + wanted);
    if (!row) {
      note(E('code-viewer-note'), 'the line anchored here is not one this page drew: ' + view.path + '#L' + wanted, 'unknown',
        'An anchor this page cannot find is stated as an anchor it cannot find, not moved to the line nearest it.');
      return;
    }
    row.classList.add('ps-code-here');
    if (body.scrollHeight > body.clientHeight) {
      const top = row.offsetTop - body.offsetTop;
      const most = body.scrollHeight - body.clientHeight;
      body.scrollTop = Math.max(0, Math.min(top, most));
    }
    watchOutline();
  }

  function copyRef(words) {
    const text = String(words);
    let taken = false;
    try {
      if (navigator.clipboard) {
        navigator.clipboard.writeText(text);
        taken = true;
      }
    } catch (noClipboard) {
    }
    if (!taken) {
      try {
        const scratch = document.createElement('textarea');
        scratch.value = text;
        scratch.style.position = 'fixed';
        scratch.style.opacity = '0';
        document.body.appendChild(scratch);
        scratch.select();
        document.execCommand('copy');
        scratch.remove();
        taken = true;
      } catch (noCopy) {
      }
    }
    if (!taken) {
      ui.toastPush('this browser would not take the words onto its clipboard: the reference stands on the page as ' + text, 'warn');
    }
  }

  // ---------- the editor mode: the IDE in the viewer column (spec §5.9) ----------
  // The reading of the file stays §5.3's; editing is a mode of the same column,
  // entered by the hand that says edit. The buffer carries {path, baseOid, text}
  // and nothing leaves this browser until a save asks the machine to commit it.
  // The vendored editor is one ESM module; this page's scripts are classic, and
  // classic scripts run before deferred module scripts, so a static module tag for
  // the bundle would land its exports after this file had already wired the room.
  // The bundle is therefore taken by dynamic import, at the press that needs it,
  // once per page, and kept here.
  const kEditorDoor = '/project/api/commit';
  const kEditorBundle = './vendor/cm6/cm-bundle.js';
  const kFormatBundle = './vendor/beautify/beautify.js';
  const kEditorMax = 2 * 1024 * 1024; // spec §5.9: a bigger buffer stays read-only
  const editorMode = {
    cm: null,
    cmWhy: '',
    format: null,
    formatWhy: '',
    view: null,
    path: '',
    baseOid: '',
    baseText: '',
    language: '',
    beautifier: '',
  };

  // The language the file's own extension names, as the vendored pack spells it.
  // An extension the pack holds nothing for stays plain text, said honestly in
  // the status row rather than guessed at.
  const kEditorLangs = [
    ['.c', 'cpp'], ['.h', 'cpp'], ['.cpp', 'cpp'],
    ['.js', 'javascript'], ['.mjs', 'javascript'],
    ['.py', 'python'], ['.rs', 'rust'], ['.go', 'go'], ['.java', 'java'],
    ['.html', 'html'], ['.htm', 'html'], ['.xml', 'xml'],
    ['.css', 'css'], ['.json', 'json'], ['.md', 'markdown'],
    ['.sh', 'shell'], ['.bash', 'shell'],
  ];
  // The vendored beautifier the same extension has words for. A plain re-format
  // of the other languages is out of scope, so no hand is drawn for them.
  const kEditorFormats = [
    ['js', 'js_beautify'], ['json', 'js_beautify'],
    ['css', 'css_beautify'], ['html', 'html_beautify'], ['xml', 'html_beautify'],
  ];

  function editorLangNamed(path) {
    const wanted = String(path || '').toLowerCase();
    for (const entry of kEditorLangs) {
      if (wanted.length > entry[0].length && wanted.endsWith(entry[0])) return entry[1];
    }
    return '';
  }

  function editorFormatNamed(language) {
    for (const entry of kEditorFormats) {
      if (entry[0] === language) return entry[1];
    }
    return '';
  }

  // The module the editor needs, taken once. A module that is not where this
  // address says is a known absence: the room names it and keeps standing.
  async function editorAskBundle() {
    if (editorMode.cm || editorMode.cmWhy) return !!editorMode.cm;
    try {
      const made = await import(kEditorBundle);
      editorMode.cm = made && made.CM ? made.CM : null;
      if (!editorMode.cm) editorMode.cmWhy = 'the bundle at ' + kEditorBundle + ' named no CM to take';
    } catch (noBundle) {
      editorMode.cmWhy = 'the vendored editor at ' + kEditorBundle + ' did not load here: ' +
        String(noBundle && noBundle.message ? noBundle.message : 'the browser refused the address');
    }
    return !!editorMode.cm;
  }

  async function editorAskFormat() {
    if (editorMode.format || editorMode.formatWhy) return !!editorMode.format;
    try {
      editorMode.format = await import(kFormatBundle);
    } catch (noBundle) {
      editorMode.formatWhy = 'the vendored beautifier at ' + kFormatBundle + ' did not load here: ' +
        String(noBundle && noBundle.message ? noBundle.message : 'the browser refused the address');
    }
    return !!editorMode.format;
  }

  // The pane the editor stands in, made once beside the reading rather than
  // copied: the reading's own hosts stay the reading's own hosts.
  function editorPane() {
    const made = E('ps-editor');
    if (made) return made;
    const body = E('code-viewer-body');
    if (!body || !body.parentNode) return null;
    const pane = el('div', 'ps-editor-pane');
    pane.id = 'ps-editor';
    pane.title = 'The editor pane: one buffer, the words this page read at the ref its head names. Nothing is asked of the machine until a save.';
    body.parentNode.insertBefore(pane, body.nextSibling);
    const status = el('div', 'ps-editor-status');
    status.id = 'ps-editor-status';
    status.title = 'What the buffer holds: the words the machine sent, and whether this page has typed into them.';
    body.parentNode.insertBefore(status, pane.nextSibling);
    return pane;
  }

  // The row beside the pane: the buffer's own truth, the hand that commits it,
  // the one line the commit is named by, and the hand that reformats only for
  // the languages the vendored beautifier has words for.
  function paintEditorStatus() {
    const status = E('ps-editor-status');
    if (!status) return null;
    status.textContent = '';
    const said = el('div', 'ps-note');
    const changed = editorMode.view &&
      editorMode.view.state.doc.toString() !== editorMode.baseText;
    said.textContent = (editorMode.language
      ? 'the machine read these words as ' + editorMode.language
      : 'no language the vendored pack holds is named by this extension: these words stay plain text') +
      (editorMode.baseOid ? ' · held as ' + editorMode.baseOid : ' · the machine named no name for these bytes') +
      (changed ? ' · this page has typed into them' : ' · the words still stand as the machine sent them');
    said.title = 'The buffer belongs to this browser. The base name is the bytes the file door sent; a save is what asks the machine for a commit.';
    status.appendChild(said);
    const row = el('div', 'ps-field-row');
    const label = el('label', null, 'commit message');
    label.htmlFor = 'ps-editor-message';
    label.title = 'The one line the commit is named by. Leave it as it stands to keep the words this page would write.';
    row.appendChild(label);
    const field = el('input');
    field.id = 'ps-editor-message';
    field.type = 'text';
    field.value = 'Update ' + view.path;
    field.title = 'A commit without words is not a commit: the machine asks for this line when the save is sent. Blank keeps the words this page would write.';
    row.appendChild(field);
    status.appendChild(row);
    const save = el('button', 'ps-code-hand', 'save');
    save.id = 'ps-editor-save';
    save.title = 'Commit these words: the machine is asked to put them on a branch with the line above as their name. Press again takes the reading back.';
    status.appendChild(save);
    ui.onPress(save, function () {
      void saveEditor();
    });
    if (editorMode.beautifier) {
      const hand = el('button', 'ps-code-hand', 'beautify');
      hand.id = 'ps-editor-beautify';
      hand.title = 'Put the buffer through the vendored beautifier for this kind of file. The words stay in this browser; a save is still what commits them.';
      status.appendChild(hand);
      ui.onPress(hand, function () {
        void formatEditor();
      });
    }
    return status;
  }

  // A line added beside what already stands in a row: the row's own hands stay
  // where they are, so a refusal can be read against the buffer it refused.
  function addNoteLine(host, words, kind, tip) {
    const line = el('div', 'ps-note' + (kind ? ' ' + kind : ''));
    line.textContent = words;
    if (tip) line.title = tip;
    host.appendChild(line);
  }

  // The buffer's own words, as they stand now.
  function editorWords() {
    if (!editorMode.view) return '';
    return String(editorMode.view.state.doc.toString());
  }

  function editorHeld() {
    return !!editorMode.view && editorMode.path === view.path;
  }

  // A ref that names a branch is what a save commits onto; a ref that names a
  // commit is a reading of one moment, and the machine commits at its own head.
  function commitBranchNamed() {
    const ref = String(view.ref || '');
    if (!ref) return '';
    if (/^[0-9a-f]{40}$/.test(ref) || /^[0-9a-f]{64}$/.test(ref)) return '';
    return ref;
  }

  // Mounting the editor. A press of a hand already in use takes the numbered
  // reading back through stepCodeMode, so the pane is left as it was.
  async function renderEdit(host, read) {
    const pane = host || editorPane();
    if (!pane) return;
    if (!read || read.binary) {
      quiet(pane, 'this file came back with no words: the machine sent bytes, not text',
        'A binary file has no words to type into: the room opened no editor for it.');
      return;
    }
    if (!await editorAskBundle()) {
      quiet(pane, 'no editor stands here',
        'The page says which address it tried: ' + editorMode.cmWhy);
      note(E('code-viewer-note'), editorMode.cmWhy, 'unknown',
        'The reading of the file above stands as it was; the editor is what could not be mounted.');
      return;
    }
    const text = typeof read.text === 'string' ? read.text : String(read.text || '');
    if (text.length > kEditorMax) {
      quiet(pane, 'this file is past the two megabytes this page will hold as a buffer',
        'The reading above holds the words as the machine sent them; a buffer this page could not type in is not opened. The reason is stated rather than left to a blank pane.');
      return;
    }
    const language = editorLangNamed(view.path);
    const extensions = [];
    if (language) {
      try {
        extensions.push(language === 'shell'
          ? new editorMode.cm.language.StreamLanguage(editorMode.cm.legacy.shell)
          : editorMode.cm[language]());
      } catch (noLang) {
        // A language the pack does not carry is the same truth as no map entry.
      }
    }
    // Line numbers, the default keymap, and the search keymap: nothing else.
    extensions.push(editorMode.cm.view.keymap.of(editorMode.cm.commands.defaultKeymap));
    extensions.push(editorMode.cm.view.keymap.of(editorMode.cm.search.searchKeymap));
    extensions.push(...editorMode.cm.view.lineNumbers());
    // The problems' marks ride a compartment of their own, made once from the
    // bundle's Decoration: paintRunMarks reconfigures it, so a build never
    // rebuilds the editor and an edit never loses the rows.
    if (!runMarks.box) {
      try {
        runMarks.mark = editorMode.cm.view.Decoration.mark;
        runMarks.set = editorMode.cm.state.RangeSet.of;
        // The compartment holds an extension, and an empty RangeSet is not one:
        // the empty stands as the empty array, which is.
        runMarks.empty = [];
        runMarks.box = new editorMode.cm.state.Compartment();
      } catch (noDeco) {
        runMarks.box = null;
      }
    }
    if (runMarks.box) extensions.push(runMarks.box.of(runMarks.empty));
    // The gutter marks' truth starts from this reading before the view is
    // built — the mounted drawing reads live from here, so the mount only
    // stands the systems; the reads come after.
    resetGutterMarks(text, read.oid ? String(read.oid) : '', read && read.answer ? read.answer : null);
    for (const ext of buildGutterExtensions()) extensions.push(ext);
    if (editorMode.view) {
      editorMode.view.destroy();
      if (editorMode.view.dom) editorMode.view.dom.remove();
    }
    let made = null;
    try {
      made = new editorMode.cm.view.EditorView({ parent: pane, doc: text, extensions: extensions });
    } catch (noEditor) {
      quiet(pane, 'the editor could not be mounted in this browser',
        'The page says why: ' + String(noEditor && noEditor.message ? noEditor.message : noEditor));
      return;
    }
    editorMode.view = made;
    editorMode.path = view.path;
    editorMode.baseOid = read.oid ? String(read.oid) : '';
    editorMode.baseText = text;
    editorMode.language = language;
    editorMode.beautifier = editorFormatNamed(language);
    paintEditorStatus();
    // The marks' first live reading: the diff module, the blame map when a
    // commit stands, and the buffer as it stands now.
    void markGutterLive();
    // The marks are the words this browser holds, and a buffer typed into is
    // not the file the last build read: they stand only where the two agree.
    if (editorWordsUnchanged()) paintRunMarks();
  }

  function editorWordsUnchanged() {
    return editorMode.view && String(editorMode.view.state.doc) === editorMode.baseText;
  }

  // Save = commit (the house rule): the buffer's words become a commit, and the
  // machine's own words answer what it did. Nothing is asked before this press.
  async function saveEditor() {
    if (!editorHeld()) {
      ui.toastPush('no editor is open for this file, so there are no words to commit', 'warn');
      return;
    }
    const content = editorWords();
    if (content === editorMode.baseText) {
      ui.toastPush('the buffer still holds the words as the machine sent them: nothing was typed, so nothing is asked', 'warn');
      return;
    }
    const field = E('ps-editor-message');
    const message = fieldText(field) || ('Update ' + view.path);
    const body = {
      project: state.project,
      message: message,
      files: [{ path: view.path, content: content }],
    };
    const branch = commitBranchNamed();
    if (branch) body.branch = branch;
    ui.log('committing the words this browser holds for ' + view.path +
      (branch ? ' onto ' + branch : ' at the machine\'s own head'), 'wait');
    // The commit is one ask with one answer: what the door said - including
    // when it said nothing - comes from this ask's own result.
    const committed = await ui.apiR(kEditorDoor, 'POST', body);
    const answer = committed.answer;
    if (!answer) {
      const why = committed.error || ui.askWhy(committed);
      ui.log('the machine did not answer about that commit: ' + why, 'err');
      ui.toastPush(oneLiner(why, 180), 'err');
      return;
    }
    const words = String(answer.words || 'the machine said nothing about what it did');
    ui.log((answer.did ? '' : 'the machine refused: ') + words, answer.did ? 'ok' : 'err');
    ui.toastPush(oneLiner(words, 180), answer.did ? 'ok' : 'err', 12);
    const status = E('ps-editor-status');
    if (!answer.did) {
      if (status) {
        addNoteLine(status, words, 'unknown',
          'The buffer keeps its words beside the reason, so the two can be read against each other.');
      }
      return;
    }
    // The commit moved what the machine holds of this file: nothing cached of it
    // is still what it is, so the reading is taken again and the pane is drawn
    // from the machine's newest words.
    delete readFiles[fileKey(view.path, view.ref)];
    if (answer.commit) editorMode.baseOid = String(answer.commit);
    editorMode.baseText = content;
    const fresh = await askFileAgain();
    if (!fresh) {
      if (status) addNoteLine(status, 'the commit landed; the file door gave no reading back, so the buffer keeps its own words', 'unknown',
        'The machine said it committed; this page says which part of its own reading it could not take again.');
      return;
    }
    const body2 = E('code-viewer-body');
    if (body2) renderBody(fresh);
    void renderEdit(E('ps-editor'), fresh);
  }

  // The file door again, with no kept answer: a read taken because the page just
  // wrote is about what the machine holds now, not about what a browser kept.
  async function askFileAgain() {
    let path = kFileDoor + '?project=' + encodeURIComponent(state.project);
    path += '&path=' + encodeURIComponent(view.path);
    path += '&ref=' + encodeURIComponent(view.ref);
    let answer = null;
    try {
      answer = await ui.api(path, 'GET', undefined, { cache: 'no-store' });
    } catch (failedRead) {
    }
    if (!answer) return null;
    const text = typeof answer.content === 'string' ? answer.content : null;
    const binary = answer.binary === true || text === null;
    const made = {
      answer: answer,
      at: Date.now(),
      oid: answer.oid ? String(answer.oid) : '',
      text: text,
      binary: binary,
    };
    readFiles[fileKey(view.path, view.ref)] = made;
    return made;
  }

  // The buffer through the vendored beautifier. Only for the languages the pack
  // has words for; the rest of them have no hand here, and say so.
  async function formatEditor() {
    if (!editorHeld()) {
      ui.toastPush('no editor is open for this file, so there is nothing to format', 'warn');
      return;
    }
    if (!editorMode.beautifier) {
      ui.toastPush('this kind of file has no vendored beautifier: a plain re-format of other languages is out of scope', 'warn');
      return;
    }
    if (!await editorAskFormat()) {
      const status = E('ps-editor-status');
      if (status) status.appendChild(note(el('div'), editorMode.formatWhy, 'unknown',
        'The address the page tried is named; the buffer keeps its words.'));
      return;
    }
    const before = editorWords();
    let after = before;
    try {
      after = String(editorMode.format[editorMode.beautifier](before));
    } catch (badFormat) {
      ui.toastPush('the beautifier refused these words: ' +
        oneLiner(String(badFormat && badFormat.message ? badFormat.message : badFormat), 160), 'err');
      return;
    }
    if (after === before) {
      ui.toastPush('the beautifier answered with the words as they stand: there was nothing to change', 'ok');
      return;
    }
    editorMode.view.dispatch({ insert: after, delete: [0, before.length] });
    const status = E('ps-editor-status');
    if (status) {
      status.appendChild(note(el('div'), 'the buffer now holds the words the beautifier answered (' +
        editorMode.beautifier + ')', '',
        'A format is a change to the buffer, not a write: a save is still what asks the machine for a commit.'));
    }
  }

  // The keyboard that walks the files column is not a keyboard that types into
  // the buffer: while the pane holds the browser's focus, a press of a row is
  // words going into the file, not the room moving.
  function editorFocusHeld() {
    if (!editorMode.view) return false;
    try {
      const active = document.activeElement;
      const pane = E('ps-editor');
      return !!active && !!pane && (active === pane || pane.contains(active));
    } catch (noFocus) {
      return false;
    }
  }

  // ---------- the gutter marks: pending vs previous (spec §5.9) ----------
  // The marks beside the buffer keep two truths. The pending one is the
  // line diff of the words the file door sent at the base name vs what
  // this browser is typing — taken here with the page's own copy of the
  // server's diff: webui/ps-diff.js is a classic script answering with one
  // global PsDiff, so project.html carries it with its own <script> tag ahead
  // of this file and the mount point only takes that global. added lines wear
  // a green bar
  // and changed ones an amber bar in a column of their own, left of the
  // line numbers; a run of pure deletion wears a soft band on the line
  // that stands where it left. A press of a mark states its truth in a
  // toast — "pending vs <oid>". The previous one: the file door always
  // stamps the commit the reading stands on, and the blame door at that
  // ref names per line which commit last wrote it; lines named as that
  // commit's wear a dim blue bar reading "in this commit". Past 20,000
  // lines there are no marks at all, and the state row says so once. The
  // reads are rebated — 300ms between them — but the drawing is never
  // rebated, so a mark always stands on the line its index names now.
  const kDiffScript = '/ps-diff.js';
  const kMarkMaxLines = 20000;
  const kMarkBeatMs = 300;
  const gutterState = {
    diff: null,
    diffWhy: '',
    extensionsWhy: '',
    baseText: '',
    baseOid: '',
    baseLines: 0,
    commit: '',
    blameLines: null,
    blameTruncated: false,
    blameWhy: '',
    indices: null,
    kindAt: null,
    bandDeco: null,
    markClass: null,
    markers: null,
    skipped: false,
    skippedSaid: false,
    noteEl: null,
    beat: null,
    // Which buffer these reads belong to. A new buffer is a new truth and
    // bumps this; an answer that lands after one belongs to the buffer that
    // stood before it, and is dropped rather than painted here.
    gen: 0,
  };

  // The diff module, taken once per page. project.html loads webui/ps-diff.js
  // with its own classic <script> tag before this file runs, so there is
  // nothing to inject and nothing to wait on here: a node of this sort cannot
  // stand beside the document's own <html>, and the module import that carries
  // the editor cannot bring a global-scope classic in either — which is why
  // the tag stands in the page rather than in this file. What is taken here is
  // the global that tag left, once. Without it the marks say so once, in the
  // state row, and the rest of the room keeps standing.
  function takeGutterDiff() {
    if (gutterState.diff || gutterState.diffWhy) return !!gutterState.diff;
    const made = typeof globalThis === 'object' ? globalThis.PsDiff : null;
    if (made && typeof made.gutterMarks === 'function' &&
      typeof made.diffLines === 'function' && typeof made.splitLines === 'function') {
      gutterState.diff = made;
    } else {
      gutterState.diffWhy = 'the page\'s own tag at ' + kDiffScript +
        ' brought no PsDiff to take';
    }
    return !!gutterState.diff;
  }

  // An oid stated short, the way the rest of the room states names.
  function shortOid(oid) {
    const text = String(oid || '');
    return text ? text.slice(0, 8) : 'no name';
  }

  // A blame line is a bare commit-oid; the shape the blame room already
  // tolerates ({commitOid}) is tolerated here too.
  function blameLineOid(line) {
    if (typeof line === 'string') return line;
    if (line && line.commitOid) return String(line.commitOid);
    return '';
  }

  // The door's own names for one commit: equal strings, or a shared
  // prefix long enough to be a stated short name.
  function sameCommitOid(left, right) {
    if (!left || !right) return false;
    if (left === right) return true;
    return (left.length >= 8 && right.indexOf(left) === 0) ||
      (right.length >= 8 && left.indexOf(right) === 0);
  }

  // The three GutterMarkers: one instance per kind, built from the
  // bundle's own GutterMarker so the gutter's element reuse compares them
  // and draws once. The words each bar carries are its label and its
  // toast alike.
  function gutterMarkerFor(kind, label) {
    if (!gutterState.markClass) {
      const baseMark = editorMode.cm.view.GutterMarker;
      gutterState.markClass = class extends baseMark {
        constructor(kind, label) {
          super();
          this.kind = kind;
          this.label = label;
        }

        eq(other) {
          // The kind decides the bar and the label states its truth, so both
          // belong to the mark: one saying other words than these is not this
          // mark, and the gutter redraws rather than keeping the old words.
          return !!other && other.kind === this.kind && other.label === this.label;
        }

        toDOM(view) {
          const node = document.createElement('span');
          node.className = 'ps-gutter-' + this.kind;
          if (this.label) node.title = this.label;
          return node;
        }
      };
    }
    const held = gutterState.markers ? gutterState.markers[kind] : null;
    // The words a bar carries name the base and the commit it is pending
    // against, and those names move between readings. A held mark saying other
    // words than these is not this mark, so it is made again and the eq above
    // lets the difference through.
    if (held && held.label === label) return held;
    const made = new gutterState.markClass(kind, label);
    if (gutterState.markers) gutterState.markers[kind] = made;
    return made;
  }

  // The bars, drawn where the current document places them. This runs on
  // every view update: the indices may be a rebate old, but the places
  // are always the live document's, so a bar never drifts off its line.
  function buildGutterRanges(view) {
    const emptyRanges = editorMode.cm.state.RangeSet.empty;
    const kindAt = {};
    gutterState.kindAt = kindAt;
    if (!gutterState.indices) return emptyRanges;
    const doc = view.state.doc;
    const placed = [];
    const pushAll = function (indices, kind) {
      for (const at of indices) placed.push({ at: Number(at), kind: kind });
    };
    pushAll(gutterState.indices.added, 'added');
    pushAll(gutterState.indices.modified, 'modified');
    pushAll(gutterState.indices.prev, 'prev');
    if (!placed.length) return emptyRanges;
    placed.sort(function (left, right) {
      return left.at - right.at;
    });
    const builderClass = editorMode.cm.state.RangeSetBuilder;
    const builder = new builderClass();
    for (const one of placed) {
      const line = one.at + 1;
      if (line < 1 || line > doc.lines) continue;
      const from = doc.line(line).from;
      if (kindAt[from]) continue;
      kindAt[from] = one.kind;
      builder.add(from, from, gutterMarkerFor(one.kind,
        one.kind === 'prev'
          ? 'in this commit · ' + shortOid(gutterState.commit)
          : 'pending vs ' + shortOid(editorMode.baseOid) +
          ' · ' + (one.kind === 'added' ? 'added' : 'changed') + ' line'));
    }
    return builder.finish();
  }

  // The bands where words left without a replacement. The indices name
  // the line that stands at the leaving; the place is taken from the live
  // document, as the bars' is.
  function buildGutterBands(view) {
    const deco = gutterState.bandDeco;
    const none = editorMode.cm.view.Decoration.none;
    if (!deco || !gutterState.indices) return none;
    const doc = view.state.doc;
    const made = [];
    for (const at of gutterState.indices.deletedAt) {
      const line = Number(at) + 1;
      if (line < 1 || line > doc.lines) continue;
      made.push(deco.range(doc.line(line).from));
    }
    if (!made.length) return none;
    return editorMode.cm.state.RangeSet.of(made);
  }

  // Mounting the marks: the bands ride a ViewPlugin that only watches
  // whether the words moved — when they do, the bands take the live
  // indices and the reading gets its rebated turn — and the bars ride
  // the bundle's own gutter factory, whose press answers in a toast.
  // lineNumbers already carries the gutters plugin; the bundle dedupes a
  // plugin used twice, so a second gutter shares that one view.
  function buildGutterExtensions() {
    try {
      const cm = editorMode.cm;
      gutterState.bandDeco = cm.view.Decoration.line({ class: 'ps-gutter-deleted-line' });
      const bandClass = class {
        constructor(view) {
          this.decorations = buildGutterBands(view);
        }

        update(viewUpdate) {
          this.decorations = buildGutterBands(viewUpdate.view);
          if (viewUpdate.docChanged) scheduleGutterBeat();
        }
      };
      const made = [cm.view.ViewPlugin.fromClass(bandClass, {
        decorations: (band) => band.decorations,
      })];
      made.push.apply(made, cm.view.gutter({
        class: 'ps-gutter-marks',
        markers: (view) => buildGutterRanges(view),
        domEventHandlers: {
          click: (view, lineBlock, event) => {
            const kind = gutterState.kindAt ? gutterState.kindAt[lineBlock.from] : null;
            if (!kind) return false;
            ui.toastPush(gutterMarkToast(kind), 'ok');
            return true;
          },
        },
      }));
      return made;
    } catch (noGutter) {
      gutterState.extensionsWhy = 'the gutter marks could not be mounted in this browser: ' +
        String(noGutter && noGutter.message ? noGutter.message : noGutter);
      return [];
    }
  }

  function gutterMarkToast(kind) {
    if (kind === 'prev') return 'in this commit · ' + shortOid(gutterState.commit);
    return 'pending vs ' + shortOid(editorMode.baseOid) + ' · ' +
      (kind === 'added' ? 'added line' : 'changed line');
  }

  function scheduleGutterBeat() {
    if (gutterState.beat !== null) return;
    gutterState.beat = setTimeout(runGutterBeat, kMarkBeatMs);
  }

  function stopGutterBeat() {
    if (gutterState.beat === null) return;
    clearTimeout(gutterState.beat);
    gutterState.beat = null;
  }

  function runGutterBeat() {
    gutterState.beat = null;
    recomputeGutterMarks();
  }

  // Past the bound there are no marks at all, and the state row says so once.
  // Stated apart because it is read twice: before the blame door is asked at
  // all — a giant is never marked, so its blame map is never asked for either
  // — and again against the buffer as it stands on every recompute.
  function gutterPastBound(doc) {
    if (!doc || doc.lines <= kMarkMaxLines) return false;
    gutterState.skipped = true;
    gutterState.indices = null;
    if (!gutterState.skippedSaid) {
      gutterState.skippedSaid = true;
      paintGutterStatus('no gutter marks: this buffer is past ' + kMarkMaxLines +
        ' lines, and marking a giant is not this page\'s trade', 'unknown');
    }
    return true;
  }

  // A new buffer is a new truth: the state starts from this reading,
  // before the view is built, because the mounted drawing reads live.
  function resetGutterMarks(text, oid, answer) {
    stopGutterBeat();
    gutterState.baseText = String(text || '');
    gutterState.baseOid = String(oid || '');
    gutterState.extensionsWhy = '';
    const refWords = String(view.ref || '');
    const refIsCommit = /^[0-9a-f]{40}$/.test(refWords) || /^[0-9a-f]{64}$/.test(refWords);
    const stamped = answer && answer.commit ? String(answer.commit) : '';
    gutterState.commit = stamped || (refIsCommit ? refWords : '');
    gutterState.baseLines = 0;
    gutterState.blameLines = null;
    gutterState.blameTruncated = false;
    gutterState.blameWhy = '';
    gutterState.indices = { added: [], modified: [], deletedAt: [], prev: [] };
    gutterState.kindAt = null;
    gutterState.markers = {};
    gutterState.skipped = false;
    gutterState.skippedSaid = false;
    gutterState.noteEl = null;
    // A new buffer is a new answer: anything still travelling from the buffer
    // before this one is stale by the time it lands, and the generation the
    // reads below carry says so.
    gutterState.gen += 1;
  }

  // The first live reading: the diff module, the blame map at the ref the
  // head names when a commit stands, and then the buffer as it stands now.
  async function markGutterLive() {
    const gen = gutterState.gen;
    takeGutterDiff();
    if (gutterPastBound(editorMode.view ? editorMode.view.state.doc : null)) return;
    if (gutterState.commit) {
      const askedPath = view.path;
      const blameRead = await codeAsk(kBlameDoor, [['path', view.path], ['ref', view.ref]]);
      const answer = blameRead.answer;
      // The door answers when it answers, and by then this column may stand on
      // another file: a new buffer bumps the generation, and a blame map of
      // that other file is not this buffer's previous marks. Such an answer is
      // dropped whole rather than painted here.
      if (gen !== gutterState.gen || view.path !== askedPath) return;
      if (answer && Array.isArray(answer.lines)) {
        gutterState.blameLines = answer.lines;
        gutterState.blameTruncated = answer.truncated === true;
      } else {
        gutterState.blameWhy = answer
          ? 'the machine answered this door without naming any lines'
          : 'the blame door gave no answer: ' + (blameRead.error || ui.askWhy(blameRead));
      }
    }
    recomputeGutterMarks();
  }

  // The marks as they stand against the buffer now. The counts are the
  // diff's; the lines named as the shown commit's are taken through the
  // diff's unchanged-line pairing, so a previous mark stands where its
  // unchanged line stands now, not where it stood in the base.
  function recomputeGutterMarks() {
    const made = editorMode.view;
    if (!made) return;
    if (gutterState.extensionsWhy) {
      paintGutterStatus(gutterState.extensionsWhy, 'unknown');
      return;
    }
    const doc = made.state.doc;
    if (gutterPastBound(doc)) return;
    if (!gutterState.diff) {
      if (gutterState.diffWhy) paintGutterStatus('no gutter marks: ' + gutterState.diffWhy, 'unknown');
      return;
    }
    const current = doc.toString();
    gutterState.indices = makeGutterIndices(doc, current);
    paintGutterStatus(gutterStatusWords(), '');
    // A rebated reading changes what the marks say without any words
    // moving: the view is asked for a quiet update so the drawing follows.
    try {
      made.update([]);
    } catch (noNudge) {
    }
  }

  function makeGutterIndices(doc, current) {
    const added = [];
    const modified = [];
    const deletedAt = [];
    const prev = [];
    const baseText = gutterState.baseText;
    if (!gutterState.baseLines) {
      gutterState.baseLines = baseText.length ? gutterState.diff.splitLines(baseText).length : 0;
    }
    const baseCount = Number(gutterState.baseLines);
    const newCount = Number(doc.lines);
    const deleted = [];
    if (current !== baseText) {
      const marksOf = gutterState.diff.gutterMarks(baseText, current);
      if (marksOf) {
        added = marksOf.added;
        modified = marksOf.modified;
        deleted = marksOf.deleted;
      }
    }
    const deletedSet = {};
    for (const at of deleted) deletedSet[Number(at)] = true;
    const changedSet = {};
    for (const at of added) changedSet[Number(at)] = true;
    for (const at of modified) changedSet[Number(at)] = true;
    // Unchanged lines pair in order: an edit script never reorders what
    // is common, and the three arrays above are everything it marked
    // otherwise.
    const baseUn = [];
    for (let at = 0; at < baseCount; ++at) {
      if (!deletedSet[at]) baseUn.push(at);
    }
    const newUn = [];
    for (let at = 0; at < newCount; ++at) {
      if (!changedSet[at]) newUn.push(at);
    }
    const pairTo = {};
    const pairs = baseUn.length < newUn.length ? baseUn.length : newUn.length;
    for (let at = 0; at < pairs; ++at) pairTo[baseUn[at]] = newUn[at];
    // The band stands on the line that stands where the run left: the
    // next common line's place, or the last line when what followed never
    // stood.
    const seenBand = {};
    const sortedDeleted = deleted.concat([]);
    sortedDeleted.sort();
    let walk = 0;
    for (const at of sortedDeleted) {
      const baseAt = Number(at);
      while (walk < baseUn.length && baseUn[walk] <= baseAt) ++walk;
      const anchor = walk < pairs ? pairTo[baseUn[walk]] : newCount - 1;
      if (anchor < 0 || seenBand[anchor]) continue;
      seenBand[anchor] = true;
      deletedAt.push(anchor);
    }
    // Previous: the base lines the blame names as the shown commit's,
    // standing here unchanged.
    if (gutterState.commit && gutterState.blameLines) {
      for (let at = 0; at < gutterState.blameLines.length && at < baseCount; ++at) {
        if (deletedSet[at]) continue;
        const oid = blameLineOid(gutterState.blameLines[at]);
        if (oid && sameCommitOid(oid, gutterState.commit) && pairTo[at] !== undefined) prev.push(pairTo[at]);
      }
    }
    return { added: added, modified: modified, deletedAt: deletedAt, prev: prev };
  }

  function gutterStatusWords() {
    const indices = gutterState.indices;
    if (!indices) return 'no gutter marks stand here';
    const a = indices.added.length;
    const m = indices.modified.length;
    const d = indices.deletedAt.length;
    const p = indices.prev.length;
    let words = a || m || d
      ? 'pending vs ' + shortOid(gutterState.baseOid) + ' · ' + a + ' added · ' + m +
      ' changed · ' + d + ' left with no replacement'
      : 'pending: none — the buffer still holds the words the machine sent';
    if (gutterState.commit && gutterState.blameLines) {
      words += p
        ? ' · in this commit: ' + p + ' line' + (p === 1 ? '' : 's')
        : ' · no line here matches what the shown commit wrote';
      if (gutterState.blameTruncated) {
        words += ' · the machine cut this blame map: more lines are covered than it named';
      }
    } else if (gutterState.commit) {
      words += ' · no previous marks: ' + (gutterState.blameWhy || 'the blame door has not answered yet');
    } else {
      words += ' · no previous marks: the reading names no commit to be previous against';
    }
    return 'gutter · ' + words;
  }

  // The state row carries the marks' own truth: one line, kept current as
  // the marks move, and negatives stated in the machine's own orange.
  function paintGutterStatus(words, kind) {
    const status = E('ps-editor-status');
    if (!status) return;
    let line = gutterState.noteEl;
    if (!line || !status.contains(line)) {
      line = el('div', 'ps-note' + (kind ? ' ' + kind : ''));
      status.appendChild(line);
      gutterState.noteEl = line;
    }
    line.textContent = words;
    line.title = 'The pending truth is a line diff of the buffer against the bytes the file door sent at the base name, taken in this browser with the port of the server\'s diff. The previous truth is the blame door\'s map at the commit the reading stands on, which that same door stamps. Pressing a bar states its truth in a toast; a bar is never asked about again.';
  }

  // ---------- the outline column ----------
  function outlineNothing(words, tip) {
    const host = E('code-outline');
    if (!host) return;
    host.textContent = '';
    quiet(host, words, tip || 'The outline belongs to the bytes the viewer holds. With none held, nothing here describes a file.');
  }

  async function askOutline(read) {
    const host = E('code-outline');
    if (!host) return;
    const noteHost = E('code-outline-note');
    // The door reads a file the way every code door does - by its name at a ref.
    // A blob's oid is still what this page caches the outline under: bytes do
    // not change their shapes, so the machine's own name for them is the key.
    if (!read || !read.path) {
      outlineNothing('the machine named no name for these bytes, so no outline can be asked for them',
        'The outline door is asked with the file\'s own path. A reading that carries none leaves nothing to ask with, and this page does not invent one.');
      return;
    }
    const oid = read.oid ? String(read.oid) : '';
    const held = oid ? readOutlines[oid] : null;
    let answer = held ? held.answer : null;
    if (!answer && oid && window.olsProjectCache) {
      // Tier D: an outline of these bytes was kept across visits under the
      // machine's own name for them - the bytes do not change their shapes.
      const keptWords = window.olsProjectCache.getDerived(oid, 'outline');
      if (keptWords) {
        try {
          answer = JSON.parse(keptWords);
        } catch (badKept) {
          answer = null;
        }
      }
    }
    if (!answer) {
      const outlineRead = await codeAsk(kOutlineDoor, [['path', read.path], ['ref', view.ref]]);
      answer = outlineRead.answer;
      if (!answer) {
        refusedAt(outlineRead, host, noteHost || host, kOutlineDoor);
        return;
      }
      if (oid) {
        readOutlines[oid] = { answer: answer, at: Date.now() };
        if (window.olsProjectCache) void window.olsProjectCache.putDerived(oid, 'outline', JSON.stringify(answer));
      }
    }
    const symbols = Array.isArray(answer.symbols) ? answer.symbols : [];
    if (!symbols.length) {
      quiet(host, 'the machine names no outline here',
        'The outline door answered for these bytes and named no symbols: the language it read is one its scanner holds no shapes for, or it read no language. Nothing here is a guess about what the file is made of.');
      note(noteHost || host, 'the machine named a language of ' + (answer.language ? String(answer.language) : 'none'), '',
        'The language line is the machine\'s own statement; this page names no outline of its own.');
      return;
    }
    host.textContent = '';
    if (noteHost) noteHost.textContent = '';
    let placed = 0;
    for (const symbol of symbols) {
      const name = symbol && symbol.name ? String(symbol.name) : '';
      const at = symbol && Number(symbol.line) > 0 ? Number(symbol.line) : 0;
      if (!name || !(at > 0)) continue;
      placed += 1;
      const row = el('button', 'ps-outline-row', name + ' · ' +
        (symbol.kind ? String(symbol.kind) : 'a kind the machine did not name') + ' · L' + at);
      row.dataset.line = String(at);
      row.title = 'Jump to line ' + at + ' and anchor it: the address then names this file at this line. The name, its kind, and the line are what the machine read from these bytes.';
      host.appendChild(row);
    }
    if (!placed) {
      quiet(host, 'the machine named symbols it could not place',
        'A symbol with no name or no line is not drawn: the machine\'s own reading stopped short of a place to point at.');
      return;
    }
    if (noteHost) {
      note(noteHost, 'outlined by the machine for ' + (answer.language ? String(answer.language) : 'no language named') + asOfTail(answer), '',
        'The outline is a machine-side reading of one immutable blob, asked by its name; the page adds no shapes of its own.');
    }
    watchOutline();
  }

  // Which symbol the visible line stands in: the nearest one at or above the
  // line at the top of the window. Throttled, because scrolling is continuous
  // and the highlight is not the thing a reader is looking for.
  function watchOutline() {
    const body = E('code-viewer-body');
    const outline = E('code-outline');
    if (!body || !outline) return;
    for (const other of outline.querySelectorAll('.ps-outline-here')) other.classList.remove('ps-outline-here');
    if (!view.path || view.binary) return;
    const first = body.querySelector('#L1');
    if (!first) return;
    const height = first.offsetHeight > 0 ? first.offsetHeight : 1;
    const line = Math.floor(Math.max(0, body.scrollTop) / height) + 1;
    let best = null;
    for (const row of outline.querySelectorAll('.ps-outline-row')) {
      const at = Number(row.dataset.line);
      if (at > 0 && at <= line && (!best || at > Number(best.dataset.line))) best = row;
    }
    if (!best) return;
    best.classList.add('ps-outline-here');
  }

  // What the machine stated about a project. Nothing in this room is typed by
  // hand; a fact the machine never stated names itself as such.
  function paintMeta(room, answer, slug, heldAt) {
    const host = room.listEl;
    host.textContent = '';
    const rows = [
      ['name', answer.name],
      ['description', answer.description],
      ['license', answer.license],
    ];
    for (const row of rows) {
      const line = el('div', 'ps-fact-row');
      line.appendChild(el('span', 'ps-fact-name', row[0]));
      const said = row[1] === undefined || row[1] === null || row[1] === '' || row[1] === false;
      line.appendChild(el('span', said ? 'ps-fact-quiet' : 'ps-fact-said',
        said ? 'the machine says nothing here' : String(row[1] === true ? 'yes' : row[1])));
      host.appendChild(line);
    }
    const planHost = E('ps-planned');
    if (planHost) {
      planHost.textContent = '';
      const planned = Array.isArray(answer.planned_features) ? answer.planned_features : [];
      let at = 0;
      for (const item of planned) {
        const text = item && item.text ? String(item.text) : '';
        if (!text) continue;
        at += 1;
        const row = el('label', 'ps-plan-row');
        row.htmlFor = 'ps-planned-' + at;
        const check = el('input', null);
        check.id = 'ps-planned-' + at;
        check.type = 'checkbox';
        check.title = 'Mark this feature as done, or leave it unmarked as not done. The marks are sent when this room is written to the machine.';
        row.appendChild(check);
        row.appendChild(el('span', 'ps-plan-text', text));
        planHost.appendChild(row);
      }
      if (!at) {
        const none = el('div', 'ps-quiet');
        none.textContent = 'the machine has recorded no planned features for this project';
        none.title = 'Rows are drawn only when the machine named them. A planned feature is a line of work the machine holds in its own record of the project.';
        planHost.appendChild(none);
      }
    }
    note(room.noteEl, 'stated by the machine for ' + slug + asOfTail(answer) + heldTail(heldAt), '',
      'What the machine holds about this project: its name, its description, the license its own file names.');
  }

  // A reading is labeled by the moment it was taken, twice over: the machine's
  // own statement of where it stood, and this page's own moment of hearing it.
  function asOfTail(answer) {
    return answer.as_of ? ' · stated as of ' + String(answer.as_of) : ' · the machine stated no moment';
  }

  function heldTail(heldAt) {
    if (!heldAt) return '';
    return ' · this page last heard it at ' + ui.clockMs(heldAt);
  }

  // ---------- the two record rooms: pulls and issues ----------
  // Both rooms are drawn from the record doors and worked by the doors beside
  // them: the list one door reads, the record another, and the working doors -
  // review, comment, merge, resolve, close, reopen, link - answer each in the
  // machine's own words. Nothing here weighs a record, counts one, or names a
  // state a door did not state: where an answer carries no field the room says
  // the machine stated nothing, and a refusal is repeated as it came.
  //
  // Which filter stands, which record the room is reading, and what has been
  // typed into a box are this page's state, kept here; the machine is told none
  // of it until a press sends it.
  const kPullsDoor = '/project/api/pulls';
  const kBranchesDoor = '/project/api/branches';
  const kIssueRecords = '/project/api/issues';
  const kPullStates = ['open', 'merged', 'closed', 'all'];
  const kIssueStates = ['open', 'closed', 'all'];
  const kIssueSeverities = ['note', 'bug', 'want', 'blocker'];
  const kMergeStyles = ['merge', 'squash', 'fast-forward'];
  const kLinkKinds = ['pr', 'issue', 'commit'];

  const pullsRoom = {
    filter: 'open', slug: '', open: 0, detail: null, conflicts: null,
    ask: null, typed: {}, painted: null, paintedDetail: null, armed: false,
  };
  const issuesRoom = {
    filter: 'open', slug: '', file: '', open: '', detail: null,
    ask: null, typed: {}, painted: null, paintedDetail: null, armed: false,
  };

  // The words to press into the state filter this room stands on, in the door's
  // own words: these doors take one state word or none at all, and the page
  // offers no fourth.
  function pullsFilterWords() { return pullsRoom.filter; }

  function pullsFilterQuery() {
    return pullsRoom.filter === 'all' ? '' : '&state=' + encodeURIComponent(pullsRoom.filter);
  }

  function issuesFilterWords() {
    return issuesRoom.filter + (issuesRoom.file ? ':' + issuesRoom.file : '');
  }

  function issuesFilterQuery() {
    let words = issuesRoom.filter === 'all' ? '' : '&state=' + encodeURIComponent(issuesRoom.filter);
    if (issuesRoom.file) words += '&file=' + encodeURIComponent(issuesRoom.file);
    return words;
  }

  // A door ask that keeps the machine's whole answer whatever the status was.
  // The record doors answer a turned-down ask with {did:false,words} under the
  // status, and ui.apiR reads only an `error` member out of a non-ok answer -
  // the one place those doors speak through a status, the pull doors' 409s above
  // all, the words would be thrown away before this page could repeat them. So
  // the ask goes over the same fetch every other ask of this machine goes over -
  // the credential shim patched it before this page was even loaded, so the key
  // rides here exactly as it rides everywhere else and this page names none -
  // with the same body ceiling and the same cover, and keeps what came back.
  async function askDoor(path, method, body) {
    const read = { answer: null, status: 0, error: '' };
    const kind = String(method || 'GET').toUpperCase();
    if (kind !== 'GET' && kind !== 'HEAD') {
      const tooBig = ui.tooBigBody(body);
      if (tooBig) {
        read.status = tooBig.status;
        read.error = tooBig.error;
        return read;
      }
    }
    let givenUp = false;
    const held = new AbortController();
    const cover = setTimeout(function () {
      givenUp = true;
      held.abort();
    }, ui.apiCeilMs);
    try {
      const res = await fetch(path, {
        method: kind,
        body: body === undefined ? undefined : JSON.stringify(body),
        headers: body === undefined ? undefined : { 'content-type': 'application/json' },
        signal: held.signal,
      });
      read.status = res.status;
      try {
        read.answer = await res.json();
      } catch (notJson) {
        read.answer = null;
      }
      if (res.status < 200 || res.status > 299) {
        // A refusal is read the same way whatever the status was: the door's own
        // `error` words first, then the `words` the record doors answer a refused
        // ask with, and only a body that wrote nothing leaves this page to read
        // the status itself.
        const said = read.answer && read.answer.error ? String(read.answer.error)
          : read.answer && read.answer.words ? String(read.answer.words) : '';
        read.error = said || ui.askWhy(read);
      }
      return read;
    } catch (failedAsk) {
      read.answer = null;
      read.status = 0;
      read.error = givenUp
        ? 'the door did not answer in ' + Math.round(ui.apiCeilMs / 1000) +
        ' seconds, and this ask was let go'
        : 'the ask did not reach the machine: ' +
        (failedAsk && failedAsk.message ? String(failedAsk.message) : String(failedAsk));
      return read;
    } finally {
      clearTimeout(cover);
    }
  }

  // The sentence a door answered with: its own `words` when it wrote any, the
  // words of a body it refused with, and only beside a status that stated
  // nothing the page's own reading of the status.
  function doorWords(read) {
    if (read && read.answer && read.answer.words) return String(read.answer.words);
    if (read && read.error) return String(read.error);
    return ui.askWhy(read || { status: 0 });
  }

  // A refusal standing where it happened, in the machine's words and no others.
  function roomRefusal(host, read) {
    const line = el('div', 'ps-refusal', doorWords(read));
    line.title = 'The words this door answered with, sent as they came. What was pressed stays as it was pressed.';
    host.appendChild(line);
  }

  function roomHand(words, tip, act, data) {
    const hand = el('button', 'ps-code-hand', words, tip);
    hand.dataset.act = act;
    for (const name in data) hand.dataset[name] = String(data[name]);
    return hand;
  }

  function roomChip(name, said, tip, warn) {
    const chip = el('span', 'ps-chip' + (warn ? ' ps-chip-warn' : ''), null, tip);
    chip.appendChild(el('span', 'ps-chip-name', name));
    chip.appendChild(el('span', 'ps-chip-said', said));
    return chip;
  }

  function roomFact(host, name, said, tip) {
    const row = el('div', 'ps-fact-row', null, tip);
    row.appendChild(el('span', 'ps-fact-name', name));
    if (said === undefined || said === null || said === '') {
      row.appendChild(el('span', 'ps-fact-quiet', 'the machine stated nothing'));
    } else {
      row.appendChild(el('span', 'ps-fact-said', String(said)));
    }
    host.appendChild(row);
    return row;
  }

  // The state a record wears, painted as the state it names rather than as a
  // colour: the word is the machine's, and a state it does not state is said.
  function roomWord(state) {
    const said = String(state || '');
    const kind = said === 'open' || said === 'merged' || said === 'closed' ? said : 'unknown';
    return el('span', 'ps-state ps-state-' + kind, kind === 'unknown' ? 'a state the door did not name' : said);
  }

  function roomThread(host, rows, nothingWords, tip) {
    const rowsHeld = Array.isArray(rows) ? rows : [];
    if (!rowsHeld.length) {
      host.appendChild(el('div', 'ps-quiet', nothingWords, tip));
      return;
    }
    for (const one of rowsHeld) {
      const row = el('div', 'ps-thread-row');
      const who = one && one.who ? String(one.who) : 'no one named';
      row.appendChild(el('span', 'ps-thread-who', who));
      if (one && one.verdict) row.appendChild(el('span', 'ps-thread-verdict', String(one.verdict)));
      const when = whenWords(one ? one.at : 0);
      if (when) row.appendChild(el('span', 'ps-thread-when', when));
      const anchor = one && one.anchor && typeof one.anchor === 'object' ? one.anchor : null;
      if (anchor && anchor.path) {
        const at = String(anchor.path) + (Number(anchor.line) > 0 ? ':' + Number(anchor.line) : '');
        row.appendChild(el('span', 'ps-thread-anchor', at,
          'The place this comment was written against. It is the anchor the comment carries, not a claim that the line stands where it was - this door re-reads nothing.'));
      }
      const body = el('pre', 'ps-thread-body');
      body.textContent = one && one.body ? String(one.body) : '';
      row.appendChild(body);
      host.appendChild(row);
    }
  }

  // A field of a room's own asking: one label, one field, the tip saying what
  // goes out when the sheet is sent and what is left out when it stays blank.
  function roomField(host, label, name, kind, value, tip, placeholder) {
    if (!host) return null;
    const row = el('div', kind === 'textarea' ? 'ps-field-row ps-field-top' : 'ps-field-row');
    const tag = el('label', null, label);
    tag.htmlFor = name;
    row.appendChild(tag);
    let field;
    if (kind === 'textarea') {
      field = el('textarea', 'ps-room-text');
      field.rows = 4;
    } else {
      field = el('input');
      field.type = kind === 'number' ? 'number' : 'text';
      if (kind === 'number') {
        field.min = '1';
        field.step = '1';
      }
      if (placeholder !== undefined) field.placeholder = placeholder;
    }
    field.id = name;
    if (tip) field.title = tip;
    field.value = value === undefined || value === null ? '' : String(value);
    row.appendChild(field);
    host.appendChild(row);
    return field;
  }

  // One press at a time per room: a second press while a door is still answering
  // would ask the same thing twice, and the machine keeps both.
  function roomBusy(room, what) {
    if (!room.busy) return false;
    ui.toastPush('one ask of this room is still out: ' + what, 'warn');
    return true;
  }

  // Escape puts a record sheet away - the one key this page already answers to
  // for the sheet that adds a project. What was typed stays held by the room, so
  // opening the sheet again brings the words back beside the same fields.
  function closeRoomAsk() {
    for (const pair of [[pullsRoom, 'ps-pulls-ask'], [issuesRoom, 'ps-issues-ask']]) {
      if (!pair[0].ask) continue;
      pair[0].ask = null;
      const host = E(pair[1]);
      if (host) host.hidden = true;
      return;
    }
  }

  // What a working door answered: the machine's sentence, once in the log, once
  // as a toast, and once held by the room - where it stands under the record
  // until something else is pressed, so the words are still there after the toast
  // has gone by.
  function roomSaid(room, read, didWords, notWords) {
    const words = doorWords(read);
    const did = !!(read.answer && read.answer.did);
    if (did) evictRoomDoors(state.project);
    ui.log((did ? didWords : notWords) + ': ' + words, did ? 'ok' : 'err');
    ui.toastPush(oneLiner(words, 180), did ? 'ok' : 'err', 12);
    if (room) room.said = { words: words, did: did };
    return did;
  }

  // A record this page just worked has an answer the page has not heard, so the
  // reading held of the list is put away and both the list and the record are
  // asked for again.
  function roomRefresh(room, tab) {
    forgetHeld(tab + ':' + state.project);
    room.painted = null;
    const listed = roomNamed(tab);
    // The room behind a record this page just worked: the held copy from
    // before the write is gone above, and the ask beside it says so to the
    // browser's own cache as well - status-cache replays a GET inside its
    // stated freshness for any ask that does not say no-store.
    if (listed) void askRoom(listed, true);
  }

  // A mutation for the project this page stands on puts this project's list
  // readings out of the browser's own cache: the exact URL of another filter
  // variant may stand cached inside its window from before the write, and the
  // fresh ask beside the mutation carries the no-store word only for itself.
  // Only this project's record-room doors are named - other rooms' and other
  // projects' readings keep their windows.
  function evictRoomDoors(slug) {
    const cache = window.olsCache;
    if (!cache || !cache.dropPrefix) return;
    const here = encodeURIComponent(String(slug));
    for (const room of ROOMS) {
      if (room.tab === 'issues' || room.tab === 'pulls') {
        cache.dropPrefix(room.door + '?project=' + here);
      }
    }
  }

  // ---------- the pulls room ----------
  function armPullsRoom(room) {
    if (pullsRoom.armed) return;
    pullsRoom.armed = true;
    const hosts = [E('ps-pulls-bar'), E('ps-pulls-ask'), E('ps-pulls-detail'), room.listEl];
    for (const host of hosts) {
      if (!host) continue;
      host.addEventListener('click', function (event) {
        const node = event && event.target ? event.target : null;
        if (!node || !node.closest) return;
        const hit = node.closest('[data-act]');
        if (!hit || !host.contains(hit)) return;
        ui.flash(hit);
        void pressPulls(String(hit.dataset.act || ''), hit);
      });
      host.addEventListener('input', function (event) {
        const node = event && event.target ? event.target : null;
        if (!node || !node.id) return;
        pullsRoom.typed[node.id] = String(node.value || '');
      });
      host.addEventListener('change', function (event) {
        const node = event && event.target ? event.target : null;
        if (!node || !node.id) return;
        pullsRoom.typed[node.id] = String(node.value || '');
      });
    }
  }

  function paintPullsRoom(room, answer, slug, heldAt) {
    armPullsRoom(room);
    if (pullsRoom.slug !== slug) {
      pullsRoom.slug = slug;
      pullsRoom.open = 0;
      pullsRoom.detail = null;
      pullsRoom.conflicts = null;
      pullsRoom.ask = null;
      pullsRoom.typed = {};
      pullsRoom.painted = null;
      pullsRoom.paintedDetail = null;
      pullsRoom.said = null;
      const asking = E('ps-pulls-ask');
      if (asking) {
        asking.hidden = true;
        asking.textContent = '';
      }
    }
    if (pullsRoom.open > 0) {
      room.listEl.hidden = true;
      const asking = E('ps-pulls-ask');
      if (asking) asking.hidden = true;
      // The sheet stands over the list, and the list keeps its rows from
      // this reading: a pull the page just worked has left the filter the
      // list stands on, and it must be gone the moment the sheet is put
      // away. The detail is painted after, so its note stands beside the
      // open sheet.
      if (pullsRoom.painted !== answer) {
        pullsRoom.painted = answer;
        paintPullsBar(room, answer);
        paintPullsList(room, answer, slug, heldAt);
      }
      paintPullsDetail(room);
      return;
    }
    const detail = E('ps-pulls-detail');
    if (detail) detail.hidden = true;
    room.listEl.hidden = false;
    if (pullsRoom.painted === answer) return;
    pullsRoom.painted = answer;
    paintPullsBar(room, answer);
    paintPullsList(room, answer, slug, heldAt);
  }

  function paintPullsBar(room, answer) {
    const bar = E('ps-pulls-bar');
    if (!bar) return;
    bar.textContent = '';
    const filters = el('span', 'ps-modes', null,
      'Which pulls this room asks for. The pulls door takes open, merged, or closed - or the word left off entirely, which is every pull it keeps. The machine is not told a filter was chosen; the ask that follows carries it.');
    for (const word of kPullStates) {
      const hand = el('button', 'ps-mode' + (pullsRoom.filter === word ? ' ps-mode-on' : ''), word,
        'Ask for the pulls in this state.');
      hand.dataset.act = 'filter';
      hand.dataset.state = word;
      filters.appendChild(hand);
    }
    bar.appendChild(filters);
    const pills = el('span', 'ps-pills');
    const threshold = Number(answer.threshold);
    if (Number.isFinite(threshold)) {
      pills.appendChild(roomChip('threshold', String(threshold),
        'The number of approvals a pull of this project needs, as this project\'s own facts state them. The pulls door carries the number beside its rows; whether a merge is allowed is the merge door\'s own law, not this number.'));
    } else {
      const none = el('span', 'ps-fact-quiet-line', 'the machine stated no threshold');
      none.title = 'The list answer carried no threshold. No number is written here for one the door did not state.';
      pills.appendChild(none);
    }
    const rows = Array.isArray(answer.rows) ? answer.rows.length : -1;
    if (rows >= 0) {
      pills.appendChild(roomChip('rows', String(rows),
        'How many rows this one reading carried. It is a count of what the door handed this page, not a count of what the machine keeps.'));
    }
    bar.appendChild(pills);
    bar.appendChild(roomHand('open a pull',
      'Open a pull request between two branches of this project: the sheet asks which two, and for a title. Both tips are read by the machine when the sheet is sent, and what the record refuses about them comes back in its own words.',
      'ask'));
    bar.appendChild(roomHand('ask again',
      'Ask the pulls door again for what this room stands on, and draw the rows from what it answers this time.',
      'again'));
  }

  function paintPullsList(room, answer, slug, heldAt) {
    const list = room.listEl;
    list.textContent = '';
    const rows = Array.isArray(answer.rows) ? answer.rows : [];
    if (!rows.length) {
      quiet(list, 'the machine listed no pulls for ' + (pullsRoom.filter === 'all' ? 'any state' : 'state ' + pullsRoom.filter),
        'The door answered, and its answer carried no rows. A room with no rows is not a machine that stated nothing: the state asked is what bounded the reading.');
    }
    for (const one of rows) {
      const row = el('button', 'ps-rec ps-pull-row');
      row.dataset.act = 'read';
      row.dataset.n = String(one && one.number !== undefined ? one.number : '');
      row.title = 'Read pull ' + String(one && one.number !== undefined ? one.number : '?') +
        ': the record, the two branch tips as they stand at the answer, and the doors that work it.';
      row.appendChild(el('span', 'ps-rec-no', '#' + String(one && one.number !== undefined ? one.number : '?')));
      row.appendChild(el('span', 'ps-rec-name', one && one.title ? String(one.title) : 'a pull with no title'));
      const refs = (one && one.head ? String(one.head) : 'no head') + ' → ' +
        (one && one.base ? String(one.base) : 'no base');
      row.appendChild(el('span', 'ps-rec-refs', refs,
        'The head branch merges into the base branch. These are the names the record carries, not tips read now - the record reads the tips when it is opened.'));
      row.appendChild(roomWord(one ? one.state : ''));
      const approvals = one && one.approvals;
      const n = approvals ? Number(approvals.n) : NaN;
      const need = approvals ? Number(approvals.threshold) : NaN;
      if (Number.isFinite(n)) {
        row.appendChild(roomChip('approvals', String(n) + (Number.isFinite(need) ? '/' + String(need) : ''),
          'How many approvals the record carries' +
          (Number.isFinite(need) ? ', against the ' + String(need) + ' this project\'s facts ask for' : '') +
          '. Whether a merge is allowed is the merge door\'s own law, not this number.',
          Number.isFinite(need) && n < need));
      }
      const when = whenWords(one ? one.updated_at : 0);
      if (when) row.appendChild(el('span', 'ps-rec-when', when));
      list.appendChild(row);
    }
    const tails = [];
    if (answer.hasMore === true) {
      tails.push('the door stopped at the cap it reads by and says more stands beside these rows');
    }
    const unparseable = Array.isArray(answer.unparseable) ? answer.unparseable : [];
    if (unparseable.length) {
      tails.push('the machine named ' + unparseable.length + ' record' + (unparseable.length === 1 ? '' : 's') +
        ' it could not read: ' + unparseable.join(', '));
    }
    note(room.noteEl, 'listed by the machine for ' + slug + ' · asked for ' +
      (pullsRoom.filter === 'all' ? 'every state' : pullsRoom.filter) + heldTail(heldAt) +
      (tails.length ? ' · ' + tails.join(' · ') : ''), tails.length ? 'unknown' : '',
      'What the door stated about its own reading: the state it was asked for, and whether it stopped short.');
  }

  // The one record of one pull, with the two tips read now beside it: the answer
  // is the record plus head_now, base_now, and the machine's own word on whether
  // either moved since the pull opened.
  async function askPull(n, noStore) {
    if (!state.project) return null;
    const read = await askDoor(kPullsDoor + '/' + encodeURIComponent(n) +
      '?project=' + encodeURIComponent(state.project) + (noStore ? '&t=' + Date.now() : ''));
    return read;
  }

  async function openPull(n) {
    if (roomBusy(pullsRoom, 'reading a pull')) return;
    pullsRoom.busy = true;
    pullsRoom.open = n;
    pullsRoom.detail = null;
    pullsRoom.conflicts = null;
    pullsRoom.said = null;
    pullsRoom.typed = {};
    try {
      const read = await askPull(n);
      pullsRoom.detail = read;
    } finally {
      pullsRoom.busy = false;
    }
    if (state.tab === 'pulls' && pullsRoom.open === n) {
      const listed = roomNamed('pulls');
      // The room is drawn through its own door of entry: the rows go behind the
      // record rather than staying under it, and the beat repaints the record.
      if (listed) paintRoom(listed);
    }
  }

  // One record drawn, or not drawn again. The page is repainted on a beat, and a
  // record with a half-typed comment in it must not be rebuilt every beat: a
  // record is rebuilt only when the reading beside it is a different reading, or
  // when the page put new words beside it.
  function paintPullsDetail(room) {
    if (!room) return;
    const host = E('ps-pulls-detail');
    if (!host) return;
    const read = pullsRoom.detail;
    if (pullsRoom.paintedDetail && pullsRoom.paintedDetail[0] === read &&
      pullsRoom.paintedDetail[1] === pullsRoom.conflicts &&
      pullsRoom.paintedDetail[2] === pullsRoom.said && host.childNodes.length) return;
    pullsRoom.paintedDetail = [read, pullsRoom.conflicts, pullsRoom.said];
    host.hidden = false;
    host.textContent = '';
    const head = el('div', 'ps-detail-head');
    head.appendChild(roomHand('the list', 'Put this record away and draw the rows again.', 'back'));
    if (!read) {
      head.appendChild(el('span', 'ps-quiet', 'asking the machine for pull ' + pullsRoom.open + '…'));
      host.appendChild(head);
      return;
    }
    if (!read.answer) {
      head.appendChild(el('span', 'ps-quiet', 'the machine answered nothing at the record door'));
      host.appendChild(head);
      roomRefusal(host, read);
      return;
    }
    const answer = read.answer;
    const record = answer.pull && answer.pull.number !== undefined ? answer.pull : null;
    const number = record ? String(record.number) : String(pullsRoom.open);
    head.appendChild(el('span', 'ps-rec-no', '#' + number));
    head.appendChild(el('span', 'ps-rec-name', record && record.title ? String(record.title) : 'a pull with no title'));
    head.appendChild(roomWord(record ? record.state : ''));
    host.appendChild(head);

    const facts = el('div', 'ps-facts');
    const headNow = answer.head_now && typeof answer.head_now === 'object' ? answer.head_now : null;
    const baseNow = answer.base_now && typeof answer.base_now === 'object' ? answer.base_now : null;
    roomFact(facts, 'head',
      headNow ? (headNow.gone === true ? String(headNow.branch || '') + ' - the branch is gone from this repo'
        : String(headNow.branch || '') + ' at ' + shortOid(headNow.oid)) : undefined,
      'The branch the record names as its head, and the tip it stands at now. A branch taken away is named gone - that is not the same thing as a move.');
    roomFact(facts, 'base',
      baseNow ? (baseNow.gone === true ? String(baseNow.branch || '') + ' - the branch is gone from this repo'
        : String(baseNow.branch || '') + ' at ' + shortOid(baseNow.oid)) : undefined,
      'The branch the record lands in, and the tip it stands at now.');
    if (answer.moved_since_open === true) {
      roomFact(facts, 'since open', 'a tip moved since this pull was opened',
        'The machine compared the tips the record was opened with against the tips standing now, and says at least one of them moved. The merge door reads the tips standing now, not these.');
    } else if (answer.moved_since_open === false) {
      roomFact(facts, 'since open', 'both tips stand where the pull opened',
        'The machine compared the tips the record was opened with against the tips standing now and found no move.');
    } else {
      roomFact(facts, 'since open', '', 'The answer stated no word on whether the tips moved.');
    }
    if (record) {
      roomFact(facts, 'opened', whenWords(record.created_at), 'The moment the record says it was opened.');
      roomFact(facts, 'changed', whenWords(record.updated_at), 'The moment the record says it last changed.');
      const merge = record.merge && typeof record.merge === 'object' ? record.merge : null;
      if (merge) {
        roomFact(facts, 'merged', String(merge.style || 'a style the record did not name') + ' at ' +
          shortOid(merge.oid) + (merge.by ? ' by ' + String(merge.by) : '') +
          (whenWords(merge.at) ? ' · ' + whenWords(merge.at) : ''),
          'What the record states about the merge that closed it: the style, the commit, who merged, and when.');
      }
    }
    host.appendChild(facts);

    const bodyWords = record && record.body ? String(record.body) : '';
    const bodyPane = el('pre', 'ps-record-body');
    bodyPane.textContent = bodyWords || 'the record carries no body';
    if (!bodyWords) bodyPane.title = 'The machine stated no body for this pull. Nothing is written here for one it did not.';
    host.appendChild(bodyPane);

    // The merge door's hand: the style is asked for in words, and the press
    // confirms the style it is about to send before anything goes out.
    const hands = el('div', 'ps-detail-hands');
    const worn = record ? String(record.state || '') : '';
    if (worn === 'open') {
      const styleRow = el('div', 'ps-field-row');
      const label = el('label', null, 'merge style');
      label.htmlFor = 'ps-pull-style';
      styleRow.appendChild(label);
      const style = el('select');
      style.id = 'ps-pull-style';
      style.title = 'Which of the three merge shapes to ask for. The merge door takes merge, squash, or fast-forward, and answers a word it does not know with the words that name the three.';
      for (const word of kMergeStyles) {
        const choice = el('option', null, word);
        choice.value = word;
        choice.title = word === 'fast-forward'
          ? 'Fast-forward: the base moves to the head with no new commit, when it can.'
          : word === 'squash'
            ? 'Squash: the head\'s work lands on the base as one commit.'
            : 'Merge: a commit joining both sides.';
        style.appendChild(choice);
      }
      style.value = pullsRoom.typed['ps-pull-style'] || 'merge';
      styleRow.appendChild(style);
      hands.appendChild(styleRow);
      hands.appendChild(roomHand('merge',
        'Ask the machine to merge this pull into its base with the style chosen beside this hand. The machine moves the base branch and marks the record merged; if the two sides do not join it answers with the paths in conflict and the pull stays open.',
        'merge', { n: number }));
      hands.appendChild(roomHand('close',
        'Close this pull without merging it. The record keeps its folder and its number, and the machine says in its own words what it did.',
        'close', { n: number }));
    } else if (worn === 'closed') {
      hands.appendChild(roomHand('reopen',
        'Reopen this pull. The record goes back to open; a merged pull does not come back, and the machine says so in its own words.',
        'reopen', { n: number }));
    }
    if (hands.childNodes.length) host.appendChild(hands);

    const reviews = record && Array.isArray(record.reviews) ? record.reviews : [];
    const reviewsHost = el('div', 'ps-thread');
    reviewsHost.appendChild(el('div', 'ps-detail-part', 'reviews · ' + reviews.length +
      (record && Array.isArray(record.approvals) ? ' · standing approvals ' + record.approvals.length : '')));
    roomThread(reviewsHost, reviews, 'the record carries no reviews',
      'No review stands on this record. That is the record\'s own statement, not this page\'s guess about the work.');
    host.appendChild(reviewsHost);

    const comments = record && Array.isArray(record.comments) ? record.comments : [];
    const commentsHost = el('div', 'ps-thread');
    commentsHost.appendChild(el('div', 'ps-detail-part', 'comments · ' + comments.length));
    roomThread(commentsHost, comments, 'the record carries no comments',
      'No comment stands on this record.');
    host.appendChild(commentsHost);

    // The review and comment hands. `by` is sent only when a word was typed: the
    // door keeps its own word for this page - webui - when the field is left as
    // it stands.
    const work = el('div', 'ps-detail-work');
    work.appendChild(el('div', 'ps-detail-part', 'say something to the machine'));
    roomField(work, 'who signs it', 'ps-pull-by', 'text', pullsRoom.typed['ps-pull-by'] || '',
      'Who this review, comment, or merge is kept under. Blank sends no name, and each door keeps its own word for a page asking: webui for a review or a comment, studio for a merge.',
      'webui');
    roomField(work, 'words', 'ps-pull-body', 'textarea', pullsRoom.typed['ps-pull-body'] || '',
      'The words of the review or comment. They go as they are typed; a comment past the 4 KiB one is kept at is refused by the door, naming the size.');
    const sayHands = el('div', 'ps-detail-hands');
    sayHands.appendChild(roomHand('approve', 'Record an approval of this pull, with the words beside this hand as the review.',
      'approve', { n: number }));
    sayHands.appendChild(roomHand('request changes',
      'Record that this reviewer asks for changes. The door also takes that reviewer\'s standing approval away, in its own law.',
      'request-changes', { n: number }));
    sayHands.appendChild(roomHand('comment', 'Record a comment on this pull; it says nothing about approval.',
      'comment', { n: number }));
    work.appendChild(sayHands);
    host.appendChild(work);

    // What the merge door came back with last, if it came back with conflicts:
    // the paths it named, the three sides each one carries, and the place to
    // settle each one. The pull stays open through all of this.
    if (pullsRoom.conflicts) {
      host.appendChild(paintConflicts(pullsRoom.conflicts, number));
    }
    // What the machine last said about a press made in this room, standing under
    // the record: a refusal stays on the page until something else is pressed, so
    // its words are not gone by the time a person looks up.
    if (pullsRoom.said) {
      if (pullsRoom.said.did) {
        host.appendChild(el('div', 'ps-quiet', pullsRoom.said.words,
          'The machine\'s own sentence about the last thing this room pressed. It stands here until another record is opened.'));
      } else {
        roomRefusal(host, { answer: null, error: pullsRoom.said.words });
      }
    }
    const noteHost = room.noteEl;
    if (noteHost) {
      const tails = ['#' + number + ' read from ' + kPullsDoor + '/' + number];
      if (pullsRoom.conflicts) tails.push('the merge door answered with conflicts, and the pull stays open');
      note(noteHost, tails.join(' · '), pullsRoom.conflicts ? 'unknown' : '',
        'Where this room stands: the door the record came from, and whether conflicts are drawn here.');
    }
  }

  // The conflict answer, drawn as the door stated it: one row per path, the
  // three texts side by side where the answer carried them, and a settle box per
  // path that sends what is typed there - or takes the file away.
  function paintConflicts(answer, number) {
    const host = el('div', 'ps-conflicts');
    const rows = Array.isArray(answer.conflicts) ? answer.conflicts : [];
    host.appendChild(el('div', 'ps-detail-part', 'the merge door came back with ' + rows.length +
      ' path' + (rows.length === 1 ? '' : 's') + ' in conflict · merge base ' +
      shortOid(answer.merge_base)));
    if (answer.conflicts_truncated === true) {
      host.appendChild(el('div', 'ps-code-cut', 'the door cut the list of conflicts short; more stand than it named here'));
    }
    if (answer.texts_truncated === true) {
      host.appendChild(el('div', 'ps-code-cut',
        'the door ran out of room for the side texts: a pane naming no text is a text it never sent, not an empty file'));
    }
    if (answer.multiple_bases === true) {
      host.appendChild(el('div', 'ps-code-cut', 'the two sides have more than one merge base'));
    }
    const unused = Array.isArray(answer.unused) ? answer.unused : [];
    if (unused.length) {
      host.appendChild(el('div', 'ps-code-cut', 'the merge never needed these settled paths: ' + unused.join(', ')));
    }
    for (const one of rows) {
      const path = one && one.path ? String(one.path) : 'a path the door did not name';
      const block = el('div', 'ps-conflict');
      block.appendChild(el('div', 'ps-conflict-head', path + (one && one.kind ? ' · ' + String(one.kind) : '')));
      const panes = el('div', 'ps-conflict-sides');
      for (const side of [['base', one ? one.base_text : undefined, 'the merge base'],
      ['ours', one ? one.ours_text : undefined, 'the base branch as it stands'],
      ['theirs', one ? one.theirs_text : undefined, 'the head branch as it stands']]) {
        const pane = el('div', 'ps-conflict-pane');
        pane.appendChild(el('div', 'ps-conflict-side', side[0]));
        const text = typeof side[1] === 'string' ? side[1] : '';
        const pre = el('pre', 'ps-conflict-text');
        pre.textContent = text;
        if (!text) {
          pre.appendChild(el('span', 'ps-quiet', 'the machine stated no text for this side'));
        }
        pane.appendChild(pre);
        panes.appendChild(pane);
      }
      block.appendChild(panes);
      const id = 'ps-resolve-' + path.replace(/[^A-Za-z0-9]/g, '_');
      roomField(block, 'settle it', id, 'textarea', pullsRoom.typed[id] || '',
        'The words this file should wear. ' + (path === 'a path the door did not name'
          ? '' : 'They are sent as the content for ' + path + ' of pull ' + number + '.') +
        ' Nothing goes out until the hand beside this box is pressed.');
      const settle = el('div', 'ps-detail-hands');
      settle.appendChild(roomHand('settle this file',
        'Send what is in the box as the settled content for this path. The door keeps it beside the record, against the tips it was read at; the merge asked again reads them.',
        'resolve', { n: number, file: path, box: id }));
      settle.appendChild(roomHand('take the file away',
        'Send the resolution that this path is taken away. The door keeps that word instead of any content.',
        'resolve-delete', { n: number, file: path }));
      block.appendChild(settle);
      host.appendChild(block);
    }
    host.appendChild(roomHand('read the record again',
      'Ask the record door and the list door again, so the machine\'s newest answer is what this room stands on.',
      'again-detail', { n: number }));
    return host;
  }

  async function pressPulls(act, node) {
    if (!state.project) {
      ui.toastPush('no project is open here, so there is no pull to ask about', 'warn');
      return;
    }
    const room = roomNamed('pulls');
    const n = node.dataset.n || String(pullsRoom.open || '');
    if (act === 'filter') {
      const word = String(node.dataset.state || 'open');
      if (word === pullsRoom.filter) return;
      pullsRoom.filter = word;
      pullsRoom.open = 0;
      pullsRoom.detail = null;
      pullsRoom.conflicts = null;
      pullsRoom.painted = null;
      paintRoom(room);
      return;
    }
    if (act === 'again') {
      roomRefresh(pullsRoom, 'pulls');
      return;
    }
    if (act === 'read') {
      const wanted = String(node.dataset.n || '');
      if (!/^\d+$/.test(wanted)) {
        ui.toastPush('this row carries no pull number the record door could take', 'warn');
        return;
      }
      await openPull(wanted);
      return;
    }
    if (act === 'back') {
      pullsRoom.open = 0;
      pullsRoom.detail = null;
      pullsRoom.conflicts = null;
      pullsRoom.said = null;
      pullsRoom.typed = {};
      paintRoom(room);
      return;
    }
    if (act === 'again-detail') {
      const read = await askPull(n);
      pullsRoom.detail = read;
      paintPullsDetail(room);
      roomRefresh(pullsRoom, 'pulls');
      return;
    }
    if (act === 'ask') {
      openPullAsk();
      return;
    }
    if (act === 'ask-close') {
      pullsRoom.ask = null;
      E('ps-pulls-ask').hidden = true;
      return;
    }
    if (act === 'ask-send') {
      await sendPullAsk();
      return;
    }
    if (act === 'branches') {
      await fillPullBranches();
      return;
    }
    if (roomBusy(pullsRoom, 'a door is still answering the last press')) return;
    const body = { project: state.project };
    const who = fieldText(E('ps-pull-by'));
    const words = fieldText(E('ps-pull-body'));
    if (act === 'approve' || act === 'request-changes' || act === 'comment') {
      if (who) body.by = who;
      if (words) body.body = words;
      pullsRoom.busy = true;
      try {
        const asked = await askDoor(kPullsDoor + '/' + encodeURIComponent(n) + '/' + act, 'POST', body);
        const did = roomSaid(pullsRoom, asked, 'the machine took that review', 'the machine refused that review');
        if (did) {
          const field = E('ps-pull-body');
          if (field) field.value = '';
          pullsRoom.typed['ps-pull-body'] = '';
          // The review just stood: the record beside it reads fresh.
          const read = await askPull(n, true);
          pullsRoom.detail = read;
          paintPullsDetail(room);
          roomRefresh(pullsRoom, 'pulls');
        } else {
          paintPullsDetail(room);
        }
      } finally {
        pullsRoom.busy = false;
      }
      return;
    }
    if (act === 'close' || act === 'reopen') {
      pullsRoom.busy = true;
      try {
        const asked = await askDoor(kPullsDoor + '/' + encodeURIComponent(n) + '/' + act, 'POST', body);
        roomSaid(pullsRoom, asked, 'the machine worked that pull', 'the machine refused to work that pull');
        // The detail pane beside the hand re-reads its record from the wire.
        const read = await askPull(n, true);
        pullsRoom.detail = read;
        paintPullsDetail(room);
        roomRefresh(pullsRoom, 'pulls');
      } finally {
        pullsRoom.busy = false;
      }
      return;
    }
    if (act === 'merge') {
      const style = pullsRoom.typed['ps-pull-style'] || 'merge';
      const base = pullsRoom.detail && pullsRoom.detail.answer && pullsRoom.detail.answer.base_now
        ? String(pullsRoom.detail.answer.base_now.branch || 'its base') : 'its base';
      // The confirm says, in words, what is about to happen and in which shape -
      // the merge door moves a branch, and nothing here does that quietly.
      if (!window.confirm('Merge pull ' + n + ' into ' + base + ' with the ' + style +
        ' style? The machine moves ' + base + ' and marks the pull merged.')) return;
      body.style = style;
      if (who) body.by = who;
      pullsRoom.busy = true;
      try {
        const asked = await askDoor(kPullsDoor + '/' + encodeURIComponent(n) + '/merge', 'POST', body);
        const answer = asked.answer;
        if (answer && answer.conflict === true) {
          // A conflict is an answer, not a refusal: the pull stays open and the
          // paths it names are drawn with the three sides each one carries.
          pullsRoom.conflicts = answer;
          ui.log('the merge door answered with ' +
            (Array.isArray(answer.conflicts) ? answer.conflicts.length : '?') +
            ' path(s) in conflict on pull ' + n + '; the pull stays open', 'warn');
          ui.toastPush('the merge came back with conflicts: settle the paths, then merge again', 'warn', 12);
        } else {
          pullsRoom.conflicts = null;
          roomSaid(pullsRoom, asked, 'the machine merged the pull', 'the machine refused the merge');
        }
        // The record beside the hand that just merged it reads fresh.
        const read = await askPull(n, true);
        pullsRoom.detail = read;
        paintPullsDetail(room);
        roomRefresh(pullsRoom, 'pulls');
      } finally {
        pullsRoom.busy = false;
      }
      return;
    }
    if (act === 'resolve' || act === 'resolve-delete') {
      body.file = String(node.dataset.file || '');
      if (act === 'resolve-delete') {
        body.delete = true;
      } else {
        const box = E(node.dataset.box || '');
        body.content = box ? String(box.value || '') : (pullsRoom.typed[node.dataset.box] || '');
      }
      if (who) body.by = who;
      pullsRoom.busy = true;
      try {
        const asked = await askDoor(kPullsDoor + '/' + encodeURIComponent(n) + '/resolve', 'POST', body);
        roomSaid(pullsRoom, asked, 'the machine kept the settlement', 'the machine refused that settlement');
        paintPullsDetail(room);
      } finally {
        pullsRoom.busy = false;
      }
      return;
    }
  }

  // The sheet for opening a pull: two branch pickers, fed by the branches door,
  // and the two words the pull is opened with. The door reads the tips when the
  // sheet is sent; nothing here claims a branch exists because a name was typed.
  function openPullAsk() {
    const host = E('ps-pulls-ask');
    if (!host) return;
    host.hidden = false;
    host.textContent = '';
    host.appendChild(el('div', 'ps-detail-part', 'open a pull'));
    const headRow = el('div', 'ps-field-row');
    const headLabel = el('label', null, 'head');
    headLabel.htmlFor = 'ps-pull-new-head';
    headRow.appendChild(headLabel);
    const head = el('select');
    head.id = 'ps-pull-new-head';
    head.title = 'The branch whose work is being offered. The list is what the branches door named; blank leaves the machine stated nothing.';
    headRow.appendChild(head);
    const baseRow = el('div', 'ps-field-row');
    const baseLabel = el('label', null, 'base');
    baseLabel.htmlFor = 'ps-pull-new-base';
    baseRow.appendChild(baseLabel);
    const base = el('select');
    base.id = 'ps-pull-new-base';
    base.title = 'The branch the work lands in. Blank asks the machine for its own default branch - the door reads it from the project\'s facts and answers in its own words if the project states none.';
    baseRow.appendChild(base);
    host.appendChild(headRow);
    host.appendChild(baseRow);
    roomField(host, 'title', 'ps-pull-new-title', 'text', pullsRoom.typed['ps-pull-new-title'] || '',
      'The words this pull is called by. They go to the record as they stand.');
    roomField(host, 'body', 'ps-pull-new-body', 'textarea', pullsRoom.typed['ps-pull-new-body'] || '',
      'What this pull is. Blank opens the record with no body.');
    const hands = el('div', 'ps-detail-hands');
    hands.appendChild(roomHand('send this to the machine',
      'Ask the machine to open the pull between the two branches chosen above. Both tips are read now, and what the record refuses about them - the same branch on both sides, a head that already stands at the base - comes back in the machine\'s own words.',
      'ask-send'));
    hands.appendChild(roomHand('put the sheet away', 'Take the sheet away; nothing is asked of the machine.', 'ask-close'));
    hands.appendChild(roomHand('read the branches again',
      'Ask the branches door again, so the pickers hold what it names now.', 'branches'));
    host.appendChild(hands);
    const noteHost = el('div', 'ps-note');
    host.appendChild(noteHost);
    pullsRoom.ask = 'pull';
    void fillPullBranches();
  }

  // The branch pickers, filled from the branches door and nowhere else: the rows,
  // the machine's own default marked, and any word it stated about the open-pull
  // counts beside them.
  async function fillPullBranches() {
    const head = E('ps-pull-new-head');
    const base = E('ps-pull-new-base');
    const host = E('ps-pulls-ask');
    if (!head || !base) return;
    const asked = await askDoor(kBranchesDoor + '?project=' + encodeURIComponent(state.project));
    const noteHost = host ? host.querySelector('.ps-note') : null;
    head.textContent = '';
    base.textContent = '';
    if (!asked.answer || !Array.isArray(asked.answer.branches)) {
      head.appendChild(el('option', null, 'the machine named no branches'));
      base.appendChild(el('option', null, 'the machine named no branches'));
      if (noteHost) {
        note(noteHost, asked.error || 'the branches door answered nothing, so no branch is offered here', 'unknown',
          'The pickers are filled only from what the branches door named. A name typed nowhere is not a branch.');
      }
      return;
    }
    const answer = asked.answer;
    // The first choice of each picker wears no value: a head left there is no
    // head at all, and a base left there asks the door for the project's own
    // default rather than sending a name this page made up.
    const noHead = el('option', null, 'a branch to choose');
    noHead.value = '';
    head.appendChild(noHead);
    const noBase = el('option', null, 'the machine\'s own default branch');
    noBase.value = '';
    base.appendChild(noBase);
    for (const one of answer.branches) {
      const name = one && one.name ? String(one.name) : '';
      if (!name) continue;
      const words = name + (one.protected === true ? ' · protected' : '') +
        (Number(one.open_pr) > 0 ? ' · ' + String(one.open_pr) + ' open pull' + (Number(one.open_pr) === 1 ? '' : 's') : '') +
        ' · ' + shortOid(one.oid);
      const headChoice = el('option', null, words);
      headChoice.value = name;
      headChoice.title = 'The branch ' + name + ' at ' + String(one.oid || 'no oid stated') +
        (one.protected === true ? ' - the machine marks this one protected, the default of the project as its own facts state it' : '') +
        (Number(one.open_pr) > 0 ? ' - the branches door counted ' + String(one.open_pr) + ' open pull(s) with this head' : '');
      head.appendChild(headChoice);
      const baseChoice = el('option', null, words);
      baseChoice.value = name;
      baseChoice.title = headChoice.title;
      base.appendChild(baseChoice);
    }
    if (pullsRoom.typed['ps-pull-new-head']) head.value = pullsRoom.typed['ps-pull-new-head'];
    if (pullsRoom.typed['ps-pull-new-base']) base.value = pullsRoom.typed['ps-pull-new-base'];
    if (noteHost) {
      const tails = ['branches from ' + kBranchesDoor + ' for ' + state.project];
      if (answer.default) tails.push('the machine calls ' + answer.default + ' the default');
      if (answer.open_pr_truncated === true) tails.push('the open-pull counts stopped at the cap the door reads by');
      if (answer.open_pr_note) tails.push(String(answer.open_pr_note));
      note(noteHost, tails.join(' · '), answer.open_pr_note ? 'unknown' : '',
        'What the branches door stated about its own reading.');
    }
  }

  async function sendPullAsk() {
    const head = fieldText(E('ps-pull-new-head'));
    const base = fieldText(E('ps-pull-new-base'));
    const title = fieldText(E('ps-pull-new-title'));
    const bodyWords = fieldText(E('ps-pull-new-body'));
    if (!head) {
      ui.toastPush('the pull has no head branch to offer, and the door asks for one', 'warn');
      return;
    }
    const body = { project: state.project, head: head, title: title };
    if (base) body.base = base;
    if (bodyWords) body.body = bodyWords;
    if (roomBusy(pullsRoom, 'opening the last pull')) return;
    pullsRoom.busy = true;
    let asked = null;
    try {
      asked = await askDoor(kPullsDoor, 'POST', body);
    } finally {
      pullsRoom.busy = false;
    }
    if (roomSaid(pullsRoom, asked, 'the machine opened the pull', 'the machine refused to open that pull')) {
      pullsRoom.ask = null;
      const sheet = E('ps-pulls-ask');
      if (sheet) sheet.hidden = true;
      const made = asked.answer.pull && asked.answer.pull.number !== undefined
        ? String(asked.answer.pull.number) : '';
      roomRefresh(pullsRoom, 'pulls');
      if (made) await openPull(made);
      return;
    }
    const host = E('ps-pulls-ask');
    if (host) roomRefusal(host, asked);
  }

  // ---------- the issues room ----------
  // The rows are the compact reading the machine makes for agents and pages
  // alike: one line per issue with the paths it wears. The record door adds the
  // rest, and its own re-anchoring of those paths against the head as it stands
  // now - which is why a line is only ever claimed where the machine says the
  // bytes it was read in are the bytes that stand.
  function armIssuesRoom(room) {
    if (issuesRoom.armed) return;
    issuesRoom.armed = true;
    const hosts = [E('ps-issues-bar'), E('ps-issues-ask'), E('ps-issues-detail'), room.listEl];
    for (const host of hosts) {
      if (!host) continue;
      host.addEventListener('click', function (event) {
        const node = event && event.target ? event.target : null;
        if (!node || !node.closest) return;
        const hit = node.closest('[data-act]');
        if (!hit || !host.contains(hit)) return;
        ui.flash(hit);
        void pressIssues(String(hit.dataset.act || ''), hit);
      });
      host.addEventListener('input', function (event) {
        const node = event && event.target ? event.target : null;
        if (!node || !node.id) return;
        issuesRoom.typed[node.id] = String(node.value || '');
      });
      host.addEventListener('change', function (event) {
        const node = event && event.target ? event.target : null;
        if (!node || !node.id) return;
        issuesRoom.typed[node.id] = String(node.value || '');
      });
    }
  }

  function paintIssuesRoom(room, answer, slug, heldAt) {
    armIssuesRoom(room);
    if (issuesRoom.slug !== slug) {
      issuesRoom.slug = slug;
      issuesRoom.open = '';
      issuesRoom.detail = null;
      issuesRoom.ask = null;
      issuesRoom.typed = {};
      issuesRoom.painted = null;
      issuesRoom.paintedDetail = null;
      issuesRoom.said = null;
    }
    if (issuesRoom.open) {
      room.listEl.hidden = true;
      const asking = E('ps-issues-ask');
      if (asking) asking.hidden = true;
      // The sheet stands over the list, but the list keeps its own rows from
      // this reading: a record the page just worked has left the filter the
      // list stands on, and the row must be gone the moment the sheet is put
      // away - not at the next ask. The detail is painted after, so its note
      // is the one that stands beside the open sheet.
      if (issuesRoom.painted !== answer) {
        issuesRoom.painted = answer;
        paintIssuesBar(room, answer);
        paintIssuesList(room, answer, slug, heldAt);
      }
      paintIssuesDetail(room);
      return;
    }
    const detail = E('ps-issues-detail');
    if (detail) detail.hidden = true;
    room.listEl.hidden = false;
    if (issuesRoom.painted === answer) return;
    issuesRoom.painted = answer;
    paintIssuesBar(room, answer);
    paintIssuesList(room, answer, slug, heldAt);
  }

  function paintIssuesBar(room, answer) {
    const bar = E('ps-issues-bar');
    if (!bar) return;
    bar.textContent = '';
    const filters = el('span', 'ps-modes', null,
      'Which issues this room asks for. The issues door takes open or closed - or the word left off entirely, which is every issue it keeps. The machine is not told a filter was chosen; the ask that follows carries it.');
    for (const word of kIssueStates) {
      const hand = el('button', 'ps-mode' + (issuesRoom.filter === word ? ' ps-mode-on' : ''), word,
        'Ask for the issues in this state.');
      hand.dataset.act = 'filter';
      hand.dataset.state = word;
      filters.appendChild(hand);
    }
    bar.appendChild(filters);
    const pills = el('span', 'ps-pills');
    const rows = Array.isArray(answer.issues) ? answer.issues : [];
    if (rows.length) {
      // The one count this page takes itself: how many of the rows it was handed
      // wear a tag for the path the code room is standing on. It is said as the
      // count of these rows, and the door's own hasMore word rides beside it.
      const path = view.path ? String(view.path) : '';
      if (path) {
        let here = 0;
        for (const one of rows) {
          const tags = one && Array.isArray(one.files) ? one.files : [];
          for (const tag of tags) if (String(tag) === path) ++here;
        }
        pills.appendChild(roomChip('issues here', String(here),
          'How many of the ' + rows.length + ' rows this reading carried wear a tag for ' + path +
          ', the file the code room stands on. The page counted the rows it was given; the machine states no count here.'));
      }
    }
    if (issuesRoom.file) {
      pills.appendChild(roomChip('file filter', issuesRoom.file,
        'This room is asking for the issues that wear a tag for exactly this path. The hand beside these pills puts the filter away.'));
    }
    bar.appendChild(pills);
    if (view.path) {
      bar.appendChild(roomHand(issuesRoom.file ? 'put the path filter away' : 'only ' + view.path,
        issuesRoom.file
          ? 'Stop asking for one path, and let the room read the whole state again.'
          : 'Ask the issues door for the issues that wear a tag for ' + view.path +
          ', the file the code room stands on. The door matches the path exactly, as it states.',
        issuesRoom.file ? 'unfile' : 'file', { path: String(view.path) }));
    }
    bar.appendChild(roomHand('open an issue',
      'Open an issue on this project: the sheet asks for a title, the words, how much it matters, and any labels. The machine makes the id and the record, and answers with both.',
      'ask'));
    bar.appendChild(roomHand('ask again',
      'Ask the issues door again for what this room stands on, and draw the rows from what it answers this time.',
      'again'));
  }

  function paintIssuesList(room, answer, slug, heldAt) {
    const list = room.listEl;
    list.textContent = '';
    const rows = Array.isArray(answer.issues) ? answer.issues : [];
    if (!rows.length) {
      quiet(list, 'the machine listed no issues for ' +
        (issuesRoom.filter === 'all' ? 'any state' : 'state ' + issuesRoom.filter) +
        (issuesRoom.file ? ' on ' + issuesRoom.file : ''),
        'The door answered, and its answer carried no rows. The state and path asked are what bounded the reading.');
    }
    for (const one of rows) {
      const id = one && one.id ? String(one.id) : '';
      const row = el('button', 'ps-rec ps-issue-row');
      row.dataset.act = 'read';
      row.dataset.id = id;
      row.title = 'Read issue ' + (id || '?') + ': the record, the machine\'s own re-anchoring of the paths it wears, and the doors that work it.';
      row.appendChild(el('span', 'ps-rec-no', id || 'no id'));
      row.appendChild(el('span', 'ps-rec-name', one && one.title ? String(one.title) : 'an issue with no title'));
      const severity = one && one.severity ? String(one.severity) : '';
      row.appendChild(roomChip('severity', severity || 'none stated',
        'How much this issue matters, in the word the machine keeps for it: note, bug, want, or blocker.',
        !severity));
      row.appendChild(roomWord(one ? one.state : ''));
      const tags = one && Array.isArray(one.files) ? one.files : [];
      if (tags.length) {
        row.appendChild(el('span', 'ps-rec-files', tags.map(String).join('  '),
          'The paths this issue wears a tag for, as the record keeps them.'));
      }
      list.appendChild(row);
    }
    const tails = [];
    if (answer.hasMore === true) {
      tails.push('the door stopped at the cap it reads by and says more stand beside these rows');
    }
    const unparseable = Array.isArray(answer.unparseable) ? answer.unparseable : [];
    if (unparseable.length) {
      tails.push('the machine named ' + unparseable.length + ' record' + (unparseable.length === 1 ? '' : 's') +
        ' it could not read: ' + unparseable.join(', '));
    }
    const unreadable = Array.isArray(answer.unreadable) ? answer.unreadable : [];
    if (unreadable.length) {
      tails.push('the machine named ' + unreadable.length + ' issue' + (unreadable.length === 1 ? '' : 's') +
        ' whose record this reading could not take: ' + unreadable.join(', '));
    }
    note(room.noteEl, 'listed by the machine for ' + slug + ' · asked for ' +
      (issuesRoom.filter === 'all' ? 'every state' : issuesRoom.filter) +
      (issuesRoom.file ? ' · path ' + issuesRoom.file : '') + heldTail(heldAt) +
      (tails.length ? ' · ' + tails.join(' · ') : ''), tails.length ? 'unknown' : '',
      'What the door stated about its own reading: the state and path asked, and whether it stopped short.');
  }

  async function askIssue(id, noStore) {
    if (!state.project) return null;
    const asked = await askDoor(kIssueRecords + '/' + encodeURIComponent(id) +
      '?project=' + encodeURIComponent(state.project) + (noStore ? '&t=' + Date.now() : ''));
    return asked.answer ? asked : null;
  }

  async function openIssue(id) {
    if (roomBusy(issuesRoom, 'reading an issue')) return;
    issuesRoom.busy = true;
    issuesRoom.open = id;
    issuesRoom.detail = null;
    issuesRoom.said = null;
    issuesRoom.typed = {};
    try {
      issuesRoom.detail = await askIssue(id);
    } finally {
      issuesRoom.busy = false;
    }
    if (state.tab === 'issues' && issuesRoom.open === id) {
      const listed = roomNamed('issues');
      if (listed) paintRoom(listed);
    }
  }

  // The anchor states, said as the door states them. A line is claimed nowhere
  // this page has not been told the bytes behind it still stand.
  function anchorWords(row) {
    const state = row && row.state ? String(row.state) : '';
    if (state === 'unchanged') {
      return 'the bytes this was read at are the bytes that stand' +
        (row.line_stands === true ? ' · the line stands where it was read' : '');
    }
    if (state === 'renamed') {
      return 'moved to ' + (row.moved_to ? String(row.moved_to) : 'a path the machine did not name') +
        (row.line_stands === true ? ' · the line stands there too' : '');
    }
    if (state === 'changed') {
      const lineNow = Number(row.line_now);
      if (row.line_exact === true && Number.isFinite(lineNow) && lineNow > 0) {
        return 'the path is at the head wearing different bytes · its line now ' + lineNow;
      }
      return 'the path is at the head wearing different bytes · no line is claimed';
    }
    if (state === 'absent') return 'the path is not at the head · the issue is orphaned from it';
    if (state === 'no-anchor-reading') return 'this reading could not weigh the tag at all';
    return 'the machine stated no anchor state for this path';
  }

  function paintIssuesDetail(room) {
    if (!room) return;
    const host = E('ps-issues-detail');
    if (!host) return;
    const read = issuesRoom.detail;
    if (issuesRoom.paintedDetail && issuesRoom.paintedDetail[0] === read &&
      issuesRoom.paintedDetail[1] === issuesRoom.said && host.childNodes.length) return;
    issuesRoom.paintedDetail = [read, issuesRoom.said];
    host.hidden = false;
    host.textContent = '';
    const head = el('div', 'ps-detail-head');
    head.appendChild(roomHand('the list', 'Put this record away and draw the rows again.', 'back'));
    if (!read) {
      head.appendChild(el('span', 'ps-quiet', 'asking the machine for ' + issuesRoom.open + '…'));
      host.appendChild(head);
      return;
    }
    if (!read.answer) {
      head.appendChild(el('span', 'ps-quiet', 'the machine answered nothing at the record door'));
      host.appendChild(head);
      roomRefusal(host, read);
      return;
    }
    const answer = read.answer;
    const record = answer.issue && answer.issue.id ? answer.issue : null;
    const id = record ? String(record.id) : String(issuesRoom.open);
    head.appendChild(el('span', 'ps-rec-no', id));
    head.appendChild(el('span', 'ps-rec-name', record && record.title ? String(record.title) : 'an issue with no title'));
    head.appendChild(roomWord(record ? record.state : ''));
    host.appendChild(head);

    const facts = el('div', 'ps-facts');
    roomFact(facts, 'severity', record && record.severity ? String(record.severity) : undefined,
      'The word the machine keeps for how much this matters: note, bug, want, or blocker.');
    roomFact(facts, 'opened', record ? whenWords(record.created_at) : '', 'The moment the record says it was opened.');
    roomFact(facts, 'changed', record ? whenWords(record.updated_at) : '', 'The moment the record says it last changed.');
    const labels = record && Array.isArray(record.labels) ? record.labels : [];
    roomFact(facts, 'labels', labels.length ? labels.map(String).join(', ') : '',
      'The words this record carries beside its name, as it keeps them.');
    host.appendChild(facts);

    const bodyWords = record && record.body ? String(record.body) : '';
    const bodyPane = el('pre', 'ps-record-body');
    bodyPane.textContent = bodyWords || 'the record carries no body';
    if (!bodyWords) bodyPane.title = 'The machine stated no body for this issue.';
    host.appendChild(bodyPane);

    // Where the paths this issue wears stand now: the record's own tags, and the
    // machine's re-anchoring of each one against the head as it stands.
    const anchors = el('div', 'ps-thread');
    anchors.appendChild(el('div', 'ps-detail-part', 'where its paths stand now'));
    const tags = record && Array.isArray(record.files) ? record.files : [];
    const now = Array.isArray(answer.anchor_now) ? answer.anchor_now : [];
    if (!tags.length && !now.length) {
      anchors.appendChild(el('div', 'ps-quiet', 'the record wears no path',
        'Nothing in this issue is pinned to a file, so there is nothing to re-anchor. That is the record\'s statement, not this page\'s.'));
    }
    for (const tag of tags) {
      const path = tag && tag.path ? String(tag.path) : 'a path the record did not name';
      let row = null;
      for (const one of now) if (one && String(one.path) === path) row = one;
      const line = Number(row ? row.line : (tag ? tag.line : 0));
      const words = path + (Number.isFinite(line) && line > 0 ? ':' + line : '') + ' · ' + anchorWords(row);
      const standing = !!row && row.state === 'unchanged' && row.line_stands === true;
      // A renamed row whose line stands is read at the new path the machine
      // names, never at the old one.
      const openPath = standing ? path
        : (!!row && row.state === 'renamed' && row.line_stands === true && row.moved_to
          ? String(row.moved_to) : '');
      const line2 = el('div', 'ps-anchor' + (standing ? '' : ' ps-anchor-moved'));
      line2.appendChild(el('span', standing ? 'ps-anchor-path' : 'ps-anchor-quiet', words));
      if (openPath) {
        line2.appendChild(roomHand('read it here',
          'Open ' + openPath + ' in the code room at this line. ' + (standing
            ? 'The machine says the bytes behind this line are the bytes that stand at the head.'
            : 'The machine says the path was renamed and the line behind this tag stands at the new path.'),
          'open-file', { path: openPath, line: String(Number.isFinite(line) && line > 0 ? line : '') }));
      }
      anchors.appendChild(line2);
    }
    host.appendChild(anchors);

    const links = record && record.links && typeof record.links === 'object' ? record.links : null;
    const linksHost = el('div', 'ps-thread');
    linksHost.appendChild(el('div', 'ps-detail-part', 'links'));
    for (const bucket of ['prs', 'issues', 'commits']) {
      const held = links && Array.isArray(links[bucket]) ? links[bucket] : [];
      linksHost.appendChild(el('div', 'ps-link-row', bucket + ' · ' +
        (held.length ? held.map(function (one) { return String(one); }).join(', ') : 'the record keeps none')));
    }
    host.appendChild(linksHost);

    const comments = record && Array.isArray(record.comments) ? record.comments : [];
    const commentsHost = el('div', 'ps-thread');
    commentsHost.appendChild(el('div', 'ps-detail-part', 'comments · ' + comments.length));
    roomThread(commentsHost, comments, 'the record carries no comments', 'No comment stands on this record.');
    host.appendChild(commentsHost);

    const work = el('div', 'ps-detail-work');
    work.appendChild(el('div', 'ps-detail-part', 'say something to the machine'));
    roomField(work, 'who signs it', 'ps-issue-by', 'text', issuesRoom.typed['ps-issue-by'] || '',
      'Who this comment is kept under. Blank sends no name, and the door keeps its own word for a page asking: webui.',
      'webui');
    roomField(work, 'words', 'ps-issue-body', 'textarea', issuesRoom.typed['ps-issue-body'] || '',
      'The words of the comment. A comment past the 4 KiB one is kept at is refused by the door, naming the size. A closed issue takes no comment, and the door says so.');
    const hands = el('div', 'ps-detail-hands');
    hands.appendChild(roomHand('comment', 'Record this comment on the issue.', 'comment', { id: id }));
    const worn = record ? String(record.state || '') : '';
    if (worn === 'open') {
      hands.appendChild(roomHand('close',
        'Close this issue. The record keeps its file and its id, and the machine answers in its own words.',
        'close', { id: id }));
    } else if (worn === 'closed') {
      hands.appendChild(roomHand('reopen', 'Reopen this issue.', 'reopen', { id: id }));
    }
    work.appendChild(hands);
    host.appendChild(work);

    // The link editor: the kind the record buckets a link under, and the thing it
    // names. The door refuses a link with no target, in words.
    const link = el('div', 'ps-detail-work');
    link.appendChild(el('div', 'ps-detail-part', 'link it to one thing'));
    const kindRow = el('div', 'ps-field-row');
    const kindLabel = el('label', null, 'kind');
    kindLabel.htmlFor = 'ps-issue-link-kind';
    kindRow.appendChild(kindLabel);
    const kind = el('select');
    kind.id = 'ps-issue-link-kind';
    kind.title = 'Which bucket this link goes into. The issue door takes pr, issue, or commit, and refuses any other word by name.';
    for (const word of kLinkKinds) {
      const choice = el('option', null, word);
      choice.value = word;
      kind.appendChild(choice);
    }
    kind.value = issuesRoom.typed['ps-issue-link-kind'] || 'issue';
    kindRow.appendChild(kind);
    link.appendChild(kindRow);
    roomField(link, 'target', 'ps-issue-link-target', 'text', issuesRoom.typed['ps-issue-link-target'] || '',
      'The thing this issue is linked to: a pull number, another issue id, or a commit. It goes as it is typed.');
    const linkHands = el('div', 'ps-detail-hands');
    linkHands.appendChild(roomHand('link it',
      'Ask the machine to keep this link beside the record. A target left blank is refused by the door, and its words are shown here.',
      'link', { id: id }));
    link.appendChild(linkHands);
    host.appendChild(link);

    // What the machine last said about a press made in this room, standing under
    // the record until something else is pressed.
    if (issuesRoom.said) {
      if (issuesRoom.said.did) {
        host.appendChild(el('div', 'ps-quiet', issuesRoom.said.words,
          'The machine\'s own sentence about the last thing this room pressed. It stands here until another record is opened.'));
      } else {
        roomRefusal(host, { answer: null, error: issuesRoom.said.words });
      }
    }

    if (room.noteEl) {
      note(room.noteEl, id + ' read from ' + kIssueRecords + '/' + id +
        (now.length ? ' · the machine re-anchored ' + now.length + ' path' + (now.length === 1 ? '' : 's') : ''),
        '', 'Where this room stands: the door the record came from, and how many paths the machine re-anchored with it.');
    }
  }

  async function pressIssues(act, node) {
    if (!state.project) {
      ui.toastPush('no project is open here, so there is no issue to ask about', 'warn');
      return;
    }
    const room = roomNamed('issues');
    const id = node.dataset.id || String(issuesRoom.open || '');
    if (act === 'filter') {
      const word = String(node.dataset.state || 'open');
      if (word === issuesRoom.filter) return;
      issuesRoom.filter = word;
      issuesRoom.open = '';
      issuesRoom.detail = null;
      issuesRoom.painted = null;
      paintRoom(room);
      return;
    }
    if (act === 'file') {
      issuesRoom.file = String(node.dataset.path || '');
      issuesRoom.painted = null;
      paintRoom(room);
      return;
    }
    if (act === 'unfile') {
      issuesRoom.file = '';
      issuesRoom.painted = null;
      paintRoom(room);
      return;
    }
    if (act === 'again') {
      roomRefresh(issuesRoom, 'issues');
      return;
    }
    if (act === 'read') {
      const wanted = String(node.dataset.id || '');
      if (!wanted) {
        ui.toastPush('this row carries no issue id the record door could take', 'warn');
        return;
      }
      await openIssue(wanted);
      return;
    }
    if (act === 'back') {
      issuesRoom.open = '';
      issuesRoom.detail = null;
      issuesRoom.said = null;
      issuesRoom.typed = {};
      paintRoom(room);
      return;
    }
    if (act === 'open-file') {
      const path = String(node.dataset.path || '');
      const line = Number(node.dataset.line) > 0 ? Number(node.dataset.line) : 0;
      showRoom(roomNamed('code'));
      await openFile(path, line, view.ref);
      return;
    }
    if (act === 'ask') {
      openIssueAsk();
      return;
    }
    if (act === 'ask-close') {
      issuesRoom.ask = null;
      E('ps-issues-ask').hidden = true;
      return;
    }
    if (act === 'ask-send') {
      await sendIssueAsk();
      return;
    }
    if (roomBusy(issuesRoom, 'a door is still answering the last press')) return;
    const body = { project: state.project };
    const who = fieldText(E('ps-issue-by'));
    const words = fieldText(E('ps-issue-body'));
    if (act === 'comment') {
      if (who) body.by = who;
      if (words) body.body = words;
      issuesRoom.busy = true;
      try {
        const asked = await askDoor(kIssueRecords + '/' + encodeURIComponent(id) + '/comments', 'POST', body);
        const did = roomSaid(issuesRoom, asked, 'the machine took that comment', 'the machine refused that comment');
        if (did) {
          const field = E('ps-issue-body');
          if (field) field.value = '';
          issuesRoom.typed['ps-issue-body'] = '';
        }
        // The record beside the hand reads fresh, refused or not.
        issuesRoom.detail = await askIssue(id, true);
        paintIssuesDetail(room);
        roomRefresh(issuesRoom, 'issues');
      } finally {
        issuesRoom.busy = false;
      }
      return;
    }
    if (act === 'close' || act === 'reopen') {
      issuesRoom.busy = true;
      try {
        const asked = await askDoor(kIssueRecords + '/' + encodeURIComponent(id) + '/' + act, 'POST', body);
        roomSaid(issuesRoom, asked, 'the machine worked that issue', 'the machine refused to work that issue');
        // The detail pane beside the hand re-reads its record from the wire:
        // an answer kept before this press would paint yesterday's state.
        issuesRoom.detail = await askIssue(id, true);
        paintIssuesDetail(room);
        roomRefresh(issuesRoom, 'issues');
      } finally {
        issuesRoom.busy = false;
      }
      return;
    }
    if (act === 'link') {
      const kind = issueLinkKind();
      const target = fieldText(E('ps-issue-link-target'));
      body.kind = kind;
      if (target) body.target = target;
      issuesRoom.busy = true;
      try {
        const asked = await askDoor(kIssueRecords + '/' + encodeURIComponent(id) + '/link', 'POST', body);
        roomSaid(issuesRoom, asked, 'the machine kept that link', 'the machine refused that link');
        // The record beside the hand reads fresh.
        issuesRoom.detail = await askIssue(id, true);
        paintIssuesDetail(room);
        roomRefresh(issuesRoom, 'issues');
      } finally {
        issuesRoom.busy = false;
      }
      return;
    }
  }

  function issueLinkKind() {
    const field = E('ps-issue-link-kind');
    return field ? String(field.value || 'issue') : 'issue';
  }

  // The sheet for opening an issue. Title, body, severity, labels - and a path
  // with an optional line, the spine every issue is pinned to. Blank fields are
  // left out of the ask, so the machine keeps its own word where it has one.
  function openIssueAsk() {
    const host = E('ps-issues-ask');
    if (!host) return;
    host.hidden = false;
    host.textContent = '';
    host.appendChild(el('div', 'ps-detail-part', 'open an issue'));
    roomField(host, 'title', 'ps-issue-new-title', 'text', issuesRoom.typed['ps-issue-new-title'] || '',
      'The words this issue is called by. The door refuses an issue opened without one, in its own words.');
    roomField(host, 'body', 'ps-issue-new-body', 'textarea', issuesRoom.typed['ps-issue-new-body'] || '',
      'What this issue is. A body past the 64 KiB one is kept at is refused by the door, naming the size.');
    const severityRow = el('div', 'ps-field-row');
    const severityLabel = el('label', null, 'severity');
    severityLabel.htmlFor = 'ps-issue-new-severity';
    severityRow.appendChild(severityLabel);
    const severity = el('select');
    severity.id = 'ps-issue-new-severity';
    severity.title = 'How much this matters. The first choice sends no word at all, and the door keeps its own - it states that word as note.';
    const ownWord = el('option', null, 'the machine\'s own word');
    ownWord.value = '';
    severity.appendChild(ownWord);
    for (const word of kIssueSeverities) {
      const choice = el('option', null, word);
      choice.value = word;
      severity.appendChild(choice);
    }
    severity.value = issuesRoom.typed['ps-issue-new-severity'] || '';
    severityRow.appendChild(severity);
    host.appendChild(severityRow);
    roomField(host, 'labels', 'ps-issue-new-labels', 'text', issuesRoom.typed['ps-issue-new-labels'] || '',
      'Words to keep beside this issue, separated by commas. The door keeps at most 20 of at most 64 bytes each.');
    roomField(host, 'path', 'ps-issue-new-path', 'text',
      issuesRoom.typed['ps-issue-new-path'] !== undefined ? issuesRoom.typed['ps-issue-new-path'] : String(view.path || ''),
      'A file this issue is pinned to, as the project lists it. Blank pins the issue to nothing.');
    roomField(host, 'line', 'ps-issue-new-line', 'number',
      issuesRoom.typed['ps-issue-new-line'] !== undefined ? issuesRoom.typed['ps-issue-new-line'] :
        (Number(view.line) > 0 ? String(view.line) : ''),
      'The line the issue was seen at, beside the path above. Blank keeps the tag at the path alone - the machine re-anchors what it is given, and claims no line it cannot weigh.');
    const hands = el('div', 'ps-detail-hands');
    hands.appendChild(roomHand('send this to the machine',
      'Ask the machine to open this issue. It makes the id, writes the record, and answers with both; the rows are read again after.',
      'ask-send'));
    hands.appendChild(roomHand('put the sheet away', 'Take the sheet away; nothing is asked of the machine.', 'ask-close'));
    host.appendChild(hands);
    issuesRoom.ask = 'issue';
  }

  async function sendIssueAsk() {
    const title = fieldText(E('ps-issue-new-title'));
    const bodyWords = fieldText(E('ps-issue-new-body'));
    const severity = String((E('ps-issue-new-severity') || {}).value || '');
    const labels = fieldText(E('ps-issue-new-labels'));
    const path = fieldText(E('ps-issue-new-path'));
    const line = Number(fieldText(E('ps-issue-new-line')));
    const body = { project: state.project, title: title };
    if (bodyWords) body.body = bodyWords;
    if (severity) body.severity = severity;
    const labelWords = labels.split(',').map(function (one) { return one.trim(); })
      .filter(function (one) { return one.length > 0; });
    if (labelWords.length) body.labels = labelWords;
    if (path) {
      const tag = { path: path };
      if (Number.isFinite(line) && line > 0) tag.line = line;
      body.files = [tag];
    }
    if (roomBusy(issuesRoom, 'opening the last issue')) return;
    issuesRoom.busy = true;
    let asked = null;
    try {
      asked = await askDoor(kIssueRecords, 'POST', body);
    } finally {
      issuesRoom.busy = false;
    }
    roomSaid(issuesRoom, asked, 'the machine opened the issue', 'the machine refused to open that issue');
    if (!asked.answer || !asked.answer.did) {
      const host = E('ps-issues-ask');
      if (host) roomRefusal(host, asked);
      return;
    }
    issuesRoom.ask = null;
    const sheet = E('ps-issues-ask');
    if (sheet) sheet.hidden = true;
    const made = asked.answer.id ? String(asked.answer.id) : '';
    roomRefresh(issuesRoom, 'issues');
    if (made) await openIssue(made);
  }

  // ---------- the dropdown, the facts, and one reading ----------
  // The index is the one reading that says what exists. Its stamp is what the
  // page compares against: a reading that says nothing moved repaints nothing.
  function stampOf(answer) {
    const projects = Array.isArray(answer.projects) ? answer.projects : [];
    const shape = [];
    for (const project of projects) {
      shape.push([String(project.slug || ''), Number(project.rev || 0)]);
    }
    return JSON.stringify([Number(answer.rev || 0), shape]);
  }

  function namesOf(projects) {
    const names = [];
    for (const project of projects) names.push(String(project.slug || ''));
    return names.join(',');
  }

  function paintDropdown(projects) {
    dropdown.textContent = '';
    const noneOption = el('option', null, 'no project open');
    noneOption.value = '';
    noneOption.selected = true;
    noneOption.title = 'Nothing is chosen: the rooms say so rather than describing a project.';
    dropdown.appendChild(noneOption);
    for (const project of projects) {
      const slug = String(project.slug || '');
      if (!slug) continue;
      const option = el('option', null, project.name || slug);
      option.value = slug;
      option.title = 'Open this project: ' + slug + '. The rooms of this page are drawn from what this machine keeps of it.';
      dropdown.appendChild(option);
    }
  }

  // The quick facts beside the dropdown: branch and counts, only where the
  // machine stated them. A fact it never stated is not drawn as a zero.
  function paintFacts() {
    factsHost.textContent = '';
    if (!state.project) {
      const none = el('span', 'ps-fact-quiet-line', 'no project open');
      none.title = 'The quick facts stand beside the project in use. With none in use, there is nothing to state.';
      factsHost.appendChild(none);
      return;
    }
    let held = null;
    for (const project of state.listed) {
      if (String(project.slug || '') === state.project) held = project;
    }
    if (!held) {
      note(factsHost, 'the machine stated nothing about the project now in use', 'unknown',
        'The index that named this project carried no facts for it. Nothing here is filled in from what a project would usually be.');
      return;
    }
    let stated = false;
    const branch = held.default_branch;
    if (branch) {
      const chip = el('span', 'ps-chip');
      chip.appendChild(el('span', 'ps-chip-name', 'branch'));
      chip.appendChild(el('span', 'ps-chip-said', String(branch)));
      chip.title = 'The branch this project answers on, as the machine named it.';
      factsHost.appendChild(chip);
      stated = true;
    }
    stated = renderCounts(held) || stated;
    const wroteAt = Number(held.updated_at);
    if (Number.isFinite(wroteAt) && wroteAt > 0) {
      const chip = el('span', 'ps-chip');
      chip.appendChild(el('span', 'ps-chip-name', 'as of'));
      chip.appendChild(el('span', 'ps-chip-said', ui.whenText(wroteAt)));
      chip.title = 'The moment the machine last wrote these facts. A moment it does not state is not invented here.';
      factsHost.appendChild(chip);
      stated = true;
    }
    if (!stated) {
      const none = el('span', 'ps-fact-quiet-line', 'the machine stated nothing about this project');
      none.title = 'A fact the index never stated is not drawn here: the branch and the counts come from the machine, never from a page.';
      factsHost.appendChild(none);
    }
  }

  // The one place a count chip is made, so a door that widens its row lights a
  // chip here and nowhere else.
  //
  // Only the fields the door answers today are asked for. The index row states
  // issues_open and pulls_open, and states them only where its sidecar could be
  // read: a true zero is a stated zero and is painted as zero, while an absent
  // field is not a zero but a field the machine has no reading for - and no
  // chip stands for a reading nobody took.
  //
  // The branch tally, the pull-request numbers (pr_open, pr_merged,
  // pr_closed) and the uncommitted count (dirty) are the fields the issues and
  // pull-request systems would fill; the row carries none of them today, and a
  // chip that can never light is a control that answers to nothing, so no line
  // here asks for them. When the door starts stating one, its line joins the
  // list below - the field it wears in the row, the word the chip wears, and
  // the tip it carries - and the chip appears with that one edit.
  const kCountChips = [
    {
      field: 'issues_open', name: 'issues',
      tip: 'The open issues this machine counts for this project. The number is the machine\'s own count; where it states no count, no chip stands here.',
    },
    {
      field: 'pulls_open', name: 'pulls',
      tip: 'The open pull requests this machine counts for this project. The number is the machine\'s own count; where it states no count, no chip stands here.',
    },
  ];

  function renderCounts(row) {
    let painted = false;
    for (const wanted of kCountChips) {
      const value = Number(row[wanted.field]);
      if (!Number.isFinite(value)) continue;
      const chip = el('span', 'ps-chip');
      chip.appendChild(el('span', 'ps-chip-name', wanted.name));
      chip.appendChild(el('span', 'ps-chip-said', String(value)));
      chip.title = wanted.tip;
      factsHost.appendChild(chip);
      painted = true;
    }
    return painted;
  }

  function paintIndex(answer) {
    const wasNames = namesOf(state.listed);
    const projects = Array.isArray(answer.projects) ? answer.projects : [];
    state.listed = projects;
    indexStamp = stampOf(answer);
    paintDropdown(projects);
    let still = false;
    for (const project of projects) {
      if (String(project.slug || '') === state.project) still = true;
    }
    if (!still) {
      // The project this page stood on is off the machine's list: a history
      // surface walking it comes down with it, the same as through the dropdown.
      histStandDown();
      state.project = '';
      dropdown.value = '';
    }
    if (!state.project && deepLinkProject) {
      for (const project of projects) {
        if (String(project.slug || '') === deepLinkProject) state.project = deepLinkProject;
      }
      if (state.project) {
        dropdown.value = state.project;
        paintAddress();
      }
    }
    for (const room of ROOMS) {
      if (room.tab === state.tab) paintRoom(room);
    }
    paintFacts();
    if (namesOf(projects) !== wasNames) ui.log('the list of projects this machine keeps changed: the dropdown names what it lists now.', 'ok');
  }

  // What this page's own read of its index door last answered with - status
  // and words from that one result. The words about a machine that stands but
  // went quiet are drawn from this, never from a shared pair another ask may
  // have written over while this one was in flight.
  let lastIndexRead = { answer: null, status: 0, error: '' };

  // ---------- the machine, at this page's own pace ----------
  // A reading taken because the stream beside the doors says something moved
  // may not be answered from a kept copy of the answer from before the move:
  // {noStore} says so to the fetch and to this machine's own browser cache.
  async function refresh(noStore) {
    const read = await ui.apiR(kIndexDoor, noStore ? 'GET' : undefined, undefined,
      noStore ? { cache: 'no-store' } : undefined);
    lastIndexRead = read;
    const answer = read.answer;
    if (!answer) {
      // A door that is not on this machine is a known absence, not a machine
      // that went quiet: the page says which door it asked and keeps standing.
      if (read.status === 404) {
        offlineMentioned = false;
        say('this machine has no door at ' + kIndexDoor + ': the rooms say so, and the page keeps asking at its own pace.', '',
          'The shell of the page is drawn whether or not the machine behind it has these doors yet. A door that is not there is named as not there, and nothing else is invented.');
        return;
      }
      throw new Error('the index door gave no answer');
    }
    if (!Array.isArray(answer.projects)) {
      say('the index door answered without naming projects at all: nothing is painted from it.', 'warn',
        'A reading this page cannot read is not a reading about the machine. The dropdown keeps what was last well read.');
      return;
    }
    if (stampOf(answer) === indexStamp) return;
    offlineMentioned = false;
    if (namesOf(answer.projects) !== namesOf(state.listed)) {
      say('the projects this machine keeps changed: the dropdown and the rooms are painted from the new reading.', '',
        'The list of projects is what the machine stated; the page names the fact that moved.');
    }
    paintIndex(answer);
  }

  // A read taken because the page just wrote something: the page's own loop is
  // the only place that names a machine that went quiet, so a read asked outside
  // the loop names it here as well.
  async function refreshLive(noStore) {
    try {
      await refresh(noStore);
    } catch (failed) {
      offline(failed);
    }
  }

  // §9.3: the stream beside the doors says when a project moved. The page
  // re-presses what stands visible then - the room it is standing on, from
  // a reading that may not be answered from a kept copy - beside the index
  // reading that paints the dropdown and the facts. The page's own polling
  // stays the fallback either way.
  const liveInvalidator = function (slug, frame) {
    void refreshLive(true);
    // A frame for the project this page stands on moves the rows the person
    // is looking at, not only the dropdown: the visible room is re-asked the
    // way a press beside a write is - held copy put away, the ask riding
    // no-store - and this project's list doors leave the browser's own cache,
    // so a later filter switch cannot replay a reading taken before the move.
    if (state.project && String(slug || '') === state.project) {
      evictRoomDoors(state.project);
      if (state.tab === 'issues') roomRefresh(issuesRoom, 'issues');
      else if (state.tab === 'pulls') roomRefresh(pullsRoom, 'pulls');
      else if (state.tab === 'code') roomRefresh(roomNamed('code'), 'code');
    }
  };

  // Where the page stands while the machine is not answering. Two different
  // things look alike here and are not the same fact: a machine that refuses a
  // key stands, and a machine that is absent does not.
  function offline(failed) {
    if (ui.headUnknown) ui.headUnknown();
    const keyState = window.olsKeyState;
    const served = (keyState && keyState.state !== 'unknown' && keyState.state !== 'checking') ||
      lastIndexRead.status === 401 || lastIndexRead.status === 403;
    if (!offlineMentioned) {
      offlineMentioned = true;
      ui.log(served
        ? 'The machine stopped answering this page: a machine stands here and went quiet at its doors.'
        : 'The machine stopped answering: there is no server to read here.', 'err');
    }
    say(served
      ? 'a machine is here: ' + (keyState ? keyState.text : 'its doors refused this page')
      : 'not connected · retrying at the pace this page keeps',
      served ? 'warn' : 'err',
      served
        ? 'This machine stands here and went quiet: after a few refusals at its doors in a row it answers nothing at all, so a read can look like no machine. A quiet door opens again by itself once enough time passes without a new refusal.'
        : 'No server to read. The page keeps the last reading it was given, and asks again at its own pace.');
  }

  // ---------- the sheet: a project being added ----------
  // The sheet is the one place this page writes something the machine has not
  // been told. It answers with the machine's own words, and a refusal keeps the
  // sheet open so the words can be read against the fields.
  let mode = 'new';
  let sheetOpen = false;
  let sending = false;

  const fieldsNew = E('ps-fields-new');
  const fieldsLink = E('ps-fields-link');
  const modeNewBtn = E('ps-mode-new');
  const modeLinkBtn = E('ps-mode-link');
  const newName = E('ps-new-name');
  const newBranch = E('ps-new-branch');
  const newReadme = E('ps-new-readme');
  const newLicense = E('ps-new-license');
  const linkUrl = E('ps-link-url');
  const slugPreview = E('proj-slug-preview');
  const linkEcho = E('ps-link-echo');

  // The other form of the same ask, and the one this machine's door can carry
  // today: projects/adopt takes on a folder that already stands on this machine
  // under {path}, and answers a link with its own words about the fetch client
  // it has not been given yet. No door here will list folders for this page to
  // pick from, so no picker is drawn - one plain field, the words the person
  // aims it at, sent as the machine reads them.
  //
  // The branch and pull-request dials the sheet was drawn with belong to a
  // reading of an address this door cannot take: the ask is the folder or the
  // link, and nothing else. A control whose door is not there is not drawn, so
  // those two rows are taken off the page rather than left to answer nothing.
  const linkFolder = (function () {
    if (!fieldsLink) return null;
    const row = el('div', 'ps-field-row');
    row.id = 'ps-row-link-folder';
    const label = el('label', null, 'folder on this machine');
    label.htmlFor = 'ps-link-folder';
    label.title = 'A folder that already stands on this machine, with a repository in it. The machine takes that folder on as one of its own projects; the folder keeps standing where it is.';
    const input = el('input');
    input.id = 'ps-link-folder';
    input.type = 'text';
    input.title = 'Write the whole path as this machine writes it - "C:\\Projects\\its-name". The folder\'s own name is the name the project is kept under. Where no folder stands there, the machine says so, and nothing is registered.';
    row.appendChild(label);
    row.appendChild(input);
    fieldsLink.insertBefore(row, fieldsLink.firstChild);
    const branchRow = E('ps-row-link-branch');
    const prRow = E('ps-row-link-pr');
    if (branchRow) branchRow.hidden = true;
    if (prRow) prRow.hidden = true;
    return input;
  })();

  function pickMode(words) {
    mode = words === 'link' ? 'link' : 'new';
    const linkChosen = mode === 'link';
    if (fieldsNew) fieldsNew.hidden = linkChosen;
    if (fieldsLink) fieldsLink.hidden = !linkChosen;
    if (modeNewBtn) modeNewBtn.className = 'ps-mode' + (linkChosen ? '' : ' ps-mode-on');
    if (modeLinkBtn) modeLinkBtn.className = 'ps-mode' + (linkChosen ? ' ps-mode-on' : '');
    drawModeFields();
    if (sheetOpen) focusFirstField();
  }

  // The name this machine would keep the project under, shown as the words are
  // typed - so a person sees the name the machine would answer to before the
  // sheet is sent. Nothing is written anywhere here.
  //
  // This is ProjectService.cpp's proposal() carried over byte for byte, because
  // a preview that differs from the machine's naming is a preview that lies:
  // lowercase, each space becoming a dash (a run of spaces is a run of dashes -
  // nothing is collapsed), dots and underscores kept as the slug may wear them,
  // and any other character simply dropped, not turned into a dash. Leading
  // dots come off first, then the bound of 64, and only then a dot or a dash
  // the end wears - that order is the module's, and it is what makes a name cut
  // at the 64th character land 63 characters long on the disk.
  function slugOf(words) {
    const given = String(words || '');
    let slug = '';
    for (let at = 0; at < given.length; ++at) {
      let code = given.charCodeAt(at);
      if (code >= 65 && code <= 90) code += 32; // A-Z become a-z
      if (code === 32) code = 45; // a space is a dash
      const plain = (code >= 97 && code <= 122) || (code >= 48 && code <= 57) ||
        code === 46 || code === 95 || code === 45; // . _ -
      if (plain) slug += String.fromCharCode(code);
    }
    while (slug.length && slug.charAt(0) === '.') slug = slug.slice(1);
    if (slug.length > 64) slug = slug.slice(0, 64);
    while (slug.length &&
      (slug.charAt(slug.length - 1) === '.' || slug.charAt(slug.length - 1) === '-')) {
      slug = slug.slice(0, -1);
    }
    return slug;
  }

  // The link, read back before anything is sent: the host and the owner and the
  // repository are what a person aimed at, and are shown as the page read them.
  function linkParts(text) {
    const words = String(text || '').trim();
    let host = '';
    let path = '';
    if (words.indexOf('://') >= 0) {
      const at = words.indexOf('://');
      const rest = words.slice(at + 3);
      const cut = rest.indexOf('/');
      host = cut < 0 ? rest : rest.slice(0, cut);
      path = cut < 0 ? '' : rest.slice(cut + 1);
      if (host.charAt(0) === '@') host = host.slice(1);
    } else {
      // The short form a person pastes most: a name, a colon, and a path.
      const colon = words.indexOf(':');
      const at = words.indexOf('@');
      if (colon > 0 && at >= 0 && at < colon) {
        host = words.slice(at + 1, colon);
        path = words.slice(colon + 1);
      } else if (colon > 0 && words.indexOf('/') < colon) {
        host = words.slice(0, colon);
        path = words.slice(colon + 1);
      }
    }
    host = host.toLowerCase();
    if (path.endsWith('.git')) path = path.slice(0, -4);
    const parts = [];
    for (const part of path.split('/')) {
      if (part) parts.push(part);
    }
    const repo = parts.length >= 2 ? parts[parts.length - 2] + '/' + parts[parts.length - 1] : '';
    let kind = '';
    if (host.indexOf('github') >= 0) kind = 'github';
    else if (host.indexOf('gitlab') >= 0) kind = 'gitlab';
    else if (host.indexOf('gitea') >= 0) kind = 'gitea';
    return { host: host, repo: repo, kind: kind, scheme: words.indexOf('://') >= 0 };
  }

  function drawLinkEcho() {
    if (!linkEcho) return;
    const parts = linkParts(fieldText(linkUrl));
    linkEcho.textContent = '';
    if (!parts.host) {
      const none = el('div', 'ps-quiet');
      none.textContent = 'these words do not read as a link: no host is named';
      none.title = 'The page shows what it read before anything is sent. Words that name no host are sent as they stand, and the machine answers in its own words.';
      linkEcho.appendChild(none);
      return;
    }
    const line = el('div', 'ps-echo-said');
    line.textContent = 'read as: ' + (parts.kind ? parts.kind + ' at ' : 'a place at ') + parts.host +
      (parts.repo ? ' · owner/repo ' + parts.repo : ' · the path does not read as owner/repository');
    line.title = 'What was read out of the link. The machine answers for itself; this page only shows what was aimed at.';
    linkEcho.appendChild(line);
    const more = el('div', 'ps-echo-note');
    more.textContent = 'the branches and the pull requests that live there are read by the machine when its reading of an address lands.';
    more.title = 'Picking a branch or a pull request from what lives at the link comes with the reading the machine takes of that address; this phase asks for none.';
    linkEcho.appendChild(more);
  }

  function drawSlugPreview() {
    if (!slugPreview) return;
    const words = fieldText(newName);
    const slug = slugOf(words);
    slugPreview.textContent = '';
    const line = el('div', slug ? 'ps-echo-said' : 'ps-quiet');
    if (slug) {
      line.textContent = 'this machine would keep it as: ' + slug;
      line.title = 'The folder name this machine would keep the project under, from the words just typed. Nothing is written until the sheet is sent.';
    } else if (words) {
      // A name of nothing but punctuation proposes no folder at all, and the
      // module will not make one: the preview says so rather than standing
      // blank while a person waits for the sheet to be sent.
      line.textContent = 'these words leave no name: this machine keeps a folder under letters, digits, dots, underscores, and dashes';
      line.title = 'The machine names the folder from these words, and these words name nothing. It would refuse the ask rather than make a folder of nothing.';
    } else {
      line.textContent = 'no name written yet';
      line.title = 'The folder name this machine would keep the project under, from the words just typed. Nothing is written until the sheet is sent.';
    }
    slugPreview.appendChild(line);
  }

  function drawModeFields() {
    if (mode === 'link') drawLinkEcho();
    else drawSlugPreview();
  }

  // The sheet's fields take the page's place: the first field of the mode in
  // use is where a person starts typing, and the hand that opened the sheet is
  // where that place returns to when the sheet is put away.
  function focusFirstField() {
    const host = mode === 'link' ? fieldsLink : fieldsNew;
    if (!host) return;
    const first = host.querySelector('input');
    if (first) first.focus();
  }

  function openSheet() {
    if (sheetOpen || !sheet) return;
    sheetOpen = true;
    sheet.className = 'ps-sheet ps-open';
    if (backdrop) backdrop.hidden = false;
    pickMode(mode);
    if (sheetError) sheetError.hidden = true;
    focusFirstField();
    if (sheet) sheet.title = 'Adding a project: either this machine starts a new one, or takes on one that already has files - a folder standing on this machine, or a link a repository lives at. The words this sheet answers with are what the machine said. ESC, or releasing the page behind the sheet, puts it away without sending anything.';
  }

  function closeSheet() {
    if (!sheetOpen || !sheet) return;
    sheetOpen = false;
    sheet.className = 'ps-sheet';
    if (backdrop) backdrop.hidden = true;
    if (sheetError) sheetError.hidden = true;
    if (addBtn) addBtn.focus();
  }

  function refuseSheet(words) {
    if (!sheetError) return;
    sheetError.hidden = false;
    sheetError.textContent = '';
    const line = el('div', 'ps-refusal');
    line.textContent = words;
    line.title = 'The words the machine gave about why this was turned down. The sheet stays open so the fields can be read against them.';
    sheetError.appendChild(line);
  }

  async function sendSheet() {
    if (!sheetOpen || sending) return;
    sending = true;
    try {
      if (mode === 'link') await sendAdopt();
      else await sendNew();
    } finally {
      sending = false;
    }
  }

  async function sendNew() {
    const name = fieldText(newName);
    if (!name) {
      refuseSheet('no name was written, and a project this machine starts needs one.');
      ui.log('a new project was not sent: no name was written', 'warn');
      return;
    }
    if (sheetError) sheetError.hidden = true;
    // The shape the door reads: one ask, its fields at its top level, named as
    // the door's own manifest names them (OpenLocalServices.cpp projects/new:
    // name, default_branch?, with_readme?, license?). A choice tucked under an
    // object the door never reads is a choice silently dropped, so every field
    // stands where the door looks for it, and only the fields this sheet states
    // are sent: what is left out, the module decides. The slug is not sent -
    // the name's folder is the module's naming, and this page only shows the
    // proposal beside the field.
    const body = { name: name };
    const branch = fieldText(newBranch);
    if (branch) body.default_branch = branch;
    if (newReadme) body.with_readme = newReadme.checked;
    if (newLicense) {
      const license = String(newLicense.value || '');
      // "none" is a stated choice, not an absent field: the door reads an empty
      // word for it and carries no license, which is what the sheet's none says.
      body.license = license === 'none' ? '' : license;
    }
    ui.log('sending a new project to the machine: ' + name, 'wait');
    await answerSheet('a new project', await ui.apiR(kProjectsNew, 'POST', body));
  }

  async function sendAdopt() {
    const folder = fieldText(linkFolder);
    const url = fieldText(linkUrl);
    if (!folder && !url) {
      refuseSheet('nothing was written: the machine takes on either a folder that stands on this machine, or the link a repository lives at.');
      ui.log('a project was not taken on: no folder and no link were written', 'warn');
      return;
    }
    if (folder && url) {
      // The door takes one form at a time, and this page does not choose for a
      // person which of the two they meant to hand over.
      refuseSheet('one form at a time: put away either the folder or the link, so the machine knows which project to take on.');
      ui.log('a project was not taken on: both a folder and a link were written', 'warn');
      return;
    }
    if (sheetError) sheetError.hidden = true;
    // The ask in the shape the door reads it: {path} for a folder standing on
    // this machine, which the door carries today, or {url} for a link, which the
    // door answers with its own words about the fetch client it has not been
    // given yet - and this page repeats those words rather than dressing the
    // refusal up as a success. Nothing else is sent: the host, the owner and the
    // branch this sheet once read out of the link are not fields the door takes,
    // and a field a door never reads is a choice silently dropped.
    const body = folder ? { path: folder } : { url: url };
    ui.log(folder ? 'taking on the folder at ' + oneLiner(folder, 120)
      : 'taking in a project from ' + oneLiner(url, 120), 'wait');
    await answerSheet(folder ? 'the folder at ' + oneLiner(folder, 120)
      : 'a project from that link', await ui.apiR(kProjectsAdopt, 'POST', body));
  }

  // The answer, in the machine's own words: a yes is followed by a fresh read
  // of the list this page draws, the naming of the project just made, and the
  // putting of it into use; a no keeps the sheet open and puts the words where
  // they can be read against the fields. The ask carries its own answer,
  // status, and words - the sheet repeats this ask's, not a pair another ask
  // may have written over.
  async function answerSheet(what, read) {
    const answer = read.answer;
    if (!answer) {
      const why = read.error || ui.askWhy(read);
      ui.log('the machine did not answer about ' + what + ': ' + why, 'err');
      refuseSheet(oneLiner(why, 200));
      ui.toastPush(oneLiner(why, 180), 'err');
      return;
    }
    const words = String(answer.words || 'the machine said nothing about what it did');
    ui.log((answer.did ? '' : 'the machine refused: ') + words, answer.did ? 'ok' : 'err');
    ui.toastPush(oneLiner(words, 180), answer.did ? 'ok' : 'err', 12);
    if (!answer.did) {
      refuseSheet(words);
      return;
    }
    // The write changed the machine's own state: nothing held of it is still
    // what it is, so the rooms are asked again when next they are painted.
    heldRooms = {};
    closeSheet();
    await refreshLive();
    const made = answer.slug ? String(answer.slug) : '';
    if (!made) {
      say('the machine made ' + what + ' but did not name it: the dropdown shows what it lists now.', 'warn',
        'A project the machine made and did not name is not guessed at from a list that changed.');
      return;
    }
    let offered = false;
    for (const project of state.listed) {
      if (String(project.slug || '') === made) offered = true;
    }
    if (offered) {
      setProject(made, true);
      ui.log('the project just made is the one in use now: ' + made, 'ok');
      return;
    }
    say('the machine made ' + what + ' and did not name it in the list it sends: the dropdown names what it lists now.', 'warn',
      'A project the machine made and did not name is not guessed at from a list that changed.');
  }

  // Changing the dropdown: the rooms are drawn from what the page holds, and
  // then checked against the machine at its own pace. Nothing here re-reads a
  // heavy door in a page.
  function setProject(slug, quietly) {
    const wanted = String(slug || '');
    let real = '';
    for (const project of state.listed) {
      if (String(project.slug || '') === wanted) real = wanted;
    }
    // A deep link names a project the index has not been painted from yet:
    // the watch stands on the name the address carried, so the stream is
    // open from the first moment of a landed page - a frame for this slug
    // also repaints the rooms once the index lands the name.
    const watched = real || wanted;
    if (real) deepLinkProject = real;
    if (wanted) deepLinkProject = wanted;
    if (real !== state.project) {
      // The rooms of another project are painted only after the walk this room
      // was standing on has come down: a page of the old project's history
      // landing beside the new project's rooms - and a filter press reading the
      // new project's name against the old file's path - is the one thing a
      // project switch must never leave standing.
      histStandDown();
      state.project = real;
      dropdown.value = real;
      paintAddress();
      for (const room of ROOMS) {
        if (room.tab === state.tab) paintRoom(room);
      }
      paintFacts();
    }
    // §9.3: the project this page now stands on is watched, not polled -
    // the stream beside the doors names the moment its stamps move, and
    // this page re-presses only its own visible rooms then.
    if (watched) ui.watchProject(watched, liveInvalidator);
    if (!quietly && real !== wanted && wanted) {
      say('that project is not one this machine lists: the dropdown keeps what it names.', 'warn',
        'A name in an address that the machine does not list is not drawn as a project. The page says which happened.');
    }
  }

  // ---------- the page's hands, wired once ----------
  ui.onPress(addBtn, function () {
    if (sheetOpen) closeSheet();
    else openSheet();
  });
  for (const room of ROOMS) {
    ui.onPress(room.tabBtn, function () {
      if (state.tab !== room.tab) showRoom(room);
    });
  }
  ui.onPress(modeNewBtn, function () {
    pickMode('new');
  });
  ui.onPress(modeLinkBtn, function () {
    pickMode('link');
  });
  ui.onPress(sheetSubmit, sendSheet);
  ui.onPress(sheetClose, closeSheet);
  if (backdrop) {
    ui.onPress(backdrop, function () {
      if (sheetOpen) closeSheet();
    });
  }
  dropdown.addEventListener('change', function () {
    setProject(String(dropdown.value || ''), false);
  });
  if (newName) {
    newName.addEventListener('input', drawSlugPreview);
    newName.addEventListener('change', drawSlugPreview);
  }
  if (linkUrl) {
    linkUrl.addEventListener('input', drawLinkEcho);
    linkUrl.addEventListener('change', drawLinkEcho);
  }
  const metaSave = E('ps-meta-save');
  if (metaSave) ui.onPress(metaSave, writeMeta);
  document.addEventListener('keydown', function (event) {
    if (!event || String(event.key || '') !== 'Escape') return;
    if (sheetOpen) closeSheet();
    else closeRoomAsk();
  });

  async function writeMeta() {
    if (!state.project) {
      ui.toastPush('no project is open here, so there is nothing about a project to write', 'warn');
      return;
    }
    const slug = state.project;
    const body = {};
    const nameInput = E('ps-meta-name');
    const name = fieldText(nameInput);
    if (name) body.name = name;
    const description = fieldText(E('ps-meta-description'));
    if (description) body.description = description;
    const licenseSelect = E('ps-meta-license');
    if (licenseSelect) {
      const license = String(licenseSelect.value || '');
      if (license) body.license = license === 'none' ? null : license;
    }
    const planHost = E('ps-planned');
    const marks = planHost ? planHost.querySelectorAll('input[type=checkbox]') : [];
    if (marks.length) {
      const planned = [];
      for (const row of planHost.querySelectorAll('.ps-plan-row')) {
        const text = row.querySelector('span.ps-plan-text');
        if (!text) continue;
        const check = row.querySelector('input[type=checkbox]');
        planned.push({ text: String(text.textContent || ''), done: !!check && check.checked });
      }
      if (planned.length) body.planned_features = planned;
    }
    if (!body.planned_features && !body.name && !body.description && !('license' in body)) {
      ui.toastPush('nothing was written into these fields, so nothing was asked of the machine', 'warn');
      return;
    }
    ui.log('writing what this page was told about ' + slug + ' to the machine', 'wait');
    // The ask carries its own answer, status, and words: what the door said is
    // what the page repeats, whatever else was asking beside it.
    const wrote = await ui.apiR(roomDoor('settings'), 'POST', { project: slug, patch: body });
    // The door wants its fields inside a patch, under the project they belong to.
    const answer = wrote.answer;
    if (!answer) {
      const why = wrote.error || ui.askWhy(wrote);
      ui.log('the machine did not answer about those facts: ' + why, 'err');
      ui.toastPush(oneLiner(why, 180), 'err');
      return;
    }
    const words = String(answer.words || 'the machine said nothing about what it did');
    ui.log((answer.did ? '' : 'the machine refused: ') + words, answer.did ? 'ok' : 'err');
    ui.toastPush(oneLiner(words, 180), answer.did ? 'ok' : 'err', 12);
    if (!answer.did) return;
    // The write changed the machine's own state, so the room is read again and
    // the panel shows the machine's newest statement rather than the fields.
    heldRooms = {};
    for (const room of ROOMS) {
      if (room.tab === 'settings') await askRoom(room);
    }
  }

  function roomDoor(words) {
    for (const room of ROOMS) {
      if (room.tab === words && room.door) return room.door + '?project=' + encodeURIComponent(state.project);
    }
    return '';
  }

  // ---------- run strip ----------
  // The strip under the viewer's head presses the build door for the one file
  // this column holds open, and then keeps one address - the status door with
  // the job's name - until the machine says the build ended. What the strip
  // wears is what that door answered last: the state it stated, the step it
  // named, and once ended the code it came out with and the length it spent.
  // Nothing here counts a moment of its own while the machine is answering.
  const kBuildDoor = '/project/api/build';
  const kBuildStatusDoor = '/project/api/build-status';
  const kBuildStopDoor = '/project/api/build-stop';
  const kRunBeatMs = 1000;
  // The strip's own patience with a status door that answers nothing: past
  // this many failed beats in a row, or this long past the press that named
  // the job, the strip settles to its own red and stops asking. The machine
  // may well still be building — the page only states that it lost the answer.
  const kRunLostBeats = 5;
  const kRunLostMs = 120000;

  const runStrip = {
    job: '',
    path: '',
    state: '',
    stage: '',
    beat: null,
    askedAt: 0,
    ended: false,
    stopWords: '',
    rows: [],
    selected: -1,
    more: false,
    // Which job the beats belong to. A new press bumps this, and an answer of
    // the job before it — still travelling when the new one was named — is
    // dropped rather than painted onto the strip that no longer describes it.
    gen: 0,
    // Consecutive beats the status door answered with nothing.
    noAnswer: 0,
    // A press of the build door still travelling: one hand answers at a time.
    asking: false,
    // Set once the strip has given up on a job and said so in its own red.
    lost: false,
  };

  function runStripOn() {
    return !!E('code-run-build') && !!E('code-run-state');
  }

  // The button's own words: a build standing is asked to go by the same hand
  // that started it, so one control holds both states and says which it is in.
  function paintRunHand() {
    const hand = E('code-run-build');
    if (!hand) return;
    const standing = !!runStrip.job && !runStrip.ended;
    hand.textContent = standing ? 'stop' : 'build';
    hand.title = standing
      ? 'Ask this build to go: the stop door ends its whole tree, and the status door answers it as cancelled from there on.'
      : 'Compile and run this file: the build door is asked for the file open in this column, at the machine\'s own toolchain for its extension. While it stands, the state beside the button says how far it got; press again to ask a standing build to go.';
  }

  // The state line wears the status door's own words: running with the step it
  // named, ended with the code and the length it stated, cancelled as cancelled.
  // A fact the answer carried no field for is not written here at all.
  function paintRunState() {
    const host = E('code-run-state');
    if (!host) return;
    host.textContent = '';
    if (!runStrip.job) return;
    const span = el('span');
    let words = '';
    let kind = 'wait';
    if (runStrip.state === 'running') {
      words = 'running…' + (runStrip.stage ? ' · ' + runStrip.stage : '');
    } else if (runStrip.ended) {
      if (runStrip.state === 'cancelled') {
        words = 'cancelled';
        kind = 'warn';
      } else if (typeof runStrip.exitCode === 'number') {
        words = 'exit ' + runStrip.exitCode +
          (typeof runStrip.durationMs === 'number' ? ' · ' + runStrip.durationMs + 'ms' : '');
        kind = runStrip.exitCode === 0 ? 'ok' : 'err';
      } else {
        words = runStrip.state;
        kind = 'err';
      }
      if (runStrip.timedOut) words += ' · timed out';
    }
    span.textContent = words;
    // The colour is the kind class the page's own sheet styles; a word the
    // state row wears is stated by that class alone.
    span.className = 'ps-run-' + kind;
    if (runStrip.lost) {
      span.title = 'The status door answered nothing for job ' + runStrip.job +
        ' — ' + runStrip.noAnswer + ' beats of silence, or past ' + kRunLostMs +
        'ms from the press that named it — and this page stopped asking. The build may still stand on the machine; pressing build asks for a new job.';
    }
    host.appendChild(span);
  }

  function stopRunBeat() {
    if (runStrip.beat !== null) {
      clearTimeout(runStrip.beat);
      runStrip.beat = null;
    }
  }

  // One beat of the page's own pace: the status door read with no kept copy,
  // because a build is exactly the thing that has changed since the last read.
  async function askRunStatus() {
    if (!runStrip.job) return null;
    let answer = null;
    try {
      answer = await ui.api(kBuildStatusDoor + '?job=' + encodeURIComponent(runStrip.job), 'GET', undefined, { cache: 'no-store' });
    } catch (failedRead) {
    }
    return answer;
  }

  function runBeat() {
    runStrip.beat = null;
    if (!runStrip.job || runStrip.ended) return;
    void readRunStatus(runStrip.gen);
  }

  // One read of the status door, painted against the job it was asked with.
  // The generation is taken by the caller and checked again after the await:
  // an answer of a job this strip has moved off is a stale truth, and the
  // strip wears the newest job it named instead.
  async function readRunStatus(gen) {
    const answer = await askRunStatus();
    if (gen !== runStrip.gen) return;
    if (!answer) {
      if (!runStrip.job || runStrip.ended) return;
      runStrip.noAnswer += 1;
      // Five beats of nothing, or two minutes past the press, is a door that
      // is not going to answer this job: the strip settles, says so in its own
      // red, and stops asking.
      if (runStrip.noAnswer >= kRunLostBeats ||
        (runStrip.askedAt > 0 && Date.now() - runStrip.askedAt >= kRunLostMs)) {
        settleRunLost();
        return;
      }
      // Short of that the strip says which door has gone quiet — one note, not
      // one more per beat, so the host is cleared before it — keeps the words
      // it last read beside that note, and keeps its beat: a build may still
      // stand behind the silence.
      const host = E('code-run-state');
      if (host) {
        paintRunState();
        const why = el('span', 'ps-run-warn', 'the status door gave no answer');
        why.title = 'Asked at ' + kBuildStatusDoor + ' for job ' + runStrip.job +
          ' and nothing came back; the strip keeps asking at its own second, and gives up after ' +
          kRunLostBeats + ' beats of silence or ' + kRunLostMs + 'ms from the press.';
        host.appendChild(why);
      }
      if (!runStrip.ended) runStrip.beat = setTimeout(runBeat, kRunBeatMs);
      return;
    }
    runStrip.noAnswer = 0;
    paintRunFrom(answer, gen);
  }

  // The strip's own patience ending: the beat stops, the strip wears its red,
  // and the hand goes back to 'build' because this page is done asking about
  // that job. What the machine is doing with it is not stated here — only that
  // the answer was lost.
  function settleRunLost() {
    stopRunBeat();
    runStrip.ended = true;
    runStrip.lost = true;
    runStrip.state = 'status lost';
    runStrip.stage = '';
    delete runStrip.exitCode;
    delete runStrip.timedOut;
    delete runStrip.durationMs;
    paintRunState();
    paintRunHand();
    ui.log('the status door answered nothing for job ' + runStrip.job +
      '; this page stops asking. The build may still stand on the machine — ' +
      'only the answer was lost.', 'err');
  }

  // The status door's answer, painted as it stated it: the state, the step, and
  // once ended the code, the length, the compiler's rows, and its raw streams.
  function paintRunFrom(answer, gen) {
    if (gen !== runStrip.gen) return;
    runStrip.state = String(answer.state || '');
    runStrip.stage = answer.stage ? String(answer.stage) : '';
    const running = runStrip.state === 'running';
    runStrip.ended = !running;
    if (typeof answer.exit_code === 'number') runStrip.exitCode = Number(answer.exit_code);
    else delete runStrip.exitCode;
    if (answer.timed_out === true) runStrip.timedOut = true;
    else delete runStrip.timedOut;
    if (typeof answer.duration_ms === 'number') runStrip.durationMs = Number(answer.duration_ms);
    else delete runStrip.durationMs;
    paintRunState();
    paintRunHand();
    paintRunStreams(answer);
    paintProblems(answer);
    if (!running) {
      stopRunBeat();
      const outcome = runStrip.state === 'done' ? 'ok' : (runStrip.state === 'cancelled' ? 'warn' : 'err');
      ui.log('the build of ' + (answer.path ? String(answer.path) : runStrip.path) +
        ' answered: ' + stateWordsOf(answer), outcome);
      return;
    }
    runStrip.beat = setTimeout(runBeat, kRunBeatMs);
  }

  function stateWordsOf(answer) {
    const said = String(answer.state || 'a state the door did not name');
    if (typeof answer.exit_code === 'number') return said + ' · exit ' + answer.exit_code;
    if (answer.timed_out === true) return said + ' · timed out';
    return said;
  }

  // The results block: what the machine stated of its streams, drawn as it
  // stated them. A stream with no word says so; a stream the door marked cut
  // says where it stopped short. Nothing is restated or tidied here.
  function paintRunStreams(answer) {
    const host = E('ps-run-results');
    if (!host) return;
    host.hidden = false;
    for (const which of ['stdout', 'stderr']) {
      const box = E('ps-run-' + which);
      if (!box) continue;
      box.textContent = '';
      const words = typeof answer[which] === 'string' ? answer[which] : '';
      const head = el('div', 'ps-run-stream-name', which);
      head.title = 'The machine\'s own words on this stream, as the status door sent them.';
      box.appendChild(head);
      if (words) {
        box.appendChild(el('pre', 'ps-run-stream-body', words));
      } else {
        box.appendChild(el('div', 'ps-quiet', 'the machine stated no words on this stream'));
      }
      if (answer[which + '_truncated'] === true) {
        box.appendChild(el('div', 'ps-code-cut', 'the machine cut this stream: more was printed than is shown'));
      }
    }
  }

  // Press of the strip's hand. A standing build is asked to go by it; anything
  // else asks the build door for the file open in this column - refused with a
  // toast when none is open, because a build names one file and there is none.
  // The press carries the path the machine was asked with: later answers about
  // the job are stated beside that name, not beside whatever the room opened in
  // between. One press at a time: the button and its key share this hand, and a
  // second press while a door is still answering would ask the same thing twice.
  async function pressRunStrip() {
    if (!runStripOn()) return;
    if (runStrip.asking) return;
    runStrip.asking = true;
    try {
      if (runStrip.job && !runStrip.ended) {
        await stopTheBuild();
        return;
      }
      if (!state.project || !view.path) {
        ui.toastPush('no file is open in this room, so there is nothing to build', 'warn');
        return;
      }
      const body = { project: state.project, path: view.path };
      // The build ask carries its own answer, status, and words: the reason a
      // build was not taken is this ask's reason, whatever else was asking.
      const asked = await ui.apiR(kBuildDoor, 'POST', body);
      const answer = asked.answer;
      if (!answer || !answer.job) {
        const why = (answer && answer.words) ? String(answer.words) : (asked.error || ui.askWhy(asked));
        ui.log('the machine refused that build: ' + why, 'err');
        ui.toastPush(oneLiner(why, 180), 'err', 12);
        return;
      }
      stopRunBeat();
      runStrip.job = String(answer.job);
      runStrip.path = body.path;
      runStrip.state = String(answer.state || 'running');
      runStrip.stage = '';
      runStrip.ended = false;
      delete runStrip.exitCode;
      delete runStrip.timedOut;
      delete runStrip.durationMs;
      runStrip.askedAt = Date.now();
      // A new job is a new truth: the beats of the job before it, and any
      // answer of theirs still travelling, are stale from here on.
      runStrip.gen += 1;
      runStrip.noAnswer = 0;
      runStrip.lost = false;
      paintRunState();
      paintRunHand();
      ui.log('asked the machine to build ' + view.path + '; it named the job ' + runStrip.job, 'wait');
      // The first reading comes at once - a quick build should not wait a whole
      // beat to have its outcome shown.
      void readRunStatus(runStrip.gen);
    } finally {
      runStrip.asking = false;
    }
  }

  // Asking a standing build to go: the stop door ends its tree and says so; the
  // status door answers it as cancelled from there on, which is what the strip
  // then wears. A refusal keeps the strip standing exactly as it stood.
  async function stopTheBuild() {
    const job = runStrip.job;
    // The stop ask carries its own answer, status, and words: the reason a stop
    // was not taken is this ask's reason, whatever else was asking.
    const asked = await ui.apiR(kBuildStopDoor, 'POST', { job: job });
    const answer = asked.answer;
    if (!answer || !answer.did) {
      const why = (answer && answer.words) ? String(answer.words) : (asked.error || ui.askWhy(asked));
      ui.log('the machine refused to stop that build: ' + why, 'err');
      ui.toastPush(oneLiner(why, 180), 'err', 12);
      return;
    }
    runStrip.stopWords = String(answer.words || '');
    ui.log(runStrip.stopWords, 'ok');
  }

  function armRunStrip() {
    if (!runStripOn()) return;
    ui.onPress(E('code-run-build'), function () {
      void pressRunStrip();
    });
    document.addEventListener('keydown', function (event) {
      if (!event || state.tab !== 'code' || sheetOpen) return;
      const key = String(event.key || '');
      const held = event.ctrlKey || event.metaKey;
      // The editor's own keys: Ctrl+Enter and F5 press the strip's hand, while
      // the buffer keeps focus so typing stays typing.
      if (key === 'F5' || (held && key === 'Enter')) {
        event.preventDefault();
        void pressRunStrip();
      }
    });
  }

  // ---------- problems ----------
  // The rows the status door parsed from the compiler's lines: file, line, an
  // optional column and severity, the code the compiler named, and its words.
  // A row is pressed to open the file it names - if this project lists it - and
  // anchor the line; a row naming no file of the project says so instead. The
  // chosen row wears itself as chosen, and where the editor stands on that same
  // file its lines wear a mark under them.
  function paintProblems(answer) {
    const region = E('ps-problems');
    if (!region) return;
    region.hidden = false;
    // The row a press chose belongs to the person who pressed it, not to the
    // beat: while the build still stands, a choice whose row is still there
    // under the same name stays chosen. An answer that moved the rows, or one
    // that ended the build, is another answer, and the choice comes off.
    const pickedBefore = runStrip.selected >= 0 ? runStrip.rows[runStrip.selected] : null;
    runStrip.rows = Array.isArray(answer.diagnostics) ? answer.diagnostics : [];
    runStrip.more = answer.more_diagnostics === true;
    if (String(answer.state || '') !== 'running' ||
      !sameProblemRow(runStrip.rows[runStrip.selected], pickedBefore)) {
      runStrip.selected = -1;
    }
    const head = E('ps-problems-head');
    if (head) {
      head.textContent = '';
      const said = el('div', 'ps-note', runStrip.rows.length +
        (runStrip.rows.length === 1 ? ' problem' : ' problems') +
        (answer.tool ? ' · the machine built with ' + String(answer.tool) : ''));
      said.title = 'The rows are the compiler\'s own lines as the door read them; the tool named is what the machine stated answered for this file.';
      head.appendChild(said);
    }
    const list = E('ps-problems-list');
    if (list) {
      list.textContent = '';
      let at = 0;
      for (const row of runStrip.rows) {
        at += 1;
        list.appendChild(problemRow(row, at - 1));
      }
      if (!runStrip.rows.length) {
        quiet(list, 'the machine parsed no problems from these words',
          'The build\'s streams stand below as they came; a line neither family the door reads is named here stays in those streams and is never dropped.');
      } else if (runStrip.more) {
        list.appendChild(el('div', 'ps-code-cut', 'more diagnostics: the machine cut this list at its own bound'));
      }
    }
    const noteHost = E('ps-problems-note');
    if (noteHost) {
      note(noteHost, 'from the build of ' + (answer.path ? String(answer.path) : runStrip.path) +
        ' · job ' + (answer.job ? String(answer.job) : runStrip.job), '',
        'A row names what its compiler named: the file and line it read, and its own words. Pressing a row opens that file at that line.');
    }
    paintRunMarks();
  }

  // Whether the row standing at a place now is the row that stood there when
  // the press was made. The door's own fields decide it, field for field; this
  // page guesses at nothing, so a row the door restated differently is not the
  // row that was chosen.
  function sameProblemRow(row, before) {
    if (!row || !before) return false;
    return String(row.file || '') === String(before.file || '') &&
      Number(row.line) === Number(before.line) &&
      Number(row.col) === Number(before.col) &&
      String(row.code || '') === String(before.code || '') &&
      String(row.msg || '') === String(before.msg || '');
  }

  function problemRow(row, at) {
    const file = String((row && row.file) || '');
    const line = Number(row && row.line) > 0 ? Number(row.line) : 0;
    const col = Number(row && row.col) > 0 ? Number(row.col) : 0;
    const code = row && row.code ? String(row.code) : '';
    const msg = row && row.msg ? String(row.msg) : '';
    const sev = row && row.sev ? String(row.sev) : '';
    let words = (file || 'the machine named no file') + ':' + (line > 0 ? line : '?');
    if (col > 0) words += ':' + col;
    if (code) words += ' ' + code;
    if (msg) words += ' ' + msg;
    const node = el('button', 'ps-problem-row' + (sev === 'warning' ? ' ps-problem-warn' : ''), words);
    node.dataset.at = String(at);
    node.title = 'The compiler\'s line as the door read it' +
      (row && row.path ? '; pressing opens ' + String(row.path) + ' at line ' + (line > 0 ? line : '?') :
        '; this line names no file of the project, so pressing states that instead');
    // The rows are rebuilt on every beat, so a choice that was kept across one
    // wears itself chosen again here rather than only at the press.
    if (at === runStrip.selected) node.classList.add('ps-problem-sel');
    ui.onPress(node, function () {
      pressProblem(at);
    });
    return node;
  }

  // Pressing a row: the path field is the door's own answer - the built file put
  // back where the tree lists it - and only such a name opens. A row naming some
  // other file says so rather than opening nothing or guessing at one. The row
  // wears itself chosen either way, because that press was made.
  async function pressProblem(at) {
    const row = runStrip.rows[at];
    if (!row) return;
    const list = E('ps-problems-list');
    if (list) {
      for (const other of list.querySelectorAll('.ps-problem-sel')) other.classList.remove('ps-problem-sel');
    }
    runStrip.selected = at;
    const nodes = list ? list.querySelectorAll('.ps-problem-row') : [];
    if (nodes[at]) nodes[at].classList.add('ps-problem-sel');
    const wanted = row.path ? String(row.path) : '';
    const line = Number(row.line) > 0 ? Number(row.line) : 0;
    if (!wanted) {
      ui.toastPush('this problem names no file of the project, so there is nothing for this page to open', 'warn');
      return;
    }
    await openFile(wanted, line, view.ref);
    paintRunMarks();
  }

  // The squiggles: where the editor stands on the same file as a row of the
  // last build, the row's whole line wears a wavy under-mark. The marks come
  // from the bundle's own Decoration.mark, kept in a compartment so a new build
  // or an edit re-paints them without rebuilding the editor.
  const runMarks = {
    box: null,
    mark: null,
    set: null,
    empty: null,
    noteEl: null,
  };

  // A browser, or a set of rows, that the marks could not be placed under is
  // said in the row beside the buffer — once, kept where it was said — rather
  // than swallowed. The reading, the rows, and the streams stand whole either
  // way; only the under-marks are missing, and that is what this states.
  function paintRunMarksWhy(why) {
    const status = E('ps-editor-status');
    if (!status) return;
    if (!runMarks.noteEl || !status.contains(runMarks.noteEl)) {
      runMarks.noteEl = el('div', 'ps-note unknown');
      runMarks.noteEl.title = 'The rows above stand as the machine stated them; only the marks under them could not be placed.';
      status.appendChild(runMarks.noteEl);
    }
    runMarks.noteEl.textContent = why;
  }

  function paintRunMarks() {
    if (!editorMode.view || !runMarks.box || !runMarks.mark) return;
    // The marks go under the words the editor holds, so they go under the file
    // the room is standing on. A buffer of another file is not this reading's
    // buffer, and rows of this build mark nothing there.
    if (editorMode.path !== view.path) return;
    // A fresh paint either places the marks or says why it could not: a note
    // from an earlier attempt comes off before this one is tried.
    if (runMarks.noteEl) {
      runMarks.noteEl.remove();
      runMarks.noteEl = null;
    }
    try {
      const doc = editorMode.view.state.doc;
      const made = [];
      for (const row of runStrip.rows) {
        const line = Number(row && row.line) > 0 ? Number(row.line) : 0;
        if (!row || line < 1 || line > doc.lines) continue;
        // Only a row the door put back at this file marks it: a row naming
        // another file belongs to that file's reading, not this buffer's.
        if (String(row.path || '') !== view.path) continue;
        const textLine = doc.line(line);
        // A warning wears itself dimmer than an error: the severity is the
        // compiler's word, and the mark only repeats it.
        made.push(runMarks.mark({ class: 'ps-run-squiggle' + (row.sev === 'warning' ? ' ps-run-squiggle-warn' : '') })
          .range(textLine.from, Math.max(textLine.from, textLine.to - 1)));
      }
      // The compartment holds an extension, and a RangeSet is not one: the marks
      // ride the bundle's own decorations facet, and the empty stands as the
      // array the editor was mounted with. The rows come in the order the door
      // parsed them, which is not the order of a buffer, so the set is asked
      // to sort them rather than refusing a descending pair.
      const marks = made.length
        ? editorMode.cm.view.EditorView.decorations.of(runMarks.set(made, true))
        : runMarks.empty;
      editorMode.view.dispatch({ effects: runMarks.box.reconfigure(marks) });
    } catch (noDeco) {
      paintRunMarksWhy('no marks under the problems: ' +
        String(noDeco && noDeco.message ? noDeco.message : noDeco));
    }
  }

  // ---------- the whole page, in one wake ----------
  async function main() {
    // The bar and the rooms are drawn before any reading arrives, so the page
    // says where it stands while the machine is still answering. The address and
    // this browser's own record come first: an agent or a bookmark lands on
    // exactly the room it aimed at.
    const address = askedInAddress();
    if (address.project) deepLinkProject = address.project;
    const wantedRoom = roomNamed(address.tab) || roomNamed(storedTab());
    showRoom(wantedRoom || ROOMS[0]);
    armRunStrip();
    if (address.project) setProject(address.project, true);
    say('standing by: this machine has not been read here yet.', '',
      'The page is drawn from what it holds and from what it has not yet asked. The first reading comes at once, at the pace this page keeps.');
    // What this browser last read paints the page first, labeled with the
    // moment it was read at. A page that woke beside a quiet machine still
    // shows the rooms it was last told about.
    const kept = ui.lastReading(kIndexDoor);
    if (kept && kept.data && Array.isArray(kept.data.projects)) {
      paintIndex(kept.data);
      say('painted from what this browser last read, at ' + ui.clockMs(kept.at) + '.', '',
        'A replayed reading is labeled with the moment it was read at, because it describes where the machine stood then, not now.');
      if (address.project) setProject(address.project, true);
    }
    await ui.poll(refresh, offline, ui.pollMs);
  }

  main();
})();
