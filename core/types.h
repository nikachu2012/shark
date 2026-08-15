// types.h — 型の表し方（spec/types/）
#ifndef SHARK_TYPES_H
#define SHARK_TYPES_H

#include "support.h"

namespace shark {

struct ClassInfo;

enum TypeKind : uint8_t {
  T_Unknown = 0,  // 誤りからの復帰に使う内部の型。どんな型とも合う
  T_Void, T_Int, T_Float, T_Bool, T_String, T_Bytes,
  T_List, T_Map, T_Optional, T_Result,
  T_Class,      // ユーザー定義のクラス、Error、インタフェース
  T_Generic,    // 型引数 T
  T_Func,
  T_Range, T_Task, T_Channel,
  T_Time, T_Duration, T_File, T_Json, T_Regex, T_Match, T_Output,
};

struct Type {
  TypeKind kind;
  Type* a;        // list の要素 / map のキー / optional・Result・Task・channel の中身
  Type* b;        // map の値
  ClassInfo* cls; // T_Class
  Str name;       // T_Generic の名前
  Vec<Type*> params;  // T_Func の引数
  Type* ret;          // T_Func の戻り値
  Vec<ClassInfo*> constraints;  // T_Generic の制約
  Vec<Type*> targs;             // T_Class の型引数（Box<int> など）
  Type() : kind(T_Unknown), a(0), b(0), cls(0), ret(0) {}
};

class TypeTable {
 public:
  TypeTable();
  ~TypeTable();

  Type* simple(TypeKind k);
  Type* list_of(Type* e);
  Type* map_of(Type* k, Type* v);
  Type* optional_of(Type* t);
  Type* result_of(Type* t);
  Type* task_of(Type* t);
  Type* channel_of(Type* t);
  Type* class_type(ClassInfo* c);
  Type* class_type_args(ClassInfo* c, const Vec<Type*>& targs);
  Type* generic(const Str& name);
  Type* func_type(const Vec<Type*>& params, Type* ret);

  Type* t_unknown() { return simple(T_Unknown); }
  Type* t_void() { return simple(T_Void); }
  Type* t_int() { return simple(T_Int); }
  Type* t_float() { return simple(T_Float); }
  Type* t_bool() { return simple(T_Bool); }
  Type* t_string() { return simple(T_String); }
  Type* t_bytes() { return simple(T_Bytes); }

 private:
  Type* alloc(TypeKind k);
  Vec<Type*> all_;
  Type* simple_[T_Output + 1];
};

// 名前（診断に出す形）
Str type_name(Type* t);
// 同じ型か
bool type_same(Type* a, Type* b);
// from の値を to として使えるか（暗黙変換はしない。none と部分型だけ）
bool type_assignable(Type* to, Type* from);
// クラス c は base を継承（または実装）しているか
bool class_is(ClassInfo* c, ClassInfo* base);

inline bool is_optional(Type* t) { return t && t->kind == T_Optional; }
inline bool is_result(Type* t) { return t && t->kind == T_Result; }
inline bool is_numeric(Type* t) { return t && (t->kind == T_Int || t->kind == T_Float); }

}  // namespace shark
#endif
