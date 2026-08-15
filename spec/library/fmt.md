# std.fmt

書式指定（`f"{v:...}"`）で書けないものを補う。

```shark
import std.fmt;
```

数の桁揃え、ゼロ埋め、桁区切り、2進・16進、百分率は**書式指定で書ける**
（[../syntax.md](../syntax.md)）。このモジュールを使わなくてよい。

```shark
print(f"{n:05}");        // ゼロ埋め
print(f"{n:x}");         // 16進
print(f"{1234567:,}");   // 桁区切り
print(f"{pi:.2f}");      // 小数2桁
```

## 実行時に書式を決める

書式そのものを変数で持ちたいときに使う。

| 関数 | 説明 |
|---|---|
| `fmt.apply(v: int, spec: string) -> string` | 書式指定と同じ記法を実行時に適用する |
| `fmt.apply(v: float, spec: string) -> string` | 同上 |
| `fmt.apply(v: string, spec: string) -> string` | 同上 |

```shark
var spec = user_setting ?? ".2f";
print(fmt.apply(pi, spec));      // "3.14"
```

書式が正しくなければ、そのまま値を文字列にして返す。止まらない。

## 書式指定に無いもの

| 関数 | 説明 |
|---|---|
| `fmt.bytes(v: int) -> string` | データ量を読みやすく |
| `fmt.duration(d: Duration) -> string` | 期間を読みやすく |
| `fmt.truncate(s: string, width: int) -> string` | 幅で切って `…` を付ける |
| `fmt.table(rows: list<list<string>>) -> string` | 列の幅を揃えて表にする |

```shark
print(fmt.bytes(1536));                    // "1.5 KB"
print(fmt.duration(time.seconds(3725.0))); // "1時間2分5秒"
print(fmt.truncate("とても長い名前です", 8)); // "とても長…"

print(fmt.table([
  ["名前", "大きさ"],
  ["さめ", "400"],
  ["ふぐ", "20"],
]));
```

幅は表示幅で数える（全角は 2）。等幅で並べたときに桁が揃う（[text.md](text.md)）。
