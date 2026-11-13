// script.js - play scripts in the Workshop: the checker (mirrors src/script.c's parser, same messages),
// the function reference, and the ready-made scripts. Loaded by the page and by the tests.
(function (root) {
  'use strict';
  // name, fewest values, most values, what it does   (must match FN[] in src/script.c - tested)
  var FNS = [
    ['receivers', 0, 0, 'Your route runners this play (a list).'],
    ['defenders', 0, 0, 'Every defender (a list).'],
    ['offense', 0, 0, 'Every player on your offense (a list).'],
    ['defense', 0, 0, 'Same as defenders().'],
    ['deepest', 2, 2, 'deepest(list, n): the n players in the list furthest downfield.'],
    ['nearest', 4, 4, 'nearest(list, x, y, n): the n players in the list closest to a spot.'],
    ['sort_y', 1, 1, 'sort_y(list): the list ordered from the top sideline to the bottom.'],
    ['count', 1, 1, 'count(list): how many are in it.'],
    ['item', 2, 2, 'item(list, i): one player from a list, counting from 0.'],
    ['qb', 0, 0, 'Your quarterback.'],
    ['back', 0, 0, 'Your running back.'],
    ['ball', 0, 0, 'The ball.'],
    ['down', 0, 0, 'The down, 1 to 4.'],
    ['togo', 0, 0, 'Yards to go.'],
    ['quarter', 0, 0, 'The quarter.'],
    ['clock', 0, 0, 'Seconds left in the quarter.'],
    ['lead', 0, 0, 'Your score minus theirs.'],
    ['goal', 0, 0, 'Yards from the line of scrimmage to the goal line.'],
    ['los_x', 0, 0, 'The line of scrimmage (x on the field).'],
    ['fwd', 0, 0, 'Which way you are going: 1 or -1. Multiply downfield distances by it.'],
    ['field_top', 0, 0, 'The top sideline (y).'],
    ['field_bottom', 0, 0, 'The bottom sideline (y).'],
    ['legs', 0, 0, "The back's stamina, 0 to 100."],
    ['landing_x', 0, 0, 'Where your pass in the air comes down (x).'],
    ['landing_y', 0, 0, 'Where your pass in the air comes down (y).'],
    ['state', 0, 0, "The ball's state number (3 = in the air, 5 = someone has it)."],
    ['since_snap', 0, 0, 'Seconds since the snap.'],
    ['thrown', 0, 0, 'True while your pass is in the air.'],
    ['tempo', 0, 0, 'Seconds from the end of your last play to this snap (whole seconds; -1 on the first play of a drive).'],
    ['tackler', 0, 0, 'In on juke: the defender who was closing on your ball carrier.'],
    ['juke_ready', 1, 1, 'juke_ready(player): arm the JUKE prompt for him this play; a tap in time runs on juke.'],
    ['yard', 1, 1, 'yard(n): n yards in field units (1 yard is about 20.5).'],
    ['endzone_x', 1, 1, 'endzone_x(n): x of a spot n yards into the end zone you are attacking.'],
    ['random', 2, 2, 'random(a, b): a random number between a and b.'],
    ['chance', 1, 1, 'chance(p): true p percent of the time.'],
    ['abs', 1, 1, ''], ['min', 2, 2, ''], ['max', 2, 2, ''], ['floor', 1, 1, ''], ['round', 1, 1, ''], ['sqrt', 1, 1, ''],
    ['dist', 4, 4, 'dist(x1, y1, x2, y2): the distance between two spots.'],
    ['str', 1, 1, 'str(v): a number as text, to join with + into a message.'],
    ['game', 1, 1, 'game("name"): one of the game\'s own match values (down, scrimmage, minutes...).'],
    ['set_game', 2, 2, 'set_game("name", n): change one.'],
    ['global', 1, 1, 'global("name"): one of the game\'s global values.'],
    ['set_global', 2, 2, 'set_global("name", n): change one.'],
    ['call', 1, 7, 'call("routine", ...): run one of the game\'s own routines, with up to 6 numbers or texts.'],
    ['call_on', 2, 8, 'call_on(player, "routine", ...): the same, run as that player.'],
    ['get', 2, 2, 'get(player, "name"): any of a player\'s game values (same as player.name).'],
    ['set', 3, 3, 'set(player, "name", n): change one (same as player.name = n).'],
    ['place', 3, 3, 'place(player, x, y): put a player on a spot.'],
    ['steer', 3, 4, 'steer(player, x, y [, strength 0-1]): make him run toward a spot this frame. Use it in on frame.'],
    ['speed', 2, 2, 'speed(player, percent): change how fast he accelerates for this play.'],
    ['route', 2, 2, 'route(player, "route_hitch"): give a route. Names: route_hitch, route_slant, route_dig2, route_curl, route_post, route_corner, route_streak, route_out2, route_flat.'],
    ['carry', 1, 2, 'carry(x [, arc]): in on release, the throw carries along your aim until it reaches x. arc is how fast it rises (normal deep ball 5).'],
    ['icon', 1, 1, 'icon(player): a marker at the screen edge pointing to him while he is off screen. Call it in on frame.'],
    ['icons', 1, 1, 'icons(list): the same for a whole list.'],
    ['spread', 4, 4, 'spread(list, x, y_from, y_to): line them up evenly across the field at x.'],
    ['pressed', 1, 1, 'pressed("Label"): true when that pre-snap button is on this play.'],
    ['button', 1, 1, 'button("Label"): add a pre-snap button (in on presnap).'],
    ['msg', 1, 1, 'msg("text"): show a message before the snap.'],
    ['tune', 2, 2, 'tune("setting", n): change a Tune setting for this play (fractions: 0.2 is 20%).'],
    ['tune_add', 2, 2, 'tune_add("setting", n): add to one for this play.'],
    ['log', 1, 6, 'log(...): write to rbmod.log, to see what your script is doing.']
  ];
  var EVENTS = [['presnap', 'Before the snap, once a play.'], ['button', 'on button "Label": when that pre-snap button is pressed.'],
    ['snap', 'The moment the play starts.'], ['release', 'The instant the ball leaves your hand (carry() works here).'],
    ['frame', 'Every frame of your play, from the snap until it ends (60 a second).'], ['throw', 'Just after a pass is out and its landing spot is known.'],
    ['catch', ''], ['incomplete', ''], ['intercepted', ''], ['sack', ''], ['touchdown', ''], ['play_end', 'After every run or pass play.'],
    ['drive_end', 'Your drive is over: you lost the ball (score, punt, turnover) or the half ended.'], ['juke', 'You tapped JUKE in time (juke_ready armed it).']];
  var KW = ['on', 'end', 'if', 'elif', 'else', 'for', 'in', 'repeat', 'stop', 'and', 'or', 'not', 'true', 'false', 'nil'];

  function check(src) {
    var toks = [], line = 1, i = 0, s = String(src), err = null;
    function fail(l, m) { if (!err) err = { line: l, msg: m }; }
    while (i < s.length && !err) {
      var c = s[i];
      if (c === '#') { while (i < s.length && s[i] !== '\n') i++; continue; }
      if (c === '\n') { if (toks.length && toks[toks.length - 1].t !== 'nl') toks.push({ t: 'nl', line: line }); line++; i++; continue; }
      if (c === ' ' || c === '\t' || c === '\r') { i++; continue; }
      if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(s[i + 1] || ''))) { var m = /^[0-9]*\.?[0-9]*/.exec(s.slice(i))[0]; toks.push({ t: 'num', line: line }); i += m.length; continue; }
      if (/[A-Za-z_]/.test(c)) { var w = /^[A-Za-z_][A-Za-z0-9_]*/.exec(s.slice(i))[0]; toks.push({ t: 'id', s: w, line: line }); i += w.length; continue; }
      if (c === '"') { var e = s.indexOf('"', i + 1), nlp = s.indexOf('\n', i + 1); if (e < 0 || (nlp >= 0 && nlp < e)) { fail(line, 'text is missing its closing quote'); break; } toks.push({ t: 'str', s: s.slice(i + 1, e), line: line }); i = e + 1; continue; }
      var two = s.substr(i, 2);
      if (['==', '!=', '<=', '>='].indexOf(two) >= 0) { toks.push({ t: 'op', s: two, line: line }); i += 2; continue; }
      if ('+-*/%<>=().,'.indexOf(c) >= 0) { toks.push({ t: 'op', s: c, line: line }); i++; continue; }
      fail(line, 'unexpected character ' + c); break;
    }
    if (err) return err;
    if (toks.length && toks[toks.length - 1].t !== 'nl') toks.push({ t: 'nl', line: line });
    toks.push({ t: 'eof', line: line });
    var p = 0, handlers = [];
    function cur() { return toks[p]; }
    function kw(t, w) { return t.t === 'id' && t.s === w; }
    function op(t, o) { return t.t === 'op' && t.s === o; }
    function reserved(w) { return KW.indexOf(w) >= 0; }
    function nl() { if (cur().t === 'nl') { p++; return true; } fail(cur().line, op(cur(), '=') ? 'use == to compare (a single = sets a variable)' : 'expected the end of the line here'); return false; }
    function fn(name) { for (var k = 0; k < FNS.length; k++) if (FNS[k][0] === name) return FNS[k]; return null; }
    function primary() {
      var t = cur();
      if (t.t === 'num' || t.t === 'str') { p++; return { k: 'v' }; }
      if (op(t, '(')) { p++; var e = expr(); if (!e) return null; if (!op(cur(), ')')) { fail(cur().line, 'missing )'); return null; } p++; return e; }
      if (t.t === 'id') {
        if (kw(t, 'true') || kw(t, 'false') || kw(t, 'nil')) { p++; return { k: 'v' }; }
        if (reserved(t.s)) { fail(t.line, 'unexpected word ' + t.s); return null; }
        p++;
        if (op(cur(), '(')) {
          var f = fn(t.s); if (!f) { fail(t.line, 'unknown function ' + t.s); return null; }
          p++; var argc = 0;
          if (!op(cur(), ')')) for (;;) { if (!expr()) return null; argc++; if (op(cur(), ',')) { p++; continue; } break; }
          if (!op(cur(), ')')) { fail(cur().line, 'missing ) after the arguments of ' + f[0]); return null; }
          p++;
          if (argc < f[1] || argc > f[2]) { fail(t.line, f[1] === f[2] ? f[0] + '() takes ' + f[1] + ' value' + (f[1] === 1 ? '' : 's') + ', got ' + argc : f[0] + '() takes ' + f[1] + ' to ' + f[2] + ' values, got ' + argc); return null; }
          return { k: 'call' };
        }
        return { k: 'var' };
      }
      fail(t.line, 'expected a value here' + (t.t === 'op' ? ' ' + t.s : '')); return null;
    }
    function postfix() { var e = primary(); while (e && op(cur(), '.')) { p++; if (cur().t !== 'id') { fail(cur().line, 'expected a name after .'); return null; } p++; e = { k: 'member' }; } return e; }
    function unary() { if (op(cur(), '-') || kw(cur(), 'not')) { p++; return unary() ? { k: 'v' } : null; } return postfix(); }
    function bin(level) {
      if (level > 4) return unary();
      var a = bin(level + 1);
      while (a) {
        var t = cur(), ok = false;
        if (level === 0 && kw(t, 'or')) ok = true;
        else if (level === 1 && kw(t, 'and')) ok = true;
        else if (level === 2 && t.t === 'op' && ['==', '!=', '<', '>', '<=', '>='].indexOf(t.s) >= 0) ok = true;
        else if (level === 3 && t.t === 'op' && (t.s === '+' || t.s === '-')) ok = true;
        else if (level === 4 && t.t === 'op' && (t.s === '*' || t.s === '/' || t.s === '%')) ok = true;
        if (!ok) break;
        p++; if (!bin(level + 1)) return null; a = { k: 'v' };
      }
      return a;
    }
    function expr() { return bin(0); }
    function block() {
      for (;;) {
        var t = cur();
        if (t.t === 'nl') { p++; continue; }
        if (t.t === 'eof') { fail(t.line, 'missing end'); return 0; }
        if (kw(t, 'end')) return 1; if (kw(t, 'elif')) return 2; if (kw(t, 'else')) return 3;
        if (kw(t, 'on')) { fail(t.line, 'missing end before the next on'); return 0; }
        if (!stmt()) return 0;
      }
    }
    function stmt() {
      var t = cur();
      if (kw(t, 'if')) {
        p++; if (!expr() || !nl()) return false;
        var how = block(); if (!how) return false;
        while (how === 2) { p++; if (!expr() || !nl()) return false; how = block(); if (!how) return false; }
        if (how === 3) { p++; if (!nl()) return false; how = block(); if (!how) return false; if (how !== 1) { fail(cur().line, 'else must be followed by end'); return false; } }
        p++; return nl();
      }
      if (kw(t, 'for')) {
        p++; var v = cur(); if (v.t !== 'id' || reserved(v.s)) { fail(v.line, 'expected a name after for'); return false; }
        p++; if (!kw(cur(), 'in')) { fail(cur().line, 'expected in'); return false; }
        p++; if (!expr() || !nl()) return false;
        var h2 = block(); if (!h2) return false; if (h2 !== 1) { fail(cur().line, 'for must end with end'); return false; }
        p++; return nl();
      }
      if (kw(t, 'repeat')) {
        p++; if (!expr() || !nl()) return false;
        var h3 = block(); if (!h3) return false; if (h3 !== 1) { fail(cur().line, 'repeat must end with end'); return false; }
        p++; return nl();
      }
      if (kw(t, 'stop')) { p++; return nl(); }
      if (t.t === 'id' && !reserved(t.s) && op(toks[p + 1], '=')) { p += 2; return !!expr() && nl(); }
      var e = expr(); if (!e) return false;
      if (op(cur(), '=')) { if (e.k !== 'member') { fail(t.line, "only a name or a player's .variable can be set"); return false; } p++; return !!expr() && nl(); }
      if (e.k !== 'call') { fail(t.line, "this line doesn't do anything (expected a function call or an assignment)"); return false; }
      return nl();
    }
    var EV = EVENTS.map(function (x) { return x[0]; });
    for (;;) {
      var t = cur();
      if (t.t === 'nl') { p++; continue; }
      if (t.t === 'eof') break;
      if (!kw(t, 'on')) { fail(t.line, 'everything must be inside an  on <event> ... end  block'); break; }
      p++;
      var ev = cur();
      if (ev.t !== 'id' || EV.indexOf(ev.s) < 0) { fail(ev.line, 'unknown event' + (ev.t === 'id' ? ' ' + ev.s : '')); break; }
      p++;
      var label = '';
      if (ev.s === 'button') { if (cur().t !== 'str') { fail(cur().line, "on button needs the button's label in quotes"); break; } label = cur().s; p++; }
      if (!nl()) break;
      var how = block(); if (!how) break;
      if (how !== 1) { fail(cur().line, how === 2 ? 'elif without if' : 'else without if'); break; }
      p++; if (cur().t !== 'eof' && !nl()) break;
      handlers.push(ev.s + (label ? ' "' + label + '"' : ''));
    }
    return err || { ok: true, handlers: handlers };
  }

  var HAIL = '# Hail Mary: when the Hail Mary button is on, your receivers and their deep\n' +
    '# defenders are lined up in the end zone at the snap, and the throw carries\n' +
    '# there in whatever direction you aim. It ends as a jump ball.\n' +
    '# (Goes with the Hail Mary rules, which add the button.)\n\n' +
    'on snap\n  if pressed("Hail Mary")\n    rec = receivers()\n' +
    '    # receivers across the end zone, 2 yards deep, sideline to sideline\n' +
    '    spread(rec, endzone_x(2), field_top() + 50, field_bottom() - 50)\n' +
    '    # one deep defender on each receiver, plus one more\n' +
    '    rec = sort_y(rec)\n    i = 0\n    for d in deepest(defenders(), count(rec) + 1)\n' +
    '      r = item(rec, i % count(rec))\n      place(d, r.x + fwd() * yard(1), r.y + 18)\n      i = i + 1\n    end\n  end\nend\n\n' +
    'on frame\n  if pressed("Hail Mary")\n    icons(receivers())\n    for r in receivers()\n      if thrown()\n' +
    '        steer(r, landing_x(), landing_y())\n      else\n        steer(r, endzone_x(2), r.y, 0.3)\n      end\n    end\n' +
    '    if thrown()\n      for d in nearest(defenders(), landing_x(), landing_y(), count(receivers()) + 1)\n' +
    '        steer(d, landing_x(), landing_y())\n      end\n    end\n  end\nend\n\n' +
    'on release\n  if pressed("Hail Mary")\n    carry(endzone_x(5), 5)\n  end\nend\n';
  var EXAMPLES = [{ title: 'Hail Mary', about: 'Lines your receivers and their deep defenders up in the end zone at the snap, carries the throw there, and shows edge icons. Needs the Hail Mary rules for the button.', text: HAIL }];

  var api = { FNS: FNS, EVENTS: EVENTS, check: check, EXAMPLES: EXAMPLES };
  if (typeof module !== 'undefined' && module.exports) module.exports = api; else root.WS_SCRIPT = api;
})(typeof window !== 'undefined' ? window : this);
