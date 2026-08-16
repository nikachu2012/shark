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
| [`shark_web.cpp`](shark_web.cpp) | ホスト。`Engine` を呼び、出力と診断を JavaScript に渡す |
| [`app.js`](app.js) | 画面。書くところの用意、実行の刻み、ターミナル（下）と診断の表示 |
| [`lang.js`](lang.js) | Monaco に Shark を教える。色分けと入力補完（下） |
| [`api.py`](api.py) | 補完に使う API の表（`api.js`）を、仕様書と実装から作る |
| [`index.html`](index.html) / [`style.css`](style.css) | 画面の骨と見た目 |
| [`build.sh`](build.sh) | 作る。emcc の呼び出しと Monaco の取り寄せ。`web/dist/` にまとめる |
| [`serve.sh`](serve.sh) | 配る。先に `build.sh` を呼んでから、`web/dist/` をその場で配る |
| [`test.js`](test.js) | できたものを node で確かめる |
| [`examples.py`](examples.py) / [`examples/`](examples) | お手本を `examples.js` にまとめる |

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
| 説明（hover） | 署名と、仕様書に書いてある日本語の説明 |
| 引数の案内 | `(` を打つと引数が出る。オーバーロードも並ぶ |
| 定義へ飛ぶ | 同じ画面に書いた関数・クラスへ（F12） |
| 関数を探す | Ctrl（⌘）+ Shift + O |
| 誤りの指摘 | 打つ手が止まると**本物の型検査**が走り、波線と番号が付く |
| import の書き足し | `text` を選ぶと `import std.text;` を一緒に書き足す |

補完に出す標準ライブラリの表（`api.js`）は、[`api.py`](api.py) が**3つの出どころ**を
突き合わせて作る。手で書いた一覧は持たない。

| 出どころ | 何を取るか |
|---|---|
| `core/lib/*.cpp` の `r.add("...")` | **この処理系が本当に持っている関数**の名前 |
| `core/check.cpp` のメソッドの表 | 型ごとのメソッドと、引数・戻り値の型 |
| `spec/library/*.md` `spec/types/*.md` の表 | 引数の名前と、日本語の説明 |

仕様書にあっても実装に入っていないもの（`std.net` など）は補完に出ない。
`make web-test` が、`api.js` の一覧と処理系が持つモジュールの一致を確かめている。

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

## ブラウザでできないこと

| できないこと | どうなるか |
|---|---|
| 外のファイルを読む | `std.file` が触るのはタブの中だけの仮の置き場（Emscripten の MEMFS）。閉じると消える |
| 外のプログラムを呼ぶ | `os.run()` は失敗を返す（[../spec/library/os.md](../spec/library/os.md) のとおり `Result` で受け取れる） |
| その場で待つ | 移植層の `sleep` は何もしない。`sleep()` はタスクを譲るだけで、実時間は描画の刻みで進む |
| `std.net` `std.http` `std.ui` | もともとこの実装に入っていない（[../docs/implementation.md](../docs/implementation.md)） |

`os.platform()` は `"wasm"` を返す。

## 大きさ

| もの | そのまま | gzip |
|---|---|---|
| `shark.wasm`（処理系） | 741 KB | 233 KB |
| `shark.js`（つなぎ） | 72 KB | 19 KB |
| `vendor/vs`（Monaco Editor） | 4.2 MB | 1.2 MB |
| 画面まわり（`app.js` `lang.js` `api.js` ほか） | 110 KB | 28 KB |

Monaco は配られているもののうち、**書くところと入力候補まわりだけ**を残している
（他言語の色分けや TypeScript の言語サービスは外す。24 MB → 4.2 MB）。

`web/dist/` はそのまま静的な置き場に置けば動く（サーバ側の処理は要らない）。
`file://` では `.wasm` を読めないので、配って開く。
