// fmt.cpp — std.fmt（spec/library/fmt.md）
#include "../registry.h"
#include "../value.h"
#include "../vm.h"

namespace shark {

Str format_value(const Value& v, const Str& spec);

static Value* A(Value* args, int i) { return val_deref(&args[i]); }
static const Str& S(Value* args, int i) { return ((StrObj*)A(args, i)->o)->s; }

static NativeStatus f_apply(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_str(format_value(*A(a, 0), S(a, 1)));
  return N_Ok;
}
static NativeStatus f_bytes(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  double v = (double)A(a, 0)->i;
  const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  int u = 0;
  while (v >= 1024.0 && u < 5) { v /= 1024.0; u++; }
  Str r;
  if (u == 0) r = str_from_int((int64_t)v);
  else r = format_value(mk_float(v), Str(".1f"));
  // 小数第1位が 0 なら整数で見せる
  if (u > 0 && r.size() > 2 && r[r.size() - 1] == '0' && r[r.size() - 2] == '.')
    r = r.sub(0, r.size() - 2);
  r += " ";
  r += units[u];
  out = mk_str(r);
  return N_Ok;
}
static NativeStatus f_duration(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  int64_t ns = ((DurObj*)A(a, 0)->o)->ns;
  bool neg = ns < 0;
  if (neg) ns = -ns;
  int64_t total = ns / 1000000000ll;
  int64_t h = total / 3600, m = (total % 3600) / 60, s = total % 60;
  int64_t ms = (ns % 1000000000ll) / 1000000;
  Str r;
  if (neg) r += "-";
  if (h > 0) { r += str_from_int(h); r += "時間"; }
  if (h > 0 || m > 0) { r += str_from_int(m); r += "分"; }
  if (h == 0 && m == 0 && total == 0) {
    r += str_from_int(ms);
    r += "ミリ秒";
  } else {
    r += str_from_int(s);
    r += "秒";
  }
  out = mk_str(r);
  return N_Ok;
}
static NativeStatus f_truncate(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& s = S(a, 0);
  int64_t width = A(a, 1)->i;
  if (utf8_display_width(s) <= width) { out = mk_str(s); return N_Ok; }
  Str r;
  int w = 0;
  for (int i = 0; i < s.size();) {
    int cp;
    int adv = utf8_decode(s, i, &cp);
    int cw = utf8_display_width(s.sub(i, adv));
    if (w + cw > width) break;   // 幅の分だけ残して … を足す
    r.append(s.data() + i, adv);
    w += cw;
    i += adv;
  }
  r += "…";
  out = mk_str(r);
  return N_Ok;
}
static NativeStatus f_table(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  ListObj* rows = (ListObj*)A(a, 0)->o;
  Vec<int> widths;
  for (int i = 0; i < rows->v.size(); i++) {
    ListObj* row = (ListObj*)rows->v[i].o;
    for (int k = 0; k < row->v.size(); k++) {
      int w = utf8_display_width(((StrObj*)row->v[k].o)->s);
      while (widths.size() <= k) widths.push(0);
      if (w > widths[k]) widths[k] = w;
    }
  }
  Str r;
  for (int i = 0; i < rows->v.size(); i++) {
    ListObj* row = (ListObj*)rows->v[i].o;
    for (int k = 0; k < row->v.size(); k++) {
      const Str& cell = ((StrObj*)row->v[k].o)->s;
      r += cell;
      if (k + 1 < row->v.size()) {
        int pad = widths[k] - utf8_display_width(cell) + 2;
        for (int p = 0; p < pad; p++) r += " ";
      }
    }
    r += "\n";
  }
  out = mk_str(r);
  return N_Ok;
}

void register_fmt(Registry& r) {
  TypeTable& t = r.types();
  Type* ts = t.t_string();
  Type* ti = t.t_int();
  Type* tf = t.t_float();
  r.add("fmt.apply", f_apply, ts, ti, ts);
  r.add("fmt.apply", f_apply, ts, tf, ts);
  r.add("fmt.apply", f_apply, ts, ts, ts);
  r.add("fmt.bytes", f_bytes, ts, ti);
  r.add("fmt.duration", f_duration, ts, t.simple(T_Duration));
  r.add("fmt.truncate", f_truncate, ts, ts, ti);
  r.add("fmt.table", f_table, ts, t.list_of(t.list_of(ts)));
  r.enable_module("std.fmt");
}

}  // namespace shark
