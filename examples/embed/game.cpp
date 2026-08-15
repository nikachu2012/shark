// game.cpp — ゲームに組み込む例（spec/runtime/embedding.md）
//
//   1. 処理系を作る            Engine(config)
//   2. ホストの関数を登録する    register_host(...)
//   3. ソースを読み込む          load(name, source)  → 診断
//   4. 少しずつ動かす            step(budget)        → 状態
//
// ここでは「ロボットを動かす学習ゲーム」のつもりで、
// move / turn / sensor をゲーム側の関数として足している。
// 1フレームに進める命令数を区切るので、プレイヤーが無限ループを書いても固まらない。
#include <stdio.h>

#include "../../core/platform/platform.h"
#include "../../core/shark.h"

using namespace shark;

// --- ゲームの中の世界 -----------------------------------------------------
static int g_x = 0, g_y = 0, g_dir = 0;   // 0:北 1:東 2:南 3:西
static const int kGoalX = 3, kGoalY = 3;   // 今回はたどり着けない場所にしてある

static NativeStatus h_move(VM& vm, Value* args, int n, Value& out) {
  (void)vm; (void)n;
  const Str& dir = ((StrObj*)val_deref(&args[0])->o)->s;
  int d = (dir == "back") ? (g_dir + 2) % 4 : g_dir;
  if (d == 0) g_y++;
  else if (d == 1) g_x++;
  else if (d == 2) g_y--;
  else g_x--;
  printf("      [ゲーム] ロボットが動いた → (%d, %d)\n", g_x, g_y);
  out = mk_void();
  return N_Ok;
}

static NativeStatus h_turn(VM& vm, Value* args, int n, Value& out) {
  (void)vm; (void)n;
  const Str& dir = ((StrObj*)val_deref(&args[0])->o)->s;
  g_dir = (dir == "right") ? (g_dir + 1) % 4 : (g_dir + 3) % 4;
  printf("      [ゲーム] 向きが変わった → %d\n", g_dir);
  out = mk_void();
  return N_Ok;
}

static NativeStatus h_at_goal(VM& vm, Value* args, int n, Value& out) {
  (void)vm; (void)args; (void)n;
  out = mk_bool(g_x == kGoalX && g_y == kGoalY);
  return N_Ok;
}

// print の出力は、ホストが受け取って好きな場所に出す
static void on_output(void* ud, const char* s, int n) {
  (void)ud;
  printf("      [吹き出し] %.*s", n, s);
}

// --- プレイヤーが書いたコード ---------------------------------------------
static const char* kPlayerCode =
    "// ゴールに着くまで進む。わざと無限ループも混ぜてある\n"
    "func main() -> int {\n"
    "  var steps = 0;\n"
    "  while !at_goal() {\n"
    "    move(\"forward\");\n"
    "    steps += 1;\n"
    "    if steps % 2 == 0 { turn(\"right\"); }\n"
    "    if steps > 8 {\n"
    "      print(\"迷ってしまった\\n\");\n"
    "      while true { }          // ← 無限ループ。ゲームは固まらない\n"
    "    }\n"
    "  }\n"
    "  print(\"ゴール！\\n\");\n"
    "  return 0;\n"
    "}\n";

int main() {
  platform_set(platform_desktop());

  Config cfg;
  cfg.lang = LANG_JA;
  // ゲーム機ではファイルも OS も使わないことにする
  cfg.with_file = false;
  cfg.with_os = false;
  Engine engine(cfg);

  TypeTable& t = engine.types();
  engine.register_host("move", h_move, t.t_void(), t.t_string());
  engine.register_host("turn", h_turn, t.t_void(), t.t_string());
  engine.register_host("at_goal", h_at_goal, t.t_bool());

  HostIO io;
  io.write_out = on_output;
  engine.set_io(io);

  const Vec<Diagnostic>& ds = engine.load(Str("ステージ3"), Str(kPlayerCode));
  for (int i = 0; i < ds.size(); i++) {
    Str text = format_diagnostic(ds[i], Str(kPlayerCode), false);
    fputs(text.c_str(), stdout);
  }
  if (!engine.ok()) {
    printf("読み込めなかったので、実行しない\n");
    return 1;
  }

  // ゲームの毎フレーム。1フレームに 60 命令だけ進める
  for (int frame = 1; frame <= 12; frame++) {
    printf("フレーム %2d: ", frame);
    RunStatus st = engine.step(60);
    if (st == SK_Running) {
      printf("まだ動いている（描画に戻れる）\n");
      continue;
    }
    if (st == SK_Finished) {
      printf("終わった（終了コード %d）\n", engine.exit_code());
      return 0;
    }
    printf("止まった: %s\n", engine.error_message().c_str());
    return 1;
  }
  printf("\n12 フレーム進めても終わらなかったので、こちらから止める\n");
  engine.abort_run();
  engine.step(60);
  printf("止めた: %s\n", engine.error_message().c_str());
  return 0;
}
