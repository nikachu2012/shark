# 組み込み

`import` なしでどこからでも使える関数と型。

## 入出力

| 関数 | 説明 |
|---|---|
| `print(values: Any...)` | 標準出力に出して改行する。どんな型でも、いくつでも渡せる。`sep:` と `end:` で区切りと終わりを変えられる |
| `write(values: Any...)` | 改行せずに出す。どんな型でも、いくつでも渡せる。`sep:` と `end:` も同じ |
| `input() -> string?` | 標準入力から1行読む。まだ無ければ来るまで待つ。終端なら `none` |

```shark
print("さめ");           // さめ
print(42);               // 42
print("答え:", 42);      // 答え: 42（空白で区切る）
print();                 // 改行だけ
print(1, 2, 3, sep: "-");   // 1-2-3
print("つづく", end: "");   // 改行しない
write("名前: ");
var name = input() ?? "";
```

`input()` は行が打たれるまで待つ。待っている間もホストは止まらない
（[../runtime/embedding.md](../runtime/embedding.md)）。

`print` と `write` はどんな型でも、いくつでも受け取る（`Any` は「どんな型でも」の
意味で、ここだけの書き方。変数の型には書けない）。複数渡すと空白で区切って出し、
`print()` は改行だけを出す。

区切りと終わりは、名前を付けた引数で変えられる。`sep:` は値の間に入る文字列
（既定は空白）、`end:` は最後に足す文字列（既定は `print` が改行、`write` は空）。
名前で渡せるのはこの2つだけで、位置で渡したものはすべて値の並びに入る。

クラスを渡したときは、

- `to_string()` があればそれを呼ぶ。`virtual` なら実際の型のものが呼ばれる
- 無ければ `クラス名(メンバ: 値, ...)` の形で出す

```shark
print(fish);        // さめ(400cm)      to_string() があるとき
print(rock);        // Rock(w: 3)       無いとき
```

`T?` を渡したときは、値があれば同じように `to_string()` を呼び、無ければ `none` と出す。

`to_string()` を外から呼べるようにするには `public` を付ける。
呼び出しの差し替えは検査のときに決まるので、静的な型がクラスのときに働く。
型引数のときは、制約に `to_string()` を持つインタフェースを書けば同じように呼ばれる。

文字列そのものが要るときは `string(v)` を使う。
`string()` はクラスの `to_string()` が無いと誤りになる
（[../types/conversion.md](../types/conversion.md)）。

## 待つ・止める

| 関数 | 説明 |
|---|---|
| `sleep(sec: float) -> void` | 指定秒待つ。このタスクだけが止まる |
| `panic(msg: string) -> void` | 記録を残して止める。戻ってこない |
| `assert(cond: bool, msg: string) -> void` | `cond` が偽なら `panic` する |

```shark
sleep(1.0);                          // 1秒待つ
assert(xs.len() > 0, "空の配列");
```

## 数え上げ

| 関数 | 説明 |
|---|---|
| `len(s: string)` `len(b: bytes)` | 長さ |
| `len<T>(xs: list<T>)` `len<K, V>(m: map<K, V>)` | 長さ |
| `range(end: int) -> Range` | `0` から `end-1` |
| `range(start: int, end: int) -> Range` | `start` から `end-1` |
| `range_step(start: int, end: int, step: int) -> Range` | 刻み幅を指定する |

```shark
for var i in range(3) { print(i); }              // 0 1 2
for var i in range(1, 4) { print(i); }           // 1 2 3
for var i in range_step(10, 0, -2) { print(i); } // 10 8 6 4 2
```

`Range` は値を全部持たず、回すたびに次を作る。

## 型変換

| 関数 | 説明 |
|---|---|
| `int(v) -> int` / `int(s: string) -> int?` | 整数へ |
| `float(v) -> float` / `float(s: string) -> float?` | 小数へ |
| `string(v) -> string` | 文字列へ |
| `bool(s: string) -> bool?` | 真偽値へ |

詳しくは [../types/conversion.md](../types/conversion.md)。

## 型

| 型 | 説明 |
|---|---|
| `Result<T>` | 成功なら値、失敗なら理由（[../runtime/error.md](../runtime/error.md)） |
| `Error` | 失敗の理由 |
| `Task<T>` | 走っているタスク（[task.md](task.md)） |
| `channel<T>` | タスク間の受け渡し |
| `Range` | 数の並び |

## 注意

`len` は型ごとのオーバーロードとして定義されている。
処理系だけの特別な仕組みは使っておらず、同じものをユーザーが書ける。

`print` と `write` だけは例外で、どんな型でも受け取る。同じものはユーザーには書けない。
