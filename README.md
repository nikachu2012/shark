# Shark🦈

**ゲーム機で動く、プログラミング学習用ゲームの中で使う言語。**
外部依存が少なく、移植しやすいことを第一にしています。

```shark
print("Hello, Shark!");
```

- 初学者にわかりやすく、それでいて高度なことも書ける。覚える順番があり、上級機能は書くまで邪魔をしない
- すべての代入はコピー。共有したいときだけ `ref` と書く
- `null` も例外も `async` も持たない。値なし・失敗・同時実行は別の道具で表す
- 型に厳しいが、エラーは必ず直し方まで示す
- 速くする仕組みが無い環境でも動く。標準ライブラリだけで一通り書ける
- 実装は**実行系（コア）だけ**。ゲーム本体に組み込んで使い、無限ループを書かれても固まらない
  ように少しずつ実行できる。`shark` コマンドはその外側に別で作る
- コアは C++ の**雛形**。そのまま使えるところまで作り込んで git で配り、
  手を入れるのは移植層とホスト関数の2か所だけ

## 動かす

```
make                              # コアと shark コマンドを作る（外部依存なし）
./shark run examples/hello.shk    # Hello, Shark!
make test                         # tests/ を走らせる
make embed && ./examples/embed/game   # ゲームに組み込む例
```

```
./shark run <file.shk>     実行する
./shark check <file.shk>   型検査だけ
./shark test [file.shk]    test_ で始まる関数を走らせる（省くと *_test.shk 全部）
./shark explain E0102      エラーの詳しい説明
./shark modules            この処理系が持つモジュールの一覧

  --memory <MB>            使ってよいメモリの量。超えたら実行時エラー（既定 64）
  --lang ja|en / --strict  診断の言語 / 警告をエラーとして扱う
```

## 速さ

同じ内容を C・Python・Shark で書いて測ったもの。**Python と同じくらい**で、
最適化した C とは 5〜60 倍の差がある。

| 内容 | C (-O2) | Python 3.14 | Shark | Shark ÷ C | Shark ÷ Python |
|---|---|---|---|---|---|
| 整数のループ 1000万回（`sum += i % 7`） | 6 ms | 429 ms | **331 ms** | 59 倍 | 0.77 倍 |
| 再帰呼び出し `fib(32)`（436万回の呼び出し） | 6 ms | 151 ms | **312 ms** | 55 倍 | 2.06 倍 |
| 可変長配列に 100万件足して合計（5回くり返す） | 7 ms | 353 ms | **224 ms** | 34 倍 | 0.63 倍 |
| key-value に 50万件入れて、50万回引く | 7 ms | 73 ms | **50 ms** | 7 倍 | 0.68 倍 |
| 書式付きの文字列を 100万個作る | 34 ms | 121 ms | **174 ms** | 5 倍 | 1.44 倍 |
| （下敷き）起動して 0 を出すだけ | 2 ms | 10 ms | 2 ms | — | — |

- ループ・配列・key-value は Python より速く、**関数呼び出しと文字列づくりは Python より遅い**
- Shark は仮想マシンだけで動かした結果。実行時コンパイル（JIT）は仕様でも任意機能で、まだ入れていない
- 測り方: 3つの言語に同じアルゴリズムを書き、**出力が一致することを確かめてから**、
  各3回走らせていちばん速かった回を採る。プロセスの起動時間も含む（最下行がその下敷き）
- 環境: macOS 26 (arm64) / Apple clang 21 `-O2` / CPython 3.14.3 / Shark 0.1.0

```
python3 bench/run.py            # 全部測り直す
python3 bench/run.py loop fib   # 選んで測る
```

## 読むところ

| | |
|---|---|
| [docs/reference.md](docs/reference.md) | 言語の使い方（利用者向け・全17章） |
| [docs/implementation.md](docs/implementation.md) | 実装メモ（作った範囲・組み込み方・移植の手順） |
| [spec/README.md](spec/README.md) | 思想と仕様書の索引 |
| [spec/open-questions.md](spec/open-questions.md) | まだ決めていないこと |

## 構成

```
core/     実行系（コア）。C++。ファイルも端末も触らない
  platform/   移植層          ← 機種に合わせて差し替える場所
  lib/        標準ライブラリ
frontend/ shark コマンド（コアとは別実装）
examples/ サンプル。embed/ はゲームに組み込む例
tests/    テスト（make test）
bench/    C・Python・Shark の速さ比べ（python3 bench/run.py）
docs/     利用者向けのリファレンスと実装メモ
spec/     仕様
  types/      型システム
  runtime/    実行系（コア）の内部と、ホストとの境界
  library/    標準ライブラリ
  skeleton.md コアの雛形。何を入れ、どこを書き換えるか
  frontend.md コマンドライン実装（コアとは別に作る）
```
