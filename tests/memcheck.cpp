// memcheck.cpp — メモリの後始末と上限の見張り（make test から呼ばれる）
//
// ・処理系を捨てたあと、確保した量が元に戻るか（参照カウントの取りこぼしが無いか）
// ・上限が見るのは「実行中のプログラムが使う量」だけか
// ・上限を超えたら、落ちずに実行時エラーとして返るか
#include <stdio.h>

#include "../core/platform/platform.h"
#include "../core/runtime.h"
#include "../core/shark.h"

using namespace shark;

static int g_fail = 0;

static void ignore_out(void* ud, const char* s, int n) { (void)ud; (void)s; (void)n; }

// 1つ動かして、処理系を捨てたあとの残りを見る
static void check_clean(const char* label, const char* src) {
  size_t before = sk_mem_used();
  bool loaded = false;
  {
    Config cfg;
    cfg.memory_limit = 0;   // ここでは上限を使わない
    Engine e(cfg);
    HostIO io;
    io.write_out = ignore_out;
    e.set_io(io);
    const Vec<Diagnostic>& ds = e.load(Str("memcheck"), Str(src));
    for (int i = 0; i < ds.size(); i++)
      if (ds[i].severity == SEV_ERROR) printf("      %s\n", ds[i].message.c_str());
    loaded = e.ok();
    if (loaded)
      while (e.step(200000) == SK_Running) {}
  }
  size_t left = sk_mem_used() - before;
  if (!loaded) {
    printf("  fail  %s（読み込めなかった）\n", label);
    g_fail++;
    return;
  }
  if (left != 0) {
    printf("  fail  %s（%lu バイト残った）\n", label, (unsigned long)left);
    g_fail++;
    return;
  }
  printf("  ok    %s\n", label);
}

// バイトコードに保存して、実行装置（Runtime）だけで動かしたあとの残りを見る。
// 読み戻しで作ったもの（型・クラス・関数）も、捨てたときに戻ること
static void check_bytecode_clean(const char* label, const char* src) {
  size_t before = sk_mem_used();
  bool wrote = false, ran = false;
  {
    Str code;
    Config cfg;
    cfg.memory_limit = 0;
    {
      Engine e(cfg);
      HostIO io;
      io.write_out = ignore_out;
      e.set_io(io);
      const Vec<Diagnostic>& ds = e.load(Str("memcheck"), Str(src));
      for (int i = 0; i < ds.size(); i++)
        if (ds[i].severity == SEV_ERROR) printf("      %s\n", ds[i].message.c_str());
      if (e.ok()) {
        BytecodeHeader h;
        h.main_file = Str("memcheck");
        h.memory_mb = 0;
        h.modules = modules_bits(cfg);
        Str err;
        wrote = bytecode_write(*e.program(), e.registry(), h, &code, &err);
        if (!wrote) printf("      %s\n", err.c_str());
      }
    }
    if (wrote) {
      Runtime rt(cfg);
      HostIO io;
      io.write_out = ignore_out;
      rt.set_io(io);
      Str err;
      if (rt.load(code, &err)) {
        while (rt.step(200000) == SK_Running) {}
        ran = true;
      } else {
        printf("      %s\n", err.c_str());
      }
    }
  }
  size_t left = sk_mem_used() - before;
  if (!wrote || !ran) {
    printf("  fail  %s（%s）\n", label, wrote ? "実行装置が読めなかった" : "保存できなかった");
    g_fail++;
    return;
  }
  if (left != 0) {
    printf("  fail  %s（%lu バイト残った）\n", label, (unsigned long)left);
    g_fail++;
    return;
  }
  printf("  ok    %s\n", label);
}

// 読み込みで作るもの（構文木・バイトコード）は上限の対象外
// （上限は、値スタック 1MB が収まる大きさで渡す）
static void check_load_not_counted(size_t limit_mb) {
  Str src("func main() -> int {\n  var n = 0;\n");
  for (int i = 0; i < 20000; i++) src += "  n += 1;\n";
  src += "  return n;\n}\n";

  size_t before = sk_mem_used();
  bool finished = false;
  int code = 0;
  size_t total_peak = 0, run_peak = 0;
  {
    Config cfg;
    cfg.memory_limit = limit_mb << 20;
    Engine e(cfg);
    HostIO io;
    io.write_out = ignore_out;
    e.set_io(io);
    e.load(Str("memcheck"), src);
    if (e.ok()) {
      for (;;) {
        RunStatus st = e.step(200000);
        if (e.memory_total() > total_peak) total_peak = e.memory_total();
        if (e.memory_used() > run_peak) run_peak = e.memory_used();
        if (st == SK_Running) continue;
        finished = (st == SK_Finished);
        code = e.exit_code();
        break;
      }
    }
    sk_mem_set_limit(0);
  }
  size_t left = sk_mem_used() - before;
  // 読み込みだけで上限を大きく超えているのに、実行は止まらないこと
  bool compiled_big = total_peak > (limit_mb << 20);
  bool run_small = run_peak < (limit_mb << 20);
  if (finished && code == 20000 && compiled_big && run_small && left == 0) {
    printf("  ok    読み込みのぶんは数えない\n");
    return;
  }
  printf("  fail  読み込みのぶんは数えない（終わった=%d 値=%d 全体の山=%lu 実行の山=%lu 残り=%lu）\n",
         finished ? 1 : 0, code, (unsigned long)total_peak, (unsigned long)run_peak,
         (unsigned long)left);
  g_fail++;
}

// 上限を超えたときに、実行時エラーで止まるか
static void check_limit(const char* label, const char* src, size_t limit_mb) {
  size_t before = sk_mem_used();
  bool stopped = false;
  size_t peak = 0;
  {
    Config cfg;
    cfg.memory_limit = limit_mb << 20;
    Engine e(cfg);
    HostIO io;
    io.write_out = ignore_out;
    e.set_io(io);
    e.load(Str("memcheck"), Str(src));
    if (e.ok()) {
      for (;;) {
        RunStatus st = e.step(50000);
        if (sk_mem_run_used() > peak) peak = sk_mem_run_used();
        if (st == SK_Running) continue;
        stopped = (st == SK_Error);
        break;
      }
    }
    sk_mem_set_limit(0);
  }
  size_t left = sk_mem_used() - before;
  // 上限そのものは超えるが、離した後は元に戻ること。
  // 上限の 2 倍まで膨らんだら「見張りが効いていない」とみなす
  bool bounded = peak < (limit_mb << 21);
  if (stopped && bounded && left == 0) {
    printf("  ok    %s\n", label);
    return;
  }
  printf("  fail  %s（止まった=%d 山=%lu 残り=%lu）\n", label, stopped ? 1 : 0,
         (unsigned long)peak, (unsigned long)left);
  g_fail++;
}

int main() {
  platform_set(platform_desktop());
  printf("tests/memcheck.cpp\n");

  check_clean("基本の値", "func main() -> int { var s = \"さめ\" + \"だ\"; var n = 1 + 2; return s.len() + n; }");
  check_clean("配列と連想配列",
              "func main() -> int {\n"
              "  var xs = [1, 2, 3];\n"
              "  xs.push(4);\n"
              "  var m: map<string, list<int>> = {};\n"
              "  m[\"a\"] = xs;\n"
              "  m[\"a\"].push(5);\n"
              "  return m[\"a\"].len();\n"
              "}");
  check_clean("クラスとコピー",
              "class Fish : Comparable {\n"
              "  public var name: string;\n"
              "  public var size: int;\n"
              "  func init(n: string, s: int) { this.name = n; this.size = s; }\n"
              "  override func compare(o: Fish) -> int {\n"
              "    if this.size < o.size { return -1; }\n"
              "    if this.size > o.size { return 1; }\n"
              "    return 0;\n"
              "  }\n"
              "}\n"
              "func main() -> int {\n"
              "  var xs = [Fish(\"a\", 3), Fish(\"b\", 1)];\n"
              "  xs.sort();\n"
              "  var f = xs[0];\n"
              "  f.name = \"c\";\n"
              "  return xs[0].size;\n"
              "}");
  check_clean("タスクとチャネル",
              "func work(c: channel<int>) -> void { for var i in range(20) { _ = c.send(i); } c.close(); }\n"
              "func main() -> int {\n"
              "  var c = channel<int>(8);\n"
              "  task work(c);\n"
              "  var sum = 0;\n"
              "  while var v = c.recv() { sum += v; }\n"
              "  var rs = parallel { task twice(1); task twice(2); };\n"
              "  return sum + rs[0] + rs[1];\n"
              "}\n"
              "func twice(n: int) -> int { return n * 2; }");
  check_clean("Result と T?",
              "func may(p: string) -> Result<string> {\n"
              "  if p == \"\" { return Error(\"だめ\"); }\n"
              "  return p + \"!\";\n"
              "}\n"
              "func main() -> int {\n"
              "  var a = may(\"x\") ?? \"\";\n"
              "  if var b = may(\"\") { return b.len(); } else var e { return a.len() + e.message().len(); }\n"
              "}");
  check_clean("ハッシュと乱数",
              "import std.crypto;\n"
              "func main() -> int {\n"
              "  var d = crypto.sha256(\"さめ\");\n"
              "  var h = d.to_hex();\n"
              "  var back = h.from_hex() ?? b\"\";\n"
              "  var m = crypto.hmac_sha256(\"key\", d);\n"
              "  var r = crypto.random_bytes(64);\n"
              "  var u = crypto.uuid4();\n"
              "  if crypto.equal(back, d) { return m.len() + r.len() + u.len(); }\n"
              "  return 0;\n"
              "}");
  // 面（生のメモリ）と、宣言的な層の部品（クラスの実体と配列）
  check_clean("画面と部品",
              "import std.ui;\n"
              "var name = \"abc\";\n"          // ui.field(ref ...) の書き戻し先
              "func act() -> void {}\n"
              "func view(n: int) -> Widget {\n"
              "  return ui.col([ui.label(\"n\"), ui.row([ui.button(\"ok\", act),\n"
              "          ui.checkbox(\"c\", \"c\", true)]), ui.field(\"f\", \"abc\"),\n"
              "          ui.field(ref name), ui.center([ui.divider()]),\n"
              "          ui.grid(2, [ui.label(\"g\"), ui.label(\"h\"), ui.label(\"i\")])]);\n"
              "}\n"
              "func main() -> int {\n"
              "  ui.open(\"memcheck\", 40, 24);\n"
              "  ui.clear(ui.rgb(1, 2, 3));\n"
              "  ui.fill_circle(20, 12, 6, ui.rgb(255, 0, 0));\n"
              "  ui.text(1, 1, \"hi\", ui.rgb(255, 255, 255));\n"
              "  var hit = ui.show(view(1));\n"
              "  var png = ui.to_png();\n"
              "  ui.close();\n"
              "  return hit.len() + png.len() * 0;\n"
              "}");

  // 絵（Canvas）・PNG の読み書き・奥行きの面。持ちものが多いので別に見る
  check_clean("絵と PNG と奥行き",
              "import std.ui;\n"
              "func main() -> int {\n"
              "  ui.open(\"memcheck\", 32, 24);\n"
              "  var e = ui.canvas(16, 16, ui.rgb(1, 2, 3));\n"
              "  var keep = e;\n"                       // 代入のコピー（copy on write）
              "  e.clear(ui.rgba(255, 0, 0, 128));\n"   // ここで写る
              "  e.fill_rect(1, 1, 4, 4, ui.rgb(0, 255, 0));\n"
              "  e.text(0, 0, \"hi\", ui.rgb(255, 255, 255));\n"
              "  e.tri(0, 0, 0, 15, 0, 0, 0, 15, 0, ui.rgb(0, 0, 255));\n"
              "  e.blit(0, 0, 2, [1, 2, 3, 4]);\n"
              "  var small = ui.canvas(2, 2, ui.rgba(0, 0, 0, 0));\n"
              "  e.draw(small, 1, 1);\n"
              "  ui.draw(e, 0, 0, 8, 8);\n"
              "  var png = e.to_png();\n"
              "  var n = 0;\n"
              "  if var back = ui.load_png(png) { n = back.width(); }\n"
              "  ui.depth(true);\n"
              "  ui.clear_depth();\n"
              "  ui.tri(0, 0, 5, 31, 0, 5, 0, 23, 5, ui.rgb(255, 255, 0));\n"
              "  ui.close();\n"                          // 奥行きの面もここで返る
              "  return n + keep.width() * 0 + png.len() * 0;\n"
              "}");

  check_clean("実行時エラーで止まったあと",
              "func main() -> int { var xs = [1]; var s = \"ながい文字列\" + \"ですよ\"; return xs[5] + s.len(); }");

  // 同じ処理系で読み込み直しても、前のぶんが残らないこと
  {
    size_t before = sk_mem_used();
    {
      Config cfg;
      cfg.memory_limit = 0;
      Engine e(cfg);
      HostIO io;
      io.write_out = ignore_out;
      e.set_io(io);
      for (int i = 0; i < 3; i++) {
        e.load(Str("memcheck"),
               Str("var log: list<string> = [];\n"
                   "func main() -> int {\n"
                   "  for var i in range(10) { log.push(\"さめ\"); }\n"
                   "  return log.len();\n"
                   "}"));
        if (!e.ok()) break;
        while (e.step(200000) == SK_Running) {}
      }
    }
    size_t left = sk_mem_used() - before;
    if (left == 0) {
      printf("  ok    読み込み直し\n");
    } else {
      printf("  fail  読み込み直し（%lu バイト残った）\n", (unsigned long)left);
      g_fail++;
    }
  }

  // 保存したバイトコードを、実行装置だけで動かしたとき（spec/runtime/bytecode.md）
  check_bytecode_clean("バイトコード: 基本の値",
                       "func main() -> int { var s = \"さめ\" + \"だ\"; return s.len(); }");
  check_bytecode_clean("バイトコード: クラスと並べ替え",
                       "class Fish : Comparable {\n"
                       "  public var name: string;\n"
                       "  public var size: int;\n"
                       "  func init(n: string, s: int) { this.name = n; this.size = s; }\n"
                       "  override func compare(o: Fish) -> int {\n"
                       "    if this.size < o.size { return -1; }\n"
                       "    if this.size > o.size { return 1; }\n"
                       "    return 0;\n"
                       "  }\n"
                       "}\n"
                       "func main() -> int {\n"
                       "  var xs = [Fish(\"a\", 3), Fish(\"b\", 1)];\n"
                       "  xs.sort();\n"
                       "  return xs[0].size;\n"
                       "}");
  check_bytecode_clean("バイトコード: タスクと Error",
                       "func work(c: channel<int>) -> void { for var i in range(10) { _ = c.send(i); } c.close(); }\n"
                       "func may(p: string) -> Result<string> {\n"
                       "  if p == \"\" { return Error(\"だめ\"); }\n"
                       "  return p + \"!\";\n"
                       "}\n"
                       "func main() -> int {\n"
                       "  var c = channel<int>(4);\n"
                       "  task work(c);\n"
                       "  var sum = 0;\n"
                       "  while var v = c.recv() { sum += v; }\n"
                       "  if var b = may(\"\") { return b.len(); } else var e { return sum + e.message().len(); }\n"
                       "}");

  check_load_not_counted(8);

  check_limit("上限: 配列が増え続ける",
              "func main() -> int {\n"
              "  var xs: list<int> = [];\n"
              "  var i = 0;\n"
              "  while true { xs.push(i); i += 1; }\n"
              "  return 0;\n"
              "}",
              8);
  check_limit("上限: 文字列が倍々に伸びる",
              "func main() -> int {\n"
              "  var s = \"さめ\";\n"
              "  while true { s = s + s; }\n"
              "  return 0;\n"
              "}",
              8);
  check_limit("上限: タスクの中で増え続ける",
              "func hog() -> int {\n"
              "  var xs: list<int> = [];\n"
              "  var i = 0;\n"
              "  while true { xs.push(i); i += 1; }\n"
              "  return 0;\n"
              "}\n"
              "func main() -> int { var t = task hog(); return t.wait(); }",
              8);

  if (g_fail == 0) printf("\nメモリの後始末と上限: すべて成功\n");
  else printf("\nメモリの検査で %d 件失敗\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
