// Image studio: prompts, models, the gallery of finished renders, and reference
// pictures for later runs to start from. The header shows the machine's state;
// while a render runs, the header doubles as its progress bar and the panel
// below repeats it step by step.

const E = ui.E;
const log = ui.log;

const loadedSet = new Set();
const everLoaded = new Set();
let models = [];
// What the model cards last said; see modelsCardSignature.
let modelsCardState = '';
let galleryCardState = '';
// The cards that stood the last time the room was drawn, kept under the ask each
// is the card of: the node itself and a stamp of what it held. A drawing moves
// these cards into the new order, builds the ones that are new, and takes away
// only the asks that are gone - a card that still holds the same thing keeps its
// node, and the picture it had drawn stays painted while it stands.
let galleryCards = new Map();
let galleryItems = [];
// how many records this machine holds, so the gallery note can say when the list
// on screen is only the newest part of them.
let galleryHeld = 0;
// The asks the gallery grouped together, from the last time it was drawn: the
// viewer reads this to put the other runs of an ask in the row under a picture.
let galleryGroups = new Map();
// The pictures marked by the bulk buttons, kept by id so a re-draw of the list
// marks the same ones again. Only pictures on this screen are ever marked.
const markedShots = new Set();
// What this page deleted while it stands. A delete the machine confirmed still
// leaves the record in this client's memory until the next reading of its page
// passes it on; a room-walk that carries such a record back would stand it as a
// ghost whose picture is already gone.
const deletedShots = new Set();
// The pictures on this screen, in the order they are shown: the viewer steps
// through these, so prev and next answer what a person is looking at.
let shownItems = [];
// The render the viewer is showing, and its place in shownItems.
let openShot = '';
let viewerAt = -1;
// Set while the page itself repopulates a dropdown, so the change that rebuild
// causes is not read as a person's choice.
let pageRebuiltChoice = false;
// The room is loaded whole rather than a window of it, so the counts beside the
// model names describe every record this machine keeps. The server still answers a
// page at a time; the client asks for those pages in batches.
const kGalleryPage = 100;
// How many page asks are in flight at once: the asks are independent, so they are
// started together, but a room of thousands is not fired off as one huge set.
const kGalleryRoomBatch = 6;
// Where the room on screen starts, counted from the newest render: the whole room
// is shown, so this stays at the newest and only a room the batch could not reach
// leaves it short of the end.
let galleryOffset = 0;
let refs = [];
// The pictures this run starts from, in the order the model reads them.
let startRefs = [];
// A kept picture this page picked up and carried onto the prompt panel.
let draggedRef = '';
let running = null;
// The model this machine has open, per its own state.
let pickedModel = '';
// The model a run draws with, as last picked on a card; empty means the model
// this machine has open. No other control asks for a model, so everything on
// the page reads this one value.
let pickedChoice = '';
let autoPrompt = '';

const pillKeys = { tiling: 'pill-tiling', sage_attn: 'pill-sage', cpu: 'pill-cpu' };

function findModel(id) {
  for (const model of models) {
    if (model.id === id) return model;
  }
  return null;
}

// The words a model id stands for: its own page, or the id itself when the
// machine no longer offers that model.
function modelNamed(id) {
  const model = findModel(String(id || ''));
  return model ? model.name : String(id || '');
}

// The words a prompt field opens with: the scene this model is documented as
// good at.
function ownPrompt(id) {
  const model = findModel(id);
  return model ? String(model.prompt || '') : '';
}

function modelHint(model) {
  const parts = ['steps ' + model.steps];
  if (model.guidance > 0) parts.push('guidance ' + model.guidance);
  if (Number(model.width) > 0 && Number(model.height) > 0) parts.push('at ' + model.width + ' × ' + model.height);
  if (model.negative) parts.push('usually avoids: ' + model.negative);
  if (model.specialty) parts.push('best at: ' + model.specialty);
  return parts.join('\n');
}

function syncPrompt() {
  const field = E('prompt-field');
  const current = String(field.value);
  if (current.trim() === '' || current === autoPrompt) {
    autoPrompt = ownPrompt(modelChoice());
    field.value = autoPrompt;
  }
}

function applyPrefs(prefs) {
  for (const key in pillKeys) {
    const node = E(pillKeys[key]);
    if (prefs[key] === true) node.classList.add('on');
    else node.classList.remove('on');
  }
  // What this machine remembers as its preferred model drives the run.
  const wanted = String(prefs.model || '');
  if (wanted === pickedChoice) return;
  pickedChoice = wanted;
  if (wanted !== '' && models.length && !findModel(wanted)) {
    log('The model this machine remembered is not on it; the page is back on the model its memory ' +
      'holds.', 'warn');
  }
}

// The page's switches as they apply to this machine: written to the engine, and
// the answer is handed back so the page draws the switches the engine really
// has. A refused write answers with a refusal, not silence.
function savePrefs() {
  const body = { model: modelChoice() };
  for (const key in pillKeys) body[key] = ui.isOn(pillKeys[key]);
  return ui.apiR('/api/prefs', 'POST', body);
}

// What a picture in the strip is called, in words a person reads.
function pictureName(id) {
  for (const ref of refs) {
    if (String(ref.id) === String(id)) return String(ref.name || id);
  }
  for (const item of galleryItems) {
    if (String(item.id) === String(id)) return 'last render';
  }
  return 'imported picture';
}

// How many reference pictures the model in use accepts: 0 for none, stated by
// the machine on the model's card.
function refsOf(model) {
  const found = model || findModel(modelChoice());
  return Number(found.refs || 0);
}

// Put a picture into the strip: it takes the next position; the model's own
// limit is stated, not assumed, and an over-full strip is refused with the
// number it stopped at.
function addToStrip(id) {
  const model = findModel(modelChoice()) || {};
  const limit = Number(model.refs || 0);
  const wanted = String(id);
  if (limit < 1) {
    notePicture('This model takes no reference pictures; nothing was added.');
    log('This model takes no reference pictures', 'err');
    return false;
  }
  for (let at = 0; at < startRefs.length; ++at) {
    if (startRefs[at] === wanted) {
      notePicture('Already in the strip, at position ' + (at + 1) + '.');
      return false;
    }
  }
  if (startRefs.length >= limit) {
    notePicture('The strip is full: this model takes ' + limit +
      ' picture' + (limit === 1 ? '' : 's') + '. Remove one to make room.');
    return false;
  }
  startRefs.push(wanted);
  notePicture('Added to the strip at position ' + startRefs.length + ': ' + pictureName(wanted));
  ui.flash(E('ref-strip'));
  paintRunState();
  return true;
}

// Take a picture off the strip, at every position it sat in.
function takeOffStrip(id) {
  const wanted = String(id);
  const left = [];
  for (const held of startRefs) {
    if (held !== wanted) left.push(held);
  }
  if (left.length === startRefs.length) return false;
  startRefs = left;
  ui.flash(E('ref-strip'));
  paintRunState();
  return true;
}

// One card of the strip: the picture, its position, what it is called, and how
// to remove it.
function stripCard(id, at) {
  const shot = document.createElement('div');
  shot.className = 'strip-shot';
  const place = document.createElement('span');
  place.className = 'strip-position';
  place.textContent = String(at);
  place.title = 'Position ' + at + ' in the order the model reads the pictures.';
  shot.appendChild(place);

  const url = pictureUrl(id);
  const img = document.createElement('img');
  img.setAttribute('alt', 'picture at position ' + at);
  img.title = pictureName(id) + ' · tap to open';
  img.src = ui.fileSrc(url);
  ui.onPress(img, function () { showViewer(url, 'strip picture ' + at + ' · ' + pictureName(id), heldRef(id)); });
  shot.appendChild(img);

  const what = document.createElement('div');
  what.className = 'strip-name';
  what.textContent = pictureName(id);
  what.title = 'What this picture is called, and where it came from.';
  shot.appendChild(what);

  const off = document.createElement('button');
  off.textContent = 'remove';
  off.title = 'Remove this picture from the strip; the run will not start from it.';
  ui.onPress(off, function () {
    if (takeOffStrip(id)) log('Removed from the strip: ' + pictureName(id), 'warn');
  });
  shot.appendChild(off);
  return shot;
}

// Where a picture's bytes live on the machine: kept pictures in the references
// folder, renders in the output folder.
function pictureUrl(id) {
  for (const ref of refs) {
    if (String(ref.id) === String(id)) return ref.url || ('/files/refs/' + ui.encodePath(ref.file));
  }
  for (const item of galleryItems) {
    if (String(item.id) === String(id)) return item.url || ('/files/outputs/' + ui.encodePath(item.file));
  }
  return '/files/refs/' + ui.encodePath(String(id));
}

// The browser's kept state from an earlier visit may name pictures this
// machine no longer holds; those come off the strip before it is drawn again.
function dropMissingStarts() {
  const left = [];
  for (const held of startRefs) {
    if (hasPicture(held)) left.push(held);
  }
  if (left.length === startRefs.length) return;
  for (const gone of startRefs) {
    if (!hasPicture(gone)) log('Removed: this machine no longer holds that picture: ' + pictureName(gone), 'warn');
  }
  startRefs = left;
}

function hasPicture(id) {
  const wanted = String(id);
  for (const ref of refs) {
    if (String(ref.id) === wanted) return true;
  }
  for (const item of galleryItems) {
    if (String(item.id) === wanted) return true;
  }
  return false;
}

// Draw the strip in its own order, with what the model in use means for the
// page: the state line, the closeness field only for a redraw model, and the
// avoid field only where it can do anything.
function paintRunState() {
  dropMissingStarts();
  syncStartRefs();
  const strip = E('ref-strip');
  strip.textContent = '';
  let at = 0;
  for (const id of startRefs) {
    at += 1;
    strip.appendChild(stripCard(id, at));
  }
  const model = findModel(modelChoice()) || {};
  showStartState(model);
  showAvoidRow(model);
  showStrengthRow(model);
  showClipRow(model);
  paintModelCard(model);
}

// The message the page shows when it cannot take a picture.
function notePicture(text) {
  const node = E('picture-note');
  if (node) node.textContent = text;
}

// A model change may lower the picture limit, so pictures no model can read
// come off the strip and are named as gone, not left in an unused strip.
function syncStartRefs() {
  const limit = refsOf(findModel(modelChoice()) || {});
  if (startRefs.length <= limit) return;
  const dropped = startRefs.slice(limit);
  startRefs = startRefs.slice(0, limit);
  for (const id of dropped) log('Removed: the model you picked cannot take it: ' + pictureName(id), 'warn');
}

// What the run will do with the pictures in the strip, in words a person can
// act on: the model's own limit and what it does with a picture.
function showStartState(model) {
  const note = E('start-note');
  const kind = startOf(model);
  const limit = Number(model.refs || 0);
  if (limit < 1) {
    note.textContent = modelChoice() === ''
      ? 'No model open or picked: pick one; its card states what it accepts.'
      : 'The model in use draws from text only; it has no use for a picture.';
    return;
  }
  const limitWords = limit + ' picture' + (limit === 1 ? '' : 's');
  if (startRefs.length === 0) {
    note.textContent = kind === 'edit'
      ? 'Strip empty: this model draws from text · put a picture in the strip above to change it ' +
      'instead · it accepts ' + limitWords
      : 'Strip empty: this model draws from text · put one picture in the strip above to redraw it ' +
      'instead';
    return;
  }
  const named = [];
  let at = 0;
  for (const id of startRefs) {
    at += 1;
    named.push(at + ' ' + pictureName(id));
  }
  if (kind === 'edit') {
    note.textContent = 'Starting from ' + named.join(' · ') + ' · the words say what to CHANGE, not what to draw'
      + ' · this model reads ' + limitWords + ', in the order of the strip';
    return;
  }
  note.textContent = 'Redrawing "' + pictureName(startRefs[0]) + '" · closeness says how much of it may change'
    + ' · this model takes one picture';
}

// The avoid-field words only reach the sampler when the run compares them,
// which is what guidance above 1 does; below that the control would change
// nothing, so the page does not offer it.
function showAvoidRow(model) {
  const row = E('avoid-row');
  const typed = ui.textField('guidance').trim();
  const guidance = typed === '' ? Number(model.guidance || 0) : Number(typed);
  row.hidden = !(guidance !== 0 && guidance !== 1 && Number.isFinite(guidance));
}

// Closeness is only meaningful for a model that redraws one picture; an edit
// model changes pictures by what the words say, so the field stays hidden
// unless it would do something.
function showStrengthRow(model) {
  const row = E('strength-row');
  row.hidden = !(model.redraw === true && Number(model.refs || 0) === 1);
}

// A clip length means something only to a model that makes moving pictures, so
// the field stays hidden where it would do nothing. The card names the rate such
// a model moves at, which is why the field counts frames and says so: seconds
// would be a second unit the page had to keep agreeing with the model's own.
function showClipRow(model) {
  const row = E('clip-row');
  if (row) row.hidden = !(model.video === true);
}

// What the model a run would draw with is, restated at the foot of the settings
// panel rather than only down the card rail: a dial left blank is answered by
// that model's own numbers, so the number a blank answers for belongs where the
// dial is. Nothing here asks the machine again - it is the same card
// renderModels draws, from the same reading - and a card that states nothing of
// some kind says so rather than having a guess written for it.
function paintModelCard(model) {
  const box = E('model-card-here');
  if (!box) return;
  box.textContent = '';
  const chosen = modelChoice();
  if (chosen === '' || !model.id) {
    addFact(box, 'nothing in use', chosen === ''
      ? 'no model is picked on this page, so a run would take what this machine has open - the cards name which'
      : 'the name a run would draw with (' + chosen + ') is not on this machine');
    return;
  }
  addFact(box, 'model', model.name + ' (' + model.id + ')');
  addFact(box, 'good at', model.specialty ? String(model.specialty) : 'its card states no specialty');
  const live = loadedSet.has(model.id);
  const where = live ? 'in memory' : (model.card_holds === false ? 'beyond this card'
    : (model.on_disk ? 'ready on disk'
    : (model.unchecked === true ? 'here · not checked' : 'not downloaded')));
  // One size, stated the way the card states it: bytes the machine holds in
  // memory answer for what a run would need, and bytes that only lie here are
  // named as such - a card that states neither says so rather than guessing.
  const stated = Number(model.bytes) > 0 ? ui.sizeText(Number(model.bytes))
    : (Number(model.present_bytes) > 0 ? ui.sizeText(Number(model.present_bytes)) + ' here' : 'no size stated');
  addFact(box, 'weights', stated + ' · ' + where);
  const forms = Array.isArray(model.quants) ? model.quants.filter(function (q) {
    return q && q.name;
  }) : [];
  if (forms.length) {
    const using = String(model.uses_quant || model.asked_quant || '');
    addFact(box, 'form', (using ? 'opened as ' + using : 'no form opened yet') +
      ' · comes in ' + forms.length + (model.default_quant ? ' · a download naming none takes ' + model.default_quant : ''));
  }
  addFact(box, 'by default', 'steps ' + Number(model.steps) +
    (Number(model.guidance) > 0 ? ' · guidance ' + Number(model.guidance) : ' · guidance this model does not use') +
    (Number(model.width) > 0 && Number(model.height) > 0
      ? ' · at ' + Number(model.width) + ' × ' + Number(model.height) + ', the size its card names'
      : ' · at no size its card names, so a run asks for one') +
    (model.sampler ? ' · sampled with ' + String(model.sampler) : '') +
    (model.scheduler ? ' and ' + String(model.scheduler) : '') +
    (model.cache === false ? ' · no step cache: the steps its card names are too few to skip' : '') +
    (Number(model.clip_skip) > 0
      ? ' · reads the text encoder to block ' + Number(model.clip_skip) + ', as its card names'
      : '') +
    (Number(model.flow_shift) > 0
      ? ' · sampled on a time shift of ' + (Math.round(Number(model.flow_shift) * 100) / 100) + ', as its own scheduler config names'
      : '') +
    (model.negative ? ' · usually avoids ' + String(model.negative) : ''));
  // What the machine's own runs of this card's numbers cost, when it has run them.
  // The ledger is the machine's, so this is what its runs really took here - a cold
  // first run and a warm one both sit in it, which is why the quickest and the
  // slowest are stated next to the average rather than hidden behind it.
  const asked = askFields(model.id, true);
  const found = costRow(asked);
  const runMs = found ? runMsOf(found, asked) : 0;
  // The quickest and the slowest are quoted in the same money as the first
  // number. A row at another step count prices this ask per step, so its own
  // recorded runs - made at its own steps and pictures - are brought to the
  // ask's amount of work before their spread is stated beside it; left raw,
  // one sentence would price one ask two ways at once.
  const rowWork = Number(found ? found.row.steps : 0) *
    (Number(found ? found.row.batch : 0) > 0 ? Number(found.row.batch) : 1);
  const askWork = Number(asked ? asked.steps : 0) *
    (Number(asked ? asked.batch : 0) > 0 ? Number(asked.batch) : 1);
  const spread = found && rowWork > 0 && askWork > 0 ? askWork / rowWork : 1;
  addFact(box, 'what it costs here', runMs > 0
    ? costWords(runMs) + ' at these numbers · ' + Number(found.row.count) + ' run' +
    (Number(found.row.count) === 1 ? '' : 's') +
    (found.exact ? ' at exactly these numbers' : ' at these switches, priced per step') +
    ' · the quickest ' + costWords(Number(found.row.min_ms) * spread) +
    ' · the slowest ' + costWords(Number(found.row.max_ms) * spread)
    : 'nothing at these numbers has run here yet · no time is claimed');
  const open = openCost(model.id, model.uses_quant || model.asked_quant || '');
  if (open.count > 0) {
    addFact(box, 'opening it', costWords(open.ms) + ' on average · ' + open.count + ' open' +
      (open.count === 1 ? '' : 's') + ' of this form here · the quickest ' + costWords(open.min));
  }
  const limit = Number(model.refs || 0);
  addFact(box, 'pictures', limit < 1 ? 'draws from words only'
    : (model.edit ? 'changes pictures · reads ' + limit + ', in the order of the strip'
      : (model.redraw ? 'redraws the one picture you give it, kept as close as closeness says'
        : 'takes ' + limit + ' starting picture' + (limit === 1 ? '' : 's'))));
  // A model that moves is stated as moving, in the units its card names them in,
  // because that is what a person sets on the page and what the machine charges
  // for: the clip's length is frames at the model's own rate, and its sound
  // belongs to the model rather than to anything the page added.
  if (model.video === true) {
    const frames = Number(model.frames || 0);
    const rate = Number(model.fps || 0);
    addFact(box, 'moving pictures',
      (frames > 0 && rate > 0
        ? frames + ' frames at ' + rate + ' fps · ' + (Math.round((frames / rate) * 10) / 10) + ' s of movement'
        : 'a length its card does not name') +
      ' · one clip to a run, and the frames field on this page asks for another length' +
      ' · the clip comes with its own sound, kept as its own file beside it');
  }
  if (model.repo) addFact(box, 'comes from', String(model.repo));
}

// What the cards would say right now, in one string. The list is rebuilt by the
// poll, and rebuilding it means throwing away every button on it: a press that
// landed in the moment between the tear-down and the redraw was simply lost, which
// is a card that does nothing when pressed on a machine that is running fine. So
// the cards are redrawn when there is something new to say about them, and left
// alone when there is not.
function modelsCardSignature() {
  // The page's own pick is part of what the cards say - the heavier border and
  // the in use mark answer to it - so it belongs in the words that decide
  // whether a redraw has anything new to say. Left out, tapping a card moved
  // every other line on the page and left the mark on the card it was before.
  return JSON.stringify([pickedModel, pickedChoice, Array.from(loadedSet).sort(),
    costRows.length, openRows.length, models.map(function (m) {
      // What the cards would cost here is part of what they say, so a card is
      // redrawn when a run of its settings lands - and only then.
      const found = costRow(askFields(m.id, true));
      const open = openCost(m.id, m.uses_quant || m.asked_quant || '');
      return [m.id, m.on_disk, m.bytes, m.present_bytes, m.unchecked, m.asked_quant, m.uses_quant,
      found ? Number(found.row.count) : 0,
      found && found.exact ? 1 : 0,
      found ? Number(found.row.updated) : 0,
      open.count,
      (m.quants || []).map(function (q) { return [q.name, q.here, q.present, q.bytes]; })];
    })]);
}

function renderModels() {
  const list = E('models-pane-list');
  const signature = modelsCardSignature();
  if (signature === modelsCardState && list.childElementCount) return;
  modelsCardState = signature;
  let listed = false;
  for (const model of models) {
    if (model.id === pickedModel) listed = true;
  }
  if (!listed) pickedModel = '';
  list.textContent = '';
  for (const model of models) {
    const live = loadedSet.has(model.id);
    const wanted = modelChoice();
    const inUse = wanted !== '' && model.id === wanted;
    // The model a run would draw with carries a heavier border in the
    // selection colour, so the picked model reads off the cards without
    // opening anything; a picked card that is also in memory keeps the
    // picking mark, the heavier of the two.
    const marks = [];
    if (live) marks.push('model-on');
    if (inUse) marks.push('model-chosen');
    const shell = ui.cardShell(marks.join(' '));
    const card = shell.card;
    if (live) card.dataset.live = '1';

    const head = shell.main;
    const name = document.createElement('span');
    name.className = 'model-name';
    name.textContent = model.name;
    head.appendChild(name);
    head.title = modelHint(model) +
      ' Tap this card to put this model in use; tapping never loads or downloads anything.';
    // The card itself is the control: tapping it puts that model in use and
    // does nothing else — models are opened by load, never by a pick, so
    // choosing spends no time or memory. What a model is good at lives in this
    // tooltip, and the card face keeps only the name, its one-line
    // description, and real numbers.
    ui.onPress(head, function () { void putInUse(model); });

    if (model.specialty) {
      const best = document.createElement('div');
      best.className = 'model-note';
      best.textContent = String(model.specialty);
      head.appendChild(best);
    }
    const spec = ['steps ' + model.steps];
    if (model.guidance > 0) spec.push('cfg ' + model.guidance);
    const specLine = document.createElement('div');
    specLine.className = 'model-note';
    specLine.textContent = spec.join(' · ');
    specLine.title =
      'what this model wants by default; the prompt controls still answer for any change. ' +
      (model.negative ? 'what it usually avoids: ' + model.negative : '');
    head.appendChild(specLine);
    const fingers = shell.fingers;
    // Three states, not two. Bytes that sit on this machine and have never been
    // read are not the sentence "not downloaded", and until today the page had no
    // way to say the first and so said the second.
    const state = live ? 'in memory' : (model.card_holds === false ? 'beyond this card'
      : (model.on_disk ? 'ready on disk'
      : (model.unchecked === true ? 'here · not checked' : 'not downloaded')));
    fingers.appendChild(ui.finger(state, live ? 'loaded' : (model.card_holds === false ? 'warn'
      : (model.on_disk ? 'ok' : 'warn')),
      model.card_holds === false
        ? 'The math these weights are made of is more than this card can carry, and the ' +
          'machine says so up front instead of opening them and crawling on the cpu for an hour.'
        : 'What this model is right now: open in memory, kept on disk, or not fetched yet.', 'state'));
    if (inUse) {
      fingers.appendChild(ui.finger('in use', 'ok',
        'The model a run would draw with now — this card is the one with the heavier border. ' +
        (live ? 'Its memory is holding it, so a run starts straight away.'
          : 'Its files are read into memory when a run starts.') +
        ' Tapping another card puts that model in use instead.', 'use'));
    }
    if (!model.bytes && Number(model.present_bytes) > 0) {
      fingers.appendChild(ui.finger(ui.sizeText(Number(model.present_bytes)), 'size',
        'Bytes of this model that are on this machine and have not been checked yet. ' +
        'Pressing one of its forms reads them; nothing is fetched that is already here.', 'size'));
    }
    if (model.bytes) {
      fingers.appendChild(ui.finger(ui.sizeText(model.bytes), 'size',
        'The weights this model needs in memory.', 'size'));
    }
    // Which form of this model's weights the machine can hold. Some models
    // come in more than one precision of the same weights: the names and the
    // sizes come from the machine's own list, and a size nothing states is
    // left unstated rather than guessed. Pressing a form asks for that one -
    // it is fetched when it is not on the machine - and the model keeps that
    // form until another is pressed.
    const forms = Array.isArray(model.quants) ? model.quants.filter(function (q) {
      return q && q.name;
    }) : [];
    if (forms.length) {
      const askedFor = String(model.asked_quant || '');
      // The form this machine opens the model with, which the machine states. Until
      // now the card had no such word, and the form you pressed was taken off the
      // card - so ten identical buttons picked one you could not see, which read as
      // the buttons choosing a quant at random.
      const usingForm = String(model.uses_quant || askedFor || '');
      let named = '';
      for (const form of forms) {
        named += (named ? ' · ' : '') + form.name +
          (form.bytes ? ' · ' + ui.sizeText(Number(form.bytes)) : '') +
          (form.here === true ? ' · on this machine'
            : (form.present === true ? ' · here, not checked' : ' · not on this machine yet'));
      }
      const note = document.createElement('div');
      note.className = 'model-note';
      note.textContent = (usingForm ? 'opened as ' + usingForm + ' · ' : 'no form opened yet · ') +
        'comes in ' + forms.length + ' forms';
      note.title = named + '. only the transformer changes form; the text encoders and the VAE keep their own files. ' +
        (model.default_quant ? 'a download that names no form takes ' + model.default_quant + '. ' : '') +
        'the form a model is opened in is the one a page last asked for; with none asked, this machine opens what it already has.';
      head.appendChild(note);
      // The form this machine opens the model with stands on the card by itself:
      // it is the one form a person cannot change by looking, and pressing
      // another form is what moves it.
      for (const form of forms) {
        if (form.name !== usingForm) continue;
        fingers.appendChild(ui.finger('opened as ' + form.name, 'ok',
          'The form this machine opens the model with now' +
          (form.bytes ? ', ' + ui.sizeText(Number(form.bytes)) + ' of weights' : '') +
          '. Choosing another form below asks for that one instead; the machine keeps the form it is asked for.', 'use'));
      }
      // Every other form is chosen from one dropdown rather than drawn as one row
      // each. Ten precisions of one transformer used to take ten rows of the
      // card, and the card read as a list of buttons rather than as one model:
      // the word on each choice still says where those bytes would come from —
      // this machine's own file or the source — because that is the only thing
      // a person can still decide about them.
      const others = forms.filter(function (form) { return form.name !== usingForm; });
      if (others.length) {
        const row = document.createElement('div');
        row.className = 'finger pick';
        const pick = document.createElement('select');
        pick.setAttribute('aria-label', 'another form of ' + model.name);
        pick.title = 'Ask this model for another form of its transformer: ' +
          (model.default_quant ? 'a download that names no form takes ' + model.default_quant + '; ' : '') +
          'a form already on this machine is only opened, and a form this machine does not hold is fetched from the source and checked against its SHA-256.';
        const first = document.createElement('option');
        first.value = '';
        first.textContent = 'as another form (' + others.length + ')';
        pick.appendChild(first);
        for (const form of others) {
          const onThis = form.here === true || form.present === true;
          const opt = document.createElement('option');
          opt.value = form.name;
          opt.textContent = form.name +
            (form.bytes ? ' · ' + ui.sizeText(Number(form.bytes)) : '') + ' · ' +
            (form.here === true ? 'on this machine'
              : (form.present === true ? 'here, not checked' : 'from the source'));
          opt.title = form.name + (form.bytes ? ' (' + ui.sizeText(Number(form.bytes)) + ' of weights)' : '') + ': ' +
            (onThis
              ? 'that file is on this machine, so choosing it only changes which form the model opens with.'
              : 'this machine does not hold it, so choosing it fetches it from the source and checks it against the source SHA-256.');
          pick.appendChild(opt);
        }
        const mark = document.createElement('span');
        mark.className = 'pick-mark';
        mark.textContent = '▾';
        row.appendChild(pick);
        row.appendChild(mark);
        ui.onChoose(pick, function () {
          const chosen = String(pick.value || '');
          if (!chosen) return;
          // The choice is the ask, not a standing state: the form the machine
          // opens with is the machine's own word, and it comes back on the next read.
          pick.value = '';
          handAsk(model.id, 'download', { quant: chosen });
        });
        pick.setAttribute('id', 'act-' + model.id + '-forms');
        fingers.appendChild(row);
      }
    }
    if (model.edit === true) {
      const reads = Number(model.refs || 0);
      fingers.appendChild(ui.finger(reads > 1
        ? 'changes pictures · reads ' + reads
        : 'changes a picture · reads 1', 'cap',
        reads > 1
          ? 'This model changes pictures you keep: it reads up to ' + reads +
          ' of them, in the order they sit in the strip above the prompt.'
          : 'This model changes the one picture you keep as its start.', 'cap'));
    }
    if (model.redraw === true) {
      fingers.appendChild(ui.finger('redraws one picture', 'cap',
        'This model redraws the one picture you give it, guided by how close the result must stay.', 'cap'));
    }
    if (!model.on_disk) {
      const get = ui.fingerAct('download',
        function () { handAsk(model.id, 'download'); },
        'Fetch the weights and check them against the source SHA-256.', 'download');
      get.setAttribute('id', 'act-' + model.id + '-download');
      fingers.appendChild(get);
    }
    if (model.on_disk && !live) {
      const load = ui.fingerAct(everLoaded.has(model.id) ? 'reload' : 'load',
        function () { handAsk(model.id, 'load'); },
        'Open this model on the GPU now. Only one image model fits at a time.', 'load');
      load.setAttribute('id', 'act-' + model.id + '-load');
      fingers.appendChild(load);
    }
    if (live) {
      everLoaded.add(model.id);
      const unload = ui.fingerAct('unload', async function () {
        // The card changes only on the machine's word, not on the wish: a
        // refusal keeps the model standing where the machine says it stands.
        const down = await ui.apiR('/api/unload', 'POST', { model: model.id });
        if (!down.answer) {
          log('the machine would not free ' + modelName(model.id) + ': ' +
            (down.error || ui.askWhy(down)), 'err');
          return;
        }
        loadedSet.delete(model.id);
        if (pickedModel === model.id) pickedModel = '';
        renderModels();
      }, 'Free the memory this model is holding. The weights stay on disk.', 'unload');
      unload.setAttribute('id', 'act-' + model.id + '-unload');
      fingers.appendChild(unload);
    }
    // No separate "use" row: the card is the control, and the choice comes
    // from the same value the page draws, so nothing else asks for a model.
    list.appendChild(card);
  }
}

// The square a run draws at when nothing names one: the machine keeps its own
// default and answers with it, so this only stands where a model's card states
// no size of its own. A card that does state one is asked for nothing - the
// machine draws at the size the model was trained at.
const kDefaultSide = 768;

function sizePick() {
  const choice = String(ui.selectValue('size-select'));
  if (choice === 'model') {
    const card = findModel(modelChoice()) || {};
    return {
      width: Number(card.width) > 0 ? Number(card.width) : kDefaultSide,
      height: Number(card.height) > 0 ? Number(card.height) : kDefaultSide
    };
  }
  if (choice === 'custom') {
    return { width: ui.numberField('width', kDefaultSide), height: ui.numberField('height', kDefaultSide) };
  }
  const parts = choice.split('x');
  if (parts.length === 2 && /^\d+$/.test(parts[0]) && /^\d+$/.test(parts[1])) {
    return { width: Number(parts[0]), height: Number(parts[1]) };
  }
  return { width: kDefaultSide, height: kDefaultSide };
}

// The model a run will draw with: what was last picked on a card, or while
// nothing was picked, the model this machine has open (its memory was last
// used). A name this machine does not have is no choice at all, and a run
// says so.
function modelChoice() {
  if (pickedChoice !== '') {
    for (const model of models) {
      if (model.id === pickedChoice) return pickedChoice;
    }
    return '';
  }
  return pickedModel;
}

// What a model was asked to draw, kept on this browser beside the page's other
// kept choices. A model change writes what the controls hold for the model being
// left and puts back what this browser kept for the model being picked: the
// words, the dials, and the strip of starting pictures. It is kept per model
// because a model's numbers are its card's numbers - twenty-eight steps is a
// setting for one model and a mistake for a four-step distilled one - and a
// prompt written for one model's strengths says nothing useful to another. The
// starting pictures themselves are on the machine already, and the strip names
// them by what the machine calls them, so what comes back here is which ones sat
// in the strip and in what order.
const kAskStore = 'ols-model-asks';
let kAsks = null;
const kAskDials = ['prompt-field', 'negative-field', 'width', 'height', 'steps', 'guidance', 'seed', 'count'];
const kAskSelects = ['size-select', 'ref-strength'];

function readAsks() {
  if (kAsks) return kAsks;
  try {
    const kept = JSON.parse(localStorage.getItem(kAskStore) || 'null');
    kAsks = kept && typeof kept === 'object' ? kept : {};
  } catch (brokenStore) {
    kAsks = {};
  }
  return kAsks;
}

function askOf(id) {
  const kept = readAsks()[String(id)];
  return kept && typeof kept === 'object' ? kept : null;
}

function hasOption(node, want) {
  for (let at = 0; at < node.options.length; ++at) {
    if (String(node.options[at].value) === want) return true;
  }
  return false;
}

// A control's own opening value, for a model with nothing kept for it: every
// dial blank answers with the model's card, and the two that are not blank open
// where the page opens them.
function defaultDial(key, id) {
  if (key === 'prompt-field') return ownPrompt(id);
  if (key === 'seed') return '-1';
  if (key === 'count') return '1';
  return '';
}

// Write the ask the controls hold right down for one model.
function keepAsk(id) {
  const wanted = String(id || '');
  if (wanted === '') return;
  const ask = { refs: [] };
  for (const held of startRefs) ask.refs.push(String(held));
  for (const key of kAskDials) {
    const node = E(key);
    ask[key] = node ? String(node.value) : '';
  }
  for (const key of kAskSelects) ask[key] = String(ui.selectValue(key));
  const asks = readAsks();
  asks[wanted] = ask;
  try {
    localStorage.setItem(kAskStore, JSON.stringify(asks));
  } catch (noRoom) {
    log('This browser would not keep what the controls hold for ' + wanted, 'warn');
  }
}

// Put one model's ask back on the controls: the words, the dials, the strip.
// Nothing kept for it takes that model's own defaults - the prompt its card
// states, its own size, every dial blank. A stored choice the page no longer
// offers is left for the page's opening one, and a starting picture this
// machine no longer holds is named as gone by the strip's own drawing.
function restoreAsk(id) {
  const wanted = String(id || '');
  const model = findModel(wanted) || {};
  const ask = askOf(wanted);
  for (const key of kAskDials) {
    const node = E(key);
    if (!node) continue;
    node.value = ask && ask[key] !== undefined ? String(ask[key]) : defaultDial(key, wanted);
  }
  for (const key of kAskSelects) {
    const node = E(key);
    if (!node || !node.options.length) continue;
    const want = ask && ask[key] !== undefined ? String(ask[key]) : '';
    node.value = hasOption(node, want) ? want : String(node.options[0].value);
  }
  // Words this browser kept for the model are a person's own ask, so the page's
  // rule about an untouched prompt is kept from reaching over them; with nothing
  // kept, the card's prompt is the page's own suggestion and that rule keeps
  // working over it, which is what fills the field for the next model that has
  // nothing stored.
  const field = E('prompt-field');
  autoPrompt = ask ? '' : (field ? String(field.value) : '');
  const limit = Number(model.refs || 0);
  const strip = ask && Array.isArray(ask.refs) ? ask.refs : [];
  startRefs = [];
  let refused = 0;
  for (const held of strip) {
    const found = String(held);
    let twice = false;
    for (const kept of startRefs) {
      if (kept === found) twice = true;
    }
    if (twice) continue;
    if (startRefs.length >= limit) {
      refused += 1;
      continue;
    }
    startRefs.push(found);
  }
  dropMissingStarts();
  if (refused > 0) {
    notePicture('This model takes ' + limit + ' starting picture' + (limit === 1 ? '' : 's') +
      ', so ' + refused + ' from the strip you had did not come back.');
  }
}

// The reset hand: what this browser kept for this model is let go of, and the
// controls take the model's own defaults - the words its card states it is good
// at, the size its card names, every dial blank so the card answers for them.
// The strip is emptied with them, since a starting picture is part of the ask;
// the pictures themselves stay on the machine, where the strip is picked again.
function resetAsk() {
  const chosen = modelChoice();
  if (chosen === '') {
    log('No model is in use: there is nothing to reset to.', 'warn');
    return;
  }
  const asks = readAsks();
  delete asks[String(chosen)];
  try {
    localStorage.setItem(kAskStore, JSON.stringify(asks));
  } catch (noRoom) {
    log('This browser would not let go of what it kept for ' + chosen, 'warn');
  }
  restoreAsk(chosen);
  paintRunState();
  const model = findModel(chosen) || {};
  log('Reset to ' + (model.name || chosen) + "'s own defaults: its prompt, its size, every dial blank, strip empty.",
    'ok');
  ui.flash(E('prompt-panel'));
}

// Put a model in use, from its card: the page's one choice changes, the
// machine is asked to remember it so the choice survives a reload, and the
// page is redrawn from it. Nothing is loaded, read in, or downloaded here —
// a run takes the weights when it starts.
async function putInUse(model) {
  if (modelChoice() === model.id) {
    log('This model is already the one a run draws with; nothing changed.');
    return;
  }
  const from = modelChoice();
  // What the controls hold belongs to the model being left, and what this
  // browser kept for the model being picked is put back before anything is
  // redrawn from it - the strip, the words, and the dials are the new model's.
  keepAsk(from);
  pickedChoice = String(model.id);
  const kept = await savePrefs();
  log(kept ? 'Runs will draw with ' + model.name
    : 'Runs will draw with ' + model.name +
    ' on this page; the machine refused the write, so the choice will not survive a reload',
    kept ? 'ok' : 'warn');
  restoreAsk(model.id);
  renderModels();
  // the model in use changed, so what it can take, and what the page offers
  // beside it, are redrawn from it rather than left as before
  paintRunState();
}

// Every picture is its own job: this studio never hands the machine an ask of
// several at once. One job draws one picture, stands as one line in the queue,
// and takes a slot of its own - with its own bar and its own stop hand - on the
// card of the ask it belongs to; a batch is as many jobs as the ask counts
// pictures, so an ask of sixteen is sixteen jobs drawn one after another, and
// stopping one touches no other. What one run of the engine could draw back to
// back inside one model open is the machine's own business; the line into it is
// one picture deep.

// What the ask on this page will make: what its own images field named, kept
// inside the deepest ask this studio makes. Each picture is a job of its own, so
// this number is at once the pictures drawn and the jobs the machine is handed.
function askedPictures() {
  const wanted = Math.round(Number(ui.numberField('count', 1)));
  const chosen = wanted > 1 ? wanted : 1;
  return chosen > kMaxBatchPictures ? kMaxBatchPictures : chosen;
}

function payload() {
  const size = sizePick();
  const body = {
    model: modelChoice(),
    prompt: ui.textField('prompt-field').trim() || ownPrompt(modelChoice()),
    negative_prompt: ui.textField('negative-field').trim(),
    width: size.width,
    height: size.height,
    seed: ui.numberField('seed', -1),
    // One picture per job, always: a batch is several such jobs handed over one
    // at a time, each its own line in the queue, so the run this body starts
    // draws one picture and no more.
    count: 1,
  };
  const steps = ui.textField('steps').trim();
  if (steps !== '') body.steps = Number(steps);
  const guidance = ui.textField('guidance').trim();
  if (guidance !== '') body.guidance = Number(guidance);
  if (ui.isOn('pill-tiling')) body.tiling = true;
  if (ui.isOn('pill-sage')) body.sage_attn = true;
  const limit = refsOf(findModel(modelChoice()) || {});
  if (startRefs.length && limit) {
    body.references = startRefs.slice(0, limit);
    const closeness = ui.selectValue('ref-strength');
    if (closeness !== '') body.strength = Number(closeness);
  }
  // A model that moves makes one clip per run, so the ask names one clip and the
  // clip's own length is asked in frames, the unit the model's card counts in. A
  // blank leaves the length the card named, which is what the server uses -
  // nothing is invented here.
  const askedModel = findModel(modelChoice()) || {};
  if (askedModel.video === true) {
    const frames = ui.textField('clip-length').trim();
    if (frames !== '') body.frames = Number(frames);
  }
  return body;
}

// The studio's render states its progress in the header alone: the header fills
// behind its words to the step it is on, and the line beside the badges names the
// model and the step, while a run works. paintPictureNotices owns that line: every
// standing render reaches it, whichever window asked for it. There is no second
// bar here - the home page paints the same run's bar for anyone who wants it.
function finishJob() {
  running = null;
}

// What a model's own name is, for words a person reads.
function modelName(id) {
  const found = findModel(id);
  if (!found) return String(id);
  return String(found.name || found.id || id);
}

// What a run this page watched has done: the moments its own stream named, kept
// so a rate can be measured from the steps it reported and the stack can be
// repainted from the stream itself - the next reading of the machine would be the
// same numbers a few seconds later.
const jobTimes = {};

/* ---------- the ask is the tile: a picture of it is a slot on that tile ----------
   A batch is watched as the queue it is, but it is drawn as the ask it is: an
   ask of sixteen pictures stands as ONE card holding sixteen slots from the
   moment it is made, each slot with its own bar and its own stop hand, until
   its picture lands and the machine's record takes that slot's place on the
   card. An ask that already has renders here adds its slots to the card those
   renders stand on and pulls that card to the front - the ask is the tile, so
   a further run of it is more of the same tile, not a second tile beside it.
   Nothing on a slot is invented: the steps, the state, and the moment a picture
   is drawn come from the machine's record of the job the slot stands for; the
   number of slots comes from what this page asked, which is the only place that
   number exists; and the time given is a rate measured from the ticks the
   machine really made - the rate this job is holding now, otherwise the rate a
   finished run here showed, and when neither is measured the card states no
   time. */
const plannedPictures = [];
// The nodes of the standing cards, kept by the ask they stand on, so a paint
// pass can move a bar, a slot, and a line of words without drawing the gallery
// again. renderGallery fills these when it draws the slots of an ask, and the
// paint pass clears the cards whose pictures no longer stand.
const pendingEls = new Map();
// What the queue pass decided last time it ran: one row per picture that stands,
// in the order they will be drawn. The gallery draws the rows of one ask as the
// slots of the card that ask names - the card it already has, or a new one when
// the room holds no such card yet.
let pendingRows = [];
// What the machine states about the runs this page has watched: aggregate ticks
// over the wall time they took, which is the unit the machine's own count uses.
const aggregateRate = {};

// What this machine states its own work costs, read at /api/timings: one row per
// set of settings it has run to the end, with the times those runs took - the
// count, the mean, the fastest and slowest, and what one step and one picture cost
// at those settings. The ledger lives on the machine rather than on this page, so
// an estimate survives a reload and is there in a window that watched nothing; the
// page's own tally above forgets both. Nothing here is guessed: settings nobody has
// asked for at these numbers have no row, and a card that gets no row states no
// time. The machine keys a run by the model, the sides, the steps, the guidance, the
// precision the weights really stood as, the sampler and scheduler it really used,
// the step cache, the device, the switches, the batch and the reference count - and
// by no seed, because another seed is the same work at these settings.
let costRows = [];
let openRows = [];
// The card of this machine in its own full name, as the machine names it at
// /api/system ("Vulkan0 AMD Radeon(TM) 890M Graphics"). The cost ledger keys a
// run by that name now, because a "card" that meant one machine's card would
// quote another machine's numbers; a page that has not read the name yet says
// 'card', which matches no ledger row and so states no time.
let cardNamed = '';

// The settings a run of this model would be asked for, in the fields the machine
// keys its costs by: the card's numbers as this page's dials and switches leave
// them. A prompt is not in there - it changes what a picture shows, not what the
// machine pays for its steps - and neither is a seed. Asked with defaultsOnly, the
// dials and the batch are left out and only the card's own numbers stand, which is
// what a card's "by default" line means; the switches stay, because those are the
// machine's choice rather than a dial on the ask.
function askFields(model, defaultsOnly) {
  const card = findModel(String(model));
  if (!card) return null;
  const body = defaultsOnly ? {} : payload();
  const steps = defaultsOnly ? Number(card.steps)
    : (Number(body.steps) > 0 ? Number(body.steps) : askedSteps(model));
  const guidance = defaultsOnly
    ? (Number(card.guidance) > 0 ? Number(card.guidance) : 1)
    : (Number(body.guidance) > 0 ? Number(body.guidance)
      : (Number(card.guidance) > 0 ? Number(card.guidance) : 1));
  return {
    model: String(card.id),
    width: defaultsOnly ? Number(card.width) : Number(body.width),
    height: defaultsOnly ? Number(card.height) : Number(body.height),
    steps: steps,
    guidance: guidance,
    sampler: card.sampler ? String(card.sampler) : 'engine default',
    scheduler: card.scheduler ? String(card.scheduler) : 'engine default',
    cache: card.cache !== false,
    // the precision the machine would open this model as, in the card's own words;
    // a row the machine wrote carries the form the weights really stood as, and
    // these two are the same reading of the same files.
    quant: card.uses_quant ? String(card.uses_quant) : 'as shipped',
    device: ui.isOn(pillKeys.cpu) ? 'cpu' : (cardNamed || 'card'),
    sage_attn: ui.isOn(pillKeys.sage_attn),
    batch: defaultsOnly ? 1 : (Number(body.count) > 1 ? Number(body.count) : 1),
    refs: defaultsOnly ? 0 : (body.references ? body.references.length : 0),
    // A clip's length is part of an ask in the same way its size is: the machine
    // files a run at the length it was asked for, and a longer clip costs
    // proportionally more, so a cost read from another length would be a wrong
    // number rather than a near one. A model that draws pictures asks for no
    // frames, and 0 on both sides means the same thing there.
    frames: defaultsOnly ? Number(card.frames || 0)
      : (Number(body.frames) > 0 ? Number(body.frames) : Number(card.frames || 0)),
    fps: defaultsOnly ? Number(card.fps || 0)
      : (Number(body.fps) > 0 ? Number(body.fps) : Number(card.fps || 0)),
  };
}

// What the machine measured for an ask. A row at exactly these numbers answers with
// the mean of its runs; otherwise the newest row at the same model, size, solver,
// cache, device and switches stands as that model's per-step cost here, which is
// what another step count costs. The rows come newest first, so the first that fits
// is the most recent cost this machine has.
function costRow(asked) {
  if (!asked) return null;
  let near = null;
  for (const row of costRows) {
    // The model is its own field on a row the machine wrote; a row that arrived
    // without one is still named by its key, and the key is what the machine files
    // it under, so that is where the name is read back from.
    const rowModel = row.model ? String(row.model)
      : (row.key ? String(row.key).split('|')[1] : '');
    if (rowModel !== String(asked.model)) continue;
    if (String(row.device) !== String(asked.device)) continue;
    if ((row.sage_attn === true) !== (asked.sage_attn === true)) continue;
    // a row older than the machine's own naming of precisions carries none, and
    // it stands as a near row rather than vanishing; a row that does name one
    // must name the one this ask would open at.
    if (row.quant !== undefined && String(row.quant) !== String(asked.quant)) continue;
    if ((row.cache === true) !== (asked.cache === true)) continue;
    if (String(row.sampler) !== String(asked.sampler)) continue;
    if (String(row.scheduler) !== String(asked.scheduler)) continue;
    if (Number(row.refs) !== Number(asked.refs)) continue;
    if (Number(row.batch) !== Number(asked.batch)) continue;
    if (Number(row.width) !== Number(asked.width)) continue;
    if (Number(row.height) !== Number(asked.height)) continue;
    // The length of a clip is part of the ask: a row at another length is not a
    // near answer for this one, its cost belongs to a different amount of work.
    if (Number(asked.frames || 0) > 0 || Number(row.frames || 0) > 0) {
      if (Number(asked.frames || 0) !== Number(row.frames || 0)) continue;
    }
    if (Number(row.steps) === Number(asked.steps) &&
      Math.abs(Number(row.guidance) - Number(asked.guidance)) < 0.02) {
      return { row: row, exact: true };
    }
    if (!near && Number(row.mean_ms_per_step) > 0) near = { row: row, exact: false };
  }
  return near;
}

// The whole run such a row says, and the cost of one step of it. A row at exactly
// these numbers answers with the mean of its runs, which is the cost of that whole
// ask; a row at another step count prices the steps this ask asks for at its own
// per-step cost, over the pictures the ask makes in one run, since the machine
// charges for each of them. A row states its per-step cost over the steps it
// sampled for all of its pictures, so a step of a run of pictures and a step of a
// run of one are the same thing here.
function runMsOf(found, asked) {
  if (!found) return 0;
  if (found.exact && Number(found.row.mean_ms) > 0) return Number(found.row.mean_ms);
  const perStep = Number(found.row.mean_ms_per_step);
  const steps = Number(asked.steps) > 0 ? Number(asked.steps) : 0;
  const batch = Number(asked.batch) > 0 ? Number(asked.batch) : 1;
  if (perStep > 0 && steps > 0) return Math.round(perStep * steps * batch);
  return 0;
}
function stepMsOf(found) {
  if (!found) return 0;
  if (Number(found.row.mean_ms_per_step) > 0) return Number(found.row.mean_ms_per_step);
  // A row that states no per-step cost states its whole run: one of its steps is
  // that run over the steps it sampled, which is its steps for every picture it
  // was asked to make.
  const steps = Number(found.row.steps) * (Number(found.row.batch) > 0 ? Number(found.row.batch) : 1);
  if (steps > 0 && Number(found.row.mean_ms) > 0) return Number(found.row.mean_ms) / steps;
  return 0;
}

// What one open of this model has cost here. An open carries no settings beyond the
// precision it was opened as, so that is the whole key; a model nobody has opened
// here has no cost and no estimate.
function openCost(model, precision) {
  const wanted = precision && String(precision) !== '' ? String(precision) : 'as shipped';
  for (const row of openRows) {
    if (String(row.model) !== String(model)) continue;
    if (String(row.precision || 'as shipped') !== wanted) continue;
    return {
      ms: Number(row.mean_ms), count: Number(row.count),
      min: Number(row.min_ms), max: Number(row.max_ms)
    };
  }
  return { ms: 0, count: 0, min: 0, max: 0 };
}

// A cost in the words a person reads. The machine's own numbers are milliseconds;
// nobody wants to read 38206 of them.
function costWords(ms) {
  const value = Number(ms);
  if (!(value > 0)) return 'no time is claimed';
  const seconds = value / 1000;
  if (seconds < 10) return (Math.round(seconds * 10) / 10) + ' s';
  if (seconds < 600) return Math.round(seconds) + ' s';
  return Math.round(seconds / 60) + ' min';
}
let planSeq = 0;
// The biggest ask this studio makes at once: every picture of it stands as a
// slot on one card, so this number is at once the pictures drawn and the slots
// one card carries.
const kMaxBatchPictures = 16;

function pictureKeyFor(job, index) {
  return 'img-' + String(job) + '-' + String(index);
}

// Where a picture sits in the queue, in the few words its card carries under its
// state. One number for one place: the batch number when a single ask made
// several pictures, and the standing count when the machine holds more than one
// job. A one-picture job is not told twice.
function picturePosition(entry, position, standing) {
  const parts = [];
  if (Number(entry.of) > 1) {
    parts.push('picture ' + (Number(entry.n) + 1) + ' of ' + Number(entry.of) + ' asked together');
  }
  if (standing > 1) parts.push(position + ' of ' + standing + ' standing here');
  return parts.join(' · ');
}

// What this page's next ask will run at, in the words a card carries under the
// picture: the prompt it will draw with and the settings the ask named. A
// setting the ask did not name is not claimed - the machine's own default stays
// the machine's. The whole row of settings comes back too: it is what a standing
// card is shaped like, and what a cost row is keyed by before any machine
// record speaks.
function describeAsk() {
  const body = payload();
  const parts = [];
  const steps = Number(body.steps) > 0 ? Number(body.steps) : askedSteps(body.model);
  if (steps > 0) parts.push('steps ' + steps);
  if (Number(body.width) > 0 && Number(body.height) > 0) parts.push('at ' + body.width + 'x' + body.height);
  if (Number(body.guidance) > 0) parts.push('guidance ' + Number(body.guidance));
  if (Number(body.seed) >= 0) parts.push('seed ' + Number(body.seed));
  if (body.references && body.references.length) {
    parts.push('from ' + body.references.length +
      (body.references.length === 1 ? ' picture' : ' pictures'));
  }
  const card = findModel(String(body.model || '')) || {};
  return {
    prompt: String(body.prompt || ''), settings: parts.join(' · '),
    fields: {
      width: Number(body.width) > 0 ? Number(body.width) : Number(card.width) || 0,
      height: Number(body.height) > 0 ? Number(body.height) : Number(card.height) || 0,
      steps: steps,
      guidance: Number(body.guidance) > 0 ? Number(body.guidance) : Number(card.guidance) || 0,
      seed: Number(body.seed), negative: String(body.negative_prompt || ''),
      frames: Number(body.frames) > 0 ? Number(body.frames) : Number(card.frames) || 0,
      fps: Number(card.fps) || 0,
      refs: (body.references || []).slice(),
      video: card.video === true,
    },
  };
}

// The address of a kept picture's file, for a standing card's row of backs.
function refThumbUrl(id) {
  for (const ref of refs) {
    if (String(ref.id) === String(id)) return ui.fileSrc('/files/refs/' + ui.encodePath(String(ref.file || ref.id)));
  }
  return '';
}

// A row the gallery can draw as a card: shaped like a gallery record - the
// fields a record states about itself, and its whole state besides, so a paint
// pass can fill it without a second look. A standing picture is stated in the
// same fields the machine states about a landed render - no more. The fields are
// what its record will read as when it lands and joins the card of its ask, and
// they are what put the slot on that card in the first place: the ask is the
// card, and this row is one of its slots. The card's own lines still say
// everything the dials hold.
function pendingItem(row) {
  const video = row.video === true;
  return {
    id: 'pending-' + row.key, pending: row,
    model: row.model, prompt: row.prompt,
    width: row.width, height: row.height, steps: row.steps || '',
    negative_prompt: '', guidance: '',
    frames: video ? (row.frames || '') : '',
    fps: video ? (row.fps || '') : '',
    kind: video ? 'video' : '',
  };
}

// A standing card, drawn once and then only filled: the shape the pictures will
// come back at, the words they will draw with, their state as one line, and one
// slot per picture of the ask. The first slot stands in the frame the picture
// will come back at; the rest ride in a row under it at the same shape, so the
// count of an ask is read off its own card rather than off a line of cards a
// person has to count. Each slot keeps its own bar and its own stop hand: a
// picture of an ask is a job of its own at the machine, and stopping one of
// them touches none of the others.
function pendingCard(item, others) {
  const shot = document.createElement('div');
  shot.className = 'shot pending-shot';
  const said = document.createElement('div');
  said.className = 'shot-prompt';
  const meta = document.createElement('div');
  meta.className = 'shot-meta';
  const queue = document.createElement('div');
  queue.className = 'shot-queue';
  const card = { shot: shot, said: said, meta: meta, queue: queue, slots: new Map() };
  const rows = [item.pending];
  shot.appendChild(pendingSlot(item, card, true));
  shot.appendChild(said);
  shot.appendChild(meta);
  shot.appendChild(queue);
  const row0 = item.pending;
  if (row0.refThumbs && row0.refThumbs.length) {
    // The pictures this ask starts from, on the card that will hold what comes
    // out of them.
    const backs = document.createElement('div');
    backs.className = 'shot-backs';
    row0.refThumbs.forEach(function (url) {
      const back = document.createElement('img');
      back.setAttribute('width', '34');
      back.setAttribute('height', '34');
      back.setAttribute('alt', 'a picture this ask starts from');
      back.title = 'this ask starts from this picture';
      back.src = url;
      backs.appendChild(back);
    });
    shot.appendChild(backs);
  }
  if (others.length) {
    const row = document.createElement('div');
    // The row of the rest of the ask borrows the row of an ask's runs: the same
    // line of small tiles, and the same room to wrap them in.
    row.className = 'shot-stack pending-slots';
    for (const one of others) {
      row.appendChild(pendingSlot(one, card, false));
      rows.push(one.pending);
    }
    shot.appendChild(row);
  }
  pendingEls.set(stackKeyFor(item), card);
  fillCard(card, rows);
  return shot;
}

// One slot of a standing card: the shape one picture of the ask will come back
// at, its bar, and its own stop hand. The leading slot keeps the frame the
// picture will fill; the rest are kept to the width their row needs, so sixteen
// of them fit on one card rather than sixteen frames standing side by side. The
// size of a row slot is stated here rather than in the sheet: the sheet sizes
// the pictures a card holds, and a slot is not a picture - it is the shape one
// will come back at, and how many of them stand together is the ask, which only
// this page knows.
function pendingSlot(item, card, leading) {
  const box = document.createElement('div');
  box.className = 'pending-box pending-slot';
  box.style.aspectRatio = String(item.width || 1) + ' / ' + String(item.height || 1);
  if (!leading) {
    box.style.width = '52px';
    box.style.flex = '0 0 auto';
  }
  if (leading) {
    const shape = document.createElement('span');
    shape.className = 'pending-shape';
    shape.textContent = String(item.kind) === 'video'
      ? 'a clip in the making' : 'a picture in the making';
    box.appendChild(shape);
  }
  const bar = document.createElement('div');
  bar.className = 'prog-bar';
  const fill = document.createElement('i');
  bar.appendChild(fill);
  box.appendChild(bar);
  const stop = document.createElement('button');
  stop.className = 'shot-save';
  stop.textContent = 'stop';
  stop.title = 'Stop this picture: on the machine if it runs - keeping any picture it has already drawn - or off the line if its turn has not come.';
  if (!leading) {
    stop.style.fontSize = '10px';
    stop.style.padding = '2px 5px';
  }
  box.appendChild(stop);
  const els = { box: box, fill: fill, stop: stop, row: item.pending };
  card.slots.set(String(item.pending.key), els);
  ui.onPress(stop, function () {
    // Read the job from the row as it stands at the press: this slot was drawn
    // before the machine named the job, and the hand must stop what stands now.
    const standing0 = String((els.row && els.row.job) || '');
    if (standing0 === '') return;
    void (async function () {
      const down = await ui.apiR('/api/jobs/' + encodeURIComponent(standing0) + '/cancel', 'POST');
      if (down.answer) log('Stop sent for this picture.', 'warn');
      else log('This picture is not standing any more: ' + (down.error || ui.askWhy(down)), 'err');
    })().catch(function (failed) {
      log('the stop did not finish: ' + (failed && failed.message ? String(failed.message) : String(failed)), 'err');
    });
  });
  return box;
}

// One slot filled from its row: the bar, the state in its tip, and the reach of
// its stop hand. Nothing is written that already reads right.
function fillSlot(els, row) {
  const box = els.box;
  box.classList.toggle('held', !!row.held);
  box.classList.toggle('queued', !!row.queued && !row.held);
  const pct = row.max > 0 ? Math.min(100, Math.round((row.done / row.max) * 100)) : 0;
  els.fill.style.width = pct + '%';
  box.classList.toggle('measured', pct > 0 || !!row.held);
  let words = row.stage;
  if (row.position) words += ' · ' + row.position;
  if (row.estimateMs > 0) {
    words += ' · about ' + costWords(row.estimateMs) + ' away';
    if (row.basis) words += ' · ' + row.basis;
  } else if (row.noTime) {
    words += ' · ' + row.noTime;
  }
  if (String(box.getAttribute('title') || '') !== words) box.title = words;
  els.stop.disabled = !row.job;
}

// The card's own lines: the words the ask draws with, and one line that states
// the whole ask - how many of its pictures still stand, and the wait for the last
// of them. Each slot carries its own state in its tip.
function fillCard(card, rows) {
  if (!rows.length) return;
  const lead = rows[0];
  if (card.said && card.said.textContent !== lead.prompt) {
    card.said.textContent = ui.clip(lead.prompt, 200);
    card.said.title = lead.prompt;
  }
  if (card.meta) {
    const meta = modelName(lead.model) + (lead.settingsWords ? ' · ' + lead.settingsWords : '');
    if (card.meta.textContent !== meta) card.meta.textContent = meta;
  }
  let line = lead.stage;
  if (lead.position) line += ' · ' + lead.position;
  let wait = 0;
  let allQueued = true;
  let anyHeld = false;
  let anyMeasured = false;
  for (const row of rows) {
    if (row.estimateMs > wait) wait = row.estimateMs;
    if (!row.queued) allQueued = false;
    if (row.held) anyHeld = true;
    if ((row.max > 0 && row.done > 0) || row.held) anyMeasured = true;
  }
  if (rows.length > 1) {
    line += ' · ' + (rows.length - 1) + ' more of this ask still to come';
  }
  if (wait > 0) {
    line += ' · about ' + costWords(wait) + ' away';
    if (lead.basis) line += ' · ' + lead.basis;
  } else if (lead.noTime) {
    line += ' · ' + lead.noTime;
  }
  if (card.queue.textContent !== line) card.queue.textContent = line;
  card.queue.title = 'the ' + rows.length + ' picture' + (rows.length === 1 ? '' : 's') +
    ' of this ask standing here';
  // The card sways as one: only while every slot of it is still waiting is the
  // whole ask unknown how far, and a card whose work has been measured stands
  // still, the measured slot with its own bar.
  card.shot.classList.toggle('held', anyHeld);
  card.shot.classList.toggle('queued', allQueued);
  card.shot.classList.toggle('measured', anyMeasured);
}

// The steps a run this page asks for will take: what its own dial named,
// otherwise what the model's card says the model wants. Both are the machine's
// numbers, and the window of a run whose turn has not come is built from them.
function askedSteps(model) {
  const dialed = String(ui.textField('steps').trim());
  if (dialed !== '') return Number(dialed) || 0;
  const card = findModel(String(model));
  return card && Number(card.steps) > 0 ? Number(card.steps) : 0;
}

// The pictures this page is going to ask for, in the order it asks for them: the
// whole batch is put on the stack at once, so the job that has not been asked
// yet is visible as a queue rather than appearing only when its turn comes. Each
// picture is a job of its own, and the notice of a picture names the place it
// holds in the ask.
function planPictures(model, pictures) {
  const batch = 'b' + (++planSeq);
  const steps = askedSteps(model);
  const ask = describeAsk();
  for (let n = 0; n < pictures; ++n) {
    const entry = {
      key: 'pic-' + batch + '-' + n, batch: batch, n: n, of: pictures,
      model: String(model), job: '', index: -1, asked: false, steps: steps,
      dropped: false, prompt: ask.prompt, settings: ask.settings, fields: ask.fields,
    };
    plannedPictures.push(entry);
  }
  paintPictureNotices(lastJobs);
  return batch;
}

// A batch that will not continue leaves the pictures it never asked for off the
// plan: they were planned, and saying so once is enough, and keeping them would
// read as pictures still to come. A picture whose card had already come down had
// its outcome read from the machine, so it is not described a second time here;
// a picture a run was asked for is not one that was never asked for, and what
// stopped was that run.
function finishPlan(batch, words) {
  if (!batch) return;
  for (const entry of plannedPictures.slice()) {
    if (String(entry.batch) !== String(batch)) continue;
    entry.dropped = true;
    retirePicture(entry, entry.asked ? 'a picture of this ask: its run did not come out' : words,
      entry.asked ? 'err' : 'warn');
  }
}

// The job the machine just took carries the next of the pictures this page
// planned for the batch that ask laid out - an ask names its own batch, so two
// asks standing on the machine at once each keep their own pictures rather than
// one taking the other's place in the queue. The machine's own job id is what
// ties the picture to it from here on, so a page reload finds the same pictures
// at the same notices through the machine's records.
function bindPlannedPictures(batch, jobId, asked) {
  let bound = 0;
  const want = Math.max(1, Math.round(Number(asked) || 1));
  for (const entry of plannedPictures) {
    if (bound >= want) break;
    if (String(entry.batch) !== String(batch)) continue;
    if (String(entry.job) !== '' || entry.asked) continue;
    entry.job = String(jobId);
    entry.asked = true;
    entry.index = bound;
    ++bound;
  }
  return bound;
}

// Take a picture off the standing cards. A picture that landed leaves without a
// word of its own: the record of the render itself comes up in the gallery, and
// two voices for one picture is one too many. A stopped or failed picture says
// so once, in the log - and a failure says it as a toast too, because a failure
// nobody notices is a failure nobody can act on.
function retirePicture(entry, words, kind) {
  const at = plannedPictures.indexOf(entry);
  if (at >= 0) plannedPictures.splice(at, 1);
  if (!words) return;
  log(words, kind === 'err' ? 'err' : (kind === 'warn' ? 'warn' : 'ok'));
  if (kind === 'err') ui.toastPush(ui.clip(String(words), 200), 'err');
}

// The pictures a named run carries, in the order they will be drawn.
function boundPictures(jobId) {
  const found = [];
  for (const entry of plannedPictures) {
    if (String(entry.job) === String(jobId)) found.push(entry);
  }
  found.sort(function (a, b) { return a.index - b.index; });
  return found;
}

// What the last reading of the machine stated, and what a stream followed since:
// the cards are repainted from these without asking the machine again.
let lastJobs = [];
const liveProgress = {};

function repaintPictures() {
  const merged = [];
  const seen = {};
  for (const record of lastJobs) {
    const id = String(record.id);
    const live = liveProgress[id];
    if (live) {
      const copy = Object.assign({}, record);
      copy.done = live.done;
      copy.max = live.max;
      merged.push(copy);
      seen[id] = true;
    } else {
      merged.push(record);
      seen[id] = true;
    }
  }
  for (const id in liveProgress) {
    if (seen[id]) continue;
    const live = liveProgress[id];
    // A run this page is following that the last reading did not know about is the
    // newest thing standing here, and its start is the ask that started it - named
    // in the same whole seconds the machine names its own starts in, so the order
    // the notices are painted in is one order over both.
    const asked = jobTimes['job-' + id] || null;
    merged.push({
      id: id, kind: 'generate', state: 'running', model: live.model,
      // A run its stream is naming steps for stands on the card, which is the
      // place the machine's own number 0 names: without one it would sort
      // behind every row that holds a number, and this card would drop once
      // per stream tick and climb again at the next reading.
      queue_pos: 0,
      started_at: asked && asked.start ? Math.floor(asked.start / 1000) : Math.floor(Date.now() / 1000),
      done: live.done, max: live.max
    });
  }
  paintPictureNotices(merged);
}

// What a run's steps cost on this machine, in the unit the machine counts its work
// in: the ticks it made over the wall time they took. A finished run here is a
// measured number, so an estimate for a picture whose turn has not come is built
// from the work this machine really did rather than from a wish.
function rememberAggregate(model, ticks, ms) {
  if (!(Number(ticks) > 0) || !(ms > 0)) return;
  const key = String(model);
  if (!aggregateRate[key]) aggregateRate[key] = { ticks: 0, ms: 0 };
  aggregateRate[key].ticks += Number(ticks);
  aggregateRate[key].ms += Number(ms);
}

function aggregateRateFor(model) {
  const held = aggregateRate[String(model)];
  if (held && held.ticks > 0 && held.ms > 0) return held.ticks / held.ms;
  return 0;
}

// What the steps of a run this page watched cost, kept as the machine counts them:
// the steps between the first and the last its stream named over the time between
// those two. A run that named only one moment measured nothing, and nothing is
// kept for it.
function rememberMeasuredRun(model, times) {
  if (!times) return;
  const ticks = Number(times.tickDone) - Number(times.firstTicks || 0);
  const ms = Number(times.tickAt) - Number(times.firstAt);
  if (ticks > 0 && ms > 0) rememberAggregate(model, ticks, ms);
}

// The words a time estimate was built on, so a number is never read as a guess.
// The machine's own ledger comes first: it was written by a run that finished at
// these settings on this machine, and it survives the page. A run's own rate is
// nearer, and the rate the runs this page watched held is what is left when the
// machine has no row for these settings. Last stands the pace the run itself has
// kept since the machine took it up - its first tick over the whole time it has
// stood, the model's open included - which names the wait over rather than the
// wait left, and is said as what it is.
function rateBasis(liveRate, model, found, elapsedRate) {
  if (liveRate > 0) return "at this run's own rate";
  if (found && Number(found.row.count) > 0) {
    return found.exact
      ? 'at the cost ' + Number(found.row.count) + ' run' + (Number(found.row.count) === 1 ? '' : 's') +
      ' of these settings had here'
      : 'at the per-step cost its runs here showed';
  }
  if (aggregateRateFor(model) > 0) return 'at the rate its runs here held';
  if (elapsedRate > 0) return 'at the pace it has held since its start';
  return '';
}

// The settings a standing run is working at, as the machine states them beside
// the job it is working. A row of the machine's own cost ledger is keyed by
// exactly these, so a run's own words are the match. A run still queued has no
// settings yet, but it carries the ask it was filed with - its numbers, filled
// onto its model card's own defaults, are the nearest that is not a lie; what
// the dials on this page happen to read is nobody's answer about somebody
// else's run.
function standingAskOf(record) {
  const stated = record.settings;
  if (stated && typeof stated === 'object') {
    return {
      model: String(record.model),
      width: Number(stated.width),
      height: Number(stated.height),
      steps: Number(stated.steps),
      guidance: Number(stated.guidance),
      sampler: String(stated.sampler || 'engine default'),
      scheduler: String(stated.scheduler || 'engine default'),
      cache: stated.cache === true,
      device: String(stated.device || 'card'),
      quant: String(stated.quant || 'as shipped'),
      sage_attn: stated.sage_attn === true,
      batch: Number(stated.batch) > 0 ? Number(stated.batch) : 1,
      refs: Number(stated.refs) || 0,
      frames: Number(stated.frames) || 0,
      fps: Number(stated.fps) || 0,
    };
  }
  const ask = record.ask && typeof record.ask === 'object' ? record.ask : null;
  if (!ask) return null;
  const base = askFields(String(record.model), true);
  if (!base) return null;
  if (Number(ask.width) > 0) base.width = Number(ask.width);
  if (Number(ask.height) > 0) base.height = Number(ask.height);
  if (Number(ask.steps) > 0) base.steps = Number(ask.steps);
  if (Number(ask.guidance) > 0) base.guidance = Number(ask.guidance);
  if (Number(ask.count) > 1) base.batch = Number(ask.count);
  if (Number(ask.frames) > 0) base.frames = Number(ask.frames);
  if (Array.isArray(ask.refs)) base.refs = ask.refs.length;
  return base;
}

// What a run's own steps cost, from the two moments its stream named: the ticks
// it reported at its first step and the ticks it reported at its last. The open
// of the model, the file written after, and the wait between runs are none of
// the run's steps, so none of them belongs in this measure.
function runRateOf(times) {
  if (!times || !(times.firstAt > 0) || !(times.tickAt > times.firstAt)) return 0;
  const steps = Number(times.tickDone) - Number(times.firstTicks || 0);
  if (!(steps > 0)) return 0;
  return steps / (times.tickAt - times.firstAt);
}

// Draw what this machine states about every picture that stands here, as cards
// in the gallery: this page's own asks by their planned cards, and another
// window's asks from the machine's record of them - prompt, settings, and the
// pictures they lean on are what the record itself carries. One job draws one
// picture, so one standing job is one card, and the line the cards come out in
// is the machine's own: what stands on the card first, then the queued ones in
// the places the machine holds for them. The estimates stay honest: a rate is
// this run's own ticks, otherwise the cost this machine measured at these
// settings, otherwise the pace this page has watched, otherwise the pace the
// run has held since it was taken up - a last bound, named as one - and when
// not even that stands, the card says no time rather than making one up.
function paintPictureNotices(jobs) {
  const records = Array.isArray(jobs) ? jobs : [];
  const standing = [];
  const settled = {};
  for (const record of records) {
    if (String(record.kind) !== 'generate') continue;
    const state = String(record.state);
    if (state === 'running' || state === 'queued') {
      // A run of this page's own whose every planned picture has drawn and
      // landed while this reading still calls it standing keeps no card: its
      // records stand as pictures here, and its row comes down with them
      // rather than waiting for a later reading to agree.
      if (String(jobOutcomes['job-' + String(record.id)] || '') === 'done' &&
        boundPictures(String(record.id)).length === 0) continue;
      standing.push(record);
      continue;
    }
    settled[String(record.id)] = state;
  }
  // A run that settled leaves no card behind: what its card said while it ran
  // is not a description of a machine that is done. A landed picture needs no
  // word either - the record of the render itself comes up in the gallery.
  for (const entry of plannedPictures.slice()) {
    const state = settled[String(entry.job)];
    // A picture whose record is being read in this moment: its card is one
    // drawing away from becoming that record, and does not come down in the
    // meantime - a hole where a landing card stood is a blink.
    if (!state || entry.landing) continue;
    retirePicture(entry, state === 'done' ? '' :
      (state === 'cancelled' ? 'a picture stopped on the machine' : 'a picture failed: its run stopped'),
      state === 'done' ? 'ok' : (state === 'cancelled' ? 'warn' : 'err'));
  }
  standing.sort(function (a, b) {
    const pa = Number(a.queue_pos) >= 0 ? Number(a.queue_pos) : Number.MAX_SAFE_INTEGER;
    const pb = Number(b.queue_pos) >= 0 ? Number(b.queue_pos) : Number.MAX_SAFE_INTEGER;
    if (pa !== pb) return pa - pb;
    return (Number(a.started_at) || 0) - (Number(b.started_at) || 0);
  });

  const rows = [];
  for (let at = 0; at < standing.length; ++at) {
    const record = standing[at];
    const entry = boundPictures(String(record.id))[0] || null;
    const row = rowOfRecord(record, entry, at + 1, standing.length);
    if (row) rows.push(row);
  }
  // Planned and not yet standing at the machine by name: the few seconds of a
  // batch being handed over, and a job whose ask was accepted between two
  // readings. The card stands from the ask and never blinks out while the
  // machine is catching up - the record row takes it over when its reading comes.
  const standingJobs = new Set();
  for (const record of standing) standingJobs.add(String(record.id));
  for (const entry of plannedPictures) {
    if (entry.asked && standingJobs.has(String(entry.job))) continue;
    const row = rowOfEntry(entry);
    if (!row) continue;
    if (entry.asked) {
      row.job = String(entry.job);
      row.stage = 'the machine has taken this one';
    }
    if (entry.landing) {
      // The frame is drawn and its record is on its way here: the picture is
      // being written, which is what the machine itself states at this point
      // of a run, and no reading of the room has caught up yet.
      row.held = true;
      row.remaining = 0;
      row.stage = 'its steps are drawn · writing the file';
    }
    const found = costRow(askFields(entry.model));
    const stepMs = found ? stepMsOf(found) : 0;
    const held = aggregateRateFor(String(entry.model));
    row.rate = stepMs > 0 ? 1 / stepMs : held;
    rows.push(row);
  }
  // The wait in front of each card, in order, at the best rate any of them
  // commands: a job whose own settings were never timed borrows the rate a
  // standing run of the same work just showed, rather than saying nothing when
  // the page is holding the number in its hand.
  let shared = 0;
  for (const row of rows) shared = row.rate > shared ? row.rate : shared;
  let workAhead = 0;
  for (const row of rows) {
    const rate = row.rate > 0 ? row.rate : shared;
    row.estimateMs = rate > 0 ? Math.round((workAhead + row.remaining) / rate) : 0;
    if (!(row.estimateMs > 0)) row.noTime = 'its time is not measured here yet';
    workAhead += row.remaining;
  }

  pendingRows = rows;
  renderGallery(galleryItems);
  paintPending(rows);
  paintStandingHeader(standing, rows, records);
}

// The settings row of a standing record, from what the machine carries about it:
// the ask snapshot stood the moment the ask was made, and the full settings
// written when its run took the card. Only named numbers are spoken of.
function recordFields(record) {
  const ask = record.ask || {};
  const stated = record.settings || {};
  const card = findModel(String(record.model)) || {};
  const words = [];
  const steps = Number(ask.steps || stated.steps) || 0;
  if (steps > 0) words.push('steps ' + steps);
  const width = Number(ask.width || stated.width) || 0;
  const height = Number(ask.height || stated.height) || 0;
  if (width > 0 && height > 0) words.push('at ' + width + 'x' + height);
  const guidance = Number(ask.guidance || stated.guidance) || 0;
  if (guidance > 0) words.push('guidance ' + guidance);
  const seed = Number(ask.seed);
  if (Number.isFinite(seed) && seed >= 0) words.push('seed ' + seed);
  const refs = Array.isArray(ask.refs) ? ask.refs : [];
  if (refs.length) words.push('from ' + refs.length + (refs.length === 1 ? ' picture' : ' pictures'));
  return {
    width: width, height: height, steps: steps, guidance: guidance,
    negative: String(ask.negative_prompt || ''),
    frames: Number(ask.frames || stated.frames) || 0,
    fps: Number(stated.fps) || Number(card.fps) || 0,
    refUrls: Array.isArray(ask.ref_urls) ? ask.ref_urls : [],
    video: card.video === true,
    words: words.join(' · '),
  };
}

// One standing run's card row: what it is, where it stands, how far it has got,
// and what the wait in front of it is worth in the machine's own steps.
function rowOfRecord(record, entry, place, standingCount) {
  const id = String(record.id);
  const fields = entry && entry.fields ? entry.fields : recordFields(record);
  const queued = String(record.state) === 'queued';
  const done = Number(record.done) || 0;
  const max = Number(record.max) || 0;
  const unmeasured = record.done === undefined || record.done === null;
  const window = max || Number(fields.steps) || 0;
  const remaining = max > 0 ? Math.max(0, max - done) : window;
  const model = String(record.model);
  const times = jobTimes['job-' + id] || null;
  const runRate = runRateOf(times);
  const found = costRow(standingAskOf(record));
  const stepMs = found ? stepMsOf(found) : 0;
  const heldRate = aggregateRateFor(model);
  // The pace the run has kept since the machine took it up: the model's open is
  // inside it, so it names the wait over rather than the wait left - a bound,
  // said as one, rather than no number at all.
  const since = Number(record.started_at) || 0;
  const elapsedMs = since > 0 ? Date.now() - since * 1000 : 0;
  const elapsedRate = done > 1 && elapsedMs > 0 ? done / elapsedMs : 0;
  const rate = runRate > 0 ? runRate
    : (stepMs > 0 ? 1 / stepMs : (heldRate > 0 ? heldRate : elapsedRate));
  const held = !queued && !unmeasured && max > 0 && done >= max;
  let stage;
  if (queued) {
    stage = standingCount > 1 ? 'waiting its turn' : 'waiting its turn at the card';
  } else if (unmeasured) {
    stage = 'its steps are not counted here';
  } else if (done <= 0) {
    stage = 'opening what it draws with';
  } else if (held) {
    stage = 'its steps are drawn · writing the file';
  } else {
    stage = 'drawing';
  }
  let refThumbs = fields.refUrls || [];
  if (entry && entry.fields && !refThumbs.length) {
    refThumbs = [];
    for (const refId of entry.fields.refs) {
      const url = refThumbUrl(refId);
      if (url !== '') refThumbs.push(url);
    }
  }
  return {
    key: entry ? entry.key : pictureKeyFor(id, 0),
    job: id, model: model,
    prompt: entry ? entry.prompt : String((record.ask && record.ask.prompt) || ''),
    settingsWords: entry ? entry.settings : fields.words,
    width: fields.width, height: fields.height, steps: fields.steps,
    guidance: fields.guidance, negative: fields.negative,
    frames: fields.frames, fps: fields.fps, video: fields.video === true,
    refThumbs: refThumbs,
    queued: queued, held: held,
    done: done, max: max, remaining: remaining, rate: rate,
    stage: stage, estimateMs: 0, noTime: '',
    position: entry ? picturePosition(entry, place, standingCount) : '',
    basis: queued ? '' : rateBasis(runRate, model, found, elapsedRate),
  };
}

// A card for a picture this page planned before any run of its own stands: the
// same shape, the same stack, and a state that says its turn has not come.
function rowOfEntry(entry) {
  if (!entry.fields) return null;
  const fields = entry.fields;
  const thumbs = [];
  for (const refId of fields.refs) {
    const url = refThumbUrl(refId);
    if (url !== '') thumbs.push(url);
  }
  return {
    key: entry.key, job: '', model: String(entry.model),
    prompt: entry.prompt, settingsWords: entry.settings,
    width: fields.width, height: fields.height, steps: fields.steps,
    guidance: fields.guidance, negative: fields.negative,
    frames: fields.frames, fps: fields.fps, video: fields.video === true,
    refThumbs: thumbs,
    queued: true, held: false, done: 0, max: 0,
    remaining: Number(entry.steps) || 0, rate: 0,
    stage: 'not asked for yet', estimateMs: 0, noTime: '',
    position: entry.of > 1
      ? 'picture ' + (Number(entry.n) + 1) + ' of ' + entry.of + ' asked together' : '',
    basis: '',
  };
}

// Fill the standing cards from their rows, and let go of the cards whose
// pictures no longer stand. The rows are gathered under the ask they belong to
// first: a card is filled once, from all of the slots of that ask, so the line
// under the picture states the ask and not one of its pictures.
function paintPending(rows) {
  const byCard = new Map();
  for (const row of rows) {
    const key = stackKeyFor(pendingItem(row));
    const held = byCard.get(key);
    if (held) held.push(row);
    else byCard.set(key, [row]);
  }
  for (const key of Array.from(pendingEls.keys())) {
    if (!byCard.has(key)) pendingEls.delete(key);
  }
  for (const key of byCard.keys()) {
    const card = pendingEls.get(key);
    if (!card) continue;
    const held = byCard.get(key);
    for (const row of held) {
      const els = card.slots.get(String(row.key));
      if (els) fillSlot(els, row);
    }
    // A slot whose picture no longer stands is let go of where it stands: its
    // hand stops being a hand rather than sending a cancel for a job that is
    // already off the machine. The card holding it is drawn again by the next
    // room with the slots of the pictures that stand now - the standing keys are
    // part of the card's stamp, so that drawing is not skipped - and until it
    // is, this hand lies dead beside its dead job instead of pretending.
    for (const slotKey of Array.from(card.slots.keys())) {
      let stands = false;
      for (const row of held) {
        if (String(row.key) === slotKey) { stands = true; break; }
      }
      if (stands) continue;
      const stale = card.slots.get(slotKey);
      card.slots.delete(slotKey);
      if (stale) {
        stale.row = null;
        stale.stop.disabled = true;
      }
    }
    fillCard(card, held);
  }
}

// The header carries the working render: its share of steps, its model, and the
// wait it states - every window reads the same run from the same record. With
// renders queued it names the line, and with nothing of the image engine's
// standing it hands the header back to the other engines' own state.
function paintStandingHeader(standing, rows, records) {
  let run = null;
  for (const record of standing) {
    if (String(record.state) === 'running') { run = record; break; }
  }
  if (run) {
    const id = String(run.id);
    let row = null;
    for (const candidate of rows) {
      if (candidate.job === id) { row = candidate; break; }
    }
    const done = Number(run.done) || 0;
    const max = Number(run.max) || 0;
    const pct = max > 0 ? Math.min(100, Math.round((done / max) * 100)) : 0;
    let text = 'drawing · ' + modelName(String(run.model));
    if (max > 0) text += ' · step ' + done + '/' + max;
    if (row && row.estimateMs > 0) text += ' · about ' + costWords(row.estimateMs) + ' left';
    ui.headProgress(pct, text, 'image', '', max <= 0);
    return;
  }
  if (standing.length) {
    ui.headProgress(0, 'queued · ' + standing.length + ' waiting for the card · ' +
      modelName(String(standing[0].model)), 'image');
    return;
  }
  if (plannedPictures.length) {
    ui.headProgress(0, 'starting · ' + modelName(String(plannedPictures[0].model)), 'image');
    return;
  }
  ui.headerState(records);
}

// How a job ended, read from what the job's own stream said: a batch of runs is
// several jobs one after another, and the page needs the answer to "did this one
// finish, fail, or was it stopped" to know whether to start the next one. The
// stream's own words come first; a stream that closed without stating an ending
// is followed at the job's record door instead (see streamJob), because the
// machine settles a run in its record and a picture frame is not the only way
// that ending reaches a page. Nothing here is guessed: an ending neither the
// stream and no record door stated reads as no ending, and the batch stops
// rather than carry on past a run nobody explained.
const jobOutcomes = {};

function handleEvent(event, model, key) {
  const jobId = key && key.indexOf('job-') === 0 ? key.slice(4) : '';
  const mine = jobId ? boundPictures(jobId) : [];
  // A render's stream speaks to the gallery cards and the header; every other
  // piece of work this page starts - a model being opened, its bytes coming
  // down - has no card to fill, and its toast is the only place that tells it.
  const kind = jobTimes[key] ? String(jobTimes[key].kind || '') : '';
  if (event.type === 'progress') {
    const done = Number(event.done);
    const max = Number(event.max);
    const times = jobTimes[key] || (jobTimes[key] = { start: Date.now() });
    // The two moments a rate is measured from: the first step the machine reported
    // and the last, since the steps between those two over the time between them
    // are the run's work and nothing else is.
    if (!times.firstAt) {
      times.firstAt = Date.now();
      times.firstTicks = done;
    }
    times.max = max > 0 ? max : times.max;
    times.tickAt = Date.now();
    times.tickDone = done;
    if (kind === 'generate') {
      // The machine's own ticks of this run are kept so the cards are repainted
      // from the stream itself - the next reading of the machine would state the
      // same numbers a few seconds later. The header moves with them.
      liveProgress[jobId] = { model: String(model), done: done, max: max };
      repaintPictures();
      return;
    }
    // The estimate comes from what this machine has really done: this run's own
    // rate once it has taken more than one step, otherwise the cost it measured
    // for these settings, otherwise the rate the runs this page watched held.
    // None of them is a wish.
    const live = ui.toastTrack(key, '', 'wait');
    let rate = runRateOf(times);
    if (rate <= 0) {
      const found = costRow(askFields(model));
      const stepMs = found ? stepMsOf(found) : 0;
      if (stepMs > 0) rate = 1 / stepMs;
    }
    if (rate <= 0) rate = aggregateRateFor(model);
    let words = 'Working ' + modelName(model);
    if (max > 0 && done > 0) words += ' - step ' + done + ' of ' + max;
    if (rate > 0 && max > done && done > 0) {
      ui.toastEta(live, Math.round(((max - done) / rate)), words);
    } else {
      ui.toastStep(live, done, max, words);
      // work that has stated no step yet states no scale either: it waits.
      if (!max || done <= 0) ui.toastOpen(live);
    }
    return;
  }
  if (event.type === 'image' || event.type === 'video') {
    log('Render saved: ' + String(event.clip || event.url), 'ok');
    if (key) {
      // What the run's steps cost is kept from the moments its own stream named.
      // A picture's event names no steps at all, so the run's own count is the
      // only honest source of what its steps cost.
      rememberMeasuredRun(model, jobTimes[key]);
      jobLanded[key] = (Number(jobLanded[key]) || 0) + 1;
      jobOutcomes[key] = 'done';
      // The card of this frame stands while its record is read in: it comes
      // down only at the drawing that puts the picture in its place, so a
      // landing picture never leaves a hole for a beat. The cards of the run's
      // later pictures are not this frame's to retire, and stay.
      if (mine.length) mine[0].landing = true;
    }
    void landRender(String(event.url || event.clip || ''),
      mine.length ? mine[0] : null);
    return;
  }
  if (event.type === 'error') {
    log('error: ' + String(event.message), 'err');
    if (key) {
      delete jobTimes[key];
      delete liveProgress[jobId];
      jobOutcomes[key] = 'error';
      if (kind === 'generate') {
        // The run stopped at a step: its picture says what it stopped as, in the
        // machine's own words, and its card comes down saying so.
        for (const entry of mine) {
          retirePicture(entry, 'its run failed: ' + String(event.message), 'err');
        }
        repaintPictures();
      } else {
        ui.toastEnd(ui.toastTrack(key, 'Job failed: ' + String(event.message), 'err'),
          'Job failed: ' + String(event.message), 'err');
      }
    }
    return;
  }
  if (event.type === 'cancelled') {
    log('job cancelled', 'warn');
    if (key) {
      delete jobTimes[key];
      delete liveProgress[jobId];
      jobOutcomes[key] = 'cancelled';
      if (kind === 'generate') {
        for (const entry of mine) {
          retirePicture(entry, 'stopped on the machine', 'warn');
        }
        repaintPictures();
      } else {
        ui.toastEnd(ui.toastTrack(key, 'Stopped', 'warn'), 'Stopped', 'warn');
      }
    }
  }
}

// The record of a render this page watched comes straight up out of the machine:
// the file it names is the record's whole address, and waiting a poll cycle to
// see a picture that has already landed is a wait nobody asked for. A record
// that cannot be read falls back to walking the gallery's front page, which
// states the truth a moment later. The planned card of the frame - when this
// page asked for it - stands until the record is in the list, and comes down in
// the same drawing the picture goes up.
async function landRender(url, entry) {
  const name = String(url || '').split('/').pop().replace(/\.[A-Za-z]+$/, '');
  let record = null;
  if (name) record = await ui.api('/api/gallery/' + encodeURIComponent(name));
  if (record && !record.error && String(record.id || '') !== '') {
    const id = String(record.id);
    if (!galleryItems.some(function (item) { return String(item.id) === id; })) {
      // Put where its own moment puts it, not blindly on top: two pictures of
      // one batch can finish their slow reads in either order. A moment holds
      // whole seconds, and two pictures of one of those seconds stand at the
      // machine by the moment it wrote them - the order their frames landed -
      // so this one goes above the ones it shares its second with, not below.
      const created = Number(record.created) || 0;
      let at = 0;
      while (at < galleryItems.length &&
        (Number(galleryItems[at].created) || 0) > created) ++at;
      galleryItems = galleryItems.slice(0, at).concat([record], galleryItems.slice(at));
      galleryHeld += 1;
      log('Landed: ' + id, 'ok');
    }
    if (entry) retirePicture(entry, '', 'ok');
    // The rows are rebuilt from the state that now stands: the run this frame
    // drew is told off with it, so the card turns into its picture in one
    // drawing. Drawing the list straight from the last rows would stand the
    // frame twice - once as the card, once as the record - until a later
    // reading of the machine noticed.
    repaintPictures();
    return;
  }
  if (entry) retirePicture(entry, '', 'ok');
  await refreshGallery();
}

// Every piece of work this page starts lives on the timed stack while it runs: its
// notice names the work in plain words, its bar counts the steps the job states or
// estimates its time left from the rate the machine really runs at, and its words
// follow the job through. A render is followed picture by picture instead - the
// pictures this page asked for are on the stack from the ask itself, and the run
// fills their notices in - so nothing is stacked here for a render's job. Work
// that has not stated a step yet keeps the moving bar the page's own progress
// panel draws, and says it is waiting rather than inventing a time. What comes
// back is how the job ended - 'done', 'error', 'cancelled', or '' when it never
// said - which is what a batch of runs reads to decide whether to start the next.
async function streamJob(job, model, action) {
  const key = 'job-' + String(job);
  // What kind of work this is, kept beside its moments: a render's stream speaks
  // to the gallery cards, everything else to its toast, and the stream itself
  // does not say which it is.
  jobTimes[key] = { start: Date.now(), kind: String(action || '') };
  // The card of the model this work is on: what is already on this machine and
  // what its opens cost on this one are stated there, not by a model's name.
  const card = findModel(String(model)) || {};
  // A model whose bytes are already on this machine is not fetched again: the job
  // reads them and stands a voucher under them. Calling that "Downloading" over a
  // bar that never moved a byte made every check look like a re-fetch of the file
  // it had just found, which is how a download that was not one came to be
  // described as a broken download.
  const checking = action !== 'generate' && action !== 'load' &&
    (card.unchecked === true || Number(card.present_bytes) > 0);
  if (action !== 'generate') {
    const live = ui.toastTrack(key,
      action === 'load' ? 'Loading ' + modelName(model)
        : (checking ? 'Checking ' : 'Downloading ') + modelName(model),
      'wait');
    if (action === 'load') {
      // An open costs what the opens of this model cost on this machine, which the
      // machine keeps beside the precision it opened them at - a 16 GB open and a
      // 2 GB one are not the same wait, and the pair of numbers is what a person
      // watching the bar wants. An open nobody has timed here keeps the open-ended
      // bar and says no time.
      const open = openCost(card.id, card.uses_quant || card.asked_quant || '');
      if (Number(open.ms) > 0) {
        ui.toastEta(live, open.ms, 'Loading ' + modelName(model) + ' · ' + open.count +
          ' open' + (open.count === 1 ? '' : 's') + ' of this form here cost about ' +
          costWords(open.ms) + ' on average');
      } else {
        ui.toastOpen(live);
      }
    } else {
      ui.toastOpen(live);
    }
  }
  const stream = await ui.streamEvents('/api/jobs/' + encodeURIComponent(job) + '/events',
    function (event) { handleEvent(event, model, key); });
  if (stream === false) {
    log('job ' + job + ' has no event stream', 'err');
    delete jobTimes[key];
    ui.toastEnd(ui.toastTrack(key, 'This job stopped without an answer.', 'err'),
      'This job stopped without an answer.', 'err');
    finishJob();
    // A reading that fails here is the machine being quiet, not this job's
    // ending: the run is already told off, and the next reading says whatever
    // the machine says then. A batch must not die on a redraw.
    try {
      await refresh();
    } catch (gone) {
      log('the machine stopped answering while this job closed: the next reading picks it up', 'err');
    }
    return 'error';
  }
  // The wire broke part way through the run. This is said once, in plain words,
  // and the ending is then asked of the job's record door below - the machine
  // still holds the answer under the job's own name - rather than guessed from
  // the silence. Nothing stands hung: the work is told off, the run closes with
  // what was really claimed for it, and a batch moves on to the next job.
  let streamDied = false;
  if (stream === 'dead') {
    streamDied = true;
    log('the stream for job ' + job + ' broke part way through: asking the machine how it ended', 'err');
    if (action !== 'generate') {
      ui.toastEnd(ui.toastTrack(key, 'The reading broke part way through.', 'err'),
        'The reading broke part way through: the machine is asked how this job ended.', 'err');
    }
  }
  // A model opened or a file downloaded is worth confirming the moment it is
  // done; a render's own notice is taken down by the frame that carries the
  // picture, which is where the work says it finished. A run whose stream broke
  // is not confirmed as done by a wire that died: its notice already said so.
  if (action !== 'generate' && !streamDied) {
    ui.toastEnd(ui.toastTrack(key,
      (action === 'load' ? 'Model loaded: ' : (checking ? 'Checked: ' : 'File ready: ')) + modelName(model), 'ok'),
      (action === 'load' ? 'Model loaded: ' : (checking ? 'Checked: ' : 'File ready: ')) + modelName(model), 'ok');
  }
  finishJob();
  // What the run's steps cost is kept from the moments its own stream named - its
  // whole window over the ask's wall time would count the open of a model and the
  // wait before a run as the run's steps, which they are not.
  rememberMeasuredRun(model, jobTimes[key]);
  delete jobTimes[key];
  delete liveProgress[String(job)];
  // An ending is a change of fact: what runs costs now has a new row, and a
  // model that just opened or came down is no longer what the cards say.
  needHeavy = true;
  try {
    await refresh();
  } catch (gone) {
    log('the machine stopped answering while this job closed: the next reading picks it up', 'err');
  }
  // What the machine said about how this job ended. The stream's own words come
  // first. A stream that closed without ever stating one is followed at the job's
  // record door, which states finished, failed or stopped for the job by its own
  // name: a run that reached its last step and whose frames never reached this
  // browser is a run the machine settled, and leaving a batch at that reads as
  // three asks that were never made. The wait is bounded - ten minutes, the room
  // a run of this machine's own cap takes at its own pace - and a run still
  // working when that bound is reached says so as no ending, which stops the
  // batch rather than starting another run on top of a run that has not finished.
  let ended = jobOutcomes[key] || '';
  delete jobOutcomes[key];
  if (ended === '') {
    const asked = encodeURIComponent(job);
    for (let follow = 0; follow < 300 && ended === ''; ++follow) {
      const record = await ui.apiR('/api/jobs/' + asked);
      const state = record.answer && record.answer.state ? String(record.answer.state) : '';
      if (state === 'done' || state === 'error' || state === 'cancelled') ended = state;
      else if (state !== 'running' && state !== 'queued') break;
      await ui.sleepWindow('/api/jobs/' + asked, 400, 5000);
    }
  }
  return ended;
}

async function startJob(model, action, extra) {
  // Downloads go to the model's own route, like loads: there is no bare
  // /api/download in the API, so a bare path would 404. Only these two hands
  // come through here - a drawing hand has its own ask, with a plan behind it.
  // The name is carried as one escaped address segment: the monitor page asks
  // this same door with encodeURIComponent, and a name holding a space, a plus,
  // or a percent sign reaches the door as the name it is either way, not as a
  // mangled one the door answers about something else.
  const askedModel = encodeURIComponent(String(model));
  const path = action === 'download' ? '/api/models/' + askedModel + '/download'
    : '/api/models/' + askedModel + '/load';
  const body = action === 'download' ? { model: model } : {};
  if (extra) {
    for (const key in extra) body[key] = extra[key];
  }
  const started = await ui.postJob(path, body);
  if (!started.job) {
    log(action + ' refused' + (started.status ? ' ' + started.status : '') +
      (started.error ? ': ' + started.error : ''), 'err');
    ui.toastNo(action === 'load' ? 'load that model' : 'download that model', started);
    return 'error';
  }
  log('Job ' + started.job + ' started', 'ok');
  // The header shows something running the moment the click happens, before the
  // job's first step.
  ui.headProgress(0, action + ' · ' + model, 'image');
  return await streamJob(started.job, model, action);
}

// A hand that takes its ask and walks away: the job is followed elsewhere, but
// the ask itself is never left silent. A start the door refused, or a follow
// that broke somewhere unexpected, says so in the page's own log with the
// words it carries - a hand that was refused is a fact the page states, not a
// click that did nothing at all.
function handAsk(model, action, extra) {
  startJob(model, action, extra).catch(function (failed) {
    log(action + ' did not finish: ' +
      (failed && failed.message ? String(failed.message) : String(failed)), 'err');
  });
}

// The stack of toasts keeps only the work that has no card: a model being
// opened, bytes coming down. Renders stand in the gallery, not here.
ui.toastTrackedCap = 8;

// What an ask needs before it runs: the model the page picked has to be on this
// machine. A model the machine does not name is refused at its own door, so there
// is nothing to say about it here.
function readyToDraw(model) {
  for (const entry of models) {
    if (entry.id !== model) continue;
    if (entry.on_disk !== true) {
      log(entry.name + ' is not downloaded yet: press download on its card.', 'warn');
      ui.toastPush('That model is not on the machine yet. Press download on its card.', 'warn');
      return false;
    }
    return true;
  }
  return true;
}

// The ask every generate hand takes: one picture, or the quick batches. Every
// picture of an ask is a job of its own at the machine - its own line in the
// queue, its own slot with its own bar and stop hand on the card of this ask -
// so the X4 hand is four jobs and the X16 hand sixteen, drawn one after
// another, and stopping one job touches none of the others. The whole ask is
// handed over at once; each picture lands as its own record and takes its slot's
// place on the card of its ask when it does. Each job is then followed by its
// own name to its own ending, and what came out is counted from the pictures
// that really landed, and from a job the machine says finished, never from
// wishes.
// The model this ask draws with is the one this page has picked: the ask's own
// words name that pick, so the hand carries no model that another pick would
// answer for.
const jobLanded = {};
const jobBound = {};

async function drawAsk(pictures) {
  const model = modelChoice();
  if (!model) {
    log('Pick a model first.', 'warn');
    return;
  }
  if (!(pictures > 0) || !readyToDraw(model)) return;
  // The ask a job takes is the ask this model keeps: what the controls hold now
  // is written down for it, so a reload of this page opens on what was last asked
  // of this model rather than on a card's opening words.
  keepAsk(model);
  const batch = planPictures(model, pictures);
  log('An ask of ' + pictures + ' picture' + (pictures === 1 ? '' : 's') + ' with ' + modelName(model) +
    (pictures === 1 ? ', as one job.'
      : ', as ' + pictures + ' separate jobs, drawn one after another.'), 'ok');
  // Hand every job of the batch to the machine first, so all its job records
  // stand from the start and a refusal says so while the rest of the batch is
  // still whole enough to talk about.
  const jobs = [];
  for (let n = 0; n < pictures; ++n) {
    const started = await ui.postJob('/api/generate', payload());
    if (!started.job) {
      log('Render refused' + (started.status ? ' ' + started.status : '') +
        (started.error ? ': ' + started.error : ''), 'err');
      ui.toastNo('start the image', started);
      finishPlan(batch, 'never asked for: the machine refused here');
      finishJob();
      paintPictureNotices(lastJobs);
      return;
    }
    // The job the machine just took carries the next of the pictures this page
    // planned for the batch, and the machine's own job id is what ties it to
    // that picture from here on - one job, one picture, in this order. The
    // slot stands with its stop hand from this moment: the line at the machine
    // is the line the slots show.
    bindPlannedPictures(batch, started.job, 1);
    jobBound[String(started.job)] = 1;
    jobs.push(started.job);
    if (jobs.length === 1) running = started.job;
    paintPictureNotices(lastJobs);
  }
  // Follow the batch job by job, each by its own name. The machine draws them in
  // the order it holds them; a job that failed or was stopped is counted as what
  // it was - the picture of it that really landed - and never hides the
  // pictures that did come out.
  let drawn = 0;
  for (let k = 0; k < jobs.length; ++k) {
    running = jobs[k];
    // One job's follow breaking is one job's news: the row is said, the
    // picture of it counts as what it really was, and the batch still goes on
    // to the jobs the machine already holds. A throw is not allowed to take
    // the whole ask - and the plan items and notices it still stands on -
    // down with it.
    let ended = '';
    try {
      ended = await streamJob(jobs[k], model, 'generate');
    } catch (broke) {
      log('the run broke out of its follow: ' +
        (broke && broke.message ? String(broke.message) : String(broke)), 'err');
    }
    // A job that showed its picture is counted by the frames that landed - a
    // job stopped before its picture came ends without one, and its own
    // frames are the only honest count. A job that ended as done without a
    // single frame named is a stream that died silent, and the picture it was
    // asked for is all that can be claimed for it.
    const landed = Number(jobLanded['job-' + jobs[k]]) || 0;
    drawn += landed > 0 ? landed
      : ended === 'done' ? Math.max(1, Number(jobBound[String(jobs[k])]) || 1) : 0;
  }
  finishJob();
  finishPlan(batch, 'never drawn');
  if (drawn === pictures) {
    log('The ask is drawn: ' + drawn + ' pictures in the gallery.', 'ok');
  } else if (drawn > 0) {
    log('The ask ended short: ' + drawn + ' of ' + pictures + ' pictures drawn.', 'warn');
  } else {
    log('None of the ' + pictures + ' pictures came out.', 'warn');
  }
}

// One picture of the current ask, on the stack before the machine is asked for it.
async function generate() {
  await drawAsk(askedPictures());
}

// The quick batches: the same ask, drawn again and again.
async function generateMany(pictures) {
  await drawAsk(pictures);
}

// The machine's state is drawn first, from one snapshot: /api/monitor returns
// the system facts, every engine's jobs, and the counts in a single answer, so
// the header and chips answer before models, gallery, and kept pictures have
// been requested. The same painting also runs from the browser's kept reading
// of an earlier visit, which needs no server.
function drawStatus(snapshot) {
  ui.monitorChrome(snapshot);

  const sys = (snapshot && snapshot.system) || {};
  const weights = (sys.loaded && sys.loaded.loaded) ? sys.loaded : {};
  loadedSet.clear();
  for (const id of weights.loaded || []) loadedSet.add(String(id));
  const lru = weights.lru || [];
  pickedModel = lru.length ? String(lru[lru.length - 1]) : '';

  const jobs = (snapshot && Array.isArray(snapshot.jobs)) ? snapshot.jobs : [];
  lastJobs = jobs;
  // The queue pass owns the header: every standing render reaches it, and with
  // nothing of the image engine's standing it hands the header to the state of
  // the other engines. The cards are repainted from the machine's own records on
  // every reading, so the pictures that stand here - this page's or another
  // window's - are stated after a page reload, after a run finishes, and after
  // work asked for elsewhere appears.
  paintPictureNotices(jobs);
}

// The room is filled rather than a window moved: the newest page is asked for on
// every reading (a render that just landed is what belongs at the top), and only
// the pages the room has beyond what this client already holds are asked for, in
// batches. The machine hands its records out a page at a time - one answer holding
// every record would be the room's own words at once - but the client shows all of
// them, so the asks for the pages past the first are started together and awaited
// as a batch; nothing about one page depends on the next. The list grows as each
// batch lands, and repeats of one ask stack across the whole room.
let roomWalk = Promise.resolve();
function refreshGallery() {
  // One room walk at a time: two walks in flight commit two readings over each
  // other, and a record only the other walk had seen stands gone from this
  // list for a beat between the commits - and a card that goes out and comes
  // back is a blink.
  roomWalk = roomWalk.then(walkRoom, walkRoom);
  return roomWalk;
}
async function walkRoom() {
  // The newest page is asked for past the cache - and past the copy a poll made
  // a moment ago: this is exactly the reading that must not be answered with a
  // memory from before a render landed. (The held copy is what made a page that
  // came back from elsewhere stand on the world as it was before the renders
  // finished, sometimes for a full heavy round.)
  const first = await ui.api(galleryPageUrl(0), 'GET', undefined, { cache: 'no-store' });
  if (!first) return false;
  const seen = new Set();
  const merged = [];
  const take = function (rows) {
    for (const row of rows || []) {
      const id = String(row.id);
      if (seen.has(id)) continue;
      seen.add(id);
      merged.push(row);
    }
  };
  take(first.items);
  let held = Number(first.total || merged.length);
  let failed = false;
  // What this client carries that the machine's pages do not takes its place
  // inside the pages: it stands above the first record strictly older than
  // itself - and above the records of its own second, because the machine lays
  // the pictures of one second down by the moment it wrote them, and what no
  // fetched page holds yet was written after the ones it does. One place, not
  // a jump over a whole page: what landed in flight is stated where its
  // moment puts it, and the page that later carries it says the same. Walking
  // from the oldest keeps the records that landed together in their own order
  // too. A walk that has named the room's whole count without a page refusing
  // has spoken of every record the machine holds - what this client still
  // carries beside it is gone from the room, a delete said in another window,
  // and it rides no further; until a walk proves that, an unseen record is
  // only unvisited, and its place is kept. A record this page deleted stays
  // gone either way: carried back, it stood as a ghost whose picture was no
  // longer there.
  const carried = function () {
    const whole = merged.slice();
    const proven = !failed && merged.length >= held;
    for (let back = galleryItems.length - 1; back >= 0; --back) {
      const item = galleryItems[back];
      const id = String(item.id);
      if (seen.has(id) || deletedShots.has(id) || proven) continue;
      const created = Number(item.created) || 0;
      let into = 0;
      while (into < whole.length &&
        (Number(whole[into].created) || 0) > created) ++into;
      whole.splice(into, 0, item);
    }
    return whole;
  };
  const commit = function () {
    const whole = carried();
    galleryItems = whole;
    galleryHeld = Math.max(held, whole.length);
    galleryOffset = 0;
    renderGallery(galleryItems);
  };
  commit();
  // The room is walked by the machine's own windows: the offsets asked with are
  // the machine's positions, so records this client carries beside them cannot
  // make a window skip its turn - and no window is asked twice. A page that
  // refused is reported, and the walk stops when the machine's count is held.
  let walked = first.items ? first.items.length : 0;
  while (walked < held) {
    const starts = [];
    for (let step = 0; step < kGalleryRoomBatch; ++step) {
      const at = walked + step * kGalleryPage;
      if (at < held) starts.push(at);
    }
    if (!starts.length) break;
    // Every page of the room is asked at once, so each ask carries its own
    // truth in its own result: a window that refused says so by its own
    // status, and no reading beside it writes over what this one heard.
    const answers = await Promise.all(starts.map(function (at) {
      return ui.apiR(galleryPageUrl(at));
    }));
    for (let step = 0; step < answers.length; ++step) {
      const answer = answers[step] && answers[step].answer;
      if (walked + step * kGalleryPage >= held) continue;
      if (!answer) { failed = true; continue; }
      take(answer.items);
      if (answer.total !== undefined && answer.total !== null) held = Number(answer.total);
    }
    walked += starts.length * kGalleryPage;
    commit();
  }
  if (failed) return false;
  return merged.length >= held;
}

// The door for one page of the room: the window is named by where it starts,
// counted from the newest render, and how many records it takes.
function galleryPageUrl(offset) {
  const start = Math.max(0, Math.floor(offset));
  return '/api/gallery?limit=' + kGalleryPage + '&offset=' + start;
}

// The talking between this browser and the machine is the expensive thing, so
// one thing is asked on every reading - the monitor, which answers about the
// machine, every engine's jobs, and the counts at once - and everything else
// comes when the thing it describes changes: a render lands, an open or a
// download finishes, a cost table turns a minute old. Renders standing ask the
// gallery's front page again, because that is where a picture lands; a page
// that has walked the whole room pays one request for that asking.
let heavyAt = 0;
const kHeavyMs = 60000;
let needHeavy = true;

async function refresh() {
  const read = await ui.apiR('/api/monitor');
  if (!read.answer) {
    if (read.status === 401 || read.status === 403) throw new Error('the key was refused here (' + read.status + ')');
    throw new Error('/api/monitor returned nothing');
  }
  const snapshot = read.answer;
  drawStatus(snapshot);
  let standing = false;
  const rows = (snapshot && Array.isArray(snapshot.jobs)) ? snapshot.jobs : [];
  for (const job of rows) {
    if (String(job.kind) !== 'generate') continue;
    const state = String(job.state);
    if (state === 'running' || state === 'queued') { standing = true; break; }
  }
  // Does the machine still keep the same number of renders this screen shows?
  // When the two part ways, something landed or went away while this page was
  // not looking - a render that finished while the person stood on another
  // page, or a delete from another window - and the room is asked again now,
  // not whenever the next heavy round happens to fall due. The room's own
  // count is enough to notice this, and one line of it costs no more than a
  // thumbnail; a job that stands already asks the room every reading.
  let desynced = false;
  if (!standing) {
    const tell = await ui.api('/api/gallery?limit=1&offset=0');
    const keptNow = tell && tell.total !== undefined ? Number(tell.total) : -1;
    desynced = keptNow >= 0 && keptNow !== galleryHeld;
  }
  const now = Date.now();
  const heavy = needHeavy || now - heavyAt > kHeavyMs;
  if (heavy) {
    needHeavy = false;
    heavyAt = now;
    // What this machine says its own work costs, asked before the cards are
    // drawn so they can state it. A door that answers nothing - an older build
    // of the machine that has no such door, or one that refused this ask -
    // leaves the last reading standing: the costs are facts about this machine,
    // not something to forget because one answer did not come.
    const costs = await ui.api('/api/timings');
    if (costs) {
      costRows = Array.isArray(costs.generations) ? costs.generations : [];
      openRows = Array.isArray(costs.loads) ? costs.loads : [];
    }
    // The card's full name, asked once and kept: it is a fact about the machine
    // for as long as this page stands, and the cost rows are keyed by it.
    if (!cardNamed) {
      const sys = await ui.api('/api/system');
      const devs = sys && Array.isArray(sys.devices) ? sys.devices : [];
      for (const d of devs) {
        if (d.type === 'gpu' || d.type === 'accel') {
          cardNamed = String(d.name);
          break;
        }
      }
    }
    const prefs = await ui.api('/api/prefs');
    // a reading that did not answer is not a machine that remembers nothing:
    // drawing the switches from an empty answer turns every one of them off and
    // forgets the model this machine remembered, and the next write of the
    // page's own switches would store that guess over the machine's memory.
    if (prefs) applyPrefs(prefs);
    const list = await ui.api('/api/models');
    if (list) {
      models = list.models || [];
      renderModels();
      // The cards carry the choice: the marked card is what a run draws with, so
      // nothing else on the page is given a model.
      fillGalleryOptions();
      syncPrompt();
    }
    const kept = await ui.api('/api/refs');
    if (kept) {
      refs = kept.items || [];
      renderRefs();
    }
  }
  // The room is asked for again, so a render that just landed appears at the top
  // of a list that stayed put otherwise; the pages beyond what this client
  // already holds are asked for in batches. A room whose count no longer matches
  // the machine's own is asked for whatever else stands or not: this is what
  // catches a page up after it was away while the machine worked.
  if (standing || heavy || desynced) await refreshGallery();
  if (heavy) paintRunState();
}

// Rebuilding a choice list throws away the browser's current selection, so the
// value a person had picked is put back after the rebuild.
function fillGalleryOptions() {
  const select = E('gallery-model');
  const chosen = ui.selectValue('gallery-model');
  // The counts next to a name are every record this machine keeps: the room is
  // loaded whole, so a model with (0) made nothing on this machine at all, and
  // choosing it shows an empty list rather than a filtered one. While the room is
  // still arriving the counts say so, in the same words the note under the
  // controls uses.
  const counts = {};
  const seen = [];
  for (const item of galleryItems) {
    const id = String(item.model || '');
    if (counts[id] === undefined) {
      counts[id] = 0;
      seen.push(id);
    }
    counts[id] = counts[id] + 1;
  }
  select.textContent = '';
  const all = document.createElement('option');
  all.value = '';
  all.textContent = 'all models (' + galleryItems.length + ')';
  all.title = 'Show what every model made, newest first.';
  select.appendChild(all);
  const named = {};
  for (const model of models) {
    const id = String(model.id);
    named[id] = true;
    addGalleryOption(select, id, model.name, counts[id] || 0,
      'Show only the pictures ' + model.name + ' made.');
  }
  // A model that made pictures but is no longer offered is still given a line,
  // so its renders can be reached rather than left unfilterable.
  for (const id of seen) {
    if (named[id]) continue;
    addGalleryOption(select, id, id, counts[id],
      'Show only the renders this machine names ' + id + '.');
  }
  pageRebuiltChoice = true;
  ui.chooseOption(select, chosen);
  pageRebuiltChoice = false;
}

function addGalleryOption(select, value, name, made, reach) {
  const option = document.createElement('option');
  option.value = value;
  option.textContent = name + ' (' + made + ')';
  option.title = reach + (galleryItems.length < galleryHeld
    ? ' The count covers the ' + galleryItems.length + ' of the ' + galleryHeld +
    ' this machine keeps that have arrived here so far.'
    : ' The count covers every picture this machine keeps, not a page of them.');
  select.appendChild(option);
}

function galleryFilter(items) {
  const wantModel = ui.selectValue('gallery-model');
  const wantWords = ui.textField('gallery-search').trim().toLowerCase();
  const out = [];
  for (const item of items) {
    if (wantModel !== '' && String(item.model) !== wantModel) continue;
    if (wantWords !== '') {
      const haystack = (String(item.prompt || '') + ' ' + String(item.name || '') +
        ' ' + String(item.model || '')).toLowerCase();
      if (haystack.indexOf(wantWords) < 0) continue;
    }
    out.push(item);
  }
  const sort = ui.selectValue('gallery-sort');
  if (sort === 'old') out.reverse();
  if (sort === 'big') {
    out.sort((a, b) => (Number(b.width) * Number(b.height)) - (Number(a.width) * Number(a.height)));
  }
  return out;
}

function renderUrl(item) {
  return item.url || ('/files/outputs/' + ui.encodePath(item.file));
}

// A record names its own files by bare name, and the door they come through
// takes one path: this turns a name a record carries into the address a browser
// asks at, and leaves an address the machine already stated alone. The name is
// escaped a segment at a time - a name with a space in it is still the name of
// the file it names.
function filesPath(name) {
  const words = String(name || '');
  if (words === '') return '';
  return words.indexOf('/files/') === 0 ? words : ('/files/outputs/' + ui.encodePath(words));
}

// What stands as the picture of a render in an <img>. A picture is that file. A
// clip is not something a browser draws: the machine wrote, beside it, a small
// animated copy that a browser plays as an image and a still first frame, so the
// animated copy plays where there is one and the still frame stands where there
// is not. The clip itself is still what a page opens and what a save hand takes.
function cardPicture(item) {
  if (String(item.kind || '') !== 'video') return renderUrl(item);
  const preview = filesPath(item.preview);
  if (preview !== '') return preview;
  const poster = filesPath(item.poster);
  if (poster !== '') return poster;
  return renderUrl(item);
}

// A clip stated in the units the model moved it at, since that is what decides
// its length: the frames and the rate the machine encoded them at, with the
// seconds those are worth beside them. A clip whose length was not recorded says
// so rather than having a length written for it.
function clipFacts(item) {
  const frames = Number(item.frames || 0);
  const rate = Number(item.fps || 0);
  if (frames < 1) return 'a clip whose length was not recorded';
  const seconds = (rate > 0 ? frames / rate
    : (Number(item.duration_ms) > 0 ? Number(item.duration_ms) / 1000 : 0));
  return frames + ' frames' + (rate > 0 ? ' at ' + rate + ' fps' : '') +
    (seconds > 0 ? ' · ' + (Math.round(seconds * 10) / 10) + ' s' : '');
}

function shotCaption(item) {
  return (item.name || item.model) + ' · ' + item.width + 'x' + item.height +
    (String(item.kind || '') === 'video' ? ' clip · ' + clipFacts(item) : '');
}

// The kept picture a page has an id for, when this machine still says it holds
// it: the words about a picture come from the record, and a record that is no
// longer there leaves nothing to say.
function heldRef(id) {
  for (const ref of refs) {
    if (String(ref.id) === String(id)) return ref;
  }
  return null;
}

// Which render of a repeated ask a card shows: the ask is the key, and the
// value is the chosen render together with the head the card wore when it was
// chosen. The cards are drawn again on a poll and on a filter, so the choice is
// kept here rather than in the node: a picture a person picked with their own
// click does not jump back to the newest one because the machine answered a
// request in the meantime. But a new run of the same ask puts a new head on the
// card, and that picture is the one to show: a choice made about an older ask
// does not get to stare down every render that comes after it - kept past its
// head it would sit on whatever the newest pushed into its place, and slide
// further down the row with every landing. A head the choice was not made
// under, or a chosen render no longer there, shows the newest.
const galleryShown = {};

function chosenOf(group) {
  const key = stackKeyFor(group[0]);
  const held = galleryShown[key];
  if (!held) return 0;
  if (String(held.head) !== String(group[0].id)) {
    delete galleryShown[key];
    return 0;
  }
  for (let at = 0; at < group.length; ++at) {
    if (String(group[at].id) === String(held.pick)) return at;
  }
  return 0;
}

// One card of the gallery. A repeated ask is one card: every render made from the
// same prompt and settings is a picture this card can show, the newest one on top
// by default, and the row of thumbnails under the picture is what picks another.
// The pictures of that ask that are still standing at the machine are added to
// this same card as slots - the ask is the tile, so a further run of it is more
// of the tile it already has, and the card is at the front while any of them
// stands. The picture, the words about it, and its hands are all drawn from the
// render that is shown, so a hand is never left working on a different picture
// than the one on the screen: `save` saves the picture being looked at, `delete`
// deletes that one, `again` puts that one's ask back in the field.
function galleryCard(item, earlier, standing) {
  const group = earlier && earlier.length ? [item].concat(earlier) : [item];
  const shot = document.createElement('div');
  shot.className = 'shot';

  const img = document.createElement('img');
  const said = document.createElement('div');
  said.className = 'shot-prompt';
  const meta = document.createElement('div');
  meta.className = 'shot-meta';
  const act = document.createElement('div');
  act.className = 'shot-act';
  shot.appendChild(img);
  shot.appendChild(said);
  shot.appendChild(meta);
  shot.appendChild(act);

  if (group.length > 1) {
    const row = document.createElement('div');
    row.className = 'shot-stack';
    shot.appendChild(row);
    const words = document.createElement('span');
    words.className = 'stack-count';
    words.textContent = 'the ' + group.length + ' runs of this ask';
    words.title = 'The renders this same prompt and these same settings made, newest first. Press one to show it above; the one shown is ringed.';
    row.appendChild(words);
    // The row holds every run of the ask rather than only the ones left behind the
    // top card, so the picture on screen is in the row too and is ringed: which
    // one is being looked at is then read off the card, not guessed.
    group.forEach(function (past, at) {
      const thumb = document.createElement('img');
      thumb.setAttribute('width', '52');
      thumb.setAttribute('height', '52');
      thumb.setAttribute('alt', 'run ' + (at + 1) + ' of ' + group.length + ' of this prompt and these settings');
      thumb.title = 'run ' + (at + 1) + ' of ' + group.length + ' · ' + ui.whenText(past.created) +
        ' · seed ' + (Number(past.seed) >= 0 ? past.seed : 'chance') + ' · press to show it';
      thumb.src = ui.fileSrc(cardPicture(past));
      ui.onPress(thumb, function () { show(at); });
      row.appendChild(thumb);
    });
  }

  // The picture on the card, the words about it, and its hands: all of it from
  // one render, drawn again when a thumbnail of this ask is pressed.
  function show(at) {
    const one = group[at] || group[0];
    const url = renderUrl(one);
    const picture = cardPicture(one);
    const moving = String(one.kind || '') === 'video';
    galleryShown[stackKeyFor(group[0])] = {
      pick: String(one.id), head: String(group[0].id),
    };
    img.setAttribute('width', String(one.width));
    img.setAttribute('height', String(one.height));
    img.setAttribute('alt', (one.name || one.model) + ' ' + one.width + 'x' + one.height +
      (moving ? ' clip' : ''));
    img.title = shotCaption(one) + (moving ? ' · this clip plays here, at the small copy the machine wrote' : '') +
      ' · tap to open';
    // The kept copy of this file answers without a request: a render is read
    // once, and every later drawing of it — this card, the viewer, the row of
    // thumbnails — uses what is already in this browser.
    const next = ui.fileSrc(picture);
    if (img.getAttribute('src') !== next) {
      img.src = next;
      // The swap is said with a short settle, so a picture that changed under a
      // press reads as changed rather than as a card that redrew itself for
      // nothing.
      img.classList.remove('swap');
      void img.offsetWidth;
      img.classList.add('swap');
    }
    ui.onPress(img, function () { openRender(one); });

    const words = String(one.prompt || '');
    if (words === '') {
      said.textContent = 'no prompt recorded';
      said.removeAttribute('title');
    } else {
      // The whole prompt lives in the viewer; on a card the line is kept short,
      // and the ellipsis says so rather than pretending the prompt ended there.
      said.textContent = ui.clip(words, 200);
      said.title = words;
    }
    meta.textContent = ui.whenText(one.created) + ' · ' + (one.name || one.model) +
      ' · ' + one.width + 'x' + one.height + ' · steps ' + (one.steps || '?') +
      ' · seed ' + (Number(one.seed) >= 0 ? one.seed : 'chance') +
      ' · ' + modelNamed(one.model) +
      (moving ? ' · ' + clipFacts(one) : '');
    shot.classList.toggle('sel', markedShots.has(String(one.id)));
    drawHands(one);
    // Which picture of this ask is the one on screen: the ring sits on the thumb
    // that was pressed, and on that one alone.
    const row = shot.querySelector('.shot-stack');
    if (row) {
      const thumbs = row.querySelectorAll('img');
      for (let at2 = 0; at2 < thumbs.length; ++at2) {
        thumbs[at2].classList.toggle('on', at2 === at);
      }
    }
  }

  // The hands of one render, drawn again with the picture: hands that stayed put
  // under a swapped picture would have `keep` keep a render nobody is looking at.
  function drawHands(one) {
    const url = renderUrl(one);
    act.textContent = '';

    const open = document.createElement('button');
    open.textContent = 'open';
    open.title = 'Show this render in the viewer: its prompt and settings, zoom, and the pictures beside it in this list.';
    ui.onPress(open, function () { openRender(one); });
    act.appendChild(open);

    const save = document.createElement('a');
    // an anchor asks the browser for the address itself, so no request shim
    // stands in front of it: the door wrote the credential on the
    // connection as a cookie, and this browser carries it on the load.
    save.setAttribute('href', url);
    save.setAttribute('download', one.file);
    save.className = 'shot-save';
    save.textContent = 'save';
    save.title = 'Download the file to your computer.';
    act.appendChild(save);

    // A clip in the shapes that ask nothing of a player's codecs: the plain-frame
    // clip, which even the plainest player reads, and the animated picture, which
    // every browser and image viewer moves - a thinned copy, but no player has to
    // know a video codec for it. The machine writes these beside the clip
    // when it can, and a record says which came out; what is absent is not offered.
    if (String(one.kind || '') === 'video') {
      const shapes = [
        ['raw', one.raw, 'clip whose frames need no codec'],
        ['preview', one.preview, 'small moving picture every browser draws'],
      ];
      for (let at = 0; at < shapes.length; ++at) {
        const named = shapes[at][1];
        if (!named) continue;
        const shape = document.createElement('a');
        shape.setAttribute('href', ui.fileSrc(filesPath(named)));
        shape.setAttribute('download', named);
        shape.className = 'shot-save';
        shape.textContent = shapes[at][0];
        shape.title = 'Download the ' + shapes[at][2] + ' to your computer.';
        act.appendChild(shape);
      }
    }

    // A clip carries its own sound; a picture does not, so the control is offered
    // only where there is something to hear. The track is the one the machine wrote
    // beside the clip, and it stands in the card as a control a person can move -
    // seek, pause, hear it again - not a sound that only plays once under a press.
    if (String(one.kind || '') === 'video' && one.audio) {
      const wanted = ui.fileSrc(filesPath(one.audio));
      let track = shot.querySelector('audio');
      if (track) {
        const standing = track.querySelector('source');
        if (standing && standing.src !== wanted) {
          standing.src = wanted;
        }
      } else {
        track = document.createElement('audio');
        track.setAttribute('controls', '');
        track.setAttribute('preload', 'none');
        track.title = 'The sound this clip carries, on its own.';
        const standing = document.createElement('source');
        standing.src = wanted;
        standing.type = 'audio/wav';
        track.appendChild(standing);
        shot.appendChild(track);
      }
      const hear = document.createElement('button');
      hear.textContent = 'sound';
      hear.title = 'Play the sound this clip carries, on its own.';
      ui.onPress(hear, function () {
        if (Number(track.currentTime) > 0) {
          track.currentTime = 0;
        }
        void track.play();
      });
      act.appendChild(hear);
    }

    const again = document.createElement('button');
    again.textContent = 'again';
    again.title = 'Put this prompt in the field, put this picture’s model back in use, and draw it again.';
    ui.onPress(again, async function () {
      const row = models.find(function (one2) { return one2.id === one.model; });
      if (!row) {
        log('That picture was drawn with ' + (one.name || one.model) +
          ', which this machine no longer offers.', 'warn');
        return;
      }
      // The picture and the model that made it belong together. Drawing this prompt
      // with whatever else happened to be in use answered under this picture's name
      // in another model's voice - and when that other model was not on the machine,
      // the press ended in "that model is not downloaded" about a model the card
      // said was ready. The model moves first: putting a model in use puts back
      // what this browser kept for it, and writing the words before that move let
      // the kept ask pour over this picture's prompt before the draw ever saw it.
      await putInUse(row);
      E('prompt-field').value = one.prompt || '';
      const length = E('clip-length');
      if (length && Number(one.frames || 0) > 0) {
        length.value = String(one.frames);
      }
      // A render that started from kept pictures is not the same ask without them:
      // the picture that made it belongs to the pictures it was drawn from, so those
      // are put back in use before the draw. A reference the machine stopped keeping
      // is said so rather than drawn over with nothing.
      const wanted = one.references || [];
      if (wanted.length) {
        const listed = await ui.api('/api/refs');
        if (listed) refs = listed.items || [];
        renderRefs();
        startRefs = wanted.slice();
        dropMissingStarts();
        if (startRefs.length) {
          log('Put this render’s reference pictures back in use: ' + startRefs.length +
            (startRefs.length === 1 ? ' picture' : ' pictures'), 'ok');
        } else {
          log('This render asked for reference pictures that are no longer kept here, so the draw starts from nothing.', 'warn');
        }
        paintRunState();
      }
      log('Queued again: ' + (one.name || one.model) + ' · prompt and model put in use', 'ok');
      generate();
    });
    act.appendChild(again);

    const keep = document.createElement('button');
    keep.setAttribute('id', 'keep-' + one.id);
    keep.textContent = 'keep';
    keep.title = 'Keep this picture so later renders can start from it.';
    ui.onPress(keep, function () {
      void (async function () {
        const kept = await ui.apiR('/api/refs', 'POST', { source: one.id });
        if (!kept.answer) {
          log('Could not keep this render: ' + (kept.error || ui.askWhy(kept)), 'err');
          return;
        }
        log('Kept as a reference: ' + (one.name || one.model), 'ok');
        // The keep answer already lists what is kept, so nothing is requested a
        // second time to draw the same thing.
        refs = (kept.answer.items) || [];
        renderRefs();
        paintRunState();
      })().catch(function (failed) {
        log('the keep did not finish: ' + (failed && failed.message ? String(failed.message) : String(failed)), 'err');
      });
    });
    act.appendChild(keep);

    const remove = document.createElement('button');
    remove.textContent = 'delete';
    remove.title = 'Delete this render and its file. This cannot be undone.';
    ui.onPress(remove, function () {
      void (async function () {
        const gone = await deleteRenders([one]);
        if (gone < 1) return;
        log('Deleted ' + (one.name || one.model) + ' ' + one.width + 'x' + one.height +
          ' and its file.', 'warn');
      })().catch(function (failed) {
        log('the delete did not finish: ' + (failed && failed.message ? String(failed.message) : String(failed)), 'err');
      });
    });
    act.appendChild(remove);
  }

  show(chosenOf(group));
  // The pictures of this ask that are still standing at the machine: they are
  // slots on this card, not cards of their own. The card carries no frame of its
  // own for them - the picture on the card is the render that landed - so every
  // slot here is kept to the width its row needs, each with its bar and its own
  // stop hand, and the line under them states the ask.
  if (standing && standing.length) {
    const card = { shot: shot, said: null, meta: null, queue: null, slots: new Map() };
    const row = document.createElement('div');
    // The row of the rest of the ask borrows the row of an ask's runs: the same
    // line of small tiles, and the same room to wrap them in.
    row.className = 'shot-stack pending-slots';
    const rows = [];
    for (const one of standing) {
      row.appendChild(pendingSlot(one, card, false));
      rows.push(one.pending);
    }
    shot.appendChild(row);
    const queue = document.createElement('div');
    queue.className = 'shot-queue';
    shot.appendChild(queue);
    card.queue = queue;
    // The card of an ask that has work on it is marked as standing, so the sheet
    // lets its slots sway while none of them has been measured.
    shot.classList.add('pending-shot');
    pendingEls.set(stackKeyFor(item), card);
    fillCard(card, rows);
  }
  return shot;
}

function indexOfShown(id) {
  for (let at = 0; at < shownItems.length; ++at) {
    if (String(shownItems[at].id) === id) return at;
  }
  return -1;
}

// A run that repeats itself - the same prompt asked with the same settings - is
// shown as one card rather than as copies side by side. The key is what makes two
// renders the same ask: the model, the prompt, the shape asked for, the steps and
// guidance it was asked with, and the words it was told to leave out. **The seed
// is deliberately not part of it**: the pictures a machine makes of one prompt
// with its seed moved are the same ask explored at different chances, and they
// belong on one card - a suite that draws one prompt at fifteen seeds is one ask,
// not fifteen cards, and the row under the picture names the seed of each run.
// A render whose seed was left to chance carries no seed of its own, and lands in
// the same card, exactly as it did before the seed stopped mattering here.
function stackKeyFor(item) {
  return [String(item.model), String(item.prompt || ''), String(item.width) + 'x' + String(item.height),
  // The length of a clip is part of the ask the same way its size is: two runs
  // of one prompt at different clip lengths are two different amounts of work,
  // not one ask explored at another chance, so they do not belong on one card.
  String(item.steps || ''), String(item.guidance || ''), String(item.negative_prompt || ''),
  String(item.frames || ''), String(item.fps || '')].join('\u0001');
}

// The runs are grouped in the order the screen already shows them, newest first,
// so the head of a group is the newest render of that ask and the rest are what
// sits behind it. A picture still standing is in that company too: it belongs to
// the ask its own words name, and it stands as a slot on that ask's card - and
// when the room holds no card of that ask yet, the group is that ask's own new
// card, holding every slot of it.
function groupStacks(ordered) {
  const groups = new Map();
  for (const item of ordered) {
    const key = cardKeyFor(item);
    const group = groups.get(key);
    if (group) {
      group.push(item);
      continue;
    }
    groups.set(key, [item]);
  }
  return groups;
}

// The key a card stands under: the ask. Landed runs of one ask share it, and so
// do the pictures of that ask that are still standing - the ask is the tile, so
// a run of it that has not come out yet is one more slot on that tile, not a
// tile of its own beside it.
function cardKeyFor(item) {
  return stackKeyFor(item);
}

// Redrawing the list is what a filter change does: the controls say what to show,
// and the screen answers the moment they are moved rather than at the next poll.
// What stands in the line stands here too: a landed run joins the one card its
// ask names, and a picture of that ask that has not come out yet is one more
// slot on that same card - its bar, its state, and its stop hand belong to the
// slot, and the card moves to the front while any of its pictures stands.
//
// The room is drawn again rather than thrown away: each card stands from
// one drawing to the next until something its own row holds moves. A drawing
// moves standing cards into the new order, builds the cards that are new here,
// and takes away the ones that are gone; a moved card keeps the node it was
// drawn in, so a picture already painted stays painted while it shifts. A room
// rebuilt whole would take every picture down and read it again - and while a
// run is drawing, its every step would blink the room.
function renderGallery(items) {
  const pane = E('gallery');
  // The counts beside the names in the model filter are the pictures this page
  // holds, so they are set from the same list the cards are drawn from. Filling
  // them only when the models arrived left every name at (0) on a page whose
  // gallery landed afterwards.
  fillGalleryOptions();
  const real = items || [];
  let list = real;
  if (pendingRows.length) {
    const heads = [];
    for (const row of pendingRows) heads.push(pendingItem(row));
    list = heads.concat(real);
  }
  const kept = galleryFilter(list);
  shownItems = kept;
  // A picture open in the viewer keeps its place in the list, so prev and next
  // still name the one it came from after the filter moved.
  viewerAt = indexOfShown(openShot);
  const groups = groupStacks(kept);
  // The grouping the viewer reads is of landed renders only: the row under a
  // picture names the renders its ask made, and a slot is not a render yet.
  const landed = new Map();
  for (const entry of groups.values()) {
    const runs = [];
    for (const item of entry) {
      if (!item.pending) runs.push(item);
    }
    if (runs.length) landed.set(cardKeyFor(runs[0]), runs);
  }
  galleryGroups = landed;
  // A card is its ask: the newest render of it is the picture on the card, and
  // the pictures of that ask that are still standing are its slots. A group with
  // no render in it yet has no picture to show, and its first slot is the head.
  const cards = [];
  for (const entry of groups.values()) {
    let head = null;
    const runs = [];
    const standing = [];
    for (const item of entry) {
      if (item.pending) {
        standing.push(item);
        continue;
      }
      runs.push(item);
      if (!head) head = item;
    }
    if (!head) head = standing[0];
    cards.push({ head: head, runs: runs, standing: standing });
  }
  const keys = [];
  const stamps = [];
  for (const entry of groups.values()) {
    keys.push(cardKeyFor(entry[0]));
    stamps.push(cardStamp(entry));
  }
  const signature = keys.join('\u0002') + '\u0003' + stamps.join('\u0002');
  // The note counts what landed and what is marked, and those move with the
  // machine's readings while no card changes: its words are drawn every time.
  // Only landed runs are counted as shares of a card - a slot is a picture still
  // to come, not an earlier run kept behind the newest one.
  drawGalleryNote(kept.filter(function (item) { return !item.pending; }),
    real, landed);
  if (signature === galleryCardState && pane.childElementCount) return;
  galleryCardState = signature;
  const built = new Map();
  let at = 0;
  for (const card of cards) {
    // One card per ask: the head of it and nothing else. The landed runs of one
    // ask share the card their ask names, with their row of thumbnails; the
    // pictures of that ask that are still standing are added to that same card
    // as slots, and an ask with no render here yet opens its own card, whose
    // first slot is the frame the first picture will come back at.
    const head = card.head;
    const key = keys[at];
    const held = galleryCards.get(key);
    let node = held && held.stamp === stamps[at] && held.node.parentNode === pane ?
      held.node : null;
    if (!node) {
      node = head.pending
        ? pendingCard(head, card.standing.slice(1))
        : galleryCard(head, card.runs.slice(1), card.standing);
    }
    built.set(key, { node: node, stamp: stamps[at] });
    ++at;
  }
  const keeps = new Set();
  for (const held of built.values()) keeps.add(held.node);
  for (const child of Array.from(pane.children)) {
    if (!keeps.has(child)) pane.removeChild(child);
  }
  // Moving a standing node into its place keeps everything it holds; only a
  // place that was empty takes a new card.
  let cursor = 0;
  for (const held of built.values()) {
    if (pane.children[cursor] === held.node) {
      ++cursor;
      continue;
    }
    pane.insertBefore(held.node, pane.children[cursor] || null);
    ++cursor;
  }
  galleryCards = built;
}

// What one card's node holds: the runs stacked on it and the marks they wear.
// A standing row is told by the shape its card is drawn at - its bar, its
// words, and its state move in place, and stand outside this stamp. Which
// pictures stand is not: their own keys are part of the stamp, because the
// slots of a standing card are one per key, and a card kept from an earlier
// drawing holds slots of the jobs that stood when it was drawn. Stop one
// picture of an ask and ask the same shape again and nothing of the shape
// moved - the pictures are different pictures, and it is their identity that
// says the card must be drawn again.
function cardStamp(group) {
  const parts = [];
  for (const item of group) {
    if (item.pending) {
      parts.push('p' + String(item.width) + 'x' + String(item.height) +
        '·' + String(item.kind || '') +
        '·' + ((item.pending.refThumbs || []).join(',')) +
        '·#' + String(item.pending.key));
    } else {
      parts.push(String(item.id) + (markedShots.has(String(item.id)) ? '*' : ''));
    }
  }
  return parts.join('\u0001');
}

// The whole room is drawn, so the note normally says the machine's own count. It
// only speaks of a part while the room's later pages are still on their way - the
// batches land one round at a time - and a search that finds nothing says which
// part is unsearched rather than promising that nothing matches. The marked count
// is said here too, because the bulk buttons act on those pictures alone.
function drawGalleryNote(kept, loaded, groups) {
  const held = Math.max(galleryHeld, loaded.length);
  const beyond = held > loaded.length;
  let words;
  // The window on screen is named by where it stands in the room the machine
  // states, so a search that found nothing here says what is still unsearched
  // rather than promising that nothing matches.
  const window = beyond
    ? 'the pictures ' + (galleryOffset + 1) + ' to ' + (galleryOffset + loaded.length) +
    ' of the ' + held + ' this machine keeps, newest first'
    : '';
  if (held === 0) {
    words = 'Nothing is kept here yet: a render appears the first time this machine makes one';
  } else if (kept.length === 0) {
    words = beyond
      ? 'No matches in ' + window + ' — the rest of the room is still on its way here'
      : 'No matches — clear the model or search filter to see what else this machine holds';
  } else {
    words = beyond
      ? 'Showing ' + kept.length + ' of ' + window + ' · the rest of the room is still on its way here'
      : 'Showing ' + kept.length + ' of ' + held + ' this machine holds';
    // The counts name the pictures, while a repeated ask shares one landed card:
    // the words say where the rest of a repeated ask sits, so a short list of
    // cards is never read as a machine that made little.
    let hidden = 0;
    for (const entry of groups.values()) {
      hidden = hidden + entry.length - 1;
    }
    if (hidden > 0) {
      words = words + ' · ' + hidden + (hidden === 1
        ? ' of these is an earlier run kept behind the newest render of the same prompt and settings, on the same card'
        : ' of these are earlier runs kept behind the newest render of the same prompt and settings, on the same card');
    }
  }
  // What narrowed the list is said with the counts, so a short list is never read
  // as a machine that holds little.
  const wantModel = ui.selectValue('gallery-model');
  if (wantModel !== '') {
    words = words + ' · narrowed to the renders ' + modelNamed(wantModel) + ' made';
  }
  const wantWords = ui.textField('gallery-search').trim();
  if (wantWords !== '') {
    words = words + ' · narrowed to what contains ' + wantWords;
  }
  let marked = 0;
  for (const item of kept) {
    if (markedShots.has(String(item.id))) marked = marked + 1;
  }
  if (marked > 0) {
    words = words + ' · ' + marked + (marked === 1
      ? ' of these is marked, and the bulk buttons act on the marked ones on this screen'
      : ' of these are marked, and the bulk buttons act on the marked ones on this screen');
  }
  E('gallery-note').textContent = words;
}

// Deleting takes the record and its file off the machine, so the list, the counts
// beside the model names, and the note all answer from what is left. A delete
// that was refused is named there rather than counted as gone.
async function deleteRenders(targets) {
  let gone = 0;
  const cut = new Set();
  for (const item of targets) {
    const answered = await ui.apiR('/api/gallery/' + encodeURIComponent(String(item.id)), 'DELETE');
    if (!answered.answer) {
      log('Could not delete ' + (item.name || item.model) + ': ' +
        (answered.error || ui.askWhy(answered)), 'err');
      continue;
    }
    cut.add(String(item.id));
    gone = gone + 1;
  }
  if (gone === 0) return 0;
  const rest = [];
  for (const item of galleryItems) {
    if (!cut.has(String(item.id))) rest.push(item);
  }
  galleryItems = rest;
  for (const id of cut) {
    markedShots.delete(id);
    deletedShots.add(id);
  }
  galleryHeld = Math.max(0, galleryHeld - gone);
  renderGallery(galleryItems);
  fillGalleryOptions();
  return gone;
}

async function loadRefs() {
  const kept = await ui.api('/api/refs');
  refs = (kept && kept.items) || [];
}

function refCard(ref) {
  const shot = document.createElement('div');
  shot.className = 'ref-shot';
  shot.dataset.ref = ref.id;
  // A card a person can pick up and drop onto the prompt panel.
  shot.setAttribute('draggable', 'true');
  shot.addEventListener('dragstart', function () {
    draggedRef = String(ref.id);
  });
  const url = ref.url || ('/files/refs/' + ui.encodePath(ref.file));
  const img = document.createElement('img');
  img.setAttribute('alt', ref.name);
  img.title = ref.name + ' · tap to open';
  img.src = ui.fileSrc(url);
  ui.onPress(img, function () { showViewer(url, 'kept picture · ' + ref.name, ref); });
  shot.appendChild(img);
  const from = document.createElement('div');
  from.className = 'ref-from';
  from.textContent = String(ref.source) === 'kept in the browser'
    ? 'imported'
    : 'from a render';
  from.title = 'Where this kept picture came from.';
  shot.appendChild(from);

  const use = document.createElement('button');
  use.textContent = 'use';
  use.title = 'Add this picture to the strip above the prompt, at the next position. A run starts from the strip in the order the model reads it.';
  ui.onPress(use, function () {
    addToStrip(String(ref.id));
  });
  shot.appendChild(use);

  const drop = document.createElement('button');
  drop.textContent = 'remove';
  drop.title = 'Stop keeping this picture: the file is deleted on the machine, and it is taken off the strip if it was in it.';
  ui.onPress(drop, function () {
    void (async function () {
      const gone = await ui.apiR('/api/refs/' + encodeURIComponent(String(ref.id)), 'DELETE');
      if (!gone.answer) {
        log('Could not remove this kept picture: ' + (gone.error || ui.askWhy(gone)), 'err');
        return;
      }
      log('Removed the kept picture: ' + ref.name, 'warn');
      await loadRefs();
      takeOffStrip(String(ref.id));
      renderRefs();
      paintRunState();
    })().catch(function (failed) {
      log('the remove did not finish: ' + (failed && failed.message ? String(failed.message) : String(failed)), 'err');
    });
  });
  shot.appendChild(drop);
  return shot;
}

function renderRefs() {
  const pane = E('refs');
  pane.textContent = '';
  for (const ref of refs) pane.appendChild(refCard(ref));
  E('refs-note').textContent = refs.length === 0
    ? 'Nothing kept yet: press keep under a render, or drop your own picture on the prompt panel'
    : 'Keeping ' + refs.length + ' picture' + (refs.length === 1 ? '' : 's') +
    ' · tap a card to add it to the strip, or drag it onto the prompt panel';
}

// The drop target: a picture the browser holds (a file dragged onto the page,
// a screenshot pasted from the clipboard) has no path on this machine, so it is
// kept as its own bytes. A kept picture is only ever a starting point for a
// render, and a render resamples its start to its own size, so the picture is
// shrunk to that and written as a jpg — a format every picture can be written
// as and one the studio reads back.
// A file this browser was given: its bytes are read here into one image the
// machine can store. FileReader answers through events, so the read is awaited,
// not assumed; a file that cannot be read says so.
async function readPictureBytes(file) {
  if (!file) return '';
  const reader = new FileReader();
  try {
    const text = await new Promise(function (done) {
      reader.onload = function () { done(String(reader.result || '')); };
      reader.onerror = function () { done(''); };
      reader.onabort = function () { done(''); };
      reader.readAsDataURL(file);
    });
    return String(text || '');
  } catch (fileNotReadable) {
    return '';
  }
}

async function encodePicture(dataUrl, side) {
  const img = new Image();
  img.src = dataUrl;
  try {
    await img.decode();
  } catch (willNotDecode) {
    return '';
  }
  const wide0 = Number(img.naturalWidth);
  const tall0 = Number(img.naturalHeight);
  if (!(wide0 > 0) || !(tall0 > 0)) return '';
  const across = wide0 > tall0 ? wide0 : tall0;
  // one factor for both sides: the picture's own shape is kept, so a thin
  // picture stays thin - pulling each side to a floor of its own would stretch
  // it. a copy is only ever grown when its longest side is under the floor the
  // page keeps copies at, by that same single factor.
  const kSmallestCopySide = 64;
  const scale = across > side ? side / across : (across < kSmallestCopySide ? kSmallestCopySide / across : 1);
  const wide = Math.max(1, Math.round(wide0 * scale));
  const tall = Math.max(1, Math.round(tall0 * scale));
  const canvas = document.createElement('canvas');
  canvas.width = wide;
  canvas.height = tall;
  const draw = canvas.getContext('2d');
  if (!draw) return '';
  draw.fillStyle = '#ffffff';
  draw.fillRect(0, 0, wide, tall);
  draw.drawImage(img, 0, 0, wide, tall);
  return String(canvas.toDataURL('image/jpeg', 0.85));
}

async function keepBrowserPicture(name, dataUrl) {
  // One request body is what the server reads, so copies are tried largest to
  // smallest; one that fits none is named as too large rather than half kept.
  // a copy must leave room for the words that travel around the picture in that
  // same body - the field names, the format, and a name a person typed - so the
  // room is the door's own limit with that margin taken off, and the door, not a
  // guess, decides the last bytes.
  const kWordsAroundAPicture = 4096;
  const bodyRoom = ui.bodyBytes - kWordsAroundAPicture;
  const pictureSides = [1280, 960, 704];
  let encoded = '';
  let fits = false;
  for (const side of pictureSides) {
    encoded = await encodePicture(dataUrl, side);
    if (encoded === '') break;
    if (encoded.length < bodyRoom) {
      fits = true;
      break;
    }
  }
  if (!fits) {
    const why = encoded === ''
      ? 'that file is not an image this browser can decode'
      : 'this picture is too large to keep: even the smallest copy does not fit';
    notePicture('Not kept: ' + why);
    log('Not kept: ' + why, 'err');
    return;
  }
  const before = {};
  for (const kept of refs) before[String(kept.id)] = true;
  const kept = await ui.apiR('/api/refs', 'POST', {
    name: name,
    image_b64: encoded.slice(encoded.indexOf('base64,') + 7),
    format: 'jpg',
  });
  if (!kept.answer) {
    const why = 'Could not keep the imported picture: ' + (kept.error || ui.askWhy(kept));
    notePicture(why);
    log(why, 'err');
    return;
  }
  refs = kept.answer.items || [];
  for (const item of refs) {
    if (before[String(item.id)]) continue;
    // The bytes this page holds go into the browser's store under the name the
    // machine gave them, so drawing this kept picture later never asks the
    // network again.
    ui.fileKeep(item.url, encoded, 'image/jpeg');
    ui.flash(E('prompt-panel'));
    notePicture('Kept from the browser: ' + (item.name || name));
    log('Kept the imported picture: ' + (item.name || name), 'ok');
    addToStrip(String(item.id));
  }
  renderRefs();
  paintRunState();
}

// A picture that came as a drag item or a clipboard item: the file behind it is
// read in this browser and kept on the machine; no path is invented. The read's
// answer is what gets kept, so an empty file says so rather than becoming
// nothing.
function keepPictureFile(file, source) {
  if (!file) return false;
  let name = String(file.name || '');
  const dot = name.lastIndexOf('.');
  if (dot > 0) name = name.slice(0, dot);
  if (name === '') name = source === 'clipboard' ? 'pasted picture' : 'dropped picture';
  ui.flash(E('prompt-panel'));
  notePicture('Reading ' + name + '…');
  log(source === 'clipboard' ? 'Imported from the clipboard: ' + name
    : 'Imported from the page: ' + name, 'ok');
  void (async function () {
    const data = await readPictureBytes(file);
    if (data === '') {
      notePicture('Not kept: the file arrived empty.');
      log('The file arrived empty.', 'err');
      return;
    }
    await keepBrowserPicture(name, data);
  })().catch(function (failed) {
    log('the kept picture did not arrive: ' + (failed && failed.message ? String(failed.message) : String(failed)), 'err');
  });
  return true;
}

function acceptPictureItem(item, source) {
  if (!item || item.kind !== 'file') return false;
  return keepPictureFile(item.getAsFile(), source);
}

// The prompt panel is the drop target, and it answers a drag the way the
// platform asks: dragenter and dragover say the drop will be taken and cancel
// it, so the browser does nothing of its own with the file; the file is taken
// wherever an engine hands one over — some name it while the drag is over the
// panel, others only at the drop — and whichever answered is taken exactly
// once. A drag that names a format is text or an element of this page and is
// left alone. The item list is live, not an array, so it is walked by index.
// A picture released elsewhere on the page belongs to the browser and is left
// alone. A pasted picture has no position, so it lands where the paste happens.
// Every picture that came is kept; the ones within the model's own limit go
// into the strip in the order they came.
function holdingZone(event) {
  const zone = E('prompt-panel');
  if (!zone) return null;
  const target = event.target;
  // The node under the pointer is inside the panel, or the pointer landed on an
  // ancestor of it, which is what some browsers report instead.
  if (target && (zone === target || zone.contains(target) || target.contains(zone))) return zone;
  // A position over the panel is the same target, told without a node.
  const x = event.clientX;
  const y = event.clientY;
  if (typeof x !== 'number' || typeof y !== 'number') return null;
  const box = zone.getBoundingClientRect();
  if (x >= box.left && x <= box.right && y >= box.top && y <= box.bottom) return zone;
  return null;
}

async function takeChosenFiles(files) {
  if (!files || files.length === 0) return;
  const panel = E('prompt-panel');
  if (panel) ui.flash(panel);
  for (let at = 0; at < files.length; ++at) {
    const file = files[at];
    let name = String(file.name || '');
    const dot = name.lastIndexOf('.');
    if (dot > 0) name = name.slice(0, dot);
    if (name === '') name = 'chosen picture ' + (at + 1);
    notePicture('Reading ' + name + '…');
    log('Chosen picture: ' + name, 'ok');
    const data = await readPictureBytes(file);
    if (data === '') {
      notePicture('Not kept: the file arrived empty.');
      log('The file arrived empty.', 'err');
      continue;
    }
    await keepBrowserPicture(name, data);
  }
}

function attachPictureTargets() {
  // State for one drag, reset by the first event an engine sends: an engine may
  // send no dragstart at all for a native file, so a second picture would
  // otherwise inherit the first one's state.
  let overPanel = false;
  let ownDrag = false;
  let heldFile = null;
  let dropHeard = false;
  let takenFile = null;
  // The item list of a drag is live, not an array: it is walked by index.
  function fileItem(event) {
    const transfer = event.dataTransfer;
    if (!transfer || !transfer.items) return null;
    for (let at = 0; at < transfer.items.length; ++at) {
      const item = transfer.items[at];
      if (item && item.kind === 'file') return item;
    }
    return null;
  }
  function openDrag() {
    overPanel = false;
    heldFile = null;
    dropHeard = false;
    takenFile = null;
  }
  // The file of the current drag is remembered from the first event that names
  // it, because the drag store closes while the drag moves.
  function nameFile(event) {
    const item = fileItem(event);
    if (item) {
      const file = item.getAsFile();
      if (file) heldFile = file;
      return;
    }
    // Some engines carry no live item list and name the file in the plain one
    // instead; the panel answers either shape, never both at once.
    const transfer = event.dataTransfer;
    if (transfer && transfer.files && transfer.files.length > 0) heldFile = transfer.files[0];
  }
  // The file of the current drag is kept once, however it was reached.
  function takeHeld() {
    if (!heldFile || heldFile === takenFile) return false;
    if (!keepPictureFile(heldFile, 'drop')) return false;
    takenFile = heldFile;
    return true;
  }
  document.addEventListener('dragstart', function (event) {
    openDrag();
    ownDrag = false;
    const transfer = event.dataTransfer;
    if (!transfer) return;
    // A released file needs a destination, so ask for the copy effect.
    transfer.effectAllowed = 'copy';
    nameFile(event);
    // A drag that names a format is text or an element of this page; a file
    // brought in from outside this page names none, and that is what this
    // panel answers.
    if (draggedRef !== '' || (transfer.types && transfer.types.length > 0)) ownDrag = true;
  });
  document.addEventListener('dragenter', function (event) {
    openDrag();
    if (ownDrag || !holdingZone(event)) return;
    // this panel is willing to take the drop, so it cancels the question
    event.preventDefault();
  });
  document.addEventListener('dragover', function (event) {
    const zone = holdingZone(event);
    overPanel = zone !== null;
    if (ownDrag || !zone) return;
    zone.classList.add('offering');
    const transfer = event.dataTransfer;
    if (transfer) transfer.dropEffect = 'copy';
    nameFile(event);
    // A drop left uncancelled would end with the browser doing its own thing
    // with the file — opening it, and leaving the page — so the panel takes it.
    event.preventDefault();
  });
  document.addEventListener('drop', function (event) {
    if (ownDrag) return;
    const zone = holdingZone(event);
    if (!zone) return;
    dropHeard = true;
    nameFile(event);
    takeHeld();
    // Whatever this page took, the browser must not also act on it.
    event.preventDefault();
  });
  document.addEventListener('dragend', function () {
    const panel = E('prompt-panel');
    if (panel) panel.classList.remove('offering');
    if (overPanel && draggedRef !== '') addToStrip(draggedRef);
    // An engine that never sends a drop for this drag still named the file
    // while the drag moved over the panel, so the file is kept from here.
    if (!dropHeard && overPanel) takeHeld();
    draggedRef = '';
    openDrag();
    ownDrag = false;
  });
  document.addEventListener('paste', function (event) {
    const clip = event.clipboardData;
    if (!clip || !clip.items) return;
    // The clipboard list is live as well: it is walked by index.
    for (let at = 0; at < clip.items.length; ++at) {
      if (acceptPictureItem(clip.items[at], 'clipboard')) break;
    }
  });
  // The browser's own file chooser takes pictures from the computer: a person
  // picks files there, and this page reads and keeps those files. The chooser's
  // answer is the files themselves, never a path this machine made up.
  const chooser = E('picture-file');
  if (chooser) {
    chooser.addEventListener('change', function (event) {
      void takeChosenFiles(event.target.files);
    });
  }
  const form = E('picture-form');
  if (form) {
    // The chooser sits in a form so its answer can be heard; nothing is
    // submitted anywhere, so the form is stopped from its default action.
    void form.addEventListener('submit', function (event) {
      event.preventDefault();
    });
  }
}

function startOf(model) {
  return model.edit === true ? 'edit' : (model.redraw === true ? 'redraw' : '');
}

// The steps a picture can be drawn at, in the order the buttons move through
// them. fit fills the frame the picture was asked for; half is half that; real
// is the picture's own pixels, which is what the button that says 100% means;
// double and huge are the fit size times two and four. A picture smaller than
// the frame is grown to fill it here rather than left at its pixels, because
// this room is for reading a picture, and the step that names the pixels is
// right there beside it. The page works each size out from the frame's own box
// and the picture's pixels (see drawPictureAt) rather than leaving a step to a
// class the style sheet draws in, so a step means the same thing whatever the
// picture's shape and no part of the frame is left with nothing of its own.
const zoomSteps = ['half', 'fit', 'real', 'double', 'huge'];
const zoomShare = { half: 0.5, double: 2, huge: 4 };
const kZoomFit = zoomSteps.indexOf('fit');
const kZoomReal = zoomSteps.indexOf('real');
let zoomAt = kZoomFit;
// A step asked for before the picture's bytes arrived: its pixels are not known
// then, and the style sheet's own fit rule already drew the whole picture, so
// this step is drawn again the moment the bytes do rather than guessed at.
let zoomWaits = -1;
// How far the picture has been moved from the middle of the frame, in screen
// pixels: a zoom that cannot move leaves only one corner of a picture reachable.
// The frame clips the picture and never the buttons, so zooming cannot hide the
// way back out of the viewer.
let panX = 0;
let panY = 0;

function paintZoom(at) {
  zoomAt = Math.min(Math.max(at, 0), zoomSteps.length - 1);
  // A new step is a new frame of the picture, so the picture is shown from its
  // middle again rather than left at a place that no longer means anything.
  panX = 0;
  panY = 0;
  drawPictureAt(zoomAt);
}

// The size to draw a picture at, in the frame it was asked for. The frame's box
// and the picture's pixels are both known here, so the size is stated on the
// picture rather than left to how a browser reads a share against a part that
// has no size of its own; the picture is then moved by hand inside the frame.
function drawPictureAt(at) {
  const stage = E('viewer-stage');
  const img = E('viewer-img');
  if (!stage || !img) return;
  const wide = Number(img.naturalWidth);
  const tall = Number(img.naturalHeight);
  if (!(wide > 0) || !(tall > 0)) {
    zoomWaits = at;
    return;
  }
  zoomWaits = -1;
  const roomWide = Math.max(1, Number(stage.clientWidth));
  const roomTall = Math.max(1, Number(stage.clientHeight));
  // The largest whole picture that lands in the frame, both shape kept.
  const fit = Math.min(roomWide / wide, roomTall / tall);
  const step = zoomSteps[at];
  const scale = step === 'real' ? 1 : fit * (zoomShare[step] || 1);
  const drawnWide = Math.max(1, Math.round(wide * scale));
  const drawnTall = Math.max(1, Math.round(tall * scale));
  // The page's own number is what stands: the style sheet's fit limits are
  // cleared with it, so a larger step is never pulled back to the frame.
  img.style.width = drawnWide + 'px';
  img.style.height = drawnTall + 'px';
  img.style.maxWidth = 'none';
  img.style.maxHeight = 'none';
  applyPan();
}

function applyPan() {
  const img = E('viewer-img');
  if (!img) return;
  img.style.transform =
    'translate(-50%, -50%) translate(' + panX + 'px, ' + panY + 'px)';
}

function clampPan() {
  const stage = E('viewer-stage');
  const img = E('viewer-img');
  if (!stage || !img) return;
  const roomX = Math.max(0, (Number(img.offsetWidth) - stage.clientWidth) / 2);
  const roomY = Math.max(0, (Number(img.offsetHeight) - stage.clientHeight) / 2);
  if (panX > roomX) panX = roomX;
  if (panX < -roomX) panX = -roomX;
  if (panY > roomY) panY = roomY;
  if (panY < -roomY) panY = -roomY;
}

// A window made wider or narrower since the picture was drawn leaves that
// picture at a size worked out for the old frame: the step is drawn again for
// the frame this window has now, so the picture never sits at a size that no
// frame was asked for.
function attachViewerResize() {
  window.addEventListener('resize', function () {
    if (openShot === '' || zoomWaits >= 0) return;
    paintZoom(zoomAt);
  });
}

// The step asked for before a picture's bytes arrived is drawn when they do,
// and a picture that never came through says so rather than standing as a
// broken mark: its words and the buttons are still on the frame. The arrival of
// bytes is what a scroll moves on as well: the move to a picture is played when
// its bytes land, and the countdown to the next one begins then, so a picture
// that spends four seconds coming through spends none of the ten it was given.
// A picture that did not come through stops a scroll with its own words rather
// than leaving it counting towards a mark that never lands.
function attachViewerArrival() {
  const img = E('viewer-img');
  if (!img) return;
  img.addEventListener('load', function () {
    if (zoomWaits >= 0) {
      const at = zoomWaits;
      zoomWaits = -1;
      drawPictureAt(at);
    }
    playScrollMove();
    if (scrollOn) armScrollClock();
  });
  // The move belongs to the arrival, not to everything that follows it: once it
  // is made, nothing about the picture is left wearing it, so a later zoom step
  // is drawn from the page's own size and place alone.
  img.addEventListener('animationend', function () {
    img.style.animation = '';
  });
  img.addEventListener('error', function () {
    zoomWaits = -1;
    scrollMove = '';
    stopScroll('That picture did not come through, so the scroll stops here.');
    log('That picture did not come through: the words about it and the buttons stay on the frame.', 'err');
  });
}

// The scroll: the runs of one ask, moved along on a clock this page owns. A
// stacked ask - the renders one prompt and one set of settings made, kept
// together as one card - is what a scroll moves through, and the last of them
// goes back to the first, so a scroll standing on the newest of four runs
// reaches it again after four moves. Each picture stands for the time the first
// dropdown names, the next is moved to with the move the second names, made in
// the speed the third names, and the move is played on the incoming picture as
// its bytes land.
// The countdown belongs to the picture on the frame, so prev, next, and a press
// on a run under the frame all give the picture they opened its whole time, and
// a longer time asked for in the middle of a picture gives that picture its
// whole new time rather than cutting it short.
const kScrollWipes = {
  none: '',
  fade: 'viewer-fade-in',
  slide: 'viewer-slide-in',
  zoom: 'viewer-zoom-in',
  wipe: 'viewer-wipe-in'
};
// The three choices are kept in this browser, so a page opened again scrolls on
// the clock and with the move it was last given.
const kScrollStore = 'ols-scroll';
let scrollOn = false;
let scrollEach = 30;
let scrollWipe = 'fade';
let scrollSpeed = 260;
// The ask being scrolled, kept by the key that makes two renders one ask: the
// list under the frame is drawn again on a poll, and a render deleted while a
// scroll ran leaves its group without a word to the page - so the runs are read
// again from the list on screen at every move, and an ask whose runs are no
// longer here is answered as having no runs.
let scrollStack = '';
let scrollTimer = -1;
// The move asked for and not yet played: it belongs to a picture whose bytes
// have not landed, so it is carried to them rather than played on the picture
// that stands on the frame now.
let scrollMove = '';

function eachWords() {
  if (scrollEach < 60) return String(scrollEach) + (scrollEach === 1 ? ' second' : ' seconds');
  const minutes = Math.round(scrollEach / 60);
  return String(minutes) + (minutes === 1 ? ' minute' : ' minutes');
}

// The word a dropdown holds for the choice it is set to, read off the control
// itself: the page never says one thing beside a field that shows another.
function chosenWord(id) {
  const node = E(id);
  const chosen = node && node.selectedIndex >= 0 ? node.options[node.selectedIndex] : null;
  return chosen ? String(chosen.textContent).trim() : '';
}

// The runs of the ask being scrolled, as the list on this page groups them now:
// an ask that made one render, or one filtered out of the page, has nothing to
// scroll and is answered as having nothing to scroll.
function scrollGroup() {
  if (scrollStack === '') return null;
  const found = galleryGroups.get(scrollStack);
  return found && found.length > 1 ? found : null;
}

// The hand, the line on the frame, and the words over the runs: all three say
// what the scroll is doing now, in the words the dropdowns hold.
function paintScrollState() {
  const hand = E('viewer-play');
  if (hand) {
    hand.classList.toggle('on', scrollOn);
    hand.textContent = scrollOn ? 'pause' : 'play';
    hand.setAttribute('aria-pressed', scrollOn ? 'true' : 'false');
    const group = scrollGroup();
    hand.title = scrollOn
      ? 'Stop scrolling: the picture on the frame stays exactly where it stands.'
      : (group
        ? 'Scroll through the ' + group.length + ' runs of this ask: each stands ' + eachWords() +
        ', the next is moved to with a ' + chosenWord('viewer-wipe') +
        ', and the last of them goes back to the first.'
        : 'This ask made one render, so there is nothing here to scroll: the scroll moves through the runs a stacked ask made.');
  }
  const tick = E('viewer-tick');
  if (tick && !scrollOn) {
    tick.classList.remove('on');
    tick.hidden = true;
  }
  paintRunsWords();
}

// The countdown for the picture standing on the frame: the line along its bottom
// fills over the time it is kept, so what is left is read off the line rather
// than counted, and the move to the next is asked for at the same moment.
function armScrollClock() {
  if (scrollTimer >= 0) {
    clearTimeout(scrollTimer);
    scrollTimer = -1;
  }
  const tick = E('viewer-tick');
  if (!scrollOn || !tick) return;
  tick.hidden = false;
  tick.style.animationDuration = String(scrollEach * 1000) + 'ms';
  tick.classList.remove('on');
  tick.offsetWidth;
  tick.classList.add('on');
  scrollTimer = setTimeout(scrollAlong, scrollEach * 1000);
}

// One move along the runs of this ask: the next one, and the first of them after
// the last.
function scrollAlong() {
  scrollTimer = -1;
  if (!scrollOn) return;
  const group = scrollGroup();
  if (!group) {
    stopScroll('The runs of that ask are no longer on this page, so the scroll stops here.');
    return;
  }
  let at = -1;
  for (let i = 0; i < group.length; ++i) {
    if (String(group[i].id) === openShot) at = i;
  }
  openRender(group[at < 0 ? 0 : (at + 1) % group.length]);
}

function startScroll() {
  const group = scrollGroup();
  if (!group) {
    log('This ask made one render, so there is nothing to scroll: the scroll moves through the runs a stacked ask made.', 'warn');
    return;
  }
  scrollOn = true;
  paintScrollState();
  armScrollClock();
  log('Scrolling the ' + group.length + ' runs of this ask: each stands ' + eachWords() +
    ', the next is moved to with a ' + chosenWord('viewer-wipe') +
    ' over ' + chosenWord('viewer-speed') + '. The last of them goes back to the first.', 'ok');
}

function stopScroll(words) {
  const ran = scrollOn;
  scrollOn = false;
  if (scrollTimer >= 0) {
    clearTimeout(scrollTimer);
    scrollTimer = -1;
  }
  paintScrollState();
  if (ran && words) log(words, 'warn');
}

// The three dropdowns as they stand: written onto this page's own state and kept
// in this browser. A running scroll re-arms the picture it stands on.
function readScrollChoices() {
  const each = Number(ui.selectValue('viewer-each'));
  const wipe = String(ui.selectValue('viewer-wipe'));
  const speed = Number(ui.selectValue('viewer-speed'));
  if (each > 0) scrollEach = Math.round(each);
  if (kScrollWipes[wipe] !== undefined) scrollWipe = wipe;
  if (speed > 0) scrollSpeed = Math.round(speed);
  try {
    localStorage.setItem(kScrollStore,
      JSON.stringify({ each: scrollEach, wipe: scrollWipe, speed: scrollSpeed }));
  } catch (ignored) { }
  paintScrollState();
  if (scrollOn) armScrollClock();
}

// The copy this browser kept, put back on the dropdowns before any picture can
// be opened: a value the page no longer offers falls back to the option it shows
// for itself, so a field and the screen cannot hold two opinions.
function restoreScrollChoices() {
  let kept = null;
  try {
    kept = JSON.parse(localStorage.getItem(kScrollStore) || 'null');
  } catch (ignored) { }
  if (kept && kept.each) ui.chooseOption(E('viewer-each'), String(kept.each));
  if (kept && kept.wipe) ui.chooseOption(E('viewer-wipe'), String(kept.wipe));
  if (kept && kept.speed) ui.chooseOption(E('viewer-speed'), String(kept.speed));
  readScrollChoices();
}

// The move, played on the picture as it lands, for the speed the dropdown names.
// No move in the style sheet touches the picture's own transform - the page
// writes the zoom step and the place a held picture was moved into there - so a
// scroll moves a picture along exactly as it stands, at whatever step it was
// zoomed to and moved to.
function playScrollMove() {
  const move = scrollMove;
  scrollMove = '';
  const img = E('viewer-img');
  if (!img || move === '') return;
  img.style.animation = 'none';
  img.offsetWidth;
  img.style.animation = move + ' ' + String(scrollSpeed) + 'ms ease-out both';
}

// One line of what the machine recorded about a picture. The words the model was
// given and the settings that made the render belong with the render: a viewer
// that hides them leaves no way to repeat or mend what is on screen.
function addFact(box, label, value) {
  const line = document.createElement('div');
  line.className = 'fact';
  const key = document.createElement('span');
  key.className = 'fact-key';
  key.textContent = label;
  line.appendChild(key);
  line.appendChild(document.createTextNode(': ' + value));
  box.appendChild(line);
}

function paintViewerFacts(record) {
  const box = E('viewer-facts');
  box.textContent = '';
  if (!record) return;
  const words = String(record.prompt || '');
  // A render's record states what it was asked for; when it says nothing, that
  // is named rather than left as a blank line.
  if (record.model) {
    addFact(box, 'prompt', words !== '' ? words : 'no words were recorded for this picture');
    addFact(box, 'model', modelNamed(record.model) + ' (' + String(record.model) + ')');
  }
  if (record.width && record.height) {
    addFact(box, 'made at', record.width + ' × ' + record.height +
      ' · steps ' + (record.steps || 'not recorded') +
      ' · seed ' + (Number(record.seed) >= 0 ? record.seed : 'chance'));
  }
  if (record.file) {
    const when = ui.whenText(record.created);
    addFact(box, 'file', record.file + (when === '' ? '' : ' · kept ' + when));
  }
  if (record.source) addFact(box, 'source', String(record.source));
}

// The renders this same ask made, in a row under the picture: pressing one puts
// it in the frame and the ring moves to it. It is the same question the card's
// row asks - which of these runs do you want to look at - so it works the same
// way in both places.
function paintViewerRuns(record, group) {
  const row = E('viewer-runs');
  if (!row) return;
  row.textContent = '';
  if (!record || !group || group.length < 2) {
    row.hidden = true;
    return;
  }
  row.hidden = false;
  const words = document.createElement('span');
  words.className = 'runs-count';
  words.textContent = 'the ' + group.length + ' runs of this ask';
  words.title = 'The renders this same prompt and these same settings made, newest first. Press one to put it in the frame.';
  row.appendChild(words);
  group.forEach(function (one, at) {
    const thumb = document.createElement('img');
    thumb.setAttribute('width', '52');
    thumb.setAttribute('height', '52');
    thumb.setAttribute('alt', 'run ' + (at + 1) + ' of ' + group.length + ' of this prompt and these settings');
    thumb.title = 'run ' + (at + 1) + ' of ' + group.length + ' · ' + ui.whenText(one.created) +
      ' · seed ' + (Number(one.seed) >= 0 ? one.seed : 'chance') + ' · press to show it';
    thumb.src = ui.fileSrc(cardPicture(one));
    if (String(one.id) === String(record.id)) thumb.classList.add('on');
    ui.onPress(thumb, function () {
      if (String(one.id) === openShot) return;   // already the picture on screen
      openRender(one);
    });
    row.appendChild(thumb);
  });
  // The words over the row name the clock a running scroll is on, so the time
  // being kept is stated where the runs themselves are named.
  paintRunsWords();
}

// The words over the row of runs: how many runs the ask made, and while a scroll
// runs, the time each of them stands. An ask whose runs are not on this page has
// no words to be given, and is not given invented ones.
function paintRunsWords() {
  const row = E('viewer-runs');
  if (!row) return;
  const words = row.querySelector('.runs-count');
  if (!words) return;
  const count = row.querySelectorAll('img').length;
  if (count < 2) return;
  const head = 'the ' + count + ' runs of this ask';
  words.textContent = scrollOn ? head + ' · scrolling, each stands ' + eachWords() : head;
  words.classList.toggle('scrolling', scrollOn);
}

// The other runs of the ask a picture belongs to, from the grouping the gallery
// drew: a picture that is the only one of its ask has no row to be given.
function groupFor(item) {
  const found = galleryGroups.get(stackKeyFor(item));
  return found && found.length > 1 ? found : null;
}

function showViewer(url, caption, record, group) {
  E('viewer').classList.add('on');
  const img = E('viewer-img');
  img.setAttribute('alt', caption);
  img.setAttribute('title', 'The picture on screen. The buttons change the zoom and move it; prev and next step through the list on this page.');
  // The same kept copy the card drew: opening a picture in the viewer asks the
  // machine for nothing.
  img.src = ui.fileSrc(url);
  E('viewer-cap').textContent = caption;
  paintViewerFacts(record);
  paintViewerRuns(record, group);
  // A picture always opens whole in the frame, however it was reached.
  paintZoom(kZoomFit);
  // The hand and the words follow the picture the frame now holds: a scroll
  // speaks of the ask on screen, not of the one it started on.
  paintScrollState();
}

// A render from the list is opened in the viewer as the same picture the card
// showed, and remembered by id so prev and next know which place it was at. The
// other runs of its own ask come with it, in the row under the frame.
function openRender(item) {
  // A picture moved to inside a frame that stands open is moved to with the move
  // the dropdown names: the scroll's own move, a press of prev and next, and a
  // press on a run under the frame are all the same move. Opening a picture in a
  // frame that was closed is not a move - it is the frame being opened, and it
  // shows that picture whole, as it is shown whenever it is reached.
  const moving = E('viewer').classList.contains('on');
  openShot = String(item.id);
  viewerAt = indexOfShown(openShot);
  scrollStack = stackKeyFor(item);
  scrollMove = moving ? String(kScrollWipes[scrollWipe] || '') : '';
  showViewer(cardPicture(item), shotCaption(item), item, groupFor(item));
  // The scroll moves through the runs of the picture on the frame, so a picture
  // that was moved to and made no stack ends the scroll where it stands.
  if (scrollOn && !scrollGroup()) {
    stopScroll('This ask made one render, so the scroll stops here: a scroll moves through the runs a stacked ask made.');
  }
}

function stepViewer(step) {
  if (viewerAt < 0) {
    log('This picture did not come from the list on this page, so nothing is beside it to open.', 'warn');
    return;
  }
  let to = viewerAt + step;
  // A standing card is not a picture yet: the step walks past it to the next
  // render there is.
  while (to >= 0 && to < shownItems.length && shownItems[to].pending) to += step;
  if (to < 0) {
    log('This is the first of the ' + shownItems.length + ' on this page. Nothing newer is on the screen.', 'warn');
    return;
  }
  if (to >= shownItems.length) {
    log('This is the last of the ' + shownItems.length + ' on this page. Nothing older is on the screen.', 'warn');
    return;
  }
  openRender(shownItems[to]);
}

function closeViewer() {
  // A scroll does not outlive the frame it scrolls: closing the picture takes
  // the clock away with the frame, so nothing moves a picture that is no longer
  // on the screen and no line keeps filling on a closed frame.
  stopScroll();
  scrollStack = '';
  scrollMove = '';
  E('viewer').classList.remove('on');
  E('viewer').classList.remove('grabbing');
  const img = E('viewer-img');
  // A picture with an empty address asks this page's own address again, so the
  // address is taken away rather than emptied.
  img.removeAttribute('src');
  img.removeAttribute('alt');
  img.removeAttribute('title');
  img.style.transform = '';
  // A move that was mid-way when the frame was closed is taken away with it, so
  // the next picture to be opened is not arrived at part-way.
  img.style.animation = '';
  // The size the page stated for the step that was open is taken away with it,
  // so the next picture to be opened is measured for its own frame again.
  img.style.width = '';
  img.style.height = '';
  img.style.maxWidth = '';
  img.style.maxHeight = '';
  // Nothing about the picture that was open is left on the frame.
  E('viewer-cap').textContent = '';
  E('viewer-facts').textContent = '';
  const runs = E('viewer-runs');
  runs.textContent = '';
  runs.hidden = true;
  E('viewer-stage').classList.remove('grabbing');
  openShot = '';
  viewerAt = -1;
  panX = 0;
  panY = 0;
  zoomWaits = -1;
}

// A picture larger than the frame is moved by holding it: the frame clips it, so
// the part outside would stay unreachable if it could not be moved.
function attachViewerPan() {
  const stage = E('viewer-stage');
  if (!stage) return;
  let held = null;
  stage.addEventListener('pointerdown', function (event) {
    if (!E('viewer').classList.contains('on')) return;
    held = { x: Number(event.clientX), y: Number(event.clientY) };
    stage.classList.add('grabbing');
    event.preventDefault();
  });
  stage.addEventListener('pointermove', function (event) {
    if (!held) return;
    panX = panX + (Number(event.clientX) - held.x);
    panY = panY + (Number(event.clientY) - held.y);
    held = { x: Number(event.clientX), y: Number(event.clientY) };
    clampPan();
    applyPan();
  });
  const release = function () {
    held = null;
    stage.classList.remove('grabbing');
  };
  stage.addEventListener('pointerup', release);
  stage.addEventListener('pointercancel', release);
}

// One re-draw of a list for what a text field holds, at most once in a burst of
// typing: the field answers its own typing, and the list is drawn from the words
// it holds once they settle rather than once per character.
const kFieldWordWait = 120;

function watchFieldWords(id, run) {
  const field = E(id);
  if (!field) return;
  let asked = false;
  const hear = function () {
    if (asked) return;
    asked = true;
    setTimeout(function () {
      asked = false;
      run();
    }, kFieldWordWait);
  };
  field.addEventListener('change', hear);
  field.addEventListener('input', hear);
  field.addEventListener('keyup', hear);
}

async function main() {
  // Picture handling is attached the moment the page opens, so a file can be
  // dropped on the prompt panel before anything is asked of the machine.
  attachPictureTargets();
  // So is the way a zoomed picture is moved: it needs no answer from the
  // machine, and neither does the arrival of a picture's bytes or the frame a
  // wider or narrower window was given this page.
  attachViewerPan();
  attachViewerArrival();
  attachViewerResize();
  // The last reading is replayed before anything is asked of the server, so the
  // machine's state — GPU, memory, what is busy — is on the page while it is
  // still being drawn; the live reading replaces it on arrival.
  const kept = ui.lastReading('/api/monitor');
  if (kept && kept.data) drawStatus(kept.data);
  // The page's own lists come back from what this browser kept for them, drawn
  // exactly as a live answer would draw them; the polls replace any of these
  // once the kept copy has aged past its time.
  const keptPrefs = ui.lastReading('/api/prefs');
  if (keptPrefs && keptPrefs.data) applyPrefs(keptPrefs.data);
  const keptModels = ui.lastReading('/api/models');
  if (keptModels && keptModels.data) {
    models = keptModels.data.models || [];
    renderModels();
    fillGalleryOptions();
  }
  // The same window the page asks the machine for, so the list this browser
  // replayed and the counts beside the model names describe one list rather
  // than two: a reading looked up under another window was never taken down
  // under the window the page really stands on, and left the gallery empty
  // until the first poll.
  const keptGallery = ui.lastReading(galleryPageUrl(galleryOffset));
  if (keptGallery && keptGallery.data) {
    galleryItems = keptGallery.data.items || [];
    galleryHeld = Number(keptGallery.data.total || galleryItems.length);
    if (keptGallery.data.offset !== undefined && keptGallery.data.offset !== null) {
      galleryOffset = Number(keptGallery.data.offset);
    }
    renderGallery(galleryItems);
  }
  const keptRefs = ui.lastReading('/api/refs');
  if (keptRefs && keptRefs.data) {
    refs = keptRefs.data.items || [];
    renderRefs();
  }
  // What this browser last had asked of the model in use comes back over the
  // controls - the words, the dials, and the strip of starting pictures - before
  // the page is drawn from it, so a reload opens on that ask and not on a card's
  // opening words.
  restoreAsk(modelChoice());
  syncPrompt();
  paintRunState();

  ui.onPress(E('gen-btn'), function () { generate(); });
  // The reset hand sits in the prompt panel's own title: it lets go of what this
  // browser kept for the model in use and takes that model's defaults back -
  // every other hand in this panel adds to the ask, this one returns it.
  ui.onPress(E('prompt-reset'), function () { resetAsk(); });
  // The quick batches sit to the right of the one generate hand and are drawn as
  // a caution, because what they ask costs that many pictures: each one is the
  // same price as the picture a single generate draws, and no setting makes a
  // batch cheaper per picture than one run.
  ui.onPress(E('gen-4x'), function () { generateMany(4); });
  // The deepest quick batch and the most pictures one ask may stand, in the
  // gallery and on the machine, are one number.
  ui.onPress(E('gen-16x'), function () { generateMany(kMaxBatchPictures); });
  // Each button names a step; the step is clamped to the ones that exist rather
  // than left off at either end, so a press always draws a picture.
  ui.onPress(E('viewer-fit'), function () { paintZoom(kZoomFit); });
  ui.onPress(E('viewer-out'), function () { paintZoom(zoomAt - 1); });
  ui.onPress(E('viewer-in'), function () { paintZoom(zoomAt + 1); });
  ui.onPress(E('viewer-full'), function () { paintZoom(kZoomReal); });
  ui.onPress(E('viewer-prev'), function () { stepViewer(-1); });
  ui.onPress(E('viewer-next'), function () { stepViewer(1); });
  ui.onPress(E('viewer-close'), function () { closeViewer(); });
  // The picture is an overlay over the whole page, so it owes the page its
  // ending back: the key everyone reaches for on an overlay closes it, and
  // when no picture is open the key belongs to whatever else has it.
  document.addEventListener('keydown', function (event) {
    if (event.key === 'Escape' && E('viewer').classList.contains('on')) {
      closeViewer();
    }
  });
  // The scroll: one hand starts it and stops it, and the three dropdowns beside
  // it name the time each picture stands, the move to the next, and how fast
  // that move is made. Its choices are this page's, kept in this browser, and
  // read back onto the fields before any picture can be opened; a time asked for
  // in the middle of a picture re-arms that picture's countdown, so nothing a
  // person asked to be stood longer is cut short by the asking.
  ui.onPress(E('viewer-play'), function () {
    if (scrollOn) stopScroll();
    else startScroll();
  });
  for (const field of ['viewer-each', 'viewer-wipe', 'viewer-speed']) {
    ui.onChoose(E(field), function () { readScrollChoices(); });
  }
  restoreScrollChoices();
  // The dropdowns say what to show, so the list answers the moment one of them
  // is moved; a change the page made itself while repopulating a list is not a
  // person's choice and leaves the screen as it was.
  for (const field of ['gallery-model', 'gallery-sort']) {
    ui.onChoose(E(field), function () {
      if (pageRebuiltChoice) return;
      renderGallery(galleryItems);
    });
  }
  // A typed search answers as well. An input's value set by hand fires no change
  // event, so the field's own typing is what asks for the list again — one
  // re-draw once the typing settles, not one per character.
  watchFieldWords('gallery-search', function () {
    renderGallery(galleryItems);
  });
  // There are no page hands here on purpose: the whole room is asked for and drawn,
  // so a person sees everything this machine keeps rather than a window of it that
  // has to be walked. The asks for the pages past the newest are batched, and the
  // list grows as each batch lands.
  ui.onPress(E('gallery-select-all'), function () {
    if (!shownItems.length) {
      log('Nothing is on this screen to mark.', 'warn');
      return;
    }
    for (const item of shownItems) markedShots.add(String(item.id));
    renderGallery(galleryItems);
    log('Marked the ' + shownItems.length + ' picture' + (shownItems.length === 1 ? '' : 's') +
      ' here. Pictures hidden by the model or search filter were left alone.', 'ok');
  });
  ui.onPress(E('gallery-clear'), function () {
    markedShots.clear();
    renderGallery(galleryItems);
    log('Nothing is marked here now.', 'warn');
  });
  ui.onPress(E('gallery-delete-marked'), function () {
    const targets = [];
    for (const item of shownItems) {
      if (markedShots.has(String(item.id))) targets.push(item);
    }
    if (!targets.length) {
      log('No picture on this screen is marked. Mark them first with select all.', 'warn');
      return;
    }
    void (async function () {
      const gone = await deleteRenders(targets);
      if (gone > 0) {
        log('Deleted ' + gone + ' marked render' + (gone === 1 ? '' : 's') +
          ' and their files.', 'warn');
      }
    })().catch(function (failed) {
      log('the delete did not finish: ' + (failed && failed.message ? String(failed.message) : String(failed)), 'err');
    });
  });
  ui.onPress(E('gallery-delete-shown'), function () {
    if (!shownItems.length) {
      log('Nothing is on this screen to delete.', 'warn');
      return;
    }
    // The list as it stands now: the delete redraws the screen, which makes way
    // for a new list.
    const shown = shownItems.slice();
    void (async function () {
      const gone = await deleteRenders(shown);
      if (gone > 0) {
        log('Deleted ' + gone + ' render' + (gone === 1 ? '' : 's') + ' and their files.', 'warn');
      }
    })().catch(function (failed) {
      log('the delete did not finish: ' + (failed && failed.message ? String(failed.message) : String(failed)), 'err');
    });
  });
  for (const key in pillKeys) {
    const node = E(pillKeys[key]);
    if (!node) continue;
    // A switch answers by changing colour, which the style sheet eases; the
    // machine's own answer is read back after the write, so a switch the
    // engine would not take never leaves the page and the engine disagreeing.
    node.addEventListener('click', function () {
      if (node.classList.contains('on')) node.classList.remove('on');
      else node.classList.add('on');
      void (async function () {
        const saved = await savePrefs();
        if (!saved.answer) {
          log('The machine did not take that switch: ' + key + ' (' +
            (saved.error || ui.askWhy(saved)) + ')', 'err');
          if (node.classList.contains('on')) node.classList.remove('on');
          else node.classList.add('on');
          return;
        }
        applyPrefs(saved.answer);
      })().catch(function (failed) {
        log('The switch did not finish: ' + key + ' (' +
          (failed && failed.message ? String(failed.message) : String(failed)) + ')', 'err');
      });
    });
  }

  // A machine that is briefly unreachable is a fact said once, not a fresh
  // line every round: the page keeps retrying in silence and says so again
  // only when the answer comes back.
  let quiet = false;
  await ui.poll(async function () {
    const answered = await refresh();
    if (quiet) {
      log('The machine answers again.', 'ok');
      quiet = false;
    }
    return answered;
  }, function (failed) {
    ui.headUnknown();
    if (!quiet) {
      log('The machine stopped answering: ' + failed + ' - retrying quietly.', 'err');
      quiet = true;
    }
  }, ui.pollMs);
}

main();
