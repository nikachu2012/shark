#!/usr/bin/env python3
# examples.py — サンプルコード（.shk）を Web プレイグラウンド用データ（examples.js）に統合する。
# web/build.sh から呼び出される。使用方法: examples.py <リポジトリパス> <web ディレクトリ> <出力先>
import json
import os
import sys

root, here, out = sys.argv[1], sys.argv[2], sys.argv[3]

# サンプルの表示順と Web UI 上の表示タイトル。
# 表示順とタイトルを明示的に管理するためリスト形式で定義。
# examples/ および web/examples/ 内の全 .shk ファイルが漏れなく登録されているかを下部で整合性検証する。
ITEMS = [
    ("examples/hello.shk", "はじめの一歩（Hello World）"),
    ("examples/fizzbuzz.shk", "FizzBuzz"),
    ("web/examples/ask.shk", "標準入力の読み込み"),
    ("examples/fish.shk", "クラスと継承"),
    ("examples/tasks.shk", "並行処理（Task / Channel）"),
    ("examples/config.shk", "エラーハンドリング（Result / Option）"),
    ("tests/unit_test.shk", "ユニットテスト"),
    ("web/examples/ui.shk", "グラフィックス描画（std.ui プリミティブ）"),
    ("examples/paint.shk", "ペイント（低レベル描画 API）"),
    ("examples/node_editor.shk", "ノードエディタ（コード生成グラフ）"),
    ("examples/hexedit.shk", "バイナリエディタ（Hex Editor）"),
    ("examples/counter.shk", "カウンター（宣言的 UI コンポーネント）"),
    ("examples/widgets.shk", "ウィジェットカタログ（高レベル UI コンポーネント）"),
    ("examples/breakout.shk", "2D ゲーム（ブロック崩し）"),
    ("examples/cube3d.shk", "3D レンダリング（回転する立方体）"),
    ("examples/cube_ui.shk", "3D 描画と UI コントロールの統合"),
    ("web/examples/forever.shk", "無限ループと協調的マルチタスク"),
]

# サンプルコードの対象ディレクトリ。すべての .shk が ITEMS に登録されている必要がある
DIRS = ["examples", "web/examples"]

listed = [path for path, _ in ITEMS]
bad = []

# 一覧に定義されているが実ファイルが存在しない
for path in listed:
    if not os.path.exists(os.path.join(root, path)):
        bad.append("一覧に定義されていますが、ファイルが存在しません: %s" % path)

# 実ファイルが存在するが一覧に登録されていない（Web からアクセスできなくなるのを防止）
for d in DIRS:
    full = os.path.join(root, d)
    if not os.path.isdir(full):
        continue
    for name in sorted(os.listdir(full)):
        if not name.endswith(".shk"):
            continue
        rel = "%s/%s" % (d, name)
        if rel not in listed:
            bad.append("ファイルが存在しますが、一覧に登録されていません: %s" % rel)

if bad:
    sys.stderr.write("エラー: サンプルコード一覧（web/examples.py の ITEMS）に不整合があります:\n")
    for line in bad:
        sys.stderr.write("  %s\n" % line)
    sys.stderr.write("  → ITEMS に追加するか、不要なファイルを削除してください\n")
    sys.exit(1)

items = []
for path, title in ITEMS:
    with open(os.path.join(root, path), encoding="utf-8") as f:
        items.append({"title": title, "path": path, "code": f.read()})

with open(out, "w", encoding="utf-8") as f:
    f.write("// examples.js — web/build.sh により自動生成。直接編集しないでください\n")
    f.write("window.SHARK_EXAMPLES = ")
    json.dump(items, f, ensure_ascii=False, indent=1)
    f.write(";\n")

print("サンプルコード %d 件を登録" % len(items))
