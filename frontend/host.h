// host.h — フロントエンドが共通で使う道具
//
// shark コマンド（main.cpp）と、バイトコードだけを動かす実行装置（vm_main.cpp）で
// 同じものを使う。ファイル・自分の居場所・端末への出し方など、
// コアが持たない（持ってはいけない）ところだけがここにある。
#ifndef SHARK_FRONTEND_HOST_H
#define SHARK_FRONTEND_HOST_H

#include <stdio.h>

#include "../core/platform/platform.h"
#include "../core/vm.h"

#if defined(_WIN32)
// windows.h は大きいので、要らないところを外し、min/max の置き換えも止める
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <stdlib.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace shark {

// ------------------------------------------------------------------ Windows の下ごしらえ
//
// Windows の char を取る API は「その機械の言語の符号」（日本語なら CP932）で
// 読み書きする。Shark は中も外もぜんぶ UTF-8 なので、境目で直す。
// ここが無いと、日本語の診断が化け、日本語のファイル名も渡せない。
#if defined(_WIN32)
inline UINT* host_saved_cp() { static UINT cp[2] = {0, 0}; return cp; }   // 出す側・打つ側
inline void host_restore_cp() {
  UINT* cp = host_saved_cp();
  if (cp[0]) SetConsoleOutputCP(cp[0]);
  if (cp[1]) SetConsoleCP(cp[1]);
}

// 起動のはじめに一度だけ呼ぶ。命令行を UTF-8 で取り直し、端末も UTF-8 にする
inline void host_boot(int* argc, char*** argv) {
  // 1. 端末と UTF-8 でやり取りする。終わるときは元に戻す
  //    （符号の設定は、こちらが終わってもその窓に居残るため）
  UINT* cp = host_saved_cp();
  UINT out_cp = GetConsoleOutputCP(), in_cp = GetConsoleCP();
  if (out_cp != CP_UTF8 && SetConsoleOutputCP(CP_UTF8)) cp[0] = out_cp;
  if (in_cp != CP_UTF8 && SetConsoleCP(CP_UTF8)) cp[1] = in_cp;
  if (cp[0] || cp[1]) atexit(host_restore_cp);

  // 2. 色の合図（ANSI）を通す。通らない端末では色が付かないだけ
  const DWORD kVT = 0x0004;   // ENABLE_VIRTUAL_TERMINAL_PROCESSING
  DWORD which[2] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  for (int i = 0; i < 2; i++) {
    HANDLE h = GetStdHandle(which[i]);
    DWORD mode = 0;
    if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
      SetConsoleMode(h, mode | kVT);
  }

  // 3. 命令行を UTF-16 で取り直して UTF-8 に直す。
  //    分け方は OS に任せる（shell32 は実行時に取りに行くので、繋ぐものは増えない）
  typedef LPWSTR*(WINAPI * ToArgvW)(LPCWSTR, int*);
  HMODULE sh = LoadLibraryW(L"shell32.dll");
  if (!sh) return;
  ToArgvW to_argv = (ToArgvW)(void*)GetProcAddress(sh, "CommandLineToArgvW");
  if (!to_argv) return;
  int n = 0;
  LPWSTR* wargv = to_argv(GetCommandLineW(), &n);
  if (!wargv || n <= 0) return;
  char** out = (char**)malloc(sizeof(char*) * (size_t)(n + 1));
  if (!out) return;
  for (int i = 0; i < n; i++) {
    int need = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, 0, 0, 0, 0);
    char* s = (char*)malloc((size_t)(need > 0 ? need : 1));
    if (!s) { out[i] = (char*)""; continue; }
    if (need > 0) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s, need, 0, 0);
    else s[0] = 0;
    out[i] = s;
  }
  out[n] = 0;   // 終わりまで動く間ずっと使うので、返さない
  *argc = n;
  *argv = out;
}

// UTF-8 の道でファイルを開く（fopen は CP932 として読んでしまう）
inline FILE* host_fopen(const char* path, const char* mode) {
  wchar_t wp[4096], wm[8];
  if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 4096) <= 0) return 0;
  if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8) <= 0) return 0;
  return _wfopen(wp, wm);
}
#else
inline void host_boot(int* argc, char*** argv) { (void)argc; (void)argv; }
inline FILE* host_fopen(const char* path, const char* mode) { return fopen(path, mode); }
#endif

// ------------------------------------------------------------------ ファイル
inline bool read_file(const Str& path, Str* out) {
  FILE* f = host_fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, (int)n);
  bool ok = ferror(f) == 0;   // 途中で読めなくなったのを、成功として返さない
  fclose(f);
  return ok;
}

inline bool write_file(const Str& path, const Str& data, bool executable) {
  FILE* f = host_fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = data.size() == 0 || fwrite(data.data(), 1, (size_t)data.size(), f) == (size_t)data.size();
  if (fclose(f) != 0) ok = false;
#if !defined(_WIN32)
  if (ok && executable) chmod(path.c_str(), 0755);
#else
  (void)executable;
#endif
  return ok;
}

// 2つの道が同じファイルを指しているか。./a と a のような書き方の違いを吸収する
inline bool same_file(const Str& a, const Str& b) {
  if (a == b) return true;
#if !defined(_WIN32)
  struct stat sa, sb;
  if (stat(a.c_str(), &sa) != 0 || stat(b.c_str(), &sb) != 0) return false;
  return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#else
  // Windows には i ノードが無いので、OS に「同じファイルか」を尋ねる
  wchar_t wa[4096], wb[4096];
  if (MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, wa, 4096) <= 0) return false;
  if (MultiByteToWideChar(CP_UTF8, 0, b.c_str(), -1, wb, 4096) <= 0) return false;
  HANDLE ha = CreateFileW(wa, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
  if (ha == INVALID_HANDLE_VALUE) return false;
  HANDLE hb = CreateFileW(wb, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
  if (hb == INVALID_HANDLE_VALUE) { CloseHandle(ha); return false; }
  BY_HANDLE_FILE_INFORMATION ia, ib;
  bool ok = GetFileInformationByHandle(ha, &ia) && GetFileInformationByHandle(hb, &ib) &&
            ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
            ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
  CloseHandle(ha);
  CloseHandle(hb);
  return ok;
#endif
}

// 自分自身（いま動いている実行ファイル）の場所
inline bool exe_path(Str* out) {
#if defined(__APPLE__)
  char buf[4096];
  uint32_t n = sizeof buf;
  if (_NSGetExecutablePath(buf, &n) != 0) return false;
  *out = Str(buf);
  return true;
#elif defined(_WIN32)
  wchar_t wbuf[4096];
  DWORD n = GetModuleFileNameW(0, wbuf, 4096);
  if (n == 0 || n >= 4096) return false;
  char buf[4096 * 4];   // UTF-8 は1文字が最大4バイト
  int m = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, buf, (int)sizeof buf, 0, 0);
  if (m <= 0) return false;
  *out = Str(buf, m);
  return true;
#else
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf);
  if (n <= 0 || n >= (ssize_t)sizeof buf) return false;   // 収まらないと切り詰めて返る
  *out = Str(buf, (int)n);
  return true;
#endif
}

inline Str dir_of_path(const Str& path) {
  for (int i = path.size() - 1; i >= 0; i--)
    if (path[i] == '/' || path[i] == '\\') return path.sub(0, i);
  return Str(".");
}

// ------------------------------------------------------------------ 同梱のフォント
//
// 内蔵の字形は ASCII だけなので、日本語を出すには本物のフォントが要る
// （[README](../README.md) の「日本語の字を出す」）。配るときに困らないよう、
// 実行ファイルの隣に置いたものを既定にする。
//
// コアは自分からファイルを探さない（spec/README.md）ので、探すのはここ。
// 見つけたら環境変数 SHARK_FONT に入れる。これは
// 「SHARK_FONT → 機種によくある場所」というもとからの探し順の、いちばん前。
// **すでに SHARK_FONT があれば、そちらが勝つ**（使う人の指定を上書きしない）。
inline void host_use_bundled_font() {
  const PlatformOS* os = platform().os;
  const PlatformFile* fs = platform().file;
  if (!os || !os->env || !os->set_env || !fs) return;
  Str given;
  if (os->env("SHARK_FONT", &given) && given.size()) return;

  Str self;
  if (!exe_path(&self)) return;
  Str dir = dir_of_path(self);
  // 実行ファイルの隣（配ったとき）と、その1つ上（bin/ に入れたとき）
  const char* const rel[] = {"/assets/fonts/NotoSansJP-Regular.otf",
                             "/../assets/fonts/NotoSansJP-Regular.otf", 0};
  for (int i = 0; rel[i]; i++) {
    Str p = dir + rel[i];
    if (fs->exists(p.c_str())) { os->set_env("SHARK_FONT", p.c_str()); return; }
  }
}

// ------------------------------------------------------------------ 単一バイナリ
// 実行装置のうしろにバイトコードを足し、最後に 16 バイトの目印を置く。
//
//   [ 実行装置（sharkvm そのもの） ][ バイトコード ][ "SHARKPK1" ][ 長さ 8 バイト ]
//
// 動くときは、自分自身のファイルの末尾を見て、あればそれを読む。
// 目印を後ろに置くのは、前に足すと実行ファイルの形が壊れるため。
const char* const kPackMagic = "SHARKPK1";
const int kPackMagicLen = 8;
const int kPackFooterLen = 16;   // 目印 8 + 長さ 8

inline Str pack_footer(int64_t payload_len) {
  Str f(kPackMagic, kPackMagicLen);
  for (int i = 0; i < 8; i++) f.push((char)(((uint64_t)payload_len >> (8 * i)) & 0xff));
  return f;
}

// 自分自身に埋めてあるバイトコードを取り出す。無ければ false（ふつうの実行装置として動く）
inline bool pack_read_payload(const Str& path, Str* out) {
  FILE* f = host_fopen(path.c_str(), "rb");
  if (!f) return false;
  bool ok = false;
  if (fseek(f, 0, SEEK_END) == 0) {
    long size = ftell(f);
    if (size > kPackFooterLen && fseek(f, size - kPackFooterLen, SEEK_SET) == 0) {
      unsigned char foot[kPackFooterLen];
      if (fread(foot, 1, sizeof foot, f) == sizeof foot) {
        bool magic = true;
        for (int i = 0; i < kPackMagicLen; i++)
          if (foot[i] != (unsigned char)kPackMagic[i]) magic = false;
        uint64_t len = 0;
        for (int i = 0; i < 8; i++) len |= (uint64_t)foot[kPackMagicLen + i] << (8 * i);
        if (magic && len > 0 && len <= (uint64_t)(size - kPackFooterLen) &&
            fseek(f, size - kPackFooterLen - (long)len, SEEK_SET) == 0) {
          out->reserve((int)len);
          char buf[4096];
          uint64_t left = len;
          ok = true;
          while (left > 0) {
            size_t want = left < sizeof buf ? (size_t)left : sizeof buf;
            size_t got = fread(buf, 1, want, f);
            if (got == 0) { ok = false; break; }
            out->append(buf, (int)got);
            left -= got;
          }
        }
      }
    }
  }
  fclose(f);
  return ok;
}

// ------------------------------------------------------------------ 入出力
inline void host_write_out(void* ud, const char* s, int n) {
  (void)ud;
  fwrite(s, 1, (size_t)n, stdout);
}
inline bool host_read_line(void* ud, Str* out) {
  (void)ud;
  return platform().read_line(out);
}
inline HostIO host_io() {
  HostIO io;
  io.write_out = host_write_out;
  io.read_line = host_read_line;
  return io;
}

// 色を付けるか。診断も panic も標準エラーに出すので、そこが端末のときだけ付ける。
// 環境変数 NO_COLOR に何か入っていれば付けない（no-color.org）。
// バイトコードを埋めた単一バイナリは引数をぜんぶプログラムに渡す（--no-color を取らない）ので、
// ファイルに落としたときに色が混ざらないかどうかは、ここだけが決める
inline bool color_default() {
  Str v;
  if (platform().os && platform().os->env("NO_COLOR", &v) && v.size()) return false;
#if defined(_WIN32)
  return _isatty(_fileno(stderr)) != 0;
#else
  return isatty(fileno(stderr)) != 0;
#endif
}

// ------------------------------------------------------------------ 実行
inline void print_panic(VM& vm, bool color) {
  const char* red = color ? "\x1b[31m\x1b[1m" : "";
  const char* off = color ? "\x1b[0m" : "";
  Str r;
  r += red;
  r += "panic: ";
  r += off;
  r += vm.error_message;
  r += "\n";
  if (vm.error_line > 0) {
    r += "  --> ";
    r += vm.error_file;
    r += ":";
    r += str_from_int(vm.error_line);
    r += "\n";
  }
  if (vm.error_trace.size()) {
    r += vm.L("  呼び出しの経路:\n", "  call path:\n");
    r += vm.error_trace;
  }
  fwrite(r.data(), 1, (size_t)r.size(), stderr);
}

// 終わるまで刻み続ける。ゲームと違い、区切る必要がない（spec/frontend.md）
inline int run_loop(VM& vm, bool color) {
  for (;;) {
    RunStatus st = vm.step(200000);
    if (st == SK_Finished) return vm.exit_code;
    if (st == SK_Error) {
      print_panic(vm, color);
      return 1;
    }
    if (vm.idle_hint) platform().sleep_nanos(500000);  // 1000分の0.5秒だけ休む
  }
}

}  // namespace shark
#endif
