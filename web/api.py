#!/usr/bin/env python3
# api.py — 入力補完（IntelliSense）用の API 定義テーブル（api.js）を生成。web/build.sh から呼び出される。
#
#   使用方法: api.py <リポジトリパス> <出力先 api.js>
#
# stdlib/*.shk（宣言ファイル）を tools/shkdoc.py で解析して生成。
# HTML リファレンス（docs/gen.py）と同一のデータソースを使用するため、ドキュメントやコード例の整合性が保たれる。
# 手作業による定義リストは持たず、宣言と実装の乖離があればビルド時に警告を出力する。
import json
import os
import sys

root, out_path = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(root, 'tools'))
import shkdoc  # noqa: E402


def entry(item, name, sig, prefix=''):
    """エディタの入力補完用エントリを生成。lang.js から参照される"""
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
    f.write('// api.js — web/api.py により stdlib/*.shk から自動生成。直接編集しないでください\n')
    f.write('window.SHARK_API = ')
    json.dump(api, f, ensure_ascii=False, indent=1)
    f.write(';\n')

total = len(api['builtins']) + sum(len(m['members']) for m in api['modules'].values()) + \
    sum(len(v) for v in api['methods'].values())
print('API 定義生成完了: %d 件（モジュール %d / 型 %d）' % (total, len(api['modules']), len(api['methods'])))
missing, extra = shkdoc.crosscheck(pages, shkdoc.core_names(root))
if missing or extra:
    print('  警告: 宣言と実装に乖離があります: %s' % ' '.join(missing + extra))
