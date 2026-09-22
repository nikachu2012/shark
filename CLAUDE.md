# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## これは何か

Shark🦈 — ゲーム機等の組み込み環境向けプログラミング学習用言語の処理系。
ドキュメント・コメント・コミットメッセージはすべて自然な日本語で記述する
（コミットメッセージは `feat(lang):` や `fix(lib):` などのプレフィックス + 日本語の要約）。

## コマンド

```
make                # shark CLI と sharkvm ランタイムをビルド（外部依存なし。FreeType のみ任意で自動検出・リンク）
make test           # テストスイート（tests/）を実行（sh tests/run.sh。memcheck / bytecheck / imecheck / uicheck を含む）
make docs           # stdlib/ の宣言ファイルおよび docs/reference.md から docs/reference/ に HTML リファレンスを生成し、
                    # C++ 実装と突合検証する（make web は同様のドキュメントを web/dist/docs/ にも生成）
make docs-check     # 宣言ファイルに記載された全サンプルコードを shark で実行検証（python3 tools/runex.py）
make embed          # ホストへの組み込みサンプルをビルド（examples/embed/game および play_stage）
make dist           # 配布用アーカイブを生成（dist/shark-<ターゲット>.tar.gz。tools/package.sh を実行）
make web            # WebAssembly 版を web/dist/ にビルド（Emscripten が必要）
make web-serve      # ビルドして http://localhost:8000/ でローカル配信
make web-test       # ビルド成果物を Node.js でヘッドレス検証
make bench          # C・Python・Shark のベンチマークを実行（python3 bench/run.py loop fib 等で対象指定可能）
```

FreeType を静的リンクして（配布用バイナリの形式で）ビルド:

```
sh tools/freetype_static.sh                    # FreeType を静的ライブラリとしてビルド（build/ に配置）
make $(sh tools/freetype_static.sh --flags)    # 静的ライブラリをリンクしてビルド
```

Windows（Visual Studio）では make の代わりに:

```
tools\build_win.bat          # shark.exe および sharkvm.exe をビルド（外部依存なし）
tools\build_win.bat freetype # FreeType ソースを取得し静的ビルド（日本語フォント描画用。初回のみ）
tools\build_win.bat test     # ビルド後にテスト（tests\）を実行（sh が必要）
tools\build_win.bat clean    # ビルド生成物を削除
```

内部処理は `tools/build_win.ps1`。ソースファイル一覧は Makefile の RT_SRC / FE_SRC が正（マスター）であり、
このスクリプトはそのコピーであるため、**コアのソースファイルを増減した際は両方を更新する**。
MSYS2 / MinGW の make 環境であれば Makefile がそのまま動作する（`.exe` 拡張子は Makefile が制御）。

```
./shark run <file.shk>      # スクリプトを実行（.shkc も指定可能）
./shark check <file.shk>    # 型検査のみ実行
./shark test [file.shk]     # test_ で始まるテスト関数を実行（--filter 名前 で絞り込み可能）
./shark fmt <file.shk>      # ソースコードを自動整形（-w で上書き保存、--check でフォーマット検証のみ）
./shark build <file.shk>    # 単一実行バイナリを生成（--bytecode で .shkc のみ保存）
./sharkvm <file.shkc>       # 専用ランタイムで直接実行
./shark repl                # 対話モード（前の入力の変数・関数・クラスを引き継ぐ。:help で使い方）
./shark explain E0102       # エラーコードの詳細説明を表示
```

- テストを単体実行する: `SHARK_UI=off ./shark run --no-color tests/cases/01_basics.shk` の
  出力を同名の `.expected` と比較検証する（`tests/run.sh` の内部処理と同一）。
  テストを追加する際も `.shk` と `.expected` のペアを `tests/cases/`（正常系）または
  `tests/errors/`（エラー診断）に配置するのみで反映される
- テスト実行時は `SHARK_UI=off`（ウィンドウを表示せず、オフスクリーンバッファに描画）
- ヘッダ（`.h` / `.inc`）変更時は全 `.o` が再コンパイルされる。ビルド設定フラグ
  （`FREETYPE=` 等）を変更した際は `make clean && make` を実行する（Makefile はフラグ変更を自動検知しないため）

## CI/CD ワークフロー（.github/workflows）

`ci.yml`（4プラットフォームでの自動ビルドとテスト）・`pages.yml`（WebAssembly 版の Cloudflare Pages への自動デプロイ）・
`release.yml`（リリースタグ push 時のバイナリ配布）の3つが設定されている。
すべて FreeType を静的ビルドしてリンクするため、配布物は単一の実行バイナリで完結する。
**パッケージング処理は tools/package.sh に集約** されており、CI・手動リリース・`make dist` のすべてで同一スクリプトが呼ばれる。
生成されたバイナリは各実行の Artifacts からダウンロード可能。
Pages へのデプロイには `CLOUDFLARE_API_TOKEN` と `CLOUDFLARE_ACCOUNT_ID` のシークレット設定が必要（README 参照）。

## アーキテクチャ

実装は **実行系（コア）のみ** に特化し、コマンドラインツール等のホストはその外部に独立実装する、が大原則
（spec/README.md）。コアはゲーム等に組み込むコンポーネントであり、**自律的にファイル I/O を行わず、
標準出力への書き込みも行わない**。入出力はすべて呼び出し側（ホスト）から委譲される。

```
core/       実行系。字句解析→構文解析→型検査→コード生成→仮想マシン。C++17、
            -fno-exceptions -fno-rtti、外部ライブラリ依存なし（FreeType のみ任意）
  platform/   移植層（desktop / console / web）← プラットフォームごとに差し替えるモジュール
  lib/        標準ライブラリの実装（*.cpp）。ホスト関数は Engine::register_host() に登録
frontend/   shark CLI（main.cpp）と sharkvm（vm_main.cpp）。コアを利用するリファレンス実装
web/        同一コアを WebAssembly 化した Web 実行環境（移植層: core/platform/web.cpp）。
            プレイグラウンドのサンプルコードは web/examples.py の ITEMS が正であり、
            examples/ と不整合があると make web が停止する
stdlib/     標準ライブラリの宣言ファイル（*.shk）← 名前・型・説明・サンプルコードのマスター
spec/       言語仕様書（syntax / types/ / runtime/ / library/）。設計思想は spec/README.md
tests/      .shk と .expected のペア + memcheck / bytecheck / imecheck / uicheck（C++ ユニットテスト）
assets/     同梱アセット。fonts/ に Noto Sans JP（OFL 1.1）。
            探索はコアではなくフロントエンド側が担当（host_use_bundled_font → SHARK_FONT）
```

### コアは2層に分かれる（Makefile の RT_SRC / FE_SRC）

- **RT_SRC**（ランタイム）: vm・bytecode・registry・lib 等、バイトコード実行に必要な
  最小限のモジュール群。`sharkvm` はこれらのみをリンクし、`shark build` はこれをベースに
  バイトコードを埋め込んで単一実行ファイルを生成する
- **FE_SRC**（フロントエンド）: lexer・parser・check・codegen・shark.cpp。ソースコードから
  バイトコードを生成するコンパイラ部分
- コアのソースファイルを増減した際は Makefile のこの2変数を更新する。`make print-core-src` が
  一覧のマスターであり、`web/build.sh` もこれを参照する

### 生成物（直接編集しない）

- `core/prelude.h` — `stdlib/prelude.shk` および `stdlib/prelude_ui.shk`（Shark 自身で
  実装されたソート処理および `ui.run`）から `tools/prelude.py` により自動生成。修正時は .shk 側を編集する
- `docs/reference/`・`web/dist/` — `make docs` / `make web` により生成される（clean で削除可能）。
  `make web` は Web プレイグラウンドと **API ドキュメント**（`web/dist/docs/`）を同時に生成する

### stdlib/ の宣言ファイル

標準ライブラリの名前・型注釈・ドキュメント・サンプルコードは `stdlib/*.shk` が正（マスター）であり、`core/lib/*.cpp` は
実行ロジックのみを保持する。**すべての関数に、単体で完結する動作サンプルコードを付与する**。
`make docs` が宣言と実装（`r.add` および `core/check.cpp` の型情報）を突合して
不整合を検出し、`make docs-check` が全サンプルコードを実際に実行検証する。
詳細な記述ルールは stdlib/README.md 参照。

### 言語設計の原則

- `null`・例外・`async/await` は存在しない。値の不在は `T?`、失敗は `Result<T>`、並行処理は
  `task` で表現（スレッドは直接露出させない）。代入はすべて値コピーであり、共有参照は `ref` のみ
- 無限ループ時にもホスト側がフリーズしないよう、実行は一定ステップごとにホストに制御を戻す
- コンパイラ診断は構造化データとして返却し、具体的な修正案まで提示する（`--lang ja|en`）。仕様で規定されていない
  実装上の決定事項は docs/implementation.md の一覧に記載
- 未決定の仕様・検討事項は spec/open-questions.md
