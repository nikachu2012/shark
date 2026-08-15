# コレクション型

「文字列・バイト列・可変長配列・Key-value を相互に、簡単に持てるようにする」を担当する型。
どれも中身が変わる型で、代入時にはコピーされる（[../runtime/memory.md](../runtime/memory.md)）。

| 型 | 中身 |
|---|---|
| `string` | Unicode の文字列 |
| `bytes` | バイトの並び |
| `list<T>` | 同じ型 `T` を並べた可変長配列 |
| `map<K, V>` | キー `K` から値 `V` を引く表 |

## string

内部表現は UTF-8。ただし添字も長さも**文字（Unicode コードポイント）単位**で数える。

```shark
var s = "さめ🦈";
s.len();          // 3
s[0];             // "さ"
s.bytes().len();  // 10
```

バイト単位で触りたいときは `bytes` に変換する。両者を混同させないため、`string` に
バイト添字は用意しない。

| メソッド | 説明 |
|---|---|
| `len()` | 文字数 |
| `sub(start, end)` | 部分文字列 |
| `find(s)` | 見つかった位置。無ければ `none` |
| `contains(s)` `starts_with(s)` `ends_with(s)` | 判定 |
| `split(sep)` | `list<string>` に分ける |
| `trim()` `upper()` `lower()` | 整形 |
| `replace(from, to)` | 置換 |
| `bytes()` | `bytes` に変換 |

## bytes

長さの変えられるバイト列。添字はバイト単位。

```shark
var b = b"\x01\x02";
b.push(0x03);
b.len();          // 3
b.to_string();    // UTF-8 として解釈。不正なら none
```

## list&lt;T&gt;

```shark
var xs: list<int> = [1, 2, 3];
xs.push(4);
xs.pop();          // 末尾を取り出す。空なら none
xs[0] = 10;
xs.len();
xs.insert(0, 9);
xs.remove(0);
xs.contains(3);
xs.sort();         // 要素が Comparable のときだけ呼べる
for var x in xs { print(x); }
```

要素の型は全て同じでなければならない。`[1, "a"]` はエラー。

## map&lt;K, V&gt;

```shark
var m: map<string, int> = {"a": 1};
m["b"] = 2;
m["a"];            // 1
m.get("z");        // none（存在しないときは none を返す）
m.has("a");        // true
m.remove("a");
m.len();
for var k in m.keys() { print(m[k]); }
```

- 存在しないキーを `m["z"]` で読むと実行時エラー。`get()` を使えば `none` が返る
- キーに使えるのは `int` `float` `bool` `string` `bytes` のみ
- 反復の順番は**挿入した順**。プラットフォームによって順番が変わらないようにする

## 空のリテラル

空の `[]` や `{}` だけでは要素の型が決まらないので、型注釈が必要。

```shark
var xs = [];                 // エラー: 型が決まらない
var xs: list<int> = [];      // OK
```
