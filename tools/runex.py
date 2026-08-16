#!/usr/bin/env python3
# runex.py — 宣言ファイルの「例」を、本物の shark で動かして確かめる（make docs-check）
#
#   python3 tools/runex.py [名前の一部 ...]   確かめる
#   python3 tools/runex.py --show <名前の一部>  例と、その出力を並べて見る
#
# 例は1つで完結していること（受け手も自分で作る）。動かせないもの（外のプログラム、
# 通信、その場を書き換えるもの）は「例（動かさない）:」と書く。
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import shkdoc  # noqa: E402


def items_of(page):
    for it in page['items']:
        yield '', it
    for cls in page['classes']:
        for it in cls['items']:
            yield cls['name'] + '.', it


def main():
    only = sys.argv[1:]
    show = '--show' in only
    only = [o for o in only if o != '--show']
    shark = os.path.join(ROOT, 'shark')
    pages = shkdoc.parse(ROOT)
    ok = bad = skip = missing = 0
    work = tempfile.mkdtemp(prefix='shkdoc-')

    for page in pages:
        if only and not any(o in page['file'] for o in only):
            continue
        head = 'import %s;\n' % page['module'] if page['module'].startswith('std.') else ''
        for owner, it in items_of(page):
            name = '%s: %s%s' % (page['file'], owner, it['name'])
            if not it['example']:
                missing += 1
                print('例が無い     %s' % name)
                continue
            if not it['run']:
                skip += 1
                continue
            path = os.path.join(work, 'ex.shk')
            with open(path, 'w', encoding='utf-8') as f:
                f.write(head + it['example'] + '\n')
            try:
                p = subprocess.run([shark, 'run', path], capture_output=True, text=True,
                                   timeout=20, cwd=work, stdin=subprocess.DEVNULL)
            except subprocess.TimeoutExpired:
                bad += 1
                print('終わらない   %s' % name)
                continue
            if show:
                print('--- %s' % name)
                print('\n'.join('  ' + l for l in it['example'].split('\n')))
                print('  出力: %s' % (p.stdout or '').strip().replace('\n', ' / '))
            if p.returncode == 0:
                ok += 1
            else:
                bad += 1
                print('動かない     %s' % name)
                for line in (p.stderr or p.stdout).strip().split('\n')[:4]:
                    print('             %s' % line)
    print('動いた %d / 動かない %d / 動かさない %d / 例が無い %d' % (ok, bad, skip, missing))
    return 1 if (bad or missing) else 0


if __name__ == '__main__':
    sys.exit(main())
