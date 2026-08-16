#include "vm.h"

#include "opcodes.h"
#include "platform/platform.h"

namespace shark {

// ------------------------------------------------------------------ タスク
TaskState* task_new(VM& vm, int stack_size) {
  TaskState* t = new (sk_alloc(sizeof(TaskState))) TaskState();
  t->cap = stack_size;
  t->stack = (Value*)sk_alloc(sizeof(Value) * (size_t)stack_size);
  for (int i = 0; i < stack_size; i++) new (&t->stack[i]) Value();
  t->id = vm.tasks.size() + 1;
  return t;
}

static void task_free(TaskState* t) {
  for (int i = 0; i < t->sp; i++) val_release(t->stack[i]);
  for (int i = 0; i < t->cap; i++) t->stack[i].~Value();
  sk_free(t->stack);
  val_release(t->result);
  t->~TaskState();
  sk_free(t);
}

void task_unref(TaskState* t) {
  if (!t) return;
  if (--t->refs <= 0) task_free(t);
}

void chan_unref(ChannelState* c) {
  if (!c) return;
  if (--c->refs <= 0) {
    for (int i = 0; i < c->q.size(); i++) val_release(c->q[i]);
    c->~ChannelState();
    sk_free(c);
  }
}

void task_obj_dispose(TaskObj* o) { task_unref(o->t); }
void chan_obj_dispose(ChanObj* o) { chan_unref(o->c); }

// ------------------------------------------------------------------ VM
VM::VM()
    : prog(0), reg(0), diag(0), cur(0), status(SK_Running), exit_code(0), error_line(0),
      stack_size(8192), started_at(0), aborted(false), rng_state(0x853c49e6748fea9bull),
      steps_since_switch(0), has_panic(false), idle_hint(false), boot_pos(0) {}

VM::~VM() {
  for (int i = 0; i < tasks.size(); i++) task_unref(tasks[i]);
  for (int i = 0; i < globals.size(); i++) val_release(globals[i]);
}

void VM::set_program(Program* p, Registry* r) {
  prog = p;
  reg = r;
}

Value VM::default_of(Type* t) {
  if (!t) return mk_void();
  switch (t->kind) {
    case T_Int: return mk_int(0);
    case T_Float: return mk_float(0.0);
    case T_Bool: return mk_bool(false);
    case T_String: return mk_str("");
    case T_Bytes: return mk_bytes(Str());
    case T_List: return mk_list();
    case T_Map: return mk_map();
    case T_Optional: return mk_none();
    case T_Class: {
      Value v = mk_inst(t->cls);
      InstObj* o = as_inst(v);
      for (int i = 0; i < t->cls->fields.size(); i++) o->fields.push(default_of(t->cls->fields[i].type));
      return v;
    }
    case T_Time: return mk_time(0);
    case T_Duration: return mk_dur(0);
    default: return mk_none();
  }
}

void VM::start() {
  // ここから先に確保するものは「実行中のプログラムのぶん」として数える
  MemRunScope run_scope;
  // 読み込み直しに備えて、前回のぶんを離してから始める
  for (int i = 0; i < globals.size(); i++) val_release(globals[i]);
  globals.clear();
  for (int i = 0; i < tasks.size(); i++) task_unref(tasks[i]);
  tasks.clear();
  for (int i = 0; i < prog->globals.size(); i++) globals.push(default_of(prog->globals[i]->type));
  boot.clear();
  for (int i = 0; i < prog->inits.size(); i++) boot.push(prog->inits[i]);
  if (prog->entry >= 0) boot.push(prog->entry);
  boot_pos = 0;
  TaskState* main_task = task_new(*this, stack_size);
  tasks.push(main_task);
  cur = 0;
  started_at = platform().monotonic_nanos();
  status = SK_Running;
  if (boot.size() == 0) { status = SK_Finished; return; }
  // 最初の関数を呼ぶ
  FuncInfo* f = prog->funcs[boot[boot_pos++]];
  Frame fr;
  fr.fn = f;
  fr.ip = 0;
  fr.base = 0;
  for (int i = 0; i < f->nlocals; i++) push(mk_void());
  main_task->frames.push(fr);
}

void VM::push(const Value& v) {
  TaskState* t = tasks[cur];
  if (t->sp >= t->cap) {
    panic(Str("呼び出しが深すぎます（スタックがいっぱいです）"));
    return;
  }
  t->stack[t->sp++] = v;
}

Value VM::pop() {
  TaskState* t = tasks[cur];
  if (t->sp <= 0) return mk_void();
  Value v = t->stack[--t->sp];
  t->stack[t->sp] = Value();
  return v;
}

void VM::write_out(const Str& s) {
  if (io.write_out) io.write_out(io.ud, s.data(), s.size());
}

bool VM::read_line(Str* out) {
  if (io.read_line) return io.read_line(io.ud, out);
  return false;
}

Value VM::make_error(const Str& msg, int code) {
  ClassInfo* ec = 0;
  for (int i = 0; i < prog->classes.size(); i++)
    if (prog->classes[i]->name == "Error" && prog->classes[i]->module == "std") ec = prog->classes[i];
  Value v = mk_inst(ec);
  InstObj* o = as_inst(v);
  o->fields.push(mk_str(msg));
  o->fields.push(mk_int(code));
  return v;
}

Str VM::build_trace() {
  TaskState* t = tasks[cur];
  Str r;
  int shown = 0;
  for (int i = t->frames.size() - 1; i >= 0; i--) {
    if (shown == 8 && i > 8) {
      r += "    ... （";
      r += str_from_int(i - 8);
      r += " 段省略）\n";
      i = 9;   // 一番外側の数段だけ出す
      continue;
    }
    shown++;
    Frame& f = t->frames[i];
    int line = 0;
    if (f.ip > 0 && f.ip - 1 < f.fn->lines.size()) line = (int)f.fn->lines[f.ip - 1];
    r += "    ";
    r += f.fn->file.size() ? f.fn->file : f.fn->module;
    r += ":";
    r += str_from_int(line);
    r += "  ";
    r += f.fn->name;
    if (f.fn->owner) { r += " ("; r += f.fn->owner->name; r += ")"; }
    r += "\n";
  }
  return r;
}

void VM::panic(const Str& msg) {
  has_panic = true;
  pending_panic = msg;
}

void VM::finish_task(TaskState* t, const Value& result) {
  val_release(t->result);
  t->result = result;
  t->status = TS_Done;
}

// 次に動かせるタスクを選ぶ
bool VM::switch_task() {
  int n = tasks.size();
  idle_hint = false;
  for (int k = 1; k <= n; k++) {
    int i = (cur + k) % n;
    TaskState* t = tasks[i];
    if (t->status == TS_Ready) { cur = i; return true; }
    if (t->status == TS_Waiting) {
      // 起床時刻が来ていれば動かせる
      if (t->wake_at != 0 && platform().monotonic_nanos() >= t->wake_at) {
        t->status = TS_Ready;
        cur = i;
        steps_since_switch = 0;   // 時計で起きたので、行き詰まりではない
        return true;
      }
      if (t->cancel_req) { t->status = TS_Ready; cur = i; return true; }
    }
  }
  // 動かせるものが無い
  bool any_timer = false, any_waiting = false;
  for (int i = 0; i < n; i++) {
    if (tasks[i]->status != TS_Waiting) continue;
    any_waiting = true;
    if (tasks[i]->wake_at != 0) any_timer = true;
  }
  (void)any_timer;
  if (any_waiting && any_timer) { idle_hint = true; return true; }
  if (any_waiting) {
    // 全部が待ちで、目覚める見込みが無い
    cur = 0;
    for (int i = 0; i < n; i++) if (tasks[i]->status == TS_Waiting) { cur = i; break; }
    status = SK_Error;
    error_message = Str("すべてのタスクが待ったままになりました（届く見込みのない受け取り待ちです）");
    error_trace = build_trace();
    return false;
  }
  return false;
}

// ------------------------------------------------------------------ 命令の実行
static Value* deref(Value* v) {
  while (v->k == V_Ref) v = v->r;
  return v;
}

static bool add_ovf(int64_t a, int64_t b, int64_t* r) {
  uint64_t u = (uint64_t)a + (uint64_t)b;
  *r = (int64_t)u;
  return ((a > 0 && b > 0 && *r < 0) || (a < 0 && b < 0 && *r >= 0));
}
static bool sub_ovf(int64_t a, int64_t b, int64_t* r) {
  uint64_t u = (uint64_t)a - (uint64_t)b;
  *r = (int64_t)u;
  return ((a >= 0 && b < 0 && *r < 0) || (a < 0 && b > 0 && *r >= 0));
}
static bool mul_ovf(int64_t a, int64_t b, int64_t* r) {
  if (a == 0 || b == 0) { *r = 0; return false; }
  int64_t p = (int64_t)((uint64_t)a * (uint64_t)b);
  *r = p;
  if (a == -1 && b == (-9223372036854775807LL - 1)) return true;
  if (b == -1 && a == (-9223372036854775807LL - 1)) return true;
  return p / b != a;
}

bool VM::call_function(int fidx, int nargs) {
  FuncInfo* f = prog->funcs[fidx];
  TaskState* t = tasks[cur];
  if (f->is_native) {
    Value* args = t->stack + (t->sp - nargs);
    Value out;
    NativeStatus st = f->native(*this, args, nargs, out);
    if (st == N_Panic) return false;
    for (int i = 0; i < nargs; i++) val_release(t->stack[t->sp - 1 - i]);
    t->sp -= nargs;
    push(out);
    return true;
  }
  Frame fr;
  fr.fn = f;
  fr.ip = 0;
  fr.base = t->sp - nargs;
  for (int i = nargs; i < f->nlocals; i++) push(mk_void());
  if (t->frames.size() > 512) {
    panic(Str("呼び出しが深すぎます（同じ関数を呼び続けていませんか）"));
    return false;
  }
  t->frames.push(fr);
  return true;
}

int VM::spawn_task(int fidx, int nargs, Value* args) {
  FuncInfo* f = prog->funcs[fidx];
  TaskState* nt = task_new(*this, stack_size < 4096 ? stack_size : 4096);
  for (int i = 0; i < nargs; i++) nt->stack[nt->sp++] = val_retain(args[i]);
  for (int i = nargs; i < f->nlocals; i++) nt->stack[nt->sp++] = mk_void();
  Frame fr;
  fr.fn = f;
  fr.ip = 0;
  fr.base = 0;
  nt->frames.push(fr);
  nt->status = TS_Ready;
  nt->refs = 1;   // VM が1つ持つ
  tasks.push(nt);
  return tasks.size() - 1;
}

#define RD_I32(ip)                                                                   \
  ((int)((uint32_t)code[(ip) - 4] | ((uint32_t)code[(ip) - 3] << 8) |                \
         ((uint32_t)code[(ip) - 2] << 16) | ((uint32_t)code[(ip) - 1] << 24)))

RunStatus VM::step(int budget) {
  if (status != SK_Running) return status;
  // 実行中に確保したものだけが、上限の対象になる
  MemRunScope run_scope;
  idle_hint = false;

  while (budget > 0) {
    if (aborted) {
      status = SK_Error;
      error_message = Str("実行を止めました");
      return status;
    }
    TaskState* t = tasks[cur];

    if (t->status != TS_Ready) {
      if (!switch_task()) return status;
      if (idle_hint) return SK_Running;
      continue;
    }

    // 関数を全部抜けたら、そのタスクは終わり
    if (t->frames.size() == 0) {
      Value res = t->sp > 0 ? pop() : mk_void();
      if (cur == 0 && boot_pos < boot.size()) {
        val_release(res);
        FuncInfo* f = prog->funcs[boot[boot_pos++]];
        Frame fr;
        fr.fn = f;
        fr.ip = 0;
        fr.base = t->sp;
        for (int i = 0; i < f->nlocals; i++) push(mk_void());
        t->frames.push(fr);
        continue;
      }
      if (cur == 0) exit_code = (res.k == V_Int) ? (int)res.i : 0;
      finish_task(t, res);
      // 走っているタスクが残っていれば、終わるまで待つ
      bool any = false;
      for (int i = 0; i < tasks.size(); i++)
        if (tasks[i]->status == TS_Ready || tasks[i]->status == TS_Waiting) any = true;
      if (!any) {
        status = SK_Finished;
        return status;
      }
      if (!switch_task()) return status;
      if (idle_hint) return SK_Running;
      continue;
    }

    Frame& fr0 = t->frames.back();
    FuncInfo* fn = fr0.fn;
    const uint8_t* code = fn->code.data();
    int ip = fr0.ip;
    int base = fr0.base;
    int op_start = ip;
    uint8_t op = code[ip++];
    budget--;

    switch (op) {
      case OP_NOP: break;
      case OP_CONST: { ip += 4; int k = RD_I32(ip); fr0.ip = ip; push(val_retain(fn->consts[k])); break; }
      case OP_NONE: fr0.ip = ip; push(mk_none()); break;
      case OP_VOID: fr0.ip = ip; push(mk_void()); break;
      case OP_TRUE: fr0.ip = ip; push(mk_bool(true)); break;
      case OP_FALSE: fr0.ip = ip; push(mk_bool(false)); break;
      case OP_POP: { fr0.ip = ip; Value v = pop(); val_release(v); break; }
      case OP_DUP: { fr0.ip = ip; Value v = t->stack[t->sp - 1]; push(val_retain(v)); break; }
      case OP_SWAP: {
        fr0.ip = ip;
        Value tmp = t->stack[t->sp - 1];
        t->stack[t->sp - 1] = t->stack[t->sp - 2];
        t->stack[t->sp - 2] = tmp;
        break;
      }
      case OP_ROT_UNDER: {
        ip += 4; int n = RD_I32(ip); fr0.ip = ip;
        Value top = t->stack[t->sp - 1];
        for (int i = 0; i < n; i++) t->stack[t->sp - 1 - i] = t->stack[t->sp - 2 - i];
        t->stack[t->sp - 1 - n] = top;
        break;
      }

      case OP_LOAD_LOCAL: {
        ip += 4; int i = RD_I32(ip); fr0.ip = ip;
        push(val_retain(*deref(&t->stack[base + i])));
        break;
      }
      case OP_STORE_LOCAL: {
        ip += 4; int i = RD_I32(ip); fr0.ip = ip;
        Value v = pop();
        Value* slot = deref(&t->stack[base + i]);
        val_release(*slot);
        *slot = v;
        break;
      }
      case OP_LOAD_GLOBAL: {
        ip += 4; int i = RD_I32(ip); fr0.ip = ip;
        push(val_retain(globals[i]));
        break;
      }
      case OP_STORE_GLOBAL: {
        ip += 4; int i = RD_I32(ip); fr0.ip = ip;
        Value v = pop();
        val_release(globals[i]);
        globals[i] = v;
        break;
      }
      case OP_LOAD_FIELD: {
        ip += 4; int i = RD_I32(ip); fr0.ip = ip;
        Value o = pop();
        Value* ov = deref(&o);
        if (ov->k != V_Obj || ov->o->kind != O_Inst) { val_release(o); push(mk_none()); break; }
        InstObj* inst = (InstObj*)ov->o;
        push(i < inst->fields.size() ? val_retain(inst->fields[i]) : mk_none());
        val_release(o);
        break;
      }
      case OP_REF_LOCAL: {
        ip += 4; int i = RD_I32(ip); fr0.ip = ip;
        Value v;
        v.k = V_Ref;
        v.r = deref(&t->stack[base + i]);
        push(v);
        break;
      }

      // --- 代入先 ---
      case OP_PLACE_LOCAL: {
        ip += 4; int i = RD_I32(ip); fr0.ip = ip;
        t->places.push(deref(&t->stack[base + i]));
        break;
      }
      case OP_PLACE_GLOBAL: {
        ip += 4; int i = RD_I32(ip); fr0.ip = ip;
        t->places.push(&globals[i]);
        break;
      }
      case OP_PLACE_FIELD: {
        ip += 4; int i = RD_I32(ip); fr0.ip = ip;
        Value* p = t->places.back();
        t->places.pop();
        Obj* o = obj_unique(*p);
        if (!o || o->kind != O_Inst) { t->places.push(p); break; }
        InstObj* inst = (InstObj*)o;
        t->places.push(&inst->fields[i]);
        break;
      }
      case OP_PLACE_INDEX: {
        fr0.ip = ip;
        Value idx = pop();
        Value* p = t->places.back();
        t->places.pop();
        Obj* o = obj_unique(*p);
        if (!o) { val_release(idx); t->places.push(p); break; }
        if (o->kind == O_List) {
          ListObj* l = (ListObj*)o;
          int64_t i = idx.i;
          if (idx.k != V_Int || i < 0 || i >= l->v.size()) {
            panic(Str("配列の長さは ") + str_from_int(l->v.size()) + " ですが、" +
                  str_from_int(idx.k == V_Int ? idx.i : 0) + " 番目に書こうとしました");
            val_release(idx);
            t->places.push(p);
            break;
          }
          t->places.push(&l->v[(int)i]);
        } else if (o->kind == O_Map) {
          MapObj* m = (MapObj*)o;
          Value* slot = map_find(m, idx);
          if (!slot) {
            map_set(m, idx, mk_void());
            slot = map_find(m, idx);
          }
          t->places.push(slot);
        } else {
          t->places.push(p);
        }
        val_release(idx);
        break;
      }
      case OP_PLACE_LOAD: {
        fr0.ip = ip;
        Value* p = t->places.back();
        t->places.pop();
        push(val_retain(*p));
        break;
      }
      case OP_PLACE_STORE: {
        fr0.ip = ip;
        Value v = pop();
        Value* p = t->places.back();
        t->places.pop();
        val_release(*p);
        *p = v;
        break;
      }
      case OP_PLACE_DUP: {
        fr0.ip = ip;
        t->places.push(t->places.back());
        break;
      }
      case OP_PLACE_REF: {
        fr0.ip = ip;
        Value v;
        v.k = V_Ref;
        v.r = t->places.back();
        t->places.pop();
        push(v);
        break;
      }

      // --- 計算 ---
      case OP_ADD_INT: case OP_SUB_INT: case OP_MUL_INT: case OP_DIV_INT: case OP_MOD_INT: {
        fr0.ip = ip;
        Value b = pop(), a = pop();
        int64_t r = 0;
        bool ovf = false;
        if (op == OP_ADD_INT) ovf = add_ovf(a.i, b.i, &r);
        else if (op == OP_SUB_INT) ovf = sub_ovf(a.i, b.i, &r);
        else if (op == OP_MUL_INT) ovf = mul_ovf(a.i, b.i, &r);
        else {
          if (b.i == 0) {
            panic(op == OP_DIV_INT ? Str("0 で割ろうとしました") : Str("0 で割った余りを求めようとしました"));
            break;
          }
          if (b.i == -1 && a.i == (-9223372036854775807LL - 1)) ovf = true;
          else r = (op == OP_DIV_INT) ? a.i / b.i : a.i % b.i;
        }
        if (ovf) {
          panic(Str("int の計算があふれました（int は 64 ビットです）"));
          break;
        }
        push(mk_int(r));
        break;
      }
      case OP_NEG_INT: {
        fr0.ip = ip;
        Value a = pop();
        if (a.i == (-9223372036854775807LL - 1)) { panic(Str("int の計算があふれました")); break; }
        push(mk_int(-a.i));
        break;
      }
      case OP_ADD_FLOAT: case OP_SUB_FLOAT: case OP_MUL_FLOAT: case OP_DIV_FLOAT: {
        fr0.ip = ip;
        Value b = pop(), a = pop();
        double r = op == OP_ADD_FLOAT ? a.f + b.f : op == OP_SUB_FLOAT ? a.f - b.f
                   : op == OP_MUL_FLOAT ? a.f * b.f : a.f / b.f;
        push(mk_float(r));
        break;
      }
      case OP_NEG_FLOAT: { fr0.ip = ip; Value a = pop(); push(mk_float(-a.f)); break; }
      case OP_CONCAT: {
        ip += 4; int n = RD_I32(ip); fr0.ip = ip;
        Str r;
        for (int i = n - 1; i >= 0; i--) {
          Value* v = &t->stack[t->sp - 1 - i];
          if (v->k == V_Obj && (v->o->kind == O_Str || v->o->kind == O_Bytes)) r += ((StrObj*)v->o)->s;
          else r += val_to_display(*v);
        }
        for (int i = 0; i < n; i++) { Value v = pop(); val_release(v); }
        push(mk_str(r));
        break;
      }
      case OP_ADD_TIME_DUR: case OP_SUB_TIME_DUR: {
        fr0.ip = ip;
        Value b = pop(), a = pop();
        int64_t ns = ((TimeObj*)a.o)->unix_ns;
        int64_t d = ((DurObj*)b.o)->ns;
        push(mk_time(op == OP_ADD_TIME_DUR ? ns + d : ns - d));
        val_release(a); val_release(b);
        break;
      }
      case OP_SUB_TIME_TIME: {
        fr0.ip = ip;
        Value b = pop(), a = pop();
        push(mk_dur(((TimeObj*)a.o)->unix_ns - ((TimeObj*)b.o)->unix_ns));
        val_release(a); val_release(b);
        break;
      }
      case OP_ADD_DUR_DUR: case OP_SUB_DUR_DUR: {
        fr0.ip = ip;
        Value b = pop(), a = pop();
        int64_t x = ((DurObj*)a.o)->ns, y = ((DurObj*)b.o)->ns;
        push(mk_dur(op == OP_ADD_DUR_DUR ? x + y : x - y));
        val_release(a); val_release(b);
        break;
      }
      case OP_NOT: { fr0.ip = ip; Value a = pop(); push(mk_bool(!(a.k == V_Bool && a.b))); break; }
      case OP_EQ: case OP_NE: {
        fr0.ip = ip;
        Value b = pop(), a = pop();
        bool eq = val_equal(a, b);
        val_release(a); val_release(b);
        push(mk_bool(op == OP_EQ ? eq : !eq));
        break;
      }
      case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
        fr0.ip = ip;
        Value b = pop(), a = pop();
        int c = val_compare(a, b);
        val_release(a); val_release(b);
        bool r = op == OP_LT ? c < 0 : op == OP_LE ? c <= 0 : op == OP_GT ? c > 0 : c >= 0;
        push(mk_bool(r));
        break;
      }
      case OP_CMP_DYN: {
        fr0.ip = ip;
        Value b = pop(), a = pop();
        int c = 0;
        if (a.k == V_Obj && a.o->kind == O_Inst) {
          // Comparable を実装したクラス。compare を呼ぶ
          InstObj* o = (InstObj*)a.o;
          int slot = -1;
          for (int i = 0; i < prog->classes.size(); i++) {
            ClassInfo* c = prog->classes[i];
            if (!(c->name == "Comparable" && c->module == "std")) continue;
            if (c->methods.size() > 0) slot = prog->funcs[c->methods[0].func]->vslot;
            break;
          }
          int fidx = (slot >= 0 && slot < o->cls->vtable.size()) ? o->cls->vtable[slot] : -1;
          if (fidx >= 0) {
            push(a);
            push(b);
            if (!call_function(fidx, 2)) break;
            break;  // 戻り値がそのまま結果になる
          }
          val_release(a); val_release(b);
          push(mk_int(0));
          break;
        }
        c = val_compare(a, b);
        val_release(a); val_release(b);
        push(mk_int(c));
        break;
      }

      // --- 分岐 ---
      case OP_JUMP: { ip += 4; int a = RD_I32(ip); fr0.ip = a; break; }
      case OP_JUMP_IF_FALSE: {
        ip += 4; int a = RD_I32(ip);
        Value v = t->stack[t->sp - 1];
        bool f = !(v.k == V_Bool && v.b);
        Value d = pop(); val_release(d);
        fr0.ip = f ? a : ip;
        break;
      }
      case OP_JUMP_IF_TRUE: {
        ip += 4; int a = RD_I32(ip);
        Value v = t->stack[t->sp - 1];
        bool f = (v.k == V_Bool && v.b);
        Value d = pop(); val_release(d);
        fr0.ip = f ? a : ip;
        break;
      }
      case OP_JUMP_IF_FALSE_KEEP: {
        ip += 4; int a = RD_I32(ip);
        Value v = t->stack[t->sp - 1];
        fr0.ip = (!(v.k == V_Bool && v.b)) ? a : ip;
        break;
      }
      case OP_JUMP_IF_TRUE_KEEP: {
        ip += 4; int a = RD_I32(ip);
        Value v = t->stack[t->sp - 1];
        fr0.ip = (v.k == V_Bool && v.b) ? a : ip;
        break;
      }
      case OP_JUMP_IF_NONE: {
        ip += 4; int a = RD_I32(ip);
        if (t->stack[t->sp - 1].k == V_None) { Value d = pop(); val_release(d); fr0.ip = a; }
        else fr0.ip = ip;
        break;
      }
      case OP_JUMP_IF_NOT_NONE: {
        ip += 4; int a = RD_I32(ip);
        if (t->stack[t->sp - 1].k != V_None) fr0.ip = a;
        else { Value d = pop(); val_release(d); fr0.ip = ip; }
        break;
      }
      case OP_JUMP_IF_ERR: {
        ip += 4; int a = RD_I32(ip);
        Value v = t->stack[t->sp - 1];
        bool err = (v.k == V_Obj && v.o->kind == O_Result && !((ResultObj*)v.o)->ok);
        fr0.ip = err ? a : ip;
        break;
      }
      case OP_UNWRAP_OK: {
        fr0.ip = ip;
        Value r = pop();
        if (r.k == V_Obj && r.o->kind == O_Result) push(val_retain(((ResultObj*)r.o)->val));
        else push(val_retain(r));
        val_release(r);
        break;
      }
      case OP_UNWRAP_ERR: {
        fr0.ip = ip;
        Value r = pop();
        if (r.k == V_Obj && r.o->kind == O_Result) push(val_retain(((ResultObj*)r.o)->val));
        else push(make_error(Str("失敗しました"), 0));
        val_release(r);
        break;
      }
      case OP_FORCE: {
        fr0.ip = ip;
        Value v = pop();
        if (v.k == V_None) {
          panic(Str("値がありません（! を書いた場所に none が来ました）"));
          break;
        }
        if (v.k == V_Obj && v.o->kind == O_Result) {
          ResultObj* r = (ResultObj*)v.o;
          if (!r->ok) {
            Str msg = Str("失敗しました");
            if (r->val.k == V_Obj && r->val.o->kind == O_Inst) {
              InstObj* e = (InstObj*)r->val.o;
              if (e->fields.size() > 0 && e->fields[0].k == V_Obj) msg = ((StrObj*)e->fields[0].o)->s;
            }
            panic(Str("失敗した結果に ! を書きました: ") + msg);
            val_release(v);
            break;
          }
          push(val_retain(r->val));
          val_release(v);
          break;
        }
        push(v);
        break;
      }

      // --- 呼び出し ---
      case OP_CALL: {
        ip += 4; int fidx = RD_I32(ip);
        ip += 4; int n = RD_I32(ip);
        fr0.ip = ip;
        call_function(fidx, n);
        break;
      }
      case OP_CALL_VIRTUAL: {
        ip += 4; int slot = RD_I32(ip);
        ip += 4; int n = RD_I32(ip);
        fr0.ip = ip;
        Value* recv = deref(&t->stack[t->sp - n]);
        if (recv->k != V_Obj || recv->o->kind != O_Inst) {
          panic(Str("メソッドを呼べません"));
          break;
        }
        InstObj* o = (InstObj*)recv->o;
        int fidx = (slot < o->cls->vtable.size()) ? o->cls->vtable[slot] : -1;
        if (fidx < 0) {
          panic(Str("実装されていないメソッドが呼ばれました（") + o->cls->name + "）");
          break;
        }
        call_function(fidx, n);
        break;
      }
      case OP_CALL_VALUE: {
        ip += 4; int n = RD_I32(ip);
        fr0.ip = ip;
        Value fv = t->stack[t->sp - n - 1];
        if (fv.k != V_Obj || fv.o->kind != O_Func) {
          panic(Str("関数ではないものを呼び出そうとしました"));
          break;
        }
        int fidx = ((FuncObj*)fv.o)->fn;
        for (int i = 0; i < n; i++) t->stack[t->sp - n - 1 + i] = t->stack[t->sp - n + i];
        t->stack[t->sp - 1] = Value();
        t->sp--;
        val_release(fv);
        call_function(fidx, n);
        break;
      }
      case OP_CALL_NATIVE: {
        ip += 4; int id = RD_I32(ip);
        ip += 4; int n = RD_I32(ip);
        Value* args = t->stack + (t->sp - n);
        Value out;
        NativeStatus st = reg->at(id).fn(*this, args, n, out);
        if (st == N_Wait) {
          t->frames.back().ip = op_start;
          if (t->wake_at != 0) t->status = TS_Waiting;
          steps_since_switch++;
          if (!switch_task()) return status;
          if (idle_hint) return SK_Running;
          if (steps_since_switch > (tasks.size() + 1) * 8) {
            // どのタスクも進めない
            bool timer = false;
            for (int i = 0; i < tasks.size(); i++) {
              TaskState* o = tasks[i];
              if ((o->status == TS_Waiting || o->status == TS_Ready) && o->wake_at != 0) timer = true;
            }
            if (timer) { idle_hint = true; return SK_Running; }
            status = SK_Error;
            error_message = Str("すべてのタスクが待ったままになりました（届く見込みのない受け取り待ちです）");
            error_trace = build_trace();
            return status;
          }
          break;
        }
        steps_since_switch = 0;
        if (st == N_Cancel) {
          t->status = TS_Cancelled;
          t->frames.clear();
          for (int i = 0; i < t->sp; i++) val_release(t->stack[i]);
          t->sp = 0;
          if (!switch_task()) return status;
          break;
        }
        t->frames.back().ip = ip;
        if (st == N_Panic) break;
        for (int i = 0; i < n; i++) val_release(t->stack[t->sp - 1 - i]);
        t->sp -= n;
        push(out);
        break;
      }
      case OP_RET: case OP_RET_VOID: {
        Value ret = (op == OP_RET) ? pop() : mk_void();
        for (int i = base; i < t->sp; i++) { val_release(t->stack[i]); t->stack[i] = Value(); }
        t->sp = base;
        t->frames.pop();
        push(ret);
        break;
      }

      // --- 生成 ---
      case OP_NEW_LIST: {
        ip += 4; int n = RD_I32(ip); fr0.ip = ip;
        Value lv = mk_list();
        ListObj* l = as_list(lv);
        l->v.reserve(n);
        for (int i = 0; i < n; i++) l->v.push(t->stack[t->sp - n + i]);
        for (int i = 0; i < n; i++) t->stack[t->sp - 1 - i] = Value();
        t->sp -= n;
        push(lv);
        break;
      }
      case OP_NEW_MAP: {
        ip += 4; int n = RD_I32(ip); fr0.ip = ip;
        Value mv = mk_map();
        MapObj* m = as_map(mv);
        for (int i = 0; i < n; i++) {
          Value* k = &t->stack[t->sp - 2 * n + 2 * i];
          Value* v = &t->stack[t->sp - 2 * n + 2 * i + 1];
          map_set(m, *k, *v);
        }
        for (int i = 0; i < 2 * n; i++) { Value v = pop(); val_release(v); }
        push(mv);
        break;
      }
      case OP_NEW_INST: {
        ip += 4; int ci = RD_I32(ip); fr0.ip = ip;
        ClassInfo* c = prog->classes[ci];
        Value v = mk_inst(c);
        InstObj* o = as_inst(v);
        for (int i = 0; i < c->fields.size(); i++) o->fields.push(default_of(c->fields[i].type));
        push(v);
        break;
      }
      case OP_MAKE_OK: { fr0.ip = ip; Value v = pop(); Value r = mk_result_ok(v); val_release(v); push(r); break; }
      case OP_MAKE_ERR: { fr0.ip = ip; Value v = pop(); Value r = mk_result_err(v); val_release(v); push(r); break; }

      // --- 添字 ---
      case OP_INDEX_GET: {
        fr0.ip = ip;
        Value idx = pop(), col = pop();
        Value* cv = deref(&col);
        if (cv->k != V_Obj) { val_release(idx); val_release(col); push(mk_none()); break; }
        switch (cv->o->kind) {
          case O_List: {
            ListObj* l = (ListObj*)cv->o;
            if (idx.k != V_Int || idx.i < 0 || idx.i >= l->v.size()) {
              panic(Str("配列の長さは ") + str_from_int(l->v.size()) + " ですが、" +
                    str_from_int(idx.k == V_Int ? idx.i : 0) + " 番目を読もうとしました");
              val_release(idx); val_release(col);
              break;
            }
            push(val_retain(l->v[(int)idx.i]));
            break;
          }
          case O_Map: {
            MapObj* m = (MapObj*)cv->o;
            Value* p = map_find(m, idx);
            if (!p) {
              bool q = idx.k == V_Obj && idx.o->kind == O_Str;
              panic(Str("キー ") + (q ? Str("\"") : Str()) + val_to_display(idx) + (q ? Str("\"") : Str()) +
                    " がありません（get() を使うと none が返ります）");
              val_release(idx); val_release(col);
              break;
            }
            push(val_retain(*p));
            break;
          }
          case O_Str: {
            const Str& s = ((StrObj*)cv->o)->s;
            int len = utf8_len(s);
            if (idx.k != V_Int || idx.i < 0 || idx.i >= len) {
              panic(Str("文字列の長さは ") + str_from_int(len) + " ですが、" +
                    str_from_int(idx.k == V_Int ? idx.i : 0) + " 番目を読もうとしました");
              val_release(idx); val_release(col);
              break;
            }
            int a = utf8_offset(s, (int)idx.i);
            int b = utf8_offset(s, (int)idx.i + 1);
            push(mk_str(s.sub(a, b - a)));
            break;
          }
          case O_Bytes: {
            const Str& s = ((StrObj*)cv->o)->s;
            if (idx.k != V_Int || idx.i < 0 || idx.i >= s.size()) {
              panic(Str("バイト列の長さは ") + str_from_int(s.size()) + " ですが、" +
                    str_from_int(idx.k == V_Int ? idx.i : 0) + " 番目を読もうとしました");
              val_release(idx); val_release(col);
              break;
            }
            push(mk_int((unsigned char)s[(int)idx.i]));
            break;
          }
          case O_Json: {
            JsonObj* j = (JsonObj*)cv->o;
            Value out = mk_json();
            if (idx.k == V_Int && j->jk == J_List) {
              if (idx.i >= 0 && idx.i < j->items.size()) { val_release(out); out = val_retain(j->items[(int)idx.i]); }
            } else if (idx.k == V_Obj && idx.o->kind == O_Str && j->jk == J_Map) {
              const Str& key = ((StrObj*)idx.o)->s;
              for (int i = 0; i < j->keys.size(); i++)
                if (j->keys[i] == key) { val_release(out); out = val_retain(j->items[i]); break; }
            }
            push(out);
            break;
          }
          default:
            push(mk_none());
            break;
        }
        val_release(idx);
        val_release(col);
        break;
      }

      // --- 繰り返し ---
      case OP_ITER_NEW: {
        fr0.ip = ip;
        push(mk_int(0));
        break;
      }
      case OP_ITER_NEXT: {
        ip += 4; int a = RD_I32(ip);
        Value* col = &t->stack[t->sp - 2];
        Value* idxv = &t->stack[t->sp - 1];
        int64_t i = idxv->i;
        bool done = true;
        Value out;
        if (col->k == V_Obj) {
          if (col->o->kind == O_List) {
            ListObj* l = (ListObj*)col->o;
            if (i < l->v.size()) { out = val_retain(l->v[(int)i]); done = false; }
          } else if (col->o->kind == O_Range) {
            RangeObj* r = (RangeObj*)col->o;
            int64_t v = r->start + i * r->step;
            if ((r->step > 0 && v < r->end) || (r->step < 0 && v > r->end)) { out = mk_int(v); done = false; }
          } else if (col->o->kind == O_Map) {
            MapObj* m = (MapObj*)col->o;
            while (i < m->e.size() && m->e[(int)i].dead) i++;
            if (i < m->e.size()) { out = val_retain(m->e[(int)i].key); done = false; }
          } else if (col->o->kind == O_Str) {
            const Str& s = ((StrObj*)col->o)->s;
            int off = utf8_offset(s, (int)i);
            if (off < s.size()) {
              int nx = utf8_offset(s, (int)i + 1);
              out = mk_str(s.sub(off, nx - off));
              done = false;
            }
          }
        }
        if (done) {
          Value x = pop(); val_release(x);
          Value y = pop(); val_release(y);
          fr0.ip = a;
          break;
        }
        idxv->i = i + 1;
        fr0.ip = ip;
        push(out);
        break;
      }

      // --- 並行 ---
      case OP_TASK: {
        ip += 4; int fidx = RD_I32(ip);
        ip += 4; int n = RD_I32(ip);
        fr0.ip = ip;
        Value* args = t->stack + (t->sp - n);
        int ti = spawn_task(fidx, n, args);
        for (int i = 0; i < n; i++) { Value v = pop(); val_release(v); }
        TaskObj* to = new (sk_alloc(sizeof(TaskObj))) TaskObj();
        to->t = tasks[ti];
        to->t->refs++;
        push(mk_obj_value(to));
        break;
      }
      case OP_PARALLEL: {
        ip += 4; int n = RD_I32(ip);
        bool all_done = true;
        for (int i = 0; i < n; i++) {
          Value* v = &t->stack[t->sp - n + i];
          if (v->k != V_Obj || v->o->kind != O_Task) continue;
          TaskState* ts = ((TaskObj*)v->o)->t;
          if (ts->status == TS_Ready || ts->status == TS_Waiting) all_done = false;
        }
        if (!all_done) {
          t->frames.back().ip = op_start;
          steps_since_switch++;
          if (!switch_task()) return status;
          if (idle_hint) return SK_Running;
          break;
        }
        steps_since_switch = 0;
        fr0.ip = ip;
        Value lv = mk_list();
        ListObj* l = as_list(lv);
        for (int i = 0; i < n; i++) {
          Value* v = &t->stack[t->sp - n + i];
          if (v->k == V_Obj && v->o->kind == O_Task) l->v.push(val_retain(((TaskObj*)v->o)->t->result));
          else l->v.push(mk_none());
        }
        for (int i = 0; i < n; i++) { Value v = pop(); val_release(v); }
        push(lv);
        break;
      }

      case OP_PANIC: {
        fr0.ip = ip;
        Value v = pop();
        panic(val_to_display(v));
        val_release(v);
        break;
      }
      case OP_HALT:
        fr0.ip = ip;
        status = SK_Finished;
        return status;

      default:
        fr0.ip = ip;
        panic(Str("知らない命令に出会いました"));
        break;
    }

    if (op != OP_CALL_NATIVE && op != OP_PARALLEL) steps_since_switch = 0;

    // メモリを使いすぎていないか、命令の切れ目ごとに見る
    if (!has_panic && sk_mem_over()) {
      panic(Str("メモリを使いすぎました\n  上限は ") + str_from_int((int64_t)(sk_mem_limit() >> 20)) +
            " MB です。増え続ける配列や文字列がないか見てください");
    }

    // panic の後始末
    if (has_panic) {
      has_panic = false;
      TaskState* pt = tasks[cur];
      pt->panic_msg = pending_panic;
      pt->panic_trace = build_trace();
      if (cur == 0) {
        status = SK_Error;
        error_message = pending_panic;
        error_trace = pt->panic_trace;
        if (pt->frames.size() > 0) {
          Frame& f = pt->frames.back();
          error_line = (f.ip > 0 && f.ip - 1 < f.fn->lines.size()) ? (int)f.fn->lines[f.ip - 1] : 0;
          error_file = f.fn->file.size() ? f.fn->file : f.fn->module;
        }
        // 止めたあとは、抱えていた値を離す（メモリを使いすぎて止まったときのため）
        pt->frames.clear();
        for (int i = 0; i < pt->sp; i++) val_release(pt->stack[i]);
        pt->sp = 0;
        pt->places.clear();
        return status;
      }
      // タスクの panic は、そのタスクだけを止める
      pt->status = TS_Panicked;
      pt->frames.clear();
      for (int i = 0; i < pt->sp; i++) val_release(pt->stack[i]);
      pt->sp = 0;
      if (!switch_task()) return status;
    }
  }
  return status;
}

}  // namespace shark
