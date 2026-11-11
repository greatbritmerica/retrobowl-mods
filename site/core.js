// core.js - the workshop's logic with no page in it, so the same code runs in the page and in tests.
// Everything here must agree with src/rules.c (the mod's parser) and tools/tunables.json.
(function (root) {
  'use strict';

  // ---- the blocks a rule is built from (mirrors src/rules.c) ----
  var EVENTS = [
    { id: 'presnap', label: 'Before the snap', hint: 'Once each play, when your pre-snap panel appears. The place to add a button or a message.' },
    { id: 'snap', label: 'At the snap', hint: 'The moment the play starts. Speed changes belong here.' },
    { id: 'throw', label: 'A pass is thrown', hint: 'Just after release, before the defender\'s play is decided, so a rule can still change this throw\'s chances.' },
    { id: 'catch', label: 'A pass is caught', hint: '' },
    { id: 'incomplete', label: 'A pass falls incomplete', hint: '' },
    { id: 'intercepted', label: 'A pass is intercepted', hint: '' },
    { id: 'sack', label: 'You are sacked', hint: '' },
    { id: 'touchdown', label: 'You score a touchdown', hint: '' },
    { id: 'play_end', label: 'The play ends', hint: 'After every one of your run or pass plays. The place to count things.' },
    { id: 'button', label: 'A button is pressed', hint: 'A button one of your "Before the snap" rules added.', arg: true }
  ];
  // kind: num (compare to a number), yesno (is / is not), choice (pick one), counter (named), pressed (button name)
  var VARS = [
    { id: 'down', label: 'Down', kind: 'num', unit: '', hint: '1 to 4' },
    { id: 'togo', label: 'Yards to go', kind: 'num', unit: 'yds' },
    { id: 'legs', label: "Back's legs", kind: 'num', unit: '%', hint: '0 to 100' },
    { id: 'run_share', label: 'How often you run lately', kind: 'num', unit: '%' },
    { id: 'pass_dial', label: 'Pass coverage tightness', kind: 'num', unit: '%' },
    { id: 'run_dial', label: 'Run defense tightness', kind: 'num', unit: '%' },
    { id: 'held', label: 'Seconds from snap to throw', kind: 'num', unit: 's', from: 'throw' },
    { id: 'depth', label: 'Throw depth', kind: 'num', unit: 'yds', from: 'throw' },
    { id: 'yards', label: 'Yards the play gained', kind: 'num', unit: 'yds', from: 'end' },
    { id: 'is_run', label: 'It is a run', kind: 'yesno', from: 'snap' },
    { id: 'worked', label: 'The play worked', kind: 'yesno', from: 'end' },
    { id: 'play_action', label: 'Play action is on', kind: 'yesno' },
    { id: 'scrambled', label: 'You scrambled', kind: 'yesno', from: 'snap' },
    { id: 'protection', label: 'Protection call', kind: 'choice', choices: [[0, 'Normal'], [1, 'Max protect'], [2, 'Slide up'], [3, 'Slide down']] },
    { id: 'quarter', label: 'Quarter', kind: 'num', unit: '', hint: '1 to 4' },
    { id: 'clock', label: 'Seconds left in the quarter', kind: 'num', unit: 's' },
    { id: 'score_diff', label: 'Your lead', kind: 'num', unit: 'pts', hint: 'negative when you trail' },
    { id: 'goal', label: 'Yards to the end zone', kind: 'num', unit: 'yds' },
    { id: 'chance', label: 'Random chance', kind: 'num', unit: '%', hint: '"less than 30" is true 30% of the time' },
    { id: 'var', label: 'A counter', kind: 'counter' },
    { id: 'pressed', label: 'A button is on', kind: 'pressed' }
  ];
  var OPS = [['=', 'is'], ['!=', 'is not'], ['<', 'less than'], ['<=', 'at most'], ['>', 'more than'], ['>=', 'at least']];
  var GROUPS = [['qb', 'Your quarterback'], ['back', 'Your back'], ['receivers', 'Your receivers'], ['line', 'Your line'],
                ['rushers', 'Their pass rushers'], ['coverage', 'Their coverage'], ['defense', 'Their whole defense']];
  var ACTIONS = [
    { id: 'sneak', label: 'QB sneak: snap it, he keeps it', when: 'pre' },
    { id: 'push', label: 'Push: line and back drive the pile', when: 'start' },
    { id: 'kneel', label: 'Kneel: snap it and take a knee', when: 'pre' },
    { id: 'deep', label: 'Send every receiver deep', when: 'pre' },
    { id: 'penalty', label: 'Call a penalty', when: 'after' },
    { id: 'tune', label: 'Change a setting' },
    { id: 'speed', label: 'Speed a group up or down' },
    { id: 'legs', label: "Change the back's legs" },
    { id: 'button', label: 'Add a pre-snap button' },
    { id: 'msg', label: 'Show a message before the snap' },
    { id: 'var', label: 'Change a counter' }
  ];
  var AFTER = ['catch', 'incomplete', 'intercepted', 'sack', 'touchdown', 'play_end'];
  var LIMITS = { rules: 64, conds: 6, acts: 8, buttons: 3, name: 39, label: 31, msg: 71, counter: 24 };

  function ident(s) { return String(s || '').replace(/[^A-Za-z0-9_]/g, '_').slice(0, LIMITS.counter); }
  function oneLine(s, max) { return String(s || '').replace(/[\r\n\t]+/g, ' ').replace(/ +/g, ' ').trim().slice(0, max); }
  function num(v) { var n = Number(v); if (!isFinite(n)) n = 0; return String(Math.round(n * 1e6) / 1e6); }
  function byKey(T) { var m = {}; T.forEach(function (t) { m[t.key] = t; }); return m; }

  // ---- settings file ----
  function settingsText(T, values) {
    var out = ['# Retro Bowl mod settings - made with the Workshop', '# Save as rbmod_settings.txt in the game\'s folder. Lines starting with # are ignored.'];
    var group = null;
    T.forEach(function (t) {
      if (t.group !== group) { group = t.group; out.push('', '# ' + group); }
      var v = values[t.key] === undefined ? t.def : values[t.key];
      out.push(t.key + ' = ' + num(v));
    });
    return out.join('\n') + '\n';
  }
  function parseSettings(T, text) {
    var keys = byKey(T), values = {}, unknown = [];
    String(text).split(/\r?\n/).forEach(function (line) {
      line = line.trim();
      if (!line || line[0] === '#') return;
      var m = /^([A-Za-z0-9_]+)\s*=?\s*(-?[0-9.]+(?:e-?[0-9]+)?)$/.exec(line);
      if (!m) return;
      if (!keys[m[1]]) { unknown.push(m[1]); return; }
      values[m[1]] = Number(m[2]);
    });
    return { values: values, unknown: unknown };
  }
  function changed(T, values) {
    return T.filter(function (t) { return values[t.key] !== undefined && Math.abs(values[t.key] - t.def) > 1e-9; });
  }

  // ---- rules file ----
  // rule: { name, ev, arg, conds:[{v, name, op, val}], acts:[{t, ...}] }
  function condLine(c) {
    var v = c.v === 'var' ? 'var.' + ident(c.name) : c.v === 'pressed' ? 'pressed.' + oneLine(c.name, LIMITS.label).replace(/ /g, '_') : c.v;
    return 'if ' + v + ' ' + c.op + ' ' + num(c.val);
  }
  function actLine(a, T) {
    if (a.t === 'tune') {
      var t = byKey(T)[a.key], v = Number(a.val);
      if (t && t.pct && a.op !== '*') v = v / 100;          // percent settings are typed as percents, stored as fractions
      return 'then tune ' + a.key + ' ' + a.op + ' ' + num(v) + ' ' + (a.keep ? 'keep' : 'play');
    }
    if (a.t === 'sneak' || a.t === 'kneel' || a.t === 'deep') return 'then ' + a.t;
    if (a.t === 'push') return 'then push ' + num(a.val);
    if (a.t === 'penalty') return 'then penalty ' + num(a.val) + ' ' + (a.replay ? 'replay' : 'next');
    if (a.t === 'speed') return 'then speed ' + a.group + ' ' + num(a.val);
    if (a.t === 'legs') return 'then legs ' + a.op + ' ' + num(a.val);
    if (a.t === 'button') return 'then button ' + oneLine(a.text, LIMITS.label);
    if (a.t === 'msg') return 'then msg ' + oneLine(a.text, LIMITS.msg);
    if (a.t === 'var') return 'then var ' + ident(a.name) + ' ' + a.op + ' ' + num(a.val);
    return '';
  }
  function rulesText(rules, T) {
    var out = ['# Retro Bowl mod rules - made with the Workshop', '# Save as rbmod_rules.txt in the game\'s folder.'];
    rules.forEach(function (r) {
      if (r.off) return;
      out.push('', 'rule ' + (oneLine(r.name, LIMITS.name) || 'Unnamed rule'));
      out.push('when ' + r.ev + (r.ev === 'button' ? ' ' + oneLine(r.arg, LIMITS.label) : ''));
      (r.conds || []).slice(0, LIMITS.conds).forEach(function (c) { out.push(condLine(c)); });
      (r.acts || []).slice(0, LIMITS.acts).forEach(function (a) { var l = actLine(a, T); if (l) out.push(l); });
      out.push('end');
    });
    return out.join('\n') + '\n';
  }
  // what is wrong with a rule, in words (empty list = fine)
  function problems(r, rules, T) {
    var p = [], keys = byKey(T);
    if (!oneLine(r.name, LIMITS.name)) p.push('Give the rule a name.');
    if (r.ev === 'button' && !oneLine(r.arg, LIMITS.label)) p.push('Say which button this rule answers to.');
    if (r.ev === 'button') {
      var label = oneLine(r.arg, LIMITS.label), made = rules.some(function (o) {
        return !o.off && o.ev === 'presnap' && (o.acts || []).some(function (a) { return a.t === 'button' && oneLine(a.text, LIMITS.label) === label; });
      });
      if (label && !made) p.push('No "Before the snap" rule adds a button called "' + label + '", so this can never fire.');
    }
    if (!(r.acts || []).length) p.push('Add at least one thing for the rule to do.');
    (r.conds || []).forEach(function (c) {
      if ((c.v === 'var' || c.v === 'pressed') && !oneLine(c.name, LIMITS.label)) p.push('A condition is missing its ' + (c.v === 'var' ? 'counter' : 'button') + ' name.');
    });
    (r.acts || []).forEach(function (a) {
      if (a.t === 'tune' && !keys[a.key]) p.push('Pick which setting to change.');
      if ((a.t === 'button' || a.t === 'msg') && !oneLine(a.text, 80)) p.push(a.t === 'button' ? 'A button needs a label.' : 'A message needs some text.');
      if (a.t === 'button' && r.ev !== 'presnap') p.push('Buttons can only be added by a "Before the snap" rule.');
      if (a.t === 'var' && !ident(a.name)) p.push('A counter needs a name.');
      if (a.t === 'speed' && !(Number(a.val) > 0)) p.push('Speed must be above 0%.');
      var pre = r.ev === 'presnap' || r.ev === 'button', after = AFTER.indexOf(r.ev) >= 0;
      if ((a.t === 'sneak' || a.t === 'kneel' || a.t === 'deep') && !pre) p.push('"' + a.t + '" only works before the snap: use it in a "Before the snap" or button rule.');
      if (a.t === 'push' && !(pre || r.ev === 'snap')) p.push('A push has to start before or at the snap.');
      if (a.t === 'push' && !(Number(a.val) > 0)) p.push('Push strength must be above 0%.');
      if (a.t === 'penalty' && !after) p.push('A penalty is called after a play: use it in a rule for how the play ended.');
      if (a.t === 'penalty' && !Number(a.val)) p.push('A penalty needs a number of yards.');
    });
    return p;
  }

  // ---- reading a rules file back into the builder (the inverse of rulesText) ----
  function parseRules(text, T) {
    var keys = byKey(T), rules = [], skipped = 0, r = null;
    function words(s) { return s.split(/\s+/); }
    function rest(s, n) { var w = words(s); return w.slice(n).join(' '); }
    String(text).split(/\r?\n/).forEach(function (raw) {
      var line = raw.trim();
      if (!line || line[0] === '#') return;
      var w = words(line), k = w[0];
      if (k === 'rule') { r = { name: oneLine(rest(line, 1), LIMITS.name), ev: 'presnap', arg: '', conds: [], acts: [] }; rules.push(r); return; }
      if (!r) { skipped++; return; }
      if (k === 'end') { r = null; return; }
      if (k === 'when') { r.ev = w[1] || 'presnap'; r.arg = r.ev === 'button' ? oneLine(rest(line, 2), LIMITS.label) : ''; return; }
      if (k === 'if' && w.length >= 4) {
        var v = w[1], c = { op: w[2], val: Number(w[3]) };
        if (v.indexOf('var.') === 0) { c.v = 'var'; c.name = v.slice(4); }
        else if (v.indexOf('pressed.') === 0) { c.v = 'pressed'; c.name = v.slice(8).replace(/_/g, ' '); }
        else c.v = v;
        if (!VARS.some(function (x) { return x.id === c.v; }) || !isFinite(c.val)) { skipped++; return; }
        r.conds.push(c); return;
      }
      if (k === 'then' && w.length >= 2) {
        var t = w[1], a = { t: t };
        if (t === 'tune') {
          var tt = keys[w[2]]; if (!tt) { skipped++; return; }
          a.key = w[2]; a.op = w[3]; a.val = Number(w[4]); a.keep = w[5] === 'keep';
          if (tt.pct && a.op !== '*') a.val = Math.round(a.val * 100 * 1e4) / 1e4;   // the builder shows percents
        } else if (t === 'speed') { a.group = w[2]; a.val = Number(w[3]); }
        else if (t === 'legs') { a.op = w[2]; a.val = Number(w[3]); }
        else if (t === 'var') { a.name = w[2]; a.op = w[3]; a.val = Number(w[4]); }
        else if (t === 'push') { a.val = w.length > 2 ? Number(w[2]) : 100; }
        else if (t === 'penalty') { a.val = Number(w[2]); a.replay = w[3] === 'replay'; }
        else if (t === 'button' || t === 'msg') { a.text = rest(line, 2); }
        else if (!ACTIONS.some(function (x) { return x.id === t; })) { skipped++; return; }
        r.acts.push(a); return;
      }
      skipped++;
    });
    return { rules: rules, skipped: skipped };
  }

  // ---- report: rbmod_plays.txt ----
  function parsePlays(text) {
    var plays = [];
    String(text).split(/\r?\n/).forEach(function (line) {
      if (line.indexOf('kind=') < 0) return;
      var o = {};
      line.trim().split(/\s+/).forEach(function (kv) {
        var i = kv.indexOf('=');
        if (i < 1) return;
        var k = kv.slice(0, i), v = kv.slice(i + 1), n = Number(v);
        o[k] = v !== '' && isFinite(n) ? n : v;
      });
      if (o.kind === 'pass' || o.kind === 'run') plays.push(o);
    });
    return plays;
  }
  // a new game starts when the combined score drops or a long time passes
  function splitGames(plays) {
    var games = [], cur = null, lastTotal = -1, lastT = 0;
    plays.forEach(function (p) {
      var total = (p.us >= 0 ? p.us : 0) + (p.them >= 0 ? p.them : 0);
      if (!cur || total < lastTotal || (p.t - lastT) > 1500) { cur = { plays: [] }; games.push(cur); }
      cur.plays.push(p); lastTotal = total; lastT = p.t;
    });
    games.forEach(function (g) {
      var l = g.plays[g.plays.length - 1];
      g.t = g.plays[0].t; g.us = l.us; g.them = l.them;
    });
    return games;
  }
  function rate(list) {
    var n = list.length, ok = 0, yds = 0;
    list.forEach(function (p) { ok += p.ok ? 1 : 0; yds += p.yds || 0; });
    return { n: n, ok: ok, pct: n ? Math.round(100 * ok / n) : 0, avg: n ? Math.round(10 * yds / n) / 10 : 0 };
  }
  function summarize(plays) {
    var P = plays.filter(function (p) { return p.kind === 'pass'; }), R = plays.filter(function (p) { return p.kind === 'run'; });
    function pick(list, f) { return list.filter(f); }
    var s = {
      all: rate(plays), pass: rate(P), run: rate(R),
      caught: pick(P, function (p) { return p.caught; }).length,
      ints: pick(P, function (p) { return p.end === 'int'; }).length,
      breakups: pick(P, function (p) { return p.broken || p.tipped; }).length,
      tds: pick(plays, function (p) { return p.end === 'td'; }).length,
      byPlay: ['pick', 'whiff', 'breakup', 'hit'].map(function (k) {
        var l = pick(P, function (p) { return p.play === k; }), r = rate(l);
        r.key = k; r.ints = pick(l, function (p) { return p.end === 'int'; }).length; return r;
      }),
      byRelease: [['Within the full bonus', function (p) { return p.quick >= 0.99; }], ['Fairly quick', function (p) { return p.quick > 0 && p.quick < 0.99; }],
                  ['Held, or after a scramble', function (p) { return !(p.quick > 0); }]].map(function (b) { var r = rate(pick(P, b[1])); r.key = b[0]; return r; }),
      byDown: [1, 2, 3, 4].map(function (d) { var r = rate(pick(plays, function (p) { return p.dn === d; })); r.key = d; return r; }),
      byLegs: [['Fresh', function (p) { return p.legs >= 75; }], ['Winded', function (p) { return p.legs >= 40 && p.legs < 75; }],
               ['Gassed', function (p) { return p.legs < 40; }]].map(function (b) { var r = rate(pick(R, b[1])); r.key = b[0]; return r; }),
      tools: {
        pa: pick(plays, function (p) { return p.pa; }).length, hot: pick(plays, function (p) { return p.hot > 0; }).length,
        pumps: plays.reduce(function (a, p) { return a + (p.pumps || 0); }, 0), scr: pick(plays, function (p) { return p.scr; }).length,
        max: pick(P, function (p) { return p.prot === 1; }).length, slide: pick(P, function (p) { return p.prot === 2 || p.prot === 3; }).length
      },
      dial: plays.length ? { pass0: plays[0].pdial, pass1: plays[plays.length - 1].pdial, run1: plays[plays.length - 1].rdial } : null
    };
    return s;
  }

  var api = { EVENTS: EVENTS, VARS: VARS, OPS: OPS, GROUPS: GROUPS, ACTIONS: ACTIONS, LIMITS: LIMITS, ident: ident, oneLine: oneLine,
              settingsText: settingsText, parseSettings: parseSettings, parseRules: parseRules, changed: changed, rulesText: rulesText, problems: problems,
              parsePlays: parsePlays, splitGames: splitGames, summarize: summarize };
  if (typeof module !== 'undefined' && module.exports) module.exports = api; else root.WS = api;
})(typeof window !== 'undefined' ? window : this);
