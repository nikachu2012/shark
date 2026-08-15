# std.math

数の計算と乱数。

```shark
import std.math;
```

## 定数

| 名前 | 値 |
|---|---|
| `math.PI` | 円周率 |
| `math.E` | 自然対数の底 |
| `math.INF` | 無限大 |

## int と float の両方に使えるもの

同じ名前で2つずつ定義してある（オーバーロード。[../syntax.md](../syntax.md)）。

| 関数 | 説明 |
|---|---|
| `math.abs(x: int) -> int` / `math.abs(x: float) -> float` | 絶対値 |
| `math.min(a: int, b: int)` / `math.min(a: float, b: float)` | 小さい方 |
| `math.max(a: int, b: int)` / `math.max(a: float, b: float)` | 大きい方 |
| `math.clamp(x, lo, hi)` | 範囲に収める。`int` 版と `float` 版 |

`int` と `float` を混ぜて渡すことはできない。どちらかに変換してから呼ぶ。

```shark
print(math.abs(-3));           // 3
print(math.max(1.5, 2.5));     // 2.5
print(math.clamp(120, 0, 100));// 100
```

## 小数の計算

引数も戻り値も `float`。`int` を渡すときは `float()` で変換する。

| 関数 | 説明 |
|---|---|
| `math.sqrt(x: float) -> float` | 平方根 |
| `math.pow(x: float, y: float) -> float` | べき乗 |
| `math.exp(x: float)` `math.log(x: float)` `math.log10(x: float)` | 指数と対数 |
| `math.sin(x: float)` `math.cos(x: float)` `math.tan(x: float)` | 三角関数（弧度法） |
| `math.asin(x)` `math.acos(x)` `math.atan(x)` | 逆三角関数 |
| `math.atan2(y: float, x: float) -> float` | 座標から角度 |

```shark
var r = math.sqrt(float(2));        // 1.4142...
var deg = math.atan2(1.0, 1.0) * 180.0 / math.PI;   // 45.0
```

## 丸める

| 関数 | 説明 |
|---|---|
| `math.floor(x: float) -> int` | 小さい方へ |
| `math.ceil(x: float) -> int` | 大きい方へ |
| `math.round(x: float) -> int` | 四捨五入 |
| `math.trunc(x: float) -> int` | 0 の方へ |

```shark
print(math.floor(-1.5));   // -2
print(math.trunc(-1.5));   // -1
print(math.round(2.5));    // 3
```

## 調べる

| 関数 | 説明 |
|---|---|
| `math.is_nan(x: float) -> bool` | 数でないか |
| `math.is_inf(x: float) -> bool` | 無限大か |

## 乱数

| 関数 | 説明 |
|---|---|
| `math.random() -> float` | 0 以上 1 未満 |
| `math.random_int(lo: int, hi: int) -> int` | `lo` 以上 `hi` 以下 |
| `math.seed(n: int) -> void` | 種を固定する |

```shark
math.seed(42);                      // 同じ種なら同じ並びになる
print(math.random_int(1, 6));       // さいころ
```

乱数の作り方は処理系に埋め込む。同じ種を与えれば、どの環境でも同じ並びになる。
暗号には使えない。
