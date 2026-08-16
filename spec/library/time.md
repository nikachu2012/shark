# std.time

時刻と期間。

```shark
import std.time;
```

## 型

| 型 | 説明 |
|---|---|
| `Time` | ある瞬間。内部は UTC |
| `Duration` | 時間の長さ |

時刻と期間を別の型にしているのは、`Time + Time` のような意味のない計算を型で防ぐため。

## 作る

| 関数 | 説明 |
|---|---|
| `time.now() -> Time` | 現在時刻 |
| `time.date(y: int, mo: int, d: int) -> Time?` | 日付から。ありえない日付なら `none` |
| `time.datetime(y, mo, d, h, mi, s: int) -> Time?` | 日時から |
| `time.parse(s: string, format: string) -> Time?` | 文字列から |
| `time.monotonic() -> Duration` | 起動からの経過。時刻合わせの影響を受けない |

```shark
var now = time.now();
var d = time.date(2026, 8, 15)!;
var t = time.parse("2026-08-15 12:00:00", "YYYY-MM-DD hh:mm:ss")!;
```

## Time のメソッド

| メソッド | 説明 |
|---|---|
| `format(f: string) -> string` | 書式に沿って文字列にする |
| `year() -> int` `month()` `day()` | 年・月・日 |
| `hour() -> int` `minute()` `second()` | 時・分・秒 |
| `weekday() -> int` | 0（日）〜 6（土） |
| `to_local() -> Time` `to_utc() -> Time` | 表示を地域時刻／UTC に切り替える |

書式に使える記号。

| 記号 | 意味 | 例 |
|---|---|---|
| `YYYY` `MM` `DD` | 年・月・日 | `2026-08-15` |
| `hh` `mm` `ss` | 時・分・秒 | `12:34:56` |
| `SSS` | ミリ秒 | `007` |

```shark
print(now.format("YYYY/MM/DD"));           // 2026/08/15
print(now.to_local().format("hh:mm"));     // 21:05
```

切り替えるのは**表示だけ**で、指している瞬間は動かない。
`t.to_local() - t` は 0、`t.to_local().compare(t)` も 0 になる。
何度呼んでも結果は変わらない（`t.to_local().to_local()` は地域時刻のまま）。

## Duration

| 関数・メソッド | 説明 |
|---|---|
| `time.seconds(v: float) -> Duration` | 秒から作る |
| `time.minutes(v: float)` `time.hours(v: float)` `time.days(v: float)` | 分・時・日から |
| `d.seconds() -> float` | 秒に直す |
| `d.minutes()` `d.hours()` `d.days()` | 分・時・日に直す |

## 計算

| 式 | 結果 |
|---|---|
| `Time - Time` | `Duration` |
| `Time + Duration` | `Time` |
| `Time - Duration` | `Time` |
| `Duration + Duration` | `Duration` |

```shark
var d = time.now() - time.date(2026, 1, 1)!;
print(f"{d.days()} 日経った");

var tomorrow = time.now() + time.days(1.0);
```

待つのは組み込みの `sleep`（[builtin.md](builtin.md)）。
