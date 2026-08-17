// registry.h — 処理系が持つ関数の表
//
// 標準ライブラリもホスト関数も、ここに同じ形で並ぶ。
// 型検査はこの表を見て呼び出しを解決し、仮想マシンは番号で呼ぶ。
#ifndef SHARK_REGISTRY_H
#define SHARK_REGISTRY_H

#include "program.h"
#include "types.h"

namespace shark {

struct NativeEntry {
  Str name;        // "print" "math.abs" "list.push"
  NativeFn fn;
  Vec<Type*> params;
  Type* ret;
  bool ref0;       // 第1引数を ref で受け取る
  bool typed;      // 型検査に使える型を持っているか
  bool is_host;    // ホストが足した関数
  NativeEntry() : fn(0), ret(0), ref0(false), typed(true), is_host(false) {}
};

class Registry {
 public:
  explicit Registry(TypeTable& types) : types_(types) {}

  int add(const char* name, NativeFn fn, Type* ret,
          Type* p0 = 0, Type* p1 = 0, Type* p2 = 0, Type* p3 = 0);
  // 型検査では使わない（checker が型を決める）もの
  int add_untyped(const char* name, NativeFn fn);
  void mark_ref0(int id) { e_[id].ref0 = true; }
  void mark_host(int id) { e_[id].is_host = true; }

  int size() const { return e_.size(); }
  const NativeEntry& operator[](int i) const { return e_[i]; }
  NativeEntry& at(int i) { return e_[i]; }

  int find(const char* name) const;                 // 最初の1つ
  void find_all(const Str& name, Vec<int>* out) const;

  void enable_module(const char* name) { modules_.push(Str(name)); }
  bool has_module(const Str& name) const {
    for (int i = 0; i < modules_.size(); i++) if (modules_[i] == name) return true;
    return false;
  }
  const Vec<Str>& modules() const { return modules_; }

  TypeTable& types() { return types_; }

 private:
  Vec<NativeEntry> e_;
  Vec<Str> modules_;
  TypeTable& types_;
};

// 標準ライブラリの登録（lib/ の各ファイル）
void register_builtin(Registry& r);   // import なしで使えるもの
void register_math(Registry& r);
void register_crypto(Registry& r);
void register_time(Registry& r);
void register_task(Registry& r);
void register_fmt(Registry& r);
void register_json(Registry& r);
void register_path(Registry& r);
void register_file(Registry& r);
void register_os(Registry& r);
void register_text(Registry& r);
void register_test(Registry& r);
// 型に付くメソッド（string.len など）。名前で引ける形で入れておく
void register_methods(Registry& r);

// std.test の記録（フロントエンドが読む）
void test_begin();
void test_reset_hooks();
bool test_failed();
const Str& test_message();
const Str& test_desc();
int test_before_index();
int test_after_index();

// std.os にコマンド引数を渡す
void os_set_args(const Vec<Str>& args);

}  // namespace shark
#endif
