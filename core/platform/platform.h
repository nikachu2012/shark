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
