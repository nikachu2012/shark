# プログラミング言語　Shark🦈

## 名前の由来
サメってなんか可愛いよね

## 用途

**ゲーム機で動く、プログラミング学習用ゲームの中で使う言語。**
プレイヤーはゲームの中でこの言語を書き、その場で動かして学ぶ。

この前提から、次が決まっている。

| 前提 | 仕様への影響 |
|---|---|
| 書くのはプログラミングを学び始めた人 | 入口を短く、エラーは直し方まで示す |
| 動かす場所はゲーム機 | ファイルもスレッドも当てにしない。外部ライブラリに依存しない |
| ゲーム本体の中で動く | 処理系は**ゲームに組み込む部品**であって、単体のアプリではない |
| 画面はゲームが持っている | 描画も入力もホスト（ゲーム本体）から借りる |
| 止まると困る | 無限ループでゲームごと固まらないよう、少しずつ実行できる |
| メモリも限られている | プログラムを動かすのに使ってよい量をホストが決め、超えたら実行時エラーで止める |

くわしくは [runtime/embedding.md](runtime/embedding.md)。

## 実装の分け方

実装は**実行系（コア）だけ**にまとめる。
コマンドラインから叩くフロントエンドは、その外側に**別で実装する**。

```
  ゲーム本体（学習用ゲーム）        shark コマンド
            \                      /
             \                    /        ← どちらもコアを呼ぶ側
        ┌─────────────────────┐
        │        実行系（コア）        │
        │  字句解析・構文解析・型検査  │
        │  バイトコード生成・仮想マシン │
        │  標準ライブラリ              │
        └─────────────────────┘
```

| | 中身 | 決めている場所 |
|---|---|---|
| 実行系（コア） | ソース（文字列）を受け取って動かす。診断は構造化データで返す | [runtime/embedding.md](runtime/embedding.md) |
| フロントエンド（別実装） | `shark run` などのコマンド。ファイルを読み、診断を端末向けに整形する | [frontend.md](frontend.md) |

- コアは**自分からファイルを読まない**。標準出力にも直接書かない。すべて呼ぶ側が渡す
- そのため、ゲームに組み込んでも、コマンドとして使っても、同じコアが動く
- この仕様書で「処理系」と書いたら、断りがない限りコアを指す

### コアは雛形として配る

コアは **C++** で書き、**そのまま使えるところまで作り込んだ雛形**として git で配る。
使う側は複製して、自分のゲームの中で使う。

手を入れる場所は2か所に絞る。

- `platform/` を自分の機種のものに**差し替える**
- `host` にゲーム側の操作を**足す**

それ以外はそのまま使えることを目指す。触る場所が少ないほど、
雛形が直ったときに上流から取り込みやすい。
構文と型の規則、残した関数の意味は**変えない**。
くわしくは [skeleton.md](skeleton.md)。

## メイン思想
- 初学者むけにわかりやすさを残しつつ、高度なこともできるようにするプログラミング言語を目指す。
- 抽象度の高いインタプリタおよび実行時コンパイル方式を採用
- 外部依存が少なく、容易に多くのプラットフォームに移植することができる
- Pythonのような "Battery included" な言語とする。
  - 文字列、バイト列、可変長配列、Key-value形式のデータ構造などを相互に、簡単に持てるようにする
  - GUI、日付、Unicode、フォーマット文字列などに対応させる
- C++的なオブジェクト指向言語とする
- 型を厳格に定め、型推論を行う。
- 全ての代入はデータコピーとし、参照渡しは別の方法で行えるようにする
- GUIを標準でサポートし、宣言的にUIを表現できるようにする。
- １秒待つなどのOS支援を要する関数を簡単に実現できるようにする

## サンプル構文
```
func add(a: int, b: int) -> int {
  return a + b;
}

func main() -> int {
  print(add(1, 2)); // 3

  if condition {
  } else if condition2 {
  } else {
  }

  for var i in listed {
    // for loop
  }

  while condition {
    // while loop
  }
}
```

## わかりやすさと高度さの両立

思想の1行目を、仕様の側で守るための決めごと。
初学者向けの説明と学ぶ順番は [../docs/reference.md](../docs/reference.md) が受け持ち、
仕様書には書かない。

- **入口を短くする** — `main` を書かなくても、文を並べただけで動く
  （[runtime/execution.md](runtime/execution.md)）
- **厳しさは緩めない** — 暗黙変換・`null`・例外・`async` を持たない。
  規則が少ないほど、初学者にも上級者にも読みやすい
- **止めるときは直し方まで示す** — エラーの書式を仕様で固定する
  （[runtime/diagnostics.md](runtime/diagnostics.md)）
- **高度な機能は既定で効かない** — `ref` `virtual` ジェネリクス `task` は、
  書いたときだけ効く

## 仕様書の構成

このファイルは思想を定める。個々の決めごとは以下に分けて書く。

### 書き方

| ファイル | 内容 |
|---|---|
| [syntax.md](syntax.md) | 字句規則、文の一覧、演算子の優先順位 |

### 型 — [types/](types/)

| ファイル | 内容 |
|---|---|
| [types/primitive.md](types/primitive.md) | `int` `float` `bool` `void` |
| [types/collection.md](types/collection.md) | `string` `bytes` `list` `map` |
| [types/conversion.md](types/conversion.md) | 型変換（暗黙変換なし） |
| [types/inference.md](types/inference.md) | 型推論の範囲 |
| [types/optional.md](types/optional.md) | 値がない状態（`null` を持たない） |
| [types/class.md](types/class.md) | クラス、継承、可視性 |
| [types/generics.md](types/generics.md) | ジェネリクス |

### 処理系 — [runtime/](runtime/)

| ファイル | 内容 |
|---|---|
| [runtime/execution.md](runtime/execution.md) | 仮想マシンと実行時コンパイル、呼び出しの深さ |
| [runtime/memory.md](runtime/memory.md) | 値セマンティクス、参照カウント、`ref`、実行時のメモリの上限 |
| [runtime/concurrency.md](runtime/concurrency.md) | 並行処理（`async` / `await` を持たない） |
| [runtime/error.md](runtime/error.md) | エラー処理（例外を持たない） |
| [runtime/module.md](runtime/module.md) | モジュールと `import` |
| [runtime/platform.md](runtime/platform.md) | 移植層 |
| [runtime/embedding.md](runtime/embedding.md) | 実行系とホストの境界、組み込みの手順 |
| [runtime/diagnostics.md](runtime/diagnostics.md) | エラーメッセージの書き方 |

### 標準ライブラリ — [library/](library/)

| ファイル | 内容 |
|---|---|
| [library/overview.md](library/overview.md) | 構成、必須と任意 |
| [library/builtin.md](library/builtin.md) | `import` なしで使えるもの |
| [library/time.md](library/time.md) | `std.time` 時刻と期間 |
| [library/file.md](library/file.md) | `std.file` ファイル |
| [library/path.md](library/path.md) | `std.path` パス |
| [library/math.md](library/math.md) | `std.math` 数学と乱数 |
| [library/text.md](library/text.md) | `std.text` Unicode と正規表現 |
| [library/fmt.md](library/fmt.md) | `std.fmt` 桁揃え |
| [library/json.md](library/json.md) | `std.json` JSON |
| [library/net.md](library/net.md) | `std.net` TCP |
| [library/http.md](library/http.md) | `std.http` HTTP |
| [library/os.md](library/os.md) | `std.os` 引数と環境変数 |
| [library/task.md](library/task.md) | `std.task` タスクとチャネル |
| [library/ui.md](library/ui.md) | `std.ui` GUI と宣言的 UI の実行モデル |
| [library/test.md](library/test.md) | `std.test` テスト |

### そのほか

| ファイル | 内容 |
|---|---|
| [skeleton.md](skeleton.md) | コアの雛形。何を入れ、どこを書き換えるか |
| [frontend.md](frontend.md) | コマンドライン実装と編集環境の支援 |
| [open-questions.md](open-questions.md) | 決めていないことの一覧 |

利用者向けの入門は [../docs/reference.md](../docs/reference.md)。
