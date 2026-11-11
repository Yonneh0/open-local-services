// Config panel: a topbar button opens a floating panel listing the server's
// .env options. Typing a value into a row and pressing Enter sends it to the
// server, which rewrites the .env file in place. Secret rows answer with
// "set" or "not set" only - the words are never shown here - and the api key
// is what every door of this machine answers to, so the row re-asks the doors
// and speaks from the newest answer.
(function () {
  const topbar = window.ols.E('topbar');
  if (!topbar) return;

  let btn = window.ols.E('config-options-btn');
  if (!btn) {
    btn = document.createElement('button');
    btn.id = 'config-options-btn';
    btn.className = 'config-btn';
    btn.textContent = 'config options';
    btn.setAttribute('title', 'Show or hide the settings this server keeps in its .env');
    btn.setAttribute('aria-expanded', 'false');
    topbar.appendChild(btn);
  }

  const panel = document.createElement('div');
  panel.id = 'config-panel';
  panel.className = 'config-panel';
  topbar.appendChild(panel);

  let options = [];
  // What the machine does about starting this server by itself: read from the
  // machine's own manager, changed by the hands under the rows below, and never
  // guessed at from what a .env line happens to say.
  let service = null;
  let serviceBusy = false;

  let noteText = '';
  // When the last reading was taken, so a panel that has been open for a while
  // can say so rather than pass its rows off as current.
  let readAt = 0;
  // Opening the panel is the moment a person reads these values, so the panel
  // asks again then - and only then the machine would answer differently: the
  // window a reading may be reused for is the machine's own, stated with the
  // answer and kept in the store, so this panel keeps no number of its own.
  let reading = false;

  function describe(option) {
    if (option.secret) return option.value === 'unset' ? 'not set' : 'set';
    return option.value === '' ? 'not set' : option.value;
  }

  function isOpen() {
    return panel.className.indexOf('config-open') >= 0;
  }

  function openPanel() {
    panel.className = 'config-panel config-open';
    const rect = btn.getBoundingClientRect();
    const wide = Math.min(420, window.innerWidth * 0.92);
    panel.style.top = (rect.bottom + 6) + 'px';
    panel.style.left = Math.max(8, Math.min(rect.left - 10, window.innerWidth - wide - 12)) + 'px';
    btn.setAttribute('aria-expanded', 'true');
    freshRows();
    freshService();
    freshPages();
  }

  function closePanel() {
    panel.className = 'config-panel';
    btn.setAttribute('aria-expanded', 'false');
  }

  window.ols.onPress(btn, function () {
    if (isOpen()) closePanel();
    else openPanel();
  });

  // The pages this server carries inside can also be taken out and put beside
  // it, which is what a machine that serves from inside its own exe needs: the
  // hand asks the server to write them into a webui folder, and the outside
  // copies are served from then on. A folder that already holds pages is left
  // exactly as it is - nothing here overwrites what is already outside.
  //
  // This is a setting of the server rather than a thing a person presses on the
  // way past it, so it stands in the panel with the other settings: the header
  // carries one button, and it carried two that looked alike.
  let pages = null;

  function pagesWords() {
    if (!pages) return 'the machine has not said yet where its pages come from';
    return pages.words || (pages.outside
      ? 'the pages served are the copies in ' + (pages.folder || 'webui') + ' beside this server'
      : 'the pages served are the copies carried inside this binary');
  }

  function drawPages() {
    const heading = document.createElement('span');
    heading.className = 'config-lead';
    heading.textContent = 'The pages it serves';
    panel.appendChild(heading);

    const line = document.createElement('span');
    line.className = 'config-lead';
    line.textContent = pagesWords();
    line.setAttribute('title',
      'Every page, script, sheet and icon is cut into the binary when it is built, so a release started ' +
      'beside nothing still answers with them. Writing them out puts a copy in a folder a person can ' +
      'read and edit, and that folder is what is served from then on.');
    panel.appendChild(line);

    if (pages && pages.outside === false) {
      const acts = document.createElement('div');
      acts.className = 'config-row';
      const hand = document.createElement('button');
      hand.className = 'config-btn';
      hand.textContent = 'write the pages out beside the server';
      hand.setAttribute('title',
        'Write the pages this server carries inside into ' + (pages.folder || 'webui') +
        ', and serve those copies from then on. A folder that already holds pages is left as it is.');
      window.ols.onPress(hand, function () {
        void (async function () {
          const answer = await window.ols.api('/api/webui/expand', 'POST', {});
          if (!answer)
            say('Not expanded: ' + (window.ols.apiError || 'the server gave no reason'));
          else if (answer.expanded)
            say('Expanded: ' + ((answer.files || []).length) + ' page files written into ' +
              (answer.folder || 'webui') + ' · ' + (answer.bytes || 0) +
              ' bytes · the outside copies are the ones served now');
          else
            say('Not expanded: ' + (answer.error || 'the server gave no reason'));
          freshPages();
        })();
      });
      acts.appendChild(hand);
      panel.appendChild(acts);
    }
  }

  function freshPages() {
    void (async function () {
      const answer = await window.ols.api('/api/webui/expand');
      if (answer && typeof answer.outside === 'boolean') pages = answer;
      if (isOpen()) drawRows();
    })();
  }

  function serviceWords() {
    if (!service)
      return 'the machine has not answered yet about starting this server by itself';
    return (service.installed
      ? service.name + ' is set up on this machine (' + service.manager + '), ' +
      service.startup + (service.running ? ', running now' : ', not running')
      : service.name + ' is not set up on this machine' +
      (service.manager ? ' (' + service.manager + ' says nothing)' : '')
    ) + ' · started from ' + (service.root || 'where it stands') +
      ' · last ask: ' + (service.last_words || 'nothing has been asked');
  }

  function askService(action) {
    if (serviceBusy) return;
    serviceBusy = true;
    say('asking the machine: ' + action + '…');
    void (async function () {
      const answer = await window.ols.api('/api/service', 'POST', { action: action });
      serviceBusy = false;
      if (!answer) {
        say('The machine refused: ' + (window.ols.apiError || 'it gave no reason'));
      } else {
        if (answer.service) service = answer.service;
        if (action === 'shutdown') {
          say('This server is stopping now' + (answer.words ? ' — ' + answer.words : '') +
            '. This page will stop answering; open it again once the machine has started it.');
        } else {
          say((answer.did ? 'Done: ' : 'Not done: ') + (answer.words || 'the machine said nothing') +
            (answer.env_words ? ' · ' + answer.env_words : ''));
        }
      }
      freshService();
    })();
  }

  function freshService() {
    void (async function () {
      const state = await window.ols.api('/api/service');
      if (state && state.startup) service = state;
      if (isOpen()) drawRows();
    })();
  }

  function serviceHand(acts, label, title, action) {
    const hand = document.createElement('button');
    hand.className = 'config-btn';
    hand.textContent = label;
    hand.disabled = serviceBusy;
    hand.setAttribute('title', title);
    window.ols.onPress(hand, function () { askService(action); });
    acts.appendChild(hand);
  }

  function drawService() {
    const heading = document.createElement('span');
    heading.className = 'config-lead';
    heading.textContent = 'Starting by itself';
    panel.appendChild(heading);

    const line = document.createElement('span');
    line.className = 'config-lead';
    line.textContent = serviceWords();
    line.setAttribute('title',
      'What this machine\'s own manager says about ' + ((service && service.name) || 'this server') +
      '. Nothing here installs a server: it is this program, out of this folder, that the machine is' +
      ' told to start.' +
      (((service && service.platform) === 'windows')
        ? ' A console program cannot be carried by the service control manager, so on Windows this is' +
        ' a task the task scheduler starts when the machine starts.'
        : ''));
    panel.appendChild(line);

    const acts = document.createElement('div');
    acts.className = 'config-row';
    const atBoot = !!service && service.startup === 'automatic';
    serviceHand(acts, atBoot ? 'do not start at boot' : 'set up to start at boot',
      atBoot
        ? 'This machine is told to leave this server alone when it starts. A server already running is' +
        ' left running: nothing here stops a working server.'
        : 'This server, out of this folder, is set to start when the machine starts, and is started now' +
        ' if nothing is running. The folder it was set up in is the folder it starts from.',
      atBoot ? 'startup_off' : 'startup_on');
    if (service && service.installed) {
      serviceHand(acts, 'remove service',
        'Take ' + service.name + ' off this machine\'s list of things to start' +
        (service.platform === 'windows' ? ' (sc delete, or the task scheduler task deleted)'
          : ' (systemctl disable, and its unit file taken away where this server wrote it)') +
        '. A server already running is left running.',
        'remove');
    }
    if (service && service.running) {
      serviceHand(acts, 'shutdown service',
        'Stop this server: the machine\'s manager is asked to stop it and this process lets go of its' +
        ' listening port. This page stops answering the moment it is pressed.',
        'shutdown');
    }
    panel.appendChild(acts);
  }

  function say(text) {
    noteText = text;
    drawRows();
  }

  function drawRows() {
    panel.textContent = '';
    const lead = document.createElement('span');
    lead.className = 'config-lead';
    lead.textContent = 'Server settings (.env) — select a row, type a value, press Enter' +
      (readAt ? ' · read at ' + window.ols.clockMs(readAt) : ' · server not reached yet');
    lead.setAttribute('title',
      'Editing a value rewrites the .env file immediately; settings that only apply at the next ' +
      'start say so' +
      (readAt ? '; these values were read at ' + window.ols.clockMs(readAt) : ''));
    panel.appendChild(lead);

    for (const option of options) {
      const authRow = option.name === 'OLS_API_KEY';
      const row = document.createElement('div');
      row.className = 'config-row';
      row.setAttribute('title',
        'Edit ' + option.name + ': type the new value and press Enter' +
        (option.live ? '' : ' — applies at the next start') +
        (authRow ? '; this api key is what every door of this machine answers to - the rooms open only with it, handed at the door page' : ''));
      const words = document.createElement('span');
      words.textContent = option.name + ' — ' + describe(option);
      if (authRow && window.olsKeyState) words.textContent += ' · ' + window.olsKeyState.text;
      // What this browser was given at the doors, so a reader sees whether a
      // credential they used before is one this machine would accept again.
      if (authRow && window.olsKey) {
        const kept = window.olsKey.remembered();
        if (kept.length > 1) words.textContent += ' · this browser was given ' + kept.length + ' forms at this address';
      }
      row.appendChild(words);

      const field = document.createElement('input');
      field.type = 'text';
      // A path or a token is as long as it is: the cap is only what stops a
      // value from being carried whole, so it is set far above any real setting -
      // above a whole key, which is what a reader may paste.
      field.maxLength = 4096;
      field.placeholder = 'New value, then Enter';
      field.setAttribute('aria-label', authRow ?
        'new api key for this machine - the rooms are opened at the door page, not here' : 'new value for ' + option.name);
      field.addEventListener('keydown', function (event) {
        if (event.key !== 'Enter') return;
        window.ols.flash(field);
        void (async function () {
          // Trim before sending, so the value the server keeps is exactly
          // what was typed.
          const wanted = field.value.trim();
          const state = await window.ols.api('/api/config', 'POST', { [option.name]: wanted });
          if (!state) {
            say(window.ols.apiError || 'Refused: ' + option.name + ' was not accepted, and the server gave no reason');
            return;
          }
          options = state.options || options;
          const updated = options.find(function (o) { return o.name === option.name; });
          if (!updated) {
            say('Refused: ' + option.name + ' was not accepted, and the answer did not carry it back');
            return;
          }
          readAt = Date.now();
          // The row speaks from the newest answer, so the doors are re-asked
          // after this auth setting moves: what the doors answer to is exactly
          // what they ask of. No browser credential changes here - that is
          // handed at the door page, never typed into a row.
          if (authRow && window.olsKey) await window.olsKey.check();
          say('Saved: ' + option.name + ' — ' + describe(updated) +
            (updated.live ? '' : ' (applies at the next start)'));
        })();
      });
      row.appendChild(field);
      panel.appendChild(row);
    }

    drawService();
    drawPages();

    if (noteText) {
      const noteLine = document.createElement('span');
      noteLine.className = 'config-lead';
      noteLine.textContent = noteText;
      panel.appendChild(noteLine);
    }
  }

  // Ask the machine for the settings again, unless the machine still answers
  // this ask the same way as the copy on the panel, or an ask is already out
  // standing.
  function freshRows() {
    if (reading) return;
    const kept = window.ols.lastReading('/api/config');
    if (kept && kept.live && options.length) return;
    reading = true;
    void (async function () {
      const state = (await window.ols.api('/api/config')) || {};
      reading = false;
      if (!Array.isArray(state.options)) {
        say('The machine gave no settings: ' + (window.ols.apiError || 'no reason'));
        return;
      }
      options = state.options;
      readAt = Date.now();
      noteText = '';
      drawRows();
    })();
  }

  void (async function () {
    // Paint rows from the last kept reading first so the panel shows content
    // immediately, then replace it with the live read.
    const kept = window.ols.lastReading('/api/config');
    if (kept && kept.data && Array.isArray(kept.data.options)) {
      options = kept.data.options;
      readAt = Number(kept.at);
      noteText = '';
      drawRows();
    }
    freshRows();
  })();
})();
