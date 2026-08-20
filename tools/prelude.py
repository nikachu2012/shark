#!/usr/bin/env python3
# prelude.py — stdlib/prelude.shk を core/prelude.h（C++ の文字列）にする。
#
#   python3 tools/prelude.py        # make が要るときに呼ぶ
#
# コアはファイルを読まないので、Shark で書いた部分は埋め込んで持つ。
# 直すのは stdlib/prelude.shk の方で、core/prelude.h は作られたもの。
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'stdlib/prelude.shk')
SRC_UI = os.path.join(ROOT, 'stdlib/prelude_ui.shk')
OUT = os.path.join(ROOT, 'core/prelude.h')

HEAD = '''// prelude.h — Shark 自身で書いた部分（tools/prelude.py が stdlib/prelude.shk から作る）
//
// ここを直さない。直すのは stdlib/prelude.shk。
#ifndef SHARK_PRELUDE_H
#define SHARK_PRELUDE_H

namespace shark {

'''
TAIL = '''
}  // namespace shark
#endif
'''


def escape(line):
    return line.replace('\\', '\\\\').replace('"', '\\"')


def body_of(path):
    """説明の // だけの行は埋め込まない（処理系に持たせるのは中身だけ）"""
    with open(path, encoding='utf-8') as f:
        lines = f.read().split('\n')
    body, started = [], False
    for line in lines:
        if not started and (line.startswith('//') or not line.strip()):
            continue
        started = True
        body.append(line)
    while body and not body[-1].strip():
        body.pop()
    return body


def emit(out, name, body):
    out.append('static const char* %s =\n' % name)
    for line in body:
        out.append('    "%s\\n"\n' % escape(line))
    out.append(';\n\n')


def main():
    body = body_of(SRC)
    ui = body_of(SRC_UI)
    out = [HEAD]
    emit(out, 'kPreludeSource', body)
    # 宣言的に書くときの入り口（std.ui を入れたときだけ読む）
    emit(out, 'kUiRunSource', ui)
    out.append(TAIL)
    with open(OUT, 'w', encoding='utf-8') as f:
        f.write(''.join(out))
    print('core/prelude.h（%d 行 + ui %d 行）' % (len(body), len(ui)))


if __name__ == '__main__':
    main()
