// vm.h — 仮想マシン（spec/runtime/execution.md、spec/runtime/concurrency.md）
//
// ・スタック型。命令を1つずつ実行する
// ・ホストが渡した命令数（budget）だけ進めて戻る。無限ループでも固まらない
// ・タスクは仮想マシンの中で切り替える。OS のスレッドは使わない
#ifndef SHARK_VM_H
#define SHARK_VM_H

#include "diag.h"
#include "program.h"
#include "registry.h"

namespace shark {

enum RunStatus { SK_Running = 0, SK_Finished = 1, SK_Error = 2 };

struct Frame {
  FuncInfo* fn;
  int ip;
  int base;
};

enum TaskStatus { TS_Ready = 0, TS_Waiting, TS_Done, TS_Panicked, TS_Cancelled };

struct TaskState {
  int id;
  Value* stack;
  int sp;
  int cap;
  Vec<Frame> frames;
  Vec<Value*> places;
  TaskStatus status;
  int64_t wake_at;      // sleep 中の起床時刻（単調時計）。0 は未設定
  int64_t wait_state;   // 待つ関数が使う覚え書き
  bool cancel_req;
  Value result;
  Str panic_msg;
  Str panic_trace;
  int refs;             // Task ハンドルと VM から数える
  bool detached_done;
  TaskState()
      : id(0), stack(0), sp(0), cap(0), status(TS_Ready), wake_at(0), wait_state(0),
        cancel_req(false), refs(1), detached_done(false) {}
};

struct ChannelState {
  Vec<Value> q;
  int cap;
  bool closed;
  int refs;
  ChannelState() : cap(1), closed(false), refs(1) {}
};

struct HostIO {
  void* ud;
  void (*write_out)(void* ud, const char* s, int n);
  bool (*read_line)(void* ud, Str* out);   // false は終端
  // 読める行が来ているか。0 なら「いつでも読める」（端末のように read_line の中で待つホスト）。
  // false を返す間、input() は待ちに入り、ホストに刻みを返す（spec/runtime/embedding.md）
  bool (*input_ready)(void* ud);
  HostIO() : ud(0), write_out(0), read_line(0), input_ready(0) {}
};

struct VM {
  Program* prog;
  Registry* reg;
  DiagBag* diag;
  Vec<Value> globals;
  Vec<TaskState*> tasks;
  int cur;              // 実行中のタスクの位置
  RunStatus status;
  int exit_code;
  Str error_message;    // panic の理由
  Str error_trace;
  int error_line;
  Str error_file;
  HostIO io;
  int stack_size;
  int task_stack_size;
  int max_call_depth;
  int64_t started_at;
  bool aborted;
  uint64_t rng_state;
  int steps_since_switch;

  VM();
  ~VM();

  void set_program(Program* p, Registry* r);
  void start(bool with_inits = true);  // 初期化とエントリの呼び出しを準備する
  void reset();                 // 読み込み直しの前に、抱えているものを離す
  RunStatus step(int budget);   // budget 命令だけ進める
  void abort_run() { aborted = true; }

  // ライブラリから使う道具
  TaskState* task() { return tasks[cur]; }
  void panic(const Str& msg);
  // panic の言い方（spec/runtime/diagnostics.md）。診断と同じで、ホストが選ぶ。
  // diag が無ければ日本語にする
  Lang lang() const { return diag ? diag->lang() : LANG_JA; }
  Str L(const char* ja, const char* en) const { return Str(lang() == LANG_JA ? ja : en); }
  Str L(const Str& ja, const Str& en) const { return lang() == LANG_JA ? ja : en; }
  void write_out(const Str& s);
  bool read_line(Str* out);
  bool input_ready();
  Value make_error(const Str& msg, int code);
  Str build_trace();

  // 内部
  void push(const Value& v);
  Value pop();
  bool call_function(int fidx, int nargs);
  int  spawn_task(int fidx, int nargs, Value* args);
  bool switch_task();
  void finish_task(TaskState* t, const Value& result);

  Str pending_panic;
  bool has_panic;
  bool idle_hint;       // どのタスクも待ちで、進められない
  bool input_wait;      // input() がホストからの行を待っている
  Vec<int> boot;        // 初期化と入口の呼び出し順
  int boot_pos;
  Value default_of(Type* t);
};

TaskState* task_new(VM& vm, int stack_size);
void task_unref(TaskState* t);
void chan_unref(ChannelState* c);

}  // namespace shark
#endif
