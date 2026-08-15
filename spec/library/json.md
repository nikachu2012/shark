# std.json

JSON の読み書き。任意モジュール（[overview.md](overview.md)）。

```shark
import std.json;
```

## 読み出しは短く書けるようにする

型は厳格だが、JSON の読み出しはスクリプト言語のように短く書けることを優先する。
そのために `Json` は「無い」も表せる値にしてある。

```shark
var root = try json.parse(body);

print(root["items"][0]["name"].string() ?? "名無し");
```

- 添字は**常に `Json` を返す**。無いキーや範囲外でも止まらず、「無い」を表す `Json` が返る
- 途中が無くても、そのままつなげて書ける。`?.` を挟む必要がない
- 最後に `string()` `int()` などで取り出したときに、初めて `T?` になる

型が違うときも `none` になる。取り違えても落ちない。

## Json

| メソッド | 説明 |
|---|---|
| `j[key: string] -> Json` | 表から引く。無ければ「無い」を表す `Json` |
| `j[i: int] -> Json` | 配列から引く。範囲外でも同じ |
| `j.string() -> string?` | 文字列として取り出す |
| `j.int() -> int?` `j.float() -> float?` | 数として取り出す |
| `j.bool() -> bool?` | 真偽値として取り出す |
| `j.list() -> list<Json>` | 配列として取り出す。配列でなければ空 |
| `j.keys() -> list<string>` | 表のキー。表でなければ空 |
| `j.exists() -> bool` | 値があるか |
| `j.kind() -> string` | `"none"` `"bool"` `"number"` `"string"` `"list"` `"map"` |

```shark
for var item in root["items"].list() {
  var name = item["name"].string() ?? "";
  var size = item["size"].int() ?? 0;
  print(f"{name}: {size}");
}

if root["config"]["debug"].bool() ?? false {
  print("デバッグ中");
}
```

## 読む

| 関数 | 説明 |
|---|---|
| `json.parse(s: string) -> Result<Json>` | 文字列から |
| `json.parse_file(p: string) -> Result<Json>` | ファイルから |

読み込みそのものの失敗（壊れた JSON）は `Result` で返る。
中身が期待と違うことは失敗にしない。`none` として扱う。

## 書く

| 関数 | 説明 |
|---|---|
| `json.stringify(v: Json) -> string` | 1行で |
| `json.stringify_pretty(v: Json, indent: int) -> string` | 字下げ付きで |
| `json.of(v: int) -> Json` | `int` から作る |
| `json.of(v: float)` `json.of(v: string)` `json.of(v: bool)` | 同上（オーバーロード） |
| `json.of(v: list<Json>)` `json.of(v: map<string, Json>)` | 同上 |
| `json.none() -> Json` | 「無い」を表す値 |

```shark
var obj = json.of({
  "name": json.of("さめ"),
  "size": json.of(400),
});

print(json.stringify_pretty(obj, 2));
```

## クラスとの相互変換

**自動では変換しない。**`Json` から自分で詰め替える。

```shark
func fish_from(j: Json) -> Fish? {
  var name = j["name"].string();
  var size = j["size"].int();
  if name == none || size == none { return none; }
  return Fish(name!, size!);
}
```

自動変換にはメンバの名前と型を実行時に調べる仕組み（リフレクション）が要り、
型情報を実行時まで持ち越すことになる。組み込み向けには重すぎるため入れない。
読み出しを短く書けるようにすることで、詰め替えの手間を減らす方針をとる。
