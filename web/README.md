# WebAssembly / ブラウザ実行環境

Shark コアを WebAssembly にコンパイルし、Web ブラウザ上で Shark スクリプトの記述および実行を完結できるようにした環境です。
処理系は**ブラウザのタブ内（クライアントサイド）のみ**で動作し、入力したコードが外部サーバーへ送信されることはありません。

エディタには **Monaco Editor**（VS Code のコアエディタ）を採用し、Shark 向けのシンタックスハイライトおよび
インテリセンス（入力補完・ホバー情報・シグネチャヘルプ）を実装しています。構文エラーや型エラーの波線表示は、
**コンパイラ本体の型検査エンジン**と直接連動しています。
入出力インターフェースはネイティブなターミナルと同様に設計されており、`input()` による非同期標準入力にも対応しています。

```
make web                    # web/dist/ に静的アセットをビルド（Emscripten が必要）
make web-serve              # ビルド後に http://localhost:8000/ でローカル HTTP 配信
make web-serve PORT=8080    # ポート番号を指定してローカル配信
make web-test               # ビルド成果物を Node.js でヘッドレス検証
```

ビルドと配信は独立したスクリプト（`web/build.sh` と `web/serve.sh`）で提供されています。
配信スクリプトは内部でビルドスクリプトを実行するため、ソースコードの変更が即座に反映されます。
なお、ブラウザのセキュリティ制限により `file://` プロトコルでは `.wasm` を読み込めないため、ローカル確認時は HTTP 配信をご利用ください。

初回ビルド時のみ、Monaco Editor を npm からダウンロードして `web/vendor/` にキャッシュします（Git 管理対象外）。
2回目以降はローカルキャッシュを使用します。本番配布に必要なアセットは `web/dist/` 内にすべて完結しており、
**実行時に外部 CDN や外部サーバーへのアクセスは一切発生しません**。

Emscripten が未セットアップの場合は、`make web` 実行時にセットアップ案内が表示されて中断します。

```
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest
```

## 構成ファイル

| ファイル | 役割 |
|---|---|
| [`../core/platform/web.cpp`](../core/platform/web.cpp) | Web 移植層。メモリ管理・時刻取得・入出力・仮想ファイルシステムの実装 |
| [`../core/platform/screen_canvas.inc`](../core/platform/screen_canvas.inc) | Web 画面移植層。ウィンドウ描画、`std.ui` バッファの Canvas 転送、DOM イベント伝達 |
| [`../core/platform/font_canvas.inc`](../core/platform/font_canvas.inc) | Web フォント移植層。ブラウザの Canvas API によるフォントラスタライズ |
| [`shark_web.cpp`](shark_web.cpp) | ホストブリッジ。`Engine` を初期化・実行し、出力や診断情報を JS へ伝達 |
| [`app.js`](app.js) | UI コントローラ。Monaco Editor の初期化、タイムスライス実行制御、ターミナルエミュレーション |
| [`lang.js`](lang.js) | Monaco 向け言語定義。シンタックスハイライトおよび入力補完プロバイダ |
| [`api.py`](api.py) | 補完定義（`api.js`）を [`../stdlib/`](../stdlib/README.md) の宣言ファイルから生成するツール |
| [`index.html`](index.html) / [`style.css`](style.css) | プレイグラウンドの HTML 構造およびスタイルシート |
| [`build.sh`](build.sh) | ビルドスクリプト。emcc の実行、Monaco の取得、`web/dist/` への成果物集約 |
| [`serve.sh`](serve.sh) | ローカル配信スクリプト。`build.sh` を実行後にローカルサーバーを起動 |
| [`test.js`](test.js) | ビルド成果物を Node.js 環境でテストする検証スクリプト |
| [`examples.py`](examples.py) / [`examples/`](examples) | サンプルコード一覧を `examples.js` に集約するスクリプト |
| [`../docs/gen.py`](../docs/gen.py) | ドキュメント生成。宣言ファイルと `docs/reference.md` から HTML を生成 |

### ドキュメントの一体化

`make web` は、プレイグラウンドのビルドと同時に **HTML ドキュメントを `web/dist/docs/` に統合** します
（`make docs` で `docs/reference/` に生成されるものと同一）。
ヘッダーの「説明」ボタン（または `Ctrl/⌘ + I`）でモーダルウィンドウが開き、標準ライブラリの API リファレンスおよび
言語ガイド（[`../docs/reference.md`](../docs/reference.md)）を、**コードを書きながら同一画面上で参照** できます。
別タブへ遷移することなくコーディングを継続できます。ウィンドウはドラッグ移動やサイズ変更に対応し、
表示位置・サイズ・表示状態はブラウザのローカルストレージに保持されます。

すべて静的アセットとして同梱されているため、**完全オフライン環境でもドキュメントを閲覧可能** です。

### サンプルコードの管理

プレイグラウンドのセレクトボックスに表示されるサンプルコードは、[`examples.py`](examples.py) の `ITEMS` が正（マスター）です。
サンプルの表示順および日本語タイトルを一元管理しています。

[`../examples/`](../examples) に追加されたサンプルコードが `ITEMS` に登録されていない場合、
ビルド時に**双方向の整合性検証が行われ、未登録のファイルが存在する場合はビルドが停止** します（`make web` が失敗）。

- `../examples/*.shk` および `web/examples/*.shk` のすべてが `ITEMS` に登録されている必要があります
- `ITEMS` に記述されたファイルが実際に存在することを検証します

`web/examples/` に配置されているのは、ブラウザ環境特有のサンプル（対話型標準入力、無限ループデモ、Canvas フォントを使用する `ui.shk`）のみです。
それ以外の汎用サンプルは `../examples/` のファイルをそのまま参照します。

WebAssembly 経由で正常に動作するかは [`test.js`](test.js) により自動検証されます。

```
        Shark プログラム（エディタで記述）
 ────────────────────────────────
   仮想マシン・型検査・標準ライブラリ        core/           ← 全プラットフォーム共通
 ────────────────────────────────
   プラットフォーム移植層                 platform/web.cpp  ← WebAssembly 専用
 ────────────────────────────────
   ホストブリッジ                         web/shark_web.cpp
   UI・エディタ制御                       web/app.js, web/lang.js
```

`core/` には一切の特殊な変更を加えていません。Web 向けの移植層を追加したのみで、
言語機能および標準ライブラリが CLI 版と同一に動作します（詳細は [../spec/runtime/platform.md](../spec/runtime/platform.md) 参照）。

## UI がフリーズしないタイムスライス実行

ブラウザの JavaScript はシングルスレッドで動作するため、時間のかかるループ処理を同期実行すると UI 全体が応答不能（フリーズ）になります。
Shark は **実行を細かく分割（タイムスライス）してブラウザのイベントループに制御を戻す** アーキテクチャを採用しているため、UI をブロックしません（詳細は [../spec/runtime/embedding.md](../spec/runtime/embedding.md) 参照）。

```js
function tick() {
  const status = shk_pump(budget);   // 指定された budget 命令数だけ VM を進めて制御を戻す
  drainOutput();                     // その間に出力された print テキストをターミナルへ反映
  if (status === 0) requestAnimationFrame(tick);   // 継続実行の場合は次フレームへスケジュール
}
```

`budget` は消費可能な **仮想マシン命令数** であり、ミリ秒等の時間ではありません。値を増減させても実行結果には影響せず、
1フレームあたりの専有時間のみが変化します。`app.js` では 1 フレームあたりの実行時間が 6〜14 ms に収まるよう動的に調整しています。

無限ループを記述した場合でも `status` は 0 のまま戻ってくるため、ブラウザは応答性を維持し、
いつでも「停止」ボタン（`shk_abort`）で安全に中断できます。

### フレームレート制御と同期

`requestAnimationFrame` を利用しているため、プログラムの実行は描画更新タイミング（通常 60 fps / 約 16.7 ms ごと）に同期して進みます。
ここでループ末尾に `sleep(0.016)` のような固定スリープを記述してしまうと、**描画処理時間分が累積してフレーム境界を踏み外し**、
フレームドロップにより実行速度が 30 fps に低下する問題が発生します。

そのため、フレーム待機には `sleep()` ではなく **`ui.frame()`** を使用します。
`ui.frame()` は指定時間スリープするのではなく、**次フレームの目標更新時刻（デッドライン）** を算出して待機するため、処理遅延によるフレーム落ちを回避できます（詳細は [../spec/library/ui.md](../spec/library/ui.md) 参照）。

Web 移植層では `PlatformScreen::host_paced` を `true` に設定し、ホスト（ブラウザ）側がフレームペーシングを主導することを通知します（[`../core/platform/screen_canvas.inc`](../core/platform/screen_canvas.inc)）。
`ui.frame()` はこれを検知して目標フレームタイミングに合わせて復帰するため、60Hz や 120Hz ディスプレイのいずれでも正確なフレームレートを維持できます。

## ターミナルエミュレータ

画面右側にはネイティブターミナルと同等のターミナルエミュレータを配置しています。
標準出力およびユーザーの入力テキストが単一のストリームとして表示され、CLI 版（[../frontend/main.cpp](../frontend/main.cpp)）と同一のフォーマットで出力されます。

```
$ shark run playground.shk
名前を教えてください
さめ                        ← ここに入力。入力内容は履歴に残る
こんにちは、さめ さん！
$ 
```

| 操作 | ショートカット / キー |
|---|---|
| 入力の確定 | テキスト入力後に <kbd>Enter</kbd>。`input()` は入力完了まで非同期待機 |
| EOF の送信 | <kbd>Ctrl</kbd> + <kbd>D</kbd>（当該 `input()` の戻り値は `none` となる） |
| 実行の中断 | <kbd>Ctrl</kbd> + <kbd>C</kbd>（「停止」ボタンと同等） |
| 画面のクリア | <kbd>Ctrl</kbd> + <kbd>L</kbd> / <kbd>Ctrl</kbd> + <kbd>U</kbd> で入力中テキストを消去 |
| コマンド履歴参照 | <kbd>↑</kbd> <kbd>↓</kbd> |

UI 上のボタン操作だけでなく、ターミナル上で直接 `run` `check` `test` `explain E0102` `modules` `version` `clear` `help` などのコマンドを実行可能です（`shark run playground.shk` のようなフル形式も可）。
`--lang` `--memory` `--strict` `--no-color` 等の CLI オプションも同様に利用できます。
読み込み対象ファイルは左側エディタで編集中の `playground.shk` です。

文字入力はカーソル位置に配置された不可視の `textarea` で受け取るため、ブラウザの IME（かな漢字変換）が自然に動作し、`input()` 経由で日本語文字列を正しく入力できます。

### 非同期入力待機（ノンブロッキング I/O）

ブラウザのメインスレッドではスレッドを完全に停止して入力を待つことができないため、`input()` は**待機状態へ遷移してホストへ制御を戻します**（詳細は [../spec/runtime/embedding.md](../spec/runtime/embedding.md) 参照）。
`HostIO::input_ready` が未完了を返す間は `shk_waiting_input()` が 1 を返し、UI 側は入力待ちプロンプトを表示します。
ユーザーが入力を確定すると、中断箇所から実行が再開されます。
`sleep` や別タスク（`task`）も、入力待機中に並行して進行します。

## 入力補完（IntelliSense）

| 機能 | 詳細 |
|---|---|
| シンタックスハイライト | 予約語、f 文字列の `{ }`、ネストしたブロックコメントまで正確にハイライト |
| コード補完 | `math.` でモジュール関数、`xs.` で型メソッド、通常位置では予約語・テンプレート・定義済み関数・変数 |
| 継承メンバの補完 | サブクラスのインスタンスに対しても、スーパークラスの `public` メンバおよびメソッドを候補に表示 |
| メソッドオーバーライド生成 | クラス定義内で、親クラスの `virtual` メソッドに対する `override` 雛形コードを自動提示 |
| ホバー情報 | 関数のシグネチャおよび仕様書に準拠した日本語ドキュメント |
| シグネチャヘルプ | `(` 入力時に関数引数情報を表示（オーバーロード一覧の切り替えに対応） |
| 定義ジャンプ | エディタ内で定義された関数・クラスへジャンプ（F12） |
| シンボル検索 | Ctrl/⌘ + Shift + O でファイル内シンボル一覧を表示・検索 |
| リアルタイム型検査 | 入力停止時に**本物のコンパイラ型検査**がバックグラウンド実行され、エラー箇所に波線とエラーコードを表示 |
| 自動 import 補完 | 補完候補でモジュールを選択した際、未インポートであれば先頭に `import std.xxx;` を自動挿入 |

入力補完で使用する標準ライブラリ定義テーブル（`api.js`）は、[`api.py`](api.py) が [`../stdlib/*.shk`](../stdlib/README.md)（宣言ファイル）から自動生成します。手作業によるテーブル定義は持ちません。
HTML リファレンス（`make docs`）と**同一のマスターソースから生成**されるため、ドキュメントやサンプルコードに食い違いが生じません。

`api.py` はビルド時に C++ 実装（`core/lib/*.cpp`）と突合検証を行い、宣言と実装に不整合があればビルドを中断します。
仕様書に定義されていても現在の実装に含まれないモジュール（`std.net` 等）は宣言が存在しないため補完にも現れません。
`make web-test` により、`api.js` の一覧と実際のランタイム登録モジュールの一致が検証されます。

### メソッドオーバーライドの補完

`class Shark : Fish {` のクラス本体内で補完をトリガーすると、親クラス `Fish` から継承した仮想メソッド一覧が提示されます。
候補を選択すると、適切な `override` 雛形が自動展開されます。

```shark
class Shark : Fish {
  // ここで describe を選択すると以下が自動挿入される
  public override func describe() -> string {
    |
    return super.describe();
  }
}
```

- オーバーライド可能なメソッド（`virtual` または基底の `override`）のみが候補に表示されます
- 既に実装済みのメソッドやコンストラクタ（`init`）は候補から除外されます
- 親クラスだけでなく、先祖クラスやインタフェース（`Comparable` 等）のメソッドも再帰的に収集されます
- 純粋仮想メソッド（未実装の `virtual`）は空の本体と戻り値型のデフォルト値（`int` なら `return 0;`）が自動挿入され、優先度高く表示されます
- 実装を持つ仮想メソッドは `super` 呼び出しを含む雛形が生成されます
- 親クラスのアクセス修飾子（`public`）は適切に継承されます

## ホスト API（C-ABI エクスポート）

`shark_web.cpp` が提供する WebAssembly エクスポート関数一覧です。独自の Web ページに Shark を組み込む場合はこれらの API を呼び出します。

| 関数名 | 役割 |
|---|---|
| `shk_boot()` | 初期化処理。Web 移植層の登録（起動時に1度だけ実行） |
| `shk_config(memory_mb, lang_en, strict)` | 実行環境設定（メモリ上限 MB、診断言語、警告の厳格扱い） |
| `shk_add_module(path, source)` | `import` 可能な仮想モジュールソースを追加 |
| `shk_load(name, source)` | スクリプトをコンパイル。構文エラー・型エラーの件数を返却 |
| `shk_diagnostics()` | 直前のコンパイルで発生した診断メッセージ一覧（JSON 形式） |
| `shk_start_run()` / `shk_start_test()` | スクリプト実行の開始 / テスト実行の開始 |
| `shk_pump(budget)` | 指定ステップ（命令数）だけ VM を実行。0=継続中、1=正常終了、2=エラー停止 |
| `shk_abort()` / `shk_idle()` | 実行の中断要求 / VM がアイドル状態（入力待ち等）か判定 |
| `shk_out_ptr()` `shk_out_len()` `shk_out_clear()` | 標準出力バッファのポインタ・長さ取得・バッファクリア（UTF-8） |
| `shk_push_input(text)` | `input()` 待機中の VM へ1行分の入力テキストを送信 |
| `shk_push_eof()` | 入力の終端（EOF / Ctrl+D）を通知（次回の `input()` は `none` を返却） |
| `shk_waiting_input()` | VM が `input()` 入力待ちでサスペンドしているか判定 |
| `shk_error()` | 実行時エラー詳細（JSON 形式。メッセージ、発生位置、スタックトレース） |
| `shk_exit_code()` / `shk_test_passed()` / `shk_test_total()` | プロセス終了コード / テスト成功件数 / テスト総数 |
| `shk_memory_used()` / `shk_memory_limit()` | 現在のヒープ使用量 / メモリ上限（バイト単位） |
| `shk_format(source)` / `shk_formatted()` | ソースコード自動整形実行 / 整形結果の取得（`shark fmt` 相当） |
| `shk_modules()` / `shk_explain(code)` / `shk_version()` | 有効モジュール一覧 / エラー解説文 / バージョン文字列 |

標準出力を「ポインタと長さ」で取得する設計は、マルチバイト文字（UTF-8）がパケット境界で分断されても文字化けを発生させないためです。
`shk_out_*` 呼び出し直後にメモリバッファからデータをコピーして利用してください（Wasm メモリ拡張時にベースポインタが再配置される可能性があるため）。

## GUI レンダリング（std.ui）

`ui.open()` を呼び出すと、ブラウザ内に**仮想ウィンドウ**が生成されます。
タイトルバーのドラッグ移動、右下リサイズハンドルによる拡縮、閉じるボタン（×）による終了に対応しています。
UI デザインは **Dear ImGui** スタイルを踏襲しており、ダークテーマ基調のシャープな外観となっています。
ウィンドウマネージャは Web 移植層（[`../core/platform/screen_canvas.inc`](../core/platform/screen_canvas.inc)）で実装されており、
コア側は通常通りピクセルバッファに対して描画を行うため、**描画ロジックは他プラットフォームと完全に同一** です（[../spec/library/ui.md](../spec/library/ui.md) 参照）。

Web ページ内の特定の HTML 要素に直接キャンバスを埋め込みたい場合は、マウントポイントを指定することで独立ウィンドウを作らずにインライン描画が可能です。

| 優先度 | マウントターゲット |
|---|---|
| 1 | `Module.sharkMount`（DOM 要素または `querySelector` に渡すセレクタ文字列） |
| 2 | `#shark-screen` 要素 |
| 3 | いずれも未指定の場合、ブラウザ内にフローティングウィンドウを生成（プレイグラウンドの動作） |

マウント先の要素は描画開始時に初期化されるため、他のコンテンツと共用しないでください。
ウィンドウの開閉はカスタム DOM イベントにより通知されます。

```js
window.addEventListener("shark:screen-open", function (e) {
  e.detail.canvas;          // 生成された HTMLCanvasElement
  e.detail.width;           // サーフェス幅（ピクセル）
  e.detail.requestClose();  // ウィンドウを閉じる要求（ui.poll() が false を返す）
});
window.addEventListener("shark:screen-close", function (e) { /* クローズ処理 */ });
```

| 項目 | ブラウザ環境での挙動 |
|---|---|
| サーフェス解像度 | `ui.open(横, 縦)` で指定した解像度で生成。サーフェスの1ピクセルが論理ピクセルに対応。低解像度サーフェス（640×480 未満）は整数比率で拡大表示し、表示領域を超過する場合はアスペクト比を維持して自動縮小 |
| HiDPI（高精細画面） | `ui.pixel_ratio()` が `window.devicePixelRatio` を返却（1.25 や 1.5 等の小数に対応）、`ui.scale()` は整数値。ブラウザのズームや画面移動によりスケールが変更された場合はサーフェスが自動再生成される |
| リサイズ | ウィンドウ右下ハンドルの操作またはマウント要素のリサイズにより `SEV_Resize` イベントが発生し、バッファが更新される（`ui.open(…, false)` 指定時はリサイズ無効） |
| キー入力 | `keydown` / `keyup` を捕捉。矢印キー、Space、Tab 等のブラウザ標準動作は `preventDefault()` で抑制。フォーカス喪失時は押下状態を自動解除 |
| マウス入力 | Pointer Events API によりタッチ操作にもシームレスに対応。右クリックは `ui.menu` 等のコンテキストメニュー処理に割り当て（ブラウザ標準メニューを抑止） |
| テキスト入力 | `ui.field` フォーカス時のみ不可視の `textarea` を経由して文字入力を受付。**OS ネイティブの IME による日本語変換をそのまま利用可能**（未確定文字列は `ui.marked()` で取得） |
| マウスポインタ | `ui.cursor()` の指定値が CSS の `cursor` プロパティへ反映。ボタン等の操作可能要素ではポインタ（指）、入力欄ではテキスト選択カーソルへ自動変更 |
| フォント描画 | **ブラウザの Canvas API を用いてラスタライズ**（後述）。`ui.font()` により日本語フォントも高品位に描画 |
| クローズ処理 | ウィンドウの閉じるボタン押下で `SEV_Close` イベントが発生。インライン埋め込み時はホスト側から `requestClose()` を呼び出す |

環境変数 `SHARK_UI=off` を設定した場合、DOM 上に画面を生成せずオフスクリーンバッファとして描画されます（`ui.get()` や `ui.to_png()` で画像を取得可能）。Node.js 等のヘッドレス環境でも同一のコードが動作します。

## フォントラスタライズ（std.ui）

WebAssembly 環境からは OS のフォントファイルに直接アクセスできないため、FreeType は使用しません。
その代わり、**ブラウザの Canvas 2D API を用いて文字を 1 文字ずつ描画し、そのアルファチャンネル（濃淡情報）をテクスチャキャッシュに転送する** 方式を採用しています（[`../core/platform/font_canvas.inc`](../core/platform/font_canvas.inc)）。
コア側から見ると FreeType によるラスタライズ結果と同一形式のビットマップが得られるため、レンダリングパイプラインを変更することなく動作します。

```shark
var k = ui.scale();
_ = ui.font(12 * k);        // ブラウザフォントエンジンへ切り替え
ui.text(4, 4, "こんにちは", ui.rgb(255, 255, 255));
print(ui.font_name());      // 使用中のフォント名（例: Hiragino Sans）
```

使用するフォントファミリは、主要な OS（Windows、macOS、Linux、iOS、Android）に標準搭載されている日本語フォントを優先順にフォールバック設定しています。

| プラットフォーム | 優先フォントリスト |
|---|---|
| macOS / iOS | Hiragino Sans, Hiragino Kaku Gothic ProN |
| Windows | Yu Gothic UI, Yu Gothic, Meiryo, MS Gothic |
| Linux / Android / ChromeOS | Noto Sans CJK JP, Noto Sans JP, IPAexGothic, IPAGothic, VL Gothic |
| 汎用フォールバック | system-ui, sans-serif |

- `ui.font_name()` は、**クライアント環境に実際にインストールされているフォント名** を返却します（Canvas でのメトリクス比較により実在を判定）
- `ui.font(名前, サイズ)` を呼び出す際、Web 環境ではファイルパスではなく **CSS フォントファミリ名** を指定可能です（`ui.font("Meiryo", 16)`。該当フォントが存在しない場合は false を返却）
- ラスタライズされたグリフはグリフキャッシュに保持されるため、大量の日本語文字列を描画する場合でもレンダリング負荷は初回のみです
- 絵文字はアルファマスクとして抽出されるため、モノクロ描画となります（文字色は `ui.text()` に指定した描画色が適用されます）

## ブラウザ環境における制約事項

| 制約事項 | 動作仕様 |
|---|---|
| ローカルファイルアクセス | `std.file` がアクセスするのはブラウザのメモリ内仮想ファイルシステム（Emscripten MEMFS）。タブを閉じるとデータは破棄されます |
| 外部プロセスの起動 | `os.run()` は非対応であり、失敗結果（`Result.err`）を返却します（詳細は [../spec/library/os.md](../spec/library/os.md) 参照） |
| 同期スリープ | メインスレッドを完全にブロックする同期 `sleep` はサポートされません。`sleep()` はタスクスイッチをトリガーし、実時間は描画フレームに合わせて進行します |
| ネットワークソケット | `std.net` および `std.http` は現バージョンのコアには未実装です |
| 外部フォントファイルの直接読み込み | ファイルパスからのフォントファイル直接パースはサポートされません。`ui.font(bytes, size)` は false を返却します（上記の通りブラウザのフォントエンジンを使用してください） |
| クリップボードの直接読み取り | ブラウザのセキュリティ制約により、任意のタイミングでのクリップボード直接読み取りは制限されます。`ui.clipboard()` はペーストイベント（Ctrl+V）で渡されたデータ、または直前に `ui.set_clipboard()` で格納したデータを返却します |

`os.platform()` は `"wasm"` を返却します。

## バイナリサイズとフットプリント

| コンポーネント | 元サイズ | gzip 圧縮後 |
|---|---|---|
| `shark.wasm`（コア処理系） | 938 KB | 290 KB |
| `shark.js`（Emscripten グルーコード） | 87 KB | 25 KB |
| `vendor/vs`（Monaco Editor） | 4.2 MB | 1.2 MB |
| アプリケーション UI（`app.js`, `lang.js`, `api.js` 他） | 236 KB | 60 KB |

Monaco Editor は Shark の編集に必要な **コアエディタ機能および補完機能のみに最適化** してバンドルしています（他言語サポートや TypeScript 言語サービスを除去し、24 MB から 4.2 MB に削減）。

`web/dist/` ディレクトリは完全な静的サイトとして構成されており、GitHub Pages、Cloudflare Pages、S3 等の静的ホスティングへデプロイするだけでそのまま動作します（サーバーサイド処理は一切不要）。
