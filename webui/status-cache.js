// Browser-side cache of the API answers this machine serves, kept in
// localStorage and enforced by a fetch wrapper.
// - GET answers are stored by path for the max-age the server states in its
//   cache-control header; a fresh hit is replayed without touching the
//   network, and a stale entry is replaced by the next real request.
// - A successful write (POST/PUT/...) clears every reading of that service,
//   so a page never renders state the write just changed.
// - Images and audio the machine serves are kept twice apart, each in its own
//   place: while this page is open a file it read is held in this page's memory,
//   so the same picture drawn again asks the machine for nothing; across visits
//   the browser's own cache is what keeps it, because the file doors answer for
//   a year, marked immutable. Nothing that has an address on the machine is
//   copied into this store.
// - A file a page made of its own bytes (a picture it decoded here) has no
//   address to be read from again, and is stored in localStorage under the name
//   the machine gave it, via window.olsCache.keep(path, dataUrl, type).
// - Concurrent requests for one path share a single network read.
// - A page that watches one door can read the moment its kept copy leaves the
//   window the machine stated, so it asks again only past that point.
// - If localStorage refuses a store write, entries still live in this page's
//   memory for the session.
(function () {
  const STORE = 'ols-api-cache-v1';
  const FILE_STORE = 'ols-files-v1';

  let entries = {};
  try {
    const raw = JSON.parse(localStorage.getItem(STORE) || 'null');
    if (raw && typeof raw === 'object' && !Array.isArray(raw)) entries = raw;
  } catch (badStore) {
    entries = {};
  }

  // What this browser keeps across visits: only files a page made of its own
  // bytes, which the machine has no address for. Each is marked as made here.
  let files = {};
  try {
    const kept = JSON.parse(localStorage.getItem(FILE_STORE) || 'null');
    if (kept && typeof kept === 'object' && !Array.isArray(kept)) {
      for (const name in kept) {
        kept[name].page = 1;
        files[name] = kept[name];
      }
    }
  } catch (badFiles) {
    files = {};
  }

  // A file this page read from the machine, held in this page's memory only:
  // drawing it again in the same page asks for nothing, and a later visit is
  // served by the browser's own cache instead of by a copy of bytes here.
  const readFiles = {};

  // In-flight reads by path, so concurrent requests share one network round-trip.
  const pending = {};

  function save() {
    // A quota failure is not fatal: the in-memory copy still serves the page.
    // Only files a page made of its own bytes are written out - a file that has
    // an address on the machine is kept by the browser, not copied here, so this
    // store stays small whatever the gallery holds.
    try {
      localStorage.setItem(STORE, JSON.stringify(entries));
    } catch (storageFull) {
    }
    try {
      localStorage.setItem(FILE_STORE, JSON.stringify(madeHere()));
    } catch (filesFull) {
    }
  }

  function madeHere() {
    const kept = {};
    for (const name in files) {
      if (files[name].page) kept[name] = files[name];
    }
    return kept;
  }

  // What a cache entry stands under: the path and query of an ask as they
  // are written. Nothing this machine makes puts a credential in an address
  // - the door writes one on the connection instead - so there is nothing
  // to take out, and a reading and its repeat share one entry either way.
  function pathOf(url) {
    const text = String(url);
    if (text.charAt(0) === '/') return text;
    if (text.indexOf('http://') === 0 || text.indexOf('https://') === 0) {
      const at = text.indexOf('/', text.indexOf('//') + 2);
      return at < 0 ? '' : text.slice(at);
    }
    return '';
  }

  // All readings of one service share a prefix, so a write can clear them
  // together: /voice/api/... belongs to the family /voice/api/.
  function familyOf(path) {
    if (path.charAt(0) !== '/') return '';
    const parts = path.slice(1).split('/');
    if (parts.length >= 3 && parts[1] === 'api') return '/' + parts[0] + '/api/';
    return '/' + parts[0];
  }

  // Reuse window for an answer, read off its own cache-control header.
  // Zero means the answer must not be cached.
  function freshSeconds(res) {
    const line = String(res.headers.get('cache-control') || '');
    const at = line.indexOf('max-age=');
    if (at < 0) return 0;
    const seconds = parseInt(line.slice(at + 8), 10);
    return seconds > 0 ? seconds : 0;
  }

  function fileish(type) {
    const line = String(type).toLowerCase();
    return line.indexOf('image/') === 0 || line.indexOf('audio/') === 0;
  }

  function dataToBytes(data) {
    const at = data.indexOf(';base64,');
    if (at < 0) return null;
    const raw = atob(data.slice(at + 8));
    const bytes = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
    return bytes;
  }

  function store(path, text, seconds) {
    const at = Date.now();
    entries[path] = { at: at, until: at + seconds * 1000, text: text, fresh: seconds };
    save();
  }

  // A file a page made of its own bytes, kept under the name the machine gave it
  // so it can be drawn again without decoding it a second time.
  function keepFile(path, data, type, seconds) {
    const at = Date.now();
    files[path] = { at: at, until: at + seconds * 1000, data: data, type: type, fresh: seconds, page: 1 };
    save();
  }

  // A file this page read from the machine: its bytes stay in this page's memory
  // only, for as long as the door says it is unchanged. The browser's own cache
  // is what carries it over a page reload, so no copy of a whole picture is
  // written into a store that cannot hold one.
  function rememberRead(path, bytes, type, seconds) {
    const at = Date.now();
    readFiles[path] = { at: at, until: at + seconds * 1000, bytes: bytes, type: type, fresh: seconds };
  }

  // Expiry is the only eviction: past its time an entry is dropped, so paths
  // that stopped being asked return their memory at the same moment.
  function sweep() {
    const now = Date.now();
    let dropped = false;
    for (const path in entries) {
      if (!(now < Number(entries[path].until))) {
        delete entries[path];
        dropped = true;
      }
    }
    for (const path in files) {
      if (!(now < Number(files[path].until))) {
        delete files[path];
        dropped = true;
      }
    }
    for (const path in readFiles) {
      if (!(now < Number(readFiles[path].until))) {
        delete readFiles[path];
        dropped = true;
      }
    }
    if (dropped) save();
  }

  // What this page may ask for: a file it made of its own bytes and is still
  // inside its window. A file the machine serves is not answered from here - it
  // has an address, and the browser keeps that address.
  function file(path) {
    const hit = files[pathOf(path)];
    if (!hit || !hit.page) return null;
    const now = Date.now();
    if (!(now < Number(hit.until))) return null;
    return String(hit.data);
  }

  // The last kept copy for a path, fresh or stale. Pages use it only to paint
  // themselves before asking again; it never answers an API request.
  function lastCopy(path) {
    const hit = entries[pathOf(path)];
    if (!hit) return null;
    const now = Date.now();
    return { text: hit.text, at: Number(hit.at), age: now - Number(hit.at), live: now < Number(hit.until) };
  }

  // The moment a kept copy of this path leaves the window the machine stated,
  // or 0 when nothing of it is held. A page that watches one door asks again at
  // that moment: a door is never asked twice inside one of its own windows.
  function staleAt(path) {
    const hit = entries[pathOf(path)];
    return hit ? Number(hit.until) : 0;
  }

  function clearFamily(prefix) {
    for (const path of Object.keys(entries)) {
      if (path.indexOf(prefix) === 0) delete entries[path];
    }
    for (const path of Object.keys(files)) {
      if (path.indexOf(prefix) === 0) delete files[path];
    }
    save();
  }

  // What pages may ask the store for: a file this page made of its own bytes, and
  // the last copy of a reading (with whether it is still inside its window, so a
  // panel can name when its rows were read). A file the machine serves is not
  // offered here: it has an address, and the browser's own cache keeps that
  // address because the file doors say the file does not change.
  window.olsCache = {
    file: file,
    keep: function (path, data, type, seconds) {
      keepFile(pathOf(path), String(data), String(type), Number(seconds) > 0 ? Number(seconds) : 31536000);
    },
    lastCopy: lastCopy,
    staleAt: staleAt,
  };

  // A stored answer replays as an equivalent live one: same body, same
  // cache-control, status 200.
  function replay(path) {
    const hit = entries[path];
    return new Response(hit.text, {
      status: 200,
      headers: {
        'content-type': 'application/json',
        'cache-control': 'public, max-age=' + Number(hit.fresh || 0),
      },
    });
  }

  // A file this page read, replayed for another caller while the page is open.
  function replayFile(path) {
    const read = readFiles[path];
    if (read && Date.now() < Number(read.until)) {
      return new Response(read.bytes, {
        status: 200,
        headers: {
          'content-type': String(read.type),
          'cache-control': 'public, max-age=' + Number(read.fresh || 0),
        },
      });
    }
    const hit = files[path];
    if (hit && hit.page && Date.now() < Number(hit.until)) {
      return new Response(dataToBytes(String(hit.data)), {
        status: 200,
        headers: {
          'content-type': String(hit.type),
          'cache-control': 'public, max-age=' + Number(hit.fresh || 0),
        },
      });
    }
    return null;
  }

  const inner = window.fetch;

  window.fetch = async function (url, options) {
    const path = pathOf(url);
    const mode = String((options || {}).cache || '');
    // What a fetch's own standard words mean: 'no-store' and 'reload' ask the
    // machine now and take nothing from a kept copy. A reading that exists to
    // find out what changed may not be answered with the answer from before
    // the change, or it says "nothing changed" about a fact that just moved.
    const mayReplay = mode !== 'no-store' && mode !== 'reload';
    const method = String((options || {}).method || 'GET').toUpperCase();
    if (method === 'GET' && path) {
      sweep();
      const hit = entries[path];
      if (mayReplay && hit && Date.now() < Number(hit.until)) return replay(path);
      const keptFile = replayFile(path);
      if (keptFile) return keptFile;

      const waiting = pending[path];
      if (waiting) {
        try {
          await waiting;
        } catch (failedRead) {
          // The shared read failed; answer this caller with a fresh request.
          const retry = await inner(url, options);
          return retry;
        }
        const kept = entries[path];
        if (mayReplay && kept && Date.now() < Number(kept.until)) return replay(path);
        const keptNow = replayFile(path);
        if (keptNow) return keptNow;
        return await inner(url, options);
      }

      let original = null;
      const read = async function () {
        const res = await inner(url, options);
        original = res;
        if (res.status !== 200 || !res.ok) return null;
        const type = String(res.headers.get('content-type') || '');
        const seconds = freshSeconds(res);
        if (type.indexOf('json') >= 0) {
          let text;
          try {
            text = await res.text();
          } catch (badBody) {
            return null;
          }
          try {
            JSON.parse(text);
          } catch (notJson) {
            // Not JSON after all: nothing to keep, but the caller must still
            // read the body once.
            return { raw: text };
          }
          if (seconds > 0) store(path, text, seconds);
          return { text: text };
        }
        if (seconds > 0 && fileish(type)) {
          let bytes;
          try {
            bytes = await res.arrayBuffer();
          } catch (badBytes) {
            return null;
          }
          rememberRead(path, bytes, type, seconds);
          return { bytes: bytes, type: type };
        }
        return null;
      };
      pending[path] = read();
      let outcome;
      try {
        outcome = await pending[path];
      } catch (failedRead) {
        // A failed read must not poison the pending map: the path stays askable.
        delete pending[path];
        throw failedRead;
      }
      delete pending[path];
      if (outcome && outcome.text !== undefined) {
        // A door that answers now but refuses to be kept for a later visit still
        // owes this caller the words it gave: they were read here, so they are
        // handed over as they stand - looking them up in a store that keeps
        // nothing would answer with nothing.
        return new Response(outcome.text, {
          status: 200,
          headers: { 'content-type': 'application/json' },
        });
      }
      if (outcome && outcome.bytes) return replayFile(path);
      if (outcome && outcome.raw !== undefined) {
        return new Response(outcome.raw, { status: 200, headers: { 'content-type': 'application/json' } });
      }
      return original;
    }
    const res = await inner(url, options);
    if (res.ok && path && method !== 'GET' && method !== 'HEAD') clearFamily(familyOf(path));
    return res;
  };
})();
