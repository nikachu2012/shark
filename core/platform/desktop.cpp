// desktop.cpp — Windows / macOS / Linux 向けの移植層
//
// ここを自分の機種のものに差し替える（spec/skeleton.md）。
#if defined(_WIN32)
#define _CRT_RAND_S   // 暗号用の乱数 rand_s を使う。stdlib.h より前に要る
// windows.h は大きいので、要らないところを外し、min/max の置き換えも止める
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>

#include <imm.h>   // 変換つきの文字入力（screen_win.inc）。繋ぐ .lib は要らない

#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/random.h>   // getentropy
#endif

// 真の乱数は機種の道具に頼る。x86 の RDSEED は雑音源から直に取る命令で、
// 待たずに「いま取れる分」を返す（spec/runtime/platform.md）
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define SHARK_X86 1
#if defined(_MSC_VER)
#include <immintrin.h>
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace shark {
namespace {

#if defined(_WIN32)
// --- 字の符号（Windows だけ）---------------------------------------------
//
// Shark は道も名前も、中も外もぜんぶ UTF-8。ところが Windows の char を取る
// API は「その機械の言語の符号」（日本語の Windows なら CP932）として読むので、
// UTF-8 のまま渡すと日本語の入ったファイル名が開けない。
// そこで OS を呼ぶ手前で UTF-16（W 付きの API）に直し、返ってきたら戻す。
struct Wide {
  wchar_t small_[512];
  wchar_t* p;
  explicit Wide(const char* s) : p(small_) {
    small_[0] = 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, 0, 0);   // 0 で終わる分も含む長さ
    if (n <= 0) return;
    if (n > (int)(sizeof small_ / sizeof small_[0])) {
      wchar_t* big = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
      if (!big) return;   // 取れなければ空の道になり、呼んだ側が失敗として扱う
      p = big;
    }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, p, n);
  }
  ~Wide() { if (p != small_) free(p); }
  const wchar_t* get() const { return p; }
 private:
  Wide(const Wide&);
  Wide& operator=(const Wide&);
};

// UTF-8 として筋の通ったバイトの並びか
bool looks_utf8(const Str& s) {
  int i = 0;
  while (i < s.size()) {
    unsigned char c = (unsigned char)s[i];
    int n;
    if (c < 0x80) { i++; continue; }
    else if ((c & 0xe0) == 0xc0) n = 1;
    else if ((c & 0xf0) == 0xe0) n = 2;
    else if ((c & 0xf8) == 0xf0) n = 3;
    else return false;
    if (i + n >= s.size()) return false;   // 途中で切れている
    for (int k = 1; k <= n; k++)
      if (((unsigned char)s[i + k] & 0xc0) != 0x80) return false;
    i += n + 1;
  }
  return true;
}

Str from_wide(const wchar_t* w) {
  Str r;
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
  if (n <= 1) return r;   // 1 は「0 だけ」＝空
  char* buf = (char*)malloc((size_t)n);
  if (!buf) return r;
  WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, n, 0, 0);
  r.append(buf, n - 1);   // 末尾の 0 は Str が自分で置く
  free(buf);
  return r;
}

// 外のプログラムが返した字を UTF-8 にそろえる。
//
// Windows のプログラムは、その機械の言語の符号（日本語なら CP932）で書くものと、
// UTF-8 で書くものが混ざっている。どちらかは名乗ってくれないので、
// **UTF-8 として筋が通っていればそのまま**、通らなければ機械の符号として読み直す。
void adopt_utf8(Str* s) {
  if (s->size() == 0 || looks_utf8(*s)) return;
  int wn = MultiByteToWideChar(CP_OEMCP, 0, s->data(), s->size(), 0, 0);
  if (wn <= 0) return;   // それでも読めなければ、届いたバイトのまま渡す
  wchar_t* w = (wchar_t*)malloc((size_t)wn * sizeof(wchar_t));
  if (!w) return;
  MultiByteToWideChar(CP_OEMCP, 0, s->data(), s->size(), w, wn);
  int un = WideCharToMultiByte(CP_UTF8, 0, w, wn, 0, 0, 0, 0);
  if (un > 0) {
    char* u = (char*)malloc((size_t)un);
    if (u) {
      WideCharToMultiByte(CP_UTF8, 0, w, wn, u, un, 0, 0);
      *s = Str(u, un);
      free(u);
    }
  }
  free(w);
}
#endif

void* d_alloc(size_t n) { return malloc(n ? n : 1); }
void* d_realloc(void* p, size_t n) { return realloc(p, n ? n : 1); }
void  d_free(void* p) { free(p); }
void  d_fatal(const char* msg) { fputs(msg, stderr); fputc('\n', stderr); abort(); }

int64_t d_now() {
#if defined(_WIN32)
  // 1601-01-01 からの 100 ナノ秒きざみ。Unix の紀元まで 11644473600 秒ある
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
  return (int64_t)(t - 116444736000000000ull) * 100;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
#endif
}
int64_t d_mono() {
#if defined(_WIN32)
  // 高い分解能の数え上げ。clock() は 1000分の1秒しか刻めず、こまの速さに使えない
  LARGE_INTEGER f, c;
  if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return (int64_t)GetTickCount64() * 1000000ll;
  QueryPerformanceCounter(&c);
  // 先に割ってから掛ける（掛けてから割ると 64 ビットからあふれる）
  int64_t sec = (int64_t)(c.QuadPart / f.QuadPart);
  int64_t rem = (int64_t)(c.QuadPart % f.QuadPart);
  return sec * 1000000000ll + rem * 1000000000ll / (int64_t)f.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
#endif
}
void d_sleep(int64_t n) {
  if (n <= 0) return;
#if defined(_WIN32)
  // Windows は 1000分の1秒きざみでしか待てない。0 に切り捨てると
  // 「休むつもりが休まない」空回りになるので、切り上げる
  ::Sleep((DWORD)((n + 999999ll) / 1000000ll));
#else
  struct timespec ts;
  ts.tv_sec = (time_t)(n / 1000000000ll);
  ts.tv_nsec = (long)(n % 1000000000ll);
  nanosleep(&ts, 0);
#endif
}
int d_local_offset(int64_t unix_nanos) {
  time_t t = (time_t)(unix_nanos / 1000000000ll);
  struct tm lt, gt;
#if defined(_WIN32)
  localtime_s(&lt, &t); gmtime_s(&gt, &t);
#else
  localtime_r(&t, &lt); gmtime_r(&t, &gt);
#endif
  int64_t l = (int64_t)lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
  int64_t g = (int64_t)gt.tm_hour * 3600 + gt.tm_min * 60 + gt.tm_sec;
  int64_t diff = l - g;
  int dday = lt.tm_yday - gt.tm_yday;
  if (dday == 1 || dday < -1) diff += 86400;
  else if (dday == -1 || dday > 1) diff -= 86400;
  return (int)diff;
}

void d_write_out(const char* s, int n) { fwrite(s, 1, (size_t)n, stdout); fflush(stdout); }
void d_write_err(const char* s, int n) { fwrite(s, 1, (size_t)n, stderr); }
bool d_read_line(Str* out) {
  out->clear();
#if defined(_WIN32)
  // 端末から**直に**打たれているときは、UTF-16 で受け取る。
  // char で読む道は「その機械の言語の符号」を通るので、日本語が落ちることがある。
  // 流し込まれている（`< file` や `|`）ときは、下のふつうの読み方に落ちる
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
    Vec<wchar_t> line;
    bool any = false;
    for (;;) {
      wchar_t buf[512];
      DWORD got = 0;
      if (!ReadConsoleW(h, buf, (DWORD)(sizeof buf / sizeof buf[0]), &got, 0) || got == 0) break;
      any = true;
      bool eol = false;
      for (DWORD i = 0; i < got; i++) {
        if (buf[i] == L'\r') continue;
        if (buf[i] == L'\n') { eol = true; break; }
        line.push(buf[i]);
      }
      if (eol) break;   // 行が長いときは、続きをもう一度読む
    }
    if (line.size() > 0) {
      line.push(0);   // from_wide は 0 で終わる並びを取る
      *out = from_wide(&line[0]);
    }
    return any;
  }
#endif
  int c;
  bool any = false;
  while ((c = fgetc(stdin)) != EOF) {
    any = true;
    if (c == '\n') return true;
    if (c == '\r') continue;
    out->push((char)c);
  }
  return any;
}
void d_exit(int code) { exit(code); }

// --- ファイル ---
void* f_open(const char* path, const char* mode, Str* err) {
  const char* m = "rb";
  if (mode[0] == 'w') m = "wb";
  else if (mode[0] == 'a') m = "ab";
#if defined(_WIN32)
  Wide wp(path);
  wchar_t wm[4] = {(wchar_t)m[0], (wchar_t)m[1], 0, 0};
  FILE* f = _wfopen(wp.get(), wm);
#else
  FILE* f = fopen(path, m);
#endif
  if (!f) { *err = Str("ファイルを開けません: ") + path; return 0; }
  return (void*)f;
}
int f_read(void* h, char* buf, int n) { return (int)fread(buf, 1, (size_t)n, (FILE*)h); }
bool f_write(void* h, const char* buf, int n) { return (int)fwrite(buf, 1, (size_t)n, (FILE*)h) == n; }
void f_close(void* h) { fclose((FILE*)h); }
bool f_exists(const char* p) {
#if defined(_WIN32)
  Wide wp(p);
  return GetFileAttributesW(wp.get()) != INVALID_FILE_ATTRIBUTES;
#else
  struct stat st; return stat(p, &st) == 0;
#endif
}
bool f_is_dir(const char* p) {
#if defined(_WIN32)
  Wide wp(p);
  DWORD a = GetFileAttributesW(wp.get());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat st; if (stat(p, &st) != 0) return false; return S_ISDIR(st.st_mode);
#endif
}
bool f_size(const char* p, int64_t* out) {
#if defined(_WIN32)
  Wide wp(p);
  struct _stat64 st; if (_wstat64(wp.get(), &st) != 0) return false;   // 2GB を超えても数えられる
#else
  struct stat st; if (stat(p, &st) != 0) return false;
#endif
  *out = (int64_t)st.st_size; return true;
}
bool f_modified(const char* p, int64_t* ns) {
#if defined(_WIN32)
  Wide wp(p);
  struct _stat64 st; if (_wstat64(wp.get(), &st) != 0) return false;
#else
  struct stat st; if (stat(p, &st) != 0) return false;
#endif
  *ns = (int64_t)st.st_mtime * 1000000000ll; return true;
}
bool f_remove(const char* p, Str* err) {
#if defined(_WIN32)
  Wide wp(p);
  bool ok = f_is_dir(p) ? (RemoveDirectoryW(wp.get()) != 0) : (DeleteFileW(wp.get()) != 0);
  if (!ok) { *err = Str("消せません: ") + p; return false; }
  return true;
#else
  if (remove(p) != 0) { *err = Str("消せません: ") + p; return false; }
  return true;
#endif
}
bool f_rename(const char* a, const char* b, Str* err) {
#if defined(_WIN32)
  Wide wa(a), wb(b);
  // 行き先があれば置き換える（POSIX の rename と同じにする）
  if (!MoveFileExW(wa.get(), wb.get(), MOVEFILE_REPLACE_EXISTING)) {
    *err = Str("名前を変えられません: ") + a; return false;
  }
  return true;
#else
  if (rename(a, b) != 0) { *err = Str("名前を変えられません: ") + a; return false; }
  return true;
#endif
}
bool f_make_dir(const char* p, Str* err) {
  Str cur;
  const char* s = p;  // 途中の階層も順に作る
  for (int i = 0;; i++) {
    if (s[i] == '/' || s[i] == '\\' || s[i] == 0) {
      if (cur.size() > 0) {
#if defined(_WIN32)
        Wide wc(cur.c_str());
        CreateDirectoryW(wc.get(), 0);
#else
        mkdir(cur.c_str(), 0777);
#endif
      }
      if (s[i] == 0) break;
    }
    cur.push(s[i]);
  }
  if (!f_is_dir(p)) { *err = Str("作れません: ") + p; return false; }
  return true;
}
bool f_list(const char* dir, Vec<Str>* out, Str* err) {
#if defined(_WIN32)
  // Windows は「その中のもの」ではなく「この形に合うもの」を探す道具しか無いので、
  // 道のうしろに \* を足して「なんでも」にする
  Str pat(dir);
  if (pat.size() && pat[pat.size() - 1] != '/' && pat[pat.size() - 1] != '\\') pat += "\\";
  pat += "*";
  Wide wp(pat.c_str());
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(wp.get(), &fd);
  if (h == INVALID_HANDLE_VALUE) { *err = Str("開けません: ") + dir; return false; }
  do {
    const wchar_t* n = fd.cFileName;
    if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;   // . と ..
    out->push(from_wide(n));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return true;
#else
  DIR* d = opendir(dir);
  if (!d) { *err = Str("開けません: ") + dir; return false; }
  struct dirent* e;
  while ((e = readdir(d)) != 0) {
    if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
    out->push(Str(e->d_name));
  }
  closedir(d);
  return true;
#endif
}
const PlatformFile kFile = {f_open, f_read, f_write, f_close, f_exists, f_is_dir, f_size,
                            f_modified, f_remove, f_rename, f_make_dir, f_list};

// --- OS ---
const char* o_name() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__EMSCRIPTEN__)
  return "wasm";
#else
  return "linux";
#endif
}
bool o_env(const char* name, Str* out) {
#if defined(_WIN32)
  // getenv は CP932 で返すので、UTF-16 で取って UTF-8 に直す
  Wide wn(name);
  wchar_t buf[2048];
  DWORD n = GetEnvironmentVariableW(wn.get(), buf, (DWORD)(sizeof buf / sizeof buf[0]));
  if (n == 0) return false;
  if (n < (DWORD)(sizeof buf / sizeof buf[0])) { *out = from_wide(buf); return true; }
  wchar_t* big = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));   // 長いときは取り直す
  if (!big) return false;
  DWORD got = GetEnvironmentVariableW(wn.get(), big, n);
  bool ok = got > 0 && got < n;
  if (ok) *out = from_wide(big);
  free(big);
  return ok;
#else
  const char* v = getenv(name);
  if (!v) return false;
  *out = Str(v); return true;
#endif
}
void o_set_env(const char* name, const char* value) {
#if defined(_WIN32)
  Wide wn(name), wv(value);
  SetEnvironmentVariableW(wn.get(), wv.get());
#else
  setenv(name, value, 1);
#endif
}
bool o_cwd(Str* out) {
#if defined(_WIN32)
  wchar_t buf[4096];
  DWORD n = GetCurrentDirectoryW((DWORD)(sizeof buf / sizeof buf[0]), buf);
  if (n == 0 || n >= (DWORD)(sizeof buf / sizeof buf[0])) return false;
  *out = from_wide(buf); return true;
#else
  char buf[4096];
  if (!getcwd(buf, sizeof buf)) return false;
  *out = Str(buf); return true;
#endif
}
bool o_chdir(const char* p) {
#if defined(_WIN32)
  Wide wp(p);
  return SetCurrentDirectoryW(wp.get()) != 0;
#else
  return chdir(p) == 0;
#endif
}
const char* o_temp_dir() {
#if defined(_WIN32)
  // 一度調べて覚える（const char* を返すので、消えない置き場が要る）
  static Str dir;
  if (dir.size() == 0) {
    wchar_t buf[4096];
    DWORD n = GetTempPathW((DWORD)(sizeof buf / sizeof buf[0]), buf);
    if (n > 0 && n < (DWORD)(sizeof buf / sizeof buf[0])) {
      // うしろの \ は落とす（path.join が自分で足す）
      while (n > 0 && (buf[n - 1] == L'\\' || buf[n - 1] == L'/')) buf[--n] = 0;
      dir = from_wide(buf);
    }
    if (dir.size() == 0) dir = Str("C:\\Temp");
  }
  return dir.c_str();
#else
  const char* t = getenv("TMPDIR"); return t ? t : "/tmp";
#endif
}
// シェルを通さずに直接起動する。
// 文字列を並べてシェルに渡すと、引数の中身がそのまま命令として動いてしまうため。
bool o_run(const char* cmd, const Vec<Str>& args, int* code, Str* out, Str* err) {
#if defined(_WIN32)
  // Windows は引数を1本の文字列で渡すので、こちらで組み立てる。
  // 空白や " が入っていても1つの引数のままになるよう、決まった形で囲う
  // （CommandLineToArgvW が読み解く形）
  Str line;
  for (int i = -1; i < args.size(); i++) {
    const char* s = (i < 0) ? cmd : args[i].c_str();
    if (line.size()) line += " ";
    bool need = (s[0] == 0);
    for (int k = 0; s[k]; k++) if (s[k] == ' ' || s[k] == '\t' || s[k] == '"') need = true;
    if (!need) { line += s; continue; }
    line += "\"";
    for (int k = 0; s[k]; k++) {
      int bs = 0;
      while (s[k] == '\\') { bs++; k++; }
      if (s[k] == 0) {                                  // 閉じの " の直前の \ は2倍にする
        for (int t = 0; t < bs * 2; t++) line += "\\";
        break;
      }
      if (s[k] == '"') {                                // " の前の \ も2倍にして、\" と書く
        for (int t = 0; t < bs * 2 + 1; t++) line += "\\";
      } else {
        for (int t = 0; t < bs; t++) line += "\\";
      }
      line += s[k];
    }
    line += "\"";
  }

  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof sa;
  sa.lpSecurityDescriptor = 0;
  sa.bInheritHandle = TRUE;
  HANDLE or_ = 0, ow = 0, er = 0, ew = 0;
  if (!CreatePipe(&or_, &ow, &sa, 0)) { *err = Str("通り道を作れません"); return false; }
  if (!CreatePipe(&er, &ew, &sa, 0)) {
    CloseHandle(or_); CloseHandle(ow);
    *err = Str("通り道を作れません"); return false;
  }
  // 読む側は子に渡さない（渡すと、子が終わっても通り道が閉じず、読み続けて止まる）
  SetHandleInformation(or_, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(er, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = ow;
  si.hStdError = ew;
  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof pi);

  Wide wline(line.c_str());
  // CreateProcessW は渡した文字列を書き換えることがあるので、写しを渡す
  int wn = (int)wcslen(wline.get()) + 1;
  wchar_t* mut = (wchar_t*)malloc((size_t)wn * sizeof(wchar_t));
  if (!mut) {
    CloseHandle(or_); CloseHandle(ow); CloseHandle(er); CloseHandle(ew);
    *err = Str("起動できません: ") + cmd; return false;
  }
  for (int i = 0; i < wn; i++) mut[i] = wline.get()[i];
  BOOL started = CreateProcessW(0, mut, 0, 0, TRUE, CREATE_NO_WINDOW, 0, 0, &si, &pi);
  free(mut);
  CloseHandle(ow);   // 書く側はこちらでは使わない。閉じておかないと終端が来ない
  CloseHandle(ew);
  if (!started) {
    CloseHandle(or_); CloseHandle(er);
    *err = Str("起動できません: ") + cmd;
    return false;
  }

  // 片方だけ読んでいると、もう片方が詰まって止まるので、両方を代わるがわる見る
  bool o_open = true, e_open = true;
  char buf[4096];
  while (o_open || e_open) {
    bool got_any = false;
    HANDLE hs[2] = {or_, er};
    bool* opens[2] = {&o_open, &e_open};
    Str* dsts[2] = {out, err};
    for (int i = 0; i < 2; i++) {
      if (!*opens[i]) continue;
      DWORD avail = 0;
      if (!PeekNamedPipe(hs[i], 0, 0, 0, &avail, 0)) { *opens[i] = false; continue; }
      if (avail == 0) continue;
      DWORD want = avail < sizeof buf ? avail : (DWORD)sizeof buf;
      DWORD got = 0;
      if (!ReadFile(hs[i], buf, want, &got, 0) || got == 0) { *opens[i] = false; continue; }
      dsts[i]->append(buf, (int)got);
      got_any = true;
    }
    if (!got_any) {
      // まだ何も来ていない。相手が終わっていて、通り道も空なら終わり
      if (WaitForSingleObject(pi.hProcess, 1) == WAIT_OBJECT_0) {
        DWORD left = 0;
        bool more = (o_open && PeekNamedPipe(or_, 0, 0, 0, &left, 0) && left > 0) ||
                    (e_open && PeekNamedPipe(er, 0, 0, 0, &left, 0) && left > 0);
        if (!more) break;
      }
    }
  }
  CloseHandle(or_);
  CloseHandle(er);

  adopt_utf8(out);   // 相手が機械の符号で書いていたら、UTF-8 に直す
  adopt_utf8(err);

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD ec = 0;
  *code = GetExitCodeProcess(pi.hProcess, &ec) ? (int)ec : -1;
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return true;
#else
  int op[2], ep[2];
  if (pipe(op) != 0) { *err = Str("通り道を作れません"); return false; }
  if (pipe(ep) != 0) { close(op[0]); close(op[1]); *err = Str("通り道を作れません"); return false; }

  pid_t pid = fork();
  if (pid < 0) {
    close(op[0]); close(op[1]); close(ep[0]); close(ep[1]);
    *err = Str("起動できません: ") + cmd;
    return false;
  }
  if (pid == 0) {
    dup2(op[1], 1);
    dup2(ep[1], 2);
    close(op[0]); close(op[1]); close(ep[0]); close(ep[1]);
    // execvp に渡す並び。子の側なので、確保に失敗したらそのまま終わる
    char** argv = (char**)malloc(sizeof(char*) * (size_t)(args.size() + 2));
    if (!argv) _exit(127);
    argv[0] = (char*)cmd;
    for (int i = 0; i < args.size(); i++) argv[i + 1] = (char*)args[i].c_str();
    argv[args.size() + 1] = 0;
    execvp(cmd, argv);
    _exit(127);   // 起動できなかった
  }

  close(op[1]);
  close(ep[1]);
  // 片方だけ読んでいると、もう片方が詰まって止まるので、両方を見ながら読む
  bool o_open = true, e_open = true;
  char buf[4096];
  while (o_open || e_open) {
    struct pollfd fds[2];
    int n = 0;
    int oi = -1, ei = -1;
    if (o_open) { fds[n].fd = op[0]; fds[n].events = POLLIN; oi = n; n++; }
    if (e_open) { fds[n].fd = ep[0]; fds[n].events = POLLIN; ei = n; n++; }
    if (poll(fds, (nfds_t)n, -1) < 0) break;
    if (oi >= 0 && (fds[oi].revents & (POLLIN | POLLHUP))) {
      int got = (int)read(op[0], buf, sizeof buf);
      if (got > 0) out->append(buf, got);
      else o_open = false;
    }
    if (ei >= 0 && (fds[ei].revents & (POLLIN | POLLHUP))) {
      int got = (int)read(ep[0], buf, sizeof buf);
      if (got > 0) err->append(buf, got);
      else e_open = false;
    }
  }
  close(op[0]);
  close(ep[0]);

  int st = 0;
  while (waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) break;
  }
  *code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return true;
#endif
}
const PlatformOS kOS = {o_name, o_env, o_set_env, o_cwd, o_chdir, o_temp_dir, o_run};

// --- 乱数のもと ---
// std.crypto が使う。真の乱数（機器の雑音）と、OS の暗号用乱数の2つを渡す。
#if defined(SHARK_X86)
// RDSEED を持っているか。CPUID の 7 番、EBX の 18 ビット目。一度だけ調べて覚える
bool has_rdseed() {
  static int known = -1;
  if (known >= 0) return known != 0;
  unsigned int a = 0, b = 0, c = 0, d = 0;
#if defined(_MSC_VER)
  int regs[4];
  __cpuid(regs, 0);
  if (regs[0] < 7) { known = 0; return false; }
  __cpuidex(regs, 7, 0);
  b = (unsigned int)regs[1];
#else
  if (__get_cpuid_max(0, 0) < 7) { known = 0; return false; }
  __cpuid_count(7, 0, a, b, c, d);
#endif
  (void)a; (void)c; (void)d;
  known = (b & (1u << 18)) ? 1 : 0;
  return known != 0;
}
// 雑音源が追いつかないと失敗する。少しだけ待ち直して、それでも駄目なら諦める
bool rdseed64(uint64_t* out) {
  for (int retry = 0; retry < 32; retry++) {
#if defined(_MSC_VER)
    unsigned long long v = 0;
    if (_rdseed64_step(&v)) { *out = (uint64_t)v; return true; }
#else
    uint64_t v = 0;
    unsigned char ok = 0;
    __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok)::"cc");
    if (ok) { *out = v; return true; }
#endif
  }
  return false;
}
#endif

bool r_true(unsigned char* buf, int n) {
#if defined(SHARK_X86)
  if (!has_rdseed()) return false;
  for (int i = 0; i < n; i += 8) {
    uint64_t v = 0;
    if (!rdseed64(&v)) return false;
    for (int k = 0; k < 8 && i + k < n; k++) buf[i + k] = (unsigned char)(v >> (k * 8));
  }
  return true;
#else
  (void)buf; (void)n;   // 真の乱数を出す道具が無い機種
  return false;
#endif
}

bool r_secure(unsigned char* buf, int n) {
#if defined(_WIN32)
  for (int i = 0; i < n; i += 4) {   // rand_s は OS の暗号用乱数を返す
    unsigned int v = 0;
    if (rand_s(&v) != 0) return false;
    for (int k = 0; k < 4 && i + k < n; k++) buf[i + k] = (unsigned char)(v >> (k * 8));
  }
  return true;
#elif defined(__APPLE__)
  for (int i = 0; i < n; i += 256) {   // getentropy は一度に 256 バイトまで
    int m = n - i < 256 ? n - i : 256;
    if (getentropy(buf + i, (size_t)m) != 0) return false;
  }
  return true;
#else
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) return false;
  int got = 0;
  while (got < n) {
    int r = (int)read(fd, buf + got, (size_t)(n - got));
    if (r <= 0) { if (r < 0 && errno == EINTR) continue; close(fd); return false; }
    got += r;
  }
  close(fd);
  return true;
#endif
}
const PlatformRandom kRandom = {r_true, r_secure};

// --- 任意機能：画面 -------------------------------------------------------
//
// 窓を開く（macOS は AppKit、Windows は user32、Linux などは X11。
// どれも実行時に取りに行く）。
// 開けなければ画面なしになり、std.ui は見えない面に描く。
//
// 環境変数 SHARK_UI に off を入れると、窓を試さずに画面なしにできる。
#include "screen_mac.inc"
#include "screen_win.inc"
#include "screen_x11.inc"

const PlatformScreen* g_active = 0;
PlatformScreen kScreen;   // 中身は開いたときに、選んだものへ差し替える

void s_text_input(bool on, const char* initial, int x, int y, int h) {
  if (g_active && g_active->text_input) g_active->text_input(on, initial, x, y, h);
}
bool s_text_state(Str* confirmed, Str* marked) {
  return (g_active && g_active->text_state) ? g_active->text_state(confirmed, marked) : false;
}
bool s_text_selection(int* start, int* len) {
  return (g_active && g_active->text_selection) ? g_active->text_selection(start, len) : false;
}
void s_text_select(int start, int len) {
  if (g_active && g_active->text_select) g_active->text_select(start, len);
}
void s_text_replace(const char* s) {
  if (g_active && g_active->text_replace) g_active->text_replace(s);
}
// 切り貼りの置き場は、窓を開いていなくても使えることがある（macOS と Windows はそう）
bool s_clipboard_get(Str* out) {
  if (g_active && g_active->clipboard_get) return g_active->clipboard_get(out);
  if (const PlatformScreen* m = mac_screen()) if (m->clipboard_get) return m->clipboard_get(out);
  if (const PlatformScreen* w = win_screen()) if (w->clipboard_get) return w->clipboard_get(out);
  return false;
}
void s_clipboard_set(const char* s) {
  if (g_active && g_active->clipboard_set) { g_active->clipboard_set(s); return; }
  if (const PlatformScreen* m = mac_screen()) if (m->clipboard_set) { m->clipboard_set(s); return; }
  if (const PlatformScreen* w = win_screen()) if (w->clipboard_set) w->clipboard_set(s);
}

// SHARK_UI=off（none も同じ）なら窓を開かない。screen_canvas.inc と同じ見方
bool ui_off() {
  const char* p = getenv("SHARK_UI");
  if (!p || !p[0]) return false;
  return strcmp(p, "off") == 0 || strcmp(p, "none") == 0;
}

bool s_open(const char* title, int w, int h) {
  g_active = 0;
  if (ui_off()) return false;
  const PlatformScreen* order[3];
  int n = 0;
  if (const PlatformScreen* m = mac_screen()) order[n++] = m;
  if (const PlatformScreen* w2 = win_screen()) order[n++] = w2;
  if (const PlatformScreen* x = x11_screen()) order[n++] = x;
  for (int i = 0; i < n; i++) {
    if (order[i]->open(title, w, h)) {
      g_active = order[i];
      kScreen.has_key_up = order[i]->has_key_up;
      // 変換つきの文字入力を持っている出し先のときだけ、口を出す
      kScreen.text_input = order[i]->text_input ? s_text_input : 0;
      kScreen.text_state = order[i]->text_state ? s_text_state : 0;
      kScreen.text_selection = order[i]->text_selection ? s_text_selection : 0;
      kScreen.text_select = order[i]->text_select ? s_text_select : 0;
      kScreen.text_replace = order[i]->text_replace ? s_text_replace : 0;
      return true;
    }
  }
  return false;
}
void s_close() {
  if (g_active) g_active->close();
  g_active = 0;
}
void s_present(const uint32_t* px, int w, int h) {
  if (g_active) g_active->present(px, w, h);
}
bool s_poll(ScreenEvent* out) { return g_active ? g_active->poll(out) : false; }
void s_set_redraw(bool (*fn)(int, int)) {
  if (g_active && g_active->set_redraw) g_active->set_redraw(fn);
}
void s_set_resizable(bool on) {
  if (g_active && g_active->set_resizable) g_active->set_resizable(on);
}
void s_set_cursor(int kind) {
  if (g_active && g_active->set_cursor) g_active->set_cursor(kind);
}

// 画面ぜんたいの大きさ。細かさと同じく**開く前にも呼べる**。
// 窓を開かないと決まっているときは「言えない」を返す（見えない面に画面は無い）
bool s_screen_size(int* w, int* h) {
  if (ui_off()) return false;
  if (g_active && g_active->screen_size) return g_active->screen_size(w, h);
  if (const PlatformScreen* m = mac_screen()) if (m->screen_size) return m->screen_size(w, h);
  if (const PlatformScreen* v = win_screen()) if (v->screen_size) return v->screen_size(w, h);
  if (const PlatformScreen* x = x11_screen()) if (x->screen_size) return x->screen_size(w, h);
  return false;
}

// 画面の細かさ。**開く前にも呼べる**ので、まだ選んでいなければ開けそうな順に尋ねる。
// 窓を開かないと決まっているとき（SHARK_UI=off）は 1。見えない面に細かさは無い
int s_scale() {
  if (g_active) return g_active->scale ? g_active->scale() : 1;
  if (ui_off()) return 1;
  if (const PlatformScreen* m = mac_screen()) if (m->scale) return m->scale();
  if (const PlatformScreen* w = win_screen()) if (w->scale) return w->scale();
  if (const PlatformScreen* x = x11_screen()) if (x->scale) return x->scale();
  return 1;
}

// 中身は s_open が差し替える。ここでは入れ物だけ作る
struct ScreenInit {
  ScreenInit() {
    kScreen.scale = s_scale;
    kScreen.open = s_open;
    kScreen.close = s_close;
    kScreen.present = s_present;
    kScreen.poll = s_poll;
    kScreen.has_key_up = false;
    kScreen.text_input = 0;   // 選んだものが持っていれば s_open が入れる
    kScreen.text_state = 0;
    kScreen.text_selection = 0;
    kScreen.text_select = 0;
    kScreen.text_replace = 0;
    kScreen.clipboard_get = s_clipboard_get;   // 画面が無くても使えることがある
    kScreen.clipboard_set = s_clipboard_set;
    kScreen.set_redraw = s_set_redraw;
    kScreen.set_cursor = s_set_cursor;
    kScreen.set_resizable = s_set_resizable;
    kScreen.host_paced = false;   // 刻みはこちらで作る（眠って起きる）
    kScreen.screen_size = s_screen_size;
  }
};
ScreenInit g_screen_init;

const PlatformScreen* desktop_screen() { return &kScreen; }

const Platform kDesktop = {
    d_alloc, d_realloc, d_free, d_fatal,
    d_now, d_mono, d_sleep, d_local_offset,
    d_write_out, d_write_err, d_read_line, d_exit,
    &kFile, &kOS, &kRandom, desktop_screen(),
    0};   // 字は FreeType が読む（core/lib/font_ft.inc）ので、移植層は持たない

}  // namespace

const Platform* platform_desktop() { return &kDesktop; }

}  // namespace shark
