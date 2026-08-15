// codegen.h — バイトコード生成（spec/runtime/execution.md）
#ifndef SHARK_CODEGEN_H
#define SHARK_CODEGEN_H

#include "ast.h"
#include "opcodes.h"
#include "program.h"
#include "registry.h"

namespace shark {

class CodeGen {
 public:
  CodeGen(Program& prog, TypeTable& types, Registry& reg) : prog_(prog), reg_(reg), f_(0), line_(1) { (void)types; }
  void run();

 private:
  void gen_func(FuncInfo* f);
  void gen_stmt(Node* s);
  void gen_expr(Node* e);
  void gen_place(Node* e);
  void gen_default(Type* t);
  void gen_call(Node* e);
  void gen_ref(Node* e);

  int  emit(uint8_t op);
  void emit_i32(int v);
  int  emit_jump(uint8_t op);
  void patch(int at);
  void patch_to(int at, int target);
  int  here() const { return f_->code.size(); }
  int  add_const(Value v);   // 渡された値の持ち主になる（重複していれば捨てる）

  Program& prog_;
  Registry& reg_;
  FuncInfo* f_;
  int line_;
  Vec<int> breaks_;
  Vec<int> continues_;
  Vec<int> break_marks_;
  Vec<int> continue_marks_;
};

}  // namespace shark
#endif
