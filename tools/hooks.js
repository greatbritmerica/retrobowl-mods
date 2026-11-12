// hooks.js - Claude Code hooks for generated files (wired in .claude/settings.json).
//   --pre   PreToolUse:  refuse a hand edit to a generated file and name its source (exit 2)
//   --post  PostToolUse: after a source edit, rerun the generator so its outputs don't fall behind
//   --selftest
'use strict';
const path = require('path');
const { execFileSync } = require('child_process');
const ROOT = path.join(__dirname, '..');

// generated file -> what to edit instead
const GENERATED = {
  'src/tune.h': 'tools/tunables.json (then tools/gen_tune.py)',
  'src/tune_table.inc': 'tools/tunables.json (then tools/gen_tune.py)',
  'src/script_names.inc': 'nothing by hand: tools/gen_names.py rebuilds it from the game binary',
  'site/names.js': 'nothing by hand: tools/gen_names.py rebuilds it from the game binary',
  'site/index.html': 'site/page.html or site/*.js (then tools/build_site.py)',
};
// source file -> generators to run, in order
function generatorsFor(rel) {
  if (rel === 'tools/tunables.json') return ['tools/gen_tune.py', 'tools/build_site.py'];
  if (rel === 'site/page.html' || (/^site\/[^/]+\.js$/.test(rel) && rel !== 'site/names.js')) return ['tools/build_site.py'];
  return [];
}
const rel = f => path.relative(ROOT, path.resolve(f)).split(path.sep).join('/');

function readFile(cb) {
  let input = '';
  process.stdin.on('data', d => (input += d)).on('end', () => {
    try { cb((JSON.parse(input).tool_input || {}).file_path || ''); } catch (e) { /* not an edit: nothing to do */ }
  });
}

const mode = process.argv[2];
if (mode === '--pre') readFile(f => {
  const instead = f && GENERATED[rel(f)];
  if (instead) { process.stderr.write(`${rel(f)} is generated - edit ${instead} instead.\n`); process.exit(2); }
});
else if (mode === '--post') readFile(f => {
  for (const g of f ? generatorsFor(rel(f)) : []) {
    try { execFileSync('python', [path.join(ROOT, g)], { cwd: ROOT, stdio: ['ignore', 'ignore', 'pipe'] }); }
    catch (e) { process.stderr.write(`${g} failed after editing ${rel(f)}:\n${e.stderr || e.message}\n`); process.exit(2); }
  }
});
else if (mode === '--selftest') {
  const assert = require('assert');
  assert.deepStrictEqual(generatorsFor('tools/tunables.json'), ['tools/gen_tune.py', 'tools/build_site.py']);
  assert.deepStrictEqual(generatorsFor('site/core.js'), ['tools/build_site.py']);
  assert.deepStrictEqual(generatorsFor('site/names.js'), []);
  assert.deepStrictEqual(generatorsFor('src/offtools.c'), []);
  assert.ok(GENERATED[rel(path.join(ROOT, 'site', 'index.html'))]);
  console.log('selftest ok');
}
