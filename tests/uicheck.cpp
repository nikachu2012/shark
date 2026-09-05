// uicheck.cpp — 宣言的な層の部品を、偽の出し先で押して・合わせて見張る
//
// 描いた絵は見比べれば済むが（tests/cases/18_ui.shk）、**押されたときにどう動くか**は
// 出来事が要るので .shk では書けない。ここでは偽の出し先から押した・合わせた を
// 流し込んで、ui.show() が返す名札と ui.value() の数を見る。
#include <stdio.h>
#include <string.h>

#include "../core/platform/platform.h"
#include "../core/runtime.h"
#include "../core/shark.h"

using namespace shark;

static int g_fail = 0;

#include "fake_screen.inc"

// Shark の print をそのまま集める。台本の各回で1行ずつ出させて、それを見比べる
static Str g_out;
static void grab_out(void* ud, const char* s, int n) {
  (void)ud;
  for (int i = 0; i < n; i++) g_out.push(s[i]);
}

static void expect_line(const char* label, const Str& got, const char* want) {
  if (got == Str(want)) {
    printf("    ok    %s（%s）\n", label, want);
    return;
  }
  printf("    fail  %s: [%s] が返った（[%s] のはず）\n", label, got.c_str(), want);
  g_fail++;
}
static void expect_true(const char* label, bool ok) {
  if (ok) {
    printf("    ok    %s\n", label);
    return;
  }
  printf("    fail  %s\n", label);
  g_fail++;
}

// 台本を1つ進めるたびに、Shark 側が1行 print する作り。
// 出た行を切り出して返す（何も出ていなければ空）
static Str take_line() {
  int at = -1;
  for (int i = 0; i < g_out.size(); i++)
    if (g_out[i] == '\n') { at = i; break; }
  if (at < 0) return Str();
  Str line = g_out.sub(0, at);
  g_out = g_out.sub(at + 1, g_out.size() - at - 1);
  return line;
}

// くり返し描くだけのプログラムを1つ動かす。台本は step ごとに呼ばれる
struct Case {
  virtual void act(int step) = 0;      // その回に流し込む出来事
  virtual void done(int step, const Str& line) = 0;   // その回に出た行を見る
  virtual int steps() = 0;
  virtual ~Case() {}
};

// 台本は**1こま描くごとに**進める。e.step() は「命令をいくつ動かすか」なので、
// 1回で何こまも進んでしまい、こま数とは揃わない
static Case* g_case = 0;
static int g_step = 0;
// 巻物はなめらかに動くので、押す前に**止まるまで待つ**。
// 台本から settle() を呼ぶと、絵が変わらなくなるまで、こまを空回しする
static int g_settle = 0;
static int g_settle_used = 0;   // 止まるまでに、こまがいくつ要ったか
static void settle() { g_settle = 3; g_settle_used = 0; }

static void one_frame() {
  Str line = take_line();
  if (g_settle > 0) {
    g_settle_used++;
    if (fake::still()) g_settle--;
    else g_settle = 3;      // まだ動いている。数え直す
    if (g_settle > 0) return;
  }
  g_case->done(g_step, line);
  if (g_step < g_case->steps()) g_case->act(g_step);
  else fake::closed();
  g_step++;
}

static void run(const char* label, const char* src, Case* c) {
  printf("  %s\n", label);
  fake::reset();
  g_out.clear();
  Config cfg;
  Engine e(cfg);
  HostIO io;
  io.write_out = grab_out;
  e.set_io(io);
  const Vec<Diagnostic>& ds = e.load(Str("uicheck"), Str(src));
  for (int i = 0; i < ds.size(); i++)
    if (ds[i].severity == SEV_ERROR) printf("        %s\n", ds[i].message.c_str());
  if (!e.ok()) {
    printf("    fail  読み込めなかった\n");
    g_fail++;
    return;
  }
  g_case = c;
  g_step = 0;
  g_settle = 0;
  fake::on_frame = one_frame;
  int rounds = 0;
  while (e.step(200000) == SK_Running && rounds < 500) rounds++;
  fake::on_frame = 0;
  if (g_step <= c->steps()) {   // 途中で止まったら、見張りが素通りしてしまう
    printf("    fail  台本が終わらなかった（%d / %d）\n", g_step, c->steps());
    g_fail++;
  }
}

// 部品を1つ出して、動いた名札と数を1行ずつ出すだけのプログラム
static Str program(const char* widget, const char* state) {
  Str s("import std.ui;\n");
  s += state;
  s += "func main() -> int {\n"
       "  ui.font_builtin();\n"
       "  ui.open(\"t\", 200, 120);\n"
       "  while ui.poll() {\n"
       "    ui.clear(0);\n"
       "    var hit = ui.show(";
  s += widget;
  s += ", 0, 0);\n"
       "    update(hit);\n"
       "    print(f\"{hit} {ui.value()}\");\n"
       "    ui.present();\n"
       "  }\n"
       "  return 0;\n"
       "}\n";
  return s;
}

// 名札の代わりに関数を渡した形。ui.run がしているのと同じように、
// 押された部品が持っていた関数を呼ぶ。**名札が無いので、焦点や一覧の持ち主は
// 置かれた場所から決めている**（core/lib/ui.cpp の widget_key）。そこも見る
static Str program_fn(const char* widget, const char* state) {
  Str s("import std.ui;\n");
  s += state;
  s += "func main() -> int {\n"
       "  ui.font_builtin();\n"
       "  ui.open(\"t\", 200, 120);\n"
       "  while ui.poll() {\n"
       "    ui.clear(0);\n"
       "    _ = ui.show(";
  s += widget;
  s += ", 0, 0);\n"
       "    if ui.has_action() { var act = ui.action(); act(); }\n"
       "    print(shown());\n"
       "    ui.present();\n"
       "  }\n"
       "  return 0;\n"
       "}\n";
  return s;
}

// 内蔵の 5×7 のときの寸法（core/lib/ui.cpp の ui_unit まわり）
static const int kUnit = 8;
static const int kCellW = 6;
static const int kLineH = 8;
static const int kPadY = 3;    // pad_y()
static const int kFPadX = 5;   // field_pad_x()
static const int kFPadY = 3;   // field_pad_y()

// --- 選ぶ（ui.combo）------------------------------------------------------
// 押すと一覧が出て、そこから選ぶと番号が返る
struct ComboCase : Case {
  int steps() { return 4; }
  void act(int step) {
    int box_h = kLineH + kPadY * 2;      // 部品の高さ（一覧はこの下に出る）
    int item_h = kLineH + kPadY * 2;     // 一覧の1つぶんの高さ
    if (step == 0) fake::click(20, box_h / 2);                     // 押す → 一覧が出る
    else if (step == 2) fake::click(20, box_h + item_h + item_h / 2);   // 2つ目を選ぶ
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("押しただけでは選ばれない", line, " 0");
    if (step == 3) expect_line("一覧から2つ目を選ぶと番号が返る", line, "c 1");
  }
};

// --- 一覧（ui.listbox）----------------------------------------------------
struct ListCase : Case {
  int steps() { return 6; }
  void act(int step) {
    if (step == 0) fake::click(8, kFPadY + kLineH + 2);   // 2行目を押す
    else if (step == 2) { fake::key(SKEY_Down, true); fake::key(SKEY_Down, false); }
    else if (step == 4) { fake::key(SKEY_Up, true); fake::key(SKEY_Up, false); }
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("2行目を押すと 1 が返る", line, "l 1");
    if (step == 3) expect_line("下の矢印で次に進む", line, "l 2");
    if (step == 5) expect_line("上の矢印で戻る", line, "l 1");
  }
};

// --- タブ（ui.tabs）-------------------------------------------------------
struct TabsCase : Case {
  int steps() { return 4; }
  void act(int step) {
    if (step == 0) fake::click(2, 4);   // 1つ目のタブ（いまは 1 つ目が開いていない）
    else if (step == 2) fake::click(2, 4);   // もう一度。開いているタブは返らない
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("押したタブの番号が返る", line, "t 0");
    if (step == 3) expect_line("開いているタブを押しても何も返らない", line, " 0");
  }
};

// --- ラジオ（ui.radio）----------------------------------------------------
struct RadioCase : Case {
  int steps() { return 2; }
  void act(int step) {
    if (step == 0) fake::click(4, 4);
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("押すと名札が返る", line, "r 1");
  }
};

// --- 数の入力欄（ui.number）-----------------------------------------------
// − と ＋ で 1 ずつ動き、限りから出ない。打っても限りの外の数は入らない
struct NumberCase : Case {
  int minus_x, plus_x;
  NumberCase() {
    int sw = kUnit * 5 / 4;
    int w = kUnit * 5 + kFPadX * 2 + sw * 2;
    minus_x = w - sw * 2 + sw / 2;
    plus_x = w - sw + sw / 2;
  }
  int steps() { return 10; }
  void act(int step) {
    int y = (kLineH + kFPadY * 2) / 2;
    if (step == 0) fake::click(plus_x, y);         // 8 → 9
    else if (step == 2) fake::click(plus_x, y);    // 9 → 10（上の限り）
    else if (step == 4) fake::click(plus_x, y);    // 10 のまま
    else if (step == 6) fake::click(10, y);        // 字のところを押して打てるようにする
    else if (step == 8) fake::type("9");           // 109 は限りの外なので入らない
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("＋ で 1 増える", line, "n 9");
    if (step == 3) expect_line("上の限りまで増える", line, "n 10");
    if (step == 5) expect_line("上の限りからは出ない", line, " 0");
    if (step == 9) expect_line("限りから出る数は打っても入らない", line, " 0");
  }
};

// --- 名札の無い形（関数を渡す）--------------------------------------------
// 一覧の持ち主を置かれた場所から決めているので、選んだものがちゃんと届くか
struct ComboFnCase : Case {
  int steps() { return 4; }
  void act(int step) {
    int box_h = kLineH + kPadY * 2;
    if (step == 0) fake::click(20, box_h / 2);
    else if (step == 2) fake::click(20, box_h + box_h + box_h / 2);   // 2つ目
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("押しただけでは選ばれない", line, "0");
    if (step == 3) expect_line("選んだものが、渡した関数に届く", line, "1");
  }
};

// 番号を並べた文字の並び（"00", "01", ... ）。字の幅が揃うので、位置を数えやすい
static Str numbered(int n) {
  Str s("[");
  for (int i = 0; i < n; i++) {
    if (i) s += ", ";
    s += "\"";
    s.push((char)('0' + i / 10));
    s.push((char)('0' + i % 10));
    s += "\"";
  }
  s += "]";
  return s;
}

// --- 一覧が面に入りきらないとき（ui.combo）--------------------------------
// 上下の送るしるしに合わせると、少しずつ送れる。送ったぶん、番号もずれる
struct ComboScrollCase : Case {
  // 面は 200×120、1つぶんの高さは 14。20 個は入りきらないので 7 個ずつ見せる
  int item_h, arrow_h, rows, menu_y, body_y;
  int steps_ = 10;
  ComboScrollCase() {
    item_h = kLineH + kPadY * 2;
    arrow_h = item_h / 2;
    rows = (120 - 4 - arrow_h * 2) / item_h;
    menu_y = 120 - (rows * item_h + arrow_h * 2);
    body_y = menu_y + arrow_h;
  }
  int steps() { return steps_; }
  void act(int step) {
    int down_y = menu_y + rows * item_h + arrow_h * 2 - arrow_h / 2;
    if (step == 0) fake::click(20, item_h / 2);        // 押して一覧を出す
    // しるしに合わせるたびに1つ送る（間に外へ出すと、また送れる）
    else if (step == 2 || step == 4 || step == 6) fake::hover(20, down_y);
    else if (step == 3 || step == 5) fake::hover(20, body_y + item_h);
    else if (step == 7) { fake::hover(20, body_y + item_h); settle(); }   // 止まるまで待つ
    else if (step == 8) fake::click(20, body_y + item_h / 2);   // いちばん上を選ぶ
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("一覧はまだ選ばれていない", line, " 0");
    if (step == 9) expect_line("3つ送ってから、いちばん上を選ぶと 3 番", line, "c 3");
  }
};

// --- 押したまま動かして、離して決める（プルダウン）------------------------
struct ComboDragCase : Case {
  int item_h;
  ComboDragCase() : item_h(kLineH + kPadY * 2) {}
  int steps() { return 6; }
  void act(int step) {
    int second = item_h + item_h + item_h / 2;   // 一覧の2つ目のまんなか
    if (step == 0) fake::mouse(20, item_h / 2, 0, true);   // 押す（まだ離さない）
    else if (step == 2) fake::hover(20, second);            // 押したまま動かす
    else if (step == 4) fake::mouse(20, second, 0, false);  // 離す → ここで決まる
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("押しただけでは決まらない", line, " 0");
    if (step == 3) expect_line("動かしただけでも決まらない", line, " 0");
    if (step == 5) expect_line("項目の上で離すと決まる", line, "c 1");
  }
};

// --- 一覧の帯をつまんで動かす（ui.listbox）---------------------------------
struct ListDragCase : Case {
  // 幅は「いちばん長い字（12）＋余白（10）＋帯（3）」= 25。帯は右端
  int bar_x, box_h, bar_bottom;
  ListDragCase() {
    bar_x = 25 - 1 - 2;
    box_h = kLineH + kLineH * 2 + kFPadY * 2;   // 3 行ぶん
    bar_bottom = box_h - 3;
  }
  int steps() { return 6; }
  void act(int step) {
    if (step == 0) fake::mouse(bar_x + 1, bar_bottom, 0, true);    // 帯をつかむ
    else if (step == 2) fake::mouse(bar_x + 1, bar_bottom, 0, false);   // 離す
    else if (step == 4) fake::click(5, kFPadY + 2);                // いちばん上を押す
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("帯を押しただけでは、行は選ばれない", line, " 0");
    // いちばん下まで送ったので、見えているのは最後の3つ（17・18・19）
    if (step == 5) expect_line("下まで送ってから、いちばん上を押すと 17 番", line, "l 17");
  }
};

// --- 車輪（ホイール）で送る -----------------------------------------------
// 一覧は、乗せているだけで送れる。送ったぶん、押した行の番号もずれる
struct ListWheelCase : Case {
  int steps() { return 5; }
  void act(int step) {
    if (step == 0) { fake::hover(5, kFPadY + 2); fake::wheel(5); }   // 5 行ぶん送る
    else if (step == 2) settle();                     // なめらかに動くので、止まるまで待つ
    else if (step == 3) fake::click(5, kFPadY + 2);   // いちばん上を押す
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("送っただけでは選ばれない", line, " 0");
    if (step == 4) expect_line("5 行送ってから、いちばん上を押すと 5 番", line, "l 5");
  }
};

// 一覧が面に入りきらないとき（ui.combo）も、車輪で送れる
struct ComboWheelCase : Case {
  int item_h, arrow_h, rows, menu_y, body_y;
  ComboWheelCase() {
    item_h = kLineH + kPadY * 2;
    arrow_h = item_h / 2;
    rows = (120 - 4 - arrow_h * 2) / item_h;
    menu_y = 120 - (rows * item_h + arrow_h * 2);
    body_y = menu_y + arrow_h;
  }
  int steps() { return 7; }
  void act(int step) {
    if (step == 0) fake::click(20, item_h / 2);                      // 一覧を出す
    else if (step == 2) { fake::hover(20, body_y + item_h); fake::wheel(4); }
    else if (step == 4) settle();                                    // 止まるまで待つ
    else if (step == 5) fake::click(20, body_y + item_h / 2);        // いちばん上を選ぶ
  }
  void done(int step, const Str& line) {
    if (step == 6) expect_line("4 つ送ってから、いちばん上を選ぶと 4 番", line, "c 4");
  }
};

// 複数行の入力欄も、押していなくても車輪で送れる。
// 送ったところは、そのまま（カーソルのところへ戻ってしまわない）。
// 中身は「1〜3 行目は空、4 行目だけ字がある」ので、いちばん上の行を見れば分かる
struct AreaWheelCase : Case {
  int ink0, ink1;
  AreaWheelCase() : ink0(-1), ink1(-1) {}
  int steps() { return 5; }
  void act(int step) {
    if (step == 0) { fake::hover(5, kFPadY + 2); fake::wheel(3); settle(); }
    else if (step == 3) fake::hover(5, kFPadY + 2);   // もう1こま、そのまま
  }
  void done(int step, const Str& line) {
    (void)line;
    if (step == 0) ink0 = row_ink();
    if (step == 1) ink1 = row_ink();   // 止まったあと
    if (step == 4) {
      expect_true("送る前、いちばん上の行は空", ink0 == 0);
      expect_true("3 行送ると、字のある行が上に来る", ink1 > 0);
      expect_true("次のこまでも、送ったところのまま", row_ink() > 0);
    }
  }
  // いちばん上の行に出ている点の数
  int row_ink() {
    int n = 0;
    for (int y = kFPadY; y < kFPadY + kLineH; y++)
      for (int x = kFPadX; x < kFPadX + 20; x++)
        if (fake::at(x, y) > 0x404040) n++;
    return n;
  }
};

// --- ref で受ける形（update() も名札も ui.value() も要らない）--------------
// 動いたら、渡した変数が**直に書き換わる**。処理系が覚えるのは「どの var か」だけ
struct RefToggleCase : Case {
  int steps() { return 6; }
  void act(int step) {
    if (step == 0 || step == 2) fake::click(4, 4);
  }
  void done(int step, const Str& line) {
    if (step == 0) expect_line("はじめは切", line, "false");
    if (step == 1) expect_line("押すと入（変数が直に変わる）", line, "true");
    if (step == 3) expect_line("もう一度押すと切", line, "false");
  }
};

// ラジオは「自分の数」を書き戻す（ui.radio(label, ref sel, 自分の数)）
struct RefRadioCase : Case {
  int steps() { return 4; }
  void act(int step) {
    if (step == 0) fake::click(4, 4);
  }
  void done(int step, const Str& line) {
    if (step == 0) expect_line("はじめは 0", line, "0");
    if (step == 1) expect_line("押すと「自分の数」が入る", line, "2");
  }
};

// 一覧も同じ。押した行の番号がそのまま入る
struct RefListCase : Case {
  int steps() { return 4; }
  void act(int step) {
    if (step == 0) fake::click(5, kFPadY + kLineH + 2);   // 2行目
  }
  void done(int step, const Str& line) {
    if (step == 0) expect_line("はじめは 0", line, "0");
    if (step == 1) expect_line("押した行の番号が入る", line, "1");
  }
};

// --- 続けて押して選ぶ（2回で語、3回で行、4回でぜんぶ）--------------------
// 中身は "ab cd\nef gh"。字は 6 画素で、入力欄の字は x=5 から始まる
struct MultiClickCase : Case {
  int at;   // "cd" の 1 文字目（4 文字目）のあたり
  MultiClickCase() : at(kFPadX + kCellW * 4 + 2) {}
  int steps() { return 12; }
  void act(int step) {
    int y = kFPadY + 2;                       // 1 行目
    if (step == 0) fake::click(at, y);        // 1回目：焦点が来る
    else if (step == 1) fake::click(at, y);   // 2回目：語
    else if (step == 3) fake::click(at, y);   // 3回目：行
    else if (step == 5) fake::click(at, y);   // 4回目：ぜんぶ
    else if (step == 7) { platform().sleep_nanos(500000000LL); fake::click(at, y); }
  }
  void done(int step, const Str& line) {
    if (step == 2) expect_line("2回で語（空白から空白まで）", line, "[cd]");
    if (step == 4) expect_line("3回で行（書かれた改行まで）", line, "[ab cd]");
    if (step == 6) expect_line("4回でぜんぶ", line, "[ab cd/ef gh]");
    if (step == 8) expect_line("間があけば、また1回目から", line, "[]");
  }
};

// --- 右で押したときのメニュー ---------------------------------------------
// キーの書き方を右にうすく出すようにしたので、幅や押す位置がずれていないか
struct FieldMenuCase : Case {
  int item_h;
  FieldMenuCase() : item_h(kLineH + kPadY * 2) {}
  int steps() { return 6; }
  void act(int step) {
    if (step == 0) { fake::mouse(10, 5, 2, true); fake::mouse(10, 5, 2, false); }
    else if (step == 2) fake::click(20, 5 + item_h * 3 + item_h / 2);   // すべて選ぶ
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("右で押すと出る（まだ選んでいない）", line, "[]");
    if (step == 3) expect_line("「すべて選ぶ」でぜんぶ選ばれる", line, "[abcdef]");
  }
};

// --- 押したまま動かして、離して決める（右でも同じ）------------------------
struct FieldMenuDragCase : Case {
  int item_h, target_y;
  FieldMenuDragCase() : item_h(kLineH + kPadY * 2) {
    target_y = 5 + item_h * 3 + item_h / 2;   // 「すべて選ぶ」のまんなか
  }
  int steps() { return 6; }
  void act(int step) {
    if (step == 0) fake::mouse(10, 5, 2, true);        // 右で押す（まだ離さない）
    else if (step == 2) fake::hover(20, target_y);      // 押したまま動かす
    else if (step == 4) fake::mouse(20, target_y, 2, false);   // 離す → ここで決まる
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("押しただけでは決まらない", line, "[]");
    if (step == 3) expect_line("動かしただけでも決まらない", line, "[]");
    if (step == 5) expect_line("項目の上で離すと決まる（プルダウンと同じ）", line, "[abcdef]");
  }
};

// --- 取り消しとやり直し ---------------------------------------------------
// Ctrl-Z（macOS は Cmd-Z）で戻し、Shift を足すとやり直す。
// 受け皿の取り消し帳ではなくコアが持つので、どの機種でも同じに効く
struct UndoCase : Case {
  int steps() { return 12; }
  void act(int step) {
    if (step == 0) fake::click(10, 6);              // 押して打てるようにする
    else if (step == 2) fake::input("abc");         // 打つ
    else if (step == 4) { fake::key(SKEY_Ctrl, true); fake::key('z', true); }
    else if (step == 5) { fake::key('z', false); fake::key(SKEY_Ctrl, false); }
    else if (step == 7) {
      fake::key(SKEY_Ctrl, true);
      fake::key(SKEY_Shift, true);
      fake::key('z', true);
    } else if (step == 8) {
      fake::key('z', false);
      fake::key(SKEY_Shift, false);
      fake::key(SKEY_Ctrl, false);
    }
  }
  void done(int step, const Str& line) {
    if (step == 4) expect_line("打った字が入る", line, "[abc]");
    if (step == 6) expect_line("Ctrl-Z で戻る", line, "[]");
    if (step == 9) expect_line("Shift-Ctrl-Z でやり直す", line, "[abc]");
  }
};

// --- つまみは、外へ出ても付いてくる ---------------------------------------
// 押しっぱなしで動かすものは、部品の外に出たとたん止まると使いにくい。
// つかんだら、離すまで付いてくること
struct SliderDragCase : Case {
  int steps() { return 9; }
  void act(int step) {
    if (step == 0) fake::mouse(10, 5, 0, true);          // つまみをつかむ
    else if (step == 2) fake::hover(80, 5);              // 中で動かす
    else if (step == 4) fake::hover(50, 60);             // **枠の外**へ出して動かす
    else if (step == 6) fake::mouse(50, 60, 0, false);   // 離す
    else if (step == 7) fake::hover(20, 60);             // 離したあとは付いてこない
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("つかんだところの数になる", line, "8");
    if (step == 3) expect_line("中で動かすと付いてくる", line, "85");
    if (step == 5) expect_line("枠の外へ出ても付いてくる", line, "52");
    if (step == 8) expect_line("離したあとは付いてこない", line, "52");
  }
};

// --- 引いて変える欄（ui.drag）---------------------------------------------
// 押したまま横へ引くと数が変わり、動かさずに離すと打ち込みに入る
struct DragNumCase : Case {
  int steps() { return 12; }
  void act(int step) {
    if (step == 0) fake::mouse(20, 6, 0, true);          // つかむ
    else if (step == 2) fake::hover(60, 6);              // 右へ 40 画素引く
    else if (step == 4) fake::mouse(60, 6, 0, false);    // 離す
    else if (step == 6) fake::click(20, 6);              // 動かさずに押して離す
    else if (step == 8) fake::key(SKEY_Back, true);      // 打ち込みに入っている
    else if (step == 9) fake::key(SKEY_Back, false);
  }
  void done(int step, const Str& line) {
    // 限りは 0〜100。200 画素で端から端まで動くので、40 画素で 20 増える
    if (step == 3) expect_line("引くと数が変わる", line, "d 70");
    // 引いた数は残っている（70 の一桁を消せば 7）
    if (step == 9) expect_line("動かさずに離すと、打ち込みに入る", line, "d 7");
  }
};

// --- 小数のつまみ ---------------------------------------------------------
// 値も限りも小数で持ち、動いた値は ui.float_value() で受け取る
struct SliderFloatCase : Case {
  int steps() { return 6; }
  void act(int step) {
    if (step == 0) fake::mouse(50, 5, 0, true);   // つまみをつかむ
    else if (step == 2) fake::mouse(50, 5, 0, false);
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("つかんだところの小数になる", line, "s 0.53");
  }
};

// --- 巻物（ui.scroll）-----------------------------------------------------
// 車輪で送れて、**隠れているところは押せない**
struct ScrollCase : Case {
  int steps() { return 10; }
  void act(int step) {
    if (step == 0) fake::click(10, 100);                 // 巻物の外（何も起きない）
    else if (step == 2) fake::click(10, 6);              // 1つめのボタン
    else if (step == 4) { fake::hover(10, 20); fake::wheel(4); settle(); }
    else if (step == 6) fake::click(10, 6);              // 送ったので、別のボタンが来ている
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("外を押しても何も起きない", line, " 0");
    if (step == 3) expect_line("見えているものは押せる", line, "b0 1");
    if (step == 7) expect_true("送ったあとは、別のものが来ている", line != Str("b0 1"));
  }
};

// --- いくつも選べる一覧 ---------------------------------------------------
// 押すたびに入り切りが変わり、ref で渡した並びが入れ替わる
struct MultiListCase : Case {
  int steps() { return 8; }
  void act(int step) {
    if (step == 0) fake::click(20, kFPadY + 2);                   // 1つめを入れる
    else if (step == 2) fake::click(20, kFPadY + kLineH + 4);     // 2つめも入れる
    else if (step == 4) fake::click(20, kFPadY + 2);              // 1つめを外す
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("押すと選ばれる", line, "[0]");
    if (step == 3) expect_line("もう1つ選べる", line, "[0, 1]");
    if (step == 5) expect_line("もう一度押すと外れる", line, "[1]");
  }
};

// --- 入力欄に入れてよい字（.filter）---------------------------------------
struct FilterCase : Case {
  int steps() { return 8; }
  void act(int step) {
    if (step == 0) fake::click(10, 6);      // 押して打てるようにする
    else if (step == 2) fake::input("1");
    else if (step == 4) fake::input("z");   // 16 進にない字は入らない
    else if (step == 6) fake::input("F");
  }
  void done(int step, const Str& line) {
    if (step == 3) expect_line("通る字は入る", line, "[1]");
    if (step == 5) expect_line("通らない字は入らない", line, "[1]");
    if (step == 7) expect_line("大文字も通る", line, "[1F]");
  }
};

// --- 絵を出す部品（ui.image）----------------------------------------------
// 乗っているところが絵の中のどこかで分かり、名札を渡してあれば押せる
struct ImageCase : Case {
  int steps() { return 6; }
  void act(int step) {
    if (step == 0) fake::hover(10, 6);
    else if (step == 2) fake::click(10, 6);
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("乗っているところが、絵の中のどこかで分かる", line, " 10 6");
    if (step == 3) expect_line("名札があれば押せる", line, "img 10 6");
  }
};

// --- 色を選ぶ（ui.color）--------------------------------------------------
// 見本を押すと板が出て、四角の中を引くと色が変わる。外を押すと閉じる
struct ColorCase : Case {
  int sq, sx, sy;
  ColorCase() {
    sq = kUnit * 12;
    sx = kUnit * 5 / 8;                            // 板の内側の余白（pad_x）
    sy = (kLineH + kFPadY * 2) + 2 + kPadY;        // 見本の下 + pad_y
  }
  int steps() { return 12; }
  void act(int step) {
    if (step == 0) fake::click(5, 6);                       // 見本を押して板を出す
    else if (step == 2) fake::mouse(sx, sy, 0, true);   // 四角の左上（白）
    else if (step == 4) fake::hover(sx + sq - 2, sy + 1);       // 右上（いちばん濃い）
    else if (step == 6) fake::mouse(sx + sq - 2, sy + 1, 0, false);
    else if (step == 8) fake::click(150, 110);              // 外を押して閉じる
  }
  void done(int step, const Str& line) {
    if (step == 3) expect_line("四角の左上は白い", line, "16777215");
    if (step == 5) expect_true("右へ引くと濃くなる", line != Str("16777215"));
    if (step == 9)
      expect_true("外を押すと板が閉じる", fake::at(sx + sq / 2, sy + sq / 2) == 0);
  }
};

// --- 重ね置きの当たり判定（ui.stack）--------------------------------------
// 押しは「カーソルに重なっているうち、いちばん上のもの」が取る。
// 下に敷いたものには届かない（面ぜんぶを覆う幕をかぶせれば、うしろが止まる）
struct ModalCase : Case {
  int steps() { return 4; }
  void act(int step) {
    if (step == 0) fake::click(5, 5);         // 下に敷いたボタン。幕が覆っている
    else if (step == 2) fake::click(192, 6);  // 幕より上に置いたボタン
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("幕の下に敷いたボタンは押されない", line, "[]");
    if (step == 3) expect_line("幕より上のボタンは押せる", line, "[above]");
  }
};

// --- 使えなくする（.disabled）---------------------------------------------
struct DisabledCase : Case {
  int steps() { return 4; }
  void act(int step) {
    if (step == 0) fake::click(5, 5);         // ふつうのボタン
    else if (step == 2) fake::click(5, 25);   // 止めてあるボタン
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("止めていないボタンは押せる", line, "[on]");
    if (step == 3) expect_line("止めたボタンは押されない", line, "[]");
  }
};

// --- キーだけで操る（tab で送って、space / enter で押す）------------------
// マウスの無い機種（ゲーム機）でも操れること。tab は「ぜんぶ置いたあと」に
// 効くので、焦点が移るのは次のこまから
struct KeyNavCase : Case {
  int steps() { return 12; }
  void act(int step) {
    if (step == 0) { fake::key(SKEY_Tab, true); fake::key(SKEY_Tab, false); }
    else if (step == 2) { fake::key(32, true); fake::key(32, false); }
    else if (step == 4) { fake::key(SKEY_Tab, true); fake::key(SKEY_Tab, false); }
    else if (step == 6) { fake::key(32, true); fake::key(32, false); }
    else if (step == 8) {
      fake::key(SKEY_Shift, true);   // 押したまま次のこままで持たせる
      fake::key(SKEY_Tab, true);
      fake::key(SKEY_Tab, false);
    } else if (step == 10) {
      fake::key(SKEY_Enter, true);
      fake::key(SKEY_Enter, false);
      fake::key(SKEY_Shift, false);
    }
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("tab を押しただけでは、まだ押されない", line, "[]");
    if (step == 3) expect_line("space で、1つ目が押される", line, "[one]");
    if (step == 7) expect_line("もう一度 tab を送ると、2つ目が押される", line, "[two]");
    if (step == 11) expect_line("shift+tab で戻り、enter でも押せる", line, "[one]");
  }
};

// --- 折りたためる木（ui.tree）---------------------------------------------
struct TreeCase : Case {
  int steps() { return 8; }
  void act(int step) {
    if (step == 0) fake::click(5, 5);      // 見出しを押して開く
    else if (step == 4) fake::click(5, 5);   // もう一度押して閉じる
  }
  void done(int step, const Str& line) {
    if (step == 1) expect_line("押すと開く", line, "true");
    if (step == 5) expect_line("もう一度押すと閉じる", line, "false");
  }
};

// --- ゆっくり回しても落ちない ---------------------------------------------
// 1行に満たない送りを何度も受け取る（トラックパッドをゆっくり動かしたとき）。
// 端数を持ち越さないと、毎回切り捨てられて**いつまでも動かない**
static const int kSlowSteps = 20;   // 1行の 8/100 ずつ、20 回
struct SlowWheelCase : Case {
  int steps() { return kSlowSteps + 4; }
  void act(int step) {
    if (step == 0) fake::hover(5, kFPadY + 2);
    if (step < kSlowSteps) fake::wheel_fine(8);
    else if (step == kSlowSteps) settle();
    else if (step == kSlowSteps + 1) fake::click(5, kFPadY + 2);   // いちばん上を押す
  }
  void done(int step, const Str& line) {
    // 8/100 行 × 20 回 = 1.6 行ぶん。いちばん上に来ているのは 2 行目（1 番）
    if (step == kSlowSteps + 2)
      expect_line("ゆっくり回したぶんも、ためて動く", line, "l 1");
  }
};

// ui.wheel()（書く人が受け取る口）も、1行たまったところで 1 を返す
struct SlowWheelApiCase : Case {
  int steps() { return kSlowSteps + 3; }
  void act(int step) {
    if (step < kSlowSteps) fake::wheel_fine(8);
  }
  void done(int step, const Str& line) {
    // 8/100 × 20 = 160/100。1 行ぶんたまるので、合計は 1
    if (step == kSlowSteps + 1)
      expect_line("1行に満たない送りも、たまれば ui.wheel() が返す", line, "1");
  }
};

// --- 送りはなめらか（途中が出る）------------------------------------------
// 行ごとに飛ぶのではなく、目当てへ少しずつ寄せる。だから止まるまでに何こまもかかり、
// そのあいだ半端に切れた行が出る
struct SmoothCase : Case {
  int steps() { return 2; }
  void act(int step) {
    if (step == 0) { fake::hover(5, kFPadY + 2); fake::wheel(5); settle(); }
  }
  void done(int step, const Str& line) {
    (void)line;
    if (step == 1)
      expect_true("5 行の送りは、いくつものこまに分かれて動く", g_settle_used >= 8);
  }
};

// --- 入力欄は枠からはみ出さない -------------------------------------------
// 枠より長く打ってから左へ戻すと、カーソルより後ろの字が右へ流れ出ていた。
// 左を隠して見せている以上、**枠の外に描かない**のは入力欄の側の仕事
struct OverflowCase : Case {
  int out_after_type, out_after_left;
  OverflowCase() : out_after_type(-1), out_after_left(-1) {}
  int steps() { return 8; }
  void act(int step) {
    if (step == 0) fake::click(10, 6);                    // 押して打てるようにする
    else if (step == 2) fake::input("abcdefghijklmnopqrstuvwxyz0123456789");
    else if (step == 4) fake::move(-20);                  // 左へ 20 文字ぶん戻す
  }
  // 入力欄の幅は 120（内蔵の 5×7 で ui_unit × 15）。その外に点があってはいけない
  int outside() {
    int n = 0;
    for (int y = 0; y < kLineH + 6; y++)
      for (int x = 120; x < 200; x++)
        if (fake::at(x, y) != 0) n++;
    return n;
  }
  void done(int step, const Str& line) {
    (void)line;
    if (step == 4) out_after_type = outside();
    if (step == 6) out_after_left = outside();
    if (step == 7) {
      expect_true("枠より長く打っても、外にはみ出さない", out_after_type == 0);
      expect_true("そのあと左へ戻しても、外にはみ出さない", out_after_left == 0);
    }
  }
};

// --- 説明（.tooltip）------------------------------------------------------
// カーソルを合わせてすぐには出ず、少し待つと出る
struct TipCase : Case {
  uint32_t before;
  int mx, my;
  TipCase() : before(0), mx(10), my(6) {}
  int steps() { return 4; }
  void act(int step) {
    if (step == 0) fake::hover(mx, my);
    else if (step == 2) platform().sleep_nanos(450000000LL);   // 0.4 秒より長く待つ
  }
  void done(int step, const Str& line) {
    (void)line;
    if (step == 1) before = fake::at(mx + kUnit + 2, my + kUnit);
    if (step == 3)
      expect_true("合わせて少し待つと、カーソルのそばに出る",
                  fake::at(mx + kUnit + 2, my + kUnit) != before);
  }
};

// 台本の要らない見張り。1回動かして、出た行をそのまま見比べる
static void run_once(const char* label, const char* src, const char* want) {
  g_out.clear();
  Config cfg;
  Engine e(cfg);
  HostIO io;
  io.write_out = grab_out;
  e.set_io(io);
  const Vec<Diagnostic>& ds = e.load(Str("uicheck"), Str(src));
  for (int i = 0; i < ds.size(); i++)
    if (ds[i].severity == SEV_ERROR) printf("        %s\n", ds[i].message.c_str());
  if (!e.ok()) {
    printf("    fail  %s: 読み込めなかった\n", label);
    g_fail++;
    return;
  }
  int rounds = 0;
  while (e.step(200000) == SK_Running && rounds < 500) rounds++;
  expect_line(label, take_line(), want);
}

// 丸めない細かさ（ui.pixel_ratio）。Windows の 125%・150% 表示やブラウザの拡大では
// 細かさが半端な数になる。整数に丸めた ui.scale() で面を取ると、面の1画素が
// 画面の1画素に乗らず、機種の側で引き伸ばし直されてにじむ。
// ここは「丸めない数で取れば、面がきっちり画面の画素の数になる」ことを見張る
static void check_pixel_ratio() {
  printf("  丸めない細かさ（ui.pixel_ratio）\n");
  struct { double ratio; const char* want; } t[] = {
      {1.0,  "1 420 300 12"},
      {1.25, "1 525 375 15"},   // 丸めると 1。420 のままでは画面より粗い
      {1.5,  "2 630 450 18"},   // 丸めると 2。840 では画面より細かすぎる
      {2.0,  "2 840 600 24"},
  };
  const char* src =
      "import std.ui;\n"
      "func main() -> int {\n"
      "  var r = ui.pixel_ratio();\n"
      "  ui.open(\"t\", int(420.0 * r + 0.5), int(300.0 * r + 0.5));\n"
      "  print(f\"{ui.scale()} {ui.width()} {ui.height()} {int(12.0 * r + 0.5)}\");\n"
      "  return 0;\n"
      "}\n";
  for (int i = 0; i < (int)(sizeof t / sizeof t[0]); i++) {
    fake::reset();
    fake::ratio = t[i].ratio;
    run_once("面の大きさ（細かさ・面・字）", src, t[i].want);
  }
  fake::reset();
}

int main() {
  printf("uicheck\n");

  // 差し替えた移植層は、プログラムが終わるまで生かす
  static Platform p;
  p = *platform_desktop();
  p.screen = &fake::kScreen;
  platform_set(&p);

  ComboCase combo;
  run("選ぶ（ui.combo）",
      program("ui.combo(\"c\", [\"あか\", \"あお\", \"みどり\"], pick)",
              "var pick = 0;\n"
              "func update(hit: string) -> void { if hit == \"c\" { pick = ui.value(); } }\n")
          .c_str(),
      &combo);

  ListCase list;
  run("一覧（ui.listbox）",
      program("ui.listbox(\"l\", [\"あ\", \"い\", \"う\", \"え\", \"お\"], sel, 3)",
              "var sel = 0;\n"
              "func update(hit: string) -> void { if hit == \"l\" { sel = ui.value(); } }\n")
          .c_str(),
      &list);

  TabsCase tabs;
  run("タブ（ui.tabs）",
      program("ui.tabs(\"t\", [\"あ\", \"い\"], tab)",
              "var tab = 1;\n"
              "func update(hit: string) -> void { if hit == \"t\" { tab = ui.value(); } }\n")
          .c_str(),
      &tabs);

  RadioCase radio;
  run("ラジオ（ui.radio）",
      program("ui.radio(\"あ\", \"r\", on)",
              "var on = false;\n"
              "func update(hit: string) -> void { if hit == \"r\" { on = true; } }\n")
          .c_str(),
      &radio);

  NumberCase number;
  run("数の入力欄（ui.number）",
      program("ui.number(\"n\", num, 0, 10)",
              "var num = 8;\n"
              "func update(hit: string) -> void { if hit == \"n\" { num = ui.value(); } }\n")
          .c_str(),
      &number);

  ComboFnCase combo_fn;
  run("選ぶ（名札の代わりに関数を渡す形）",
      program_fn("ui.combo(func() -> void { pick = ui.value(); }, [\"あか\", \"あお\"], pick)",
                 "var pick = 0;\n"
                 "func shown() -> string { return f\"{pick}\"; }\n")
          .c_str(),
      &combo_fn);

  ComboScrollCase combo_scroll;
  {
    Str w("ui.combo(\"c\", opts, pick)");
    Str st("var opts = ");
    st += numbered(20);
    st += ";\nvar pick = 0;\n"
          "func update(hit: string) -> void { if hit == \"c\" { pick = ui.value(); } }\n";
    run("一覧が面に入りきらないとき（送る）", program(w.c_str(), st.c_str()).c_str(),
        &combo_scroll);
  }

  ComboDragCase combo_drag;
  run("押したまま動かして、離して決める",
      program("ui.combo(\"c\", [\"あか\", \"あお\", \"みどり\"], pick)",
              "var pick = 0;\n"
              "func update(hit: string) -> void { if hit == \"c\" { pick = ui.value(); } }\n")
          .c_str(),
      &combo_drag);

  ListDragCase list_drag;
  {
    Str w("ui.listbox(\"l\", opts, sel, 3)");
    Str st("var opts = ");
    st += numbered(20);
    st += ";\nvar sel = 0;\n"
          "func update(hit: string) -> void { if hit == \"l\" { sel = ui.value(); } }\n";
    run("一覧の帯をつまんで動かす", program(w.c_str(), st.c_str()).c_str(), &list_drag);
  }

  ListWheelCase list_wheel;
  {
    Str w("ui.listbox(\"l\", opts, sel, 3)");
    Str st("var opts = ");
    st += numbered(20);
    st += ";\nvar sel = 0;\n"
          "func update(hit: string) -> void { if hit == \"l\" { sel = ui.value(); } }\n";
    run("車輪で一覧を送る", program(w.c_str(), st.c_str()).c_str(), &list_wheel);
  }

  ComboWheelCase combo_wheel;
  {
    Str w("ui.combo(\"c\", opts, pick)");
    Str st("var opts = ");
    st += numbered(20);
    st += ";\nvar pick = 0;\n"
          "func update(hit: string) -> void { if hit == \"c\" { pick = ui.value(); } }\n";
    run("車輪で、出ている一覧を送る", program(w.c_str(), st.c_str()).c_str(), &combo_wheel);
  }

  AreaWheelCase area_wheel;
  run("車輪で複数行の入力欄を送る",
      program("ui.textarea(\"m\", memo, 3)",
              "var memo = \"\\n\\n\\n########\\n\\n\\n\\n\\n\";\n"
              "func update(hit: string) -> void { if hit == \"m\" { memo = ui.text_value(); } }\n")
          .c_str(),
      &area_wheel);

  RefToggleCase ref_toggle;
  run("ref で受ける（ui.checkbox）",
      program_fn("ui.checkbox(\"あ\", ref on)",
                 "var on = false;\n"
                 "func shown() -> string { return f\"{on}\"; }\n")
          .c_str(),
      &ref_toggle);

  RefRadioCase ref_radio;
  run("ref で受ける（ui.radio）",
      program_fn("ui.radio(\"あ\", ref sel, 2)",
                 "var sel = 0;\n"
                 "func shown() -> string { return f\"{sel}\"; }\n")
          .c_str(),
      &ref_radio);

  RefListCase ref_list;
  run("ref で受ける（ui.listbox）",
      program_fn("ui.listbox(ref sel, [\"あ\", \"い\", \"う\"], 3)",
                 "var sel = 0;\n"
                 "func shown() -> string { return f\"{sel}\"; }\n")
          .c_str(),
      &ref_list);

  FieldMenuCase fmenu;
  run("右で押したときのメニュー（ui.field）",
      program_fn("ui.field(ref name)",
                 "var name = \"abcdef\";\n"
                 "func shown() -> string { return f\"[{ui.selected()}]\"; }\n")
          .c_str(),
      &fmenu);

  FieldMenuDragCase fmenu_drag;
  run("右で押したときのメニュー、引きずって選ぶ（ui.field）",
      program_fn("ui.field(ref name)",
                 "var name = \"abcdef\";\n"
                 "func shown() -> string { return f\"[{ui.selected()}]\"; }\n")
          .c_str(),
      &fmenu_drag);

  UndoCase undo;
  run("取り消しとやり直し（ui.field）",
      program_fn("ui.field(ref name)",
                 "var name = \"\";\n"
                 "func shown() -> string { return f\"[{name}]\"; }\n")
          .c_str(),
      &undo);

  MultiClickCase multi;
  run("続けて押して選ぶ（ui.textarea）",
      program_fn("ui.textarea(ref memo, 3)",
                 "var memo = \"ab cd\\nef gh\";\n"
                 "func shown() -> string {\n"
                 "  var s = ui.selected().replace(\"\\n\", \"/\");\n"
                 "  return f\"[{s}]\";\n"
                 "}\n")
          .c_str(),
      &multi);

  SliderDragCase slide;
  run("つまみは外へ出ても付いてくる（ui.slider）",
      program_fn("ui.slider(ref vol, 0, 100)",
                 "var vol = 0;\n"
                 "func shown() -> string { return f\"{vol}\"; }\n")
          .c_str(),
      &slide);

  SlowWheelCase slow;
  {
    Str w("ui.listbox(\"l\", opts, sel, 3)");
    Str st("var opts = ");
    st += numbered(20);
    st += ";\nvar sel = 0;\n"
          "func update(hit: string) -> void { if hit == \"l\" { sel = ui.value(); } }\n";
    run("ゆっくり回しても落ちない（一覧）", program(w.c_str(), st.c_str()).c_str(), &slow);
  }

  SlowWheelApiCase slow_api;
  run("ゆっくり回しても落ちない（ui.wheel）",
      program_fn("ui.label(\"a\")",
                 "var total = 0;\n"
                 "func shown() -> string { total += ui.wheel(); return f\"{total}\"; }\n")
          .c_str(),
      &slow_api);

  SmoothCase smooth;
  {
    Str w("ui.listbox(\"l\", opts, sel, 3)");
    Str st("var opts = ");
    st += numbered(20);
    st += ";\nvar sel = 0;\n"
          "func update(hit: string) -> void { if hit == \"l\" { sel = ui.value(); } }\n";
    run("送りはなめらか（途中が出る）", program(w.c_str(), st.c_str()).c_str(), &smooth);
  }

  OverflowCase over;
  run("入力欄は枠からはみ出さない",
      program("ui.field(\"f\", name)",
              "var name = \"\";\n"
              "func update(hit: string) -> void { if hit == \"f\" { name = ui.text_value(); } }\n")
          .c_str(),
      &over);

  DragNumCase drag_num;
  run("引いて変える欄（ui.drag）",
      program("ui.drag(\"d\", num, 0, 100)",
              "var num = 50;\n"
              "func update(hit: string) -> void { if hit == \"d\" { num = ui.value(); } }\n")
          .c_str(),
      &drag_num);

  SliderFloatCase slider_f;
  {
    Str src("import std.ui;\n"
            "var vol = 0.0;\n"
            "func main() -> int {\n"
            "  ui.font_builtin();\n"
            "  ui.open(\"t\", 200, 120);\n"
            "  while ui.poll() {\n"
            "    ui.clear(0);\n"
            "    var hit = ui.show(ui.slider(\"s\", vol, 0.0, 1.0), 0, 0);\n"
            "    if hit == \"s\" { vol = ui.float_value(); }\n"
            "    print(f\"{hit} {vol:.2f}\");\n"
            "    ui.present();\n"
            "  }\n"
            "  return 0;\n"
            "}\n");
    run("小数のつまみ", src.c_str(), &slider_f);
  }

  ScrollCase scroll;
  {
    Str src("import std.ui;\n"
            "func view() -> Widget {\n"
            "  var rows: list<Widget> = [];\n"
            "  for var i in range(12) { rows.push(ui.button(f\"b{i}\", f\"b{i}\")); }\n"
            "  return ui.scroll(rows).height(40);\n"
            "}\n"
            "func main() -> int {\n"
            "  ui.font_builtin();\n"
            "  ui.open(\"t\", 200, 120);\n"
            "  while ui.poll() {\n"
            "    ui.clear(0);\n"
            "    var hit = ui.show(view(), 0, 0);\n"
            "    print(f\"{hit} {ui.value()}\");\n"
            "    ui.present();\n"
            "  }\n"
            "  return 0;\n"
            "}\n");
    run("巻物（送る・隠れたところは押せない）", src.c_str(), &scroll);
  }

  MultiListCase multi_list;
  {
    Str src("import std.ui;\n"
            "var picked: list<int> = [];\n"
            "func main() -> int {\n"
            "  ui.font_builtin();\n"
            "  ui.open(\"t\", 200, 120);\n"
            "  while ui.poll() {\n"
            "    ui.clear(0);\n"
            "    _ = ui.show(ui.listbox(ref picked, [\"あ\", \"い\", \"う\"], 3), 0, 0);\n"
            "    print(f\"{picked}\");\n"
            "    ui.present();\n"
            "  }\n"
            "  return 0;\n"
            "}\n");
    run("いくつも選べる一覧", src.c_str(), &multi_list);
  }

  FilterCase filter;
  {
    Str src("import std.ui;\n"
            "var hex = \"\";\n"
            "func main() -> int {\n"
            "  ui.font_builtin();\n"
            "  ui.open(\"t\", 200, 120);\n"
            "  while ui.poll() {\n"
            "    ui.clear(0);\n"
            "    _ = ui.show(ui.field(ref hex).filter(\"hex\"), 0, 0);\n"
            "    print(f\"[{hex}]\");\n"
            "    ui.present();\n"
            "  }\n"
            "  return 0;\n"
            "}\n");
    run("入力欄に入れてよい字（.filter）", src.c_str(), &filter);
  }

  ImageCase image;
  {
    Str src("import std.ui;\n"
            "var paper = ui.canvas(40, 20, ui.rgb(250, 250, 250));\n"
            "func main() -> int {\n"
            "  ui.font_builtin();\n"
            "  ui.open(\"t\", 200, 120);\n"
            "  while ui.poll() {\n"
            "    ui.clear(0);\n"
            "    var hit = ui.show(ui.image(\"img\", paper), 0, 0);\n"
            "    print(f\"{hit} {ui.point_x()} {ui.point_y()}\");\n"
            "    ui.present();\n"
            "  }\n"
            "  return 0;\n"
            "}\n");
    run("絵を出す部品（ui.image）", src.c_str(), &image);
  }

  ColorCase color;
  {
    Str src("import std.ui;\n"
            "var col = ui.rgb(80, 160, 220);\n"
            "func main() -> int {\n"
            "  ui.font_builtin();\n"
            "  ui.open(\"t\", 200, 120);\n"
            "  while ui.poll() {\n"
            "    ui.clear(0);\n"
            "    _ = ui.show(ui.color(ref col), 0, 0);\n"
            "    print(f\"{col}\");\n"
            "    ui.present();\n"
            "  }\n"
            "  return 0;\n"
            "}\n");
    run("色を選ぶ（ui.color）", src.c_str(), &color);
  }

  TreeCase tree;
  {
    Str src("import std.ui;\n"
            "var open = false;\n"
            "func main() -> int {\n"
            "  ui.font_builtin();\n"
            "  ui.open(\"t\", 200, 120);\n"
            "  while ui.poll() {\n"
            "    ui.clear(0);\n"
            "    _ = ui.show(ui.tree(\"え\", ref open, [ui.label(\"なか\")]), 0, 0);\n"
            "    print(f\"{open}\");\n"
            "    ui.present();\n"
            "  }\n"
            "  return 0;\n"
            "}\n");
    run("折りたためる木（ui.tree）", src.c_str(), &tree);
  }

  check_pixel_ratio();

  TipCase tip;
  run("説明（.tooltip）",
      program("ui.button(\"おす\", \"b\").tooltip(\"ここを押すと進みます\")",
              "func update(hit: string) -> void { }\n")
          .c_str(),
      &tip);

  ModalCase modal;
  {
    Str src("import std.ui;\n"
            "func main() -> int {\n"
            "  ui.font_builtin();\n"
            "  ui.open(\"t\", 200, 120);\n"
            "  while ui.poll() {\n"
            "    ui.clear(0);\n"
            "    var hit = ui.show(ui.stack([\n"
            "      ui.button(\"a\", \"below\"),\n"
            "      ui.label(\"\").width(float.infinity()).height(float.infinity())\n"
            "          .background(ui.rgba(0, 0, 0, 150)),\n"
            "      ui.button(\"b\", \"above\").align(\"right\"),\n"
            "    ]), 0, 0);\n"
            "    print(f\"[{hit}]\");\n"
            "    ui.present();\n"
            "  }\n"
            "  return 0;\n"
            "}\n");
    run("重ね置きの当たり判定（ui.stack）", src.c_str(), &modal);
  }

  DisabledCase off;
  {
    Str src("import std.ui;\n"
            "func main() -> int {\n"
            "  ui.font_builtin();\n"
            "  ui.open(\"t\", 200, 120);\n"
            "  while ui.poll() {\n"
            "    ui.clear(0);\n"
            "    var hit = ui.show(ui.col([\n"
            "      ui.button(\"a\", \"on\"),\n"
            "      ui.button(\"b\", \"off\").disabled(true),\n"
            "    ]), 0, 0);\n"
            "    print(f\"[{hit}]\");\n"
            "    ui.present();\n"
            "  }\n"
            "  return 0;\n"
            "}\n");
    run("使えなくする（.disabled）", src.c_str(), &off);
  }

  KeyNavCase nav;
  {
    Str src("import std.ui;\n"
            "func main() -> int {\n"
            "  ui.font_builtin();\n"
            "  ui.open(\"t\", 200, 120);\n"
            "  while ui.poll() {\n"
            "    ui.clear(0);\n"
            "    var hit = ui.show(ui.col([\n"
            "      ui.button(\"a\", \"one\"),\n"
            "      ui.button(\"b\", \"two\"),\n"
            "    ]), 0, 0);\n"
            "    print(f\"[{hit}]\");\n"
            "    ui.present();\n"
            "  }\n"
            "  return 0;\n"
            "}\n");
    run("キーだけで操る（tab / space / enter）", src.c_str(), &nav);
  }

  if (g_fail) {
    printf("uicheck: %d 件おかしい\n", g_fail);
    return 1;
  }
  printf("uicheck: ぜんぶ通った\n");
  return 0;
}
