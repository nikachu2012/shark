// shark_web.cpp — WebAssembly / ブラウザ環境向けホスト実装（spec/runtime/embedding.md）
//
// コア処理系の外部実装。frontend/main.cpp が CLI 向けに行う処理（初期化、ループ進行、I/O 転送）を
// Web ブラウザ向けに提供する。コアライブラリはこのファイルに依存しない。
//
//   1. スクリプトのロード      shk_load()
//   2. 命令ステップの進行      shk_pump()   ← requestAnimationFrame ごとに呼び出し
//   3. 出力の取得              shk_out_ptr() / shk_out_len()
//
// 協調的マルチタスク機構により、無限ループを含むスクリプトでもブラウザメインスレッドをブロックしない。
// 1 回の pump で実行する VM 命令数を JavaScript 側からバジェットとして指定可能。
//
// 文字列ポインタを返す関数は、次回同一関数呼び出しまでの間のみバッファの生存期間が保証される。

#include <stdio.h>
#include <string.h>
#include <emscripten/emscripten.h>

#include "../core/platform/web.h"
#include "../core/registry.h"   // ui_shutdown（core/lib/ui.cpp）
#include "../core/fmt_src.h"
#include "../core/shark.h"

using namespace shark;

namespace {

// ------------------------------------------------------------ 入出力バッファ
Str g_out;          // print / write の出力バッファ。JavaScript 側が取得後にクリア
Str g_in;           // input() 用の入力バッファ。JavaScript 側から随時追記
int g_in_pos = 0;
int g_eof_at = -1;  // EOF 位置（端末の Ctrl-D 相当）。-1 は EOF 未設定

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
  if (g_in_pos >= g_in.size()) {   // 終端到達。nil を返却
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
// 読み取り可能な行（または EOF）が存在するか判定。データが到達するまで input() はブロック（待機）
// CLI で read() がブロックするのと同様の挙動を、ノンブロッキングな pump ループ上で実現
bool on_input_ready(void* ud) {
  (void)ud;
  if (g_in_pos < g_in.size()) return true;
  return g_eof_at >= 0 && g_in_pos >= g_eof_at;
}

// ------------------------------------------------------------ 状態管理
Engine* g_engine = 0;
Config  g_cfg;
int     g_last_status = 0;   // 0=running 1=done 2=error

enum Mode { M_IDLE = 0, M_RUN = 1, M_TEST = 2, M_DONE = 3 };
Mode g_mode = M_IDLE;

Vec<Str> g_mod_paths, g_mod_sources;      // 事前ロード済み仮想モジュール（import 解決用）
Vec<Str> g_src_names, g_src_texts;        // エラー診断メッセージ整形用の元ソースコード
Str g_answer;                             // 文字列戻り値用バッファ

// テスト実行状態（frontend/main.cpp の cmd_test と同様のフェーズ遷移）
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
    // CLI と同様の整形済み診断メッセージ
    json_field(&r, "text", format_diagnostic(d, source_of(d.file), false, g_cfg.lang));
    r += "}";
  }
  r += "]";
  return r;
}

// ------------------------------------------------------------ テスト実行制御
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

// 実行対象のテスト関数が残っていない場合は false
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

// ================================================================ エクスポート C API
#define API extern "C" EMSCRIPTEN_KEEPALIVE

// 初期化（初回 1 回のみ）。Web プラットフォーム抽象化レイヤーを登録
API void shk_boot() {
  platform_set(platform_web());
  web_set_sink(on_platform_write, 0);
}

API const char* shk_version() { return "0.1.0"; }

// 次回 shk_load() で使用するランタイム設定
API void shk_config(int memory_mb, int lang_en, int strict) {
  g_cfg = Config();
  g_cfg.lang = lang_en ? LANG_EN : LANG_JA;
  g_cfg.strict = strict != 0;
  g_cfg.memory_limit = (size_t)(memory_mb > 0 ? memory_mb : 0) << 20;
}

// 仮想モジュールを登録（shk_load 前に呼び出し）
API void shk_add_module(const char* path, const char* source) {
  g_mod_paths.push(Str(path));
  g_mod_sources.push(Str(source));
}
API void shk_clear_modules() {
  g_mod_paths.clear();
  g_mod_sources.clear();
}

// スクリプトの読み込み（字句解析・構文解析・型検査・バイトコード生成）。エラー数を返す
API int shk_load(const char* name, const char* source) {
  if (g_engine) {   // メモリ確保・解放はプラットフォーム抽象化レイヤー経由（core/support.h）
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

// ソースコード整形（フォーマッター core/fmt_src.cpp）。構文エラーがある場合は元の文字列を返す。
// 整形が成功したかどうかは shk_formatted() で判定
Str g_fmt;
int g_fmt_ok = 0;

API const char* shk_format(const char* source) {
  bool ok = false;
  g_fmt = format_source(Str(source ? source : ""), &ok);
  g_fmt_ok = ok ? 1 : 0;
  return g_fmt.c_str();
}
API int shk_formatted() { return g_fmt_ok; }

// 直前の shk_load() で生成された診断メッセージ（JSON 形式）
API const char* shk_diagnostics() { return g_answer.c_str(); }
API int shk_ok() { return g_engine && g_engine->ok() ? 1 : 0; }
API int shk_has_entry() { return g_engine && g_engine->has_entry() ? 1 : 0; }

// input() 用の入力バッファにテキストを追加（入力された行を都度渡す）
API void shk_push_input(const char* text) {
  g_in += Str(text);
  if (g_in.size() && g_in[g_in.size() - 1] != '\n') g_in.push('\n');
  if (g_eof_at >= 0 && g_eof_at < g_in.size()) g_eof_at = -1;   // 入力が追記されたため EOF を解除
}

// 入力ストリームの終端（EOF）を通知（Ctrl-D 相当）。次回 input() が nil を返す
API void shk_push_eof() { g_eof_at = g_in.size(); }

// input() が入力待機中（ブロック状態）か判定。JavaScript 側で入力プロンプトを表示する判定に使用
API int shk_waiting_input() { return g_engine && g_engine->waiting_input() ? 1 : 0; }

// 通常実行を開始。shk_load() 完了時点で main 関数呼び出しの準備が完了している
API int shk_start_run() {
  if (!g_engine || !g_engine->ok()) return 0;
  if (!g_engine->has_entry()) return 0;
  g_mode = M_RUN;
  g_last_status = 0;
  return 1;
}

// test_ で始まるテスト関数の実行を開始。検出されたテスト数を返す
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
  // モジュール最上位文（test.before_each の登録等）を先行実行
  g_engine->run_only(g_engine->has_entry() ? g_engine->program()->entry : -1, true);
  g_mode = M_TEST;
  g_last_status = 0;
  return g_test_idx.size();
}

// 指定されたバジェット（命令ステップ数）だけ実行を進める。戻り値: 0=実行継続中 1=正常終了 2=ランタイムパニック停止
API int shk_pump(int budget) {
  if (!g_engine || g_mode == M_IDLE || g_mode == M_DONE) return g_last_status;
  RunStatus st = g_engine->step(budget);
  if (st == SK_Running) return (g_last_status = 0);

  if (g_mode == M_RUN) {
    g_mode = M_DONE;
    return (g_last_status = (st == SK_Finished ? 1 : 2));
  }

  // テスト実行中: フェーズ完了ごとに次のフェーズへ遷移
  if (g_test_phase == 0) {                      // トップレベル
    if (st == SK_Error) {
      g_test_fail = true;
      g_test_fail_msg = g_engine->error_message();
      test_record(false);
      g_test_json += "]";
      g_mode = M_DONE;
      return (g_last_status = 2);
    }
    if (!test_next()) return (g_last_status = 1);
    return (g_last_status = 0);
  }
  if (st == SK_Error) {                         // テストケース内でパニック/エラー発生
    g_test_fail = true;
    g_test_fail_msg = g_engine->error_message();
  }
  if (g_test_phase == 1) {                      // セットアップ（before_each） → テスト本体
    g_test_phase = 2;
    g_engine->run_only(g_test_idx[g_test_cur], false);
    return (g_last_status = 0);
  }
  if (g_test_phase == 2 && test_after_index() >= 0) {   // テスト本体 → ティアダウン（after_each）
    g_test_phase = 3;
    g_engine->run_only(test_after_index(), false);
    return (g_last_status = 0);
  }
  test_finish_one();
  return (g_last_status = (g_mode == M_DONE ? 1 : 0));
}

// 実行終了時に UI ウィンドウが開いたままの場合はクリーンアップを行う。
// CLI ではプロセス終了時に OS 側で破棄されるが、ブラウザ環境ではページが存続するため、
// 未解放のウィンドウやリソースが残り続けるのを防ぐ。
API void shk_ui_close() { ui_shutdown(); }

API int shk_idle() { return g_engine && g_engine->idle() ? 1 : 0; }
API void shk_abort() { if (g_engine) g_engine->abort_run(); }
API int shk_exit_code() { return g_engine ? g_engine->exit_code() : 0; }

// ランタイムエラー詳細（JSON 形式）
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

// テスト実行結果一覧（JSON 形式）
API const char* shk_test_results() { return g_test_json.c_str(); }
API int shk_test_passed() { return g_test_passed; }
API int shk_test_total() { return g_test_idx.size(); }

// 実行中プログラムの動的ヒープ使用量（バイト）
API double shk_memory_used() { return g_engine ? (double)g_engine->memory_used() : 0.0; }
// スクリプトロード時の静的データ（AST・バイトコード・型定義テーブル）を含む総メモリ使用量
API double shk_memory_total() { return g_engine ? (double)g_engine->memory_total() : 0.0; }
API double shk_memory_limit() { return g_engine ? (double)g_engine->memory_limit() : 0.0; }

// print / write による出力データ。取得後に shk_out_clear() でクリア
API const char* shk_out_ptr() { return g_out.data(); }
API int shk_out_len() { return g_out.size(); }
API void shk_out_clear() { g_out.clear(); }

// 利用可能な組み込みモジュール一覧（JSON 配列）
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

// エラーコード（診断コード）の詳細解説テキストを取得（未定義の場合は空文字列）
API const char* shk_explain(const char* code) {
  const char* text = diag_explain(code, g_cfg.lang);
  return text ? text : "";
}
