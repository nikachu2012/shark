// main.cpp — shark コマンド（spec/frontend.md）
//
// これは実行系（コア）の外側の実装。ファイルを読み、コアを呼び、
// 返ってきた診断を端末向けに整形する。コアはこのファイルを必要としない。
#include <stdio.h>
#include <stdlib.h>

#include "../core/platform/platform.h"
#include "../core/shark.h"

namespace shark {

// ------------------------------------------------------------------ 道具
static bool read_file(const Str& path, Str* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, (int)n);
  fclose(f);
  return true;
}

static void out_str(void* ud, const char* s, int n) {
  (void)ud;
  fwrite(s, 1, (size_t)n, stdout);
}
static bool in_line(void* ud, Str* out) {
  (void)ud;
  return platform().read_line(out);
}

static Lang g_lang = LANG_JA;

struct SourceMap {
  Vec<Str> names;
  Vec<Str> sources;
  void add(const Str& n, const Str& s) { names.push(n); sources.push(s); }
  Str find(const Str& n) const {
    for (int i = 0; i < names.size(); i++) if (names[i] == n) return sources[i];
    return Str();
  }
};

static SourceMap g_sources;
static Str g_base_dir;

static Str dir_of(const Str& path) {
  for (int i = path.size() - 1; i >= 0; i--)
    if (path[i] == '/' || path[i] == '\\') return path.sub(0, i);
  return Str(".");
}

static Str clean_path(const Str& p) {
  // "a/./b" のような形をならす（表示を読みやすくするだけ）
  Str r;
  for (int i = 0; i < p.size(); i++) {
    if (p[i] == '.' && i + 1 < p.size() && p[i + 1] == '/' && r.size() > 0 && r[r.size() - 1] == '/') {
      i++;
      continue;
    }
    r.push(p[i]);
  }
  return r;
}

// import の探し方（spec/runtime/module.md）
//   1. ./ ../ で始まるものは、実行するファイルからの相対
//   2. それ以外は、プロジェクトの lib/ → 環境変数 SHARK_PATH
static bool module_loader(void* ud, const Str& path, Str* src, Str* display) {
  (void)ud;
  Vec<Str> tries;
  tries.push(clean_path(g_base_dir + "/" + path + ".shk"));
  tries.push(clean_path(g_base_dir + "/lib/" + path + ".shk"));
  Str env;
  if (platform().os && platform().os->env("SHARK_PATH", &env)) {
    Str cur;
    for (int i = 0; i <= env.size(); i++) {
      if (i == env.size() || env[i] == ':') {
        if (cur.size()) tries.push(clean_path(cur + "/" + path + ".shk"));
        cur.clear();
        continue;
      }
      cur.push(env[i]);
    }
  }
  for (int i = 0; i < tries.size(); i++) {
    if (!read_file(tries[i], src)) continue;
    *display = tries[i];
    g_sources.add(tries[i], *src);
    return true;
  }
  return false;
}

static void print_diags(const Vec<Diagnostic>& ds, bool color) {
  for (int i = 0; i < ds.size(); i++) {
    Str src = g_sources.find(ds[i].file);
    Str text = format_diagnostic(ds[i], src, color, g_lang);
    fwrite(text.data(), 1, (size_t)text.size(), stderr);
    fputc('\n', stderr);
  }
}

static int count_errors(const Vec<Diagnostic>& ds) {
  int n = 0;
  for (int i = 0; i < ds.size(); i++) if (ds[i].severity == SEV_ERROR) n++;
  return n;
}

static void print_panic(Engine& e, bool color) {
  const char* red = color ? "\x1b[31m\x1b[1m" : "";
  const char* off = color ? "\x1b[0m" : "";
  Str r;
  r += red;
  r += "panic: ";
  r += off;
  r += e.error_message();
  r += "\n";
  if (e.error_line() > 0) {
    r += "  --> ";
    r += e.error_file();
    r += ":";
    r += str_from_int(e.error_line());
    r += "\n";
  }
  if (e.error_trace().size()) {
    r += "  呼び出しの経路:\n";
    r += e.error_trace();
  }
  fwrite(r.data(), 1, (size_t)r.size(), stderr);
}

// ------------------------------------------------------------------ 実行
static int run_loop(Engine& e, bool color) {
  for (;;) {
    RunStatus st = e.step(200000);
    if (st == SK_Finished) return e.exit_code();
    if (st == SK_Error) {
      print_panic(e, color);
      return 1;
    }
    if (e.idle()) platform().sleep_nanos(500000);  // 1000分の0.5秒だけ休む
  }
}

static void setup(Engine& e) {
  HostIO io;
  io.write_out = out_str;
  io.read_line = in_line;
  e.set_io(io);
  e.set_module_loader(module_loader, 0);
}

static size_t g_memory_mb = 64;   // --memory で変えられる

static int cmd_run(const Str& file, bool check_only, Lang lang, bool strict, bool color) {
  Str src;
  if (!read_file(file, &src)) {
    fprintf(stderr, "ファイルを開けません: %s\n", file.c_str());
    return 2;
  }
  g_base_dir = dir_of(file);
  g_sources.add(file, src);
  Config cfg;
  cfg.lang = lang;
  cfg.strict = strict;
  cfg.memory_limit = g_memory_mb << 20;
  Engine e(cfg);
  setup(e);
  const Vec<Diagnostic>& ds = e.load(file, src);
  print_diags(ds, color);
  int errs = count_errors(ds);
  if (errs > 0) {
    fprintf(stderr, "%d 件の誤りがあります\n", errs);
    return 1;
  }
  if (check_only) {
    if (ds.size() == 0) printf("問題ありません\n");
    return 0;
  }
  if (!e.has_entry()) {
    fprintf(stderr,
            "実行するものがありません\n"
            "  直し方: func main() -> int { } を書くか、文をそのまま並べます\n");
    return 1;
  }
  return run_loop(e, color);
}

static int cmd_test(const Str& file, Lang lang, bool color, const Str& filter) {
  Str src;
  if (!read_file(file, &src)) {
    fprintf(stderr, "ファイルを開けません: %s\n", file.c_str());
    return 2;
  }
  g_base_dir = dir_of(file);
  g_sources.add(file, src);
  Config cfg;
  cfg.lang = lang;
  cfg.memory_limit = g_memory_mb << 20;
  Engine e(cfg);
  setup(e);
  const Vec<Diagnostic>& ds = e.load(file, src);
  print_diags(ds, color);
  if (count_errors(ds) > 0) return 1;

  Vec<int> tests;
  Vec<Str> names;
  e.find_tests(&tests, &names);
  if (filter.size()) {
    Vec<int> t2;
    Vec<Str> n2;
    for (int i = 0; i < names.size(); i++) {
      bool hit = false;
      for (int k = 0; k + filter.size() <= names[i].size(); k++) {
        bool same = true;
        for (int m = 0; m < filter.size(); m++)
          if (names[i][k + m] != filter[m]) { same = false; break; }
        if (same) { hit = true; break; }
      }
      if (hit) { t2.push(tests[i]); n2.push(names[i]); }
    }
    tests = t2;
    names = n2;
  }
  printf("%s\n", file.c_str());
  int passed = 0;
  for (int i = 0; i < tests.size(); i++) {
    test_begin();
    e.run_only(tests[i]);
    int rc = run_loop(e, color);
    bool ok = !test_failed() && rc == 0;
    if (ok) {
      passed++;
      printf("  ok    %s\n", names[i].c_str());
    } else {
      printf("  fail  %s\n", names[i].c_str());
      if (test_desc().size()) printf("        %s\n", test_desc().c_str());
      if (test_message().size()) printf("        %s\n", test_message().c_str());
    }
  }
  printf("\n%d 件中 %d 件成功\n", tests.size(), passed);
  return passed == tests.size() ? 0 : 1;
}

// いまの場所以下の *_test.shk をまとめて走らせる
static int cmd_test_dir(Lang lang, bool color, const Str& filter) {
  Vec<Str> names;
  Str err;
  if (!platform().file || !platform().file->list(".", &names, &err)) {
    fprintf(stderr, "いまいる場所の一覧を取れません\n");
    return 2;
  }
  int rc = 0;
  int files = 0;
  for (int i = 0; i < names.size(); i++) {
    const Str& n = names[i];
    if (n.size() < 10) continue;
    if (!(n.sub(n.size() - 9, 9) == "_test.shk")) continue;
    files++;
    if (cmd_test(n, lang, color, filter) != 0) rc = 1;
  }
  if (files == 0) {
    printf("*_test.shk が見つかりません\n");
    return 0;
  }
  return rc;
}

static int cmd_explain(const Str& code, Lang lang) {
  const char* text = diag_explain(code.c_str(), lang);
  if (!text) {
    printf("%s という番号の説明はありません\n", code.c_str());
    return 1;
  }
  printf("%s\n%s\n", code.c_str(), text);
  return 0;
}

static void usage() {
  printf(
      "Shark🦈  ゲーム機で動く学習用プログラミング言語\n"
      "\n"
      "使い方:\n"
      "  shark run <file.shk>      実行する\n"
      "  shark check <file.shk>    型検査だけを行う\n"
      "  shark test [file.shk]     test_ で始まる関数を走らせる（省略すると *_test.shk 全部）\n"
      "  shark explain E0102       エラーの詳しい説明を出す\n"
      "  shark modules             この処理系が持つモジュールを並べる\n"
      "\n"
      "選べるもの:\n"
      "  --lang ja|en   診断の言語（既定は ja）\n"
      "  --memory <MB>  使ってよいメモリの量。超えたら実行時エラー（既定は 64、0 で上限なし）\n"
      "  --strict       警告もエラーとして扱う\n"
      "  --no-color     色を付けない\n");
}

int main_impl(int argc, char** argv) {
  platform_set(platform_desktop());
  Lang lang = LANG_JA;
  bool strict = false;
  bool color = true;
  Vec<Str> rest;
  Vec<Str> script_args;
  for (int i = 1; i < argc; i++) {
    Str a(argv[i]);
    if (a == "--lang" && i + 1 < argc) {
      lang = Str(argv[++i]) == "en" ? LANG_EN : LANG_JA;
      g_lang = lang;
      continue;
    }
    if (a == "--strict") { strict = true; continue; }
    if (a == "--memory" && i + 1 < argc) {
      int64_t mb = 0;
      if (!str_to_int(Str(argv[++i]), &mb) || mb < 0) {
        fprintf(stderr, "--memory には MB の数を渡します（例: --memory 32）\n");
        return 2;
      }
      g_memory_mb = (size_t)mb;
      continue;
    }
    if (a == "--no-color") { color = false; continue; }
    if (a == "-h" || a == "--help") { usage(); return 0; }
    rest.push(a);
  }
  if (rest.size() == 0) { usage(); return 0; }
  Str cmd = rest[0];
  for (int i = 1; i < rest.size(); i++) script_args.push(rest[i]);
  os_set_args(script_args);

  if (cmd == "run") {
    if (rest.size() < 2) { usage(); return 2; }
    return cmd_run(rest[1], false, lang, strict, color);
  }
  if (cmd == "check") {
    if (rest.size() < 2) { usage(); return 2; }
    return cmd_run(rest[1], true, lang, strict, color);
  }
  if (cmd == "test") {
    Str filter;
    Str target;
    for (int i = 1; i < rest.size(); i++) {
      if (rest[i] == "--filter" && i + 1 < rest.size()) { filter = rest[++i]; continue; }
      if (target.size() == 0) target = rest[i];
    }
    if (target.size() == 0) return cmd_test_dir(lang, color, filter);
    return cmd_test(target, lang, color, filter);
  }
  if (cmd == "explain") {
    if (rest.size() < 2) { usage(); return 2; }
    return cmd_explain(rest[1], lang);
  }
  if (cmd == "modules") {
    Config cfg;
    Engine e(cfg);
    const Vec<Str>& m = e.module_list();
    for (int i = 0; i < m.size(); i++) printf("%s\n", m[i].c_str());
    return 0;
  }
  if (cmd == "version") {
    printf("shark 0.1.0\n");
    return 0;
  }
  // 拡張子が .shk なら run とみなす
  if (cmd.size() > 4 && cmd.sub(cmd.size() - 4, 4) == ".shk") return cmd_run(cmd, false, lang, strict, color);
  fprintf(stderr, "知らないコマンドです: %s\n", cmd.c_str());
  usage();
  return 2;
}

}  // namespace shark

int main(int argc, char** argv) { return shark::main_impl(argc, argv); }
