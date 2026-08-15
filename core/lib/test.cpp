// test.cpp — std.test（spec/library/test.md）
//
// 失敗しても panic はせず、そのテストだけを失敗として記録して次へ進む。
#include "../registry.h"
#include "../value.h"
#include "../vm.h"

namespace shark {

static bool g_failed = false;
static Str g_message;
static Str g_desc;
static int g_before = -1;
static int g_after = -1;

void test_begin() {
  g_failed = false;
  g_message.clear();
  g_desc.clear();
}
bool test_failed() { return g_failed; }
const Str& test_message() { return g_message; }
const Str& test_desc() { return g_desc; }
int test_before_index() { return g_before; }
int test_after_index() { return g_after; }

static Value* A(Value* args, int i) { return val_deref(&args[i]); }
static const Str& S(Value* args, int i) { return ((StrObj*)A(args, i)->o)->s; }

static void fail(VM& vm, const Str& msg) {
  if (g_failed) return;
  g_failed = true;
  g_message = msg;
  Str tr = vm.build_trace();
  if (tr.size()) {
    // 一番内側の1行だけを添える
    int end = 0;
    while (end < tr.size() && tr[end] != '\n') end++;
    g_message += "\n        ";
    g_message += tr.sub(4, end - 4);
  }
}

static NativeStatus t_ok(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!(A(a, 0)->k == V_Bool && A(a, 0)->b)) fail(vm, Str("true のはずが false でした"));
  out = mk_void();
  return N_Ok;
}
static NativeStatus t_eq(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!val_equal(*A(a, 0), *A(a, 1)))
    fail(vm, Str("expected ") + val_to_display(*A(a, 1)) + ", actual " + val_to_display(*A(a, 0)));
  out = mk_void();
  return N_Ok;
}
static NativeStatus t_ne(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (val_equal(*A(a, 0), *A(a, 1)))
    fail(vm, Str("違う値のはずが、どちらも ") + val_to_display(*A(a, 0)) + " でした");
  out = mk_void();
  return N_Ok;
}
static NativeStatus t_near(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  double x = A(a, 0)->f, y = A(a, 1)->f, tol = A(a, 2)->f;
  double d = x - y;
  if (d < 0) d = -d;
  if (!(d <= tol))
    fail(vm, Str("expected ") + str_from_float(y) + " ± " + str_from_float(tol) + ", actual " +
                  str_from_float(x));
  out = mk_void();
  return N_Ok;
}
static NativeStatus t_is_error(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Value* v = A(a, 0);
  bool is_err = v->k == V_Obj && v->o->kind == O_Result && !((ResultObj*)v->o)->ok;
  if (!is_err) fail(vm, Str("失敗のはずが、成功していました"));
  out = mk_void();
  return N_Ok;
}
static NativeStatus t_is_none(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (A(a, 0)->k != V_None) fail(vm, Str("none のはずが ") + val_to_display(*A(a, 0)) + " でした");
  out = mk_void();
  return N_Ok;
}
static NativeStatus t_fail(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  fail(vm, S(a, 0));
  out = mk_void();
  return N_Ok;
}
static NativeStatus t_describe(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  g_desc = S(a, 0);
  out = mk_void();
  return N_Ok;
}
static NativeStatus t_before(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Value* v = A(a, 0);
  if (v->k == V_Obj && v->o->kind == O_Func) g_before = ((FuncObj*)v->o)->fn;
  out = mk_void();
  return N_Ok;
}
static NativeStatus t_after(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Value* v = A(a, 0);
  if (v->k == V_Obj && v->o->kind == O_Func) g_after = ((FuncObj*)v->o)->fn;
  out = mk_void();
  return N_Ok;
}

void register_test(Registry& r) {
  TypeTable& t = r.types();
  Type* tv = t.t_void();
  Type* tb = t.t_bool();
  Type* tf = t.t_float();
  Type* ts = t.t_string();
  Type* any = t.t_unknown();
  Vec<Type*> noparams;
  r.add("test.ok", t_ok, tv, tb);
  r.add("test.eq", t_eq, tv, any, any);
  r.add("test.ne", t_ne, tv, any, any);
  r.add("test.near", t_near, tv, tf, tf, tf);
  r.add("test.is_error", t_is_error, tv, any);
  r.add("test.is_none", t_is_none, tv, any);
  r.add("test.fail", t_fail, tv, ts);
  r.add("test.describe", t_describe, tv, ts);
  r.add("test.before_each", t_before, tv, t.func_type(noparams, tv));
  r.add("test.after_each", t_after, tv, t.func_type(noparams, tv));
  r.enable_module("std.test");
}

}  // namespace shark
