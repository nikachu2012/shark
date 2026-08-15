# std.test

テストの書き方と実行。

```shark
import std.test;
```

## 書き方

`test_` で始まる `public` 関数がテストになる。戻り値は書かない。

```shark
// fish_test.shk
import std.test;
import ./fish;

public func test_name_is_set() {
  var f = fish.Fish("さめ", 400);
  test.eq(f.name(), "さめ");
}

public func test_size_is_never_negative() {
  test.describe("負の大きさを渡したら 0 に直す");
  var f = fish.Fish("さめ", -1);
  test.eq(f.size(), 0);
}
```

識別子に日本語は使えないので、テスト名は英数字で書く（[../syntax.md](../syntax.md)）。
日本語の説明を付けたいときは `test.describe()` を使う。

## 確かめる

| 関数 | 説明 |
|---|---|
| `test.ok(cond: bool) -> void` | 真であること |
| `test.eq<T>(actual: T, expected: T) -> void` | 等しいこと |
| `test.ne<T>(a: T, b: T) -> void` | 等しくないこと |
| `test.near(a: float, b: float, tolerance: float) -> void` | 小数がほぼ等しいこと |
| `test.is_error<T>(r: Result<T>) -> void` | 失敗していること |
| `test.is_none<T>(v: T?) -> void` | 値がないこと |
| `test.fail(msg: string) -> void` | その場で失敗させる |
| `test.describe(s: string) -> void` | このテストの説明 |

失敗しても `panic` はせず、そのテストだけを失敗として記録し、次のテストへ進む。

```shark
test.eq(add(1, 2), 3);
test.near(math.sqrt(2.0), 1.4142, 0.001);
test.is_error(file.read("ない.txt"));
```

## 前後の処理

| 関数 | 説明 |
|---|---|
| `test.before_each(f: func())` | 各テストの前 |
| `test.after_each(f: func())` | 各テストの後 |

## 実行

テストを集めて走らせるのはフロントエンドの役目（[../frontend.md](../frontend.md)）。
コアは1件ずつ実行して結果を返すだけ。

```
shark test              # いまの場所以下の *_test.shk を全部
shark test fish_test.shk
shark test --filter 名前
```

出力の例。

```
fish_test.shk
  ok    test_name_is_set
  fail  test_size_is_never_negative
        負の大きさを渡したら 0 に直す
        fish_test.shk:13  expected 0, actual -1

2 件中 1 件成功
```

テストは**1件ずつ順に**走らせる。同時には走らせない。
順番による失敗が起きず、出力が混ざらないため。
