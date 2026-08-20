// ast.h — 構文木（spec/syntax.md の「文法の概略」に対応する）
#ifndef SHARK_AST_H
#define SHARK_AST_H

#include "support.h"
#include "types.h"

namespace shark {

struct FuncInfo;
struct ClassInfo;
struct FuncDecl;

// まとめて確保し、まとめて捨てる
//
// 構文木の節は Vec や Str を持つので、捨てるときに後始末が要る。
// make<T>() で作ったものは、Arena が壊れるときにまとめて後始末する。
class Arena {
 public:
  Arena() {}
  ~Arena() {
    for (int i = dtors_.size() - 1; i >= 0; i--) dtors_[i].fn(dtors_[i].obj);
    for (int i = 0; i < blocks_.size(); i++) sk_free(blocks_[i]);
  }

  template <class T>
  T* make() {
    T* p = new (alloc(sizeof(T))) T();
    Dtor d;
    d.obj = p;
    d.fn = &destroy_one<T>;
    dtors_.push(d);
    return p;
  }

  void* alloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!cur_ || used_ + n > cap_) {
      size_t size = n > 32768 ? n : 32768;
      cur_ = (char*)sk_alloc(size);
      blocks_.push(cur_);
      cap_ = size;
      used_ = 0;
    }
    char* p = cur_ + used_;
    used_ += n;
    return p;
  }
 private:
  template <class T>
  static void destroy_one(void* p) { ((T*)p)->~T(); }
  struct Dtor { void* obj; void (*fn)(void*); };

  Vec<Dtor> dtors_;
  Vec<char*> blocks_;
  char* cur_ = 0;
  size_t used_ = 0, cap_ = 0;
};

enum NodeKind : uint8_t {
  // 式
  E_Int, E_Float, E_Bool, E_Str, E_Bytes, E_None, E_FStr, E_ListLit, E_MapLit,
  E_Ident, E_This, E_Super, E_Field, E_Index, E_Call, E_Unary, E_Binary,
  E_Force, E_Task, E_Parallel, E_Try, E_Ref, E_TypeName, E_Lambda,
  // 文
  S_VarDecl, S_Assign, S_Expr, S_If, S_While, S_For, S_Break, S_Continue,
  S_Return, S_Block, S_Panic,
};

// 型の書き方（`list<int>` `Fish?` `func(int) -> bool`）
struct TypeExpr {
  Str name;                 // "int" "list" "Fish" "func"
  Vec<TypeExpr*> args;      // 型引数
  bool optional;            // 末尾の ?
  Vec<TypeExpr*> fn_params; // func のとき
  TypeExpr* fn_ret;
  bool bad;                 // 型名として読めなかった。誤りは構文解析でもう出ている
  int line, col, len;
  TypeExpr() : optional(false), fn_ret(0), bad(false), line(0), col(0), len(0) {}
};

struct Node;

struct FStrPart {
  bool is_expr;
  Str text;      // 文字のとき
  Node* expr;    // 式のとき
  Str spec;      // 書式（`:` の後ろ）
  FStrPart() : is_expr(false), expr(0) {}
};

struct MapPair { Node* key; Node* val; };

struct Node {
  NodeKind kind;
  int line, col, len;

  // 共通の置き場
  Str name;              // 識別子、メンバ名、演算子
  int64_t ival;
  double dval;
  Node* a;               // 左 / 対象 / 条件
  Node* b;               // 右 / 添字 / 本体
  Node* c;               // それ以外（else など）
  Vec<Node*> list;       // 引数、要素、文の並び
  Vec<MapPair> pairs;
  Vec<FStrPart> parts;
  Vec<TypeExpr*> targs;  // 明示した型引数
  TypeExpr* tann;        // 型注釈
  FuncDecl* fdecl;       // E_Lambda（その場に書いた関数）の中身
  Str bind;              // if var / while var / for var の変数名
  Str bind2;             // else var e
  bool is_const;
  bool optional_chain;   // ?.

  // --- 型検査が書き込むところ ---
  Type* type;
  Type* bind_type;
  Type* bind2_type;
  int slot;        // 局所変数の位置
  int slot2;       // 2つ目（else var など）
  int resolved;    // 関数・フィールド・ネイティブの番号
  int resolved2;   // 補助（vslot など）
  int opcode;      // 演算の種類
  ClassInfo* rcls;
  FuncInfo* rfunc;
  bool is_global;
  bool is_ref_param;
  bool checked;          // 検査を済ませてある（トップレベルの初期化を文の間に混ぜたとき）

  Node()
      : kind(E_Int), line(0), col(0), len(0), ival(0), dval(0), a(0), b(0), c(0), tann(0),
        fdecl(0), is_const(false), optional_chain(false), type(0), bind_type(0), bind2_type(0),
        slot(-1), slot2(-1), resolved(-1), resolved2(-1), opcode(0), rcls(0), rfunc(0),
        is_global(false), is_ref_param(false), checked(false) {}
};

struct ParamDecl {
  Str name;
  TypeExpr* type;
  bool is_ref;
  int line, col, len;
  ParamDecl() : type(0), is_ref(false), line(0), col(0), len(0) {}
};

struct GenericParam {
  Str name;
  Vec<Str> constraints;
};

struct FuncDecl {
  Str name;
  Vec<GenericParam> gparams;
  Vec<ParamDecl> params;
  TypeExpr* ret;
  Node* body;          // S_Block（純粋仮想なら 0）
  bool is_public, is_virtual, is_override;
  int line, col, len;
  FuncInfo* info;
  FuncDecl() : ret(0), body(0), is_public(false), is_virtual(false), is_override(false),
               line(0), col(0), len(0), info(0) {}
};

struct FieldDecl {
  Str name;
  TypeExpr* type;
  bool is_public;
  int line, col, len;
  FieldDecl() : type(0), is_public(false), line(0), col(0), len(0) {}
};

struct ClassDecl {
  Str name;
  Vec<GenericParam> gparams;
  Vec<Str> bases;
  Vec<int> base_lines, base_cols, base_lens;
  Vec<FieldDecl> fields;
  Vec<FuncDecl*> methods;
  bool is_public;
  int line, col, len;
  ClassInfo* info;
  ClassDecl() : is_public(false), line(0), col(0), len(0), info(0) {}
};

struct ImportDecl {
  Str path;      // "std.time" "./util"
  Str alias;     // 別名（無ければ空）
  int line, col, len;
  ImportDecl() : line(0), col(0), len(0) {}
};

struct GlobalDecl {
  Str name;
  TypeExpr* type;
  Node* init;
  bool is_public, is_const;
  int line, col, len;
  int index;
  GlobalDecl() : type(0), init(0), is_public(false), is_const(false), line(0), col(0), len(0), index(-1) {}
};

struct Unit {
  Str module;          // モジュール名（"main" "util" "std.math"）
  Str display;         // 診断に出す名前（load に渡された名前）
  Vec<ImportDecl> imports;
  Vec<FuncDecl*> funcs;
  Vec<ClassDecl*> classes;
  Vec<GlobalDecl*> globals;
  Vec<Node*> top_stmts;   // トップレベルに並べた文（実行するファイルだけ）
  bool has_main;
  bool is_entry;
  Unit() : has_main(false), is_entry(false) {}
};

}  // namespace shark
#endif
