# 型変換

暗黙の型変換は一切行わない。変換は必ずコードに書く。

## 変換の書き方

型名を関数のように呼ぶ。

```shark
int("123");      // 123
int(3.9);        // 3（0 方向に切り捨て）
float(1);        // 1.0
string(42);      // "42"
string(3.14);    // "3.14"
bool("true");    // true
```

## 失敗しうる変換

文字列から数値への変換のように失敗しうるものは、`T?` を返す（[optional.md](optional.md)）。

```shark
var n = int("abc");      // none
var m = int("12") ?? 0;  // 12
```

失敗しない変換（`int` → `float`、数値 → `string`）は、そのままの型を返す。

## 相互変換の一覧

| から \ へ | `int` | `float` | `string` | `bytes` | `list` |
|---|---|---|---|---|---|
| `int` | — | `float(n)` | `string(n)` | `n.to_bytes()` | — |
| `float` | `int(f)` 切り捨て | — | `string(f)` | — | — |
| `string` | `int(s)` → `int?` | `float(s)` → `float?` | — | `s.bytes()` | `s.chars()` |
| `bytes` | — | — | `b.to_string()` → `string?` | — | `b.list()` |
| `list<T>` | — | — | `xs.join(sep)`（`T` が `string` のとき） | — | — |

`bytes` → `string` が `string?` なのは、UTF-8 として不正なバイト列がありうるため。

## クラスの変換

ユーザー定義のクラスを `string` にしたいときは `to_string()` メソッドを定義する。
定義してあれば `string(obj)` と `f"{obj}"` の両方で使われる。

```shark
class Fish {
  var name: string;
  func to_string() -> string { return this.name; }
}
```
