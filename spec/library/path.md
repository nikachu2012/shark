# std.path

パスの文字列を組み立て、分解する。ファイルを読み書きするのは [file.md](file.md)。

```shark
import std.path;
```

| 関数 | 説明 |
|---|---|
| `path.join(a: string, b: string) -> string` | つなぐ |
| `path.join_all(parts: list<string>) -> string` | まとめてつなぐ |
| `path.dir(p: string) -> string` | 親ディレクトリ |
| `path.name(p: string) -> string` | ファイル名（拡張子つき） |
| `path.stem(p: string) -> string` | ファイル名（拡張子なし） |
| `path.ext(p: string) -> string` | 拡張子（`.` を含む。無ければ空文字） |
| `path.normalize(p: string) -> string` | `.` や `..` を畳む |
| `path.absolute(p: string) -> Result<string>` | 絶対パスにする |
| `path.is_absolute(p: string) -> bool` | 絶対パスかどうか |
| `path.separator() -> string` | 区切り文字（`/` か `\`） |

```shark
var p = path.join("data", "fish.json");   // "data/fish.json"

print(path.dir(p));      // "data"
print(path.name(p));     // "fish.json"
print(path.stem(p));     // "fish"
print(path.ext(p));      // ".json"

print(path.normalize("a/b/../c"));   // "a/c"
```

区切り文字は環境によって違うが、`path.join` は環境に合ったものを使う。
プログラムの中では `/` で書いておいてよい。
