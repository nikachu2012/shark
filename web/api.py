#!/usr/bin/env python3
# api.py — 入力補完（IntelliSense）が使う API の表（api.js）を作る。web/build.sh から呼ばれる。
#
#   使い方: api.py <リポジトリの場所> <出力先 api.js>
#
# 中身は stdlib/*.shk（宣言ファイル）が正で、読むのは tools/shkdoc.py。
# HTML のリファレンス（docs/gen.py）と同じ出どころなので、説明も例も一致する。
# 手で書いた一覧は持たない。実装と食い違っていれば、作るときに知らせる。
import json
import os
import sys

root, out_path = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(root, 'tools'))
import shkdoc  # noqa: E402


def entry(item, name, sig, prefix=''):
    """補完が使う形。lang.js が読む"""
    e = {'name': name, 'sig': sig, 'params': item['params'], 'ret': item['ret'],
         'doc': item['doc'], 'kind': 'const' if item['kind'] == 'const' else 'function'}
    if item['overloads']:
        e['overloads'] = [prefix + o for o in item['overloads']]
    if any(a[2] for a in item['args']):
        e['args'] = item['args']
    if item['example']:
        e['example'] = item['example']
    return e


def build(root):
    api = {'modules': {}, 'methods': {}, 'builtins': []}
    pages = shkdoc.parse(root)
    for page in pages:
        mod = page['module']
        short = mod.split('.')[-1] if mod.startswith('std.') else ''
        for it in page['items']:
            name = it['name']
            qualified = '%s.%s' % (short, name) if short else name
            sig = it['sig'] if it['kind'] == 'const' else \
                shkdoc.signature(qualified + it['generic'], it['params'], it['ret'])
            if it['kind'] == 'const':
                sig = qualified
            e = entry(it, name, sig, (short + '.') if short else '')
            if short:
                api['modules'].setdefault(short, {'members': []})['members'].append(e)
            else:
                api['builtins'].append(e)
        for cls in page['classes']:
            for it in cls['items']:
                sig = shkdoc.signature(it['name'] + it['generic'], it['params'], it['ret'])
                api['methods'].setdefault(cls['name'], []).append(entry(it, it['name'], sig))
    return api, pages


api, pages = build(root)

with open(out_path, 'w', encoding='utf-8') as f:
    f.write('// api.js — web/api.py が stdlib/*.shk から作る。直に書き換えない\n')
    f.write('window.SHARK_API = ')
    json.dump(api, f, ensure_ascii=False, indent=1)
    f.write(';\n')

total = len(api['builtins']) + sum(len(m['members']) for m in api['modules'].values()) + \
    sum(len(v) for v in api['methods'].values())
print('API %d 件（モジュール %d / 型 %d）' % (total, len(api['modules']), len(api['methods'])))
missing, extra = shkdoc.crosscheck(pages, shkdoc.core_names(root))
if missing or extra:
    print('  宣言と実装が食い違っています: %s' % ' '.join(missing + extra))
