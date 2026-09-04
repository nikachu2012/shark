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
make                              # コアと shark コマンド、実行装置を作る（外部依存なし）
                                  #   日本語の字を出すときだけ FreeType が要る（下）
./shark run examples/hello.shk    # Hello, Shark!
make test                         # tests/ を走らせる
make embed && ./examples/embed/game   # ゲームに組み込む例
./examples/embed/play_stage           # バイトコードを焼き込んだ例（前側を持たない）
```

```
./shark run <file.shk>     実行する（.shkc を渡すと、保存したバイトコードを動かす）
./shark check <file.shk>   型検査だけ
./shark build <file.shk>   処理系ごと1つにまとめる（下の「1つのファイルにして配る」）
./shark test [file.shk]    test_ で始まる関数を走らせる（省くと *_test.shk 全部）
./shark fmt <file.shk>…    見た目を整える（-w で書き換え、--check で確かめるだけ）
./shark explain E0102      エラーの詳しい説明
./shark modules            この処理系が持つモジュールの一覧

  --memory <MB>            使ってよいメモリの量。超えたら実行時エラー（既定 256）
  --lang ja|en / --strict  診断の言語 / 警告をエラーとして扱う
```

## Windows で作る

Windows には `make` が無いので、代わりに `tools\build_win.bat` を使う。
**要るのは Visual Studio の C++ だけ**で、外のライブラリは1つも要らない
（窓は `user32.dll` を実行時に取りに行く）。

```
tools\build_win.bat            shark.exe と sharkvm.exe を作る
tools\build_win.bat freetype   日本語の字を出す FreeType を足す（一度だけ。下）
tools\build_win.bat test       作ってから tests\ を走らせる（sh が要る）
tools\build_win.bat clean      作ったものを消す

.\shark.exe run examples\hello.shk
```

| | |
|---|---|
| コンパイラ | [Visual Studio](https://visualstudio.microsoft.com/) 2019 以降の「**C++ によるデスクトップ開発**」（Build Tools だけでもよい）。置き場所は `vswhere` が自動で探す |
| `test` に要るもの | `sh`（[Git for Windows](https://gitforwindows.org/) に付いてくる）。無いときは作るところまでは動く |
| MSYS2 / MinGW を使うなら | `make` がそのまま動く（`.exe` の付け外しは Makefile が面倒を見る） |

- **日本語はそのまま通る。**診断も `print` も UTF-8 で出て、`shark run 日本語.shk` のような
  ファイル名も渡せる（端末の符号と命令行を、起動のときに UTF-8 にそろえている）
- `shark build` が作るのは `.exe`。名前に付いていなければ自動で足す
- 窓（`std.ui`）は Win32 の窓が開く。キー・文字・マウス・大きさの変更・× で閉じるが届き、
  切り貼りの置き場（クリップボード）とマウスの形も使える。
  画面の細かさ（HiDPI）は OS に尋ねて `ui.scale()` が返す
- **日本語の変換（IME）もそのまま使える。**変換中の字は入力欄に下線つきで出て、
  候補の一覧は Windows のものが入力欄のすぐ下に出る。確定するとその字が入る。
  入力欄では ctrl+A / ctrl+C / ctrl+V / ctrl+X も効く
- 日本語の**字形**は同梱の Noto Sans JP で出る。そのためには FreeType を足しておく
  （`tools\build_win.bat freetype`。下の「日本語の字を出す」）。
  足さないと窓の中の日本語は □ になる

## ブラウザで動かす

同じコアを WebAssembly にしたものが [web/](web/README.md) にある。
書いて動かすところまで、タブの中だけで完結する（何も外に送らない）。

```
make web            # web/dist/ に作る（Emscripten が要る）
make web-serve      # 作ってから http://localhost:8000/ に配る
make web-test       # 作ったものを node で確かめる
```

- 移植層（`core/platform/web.cpp`）を1つ足しただけ。言語も標準ライブラリもそのまま動く
- `std.ui` はブラウザの中に**窓を出す**（帯を引けば動き、隅で大きさが変わり、× で閉じる）。
  日本語もそのまま出る（字はブラウザに描いてもらう → [web/README.md](web/README.md)）
- **ゲームもそのまま動く。**お手本から 2D（ブロック崩し）と 3D（回る立方体）が選べる。
  キーもマウスも面に届き、`ui.frame()` が刻みを合わせるので、`shark` コマンドと同じ速さで動く
- 実行は刻んでホストに返るので、**止まらない繰り返しを書いてもブラウザは固まらない**。
  ゲームに組み込むときと同じ仕組み
- 書くところは Monaco Editor。入力候補・説明・引数の案内が出て、
  誤りの波線は**本物の型検査**から引いている
- 出し入れは**端末とおなじ**。出力も打った文字も1本の流れに並び、`input()` は打たれるまで待つ。
  出る形（診断・panic・テストの結果）も `shark` コマンドと同じ
- **説明も一緒に入る。**上の帯の「説明」（`Ctrl`（`⌘`）+ `I`）で**小窓**が開き、
  関数の一覧と言語の使い方を**書きながら**読める。頭をつかんで動かし、右下の角で大きさを変える
  （`web/dist/docs/`。`make docs` が作るのと同じもの）。配るものの中で閉じているので、
  繋がっていなくても読める
- `shark.wasm` は 940 KB（gzip 290 KB）。置き場に置くだけで動き、サーバ側の処理は要らない

## 画面に描く

`std.ui` には層が2つある。どちらも描くのはこの処理系の中で、
外の描画ライブラリにもフォントにも、**窓の道具にも頼らない**。

```
./shark run examples/paint.shk       # 下の層。マウスで描く
./shark run examples/node_editor.shk # 下の層。ノードをつなぐと Shark のコードになる
./shark run examples/counter.shk     # 上の層。部品を組んで返す
./shark run examples/widgets.shk     # 上の層。部品をぜんぶ1つの画面に出す
./shark run examples/breakout.shk    # ブロック崩し。絵（Canvas）と透明を使う
./shark run examples/cube3d.shk      # 回る立方体。三角形と奥行き（z バッファ）を使う
```

**下の層**は、画素の並び1枚（面）と、押された・動いたという出来事だけ。

```shark
import std.ui;

ui.open("さかな", 160, 120);   // 面は画素の並び。細かい画面なら ui.scale() を掛ける
while ui.poll() {
  if ui.pressed("esc") { ui.quit(); }
  ui.clear(ui.rgb(0, 20, 40));
  ui.fill_circle(ui.mouse_x(), ui.mouse_y(), 12, ui.rgb(255, 140, 60));
  ui.present();
  ui.frame();                  // 次のこまの刻みまで待つ（sleep で待つと足が出る）
}
```

**上の層**は、「いまどうあるべきか」を **`Widget` 1つに組んで返す**だけ。
くり返しも描き直しも `ui.run()` が引き受けるので、**書くのは「いまの姿」1つ**で済む。

```shark
var count = 0;                        // 状態はふつうの変数

func view() -> Widget {               // いまどうあるべきか
  return ui.col([                     // 縦に並べる。横は ui.row、格子は ui.grid
    ui.label(f"{count} 回"),
    ui.row([
      ui.button("ふやす", func() -> void { count += 1; }),   // 押されたときの動き
      ui.button("へらす", func() -> void { count -= 1; }),
    ]),
  ]);
}

ui.run("かうんた", 420, 300, view);
```

- 呼び出しにブロックを続ける記法は言語に無いので、入れ子は `ui.col` / `ui.row` / `ui.grid` に**配列**で渡して表す
- **押されたときの動きは部品に持たせる。**名前を付けた関数を渡してもよいし、
  まとめて振り分けたいときは、名札を渡して `update(hit)` で受けてもよい
- 部品は状態を持たない。値は呼んだ側が持って毎回渡し直すので、
  **「いまの状態」と「画面」が食い違わない**
- 見た目は鎖でつないで変える。`ui.label("さめ").color(c).padding(6)`
- `ui.run()` は**処理系の中で Shark 自身で書いてある**。特別な仕掛けは無く、
  下の層（`ui.poll` / `ui.show` / `ui.present`）を呼んでいるだけ

### 出し先

`shark` コマンドは、出せるところに出す。

| | |
|---|---|
| macOS | 窓（AppKit） |
| Windows | 窓（Win32 + GDI） |
| Linux ほか | 窓（X11） |
| 窓が開けないところ | 見えない面に描く。結果は `ui.get()` と `ui.to_png()` で取れる |

面の1画素は画面の1画素にそのまま乗る。**細かい画面（HiDPI）では面も細かく取る。**

```shark
var k = ui.scale();                  // ふつうは 1、Retina なら 2
ui.open("さめ", 420 * k, 300 * k);   // 見た目の大きさは変わらず、中身が細かくなる
_ = ui.font(12 * k);                 // 12pt くらい
```

- 窓に要る関数は**実行時に**取りに行く（`dlopen` / `LoadLibrary`）ので、
  **作るときに要るライブラリは無い**。X11 の無い機械でもそのまま作れる
- 画面が無くても同じように動くので、**画面の要るプログラムでもテストが書ける**
- `SHARK_UI=off` で窓を開かず、見えない面に描かせられる
- 移植層に求めるのは「面を出す」と「出来事を渡す」の2つだけ。
  ゲームに組み込むときは、その面をゲーム本体が受け取る
  （[spec/library/ui.md](spec/library/ui.md)）

内蔵の字形は ASCII だけ。日本語などは □ になる。

## 日本語の字を出す（FreeType）

内蔵の字形は ASCII だけ。日本語などを出すには **FreeType** が要る。
これが**唯一の外部ライブラリ**で、**任意**。入れなくても処理系は作れて動く
（日本語が □ になるだけ）。日本語の字形を自前で抱えると処理系が数百 KB 太るので、
ここだけ外に頼ることにした（[spec/library/ui.md](spec/library/ui.md)）。

**フォントは同梱してある。**`assets/fonts/NotoSansJP-Regular.otf`
（Noto Sans JP Regular / [SIL Open Font License 1.1](assets/fonts/LICENSE-NotoSansJP.txt)）で、
`shark` は実行ファイルの隣にあるこれを既定にする。
機種に何が入っているかを気にせず、どこでも同じ字で出る。
別のものを使いたいときは、環境変数 `SHARK_FONT` にその場所を入れる
（そちらが勝つ）か、`ui.font(path, size)` で直に渡す。

### 1. FreeType を入れる

| | |
|---|---|
| Windows（Visual Studio） | `tools\build_win.bat freetype`（元を取ってきて静的に作る。ほかに要るものは無い） |
| macOS | `brew install freetype` |
| Debian / Ubuntu | `sudo apt install libfreetype-dev pkg-config` |
| Fedora / RHEL | `sudo dnf install freetype-devel pkgconf-pkg-config` |
| Arch | `sudo pacman -S freetype2 pkgconf` |
| Windows（MSYS2） | `pacman -S mingw-w64-x86_64-freetype mingw-w64-x86_64-pkgconf` |

### 2. 作り直す

`make` が `pkg-config` で見つけて、自動で使う。

```
make clean && make          # 見つかれば FreeType つきで作られる
```

| したいこと | |
|---|---|
| 入っていても使わない | `make FREETYPE=0` |
| pkg-config が無い | `make FREETYPE=1 FT_CFLAGS=-I/opt/freetype/include/freetype2 FT_LIBS="-L/opt/freetype/lib -lfreetype"` |
| Windows（Visual Studio） | `tools\build_win.bat freetype` のあと `tools\build_win.bat`。すでに手元にあるものを使うなら `tools\build_win.bat build -FtInclude <include> -FtLib <freetype.lib>` |
| 使われているか見る | `./shark run examples/counter.shk`（日本語が出れば入っている） |

`make clean` を挟むのは、`make` が「作るときの指定が変わったこと」までは見ないため。

### 3. プログラムから読む

**何もしなければ内蔵の字形のまま。**読むかどうかは書く人が決める。

```shark
import std.ui;

var k = ui.scale();                   // 細かい画面（Retina）なら 2
ui.open("さめ", 420 * k, 300 * k);
if !ui.font(12 * k) {                 // 12pt くらい。画素で渡す
  print("フォントが見つかりません");
}
ui.text(8 * k, 8 * k, "こんにちは", ui.rgb(255, 255, 255));
```

探す順番は、環境変数 `SHARK_FONT` → 機種によくある場所。
`shark` コマンドは、`SHARK_FONT` が空なら**同梱の Noto Sans JP**（実行ファイルの隣の
`assets/fonts/`）をそこに入れるので、何もしなければそれが使われる。
同梱のものが無いときは機種によくある場所を見る
（macOS はヒラギノ角ゴシック W4、Windows は游ゴシック、Linux は Noto Sans CJK Regular）。
探すのは**本文の太さ**で、別の太さが要るときは自分で選ぶ。

### 持っていない字は、控えのフォントから（フォールバック）

1本のフォントに世界中の字は入っていない。同梱の Noto Sans JP も、日本語と英数字は
持っているが**絵文字やハングルは持っていない**。そこで、いま使っているフォントに
無い字が来たら、**機種のフォントから順に探す**。

```shark
_ = ui.font(20);
ui.text(10, 10, "日本語 ✓ 한국어 🦈", ui.rgb(0, 0, 0));   // ぜんぶ出る
```

- 控えは**要るときに1本ずつ**読む。足りているうちは1本も読まない
- 探すのは `SHARK_FONT_FALLBACK`（`;` 区切り。使う人が決めたものが勝つ）→
  機種のフォント（記号 → 絵文字 → ハングル → 日本語 の順）
- どの控えも持っていなければ □ になる。無いことが見て分かるように、そのまま出す
- 行の高さと基準線は**本命のフォント**で決まるので、控えが混じっても行が揺れない
- 読んだフォントはそのまま抱えるので、**そのぶんメモリが要る**（4本まで）。
  Windows で測ると、日本語だけなら 17 MB、絵文字を出すと 39 MB。
  絵文字のフォントが 12 MB あるためで、使わなければ増えない
- **色は付かない。**絵文字は形（白黒）で出る。字の色は `ui.text()` に渡した色になる

```shark
_ = ui.font("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 15);
_ = ui.font(data, 15);     // 自分で読んだ中身（bytes）から
ui.font_builtin();          // 内蔵の 5×7 に戻す
print(ui.font_name());      // いま使っているもの。内蔵なら空
```

### 気をつけること

- **字の幅と高さは、その機種のフォント次第**になる。`ui.text_width()` の答えも変わる。
  内蔵の字形のままなら、どの機種でも同じ形・同じ大きさで出る。
  だから**既定は内蔵**で、`ui.font()` を呼んだときだけ切り替わる
- 部品（ボタンなど）の寸法は字の大きさから決まるので、`ui.font()` を変えるだけで
  全体の釣り合いが付いてくる
- `shark build` で作った単一バイナリは、FreeType つきで作ったなら
  配る先にも FreeType が要る（`make FREETYPE=0` で作れば要らない）。
  Windows の `tools\build_win.bat freetype` は**静的に**繋ぐので、これは要らない
- 同梱のフォントを一緒に配るときは、`assets/fonts/` を実行ファイルの隣に置く。
  置かないときは機種のフォントに落ちる（見つからなければ日本語は □）
- **絵文字は同梱のフォントには入っていない。**機種の絵文字フォントを控えから読むので
  形は出るが、**色は付かない**（上の「控えのフォントから」）
- ブラウザ版（`make web`）とゲーム機向けの雛形には FreeType を入れない。
  ブラウザは**ブラウザ自身に字を描いてもらう**ので、`ui.font()` はそのまま使えて日本語も出る
  （移植層の `PlatformFont`。ゲーム機向けの雛形は内蔵の字形だけ）

## 押すたびの検査と、配りかた（CI/CD）

`.github/workflows/` に3つ置いてある。

| 台本 | いつ | すること |
|---|---|---|
| `ci.yml` | main への push と pull request | macOS・Linux・Windows で作って `make test`、`make docs`、`make docs-check` |
| `pages.yml` | main への push | ブラウザ版（WebAssembly）を作って **Cloudflare Pages** に載せる |
| `release.yml` | `v` で始まるタグを押したとき | 4つの機種の実行ファイルを作って、Release に付ける |

どれも FreeType を**元から静的に作って**繋ぐ（`tools/freetype_static.sh`、
Windows は `tools\build_win.bat freetype`）。配るものは、それだけで動く1つの
実行ファイルになり、相手の機械には何も入れてもらわなくてよい。

```
sh tools/freetype_static.sh                    # 元から静的に作る（一度だけ）
make $(sh tools/freetype_static.sh --flags)    # それを繋いで作る
```

### できたものを取る

押すたびの検査（`ci.yml`）は、4つの機種ぶんを包んで置いていく。
GitHub の **Actions → その実行 → Artifacts** から落とせる（14 日で消える）。
ブラウザ版も同じで、`pages.yml` の実行に `shark-web`（`web/dist` そのもの）が付く。

| 名前 | 中身 |
|---|---|
| `shark-macos-arm64` / `shark-macos-x86_64` | macOS の実行ファイル一式 |
| `shark-linux-x86_64` | Linux の実行ファイル一式 |
| `shark-windows-x86_64` | Windows の実行ファイル一式 |
| `shark-web` | ブラウザ版（そのまま静的に配れる） |

同じ包みは手元でも作れる。中身は実行ファイル（`shark` / `sharkvm`）と、
同梱のフォント・見本・説明（`docs/`。ブラウザ版に入れるのと同じもの）・README で、
**入れてもらうものは何も無い**。

```
make dist                    # → dist/shark-<機種>.tar.gz（Windows は .zip）
make dist NAME=macos-arm64   # 機種の名前を自分で決める
```

名札（タグ）を押したときは、同じものが Release にも付く。

Cloudflare Pages に載せるには、リポジトリの Settings → Secrets and variables →
Actions に2つ入れておく。無ければ、作るところまでで止まる（載せない）。

| 秘密 | 中身 |
|---|---|
| `CLOUDFLARE_API_TOKEN` | Pages を編集できる合鍵（Cloudflare の My Profile → API Tokens） |
| `CLOUDFLARE_ACCOUNT_ID` | Cloudflare の口座の番号（ダッシュボードの右下） |

Pages のプロジェクト名は `pages.yml` の `CF_PAGES_PROJECT`（既定は `shark`）。
Cloudflare 側に同じ名前で先に作っておく。

## 1つのファイルにして配る

`shark build` は、書いたプログラムを**それだけで動く1つの実行ファイル**にする。
渡す相手に処理系を入れてもらう必要はなく、`.shk` も要らない
（[spec/runtime/bytecode.md](spec/runtime/bytecode.md)）。

```
./shark build examples/hello.shk   # → ./hello（446 KB）
./hello                            # Hello, Shark!
```

- 中身は**バイトコード実行装置＋バイトコード**。
  字句解析・構文解析・型検査・コード生成は入らないので、`shark` 自身（929 KB）より小さい
  （実行装置 444 KB ＋ hello のバイトコード 2 KB。gzip で 152 KB）
- 型検査は作るときに済んでいる。動かすときはバイトコードを読んで走らせるだけ
- `import` したモジュールも中に入る。作ったファイルはどの場所からでも動く
- メモリの上限（`--memory`）と診断の言語（`--lang`）は、**作るときに**決まる
- 引数と標準入力はそのままプログラムに届く（`os.args()` と `input()`）

```
./shark build --bytecode main.shk   # バイトコードだけ保存する（main.shkc）
./sharkvm main.shkc                 # 実行装置で動かす
./shark run main.shkc               # shark からも動かせる
```

`sharkvm` は仮想マシンだけを持つ実行装置で、`make` で一緒に作られる。
`shark build` はこれを土台にして単一バイナリを組み立てる。

- 起動の速さは `shark run` とほぼ変わらない（型検査はもともと 1 ミリ秒ほど）。
  縮むのは**配るものの大きさと、要るもの**
- 保存したバイトコードは、同じ版の処理系で作り直せる前提。
  版や関数の表が食い違うファイルは、動かす前に気づいて止まる
- macOS では、足したぶんが実行ファイルの形の外に出るので、`codesign -v` は
  「うしろに余りがある」として通らない（そのままでは動く。人に配って
  Gatekeeper や公証を通すには、`.app` に包むなど別の手当てが要る）

## 速さ

同じ内容を C・Python・Shark で書いて測ったもの。**Python と同じくらい**で、
最適化した C とは 5〜60 倍の差がある。

| 内容 | C (-O2) | Python 3.14 | Shark | Shark ÷ C | Shark ÷ Python |
|---|---|---|---|---|---|
| 整数のループ 1000万回（`sum += i % 7`） | 5 ms | 425 ms | **289 ms** | 59 倍 | 0.68 倍 |
| 再帰呼び出し `fib(32)`（436万回の呼び出し） | 5 ms | 149 ms | **309 ms** | 61 倍 | 2.07 倍 |
| 可変長配列に 100万件足して合計（5回くり返す） | 6 ms | 346 ms | **206 ms** | 33 倍 | 0.60 倍 |
| key-value に 50万件入れて、50万回引く | 6 ms | 73 ms | **45 ms** | 7 倍 | 0.62 倍 |
| 書式付きの文字列を 100万個作る | 32 ms | 121 ms | **169 ms** | 5 倍 | 1.39 倍 |
| （下敷き）起動して 0 を出すだけ | 3 ms | 10 ms | 2 ms | — | — |

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
| docs/reference/（`make docs`） | 標準ライブラリのリファレンス。ライブラリごとに1枚、全関数に動く例つき |
| [stdlib/README.md](stdlib/README.md) | その元になる宣言ファイルの書き方 |
| [docs/implementation.md](docs/implementation.md) | 実装メモ（作った範囲・組み込み方・移植の手順） |
| [web/README.md](web/README.md) | ブラウザで動かす（作り方・ホストの入口・できないこと） |
| [spec/README.md](spec/README.md) | 思想と仕様書の索引 |
| [spec/open-questions.md](spec/open-questions.md) | まだ決めていないこと |

## 構成

```
core/     実行系（コア）。C++。ファイルも端末も触らない
  platform/   移植層          ← 機種に合わせて差し替える場所
  lib/        標準ライブラリ
  bytecode    バイトコードの保存と読み戻し
  runtime     バイトコードだけを動かす実行装置（前側を持たない Engine）
frontend/ shark コマンドと sharkvm（実行装置）。どちらもコアとは別実装
web/      ブラウザで動かす一式（WebAssembly。これもコアとは別実装）
examples/ サンプル。embed/ はゲームに組み込む例（その場で読む形と、焼き込む形）
tests/    テスト（make test）
bench/    C・Python・Shark の速さ比べ（python3 bench/run.py）
stdlib/   標準ライブラリの宣言（名前・型・説明・例）。リファレンスと補完のもと
tools/    宣言を読む道具（リファレンス生成・例の実行・prelude の埋め込み）と、
          Windows で作る入口（build_win.bat / build_win.ps1）
assets/   同梱するもの。fonts/ に Noto Sans JP（日本語の字形）
docs/     利用者向けのリファレンスと実装メモ。gen.py が stdlib/ から HTML を作る
spec/     仕様
  types/      型システム
  runtime/    実行系（コア）の内部と、ホストとの境界
  library/    標準ライブラリ
  skeleton.md コアの雛形。何を入れ、どこを書き換えるか
  frontend.md コマンドライン実装（コアとは別に作る）
```
