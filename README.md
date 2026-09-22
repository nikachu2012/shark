# Shark🦈

**ゲーム機等の組み込み環境でも動作する、プログラミング学習用ゲーム向けのプログラミング言語。**
外部依存が少なく、移植しやすい設計を第一としています。

```shark
print("Hello, Shark!");
```

- 初学者にとって理解しやすく、かつ高度な処理も記述可能。段階的な学習ステップを意識した設計で、高度な機能は必要になるまで意識させません
- 代入はすべて値コピー。状態を共有したい場合のみ明示的に `ref` を指定します
- `null`、例外、`async/await` を排除。値の不在（Optional）、エラー（Result）、並行実行（Task）をそれぞれ専用の機構で表現します
- 厳格な静的型付けを採用しつつ、コンパイルエラーには具体的な修正方法を必ず提示します
- JITコンパイラ等の高度な最適化機構がない環境でも軽快に動作。充実した標準ライブラリ（Battery-included）のみで一通りの処理が完結します
- コアは実行系（ランタイム）のみに特化。ゲーム等のホストアプリケーションへの組み込みを前提とし、無限ループが記述されてもホスト側がフリーズしないタイムスライス（ステップ）実行に対応。`shark` CLI はコアの外部に独立して実装されています
- コアは C++17 によるリファレンス実装。そのまま動作する完成度で配布されており、改修が必要なのはプラットフォーム移植層とホスト関数の登録の2箇所のみです

## クイックスタート

```
make                              # コア、shark CLI、専用ランタイム（sharkvm）をビルド（外部依存なし）
                                  #   日本語フォント描画時のみ FreeType が必要（後述）
./shark run examples/hello.shk    # Hello, Shark!
make test                         # テストスイート（tests/）を実行
make embed && ./examples/embed/game   # ホストへの組み込みサンプルを実行
./examples/embed/play_stage           # 事前生成したバイトコードを埋め込んだ実行例（コンパイラ不要）
```

```
./shark run <file.shk>     スクリプトを実行（.shkc を指定した場合はコンパイル済みバイトコードを実行）
./shark check <file.shk>   型検査のみ実行
./shark build <file.shk>   ランタイムとバイトコードを統合した単一バイナリを生成（後述）
./shark test [file.shk]    test_ で始まるテスト関数を実行（ファイル省略時は *_test.shk をすべて実行）
./shark fmt <file.shk>…    ソースコードを自動整形（-w で上書き保存、--check で検証のみ）
./shark repl               対話モード（1行ずつ入力してその場で実行。式を打つと値を表示）
./shark explain E0102      エラーコードの詳細説明を表示
./shark modules            有効化されているモジュール一覧を表示

  --memory <MB>            メモリ使用量上限（MB）。超過時は実行時エラー（デフォルト: 256）
  --lang ja|en / --strict  診断メッセージの言語切り替え / 警告をエラーとして扱う
```

## Windows でのビルド

Windows 環境では `make` の代わりに `tools\build_win.bat` を使用します。
**Visual Studio の C++ 開発環境のみでビルド可能**であり、外部ライブラリへの依存はありません
（GUI ウィンドウ生成に必要な Win32 API は実行時に動的ロードされます）。

```
tools\build_win.bat            shark.exe および sharkvm.exe をビルド
tools\build_win.bat freetype   日本語フォント描画用 FreeType を取得・静的ビルド（初回のみ。後述）
tools\build_win.bat test       ビルド後にテスト（tests\）を実行（sh が必要）
tools\build_win.bat clean      ビルド生成物を削除

.\shark.exe run examples\hello.shk
```

| 項目 | 詳細 |
|---|---|
| コンパイラ | [Visual Studio](https://visualstudio.microsoft.com/) 2019 以降の「**C++ によるデスクトップ開発**」（Build Tools でも可）。`vswhere` によりパスを自動検出 |
| `test` の実行要件 | `sh`（[Git for Windows](https://gitforwindows.org/) 等に同梱）。`sh` がない場合でもビルド自体は可能 |
| MSYS2 / MinGW を利用する場合 | `make` がそのまま動作可能（`.exe` の拡張子処理は Makefile 内で自動制御） |

- **日本語を完全サポート。** 診断メッセージも `print` 出力も UTF-8 で統一され、`shark run 日本語.shk` のような日本語ファイル名も扱えます（起動時にコンソールのコードページとコマンドライン引数を UTF-8 に統一）。
- `shark build` は Windows 環境では `.exe` を生成します。出力ファイル名に拡張子がない場合は自動補完されます。
- GUI（`std.ui`）はネイティブな Win32 ウィンドウを開きます。キー入力、文字入力、マウス操作、リサイズ、閉じる操作に対応し、クリップボード連携やマウスポインタ形状の変更も可能です。ディスプレイの拡大率（HiDPI）は OS から取得し、`ui.scale()` で参照できます。
- **日本語入力（IME）にネイティブ対応。** 変換中の未確定文字列は入力欄内に下線付きでインライン表示され、変換候補ウィンドウは入力欄直下に配置されます。確定文字列がそのまま反映され、入力欄での Ctrl+A / Ctrl+C / Ctrl+V / Ctrl+X 等のショートカットも動作します。
- 日本語フォントは同梱の Noto Sans JP により描画されます。日本語を描画するには FreeType のリンクが必要です（`tools\build_win.bat freetype`。後述の「日本語フォント描画」参照）。リンクしない場合、ウィンドウ内の日本語文字は豆腐（□）として表示されます。

## WebAssembly / ブラウザでの動作

同一のコアを WebAssembly にコンパイルした Web 版が [web/](web/README.md) に用意されています。
コードの編集から実行まで、ブラウザのタブ内（クライアントサイド）のみで完結します（サーバーへのコード送信等は一切行いません）。

```
make web            # web/dist/ にビルド（Emscripten が必要）
make web-serve      # ビルドして http://localhost:8000/ でローカル配信
make web-test       # ビルド成果物を Node.js で検証
```

- 移植層（`core/platform/web.cpp`）を追加したのみで、言語処理系および標準ライブラリがそのまま動作します
- `std.ui` はブラウザ内にフローティングウィンドウを描画します（タイトルバーのドラッグ移動、右下でのリサイズ、閉じるボタンに対応）。ブラウザの Canvas API を介して日本語フォントもそのまま描画されます（詳細は [web/README.md](web/README.md) 参照）
- **ゲームもそのまま動作。** サンプルから 2D（ブロック崩し）や 3D（回転する立方体）を選択可能です。キー・マウスイベントがキャンバスに伝達され、`ui.frame()` によりブラウザの描画フレーム（`requestAnimationFrame`）と同期して動作します
- 実行は一定ステップごとにブラウザのイベントループへ制御を戻すため、**無限ループを記述してもブラウザがフリーズすることはありません**。ゲーム組み込み時と同様のタイムスライス機構を採用しています
- エディタには Monaco Editor を採用。コード補完、ホバー説明、引数ヒントが表示され、エラー波線はコンパイラ本体の型検査エンジンと連動しています
- 入出力はターミナルと同様に動作します。標準出力と入力プロンプトが単一のストリームとして表示され、`input()` は入力完了まで非同期に待機します。診断メッセージ、パニック、テスト結果の表示形式も CLI 版と同一です
- **ドキュメントも一体化。** ヘッダーメニューの「説明」（または `Ctrl/⌘ + I`）でモーダルウィンドウが開き、コードを編集しながら API リファレンスや言語ガイドを参照できます。ウィンドウはドラッグで移動・リサイズが可能です。すべて静的ファイル（`web/dist/docs/`）として同梱されているため、完全オフライン環境でも閲覧できます
- `shark.wasm` のバイナリサイズは 940 KB（gzip 時 290 KB）。静的ホスティングに配置するだけで動作し、サーバーサイドの処理は一切不要です

## GUI・グラフィック描画（std.ui）

`std.ui` は2つのレイヤーで構成されています。どちらも処理系内部で直接描画を行い、
外部のGUIツールキットやフォントエンジンに依存しません。

```
./shark run examples/paint.shk       # 低レベル層: マウス描画ツール
./shark run examples/node_editor.shk # 低レベル層: ノードエディタ（ノードを接続して Shark コードを生成）
./shark run examples/counter.shk     # 高レベル層: 最小の宣言的 UI カウンタ
./shark run examples/widgets.shk     # 高レベル層: 全 UI ウィジェットの総合デモ
./shark run examples/breakout.shk    # ブロック崩し（Canvas とアルファブレンディングを使用）
./shark run examples/cube3d.shk      # 3D回転立方体（三角形ラスタライズと Z バッファを使用）
./shark run examples/hexedit.shk     # Hex エディタ（低レベル層でテーブル描画、キー・マウス操作対応）
```

**低レベル描画 API（Immediate-style）** は、ピクセルバッファ（サーフェス）とマウス・キーボードイベントを直接扱います。

```shark
import std.ui;

ui.open("さかな", 160, 120);   // ピクセルバッファを生成。HiDPI 環境では ui.scale() を乗算
while ui.poll() {
  if ui.pressed("esc") { ui.quit(); }
  ui.clear(ui.rgb(0, 20, 40));
  ui.fill_circle(ui.mouse_x(), ui.mouse_y(), 12, ui.rgb(255, 140, 60));
  ui.present();
  ui.frame();                  // 次のフレーム更新まで待機（フレームレートを自動同期）
}
```

**高レベル宣言的 UI（Declarative UI）** は、現在の状態に応じた UI 構造を **`Widget` ツリーとして構築して返す** だけのシンプルな設計です。
イベントループや再描画処理は `ui.run()` が一括管理するため、**開発者は「現在の UI の状態」のみを記述** します。

```shark
var count = 0;                        // 状態は通常の変数

func view() -> Widget {               // 画面の構成を返す
  return ui.col([                     // 縦方向に配置（横方向は ui.row、グリッドは ui.grid）
    ui.label(f"{count} 回"),
    ui.row([
      ui.button("ふやす", func() -> void { count += 1; }),   // クリック時のコールバック
      ui.button("へらす", func() -> void { count -= 1; }),
    ]),
  ]);
}

ui.run("かうんた", 420, 300, view);
```

- 関数の末尾にブロックを続ける記法は持たないため、子要素の入れ子は `ui.col` / `ui.row` / `ui.grid` に **配列（リスト）** として渡して表現します
- **イベントハンドラはウィジェットに直接設定します。** 名前付き関数や無名関数を渡すことも、タグ名（文字列）を渡して `update(hit)` で一括処理することも可能です
- ウィジェット自身は状態を保持しません。状態は呼び出し側が保持して毎フレーム渡すため、**内部状態と描画内容の不整合（状態の二重管理）が発生しません**
- スタイリングはメソッドチェーンで行います: `ui.label("さめ").color(c).padding(6)`
- `ui.run()` は **Shark 自身で記述された標準ライブラリコード** です。特別な内部機構ではなく、低レベル層（`ui.poll` / `ui.show` / `ui.present`）を呼び出してループを実行しています

### 対応プラットフォーム / バックエンド

`shark` コマンドは実行環境に応じた描画バックエンドを自動選択します。

| プラットフォーム | ウィンドウシステム |
|---|---|
| macOS | ネイティブウィンドウ（AppKit） |
| Windows | ネイティブウィンドウ（Win32 + GDI） |
| Linux その他 | ネイティブウィンドウ（X11） |
| ヘッドレス環境（SSH等） | オフスクリーンバッファに描画。描画結果は `ui.get()` や `ui.to_png()` で取得可能 |

サーフェスの1ピクセルは画面の物理ピクセルに対応します。**HiDPI（高精細ディスプレイ）環境ではサーフェス解像度をスケールに合わせて確保します。**

```shark
var k = ui.scale();                  // 通常は 1、Retina 等では 2
ui.open("さめ", 420 * k, 300 * k);   // 見かけのサイズを維持したまま高精細に描画
_ = ui.font(12 * k);                 // 12pt 相当のフォントサイズ
```

- ウィンドウ生成に必要なシステム API は **実行時に動的ロード**（`dlopen` / `LoadLibrary`）するため、**ビルド時のライブラリ依存はありません**。X11 がインストールされていない環境でもコンパイル可能です
- ディスプレイがない環境でも同一のコードが動作するため、**GUI を含むプログラムでも自動テストを実行可能** です
- 環境変数 `SHARK_UI=off` を設定することで、ウィンドウを表示せずオフスクリーン描画に切り替えられます
- 移植層に要求されるのは「ピクセルバッファの転送」と「イベントの通知」の2点のみです。ゲーム等のホストに組み込む際は、そのピクセルバッファをゲーム側のテクスチャ等として受け取ります（詳細は [spec/library/ui.md](spec/library/ui.md) 参照）

## 日本語フォント描画（FreeType）

内蔵フォントは ASCII（5×7 ドット）のみをサポートしています。日本語等のアウトラインフォントを描画するには **FreeType** が必要です。
FreeType は本処理系における **唯一の外部ライブラリ** ですが、**リンクは任意** です。FreeType なしでもビルド・実行は正常に行えます
（日本語が □ と表示されるのみ）。日本語グリフをバイナリ内に静的保持するとバイナリサイズが肥大化するため、フォントラスタライズのみ外部ライブラリを利用可能としています（[spec/library/ui.md](spec/library/ui.md)）。

**フォントファイルは同梱されています。** `assets/fonts/NotoSansJP-Regular.otf`
（Noto Sans JP Regular / [SIL Open Font License 1.1](assets/fonts/LICENSE-NotoSansJP.txt)）が用意されており、
`shark` は実行ファイルと同一ディレクトリの `assets/fonts/` にあるフォントをデフォルトで使用します。
OS のフォントインストール状況に左右されず、全環境で同一の表示結果が得られます。
別のフォントを使用したい場合は、環境変数 `SHARK_FONT` にフォントパスを指定するか（優先適用）、`ui.font(path, size)` で直接指定します。

### 1. FreeType のインストール

| 環境 | 手順 |
|---|---|
| Windows（Visual Studio） | `tools\build_win.bat freetype`（ソースを取得して静的ライブラリを自動ビルド。追加ツール不要） |
| macOS | `brew install freetype` |
| Debian / Ubuntu | `sudo apt install libfreetype-dev pkg-config` |
| Fedora / RHEL | `sudo dnf install freetype-devel pkgconf-pkg-config` |
| Arch Linux | `sudo pacman -S freetype2 pkgconf` |
| Windows（MSYS2） | `pacman -S mingw-w64-x86_64-freetype mingw-w64-x86_64-pkgconf` |

### 2. 再コンパイル

`make` は `pkg-config` を用いて FreeType を自動検出し、利用可能であればリンクします。

```
make clean && make          # FreeType が検出されれば有効化してビルド
```

| 設定内容 | コマンド |
|---|---|
| FreeType を意図的に無効化 | `make FREETYPE=0` |
| pkg-config なしで手動指定 | `make FREETYPE=1 FT_CFLAGS=-I/opt/freetype/include/freetype2 FT_LIBS="-L/opt/freetype/lib -lfreetype"` |
| Windows（Visual Studio） | `tools\build_win.bat freetype` 実行後に `tools\build_win.bat`。手元のライブラリを使う場合は `tools\build_win.bat build -FtInclude <include> -FtLib <freetype.lib>` |
| 有効化の確認 | `./shark run examples/counter.shk`（日本語が表示されればリンク成功） |

※ ビルドフラグの変更時は `make clean` を実行してください（`make` はフラグ変更を検知しないため）。

### 3. スクリプトからのフォント読み込み

**明示的に指定しない場合は内蔵 5×7 フォントが使用されます。** アウトラインフォントを読み込むかはプログラム側で制御します。

```shark
import std.ui;

var k = ui.scale();                   // 高解像度ディスプレイ（Retina 等）では 2
ui.open("さめ", 420 * k, 300 * k);
if !ui.font(12 * k) {                 // ピクセル単位でフォントサイズを指定
  print("フォントが見つかりません");
}
ui.text(8 * k, 8 * k, "こんにちは", ui.rgb(255, 255, 255));
```

フォントの探索順序は、環境変数 `SHARK_FONT` → OS の標準フォントディレクトリです。
`shark` コマンドは、`SHARK_FONT` が未設定の場合、**同梱の Noto Sans JP**（実行ファイルの隣の `assets/fonts/`）を優先して探索します。
同梱フォントが見つからない場合は、各 OS の代表的なフォントを探索します
（macOS: ヒラギノ角ゴシック W4、Windows: 游ゴシック、Linux: Noto Sans CJK Regular）。

### フォントフォールバック機能

1つのフォントファイルにすべての文字が含まれているわけではありません。同梱の Noto Sans JP も日本語と英数字をカバーしていますが、**絵文字やハングル文字などは含まれていません**。そこで、現在選択されているフォントにグリフが存在しない文字が指定された場合、**OS のフォントを順次探索してフォールバック表示** します。

```shark
_ = ui.font(20);
ui.text(10, 10, "日本語 ✓ 한국어 🦈", ui.rgb(0, 0, 0));   // すべて正常に描画
```

- フォールバックフォントは**必要になった時点で遅延ロード**されます。不要な文字が使われない限りロードされません
- 探索順序は `SHARK_FONT_FALLBACK`（`;` 区切りのパスリスト。ユーザー指定が最優先）→ OS の標準フォント（記号 → 絵文字 → ハングル → 日本語の順）
- いずれのフォントにもグリフが存在しない文字のみ □ として描画されます
- 行の高さ（行送り）やベースラインは**プライマリフォント**のメトリクスを維持するため、フォールバック文字が混在しても行のレイアウトが崩れません
- ロードされたフォントはメモリ上にキャッシュされます（最大4フォントまで）。フォント使用時のみメモリを消費します
- **カラー絵文字には非対応です。** 絵文字はモノクロのアウトラインとして描画され、文字色は `ui.text()` 等で指定した色が適用されます

```shark
_ = ui.font("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 15);
_ = ui.font(data, 15);     // メモリ上のフォントデータ（bytes）から読み込み
ui.font_builtin();          // 内蔵の 5×7 フォントに戻す
print(ui.font_name());      // 現在ロードされているフォント名（内蔵フォント時は空文字）
```

### 注意事項

- **文字の幅と高さはロードされたフォントに依存** します（`ui.text_width()` の戻り値も変化します）。内蔵フォント使用時は全環境で同一のピクセルサイズとなります。そのため**デフォルトでは内蔵フォント**が選択されており、`ui.font()` を呼び出した場合のみアウトラインフォントに切り替わります
- ウィジェット（ボタンや入力欄等）の各種寸法はフォントサイズに対する比率で算出されるため、`ui.font()` でサイズを変更すると UI 全体のバランスが連動してスケーリングされます
- `shark build` で生成した単一バイナリは、FreeType 有効でビルドされた場合、配布先環境にも FreeType 動的ライブラリが必要です（`make FREETYPE=0` でビルドした場合は不要）。Windows の `tools\build_win.bat freetype` では **静的リンク** されるため、配布先へのライブラリ同梱は不要です
- 同梱フォントを使用する場合は、`assets/fonts/` ディレクトリを実行ファイルと同一ディレクトリに配置して配布してください。配置されていない場合は OS のシステムフォントへフォールバックします
- ブラウザ版（`make web`）およびゲーム機向けテンプレートでは FreeType をリンクしません。ブラウザ環境ではブラウザ自身の Canvas API を用いてグリフを描画するため、`ui.font()` はそのまま利用可能で日本語も正常に描画されます（移植層の `PlatformFont`。ゲーム機向けテンプレートは内蔵フォントのみサポート）

## CI/CD と配布パッケージ（GitHub Actions）

`.github/workflows/` に 3 つのワークフローが設定されています。

| ワークフロー | トリガー | 処理内容 |
|---|---|---|
| `ci.yml` | `main` への push / Pull Request | macOS・Linux・Windows でビルド、`make test`、`make docs`、`make docs-check` を実行 |
| `pages.yml` | `main` への push | WebAssembly 版をビルドし **Cloudflare Pages** に自動デプロイ |
| `release.yml` | `v` から始まるタグの push | 主要4プラットフォーム向けのバイナリをビルドし、GitHub Release にアタッチ |

すべてのワークフローで FreeType を **ソースから静的ビルド** してリンクします（`tools/freetype_static.sh`、Windows は `tools\build_win.bat freetype`）。配布バイナリは外部依存のない単一実行ファイルとなり、配布先環境での追加インストールは不要です。

```
sh tools/freetype_static.sh                    # FreeType を静的ライブラリとしてビルド（初回のみ）
make $(sh tools/freetype_static.sh --flags)    # 静的ライブラリをリンクしてビルド
```

### ビルド成果物（Artifacts）の取得

CI ワークフロー（`ci.yml`）では、ビルドされた各プラットフォーム向けパッケージをアーカイブして保存します。
GitHub の **Actions → 該当の実行履歴 → Artifacts** からダウンロード可能です（保存期間 14 日間）。
Web 版も同様に、`pages.yml` の実行結果から `shark-web`（`web/dist` の静的成果物一式）を取得できます。

| パッケージ名 | 内容 |
|---|---|
| `shark-macos-arm64` / `shark-macos-x86_64` | macOS 向け実行ファイル一式 |
| `shark-linux-x86_64` | Linux 向け実行ファイル一式 |
| `shark-windows-x86_64` | Windows 向け実行ファイル一式 |
| `shark-web` | Web ブラウザ版（静的ファイル一式） |

配布パッケージはローカル環境でも作成可能です。実行ファイル（`shark` / `sharkvm`）、同梱フォント、サンプルコード、HTML ドキュメント（`docs/`）、README が同梱され、**追加依存なしで即座に動作** します。

```
make dist                    # → dist/shark-<ターゲット>.tar.gz（Windows は .zip）
make dist NAME=macos-arm64   # ターゲット名を明示指定
```

リリースタグ（`v*`）を push すると、GitHub Release にも同一のアーカイブが自動添付されます。

Cloudflare Pages への自動デプロイを有効にするには、リポジトリの Settings → Secrets and variables → Actions に以下の 2 つのシークレットを登録します。登録がない場合、ビルドのみ実行されてデプロイはスキップされます。

| シークレット名 | 内容 |
|---|---|
| `CLOUDFLARE_API_TOKEN` | Cloudflare Pages デプロイ権限を持つ API トークン |
| `CLOUDFLARE_ACCOUNT_ID` | Cloudflare アカウント ID（ダッシュボード右下に表示） |

Pages のプロジェクト名は `pages.yml` の `CF_PAGES_PROJECT`（デフォルト: `shark`）です。あらかじめ Cloudflare 側で同名のプロジェクトを作成してください。

## 単一実行ファイルへのビルド（shark build）

`shark build` は、作成した Shark スクリプトを **単体で動作する単一バイナリ** にコンパイル・パッケージングします。
配布先環境に Shark 処理系をインストールする必要はなく、`.shk` ソースコードの配布も不要です
（詳細は [spec/runtime/bytecode.md](spec/runtime/bytecode.md) 参照）。

```
./shark build examples/hello.shk   # → ./hello（約 446 KB）
./hello                            # Hello, Shark!
```

- 内部構成は **専用ランタイム（VM）＋ コンパイル済みバイトコード** です。
  字句解析・構文解析・型検査・コード生成モジュールは含まれないため、`shark` CLI バイナリ（約 929 KB）よりも大幅に軽量です
  （ランタイム約 444 KB ＋ hello バイトコード約 2 KB。gzip 圧縮時約 152 KB）
- 型検査およびコード生成はビルド時に完了しているため、実行時はバイトコードをロードして実行するのみです
- `import` されたモジュールもバイナリ内に静的統合されるため、生成されたバイナリは単独で任意のパスから実行可能です
- メモリ使用量上限（`--memory`）や診断言語（`--lang`）のオプションは **ビルド時に固定** されます
- コマンドライン引数および標準入力はそのままプログラムへ伝達されます（`os.args()`、`input()`）

```
./shark build --bytecode main.shk   # バイトコードのみ保存（main.shkc）
./sharkvm main.shkc                 # 専用ランタイムで直接実行
./shark run main.shkc               # shark CLI からもバイトコードを直接実行可能
```

`sharkvm` は仮想マシンランタイムのみで構成された軽量実行バイナリであり、`make` 時に同時に生成されます。
`shark build` はこの `sharkvm` をベースとして単一バイナリを構築します。

- 起動速度は `shark run` と同等です（型検査自体が通常 1 ミリ秒程度で完了するため）。単一バイナリ化のメリットは **配布フットプリントの最小化と外部依存の排除** です
- 生成されたバイトコードは同一バージョンのランタイムとの互換性を前提としています。バージョンやホスト関数のシグネチャが不一致のバイトコードは、実行前に検証され安全に拒否されます
- macOS ではバイナリ末尾にペイロードを追加する構造上、`codesign -v` で「追加データが存在する」として署名検証エラーとなります（ローカル実行は可能ですが、Gatekeeper や公証を通す場合は `.app` バンドル化等の対応が必要です）

## パフォーマンス（ベンチマーク）

同一のアルゴリズムを C、Python、Shark で実装して計測した実行速度の比較です。**Python と同等以上の実行速度** を達成しており、高度に最適化された C と比較して約 5〜60 倍程度の実行時間となります。

| テスト内容 | C (-O2) | Python 3.14 | Shark | Shark ÷ C | Shark ÷ Python |
|---|---|---|---|---|---|
| 整数ループ 1,000万回（`sum += i % 7`） | 5 ms | 425 ms | **289 ms** | 59 倍 | 0.68 倍 |
| 再帰呼び出し `fib(32)`（436万回呼び出し） | 5 ms | 149 ms | **309 ms** | 61 倍 | 2.07 倍 |
| 可変長配列に 100万件追加して合計（5回反復） | 6 ms | 346 ms | **206 ms** | 33 倍 | 0.60 倍 |
| 連想配列（map）に 50万件挿入、50万回参照 | 6 ms | 73 ms | **45 ms** | 7 倍 | 0.62 倍 |
| 書式付き文字列生成 100万回 | 32 ms | 121 ms | **169 ms** | 5 倍 | 1.39 倍 |
| （ベースライン）プロセス起動と終了のみ | 3 ms | 10 ms | 2 ms | — | — |

- ループ、配列操作、マップ操作は Python より高速であり、**関数呼び出しおよび文字列生成のオーバーヘッドは Python より大きい** 結果となっています
- Shark の計測値はバイトコード仮想マシン（インタープリタ）によるものです。JIT コンパイラは仕様上任意機能と定義されており、現時点では未実装です
- 計測方法: 3言語で同一アルゴリズムを記述し、**出力結果の一致を検証した上で**、各3回計測した最速値を採用。プロセスの起動時間を含みます（最下行のベースライン参照）
- 計測環境: macOS 26 (arm64) / Apple clang 21 `-O2` / CPython 3.14.3 / Shark 0.1.0

```
python3 bench/run.py            # 全ベンチマークを実行
python3 bench/run.py loop fib   # 特定のベンチマークのみ実行
```

## ドキュメント一覧

| ドキュメント | 内容 |
|---|---|
| [docs/tutorial.md](docs/tutorial.md) | やさしい入門・実践解説書（全13章・ゲームを作りながら楽しく学ぶ） |
| [docs/reference.md](docs/reference.md) | 言語機能ガイド・文法リファレンス（全17章） |
| docs/reference/（`make docs`） | 標準ライブラリ API リファレンス（モジュール別 HTML、全関数の動作サンプル付き） |
| [stdlib/README.md](stdlib/README.md) | 標準ライブラリ宣言ファイル（`.shk`）の仕様・記述ルール |
| [docs/implementation.md](docs/implementation.md) | 実装仕様書（実装範囲・組み込み手順・プラットフォーム移植手順） |
| [web/README.md](web/README.md) | WebAssembly 版ガイド（ビルド手順・ホスト連携・仕様差分） |
| [spec/README.md](spec/README.md) | 言語設計思想および言語仕様書インデックス |
| [spec/open-questions.md](spec/open-questions.md) | 未決定事項・検討中の仕様一覧 |

## ディレクトリ構成

```
core/     実行系コア（C++17）。ファイル I/O やコンソール入出力を行わない組み込み可能設計
  platform/   移植層（desktop / console / web）← ターゲット環境に応じて差し替えるモジュール
  lib/        標準ライブラリの C++ 実装
  bytecode    バイトコードのシリアライズおよびデシリアライズ
  runtime     バイトコード実行専用ランタイム（コンパイラを含まない Engine）
frontend/ shark CLI コマンドおよび sharkvm ランタイムの実装（コアとは独立）
web/      WebAssembly 版関連ファイル一式（移植層: core/platform/web.cpp）
examples/ サンプルコード。embed/ には C++ アプリケーションへの組み込みサンプルを収録
tests/    回帰テストスイート（make test）
bench/    C・Python・Shark のパフォーマンステスト（python3 bench/run.py）
stdlib/   標準ライブラリの型・関数宣言ファイル（*.shk）。API リファレンスおよび入力補完のマスター
tools/    開発支援ツール群（リファレンス生成・サンプル検証・prelude 埋め込み・Windows ビルドスクリプト）
assets/   同梱アセット。fonts/ に Noto Sans JP（日本語フォント）を収録
docs/     言語ガイド・実装ドキュメント。gen.py により stdlib/ から HTML リファレンスを生成
spec/     詳細言語仕様書
  types/      型システム仕様
  runtime/    ランタイム内部構造およびホスト境界仕様
  library/    標準ライブラリ仕様
  skeleton.md コアの雛形設計。モジュール構成と拡張ポイント
  frontend.md CLI フロントエンドの実装仕様
```
