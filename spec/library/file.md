# std.file

ファイルの読み書き。

```shark
import std.file;
```

任意モジュール。ファイルを扱えない処理系では `import` の時点でエラーになる
（[overview.md](overview.md)）。
取り込めた場合でも、ファイルが無い・権限が無いといった失敗は `Result` で返る。

## まるごと読み書き

| 関数 | 説明 |
|---|---|
| `file.read(p: string) -> Result<string>` | UTF-8 として読む |
| `file.read_bytes(p: string) -> Result<bytes>` | バイト列として読む |
| `file.write(p: string, text: string) -> Result<void>` | 上書きする |
| `file.write_bytes(p: string, b: bytes) -> Result<void>` | 上書きする |
| `file.append(p: string, text: string) -> Result<void>` | 末尾に足す |

```shark
var text = try file.read("data.txt");
_ = file.write("out.txt", text.upper());

if var body = file.read("なにか.txt") {
  print(body);
} else var e {
  print(e.message());
}
```

## 調べる・操作する

| 関数 | 説明 |
|---|---|
| `file.exists(p: string) -> bool` | あるかどうか |
| `file.is_dir(p: string) -> bool` | ディレクトリかどうか |
| `file.size(p: string) -> Result<int>` | バイト数 |
| `file.modified(p: string) -> Result<Time>` | 最終更新時刻 |
| `file.remove(p: string) -> Result<void>` | 消す |
| `file.copy(from: string, to: string) -> Result<void>` | 複製する |
| `file.rename(from: string, to: string) -> Result<void>` | 名前を変える |
| `file.list(dir: string) -> Result<list<string>>` | 中身の名前を並べる |
| `file.make_dir(p: string) -> Result<void>` | 作る。途中の階層も作る |

```shark
for var name in try file.list("data") {
  if path.ext(name) == ".json" {
    print(name);
  }
}
```

## 少しずつ読む

大きなファイルは、全部を持たずに読む。

| 関数・メソッド | 説明 |
|---|---|
| `file.open(p: string, mode: string) -> Result<File>` | `"r"` `"w"` `"a"` |
| `f.read_line() -> string?` | 1行読む。終端なら `none` |
| `f.read(n: int) -> Result<bytes>` | `n` バイト読む |
| `f.write(text: string) -> Result<void>` | 書く |
| `f.close() -> void` | 閉じる |

```shark
var f = try file.open("big.log", "r");
while var line = f.read_line() {
  if line.contains("エラー") { print(line); }
}
f.close();
```

`File` はハンドル型。コピーしても同じファイルを指す
（[../runtime/memory.md](../runtime/memory.md)）。
