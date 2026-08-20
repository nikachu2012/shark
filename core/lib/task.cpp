// task.cpp — std.task（spec/library/task.md、spec/runtime/concurrency.md）
//
// タスクの切り替えは仮想マシンの中で行う。OS のスレッドは使わない。
// 待つ関数は「まだ終わらない（N_Wait）」を返し、次の刻みで呼び直される。
#include "../platform/platform.h"
#include "../registry.h"
#include "../value.h"
#include "../vm.h"

namespace shark {

static Value* A(Value* args, int i) { return val_deref(&args[i]); }

// ------------------------------------------------------------------ channel
static NativeStatus ch_new(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  ChanObj* o = new (sk_alloc(sizeof(ChanObj))) ChanObj();
  o->c = new (sk_alloc(sizeof(ChannelState))) ChannelState();
  o->c->cap = 1;
  out = mk_obj_value(o);
  return N_Ok;
}
static NativeStatus ch_new_cap(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  ChanObj* o = new (sk_alloc(sizeof(ChanObj))) ChanObj();
  o->c = new (sk_alloc(sizeof(ChannelState))) ChannelState();
  int64_t cap = A(a, 0)->i;
  o->c->cap = cap < 1 ? 1 : (int)cap;
  out = mk_obj_value(o);
  return N_Ok;
}
static NativeStatus ch_send(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  ChannelState* c = ((ChanObj*)A(a, 0)->o)->c;
  TaskState* t = vm.task();
  if (c->closed) {
    t->wait_state = 0;
    out = mk_result_err(vm.make_error(Str("閉じたチャネルには送れません"), 0));
    return N_Ok;
  }
  if (t->cancel_req) return N_Cancel;
  if (c->q.size() >= c->cap) return N_Wait;   // 空くまで待つ
  c->q.push(val_retain(*A(a, 1)));
  out = mk_result_ok(mk_void());
  return N_Ok;
}
static NativeStatus ch_recv(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  ChannelState* c = ((ChanObj*)A(a, 0)->o)->c;
  TaskState* t = vm.task();
  if (c->q.size() > 0) {
    out = c->q[0];
    c->q.remove(0);
    return N_Ok;
  }
  if (c->closed) { out = mk_none(); return N_Ok; }
  if (t->cancel_req) return N_Cancel;
  return N_Wait;   // 届くまで待つ
}
static NativeStatus ch_try_recv(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  ChannelState* c = ((ChanObj*)A(a, 0)->o)->c;
  if (c->q.size() > 0) {
    out = c->q[0];
    c->q.remove(0);
    return N_Ok;
  }
  out = mk_none();
  return N_Ok;
}
static NativeStatus ch_close(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  ((ChanObj*)A(a, 0)->o)->c->closed = true;
  out = mk_void();
  return N_Ok;
}
static NativeStatus ch_len(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_int(((ChanObj*)A(a, 0)->o)->c->q.size());
  return N_Ok;
}

// ------------------------------------------------------------------ Task
static NativeStatus tk_wait(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  TaskState* target = ((TaskObj*)A(a, 0)->o)->t;
  TaskState* self = vm.task();
  if (target->status == TS_Ready || target->status == TS_Waiting) {
    if (self->cancel_req) return N_Cancel;
    return N_Wait;
  }
  if (target->status == TS_Panicked) {
    vm.panic(vm.L(Str("待っていたタスクが止まりました: ") + target->panic_msg,
                  Str("the task being waited on stopped: ") + target->panic_msg));
    return N_Panic;
  }
  if (target->status == TS_Cancelled) {
    vm.panic(vm.L("取り消したタスクの結果は受け取れません（wait_timeout なら none が返ります）",
                  "a cancelled task has no result (wait_timeout returns none instead)"));
    return N_Panic;
  }
  out = val_retain(target->result);
  return N_Ok;
}
static NativeStatus tk_wait_timeout(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  TaskState* target = ((TaskObj*)A(a, 0)->o)->t;
  TaskState* self = vm.task();
  if (self->wait_state == 0) {
    int64_t d = ((DurObj*)A(a, 1)->o)->ns;
    self->wait_state = platform().monotonic_nanos() + (d > 0 ? d : 0);
  }
  if (target->status == TS_Ready || target->status == TS_Waiting) {
    if (platform().monotonic_nanos() >= self->wait_state) {
      self->wait_state = 0;
      out = mk_none();
      return N_Ok;
    }
    if (self->cancel_req) { self->wait_state = 0; return N_Cancel; }
    return N_Wait;
  }
  self->wait_state = 0;
  if (target->status == TS_Panicked || target->status == TS_Cancelled) { out = mk_none(); return N_Ok; }
  out = val_retain(target->result);
  return N_Ok;
}
static NativeStatus tk_done(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  TaskState* t = ((TaskObj*)A(a, 0)->o)->t;
  out = mk_bool(!(t->status == TS_Ready || t->status == TS_Waiting));
  return N_Ok;
}
static NativeStatus tk_cancel(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  ((TaskObj*)A(a, 0)->o)->t->cancel_req = true;   // 要求を立てるだけ
  out = mk_void();
  return N_Ok;
}
static NativeStatus tk_id(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_int(((TaskObj*)A(a, 0)->o)->t->id);
  return N_Ok;
}

// ------------------------------------------------------------------ そのほか
static NativeStatus tk_yield(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  TaskState* t = vm.task();
  if (t->wait_state == 0) { t->wait_state = 1; return N_Wait; }
  t->wait_state = 0;
  out = mk_void();
  return N_Ok;
}
static NativeStatus tk_count(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  int c = 0;
  for (int i = 0; i < vm.tasks.size(); i++)
    if (vm.tasks[i]->status == TS_Ready || vm.tasks[i]->status == TS_Waiting) c++;
  out = mk_int(c);
  return N_Ok;
}

void register_task(Registry& r) {
  TypeTable& t = r.types();
  Type* ti = t.t_int();
  Type* tv = t.t_void();
  r.add_untyped("task.channel", ch_new);
  r.add_untyped("task.channel_cap", ch_new_cap);
  r.add_untyped("task.channel.send", ch_send);
  r.add_untyped("task.channel.recv", ch_recv);
  r.add_untyped("task.channel.try_recv", ch_try_recv);
  r.add_untyped("task.channel.close", ch_close);
  r.add_untyped("task.channel.len", ch_len);
  r.add_untyped("task.Task.wait", tk_wait);
  r.add_untyped("task.Task.wait_timeout", tk_wait_timeout);
  r.add_untyped("task.Task.done", tk_done);
  r.add_untyped("task.Task.cancel", tk_cancel);
  r.add_untyped("task.Task.id", tk_id);
  r.add("task.yield", tk_yield, tv);
  r.add("task.count", tk_count, ti);
  r.enable_module("std.task");
}

}  // namespace shark
