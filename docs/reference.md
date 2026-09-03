# Shark🦈 言語リファレンス（簡易版）

Shark は、**ゲーム機で動くプログラミング学習用ゲームの中で使う**ための言語です。
抽象度の高いインタプリタ／実行時コンパイル方式を採っています。
外部依存が少なく移植しやすいこと、標準ライブラリだけで一通り書けること（Battery included）、
型が厳格であること、そして**すべての代入がコピーである**ことを大事にしています。

各章の末尾に、詳しい決めごとを書いた仕様書へのリンクがあります。

## 読む順番

上から順に覚えれば、下を知らなくてもプログラムは書けます。
下のものは、書かなければ無いのと同じです。

| 段階 | 覚えるもの | 章 |
|---|---|---|
| 最初 | `var` `func` `if` `for` `while` `print` `list` `map` `string` | 1〜7、11 |
| 次 | `class` 継承 `T?` `Result` `try` | 8〜10 |
| その次 | `ref` `virtual` `override` インタフェース | 4、8 |
| 必要になったら | ジェネリクス その場に書く関数 `task` `parallel` `std.ui` | 7、8、14、15 |

---

## 1. はじめてのプログラム

```shark
func main() -> int {
  print("Hello, Shark!");
  return 0;
}
```

- プログラムは `main` 関数から始まります。返した `int` が終了コードです。
- 文の終わりには `;` を付けます。ブロックで終わる文には付けません。
- ファイルの拡張子は `.shk`、文字コードは UTF-8 です。

```shark
// 行末までコメント
/* 囲みコメント。入れ子にできる /* ここも */ まだコメント */
```

### main を書かなくてもよい

文をそのまま並べたファイルも、スクリプトとして実行できます。

```shark
// hello.shk
print("Hello, Shark!");
```

```
shark run hello.shk
```

上から順に実行され、終了コードは 0 です。`main` を書くようになっても動きは変わりません。
`main` があるファイルにトップレベルの文を書くとエラーになります。

### エラーが出たら

Shark は型に厳しいので、書き始めのうちはよくエラーが出ます。
黙って動いて後で壊れるより、その場で止める方針です。
**エラーは必ず直し方まで書いてあります。**

```
error[E0102]: int と float は足せません
  --> main.shk:3:13
  3 | var total = count + price;
    |             ----- int
    |                     ----- float
  直し方: float(count) + price と書くと足せます
```

番号で詳しい説明も読めます。

```
shark explain E0102
```

→ [spec/syntax.md](../spec/syntax.md) / [runtime/diagnostics.md](../spec/runtime/diagnostics.md)

---

## 2. 変数と型推論

型は厳格ですが、関数の中では書かなくても推論されます。

```shark
var n = 10;          // int
var pi = 3.14;       // float
var name = "Shark";  // string
var xs = [1, 2, 3];  // list<int>

var count: int = 0;  // 明示してもよい
const MAX = 100;     // 定数
```

**関数の引数・戻り値・クラスのメンバには型注釈が必須**です。呼ぶ側が定義を読まずに使えるようにするためです。

推論はその行だけで完結します。後の行の使われ方から遡って決めることはしません。

```shark
var xs = [];              // エラー: 型が決まらない
var xs: list<int> = [];   // OK
```

一度決まった型は変わりません。

→ [spec/types/inference.md](../spec/types/inference.md)

---

## 3. 型

### 基本型

| 型 | 説明 | 例 |
|---|---|---|
| `int` | 64ビット整数（どの環境でも64ビット） | `42` |
| `float` | 64ビット小数 | `3.14` |
| `bool` | 真偽値 | `true` / `false` |
| `string` | 文字列（Unicode） | `"さめ"` |
| `bytes` | バイト列 | `b"\x01\x02"` |
| `void` | 戻り値なし | — |

**暗黙の型変換はありません。** `int` と `float` すら混ぜて計算できません。

```shark
var c = 1 + 2.0;          // エラー
var d = float(1) + 2.0;   // OK
```

整数どうしの `/` は整数を返し、0 方向に切り捨てます（`-7 / 2` は `-3`）。
`%` の符号は割られる数に従います（`-7 % 2` は `-1`）。C 言語と同じです。

条件式に書けるのは `bool` だけです。`if n {}` は書けません。

### コレクション型

```shark
var xs: list<int> = [1, 2, 3];
var m: map<string, int> = {"a": 1, "b": 2};

xs.push(4);
xs[0];        // 1
xs.len();     // 4

m["c"] = 3;
m.has("a");   // true
for var k in m.keys() { print(m[k]); }
```

- `string` は**文字単位**で数えます（`"さめ🦈".len()` は 3）。バイト単位で触るときは `bytes` に変換します。
- `map` の反復順は**挿入した順**です。環境によって変わりません。

### 型変換

```shark
var n = int("123");      // 失敗しうるので int? が返る
var f = float(1);        // 1.0
var s = string(42);      // "42"
var b = s.bytes();       // バイト列へ
var h = b.to_hex();      // 16 進の文字列へ（"e38195..."）
var b2 = h.from_hex();   // 16 進から戻す。読めなければ none
```

→ [spec/types/primitive.md](../spec/types/primitive.md) / [collection.md](../spec/types/collection.md) / [conversion.md](../spec/types/conversion.md)

---

## 4. コピーと参照（Shark の中心）

**すべての代入はデータのコピー**です。渡した先での変更は、呼び出し元に影響しません。

```shark
var a = [1, 2, 3];
var b = a;      // 中身がまるごとコピーされる
b.push(4);
print(a.len()); // 3 （a は変わらない）
```

同じ実体を触りたいときだけ `ref` を使います。**呼ぶ側にも書きます**。

```shark
func add_one(ref xs: list<int>) -> void {
  xs.push(1);
}

var xs = [1];
add_one(ref xs);  // 呼び出しを見れば書き換わると分かる
print(xs.len());  // 2
```

`ref` は「呼んでいる間だけの借用」です。変数に取っておくことも、返すことも、
クラスのメンバに持つこともできません。無効な参照が残ることが構造上ありえなくなります。

**この設計から導かれること：**

- 実装は書き込み時コピー（COW）。代入では複製せず、書き込む瞬間に複製します。
- 循環参照が作れないので、ガベージコレクタは不要で、参照カウントで足ります。停止時間が読めるため GUI に有利です。
- 別スレッドに渡す値もコピーなので、データ競合が起きません。

→ [spec/runtime/memory.md](../spec/runtime/memory.md)

---

## 5. 演算子

強く結び付く順に並べています。

| 順位 | 演算子 |
|---|---|
| 1 | `a.b` `a(...)` `a[...]` |
| 2 | `**`（冪乗） |
| 3 | `!a` `-a` `~a` |
| 4 | `*` `/` `%` |
| 5 | `+` `-` |
| 6 | `<<` `>>` |
| 7 | `<` `<=` `>` `>=` |
| 8 | `==` `!=` |
| 9 | `&` |
| 10 | `^` |
| 11 | `\|` |
| 12 | `&&` |
| 13 | `\|\|` |
| 14 | `??`（値なしのときの既定値） |

代入は `=` `+=` `-=` `*=` `/=` `&=` `\|=` `^=` `<<=` `>>=`。
文字列は `+` で連結できます。

### ビット演算と冪乗

ビット演算は `int` だけに使えて、動きは **C と同じ**です。

```shark
print(12 & 10);        // 8    どちらも立っているビット
print(12 | 10);        // 14   どちらかが立っているビット
print(12 ^ 10);        // 6    片方だけ立っているビット
print(~12);            // -13  すべて反転
print(1 << 10);        // 1024 左へずらす
print(-16 >> 2);       // -4   右へずらす（符号は保つ）
print(f"{5 << 2:b}");  // 10100
```

- ずらす幅に `0` 〜 `63` の外を書くと実行時エラーになります
- `&` `^` `|` は比較より弱く結び付きます（C と同じ）。
  `n & 3 == 3` は `n & (3 == 3)` と読まれるので、`(n & 3) == 3` と書きます
- 真偽値をまとめるのは `&&` と `||` です

冪乗は `**` です。右結合で、単項より強く結び付きます。

```shark
print(2 ** 10);        // 1024
print(2 ** 3 ** 2);    // 512（2 ** 9）
print(-2 ** 2);        // -4（-(2 ** 2)）
print(2.0 ** 0.5);     // 1.4142135623730951
```

`int ** int` の指数が負のときと、あふれたときは実行時エラーです。

→ [spec/syntax.md](../spec/syntax.md)

---

## 6. 制御構文

```shark
if condition {
  // ...
} else if condition2 {
  // ...
} else {
  // ...
}
```

条件式に `( )` は不要、ブロックの `{ }` は省略不可です。

```shark
for var i in listed {        // コレクションを回す
  print(i);
}

for var i in range(0, 10) {  // 0〜9
  print(i);
}

while condition {
  if x < 0 { continue; }
  if x > 100 { break; }
}
```

→ [spec/syntax.md](../spec/syntax.md)

---

## 7. 関数

```shark
func add(a: int, b: int) -> int {
  return a + b;
}

print(add(1, 2)); // 3
```

- 引数と戻り値の型は必ず書きます。戻り値がなければ `-> void`。
- 引数はコピー渡し。書き換えたいときは `ref`（4章）。

### 同じ名前の関数

引数の型か個数が違えば、同じ名前で何個でも定義できます。

```shark
func abs(x: int) -> int { if x < 0 { return -x; } return x; }
func abs(x: float) -> float { if x < 0.0 { return -x; } return x; }

print(abs(-3));      // int の方
print(abs(-3.0));    // float の方
```

暗黙の型変換が無いので、**引数の型が完全に一致するもの**が選ばれます。当てはまるものは多くても1つで、
どれが呼ばれるかは引数を見れば決まります。

戻り値の型だけが違うものは定義できません。`math.abs` や `print` も、この仕組みで書かれています。

### 可変長引数

最後の引数の型に `...` を付けると、余った引数をいくつでも受け取れます。
関数の中では、まとめて1つの `list` になっています。

```shark
func sum(xs: int...) -> int {
  var t = 0;
  for var x in xs { t += x; }   // xs は list<int>
  return t;
}

print(sum());          // 0
print(sum(1, 2, 3));   // 6
```

書けるのは最後の引数に1つだけで、`ref` は付けられません。
同じ名前で個数がぴったり合う関数があれば、そちらが選ばれます。

### キーワード引数

呼び出す側で `名前: 値` と書くと、引数を名前で渡せます。名前を付けた引数は
位置で渡す引数より後ろに書き、付ければ並びは自由です。

```shark
func window(title: string, width: int, height: int) -> void { }

window("さめ", height: 240, width: 320);
```

省略できる引数は無いので、渡す個数は変わりません。処理系が持つ関数
（`math.abs` など）・型変換・関数の値には使えません。例外として `print` と
`write` だけは `sep:`（値の区切り）と `end:`（終わりの文字）を名前で渡せます。

```shark
print(1, 2, 3, sep: "-");   // 1-2-3
print("つづく", end: "");   // 改行しない
```

### その場に書く関数

名前を付けずに、渡すその場に書けます。書き方は上と同じで、名前だけがありません。

```shark
func apply(n: int, f: func(int) -> int) -> int { return f(n); }

print(apply(3, func(x: int) -> int { return x * 2; }));   // 6
```

値としての型は `func(引数の型, ...) -> 戻り値の型` です。変数に入れる・配列や連想配列に入れる・
関数から返す、のどれもできます。戻り値を書かなければ `-> void` と同じです。

```shark
var note = func(s: string) -> void { print(s); };
note("さめ");
```

**外側の変数は見えません。**見えるのは自分の引数と自分の中で作った変数、
それに一番外側に書いたもの（`var` `const` `func` `class`）だけです。

```shark
var n = 0;
var bump = func() -> void { n += 1; };   // エラー(E0156): 外側の n は見えない
```

関数の値は「**どの関数か**」を指すだけで、値を持ち出しません。だから渡しても写しても
余分なものが付いてこず、「コピーした値を書き換えたのに外に届かない」という迷いも起きません。
使いたいものは引数で受け取り、どこからでも書き換えたい値は一番外側の `var` にします。

→ [spec/syntax.md](../spec/syntax.md)

---

## 8. クラス

C++ 的なオブジェクト指向です。ただし**インスタンスも値**で、代入するとコピーされます。

```shark
class Fish {
  var name: string;
  var size: int;

  func init(name: string, size: int) {   // コンストラクタ
    this.name = name;
    this.size = size;
  }

  public func describe() -> string {
    return f"{this.name} ({this.size}cm)";
  }
}

var f = Fish("サメ", 400);
print(f.describe());
```

- メンバへは `this.` を必ず付けます。
- 既定は `private`。外に見せるものに `public` を付けます。

### 継承と virtual

実装を持つ親は1つまでです。**上書きできるのは `virtual` を付けたメソッドだけ**で、
上書きする側には `override` が必要です。

```shark
class Fish {
  var name: string;

  virtual func describe() -> string {    // 上書きしてよい
    return this.name;
  }
  func id() -> int { return 1; }         // 上書きできない
}

class Shark : Fish {
  func init(name: string) { super.init(name, 400); }

  override func describe() -> string {
    return f"🦈 {super.describe()}";
  }
}
```

| 書き方 | 意味 |
|---|---|
| `virtual func f()` | 子が上書きしてよい |
| `override func f()` | 親の `virtual` を上書きする |
| `func f()` | 上書きできない |

`override` を書き忘れたり、親に `virtual` が無いのに同じ名前を定義したりすると、
コンパイル時にエラーになります。書き間違いで別のメソッドが増えるのを防ぐためです。

呼ばれるのは、`virtual` なら**実体**の型のもの、そうでなければ**変数**の型のものです。

```shark
var f: Fish = Shark("さめ");
print(f.describe());   // Shark の方
print(f.id());         // Fish の方
```

コピーしても実体の型は変わりません。C++ と違い、親の型に入れたときに
親の部分だけが切り取られること（スライシング）は起きません。

### 純粋仮想とインタフェース

本体を書かない `virtual` メソッドが**純粋仮想**（C++ の `= 0`）です。
1つでも持つクラスは抽象クラスになり、インスタンスを作れません。

```shark
class Shape {
  virtual func area() -> float;      // 本体なし
}

var s = Shape();                     // エラー: 抽象クラス
```

**メンバ変数を持たず、純粋仮想だけを並べたクラス**がインタフェースです。
専用の書き方はなく、普通のクラスとして書きます。継承の2番目以降に、いくつでも並べられます。

```shark
class Swimmer {
  virtual func speed() -> float;
}

class Shark : Fish, Swimmer {        // 実装を持つ親は1つ、あとはインタフェース
  override func speed() -> float { return 8.0; }
}

func race(a: Swimmer, b: Swimmer) -> string {
  if a.speed() > b.speed() { return "先"; }
  return "後";
}
```

実装を持つ親を2つ以上書くことはできません（菱形継承を避けるため）。

### Comparable と This

インタフェースの中では `This` と書けます。**実装したクラス自身**を指します。

```shark
class Comparable {                         // 処理系が用意しているもの
  virtual func compare(other: This) -> int;
}

class Fish : Comparable {
  override func compare(other: Fish) -> int {   // This は Fish になる
    if this.size < other.size { return -1; }
    if this.size > other.size { return 1; }
    return 0;
  }
}
```

`int` `float` `string` `bytes` `bool` は、処理系が `Comparable` を実装済みとして扱います。
そのため `list<int>` も `list<Fish>` も同じ `sort()` で並べ替えられます。
基本型にユーザーが後からインタフェースを実装することはできません。

メソッドとコンストラクタもオーバーロードできます。`virtual` と `override` は、
引数まで含めて対応を見ます。

```shark
class Fish {
  func init(name: string) { this.init(name, 0); }
  func init(name: string, size: int) { ... }
}
```

`==` は、実体の型が同じならメンバを順に比べます。
演算子の多重定義と静的メソッドは持ちません。

### ジェネリクス

```shark
func first<T>(xs: list<T>) -> T? {
  if xs.len() == 0 { return none; }
  return xs[0];
}

class Box<T> {
  var value: T;
}
```

型引数の後ろに `: インタフェース名` を書くと、それを実装した型だけを受け取れます。
制約を書けば、そのインタフェースが約束するメソッドを `T` に対して呼べます。

```shark
func max<T: Comparable>(a: T, b: T) -> T {
  if a.compare(b) > 0 { return a; }
  return b;
}

print(max(1, 2));            // int は Comparable
print(max(fish_a, fish_b));  // Fish が Comparable を実装していれば
```

複数要るときは `+` でつなぎます。制約の無い `T` には、代入とコピーしかできません。
インタフェースを実装していない型を扱うときは、比べ方をその場で渡します（7章）。

```shark
func max_by<T>(a: T, b: T, greater: func(T, T) -> bool) -> T {
  if greater(a, b) { return a; }
  return b;
}

var big = max_by(a, b, func(x: Fish, y: Fish) -> bool { return x.size() > y.size(); });
```

→ [spec/types/class.md](../spec/types/class.md) / [generics.md](../spec/types/generics.md)

---

## 9. 値がないとき

**`null` はありません。**「値がないかもしれない」ことは型に書きます。

```shark
var a: int = 1;      // 必ず値がある
var b: int? = none;  // 値がないこともある

var c: int = b;      // エラー: int? は int ではない
```

取り出し方は3つです。

```shark
var n = int("abc") ?? 0;      // 1. 既定値を決める

if var n = maybe_num {        // 2. ある時だけ処理する
  print(n);                   //    ここでは n は int
} else {
  print("値なし");
}

var n = maybe_num!;           // 3. 無いはずがないと断言（none なら停止）
```

`m.get(k)`、`xs.pop()`、`s.find(x)`、`int(s)` など、失敗しうる操作が `T?` を返します。

→ [spec/types/optional.md](../spec/types/optional.md)

---

## 10. エラー処理

**例外はありません。** エラーは戻り値で伝えます。実装が小さくなり、
どこから飛ぶか読めないコードを避けられるためです。

| 種類 | 例 | 扱い |
|---|---|---|
| 回復できる | ファイルが無い | `Result<T>` を返す |
| 回復できない | 0除算、範囲外の添字、あふれ、メモリの使いすぎ | `panic` して停止 |

```shark
func read_config(path: string) -> Result<string> {
  if !file.exists(path) {
    return Error(f"設定ファイルがありません: {path}");
  }
  return file.read(path);
}
```

受け取り方は4つです。

```shark
if var text = read_config(p) {      // 1. 中身を取り出す
  print(text);
} else var e {
  print(e.message());
}

var text = try read_config(p);      // 2. 失敗なら呼び出し元へ返す
var text = read_config(p) ?? "";    // 3. 既定値にする
if r.ok() { print(r.value()); }     // 4. その場で調べる
```

`Result` を受け取らずに捨てると警告になります。意図して捨てるときは `_ = f();` と書きます。

配列や文字列が増え続けて、使ってよいメモリの量を超えたときも `panic` で止まります。
落ちるのではなく、上限がいくつだったかを示して止まるので、どこで増やしすぎたかを探せます。

→ [spec/runtime/error.md](../spec/runtime/error.md)、[spec/runtime/memory.md](../spec/runtime/memory.md)

---

## 11. 文字列とフォーマット

```shark
var s = "さめ🦈";
s.len();            // 3（文字数）
s.sub(0, 2);
s.split(",");
s.trim();
s.contains("め");

var name = "Shark";
var n = 3;
print(f"{name} を {n} 匹みつけた");   // Shark を 3 匹みつけた
```

### 書式指定

`{ }` の中は `式:書式` と書けます。書式は Python に倣っています。

```
{式:[埋め文字][寄せ][0][幅][,][.桁数][種類]}
```

```shark
print(f"{n:5}");        // "   42"      幅5で右寄せ
print(f"{n:<5}|");      // "42   |"     左寄せ（^ で中央）
print(f"{n:05}");       // "00042"      ゼロ埋め
print(f"{1234567:,}");  // "1,234,567"  桁区切り
print(f"{pi:.2f}");     // "3.14"       小数2桁
print(f"{n:x}");        // "2a"         16進（b は2進、o は8進）
print(f"{0.125:.1%}");  // "12.5%"      百分率
print(f"{name:*<10}");  // "さめ******" 埋め文字を指定
```

幅は表示幅で数えます（全角は 2）。`{` `}` そのものは `{{` `}}` と書きます。
日付は書式指定では扱わず、`time` の `format()` を使います。

クラスに `public func to_string() -> string` を定義すると、`string(obj)` と
`f"{obj}"` と `print(obj)` で使われます。`print` は `to_string()` が無いクラスでも渡せて、
そのときは `クラス名(メンバ: 値, ...)` の形で出ます。

```shark
class Fish {
  var name: string;
  func init(n: string) { this.name = n; }
  public func to_string() -> string { return this.name; }
}
class Rock { var w: int; func init(w: int) { this.w = w; } }

print(Fish("さめ"));   // さめ
print(Rock(3));        // Rock(w: 3)
```

→ [spec/types/collection.md](../spec/types/collection.md)

---

## 12. モジュール

1ファイルが1モジュールです。ファイル名がそのままモジュール名になります。

```shark
import std.time;      // 標準ライブラリ
import ./util;        // 同じ場所の util.shk
import ./util as u;   // 別名

util.add(1, 2);       // モジュール名を付けて使う
```

トップレベルの宣言は既定で非公開。`public` を付けたものだけが外から見えます。
名前を直接持ち込む形（`from ... import ...`）は用意しません。どこから来た名前か読んで分かるようにするためです。

循環 import はエラーです。

→ [spec/runtime/module.md](../spec/runtime/module.md)

---

## 13. 標準ライブラリ

インストール不要です。次のものは `import` なしで使えます。

```shark
print(v);  write(v);  input();          // 入出力
sleep(1.0);  panic(msg);  assert(c, m); // 待つ・止める
len(v);  range(0, 10);                  // 数え上げ
int(s);  float(s);  string(v);  bool(s);// 型変換
```

そのほかは `import std.xxx;` で取り込み、`xxx.` を付けて使います。
使える関数の一覧は `make docs` で HTML にできます（`docs/reference/`）。
ライブラリごとに1枚あり、**すべての関数に動く例**が付いています。

| モジュール | 中身 | 仕様 |
|---|---|---|
| `std.time` | 時刻、日付、期間 | [time.md](../spec/library/time.md) |
| `std.file` | ファイルの読み書き | [file.md](../spec/library/file.md) |
| `std.path` | パスの組み立てと分解 | [path.md](../spec/library/path.md) |
| `std.math` | 数学、丸め、乱数 | [math.md](../spec/library/math.md) |
| `std.crypto` | ハッシュと安全な乱数 | [crypto.md](../spec/library/crypto.md) |
| `std.text` | Unicode、正規表現 | [text.md](../spec/library/text.md) |
| `std.fmt` | 桁揃え、桁区切り | [fmt.md](../spec/library/fmt.md) |
| `std.json` | JSON の読み書き | [json.md](../spec/library/json.md) |
| `std.net` / `std.http` | TCP と HTTP | [net.md](../spec/library/net.md) / [http.md](../spec/library/http.md) |
| `std.os` | 引数、環境変数、外部プログラム | [os.md](../spec/library/os.md) |
| `std.task` | タスクとチャネル | [task.md](../spec/library/task.md) |
| `std.ui` | 画面に描く | [ui.md](../spec/library/ui.md) |
| `std.test` | テスト | [test.md](../spec/library/test.md) |

### よく使うもの

```shark
import std.time;
import std.file;
import std.http;
import std.json;

var now = time.now();
print(now.format("YYYY-MM-DD hh:mm:ss"));
var diff = now - time.date(2026, 8, 15)!;    // Duration
print(f"{diff.days()} 日経った");

var text = try file.read("data.txt");
for var name in try file.list("data") { print(name); }

var res = try http.get("https://example.com/fish.json");
var root = try json.parse(res.body());
var name = root["items"][0]["name"].string() ?? "名無し";   // 途中が無くても止まらない
```

時刻 `Time` と期間 `Duration` は別の型です。足し引きの誤りを型で防ぎます。

### 使えない環境があるもの

| 区分 | モジュール |
|---|---|
| 必須 | `time` `math` `task` |
| 任意 | `file` `path` `text` `fmt` `json` `net` `http` `os` `ui` `test` |

必須は最小限です。処理系が持たないモジュールを `import` すると、**実行を始める前にエラー**に
なります。動き出してから使えないと分かるのでは遅いためです。

取り込めた後で、ファイルが無い・通信できないといった失敗が起きたときは `Result` で返ります。

→ [spec/library/overview.md](../spec/library/overview.md)

---

## 14. 並行処理

**`async` / `await` はありません。**すべての関数は上から順に実行されます。
「待つ関数」と「待たない関数」を区別しないので、関数に色が伝染しません。

```shark
func fetch(url: string) -> Result<string> {
  var conn = net.connect(url);     // ここで待つ
  return conn.read();              // 上から順に読める
}
```

この関数は、そのまま呼んでも、タスクとして走らせても構いません。**呼び方を変えるために関数を書き直す必要はありません。**

### task

関数呼び出しの前に `task` と書くと、別のタスクで走り始め、その場で次の行に進みます。

```shark
var t = task fetch(url);     // 走り始める。ここでは待たない
do_other_work();
var r = t.wait();            // 終わるまで待って結果を受け取る
```

`t.done()` で終了確認、`t.cancel()` で取り消し要求ができます。

### parallel

まとめて走らせ、全部終わるのを待ちます。ブロックを抜けた時点で、中のタスクは必ず全部終わっています。

```shark
var pages = parallel {
  task fetch(url_a);
  task fetch(url_b);
  task fetch(url_c);
};                    // ここで3つとも終わる
print(pages.len());   // 3。書いた順に並ぶ
```

### channel

タスク間で値を渡します。**送り手は何個でもよく、受け手は1つ**です。

```shark
var ch = channel<string>();
task worker(ch, "a");
task worker(ch, "b");

for var i in range(2) {
  print(ch.recv()!);     // 先に終わった方から届く
}
```

「どれか先に終わった方を処理する」はこれで書けるので、**複数のチャネルを同時に待つ書き方
（`select` にあたるもの）はありません**。種類の違う通知を捌きたいときは、チャネルと
受け取るタスクを分けます。1本のチャネルに意味の違う通知を混ぜない、という決めごとです。

`channel` は、コピーしても同じ待ち行列を指す**唯一の例外**です。それ以外の値は必ずコピーされます。

### 取り消し

`t.cancel()` は**要求を立てるだけ**で、待っている関数を外から断ち切りません。
待つ関数（`sleep`、`recv`、通信）が要求を見て、その場でやめます。

```shark
var t = task download(url);
t.cancel();                                   // すぐには止まらない
var r = t.wait_timeout(time.seconds(1.0));    // 止まったかを見る
```

確認するのはコアが実行を区切って戻るたび（ゲームなら1フレームごと）なので、
効くまでの遅れはその1回分です。OS のシグナルは使いません。

### 待つとどうなるか

```shark
sleep(1.0);     // このタスクだけが1秒止まる。他のタスクは動き続ける
```

タスクの切り替えは仮想マシンの中で行われ、**OS のスレッドは必須ではありません**。
**既定はスレッド1本**で、すべてのタスクを1本の上で切り替えます。
切り替わるのは待ちが発生した時点だけで、処理の途中に割り込まれることはありません。

そのため、ロックも排他制御も、スレッド数の設定も要りません。
待ちの多い処理（通信、ファイル、`sleep`）はこれで十分に重ねられます。

タスクに渡る値はコピーで、`ref` はタスクの境界を越えられません。だからロックを書く必要がありません。

→ [spec/runtime/concurrency.md](../spec/runtime/concurrency.md)

---

## 15. 画面に描く

`std.ui` には層が2つあります。**下の層**は画素の並び1枚（面）と、押された・動いたという
出来事だけ。**上の層**は、部品を配列にして返すと、そのとおりに描いてくれます。
混ぜて使えます。

`shark` コマンドは、macOS と Linux では**窓を開きます**（外部のライブラリは使っていません）。
窓を持てないところ（ssh の先など）では、見えない面に描きます。

### 下の層 — 自分で描く

くり返しの形はいつも同じです。

```shark
import std.ui;

func main() -> int {
  ui.open("さかな", 160, 120);
  var x = 10;

  while ui.poll() {                        // 1. 押された・動いた を取り込む
    if ui.pressed("esc") { ui.quit(); }
    if ui.key("right") { x += 2; }

    ui.clear(ui.rgb(0, 20, 40));           // 2. 描く
    ui.fill_circle(x, 60, 12, ui.rgb(255, 140, 60));
    ui.text(4, 4, "press esc", ui.rgb(255, 255, 255));

    ui.present();                          // 3. 画面に出す
    ui.frame();                            // 4. 次のこまの刻みまで待つ
  }
  ui.close();
  return 0;
}
```

描けるのは、点・線・四角・円・文字と、画素の並びをそのまま置く `ui.blit()` です。
色は `int` 1つで、`ui.rgb(赤, 緑, 青)` で作ります。

`ui.poll()` は待ちません。`ui.key()` などが答えるのは
「最後に `poll` したときの様子」なので、1回のくり返しの中では答えが変わりません。

### 上の層 — いまどうあるべきかを返す

部品を作って**配列に入れて返す**と、そのとおりに描かれます。
くり返しも描き直しも `ui.run()` が引き受け、**押されたときの動きは部品に持たせます**。

```shark
import std.ui;

var count = 0;                        // 状態はふつうの変数

func view() -> list<Widget> {         // いまどうあるべきか
  return [
    ui.label(f"{count} 回"),
    ui.row([
      ui.button("ふやす", func() -> void { count += 1; }),   // 押されたときの動き
      ui.button("へらす", func() -> void { count -= 1; }),
    ]),
  ];
}

func main() -> int {
  ui.run("かうんた", 420, 300, view);
  return 0;
}
```

動きは名前を付けて渡してもかまいません（`ui.button("ふやす", inc)`）。長いものや、
何か所からも使うものは、名前を付けた方が読みやすくなります。

まとめて振り分けたいときは、関数の代わりに**名札**を渡して `update()` で受けます。

```shark
ui.button("ふやす", "inc")            // ui.show() が "inc" を返す

func update(hit: string) -> void {
  if hit == "inc" { count += 1; }
}
ui.run("かうんた", 420, 300, view, update);
```

値を持つ部品は、**変数を `ref` で渡す**のがいちばん短い書き方です。
動いたらその変数が直に書き換わるので、名札も `update()` も `ui.value()` も要りません。

```shark
ui.checkbox("音を出す", ref sound),      // 押すと sound が入切する
ui.slider(ref volume, 0, 100),           // 動かすと volume が変わる
ui.number(ref age, 0, 120),
ui.combo(ref iro, iro_na),
ui.listbox(ref sakana, sakana_na, 5),
ui.tabs(ref page, ["メモ", "いろ"]),
ui.radio("小", ref size, 0),             // 押すと size に 0 が入る
```

これでも**部品は状態を持ちません**。値は自分の変数にあり、処理系が覚えるのは
「どの `var` か」だけです。だから「いまの状態」と「画面」が食い違いません。

入力欄も同じです。打たれるたびに、その変数が書き換わります。

```shark
var name = "さめ";                    // 一番外側の var に置きます
var memo = "1行目\n2行目";

ui.field(ref name),                   // 打つと name が変わる
ui.textarea(ref memo, 5),             // 複数行。5 行ぶんの高さで出す
```

`ui.textarea` は **`enter` で改行が入ります**（`ui.field` は入力欄から離れます）。
離れるのは、外を押すか `esc` です。置ける幅に入らない行はひとりでに折り返し、
上下の矢印で行を行き来できます。中身は改行を持つ**1つの文字列**なので、
行に分けたいときは `memo.split("\n")` のように自分で分けます。

`ref` で渡せるのは**一番外側の `var`** だけです（誤り `E0307`）。
覚えているのは借用ではなく「どの `var` か」で、書き戻しはその `var` への代入と同じだからです。
ほかのモジュールの `var`（`state.name`）でもかまいません。

`ui.show()` が返すのは、押されたものの**名札**（自分で決めた `id`）です。
値を持つ部品なら、新しい値は `ui.value()` か `ui.text_value()` で受け取ります。

```shark
ui.checkbox("音を出す", "sound", sound),   // 押されると "sound" が返る
ui.slider("volume", volume, 0, 100),       // つまむと "volume" が返る
ui.field("name", name),                    // 打つと "name" が返る（ref を使わない書き方）
ui.textarea("memo", memo),                 // 複数行の入力欄も同じ
```

いくつかの中から1つ選ぶ部品は3つあります。どれも並べるものを `list<string>` で渡し、
**選ばれた番号**（0 から）が `ui.value()` で返ります。

```shark
ui.radio("小", "s0", size == 0),           // 丸を並べる。2〜4 個くらいのとき
ui.combo("iro", iro_na, iro),              // 押すと一覧が出る。数が多いとき
ui.listbox("sakana", sakana_na, sel, 5),   // 一覧が出たまま。見比べたいとき
```

`ui.combo` は、押して離してからもう一度押しても、**押したまま動かして離しても**選べます。
一覧が画面に入りきらないときは上と下に ▲▼ が出て、そこに合わせているあいだ送れます。

`ui.listbox` も入りきらないときに右へ帯が出ます。**帯はつまんで動かせます。**

巻物を持つ部品（`ui.listbox`・`ui.textarea`・`ui.combo` の一覧）は、
**カーソルを乗せて車輪（ホイール）を回しても送れます**。押さなくてもかまいません。
送りは行ごとに飛ばず、**なめらかに動きます**（上と下に半端に切れた行が出ます）。
自分で巻物を書くときは `ui.wheel()`（下が正）と `ui.wheel_x()`（右が正）で受け取ります。
部品が使ったぶんは 0 になるので、二重に動くことはありません。

タブは**見出しだけ**を出します。中身は自分で選んで返します。

```shark
ui.col([
  ui.tabs("tab", ["メモ", "いろ"], tab),
  body(),        // tab を見て、いまの中身を組みます
])
```

数を打つ入力欄は、**上と下の限りから外に出られません**。
限りから出る数は打っても入らないので、範囲の外の値になることがありません。
右の − と ＋、上下の矢印でも 1 ずつ動きます。

```shark
ui.number("age", age, 0, 120),             // 0 から 120 まで
```

あいことばは `ui.password` です。中身の持ち方は `ui.field` と同じで、
**出すときだけ** 1 字が 1 つの `*` になります。

どの部品にも `.tooltip("説明")` を付けられます。カーソルを合わせて少し待つと出ます。

あいだを空けたいときは `ui.spacer()` です。**並んでいる向き**に場所を取るので、
`ui.row` の中なら横、`ui.col` の中なら縦に空きます。数を省くと余りをぜんぶ取るので、
端に寄せたいときに使えます（`ui.space(h)` は縦にしか空きません）。

```shark
ui.row([ui.label("なまえ"), ui.spacer(), ui.button("消す", "del")]),   // 右端へ
ui.row([ui.spacer(20), ui.label("すこし右")]),                          // 20 画素ぶん
```

入力欄には `.placeholder("なまえ")` を付けられます。空のあいだだけ、うすく出ます。
出しているだけなので中身にはなりません。

```shark
ui.field("name", name).placeholder("なまえ"),
```

```shark
if hit == "sound" { sound = ui.value() == 1; }
if hit == "name"  { name = ui.text_value(); }
```

**部品は状態を持ちません。**値は自分で持ち、毎回渡し直します。
だから「いまの状態」と「画面に出ているもの」が食い違いません。

見た目は、メソッドをつないで変えられます。**返るのは変えたもので、元は変わりません。**

```shark
ui.label("さめ").color(ui.rgb(255, 200, 0)).background(ui.rgb(30, 30, 40)).padding(6)
ui.button("ok", "ok").width(200).align("center")
ui.center([ui.label("まんなか")])
ui.divider()
```

`ui.run()` は Shark 自身で書かれていて、中では下の層を呼んでいるだけです。
自分でくり返しを書きたいときは `ui.poll()` / `ui.show()` / `ui.present()` を並べます。

### 細かい画面（HiDPI）

面の1画素は、画面の1画素にそのまま乗ります。細かい画面（Retina）では、
面もそのぶん細かく取ると、字がなめらかに出ます。

```shark
var k = ui.scale();                  // ふつうは 1、細かい画面なら 2
ui.open("さめ", 420 * k, 300 * k);   // 見た目の大きさは変わりません
_ = ui.font(12 * k);                 // 12pt くらい
```

部品の寸法は字の大きさから決まるので、`ui.font()` を変えれば全体の釣り合いが付いてきます。

### 画面が無くても動く

画面を持たない機種や、`SHARK_UI=off` のときは、描く先が**見えない面**になります。
描き方は何も変わりません。結果は `ui.get()` で読めるので、
**画面の要るプログラムでもテストが書けます**。

```shark
ui.open(8, 8);
ui.set(2, 3, ui.rgb(255, 0, 0));
print(ui.get(2, 3));      // 16711680
```

`ui.to_png()` で、そのときの面を PNG として取り出せます。

### 日本語を出す

内蔵の字形は ASCII だけです。日本語などは `ui.font()` で機種のフォントを読むと出せます
（処理系を FreeType つきで作ったときだけ。作り方は [README](../README.md) の
「日本語の字を出す」）。

```shark
if !ui.font(12 * ui.scale()) { print("フォントが見つかりません"); }
ui.text(8, 8, "こんにちは", ui.rgb(255, 255, 255));
```

読まなければ内蔵の字形のままです。内蔵ならどの機種でも同じ形・同じ大きさで出ますが、
機種のフォントを読むと `ui.text_width()` の答えもその機種のものになります。

### 日本語を打つ

`ui.field` と `ui.textarea` は日本語も打てます。変換は**出し先に任せます**。

| 出し先 | どうなるか |
|---|---|
| 窓（macOS） | OS の変換が効きます。変換中の文字には下線が出ます |
| ブラウザ | ブラウザの変換がそのまま効きます |
| そのほか | 打った文字がそのまま入ります |

変換中のキー（確定の enter、取り消しの esc）はプログラムには届きません。
確定しても入力欄から焦点は外れません。

文字はなぞって選べます（shift＋矢印でも）。続けて押すと、まとめて選べます。

| 押す回数 | 選ばれるもの |
|---|---|
| 2回 | 語（空白から空白まで。日本語は書かれたひと続き） |
| 3回 | 行（折り返しではなく、書かれた改行まで） |
| 4回 | ぜんぶ |

取り消しは **Cmd-Z**（Windows と Linux は Ctrl-Z）、やり直しは **Shift-Cmd-Z**（Ctrl-Y でも同じ）です。
続けて打った字はまとめて1回ぶんに戻ります。

**右で押すと切り貼りのメニュー**が出ます。
窓（macOS）では Cmd-C / Cmd-V / Cmd-X / Cmd-A も効きます。
置き場は `ui.clipboard()` と `ui.set_clipboard()` でプログラムからも触れます。

自分でメニューを出したいときは `ui.menu(x, y, 並べるもの)` と `ui.menu_pick()` です。

自分で入力欄を描くときは `ui.input()` と `ui.marked()` を使います。

### 気をつけること

- 内蔵の字形は ASCII だけです。日本語などは □ になります（部品のラベルも同じ）
- 「キーを離した」が届かない機種があります。押しっぱなしで動かしたいときは、
  `ui.pressed()` で向きを覚えておくと、どの機種でも同じに動きます

動く例が [examples/paint.shk](../examples/paint.shk)（下の層）と
[examples/counter.shk](../examples/counter.shk)（上の層）、
[examples/widgets.shk](../examples/widgets.shk)（部品をぜんぶ出したもの）、
[examples/breakout.shk](../examples/breakout.shk)（絵と透明を使ったブロック崩し）、
[examples/cube3d.shk](../examples/cube3d.shk)（三角形と奥行きで描く 3D）
にあります。

→ [spec/library/ui.md](../spec/library/ui.md)

---

## 16. 実行と移植

ソースは 字句解析 → 構文解析 → 型検査 → バイトコード → 仮想マシン の順に処理されます。
型検査は実行前に必ず終わります。よく実行される関数は機械語に変換されますが、
**この仕組みが無い環境でも動く**ことを保証します。

### 実行系は組み込んで使う

実装されているのは**実行系（コア）だけ**です。コアは単体で動くアプリではなく、
ゲーム本体などのホストに組み込んで使う部品です。C++ で書かれ、
**そのまま使えるところまで作り込んだ雛形**として git で配られます。
手を入れるのは移植層（自分の機種のものに差し替える）と、
ホスト関数（ゲーム側の操作を足す）の2か所だけで、それ以外はそのまま使えます。
構文と型の規則は変えません。

| コアがしないこと | 代わりに |
|---|---|
| ファイルを開く | ソースは文字列で渡してもらう |
| 画面や端末に書く | `print` の出力はホストが受け取る |
| エラーを整形して出す | 診断は構造化データで返す |

ホストは実行する量を区切って呼びます。ゲームなら毎フレーム少しずつ進めるので、
**無限ループを書いてもゲームは固まりません**。ゲーム側の操作（`move` など）は、
ホストが関数として登録すれば、普通の関数と同じように呼べます。

### コマンドは別の実装

```
shark run main.shk      # 実行
shark check main.shk    # 型検査だけ
shark build main.shk    # 1つで動く実行ファイルにする（→ ./main）
```

この `shark` コマンドは、コアの外側に**別で作る**フロントエンドです。
ファイルを読んでコアに渡し、返ってきた診断を端末向けに整形します。
学習用ゲーム本体も、同じコアを呼ぶ別のフロントエンドにあたります。

`shark build` で作ったものは、それだけで動きます。中身は
**バイトコード実行装置＋バイトコード**で、字句解析も型検査も入っていません
（作るときに済んでいるため）。書いたプログラムを、処理系を入れていない人にも渡せます。

### 移植

移植するときは、OS に頼る部分を集めた「移植層」だけを書きます。
必須はメモリ・時間・標準入出力・終了だけで、ファイルもスレッドも必須ではありません。
ゲーム機ではこれらが自由に使えないことがあるためです。外部ライブラリには依存しません。

→ [spec/runtime/execution.md](../spec/runtime/execution.md) / [embedding.md](../spec/runtime/embedding.md) / [platform.md](../spec/runtime/platform.md) / [spec/skeleton.md](../spec/skeleton.md) / [frontend.md](../spec/frontend.md)

---

## 17. 残っている検討課題

紙の上で決められることは、ひととおり決まりました。残っているのは次の3種類です。

| 区分 | 内容 |
|---|---|
| 実装しながら決める | 画面の日本語の字形と文字入力、部品をどこまで増やすか、実行時コンパイルのしきい値、Unicode の表の大きさ、C++ の規格 |
| 組み込みの詰め | ホストの関数にクラスを渡せるか、実行の途中状態を保存できるか |
| コアの外で決める | 言語サーバの形、整形規則、ツールの配り方 |

一覧と経緯は [spec/open-questions.md](../spec/open-questions.md) にあります。

---

## 付録: 予約語

```
func return var const if else while for in break continue
class this super public private virtual override ref import as
task parallel try panic
true false none
int float bool string bytes void list map
channel Task Result Error Range
```

型名も予約語です。同名の変数や型は作れません。

名前が要るところに予約語を書くと `E0003` が出ます。名前を数字で始めたときも同じ番号です。

```
error[E0003]: if は予約語です。変数の名前としては使えません
error[E0003]: 名前は数字で始められません
```
