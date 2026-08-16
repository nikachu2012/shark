// builtin.cpp — import なしで使えるもの（spec/library/builtin.md）と、型ごとのメソッド
#include "../platform/platform.h"
#include "../registry.h"
#include "../value.h"
#include "../vm.h"

namespace shark {

Str format_value(const Value& v, const Str& spec);

static Value* A(Value* args, int i) { return val_deref(&args[i]); }
static const Str& S(Value* args, int i) { return ((StrObj*)A(args, i)->o)->s; }

// ------------------------------------------------------------------ 入出力
static NativeStatus n_print(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  vm.write_out(val_to_display(*A(a, 0)) + "\n");
  out = mk_void();
  return N_Ok;
}
static NativeStatus n_write(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  vm.write_out(val_to_display(*A(a, 0)));
  out = mk_void();
  return N_Ok;
}
static NativeStatus n_input(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  // ホストがまだ行を渡していなければ、渡されるまで待つ（端末が read で待つのと同じ）
  if (!vm.input_ready()) {
    if (vm.task()->cancel_req) return N_Cancel;
    vm.input_wait = true;
    return N_Wait;
  }
  Str line;
  if (!vm.read_line(&line)) { out = mk_none(); return N_Ok; }
  out = mk_str(line);
  return N_Ok;
}

// ------------------------------------------------------------------ 待つ・止める
static NativeStatus n_sleep(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  TaskState* t = vm.task();
  if (t->cancel_req) { t->wake_at = 0; return N_Cancel; }
  if (t->wake_at == 0) {
    double sec = A(a, 0)->f;
    if (sec <= 0) { out = mk_void(); return N_Ok; }
    t->wake_at = platform().monotonic_nanos() + (int64_t)(sec * 1e9);
    return N_Wait;
  }
  if (platform().monotonic_nanos() >= t->wake_at) {
    t->wake_at = 0;
    out = mk_void();
    return N_Ok;
  }
  return N_Wait;
}
static NativeStatus n_assert(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!(A(a, 0)->k == V_Bool && A(a, 0)->b)) {
    vm.panic(Str("assert に失敗しました: ") + S(a, 1));
    return N_Panic;
  }
  out = mk_void();
  return N_Ok;
}

// ------------------------------------------------------------------ 数え上げ
static NativeStatus n_len(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Value* v = A(a, 0);
  int64_t r = 0;
  if (v->k == V_Obj) {
    switch (v->o->kind) {
      case O_Str: r = utf8_len(((StrObj*)v->o)->s); break;
      case O_Bytes: r = ((StrObj*)v->o)->s.size(); break;
      case O_List: r = ((ListObj*)v->o)->v.size(); break;
      case O_Map: r = ((MapObj*)v->o)->live; break;
      default: break;
    }
  }
  out = mk_int(r);
  return N_Ok;
}
static NativeStatus n_range1(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_range(0, A(a, 0)->i, 1);
  return N_Ok;
}
static NativeStatus n_range2(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_range(A(a, 0)->i, A(a, 1)->i, 1);
  return N_Ok;
}
static NativeStatus n_range3(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int64_t st = A(a, 2)->i;
  if (st == 0) { vm.panic(Str("range_step の刻み幅に 0 は使えません")); return N_Panic; }
  out = mk_range(A(a, 0)->i, A(a, 1)->i, st);
  return N_Ok;
}

// ------------------------------------------------------------------ 型変換
static NativeStatus c_int_from_float(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  double d = A(a, 0)->f;
  if (d != d || d > 9.2233720368547758e18 || d < -9.2233720368547758e18) {
    vm.panic(Str("float から int に変換できません（大きすぎます）"));
    return N_Panic;
  }
  out = mk_int((int64_t)d);
  return N_Ok;
}
static NativeStatus c_int_from_string(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  int64_t v = 0;
  if (str_to_int(S(a, 0), &v)) out = mk_int(v);
  else out = mk_none();
  return N_Ok;
}
static NativeStatus c_int_from_bool(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_int(A(a, 0)->b ? 1 : 0);
  return N_Ok;
}
static NativeStatus c_float_from_int(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_float((double)A(a, 0)->i);
  return N_Ok;
}
static NativeStatus c_float_from_string(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  double v = 0;
  if (str_to_float(S(a, 0), &v)) out = mk_float(v);
  else out = mk_none();
  return N_Ok;
}
static NativeStatus c_bool_from_string(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& s = S(a, 0);
  if (s == "true") out = mk_bool(true);
  else if (s == "false") out = mk_bool(false);
  else out = mk_none();
  return N_Ok;
}
static NativeStatus c_string_from(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_str(val_to_display(*A(a, 0)));
  return N_Ok;
}
static NativeStatus n_fmt(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_str(format_value(*A(a, 0), S(a, 1)));
  return N_Ok;
}

// ------------------------------------------------------------------ Error
NativeStatus n_error_message(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  InstObj* o = (InstObj*)A(a, 0)->o;
  out = o->fields.size() > 0 ? val_retain(o->fields[0]) : mk_str("");
  return N_Ok;
}
NativeStatus n_error_code(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  InstObj* o = (InstObj*)A(a, 0)->o;
  out = o->fields.size() > 1 ? val_retain(o->fields[1]) : mk_int(0);
  return N_Ok;
}
NativeStatus n_error_init1(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  InstObj* o = (InstObj*)A(a, 0)->o;
  if (o->fields.size() > 0) { val_release(o->fields[0]); o->fields[0] = val_retain(*A(a, 1)); }
  out = val_retain(*A(a, 0));   // init は作った実体を返す
  return N_Ok;
}
NativeStatus n_error_init2(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  InstObj* o = (InstObj*)A(a, 0)->o;
  if (o->fields.size() > 1) {
    val_release(o->fields[0]);
    o->fields[0] = val_retain(*A(a, 1));
    val_release(o->fields[1]);
    o->fields[1] = val_retain(*A(a, 2));
  }
  out = val_retain(*A(a, 0));   // init は作った実体を返す
  return N_Ok;
}

// ------------------------------------------------------------------ string
static NativeStatus s_len(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_int(utf8_len(S(a, 0))); return N_Ok; }
static NativeStatus s_sub(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& s = S(a, 0);
  int len = utf8_len(s);
  int64_t st = A(a, 1)->i, en = A(a, 2)->i;
  if (st < 0) st = 0;
  if (en > len) en = len;
  if (en < st) en = st;
  int b0 = utf8_offset(s, (int)st), b1 = utf8_offset(s, (int)en);
  out = mk_str(s.sub(b0, b1 - b0));
  return N_Ok;
}
static int find_bytes(const Str& hay, const Str& needle, int from) {
  if (needle.size() == 0) return from;
  for (int i = from; i + needle.size() <= hay.size(); i++) {
    bool ok = true;
    for (int k = 0; k < needle.size(); k++) if (hay[i + k] != needle[k]) { ok = false; break; }
    if (ok) return i;
  }
  return -1;
}
static NativeStatus s_find(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& s = S(a, 0);
  int at = find_bytes(s, S(a, 1), 0);
  if (at < 0) { out = mk_none(); return N_Ok; }
  out = mk_int(utf8_len(s.sub(0, at)));
  return N_Ok;
}
static NativeStatus s_contains(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_bool(find_bytes(S(a, 0), S(a, 1), 0) >= 0); return N_Ok; }
static NativeStatus s_starts(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& s = S(a, 0);
  const Str& p = S(a, 1);
  bool ok = p.size() <= s.size();
  for (int i = 0; ok && i < p.size(); i++) if (s[i] != p[i]) ok = false;
  out = mk_bool(ok);
  return N_Ok;
}
static NativeStatus s_ends(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& s = S(a, 0);
  const Str& p = S(a, 1);
  bool ok = p.size() <= s.size();
  for (int i = 0; ok && i < p.size(); i++) if (s[s.size() - p.size() + i] != p[i]) ok = false;
  out = mk_bool(ok);
  return N_Ok;
}
static NativeStatus s_split(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& s = S(a, 0);
  const Str& sep = S(a, 1);
  out = mk_list();
  ListObj* l = as_list(out);
  if (sep.size() == 0) {
    for (int i = 0; i < s.size();) {
      int cp;
      int adv = utf8_decode(s, i, &cp);
      l->v.push(mk_str(s.sub(i, adv)));
      i += adv;
    }
    return N_Ok;
  }
  int from = 0;
  for (;;) {
    int at = find_bytes(s, sep, from);
    if (at < 0) { l->v.push(mk_str(s.sub(from, s.size() - from))); break; }
    l->v.push(mk_str(s.sub(from, at - from)));
    from = at + sep.size();
  }
  return N_Ok;
}
static NativeStatus s_trim(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& s = S(a, 0);
  int b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) b++;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) e--;
  out = mk_str(s.sub(b, e - b));
  return N_Ok;
}
static NativeStatus s_upper(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Str r = S(a, 0);
  for (int i = 0; i < r.size(); i++) if (r[i] >= 'a' && r[i] <= 'z') r[i] = (char)(r[i] - 32);
  out = mk_str(r);
  return N_Ok;
}
static NativeStatus s_lower(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Str r = S(a, 0);
  for (int i = 0; i < r.size(); i++) if (r[i] >= 'A' && r[i] <= 'Z') r[i] = (char)(r[i] + 32);
  out = mk_str(r);
  return N_Ok;
}
static NativeStatus s_replace(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& s = S(a, 0);
  const Str& from = S(a, 1);
  const Str& to = S(a, 2);
  if (from.size() == 0) { out = mk_str(s); return N_Ok; }
  Str r;
  int at = 0;
  for (;;) {
    int p = find_bytes(s, from, at);
    if (p < 0) { r += s.sub(at, s.size() - at); break; }
    r += s.sub(at, p - at);
    r += to;
    at = p + from.size();
  }
  out = mk_str(r);
  return N_Ok;
}
static NativeStatus s_bytes(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_bytes(S(a, 0)); return N_Ok; }
static NativeStatus s_chars(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& s = S(a, 0);
  out = mk_list();
  ListObj* l = as_list(out);
  for (int i = 0; i < s.size();) {
    int cp;
    int adv = utf8_decode(s, i, &cp);
    l->v.push(mk_str(s.sub(i, adv)));
    i += adv;
  }
  return N_Ok;
}

// ------------------------------------------------------------------ bytes
static NativeStatus b_len(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_int(S(a, 0).size()); return N_Ok; }
static NativeStatus b_push(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Value* recv = A(a, 0);
  Obj* o = obj_unique(*recv);
  ((StrObj*)o)->s.push((char)(A(a, 1)->i & 0xff));
  out = mk_void();
  return N_Ok;
}
static NativeStatus b_to_string(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& s = S(a, 0);
  // UTF-8 として正しいか確かめる
  int i = 0;
  bool ok = true;
  while (i < s.size()) {
    unsigned char c = (unsigned char)s[i];
    int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (len == 0 || i + len > s.size()) { ok = false; break; }
    for (int k = 1; k < len; k++)
      if (((unsigned char)s[i + k] & 0xC0) != 0x80) { ok = false; break; }
    if (!ok) break;
    i += len;
  }
  out = ok ? mk_str(s) : mk_none();
  return N_Ok;
}
static NativeStatus b_list(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& s = S(a, 0);
  out = mk_list();
  ListObj* l = as_list(out);
  for (int i = 0; i < s.size(); i++) l->v.push(mk_int((unsigned char)s[i]));
  return N_Ok;
}
static NativeStatus b_sub(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& s = S(a, 0);
  int64_t st = A(a, 1)->i, en = A(a, 2)->i;
  if (st < 0) st = 0;
  if (en > s.size()) en = s.size();
  if (en < st) en = st;
  out = mk_bytes(s.sub((int)st, (int)(en - st)));
  return N_Ok;
}
static NativeStatus i_to_bytes(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Str s;
  uint64_t u = (uint64_t)A(a, 0)->i;
  for (int i = 0; i < 8; i++) s.push((char)((u >> (i * 8)) & 0xff));
  out = mk_bytes(s);
  return N_Ok;
}
static NativeStatus i_wadd(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_int((int64_t)((uint64_t)A(a, 0)->i + (uint64_t)A(a, 1)->i)); return N_Ok; }
static NativeStatus i_wsub(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_int((int64_t)((uint64_t)A(a, 0)->i - (uint64_t)A(a, 1)->i)); return N_Ok; }
static NativeStatus i_wmul(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_int((int64_t)((uint64_t)A(a, 0)->i * (uint64_t)A(a, 1)->i)); return N_Ok; }

// ------------------------------------------------------------------ list
static NativeStatus l_push(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Value* recv = A(a, 0);
  ListObj* l = (ListObj*)obj_unique(*recv);
  l->v.push(val_retain(*A(a, 1)));
  out = mk_void();
  return N_Ok;
}
static NativeStatus l_pop(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Value* recv = A(a, 0);
  ListObj* l = (ListObj*)obj_unique(*recv);
  if (l->v.size() == 0) { out = mk_none(); return N_Ok; }
  out = l->v.back();
  l->v.pop();
  return N_Ok;
}
static NativeStatus l_len(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_int(((ListObj*)A(a, 0)->o)->v.size()); return N_Ok; }
static NativeStatus l_insert(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Value* recv = A(a, 0);
  ListObj* l = (ListObj*)obj_unique(*recv);
  int64_t at = A(a, 1)->i;
  if (at < 0 || at > l->v.size()) {
    vm.panic(Str("配列の長さは ") + str_from_int(l->v.size()) + " ですが、" + str_from_int(at) + " 番目に入れようとしました");
    return N_Panic;
  }
  l->v.insert((int)at, val_retain(*A(a, 2)));
  out = mk_void();
  return N_Ok;
}
static NativeStatus l_remove(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Value* recv = A(a, 0);
  ListObj* l = (ListObj*)obj_unique(*recv);
  int64_t at = A(a, 1)->i;
  if (at < 0 || at >= l->v.size()) {
    vm.panic(Str("配列の長さは ") + str_from_int(l->v.size()) + " ですが、" + str_from_int(at) + " 番目を消そうとしました");
    return N_Panic;
  }
  Value v = l->v[(int)at];
  val_release(v);
  l->v.remove((int)at);
  out = mk_void();
  return N_Ok;
}
static NativeStatus l_contains(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  ListObj* l = (ListObj*)A(a, 0)->o;
  bool found = false;
  for (int i = 0; i < l->v.size(); i++) if (val_equal(l->v[i], *A(a, 1))) { found = true; break; }
  out = mk_bool(found);
  return N_Ok;
}
static NativeStatus l_join(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  ListObj* l = (ListObj*)A(a, 0)->o;
  const Str& sep = S(a, 1);
  Str r;
  for (int i = 0; i < l->v.size(); i++) {
    if (i) r += sep;
    r += val_to_display(l->v[i]);
  }
  out = mk_str(r);
  return N_Ok;
}

// ------------------------------------------------------------------ map
static NativeStatus m_get(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  MapObj* m = (MapObj*)A(a, 0)->o;
  Value* p = map_find(m, *A(a, 1));
  out = p ? val_retain(*p) : mk_none();
  return N_Ok;
}
static NativeStatus m_has(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_bool(map_find((MapObj*)A(a, 0)->o, *A(a, 1)) != 0);
  return N_Ok;
}
static NativeStatus m_remove(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Value* recv = A(a, 0);
  MapObj* m = (MapObj*)obj_unique(*recv);
  map_remove(m, *A(a, 1));
  out = mk_void();
  return N_Ok;
}
static NativeStatus m_len(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_int(((MapObj*)A(a, 0)->o)->live); return N_Ok; }
static NativeStatus m_keys(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  MapObj* m = (MapObj*)A(a, 0)->o;
  out = mk_list();
  ListObj* l = as_list(out);
  for (int i = 0; i < m->e.size(); i++) if (!m->e[i].dead) l->v.push(val_retain(m->e[i].key));
  return N_Ok;
}
static NativeStatus m_values(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  MapObj* m = (MapObj*)A(a, 0)->o;
  out = mk_list();
  ListObj* l = as_list(out);
  for (int i = 0; i < m->e.size(); i++) if (!m->e[i].dead) l->v.push(val_retain(m->e[i].val));
  return N_Ok;
}

// ------------------------------------------------------------------ Result
static NativeStatus r_ok(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_bool(((ResultObj*)A(a, 0)->o)->ok); return N_Ok; }
static NativeStatus r_value(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  ResultObj* r = (ResultObj*)A(a, 0)->o;
  if (!r->ok) { vm.panic(Str("失敗した結果に value() を呼びました。先に ok() で調べます")); return N_Panic; }
  out = val_retain(r->val);
  return N_Ok;
}
static NativeStatus r_error(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  ResultObj* r = (ResultObj*)A(a, 0)->o;
  if (r->ok) { out = vm.make_error(Str("失敗していません"), 0); return N_Ok; }
  out = val_retain(r->val);
  return N_Ok;
}

// ------------------------------------------------------------------ 登録
void register_builtin(Registry& r) {
  TypeTable& t = r.types();
  Type* ti = t.t_int();
  Type* tf = t.t_float();
  Type* tb = t.t_bool();
  Type* ts = t.t_string();
  Type* tv = t.t_void();
  Type* tr = t.simple(T_Range);

  r.add_untyped("print", n_print);
  r.add_untyped("write", n_write);
  r.add("input", n_input, t.optional_of(ts));
  r.add("sleep", n_sleep, tv, tf);
  r.add("assert", n_assert, tv, tb, ts);
  r.add_untyped("len", n_len);
  r.add("range", n_range1, tr, ti);
  r.add("range", n_range2, tr, ti, ti);
  r.add("range_step", n_range3, tr, ti, ti, ti);

  r.add("conv.int_from_float", c_int_from_float, ti, tf);
  r.add("conv.int_from_string", c_int_from_string, t.optional_of(ti), ts);
  r.add("conv.int_from_bool", c_int_from_bool, ti, tb);
  r.add("conv.float_from_int", c_float_from_int, tf, ti);
  r.add("conv.float_from_string", c_float_from_string, t.optional_of(tf), ts);
  r.add("conv.bool_from_string", c_bool_from_string, t.optional_of(tb), ts);
  r.add_untyped("conv.string_from", c_string_from);
  r.add_untyped("__fmt", n_fmt);
}

void register_methods(Registry& r) {
  r.add_untyped("string.len", s_len);
  r.add_untyped("string.sub", s_sub);
  r.add_untyped("string.find", s_find);
  r.add_untyped("string.contains", s_contains);
  r.add_untyped("string.starts_with", s_starts);
  r.add_untyped("string.ends_with", s_ends);
  r.add_untyped("string.split", s_split);
  r.add_untyped("string.trim", s_trim);
  r.add_untyped("string.upper", s_upper);
  r.add_untyped("string.lower", s_lower);
  r.add_untyped("string.replace", s_replace);
  r.add_untyped("string.bytes", s_bytes);
  r.add_untyped("string.chars", s_chars);

  r.add_untyped("bytes.len", b_len);
  r.add_untyped("bytes.push", b_push);
  r.add_untyped("bytes.to_string", b_to_string);
  r.add_untyped("bytes.list", b_list);
  r.add_untyped("bytes.sub", b_sub);

  r.add_untyped("int.to_bytes", i_to_bytes);
  r.add_untyped("int.wrapping_add", i_wadd);
  r.add_untyped("int.wrapping_sub", i_wsub);
  r.add_untyped("int.wrapping_mul", i_wmul);

  r.add_untyped("list.push", l_push);
  r.add_untyped("list.pop", l_pop);
  r.add_untyped("list.len", l_len);
  r.add_untyped("list.insert", l_insert);
  r.add_untyped("list.remove", l_remove);
  r.add_untyped("list.contains", l_contains);
  r.add_untyped("list.join", l_join);

  r.add_untyped("map.get", m_get);
  r.add_untyped("map.has", m_has);
  r.add_untyped("map.remove", m_remove);
  r.add_untyped("map.len", m_len);
  r.add_untyped("map.keys", m_keys);
  r.add_untyped("map.values", m_values);

  r.add_untyped("result.ok", r_ok);
  r.add_untyped("result.value", r_value);
  r.add_untyped("result.error", r_error);
}

}  // namespace shark
