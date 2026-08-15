#include "registry.h"

namespace shark {

int Registry::add(const char* name, NativeFn fn, Type* ret, Type* p0, Type* p1, Type* p2, Type* p3) {
  NativeEntry e;
  e.name = Str(name);
  e.fn = fn;
  e.ret = ret ? ret : types_.t_void();
  if (p0) e.params.push(p0);
  if (p1) e.params.push(p1);
  if (p2) e.params.push(p2);
  if (p3) e.params.push(p3);
  e_.push(e);
  return e_.size() - 1;
}

int Registry::add_untyped(const char* name, NativeFn fn) {
  NativeEntry e;
  e.name = Str(name);
  e.fn = fn;
  e.typed = false;
  e.ret = types_.t_void();
  e_.push(e);
  return e_.size() - 1;
}

int Registry::find(const char* name) const {
  for (int i = 0; i < e_.size(); i++) if (e_[i].name == name) return i;
  return -1;
}

void Registry::find_all(const Str& name, Vec<int>* out) const {
  for (int i = 0; i < e_.size(); i++) if (e_[i].name == name) out->push(i);
}

}  // namespace shark
