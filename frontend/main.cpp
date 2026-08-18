// main.cpp — shark コマンド（spec/frontend.md）
//
// これは実行系（コア）の外側の実装。ファイルを読み、コアを呼び、
// 返ってきた診断を端末向けに整形する。コアはこのファイルを必要としない。
#include <stdio.h>
#include <stdlib.h>

#include "../core/platform/platform.h"
#include "../core/runtime.h"
#include "../core/shark.h"
#include "host.h"

namespace shark {

// ファイルの読み書き・自分の居場所・端末への出し方は host.h にある

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

// ------------------------------------------------------------------ 実行
static void setup(Engine& e) {
  e.set_io(host_io());
  e.set_module_loader(module_loader, 0);
}

static size_t g_memory_mb = 64;   // --memory で変えられる
static bool g_memory_given = false;  // 保存したバイトコードは、作ったときの量を引き継ぐ

static bool looks_like_bytecode(const Str& s) {
  if (s.size() < 4) return false;
  for (int i = 0; i < 4; i++) if (s[i] != kBytecodeMagic[i]) return false;
  return true;
}

// 保存したバイトコード（.shkc）を動かす。ここでは型検査もコード生成も要らない
static int cmd_run_bytecode(const Str& file, const Str& bytes, Lang lang, bool color) {
  Str err;
  BytecodeHeader h;
  if (!bytecode_read_header(bytes, &h, lang, &err)) {
    fprintf(stderr, "%s: %s\n", file.c_str(), err.c_str());
    return 2;
  }
  Config cfg;
  modules_to_config(h.modules, &cfg);   // 作ったときと同じ組み合わせにする
  cfg.lang = lang;
  cfg.memory_limit = (size_t)(g_memory_given ? (int)g_memory_mb : h.memory_mb) << 20;
  Runtime rt(cfg);
  rt.set_io(host_io());
  if (!rt.load(bytes, &err)) {
    fprintf(stderr, "%s: %s\n", file.c_str(), err.c_str());
    return 2;
  }
  if (!rt.has_entry()) {
    fprintf(stderr, "実行するものがありません\n");
    return 1;
  }
  return run_loop(rt.vm(), color);
}

static int cmd_run(const Str& file, bool check_only, Lang lang, bool strict, bool color) {
  Str src;
  if (!read_file(file, &src)) {
    fprintf(stderr, "ファイルを開けません: %s\n", file.c_str());
    return 2;
  }
  if (looks_like_bytecode(src)) {
    if (check_only) {
      fprintf(stderr,
              "%s は保存したバイトコードです。型検査はできません\n"
              "  直し方: もとの .shk を渡します\n", file.c_str());
      return 2;
    }
    return cmd_run_bytecode(file, src, lang, color);
  }
  g_base_dir = dir_of_path(file);
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
  return run_loop(e.vm(), color);
}

// ------------------------------------------------------------------ 作る（build）
static Str strip_ext(const Str& p) {
  for (int i = p.size() - 1; i >= 0; i--) {
    if (p[i] == '/' || p[i] == '\\') break;
    if (p[i] == '.') return p.sub(0, i);
  }
  return p;
}

static Str base_name(const Str& p) {
  for (int i = p.size() - 1; i >= 0; i--)
    if (p[i] == '/' || p[i] == '\\') return p.sub(i + 1, p.size() - i - 1);
  return p;
}

// 単一バイナリの土台になる実行装置（sharkvm）を探す。
//   1. --runtime <path>
//   2. 環境変数 SHARK_RUNTIME
//   3. shark 自身と同じ場所
//   4. いまいる場所
static bool find_runtime(const Str& given, Str* out, Str* tried) {
  Vec<Str> cand;
  if (given.size()) {
    cand.push(given);
  } else {
    Str env;
    if (platform().os && platform().os->env("SHARK_RUNTIME", &env) && env.size()) cand.push(env);
    Str self;
    if (exe_path(&self)) {
      cand.push(dir_of_path(self) + "/sharkvm");
      cand.push(dir_of_path(self) + "/sharkvm.exe");
    }
    cand.push(Str("./sharkvm"));
    cand.push(Str("./sharkvm.exe"));
  }
  for (int i = 0; i < cand.size(); i++) {
    if (platform().file && platform().file->exists(cand[i].c_str())) { *out = cand[i]; return true; }
    if (tried->size()) *tried += "\n            ";
    *tried += cand[i];
  }
  return false;
}

// 渡されたファイルが本当に実行装置かを見る（vm_main.cpp が持つ目印を探す）
static bool has_stub_mark(const Str& bytes) {
  Str mark = Str("shark-runtime") + "-stub-1";   // 分けて書く（この shark 自身と混ざらないように）
  for (int i = 0; i + mark.size() <= bytes.size(); i++) {
    bool same = true;
    for (int k = 0; k < mark.size(); k++) if (bytes[i + k] != mark[k]) { same = false; break; }
    if (same) return true;
  }
  return false;
}

static bool already_packed(const Str& bytes) {
  if (bytes.size() < kPackFooterLen) return false;
  int at = bytes.size() - kPackFooterLen;
  for (int i = 0; i < kPackMagicLen; i++) if (bytes[at + i] != kPackMagic[i]) return false;
  return true;
}

static int kb(int bytes) { return (bytes + 1023) / 1024; }

// ソースからバイトコードを作り、実行装置のうしろに埋めて、単一バイナリにする。
// --bytecode なら .shkc として保存するだけ（spec/runtime/bytecode.md）
static int cmd_build(const Str& file, const Str& out_given, bool bytecode_only,
                     const Str& runtime_given, Lang lang, bool strict, bool color) {
  Str src;
  if (!read_file(file, &src)) {
    fprintf(stderr, "ファイルを開けません: %s\n", file.c_str());
    return 2;
  }
  if (looks_like_bytecode(src)) {
    fprintf(stderr,
            "%s は、もう作ったバイトコードです\n"
            "  直し方: もとの .shk を渡します\n", file.c_str());
    return 2;
  }
  g_base_dir = dir_of_path(file);
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
  if (!e.has_entry()) {
    fprintf(stderr,
            "実行するものがありません\n"
            "  直し方: func main() -> int { } を書くか、文をそのまま並べます\n");
    return 1;
  }

  BytecodeHeader h;
  h.main_file = file;
  h.lang = lang;
  h.memory_mb = (int)g_memory_mb;   // 動かすときの上限は、ここで決まる
  h.modules = modules_bits(cfg);
  Str code, err;
  if (!bytecode_write(*e.program(), e.registry(), h, &code, &err)) {
    fprintf(stderr, "バイトコードにできません: %s\n", err.c_str());
    return 1;
  }

  if (bytecode_only) {
    Str out = out_given.size() ? out_given : strip_ext(file) + ".shkc";
    if (!write_file(out, code, false)) {
      fprintf(stderr, "書き出せません: %s\n", out.c_str());
      return 2;
    }
    printf("%s を作りました（バイトコード %d KB）\n", out.c_str(), kb(code.size()));
    printf("  動かす: ./sharkvm %s\n", out.c_str());
    return 0;
  }

  Str rt_path, tried;
  if (!find_runtime(runtime_given, &rt_path, &tried)) {
    fprintf(stderr,
            "実行装置（sharkvm）が見つかりません\n"
            "  探した場所: %s\n"
            "  直し方: make sharkvm で作ります。場所は --runtime <path> でも渡せます\n",
            tried.c_str());
    return 2;
  }
  Str stub;
  if (!read_file(rt_path, &stub)) {
    fprintf(stderr, "実行装置を読めません: %s\n", rt_path.c_str());
    return 2;
  }
  if (already_packed(stub)) {
    fprintf(stderr,
            "%s は、もうバイトコードを埋めたものです\n"
            "  直し方: 埋めていない sharkvm を渡します\n", rt_path.c_str());
    return 2;
  }
  if (!has_stub_mark(stub)) {
    fprintf(stderr,
            "%s は Shark の実行装置ではないようです\n"
            "  直し方: make sharkvm で作ったものを渡します\n", rt_path.c_str());
    return 2;
  }

  Str out = out_given.size() ? out_given : base_name(strip_ext(file));
  Str packed = stub;
  packed += code;
  packed += pack_footer(code.size());
  if (!write_file(out, packed, true)) {
    fprintf(stderr, "書き出せません: %s\n", out.c_str());
    return 2;
  }
  printf("%s を作りました（実行装置 %d KB ＋ バイトコード %d KB＝%d KB）\n", out.c_str(),
         kb(stub.size()), kb(code.size()), kb(packed.size()));
  printf("  動かす: ./%s\n", base_name(out).c_str());
  return 0;
}

static int cmd_test(const Str& file, Lang lang, bool color, const Str& filter) {
  Str src;
  if (!read_file(file, &src)) {
    fprintf(stderr, "ファイルを開けません: %s\n", file.c_str());
    return 2;
  }
  if (looks_like_bytecode(src)) {
    fprintf(stderr,
            "%s は保存したバイトコードです。テストは走らせられません\n"
            "  直し方: もとの .shk を渡します\n", file.c_str());
    return 2;
  }
  g_base_dir = dir_of_path(file);
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
  // トップレベルの文（test.before_each の登録など）を先に済ませる
  test_reset_hooks();
  e.run_only(e.has_entry() ? e.program()->entry : -1, true);
  if (run_loop(e.vm(), color) != 0) return 1;

  int passed = 0;
  for (int i = 0; i < tests.size(); i++) {
    test_begin();
    if (test_before_index() >= 0) {
      e.run_only(test_before_index(), false);
      run_loop(e.vm(), color);
    }
    e.run_only(tests[i], false);
    int rc = run_loop(e.vm(), color);
    if (test_after_index() >= 0) {
      e.run_only(test_after_index(), false);
      run_loop(e.vm(), color);
    }
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
      "  shark run <file.shk>      実行する（.shkc を渡すと、保存したバイトコードを動かす）\n"
      "  shark check <file.shk>    型検査だけを行う\n"
      "  shark build <file.shk>    どこでも動く1つのファイルにする（実行装置＋バイトコード）\n"
      "  shark test [file.shk]     test_ で始まる関数を走らせる（省略すると *_test.shk 全部）\n"
      "  shark explain E0102       エラーの詳しい説明を出す\n"
      "  shark modules             この処理系が持つモジュールを並べる\n"
      "\n"
      "選べるもの:\n"
      "  --lang ja|en   診断の言語（既定は ja）\n"
      "  --memory <MB>  使ってよいメモリの量。超えたら実行時エラー（既定は 64、0 で上限なし）\n"
      "  --strict       警告もエラーとして扱う\n"
      "  --no-color     色を付けない（環境変数 NO_COLOR でも同じ）\n"
      "\n"
      "build のときだけ:\n"
      "  -o <name>      作るものの名前（省くと、ソースの名前から決める）\n"
      "  --bytecode     単一バイナリにせず、バイトコード（.shkc）だけ保存する\n"
      "  --runtime <p>  土台にする実行装置（省くと shark の隣の sharkvm を使う）\n"
      "  --memory <MB>  作ったものが動くときの上限も、ここで決まる\n");
}

int main_impl(int argc, char** argv) {
  platform_set(platform_desktop());
  Lang lang = LANG_JA;
  bool strict = false;
  bool color = color_default();
  Str out_path;          // build の -o
  bool bytecode_only = false;
  Str runtime_path;      // build の --runtime
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
      g_memory_given = true;
      continue;
    }
    if ((a == "-o" || a == "--out") && i + 1 < argc) { out_path = Str(argv[++i]); continue; }
    if (a == "--bytecode") { bytecode_only = true; continue; }
    if (a == "--runtime" && i + 1 < argc) { runtime_path = Str(argv[++i]); continue; }
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
  if (cmd == "build") {
    if (rest.size() < 2) { usage(); return 2; }
    return cmd_build(rest[1], out_path, bytecode_only, runtime_path, lang, strict, color);
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
