// Voice studio: model cards read from the linked audio.cpp engine, a text
// panel whose dials are the model's own declared options, the shared voice
// room, and the kept renders. Nothing here knows a model's shape by heart:
// the parameters a card shows are the ones the model's own handle declared,
// and a model that stands loaded says so - a model that does not is drawn
// without dials rather than with invented ones.
const listPane = ui.E('models-pane-list');
const voiceLibraryPane = ui.E('voice-library-pane');
const voiceKeepZone = ui.E('voice-keep-zone');
const progressPanel = ui.E('progress-panel');
const rendersPane = ui.E('renders-pane');
const updated = ui.E('voice-updated');
const log = ui.log;

let engineStatus = null;
let lastModels = [];
// The place of each card in the list above, by its id: the page reads a card by
// name long after the page it came from was drawn, and a list of hundreds walked
// for every card the page draws is work it repeats for nothing.
const cardIndex = new Map();
let chosenModel = '';
let chosenPresets = [];
let libraryVoices = [];
let lastRenderList = [];

// ---------- shared words ----------

// The card this page holds for that id, answered in one step through the index
// that walks beside the list.
function cardAt(id) {
  const heldAt = cardIndex.get(String(id));
  if (heldAt === undefined) return null;
  return lastModels[heldAt];
}

function nameOf(id) {
  const held = cardAt(id);
  if (held) return String(held.name || held.id);
  return String(id);
}

function modelById(id) {
  const held = cardAt(id);
  if (held) return held;
  return null;
}

// A wav this page just finished, played straight from the file the server
// kept; the url comes from the job record itself.
function playUrl(url) {
  return ui.fileSrc(url);
}


// ---------- say it: one set of controls, redrawn when the model changes ----

let saySelect = null;
let sayVoice = null;
let sayField = null;
let sayBtn = null;
// The dials a model declares belong to two places: the controls of a single run
// sit beside the door that runs, and the controls of the model's standing copy
// - the settings a session is built with, such as how its weights are kept or
// how much of its state it caches - sit beside that door too, where changing one
// starts a new standing copy rather than answering with the wrong settings. Each
// door keeps its own fields, so a model picked at one door never shows the other
// door's dials. A field left blank is not sent, so the model's own defaults hold.
const optionDoors = {
  say: { runZone: 'say-options', standZone: 'say-session', runRows: {}, standRows: {}, drawnFor: '' },
  listen: { runZone: 'listen-options', standZone: 'listen-session', runRows: {}, standRows: {}, drawnFor: '' },
};

// the text field, both pickers, and the option rows live in the page file;
// this code wires them, fills the pickers, and answers the choices.
function ensureSayZone() {
  if (saySelect) return;
  saySelect = ui.E('say-model');
  sayVoice = ui.E('say-voice');
  sayField = ui.E('say-field');
  if (!saySelect) return;
  ui.onChoose(saySelect, function () {
    const wanted = String(ui.chosenOption(saySelect) || '');
    const item = modelById(wanted);
    if (item && item.loaded) {
      chosenModel = wanted;
      void (async function () {
        const voices = await ui.api('/voice/api/voices/' + wanted);
        if (voices && Array.isArray(voices.presets)) {
          chosenPresets = voices.presets;
          drawSayZone(lastModels);
        }
      })();
    } else if (wanted) {
      log('that model holds no memory: press its load hand first', 'err');
      ui.toastPush('That model is not loaded yet. Press load on its card first.', 'warn');
    }
  });
  const zone = ui.E('voice-say-zone');
  sayBtn = ui.fingerAct('speak it', function () {
    void (async function () {
      await speakNow();
    })();
  });
  if (zone) zone.appendChild(sayBtn);
}

function uiOptionRow(param) {
  // One dial per declared request option: a number gets a number field when
  // the model states its bounds; everything else gets a word field. The
  // field's own text is the model's description of the option.
  const row = document.createElement('div');
  row.className = 'opt-row';
  const field = document.createElement('input');
  const numeric = param.min !== '' && param.max !== '';
  field.type = numeric ? 'number' : 'text';
  const label = String(param.name);
  field.name = 'opt-' + label;
  const labelNode = document.createElement('label');
  labelNode.textContent = label + (String(param.description) ? ' — ' + String(param.description) : '');
  row.appendChild(labelNode);
  row.appendChild(field);
  return row;
}

function drawOptionsFor(door, model) {
  const place = optionDoors[door];
  if (!place || place.drawnFor === String(model)) return;
  place.drawnFor = String(model);
  place.runRows = {};
  place.standRows = {};
  const runZone = ui.E(place.runZone);
  const standZone = ui.E(place.standZone);
  if (runZone) runZone.textContent = '';
  if (standZone) standZone.textContent = '';
  const item = modelById(String(model));
  const params = item && Array.isArray(item.params) ? item.params : [];
  for (const param of params) {
    // A control is offered where the engine takes it: a run control at the door,
    // a standing-model control beside it. The controls of a model's files belong
    // to the load hand, which already reads them off that card.
    const scope = String(param.scope);
    const atRun = scope === 'request';
    const atStand = scope === 'session';
    if (!atRun && !atStand) continue;
    const row = uiOptionRow(param);
    const field = row.querySelector('input');
    if (atRun) {
      place.runRows[String(param.name)] = field;
      if (runZone) runZone.appendChild(row);
    } else {
      place.standRows[String(param.name)] = field;
      if (standZone) standZone.appendChild(row);
    }
  }
}

// The values a person moved: blank fields are not sent, so the model keeps what
// its own release asks for.
function collectRows(rows) {
  const out = {};
  for (const key of Object.keys(rows)) {
    const field = rows[key];
    if (!field) continue;
    const value = String(field.value || '');
    if (value === '') continue;
    out[key] = value;
  }
  return out;
}

function collectOptions() {
  return collectRows(optionDoors.say.runRows);
}

function collectStanding(door) {
  const place = optionDoors[door];
  return place ? collectRows(place.standRows) : {};
}

function drawSayZone(models) {
  ensureSayZone();
  if (!saySelect || !sayVoice) return;
  // keep a human choice that still names a loaded model; clear it otherwise.
  if (chosenModel) {
    let still = false;
    for (const item of models) {
      if (String(item.id) === String(chosenModel) && item.loaded) still = true;
    }
    if (!still) chosenModel = '';
  }
  saySelect.textContent = '';
  for (const item of models) {
    if (!item.loaded) continue;
    const option = document.createElement('option');
    option.value = String(item.id);
    option.textContent = nameOf(String(item.id));
    saySelect.appendChild(option);
  }

  sayVoice.textContent = '';
  const noneOption = document.createElement('option');
  noneOption.value = '';
  noneOption.textContent = '(no voice)';
  sayVoice.appendChild(noneOption);
  for (const voice of libraryVoices) {
    const option = document.createElement('option');
    option.value = String(voice.name);
    option.textContent = String(voice.name);
    sayVoice.appendChild(option);
  }
  for (const preset of chosenPresets) {
    const option = document.createElement('option');
    option.value = String(preset.name);
    option.textContent = String(preset.name);
    sayVoice.appendChild(option);
  }

  drawOptionsFor('say', chosenModel);
}

async function speakNow() {
  if (!chosenModel) {
    log('no loaded model to speak with', 'err');
    return;
  }
  const text = String(ui.textField('say-field') || '');
  if (!text.trim()) {
    log('nothing to say', 'err');
    return;
  }
  const body = {
    model: chosenModel,
    text: text,
  };
  const voice = String(ui.chosenOption(sayVoice) || '');
  if (voice) body.voice = voice;
  const options = collectOptions();
  // a model that declares the option for stating where its answer lands is
  // asked to state it: the option is the model's own declared name, read off
  // the card, never one kept here by heart.
  const card = modelById(chosenModel);
  if (card && Array.isArray(card.params)) {
    for (const param of card.params) {
      if (String(param.scope) !== 'request') continue;
      if (String(param.name).toLowerCase().indexOf('timestamp') < 0) continue;
      if (!String(options[param.name] || '')) options[param.name] = 'true';
    }
  }
  const optionKeys = Object.keys(options);
  if (optionKeys.length) {
    body.options = {};
    for (const key of optionKeys) body.options[key] = options[key];
  }
  const standing = collectStanding('say');
  const standingKeys = Object.keys(standing);
  if (standingKeys.length) {
    body.session_options = {};
    for (const key of standingKeys) body.session_options[key] = standing[key];
  }

  const started = await ui.postJob('/voice/api/speak', body);
  if (!started.job) {
    log('speak refused: ' + ui.askWhy(started), 'err');
    ui.toastNo('start that speaking', started);
    return;
  }
  log('speaking with ' + nameOf(chosenModel), '');
  void (async function () {
    await followSpeak(started.job, chosenModel);
  })();
}

// The runs this page is watching, by job, so the header names every job the page
// knows is running rather than only the one its own loop is following. A voice
// engine runs inside the server, so this page is the only place that would learn
// about such a run before the next machine reading - the header bar follows the
// run here, in the words the other pages read it in, and stops when it does.
const pageJobs = {};

function paintHeader(job, state) {
  if (state && String(state.state) === 'running') pageJobs[String(job)] = state;
  else delete pageJobs[String(job)];
  const watched = [];
  for (const key of Object.keys(pageJobs)) watched.push(pageJobs[key]);
  ui.headerState(watched);
}

async function followSpeak(job, model) {
  // no counted steps exist: the bar counts real seconds, as the progress
  // panel's own title says.
  const startedAt = Date.now();
  while (true) {
    const state = await ui.api('/voice/api/jobs/' + job);
    paintHeader(job, state);
    if (!state) {
      log('the job stopped answering; it stays in the records', 'err');
      break;
    }
    const elapsed = Date.now() - startedAt;
    ui.progressSet(progressPanel, job, 0, 1,
      'speaking · ' + Math.floor(elapsed / 1000) + 's', 'load');
    if (String(state.state) === 'running') {
      await ui.sleepWindow('/voice/api/jobs/' + job, 400, 5000);
      continue;
    }
    ui.progressClear(progressPanel, job);
    if (String(state.state) === 'done') {
      log('a render stands: ' + String(state.model || ''), '');
      drawRenderResult(state);
    } else if (String(state.state) === 'cancelled') {
      log('stopped: ' + String(state.model || ''), 'stop');
    } else {
      log('the run refused: ' + ui.askWhy(state.error), 'err');
      ui.toastNo('speak that text', state.error);
    }
    break;
  }
}

// A package's files coming across the wire. The card that offered the hand said
// its files are not on this machine; leaving that card standing after they
// arrived is what made a finished fetch look like a failed one, and made a person
// reload the page or restart the server to be believed. So the page waits on the
// job it started, says how it ended, and redraws the card from the machine's own
// answer - which states the files as on disk the moment they are.
async function followFetch(modelId, job) {
  const startedAt = Date.now();
  const key = 'fetch-' + String(job);
  ui.toastTrack(key, 'Fetching the files for ' + nameOf(modelId), 'wait');
  while (true) {
    const state = await ui.api('/voice/api/jobs/' + job);
    if (!state) {
      ui.toastEnd(ui.toastTrack(key, '', 'err'),
        'The fetch stopped answering. Its job is still in the records.', 'err');
      break;
    }
    const max = Number(state.max || 0);
    const done = Number(state.done || 0);
    ui.progressSet(progressPanel, job, max > 0 ? done : 0, max,
      (max > 0 ? 'fetching · ' + done + ' of ' + max : 'fetching') +
      ' · ' + Math.floor((Date.now() - startedAt) / 1000) + 's', 'load');
    if (String(state.state) === 'running') {
      await ui.sleepWindow('/voice/api/jobs/' + job, 400, 5000);
      continue;
    }
    ui.progressClear(progressPanel, job);
    if (String(state.state) === 'done') {
      ui.toastEnd(ui.toastTrack(key, '', 'ok'),
        nameOf(modelId) + ' has its files on this machine now.', 'ok');
      log('its files are down: ' + String(state.model || modelId), 'ok');
      refreshModels();
      void (async function () { await refresh(); })();
    } else if (String(state.state) === 'cancelled') {
      ui.toastEnd(ui.toastTrack(key, '', 'warn'), 'The fetch was stopped.', 'warn');
      log('stopped fetching ' + modelId, 'stop');
      refreshModels();
    } else {
      const why = ui.askWhy(state.error);
      ui.toastEnd(ui.toastTrack(key, '', 'err'),
        'Could not fetch the files for ' + nameOf(modelId) + (why ? ': ' + why : '.'), 'err');
      log('the fetch refused: ' + why, 'err');
      refreshModels();
    }
    break;
  }
}

// What the model answered is kept as the render the library draws: the job's own
// words name what was made, so a render that was just made reads the same way as
// one that was kept, and its hands work the same way.
function drawRenderResult(record) {
  const url = String(record.url || '');
  if (!url) {
    log('that render answered with no audio to keep', 'err');
    return;
  }
  if (!rendersPane) return;
  const item = {
    id: String(record.id || ''),
    model: String(record.model || ''),
    voice: String(record.voice || ''),
    audio_s: Number(record.audio_s || 0),
    wav: url,
    said_words: String(record.said_words || ''),
    word_times: Array.isArray(record.word_times) ? record.word_times : [],
    made_ms: Number(record.elapsed_ms || 0),
  };
  rendersPane.insertBefore(renderRow(item), rendersPane.firstChild);
}

// A file door, answered the way the browser asks and the picture doors on the
// image page have shown works: the change event names the field it answered,
// and that field carries the files. The list is walked by index, and the form
// holding the field is stopped from its default action, so choosing a file
// never navigates the page to it. The answer is the file itself - this machine
// never invents a path, and never asks the server to read a local file.
function keepFileDoor(fieldId, formId, handler) {
  const field = ui.E(fieldId);
  if (field) {
    field.addEventListener('change', function (event) {
      const chosen = (event && event.target && event.target.files) ||
        (field.files ? field.files : null);
      if (!chosen || !chosen.length) return;
      handler(chosen);
    });
  }
  const form = ui.E(formId);
  if (form) {
    form.addEventListener('submit', function (event) {
      event.preventDefault();
    });
  }
}

// How long an answer took, in milliseconds: a job record names this elapsed_ms,
// written once when the job ends by whichever module owns it - the one name the
// monitor reads too. A record that carries no such number stays zero, and the
// room that shows it says the time was not stated rather than drawing a zero.
function answerTakenMs(record) {
  const stated = Number(record.elapsed_ms);
  return Number.isFinite(stated) && stated > 0 ? stated : 0;
}

// The newest results a room of this page keeps on screen. A listen's words are not
// kept as a list the machine can hand back - they live in a job record that is
// gone when the machine is restarted - so the page holds what its own session
// made, and says how many it shows rather than growing without end. The noun names
// what the room's rows are, in the room's own words.
const keptPageRows = 12;

function addNewestRow(pane, row, noun) {
  pane.insertBefore(row, pane.firstChild);
  while (pane.children.length > keptPageRows) pane.removeChild(pane.lastChild);
  if (!pane.getAttribute('title')) {
    pane.setAttribute('title', 'the newest ' + keptPageRows + ' ' + noun + ' of this page, newest first');
  }
}

// ---------- the model cards ----------

// The cards are the models: a card carries its files' state, the bytes those
// files are, and what the model's own handle states while it stands - its tasks,
// whether it listens to a recording, whether it designs from words, whether it
// states where words land, its languages, its options, its packaged voices. The
// two hands a card offers are its own: put its weights into the server, or give
// that memory back. A card whose words come from its package's own spec says so,
// rather than dressing up as a model that stands loaded. The packages stand
// grouped under their family, and the families that can answer now come first.
// ---------- the model room: the counts, the filters, one page of cards ----------
// Nothing here remembers a model, a family, or a task list. The counts are the
// engine's own house answer, the words of a filter are the words that answer
// named, and the cards are one page of what that filter matched - so a page
// never offers a filter this machine has nothing for, and never draws stats it
// made up. The order the cards stand in is the order the answer was asked in.

const PAGE_CARDS = 40;

// The counts and the words the house table names change far slower than the cards
// do, and a minute between its asks is enough to show the load or the put-down of
// one model: what is asked there is what the filters below are built from.
const houseAskMs = 60000;
// Every model that stands loaded, for the studio doors: the room of memory is asked
// for at once, and a machine cannot stand more loaded than this.
const loadedAskRows = 200;
// A filter field is not for a wall of words: the language words are offered in the
// order the answer says they matter, and a word that is not offered may still be
// typed in the find field.
const languageWordsOffered = 40;

let house = null;            // the last house answer, kept until it ages
let houseAge = 0;            // when it was read; the counts age slower than the cards
let lastAnswer = null;       // the last page answer: its count is what the cursor says
let pageAt = 0;              // the card the page is drawing first
let usableModels = [];       // every model that stands loaded, whatever the filter asks

function familyRows() {
  return house && Array.isArray(house.family_rows) ? house.family_rows : [];
}

function familyStats(family) {
  for (const row of familyRows()) {
    if (String(row.family) === String(family)) return row;
  }
  return null;
}

function askedFilter() {
  const words = [];
  const family = ui.selectValue('model-family');
  if (family && family !== 'any') words.push('family=' + encodeURIComponent(family));
  const task = ui.selectValue('model-task');
  if (task && task !== 'any') words.push('task=' + encodeURIComponent(task));
  const language = ui.selectValue('model-language');
  if (language && language !== 'any') words.push('language=' + encodeURIComponent(language));
  const state = ui.selectValue('model-state');
  if (state === 'memory') words.push('state=memory');
  else if (state === 'disk') words.push('state=disk');
  else if (state === 'missing') words.push('state=missing');
  const wanted = ui.textField('model-search').trim();
  if (wanted) words.push('q=' + encodeURIComponent(wanted));
  const order = ui.selectValue('model-sort');
  if (order === 'biggest') words.push('sort=bytes');
  else if (order === 'named') words.push('sort=name');
  return words.join('&');
}

// The counts, and the words a filter may use: read again only when they age.
async function askHouse() {
  if (house && Date.now() - houseAge < houseAskMs) return;
  const answer = await ui.api('/voice/api/model-stats');
  if (!answer) return;
  house = answer;
  houseAge = Date.now();
  drawHouseStats();
  drawFilterChoices();
}

// One page of the cards, asked in the words the page's own controls show.
async function askCards(offset) {
  const filter = askedFilter();
  const path = '/voice/api/model-cards?offset=' + offset + '&limit=' + PAGE_CARDS +
    (filter ? '&' + filter : '');
  const answer = await ui.api(path);
  if (!answer) return null;
  lastAnswer = answer;
  pageAt = Number(answer.offset || offset);
  // A page a person was reading belongs to the set they were reading. When the
  // words above narrow that set and this offset is past its end, the door hands
  // back no cards, and the last page of what matches now is asked again - a
  // panel that would otherwise stand empty under words that name cards.
  const rows = Array.isArray(answer.rows) ? answer.rows : [];
  if (rows.length === 0 && offset > 0) {
    const total = Number(answer.total || 0);
    if (offset >= total) {
      const step = Number(answer.limit || PAGE_CARDS);
      return await askCards(total > step ? Math.floor((total - 1) / step) * step : 0);
    }
  }
  return rows;
}

// Whatever this machine can answer with right now, whatever the page is
// filtering: the pickers of the studio doors are filled from these, so a page
// that narrows the cards never hides a model the doors could answer with.
async function askUsable() {
  const answer = await ui.api('/voice/api/model-cards?state=memory&limit=' + loadedAskRows);
  // a reading that did not answer is told apart from one that says nothing stands
  // loaded: the first keeps the page's list, the second is reality.
  if (!answer || !Array.isArray(answer.rows)) return null;
  usableModels = answer.rows;
  mergeCards(usableModels);
  return usableModels;
}

function drawHouseStats() {
  const line = ui.E('model-stats-line');
  if (!line || !house) return;
  line.textContent = '';
  // the linked engine is named by the version it states about itself: its own
  // build words when its build asked for them, and otherwise the version its
  // interface header states - a page that printed "engine unknown" named nothing.
  const engineNamed = house.build && house.build !== 'unknown'
    ? 'engine ' + String(house.build)
    : 'engine audio.cpp · interface ' + String(house.abi_named || 'not stated');
  line.appendChild(ui.finger(engineNamed, 'cap',
    'The audio.cpp engine, linked into this server: no other window, no other port.'));
  line.appendChild(ui.finger(String(Number(house.packages || 0)) + ' packages in ' +
    String(Number(house.families || 0)) + ' families', 'cap',
    'What this machine can offer: every package the engine knows, counted by family.'));
  const noLoader = Array.isArray(house.families_without_loaders_named) ? house.families_without_loaders_named : [];
  if (noLoader.length)
    line.appendChild(ui.finger(String(Number(house.packages_without_loaders || 0)) +
      ' packages the set names are not in this build · ' + noLoader.join(' · '), 'warn',
      "The engine's own card set names these families, and this build carries no loader that runs " +
      "them. " +
      'They are left out of the cards rather than offered as a model no door could answer with, and' +
      ' counted here so the ' +
      'page states the whole set it was made from.'));
  line.appendChild(ui.finger(String(Number(house.present || 0)) + ' with files here · ' +
    String(Number(house.loaded || 0)) + ' in memory', 'ok',
    'The packages whose files sit in this weights room, and the models standing loaded in this ' +
    'server.'));
  line.appendChild(ui.finger(ui.sizeText(Number(house.bytes || 0)), 'size',
    'The weight this machine holds for those packages, as the files really are on disk.'));
  const tasks = Array.isArray(house.tasks) ? house.tasks : [];
  if (tasks.length)
    line.appendChild(ui.finger('this machine names ' + tasks.length + ' tasks · ' + tasks.join(' · '), 'cap',
      'The task words these packages name. A filter offers only these.'));
  const languages = Array.isArray(house.languages) ? house.languages : [];
  if (languages.length)
    line.appendChild(ui.finger('speaks ' + languages.length + ' languages', 'cap',
      'The languages those families name, counted across the set.'));
}

// The picker of a filter is filled from the words the house answer named, so a
// page cannot offer a family, a task, or a language this machine has nothing
// for. A word that hundreds of families name would fill the field with a wall,
// so the words are offered in the order the answer says they matter - and any
// word that is not offered may still be typed into the find field, which
// matches the languages a card names.
function countFamiliesNaming(words) {
  const counts = {};
  for (const row of familyRows()) {
    // The family rows name the languages they speak under that one key, in the
    // words the machine owns; the page asks for that name and nothing else.
    const named = Array.isArray(row.languages) ? row.languages : [];
    for (const word of named) {
      const key = String(word);
      counts[key] = (counts[key] || 0) + 1;
    }
  }
  const options = [];
  for (const word of words) {
    const key = String(word);
    options.push({ id: key, label: key + ' · ' + String(counts[key] || 0) + ' families', at: counts[key] || 0 });
  }
  options.sort(function (a, b) {
    if (a.at !== b.at) return b.at - a.at;
    return String(a.id).localeCompare(String(b.id));
  });
  return options;
}

function drawFilterChoices() {
  if (!house) return;
  const rows = familyRows();
  const familyOptions = [];
  for (const row of rows) {
    familyOptions.push({
      id: String(row.family),
      label: String(row.name || row.family) + ' · ' + String(Number(row.packages || 0)) + ' packages',
    });
  }
  familyOptions.sort(function (a, b) { return String(a.label).localeCompare(String(b.label)); });
  fillOptions('model-family', familyOptions);
  const tasks = [];
  for (const word of (Array.isArray(house.tasks) ? house.tasks : [])) {
    const token = String(word);
    // The engine's own word stays the value the server narrows by; the label
    // names the door this page answers that kind of work at, so a person can
    // tell a kind they can ask about from one a model only states.
    const door = doorFor(token);
    tasks.push({
      id: token,
      label: door === 'speak' ? token + ' · speaking door'
        : door === 'listen' ? token + ' · listening door'
          : token + ' · no door on this page',
    });
  }
  tasks.sort(function (a, b) { return String(a.id).localeCompare(String(b.id)); });
  fillOptions('model-task', tasks);
  const languageWords = countFamiliesNaming(Array.isArray(house.languages) ? house.languages : []);
  const offered = languageWords.slice(0, languageWordsOffered);
  fillOptions('model-language', offered);
  const note = ui.E('model-language-note');
  if (note) {
    note.textContent = offered.length < languageWords.length
      ? languageWords.length + ' language words are named here; the ' + offered.length + ' that most families name are offered - any other word may be typed in find.'
      : 'every language word this machine names is offered.';
  }
}

function fillOptions(id, options) {
  const select = ui.E(id);
  if (!select) return;
  const chosen = ui.selectValue(id);
  select.textContent = '';
  const any = document.createElement('option');
  any.value = 'any';
  any.textContent = 'any';
  select.appendChild(any);
  for (const item of options) {
    const option = document.createElement('option');
    option.value = String(item.id);
    option.textContent = String(item.label);
    select.appendChild(option);
  }
  ui.chooseOption(select, chosen);
}

// Every card this page has seen, kept for the words only - a name is asked
// long after its page was drawn, by a render record or a studio door. The words
// the machine answered most recently are the words a card keeps, so a door that
// reads a card reads what is true now and not what was true the first time this
// page saw it.
function mergeCards(rows) {
  if (!Array.isArray(rows)) return;
  for (const item of rows) {
    const id = String(item.id || '');
    if (id === '') continue;
    const heldAt = cardIndex.get(id);
    if (heldAt === undefined) {
      cardIndex.set(id, lastModels.length);
      lastModels.push(item);
      continue;
    }
    lastModels[heldAt] = item;
  }
}

// The same words for the card set: every field a person moved is named as itself.
function modelsWhyEmpty() {
  const causes = [];
  if (ui.textField('model-search').trim()) causes.push('the words typed here');
  const family = ui.selectValue('model-family');
  if (family && family !== 'any') causes.push('the family picked here');
  const task = ui.selectValue('model-task');
  if (task && task !== 'any') causes.push('the task picked here');
  const language = ui.selectValue('model-language');
  if (language && language !== 'any') causes.push('the language picked here');
  const state = ui.selectValue('model-state');
  if (state && state !== 'any') causes.push('the state picked here');
  if (!causes.length) return 'this machine names no packages';
  return 'nothing matches ' + causes.join(' - ');
}

// The page cursor: what the filters matched, which cards of that set are drawn
// now, and the hands that move through the set. A set of hundreds of packages
// is read a page at a time, so the page never carries a wall of cards.
function drawPageCursor() {
  const line = ui.E('model-page-cursor');
  if (!line) return;
  line.textContent = '';
  if (!lastAnswer) {
    line.textContent = 'the counts are not read yet';
    return;
  }
  const total = Number(lastAnswer.total || 0);
  const drawn = Array.isArray(lastAnswer.rows) ? lastAnswer.rows.length : 0;
  // The hands move by the page size the answer itself used, and the door's own
  // word says whether another page exists: a page that stepped by a size it
  // assumed would skip cards whenever the answer answered a different one.
  const step = Number(lastAnswer.limit || PAGE_CARDS);
  const more = lastAnswer.more === undefined ? (pageAt + drawn < total)
    : lastAnswer.more === true;
  const words = document.createElement('span');
  words.textContent = total === 0
    ? modelsWhyEmpty()
    : total + ' card' + (total === 1 ? ' matches' : 's match') + ' · showing ' + (pageAt + 1) + '-' + (pageAt + drawn);
  words.title = "The count is the engine's own: it answers the whole set, and this page draws one part of it.";
  line.appendChild(words);
  if (pageAt > 0) {
    line.appendChild(ui.fingerAct('back', function () {
      pageAt = pageAt > step ? pageAt - step : 0;
      void (async function () { await refreshModels(); })();
    }, 'the page before this one'));
    line.appendChild(ui.fingerAct('first', function () {
      pageAt = 0;
      void (async function () { await refreshModels(); })();
    }, 'the first page of this set'));
  }
  if (more) {
    line.appendChild(ui.fingerAct('next', function () {
      pageAt = pageAt + step;
      void (async function () { await refreshModels(); })();
    }, 'the page after this one'));
    line.appendChild(ui.fingerAct('last', function () {
      pageAt = total > step ? (Math.floor((total - 1) / step) * step) : 0;
      void (async function () { await refreshModels(); })();
    }, 'the last page of this set'));
  }
}

function drawModels(models) {
  const page = Array.isArray(models) ? models : [];
  mergeCards(page);
  if (!listPane) return;
  listPane.textContent = '';
  if (!page.length) {
    // Words that match no card leave an empty panel: the cursor below says the
    // set is empty, but a person looking at the panel itself would read it as a
    // page that never filled. The words above are named where they are; the
    // panel says the same here.
    const none = document.createElement('div');
    none.className = 'meta';
    none.textContent = 'No card matches the fields above.';
    none.setAttribute('title',
      'The cards this machine answers about are named in the fields above - the words typed and ' +
      'the picks made - and none of them match those cards.');
    listPane.appendChild(none);
    drawPageCursor();
    return;
  }
  drawPageCursor();
  let familyShown = '';
  for (const item of page) {
    const family = String(item.family || '');
    if (family !== familyShown) {
      familyShown = family;
      const mark = document.createElement('div');
      mark.className = 'family-mark';
      const stats = familyStats(family);
      mark.textContent = String(item.family_name || family) + ' - ' +
        (stats ? String(Number(stats.packages || 0)) + ' packages · ' +
          String(Number(stats.present || 0)) + ' here · ' +
          String(Number(stats.loaded || 0)) + ' in memory · ' +
          ui.sizeText(Number(stats.bytes || 0))
          : 'this page of them');
      if (stats)
        mark.title = 'Across this machine: ' + String(Number(stats.packages || 0)) +
          ' packages named, ' + String(Number(stats.present || 0)) +
          ' with files here, ' + String(Number(stats.loaded || 0)) +
          ' standing in memory, weighing ' + ui.sizeText(Number(stats.bytes || 0)) +
          '. The languages and tasks below are the ones this family names.';
      const familyPicker = ui.E('model-family');
      if (familyPicker)
        mark.appendChild(ui.fingerAct('filter this family', function () {
          ui.chooseOption(familyPicker, family);
          void (async function () { await refreshModels(); })();
        }, 'show only this family'));
      listPane.appendChild(mark);
    }
    const id = String(item.id);
    const onDisk = item.weights_present === true;
    const live = item.loaded === true;
    // A package this machine has no files for is a card like any other, in muted
    // paint: it names what it is, what its package declares, where its files come
    // from, and the one hand it has - bring those files here. The layout is the
    // same so a machine without that package's weights still reads as a set of
    // cards rather than as lines among them.
    const marks = [];
    if (live) marks.push('model-on');
    if (!onDisk && !live) marks.push('model-rest');
    // The model the studio doors would answer with now carries the heavier
    // border, so the choice reads off the cards with nothing opened. Tapping a
    // card is a choice and nothing else: it loads no weights and fetches no
    // files.
    if (chosenModel === id) marks.push('model-chosen');
    const shell = ui.cardShell(marks.join(' '));
    const card = shell.card;

    const head = shell.main;
    const name = document.createElement('span');
    name.className = 'model-name';
    name.textContent = nameOf(id);
    head.appendChild(name);
    const brief = document.createElement('div');
    brief.className = 'model-note';
    brief.textContent = String(item.description || '');
    head.appendChild(brief);
    head.title = cardWords(item, id);
    ui.onPress(head, function () {
      if (!onDisk && !live) {
        log('that package has no files on this machine: its card names the hand that brings them', 'err');
        return;
      }
      if (chosenModel === id) {
        log(nameOf(id) + ' is already the model these doors answer with', 'err');
        return;
      }
      chosenModel = id;
      log('these doors will answer with ' + nameOf(id), 'ok');
      if (saySelect) {
        for (const option of ui.optionList(saySelect)) {
          if (String(option.value) === id) ui.chooseOption(saySelect, id);
        }
      }
      drawSayZone(usableModels);
    });

    const fingers = shell.fingers;
    // The state a card can wear is one of three: its memory is open, its files
    // are here, or neither - the last says what its package declares and where
    // its files come from.
    if (live) {
      fingers.appendChild(ui.finger('in memory', 'loaded',
        'Its weights are in this server, so it answers straight away.'));
    } else if (onDisk) {
      fingers.appendChild(ui.finger('files on disk', 'ok',
        'Its files are on this machine; the load hand on this card puts them in memory.'));
    } else {
      fingers.appendChild(ui.finger('no files here', 'warn',
        'This machine has no weights for that package, so it cannot answer yet; what its package ' +
        'declares is still stated.'));
    }
    if (chosenModel === id) {
      fingers.appendChild(ui.finger('in use', 'ok',
        'The model the studio doors answer with now. This card carries the heavier border; tapping ' +
        'another card puts that model in use instead.'));
    }
    const bytes = Number(item.weights_bytes || 0);
    const namedFiles = Array.isArray(item.files) ? item.files : [];
    if (bytes)
      fingers.appendChild(ui.finger(namedFiles.length + ' files · ' + ui.sizeText(bytes), 'size',
        'The files this package names, as they sit on this machine.'));
    const statedTasks = uniqueWords(Array.isArray(item.tasks_offline) && item.tasks_offline.length
      ? item.tasks_offline
      : (Array.isArray(item.declared_tasks) ? item.declared_tasks : []));
    if (statedTasks.length)
      fingers.appendChild(ui.finger('runs ' + statedTasks.join(' · '), 'cap',
        item.loaded ? 'The tasks this model states it runs, in the words this engine accepts for them.'
          : 'The tasks this package states it runs, in the words this engine accepts for them - a ' +
          'task name this engine has no word for is kept exactly as that package wrote it.'));
    // What this room can be asked to do with that model: the door each kind of
    // work is answered at, so a person reads which hand to press rather than
    // choosing between a list of words. A kind no door here is named for is
    // still stated, with that said under it, so a card never reads as a model
    // more of whose work this page could ask.
    const atSpeak = [];
    const atListen = [];
    const noDoor = [];
    for (const word of statedTasks) {
      const door = doorFor(word);
      if (door === 'speak') {
        if (atSpeak.indexOf(word) < 0) atSpeak.push(word);
      } else if (door === 'listen') {
        if (atListen.indexOf(word) < 0) atListen.push(word);
      } else if (noDoor.indexOf(word) < 0) {
        noDoor.push(word);
      }
    }
    if (atSpeak.length)
      fingers.appendChild(ui.finger('speaking door: ' + atSpeak.join(' · '), 'ok',
        'Asked at the speaking door: the model makes a voice from words, designs one from a ' +
        'description, or says new words in a voice this page was handed a recording of. The engine ' +
        'is opened for that kind of work when the door runs it.'));
    if (atListen.length)
      fingers.appendChild(ui.finger('listening door: ' + atListen.join(' · '), 'ok',
        'Asked at the listening door: the model answers about a recording this page was handed - ' +
        'the words it heard, who spoke and where, or the audio and labels it leaves behind. The ' +
        'engine is opened for that kind of work when the door runs it.'));
    if (noDoor.length)
      fingers.appendChild(ui.finger('no door for ' + noDoor.join(' · '), 'warn',
        'That work runs in this engine, and no door on this page is named for it. The card states a' +
        ' model can do more than this page can ask.'));
    // The same work is named two ways: this engine's words, which the doors use,
    // and the names that package carries for it, which is how its own docs read.
    // The search below matches either, so a card that shows only one hides a word
    // a person typed a search for - both are shown, the package's names only where
    // they add to what is already shown.
    const ownWords = [];
    for (const word of (Array.isArray(item.can) ? item.can : [])) {
      const low = String(word).toLowerCase();
      if (statedTasks.indexOf(low) >= 0) continue;
      if (ownWords.indexOf(low) < 0) ownWords.push(low);
    }
    if (ownWords.length)
      fingers.appendChild(ui.finger('its own names: ' + ownWords.join(' · '), 'cap',
        'What that package calls this work, in its own words. The engine words above are what the ' +
        'doors answer with; the filter and the search here reach both.'));
    if (item.speaker_reference === true)
      fingers.appendChild(ui.finger('listens to a recording', 'cap',
        'A voice it is given as a wav: it speaks new words in that same speaker.'));
    if (canListen(item))
      fingers.appendChild(ui.finger('answers from a recording', 'cap',
        'A recording comes in and it answers with what its own handle states from it: the words it ' +
        'heard and where they sit, the speakers it found, or the audio it made from it.'));
    if (item.style_condition === true)
      fingers.appendChild(ui.finger('takes style words', 'cap',
        'Directions in words travel with the line: an instruction, an emotion, how it speaks.'));
    if (item.timestamps === true)
      fingers.appendChild(ui.finger('states where words land', 'cap',
        'Asked with its own word-tracking option, it answers with the position of each word - the ' +
        'player below lights them up as the audio runs.'));
    const languages = uniqueWords(Array.isArray(item.languages) && item.languages.length
      ? item.languages
      : (Array.isArray(item.declared_languages) ? item.declared_languages : []));
    if (languages.length)
      fingers.appendChild(ui.finger('speaks ' + languages.join(' · '), 'cap',
        (item.loaded ? 'The languages this model states it speaks.'
          : "The languages this package's own spec names, before anything is loaded.") +
        ' The language filter below answers all of them.'));
    const params = Array.isArray(item.params) ? item.params : [];
    if (params.length) {
      // The controls a model's own engine declares, split by the scope it takes
      // them at - a name and a count alone would make a card read as a model that
      // answers with a number of options nobody can see. The names are printed,
      // and every one is repeated in the hint with what it defaults to and whether
      // the model says it is required; the panel's own dials are these.
      const byScope = {};
      for (const param of params) {
        const scope = String(param.scope || 'request');
        if (!byScope[scope]) byScope[scope] = [];
        byScope[scope].push(param);
      }
      const scopeWords = { request: 'one run', session: 'the standing model', load: 'its files' };
      for (const scope of ['request', 'session', 'load']) {
        const owned = byScope[scope];
        if (!owned) continue;
        const names = [];
        const detail = [];
        for (const param of owned) {
          const name = String(param.name || '');
          if (!name.length) continue;
          names.push(name);
          const marks = [];
          const at = String(param.default || '');
          if (at) marks.push('default ' + at);
          if (param.required === true) marks.push('required');
          detail.push(name + (marks.length ? ' (' + marks.join(', ') + ')' : ''));
        }
        if (!names.length) continue;
        const shown = names.length > 4 ? names.slice(0, 4).join(' · ') + ' · +' + (names.length - 4) : names.join(' · ');
        fingers.appendChild(ui.finger(shown + ' · ' + names.length + ' controls for ' + (scopeWords[scope] || scope), 'cap',
          'The run controls this model names at ' + (scopeWords[scope] || scope) + ', in its own words, each with what it defaults to and whether it says one is needed: ' +
          detail.join('\n') + '. The dials on this panel are these; a control that names no default keeps what that model asks for by itself.'));
      }
    }
    const presets = Array.isArray(item.preset_voices) ? item.preset_voices : [];
    if (presets.length)
      fingers.appendChild(ui.finger('ships ' + presets.length + ' voices', 'cap',
        'Packaged voices this card documents by name.'));
    const why = String(item.load_error || '');
    if (why)
      fingers.appendChild(ui.finger(why, 'err', 'The engine stated these words when the model refused to stand.'));
    if (onDisk && !live) {
      fingers.appendChild(ui.fingerAct('load', function () {
        void (async function () {
          const opened = await ui.api('/voice/api/models/' + encodeURIComponent(id) + '/load', 'POST', {});
          if (!opened) {
            log('that model stayed down: ' + (ui.apiError || 'the engine gave no words'), 'err');
            return;
          }
          log('opening ' + nameOf(id), 'ok');
          refreshModels();
        })();
      }, 'Put the weights this model names into the server. The answer is the engine\'s own.'));
    }
    // The hand of a package whose files are not here: the repo its own spec names.
    // A package that names no repo is a conversion this machine makes for itself,
    // and says so rather than showing a hand that leads nowhere.
    if (!onDisk && !live) {
      const repo = String(item.repo || '');
      if (repo) {
        fingers.appendChild(ui.finger('its files come from ' + repo, 'cap',
          'Where that package keeps its files, in its own spec words.'));
        fingers.appendChild(ui.fingerAct('bring its files', function () {
          void (async function () {
            const started = await ui.api('/voice/api/models/' + encodeURIComponent(id) + '/download', 'POST', {});
            if (!started || !started.job_id) {
              log(ui.apiError || 'that package names no files to fetch', 'err');
              return;
            }
            log('fetching ' + repo + ' for ' + id + ': ' + started.job_id, 'ok');
            await followFetch(id, started.job_id);
          })();
        }, 'the files come from the repo that package names'));
      } else {
        fingers.appendChild(ui.finger('names no repo', 'warn',
          'That package names no repo to bring its files from: it is a conversion this machine ' +
          'makes for itself.'));
      }
    }
    if (live) {
      fingers.appendChild(ui.fingerAct('put down', function () {
        void (async function () {
          const down = await ui.api('/voice/api/models/' + encodeURIComponent(id) + '/unload', 'POST', {});
          if (!down) {
            log(ui.apiError || 'that model kept its memory', 'err');
            return;
          }
          log('gave ' + nameOf(id) + ' its memory back', 'ok');
          refreshModels();
        })();
      }, 'Give the model its memory back. A later ask opens it again.'));
    }
    listPane.appendChild(card);
  }
}

// The cards again, as they are now: only the page the reader is looking at is
// asked again, in the same words the page's controls show, so a load or a put-
// down redraws what it changed rather than the whole set.
function refreshModels() {
  void (async function () {
    const rows = await askCards(pageAt);
    if (rows) drawModels(rows);
  })();
}

function cardWords(item, id) {
  return String(item.name || id) + '. ' + String(item.description || '') +
    (item.weights_present ? ' Its files are on disk.' : ' Its files are not on this machine.');
}

// ---------- voice library ----------

function drawVoiceLibrary(voices) {
  libraryVoices = Array.isArray(voices) ? voices : [];
  if (!voiceLibraryPane) return;
  voiceLibraryPane.textContent = '';
  for (const voice of libraryVoices) {
    const name = String(voice.name);
    const shell = ui.cardShell();
    const head = document.createElement('div');
    head.className = 'model-name';
    head.textContent = name;
    shell.main.appendChild(head);
    const state = document.createElement('div');
    state.className = 'model-shape';
    state.textContent = voice.has_transcript ? 'with its words' : 'no words kept';
    if (Number(voice.audio_s || 0) > 0) {
      const seconds = document.createElement('span');
      seconds.className = 'meta';
      seconds.textContent = Math.round(Number(voice.audio_s) * 10) / 10 + 's';
      state.appendChild(seconds);
    }
    shell.main.appendChild(state);
    const play = ui.fingerAct('play', function () {
      void (async function () {
        const node = document.createElement('audio');
        node.controls = true;
        node.src = await playUrl(String(voice.wav || ''));
        shell.main.appendChild(node);
      })();
    }, 'hear this voice');
    shell.fingers.appendChild(play);
    // A kept recording is a person's own: it can be renamed or let go from its
    // card, and the door answers with its own words either way.
    const toField = document.createElement('input');
    toField.type = 'text';
    toField.className = 'voice-rename-field';
    toField.title = 'the new name for this recording: letters, digits, dot, dash';
    shell.main.appendChild(toField);
    const rename = ui.fingerAct('rename this', function () {
      const given = String(toField.value || '');
      const wanted = safeVoiceName(given);
      if (!given.trim()) {
        log('no new name was given for this voice', 'err');
        return;
      }
      if (!wanted) {
        log('the name given carries no letters or digits: a voice name is made of letters, digits, dot, dash', 'err');
        return;
      }
      void (async function () {
        const moved = await ui.actDoor('/voice/api/voices/rename',
          { name: name, new_name: wanted });
        if (!moved.ok) log('the name did not move: ' + (moved.error || ''), 'err');
        else void (async function () { await refresh(); })();
      })();
    }, 'give this recording a new name; what was kept with it moves along');
    shell.fingers.appendChild(rename);
    const drop = ui.fingerAct('drop this', function () {
      void (async function () {
        const gone = await ui.actDoor('/voice/api/voices/delete', { name: name });
        if (!gone.ok) log('the voice was not let go: ' + (gone.error || ''), 'err');
        else void (async function () { await refresh(); })();
      })();
    }, 'forget this recording');
    shell.fingers.appendChild(drop);
    voiceLibraryPane.appendChild(shell.card);
  }
}

// What the browser last said about a file it would not hand over, or a sound it
// would not read. Kept beside the reading so the door that runs later can name
// which of the two this was, in the browser's own words rather than in a guess.
let browserWords = '';

// The line a door gives when a recording did not come through: the browser's own
// words when it gave any, since those name which of the two this was - a file it
// would not hand over, or a sound it cannot read - and the plain truth when it
// said nothing.
function refusedWords() {
  // The browser's own words say which of the two this was; the sentence under
  // them says what to bring instead, so a refusal is not the end of an attempt.
  return (browserWords || 'that recording was not read, or this browser could not decode it') +
    ' · wav, mp3, m4a, mp4, aac, flac, ogg, and webm are opened here and turned into the plain wav ' +
    'a model listens to, so what is left is a container or a codec this browser cannot hear - ' +
    'change it to one of those, or hand a wav, and the door works.';
}

async function readBytes(file) {
  // The browser's own byte reader is asked for first: it hands the bytes over
  // whole, with no words to turn back into them. The data-url door is kept only
  // as the fallback for an engine that will not answer a raw read, because
  // turning a file into base64 and back is four thirds of its bytes and a loop
  // over every one of them.
  browserWords = '';
  if (!file) return null;
  const raw = await new Promise(function (done) {
    const reader = new FileReader();
    reader.onload = function () { done(reader.result); };
    reader.onerror = function () { done(reader.error); };
    reader.onabort = function () { done(reader.error); };
    reader.readAsArrayBuffer(file);
  });
  if (raw instanceof ArrayBuffer) return new Uint8Array(raw);
  browserWords = raw && raw.message ? String(raw.message)
    : 'the browser would not hand this file over';
  const dataUrl = await new Promise(function (done) {
    const reader = new FileReader();
    reader.onload = function () { done(String(reader.result || '')); };
    reader.onerror = function () { done(''); };
    reader.onabort = function () { done(''); };
    reader.readAsDataURL(file);
  });
  const comma = dataUrl.indexOf(',');
  if (comma < 0) return null;
  const text = atob(dataUrl.slice(comma + 1));
  const bytes = new Uint8Array(text.length);
  for (let at = 0; at < text.length; ++at) bytes[at] = text.charCodeAt(at);
  return bytes;
}

function bytesToBase64(bytes) {
  let out = '';
  const chunk = 0x8000;
  for (let at = 0; at < bytes.length; at += chunk) {
    let part = '';
    const end = Math.min(at + chunk, bytes.length);
    for (let i = at; end > i; ++i) part += String.fromCharCode(bytes[i]);
    out += btoa(part);
  }
  return out;
}

// A file's words travel as text, so a base64 copy is four thirds of its bytes;
// the door every request here reads at most one body, of the size the shared
// library states. Naming a file that would not fit before it is sent is what
// lets a reader trim it: a refusal that names nothing leaves no move to make.
function base64IfItFits(bytes) {
  // the words around the file in that same body: field names and a short name
  const kWordsAroundAFile = 4096;
  const room = ui.bodyBytes - kWordsAroundAFile;
  if (Math.ceil(Number(bytes.length) / 3) * 4 > room) return null;
  return bytesToBase64(bytes);
}

function tooBigForOneRequest(bytes) {
  return 'that file is bigger than one request can carry: its own bytes are ' +
    ui.sizeText(Number(bytes.length)) + ', and the door reads at most two megabytes ' +
    'at a time. trim it, or pick a shorter part.';
}

function safeVoiceName(raw) {
  let name = String(raw || '');
  const dot = name.lastIndexOf('.');
  if (dot > 0) name = name.slice(0, dot);
  name = name.replace(/[^A-Za-z0-9_.-]/g, '_');
  while (name.length && name[0] === '.') name = name.slice(1);
  return name.slice(0, 64);
}

function keepFile(file) {
  const name = safeVoiceName(file && file.name);
  if (!name) {
    log('the file carries no usable name', 'err');
    return;
  }
  void (async function () {
    const wavBytes = await referenceBytes(file);
    if (!wavBytes) {
      log(refusedWords(), 'err');
      return;
    }
    const keptBytes = base64IfItFits(wavBytes);
    if (!keptBytes) {
      log(tooBigForOneRequest(wavBytes), 'err');
      return;
    }
    const answer = await ui.actDoor('/voice/api/voices/save',
      { name: name, audio_b64: keptBytes });
    if (!answer.ok) log('the voice was not kept: ' + (answer.error || ''), 'err');
    else {
      // Keeping a name the room already holds replaces what stood there - the
      // machine says only that the keep went through, so the page says which of
      // its two meanings this was, rather than reading a second voice into a
      // room that did not gain one.
      let stoodThere = false;
      for (const voice of libraryVoices) {
        if (String(voice.name) === name) stoodThere = true;
      }
      if (stoodThere) log('that name already kept a recording: the older one is let go and these bytes take its place', 'ok');
      // A recording that was kept is a speaker this page can clone with right
      // away, however it arrived: chosen, dropped, or pasted.
      clonedFile = file;
      void (async function () { await refresh(); })();
    }
  })();
}

// Whether a file's own bytes are already the plain wav a model listens to, in
// the shape this server reads back: a RIFF wave holding uncompressed 16-bit PCM,
// whatever its channel count. Checked here, and not just for the word RIFF at the
// start, because a file that is close but not that - a riff flac, a wave at
// 8-bit or 24-bit - would otherwise be handed on as it is and refused by the
// model rather than turned over the decoder here.
function plainPcmWavHere(bytes) {
  if (!bytes || bytes.length < 44) return false;
  const tag = function (at) {
    return String.fromCharCode(bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]);
  };
  if (tag(0) !== 'RIFF' || tag(8) !== 'WAVE') return false;
  let format = 0;
  let bits = 0;
  let channels = 0;
  let rate = 0;
  let held = 0;
  for (let at = 12; at + 8 <= bytes.length;) {
    const name = tag(at);
    const len = bytes[at + 4] | (bytes[at + 5] << 8) | (bytes[at + 6] << 16) | (bytes[at + 7] << 24);
    const body = at + 8;
    if (body + len > bytes.length) break;
    if (name === 'fmt ') {
      if (len >= 16) {
        format = bytes[body] | (bytes[body + 1] << 8);
        channels = bytes[body + 2] | (bytes[body + 3] << 8);
        rate = bytes[body + 4] | (bytes[body + 5] << 8) | (bytes[body + 6] << 16) | (bytes[body + 7] << 24);
        bits = bytes[body + 14] | (bytes[body + 15] << 8);
      }
    } else if (name === 'data') {
      held = len;
    }
    at = body + ((len + 1) & ~1);
  }
  if (format !== 1 || bits !== 16 || channels < 1 || rate < 1) return false;
  return held >= channels * 2;
}

// The bytes of a recording a model will listen to: read from the browser, and
// handed on as the plain wav a model expects. A file whose own bytes are already
// that shape travels as it is, when one request can carry them; a longer one is
// turned over the decoder like any other, because the decoder's answer is what
// can be cut to a length that does fit - the door reads one request body at a
// time, so the longest part that fits is kept and the page says which part.
function rawBytesOneRequest() {
  // the largest file whose base64 still fits inside one request body, leaving
  // room for the words that travel around it in that same body.
  const room = ui.bodyBytes - 4096;
  return Math.floor(room / 4) * 3;
}

async function referenceBytes(file) {
  const bytes = await readBytes(file);
  if (!bytes) return null;
  if (plainPcmWavHere(bytes) && bytes.length <= rawBytesOneRequest()) return bytes;
  const decoded = await audioDecode(bytes);
  if (!decoded) return null;
  const whole = Number(decoded.length);
  const cap = Math.floor((rawBytesOneRequest() - 44) / 2);
  if (whole > cap) {
    if (cap < 1) {
      log('one request cannot carry even a moment of this recording', 'err');
      return null;
    }
    const keptSeconds = Math.floor((cap / Number(decoded.sampleRate || 1)) * 10) / 10;
    log('that recording is longer than one request can carry: the first ' +
      keptSeconds + 's of it are what the door keeps', 'warn');
  }
  return await toPlainWav(decoded, cap);
}

// The browser's own decoder. A buffer is never handed to it: a fixed one would
// cut the recording to its own length, or refuse it, and the answer then held
// no frames at all. The decoder sizes its own answer, whose length, rate, and
// channels are read off it.
async function audioDecode(bytes) {
  const data = new Uint8Array(bytes).slice().buffer;
  let context = null;
  try {
    context = new window.AudioContext();
  } catch (refused) {
    browserWords = 'this browser would not open its audio decoder';
    return null;
  }
  if (!context) return null;
  try {
    return await context.decodeAudioData(data);
  } catch (refused) {
    browserWords = refused && refused.message ? String(refused.message) : 'this browser could not read those bytes';
    return null;
  }
}

// A recording of any audio format this browser can read, written out as the
// plain wav a model listens to: 16-bit samples, one channel, at the rate the
// browser decoded it at. A cap of frames keeps the first part of a recording
// that longer than that would not fit one request.
async function toPlainWav(decoded, capFrames) {
  const framesAll = Number(decoded.length);
  const rate = Number(decoded.sampleRate);
  const channels = Number(decoded.numberOfChannels);
  if (!framesAll || !rate || !channels) {
    browserWords = 'those bytes hold no audio frames this browser can read';
    return null;
  }
  let frames = framesAll;
  if (Number(capFrames) > 0 && Number(capFrames) < frames) frames = Math.floor(Number(capFrames));
  if (frames < 1) return null;
  const mono = new Float32Array(frames);
  for (let channel = 0; channel < channels; ++channel) {
    const one = decoded.getChannelData(channel);
    if (!one) continue;
    for (let at = 0; at < frames; ++at) mono[at] += Number(one[at]) / channels;
  }
  const out = new Uint8Array(44 + frames * 2);
  const view = new DataView(out.buffer);
  const write = function (at, text) {
    for (let each = 0; each < text.length; ++each) out[at + each] = text.charCodeAt(each);
  };
  write(0, 'RIFF');
  view.setUint32(4, 36 + frames * 2, true);
  write(8, 'WAVE');
  write(12, 'fmt ');
  view.setUint32(16, 16, true);
  view.setUint32(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, rate, true);
  view.setUint32(28, rate * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  write(36, 'data');
  view.setUint32(40, frames * 2, true);
  for (let at = 0; at < frames; ++at) {
    const level = Math.max(-1, Math.min(1, mono[at]));
    view.setInt16(44 + at * 2, Math.round(level * 32767), true);
  }
  return out;
}

function startMicRecording() {
  if (!window.MediaRecorder || !navigator.mediaDevices) {
    log('this browser cannot record from its microphone', 'err');
    return;
  }
  // The recorder belongs to a stream, not to a made-up thing: the interface
  // records a media stream, and the microphone is what the browser hands over
  // when the page asks for one. A recorder built over nothing cannot start, so
  // the take never reached the page at all.
  void (async function () {
    let stream = null;
    try {
      stream = await navigator.mediaDevices.getUserMedia({ audio: true });
    } catch (refused) {
      log('the microphone was not given to this page: ' + String(refused), 'err');
      return;
    }
    const hold = new MediaRecorder(stream);
    hold.addEventListener('error', function (event) {
      log('the take stopped: ' + String(event.error || 'the browser gave no words'), 'err');
    });
    hold.addEventListener('dataavailable', function (event) {
      keepTheTake(event.data);
    });
    hold.start();
    log('recording from the microphone', '');
    const stopper = ui.fingerAct('stop', function () {
      hold.stop();
      // the microphone itself is let go, so the take ends and the browser stops
      // feeding a stream nobody reads.
      for (const track of stream.getTracks()) track.stop();
    }, 'end the take');
    voiceKeepZone.appendChild(stopper);
  })();
}

// What was recorded is turned into the plain audio a model listens to and kept
// under a name of this page's own, so a take is a speaker like any other.
async function keepTheTake(blob) {
  if (!blob) {
    log('the take held nothing', 'err');
    return;
  }
  void (async function () {
    const wav = await referenceBytes(blob);
    if (!wav) {
      log(refusedWords(), 'err');
      return;
    }
    const keptBytes = base64IfItFits(wav);
    if (!keptBytes) {
      log(tooBigForOneRequest(wav) + ' a take that long can be recorded shorter.', 'err');
      return;
    }
    // the whole clock reading, not part of it: a name that wraps would quietly
    // take over another take's voice.
    const answer = await ui.actDoor('/voice/api/voices/save',
      { name: 'take-' + String(Date.now()), audio_b64: keptBytes });
    if (!answer.ok) log('the voice was not kept: ' + (answer.error || ''), 'err');
    else void (async function () { await refresh(); })();
  })();
}

// A recording released onto the page is taken by this page, not by the browser.
// The doors a released file answers at are the voice room's keep door, the
// cloner's own, and the listener's: each takes a recording, and a file released
// outside these belongs to the browser. The answer is the same order the picture
// studio answers a drag in: dragstart opens the drag and asks for the copy
// effect; dragenter and dragover say the drop will be taken and cancel it, so
// the browser does nothing of its own with the file; the file is named
// wherever an engine hands one over - some name it while the drag moves over
// the door, others only at the drop - and whichever answered is taken exactly
// once. A drag that names formats is text or an element of this page and is
// left alone; a file released outside these doors belongs to the browser.
// The item list of a drag is live, not an array, so it is walked by index.
function keepVoiceDrop(file) {
  if (!file) return false;
  log('a recording came in through the page', 'ok');
  keepFile(file);
  return true;
}

// What a browser handed over when it named a file but gave no file to read:
// the words it said, so the page can say what it got and what to do instead.
function sayNamedOnly(words) {
  if (!words) return;
  log('that drop named a file (' + ui.clip(words, 120) + ') but gave no file to read: choose it with the file field on this page instead', 'err');
}

// The recordings a person dropped, kept here so the door that runs later can ask
// for the same file: declared beside the drop that fills them, so a page read in
// order meets a held file where it is taken rather than after the doors that use
// it. The listener's own and the cloner's are kept apart because one recording
// answers at either door, and which one answered is what runs.
let listenAudioFile = null;
let clonedFile = null;

function attachVoiceDrops() {
  let overZone = null;
  let ownDrag = false;
  let heldFile = null;
  let dropHeard = false;
  let takenFile = null;
  function openDrag() {
    overZone = null;
    heldFile = null;
    dropHeard = false;
    takenFile = null;
  }
  function voiceZone(event) {
    const target = event.target;
    for (const name of ['voice-keep-zone', 'clone-zone', 'listen-zone']) {
      const zone = ui.E(name);
      if (!zone) continue;
      // The node under the pointer is inside the door, or the pointer landed on
      // an ancestor of it, which is what some browsers report instead.
      if (target && (zone === target || zone.contains(target) || target.contains(zone))) return zone;
      // A position over the door is the same target, told without a node. The
      // door's target is the whole panel that names it - the panel whose own
      // words describe that door - not the small box inside it, so a file
      // dropped on the panel's heading or its edge is the same drop, the way
      // the picture studio takes one anywhere over its prompt panel.
      const door = doorPanel(zone);
      const x = event.clientX;
      const y = event.clientY;
      if (typeof x !== 'number' || typeof y !== 'number') continue;
      const box = door.getBoundingClientRect();
      if (x >= box.left && x <= box.right && y >= box.top && y <= box.bottom) return zone;
    }
    return null;
  }
  // The panel a door belongs to: the section that carries its words. A door
  // standing outside any panel is its own box, which is all that can be asked
  // of it.
  function doorPanel(zone) {
    let node = zone.parentNode;
    while (node && node.nodeType === 1) {
      const classes = String(node.getAttribute('class') || '').split(/\s+/);
      for (const one of classes) {
        if (one === 'panel') return node;
      }
      node = node.parentNode;
    }
    return zone;
  }
  function fileItem(event) {
    const transfer = event.dataTransfer;
    if (!transfer || !transfer.items) return null;
    for (let at = 0; at < transfer.items.length; ++at) {
      const item = transfer.items[at];
      if (item && item.kind === 'file') return item;
    }
    return null;
  }
  function nameFile(event) {
    const item = fileItem(event);
    if (item) {
      const file = item.getAsFile();
      if (file) heldFile = file;
      return;
    }
    // Some engines carry no live item list and name the file in the plain one
    // instead; the doors answer either shape, never both at once.
    const transfer = event.dataTransfer;
    if (transfer && transfer.files && transfer.files.length > 0) heldFile = transfer.files[0];
  }

  // A browser that hands over only a name, and not the file itself, cannot have
  // that file read here: no path is invented, and the door says what it got.
  function namedOnly(event) {
    const transfer = event.dataTransfer;
    if (!transfer) return '';
    const words = transfer.types || [];
    if (!words.length) return '';
    if (heldFile) return '';
    const uri = transfer.getData ? transfer.getData('text/uri-list') : null;
    if (uri) return String(uri);
    const raw = transfer.getData ? transfer.getData('Files') : null;
    return raw ? String(raw) : '';
  }

  function takeHeld(zone) {
    if (!heldFile || heldFile === takenFile) return false;
    if (zone && String(zone.id) === 'listen-zone') {
      // The listening door keeps the recording to answer with; it does not keep
      // a voice.
      listenAudioFile = heldFile;
      takenFile = heldFile;
      log('that recording is the one to listen to', 'ok');
      return true;
    }
    if (zone && String(zone.id) === 'clone-zone') {
      // The clone door holds the recording it will listen to, and the room
      // keeps it as any other recording would.
      clonedFile = heldFile;
    }
    if (!keepVoiceDrop(heldFile)) return false;
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
    // A drag that names formats is text or an element of this page; a file from
    // this computer names none. Only that kind is what these doors answer.
    if (transfer.types && transfer.types.length > 0) ownDrag = true;
  });
  document.addEventListener('dragenter', function (event) {
    openDrag();
    if (ownDrag || !voiceZone(event)) return;
    // These doors are willing to take the drop, so they cancel the question -
    // left alone, the browser would open the file and leave the page.
    event.preventDefault();
  });
  document.addEventListener('dragover', function (event) {
    if (ownDrag) return;
    const zone = voiceZone(event);
    overZone = zone;
    if (!zone) return;
    zone.classList.add('offering');
    const transfer = event.dataTransfer;
    if (transfer) transfer.dropEffect = 'copy';
    nameFile(event);
    event.preventDefault();
  });
  document.addEventListener('drop', function (event) {
    if (ownDrag) return;
    const zone = voiceZone(event);
    if (!zone) return;
    dropHeard = true;
    nameFile(event);
    if (takeHeld(zone)) ui.flash(zone);
    else if (!heldFile) sayNamedOnly(namedOnly(event));
    // Whatever these doors took, the browser must not also act on it.
    event.preventDefault();
  });
  document.addEventListener('dragend', function (event) {
    for (const name of ['voice-keep-zone', 'clone-zone', 'listen-zone']) {
      const zone = ui.E(name);
      if (zone) zone.classList.remove('offering');
    }
    if (!ownDrag) {
      // An engine that never sends a drop for this drag still named the file
      // while the drag moved over the door, so the file is taken from here.
      if (!dropHeard && overZone && takeHeld(overZone)) ui.flash(overZone);
      else if (!dropHeard && !heldFile) sayNamedOnly(namedOnly(event));
    }
    openDrag();
    ownDrag = false;
  });
  document.addEventListener('paste', function (event) {
    const clip = event.clipboardData;
    if (!clip || !clip.items) return;
    // The clipboard list is live as well: it is walked by index.
    for (let at = 0; at < clip.items.length; ++at) {
      const item = clip.items[at];
      if (!item || item.kind !== 'file') continue;
      const file = item.getAsFile();
      if (!file) continue;
      keepFile(file);
      log('a recording came in through the clipboard', 'ok');
      break;
    }
  });
}

function attachVoiceDoors() {
  attachVoiceDrops();
  keepFileDoor('voice-file-input', 'voice-file-form', function (files) {
    keepFile(files[0]);
  });
  const micBtn = ui.fingerAct('record with the microphone', function () {
    startMicRecording();
  }, 'a short clear take works best');
  if (voiceKeepZone) voiceKeepZone.appendChild(micBtn);
}

// ---------- renders ----------

// One row of a render. The same drawing answers a render this page has just
// finished and one the machine hands back from the library, so a render that was
// just made and one that was kept are read the same way, and the hands on one
// work on the other.
function renderRow(item) {
  const shell = ui.cardShell();
  const head = document.createElement('div');
  head.className = 'model-name';
  head.textContent = String(item.id);
  shell.main.appendChild(head);
  const words = document.createElement('div');
  words.className = 'model-shape';
  let wordsText = 'model: ' + nameOf(String(item.model || ''));
  if (item.voice) wordsText += ' · voice: ' + String(item.voice);
  if (Number(item.audio_s || 0) > 0) wordsText += ' · ' + Math.round(Number(item.audio_s) * 10) / 10 + 's';
  // The words a render spoke are on the record and on the library row, and a
  // room of sounds that reads only "2.8s" cannot tell apart two renders of the
  // same length; naming what one said is what makes the room findable.
  if (String(item.said_words || '')) wordsText += ' · it said: ' + ui.clip(String(item.said_words), 80);
  if (Array.isArray(item.word_times) && item.word_times.length) wordsText += ' · with its words';
  // A fresh render's own row is the one that can say how long the work took;
  // a long render is otherwise read as the page having gone quiet.
  if (Number(item.made_ms || 0) > 0) wordsText += ' · made in ' + (Math.round(Number(item.made_ms) / 100) / 10) + 's';
  words.textContent = wordsText;
  shell.main.appendChild(words);
  const play = ui.fingerAct('play', function () {
    void (async function () {
      const node = document.createElement('audio');
      node.controls = true;
      node.src = await playUrl(String(item.wav || ''));
      shell.main.appendChild(node);
    })();
  }, 'hear this render');
  shell.fingers.appendChild(play);
  const drop = ui.fingerAct('drop', function () {
    void (async function () {
      const answer = await ui.actDoor('/voice/api/library/delete', { id: String(item.id) });
      if (!answer.ok) log('the render was not dropped: ' + (answer.error || ''), 'err');
      else void (async function () { await refresh(); })();
    })();
  }, 'forget this render');
  shell.fingers.appendChild(drop);
  return shell.card;
}

function drawRenders(items) {
  lastRenderList = Array.isArray(items) ? items : [];
  if (!rendersPane) return;
  rendersPane.textContent = '';
  for (const item of lastRenderList) rendersPane.appendChild(renderRow(item));
}

// ---------- refresh ----------

// Whether the page already named that nothing stands loaded: the same absence is
// one state, so it is named once when it newly happens and not again on every read.
let nothingNamedLoaded = false;

async function refresh() {
  // The header is one machine reading, drawn before the rooms ask: the bar names
  // what runs and the chips state what the machine holds, exactly as the other
  // pages read them. The engines count every job this page starts, so their own
  // words name a running voice here as well.
  const machine = await ui.api('/api/monitor');
  if (machine) {
    ui.monitorChrome(machine);
    ui.headerState(Array.isArray(machine.jobs) ? machine.jobs : []);
  }
  engineStatus = await ui.api('/voice/api/status');
  // A voice engine that gives no answer is one room with nothing to draw, not a
  // page that lost the machine: the header, the cards, and the library were read
  // from their own answers, so this room names its own absence and the
  // reading continues rather than ending here - which is what stopped the header
  // from ever naming what the machine was running.
  if (!engineStatus) log('the voice engine gave no answer: its rooms stay as they were last read', 'err');
  // The counts and the words a filter may use, then one page of the cards,
  // then what stands loaded whatever the cards are filtered to: three answers
  // that are each about what the page draws, instead of one about the whole
  // set of packages.
  await askHouse();
  const rows = await askCards(pageAt);
  // a card page that did not answer keeps the cards the last good one drew; the
  // page already said it could not reach the machine when nothing answers at all.
  if (rows && Array.isArray(rows)) drawModels(rows);
  const found = await askUsable();
  const models = found || usableModels;
  // every engine being closed is a state the machine is really in, not a page that
  // lost the server: the cards and the library still say what they hold, and the
  // room where a voice is asked for says what is missing.
  if (found && !models.length) {
    if (!nothingNamedLoaded)
      log('Nothing stands loaded to answer with: open a model and its name appears in the picker.', 'warn');
    nothingNamedLoaded = true;
  } else {
    nothingNamedLoaded = false;
  }
  keepChosenOrClear(models);
  libraryVoices = [];
  chosenPresets = [];
  let probeModel = String(chosenModel);
  if (!probeModel) {
    for (const item of models) {
      if (item.weights_present) {
        probeModel = String(item.id);
        break;
      }
    }
  }
  const voices = probeModel ? await ui.api('/voice/api/voices/' + probeModel) : null;
  if (voices && Array.isArray(voices.library)) {
    for (const voice of voices.library) libraryVoices.push(voice);
    if (String(chosenModel) === probeModel && Array.isArray(voices.presets)) {
      for (const preset of voices.presets) chosenPresets.push(preset);
    }
  }
  drawVoiceLibrary(libraryVoices);
  drawSayZone(models);
  const renders = await ui.api('/voice/api/library');
  if (renders && Array.isArray(renders)) drawRenders(renders);
  drawStudioPanels(models);
  // the shared library states a moment one way, so the page does not state one
  // in another format.
  if (updated) updated.textContent = ui.clockMs(Date.now());
}

function keepChosenOrClear(models) {
  if (!chosenModel) return;
  for (const item of models) {
    if (String(item.id) === String(chosenModel) && item.loaded) return;
  }
  chosenModel = '';
}

// A machine that stops answering is said once; the loop keeps trying quietly
// and says so only when the answer comes back.
let offlineSaid = false;
function offline(err) {
  ui.headUnknown();
  if (updated) updated.textContent = 'not connected · retrying';
  if (offlineSaid) return;
  offlineSaid = true;
  const served = ui.apiStatus === 401 || ui.apiStatus === 403;
  log(served
    ? 'a server stands here and refused this page: ' + String(ui.apiError || '')
    : 'the machine did not answer (' + String(err.message || err) + '): the page keeps what it last read and asks again', 'err');
}

async function main() {
  const keptStatus = ui.lastReading('/voice/api/status');
  if (keptStatus && keptStatus.data) {
    engineStatus = keptStatus.data;
  }
  const keptHouse = ui.lastReading('/voice/api/model-stats');
  if (keptHouse && keptHouse.data) {
    house = keptHouse.data;
    houseAge = Date.now() - 60000;
    drawHouseStats();
    drawFilterChoices();
  }
  // The cards this browser last had, drawn exactly as a live page would draw
  // them, so the room reads as it did while the machine is being read again.
  const keptCards = ui.lastReading('/voice/api/model-cards?offset=0&limit=' + PAGE_CARDS);
  if (keptCards && keptCards.data && Array.isArray(keptCards.data.rows)) {
    lastAnswer = keptCards.data;
    pageAt = Number(keptCards.data.offset || 0);
    drawModels(keptCards.data.rows);
  }
  const keptRenders = ui.lastReading('/voice/api/library');
  if (keptRenders && keptRenders.data && Array.isArray(keptRenders.data)) drawRenders(keptRenders.data);
  attachVoiceDoors();
  attachStudioDoors();
  await ui.poll(async function () {
    const answered = await refresh();
    if (offlineSaid) {
      log('The machine answers again.', 'ok');
      offlineSaid = false;
    }
    return answered;
  }, offline, ui.pollMs);
}

// ---------- the studio doors: clone, create, edit, play ----------
// Every one of these asks an engine that is standing right now; the words a
// panel offers come from what that engine's own card declared, so a door never
// offers a model that cannot do the work it names. Answers ride the same job
// records as everything else on the page, and the progress panel draws them.

function hasOption(item, name) {
  if (!item || !Array.isArray(item.params)) return false;
  for (const param of item.params) {
    if (String(param.name) === name) return true;
  }
  return false;
}

// A card states a task in one of two shapes: the engine's own token, in the
// words it accepts for that task, or the name a model's spec carries for it.
// Both are here once, in the rows the engine keeps beside its vocabulary, so a
// hand can ask about a task however the card wrote it - a wake-word model names
// itself wake_word on its capabilities and wake in the engine's words, and a
// hand that read only one of the two never offers that model.
function taskWords(token) {
  const rows = {
    vad: ['vad'], asr: ['asr'], diar: ['diar'], sep: ['sep'],
    gen: ['gen', 'music', 'sfx', 'edit', 'audio_generation'],
    tts: ['tts'], clon: ['clon', 'clone'], vc: ['vc'], s2s: ['s2s'],
    align: ['align'], vdes: ['vdes', 'design'], spk: ['spk', 'speaker'],
    svc: ['svc'], midi: ['midi'], turn: ['turn'],
    wake: ['wake', 'wake_word'], cls: ['cls', 'classify'],
  };
  return rows[token] || [token];
}

// Whether a model names that task at all, in either shape.
function namesTask(item, token) {
  if (!item) return false;
  const stated = [];
  if (Array.isArray(item.declared_tasks)) stated.push(item.declared_tasks);
  if (Array.isArray(item.can)) stated.push(item.can);
  for (const word of taskWords(token)) {
    for (const list of stated) {
      for (const each of list) {
        if (String(each).toLowerCase() === word) return true;
      }
    }
  }
  return false;
}

// The door this page asks that kind of work at. The server hands the engine the
// kind of work when it opens it - the speaking door for a voice a model makes,
// designs, or converts from one in hand; the listening door for a voice it answers
// about - so every kind of work the engine names has a door here, and the door a
// card states is the one a person presses. A word no door here is named for
// stays empty rather than being placed at one.
function doorFor(word) {
  const low = String(word).toLowerCase();
  const doors = [
    ['speak', ['tts', 'clon', 'vdes', 'gen']],
    ['listen', ['asr', 'diar', 'align', 'vad', 'spk', 'cls', 'wake', 'turn',
      'sep', 'vc', 's2s', 'svc', 'midi']],
  ];
  for (const door of doors) {
    for (const token of door[1]) {
      for (const name of taskWords(token)) {
        if (low === name) return door[0];
      }
    }
  }
  return '';
}

// One engine word can name several of a package's names - the engine keeps those
// names together on purpose, one kind of work answering in one word - so words a
// card prints are printed once each, in the order the model states them.
function uniqueWords(words) {
  const shown = [];
  for (const word of (Array.isArray(words) ? words : [])) {
    const low = String(word).toLowerCase();
    if (shown.indexOf(low) < 0) shown.push(low);
  }
  return shown;
}

// Clone: a model whose own handle says it listens to a speaker reference.
function canClone(item) {
  return standsLoaded(item) && item.speaker_reference === true;
}
// Create: a model that takes the words describing a voice as its own
// instruction - the name of that option is on the card, not kept here.
function canCreate(item) {
  return standsLoaded(item) && (hasOption(item, 'instruct') || namesTask(item, 'vdes'));
}

function fillSelect(id, rows, labelOf) {
  const select = ui.E(id);
  if (!select) return;
  select.textContent = '';
  for (const item of rows) {
    const option = document.createElement('option');
    option.value = String(item.id);
    option.textContent = labelOf(item);
    select.appendChild(option);
  }
}

function fillVoiceSelect(id) {
  const select = ui.E(id);
  if (!select) return;
  select.textContent = '';
  for (const voice of libraryVoices) {
    const option = document.createElement('option');
    option.value = String(voice.name);
    option.textContent = String(voice.name);
    select.appendChild(option);
  }
}

// one answer from an engine, followed on the page: the bar says how long it
// has taken, as the progress panel's own title states. the finished record is
// what the caller draws; nothing is guessed while it runs.
async function followJob(job, words) {
  const startedAt = Date.now();
  while (true) {
    const state = await ui.api('/voice/api/jobs/' + job);
    paintHeader(job, state);
    if (!state) {
      log('that answer stopped answering; the record stays where it is', 'err');
      return null;
    }
    const elapsed = Math.floor((Date.now() - startedAt) / 1000);
    ui.progressSet(progressPanel, job, 0, 1, words + ' · ' + elapsed + 's', 'load');
    const stateName = String(state.state);
    if (stateName === 'running') {
      await ui.sleepWindow('/voice/api/jobs/' + job, 400, 5000);
      continue;
    }
    ui.progressClear(progressPanel, job);
    if (stateName === 'done') return state;
    if (stateName === 'cancelled') log('stopped: ' + words, 'stop');
    // the ask was turned down, which is not the same as an engine that ran and
    // said no: the door's own words are repeated under the name of what was
    // asked, so a reader learns which of the two happened.
    else {
      log(words + ' was refused: ' + ui.askWhy(state.error), 'err');
      ui.toastNo(words, state.error);
    }
    return null;
  }
}

// ---------- the listening door: a recording comes in, the model answers ----

let listenPane = null;

// what the listening door can be asked of a model: every task the engine names
// that starts from a recording. The words are the engine's own task tokens, so
// a family that listens is offered here by what its card states, not by a list
// kept on the page.
const listeningTasks = ['asr', 'diar', 'vad', 'sep', 'vc', 's2s', 'svc',
  'wake', 'turn', 'cls', 'spk', 'align'];
function canListen(item) {
  if (!standsLoaded(item)) return false;
  for (const token of listeningTasks) {
    if (namesTask(item, token)) return true;
  }
  return false;
}
// A model whose engine is in this process: the doors below answer with what that
// engine holds, so a card whose files only sit on disk is not offered where an
// answer is asked for.
function standsLoaded(item) {
  return !!item && item.loaded;
}

async function listenNow() {
  const wanted = String(ui.selectValue('listen-model') || '');
  const item = modelById(wanted);
  if (!standsLoaded(item)) {
    log('that model holds no memory: press its load hand first', 'err');
    return;
  }
  if (!canListen(item)) {
    log('pick a model whose card says it listens to a recording', 'err');
    return;
  }
  const body = { model: wanted };
  // The dials of this door belong to the model just chosen, so they are drawn
  // before anything is asked; a person who moved one after picking the model
  // finds the same fields again.
  drawOptionsFor('listen', wanted);
  const kept = String(ui.selectValue('listen-voice') || '');
  if (kept) {
    body.voice = kept;
  } else if (listenAudioFile) {
    const bytes = await referenceBytes(listenAudioFile);
    if (!bytes) {
      log(refusedWords(), 'err');
      return;
    }
    body.audio_b64 = base64IfItFits(bytes);
    if (!body.audio_b64) {
      log(tooBigForOneRequest(bytes), 'err');
      return;
    }
  } else {
    log('drop a recording here, or pick one already kept in the voice room', 'err');
    return;
  }
  // What this model declared is sent as it was moved: run controls under the
  // door's own name and the settings of the standing model beside them, so the
  // session that answers runs with the settings a person chose.
  const listenRun = collectRows(optionDoors.listen.runRows);
  const listenStand = collectStanding('listen');
  const listenRunKeys = Object.keys(listenRun);
  if (listenRunKeys.length) {
    body.options = {};
    for (const key of listenRunKeys) body.options[key] = listenRun[key];
  }
  const listenStandKeys = Object.keys(listenStand);
  if (listenStandKeys.length) {
    body.session_options = {};
    for (const key of listenStandKeys) body.session_options[key] = listenStand[key];
  }
  const started = await ui.postJob('/voice/api/listen', body);
  if (!started.job) {
    log('the listen was refused: ' + ui.askWhy(started), 'err');
    ui.toastNo('start that listen', started);
    return;
  }
  log('listening with ' + nameOf(wanted), '');
  void (async function () {
    const record = await followJob(started.job, 'listening');
    if (record) drawListenResult(record, item);
  })();
}

// What the model answered is drawn as the model stated it: the words it heard,
// the segments with where they sit in the recording, the speaker turns with the
// speaker it named, and every audio stream it produced - a converted voice, a
// separated source, a cleaned track - each kept as its own file and playable
// here. Nothing is guessed: a model that states nothing leaves nothing here, and
// the page says so.
function drawListenResult(record, item) {
  if (!listenPane) return;
  const heard = String(record.words || '');
  const segments = Array.isArray(record.segments) ? record.segments : [];
  const turns = Array.isArray(record.speaker_turns) ? record.speaker_turns : [];
  const streams = Array.isArray(record.streams) ? record.streams : [];
  const artifacts = Array.isArray(record.artifacts) ? record.artifacts : [];
  const shell = ui.cardShell();
  const name = document.createElement('div');
  name.className = 'model-name';
  name.textContent = nameOf(String(record.model || (item && item.id)));
  shell.main.appendChild(name);
  const brief = document.createElement('div');
  brief.className = 'model-note';
  const taken = answerTakenMs(record);
  brief.textContent = taken > 0 ? 'answered in ' + (Math.round(taken / 100) / 10) + 's'
    : 'its time is not stated';
  shell.main.appendChild(brief);

  const fingers = shell.fingers;
  if (heard) {
    fingers.appendChild(ui.finger('heard ' + heard.length + ' characters', 'ok'));
  }
  if (String(record.language || '')) {
    // A model states the language it heard in the code it names it by, the same
    // way its card names the languages it speaks; repeating it here is what lets
    // a reader see that another model, or another ask, is the one worth trying.
    fingers.appendChild(ui.finger('heard in ' + String(record.language), 'cap',
      'The language that model states it heard.'));
  }
  if (segments.length) {
    fingers.appendChild(ui.finger('found ' + segments.length + ' segments', 'ok'));
  }
  if (turns.length) {
    fingers.appendChild(ui.finger('found ' + turns.length + ' speaker turns', 'ok'));
  }
  if (streams.length) {
    fingers.appendChild(ui.finger('made ' + streams.length + ' audio streams', 'ok'));
  }
  if (artifacts.length) {
    fingers.appendChild(ui.finger('left ' + artifacts.length + ' artifact files', 'ok'));
  }
  if (!heard && !segments.length && !turns.length && !streams.length && !artifacts.length) {
    fingers.appendChild(ui.finger('that model stated nothing this page can show', 'err'));
  }

  if (heard) {
    const line = document.createElement('div');
    line.className = 'lib-note';
    line.textContent = heard;
    shell.main.appendChild(line);
  }
  for (const segment of segments) {
    const line = document.createElement('div');
    line.className = 'lib-row';
    line.textContent = 'at ' + Number(segment.start_s).toFixed(2) + 's to ' +
      Number(segment.end_s).toFixed(2) + 's - ' + String(segment.words || '');
    shell.main.appendChild(line);
  }
  for (const turn of turns) {
    const line = document.createElement('div');
    line.className = 'lib-row';
    line.textContent = 'speaker ' + String(turn.speaker || '?') + ' at ' +
      Number(turn.start_s).toFixed(2) + 's to ' + Number(turn.end_s).toFixed(2) + 's - ' +
      String(turn.words || '');
    shell.main.appendChild(line);
  }
  for (const stream of streams) {
    const url = String(stream.url || '');
    if (!url) continue;
    const audio = document.createElement('audio');
    audio.controls = true;
    audio.title = 'stream named ' + String(stream.name || '') + ' by the model';
    void (async function () { audio.src = await playUrl(url); })();
    shell.main.appendChild(audio);
  }
  for (const artifact of artifacts) {
    const url = String(artifact.url || '');
    if (!url) continue;
    const line = document.createElement('div');
    line.className = 'lib-row';
    line.textContent = 'left a ' + String(artifact.kind || 'file') + ' of ' +
      ui.sizeText(Number(artifact.bytes || 0)) + ' - ' + url;
    shell.main.appendChild(line);
    const link = document.createElement('a');
    link.className = 'finger';
    link.textContent = 'keep that file';
    link.title = 'the engine wrote these bytes for ' + String(artifact.kind || 'a result');
    void (async function () {
      link.href = await playUrl(url);
      link.download = String(artifact.name || 'artifact');
    })();
    shell.fingers.appendChild(link);
  }
  addNewestRow(listenPane, shell.card, 'listening results');
}

// ---------- voice clone ----------

async function cloneNow() {
  const model = String(ui.selectValue('clone-model') || '');
  const item = modelById(model);
  if (!standsLoaded(item)) {
    log('that model holds no memory: press its load hand first', 'err');
    return;
  }
  if (!canClone(item)) {
    log('pick a model whose card says it listens to a recording', 'err');
    return;
  }
  const text = String(ui.textField('clone-say') || '');
  if (!text.trim()) {
    log('nothing for the cloned voice to say', 'err');
    return;
  }
  const body = { model: model, text: text };
  const savedVoice = String(ui.selectValue('clone-voice') || '');
  const heldFile = clonedFile;
  if (savedVoice) {
    body.voice = savedVoice;
  } else if (heldFile) {
    const bytes = await referenceBytes(heldFile);
    if (!bytes) {
      log(refusedWords(), 'err');
      return;
    }
    body.reference_b64 = base64IfItFits(bytes);
    if (!body.reference_b64) {
      log(tooBigForOneRequest(bytes), 'err');
      return;
    }
  } else {
    log('pick a kept voice, or drop a recording anywhere on this panel', 'err');
    return;
  }
  const said = String(ui.textField('clone-words') || '');
  if (said.trim()) body.reference_text = said.trim();
  const started = await ui.postJob('/voice/api/speak', body);
  if (!started.job) {
    log('the clone was refused: ' + ui.askWhy(started), 'err');
    ui.toastNo('start that clone', started);
    return;
  }
  log('cloning a voice with ' + nameOf(model), '');
  void (async function () {
    const record = await followJob(started.job, 'cloning');
    if (record) drawRenderResult(record);
  })();
}

// ---------- voice creator ----------

async function createNow() {
  const model = String(ui.selectValue('create-model') || '');
  const item = modelById(model);
  if (!standsLoaded(item)) {
    log('that model holds no memory: press its load hand first', 'err');
    return;
  }
  if (!canCreate(item)) {
    log('pick a model whose card says it designs a voice from words', 'err');
    return;
  }
  const describe = String(ui.textField('create-words') || '');
  const text = String(ui.textField('create-say') || '');
  if (!describe.trim()) {
    log('nothing describing the voice', 'err');
    return;
  }
  if (!text.trim()) {
    log('nothing for the created voice to say', 'err');
    return;
  }
  const body = { model: model, text: text, options: { instruct: describe.trim() } };
  const started = await ui.postJob('/voice/api/speak', body);
  if (!started.job) {
    log('the creation was refused: ' + ui.askWhy(started), 'err');
    ui.toastNo('create that voice', started);
    return;
  }
  log('creating a voice with ' + nameOf(model), '');
  void (async function () {
    const record = await followJob(started.job, 'creating a voice');
    if (!record) return;
    drawRenderResult(record);
    const keepAs = String(ui.textField('create-name') || '');
    const safe = safeVoiceName(keepAs);
    if (!safe) return;
    const url = String(record.url || '');
    if (!url) return;
    const bytes = await fetchBytes(url);
    if (!bytes) {
      log('the created voice could not be read back to keep it', 'err');
      return;
    }
    const keptBytes = base64IfItFits(bytes);
    if (!keptBytes) {
      log('the voice that was just made is too big to keep in one request: ' +
        tooBigForOneRequest(bytes) + ' ask this model for a shorter line.', 'err');
      return;
    }
    const kept = await ui.actDoor('/voice/api/voices/save',
      { name: safe, audio_b64: keptBytes, prompt_text: describe.trim() });
    if (!kept.ok) log('the created voice was not kept: ' + (kept.error || ''), 'err');
    else void (async function () { await refresh(); })();
  })();
}

// ---------- voice editor ----------

async function editNow() {
  const model = String(ui.selectValue('edit-for') || '');
  const item = modelById(model);
  if (!standsLoaded(item)) {
    log('pick the model that speaks this voice again', 'err');
    return;
  }
  const voice = String(ui.selectValue('edit-voice') || '');
  const text = String(ui.textField('edit-say') || '');
  if (!voice) {
    log('no voice picked to say again', 'err');
    return;
  }
  if (!text.trim()) {
    log('nothing to say again', 'err');
    return;
  }
  const body = { model: model, text: text, voice: voice };
  const options = {};
  const instruct = String(ui.textField('edit-instruct') || '');
  const language = String(ui.textField('edit-language') || '');
  const speed = String(ui.textField('edit-speed') || '');
  if (instruct.trim() && hasOption(item, 'instruct')) options.instruct = instruct.trim();
  if (language.trim() && hasOption(item, 'language')) options.language = language.trim();
  if (speed.trim() && hasOption(item, 'speed')) options.speed = speed.trim();
  if (!Object.keys(options).length) {
    log('no direction was changed: the words say the voice as it stands', 'stop');
  }
  if (Object.keys(options).length) body.options = options;
  const started = await ui.postJob('/voice/api/speak', body);
  if (!started.job) {
    log('the edit was refused: ' + ui.askWhy(started), 'err');
    ui.toastNo('change that voice', started);
    return;
  }
  log('saying ' + voice + ' again with the directions changed', '');
  void (async function () {
    const record = await followJob(started.job, 'editing');
    if (record) drawRenderResult(record);
  })();
}

// ---------- play it: the audio, its words, and the sound drawn ----------

let playItem = null;

// One bar per slice of the sound, whatever its length: the bars are the shape of
// the render, and the drawing spaces them across whatever box the page shows.
const eqBars = 60;

function drawPlayChoices(items) {
  const select = ui.E('play-render');
  if (!select) return;
  select.textContent = '';
  for (const item of items) {
    const option = document.createElement('option');
    option.value = String(item.id);
    option.textContent = String(item.id);
    select.appendChild(option);
  }
}

async function fetchBytes(path) {
  // the file this page just played: the browser already holds a copy through
  // the shared store, so this read asks the network only the first time.
  const src = await playUrl(path);
  const res = await fetch(src);
  if (!res.ok) return null;
  const raw = await res.arrayBuffer();
  return new Uint8Array(raw);
}

function peaksFromBytes(bytes, buckets) {
  // the loudest amplitude of each slice of the audio, straight off the wav the
  // server kept: a plain 16-bit little-endian wav is what those files are.
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (String.fromCharCode(dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3)) !== 'RIFF') return [];
  if (String.fromCharCode(dv.getUint8(8), dv.getUint8(9), dv.getUint8(10), dv.getUint8(11)) !== 'WAVE') return [];
  let at = 12;
  let bits = 0;
  let rate = 0;
  let channels = 1;
  let dataAt = -1;
  let dataLen = 0;
  while (at + 8 <= bytes.length) {
    const tag = String.fromCharCode(dv.getUint8(at), dv.getUint8(at + 1), dv.getUint8(at + 2), dv.getUint8(at + 3));
    const size = dv.getUint32(at + 4, true);
    if (tag === 'fmt ') {
      channels = dv.getUint16(at + 10, true) || 1;
      rate = dv.getUint32(at + 12, true);
      bits = dv.getUint16(at + 22, true);
    } else if (tag === 'data') {
      dataAt = at + 8;
      dataLen = Math.min(size, bytes.length - at - 8);
    }
    at += 8 + size + (size & 1);
  }
  if (dataAt < 0 || bits !== 16 || rate <= 0 || dataLen <= 0)
    return [];
  const totalFrames = Math.floor(dataLen / (2 * channels));
  const peaks = [];
  for (let bucket = 0; bucket < buckets; ++bucket) {
    let loudest = 0;
    // the window this bar stands for, taken from the whole sound: every frame
    // belongs to one bar, so a short render still draws its own shape rather
    // than losing the last bars.
    const from = Math.floor((totalFrames * bucket) / buckets);
    const to = Math.floor((totalFrames * (bucket + 1)) / buckets);
    for (let f = from; f < to; ++f) {
      const at2 = dataAt + (f * channels) * 2;
      if (at2 + 1 >= bytes.length)
        break;
      const value = dv.getInt16(at2, true);
      const abs = value < 0 ? -value : value;
      if (abs > loudest)
        loudest = abs;
    }
    peaks.push(loudest / 32767);
  }
  return peaks;
}

async function playNow() {
  if (!playItem) {
    log('no render picked to play', 'err');
    return;
  }
  const item = playItem;
  const url = String(item.wav || '');
  if (!url) {
    // a render the page was asked to play that names no file answers with words
    // rather than leaving a person in front of a silent box.
    log('that render names no file the machine can play: ' + String(item.id), 'err');
    return;
  }
  const zone = ui.E('play-zone');
  const canvas = ui.E('eq-canvas');
  const track = ui.E('word-track');
  // The picker, the drawing, and the word line live in this box and are meant
  // to stay: only a previous player is taken out of it, so the second play
  // still has somewhere to draw and something to pick from.
  if (track) track.textContent = '';
  if (zone) {
    for (const left of Array.from(zone.children)) {
      if (String(left.tagName).toLowerCase() === 'audio') zone.removeChild(left);
    }
  }
  // the drawing is sized to the box the page draws on, at that screen's own
  // sharpness, on each frame - see runPlayback.
  const audio = document.createElement('audio');
  audio.controls = true;
  audio.src = await playUrl(url);
  if (zone) zone.appendChild(audio);
  const words = Array.isArray(item.word_times) ? item.word_times : [];
  const cells = [];
  if (track && words.length) {
    // the words this render was told to say come with the render now, so where
    // they line up one to one with the positions the model stated, the line's
    // own words are shown - that is what a person reads along. where a family
    // states its groups in its own alphabet, or kept no words, those groups
    // come as the model stated them.
    const askedWords = String(item.said_words || '').split(/\s+/).filter(function (w) { return w.length; });
    const byOwnWords = askedWords.length === words.length;
    let wordAt = 0;
    for (const word of words) {
      const cell = document.createElement('span');
      cell.className = 'word-cell';
      const label = byOwnWords ? String(askedWords[wordAt]) : String(word.word);
      wordAt += 1;
      cell.textContent = label;
      // A word the model gave no position for is kept out of the tracking rather
      // than lit at the start of the line: its place on the audio is not a fact
      // the machine stated.
      const startStated = Number(word.start_s);
      const endStated = Number(word.end_s);
      const stated = Number.isFinite(startStated) && Number.isFinite(endStated) && endStated > startStated;
      cell.title = stated
        ? 'from ' + String(Math.round(startStated * 100) / 100) + 's to ' +
        String(Math.round(endStated * 100) / 100) + 's'
        : 'this word carries no position in the audio';
      track.appendChild(cell);
      if (stated) cells.push({ node: cell, start: startStated, end: endStated });
    }
  }
  // The words and their places come with the record, so the tracking is drawn
  // whatever the sound file did. The bars need the file itself: a file the
  // browser would not hand over leaves the bars undrawn and says so, instead of
  // losing the words as well.
  let bytes = null;
  try {
    bytes = await fetchBytes(url);
  } catch (notHandedOver) {
    bytes = null;
  }
  if (bytes === null) log('the file that render names did not come back: the drawing keeps to the words', 'warn');
  const peaks = bytes ? peaksFromBytes(bytes, eqBars) : [];
  if (bytes && peaks.length === 0) log('the sound that came back is not one this page can draw bars for', 'warn');
  log('playing ' + String(item.id) + (cells.length ? ' with its stated groups lit' : ' - that model states no positions'), '');
  audio.addEventListener('play', function () {
    void (async function () {
      await runPlayback(audio, peaks, cells);
    })();
  });
  audio.play();
}

// The colours a canvas is painted in, read off the page's own colour token: a
// drawing cannot use a token by itself, so the token is turned into the rgba the
// drawing needs. The named fallback is that same colour written out, for a
// browser that cannot hand the token over.
function barPaintOf(token) {
  const raw = String(getComputedStyle(document.documentElement).getPropertyValue(token) || '').trim();
  let hex = raw;
  if (hex.length === 4) {
    hex = '#' + hex[1] + hex[1] + hex[2] + hex[2] + hex[3] + hex[3];
  }
  const reached = String(hex.length === 7 && hex[0] === '#'
    ? parseInt(hex.slice(1, 3), 16) + ',' + parseInt(hex.slice(3, 5), 16) + ',' + parseInt(hex.slice(5, 7), 16)
    : '255,143,177');
  return { reached: 'rgba(' + reached + ',0.85)', waiting: 'rgba(' + reached + ',0.14)' };
}

async function runPlayback(audio, peaks, cells) {
  const canvas = ui.E('eq-canvas');
  const draw = canvas && canvas.getContext ? canvas.getContext('2d') : null;
  const seconds = () => Number(audio.currentTime || 0);
  const total = Math.max(0.001, Number(audio.duration || 0));
  // The drawing is asked for a few times a second: the bars and the words follow
  // the clock, and a frame every millisecond would buy nothing the clock cannot
  // show.
  const frameMs = 60;
  // The size of the drawing the bitmap was last made for, so the bitmap is only
  // remade when that size is no longer the one the page shows.
  let boxWide = 0;
  let boxTall = 0;
  let boxSharp = 0;
  // The element's own words for the end of playback: paused again - which is also
  // what a person does - or finished playing. The element is never asked whether
  // it is complete: that flag says its bytes were all read, which is true long
  // before the sound has finished, and a drawing that stopped on it stopped with
  // the first frame.
  // the bars wear the tint this page's voice rows carry, read off the token here
  const barColor = barPaintOf('--accent2');
  if (draw) {
    draw.clearRect(0, 0, Number(canvas.width), Number(canvas.height));
  }
  const paintWords = function (clock) {
    for (const cell of cells) {
      // The clock is only asked for a few times a second, so a word cannot be
      // tracked by whether a sample lands inside its own span - a short word
      // would go unlit, and a word already past would lose its mark. The audio
      // has reached a word when its clock passes where the word starts, so the
      // words said keep their mark and the one being said now wears the full
      // one: the line fills along with the sound.
      if (clock >= cell.end) cell.node.className = 'word-cell word-said';
      else if (clock >= cell.start) cell.node.className = 'word-cell word-on';
      else cell.node.className = 'word-cell';
    }
  };
  let at = 0;
  while (!audio.paused && !audio.ended) {
    at = seconds();
    // The playback is over once the clock reaches its length, however the
    // element reports it: this loop holds a timer, so it lets go here rather
    // than trusting the element's own end flags.
    if (at >= total) break;
    if (draw) {
      // The bars are drawn in the box the page shows on screen, not in a fixed
      // drawing width: a page box and a drawing width are different facts, and
      // a fixed one stretches or blurs the bars against the sheet's own size.
      // The screen's sharpness is taken at the same moment, so the drawing
      // stays sharp where the screen asks for more pixels.
      const wide = Math.round(Number(canvas.clientWidth || 0));
      const tall = Math.round(Number(canvas.clientHeight || 0));
      const sharp = Math.max(1, Number(window.devicePixelRatio || 1));
      if (wide > 0 && tall > 0) {
        // The bitmap is only made again when the box it stands in, or the
        // screen's sharpness, actually changed: a bitmap remade on every frame
        // throws away the pixels of the frame that was just drawn, and buys
        // nothing the clock could show.
        if (wide !== boxWide || tall !== boxTall || sharp !== boxSharp) {
          canvas.width = Math.round(wide * sharp);
          canvas.height = Math.round(tall * sharp);
          // the drawing state stands across a bitmap resize, so the sharpness is
          // set absolutely: multiplying it in would build on whatever a previous
          // size had already set, and the bars of a resized box would wear a
          // sharpness twice over.
          draw.setTransform(sharp, 0, 0, sharp, 0, 0);
          boxWide = wide;
          boxTall = tall;
          boxSharp = sharp;
        }
        draw.clearRect(0, 0, wide, tall);
        if (peaks.length) {
          const bar = Math.max(2, Math.floor(wide / peaks.length) - 2);
          const played = at / total;
          for (let at2 = 0; at2 < peaks.length; ++at2) {
            const x = Math.floor(at2 * (wide / peaks.length));
            const height = Math.max(1, Math.round(Math.min(1, peaks[at2]) * (tall - 6)));
            draw.fillStyle = at2 / peaks.length <= played ? barColor.reached : barColor.waiting;
            draw.fillRect(x, tall - height - 3, bar, height);
          }
        }
      } else {
        draw.clearRect(0, 0, Number(canvas.width), Number(canvas.height));
      }
    }
    paintWords(at);
    await ui.sleep(frameMs);
  }
  // The playback ended however the element said it - paused, finished, or its
  // clock at the length - so the line is left in the state the sound reached
  // rather than holding its last sampled frame.
  paintWords(at);
}

// ---------- wiring the new doors into the page ----------

function attachStudioDoors() {
  listenPane = ui.E('listen-pane');
  const listenBtn = ui.fingerAct('listen to it', function () {
    void (async function () { await listenNow(); })();
  }, 'what that recording says, said by the model that listened');
  if (ui.E('listen-zone')) ui.E('listen-zone').appendChild(listenBtn);
  keepFileDoor('listen-audio', 'listen-audio-form', function (files) {
    listenAudioFile = files[0];
    if (listenAudioFile) log('that recording is the one to listen to', 'ok');
  });
  ui.onChoose(ui.E('listen-model'), function () {
    const wanted = String(ui.chosenOption(ui.E('listen-model')) || '');
    if (wanted) drawOptionsFor('listen', wanted);
    void (async function () { await listenNow(); })();
  });
  ui.onChoose(ui.E('clone-model'), function () {
    void (async function () { await cloneNow(); })();
  });
  const cloneBtn = ui.fingerAct('clone it', function () {
    void (async function () { await cloneNow(); })();
  }, 'that recording speaks these new words');
  if (ui.E('clone-zone')) ui.E('clone-zone').appendChild(cloneBtn);
  keepFileDoor('clone-file', 'clone-file-form', function (files) {
    clonedFile = files[0];
    if (clonedFile) log('that recording is the one to clone with', 'ok');
  });
  ui.onChoose(ui.E('create-model'), function () {
    void (async function () { await createNow(); })();
  });
  const createBtn = ui.fingerAct('create it', function () {
    void (async function () { await createNow(); })();
  }, 'a voice from those words, saying this line');
  if (ui.E('create-zone')) ui.E('create-zone').appendChild(createBtn);
  ui.onChoose(ui.E('edit-voice'), function () {
    void (async function () { await editNow(); })();
  });
  const editBtn = ui.fingerAct('edit it', function () {
    void (async function () { await editNow(); })();
  }, 'say that voice again with new directions');
  if (ui.E('edit-zone')) ui.E('edit-zone').appendChild(editBtn);
  ui.onChoose(ui.E('play-render'), function () {
    const wanted = String(ui.selectValue('play-render') || '');
    playItem = null;
    for (const item of lastRenderList) {
      if (String(item.id) === wanted) playItem = item;
    }
    if (playItem) void (async function () { await playNow(); })();
  });
  const playBtn = ui.fingerAct('play it', function () {
    void (async function () { await playNow(); })();
  }, 'the audio, its words, and the sound drawn');
  if (ui.E('play-zone')) ui.E('play-zone').appendChild(playBtn);
}

function drawStudioPanels(models) {
  fillSelect('listen-model', models.filter(canListen), function (item) { return String(item.name || item.id); });
  fillVoiceSelect('listen-voice');
  fillSelect('clone-model', models.filter(canClone), function (item) { return String(item.name || item.id); });
  fillSelect('create-model', models.filter(canCreate), function (item) { return String(item.name || item.id); });
  fillVoiceSelect('clone-voice');
  fillVoiceSelect('edit-voice');
  fillSelect('edit-for', models.filter(standsLoaded), function (item) { return String(item.name || item.id); });
  drawPlayChoices(lastRenderList);
}

main();
