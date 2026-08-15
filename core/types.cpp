#include "types.h"
#include "program.h"

namespace shark {

TypeTable::TypeTable() { for (int i = 0; i <= (int)T_Output; i++) simple_[i] = 0; }

TypeTable::~TypeTable() {
  for (int i = 0; i < all_.size(); i++) { all_[i]->~Type(); sk_free(all_[i]); }
}

Type* TypeTable::alloc(TypeKind k) {
  Type* t = new (sk_alloc(sizeof(Type))) Type();
  t->kind = k;
  all_.push(t);
  return t;
}

Type* TypeTable::simple(TypeKind k) {
  if (!simple_[k]) simple_[k] = alloc(k);
  return simple_[k];
}

Type* TypeTable::list_of(Type* e) {
  for (int i = 0; i < all_.size(); i++)
    if (all_[i]->kind == T_List && all_[i]->a == e) return all_[i];
  Type* t = alloc(T_List); t->a = e; return t;
}
Type* TypeTable::map_of(Type* k, Type* v) {
  for (int i = 0; i < all_.size(); i++)
    if (all_[i]->kind == T_Map && all_[i]->a == k && all_[i]->b == v) return all_[i];
  Type* t = alloc(T_Map); t->a = k; t->b = v; return t;
}
Type* TypeTable::optional_of(Type* x) {
  if (!x) return 0;
  if (x->kind == T_Optional) return x;  // T?? は作らない
  for (int i = 0; i < all_.size(); i++)
    if (all_[i]->kind == T_Optional && all_[i]->a == x) return all_[i];
  Type* t = alloc(T_Optional); t->a = x; return t;
}
Type* TypeTable::result_of(Type* x) {
  for (int i = 0; i < all_.size(); i++)
    if (all_[i]->kind == T_Result && all_[i]->a == x) return all_[i];
  Type* t = alloc(T_Result); t->a = x; return t;
}
Type* TypeTable::task_of(Type* x) {
  for (int i = 0; i < all_.size(); i++)
    if (all_[i]->kind == T_Task && all_[i]->a == x) return all_[i];
  Type* t = alloc(T_Task); t->a = x; return t;
}
Type* TypeTable::channel_of(Type* x) {
  for (int i = 0; i < all_.size(); i++)
    if (all_[i]->kind == T_Channel && all_[i]->a == x) return all_[i];
  Type* t = alloc(T_Channel); t->a = x; return t;
}
Type* TypeTable::class_type(ClassInfo* c) {
  for (int i = 0; i < all_.size(); i++)
    if (all_[i]->kind == T_Class && all_[i]->cls == c) return all_[i];
  Type* t = alloc(T_Class); t->cls = c; return t;
}
Type* TypeTable::class_type_args(ClassInfo* c, const Vec<Type*>& targs) {
  if (targs.size() == 0) return class_type(c);
  for (int i = 0; i < all_.size(); i++) {
    Type* t = all_[i];
    if (t->kind != T_Class || t->cls != c || t->targs.size() != targs.size()) continue;
    bool same = true;
    for (int k = 0; k < targs.size(); k++) if (t->targs[k] != targs[k]) { same = false; break; }
    if (same) return t;
  }
  Type* t = alloc(T_Class); t->cls = c; t->targs = targs; return t;
}

Type* TypeTable::generic(const Str& name) {
  Type* t = alloc(T_Generic); t->name = name; return t;
}
Type* TypeTable::func_type(const Vec<Type*>& params, Type* ret) {
  for (int i = 0; i < all_.size(); i++) {
    Type* t = all_[i];
    if (t->kind != T_Func || t->ret != ret || t->params.size() != params.size()) continue;
    bool same = true;
    for (int k = 0; k < params.size(); k++) if (t->params[k] != params[k]) { same = false; break; }
    if (same) return t;
  }
  Type* t = alloc(T_Func); t->params = params; t->ret = ret; return t;
}

Str type_name(Type* t) {
  if (!t) return Str("?");
  switch (t->kind) {
    case T_Unknown: return Str("?");
    case T_Void: return Str("void");
    case T_Int: return Str("int");
    case T_Float: return Str("float");
    case T_Bool: return Str("bool");
    case T_String: return Str("string");
    case T_Bytes: return Str("bytes");
    case T_List: return Str("list<") + type_name(t->a) + ">";
    case T_Map: return Str("map<") + type_name(t->a) + ", " + type_name(t->b) + ">";
    case T_Optional: return type_name(t->a) + "?";
    case T_Result: return Str("Result<") + type_name(t->a) + ">";
    case T_Class: {
      if (!t->cls) return Str("class");
      Str r = class_name(t->cls);
      if (t->targs.size()) {
        r += "<";
        for (int i = 0; i < t->targs.size(); i++) { if (i) r += ", "; r += type_name(t->targs[i]); }
        r += ">";
      }
      return r;
    }
    case T_Generic: return t->name;
    case T_Func: {
      Str r("func(");
      for (int i = 0; i < t->params.size(); i++) { if (i) r += ", "; r += type_name(t->params[i]); }
      r += ")";
      if (t->ret && t->ret->kind != T_Void) { r += " -> "; r += type_name(t->ret); }
      return r;
    }
    case T_Range: return Str("Range");
    case T_Task: return Str("Task<") + type_name(t->a) + ">";
    case T_Channel: return Str("channel<") + type_name(t->a) + ">";
    case T_Time: return Str("Time");
    case T_Duration: return Str("Duration");
    case T_File: return Str("File");
    case T_Json: return Str("Json");
    case T_Regex: return Str("Regex");
    case T_Match: return Str("Match");
    case T_Output: return Str("Output");
  }
  return Str("?");
}

bool type_same(Type* a, Type* b) {
  if (a == b) return true;
  if (!a || !b) return false;
  if (a->kind == T_Unknown || b->kind == T_Unknown) return true;
  if (a->kind != b->kind) return false;
  switch (a->kind) {
    case T_List: case T_Optional: case T_Result: case T_Task: case T_Channel:
      return type_same(a->a, b->a);
    case T_Map: return type_same(a->a, b->a) && type_same(a->b, b->b);
    case T_Class: {
      if (a->cls != b->cls) return false;
      if (a->targs.size() != b->targs.size()) return false;
      for (int i = 0; i < a->targs.size(); i++) if (!type_same(a->targs[i], b->targs[i])) return false;
      return true;
    }
    case T_Generic: return a->name == b->name;
    case T_Func: {
      if (a->params.size() != b->params.size()) return false;
      for (int i = 0; i < a->params.size(); i++) if (!type_same(a->params[i], b->params[i])) return false;
      return type_same(a->ret, b->ret);
    }
    default: return true;
  }
}

bool type_assignable(Type* to, Type* from) {
  if (!to || !from) return false;
  if (to->kind == T_Unknown || from->kind == T_Unknown) return true;
  if (type_same(to, from)) return true;
  // T の値は T? に入れられる（逆はできない）
  if (to->kind == T_Optional && type_assignable(to->a, from)) return true;
  // 子クラスは親・インタフェースとして扱える
  if (to->kind == T_Class && from->kind == T_Class) return class_is(from->cls, to->cls);
  if (to->kind == T_Optional && from->kind == T_Optional) return type_assignable(to->a, from->a);
  return false;
}

}  // namespace shark
