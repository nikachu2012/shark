// value.h — 値の表し方（spec/runtime/memory.md）
//
// ・代入はコピー。実装は参照数を数え、書き込み時に複製する（copy on write）
// ・寿命は参照カウントで決める。ごみ集めは持たない（循環は作れない）
// ・ハンドル型（File / Task / channel）だけは、コピーしても同じ実体を指す
#ifndef SHARK_VALUE_H
#define SHARK_VALUE_H

#include "support.h"

namespace shark {

struct Obj;
struct ClassInfo;
struct VM;
struct TaskState;
struct ChannelState;

enum ValKind : uint8_t {
  V_Void = 0,   // 値なし（void）
  V_None,       // none（T? の空）
  V_Int,
  V_Float,
  V_Bool,
  V_Obj,        // 参照カウントを持つ実体
  V_Ref,        // ref 引数の間だけ現れる借用
};

struct Value {
  ValKind k;
  union {
    int64_t i;
    double  f;
    bool    b;
    Obj*    o;
    Value*  r;
  };
  Value() : k(V_Void) { i = 0; }
};

enum ObjKind : uint8_t {
  O_Str, O_Bytes, O_List, O_Map, O_Inst, O_Result, O_Range, O_Func,
  O_Time, O_Dur, O_File, O_Json, O_Task, O_Chan, O_Regex, O_Match,
};

struct Obj {
  int32_t rc;
  ObjKind kind;
  Obj(ObjKind k) : rc(1), kind(k) {}
};

struct StrObj : Obj {   // string と bytes（中身は同じ器）
  Str s;
  StrObj(ObjKind k) : Obj(k) {}
};

struct ListObj : Obj {
  Vec<Value> v;
  ListObj() : Obj(O_List) {}
};

struct MapEntry { Value key; Value val; bool dead; MapEntry() : dead(false) {} };

struct MapObj : Obj {
  Vec<MapEntry> e;      // 挿入した順に並べる（反復の順はこれ）
  Vec<int32_t> idx;     // 引くための索引（開番地法）。-1 は空、-2 は消した跡
  int live;
  MapObj() : Obj(O_Map), live(0) {}
};

struct InstObj : Obj {  // クラスの実体。どのクラスかを持つので、コピーしても型は変わらない
  ClassInfo* cls;
  Vec<Value> fields;
  InstObj() : Obj(O_Inst), cls(0) {}
};

struct ResultObj : Obj {  // 成功なら val、失敗なら val に Error の実体
  bool ok;
  Value val;
  ResultObj() : Obj(O_Result), ok(true) {}
};

struct RangeObj : Obj {
  int64_t start, end, step;
  RangeObj() : Obj(O_Range), start(0), end(0), step(1) {}
};

struct FuncObj : Obj {  // 関数を値として持つ（func(int) -> bool など）
  int fn;
  FuncObj() : Obj(O_Func), fn(-1) {}
};

struct TimeObj : Obj { int64_t unix_ns; TimeObj() : Obj(O_Time), unix_ns(0) {} };
struct DurObj  : Obj { int64_t ns; DurObj() : Obj(O_Dur), ns(0) {} };

struct FileObj : Obj {  // ハンドル型
  void* h; bool closed; Str pending; bool eof;
  FileObj() : Obj(O_File), h(0), closed(false), eof(false) {}
};

enum JsonKind { J_None, J_Bool, J_Num, J_Str, J_List, J_Map };
struct JsonObj : Obj {
  JsonKind jk;
  bool b; double num; bool num_is_int; int64_t inum;
  Str s;
  Vec<Str> keys;     // J_Map のとき
  Vec<Value> items;  // J_List / J_Map の値（Json）
  JsonObj() : Obj(O_Json), jk(J_None), b(false), num(0), num_is_int(false), inum(0) {}
};

struct TaskObj : Obj { TaskState* t; TaskObj() : Obj(O_Task), t(0) {} };
struct ChanObj : Obj { ChannelState* c; ChanObj() : Obj(O_Chan), c(0) {} };

struct RegexProg;
struct RegexObj : Obj { RegexProg* p; RegexObj() : Obj(O_Regex), p(0) {} };
struct MatchObj : Obj {
  Vec<Str> groups; Vec<int> starts; Vec<int> ends;  // 文字単位
  MatchObj() : Obj(O_Match) {}
};

// --- 作る -----------------------------------------------------------------
Value mk_void();
Value mk_none();
Value mk_int(int64_t v);
Value mk_float(double v);
Value mk_bool(bool v);
Value mk_str(const Str& s);
Value mk_str(const char* s);
Value mk_bytes(const Str& s);
Value mk_list();
Value mk_map();
Value mk_inst(ClassInfo* c);
Value mk_result_ok(const Value& v);
Value mk_result_err(const Value& err);
Value mk_range(int64_t s, int64_t e, int64_t st);
Value mk_func(int fn);
Value mk_time(int64_t ns);
Value mk_dur(int64_t ns);
Value mk_json();
Value mk_obj_value(Obj* o);  // 参照数を増やさずに包む

// --- 参照カウント ---------------------------------------------------------
void obj_retain(Obj* o);
void obj_release(Obj* o);
inline Value val_retain(const Value& v) { if (v.k == V_Obj) obj_retain(v.o); return v; }
inline void  val_release(Value& v) { if (v.k == V_Obj) { obj_release(v.o); v.k = V_Void; v.i = 0; } }

// ref 引数の借用をたどる
inline Value* val_deref(Value* v) { while (v->k == V_Ref) v = v->r; return v; }

// 片付け待ちの置き場を返す（処理系を捨てるときに一度だけ呼ぶ）
void obj_release_pool_free();

// 中身を書き換える前に呼ぶ。参照数が 2 以上なら複製して、自分だけの実体にする
Obj* obj_unique(Value& v);
inline StrObj*  as_str(const Value& v) { return (StrObj*)v.o; }
inline ListObj* as_list(const Value& v) { return (ListObj*)v.o; }
inline MapObj*  as_map(const Value& v) { return (MapObj*)v.o; }
inline InstObj* as_inst(const Value& v) { return (InstObj*)v.o; }
inline ResultObj* as_result(const Value& v) { return (ResultObj*)v.o; }
inline JsonObj* as_json(const Value& v) { return (JsonObj*)v.o; }

// --- 比べる ---------------------------------------------------------------
bool val_equal(const Value& a, const Value& b);
uint64_t val_hash(const Value& v);
int  val_compare(const Value& a, const Value& b);  // 基本型のみ。-1/0/1

// --- map の操作 -----------------------------------------------------------
Value* map_find(MapObj* m, const Value& key);
void   map_set(MapObj* m, const Value& key, const Value& val);
bool   map_remove(MapObj* m, const Value& key);

// --- 文字列（UTF-8）------------------------------------------------------
int  utf8_len(const Str& s);                 // コードポイントの個数
int  utf8_offset(const Str& s, int chars);   // 文字位置 → バイト位置（超えたら size）
int  utf8_decode(const Str& s, int at, int* cp);  // 進んだバイト数
void utf8_encode(Str& out, int cp);
int  utf8_display_width(const Str& s);       // 全角は 2

// --- 表示 -----------------------------------------------------------------
Str val_to_display(const Value& v);  // print / string() 用（クラスは呼び出し側で to_string）

}  // namespace shark
#endif
