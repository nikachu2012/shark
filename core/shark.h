// shark.h — 実行系（コア）の入口（spec/runtime/embedding.md）
//
//   1. 処理系を作る            Engine(config)
//   2. ホストの関数を登録する    register_host(...)
//   3. ソースを読み込む          load(name, source) → 診断
//   4. 少しずつ動かす            step(budget)       → 状態
//   5. 片付ける                 デストラクタ
//
// コアはファイルを開かず、標準出力にも書かない。すべて呼ぶ側が渡す。
#ifndef SHARK_H
#define SHARK_H

#include "config.h"
#include "diag.h"
#include "program.h"
#include "registry.h"
#include "types.h"
#include "vm.h"

namespace shark {

class Arena;
struct Unit;
class Checker;

// import を解決するとき、ホストにソースを尋ねる
typedef bool (*ModuleLoader)(void* ud, const Str& path, Str* out_source, Str* out_display);

class Engine {
 public:
  explicit Engine(const Config& cfg);
  ~Engine();

  TypeTable& types() { return types_; }
  Registry& registry() { return reg_; }

  // ホストの関数を足す（ゲーム側の操作を言語から呼べるようにする）
  int register_host(const char* name, NativeFn fn, Type* ret,
                    Type* p0 = 0, Type* p1 = 0, Type* p2 = 0, Type* p3 = 0);

  // import で使うモジュールを先に登録しておく
  void add_module(const Str& path, const Str& source, const Str& display);
  void set_module_loader(ModuleLoader fn, void* ud) { loader_ = fn; loader_ud_ = ud; }

  // ソースを読み込む（字句解析・構文解析・型検査・バイトコード生成）
  const Vec<Diagnostic>& load(const Str& name, const Str& source);
  bool ok() const { return ok_; }
  bool has_entry() const { return prog_ && prog_->entry >= 0; }

  // 少しずつ動かす
  RunStatus step(int budget);
  bool idle() const { return vm_.idle_hint; }
  // input() がホストからの行を待っている（HostIO::input_ready を使うホストだけ）
  bool waiting_input() const { return vm_.input_wait; }
  void abort_run() { vm_.abort_run(); }
  int exit_code() const { return vm_.exit_code; }
  const Str& error_message() const { return vm_.error_message; }
  const Str& error_trace() const { return vm_.error_trace; }
  int error_line() const { return vm_.error_line; }
  const Str& error_file() const { return vm_.error_file; }
  // 実行中のプログラムが使っているメモリの量（バイト）。上限はこれに掛かる
  size_t memory_used() const { return sk_mem_run_used(); }
  size_t memory_limit() const { return sk_mem_limit(); }
  // 読み込みで作ったもの（構文木・バイトコード・型の表）も含めた全体
  size_t memory_total() const { return sk_mem_used(); }

  // 入出力はホストが受け取る
  void set_io(const HostIO& io) { vm_.io = io; }

  // REPL（spec/frontend.md）。入力を1回分ずつ足していく。
  // 前の入力で作った変数・関数・クラスは残り、前の文はもう一度は走らない。
  // 誤りがあれば、その入力は無かったことになる（ok() が false）。
  // 通ったら step() で終わるまで動かす。走らせるものが無ければ、すぐ SK_Finished になる。
  // load() を呼ぶと、それまでの入力は捨てられる
  const Vec<Diagnostic>& repl_eval(const Str& name, const Str& source);
  // os.exit() で終わった（REPL を閉じる合図）
  bool exit_requested() const { return vm_.exit_requested; }

  // テスト（spec/library/test.md）。1件ずつ順に走らせるのは呼ぶ側
  void find_tests(Vec<int>* out, Vec<Str>* names);
  // 関数を1つだけ走らせる。with_inits が false なら、いまのグローバルをそのまま使う
  void run_only(int func_index, bool with_inits = true);

  Program* program() { return prog_; }
  const Vec<Str>& module_list() const { return reg_.modules(); }
  VM& vm() { return vm_; }

 private:
  Unit* load_unit(const Str& path, const Str& source, const Str& display, bool is_entry, int depth,
                  int line = 1, int col = 1, int len = 1);
  bool find_module_source(const Str& path, Str* src, Str* display);
  void begin_program();   // 前のプログラムを捨て、前奏だけを読んだ状態にする
  void repl_begin();

  // REPL で名前を付け直した前の定義（入力が通らなかったとき戻す）
  struct Renamed {
    Str* name;
    Str old;
  };

  Config cfg_;
  TypeTable types_;
  Registry reg_;
  DiagBag diag_;
  Arena* arena_;
  Program* prog_;
  Checker* checker_;
  VM vm_;
  bool ok_;
  ModuleLoader loader_;
  void* loader_ud_;
  Vec<Str> mod_paths_, mod_sources_, mod_displays_;
  Vec<Str> loading_;   // 循環 import の検出
  Vec<Str> loaded_;
  Vec<Unit*> units_;
  bool repl_;         // REPL の途中（プログラムが入力のたびに育つ）
  int repl_count_;    // これまでに受け取った入力の数
  int repl_inits_;    // 走らせ終えた初期化の数（Program::inits の位置）
};

// 診断を1件、人が読む形に整える（フロントエンドの参考。コアは使わない）
Str format_diagnostic(const Diagnostic& d, const Str& source, bool color, Lang lang = LANG_JA);

}  // namespace shark
#endif
