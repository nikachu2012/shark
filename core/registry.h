// registry.h — 処理系が持つ関数の表
//
// 標準ライブラリもホスト関数も、ここに同じ形で並ぶ。
// 型検査はこの表を見て呼び出しを解決し、仮想マシンは番号で呼ぶ。
#ifndef SHARK_REGISTRY_H
#define SHARK_REGISTRY_H

#include "config.h"
#include "program.h"
#include "types.h"

namespace shark {

struct NativeEntry {
  Str name;        // "print" "math.abs" "list.push"
  NativeFn fn;
  Vec<Type*> params;
  Type* ret;
  int ref_at;      // ref で受け取る引数の番号（-1 なら無い）
  bool ref_var;    // その ref を「どの var か」として覚える（一番外側の var だけを受ける）
  bool typed;      // 型検査に使える型を持っているか
  bool is_host;    // ホストが足した関数
  NativeEntry()
      : fn(0), ret(0), ref_at(-1), ref_var(false), typed(true), is_host(false) {}
};

class Registry {
 public:
  explicit Registry(TypeTable& types) : types_(types) {}

  // 引数は 10 個まで（ui.tri が 10 個いる）
  int add(const char* name, NativeFn fn, Type* ret,
          Type* p0 = 0, Type* p1 = 0, Type* p2 = 0, Type* p3 = 0, Type* p4 = 0,
          Type* p5 = 0, Type* p6 = 0, Type* p7 = 0, Type* p8 = 0, Type* p9 = 0);
  // 型検査では使わない（checker が型を決める）もの
  int add_untyped(const char* name, NativeFn fn);
  void mark_ref0(int id) { e_[id].ref_at = 0; }
  // ref を、呼び出しのあとも書き戻し先にするもの（ui.field など）。
  // 持ち続けるのは借用ではなく「どの var か」なので、型検査は一番外側の var だけを通す
  // （check.cpp。一番外側の var はプログラムが終わるまで生きている）。
  // at は何番目の引数か（ui.checkbox(label, ref on) のように 0 でないこともある）
  void mark_ref_var(int id, int at = 0) { e_[id].ref_at = at; e_[id].ref_var = true; }
  void mark_ref0_var(int id) { mark_ref_var(id, 0); }
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
void register_ui(Registry& r);
// 型に付くメソッド（string.len など）。名前で引ける形で入れておく
void register_methods(Registry& r);

// 設定にある標準ライブラリを、決まった順で全部入れる。
// バイトコードは関数を**番号**で指すので、この順が表の番号を決める。
// ソースから作るとき（Engine）と、保存したバイトコードを動かすとき（Runtime）で
// 同じものを呼ぶことで、番号が食い違わないようにしている。
void register_modules(Registry& r, const Config& cfg);

// 表の指紋。並びと名前と引数の数から作る。
// 保存したバイトコードが、いまの処理系の表と合っているかを見るのに使う
uint64_t registry_signature(const Registry& r);

// 処理系が持つクラス（Error、Widget）のメソッドの本体を、名前と引数の型で引く
// （本体は lib/builtin.cpp）。保存したバイトコードを読み戻すとき、関数ポインタをつなぎ直すのに使う。
// 名前と数だけでは足りない（Widget.width は int と float で別の本体）ので、引数の並びごと渡す
NativeFn builtin_native_method(const Str& cls, const Str& name, const Vec<ParamInfo>& params);

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

// std.ui の後始末（面を捨て、画面を閉じる）。処理系を捨てるときに呼ぶ
void ui_shutdown();

// std.ui の Widget。関数の表には仮のクラスで登録してあるので、
// 型検査が本物を作ったところで差し替える（core/check.cpp から呼ぶ）
struct ClassInfo;
void ui_bind_widget_class(Registry& r, ClassInfo* real);
void ui_bind_canvas_class(Registry& r, ClassInfo* real);

}  // namespace shark
#endif
