// path.cpp — std.path（spec/library/path.md）
#include "../platform/platform.h"
#include "../registry.h"
#include "../value.h"
#include "../vm.h"

namespace shark {

static Value* A(Value* args, int i) { return val_deref(&args[i]); }
static const Str& S(Value* args, int i) { return ((StrObj*)A(args, i)->o)->s; }

static bool is_sep(char c) { return c == '/' || c == '\\'; }

static Str sep_str() {
#if defined(_WIN32)
  return Str("\\");
#else
  return Str("/");
#endif
}

static Str join2(const Str& a, const Str& b) {
  if (a.size() == 0) return b;
  if (b.size() == 0) return a;
  if (is_sep(b[0])) return b;  // b が絶対パスならそのまま
  Str r = a;
  if (!is_sep(r[r.size() - 1])) r += sep_str();
  r += b;
  return r;
}

static NativeStatus p_join(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_str(join2(S(a, 0), S(a, 1)));
  return N_Ok;
}
static NativeStatus p_join_all(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  ListObj* l = (ListObj*)A(a, 0)->o;
  Str r;
  for (int i = 0; i < l->v.size(); i++) r = join2(r, ((StrObj*)l->v[i].o)->s);
  out = mk_str(r);
  return N_Ok;
}
static int last_sep(const Str& p) {
  for (int i = p.size() - 1; i >= 0; i--) if (is_sep(p[i])) return i;
  return -1;
}
static NativeStatus p_dir(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& p = S(a, 0);
  int i = last_sep(p);
  if (i < 0) { out = mk_str("."); return N_Ok; }
  if (i == 0) { out = mk_str(p.sub(0, 1)); return N_Ok; }
  out = mk_str(p.sub(0, i));
  return N_Ok;
}
static NativeStatus p_name(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& p = S(a, 0);
  int i = last_sep(p);
  out = mk_str(p.sub(i + 1, p.size() - i - 1));
  return N_Ok;
}
static int ext_pos(const Str& name) {
  for (int i = name.size() - 1; i > 0; i--) {
    if (is_sep(name[i])) break;
    if (name[i] == '.') return i;
  }
  return -1;
}
static NativeStatus p_stem(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& p = S(a, 0);
  Str name = p.sub(last_sep(p) + 1, p.size() - last_sep(p) - 1);
  int e = ext_pos(name);
  out = mk_str(e < 0 ? name : name.sub(0, e));
  return N_Ok;
}
static NativeStatus p_ext(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& p = S(a, 0);
  Str name = p.sub(last_sep(p) + 1, p.size() - last_sep(p) - 1);
  int e = ext_pos(name);
  out = mk_str(e < 0 ? Str() : name.sub(e, name.size() - e));
  return N_Ok;
}
static NativeStatus p_normalize(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& p = S(a, 0);
  bool absolute = p.size() > 0 && is_sep(p[0]);
  Vec<Str> parts;
  Str cur;
  for (int i = 0; i <= p.size(); i++) {
    if (i == p.size() || is_sep(p[i])) {
      if (cur == "." || cur.size() == 0) { cur.clear(); continue; }
      if (cur == "..") {
        if (parts.size() > 0 && !(parts.back() == "..")) parts.pop();
        else if (!absolute) parts.push(cur);
      } else {
        parts.push(cur);
      }
      cur.clear();
      continue;
    }
    cur.push(p[i]);
  }
  Str r;
  if (absolute) r += sep_str();
  for (int i = 0; i < parts.size(); i++) {
    if (i) r += sep_str();
    r += parts[i];
  }
  if (r.size() == 0) r = Str(".");
  out = mk_str(r);
  return N_Ok;
}
static NativeStatus p_is_absolute(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& p = S(a, 0);
  bool abs = p.size() > 0 && (is_sep(p[0]) || (p.size() > 2 && p[1] == ':' && is_sep(p[2])));
  out = mk_bool(abs);
  return N_Ok;
}
static NativeStatus p_absolute(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  const Str& p = S(a, 0);
  if (p.size() > 0 && is_sep(p[0])) { out = mk_result_ok(mk_str(p)); return N_Ok; }
  if (!platform().os) {
    out = mk_result_err(vm.make_error(Str("この処理系はいまいる場所を知りません"), 0));
    return N_Ok;
  }
  Str cwd;
  if (!platform().os->cwd(&cwd)) {
    out = mk_result_err(vm.make_error(Str("いまいる場所を取れません"), 0));
    return N_Ok;
  }
  out = mk_result_ok(mk_str(join2(cwd, p)));
  return N_Ok;
}
static NativeStatus p_separator(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_str(sep_str());
  return N_Ok;
}

void register_path(Registry& r) {
  TypeTable& t = r.types();
  Type* ts = t.t_string();
  Type* tb = t.t_bool();
  r.add("path.join", p_join, ts, ts, ts);
  r.add("path.join_all", p_join_all, ts, t.list_of(ts));
  r.add("path.dir", p_dir, ts, ts);
  r.add("path.name", p_name, ts, ts);
  r.add("path.stem", p_stem, ts, ts);
  r.add("path.ext", p_ext, ts, ts);
  r.add("path.normalize", p_normalize, ts, ts);
  r.add("path.absolute", p_absolute, t.result_of(ts), ts);
  r.add("path.is_absolute", p_is_absolute, tb, ts);
  r.add("path.separator", p_separator, ts);
  r.enable_module("std.path");
}

}  // namespace shark
