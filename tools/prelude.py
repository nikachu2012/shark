#!/usr/bin/env python3
# prelude.py — stdlib/prelude.shk および prelude_ui.shk を C++ ヘッダ（core/prelude.h）に変換する
#
#   python3 tools/prelude.py        # Makefile から必要時に自動実行
#
# コア自身はファイル I/O を行わないため、Shark 自身で記述された標準処理は C++ 文字列として埋め込む。
# 修正対象は stdlib/prelude.shk / prelude_ui.shk であり、core/prelude.h は自動生成物。
import os
import sys

# Windows コンソールのデフォルト文字コード対策（文字化け防止のため UTF-8 に統一）
if sys.platform == 'win32':
    import ctypes
    ctypes.windll.kernel32.SetConsoleOutputCP(65001)
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'stdlib/prelude.shk')
SRC_UI = os.path.join(ROOT, 'stdlib/prelude_ui.shk')
OUT = os.path.join(ROOT, 'core/prelude.h')

HEAD = '''// prelude.h — Shark 自身で実装されたコード（tools/prelude.py により自動生成）
//
// このファイルは自動生成されます。直接編集せず、stdlib/prelude.shk / prelude_ui.shk を編集してください。
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
    """先頭のコメント行は埋め込まない（コード本体のみを埋め込む）"""
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
    # 行の終わりは LF に固定する（Windows の既定は CRLF で、
    # そのままだと機種ごとに違う core/prelude.h ができてしまう）
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(''.join(out))
    print('core/prelude.h（%d 行 + ui %d 行）' % (len(body), len(ui)))


if __name__ == '__main__':
    main()
