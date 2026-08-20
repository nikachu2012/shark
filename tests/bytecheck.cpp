// bytecheck.cpp — 壊れたバイトコードを、実行装置が落ちずに断るか（make test から呼ばれる）
//
// 番号が範囲に収まっているかは、読むときに見る決まり（spec/runtime/bytecode.md）。
// ここでは、書く側では起こらない形に Program を崩してから保存し、
// 読み戻しが「壊れています」と断ることを見る。断らずに進むと、
// 既定値を作るところや仮想の呼び出しで、この実行ファイルごと落ちる
#include <stdio.h>

#include "../core/platform/platform.h"
#include "../core/runtime.h"
#include "../core/shark.h"

using namespace shark;

static int g_fail = 0;

static void ignore_out(void* ud, const char* s, int n) { (void)ud; (void)s; (void)n; }

// クラスと、それを値として持つグローバルがある小さなプログラム
static const char* kSrc =
    "class Fish : Comparable {\n"
    "  public var size: int;\n"
    "  public func init(size: int) { this.size = size; }\n"
    "  override func compare(other: Fish) -> int { return this.size - other.size; }\n"
    "}\n"
    "var pet: Fish = Fish(3);\n"
    "func main() -> int { print(pet.size); return 0; }\n";

// 崩しかた。どれも、書く側からは出てこない形
enum Break {
  B_MethodFunc,   // メソッドが関数を指していない（-1）
  B_SelfField,    // クラスが自分自身を値として持っている
  B_NoClass,      // クラスの型が、クラスを指していない
};

static ClassInfo* find_class(Program* p, const char* name) {
  for (int i = 0; i < p->classes.size(); i++)
    if (p->classes[i]->name == name) return p->classes[i];
  return 0;
}

static void check_rejected(const char* label, Break how) {
  Config cfg;
  cfg.memory_limit = 0;
  Str code;
  bool wrote = false;
  {
    Engine e(cfg);
    HostIO io;
    io.write_out = ignore_out;
    e.set_io(io);
    const Vec<Diagnostic>& ds = e.load(Str("bytecheck"), Str(kSrc));
    for (int i = 0; i < ds.size(); i++)
      if (ds[i].severity == SEV_ERROR) printf("      %s\n", ds[i].message.c_str());
    if (!e.ok()) {
      printf("  fail  %s（もとのプログラムが読み込めなかった）\n", label);
      g_fail++;
      return;
    }
    Program* p = e.program();
    ClassInfo* fish = find_class(p, "Fish");
    Type* ft = p->globals.size() ? p->globals[0]->type : 0;
    if (!fish || !fish->methods.size() || !fish->fields.size() || !ft || ft->kind != T_Class) {
      printf("  fail  %s（崩す先が見つからない）\n", label);
      g_fail++;
      return;
    }
    switch (how) {
      case B_MethodFunc: fish->methods[0].func = -1; break;
      case B_SelfField: fish->fields[0].type = ft; break;
      case B_NoClass: ft->cls = 0; break;
    }
    BytecodeHeader h;
    h.main_file = Str("bytecheck");
    h.memory_mb = 0;
    h.modules = modules_bits(cfg);
    Str err;
    wrote = bytecode_write(*p, e.registry(), h, &code, &err);
    if (!wrote) printf("      %s\n", err.c_str());
  }
  if (!wrote) {
    printf("  fail  %s（崩したものを保存できなかった）\n", label);
    g_fail++;
    return;
  }
  Runtime rt(cfg);
  HostIO io;
  io.write_out = ignore_out;
  rt.set_io(io);
  Str err;
  if (rt.load(code, &err)) {
    printf("  fail  %s（読めてしまった）\n", label);
    g_fail++;
    return;
  }
  printf("  ok    %s（%s）\n", label, err.c_str());
}

int main() {
  printf("壊れたバイトコードを断るか\n");
  check_rejected("メソッドが関数を指していない", B_MethodFunc);
  check_rejected("クラスが自分自身を値として持っている", B_SelfField);
  check_rejected("クラスの型が、クラスを指していない", B_NoClass);
  if (g_fail == 0) printf("\n壊れたバイトコード: すべて断った\n");
  else printf("\n壊れたバイトコードの検査で %d 件失敗\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
