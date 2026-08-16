// desktop.cpp — Windows / macOS / Linux 向けの移植層
//
// ここを自分の機種のものに差し替える（spec/skeleton.md）。
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
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
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

const Platform kDesktop = {
    d_alloc, d_realloc, d_free, d_fatal,
    d_now, d_mono, d_sleep, d_local_offset,
    d_write_out, d_write_err, d_read_line, d_exit,
    &kFile, &kOS};

}  // namespace

const Platform* platform_desktop() { return &kDesktop; }

}  // namespace shark
