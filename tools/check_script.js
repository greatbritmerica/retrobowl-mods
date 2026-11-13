// check_script.js - check play scripts with the Workshop's own checker (site/script.js) plus the
// engine's limits (48 on blocks, 128 variables (v94+); src/script.c), alone and as deployed (Hail Mary + step 1).
//   node tools/check_script.js [file ...]     (default: the two example scripts)
//   node tools/check_script.js --hook         (Claude Code hook: reads the edit from stdin, checks
//                                              only when a script in examples/ changed, exit 2 on failure)
//   node tools/check_script.js --selftest
'use strict';
const fs = require('fs');
const path = require('path');
const ROOT = path.join(__dirname, '..');
const { check } = require(path.join(ROOT, 'site', 'script.js'));
const MAX_BLOCKS = 48, MAX_VARS = 128;

function vars(src) {   // distinct names a script sets (assignments and for-loop names), as the engine counts them
  const t = src.replace(/#.*/g, '');
  const names = new Set();
  for (const m of t.matchAll(/^\s*([A-Za-z_]\w*)\s*=[^=]/gm)) names.add(m[1]);
  for (const m of t.matchAll(/^\s*for\s+(\w+)/gm)) names.add(m[1]);
  return names.size;
}

function verdict(label, src) {   // -> [ok, message]
  const r = check(src);
  if (!r.ok) return [false, `${label}: line ${r.line}: ${r.msg}`];
  const b = r.handlers.length, v = vars(src);
  const ok = b <= MAX_BLOCKS && v <= MAX_VARS;
  return [ok, `${label}: loads - ${b}/${MAX_BLOCKS} on blocks, ${v}/${MAX_VARS} variables${ok ? '' : '  OVER THE LIMIT'}`];
}

function run(files) {
  const read = f => fs.readFileSync(f, 'utf8');
  const results = files.map(f => verdict(path.relative(ROOT, f), read(f)));
  const hm = path.join(ROOT, 'examples', 'rbmod_script.txt'), s1 = path.join(ROOT, 'examples', 'step1_script.txt');
  if (fs.existsSync(hm) && fs.existsSync(s1)) results.push(verdict('deployed (Hail Mary + step 1)', read(hm) + '\n\n' + read(s1)));
  for (const [, m] of results) console.log(m);
  return results.every(([ok]) => ok);
}

function selftest() {
  const assert = require('assert');
  assert.deepStrictEqual(verdict('a', 'on snap\n  x = 1\nend\n'), [true, 'a: loads - 1/48 on blocks, 1/128 variables']);
  assert.strictEqual(verdict('b', 'on snap\n  frob()\nend\n')[0], false);
  assert.strictEqual(verdict('c', 'on snap\n' + Array.from({ length: 129 }, (_, i) => `  v${i} = 1\n`).join('') + 'end\n')[0], false);
  assert.strictEqual(vars('on snap\n  if a == 1\n    b = 2   # c = 3\n  end\n  for p in offense()\n  end\nend\n'), 2);
  console.log('selftest ok');
}

const args = process.argv.slice(2);
if (args[0] === '--selftest') selftest();
else if (args[0] === '--hook') {
  let input = '';
  process.stdin.on('data', d => (input += d)).on('end', () => {
    let file = '';
    try { file = (JSON.parse(input).tool_input || {}).file_path || ''; } catch (e) { return; }
    if (!/[\\/]examples[\\/][^\\/]+\.txt$/.test(file) || /rules/.test(path.basename(file))) return;
    const out = [];
    const log = console.log; console.log = m => out.push(m);
    const ok = run([path.join(ROOT, 'examples', 'rbmod_script.txt'), path.join(ROOT, 'examples', 'step1_script.txt')]);
    console.log = log;
    if (!ok) { process.stderr.write('Script check failed:\n' + out.join('\n') + '\n'); process.exit(2); }
  });
} else {
  const files = args.length ? args.map(f => path.resolve(f))
    : [path.join(ROOT, 'examples', 'rbmod_script.txt'), path.join(ROOT, 'examples', 'step1_script.txt')];
  process.exit(run(files) ? 0 : 1);
}
