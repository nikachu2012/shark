// platform.h — 移植層の境界（spec/runtime/platform.md）
//
// 新しいプラットフォームへ移植する際は、本ヘッダの Platform を実装する。
// コアはここから先の OS API を直接呼び出さない。
#ifndef SHARK_PLATFORM_H
#define SHARK_PLATFORM_H

#include "../support.h"

namespace shark {

// --- 任意機能：ファイル ---------------------------------------------------
struct PlatformFile {
  // ハンドルは void*。開けなければ 0 を返し、err にメッセージを入れる
  void* (*open)(const char* path, const char* mode, Str* err);
  // 読めたバイト数。0 は終端
  int (*read)(void* h, char* buf, int n);
  bool (*write)(void* h, const char* buf, int n);
  void (*close)(void* h);

  bool (*exists)(const char* path);
  bool (*is_dir)(const char* path);
  bool (*size)(const char* path, int64_t* out);
  bool (*modified)(const char* path, int64_t* unix_nanos);
  bool (*remove)(const char* path, Str* err);
  bool (*rename)(const char* from, const char* to, Str* err);
  bool (*make_dir)(const char* path, Str* err);
  bool (*list)(const char* dir, Vec<Str>* out, Str* err);
};

// --- 任意機能：OS ---------------------------------------------------------
struct PlatformOS {
  const char* (*name)();  // "macos" "windows" "linux" "wasm" "embedded"
  bool (*env)(const char* name, Str* out);
  void (*set_env)(const char* name, const char* value);
  bool (*cwd)(Str* out);
  bool (*chdir)(const char* path);
  const char* (*temp_dir)();
  // 外部プログラム。持たない環境では 0。
  // out と err は UTF-8 で返すこと（機種の符号で書くプログラムがある環境では、
  // ここで直す → spec/library/os.md）
  bool (*run)(const char* cmd, const Vec<Str>& args, int* code, Str* out, Str* err);
};

// --- 任意機能：エントロピー源（乱数） -------------------------------------
//
// std.crypto が使用。両方 nullptr で可（その場合 crypto の乱数は利用不可）。
// 優先順位は true_bytes が先で、未取得時は secure_bytes へフォールバック。
// どちらから取得できたかは crypto.source() で判定可能（spec/library/crypto.md）。
struct PlatformRandom {
  // ハードウェアノイズ等による真の乱数。非対応環境またはエントロピー不足時は false
  bool (*true_bytes)(unsigned char* buf, int n);
  // OS 提供の暗号論的擬似乱数。取得失敗時は false
  bool (*secure_bytes)(unsigned char* buf, int n);
};

// --- 任意機能：フォント描画 -----------------------------------------------
//
// std.ui が使用。プラットフォーム側でフォント描画を提供する環境（ブラウザ等）向けインターフェース。
// 未サポート時は nullptr で可（内蔵 5×7 ASCII フォントを使用）。
// FreeType がリンクされている場合はそちらが優先される（spec/library/ui.md）。
//
// グリフはアルファマスク（0〜255）として返却する。座標系は FreeType と同一で、
// left はベースライン起点から右方向、top はベースライン起点から上方向。
struct PlatformGlyph {
  const unsigned char* bits;  // w×h の濃さ。移植層の持ちもの（次に glyph を呼ぶまで有効）
  int w, h;
  int left, top;
  int adv;                    // 次の字までの幅
};

struct PlatformFont {
  // 使えるようにする。name が 0 なら、その機種でふつうに使えるものを選ぶ。
  // px は字の大きさ（画素）。使えなければ false
  bool (*open)(const char* name, int px);
  void (*close)();
  const char* (*name)();          // いま使っているものの名前（無ければ ""）
  bool (*glyph)(int cp, int px, PlatformGlyph* out);
  int (*line_height)(int px);     // 1行の高さ（字の上端から下端まで）
  int (*ascender)(int px);        // 基準線から字の上端まで
};

// --- 任意機能：画面描画（スクリーン） -------------------------------------
//
// std.ui が使用（spec/library/ui.md）。GUI 非対応環境では nullptr で可
// （オフスクリーンバッファ描画となり、描画結果は ui.get() や ui.to_png() で取得可能）。
//
// コア自身がピクセルバッファ（RGB 配列）を描画し、本インターフェースに要求されるのは以下の3点のみ。
//   1. バッファを画面へ転送する  present
//   2. 発生したイベントを渡す    poll（ノンブロッキング。イベントなしなら false）
//   3. 初期化とクローズ          open / close

enum ScreenEventKind {
  SEV_None = 0,
  SEV_Close,   // ウィンドウのクローズ要求
  SEV_Key,     // キー押下・解放イベント（code にキーコード）
  SEV_Text,    // 文字入力イベント（text に UTF-8 文字列）
  SEV_Mouse,   // マウス押下・解放・移動イベント（code はボタン番号、-1 は移動）
  SEV_Resize,  // ウィンドウリサイズイベント（x, y に新しいサーフェス解像度ピクセル）
  SEV_Wheel,   // マウスホイールイベント（x, y にスクロール量。下・右が正）
};

// キーの番号。印字できる文字はその ASCII（英字は小文字）をそのまま使い、
// それ以外はここの番号を使う
enum ScreenKey {
  SKEY_Left = 0x100, SKEY_Right, SKEY_Up, SKEY_Down,
  SKEY_Enter, SKEY_Escape, SKEY_Tab, SKEY_Back, SKEY_Delete,
  SKEY_Home, SKEY_End, SKEY_PageUp, SKEY_PageDown,
  SKEY_Shift, SKEY_Ctrl, SKEY_Alt,
  SKEY_Meta,   // macOS の Command、Windows の Windows キー、Linux の Super
  SKEY_F1, SKEY_F2, SKEY_F3, SKEY_F4, SKEY_F5, SKEY_F6,
  SKEY_F7, SKEY_F8, SKEY_F9, SKEY_F10, SKEY_F11, SKEY_F12,
  SKEY_Max
};

// マウスポインタ形状。set_cursor に渡す。未サポートの形状は近似形状で代替可能
enum ScreenCursor {
  SCUR_Arrow = 0,   // ふつう
  SCUR_Hand,        // 押せるところ（ボタンの上など）
  SCUR_Text,        // 文字を打つところ
  SCUR_Cross,       // 照準
  SCUR_Wait,        // 待たせているところ
  SCUR_ResizeX,     // 横に伸ばす
  SCUR_ResizeY,     // 縦に伸ばす
  SCUR_Move,        // つかんで動かす
  SCUR_None,        // 消す（自分で描くとき）
  SCUR_Max
};

struct ScreenEvent {
  int kind;      // ScreenEventKind
  int code;      // SEV_Key: ScreenKey / SEV_Mouse: 0=左 1=中 2=右、-1 は移動
  bool down;     // SEV_Key / SEV_Mouse: 押されたなら true
  // SEV_Wheel の x, y は**送りぶん（1/100 行）**で、下と右が正。
  // 1段ぶん回すと 100。**行より細かく取れるようにしてある**ので、
  // トラックパッドをゆっくり動かしたぶんも落ちない（行の数で渡すと、
  // 1行たまるまで何も動かず、動き出すと1行飛ぶ）。
  // 段でしか送れない機種（車輪だけのマウス）は、100 の倍数で渡せばよい。
  // 位置は直前の SEV_Mouse のものを使う
  int x, y;      // SEV_Mouse: 面の中の位置（画素） / SEV_Resize: 新しい面の大きさ
                 // SEV_Wheel: 送りぶん（1/100 行。下と右が正）
  char text[8];  // SEV_Text: 打たれた文字（UTF-8。0 で終わる）
  ScreenEvent() : kind(SEV_None), code(0), down(false), x(0), y(0) { text[0] = 0; }
};

struct PlatformScreen {
  // ディスプレイのスケーリング整数比（1=等倍、2=HiDPI）。**ウィンドウ生成前にも呼べること**。
  // std.ui はこれを参照してサーフェスサイズとフォントサイズを決定する（ui.scale()）。
  // 非整数のスケール（1.5 等）の場合は下記の pixel_ratio() が優先される
  int (*scale)();
  // ウィンドウを初期化・表示する。失敗時は false（コアはオフスクリーン描画にフォールバック）
  bool (*open)(const char* title, int w, int h);
  void (*close)();
  // サーフェスバッファを画面に転送する。px は幅 w・高さ h の配列で、各ピクセルは 0x00RRGGBB
  void (*present)(const uint32_t* px, int w, int h);
  // 発生したイベントを1件取得する。未発生時は false を返す（ノンブロッキング）
  bool (*poll)(ScreenEvent* out);
  // キー解放イベントを検知可能か。非対応環境ではコア側で
  // 「押下フレームの間のみ押下状態」と判定する
  bool has_key_up;

  // --- 文字入力（IME）。無ければ 0 でよい -------------------------------
  //
  // 日本語などの変換は OS が持っている。自前で候補一覧まで描くと機種ごとの
  // 作り込みが際限なく増えるので、**変換は OS に任せて、結果だけ受け取る**
  // （spec/library/ui.md）。
  //
  // 入力欄に文字を入れているあいだだけ on にする。off のあいだ、キーは
  // ふつうの出来事（SEV_Key / SEV_Text）として届く。
  //   initial  受け付け始めるときの中身。0 なら今の中身のまま（置き場所だけ変える）
  //   x, y, h  面の中の位置。変換中の候補をこのあたりに出す
  //   multiline  複数行テキスト入力か。true の場合は改行を保持する。
  //              ナビゲーションキー（Enter・上下矢印・Home・End）はコア側で処理するため、
  //              OS 側のテキストビューには渡さない（二重処理を防止）
  void (*text_input)(bool on, const char* initial, int x, int y, int h, bool multiline);
  // いまの中身。confirmed に確定した文字列、marked に変換中の文字列。
  // text_input(on) のあいだだけ意味がある。取れなければ false
  bool (*text_state)(Str* confirmed, Str* marked);

  // 選んでいるところ。単位は**文字の数**（先頭から数えて start から len 文字）。
  // 持っている機種では、矢印・shift・二度押しでの選択も OS がやってくれる。
  //
  // **インデックス管理の注意:** OS 側のテキスト入力コンポーネントがコンポジション文字列を含む
  // 全体を 1 つのバッファとして保持する環境（macOS の NSTextView、ブラウザの <textarea>）では、
  // 返されるオフセットはコンポジション文字列も含めたインデックスとなる。一方 text_state の confirmed は
  // 未確定テキストを除外した確定文字列であるため、変換中はオフセット基準が乖離する。
  // コア側はこの差異を考慮し、コンポジション中はカーソル追従を行わず開始位置を保持する
  // （core/lib/ui.cpp、tests/imecheck.cpp 参照）。
  // 移植層はどちらのオフセット管理方式でもよいが、変換中に独自にオフセットを再計算しないこと。
  bool (*text_selection)(int* start, int* len);
  void (*text_select)(int start, int len);
  // 選択範囲をこの文字列で置換（ペーストおよびカット時に使用）
  void (*text_replace)(const char* s);

  // --- クリップボード連携。未サポート時は nullptr -----------------------
  bool (*clipboard_get)(Str* out);
  void (*clipboard_set)(const char* s);

  // --- ウィンドウリサイズ中の再描画コールバック。未サポート時は nullptr -
  //
  // 縁を引いている間、OS がプログラムを止めてしまう機種（macOS）で使う。
  // コアが open のあとに fn を渡しておくと、移植層は止まっている間に
  // 「この大きさで絵を出し直してくれ」と fn を呼べる。fn は面を作り直し、
  // 覚えている部品を新しい大きさで置き直して present まで済ませたら true を返す。
  // false なら移植層が自分の手当て（等倍のまま置き直すなど）に落ちる
  void (*set_redraw)(bool (*fn)(int w, int h));

  // --- マウスポインタ形状設定。未サポート時は nullptr -------------------
  //
  // std.ui が、押せるところに合わせたときなどに呼ぶ（ui.cursor()）。
  // 変わったときにだけ呼ばれる。kind は ScreenCursor
  void (*set_cursor)(int kind);

  // --- ウィンドウリサイズ可否の設定。未サポート時は nullptr -------------
  //
  // false で「縁を引いても大きさの変わらない窓」にする。コアが open の
  // すぐあとに呼ぶ。大きさをこちらで決められない出し先は持たなくてよい
  void (*set_resizable)(bool on);

  // --- ホスト側主導のフレームペーシング（デフォルト: false）-------------
  //
  // ブラウザのように、**プログラムを進めるきっかけをホストが決めている**
  // 機種で true にする（requestAnimationFrame ＝ 1秒に 60 回など）。
  //
  // その機種では、こまの刻限のすぐ後ろで起きると、その回のきっかけに間に合わず
  // 次のきっかけまで丸ごと待つことになり、**速さが半分になる**。
  // true なら ui.frame() が刻限の少し手前で起きて、これを避ける
  // （spec/library/ui.md「こまの速さ」）。きっかけを自分で作れる機種は false
  bool host_paced;

  // --- OS ネイティブのファイル選択ダイアログ。未サポート時は nullptr ----
  //
  // std.ui の ui.pick_file() / ui.pick_save() から呼ぶ。OS の窓を出して、
  // 選ばれたら true と、そのパスを out に返す。取りやめたら false。
  // **選んでいるあいだプログラムは止まる**（OS が窓を持つあいだ返らない）。
  //   save   保存する先を選ぶなら true。false なら開くものを選ぶ
  //   title  ダイアログのタイトル（nullptr 許容）
  //   name   保存のとき、はじめに入れておく名前（0 でもよい）
  bool (*pick_file)(bool save, const char* title, const char* name, Str* out);

  // --- ディスプレイスケーリング（非整数値）。未サポート時は nullptr -----
  //
  // scale() は整数に丸めたスケール値で、通常 1 または 2 を返す。
  // しかし実際には Windows の 125% / 150% 表示やブラウザの拡大率など、
  // **非整数のスケーリング値**が一般的である。整数に丸めたサイズでバッファを生成すると、
  // キャンバスのピクセルが物理画面のピクセルグリッドと一致せず、
  // プラットフォーム側で再サンプリングされて**表示が滲む**。
  //
  // 本関数は浮動小数点のスケール値（1.5 等）を返す。std.ui はこれに基づいて
  // バッファ解像度を決定するため、キャンバスの 1 ピクセルが物理画面の 1 ピクセルに
  // 正確に一致する（ui.pixel_ratio()）。
  // 1 未満または 0 を返してはならない。サポートしない環境では nullptr を指定し、
  // その場合は scale() の整数値がフォールバックとして使用される。
  double (*pixel_ratio)();
};

// --- 必須 ----------------------------------------------------------------
struct Platform {
  void* (*alloc)(size_t n);
  void* (*realloc)(void* p, size_t n);
  void  (*free)(void* p);
  void  (*fatal)(const char* msg);

  int64_t (*now_unix_nanos)();        // 実時刻（UTC）
  int64_t (*monotonic_nanos)();       // 単調増加。時刻合わせの影響を受けない
  void    (*sleep_nanos)(int64_t n);  // 短い待機。コア本体は使わない
  int     (*local_offset_seconds)(int64_t unix_nanos);

  void (*write_out)(const char* s, int n);
  void (*write_err)(const char* s, int n);
  bool (*read_line)(Str* out);  // false は終端
  void (*exit_process)(int code);

  const PlatformFile*   file;    // 無ければ 0
  const PlatformOS*     os;      // 無ければ 0
  const PlatformRandom* random;  // 無ければ 0
  const PlatformScreen* screen;  // 無ければ 0
  const PlatformFont*   font;    // 無ければ 0（内蔵の 5×7 か FreeType になる）
};

// いま使っている移植層。差し替えるときは platform_set() を呼ぶ
const Platform& platform();
void platform_set(const Platform* p);

// 用意してある移植層
const Platform* platform_desktop();  // platform/desktop.cpp
const Platform* platform_console();  // platform/console.cpp（OS やファイル機能を持たない組み込み向けの例）
const Platform* platform_web();      // platform/web.cpp（ブラウザ。Emscripten 以外では 0）

}  // namespace shark
#endif
