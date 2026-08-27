// web.cpp — ブラウザ（WebAssembly）向けの移植層
//
// Emscripten でコンパイルして使う（web/build.sh）。
// ここを埋めるだけで、コアはそのままブラウザで動く（spec/runtime/platform.md）。
//
// ブラウザには「戻ってこない待ち」を持ち込めない。待つとタブごと固まるので、
// 待機は何もしない。進む量は step() の刻みでホストが決める
// （spec/runtime/embedding.md）。
//
// ファイルは、そのタブの中だけにある仮想のもの（Emscripten の MEMFS）。
// 外の世界のファイルは触れないし、閉じれば消える。
//
// 画面（std.ui）は canvas に出す。中身は screen_canvas.inc。
#include "web.h"

namespace shark {
namespace {
WebSink g_sink = 0;
void* g_sink_ud = 0;
}  // namespace

void web_set_sink(WebSink fn, void* ud) { g_sink = fn; g_sink_ud = ud; }

}  // namespace shark

#if defined(__EMSCRIPTEN__)

#include <dirent.h>
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace shark {
namespace {

void emit(const char* s, int n, bool is_err) {
  if (g_sink) g_sink(g_sink_ud, s, n, is_err);
}

// --- 必須 ---
void* w_alloc(size_t n) { return malloc(n ? n : 1); }
void* w_realloc(void* p, size_t n) { return realloc(p, n ? n : 1); }
void  w_free(void* p) { free(p); }
void  w_fatal(const char* msg) {
  emit(msg, (int)sk_strlen(msg), true);
  emit("\n", 1, true);
  abort();
}

int64_t w_now() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}
int64_t w_mono() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}
// 待たない。ブラウザは1本の流れで動いていて、ここで待つと画面ごと止まる
void w_sleep(int64_t) {}

int w_local_offset(int64_t unix_nanos) {
  time_t t = (time_t)(unix_nanos / 1000000000ll);
  struct tm lt, gt;
  localtime_r(&t, &lt);   // ブラウザの時間帯を Emscripten が渡してくれる
  gmtime_r(&t, &gt);
  int64_t l = (int64_t)lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
  int64_t g = (int64_t)gt.tm_hour * 3600 + gt.tm_min * 60 + gt.tm_sec;
  int64_t diff = l - g;
  int dday = lt.tm_yday - gt.tm_yday;
  if (dday == 1 || dday < -1) diff += 86400;
  else if (dday == -1 || dday > 1) diff -= 86400;
  return (int)diff;
}

void w_write_out(const char* s, int n) { emit(s, n, false); }
void w_write_err(const char* s, int n) { emit(s, n, true); }
// 端末が無いので、ここでは読めない。input() に返す文字列はホストが渡す
bool w_read_line(Str* out) { out->clear(); return false; }
// タブを閉じるわけにはいかない。終わり方はホストが決める
void w_exit(int) {}

// --- ファイル（タブの中だけにある仮想のもの） ---
void* f_open(const char* path, const char* mode, Str* err) {
  const char* m = "rb";
  if (mode[0] == 'w') m = "wb";
  else if (mode[0] == 'a') m = "ab";
  FILE* f = fopen(path, m);
  if (!f) { *err = Str("ファイルを開けません: ") + path; return 0; }
  return (void*)f;
}
int  f_read(void* h, char* buf, int n) { return (int)fread(buf, 1, (size_t)n, (FILE*)h); }
bool f_write(void* h, const char* buf, int n) { return (int)fwrite(buf, 1, (size_t)n, (FILE*)h) == n; }
void f_close(void* h) { fclose((FILE*)h); }
bool f_exists(const char* p) { struct stat st; return stat(p, &st) == 0; }
bool f_is_dir(const char* p) {
  struct stat st;
  if (stat(p, &st) != 0) return false;
  return S_ISDIR(st.st_mode);
}
bool f_size(const char* p, int64_t* out) {
  struct stat st;
  if (stat(p, &st) != 0) return false;
  *out = (int64_t)st.st_size;
  return true;
}
bool f_modified(const char* p, int64_t* ns) {
  struct stat st;
  if (stat(p, &st) != 0) return false;
  *ns = (int64_t)st.st_mtime * 1000000000ll;
  return true;
}
bool f_remove(const char* p, Str* err) {
  if (remove(p) != 0) { *err = Str("消せません: ") + p; return false; }
  return true;
}
bool f_rename(const char* a, const char* b, Str* err) {
  if (rename(a, b) != 0) { *err = Str("名前を変えられません: ") + a; return false; }
  return true;
}
bool f_make_dir(const char* p, Str* err) {
  Str cur;
  for (int i = 0;; i++) {   // 途中の階層も順に作る
    if (p[i] == '/' || p[i] == 0) {
      if (cur.size() > 0) mkdir(cur.c_str(), 0777);
      if (p[i] == 0) break;
    }
    cur.push(p[i]);
  }
  if (!f_is_dir(p)) { *err = Str("作れません: ") + p; return false; }
  return true;
}
bool f_list(const char* dir, Vec<Str>* out, Str* err) {
  DIR* d = opendir(dir);
  if (!d) { *err = Str("開けません: ") + dir; return false; }
  struct dirent* e;
  while ((e = readdir(d)) != 0) {
    if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
    out->push(Str(e->d_name));
  }
  closedir(d);
  return true;
}
const PlatformFile kFile = {f_open, f_read, f_write, f_close, f_exists, f_is_dir, f_size,
                            f_modified, f_remove, f_rename, f_make_dir, f_list};

// --- OS ---
const char* o_name() { return "wasm"; }
bool o_env(const char* name, Str* out) {
  const char* v = getenv(name);
  if (!v) return false;
  *out = Str(v);
  return true;
}
void o_set_env(const char* name, const char* value) { setenv(name, value, 1); }
bool o_cwd(Str* out) {
  char buf[4096];
  if (!getcwd(buf, sizeof buf)) return false;
  *out = Str(buf);
  return true;
}
bool o_chdir(const char* p) { return chdir(p) == 0; }
const char* o_temp_dir() { return "/tmp"; }
// ブラウザの中には呼べる外のプログラムが無い。持たない機能は失敗として返す
bool o_run(const char* cmd, const Vec<Str>& args, int* code, Str* out, Str* err) {
  (void)cmd; (void)args; (void)code; (void)out;
  *err = Str("ブラウザでは外のプログラムを呼べません");
  return false;
}
const PlatformOS kOS = {o_name, o_env, o_set_env, o_cwd, o_chdir, o_temp_dir, o_run};

// --- 乱数のもと ---
// ブラウザの暗号用乱数（crypto.getRandomValues）を借りる。
// 雑音源そのものには手が届かないので、真の乱数は持たない
bool w_secure(unsigned char* buf, int n) {
  int ok = EM_ASM_INT({
    var c = (typeof crypto !== 'undefined' && crypto.getRandomValues) ? crypto : null;
    if (!c && typeof require === 'function') {          // node で動かすとき
      try { c = require('crypto').webcrypto; } catch (e) { c = null; }
    }
    if (!c || !c.getRandomValues) return 0;
    for (var i = 0; i < $1; i += 65536) {               // 一度に 65536 バイトまで
      var end = Math.min(i + 65536, $1);
      c.getRandomValues(HEAPU8.subarray($0 + i, $0 + end));
    }
    return 1;
  }, (int)(size_t)buf, n);
  return ok != 0;
}
const PlatformRandom kRandom = {0, w_secure};

// --- 画面 ---
#include "screen_canvas.inc"

const Platform kWeb = {
    w_alloc, w_realloc, w_free, w_fatal,
    w_now, w_mono, w_sleep, w_local_offset,
    w_write_out, w_write_err, w_read_line, w_exit,
    &kFile, &kOS, &kRandom,
    &kCanvasScreen};   // std.ui の面は canvas に出す（spec/library/ui.md）

}  // namespace

const Platform* platform_web() { return &kWeb; }

}  // namespace shark

#else   // Emscripten 以外では中身を持たない

namespace shark {
const Platform* platform_web() { return 0; }
}  // namespace shark

#endif
