# std.os

プログラムの外側とのやり取り。

```shark
import std.os;
```

## 起動時の情報

| 関数 | 説明 |
|---|---|
| `os.args() -> list<string>` | コマンド引数。1番目はプログラム名 |
| `os.env(name: string) -> string?` | 環境変数を読む |
| `os.set_env(name: string, value: string) -> void` | 環境変数を書く |
| `os.platform() -> string` | `"macos"` `"windows"` `"linux"` `"wasm"` `"embedded"` |

```shark
func main() -> int {
  var args = os.args();
  if args.len() < 2 {
    print("使い方: fish <ファイル>");
    return 1;
  }
  var home = os.env("HOME") ?? ".";
  return 0;
}
```

## 場所

| 関数 | 説明 |
|---|---|
| `os.cwd() -> Result<string>` | いまいるディレクトリ |
| `os.chdir(p: string) -> Result<void>` | 移動する |
| `os.temp_dir() -> string` | 一時ファイルの置き場所 |

## 終わる

| 関数 | 説明 |
|---|---|
| `os.exit(code: int) -> void` | すぐ終わる。戻ってこない |

`main` から `return` した方がよい。`os.exit` は走っているタスクを待たない。

## 外のプログラムを呼ぶ

| 関数・メソッド | 説明 |
|---|---|
| `os.run(cmd: string, args: list<string>) -> Result<Output>` | 終わるまで待つ |
| `o.code() -> int` | 終了コード |
| `o.out() -> string` `o.err() -> string` | 出力 |

```shark
var o = try os.run("git", ["status", "--short"]);
if o.code() == 0 { print(o.out()); }
```

任意モジュール。外のプログラムを呼べない処理系では、`os.run` だけが失敗を返す。

出力は UTF-8 の文字列として返る。Windows のように、機械の言語の符号（日本語なら
CP932）で書くプログラムと UTF-8 で書くプログラムが混ざる環境では、移植層が
**UTF-8 として筋が通っていればそのまま、通らなければ機械の符号として読み直す**。
どちらとも読めないバイトは、届いたまま渡る。
