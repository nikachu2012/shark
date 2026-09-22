#include "shark.h"

#include "ast.h"
#include "check.h"
#include "codegen.h"
#include "parser.h"
#include "prelude.h"

namespace shark {

Engine::Engine(const Config& cfg)
    : cfg_(cfg), reg_(types_), arena_(0), prog_(0), checker_(0), ok_(false), loader_(0),
      loader_ud_(0), repl_(false), repl_count_(0), repl_inits_(0) {
  diag_.set_lang(cfg.lang);
  diag_.set_strict(cfg.strict);
  vm_.stack_size = cfg.stack_size;
  vm_.task_stack_size = cfg.task_stack_size;
  vm_.max_call_depth = cfg.max_call_depth;
  vm_.diag = &diag_;
  // 上限が見るのは、実行中のプログラムが使う量だけ（spec/runtime/memory.md）。
  // 数えるのは確保の口なので、1つのプロセスに1つ
  sk_mem_set_limit(cfg.memory_limit);

  // 標準ライブラリ。番号の並びは registry.cpp が決める（バイトコードがその番号を指す）
  register_modules(reg_, cfg);
}

Engine::~Engine() {
  ui_shutdown();   // 面を捨て、画面を開いたままなら閉じる（lib/ui.cpp）
  if (checker_) { checker_->~Checker(); sk_free(checker_); }
  if (prog_) { prog_->~Program(); sk_free(prog_); }
  if (arena_) { arena_->~Arena(); sk_free(arena_); }
}

int Engine::register_host(const char* name, NativeFn fn, Type* ret, Type* p0, Type* p1, Type* p2,
                          Type* p3) {
  int id = reg_.add(name, fn, ret, p0, p1, p2, p3);
  reg_.mark_host(id);
  return id;
}

void Engine::add_module(const Str& path, const Str& source, const Str& display) {
  mod_paths_.push(path);
  mod_sources_.push(source);
  mod_displays_.push(display.size() ? display : path);
}

bool Engine::find_module_source(const Str& path, Str* src, Str* display) {
  for (int i = 0; i < mod_paths_.size(); i++)
    if (mod_paths_[i] == path) {
      *src = mod_sources_[i];
      *display = mod_displays_[i];
      return true;
    }
  if (loader_) {
    Str d;
    if (loader_(loader_ud_, path, src, &d)) {
      *display = d.size() ? d : path;
      add_module(path, *src, *display);
      return true;
    }
  }
  return false;
}

static Str short_name_of(const Str& path) {
  int last = -1;
  for (int i = 0; i < path.size(); i++) if (path[i] == '.' || path[i] == '/') last = i;
  return path.sub(last + 1, path.size() - last - 1);
}

Unit* Engine::load_unit(const Str& path, const Str& source, const Str& display, bool is_entry,
                        int depth, int line, int col, int len) {
  for (int i = 0; i < loading_.size(); i++) {
    if (loading_[i] == path) {
      Diagnostic& d = diag_.error("E0502", diag_.L(Str("import が輪になっています: ") + path,
                                                   Str("circular import: ") + path));
      d.spans.push(Span(line, col, len));
      d.help.push(diag_.L("共通の部分を別のモジュールに切り出します",
                          "extract the shared part into a third module"));
      return 0;
    }
  }
  for (int i = 0; i < loaded_.size(); i++) if (loaded_[i] == path) return 0;  // もう読んである
  if (depth > 32) return 0;

  loading_.push(path);
  diag_.set_file(display);
  Parser parser(source, is_entry ? Str("main") : path, *arena_, diag_);
  Unit* u = parser.parse();
  u->display = display;
  u->is_entry = is_entry;
  u->module = is_entry ? Str("main") : path;

  // import を先に読む（依存の深い順に並べる）
  for (int i = 0; i < u->imports.size(); i++) {
    const ImportDecl& im = u->imports[i];
    const Str& p = im.path;
    bool is_std = p.size() > 4 && p.sub(0, 4) == "std.";
    if (is_std) {
      if (!reg_.has_module(p)) {
        Str avail;
        for (int k = 0; k < reg_.modules().size(); k++) {
          if (avail.size()) avail += ", ";
          avail += short_name_of(reg_.modules()[k]);
        }
        Diagnostic& d = diag_.error("E0501", diag_.L(Str("この処理系は ") + p + " を持っていません",
                                                     Str("this runtime does not include ") + p));
        d.spans.push(Span(im.line, im.col, im.len > 0 ? im.len : (int)p.size()));
        d.help.push(diag_.L(Str("この処理系が持つモジュール: ") + avail,
                            Str("available modules: ") + avail));
      }
      continue;
    }
    Str src, disp;
    if (!find_module_source(p, &src, &disp)) {
      Diagnostic& d = diag_.error("E0503", diag_.L(Str("モジュール ") + p + " が見つかりません",
                                                   Str("module not found: ") + p));
      d.spans.push(Span(im.line, im.col, im.len > 0 ? im.len : (int)p.size()));
      d.help.push(diag_.L("同じ場所に .shk があるか、名前の綴りを確かめます",
                          "check that the .shk file exists next to this one"));
      continue;
    }
    load_unit(p, src, disp, false, depth + 1, im.line, im.col, im.len > 0 ? im.len : (int)p.size());
  }

  loading_.pop();
  loaded_.push(path);
  units_.push(u);
  checker_->collect(u);
  return u;
}

void Engine::begin_program() {
  // 先に仮想マシンを片付ける（この後で Program を捨てるため）
  vm_.reset();
  if (checker_) { checker_->~Checker(); sk_free(checker_); checker_ = 0; }
  if (prog_) { prog_->~Program(); sk_free(prog_); prog_ = 0; }
  if (arena_) { arena_->~Arena(); sk_free(arena_); arena_ = 0; }
  units_.clear();
  loaded_.clear();
  loading_.clear();
  repl_ = false;

  arena_ = new (sk_alloc(sizeof(Arena))) Arena();
  prog_ = new (sk_alloc(sizeof(Program))) Program();
  prog_->types = &types_;
  checker_ = new (sk_alloc(sizeof(Checker))) Checker(*prog_, reg_, types_, diag_, *arena_);

  // Shark 自身で書いた部分を先に読む
  {
    Parser p(Str(kPreludeSource), Str("@prelude"), *arena_, diag_);
    Unit* pu = p.parse();
    pu->display = Str("@prelude");
    pu->module = Str("@prelude");
    units_.push(pu);
    loaded_.push(Str("@prelude"));
    checker_->collect(pu);
  }
  // 宣言的に書くときの入り口（ui.run）。これも Shark で書いてある。
  // std.ui を持つときだけ読み、モジュールは std.ui にする。
  // こうすると ui.run(...) が、ふつうのモジュールの関数として解決される
  if (reg_.has_module(Str("std.ui"))) {
    Parser p(Str(kUiRunSource), Str("@std.ui"), *arena_, diag_);
    Unit* uu = p.parse();
    uu->display = Str("@std.ui");
    uu->module = Str("std.ui");
    units_.push(uu);
    loaded_.push(Str("@std.ui"));
    checker_->collect(uu);
  }
}

// 前奏の中の警告は利用者に見せない
static void drop_prelude_warnings(Vec<Diagnostic>& items) {
  for (int i = items.size() - 1; i >= 0; i--)
    if ((items[i].file == "@prelude" || items[i].file == "@std.ui") &&
        items[i].severity == SEV_WARNING)
      items.remove(i);
}

const Vec<Diagnostic>& Engine::load(const Str& name, const Str& source) {
  diag_.clear();
  ok_ = false;
  begin_program();

  load_unit(Str("@entry"), source, name, true, 0);
  checker_->check_all();

  if (!diag_.has_error()) {
    CodeGen cg(*prog_, types_, reg_);
    cg.run();
    vm_.set_program(prog_, &reg_);
    vm_.start();
    ok_ = true;
  }
  drop_prelude_warnings(diag_.items());
  return diag_.items();
}

// ------------------------------------------------------------------ REPL
// 前奏だけを型検査・コード生成まで済ませておく。入力はこの上に足していく
void Engine::repl_begin() {
  begin_program();
  checker_->check_all();
  CodeGen cg(*prog_, types_, reg_);
  cg.run();
  vm_.set_program(prog_, &reg_);
  repl_ = true;
  repl_count_ = 0;
  repl_inits_ = 0;
}

const Vec<Diagnostic>& Engine::repl_eval(const Str& name, const Str& source) {
  diag_.clear();
  ok_ = false;
  if (!repl_) repl_begin();
  repl_count_++;

  // 通らなかったときに戻すための目印
  int nfuncs = prog_->funcs.size();
  int nclasses = prog_->classes.size();
  int nglobals = prog_->globals.size();
  int ninits = prog_->inits.size();
  int nunits = units_.size();
  int nloaded = loaded_.size();
  int cunits = checker_->unit_count();
  int cvkeys = checker_->vkey_count();
  int saved_entry = prog_->entry;
  prog_->entry = -1;

  // 入力ごとに別のモジュールとして読む（名前はどれも main。互いの宣言が見える）
  Str path = Str("@entry") + str_from_int(repl_count_);
  Unit* u = load_unit(path, source, name, true, 0);
  Vec<Renamed> renamed;
  if (u) {
    u->is_repl = true;
    // 前の入力で import したものは、続けて使える
    for (int i = nunits - 1; i >= 0; i--) {
      Unit* pu = units_[i];
      if (!pu->is_repl) continue;
      for (int k = 0; k < pu->imports.size(); k++) {
        const ImportDecl& im = pu->imports[k];
        bool dup = false;
        for (int m = 0; m < u->imports.size(); m++)
          if (u->imports[m].path == im.path && u->imports[m].alias == im.alias) dup = true;
        if (!dup) u->imports.push(im);
      }
    }
    // 同じ名前で定義し直したら、新しいほうが見えるようにする。
    // 前の定義は名前を変えて残す（それを使って作った値や関数は、前の定義のまま動く）
    Str tag = Str("@") + str_from_int(repl_count_);
    for (int i = 0; i < u->funcs.size(); i++)
      for (int k = 0; k < nfuncs; k++) {
        FuncInfo* f = prog_->funcs[k];
        if (f->owner || !(f->module == "main") || !(f->name == u->funcs[i]->name)) continue;
        Renamed r; r.name = &f->name; r.old = f->name;
        renamed.push(r);
        f->name += tag;
      }
    for (int i = 0; i < u->classes.size(); i++)
      for (int k = 0; k < nclasses; k++) {
        ClassInfo* c = prog_->classes[k];
        if (!(c->module == "main") || !(c->name == u->classes[i]->name)) continue;
        Renamed r; r.name = &c->name; r.old = c->name;
        renamed.push(r);
        c->name += tag;
      }
    for (int i = 0; i < u->globals.size(); i++)
      for (int k = 0; k < nglobals; k++) {
        GlobalInfo* g = prog_->globals[k];
        if (!(g->module == "main") || !(g->name == u->globals[i]->name)) continue;
        Renamed r; r.name = &g->name; r.old = g->name;
        renamed.push(r);
        g->name += tag;
      }
    checker_->check_all(cunits);
  }

  if (!u || diag_.has_error()) {
    // この入力は無かったことにする
    for (int i = prog_->funcs.size() - 1; i >= nfuncs; i--) {
      prog_->funcs[i]->~FuncInfo();
      sk_free(prog_->funcs[i]);
    }
    prog_->funcs.resize(nfuncs, 0);
    for (int i = prog_->classes.size() - 1; i >= nclasses; i--) {
      prog_->classes[i]->~ClassInfo();
      sk_free(prog_->classes[i]);
    }
    prog_->classes.resize(nclasses, 0);
    for (int i = prog_->globals.size() - 1; i >= nglobals; i--) {
      prog_->globals[i]->~GlobalInfo();
      sk_free(prog_->globals[i]);
    }
    prog_->globals.resize(nglobals, 0);
    prog_->inits.resize(ninits, 0);
    prog_->entry = saved_entry;
    for (int i = 0; i < renamed.size(); i++) *renamed[i].name = renamed[i].old;
    units_.resize(nunits, 0);
    loaded_.resize(nloaded, Str());
    checker_->truncate(cunits, cvkeys);
    repl_count_--;
    drop_prelude_warnings(diag_.items());
    return diag_.items();
  }

  CodeGen cg(*prog_, types_, reg_);
  cg.run(nfuncs);
  vm_.status = SK_Running;
  vm_.has_panic = false;
  vm_.aborted = false;
  vm_.error_message.clear();
  vm_.error_trace.clear();
  vm_.error_line = 0;
  vm_.exit_code = 0;
  // 足した import の初期化と、この入力の文だけを走らせる。グローバルはそのまま
  vm_.start(false, repl_inits_);
  repl_inits_ = prog_->inits.size();
  ok_ = true;
  drop_prelude_warnings(diag_.items());
  return diag_.items();
}

RunStatus Engine::step(int budget) { return vm_.step(budget); }

void Engine::find_tests(Vec<int>* out, Vec<Str>* names) {
  if (!prog_) return;
  for (int i = 0; i < prog_->funcs.size(); i++) {
    FuncInfo* f = prog_->funcs[i];
    if (!f->is_test || f->owner) continue;
    if (!(f->module == "main")) continue;
    out->push(i);
    names->push(f->name);
  }
}

void Engine::run_only(int func_index, bool with_inits) {
  // 関数を1つだけ走らせる（テストの前後の処理と、テスト本体に使う）
  int saved = prog_->entry;
  prog_->entry = func_index;
  vm_.status = SK_Running;
  vm_.has_panic = false;
  vm_.aborted = false;
  vm_.error_message.clear();
  vm_.error_trace.clear();
  vm_.start(with_inits);
  prog_->entry = saved;
}

// ------------------------------------------------------------------ 診断の整形
// 行末の改行は落とす。Windows で書かれたソースは行が \r\n で終わるので、
// 残したままだと診断に見えない \r が混ざる
static Str trim_cr(const Str& s) {
  if (s.size() > 0 && s[s.size() - 1] == '\r') return s.sub(0, s.size() - 1);
  return s;
}

static Str line_of(const Str& src, int line) {
  int cur = 1, start = 0;
  for (int i = 0; i < src.size(); i++) {
    if (cur == line && (src[i] == '\n' || i == src.size() - 1)) {
      int end = src[i] == '\n' ? i : i + 1;
      return trim_cr(src.sub(start, end - start));
    }
    if (src[i] == '\n') { cur++; start = i + 1; }
  }
  if (cur == line && start < src.size()) return trim_cr(src.sub(start, src.size() - start));
  return Str();
}

Str format_diagnostic(const Diagnostic& d, const Str& source, bool color, Lang lang) {
  const char* red = color ? "\x1b[31m" : "";
  const char* yellow = color ? "\x1b[33m" : "";
  const char* bold = color ? "\x1b[1m" : "";
  const char* dim = color ? "\x1b[2m" : "";
  const char* off = color ? "\x1b[0m" : "";

  Str r;
  r += d.severity == SEV_ERROR ? red : yellow;
  r += bold;
  r += d.severity == SEV_ERROR ? "error[" : "warning[";
  r += d.code;
  r += "]";
  r += off;
  r += bold;
  r += ": ";
  r += d.message;
  r += off;
  r += "\n";
  if (d.spans.size() > 0) {
    const Span& s0 = d.spans[0];
    r += dim;
    r += "  --> ";
    r += off;
    r += d.file;
    r += ":";
    r += str_from_int(s0.line);
    r += ":";
    r += str_from_int(s0.col);
    r += "\n";
    for (int i = 0; i < d.spans.size(); i++) {
      const Span& s = d.spans[i];
      Str src_line = line_of(source, s.line);
      if (src_line.size() == 0 && s.line > 0 && source.size() == 0) continue;
      Str num = str_from_int(s.line);
      if (i == 0 || d.spans[i - 1].line != s.line) {
        r += dim;
        r += "  ";
        r += num;
        r += " | ";
        r += off;
        r += src_line;
        r += "\n";
      }
      // 下線。桁は文字で数えるが、端末では全角が 2 つ分の幅を取るので、
      // 位置も長さも見た目の幅に直してから並べる
      r += dim;
      r += "  ";
      for (int k = 0; k < num.size(); k++) r += " ";
      r += " | ";
      r += off;
      int col = s.col > 0 ? s.col - 1 : 0;
      int len = s.len > 0 ? s.len : 1;
      int at = 0;
      for (int k = 0; k < col; k++) {
        if (at >= src_line.size()) { r += " "; continue; }   // 行の右を指しているとき
        int cp = 0;
        int adv = utf8_decode(src_line, at, &cp);
        if (src_line[at] == '\t') r += "\t";
        else {
          int w = utf8_display_width(src_line.sub(at, adv));
          for (int m = 0; m < w; m++) r += " ";
        }
        at += adv;
      }
      r += d.severity == SEV_ERROR ? red : yellow;
      int tail = utf8_offset(src_line, col + len);
      int width = tail > at ? utf8_display_width(src_line.sub(at, tail - at)) : len;
      for (int k = 0; k < width; k++) r += "-";
      if (s.label.size()) { r += " "; r += s.label; }
      r += off;
      r += "\n";
    }
  }
  for (int i = 0; i < d.help.size(); i++) {
    r += bold;
    r += lang == LANG_JA ? "  直し方: " : "  help: ";
    r += off;
    const Str& h = d.help[i];
    for (int k = 0; k < h.size(); k++) {
      r.push(h[k]);
      if (h[k] == '\n') r += "    ";   // 続きの行は字下げする
    }
    r += "\n";
  }
  return r;
}

}  // namespace shark
