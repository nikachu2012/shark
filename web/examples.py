#!/usr/bin/env python3
# examples.py — .shk のお手本を、ブラウザから選べる形（examples.js）にまとめる。
# web/build.sh から呼ばれる。使い方: examples.py <リポジトリの場所> <web の場所> <出力先>
import json
import os
import sys

root, here, out = sys.argv[1], sys.argv[2], sys.argv[3]

# 並べる順と、選ぶところに出す名前。
# **手で並べているのは、順と名前を決めたいから。**そのぶん、examples/ に足した
# ものがここに入っていないと、ブラウザからは見えないまま古くなる。
# なので下で突き合わせて、食い違っていれば止める（Makefile の RT_SRC と同じ考え）。
ITEMS = [
    ("examples/hello.shk", "はじめの一歩"),
    ("examples/fizzbuzz.shk", "FizzBuzz"),
    ("web/examples/ask.shk", "入力を読む"),
    ("examples/fish.shk", "クラスと継承"),
    ("examples/tasks.shk", "並行処理"),
    ("examples/config.shk", "失敗するかもしれない処理"),
    ("tests/unit_test.shk", "テストを書く"),
    ("web/examples/ui.shk", "画面に出す（std.ui）"),
    ("examples/paint.shk", "マウスで描く（下の層）"),
    ("examples/node_editor.shk", "ノードエディタ（つないでコードにする）"),
    ("examples/counter.shk", "部品を組んで返す（上の層）"),
    ("examples/widgets.shk", "部品をぜんぶ出す（std.ui）"),
    ("examples/breakout.shk", "2D のゲーム（ブロック崩し）"),
    ("examples/cube3d.shk", "3D を描く（回る立方体）"),
    ("examples/cube_ui.shk", "3D を部品で動かす（上の層）"),
    ("web/examples/forever.shk", "止まらない繰り返し"),
]

# お手本を置いてよいところ。ここの .shk は、ぜんぶ上の一覧に入っていること
DIRS = ["examples", "web/examples"]

listed = [path for path, _ in ITEMS]
bad = []

# 一覧にあるのに、ファイルが無い
for path in listed:
    if not os.path.exists(os.path.join(root, path)):
        bad.append("一覧にあるのに、ファイルがありません: %s" % path)

# ファイルはあるのに、一覧に無い（ブラウザから見えないまま古くなる）
for d in DIRS:
    full = os.path.join(root, d)
    if not os.path.isdir(full):
        continue
    for name in sorted(os.listdir(full)):
        if not name.endswith(".shk"):
            continue
        rel = "%s/%s" % (d, name)
        if rel not in listed:
            bad.append("ファイルはあるのに、一覧にありません: %s" % rel)

if bad:
    sys.stderr.write("お手本の一覧（web/examples.py の ITEMS）が食い違っています:\n")
    for line in bad:
        sys.stderr.write("  %s\n" % line)
    sys.stderr.write("  → ITEMS に足すか、そのファイルを消します\n")
    sys.exit(1)

items = []
for path, title in ITEMS:
    with open(os.path.join(root, path), encoding="utf-8") as f:
        items.append({"title": title, "path": path, "code": f.read()})

with open(out, "w", encoding="utf-8") as f:
    f.write("// examples.js — web/build.sh が作る。ここを直に書き換えても次の作り直しで消える\n")
    f.write("window.SHARK_EXAMPLES = ")
    json.dump(items, f, ensure_ascii=False, indent=1)
    f.write(";\n")

print("お手本 %d 件" % len(items))
