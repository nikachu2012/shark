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
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace shark {

// ------------------------------------------------------------------ ファイル
inline bool read_file(const Str& path, Str* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, (int)n);
  fclose(f);
  return true;
}

inline bool write_file(const Str& path, const Str& data, bool executable) {
  FILE* f = fopen(path.c_str(), "wb");
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

// 自分自身（いま動いている実行ファイル）の場所
inline bool exe_path(Str* out) {
#if defined(__APPLE__)
  char buf[4096];
  uint32_t n = sizeof buf;
  if (_NSGetExecutablePath(buf, &n) != 0) return false;
  *out = Str(buf);
  return true;
#elif defined(_WIN32)
  char buf[4096];
  DWORD n = GetModuleFileNameA(0, buf, sizeof buf);
  if (n == 0 || n >= sizeof buf) return false;
  *out = Str(buf, (int)n);
  return true;
#else
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf);
  if (n <= 0) return false;
  *out = Str(buf, (int)n);
  return true;
#endif
}

inline Str dir_of_path(const Str& path) {
  for (int i = path.size() - 1; i >= 0; i--)
    if (path[i] == '/' || path[i] == '\\') return path.sub(0, i);
  return Str(".");
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
  FILE* f = fopen(path.c_str(), "rb");
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

// 色を付けるか。環境変数 NO_COLOR に何か入っていれば付けない（no-color.org）
inline bool color_default() {
  Str v;
  if (platform().os && platform().os->env("NO_COLOR", &v) && v.size()) return false;
  return true;
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
    r += "  呼び出しの経路:\n";
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
