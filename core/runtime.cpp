#include "runtime.h"

namespace shark {

Runtime::Runtime(const Config& cfg) : cfg_(cfg), reg_(types_), prog_(0), ok_(false) {
  diag_.set_lang(cfg.lang);
  diag_.set_strict(cfg.strict);
  vm_.stack_size = cfg.stack_size;
  vm_.task_stack_size = cfg.task_stack_size;
  vm_.max_call_depth = cfg.max_call_depth;
  vm_.diag = &diag_;
  sk_mem_set_limit(cfg.memory_limit);
  // Engine と同じものを同じ順で入れる。ここが揃っていないと関数の番号が食い違う
  register_modules(reg_, cfg);
}

Runtime::~Runtime() {
  vm_.reset();
  if (prog_) { prog_->~Program(); sk_free(prog_); }
}

int Runtime::register_host(const char* name, NativeFn fn, Type* ret, Type* p0, Type* p1, Type* p2,
                           Type* p3) {
  int id = reg_.add(name, fn, ret, p0, p1, p2, p3);
  reg_.mark_host(id);
  return id;
}

bool Runtime::load(const Str& bytecode, Str* err) {
  ok_ = false;
  vm_.reset();
  if (prog_) { prog_->~Program(); sk_free(prog_); prog_ = 0; }
  prog_ = new (sk_alloc(sizeof(Program))) Program();
  if (!bytecode_read(bytecode, prog_, types_, reg_, cfg_.lang, err)) {
    prog_->~Program();
    sk_free(prog_);
    prog_ = 0;
    return false;
  }
  vm_.set_program(prog_, &reg_);
  vm_.start();
  ok_ = true;
  return true;
}

RunStatus Runtime::step(int budget) { return vm_.step(budget); }

}  // namespace shark
