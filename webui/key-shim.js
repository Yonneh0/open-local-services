// What a room knows about the credential of this visit: the door page is
// where a credential is handed over, and what this machine accepted is kept -
// the active one under ols-key, beside it every form this browser was given,
// newest first, each marked by how it was given. This page's own asks carry
// what it holds as a header, and everything the browser loads on its own -
// pictures, sounds, event streams - carries the cookie the door wrote, so no
// address of this machine ever names a credential. window.olsKeyState holds
// what this page knows of the doors - the standing question answered once
// per page, and what the doors behind it answered to this page's own asks -
// for the config row and the home line to read. A machine that refused a
// visit is never drawn as a missing machine.
(function () {
  const KEY_STORE = 'ols-key';
  // What this browser was given, newest first, each marked by how it was given.
  const KEY_CACHE = 'ols-keys-v1';
  const kKeysKept = 12;
  // The door's walk count belongs to the door's visit: arriving at a room
  // clears it, so a later door - here or elsewhere - starts its own again.
  const WALK_MARK = 'ols-door-attempt';

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

  function letGoOfConnection() {
    try {
      document.cookie = "ols-key=; Path=/; Max-Age=0";
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

  // A form this page puts up joins the store once, at the front; the rest
  // keep their order, and the oldest past the room are dropped.
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

  try {
    localStorage.removeItem(WALK_MARK);
  } catch (noStorage) {
  }

  // What this page knows of the doors. The standing question - what the
  // doors ask of - is asked of once per page; what the doors behind it
  // answered to this page's own asks is read from their statuses.
  window.olsKeyState = { state: 'checking', text: 'the doors have not been asked yet', forms: -1, apiAsked: null };

  function say(state, text) {
    window.olsKeyState.state = state;
    window.olsKeyState.text = text;
  }

  // The one standing ask of this machine's doors, shared by every request on
  // the page: the first fetch that needs to know what the doors ask of waits
  // on it, and no page pays for the question twice.
  let standingAsk = null;
  // Whether the ask in flight is the door's own, whose answer says nothing about
  // this page's place at the doors.
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
    window.olsKeyState.forms = Number(info.page_keys);
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

  window.fetch = (function () {
    const rawFetch = window.fetch;
    return async function (url, options) {
      const carried = stored();
      if (carried) {
        const headers = Object.assign({}, (options || {}).headers || {});
        headers['X-Api-Key'] = carried;
        options = Object.assign({}, options || {});
        options.headers = headers;
      }
      // What the doors ask of is asked of once per page, and the first ask that
      // is not the door's own waits for it, so a page never reads a door with no
      // idea of what the doors stand at.
      if (!atOwnDoor && window.olsKeyState.apiAsked === null) {
        if (!standingAsk) standingAsk = askTheDoors();
        try {
          await standingAsk;
        } catch (doorsQuiet) {
        }
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
      // A room that refused walked this visit to the door of this machine: the
      // words came from another address than the one asked for, so the page says
      // so rather than calling a walked visit an accepted one. The count of
      // these belongs to this visit, so a room refused twice is told as much.
      let walked = false;
      try {
        const asked = new URL(String(url), window.location.href);
        const landed = new URL(String(res.url));
        walked = !atOwnDoor && landed.pathname !== asked.pathname;
      } catch (noUrl) {
        walked = false;
      }
      if (walked) {
        let tries = 0;
        try {
          tries = Number(localStorage.getItem(WALK_MARK) || '0') + 1;
          localStorage.setItem(WALK_MARK, String(tries));
        } catch (noStorage) {
          tries = 1;
        }
        say('walked', tries > 1
          ? 'this room was walked to the door a second time: this browser carries no credential of its own'
          : 'this room walked this visit to the door of this machine: no credential came with it');
      }
      // The doors that obtain a credential answer regardless of any door, so
      // their own answers say nothing about this page's place at the doors;
      // only the doors behind it are felt.
      if (!atOwnDoor && !walked) {
        if (res.status === 401 || res.status === 403)
          say('refused', 'this page was not let through the door of this machine');
        else if (res.status < 400)
          say('accepted', 'this browser was let through the door of this machine');
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
    set: function (value) {
      if (!String(value || '')) {
        try {
          localStorage.removeItem(KEY_STORE);
        } catch (noStorage) {
        }
        letGoOfConnection();
        say('checking', 'this page carries no credential: it may still be let through, or walked to the door');
        return;
      }
      writeGiven(value, 'typed');
      say('checking', 'a new credential is carried: the next read will tell');
    },
    // Re-ask what the doors ask of - the config row does this when a
    // setting changes, so the row speaks from the newest answer.
    check: checkDoors,
  };
})();
