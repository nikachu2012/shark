# 標準ライブラリ

「Python のような Battery included」を実現する構成。
インストール不要で、処理系に最初から入っている。

## 組み込み

`import` なしで使えるもの（`print` `input` `sleep` `len` `range` `panic` `assert` と型変換）は
[builtin.md](builtin.md) にまとめてある。

## モジュール

`import std.xxx;` で取り込み、`xxx.` を付けて使う。

| モジュール | 中身 | |
|---|---|---|
| `std.time` | 時刻、日付、期間 | [time.md](time.md) |
| `std.file` | ファイルの読み書き | [file.md](file.md) |
| `std.path` | パスの組み立てと分解 | [path.md](path.md) |
| `std.math` | 数学、丸め、乱数 | [math.md](math.md) |
| `std.crypto` | ハッシュと、当てられては困る乱数 | [crypto.md](crypto.md) |
| `std.text` | Unicode、正規表現、文字コード | [text.md](text.md) |
| `std.fmt` | 桁揃え、桁区切り | [fmt.md](fmt.md) |
| `std.json` | JSON の読み書き | [json.md](json.md) |
| `std.net` | TCP の通信 | [net.md](net.md) |
| `std.http` | HTTP でのやり取り | [http.md](http.md) |
| `std.os` | 引数、環境変数、外部プログラム | [os.md](os.md) |
| `std.task` | タスクとチャネル | [task.md](task.md) |
| `std.ui` | GUI | [ui.md](ui.md) |
| `std.test` | テスト | [test.md](test.md) |

`std.ui` と `std.task` は、`ui.` `task` `parallel` が構文と結び付いているため、
使うファイルでは `import` を省略できる。

## 必須と任意

必須にするのは**最小限**だけ。次の2つを両方満たすものに限る。

1. 移植層の必須項目（メモリ・時間・標準入出力・終了）だけで実装できる
2. 埋め込むデータが小さい

| 区分 | モジュール |
|---|---|
| 必須 | `time` `math` `task` |
| 任意 | `file` `path` `text` `fmt` `json` `net` `http` `os` `crypto` `ui` `test` |

`text` と `json` は OS の支援こそ要らないが、Unicode の表を抱えるため任意にする。
組み込み向けでは外せる。`crypto` はハッシュだけなら必須項目で足りるが、
乱数のもとを移植層に頼るため任意にする（[crypto.md](crypto.md)）。

どのモジュールを入れるかは、**使う側が選ぶ**。
雛形は全部入れた状態で配るので、要らないものは**ビルドに含めない**
（[../skeleton.md](../skeleton.md)）。ファイルは消さない。後から雛形の直しを取り込めなくなるため。
入れたうえで、学習の段階に応じて使えるものを絞ることもできる
（[../runtime/embedding.md](../runtime/embedding.md)）。

### 持っていないモジュールを取り込んだとき

**`import` の時点でエラーにする。**実行を始める前に止まる。

```
error[E0501]: この処理系は std.net を持っていません
  --> main.shk:2:1
  2 | import std.net;
    | --------------
  この処理系が持つモジュール: time, math, task, file, path
```

呼んだときに失敗を返す形にはしない。プログラムが動き出してから、
使えないと分かるのでは遅いため。

なお、モジュールを持っていても実行時に失敗することはある
（ファイルが無い、通信できないなど）。それは `Result` の失敗として返る。

## 決めごと

- 外部ライブラリには依存しない。必要な表（Unicode、乱数、圧縮）は処理系に埋め込む
  （[../runtime/platform.md](../runtime/platform.md)）
- 失敗しうる関数は `Result<T>`、理由が1つしかない失敗は `T?` を返す
  （[../runtime/error.md](../runtime/error.md)）
- 待つ関数は、そのタスクだけを止める（[../runtime/concurrency.md](../runtime/concurrency.md)）
- 同じ入力なら、どの環境でも同じ結果を返す

## バージョン間の約束

**ソースコードの上で動きが変われば、それは互換を壊したことになる。**
逆に、ソースの動きが同じであれば、中身は自由に変えてよい。

| 対象 | 約束 |
|---|---|
| 関数の名前・引数・戻り値 | 変えない。増やすのはよい |
| 同じ入力に対する結果 | 変えない |
| 実装のしかた（内部の表、アルゴリズム） | 変えてよい |
| バイトコードの形式 | 変えてよい。`.shkc` は同じバージョンでだけ動く |

`.shkc` を配る場合は、処理系のバージョンも一緒に示す
（[../runtime/execution.md](../runtime/execution.md)）。

## 決めていないこと

- 埋め込む Unicode の表をどこまで小さくできるか（作って測ってから決める）
