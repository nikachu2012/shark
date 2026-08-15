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
