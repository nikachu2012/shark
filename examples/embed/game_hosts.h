// game_hosts.h — ゲーム側の操作（move / turn / at_goal）と、その登録
//
// バイトコードは処理系の関数を**番号**で指す（spec/runtime/bytecode.md）。
// 番号は登録した順で決まるので、**作る側（build_stage）と動かす側（play_stage）で
// 同じものを同じ順に登録する**必要がある。そこを間違えないように、
// 設定も登録も、このファイル1つにまとめてある。
//
// 食い違ったまま動かそうとしても、読むときに指紋が合わずに止まる。
#ifndef SHARK_EXAMPLE_GAME_HOSTS_H
#define SHARK_EXAMPLE_GAME_HOSTS_H

#include <stdio.h>

#include "../../core/config.h"
#include "../../core/registry.h"
#include "../../core/value.h"
#include "../../core/vm.h"

namespace game {

using namespace shark;

// --- ゲームの中の世界 -----------------------------------------------------
static int g_x = 0, g_y = 0, g_dir = 0;   // 0:北 1:東 2:南 3:西
static const int kGoalX = 2, kGoalY = 2;

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

// --- 作る側と動かす側で、同じにするもの -----------------------------------
static Config game_config() {
  Config cfg;
  cfg.lang = LANG_JA;
  // ゲーム機ではファイルも OS も使わないことにする
  cfg.with_file = false;
  cfg.with_os = false;
  return cfg;
}

// Engine（作る側）と Runtime（動かす側）は同じ形の register_host を持つので、
// どちらにも同じ手で登録できる
template <class T>
void register_game_hosts(T& e) {
  TypeTable& t = e.types();
  e.register_host("move", h_move, t.t_void(), t.t_string());
  e.register_host("turn", h_turn, t.t_void(), t.t_string());
  e.register_host("at_goal", h_at_goal, t.t_bool());
}

}  // namespace game
#endif
