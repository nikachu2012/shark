// jit.cpp — 実行時コンパイル（spec/runtime/execution.md）
//
// 機種によらない土台はここ。機械語そのものを作るところは
// jit_arm64.inc（ARM64）と jit_x64.inc（x86-64）にある。
//
//   1. 仮想マシンが「この関数はよく通る」と数える（jit_hot）
//   2. しきい値を超えたら、その関数を丸ごと機械語にする（jit_compile）
//   3. 次にその関数に入るとき、機械語のほうを走らせる（jit_run）
//
// 作り方の分からない命令は、その場で仮想マシン（VM::step(1)）に1つだけ
// 動かしてもらい、続きに戻る。だから**どの命令でも意味は仮想マシンと同じ**になる。
#include "jit.h"

#include <stddef.h>

#include "opcodes.h"
#include "platform/platform.h"
#include "vm.h"

// 機械語の作り方を知っている機種か。知らない機種では、常に仮想マシンで実行する
#if defined(__aarch64__) || defined(_M_ARM64)
#define SHARK_JIT_ARM64 1
#elif defined(__x86_64__) || defined(_M_X64)
#define SHARK_JIT_X64 1
#endif

#if defined(SHARK_JIT_ARM64) || defined(SHARK_JIT_X64)
#define SHARK_JIT 1
#endif

namespace shark {

// 何回通ったら機械語にするか（spec/open-questions.md の 2）。
// 小さすぎると一度きりのコードまで作ってしまい、大きすぎると熱くなるのが遅れる。
// bench/ で測って決めた（docs/implementation.md）
static const int kHotThreshold = 40;

// 一度に取る機械語の置き場。足りなくなったらもう1つ取る
static const size_t kBlockSize = 64u * 1024u;

// 大きすぎる関数は作らない（作る時間と置き場が見合わない）
static const int kMaxCode = 64 * 1024;

// ------------------------------------------------------------------ 命令の長さ
//
// オペランドのバイト数。**vm.cpp の読み方と合わせること**。
// 知らない命令には -1 を返し、その関数は機械語にしない
static int op_operand_len(int op) {
  switch (op) {
    case OP_NOP: case OP_NONE: case OP_VOID: case OP_TRUE: case OP_FALSE:
    case OP_POP: case OP_DUP: case OP_SWAP:
    case OP_PLACE_INDEX: case OP_PLACE_LOAD: case OP_PLACE_STORE:
    case OP_PLACE_DUP: case OP_PLACE_REF:
    case OP_ADD_INT: case OP_SUB_INT: case OP_MUL_INT: case OP_DIV_INT: case OP_MOD_INT:
    case OP_NEG_INT:
    case OP_ADD_FLOAT: case OP_SUB_FLOAT: case OP_MUL_FLOAT: case OP_DIV_FLOAT:
    case OP_NEG_FLOAT:
    case OP_AND_INT: case OP_OR_INT: case OP_XOR_INT: case OP_SHL_INT: case OP_SHR_INT:
    case OP_BNOT_INT: case OP_POW_INT: case OP_POW_FLOAT:
    case OP_ADD_TIME_DUR: case OP_SUB_TIME_DUR: case OP_SUB_TIME_TIME:
    case OP_ADD_DUR_DUR: case OP_SUB_DUR_DUR:
    case OP_NOT: case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
    case OP_CMP_DYN:
    case OP_UNWRAP_OK: case OP_UNWRAP_ERR: case OP_FORCE:
    case OP_RET: case OP_RET_VOID:
    case OP_MAKE_OK: case OP_MAKE_ERR:
    case OP_INDEX_GET: case OP_ITER_NEW:
    case OP_PANIC: case OP_HALT:
      return 0;

    case OP_CONST: case OP_ROT_UNDER:
    case OP_LOAD_LOCAL: case OP_STORE_LOCAL: case OP_LOAD_GLOBAL: case OP_STORE_GLOBAL:
    case OP_LOAD_FIELD: case OP_REF_LOCAL:
    case OP_PLACE_LOCAL: case OP_PLACE_GLOBAL: case OP_PLACE_FIELD:
    case OP_CONCAT:
    case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE:
    case OP_JUMP_IF_FALSE_KEEP: case OP_JUMP_IF_TRUE_KEEP:
    case OP_JUMP_IF_NONE: case OP_JUMP_IF_NOT_NONE: case OP_JUMP_IF_ERR:
    case OP_CALL_VALUE:
    case OP_NEW_LIST: case OP_NEW_MAP: case OP_NEW_INST:
    case OP_ITER_NEXT: case OP_PARALLEL:
      return 4;

    case OP_CALL: case OP_CALL_NATIVE: case OP_CALL_VIRTUAL: case OP_TASK:
      return 8;

    default:
      return -1;
  }
}

// 飛び先を持つ命令か（持つなら、オペランドの先頭 4 バイトが飛び先）
static bool op_is_jump(int op) {
  switch (op) {
    case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE:
    case OP_JUMP_IF_FALSE_KEEP: case OP_JUMP_IF_TRUE_KEEP:
    case OP_JUMP_IF_NONE: case OP_JUMP_IF_NOT_NONE: case OP_JUMP_IF_ERR:
    case OP_ITER_NEXT:
      return true;
    default:
      return false;
  }
}

static int rd_i32(const uint8_t* c, int at) {
  return (int)((uint32_t)c[at] | ((uint32_t)c[at + 1] << 8) | ((uint32_t)c[at + 2] << 16) |
               ((uint32_t)c[at + 3] << 24));
}

// ------------------------------------------------------------------ 下ごしらえ
//
// バイトコードを読んで、命令の切れ目と区切り（basic block）の先頭を調べる。
// 読み方が少しでも合わなければ false を返し、その関数は機械語にしない
struct JitScan {
  Vec<uint8_t> start;   // その位置が命令の先頭か
  Vec<uint8_t> block;   // 1 = 区切りの先頭、2 = ループの入口（後ろから戻ってくる）
  Vec<int32_t> ninst;   // 区切りの先頭なら、その区切りに入っている命令の数
  int ninsts;           // 関数ぜんぶの命令の数
  JitScan() : ninsts(0) {}
};

static bool jit_scan(FuncInfo* f, JitScan* s) {
  const uint8_t* c = f->code.data();
  int n = f->code.size();
  if (n <= 0) return false;
  s->start.resize(n, 0);
  s->block.resize(n, 0);
  s->ninst.resize(n, 0);

  // 1. 命令の切れ目
  int ip = 0;
  while (ip < n) {
    int len = op_operand_len(c[ip]);
    if (len < 0) return false;
    if (ip + 1 + len > n) return false;
    s->start[ip] = 1;
    s->ninsts++;
    ip += 1 + len;
  }
  if (ip != n) return false;

  // 2. 飛び先。切れ目に合っていなければ、読み方が違うということ
  s->block[0] = 1;
  for (ip = 0; ip < n;) {
    int op = c[ip];
    int len = op_operand_len(op);
    if (op_is_jump(op)) {
      int to = rd_i32(c, ip + 1);
      if (to < 0 || to >= n || !s->start[to]) return false;
      if (s->block[to] < 1) s->block[to] = 1;
      if (to <= ip) s->block[to] = 2;   // 後ろへ戻る＝ループの入口
      // 分岐の次も区切りの先頭になる
      int next = ip + 1 + len;
      if (next < n && s->block[next] < 1) s->block[next] = 1;
    }
    ip += 1 + len;
  }

  // 3. 区切りごとの命令の数
  int head = -1;
  for (ip = 0; ip < n;) {
    if (s->block[ip]) head = ip;
    if (head >= 0) s->ninst[head]++;
    ip += 1 + op_operand_len(c[ip]);
  }
  return true;
}

// ------------------------------------------------------------------ 置き場
struct JitBlock {
  void* mem;
  size_t cap;
  size_t used;
};

struct JitFunc {
  void* code;          // 機械語の先頭（＝入口。前口上がここにある）
  Vec<int32_t> off;    // 命令の位置 -> 機械語の中の位置（バイト）。-1 は入れない
  int max_push;        // この関数が値を積みうる最大の数（余裕を見た上限）
};

struct JitState {
  Vec<JitFunc*> code;    // 関数の位置 -> 機械語（無ければ 0）
  Vec<int32_t> hot;      // 関数の位置 -> 通った回数
  Vec<JitBlock> blocks;
  bool busy;             // いま機械語の中にいる（そこから先は仮想マシンで動かす）
  bool broken;           // 置き場が取れなくなった。これ以上は作らない
  JitState() : busy(false), broken(false) {}
};

// 機械語と覚え書きは「処理系のもの」。実行中のプログラムが使う量には数えない
// （spec/runtime/memory.md）
struct MemToolScope {
  int prev;
  MemToolScope() : prev(sk_mem_set_phase(0)) {}
  ~MemToolScope() { sk_mem_set_phase(prev); }
};

static void* jit_alloc_code(JitState* j, size_t n) {
  const PlatformExec* ex = platform().exec;
  if (!ex || j->broken) return 0;
  n = (n + 15u) & ~(size_t)15u;
  for (int i = 0; i < j->blocks.size(); i++) {
    JitBlock& b = j->blocks[i];
    if (b.cap - b.used >= n) {
      void* p = (char*)b.mem + b.used;
      b.used += n;
      return p;
    }
  }
  size_t cap = n > kBlockSize ? ((n + kBlockSize - 1) / kBlockSize) * kBlockSize : kBlockSize;
  void* mem = ex->alloc(cap);
  if (!mem) {
    j->broken = true;
    return 0;
  }
  JitBlock b;
  b.mem = mem;
  b.cap = cap;
  b.used = n;
  j->blocks.push(b);
  return mem;
}

// ------------------------------------------------------------------ 値の並び
//
// 機械語から値を直に読み書きするので、置かれ方を調べておく。
// offsetof は継承したものに使えないので、実物の場所の差で測る
struct JitLayout {
  int obj_rc, obj_kind;
  int vec_data, vec_size;
  int list_vec, inst_fields;
  int range_start, range_end, range_step;
  int vm_steps;
};
static JitLayout g_lay;
static bool g_lay_done = false;

static int diff(const void* base, const void* member) {
  return (int)((const char*)member - (const char*)base);
}

static void layout_init() {
  if (g_lay_done) return;
  g_lay_done = true;
  ListObj lo;
  InstObj io;
  RangeObj ro;
  Obj* ob = &lo;
  g_lay.obj_rc = diff(ob, &ob->rc);
  g_lay.obj_kind = diff(ob, &ob->kind);
  g_lay.vec_data = Vec<Value>::data_offset();
  g_lay.vec_size = Vec<Value>::size_offset();
  g_lay.list_vec = diff(&lo, &lo.v);
  g_lay.inst_fields = diff(&io, &io.fields);
  g_lay.range_start = diff(&ro, &ro.start);
  g_lay.range_end = diff(&ro, &ro.end);
  g_lay.range_step = diff(&ro, &ro.step);
  g_lay.vm_steps = (int)offsetof(VM, steps_since_switch);
}

// 機械語の入口。機種ごとの .inc が作った前口上を、この形で呼ぶ
typedef int64_t (*JitEntry)(VM* vm, Value* locals, Value* top, void* target, int64_t budget,
                            Value* globals, Value* limit);

// ------------------------------------------------------------------ 手伝いの関数
//
// 機械語から呼ぶ。番号は下の表（g_helpers）の並びで、機種ごとの .inc が使う
#if defined(SHARK_JIT)
enum { H_FALLBACK = 0, H_RELEASE, H_COMPARE, H_SYNC, H_CALL, H_CALL_NATIVE, H_RET, H_MAX };

// 1命令のあとも、同じタスクの同じ枠で続けられるか。
// 違っていれば機械語から抜けて、そこから先は仮想マシンに任せる
static bool jit_same_frame(VM* vm, TaskState* t, FuncInfo* fn, int base, int depth, int ip) {
  if (vm->status != SK_Running || vm->aborted) return false;
  if (vm->tasks[vm->cur] != t || t->status != TS_Ready) return false;
  if (t->frames.size() != depth) return false;
  Frame& f = t->frames.back();   // 枠の置き場は増えていることがあるので、読み直す
  return f.fn == fn && f.base == base && f.ip == ip;
}

// 分からない命令を、仮想マシンに1つだけ動かしてもらう。
// 同じ枠の次の命令へ進めたなら新しい「積んだ値の上」を返し、
// そうでなければ 0 を返す（0 のときは機械語から抜ける）
static Value* jit_fallback(VM* vm, int op_ip, int next_ip, Value* top) {
  TaskState* t = vm->tasks[vm->cur];
  if (t->frames.size() == 0) return 0;
  Frame* fr = &t->frames.back();
  FuncInfo* fn = fr->fn;
  int base = fr->base;
  int depth = t->frames.size();
  fr->ip = op_ip;
  t->sp = (int)(top - t->stack);

  vm->step(1);

  if (!jit_same_frame(vm, t, fn, base, depth, next_ip)) return 0;
  return t->stack + t->sp;
}

// 機械語から抜ける前に、仮想マシンの側の覚え書きを合わせる
static void jit_sync(VM* vm, Value* top, int ip) {
  TaskState* t = vm->tasks[vm->cur];
  t->sp = (int)(top - t->stack);
  if (t->frames.size() > 0) t->frames.back().ip = ip;
}

// int どうし以外の比べもの。仮想マシンと同じ答えを返す（つまずかない・確保しない）
static Value* jit_compare(Value* top, int op) {
  Value b = top[-1], a = top[-2];
  bool r;
  if (op == OP_EQ || op == OP_NE) {
    bool e = val_equal(a, b);
    r = (op == OP_EQ) ? e : !e;
  } else {
    int c = val_compare(a, b);
    r = op == OP_LT ? c < 0 : op == OP_LE ? c <= 0 : op == OP_GT ? c > 0 : c >= 0;
  }
  val_release(a);
  val_release(b);
  top[-2] = mk_bool(r);
  return top - 1;
}

// 関数を呼ぶ。枠を積めたら 1（機械語から抜けて、呼ばれた側から続ける）。
// つまずきうるとき（ネイティブ・深すぎ・置き場が足りない）は 0 を返し、仮想マシンに任せる
static int jit_call(VM* vm, Value* top, int fidx, int nargs, int ret_ip) {
  FuncInfo* f = vm->prog->funcs[fidx];
  if (f->is_native) return 0;
  TaskState* t = vm->tasks[vm->cur];
  if (t->frames.size() >= vm->max_call_depth) return 0;
  int base = (int)(top - t->stack) - nargs;
  if (base < 0 || base + f->nlocals > t->cap) return 0;
  // ここから先は vm.cpp の OP_CALL と同じ
  t->sp = (int)(top - t->stack);
  t->frames.back().ip = ret_ip;
  vm->steps_since_switch = 0;
  vm->call_function(fidx, nargs);
  vm->after_step();
  return 1;
}

// 処理系が持つ関数を呼ぶ（vm.cpp の OP_CALL_NATIVE と同じ）。
// 続けられるなら新しい「積んだ値の上」、そうでなければ 0
static Value* jit_call_native(VM* vm, Value* top, int id, int nargs, int op_ip, int next_ip) {
  TaskState* t = vm->tasks[vm->cur];
  Frame* fr = &t->frames.back();
  FuncInfo* fn = fr->fn;
  int base = fr->base;
  int depth = t->frames.size();
  t->sp = (int)(top - t->stack);
  Value* args = t->stack + (t->sp - nargs);
  Value out;
  NativeStatus st = vm->reg->at(id).fn(*vm, args, nargs, out);
  if (st == N_Wait || st == N_Cancel) {
    // 待ちと取り消しの段取り（タスクの切り替えまで）は仮想マシンに任せる。
    // この命令をやり直してもらう。待つ関数は「同じ引数で呼び直してよい」決まりなので、
    // もう一度呼ばれても同じ意味になる（program.h の NativeStatus）
    t->frames.back().ip = op_ip;
    vm->step(1);
    return 0;
  }
  t->frames.back().ip = next_ip;
  vm->steps_since_switch = 0;
  if (st == N_Panic) {
    vm->after_step();
    return 0;
  }
  for (int i = 0; i < nargs; i++) val_release(t->stack[t->sp - 1 - i]);
  t->sp -= nargs;
  vm->push(out);
  if (!vm->after_step()) return 0;
  // つまずいたタスクは止まり、別のタスクに移っていることがある
  if (!jit_same_frame(vm, t, fn, base, depth, next_ip)) return 0;
  return t->stack + t->sp;
}

// 関数から返る（vm.cpp の OP_RET と同じ）。枠が変わるので、必ず機械語から抜ける
static void jit_ret(VM* vm, Value* top, int has_value) {
  TaskState* t = vm->tasks[vm->cur];
  t->sp = (int)(top - t->stack);
  Value ret = has_value ? vm->pop() : mk_void();
  int base = t->frames.back().base;
  for (int i = base; i < t->sp; i++) {
    val_release(t->stack[i]);
    t->stack[i] = Value();
  }
  t->sp = base;
  t->frames.pop();
  vm->push(ret);
  vm->steps_since_switch = 0;
  vm->after_step();
}

typedef void (*JitHelper)();
static JitHelper g_helpers[H_MAX] = {(JitHelper)&jit_fallback,    (JitHelper)&obj_release,
                                     (JitHelper)&jit_compare,     (JitHelper)&jit_sync,
                                     (JitHelper)&jit_call,        (JitHelper)&jit_call_native,
                                     (JitHelper)&jit_ret};

#if defined(SHARK_JIT_ARM64)
#include "jit_arm64.inc"
#else
#include "jit_x64.inc"
#endif

#else   // SHARK_JIT
// 機械語の作り方を知らない機種。作らないので、いつも仮想マシンで動く
static bool jit_emit(FuncInfo*, const JitScan&, Vec<uint8_t>*, Vec<int32_t>*) { return false; }
#endif

// ------------------------------------------------------------------ 表からの口
bool jit_available() {
#if defined(SHARK_JIT)
  return platform().exec != 0;
#else
  return false;
#endif
}

JitState* jit_new() {
  if (!jit_available()) return 0;
  MemToolScope tool;
  layout_init();
  return new (sk_alloc(sizeof(JitState))) JitState();
}

void jit_free(JitState* j) {
  if (!j) return;
  MemToolScope tool;
  jit_clear(j);
  const PlatformExec* ex = platform().exec;
  for (int i = 0; i < j->blocks.size(); i++)
    if (ex) ex->free(j->blocks[i].mem, j->blocks[i].cap);
  j->~JitState();
  sk_free(j);
}

void jit_clear(JitState* j) {
  if (!j) return;
  MemToolScope tool;
  for (int i = 0; i < j->code.size(); i++) {
    if (!j->code[i]) continue;
    j->code[i]->~JitFunc();
    sk_free(j->code[i]);
  }
  j->code.clear();
  j->hot.clear();
  // 置き場は使い回す（機械語を捨てただけなので、先頭から詰め直す）
  for (int i = 0; i < j->blocks.size(); i++) j->blocks[i].used = 0;
  j->broken = false;
}

static void jit_grow(JitState* j, VM& vm) {
  MemToolScope tool;   // この覚え書きは処理系のもの。プログラムが使う量には数えない
  int n = vm.prog ? vm.prog->funcs.size() : 0;
  while (j->code.size() < n) j->code.push(0);
  while (j->hot.size() < n) j->hot.push(0);
}

// 関数を1つ、機械語にする。作れなければ 0（次からは数え直さない）
static JitFunc* jit_compile(JitState* j, FuncInfo* f) {
  MemToolScope tool;
  JitScan sc;
  if (f->is_native || f->code.size() == 0 || f->code.size() > kMaxCode) return 0;
  if (!jit_scan(f, &sc)) return 0;

  // 命令の長さが機種によって違う（ARM64 は 4 バイト、x86-64 は 1〜15 バイト）ので、
  // 受け取るのはバイトの並び。中の位置（off）もバイトで数える
  Vec<uint8_t> code;
  Vec<int32_t> off;
  if (!jit_emit(f, sc, &code, &off)) return 0;

  size_t bytes = (size_t)code.size();
  void* mem = jit_alloc_code(j, bytes);
  if (!mem) return 0;
  const PlatformExec* ex = platform().exec;
  ex->unlock(mem, bytes);
  uint8_t* dst = (uint8_t*)mem;
  for (int i = 0; i < code.size(); i++) dst[i] = code[i];
  ex->commit(mem, bytes);

  JitFunc* jf = new (sk_alloc(sizeof(JitFunc))) JitFunc();
  jf->code = mem;
  jf->off.swap_with(off);
  // 値を積む深さの上限。命令1つが積むのは多くても1つなので、命令の数で足りる
  jf->max_push = sc.ninsts + 8;
  return jf;
}

bool jit_hot(JitState* j, VM& vm, FuncInfo* f) {
  if (!j || j->busy || j->broken || f->index < 0) return false;
  jit_grow(j, vm);
  if (f->index >= j->hot.size()) return false;
  if (j->code[f->index]) return false;
  int limit = vm.jit_threshold > 0 ? vm.jit_threshold : kHotThreshold;
  if (++j->hot[f->index] < limit) return false;
  j->hot[f->index] = -0x40000000;   // 作れなかったときに、もう一度来ないように
  JitFunc* jf = jit_compile(j, f);
  if (!jf) return false;
  j->code[f->index] = jf;
  return true;
}

bool jit_run(JitState* j, VM& vm, TaskState* t, Frame& fr, int* budget) {
  if (!j || j->busy) return false;
  FuncInfo* f = fr.fn;
  if (f->index < 0 || f->index >= j->code.size()) return false;
  JitFunc* jf = j->code[f->index];
  if (!jf) return false;
  if (fr.ip < 0 || fr.ip >= jf->off.size()) return false;
  int32_t at = jf->off[fr.ip];
  if (at < 0) return false;
  // 値の置き場が足りるか。足りなければ仮想マシンに任せる（あちらが正しく止める）
  if (t->sp + jf->max_push > t->cap) return false;

  j->busy = true;
  int64_t left = ((JitEntry)jf->code)(&vm, t->stack + fr.base, t->stack + t->sp,
                                      (char*)jf->code + at, (int64_t)*budget, vm.globals.data(),
                                      t->stack + (t->cap - jf->max_push));
  j->busy = false;
  *budget = (int)left;
  return true;
}

}  // namespace shark
