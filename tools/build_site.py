#!/usr/bin/env python3
"""Build site/index.html (one self-contained page) from site/page.html + core.js + examples.js + tools/tunables.json."""
import json, os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rd = lambda *p: open(os.path.join(root, *p), encoding='utf-8').read()
page = rd('site', 'page.html')
for mark, text in (('/*CORE*/', rd('site', 'core.js')), ('/*EXAMPLES*/', rd('site', 'examples.js')), ('/*SCRIPT*/', rd('site', 'script.js')), ('/*NAMES*/', rd('site', 'names.js')),
                   ('/*TUNABLES*/[]', json.dumps(json.loads(rd('tools', 'tunables.json')), separators=(',', ':')))):
    assert page.count(mark) == 1, mark
    assert '</script' not in text.lower()
    page = page.replace(mark, text)
open(os.path.join(root, 'site', 'index.html'), 'w', encoding='utf-8').write(page)
print('site/index.html', len(page), 'bytes')
