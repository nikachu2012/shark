// imecheck.cpp — 変換つきの文字入力（IME）を、偽の出し先で動かして見張る
//
// macOS の受け皿（NSTextView）とブラウザの受け皿（<textarea>）は、
// **確定した中身と、選んでいるところの数え方が違う**。
//
//   text_state    が返す確定文字列 … 変換中の字を**抜いた**もの
//   text_selection が返す位置      … 変換中の字も**数に入れた**もの
//
// この2つを同じ物差しだと思って使うと、変換中の字が増えるたびにカーソルが
// 1文字ずつずれていく。ここでは同じ数え方をする偽の出し先を作って、
// 変換中の字が**カーソルのところ**に出ることを絵で確かめる。
#include <stdio.h>
#include <string.h>

#include "../core/platform/platform.h"
#include "../core/runtime.h"
#include "../core/shark.h"

using namespace shark;

static int g_fail = 0;
static void ignore_out(void* ud, const char* s, int n) { (void)ud; (void)s; (void)n; }

#include "fake_screen.inc"

// 内蔵の 5×7 は 1 字 6 画素。入力欄は (0,0) に置くので、字は x=5 から始まる
static const int kCellW = 6;
static const int kTextX = 5;     // field_pad_x()
static const int kTextY = 3;     // field_pad_y()
static const int kLineH = 8;

// --- 絵から読み取る -------------------------------------------------------
// 変換中の字には下線が引かれる（差し色）。その左端の画素を探す。
// 枠も差し色なので、字の始まる x から右へ、枠の上下を外して見る
// （下線の行そのものは字の大きさで動くので、決め打ちにしない）
static int underline_x(int from) {
  // 見るのは字の行のあたりだけ。枠の上下の線も差し色なので、そこまで見ると
  // いちばん左の枠を拾ってしまう
  for (int x = from; x < fake::pw; x++)
    for (int y = 1; y <= kTextY + kLineH + 1; y++)
      if (fake::at(x, y) == fake::kAccent) return x;
  return -1;
}

static void expect(const char* label, int got, int want) {
  if (got == want) {
    printf("    ok    %s（%d）\n", label, got);
    return;
  }
  printf("    fail  %s: %d が返った（%d のはず）\n", label, got, want);
  g_fail++;
}

// 台本。1こま描くごとに1つ進める
//   0: "abc" のうしろを押す（3 文字目にカーソルが行く）
//   1: 離す
//   2: 焦点と受け皿が落ち着くのを1こま待つ
//   3〜5: 変換中の字を1つずつ増やす
//   6: 確定
static int g_step = 0;
static int g_seen[8];
static void one_frame() {
  // 描き終わったところ。変換中の下線がどこから始まっているかを控える
  if (g_step >= 3 && g_step <= 6) g_seen[g_step] = underline_x(kTextX);
  switch (g_step) {
    case 0: fake::mouse(kTextX + kCellW * 3, kTextY + 2, 0, true); break;
    case 1: fake::mouse(kTextX + kCellW * 3, kTextY + 2, 0, false); break;
    case 2: break;
    case 3: fake::compose("ん"); break;
    case 4: fake::compose("んご"); break;
    case 5: fake::compose("んごう"); break;
    case 6: fake::commit(); break;
    default: fake::closed(); break;
  }
  g_step++;
}

// 入力欄を1つ出して、くり返し描くだけのプログラム（ui.run がしているのと同じ順）。
// 台本どおりに押して・変換して、変換中の下線がどこから始まるかを見る
static void check(const char* label, const char* widget) {
  printf("  %s\n", label);
  Str src("import std.ui;\n"
          "var memo = \"abcdef\";\n"
          "func main() -> int {\n"
          "  ui.font_builtin();\n"
          "  ui.open(\"t\", 160, 40);\n"
          "  while ui.poll() {\n"
          "    ui.clear();\n"
          "    _ = ui.show(");
  src += widget;
  src += ", 0, 0);\n"
         "    if ui.edited() { ui.clear(); _ = ui.show(";
  src += widget;
  src += ", 0, 0); }\n"
         "    ui.present();\n"
         "  }\n"
         "  return 0;\n"
         "}\n";

  fake::reset();
  Config cfg;
  Engine e(cfg);
  HostIO io;
  io.write_out = ignore_out;
  e.set_io(io);
  const Vec<Diagnostic>& ds = e.load(Str("imecheck"), src);
  for (int i = 0; i < ds.size(); i++)
    if (ds[i].severity == SEV_ERROR) printf("        %s\n", ds[i].message.c_str());
  if (!e.ok()) {
    printf("    fail  読み込めなかった\n");
    g_fail++;
    return;
  }

  g_step = 0;
  for (int i = 0; i < 8; i++) g_seen[i] = -2;
  fake::on_frame = one_frame;   // 台本は1こまごとに進める
  int rounds = 0;
  while (e.step(200000) == SK_Running && rounds < 100) rounds++;
  fake::on_frame = 0;
  int* seen = g_seen;

  // 数え方が食い違っていると、打つたびに 6 画素（1 字）ずつ右へずれていく
  int want = kTextX + kCellW * 3;
  expect("変換の1字目が出るところ", seen[4], want);
  expect("2字目まで伸ばしても同じ", seen[5], want);
  expect("3字目まで伸ばしても同じ", seen[6], want);
}

int main() {
  printf("imecheck\n");

  // 差し替えた移植層は**プログラムが終わるまで生かす**。ここを自動変数にすると、
  // main を抜けたあとの後始末（Vec の解放など）が消えた移植層を触ってしまう
  static Platform p;
  p = *platform_desktop();
  p.screen = &fake::kScreen;
  platform_set(&p);

  // "abcdef" の3文字目のうしろを押してから、そこで変換を始める。
  // 変換中の字は**押したところ**から出るはずで、打つたびに右へずれてはいけない
  check("複数行の入力欄（ui.textarea）", "ui.textarea(ref memo, 2)");
  check("1行の入力欄（ui.field）", "ui.field(ref memo)");

  if (g_fail) {
    printf("imecheck: %d 件おかしい\n", g_fail);
    return 1;
  }
  printf("imecheck: ぜんぶ通った\n");
  return 0;
}
