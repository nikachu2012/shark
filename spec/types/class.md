# クラス

C++ 的なオブジェクト指向。ただしインスタンスも値であり、代入するとコピーされる。

## 定義

```shark
class Fish {
  var name: string;
  var size: int;

  func init(name: string, size: int) {
    this.name = name;
    this.size = size;
  }

  func describe() -> string {
    return f"{this.name} ({this.size}cm)";
  }
}

var f = Fish("サメ", 400);
print(f.describe());
```

- メンバ変数には型注釈が必須
- `init` はコンストラクタ。無い場合は、全メンバが既定値のインスタンスが作られる
- メンバへは `this.` を付けて触る。省略はできない（局所変数との取り違えを防ぐ）

## 可視性

既定は `private`（クラスの中からだけ触れる）。外から使わせたいものに `public` を付ける。

```shark
class Counter {
  var count: int;                                  // private
  public func value() -> int { return this.count; }
}
```

## 継承

実装を持つ親は**1つまで**。

```shark
class Shark : Fish {
  var teeth: int;

  func init(name: string, teeth: int) {
    super.init(name, 400);      // 親のコンストラクタ
    this.teeth = teeth;
  }
}
```

- `Shark` は `Fish` として扱える
- 親の `public` なメンバとメソッドは、そのまま使える

## virtual と override

**上書きできるのは `virtual` を付けたメソッドだけ。**

```shark
class Fish {
  var name: string;

  virtual func describe() -> string {      // 上書きしてよい
    return this.name;
  }

  func id() -> int { return 1; }           // 上書きできない
}

class Shark : Fish {
  override func describe() -> string {     // override は必須
    return f"🦈 {super.describe()}";
  }
}
```

| 書き方 | 意味 |
|---|---|
| `virtual func f()` | 子が上書きしてよい |
| `override func f()` | 親の `virtual` を上書きする |
| `func f()` | 上書きできない |

規則。

- 親に `virtual` が無いメソッドと同じ名前を子で定義すると**エラー**
- `override` を書かずに上書きしようとすると**エラー**
- `override` と書いたのに親に該当するメソッドが無い場合も**エラー**
- 上書きするメソッドは、引数と戻り値の型が親と同じでなければならない

`override` を必須にするのは、名前を書き間違えたときに黙って別のメソッドが増えるのを防ぐため。

### どちらが呼ばれるか

- `virtual` なメソッドは、**入っている実体**の型で決まる
- `virtual` でないメソッドは、**変数の型**で決まる

```shark
var f: Fish = Shark("さめ");
print(f.describe());     // Shark の方（virtual なので）
print(f.id());           // Fish の方（virtual でないので）
```

## 純粋仮想と抽象クラス

本体を書かない `virtual` メソッドを**純粋仮想**と呼ぶ（C++ の `= 0` にあたる）。

```shark
class Shape {
  virtual func area() -> float;          // 本体なし
  virtual func name() -> string;
}
```

純粋仮想を1つでも持つクラスは**抽象クラス**になり、インスタンスを作れない。

```shark
var s = Shape();      // エラー: 抽象クラス
```

子がすべての純粋仮想を `override` すれば、インスタンスを作れる。

```shark
class Circle : Shape {
  var r: float;
  func init(r: float) { this.r = r; }

  override func area() -> float { return math.PI * this.r * this.r; }
  override func name() -> string { return "円"; }
}

var c: Shape = Circle(2.0);
print(c.area());      // 12.56...
```

## インタフェース

**メンバ変数を持たず、純粋仮想だけを並べたクラス**をインタフェースと呼ぶ。
言語としては普通のクラスで、専用の書き方は無い。

```shark
class Swimmer {
  virtual func swim() -> void;
  virtual func speed() -> float;
}

class Nameable {
  virtual func label() -> string;
}
```

継承の2番目以降には、**インタフェースだけ**を並べられる。いくつでもよい。

```shark
class Shark : Fish, Swimmer, Nameable {
  override func swim() -> void { print("すいすい"); }
  override func speed() -> float { return 8.0; }
  override func label() -> string { return this.name; }
}
```

- 1番目が実装を持つ親（省略してよい）
- 2番目以降はインタフェース
- 実装を持つ親を2つ以上書くことはできない（菱形継承を避けるため）

インタフェース型の変数として扱える。

```shark
func race(a: Swimmer, b: Swimmer) -> string {
  if a.speed() > b.speed() { return "先"; }
  return "後";
}

print(race(Shark("さめ"), Dolphin("いるか")));
```

未実装の純粋仮想が残っていれば、そのクラスも抽象クラスのままになる。

### This

インタフェースの中でだけ、`This` と書ける。**実装したクラス自身**を指す。

```shark
class Comparable {
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

`This` が無いと、比較の相手を `Comparable` として受け取ることになり、
中身に触るために型を戻す操作が要る。`This` はそれを避けるためだけの仕組みで、
インタフェースの外では書けない。

### 標準のインタフェース

処理系が用意しているもの。

| 名前 | 約束するもの |
|---|---|
| `Comparable` | `compare(other: This) -> int`（`-1` `0` `1`） |

`int` `float` `string` `bytes` `bool` は、処理系が `Comparable` を実装済みとして扱う。
そのため `list<int>` も `list<Fish>` も、同じ `sort()` で並べ替えられる
（[collection.md](collection.md)）。

**ユーザーが基本型に後からインタフェースを実装することはできない。**
できるようにすると、同じ型が場所によって違う振る舞いをしうるため。

## 値であること

インスタンスは代入・引数渡しでコピーされる。

```shark
var a = Fish("さめ", 400);
var b = a;
b.name = "ふぐ";
print(a.name);      // "さめ"（変わらない）
```

同じ実体を触りたいときは `ref` を使う。

```shark
func rename(ref f: Fish) { f.name = "しゃち"; }
rename(ref a);
```

### コピーしても実体の型は変わらない

親やインタフェースの型の変数に入れても、**中身は子のまま**コピーされる。
C++ のように親の部分だけが切り取られること（スライシング）は起きない。

```shark
var f: Fish = Shark("さめ");
var g = f;                 // Shark としてコピーされる
print(g.describe());       // Shark の方が呼ばれる
```

実体には「どのクラスか」が記録されており、コピーのときも一緒に複製されるため
（[../runtime/memory.md](../runtime/memory.md)）。

## 等値比較

`==` は次の順で判定する。

1. 実体の型が違えば等しくない
2. 同じなら、メンバを順に比べる

別の判定にしたいときは `equals()` を `virtual` として定義する。

## メソッドのオーバーロード

引数の型か個数が違えば、同じ名前のメソッドを定義できる（[../syntax.md](../syntax.md)）。
コンストラクタも同じ。

```shark
class Fish {
  func init(name: string) { this.init(name, 0); }
  func init(name: string, size: int) {
    this.name = name;
    this.size = size;
  }
}
```

`virtual` と `override` は、**引数まで含めて**対応を見る。

```shark
class Fish {
  virtual func grow(n: int) -> void { }
  virtual func grow(n: float) -> void { }
}

class Shark : Fish {
  override func grow(n: int) -> void { }     // int の方だけ上書き
}
```

親のオーバーロードのどれとも引数が一致しない `override` はエラーになる。

## 持たないもの

- 多重継承（実装を持つ親は1つまで）
- 演算子の多重定義
- 静的メソッド

## 実装のしかた

- `virtual` を持つクラスは、クラスごとにメソッド表を1つ持つ。実体はその表を指す
- 表はクラスに1つなので、インスタンスをコピーしても増えない。コピーの重さは変わらない
- `virtual` を1つも持たないクラスは表を持たない。呼び出しはすべて静的に決まる
