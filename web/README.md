# ブラウザで動かす

コアをそのまま WebAssembly にして、ブラウザの中で Shark を書いて動かせるようにしたもの。
処理系は**そのタブの中だけ**で動く。書いたものはどこにも送られない。

書くところは **Monaco Editor**（VS Code と同じもの）で、Shark 用の色分けと
入力補完を付けてある。誤りの指摘は**本物の型検査**から出している。
出したり打ったりするところは**端末とおなじ**で、`input()` は打たれるまで待つ（下の「ターミナル」）。

```
make web                    # web/dist/ に作る（Emscripten が要る）
make web-serve              # 作ってから http://localhost:8000/ に配る
make web-serve PORT=8080    # 港（ポート）を変える
make web-test               # 作ったものを node で確かめる（画面は出さない）
```

作るのと配るのは別の手（`web/build.sh` と `web/serve.sh`）。
配る側は中で作る側を毎回呼ぶので、直したものがそのまま出る。
`file://` では `.wasm` を読めないので、見るときは配って開く。

初回だけ、Monaco Editor を npm から取り寄せて `web/vendor/` にためる（git には入れない）。
2回目からは取り寄せない。配るときに要るのは `web/dist/` だけで、
**動かすときに外の置き場は見に行かない**。

Emscripten が入っていないときは、`make web` が入れ方を出して止まる。

```
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest
```

## 中身

| ファイル | 何をするか |
|---|---|
| [`../core/platform/web.cpp`](../core/platform/web.cpp) | 移植層。ブラウザ向けに、メモリ・時間・入出力・ファイルを埋めたもの |
| [`../core/platform/screen_canvas.inc`](../core/platform/screen_canvas.inc) | 移植層の画面。窓をこしらえ、`std.ui` の面を canvas に出し、DOM の出来事を渡す（下） |
| [`../core/platform/font_canvas.inc`](../core/platform/font_canvas.inc) | 移植層の字。ブラウザに1文字ずつ描いてもらう（下） |
| [`shark_web.cpp`](shark_web.cpp) | ホスト。`Engine` を呼び、出力と診断を JavaScript に渡す |
| [`app.js`](app.js) | 画面。書くところの用意、実行の刻み、ターミナル（下）と診断の表示 |
| [`lang.js`](lang.js) | Monaco に Shark を教える。色分けと入力補完（下） |
| [`api.py`](api.py) | 補完に使う `api.js` を、[`../stdlib/`](../stdlib/README.md) の宣言ファイルから作る |
| [`index.html`](index.html) / [`style.css`](style.css) | 画面の骨と見た目 |
| [`build.sh`](build.sh) | 作る。emcc の呼び出しと Monaco の取り寄せ。`web/dist/` にまとめる |
| [`serve.sh`](serve.sh) | 配る。先に `build.sh` を呼んでから、`web/dist/` をその場で配る |
| [`test.js`](test.js) | できたものを node で確かめる |
| [`examples.py`](examples.py) / [`examples/`](examples) | お手本を `examples.js` にまとめる（下） |
| [`../docs/gen.py`](../docs/gen.py) | 説明（`dist/docs/`）を作る。中身は宣言ファイルと `docs/reference.md` が正（下） |

### 説明も一緒に配る

`make web` は、プレイグラウンドと一緒に**説明も `web/dist/docs/` に入れる**
（`docs/gen.py`。`make docs` が `docs/reference/` に作るのと同じもの）。
上の帯の「説明」（`Ctrl`（`⌘`）+ `I`）で**小窓**が開き、関数の一覧（宣言ファイルが正）と、
言語の使い方（[`../docs/reference.md`](../docs/reference.md) を HTML にしたもの）を
**書きながら**読める。別のタブに飛ばすと書いているものが見えなくなるので、
同じ画面に置ける形にしてある。頭をつかんで動かし、右下の角で大きさを変える。
置き場所・大きさ・開いていたかどうかは覚えておく。

配るものの中で閉じているので、**繋がっていなくても読める**。
外（リポジトリの中）へのリンクは作らない。作ったときに宣言と実装が食い違っていれば、
お手本の突き合わせと同じように `make web` が止まる。

### お手本

選ぶところに出すお手本は、[`examples.py`](examples.py) の `ITEMS` が正。
並べる順と、選ぶところに出す名前を決めたいので手で並べている。

そのぶん [`../examples/`](../examples) に足したものが `ITEMS` に入っていないと、
ブラウザからは見えないまま古くなる。なので作るときに**両方向で突き合わせて、
食い違っていれば止める**（`make web` が失敗する）。

- `../examples/*.shk` と `web/examples/*.shk` は、ぜんぶ `ITEMS` に入っていること
- `ITEMS` に書いたファイルは、あること

`web/examples/` にあるのは、ブラウザでしか意味のないもの（入力を読む、
止まらない繰り返し、ブラウザの字と変換を使う `ui.shk`）だけ。
それ以外は `../examples/` のものをそのまま載せる。

載ったものが**ブラウザの道でも読める**かは [`test.js`](test.js) が見ている。

```
        Shark のプログラム（プレイグラウンドに書くもの）
 ────────────────────────────────
   仮想マシン・型検査・標準ライブラリ        core/           ← どこでも同じ
 ────────────────────────────────
   移植層                                 platform/web.cpp  ← ブラウザ向けはこれ
 ────────────────────────────────
   ホスト                                 web/shark_web.cpp
   画面・書くところ                        web/app.js, web/lang.js
```

`core/` には手を入れていない。移植層を1つ足しただけで、
言語も標準ライブラリも `shark` コマンドと同じように動く
（[../spec/runtime/platform.md](../spec/runtime/platform.md)）。

## 固まらない仕組み

ブラウザは1本の流れで動くので、重い処理をそのまま走らせると画面ごと止まる。
Shark は**実行を刻んでホストに返す**ので、その心配がない
（[../spec/runtime/embedding.md](../spec/runtime/embedding.md)）。

```js
function tick() {
  const status = shk_pump(budget);   // budget 命令だけ進めて、必ず戻ってくる
  drainOutput();                     // その間に出た print を画面に出す
  if (status === 0) requestAnimationFrame(tick);   // 続きは次の描画で
}
```

`budget` は**命令の数**で、時間ではない。多くしても少なくしても結果は変わらず、
1回の描画で止まる時間だけが変わる。`app.js` は 6〜14 ミリ秒に収まるように増減させている。

止まらない繰り返しを書いても `status` は 0 のままなので、画面は動き続け、
「止める」（`shk_abort`）で終われる。

### ゲームの速さ

`requestAnimationFrame` なので、**プログラムが進むのは画面を描く合図ごとに1度だけ**
（ふつう 1 秒に 60 回）。ここで、くり返しの終わりに `sleep(0.016)` と書くと
**描くのにかかった分だけ足が出て**、1こま 16.7ms の合図に間に合わない。
1つ飛ばして**半分の速さ（30 fps）**になる。

待つのは `sleep()` ではなく **`ui.frame()`**。眠る長さではなく次のこまの
**刻限**を決めて待つので、足が出ない（[../spec/library/ui.md](../spec/library/ui.md)
「こまの速さ」）。

この移植層は `PlatformScreen::host_paced` を `true` にして「刻みはこちらが握っている」と
名乗る（[../core/platform/screen_canvas.inc](../core/platform/screen_canvas.inc)）。
`ui.frame()` はそれを見て**刻限の半こま手前で起きる**ので、合図に間に合う。
ここを Shark の側で当て推量させないための口で、120Hz の画面でも同じように効く。

## ターミナル

右がわは**端末とおなじ**にしてある。出力も、打った文字も1本の流れに並び、
出るものは `shark` コマンド（[../frontend/main.cpp](../frontend/main.cpp)）と同じ形。

```
$ shark run playground.shk
名前を教えてください
さめ                        ← ここで打つ。打った行はそのまま流れに残る
こんにちは、さめ さん！
$ 
```

| すること | どうする |
|---|---|
| プログラムに答える | そのまま打って <kbd>Enter</kbd>。`input()` は**打たれるまで待つ** |
| 入力の終わり | <kbd>Ctrl</kbd> + <kbd>D</kbd>。その `input()` は `none` になる |
| 止める | <kbd>Ctrl</kbd> + <kbd>C</kbd>（「止める」ボタンと同じ） |
| 流れを消す | <kbd>Ctrl</kbd> + <kbd>L</kbd> / <kbd>Ctrl</kbd> + <kbd>U</kbd> は打ちかけの行を消す |
| 打った覚え | <kbd>↑</kbd> <kbd>↓</kbd> |

ボタンを押さずに、`run` `check` `test` `explain E0102` `modules` `version`
`clear` `help` と打っても動く（`shark run playground.shk` のように書いてもよい）。
`--lang` `--memory` `--strict` `--no-color` も端末と同じに効く。
開けるファイルは `playground.shk`（左に書いているもの）だけ。

打つ文字を受けるのは、印（カーソル）の場所に置いた見えない `textarea`。
かな漢字変換の窓がそこに出るようにするためで、`input()` に日本語を渡せる。

### 待てないところで待つ

ブラウザは止まって待てないので、`input()` は**待ちに入って刻みをホストに返す**
（[../spec/runtime/embedding.md](../spec/runtime/embedding.md)）。
`HostIO::input_ready` が「まだ来ていない」と答える間、`shk_waiting_input()` が 1 になり、
画面はそれを見て入力を促す。行が来れば、そのまま続きから動く。
`sleep` や `task` は待っている間も動く。

## 入力補完（IntelliSense）

| できること | 中身 |
|---|---|
| 色分け | 予約語・f 文字列の `{ }`・入れ子コメントまで |
| 入力候補 | `math.` でモジュールの関数、`xs.` で型のメソッド、ふつうの位置では予約語・雛形・書いた関数や変数 |
| 受け継いだメンバ | 子の実体でも、親の `public` なメンバとメソッドが `.` の候補に出る |
| 親の関数を上書き | クラスの中では、継承元の `virtual` が `override` の雛形として出る（下） |
| 説明（hover） | 署名と、仕様書に書いてある日本語の説明 |
| 引数の案内 | `(` を打つと引数が出る。オーバーロードも並ぶ |
| 定義へ飛ぶ | 同じ画面に書いた関数・クラスへ（F12） |
| 関数を探す | Ctrl（⌘）+ Shift + O |
| 誤りの指摘 | 打つ手が止まると**本物の型検査**が走り、波線と番号が付く |
| import の書き足し | `text` を選ぶと `import std.text;` を一緒に書き足す |

補完に出す標準ライブラリの表（`api.js`）は、[`api.py`](api.py) が
[`../stdlib/*.shk`](../stdlib/README.md)（宣言ファイル）から作る。手で書いた一覧は持たない。
HTML のリファレンス（`make docs`）と**同じ出どころ**なので、説明も例も食い違わない。

| 宣言ファイルに書いたもの | 補完でどう出るか |
|---|---|
| 署名（`func sqrt(x: float) -> float;`） | 候補の型と、引数の案内 |
| `///` の説明 | hover と候補の説明 |
| `引数:` の節 | 引数の案内で、いま打っている引数の説明 |
| `例:` の節 | hover と引数の案内に出る、動く例 |

`api.py` は作るときに実装（`core/lib/*.cpp`）と突き合わせ、
宣言と実装が食い違っていれば知らせる。仕様書にあっても実装に入っていないもの
（`std.net` など）は宣言が無いので補完にも出ない。
`make web-test` が、`api.js` の一覧と処理系が持つモジュールの一致を確かめている。

### 親の関数を上書きする

`class Shark : Fish {` の中で候補を出すと、`Fish` から受け継いだ関数が並ぶ。
選ぶと、`override` の付いた雛形がそのまま入る。

```shark
class Shark : Fish {
  // ここで describe を選ぶと ↓ が入る
  public override func describe() -> string {
    ⏐
    return super.describe();
  }
}
```

| 見ているところ | 雛形への出かた |
|---|---|
| 上書きしてよい関数 | `virtual` と `override` だけ。ふつうの `func` は出ない |
| もう書いた関数 | 出ない（`init` も出ない） |
| 親の親 | たどって集める。インタフェース（`Comparable` など）も同じ |
| 本体の無い `virtual`（純粋仮想） | 空の本体と、戻り値に合う値（`int` なら `return 0;`）。先に出す |
| 本体のある `virtual` | `super` を呼ぶところまで書く |
| 親の `public` | 引き継ぐ。落とすと、その関数は子の中からしか呼べなくなる |
| インタフェースの `This` | 自分のクラス名に置き換える |

`override` や `public` と打った後に候補を出すと、その分は重ねずに続きだけを入れる。
メソッドの本体の中では出さない（`{ }` を数えて、クラスの直下かどうかを見ている）。

親をたどるのは `.` の後ろも同じで、子の実体からは親の `public` なメンバも候補に出る
（`private` は出ない）。子で上書きした関数は、子の方だけが出る。

変数の型は `lang.js` が軽く見当をつける（宣言の型注釈、リテラル、
`time.now()` のような呼び出しの戻り値、`for var x in xs` の中身）。
細かい判定はしない。**正しいかどうかを決めるのは、いつでもコアの型検査**。

## ホストの入口

`shark_web.cpp` が出している関数。自分のページに組み込むときはこれを呼ぶ。

| 関数 | 内容 |
|---|---|
| `shk_boot()` | 最初に1度。移植層を差し込む |
| `shk_config(memory_mb, lang_en, strict)` | 次の読み込みから使う設定 |
| `shk_add_module(path, source)` | `import` で使えるようにしておくもの |
| `shk_load(name, source)` | 読み込む。誤りの数を返す |
| `shk_diagnostics()` | 直前の読み込みの診断（JSON） |
| `shk_start_run()` / `shk_start_test()` | 実行を始める / `test_` を走らせ始める |
| `shk_pump(budget)` | `budget` 命令だけ進める。0=続く 1=終わり 2=止まった |
| `shk_abort()` / `shk_idle()` | 止める / 待ちに入っているか |
| `shk_out_ptr()` `shk_out_len()` `shk_out_clear()` | `print` の出力（UTF-8 のまま受け取る） |
| `shk_push_input(text)` | `input()` に返す行を渡す（打たれたそばから呼ぶ） |
| `shk_push_eof()` | もう入力は無いと伝える（端末の Ctrl + D）。次の `input()` が `none` になる |
| `shk_waiting_input()` | `input()` が行を待って止まっているか |
| `shk_error()` | 止まった理由（JSON。理由・場所・呼び出しの経路） |
| `shk_exit_code()` / `shk_test_passed()` / `shk_test_total()` | 終了コード / テストの結果 |
| `shk_memory_used()` / `shk_memory_limit()` | 使っている量 / 上限（バイト） |
| `shk_format(source)` / `shk_formatted()` | 見た目を整えたソース / 整えられたか（`shark fmt` と同じもの） |
| `shk_modules()` / `shk_explain(code)` / `shk_version()` | モジュールの一覧 / 番号の説明 / 版 |

いちばん短い使い方:

```html
<script src="shark.js"></script>
<script>
createShark().then((M) => {
  const call = (name, ret, args) => M.cwrap(name, ret, args);
  call('shk_boot', null, [])();
  call('shk_config', null, ['number', 'number', 'number'])(64, 0, 0);

  const errs = call('shk_load', 'number', ['string', 'string'])('hello.shk', 'print("やあ");');
  if (errs > 0) {
    console.log(JSON.parse(call('shk_diagnostics', 'string', [])()));
    return;
  }
  call('shk_start_run', 'number', [])();

  const pump = call('shk_pump', 'number', ['number']);
  (function tick() {
    const st = pump(200000);
    const len = M._shk_out_len();
    if (len) {
      const p = M._shk_out_ptr();
      console.log(new TextDecoder().decode(M.HEAPU8.slice(p, p + len)));
      M._shk_out_clear();
    }
    if (st === 0) requestAnimationFrame(tick);
  })();
});
</script>
```

出力を「番地と長さ」で受け取るのは、UTF-8 の途中で切れても崩れないようにするため。
`shk_out_*` を呼んだ直後にその場で写して使う（メモリが伸びると番地が変わる）。

## 画面（std.ui）

`ui.open()` を呼ぶと**窓が出る**。名札の帯を引けば動き、右下の隅を引けば大きさが変わり、
× で閉じる ― 机の上の窓と同じ。見た目は **Dear ImGui** に寄せてある（角は立てたまま、
細い縁、濃い青の帯、右下に三角の持ち手。帯の ▼ でたためて、触っていない窓の帯は黒に近くなる）。
こしらえるのは移植層
（[`../core/platform/screen_canvas.inc`](../core/platform/screen_canvas.inc)）で、
コアは今までどおり面（画素の並び）に描くだけ。**描くところは何も変わらない**
（[../spec/library/ui.md](../spec/library/ui.md)）。

ページの中に埋め込みたいときは、置き場を用意しておくと窓を作らずそこに面だけ出す。

| 順 | 出す先 |
|---|---|
| 1 | `Module.sharkMount`（要素そのものか、`querySelector` に渡す文字列） |
| 2 | `#shark-screen` という要素 |
| 3 | どちらも無ければ**窓をこしらえる**（プレイグラウンドはこれ） |

置き場の中身は開くたびに空にするので、**そこには他のものを置かない**。
開け閉めは `window` の出来事で知らせる。

```js
window.addEventListener('shark:screen-open', function (e) {
  e.detail.canvas;          // できた canvas
  e.detail.width;           // 面の大きさ（画素）
  e.detail.requestClose();  // 窓の × にあたる。ui.poll() が false を返す
});
window.addEventListener('shark:screen-close', function (e) { /* … */ });
```

| もの | ブラウザでは |
|---|---|
| 面の大きさ | `ui.open(横, 縦)` のまま。面の1画素は画面の1画素。小さい面（640×480 未満）だけ整数倍に引き伸ばし、置き場に入りきらないときは縦横の比を保って縮める |
| 細かい画面（HiDPI） | `ui.pixel_ratio()` が `devicePixelRatio` そのまま（1.25 や 1.5 もある）、`ui.scale()` はそれを整数に丸めたもの。どちらも開く前に呼べる。canvas の CSS の大きさは**丸めない数**で割るので、canvas の1画素はいつも画面の1画素。拡大や別の画面への移動で細かさが変わったら、面を取り直す |
| 大きさを変える | 隅の持ち手（埋め込みなら置き場の大きさ）が変わると `SEV_Resize`。窓の縁を引いたのと同じで、面が作り直される（`ui.open(…, false)` なら持ち手を出さない） |
| キー | `keydown` / `keyup`。矢印・空白・Tab などはブラウザの既定の動きを止める。焦点が外れたら、押しっぱなしのキーは離したことにする |
| マウス | `pointer*` で受けるので、指でも同じように届く。右で押すのは `ui.menu` のもの（ブラウザのメニューは出さない） |
| 文字入力 | `ui.field` の間だけ、見えない `textarea` に任せる。**かな漢字変換はブラウザのものがそのまま使える**（変換中は `ui.marked()`） |
| マウスの形 | `ui.cursor()` が CSS の `cursor` になる。押せるところ（ボタン・つまみ）では手、入力欄では文字の形に、宣言的な層が自分で変える |
| 字 | **ブラウザに描いてもらう**（下）。`ui.font()` で日本語もそのまま出る |
| 閉じる | 窓の × で `SEV_Close`。埋め込んだときはホストが `requestClose()` を呼ぶ |

`SHARK_UI=off` を渡しておくと画面を開かず、見えない面に描く（`ui.get()` と `ui.to_png()` で取れる）。
node で動かしたときも同じで、`document` が無ければ `ui.visible()` は false になる。

## 字（std.ui）

ブラウザの中からはフォントのファイルを読めないので、FreeType は使えない。
かわりに**ブラウザに1文字ずつ描いてもらい、その濃さを写し取る**
（[`../core/platform/font_canvas.inc`](../core/platform/font_canvas.inc)）。
返す形は FreeType と同じなので、コアから見れば機種のフォントが読めたのと変わらない。

```shark
var k = ui.scale();
_ = ui.font(12 * k);        // ここでブラウザの字に切り替わる（ui.run は自分で呼ぶ）
ui.text(4, 4, "こんにちは", ui.rgb(255, 255, 255));
print(ui.font_name());      // 例: Hiragino Sans
```

使う字は、Windows・macOS・Linux のどれにも**はじめから入っている**ものを選んである。
canvas は字ごとに、持っているものへ落ちていく。

| 機種 | 選んだもの |
|---|---|
| macOS / iOS | Hiragino Sans、Hiragino Kaku Gothic ProN |
| Windows | Yu Gothic UI、Yu Gothic、Meiryo、MS Gothic |
| Linux / Android / ChromeOS | Noto Sans CJK JP、Noto Sans JP、IPAexGothic、IPAGothic、VL Gothic、Droid Sans Japanese |
| 最後の受け皿 | system-ui、sans-serif |

- `ui.font_name()` は、**実際にその機械にあったもの**の名前を返す。
  ブラウザは `document.fonts.check()` に何でも true と答えるので、
  幅を比べて（`monospace` と並べて測って）確かめている
- `ui.font(名前, 大きさ)` は、ブラウザではファイルの場所ではなく**フォントの名前**として渡る
  （`ui.font("Meiryo", 16)`。その機械に無ければ false）
- 一度描いた字形は覚えておく（文字と大きさが鍵）。日本語をたくさん出しても、
  描き直すのは初回だけ
- 絵文字は色が付かない（濃さだけを写し取るので、影のように出る）

## ブラウザでできないこと

| できないこと | どうなるか |
|---|---|
| 外のファイルを読む | `std.file` が触るのはタブの中だけの仮の置き場（Emscripten の MEMFS）。閉じると消える |
| 外のプログラムを呼ぶ | `os.run()` は失敗を返す（[../spec/library/os.md](../spec/library/os.md) のとおり `Result` で受け取れる） |
| その場で待つ | 移植層の `sleep` は何もしない。`sleep()` はタスクを譲るだけで、実時間は描画の刻みで進む |
| `std.net` `std.http` | もともとこの実装に入っていない（[../docs/implementation.md](../docs/implementation.md)） |
| フォントのファイルを読む | ブラウザの中からは読めない。`ui.font(中身, 大きさ)` は false を返す（上の「字」のとおり、描くのはブラウザに頼む） |
| 切り貼りの置き場を読む | ブラウザからは勝手に読めない。`ui.clipboard()` が返すのは、貼り付け（Ctrl+V）で届いたものと、自分で `ui.set_clipboard()` に入れたもの |

`os.platform()` は `"wasm"` を返す。

## 大きさ

| もの | そのまま | gzip |
|---|---|---|
| `shark.wasm`（処理系） | 938 KB | 290 KB |
| `shark.js`（つなぎ） | 87 KB | 25 KB |
| `vendor/vs`（Monaco Editor） | 4.2 MB | 1.2 MB |
| 画面まわり（`app.js` `lang.js` `api.js` ほか） | 236 KB | 60 KB |

Monaco は配られているもののうち、**書くところと入力候補まわりだけ**を残している
（他言語の色分けや TypeScript の言語サービスは外す。24 MB → 4.2 MB）。

`web/dist/` はそのまま静的な置き場に置けば動く（サーバ側の処理は要らない）。
`file://` では `.wasm` を読めないので、配って開く。
