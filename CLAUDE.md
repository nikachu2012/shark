# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## これは何か

Shark🦈 — ゲーム機で動くプログラミング学習用ゲームの中で使う言語の処理系。
ドキュメント・コメント・コミットメッセージはすべて日本語で書く
（コミットは `feat(lang):` `fix(lib):` のような形式 + 日本語の要約）。

## コマンド

```
make                # shark と sharkvm を作る（外部依存なし。FreeType だけ任意で自動検出）
make test           # tests/ を走らせる（sh tests/run.sh。memcheck / bytecheck / imecheck / uicheck も含む）
make docs           # stdlib/ の宣言から docs/reference/ に HTML を作り、実装と突き合わせる
make docs-check     # 宣言ファイルの例をぜんぶ本物の shark で動かす（python3 tools/runex.py）
make embed          # ゲームに組み込む例（examples/embed/game と play_stage）
make dist           # 配れる形に包む（dist/shark-<機種>.tar.gz。中身は tools/package.sh）
make web            # WebAssembly 版を web/dist/ に作る（Emscripten が要る）
make web-serve      # 作ってから http://localhost:8000/ に配る
make web-test       # 作ったものを node で確かめる
make bench          # C・Python・Shark の速さ比べ（python3 bench/run.py loop fib で絞れる）
```

FreeType を静的に繋いで（配るときの形で）作る:

```
sh tools/freetype_static.sh                    # 元から静的に作る（build/ に置く）
make $(sh tools/freetype_static.sh --flags)    # それを繋ぐ
```

Windows（Visual Studio）では make の代わりに:

```
tools\build_win.bat          # shark.exe と sharkvm.exe を作る（外部依存なし）
tools\build_win.bat freetype # FreeType を取ってきて静的に作る（日本語の字形。一度だけ）
tools\build_win.bat test     # 作ってから tests\ を走らせる（sh が要る）
tools\build_win.bat clean
```

中身は `tools/build_win.ps1`。ソースの一覧は Makefile の RT_SRC / FE_SRC が正で、
このスクリプトはその写しなので、**コアのソースを増減したら両方を直す**。
MSYS2 / MinGW の make なら Makefile がそのまま動く（`.exe` は Makefile が付ける）。

```
./shark run <file.shk>      # 実行（.shkc も渡せる）
./shark check <file.shk>    # 型検査だけ
./shark test [file.shk]     # test_ で始まる関数を走らせる（--filter 名前 で絞る）
./shark fmt <file.shk>      # 見た目を整える（-w で書き換え、--check で確かめるだけ）
./shark build <file.shk>    # 単一バイナリにする（--bytecode で .shkc だけ保存）
./sharkvm <file.shkc>       # 実行装置だけで動かす
./shark explain E0102       # エラー番号の詳しい説明
```

- テストを1件だけ走らせる: `SHARK_UI=off ./shark run --no-color tests/cases/01_basics.shk` の
  出力を同名の `.expected` と見比べる（`tests/run.sh` がやっているのと同じ）。
  テストを足すときも `.shk` + `.expected` の組を `tests/cases/`（正常系）か
  `tests/errors/`（診断）に置くだけでよい
- テスト中は `SHARK_UI=off`（窓を開かず、見えない面に描く）
- ヘッダ（`.h` / `.inc`）を直すと全 `.o` が作り直される。ビルドの指定
  （`FREETYPE=` など）を変えたときは `make clean && make`（Makefile は指定の変化を見ない）

## 押すたびの検査（.github/workflows）

`ci.yml`（4機種で作って試す）・`pages.yml`（ブラウザ版を Cloudflare Pages へ）・
`release.yml`（タグを押すと実行ファイルを配る）の3つ。
どれも FreeType を静的に作って繋ぐので、配るものは1つの実行ファイルで済む。
**包み方は tools/package.sh の1か所**で、CI も配るときも `make dist` も同じものを呼ぶ。
できたものは、実行ごとの Artifacts から落とせる。
Pages に載せるには `CLOUDFLARE_API_TOKEN` と `CLOUDFLARE_ACCOUNT_ID` が要る（README）。

## アーキテクチャ

実装は**実行系（コア）だけ**にまとめ、コマンドラインはその外側に別実装する、が大原則
（spec/README.md）。コアはゲームに組み込む部品であり、**自分からファイルを読まず、
標準出力にも書かない**。すべて呼ぶ側（ホスト）が渡す。

```
core/       実行系。字句解析→構文解析→型検査→コード生成→仮想マシン。C++17、
            -fno-exceptions -fno-rtti、外部ライブラリなし（FreeType のみ任意）
  platform/   移植層（desktop / console / web）← 機種ごとに差し替える場所
  lib/        標準ライブラリの実装（*.cpp）。ホスト関数は Engine::register_host() に足す
frontend/   shark コマンド（main.cpp）と sharkvm（vm_main.cpp）。コアを呼ぶ参考実装
web/        同じコアを WebAssembly にした一式（移植層は core/platform/web.cpp）。
            プレイグラウンドのお手本は web/examples.py の ITEMS が正で、
            examples/ と食い違うと make web が止まる
stdlib/     標準ライブラリの宣言ファイル（*.shk）← 名前・型・説明・例はここが正
spec/       仕様（syntax / types/ / runtime/ / library/）。思想は spec/README.md
tests/      .shk と .expected の組 + memcheck / bytecheck / imecheck / uicheck（C++ 側の検査）
assets/     同梱するもの。fonts/ に Noto Sans JP（OFL 1.1）。
            探すのはコアではなくフロントエンド（host_use_bundled_font → SHARK_FONT）
```

### コアは2層に分かれる（Makefile の RT_SRC / FE_SRC）

- **RT_SRC**（実行装置）: vm・bytecode・registry・lib など、バイトコードを動かすのに
  要るもの。`sharkvm` はこれだけをリンクし、`shark build` はこれを土台に
  バイトコードを焼き込んで単一バイナリを作る
- **FE_SRC**（前側）: lexer・parser・check・codegen・shark.cpp。ソースから
  バイトコードを作るところ
- コアのソースを増減したら Makefile のこの2変数を直す。`make print-core-src` が
  一覧の正で、`web/build.sh` もこれを読む

### 生成物（直接編集しない）

- `core/prelude.h` — `stdlib/prelude.shk` と `stdlib/prelude_ui.shk`（Shark 自身で
  書いた並べ替えと `ui.run`）から `tools/prelude.py` が作る。直すのは .shk の側
- `docs/reference/`・`web/dist/` — `make docs` / `make web` が作る（clean で消える）

### stdlib/ の宣言ファイル

標準ライブラリの名前・型・説明・例は `stdlib/*.shk` が正で、`core/lib/*.cpp` は
動きだけを持つ。**すべての関数に、1つで完結する動く例を付ける**。
`make docs` が宣言と実装（`r.add` と `core/check.cpp` のメソッド）を突き合わせて
食い違いを知らせ、`make docs-check` が例をぜんぶ実行する。
書き方の詳細は stdlib/README.md。

### 言語側の決めごと

- `null`・例外・`async` は無い。値なしは `T?`、失敗は `Result<T>`、同時実行は
  `task`（スレッドは使わない）。すべての代入はコピーで、共有は `ref` だけ
- 無限ループでもホストが固まらないよう、実行は刻んでホストに返る
- 診断は構造化データで返し、直し方まで示す（`--lang ja|en`）。仕様が決めていない
  実装上の決めごとは docs/implementation.md の表にある
- 未決事項は spec/open-questions.md
