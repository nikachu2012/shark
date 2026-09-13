// imecheck.cpp — モック画面環境での IME（テキスト入力・コンポジション）挙動検証
//
// macOS のネイティブ入力（NSTextView）および Web ブラウザ（<textarea>）では、
// 確定文字列と選択範囲（カーソル位置）でオフセットの基準が異なる:
//
//   text_state    が返す確定文字列 … 未確定（コンポジション中）文字列を除外した確定テキスト
//   text_selection が返すインデックス … 未確定文字列も含めた全体のテキストインデックス
//
// このオフセット管理方式を正しく考慮しないと、変換候補文字数が増加するたびに
// カーソル描画位置がずれていく。本テストでは同等のオフセット管理を行うモック画面実装を用いて、
// コンポジション文字列および下線が正しいカーソル位置に描画されるかを検証する。
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

// --- 画面バッファの解析 ---------------------------------------------------
// 変換中のテキストにはアクセントカラーの下線が引かれる。その左端 X 座標を走査する。
// 枠線も同色のため、テキスト領域内の Y 座標のみを対象とする。
static int underline_x(int from) {
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

// テストシナリオ。1 フレーム描画ごとに進行する
//   0: "abc" の末尾をクリック（3 文字目にカーソルを移動）
//   1: マウスアップ
//   2: フォーカス状態の安定化を 1 フレーム待機
//   3〜5: コンポジション文字列を 1 文字ずつ追記
//   6: 確定
static int g_step = 0;
static int g_seen[8];
static void one_frame() {
  // 描画完了時の検証: コンポジション下線の開始 X 座標を記録
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

  // インデックス計算の不整合があると、入力ごとに 6 ピクセル（1 文字）ずつ右にずれていく
  int want = kTextX + kCellW * 3;
  expect("変換第1文字目の描画位置", seen[4], want);
  expect("2文字目への伸長時も位置維持", seen[5], want);
  expect("3文字目への伸長時も位置維持", seen[6], want);
}

int main() {
  printf("imecheck\n");

  // 差し替えた移植層は**プログラムが終わるまで生かす**。ここを自動変数にすると、
  // main を抜けたあとの後始末（Vec の解放など）が消えた移植層を触ってしまう
  static Platform p;
  p = *platform_desktop();
  p.screen = &fake::kScreen;
  platform_set(&p);

  // "abcdef" の 3 文字目の後方をクリックし、その位置で変換を開始する。
  // 未確定文字列はクリック位置を起点として描画され、入力文字数が増加しても開始位置がずれないことを検証する。
  check("複数行テキストエリア（ui.textarea）", "ui.textarea(ref memo, 2)");
  check("単行テキストフィールド（ui.field）", "ui.field(ref memo)");

  if (g_fail) {
    printf("imecheck: %d 件失敗\n", g_fail);
    return 1;
  }
  printf("imecheck: 全テスト合格\n");
  return 0;
}
