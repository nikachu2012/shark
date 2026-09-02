// stepcheck.cpp — 止まらないループでも、刻みでホストに返るか（make test から呼ばれる）
//
// コアは「ホストが渡した命令数だけ進めて戻る」決まり
// （spec/runtime/embedding.md、spec/runtime/execution.md）。
// 実行時コンパイルを入れても、機械語の中で回り続けてはいけない。
// ここでは止まらないループを両方（仮想マシンだけ／機械語にして）走らせ、
// どちらも刻みごとに戻ってくることを見る。
#include <stdio.h>

#include "../core/platform/platform.h"
#include "../core/shark.h"

using namespace shark;

static int g_fail = 0;

static void ignore_out(void* ud, const char* s, int n) { (void)ud; (void)s; (void)n; }

// 何度 step() を呼んでも、そのつど戻ってくること
static void check_yields(const char* label, const char* src, bool jit, int threshold) {
  Config cfg;
  cfg.jit = jit;
  cfg.jit_threshold = threshold;
  Engine e(cfg);
  HostIO io;
  io.write_out = ignore_out;
  e.set_io(io);
  e.load(Str("stepcheck"), Str(src));
  if (!e.ok()) {
    printf("  fail  %s（読み込めなかった）\n", label);
    g_fail++;
    return;
  }
  for (int i = 0; i < 2000; i++) {
    if (e.step(1000) == SK_Running) continue;
    printf("  fail  %s（%d 回目で走るのをやめた）\n", label, i);
    g_fail++;
    return;
  }
  printf("  ok    %s\n", label);
}

int main() {
  platform_set(platform_desktop());
  printf("tests/stepcheck.cpp\n");

  // 中で何も呼ばない整数のループ。機械語にすると、戻る道は刻みの見張りだけになる
  const char* spin = "func main() -> int { var i = 0; while true { i += 1; } return 0; }";
  check_yields("止まらないループ（仮想マシン）", spin, false, 0);
  check_yields("止まらないループ（機械語）", spin, true, 1);

  // 呼び出しをまたぐループも同じ
  const char* calls =
      "func step(n: int) -> int { return n + 1; }\n"
      "func main() -> int { var i = 0; while true { i = step(i); } return 0; }";
  check_yields("止まらない呼び出し（仮想マシン）", calls, false, 0);
  check_yields("止まらない呼び出し（機械語）", calls, true, 1);

  if (g_fail == 0) printf("\n刻みでホストに返る: すべて成功\n");
  else printf("\n刻みの検査で %d 件失敗\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
