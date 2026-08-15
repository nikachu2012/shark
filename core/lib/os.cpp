// os.cpp — std.os（spec/library/os.md）
#include "../platform/platform.h"
#include "../registry.h"
#include "../value.h"
#include "../vm.h"

namespace shark {

static Value* A(Value* args, int i) { return val_deref(&args[i]); }
static const Str& S(Value* args, int i) { return ((StrObj*)A(args, i)->o)->s; }

static Vec<Str> g_args;
void os_set_args(const Vec<Str>& args) { g_args = args; }

static NativeStatus o_args(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_list();
  for (int i = 0; i < g_args.size(); i++) as_list(out)->v.push(mk_str(g_args[i]));
  return N_Ok;
}
static NativeStatus o_env(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Str v;
  if (platform().os && platform().os->env(S(a, 0).c_str(), &v)) out = mk_str(v);
  else out = mk_none();
  return N_Ok;
}
static NativeStatus o_set_env(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  if (platform().os) platform().os->set_env(S(a, 0).c_str(), S(a, 1).c_str());
  out = mk_void();
  return N_Ok;
}
static NativeStatus o_platform(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_str(platform().os ? platform().os->name() : "embedded");
  return N_Ok;
}
static NativeStatus o_cwd(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  Str p;
  if (platform().os && platform().os->cwd(&p)) out = mk_result_ok(mk_str(p));
  else out = mk_result_err(vm.make_error(Str("いまいる場所を取れません"), 0));
  return N_Ok;
}
static NativeStatus o_chdir(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (platform().os && platform().os->chdir(S(a, 0).c_str())) out = mk_result_ok(mk_void());
  else out = mk_result_err(vm.make_error(Str("移動できません: ") + S(a, 0), 0));
  return N_Ok;
}
static NativeStatus o_temp_dir(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_str(platform().os ? platform().os->temp_dir() : ".");
  return N_Ok;
}
static NativeStatus o_exit(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  vm.exit_code = (int)A(a, 0)->i;
  vm.status = SK_Finished;
  out = mk_void();
  return N_Ok;
}
static NativeStatus o_run(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!platform().os || !platform().os->run) {
    out = mk_result_err(vm.make_error(Str("この処理系は外のプログラムを呼べません"), 0));
    return N_Ok;
  }
  ListObj* l = (ListObj*)A(a, 1)->o;
  Vec<Str> args;
  for (int i = 0; i < l->v.size(); i++) args.push(((StrObj*)l->v[i].o)->s);
  int code = 0;
  Str so, se;
  if (!platform().os->run(S(a, 0).c_str(), args, &code, &so, &se)) {
    out = mk_result_err(vm.make_error(se.size() ? se : Str("実行できません"), 0));
    return N_Ok;
  }
  // Output は list<string> で持つ（0: 出力, 1: エラー出力, 2: 終了コード）
  Value o = mk_list();
  as_list(o)->v.push(mk_str(so));
  as_list(o)->v.push(mk_str(se));
  as_list(o)->v.push(mk_int(code));
  out = mk_result_ok(o);
  val_release(o);
  return N_Ok;
}
static NativeStatus out_code(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = val_retain(((ListObj*)A(a, 0)->o)->v[2]);
  return N_Ok;
}
static NativeStatus out_out(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = val_retain(((ListObj*)A(a, 0)->o)->v[0]);
  return N_Ok;
}
static NativeStatus out_err(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = val_retain(((ListObj*)A(a, 0)->o)->v[1]);
  return N_Ok;
}

void register_os(Registry& r) {
  TypeTable& t = r.types();
  Type* ts = t.t_string();
  Type* ti = t.t_int();
  Type* tv = t.t_void();
  Type* to = t.simple(T_Output);
  r.add("os.args", o_args, t.list_of(ts));
  r.add("os.env", o_env, t.optional_of(ts), ts);
  r.add("os.set_env", o_set_env, tv, ts, ts);
  r.add("os.platform", o_platform, ts);
  r.add("os.cwd", o_cwd, t.result_of(ts));
  r.add("os.chdir", o_chdir, t.result_of(tv), ts);
  r.add("os.temp_dir", o_temp_dir, ts);
  r.add("os.exit", o_exit, tv, ti);
  r.add("os.run", o_run, t.result_of(to), ts, t.list_of(ts));
  r.add_untyped("os.Output.code", out_code);
  r.add_untyped("os.Output.out", out_out);
  r.add_untyped("os.Output.err", out_err);
  r.enable_module("std.os");
}

}  // namespace shark
