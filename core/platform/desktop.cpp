// desktop.cpp — Windows / macOS / Linux 向けの移植層
//
// ここを自分の機種のものに差し替える（spec/skeleton.md）。
#if defined(_WIN32)
#define _CRT_RAND_S   // 暗号用の乱数 rand_s を使う。stdlib.h より前に要る
#endif

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
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

void* d_alloc(size_t n) { return malloc(n ? n : 1); }
void* d_realloc(void* p, size_t n) { return realloc(p, n ? n : 1); }
void  d_free(void* p) { free(p); }
void  d_fatal(const char* msg) { fputs(msg, stderr); fputc('\n', stderr); abort(); }

int64_t d_now() {
#if defined(_WIN32)
  return (int64_t)time(0) * 1000000000ll;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
#endif
}
int64_t d_mono() {
#if defined(_WIN32)
  return (int64_t)clock() * (1000000000ll / CLOCKS_PER_SEC);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
#endif
}
void d_sleep(int64_t n) {
  if (n <= 0) return;
#if defined(_WIN32)
  extern void Sleep(unsigned long);
  Sleep((unsigned long)(n / 1000000));
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
  FILE* f = fopen(path, m);
  if (!f) { *err = Str("ファイルを開けません: ") + path; return 0; }
  return (void*)f;
}
int f_read(void* h, char* buf, int n) { return (int)fread(buf, 1, (size_t)n, (FILE*)h); }
bool f_write(void* h, const char* buf, int n) { return (int)fwrite(buf, 1, (size_t)n, (FILE*)h) == n; }
void f_close(void* h) { fclose((FILE*)h); }
bool f_exists(const char* p) {
#if defined(_WIN32)
  return _access(p, 0) == 0;
#else
  struct stat st; return stat(p, &st) == 0;
#endif
}
bool f_is_dir(const char* p) {
#if defined(_WIN32)
  struct _stat st; if (_stat(p, &st) != 0) return false; return (st.st_mode & _S_IFDIR) != 0;
#else
  struct stat st; if (stat(p, &st) != 0) return false; return S_ISDIR(st.st_mode);
#endif
}
bool f_size(const char* p, int64_t* out) {
#if defined(_WIN32)
  struct _stat st; if (_stat(p, &st) != 0) return false;
#else
  struct stat st; if (stat(p, &st) != 0) return false;
#endif
  *out = (int64_t)st.st_size; return true;
}
bool f_modified(const char* p, int64_t* ns) {
#if defined(_WIN32)
  struct _stat st; if (_stat(p, &st) != 0) return false;
#else
  struct stat st; if (stat(p, &st) != 0) return false;
#endif
  *ns = (int64_t)st.st_mtime * 1000000000ll; return true;
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
  const char* s = p;  // 途中の階層も順に作る
  for (int i = 0;; i++) {
    if (s[i] == '/' || s[i] == '\\' || s[i] == 0) {
      if (cur.size() > 0) {
#if defined(_WIN32)
        _mkdir(cur.c_str());
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
  *err = Str("一覧を取れません: ") + dir;
  return false;
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
  const char* v = getenv(name);
  if (!v) return false;
  *out = Str(v); return true;
}
void o_set_env(const char* name, const char* value) {
#if defined(_WIN32)
  Str s = Str(name) + "=" + value; _putenv(s.c_str());
#else
  setenv(name, value, 1);
#endif
}
bool o_cwd(Str* out) {
  char buf[4096];
#if defined(_WIN32)
  if (!_getcwd(buf, sizeof buf)) return false;
#else
  if (!getcwd(buf, sizeof buf)) return false;
#endif
  *out = Str(buf); return true;
}
bool o_chdir(const char* p) {
#if defined(_WIN32)
  return _chdir(p) == 0;
#else
  return chdir(p) == 0;
#endif
}
const char* o_temp_dir() {
#if defined(_WIN32)
  const char* t = getenv("TEMP"); return t ? t : "C:\\Temp";
#else
  const char* t = getenv("TMPDIR"); return t ? t : "/tmp";
#endif
}
// シェルを通さずに直接起動する。
// 文字列を並べてシェルに渡すと、引数の中身がそのまま命令として動いてしまうため。
bool o_run(const char* cmd, const Vec<Str>& args, int* code, Str* out, Str* err) {
#if defined(_WIN32)
  (void)cmd; (void)args; (void)code; (void)out;
  *err = Str("この移植層は外のプログラムを呼べません");
  return false;
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
// 出せるところに出す。順に試して、最初に開けたものを使う。
//
//   1. 窓（macOS は AppKit、Linux などは X11。どちらも実行時に取りに行く）
//   2. 端末（升目1つに上下2画素）
//   3. どちらも駄目なら、画面なし（std.ui は見えない面に描く）
//
// 環境変数 SHARK_UI で選べる。window / terminal / off
#include "screen_mac.inc"
#include "screen_x11.inc"
#include "screen_term.inc"

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
// 切り貼りの置き場は、窓を開いていなくても使えることがある（macOS はそう）
bool s_clipboard_get(Str* out) {
  if (g_active && g_active->clipboard_get) return g_active->clipboard_get(out);
  if (const PlatformScreen* m = mac_screen()) if (m->clipboard_get) return m->clipboard_get(out);
  return false;
}
void s_clipboard_set(const char* s) {
  if (g_active && g_active->clipboard_set) { g_active->clipboard_set(s); return; }
  if (const PlatformScreen* m = mac_screen()) if (m->clipboard_set) m->clipboard_set(s);
}

bool s_open(const char* title, int w, int h) {
  const char* pick = getenv("SHARK_UI");
  bool want_win = true, want_term = true;
  if (pick) {
    if (strcmp(pick, "window") == 0) want_term = false;
    else if (strcmp(pick, "terminal") == 0 || strcmp(pick, "term") == 0) want_win = false;
    else if (strcmp(pick, "off") == 0 || strcmp(pick, "none") == 0) want_win = want_term = false;
  }
  const PlatformScreen* order[3];
  int n = 0;
  if (want_win) {
    if (const PlatformScreen* m = mac_screen()) order[n++] = m;
    if (const PlatformScreen* x = x11_screen()) order[n++] = x;
  }
  if (want_term) {
    if (const PlatformScreen* t = term_screen()) order[n++] = t;
  }
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
  g_active = 0;
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

// 画面の細かさ。**開く前にも呼べる**ので、まだ選んでいなければ出せそうな順に尋ねる。
// 窓を使わないと決まっているとき（SHARK_UI）は 1。端末にも見えない面にも細かさは無い
int s_scale() {
  if (g_active) return g_active->scale ? g_active->scale() : 1;
  const char* pick = getenv("SHARK_UI");
  if (pick && pick[0] && strcmp(pick, "window") != 0) return 1;
  if (const PlatformScreen* m = mac_screen()) if (m->scale) return m->scale();
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
    kScreen.set_resizable = s_set_resizable;
  }
};
ScreenInit g_screen_init;

const PlatformScreen* desktop_screen() { return &kScreen; }

const Platform kDesktop = {
    d_alloc, d_realloc, d_free, d_fatal,
    d_now, d_mono, d_sleep, d_local_offset,
    d_write_out, d_write_err, d_read_line, d_exit,
    &kFile, &kOS, &kRandom, desktop_screen()};

}  // namespace

const Platform* platform_desktop() { return &kDesktop; }

}  // namespace shark
