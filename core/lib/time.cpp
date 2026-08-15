// time.cpp — std.time（spec/library/time.md）
#include "../platform/platform.h"
#include "../registry.h"
#include "../value.h"
#include "../vm.h"

namespace shark {

static Value* A(Value* args, int i) { return val_deref(&args[i]); }
static const Str& S(Value* args, int i) { return ((StrObj*)A(args, i)->o)->s; }

// 暦の計算（うるう年を含む。環境に依存しないよう自前で持つ）
static int64_t days_from_civil(int64_t y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
static void civil_from_days(int64_t z, int64_t* y, int* m, int* d) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t yy = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  int64_t dd = doy - (153 * mp + 2) / 5 + 1;
  int64_t mm = mp + (mp < 10 ? 3 : -9);
  *y = yy + (mm <= 2);
  *m = (int)mm;
  *d = (int)dd;
}

struct Parts { int64_t y; int mo, d, h, mi, s, ms, wd; };

static void split_time(int64_t ns, Parts* p) {
  int64_t total = ns / 1000000000ll;
  int64_t rem = ns % 1000000000ll;
  if (rem < 0) { rem += 1000000000ll; total -= 1; }
  int64_t days = total / 86400;
  int64_t secs = total % 86400;
  if (secs < 0) { secs += 86400; days -= 1; }
  civil_from_days(days, &p->y, &p->mo, &p->d);
  p->h = (int)(secs / 3600);
  p->mi = (int)((secs % 3600) / 60);
  p->s = (int)(secs % 60);
  p->ms = (int)(rem / 1000000);
  int64_t wd = (days + 4) % 7;   // 1970-01-01 は木曜
  if (wd < 0) wd += 7;
  p->wd = (int)wd;
}

static NativeStatus t_now(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_time(platform().now_unix_nanos());
  return N_Ok;
}
static NativeStatus t_monotonic(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  out = mk_dur(platform().monotonic_nanos() - vm.started_at);
  return N_Ok;
}
static bool valid_date(int64_t y, int mo, int d) {
  if (mo < 1 || mo > 12 || d < 1) return false;
  static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int max = dim[mo - 1];
  if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) max = 29;
  return d <= max;
}
static NativeStatus t_date(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  int64_t y = A(a, 0)->i;
  int mo = (int)A(a, 1)->i, d = (int)A(a, 2)->i;
  if (!valid_date(y, mo, d)) { out = mk_none(); return N_Ok; }
  out = mk_time(days_from_civil(y, mo, d) * 86400ll * 1000000000ll);
  return N_Ok;
}
static NativeStatus t_datetime(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  int64_t y = A(a, 0)->i;
  int mo = (int)A(a, 1)->i, d = (int)A(a, 2)->i;
  int h = (int)A(a, 3)->i, mi = (int)A(a, 4)->i, s = (int)A(a, 5)->i;
  if (!valid_date(y, mo, d) || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) {
    out = mk_none();
    return N_Ok;
  }
  int64_t days = days_from_civil(y, mo, d);
  out = mk_time(((days * 86400ll) + h * 3600 + mi * 60 + s) * 1000000000ll);
  return N_Ok;
}
// "YYYY-MM-DD hh:mm:ss" のような書式で読む
static NativeStatus t_parse(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& src = S(a, 0);
  const Str& f = S(a, 1);
  int64_t y = 1970;
  int mo = 1, d = 1, h = 0, mi = 0, s = 0;
  int si = 0;
  bool ok = true;
  for (int i = 0; i < f.size() && ok;) {
    auto num = [&](int digits, int* dst) {
      int v = 0, got = 0;
      while (got < digits && si < src.size() && src[si] >= '0' && src[si] <= '9') {
        v = v * 10 + (src[si] - '0');
        si++;
        got++;
      }
      if (got == 0) ok = false;
      *dst = v;
    };
    if (i + 3 < f.size() && f.sub(i, 4) == "YYYY") { int v; num(4, &v); y = v; i += 4; continue; }
    if (i + 2 < f.size() && f.sub(i, 3) == "SSS") { int v; num(3, &v); (void)v; i += 3; continue; }
    if (i + 1 < f.size()) {
      Str two = f.sub(i, 2);
      if (two == "MM") { num(2, &mo); i += 2; continue; }
      if (two == "DD") { num(2, &d); i += 2; continue; }
      if (two == "hh") { num(2, &h); i += 2; continue; }
      if (two == "mm") { num(2, &mi); i += 2; continue; }
      if (two == "ss") { num(2, &s); i += 2; continue; }
    }
    if (si < src.size() && src[si] == f[i]) { si++; i++; continue; }
    ok = false;
  }
  if (!ok || !valid_date(y, mo, d)) { out = mk_none(); return N_Ok; }
  int64_t days = days_from_civil(y, mo, d);
  out = mk_time(((days * 86400ll) + h * 3600 + mi * 60 + s) * 1000000000ll);
  return N_Ok;
}

static NativeStatus d_seconds(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_dur((int64_t)(A(a, 0)->f * 1e9)); return N_Ok; }
static NativeStatus d_minutes(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_dur((int64_t)(A(a, 0)->f * 60e9)); return N_Ok; }
static NativeStatus d_hours(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_dur((int64_t)(A(a, 0)->f * 3600e9)); return N_Ok; }
static NativeStatus d_days(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_dur((int64_t)(A(a, 0)->f * 86400e9)); return N_Ok; }

static NativeStatus dm_seconds(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_float((double)((DurObj*)A(a, 0)->o)->ns / 1e9); return N_Ok; }
static NativeStatus dm_minutes(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_float((double)((DurObj*)A(a, 0)->o)->ns / 60e9); return N_Ok; }
static NativeStatus dm_hours(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_float((double)((DurObj*)A(a, 0)->o)->ns / 3600e9); return N_Ok; }
static NativeStatus dm_days(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_float((double)((DurObj*)A(a, 0)->o)->ns / 86400e9); return N_Ok; }

static void pad2(Str& out, int v) {
  if (v < 10) out += "0";
  out += str_from_int(v);
}
static NativeStatus tm_format(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Parts p;
  split_time(((TimeObj*)A(a, 0)->o)->unix_ns, &p);
  const Str& f = S(a, 1);
  Str r;
  for (int i = 0; i < f.size();) {
    if (i + 3 < f.size() && f.sub(i, 4) == "YYYY") {
      Str y = str_from_int(p.y);
      while (y.size() < 4) y = Str("0") + y;
      r += y;
      i += 4;
      continue;
    }
    if (i + 2 < f.size() && f.sub(i, 3) == "SSS") {
      if (p.ms < 100) r += "0";
      if (p.ms < 10) r += "0";
      r += str_from_int(p.ms);
      i += 3;
      continue;
    }
    if (i + 1 < f.size()) {
      Str two = f.sub(i, 2);
      if (two == "MM") { pad2(r, p.mo); i += 2; continue; }
      if (two == "DD") { pad2(r, p.d); i += 2; continue; }
      if (two == "hh") { pad2(r, p.h); i += 2; continue; }
      if (two == "mm") { pad2(r, p.mi); i += 2; continue; }
      if (two == "ss") { pad2(r, p.s); i += 2; continue; }
    }
    r.push(f[i]);
    i++;
  }
  out = mk_str(r);
  return N_Ok;
}
#define TM_PART(name, field)                                                   \
  static NativeStatus name(VM& vm, Value* a, int n, Value& out) {              \
    (void)vm; (void)n;                                                         \
    Parts p;                                                                   \
    split_time(((TimeObj*)A(a, 0)->o)->unix_ns, &p);                           \
    out = mk_int(p.field);                                                     \
    return N_Ok;                                                               \
  }
TM_PART(tm_year, y)
TM_PART(tm_month, mo)
TM_PART(tm_day, d)
TM_PART(tm_hour, h)
TM_PART(tm_minute, mi)
TM_PART(tm_second, s)
TM_PART(tm_weekday, wd)

static NativeStatus tm_to_local(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  int64_t ns = ((TimeObj*)A(a, 0)->o)->unix_ns;
  out = mk_time(ns + (int64_t)platform().local_offset_seconds(ns) * 1000000000ll);
  return N_Ok;
}
static NativeStatus tm_to_utc(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  int64_t ns = ((TimeObj*)A(a, 0)->o)->unix_ns;
  out = mk_time(ns - (int64_t)platform().local_offset_seconds(ns) * 1000000000ll);
  return N_Ok;
}

void register_time(Registry& r) {
  TypeTable& t = r.types();
  Type* ti = t.t_int();
  Type* tf = t.t_float();
  Type* ts = t.t_string();
  Type* tt = t.simple(T_Time);
  Type* td = t.simple(T_Duration);
  r.add("time.now", t_now, tt);
  r.add("time.monotonic", t_monotonic, td);
  r.add("time.date", t_date, t.optional_of(tt), ti, ti, ti);
  {
    NativeEntry* e;
    int id = r.add("time.datetime", t_datetime, t.optional_of(tt), ti, ti, ti);
    e = &r.at(id);
    e->params.push(ti);
    e->params.push(ti);
    e->params.push(ti);
  }
  r.add("time.parse", t_parse, t.optional_of(tt), ts, ts);
  r.add("time.seconds", d_seconds, td, tf);
  r.add("time.minutes", d_minutes, td, tf);
  r.add("time.hours", d_hours, td, tf);
  r.add("time.days", d_days, td, tf);

  r.add_untyped("time.Time.format", tm_format);
  r.add_untyped("time.Time.year", tm_year);
  r.add_untyped("time.Time.month", tm_month);
  r.add_untyped("time.Time.day", tm_day);
  r.add_untyped("time.Time.hour", tm_hour);
  r.add_untyped("time.Time.minute", tm_minute);
  r.add_untyped("time.Time.second", tm_second);
  r.add_untyped("time.Time.weekday", tm_weekday);
  r.add_untyped("time.Time.to_local", tm_to_local);
  r.add_untyped("time.Time.to_utc", tm_to_utc);
  r.add_untyped("time.Duration.seconds", dm_seconds);
  r.add_untyped("time.Duration.minutes", dm_minutes);
  r.add_untyped("time.Duration.hours", dm_hours);
  r.add_untyped("time.Duration.days", dm_days);
  r.enable_module("std.time");
}

}  // namespace shark
