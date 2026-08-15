// check.h — 型検査（spec/types/）
#ifndef SHARK_CHECK_H
#define SHARK_CHECK_H

#include "ast.h"
#include "diag.h"
#include "program.h"
#include "registry.h"

namespace shark {

struct Local {
  Str name;
  Type* type;
  int slot;
  bool is_const;
  bool is_ref;
  bool used;
  int line, col, len;
  Local() : type(0), slot(0), is_const(false), is_ref(false), used(false), line(0), col(0), len(0) {}
};

struct FuncCtx {
  FuncInfo* fi;
  Vec<Local> locals;
  Vec<int> scopes;
  int next_slot;
  int max_slot;
  ClassInfo* cls;
  Type* ret;
  int loop_depth;
  bool in_task;
  Vec<Type*> gtypes;   // 型引数（T_Generic）
  Vec<Str> gnames;
  FuncCtx() : fi(0), next_slot(0), max_slot(0), cls(0), ret(0), loop_depth(0), in_task(false) {}
};

class Checker {
 public:
  Checker(Program& prog, Registry& reg, TypeTable& types, DiagBag& diag, Arena& arena);

  // 1) すべてのモジュールの宣言を集める
  void collect(Unit* u);
  // 2) 本体を検査する。誤りが無ければ true
  bool check_all();

  ClassInfo* class_error() { return c_error_; }
  ClassInfo* class_comparable() { return c_comparable_; }

 private:
  // --- 宣言の収集 ---
  void make_builtin_classes();
  void collect_class_shells(Unit* u);
  void collect_class_bodies(Unit* u);
  void collect_funcs(Unit* u);
  void collect_globals(Unit* u);
  void layout_class(ClassInfo* c);
  int  vslot_for(FuncInfo* f);

  // --- 型 ---
  Type* resolve_type(TypeExpr* te, Unit* u, FuncCtx* fc, ClassInfo* cls);
  ClassInfo* find_class(const Str& name, Unit* u);
  Type* subst(Type* t, const Vec<Str>& names, const Vec<Type*>& args);
  bool satisfies(Type* t, ClassInfo* iface);

  // --- 文 ---
  void check_func_body(FuncInfo* fi, FuncDecl* fd, Unit* u, ClassInfo* cls);
  void check_stmt(Node* s);
  void check_block(Node* b);
  void check_var_decl(Node* s);
  void check_assign(Node* s);
  void check_if(Node* s);
  void check_while(Node* s);
  void check_for(Node* s);
  void check_return(Node* s);

  // --- 式 ---
  Type* check_expr(Node* e);
  Type* check_call(Node* e);
  Type* check_field(Node* e);
  Type* check_index(Node* e);
  Type* check_binary(Node* e);
  Type* check_unary(Node* e);
  Type* check_fstr(Node* e);
  Type* check_list_lit(Node* e);
  Type* check_map_lit(Node* e);
  Type* check_ident(Node* e);
  Type* check_task(Node* e);
  Type* check_parallel(Node* e);
  Type* check_try(Node* e);

  bool resolve_builtin_method(Node* call, Type* recv, const Str& name, Vec<Node*>& args);
  bool resolve_class_method(Node* call, Type* recv, const Str& name, Vec<Node*>& args, bool via_super);
  bool resolve_overload(Node* call, const Vec<int>& cand_funcs, const Vec<int>& cand_natives,
                        Vec<Node*>& args, const Str& shown_name,
                        const Vec<Str>* cls_gparams = 0, const Vec<Type*>* cls_targs = 0);
  bool resolve_convert(Node* call, const Str& name, Vec<Node*>& args);
  bool resolve_ctor(Node* call, ClassInfo* c, Vec<Node*>& args, const Vec<Type*>& targs);

  // --- 変数 ---
  Local* find_local(const Str& name);
  int declare_local(const Str& name, Type* t, bool is_const, Node* at);
  void push_scope();
  void pop_scope();
  GlobalInfo* find_global(const Str& name, Unit* u);
  Str module_of_alias(const Str& alias, Unit* u, bool* found);

  // --- 診断の道具 ---
  void err(const char* code, const Str& msg, Node* at, const Str& label = Str());
  void err_at(const char* code, const Str& msg, int line, int col, int len);
  void warn(const char* code, const Str& msg, Node* at);
  void need_bool(Node* cond, const char* what);
  bool need_assign(Type* to, Type* from, Node* at, const char* what_ja, const char* what_en);
  void note_optional(Diagnostic& d, const Str& expr_text);

  Program& prog_;
  Registry& reg_;
  TypeTable& t_;
  DiagBag& diag_;
  Arena& arena_;

  Vec<Unit*> units_;
  Unit* unit_;         // いま検査しているモジュール
  FuncCtx* fc_;
  Vec<Str> vkeys_;     // virtual の表の位置を決めるための鍵
  ClassInfo* c_error_;
  ClassInfo* c_comparable_;
  Type* t_none_;       // none リテラルの型（どの T? にも入る）
};

}  // namespace shark
#endif
