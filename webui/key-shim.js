// What a room knows about the credential of this visit: the machine's own key,
// and whether this browser carries it. What this machine accepted is kept - the
// active one under ols-key, beside it every form this browser was given, newest
// first, each marked by how it was given. This page's own asks carry what it
// holds as a header, and everything the browser loads on its own - pictures,
// sounds, files it saves - carries the cookie this machine wrote on the
// connection when it took the key, so no address of this machine need name a
// credential; a browser that wakes with a key written in its address takes it
// up at once and wipes the address clean.
// When the machine asks for a key this browser does not carry, the page gates:
// before the page's first ask leaves for a door, the prompt stands, what is
// handed goes over the same door any program hands a credential, and nothing
// behind the doors is asked until the machine says yes. A person who asks for
// none is answered honestly - the page goes on reading, says where it stands at
// the doors, and asks again when it is told to. window.olsKeyState holds what
// this page knows of the doors, for the config row and the home line to read; a
// machine that refused a visit is never drawn as a missing machine.
(function () {
  const KEY_STORE = 'ols-key';
  // What this browser was given, newest first, each marked by how it was given.
  const KEY_CACHE = 'ols-keys-v1';
  const kKeysKept = 12;

  // What this page knows of the credential also rides on the connection, so the
  // things a browser loads on its own - a picture, a sound, a file it saves -
  // come through the doors with it: a file door answers a bare refusal and writes
  // nothing of its own, so a room that carries the key only in a header would
  // save those bytes as the door's words rather than as the file.
  function carryOnConnection(value) {
    try {
      document.cookie = "ols-key=" + encodeURIComponent(value) + "; Path=/; SameSite=Lax";
    } catch (noCookie) {
    }
  }

  function stored() {
    try {
      return localStorage.getItem(KEY_STORE) || '';
    } catch (noStorage) {
      return '';
    }
  }

  // A key this browser kept on an earlier visit rides on the connection again.
  if (stored() !== '') carryOnConnection(stored());

  function remembered() {
    try {
      const kept = JSON.parse(localStorage.getItem(KEY_CACHE) || '[]');
      return Array.isArray(kept) ? kept : [];
    } catch (noStorage) {
      return [];
    }
  }

  // A key this machine took joins the store once, at the front; the rest keep
  // their order, and the oldest past the room are dropped.
  function writeGiven(value, via) {
    if (!value) return;
    try {
      localStorage.setItem(KEY_STORE, value);
    } catch (noStorage) {
    }
    try {
      const kept = remembered().filter(function (one) {
        return String((one || {}).key || '') !== String(value);
      });
      kept.unshift({ key: String(value), via: via });
      localStorage.setItem(KEY_CACHE, JSON.stringify(kept.slice(0, kKeysKept)));
    } catch (noStorage) {
    }
    carryOnConnection(value);
  }

  // The active key lets go: what the doors refuse is not carried into every
  // later ask to be refused again.
  function letGoOfKey() {
    try {
      localStorage.removeItem(KEY_STORE);
    } catch (noStorage) {
    }
    try {
      document.cookie = "ols-key=; Path=/; Max-Age=0";
    } catch (noCookie) {
    }
  }

  // The address is where a program, and a browser walking over from a machine
  // next door, can hand a credential to this page: it is taken up the moment the
  // page wakes and wiped off the address, where it belongs to nobody's history.
  (function fromTheAddress() {
    let here = null;
    try {
      here = new URL(window.location.href);
    } catch (noUrl) {
      return;
    }
    const given = here.searchParams.get('key');
    if (!given) return;
    here.searchParams.delete('key');
    try {
      window.history.replaceState(null, '', here.pathname + here.search + here.hash);
    } catch (noHistory) {
    }
    writeGiven(given, 'carried in the address');
  })();

  // What this page knows of the doors: the standing question, asked once per
  // page, and what the doors behind it answered to this page's own asks.
  window.olsKeyState = { state: 'checking', text: 'the doors have not been asked yet', apiAsked: null };

  function say(state, text) {
    window.olsKeyState.state = state;
    window.olsKeyState.text = text;
  }

  // The one standing ask of this machine's doors, shared by every request on
  // the page: the first fetch that needs to know what the doors ask of waits on
  // it, and no page pays for the question twice.
  let standingAsk = null;
  // Whether the ask in flight is one of the doors' own - what they ask of, or a
  // credential being handed - whose answers say nothing about this page's place
  // at the doors, and which must never wait on the gate themselves.
  let atOwnDoor = false;

  // The door of this machine is the bare address: it stands open to nothing it
  // asks of, so a visit reaches it before anything else can. What it answers
  // names counts and never a letter of a key.
  async function askTheDoors() {
    atOwnDoor = true;
    let res = null;
    try {
      res = await window.fetch('/', { headers: { Accept: 'application/json' } });
    } finally {
      atOwnDoor = false;
    }
    if (!res || !res.ok) {
      say('unknown', 'this machine did not answer what is asked of');
      return;
    }
    let info = null;
    try {
      info = await res.json();
    } catch (notRead) {
      info = null;
    }
    if (!info) {
      say('unknown', 'this machine answered the ask about its doors in words this page cannot read');
      return;
    }
    const asked = info.api_required === true;
    window.olsKeyState.apiAsked = asked;
    // Whether this machine keeps a fleet table: the page only asks /api/peers
    // where the door itself said one stands. Until the doors have answered, the
    // word is no one's - an unanswered machine has a table neither way.
    window.olsKeyState.peers = info.peers === true;
    if (asked)
      say('asked', 'this machine asks for a key it set for itself, at every door of it');
    else
      say('open', 'this machine asks of nothing: its doors are open');
  }

  // A fresh ask of what the doors ask of: the config row does this when a
  // setting moves, so the row speaks from the newest answer.
  function checkDoors() {
    standingAsk = askTheDoors();
    return standingAsk;
  }

  // Hand a credential to the bare address, as a program hands one: the
  // machine's yes is the whole answer, and a yes writes the key on the
  // connection for everything the browser loads on its own. 'quiet' when no
  // answer came at all - a door that is silent is not a door that refused.
  async function tryTheKey(value) {
    atOwnDoor = true;
    try {
      const res = await window.fetch('/', { method: 'POST', body: String(value) });
      const answer = await res.json().catch(function () {
        return null;
      });
      if (answer && answer.opens_doors === true) return true;
      return res.ok ? false : 'quiet';
    } catch (noAnswer) {
      return 'quiet';
    } finally {
      atOwnDoor = false;
    }
  }

  // The page's own ask of the person standing at it: the prompt stands until
  // this machine says yes to what it was handed - the key this browser carries
  // first, then whatever is typed. Cancelled once, the page goes on honestly
  // without a credential until it is asked to try again.
  let doorOpen = null;
  let doorsOpen = false;
  let askedForNone = false;

  async function gateThePage() {
    if (!standingAsk) standingAsk = askTheDoors();
    try {
      await standingAsk;
    } catch (doorsQuiet) {
    }
    if (window.olsKeyState.apiAsked !== true) return true;
    let note = '';
    for (; ;) {
      if (askedForNone) return false;
      const carried = stored();
      if (carried) {
        const known = await tryTheKey(carried);
        if (known === true) {
          say('accepted', 'this browser was let through the door of this machine');
          return true;
        }
        if (known === 'quiet') {
          say('unknown', 'this machine did not answer the credential this page carries');
          return false;
        }
        letGoOfKey();
        note = 'the key this browser carried is not the one this machine asks for now; ';
      }
      const handed = window.prompt(note + 'This machine asks for its key. The key opens every door of it.', '');
      if (handed === null) {
        askedForNone = true;
        say('held', 'this page carries no credential: no key was asked for');
        return false;
      }
      const given = handed.trim();
      if (!given) {
        note = 'nothing was handed over; ';
        continue;
      }
      const taken = await tryTheKey(given);
      if (taken === true) {
        writeGiven(given, 'typed');
        say('accepted', 'this browser was let through the door of this machine');
        return true;
      }
      if (taken === 'quiet') {
        say('unknown', 'this machine did not answer the key this page handed it');
        return false;
      }
      note = 'that is not the key this machine asks for; ';
    }
  }

  // The one gate this page stands behind: every ask waits on it, and while it
  // stands open nothing asks the person twice.
  function throughTheDoors() {
    if (!doorOpen) {
      doorOpen = gateThePage().then(function (open) {
        doorsOpen = open;
        if (!open) doorOpen = null;
        return open;
      });
    }
    return doorOpen;
  }

  window.fetch = (function () {
    const rawFetch = window.fetch;
    return async function (url, options) {
      // What the doors ask of is asked of once per page, and the first ask that
      // is not the doors' own waits on it - and then on the page's own ask of
      // this person, so nothing behind a door is reached before the key was
      // handed.
      if (!atOwnDoor) {
        try {
          await throughTheDoors();
        } catch (doorsQuiet) {
        }
      }
      // Read the credential after waiting, so the first ask that stands behind
      // the gate already carries what the gate just took in.
      const carried = stored();
      if (carried) {
        const headers = Object.assign({}, (options || {}).headers || {});
        headers['X-Api-Key'] = carried;
        options = Object.assign({}, options || {});
        options.headers = headers;
      }
      let res = null;
      try {
        res = await rawFetch(url, options);
      } catch (notHandedOver) {
        // The answer may be no answer because the gate went quiet after
        // refusals, not because the machine is away: one more ask, one
        // breath later, tells the two apart - a quiet gate stays quiet.
        try {
          await new Promise(function (done) { setTimeout(done, 400); });
          res = await rawFetch(url, options);
        } catch (stillNoAnswer) {
          throw notHandedOver;
        }
      }
      // The doors that obtain a credential answer regardless of any door, so
      // their own answers say nothing about this page's place at the doors;
      // only the doors behind it are felt.
      if (!atOwnDoor) {
        if (res.status === 401 || res.status === 403) {
          say('refused', 'this page was not let through the door of this machine');
          // A credential this page believed in stopped opening the doors - the
          // key moved while the page stood. The next ask waits on a fresh
          // prompt; asks already out keep the refusal they were answered with.
          if (window.olsKeyState.apiAsked === true && doorsOpen) {
            doorsOpen = false;
            doorOpen = null;
          }
        } else if (res.status < 400) {
          say('accepted', 'this browser was let through the door of this machine');
        }
      }
      // Honor a rate-limit response: wait for the server's Retry-After, up to
      // three retries.
      for (let tries = 0; res.status === 429 && tries < 3; tries++) {
        const wait = Math.min(Math.max(parseInt(res.headers.get('Retry-After') || '2', 10) * 1000, 500), 15000);
        await new Promise(function (done) { setTimeout(done, wait); });
        res = await rawFetch(url, options);
      }
      return res;
    };
  })();

  window.olsKey = {
    get: stored,
    remembered: remembered,
    // Stand behind the doors again: after a refusal, or after the prompt was
    // asked for none, this hands the page its ask of the person once more. A
    // gate already standing is the same gate - a second ask never opens a
    // second prompt beside it.
    ask: function () {
      if (doorOpen) return doorOpen;
      askedForNone = false;
      doorsOpen = false;
      return throughTheDoors();
    },
    // Re-ask what the doors ask of - the config row does this when a
    // setting changes, so the row speaks from the newest answer.
    check: checkDoors,
  };
})();
