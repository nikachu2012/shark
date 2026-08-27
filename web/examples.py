#!/usr/bin/env python3
# examples.py — .shk のお手本を、ブラウザから選べる形（examples.js）にまとめる。
# web/build.sh から呼ばれる。使い方: examples.py <リポジトリの場所> <web の場所> <出力先>
import json
import os
import sys

root, here, out = sys.argv[1], sys.argv[2], sys.argv[3]

# 並べる順と、選ぶところに出す名前
ITEMS = [
    ("examples/hello.shk", "はじめの一歩"),
    ("examples/fizzbuzz.shk", "FizzBuzz"),
    ("web/examples/ask.shk", "入力を読む"),
    ("examples/fish.shk", "クラスと継承"),
    ("examples/tasks.shk", "並行処理"),
    ("examples/config.shk", "失敗するかもしれない処理"),
    ("tests/unit_test.shk", "テストを書く"),
    ("web/examples/ui.shk", "画面に出す（std.ui）"),
    ("web/examples/forever.shk", "止まらない繰り返し"),
]

items = []
for path, title in ITEMS:
    full = os.path.join(root, path)
    if not os.path.exists(full):
        sys.stderr.write("見つかりません（飛ばします）: %s\n" % path)
        continue
    with open(full, encoding="utf-8") as f:
        items.append({"title": title, "path": path, "code": f.read()})

with open(out, "w", encoding="utf-8") as f:
    f.write("// examples.js — web/build.sh が作る。ここを直に書き換えても次の作り直しで消える\n")
    f.write("window.SHARK_EXAMPLES = ")
    json.dump(items, f, ensure_ascii=False, indent=1)
    f.write(";\n")

print("お手本 %d 件" % len(items))
