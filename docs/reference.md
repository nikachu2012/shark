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
| 必要になったら | ジェネリクス `task` `parallel` `ui` | 8、14、15 |

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
インタフェースを実装していない型を並べ替えるときは、比較の仕方をその場で渡します。

```shark
fishes.sort_by(func(a: Fish, b: Fish) -> bool { return a.size() < b.size(); });
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

クラスに `to_string()` を定義すると、`string(obj)` で使われます。

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
| `std.text` | Unicode、正規表現 | [text.md](../spec/library/text.md) |
| `std.fmt` | 桁揃え、桁区切り | [fmt.md](../spec/library/fmt.md) |
| `std.json` | JSON の読み書き | [json.md](../spec/library/json.md) |
| `std.net` / `std.http` | TCP と HTTP | [net.md](../spec/library/net.md) / [http.md](../spec/library/http.md) |
| `std.os` | 引数、環境変数、外部プログラム | [os.md](../spec/library/os.md) |
| `std.task` | タスクとチャネル | [task.md](../spec/library/task.md) |
| `std.ui` | GUI | [ui.md](../spec/library/ui.md) |
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

## 15. GUI

「どう描くか」ではなく「今どうあるべきか」を書きます。状態が変わると、
記述をたどり直して前回との差だけを画面に反映します。

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

`ui.window` は窓が閉じるまで返りません。だからブロックの中から外側の `count` を
`ref` として捕まえられます（借用の規則を破っていません）。

入力欄を選ぶと **OS の入力ダイアログが開き**、確定した文字列が変数に入ります。
変換中の文字を欄の中に表示する方式（インライン変換）は行いません。描画を自前でしている以上、
変換中の下線や候補一覧まで作り込むと OS ごとの手間が際限なく増えるためです。

数の変わるリストには `ui.key()` で目印を付けます。

```shark
for var f in fishes {
  ui.key(f.id) { ui.text(f.name); }
}
```

イベント処理は UI タスクの上で順に実行されるので、状態の書き換えが描画と競合しません。
ただし待つ関数を直接呼ぶと画面が固まるため、時間のかかる処理は `task` に逃がし、`Task<T>` を状態として持ちます。

```shark
var loading: Task<Result<string>>? = none;

ui.button("読み込む") {
  loading = task http.get(url);      // すぐ返る
}

if var t = loading {
  if t.done() { ui.text(t.wait().value() ?? "失敗"); }
  else        { ui.text("読み込み中..."); }
}
```

タスクが終わった時点で再描画されます。`await` のような記法は要りません。

見た目は OS ごとの流儀に合わせず、どこでも同じにします。描画は自前で行い、
プラットフォームには「描く面」と「入力」だけを求めます。

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
shark build main.shk    # バイトコードにする
```

この `shark` コマンドは、コアの外側に**別で作る**フロントエンドです。
ファイルを読んでコアに渡し、返ってきた診断を端末向けに整形します。
学習用ゲーム本体も、同じコアを呼ぶ別のフロントエンドにあたります。

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
| 実装しながら決める | GUI の既定値、実行時コンパイルのしきい値、Unicode の表の大きさ、C++ の規格 |
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
