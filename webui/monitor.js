// The home page: everything the server is doing, read live, in one place.
// The page is built around the models: every model that is open or has worked
// and is still kept gets a full-width bar - named, stating what it holds,
// painting its running work - and the deeper numbers its engine states (its
// memory line, its files, its job table) wait behind hands on that bar. The
// machine gets the same treatment: device pools,
// memory, what the engines hold open, and what is in motion, each with its
// history line. Nothing here is a guess: a number no engine states is named as
// missing rather than drawn as zero, and the history lines are this document's
// own record of its own readings, labelled with the moments they were taken.

const E = ui.E;

const updated = E('updated');
const totalsLine = E('totals-line');
const sinceLine = E('since');
const machinePanel = E('machine-metrics');
const cardsHost = E('model-cards');
const keyStateLine = E('key-state');
// Whether the press that hands the key has been wired: the door line is
// redrawn with every reading, and the hand must be hung once.
let pressWired = false;
// The two engine marks worn beside the open-models panel's name: each names
// one engine, and which wear says which engines get model cards here.
const modelToggles = [
  { engine: 'image', words: 'picture', node: E('model-toggle-image') },
  { engine: 'voice', words: 'spoken', node: E('model-toggle-voice') },
];
// Both marks wear at the start: every model gets a card until a person takes
// one engine off the page.
const engineShown = { image: true, voice: true };
const logPanel = E('log');
const logLine = E('log-line');
const logClear = E('log-clear');

// The most lines the log panel draws. The record every page writes is longer
// than a screen; the panel is a window on its newest part, and the line under
// its title says what the window holds.
const logShown = 60;
let logStamp = null;

let selected = null;
let records = [];
let modelRows = [];

// Times this page's own reads of the status document took, newest last. This is
// the one number on this page not read from the machine under the server: the
// browser's view of the way to it, kept here because it is only measured here.
let pageReads = [];

// The most job rows a card draws. A card is a column the width of the page, so
// its rows are a window on the work, and the words under it say how wide that
// window is against what is kept rather than making a reader count them.
const cardRowsShown = 12;

// The last machine reading this page drew: a reader’s own hand on a bar -
// opening its facts or its jobs - redraws the panel from this reading, so the
// page answers the touch at once instead of waiting for a number to move.
let lastSnapshot = null;

// The cap each engine keeps its own jobs to - the one number the shared job
// ledger states for every module that owns a store. The page names it once, so
// its two sentences cannot drift apart from each other or from the machine.
const engineKeptJobs = 200;

// ---------- the words the page writes about numbers ----------

function whole(value) {
  const n = Number(value);
  if (!Number.isFinite(n)) return '';
  const negative = n < 0;
  let text = String(Math.abs(Math.round(n)));
  let grouped = '';
  while (text.length > 3) {
    grouped = ',' + text.slice(-3) + grouped;
    text = text.slice(0, -3);
  }
  return (negative ? '-' : '') + text + grouped;
}

// A rate needs both halves of a job: what it did and how long it took. A job
// missing either has no rate, and says so rather than one being invented from
// whole seconds of wall clock.
function perSecond(count, ms) {
  const value = Number(count);
  const took = Number(ms);
  if (!(value > 0) || !(took > 0)) return null;
  return (value * 1000) / took;
}

function rateText(value, unit) {
  if (value === null || !Number.isFinite(value)) return 'not stated';
  const words = (value < 10) ? value.toFixed(1) : whole(Math.round(value));
  return unit ? words + ' ' + unit : words;
}

function secondsText(ms) {
  if (!(Number(ms) > 0)) return 'not stated';
  const seconds = Number(ms) / 1000;
  return (seconds < 100 ? seconds.toFixed(1) : whole(Math.round(seconds))) + 's';
}

// A rate of bytes in the machine's own size words, which is the only way this
// page has for saying megabytes a second; a rate the machine never stated is
// said so, not guessed at from the clock.
function byteRate(bytesPerSecond) {
  const value = Number(bytesPerSecond);
  if (!(value > 0)) return 'not stated';
  return ui.sizeText(value) + '/s';
}

// Seconds of waiting in the words a waiting person reads: tens of seconds up
// to a minute, whole minutes up to an hour, hours past that.
function longSeconds(seconds) {
  const value = Number(seconds);
  if (!(value >= 0)) return 'not stated';
  const secs = Math.round(value);
  if (secs < 60) return secs + 's';
  const minutes = Math.round(secs / 60);
  if (minutes < 60) return minutes + 'm';
  // The hour is only ever what the minutes hold: rounding it from the total
  // would say 2h of a stretch that ran for ninety minutes.
  return Math.floor(minutes / 60) + 'h ' + (minutes % 60) + 'm';
}

// Speech is stated to the tenth of a second, the finest the engine names: a
// rounding to whole seconds would read a render of 1.7s aloud as though it had
// made two.
function speechText(seconds) {
  const value = Number(seconds);
  if (!(value > 0)) return 'not stated';
  return (Math.round(value * 10) / 10).toFixed(1) + 's';
}

// A count the machine stated for a run, or the words that say it did not: the
// rows arrive with unstated numbers left out rather than written as a zero, so a
// column of the table is drawn with those words instead of a zero that would
// claim the machine measured nothing.
function statedCount(run, name, unit) {
  const value = run[name];
  if (value === undefined || value === null || !Number.isFinite(Number(value))) return 'not stated';
  const words = whole(Number(value));
  return unit ? words + ' ' + unit : words;
}

// The words one figure of a meter is drawn with, and the unit word its label
// carries: the words of a byte count name their own unit, so a plain count keeps
// its unit for the meter's own label - said once after the numbers rather than
// after each of them, which reads wrong for one of anything.
function meterFigures(unit) {
  if (unit === 'bytes') return { value: (value) => ui.sizeText(value), unit: '' };
  // A rate keeps the decimals its own words use - a rate of 18.4 is not 18 -
  // and names the unit it was measured in.
  if (unit === 'steps/s' || unit === '× real time') {
    return { value: (value) => rateText(value), unit: ' ' + unit };
  }
  // A share of a thing keeps the decimal the kernel stated it in - a thread at
  // 62.4 of its own is not a thread at 62 - and names the share, not a count.
  if (unit === '%') {
    return { value: (value) => (Math.abs(Number(value)) < 100 ? Number(value).toFixed(1) : whole(Math.round(Number(value)))), unit: ' %' };
  }
  // A load average keeps the two decimals its own words use - 1.20 waiting is not
  // 1 - and names what it counts rather than a thread count.
  if (unit === 'load') {
    return { value: (value) => Number(value).toFixed(2), unit: ' waiting' };
  }
  return { value: (value) => whole(value), unit: unit ? ' ' + unit : '' };
}

function meanOf(values) {
  let sum = 0;
  let count = 0;
  for (const value of values) {
    if (!Number.isFinite(value)) continue;
    sum += value;
    ++count;
  }
  return count ? sum / count : null;
}

// ---------- the figures every card draws ----------
// A figure is a reading of numbers this machine stated: a line of where a value
// has been, or a bar per job for what each job did. A value that was not stated
// is a gap, never a zero, and a figure with nothing to draw says so in the
// words that name what is missing.
const NS = 'http://www.w3.org/2000/svg';

// The measure every line is drawn at: the words beside a point are placed
// against this same box, so a label cannot drift off the drawing it names.
const kTraceWidth = 320;
const kTraceHeight = 62;

function svgFigure(kind, width, height) {
  const svg = document.createElementNS(NS, 'svg');
  svg.setAttribute('viewBox', '0 0 ' + width + ' ' + height);
  svg.setAttribute('preserveAspectRatio', 'none');
  svg.setAttribute('aria-hidden', 'true');
  svg.setAttribute('class', 'graph graph-' + kind);
  return svg;
}

function put(name, attrs) {
  const node = document.createElementNS(NS, name);
  for (const key in attrs) node.setAttribute(key, String(attrs[key]));
  return node;
}

function noFigure(host, words, reason) {
  const gap = document.createElement('div');
  gap.className = 'graph-none';
  gap.textContent = words;
  if (reason) gap.setAttribute('title', reason);
  host.appendChild(gap);
}

// Where a number, or a pair of them, has been across the moments this document
// read the machine. Every line is drawn across one fixed window that scrolls: the
// newest reading stands at the right edge, every older reading slides left as it
// ages, and a reading that has passed out of the window stops being drawn. The
// window is the reach this document keeps of its own readings - the monitor's
// newest 120 readings, taken 2 seconds apart - so a line never reaches past what
// the machine remembers. The words under the figure name the window, the step
// the readings were taken at, and the lowest, mean, and highest reading really
// inside it; the same three numbers stand beside the places they were read, so a
// reader sees how far a level sits from its own average rather than only that it
// moved.
const kTraceWindowSeconds = 240; // four minutes, the record the monitor document keeps of its own readings

// The readings one metric had at each moment, as {at: seconds, value: number} in
// the order the machine stated them. A moment that stated nothing is left out of
// the points rather than drawn as a zero.
function traceOf(samples, pick) {
  const points = [];
  for (const sample of samples) {
    const at = Number(sample && sample.t);
    if (!(at > 0)) continue;
    const stated = pick(sample);
    if (stated === null || stated === undefined || stated === '') continue;
    const value = Number(stated);
    if (!Number.isFinite(value)) continue;
    points.push({ at: at, value: value });
  }
  return points;
}

// host is the box the figure stands in. series is [{points, name, cls}]: cls 'a'
// is the line the panel is about, drawn filled under itself, and cls 'b' is the
// line drawn beside it for comparison. unit names what the numbers are counted
// in, so the words state them in their own units.
function drawTrace(host, series, unit) {
  const window = kTraceWindowSeconds;
  // The right edge is the newest moment anything here was read, not the moment
  // this page runs: a figure whose numbers did not move draws again in the same
  // place, so the wall does not slide sideways on every read while the machine
  // says nothing new.
  let right = 0;
  for (const line of series) {
    for (const point of (line && line.points ? line.points : [])) {
      if (point.at > right) right = point.at;
    }
  }
  if (!(right > 0)) {
    noFigure(host, 'no line yet',
      'Nothing at all was read here; a line needs two moments read inside the window.');
    return;
  }
  const left = right - window;
  const lines = [];
  const apart = [];
  for (const line of series) {
    const kept = [];
    for (const point of (line && line.points ? line.points : [])) {
      if (point.at < left || point.at > right) continue;
      kept.push(point);
    }
    if (kept.length < 2) continue;
    // The step of this line on its own: a line whose own readings are a minute
    // apart must not borrow the step of a line drawn beside it every two seconds,
    // or its own gaps would be drawn across as a flat run.
    const gaps = [];
    for (let at = 1; at < kept.length; ++at) {
      const gap = kept[at].at - kept[at - 1].at;
      if (gap > 0) gaps.push(gap);
    }
    gaps.sort();
    const ownStep = gaps.length ? gaps[Math.floor(gaps.length / 2)] : 0;
    for (const gap of gaps) apart.push(gap);
    lines.push({ points: kept, name: (line && line.name) ? String(line.name) : '', cls: (line && line.cls === 'b') ? 'b' : 'a', step: ownStep });
  }
  if (!lines.length) {
    noFigure(host, 'no line yet',
      'Nothing was read here inside the ' + whole(window / 60) + '-minute window this page draws; a line needs two moments read inside it.');
    return;
  }
  // The step these readings were taken at, read off the readings themselves: the
  // finest step any line here was read at, so the words name the closest pairing
  // this page actually holds.
  apart.sort();
  const step = apart.length ? apart[Math.floor(apart.length / 2)] : 2;
  // A figure with more than one line in it - every thread of a processor, every
  // engine, two metrics sharing a panel - keeps its lines unlabelled and names
  // one scale across all of them: a word at each point of several lines would
  // stand on top of itself.
  const many = lines.length > 1;
  const allPoints = [];
  // One scale for every line in this figure, so two lines drawn in the same
  // panel compare with each other rather than each with its own low.
  let low = 0;
  let high = 0;
  let first = true;
  for (const line of lines) {
    for (const point of line.points) {
      if (first) {
        low = point.value;
        high = point.value;
        first = false;
        continue;
      }
      if (point.value < low) low = point.value;
      if (point.value > high) high = point.value;
    }
  }
  if (high <= low) high = low + 1;
  const width = kTraceWidth;
  const height = kTraceHeight;
  const x = (at) => ((at - left) / window) * width;
  const y = (value) => height - 4 - ((value - low) / (high - low)) * (height - 8);
  // The lines and their words stand in one box, so the words set beside a
  // point are placed against the drawing and not against the panel around it.
  const box = document.createElement('div');
  box.className = 'trace';
  host.appendChild(box);
  const svg = svgFigure('trace', width, height);
  box.appendChild(svg);
  const figures = meterFigures(unit);
  const said = ['window ' + whole(window / 60) + ' min · readings ' + whole(Math.max(1, step)) + 's apart'];
  let drawable = 0;
  for (const line of lines) {
    // A reading past a stretch this line's own readings did not cover ends the
    // drawn run.
    const gapLimit = (line.step > 0 ? line.step : step) * 3;
    const runs = [];
    let run = [];
    let previous = null;
    for (const point of line.points) {
      if (previous !== null && point.at - previous > gapLimit) {
        if (run.length) runs.push(run);
        run = [];
      }
      run.push(point);
      previous = point.at;
    }
    if (run.length) runs.push(run);
    let linePath = '';
    let areaPath = '';
    const points = [];
    for (const part of runs) {
      if (part.length < 2) continue;
      for (let at = 0; at < part.length; ++at) {
        const px = x(part[at].at).toFixed(1);
        const py = y(part[at].value).toFixed(1);
        linePath += (at === 0 ? 'M' : 'L') + px + ' ' + py;
        areaPath += (at === 0 ? 'M' + px + ' ' + height + 'L' + px + ' ' + py : 'L' + px + ' ' + py);
        points.push(part[at]);
      }
      const last = part[part.length - 1];
      areaPath += 'L' + x(last.at).toFixed(1) + ' ' + height + 'Z';
    }
    if (!linePath) continue;
    ++drawable;
    if (line.cls === 'a' && !many) {
      svg.appendChild(put('path', { d: areaPath, class: 'trace-area' }));
    }
    svg.appendChild(put('path', { d: linePath, class: 'trace-line-' + line.cls }));
    if (many) {
      // Many lines in one figure: their readings are gathered so the figure can
      // name one scale for all of them, rather than a word at every point.
      for (const point of points) allPoints.push(point);
      continue;
    }
    // The mean of the readings this line shows, drawn across the plot.
    const mean = meanOf(points.map((point) => point.value));
    if (mean !== null && Number.isFinite(mean)) {
      svg.appendChild(put('path', { d: 'M0 ' + y(mean).toFixed(1) + 'H' + width, class: 'trace-avg' }));
    }
    // The three numbers, set beside the places they were read.
    let top = null;
    let bottom = null;
    for (const point of points) {
      if (top === null || point.value > top.value) top = point;
      if (bottom === null || point.value < bottom.value) bottom = point;
    }
    if (!points.length) continue;
    const named = line.name ? line.name + ' ' : '';
    if (top) {
      tag(box, 'max', x(top.at), y(top.value), named + 'max ' + figures.value(top.value) + figures.unit);
    }
    if (bottom) {
      tag(box, 'min', x(bottom.at), y(bottom.value), named + 'min ' + figures.value(bottom.value) + figures.unit);
    }
    if (mean !== null && Number.isFinite(mean)) {
      tag(box, 'avg', x(points[points.length - 1].at), y(mean), named + 'avg ' + figures.value(mean) + figures.unit);
    }
  }
  if (many && allPoints.length) {
    // One scale for the whole figure: the highest, lowest, and mean reading any
    // of the lines holds, named once, with the mean drawn across the plot.
    let top = null;
    let bottom = null;
    for (const point of allPoints) {
      if (top === null || point.value > top.value) top = point;
      if (bottom === null || point.value < bottom.value) bottom = point;
    }
    const mean = meanOf(allPoints.map((point) => point.value));
    if (mean !== null && Number.isFinite(mean)) {
      // The mean of every reading in the figure, drawn across the plot.
      svg.appendChild(put('path', { d: 'M0 ' + y(mean).toFixed(1) + 'H' + width, class: 'trace-avg' }));
    }
    if (top) {
      tag(box, 'max', x(top.at), y(top.value), 'max ' + figures.value(top.value) + figures.unit);
    }
    if (bottom) {
      tag(box, 'min', x(bottom.at), y(bottom.value), 'min ' + figures.value(bottom.value) + figures.unit);
    }
    if (mean !== null && Number.isFinite(mean)) {
      tag(box, 'avg', width, y(mean), 'avg ' + figures.value(mean) + figures.unit);
      said.push('across ' + whole(lines.length) + ' lines, one scale');
    }
  }
  // A reading here and there, never two moments near enough to join, draws no
  // line: the panel says so rather than showing an empty box.
  if (!drawable) {
    box.remove();
    noFigure(host, 'no line yet',
      'The readings taken here were never two moments near enough to join into a line.');
    return;
  }
  const scale = document.createElement('div');
  scale.className = 'graph-scale';
  scale.textContent = said.join(' · ');
  scale.setAttribute('title',
    'The window drawn is ' + whole(window / 60) + ' minutes ending at the newest reading here; that reading stands at the right edge, and an older one sits to the left of it. ' +
    'Each label sits on the plot at the reading it names: the highest, the lowest, and the mean taken inside that window.');
  host.appendChild(scale);
}

// One of those words, set beside the point it names: the place is kept inside the
// drawing, so the words stay readable whether the reading was a high one or a low
// one, and never name a value without standing where that value was.
function tag(box, kind, at, level, words) {
  const node = document.createElement('span');
  node.className = 'trace-tag ' + kind;
  node.style.left = Math.max(3, Math.min(97, (at / kTraceWidth) * 100)).toFixed(1) + '%';
  node.style.top = Math.max(18, Math.min(86, (level / kTraceHeight) * 100)).toFixed(1) + '%';
  node.textContent = words;
  box.appendChild(node);
}

// What each job did, one bar per job, oldest at the left.
function drawBars(host, values, unit) {
  const clean = values.filter((value) => Number.isFinite(value));
  if (!clean.length) {
    noFigure(host, 'no bars', 'nothing was measured in these jobs to draw');
    return;
  }
  // the words name the tallest value the jobs really measured; the drawing divides
  // by that same tallest, or by one when nothing was measured, so an all-zero set
  // still gets a shape rather than an invented tallest.
  let top = 0;
  for (const value of clean) if (value > top) top = value;
  const smallestDivisor = 1;
  const divisor = (top > 0) ? top : smallestDivisor;
  const width = 240;
  const height = 46;
  const svg = svgFigure('bars', width, height);
  const step = width / values.length;
  for (let index = 0; index < values.length; ++index) {
    const raw = values[index];
    // A job that stated nothing for this figure is a gap in the row of bars, not
    // a bar of zero: the engine kept the job, it did not measure this of it.
    if (raw === null || raw === undefined || !Number.isFinite(Number(raw))) continue;
    const value = Number(raw);
    const tall = Math.max(1, (value / divisor) * (height - 4));
    svg.appendChild(put('rect', {
      x: (index * step + step * 0.15).toFixed(2),
      y: (height - tall - 2).toFixed(2),
      width: Math.max(1, step * 0.7).toFixed(2),
      height: tall.toFixed(2),
      class: 'bar',
    }));
  }
  host.appendChild(svg);
  const figures = meterFigures(unit);
  const scale = document.createElement('div');
  scale.className = 'graph-scale';
  scale.textContent = 'each bar one job · tallest ' + figures.value(top) + figures.unit;
  host.appendChild(scale);
}

// a pool that has a limit: how much of it is used now. the limit is stated
// beside it, never assumed.
function drawPool(host, used, total, unit) {
  const wrap = document.createElement('div');
  wrap.className = 'pool';
  const bar = document.createElement('div');
  bar.className = 'pool-bar';
  const fill = document.createElement('div');
  fill.className = 'pool-fill';
  bar.appendChild(fill);
  wrap.appendChild(bar);
  const scale = document.createElement('div');
  scale.className = 'graph-scale';
  if (!(Number(total) > 0)) {
    scale.textContent = 'no limit is stated for this pool';
    wrap.appendChild(scale);
    host.appendChild(wrap);
    return;
  }
  const pct = Math.max(0, Math.min(100, Math.round((Number(used) / Number(total)) * 100)));
  fill.style.width = pct + '%';
  scale.textContent = whole(pct) + '% ' + unit;
  wrap.appendChild(scale);
  host.appendChild(wrap);
}

// A tile for one metric: the number, the words naming it, and the graph under
// it. A metric nothing states keeps its tile and says so, so the place stays
// and a reader knows the machine was asked.
function metric(host, title, value, unit, notes) {
  const box = document.createElement('div');
  box.className = 'metric';
  // What a tile means and any caveats live in its tooltip: the visible box is
  // its words, its number, and the figure — no prose in the grid.
  const tips = [];
  if (notes) for (const note of notes) if (note) tips.push(note);
  if (tips.length) box.setAttribute('title', tips.join('\n'));
  const head = document.createElement('div');
  head.className = 'metric-title';
  head.textContent = title;
  box.appendChild(head);
  const big = document.createElement('div');
  big.className = 'metric-value';
  big.textContent = value;
  if (unit) {
    const suffix = document.createElement('span');
    suffix.className = 'metric-unit';
    suffix.textContent = ' ' + unit;
    big.appendChild(suffix);
  }
  box.appendChild(big);
  host.appendChild(box);
  return box;
}

// ---------- the machine: every number this machine states ----------
//
// The machine's own numbers are read as a wall of panels in rows of one height:
// each panel names itself at its left and states, beside that name, the numbers
// it holds now - and for anything that stands under a limit, the share of that
// limit it holds. The rest of the panel is the line of where that number has
// been, drawn across the same fixed window every other line on the page uses, so
// the level can be compared from one panel to the next and the whole wall
// scrolls as the machine is read again. What a panel states is only ever what
// this machine states: a number no driver states keeps its place in the panel and
// says so rather than standing in as a zero. The sentences that say what a
// panel means, and any caveat, live in its tooltip - the panel itself carries
// names and numbers.

function drawMachine(system, summary, history, jobs) {
  machinePanel.textContent = '';
  const samples = (history && Array.isArray(history.samples)) ? history.samples : [];
  const devices = Array.isArray(system.devices) ? system.devices : [];
  const names = (history && Array.isArray(history.devices)) ? history.devices : [];

  // One pool as this machine reports it: what is used of what there is. A device
  // that states no byte counts has nothing to say, and is left out rather than
  // drawn as a zero.
  const poolOf = function (state) {
    const total = Number(state && state.total_bytes || 0);
    if (!(total > 0)) return null;
    const free = Number(state.free_bytes || 0);
    return { total: total, used: (free > total) ? 0 : (total - free) };
  };

  // How a pool's share is coloured: a pool mostly free is calm, one filling up
  // asks to be looked at, and one at the end of its room says so in the colour a
  // refusal is read in. The share is always stated in words as well - the colour
  // is a glance, the words are the fact.
  const levelWord = function (share) {
    if (share >= 95) return 'err';
    if (share >= 80) return 'warn';
    return 'ok';
  };

  // What the memory sticks under the numbers state that they are, in the
  // machine's own inventory words: the technology, the speed they are running
  // at, how many of them lay how wide a path, and the rate that path can carry
  // at full stretch. An inventory that names no sticks gets no line - nothing
  // here is ever read off a product name.
  const hw = system.memory_hw || {};
  const busText = function (withSticks) {
    const speed = Number(hw.speed_mts || 0);
    const bits = Number(hw.data_bits || 0);
    const modules = Number(hw.modules || 0);
    if (!(speed > 0) || !(bits > 0) || !(modules > 0)) return null;
    const parts = [];
    const kind = String(hw.kind || '');
    parts.push(kind ? kind + '-' + speed : speed + ' MT/s');
    if (withSticks && Number(hw.bytes || 0) > 0) {
      const per = ui.sizeText(Math.round(Number(hw.bytes) / modules)).replace(/\.0(?= [KMGT]?B$)/, '');
      parts.push(whole(modules) + ' × ' + per);
    }
    parts.push(whole(bits) + '-bit');
    const rate = Number(hw.bandwidth_gbps || 0);
    if (rate > 0) parts.push('≈' + rate.toFixed(1) + ' GB/s');
    return parts.join(' · ');
  };
  const busTip = function (what) {
    const stated = [];
    stated.push(what);
    if (Number(hw.rated_mts || 0) > 0 && Number(hw.rated_mts) !== Number(hw.speed_mts)) {
      stated.push('The sticks are rated for ' + Number(hw.rated_mts) + ' MT/s and run at ' + Number(hw.speed_mts) + ' MT/s.');
    } else if (Number(hw.rated_mts || 0) > 0) {
      stated.push('The sticks run at the ' + Number(hw.rated_mts) + ' MT/s they are rated for.');
    }
    if (hw.maker || hw.part) {
      stated.push('As the inventory names them: ' + [hw.maker, hw.part].filter(Boolean).join(' '));
    }
    return stated.join('\n');
  };
  // The memory panel's rating: the machine's own sticks, at their own speed.
  const memoryFoot = (function () {
    const text = busText(true);
    if (!text) return null;
    return { text: text, title: busTip('The machine\'s inventory of its own memory: what these sticks are and the ceiling their bus can carry.') };
  })();
  // The graphics panel's rating: where a management library makes the card
  // state its own clocks, they ride here beside the ceiling the card names;
  // where none answers - an AMD core keeps its clocks to itself - the honest
  // speed a graphics memory here runs at is the system bus above, which is
  // what this machine's graphics really reads. A core clock is never guessed.
  const gpuFoot = (function () {
    const clocks = system.gpu_clocks || {};
    const core = Number(clocks.core_mhz || 0);
    const mem = Number(clocks.mem_mhz || 0);
    if (core > 0 || mem > 0) {
      const parts = [];
      if (core > 0) parts.push('core ' + core + ' MHz' + (Number(clocks.core_max_mhz) > 0 ? ' (max ' + Number(clocks.core_max_mhz) + ')' : ''));
      if (mem > 0) parts.push('mem ' + mem + ' MHz' + (Number(clocks.mem_max_mhz) > 0 ? ' (max ' + Number(clocks.mem_max_mhz) + ')' : ''));
      return { text: parts.join(' · '), title: 'The card\'s own statement of the clocks it runs at this moment, beside the ceiling it states.' };
    }
    const text = busText(false);
    if (!text) return null;
    return { text: 'bus ' + text, title: busTip('The card itself states no clocks to this server, and nothing is guessed for it; its memory is this machine\'s own, and runs at this bus.') };
  })();

  // A panel: its name, the numbers it states beside that name, and the line of
  // where those numbers have been. series is the lines this panel draws; a panel
  // that has no line to draw keeps the place and names why in gap.
  const panel = function (cls, title, chips, series, unit, notes, gap, foot) {
    const card = document.createElement('div');
    card.className = 'gauge ' + cls;
    const tips = [];
    if (notes) {
      for (const note of notes) if (note) tips.push(note);
    }
    if (tips.length) card.setAttribute('title', tips.join('\n'));
    const head = document.createElement('div');
    head.className = 'gauge-head';
    const name = document.createElement('div');
    name.className = 'gauge-title';
    name.textContent = title;
    head.appendChild(name);
    const chipHost = document.createElement('div');
    chipHost.className = 'gauge-chips';
    for (const chip of chips) {
      if (!chip) continue;
      const pill = document.createElement('span');
      pill.className = 'gauge-chip ' + (chip.cls ? chip.cls : 'value');
      const words = document.createElement('b');
      words.className = 'gauge-value';
      words.textContent = chip.value;
      pill.appendChild(words);
      if (chip.unit) {
        const unitWords = document.createElement('span');
        unitWords.className = 'gauge-unit';
        unitWords.textContent = ' ' + chip.unit;
        pill.appendChild(unitWords);
      }
      chipHost.appendChild(pill);
    }
    head.appendChild(chipHost);
    card.appendChild(head);
    const plot = document.createElement('div');
    plot.className = 'gauge-plot';
    card.appendChild(plot);
    if (series.length) {
      drawTrace(plot, series, unit);
    } else {
      noFigure(plot, gap && gap.words ? gap.words : 'no line drawn',
        gap && gap.reason ? gap.reason : 'This machine states nothing here to draw a line from.');
    }
    // The one line this panel does not measure: what the hardware under the
    // numbers states that it is, pinned to the foot of the panel so a row of
    // them reads as a wall of nameplates.
    if (foot && foot.text) {
      const line = document.createElement('div');
      line.className = 'gauge-foot';
      line.textContent = foot.text;
      if (foot.title) line.setAttribute('title', foot.title);
      card.appendChild(line);
    }
    machinePanel.appendChild(card);
    return card;
  };

  // cpu: what this machine states about its processor. Where the kernel keeps a
  // number for each thread of the machine - as the kernel of Windows does - every
  // thread gets its own line, all of them drawn against one scale so a reader
  // compares threads against each other rather than each against its own low. A
  // machine that states only a load average draws that one line; one that states
  // neither names the absence rather than drawing a flat line.
  const cpu = system.cpu || {};
  const cores = Number(cpu.cores || 0);
  const load = (cpu.load && Number.isFinite(Number(cpu.load.one))) ? Number(cpu.load.one) : null;
  let threadCount = 0;
  for (const sample of samples) {
    if (Array.isArray(sample.th) && sample.th.length > threadCount) threadCount = sample.th.length;
  }
  const threads = [];
  for (let at = 0; at < threadCount; ++at) {
    const points = traceOf(samples, (sample) =>
      (Array.isArray(sample.th) && at < sample.th.length) ? sample.th[at] : null);
    if (points.length >= 2) {
      threads.push({ points: points, name: 'thread ' + whole(at + 1), cls: 'c' });
    }
  }
  // The share of work every thread is doing at the newest reading this page holds.
  const busyNow = meanOf(threads.map((line) => line.points[line.points.length - 1].value));
  const cpuSeries = threads.length
    ? threads
    : (load === null ? [] : [{ points: traceOf(samples, (sample) => sample.load) }]);
  panel('cpu', 'cpu', [
    {
      value: cores ? whole(cores) : (threadCount ? whole(threadCount) : 'not stated'),
      unit: (cores || threadCount) ? 'threads' : '',
      cls: (cores || threadCount) ? '' : 'none',
    },
    busyNow !== null
      ? { value: whole(busyNow) + '%', unit: 'of each thread now', cls: levelWord(busyNow) }
      : (load === null
        ? { value: 'not stated', unit: '', cls: 'none' }
        : { value: load.toFixed(2), unit: 'waiting', cls: '' }),
  ], cpuSeries, threads.length ? '%' : 'load',
    [cpu.name ? String(cpu.name) : null,
    threads.length
      ? 'One line per thread of this machine, as its kernel states the share of that thread it spends working.'
      : null,
    cores ? 'Hardware threads this machine states it has.' : null,
    load !== null
      ? 'Load above the thread count means work queues behind the processor; the line is this page\'s readings of it.'
      : 'This system reports no load average; nothing is inferred.'],
    {
      words: threads.length ? 'no thread read yet' : 'no load stated',
      reason: threads.length
        ? 'The kernel of this machine states no per-thread numbers, so there is no line to draw.'
        : 'This system reports no load average, so there is no line to draw.'
    });

  // memory: what of it is in use now, and the line of where that use has been.
  // A machine that states no memory of its own falls to the one device that
  // carries the same pool, the way the header chips read it.
  let memory = poolOf(system.memory || {});
  if (!memory) {
    for (const device of devices) {
      if (String(device.type || '') !== 'cpu') continue;
      const pool = poolOf(device);
      if (pool) {
        memory = pool;
        break;
      }
    }
  }
  if (memory) {
    const share = Math.round((memory.used / memory.total) * 100);
    panel('mem', 'memory', [
      { value: whole(share) + '%', unit: 'used', cls: levelWord(share) },
      { value: ui.sizeText(memory.used) + ' / ' + ui.sizeText(memory.total) },
    ], [{ points: traceOf(samples, (sample) => (Array.isArray(sample.ram) && sample.ram.length > 1) ? sample.ram[1] : null) }],
      'bytes', ['As reported by this machine; the line is this page\'s own readings of it.'], null, memoryFoot);
  } else {
    panel('mem', 'memory', [{ value: 'not stated', cls: 'none' }], [], '',
      ['No driver states this machine\'s memory; nothing is drawn.'],
      { words: 'no memory stated', reason: 'No driver states this machine\'s memory.' });
  }

  // each device the machine states memory for gets its own panel, named by its
  // own name, drawn the way the machine's memory is drawn - the driver's words
  // sit in the tip.
  const devicesStated = [];
  for (const device of devices) {
    const type = String(device.type || '');
    if (type !== 'gpu' && type !== 'accel') continue;
    const pool = poolOf(device);
    if (!pool) continue;
    devicesStated.push(String(device.name));
    const share = Math.round((pool.used / pool.total) * 100);
    const at = names.indexOf(String(device.name));
    panel('dev', 'gpu · ' + String(device.name), [
      { value: whole(share) + '%', unit: 'used', cls: levelWord(share) },
      { value: ui.sizeText(pool.used) + ' / ' + ui.sizeText(pool.total) },
    ], at >= 0
      ? [{ points: traceOf(samples, (sample) => (sample.p && at < sample.p.length) ? sample.p[at] : null) }]
      : [],
      'bytes',
      [device.description ? String(device.description).trim() : null,
        'As reported by the driver; the line is this page\'s own readings of it.',
      at >= 0 ? null : 'This device has not been open during enough of this page\'s readings to draw a line.'],
      at >= 0 ? null : { words: 'no line yet', reason: 'Too few readings of this device to draw a line.' }, gpuFoot);
  }
  if (!devicesStated.length) {
    panel('dev', 'gpu', [{ value: 'no driver found', cls: 'warn' }], [], '',
      ['Nothing is guessed; this page only lists what a driver actually states.'],
      { words: 'no device stated', reason: 'No driver states a device with memory of its own.' });
  }

  // the one number here the server does not state: how long the browser's own
  // read of this status document took.
  const readPoints = [];
  for (const read of pageReads) {
    readPoints.push({ at: Math.floor(Number(read.t) / 1000), value: Number(read.ms) });
  }
  const readMean = pageReads.length
    ? meanOf(pageReads.map((read) => Number(read.ms))) : null;
  panel('read', 'page read', [
    { value: pageReads.length ? whole(pageReads[pageReads.length - 1].ms) : 'not read yet', unit: pageReads.length ? 'ms round trip' : '', cls: pageReads.length ? '' : 'none' },
    readMean === null ? null : { value: whole(Math.round(readMean)), unit: 'ms average' },
  ], [{ points: readPoints }], 'ms',
    ['The only figure here not measured by the server: the round trip the browser makes for the ' +
      'status document, timed on this page. It says nothing about any other network.'],
    { words: 'no line yet', reason: 'Only one read so far; a line needs two.' });

  // What the engines did their work at, drawn as the reference draws a bandwidth
  // panel: the rate at which this machine moves work through itself. The numbers
  // come from the jobs the engines keep - the same records the model cards state
  // - placed at the moment each job was asked, which is the moment its own row
  // names, so every point is a rate the machine really reported rather than a
  // smoothed guess. A line through those points joins measured jobs to each other
  // and nothing in between: it is read as the run of rates this machine made, not
  // as work that ran at that rate the whole way. Work that no kept job states a
  // rate for draws no panel, and a unit is never drawn beside a unit it
  // does not belong to: picture work counts steps and speech work counts seconds
  // of speech made.
  const workTitles = { 'steps/s': 'picture work', '× real time': 'speech work' };
  const grouped = {};
  const kindsSeen = {};
  for (const job of jobs) {
    const unit = rateUnitFor(job);
    const kind = String(job && job.kind || 'job');
    kindsSeen[kind] = true;
    if (!workTitles[unit]) continue;
    const rate = runRate(job);
    if (rate === null) continue;
    const at = Math.floor(Number(job.t));
    if (!(at > 0)) continue;
    const engine = engineWords(job && job.engine);
    const byEngine = grouped[unit] || (grouped[unit] = {});
    const points = byEngine[engine] || (byEngine[engine] = []);
    points.push({ at: at, value: rate });
  }
  let ratePanels = 0;
  for (const unit of ['steps/s', '× real time']) {
    const byEngine = grouped[unit];
    if (!byEngine) continue;
    const engines = [];
    for (const engine in byEngine) {
      const points = byEngine[engine];
      points.sort((first, second) => first.at - second.at);
      if (points.length) engines.push({ engine: engine, points: points });
    }
    if (!engines.length) continue;
    const series = [];
    const values = [];
    let newest = null;
    for (const line of engines) {
      series.push({ points: line.points, name: line.engine, cls: engines.length > 1 ? 'c' : 'a' });
      for (const point of line.points) {
        values.push(point.value);
        if (!newest || point.at >= newest.at) newest = point;
      }
    }
    const mean = meanOf(values);
    ++ratePanels;
    panel('rate', workTitles[unit], [
      { value: rateText(newest ? newest.value : null), unit: unit, cls: '' },
      mean === null ? null : { value: rateText(mean), unit: unit + ' average' },
    ], series, unit,
      ['The rate each kept job of this work stated for itself, placed at the moment that job was asked - the moment its own row names.'],
      null);
  }
  if (!ratePanels) {
    // The kept jobs are work that states no rate - a load, a download, a listen.
    // The panel keeps its place and names what is missing rather than drawing a
    // flat line at zero.
    panel('rate', 'work rate', [{ value: 'not stated', cls: 'none' }], [], '',
      [Object.keys(kindsSeen).length
        ? 'The jobs kept here are ' + Object.keys(kindsSeen).sort().join(' · ') + '; none of those kinds states both what it did and how long it took.'
        : 'No engine keeps a job right now, so there is no work here to state a rate.'],
      { words: 'no rate stated', reason: 'No kept job states a rate: a rate needs both the work it did and the time it took.' });
  }

  // The volume this server runs on is read as a storage card: its name, what it
  // states about itself now, and a rule grown to the share of its room that is
  // used. Free space bounds what can still be downloaded; the models' own sizes
  // appear on their cards.
  const disk = poolOf(system.disk || {});
  if (disk) {
    const share = Math.round((disk.used / disk.total) * 100);
    const card = document.createElement('div');
    card.className = 'storage-card ' + levelWord(share);
    card.setAttribute('title',
      'The volume this server reads its free space on. Free space bounds what can still be downloaded.');
    // What of this volume has been in use, drawn behind the card's words: the
    // monitor records what of the volume was used at each of its own readings,
    // so the line shows what grew and shrank as files were written and removed.
    const volumeFigure = document.createElement('div');
    volumeFigure.className = 'volume-figure';
    card.appendChild(volumeFigure);
    drawTrace(volumeFigure, [{
      points: traceOf(samples,
        (sample) => (Array.isArray(sample.disk) && Number(sample.disk[0]) > 0) ? sample.disk[1] : null),
    }], 'bytes');
    const mark = document.createElement('span');
    mark.className = 'storage-dot';
    card.appendChild(mark);
    const name = document.createElement('div');
    name.className = 'storage-name';
    name.textContent = 'this server\'s volume';
    card.appendChild(name);
    const state = document.createElement('div');
    state.className = 'storage-state';
    state.textContent = whole(share) + '% used';
    card.appendChild(state);
    const detail = document.createElement('div');
    detail.className = 'storage-detail';
    detail.textContent = ui.sizeText(disk.used) + ' / ' + ui.sizeText(disk.total) +
      ' · ' + ui.sizeText(disk.total - disk.used) + ' left';
    card.appendChild(detail);
    const bar = document.createElement('div');
    bar.className = 'storage-bar';
    const fill = document.createElement('i');
    fill.style.width = Math.max(0, Math.min(100, share)) + '%';
    bar.appendChild(fill);
    card.appendChild(bar);
    const strip = document.createElement('div');
    strip.className = 'gauge-strip';
    strip.appendChild(card);
    machinePanel.appendChild(strip);
  }

  const notStated = [];
  if (!threadCount) notStated.push('the share of each thread of the processor');
  if (!devicesStated.length) notStated.push('device memory');
  if (!memory) notStated.push('memory on the machine');
  if (!disk) notStated.push('the space on the volume this server runs on');
  if (notStated.length) {
    const gapLine = document.createElement('div');
    gapLine.className = 'log warn';
    gapLine.textContent = 'Not stated by this machine: ' + notStated.join(' · ');
    gapLine.setAttribute('title', 'A metric no driver states is named here, never drawn as a zero.');
    machinePanel.appendChild(gapLine);
  }
}

// ---------- every engine at a glance ----------

function engineWords(engine) {
  if (engine === 'image' || engine === 'voice') return engine;
  return 'job';
}

// What one model did across the jobs its engine keeps: totals in the same order
// as the words for each kind of work. A number no job stated stays out rather
// than counting as zero.
function modelUseWords(runs) {
  let steps = 0;
  let audioTenths = 0;
  let blocks = 0;
  let workMs = 0;
  let fastest = null;
  let fastestRun = null;
  for (const run of runs) {
    if (Number(run.max) > 0 && run.kind === 'generate') steps += Number(run.max);
    if (Number(run.audio_s) > 0) audioTenths += Math.round(Number(run.audio_s) * 10);
    if (Number(run.blocks) > 0) blocks += Number(run.blocks);
    const taken = runMs(run);
    if (taken > 0) workMs += taken;
    const rate = runRate(run);
    if (rate !== null && (fastest === null || rate > fastest)) {
      fastest = rate;
      fastestRun = run;
    }
  }
  const said = [];
  if (steps > 0) said.push(whole(steps) + ' steps run');
  if (audioTenths > 0) said.push(speechText(audioTenths / 10) + ' of speech made');
  if (blocks > 0) said.push(whole(blocks) + ' text blocks requested');
  if (workMs > 0) said.push(secondsText(workMs) + ' of work');
  if (fastest !== null) said.push('fastest run at ' + rateText(fastest, rateUnitFor(fastestRun)));
  return said;
}

// The one line the page keeps about all engines at once: what the last finished
// job of each kind took. Each engine and model's detail lives on that model's
// own card, not here.
function drawTotals(summary) {
  if (!summary) return;
  const parts = [];
  for (const pair of [['image', 'last image'], ['voice', 'last speech']]) {
    const stat = summary[pair[0]] || {};
    if (Number(stat.last_s || 0) > 0) parts.push(pair[1] + ': ' + whole(stat.last_s) + 's');
  }
  // the shared library states a moment one way, so the page does not state one
  // in another format.
  updated.textContent = 'live · wall drawn ' + ui.clockMs(Date.now()) + ' · ' +
    whole(Number(summary.retained || 0)) + ' job' + (Number(summary.retained || 0) === 1 ? '' : 's') +
    ' kept · ' + whole(Number(summary.active || 0)) + ' in motion';
  totalsLine.textContent = parts.join(' · ');
}

// ---------- one model, one card the width of the page ----------

function runText(run, engine, model) {
  return [run.id, run.kind, run.state, engine, model, run.voice, run.port].join(' ').toLowerCase();
}

// A row shows when the filter and the search both say so, matched in the words
// that row's own model states.
// The hands one table carries over its own rows: what to show and what to search
// for. They are built beside the table because they are that table's controls -
// a page reading several cards narrows one table and leaves the tables next to it
// standing as they were.
// A table's hands are kept between drawings: the wall and the cards are drawn
// again whenever the machine says something new, and what a reader chose is their
// own, so it stays put while the page redraws around it.
const tableHands = new Map();

function cardRowControls(engine, model) {
  const key = engine + '\t' + model;
  const held = tableHands.get(key);
  if (held) return held;
  const wrap = document.createElement('div');
  wrap.className = 'card-controls';
  const words = document.createElement('span');
  words.className = 'card-controls-words';
  words.textContent = 'rows in this table:';
  wrap.appendChild(words);
  const select = document.createElement('select');
  select.setAttribute('aria-label', 'Narrow this table to one kind of job');
  select.setAttribute('title', 'Narrow the rows in this table to one kind of job.');
  for (const option of [
    ['all', 'Every job in this table, from this model.', 'all jobs'],
    ['running', 'Only the jobs working right now.', 'running now'],
    ['done', 'Only the jobs that finished.', 'finished'],
    ['error', 'Only the jobs that failed or were stopped.', 'problems'],
    ['image', 'Only picture jobs.', 'image only'],
    ['voice', 'Only spoken renders.', 'voice only'],
  ]) {
    const node = document.createElement('option');
    node.setAttribute('value', option[0]);
    node.setAttribute('title', option[1]);
    node.textContent = option[2];
    select.appendChild(node);
  }
  wrap.appendChild(select);
  const search = document.createElement('input');
  search.setAttribute('aria-label', 'Search this table');
  search.setAttribute('placeholder', 'model, voice, kind, or job id');
  search.setAttribute('title', 'Matches the model, voice, kind, or job id in this table.');
  wrap.appendChild(search);
  const controls = { wrap: wrap, select: select, search: search };
  tableHands.set(key, controls);
  return controls;
}

// Which rows of one table show through that table's own hands.
function rowVisible(engine, model, controls) {
  return (run) => {
    const query = String(controls.search.value || '').trim().toLowerCase();
    if (query && runText(run, engine, model).indexOf(query) < 0) return false;
    const filter = ui.chosenOption(controls.select) || 'all';
    if (filter === 'running') return run.state === 'running';
    if (filter === 'done') return run.state === 'done';
    if (filter === 'error') return run.state === 'error' || run.state === 'cancelled';
    if (filter !== 'all') return engineWords(engine) === filter;
    return true;
  };
}

// What a model's own engine states about it, read off the model rows this page
// collected: a name, the weights on disk, the files it names. Each engine
// names these in its own words — a picture table says bytes and on_disk, the
// voice catalog says weights_bytes and weights_present — so both spellings are
// settled here into the one shape a card reads, taken from the words each
// engine actually uses and never guessed from the model.
function tableRow(model) {
  for (const row of modelRows) {
    if (String(row.id) !== String(model)) continue;
    const facts = {
      id: row.id,
      name: row.name,
      bytes: Number(row.bytes !== undefined ? row.bytes : row.weights_bytes || 0),
      on_disk: row.on_disk !== undefined ? row.on_disk : row.weights_present,
      files: Array.isArray(row.files) ? row.files : [],
    };
    if (row.memory) facts.memory = row.memory;
    return facts;
  }
  return null;
}

// A rate, in the words that engine's own kind of work measures it by. The taken
// time is named elapsed_ms by whichever module owns a job, and that is the one
// name every reading here asks for.
function runMs(run) {
  return Number(run.elapsed_ms || 0);
}

function runRate(run) {
  const ms = runMs(run);
  if (!(ms > 0)) return null;
  // a voice run is counted as the real-time factor it made speech at: seconds of
  // speech per second of work.
  if (run.kind === 'speak') return perSecond(Number(run.audio_s || 0), ms);
  // a run that samples pictures is counted in the steps it was asked for; a load,
  // a download, a listen, a measurement counts something else, so no rate is read
  // from it under a unit that does not name that something.
  if (run.kind === 'generate') return perSecond(Number(run.max || run.done || 0), ms);
  return null;
}

// the words a run's own kind of work counts its rate in, so a number is never
// shown without the unit it was measured in.
function rateUnitFor(run) {
  if (run.kind === 'speak') return '× real time';
  if (run.kind === 'generate') return 'steps/s';
  return '';
}

// What a bar keeps open between readings: the facts strip and the jobs table
// belong to the reader who opened them, and they stand through every reading
// until that reader closes them again.
const barOpen = new Map();

// One model, one bar: the line that names it and states how it stands; the
// line that moves - a running job's bar, or the last thing that moved here;
// and behind two hands, the facts strip and the jobs table, for the reader
// who wants them. A panel watching twenty models stays a page a person can
// read, and nothing the old card stated is lost - only what was asked for is
// drawn.
// One standing picture as a line: what it starts from, what it will draw, what
// it costs, where it stands, and the hands over it. Pressing up or down moves
// it within the line the machine holds; the header tells how the drawing goes.
function queueLine(host, engine, model, run, total) {
  const line = document.createElement('div');
  line.className = 'queue-line ' + String(run.state || '');
  const place = Number(run.queue_pos);
  const where = document.createElement('span');
  where.className = 'queue-place';
  where.textContent = place === 0 ? 'on the card' : (place > 0 ? 'line ' + place : 'not lined yet');
  where.title = total > 1 ? whole(total) + ' standing for this model' : 'the only one standing for this model';
  line.appendChild(where);
  if (Array.isArray(run.ref_urls)) {
    for (const url of run.ref_urls) {
      const back = document.createElement('img');
      back.className = 'queue-ref';
      back.setAttribute('width', '26');
      back.setAttribute('height', '26');
      back.setAttribute('alt', 'a picture this ask starts from');
      back.title = 'this ask starts from this picture';
      back.src = ui.fileSrc(String(url));
      line.appendChild(back);
    }
  }
  const words = document.createElement('span');
  words.className = 'queue-words';
  const prompt = String(run.prompt || '');
  words.textContent = prompt !== '' ? ui.clip(prompt, 90) : '(no words kept with this job)';
  words.title = prompt !== '' ? prompt : 'The ask came without words this line can read.';
  line.appendChild(words);
  const sets = [];
  if (Number(run.steps) > 0) sets.push('steps ' + whole(Number(run.steps)));
  if (Number(run.width) > 0 && Number(run.height) > 0) sets.push(Number(run.width) + 'x' + Number(run.height));
  if (sets.length) {
    const set = document.createElement('span');
    set.className = 'queue-set';
    set.textContent = sets.join(' · ');
    line.appendChild(set);
  }
  const idn = document.createElement('span');
  idn.className = 'queue-id';
  idn.textContent = String(run.id || '');
  idn.title = 'job ' + String(run.id || '');
  line.appendChild(idn);
  const hands = document.createElement('span');
  hands.className = 'queue-hands';
  if (String(run.state) === 'queued') {
    hands.appendChild(ui.fingerAct('up', function () { void moveJobLine(engine, model, run, -1); },
      'Move this picture one place nearer the front of the line. The one on the card keeps its place.', 'up'));
    hands.appendChild(ui.fingerAct('down', function () { void moveJobLine(engine, model, run, 1); },
      'Move this picture one place further back in the line.', 'down'));
  }
  hands.appendChild(ui.fingerAct('stop', function () { void stopJobLine(String(run.id || '')); },
    'Stop this job: on the machine if it draws, or off the line if its turn has not come.', 'cancel'));
  line.appendChild(hands);
  host.appendChild(line);
}

// Where a standing job sits in the line: the one on the card first, the queued
// ones in the machine's own order.
function queueSort(a, b) {
  const pa = Number(a.queue_pos) >= 0 ? Number(a.queue_pos) : Number.MAX_SAFE_INTEGER;
  const pb = Number(b.queue_pos) >= 0 ? Number(b.queue_pos) : Number.MAX_SAFE_INTEGER;
  if (pa !== pb) return pa - pb;
  return (Number(a.t) || 0) - (Number(b.t) || 0);
}

// Move a line by trading its place with a neighbour in the page's own reading,
// so the card answers the press at once. The machine is told after; when it
// agrees, the next reading says the same. When it does not, its reading is the
// one that redraws the card, and this page keeps no order of its own.
function shiftLine(engine, model, id, shift) {
  const models = (lastSnapshot && Array.isArray(lastSnapshot.models)) ? lastSnapshot.models : [];
  for (const entry of models) {
    if (engineWords(entry.engine) !== engine || String(entry.model) !== model) continue;
    const runs = Array.isArray(entry.runs) ? entry.runs : [];
    const lined = runs.filter((run) => run.kind === 'generate' &&
      (run.state === 'running' || run.state === 'queued')).sort(queueSort);
    const at = lined.findIndex((run) => String(run.id) === String(id));
    if (at < 0) return false;
    const to = at + shift;
    if (to < 0 || to >= lined.length) return false;
    const pa = Number(lined[at].queue_pos);
    const pb = Number(lined[to].queue_pos);
    if (!(pa >= 1) || !(pb >= 1)) return false;
    const keep = lined[at].queue_pos;
    lined[at].queue_pos = lined[to].queue_pos;
    lined[to].queue_pos = keep;
    return true;
  }
  return false;
}

async function moveJobLine(engine, model, run, shift) {
  const id = String(run.id || '');
  if (id === '') return;
  if (shiftLine(engine, model, id, shift) && lastSnapshot) drawModelCards(lastSnapshot);
  const answer = await ui.actDoor('/api/jobs/reorder', { id: id, shift: shift });
  if (!answer || answer.ok === false) {
    ui.log('the machine would not move that job: ' +
      ((answer && answer.error) || 'it gave no words'), 'warn');
    await refresh();
  }
}

async function stopJobLine(id) {
  if (id === '') return;
  const answer = await ui.api('/api/jobs/' + encodeURIComponent(id) + '/cancel', 'POST');
  if (answer) ui.log('stop sent for job ' + id + '; the drawn ones stay drawn.', 'warn');
  else ui.log('that job is not standing any more: ' + (ui.apiError || 'the machine gave no words'), 'err');
}

function drawCard(entry, system, history) {
  const engine = engineWords(entry.engine);
  const model = String(entry.model);
  const key = engine + '\t' + model;
  const shell = ui.cardShell(engine + ' model-bar', 'status-card');
  const card = shell.card;
  const main = shell.main;
  // The hands ride at the end of the bar's first line, so the facts rail the
  // shell offers by default has nothing left to carry.
  shell.fingers.remove();

  const runs = Array.isArray(entry.runs) ? entry.runs : [];
  const running = runs.filter((run) => run.state === 'running');
  const controls = cardRowControls(engine, model);
  const keep = rowVisible(engine, model, controls);
  const shown = runs.filter(keep);
  const table = tableRow(model);
  const held = Number(entry.resident_bytes || 0);
  const budget = Number((system.loaded && system.loaded.budget_bytes) || 0);
  const opened = barOpen.get(key) || {};
  let selectedPanel = null;

  const toggle = (which, label, tip) => {
    const hand = ui.fingerAct(label, function () {
      const now = barOpen.get(key) || {};
      now[which] = !(barOpen.get(key) && barOpen.get(key)[which]);
      barOpen.set(key, now);
      if (lastSnapshot) drawModelCards(lastSnapshot);
    }, tip);
    if (opened[which]) hand.classList.add('on');
    return hand;
  };

  // ---------- the line that names this model and how it stands ----------
  const head = document.createElement('div');
  head.className = 'bar-head';

  const tag = document.createElement('span');
  tag.className = 'card-tag';
  tag.textContent = engine;
  head.appendChild(tag);

  const title = document.createElement('span');
  title.className = 'card-title';
  title.textContent = model;
  head.appendChild(title);

  const state = document.createElement('span');
  state.className = 'card-state ' + (running.length ? 'running' : entry.open ? 'open' : 'closed');
  state.textContent = running.length ? whole(running.length) + ' running'
    : entry.open ? 'open' : 'not open';
  head.appendChild(state);

  if (Number(entry.port || 0) > 0) {
    const port = document.createElement('span');
    port.className = 'card-extra';
    port.textContent = 'port ' + entry.port;
    head.appendChild(port);
  }

  // What this model stands open with, and the pool the number fits in.
  if (engine === 'voice') {
    const voiceHeld = document.createElement('span');
    voiceHeld.className = 'card-extra';
    voiceHeld.textContent = 'engine memory';
    voiceHeld.setAttribute('title',
      'The voice engine lives inside this server; it does not report its own memory.');
    head.appendChild(voiceHeld);
  } else if (held > 0) {
    const heldWords = document.createElement('span');
    heldWords.className = 'card-extra bar-held';
    const fill = document.createElement('i');
    if (budget > 0) fill.style.width = Math.min(100, Math.round((held / budget) * 100)) + '%';
    const pool = document.createElement('span');
    pool.className = 'bar-pool';
    pool.appendChild(fill);
    pool.setAttribute('title', whole(held) + ' of ' + whole(budget) + ' bytes this engine may hold.');
    heldWords.textContent = ui.sizeText(held) + ' held';
    heldWords.appendChild(pool);
    head.appendChild(heldWords);
  } else {
    const heldWords = document.createElement('span');
    heldWords.className = 'card-extra';
    heldWords.textContent = 'nothing held';
    heldWords.setAttribute('title', entry.open
      ? 'This engine answers this model open but states no bytes for it.'
      : 'This model holds no memory until a load hand opens it.');
    head.appendChild(heldWords);
  }

  // The quant a model stands open as, named on its own card in the same words
  // the finished files keep beside them: an open model says what weights it
  // works with, not just that it is open.
  if (entry.open && entry.quant) {
    const quant = document.createElement('span');
    quant.className = 'card-extra';
    quant.textContent = String(entry.quant);
    quant.setAttribute('title', 'The quant this model stands open as - the weights the next asked job will run.');
    head.appendChild(quant);
  }

  const weights = table ? Number(table.bytes || 0) : 0;
  const shapeBytes = (table && table.memory) ? Number(table.memory.weights_bytes || 0) : 0;
  if (weights > 0 || shapeBytes > 0) {
    const disk = document.createElement('span');
    disk.className = 'card-extra';
    disk.textContent = ui.sizeText(weights || shapeBytes) + ' on disk';
    disk.setAttribute('title', table && table.on_disk === false
      ? 'the model table states this size but not these files on this machine'
      : 'This model\'s weight files, measured on disk.');
    head.appendChild(disk);
  }

  const actions = document.createElement('span');
  actions.className = 'bar-actions';
  if (entry.open) {
    const down = ui.fingerAct('put down', async function () {
      try {
        if (engine === 'voice') await ui.api('/voice/api/models/' + encodeURIComponent(model) + '/unload', 'POST', {});
        else await ui.api('/api/unload', 'POST', { model: model });
        ui.log(model + ' put down: its memory is free again', 'ok');
      } catch (refused) {
        ui.log('refused to put down ' + model + ': ' + refused.message, 'err');
      }
      await refresh();
    }, 'Let go of the memory this model holds right now. The weights stay on disk.', 'unload');
    actions.appendChild(down);
  } else {
    const up = ui.fingerAct('open', async function () {
      try {
        if (engine === 'voice') await ui.api('/voice/api/models/' + encodeURIComponent(model) + '/load', 'POST', {});
        else await ui.api('/api/models/' + encodeURIComponent(model) + '/load', 'POST', {});
        ui.log('opening ' + model + (engine === 'voice' ? '' : ' - its progress stands on this bar'), 'ok');
      } catch (refused) {
        ui.log('refused to open ' + model + ': ' + refused.message, 'err');
      }
      await refresh();
    }, 'Stand this model up in its engine. A big model takes its own time and its own memory.', 'load');
    actions.appendChild(up);
  }
  actions.appendChild(toggle('info', 'facts',
    'What this model is: the memory it holds and its line, its weight files, their quant and layers, and the rate of its kept jobs.'));
  actions.appendChild(toggle('jobs', 'jobs',
    'The jobs this engine keeps about this model: standing ones first with their hands, finished ones resting beneath. The hands above narrow them.'));
  head.appendChild(actions);
  main.appendChild(head);

  // ---------- the line that moves ----------
  // Every standing picture of this model is a line here: what it stands for,
  // where it stands in the line the machine holds, the hands that move it, and
  // the hand that stops it. The progress of what is drawing now is not repeated
  // here - the header of this page owns that, and every window reads the same
  // line. Work of the other kinds (a model being opened, bytes coming down)
  // keeps its moving bar, for the reader watching this card.
  const live = document.createElement('div');
  live.className = 'bar-live';
  const standing = runs.filter((run) => run.kind === 'generate' &&
    (run.state === 'running' || run.state === 'queued'));
  const otherRunning = running.filter((run) => run.kind !== 'generate');
  // A standing job is drawn once. While the jobs table is open the standing
  // ones take their places at the top of that table with their hands beside
  // them, and the card does not repeat them above it; with the table closed
  // this line is where they live.
  if (standing.length) {
    card.classList.add('is-running');
    standing.sort(queueSort);
    if (!opened.jobs) {
      live.classList.add('job-line');
      for (const run of standing) queueLine(live, engine, model, run, standing.length);
    }
  }
  for (const run of otherRunning) {
    card.classList.add('is-running');
    const max = Number(run.max || 0);
    const elapsed = Math.max(0, Math.round(Date.now() / 1000 - Number(run.t || 0)));
    const bytes = Number(run.bytes || 0);
    const total = Number(run.total_bytes || 0);
    if (String(run.kind) === 'download' && bytes > 0) {
      // a fetch states its bytes each round it ends, with the rate of the last
      // one and the average over the fetch: the bar moves on the bytes of the
      // file coming down, and the line says how much of it stands here, how
      // fast it is coming, and how long the rest stands at that rate.
      const speed = Number(run.speed || 0);
      const avg = Number(run.avg || 0);
      const words = ['download'];
      if (max > 1) words.push(whole(run.done) + ' of ' + whole(max) + ' files');
      if (max > 1 && run.file) words.push(String(run.file));
      words.push(total > 0 ? ui.sizeText(bytes) + ' of ' + ui.sizeText(total) : ui.sizeText(bytes) + ' down');
      words.push(byteRate(speed) + ' now');
      words.push(byteRate(avg) + ' average');
      if (total > 0)
        words.push(speed > 0 ? 'about ' + longSeconds((total - bytes) / speed) + ' left' : 'time left not stated');
      else
        words.push(secondsText(elapsed * 1000) + ' so far');
      ui.progressSet(live, String(run.id), bytes, total, words.join(' · '), '');
      continue;
    }
    ui.progressSet(live, String(run.id), Number(run.done || 0), max,
      run.kind + (max > 0 ? ' · ' + whole(run.done) + ' of ' + whole(max) + ' ' +
        (String(run.kind) === 'download' ? 'files' : 'steps') : ' · ' + elapsed + 's so far'),
      run.kind === 'load' ? 'load' : '');
  }
  if (!standing.length && !otherRunning.length) {
    const last = shown.find((run) => runMs(run) > 0) || shown[0];
    if (last) {
      const rate = runRate(last);
      const words = ['last ' + String(last.kind) + ' ' + String(last.state)];
      if (runMs(last) > 0) words.push(secondsText(runMs(last)) + ' taken');
      if (rate !== null) words.push(rateText(rate, rateUnitFor(last)));
      const when = ui.clock(last.t);
      if (when) words.push(when);
      live.textContent = words.join(' · ');
      live.setAttribute('title', 'The newest job this engine keeps its numbers about. Open the jobs to see the rest.');
    } else if (Number(entry.last_started_at || 0) > 0) {
      live.textContent = 'asked at ' + ui.clock(entry.last_started_at) + ' · no job numbers kept';
    } else {
      live.textContent = entry.open
        ? 'open and idle · nothing asked of it since this machine started'
        : 'nothing asked of this model since this machine started';
    }
  }
  main.appendChild(live);

  // ---------- the facts, for the reader who asked ----------
  if (opened.info) {
    const tiles = document.createElement('div');
    tiles.className = 'card-metrics ' + engine;
    const group = (words) => {
      const label = document.createElement('div');
      label.className = 'card-group';
      label.textContent = words;
      tiles.appendChild(label);
    };

    group('memory');
    const heldTile = metric(tiles, 'held in memory', held ? ui.sizeText(held) : 'not reported',
      held ? 'on this machine, right now' : 'from this engine',
      engine === 'voice'
        ? ['The voice engine lives inside this server; it does not report its own memory.']
        : null);
    if (engine === 'image') {
      drawPool(heldTile, held, budget, 'held');
      if (Array.isArray(history.models) && Array.isArray(history.samples)) {
        const at = history.models.indexOf(model);
        if (at >= 0) {
          drawTrace(heldTile, [{
            points: traceOf(history.samples,
              (sample) => (sample.m && at < sample.m.length) ? sample.m[at] : null),
          }], 'bytes');
        } else {
          noFigure(heldTile, 'no line yet',
            'This model has not been in memory during any page reading of this machine, so there is nothing to draw.');
        }
      } else {
        noFigure(heldTile, 'no line yet', 'Too few machine readings to draw this line yet.');
      }
    }

    group('on disk');
    const files = (table && Array.isArray(table.files)) ? table.files : [];
    if (files.length) {
      metric(tiles, 'files', files.join(', '), '',
        ['File names from the model tables for this model']);
    } else {
      // two different absences: a table that names this model and stops at its
      // files, and a model the tables never mentioned at all.
      metric(tiles, 'files', 'not stated', '',
        [table ? 'No file names in the model tables for this model'
          : 'This model is not in the model tables this page read, so nothing here names its files']);
    }
    metric(tiles, 'quantised to', quantWords(files), '',
      ['Read from the file names above; tensor types inside the files are not read.']);
    const shape = (table && table.memory) ? table.memory : null;
    if (shape) {
      if (Number(shape.n_layer || 0) > 0) {
        metric(tiles, 'layers', whole(Number(shape.n_layer)), 'of this model', null);
      } else {
        metric(tiles, 'layers', 'not stated', '', ['This model\'s file does not state its layer count.']);
      }
    }

    // The rate of the work kept here, in the unit that engine's kind of work
    // measures itself by, with a bar per job beside it.
    const steps = [];
    const stepRates = [];
    const speech = [];
    const speechRates = [];
    for (const run of shown.slice().reverse()) {
      if (run.kind === 'generate') {
        steps.push(Number(run.max || run.done || 0) || null);
        stepRates.push(runRate(run));
      } else if (run.kind === 'speak') {
        speech.push(Number(run.audio_s || 0) || null);
        speechRates.push(runRate(run));
      }
    }
    const lastFinished = shown.find((run) => runMs(run) > 0);
    group('history');
    if (engine === 'image') {
      const pictureRate = (lastFinished && lastFinished.kind === 'generate')
        ? runRate(lastFinished) : meanOf(stepRates);
      metric(tiles, 'steps/s', rateText(pictureRate), pictureRate === null ? '' : 'steps/s',
        ['The last picture that finished, or the mean over the ones kept here.']);
      drawBars(tiles, steps, 'steps');
      drawBars(tiles, stepRates, 'steps/s');
    } else if (engine === 'voice') {
      const speechRate = (lastFinished && lastFinished.kind === 'speak')
        ? runRate(lastFinished) : meanOf(speechRates);
      metric(tiles, 'speech speed', rateText(speechRate), speechRate === null ? '' : '× real time',
        ['How much sound a render made against the wall clock it took; above 1 is faster than real time.']);
      drawBars(tiles, speech, 's of speech');
      drawBars(tiles, speechRates, '× real time');
    }
    const useWords = modelUseWords(shown);
    metric(tiles, 'work totals',
      shown.length ? shown.length + ' of ' + whole(Number(entry.kept || 0)) + ' kept jobs' : 'nothing shows through the filter',
      '',
      useWords.length ? [useWords.join(' · ')] : ['Nothing measured in the kept jobs here.']);
    main.appendChild(tiles);
  }

  // ---------- the jobs table, for the reader who asked ----------
  if (!opened.jobs) {
    cardsHost.appendChild(card);
    return;
  }

  // The table carries its own hands over its rows, set down above it, and the
  // words for a job a reader pressed open under that row. Nothing about one
  // table lives elsewhere on the page, so a reader follows the table they are
  // looking at and the tables beside it stay as they were.
  main.appendChild(controls.wrap);

  // The jobs themselves in the engine's own words: the ones standing come
  // first in the machine's queue order, and the finished ones rest beneath
  // them newest first - a job that is done stops climbing the list. The rows
  // are a window on that work: the words under the table say what it shows
  // against what passes the filter and what the engine keeps.
  const standingRun = (run) => run.kind === 'generate' &&
    (run.state === 'running' || run.state === 'queued');
  const ordered = shown.slice().sort((a, b) => {
    const sa = standingRun(a) ? 0 : 1;
    const sb = standingRun(b) ? 0 : 1;
    if (sa !== sb) return sa - sb;
    if (sa === 0) return queueSort(a, b);
    return 0;
  });
  const listed = ordered.slice(0, cardRowsShown);
  const listing = document.createElement('div');
  listing.className = 'card-table';
  main.appendChild(listing);
  const headers = {
    image: ['when', 'job', 'state', 'steps', 'taken', 'steps/s'],
    voice: ['when', 'job', 'state', 'speech', 'speech/s'],
  }[engine] || ['when', 'job', 'state'];
  const headRow = document.createElement('div');
  headRow.className = 'table-row table-head';
  for (const label of headers) {
    const cell = document.createElement('span');
    cell.textContent = label;
    headRow.appendChild(cell);
  }
  listing.appendChild(headRow);
  for (const run of listed) {
    const row = document.createElement('div');
    row.className = 'table-row ' + (run.state || '');
    const cells = [ui.clock(run.t), run.kind, run.state];
    if (engine === 'image') {
      cells.push(statedCount(run, 'max'), secondsText(runMs(run)), rateText(runRate(run), 'steps/s'));
    } else if (engine === 'voice') {
      cells.push(Number(run.audio_s || 0) > 0 ? speechText(run.audio_s) : 'not stated',
        rateText(runRate(run), '× real time'));
    }
    for (const cell of cells) {
      const node = document.createElement('span');
      node.textContent = cell;
      row.appendChild(node);
    }
    if (run.voice) {
      const voice = document.createElement('span');
      voice.className = 'card-extra';
      voice.textContent = 'voice: ' + String(run.voice);
      row.appendChild(voice);
    }
    if (run.error) {
      const said = document.createElement('span');
      said.className = 'card-error';
      said.textContent = String(run.error);
      said.setAttribute('title', 'The full text of what the engine said when this job failed.');
      row.appendChild(said);
    }
    if (run.url) {
      const made = document.createElement('span');
      const madeWords = String(run.url);
      made.className = 'card-file';
      // A made file is named by its own file here; the full path is the tooltip.
      const slash = Math.max(madeWords.lastIndexOf('/'), madeWords.lastIndexOf('\\'));
      made.textContent = 'made: ' + (slash >= 0 ? madeWords.slice(slash + 1) : madeWords);
      made.setAttribute('title', madeWords);
      row.appendChild(made);
    }
    if (standingRun(run)) {
      // The hands that move a line ride beside the standing row itself: the
      // table is where a standing job is drawn while it is open, so this is
      // where its turn gets pushed or stopped.
      const hands = document.createElement('span');
      hands.className = 'queue-hands';
      if (String(run.state) === 'queued') {
        hands.appendChild(ui.fingerAct('up', function () { void moveJobLine(engine, model, run, -1); },
          'Move this picture one place nearer the front of the line. The one on the card keeps its place.', 'up'));
        hands.appendChild(ui.fingerAct('down', function () { void moveJobLine(engine, model, run, 1); },
          'Move this picture one place further back in the line.', 'down'));
      }
      hands.appendChild(ui.fingerAct('stop', function () { void stopJobLine(String(run.id || '')); },
        'Stop this job: on the machine if it draws, or off the line if its turn has not come.', 'cancel'));
      row.appendChild(hands);
    }
    listing.appendChild(row);
    const id = String(run.id || '');
    if (!id) continue;
    row.setAttribute('title', 'Job ' + id + ' — press to select it');
    if (id === selected) {
      row.classList.add('selected');
    } else {
      ui.onPress(row, function () {
        selected = id;
        row.classList.add('selected');
        if (selectedPanel && selectedPanel.parentNode) selectedPanel.parentNode.removeChild(selectedPanel);
        selectedPanel = jobPanel(engine, model, table, run);
        main.appendChild(selectedPanel);
      });
    }
  }

  const keptCount = whole(Number(entry.kept || 0));
  const windowNamed = (listed.length < shown.length)
    ? 'the newest ' + listed.length + ' of ' + shown.length + ' that pass the filter'
    : 'all ' + shown.length + ' that pass the filter';
  const foot = document.createElement('div');
  foot.className = 'card-foot';
  foot.textContent = shown.length
    ? windowNamed + ' · ' + keptCount + ' kept'
    : 'nothing kept shows through the filter';
  if (Number(entry.last_started_at || 0) > 0) foot.textContent += ' · last asked ' + ui.clock(entry.last_started_at);
  foot.setAttribute('title',
    shown.length
      ? 'The rows above are ' + windowNamed + ', of the ' + keptCount + ' jobs kept for this model.'
      : 'No jobs kept for this model show through the filter.');
  main.appendChild(foot);

  // A job a reader selected in an earlier reading keeps its card below this
  // table too, so the reading that follows does not quietly take what they asked
  // for off the page.
  for (const run of runs) {
    if (String(run.id || '') !== String(selected || '')) continue;
    selectedPanel = jobPanel(engine, model, table, run);
    main.appendChild(selectedPanel);
    break;
  }

  cardsHost.appendChild(card);
}

function drawModelCards(snapshot) {
  cardsHost.textContent = '';
  sinceLine.textContent = '';
  const models = Array.isArray(snapshot.models) ? snapshot.models : [];
  const open = models.filter((entry) => entry.open);
  const kept = models.filter((entry) => !entry.open && Number(entry.kept || 0) > 0);
  if (!open.length && !kept.length) {
    const empty = document.createElement('div');
    empty.className = 'meta';
    empty.textContent = 'No model is open in any engine, and no engine keeps jobs about one.';
    empty.setAttribute('title',
      'A model gets a card the moment an engine opens it or keeps a job about it. What each engine ' +
      'has done is named on its own page.');
    cardsHost.appendChild(empty);
  }
  const system = snapshot.system || {};
  const history = snapshot.history || {};
  // Open first, newest work first, then the models an engine only keeps jobs
  // about: a kept model's card is drawn the same way, and its own state line
  // says it is not open, so the page never draws one as if it held memory.
  open.sort((a, b) => Number(b.last_started_at || 0) - Number(a.last_started_at || 0));
  kept.sort((a, b) => Number(b.last_started_at || 0) - Number(a.last_started_at || 0));
  // The two marks beside the panel's name say which engines get a card: both
  // wear by default, and the whole set a person is looking at stands at once -
  // this page cuts nothing short.
  const matches = (entry) => {
    const engine = engineWords(entry.engine);
    return engine in engineShown ? engineShown[engine] : true;
  };
  const pool = open.concat(kept).filter(matches);
  if (!pool.length && models.length) {
    const hidden = [];
    if (!engineShown.image) hidden.push('picture');
    if (!engineShown.voice) hidden.push('spoken');
    const none = document.createElement('div');
    none.className = 'meta';
    none.textContent = hidden.length
      ? 'No model card shows while the ' + hidden.join(' and ') + ' mark' +
        (hidden.length > 1 ? 's are' : ' is') + ' pressed down.'
      : 'No model card matches what this machine named.';
    none.setAttribute('title', 'Press the mark above to draw that engine again.');
    cardsHost.appendChild(none);
  } else {
    for (const entry of pool) {
      drawCard(entry, system, history);
    }
  }
  let firstAt = 0;
  for (const job of records) {
    const at = Number(job.started_at || 0);
    if (at > 0 && (firstAt === 0 || at < firstAt)) firstAt = at;
  }
  sinceLine.textContent = firstAt
    ? 'Each engine keeps its newest ' + engineKeptJobs + ' jobs · oldest kept: ' + ui.clock(firstAt)
    : 'Nothing has run yet.';
  if (firstAt) sinceLine.setAttribute('title',
    'Each engine keeps its newest ' + engineKeptJobs + ' jobs; older ones are dropped. The oldest kept job started at ' + ui.clock(firstAt) + '.');
}

// Which way each engine mark wears now: a mark wears while its engine's cards
// are drawn, and its words say what pressing it will do next.
function drawEngineMarks() {
  for (const toggle of modelToggles) {
    if (!toggle.node) continue;
    const on = engineShown[toggle.engine];
    toggle.node.classList.toggle('on', on);
    toggle.node.setAttribute('aria-pressed', on ? 'true' : 'false');
    toggle.node.title = on
      ? toggle.words + ' models wear a card on this page - press to take them off'
      : toggle.words + ' models are off this page - press to draw them again';
  }
}

// ---------- a job the reader asked about, on its own ----------

// One card, drawn below the rows it belongs to: the model at the top left, the
// job it is named by in italic across the middle of that line, and the seconds
// it has stood here in the right hand. Everything else the machine stated about
// that job is drawn under that line as labelled fields, and a field the machine
// did not state is not drawn - the card holds what the machine said and nothing
// else, so a job another page asked for is stated as thinly as the machine does.
function jobPanel(engine, model, table, run) {
  const host = document.createElement('div');
  host.className = 'job-panel';
  const id = String(run.id || '');
  // The table's own row names the job; the page's job records state more about
  // it. Whichever the machine gave is what is written.
  const full = records.find((job) => String(job.id) === id);
  const chosen = full || run;
  const seconds = Number(chosen.elapsed_ms || 0);
  const asked = Number(chosen.started_at !== undefined ? chosen.started_at : chosen.t || 0);
  let stood = '';
  if (seconds > 0) stood = secondsText(seconds) + ' taken';
  else if (asked > 0 && String(chosen.state) === 'running') {
    stood = Math.max(0, Math.round(Date.now() / 1000 - asked)) + 's here';
  }
  UI.jobCard(host, {
    model: table && table.name ? String(table.name) : model,
    id: id,
    time: stood,
  }, [
    ['kind', chosen.kind],
    ['engine', engineWords(chosen.engine)],
    ['state', chosen.state],
    ['progress', Number(chosen.max || 0) > 0 ? whole(chosen.done) + ' of ' + whole(chosen.max) + ' steps'
      : (chosen.done !== undefined && chosen.done !== null ? whole(chosen.done) + ' answered · its window is not stated' : '')],
    ['bytes', String(chosen.kind) === 'download' && Number(chosen.bytes || 0) > 0
      ? ui.sizeText(chosen.bytes) + (Number(chosen.total_bytes || 0) > 0 ? ' of ' + ui.sizeText(chosen.total_bytes) : '') +
      (Number(chosen.speed || 0) > 0 ? ' · ' + byteRate(chosen.speed) + ' last round · ' + byteRate(chosen.avg) + ' average' : '') +
      (chosen.file ? ' · ' + String(chosen.file) : '')
      : ''],
    ['asked', ui.clock(asked)],
    ['ended', ui.clock(chosen.ended_at)],
    ['taken', secondsText(chosen.elapsed_ms)],
    ['prompt', chosen.prompt],
    ['asked by', chosen.source],
    ['its form', chosen.quant],
    ['voice', chosen.voice],
    ['port', Number(chosen.port || 0) > 0 ? chosen.port : ''],
    ['speech made', Number(chosen.audio_s || 0) > 0 ? speechText(chosen.audio_s) : ''],
    ['blocks', Number(chosen.blocks || 0) > 0 ? whole(chosen.blocks) : ''],
    ['share', chosen.share],
    ['made', chosen.url],
    ['error', chosen.error],
  ]);
  return host;
}

// The header of the page, painted from one machine reading: the chips state what
// the machine holds, and the bar names what runs. The bar reads the same job
// records the panels do, so a picture's or a voice's work is visible here and not
// only on the page that started it.
function paintMachineHeader(snapshot) {
  ui.monitorChrome(snapshot);
  ui.headerState(Array.isArray(snapshot && snapshot.jobs) ? snapshot.jobs : []);
}

// What the pages of this machine have said as they worked. Every page writes
// into one record (see UI.log) and this is the only page that draws it, because
// this is where a person comes to read what the machine is doing. A line names
// the page that wrote it, so a voice studio note is not read as a note about a
// picture, and nothing is worded here that the writing page did not say.
function drawLog() {
  if (!logPanel) return;
  const lines = ui.logLines(logShown);
  // A record that has not gained a line is left standing: this panel is redrawn
  // with every read of the machine, and rewriting it when nothing was said
  // would knock loose whatever a reader had just selected.
  const stamp = lines.length ? lines[lines.length - 1].at + '/' + lines.length : 'nothing kept';
  if (stamp === logStamp) return;
  logStamp = stamp;
  logPanel.textContent = '';
  for (const row of lines) {
    const line = document.createElement('div');
    line.className = 'log-line' + (row.kind ? ' ' + row.kind : '');
    line.textContent = ui.clockMs(Number(row.at) || 0) + '  ' + (row.page || 'page') +
      ' · ' + String(row.words || '');
    logPanel.appendChild(line);
  }
  if (logLine) {
    logLine.textContent = lines.length
      ? lines.length + ' line' + (lines.length === 1 ? '' : 's') +
      ' this browser has kept, oldest first' +
      (lines.length >= logShown ? '; the oldest are dropped past ' + logShown + '.' : '.')
      : 'nothing has been said in this browser yet: a page writes here as it works.';
  }
  logPanel.scrollTop = logPanel.scrollHeight;
}

function drawStatus(snapshot) {
  records = Array.isArray(snapshot.jobs) ? snapshot.jobs : [];
  lastSnapshot = snapshot;
  drawTotals(snapshot.summary || {});
  drawMachine(snapshot.system || {}, (snapshot.summary || {}), (snapshot.history || {}), (snapshot.jobs || []));
  drawModelCards(snapshot);
  drawKeyState();
  drawLog();
}

// A read that throws is not always a missing machine: these doors answer
// nothing at all once a visit has been refused a few times in a row. These
// words name which of the two happened, using the page's own door question;
// a refused visit is never called a missing machine.
function offline(failed) {
  ui.headUnknown();
  const keyState = window.olsKeyState;
  // The doors that state what is asked of answer before any door refuses, so
  // any answer beyond unknown or checking means a machine stands here and this
  // read found its own doors closed.
  const served = keyState && keyState.state !== 'unknown' && keyState.state !== 'checking' ||
    ui.apiStatus === 401 || ui.apiStatus === 403;
  updated.textContent = served
    ? 'a machine is here: ' + keyState.text
    : 'not connected · retrying';
  totalsLine.textContent = '';
  sinceLine.textContent = 'nothing can be read.';
  machinePanel.textContent = '';
  const said = document.createElement('div');
  said.className = served ? 'log warn' : 'meta';
  said.textContent = served
    ? 'This machine stands here and went quiet: after a few refusals at its doors in a row it answers nothing at all, so a read can look like no machine. A quiet door opens again by itself once enough time passes without a new refusal.'
    : 'No server to read. Start it with run.bat on Windows or run.sh on Linux, then reload this page.';
  if (served) said.setAttribute('title',
    'After a few refusals in a row the doors answer nothing, so a read can look like no machine. A quiet door opens again by itself once enough time passes without a new refusal.');
  machinePanel.appendChild(said);
  cardsHost.textContent = '';
  const none = document.createElement('div');
  none.className = 'meta';
  none.textContent = served
    ? 'Nothing is drawn while this page is refused at the doors.'
    : 'No server to read, so no model has a card.';
  cardsHost.appendChild(none);
  drawKeyState();
}

// Where the key stands at this page: the answer of the page's own auth question,
// kept by the key shim, shown beside the row that takes the key. A server that
// refused a key is never presented as a missing server.
function drawKeyState() {
  if (!keyStateLine) return;
  const state = window.olsKeyState;
  if (!state) {
    keyStateLine.textContent = 'This page cannot ask about the key.';
    keyStateLine.className = 'meta';
    return;
  }
  keyStateLine.textContent = 'at the doors: ' + state.text;
  keyStateLine.className = state.state === 'accepted' ? 'meta-ok' : 'meta';
  // A visit standing without a credential is one press from asking again: the
  // page's own prompt, not a walk to some other address. The hand is wired
  // once, however many times this line is redrawn.
  const stuck = state.state === 'held' || state.state === 'refused';
  if (stuck && window.olsKey) {
    keyStateLine.textContent += ' · press to hand the key';
    keyStateLine.title = 'this machine asks for its key: press and the page asks you for it';
    keyStateLine.className = 'meta-press';
    if (!pressWired) {
      pressWired = true;
      ui.onPress(keyStateLine, function () { void window.olsKey.ask(); });
    }
  } else {
    keyStateLine.removeAttribute('title');
  }
}

// The quant a model's weights are kept at, read off the file names of the files
// that model stands on: the name of a weight file is the fact here, and the
// tensors inside it are not read. A name that does not say is named as such.
function quantWords(files) {
  const named = [];
  for (const file of files) {
    const name = String(file);
    const match = name.match(/-((?:Q|B|F)[A-Za-z0-9_]+)\.(?:gguf|safetensors|bin|model\.bin)/i);
    if (match) named.push(match[1]);
  }
  if (!named.length) return 'not stated by these file names';
  return named.join(', ');
}

// The model rows a card reads its sizes from. The picture table is small, so it
// is read whole and kept for as long as the server says it is fresh. The voice
// catalog is four hundred rows and is never read whole: a card here names one
// model, and that model is asked for by its own name — one small page per card,
// replayed from this browser's cache while the server still calls that page fresh.
async function readModelRows(snapshot) {
  const rows = [];
  // a table that did not answer is not a table that says nothing: keeping the
  // rows the last complete reading collected is the only honest option, since a
  // card whose model is missing from these rows reads its weights as none.
  let complete = true;
  // A refused answer is one missing table, not a reading that stops: each request
  // answers on its own, and the page decides from all of them together.
  const askOne = async function (path) {
    try {
      return await ui.api(path);
    } catch (e) {
      return null;
    }
  };
  const paths = ['/api/models'];
  const wanted = [];
  const asked = {};
  for (const entry of (snapshot && Array.isArray(snapshot.models)) ? snapshot.models : []) {
    if (entry.engine !== 'voice') continue;
    const id = String(entry.model || '');
    if (!id || asked[id]) continue;
    asked[id] = true;
    wanted.push('/voice/api/model-cards?q=' + encodeURIComponent(id) + '&limit=4');
  }
  const answers = [];
  for (const answer of await Promise.all(paths.concat(wanted).map(askOne))) answers.push(answer);
  for (let at = 0; at < paths.length; ++at) {
    const table = answers[at];
    if (!table) { complete = false; continue; }
    if (Array.isArray(table)) rows.push.apply(rows, table);
    else if (Array.isArray(table.models)) rows.push.apply(rows, table.models);
  }
  for (let at = paths.length; at < answers.length; ++at) {
    const page = answers[at];
    if (!page || !Array.isArray(page.rows)) { complete = false; continue; }
    rows.push.apply(rows, page.rows);
  }
  if (!complete) return;
  modelRows = rows;
}

// What a read would draw, apart from the moments its readings were taken at: two
// reads whose numbers agree would draw the same wall, the same cards, and the
// same lines - so the page keeps what it drew instead of drawing over it. A
// reader's own choices in a table live in nodes kept between drawings, so they
// survive whatever is drawn again.
function wallOf(snapshot) {
  const history = snapshot.history || {};
  const samples = (Array.isArray(history.samples) ? history.samples : []).map((sample) =>
    JSON.stringify([sample.p || [], sample.ram || [], sample.m || [], sample.open,
    sample.bytes, sample.run, sample.disk || [], sample.load, sample.th || []]));
  return JSON.stringify([history.devices || [], history.models || [], samples,
  snapshot.summary || {}, snapshot.jobs || [], snapshot.system || {},
  Array.isArray(snapshot.models) ? snapshot.models : []]);
}

let lastWallDrawn = '';

async function refresh() {
  const began = Date.now();
  const snapshot = await ui.api('/api/monitor');
  const took = Date.now() - began;
  if (!snapshot) {
    if (ui.apiStatus === 401 || ui.apiStatus === 403) throw new Error('Key refused here (' + ui.apiStatus + ')');
    throw new Error('monitor returned no answer');
  }
  const wall = wallOf(snapshot);
  if (wall !== lastWallDrawn) {
    lastWallDrawn = wall;
    pageReads.push({ t: began, ms: took });
    while (pageReads.length > 60)
      pageReads.shift();
    // The header first: the rows below are read one request per model this page
    // names, so a header painted after them would lag the machine by that reading
    // while the machine next to it is plainly running.
    paintMachineHeader(snapshot);
    await readModelRows(snapshot);
    drawStatus(snapshot);
  }
  // The record of what the pages said is kept in this browser, so it is asked
  // about even on a read that drew nothing: it says nothing when nothing changed.
  drawLog();
}

// ---------- the whole machine, in one reading ----------
async function main() {
  // The doors' own line and the log are drawn before any reading arrives, so
  // the page speaks of where it stands even while the machine is still
  // answering.
  drawKeyState();
  // The record of what the pages said is kept in this browser, so it is drawn
  // whether or not there is a machine to read right now: the lines a page wrote
  // before a server stopped are read again here.
  drawLog();
  if (logClear) {
    ui.onPress(logClear, function () {
      ui.logClear();
      logStamp = null;
      drawLog();
    });
  }
  // The two engine marks beside the panel's name: pressing one takes its
  // engine's cards off the page or draws them again, from the reading the page
  // already holds - the answer to the touch is at once.
  for (const toggle of modelToggles) {
    if (!toggle.node) continue;
    ui.onPress(toggle.node, function () {
      engineShown[toggle.engine] = !engineShown[toggle.engine];
      drawEngineMarks();
      if (lastSnapshot) drawModelCards(lastSnapshot);
    });
  }
  drawEngineMarks();
  const kept = ui.lastReading('/api/monitor');
  if (kept && kept.data) {
    paintMachineHeader(kept.data);
    await readModelRows(kept.data);
    drawStatus(kept.data);
  }
  // The home page is the machine's own state, and the machine keeps that state
  // fresh for two seconds: the page reads at that pace, so every reading it asks
  // for is a real one and none is served from a cache it already aged out of.
  const kStatusPollMs = 2000;
  await ui.poll(refresh, offline, kStatusPollMs);
}

main();
