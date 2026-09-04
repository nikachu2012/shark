// shark_web.cpp — ブラウザ側のホスト（spec/runtime/embedding.md）
//
// これはコアの外側の実装。frontend/main.cpp が端末に向けてしていることを、
// そのままブラウザに向けて行う。コアはこのファイルを必要としない。
//
//   1. 処理系を作る            shk_load()
//   2. 少しずつ動かす          shk_pump()   ← 画面の更新1回につき1度呼ぶ
//   3. 出力を受け取る          shk_out_ptr() / shk_out_len()
//
// 無限ループを書かれても固まらないのは、組み込みと同じ仕組み。
// 1回に進める命令の数を JavaScript 側が決めて渡す。
//
// 文字列を返すものは、次に同じ関数を呼ぶまでの間だけ中身が保つ。
#include <emscripten/emscripten.h>

#include "../core/platform/web.h"
#include "../core/registry.h"   // ui_shutdown（core/lib/ui.cpp）
#include "../core/fmt_src.h"
#include "../core/shark.h"

using namespace shark;

namespace {

// ------------------------------------------------------------ 出入り口
Str g_out;          // print と write が書いたもの。JavaScript が読んでは空にする
Str g_in;           // input() に返す行。JavaScript が打たれたそばから入れる
int g_in_pos = 0;
int g_eof_at = -1;  // ここまで読んだら終端を1度返す（端末の Ctrl-D にあたる）。-1 は無し

void on_output(void* ud, const char* s, int n) {
  (void)ud;
  g_out.append(s, n);
}
void on_platform_write(void* ud, const char* s, int n, bool is_err) {
  (void)ud; (void)is_err;
  g_out.append(s, n);
}
bool on_input(void* ud, Str* out) {
  (void)ud;
  out->clear();
  if (g_in_pos >= g_in.size()) {   // 終端。none になる
    g_eof_at = -1;
    return false;
  }
  while (g_in_pos < g_in.size()) {
    char c = g_in[g_in_pos++];
    if (c == '\n') return true;
    if (c == '\r') continue;
    out->push(c);
  }
  return true;
}
// 読める行（または終端）が来ているか。来るまで input() は待つ。
// 端末で read が返るまで止まっているのと同じことを、刻んで動くまま行う
bool on_input_ready(void* ud) {
  (void)ud;
  if (g_in_pos < g_in.size()) return true;
  return g_eof_at >= 0 && g_in_pos >= g_eof_at;
}

// ------------------------------------------------------------ 持ち物
enum Mode { M_IDLE = 0, M_RUN = 1, M_TEST = 2, M_DONE = 3 };

Config g_cfg;
Engine* g_engine = 0;
Mode g_mode = M_IDLE;
int g_last_status = 0;

Vec<Str> g_mod_paths, g_mod_sources;      // import で使えるようにしておくもの
Vec<Str> g_src_names, g_src_texts;        // 診断を整形するときに元のソースが要る
Str g_answer;                             // 返す文字列の置き場

// テストの進み具合（frontend/main.cpp の cmd_test と同じ順で進める）
Vec<int> g_test_idx;
Vec<Str> g_test_names;
int  g_test_cur = -1;
int  g_test_phase = 0;    // 0=トップレベル 1=前処理 2=本体 3=後処理
int  g_test_passed = 0;
bool g_test_fail = false;
Str  g_test_fail_msg;
Str  g_test_json;

Str source_of(const Str& name) {
  for (int i = 0; i < g_src_names.size(); i++)
    if (g_src_names[i] == name) return g_src_texts[i];
  return Str();
}

// ------------------------------------------------------------ JSON にする
void json_str(Str* r, const Str& s) {
  r->push('"');
  for (int i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') { r->push('\\'); r->push((char)c); continue; }
    if (c == '\n') { *r += "\\n"; continue; }
    if (c == '\r') { *r += "\\r"; continue; }
    if (c == '\t') { *r += "\\t"; continue; }
    if (c < 0x20) {
      const char* hex = "0123456789abcdef";
      *r += "\\u00";
      r->push(hex[(c >> 4) & 15]);
      r->push(hex[c & 15]);
      continue;
    }
    r->push((char)c);
  }
  r->push('"');
}
void json_field(Str* r, const char* key, const Str& v) {
  *r += "\"";
  *r += key;
  *r += "\":";
  json_str(r, v);
}
void json_int(Str* r, const char* key, int v) {
  *r += "\"";
  *r += key;
  *r += "\":";
  *r += str_from_int(v);
}

Str diagnostics_json(const Vec<Diagnostic>& ds) {
  Str r("[");
  for (int i = 0; i < ds.size(); i++) {
    const Diagnostic& d = ds[i];
    if (i) r += ",";
    r += "{";
    json_field(&r, "severity", Str(d.severity == SEV_ERROR ? "error" : "warning"));
    r += ",";
    json_field(&r, "code", d.code);
    r += ",";
    json_field(&r, "message", d.message);
    r += ",";
    json_field(&r, "file", d.file);
    r += ",";
    json_int(&r, "line", d.spans.size() ? d.spans[0].line : 0);
    r += ",";
    json_int(&r, "col", d.spans.size() ? d.spans[0].col : 0);
    r += ",";
    json_int(&r, "len", d.spans.size() ? d.spans[0].len : 0);
    r += ",\"spans\":[";
    for (int k = 0; k < d.spans.size(); k++) {
      if (k) r += ",";
      r += "{";
      json_int(&r, "line", d.spans[k].line);
      r += ",";
      json_int(&r, "col", d.spans[k].col);
      r += ",";
      json_int(&r, "len", d.spans[k].len);
      r += ",";
      json_field(&r, "label", d.spans[k].label);
      r += "}";
    }
    r += "],\"help\":[";
    for (int k = 0; k < d.help.size(); k++) {
      if (k) r += ",";
      json_str(&r, d.help[k]);
    }
    r += "],";
    // 端末と同じ整形。そのまま出したいときのために添える
    json_field(&r, "text", format_diagnostic(d, source_of(d.file), false, g_cfg.lang));
    r += "}";
  }
  r += "]";
  return r;
}

// ------------------------------------------------------------ テストを進める
void test_record(bool ok) {
  if (g_test_json.size() > 1) g_test_json += ",";
  g_test_json += "{";
  json_field(&g_test_json, "name", g_test_names[g_test_cur]);
  g_test_json += ",\"ok\":";
  g_test_json += ok ? "true" : "false";
  g_test_json += ",";
  json_field(&g_test_json, "desc", test_desc());
  g_test_json += ",";
  json_field(&g_test_json, "message", g_test_fail_msg.size() ? g_test_fail_msg : test_message());
  g_test_json += "}";
}

void test_start(int i) {
  g_test_cur = i;
  g_test_fail = false;
  g_test_fail_msg.clear();
  test_begin();
  if (test_before_index() >= 0) {
    g_test_phase = 1;
    g_engine->run_only(test_before_index(), false);
    return;
  }
  g_test_phase = 2;
  g_engine->run_only(g_test_idx[i], false);
}

// もう走らせるものが無ければ false
bool test_next() {
  int i = g_test_cur + 1;
  if (i >= g_test_idx.size()) {
    g_mode = M_DONE;
    g_test_json += "]";
    return false;
  }
  test_start(i);
  return true;
}

void test_finish_one() {
  bool ok = !g_test_fail && !test_failed();
  if (ok) g_test_passed++;
  test_record(ok);
  g_out += ok ? "  ok    " : "  fail  ";
  g_out += g_test_names[g_test_cur];
  g_out += "\n";
  if (!ok) {
    if (test_desc().size()) { g_out += "        "; g_out += test_desc(); g_out += "\n"; }
    Str msg = g_test_fail_msg.size() ? g_test_fail_msg : test_message();
    if (msg.size()) { g_out += "        "; g_out += msg; g_out += "\n"; }
  }
  test_next();
}

}  // namespace

// ================================================================ 外に出す
#define API extern "C" EMSCRIPTEN_KEEPALIVE

// 最初に1度だけ。移植層を差し込む
API void shk_boot() {
  platform_set(platform_web());
  web_set_sink(on_platform_write, 0);
}

API const char* shk_version() { return "0.1.0"; }

// 次の shk_load() から使う設定
API void shk_config(int memory_mb, int lang_en, int strict) {
  g_cfg = Config();
  g_cfg.lang = lang_en ? LANG_EN : LANG_JA;
  g_cfg.strict = strict != 0;
  g_cfg.memory_limit = (size_t)(memory_mb > 0 ? memory_mb : 0) << 20;
}

// import で使えるようにしておくもの（shk_load の前に足す）
API void shk_add_module(const char* path, const char* source) {
  g_mod_paths.push(Str(path));
  g_mod_sources.push(Str(source));
}
API void shk_clear_modules() {
  g_mod_paths.clear();
  g_mod_sources.clear();
}

// 読み込む（字句解析・構文解析・型検査・バイトコード生成）。誤りの数を返す
API int shk_load(const char* name, const char* source) {
  if (g_engine) {   // 確保と解放は移植層に通す（core/support.h）
    g_engine->~Engine();
    sk_free(g_engine);
    g_engine = 0;
  }
  g_out.clear();
  g_in.clear();
  g_in_pos = 0;
  g_eof_at = -1;
  g_mode = M_IDLE;
  g_last_status = 0;
  g_src_names.clear();
  g_src_texts.clear();

  g_engine = new (sk_alloc(sizeof(Engine))) Engine(g_cfg);
  HostIO io;
  io.write_out = on_output;
  io.read_line = on_input;
  io.input_ready = on_input_ready;
  g_engine->set_io(io);

  Str n(name), s(source);
  g_src_names.push(n);
  g_src_texts.push(s);
  for (int i = 0; i < g_mod_paths.size(); i++) {
    Str display = g_mod_paths[i] + ".shk";
    g_engine->add_module(g_mod_paths[i], g_mod_sources[i], display);
    g_src_names.push(display);
    g_src_texts.push(g_mod_sources[i]);
  }

  const Vec<Diagnostic>& ds = g_engine->load(n, s);
  g_answer = diagnostics_json(ds);
  int errs = 0;
  for (int i = 0; i < ds.size(); i++) if (ds[i].severity == SEV_ERROR) errs++;
  return errs;
}

// 見た目を整える（core/fmt_src.cpp）。読めないソースは、もとのまま返る。
// 整えられたかどうかは shk_formatted() で分かる
Str g_fmt;
int g_fmt_ok = 0;

API const char* shk_format(const char* source) {
  bool ok = false;
  g_fmt = format_source(Str(source ? source : ""), &ok);
  g_fmt_ok = ok ? 1 : 0;
  return g_fmt.c_str();
}
API int shk_formatted() { return g_fmt_ok; }

// 直前の shk_load() が返した診断（JSON）
API const char* shk_diagnostics() { return g_answer.c_str(); }
API int shk_ok() { return g_engine && g_engine->ok() ? 1 : 0; }
API int shk_has_entry() { return g_engine && g_engine->has_entry() ? 1 : 0; }

// input() に返す文字列をためる（打たれた行をそのつど渡す）
API void shk_push_input(const char* text) {
  g_in += Str(text);
  if (g_in.size() && g_in[g_in.size() - 1] != '\n') g_in.push('\n');
  if (g_eof_at >= 0 && g_eof_at < g_in.size()) g_eof_at = -1;   // 続きが来たので終端は取り消し
}

// もう入力は無い、と伝える（端末の Ctrl-D）。次の input() が none になる
API void shk_push_eof() { g_eof_at = g_in.size(); }

// input() が行を待って止まっているか。JavaScript はこれを見て入力を促す
API int shk_waiting_input() { return g_engine && g_engine->waiting_input() ? 1 : 0; }

// 実行を始める。読み込みの時点で main は呼ぶ準備ができている
API int shk_start_run() {
  if (!g_engine || !g_engine->ok()) return 0;
  if (!g_engine->has_entry()) return 0;
  g_mode = M_RUN;
  g_last_status = 0;
  return 1;
}

// test_ で始まる関数を走らせ始める。見つかった件数を返す
API int shk_start_test() {
  if (!g_engine || !g_engine->ok()) return -1;
  g_test_idx.clear();
  g_test_names.clear();
  g_engine->find_tests(&g_test_idx, &g_test_names);
  g_test_cur = -1;
  g_test_phase = 0;
  g_test_passed = 0;
  g_test_fail = false;
  g_test_json = Str("[");
  test_reset_hooks();
  // トップレベルの文（test.before_each の登録など）を先に済ませる
  g_engine->run_only(g_engine->has_entry() ? g_engine->program()->entry : -1, true);
  g_mode = M_TEST;
  g_last_status = 0;
  return g_test_idx.size();
}

// budget 命令だけ進める。0=まだ続く 1=終わった 2=止まった
API int shk_pump(int budget) {
  if (!g_engine || g_mode == M_IDLE || g_mode == M_DONE) return g_last_status;
  RunStatus st = g_engine->step(budget);
  if (st == SK_Running) return (g_last_status = 0);

  if (g_mode == M_RUN) {
    g_mode = M_DONE;
    return (g_last_status = (st == SK_Finished ? 1 : 2));
  }

  // ここから先はテスト。1つの段が終わるたびに次の段へ進める
  if (g_test_phase == 0) {                      // トップレベル
    if (st == SK_Error) {
      g_mode = M_DONE;
      g_test_json += "]";
      return (g_last_status = 2);
    }
    if (!test_next()) return (g_last_status = 1);
    return (g_last_status = 0);
  }
  if (st == SK_Error) {                         // テストの中で止まった
    g_test_fail = true;
    g_test_fail_msg = g_engine->error_message();
  }
  if (g_test_phase == 1) {                      // 前処理 → 本体
    g_test_phase = 2;
    g_engine->run_only(g_test_idx[g_test_cur], false);
    return (g_last_status = 0);
  }
  if (g_test_phase == 2 && test_after_index() >= 0) {   // 本体 → 後処理
    g_test_phase = 3;
    g_engine->run_only(test_after_index(), false);
    return (g_last_status = 0);
  }
  test_finish_one();
  return (g_last_status = (g_mode == M_DONE ? 1 : 0));
}

// 走らせるのが終わったのに面が開いたままなら、ここで片づける。
// `shark` コマンドならプロセスごと消えて窓も消えるところ。ブラウザは頁が残るので、
// 誰も見ていない窓（閉じるボタンを押しても、受け取る側がもう居ない）が居座ってしまう
API void shk_ui_close() { ui_shutdown(); }

API int shk_idle() { return g_engine && g_engine->idle() ? 1 : 0; }
API void shk_abort() { if (g_engine) g_engine->abort_run(); }
API int shk_exit_code() { return g_engine ? g_engine->exit_code() : 0; }

// 止まった理由（JSON）
API const char* shk_error() {
  Str r("{");
  if (g_engine) {
    json_field(&r, "message", g_engine->error_message());
    r += ",";
    json_field(&r, "file", g_engine->error_file());
    r += ",";
    json_int(&r, "line", g_engine->error_line());
    r += ",";
    json_field(&r, "trace", g_engine->error_trace());
  }
  r += "}";
  g_answer = r;
  return g_answer.c_str();
}

// テストの結果（JSON）
API const char* shk_test_results() { return g_test_json.c_str(); }
API int shk_test_passed() { return g_test_passed; }
API int shk_test_total() { return g_test_idx.size(); }

// いま動いているプログラムが使っている量（バイト）
API double shk_memory_used() { return g_engine ? (double)g_engine->memory_used() : 0.0; }
// 読み込みで作ったもの（構文木・バイトコード・型の表）も含めた全体
API double shk_memory_total() { return g_engine ? (double)g_engine->memory_total() : 0.0; }
API double shk_memory_limit() { return g_engine ? (double)g_engine->memory_limit() : 0.0; }

// print と write が書いたもの。読んだら shk_out_clear() で空にする
API const char* shk_out_ptr() { return g_out.data(); }
API int shk_out_len() { return g_out.size(); }
API void shk_out_clear() { g_out.clear(); }

// この処理系が持つモジュールの一覧（JSON）
API const char* shk_modules() {
  Str r("[");
  if (g_engine) {
    const Vec<Str>& m = g_engine->module_list();
    for (int i = 0; i < m.size(); i++) {
      if (i) r += ",";
      json_str(&r, m[i]);
    }
  }
  r += "]";
  g_answer = r;
  return g_answer.c_str();
}

// エラー番号の詳しい説明（無ければ空）
API const char* shk_explain(const char* code) {
  const char* text = diag_explain(code, g_cfg.lang);
  return text ? text : "";
}
