# ジェネリクス

`list<T>` や `map<K, V>` のように、型を後から決められる仕組み。
ユーザーも定義できる。

## 関数

```shark
func first<T>(xs: list<T>) -> T? {
  if xs.len() == 0 { return none; }
  return xs[0];
}

var a = first([1, 2, 3]);       // int?
var b = first(["x", "y"]);      // string?
```

型引数は呼び出しの引数から決まる。決まらないときは明示する。

```shark
var e = empty<int>();
```

## クラス

```shark
class Box<T> {
  var value: T;

  func init(value: T) { this.value = value; }
  func get() -> T { return this.value; }
}

var b = Box<int>(1);
```

## 制約

型引数の後ろに `: インタフェース名` を書くと、それを実装した型だけを受け取れる。
制約を書けば、そのインタフェースが約束するメソッドを `T` に対して呼べる。

```shark
func max<T: Comparable>(a: T, b: T) -> T {
  if a.compare(b) > 0 { return a; }
  return b;
}

print(max(1, 2));                      // int は Comparable
print(max(Fish("さめ"), Fish("ふぐ")));  // Fish が Comparable を実装していれば
```

複数要るときは `+` でつなぐ。

```shark
func show_all<T: Comparable + Nameable>(xs: list<T>) -> void { }   // Nameable は自分で定義したもの
```

制約の無い `T` に対しては、代入とコピーしかできない。

```shark
func max<T>(a: T, b: T) -> T {
  if a.compare(b) > 0 { return a; }   // エラー: T が Comparable とは限らない
  return b;
}
```

比較の仕方をその場で渡す書き方も残す。インタフェースを実装していない型にはこちらを使う。

```shark
func max_by<T>(a: T, b: T, greater: func(T, T) -> bool) -> T {
  if greater(a, b) { return a; }
  return b;
}
```

インタフェースの一覧と、基本型の扱いは [class.md](class.md)。

## 実行のしかた

型検査はコンパイル時に行い、実行時は型ごとにコードを複製せず、同じコードを共有する。
値の大きさは実行時に分かるため、型ごとの機械語を用意する必要がない。
これはインタプリタの実装を小さく保つための選択（[../runtime/execution.md](../runtime/execution.md)）。
