# 見本（examples/）

どれも `./shark run examples/<名前>.shk` で動く。
画面を持たないところ（`SHARK_UI=off` や ssh の先）では、窓の出るものは
**見えない面**に描いて PNG に保存する（`ui.visible()` で見分けている）。

ブラウザからも同じものを選べる。並べる順と名前は
[web/examples.py](../web/examples.py) の `ITEMS` が正で、ここに増やしたら
あちらにも足す（食い違うと `make web` が止まる）。

## 言語のかたち

| 見本 | 分かること |
|---|---|
| [hello.shk](hello.shk) | いちばん短いプログラム（`main` は無くてもよい） |
| [fizzbuzz.shk](fizzbuzz.shk) | 繰り返しと分岐 |
| [fish.shk](fish.shk) | クラス・継承・インタフェース・並べ替え |
| [config.shk](config.shk) | 失敗するかもしれない処理（`Result` / `try` / `if var`） |
| [tasks.shk](tasks.shk) | 並行処理（`task`。`async` も `await` も無い） |

## 画面（std.ui）

`std.ui` は2つの層に分かれる。**下の層**は点や線を自分で描くところ、
**上の層**は部品を組んで1つ返すところ（[spec/library/ui.md](../spec/library/ui.md)）。

| 見本 | 層 | 分かること |
|---|---|---|
| [paint.shk](paint.shk) | 下 | マウスで描く。押された・動いたを自分で見る |
| [node_editor.shk](node_editor.shk) | 下 | **ノードをつないでプログラムを作る。**組んだものが Shark のコードになって出る。拡大縮小・ミニマップと、線と丸を自分でなめらかに描くところも |
| [breakout.shk](breakout.shk) | 下 | 2D のゲーム。絵（Canvas）と透けた色 |
| [cube3d.shk](cube3d.shk) | 下 | 3D。三角形（`ui.tri`）と奥行き（z バッファ）だけで書く |
| [counter.shk](counter.shk) | 上 | いちばん小さい宣言的な書き方（`ui.run` と `view()`） |
| [widgets.shk](widgets.shk) | 上 | **部品をぜんぶ出す見本。**飾りの鎖（`.border` など）も一とおり |
| [cube_ui.shk](cube_ui.shk) | 上 | 状態はふつうの変数のまま、部品（つまみ・木・色）で 3D を動かす |

## ゲームに組み込む

| ところ | 分かること |
|---|---|
| [embed/](embed) | C++ のゲームから Shark を呼ぶ。バイトコードを焼き込む形も（`make embed`） |
