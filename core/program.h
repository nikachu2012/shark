// program.h — 型検査が作り、仮想マシンが動かす「プログラム」の形
#ifndef SHARK_PROGRAM_H
#define SHARK_PROGRAM_H

#include "support.h"
#include "types.h"
#include "value.h"

namespace shark {

struct VM;
struct FuncDecl;
struct ClassDecl;

// ネイティブ関数（標準ライブラリとホスト関数）の戻り
enum NativeStatus {
  N_Ok = 0,    // 終わった。out に戻り値
  N_Panic,     // 止める。vm に理由を入れてある
  N_Wait,      // まだ終わらない。次の刻みで同じ引数のまま呼び直す
  N_Cancel,    // 取り消しが立っていたので、このタスクを終わらせる
};
typedef NativeStatus (*NativeFn)(VM& vm, Value* args, int nargs, Value& out);

struct ParamInfo {
  Str name;
  Type* type;
  bool is_ref;
  ParamInfo() : type(0), is_ref(false) {}
};

struct ClassInfo;

struct FuncInfo {
  Str name;
  Str module;
  Str file;      // 診断に出す名前（load に渡された名前）
  Vec<ParamInfo> params;
  Type* ret;

  bool is_public;
  bool is_method;
  bool is_init;
  bool is_native;
  bool is_virtual;
  bool is_override;
  bool is_pure;      // 本体のない virtual（純粋仮想）
  bool is_test;
  ClassInfo* owner;

  Vec<Str> gparams;             // 型引数の名前
  Vec<Type*> gtypes;            // 対応する T_Generic
  bool is_generic;

  int index;      // Program::funcs の位置
  int vslot;      // virtual のときの表の位置。それ以外は -1
  int nlocals;

  Vec<uint8_t> code;
  Vec<Value> consts;
  Vec<uint32_t> lines;   // 命令位置 -> 行

  NativeFn native;
  int native_data;

  FuncDecl* decl;

  FuncInfo()
      : ret(0), is_public(false), is_method(false), is_init(false), is_native(false),
        is_virtual(false), is_override(false), is_pure(false), is_test(false), owner(0),
        is_generic(false), index(-1), vslot(-1), nlocals(0), native(0), native_data(0), decl(0) {}
  ~FuncInfo() { for (int i = 0; i < consts.size(); i++) val_release(consts[i]); }
};

struct FieldInfo {
  Str name;
  Type* type;
  bool is_public;
  ClassInfo* owner;
  FieldInfo() : type(0), is_public(false), owner(0) {}
};

struct MethodRef {
  Str name;
  int func;       // Program::funcs の位置
  bool is_public;
  MethodRef() : func(-1), is_public(false) {}
};

struct ClassInfo {
  Str name;
  Str module;
  ClassInfo* base;                 // 実装を持つ親（1つまで）
  Vec<ClassInfo*> interfaces;      // 2番目以降
  Vec<FieldInfo> fields;           // 親のぶんを先頭に含めた通し番号
  Vec<MethodRef> methods;          // 自分で定義したもの（オーバーロード含む）
  Vec<int> vtable;                 // 位置 -> Program::funcs の位置（-1 は純粋仮想）
  bool is_public;
  bool is_abstract;
  bool is_interface;
  bool is_builtin;                 // Error など処理系が持つクラス
  int layout_state;                // 0=未 1=作業中 2=済
  Vec<Str> gparams;
  Vec<Type*> gtypes;               // 型引数（制約を持つ）
  ClassDecl* decl;
  ClassInfo()
      : base(0), is_public(false), is_abstract(false), is_interface(false), is_builtin(false),
        layout_state(0), decl(0) {}
};

struct GlobalInfo {
  Str name;
  Str module;
  Type* type;
  bool is_public;
  bool is_const;
  int index;
  GlobalInfo() : type(0), is_public(false), is_const(false), index(-1) {}
};

struct ModuleInfo {
  Str name;        // "std.math" や "./util"
  Str short_name;  // "math" "util"
  bool is_std;
  bool loaded;
  ModuleInfo() : is_std(false), loaded(false) {}
};

struct Program {
  Vec<FuncInfo*> funcs;
  Vec<ClassInfo*> classes;
  Vec<GlobalInfo*> globals;
  Vec<int> inits;     // モジュールごとのトップレベル初期化（依存の深い順）
  int entry;          // main の位置。無ければトップレベル文をまとめたもの
  TypeTable* types;
  Program() : entry(-1), types(0) {}
  ~Program() {
    for (int i = 0; i < funcs.size(); i++) { funcs[i]->~FuncInfo(); sk_free(funcs[i]); }
    for (int i = 0; i < classes.size(); i++) { classes[i]->~ClassInfo(); sk_free(classes[i]); }
    for (int i = 0; i < globals.size(); i++) { globals[i]->~GlobalInfo(); sk_free(globals[i]); }
  }
};

Str class_name(ClassInfo* c);
bool class_is(ClassInfo* c, ClassInfo* base);
Str inst_to_display(InstObj* o);
const char* obj_kind_name(ObjKind k);

// 値の後始末（ハンドル型）。それぞれのライブラリが持つ
void file_obj_dispose(FileObj* f);
void task_obj_dispose(TaskObj* t);
void chan_obj_dispose(ChanObj* c);
void regex_obj_dispose(RegexObj* r);
Str  json_to_text(const Value& v, int indent);

}  // namespace shark
#endif
