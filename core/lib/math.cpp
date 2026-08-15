// math.cpp — std.math（spec/library/math.md）
#include <math.h>

#include "../registry.h"
#include "../value.h"
#include "../vm.h"

namespace shark {

static Value* A(Value* args, int i) { return val_deref(&args[i]); }

#define M_CONST(name, expr)                                                    \
  static NativeStatus name(VM& vm, Value* a, int n, Value& out) {              \
    (void)vm; (void)a; (void)n;                                                \
    out = mk_float(expr);                                                      \
    return N_Ok;                                                               \
  }
M_CONST(m_pi, 3.14159265358979323846)
M_CONST(m_e, 2.71828182845904523536)
M_CONST(m_inf, HUGE_VAL)

#define M_F1(name, expr)                                                       \
  static NativeStatus name(VM& vm, Value* a, int n, Value& out) {              \
    (void)vm; (void)n;                                                         \
    double x = A(a, 0)->f;                                                     \
    out = mk_float(expr);                                                      \
    return N_Ok;                                                               \
  }
M_F1(m_sqrt, sqrt(x))
M_F1(m_exp, exp(x))
M_F1(m_log, log(x))
M_F1(m_log10, log10(x))
M_F1(m_sin, sin(x))
M_F1(m_cos, cos(x))
M_F1(m_tan, tan(x))
M_F1(m_asin, asin(x))
M_F1(m_acos, acos(x))
M_F1(m_atan, atan(x))

static NativeStatus m_pow(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_float(pow(A(a, 0)->f, A(a, 1)->f));
  return N_Ok;
}
static NativeStatus m_atan2(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_float(atan2(A(a, 0)->f, A(a, 1)->f));
  return N_Ok;
}
static NativeStatus m_abs_i(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int64_t x = A(a, 0)->i;
  if (x == (-9223372036854775807LL - 1)) { vm.panic(Str("int の計算があふれました")); return N_Panic; }
  out = mk_int(x < 0 ? -x : x);
  return N_Ok;
}
static NativeStatus m_abs_f(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; double x = A(a, 0)->f; out = mk_float(x < 0 ? -x : x); return N_Ok; }
static NativeStatus m_min_i(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; int64_t x = A(a, 0)->i, y = A(a, 1)->i; out = mk_int(x < y ? x : y); return N_Ok; }
static NativeStatus m_min_f(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; double x = A(a, 0)->f, y = A(a, 1)->f; out = mk_float(x < y ? x : y); return N_Ok; }
static NativeStatus m_max_i(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; int64_t x = A(a, 0)->i, y = A(a, 1)->i; out = mk_int(x > y ? x : y); return N_Ok; }
static NativeStatus m_max_f(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; double x = A(a, 0)->f, y = A(a, 1)->f; out = mk_float(x > y ? x : y); return N_Ok; }
static NativeStatus m_clamp_i(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  int64_t x = A(a, 0)->i, lo = A(a, 1)->i, hi = A(a, 2)->i;
  out = mk_int(x < lo ? lo : (x > hi ? hi : x));
  return N_Ok;
}
static NativeStatus m_clamp_f(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  double x = A(a, 0)->f, lo = A(a, 1)->f, hi = A(a, 2)->f;
  out = mk_float(x < lo ? lo : (x > hi ? hi : x));
  return N_Ok;
}
static NativeStatus m_floor(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_int((int64_t)floor(A(a, 0)->f)); return N_Ok; }
static NativeStatus m_ceil(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_int((int64_t)ceil(A(a, 0)->f)); return N_Ok; }
static NativeStatus m_trunc(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; out = mk_int((int64_t)A(a, 0)->f); return N_Ok; }
static NativeStatus m_round(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  double x = A(a, 0)->f;
  out = mk_int((int64_t)(x < 0 ? -floor(-x + 0.5) : floor(x + 0.5)));
  return N_Ok;
}
static NativeStatus m_is_nan(VM& vm, Value* a, int n, Value& out) { (void)vm; (void)n; double x = A(a, 0)->f; out = mk_bool(x != x); return N_Ok; }
static NativeStatus m_is_inf(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  double x = A(a, 0)->f;
  out = mk_bool(x > 1.7976931348623157e308 || x < -1.7976931348623157e308);
  return N_Ok;
}

// 乱数は処理系に埋め込む。同じ種なら、どの環境でも同じ並びになる
static uint64_t next_rand(VM& vm) {
  vm.rng_state ^= vm.rng_state << 13;
  vm.rng_state ^= vm.rng_state >> 7;
  vm.rng_state ^= vm.rng_state << 17;
  return vm.rng_state;
}
static NativeStatus m_random(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  uint64_t r = next_rand(vm) >> 11;  // 53 ビット
  out = mk_float((double)r / 9007199254740992.0);
  return N_Ok;
}
static NativeStatus m_random_int(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int64_t lo = A(a, 0)->i, hi = A(a, 1)->i;
  if (hi < lo) { vm.panic(Str("random_int の範囲が逆です")); return N_Panic; }
  uint64_t span = (uint64_t)(hi - lo) + 1;
  out = mk_int(lo + (int64_t)(next_rand(vm) % span));
  return N_Ok;
}
static NativeStatus m_seed(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  uint64_t s = (uint64_t)A(a, 0)->i;
  vm.rng_state = s ? s : 0x9e3779b97f4a7c15ull;
  for (int i = 0; i < 8; i++) next_rand(vm);
  out = mk_void();
  return N_Ok;
}

void register_math(Registry& r) {
  TypeTable& t = r.types();
  Type* ti = t.t_int();
  Type* tf = t.t_float();
  Type* tb = t.t_bool();
  Type* tv = t.t_void();
  r.add("math.PI", m_pi, tf);
  r.add("math.E", m_e, tf);
  r.add("math.INF", m_inf, tf);
  r.add("math.abs", m_abs_i, ti, ti);
  r.add("math.abs", m_abs_f, tf, tf);
  r.add("math.min", m_min_i, ti, ti, ti);
  r.add("math.min", m_min_f, tf, tf, tf);
  r.add("math.max", m_max_i, ti, ti, ti);
  r.add("math.max", m_max_f, tf, tf, tf);
  r.add("math.clamp", m_clamp_i, ti, ti, ti, ti);
  r.add("math.clamp", m_clamp_f, tf, tf, tf, tf);
  r.add("math.sqrt", m_sqrt, tf, tf);
  r.add("math.pow", m_pow, tf, tf, tf);
  r.add("math.exp", m_exp, tf, tf);
  r.add("math.log", m_log, tf, tf);
  r.add("math.log10", m_log10, tf, tf);
  r.add("math.sin", m_sin, tf, tf);
  r.add("math.cos", m_cos, tf, tf);
  r.add("math.tan", m_tan, tf, tf);
  r.add("math.asin", m_asin, tf, tf);
  r.add("math.acos", m_acos, tf, tf);
  r.add("math.atan", m_atan, tf, tf);
  r.add("math.atan2", m_atan2, tf, tf, tf);
  r.add("math.floor", m_floor, ti, tf);
  r.add("math.ceil", m_ceil, ti, tf);
  r.add("math.round", m_round, ti, tf);
  r.add("math.trunc", m_trunc, ti, tf);
  r.add("math.is_nan", m_is_nan, tb, tf);
  r.add("math.is_inf", m_is_inf, tb, tf);
  r.add("math.random", m_random, tf);
  r.add("math.random_int", m_random_int, ti, ti, ti);
  r.add("math.seed", m_seed, tv, ti);
  r.enable_module("std.math");
}

}  // namespace shark
