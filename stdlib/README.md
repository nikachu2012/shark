# stdlib/ — 宣言ファイル

標準ライブラリの**名前・型・説明・例**は、ここの `.shk` が正。
実装（`core/lib/*.cpp`）は動きだけを持ち、説明は持たない。

| ファイル | 何が入っているか |
|---|---|
| [`builtin.shk`](builtin.shk) | `import` なしで使える関数 |
| [`types.shk`](types.shk) | `string` `list` `map` `Result` など、コアが持つ型と、その関数 |
| `math.shk` `time.shk` … | `import std.xxx;` で使うモジュール（1ファイル1モジュール） |
| [`prelude.shk`](prelude.shk) | **宣言ではなく実装**。Shark 自身で書いた並べ替え |
| [`prelude_ui.shk`](prelude_ui.shk) | 同じく実装。宣言的に書くときの入り口（`ui.run`） |

読むのは [`tools/shkdoc.py`](../tools/shkdoc.py)。使う側は2つ。

```
make docs         docs/reference/ に HTML（ライブラリごとに1枚）
make web          プレイグラウンドの入力補完（api.js）
make docs-check   例をぜんぶ本物の shark で動かす
```

## 書き方

```shark
/// std.math                      ← module の上は、そのページの題名と説明
///
/// 数の計算と乱数。
module std.math;

/// 平方根。負の数なら nan。      ← 1行目が一覧に出る。空行で段落を分ける
///
/// 引数:
///   x   0 以上の数
///
/// 例:
///   print(math.sqrt(9.0));   // 3.0
func sqrt(x: float) -> float;
```

- 宣言は**本体を書かない**。`func 名前(引数) -> 戻り値;` で終える
- 型に付く関数は `class` で囲む。`class list<T> { func push(v: T) -> void; }`
- 定数は `const PI: float;`
- 同じ名前を並べて書くとオーバーロードになる。説明と例は最初の1つに書く
- 節に書けるのは `引数:` `戻り値:` `例:` `注意:`。字下げした行がその中身

## 例の決まり

**すべての関数に例を付ける。**`make docs-check` が本物の `shark` で動かすので、
動かない例は入らない。

- 例は**1つで完結**させる。受け手や入力も、その中で作る
- モジュールの `import` は書かない（そのページのものは自動で付く）。
  ほかのモジュールを使うときだけ、例の中に書く
- 出た値は `// 3` のように行末に書く。
  `python3 tools/runex.py --show <名前の一部>` で、実際の出力と見比べられる
- 外のプログラムを呼ぶなど、動かせないものは `例（動かさない）:` と書く

## 実装との突き合わせ

`make docs` は、宣言と実装（`core/lib/*.cpp` の `r.add`、`core/check.cpp` のメソッド）を
突き合わせる。**どちらか片方にしか無いものがあれば、名前を挙げて知らせる**。
標準ライブラリに関数を足したら、ここにも書き足す。
