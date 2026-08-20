#include "program.h"

namespace shark {

Str class_name(ClassInfo* c) { return c ? c->name : Str("?"); }

bool class_is(ClassInfo* c, ClassInfo* base) {
  if (!c || !base) return false;
  if (c == base) return true;
  for (ClassInfo* p = c; p; p = p->base) {
    if (p == base) return true;
    for (int i = 0; i < p->interfaces.size(); i++)
      if (class_is(p->interfaces[i], base)) return true;
  }
  return false;
}

// クラスが自分自身を値として持っていないか（代入がコピーなので大きさが決まらない）。
// 輪になっていると、既定値を作るところ（VM::default_of）が戻ってこない
bool class_holds_by_value(ClassInfo* c, ClassInfo* target, Vec<ClassInfo*>* seen) {
  for (int i = 0; i < c->fields.size(); i++) {
    Type* ft = c->fields[i].type;
    if (!ft || ft->kind != T_Class || !ft->cls) continue;
    if (ft->cls == target) return true;
    bool visited = false;
    for (int k = 0; k < seen->size(); k++) if ((*seen)[k] == ft->cls) visited = true;
    if (visited) continue;
    seen->push(ft->cls);
    if (class_holds_by_value(ft->cls, target, seen)) return true;
  }
  return false;
}

Str inst_to_display(InstObj* o) {
  Str r = o->cls ? o->cls->name : Str("object");
  r += "(";
  for (int i = 0; i < o->fields.size(); i++) {
    if (i) r += ", ";
    if (o->cls && i < o->cls->fields.size()) { r += o->cls->fields[i].name; r += ": "; }
    bool q = o->fields[i].k == V_Obj && o->fields[i].o->kind == O_Str;
    if (q) r += "\"";
    r += val_to_display(o->fields[i]);
    if (q) r += "\"";
  }
  r += ")";
  return r;
}

}  // namespace shark
