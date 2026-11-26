// webui/ps-diff.js — the code tab's line diff: the port of src/gitcore/diff.cpp
// (spec §3.3; port design ide-research.md §3). A classic script, and the page
// loads it the way a classic is loaded: project.html carries it in a <script>
// tag of its own, ahead of project.js, so the one global PsDiff stands before
// the editor mounts — project.js only reads that global, never injects this
// file. No modules, no dependencies.
//
// The algorithm is the server's, shape for shape, so that where both run their
// answers agree: the prefix/suffix trim always runs first, the Myers O(ND)
// search over the trimmed core is taken inside the same cell budget, the trace
// it stands up while walking is bounded by that very number and no other, the
// patience fallback (the run over lines unique on both sides) answers when
// either is passed and the result says so with approx:true, honestly. The tie-
// break of the search and of the backtrack is copied verbatim from diff.cpp —
// that choice of path is what makes a client change run stand where the
// server's hunk stands. And a line carries the newline that ended it: when the
// two sides disagree about the final newline, the last line's identity differs
// too, so a trailing-newline change is a hunk here and on the server alike —
// never silence.
//
// The two choices worth naming:
//
//  - splitLines mirrors split_lines (diff.cpp) meaning for meaning: text
//    splits at '\n' alone — a '\r' stays part of the line, which is what makes
//    a CRLF-vs-LF pair differ at all, exactly as test_diff.cpp's EOL test
//    claims — and a text's trailing '\n' does NOT open an empty last line:
//    "a\n" is one line, "a\n\n" is two. A text not ending at a '\n' keeps its
//    last partial line; the empty text is no lines at all. (The C++ split also
//    reports the missing final newline as a flag for the unified emitter's
//    "\ No newline" markers; splitLinesData reports it too, and diffLines
//    folds it into the last line's identity when the two sides disagree, so
//    the flag does cross into the answers — the server's fold, mirrored.)
//
//  - Failure is a return value, never a thrown exception, as in gitcore:
//    what is not text is taken as the empty text, and a giant past 2,000,000
//    lines on either side is refused the search and the patience both — the
//    whole block answers instead (every base line deleted, every new line
//    added, approx:true) rather than the tab hanging on it. The C++ search's
//    own int domain (kLineCountLimit) is ported too; the refusal simply
//    stands long before it.

(function () {
  'use strict';

  // The cell budget between the exact search and the patience fallback — the
  // one truth of the server's too: DiffOptions::cellBudget, diff.h. Both
  // gates answer to it: the core the search may afford (coreA × coreB) and
  // the trace it may stand up while walking (one row of 2*(n+m)+1 per edit
  // distance). There is no separate working-set limit, and there never is
  // again: one number, or the two engines would answer differently.
  var CELL_BUDGET = 4000000;
  // Past this many lines on either side the whole block answers at once: the
  // tab's refusal, so a hostile paste is a flat answer and not a hang.
  var MAX_LINES = 2000000;
  // The line counts the search works in int (the diagonal offset, the row,
  // the column): kLineCountLimit, diff.cpp. The refusal above keeps this from
  // ever mattering here; it is ported so the windows read the same as the C++.
  var kLineCountLimit = 1 << 30;
  // The widest grid the search's numbering holds: width = 2*(n+m)+1 stays
  // inside an int only while the SPAN n+m stays under kLineCountLimit — the
  // server's kGridSpanLimit, and the guard that refuses before any
  // allocation. (JS numbers would not wrap; the parity is the point, so the
  // two engines refuse the same windows.)
  var kGridSpanLimit = (1 << 30) - 1;

  // What is handed in becomes text: what is not text is the empty text, since
  // nothing here escapes as an exception.
  function asText(value) {
    if (typeof value === 'string') return value;
    if (value === null || value === undefined) return '';
    try {
      return String(value);
    } catch (notText) {
      return '';
    }
  }

  // split_lines, diff.cpp:464: bytes to lines at '\n' alone. The trailing
  // newline does not open an empty last line; a last line without one is
  // still a line (and says so through noNewline, which only the splitter
  // itself reports — the diff's answers never depend on it).
  function splitLinesData(text) {
    var lines = [];
    if (text.length === 0) {
      return { lines: lines, noNewline: false };
    }
    var pos = 0;
    for (; ;) {
      var nl = text.indexOf('\n', pos);
      if (nl === -1) {
        lines.push(text.slice(pos));
        return { lines: lines, noNewline: true };
      }
      lines.push(text.slice(pos, nl));
      pos = nl + 1;
      if (pos === text.length) {
        return { lines: lines, noNewline: false };
      }
    }
  }

  // The same line count without standing the array: what the giant's refusal
  // reads before it commits to splitting either side.
  function countLines(text) {
    if (text.length === 0) return 0;
    var count = 0;
    var at = text.indexOf('\n');
    while (at !== -1) {
      ++count;
      at = text.indexOf('\n', at + 1);
    }
    return text.charAt(text.length - 1) === '\n' ? count : count + 1;
  }

  // ---- the edit script --------------------------------------------------------

  // One operation of the edit script, already in the public shape: 'c' common
  // to both sides, 'd' only in the base (a line deleted from it), 'a' only in
  // the new text (a line added by it). baseLine and newLine are the row and
  // the column the operation stands at, exactly as the C++ Op carries them
  // (diff.cpp:34-38): a common line names both, a base-only line names its
  // row and the column it stands beside in the new text, a new-only line
  // names the row it stands beside in the base and its column.
  function commonOp(a, b) { return { kind: 'c', baseLine: a, newLine: b }; }
  function delOp(a, bRef) { return { kind: 'd', baseLine: a, newLine: bRef }; }
  function addOp(aRef, b) { return { kind: 'a', baseLine: aRef, newLine: b }; }

  // The Myers search over the window [a0,a1) x [b0,b1), appending operations
  // at the positions inside it — myers_window, diff.cpp. Refuses (false,
  // having appended nothing) when the window stands past the search's own
  // numbering, or when the trace it would need passes the caller's cell
  // budget — the same number that decided the search was worth taking, so
  // one truth governs both gates — so the caller can fall back.
  function myersWindow(a, b, a0, a1, b0, b1, budget, ops) {
    var nAll = a1 - a0, mAll = b1 - b0;
    if (nAll + mAll > kGridSpanLimit) return false;
    if (nAll === 0) {
      for (var j = 0; j < mAll; ++j) ops.push(addOp(a0, b0 + j));
      return true;
    }
    if (mAll === 0) {
      for (var i = 0; i < nAll; ++i) ops.push(delOp(a0 + i, b0));
      return true;
    }

    var n = nAll, m = mAll;
    var maxD = n + m;
    var width = 2 * maxD + 1;
    var off = maxD;

    var trace = [];
    // The row is measured before it is stood up: a width past the budget is
    // an allocation the answer is not worth, and refusals come back false.
    if (width > budget) return false;
    var v = new Int32Array(width); // vector<int>(width, 0)

    for (var d = 0; d <= maxD; ++d) {
      // The trace is one row of width per edit distance, bounded by the
      // caller's cell budget — the documented fallback rather than a second,
      // unstated limit. The same integer-division gate as diff.cpp.
      if (trace.length + 1 > Math.floor(budget / width)) return false;
      trace.push(v.slice());
      for (var k = -d; k <= d; k += 2) {
        var kx = k + off;
        var x;
        // The tie-break verbatim from diff.cpp:102. It is not decoration:
        // this is the path choice that makes the client's answer and the
        // server's one answer.
        if (k === -d || (k !== d && v[kx - 1] < v[kx + 1])) {
          x = v[kx + 1]; // down: a line taken only from b
        } else {
          x = v[kx - 1] + 1; // right: a line taken only from a
        }
        var y = x - k;
        while (x < n && y < m && a[a0 + x] === b[b0 + y]) {
          ++x;
          ++y;
        }
        v[kx] = x;
        if (x >= n && y >= m) {
          // The path is found; walk it back from the end of the grid, the
          // backtrack of diff.cpp:115-149 with its tie-break kept verbatim.
          var back = [];
          var cx = n, cy = m;
          for (var dd = trace.length - 1; dd >= 0; --dd) {
            var vd = trace[dd];
            var kk = cx - cy;
            var prevK;
            if (kk === -dd || (kk !== dd && vd[kk - 1 + off] < vd[kk + 1 + off])) {
              prevK = kk + 1;
            } else {
              prevK = kk - 1;
            }
            var px = vd[prevK + off];
            var py = px - prevK;
            while (cx > px && cy > py) {
              back.push(commonOp(a0 + cx - 1, b0 + cy - 1));
              --cx;
              --cy;
            }
            if (dd > 0) {
              if (prevK === kk + 1) {
                back.push(addOp(a0 + px, b0 + py));
              } else {
                back.push(delOp(a0 + px, b0 + py));
              }
            }
            cx = px;
            cy = py;
          }
          back.reverse();
          for (var t = 0; t < back.length; ++t) ops.push(back[t]);
          return true;
        }
      }
    }
    return false; // the grid was not reached: cannot happen for its sizes
  }

  // Solves one window exactly within the budget — and cuts it honestly when
  // it cannot: every line of a removed, every line of b added. solve_window,
  // diff.cpp:158.
  function solveWindow(a, b, a0, a1, b0, b1, budget, ops) {
    var n = a1 - a0, m = b1 - b0;
    if (n === 0 && m === 0) return;
    if (n === 0) {
      for (var j = 0; j < m; ++j) ops.push(addOp(a0, b0 + j));
      return;
    }
    if (m === 0) {
      for (var i = 0; i < n; ++i) ops.push(delOp(a0 + i, b0));
      return;
    }
    // n <= budget / m in C++ integer division reads exactly as the product
    // test below at these sizes (both sides bounded well under 2^53).
    if (n * m <= budget) {
      if (myersWindow(a, b, a0, a1, b0, b1, budget, ops)) return;
    }
    for (var i2 = 0; i2 < n; ++i2) ops.push(delOp(a0 + i2, b0));
    for (var j2 = 0; j2 < m; ++j2) ops.push(addOp(a0, b0 + j2));
  }

  // The patience-style fallback for one window: lines whose content appears
  // exactly once on each side are anchors, the longest increasing run of their
  // columns is the common subsequence, and the windows between the anchors are
  // solved exactly when the budget allows and cut honestly when it does not —
  // patience_window, diff.cpp:197. The caller has already set the answer's
  // approx flag; this only says what the lines are.
  function patienceWindow(a, b, a0, a1, b0, b1, budget, ops) {
    var n = a1 - a0, m = b1 - b0;
    if (n === 0 || m === 0) {
      solveWindow(a, b, a0, a1, b0, b1, budget, ops);
      return;
    }
    // The anchors are held as int positions for the run, so the same domain
    // binds this pass too: past it the honest answer is the cut (diff.cpp:209).
    if (n > kLineCountLimit || m > kLineCountLimit) {
      solveWindow(a, b, a0, a1, b0, b1, budget, ops);
      return;
    }

    // Lines that appear exactly once on each side: a first position, or -1
    // once the content stands more than once (diff.cpp:217-236).
    var onceInA = new Map();
    for (var i = 0; i < n; ++i) {
      var lineA = a[a0 + i];
      if (onceInA.has(lineA)) {
        if (onceInA.get(lineA) >= 0) onceInA.set(lineA, -1);
      } else {
        onceInA.set(lineA, i);
      }
    }
    var onceInB = new Map();
    for (var j = 0; j < m; ++j) {
      var lineB = b[b0 + j];
      if (onceInB.has(lineB)) {
        if (onceInB.get(lineB) >= 0) onceInB.set(lineB, -1);
      } else {
        onceInB.set(lineB, j);
      }
    }

    // The anchors (row, column), ordered by row (diff.cpp:237-249).
    var anchorRow = [], anchorCol = [];
    for (var r = 0; r < n; ++r) {
      var line = a[a0 + r];
      var inA = onceInA.get(line);
      if (inA === undefined || inA < 0) continue;
      var inB = onceInB.get(line);
      if (inB === undefined || inB < 0) continue;
      anchorRow.push(r);
      anchorCol.push(inB);
    }

    // Patience: the longest increasing run of the anchors' columns. tails[len]
    // stands for the index into the anchors of the smallest column that ends a
    // run of length len+1; the binary search is the lower_bound of diff.cpp:256
    // with its comparator (first tail whose column is not below col).
    var count = anchorRow.length;
    var tails = [];
    var parent = new Int32Array(count);
    parent.fill(-1);
    for (var t = 0; t < count; ++t) {
      var col = anchorCol[t];
      var lo = 0, hi = tails.length;
      while (lo < hi) {
        var mid = (lo + hi) >> 1;
        if (anchorCol[tails[mid]] < col) lo = mid + 1;
        else hi = mid;
      }
      var p = lo === 0 ? -1 : parent[tails[lo - 1]];
      if (lo === tails.length) tails.push(t);
      else tails[lo] = t;
      parent[t] = p;
    }
    var run = [];
    if (tails.length > 0) {
      var cur = tails[tails.length - 1];
      for (; ;) {
        run.push(cur);
        var prev = parent[cur];
        if (prev < 0) break;
        cur = prev;
      }
      run.reverse();
    }

    // The windows between the kept anchors (diff.cpp:283-292).
    var rowCut = a0, colCut = b0;
    for (var w = 0; w < run.length; ++w) {
      var row = a0 + anchorRow[run[w]];
      var column = b0 + anchorCol[run[w]];
      solveWindow(a, b, rowCut, row, colCut, column, budget, ops);
      ops.push(commonOp(row, column));
      rowCut = row + 1;
      colCut = column + 1;
    }
    solveWindow(a, b, rowCut, a1, colCut, b1, budget, ops);
  }

  // The whole edit script: the trim always runs, the core is taken exactly
  // when the budget allows, and the flag says honestly whether the fallback
  // answered. compute_ops, diff.cpp:297.
  function computeOps(a, b, budget, ops) {
    var n = a.length, m = b.length;

    var prefix = 0;
    while (prefix < n && prefix < m && a[prefix] === b[prefix]) ++prefix;
    var suffix = 0;
    while (suffix < n - prefix && suffix < m - prefix &&
      a[n - 1 - suffix] === b[m - 1 - suffix]) ++suffix;

    ops.length = 0;
    for (var i = 0; i < prefix; ++i) ops.push(commonOp(i, i));

    var coreA = n - prefix - suffix;
    var coreB = m - prefix - suffix;
    var approx = false;
    // coreA <= budget / coreB in C++ integer division reads exactly as the
    // product test below at these sizes.
    if (coreA === 0 || coreB === 0 || coreA * coreB <= budget) {
      // Inside the cell budget: the exact search answers the core.
      var core = [];
      if (!myersWindow(a, b, prefix, n - suffix, prefix, m - suffix, budget, core)) {
        approx = true; // the trace working set was passed: the fallback answers
        patienceWindow(a, b, prefix, n - suffix, prefix, m - suffix, budget, ops);
      } else {
        for (var t = 0; t < core.length; ++t) ops.push(core[t]);
      }
    } else {
      approx = true; // over the cell budget: the patience fallback answers
      patienceWindow(a, b, prefix, n - suffix, prefix, m - suffix, budget, ops);
    }

    for (var s = 0; s < suffix; ++s) ops.push(commonOp(n - suffix + s, m - suffix + s));
    return approx;
  }

  // The block answer for a giant or an accident: the whole of the base
  // deleted, the whole of the new added, with the reference positions the
  // honest cut gives at the window's origin (solve_window's cut, the window
  // here being everything).
  function wholeBlock(n, m) {
    var lines = [];
    for (var i = 0; i < n; ++i) lines.push(delOp(i, 0));
    for (var j = 0; j < m; ++j) lines.push(addOp(0, j));
    return lines;
  }

  // ---- the public surface -----------------------------------------------------

  // Text to lines on the server's semantics (split_lines, diff.cpp:464): the
  // array of lines, '\n' the only terminator, '\r' part of the line, a
  // trailing '\n' not opening an empty last line, "" giving no lines.
  function splitLines(text) {
    return splitLinesData(asText(text)).lines;
  }

  // The lines as the bytes had them: each line with the newline that ended it
  // back on it, and the last one only when the side ended with one. Under
  // this identity "x" and "x\n" are different lines — git's own reading, the
  // server's fold (with_terminators, diff.cpp), mirrored so the two engines
  // answer the same hunk to the same pair.
  function withTerminators(lines, noNewline) {
    var out = new Array(lines.length);
    for (var i = 0; i < lines.length; ++i) {
      out[i] = (i + 1 < lines.length || !noNewline) ? lines[i] + '\n' : lines[i];
    }
    return out;
  }

  // The diff of two texts as lines: { approx, lines: [{kind:'c'|'d'|'a',
  // baseLine, newLine}] }, the kinds mirroring HunkLine.kind (diff.h:56) and
  // the coordinates mirroring the C++ Op's. approx is true whenever the exact
  // search was cut — by the cell budget, by the trace working set, or by the
  // line-count refusal — and false when the script stands minimal over the
  // lines. opts = { cellBudget } overrides the budget for a caller that knows
  // better; the default is the server's 4,000,000 cells.
  function diffLines(baseText, newText, opts) {
    var budget = CELL_BUDGET;
    if (opts && typeof opts.cellBudget === 'number' && isFinite(opts.cellBudget) &&
      opts.cellBudget >= 0) {
      budget = Math.floor(opts.cellBudget);
    }
    var base = asText(baseText), fresh = asText(newText);
    var aLines = null, bLines = null;
    var nCount = 0, mCount = 0;
    try {
      nCount = countLines(base);
      mCount = countLines(fresh);
      if (nCount > MAX_LINES || mCount > MAX_LINES) {
        // The refusal: the counts answer from a scan alone; neither side is
        // split, and the block answers instead of a hang.
        return { approx: true, lines: wholeBlock(nCount, mCount) };
      }
      var aData = splitLinesData(base);
      var bData = splitLinesData(fresh);
      aLines = aData.lines;
      bLines = bData.lines;
      // The flag belongs to the last line's identity: a side that lost its
      // final newline changed a line, and the tab must see the change. The
      // fold is made once, here, and nowhere else — and only when the sides
      // disagree, because on every line before the last the newline stands
      // on both sides alike.
      if (aData.noNewline !== bData.noNewline) {
        aLines = withTerminators(aLines, aData.noNewline);
        bLines = withTerminators(bLines, bData.noNewline);
      }
      var ops = [];
      var approx = computeOps(aLines, bLines, budget, ops);
      return { approx: approx, lines: ops };
    } catch (accident) {
      // Never past here: the block answers what the lines reached, and the
      // empty answer what even that could not stand.
      try {
        return {
          approx: true,
          lines: wholeBlock(aLines ? aLines.length : nCount, bLines ? bLines.length : mCount)
        };
      } catch (alsoTooBig) {
        return { approx: true, lines: [] };
      }
    }
  }

  // The gutter's view of the same diff, as arrays of line indices:
  //   added    — indices in the NEW text of runs of pure addition;
  //   deleted  — indices in the BASE of runs of pure deletion (the lines that
  //              left with no replacement — the gutter marks them where its
  //              own model puts a vanished line, after the deletion point);
  //   modified — indices in the NEW text of runs that change something on both
  //              sides: a run of deletions with additions in it, the d-run-
  //              followed-by-a of the gutter's sentence. A modified run names
  //              only its new-side lines; its departed base lines belong to
  //              the modification, not to `deleted`, and diffLines carries
  //              them for a caller that wants the hunk inline.
  // A run's kinds may interleave as the search walked it; both-kinds is a
  // modification whichever order the operations stand in.
  function gutterMarks(baseText, newText, opts) {
    var added = [], deleted = [], modified = [];
    try {
      var lines = diffLines(baseText, newText, opts).lines;
      var i = 0;
      while (i < lines.length) {
        if (lines[i].kind === 'c') {
          ++i;
          continue;
        }
        var start = i;
        var hasD = false, hasA = false;
        while (i < lines.length && lines[i].kind !== 'c') {
          if (lines[i].kind === 'd') hasD = true;
          else hasA = true;
          ++i;
        }
        for (var t = start; t < i; ++t) {
          var op = lines[t];
          if (hasD && hasA) {
            if (op.kind === 'a') modified.push(op.newLine);
          } else if (hasA) {
            added.push(op.newLine);
          } else {
            deleted.push(op.baseLine);
          }
        }
      }
    } catch (accident) {
      // A gutter reads a clean file over no gutter at all.
    }
    return { added: added, deleted: deleted, modified: modified };
  }

  var root = typeof globalThis === 'object' ? globalThis
    : (typeof window === 'object' ? window : this);
  root.PsDiff = {
    splitLines: splitLines,
    diffLines: diffLines,
    gutterMarks: gutterMarks,
    // The budgets, read-only facts for a caller that wants to say what an
    // approx answer cost: the server's cell budget and the trace's working set.
    CELL_BUDGET: CELL_BUDGET,
    MAX_LINES: MAX_LINES
  };
})();
