# std.text

Unicode の細かい扱いと正規表現。
`upper()` や `split()` のような日常的な操作は `string` のメソッドにある
（[../types/collection.md](../types/collection.md)）。

```shark
import std.text;
```

## Unicode

| 関数 | 説明 |
|---|---|
| `text.normalize(s: string, form: string) -> string` | 正規化。`"NFC"` `"NFD"` |
| `text.width(s: string) -> int` | 表示幅。全角は 2 と数える |
| `text.fold_case(s: string) -> string` | 大文字小文字を無視した比較用の形 |
| `text.code(c: string) -> int` | 1文字をコードポイントへ |
| `text.from_code(n: int) -> string?` | コードポイントから1文字へ |

```shark
print(text.width("さめ"));            // 4
print(text.normalize("が", "NFD").len());   // 2（か + 濁点）

if text.fold_case(a) == text.fold_case(b) { print("同じ"); }
```

## 文字の種類

| 関数 | 説明 |
|---|---|
| `text.is_digit(c: string) -> bool` | 数字か |
| `text.is_alpha(c: string) -> bool` | 文字か |
| `text.is_space(c: string) -> bool` | 空白か |
| `text.is_upper(c)` `text.is_lower(c)` | 大文字・小文字か |

```shark
for var c in s.chars() {
  if !text.is_digit(c) { return none; }
}
```

## 並べ替え

| 関数 | 説明 |
|---|---|
| `text.compare(a: string, b: string) -> int` | コードポイント順。`-1` `0` `1` |
| `text.compare_ja(a: string, b: string) -> int` | 五十音順 |
| `text.sort_ja(ref xs: list<string>) -> void` | 五十音順に並べ替える |

`xs.sort()` はコードポイント順。どの環境でも同じ結果になるようにするため。

### 五十音順

対応するのは**日本語だけ**。ほかの言語ごとの辞書順は持たない。
仮名の表だけなら小さく、埋め込んでも負担にならないため。

```shark
var xs = ["サメ", "あじ", "ぶり", "いわし"];
text.sort_ja(ref xs);
print(xs);      // あじ, いわし, サメ, ぶり
```

決めごと。

| 対象 | 扱い |
|---|---|
| ひらがなとカタカナ | 同じものとして扱う |
| 清音・濁音・半濁音 | `か` → `が`、`は` → `ば` → `ぱ` の順 |
| 小書き（`ぁ` `っ` `ゃ`） | 大書きの後 |
| 長音（`ー`） | 直前の母音として扱う |
| 数字・英字 | 仮名より前 |
| 漢字 | **対象外**。コードポイント順に置く |

漢字を五十音順に並べるには読みが要り、辞書を丸ごと埋め込むことになる。
「外部依存を持たない」方針と釣り合わないため、対象外とする。
読み仮名で並べたいときは、読みを別に持って `text.sort_ja` に渡す。

## 正規表現

| 関数・メソッド | 説明 |
|---|---|
| `text.regex(pattern: string) -> Result<Regex>` | 組み立てる |
| `re.find(s: string) -> Match?` | 最初の1つ |
| `re.find_all(s: string) -> list<Match>` | 全部 |
| `re.matches(s: string) -> bool` | 見つかるか |
| `re.replace(s: string, to: string) -> string` | 置き換える |
| `re.split(s: string) -> list<string>` | 区切る |
| `m.text() -> string` | 一致した部分 |
| `m.start() -> int` `m.end() -> int` | 位置（文字単位） |
| `m.group(i: int) -> string?` | `( )` で囲んだ部分 |

```shark
var re = try text.regex("([0-9]{4})-([0-9]{2})-([0-9]{2})");

if var m = re.find("期限は 2026-08-15 です") {
  print(m.group(1)!);      // 2026
  print(m.start());        // 4
}

print(re.replace("2026-08-15", "$1年"));   // 2026年
```

書ける記法は最小限にする（文字クラス、`* + ? { }`、`|`、`( )`、`^ $`）。
先読みや後方参照は持たない。処理系に埋め込むため小さく保つ。

## 文字コード

| 関数 | 説明 |
|---|---|
| `text.decode(b: bytes, enc: string) -> Result<string>` | バイト列から文字列へ |
| `text.encode(s: string, enc: string) -> Result<bytes>` | 文字列からバイト列へ |

必ず使えるのは `"utf-8"` だけ。それ以外（`"shift_jis"` など）は任意で、
対応していない処理系では失敗を返す。変換表が大きく、移植の負担になるため。
