# 値がない状態

Shark に `null` はない。「値がないかもしれない」ことは型に書く。

## `T?`

型の後ろに `?` を付けると、「`T` の値」か「値なし（`none`）」のどちらかを表す。

```shark
var a: int = 1;      // 必ず値がある
var b: int? = none;  // 値がないこともある
```

`int` に `none` は入らない。`int?` を `int` として使うこともできない。
うっかり値なしを触ってしまう事故は、実行時ではなくコンパイル時に止まる。

エラーには下の3つの取り出し方を並べて示す（[../runtime/diagnostics.md](../runtime/diagnostics.md) E0201）。

```shark
var b: int? = 1;
var c: int = b;      // エラー: int? は int ではない
print(b + 1);        // エラー: int? に + は使えない
```

## 取り出し方

### 1. 既定値を決める `??`

```shark
var n = int("abc") ?? 0;    // 変換できなければ 0
```

### 2. 中身がある時だけ処理する `if var`

```shark
if var n = maybe_num {
  print(n);        // このブロックの中では n は int
} else {
  print("値なし");
}
```

### 3. 無いはずがない時に断言する `!`

```shark
var n = maybe_num!;   // none だったら実行時エラーで止まる
```

`!` は「絶対にあると分かっている」場所だけで使う。

## どこで `T?` が出てくるか

失敗しうる操作は、エラーではなく `none` を返す。

```shark
m.get("z");        // map に無いキー
xs.pop();          // 空のリスト
s.find("a");       // 見つからない
int("abc");        // 変換できない
```

「失敗したが理由はひとつしかない」場合は `T?`、
「失敗の理由を伝えたい」場合は `Result<T>` を使う（[../runtime/error.md](../runtime/error.md)）。

## クラスのメンバ

`T?` のメンバの既定値は `none`。

```shark
class Node {
  var next: Node?;    // 既定は none
}
```

なお、代入がコピーである以上、`Node` が `Node?` を持っても循環はできない
（コピーされた別の実体になる）。詳しくは [../runtime/memory.md](../runtime/memory.md)。
