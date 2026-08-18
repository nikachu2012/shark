// runtime.h — バイトコードだけを動かす実行装置（spec/runtime/bytecode.md）
//
//   1. 覚え書きを読む         bytecode_read_header() → 入れるモジュールが分かる
//   2. 実行装置を作る          Runtime(config)
//   3. バイトコードを読む       load(bytes)        → つまずいたら理由が返る
//   4. 少しずつ動かす          step(budget)       → 状態
//
// 字句解析・構文解析・型検査・コード生成は持たない。Engine（shark.h）から
// 実行に要るところだけを抜き出したもので、入口と出口は Engine と同じ形にしてある。
// コアはここでもファイルを開かない。バイト列は呼ぶ側が渡す。
#ifndef SHARK_RUNTIME_H
#define SHARK_RUNTIME_H

#include "bytecode.h"
#include "config.h"
#include "diag.h"
#include "program.h"
#include "registry.h"
#include "types.h"
#include "vm.h"

namespace shark {

class Runtime {
 public:
  explicit Runtime(const Config& cfg);
  ~Runtime();

  TypeTable& types() { return types_; }
  Registry& registry() { return reg_; }

  // ホストの関数を足す。バイトコードを作ったときと**同じ順**で足すこと。
  // 食い違っていれば load() が指紋の違いとして知らせる
  int register_host(const char* name, NativeFn fn, Type* ret,
                    Type* p0 = 0, Type* p1 = 0, Type* p2 = 0, Type* p3 = 0);

  // バイトコードを読み、動かす用意までする
  bool load(const Str& bytecode, Str* err);
  bool ok() const { return ok_; }
  bool has_entry() const { return prog_ && prog_->entry >= 0; }

  // 少しずつ動かす（Engine と同じ）
  RunStatus step(int budget);
  bool idle() const { return vm_.idle_hint; }
  bool waiting_input() const { return vm_.input_wait; }
  void abort_run() { vm_.abort_run(); }
  int exit_code() const { return vm_.exit_code; }
  const Str& error_message() const { return vm_.error_message; }
  const Str& error_trace() const { return vm_.error_trace; }
  int error_line() const { return vm_.error_line; }
  const Str& error_file() const { return vm_.error_file; }
  size_t memory_used() const { return sk_mem_run_used(); }
  size_t memory_limit() const { return sk_mem_limit(); }
  size_t memory_total() const { return sk_mem_used(); }

  void set_io(const HostIO& io) { vm_.io = io; }

  Program* program() { return prog_; }
  const Vec<Str>& module_list() const { return reg_.modules(); }
  VM& vm() { return vm_; }

 private:
  Config cfg_;
  TypeTable types_;
  Registry reg_;
  DiagBag diag_;
  Program* prog_;
  VM vm_;
  bool ok_;
};

}  // namespace shark
#endif
