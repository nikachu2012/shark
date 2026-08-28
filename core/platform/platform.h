// platform.h — 移植層の境界（spec/runtime/platform.md）
//
// 新しい機種に載せるときは、このファイルの Platform を埋めたものを1つ足す。
// コアはここから先の OS を直接触らない。
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
  // 外部プログラム。持たない環境では 0
  bool (*run)(const char* cmd, const Vec<Str>& args, int* code, Str* out, Str* err);
};

// --- 任意機能：乱数のもと -------------------------------------------------
//
// std.crypto が使う。2つとも 0 でよく、その場合 crypto の乱数は使えない。
// 埋める順は true_bytes が先で、取れなければ secure_bytes に落ちる。
// どちらから取れたかは crypto.source() で分かる（spec/library/crypto.md）。
struct PlatformRandom {
  // 機器の雑音から作る真の乱数。無い機種や、雑音が足りないときは false
  bool (*true_bytes)(unsigned char* buf, int n);
  // OS が渡す暗号学的に安全な乱数。取れなければ false
  bool (*secure_bytes)(unsigned char* buf, int n);
};

// --- 任意機能：字 ---------------------------------------------------------
//
// std.ui が使う。**機種が字を描いてくれるところ**（ブラウザなど）のための口で、
// 無い機種では 0 でよい。その場合 std.ui は内蔵の 5×7（ASCII）だけになる。
// FreeType を組み込んであるときは、そちらが先に使われる（spec/library/ui.md）。
//
// 字形は「濃さ」（0〜255）の並びで返す。置き場所の数え方は FreeType と同じで、
// left は基準線の起点から右へ、top はその起点から上へ数える。
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

// --- 任意機能：画面 -------------------------------------------------------
//
// std.ui が使う（spec/library/ui.md）。無い機種では 0 でよく、その場合 std.ui は
// 「見えない面」に描くだけになる（描いた結果は ui.get() や ui.to_png() で取れる）。
//
// コアは面（RGB の並び）を自前で描き、ここに求めるのは次の3つだけ。
//   1. その面を画面に出す        present
//   2. 起きた出来事を渡す        poll（待たない。無ければ false）
//   3. 開ける・閉じる            open / close

enum ScreenEventKind {
  SEV_None = 0,
  SEV_Close,   // 閉じてくれと言われた
  SEV_Key,     // キーが押された・離された（code に下の番号）
  SEV_Text,    // 文字が打たれた（text に UTF-8）
  SEV_Mouse,   // 押された・離された・動いた（code はボタン、-1 は移動）
  SEV_Resize,  // 窓の大きさが変わった（x, y に新しい面の大きさ。画素）
};

// キーの番号。印字できる文字はその ASCII（英字は小文字）をそのまま使い、
// それ以外はここの番号を使う
enum ScreenKey {
  SKEY_Left = 0x100, SKEY_Right, SKEY_Up, SKEY_Down,
  SKEY_Enter, SKEY_Escape, SKEY_Tab, SKEY_Back, SKEY_Delete,
  SKEY_Home, SKEY_End, SKEY_PageUp, SKEY_PageDown,
  SKEY_Shift, SKEY_Ctrl, SKEY_Alt,
  SKEY_F1, SKEY_F2, SKEY_F3, SKEY_F4, SKEY_F5, SKEY_F6,
  SKEY_F7, SKEY_F8, SKEY_F9, SKEY_F10, SKEY_F11, SKEY_F12,
  SKEY_Max
};

// マウスの形。set_cursor に渡す。持っていない形は、近いものに寄せてよい
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
  int x, y;      // SEV_Mouse: 面の中の位置（画素） / SEV_Resize: 新しい面の大きさ
  char text[8];  // SEV_Text: 打たれた文字（UTF-8。0 で終わる）
  ScreenEvent() : kind(SEV_None), code(0), down(false), x(0), y(0) { text[0] = 0; }
};

struct PlatformScreen {
  // 画面の細かさ（1 なら等倍、2 なら HiDPI）。**開く前にも呼べること**。
  // std.ui はこれを見て、面の大きさと字の大きさを決める（ui.scale()）
  int (*scale)();
  // 画面を用意する。出せないなら false（そのときコアは見えない面に描く）
  bool (*open)(const char* title, int w, int h);
  void (*close)();
  // 面の中身を画面に出す。px は横 w・縦 h の並びで、1つが 0x00RRGGBB
  void (*present)(const uint32_t* px, int w, int h);
  // 起きた出来事を1つ取る。無ければ false を返す。**待たない**
  bool (*poll)(ScreenEvent* out);
  // 離した合図を出せるか。出せない機種（端末など）では、コアが
  // 「押された刻みのあいだだけ押されている」とみなす
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
  void (*text_input)(bool on, const char* initial, int x, int y, int h);
  // いまの中身。confirmed に確定した文字列、marked に変換中の文字列。
  // text_input(on) のあいだだけ意味がある。取れなければ false
  bool (*text_state)(Str* confirmed, Str* marked);

  // 選んでいるところ。単位は**文字の数**（先頭から数えて start から len 文字）。
  // 持っている機種では、矢印・shift・二度押しでの選択も OS がやってくれる
  bool (*text_selection)(int* start, int* len);
  void (*text_select)(int start, int len);
  // 選んでいるところを、この文字列で置き換える（貼り付けと切り取りに使う）
  void (*text_replace)(const char* s);

  // --- 切り貼りの置き場（クリップボード）。無ければ 0 -------------------
  bool (*clipboard_get)(Str* out);
  void (*clipboard_set)(const char* s);

  // --- 窓の縁を引いている間の描き直し。無ければ 0 -----------------------
  //
  // 縁を引いている間、OS がプログラムを止めてしまう機種（macOS）で使う。
  // コアが open のあとに fn を渡しておくと、移植層は止まっている間に
  // 「この大きさで絵を出し直してくれ」と fn を呼べる。fn は面を作り直し、
  // 覚えている部品を新しい大きさで置き直して present まで済ませたら true を返す。
  // false なら移植層が自分の手当て（等倍のまま置き直すなど）に落ちる
  void (*set_redraw)(bool (*fn)(int w, int h));

  // --- マウスの形。無ければ 0 -------------------------------------------
  //
  // std.ui が、押せるところに合わせたときなどに呼ぶ（ui.cursor()）。
  // 変わったときにだけ呼ばれる。kind は ScreenCursor
  void (*set_cursor)(int kind);

  // --- 大きさを変えられない窓。無ければ 0 -------------------------------
  //
  // false で「縁を引いても大きさの変わらない窓」にする。コアが open の
  // すぐあとに呼ぶ。端末など、大きさをこちらで決められない出し先は持たなくてよい
  void (*set_resizable)(bool on);
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
const Platform* platform_console();  // platform/console.cpp（ファイルも OS も無い機種の例）
const Platform* platform_web();      // platform/web.cpp（ブラウザ。Emscripten 以外では 0）

}  // namespace shark
#endif
