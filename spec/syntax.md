# 構文

Shark のソースコードの書き方を定める。

## ファイル

- 拡張子は `.shk`
- 文字コードは UTF-8 のみ。BOM は付けても付けなくてもよい（読み飛ばす）
- 改行は `\n` と `\r\n` の両方を認める

## 字句

### 空白とコメント

空白・タブ・改行は区切りとしてのみ意味を持つ。

```
// 行末までコメント
/* 囲みコメント。入れ子にできる /* ここも */ まだコメント */
```

### 識別子

- 1文字目は英字か `_`、2文字目以降は英数字か `_`
- 大文字と小文字は区別する
- 日本語などの非 ASCII 文字は識別子に使えない（移植性のため）
- 予約語は識別子にできない

破ると E0003 になる。`var 1abc = 1;` は「名前は数字で始められません」、
`var if = 1;` は「if は予約語です。変数の名前としては使えません」と出る。

### リテラル

| 種類 | 書き方 | 型 |
|---|---|---|
| 整数 | `42` `0xFF` `0b1010` `1_000_000` | `int` |
| 小数 | `3.14` `1.0e-3` | `float` |
| 真偽 | `true` `false` | `bool` |
| 文字列 | `"さめ"` | `string` |
| バイト列 | `b"\x01\x02"` | `bytes` |
| フォーマット文字列 | `f"{name} さん"` | `string` |
| 配列 | `[1, 2, 3]` | `list<T>` |
| 連想配列 | `{"a": 1}` | `map<K, V>` |
| 値なし | `none` | `T?` |

数値リテラルの `_` は読みやすさのための区切りで、値には影響しない。

### 文字列のエスケープ

`\n` `\t` `\r` `\\` `\"` `\0` `\xNN`（バイト）`\u{XXXX}`（Unicode コードポイント）

## 文

トップレベルにも文を置ける。その場合は上から順に実行される
（[runtime/execution.md](runtime/execution.md)）。

文の終わりには `;` を付ける。ただしブロック `{ }` で終わる文（`if` `for` `while` `func` `class`）には付けない。

| 文 | 例 |
|---|---|
| 変数宣言 | `var n: int = 0;` |
| 定数宣言 | `const MAX = 100;` |
| 代入 | `n = 1;` `n += 1;` |
| 式文 | `print(n);` |
| 条件分岐 | `if c { } else if c2 { } else { }` |
| 繰り返し | `for var i in xs { }` / `while c { }` |
| 脱出 | `break;` `continue;` |
| 返却 | `return expr;` / `return;` |
| 関数定義 | `func f(a: int) -> int { }` |
| クラス定義 | `class C : Base, IFace { }` |
| 取り込み | `import std.time;` |

条件式に `( )` は不要。ブロックの `{ }` は1文でも省略できない。

### 値を取り出しながら分岐する

`T?` や `Result<T>` から中身を取り出しつつ、あるときだけブロックに入る書き方。

```shark
if var n = maybe_num {          // n は int
  print(n);
} else {
  print("値なし");
}

if var text = read_config(p) {  // Result の成功側
  print(text);
} else var e {                  // 失敗の理由を受け取る
  print(e.message());
}

while var line = f.read_line() {   // none が返るまで繰り返す
  print(line);
}
```

取り出した変数はブロックの中だけで使える
（[types/optional.md](types/optional.md)、[runtime/error.md](runtime/error.md)）。

## 同じ名前の関数（オーバーロード）

引数の**型か個数**が違えば、同じ名前の関数を何個でも定義できる。

```shark
func abs(x: int) -> int { if x < 0 { return -x; } return x; }
func abs(x: float) -> float { if x < 0.0 { return -x; } return x; }

print(abs(-3));      // int の方
print(abs(-3.0));    // float の方
```

### 選び方

呼び出しの引数の型が**完全に一致する**ものを選ぶ。

- 暗黙の型変換が無いので、当てはまるものは多くても1つしかない
- 当てはまるものが無ければエラー。近いものを候補として示す
  （[runtime/diagnostics.md](runtime/diagnostics.md)）
- 順位付けも優先規則も要らない。どれが呼ばれるかは引数を見れば決まる

### 決めごと

- **戻り値の型だけが違うもの**は定義できない（呼び出しから選べないため）
- 引数の名前だけが違うものも定義できない
- ジェネリック関数と普通の関数の両方が当てはまるときは、**普通の方**を選ぶ
- メソッドとコンストラクタ（`init`）も同じようにオーバーロードできる
- `override` は、親の同じ引数のものを上書きする（[types/class.md](types/class.md)）

```shark
class Fish {
  func init(name: string) { this.init(name, 0); }
  func init(name: string, size: int) { ... }
}
```

## 型引数の制約

型引数の後ろに `: インタフェース名` を書くと、それを実装した型だけを受け取れる。
複数要るときは `+` でつなぐ。

```shark
func largest<T: Comparable>(xs: list<T>) -> T? {
  if xs.len() == 0 { return none; }
  var best = xs[0];
  for var x in xs {
    if x.compare(best) > 0 { best = x; }
  }
  return best;
}
```

制約を書くと、その型が持つと約束されたメソッドを `T` に対して呼べる
（[types/generics.md](types/generics.md)）。

## 書式指定

`f"..."` の `{ }` の中は、`式` か `式:書式` と書く。書式は Python に倣う。

```
{式:[埋め文字][寄せ][0][幅][,][.桁数][種類]}
```

| 部分 | 書き方 | 意味 |
|---|---|---|
| 寄せ | `<` `>` `^` | 左・右・中央 |
| 埋め文字 | 寄せの前に1文字 | 余白を埋める文字（既定は空白） |
| ゼロ埋め | `0` | 数の左を 0 で埋める |
| 幅 | 数字 | 最小の幅。表示幅で数える（全角は 2） |
| 桁区切り | `,` | 3桁ごとに区切る |
| 桁数 | `.` と数字 | 小数点以下の桁数 |
| 種類 | `d` `f` `e` `b` `o` `x` `X` `%` | 10進・小数・指数・2進・8進・16進・百分率 |

```shark
var n = 42;
var pi = 3.14159;

print(f"{n:5}");        // "   42"
print(f"{n:<5}|");      // "42   |"
print(f"{n:^7}|");      // "  42   |"
print(f"{n:05}");       // "00042"
print(f"{n:x}");        // "2a"
print(f"{n:b}");        // "101010"
print(f"{1234567:,}");  // "1,234,567"
print(f"{pi:.2f}");     // "3.14"
print(f"{0.125:.1%}");  // "12.5%"
print(f"{name:*<10}");  // "さめ******"
```

- `{` と `}` そのものを書きたいときは `{{` `}}`
- 種類を書かなければ、値の型に合わせて 10 進か文字列として出す
- 日付は書式指定では扱わない。`time` の `format()` を使う
  （[library/time.md](library/time.md)）

## 式の優先順位

上ほど強く結び付く。

| 順位 | 演算子 | 結合 |
|---|---|---|
| 1 | `a.b` `a?.b` `a(...)` `a[...]` `a!` | 左 |
| 2 | `a ** b`（冪乗） | 右 |
| 3 | `!a` `-a` `~a` | 右 |
| 4 | `a * b` `a / b` `a % b` | 左 |
| 5 | `a + b` `a - b` | 左 |
| 6 | `a << b` `a >> b` | 左 |
| 7 | `a < b` `a <= b` `a > b` `a >= b` | 左 |
| 8 | `a == b` `a != b` | 左 |
| 9 | `a & b` | 左 |
| 10 | `a ^ b` | 左 |
| 11 | `a \| b` | 左 |
| 12 | `a && b` | 左 |
| 13 | `a \|\| b` | 左 |
| 14 | `a ?? b`（値なしのときの既定値） | 右 |

ビット演算と冪乗の強さは **C と同じ**にしてある。C を知っていれば読み替えが要らない。
`**` だけは C に無いので、`-2 ** 2` が `-4`（単項より強い）、
`2 ** 3 ** 2` が `2 ** 9`（右結合）と決めた。

`&&` と `||` は左辺で結果が決まれば右辺を評価しない。

`a?.b` は `a` が `none` なら `b` を呼ばずに `none` を返す。結果は必ず `T?` になる。
`a!` は `none` のとき止まる（[types/optional.md](types/optional.md)）。

```shark
var name = find_fish(id)?.name() ?? "名無し";   // find_fish は Fish? を返す
```

### ビット演算

`int`（64 ビットの2の補数）にだけ使える。結果は **C と同じ**。

| 演算子 | 意味 |
|---|---|
| `a & b` `a \| b` `a ^ b` | 論理積・論理和・排他的論理和 |
| `~a` | 各ビットの反転 |
| `a << b` | 左へずらす。はみ出した桁は落とす（あふれを見ない） |
| `a >> b` | 右へずらす。符号を保つ（算術シフト） |

```shark
print(12 & 10);        // 8
print(1 << 10);        // 1024
print(-16 >> 2);       // -4
print(f"{5 << 2:b}");  // 10100
```

- 書き換えながら使う `&=` `\|=` `^=` `<<=` `>>=` もある
- ずらす幅は `0` 〜 `63`。外を書くと**実行時エラー**にする。
  C では未定義だが、未定義のまま動かすより止める方がよい
- `bool` には使えない。真偽値をまとめるのは `&&` と `\|\|`
- `&` `^` `\|` は比較より弱い（C と同じ）。
  `a & b == c` は `a & (b == c)` と読まれるので、`(a & b) == c` と括弧を書く。
  型が合わないので、そのままではエラーになる（E0131）

### 冪乗

| 書き方 | 結果 |
|---|---|
| `int ** int` | `int`。`0 ** 0` は `1` |
| `float ** float` | `float` |

```shark
print(2 ** 10);        // 1024
print(2.0 ** 0.5);     // 1.4142135623730951
```

- 指数が負の `int ** int` は実行時エラー。小数が要るなら `float(2) ** -1.0` と書く
- あふれたときも実行時エラー（`*` と同じ）
- `int` と `float` は混ぜられない（[types/primitive.md](types/primitive.md)）

## 文法の概略

```
program     := import* (declaration | statement)*
declaration := funcDecl | classDecl | varDecl | constDecl
funcDecl    := "public"? "func" IDENT genericParams? "(" params? ")" ("->" type)? block
classDecl   := "public"? "class" IDENT genericParams? (":" IDENT ("," IDENT)*)? classBody
genericParams := "<" genericParam ("," genericParam)* ">"
genericParam  := IDENT (":" IDENT ("+" IDENT)*)?
classBody   := "{" member* "}"
member      := "public"? "private"? (fieldDecl | methodDecl)
fieldDecl   := "var" IDENT ":" type ";"
methodDecl  := ("virtual" | "override")? "func" IDENT "(" params? ")" ("->" type)?
               (block | ";")
params      := param ("," param)*
param       := "ref"? IDENT ":" type
block       := "{" statement* "}"
statement   := varDecl | assign | exprStmt | ifStmt | forStmt | whileStmt
             | "break" ";" | "continue" ";" | "return" expr? ";"
ifStmt      := "if" cond block ("else" "if" cond block)* ("else" bind? block)?
whileStmt   := "while" cond block
forStmt     := "for" "var" IDENT "in" expr block
cond        := expr | bind "=" expr
bind        := "var" IDENT
type        := IDENT ("<" type ("," type)* ">")? "?"?
             | "func" "(" (type ("," type)*)? ")" ("->" type)?
```

## 予約語

```
func return var const if else while for in break continue
class this super This public private virtual override ref import as
task parallel try panic
true false none
int float bool string bytes void list map
channel Task Result Error Range
```

型名も予約語とし、ユーザーが同名の型や変数を作ることはできない。

名前が要るところ（変数・関数・クラス・引数・型注釈など）に予約語を書くと E0003 になる。
`.` の後ろは名前の別の並びなので、`json.none()` のように予約語と同じ綴りでもよい。
