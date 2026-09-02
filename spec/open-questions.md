# 決めていないこと

仕様を書く中で保留にした判断の一覧。各仕様書の「決めていないこと」を集めたもの。
決まったものは [下](#決まったこと) に移す。

紙の上で決められるものは、ひととおり決まった。残りは次の3種類。

| # | 決めること | 区分 | 直すファイル |
|---|---|---|---|
| 1 | 画面まわりの残り（FreeType 無しでの日本語、文字入力） | 実装時 | library/ui.md |
| 2 | 実行時コンパイルのしきい値 | 実装時 | runtime/execution.md |
| 3 | 埋め込む Unicode の表の大きさ | 実装時 | library/overview.md |
| 4 | C++ のどの規格に合わせるか | 実装時 | skeleton.md |
| 5 | ホストの関数にクラスを渡せるようにするか | 組み込み | runtime/embedding.md |
| 6 | 実行の途中状態を保存できるようにするか | 組み込み | runtime/embedding.md |
| 7 | 言語サーバとして常駐させるか | 別実装 | frontend.md |
| 8 | 整形の細かい規則 | 別実装 | frontend.md |
| 9 | ツールをコアと同梱で配るか | 別実装 | frontend.md |

---

## 実装するときに決める

作って動かしてみないと良し悪しが分からないもの。

### 1. 画面まわりの残り

作って決まったこと（[library/ui.md](library/ui.md)）。

- 下の層（面に描く、出来事を受け取る）。色は `int` 1つの `0xRRGGBB`、寸法は画素、
  字形は 5×7 を内蔵、透過は持たない
- 宣言的な層は**部品の配列を返す形**にした。呼び出しにブロックを続ける記法
  （[library/ui-declarative.md](library/ui-declarative.md)）は言語に無く、
  配列ならいまの言語のまま書けるため
- 部品の既定値（間・余白・色）も決めた。寸法を細かく指定する仕組みは持たない

残っているのは3つ。

- **FreeType の無いところでの日本語**。日本語は FreeType（任意の外部ライブラリ）で
  出せるようにした。入れていないときは内蔵の ASCII だけになる。
  仮名だけの字形を内蔵するか、絵として貼るかは決めていない
- **X11 での変換つき入力**。変換は OS に任せる形にして、macOS（窓）と
  ブラウザでは日本語が入る。X11 は XIM を持っていないので、打った文字がそのまま入る
- **上の層の部品をどこまで増やすか**（一覧、絵、はみ出したときのスクロール）

### 2. 実行時コンパイルのしきい値

「何回呼ばれたら機械語にするか」の具体的な数。測ってから決める。
コアが区切って戻る仕組み（[runtime/embedding.md](runtime/embedding.md)）との
兼ね合いも要る。

### 3. 埋め込む Unicode の表の大きさ

`std.text` は正規化と五十音順のために表を抱える。
ゲーム機に載せられる大きさに収まるかは、作ってから測る
（[library/overview.md](library/overview.md)）。

### 4. C++ のどの規格に合わせるか

実装は C++ に決めた（[skeleton.md](skeleton.md)）。
どの規格に合わせるかは、載せる機種のコンパイラが揃っている範囲を見てから決める。

---

## 組み込みの詰め

ゲーム本体を作りながら、必要になった時点で決めるもの
（[runtime/embedding.md](runtime/embedding.md)）。

### 5. ホストの関数にクラスを渡せるようにするか

いまは基本型とコレクションだけ。ゲーム側の「ロボット」のような値を
そのまま渡したくなったら考える。

### 6. 実行の途中状態を保存できるようにするか

止めた場所から続けられると、学習ゲームでは中断・再開が作れる。
仮想マシンの状態を丸ごと書き出すことになるので、必要になってから決める。

---

## フロントエンド側で決める

コアの仕様の外。コマンドライン実装を作るときに決める（[frontend.md](frontend.md)）。

- 言語サーバとして常駐させるか、`shark` を都度呼ぶか
- 整形の細かい規則（字下げ幅、1行の長さ）
- ツールをコアと同梱で配るか、別に配るか

---

## 決まったこと

| 決めたこと | 結論 | 書いた場所 |
|---|---|---|
| 抽象クラス／インタフェース | `virtual` と純粋仮想で表す。実装を持つ親は1つ、インタフェースは複数 | [types/class.md](types/class.md) |
| 基本型ごとの関数の書き分け | 関数オーバーロードを入れる。暗黙変換が無いので完全一致で1つに決まる | [syntax.md](syntax.md) |
| その場に書く関数の捕まえ方 | 捕まえない。見えるのは自分の引数と一番外側のものだけ。関数の値は「どの関数か」を指すだけで済み、「代入はコピー」と食い違わない | [syntax.md](syntax.md) |
| ユーザー定義型の並べ替え | `Comparable` インタフェースと制約構文 `<T: Comparable>`。`This` 型を用意 | [types/generics.md](types/generics.md) |
| 標準ライブラリの必須範囲 | 必須は `time` `math` `task` だけ。持たないモジュールの `import` は読み込み時エラー | [library/overview.md](library/overview.md) |
| 剰余 `%` の符号 | 割られる数に従う（C 言語と同じ）。`-7 % 2` は `-1` | [types/primitive.md](types/primitive.md) |
| 書式指定の文法 | Python に倣う。桁揃え・ゼロ埋め・桁区切り・2進・16進・百分率 | [syntax.md](syntax.md) |
| 日本語入力（IME） | 変換は OS に任せ、確定した文字列と変換中の文字列だけを受け取る。候補一覧は自前で描かない | [library/ui.md](library/ui.md) |
| 言語ごとの辞書順 | 日本語（五十音順）だけ入れる。漢字は読みが要るので対象外 | [library/text.md](library/text.md) |
| パッケージの配布 | 持たない。`import` の手間は編集環境の側で減らす | [runtime/module.md](runtime/module.md), [frontend.md](frontend.md) |
| JSON とクラスの相互変換 | 自動変換はしない。代わりに読み出しを短く書けるようにする | [library/json.md](library/json.md) |
| 実装に使う言語 | C++。例外と RTTI は使わない | [skeleton.md](skeleton.md) |
| 雛形の配り方 | git で配る。触る場所を絞ることで、上流の直しを取り込みやすくする | [skeleton.md](skeleton.md) |
| 複数のチャネルを待つ書き方 | `select` は持たない。送り手を増やす（多対一）か、チャネルと受け手を分ける | [runtime/concurrency.md](runtime/concurrency.md) |
| 取り消しの届き方 | 要求を立てるだけ。待つ関数が `step()` ごとに見る。移植層の通信は待ちっぱなしにしない | [runtime/concurrency.md](runtime/concurrency.md), [runtime/platform.md](runtime/platform.md) |
| タスクを割り振るスレッド数 | 既定は1本。ロックも設定も要らなくなる | [runtime/concurrency.md](runtime/concurrency.md) |
| テストを同時に走らせるか | 1件ずつ順に走らせる | [library/test.md](library/test.md) |
| バージョン間の互換 | ソースコード上で動きが変わらなければよい。バイトコードは変えてよい | [library/overview.md](library/overview.md) |
| テストの書き方 | `test_` で始まる関数。`test.eq` などで確かめる | [library/test.md](library/test.md) |
| メモリを使いすぎたとき | 実行中に使う量の上限をホストが決め、超えたら実行時エラーで止める。読み込みのぶんは数えず、確保そのものは断らない | [runtime/memory.md](runtime/memory.md), [runtime/embedding.md](runtime/embedding.md) |
| 可変長引数とキーワード引数 | 最後の引数の `...` で余りを list に束ねて受け取る。呼び出しは `名前: 値` で並びを自由にできる（省略は無し）。束ねるのは呼ぶ側なので仮想マシンは変えない | [syntax.md](syntax.md) |
