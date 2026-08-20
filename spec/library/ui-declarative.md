# ブロックで書く宣言的 UI（作っていない方の案）

**これは、いま動くものの仕様ではない。**

宣言的な UI は[ui.md](ui.md) の「宣言的な層」として**配列を返す形で作ってある**。

```shark
return [ui.label(f"{count} 回"), ui.button("押す", "inc")];   // ← いま動く形
```

ここに書いてあるのは、それとは別の書き方——**呼び出しに続けてブロックを書く形**の
下書き。こちらを実現するには、言語の側に記法（`ui.button("押す") { ... }`）と、
ブロックが外側の変数を `ref` で捕まえる規則が要る。いまの構文には無い。

配列の形を選んだ理由は [ui.md](ui.md) の「なぜ配列なのか」にある。
この文書は、将来ブロックを入れるときのために設計の控えとして残しておく。

---

「GUI を標準でサポートし、宣言的に UI を表現できるようにする」を担当する層。

## 考え方

「どう描くか」ではなく「今どうあるべきか」を書く。
状態が変わったら、`ui` の記述をもう一度たどり直し、前回との差だけを実際の画面に反映する。

```shark
func main() -> int {
  var count = 0;

  ui.window("かうんた") {
    ui.column {
      ui.text(f"{count} 回");
      ui.button("押す") {
        count += 1;
      }
    }
  }
  return 0;
}
```

## 状態の持ち方

`ui.window` のブロックと、その中のイベント処理は、
**外側の変数を `ref` として捕まえる**。

```
main のスコープ（count がある）
 └ ui.window(...) { ... }   ← 窓が閉じるまで返らない
```

`ui.window` は窓が閉じるまで返らないので、`count` は必ず生きている。
そのため [memory.md](../runtime/memory.md) の「`ref` は呼んでいる間だけの借用」という規則を破らずに、
ボタンから外側の変数を書き換えられる。

- `ui.window` の外の変数を、別のタスクから触ることはできない
- 状態を別モジュールに持たせたいときは、`ui.window` に `ref` で渡す

## 再描画

1. イベント処理（ボタンなど）が終わる
2. `ref` で捕まえた変数のどれかが書き換わっていたら、再構築の対象にする
3. `ui` のブロックをもう一度たどり、前回の構造と比べる
4. 違うところだけを描画に反映する

比較は「同じ位置の同じ種類の部品は同じもの」とみなす。
リストのように数が変わるものには `ui.key()` で目印を付ける。

```shark
for var f in fishes {
  ui.key(f.id) {
    ui.text(f.name);
  }
}
```

## 部品の一覧

### 入れ物

| 関数 | 説明 |
|---|---|
| `ui.window(title: string) { }` | 窓。閉じるまで返らない |
| `ui.column { }` | 縦に並べる |
| `ui.row { }` | 横に並べる |
| `ui.stack { }` | 重ねる |
| `ui.scroll { }` | はみ出したらスクロールさせる |
| `ui.key(k: string) { }` | 数の変わる並びに目印を付ける |

```shark
ui.window("さかな") {
  ui.row {
    ui.column { ui.text("左"); }
    ui.column { ui.text("右"); }
  }
}
```

### 表示

| 関数 | 説明 |
|---|---|
| `ui.text(s: string)` | 文字 |
| `ui.image(b: bytes)` | 画像（PNG） |
| `ui.spacer()` | 余りを埋める |
| `ui.divider()` | 区切り線 |

### 入力

書き換える変数は `ref` で渡す（[../runtime/memory.md](../runtime/memory.md)）。

| 関数 | 説明 |
|---|---|
| `ui.button(label: string) { }` | 押されたらブロックを実行する |
| `ui.text_field(ref value: string)` | 1行の入力欄 |
| `ui.text_area(ref value: string)` | 複数行の入力欄 |
| `ui.input_dialog(title: string, initial: string) -> string?` | 入力ダイアログを開く |
| `ui.checkbox(ref on: bool, label: string)` | チェック |
| `ui.slider(ref v: float, lo: float, hi: float)` | つまみ |
| `ui.select(ref index: int, options: list<string>)` | 選択 |

```shark
var name = "";
var agreed = false;

ui.column {
  ui.text_field(ref name);
  ui.checkbox(ref agreed, "同意する");
  ui.button("送る") {
    if agreed { send(name); }
  }
}
```

### 調整

入れ子にして、中の部品の見え方を変える。

| 関数 | 説明 |
|---|---|
| `ui.padding(n: int) { }` | 内側の余白 |
| `ui.size(w: int, h: int) { }` | 大きさ |
| `ui.align(a: string) { }` | `"left"` `"center"` `"right"` |
| `ui.color(fg: string) { }` | 文字の色。`"#RRGGBB"` |
| `ui.background(bg: string) { }` | 背景の色 |
| `ui.font(size: int) { }` | 文字の大きさ |

```shark
ui.padding(16) {
  ui.color("#0B6E77") {
    ui.font(24) { ui.text("さめ"); }
  }
}
```

寸法は論理ピクセル。画面の細かさが違っても同じ大きさに見える。

### 日本語などの入力

入力欄を選ぶと、**OS の入力ダイアログが開く**。確定した文字列が変数に入る。

```shark
ui.text_field(ref name);      // 選ぶとダイアログが開き、確定した文字列が name に入る
```

- 変換中の文字列を欄の中に表示する方式（インライン変換）は**行わない**
- 移植層に求めるのは「文字列入力ダイアログを開く」だけ
  （[../runtime/platform.md](../runtime/platform.md)）
- ダイアログを持たない環境では、英数字の直接入力だけができる

描画を自前で行う以上、変換中の下線や候補一覧まで自前で描くと、
OS ごとの作り込みが際限なく増える。ダイアログに任せることで移植層を薄く保つ。

### そのほか

| 関数 | 説明 |
|---|---|
| `ui.quit()` | 窓を閉じて `ui.window` から抜ける |
| `ui.redraw()` | 明示的に描き直す |

```shark
ui.button("終わる") { ui.quit(); }
```

部品の細かい既定値（余白の量、色の名前、フォントの選び方）は未定
（[../open-questions.md](../open-questions.md)）。

## 描画

- 描画は自前で行い、プラットフォームには「画面に出す面」と「入力イベント」だけを求める
  （[platform.md](../runtime/platform.md)）
- ゲームに組み込む場合、その面はゲーム本体が渡す。ゲームの画面の一部に描くことになる
  （[../runtime/embedding.md](../runtime/embedding.md)）
- 文字の描画には Unicode の字形が要る。フォントは OS のものを使い、無ければ内蔵のものを使う
- OS ごとの見た目の違いは追わない。どこでも同じ見た目にする

## 時間のかかる処理

イベント処理は **UI タスクの上で順に実行される**。
そのため状態の書き換えが描画と競合することはない。

ただし、イベント処理の中で待つ関数（`sleep`、通信、ファイル読み）を直接呼ぶと、
その間 UI タスクが止まり、画面が固まる。コンパイル時に警告を出す。

```shark
ui.button("読み込む") {
  var body = http.get(url);   // 警告: UI が止まる
}
```

時間のかかる処理は `task` で別タスクに逃がし、`Task<T>` を状態として持つ
（[runtime/concurrency.md](../runtime/concurrency.md)）。

```shark
var loading: Task<Result<string>>? = none;

ui.window("よみこみ") {
  ui.button("読み込む") {
    loading = task http.get(url);    // すぐ返る
  }

  if var t = loading {
    if t.done() {
      ui.text(t.wait().value() ?? "失敗しました");
    } else {
      ui.text("読み込み中...");
    }
  }
}
```

タスクが終わった時点で再描画の対象になる。`await` のような記法は要らない。
