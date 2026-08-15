# エラー処理

例外は持たない。エラーは**戻り値**で伝える。

例外を持たない理由は、実装が大きくなり、移植先ごとに巻き戻しの仕組みが要るため。
また、どの行から飛ぶか読めないコードを避けるため。

## 2種類のエラー

| 種類 | 例 | 扱い |
|---|---|---|
| 回復できる | ファイルが無い、変換に失敗した | `Result<T>` を返す |
| 回復できない | 0 除算、範囲外の添字、`int` のあふれ、メモリの使いすぎ | `panic` して停止 |

## Result&lt;T&gt;

成功なら値、失敗なら理由を持つ型。

```shark
func read_config(path: string) -> Result<string> {
  if !file.exists(path) {
    return Error(f"設定ファイルがありません: {path}");
  }
  return file.read(path);
}
```

## 受け取り方

### 1. その場で調べる

```shark
var r = read_config("a.toml");
if r.ok() {
  print(r.value());
} else {
  print(r.error().message());
}
```

### 2. 中身を取り出す `if var`

```shark
if var text = read_config("a.toml") {
  print(text);
} else var e {
  print(e.message());
}
```

### 3. 呼び出し元に投げ返す `try`

失敗ならその場で `return` する。成功なら中身を取り出して続ける。

```shark
func load() -> Result<Config> {
  var text = try read_config("a.toml");   // 失敗ならここで返る
  return parse(text);
}
```

`try` は `Result` を返す関数の中でだけ書ける。

### 4. 既定値にする

```shark
var text = read_config("a.toml") ?? "";
```

## Error

```shark
class Error {
  public func message() -> string;
  public func code() -> int;
}
```

独自のエラーは `Error` を継承して作る。

## panic

回復できない誤りは `panic` でその場で止める。

```shark
panic("ここには来ないはず");
```

- 呼び出しの経路（スタックトレース）を出して終了する
- 途中で捕まえることは**できない**。プログラムの誤りは隠さず落とす、という方針
- メモリを使いすぎたときも `panic` で止める。落ちるのではなく、
  どこで使いすぎたかを示して止まる（[memory.md](memory.md)）
- ただし並行処理では、1つのタスクの panic で全体を落とさないよう、
  タスク単位で捕まえて記録する（[concurrency.md](concurrency.md)）

## 無視させない

`Result` を返す関数の戻り値を、受け取らずに捨てるとコンパイル時の警告になる。
意図して捨てるときは `_ = f();` と書く。

警告には `try` `if var` `_ =` の3つを並べて示す（[diagnostics.md](diagnostics.md) E0202）。
