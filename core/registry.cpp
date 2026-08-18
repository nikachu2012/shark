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

// 番号の並びを決める場所。ここを触ると、前に保存したバイトコードは読めなくなる
// （指紋が変わるので、読むときに気づける）
void register_modules(Registry& r, const Config& cfg) {
  // 必ず入るもの
  register_builtin(r);
  register_methods(r);
  register_math(r);
  register_time(r);
  register_task(r);
  // 選べるもの（spec/library/overview.md）
  if (cfg.with_fmt) register_fmt(r);
  if (cfg.with_path) register_path(r);
  if (cfg.with_file) register_file(r);
  if (cfg.with_text) register_text(r);
  if (cfg.with_json) register_json(r);
  if (cfg.with_os) register_os(r);
  if (cfg.with_crypto) register_crypto(r);
  if (cfg.with_test) register_test(r);
}

uint64_t registry_signature(const Registry& r) {
  uint64_t h = 0xcbf29ce484222325ull;   // FNV-1a
  for (int i = 0; i < r.size(); i++) {
    const NativeEntry& e = r[i];
    Str k = e.name;
    k += "/";
    k += str_from_int(e.params.size());
    k += e.typed ? "t" : "u";
    for (int j = 0; j < k.size(); j++) {
      h ^= (uint64_t)(unsigned char)k[j];
      h *= 0x100000001b3ull;
    }
    h ^= 0x2f;
    h *= 0x100000001b3ull;
  }
  return h;
}

}  // namespace shark
