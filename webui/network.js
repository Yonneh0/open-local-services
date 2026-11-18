// Network panel: one document read from /api/network paints the machine's
// links, wifi and doors, and one door (/api/network, posted) works
// them. Every number here is the machine's own: rates come from the counter
// pairs the server sampled, uptimes from the moments it saw a link change,
// and every change answers with the machine's own tools' words - netsh on
// Windows, netplan and the rest on Linux - never with a guess about what
// probably happened. Controls the machine cannot honour are drawn turned off
// beside the reason it gave, because a door that refuses honestly is worth
// more than a button that lies.
(function () {
  const ui = window.ols;
  const E = ui.E;

  // The hosts this panel fills, and the two lines that state its state.
  const line = E('net-line');
  const linksChip = E('net-chip-links');
  const ratesChip = E('net-chip-rates');
  const wifiChip = E('net-chip-wifi');
  const doorsChip = E('net-chip-doors');
  const adaptersHost = E('net-adapters');
  const wifiHost = E('net-wifi');
  const settingsHost = E('net-settings');
  const scanHand = E('net-scan-hand');
  if (!line || !adaptersHost) return; // the page has no network panel

  let snapshot = null;    // the last whole document read
  let scanDoc = null;     // the last scan the radio answered (or the one kept)
  let busy = false;       // one change at a time, as the server keeps them
  let scanning = false;
  let firstScanAsked = false;
  // What a person typed into a form survives a redraw: the values live here,
  // keyed by adapter or section, and the inputs are the window onto them.
  const formMemory = {};

  // ---------- words for numbers ----------
  // Rate words keep two figures where they help: the byte count runs with a
  // unit of its own, and nothing is padded out to a zero the machine never
  // stated.
  function rateText(bytesPerSecond) {
    const value = Number(bytesPerSecond);
    if (!Number.isFinite(value) || value < 0) return 'not stated';
    return ui.sizeText(Math.round(value)) + '/s';
  }

  function ppsText(pps) {
    const value = Number(pps);
    if (!Number.isFinite(value) || value < 0) return 'not stated';
    return Math.round(value).toLocaleString() + ' pkt/s';
  }

  // A span of time in the fewest words that still say it: hours when there
  // are hours, minutes when there are not.
  function heldText(seconds) {
    const s = Math.max(0, Math.round(Number(seconds) || 0));
    if (s < 60) return s + 's';
    const m = Math.floor(s / 60);
    if (m < 60) return m + 'm ' + ui.pad2(s % 60) + 's';
    const h = Math.floor(m / 60);
    if (h < 48) return h + 'h ' + ui.pad2(m % 60) + 'm';
    return Math.floor(h / 24) + 'd ' + (h % 24) + 'h';
  }

  function speedText(bps) {
    const value = Number(bps);
    if (!Number.isFinite(value) || value <= 0) return '';
    if (value >= 1e9) return (value / 1e9).toFixed(value % 1e9 ? 1 : 0) + ' Gbps';
    if (value >= 1e6) return Math.round(value / 1e6) + ' Mbps';
    return Math.round(value / 1e3) + ' kbps';
  }

  // Five bars carry a wifi signal: the shape reads across the scan table
  // faster than the percent beside it, and the percent answers when the
  // bars all look alike.
  function signalBars(percent) {
    const box = document.createElement('span');
    box.className = 'net-bars';
    const pct = Number(percent);
    const lit = !Number.isFinite(pct) || pct < 0 ? 0 : Math.max(1, Math.round(pct / 20));
    for (let at = 1; at <= 5; ++at) {
      const bar = document.createElement('i');
      if (at <= lit) bar.className = 'lit' + (lit >= 4 ? ' strong' : lit === 3 ? ' fair' : '');
      box.appendChild(bar);
    }
    return box;
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

  // ---------- the one working door ----------
  // Every change is one post; the answer's own words go to the log, the
  // toast, and - because the machine knows best now - straight into a fresh
  // read.
  async function ask(body, doingWords) {
    if (busy) {
      ui.toastPush('one change at a time: the last one is still with the machine', 'warn');
      return null;
    }
    busy = true;
    ui.log('net: ' + doingWords, 'wait');
    let answer = null;
    try {
      answer = await ui.api('/api/network', 'POST', body);
    } finally {
      busy = false;
    }
    if (!answer) {
      const why = ui.apiError || ('the door said ' + ui.apiStatus);
      ui.log('net: ' + doingWords + ' - the door did not answer: ' + why, 'err');
      ui.toastPush(oneLiner(why, 160), 'err');
      return null;
    }
    const words = String(answer.words || 'the machine said nothing');
    ui.log('net: ' + words, answer.did ? 'ok' : 'err');
    ui.toastPush(oneLiner(words, 180), answer.did ? 'ok' : 'err', 12);
    refresh().catch(function () {});
    return answer;
  }

  // A hand for a change: it reads turned-off whenever a change is in flight
  // or the server cannot act, and says why instead of pretending.
  function hand(words, kind, tip, handler, disabled, disabledWhy) {
    const button = el('button', 'pill net-hand' + (kind ? ' net-' + kind : ''), words);
    button.title = disabled ? disabledWhy : tip;
    if (disabled) button.disabled = true;
    else ui.onPress(button, function () {
      handler();
    });
    return button;
  }

  function canChange(doc) {
    if (!doc) return { yes: false, why: 'the machine has not answered yet' };
    if (doc.elevated) return { yes: true, why: '' };
    return {
      yes: false,
      why: 'this server runs as a plain person: changing a setting belongs to the ' +
           "machine's manager",
    };
  }

  // A field that remembers: the value lives in formMemory under key, and a
  // person mid-typing is never overwritten by a redraw.
  function field(host, key, labelWords, placeholder, isNumber, secret) {
    const wrap = el('label', 'net-field');
    wrap.appendChild(el('span', 'net-field-name', labelWords));
    const input = el('input', 'net-input');
    input.type = isNumber ? 'number' : secret ? 'password' : 'text';
    if (placeholder !== undefined) input.placeholder = placeholder;
    if (formMemory[key] !== undefined) input.value = formMemory[key];
    input.addEventListener('input', function () {
      formMemory[key] = input.value;
    });
    wrap.appendChild(input);
    host.appendChild(wrap);
    return input;
  }

  function choose(host, key, labelWords, options, title) {
    const wrap = el('label', 'net-field');
    wrap.appendChild(el('span', 'net-field-name', labelWords));
    const select = el('select', 'net-input');
    if (title) select.title = title;
    for (const option of options) {
      const item = el('option', null, option[1]);
      item.value = String(option[0]);
      select.appendChild(item);
    }
    if (formMemory[key] !== undefined) select.value = String(formMemory[key]);
    select.addEventListener('change', function () {
      formMemory[key] = select.value;
    });
    wrap.appendChild(select);
    host.appendChild(wrap);
    return select;
  }

  // ---------- adapter cards ----------
  // The card set is rebuilt only when its shape changes - an adapter came,
  // went, or changed state - so a person typing in a form is never standing
  // on a card being redrawn under them. Numbers inside the cards move every
  // read.
  let adapterShapeKey = '';
  const adapterRefs = {};

  function adapterShape(adapters) {
    return adapters.map(function (adapter) {
      return String(adapter.name) + '' + (adapter.admin ? 'a' : 'A') +
             String(adapter.oper) + '' + (snapshot && snapshot.elevated ? 'e' : 'x');
    }).join('');
  }

  function kindChip(kind) {
    const words = { ethernet: 'ethernet', wifi: 'wifi', bond: 'bond', vlan: 'vlan',
                    bridge: 'bridge', tunnel: 'tunnel', virtual: 'virtual',
                    loopback: 'loopback' };
    return el('span', 'net-kind net-kind-' + kind, words[kind] || kind || 'link');
  }

  function drawAdapters(doc) {
    const adapters = Array.isArray(doc.adapters) ? doc.adapters : [];
    const shape = adapterShape(adapters);
    if (shape !== adapterShapeKey) {
      adapterShapeKey = shape;
      adaptersHost.innerHTML = '';
      for (const name in adapterRefs) delete adapterRefs[name];
      for (const adapter of adapters) adaptersHost.appendChild(buildAdapterCard(adapter));
    }
    // The numbers, whether the card is new or stands from the last read.
    for (const adapter of adapters) updateAdapterCard(adapter);
  }

  function buildAdapterCard(adapter) {
    const name = String(adapter.name);
    const card = el('div', 'net-card net-card-' + String(adapter.kind || 'link'));
    const refs = { rows: {} };
    adapterRefs[name] = refs;

    const head = el('div', 'net-head');
    // A dot that breathes while the link is up: the state reads at a glance,
    // and the breath says the page is reading it live, not holding a photo.
    refs.dot = el('span', 'net-dot ' + (adapter.oper === 'up' ? 'up' : adapter.oper === 'down' ? 'down' : 'other'));
    head.appendChild(refs.dot);
    head.appendChild(el('span', 'net-card-name', name));
    head.appendChild(kindChip(adapter.kind));
    refs.oper = el('span', 'net-state', adapter.admin ? String(adapter.oper) : 'disabled by the machine');
    head.appendChild(refs.oper);
    if (adapter.managed_by_this_server)
      head.appendChild(el('span', 'net-tag', 'made by this server'));
    if (adapter.master) head.appendChild(el('span', 'net-tag', 'part of ' + adapter.master));
    if (Array.isArray(adapter.slaves) && adapter.slaves.length)
      head.appendChild(el('span', 'net-tag', 'holds ' + adapter.slaves.join(', ')));
    if (adapter.vlan && adapter.vlan.id !== undefined)
      head.appendChild(el('span', 'net-tag', 'vlan ' + adapter.vlan.id + ' on ' + (adapter.vlan.parent || '?')));
    card.appendChild(head);
    if (adapter.description) card.appendChild(el('div', 'meta', oneLiner(adapter.description, 120), adapter.description));

    // The rate figure: rx filled under its line, tx a line beside it, both on
    // one scale, across the four minutes this machine remembers.
    const rateBox = el('div', 'net-rate');
    refs.rateWords = el('div', 'net-rate-words');
    refs.chart = el('div', 'net-chart');
    rateBox.appendChild(refs.rateWords);
    rateBox.appendChild(refs.chart);
    card.appendChild(rateBox);

    // The facts, in the small monospace voice the other panels use for
    // machine-read words.
    const facts = el('div', 'net-facts');
    refs.rows.ip = el('div', 'net-fact');
    refs.rows.link = el('div', 'net-fact');
    refs.rows.dns = el('div', 'net-fact');
    refs.rows.stats = el('div', 'net-fact');
    facts.appendChild(refs.rows.ip);
    facts.appendChild(refs.rows.link);
    facts.appendChild(refs.rows.dns);
    facts.appendChild(refs.rows.stats);
    card.appendChild(facts);

    // The forms, folded: nobody sees them until a setting is being changed.
    const gate = canChange(snapshot);
    const forms = el('details', 'net-form');
    forms.appendChild(el('summary', null, 'change this link'));
    const formBody = el('div', 'net-form-body');

    const powerRow = el('div', 'net-row');
    const linkDown = adapter.admin === false || adapter.oper !== 'up';
    powerRow.appendChild(linkDown
      ? hand('bring up', 'ok', 'the machine puts the link back in service',
             function () { ask({ action: 'adapter_up', name: name }, 'asking the machine to bring ' + name + ' up'); },
             !gate.yes, gate.why)
      : hand('take down', 'err', 'the machine takes the link out of service - nothing flows until it is brought back',
             function () {
               if (!window.confirm('Take ' + name + ' down? Everything running through it stops.')) return;
               ask({ action: 'adapter_down', name: name }, 'asking the machine to take ' + name + ' down');
             },
             !gate.yes, gate.why));
    formBody.appendChild(powerRow);

    const mtuForm = el('div', 'net-row');
    field(mtuForm, 'mtu:' + name, 'mtu', '1500', true);
    mtuForm.appendChild(hand('set mtu', null, 'the machine sets the frame size; 1280 to 65535', function () {
      const value = Number(formMemory['mtu:' + name] || 0);
      ask({ action: 'mtu_set', name: name, mtu: value }, 'asking for mtu ' + value + ' on ' + name);
    }, !gate.yes, gate.why));
    formBody.appendChild(mtuForm);

    const ipForm = el('div', 'net-row net-row-form');
    const firstV4 = Array.isArray(adapter.ipv4) && adapter.ipv4.length ? adapter.ipv4[0] : null;
    if (formMemory['ipmode:' + name] === undefined)
      formMemory['ipmode:' + name] = firstV4 && adapter.dhcp !== false ? 'dhcp' : (firstV4 ? 'static' : 'dhcp');
    if (formMemory['addr:' + name] === undefined && firstV4)
      formMemory['addr:' + name] = String(firstV4.address);
    if (formMemory['prefix:' + name] === undefined && firstV4 && Number(firstV4.prefix) > 0)
      formMemory['prefix:' + name] = String(firstV4.prefix);
    if (formMemory['gw:' + name] === undefined && adapter.gateway)
      formMemory['gw:' + name] = String(adapter.gateway);
    if (formMemory['dns:' + name] === undefined && Array.isArray(adapter.dns))
      formMemory['dns:' + name] = adapter.dns.join(' ');
    const mode = choose(ipForm, 'ipmode:' + name, 'addresses by', [
      ['dhcp', 'the router hands them (dhcp)'],
      ['static', 'I state them (static)'],
    ], 'dhcp lets the machine hand out addresses; static keeps the ones stated here. On Linux this writes the netplan file this server owns and applies it.');
    const addr = field(ipForm, 'addr:' + name, 'address', '192.168.1.50');
    const prefix = field(ipForm, 'prefix:' + name, 'prefix', '24', true);
    const gateway = field(ipForm, 'gw:' + name, 'gateway', '192.168.1.1');
    const dns = field(ipForm, 'dns:' + name, 'dns servers', 'space separated');
    const metric = field(ipForm, 'metric:' + name, 'metric', 'leave blank to leave alone', true);
    addr.addEventListener('change', function () {
      if (/^[0-9a-fA-F:]+$/.test(String(addr.value)) && addr.value.indexOf(':') >= 0 && formMemory['prefix:' + name] === '24')
        formMemory['prefix:' + name] = '64';
    });
    mode.addEventListener('change', function () {});
    ipForm.appendChild(hand('apply addresses', null, 'one by one, the machine applies these; anything it refuses comes back in its own words', function () {
      const body = {
        action: 'ip_set',
        name: name,
        mode: String(mode.value),
        address: String(addr.value || '').trim(),
        prefix: Number(prefix.value || 0),
        gateway: String(gateway.value || '').trim(),
        dns: String(dns.value || '').split(/\s+/).filter(Boolean),
      };
      const metricValue = Number(metric.value || -1);
      if (Number.isFinite(metricValue) && metricValue >= 0) body.metric = metricValue;
      ask(body, 'setting the addresses of ' + name + (body.mode === 'dhcp' ? ' back to dhcp' : ' to ' + body.address));
    }, !gate.yes, gate.why));
    formBody.appendChild(ipForm);
    if (snapshot && snapshot.platform !== 'windows')
      formBody.appendChild(el('div', 'meta', 'on Linux these words are written to /etc/netplan/70-open-local-services.yaml, checked with netplan generate, and applied; a file generate refuses is rolled back untouched.'));
    forms.appendChild(formBody);
    card.appendChild(forms);
    return card;
  }

  function fact(ref, label, value) {
    ref.textContent = '';
    ref.appendChild(el('span', 'net-fact-name', label));
    ref.appendChild(el('span', null, value || 'not stated'));
  }

  function updateAdapterCard(adapter) {
    const refs = adapterRefs[String(adapter.name)];
    if (!refs) return;
    const upNow = adapter.oper === 'up';
    refs.dot.className = 'net-dot ' + (upNow ? 'up' : adapter.oper === 'down' ? 'down' : 'other');
    refs.oper.textContent = adapter.admin ? String(adapter.oper || 'unknown') : 'disabled by the machine';

    const rate = adapter.rate || {};
    const history = adapter.history || {};
    const words = ['down ' + rateText(rate.rx), 'up ' + rateText(rate.tx)];
    if (Number(rate.avg_rx) > 0 || Number(rate.avg_tx) > 0)
      words.push('recent mean ' + rateText(rate.avg_rx) + ' / ' + rateText(rate.avg_tx));
    if (Number(rate.rx_pps) > 0) words.push(ppsText(rate.rx_pps));
    if (adapter.up_time)
      words.push(adapter.up_time.measured
        ? 'link up ' + heldText(adapter.up_time.held_s)
        : 'link up for at least ' + heldText(adapter.up_time.held_s) + ' (since ' + ui.clock(adapter.up_time.since) + ')');
    refs.rateWords.textContent = '';
    for (const word of words) refs.rateWords.appendChild(el('span', 'net-rate-word', word));

    // The figure: gaps stay gaps; a minute the sampler did not see this link
    // draws as a break, not a zero.
    const samples = [];
    const times = Array.isArray(history.t) ? history.t : [];
    for (let at = 0; at < times.length; ++at) {
      samples.push({
        t: Number(times[at]),
        rx: Array.isArray(history.rx) ? Number(history.rx[at]) : null,
        tx: Array.isArray(history.tx) ? Number(history.tx[at]) : null,
      });
    }
    refs.chart.innerHTML = '';
    if (samples.length) {
      drawTrace(refs.chart, [
        { points: traceOf(samples, function (s) { return s.rx >= 0 ? s.rx / 1024 : null; }), name: 'down', cls: 'a' },
        { points: traceOf(samples, function (s) { return s.tx >= 0 ? s.tx / 1024 : null; }), name: 'up', cls: 'b' },
      ], 'kB/s');
    } else {
      noFigure(refs.chart, 'no line yet',
        'The sampler reads this link every two seconds; the line starts once it has two readings.');
    }

    const ips = [];
    for (const item of (adapter.ipv4 || []))
      ips.push(String(item.address) + (Number(item.prefix) > 0 ? '/' + item.prefix : ''));
    for (const item of (adapter.ipv6 || []))
      ips.push(oneLiner(item.address, 32) + (Number(item.prefix) > 0 ? '/' + item.prefix : ''));
    fact(refs.rows.ip, 'addresses', ips.join('  ·  ') + (adapter.gateway ? '   gw ' + adapter.gateway : ''));
    const linkWords = [
      adapter.mac || 'no hardware address',
      Number(adapter.mtu) > 0 ? 'mtu ' + adapter.mtu : '',
      adapter.speed_bps > 0 ? speedText(adapter.speed_bps) : '',
      adapter.dhcp === null || adapter.dhcp === undefined ? 'dhcp: not stated' : adapter.dhcp ? 'dhcp on' : 'dhcp off',
      Number(adapter.metric) >= 0 ? 'metric ' + adapter.metric : '',
    ].filter(Boolean);
    fact(refs.rows.link, 'link', linkWords.join('  ·  '));
    const dnsWords = (adapter.dns || []).join('  ') + (adapter.dns_via_resolved ? ' (via systemd-resolved)' : '');
    fact(refs.rows.dns, 'dns', dnsWords || (adapter.dns_suffix ? 'suffix ' + adapter.dns_suffix : 'not stated'));
    const stats = adapter.stats || {};
    fact(refs.rows.stats, 'since boot',
         ui.sizeText(Number(stats.rx_bytes) || 0) + ' in · ' + ui.sizeText(Number(stats.tx_bytes) || 0) + ' out · ' +
         (Number(stats.rx_packets) || 0).toLocaleString() + '/' + (Number(stats.tx_packets) || 0).toLocaleString() + ' packets' +
         ((Number(stats.rx_errors) || 0) + (Number(stats.tx_errors) || 0)
           ? ' · ' + (Number(stats.rx_errors) || 0) + '/' + (Number(stats.tx_errors) || 0) + ' errors' : '') +
         ((Number(stats.rx_dropped) || 0) + (Number(stats.tx_dropped) || 0)
           ? ' · ' + (Number(stats.rx_dropped) || 0) + '/' + (Number(stats.tx_dropped) || 0) + ' dropped' : ''));
  }

  // ---------- wifi ----------
  let wifiShapeKey = '';

  function wifiShape(doc) {
    const list = (doc.wifi && Array.isArray(doc.wifi.interfaces)) ? doc.wifi.interfaces : [];
    const at = doc.scan ? String(doc.scan.at) : 'none';
    return list.map(function (item) { return String(item.name) + String(item.state) + String(item.ssid); }).join('')
      + '|' + ((doc.wifi && doc.wifi.blocked || []).join(','))
      + '|' + at + '|' + (doc.elevated ? 'e' : 'x')
      + '|' + (doc.ap ? String(doc.ap.supported) + String(doc.ap.active) + String((doc.ap.clients || []).length) : '-')
      + '|' + String((doc.features || {}).scan);
  }

  function wifiIfaceNames(doc) {
    const list = (doc.wifi && Array.isArray(doc.wifi.interfaces)) ? doc.wifi.interfaces : [];
    return list.map(function (item) { return item.name; });
  }

  function drawWifi(doc) {
    const shape = wifiShape(doc);
    if (shape !== wifiShapeKey) {
      wifiShapeKey = shape;
      wifiHost.innerHTML = '';
      wifiHost.appendChild(buildWifiBlock(doc));
    }
  }

  function buildWifiBlock(doc) {
    const block = el('div', 'net-section');
    const gate = canChange(doc);
    const features = doc.features || {};
    const ifaces = (doc.wifi && Array.isArray(doc.wifi.interfaces)) ? doc.wifi.interfaces : [];
    const blocked = ((doc.wifi && doc.wifi.blocked) || []).indexOf('wifi') >= 0;

    const titleRow = el('div', 'net-section-head');
    titleRow.appendChild(el('span', 'net-section-name', 'wifi'));
    if (blocked) titleRow.appendChild(el('span', 'chip err', 'radio blocked'));
    titleRow.appendChild(hand(blocked ? 'unblock radio' : 'block radio', blocked ? 'ok' : 'warn',
      doc.platform === 'windows'
        ? 'Windows has no radio switch: this turns the wifi adapter out of service, which is the nearest it comes'
        : 'rfkill switches the radio itself',
      function () {
        ask({ action: 'radio_block', on: !blocked }, blocked ? 'unblocking the radio' : 'blocking the radio');
      }, !gate.yes, gate.why));
    block.appendChild(titleRow);

    if (!features.wifi) {
      block.appendChild(el('div', 'meta', 'this machine has no wifi this server can reach' +
        (features.wifi_reason ? ': ' + features.wifi_reason : '')));
      return block;
    }
    if (!ifaces.length) {
      block.appendChild(el('div', 'meta', 'this machine has no wifi interface to report'));
      return block;
    }

    for (const item of ifaces) {
      const card = el('div', 'net-card');
      const head = el('div', 'net-head');
      head.appendChild(el('span', 'net-dot ' + (item.state === 'connected' || item.state === 'completed' ? 'up' : 'other')));
      head.appendChild(el('span', 'net-card-name', item.ssid || (item.state === 'connected' ? 'a network whose name the machine keeps to itself' : 'not connected')));
      head.appendChild(el('span', 'net-state', String(item.state || 'unknown') + (item.mode ? ' · ' + item.mode : '')));
      if (item.state === 'connected' || item.state === 'completed')
        head.appendChild(hand('leave this network', 'err', 'the radio lets go; a machine usually rejoins on its own',
          function () { ask({ action: 'wifi_disconnect', interface: item.name }, 'asking the radio to leave ' + (item.ssid || 'the network')); },
          !gate.yes, gate.why));
      card.appendChild(head);
      const parts = [];
      if (item.bssid) parts.push(item.bssid);
      if (item.band) parts.push(item.band + (Number(item.channel) >= 0 ? ' ch ' + item.channel : ''));
      if (Number(item.signal) >= 0) parts.push(item.signal + '%');
      if (Number(item.rssi) < 0) parts.push(item.rssi + ' dBm');
      if (Number(item.rx_rate) > 0) parts.push('rx ' + item.rx_rate + ' Mbps');
      if (Number(item.tx_rate) > 0) parts.push('tx ' + item.tx_rate + ' Mbps');
      if (item.auth) parts.push(item.auth + (item.cipher ? '/' + item.cipher : ''));
      if (item.radio) parts.push(String(item.radio));
      card.appendChild(el('div', 'net-fact', parts.join('  ·  ') || 'the machine says this interface exists and nothing more'));
      if (Number(item.signal) >= 0) {
        const signalRow = el('div', 'net-fact');
        signalRow.appendChild(signalBars(item.signal));
        signalRow.appendChild(el('span', null, item.signal + '%'));
        card.appendChild(signalRow);
      }
      block.appendChild(card);
    }

    // The join form: what a scan row does not fill by hand, a person types.
    const join = el('details', 'net-form net-join');
    join.appendChild(el('summary', null, 'join another network'));
    // A rescan rebuilds this block while a key may be half-typed: whether the
    // form was open is remembered with everything else a person was doing.
    if (formMemory['join:open']) join.open = true;
    join.addEventListener('toggle', function () {
      formMemory['join:open'] = join.open;
    });
    const joinBody = el('div', 'net-form-body');
    field(joinBody, 'join:ssid', 'network name', 'exactly as the scan lists it');
    field(joinBody, 'join:psk', 'key', 'leave blank for an open network', false, true);
    choose(joinBody, 'join:auth', 'security', [
      ['auto', 'let the machine tell'], ['wpa2', 'WPA2'], ['wpa3', 'WPA3'], ['wpa', 'WPA'], ['open', 'open'],
    ]);
    const ifacesList = wifiIfaceNames(doc);
    if (ifacesList.length > 1) choose(joinBody, 'join:iface', 'radio', ifacesList.map(function (n) { return [n, n]; }));
    const hiddenWrap = el('label', 'net-field');
    hiddenWrap.appendChild(el('span', 'net-field-name', 'hidden'));
    const hidden = el('input', 'net-input');
    hidden.type = 'checkbox';
    hiddenWrap.appendChild(hidden);
    joinBody.appendChild(hiddenWrap);
    joinBody.appendChild(hand('join', null, 'the machine joins; on Windows this writes a wifi profile, on Linux it asks NetworkManager',
      function () {
        const body = {
          action: 'wifi_connect',
          ssid: String(formMemory['join:ssid'] || ''),
          psk: String(formMemory['join:psk'] || ''),
          auth: String(formMemory['join:auth'] || 'auto'),
          hidden: !!hidden.checked,
        };
        if (formMemory['join:iface']) body.interface = String(formMemory['join:iface']);
        if (!body.ssid) { ui.toastPush('a network has to be named to join it', 'warn'); return; }
        ask(body, 'asking the radio to join ' + body.ssid);
      }, !gate.yes, gate.why));
    join.appendChild(joinBody);
    block.appendChild(join);

    block.appendChild(buildScanBlock(doc, gate));
    block.appendChild(buildAccessPoint(doc, gate));
    return block;
  }

  function buildScanBlock(doc, gate) {
    const box = el('div', 'net-scan');
    const scan = scanDoc || doc.scan || null;
    const head = el('div', 'net-section-head');
    head.appendChild(el('span', 'net-section-name', 'networks around this machine'));
    if (scan) {
      const age = Number(scan.age_s);
      head.appendChild(el('span', 'meta', scan.note ||
        (Number.isFinite(age) ? (age <= 2 ? 'fresh from the radio' : 'kept ' + heldText(age) + ' - press re-scan for now') : 'kept from an earlier read')));
    } else {
      head.appendChild(el('span', 'meta', 'not scanned yet - the panel will scan once by itself, or press re-scan'));
    }
    box.appendChild(head);
    if (!doc.features || !doc.features.scan) {
      box.appendChild(el('div', 'meta', 'this machine cannot scan for networks from here'));
      return box;
    }
    if (!scan || !Array.isArray(scan.networks) || !scan.networks.length) {
      box.appendChild(el('div', 'meta', scan && scan.ok === false ? String(scan.note || 'the scan failed') : 'the scan found nothing, or has not been read yet'));
      return box;
    }
    const networks = scan.networks.slice().sort(function (a, b) { return (Number(b.signal) || 0) - (Number(a.signal) || 0); });
    const table = el('div', 'net-table');
    const headRow = el('div', 'net-tr net-th');
    for (const word of ['', 'network', 'bssid', 'band', 'ch', 'security', 'radio', ''])
      headRow.appendChild(el('span', null, word));
    table.appendChild(headRow);
    for (const network of networks) {
      const row = el('div', 'net-tr');
      const signalCell = el('span', 'net-signal');
      signalCell.appendChild(signalBars(network.signal));
      signalCell.appendChild(el('i', 'net-signal-pct', Number(network.signal) >= 0 ? String(network.signal) + '%' : ''));
      row.appendChild(signalCell);
      const nameCell = el('span', 'net-ssid');
      nameCell.appendChild(el('span', null, network.hidden ? '(hidden network)' : String(network.ssid || '?')));
      if (network.hidden) nameCell.appendChild(el('span', 'net-tag', 'hidden'));
      row.appendChild(nameCell);
      row.appendChild(el('span', 'net-mono', oneLiner(network.bssid, 17)));
      row.appendChild(el('span', null, String(network.band || '')));
      row.appendChild(el('span', 'net-mono', Number(network.channel) >= 0 ? String(network.channel) : ''));
      row.appendChild(el('span', null, String(network.auth || '') + (network.encryption ? ' · ' + String(network.encryption).toUpperCase() : '')));
      // netsh states one rate ladder for every BSS it sees, so the radio the
      // network speaks on carries more truth per character here than the
      // fastest frame, which is the same number for everything in sight.
      const rateCell = el('span', 'net-mono', String(network.radio || '') ||
        (Number(network.max_rate) > 0 ? network.max_rate + ' M' : ''));
      if (Number(network.max_rate) > 0)
        rateCell.title = 'the machine names this network’s frame ladder at ' + network.max_rate +
                         ' Mbps; the radio type says what it really speaks';
      row.appendChild(rateCell);
      const actions = el('span', 'net-row-actions');
      actions.appendChild(hand('join', null, 'fills the join form with this network; the machine still asks for the right key', function () {
        formMemory['join:ssid'] = String(network.ssid || '');
        const join = wifiHost.querySelector('.net-join');
        if (join) {
          join.open = true;
          const ssidField = join.querySelector('input');
          if (ssidField) { ssidField.value = String(network.ssid || ''); }
          join.scrollIntoView({ behavior: 'smooth', block: 'center' });
        }
        ui.toastPush('the join form knows about ' + oneLiner(network.ssid || 'that network', 40) + ' - its key belongs to a person, not this page', 'wait', 6);
      }));
      actions.appendChild(hand('forget', 'err', 'the machine forgets the saved profile for this network name', function () {
        ask({ action: 'wifi_forget', ssid: String(network.ssid || '') }, 'asking the machine to forget ' + network.ssid);
      }, !gate.yes, gate.why));
      row.appendChild(actions);
      table.appendChild(row);
    }
    box.appendChild(table);
    box.appendChild(el('div', 'meta', networks.length + ' network' + (networks.length === 1 ? '' : 's') +
      (Number(scan.at) > 0 ? ', scanned ' + ui.clock(scan.at) : '') + (scan.total_more ? ' at the front of a longer list' : '')));
    return box;
  }

  function buildAccessPoint(doc, gate) {
    const box = el('div', 'net-ap');
    const ap = doc.ap || {};
    const head = el('div', 'net-section-head');
    head.appendChild(el('span', 'net-section-name', 'access point'));
    box.appendChild(head);
    if (!ap.supported) {
      box.appendChild(el('div', 'meta', 'this machine will not stand an access point up from here: ' +
        (ap.reason || 'the machine said nothing about why')));
      return box;
    }
    if (ap.active) {
      const bandWord = { Auto: 'auto', TwoPointFourGigahertz: '2.4 GHz', FiveGigahertz: '5 GHz', SixGigahertz: '6 GHz' };
      box.appendChild(el('div', 'meta', 'up as ' + (ap.ssid || 'a name the machine keeps') +
        (Number(ap.channel) >= 0 ? ' on channel ' + ap.channel : '') +
        (ap.band ? ' with the radio on ' + (bandWord[ap.band] || ap.band) : '') +
        (ap.interface ? ' on ' + ap.interface : '') + ', with ' + (ap.clients || []).length + ' client' + ((ap.clients || []).length === 1 ? '' : 's') + ' present'));
      if (Array.isArray(ap.clients) && ap.clients.length) {
        const list = el('div', 'net-facts');
        for (const client of ap.clients)
          list.appendChild(el('div', 'net-fact', typeof client === 'string' ? client : JSON.stringify(client)));
        box.appendChild(list);
      }
      box.appendChild(hand('take the access point down', 'err', 'the machine stops it; its clients fall off',
        function () { ask({ action: 'ap_stop', interface: ap.interface || undefined }, 'asking the machine to take the access point down'); },
        !gate.yes, gate.why));
      return box;
    }
    box.appendChild(el('div', 'meta', doc.platform === 'windows'
      ? 'this machine keeps a mobile hotspot - the same one its own settings page runs; it shares whatever Internet connection stands, and the radio picks the channel'
      : 'hostapd stands this radio up as an access point; the machine keeps its own hostapd rules'));
    const form = el('div', 'net-row net-row-form');
    field(form, 'ap:ssid', 'network name', 'what stations will see');
    field(form, 'ap:psk', 'key', doc.platform === 'windows'
      ? '8 to 63 characters; blank keeps the key the machine carries'
      : '8 to 63 characters; blank is open', false, true);
    if (doc.platform === 'windows') {
      choose(form, 'ap:wband', 'band', [['auto', 'as the machine chose'], ['2.4', '2.4 GHz'], ['5', '5 GHz'], ['6', '6 GHz']]);
      form.appendChild(hand('stand it up', null, 'the machine\'s own tethering manager, with these words; if it refuses, its words come back whole', function () {
        ask({
          action: 'ap_start',
          ssid: String(formMemory['ap:ssid'] || ''),
          psk: String(formMemory['ap:psk'] || ''),
          band: String(formMemory['ap:wband'] || 'auto'),
        }, 'asking the machine to share through its mobile hotspot');
      }, !gate.yes, gate.why));
    } else {
      choose(form, 'ap:band', 'band', [['2.4', '2.4 GHz'], ['5', '5 GHz'], ['6', '6 GHz']]);
      field(form, 'ap:channel', 'channel', 'blank lets the radio choose', true);
      field(form, 'ap:country', 'country', 'two letters, for the radio rules');
      field(form, 'ap:max', 'clients at most', '8', true);
      form.appendChild(hand('stand it up', null, 'hostapd on this radio with these words; down with the press of the same door', function () {
        const body = {
          action: 'ap_start',
          ssid: String(formMemory['ap:ssid'] || ''),
          psk: String(formMemory['ap:psk'] || ''),
          band: String(formMemory['ap:band'] || '2.4'),
        };
        const channel = Number(formMemory['ap:channel'] || 0);
        if (Number.isFinite(channel) && channel > 0) body.channel = channel;
        if (formMemory['ap:country']) body.country = String(formMemory['ap:country']);
        const maxClients = Number(formMemory['ap:max'] || 0);
        if (Number.isFinite(maxClients) && maxClients > 0) body.max_clients = maxClients;
        ask(body, 'asking hostapd to stand ' + (body.ssid || 'the network') + ' up');
      }, !gate.yes, gate.why));
    }
    box.appendChild(form);
    return box;
  }

  // ---------- what this machine can change, and what it said about that ----------
  let settingsShapeKey = '';

  function settingsShape(doc) {
    return String(doc.elevated) + '|' + JSON.stringify(doc.features || {}) + '|' +
      JSON.stringify(doc.sharing || {}) + '|' + String((doc.actions_last || {}).at || 0) + '|' +
      (doc.adapters || []).map(function (a) { return String(a.name) + (a.managed_by_this_server ? 'M' : '') + a.kind + a.oper; }).join(',');
  }

  function featureChip(box, name, on, reason) {
    const chip = el('span', 'chip ' + (on ? 'ok' : ''), name);
    chip.title = on ? (reason || 'this machine can, from here') : (reason || 'this machine cannot, from here');
    box.appendChild(chip);
  }

  function drawSettings(doc) {
    const shape = settingsShape(doc);
    if (shape === settingsShapeKey) return;
    settingsShapeKey = shape;
    settingsHost.innerHTML = '';
    const gate = canChange(doc);
    const features = doc.features || {};

    const featuresBox = el('div', 'net-section');
    const title = el('div', 'net-section-head');
    title.appendChild(el('span', 'net-section-name', 'what this machine will let this server change'));
    title.appendChild(el('span', 'chip ' + (doc.elevated ? 'ok' : 'warn'),
      doc.elevated ? (doc.platform === 'windows' ? 'the manager: yes' : 'root: yes')
                   : (doc.platform === 'windows' ? 'the manager: no - settings are read-only from here' : 'root: no - settings are read-only from here')));
    featuresBox.appendChild(title);
    const chips = el('div', 'machine-chips');
    featureChip(chips, 'wifi', features.wifi, features.wifi_reason);
    featureChip(chips, 'scans', features.scan);
    featureChip(chips, 'bonding', features.bonding, features.bonding_reason);
    featureChip(chips, 'vlan', features.vlan, features.vlan_reason);
    featureChip(chips, 'netplan', features.netplan, features.netplan_reason);
    featureChip(chips, 'NetworkManager', features.nm);
    featureChip(chips, 'rfkill', features.rfkill, features.rfkill_reason);
    featureChip(chips, 'access point', features.ap, features.ap_reason);
    featureChip(chips, 'sharing', features.sharing, features.sharing_reason);
    featuresBox.appendChild(chips);
    settingsHost.appendChild(featuresBox);

    // Sharing.
    const sharing = doc.sharing || {};
    const sharingBox = el('div', 'net-section');
    const sharingHead = el('div', 'net-section-head');
    sharingHead.appendChild(el('span', 'net-section-name', 'sharing one connection with the others'));
    sharingBox.appendChild(sharingHead);
    if (!sharing.supported) {
      sharingBox.appendChild(el('div', 'meta', 'this machine will not share a connection from here: ' +
        (sharing.reason || 'it said nothing about why')));
    } else {
      const state = sharing.active === true ? 'is sharing now'
        : sharing.active === false ? 'is not sharing'
        : 'has not said whether it is sharing';
      sharingBox.appendChild(el('div', 'meta', state + (sharing.mode ? ' · ' + sharing.mode : '') +
        (sharing.service ? ' · service ' + sharing.service : '')));
      const candidates = (doc.adapters || []).filter(function (a) { return a.kind !== 'loopback' && !a.master; });
      const form = el('div', 'net-row net-row-form');
      choose(form, 'share:ext', 'reaches the internet',
        candidates.map(function (a) { return [a.name, a.name]; }));
      choose(form, 'share:int', 'gets shared with',
        candidates.map(function (a) { return [a.name, a.name]; }));
      form.appendChild(hand('share it', null, 'the machine turns on sharing from the first to the second',
        function () {
          ask({ action: 'share_enable', external: String(formMemory['share:ext'] || ''), internal: String(formMemory['share:int'] || '') },
            'asking the machine to share ' + formMemory['share:ext'] + ' with ' + formMemory['share:int']);
        }, !gate.yes, gate.why));
      form.appendChild(hand('stop sharing', 'err', 'whatever rules this server wrote are taken back off',
        function () { ask({ action: 'share_disable' }, 'asking the machine to stop sharing'); }, !gate.yes, gate.why));
      sharingBox.appendChild(form);
    }
    settingsHost.appendChild(sharingBox);

    // Bonds and vlans: the machine's own managers on Linux, refused with a
    // reason elsewhere.
    drawLinksMade(doc, features, gate);

    // The last thing this door was asked to do, kept where the machine keeps it.
    const last = doc.actions_last;
    if (last && last.action) {
      const lastBox = el('div', 'net-section net-last');
      lastBox.appendChild(el('div', 'meta',
        'last change asked (' + ui.clock(Number(last.at) || 0) + '): ' + String(last.action) +
        ' - ' + (last.did ? 'the machine did it' : 'the machine did not') + ' - ' + oneLiner(String(last.words || ''), 400),
        String(last.words || '')));
      settingsHost.appendChild(lastBox);
    }
  }

  function drawLinksMade(doc, features, gate) {
    const adapters = doc.adapters || [];
    const box = el('div', 'net-section');
    const head = el('div', 'net-section-head');
    head.appendChild(el('span', 'net-section-name', 'bonds and vlans'));
    box.appendChild(head);
    if (!features.bonding && !features.vlan) {
      box.appendChild(el('div', 'meta', String(features.bonding_reason || 'this machine does not manage bonds from here') +
        ' · ' + String(features.vlan_reason || 'and none for vlans either')));
      return;
    }
    const bonds = adapters.filter(function (a) { return a.kind === 'bond'; });
    const vlans = adapters.filter(function (a) { return a.kind === 'vlan'; });
    if (!bonds.length) box.appendChild(el('div', 'meta', 'no bonds on this machine'));
    for (const bond of bonds) {
      box.appendChild(el('div', 'net-fact',
        'bond ' + bond.name + ' · ' + String(bond.oper) + ' · holds ' + (bond.slaves || []).join(', ') +
        (bond.managed_by_this_server ? ' · made by this server' : ' · made by someone else')));
      if (bond.managed_by_this_server)
        box.appendChild(hand('dissolve ' + bond.name, 'err', 'the bond and its netplan lines come off; its members come back plain',
          function () {
            if (!window.confirm('Dissolve ' + bond.name + '? Its members come back plain, and the traffic on it stops.')) return;
            ask({ action: 'bond_delete', name: bond.name }, 'asking the machine to dissolve ' + bond.name);
          }, !gate.yes, gate.why));
    }
    if (!vlans.length) box.appendChild(el('div', 'meta', 'no vlans on this machine'));
    for (const vlan of vlans) {
      box.appendChild(el('div', 'net-fact',
        'vlan ' + vlan.name + (vlan.vlan ? ' id ' + vlan.vlan.id + ' on ' + vlan.vlan.parent : '') +
        ' · ' + String(vlan.oper) + (vlan.managed_by_this_server ? ' · made by this server' : ' · made by someone else')));
      if (vlan.managed_by_this_server)
        box.appendChild(hand('remove ' + vlan.name, 'err', 'the vlan and its netplan line come off',
          function () { ask({ action: 'vlan_delete', name: vlan.name }, 'asking the machine to remove ' + vlan.name); },
          !gate.yes, gate.why));
    }
    const plain = adapters.filter(function (a) { return (a.kind === 'ethernet' || a.kind === 'bond' || a.kind === 'vlan') && !a.master; });
    const form = el('details', 'net-form');
    form.appendChild(el('summary', null, 'make a bond or a vlan'));
    const body = el('div', 'net-form-body');
    if (features.bonding) {
      const bondForm = el('div', 'net-row net-row-form');
      field(bondForm, 'bond:name', 'bond name', 'bond0');
      field(bondForm, 'bond:slaves', 'adapters to hold', 'comma separated, two or more');
      choose(bondForm, 'bond:mode', 'mode', [
        ['802.3ad', '802.3ad (lacp)'], ['active-backup', 'active-backup'], ['balance-rr', 'balance-rr'],
        ['balance-xor', 'balance-xor'], ['broadcast', 'broadcast'], ['balance-tlb', 'balance-tlb'], ['balance-alb', 'balance-alb'],
      ]);
      field(bondForm, 'bond:addr', 'address', 'blank lets dhcp hand one');
      field(bondForm, 'bond:prefix', 'prefix', '24', true);
      field(bondForm, 'bond:gw', 'gateway', '');
      field(bondForm, 'bond:dns', 'dns servers', 'space separated');
      bondForm.appendChild(hand('make the bond', null, 'written to this server\'s netplan file and applied; a file netplan generate refuses is rolled back',
        function () {
          ask({
            action: 'bond_create',
            name: String(formMemory['bond:name'] || ''),
            slaves: String(formMemory['bond:slaves'] || '').split(/[,\s]+/).filter(Boolean),
            mode: String(formMemory['bond:mode'] || '802.3ad'),
            address: String(formMemory['bond:addr'] || ''),
            prefix: Number(formMemory['bond:prefix'] || 24),
            gateway: String(formMemory['bond:gw'] || ''),
            dns: String(formMemory['bond:dns'] || '').split(/\s+/).filter(Boolean),
          }, 'asking the machine for a bond named ' + formMemory['bond:name']);
        }, !gate.yes, gate.why));
      body.appendChild(bondForm);
    } else {
      body.appendChild(el('div', 'meta', 'no bonds from here: ' + String(features.bonding_reason || 'the machine said nothing about why')));
    }
    if (features.vlan) {
      const vlanForm = el('div', 'net-row net-row-form');
      choose(vlanForm, 'vlan:parent', 'on top of', plain.map(function (a) { return [a.name, a.name]; }));
      field(vlanForm, 'vlan:id', 'id', '1 to 4094', true);
      field(vlanForm, 'vlan:name', 'name', 'blank names it parent.id');
      field(vlanForm, 'vlan:addr', 'address', 'blank lets dhcp hand one');
      field(vlanForm, 'vlan:prefix', 'prefix', '24', true);
      field(vlanForm, 'vlan:gw', 'gateway', '');
      field(vlanForm, 'vlan:dns', 'dns servers', 'space separated');
      vlanForm.appendChild(hand('make the vlan', null, 'written to this server\'s netplan file and applied', function () {
        const vbody = {
          action: 'vlan_create',
          parent: String(formMemory['vlan:parent'] || ''),
          id: Number(formMemory['vlan:id'] || 0),
          address: String(formMemory['vlan:addr'] || ''),
          prefix: Number(formMemory['vlan:prefix'] || 24),
          gateway: String(formMemory['vlan:gw'] || ''),
          dns: String(formMemory['vlan:dns'] || '').split(/\s+/).filter(Boolean),
        };
        if (formMemory['vlan:name']) vbody.name = String(formMemory['vlan:name']);
        ask(vbody, 'asking the machine for a vlan ' + vbody.parent + '.' + vbody.id);
      }, !gate.yes, gate.why));
      body.appendChild(vlanForm);
    } else {
      body.appendChild(el('div', 'meta', 'no vlans from here: ' + String(features.vlan_reason || 'the machine said nothing about why')));
    }
    form.appendChild(body);
    box.appendChild(form);
    settingsHost.appendChild(box);
  }

  // ---------- the chips across the top ----------
  function drawChips(doc) {
    const adapters = doc.adapters || [];
    const up = adapters.filter(function (a) { return a.oper === 'up'; }).length;
    ui.chip(linksChip, 'links: ' + up + ' up of ' + adapters.length,
      up ? 'ok' : '', adapters.length + ' adapter' + (adapters.length === 1 ? '' : 's') + ' on this machine');
    let rx = 0, tx = 0;
    let anyRate = false;
    for (const adapter of adapters) {
      if (adapter.rate) {
        rx += Number(adapter.rate.rx) || 0;
        tx += Number(adapter.rate.tx) || 0;
        anyRate = true;
      }
    }
    ui.chip(ratesChip, anyRate ? 'all links: ' + rateText(rx) + ' down · ' + rateText(tx) + ' up' : 'traffic: not read yet',
      anyRate ? 'loaded' : '', 'summed over every adapter the sampler reads');
    const ifaces = (doc.wifi && Array.isArray(doc.wifi.interfaces)) ? doc.wifi.interfaces : [];
    const connected = ifaces.filter(function (i) { return i.state === 'connected' || i.state === 'completed'; });
    const blockedWifi = ((doc.wifi && doc.wifi.blocked) || []).length > 0;
    ui.chip(wifiChip,
      blockedWifi ? 'wifi: radio blocked'
      : connected.length ? 'wifi: ' + oneLiner(connected[0].ssid || 'a network', 28) + (Number(connected[0].signal) >= 0 ? ' · ' + connected[0].signal + '%' : '')
      : ifaces.length ? 'wifi: not connected' : 'wifi: none',
      blockedWifi ? 'err' : connected.length ? 'ok' : ifaces.length ? 'warn' : '',
      connected.length
        ? (connected[0].auth || 'a network') + (connected[0].bssid ? ' at ' + connected[0].bssid : '')
        : (ifaces.length ? ifaces.length + ' radio' + (ifaces.length === 1 ? '' : 's') + ' with no network' : 'no wifi this server can see'));
    const last = doc.actions_last;
    ui.chip(doorsChip,
      last && last.action ? (last.did ? 'last change: done' : 'last change: refused') : 'changes: none asked',
      last ? (last.did ? 'ok' : 'err') : '',
      last ? String(last.action) + ' - ' + String(last.words || '') : 'this panel has not been asked to change anything yet');
  }

  // ---------- the read ----------
  function draw(doc) {
    snapshot = doc;
    drawChips(doc);
    drawAdapters(doc);
    drawWifi(doc);
    drawSettings(doc);
    // The scan kept by the server is the newest one this browser may have;
    // keep them one fact, not two.
    if (doc.scan && (!scanDoc || Number(doc.scan.at) > Number(scanDoc.at))) {
      scanDoc = doc.scan;
      wifiShapeKey = ''; // the table belongs to the wifi block; let it redraw
    }
    const features = doc.features || {};
    // The first page on a machine with a radio asks for one scan of its own:
    // a table that fills itself reads better than a hand no one pressed.
    if (!firstScanAsked && features.scan && !scanDoc) {
      firstScanAsked = true;
      rescan(false);
    }
    line.textContent = 'live · ' + String(doc.os || doc.platform || 'a machine') + ' · ' + String(doc.hostname || '') +
      (doc.kernel ? ' · kernel ' + doc.kernel : '') +
      ' · up ' + heldText(Number(doc.uptime_s) || 0) +
      ' · read ' + (Number(doc.at) > 0 ? ui.clock(Number(doc.at)) : '') + ' · ' +
      (doc.elevated ? 'settings are changeable from here' : 'a plain person reads; changes belong to the machine\'s manager');
    line.className = Number(doc.at) > 0 ? 'meta-ok' : 'meta';
  }

  async function rescan(forced) {
    if (scanning) return;
    scanning = true;
    const was = line.textContent;
    if (forced) line.textContent = 'the radio is scanning - it answers in a second or two…';
    let doc = null;
    try {
      doc = await ui.api('/api/network/scan' + (forced ? '?fresh=1' : ''));
    } finally {
      scanning = false;
    }
    if (doc && doc.ok) {
      scanDoc = doc;
      wifiShapeKey = '';
      if (snapshot) drawWifi(snapshot);
      if (forced)
        ui.log('net: the radio answered with ' + (Array.isArray(doc.networks) ? doc.networks.length : 0) +
               ' network' + (Array.isArray(doc.networks) && doc.networks.length === 1 ? '' : 's'), 'ok');
    } else if (forced) {
      ui.log('net: the scan did not answer' + (doc && doc.note ? ': ' + String(doc.note) : ''), 'err');
    }
    if (forced && doc) line.textContent = was;
  }

  async function refresh() {
    const doc = await ui.api('/api/network');
    if (!doc) {
      offline();
      return;
    }
    draw(doc);
  }

  function offline() {
    line.textContent = 'the network door did not answer';
    line.className = 'meta';
    if (adapterShapeKey) { adapterShapeKey = ''; }
    wifiShapeKey = '';
    settingsShapeKey = '';
  }

  // ---------- hands and the loop ----------
  if (scanHand) ui.onPress(scanHand, function () { rescan(true); });

  const kept = ui.lastReading('/api/network');
  if (kept && kept.data) draw(kept.data);

  async function main() {
    // The server samples the counters every two seconds; the page reads at
    // that pace, so every reading it draws is one the machine actually took.
    await ui.poll(refresh, offline, 2000);
  }

  main();
})();
